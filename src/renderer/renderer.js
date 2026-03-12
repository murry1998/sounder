// ═══════════════════════════════════════════════════════════
//  SOUNDER DESKTOP — Renderer (IPC bridge to native engine)
// ═══════════════════════════════════════════════════════════

const engine = window.sounderEngine;
const mixerBoard = new MixerBoard(engine);
const pianoRoll = new PianoRoll(engine);
const undoManager = new UndoManager(100);
pianoRoll.undoManager = undoManager;

// ─── Undo / Redo ─────────────────────────────────────────
async function refreshAudioTrackWaveforms() {
  for (const track of tracks) {
    if (track.type !== 'audio' || !track.hasBuff) continue;
    const wf = await engine.getTrackWaveform(track.id, 4000);
    if (wf && wf.data && wf.data.length > 0) {
      track.waveformData = new Float32Array(wf.data);
      track.duration = wf.duration || track.duration;
      // Update region clip end if it existed
      if (track.regions && track.regions.length > 0) {
        track.regions[0].clipEnd = track.duration;
        track.regions[0].duration = track.duration;
        track.regions[0].waveformData = track.waveformData;
      }
    }
  }
}

async function performUndo() {
  const did = await undoManager.undo();
  if (!did) return;
  // Refresh piano roll if open
  if (pianoRoll.isOpen && pianoRoll.trackId != null) {
    const result = await engine.getMidiNotes(pianoRoll.trackId);
    pianoRoll._syncNotes(result.notes || []);
    pianoRoll._render();
  }
  // Refresh MIDI track previews
  for (const track of tracks) {
    if (track.type === 'midi') {
      await refreshMidiNotes(track);
      renderMidiTrackPreview(track);
    }
  }
  // Refresh audio track waveforms (for audio function undo)
  await refreshAudioTrackWaveforms();
  renderAllTracks();
}

async function performRedo() {
  const did = await undoManager.redo();
  if (!did) return;
  if (pianoRoll.isOpen && pianoRoll.trackId != null) {
    const result = await engine.getMidiNotes(pianoRoll.trackId);
    pianoRoll._syncNotes(result.notes || []);
    pianoRoll._render();
  }
  for (const track of tracks) {
    if (track.type === 'midi') {
      await refreshMidiNotes(track);
      renderMidiTrackPreview(track);
    }
  }
  await refreshAudioTrackWaveforms();
  renderAllTracks();
}

// Sync MIDI timeline preview when piano roll closes or notes change
pianoRoll.onClose = async (trackId) => {
  const track = tracks.find(t => t.id === trackId);
  if (track && track.type === 'midi') {
    await refreshMidiNotes(track);
    renderMidiTrackPreview(track);
  }
};
let _midiPreviewTimer = null;
pianoRoll.onChange = (trackId) => {
  clearTimeout(_midiPreviewTimer);
  _midiPreviewTimer = setTimeout(async () => {
    const track = tracks.find(t => t.id === trackId);
    if (track && track.type === 'midi') {
      await refreshMidiNotes(track);
      renderMidiTrackPreview(track);
    }
  }, 150);
};

const TRACK_COLORS = [
  '#e8913a','#d97706','#22c55e','#06b6d4','#3b82f6',
  '#8b5cf6','#ec4899','#f43f5e','#14b8a6','#eab308',
];


// ─── Track Model (UI-only, audio lives in native engine) ──
let tracks = [];
let busTracks = [];
let trackIdCounter = 0;
let busIdCounter = 0;
let projectName = 'Untitled';

async function createTrack(name) {
  const trackName = name || `Audio ${tracks.filter(t => t.type === 'audio').length + 1}`;
  const result = await engine.addTrack(trackName);
  const id = result.trackId >= 0 ? result.trackId : trackIdCounter;
  const color = TRACK_COLORS[id % TRACK_COLORS.length];

  const track = {
    id,
    type: 'audio',
    name: trackName,
    color,
    volume: 0.8,
    pan: 0,
    muted: false,
    solo: false,
    armed: false,
    hasBuff: false,
    duration: 0,
    waveformData: null,
    peakLevel: 0,
    peakHoldTime: 0,
    clipping: false,
    fx: {
      eqEnabled: false, compEnabled: false, delayEnabled: false,
      eqLowGain: 0, eqMidGain: 0, eqMidFreq: 1000, eqHighGain: 0,
      compThreshold: -24, compRatio: 4, compAttack: 0.003, compRelease: 0.25,
      delayTime: 0, delayMix: 0, delayFeedback: 0.3
    },
    plugins: [],
    outputBus: -1,
    regions: [],
    _hiddenNativeIds: [],
    get region() { return this.regions[0] || null; },
    set region(v) {
      if (v === null) { this.regions = []; }
      else if (this.regions.length === 0) { this.regions.push(v); }
      else { this.regions[0] = v; }
    }
  };
  trackIdCounter = Math.max(trackIdCounter, id + 1);
  tracks.push(track);
  undoManager.push(new CreateTrackCommand(engine, track, tracks, renderAllTracks));
  return track;
}

async function createMidiTrack(name) {
  const trackName = name || `MIDI ${tracks.filter(t => t.type === 'midi').length + 1}`;
  const result = await engine.addMidiTrack(trackName);
  const id = result.trackId >= 0 ? result.trackId : trackIdCounter;
  const color = TRACK_COLORS[id % TRACK_COLORS.length];

  const track = {
    id,
    type: 'midi',
    name: trackName,
    color,
    volume: 0.8,
    pan: 0,
    muted: false,
    solo: false,
    armed: false,
    instrumentName: 'No Instrument',
    instrumentType: null,
    notes: [],
    peakLevel: 0,
    peakHoldTime: 0,
    clipping: false,
    plugins: [],
    outputBus: -1,
    regions: [],
    splitBeats: [],
    get region() { return this.regions[0] || null; },
    set region(v) {
      if (v === null) { this.regions = []; }
      else if (this.regions.length === 0) { this.regions.push(v); }
      else { this.regions[0] = v; }
    }
  };
  trackIdCounter = Math.max(trackIdCounter, id + 1);
  tracks.push(track);
  undoManager.push(new CreateTrackCommand(engine, track, tracks, renderAllTracks));

  // Auto-insert a limiter on slot 4 (last slot) to prevent MIDI clipping
  try {
    await engine.insertMidiBuiltInEffect(track.id, 4, 'limiter');
    await engine.setMidiInsertEffectParam(track.id, 4, 'threshold', -0.3);
    await engine.setMidiInsertEffectParam(track.id, 4, 'release', 50);
    track._hasSystemLimiter = true;
  } catch (e) { /* limiter insertion is best-effort */ }

  return track;
}

async function createBusTrack(name) {
  const busName = name || `Bus ${busTracks.length + 1}`;
  const result = await engine.addBusTrack(busName);
  const id = result.trackId >= 0 ? result.trackId : busIdCounter;
  const bus = {
    id,
    type: 'bus',
    name: busName,
    color: '#06b6d4',
    volume: 0.8,
    pan: 0,
    muted: false,
    solo: false,
    peakLevel: 0,
    peakHoldTime: 0,
    clipping: false,
  };
  busIdCounter = Math.max(busIdCounter, id + 1);
  busTracks.push(bus);
  return bus;
}

async function removeBusTrack(id) {
  const idx = busTracks.findIndex(b => b.id === id);
  if (idx === -1) return;
  await engine.removeBusTrack(id);
  busTracks.splice(idx, 1);
  // Reset any tracks routed to this bus
  for (const track of tracks) {
    if (track.outputBus === id) {
      track.outputBus = -1;
      if (track.type === 'midi') await engine.setMidiTrackOutput(track.id, -1);
      else await engine.setTrackOutput(track.id, -1);
    }
  }
}

async function removeTrack(id, skipUndo) {
  const idx = tracks.findIndex(t => t.id === id);
  if (idx === -1) return;
  const track = tracks[idx];

  // Snapshot for undo (MIDI tracks only)
  let noteSnapshot = null;
  if (!skipUndo && track.type === 'midi') {
    const notesResult = await engine.getMidiNotes(id);
    noteSnapshot = (notesResult.notes || []).map(n => ({
      noteNumber: n.noteNumber, startBeat: n.startBeat,
      lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
    }));
  }
  const trackSnapshot = JSON.parse(JSON.stringify({
    id: track.id, type: track.type, name: track.name, color: track.color,
    volume: track.volume, pan: track.pan, muted: track.muted, solo: track.solo,
    instrumentName: track.instrumentName, instrumentType: track.instrumentType,
    sfzPresetId: track.sfzPresetId, outputBus: track.outputBus
  }));

  if (track.type === 'midi') {
    await engine.removeMidiTrack(id);
  } else {
    // Remove all hidden native sub-tracks first
    for (const nId of (track._hiddenNativeIds || [])) {
      try { await engine.removeTrack(nId); } catch(e) {}
    }
    await engine.removeTrack(id);
  }
  tracks.splice(idx, 1);

  if (!skipUndo) {
    undoManager.push(new RemoveTrackCommand(
      engine, trackSnapshot, noteSnapshot, idx, tracks, renderAllTracks
    ));
  }
}


// ─── Transport State ──────────────────────────────────
let isPlaying = false;
let isRecording = false;
let currentTime = 0;
let loopEnabled = false;
let loopStart = 0;
let loopEnd = 10;
let zoomLevel = 40;
let scrollOffset = 0;      // seconds into the timeline
let followPlayhead = false;

// ─── Selection & Clipboard ────────────────────────────
let clipboard = null;
let selectedTrack = null;
let selectedRegionIndex = 0;
let allRegionsSelected = false;

function highlightAllRegions(track) {
  // Remove existing region selection highlights
  document.querySelectorAll('.audio-region.selected, .midi-region.selected').forEach(el => el.classList.remove('selected'));
  // Find the track row and highlight all regions in it
  const trackRow = document.querySelector(`.track-row[data-track-id="${track.id}"]`);
  if (trackRow) {
    trackRow.querySelectorAll('.audio-region, .midi-region').forEach(el => el.classList.add('selected'));
  }
}

// ─── Sub-Track Sync (propagate params to all native sub-tracks) ──
async function syncSubTrackParams(track) {
  if (track.type !== 'audio') return;
  const allNativeIds = [track.id, ...(track._hiddenNativeIds || [])];
  for (const nId of allNativeIds) {
    await engine.setTrackVolume(nId, track.volume);
    await engine.setTrackPan(nId, track.pan);
    await engine.setTrackMute(nId, track.muted);
    await engine.setTrackSolo(nId, track.solo);
  }
}

// ─── Waveform Resolution ──────────────────────────────
function getWaveformResolution(track) {
  const dpr = window.devicePixelRatio || 1;
  const numPoints = Math.ceil((track.duration || 1) * zoomLevel * dpr);
  return Math.min(Math.max(numPoints, 1000), 20000);
}

let _zoomWaveformTimer = null;
function scheduleWaveformRefetch() {
  clearTimeout(_zoomWaveformTimer);
  _zoomWaveformTimer = setTimeout(async () => {
    for (const t of tracks) {
      if (t.type === 'audio' && t.hasBuff) {
        const numPoints = getWaveformResolution(t);
        for (const r of (t.regions || [])) {
          const nId = r.nativeTrackId || t.id;
          const wf = await engine.getTrackWaveform(nId, numPoints);
          if (wf && wf.data) {
            r.waveformData = new Float32Array(wf.data);
            r.duration = wf.duration || r.duration;
          }
        }
        // Also update track-level for backward compat
        if (t.regions.length > 0) {
          t.waveformData = t.regions[0].waveformData;
        }
        renderTrackWaveform(t);
      }
    }
  }, 200);
}

// ─── Snap System ──────────────────────────────────────
let snapMode = '1/4';

function getSnapGridSec() {
  const bpm = parseInt(document.getElementById('bpm-input').value) || 120;
  const beatSec = 60 / bpm;
  if (snapMode === '1/4') return beatSec;
  if (snapMode === '1/8') return beatSec / 2;
  if (snapMode === '1/16') return beatSec / 4;
  return 0; // free
}

function snapTime(t) {
  const grid = getSnapGridSec();
  if (grid <= 0) return t;
  return Math.round(t / grid) * grid;
}
function snapTimeCeil(t) {
  const grid = getSnapGridSec();
  if (grid <= 0) return t;
  return Math.ceil(t / grid) * grid;
}
function snapTimeFloor(t) {
  const grid = getSnapGridSec();
  if (grid <= 0) return t;
  return Math.floor(t / grid) * grid;
}

// Live recording waveform poll
let _recWaveformTimer = null;

// ─── Transport Controls ───────────────────────────────
async function play() {
  if (isPlaying) return;
  await engine.play();
  isPlaying = true;
  updateTransportUI();
  setStatus('Playing');
}

async function stop() {
  const wasRecording = isRecording;
  await engine.stop();
  isPlaying = false;
  isRecording = false;
  clearInterval(_recWaveformTimer);
  updateTransportUI();
  setStatus('Stopped');

  // After recording, refresh waveforms/notes for armed tracks
  if (wasRecording) {
    for (const track of tracks) {
      if (!track.armed) continue;
      if (track.type === 'audio') {
        const result = await engine.getTrackWaveform(track.id, 4000);
        if (result && result.data && result.data.length > 0) {
          track.hasBuff = true;
          track.waveformData = new Float32Array(result.data);
          track.duration = result.duration || 0;
          // Set up region for newly recorded audio
          track.region = {
            nativeTrackId: track.id,
            offset: 0,
            clipStart: 0,
            clipEnd: track.duration,
            loopEnabled: false,
            loopCount: 0,
            fadeIn: 0, fadeOut: 0,
            waveformData: track.waveformData,
            duration: track.duration
          };
          await syncAudioRegionToNative(track);
        }
      } else if (track.type === 'midi') {
        // Refresh notes from engine after MIDI recording
        await refreshMidiNotes(track);
        computeMidiRegion(track);
        renderMidiTrackPreview(track);
      }
    }
    renderAllTracks();
  }
}

async function rewind() {
  const wasPlaying = isPlaying;
  if (isPlaying) await stop();
  await engine.rewind();
  currentTime = loopEnabled ? loopStart : 0;
  updateTimeDisplay();
  updatePlayhead();
  if (wasPlaying) await play();
  setStatus('Rewound');
}

async function toggleRecord() {
  if (isRecording) {
    await stop();
  } else {
    const armedTracks = tracks.filter(t => t.armed);
    if (armedTracks.length === 0) {
      setStatus('Arm at least one track before recording.');
      return;
    }
    const result = await engine.record();
    if (result && result.error) {
      setStatus(result.error);
      return;
    }
    isRecording = true;
    isPlaying = true;
    updateTransportUI();
    setStatus('Recording...');

    // Start live recording waveform polling
    clearInterval(_recWaveformTimer);
    _recWaveformTimer = setInterval(async () => {
      if (!isRecording) { clearInterval(_recWaveformTimer); return; }
      for (const track of tracks) {
        if (!track.armed || track.type !== 'audio') continue;
        try {
          const wf = await engine.getRecordingWaveform(track.id, 4000);
          if (wf && wf.data && wf.data.length > 0 && wf.duration > 0) {
            track.waveformData = new Float32Array(wf.data);
            track.duration = wf.duration;
            track.hasBuff = true;
            // Render directly on canvas (no region div during recording)
            const row = document.querySelector(`.track-row[data-id="${track.id}"]`);
            if (row) {
              const canvas = row.querySelector('.waveform-canvas');
              if (canvas) {
                const rect = canvas.parentElement.getBoundingClientRect();
                const dpr = window.devicePixelRatio || 1;
                canvas.width = rect.width * dpr;
                canvas.height = rect.height * dpr;
                const c = canvas.getContext('2d');
                c.scale(dpr, dpr);
                c.clearRect(0, 0, rect.width, rect.height);
                const pxPerSec = zoomLevel;
                const totalWidth = wf.duration * pxPerSec;
                const data = track.waveformData;
                const grad = c.createLinearGradient(0, 0, 0, rect.height);
                grad.addColorStop(0, hexToRGBA(track.color, 0.6));
                grad.addColorStop(0.5, hexToRGBA(track.color, 0.85));
                grad.addColorStop(1, hexToRGBA(track.color, 0.6));
                c.fillStyle = grad;
                const samplesPerPx = data.length / Math.max(1, totalWidth);
                const offsetPx = scrollOffset * pxPerSec;
                const xEnd = Math.min(Math.ceil(totalWidth - offsetPx), rect.width);
                for (let x = Math.max(0, -offsetPx); x < xEnd; x++) {
                  const sIdx = Math.floor((x + offsetPx) * samplesPerPx);
                  const val = data[Math.min(sIdx, data.length - 1)] || 0;
                  const barH = val * rect.height * 0.85;
                  c.fillRect(x, (rect.height - barH) / 2, 1, barH);
                }
              }
            }
          }
        } catch (_) {}
      }
    }, 250);
  }
}

async function togglePlay() {
  if (isPlaying || isRecording) await stop();
  else await play();
}

async function toggleLoop() {
  loopEnabled = !loopEnabled;
  await engine.setLoopEnabled(loopEnabled);
  if (loopEnabled) await engine.setLoopRegion(loopStart, loopEnd);
  document.getElementById('btn-loop').classList.toggle('loop-active', loopEnabled);
  document.getElementById('loop-overlay').style.display = loopEnabled ? 'block' : 'none';
  updateLoopOverlay();
  setStatus(loopEnabled ? 'Loop enabled' : 'Loop disabled');
}

async function seekTo(time) {
  const wasPlaying = isPlaying;
  if (isPlaying) { await engine.stop(); isPlaying = false; }
  currentTime = Math.max(0, snapTime(time));
  await engine.seekTo(currentTime);
  updateTimeDisplay();
  updatePlayhead();
  if (wasPlaying) await play();
}


// ─── Waveform Rendering ───────────────────────────────
function renderTrackWaveform(track) {
  const row = document.querySelector(`.track-row[data-id="${track.id}"]`);
  if (!row) return;
  const waveformEl = row.querySelector('.track-waveform');
  const canvas = row.querySelector('.waveform-canvas');
  if (!canvas) return;

  const rect = canvas.parentElement.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  const c = canvas.getContext('2d');
  c.scale(dpr, dpr);
  const w = rect.width;
  const h = rect.height;
  c.clearRect(0, 0, w, h);

  // Remove old audio region elements
  if (waveformEl) {
    waveformEl.querySelectorAll('.audio-region, .audio-region-loop-ghost, .audio-region-ctx-menu, .region-loop-handle-end').forEach(el => el.remove());
  }

  if (!track.hasBuff || !track.waveformData) return;

  // Ensure region model exists
  computeAudioRegion(track);
  if (track.regions.length === 0) return;

  const pxPerSec = zoomLevel;
  const rH = (waveformEl ? waveformEl.offsetHeight : h) - 4;

  // Iterate all regions
  for (let ri = 0; ri < track.regions.length; ri++) {
    const r = track.regions[ri];
    const clipDuration = r.clipEnd - r.clipStart;
    if (clipDuration <= 0) continue;

    // Main region div
    const regionX = (r.offset - scrollOffset) * pxPerSec;
    const regionW = clipDuration * pxPerSec;

    const regionEl = document.createElement('div');
    regionEl.className = 'audio-region';
    regionEl.dataset.regionIndex = ri;
    regionEl.innerHTML = '<canvas class="region-waveform-canvas"></canvas>'
      + '<div class="region-edge-left"></div>'
      + '<div class="region-edge-right"></div>'
      + '<div class="region-loop-handle" title="Drag to loop"></div>';
    regionEl.style.left = regionX + 'px';
    regionEl.style.width = Math.max(4, regionW) + 'px';
    regionEl.style.background = hexToRGBA(track.color, 0.08);
    regionEl.style.borderColor = hexToRGBA(track.color, 0.55);

    if (waveformEl) waveformEl.appendChild(regionEl);

    // Draw waveform on region canvas
    const rCanvas = regionEl.querySelector('.region-waveform-canvas');
    drawAudioWaveformOnCanvas(rCanvas, track, r, r.clipStart, r.clipEnd,
      Math.max(4, regionW), rH, dpr);

    // Bind drag handlers (pass regionIndex)
    bindAudioRegionMoveDrag(regionEl, track, waveformEl, pxPerSec, ri);
    bindAudioRegionEdgeDrag(regionEl.querySelector('.region-edge-left'), track, waveformEl, 'left', pxPerSec, ri);
    bindAudioRegionEdgeDrag(regionEl.querySelector('.region-edge-right'), track, waveformEl, 'right', pxPerSec, ri);
    bindAudioLoopHandleDrag(regionEl.querySelector('.region-loop-handle'), track, waveformEl, pxPerSec, ri);

    // Context menu
    regionEl.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      e.stopPropagation();
      showAudioRegionContextMenu(e, track, waveformEl, ri);
    });

    // Render loop ghosts for this region
    const loopCount = r.loopCount || 0;
    if (loopCount > 0 && waveformEl) {
      for (let i = 1; i <= loopCount; i++) {
        const ghostOffset = r.offset + clipDuration * i;
        const ghostX = (ghostOffset - scrollOffset) * pxPerSec;
        const ghost = document.createElement('div');
        ghost.className = 'audio-region-loop-ghost';
        ghost.style.left = ghostX + 'px';
        ghost.style.width = Math.max(4, regionW) + 'px';
        ghost.style.background = hexToRGBA(track.color, 0.04);
        ghost.style.borderColor = hexToRGBA(track.color, 0.3);
        const gCanvas = document.createElement('canvas');
        gCanvas.className = 'region-waveform-canvas';
        ghost.appendChild(gCanvas);
        waveformEl.appendChild(ghost);
        drawAudioWaveformOnCanvas(gCanvas, track, r, r.clipStart, r.clipEnd,
          Math.max(4, regionW), rH, dpr);
      }
      // Standalone loop handle at end of looped content
      const totalEnd = r.offset + clipDuration * (loopCount + 1);
      const handleEndEl = document.createElement('div');
      handleEndEl.className = 'region-loop-handle-end';
      handleEndEl.title = 'Drag to extend loop';
      handleEndEl.style.left = ((totalEnd - scrollOffset) * pxPerSec - 14) + 'px';
      waveformEl.appendChild(handleEndEl);
      bindAudioLoopHandleDrag(handleEndEl, track, waveformEl, pxPerSec, ri);
    }
  }

  // Loop region overlay on background canvas
  if (loopEnabled) {
    const ls = (loopStart - scrollOffset) * pxPerSec;
    const le = (loopEnd - scrollOffset) * pxPerSec;
    c.fillStyle = 'rgba(6,182,212,0.06)';
    c.fillRect(ls, 0, le - ls, h);
  }
}

function drawMidiNotesOnCanvas(canvas, notes, regionStartBeat, regionW, rH, pxPerBeat, color, dpr) {
  canvas.width = regionW * dpr;
  canvas.height = rH * dpr;
  const rc = canvas.getContext('2d');
  rc.scale(dpr, dpr);
  rc.clearRect(0, 0, regionW, rH);
  if (!notes.length) return;

  let minNote = 127, maxNote = 0;
  for (const n of notes) {
    if (n.note < minNote) minNote = n.note;
    if (n.note > maxNote) maxNote = n.note;
  }
  minNote = Math.max(0, minNote - 2);
  maxNote = Math.min(127, maxNote + 2);
  const noteRange = Math.max(maxNote - minNote, 12);
  const pad = 4;
  const drawH = rH - pad * 2;
  const noteH = Math.max(2, drawH / noteRange);

  for (const n of notes) {
    const x = (n.startBeat - regionStartBeat) * pxPerBeat;
    const nw = Math.max(2, n.lengthBeats * pxPerBeat);
    const y = pad + drawH - ((n.note - minNote) / noteRange) * drawH - noteH;
    const alpha = 0.5 + (n.velocity / 127) * 0.5;
    rc.fillStyle = hexToRGBA(color, alpha);
    rc.fillRect(x, y, nw, noteH);
  }
}

function renderMidiTrackPreview(track) {
  const row = document.querySelector(`.track-row[data-id="${track.id}"]`);
  if (!row) return;
  const waveformEl = row.querySelector('.track-waveform');
  const canvas = row.querySelector('.waveform-canvas');
  if (!canvas) return;

  const rect = canvas.parentElement.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  const c = canvas.getContext('2d');
  c.scale(dpr, dpr);
  c.clearRect(0, 0, rect.width, rect.height);

  const notes = track.notes || [];
  if (!track._edgeDragging) computeMidiRegion(track);

  // Remove old region elements, ghosts, and standalone loop handles
  waveformEl.querySelectorAll('.midi-region, .midi-region-loop-ghost, .region-loop-handle-end').forEach(el => el.remove());

  if (notes.length === 0) {
    c.fillStyle = hexToRGBA(track.color, 0.15);
    c.font = '11px -apple-system, Helvetica Neue, sans-serif';
    c.textAlign = 'center';
    c.fillText('Double-click to edit MIDI', rect.width / 2, rect.height / 2 + 4);
    return;
  }

  if (track.regions.length === 0) return;

  const bpm = parseInt(document.getElementById('bpm-input').value) || 120;
  const pxPerBeat = zoomLevel * (60 / bpm);
  const rH = waveformEl.offsetHeight - 4;
  const scrollPxMidi = scrollOffset * zoomLevel;

  // Iterate all regions
  for (let ri = 0; ri < track.regions.length; ri++) {
    const region = track.regions[ri];
    const regionX = region.startBeat * pxPerBeat;
    const regionW = Math.max(20, (region.endBeat - region.startBeat) * pxPerBeat);
    const contentLen = region.contentEnd - region.contentStart;

    // Filter notes that belong to this region
    const regionNotes = notes.filter(n => {
      const nEnd = n.startBeat + n.lengthBeats;
      return n.startBeat < region.endBeat && nEnd > region.startBeat;
    });

    // Create region element
    const regionEl = document.createElement('div');
    regionEl.className = 'midi-region';
    regionEl.dataset.regionIndex = ri;
    regionEl.innerHTML = '<canvas class="region-notes-canvas"></canvas>'
      + '<div class="region-edge-left"></div>'
      + '<div class="region-edge-right"></div>'
      + '<div class="region-loop-handle" title="Drag to loop"></div>';
    waveformEl.appendChild(regionEl);

    regionEl.style.left = (regionX - scrollPxMidi) + 'px';
    regionEl.style.width = regionW + 'px';
    regionEl.style.background = hexToRGBA(track.color, 0.18);
    regionEl.style.borderColor = hexToRGBA(track.color, 0.5);

    // Draw notes on region canvas
    const rCanvas = regionEl.querySelector('.region-notes-canvas');
    drawMidiNotesOnCanvas(rCanvas, regionNotes, region.startBeat, regionW, rH, pxPerBeat, track.color, dpr);

    // Event: double-click opens piano roll
    regionEl.addEventListener('dblclick', (e) => {
      e.stopPropagation();
      pianoRoll.open(track.id, track.name);
    });
    // Event: mousedown to drag-move region (click without drag seeks)
    bindRegionMoveDrag(regionEl, track, waveformEl, pxPerBeat, ri);
    // Event: right-click context menu
    regionEl.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      e.stopPropagation();
      showMidiRegionContextMenu(e, track, waveformEl, ri);
    });

    // Edge drags
    bindRegionEdgeDrag(regionEl.querySelector('.region-edge-left'), track, waveformEl, 'left', pxPerBeat, ri);
    bindRegionEdgeDrag(regionEl.querySelector('.region-edge-right'), track, waveformEl, 'right', pxPerBeat, ri);
    // Loop handle drag
    bindLoopHandleDrag(regionEl.querySelector('.region-loop-handle'), track, waveformEl, pxPerBeat, ri);

    // Render loop ghosts for this region
    const loopCount = region.loopCount || 0;
    if (loopCount > 0) {
      for (let i = 1; i <= loopCount; i++) {
        const ghostStart = region.endBeat + (i - 1) * contentLen;
        const ghostX = ghostStart * pxPerBeat;
        const ghostW = contentLen * pxPerBeat;
        const ghost = document.createElement('div');
        ghost.className = 'midi-region-loop-ghost';
        ghost.style.left = (ghostX - scrollPxMidi) + 'px';
        ghost.style.width = ghostW + 'px';
        ghost.style.background = hexToRGBA(track.color, 0.10);
        ghost.style.borderColor = hexToRGBA(track.color, 0.3);
        const gCanvas = document.createElement('canvas');
        gCanvas.className = 'region-notes-canvas';
        ghost.appendChild(gCanvas);
        waveformEl.appendChild(ghost);
        drawMidiNotesOnCanvas(gCanvas, regionNotes, region.contentStart, ghostW, rH, pxPerBeat, track.color, dpr);
      }

      // Standalone loop handle at the far right of the last ghost
      const totalEndBeat = region.endBeat + loopCount * contentLen;
      const handleEl = document.createElement('div');
      handleEl.className = 'region-loop-handle-end';
      handleEl.title = 'Drag to extend loop';
      handleEl.style.left = ((totalEndBeat * pxPerBeat) - scrollPxMidi - 14) + 'px';
      waveformEl.appendChild(handleEl);
      bindLoopHandleDrag(handleEl, track, waveformEl, pxPerBeat, ri);
    }
  }
}

