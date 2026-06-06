// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 preppdev — Isolated Record (obs-isocorder)

/*
 * Isolated Record - core filter implementation.
 *
 * See isolated-record.hpp for the high-level design. In short:
 *   - The filter is transparent in the video chain (render() just skips it).
 *   - A private obs_view renders the parent source into its own video_t.
 *   - That video_t feeds a dedicated video encoder; a privately opened
 *     audio_t (mixing the parent's audio) feeds a dedicated audio encoder.
 *   - Both feed an ffmpeg_muxer output writing the source's own file.
 *
 * Teardown is fully synchronous (stop_recording) so unloading OBS or removing
 * the filter cannot leave a "stop" signal callback dereferencing freed memory
 * — the class of bug that has historically crashed similar plugins on unload.
 */
#include "isolated-record.hpp"
#include "recorder-api.hpp"

#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/config-file.h>
#include <util/dstr.h>
#include <media-io/audio-io.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Sentinel "audio_source" value selecting a video-only file (no audio track).
 * The empty string selects the source's own audio; anything else is a source
 * name. Kept out of the source namespace by the double underscores. */
#define IR_AUDIO_NONE_TOKEN "__none__"

/* obs_enum_sources callback: add every audio-capable input to the property
 * list (param is the obs_property_t* combo being populated). */
static bool add_audio_source_to_list(void *param, obs_source_t *source)
{
	obs_property_t *list = (obs_property_t *)param;
	if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(source);
		if (name && *name)
			obs_property_list_add_string(list, name, name);
	}
	return true;
}

/* File extension for a recording-format value (mirrors OBS's RecFormat2). */
static const char *format_to_ext(const char *format)
{
	if (strcmp(format, "fragmented_mp4") == 0 || strcmp(format, "hybrid_mp4") == 0)
		return "mp4";
	if (strcmp(format, "fragmented_mov") == 0 || strcmp(format, "hybrid_mov") == 0)
		return "mov";
	if (strcmp(format, "mpegts") == 0)
		return "ts";
	if (strcmp(format, "flv") == 0)
		return "flv";
	if (strcmp(format, "mkv") == 0)
		return "mkv";
	if (strcmp(format, "mp4") == 0)
		return "mp4";
	if (strcmp(format, "mov") == 0)
		return "mov";
	return "mkv";
}

/* OBS output type for a recording format: the "hybrid" formats use the native
 * mp4/mov outputs; everything else uses ffmpeg_muxer (the container is chosen
 * by the file extension, with fragmentation applied via muxer_settings). */
static const char *format_to_output_id(const char *format)
{
	if (strcmp(format, "hybrid_mp4") == 0)
		return "mp4_output";
	if (strcmp(format, "hybrid_mov") == 0)
		return "mov_output";
	return "ffmpeg_muxer";
}

/* Fragmented mp4/mov need these FFmpeg movflags; other formats need none. */
static const char *format_muxer_settings(const char *format)
{
	if (strcmp(format, "fragmented_mp4") == 0 || strcmp(format, "fragmented_mov") == 0)
		return "movflags=frag_keyframe+empty_moov+delay_moov";
	return "";
}

static bool output_exists(const char *id)
{
	const char *out_id;
	size_t i = 0;
	while (obs_enum_output_types(i++, &out_id))
		if (strcmp(out_id, id) == 0)
			return true;
	return false;
}

/* Name encoders/outputs/files after the PARENT source (unique per source) so
 * two filters — which are all named "Isolated Record" — don't collide. */
static const char *rec_name(struct isolated_record *ir)
{
	obs_source_t *parent = obs_filter_get_parent(ir->source);
	const char *n = parent ? obs_source_get_name(parent) : NULL;
	return (n && *n) ? n : obs_source_get_name(ir->source);
}

/* Make a source name safe to embed in a filename — path separators (and ':')
 * would otherwise create unwanted subfolders. */
static void sanitize_name(char *dst, size_t n, const char *src)
{
	size_t j = 0;
	for (size_t i = 0; src && src[i] && j + 1 < n; i++) {
		char c = src[i];
		dst[j++] = (c == '/' || c == '\\' || c == ':') ? '-' : c;
	}
	dst[j] = '\0';
}

/* Map the friendly encoder choice to a concrete OBS encoder id, falling back
 * to the always-available software x264 if the chosen one is missing. */
static const char *resolve_video_encoder(obs_data_t *settings)
{
	const char *enc = obs_data_get_string(settings, "video_encoder");
	if (!enc || !*enc)
		enc = "obs_x264";

	/* Verify availability; otherwise fall back to x264. */
	int i = 0;
	const char *val;
	while (obs_enum_encoder_types(i++, &val)) {
		if (strcmp(val, enc) == 0)
			return enc;
	}
	return "obs_x264";
}

static void ensure_directory(char *path)
{
	char *slash = strrchr(path, '/');
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash && (!slash || backslash > slash))
		slash = backslash;
#endif
	if (!slash)
		return;
	*slash = '\0';
	os_mkdirs(path);
	*slash = '/';
}

