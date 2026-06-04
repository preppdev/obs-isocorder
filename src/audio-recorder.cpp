#include "audio-recorder.hpp"
#include "recorder-api.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <media-io/audio-io.h>
#include <mutex>
#include <ctime>
#include <cstring>

/*
 * Audio-only recording. OBS has no audio-only output (ffmpeg_muxer is
 * OBS_OUTPUT_AV and refuses to start without a video encoder), so we tap an
 * audio_t directly via audio_output_connect and write a 16-bit PCM WAV file
 * ourselves. SOURCE mode taps a private mix of one source; TRACK mode taps a
 * global mix track (1-6).
 */

namespace {

std::mutex g_mutex;
std::vector<struct audio_recorder *> g_recorders;
int g_counter = 0;

bool registered_locked(void *h)
{
	for (auto *r : g_recorders)
		if (r == h)
			return true;
	return false;
}

void default_audio_path(char *out, size_t n)
{
	std::string f = ir::default_folder();
	snprintf(out, n, "%s", f.c_str());
}

void ensure_dir(char *path)
{
	char *slash = strrchr(path, '/');
	if (!slash)
		return;
	*slash = '\0';
	os_mkdirs(path);
	*slash = '/';
}

/* Replace path separators (and ':') so a source name like "Mic/Aux" doesn't
 * create a subfolder in the generated filename. */
void sanitize_name(char *dst, size_t n, const char *src)
{
	size_t j = 0;
	for (size_t i = 0; src && src[i] && j + 1 < n; i++) {
		char c = src[i];
		dst[j++] = (c == '/' || c == '\\' || c == ':') ? '-' : c;
	}
	dst[j] = '\0';
}

/* ---- WAV writing ---- */

void wr_u32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}
void wr_u16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

void write_wav_header(FILE *f, uint32_t sample_rate, uint16_t channels, uint32_t data_bytes)
{
	uint8_t h[44];
	memcpy(h + 0, "RIFF", 4);
	wr_u32(h + 4, 36 + data_bytes);
	memcpy(h + 8, "WAVE", 4);
	memcpy(h + 12, "fmt ", 4);
	wr_u32(h + 16, 16);
	wr_u16(h + 20, 1); /* PCM */
	wr_u16(h + 22, channels);
	wr_u32(h + 24, sample_rate);
	wr_u32(h + 28, sample_rate * channels * 2); /* byte rate */
	wr_u16(h + 32, (uint16_t)(channels * 2));   /* block align */
	wr_u16(h + 34, 16);                         /* bits per sample */
	memcpy(h + 36, "data", 4);
	wr_u32(h + 40, data_bytes);
	fwrite(h, 1, sizeof(h), f);
}

/* Audio consumer — runs on the audio thread. */
void capture_cb(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct audio_recorder *ar = (struct audio_recorder *)param;
	if (!ar->wav || !data || !data->data[0] || !data->frames)
		return;
	const size_t bytes = (size_t)data->frames * ar->channels * 2; /* 16-bit interleaved */
	fwrite(data->data[0], 1, bytes, ar->wav);
	ar->data_bytes += bytes;
	ar->bytes = ar->data_bytes;
}

/* ---- SOURCE-mode private mix (sums the target source's own audio) ---- */