async function refreshMidiNotes(track) {
  const result = await engine.getMidiNotes(track.id);
  track.notes = (result.notes || []).map(n => ({
    note: n.noteNumber != null ? n.noteNumber : n.note,
    startBeat: n.startBeat,
    lengthBeats: n.lengthBeats,
    velocity: n.velocity,
    channel: n.channel || 1
  }));
  computeMidiRegion(track);
}

function computeMidiRegion(track) {
  computeMidiRegions(track);
}

function computeMidiRegions(track) {
  const notes = track.notes || [];
  if (notes.length === 0) { track.regions = []; return; }

  const splitBeats = (track.splitBeats || []).slice().sort((a, b) => a - b);

  if (splitBeats.length === 0) {
    // No splits — single region (preserve existing behavior)
    let minBeat = Infinity, maxBeat = 0;
    for (const n of notes) {
      if (n.startBeat < minBeat) minBeat = n.startBeat;
      const end = n.startBeat + n.lengthBeats;
      if (end > maxBeat) maxBeat = end;
    }
    const contentEnd = Math.ceil(maxBeat);
    const contentStart = Math.floor(minBeat);
    const prev = track.regions[0] || {};
    track.regions = [{
      contentStart,
      contentEnd,
      startBeat: prev.startBeat != null ? Math.min(prev.startBeat, contentStart) : contentStart,
      endBeat: prev.endBeat != null ? Math.max(prev.endBeat, contentEnd) : contentEnd,
      loopCount: prev.loopCount || 0
    }];
    return;
  }

  // Build region boundaries from split beats
  const allNoteStart = Math.floor(Math.min(...notes.map(n => n.startBeat)));
  const allNoteEnd = Math.ceil(Math.max(...notes.map(n => n.startBeat + n.lengthBeats)));
  const boundaries = [allNoteStart, ...splitBeats.filter(b => b > allNoteStart && b < allNoteEnd), allNoteEnd];

  const newRegions = [];
  for (let i = 0; i < boundaries.length - 1; i++) {
    const rStart = boundaries[i];
    const rEnd = boundaries[i + 1];
    const regionNotes = notes.filter(n => {
      const nEnd = n.startBeat + n.lengthBeats;
      return n.startBeat < rEnd && nEnd > rStart;
    });
    if (regionNotes.length > 0) {
      const cStart = Math.floor(Math.min(...regionNotes.map(n => n.startBeat)));
      const cEnd = Math.ceil(Math.max(...regionNotes.map(n => n.startBeat + n.lengthBeats)));
      newRegions.push({
        contentStart: Math.max(cStart, rStart),
        contentEnd: Math.min(cEnd, rEnd),
        startBeat: rStart,
        endBeat: rEnd,
        loopCount: 0
      });
    }
  }
  track.regions = newRegions;
}

function snapToQuarter(beat) {
  return Math.round(beat * 4) / 4;
}

function snapToQuarterCeil(beat) {
  return Math.ceil(beat * 4) / 4;
}

// ─── Audio Region Model ──────────────────────────────
function computeAudioRegion(track) {
  if (!track.hasBuff || !track.duration) return;
  if (track.regions.length === 0) {
    track.regions.push({
      nativeTrackId: track.id,
      offset: 0,
      clipStart: 0,
      clipEnd: track.duration,
      loopEnabled: false,
      loopCount: 0,
      fadeIn: 0,
      fadeOut: 0,
      waveformData: track.waveformData,
      duration: track.duration
    });
  }
  // Ensure clipEnd doesn't exceed duration for first region
  const r0 = track.regions[0];
  if (r0.clipEnd > (r0.duration || track.duration))
    r0.clipEnd = r0.duration || track.duration;
}

async function syncAudioRegionToNative(track) {
  for (const r of (track.regions || [])) {
    const nId = r.nativeTrackId || track.id;
    await engine.setAudioRegion(nId, r.offset, r.clipStart, r.clipEnd, r.loopEnabled, r.loopCount || 0);
    await engine.setAudioFades(nId, r.fadeIn || 0, r.fadeOut || 0);
  }
}

function drawAudioWaveformOnCanvas(canvas, track, region, clipStart, clipEnd, width, height, dpr) {
  canvas.width = width * dpr;
  canvas.height = height * dpr;
  const c = canvas.getContext('2d');
  c.scale(dpr, dpr);
  c.clearRect(0, 0, width, height);

  // Use per-region waveform data if available, else fall back to track
  const data = (region && region.waveformData) || track.waveformData;
  if (!data || data.length === 0) return;

  const totalDuration = (region && region.duration) || track.duration || 1;
  const samplesPerSec = data.length / totalDuration;
  const startIdx = Math.floor(clipStart * samplesPerSec);
  const endIdx = Math.ceil(clipEnd * samplesPerSec);
  const clipSamples = endIdx - startIdx;
  if (clipSamples <= 0) return;

  const grad = c.createLinearGradient(0, 0, 0, height);
  grad.addColorStop(0, hexToRGBA(track.color, 0.6));
  grad.addColorStop(0.5, hexToRGBA(track.color, 0.85));
  grad.addColorStop(1, hexToRGBA(track.color, 0.6));
  c.fillStyle = grad;

  for (let x = 0; x < width; x++) {
    const sIdx = startIdx + Math.floor((x / width) * clipSamples);
    const val = data[Math.min(sIdx, data.length - 1)] || 0;
    const barH = val * height * 0.85;
    c.fillRect(x, (height - barH) / 2, 1, barH);
  }

  // Draw fade overlays using the passed region
  const r = region;
  if (r) {
    const clipDur = clipEnd - clipStart;
    if (r.fadeIn > 0 && clipDur > 0) {
      const fadeW = (r.fadeIn / clipDur) * width;
      const fadeGrad = c.createLinearGradient(0, 0, fadeW, 0);
      fadeGrad.addColorStop(0, 'rgba(0,0,0,0.6)');
      fadeGrad.addColorStop(1, 'rgba(0,0,0,0)');
      c.fillStyle = fadeGrad;
      c.fillRect(0, 0, fadeW, height);
    }
    if (r.fadeOut > 0 && clipDur > 0) {
      const fadeW = (r.fadeOut / clipDur) * width;
      const fadeGrad = c.createLinearGradient(width - fadeW, 0, width, 0);
      fadeGrad.addColorStop(0, 'rgba(0,0,0,0)');
      fadeGrad.addColorStop(1, 'rgba(0,0,0,0.6)');
      c.fillStyle = fadeGrad;
      c.fillRect(width - fadeW, 0, fadeW, height);
    }
  }
}

// ─── Audio Region Drag Handlers ──────────────────────
function bindAudioRegionMoveDrag(regionEl, track, waveformEl, pxPerSec, regionIndex) {
  regionEl.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    if (e.target.closest('.region-edge-left, .region-edge-right, .region-loop-handle')) return;
    e.stopPropagation();
    e.preventDefault();
    const r = track.regions[regionIndex];
    if (!r) return;
    const startX = e.clientX;
    const startY = e.clientY;
    const origOffset = r.offset;
    let didDrag = false;
    let verticalDrag = false;
    let dropTargetTrack = null;

    const onMove = (ev) => {
      const dx = ev.clientX - startX;
      const dy = ev.clientY - startY;
      if (Math.abs(dx) < 3 && Math.abs(dy) < 3 && !didDrag) return;
      didDrag = true;

      // Detect vertical drag for track-to-track move
      if (Math.abs(dy) > 20) {
        verticalDrag = true;
        // Find target audio track under cursor
        document.querySelectorAll('.drop-zone-indicator').forEach(el => el.remove());
        dropTargetTrack = null;
        const rows = document.querySelectorAll('.track-row');
        for (const row of rows) {
          const rect = row.getBoundingClientRect();
          if (ev.clientY >= rect.top && ev.clientY <= rect.bottom) {
            const tId = parseInt(row.dataset.id);
            const t = tracks.find(tr => tr.id === tId);
            if (t && t.type === 'audio' && t !== track) {
              dropTargetTrack = t;
              const indicator = document.createElement('div');
              indicator.className = 'drop-zone-indicator';
              row.querySelector('.track-waveform').appendChild(indicator);
            }
            break;
          }
        }
      }

      // Still do horizontal offset move
      const rr = track.regions[regionIndex];
      if (!rr) return;
      const dSec = dx / pxPerSec;
      rr.offset = Math.max(0, snapTime(origOffset + dSec));
      renderTrackWaveform(track);
    };
    const onUp = async () => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      document.querySelectorAll('.drop-zone-indicator').forEach(el => el.remove());

      if (!didDrag) {
        // Select this track/region
        selectedTrack = track;
        selectedRegionIndex = regionIndex;
        allRegionsSelected = false;
        document.querySelectorAll('.audio-region.selected, .midi-region.selected').forEach(el => el.classList.remove('selected'));
        document.querySelectorAll('.track-row.selected').forEach(el => el.classList.remove('selected'));
        regionEl.classList.add('selected');
        const row = regionEl.closest('.track-row');
        if (row) row.classList.add('selected');
        const rRect = waveformEl.getBoundingClientRect();
        seekTo(scrollOffset + (e.clientX - rRect.left) / zoomLevel);
      } else if (verticalDrag && dropTargetTrack) {
        // Move region to target track
        await moveAudioRegionToTrack(track, regionIndex, dropTargetTrack);
      } else {
        await syncAudioRegionToNative(track);
      }
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
}

async function moveAudioRegionToTrack(sourceTrack, regionIndex, targetTrack) {
  const region = sourceTrack.regions[regionIndex];
  if (!region) return;

  // Remove region from source track
  sourceTrack.regions.splice(regionIndex, 1);
  const nId = region.nativeTrackId || sourceTrack.id;

  // Move nativeTrackId from source hidden list to target hidden list
  const hiddenIdx = sourceTrack._hiddenNativeIds.indexOf(nId);
  if (hiddenIdx >= 0) sourceTrack._hiddenNativeIds.splice(hiddenIdx, 1);
  if (nId !== targetTrack.id) {
    targetTrack._hiddenNativeIds.push(nId);
  }

  // Add region to target track
  targetTrack.regions.push(region);
  if (!targetTrack.hasBuff && region.waveformData) {
    targetTrack.hasBuff = true;
    targetTrack.waveformData = region.waveformData;
    targetTrack.duration = region.duration || 0;
  }

  // Sync params on both tracks
  await syncSubTrackParams(sourceTrack);
  await syncSubTrackParams(targetTrack);
  await syncAudioRegionToNative(sourceTrack);
  await syncAudioRegionToNative(targetTrack);

  // If source track has no regions left
  if (sourceTrack.regions.length === 0) {
    sourceTrack.waveformData = null;
    sourceTrack.hasBuff = false;
    sourceTrack.duration = 0;
  }

  renderAllTracks();
  setStatus(`Moved region to ${targetTrack.name}`);
}