/* ------------------------------------------------------------------ */
/* Audio capture                                                       */
/* ------------------------------------------------------------------ */
/*
 * We capture the target source's audio via obs_source_add_audio_capture_callback
 * (a PUSH callback that fires whenever the source outputs audio), buffering it
 * in per-channel deques. The private audio_output's input_callback then PULLS
 * AUDIO_OUTPUT_FRAMES at a time from those deques to feed the encoder.
 *
 * The earlier approach read obs_source_get_audio_mix() inside the input_callback
 * (as obs-source-record does), but that buffer is only populated for sources
 * actively rendered in OBS's main audio mix — so any source not on the live
 * program (and even some that are) recorded pure silence. The push callback is
 * delivered straight from the source's own audio output, independent of the
 * main mix, which is exactly what "record each source in isolation" needs.
 *
 * Capture callbacks are NOT fired by composite sources (scenes/groups); the
 * intended targets are regular inputs (cameras, mics), which do fire them.
 */

/* frames -> nanoseconds at a sample rate (rate falls back to 48k if unset). */
static inline uint64_t ir_frames_to_ns(uint64_t frames, uint32_t rate)
{
	return frames * 1000000000ULL / (rate ? rate : 48000);
}

/* Capture callback: append the source's audio into our per-channel deques and
 * remember the timestamp of the END of the newest sample. Runs on the captured
 * source's audio thread. */
static void ir_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(source);
	struct isolated_record *ir = (struct isolated_record *)param;
	const size_t add = audio_data->frames * sizeof(float);
	if (!add)
		return;

	pthread_mutex_lock(&ir->audio_buf_mutex);
	/* Bound the backlog (~0.5s) so a stalled consumer can't grow it forever.
	 * Dropping the oldest is fine: the drain derives its timestamp from the
	 * newest sample and the current occupancy, so sync stays correct. */
	const size_t cap = (size_t)(ir->audio_sample_rate / 2) * sizeof(float);
	for (size_t ch = 0; ch < ir->audio_channels; ch++) {
		if (cap && ir->audio_buf[ch].size > cap)
			deque_pop_front(&ir->audio_buf[ch], NULL, ir->audio_buf[ch].size - cap);
		if (muted || !audio_data->data[ch])
			deque_push_back_zero(&ir->audio_buf[ch], add);
		else
			deque_push_back(&ir->audio_buf[ch], audio_data->data[ch], add);
	}
	/* Tail = system time the audio arrived. NOTE: audio_data->timestamp here
	 * is the source's RAW device/media clock (OBS passes the un-adjusted value
	 * to capture callbacks), which can differ from OBS's system clock by
	 * seconds — and the video frames are on the system clock. Using
	 * os_gettime_ns() keeps audio and video on the same timeline (the prior
	 * raw-timestamp version drifted A/V by 1-5s). */
	ir->audio_buf_ts = os_gettime_ns();
	pthread_mutex_unlock(&ir->audio_buf_mutex);
}

/* The private audio_output pulls from here. Drains one frame-block from the
 * deques into every requested mix; emits silence on underrun. The block's
 * timestamp is derived as tail_ts - (buffered frames), which self-corrects for
 * any dropped/short reads (no free-running accumulation -> no A/V drift). */
static bool audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts,
				 uint32_t mixers, struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(end_ts_in);
	struct isolated_record *ir = (struct isolated_record *)param;
	const size_t need = (size_t)AUDIO_OUTPUT_FRAMES * sizeof(float);

	pthread_mutex_lock(&ir->audio_buf_mutex);
	const bool have = ir->audio_channels && ir->audio_buf[0].size >= need;
	if (have) {
		const uint64_t buffered_frames = ir->audio_buf[0].size / sizeof(float);
		*out_ts = ir->audio_buf_ts - ir_frames_to_ns(buffered_frames, ir->audio_sample_rate);
		for (size_t ch = 0; ch < ir->audio_channels; ch++) {
			float tmp[AUDIO_OUTPUT_FRAMES];
			deque_pop_front(&ir->audio_buf[ch], tmp, need);
			for (size_t m = 0; m < MAX_AUDIO_MIXES; m++) {
				if ((mixers & (1 << m)) == 0)
					continue;
				if (mixes[m].data[ch])
					memcpy(mixes[m].data[ch], tmp, need);
			}
		}
	}
	pthread_mutex_unlock(&ir->audio_buf_mutex);

	if (!have)
		*out_ts = start_ts_in; /* silence; audio_output leaves mixes cleared */
	return true;
}

/* Resolve the source whose audio should be embedded (strong ref; caller frees).
 * OWN = parent, SOURCE = the override, NONE = NULL. */
static obs_source_t *resolve_audio_target(struct isolated_record *ir)
{
	obs_source_t *target = NULL;
	pthread_mutex_lock(&ir->mutex);
	if (ir->audio_mode == IR_AUDIO_OWN)
		target = obs_source_get_ref(obs_filter_get_parent(ir->source));
	else if (ir->audio_mode == IR_AUDIO_SOURCE && ir->audio_weak)
		target = obs_weak_source_get_source(ir->audio_weak);
	pthread_mutex_unlock(&ir->mutex);
	return target;
}

/* Ensure the capture callback is registered on the currently-desired target,
 * re-registering (and flushing stale audio) when the override changes live.
 * Runs on the graphics thread (from start_recording / reconcile). */