void calc_min_ts(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	uint64_t *min_ts = (uint64_t *)param;
	if (!child || obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (ts && (!*min_ts || ts < *min_ts))
		*min_ts = ts;
}

void mix_audio(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	if (!child || obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts)
		return;
	struct obs_source_audio *mixed = (struct obs_source_audio *)param;
	if (ts < mixed->timestamp)
		return;
	const size_t pos = (size_t)ns_to_audio_frames(mixed->samples_per_sec, ts - mixed->timestamp);
	if (pos >= AUDIO_OUTPUT_FRAMES)
		return;
	const size_t count = AUDIO_OUTPUT_FRAMES - pos;
	struct obs_source_audio_mix ca;
	obs_source_get_audio_mix(child, &ca);
	for (size_t ch = 0; ch < (size_t)mixed->speakers; ch++) {
		float *out = ((float *)mixed->data[ch]) + pos;
		float *in = ca.output[0].data[ch];
		if (!in)
			continue;
		for (size_t i = 0; i < count; i++)
			out[i] += in[i];
	}
}

bool source_mix_cb(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *out_ts, uint32_t mixers,
		   struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(end_ts);
	struct audio_recorder *ar = (struct audio_recorder *)param;
	if (ar->closing || !ar->weak) {
		*out_ts = start_ts;
		return true;
	}
	obs_source_t *src = obs_weak_source_get_source(ar->weak);
	if (!src || obs_source_removed(src)) {
		if (src)
			obs_source_release(src);
		*out_ts = start_ts;
		return true;
	}

	const uint32_t flags = obs_source_get_output_flags(src);
	bool ok = false;

	if ((flags & OBS_SOURCE_COMPOSITE) != 0) {
		uint64_t min_ts = 0;
		obs_source_enum_active_tree(src, calc_min_ts, &min_ts);
		if (min_ts) {
			struct obs_source_audio mixed = {};
			for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++)
				mixed.data[i] = (uint8_t *)mixes->data[i];
			mixed.timestamp = min_ts;
			mixed.speakers = (enum speaker_layout)audio_output_get_channels(ar->priv_audio);
			mixed.samples_per_sec = audio_output_get_sample_rate(ar->priv_audio);
			mixed.format = AUDIO_FORMAT_FLOAT_PLANAR;
			obs_source_enum_active_tree(src, mix_audio, &mixed);
			for (size_t m = 0; m < MAX_AUDIO_MIXES; m++) {
				if ((mixers & (1 << m)) == 0)
					continue;
				for (size_t ch = 0; ch < (size_t)mixed.speakers; ch++) {
					float *d = mixes[m].data[ch];
					for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
						float v = d[i];
						d[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
					}
				}
			}
			*out_ts = min_ts;
			ok = true;
		}
	} else if ((flags & OBS_SOURCE_AUDIO) != 0) {
		const uint64_t sts = obs_source_get_audio_timestamp(src);
		if (obs_source_audio_pending(src)) {
			obs_source_release(src);
			return false;
		}
		if (sts) {
			struct obs_source_audio_mix a;
			obs_source_get_audio_mix(src, &a);
			const size_t ch_n = audio_output_get_channels(ar->priv_audio);
			for (size_t m = 0; m < MAX_AUDIO_MIXES; m++) {
				if ((mixers & (1 << m)) == 0)
					continue;
				for (size_t ch = 0; ch < ch_n; ch++) {
					float *out = mixes[m].data[ch];
					float *in = a.output[0].data[ch];
					if (!in)
						continue;
					for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
						out[i] += in[i];
						if (out[i] > 1.0f)
							out[i] = 1.0f;
						else if (out[i] < -1.0f)
							out[i] = -1.0f;
					}
				}
			}
			*out_ts = sts;
			ok = true;
		}
	}

	obs_source_release(src);
	if (!ok)
		*out_ts = start_ts;
	return true;
}

/* ---- lifecycle ---- */

void stop_capture(struct audio_recorder *ar)
{
	if (ar->cap_audio) {
		audio_output_disconnect(ar->cap_audio, ar->mix_idx, capture_cb, ar);
		ar->cap_audio = NULL;
	}
	if (ar->priv_audio) {
		audio_output_close(ar->priv_audio);
		ar->priv_audio = NULL;
	}
	if (ar->wav) {
		/* Fix up RIFF + data chunk sizes now that we know the length. */
		uint8_t buf[4];
		fseek(ar->wav, 4, SEEK_SET);
		wr_u32(buf, 36 + (uint32_t)ar->data_bytes);
		fwrite(buf, 1, 4, ar->wav);
		fseek(ar->wav, 40, SEEK_SET);
		wr_u32(buf, (uint32_t)ar->data_bytes);
		fwrite(buf, 1, 4, ar->wav);
		fclose(ar->wav);
		ar->wav = NULL;
	}
	ar->output_active = false;
	pthread_mutex_lock(&ar->mutex);
	ar->start_time_ns = 0;
	ar->current_file[0] = '\0';
	pthread_mutex_unlock(&ar->mutex);
	ar->data_bytes = 0;
	ar->bytes = 0;
}

