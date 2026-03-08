const { app, BrowserWindow, ipcMain, dialog, Menu, systemPreferences, shell } = require('electron');
const { fork } = require('child_process');
const path = require('path');
const fs = require('fs');

// Native addon — try packaged (asar-unpacked) path first, then dev path
let sounderNative = null;
try {
  const devPath = path.join(__dirname, '../../native/build/Release/sounder_engine.node');
  const packedPath = devPath.replace('app.asar', 'app.asar.unpacked');
  if (fs.existsSync(packedPath)) {
    sounderNative = require(packedPath);
  } else {
    sounderNative = require(devPath);
  }
} catch (e) {
  console.log('Native addon not loaded — running in UI-only mode:', e.message);
}

let mainWindow = null;
const PROJECTS_DIR = path.join(app.getPath('documents'), 'Sounder');

// Spawn a worker process that loads the native addon fresh and scans plugins.
// If a plugin crashes the worker, the main app survives. Results are saved
// incrementally to ~/.sounder/known-plugins.xml by the worker. After the
// worker exits (cleanly or via crash), we reload from disk.
function runScanWorker(directory) {
  return new Promise((resolve) => {
    const workerPath = path.join(__dirname, 'scan-worker.js');
    const worker = fork(workerPath, [], { stdio: ['pipe', 'pipe', 'pipe', 'ipc'] });

    worker.on('exit', (code) => {
      // Reload persisted plugins from disk into the running engine
      if (sounderNative) sounderNative.reloadPlugins();
      const plugins = sounderNative ? sounderNative.getPluginList() : [];
      resolve({ count: plugins.length, partial: code !== 0 });
    });

    worker.on('error', () => {
      if (sounderNative) sounderNative.reloadPlugins();
      const plugins = sounderNative ? sounderNative.getPluginList() : [];
      resolve({ count: plugins.length, partial: true });
    });

    // Tell the worker to start scanning
    worker.send({ type: 'scan', directory: directory || '' });
  });
}

// ── Microphone permission handling ──
// On macOS Sequoia (15+), TCC enforces an "attribution chain" — the responsible
// process (typically the terminal or IDE that spawned the app) must also have the
// com.apple.security.device.audio-input entitlement. When launched from terminals
// that lack this entitlement (e.g., VS Code, Claude Code), the permission dialog
// is silently suppressed and access returns false while status remains
// "not-determined". Launching via `open Electron.app --args .` or from Finder
// makes the app its own responsible process, avoiding this issue.
async function requestMicrophonePermission() {
  const currentStatus = systemPreferences.getMediaAccessStatus('microphone');
  console.log('[Sounder] Microphone status before request:', currentStatus);

  if (currentStatus === 'granted') {
    return true;
  }

  // Try Electron's API (recommended approach)
  let granted = false;
  try {
    granted = await systemPreferences.askForMediaAccess('microphone');
    console.log('[Sounder] Electron askForMediaAccess result:', granted);
  } catch (e) {
    console.log('[Sounder] Electron mic request error:', e.message);
  }

  // Also try native AVFoundation as fallback
  if (!granted && sounderNative) {
    try {
      granted = await sounderNative.requestMicrophoneAccess();
      console.log('[Sounder] Native AVFoundation mic result:', granted);
    } catch (e) {
      console.log('[Sounder] Native mic request error:', e.message);
    }
  }

  const finalStatus = systemPreferences.getMediaAccessStatus('microphone');
  console.log('[Sounder] Microphone status after request:', finalStatus);

  // If status is still "not-determined" after requesting, the TCC attribution
  // chain is blocking us (parent process lacks entitlement). Guide the user.
  if (finalStatus === 'not-determined' && !granted) {
    console.log('[Sounder] WARNING: Microphone permission dialog was suppressed.');
    console.log('[Sounder] This typically happens when launching from a terminal');
    console.log('[Sounder] whose process lacks the audio-input entitlement.');
    console.log('[Sounder] To fix: launch with ./dev-launch.sh or:');
    console.log('[Sounder]   open node_modules/electron/dist/Electron.app --args "$(pwd)"');
    console.log('[Sounder] Or grant microphone access manually in:');
    console.log('[Sounder]   System Settings > Privacy & Security > Microphone');
  } else if (finalStatus === 'denied') {
    console.log('[Sounder] Microphone access was denied by the user.');
    console.log('[Sounder] To enable: System Settings > Privacy & Security > Microphone');
  }

  return finalStatus === 'granted';
}