static void update_audio_capture(struct isolated_record *ir)
{
	obs_source_t *want = resolve_audio_target(ir);
	if (want == ir->audio_cb_source) {
		obs_source_release(want); /* no change; drop the extra ref */
		return;
	}

	if (ir->audio_cb_source) {
		obs_source_remove_audio_capture_callback(ir->audio_cb_source, ir_audio_capture, ir);
		obs_source_release(ir->audio_cb_source);
		ir->audio_cb_source = NULL;
	}

	/* Drop audio buffered from the previous target. */
	pthread_mutex_lock(&ir->audio_buf_mutex);
	for (size_t ch = 0; ch < ir->audio_channels; ch++)
		deque_pop_front(&ir->audio_buf[ch], NULL, ir->audio_buf[ch].size);
	ir->audio_buf_ts = 0;
	pthread_mutex_unlock(&ir->audio_buf_mutex);

	if (want) {
		obs_source_add_audio_capture_callback(want, ir_audio_capture, ir);
		ir->audio_cb_source = want; /* transfer ref */
	}
}

/* Tear down audio capture: unregister the callback and free the deques. */
static void stop_audio_capture(struct isolated_record *ir)
{
	if (ir->audio_cb_source) {
		obs_source_remove_audio_capture_callback(ir->audio_cb_source, ir_audio_capture, ir);
		obs_source_release(ir->audio_cb_source);
		ir->audio_cb_source = NULL;
	}
	pthread_mutex_lock(&ir->audio_buf_mutex);
	for (size_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++)
		deque_free(&ir->audio_buf[ch]);
	ir->audio_buf_ts = 0;
	ir->audio_channels = 0;
	pthread_mutex_unlock(&ir->audio_buf_mutex);
}

/* ------------------------------------------------------------------ */
/* Pipeline lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* (Re)build the private obs_view + video_t sized to the parent source.
 * Returns true if a usable video_output exists afterwards. Must run on the
 * graphics thread (called from tick). */
static bool ensure_pipeline(struct isolated_record *ir)
{
	obs_source_t *parent = obs_filter_get_parent(ir->source);
	if (!parent)
		return false;

	uint32_t width = obs_source_get_width(parent);
	uint32_t height = obs_source_get_height(parent);
	width += (width & 1);   /* encoders want even dimensions */
	height += (height & 1);
	if (!width || !height)
		return false;

	if (ir->video_output && ir->width == width && ir->height == height)
		return true; /* already correct */

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	ovi.base_width = width;
	ovi.base_height = height;
	ovi.output_width = width;
	ovi.output_height = height;

	if (!ir->view)
		ir->view = obs_view_create();

	const bool rebuilding = ir->video_output != NULL;
	if (rebuilding)
		obs_view_remove(ir->view); /* drops the old video_t */

	ir->video_output = obs_view_add2(ir->view, &ovi);
	if (!ir->video_output)
		return false;

	ir->width = width;
	ir->height = height;
	if (rebuilding && ir->output_active)
		ir->needs_restart = true;
	return true;
}

/* Translate the OBS-style recording-quality preset into encoder rate-control
 * settings (mirrors Simple-output behavior):
 *   Stream  -> CBR at the configured bitrate ("same as stream")
 *   Small   -> High Quality, Medium File Size      (~CRF/CQP 23)
 *   HQ      -> Indistinguishable Quality, Large     (~CRF/CQP 16)
 *   Lossless-> Lossless, Tremendously Large         (CRF/CQP 0)
 * x264 uses CRF; hardware encoders use CQP. */
static void apply_quality(obs_data_t *s, const char *enc_id, const char *quality, int bitrate)
{
	int q = -1;
	if (strcmp(quality, "Small") == 0)
		q = 23;
	else if (strcmp(quality, "HQ") == 0)
		q = 16;
	else if (strcmp(quality, "Lossless") == 0)
		q = 0;

	if (q < 0) { /* Stream / "Same as stream" => constant bitrate */
		obs_data_set_string(s, "rate_control", "CBR");
		obs_data_set_int(s, "bitrate", bitrate);
		return;
	}

	if (strstr(enc_id, "x264")) {
		obs_data_set_string(s, "rate_control", "CRF");
		obs_data_set_int(s, "crf", q);
	} else {
		/* NVENC/QSV/VideoToolbox and friends: constant quantizer. */
		obs_data_set_string(s, "rate_control", "CQP");
		obs_data_set_int(s, "cqp", q);
		obs_data_set_int(s, "qp", q);
	}
}

