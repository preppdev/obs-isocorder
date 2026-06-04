# Isolated Record

An OBS Studio plugin to record **individual sources to their own separate
files**, concurrently and independently of OBS's main recording/stream.

Add the **Isolated Record** filter to any source (right-click source →
Filters). Each source you add it to gets its own private render pipeline,
encoders, and output file — so you can capture, say, a webcam, a game capture,
and a microphone scene each to a separate file at the same time.

This is a clean-room re-implementation inspired by the fundamentals of
[exeldro/obs-source-record](https://github.com/exeldro/obs-source-record),
focused on a smaller, well-structured core with fully synchronous teardown.

## How it works

The filter is **transparent** in the source's normal render chain (it does not
alter how the source looks in your scene). Capture happens through a separate
`obs_view` that renders the parent source into a dedicated `video_t`, which
feeds a private video encoder. Audio is captured through a privately opened
`audio_t` that mixes the parent source's audio. Both feed an `ffmpeg_muxer`
output writing the source's own file.

```
parent source ──▶ obs_view ──▶ video_t ──▶ video encoder ─┐
              └──▶ private audio_t ──▶ audio encoder ──────┴──▶ ffmpeg_muxer ──▶ file
```

## The interface — "mission control" dock

Instead of OBS's default per-source filter-properties dialog, this plugin adds
a dockable panel (View → Docks → **Isolated Record**) that is the primary way
to drive everything:

```
┌─ Isolated Record ─────────────────────────────┐
│  ● Record All    ■ Stop All        + Add source│
├────────────────────────────────────────────────┤
│  ●  Webcam          00:02:14        412.0 MB  ⚙ │
│  ○  Game Capture       --              --     ⚙ │
│  ●  Mic / Aux       00:02:14         38.0 MB  ⚙ │
├────────────────────────────────────────────────┤
│  2 recording · 3 total                         │
└────────────────────────────────────────────────┘
```

- **+ Add source** attaches the (invisible) capture filter to any source for you.
- Per-row **Record/Stop** for manual sources; "record when active" rows follow
  their source automatically.
- Live **status dot**, **elapsed time**, and **file size**, refreshed 2×/sec.
- The **⚙** gear opens that source's settings (encoder/format/path).

The filter still exists, but only as invisible plumbing (it provides the
per-frame tick and the independent `obs_view` capture pipeline). Users interact
with the dock, not the filter dialog.

## Status

This is a **scaffold** — implemented:

- [x] Per-source isolated video + audio recording to its own file
- [x] Mission-control Qt dock (status, toggles, Record All / Stop All, Add source)
- [x] "Record when source active" and "Manual (button/hotkey)" modes
- [x] Encoder / format / bitrate / rescale options
- [x] `inc_showing` so off-screen sources still record
- [x] Synchronous, crash-safe teardown on unload & filter removal

**Loads and runs in OBS 32.1.2** on macOS (Apple Silicon). The dock appears
and OBS logs `[isolated-record] loaded version 0.1.0` with no errors. Built
against Qt **6.8.3** (matching OBS's bundled Qt exactly) and linked via
`@rpath` so it resolves to OBS's own frameworks at runtime.

Build + install on this machine with `./pack-macos.sh` (see that script and
`docs` below for the toolchain it expects). `./verify-build.sh` is a faster
compile/link-only check.

Verified so far: module load, dock registration/render, clean teardown path.
Not yet verified end-to-end: an actual recorded file from a click (3-click
test in the dock — Add source → Record → check the folder).

Known scaffold limitations / next steps:

- Graceful stop currently force-stops the muxer for synchronous teardown — fine
  for MKV; MP4/MOV finalization would benefit from an async graceful stop path.
- Per-recorder settings persistence relies on the filter's own settings; a
  dock-level config store could be added.
- Replay buffer, streaming, split-file, and a websocket vendor API are future
  work (the original obs-source-record has these).
- **Qt ABI:** the build must match OBS's bundled Qt version (6.8.3 here). The
  `pack-macos.sh` script pins this; if you upgrade OBS, re-check its Qt version
  (`strings .../QtCore.framework/QtCore | grep '^6\.'`).

## Build (out of tree)

```sh
cmake -S . -B build -DBUILD_OUT_OF_TREE=On \
  -DCMAKE_PREFIX_PATH="<path-to-libobs-and-frontend-api>"
cmake --build build
```

On macOS the produced plugin must be code-signed with a Developer ID and
notarized before it will load on other machines (Gatekeeper). The CMake
includes the standard plugin entitlements under `cmake/Bundle/macos/`.

## Layout

```
src/plugin-main.cpp       module entry, registers source + dock
src/isolated-record.hpp   per-source recorder definition
src/isolated-record.cpp   recording engine (view/encoders/output) + filter callbacks
src/recorder-api.hpp      thread-safe dock-facing API
src/recorder-api.cpp      registry + snapshot/control implementation
src/dock.hpp / dock.cpp   the Qt "mission control" dock
data/locale/en-US.ini     UI strings
cmake/                     OBS plugin helpers + macOS bundle config
```

## License

Licensed under the **GNU General Public License v2.0 or later** (GPL-2.0-or-later)
— see [LICENSE](LICENSE). This matches OBS Studio / libobs, which this plugin
links against. You are free to use, modify, and redistribute it; derivative
works distributed to others must also be made available under the GPL.

## Credits

Clean-room re-implementation inspired by the fundamentals of
[exeldro/obs-source-record](https://github.com/exeldro/obs-source-record)
(also GPL-2.0). Built on the OBS Studio plugin APIs.
