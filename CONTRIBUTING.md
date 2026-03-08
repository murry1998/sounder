# Contributing to Sounder

## Development Setup

### Prerequisites

- macOS 11.0+ (Apple Silicon or Intel)
- Node.js 18+
- CMake 3.22+
- Xcode Command Line Tools: `xcode-select --install`

### First-Time Setup

```bash
git clone https://github.com/murry1998/sounder.git
cd sounder
npm install
npm run setup:models
npm run rebuild:native
npm start
```

### Running in Development

```bash
# Standard launch
npm start

# Launch with proper microphone entitlements (macOS)
npm run dev:launch
```

## Code Structure

### Main Process (`src/main/`)

- `main.js` -- App lifecycle, window management, IPC handler registration
- `menu.js` -- Native macOS menu
- `aiAudioEngine.js` -- AI worker management and model registry
- `ai-worker.js` -- MusicGen inference (runs in child process)
- `scan-worker.js` -- Plugin scanning (crash-isolated)

### Preload (`src/preload/`)

- `preload.js` -- Context-isolated API bridge (renderer cannot access Node.js directly)

### Renderer (`src/renderer/`)

- `renderer.js` -- Main UI logic, track management, modal handlers
- `mixer-board.js` -- Mixer UI component
- `piano-roll.js` -- MIDI editor
- `undo-manager.js` -- Edit history
- `index.html` -- App shell and modal templates
- `styles.css` -- All styling

### Native Addon (`native/`)

- `src/SounderEngine.cpp` -- N-API exports and engine facade
- `src/AudioTrack.cpp` -- Audio playback with region/fade/loop
- `src/MidiTrack.cpp` -- MIDI sequencing and note management
- `src/AudioGraph.cpp` -- Track routing and device management
- `src/TransportEngine.cpp` -- Play/stop/record state machine
- `src/PluginHost.cpp` -- VST3/AU plugin loading and management
- `src/BuiltInEffect.cpp` -- 11 DSP effects (EQ, compressor, reverb, etc.)
- `src/StemSeparator.cpp` -- HTDemucs ONNX inference for stem separation
- `src/MidiGenerator.cpp` -- Algorithmic MIDI pattern generation
- `src/FileIO.cpp` -- Project and audio file I/O
- `include/` -- Header files for all C++ classes

## Adding a New Built-in Effect

1. Add the effect type enum in `native/include/BuiltInEffect.h`
2. Implement the DSP in the switch block in `native/src/BuiltInEffect.cpp`
3. Add parameter getters/setters following the existing pattern
4. The effect is automatically available through the insert slot UI

## Adding a New AI Model

1. Add the model entry to `AI_MODELS` in `src/main/aiAudioEngine.js`
2. Add download URLs to `scripts/download-models.js`
3. If the model uses a different architecture than MusicGen, add a handler in `src/main/ai-worker.js`

## Pull Requests

- Keep changes focused. One feature or fix per PR.
- Test that the app builds and runs: `npm run rebuild:native && npm start`
- Verify existing features still work (stems, MIDI AI, plugin hosting)
- Include a description of what changed and why.

## Reporting Issues

Open an issue at https://github.com/murry1998/sounder/issues with:

- Steps to reproduce
- Expected behavior
- Actual behavior
- macOS version and hardware (Intel or Apple Silicon)
- Console output if relevant