static void build_video_encoder(struct isolated_record *ir, obs_data_t *settings)
{
	const char *enc_id = resolve_video_encoder(settings);
	const char *quality = obs_data_get_string(settings, "rec_quality");
	if (!quality || !*quality)
		quality = "Stream";
	int bitrate = (int)obs_data_get_int(settings, "video_bitrate");
	if (bitrate <= 0)
		bitrate = 2500;

	obs_data_t *es = obs_data_create();
	apply_quality(es, enc_id, quality, bitrate);

	if (!ir->video_encoder || strcmp(obs_encoder_get_id(ir->video_encoder), enc_id) != 0) {
		obs_encoder_release(ir->video_encoder);
		ir->video_encoder = obs_video_encoder_create(enc_id, rec_name(ir), es, NULL);
	} else if (!obs_encoder_active(ir->video_encoder)) {
		obs_encoder_update(ir->video_encoder, es);
	}
	obs_data_release(es);
	obs_encoder_set_video(ir->video_encoder, ir->video_output);

	if (obs_data_get_bool(settings, "scale")) {
		uint32_t w = (uint32_t)obs_data_get_int(settings, "scale_width");
		uint32_t h = (uint32_t)obs_data_get_int(settings, "scale_height");
		obs_encoder_set_scaled_size(ir->video_encoder, (w && h) ? w : 0, (w && h) ? h : 0);
	} else {
		obs_encoder_set_scaled_size(ir->video_encoder, 0, 0);
	}
}

static void build_audio_encoder(struct isolated_record *ir, obs_data_t *settings)
{
	if (!ir->audio_output) {
		/* Match OBS's channel layout + sample rate so the channel count lines
		 * up with what the capture callback delivers. */
		const struct audio_output_info *aoi = audio_output_get_info(obs_get_audio());
		struct audio_output_info oi = {};
		oi.name = rec_name(ir);
		oi.speakers = aoi ? aoi->speakers : SPEAKERS_STEREO;
		oi.samples_per_sec = aoi ? aoi->samples_per_sec : 48000;
		oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
		oi.input_param = ir;
		oi.input_callback = audio_input_callback;
		audio_output_open(&ir->audio_output, &oi);

		pthread_mutex_lock(&ir->audio_buf_mutex);
		ir->audio_channels = audio_output_get_channels(ir->audio_output);
		ir->audio_sample_rate = audio_output_get_sample_rate(ir->audio_output);
		ir->audio_buf_ts = 0;
		pthread_mutex_unlock(&ir->audio_buf_mutex);
	}

	if (!ir->audio_encoder) {
		const char *aenc = obs_data_get_string(settings, "audio_encoder");
		if (!aenc || !*aenc)
			aenc = "ffmpeg_aac";
		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", obs_data_get_int(settings, "audio_bitrate"));
		ir->audio_encoder = obs_audio_encoder_create(aenc, rec_name(ir), as, 0, NULL);
		obs_data_release(as);
	}
	obs_encoder_set_audio(ir->audio_encoder, ir->audio_output);
}

/* Build the file path and the ffmpeg output, attach encoders, and start.
 * Returns true if obs_output_start succeeded. Runs on the graphics thread. */
static bool start_recording(struct isolated_record *ir, obs_data_t *settings)
{
	if (!ir->video_output)
		return false;

	build_video_encoder(ir, settings);

	pthread_mutex_lock(&ir->mutex);
	const bool want_audio = (ir->audio_mode != IR_AUDIO_NONE);
	pthread_mutex_unlock(&ir->mutex);
	if (want_audio) {
		build_audio_encoder(ir, settings);
		update_audio_capture(ir); /* register the capture cb on the target */
	}

	const char *format = obs_data_get_string(settings, "rec_format");
	if (!format || !*format)
		format = "mkv";

	/* Prefix the source name so concurrent recordings never resolve to the
	 * same file (identical timestamp-to-the-second would otherwise collide). */
	char safe_name[256];
	sanitize_name(safe_name, sizeof(safe_name), rec_name(ir));
	struct dstr fmt = {};
	dstr_printf(&fmt, "%s %s", safe_name, obs_data_get_string(settings, "filename_formatting"));
	char *filename = os_generate_formatted_filename(format_to_ext(format), true, fmt.array);
	dstr_free(&fmt);

	/* Optional per-session subfolder set by a grouped "Record All". */
	char session[128];
	pthread_mutex_lock(&ir->mutex);
	snprintf(session, sizeof(session), "%s", ir->session_subfolder);
	pthread_mutex_unlock(&ir->mutex);

	struct dstr path = {};
	if (session[0])
		dstr_printf(&path, "%s/%s/%s", obs_data_get_string(settings, "path"), session, filename);
	else
		dstr_printf(&path, "%s/%s", obs_data_get_string(settings, "path"), filename);
	bfree(filename);
	ensure_directory(path.array);

	char saved_path[512];
	snprintf(saved_path, sizeof(saved_path), "%s", path.array);

	const char *output_id = format_to_output_id(format);
	obs_data_t *os = obs_data_create();
	obs_data_set_string(os, "path", path.array);
	/* ffmpeg_muxer reads "muxer_settings"; fragmented formats set movflags. */
	const char *muxer = format_muxer_settings(format);
	if (*muxer)
		obs_data_set_string(os, "muxer_settings", muxer);
	dstr_free(&path);

	if (!ir->file_output || strcmp(obs_output_get_id(ir->file_output), output_id) != 0) {
		obs_output_release(ir->file_output);
		ir->file_output = obs_output_create(output_id, rec_name(ir), os, NULL);
	} else {
		obs_output_update(ir->file_output, os);
	}
	obs_data_release(os);

	obs_output_set_video_encoder(ir->file_output, ir->video_encoder);
	/* NONE => video-only file. Pass NULL to clear any stale encoder on a
	 * reused output that previously recorded with audio. */
	obs_output_set_audio_encoder(ir->file_output, want_audio ? ir->audio_encoder : NULL, 0);

	/* Feed the parent source into the private view, and force it to render
	 * even when it isn't visible in the active scene. */
	obs_source_t *parent = obs_filter_get_parent(ir->source);
	obs_source_t *cur = obs_view_get_source(ir->view, IR_SOURCE_CHANNEL);
	if (cur != parent)
		obs_view_set_source(ir->view, IR_SOURCE_CHANNEL, parent);
	obs_source_release(cur);

	if (!ir->showing && parent) {
		obs_source_inc_showing(parent);
		ir->showing = true;
	}

	if (!obs_output_start(ir->file_output)) {
		blog(LOG_WARNING, "[isolated-record] failed to start output for '%s': %s",
		     obs_source_get_name(parent), obs_output_get_last_error(ir->file_output));
		/* Release the dead output and showing ref so we start clean next time. */
		obs_output_release(ir->file_output);
		ir->file_output = NULL;
		if (ir->showing && parent) {
			obs_source_dec_showing(parent);
			ir->showing = false;
		}
		return false;
	}

	/* Record live status for the dock (under the lock the dock reads with). */
	pthread_mutex_lock(&ir->mutex);
	ir->start_time_ns = os_gettime_ns();
	snprintf(ir->current_file, sizeof(ir->current_file), "%s", saved_path);
	pthread_mutex_unlock(&ir->mutex);
	ir->bytes = 0;

	ir->output_active = true;
	blog(LOG_INFO, "[isolated-record] recording '%s'", obs_source_get_name(parent));
	return true;
}

