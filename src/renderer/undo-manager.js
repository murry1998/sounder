// ═══════════════════════════════════════════════════════════
//  Undo / Redo System — Command Pattern
// ═══════════════════════════════════════════════════════════

// ─── Helper: find a note's current index by its properties ─
async function findNoteIndex(engine, trackId, noteNumber, startBeat, lengthBeats, velocity) {
  const result = await engine.getMidiNotes(trackId);
  const notes = result.notes || [];
  for (let i = 0; i < notes.length; i++) {
    const n = notes[i];
    if (n.noteNumber === noteNumber &&
        Math.abs(n.startBeat - startBeat) < 0.001 &&
        Math.abs(n.lengthBeats - lengthBeats) < 0.001 &&
        n.velocity === velocity) {
      return i;
    }
  }
  return -1;
}

// ─── UndoManager ───────────────────────────────────────────
class UndoManager {
  constructor(maxHistory = 100) {
    this.undoStack = [];
    this.redoStack = [];
    this.maxHistory = maxHistory;
    this._batchDepth = 0;
    this._batchCommands = [];
    this._busy = false;
  }

  // Push a command that has already been executed
  push(command) {
    if (this._batchDepth > 0) {
      this._batchCommands.push(command);
      return;
    }
    this.undoStack.push(command);
    // Destroy discarded redo commands to free native memory
    for (const cmd of this.redoStack) {
      if (cmd.destroy) cmd.destroy();
    }
    this.redoStack.length = 0;
    if (this.undoStack.length > this.maxHistory) {
      const evicted = this.undoStack.shift();
      if (evicted && evicted.destroy) evicted.destroy();
    }
  }

  // Execute a command and record it
  async execute(command) {
    if (this._batchDepth > 0) {
      await command.execute();
      this._batchCommands.push(command);
      return;
    }
    await command.execute();
    this.undoStack.push(command);
    for (const cmd of this.redoStack) {
      if (cmd.destroy) cmd.destroy();
    }
    this.redoStack.length = 0;
    if (this.undoStack.length > this.maxHistory) {
      const evicted = this.undoStack.shift();
      if (evicted && evicted.destroy) evicted.destroy();
    }
  }

  async undo() {
    if (this._busy || this.undoStack.length === 0) return false;
    this._busy = true;
    try {
      const command = this.undoStack.pop();
      await command.undo();
      this.redoStack.push(command);
      return true;
    } finally {
      this._busy = false;
    }
  }

  async redo() {
    if (this._busy || this.redoStack.length === 0) return false;
    this._busy = true;
    try {
      const command = this.redoStack.pop();
      await command.redo();
      this.undoStack.push(command);
      return true;
    } finally {
      this._busy = false;
    }
  }

  beginBatch() {
    this._batchDepth++;
    if (this._batchDepth === 1) {
      this._batchCommands = [];
    }
  }

  endBatch(label) {
    this._batchDepth--;
    if (this._batchDepth === 0 && this._batchCommands.length > 0) {
      const batch = new BatchCommand(this._batchCommands, label);
      this.undoStack.push(batch);
      this.redoStack.length = 0;
      if (this.undoStack.length > this.maxHistory) {
        this.undoStack.shift();
      }
      this._batchCommands = [];
    }
  }

  canUndo() { return this.undoStack.length > 0; }
  canRedo() { return this.redoStack.length > 0; }

  clear() {
    for (const cmd of this.undoStack) {
      if (cmd.destroy) cmd.destroy();
    }
    for (const cmd of this.redoStack) {
      if (cmd.destroy) cmd.destroy();
    }
    this.undoStack.length = 0;
    this.redoStack.length = 0;
    this._batchDepth = 0;
    this._batchCommands = [];
  }
}

// ─── BatchCommand ──────────────────────────────────────────
class BatchCommand {
  constructor(commands, label) {
    this.commands = commands;
    this.label = label || 'Batch';
  }

  async execute() {
    for (const cmd of this.commands) await cmd.execute();
  }