bool start(struct audio_recorder *ar)
{
	char name[160], path[512], ffmt[128], session[128];
	pthread_mutex_lock(&ar->mutex);
	snprintf(name, sizeof(name), "%s", ar->name);
	snprintf(path, sizeof(path), "%s", ar->path);
	snprintf(ffmt, sizeof(ffmt), "%s", ar->filename_format);
	snprintf(session, sizeof(session), "%s", ar->session_subfolder);
	pthread_mutex_unlock(&ar->mutex);

	/* Decide which audio_t to tap. */
	if (ar->global_track > 0) {
		ar->cap_audio = obs_get_audio();
		ar->mix_idx = ar->global_track - 1;
	} else {
		struct audio_output_info oi = {};
		oi.name = name;
		oi.speakers = SPEAKERS_STEREO;
		oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
		oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
		oi.input_param = ar;
		oi.input_callback = source_mix_cb;
		if (audio_output_open(&ar->priv_audio, &oi) != 0)
			return false;
		ar->cap_audio = ar->priv_audio;
		ar->mix_idx = 0;
	}
	if (!ar->cap_audio) {
		stop_capture(ar);
		return false;
	}

	ar->channels = 2;
	ar->sample_rate = audio_output_get_sample_rate(ar->cap_audio);

	/* Build the .wav path (sanitize name so a "/" doesn't make a subfolder). */
	char safe_name[160];
	sanitize_name(safe_name, sizeof(safe_name), name);
	struct dstr fmt = {};
	dstr_printf(&fmt, "%s %s", safe_name, ffmt);
	char *filename = os_generate_formatted_filename("wav", true, fmt.array);
	dstr_free(&fmt);
	struct dstr full = {};
	if (session[0])
		dstr_printf(&full, "%s/%s/%s", path, session, filename);
	else
		dstr_printf(&full, "%s/%s", path, filename);
	bfree(filename);
	ensure_dir(full.array);

	ar->wav = os_fopen(full.array, "wb");
	if (!ar->wav) {
		blog(LOG_WARNING, "[isolated-record] could not open '%s'", full.array);
		dstr_free(&full);
		stop_capture(ar);
		return false;
	}
	write_wav_header(ar->wav, ar->sample_rate, (uint16_t)ar->channels, 0);
	ar->data_bytes = 0;

	char saved[512];
	snprintf(saved, sizeof(saved), "%s", full.array);
	dstr_free(&full);

	struct audio_convert_info conv = {};
	conv.samples_per_sec = ar->sample_rate;
	conv.format = AUDIO_FORMAT_16BIT; /* interleaved 16-bit PCM */
	conv.speakers = SPEAKERS_STEREO;
	if (!audio_output_connect(ar->cap_audio, ar->mix_idx, &conv, capture_cb, ar)) {
		blog(LOG_WARNING, "[isolated-record] audio tap failed for '%s'", name);
		stop_capture(ar);
		return false;
	}

	pthread_mutex_lock(&ar->mutex);
	ar->start_time_ns = os_gettime_ns();
	snprintf(ar->current_file, sizeof(ar->current_file), "%s", saved);
	pthread_mutex_unlock(&ar->mutex);
	ar->output_active = true;
	blog(LOG_INFO, "[isolated-record] audio recording '%s' -> %s", name, saved);
	return true;
}

void reconcile_one(struct audio_recorder *ar)
{
	if (ar->closing)
		return;

	/* Lazy-resolve SOURCE target by name (sources load after the module). */
	if (ar->global_track == 0 && !ar->weak && ar->source_name[0]) {
		obs_source_t *s = obs_get_source_by_name(ar->source_name);
		if (s) {
			ar->weak = obs_source_get_weak_source(s);
			obs_source_release(s);
		}
	}

	const bool want = ar->want_record.load();
	if (want && !ar->output_active) {
		if (ar->global_track == 0 && !ar->weak)
			return; /* source not resolved yet */
		if (!start(ar))
			ar->want_record = false;
	} else if (!want && ar->output_active) {
		stop_capture(ar);
	}
}

void make_id(char *out, size_t n)
{
	snprintf(out, n, "ar-%d", ++g_counter);
}

struct audio_recorder *new_recorder()
{
	auto *ar = (struct audio_recorder *)bzalloc(sizeof(struct audio_recorder));
	pthread_mutex_init(&ar->mutex, NULL);
	make_id(ar->id, sizeof(ar->id));
	snprintf(ar->filename_format, sizeof(ar->filename_format), "%%CCYY-%%MM-%%DD %%hh-%%mm-%%ss");
	default_audio_path(ar->path, sizeof(ar->path));
	return ar;
}

} // namespace

namespace air {

void add_source(obs_source_t *source)
{
	if (!source)
		return;
	const char *sname = obs_source_get_name(source);
	auto *ar = new_recorder();
	ar->global_track = 0;
	ar->weak = obs_source_get_weak_source(source);
	snprintf(ar->source_name, sizeof(ar->source_name), "%s", sname ? sname : "");
	snprintf(ar->name, sizeof(ar->name), "%s (audio)", sname ? sname : "audio");
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_recorders.push_back(ar);
	}
	save();
}

void add_track(int track)
{
	if (track < 1 || track > 6)
		return;
	auto *ar = new_recorder();
	ar->global_track = track;
	snprintf(ar->name, sizeof(ar->name), "Track %d", track);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_recorders.push_back(ar);
	}
	save();
}

void remove(void *handle)
{
	struct audio_recorder *ar = NULL;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (size_t i = 0; i < g_recorders.size(); i++) {
			if (g_recorders[i] == handle) {
				ar = g_recorders[i];
				g_recorders.erase(g_recorders.begin() + i);
				break;
			}
		}
	}
	if (!ar)
		return;
	ar->closing = true;
	stop_capture(ar);
	if (ar->weak)
		obs_weak_source_release(ar->weak);
	pthread_mutex_destroy(&ar->mutex);
	bfree(ar);
	save();
}

void reconcile_all()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (auto *r : g_recorders)
		reconcile_one(r);
}

bool is_registered(void *handle)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return registered_locked(handle);
}