/* Release recording resources. Assumes the output is ALREADY inactive (either
 * it finished a graceful stop, failed, or was force-stopped during teardown).
 * Does not call obs_output_stop/force_stop itself, so it never blocks the
 * render thread. Preserves the view unless full_teardown. */
static void release_resources(struct isolated_record *ir, bool full_teardown)
{
	if (ir->showing) {
		obs_source_t *parent = obs_filter_get_parent(ir->source);
		if (parent)
			obs_source_dec_showing(parent);
		ir->showing = false;
	}

	if (ir->file_output) {
		obs_output_release(ir->file_output);
		ir->file_output = NULL;
	}
	obs_encoder_release(ir->video_encoder);
	ir->video_encoder = NULL;
	obs_encoder_release(ir->audio_encoder);
	ir->audio_encoder = NULL;
	if (ir->audio_output) {
		audio_output_close(ir->audio_output);
		ir->audio_output = NULL;
	}
	/* After the audio_output (the puller) is closed, drop the capture callback
	 * (the pusher) and free the deques — in that order so neither thread can
	 * touch the buffers after they're gone. */
	stop_audio_capture(ir);

	ir->output_active = false;
	pthread_mutex_lock(&ir->mutex);
	ir->start_time_ns = 0;
	ir->current_file[0] = '\0';
	pthread_mutex_unlock(&ir->mutex);
	ir->bytes = 0;

	if (full_teardown && ir->view) {
		obs_view_set_source(ir->view, IR_SOURCE_CHANNEL, NULL);
		if (ir->video_output) {
			obs_view_remove(ir->view);
			ir->video_output = NULL;
		}
		obs_view_destroy(ir->view);
		ir->view = NULL;
	}
}

/* Request a graceful async stop. Used from the render-thread reconcile; the
 * muxer finalizes the file on its own thread and reconcile releases the
 * resources on a later frame once obs_output_active() reports false. This must
 * NOT block: force-stopping here would deadlock (force_stop waits for the
 * encoder, which waits for a frame from the render thread we're on). */
static void request_stop(struct isolated_record *ir)
{
	if (ir->file_output && obs_output_active(ir->file_output))
		obs_output_stop(ir->file_output);
}

/* Synchronous stop + release, for teardown only (destroy / filter removal),
 * which runs off the render thread so force_stop can safely block. */
static void stop_recording(struct isolated_record *ir, bool full_teardown)
{
	if (ir->file_output && obs_output_active(ir->file_output)) {
		obs_output_stop(ir->file_output);
		if (obs_output_active(ir->file_output))
			obs_output_force_stop(ir->file_output);
	}
	release_resources(ir, full_teardown);
}

/* ------------------------------------------------------------------ */
/* Hotkey                                                              */
/* ------------------------------------------------------------------ */

static void record_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct isolated_record *ir = (struct isolated_record *)data;
	ir->want_record = !ir->want_record.load();
}

/* ------------------------------------------------------------------ */
/* Properties UI                                                       */
/* ------------------------------------------------------------------ */

static bool record_button_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct isolated_record *ir = (struct isolated_record *)data;
	ir->want_record = !ir->want_record.load();
	return false;
}