  async undo() {
    for (let i = this.commands.length - 1; i >= 0; i--) {
      await this.commands[i].undo();
    }
  }

  async redo() {
    for (const cmd of this.commands) await cmd.redo();
  }
}

// ─── AddNoteCommand ────────────────────────────────────────
class AddNoteCommand {
  constructor(engine, trackId, noteNumber, startBeat, lengthBeats, velocity) {
    this.engine = engine;
    this.trackId = trackId;
    this.noteNumber = noteNumber;
    this.startBeat = startBeat;
    this.lengthBeats = lengthBeats;
    this.velocity = velocity;
    this.label = 'Add Note';
  }

  async execute() {
    await this.engine.addMidiNote(
      this.trackId, this.noteNumber, this.startBeat, this.lengthBeats, this.velocity
    );
  }

  async undo() {
    const idx = await findNoteIndex(
      this.engine, this.trackId,
      this.noteNumber, this.startBeat, this.lengthBeats, this.velocity
    );
    if (idx >= 0) await this.engine.removeMidiNote(this.trackId, idx);
  }

  async redo() { await this.execute(); }
}

// ─── RemoveNoteCommand ─────────────────────────────────────
class RemoveNoteCommand {
  constructor(engine, trackId, noteNumber, startBeat, lengthBeats, velocity) {
    this.engine = engine;
    this.trackId = trackId;
    this.noteNumber = noteNumber;
    this.startBeat = startBeat;
    this.lengthBeats = lengthBeats;
    this.velocity = velocity;
    this.label = 'Delete Note';
  }

  async execute() {
    const idx = await findNoteIndex(
      this.engine, this.trackId,
      this.noteNumber, this.startBeat, this.lengthBeats, this.velocity
    );
    if (idx >= 0) await this.engine.removeMidiNote(this.trackId, idx);
  }

  async undo() {
    await this.engine.addMidiNote(
      this.trackId, this.noteNumber, this.startBeat, this.lengthBeats, this.velocity
    );
  }

  async redo() { await this.execute(); }
}

// ─── MoveNoteCommand ───────────────────────────────────────
class MoveNoteCommand {
  constructor(engine, trackId, oldNote, oldStart, newNote, newStart, lengthBeats, velocity) {
    this.engine = engine;
    this.trackId = trackId;
    this.oldNote = oldNote;
    this.oldStart = oldStart;
    this.newNote = newNote;
    this.newStart = newStart;
    this.lengthBeats = lengthBeats;
    this.velocity = velocity;
    this.label = 'Move Note';
  }

  async execute() {
    const idx = await findNoteIndex(
      this.engine, this.trackId,
      this.oldNote, this.oldStart, this.lengthBeats, this.velocity
    );
    if (idx >= 0) await this.engine.moveMidiNote(this.trackId, idx, this.newNote, this.newStart);
  }

  async undo() {
    const idx = await findNoteIndex(
      this.engine, this.trackId,
      this.newNote, this.newStart, this.lengthBeats, this.velocity
    );
    if (idx >= 0) await this.engine.moveMidiNote(this.trackId, idx, this.oldNote, this.oldStart);
  }

  async redo() { await this.execute(); }
}

// ─── ResizeNoteCommand ─────────────────────────────────────
class ResizeNoteCommand {
  constructor(engine, trackId, noteNumber, startBeat, oldLength, newLength, velocity) {
    this.engine = engine;
    this.trackId = trackId;
    this.noteNumber = noteNumber;
    this.startBeat = startBeat;
    this.oldLength = oldLength;
    this.newLength = newLength;
    this.velocity = velocity;
    this.label = 'Resize Note';
  }

  async execute() {
    const idx = await findNoteIndex(
      this.engine, this.trackId,
      this.noteNumber, this.startBeat, this.oldLength, this.velocity
    );
    if (idx >= 0) await this.engine.resizeMidiNote(this.trackId, idx, this.newLength);
  }

