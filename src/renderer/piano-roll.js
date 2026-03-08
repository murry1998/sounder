// ═══════════════════════════════════════════════════════════
//  SOUNDER — Piano Roll MIDI Editor
// ═══════════════════════════════════════════════════════════

class PianoRoll {
  constructor(engine) {
    this.engine = engine;
    this.trackId = null;
    this.trackName = '';
    this.notes = [];
    this.isOpen = false;

    // View state
    this.scrollX = 0;
    this.scrollY = 0;
    this.zoomX = 80;   // px per beat
    this.zoomY = 14;   // px per note row
    this.gridSize = 0.25; // beats (1/16th)

    // Piano
    this.noteMin = 0;
    this.noteMax = 127;
    this.pianoWidth = 48;

    // Tools: 'select', 'draw', 'erase'
    this.tool = 'draw';

    // Selection
    this.selectedNotes = new Set();
    this.dragState = null; // { type, noteIdx, startX, startY, origNote, origBeat, origLen }

    // Undo manager (set externally by renderer.js)
    this.undoManager = null;

    // Velocity lane
    this.velocityLaneHeight = 60;
    this.showVelocity = true;

    // Playhead
    this.playheadBeat = 0;

    // Default note length for draw tool
    this.drawLength = 0.25;

    // DOM
    this.overlay = document.getElementById('piano-roll-overlay');
    this.canvas = document.getElementById('piano-roll-canvas');
    this.velCanvas = document.getElementById('piano-roll-velocity');
    this.pianoCanvas = document.getElementById('piano-roll-piano');
    this.toolBtns = document.querySelectorAll('.pr-tool-btn');
    this.gridSelect = document.getElementById('pr-grid-size');
    this.titleEl = document.getElementById('pr-title');

    this.ctx = this.canvas.getContext('2d');
    this.velCtx = this.velCanvas.getContext('2d');
    this.pianoCtx = this.pianoCanvas.getContext('2d');

    this._bindEvents();
  }

  // ─── Open / Close ──────────────────────────────────

  async open(trackId, trackName) {
    this.trackId = trackId;
    this.trackName = trackName;
    this.titleEl.textContent = `Piano Roll: ${trackName}`;
    this.isOpen = true;
    this.selectedNotes.clear();

    // Fetch notes from engine
    const result = await this.engine.getMidiNotes(trackId);
    this.notes = (result.notes || []).map((n, i) => ({
      idx: i,
      note: n.noteNumber,
      start: n.startBeat,
      length: n.lengthBeats,
      velocity: n.velocity,
      channel: n.channel || 1
    }));

    // Center view around existing notes
    if (this.notes.length > 0) {
      const minNote = Math.min(...this.notes.map(n => n.note));
      const maxNote = Math.max(...this.notes.map(n => n.note));
      const center = Math.floor((minNote + maxNote) / 2);
      this.scrollY = Math.max(0, (127 - center) * this.zoomY - this.canvas.height / 2);
    } else {
      this.scrollY = (127 - 60) * this.zoomY - 200; // Center around C4
    }
    this.scrollX = 0;

    this.overlay.classList.add('open');
    this._resize();
    this._render();
  }

  close() {
    this.isOpen = false;
    this.overlay.classList.remove('open');
    const closedId = this.trackId;
    this.trackId = null;
    if (this.onClose && closedId != null) this.onClose(closedId);
  }

  // ─── Note CRUD (local + engine sync) ──────────────

  async addNote(noteNumber, startBeat, lengthBeats, velocity) {
    await this.engine.addMidiNote(this.trackId, noteNumber, startBeat, lengthBeats, velocity);
    if (this.undoManager) {
      this.undoManager.push(new AddNoteCommand(
        this.engine, this.trackId, noteNumber, startBeat, lengthBeats, velocity
      ));
    }
    const result = await this.engine.getMidiNotes(this.trackId);
    this._syncNotes(result.notes || []);
    this._render();
  }

  async removeNote(noteIdx) {
    const n = this.notes[noteIdx];
    await this.engine.removeMidiNote(this.trackId, noteIdx);
    if (this.undoManager && n) {
      this.undoManager.push(new RemoveNoteCommand(
        this.engine, this.trackId, n.note, n.start, n.length, n.velocity
      ));
    }
    const result = await this.engine.getMidiNotes(this.trackId);
    this._syncNotes(result.notes || []);
    this._render();
  }

