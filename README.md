# PyMSS ARA Plugin

An **ARA-enabled VST3 audio plugin** that performs **Music Source Separation** directly inside a DAW, powered by the Python [`pymss`](https://github.com/) package.

The plugin reads an entire ARA-assigned track, separates it into stems (vocals, drums, bass, etc.) on a background process running the pymss model, and lets you monitor each stem with per-track **mute / solo** — all without bouncing files or leaving the DAW.

> Windows-first. Builds and runs as a VST3 on Windows. Other platforms are not yet supported by the worker transport.

## Features

- **ARA playback rendering** — reads the assigned region/source through the ARA interface, no file import/export round-trip.
- **Dry passthrough before separation** — the original track plays through immediately. Source-to-host sample-rate mismatch is handled with a streaming resampler, so pitch stays correct without changing the project rate.
- **Background separation** — inference runs in a long-lived Python worker process (`pymss`), keeping the audio thread free. The UI never blocks.
- **Stem monitor** — after separation, each stem is listed with **Mute** and **Solo** controls. The output bus is the live mix of the active stems.
- **Model browser** — lists all models in the pymss catalog; already-installed models are listed first, uninstalled ones are auto-downloaded on use (`download=True`).
- **Inference parameters** — `batch_size`, `overlap_size`, `chunk_size` (0 = use the model default), `normalize`.
- **Progress + cancel** — real-time progress from the model, with a Cancel button that aborts the in-flight job.
- **Persistent settings** — Python interpreter path and model directory are stored at `~/.pymss/settings/ara.json`.

## Building

```bash
git clone --recurse-submodules <repo-url> pymss-ara
cd pymss-ara

# Configure (Visual Studio generator, x64)
cmake -B build -G "Visual Studio 18 2026" -A x64

# Build the VST3 (Release)
cmake --build build --target PyMSS_ARA_VST3 --config Release
```

The build:
- Produces `build/PyMSS_ARA_artefacts/Release/VST3/PyMSS ARA Plugin.vst3`.
- With `COPY_PLUGIN_AFTER_BUILD=TRUE` (the default in this project), installs it to `C:\Program Files\Common Files\VST3\`.
- Copies `python/worker.py` into the bundle's `Contents/Resources/`.

> **Close your DAW before rebuilding.** A loaded VST3 is file-locked by Windows, so the install step silently fails to overwrite it while the DAW is open.

## Configuration

On first launch the plugin creates `~/.pymss/settings/ara.json`:

```json
{
  "python_path": "",
  "model_path": ""
}
```

| Field | Description |
|---|---|
| `python_path` | Path to a Python interpreter that has `pymss` installed. Empty → system `python`. |
| `model_path` | Directory containing downloaded model files. Empty → pymss default model directory. |

You can edit this file by hand or use the **Settings** button in the plugin's top-right corner (with file/folder browsers).

The Python worker's stderr is redirected to `~/.pymss/logs/worker_stderr.txt` for troubleshooting.

## Usage

1. **Load the plugin as ARA** on an audio track in an ARA-capable host (e.g. Reaper, Studio One). Make sure to pick the `(ARA)` entry so the host assigns the region to the plugin.
2. Open the plugin UI. Press **play** — you should hear the original track (dry passthrough).
3. Pick a model from the dropdown. Uninstalled models are auto-downloaded when you separate.
4. (Optional) Adjust inference parameters. `0` means "use the model default".
5. Click **Start Separation**. Watch the progress bar; you can **Cancel** at any time.
6. When finished, the **Stems** panel lists each separated stem with **Mute (M)** and **Solo (S)** buttons. The output is the live mix of the active stems.

## Project layout

```
pymss-ara/
├── CMakeLists.txt
├── python/
│   └── worker.py              # pymss worker process (IPC + separation)
├── src/
│   ├── PluginProcessor.*      # AudioProcessor (1-in/1-out stereo)
│   ├── PluginEditor.*         # UI: settings, model picker, params, stem monitor
│   ├── PyMSSDocumentController.*   # ARA document controller (owns engine/worker)
│   ├── PyMSSPlaybackRenderer.*     # ARA renderer: dry passthrough + stem mix
│   ├── SeparationEngine.*     # background read → separate → cache, cancel
│   ├── WorkerClient.*         # spawns worker, framed IPC (Win32 pipes)
│   ├── WorkerProtocol.h       # shared frame helpers
│   ├── Settings.*             # ~/.pymss/settings/ara.json read/write
│   └── Stems.h                # shared data structures
└── third_party/
    ├── JUCE/                  # submodule
    └── ARA_SDK/               # submodule
```

## Architecture

```
 ┌─────────────────────── DAW (ARA host) ───────────────────────┐
 │                                                               │
 │  PyMSS ARA Plugin (VST3, C++/JUCE)                            │
 │   ├─ PyMSSAudioProcessor        1-in / 1-out stereo bus       │
 │   ├─ PyMSSPlaybackRenderer      region → dry/stem mix output  │
 │   ├─ PyMSSDocumentController    owns shared machinery         │
 │   │   ├─ SettingsStore          ~/.pymss/settings/ara.json    │
 │   │   ├─ WorkerClient           spawns + talks to the worker  │
 │   │   └─ SeparationEngine       background read/separate/cache│
 │   └─ PyMSSAudioProcessorEditor  model picker, params, monitor │
 │                  │                                            │
 │                  │  stdin/stdout binary frames                │
 │                  ▼                                            │
 │  python worker.py  (pymss)                                    │
 │   ├─ list_models / model_info                                 │
 │   └─ separate (MSSeparator) → stems (float32 PCM)             │
 └───────────────────────────────────────────────────────────────┘
```

### How separation works

1. The user clicks **Start Separation**. The engine reads the ARA audio source on a background thread, resamples it to the host sample rate, and sends it to the Python worker.
2. The worker resamples to the model's sample rate (44100 Hz), runs `MSSeparator.separate(...)`, resamples the stems back to the host rate, and streams progress back.
3. The engine caches the stems keyed by the audio source's persistent ID and notifies the editor.
4. During playback, the `PlaybackRenderer` mixes the **active** stems (per mute/solo) into the output bus. Before separation it passes the dry source through (resampled).

### IPC protocol

A binary framed protocol over the worker's stdin/stdout (see [python/worker.py](python/worker.py) and [src/WorkerProtocol.h](src/WorkerProtocol.h)):

```
uint32  headerLen   length of the JSON header
uint32  bodyLen     length of the binary body
bytes   headerLen   UTF-8 JSON header (single-line)
bytes   bodyLen     raw binary payload (interleaved float32 PCM for audio)
```

Requests: `ping`, `check_pymss`, `list_models`, `model_info`, `separate`, `cancel`, `shutdown`.
Replies/events: `ready`, `result`, `progress`, `error`.

## Prerequisites

### Build tools (Windows)
- **CMake ≥ 3.22**
- **Visual Studio 2022 / 2026** with the C++ workload (MSVC)
- Git

### Runtime
- A **Python interpreter with `pymss` installed** (and its model stack: PyTorch/CUDA, etc.). You configure its path in the plugin's Settings panel.

The third-party dependencies are checked in as git submodules:
- [JUCE](https://github.com/juce-framework/JUCE) 8.x — `third_party/JUCE`
- [ARA SDK](https://github.com/Celemony/ARA_SDK) 2.3.x — `third_party/ARA_SDK`

## Notes & limitations

- **Sample rate** — the model runs at 44100 Hz internally; resampling in/out is handled automatically. Dry passthrough also resamples, so the project rate need not match the source rate.
- **Region offsets** assume linear (non-stretched) playback, consistent with the JUCE ARA demo.
- **Pre-separation monitoring** reads the source on the audio thread; on very long sources this relies on the host's ARA read performance. If you hear dropouts only before separation, that is the likely cause.
- **macOS/Linux** — the worker launch currently uses Win32 anonymous pipes; a POSIX transport is needed for other platforms.
- The plugin keeps no per-source persistent state in the ARA archive, so stem selections are not saved with the DAW project (only the chosen model + parameters are).

## Credits

- [JUCE](https://juce.com/) — audio framework
- [ARA SDK](https://github.com/Celemony/ARA_SDK) — Celemony ARA
- [pymss](https://github.com/) — music source separation runtime

Plugin vendor: **pymss-project**.