  async undo() {
    const idx = await findNoteIndex(
      this.engine, this.trackId,
      this.noteNumber, this.startBeat, this.newLength, this.velocity
    );
    if (idx >= 0) await this.engine.resizeMidiNote(this.trackId, idx, this.oldLength);
  }

  async redo() { await this.execute(); }
}

// ─── SetVelocityCommand ────────────────────────────────────
class SetVelocityCommand {
  constructor(engine, trackId, noteNumber, startBeat, lengthBeats, oldVelocity, newVelocity) {
    this.engine = engine;
    this.trackId = trackId;
    this.noteNumber = noteNumber;
    this.startBeat = startBeat;
    this.lengthBeats = lengthBeats;
    this.oldVelocity = oldVelocity;
    this.newVelocity = newVelocity;
    this.label = 'Set Velocity';
  }

  async execute() {
    const idx = await findNoteIndex(
      this.engine, this.trackId,
      this.noteNumber, this.startBeat, this.lengthBeats, this.oldVelocity
    );
    if (idx >= 0) await this.engine.setMidiNoteVelocity(this.trackId, idx, this.newVelocity);
  }

  async undo() {
    const idx = await findNoteIndex(
      this.engine, this.trackId,
      this.noteNumber, this.startBeat, this.lengthBeats, this.newVelocity
    );
    if (idx >= 0) await this.engine.setMidiNoteVelocity(this.trackId, idx, this.oldVelocity);
  }

  async redo() { await this.execute(); }
}

// ─── SnapshotNotesCommand (quantize, multi-delete, bulk ops) ─
class SnapshotNotesCommand {
  constructor(engine, trackId, beforeNotes, afterNotes, label) {
    this.engine = engine;
    this.trackId = trackId;
    this.beforeNotes = beforeNotes;
    this.afterNotes = afterNotes;
    this.label = label || 'Edit Notes';
  }

  async execute() {
    // afterNotes already in engine (pushed post-execution)
  }

  async undo() {
    await this.engine.injectMidiNotes(
      this.trackId,
      this.beforeNotes.map(n => ({
        noteNumber: n.noteNumber, startBeat: n.startBeat,
        lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
      })),
      true
    );
  }

  async redo() {
    await this.engine.injectMidiNotes(
      this.trackId,
      this.afterNotes.map(n => ({
        noteNumber: n.noteNumber, startBeat: n.startBeat,
        lengthBeats: n.lengthBeats, velocity: n.velocity, channel: n.channel || 1
      })),
      true
    );
  }
}

// ─── AudioSnapshotCommand (undo for transpose, normalize, quantize) ─
class AudioSnapshotCommand {
  constructor(engine, trackId, beforeSnapshotId, afterSnapshotId, label) {
    this.engine = engine;
    this.trackId = trackId;
    this.beforeSnapshotId = beforeSnapshotId;
    this.afterSnapshotId = afterSnapshotId;
    this.label = label || 'Audio Edit';
  }

  async execute() { /* already executed before push */ }

  async undo() {
    return await this.engine.restoreAudioSnapshot(this.trackId, this.beforeSnapshotId);
  }

  async redo() {
    return await this.engine.restoreAudioSnapshot(this.trackId, this.afterSnapshotId);
  }

  // Free native memory when this command is discarded
  async destroy() {
    if (this.beforeSnapshotId) {
      await this.engine.freeAudioSnapshot(this.beforeSnapshotId);
      this.beforeSnapshotId = null;
    }
    if (this.afterSnapshotId) {
      await this.engine.freeAudioSnapshot(this.afterSnapshotId);
      this.afterSnapshotId = null;
    }
  }
}

// ─── CreateTrackCommand ────────────────────────────────────
class CreateTrackCommand {
  constructor(engine, trackSnapshot, tracksArray, renderFn) {
    this.engine = engine;
    this.trackSnapshot = trackSnapshot;
    this.tracksArray = tracksArray;
    this.renderFn = renderFn;
    this.label = 'Create Track';
  }

