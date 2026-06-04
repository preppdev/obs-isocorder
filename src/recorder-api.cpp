/*
 * Registry + dock-facing API implementation.
 *
 * Threading model:
 *   - The registry list is added to / removed from on the UI thread
 *     (ir_create / ir_destroy) and the dock's calls also run on the UI thread,
 *     so list structure is only ever touched from one thread. The graphics
 *     thread never mutates the list (it only updates per-recorder atomics).
 *   - Per-recorder live values exposed to the dock (bytes, want_record) are
 *     std::atomic. Strings are copied under the per-recorder mutex.
 */
#include "recorder-api.hpp"
#include "isolated-record.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/config-file.h>
#include <mutex>
#include <ctime>

namespace {

std::mutex g_registry_mutex;
std::vector<struct isolated_record *> g_recorders;

/* Global settings, loaded from / saved to the module config. */
std::mutex g_settings_mutex;
bool g_group_record_all = false;
std::string g_default_format; /* empty => use the platform default */
std::string g_default_folder; /* empty => use profile path / ~/Movies */
std::string g_default_quality; /* empty => "Stream" */
std::string g_dock_columns;   /* base64 QHeaderView state */

bool output_type_exists(const char *id)
{
	const char *out_id;
	size_t i = 0;
	while (obs_enum_output_types(i++, &out_id))
		if (strcmp(out_id, id) == 0)
			return true;
	return false;
}

std::string platform_default_format()
{
#if defined(__APPLE__)
	return output_type_exists("mov_output") ? "hybrid_mov" : "mov";
#elif defined(_WIN32)
	return output_type_exists("mp4_output") ? "hybrid_mp4" : "mp4";
#else
	return "mkv";
#endif
}

bool is_registered(void *handle)
{
	for (auto *r : g_recorders)
		if (r == handle)
			return true;
	return false;
}

char *settings_path()
{
	return obs_module_get_config_path(obs_current_module(), "settings.json");
}

void save_settings()
{
	obs_data_t *d = obs_data_create();
	{
		std::lock_guard<std::mutex> lock(g_settings_mutex);
		obs_data_set_bool(d, "group_record_all", g_group_record_all);
		if (!g_default_format.empty())
			obs_data_set_string(d, "default_format", g_default_format.c_str());
		if (!g_default_folder.empty())
			obs_data_set_string(d, "default_folder", g_default_folder.c_str());
		if (!g_default_quality.empty())
			obs_data_set_string(d, "default_quality", g_default_quality.c_str());
		if (!g_dock_columns.empty())
			obs_data_set_string(d, "dock_columns", g_dock_columns.c_str());
	}
	char *path = settings_path();
	if (path) {
		/* Ensure the config directory exists. */
		char *dir = obs_module_get_config_path(obs_current_module(), "");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
		obs_data_save_json(d, path);
		bfree(path);
	}
	obs_data_release(d);
}

} // namespace

namespace ir {

bool group_record_all()
{
	std::lock_guard<std::mutex> lock(g_settings_mutex);
	return g_group_record_all;
}

void set_group_record_all(bool on)
{
	{
		std::lock_guard<std::mutex> lock(g_settings_mutex);
		g_group_record_all = on;
	}
	save_settings();
}

std::string default_format()
{
	{
		std::lock_guard<std::mutex> lock(g_settings_mutex);
		if (!g_default_format.empty())
			return g_default_format;
	}
	return platform_default_format();
}

void set_default_format(const char *value)
{
	{
		std::lock_guard<std::mutex> lock(g_settings_mutex);
		g_default_format = value ? value : "";
	}
	save_settings();
}

std::string default_quality()
{
	std::lock_guard<std::mutex> lock(g_settings_mutex);
	return g_default_quality.empty() ? "Stream" : g_default_quality;
}

void set_default_quality(const char *value)
{
	{
		std::lock_guard<std::mutex> lock(g_settings_mutex);
		g_default_quality = value ? value : "";
	}
	save_settings();
}

std::string default_folder()
{
	{
		std::lock_guard<std::mutex> lock(g_settings_mutex);
		if (!g_default_folder.empty())
			return g_default_folder;
	}
	config_t *config = obs_frontend_get_profile_config();
	const char *p = config ? config_get_string(config, "SimpleOutput", "FilePath") : NULL;
	if (p && *p)
		return p;
	const char *home = getenv("HOME");
	return home ? std::string(home) + "/Movies" : "";
}

void set_default_folder(const char *value)
{
	{
		std::lock_guard<std::mutex> lock(g_settings_mutex);
		g_default_folder = value ? value : "";
	}
	save_settings();
}

std::string dock_column_state()
{
	std::lock_guard<std::mutex> lock(g_settings_mutex);
	return g_dock_columns;
}

void set_dock_column_state(const char *base64)
{
	{
		std::lock_guard<std::mutex> lock(g_settings_mutex);
		g_dock_columns = base64 ? base64 : "";
	}
	save_settings();
}

void load_settings()
{
	char *path = settings_path();
	if (!path)
		return;
	obs_data_t *d = obs_data_create_from_json_file(path);
	bfree(path);
	if (!d)
		return;
	std::lock_guard<std::mutex> lock(g_settings_mutex);
	g_group_record_all = obs_data_get_bool(d, "group_record_all");
	const char *df = obs_data_get_string(d, "default_format");
	g_default_format = df ? df : "";
	const char *dfo = obs_data_get_string(d, "default_folder");
	g_default_folder = dfo ? dfo : "";
	const char *dq = obs_data_get_string(d, "default_quality");
	g_default_quality = dq ? dq : "";
	const char *dc = obs_data_get_string(d, "dock_columns");
	g_dock_columns = dc ? dc : "";
	obs_data_release(d);
}

void registry_add(void *handle)
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	g_recorders.push_back(reinterpret_cast<struct isolated_record *>(handle));
}

