# Sounder

> **Beta Release** -- Sounder is under active development. Core audio, MIDI, mixing, and plugin hosting are functional. AI features (MusicGen, stem separation) work but may have rough edges. Expect bugs. [Report issues here.](https://github.com/murry1998/sounder/issues)

A desktop DAW (Digital Audio Workstation) built with Electron and a native C++ audio engine powered by JUCE. Features AI music generation, stem separation, VST3/AU plugin hosting, and a full suite of built-in effects and instruments.

## Features

**Audio Engine**
- Low-latency native audio via JUCE and N-API
- 48 kHz / 512-sample buffer playback
- Audio and MIDI track types with bus routing
- Master output with metering

**Recording and Editing**
- Audio recording with live waveform preview
- MIDI recording from hardware controllers
- Audio region editing (clip, loop, fade in/out)
- Track splitting and duplication
- Sync to BPM (time-stretch audio to match project tempo without pitch change)
- Quantize (snap detected beats to a regular grid)
- Transpose (pitch shift with optional tempo preservation)
- Normalize (peak level adjustment)
- Undo/redo (100-action history)

**Mixing**
- Per-track volume, pan, mute, solo
- Bus/aux tracks for submixing
- Mixer board UI with real-time level meters
- 5 insert effect slots per track (native or plugin)
- Master bus with 5 insert slots

**Built-in Effects**
- EQ (3-band parametric)
- Compressor (with sidechain)
- Gate
- Filter (LP/HP/BP)
- Distortion
- Chorus
- Phaser
- Delay
- Reverb
- Limiter

**Built-in Instruments**
- BasicSynth (polyphonic synthesizer)
- SamplePlayer
- DrumKit (16-pad, GM-mapped)
- SFZ soundfont loader

**Plugin Hosting**
- VST3, AU plugin support
- Crash-isolated plugin scanning (worker process)
- Plugin editor windows
- State save/restore

**AI Features**
- Text-to-music generation via MusicGen Small (Meta, 300M params)
- 4-stem audio separation via HTDemucs (vocals, bass, drums, other)
- Algorithmic MIDI generation (melody, bass, chords, arpeggios, drums)

**Import/Export**
- Import: WAV, AIFF, MP3, FLAC, OGG, M4A, MIDI
- Export: WAV (16/24/32-bit), AIFF, MP3 (128-320 kbps), MIDI
- Per-track stem export
- Project save/load (.sounder format with embedded audio)

**MIDI**
- Piano roll editor with grid snapping
- MIDI learn for controller mapping
- Hardware MIDI device support
- CC data editing

## Requirements

- **macOS 11.0+** (Apple Silicon or Intel)
- **Node.js 18+**
- **CMake 3.22+**
- **Xcode Command Line Tools** (`xcode-select --install`)

## Quick Start

```bash
git clone https://github.com/murry1998/sounder.git
cd sounder
npm install
npm run setup:models    # Downloads AI models (~2.3 GB)
npm run rebuild:native  # Compiles the C++ audio engine
npm start
```

## AI Models

Sounder uses two AI models that are downloaded separately (not included in the repository due to size):

| Model | Size | Purpose |
|-------|------|---------|
| MusicGen Small | ~2.1 GB | Text-to-music generation |
| HTDemucs | ~166 MB | Audio stem separation |

Run `npm run setup:models` to download them from HuggingFace. Files are placed in `resources/models/`.

MusicGen runs in a separate Node.js worker process to avoid ONNX Runtime conflicts with the native audio engine. First generation takes 30-60 seconds as the model loads into memory; subsequent generations are faster.

## Architecture

```
src/
  main/           Electron main process (app lifecycle, IPC, AI engine)
  preload/        Context-isolated bridge between main and renderer
  renderer/       HTML/CSS/JS UI (mixer, piano roll, transport, modals)
native/
  src/            C++ audio engine (JUCE + N-API bindings)
  include/        C++ headers
  JUCE/           JUCE framework (submodule)
resources/
  models/         AI model files (downloaded via setup script)
scripts/          Build and setup utilities
```

**Main process** handles app lifecycle, native addon loading, IPC routing, and AI generation coordination.

**Native addon** (`sounder_engine.node`) provides real-time audio processing, MIDI sequencing, plugin hosting, and DSP effects via JUCE, exposed to JavaScript through N-API.

**Renderer** is a single-page HTML/JS application with canvas-based waveform rendering, a mixer board, piano roll editor, and modal dialogs for AI features.

**AI worker** runs MusicGen inference in a separate Node.js child process using `@huggingface/transformers` and `onnxruntime-node`, communicating with the main process via IPC.

## Tech Stack

- **Electron 33** -- Application framework
- **JUCE 8** -- Audio processing, DSP, plugin hosting
- **ONNX Runtime 1.24** -- ML inference (stem separation in C++, music generation in JS)
- **transformers.js** -- HuggingFace model loading and tokenization
- **N-API 8** -- Native C++ to JavaScript bridge
- **CMake** -- Native build system (via cmake-js)

## Building the Native Addon

The native audio engine requires compilation:

```bash
# Development build
npm run build:native

# Electron-compatible build (required for packaging)
npm run rebuild:native
```

Prerequisites for native compilation:
- CMake 3.22+
- C++17 compiler (clang via Xcode)
- JUCE (included as subdirectory in `native/JUCE/`)
- ONNX Runtime (auto-fetched by CMake from GitHub releases)

## License

Copyright (C) 2026 Dylan James Brock

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

See [LICENSE](LICENSE) for the full text.

JUCE is used under its [GPL v3 license](https://juce.com/legal/juce-8-license/).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and guidelines.
