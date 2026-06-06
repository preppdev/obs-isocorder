// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 preppdev — Isolated Record (obs-isocorder)

/*
 * Isolated Record - OBS Studio plugin
 *
 * A source filter that records the source it is attached to into its OWN
 * dedicated file, independently of OBS's main recording/streaming. Each
 * source gets its own private render pipeline (obs_view), video encoder,
 * audio encoder, and ffmpeg output.
 *
 * This header declares the per-filter recorder. The implementation lives in
 * isolated-record.cpp. The design follows the proven approach used by
 * exeldro/obs-source-record: the filter itself is transparent in the video
 * chain (it does not alter the source it is attached to); the actual capture
 * happens through a separate obs_view that renders the parent source into a
 * private video_t pipeline feeding the encoder.
 */
#pragma once

#include <obs-module.h>
#include <util/threading.h>
#include <util/deque.h>
#include <media-io/audio-io.h>
#include <atomic>

/* obs_view channel used to feed the parent source into the private pipeline. */
#define IR_SOURCE_CHANNEL 0

/* How recording starts/stops. */
enum ir_record_mode {
	IR_MODE_WHEN_ACTIVE = 0, /* record whenever the source is enabled/visible */
	IR_MODE_MANUAL = 1,      /* start/stop via the property buttons or hotkey */
};

/* Which audio gets embedded in this source's video file. */
enum ir_audio_mode {
	IR_AUDIO_OWN = 0,    /* the parent source's own audio (default) */
	IR_AUDIO_NONE = 1,   /* no audio track at all (video-only file) */
	IR_AUDIO_SOURCE = 2, /* a specific other audio source (audio_weak) */
};

struct isolated_record {
	obs_source_t *source; /* the filter source itself */

	/* Private render + encode pipeline for the attached source. */
	obs_view_t *view;
	video_t *video_output;
	audio_t *audio_output; /* privately opened, mixes the parent's audio */

	obs_encoder_t *video_encoder;
	obs_encoder_t *audio_encoder;
	obs_output_t *file_output;

	/* Dimensions the private pipeline is currently configured for. */
	uint32_t width;
	uint32_t height;

	/* State. Touched from the graphics thread (tick) and frontend/UI thread;
	 * use a mutex for the few cross-thread fields. */
	pthread_mutex_t mutex;
	enum ir_record_mode mode;

	/* Audio source override for the embedded track (guarded by mutex; read on
	 * the audio thread). Default IR_AUDIO_OWN = the parent's own audio. When
	 * IR_AUDIO_SOURCE, audio_weak points at the chosen source (resolved lazily
	 * from audio_source_name so it survives source load order / renames). */
	enum ir_audio_mode audio_mode;
	obs_weak_source_t *audio_weak;
	char audio_source_name[256];

	/* Push-based audio capture. While recording, a capture callback is
	 * registered on the resolved audio target; it appends that source's own
	 * audio into per-channel deques, which the private audio_output's
	 * input_callback drains. This replaces reading obs_source_get_audio_mix(),
	 * which only yields audio for sources actively rendered in the main mix —
	 * so isolated/off-program sources captured pure silence. Guarded by
	 * audio_buf_mutex (touched by the captured source's audio thread and the
	 * audio_output thread). */
	pthread_mutex_t audio_buf_mutex;
	struct deque audio_buf[MAX_AUDIO_CHANNELS];
	size_t audio_channels;         /* channels buffered = OBS output channels */
	uint32_t audio_sample_rate;    /* OBS sample rate */
	uint64_t audio_buf_ts;         /* ns timestamp of the oldest buffered sample */
	obs_source_t *audio_cb_source; /* source the capture cb is on (strong ref) */
	std::atomic<bool> output_active;
	std::atomic<bool> want_record;   /* manual-mode request flag */
	std::atomic<bool> starting;      /* an async start task is in flight */
	bool closing;                    /* filter is being removed/destroyed */
	bool needs_restart;              /* pipeline must be rebuilt (size change) */
	bool showing;                    /* we hold an inc_showing on the parent */
	bool prev_active;                /* last-seen source-active state (edge detect) */

	/* Live status for the dock UI. */
	uint64_t start_time_ns;          /* when the current recording started */
	char current_file[512];          /* path of the file being written */
	std::atomic<uint64_t> bytes;     /* bytes written so far (updated in tick) */

	/* Session subfolder for a "Record All" group (empty = none). Guarded by
	 * mutex; set by the dock's Record All, consumed when the output starts. */
	char session_subfolder[128];

	/* Hotkey to toggle manual recording. */
	obs_hotkey_id record_hotkey;
};

/* Registered with OBS in plugin-main.cpp. */
extern struct obs_source_info isolated_record_filter_info;