  async execute() { /* already executed before push */ }

  async undo() {
    const id = this.trackSnapshot.id;
    if (this.trackSnapshot.type === 'midi') {
      await this.engine.removeMidiTrack(id);
    } else {
      await this.engine.removeTrack(id);
    }
    const idx = this.tracksArray.findIndex(t => t.id === id);
    if (idx !== -1) this.tracksArray.splice(idx, 1);
    this.renderFn();
  }

  async redo() {
    let result;
    if (this.trackSnapshot.type === 'midi') {
      result = await this.engine.addMidiTrack(this.trackSnapshot.name);
    } else {
      result = await this.engine.addTrack(this.trackSnapshot.name);
    }
    const newId = result.trackId;
    const restored = { ...this.trackSnapshot, id: newId };
    this.trackSnapshot = restored;
    this.tracksArray.push(restored);
    this.renderFn();
  }
}

// ─── RemoveTrackCommand ────────────────────────────────────
class RemoveTrackCommand {
  constructor(engine, trackSnapshot, noteSnapshot, insertionIndex, tracksArray, renderFn) {
    this.engine = engine;
    this.trackSnapshot = trackSnapshot;
    this.noteSnapshot = noteSnapshot;
    this.insertionIndex = insertionIndex;
    this.tracksArray = tracksArray;
    this.renderFn = renderFn;
    this.label = 'Delete Track';
  }

  async execute() { /* already executed before push */ }

  async undo() {
    const snap = this.trackSnapshot;
    let result;
    if (snap.type === 'midi') {
      result = await this.engine.addMidiTrack(snap.name);
    } else {
      result = await this.engine.addTrack(snap.name);
    }
    const newId = result.trackId;

    // Restore notes
    if (snap.type === 'midi' && this.noteSnapshot && this.noteSnapshot.length > 0) {
      await this.engine.injectMidiNotes(newId, this.noteSnapshot, true);
    }

    // Restore instrument
    if (snap.type === 'midi' && snap.instrumentType === 'sfzInstrument' && snap.sfzPresetId) {
      await this.engine.loadSFZPreset(newId, snap.sfzPresetId);
    } else if (snap.type === 'midi' && snap.instrumentType && snap.instrumentType !== 'sfzInstrument') {
      await this.engine.setMidiTrackBuiltInInstrument(newId, snap.instrumentType);
    }

    // Restore mixer state
    if (snap.type === 'midi') {
      await this.engine.setMidiTrackVolume(newId, snap.volume != null ? snap.volume : 0.8);
      await this.engine.setMidiTrackPan(newId, snap.pan != null ? snap.pan : 0);
      if (snap.muted) await this.engine.setMidiTrackMute(newId, true);
      if (snap.solo) await this.engine.setMidiTrackSolo(newId, true);
    } else {
      await this.engine.setTrackVolume(newId, snap.volume != null ? snap.volume : 0.8);
      await this.engine.setTrackPan(newId, snap.pan != null ? snap.pan : 0);
      if (snap.muted) await this.engine.setTrackMute(newId, true);
      if (snap.solo) await this.engine.setTrackSolo(newId, true);
    }

    // Rebuild JS track
    const restored = { ...snap, id: newId, notes: [] };
    this.trackSnapshot = restored;

    // Insert at original position
    if (this.insertionIndex >= 0 && this.insertionIndex <= this.tracksArray.length) {
      this.tracksArray.splice(this.insertionIndex, 0, restored);
    } else {
      this.tracksArray.push(restored);
    }

    this.renderFn();
  }

  async redo() {
    const id = this.trackSnapshot.id;
    if (this.trackSnapshot.type === 'midi') {
      await this.engine.removeMidiTrack(id);
    } else {
      await this.engine.removeTrack(id);
    }
    const idx = this.tracksArray.findIndex(t => t.id === id);
    if (idx !== -1) {
      this.insertionIndex = idx;
      this.tracksArray.splice(idx, 1);
    }
    this.renderFn();
  }
}