void set_record(void *handle, bool on)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!registered_locked(handle))
		return;
	auto *ar = (struct audio_recorder *)handle;
	if (on) {
		pthread_mutex_lock(&ar->mutex);
		ar->session_subfolder[0] = '\0';
		pthread_mutex_unlock(&ar->mutex);
	}
	ar->want_record = on;
}

void record_all(bool on, const char *session)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (auto *ar : g_recorders) {
		pthread_mutex_lock(&ar->mutex);
		snprintf(ar->session_subfolder, sizeof(ar->session_subfolder), "%s", (on && session) ? session : "");
		pthread_mutex_unlock(&ar->mutex);
		ar->want_record = on;
	}
}

void shutdown_all()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (auto *ar : g_recorders) {
		ar->closing = true;
		stop_capture(ar);
		if (ar->weak)
			obs_weak_source_release(ar->weak);
		pthread_mutex_destroy(&ar->mutex);
		bfree(ar);
	}
	g_recorders.clear();
}

std::vector<Status> snapshot()
{
	std::vector<Status> out;
	std::lock_guard<std::mutex> lock(g_mutex);
	for (auto *ar : g_recorders) {
		Status s = {};
		s.handle = ar;
		s.active = ar->output_active.load();
		s.bytes = ar->bytes.load();
		pthread_mutex_lock(&ar->mutex);
		s.name = ar->name;
		s.file = ar->current_file;
		s.folder = ar->path;
		const uint64_t start_ns = ar->start_time_ns;
		pthread_mutex_unlock(&ar->mutex);
		s.elapsed_sec = (s.active && start_ns) ? (int)((os_gettime_ns() - start_ns) / 1000000000ULL) : 0;
		out.push_back(std::move(s));
	}
	return out;
}

std::string get_path(void *handle)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!registered_locked(handle))
		return "";
	return ((struct audio_recorder *)handle)->path;
}

void set_path(void *handle, const char *path)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!registered_locked(handle) || !path)
			return;
		auto *ar = (struct audio_recorder *)handle;
		pthread_mutex_lock(&ar->mutex);
		snprintf(ar->path, sizeof(ar->path), "%s", path);
		pthread_mutex_unlock(&ar->mutex);
	}
	save();
}

/* ---- persistence ---- */

static char *config_file()
{
	return obs_module_get_config_path(obs_current_module(), "audio-recorders.json");
}

void save()
{
	obs_data_array_t *arr = obs_data_array_create();
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (auto *ar : g_recorders) {
			obs_data_t *d = obs_data_create();
			pthread_mutex_lock(&ar->mutex);
			obs_data_set_string(d, "id", ar->id);
			obs_data_set_int(d, "track", ar->global_track);
			obs_data_set_string(d, "source_name", ar->source_name);
			obs_data_set_string(d, "name", ar->name);
			obs_data_set_string(d, "path", ar->path);
			obs_data_set_string(d, "filename_format", ar->filename_format);
			pthread_mutex_unlock(&ar->mutex);
			obs_data_array_push_back(arr, d);
			obs_data_release(d);
		}
	}
	obs_data_t *root = obs_data_create();
	obs_data_set_array(root, "recorders", arr);
	char *path = config_file();
	if (path) {
		char *dir = obs_module_get_config_path(obs_current_module(), "");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
		obs_data_save_json(root, path);
		bfree(path);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
}

void load()
{
	char *path = config_file();
	if (!path)
		return;
	obs_data_t *root = obs_data_create_from_json_file(path);
	bfree(path);
	if (!root)
		return;
	obs_data_array_t *arr = obs_data_get_array(root, "recorders");
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	std::lock_guard<std::mutex> lock(g_mutex);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *d = obs_data_array_item(arr, i);
		auto *ar = (struct audio_recorder *)bzalloc(sizeof(struct audio_recorder));
		pthread_mutex_init(&ar->mutex, NULL);
		snprintf(ar->id, sizeof(ar->id), "%s", obs_data_get_string(d, "id"));
		ar->global_track = (int)obs_data_get_int(d, "track");
		snprintf(ar->source_name, sizeof(ar->source_name), "%s", obs_data_get_string(d, "source_name"));
		snprintf(ar->name, sizeof(ar->name), "%s", obs_data_get_string(d, "name"));
		snprintf(ar->path, sizeof(ar->path), "%s", obs_data_get_string(d, "path"));
		snprintf(ar->filename_format, sizeof(ar->filename_format), "%s",
			 obs_data_get_string(d, "filename_format"));
		if (!ar->filename_format[0])
			snprintf(ar->filename_format, sizeof(ar->filename_format), "%%CCYY-%%MM-%%DD %%hh-%%mm-%%ss");
		g_recorders.push_back(ar);
		++g_counter;
		obs_data_release(d);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
}

} // namespace air