static obs_properties_t *ir_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *mode = obs_properties_add_list(props, "record_mode", obs_module_text("RecordMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Mode.WhenActive"), IR_MODE_WHEN_ACTIVE);
	obs_property_list_add_int(mode, obs_module_text("Mode.Manual"), IR_MODE_MANUAL);

	obs_properties_add_button(props, "record_button", obs_module_text("ToggleRecording"), record_button_clicked);

	obs_properties_add_path(props, "path", obs_module_text("Path"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(props, "filename_formatting", obs_module_text("FilenameFormatting"), OBS_TEXT_DEFAULT);

	/* Mirror OBS's built-in recording-format list (RecFormat2 values). */
	obs_property_t *fmt = obs_properties_add_list(props, "rec_format", obs_module_text("Format"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(fmt, "Flash Video (.flv)", "flv");
	obs_property_list_add_string(fmt, "Matroska Video (.mkv)", "mkv");
	obs_property_list_add_string(fmt, "MPEG-4 (.mp4)", "mp4");
	obs_property_list_add_string(fmt, "QuickTime (.mov)", "mov");
	if (output_exists("mp4_output"))
		obs_property_list_add_string(fmt, "Hybrid MP4 (.mp4)", "hybrid_mp4");
	if (output_exists("mov_output"))
		obs_property_list_add_string(fmt, "Hybrid MOV (.mov)", "hybrid_mov");
	obs_property_list_add_string(fmt, "Fragmented MP4 (.mp4)", "fragmented_mp4");
	obs_property_list_add_string(fmt, "Fragmented MOV (.mov)", "fragmented_mov");
	obs_property_list_add_string(fmt, "MPEG-TS (.ts)", "mpegts");

	/* Recording quality, mirroring OBS's Simple-output presets. */
	obs_property_t *q = obs_properties_add_list(props, "rec_quality", obs_module_text("Quality"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(q, obs_module_text("Quality.Stream"), "Stream");
	obs_property_list_add_string(q, obs_module_text("Quality.Small"), "Small");
	obs_property_list_add_string(q, obs_module_text("Quality.HQ"), "HQ");
	obs_property_list_add_string(q, obs_module_text("Quality.Lossless"), "Lossless");

	obs_property_t *venc = obs_properties_add_list(props, "video_encoder", obs_module_text("VideoEncoder"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	const char *enc_id;
	const char *enc_name;
	size_t i = 0;
	while (obs_enum_encoder_types(i++, &enc_id)) {
		if (obs_get_encoder_type(enc_id) != OBS_ENCODER_VIDEO)
			continue;
		if (obs_get_encoder_caps(enc_id) & OBS_ENCODER_CAP_DEPRECATED)
			continue;
		enc_name = obs_encoder_get_display_name(enc_id);
		obs_property_list_add_string(venc, enc_name ? enc_name : enc_id, enc_id);
	}

	obs_properties_add_int(props, "video_bitrate", obs_module_text("VideoBitrate"), 200, 100000, 50);

	obs_property_t *scale = obs_properties_add_bool(props, "scale", obs_module_text("Rescale"));
	UNUSED_PARAMETER(scale);
	obs_properties_add_int(props, "scale_width", obs_module_text("Width"), 0, 16384, 2);
	obs_properties_add_int(props, "scale_height", obs_module_text("Height"), 0, 16384, 2);

	/* Which audio is embedded in this source's file. Default ("") is the
	 * source's own audio; "__none__" makes a video-only file; or pick another
	 * audio source to override (e.g. a clean mic feed). */
	obs_property_t *asrc = obs_properties_add_list(props, "audio_source", obs_module_text("AudioSource"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(asrc, obs_module_text("AudioSource.Own"), "");
	obs_property_list_add_string(asrc, obs_module_text("AudioSource.None"), IR_AUDIO_NONE_TOKEN);
	obs_enum_sources(add_audio_source_to_list, asrc);

	obs_property_t *aenc = obs_properties_add_list(props, "audio_encoder", obs_module_text("AudioEncoder"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	i = 0;
	while (obs_enum_encoder_types(i++, &enc_id)) {
		if (obs_get_encoder_type(enc_id) != OBS_ENCODER_AUDIO)
			continue;
		enc_name = obs_encoder_get_display_name(enc_id);
		obs_property_list_add_string(aenc, enc_name ? enc_name : enc_id, enc_id);
	}
	obs_properties_add_int(props, "audio_bitrate", obs_module_text("AudioBitrate"), 32, 1024, 32);

	return props;
}

static void ir_get_defaults(obs_data_t *settings)
{
	/* Default to Manual so the dock's Record/Stop buttons are authoritative and
	 * adding a source simply lists it idle. "When active" is opt-in per source. */
	obs_data_set_default_int(settings, "record_mode", IR_MODE_MANUAL);
	obs_data_set_default_string(settings, "filename_formatting", "%CCYY-%MM-%DD %hh-%mm-%ss");
	/* New sources inherit the global default format (platform-specific). */
	obs_data_set_default_string(settings, "rec_format", ir::default_format().c_str());
	obs_data_set_default_string(settings, "rec_quality", ir::default_quality().c_str());
	obs_data_set_default_string(settings, "video_encoder", "obs_x264");
	obs_data_set_default_int(settings, "video_bitrate", 2500);
	obs_data_set_default_string(settings, "audio_encoder", "ffmpeg_aac");
	obs_data_set_default_int(settings, "audio_bitrate", 160);
	/* Default audio = the source's own audio (empty selects IR_AUDIO_OWN). */
	obs_data_set_default_string(settings, "audio_source", "");

	/* New sources inherit the global default destination folder. */
	std::string folder = ir::default_folder();
	if (!folder.empty())
		obs_data_set_default_string(settings, "path", folder.c_str());
}

/* ------------------------------------------------------------------ */
/* obs_source_info callbacks                                           */
/* ------------------------------------------------------------------ */

static const char *ir_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("IsolatedRecord");
}

/* Parse the "audio_source" setting into mode + (re)resolved weak ref. Resolves
 * the named source OUTSIDE ir->mutex (obs_get_source_by_name takes the global
 * source lock) to avoid any lock-order coupling, then swaps under the lock. */
static void update_audio_override(struct isolated_record *ir, obs_data_t *settings)
{
	const char *sel = obs_data_get_string(settings, "audio_source");

	enum ir_audio_mode mode;
	char name[256];
	name[0] = '\0';
	if (!sel || !*sel) {
		mode = IR_AUDIO_OWN;
	} else if (strcmp(sel, IR_AUDIO_NONE_TOKEN) == 0) {
		mode = IR_AUDIO_NONE;
	} else {
		mode = IR_AUDIO_SOURCE;
		snprintf(name, sizeof(name), "%s", sel);
	}

	obs_weak_source_t *new_weak = NULL;
	if (mode == IR_AUDIO_SOURCE) {
		obs_source_t *s = obs_get_source_by_name(name);
		if (s) {
			new_weak = obs_source_get_weak_source(s);
			obs_source_release(s);
		}
	}

	pthread_mutex_lock(&ir->mutex);
	ir->audio_mode = mode;
	snprintf(ir->audio_source_name, sizeof(ir->audio_source_name), "%s", name);
	obs_weak_source_t *old_weak = ir->audio_weak;
	ir->audio_weak = new_weak;
	pthread_mutex_unlock(&ir->mutex);
	obs_weak_source_release(old_weak);
}

static void ir_update(void *data, obs_data_t *settings)
{
	struct isolated_record *ir = (struct isolated_record *)data;
	pthread_mutex_lock(&ir->mutex);
	ir->mode = (enum ir_record_mode)obs_data_get_int(settings, "record_mode");
	pthread_mutex_unlock(&ir->mutex);

	update_audio_override(ir, settings);

	/* If recording and encoder settings are editable (not active), push the
	 * bitrate/format change through on next start; for simplicity we update
	 * the live encoder only when idle. */
	if (!ir->output_active && ir->video_encoder && !obs_encoder_active(ir->video_encoder))
		obs_encoder_update(ir->video_encoder, settings);
}

static void *ir_create(obs_data_t *settings, obs_source_t *source)
{
	struct isolated_record *ir = (struct isolated_record *)bzalloc(sizeof(struct isolated_record));
	ir->source = source;
	ir->record_hotkey = OBS_INVALID_HOTKEY_ID;
	pthread_mutex_init(&ir->mutex, NULL);
	pthread_mutex_init(&ir->audio_buf_mutex, NULL);
	ir_update(ir, settings);
	ir::registry_add(ir);
	return ir;
}

static void ir_destroy(void *data)
{
	struct isolated_record *ir = (struct isolated_record *)data;

	/* Remove from the dock registry first so no snapshot/control can find
	 * us once teardown begins. */
	ir::registry_remove(ir);

	/* Mark closing first so the audio callback bails out, then tear the
	 * pipeline down synchronously. No async stop callbacks are ever armed,
	 * so nothing can reference this struct after we free it. */
	pthread_mutex_lock(&ir->mutex);
	ir->closing = true;
	pthread_mutex_unlock(&ir->mutex);

	stop_recording(ir, true /* full_teardown */);

	if (ir->record_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(ir->record_hotkey);

	obs_weak_source_release(ir->audio_weak);

	/* stop_recording() above already removed the capture cb and freed the
	 * deques via release_resources(); the mutex is now safe to destroy. */
	pthread_mutex_destroy(&ir->audio_buf_mutex);
	pthread_mutex_destroy(&ir->mutex);
	bfree(ir);
}

/* Decide whether one recorder should be recording, and reconcile its pipeline.
 * Called from the filter's video_tick (graphics thread, TICK phase — a safe
 * point to mutate the obs_view graph and start/stop outputs). */
static void reconcile(struct isolated_record *ir)
{
	obs_source_t *parent = obs_filter_get_parent(ir->source);
	if (!parent || obs_obj_is_private(parent))
		return;

	if (ir->closing)
		return;

	/* If an override audio source was named but not yet resolved (e.g. it
	 * loaded after this filter, or was renamed/re-added), try once now. */
	pthread_mutex_lock(&ir->mutex);
	const bool need_resolve = ir->audio_mode == IR_AUDIO_SOURCE && !ir->audio_weak && ir->audio_source_name[0];
	char want_name[256];
	if (need_resolve)
		snprintf(want_name, sizeof(want_name), "%s", ir->audio_source_name);
	pthread_mutex_unlock(&ir->mutex);
	if (need_resolve) {
		obs_source_t *s = obs_get_source_by_name(want_name);
		if (s) {
			obs_weak_source_t *w = obs_source_get_weak_source(s);
			obs_source_release(s);
			pthread_mutex_lock(&ir->mutex);
			/* Re-check: the user may have changed the selection meanwhile. */
			if (ir->audio_mode == IR_AUDIO_SOURCE && !ir->audio_weak &&
			    strcmp(ir->audio_source_name, want_name) == 0)
				ir->audio_weak = w;
			else
				w = NULL; /* discard; another update won the race */
			pthread_mutex_unlock(&ir->mutex);
			obs_weak_source_release(w); /* NULL if we kept it */
		}
	}

	/* Register the toggle hotkey lazily once we know the parent. */
	if (ir->record_hotkey == OBS_INVALID_HOTKEY_ID) {
		ir->record_hotkey = obs_hotkey_register_source(parent, "isolated_record.toggle",
							       obs_module_text("ToggleRecording"), record_hotkey, ir);
	}

	/* Determine desired recording state.
	 *
	 * want_record is the single source of truth for ON/OFF (dock Record/Stop,
	 * Record All/Stop All, hotkey). In "when active" mode we additionally
	 * auto-arm want_record on the rising edge of the source becoming active,
	 * and gate recording on the source actually being active — so Stop All is
	 * a true kill-switch and a stopped source resumes when it next activates. */
	const bool src_active = obs_source_enabled(ir->source) && obs_source_active(parent);
	bool should_record;
	pthread_mutex_lock(&ir->mutex);
	const bool when_active = (ir->mode == IR_MODE_WHEN_ACTIVE);
	pthread_mutex_unlock(&ir->mutex);

	if (when_active) {
		if (src_active && !ir->prev_active)
			ir->want_record = true; /* auto-arm on activation */
		should_record = ir->want_record.load() && src_active;
	} else {
		should_record = ir->want_record.load();
	}
	ir->prev_active = src_active;

	/* Only build/keep the private view+encoder pipeline when we actually need
	 * it (recording or about to). Avoids creating a render pipeline for every
	 * idle source — cheaper and safer at startup. */
	bool have_pipeline = ir->video_output != NULL;
	if (should_record || ir->output_active)
		have_pipeline = ensure_pipeline(ir);

	const bool running = ir->file_output && obs_output_active(ir->file_output);

	if (ir->output_active && !running) {
		/* We started an output but it is no longer running. Give it a 1s
		 * grace to spin up; after that, treat it as finished/failed and
		 * release. (Graceful stops we requested also land here once the
		 * muxer has finalized the file.) */
		const uint64_t age = os_gettime_ns() - ir->start_time_ns;
		if (age > 1000000000ULL) {
			if (should_record) {
				blog(LOG_WARNING, "[isolated-record] output for '%s' stopped unexpectedly: %s",
				     rec_name(ir), obs_output_get_last_error(ir->file_output));
				ir->want_record = false; /* don't hammer a failing start */
			}
			release_resources(ir, false);
		}
		return;
	}

	/* Size change while recording: request a stop; we'll restart once the
	 * resources are released and should_record is still true. */
	if (ir->needs_restart && running) {
		ir->needs_restart = false;
		request_stop(ir);
		return;
	}

	if (should_record && !ir->output_active && have_pipeline) {
		obs_data_t *s = obs_source_get_settings(ir->source);
		bool ok = start_recording(ir, s);
		obs_data_release(s);
		if (!ok)
			ir->want_record = false;
	} else if (!should_record && running) {
		request_stop(ir); /* graceful; release happens when it goes inactive */
	}

	/* Refresh cached byte counter for the dock. */
	if (running)
		ir->bytes = obs_output_get_total_bytes(ir->file_output);

	/* While recording with audio, keep the capture callback pointed at the
	 * current target so a live "Embedded audio" change takes effect. (NONE
	 * recordings never open the audio path, so audio_output is NULL there.) */
	if (running && ir->audio_output)
		update_audio_capture(ir);
}

/* Reconcile from the filter's tick (graphics thread, TICK phase). The tick
 * phase — unlike the render phase — is a safe point to create/modify the
 * obs_view graph and start/stop outputs. (An earlier version reconciled from
 * obs_add_main_render_callback, but that runs during render where obs_view_add2
 * deadlocks against the video lock, freezing OBS on startup.) */
static void ir_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	reconcile((struct isolated_record *)data);
}

static void ir_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct isolated_record *ir = (struct isolated_record *)data;
	/* Transparent: do not alter the source's own rendering. */
	obs_source_skip_video_filter(ir->source);
}

static void ir_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	struct isolated_record *ir = (struct isolated_record *)data;
	/* Remove from the registry first: this blocks until any in-flight
	 * reconcile_all() finishes and prevents future ones from touching us,
	 * so the synchronous stop below cannot race the render hook. */
	ir::registry_remove(ir);
	pthread_mutex_lock(&ir->mutex);
	ir->closing = true;
	pthread_mutex_unlock(&ir->mutex);
	stop_recording(ir, false);
}

struct obs_source_info isolated_record_filter_info = {
	.id = "isolated_record_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = ir_get_name,
	.create = ir_create,
	.destroy = ir_destroy,
	.get_defaults = ir_get_defaults,
	.get_properties = ir_get_properties,
	.update = ir_update,
	.video_tick = ir_tick,
	.video_render = ir_render,
	.filter_remove = ir_filter_remove,
};