  async moveNote(noteIdx, newNote, newStart) {
    await this.engine.moveMidiNote(this.trackId, noteIdx, newNote, newStart);
    const result = await this.engine.getMidiNotes(this.trackId);
    this._syncNotes(result.notes || []);
    this._render();
  }

  async resizeNote(noteIdx, newLength) {
    await this.engine.resizeMidiNote(this.trackId, noteIdx, newLength);
    const result = await this.engine.getMidiNotes(this.trackId);
    this._syncNotes(result.notes || []);
    this._render();
  }

  async setVelocity(noteIdx, velocity) {
    await this.engine.setMidiNoteVelocity(this.trackId, noteIdx, velocity);
    const result = await this.engine.getMidiNotes(this.trackId);
    this._syncNotes(result.notes || []);
    this._render();
  }

  async quantize() {
    // Snapshot before
    const beforeResult = await this.engine.getMidiNotes(this.trackId);
    const beforeSnapshot = (beforeResult.notes || []).map(n => ({
      noteNumber: n.noteNumber, startBeat: n.startBeat,
      lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
    }));

    await this.engine.quantizeMidiNotes(this.trackId, this.gridSize);

    const afterResult = await this.engine.getMidiNotes(this.trackId);
    const afterSnapshot = (afterResult.notes || []).map(n => ({
      noteNumber: n.noteNumber, startBeat: n.startBeat,
      lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
    }));

    if (this.undoManager) {
      this.undoManager.push(new SnapshotNotesCommand(
        this.engine, this.trackId, beforeSnapshot, afterSnapshot, 'Quantize'
      ));
    }

    this._syncNotes(afterResult.notes || []);
    this._render();
  }

  _syncNotes(rawNotes) {
    this.notes = rawNotes.map((n, i) => ({
      idx: i,
      note: n.noteNumber,
      start: n.startBeat,
      length: n.lengthBeats,
      velocity: n.velocity,
      channel: n.channel || 1
    }));
    this.selectedNotes.clear();
    if (this.onChange && this.trackId != null) this.onChange(this.trackId);
  }

  // ─── Coordinate conversions ────────────────────────

  beatToX(beat) { return beat * this.zoomX - this.scrollX; }
  xToBeat(x) { return (x + this.scrollX) / this.zoomX; }
  noteToY(note) { return (127 - note) * this.zoomY - this.scrollY; }
  yToNote(y) { return 127 - Math.floor((y + this.scrollY) / this.zoomY); }

  snapBeat(beat) {
    return Math.round(beat / this.gridSize) * this.gridSize;
  }

  // ─── Hit testing ───────────────────────────────────

  noteAtXY(x, y) {
    const beat = this.xToBeat(x);
    const note = this.yToNote(y);
    for (let i = this.notes.length - 1; i >= 0; i--) {
      const n = this.notes[i];
      if (n.note === note && beat >= n.start && beat < n.start + n.length) {
        // Check if near right edge (resize handle)
        const rightEdgeX = this.beatToX(n.start + n.length);
        const isEdge = Math.abs(x - rightEdgeX) < 6;
        return { index: i, note: n, isEdge };
      }
    }
    return null;
  }

  velocityNoteAtX(x) {
    const beat = this.xToBeat(x);
    for (let i = this.notes.length - 1; i >= 0; i--) {
      const n = this.notes[i];
      const nx = this.beatToX(n.start);
      if (Math.abs(x - nx) < 6) return { index: i, note: n };
    }
    return null;
  }

  // ─── Rendering ─────────────────────────────────────

