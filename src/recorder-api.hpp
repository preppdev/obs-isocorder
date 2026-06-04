/*
 * Dock-facing API for Isolated Record.
 *
 * The Qt dock (dock.cpp) must not poke at struct isolated_record directly —
 * that struct is mutated on the graphics/audio threads. These functions take
 * the registry lock internally and return value-copied snapshots, or marshal
 * control requests safely.
 */
#pragma once

#include <obs.h>
#include <string>
#include <vector>

struct isolated_record;

namespace ir {

/* A point-in-time, lock-free-to-read copy of one recorder's state. */
struct RecorderStatus {
	void *handle;        /* opaque struct isolated_record* (registry-owned) */
	std::string name;    /* parent source name */
	bool active;         /* currently writing a file */
	bool want_record;    /* manual-mode request flag */
	bool when_active;    /* mode == record-when-active */
	int elapsed_sec;     /* seconds since recording started (0 if idle) */
	uint64_t bytes;      /* bytes written so far (0 if idle) */
	std::string file;    /* output file path (empty if idle) */
	std::string folder;  /* configured destination folder for this source */
};

/* Thread-safe snapshot of every active recorder, for the dock to render. */
std::vector<RecorderStatus> snapshot();

/* Toggle a specific recorder by its opaque handle (validated against the
 * registry before use). Safe to call from the UI thread. */
void set_record(void *handle, bool on);

/* Global controls. `session` is the shared subfolder name for a grouped Record
 * All (pass "" for none); generate it once with make_session_token() so video
 * and audio recorders land in the same folder. */
void record_all(bool on, const char *session);

/* Returns a timestamped session-subfolder name if Record All grouping is on,
 * else an empty string. */
std::string make_session_token();

/* Attach the Isolated Record filter to a source (so it shows up in the dock).
 * `source` is borrowed; the caller keeps its own reference. */
void add_to_source(obs_source_t *source);

/* Open OBS's per-source settings for a recorder's parent source. */
void open_settings(void *handle);

/* ---- Global plugin settings (persisted to the module config) ---- */

/* If true, a "Record All" event places that session's files in a per-session
 * subfolder (named by timestamp) inside each source's folder; if false, files
 * are written directly into each source's folder as before. */
bool group_record_all();
void set_group_record_all(bool on);

/* Default recording format (RecFormat2 value) that newly added sources inherit.
 * Defaults to Hybrid MOV on macOS, Hybrid MP4 on Windows, MKV elsewhere (with a
 * fallback if the hybrid output type isn't available). */
std::string default_format();
void set_default_format(const char *value);

/* Default recording quality (OBS RecQuality value: "Stream", "Small", "HQ",
 * "Lossless") that newly added sources inherit. Defaults to "Stream". */
std::string default_quality();
void set_default_quality(const char *value);

/* Default destination folder that newly added sources/recorders inherit. When
 * unset, falls back to the profile's recording path, then ~/Movies. */
std::string default_folder();
void set_default_folder(const char *value);

/* Persisted dock column layout (QHeaderView state, base64). Empty if unset. */
std::string dock_column_state();
void set_dock_column_state(const char *base64);

/* Load persisted settings (call once at module load). Saving happens in the
 * setters. */
void load_settings();

/* Registry maintenance — called by the engine, not the dock. */
void registry_add(void *handle);
void registry_remove(void *handle);

/* Invoke `fn` for every registered recorder while holding the registry lock,
 * so a recorder cannot be destroyed mid-reconcile. Called from the global
 * graphics render hook (engine), not the dock. */
void reconcile_all(void (*fn)(struct isolated_record *));

} // namespace ir
