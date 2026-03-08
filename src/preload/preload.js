const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('sounderEngine', {
  // Transport
  play: () => ipcRenderer.invoke('engine:play'),
  stop: () => ipcRenderer.invoke('engine:stop'),
  record: () => ipcRenderer.invoke('engine:record'),
  rewind: () => ipcRenderer.invoke('engine:rewind'),
  seekTo: (time) => ipcRenderer.invoke('engine:seekTo', time),
  getTransportState: () => ipcRenderer.invoke('engine:getTransportState'),

  // Loop
  setLoopEnabled: (enabled) => ipcRenderer.invoke('engine:setLoopEnabled', enabled),
  setLoopRegion: (start, end) => ipcRenderer.invoke('engine:setLoopRegion', start, end),

  // Tracks
  addTrack: (name) => ipcRenderer.invoke('engine:addTrack', name),
  removeTrack: (trackId) => ipcRenderer.invoke('engine:removeTrack', trackId),
  setTrackVolume: (trackId, volume) => ipcRenderer.invoke('engine:setTrackVolume', trackId, volume),
  setTrackPan: (trackId, pan) => ipcRenderer.invoke('engine:setTrackPan', trackId, pan),
  setTrackMute: (trackId, muted) => ipcRenderer.invoke('engine:setTrackMute', trackId, muted),
  setTrackSolo: (trackId, solo) => ipcRenderer.invoke('engine:setTrackSolo', trackId, solo),
  setTrackArmed: (trackId, armed) => ipcRenderer.invoke('engine:setTrackArmed', trackId, armed),
  getTrackWaveform: (trackId, numPoints) => ipcRenderer.invoke('engine:getTrackWaveform', trackId, numPoints),
  getTrackDuration: (trackId) => ipcRenderer.invoke('engine:getTrackDuration', trackId),
  importAudioToTrack: (trackId, filePath) => ipcRenderer.invoke('engine:importAudioToTrack', trackId, filePath),
  separateStems: (trackId, options) => ipcRenderer.invoke('engine:separateStems', trackId, options),
  quantizeAudio: (trackId, options) =>
    ipcRenderer.invoke('engine:quantizeAudio', trackId, options),

  // Audio Region
  setAudioRegion: (trackId, offset, clipStart, clipEnd, loopEnabled) =>
    ipcRenderer.invoke('engine:setAudioRegion', trackId, offset, clipStart, clipEnd, loopEnabled),
  splitAudioTrack: (trackId, splitTime) =>
    ipcRenderer.invoke('engine:splitAudioTrack', trackId, splitTime),
  duplicateAudioTrack: (trackId) =>
    ipcRenderer.invoke('engine:duplicateAudioTrack', trackId),
  setAudioFades: (trackId, fadeIn, fadeOut) =>
    ipcRenderer.invoke('engine:setAudioFades', trackId, fadeIn, fadeOut),
  getRecordingWaveform: (trackId, numPoints) =>
    ipcRenderer.invoke('engine:getRecordingWaveform', trackId, numPoints),

  // MIDI Tracks
  addMidiTrack: (name) => ipcRenderer.invoke('engine:addMidiTrack', name),
  removeMidiTrack: (trackId) => ipcRenderer.invoke('engine:removeMidiTrack', trackId),
  setMidiTrackVolume: (trackId, volume) => ipcRenderer.invoke('engine:setMidiTrackVolume', trackId, volume),
  setMidiTrackPan: (trackId, pan) => ipcRenderer.invoke('engine:setMidiTrackPan', trackId, pan),
  setMidiTrackMute: (trackId, muted) => ipcRenderer.invoke('engine:setMidiTrackMute', trackId, muted),
  setMidiTrackSolo: (trackId, solo) => ipcRenderer.invoke('engine:setMidiTrackSolo', trackId, solo),
  setMidiTrackArmed: (trackId, armed) => ipcRenderer.invoke('engine:setMidiTrackArmed', trackId, armed),
  setMidiTrackInstrument: (trackId, pluginId) =>
    ipcRenderer.invoke('engine:setMidiTrackInstrument', trackId, pluginId),
  setMidiTrackBuiltInInstrument: (trackId, instrumentType) =>
    ipcRenderer.invoke('engine:setMidiTrackBuiltInInstrument', trackId, instrumentType),

  // MIDI Note Editing
  addMidiNote: (trackId, note, startBeat, lengthBeats, velocity) =>
    ipcRenderer.invoke('engine:addMidiNote', trackId, note, startBeat, lengthBeats, velocity),
  removeMidiNote: (trackId, noteIndex) =>
    ipcRenderer.invoke('engine:removeMidiNote', trackId, noteIndex),
  moveMidiNote: (trackId, noteIndex, newNote, newStartBeat) =>
    ipcRenderer.invoke('engine:moveMidiNote', trackId, noteIndex, newNote, newStartBeat),
  resizeMidiNote: (trackId, noteIndex, newLengthBeats) =>
    ipcRenderer.invoke('engine:resizeMidiNote', trackId, noteIndex, newLengthBeats),
  setMidiNoteVelocity: (trackId, noteIndex, velocity) =>
    ipcRenderer.invoke('engine:setMidiNoteVelocity', trackId, noteIndex, velocity),
  getMidiNotes: (trackId) => ipcRenderer.invoke('engine:getMidiNotes', trackId),
  quantizeMidiNotes: (trackId, gridSizeBeats) =>
    ipcRenderer.invoke('engine:quantizeMidiNotes', trackId, gridSizeBeats),
  addMidiCC: (trackId, cc, value, beat) =>
    ipcRenderer.invoke('engine:addMidiCC', trackId, cc, value, beat),

  // AI MIDI Generation
  generateMidi: (config) => ipcRenderer.invoke('engine:generateMidi', config),
  injectMidiNotes: (trackId, notes, clearFirst) =>
    ipcRenderer.invoke('engine:injectMidiNotes', trackId, notes, clearFirst),

  // AI Audio Injection (generation via transformers.js)
  injectAudioBuffer: (trackId, waveform, sampleRate, numChannels) =>
    ipcRenderer.invoke('engine:injectAudioBuffer', trackId, waveform, sampleRate, numChannels),

  // AI Audio Generation (transformers.js, bundled models)
  getAIModels: () => ipcRenderer.invoke('ai:getModels'),
  generateAIAudio: (modelId, prompt, config) =>
    ipcRenderer.invoke('ai:generateAudio', modelId, prompt, config),
  cancelAIGeneration: () => ipcRenderer.invoke('ai:cancelGeneration'),
  onAIGenerateProgress: (callback) => {
    ipcRenderer.on('ai:generateProgress', (_e, data) => callback(data));
  },

  // Built-in Instrument Parameters
  setBuiltInSynthParam: (trackId, param, value) =>
    ipcRenderer.invoke('engine:setBuiltInSynthParam', trackId, param, value),
  getBuiltInSynthParam: (trackId, param) =>
    ipcRenderer.invoke('engine:getBuiltInSynthParam', trackId, param),
  loadSampleToPlayer: (trackId, filePath) =>
    ipcRenderer.invoke('engine:loadSampleToPlayer', trackId, filePath),
  loadDrumPadSample: (trackId, padIndex, filePath) =>
    ipcRenderer.invoke('engine:loadDrumPadSample', trackId, padIndex, filePath),
  getBuiltInInstrumentTypes: () => ipcRenderer.invoke('engine:getBuiltInInstrumentTypes'),

  // SFZ Instruments
  loadSFZPreset: (trackId, presetId) =>
    ipcRenderer.invoke('engine:loadSFZPreset', trackId, presetId),
  getSFZPresets: () => ipcRenderer.invoke('engine:getSFZPresets'),
  loadSFZFile: (trackId, filePath) =>
    ipcRenderer.invoke('engine:loadSFZFile', trackId, filePath),

  // Effects
  setTrackFxParam: (trackId, fxType, param, value) =>
    ipcRenderer.invoke('engine:setTrackFxParam', trackId, fxType, param, value),
  setTrackFxEnabled: (trackId, fxType, enabled) =>
    ipcRenderer.invoke('engine:setTrackFxEnabled', trackId, fxType, enabled),

  // Plugins (VST/VST3/AU)
  scanPlugins: () => ipcRenderer.invoke('engine:scanPlugins'),
  scanPluginDirectory: () => ipcRenderer.invoke('engine:scanPluginDirectory'),
  getPluginList: () => ipcRenderer.invoke('engine:getPluginList'),
  insertPlugin: (trackId, slotIndex, pluginId) =>
    ipcRenderer.invoke('engine:insertPlugin', trackId, slotIndex, pluginId),
  removePlugin: (trackId, slotIndex) =>
    ipcRenderer.invoke('engine:removePlugin', trackId, slotIndex),
  openPluginEditor: (trackId, slotIndex) =>
    ipcRenderer.invoke('engine:openPluginEditor', trackId, slotIndex),
  openMidiPluginEditor: (trackId, slotIndex) =>
    ipcRenderer.invoke('engine:openMidiPluginEditor', trackId, slotIndex),
  openMidiInstrumentEditor: (trackId) =>
    ipcRenderer.invoke('engine:openMidiInstrumentEditor', trackId),
  closePluginEditor: (trackId, slotIndex) =>
    ipcRenderer.invoke('engine:closePluginEditor', trackId, slotIndex),
  getPluginState: (trackId, slotIndex) =>
    ipcRenderer.invoke('engine:getPluginState', trackId, slotIndex),

  // Built-in Effects
  getBuiltInEffectTypes: () => ipcRenderer.invoke('engine:getBuiltInEffectTypes'),
  insertBuiltInEffect: (trackId, slotIndex, effectType) =>
    ipcRenderer.invoke('engine:insertBuiltInEffect', trackId, slotIndex, effectType),
  insertMidiBuiltInEffect: (trackId, slotIndex, effectType) =>
    ipcRenderer.invoke('engine:insertMidiBuiltInEffect', trackId, slotIndex, effectType),
  setInsertEffectParam: (trackId, slotIndex, paramName, value) =>
    ipcRenderer.invoke('engine:setInsertEffectParam', trackId, slotIndex, paramName, value),
  getInsertEffectParam: (trackId, slotIndex, paramName) =>
    ipcRenderer.invoke('engine:getInsertEffectParam', trackId, slotIndex, paramName),
  setMidiInsertEffectParam: (trackId, slotIndex, paramName, value) =>
    ipcRenderer.invoke('engine:setMidiInsertEffectParam', trackId, slotIndex, paramName, value),
  getMidiInsertEffectParam: (trackId, slotIndex, paramName) =>
    ipcRenderer.invoke('engine:getMidiInsertEffectParam', trackId, slotIndex, paramName),

  // Insert Chain Info
  getInsertChainInfo: (trackId) => ipcRenderer.invoke('engine:getInsertChainInfo', trackId),
  getMidiInsertChainInfo: (trackId) => ipcRenderer.invoke('engine:getMidiInsertChainInfo', trackId),
  removeInsert: (trackId, slotIndex) => ipcRenderer.invoke('engine:removeInsert', trackId, slotIndex),
  removeMidiInsert: (trackId, slotIndex) => ipcRenderer.invoke('engine:removeMidiInsert', trackId, slotIndex),
  insertMidiPlugin: (trackId, slotIndex, pluginId) =>
    ipcRenderer.invoke('engine:insertMidiPlugin', trackId, slotIndex, pluginId),

  // Track Output Routing
  setTrackOutput: (trackId, busId) => ipcRenderer.invoke('engine:setTrackOutput', trackId, busId),
  getTrackOutput: (trackId) => ipcRenderer.invoke('engine:getTrackOutput', trackId),
  setMidiTrackOutput: (trackId, busId) => ipcRenderer.invoke('engine:setMidiTrackOutput', trackId, busId),
  getMidiTrackOutput: (trackId) => ipcRenderer.invoke('engine:getMidiTrackOutput', trackId),

  // Bus Tracks
  addBusTrack: (name) => ipcRenderer.invoke('engine:addBusTrack', name),
  removeBusTrack: (trackId) => ipcRenderer.invoke('engine:removeBusTrack', trackId),
  setBusTrackVolume: (trackId, volume) => ipcRenderer.invoke('engine:setBusTrackVolume', trackId, volume),
  setBusTrackPan: (trackId, pan) => ipcRenderer.invoke('engine:setBusTrackPan', trackId, pan),
  setBusTrackMute: (trackId, muted) => ipcRenderer.invoke('engine:setBusTrackMute', trackId, muted),
  setBusTrackSolo: (trackId, solo) => ipcRenderer.invoke('engine:setBusTrackSolo', trackId, solo),
  insertBusBuiltInEffect: (busId, slotIndex, effectType) =>
    ipcRenderer.invoke('engine:insertBusBuiltInEffect', busId, slotIndex, effectType),
  removeBusInsert: (busId, slotIndex) => ipcRenderer.invoke('engine:removeBusInsert', busId, slotIndex),
  getBusInsertChainInfo: (busId) => ipcRenderer.invoke('engine:getBusInsertChainInfo', busId),
  setBusInsertEffectParam: (busId, slotIndex, paramName, value) =>
    ipcRenderer.invoke('engine:setBusInsertEffectParam', busId, slotIndex, paramName, value),
  getBusInsertEffectParam: (busId, slotIndex, paramName) =>
    ipcRenderer.invoke('engine:getBusInsertEffectParam', busId, slotIndex, paramName),
  insertBusPlugin: (busId, slotIndex, pluginId) =>
    ipcRenderer.invoke('engine:insertBusPlugin', busId, slotIndex, pluginId),
  setBusTrackFxParam: (busId, fxType, param, value) =>
    ipcRenderer.invoke('engine:setBusTrackFxParam', busId, fxType, param, value),
  setBusTrackFxEnabled: (busId, fxType, enabled) =>
    ipcRenderer.invoke('engine:setBusTrackFxEnabled', busId, fxType, enabled),

  // Master
  setMasterVolume: (volume) => ipcRenderer.invoke('engine:setMasterVolume', volume),
  insertMasterBuiltInEffect: (slotIndex, effectType) =>
    ipcRenderer.invoke('engine:insertMasterBuiltInEffect', slotIndex, effectType),
  removeMasterInsert: (slotIndex) => ipcRenderer.invoke('engine:removeMasterInsert', slotIndex),
  getMasterInsertChainInfo: () => ipcRenderer.invoke('engine:getMasterInsertChainInfo'),
  setMasterInsertEffectParam: (slotIndex, paramName, value) =>
    ipcRenderer.invoke('engine:setMasterInsertEffectParam', slotIndex, paramName, value),
  getMasterInsertEffectParam: (slotIndex, paramName) =>
    ipcRenderer.invoke('engine:getMasterInsertEffectParam', slotIndex, paramName),
  insertMasterPlugin: (slotIndex, pluginId) =>
    ipcRenderer.invoke('engine:insertMasterPlugin', slotIndex, pluginId),

  // Metronome
  setMetronomeVolume: (volume) => ipcRenderer.invoke('engine:setMetronomeVolume', volume),
  setBPM: (bpm) => ipcRenderer.invoke('engine:setBPM', bpm),
  setTimeSignature: (numerator, denominator) =>
    ipcRenderer.invoke('engine:setTimeSignature', numerator, denominator),

  // Meters (subscribe to pushed data from main)
  onMeterData: (callback) => {
    ipcRenderer.on('engine:meterData', (_event, data) => callback(data));
  },
  onTransportUpdate: (callback) => {
    ipcRenderer.on('engine:transportUpdate', (_event, data) => callback(data));
  },

  // Audio devices
  getAudioDevices: () => ipcRenderer.invoke('engine:getAudioDevices'),
  getInputDevices: () => ipcRenderer.invoke('engine:getInputDevices'),
  getAudioDeviceInfo: () => ipcRenderer.invoke('engine:getAudioDeviceInfo'),
  setAudioDevice: (name, sampleRate, bufferSize) => ipcRenderer.invoke('engine:setAudioDevice', name, sampleRate, bufferSize),
  setAudioDeviceSeparate: (outputName, inputName, sampleRate, bufferSize) =>
    ipcRenderer.invoke('engine:setAudioDeviceSeparate', outputName, inputName, sampleRate, bufferSize),

  // MIDI input
  getMidiDevices: () => ipcRenderer.invoke('engine:getMidiDevices'),
  openMidiDevice: (identifier) => ipcRenderer.invoke('engine:openMidiDevice', identifier),
  closeMidiDevice: (identifier) => ipcRenderer.invoke('engine:closeMidiDevice', identifier),
  setMidiTarget: (trackId) => ipcRenderer.invoke('engine:setMidiTarget', trackId),
  startMidiLearn: (paramPath) => ipcRenderer.invoke('engine:startMidiLearn', paramPath),
  stopMidiLearn: () => ipcRenderer.invoke('engine:stopMidiLearn'),
  getMidiBindings: () => ipcRenderer.invoke('engine:getMidiBindings'),
  removeMidiBinding: (cc, channel) => ipcRenderer.invoke('engine:removeMidiBinding', cc, channel),

  // File I/O
  saveProject: (name) => ipcRenderer.invoke('project:save', name),
  loadProject: (projectId) => ipcRenderer.invoke('project:load', projectId),
  listProjects: () => ipcRenderer.invoke('project:list'),
  deleteProject: (projectId) => ipcRenderer.invoke('project:delete', projectId),
  exportWAV: () => ipcRenderer.invoke('project:exportWAV'),
  exportAIFF: () => ipcRenderer.invoke('project:exportAIFF'),
  exportStems: (options) => ipcRenderer.invoke('project:exportStems', options),
  importMidiFile: (trackId) => ipcRenderer.invoke('midi:importFile', trackId),
  exportMidiFile: (trackId) => ipcRenderer.invoke('midi:exportFile', trackId),
  importAudioFile: () => ipcRenderer.invoke('project:importAudioFile'),

  // Menu events from native menu
  onMenuAction: (callback) => {
    ipcRenderer.on('menu-action', (_event, action) => callback(action));
  }
});