  _resize() {
    const dpr = window.devicePixelRatio || 1;
    const area = this.overlay.querySelector('.pr-body');
    if (!area) return;

    // Main canvas
    const mainWrap = this.canvas.parentElement;
    const mw = mainWrap.clientWidth;
    const mh = mainWrap.clientHeight;
    this.canvas.width = mw * dpr;
    this.canvas.height = mh * dpr;
    this.canvas.style.width = mw + 'px';
    this.canvas.style.height = mh + 'px';
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    // Velocity canvas
    const velWrap = this.velCanvas.parentElement;
    const vw = velWrap.clientWidth;
    const vh = velWrap.clientHeight;
    this.velCanvas.width = vw * dpr;
    this.velCanvas.height = vh * dpr;
    this.velCanvas.style.width = vw + 'px';
    this.velCanvas.style.height = vh + 'px';
    this.velCtx.setTransform(dpr, 0, 0, dpr, 0, 0);

    // Piano canvas
    const pw = this.pianoCanvas.parentElement.clientWidth;
    const ph = this.pianoCanvas.parentElement.clientHeight;
    this.pianoCanvas.width = pw * dpr;
    this.pianoCanvas.height = ph * dpr;
    this.pianoCanvas.style.width = pw + 'px';
    this.pianoCanvas.style.height = ph + 'px';
    this.pianoCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  _render() {
    if (!this.isOpen) return;
    this._renderGrid();
    this._renderNotes();
    this._renderPlayhead();
    this._renderPiano();
    this._renderVelocity();
  }

  _renderGrid() {
    const c = this.ctx;
    const w = this.canvas.clientWidth;
    const h = this.canvas.clientHeight;
    c.clearRect(0, 0, w, h);

    // Background - warm medium tone
    c.fillStyle = '#3a3735';
    c.fillRect(0, 0, w, h);

    // Note rows - dimmed gray tones
    const blackKeys = [1, 3, 6, 8, 10];
    for (let note = 0; note <= 127; note++) {
      const y = this.noteToY(note);
      if (y > h || y + this.zoomY < 0) continue;
      const pc = note % 12;
      if (blackKeys.includes(pc)) {
        c.fillStyle = '#2d2d2d';
        c.fillRect(0, y, w, this.zoomY);
      } else {
        c.fillStyle = '#4a4a4a';
        c.fillRect(0, y, w, this.zoomY);
      }
      // Horizontal line - octave boundaries brighter
      c.strokeStyle = pc === 0 ? 'rgba(255,255,255,0.15)' : 'rgba(0,0,0,0.25)';
      c.lineWidth = 1;
      c.beginPath();
      c.moveTo(0, y + this.zoomY);
      c.lineTo(w, y + this.zoomY);
      c.stroke();
    }

    // Measure header background
    c.fillStyle = '#222';
    c.fillRect(0, 0, w, 20);
    c.strokeStyle = 'rgba(255,255,255,0.08)';
    c.lineWidth = 1;
    c.beginPath();
    c.moveTo(0, 20);
    c.lineTo(w, 20);
    c.stroke();

    // Vertical beat/measure lines
    const startBeat = Math.floor(this.xToBeat(0) / this.gridSize) * this.gridSize;
    const endBeat = this.xToBeat(w);
    for (let beat = startBeat; beat <= endBeat; beat += this.gridSize) {
      const x = this.beatToX(beat);
      const isMeasure = Math.abs(beat % 4) < 0.001;
      const isBeat = Math.abs(beat % 1) < 0.001;
      if (isMeasure) {
        c.strokeStyle = 'rgba(255,255,255,0.22)';
        c.lineWidth = 2;
      } else if (isBeat) {
        c.strokeStyle = 'rgba(255,255,255,0.08)';
        c.lineWidth = 1;
      } else {
        c.strokeStyle = 'rgba(255,255,255,0.03)';
        c.lineWidth = 1;
      }
      c.beginPath();
      c.moveTo(x, isMeasure ? 0 : 20);
      c.lineTo(x, h);
      c.stroke();
    }

    // Measure numbers in header
    c.font = 'bold 11px -apple-system, Helvetica Neue, sans-serif';
    c.fillStyle = 'rgba(255,255,255,0.6)';
    c.textBaseline = 'middle';
    for (let beat = Math.floor(this.xToBeat(0)); beat <= endBeat; beat++) {
      if (beat < 0) continue;
      const beatInMeasure = (beat % 4);
      if (Math.abs(beatInMeasure) < 0.001) {
        const x = this.beatToX(beat);
        const measure = Math.floor(beat / 4) + 1;
        c.fillText(`${measure}`, x + 5, 11);
      }
    }
    c.textBaseline = 'alphabetic';
  }

  _renderNotes() {
    const c = this.ctx;
    const w = this.canvas.clientWidth;
    const h = this.canvas.clientHeight;

    for (let i = 0; i < this.notes.length; i++) {
      const n = this.notes[i];
      const x = this.beatToX(n.start);
      const y = this.noteToY(n.note);
      const nw = n.length * this.zoomX;
      const nh = this.zoomY - 1;

      if (x + nw < 0 || x > w || y + nh < 0 || y > h) continue;

      const selected = this.selectedNotes.has(i);
      const alpha = 0.5 + (n.velocity / 127) * 0.5;

      // Note body - warm orange (selected: warm gold)
      c.fillStyle = selected
        ? `rgba(245,216,98,${alpha})`
        : `rgba(232,145,58,${alpha})`;
      c.fillRect(x, y, nw, nh);

      // Border
      c.strokeStyle = selected ? 'rgba(245,216,98,0.9)' : 'rgba(240,168,86,0.7)';
      c.lineWidth = selected ? 1.5 : 1;
      c.strokeRect(x + 0.5, y + 0.5, nw - 1, nh - 1);

      // Resize handle
      if (nw > 10) {
        c.fillStyle = selected ? 'rgba(245,216,98,0.5)' : 'rgba(240,168,86,0.3)';
        c.fillRect(x + nw - 4, y, 4, nh);
      }
    }
  }

  _renderPlayhead() {
    const c = this.ctx;
    const h = this.canvas.clientHeight;
    const x = this.beatToX(this.playheadBeat);
    if (x < 0 || x > this.canvas.clientWidth) return;

    c.strokeStyle = '#e8913a';
    c.lineWidth = 1.5;
    c.beginPath();
    c.moveTo(x, 0);
    c.lineTo(x, h);
    c.stroke();
  }

  _renderPiano() {
    const c = this.pianoCtx;
    const pw = this.pianoCanvas.clientWidth;
    const ph = this.pianoCanvas.clientHeight;
    c.clearRect(0, 0, pw, ph);
    c.fillStyle = '#2c2c2c';
    c.fillRect(0, 0, pw, ph);

    const noteNames = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
    const blackKeys = [1, 3, 6, 8, 10];

    for (let note = 0; note <= 127; note++) {
      const y = this.noteToY(note);
      if (y > ph || y + this.zoomY < 0) continue;

      const pc = note % 12;
      const octave = Math.floor(note / 12) - 1;
      const isBlack = blackKeys.includes(pc);

      // Piano keys: gray for white, dark for black
      c.fillStyle = isBlack ? '#2a2a2a' : '#6a6a6a';
      c.fillRect(0, y, pw, this.zoomY - 1);

      c.strokeStyle = 'rgba(0,0,0,0.2)';
      c.lineWidth = 1;
      c.beginPath();
      c.moveTo(0, y + this.zoomY);
      c.lineTo(pw, y + this.zoomY);
      c.stroke();

      if (pc === 0 && this.zoomY >= 8) {
        c.font = '8px -apple-system, Helvetica Neue, sans-serif';
        c.fillStyle = isBlack ? 'rgba(255,255,255,0.4)' : 'rgba(255,255,255,0.7)';
        c.fillText(`C${octave}`, 2, y + this.zoomY - 3);
      }
    }
  }

  _renderVelocity() {
    if (!this.showVelocity) return;
    const c = this.velCtx;
    const w = this.velCanvas.clientWidth;
    const h = this.velCanvas.clientHeight;
    c.clearRect(0, 0, w, h);

    c.fillStyle = '#2a2a2a';
    c.fillRect(0, 0, w, h);

    // Grid line at 50%
    c.strokeStyle = 'rgba(255,255,255,0.08)';
    c.lineWidth = 1;
    c.beginPath();
    c.moveTo(0, h / 2);
    c.lineTo(w, h / 2);
    c.stroke();

    for (let i = 0; i < this.notes.length; i++) {
      const n = this.notes[i];
      const x = this.beatToX(n.start);
      if (x < -10 || x > w + 10) continue;

      const barH = (n.velocity / 127) * (h - 4);
      const selected = this.selectedNotes.has(i);

      // Warm orange bars (selected: warm gold)
      c.fillStyle = selected ? 'rgba(245,216,98,0.7)' : 'rgba(232,145,58,0.7)';
      c.fillRect(x - 2, h - barH, 5, barH);
    }
  }

  // ─── Event handling ────────────────────────────────

  _bindEvents() {
    // Tool buttons
    this.toolBtns.forEach(btn => {
      btn.onclick = () => {
        this.tool = btn.dataset.tool;
        this.toolBtns.forEach(b => b.classList.toggle('active', b.dataset.tool === this.tool));
      };
    });

    // Grid size
    if (this.gridSelect) {
      this.gridSelect.onchange = () => {
        this.gridSize = parseFloat(this.gridSelect.value);
        this.drawLength = this.gridSize;
        this._render();
      };
    }

    // Close
    document.getElementById('pr-close')?.addEventListener('click', () => this.close());

    // Quantize
    document.getElementById('pr-quantize')?.addEventListener('click', () => this.quantize());

    // Main canvas events
    this.canvas.addEventListener('mousedown', e => this._onMouseDown(e));
    this.canvas.addEventListener('mousemove', e => this._onMouseMove(e));
    this.canvas.addEventListener('mouseup', e => this._onMouseUp(e));
    this.canvas.addEventListener('wheel', e => this._onWheel(e), { passive: false });

    // Velocity canvas events
    this.velCanvas.addEventListener('mousedown', e => this._onVelMouseDown(e));

    // Keyboard
    this.overlay.addEventListener('keydown', e => this._onKeyDown(e));

    // Resize
    window.addEventListener('resize', () => {
      if (this.isOpen) { this._resize(); this._render(); }
    });
  }

  _canvasXY(e) {
    const rect = this.canvas.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  }

  _onMouseDown(e) {
    const { x, y } = this._canvasXY(e);
    const hit = this.noteAtXY(x, y);

    if (this.tool === 'draw') {
      if (hit) {
        // Start drag-move
        this.dragState = {
          type: hit.isEdge ? 'resize' : 'move',
          noteIdx: hit.index,
          startX: x, startY: y,
          origNote: hit.note.note,
          origBeat: hit.note.start,
          origLen: hit.note.length
        };
      } else {
        // Create new note
        const beat = this.snapBeat(this.xToBeat(x));
        const note = this.yToNote(y);
        if (note >= 0 && note <= 127 && beat >= 0) {
          this.addNote(note, beat, this.drawLength, 100);
        }
      }
    } else if (this.tool === 'select') {
      if (hit) {
        if (!e.shiftKey) this.selectedNotes.clear();
        if (this.selectedNotes.has(hit.index)) {
          this.selectedNotes.delete(hit.index);
        } else {
          this.selectedNotes.add(hit.index);
        }
        this.dragState = {
          type: hit.isEdge ? 'resize' : 'move',
          noteIdx: hit.index,
          startX: x, startY: y,
          origNote: hit.note.note,
          origBeat: hit.note.start,
          origLen: hit.note.length
        };
        this._render();
      } else {
        if (!e.shiftKey) { this.selectedNotes.clear(); this._render(); }
      }
    } else if (this.tool === 'erase') {
      if (hit) this.removeNote(hit.index);
    }
  }

  _onMouseMove(e) {
    if (!this.dragState) {
      // Cursor hint
      const { x, y } = this._canvasXY(e);
      const hit = this.noteAtXY(x, y);
      if (this.tool === 'erase') {
        this.canvas.style.cursor = 'crosshair';
      } else if (hit && hit.isEdge) {
        this.canvas.style.cursor = 'ew-resize';
      } else if (hit) {
        this.canvas.style.cursor = this.tool === 'select' ? 'grab' : 'pointer';
      } else {
        this.canvas.style.cursor = this.tool === 'draw' ? 'crosshair' : 'default';
      }
      return;
    }

    const { x, y } = this._canvasXY(e);
    const ds = this.dragState;

    if (ds.type === 'move') {
      const dBeat = this.xToBeat(x) - this.xToBeat(ds.startX);
      const dNote = this.yToNote(y) - this.yToNote(ds.startY);
      const newBeat = this.snapBeat(Math.max(0, ds.origBeat + dBeat));
      const newNote = Math.max(0, Math.min(127, ds.origNote + dNote));

      // Update local preview
      const n = this.notes[ds.noteIdx];
      if (n) { n.start = newBeat; n.note = newNote; }
      this._render();
    } else if (ds.type === 'resize') {
      const beat = this.snapBeat(this.xToBeat(x));
      const n = this.notes[ds.noteIdx];
      if (n) {
        const newLen = Math.max(this.gridSize, beat - n.start);
        n.length = newLen;
      }
      this._render();
    }
  }

  async _onMouseUp(e) {
    if (!this.dragState) return;
    const ds = this.dragState;
    const n = this.notes[ds.noteIdx];
    this.dragState = null;

    if (!n) return;

    if (ds.type === 'move') {
      if (n.note !== ds.origNote || n.start !== ds.origBeat) {
        await this.engine.moveMidiNote(this.trackId, ds.noteIdx, n.note, n.start);
        if (this.undoManager) {
          this.undoManager.push(new MoveNoteCommand(
            this.engine, this.trackId,
            ds.origNote, ds.origBeat, n.note, n.start,
            ds.origLen, n.velocity
          ));
        }
        const result = await this.engine.getMidiNotes(this.trackId);
        this._syncNotes(result.notes || []);
        this._render();
      }
    } else if (ds.type === 'resize') {
      if (n.length !== ds.origLen) {
        await this.engine.resizeMidiNote(this.trackId, ds.noteIdx, n.length);
        if (this.undoManager) {
          this.undoManager.push(new ResizeNoteCommand(
            this.engine, this.trackId,
            n.note, n.start, ds.origLen, n.length, n.velocity
          ));
        }
        const result = await this.engine.getMidiNotes(this.trackId);
        this._syncNotes(result.notes || []);
        this._render();
      }
    }
  }

  _onWheel(e) {
    e.preventDefault();
    if (e.ctrlKey || e.metaKey) {
      // Zoom
      const factor = e.deltaY > 0 ? 0.9 : 1.1;
      if (e.shiftKey) {
        this.zoomY = Math.max(4, Math.min(40, this.zoomY * factor));
      } else {
        this.zoomX = Math.max(10, Math.min(400, this.zoomX * factor));
      }
    } else {
      // Scroll
      if (e.shiftKey) {
        this.scrollX = Math.max(0, this.scrollX + e.deltaY);
      } else {
        this.scrollY = Math.max(0, this.scrollY + e.deltaY);
      }
    }
    this._render();
  }

  _onVelMouseDown(e) {
    const rect = this.velCanvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    const h = this.velCanvas.clientHeight;
    const hit = this.velocityNoteAtX(x);
    if (!hit) return;

    const oldVelocity = hit.note.velocity;
    const vel = Math.max(1, Math.min(127, Math.round((1 - y / h) * 127)));
    this.notes[hit.index].velocity = vel;
    this._renderVelocity();

    const onMove = (me) => {
      const my = me.clientY - rect.top;
      const v = Math.max(1, Math.min(127, Math.round((1 - my / h) * 127)));
      this.notes[hit.index].velocity = v;
      this._renderVelocity();
    };
    const onUp = async (me) => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      const my = me.clientY - rect.top;
      const v = Math.max(1, Math.min(127, Math.round((1 - my / h) * 127)));
      await this.engine.setMidiNoteVelocity(this.trackId, hit.index, v);
      if (this.undoManager && v !== oldVelocity) {
        this.undoManager.push(new SetVelocityCommand(
          this.engine, this.trackId,
          hit.note.note, hit.note.start, hit.note.length,
          oldVelocity, v
        ));
      }
      const result = await this.engine.getMidiNotes(this.trackId);
      this._syncNotes(result.notes || []);
      this._render();
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  }

  _onKeyDown(e) {
    if (!this.isOpen) return;

    // Undo / Redo
    if ((e.metaKey || e.ctrlKey) && e.code === 'KeyZ') {
      e.preventDefault();
      e.stopPropagation();
      if (e.shiftKey) {
        if (typeof performRedo === 'function') performRedo();
      } else {
        if (typeof performUndo === 'function') performUndo();
      }
      return;
    }

    if (e.code === 'Escape') {
      e.preventDefault();
      this.close();
      return;
    }

    if (e.code === 'Delete' || e.code === 'Backspace') {
      e.preventDefault();
      const sorted = [...this.selectedNotes].sort((a, b) => b - a);
      if (sorted.length === 0) return;
      (async () => {
        if (sorted.length === 1) {
          await this.removeNote(sorted[0]);
        } else {
          // Multi-note delete: use snapshot for single undo step
          const beforeResult = await this.engine.getMidiNotes(this.trackId);
          const beforeSnapshot = (beforeResult.notes || []).map(n => ({
            noteNumber: n.noteNumber, startBeat: n.startBeat,
            lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
          }));
          for (const idx of sorted) {
            await this.engine.removeMidiNote(this.trackId, idx);
          }
          const afterResult = await this.engine.getMidiNotes(this.trackId);
          const afterSnapshot = (afterResult.notes || []).map(n => ({
            noteNumber: n.noteNumber, startBeat: n.startBeat,
            lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
          }));
          if (this.undoManager) {
            this.undoManager.push(new SnapshotNotesCommand(
              this.engine, this.trackId, beforeSnapshot, afterSnapshot, 'Delete Notes'
            ));
          }
          this._syncNotes(afterResult.notes || []);
          this._render();
        }
        this.selectedNotes.clear();
      })();
      return;
    }

    if ((e.metaKey || e.ctrlKey) && e.code === 'KeyA') {
      e.preventDefault();
      this.selectedNotes.clear();
      this.notes.forEach((_, i) => this.selectedNotes.add(i));
      this._render();
      return;
    }

    // Arrow keys nudge selected notes (batched for undo)
    if (this.selectedNotes.size > 0) {
      const isArrow = ['ArrowUp','ArrowDown','ArrowLeft','ArrowRight'].includes(e.code);
      if (isArrow) {
        e.preventDefault();
        (async () => {
          // Snapshot before nudge
          const beforeResult = await this.engine.getMidiNotes(this.trackId);
          const beforeSnapshot = (beforeResult.notes || []).map(n => ({
            noteNumber: n.noteNumber, startBeat: n.startBeat,
            lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
          }));

          for (const idx of this.selectedNotes) {
            const n = this.notes[idx];
            if (!n) continue;
            if (e.code === 'ArrowUp' && n.note < 127) {
              await this.moveNote(idx, n.note + 1, n.start);
            } else if (e.code === 'ArrowDown' && n.note > 0) {
              await this.moveNote(idx, n.note - 1, n.start);
            } else if (e.code === 'ArrowRight') {
              await this.moveNote(idx, n.note, n.start + this.gridSize);
            } else if (e.code === 'ArrowLeft' && n.start >= this.gridSize) {
              await this.moveNote(idx, n.note, n.start - this.gridSize);
            }
          }

          // Snapshot after nudge
          if (this.undoManager) {
            const afterResult = await this.engine.getMidiNotes(this.trackId);
            const afterSnapshot = (afterResult.notes || []).map(n => ({
              noteNumber: n.noteNumber, startBeat: n.startBeat,
              lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
            }));
            this.undoManager.push(new SnapshotNotesCommand(
              this.engine, this.trackId, beforeSnapshot, afterSnapshot, 'Nudge Notes'
            ));
          }
        })();
        return;
      }
    }

    // Tool shortcuts
    if (e.code === 'Digit1') { this.tool = 'select'; this._updateToolUI(); }
    if (e.code === 'Digit2') { this.tool = 'draw'; this._updateToolUI(); }
    if (e.code === 'Digit3') { this.tool = 'erase'; this._updateToolUI(); }
  }

  _updateToolUI() {
    this.toolBtns.forEach(b => b.classList.toggle('active', b.dataset.tool === this.tool));
  }

  // ─── Playhead update (called from transport callback) ──

  updatePlayhead(currentTimeSeconds, bpm) {
    if (!this.isOpen) return;
    this.playheadBeat = currentTimeSeconds * (bpm / 60);
    this._render();
  }
}
