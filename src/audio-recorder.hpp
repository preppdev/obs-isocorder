/*
 * Isolated Record - audio-only recorders.
 *
 * Unlike the video path (a filter on a source), audio recorders are standalone
 * manager-owned objects that write a detached audio-only file. Two flavours:
 *   - SOURCE mode: capture a specific audio source's own audio (e.g. a mic /
 *     audio-input source already in OBS) via a private audio_output.
 *   - TRACK mode: capture one of OBS's 6 global mix tracks directly — useful to
 *     iso-record audio that isn't otherwise being recorded as video.
 *
 * Audio recording involves NO graphics/obs_view work, so these are reconciled
 * from the dock's UI-thread timer (obs_output/encoder/audio_output calls are
 * thread-agnostic). Persisted to the plugin config and recreated on load.
 */
#pragma once

#include <obs.h>
#include <util/threading.h>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>

struct audio_recorder {
	char id[64];             /* stable id for persistence */
	obs_weak_source_t *weak; /* SOURCE mode target (NULL until resolved) */
	char source_name[160];   /* SOURCE mode target name (for lazy resolve) */
	int global_track;        /* 0 = source mode; 1..6 = global mix track */

	/* Config (guarded by mutex). */
	pthread_mutex_t mutex;
	char name[160];
	char path[512];
	char filename_format[128];
	char session_subfolder[128]; /* set by a grouped Record All */

	/* Runtime. Audio is captured by tapping an audio_t via audio_output_connect
	 * and written as a WAV file (no OBS output / FFmpeg muxer, which can't do
	 * audio-only). */
	audio_t *priv_audio; /* private mix for SOURCE mode (NULL in TRACK mode) */
	audio_t *cap_audio;  /* the audio_t we connected to (priv_audio or global) */
	int mix_idx;         /* mix index we tapped (0 for source; track-1 for track) */
	FILE *wav;           /* open output file */
	uint32_t channels;
	uint32_t sample_rate;
	uint64_t data_bytes; /* PCM bytes written (for the WAV header fixup) */
	std::atomic<bool> want_record;
	std::atomic<bool> output_active;
	std::atomic<uint64_t> bytes;
	uint64_t start_time_ns;
	char current_file[512];
	bool closing;
};

namespace air {

/* Create recorders. `source` is borrowed. Returns the new handle (opaque). */
void add_source(obs_source_t *source);
void add_track(int track /* 1..6 */);
void remove(void *handle);

/* Reconcile every audio recorder (call from the dock's UI-thread timer). */
void reconcile_all();

/* Controls (mirror the video ir:: API; handle validated internally). */
void set_record(void *handle, bool on);
void record_all(bool on, const char *session /* "" = none */);
bool is_registered(void *handle);

/* Stop + free everything (module unload). */
void shutdown_all();

/* Persistence. */
void load();
void save();

/* Status for the dock — appended to the unified list. */
struct Status {
	void *handle;
	std::string name;
	bool active;
	int elapsed_sec;
	uint64_t bytes;
	std::string file;
	std::string folder;
};
std::vector<Status> snapshot();

/* Per-recorder settings (audio recorders write 16-bit PCM WAV). */
std::string get_path(void *handle);
void set_path(void *handle, const char *path);

} // namespace air