void registry_remove(void *handle)
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	for (size_t i = 0; i < g_recorders.size(); i++) {
		if (g_recorders[i] == handle) {
			g_recorders.erase(g_recorders.begin() + i);
			return;
		}
	}
}

void reconcile_all(void (*fn)(struct isolated_record *))
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	for (auto *r : g_recorders)
		fn(r);
}

std::vector<RecorderStatus> snapshot()
{
	std::vector<RecorderStatus> out;
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	out.reserve(g_recorders.size());
	for (auto *r : g_recorders) {
		RecorderStatus s = {};
		s.handle = r;
		s.active = r->output_active.load();
		s.want_record = r->want_record.load();
		s.bytes = r->bytes.load();

		pthread_mutex_lock(&r->mutex);
		s.when_active = (r->mode == IR_MODE_WHEN_ACTIVE);
		obs_source_t *parent = obs_filter_get_parent(r->source);
		const char *name = parent ? obs_source_get_name(parent) : "";
		s.name = name ? name : "";
		s.file = r->current_file;
		const uint64_t start = r->start_time_ns;
		pthread_mutex_unlock(&r->mutex);

		obs_data_t *st = obs_source_get_settings(r->source);
		const char *folder = obs_data_get_string(st, "path");
		s.folder = folder ? folder : "";
		obs_data_release(st);

		s.elapsed_sec = (s.active && start) ? (int)((os_gettime_ns() - start) / 1000000000ULL) : 0;
		out.push_back(std::move(s));
	}
	return out;
}

void set_record(void *handle, bool on)
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	if (!is_registered(handle))
		return;
	auto *r = reinterpret_cast<struct isolated_record *>(handle);
	if (on) {
		/* Individual record goes to the source's own folder, not a group. */
		pthread_mutex_lock(&r->mutex);
		r->session_subfolder[0] = '\0';
		pthread_mutex_unlock(&r->mutex);
	}
	r->want_record = on;
}

std::string make_session_token()
{
	if (!group_record_all())
		return "";
	time_t t = time(NULL);
	struct tm tmv;
	localtime_r(&t, &tmv);
	char buf[128];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H-%M-%S", &tmv);
	return buf;
}

void record_all(bool on, const char *session)
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	for (auto *r : g_recorders) {
		pthread_mutex_lock(&r->mutex);
		snprintf(r->session_subfolder, sizeof(r->session_subfolder), "%s", (on && session) ? session : "");
		pthread_mutex_unlock(&r->mutex);
		r->want_record = on;
	}
}

void add_to_source(obs_source_t *source)
{
	if (!source)
		return;
	/* Don't double-add. */
	obs_source_t *existing = obs_source_get_filter_by_name(source, "Isolated Record");
	if (existing) {
		obs_source_release(existing);
		return;
	}
	obs_source_t *filter = obs_source_create("isolated_record_filter", "Isolated Record", nullptr, nullptr);
	if (!filter)
		return;
	obs_source_filter_add(source, filter);
	obs_source_release(filter);
}

void open_settings(void *handle)
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	if (!is_registered(handle))
		return;
	auto *r = reinterpret_cast<struct isolated_record *>(handle);
	obs_source_t *parent = obs_filter_get_parent(r->source);
	if (parent)
		obs_frontend_open_source_filters(parent);
}

} // namespace ir