app.whenReady().then(async () => {
  // Ensure projects directory exists
  fs.mkdirSync(PROJECTS_DIR, { recursive: true });

  // Request microphone permission on macOS
  if (process.platform === 'darwin') {
    const micGranted = await requestMicrophonePermission();
    console.log('[Sounder] Microphone access granted:', micGranted);
  }

  // Initialize native audio engine if available
  if (sounderNative) {
    sounderNative.initialize({
      sampleRate: 48000,
      bufferSize: 512,
      projectsDir: PROJECTS_DIR
    });
  }

  mainWindow = new BrowserWindow({
    width: 1400,
    height: 850,
    minWidth: 900,
    minHeight: 600,
    titleBarStyle: 'hiddenInset',
    backgroundColor: '#09090b',
    webPreferences: {
      preload: path.join(__dirname, '../preload/preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  });

  mainWindow.loadFile(path.join(__dirname, '../renderer/index.html'));


  // Register all IPC handlers
  registerIPCHandlers();

  // Build native macOS menu
  const menuTemplate = require('./menu.js')(mainWindow);
  Menu.setApplicationMenu(Menu.buildFromTemplate(menuTemplate));
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', () => {
  if (sounderNative) sounderNative.shutdown();
});

function registerIPCHandlers() {
  // ── Transport ──
  ipcMain.handle('engine:play', () => {
    if (sounderNative) sounderNative.play();
    return { ok: true };
  });
  ipcMain.handle('engine:stop', () => {
    if (sounderNative) sounderNative.stop();
    return { ok: true };
  });
  ipcMain.handle('engine:record', () => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    const result = sounderNative.record();
    return result && result.error ? { error: result.error } : { ok: true };
  });
  ipcMain.handle('engine:rewind', () => {
    if (sounderNative) sounderNative.rewind();
    return { ok: true };
  });
  ipcMain.handle('engine:seekTo', (_event, time) => {
    if (sounderNative) sounderNative.seekTo(time);
    return { ok: true };
  });
  ipcMain.handle('engine:getTransportState', () => {
    if (!sounderNative) return { state: 'stopped', currentTime: 0, totalDuration: 0 };
    return sounderNative.getTransportState();
  });

  // ── Loop ──
  ipcMain.handle('engine:setLoopEnabled', (_event, enabled) => {
    if (sounderNative) sounderNative.setLoopEnabled(enabled);
    return { ok: true };
  });
  ipcMain.handle('engine:setLoopRegion', (_event, start, end) => {
    if (sounderNative) sounderNative.setLoopRegion(start, end);
    return { ok: true };
  });

  // ── Tracks ──
  ipcMain.handle('engine:addTrack', (_event, name) => {
    if (!sounderNative) return { trackId: -1 };
    const trackId = sounderNative.addTrack(name);
    return { trackId };
  });
  ipcMain.handle('engine:removeTrack', (_event, trackId) => {
    if (sounderNative) sounderNative.removeTrack(trackId);
    return { ok: true };
  });
  ipcMain.handle('engine:setTrackVolume', (_event, trackId, volume) => {
    if (sounderNative) sounderNative.setTrackVolume(trackId, volume);
    return { ok: true };
  });
  ipcMain.handle('engine:setTrackPan', (_event, trackId, pan) => {
    if (sounderNative) sounderNative.setTrackPan(trackId, pan);
    return { ok: true };
  });
  ipcMain.handle('engine:setTrackMute', (_event, trackId, muted) => {
    if (sounderNative) sounderNative.setTrackMute(trackId, muted);
    return { ok: true };
  });
  ipcMain.handle('engine:setTrackSolo', (_event, trackId, solo) => {
    if (sounderNative) sounderNative.setTrackSolo(trackId, solo);
    return { ok: true };
  });
  ipcMain.handle('engine:setTrackArmed', (_event, trackId, armed) => {
    if (sounderNative) sounderNative.setTrackArmed(trackId, armed);
    return { ok: true };
  });
  ipcMain.handle('engine:getTrackWaveform', (_event, trackId, numPoints) => {
    if (!sounderNative) return { data: [], duration: 0 };
    const raw = sounderNative.getTrackWaveform(trackId, numPoints);
    const dur = sounderNative.getTrackDuration(trackId);
    // Convert to plain array so IPC structured clone works reliably
    const data = Array.from(raw);
    return { data, duration: dur || 0 };
  });
  ipcMain.handle('engine:getTrackDuration', (_event, trackId) => {
    if (!sounderNative) return 0;
    return sounderNative.getTrackDuration(trackId);
  });
  ipcMain.handle('engine:importAudioToTrack', (_event, trackId, filePath) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    sounderNative.importAudioToTrack(trackId, filePath);
    return { ok: true };
  });

  // ── Audio Region ──
  ipcMain.handle('engine:setAudioRegion', (_event, trackId, offset, clipStart, clipEnd, loopEnabled) => {
    if (sounderNative) sounderNative.setAudioRegion(trackId, offset, clipStart, clipEnd, loopEnabled);
    return { ok: true };
  });
  ipcMain.handle('engine:splitAudioTrack', async (_event, trackId, splitTime) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const result = sounderNative.splitAudioTrack(trackId, splitTime);
      if (!result || result.newTrackId === undefined) return { error: 'Split failed' };
      return { ok: true, newTrackId: result.newTrackId, newTrackName: result.newTrackName };
    } catch (e) {
      return { error: e.message || 'Split failed' };
    }
  });

  ipcMain.handle('engine:duplicateAudioTrack', async (_event, trackId) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const result = sounderNative.duplicateAudioTrack(trackId);
      if (!result || result.newTrackId === undefined) return { error: 'Duplicate failed' };
      return { ok: true, newTrackId: result.newTrackId, newTrackName: result.newTrackName };
    } catch (e) {
      return { error: e.message || 'Duplicate failed' };
    }
  });
  ipcMain.handle('engine:setAudioFades', async (_event, trackId, fadeIn, fadeOut) => {
    if (!sounderNative) return;
    sounderNative.setAudioFades(trackId, fadeIn, fadeOut);
  });

  ipcMain.handle('engine:getRecordingWaveform', (_event, trackId, numPoints) => {
    if (!sounderNative) return { data: [], duration: 0 };
    try {
      const result = sounderNative.getRecordingWaveform(trackId, numPoints);
      return { data: Array.from(result.data), duration: result.duration };
    } catch (e) {
      return { data: [], duration: 0 };
    }
  });

  // ── MIDI Tracks ──
  ipcMain.handle('engine:addMidiTrack', (_event, name) => {
    if (!sounderNative) return { trackId: -1 };
    const trackId = sounderNative.addMidiTrack(name);
    return { trackId };
  });
  ipcMain.handle('engine:removeMidiTrack', (_event, trackId) => {
    if (sounderNative) sounderNative.removeMidiTrack(trackId);
    return { ok: true };
  });
  ipcMain.handle('engine:setMidiTrackVolume', (_event, trackId, volume) => {
    if (sounderNative) sounderNative.setMidiTrackVolume(trackId, volume);
    return { ok: true };
  });
  ipcMain.handle('engine:setMidiTrackPan', (_event, trackId, pan) => {
    if (sounderNative) sounderNative.setMidiTrackPan(trackId, pan);
    return { ok: true };
  });
  ipcMain.handle('engine:setMidiTrackMute', (_event, trackId, muted) => {
    if (sounderNative) sounderNative.setMidiTrackMute(trackId, muted);
    return { ok: true };
  });
  ipcMain.handle('engine:setMidiTrackSolo', (_event, trackId, solo) => {
    if (sounderNative) sounderNative.setMidiTrackSolo(trackId, solo);
    return { ok: true };
  });
  ipcMain.handle('engine:setMidiTrackArmed', (_event, trackId, armed) => {
    if (sounderNative) sounderNative.setMidiTrackArmed(trackId, armed);
    return { ok: true };
  });
  ipcMain.handle('engine:setMidiTrackInstrument', async (_event, trackId, pluginId) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const result = await sounderNative.setMidiTrackInstrument(trackId, pluginId);
      return result && result.error ? { error: result.error } : { ok: true };
    } catch (e) {
      return { error: e.message || 'Failed to load plugin' };
    }
  });
  ipcMain.handle('engine:setMidiTrackBuiltInInstrument', (_event, trackId, type) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    const result = sounderNative.setMidiTrackBuiltInInstrument(trackId, type);
    return result && result.error ? { error: result.error } : { ok: true };
  });

  // ── MIDI Note Editing ──
  ipcMain.handle('engine:addMidiNote', (_event, trackId, noteNumber, startBeat, lengthBeats, velocity) => {
    if (!sounderNative) return {};
    return sounderNative.addMidiNote(trackId, noteNumber, startBeat, lengthBeats, velocity);
  });
  ipcMain.handle('engine:removeMidiNote', (_event, trackId, noteIndex) => {
    if (sounderNative) sounderNative.removeMidiNote(trackId, noteIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:moveMidiNote', (_event, trackId, noteIndex, newNote, newStart) => {
    if (sounderNative) sounderNative.moveMidiNote(trackId, noteIndex, newNote, newStart);
    return { ok: true };
  });
  ipcMain.handle('engine:resizeMidiNote', (_event, trackId, noteIndex, newLength) => {
    if (sounderNative) sounderNative.resizeMidiNote(trackId, noteIndex, newLength);
    return { ok: true };
  });
  ipcMain.handle('engine:setMidiNoteVelocity', (_event, trackId, noteIndex, velocity) => {
    if (sounderNative) sounderNative.setMidiNoteVelocity(trackId, noteIndex, velocity);
    return { ok: true };
  });
  ipcMain.handle('engine:getMidiNotes', (_event, trackId) => {
    if (!sounderNative) return { notes: [] };
    return sounderNative.getMidiNotes(trackId);
  });
  ipcMain.handle('engine:quantizeMidiNotes', (_event, trackId, gridSize) => {
    if (sounderNative) sounderNative.quantizeMidiNotes(trackId, gridSize);
    return { ok: true };
  });
  ipcMain.handle('engine:addMidiCC', (_event, trackId, cc, value, beat) => {
    if (sounderNative) sounderNative.addMidiCC(trackId, cc, value, beat);
    return { ok: true };
  });

  // ── AI MIDI Generation ──
  ipcMain.handle('engine:generateMidi', async (_event, config) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try { return sounderNative.generateMidi(config); }
    catch (e) { return { error: e.message }; }
  });
  ipcMain.handle('engine:injectMidiNotes', (_event, trackId, notes, clearFirst) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try { return sounderNative.injectMidiNotes(trackId, notes, clearFirst); }
    catch (e) { return { error: e.message }; }
  });

  // ── AI Audio Injection (generation via transformers.js in aiAudioEngine) ──
  ipcMain.handle('engine:injectAudioBuffer', (_event, trackId, waveform, sampleRate, numChannels) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try { return sounderNative.injectAudioBuffer(trackId, waveform, sampleRate, numChannels); }
    catch (e) { return { error: e.message }; }
  });

  // ── AI Audio Generation (transformers.js) ──
  const aiEngine = require('./aiAudioEngine.js');

  ipcMain.handle('ai:getModels', async () => {
    try { return aiEngine.getModels(); }
    catch (e) { return { models: [], error: e.message }; }
  });

  ipcMain.handle('ai:generateAudio', async (event, modelId, prompt, config) => {
    try {
      return await aiEngine.generateAudio(modelId, prompt, config, (progress) => {
        if (event.sender && !event.sender.isDestroyed()) {
          event.sender.send('ai:generateProgress', progress);
        }
      });
    } catch (e) { return { error: e.message }; }
  });

  ipcMain.handle('ai:cancelGeneration', async () => {
    try { return aiEngine.cancelGeneration(); }
    catch (e) { return { error: e.message }; }
  });

  // ── Built-in Instrument Parameters ──
  ipcMain.handle('engine:setBuiltInSynthParam', (_event, trackId, param, value) => {
    if (sounderNative) sounderNative.setBuiltInSynthParam(trackId, param, value);
    return { ok: true };
  });
  ipcMain.handle('engine:getBuiltInSynthParam', (_event, trackId, param) => {
    if (!sounderNative) return 0;
    return sounderNative.getBuiltInSynthParam(trackId, param);
  });
  ipcMain.handle('engine:loadSampleToPlayer', (_event, trackId, filePath) => {
    if (sounderNative) sounderNative.loadSampleToPlayer(trackId, filePath);
    return { ok: true };
  });
  ipcMain.handle('engine:loadDrumPadSample', (_event, trackId, padIndex, filePath) => {
    if (sounderNative) sounderNative.loadDrumPadSample(trackId, padIndex, filePath);
    return { ok: true };
  });
  ipcMain.handle('engine:getBuiltInInstrumentTypes', () => {
    if (!sounderNative) return [];
    return sounderNative.getBuiltInInstrumentTypes();
  });

  // ── SFZ Instruments ──
  ipcMain.handle('engine:loadSFZPreset', (_event, trackId, presetId) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    sounderNative.loadSFZPreset(trackId, presetId);
    return { ok: true };
  });
  ipcMain.handle('engine:getSFZPresets', () => {
    if (!sounderNative) return [];
    return sounderNative.getSFZPresets();
  });
  ipcMain.handle('engine:loadSFZFile', async (_event, trackId, filePath) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    if (!filePath) {
      const { dialog } = require('electron');
      const result = await dialog.showOpenDialog({
        title: 'Load SFZ Instrument',
        filters: [{ name: 'SFZ Files', extensions: ['sfz'] }],
        properties: ['openFile']
      });
      if (result.canceled || !result.filePaths.length) return { canceled: true };
      filePath = result.filePaths[0];
    }
    sounderNative.loadSFZFile(trackId, filePath);
    return { ok: true, path: filePath };
  });

  // ── Effects ──
  ipcMain.handle('engine:setTrackFxParam', (_event, trackId, fxType, param, value) => {
    if (sounderNative) sounderNative.setTrackFxParam(trackId, fxType, param, value);
    return { ok: true };
  });
  ipcMain.handle('engine:setTrackFxEnabled', (_event, trackId, fxType, enabled) => {
    if (sounderNative) sounderNative.setTrackFxEnabled(trackId, fxType, enabled);
    return { ok: true };
  });

  // ── Plugins (scan via worker process for crash isolation) ──
  ipcMain.handle('engine:scanPlugins', async () => {
    if (!sounderNative) return { count: 0, partial: false };
    return runScanWorker();
  });
  ipcMain.handle('engine:scanPluginDirectory', async () => {
    const dlg = await dialog.showOpenDialog(mainWindow, {
      title: 'Select Plugin Directory',
      properties: ['openDirectory']
    });
    if (dlg.canceled || !dlg.filePaths.length) return { canceled: true };
    if (!sounderNative) return { count: 0 };
    const result = await runScanWorker(dlg.filePaths[0]);
    result.directory = dlg.filePaths[0];
    return result;
  });
  ipcMain.handle('engine:getPluginList', () => {
    if (!sounderNative) return { plugins: [] };
    return { plugins: sounderNative.getPluginList() };
  });
  ipcMain.handle('engine:insertPlugin', async (_event, trackId, slotIndex, pluginId) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const result = await sounderNative.insertPlugin(trackId, slotIndex, pluginId);
      return result && result.error ? { error: result.error } : { ok: true };
    } catch (e) {
      return { error: e.message || 'Failed to load plugin' };
    }
  });
  ipcMain.handle('engine:removePlugin', (_event, trackId, slotIndex) => {
    if (sounderNative) sounderNative.removePlugin(trackId, slotIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:openPluginEditor', (_event, trackId, slotIndex) => {
    if (sounderNative) sounderNative.openPluginEditor(trackId, slotIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:openMidiPluginEditor', (_event, trackId, slotIndex) => {
    if (sounderNative) sounderNative.openMidiPluginEditor(trackId, slotIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:openMidiInstrumentEditor', (_event, trackId) => {
    if (sounderNative) sounderNative.openMidiInstrumentEditor(trackId);
    return { ok: true };
  });
  ipcMain.handle('engine:closePluginEditor', (_event, trackId, slotIndex) => {
    if (sounderNative) sounderNative.closePluginEditor(trackId, slotIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:getPluginState', (_event, trackId, slotIndex) => {
    if (!sounderNative) return {};
    return sounderNative.getPluginState(trackId, slotIndex);
  });

  // ── Master ──
  ipcMain.handle('engine:setMasterVolume', (_event, volume) => {
    if (sounderNative) sounderNative.setMasterVolume(volume);
    return { ok: true };
  });

  // ── Metronome ──
  ipcMain.handle('engine:setMetronomeVolume', (_event, volume) => {
    if (sounderNative) sounderNative.setMetronomeVolume(volume);
    return { ok: true };
  });
  ipcMain.handle('engine:setBPM', (_event, bpm) => {
    if (sounderNative) sounderNative.setBPM(bpm);
    return { ok: true };
  });
  ipcMain.handle('engine:setTimeSignature', (_event, numerator, denominator) => {
    if (sounderNative) sounderNative.setTimeSignature(numerator, denominator);
    return { ok: true };
  });

  // ── Audio Devices ──
  ipcMain.handle('engine:getAudioDevices', () => {
    if (!sounderNative) return [];
    return sounderNative.getAudioDevices();
  });
  ipcMain.handle('engine:getInputDevices', () => {
    if (!sounderNative) return [];
    return sounderNative.getInputDevices();
  });
  ipcMain.handle('engine:getAudioDeviceInfo', () => {
    if (!sounderNative) return {};
    return sounderNative.getAudioDeviceInfo();
  });
  ipcMain.handle('engine:setAudioDevice', (_event, name, sampleRate, bufferSize) => {
    if (!sounderNative) return { ok: false };
    return sounderNative.setAudioDevice(name, sampleRate, bufferSize);
  });
  ipcMain.handle('engine:setAudioDeviceSeparate', (_event, outputName, inputName, sampleRate, bufferSize) => {
    if (!sounderNative) return { ok: false };
    return sounderNative.setAudioDeviceSeparate(outputName, inputName, sampleRate, bufferSize);
  });

  // ── MIDI Input ──
  ipcMain.handle('engine:getMidiDevices', () => {
    if (!sounderNative) return [];
    return sounderNative.getMidiDevices();
  });
  ipcMain.handle('engine:openMidiDevice', (_event, identifier) => {
    if (!sounderNative) return false;
    return sounderNative.openMidiDevice(identifier);
  });
  ipcMain.handle('engine:closeMidiDevice', (_event, identifier) => {
    if (sounderNative) sounderNative.closeMidiDevice(identifier);
  });
  ipcMain.handle('engine:setMidiTarget', (_event, trackId) => {
    if (sounderNative) sounderNative.setMidiTarget(trackId);
  });
  ipcMain.handle('engine:startMidiLearn', (_event, paramPath) => {
    if (sounderNative) sounderNative.startMidiLearn(paramPath);
  });
  ipcMain.handle('engine:stopMidiLearn', () => {
    if (sounderNative) sounderNative.stopMidiLearn();
  });
  ipcMain.handle('engine:getMidiBindings', () => {
    if (!sounderNative) return [];
    return sounderNative.getMidiBindings();
  });
  ipcMain.handle('engine:removeMidiBinding', (_event, cc, channel) => {
    if (sounderNative) sounderNative.removeMidiBinding(cc, channel);
  });

  // ── File I/O ──
  ipcMain.handle('project:save', (_event, name) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const ok = sounderNative.saveProject(PROJECTS_DIR, name);
      if (!ok) return { error: 'Save failed' };
      return { ok: true, projectId: name };
    } catch (e) {
      return { error: e.message || 'Save failed' };
    }
  });
  ipcMain.handle('project:load', (_event, projectId) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    return sounderNative.loadProject(PROJECTS_DIR, projectId);
  });
  ipcMain.handle('project:list', () => {
    // List project directories in ~/Documents/Sounder/
    try {
      const entries = fs.readdirSync(PROJECTS_DIR, { withFileTypes: true });
      const projects = [];
      for (const entry of entries) {
        if (!entry.isDirectory()) continue;
        const jsonPath = path.join(PROJECTS_DIR, entry.name, 'project.json');
        if (!fs.existsSync(jsonPath)) continue;
        try {
          const data = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
          projects.push({
            id: entry.name,
            name: data.name || entry.name,
            date: data.updatedAt || data.createdAt || '',
            trackCount: (data.tracks || []).length + (data.midiTracks || []).length
          });
        } catch (e) { /* skip malformed */ }
      }
      projects.sort((a, b) => (b.date || '').localeCompare(a.date || ''));
      return { projects };
    } catch (e) {
      return { projects: [] };
    }
  });
  ipcMain.handle('project:delete', (_event, projectId) => {
    const projectDir = path.join(PROJECTS_DIR, projectId);
    if (fs.existsSync(projectDir)) {
      fs.rmSync(projectDir, { recursive: true, force: true });
    }
    return { ok: true };
  });
  ipcMain.handle('project:exportWAV', async () => {
    const result = await dialog.showSaveDialog(mainWindow, {
      title: 'Export WAV',
      defaultPath: 'Untitled.wav',
      filters: [{ name: 'WAV Audio', extensions: ['wav'] }]
    });
    if (result.canceled) return { canceled: true };
    if (sounderNative) sounderNative.exportWAV(result.filePath);
    return { ok: true, path: result.filePath };
  });
  ipcMain.handle('project:exportAIFF', async () => {
    const result = await dialog.showSaveDialog(mainWindow, {
      title: 'Export AIFF',
      defaultPath: 'Untitled.aiff',
      filters: [{ name: 'AIFF Audio', extensions: ['aiff', 'aif'] }]
    });
    if (result.canceled) return { canceled: true };
    if (sounderNative) sounderNative.exportAIFF(result.filePath);
    return { ok: true, path: result.filePath };
  });
  // ── Stem Separation ──
  ipcMain.handle('engine:separateStems', async (_event, trackId, options) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const stems = sounderNative.separateStems(trackId, options || {});
      if (!stems) return { error: 'Stem separation failed' };
      return { ok: true, tracks: Array.from(stems) };
    } catch (e) {
      return { error: e.message || 'Stem separation failed' };
    }
  });

  // ── Tempo Match ──
  ipcMain.handle('engine:quantizeAudio', async (_event, trackId, options) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const result = sounderNative.quantizeAudio(trackId, options || {});
      if (!result || !result.ok) return { error: result?.detectedBPM ? `Tempo match failed (detected ${result.detectedBPM.toFixed(1)} BPM)` : 'Could not detect BPM' };
      return { ok: true, detectedBPM: result.detectedBPM, targetBPM: result.targetBPM, duration: result.duration, waveform: Array.from(result.waveform) };
    } catch (e) {
      return { error: e.message || 'Tempo match failed' };
    }
  });

  ipcMain.handle('project:exportStems', async (_event, options) => {
    const result = await dialog.showOpenDialog(mainWindow, {
      title: 'Choose Export Folder',
      properties: ['openDirectory', 'createDirectory']
    });
    if (result.canceled || !result.filePaths.length) return { canceled: true };
    const outputDir = result.filePaths[0];
    if (!sounderNative) return { ok: false, error: 'No engine' };
    const exportResult = sounderNative.exportStems({ ...options, outputDir });
    return exportResult;
  });
  ipcMain.handle('midi:importFile', async (_event, trackId) => {
    const result = await dialog.showOpenDialog(mainWindow, {
      title: 'Import MIDI File',
      filters: [{ name: 'MIDI Files', extensions: ['mid', 'midi'] }],
      properties: ['openFile']
    });
    if (result.canceled || !result.filePaths.length) return { canceled: true };
    if (!sounderNative) return { error: 'No engine' };
    return sounderNative.importMidiFile(trackId, result.filePaths[0]);
  });
  ipcMain.handle('midi:exportFile', async (_event, trackId) => {
    const result = await dialog.showSaveDialog(mainWindow, {
      title: 'Export MIDI File',
      defaultPath: 'track.mid',
      filters: [{ name: 'MIDI Files', extensions: ['mid', 'midi'] }]
    });
    if (result.canceled) return { canceled: true };
    if (!sounderNative) return { error: 'No engine' };
    return sounderNative.exportMidiFile(trackId, result.filePath);
  });
  ipcMain.handle('project:importAudioFile', async () => {
    const result = await dialog.showOpenDialog(mainWindow, {
      title: 'Import Audio File',
      filters: [
        { name: 'Audio Files', extensions: ['wav', 'aiff', 'aif', 'mp3', 'flac', 'ogg', 'm4a'] }
      ],
      properties: ['openFile', 'multiSelections']
    });
    if (result.canceled) return { canceled: true };
    const imported = [];
    for (const filePath of result.filePaths) {
      const name = path.basename(filePath, path.extname(filePath));
      if (sounderNative) {
        const trackId = sounderNative.addTrack(name);
        sounderNative.importAudioToTrack(trackId, filePath);
        const rawWf = sounderNative.getTrackWaveform(trackId, 4000);
        const waveform = Array.from(rawWf);
        const duration = sounderNative.getTrackDuration(trackId);
        imported.push({ trackId, fileName: path.basename(filePath), waveform, duration });
      }
    }
    return { ok: true, tracks: imported };
  });

  // ── Built-in Effects ──
  ipcMain.handle('engine:getBuiltInEffectTypes', () => {
    if (!sounderNative) return [];
    return sounderNative.getBuiltInEffectTypes();
  });
  ipcMain.handle('engine:insertBuiltInEffect', (_event, trackId, slotIndex, effectType) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    const result = sounderNative.insertBuiltInEffect(trackId, slotIndex, effectType);
    return result && result.error ? { error: result.error } : { ok: true };
  });
  ipcMain.handle('engine:insertMidiBuiltInEffect', (_event, trackId, slotIndex, effectType) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    const result = sounderNative.insertMidiBuiltInEffect(trackId, slotIndex, effectType);
    return result && result.error ? { error: result.error } : { ok: true };
  });
  ipcMain.handle('engine:setInsertEffectParam', (_event, trackId, slotIndex, paramName, value) => {
    if (sounderNative) sounderNative.setInsertEffectParam(trackId, slotIndex, paramName, value);
    return { ok: true };
  });
  ipcMain.handle('engine:getInsertEffectParam', (_event, trackId, slotIndex, paramName) => {
    if (!sounderNative) return 0;
    return sounderNative.getInsertEffectParam(trackId, slotIndex, paramName);
  });
  ipcMain.handle('engine:setMidiInsertEffectParam', (_event, trackId, slotIndex, paramName, value) => {
    if (sounderNative) sounderNative.setMidiInsertEffectParam(trackId, slotIndex, paramName, value);
    return { ok: true };
  });
  ipcMain.handle('engine:getMidiInsertEffectParam', (_event, trackId, slotIndex, paramName) => {
    if (!sounderNative) return 0;
    return sounderNative.getMidiInsertEffectParam(trackId, slotIndex, paramName);
  });

  // ── Insert Chain Info ──
  ipcMain.handle('engine:getInsertChainInfo', (_event, trackId) => {
    if (!sounderNative) return [];
    return sounderNative.getInsertChainInfo(trackId);
  });
  ipcMain.handle('engine:getMidiInsertChainInfo', (_event, trackId) => {
    if (!sounderNative) return [];
    return sounderNative.getMidiInsertChainInfo(trackId);
  });
  ipcMain.handle('engine:removeInsert', (_event, trackId, slotIndex) => {
    if (sounderNative) sounderNative.removeInsert(trackId, slotIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:removeMidiInsert', (_event, trackId, slotIndex) => {
    if (sounderNative) sounderNative.removeMidiInsert(trackId, slotIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:insertMidiPlugin', async (_event, trackId, slotIndex, pluginId) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const result = await sounderNative.insertMidiPlugin(trackId, slotIndex, pluginId);
      return result && result.error ? { error: result.error } : { ok: true };
    } catch (e) {
      return { error: e.message || 'Failed to load plugin' };
    }
  });

  // ── Track Output Routing ──
  ipcMain.handle('engine:setTrackOutput', (_event, trackId, busId) => {
    if (sounderNative) sounderNative.setTrackOutput(trackId, busId);
    return { ok: true };
  });
  ipcMain.handle('engine:getTrackOutput', (_event, trackId) => {
    if (!sounderNative) return -1;
    return sounderNative.getTrackOutput(trackId);
  });
  ipcMain.handle('engine:setMidiTrackOutput', (_event, trackId, busId) => {
    if (sounderNative) sounderNative.setMidiTrackOutput(trackId, busId);
    return { ok: true };
  });
  ipcMain.handle('engine:getMidiTrackOutput', (_event, trackId) => {
    if (!sounderNative) return -1;
    return sounderNative.getMidiTrackOutput(trackId);
  });

  // ── Bus Tracks ──
  ipcMain.handle('engine:addBusTrack', (_event, name) => {
    if (!sounderNative) return { trackId: -1 };
    const trackId = sounderNative.addBusTrack(name);
    return { trackId };
  });
  ipcMain.handle('engine:removeBusTrack', (_event, trackId) => {
    if (sounderNative) sounderNative.removeBusTrack(trackId);
    return { ok: true };
  });
  ipcMain.handle('engine:setBusTrackVolume', (_event, trackId, volume) => {
    if (sounderNative) sounderNative.setBusTrackVolume(trackId, volume);
    return { ok: true };
  });
  ipcMain.handle('engine:setBusTrackPan', (_event, trackId, pan) => {
    if (sounderNative) sounderNative.setBusTrackPan(trackId, pan);
    return { ok: true };
  });
  ipcMain.handle('engine:setBusTrackMute', (_event, trackId, muted) => {
    if (sounderNative) sounderNative.setBusTrackMute(trackId, muted);
    return { ok: true };
  });
  ipcMain.handle('engine:setBusTrackSolo', (_event, trackId, solo) => {
    if (sounderNative) sounderNative.setBusTrackSolo(trackId, solo);
    return { ok: true };
  });
  ipcMain.handle('engine:insertBusBuiltInEffect', (_event, busId, slotIndex, effectType) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    const result = sounderNative.insertBusBuiltInEffect(busId, slotIndex, effectType);
    return result && result.error ? { error: result.error } : { ok: true };
  });
  ipcMain.handle('engine:removeBusInsert', (_event, busId, slotIndex) => {
    if (sounderNative) sounderNative.removeBusInsert(busId, slotIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:getBusInsertChainInfo', (_event, busId) => {
    if (!sounderNative) return [];
    return sounderNative.getBusInsertChainInfo(busId);
  });
  ipcMain.handle('engine:setBusInsertEffectParam', (_event, busId, slotIndex, paramName, value) => {
    if (sounderNative) sounderNative.setBusInsertEffectParam(busId, slotIndex, paramName, value);
    return { ok: true };
  });
  ipcMain.handle('engine:getBusInsertEffectParam', (_event, busId, slotIndex, paramName) => {
    if (!sounderNative) return 0;
    return sounderNative.getBusInsertEffectParam(busId, slotIndex, paramName);
  });
  ipcMain.handle('engine:insertBusPlugin', async (_event, busId, slotIndex, pluginId) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const result = await sounderNative.insertBusPlugin(busId, slotIndex, pluginId);
      return result && result.error ? { error: result.error } : { ok: true };
    } catch (e) {
      return { error: e.message || 'Failed to load plugin' };
    }
  });
  ipcMain.handle('engine:setBusTrackFxParam', (_event, busId, fxType, param, value) => {
    if (sounderNative) sounderNative.setBusTrackFxParam(busId, fxType, param, value);
    return { ok: true };
  });
  ipcMain.handle('engine:setBusTrackFxEnabled', (_event, busId, fxType, enabled) => {
    if (sounderNative) sounderNative.setBusTrackFxEnabled(busId, fxType, enabled);
    return { ok: true };
  });

  // ── Master Inserts ──
  ipcMain.handle('engine:insertMasterBuiltInEffect', (_event, slotIndex, effectType) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    const result = sounderNative.insertMasterBuiltInEffect(slotIndex, effectType);
    return result && result.error ? { error: result.error } : { ok: true };
  });
  ipcMain.handle('engine:removeMasterInsert', (_event, slotIndex) => {
    if (sounderNative) sounderNative.removeMasterInsert(slotIndex);
    return { ok: true };
  });
  ipcMain.handle('engine:getMasterInsertChainInfo', () => {
    if (!sounderNative) return [];
    return sounderNative.getMasterInsertChainInfo();
  });
  ipcMain.handle('engine:setMasterInsertEffectParam', (_event, slotIndex, paramName, value) => {
    if (sounderNative) sounderNative.setMasterInsertEffectParam(slotIndex, paramName, value);
    return { ok: true };
  });
  ipcMain.handle('engine:getMasterInsertEffectParam', (_event, slotIndex, paramName) => {
    if (!sounderNative) return 0;
    return sounderNative.getMasterInsertEffectParam(slotIndex, paramName);
  });
  ipcMain.handle('engine:insertMasterPlugin', async (_event, slotIndex, pluginId) => {
    if (!sounderNative) return { error: 'Native engine not loaded' };
    try {
      const result = await sounderNative.insertMasterPlugin(slotIndex, pluginId);
      return result && result.error ? { error: result.error } : { ok: true };
    } catch (e) {
      return { error: e.message || 'Failed to load plugin' };
    }
  });

  // ── Meter data push (when native addon is built) ──
  if (sounderNative && sounderNative.setMeterCallback) {
    sounderNative.setMeterCallback((meterData) => {
      if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send('engine:meterData', meterData);
        mainWindow.webContents.send('engine:transportUpdate', {
          state: meterData.transportState,
          currentTime: meterData.currentTime,
          currentBeat: meterData.currentBeat
        });
      }
    });
  }
}