function bindAudioRegionEdgeDrag(handleEl, track, waveformEl, side, pxPerSec, regionIndex) {
  handleEl.addEventListener('mousedown', (e) => {
    e.stopPropagation();
    e.preventDefault();
    const r = track.regions[regionIndex];
    if (!r) return;
    const startX = e.clientX;
    const origOffset = r.offset;
    const origClipStart = r.clipStart;
    const origClipEnd = r.clipEnd;

    const onMove = (ev) => {
      const rr = track.regions[regionIndex];
      if (!rr) return;
      const dx = ev.clientX - startX;
      const dSec = dx / pxPerSec;
      if (side === 'left') {
        let newClipStart = snapTime(origClipStart + dSec);
        if (newClipStart < 0) newClipStart = 0;
        if (newClipStart >= rr.clipEnd) return;
        const delta = newClipStart - origClipStart;
        rr.clipStart = newClipStart;
        rr.offset = origOffset + delta;
      } else {
        let newClipEnd = snapTime(origClipEnd + dSec);
        if (newClipEnd <= rr.clipStart) return;
        // Allow extending beyond buffer (silence fills)
        rr.clipEnd = newClipEnd;
      }
      renderTrackWaveform(track);
    };
    const onUp = async () => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      await syncAudioRegionToNative(track);
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
}

function bindAudioLoopHandleDrag(handleEl, track, waveformEl, pxPerSec, regionIndex) {
  handleEl.addEventListener('mousedown', (e) => {
    e.stopPropagation();
    e.preventDefault();
    const r = track.regions[regionIndex];
    if (!r) return;
    const startX = e.clientX;
    const clipDuration = r.clipEnd - r.clipStart;
    if (clipDuration <= 0) return;
    const origLoopCount = r.loopCount || 0;
    const origScrollOffset = scrollOffset;
    let scrollDelta = 0;
    let autoScrollRAF = null;
    let lastMouseX = e.clientX;

    const doAutoScroll = () => {
      const rr = track.regions[regionIndex];
      if (!rr) return;
      const waveRect = waveformEl.getBoundingClientRect();
      const edgeZone = 60;
      const rightDist = waveRect.right - lastMouseX;
      const leftDist = lastMouseX - waveRect.left;
      let speed = 0;
      if (rightDist < edgeZone && rightDist >= 0) {
        speed = (1 - rightDist / edgeZone) * 8;
      } else if (leftDist < edgeZone && leftDist >= 0 && scrollOffset > 0) {
        speed = -(1 - leftDist / edgeZone) * 8;
      }
      if (speed !== 0) {
        scrollDelta += speed;
        scrollOffset = Math.max(0, origScrollOffset + scrollDelta / zoomLevel);
        const totalDx = lastMouseX - startX + scrollDelta;
        const dSec = totalDx / pxPerSec;
        const newCount = Math.max(0, Math.round((origLoopCount * clipDuration + dSec) / clipDuration));
        rr.loopCount = newCount;
        rr.loopEnabled = newCount > 0;
        renderTrackWaveform(track);
        renderRuler();
        updatePlayhead();
      }
      autoScrollRAF = requestAnimationFrame(doAutoScroll);
    };
    autoScrollRAF = requestAnimationFrame(doAutoScroll);

    const onMove = (ev) => {
      const rr = track.regions[regionIndex];
      if (!rr) return;
      lastMouseX = ev.clientX;
      const totalDx = ev.clientX - startX + scrollDelta;
      const dSec = totalDx / pxPerSec;
      const newCount = Math.max(0, Math.round((origLoopCount * clipDuration + dSec) / clipDuration));
      rr.loopCount = newCount;
      rr.loopEnabled = newCount > 0;
      renderTrackWaveform(track);
    };
    const onUp = async () => {
      if (autoScrollRAF) cancelAnimationFrame(autoScrollRAF);
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      await syncAudioRegionToNative(track);
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
}

// ─── Audio Split + Context Menu ──────────────────────
async function splitAudioAtPlayhead(track, regionIndex) {
  if (!track.hasBuff) return;
  // Find which region the playhead is in
  if (regionIndex == null) {
    regionIndex = track.regions.findIndex(r => {
      const rEnd = r.offset + (r.clipEnd - r.clipStart);
      return currentTime > r.offset && currentTime < rEnd;
    });
  }
  const r = track.regions[regionIndex];
  if (!r) { setStatus('Playhead outside region bounds'); return; }

  const splitTime = snapTime(currentTime);
  if (splitTime <= r.offset || splitTime >= r.offset + (r.clipEnd - r.clipStart)) {
    setStatus('Playhead outside region bounds');
    return;
  }

  // Determine which native track to split
  const nId = r.nativeTrackId || track.id;
  const result = await engine.splitAudioTrack(nId, splitTime);
  if (!result || result.error) {
    setStatus(result ? result.error : 'Split failed');
    return;
  }

  // Refresh left region's waveform data from its native track
  const wfLeft = await engine.getTrackWaveform(nId, 4000);
  const origOffset = r.offset;
  if (wfLeft && wfLeft.data) {
    r.waveformData = new Float32Array(wfLeft.data);
    r.duration = wfLeft.duration || 0;
    r.clipStart = 0;
    r.clipEnd = r.duration;
    r.loopEnabled = false;
    r.loopCount = 0;
    r.fadeOut = 0.01; // Auto-crossfade
  }

  // Fetch waveform for right side (new native track)
  const wfRight = await engine.getTrackWaveform(result.newTrackId, 4000);
  const rightDuration = (wfRight && wfRight.duration) || 0;
  const rightWaveform = (wfRight && wfRight.data) ? new Float32Array(wfRight.data) : null;

  // Create new region (right side) in the SAME JS track
  const rightRegion = {
    nativeTrackId: result.newTrackId,
    offset: splitTime,
    clipStart: 0,
    clipEnd: rightDuration,
    loopEnabled: false,
    loopCount: 0,
    fadeIn: 0.01, // Auto-crossfade
    fadeOut: 0,
    waveformData: rightWaveform,
    duration: rightDuration
  };

  // Register the new native track as a hidden sub-track
  track._hiddenNativeIds.push(result.newTrackId);
  trackIdCounter = Math.max(trackIdCounter, result.newTrackId + 1);

  // Insert right region after the split region in the same track
  track.regions.splice(regionIndex + 1, 0, rightRegion);

  // Sync params (volume/pan/mute/solo) to all native sub-tracks
  await syncSubTrackParams(track);

  // Sync regions to native
  await syncAudioRegionToNative(track);

  renderAllTracks();
  setStatus('Split region at playhead');
}

function showAudioRegionContextMenu(e, track, waveformEl, regionIndex) {
  // Remove any existing context menus
  document.querySelectorAll('.audio-region-ctx-menu').forEach(el => el.remove());

  const menu = document.createElement('div');
  menu.className = 'audio-region-ctx-menu';

  const r = track.regions[regionIndex];
  const items = [
    { label: 'Split at Playhead', action: () => splitAudioAtPlayhead(track, regionIndex) },
    { label: r && r.loopEnabled ? 'Disable Loop' : 'Enable Loop',
      action: () => {
        const rr = track.regions[regionIndex];
        if (!rr) return;
        rr.loopEnabled = !rr.loopEnabled;
        if (!rr.loopEnabled) rr.loopCount = 0;
        else if (rr.loopCount === 0) rr.loopCount = 1;
        syncAudioRegionToNative(track);
        renderTrackWaveform(track);
      }
    },
    { label: 'Reset Region', action: () => {
        const rr = track.regions[regionIndex];
        if (!rr) return;
        rr.offset = 0;
        rr.clipStart = 0;
        rr.clipEnd = rr.duration || track.duration;
        rr.loopEnabled = false;
        rr.loopCount = 0;
        syncAudioRegionToNative(track);
        renderTrackWaveform(track);
      }
    },
    { label: 'Set Fade In...', action: () => {
        const rr = track.regions[regionIndex];
        if (!rr) return;
        const val = prompt('Fade in duration (seconds):', (rr.fadeIn || 0).toString());
        if (val == null) return;
        rr.fadeIn = Math.max(0, parseFloat(val) || 0);
        syncAudioRegionToNative(track);
        renderTrackWaveform(track);
      }
    },
    { label: 'Set Fade Out...', action: () => {
        const rr = track.regions[regionIndex];
        if (!rr) return;
        const val = prompt('Fade out duration (seconds):', (rr.fadeOut || 0).toString());
        if (val == null) return;
        rr.fadeOut = Math.max(0, parseFloat(val) || 0);
        syncAudioRegionToNative(track);
        renderTrackWaveform(track);
      }
    },
    { label: 'Delete Region', action: async () => {
        const rr = track.regions[regionIndex];
        if (!rr) return;
        const nId = rr.nativeTrackId || track.id;
        await engine.setAudioRegion(nId, 0, 0, 0, false);
        track.regions.splice(regionIndex, 1);
        // If no regions left, mark track as empty
        if (track.regions.length === 0) {
          track.waveformData = null;
          track.hasBuff = false;
          track.duration = 0;
        }
        renderAllTracks();
        setStatus(`Deleted audio region on ${track.name}`);
      }
    },
    { label: 'Delete Track', cls: 'delete-region', action: async () => {
        // Remove all hidden native sub-tracks
        for (const nId of (track._hiddenNativeIds || [])) {
          await engine.removeTrack(nId);
        }
        await engine.removeTrack(track.id);
        tracks.splice(tracks.indexOf(track), 1);
        renderAllTracks();
      }
    }
  ];

  for (const item of items) {
    const el = document.createElement('div');
    el.className = 'ctx-item' + (item.cls ? ' ' + item.cls : '');
    el.textContent = item.label;
    el.onclick = () => { menu.remove(); item.action(); };
    menu.appendChild(el);
  }

  menu.style.left = e.clientX + 'px';
  menu.style.top = e.clientY + 'px';
  document.body.appendChild(menu);

  const dismiss = (ev) => {
    if (!menu.contains(ev.target)) { menu.remove(); document.removeEventListener('mousedown', dismiss); }
  };
  setTimeout(() => document.addEventListener('mousedown', dismiss), 0);
}

function bindRegionEdgeDrag(handleEl, track, waveformEl, side, pxPerBeat, regionIndex) {
  handleEl.addEventListener('mousedown', (e) => {
    e.stopPropagation();
    e.preventDefault();
    const r = track.regions[regionIndex];
    if (!r) return;
    const startX = e.clientX;
    const origStart = r.startBeat;
    const origEnd = r.endBeat;
    // Snapshot notes before trimming (for undo)
    const beforeSnapshot = (track.notes || []).map(n => ({
      noteNumber: n.note, startBeat: n.startBeat,
      lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
    }));
    track._edgeDragging = true;

    const onMove = (ev) => {
      const rr = track.regions[regionIndex];
      if (!rr) return;
      const dx = ev.clientX - startX;
      const dBeats = dx / pxPerBeat;
      if (side === 'left') {
        let newStart = snapToQuarter(origStart + dBeats);
        if (newStart < 0) newStart = 0;
        // Enforce minimum region width of 0.25 beats
        if (newStart >= rr.endBeat - 0.25) newStart = rr.endBeat - 0.25;
        rr.startBeat = newStart;
      } else {
        let newEnd = snapToQuarterCeil(origEnd + dBeats);
        // Enforce minimum region width of 0.25 beats
        if (newEnd <= rr.startBeat + 0.25) newEnd = rr.startBeat + 0.25;
        rr.endBeat = newEnd;
      }
      renderMidiTrackPreview(track);
    };
    const onUp = async () => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      track._edgeDragging = false;
      const rr = track.regions[regionIndex];
      if (!rr) return;

      // Trim/remove notes outside the new region boundaries
      const rStart = rr.startBeat;
      const rEnd = rr.endBeat;
      let trimmed = false;
      const newNotes = [];
      for (const n of (track.notes || [])) {
        const nEnd = n.startBeat + n.lengthBeats;
        // Note entirely outside region: drop it
        if (n.startBeat >= rEnd || nEnd <= rStart) { trimmed = true; continue; }
        const clipped = { ...n };
        // Trim note extending past right edge
        if (nEnd > rEnd) {
          clipped.lengthBeats = Math.max(0.001, rEnd - clipped.startBeat);
          trimmed = true;
        }
        // Trim note extending past left edge
        if (clipped.startBeat < rStart) {
          const cutAmount = rStart - clipped.startBeat;
          clipped.startBeat = rStart;
          clipped.lengthBeats = Math.max(0.001, clipped.lengthBeats - cutAmount);
          trimmed = true;
        }
        newNotes.push(clipped);
      }

      if (trimmed) {
        // Push undo snapshot
        const afterSnapshot = newNotes.map(n => ({
          noteNumber: n.note, startBeat: n.startBeat,
          lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
        }));
        if (typeof undoManager !== 'undefined' && undoManager) {
          undoManager.push(new SnapshotNotesCommand(
            engine, track.id, beforeSnapshot, afterSnapshot, 'Trim Region'
          ));
        }
        // Inject trimmed notes into engine
        await engine.injectMidiNotes(track.id, afterSnapshot, true);
        track.notes = newNotes;
        computeMidiRegion(track);
      }

      syncMidiRegionLoop(track);
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
}

function bindLoopHandleDrag(handleEl, track, waveformEl, pxPerBeat, regionIndex) {
  handleEl.addEventListener('mousedown', (e) => {
    e.stopPropagation();
    e.preventDefault();
    const r = track.regions[regionIndex];
    if (!r) return;
    const startX = e.clientX;
    const contentLen = r.contentEnd - r.contentStart;
    if (contentLen <= 0) return;
    const origLoopCount = r.loopCount || 0;
    const origScrollOffset = scrollOffset;
    let scrollDelta = 0;
    let autoScrollRAF = null;
    let lastMouseX = e.clientX;

    const doAutoScroll = () => {
      const rr = track.regions[regionIndex];
      if (!rr) return;
      const waveRect = waveformEl.getBoundingClientRect();
      const edgeZone = 60;
      const rightDist = waveRect.right - lastMouseX;
      const leftDist = lastMouseX - waveRect.left;
      let speed = 0;
      if (rightDist < edgeZone && rightDist >= 0) {
        speed = (1 - rightDist / edgeZone) * 8;
      } else if (leftDist < edgeZone && leftDist >= 0 && scrollOffset > 0) {
        speed = -(1 - leftDist / edgeZone) * 8;
      }
      if (speed !== 0) {
        scrollDelta += speed;
        scrollOffset = Math.max(0, origScrollOffset + scrollDelta / zoomLevel);
        const totalDx = lastMouseX - startX + scrollDelta;
        const dBeats = totalDx / pxPerBeat;
        let newCount = Math.max(0, Math.round((origLoopCount * contentLen + dBeats) / contentLen));
        rr.loopCount = newCount;
        renderMidiTrackPreview(track);
        renderRuler();
        updatePlayhead();
      }
      autoScrollRAF = requestAnimationFrame(doAutoScroll);
    };
    autoScrollRAF = requestAnimationFrame(doAutoScroll);

    const onMove = (ev) => {
      const rr = track.regions[regionIndex];
      if (!rr) return;
      lastMouseX = ev.clientX;
      const totalDx = ev.clientX - startX + scrollDelta;
      const dBeats = totalDx / pxPerBeat;
      let newCount = Math.max(0, Math.round((origLoopCount * contentLen + dBeats) / contentLen));
      rr.loopCount = newCount;
      renderMidiTrackPreview(track);
    };
    const onUp = () => {
      if (autoScrollRAF) cancelAnimationFrame(autoScrollRAF);
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      syncMidiRegionLoop(track);
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
}

function bindRegionMoveDrag(regionEl, track, waveformEl, pxPerBeat, regionIndex) {
  regionEl.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    if (e.target.closest('.region-edge-left, .region-edge-right, .region-loop-handle')) return;
    e.stopPropagation();
    e.preventDefault();
    const r = track.regions[regionIndex];
    if (!r) return;
    const startX = e.clientX;
    const startY = e.clientY;
    const origStart = r.startBeat;
    const origEnd = r.endBeat;
    const origContentStart = r.contentStart;
    const origContentEnd = r.contentEnd;
    let didDrag = false;
    let verticalDrag = false;
    let dropTargetTrack = null;

    const onMove = (ev) => {
      const dx = ev.clientX - startX;
      const dy = ev.clientY - startY;
      if (Math.abs(dx) < 3 && Math.abs(dy) < 3 && !didDrag) return;
      didDrag = true;

      // Detect vertical drag for track-to-track move
      if (Math.abs(dy) > 20) {
        verticalDrag = true;
        document.querySelectorAll('.drop-zone-indicator').forEach(el => el.remove());
        dropTargetTrack = null;
        const rows = document.querySelectorAll('.track-row');
        for (const row of rows) {
          const rect = row.getBoundingClientRect();
          if (ev.clientY >= rect.top && ev.clientY <= rect.bottom) {
            const tId = parseInt(row.dataset.id);
            const t = tracks.find(tr => tr.id === tId);
            if (t && t.type === 'midi' && t !== track) {
              dropTargetTrack = t;
              const indicator = document.createElement('div');
              indicator.className = 'drop-zone-indicator';
              row.querySelector('.track-waveform').appendChild(indicator);
            }
            break;
          }
        }
      }

      const rr = track.regions[regionIndex];
      if (!rr) return;
      const dBeats = snapToQuarter(dx / pxPerBeat);
      const newContentStart = Math.max(0, origContentStart + dBeats);
      const shift = newContentStart - origContentStart;
      rr.contentStart = origContentStart + shift;
      rr.contentEnd = origContentEnd + shift;
      rr.startBeat = origStart + shift;
      rr.endBeat = origEnd + shift;
      // Shift notes in local model
      const origNotes = track._dragOrigNotes || track.notes.map(n => ({ ...n }));
      if (!track._dragOrigNotes) track._dragOrigNotes = origNotes;
      track.notes = origNotes.map(n => ({ ...n, startBeat: n.startBeat + shift }));
      renderMidiTrackPreview(track);
    };
    const onUp = async () => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      document.querySelectorAll('.drop-zone-indicator').forEach(el => el.remove());

      if (!didDrag) {
        // Select this track/region
        selectedTrack = track;
        selectedRegionIndex = regionIndex;
        allRegionsSelected = false;
        document.querySelectorAll('.audio-region.selected, .midi-region.selected').forEach(el => el.classList.remove('selected'));
        document.querySelectorAll('.track-row.selected').forEach(el => el.classList.remove('selected'));
        regionEl.classList.add('selected');
        const row = regionEl.closest('.track-row');
        if (row) row.classList.add('selected');
        // Was just a click, seek to position
        const rRect = waveformEl.getBoundingClientRect();
        seekTo(scrollOffset + (e.clientX - rRect.left) / zoomLevel);
      } else if (verticalDrag && dropTargetTrack) {
        // Move MIDI region to target track
        await moveMidiRegionToTrack(track, regionIndex, dropTargetTrack);
        delete track._dragOrigNotes;
      } else if (track._dragOrigNotes) {
        // Commit moved notes to engine
        const result = await engine.getMidiNotes(track.id);
        const engineNotes = result.notes || [];
        for (let i = engineNotes.length - 1; i >= 0; i--) {
          await engine.removeMidiNote(track.id, i);
        }
        for (const n of track.notes) {
          await engine.addMidiNote(track.id, n.note, n.startBeat, n.lengthBeats, n.velocity);
        }
        // Re-apply loops if any
        const rr = track.regions[regionIndex];
        if (rr && rr.loopCount > 0) {
          const contentLen = rr.contentEnd - rr.contentStart;
          for (let loop = 1; loop <= rr.loopCount; loop++) {
            const offset = loop * contentLen;
            for (const n of track.notes) {
              await engine.addMidiNote(track.id, n.note, n.startBeat + offset, n.lengthBeats, n.velocity);
            }
          }
        }
        delete track._dragOrigNotes;
        renderAllTracks();
      }
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
}

async function moveMidiRegionToTrack(sourceTrack, regionIndex, targetTrack) {
  const region = sourceTrack.regions[regionIndex];
  if (!region) return;

  // Get notes within this region's beat range
  const regionNotes = (sourceTrack.notes || []).filter(n => {
    const nEnd = n.startBeat + n.lengthBeats;
    return n.startBeat < region.endBeat && nEnd > region.startBeat;
  });

  // Remove those notes from source track (engine + local)
  const remainingNotes = (sourceTrack.notes || []).filter(n => {
    const nEnd = n.startBeat + n.lengthBeats;
    return !(n.startBeat < region.endBeat && nEnd > region.startBeat);
  });
  // Clear source engine notes and re-add remaining
  const srcEng = (await engine.getMidiNotes(sourceTrack.id)).notes || [];
  for (let i = srcEng.length - 1; i >= 0; i--) {
    await engine.removeMidiNote(sourceTrack.id, i);
  }
  for (const n of remainingNotes) {
    await engine.addMidiNote(sourceTrack.id, n.note, n.startBeat, n.lengthBeats, n.velocity);
  }
  sourceTrack.notes = remainingNotes;

  // Remove split beat boundaries if they existed
  if (sourceTrack.splitBeats) {
    sourceTrack.splitBeats = sourceTrack.splitBeats.filter(b => b !== region.startBeat && b !== region.endBeat);
  }

  // Add notes to target track (engine + local)
  for (const n of regionNotes) {
    await engine.addMidiNote(targetTrack.id, n.note, n.startBeat, n.lengthBeats, n.velocity);
  }
  targetTrack.notes = [...(targetTrack.notes || []), ...regionNotes];

  // Recompute regions for both tracks
  computeMidiRegions(sourceTrack);
  computeMidiRegions(targetTrack);

  renderAllTracks();
  setStatus(`Moved MIDI region to ${targetTrack.name}`);
}

async function syncMidiRegionLoop(track) {
  const region = track.region;
  if (!region) return;
  const notes = track.notes || [];
  const loopCount = region.loopCount || 0;
  const contentLen = region.contentEnd - region.contentStart;

  // Clear all notes from engine, then re-add originals + loop copies
  const result = await engine.getMidiNotes(track.id);
  const engineNotes = result.notes || [];
  for (let i = engineNotes.length - 1; i >= 0; i--) {
    await engine.removeMidiNote(track.id, i);
  }
  // Re-add original notes
  for (const n of notes) {
    await engine.addMidiNote(track.id, n.note, n.startBeat, n.lengthBeats, n.velocity);
  }
  // Add loop copies
  for (let loop = 1; loop <= loopCount; loop++) {
    const offset = loop * contentLen;
    for (const n of notes) {
      await engine.addMidiNote(track.id, n.note, n.startBeat + offset, n.lengthBeats, n.velocity);
    }
  }
  if (loopCount > 0) setStatus(`${track.name}: ${loopCount} loop${loopCount > 1 ? 's' : ''}`);
  renderAllTracks();
}

async function splitMidiAtPlayhead(track) {
  if (!track.notes || track.notes.length === 0) return;
  if (track.regions.length === 0) { computeMidiRegion(track); }
  if (track.regions.length === 0) return;

  const bpm = parseInt(document.getElementById('bpm-input').value) || 120;
  const beatsPerSec = bpm / 60;
  const splitBeat = snapToQuarter(currentTime * beatsPerSec);

  // Check if playhead is inside any region
  const inRegion = track.regions.some(r => splitBeat > r.startBeat && splitBeat < r.endBeat);
  if (!inRegion) {
    setStatus('Playhead outside MIDI region bounds');
    return;
  }

  // Split any notes that span the split point
  const newNotes = [];
  for (const n of track.notes) {
    const noteEnd = n.startBeat + n.lengthBeats;
    if (n.startBeat < splitBeat && noteEnd > splitBeat) {
      // Note spans the split — trim left, create right remainder
      newNotes.push({ ...n, lengthBeats: splitBeat - n.startBeat });
      newNotes.push({ ...n, startBeat: splitBeat, lengthBeats: noteEnd - splitBeat });
    } else {
      newNotes.push({ ...n });
    }
  }

  // Update notes in engine
  const engineNotes = (await engine.getMidiNotes(track.id)).notes || [];
  for (let i = engineNotes.length - 1; i >= 0; i--) {
    await engine.removeMidiNote(track.id, i);
  }
  for (const n of newNotes) {
    await engine.addMidiNote(track.id, n.note, n.startBeat, n.lengthBeats, n.velocity);
  }
  track.notes = newNotes;

  // Add the split beat to splitBeats array
  if (!track.splitBeats) track.splitBeats = [];
  if (!track.splitBeats.includes(splitBeat)) {
    track.splitBeats.push(splitBeat);
  }

  // Recompute regions from split beats
  computeMidiRegions(track);

  renderAllTracks();
  setStatus('Split MIDI region at playhead');
}

function showMidiRegionContextMenu(e, track, waveformEl, regionIndex) {
  // Remove any existing context menu
  const old = document.querySelector('.midi-region-ctx-menu');
  if (old) old.remove();

  const menu = document.createElement('div');
  menu.className = 'midi-region-ctx-menu';

  const r = track.regions[regionIndex];
  const items = [
    { label: 'Split at Playhead', action: () => splitMidiAtPlayhead(track) },
    { label: r && r.loopCount > 0 ? 'Disable Loop' : 'Enable Loop',
      action: () => {
        const rr = track.regions[regionIndex];
        if (!rr) return;
        if (rr.loopCount > 0) {
          rr.loopCount = 0;
        } else {
          rr.loopCount = 1;
        }
        syncMidiRegionLoop(track);
        renderMidiTrackPreview(track);
      }
    },
    { label: 'Delete Region', cls: 'delete-region', action: async () => {
        const notes = track.notes || [];
        for (let i = notes.length - 1; i >= 0; i--) {
          await engine.removeMidiNote(track.id, i);
        }
        track.notes = [];
        track.regions = [];
        renderAllTracks();
        setStatus(`Deleted MIDI region on ${track.name}`);
      }
    }
  ];

  for (const item of items) {
    const el = document.createElement('div');
    el.className = 'ctx-item' + (item.cls ? ' ' + item.cls : '');
    el.textContent = item.label;
    el.onclick = () => { menu.remove(); item.action(); };
    menu.appendChild(el);
  }

  menu.style.left = e.clientX + 'px';
  menu.style.top = e.clientY + 'px';
  document.body.appendChild(menu);

  const dismiss = (ev) => {
    if (!menu.contains(ev.target)) { menu.remove(); document.removeEventListener('click', dismiss); }
  };
  setTimeout(() => document.addEventListener('click', dismiss), 0);
}

function hexToRGBA(hex, alpha) {
  const r = parseInt(hex.slice(1,3), 16);
  const g = parseInt(hex.slice(3,5), 16);
  const b = parseInt(hex.slice(5,7), 16);
  return `rgba(${r},${g},${b},${alpha})`;
}


// ─── Timeline Ruler ───────────────────────────────────
function renderRuler() {
  const canvas = document.getElementById('ruler-canvas');
  const wrap = document.getElementById('ruler-canvas-wrap');
  const rect = wrap.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  const c = canvas.getContext('2d');
  c.scale(dpr, dpr);
  const w = rect.width;
  const h = rect.height;
  c.clearRect(0, 0, w, h);

  const pxPerSec = zoomLevel;
  let interval = 1;
  if (pxPerSec < 8) interval = 10;
  else if (pxPerSec < 20) interval = 5;
  else if (pxPerSec < 50) interval = 2;
  else if (pxPerSec < 120) interval = 1;
  else interval = 0.5;

  const viewSecs = w / pxPerSec;
  const startSec = scrollOffset;
  const endSec = scrollOffset + viewSecs;
  c.font = '9px -apple-system, Helvetica Neue, sans-serif';

  // Sub-ticks
  const subInterval = interval / 4;
  c.strokeStyle = '#5e5955';
  c.lineWidth = 1;
  const firstSub = Math.floor(startSec / subInterval) * subInterval;
  for (let t = firstSub; t <= endSec; t += subInterval) {
    const x = Math.round((t - scrollOffset) * pxPerSec) + 0.5;
    if (x < 0) continue;
    c.beginPath();
    c.moveTo(x, h);
    c.lineTo(x, h - 4);
    c.stroke();
  }

  // Major ticks + labels
  c.fillStyle = '#9e9690';
  c.strokeStyle = '#7a7470';
  const firstMajor = Math.floor(startSec / interval) * interval;
  for (let t = firstMajor; t <= endSec; t += interval) {
    const x = Math.round((t - scrollOffset) * pxPerSec) + 0.5;
    if (x < 0) continue;
    c.beginPath();
    c.moveTo(x, h);
    c.lineTo(x, h - 10);
    c.stroke();
    const mins = Math.floor(t / 60);
    const secs = Math.floor(t % 60);
    const frac = t % 1;
    let label = `${mins}:${String(secs).padStart(2, '0')}`;
    if (frac > 0) label += `.5`;
    c.fillText(label, x + 3, 10);
  }
}

function updateLoopOverlay() {
  const overlay = document.getElementById('loop-overlay');
  if (!loopEnabled) { overlay.style.display = 'none'; return; }
  overlay.style.display = 'block';
  overlay.style.left = ((loopStart - scrollOffset) * zoomLevel) + 'px';
  overlay.style.width = ((loopEnd - loopStart) * zoomLevel) + 'px';
}


// ─── Time & Playhead ──────────────────────────────────
function formatTime(t) {
  const mins = Math.floor(t / 60);
  const secs = Math.floor(t % 60);
  const ms = Math.floor((t % 1) * 1000);
  return `${String(mins).padStart(2,'0')}:${String(secs).padStart(2,'0')}.${String(ms).padStart(3,'0')}`;
}

function updateTimeDisplay() {
  document.getElementById('time-display').textContent = formatTime(currentTime);
}

function updatePlayhead() {
  const el = document.getElementById('playhead');
  const wrapper = document.getElementById('tracks-wrapper');
  const waveArea = document.querySelector('.track-waveform');
  let sidebarWidth = 310;
  if (waveArea && wrapper) {
    sidebarWidth = waveArea.getBoundingClientRect().left - wrapper.getBoundingClientRect().left;
  }
  const px = sidebarWidth + (currentTime - scrollOffset) * zoomLevel;
  el.style.left = px + 'px';
  const wrapperWidth = wrapper ? wrapper.getBoundingClientRect().width : window.innerWidth;
  el.style.display = (px < sidebarWidth || px > wrapperWidth) ? 'none' : '';
}


// ─── Meter Updates (pushed from native engine at 30Hz) ──
engine.onMeterData((data) => {
  // Master meters
  const pctL = Math.min((data.masterPeakL || 0) * 100 * 1.5, 100);
  const pctR = Math.min((data.masterPeakR || 0) * 100 * 1.5, 100);
  const meterL = document.getElementById('master-meter-l');
  const meterR = document.getElementById('master-meter-r');
  meterL.style.width = pctL + '%';
  meterR.style.width = pctR + '%';
  const colorForPct = p => p > 85 ? 'var(--meter-red)' : p > 60 ? 'var(--meter-yellow)' : 'var(--meter-green)';
  meterL.style.background = colorForPct(pctL);
  meterR.style.background = colorForPct(pctR);

  const peakDb = 20 * Math.log10(Math.max(data.masterPeakL || 0, data.masterPeakR || 0) || 0.0001);
  const peakEl = document.getElementById('master-peak-db');
  peakEl.textContent = peakDb > -0.5 ? 'CLIP' : (peakDb > -60 ? peakDb.toFixed(1) + ' dB' : '-\u221E dB');
  peakEl.style.color = peakDb > -0.5 ? 'var(--meter-red)' : 'var(--text-dim)';

  // Per-track meters
  const now = performance.now();
  for (const tm of (data.tracks || [])) {
    const row = document.querySelector(`.track-row[data-id="${tm.trackId}"]`);
    if (!row) continue;
    const meter = row.querySelector('.track-meter-fill');
    const peakDot = row.querySelector('.track-peak-dot');
    if (!meter) continue;

    const tPct = Math.min((tm.peakL || 0) * 100 * 1.5, 100);
    meter.style.width = tPct + '%';
    meter.style.background = tPct > 85 ? 'var(--meter-red)' : tPct > 60 ? 'var(--meter-yellow)' : 'var(--meter-green)';

    const track = tracks.find(t => t.id === tm.trackId);
    if (track && tm.clipping && peakDot) {
      peakDot.classList.add('clip');
      track.clipping = true;
      track.peakHoldTime = now;
    }
    if (track && track.clipping && now - track.peakHoldTime > 2000 && peakDot) {
      peakDot.classList.remove('clip');
      track.clipping = false;
    }
  }

  // Per-bus-track meters
  for (const bm of (data.busTracks || [])) {
    const busRow = document.querySelector(`.track-row.bus-track[data-id="${bm.trackId}"]`);
    if (!busRow) continue;
    const meter = busRow.querySelector('.track-meter-fill');
    if (!meter) continue;
    const bPct = Math.min((bm.peakL || 0) * 100 * 1.5, 100);
    meter.style.width = bPct + '%';
    meter.style.background = bPct > 85 ? 'var(--meter-red)' : bPct > 60 ? 'var(--meter-yellow)' : 'var(--meter-green)';
  }

  // Update mixer board meters
  mixerBoard.updateMeters(data);

  // MIDI activity LED
  const midiLed = document.getElementById('midi-led');
  if (midiLed) {
    midiLed.classList.toggle('active', !!data.midiActivity);
  }
});

engine.onTransportUpdate((data) => {
  currentTime = data.currentTime || 0;
  updateTimeDisplay();
  updatePlayhead();

  // Follow playhead auto-scroll
  if (followPlayhead && isPlaying) {
    const waveArea = document.querySelector('.track-waveform');
    const viewSecs = (waveArea ? waveArea.getBoundingClientRect().width : 500) / zoomLevel;
    if (currentTime > scrollOffset + viewSecs * 0.8 || currentTime < scrollOffset) {
      scrollOffset = Math.max(0, currentTime - viewSecs * 0.2);
      renderRuler();
      tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t));
      updatePlayhead();
      updateLoopOverlay();
    }
  }

  // Feed piano roll playhead
  if (pianoRoll.isOpen) {
    const bpm = parseInt(document.getElementById('bpm-input').value) || 120;
    pianoRoll.updatePlayhead(currentTime, bpm);
  }

  // Metronome beat flash
  if (data.currentBeat !== undefined) {
    const dot = document.getElementById('metro-dot');
    dot.classList.add('beat');
    setTimeout(() => dot.classList.remove('beat'), 80);
  }

  // Live recording waveform is now handled via setInterval in toggleRecord
});


// ─── UI Rendering ─────────────────────────────────────
function renderAllTracks() {
  const container = document.getElementById('tracks-container');
  const emptyState = document.getElementById('empty-state');

  container.innerHTML = '';

  if (tracks.length === 0) {
    emptyState.style.display = 'flex';
    return;
  }
  emptyState.style.display = 'none';

  const audioCount = tracks.filter(t => t.type !== 'midi').length;
  const midiCount = tracks.filter(t => t.type === 'midi').length;
  const busCount = busTracks.length;
  const infoParts = [];
  if (audioCount) infoParts.push(`${audioCount}A`);
  if (midiCount) infoParts.push(`${midiCount}M`);
  if (busCount) infoParts.push(`${busCount}B`);
  document.getElementById('ruler-info').textContent =
    infoParts.length === 0 ? '0 tracks' : infoParts.join(' / ');

  for (const track of tracks) {
    const row = document.createElement('div');
    row.className = 'track-row';
    if (track.type === 'midi') row.classList.add('midi-track');
    if (track.armed) row.classList.add('armed');
    if (track.muted) row.classList.add('muted');
    if (track === selectedTrack) row.classList.add('selected');
    row.dataset.id = track.id;
    row.dataset.type = track.type || 'audio';

    const panDisplay = track.pan === 0 ? 'C' : (track.pan < 0 ? Math.round(Math.abs(track.pan)*100)+'L' : Math.round(track.pan*100)+'R');
    const isMidi = track.type === 'midi';

    const headerButtons = isMidi
      ? `<button class="track-btn arm-btn ${track.armed?'active':''}" data-action="arm" title="Arm">R</button>
         <button class="track-btn mute-btn ${track.muted?'active':''}" data-action="mute" title="Mute">M</button>
         <button class="track-btn solo-btn ${track.solo?'active':''}" data-action="solo" title="Solo">S</button>
         <button class="track-btn inst-btn" data-action="instrument" title="Instrument">INST</button>
         <button class="track-btn edit-btn" data-action="edit-midi" title="Open Piano Roll">EDIT</button>
         <button class="track-btn" data-action="midi-split-at-playhead" title="Split at Playhead">&#9986;</button>
         <button class="track-btn ai-btn" data-action="midi-ai-generate" title="AI MIDI Generate">AI</button>
         <button class="track-btn delete-btn" data-action="delete" title="Delete">&#215;</button>`
      : `<button class="track-btn arm-btn ${track.armed?'active':''}" data-action="arm" title="Arm">R</button>
         <button class="track-btn mute-btn ${track.muted?'active':''}" data-action="mute" title="Mute">M</button>
         <button class="track-btn solo-btn ${track.solo?'active':''}" data-action="solo" title="Solo">S</button>
         <button class="track-btn" data-action="split-at-playhead" title="Split at Playhead">&#9986;</button>
         <button class="track-btn stems-btn" data-action="split-stems" title="Split into Stems">Stems</button>
         <button class="track-btn ai-btn" data-action="audio-ai-generate" title="AI Audio Generate">AI</button>
         <button class="track-btn delete-btn" data-action="delete" title="Delete">&#215;</button>`;

    const typeLabel = isMidi ? '<span class="track-type-badge midi">MIDI</span>' : '';
    const instLabelText = escHTML(track.instrumentName || 'No Instrument');
    const instClickAttr = track.instrumentType ? ' data-action="inst-params" style="cursor:pointer;" title="Edit parameters"' : '';
    const instEditorLink = track.hasPluginInstrument ? ' <span class="inst-edit-link" data-action="open-inst-editor" title="Open plugin window">&#9881;</span>' : '';
    const instrumentLabel = isMidi
      ? `<div class="track-instrument-label"${instClickAttr}>${instLabelText}${instEditorLink}</div>`
      : '';

    // Build output routing dropdown
    let outputRouteHTML = '';
    if (busTracks.length > 0) {
      const busOptions = busTracks.map(b =>
        `<option value="${b.id}" ${track.outputBus === b.id ? 'selected' : ''}>${escHTML(b.name)}</option>`
      ).join('');
      outputRouteHTML = `
        <div class="output-route-wrap">
          <label>Out</label>
          <select class="output-route-select" data-action="output-route">
            <option value="-1" ${track.outputBus === -1 ? 'selected' : ''}>Master</option>
            ${busOptions}
          </select>
        </div>`;
    }

    // Build insert chain display (5 slots)
    const insertChainHTML = `
      <div class="insert-chain" data-track-id="${track.id}" data-track-type="${track.type}">
        <div class="insert-chain-label">Inserts</div>
        ${[0,1,2,3,4].map(i => `
          <div class="insert-slot" data-slot="${i}">
            <span class="insert-name" data-action="insert-click">— empty —</span>
            <span class="insert-add" data-action="insert-add" title="Add effect">+</span>
          </div>
        `).join('')}
      </div>`;

    row.innerHTML = `
      <div class="track-info">
        <div class="track-header">
          <div class="track-color-dot" style="background:${track.color}" data-action="color"></div>
          ${typeLabel}
          <input class="track-name" value="${escHTML(track.name)}" data-action="rename">
          ${headerButtons}
        </div>
        ${instrumentLabel}
        ${insertChainHTML}
        ${outputRouteHTML}
        <div class="track-controls">
          <div class="track-slider-group">
            <label>Vol</label>
            <input type="range" min="0" max="1" step="0.01" value="${track.volume}" data-action="volume">
            <span class="val">${Math.round(track.volume*100)}</span>
          </div>
          <div class="track-knob-group">
            <label>Pan</label>
            <div class="track-pan-knob-wrap">
              <div class="track-pan-knob" data-role="pan-knob"><div class="track-pan-indicator"></div></div>
              <input type="range" class="track-pan-input" min="-1" max="1" step="0.01" value="${track.pan}" data-action="pan">
            </div>
            <span class="val">${panDisplay}</span>
          </div>
        </div>
        <div class="track-meter-wrap">
          <div class="track-meter-fill"></div>
          <div class="track-peak-dot"></div>
        </div>
      </div>
      <div class="track-waveform">
        <canvas class="waveform-canvas"></canvas>
      </div>
    `;

    // Events — all calls go through engine IPC
    row.querySelector('[data-action="arm"]').onclick = async () => {
      track.armed = !track.armed;
      if (isMidi) await engine.setMidiTrackArmed(track.id, track.armed);
      else await engine.setTrackArmed(track.id, track.armed);
      renderAllTracks();
    };
    row.querySelector('[data-action="mute"]').onclick = async () => {
      track.muted = !track.muted;
      if (isMidi) await engine.setMidiTrackMute(track.id, track.muted);
      else {
        await engine.setTrackMute(track.id, track.muted);
        if (track._hiddenNativeIds && track._hiddenNativeIds.length > 0) await syncSubTrackParams(track);
      }
      renderAllTracks();
    };
    row.querySelector('[data-action="solo"]').onclick = async () => {
      track.solo = !track.solo;
      if (isMidi) await engine.setMidiTrackSolo(track.id, track.solo);
      else {
        await engine.setTrackSolo(track.id, track.solo);
        if (track._hiddenNativeIds && track._hiddenNativeIds.length > 0) await syncSubTrackParams(track);
      }
      renderAllTracks();
    };
    if (isMidi) {
      row.querySelector('[data-action="instrument"]').onclick = () => openInstrumentModal(track);
      row.querySelector('[data-action="edit-midi"]').onclick = () => pianoRoll.open(track.id, track.name);
      const instParamBtn = row.querySelector('[data-action="inst-params"]');
      if (instParamBtn) instParamBtn.onclick = (e) => { e.stopPropagation(); openInstParamPanel(track); };
      const instEditorBtn = row.querySelector('[data-action="open-inst-editor"]');
      if (instEditorBtn) instEditorBtn.onclick = (e) => { e.stopPropagation(); engine.openMidiInstrumentEditor(track.id); };
      const midiSplitBtn = row.querySelector('[data-action="midi-split-at-playhead"]');
      if (midiSplitBtn) midiSplitBtn.onclick = () => splitMidiAtPlayhead(track);
    } else {
      const splitBtn = row.querySelector('[data-action="split-at-playhead"]');
      if (splitBtn) splitBtn.onclick = () => splitAudioAtPlayhead(track);

      const stemsBtn = row.querySelector('[data-action="split-stems"]');
      if (stemsBtn) stemsBtn.onclick = () => openStemsModal(track);

      const qBtn = row.querySelector('[data-action="tempo-match"]');
      if (qBtn) qBtn.onclick = () => openSyncModal(track);
    }
    // AI Generate buttons - separate actions for MIDI and audio tracks
    const midiAiBtn = row.querySelector('[data-action="midi-ai-generate"]');
    if (midiAiBtn) {
      midiAiBtn.onclick = () => openMidiAIModal(track);
    }
    const audioAiBtn = row.querySelector('[data-action="audio-ai-generate"]');
    if (audioAiBtn) {
      audioAiBtn.onclick = () => {
        if (track.type === 'midi') { setStatus('Audio AI is not available on MIDI tracks.'); return; }
        openAudioAIModal(track);
      };
    }
    row.querySelector('[data-action="delete"]').onclick = async () => {
      await removeTrack(track.id);
      renderAllTracks();
    };
    row.querySelector('[data-action="rename"]').onchange = e => { track.name = e.target.value; };
    row.querySelector('[data-action="color"]').onclick = () => {
      const idx = TRACK_COLORS.indexOf(track.color);
      track.color = TRACK_COLORS[(idx + 1) % TRACK_COLORS.length];
      renderAllTracks();
    };
    row.querySelector('[data-action="volume"]').oninput = (e) => {
      track.volume = parseFloat(e.target.value);
      e.target.parentElement.querySelector('.val').textContent = Math.round(track.volume * 100);
      if (e.target._rafId) cancelAnimationFrame(e.target._rafId);
      e.target._rafId = requestAnimationFrame(() => {
        e.target._rafId = null;
        if (isMidi) engine.setMidiTrackVolume(track.id, track.volume);
        else {
          engine.setTrackVolume(track.id, track.volume);
          if (track._hiddenNativeIds && track._hiddenNativeIds.length > 0) syncSubTrackParams(track);
        }
      });
    };
    row.querySelector('[data-action="pan"]').oninput = (e) => {
      track.pan = parseFloat(e.target.value);
      const d = track.pan === 0 ? 'C' : (track.pan < 0 ? Math.round(Math.abs(track.pan)*100)+'L' : Math.round(track.pan*100)+'R');
      const knobGroup = e.target.closest('.track-knob-group');
      if (knobGroup) knobGroup.querySelector('.val').textContent = d;
      const knob = row.querySelector('[data-role="pan-knob"]');
      if (knob) knob.style.transform = `rotate(${track.pan * 135}deg)`;
      if (e.target._rafId) cancelAnimationFrame(e.target._rafId);
      e.target._rafId = requestAnimationFrame(() => {
        e.target._rafId = null;
        if (isMidi) engine.setMidiTrackPan(track.id, track.pan);
        else {
          engine.setTrackPan(track.id, track.pan);
          if (track._hiddenNativeIds && track._hiddenNativeIds.length > 0) syncSubTrackParams(track);
        }
      });
    };
    row.querySelector('.track-waveform').onclick = e => {
      selectedTrack = track;
      selectedRegionIndex = -1;
      allRegionsSelected = true;
      document.querySelectorAll('.track-row.selected').forEach(el => el.classList.remove('selected'));
      row.classList.add('selected');
      highlightAllRegions(track);
      const rect = e.currentTarget.getBoundingClientRect();
      seekTo(scrollOffset + (e.clientX - rect.left) / zoomLevel);
    };
    row.querySelector('.track-info').addEventListener('mousedown', (ev) => {
      if (ev.target.closest('button, input, select, .insert-slot')) return;
      selectedTrack = track;
      selectedRegionIndex = -1;
      allRegionsSelected = true;
      document.querySelectorAll('.track-row.selected').forEach(el => el.classList.remove('selected'));
      row.classList.add('selected');
      highlightAllRegions(track);
    });
    if (isMidi) {
      row.querySelector('.track-waveform').ondblclick = () => {
        pianoRoll.open(track.id, track.name);
      };
    }

    // Output routing
    const routeSelect = row.querySelector('[data-action="output-route"]');
    if (routeSelect) {
      routeSelect.onchange = async (e) => {
        track.outputBus = parseInt(e.target.value);
        if (isMidi) await engine.setMidiTrackOutput(track.id, track.outputBus);
        else await engine.setTrackOutput(track.id, track.outputBus);
      };
    }

    // Insert chain: populate from engine and bind events
    refreshInsertChain(row, track);

    // Set initial pan knob rotation
    const panKnob = row.querySelector('[data-role="pan-knob"]');
    if (panKnob) panKnob.style.transform = `rotate(${track.pan * 135}deg)`;

    container.appendChild(row);
  }

  // ─── Bus Track Rows ───
  if (busTracks.length > 0) {
    const busHeader = document.createElement('div');
    busHeader.className = 'bus-section-header';
    busHeader.innerHTML = '<span>Bus Tracks</span>';
    container.appendChild(busHeader);

    for (const bus of busTracks) {
      const busRow = document.createElement('div');
      busRow.className = 'track-row bus-track';
      busRow.dataset.id = bus.id;
      busRow.dataset.type = 'bus';

      const panDisplay = bus.pan === 0 ? 'C' : (bus.pan < 0 ? Math.round(Math.abs(bus.pan)*100)+'L' : Math.round(bus.pan*100)+'R');

      const busInsertChainHTML = `
        <div class="insert-chain" data-track-id="${bus.id}" data-track-type="bus">
          <div class="insert-chain-label">Inserts</div>
          ${[0,1,2,3,4].map(i => `
            <div class="insert-slot" data-slot="${i}">
              <span class="insert-name" data-action="insert-click">— empty —</span>
              <span class="insert-add" data-action="insert-add" title="Add effect">+</span>
            </div>
          `).join('')}
        </div>`;

      busRow.innerHTML = `
        <div class="track-info">
          <div class="track-header">
            <div class="track-color-dot" style="background:${bus.color}" data-action="color"></div>
            <span class="track-type-badge bus">BUS</span>
            <input class="track-name" value="${escHTML(bus.name)}" data-action="rename">
            <button class="track-btn mute-btn ${bus.muted?'active':''}" data-action="mute" title="Mute">M</button>
            <button class="track-btn solo-btn ${bus.solo?'active':''}" data-action="solo" title="Solo">S</button>
            <button class="track-btn delete-btn" data-action="delete" title="Delete">&#215;</button>
          </div>
          ${busInsertChainHTML}
          <div class="track-controls">
            <div class="track-slider-group">
              <label>Vol</label>
              <input type="range" min="0" max="1" step="0.01" value="${bus.volume}" data-action="volume">
              <span class="val">${Math.round(bus.volume*100)}</span>
            </div>
            <div class="track-knob-group">
              <label>Pan</label>
              <div class="track-pan-knob-wrap">
                <div class="track-pan-knob" data-role="pan-knob"><div class="track-pan-indicator"></div></div>
                <input type="range" class="track-pan-input" min="-1" max="1" step="0.01" value="${bus.pan}" data-action="pan">
              </div>
              <span class="val">${panDisplay}</span>
            </div>
          </div>
          <div class="track-meter-wrap">
            <div class="track-meter-fill"></div>
            <div class="track-peak-dot"></div>
          </div>
        </div>
        <div class="track-waveform"><canvas class="waveform-canvas"></canvas></div>
      `;

      busRow.querySelector('[data-action="mute"]').onclick = async () => {
        bus.muted = !bus.muted;
        await engine.setBusTrackMute(bus.id, bus.muted);
        renderAllTracks();
      };
      busRow.querySelector('[data-action="solo"]').onclick = async () => {
        bus.solo = !bus.solo;
        await engine.setBusTrackSolo(bus.id, bus.solo);
        renderAllTracks();
      };
      busRow.querySelector('[data-action="delete"]').onclick = async () => {
        await removeBusTrack(bus.id);
        renderAllTracks();
      };
      busRow.querySelector('[data-action="rename"]').onchange = e => { bus.name = e.target.value; };
      busRow.querySelector('[data-action="volume"]').oninput = (e) => {
        bus.volume = parseFloat(e.target.value);
        e.target.parentElement.querySelector('.val').textContent = Math.round(bus.volume * 100);
        if (e.target._rafId) cancelAnimationFrame(e.target._rafId);
        e.target._rafId = requestAnimationFrame(() => {
          e.target._rafId = null;
          engine.setBusTrackVolume(bus.id, bus.volume);
        });
      };
      busRow.querySelector('[data-action="pan"]').oninput = (e) => {
        bus.pan = parseFloat(e.target.value);
        const d = bus.pan === 0 ? 'C' : (bus.pan < 0 ? Math.round(Math.abs(bus.pan)*100)+'L' : Math.round(bus.pan*100)+'R');
        const knobGroup = e.target.closest('.track-knob-group');
        if (knobGroup) knobGroup.querySelector('.val').textContent = d;
        const knob = busRow.querySelector('[data-role="pan-knob"]');
        if (knob) knob.style.transform = `rotate(${bus.pan * 135}deg)`;
        if (e.target._rafId) cancelAnimationFrame(e.target._rafId);
        e.target._rafId = requestAnimationFrame(() => {
          e.target._rafId = null;
          engine.setBusTrackPan(bus.id, bus.pan);
        });
      };

      refreshInsertChain(busRow, bus);

      const busPanKnob = busRow.querySelector('[data-role="pan-knob"]');
      if (busPanKnob) busPanKnob.style.transform = `rotate(${bus.pan * 135}deg)`;

      container.appendChild(busRow);
    }
  }

  requestAnimationFrame(() => {
    tracks.forEach(t => {
      if (t.type === 'midi') renderMidiTrackPreview(t);
      else renderTrackWaveform(t);
    });
    renderRuler();
    updateLoopOverlay();
  });

  // Keep mixer in sync
  if (mixerBoard.isOpen) mixerBoard.render();
}

function updateTransportUI() {
  document.getElementById('btn-play').classList.toggle('active', isPlaying && !isRecording);
  document.getElementById('btn-record').classList.toggle('recording', isRecording);
}

function setStatus(msg) {
  document.getElementById('status-text').textContent = msg;
}

function escHTML(s) {
  return s.replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}


// ─── Effects Panel ────────────────────────────────────
let fxPanelTrack = null;

function openFxPanel(track) {
  fxPanelTrack = track;
  const panel = document.getElementById('fx-panel');
  const body = document.getElementById('fx-panel-body');
  document.getElementById('fx-panel-title').textContent = `FX: ${track.name}`;

  body.innerHTML = `
    ${fxSectionHTML('3-Band EQ', 'eq', track.fx.eqEnabled, [
      { label: 'Low', param: 'eqLowGain', min: -12, max: 12, step: 0.5, val: track.fx.eqLowGain, unit: 'dB' },
      { label: 'Mid', param: 'eqMidGain', min: -12, max: 12, step: 0.5, val: track.fx.eqMidGain, unit: 'dB' },
      { label: 'Mid Freq', param: 'eqMidFreq', min: 200, max: 8000, step: 10, val: track.fx.eqMidFreq, unit: 'Hz' },
      { label: 'High', param: 'eqHighGain', min: -12, max: 12, step: 0.5, val: track.fx.eqHighGain, unit: 'dB' },
    ])}
    ${fxSectionHTML('Compressor', 'comp', track.fx.compEnabled, [
      { label: 'Threshold', param: 'compThreshold', min: -60, max: 0, step: 1, val: track.fx.compThreshold, unit: 'dB' },
      { label: 'Ratio', param: 'compRatio', min: 1, max: 20, step: 0.5, val: track.fx.compRatio, unit: ':1' },
      { label: 'Attack', param: 'compAttack', min: 0, max: 0.1, step: 0.001, val: track.fx.compAttack, unit: 's' },
      { label: 'Release', param: 'compRelease', min: 0.01, max: 1, step: 0.01, val: track.fx.compRelease, unit: 's' },
    ])}
    ${fxSectionHTML('Delay', 'delay', track.fx.delayEnabled, [
      { label: 'Time', param: 'delayTime', min: 0, max: 2, step: 0.01, val: track.fx.delayTime, unit: 's' },
      { label: 'Mix', param: 'delayMix', min: 0, max: 1, step: 0.01, val: track.fx.delayMix, unit: '' },
      { label: 'Feedback', param: 'delayFB', min: 0, max: 0.9, step: 0.01, val: track.fx.delayFeedback, unit: '' },
    ])}
  `;

  // Bind toggle buttons
  body.querySelectorAll('.fx-toggle').forEach(btn => {
    btn.onclick = async () => {
      btn.classList.toggle('on');
      const section = btn.dataset.fxSection;
      const enabled = btn.classList.contains('on');
      if (section === 'eq') { track.fx.eqEnabled = enabled; await engine.setTrackFxEnabled(track.id, 'eq', enabled); }
      if (section === 'comp') { track.fx.compEnabled = enabled; await engine.setTrackFxEnabled(track.id, 'compressor', enabled); }
      if (section === 'delay') { track.fx.delayEnabled = enabled; await engine.setTrackFxEnabled(track.id, 'delay', enabled); }
    };
  });

  // Bind sliders
  body.querySelectorAll('input[data-param]').forEach(slider => {
    slider.oninput = () => {
      const val = parseFloat(slider.value);
      const param = slider.dataset.param;
      slider.parentElement.querySelector('.fx-val').textContent = formatFxVal(val, slider.dataset.unit);
      // Throttle IPC: one call per param per animation frame
      if (slider._rafId) cancelAnimationFrame(slider._rafId);
      slider._rafId = requestAnimationFrame(() => {
        slider._rafId = null;
        applyFxParam(track, param, parseFloat(slider.value));
      });
    };
  });

  panel.classList.add('open');
}

function fxSectionHTML(title, sectionKey, enabled, params) {
  return `
    <div class="fx-section">
      <div class="fx-section-header">
        <h4>${title}</h4>
        <button class="fx-toggle ${enabled ? 'on' : ''}" data-fx-section="${sectionKey}"></button>
      </div>
      <div class="fx-section-body">
        ${params.map(p => `
          <div class="fx-param">
            <label>${p.label}</label>
            <input type="range" min="${p.min}" max="${p.max}" step="${p.step}" value="${p.val}" data-param="${p.param}" data-unit="${p.unit}">
            <span class="fx-val">${formatFxVal(p.val, p.unit)}</span>
          </div>
        `).join('')}
      </div>
    </div>
  `;
}

function formatFxVal(val, unit) {
  if (unit === 'Hz') return val >= 1000 ? (val / 1000).toFixed(1) + ' kHz' : Math.round(val) + ' Hz';
  if (unit === 'dB') return val.toFixed(1) + ' dB';
  if (unit === ':1') return val.toFixed(1) + ':1';
  if (unit === 'ms') return val.toFixed(1) + ' ms';
  if (unit === 's') return val >= 0.01 ? val.toFixed(2) + 's' : (val * 1000).toFixed(0) + 'ms';
  if (unit === '%') return Math.round(val * 100) + '%';
  if (unit === 'x') return val.toFixed(1) + 'x';
  return val.toFixed(2);
}

const FX_PARAM_MAP = {
  eqLowGain:     ['eq', 'lowGain'],
  eqMidGain:     ['eq', 'midGain'],
  eqMidFreq:     ['eq', 'midFreq'],
  eqHighGain:    ['eq', 'highGain'],
  compThreshold: ['compressor', 'threshold'],
  compRatio:     ['compressor', 'ratio'],
  compAttack:    ['compressor', 'attack'],
  compRelease:   ['compressor', 'release'],
  delayTime:     ['delay', 'time'],
  delayMix:      ['delay', 'mix'],
  delayFB:       ['delay', 'feedback'],
};

async function applyFxParam(track, param, val) {
  // Update local state
  const stateMap = {
    eqLowGain: 'eqLowGain', eqMidGain: 'eqMidGain', eqMidFreq: 'eqMidFreq',
    eqHighGain: 'eqHighGain', compThreshold: 'compThreshold', compRatio: 'compRatio',
    compAttack: 'compAttack', compRelease: 'compRelease',
    delayTime: 'delayTime', delayMix: 'delayMix', delayFB: 'delayFeedback',
  };
  if (stateMap[param]) track.fx[stateMap[param]] = val;

  // Send to native engine
  const mapping = FX_PARAM_MAP[param];
  if (mapping) {
    await engine.setTrackFxParam(track.id, mapping[0], mapping[1], val);
  }
}

document.getElementById('fx-panel-close').onclick = () => {
  document.getElementById('fx-panel').classList.remove('open');
  fxPanelTrack = null;
};


// ─── Plugin Browser ───────────────────────────────────
let pluginTrack = null;
let pluginSlotIndex = 0;
let cachedPlugins = [];

async function openPluginModal(track) {
  pluginTrack = track;
  pluginSlotIndex = track.plugins.length;
  const modal = document.getElementById('plugin-modal');
  const list = document.getElementById('plugin-list');
  const search = document.getElementById('plugin-search');
  search.value = '';

  list.innerHTML = '<li class="no-projects">Loading plugins...</li>';
  modal.classList.add('open');

  const result = await engine.getPluginList();
  cachedPlugins = result.plugins || [];
  renderPluginList('');

  search.oninput = () => renderPluginList(search.value.toLowerCase());
}

function renderPluginList(filter) {
  const list = document.getElementById('plugin-list');
  const filtered = (filter
    ? cachedPlugins.filter(p => p.name.toLowerCase().includes(filter) || (p.manufacturer || '').toLowerCase().includes(filter))
    : cachedPlugins
  ).sort((a, b) => (a.name || '').localeCompare(b.name || '', undefined, { sensitivity: 'base' }));

  if (filtered.length === 0) {
    list.innerHTML = '<li class="no-projects">No plugins found.</li>';
    return;
  }

  list.innerHTML = '';
  for (const plugin of filtered) {
    const li = document.createElement('li');
    li.className = 'plugin-item';
    li.innerHTML = `
      <div>
        <div class="plugin-name">${escHTML(plugin.name)}</div>
        <div class="plugin-mfr">${escHTML(plugin.manufacturer || '')}</div>
      </div>
      <span class="plugin-format">${escHTML(plugin.format || '')}</span>
    `;
    li.onclick = async () => {
      if (!pluginTrack) return;
      const result = await engine.insertPlugin(pluginTrack.id, pluginSlotIndex, plugin.pluginId);
      if (result.error) {
        setStatus(`Plugin error: ${result.error}`);
      } else {
        pluginTrack.plugins.push({ slotIndex: pluginSlotIndex, pluginId: plugin.pluginId, name: plugin.name });
        await engine.openPluginEditor(pluginTrack.id, pluginSlotIndex);
        setStatus(`Loaded: ${plugin.name}`);
      }
      document.getElementById('plugin-modal').classList.remove('open');
    };
    list.appendChild(li);
  }
}

document.getElementById('btn-scan-plugins').onclick = async () => {
  setStatus('Scanning plugins...');
  const result = await engine.scanPlugins();
  if (result.partial) setStatus(`Found ${result.count} plugins (some failed to scan — run again to find more)`);
  else setStatus(`Found ${result.count} plugins`);
  const listResult = await engine.getPluginList();
  cachedPlugins = listResult.plugins || [];
  renderPluginList(document.getElementById('plugin-search').value.toLowerCase());
};

document.getElementById('btn-browse-plugins').onclick = async () => {
  setStatus('Select plugin directory...');
  const result = await engine.scanPluginDirectory();
  if (result.canceled) { setStatus('Ready'); return; }
  setStatus(`Found ${result.count} new plugins in ${result.directory}`);
  const listResult = await engine.getPluginList();
  cachedPlugins = listResult.plugins || [];
  renderPluginList(document.getElementById('plugin-search').value.toLowerCase());
};

document.getElementById('btn-close-plugins').onclick = () => {
  document.getElementById('plugin-modal').classList.remove('open');
  pluginTrack = null;
};

document.getElementById('plugin-modal').onclick = e => {
  if (e.target === e.currentTarget) {
    e.currentTarget.classList.remove('open');
    pluginTrack = null;
  }
};


// ─── Instrument Selection (MIDI tracks) ──────────────
let instrumentTrack = null;

const BUILTIN_INSTRUMENTS = [
  { name: 'Basic Synth', type: 'basicSynth', icon: '&#9835;', description: 'Subtractive synthesizer' },
  { name: 'Sample Player', type: 'samplePlayer', icon: '&#9654;', description: 'Pitched sample playback' },
  { name: 'Drum Kit', type: 'drumKit', icon: '&#9833;', description: '16-pad drum machine' },
];

const SFZ_INSTRUMENTS = [
  { id: 'grandPiano',      name: 'Grand Piano',       icon: '&#127929;', description: 'Acoustic piano' },
  { id: 'electricPiano',   name: 'Electric Piano',    icon: '&#127928;', description: 'FM Rhodes-style' },
  { id: 'pipeOrgan',       name: 'Pipe Organ',        icon: '&#9961;',   description: 'Drawbar organ' },
  { id: 'stringEnsemble',  name: 'String Ensemble',   icon: '&#127931;', description: 'Orchestral strings' },
  { id: 'brassSection',    name: 'Brass Section',     icon: '&#127930;', description: 'Brass ensemble' },
  { id: 'fingeredBass',    name: 'Fingered Bass',     icon: '&#127928;', description: 'Electric bass' },
  { id: 'nylonGuitar',     name: 'Nylon Guitar',      icon: '&#127928;', description: 'Classical guitar' },
  { id: 'synthPad',        name: 'Synth Pad',         icon: '&#9836;',   description: 'Super-saw pad' },
  { id: '808kit',          name: '808 Kit',            icon: '&#129345;', description: 'Classic TR-808 drum machine' },
  { id: 'flute',           name: 'Flute',             icon: '&#127926;', description: 'Breathy flute' },
  { id: 'squareLead',     name: 'Square Lead',       icon: '&#9633;',   description: 'Square wave synth lead' },
  { id: 'sawLead',        name: 'Saw Lead',          icon: '&#9651;',   description: 'Sawtooth synth lead' },
  { id: 'analogKit',      name: 'Analog Kit',        icon: '&#129345;', description: 'LinnDrum-inspired drums' },
  { id: 'cr78Kit',        name: 'CR-78 Kit',         icon: '&#129345;', description: 'Roland CR-78 drums' },
  { id: 'lm1Kit',         name: 'LM-1 Kit',          icon: '&#129345;', description: 'Linn LM-1 drums' },
];

async function openInstrumentModal(track) {
  try {
  instrumentTrack = track;
  const modal = document.getElementById('instrument-modal');

  // Close any open parameter panel so it doesn't overlap the modal
  document.getElementById('inst-param-panel').classList.remove('open');
  instParamTrack = null;

  // Reset tabs to Built-in
  modal.querySelectorAll('.inst-tab').forEach(t => t.classList.toggle('active', t.dataset.tab === 'builtin'));
  document.getElementById('inst-tab-builtin').classList.add('active');
  document.getElementById('inst-tab-vst').classList.remove('active');

  // Render SFZ software instrument cards
  const sfzGrid = document.getElementById('inst-sfz-grid');
  if (sfzGrid) {
    sfzGrid.innerHTML = '';
    for (const preset of SFZ_INSTRUMENTS) {
      const card = document.createElement('div');
      card.className = 'inst-card-sfz';
      card.innerHTML = `
        <div class="inst-card-icon">${preset.icon}</div>
        <div class="inst-card-name">${escHTML(preset.name)}</div>
        <div class="inst-card-desc">${escHTML(preset.description)}</div>
      `;
      card.onclick = async () => {
        try {
          if (!instrumentTrack) return;
          const trackRef = instrumentTrack;
          modal.classList.remove('open');
          setStatus(`Loading ${preset.name}...`);
          await engine.loadSFZPreset(trackRef.id, preset.id);
          await applySfzPresetDefaults(trackRef.id, preset.id);
          trackRef.instrumentName = preset.name;
          trackRef.instrumentType = 'sfzInstrument';
          trackRef.sfzPresetId = preset.id;
          setStatus(`Instrument: ${preset.name}`);
          renderAllTracks();
          await openInstParamPanel(trackRef);
        } catch (err) {
          console.error('SFZ instrument load error:', err);
          setStatus(`Error loading instrument: ${err.message}`);
        }
      };
      sfzGrid.appendChild(card);
    }

    // Add "Load SFZ File..." button
    const actionsDiv = document.getElementById('inst-sfz-actions');
    if (actionsDiv) {
      actionsDiv.innerHTML = '';
      const loadBtn = document.createElement('button');
      loadBtn.className = 'action-btn';
      loadBtn.textContent = 'Load SFZ File\u2026';
      loadBtn.onclick = async () => {
        if (!instrumentTrack) return;
        const trackRef = instrumentTrack;
        modal.classList.remove('open');
        const result = await engine.loadSFZFile(trackRef.id);
        if (result && !result.canceled && result.ok) {
          trackRef.instrumentName = 'Custom SFZ';
          trackRef.instrumentType = 'sfzInstrument';
          setStatus('Loaded SFZ instrument');
          renderAllTracks();
        } else {
          setStatus('SFZ file load cancelled or failed');
        }
      };
      actionsDiv.appendChild(loadBtn);
    }
  }

  // Load VST/AU instrument list
  const vstList = document.getElementById('inst-vst-list');
  const vstSearch = document.getElementById('inst-vst-search');
  vstSearch.value = '';
  vstList.innerHTML = '<li class="no-projects">Loading...</li>';
  engine.getPluginList().then(result => {
    const instruments = (result.plugins || []).filter(p => p.isInstrument);
    if (instruments.length === 0) {
      vstList.innerHTML = '<li class="no-projects">No instrument plugins found. Scan for plugins first.</li>';
    } else {
      renderInstVstList(instruments, '');
    }
    vstSearch.oninput = () => renderInstVstList(instruments, vstSearch.value.toLowerCase());
  }).catch(err => {
    console.error('Failed to load plugin list:', err);
    vstList.innerHTML = '<li class="no-projects">Failed to load plugin list.</li>';
  });

  modal.classList.add('open');
  } catch (err) {
    console.error('openInstrumentModal error:', err);
    setStatus(`Error opening instrument modal: ${err.message}`);
  }
}

function renderInstVstList(instruments, filter) {
  const list = document.getElementById('inst-vst-list');
  const filtered = (filter
    ? instruments.filter(p => p.name.toLowerCase().includes(filter) || (p.manufacturer || '').toLowerCase().includes(filter))
    : instruments
  ).sort((a, b) => (a.name || '').localeCompare(b.name || '', undefined, { sensitivity: 'base' }));
  list.innerHTML = '';
  if (filtered.length === 0) {
    list.innerHTML = '<li class="no-projects">No matches.</li>';
    return;
  }
  for (const plug of filtered) {
    const li = document.createElement('li');
    li.className = 'plugin-item';
    li.innerHTML = `<span class="plugin-name">${escHTML(plug.name || '')}</span><span class="plugin-mfr">${escHTML(plug.manufacturer || '')}</span>`;
    li.onclick = async () => {
      try {
        if (!instrumentTrack) return;
        const trackRef = instrumentTrack;
        document.getElementById('instrument-modal').classList.remove('open');
        setStatus(`Loading ${plug.name}...`);
        const result = await engine.setMidiTrackInstrument(trackRef.id, plug.pluginId);
        if (result && result.error) {
          setStatus(`Error: ${result.error}`);
          return;
        }
        trackRef.instrumentName = plug.name;
        trackRef.instrumentType = 'vstInstrument';
        trackRef.hasPluginInstrument = true;
        renderAllTracks();
        await engine.openMidiInstrumentEditor(trackRef.id);
        setStatus(`Instrument: ${plug.name}`);
      } catch (err) {
        console.error('VST instrument load error:', err);
        setStatus(`Error loading plugin: ${err.message}`);
      }
    };
    list.appendChild(li);
  }
}

// Instrument modal tab switching
document.querySelectorAll('#instrument-modal .inst-tab').forEach(tab => {
  tab.onclick = () => {
    document.querySelectorAll('#instrument-modal .inst-tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('#instrument-modal .inst-tab-content').forEach(c => c.classList.remove('active'));
    tab.classList.add('active');
    document.getElementById(`inst-tab-${tab.dataset.tab}`).classList.add('active');
  };
});

document.getElementById('btn-close-instruments').onclick = () => {
  document.getElementById('instrument-modal').classList.remove('open');
};
document.getElementById('instrument-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Built-in Instrument Parameter Panel ──────────
const SYNTH_PARAMS = [
  { section: 'Oscillators', params: [
    { label: 'Osc 1 Wave', param: 'osc1Waveform', min: 0, max: 3, step: 1, val: 1, unit: 'wave', names: ['Sine','Saw','Square','Triangle'] },
    { label: 'Osc 2 Wave', param: 'osc2Waveform', min: 0, max: 3, step: 1, val: 0, unit: 'wave', names: ['Sine','Saw','Square','Triangle'] },
    { label: 'Osc 2 Detune', param: 'osc2Detune', min: -24, max: 24, step: 0.1, val: 0, unit: 'st' },
    { label: 'Osc Mix', param: 'oscMix', min: 0, max: 1, step: 0.01, val: 0.5, unit: '' },
  ]},
  { section: 'Filter', params: [
    { label: 'Type', param: 'filterType', min: 0, max: 2, step: 1, val: 0, unit: 'ftype', names: ['LowPass','HighPass','BandPass'] },
    { label: 'Cutoff', param: 'filterCutoff', min: 20, max: 20000, step: 1, val: 8000, unit: 'Hz' },
    { label: 'Resonance', param: 'filterResonance', min: 0, max: 1, step: 0.01, val: 0.5, unit: '' },
  ]},
  { section: 'Amp Envelope', params: [
    { label: 'Attack', param: 'ampAttack', min: 0, max: 5, step: 0.01, val: 0.01, unit: 's' },
    { label: 'Decay', param: 'ampDecay', min: 0, max: 5, step: 0.01, val: 0.1, unit: 's' },
    { label: 'Sustain', param: 'ampSustain', min: 0, max: 1, step: 0.01, val: 0.8, unit: '' },
    { label: 'Release', param: 'ampRelease', min: 0, max: 10, step: 0.01, val: 0.3, unit: 's' },
  ]},
  { section: 'Filter Envelope', params: [
    { label: 'Attack', param: 'filterEnvAttack', min: 0, max: 5, step: 0.01, val: 0.01, unit: 's' },
    { label: 'Decay', param: 'filterEnvDecay', min: 0, max: 5, step: 0.01, val: 0.3, unit: 's' },
    { label: 'Sustain', param: 'filterEnvSustain', min: 0, max: 1, step: 0.01, val: 0.5, unit: '' },
    { label: 'Release', param: 'filterEnvRelease', min: 0, max: 10, step: 0.01, val: 0.3, unit: 's' },
    { label: 'Depth', param: 'filterEnvDepth', min: -1, max: 1, step: 0.01, val: 0.5, unit: '' },
  ]},
];

const SAMPLER_PARAMS = [
  { section: 'Sample', params: [
    { label: 'Root Note', param: 'rootNote', min: 0, max: 127, step: 1, val: 60, unit: 'note' },
    { label: 'Low Note', param: 'lowNote', min: 0, max: 127, step: 1, val: 0, unit: 'note' },
    { label: 'High Note', param: 'highNote', min: 0, max: 127, step: 1, val: 127, unit: 'note' },
  ]},
  { section: 'Envelope', params: [
    { label: 'Attack', param: 'ampAttack', min: 0, max: 5, step: 0.01, val: 0.01, unit: 's' },
    { label: 'Decay', param: 'ampDecay', min: 0, max: 5, step: 0.01, val: 0.1, unit: 's' },
    { label: 'Sustain', param: 'ampSustain', min: 0, max: 1, step: 0.01, val: 1.0, unit: '' },
    { label: 'Release', param: 'ampRelease', min: 0, max: 10, step: 0.01, val: 0.3, unit: 's' },
  ]},
  { section: 'Options', params: [
    { label: 'Velocity Sens.', param: 'velocitySensitivity', min: 0, max: 1, step: 0.01, val: 0.8, unit: '' },
  ]},
];

const DRUMKIT_PARAMS = [
  { section: 'Global', params: [
    { label: 'Velocity Sens.', param: 'velocitySensitivity', min: 0, max: 1, step: 0.01, val: 0.8, unit: '' },
  ]},
];

const SFZ_INSTRUMENT_PARAMS = [
  { section: 'Envelope', params: [
    { label: 'Attack', param: 'ampegAttack', min: 0, max: 5, step: 0.001, val: 0.005, unit: 's' },
    { label: 'Decay', param: 'ampegDecay', min: 0, max: 10, step: 0.01, val: 0.5, unit: 's' },
    { label: 'Sustain', param: 'ampegSustain', min: 0, max: 1, step: 0.01, val: 0.5, unit: '' },
    { label: 'Release', param: 'ampegRelease', min: 0, max: 10, step: 0.01, val: 0.3, unit: 's' },
  ]},
  { section: 'Output', params: [
    { label: 'Volume', param: 'masterVolume', min: 0, max: 2, step: 0.01, val: 1.0, unit: '' },
    { label: 'Velocity Sens.', param: 'velocitySensitivity', min: 0, max: 1, step: 0.01, val: 0.8, unit: '' },
  ]},
];

const SFZ_DRUMKIT_PARAMS = [
  { section: 'Envelope', params: [
    { label: 'Attack', param: 'ampegAttack', min: 0, max: 1, step: 0.001, val: 0.0, unit: 's' },
    { label: 'Release', param: 'ampegRelease', min: 0, max: 5, step: 0.01, val: 0.1, unit: 's' },
  ]},
  { section: 'Output', params: [
    { label: 'Volume', param: 'masterVolume', min: 0, max: 2, step: 0.01, val: 1.0, unit: '' },
    { label: 'Velocity Sens.', param: 'velocitySensitivity', min: 0, max: 1, step: 0.01, val: 0.8, unit: '' },
  ]},
];

const SFZ_DRUM_IDS = ['808kit', 'analogKit', 'cr78Kit', 'lm1Kit'];

// Per-instrument default parameters (applied after loading preset)
const SFZ_PRESET_DEFAULTS = {
  // ── Percussive (naturally decaying) ──
  grandPiano:      { ampegAttack: 0.005, ampegDecay: 8.0,  ampegSustain: 0,    ampegRelease: 0.4,  masterVolume: 1.0,  velocitySensitivity: 0.9  },
  electricPiano:   { ampegAttack: 0.002, ampegDecay: 5.0,  ampegSustain: 0.05, ampegRelease: 0.3,  masterVolume: 1.0,  velocitySensitivity: 0.85 },
  fingeredBass:    { ampegAttack: 0.005, ampegDecay: 1.5,  ampegSustain: 0,    ampegRelease: 0.1,  masterVolume: 1.0,  velocitySensitivity: 0.7  },
  nylonGuitar:     { ampegAttack: 0.005, ampegDecay: 2.5,  ampegSustain: 0,    ampegRelease: 0.2,  masterVolume: 1.0,  velocitySensitivity: 0.75 },
  // ── Sustained (looping) ──
  pipeOrgan:       { ampegAttack: 0.02,  ampegDecay: 0,    ampegSustain: 1.0,  ampegRelease: 0.1,  masterVolume: 0.9,  velocitySensitivity: 0.2  },
  stringEnsemble:  { ampegAttack: 0.3,   ampegDecay: 0,    ampegSustain: 1.0,  ampegRelease: 0.5,  masterVolume: 1.0,  velocitySensitivity: 0.6  },
  brassSection:    { ampegAttack: 0.05,  ampegDecay: 0.5,  ampegSustain: 0.8,  ampegRelease: 0.2,  masterVolume: 1.0,  velocitySensitivity: 0.8  },
  synthPad:        { ampegAttack: 1.0,   ampegDecay: 0,    ampegSustain: 1.0,  ampegRelease: 2.0,  masterVolume: 0.9,  velocitySensitivity: 0.3  },
  flute:           { ampegAttack: 0.08,  ampegDecay: 0,    ampegSustain: 1.0,  ampegRelease: 0.15, masterVolume: 1.0,  velocitySensitivity: 0.6  },
  squareLead:      { ampegAttack: 0.005, ampegDecay: 0.1,  ampegSustain: 0.8,  ampegRelease: 0.1,  masterVolume: 0.85, velocitySensitivity: 0.5  },
  sawLead:         { ampegAttack: 0.005, ampegDecay: 0,    ampegSustain: 1.0,  ampegRelease: 0.15, masterVolume: 0.85, velocitySensitivity: 0.5  },
  // ── Drum kits (only volume/velocity; per-drum envelopes stay intact) ──
  '808kit':        { masterVolume: 1.0,  velocitySensitivity: 0.8  },
  analogKit:       { masterVolume: 1.0,  velocitySensitivity: 0.8  },
  cr78Kit:         { masterVolume: 1.0,  velocitySensitivity: 0.6  },
  lm1Kit:          { masterVolume: 1.0,  velocitySensitivity: 0.75 },
};

async function applySfzPresetDefaults(trackId, presetId) {
  const defaults = SFZ_PRESET_DEFAULTS[presetId];
  if (!defaults) return;
  for (const [param, value] of Object.entries(defaults)) {
    try { await engine.setBuiltInSynthParam(trackId, param, value); } catch (_) {}
  }
}

function getParamDefsForInstrument(track) {
  const type = track.instrumentType;
  if (type === 'basicSynth') return SYNTH_PARAMS;
  if (type === 'samplePlayer') return SAMPLER_PARAMS;
  if (type === 'drumKit') return DRUMKIT_PARAMS;
  if (type === 'sfzInstrument') {
    return SFZ_DRUM_IDS.includes(track.sfzPresetId) ? SFZ_DRUMKIT_PARAMS : SFZ_INSTRUMENT_PARAMS;
  }
  return [];
}

const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
function midiNoteName(n) { return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1); }

function formatInstVal(val, unit, p) {
  if (unit === 'Hz') return Math.round(val) + ' Hz';
  if (unit === 's') return val >= 0.01 ? val.toFixed(2) + 's' : (val * 1000).toFixed(0) + 'ms';
  if (unit === 'st') return val.toFixed(1) + ' st';
  if (unit === 'note') return midiNoteName(Math.round(val));
  if (unit === 'wave' && p.names) return p.names[Math.round(val)] || String(Math.round(val));
  if (unit === 'ftype' && p.names) return p.names[Math.round(val)] || String(Math.round(val));
  return val.toFixed(2);
}

let instParamTrack = null;

async function openInstParamPanel(track) {
  instParamTrack = track;
  const panel = document.getElementById('inst-param-panel');
  const body = document.getElementById('inst-param-body');
  document.getElementById('inst-param-title').textContent = track.instrumentName || 'Instrument';

  // Bind close button each time panel opens
  const closeBtn = document.getElementById('inst-param-close');
  if (closeBtn) {
    closeBtn.onclick = (e) => {
      e.stopPropagation();
      panel.classList.remove('open');
      instParamTrack = null;
    };
  }

  const defs = getParamDefsForInstrument(track);
  if (defs.length === 0) {
    body.innerHTML = '<div style="color:var(--text-dim);font-size:12px;padding:16px 0;">No editable parameters for this instrument.</div>';
    panel.classList.add('open');
    return;
  }

  // Read actual parameter values from engine
  const liveVals = {};
  for (const section of defs) {
    for (const p of section.params) {
      try {
        liveVals[p.param] = await engine.getBuiltInSynthParam(track.id, p.param);
      } catch (_) {
        liveVals[p.param] = p.val;
      }
    }
  }

  body.innerHTML = defs.map(section => `
    <div class="fx-section">
      <div class="fx-section-header"><h4>${section.section}</h4></div>
      <div class="fx-section-body">
        ${section.params.map(p => {
          const live = liveVals[p.param] != null ? liveVals[p.param] : p.val;
          const effectiveMax = Math.max(p.max, live);
          return `
          <div class="fx-param">
            <label>${p.label}</label>
            <input type="range" min="${p.min}" max="${effectiveMax}" step="${p.step}" value="${live}"
                   data-inst-param="${p.param}" data-unit="${p.unit}"
                   ${p.names ? `data-names="${p.names.join(',')}"` : ''}>
            <span class="fx-val">${formatInstVal(live, p.unit, p)}</span>
          </div>`;
        }).join('')}
      </div>
    </div>
  `).join('');

  body.querySelectorAll('input[data-inst-param]').forEach(slider => {
    slider.oninput = () => {
      const val = parseFloat(slider.value);
      const param = slider.dataset.instParam;
      const unit = slider.dataset.unit;
      const names = slider.dataset.names ? slider.dataset.names.split(',') : null;
      const p = { names };
      slider.parentElement.querySelector('.fx-val').textContent = formatInstVal(val, unit, p);
      // Throttle IPC: one call per param per animation frame
      if (slider._rafId) cancelAnimationFrame(slider._rafId);
      slider._rafId = requestAnimationFrame(() => {
        slider._rafId = null;
        engine.setBuiltInSynthParam(track.id, param, parseFloat(slider.value));
      });
    };
  });

  panel.classList.add('open');
}

document.getElementById('inst-param-close').onclick = () => {
  document.getElementById('inst-param-panel').classList.remove('open');
  instParamTrack = null;
};

async function openMidiPluginModal(track) {
  // Reuse the plugin modal but for effect plugins on a MIDI track
  pluginTrack = track;
  pluginSlotIndex = track.plugins.length;
  const modal = document.getElementById('plugin-modal');
  const list = document.getElementById('plugin-list');
  const search = document.getElementById('plugin-search');
  search.value = '';

  list.innerHTML = '<li class="no-projects">Loading effects...</li>';
  modal.classList.add('open');

  const result = await engine.getPluginList();
  const effects = (result.plugins || []).filter(p => !p.isInstrument);
  renderMidiEffectList(track, effects, '');
  search.oninput = () => renderMidiEffectList(track, effects, search.value.toLowerCase());
}

function renderMidiEffectList(track, effects, filter) {
  const list = document.getElementById('plugin-list');
  const filtered = (filter
    ? effects.filter(p => p.name.toLowerCase().includes(filter) || (p.manufacturer || '').toLowerCase().includes(filter))
    : effects
  ).sort((a, b) => (a.name || '').localeCompare(b.name || '', undefined, { sensitivity: 'base' }));

  if (filtered.length === 0) {
    list.innerHTML = '<li class="no-projects">No effects found.</li>';
    return;
  }

  list.innerHTML = '';
  for (const plugin of filtered) {
    const li = document.createElement('li');
    li.className = 'plugin-item';
    li.innerHTML = `
      <div>
        <div class="plugin-name">${escHTML(plugin.name)}</div>
        <div class="plugin-mfr">${escHTML(plugin.manufacturer || '')}</div>
      </div>
      <span class="plugin-format">${escHTML(plugin.format || '')}</span>
    `;
    li.onclick = async () => {
      // MIDI track effect slots start at index 1 (0 is instrument)
      const slotIdx = track.plugins.length + 1;
      const r = await engine.insertPlugin(track.id, slotIdx, plugin.pluginId);
      if (r.error) {
        setStatus(`Plugin error: ${r.error}`);
      } else {
        track.plugins.push({ slotIndex: slotIdx, pluginId: plugin.pluginId, name: plugin.name });
        setStatus(`FX: ${plugin.name}`);
      }
      document.getElementById('plugin-modal').classList.remove('open');
    };
    list.appendChild(li);
  }
}


// ─── Insert Chain ─────────────────────────────────────
async function refreshInsertChain(row, track) {
  const chain = row.querySelector('.insert-chain');
  if (!chain) return;

  let info;
  if (track.type === 'midi') info = await engine.getMidiInsertChainInfo(track.id);
  else if (track.type === 'bus') info = await engine.getBusInsertChainInfo(track.id);
  else info = await engine.getInsertChainInfo(track.id);

  if (!info || !Array.isArray(info)) return;

  const slots = chain.querySelectorAll('.insert-slot');
  slots.forEach((slot, i) => {
    const data = info[i];
    // Hide system limiter on MIDI tracks (slot 4)
    if (track._hasSystemLimiter && i === 4 && data && data.isBuiltIn && data.effectType === 'limiter') {
      slot.style.display = 'none';
      return;
    }
    slot.style.display = '';
    const nameEl = slot.querySelector('.insert-name');
    const addEl = slot.querySelector('.insert-add');
    const existingRemove = slot.querySelector('.insert-remove');
    if (existingRemove) existingRemove.remove();

    if (data && data.name) {
      slot.classList.add('occupied');
      slot.classList.toggle('native', data.isBuiltIn);
      nameEl.textContent = data.name;
      nameEl.style.cursor = 'pointer';
      addEl.style.display = 'none';

      // Add remove button
      const removeBtn = document.createElement('span');
      removeBtn.className = 'insert-remove';
      removeBtn.textContent = '\u00d7';
      removeBtn.onclick = async (e) => {
        e.stopPropagation();
        if (track.type === 'midi') await engine.removeMidiInsert(track.id, i);
        else if (track.type === 'bus') await engine.removeBusInsert(track.id, i);
        else await engine.removeInsert(track.id, i);
        refreshInsertChain(row, track);
      };
      slot.appendChild(removeBtn);

      // Click name to open params (native) or plugin editor (VST)
      nameEl.onclick = (e) => {
        e.stopPropagation();
        if (data.isBuiltIn && data.effectType) {
          openInsertParamPanel(track, i, data.effectType, data.name);
        } else {
          // VST: open plugin editor
          if (track.type !== 'bus') engine.openPluginEditor(track.id, i);
        }
      };
    } else {
      slot.classList.remove('occupied', 'native');
      nameEl.textContent = '— empty —';
      nameEl.style.cursor = 'default';
      nameEl.onclick = null;
      addEl.style.display = '';
      addEl.onclick = (e) => {
        e.stopPropagation();
        openInsertPickerModal(track, i);
      };
    }
  });
}

// ─── Insert Picker Modal ──────────────────────────────
let insertPickerTarget = null;
let insertPickerSlot = 0;

async function openInsertPickerModal(track, slotIndex) {
  insertPickerTarget = track;
  insertPickerSlot = slotIndex;
  const modal = document.getElementById('insert-picker-modal');
  const grid = document.getElementById('insert-native-grid');
  const vstList = document.getElementById('insert-vst-list');
  const vstSearch = document.getElementById('insert-vst-search');

  // Set active tab to native
  modal.querySelectorAll('.insert-tab').forEach(t => t.classList.toggle('active', t.dataset.tab === 'native'));
  document.getElementById('insert-tab-native').classList.add('active');
  document.getElementById('insert-tab-vst').classList.remove('active');

  // Render native effects grid
  grid.innerHTML = '';
  const effectTypes = await engine.getBuiltInEffectTypes();
  for (const fx of effectTypes) {
    const card = document.createElement('div');
    card.className = 'insert-native-card';
    card.innerHTML = `
      <div class="insert-native-name">${escHTML(fx.name)}</div>
      <div class="insert-native-cat">${escHTML(fx.category)}</div>
    `;
    card.onclick = async () => {
      if (!insertPickerTarget) return;
      if (insertPickerTarget.type === 'master') {
        await engine.insertMasterBuiltInEffect(insertPickerSlot, fx.type);
      } else if (insertPickerTarget.type === 'midi') {
        await engine.insertMidiBuiltInEffect(insertPickerTarget.id, insertPickerSlot, fx.type);
      } else if (insertPickerTarget.type === 'bus') {
        await engine.insertBusBuiltInEffect(insertPickerTarget.id, insertPickerSlot, fx.type);
      } else {
        await engine.insertBuiltInEffect(insertPickerTarget.id, insertPickerSlot, fx.type);
      }
      modal.classList.remove('open');
      // Refresh the insert chain on the track row (not for master)
      if (insertPickerTarget.type !== 'master') {
        const row = document.querySelector(`.track-row[data-id="${insertPickerTarget.id}"]`);
        if (row) await refreshInsertChain(row, insertPickerTarget);
      }
      // Refresh mixer inserts
      if (typeof mixerBoard !== 'undefined' && mixerBoard.isOpen) {
        mixerBoard._loadAllInserts();
      }
      // Auto-open the param panel for the newly inserted effect
      openInsertParamPanel(insertPickerTarget, insertPickerSlot, fx.type, fx.name);
      setStatus(`Inserted: ${fx.name}`);
    };
    grid.appendChild(card);
  }

  // Fetch VST effects
  vstSearch.value = '';
  vstList.innerHTML = '<li class="no-projects">Loading...</li>';

  modal.classList.add('open');

  engine.getPluginList().then(result => {
    const effects = (result.plugins || []).filter(p => !p.isInstrument);
    if (effects.length === 0) {
      vstList.innerHTML = '<li class="no-projects">No effect plugins found. Scan for plugins first.</li>';
    } else {
      renderInsertVstList(effects, '');
    }
    vstSearch.oninput = () => renderInsertVstList(effects, vstSearch.value.toLowerCase());
  });
}

function renderInsertVstList(effects, filter) {
  const list = document.getElementById('insert-vst-list');
  const filtered = (filter
    ? effects.filter(p => p.name.toLowerCase().includes(filter) || (p.manufacturer || '').toLowerCase().includes(filter))
    : effects
  ).sort((a, b) => (a.name || '').localeCompare(b.name || '', undefined, { sensitivity: 'base' }));

  if (filtered.length === 0) {
    list.innerHTML = '<li class="no-projects">No effects found.</li>';
    return;
  }

  list.innerHTML = '';
  for (const plugin of filtered) {
    const li = document.createElement('li');
    li.className = 'plugin-item';
    li.innerHTML = `
      <div>
        <div class="plugin-name">${escHTML(plugin.name)}</div>
        <div class="plugin-mfr">${escHTML(plugin.manufacturer || '')}</div>
      </div>
      <span class="plugin-format">${escHTML(plugin.format || '')}</span>
    `;
    li.onclick = async () => {
      if (!insertPickerTarget) return;
      let result;
      if (insertPickerTarget.type === 'master') {
        result = await engine.insertMasterPlugin(insertPickerSlot, plugin.pluginId);
      } else if (insertPickerTarget.type === 'midi') {
        result = await engine.insertMidiPlugin(insertPickerTarget.id, insertPickerSlot, plugin.pluginId);
      } else if (insertPickerTarget.type === 'bus') {
        result = await engine.insertBusPlugin(insertPickerTarget.id, insertPickerSlot, plugin.pluginId);
      } else {
        result = await engine.insertPlugin(insertPickerTarget.id, insertPickerSlot, plugin.pluginId);
      }
      if (result && result.error) {
        setStatus(`Plugin error: ${result.error}`);
      } else {
        setStatus(`Inserted: ${plugin.name}`);
        // Auto-open the plugin editor window
        try {
          if (insertPickerTarget.type === 'midi') {
            await engine.openMidiPluginEditor(insertPickerTarget.id, insertPickerSlot);
          } else if (insertPickerTarget.type !== 'master' && insertPickerTarget.type !== 'bus') {
            await engine.openPluginEditor(insertPickerTarget.id, insertPickerSlot);
          }
        } catch (e) { /* editor open is best-effort */ }
      }
      document.getElementById('insert-picker-modal').classList.remove('open');
      const _pickerTrack = insertPickerTarget;
      if (_pickerTrack.type !== 'master') {
        const row = document.querySelector(`.track-row[data-id="${_pickerTrack.id}"]`);
        if (row) {
          await refreshInsertChain(row, _pickerTrack);
          // Re-refresh after short delay for async VST loads that may resolve late
          setTimeout(() => refreshInsertChain(row, _pickerTrack), 500);
        }
      }
      if (typeof mixerBoard !== 'undefined' && mixerBoard.isOpen) {
        mixerBoard._loadAllInserts();
        setTimeout(() => mixerBoard._loadAllInserts(), 500);
      }
    };
    list.appendChild(li);
  }
}

// Insert picker tab switching
document.querySelectorAll('.insert-tab').forEach(tab => {
  tab.onclick = () => {
    document.querySelectorAll('.insert-tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.insert-tab-content').forEach(c => c.classList.remove('active'));
    tab.classList.add('active');
    document.getElementById(`insert-tab-${tab.dataset.tab}`).classList.add('active');
  };
});

document.getElementById('btn-close-insert-picker').onclick = () => {
  document.getElementById('insert-picker-modal').classList.remove('open');
};
document.getElementById('insert-picker-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Insert Effect Parameter Panel ────────────────────

const INSERT_EFFECT_PARAMS = {
  gate: [
    { label:'Threshold', param:'threshold', min:-60, max:0, step:1, unit:'dB' },
    { label:'Ratio', param:'ratio', min:1, max:20, step:0.5, unit:':1' },
    { label:'Attack', param:'attack', min:0.1, max:100, step:0.1, unit:'ms' },
    { label:'Release', param:'release', min:1, max:500, step:1, unit:'ms' },
  ],
  eq: [
    { label:'Low Gain', param:'lowGain', min:-12, max:12, step:0.5, unit:'dB' },
    { label:'Mid Gain', param:'midGain', min:-12, max:12, step:0.5, unit:'dB' },
    { label:'Mid Freq', param:'midFreq', min:200, max:8000, step:10, unit:'Hz' },
    { label:'High Gain', param:'highGain', min:-12, max:12, step:0.5, unit:'dB' },
  ],
  compressor: [
    { label:'Threshold', param:'threshold', min:-60, max:0, step:1, unit:'dB' },
    { label:'Ratio', param:'ratio', min:1, max:20, step:0.5, unit:':1' },
    { label:'Attack', param:'attack', min:0.1, max:100, step:0.1, unit:'ms' },
    { label:'Release', param:'release', min:1, max:500, step:1, unit:'ms' },
  ],
  distortion: [
    { label:'Drive', param:'drive', min:0.1, max:10, step:0.1, unit:'x' },
    { label:'Mix', param:'mix', min:0, max:1, step:0.01, unit:'%' },
  ],
  filter: [
    { label:'Mode', param:'mode', type:'select', options:['Lowpass','Highpass','Bandpass'] },
    { label:'Cutoff', param:'cutoff', min:20, max:20000, step:1, unit:'Hz' },
    { label:'Resonance', param:'resonance', min:0, max:1, step:0.01, unit:'%' },
  ],
  chorus: [
    { label:'Rate', param:'rate', min:0.1, max:10, step:0.1, unit:'Hz' },
    { label:'Depth', param:'depth', min:0, max:1, step:0.01, unit:'%' },
    { label:'Centre Delay', param:'centreDelay', min:1, max:30, step:0.5, unit:'ms' },
    { label:'Feedback', param:'feedback', min:0, max:0.95, step:0.01, unit:'%' },
    { label:'Mix', param:'mix', min:0, max:1, step:0.01, unit:'%' },
  ],
  phaser: [
    { label:'Rate', param:'rate', min:0.1, max:10, step:0.1, unit:'Hz' },
    { label:'Depth', param:'depth', min:0, max:1, step:0.01, unit:'%' },
    { label:'Centre Freq', param:'centreFrequency', min:200, max:5000, step:10, unit:'Hz' },
    { label:'Feedback', param:'feedback', min:0, max:0.95, step:0.01, unit:'%' },
    { label:'Mix', param:'mix', min:0, max:1, step:0.01, unit:'%' },
  ],
  delay: [
    { label:'Time', param:'time', min:0.01, max:2, step:0.01, unit:'s' },
    { label:'Feedback', param:'feedback', min:0, max:0.95, step:0.01, unit:'%' },
    { label:'Mix', param:'mix', min:0, max:1, step:0.01, unit:'%' },
  ],
  reverb: [
    { label:'Room Size', param:'size', min:0, max:1, step:0.01, unit:'%' },
    { label:'Damping', param:'damping', min:0, max:1, step:0.01, unit:'%' },
    { label:'Width', param:'width', min:0, max:1, step:0.01, unit:'%' },
    { label:'Mix', param:'mix', min:0, max:1, step:0.01, unit:'%' },
  ],
  limiter: [
    { label:'Threshold', param:'threshold', min:-30, max:0, step:0.5, unit:'dB' },
    { label:'Release', param:'release', min:1, max:500, step:1, unit:'ms' },
  ],
};

function openInsertParamPanel(track, slotIndex, effectType, effectName) {
  const EFFECT_ICONS = {
    gate:'G', eq:'EQ', compressor:'C', distortion:'D', filter:'F',
    chorus:'CH', phaser:'PH', delay:'DL', reverb:'RV', limiter:'L'
  };
  const EFFECT_CATS = {
    gate:'Dynamics', eq:'EQ', compressor:'Dynamics', distortion:'Saturation', filter:'Filter',
    chorus:'Modulation', phaser:'Modulation', delay:'Time', reverb:'Time', limiter:'Dynamics'
  };

  const params = INSERT_EFFECT_PARAMS[effectType] || [];
  const winId = `fx-win-${track.id}-${slotIndex}`;

  // If window already exists, bring it to front
  const existing = document.getElementById(winId);
  if (existing) { existing.style.zIndex = ++_fxWinZ; return; }

  const win = document.createElement('div');
  win.id = winId;
  win.className = 'fx-float-win';
  win.style.zIndex = ++_fxWinZ;

  // Position near center, offset by number of open windows
  const openCount = document.querySelectorAll('.fx-float-win').length;
  win.style.left = (200 + openCount * 24) + 'px';
  win.style.top = (120 + openCount * 24) + 'px';

  const titleText = effectName || effectType;
  const catText = EFFECT_CATS[effectType] || '';
  const iconText = EFFECT_ICONS[effectType] || '?';

  let paramsHTML;
  if (params.length === 0) {
    paramsHTML = '<div style="color:var(--text-dim);font-size:12px;padding:12px 0;">No parameters.</div>';
  } else {
    paramsHTML = params.map(p => {
      if (p.type === 'select') {
        return `<div class="fx-param">
          <label>${p.label}</label>
          <select data-insert-param="${p.param}" class="fx-select">
            ${p.options.map((opt, idx) => `<option value="${idx}">${opt}</option>`).join('')}
          </select>
        </div>`;
      }
      return `<div class="fx-param">
        <label>${p.label}</label>
        <input type="range" min="${p.min}" max="${p.max}" step="${p.step}" value="${p.min}"
               data-insert-param="${p.param}" data-unit="${p.unit}">
        <span class="fx-val">${formatFxVal(p.min, p.unit)}</span>
      </div>`;
    }).join('');
  }

  win.innerHTML = `
    <div class="fx-float-titlebar">
      <span class="fx-float-icon">${iconText}</span>
      <span class="fx-float-title">${titleText}</span>
      <span class="fx-float-cat">${catText}</span>
      <button class="fx-float-close">&times;</button>
    </div>
    <div class="fx-float-body">${paramsHTML}</div>
  `;

  document.body.appendChild(win);

  // Close button
  win.querySelector('.fx-float-close').onclick = () => win.remove();

  // Bring to front on click
  win.addEventListener('mousedown', () => { win.style.zIndex = ++_fxWinZ; });

  // Draggable titlebar
  const titlebar = win.querySelector('.fx-float-titlebar');
  titlebar.addEventListener('mousedown', (e) => {
    if (e.target.classList.contains('fx-float-close')) return;
    e.preventDefault();
    const startX = e.clientX, startY = e.clientY;
    const origLeft = win.offsetLeft, origTop = win.offsetTop;
    const onMove = (ev) => {
      win.style.left = (origLeft + ev.clientX - startX) + 'px';
      win.style.top = (origTop + ev.clientY - startY) + 'px';
    };
    const onUp = () => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });

  // Load current values and bind controls
  win.querySelectorAll('[data-insert-param]').forEach(async (ctrl) => {
    const paramName = ctrl.dataset.insertParam;
    let val;
    if (track.type === 'master') val = await engine.getMasterInsertEffectParam(slotIndex, paramName);
    else if (track.type === 'midi') val = await engine.getMidiInsertEffectParam(track.id, slotIndex, paramName);
    else if (track.type === 'bus') val = await engine.getBusInsertEffectParam(track.id, slotIndex, paramName);
    else val = await engine.getInsertEffectParam(track.id, slotIndex, paramName);

    if (ctrl.tagName === 'SELECT') {
      if (typeof val === 'number') ctrl.value = Math.round(val);
      ctrl.onchange = async () => {
        const v = parseFloat(ctrl.value);
        if (track.type === 'master') await engine.setMasterInsertEffectParam(slotIndex, paramName, v);
        else if (track.type === 'midi') await engine.setMidiInsertEffectParam(track.id, slotIndex, paramName, v);
        else if (track.type === 'bus') await engine.setBusInsertEffectParam(track.id, slotIndex, paramName, v);
        else await engine.setInsertEffectParam(track.id, slotIndex, paramName, v);
      };
    } else {
      if (typeof val === 'number') {
        ctrl.value = val;
        ctrl.parentElement.querySelector('.fx-val').textContent = formatFxVal(val, ctrl.dataset.unit);
      }
      ctrl.oninput = () => {
        const v = parseFloat(ctrl.value);
        ctrl.parentElement.querySelector('.fx-val').textContent = formatFxVal(v, ctrl.dataset.unit);
        // Throttle IPC: one call per param per animation frame
        if (ctrl._rafId) cancelAnimationFrame(ctrl._rafId);
        ctrl._rafId = requestAnimationFrame(() => {
          ctrl._rafId = null;
          const lv = parseFloat(ctrl.value);
          if (track.type === 'master') engine.setMasterInsertEffectParam(slotIndex, paramName, lv);
          else if (track.type === 'midi') engine.setMidiInsertEffectParam(track.id, slotIndex, paramName, lv);
          else if (track.type === 'bus') engine.setBusInsertEffectParam(track.id, slotIndex, paramName, lv);
          else engine.setInsertEffectParam(track.id, slotIndex, paramName, lv);
        });
      };
    }
  });
}
let _fxWinZ = 2000;


// ─── Import / Export ──────────────────────────────────
document.getElementById('btn-import').onclick = async () => {
  const result = await engine.importAudioFile();
  if (result.canceled) return;
  for (const imported of (result.tracks || [])) {
    // Find or create the track in our local model
    let track = tracks.find(t => t.id === imported.trackId);
    if (!track) {
      track = {
        id: imported.trackId,
        type: 'audio',
        name: imported.fileName.replace(/\.[^/.]+$/, ''),
        color: TRACK_COLORS[imported.trackId % TRACK_COLORS.length],
        volume: 0.8, pan: 0, muted: false, solo: false, armed: false,
        hasBuff: true, duration: imported.duration || 0,
        waveformData: imported.waveform ? new Float32Array(imported.waveform) : null,
        peakLevel: 0, peakHoldTime: 0, clipping: false,
        fx: { eqEnabled: false, compEnabled: false, delayEnabled: false,
              eqLowGain: 0, eqMidGain: 0, eqMidFreq: 1000, eqHighGain: 0,
              compThreshold: -24, compRatio: 4, compAttack: 0.003, compRelease: 0.25,
              delayTime: 0, delayMix: 0, delayFeedback: 0.3 },
        plugins: [],
        regions: [], _hiddenNativeIds: [],
        get region() { return this.regions[0] || null; },
        set region(v) {
          if (v === null) { this.regions = []; }
          else if (this.regions.length === 0) { this.regions.push(v); }
          else { this.regions[0] = v; }
        }
      };
      trackIdCounter = Math.max(trackIdCounter, imported.trackId + 1);
      tracks.push(track);
    } else {
      track.hasBuff = true;
      if (imported.duration) track.duration = imported.duration;
      if (imported.waveform) track.waveformData = new Float32Array(imported.waveform);
    }
    // Set up region
    if (track.hasBuff && track.duration > 0) {
      track.region = { nativeTrackId: track.id, offset: 0, clipStart: 0, clipEnd: track.duration, loopEnabled: false, loopCount: 0, fadeIn: 0, fadeOut: 0, waveformData: track.waveformData, duration: track.duration };
    }
  }
  renderAllTracks();
  // Re-render after layout settles to ensure waveform canvases have dimensions
  setTimeout(() => renderAllTracks(), 100);
  setStatus('Imported audio');
};

document.getElementById('btn-export').onclick = () => {
  if (tracks.length === 0) {
    setStatus('Nothing to export.');
    return;
  }
  document.getElementById('export-progress').style.display = 'none';
  document.getElementById('export-progress-fill').style.width = '0%';
  document.getElementById('export-progress-text').textContent = 'Exporting...';
  document.getElementById('btn-do-export').disabled = false;
  document.getElementById('export-modal').classList.add('open');
};

document.getElementById('export-format').onchange = () => {
  const fmt = document.getElementById('export-format').value;
  document.getElementById('export-bitdepth-row').style.display = (fmt === 'mp3') ? 'none' : '';
  document.getElementById('export-bitrate-row').style.display = (fmt === 'mp3') ? '' : 'none';
};

document.getElementById('btn-do-export').onclick = async () => {
  const exportStems = document.getElementById('export-stems').checked;
  const exportMixdown = document.getElementById('export-mixdown').checked;
  const format = document.getElementById('export-format').value;
  const bitDepth = parseInt(document.getElementById('export-bitdepth').value);
  const mp3Bitrate = parseInt(document.getElementById('export-bitrate').value);

  if (!exportStems && !exportMixdown) {
    setStatus('Select at least one export option.');
    return;
  }

  const progressDiv = document.getElementById('export-progress');
  const progressFill = document.getElementById('export-progress-fill');
  const progressText = document.getElementById('export-progress-text');
  const exportBtn = document.getElementById('btn-do-export');

  progressDiv.style.display = '';
  progressFill.style.width = '0%';
  progressText.textContent = 'Exporting...';
  exportBtn.disabled = true;

  const result = await engine.exportStems({
    exportStems, exportMixdown, bitDepth, format, mp3Bitrate
  });

  if (result && result.canceled) {
    progressDiv.style.display = 'none';
    exportBtn.disabled = false;
    setStatus('Export canceled.');
  } else if (result && result.error) {
    progressText.textContent = `Error: ${result.error}`;
    exportBtn.disabled = false;
    setStatus('Export failed.');
  } else {
    progressFill.style.width = '100%';
    progressText.textContent = 'Export complete!';
    setStatus('Export complete.');
  }
};

document.getElementById('btn-close-export').onclick = () => {
  document.getElementById('export-modal').classList.remove('open');
};
document.getElementById('export-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};


// ─── Bounce Modal ──
document.getElementById('btn-bounce').onclick = () => {
  if (tracks.length === 0) { setStatus('Nothing to bounce.'); return; }
  document.getElementById('bounce-progress').style.display = 'none';
  document.getElementById('btn-do-bounce').disabled = false;
  document.getElementById('bounce-modal').classList.add('open');
};

document.getElementById('bounce-mp3').onchange = () => {
  document.getElementById('bounce-mp3-bitrate-row').style.display =
    document.getElementById('bounce-mp3').checked ? '' : 'none';
};

document.getElementById('btn-do-bounce').onclick = async () => {
  const format = document.getElementById('bounce-format').value;
  const bitDepth = parseInt(document.getElementById('bounce-bitdepth').value);
  const alsoMp3 = document.getElementById('bounce-mp3').checked;
  const mp3Bitrate = parseInt(document.getElementById('bounce-mp3-bitrate').value);
  const normalize = document.getElementById('bounce-normalize').checked;

  const bounceBtn = document.getElementById('btn-do-bounce');
  const progressDiv = document.getElementById('bounce-progress');
  const progressFill = document.getElementById('bounce-progress-fill');
  const progressText = document.getElementById('bounce-progress-text');

  bounceBtn.disabled = true;
  progressDiv.style.display = '';
  progressFill.style.width = '50%';
  progressText.textContent = 'Bouncing...';

  const result = await engine.bounceProject({ format, bitDepth, alsoMp3, mp3Bitrate, normalize });

  if (result && result.canceled) {
    progressDiv.style.display = 'none';
    bounceBtn.disabled = false;
    setStatus('Bounce canceled.');
  } else if (result && !result.ok) {
    progressText.textContent = `Error: ${result.error}`;
    bounceBtn.disabled = false;
    setStatus('Bounce failed.');
  } else {
    progressFill.style.width = '100%';
    let msg = 'Bounce complete!';
    if (result.mp3Path) msg += ' (+ MP3)';
    progressText.textContent = msg;
    setStatus(`Bounced: ${result.path}`);
  }
};

document.getElementById('btn-close-bounce').onclick = () => {
  document.getElementById('bounce-modal').classList.remove('open');
};
document.getElementById('bounce-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Functions Dropdown ──
let _functionsDropdown = null;

function closeFunctionsDropdown() {
  if (_functionsDropdown) {
    _functionsDropdown.remove();
    _functionsDropdown = null;
  }
  document.removeEventListener('click', _functionsOutsideClick);
}

function _functionsOutsideClick(e) {
  if (_functionsDropdown && !_functionsDropdown.contains(e.target) && e.target.id !== 'btn-functions') {
    closeFunctionsDropdown();
  }
}

document.getElementById('btn-functions').onclick = (e) => {
  if (_functionsDropdown) { closeFunctionsDropdown(); return; }

  const btn = e.currentTarget;
  const rect = btn.getBoundingClientRect();

  const dropdown = document.createElement('div');
  dropdown.className = 'functions-dropdown';
  dropdown.style.left = rect.left + 'px';
  dropdown.style.top = (rect.bottom + 4) + 'px';

  const items = [
    { label: 'Transpose', action: 'transpose' },
    { label: 'Normalize', action: 'normalize' },
    { label: 'Sync to BPM', action: 'sync' },
    { label: 'Quantize', action: 'quantize' }
  ];

  for (const item of items) {
    const btn = document.createElement('button');
    btn.className = 'functions-dropdown-item';
    btn.textContent = item.label;
    btn.onclick = () => {
      closeFunctionsDropdown();
      if (item.action === 'transpose') openTransposeModal();
      else if (item.action === 'normalize') openNormalizeModal();
      else if (item.action === 'sync') openSyncFromFunctions();
      else if (item.action === 'quantize') openQuantizeFromFunctions();
    };
    dropdown.appendChild(btn);
  }

  document.body.appendChild(dropdown);
  _functionsDropdown = dropdown;
  setTimeout(() => document.addEventListener('click', _functionsOutsideClick), 0);
};

// ─── Transpose Modal ──
function openTransposeModal() {
  if (!selectedTrack || selectedTrack.type !== 'audio' || !selectedTrack.hasBuff) {
    setStatus('Select an audio track with audio first.');
    return;
  }
  document.getElementById('transpose-semitones').value = 0;
  document.getElementById('transpose-semitones-val').textContent = '0';
  document.getElementById('transpose-modal').classList.add('open');
}

document.getElementById('transpose-semitones').oninput = (e) => {
  const val = parseInt(e.target.value);
  document.getElementById('transpose-semitones-val').textContent = (val > 0 ? '+' : '') + val;
};

document.getElementById('btn-do-transpose').onclick = async () => {
  if (!selectedTrack || selectedTrack.type !== 'audio') return;
  const semitones = parseInt(document.getElementById('transpose-semitones').value);
  const preserveTempo = document.getElementById('transpose-mode').value === 'preserve';

  if (semitones === 0) {
    document.getElementById('transpose-modal').classList.remove('open');
    return;
  }

  setStatus('Transposing audio...');
  document.getElementById('btn-do-transpose').disabled = true;

  try {
    // Save before-snapshot for undo
    const beforeSnap = await engine.saveAudioSnapshot(selectedTrack.id);
    const beforeId = beforeSnap?.snapshotId;

    const result = await engine.transposeAudio(selectedTrack.id, semitones, preserveTempo);
    if (result && result.ok) {
      // Save after-snapshot for redo
      const afterSnap = await engine.saveAudioSnapshot(selectedTrack.id);
      const afterId = afterSnap?.snapshotId;

      if (beforeId && afterId) {
        undoManager.push(new AudioSnapshotCommand(
          engine, selectedTrack.id, beforeId, afterId, 'Transpose'));
      }

      const oldDuration = selectedTrack.duration;
      selectedTrack.duration = result.duration;
      if (result.waveform) {
        selectedTrack.waveformData = result.waveform instanceof Float32Array ? result.waveform : new Float32Array(result.waveform);
      }
      // Update all regions to reflect new duration and waveform
      for (const r of (selectedTrack.regions || [])) {
        // Scale region bounds proportionally to the duration change
        if (oldDuration > 0 && result.duration !== oldDuration) {
          const ratio = result.duration / oldDuration;
          r.clipStart *= ratio;
          r.clipEnd *= ratio;
        }
        r.duration = result.duration;
        r.waveformData = selectedTrack.waveformData;
      }
      renderTrackWaveform(selectedTrack);
      setStatus(`Transposed ${semitones > 0 ? '+' : ''}${semitones} semitones`);
    } else {
      // Free the before-snapshot since the operation failed
      if (beforeId) engine.freeAudioSnapshot(beforeId);
      setStatus('Transpose failed: ' + (result?.error || 'unknown'));
    }
  } catch (err) {
    setStatus('Transpose failed: ' + err.message);
  }

  document.getElementById('btn-do-transpose').disabled = false;
  document.getElementById('transpose-modal').classList.remove('open');
};

document.getElementById('btn-close-transpose').onclick = () => {
  document.getElementById('transpose-modal').classList.remove('open');
};
document.getElementById('transpose-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Normalize Modal ──
let _normalizeTrack = null;
function openNormalizeModal() {
  if (!selectedTrack || selectedTrack.type !== 'audio' || !selectedTrack.hasBuff) {
    setStatus('Select an audio track with audio first.');
    return;
  }
  _normalizeTrack = selectedTrack;
  document.getElementById('normalize-modal').classList.add('open');
}

document.getElementById('btn-do-normalize').onclick = async () => {
  if (!_normalizeTrack || _normalizeTrack.type !== 'audio') return;
  const track = _normalizeTrack;
  const targetDb = parseFloat(document.getElementById('normalize-target').value);

  document.getElementById('normalize-modal').classList.remove('open');
  setStatus('Normalizing audio...');
  document.getElementById('btn-do-normalize').disabled = true;

  try {
    // Save before-snapshot for undo
    const beforeSnap = await engine.saveAudioSnapshot(track.id);
    const beforeId = beforeSnap?.snapshotId;

    const result = await engine.normalizeAudio(track.id, targetDb);
    if (result && result.ok) {
      // Save after-snapshot for redo
      const afterSnap = await engine.saveAudioSnapshot(track.id);
      const afterId = afterSnap?.snapshotId;

      if (beforeId && afterId) {
        undoManager.push(new AudioSnapshotCommand(
          engine, track.id, beforeId, afterId, 'Normalize'));
      }

      track.duration = result.duration;
      if (result.waveform) {
        track.waveformData = result.waveform instanceof Float32Array ? result.waveform : new Float32Array(result.waveform);
      }
      renderTrackWaveform(track);
      setStatus(`Normalized to ${targetDb} dBFS`);
    } else {
      if (beforeId) engine.freeAudioSnapshot(beforeId);
      setStatus('Normalize failed: ' + (result?.error || 'unknown'));
    }
  } catch (err) {
    setStatus('Normalize failed: ' + err.message);
  }

  document.getElementById('btn-do-normalize').disabled = false;
};

document.getElementById('btn-close-normalize').onclick = () => {
  document.getElementById('normalize-modal').classList.remove('open');
};
document.getElementById('normalize-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Sync to BPM (from Functions menu) ──
function openSyncFromFunctions() {
  const track = selectedTrack || tracks.find(t => t.type === 'audio' && (t.hasBuff || (t.regions && t.regions.length > 0)));
  if (!track || track.type !== 'audio' || (!track.hasBuff && !(track.regions && track.regions.length > 0))) {
    setStatus('Select an audio track with audio first.');
    return;
  }
  openSyncModal(track);
}

// ─── Sync Modal (time-stretch to match project BPM) ──
let _tmTrack = null;
function openSyncModal(track) {
  _tmTrack = track;
  const projectBPM = parseFloat(document.getElementById('bpm-input').value) || 120;
  document.getElementById('tm-target-bpm').value = String(projectBPM);
  document.getElementById('tm-source-mode').value = 'auto';
  document.getElementById('tm-manual-row').style.display = 'none';
  document.getElementById('tm-manual-bpm').value = '120';
  document.getElementById('tempo-match-modal').classList.add('open');
}
document.getElementById('tm-source-mode').onchange = () => {
  document.getElementById('tm-manual-row').style.display =
    document.getElementById('tm-source-mode').value === 'manual' ? '' : 'none';
};
document.getElementById('btn-tm-go').onclick = async () => {
  if (!_tmTrack) return;
  const track = _tmTrack;
  const mode = document.getElementById('tm-source-mode').value;
  const opts = {
    sourceBpmMode: mode,
    manualBpm: parseFloat(document.getElementById('tm-manual-bpm').value) || 120,
    minBpm: 40,
    maxBpm: 220
  };
  document.getElementById('tempo-match-modal').classList.remove('open');
  setStatus('Syncing audio to project BPM...');
  try {
    const beforeSnap = await engine.saveAudioSnapshot(track.id);
    const beforeId = beforeSnap?.snapshotId;

    const result = await engine.syncAudio(track.id, opts);
    if (result && result.ok) {
      const afterSnap = await engine.saveAudioSnapshot(track.id);
      const afterId = afterSnap?.snapshotId;
      if (beforeId && afterId) {
        undoManager.push(new AudioSnapshotCommand(
          engine, track.id, beforeId, afterId, 'Sync to BPM'));
      }
      track.duration = result.duration;
      const newWf = result.waveform instanceof Float32Array ? result.waveform : new Float32Array(result.waveform);
      track.waveformData = newWf;
      if (track.regions && track.regions.length > 0) {
        track.regions[0].clipEnd = result.duration;
        track.regions[0].duration = result.duration;
        track.regions[0].waveformData = newWf;
      }
      await syncAudioRegionToNative(track);
      renderTrackWaveform(track);
      setStatus(`Synced: ${result.detectedBPM.toFixed(1)} → ${result.targetBPM.toFixed(1)} BPM`);
    } else {
      if (beforeId) engine.freeAudioSnapshot(beforeId);
      setStatus('Sync failed: ' + (result?.error || 'unknown'));
    }
  } catch (err) {
    setStatus('Sync failed: ' + err.message);
  }
};
document.getElementById('btn-tm-close').onclick = () => {
  document.getElementById('tempo-match-modal').classList.remove('open');
};
document.getElementById('tempo-match-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Quantize (snap beats to grid at project BPM) ──
function openQuantizeFromFunctions() {
  const track = selectedTrack || tracks.find(t => t.type === 'audio' && (t.hasBuff || (t.regions && t.regions.length > 0)));
  if (!track || track.type !== 'audio' || (!track.hasBuff && !(track.regions && track.regions.length > 0))) {
    setStatus('Select an audio track with audio first.');
    return;
  }
  doQuantizeAudio(track);
}

async function doQuantizeAudio(track) {
  setStatus('Quantizing beats to grid...');
  try {
    const beforeSnap = await engine.saveAudioSnapshot(track.id);
    const beforeId = beforeSnap?.snapshotId;

    const result = await engine.quantizeAudio(track.id, { gridDivision: 1.0, strength: 1.0 });
    if (result && result.ok) {
      const afterSnap = await engine.saveAudioSnapshot(track.id);
      const afterId = afterSnap?.snapshotId;
      if (beforeId && afterId) {
        undoManager.push(new AudioSnapshotCommand(
          engine, track.id, beforeId, afterId, 'Quantize'));
      }
      track.duration = result.duration;
      const qWf = result.waveform instanceof Float32Array ? result.waveform : new Float32Array(result.waveform);
      track.waveformData = qWf;
      if (track.regions && track.regions.length > 0) {
        track.regions[0].clipEnd = result.duration;
        track.regions[0].duration = result.duration;
        track.regions[0].waveformData = qWf;
      }
      await syncAudioRegionToNative(track);
      renderTrackWaveform(track);
      setStatus(`Quantized beats to ${result.bpm.toFixed(1)} BPM grid`);
    } else {
      if (beforeId) engine.freeAudioSnapshot(beforeId);
      setStatus('Quantize failed: ' + (result?.error || 'unknown'));
    }
  } catch (err) {
    setStatus('Quantize failed: ' + err.message);
  }
}

// ─── MIDI AI Generation Modal ──
let _midiAITrack = null;
function openMidiAIModal(track) {
  _midiAITrack = track;
  // Auto-select drums style if instrument is drumKit
  if (track.instrumentType === 'drumKit') {
    document.getElementById('ai-midi-style').value = 'drums';
  }
  document.getElementById('midi-ai-modal').classList.add('open');
}

// Slider value displays
document.getElementById('ai-midi-bars').oninput = e => {
  document.getElementById('ai-midi-bars-val').textContent = e.target.value + ' bars';
};
document.getElementById('ai-midi-density').oninput = e => {
  document.getElementById('ai-midi-density-val').textContent = e.target.value + '%';
};
document.getElementById('ai-midi-temp').oninput = e => {
  document.getElementById('ai-midi-temp-val').textContent = (parseInt(e.target.value) / 10).toFixed(1);
};
document.getElementById('ai-midi-swing').oninput = e => {
  document.getElementById('ai-midi-swing-val').textContent = e.target.value + '%';
};

document.getElementById('btn-midi-ai-generate').onclick = async () => {
  if (!_midiAITrack) return;
  const track = _midiAITrack;
  const config = {
    numBars: parseInt(document.getElementById('ai-midi-bars').value),
    temperature: parseInt(document.getElementById('ai-midi-temp').value) / 10,
    keyRoot: parseInt(document.getElementById('ai-midi-key').value),
    scaleType: document.getElementById('ai-midi-scale').value,
    style: document.getElementById('ai-midi-style').value,
    density: parseInt(document.getElementById('ai-midi-density').value) / 100,
    octaveLow: parseInt(document.getElementById('ai-midi-oct-low').value),
    octaveHigh: parseInt(document.getElementById('ai-midi-oct-high').value),
    swingAmount: parseInt(document.getElementById('ai-midi-swing').value) / 100
  };
  const clearFirst = false; // Always append, never replace

  setStatus('Generating MIDI...');
  try {
    // Snapshot before for undo
    const beforeResult = await engine.getMidiNotes(track.id);
    const beforeSnapshot = (beforeResult.notes || []).map(n => ({
      noteNumber: n.noteNumber, startBeat: n.startBeat,
      lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
    }));

    const result = await engine.generateMidi(config);
    if (result && result.error) {
      setStatus('AI generation failed: ' + result.error);
      return;
    }
    const notes = result.notes || [];
    const inject = await engine.injectMidiNotes(track.id, notes, clearFirst);
    if (inject && inject.error) {
      setStatus('Injection failed: ' + inject.error);
      return;
    }

    // Snapshot after for undo
    const afterResult = await engine.getMidiNotes(track.id);
    const afterSnapshot = (afterResult.notes || []).map(n => ({
      noteNumber: n.noteNumber, startBeat: n.startBeat,
      lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
    }));
    undoManager.push(new SnapshotNotesCommand(
      engine, track.id, beforeSnapshot, afterSnapshot, 'Generate MIDI'
    ));

    // Refresh MIDI notes + region in UI
    await refreshMidiNotes(track);
    renderAllTracks();
    setStatus(`Generated ${notes.length} notes (${config.style}, ${config.numBars} bars)`);
  } catch (err) {
    setStatus('AI generation failed: ' + err.message);
  }
  document.getElementById('midi-ai-modal').classList.remove('open');
};

document.getElementById('btn-midi-ai-close').onclick = () => {
  document.getElementById('midi-ai-modal').classList.remove('open');
};
document.getElementById('midi-ai-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Audio AI Modal (transformers.js, bundled models) ──
let _audioAITrack = null;
let _audioAIGenerating = false;

async function openAudioAIModal(track) {
  if (track.type === 'midi') {
    setStatus('Audio AI is not available on MIDI tracks. Use MIDI AI instead.');
    return;
  }
  _audioAITrack = track;

  const select = document.getElementById('ai-audio-model');
  const generateBtn = document.getElementById('btn-audio-ai-generate');
  const progressWrap = document.getElementById('ai-audio-progress-wrap');
  const statusEl = document.getElementById('ai-audio-status');

  select.innerHTML = '<option value="">Loading...</option>';
  select.disabled = true;
  generateBtn.disabled = true;
  progressWrap.style.display = 'none';
  statusEl.textContent = '';

  let result;
  try {
    result = await engine.getAIModels();
  } catch (err) {
    result = { models: [] };
  }
  const models = (result && result.models) || [];

  select.innerHTML = '';
  // Remove any existing download button
  const oldDlBtn = document.getElementById('btn-audio-ai-download');
  if (oldDlBtn) oldDlBtn.remove();

  const availableModels = models.filter(m => m.available);
  if (availableModels.length === 0) {
    select.innerHTML = '<option value="">AI models not downloaded</option>';
    select.disabled = true;
    generateBtn.disabled = true;
    // Show download button
    const dlBtn = document.createElement('button');
    dlBtn.id = 'btn-audio-ai-download';
    dlBtn.className = 'action-btn primary';
    dlBtn.textContent = 'Download AI Models (~2.3 GB)';
    dlBtn.onclick = () => startModelDownload(dlBtn, statusEl, progressWrap);
    generateBtn.parentNode.insertBefore(dlBtn, generateBtn);
  } else {
    select.disabled = false;
    generateBtn.disabled = false;
    for (const m of availableModels) {
      const opt = document.createElement('option');
      opt.value = m.id;
      opt.textContent = m.name;
      select.appendChild(opt);
    }
  }

  document.getElementById('audio-ai-modal').classList.add('open');
}

function formatBytes(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + ' MB';
  return (bytes / 1073741824).toFixed(2) + ' GB';
}

async function startModelDownload(dlBtn, statusEl, progressWrap) {
  dlBtn.disabled = true;
  dlBtn.textContent = 'Downloading...';
  progressWrap.style.display = 'block';
  const progressBar = document.getElementById('ai-audio-progress-bar');
  progressBar.style.width = '0%';
  progressBar.classList.remove('indeterminate');
  statusEl.textContent = 'Starting download...';

  // Listen for progress
  if (engine.onAIDownloadProgress) {
    engine.onAIDownloadProgress((data) => {
      if (data.status === 'downloading' && data.total > 0) {
        const pct = ((data.received / data.total) * 100).toFixed(1);
        progressBar.style.width = pct + '%';
        statusEl.textContent = `${data.file}: ${formatBytes(data.received)} / ${formatBytes(data.total)} (${pct}%)`;
      } else if (data.status === 'downloading') {
        statusEl.textContent = `${data.file}: ${formatBytes(data.received)}`;
      } else if (data.status === 'done') {
        statusEl.textContent = `${data.file}: complete`;
      } else if (data.status === 'skip') {
        statusEl.textContent = `${data.file}: already exists`;
      }
    });
  }

  // Download MusicGen first, then HTDemucs
  let result = await engine.downloadAIModels('musicgen-small');
  if (result && result.error) {
    statusEl.textContent = result.error;
    dlBtn.textContent = 'Retry Download';
    dlBtn.disabled = false;
    return;
  }

  result = await engine.downloadAIModels('htdemucs');
  if (result && result.error) {
    statusEl.textContent = result.error;
    dlBtn.textContent = 'Retry Download';
    dlBtn.disabled = false;
    return;
  }

  statusEl.textContent = 'All models downloaded. Refreshing...';
  progressBar.style.width = '100%';
  dlBtn.remove();

  // Refresh the modal to show available models
  setTimeout(() => openAudioAIModal(_audioAITrack), 500);
}

document.getElementById('ai-audio-duration').oninput = e => {
  document.getElementById('ai-audio-duration-val').textContent = e.target.value + 's';
};
document.getElementById('ai-audio-temp').oninput = e => {
  document.getElementById('ai-audio-temp-val').textContent =
    parseFloat(e.target.value).toFixed(1);
};

// Listen for generation progress events
if (engine.onAIGenerateProgress) {
  engine.onAIGenerateProgress((data) => {
    const statusEl = document.getElementById('ai-audio-status');
    if (data.message) statusEl.textContent = data.message;
  });
}

// Generate button
document.getElementById('btn-audio-ai-generate').onclick = async () => {
  if (!_audioAITrack || _audioAIGenerating) return;
  const track = _audioAITrack;
  const modelId = document.getElementById('ai-audio-model').value;
  if (!modelId) { setStatus('No model selected'); return; }

  const prompt = document.getElementById('ai-audio-prompt').value.trim();
  if (!prompt) { setStatus('Enter a text prompt'); return; }

  const duration = parseInt(document.getElementById('ai-audio-duration').value);
  const temperature = parseFloat(document.getElementById('ai-audio-temp').value);

  const generateBtn = document.getElementById('btn-audio-ai-generate');
  const cancelBtn = document.getElementById('btn-audio-ai-cancel');
  const progressWrap = document.getElementById('ai-audio-progress-wrap');
  const progressBar = document.getElementById('ai-audio-progress-bar');
  const statusEl = document.getElementById('ai-audio-status');

  _audioAIGenerating = true;
  generateBtn.disabled = true;
  generateBtn.textContent = 'Generating...';
  cancelBtn.style.display = 'inline-flex';
  progressWrap.style.display = 'block';
  progressBar.style.width = '0%';
  progressBar.classList.add('indeterminate');
  statusEl.textContent = 'Generating audio... this may take a minute';
  setStatus('Generating AI audio...');

  try {
    const genResult = await engine.generateAIAudio(modelId, prompt, { duration, temperature });

    if (genResult && genResult.error) {
      setStatus('Generation failed: ' + genResult.error);
      statusEl.textContent = 'Failed: ' + genResult.error;
      resetAudioAIButtons();
      return;
    }

    statusEl.textContent = 'Injecting audio into track...';
    const waveform = new Float32Array(genResult.waveform);

    let inject;
    const hadAudio = track.hasBuff;
    if (hadAudio) {
      // Append to existing audio instead of replacing
      inject = await engine.appendAudioBuffer(
        track.id, waveform, genResult.sampleRate, genResult.numChannels);
    } else {
      inject = await engine.injectAudioBuffer(
        track.id, waveform, genResult.sampleRate, genResult.numChannels);
    }

    if (inject && inject.error) {
      setStatus('Injection failed: ' + inject.error);
      statusEl.textContent = 'Failed: ' + inject.error;
      resetAudioAIButtons();
      return;
    }

    // Refresh waveform display
    track.duration = inject.duration || genResult.duration || duration;
    const wfResult = await engine.getTrackWaveform(track.id, 4000);
    if (wfResult && wfResult.data && wfResult.data.length > 0) {
      track.hasBuff = true;
      track.waveformData = new Float32Array(wfResult.data);
      track.duration = wfResult.duration || track.duration;

      if (hadAudio && inject.appendOffset != null) {
        // Add a new region for the appended audio
        track.regions.push({
          nativeTrackId: track.id,
          offset: inject.appendOffset,
          clipStart: inject.appendOffset,
          clipEnd: track.duration,
          loopEnabled: false,
          loopCount: 0,
          fadeIn: 0, fadeOut: 0,
          waveformData: track.waveformData,
          duration: track.duration
        });
        // Update the overall region clip end
        if (track.regions[0]) track.regions[0].clipEnd = track.duration;
      } else {
        // First generation — set single region
        track.regions = [{
          nativeTrackId: track.id,
          offset: 0,
          clipStart: 0,
          clipEnd: track.duration,
          loopEnabled: false,
          loopCount: 0,
          fadeIn: 0, fadeOut: 0,
          waveformData: track.waveformData,
          duration: track.duration
        }];
      }
      await syncAudioRegionToNative(track);
    }
    renderAllTracks();
    setStatus(`Generated ${Math.round(genResult.duration || duration)}s of AI audio`);
    statusEl.textContent = 'Done';
    document.getElementById('audio-ai-modal').classList.remove('open');
  } catch (err) {
    setStatus('AI generation failed: ' + err.message);
    statusEl.textContent = 'Error: ' + err.message;
  }
  resetAudioAIButtons();
};

function resetAudioAIButtons() {
  _audioAIGenerating = false;
  const generateBtn = document.getElementById('btn-audio-ai-generate');
  const cancelBtn = document.getElementById('btn-audio-ai-cancel');
  const progressBar = document.getElementById('ai-audio-progress-bar');
  generateBtn.disabled = false;
  generateBtn.textContent = 'Generate';
  cancelBtn.style.display = 'none';
  progressBar.classList.remove('indeterminate');
}

// Cancel button
document.getElementById('btn-audio-ai-cancel').onclick = async () => {
  try { await engine.cancelAIGeneration(); } catch (e) {}
  document.getElementById('ai-audio-status').textContent = 'Cancelled';
  resetAudioAIButtons();
};

document.getElementById('btn-audio-ai-close').onclick = () => {
  document.getElementById('audio-ai-modal').classList.remove('open');
};
document.getElementById('audio-ai-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Stems Modal ──
let _stemsTrack = null;
function openStemsModal(track) {
  _stemsTrack = track;
  document.getElementById('stems-vocals').checked = true;
  document.getElementById('stems-bass').checked = true;
  document.getElementById('stems-drums').checked = true;
  document.getElementById('stems-other').checked = true;
  document.getElementById('stems-mute-original').checked = false;
  document.getElementById('stems-modal').classList.add('open');
}
document.getElementById('btn-stems-go').onclick = async () => {
  if (!_stemsTrack) { setStatus('No track selected for stem separation.'); return; }
  const track = _stemsTrack;
  if (!track.hasBuff) { setStatus('Track has no audio to separate.'); return; }
  const selectedStems = [];
  if (document.getElementById('stems-vocals').checked) selectedStems.push('Vocals');
  if (document.getElementById('stems-bass').checked) selectedStems.push('Bass');
  if (document.getElementById('stems-drums').checked) selectedStems.push('Drums');
  if (document.getElementById('stems-other').checked) selectedStems.push('Other');
  const muteOriginal = document.getElementById('stems-mute-original').checked;

  if (selectedStems.length === 0) {
    setStatus('Select at least one stem.');
    return;
  }

  document.getElementById('stems-modal').classList.remove('open');
  const progressEl = document.getElementById('stems-progress');
  if (progressEl) progressEl.classList.add('open');
  setStatus('Separating stems... (this may take a moment)');
  try {
    const result = await engine.separateStems(track.id, { stems: selectedStems, muteOriginal });
    if (result && result.ok && result.tracks && result.tracks.length > 0) {
      for (const st of result.tracks) {
        const newTrack = {
          id: st.trackId,
          type: 'audio',
          name: st.name,
          color: TRACK_COLORS[tracks.length % TRACK_COLORS.length],
          volume: 0.8, pan: 0, muted: false, solo: false, armed: false,
          hasBuff: true,
          waveformData: st.waveform instanceof Float32Array ? st.waveform : new Float32Array(st.waveform),
          duration: st.duration || track.duration,
          peakLevel: 0, peakHoldTime: 0, clipping: false,
          fx: { eqEnabled: false, compEnabled: false, delayEnabled: false,
                eqLowGain: 0, eqMidGain: 0, eqMidFreq: 1000, eqHighGain: 0,
                compThreshold: -24, compRatio: 4, compAttack: 0.003, compRelease: 0.25,
                delayTime: 0, delayMix: 0, delayFeedback: 0.3 },
          plugins: [],
          regions: [{
            nativeTrackId: st.trackId,
            offset: 0, clipStart: 0, clipEnd: st.duration || track.duration,
            loopEnabled: false, loopCount: 0, fadeIn: 0, fadeOut: 0,
            waveformData: st.waveform instanceof Float32Array ? st.waveform : new Float32Array(st.waveform),
            duration: st.duration || track.duration
          }],
          get region() { return this.regions[0] || null; },
          set region(v) {
            if (v === null) { this.regions = []; }
            else if (this.regions.length === 0) { this.regions.push(v); }
            else { this.regions[0] = v; }
          }
        };
        tracks.push(newTrack);
        trackIdCounter = Math.max(trackIdCounter, st.trackId + 1);
      }
      if (muteOriginal) {
        track.muted = true;
        await engine.setTrackMute(track.id, true);
      }
      renderAllTracks();
      setStatus(`Stems separated: ${result.tracks.length} new track${result.tracks.length !== 1 ? 's' : ''} created`);
    } else {
      const errMsg = result?.error || 'Separation returned no tracks';
      setStatus('Stem separation failed: ' + errMsg);
      console.error('Stem separation result:', result);
    }
  } catch (err) {
    setStatus('Stem separation failed: ' + err.message);
    console.error('Stem separation error:', err);
  }
  if (progressEl) progressEl.classList.remove('open');
};
document.getElementById('btn-stems-close').onclick = () => {
  document.getElementById('stems-modal').classList.remove('open');
};
document.getElementById('stems-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Project Persistence (file-based via main process) ──
async function saveProject() {
  const name = await promptProjectName(projectName);
  if (!name) return;
  projectName = name;
  setStatus('Saving...');
  const result = await engine.saveProject(name);
  if (result.error) setStatus('Save failed: ' + result.error);
  else setStatus(`Saved: ${projectName}`);
}

async function restoreProjectState(result, fallbackName) {
  undoManager.clear();
  // Clear existing native tracks before loading new ones
  for (const track of [...tracks]) {
    try {
      if (track.type === 'midi') await engine.removeMidiTrack(track.id);
      else {
        for (const nId of (track._hiddenNativeIds || [])) {
          try { await engine.removeTrack(nId); } catch(e) {}
        }
        await engine.removeTrack(track.id);
      }
    } catch(e) { /* best-effort cleanup */ }
  }
  tracks = [];
  trackIdCounter = 0;

  let p = result.project;
  if (!p && result.projectJson) {
    try { p = JSON.parse(result.projectJson); } catch(e) {}
  }

  if (p) {
    projectName = p.name || fallbackName || 'Untitled';
    document.getElementById('bpm-input').value = p.bpm || result.bpm || 120;
    document.getElementById('time-sig').value = p.timeSignature ? String(p.timeSignature) : '4';
    document.getElementById('master-volume').value = p.masterVolume || 0.8;
    loopEnabled = p.loop ? p.loop.enabled : false;
    loopStart = p.loop ? p.loop.startSec : 0;
    loopEnd = p.loop ? p.loop.endSec : 10;
    document.getElementById('btn-loop').classList.toggle('loop-active', loopEnabled);

    for (const td of (p.tracks || [])) {
      const track = {
        id: td.id, type: 'audio', name: td.name,
        color: td.color || TRACK_COLORS[td.id % TRACK_COLORS.length],
        volume: td.volume, pan: td.pan,
        muted: td.muted, solo: td.soloed, armed: false,
        hasBuff: !!td.audioFile, duration: td.duration || 0,
        waveformData: null,
        peakLevel: 0, peakHoldTime: 0, clipping: false,
        fx: td.fx || { eqEnabled: false, compEnabled: false, delayEnabled: false,
                       eqLowGain: 0, eqMidGain: 0, eqMidFreq: 1000, eqHighGain: 0,
                       compThreshold: -24, compRatio: 4, compAttack: 0.003, compRelease: 0.25,
                       delayTime: 0, delayMix: 0, delayFeedback: 0.3 },
        plugins: td.plugins || [],
        regions: [], _hiddenNativeIds: [],
        get region() { return this.regions[0] || null; },
        set region(v) {
          if (v === null) { this.regions = []; }
          else if (this.regions.length === 0) { this.regions.push(v); }
          else { this.regions[0] = v; }
        }
      };
      trackIdCounter = Math.max(trackIdCounter, td.id + 1);
      tracks.push(track);

      if (track.hasBuff) {
        const wf = await engine.getTrackWaveform(track.id, 4000);
        if (wf && wf.data && wf.data.length > 0) {
          track.waveformData = new Float32Array(wf.data);
          if (wf.duration) track.duration = wf.duration;
        }
        track.region = {
          nativeTrackId: track.id,
          offset: td.regionOffset || 0,
          clipStart: td.regionClipStart || 0,
          clipEnd: (td.regionClipEnd != null && td.regionClipEnd >= 0) ? td.regionClipEnd : (track.duration || 0),
          loopEnabled: td.regionLoopEnabled || false,
          loopCount: td.regionLoopEnabled ? 1 : 0,
          fadeIn: td.fadeIn || 0, fadeOut: td.fadeOut || 0,
          waveformData: track.waveformData, duration: track.duration
        };
        await syncAudioRegionToNative(track);
      }
    }

    for (const mt of (p.midiTracks || [])) {
      const track = {
        id: mt.id, type: 'midi', name: mt.name,
        color: mt.color || TRACK_COLORS[mt.id % TRACK_COLORS.length],
        volume: mt.volume, pan: mt.pan,
        muted: mt.muted, solo: mt.soloed, armed: false,
        instrumentName: mt.instrumentName || 'No Instrument',
        instrumentType: mt.instrumentType || null,
        notes: mt.notes || [],
        peakLevel: 0, peakHoldTime: 0, clipping: false,
        plugins: mt.plugins || [], regions: [], splitBeats: [],
        get region() { return this.regions[0] || null; },
        set region(v) {
          if (v === null) { this.regions = []; }
          else if (this.regions.length === 0) { this.regions.push(v); }
          else { this.regions[0] = v; }
        }
      };
      trackIdCounter = Math.max(trackIdCounter, mt.id + 1);
      tracks.push(track);

      if (mt.instrumentType) {
        await engine.setMidiTrackBuiltInInstrument(track.id, mt.instrumentType);
        if (mt.instrumentParams) {
          for (const [param, val] of Object.entries(mt.instrumentParams)) {
            await engine.setBuiltInSynthParam(track.id, param, val);
          }
        }
      }

      // Refresh notes from native engine to get canonical format
      await refreshMidiNotes(track);

      try {
        const chainInfo = await engine.getMidiInsertChainInfo(track.id);
        if (chainInfo && chainInfo[4] && chainInfo[4].isBuiltIn && chainInfo[4].effectType === 'limiter') {
          track._hasSystemLimiter = true;
        } else {
          await engine.insertMidiBuiltInEffect(track.id, 4, 'limiter');
          await engine.setMidiInsertEffectParam(track.id, 4, 'threshold', -0.3);
          await engine.setMidiInsertEffectParam(track.id, 4, 'release', 50);
          track._hasSystemLimiter = true;
        }
      } catch (e) { /* best-effort */ }
    }
  }

  currentTime = 0;
  renderAllTracks();
  updateTimeDisplay();
  updatePlayhead();
  // Delayed re-render to ensure DOM elements have layout dimensions for waveform canvases
  setTimeout(() => renderAllTracks(), 100);
}

async function loadProject(id) {
  setStatus('Loading...');
  if (isPlaying || isRecording) await stop();
  const result = await engine.loadProject(id);
  if (result.error) { setStatus('Load failed: ' + result.error); return; }
  await restoreProjectState(result, id);
  setStatus(`Loaded: ${projectName}`);
}

let currentFilePath = null; // tracks the path of the currently open file

async function saveProjectFile() {
  const name = projectName || 'Untitled';
  setStatus('Saving file...');
  if (currentFilePath) {
    // Save in-place to the existing file path
    const result = await engine.saveProjectFileToPath(name, currentFilePath);
    if (result.error) { setStatus('Save failed: ' + result.error); return; }
    setStatus(`Saved: ${currentFilePath}`);
  } else {
    // No file yet — prompt with dialog (same as Save As)
    await saveProjectFileAs();
  }
}

function promptProjectName(defaultName) {
  return new Promise((resolve) => {
    const modal = document.getElementById('save-name-modal');
    const input = document.getElementById('save-name-input');
    const btnSave = document.getElementById('btn-do-save-name');
    const btnCancel = document.getElementById('btn-cancel-save-name');
    input.value = defaultName || 'Untitled';
    modal.classList.add('open');
    input.focus();
    input.select();

    function cleanup() {
      modal.classList.remove('open');
      btnSave.removeEventListener('click', onSave);
      btnCancel.removeEventListener('click', onCancel);
      input.removeEventListener('keydown', onKey);
    }
    function onSave() { cleanup(); resolve(input.value.trim() || null); }
    function onCancel() { cleanup(); resolve(null); }
    function onKey(e) {
      if (e.key === 'Enter') { e.preventDefault(); onSave(); }
      if (e.key === 'Escape') { e.preventDefault(); onCancel(); }
    }
    btnSave.addEventListener('click', onSave);
    btnCancel.addEventListener('click', onCancel);
    input.addEventListener('keydown', onKey);
  });
}

async function saveProjectFileAs() {
  const inputName = await promptProjectName(projectName || 'Untitled');
  if (!inputName) { setStatus('Save canceled'); return; }
  projectName = inputName;
  setStatus('Saving file...');
  const result = await engine.saveProjectFile(projectName);
  if (result.canceled) { setStatus('Save canceled'); return; }
  if (result.error) { setStatus('Save failed: ' + result.error); return; }
  currentFilePath = result.path;
  setStatus(`Saved: ${result.path}`);
}

async function openProjectFile() {
  if (isPlaying || isRecording) await stop();
  setStatus('Opening file...');
  const result = await engine.openProjectFile();
  if (result.canceled) { setStatus('Open canceled'); return; }
  if (result.error) { setStatus('Open failed: ' + result.error); return; }
  currentFilePath = result.path || null;
  await restoreProjectState(result, 'Imported');
  setStatus(`Opened: ${projectName}`);
}

async function showProjectsModal() {
  const modal = document.getElementById('project-modal');
  const list = document.getElementById('project-list');
  list.innerHTML = '';

  const result = await engine.listProjects();
  const projects = result.projects || [];

  if (projects.length === 0) {
    list.innerHTML = '<li class="no-projects">No saved projects.</li>';
  } else {
    for (const p of projects) {
      const li = document.createElement('li');
      li.className = 'project-item';
      const dateStr = p.date ? new Date(p.date).toLocaleDateString() : '';
      li.innerHTML = `
        <div>
          <div class="proj-name">${escHTML(p.name)}</div>
          <div class="proj-date">${dateStr} &middot; ${p.trackCount || 0} tracks</div>
        </div>
        <button class="proj-delete" title="Delete">&times;</button>
      `;
      li.querySelector('.proj-delete').onclick = async (e) => {
        e.stopPropagation();
        await engine.deleteProject(p.id);
        showProjectsModal();
      };
      li.onclick = () => { loadProject(p.id); modal.classList.remove('open'); };
      list.appendChild(li);
    }
  }

  modal.classList.add('open');
}


// ─── Event Bindings ───────────────────────────────────
document.getElementById('btn-play').onclick = togglePlay;
document.getElementById('btn-stop').onclick = stop;
document.getElementById('btn-record').onclick = toggleRecord;
document.getElementById('btn-rewind').onclick = rewind;
document.getElementById('btn-loop').onclick = toggleLoop;
document.getElementById('btn-add-track').onclick = async () => { await createTrack(); renderAllTracks(); };
document.getElementById('btn-add-midi-track').onclick = async () => { await createMidiTrack(); renderAllTracks(); };
document.getElementById('btn-add-bus-track').onclick = async () => { await createBusTrack(); renderAllTracks(); };
document.getElementById('btn-projects').onclick = showRecentFilesModal;
document.getElementById('btn-new-project').onclick = async () => {
  if (isPlaying || isRecording) await stop();
  for (const track of [...tracks]) {
    if (track.type === 'midi') await engine.removeMidiTrack(track.id);
    else await engine.removeTrack(track.id);
  }
  tracks = [];
  trackIdCounter = 0;
  projectName = 'Untitled';
  currentFilePath = null;
  currentTime = 0;
  renderAllTracks();
  updateTimeDisplay();
  updatePlayhead();
  document.getElementById('project-modal').classList.remove('open');
  setStatus('New project');
};
document.getElementById('btn-close-projects').onclick = () => {
  document.getElementById('project-modal').classList.remove('open');
};
document.getElementById('btn-help').onclick = () => {
  document.getElementById('help-overlay').classList.toggle('open');
};
document.getElementById('help-overlay').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};
document.getElementById('project-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// ─── Audio Settings ────────────────────────────────
let audioDeviceList = [];

let audioInputList = [];

async function showAudioSettings() {
  const modal = document.getElementById('audio-settings-modal');
  const deviceSelect = document.getElementById('audio-device-select');
  const inputSelect = document.getElementById('audio-input-select');
  const rateSelect = document.getElementById('audio-samplerate-select');
  const bufSelect = document.getElementById('audio-buffersize-select');
  const infoDiv = document.getElementById('audio-device-info');

  // Fetch device lists and current state
  audioDeviceList = await engine.getAudioDevices() || [];
  audioInputList = await engine.getInputDevices() || [];
  const current = await engine.getAudioDeviceInfo() || {};

  // Populate output device dropdown
  deviceSelect.innerHTML = '';
  audioDeviceList.forEach(d => {
    const opt = document.createElement('option');
    opt.value = d.name;
    opt.textContent = `${d.name} (${d.typeName})`;
    if (d.name === current.name) opt.selected = true;
    deviceSelect.appendChild(opt);
  });

  // Populate input device dropdown
  inputSelect.innerHTML = '';
  const noneOpt = document.createElement('option');
  noneOpt.value = '';
  noneOpt.textContent = 'None (No Recording Input)';
  inputSelect.appendChild(noneOpt);
  audioInputList.forEach(d => {
    const opt = document.createElement('option');
    opt.value = d.name;
    opt.textContent = `${d.name} (${d.numInputChannels} ch)`;
    inputSelect.appendChild(opt);
  });
  // Select current input device by its actual name
  if (current.inputDeviceName) {
    inputSelect.value = current.inputDeviceName;
  }

  updateAudioSettingsDropdowns(current.name, current.sampleRate, current.bufferSize);
  updateAudioDeviceInfoDisplay(current);

  // Populate MIDI devices
  await refreshMidiDeviceList();

  modal.classList.add('open');
}

async function refreshMidiDeviceList() {
  const container = document.getElementById('midi-device-list');
  const midiDevices = await engine.getMidiDevices() || [];
  container.innerHTML = '';

  // Refresh button
  const refreshRow = document.createElement('div');
  refreshRow.style.cssText = 'display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;';
  refreshRow.innerHTML = `
    <span style="font-size:11px;color:var(--text-secondary)">${midiDevices.length} device${midiDevices.length !== 1 ? 's' : ''} found</span>
    <button class="midi-dev-toggle" style="font-size:10px;padding:2px 8px;">Refresh</button>
  `;
  refreshRow.querySelector('button').onclick = () => refreshMidiDeviceList();
  container.appendChild(refreshRow);

  if (midiDevices.length === 0) {
    const hint = document.createElement('div');
    hint.className = 'no-midi-devices';
    hint.innerHTML = 'No MIDI devices detected.<br><span style="font-size:10px;opacity:0.6">Connect a USB MIDI controller or pair a Bluetooth MIDI device in Audio MIDI Setup.</span>';
    container.appendChild(hint);
    return;
  }

  midiDevices.forEach(d => {
    const row = document.createElement('div');
    row.className = 'midi-device-row';
    row.innerHTML = `
      <span class="midi-dev-name">${escHTML(d.name)}</span>
      <button class="midi-dev-toggle ${d.isOpen ? 'connected' : ''}">${d.isOpen ? 'Connected' : 'Connect'}</button>
    `;
    row.querySelector('.midi-dev-toggle').onclick = async () => {
      if (d.isOpen) {
        await engine.closeMidiDevice(d.identifier);
      } else {
        const ok = await engine.openMidiDevice(d.identifier);
        if (ok) setStatus(`MIDI: ${d.name} connected`);
        else setStatus(`MIDI: Failed to open ${d.name}`);
      }
      await refreshMidiDeviceList();
    };
    container.appendChild(row);
  });
}

function updateAudioSettingsDropdowns(deviceName, currentRate, currentBuf) {
  const rateSelect = document.getElementById('audio-samplerate-select');
  const bufSelect = document.getElementById('audio-buffersize-select');
  const dev = audioDeviceList.find(d => d.name === deviceName);
  if (!dev) return;

  rateSelect.innerHTML = '';
  (dev.sampleRates || []).forEach(r => {
    const opt = document.createElement('option');
    opt.value = r;
    opt.textContent = `${r} Hz`;
    if (r === currentRate) opt.selected = true;
    rateSelect.appendChild(opt);
  });

  bufSelect.innerHTML = '';
  (dev.bufferSizes || []).forEach(b => {
    const opt = document.createElement('option');
    opt.value = b;
    opt.textContent = `${b} samples`;
    if (b === currentBuf) opt.selected = true;
    bufSelect.appendChild(opt);
  });
}

function updateAudioDeviceInfoDisplay(info) {
  const infoDiv = document.getElementById('audio-device-info');
  if (!info || !info.name) {
    infoDiv.innerHTML = '<span>No device connected</span>';
    return;
  }
  const totalLatency = ((info.inputLatencyMs || 0) + (info.outputLatencyMs || 0)).toFixed(1);
  infoDiv.innerHTML = `
    <div class="info-row"><span>Device</span><span class="info-val">${escHTML(info.name)}</span></div>
    <div class="info-row"><span>Type</span><span class="info-val">${escHTML(info.typeName || '')}</span></div>
    <div class="info-row"><span>Sample Rate</span><span class="info-val">${info.sampleRate || 0} Hz</span></div>
    <div class="info-row"><span>Buffer Size</span><span class="info-val">${info.bufferSize || 0} samples</span></div>
    <div class="info-row"><span>Inputs</span><span class="info-val">${info.activeInputs || 0} ch</span></div>
    <div class="info-row"><span>Outputs</span><span class="info-val">${info.activeOutputs || 0} ch</span></div>
    <div class="info-row"><span>Round-trip Latency</span><span class="info-val">${totalLatency} ms</span></div>
  `;
}

document.getElementById('audio-device-select').onchange = e => {
  const rate = parseFloat(document.getElementById('audio-samplerate-select').value) || 48000;
  const buf = parseInt(document.getElementById('audio-buffersize-select').value) || 512;
  updateAudioSettingsDropdowns(e.target.value, rate, buf);
};

document.getElementById('btn-apply-audio').onclick = async () => {
  const outputName = document.getElementById('audio-device-select').value;
  const inputName = document.getElementById('audio-input-select').value;
  const rate = parseFloat(document.getElementById('audio-samplerate-select').value);
  const buf = parseInt(document.getElementById('audio-buffersize-select').value);
  const applyBtn = document.getElementById('btn-apply-audio');
  if (!outputName || isNaN(rate) || isNaN(buf)) {
    setStatus('Select a valid device, sample rate, and buffer size');
    return;
  }
  applyBtn.textContent = 'Applying...';
  applyBtn.disabled = true;
  try {
    let result;
    if (inputName && inputName !== outputName) {
      result = await engine.setAudioDeviceSeparate(outputName, inputName, rate, buf);
    } else {
      result = await engine.setAudioDevice(inputName || outputName, rate, buf);
    }
    if (result && result.ok) {
      const info = await engine.getAudioDeviceInfo();
      updateAudioDeviceInfoDisplay(info);
      applyBtn.textContent = 'Applied';
      setStatus(`Output: ${outputName} | Input: ${inputName || 'None'} @ ${rate}Hz / ${buf} samples`);
      setTimeout(() => { applyBtn.textContent = 'Apply'; }, 1500);
    } else {
      applyBtn.textContent = 'Failed';
      setStatus('Failed to switch audio device');
      setTimeout(() => { applyBtn.textContent = 'Apply'; }, 1500);
    }
  } catch (e) {
    console.error('Audio settings apply error:', e);
    applyBtn.textContent = 'Error';
    setStatus('Error applying audio settings');
    setTimeout(() => { applyBtn.textContent = 'Apply'; }, 1500);
  }
  applyBtn.disabled = false;
};

document.getElementById('btn-close-audio-settings').onclick = () => {
  document.getElementById('audio-settings-modal').classList.remove('open');
};
document.getElementById('btn-audio-settings').onclick = () => showAudioSettings();

// Mixer toggle
document.getElementById('btn-mixer').onclick = () => {
  if (mixerBoard.isOpen) mixerBoard.close();
  else mixerBoard.open(tracks, busTracks);
};
document.getElementById('audio-settings-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// Master volume
document.getElementById('master-volume').oninput = e => {
  engine.setMasterVolume(parseFloat(e.target.value));
  // Sync mixer master fader
  const mixerMasterFader = document.getElementById('mixer-master-fader');
  if (mixerMasterFader) mixerMasterFader.value = e.target.value;
  const mixerMasterLabel = document.getElementById('mixer-master-vol-label');
  if (mixerMasterLabel) {
    const v = parseFloat(e.target.value);
    mixerMasterLabel.textContent = v < 0.0001 ? '-∞' : (20 * Math.log10(v)).toFixed(1);
  }
};

// Metronome volume
document.getElementById('metro-vol').oninput = e => {
  engine.setMetronomeVolume(parseFloat(e.target.value));
};

// BPM
document.getElementById('bpm-input').onchange = e => {
  engine.setBPM(parseInt(e.target.value) || 120);
};

// Time signature
document.getElementById('time-sig').onchange = e => {
  const val = parseInt(e.target.value);
  engine.setTimeSignature(val, 4);
};

// Zoom
document.getElementById('zoom-slider').oninput = e => {
  zoomLevel = parseFloat(e.target.value);
  renderRuler();
  tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t));
  updatePlayhead();
  updateLoopOverlay();
  scheduleWaveformRefetch();
};

// Snap mode selector
document.getElementById('snap-mode').onchange = (e) => {
  snapMode = e.target.value;
};

// Ruler click to seek
document.getElementById('ruler-canvas-wrap').onclick = e => {
  const rect = e.currentTarget.getBoundingClientRect();
  seekTo(scrollOffset + (e.clientX - rect.left) / zoomLevel);
};

// Horizontal scroll via trackpad/mouse wheel
function handleTimelineScroll(e) {
  const dx = e.deltaX || (e.shiftKey ? e.deltaY : 0);
  if (dx === 0) return;
  e.preventDefault();
  scrollOffset = Math.max(0, scrollOffset + dx / zoomLevel);
  renderRuler();
  tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t));
  updatePlayhead();
  updateLoopOverlay();
}
document.getElementById('tracks-panel').addEventListener('wheel', handleTimelineScroll, { passive: false });
document.getElementById('ruler-canvas-wrap').addEventListener('wheel', handleTimelineScroll, { passive: false });

// Follow playhead toggle
document.getElementById('btn-follow').onclick = () => {
  followPlayhead = !followPlayhead;
  document.getElementById('btn-follow').classList.toggle('active', followPlayhead);
  if (followPlayhead && isPlaying) {
    const waveArea = document.querySelector('.track-waveform');
    const viewSecs = (waveArea ? waveArea.getBoundingClientRect().width : 500) / zoomLevel;
    scrollOffset = Math.max(0, currentTime - viewSecs * 0.2);
    renderRuler();
    tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t));
    updatePlayhead();
    updateLoopOverlay();
  }
};

// Keyboard shortcuts
document.addEventListener('keydown', async (e) => {
  if (e.target.tagName === 'TEXTAREA') return;
  if (e.target.tagName === 'INPUT' && e.target.type !== 'range') return;

  // Undo / Redo
  if ((e.metaKey || e.ctrlKey) && e.code === 'KeyZ') {
    e.preventDefault();
    if (e.shiftKey) performRedo();
    else performUndo();
    return;
  }

  if ((e.metaKey || e.ctrlKey) && e.shiftKey && e.code === 'KeyS') { e.preventDefault(); saveProjectFileAs(); return; }
  if ((e.metaKey || e.ctrlKey) && e.code === 'KeyS') { e.preventDefault(); saveProjectFile(); return; }
  if ((e.metaKey || e.ctrlKey) && e.code === 'KeyO') { e.preventDefault(); openProjectFile(); return; }

  // Blade/Split at playhead (Cmd+B)
  if ((e.metaKey || e.ctrlKey) && e.code === 'KeyB') {
    e.preventDefault();
    const bpm = parseInt(document.getElementById('bpm-input').value) || 120;
    const playBeat = currentTime * (bpm / 60);
    const target = selectedTrack || tracks.find(t => {
      if (t.type === 'audio' && t.regions.length > 0) {
        return t.regions.some(r => currentTime > r.offset && currentTime < r.offset + (r.clipEnd - r.clipStart));
      }
      if (t.type === 'midi' && t.regions.length > 0) {
        return t.regions.some(r => playBeat > r.startBeat && playBeat < r.endBeat);
      }
      return false;
    });
    if (target) {
      if (target.type === 'audio') splitAudioAtPlayhead(target);
      else if (target.type === 'midi') splitMidiAtPlayhead(target);
    }
    return;
  }

  // Copy region (Cmd+C)
  if ((e.metaKey || e.ctrlKey) && e.code === 'KeyC') {
    e.preventDefault();
    if (!selectedTrack || selectedTrack.regions.length === 0) { setStatus('No region selected'); return; }
    const ri = selectedRegionIndex || 0;
    const selRegion = selectedTrack.regions[ri];
    if (!selRegion) { setStatus('No region selected'); return; }
    if (selectedTrack.type === 'audio') {
      clipboard = {
        type: 'audio',
        trackId: selRegion.nativeTrackId || selectedTrack.id,
        region: { ...selRegion },
        waveformData: selRegion.waveformData || selectedTrack.waveformData ? new Float32Array(selRegion.waveformData || selectedTrack.waveformData) : null,
        duration: selRegion.duration || selectedTrack.duration,
        color: selectedTrack.color,
        name: selectedTrack.name
      };
    } else if (selectedTrack.type === 'midi') {
      // Copy notes within the selected region's beat range
      const regionNotes = (selectedTrack.notes || []).filter(n => {
        const nEnd = n.startBeat + n.lengthBeats;
        return n.startBeat < selRegion.endBeat && nEnd > selRegion.startBeat;
      });
      clipboard = {
        type: 'midi',
        notes: regionNotes.map(n => ({ ...n })),
        region: { ...selRegion },
        instrumentName: selectedTrack.instrumentName,
        instrumentType: selectedTrack.instrumentType,
        color: selectedTrack.color,
        name: selectedTrack.name
      };
    }
    setStatus('Copied region');
    return;
  }

  // Cut region (Cmd+X) — only when Cmd held, otherwise KeyX toggles mixer
  if ((e.metaKey || e.ctrlKey) && e.code === 'KeyX') {
    e.preventDefault();
    if (!selectedTrack || selectedTrack.regions.length === 0) { setStatus('No region selected'); return; }
    const ri = selectedRegionIndex || 0;
    const selRegion = selectedTrack.regions[ri];
    if (!selRegion) { setStatus('No region selected'); return; }
    // Copy first
    if (selectedTrack.type === 'audio') {
      clipboard = {
        type: 'audio',
        trackId: selRegion.nativeTrackId || selectedTrack.id,
        region: { ...selRegion },
        waveformData: selRegion.waveformData || selectedTrack.waveformData ? new Float32Array(selRegion.waveformData || selectedTrack.waveformData) : null,
        duration: selRegion.duration || selectedTrack.duration,
        color: selectedTrack.color,
        name: selectedTrack.name
      };
      // Delete this specific region
      const nId = selRegion.nativeTrackId || selectedTrack.id;
      await engine.setAudioRegion(nId, 0, 0, 0, false);
      selectedTrack.regions.splice(ri, 1);
      if (selectedTrack.regions.length === 0) {
        selectedTrack.waveformData = null;
        selectedTrack.hasBuff = false;
        selectedTrack.duration = 0;
      }
    } else if (selectedTrack.type === 'midi') {
      const regionNotes = (selectedTrack.notes || []).filter(n => {
        const nEnd = n.startBeat + n.lengthBeats;
        return n.startBeat < selRegion.endBeat && nEnd > selRegion.startBeat;
      });
      clipboard = {
        type: 'midi',
        notes: regionNotes.map(n => ({ ...n })),
        region: { ...selRegion },
        instrumentName: selectedTrack.instrumentName,
        instrumentType: selectedTrack.instrumentType,
        color: selectedTrack.color,
        name: selectedTrack.name
      };
      // Remove region's notes from engine and local model
      const remainingNotes = (selectedTrack.notes || []).filter(n => {
        const nEnd = n.startBeat + n.lengthBeats;
        return !(n.startBeat < selRegion.endBeat && nEnd > selRegion.startBeat);
      });
      const eng = (await engine.getMidiNotes(selectedTrack.id)).notes || [];
      for (let i = eng.length - 1; i >= 0; i--) await engine.removeMidiNote(selectedTrack.id, i);
      for (const n of remainingNotes) {
        await engine.addMidiNote(selectedTrack.id, n.note, n.startBeat, n.lengthBeats, n.velocity);
      }
      selectedTrack.notes = remainingNotes;
      // Remove the split beat if it was a split boundary
      if (selectedTrack.splitBeats) {
        selectedTrack.splitBeats = selectedTrack.splitBeats.filter(b => b !== selRegion.startBeat && b !== selRegion.endBeat);
      }
      computeMidiRegions(selectedTrack);
    }
    renderAllTracks();
    setStatus('Cut region');
    return;
  }

  // Paste region (Cmd+V) — paste INTO selected track at playhead
  if ((e.metaKey || e.ctrlKey) && e.code === 'KeyV') {
    e.preventDefault();
    if (!clipboard) { setStatus('Clipboard empty'); return; }

    if (clipboard.type === 'midi') {
      const bpm = parseInt(document.getElementById('bpm-input').value) || 120;
      const beatsPerSec = bpm / 60;
      const pasteBeat = snapToQuarter(currentTime * beatsPerSec);
      const minBeat = Math.min(...clipboard.notes.map(n => n.startBeat));
      const shiftBeats = pasteBeat - minBeat;

      // Paste into selected MIDI track if available, else create new
      let targetTrack = (selectedTrack && selectedTrack.type === 'midi') ? selectedTrack : null;
      if (!targetTrack) {
        targetTrack = await createMidiTrack(clipboard.name + ' (paste)');
        if (clipboard.instrumentType) {
          await engine.setMidiTrackBuiltInInstrument(targetTrack.id, clipboard.instrumentType);
          targetTrack.instrumentName = clipboard.instrumentName;
          targetTrack.instrumentType = clipboard.instrumentType;
        }
      }
      for (const n of clipboard.notes) {
        await engine.addMidiNote(targetTrack.id, n.note, n.startBeat + shiftBeats, n.lengthBeats, n.velocity);
      }
      await refreshMidiNotes(targetTrack);
      renderAllTracks();
      setStatus('Pasted MIDI region');
    } else if (clipboard.type === 'audio') {
      try {
        const result = await engine.duplicateAudioTrack(clipboard.trackId);
        if (result && result.newTrackId >= 0) {
          trackIdCounter = Math.max(trackIdCounter, result.newTrackId + 1);
          const wf = await engine.getTrackWaveform(result.newTrackId, 4000);
          const pasteDuration = (wf && wf.duration) || 0;
          const pasteWaveform = (wf && wf.data) ? new Float32Array(wf.data) : null;

          const pasteRegion = {
            nativeTrackId: result.newTrackId,
            offset: currentTime,
            clipStart: 0,
            clipEnd: pasteDuration,
            loopEnabled: false,
            loopCount: 0,
            fadeIn: 0,
            fadeOut: 0,
            waveformData: pasteWaveform,
            duration: pasteDuration
          };
          await engine.setAudioRegion(result.newTrackId, currentTime, 0, pasteDuration, false);

          // Paste into selected audio track if available, else create new
          if (selectedTrack && selectedTrack.type === 'audio') {
            selectedTrack.regions.push(pasteRegion);
            selectedTrack._hiddenNativeIds.push(result.newTrackId);
            await syncSubTrackParams(selectedTrack);
          } else {
            // Create new track
            const newTrack = {
              id: result.newTrackId,
              type: 'audio',
              name: result.newTrackName || clipboard.name + ' (paste)',
              color: TRACK_COLORS[result.newTrackId % TRACK_COLORS.length],
              volume: 0.8, pan: 0, muted: false, solo: false, armed: false,
              hasBuff: true, duration: pasteDuration, waveformData: pasteWaveform,
              peakLevel: 0, peakHoldTime: 0, clipping: false,
              fx: { eqEnabled: false, compEnabled: false, delayEnabled: false,
                eqLowGain: 0, eqMidGain: 0, eqMidFreq: 1000, eqHighGain: 0,
                compThreshold: -24, compRatio: 4, compAttack: 0.003, compRelease: 0.25,
                delayTime: 0, delayMix: 0, delayFeedback: 0.3 },
              plugins: [], outputBus: -1,
              regions: [pasteRegion], _hiddenNativeIds: [],
              get region() { return this.regions[0] || null; },
              set region(v) {
                if (v === null) { this.regions = []; }
                else if (this.regions.length === 0) { this.regions.push(v); }
                else { this.regions[0] = v; }
              }
            };
            tracks.push(newTrack);
          }
          renderAllTracks();
          setStatus('Pasted audio region');
        }
      } catch (err) {
        setStatus('Audio paste failed: ' + (err.message || err));
      }
    }
    return;
  }

  if ((e.metaKey || e.ctrlKey) && (e.code === 'Equal' || e.code === 'NumpadAdd')) {
    e.preventDefault();
    zoomLevel = Math.min(200, zoomLevel * 1.25);
    document.getElementById('zoom-slider').value = zoomLevel;
    renderRuler(); tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t)); updatePlayhead(); updateLoopOverlay();
    scheduleWaveformRefetch();
    return;
  }
  if ((e.metaKey || e.ctrlKey) && (e.code === 'Minus' || e.code === 'NumpadSubtract')) {
    e.preventDefault();
    zoomLevel = Math.max(2, zoomLevel / 1.25);
    document.getElementById('zoom-slider').value = zoomLevel;
    renderRuler(); tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t)); updatePlayhead(); updateLoopOverlay();
    scheduleWaveformRefetch();
    return;
  }

  switch (e.code) {
    case 'Space': e.preventDefault(); togglePlay(); break;
    case 'KeyR': e.preventDefault(); toggleRecord(); break;
    case 'KeyT': e.preventDefault(); createTrack().then(() => renderAllTracks()); break;
    case 'KeyM': e.preventDefault(); createMidiTrack().then(() => renderAllTracks()); break;
    case 'KeyB': e.preventDefault(); createBusTrack().then(() => renderAllTracks()); break;
    case 'KeyL': e.preventDefault(); toggleLoop(); break;
    case 'KeyX': e.preventDefault(); if (mixerBoard.isOpen) mixerBoard.close(); else mixerBoard.open(tracks, busTracks); break;
    case 'Home': e.preventDefault(); rewind(); break;
    case 'Escape':
      e.preventDefault();
      if (mixerBoard.isOpen) { mixerBoard.close(); break; }
      if (pianoRoll.isOpen) { pianoRoll.close(); break; }
      if (isPlaying || isRecording) stop();
      document.getElementById('help-overlay').classList.remove('open');
      document.getElementById('project-modal').classList.remove('open');
      document.getElementById('plugin-modal').classList.remove('open');
      document.getElementById('fx-panel').classList.remove('open');
      document.getElementById('audio-settings-modal').classList.remove('open');
      document.getElementById('export-modal').classList.remove('open');
      document.getElementById('instrument-modal').classList.remove('open');
      document.getElementById('inst-param-panel').classList.remove('open');
      document.getElementById('insert-picker-modal').classList.remove('open');
      document.getElementById('insert-param-panel').classList.remove('open');
      break;
    case 'Slash':
      if (e.shiftKey) { e.preventDefault(); document.getElementById('help-overlay').classList.toggle('open'); }
      break;
  }
});

// Resize
window.addEventListener('resize', () => {
  renderRuler();
  tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t));
  updatePlayhead();
  updateLoopOverlay();
});

// Drag and drop — import via native engine
document.addEventListener('dragover', e => e.preventDefault());
document.addEventListener('drop', async (e) => {
  e.preventDefault();
  const files = [...e.dataTransfer.files].filter(f => f.type.startsWith('audio/'));
  for (const file of files) {
    const name = file.name.replace(/\.[^/.]+$/, '');
    const track = await createTrack(name);
    // Native engine handles the file via its path
    if (file.path) {
      await engine.importAudioToTrack(track.id, file.path);
      const wf = await engine.getTrackWaveform(track.id, 4000);
      if (wf && wf.data && wf.data.length > 0) {
        track.hasBuff = true;
        track.waveformData = new Float32Array(wf.data);
        track.duration = wf.duration || 0;
        track.region = { nativeTrackId: track.id, offset: 0, clipStart: 0, clipEnd: track.duration, loopEnabled: false, loopCount: 0, fadeIn: 0, fadeOut: 0, waveformData: track.waveformData, duration: track.duration };
      }
    }
  }
  renderAllTracks();
  setTimeout(() => renderAllTracks(), 100);
  setStatus('Imported audio');
});

// Menu actions from native macOS menu
engine.onMenuAction((action) => {
  switch (action) {
    case 'undo':
      if (document.activeElement?.tagName === 'INPUT' && document.activeElement?.type !== 'range') {
        document.execCommand('undo');
      } else {
        performUndo();
      }
      break;
    case 'redo':
      if (document.activeElement?.tagName === 'INPUT' && document.activeElement?.type !== 'range') {
        document.execCommand('redo');
      } else {
        performRedo();
      }
      break;
    case 'new': document.getElementById('btn-new-project').click(); break;
    case 'open': openProjectFile(); break;
    case 'openFile': openProjectFile(); break;
    case 'openRecent': showRecentFilesModal(); break;
    case 'save': saveProjectFile(); break;
    case 'saveAs': saveProjectFileAs(); break;
    case 'import': document.getElementById('btn-import').click(); break;
    case 'exportWav': document.getElementById('btn-export').click(); break;
    case 'exportAiff': document.getElementById('btn-export').click(); break;
    case 'bounce': document.getElementById('btn-bounce').click(); break;
    case 'zoomIn':
      zoomLevel = Math.min(200, zoomLevel * 1.25);
      document.getElementById('zoom-slider').value = zoomLevel;
      renderRuler(); tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t)); updatePlayhead(); updateLoopOverlay();
      scheduleWaveformRefetch();
      break;
    case 'zoomOut':
      zoomLevel = Math.max(2, zoomLevel / 1.25);
      document.getElementById('zoom-slider').value = zoomLevel;
      renderRuler(); tracks.forEach(t => t.type === 'midi' ? renderMidiTrackPreview(t) : renderTrackWaveform(t)); updatePlayhead(); updateLoopOverlay();
      scheduleWaveformRefetch();
      break;
  }
});

// Init
renderAllTracks();
renderRuler();

// Auto-connect first available MIDI device (with retry for late-appearing devices)
async function autoConnectMidi(attempt = 1) {
  try {
    const midiDevices = await engine.getMidiDevices() || [];
    console.log(`[MIDI] Scan attempt ${attempt}: found ${midiDevices.length} device(s)`, midiDevices.map(d => d.name));
    if (midiDevices.length > 0 && !midiDevices.some(d => d.isOpen)) {
      const ok = await engine.openMidiDevice(midiDevices[0].identifier);
      if (ok) {
        setStatus(`MIDI: ${midiDevices[0].name} connected`);
        console.log(`[MIDI] Connected: ${midiDevices[0].name}`);
      } else {
        console.warn(`[MIDI] Failed to open: ${midiDevices[0].name}`);
      }
    } else if (midiDevices.length === 0 && attempt < 5) {
      // CoreMIDI devices can appear asynchronously; retry with backoff
      const delay = attempt * 1000;
      console.log(`[MIDI] No devices found, retrying in ${delay}ms...`);
      setTimeout(() => autoConnectMidi(attempt + 1), delay);
    }
  } catch (e) {
    console.warn('[MIDI] Auto-connect error:', e);
    if (attempt < 5) setTimeout(() => autoConnectMidi(attempt + 1), attempt * 1000);
  }
}
autoConnectMidi();

// ─── Recent Files Modal ──
async function showRecentFilesModal() {
  const modal = document.getElementById('recent-files-modal');
  const list = document.getElementById('recent-files-list');
  list.innerHTML = '';

  const result = await engine.getRecentFiles();
  const files = (result && result.files) || [];

  if (files.length === 0) {
    list.innerHTML = '<li class="no-projects">No recent projects. Use Browse to open a file.</li>';
  } else {
    const showFiles = files.slice(0, 8);
    for (const f of showFiles) {
      const li = document.createElement('li');
      li.className = 'project-item';
      const dateStr = f.date ? new Date(f.date).toLocaleDateString() : '';
      li.innerHTML = `
        <div>
          <div class="proj-name">${escHTML(f.name)}</div>
          <div class="proj-date">${dateStr} &middot; ${escHTML(f.path)}</div>
        </div>
      `;
      li.onclick = async () => {
        modal.classList.remove('open');
        setStatus('Opening file...');
        if (isPlaying || isRecording) await stop();
        const openResult = await engine.openFilePath(f.path);
        if (openResult.error) { setStatus('Open failed: ' + openResult.error); return; }
        currentFilePath = f.path;
        await restoreProjectState(openResult, f.name);
        setStatus(`Opened: ${projectName}`);
      };
      list.appendChild(li);
    }
  }

  modal.classList.add('open');
}

document.getElementById('btn-recent-open-file').onclick = () => {
  document.getElementById('recent-files-modal').classList.remove('open');
  openProjectFile();
};
document.getElementById('btn-recent-new').onclick = () => {
  document.getElementById('recent-files-modal').classList.remove('open');
  document.getElementById('btn-new-project').click();
};
document.getElementById('btn-recent-close').onclick = () => {
  document.getElementById('recent-files-modal').classList.remove('open');
};
document.getElementById('recent-files-modal').onclick = e => {
  if (e.target === e.currentTarget) e.currentTarget.classList.remove('open');
};

// Show recent files on startup
setTimeout(() => showRecentFilesModal(), 500);

// Periodic MIDI device scan for hotplug support
setInterval(async () => {
  try {
    const midiDevices = await engine.getMidiDevices() || [];
    if (midiDevices.length > 0 && !midiDevices.some(d => d.isOpen)) {
      const ok = await engine.openMidiDevice(midiDevices[0].identifier);
      if (ok) {
        setStatus(`MIDI: ${midiDevices[0].name} connected`);
        console.log(`[MIDI] Hotplug connected: ${midiDevices[0].name}`);
      }
    }
  } catch (_) {}
}, 5000);

setStatus('Ready');
