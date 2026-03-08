// ═══════════════════════════════════════════════════════════
//  SOUNDER — Mixer Board (Logic Pro Style)
// ═══════════════════════════════════════════════════════════

class MixerBoard {
  constructor(engine) {
    this.engine = engine;
    this.isOpen = false;
    this.tracksRef = null;
    this.busTracksRef = null;

    // DOM
    this.overlay = document.getElementById('mixer-overlay');
    this.stripsContainer = document.getElementById('mixer-strips');
    this.masterContainer = document.querySelector('.mixer-master-strip');
    this._peakHolds = new Map();

    this._bindEvents();
  }

  // ─── Open / Close ──────────────────────────────────

  open(tracks, busTracks) {
    this.tracksRef = tracks;
    this.busTracksRef = busTracks || [];
    this.isOpen = true;
    this.overlay.classList.add('open');
    this.render();
  }

  close() {
    this.isOpen = false;
    this.overlay.classList.remove('open');
  }

  // ─── Render ────────────────────────────────────────

  render() {
    if (!this.isOpen || !this.tracksRef || !this.stripsContainer) return;
    this.stripsContainer.innerHTML = '';

    // Channel strips for audio/midi tracks
    for (const track of this.tracksRef) {
      this.stripsContainer.appendChild(this._createChannelStrip(track));
    }

    // Bus strips
    for (const bus of this.busTracksRef) {
      this.stripsContainer.appendChild(this._createBusStrip(bus));
    }

    // Master strip (rendered via JS now)
    this._renderMasterStrip();

    // Load insert chain info for all strips
    this._loadAllInserts();

    requestAnimationFrame(() => this._sizeFaders());
  }

  _sizeFaders() {
    this.overlay.querySelectorAll('.mixer-fader-meter-area').forEach(area => {
      const h = area.clientHeight;
      const fader = area.querySelector('.mixer-fader');
      if (fader && h > 0) fader.style.height = (h - 8) + 'px';
    });
  }

  // ─── Insert Slots HTML ────────────────────────────

  _insertSlotsHTML(trackId, trackType) {
    let html = '<div class="mixer-insert-chain" data-track-id="' + trackId + '" data-track-type="' + trackType + '">';
    html += '<div class="mixer-insert-label">Inserts</div>';
    for (let i = 0; i < 5; i++) {
      html += `<div class="mixer-insert-slot" data-slot="${i}">
        <span class="mixer-insert-name">\u2014 empty \u2014</span>
        <span class="mixer-insert-add">+</span>
      </div>`;
    }
    html += '</div>';
    return html;
  }

  _masterInsertSlotsHTML() {
    let html = '<div class="mixer-insert-chain" data-track-type="master">';
    html += '<div class="mixer-insert-label">Inserts</div>';
    for (let i = 0; i < 5; i++) {
      html += `<div class="mixer-insert-slot" data-slot="${i}">
        <span class="mixer-insert-name">\u2014 empty \u2014</span>
        <span class="mixer-insert-add">+</span>
      </div>`;
    }
    html += '</div>';
    return html;
  }

  // ─── Load Insert Info ──────────────────────────────

  async _loadAllInserts() {
    const strips = this.stripsContainer.querySelectorAll('.mixer-strip, .mixer-bus-strip');
    for (const strip of strips) {
      const chain = strip.querySelector('.mixer-insert-chain');
      if (!chain) continue;
      const trackId = parseInt(chain.dataset.trackId);
      const trackType = chain.dataset.trackType;
      await this._refreshMixerInserts(chain, trackId, trackType);
    }

    // Master insert chain
    if (this.masterContainer) {
      const masterChain = this.masterContainer.querySelector('.mixer-insert-chain');
      if (masterChain) {
        await this._refreshMixerInserts(masterChain, null, 'master');
      }
    }
  }

  async _refreshMixerInserts(chain, trackId, trackType) {
    let info;
    if (trackType === 'master') info = await this.engine.getMasterInsertChainInfo();
    else if (trackType === 'midi') info = await this.engine.getMidiInsertChainInfo(trackId);
    else if (trackType === 'bus') info = await this.engine.getBusInsertChainInfo(trackId);
    else info = await this.engine.getInsertChainInfo(trackId);

    if (!info || !Array.isArray(info)) return;

    const slots = chain.querySelectorAll('.mixer-insert-slot');
    slots.forEach((slot, i) => {
      const data = info[i];
      const nameEl = slot.querySelector('.mixer-insert-name');
      const addEl = slot.querySelector('.mixer-insert-add');
      const existingRemove = slot.querySelector('.mixer-insert-remove');
      if (existingRemove) existingRemove.remove();

      if (data && data.name) {
        slot.classList.add('occupied');
        nameEl.textContent = data.name;
        nameEl.style.cursor = 'pointer';
        addEl.style.display = 'none';

        const removeBtn = document.createElement('span');
        removeBtn.className = 'mixer-insert-remove';
        removeBtn.textContent = '\u00d7';
        removeBtn.onclick = async (e) => {
          e.stopPropagation();
          if (trackType === 'master') await this.engine.removeMasterInsert(i);
          else if (trackType === 'midi') await this.engine.removeMidiInsert(trackId, i);
          else if (trackType === 'bus') await this.engine.removeBusInsert(trackId, i);
          else await this.engine.removeInsert(trackId, i);
          await this._refreshMixerInserts(chain, trackId, trackType);
          // Sync main track row (not applicable for master)
          if (trackType !== 'master') {
            const row = document.querySelector(`.track-row[data-id="${trackId}"]`);
            const track = this._findTrack(trackId, trackType);
            if (row && track && typeof refreshInsertChain === 'function') refreshInsertChain(row, track);
          }
        };
        slot.appendChild(removeBtn);

        nameEl.onclick = (e) => {
          e.stopPropagation();
          if (trackType === 'master') {
            if (data.isBuiltIn && data.effectType) {
              if (typeof openInsertParamPanel === 'function')
                openInsertParamPanel({ id: '__master__', type: 'master' }, i, data.effectType, data.name);
            }
          } else {
            const track = this._findTrack(trackId, trackType);
            if (data.isBuiltIn && data.effectType && track) {
              if (typeof openInsertParamPanel === 'function')
                openInsertParamPanel(track, i, data.effectType, data.name);
            }
          }
        };
      } else {
        slot.classList.remove('occupied');
        nameEl.textContent = '\u2014 empty \u2014';
        nameEl.style.cursor = 'default';
        nameEl.onclick = null;
        addEl.style.display = '';
        addEl.onclick = (e) => {
          e.stopPropagation();
          if (trackType === 'master') {
            if (typeof openInsertPickerModal === 'function')
              openInsertPickerModal({ id: '__master__', type: 'master' }, i);
          } else {
            const track = this._findTrack(trackId, trackType);
            if (track && typeof openInsertPickerModal === 'function')
              openInsertPickerModal(track, i);
          }
        };
      }
    });
  }

  _findTrack(trackId, trackType) {
    if (trackType === 'bus') return (this.busTracksRef || []).find(b => b.id === trackId);
    return (this.tracksRef || []).find(t => t.id === trackId);
  }

  // ─── Channel Strip ─────────────────────────────────

  _createChannelStrip(track) {
    const strip = document.createElement('div');
    strip.className = 'mixer-strip';
    strip.dataset.trackId = track.id;

    const isMidi = track.type === 'midi';
    const isBus = track.type === 'bus';
    const panDisplay = track.pan === 0 ? 'C'
      : (track.pan < 0 ? Math.round(Math.abs(track.pan) * 100) + 'L'
        : Math.round(track.pan * 100) + 'R');
    const volDb = track.volume > 0 ? (20 * Math.log10(track.volume)).toFixed(1) : '-\u221E';
    const trackIndex = this.tracksRef.indexOf(track) + 1;
    const outputBus = track.outputBus >= 0
      ? ((this.busTracksRef || []).find(b => b.id === track.outputBus)?.name || 'Bus')
      : 'St Out';

    strip.innerHTML = `
      <div class="mixer-ch-badge">
        <span class="mixer-ch-number">${trackIndex}</span>
        <span class="mixer-ch-icon">${isMidi ? '\u266A' : '\u{1F399}'}</span>
      </div>

      <div class="mixer-slots-section">
        <div class="mixer-slot-row">
          <span class="mixer-slot-label">Input</span>
          <button class="mixer-slot-btn">${isMidi ? 'MIDI' : 'In ' + trackIndex}</button>
        </div>
        <div class="mixer-slot-row">
          <span class="mixer-slot-label">Sends</span>
          <button class="mixer-slot-btn" data-role="sends">${outputBus !== 'St Out' ? outputBus : '\u2014'}</button>
        </div>
        <div class="mixer-slot-row">
          <span class="mixer-slot-label">Output</span>
          <button class="mixer-slot-btn" data-role="output">${outputBus}</button>
        </div>
      </div>

      ${this._insertSlotsHTML(track.id, track.type)}

      <div class="mixer-pan-knob-area">
        <div class="mixer-knob-wrapper">
          <div class="mixer-knob" data-role="knob-visual">
            <div class="mixer-knob-indicator"></div>
          </div>
          <input type="range" class="mixer-knob-input" min="-1" max="1" step="0.01"
                 value="${track.pan}" data-role="pan">
        </div>
        <span class="mixer-pan-val" data-role="pan-label">${panDisplay}</span>
      </div>

      <div class="mixer-db-display">
        <span class="mixer-db-current" data-role="db-current">${volDb}</span>
        <span class="mixer-db-peak" data-role="db-peak">-\u221E</span>
      </div>

      <div class="mixer-fader-meter-area">
        <div class="mixer-db-scale">
          <span data-db="6">6</span>
          <span data-db="3">3</span>
          <span data-db="0">0</span>
          <span data-db="-3">-3</span>
          <span data-db="-6">-6</span>
          <span data-db="-10">-10</span>
          <span data-db="-15">-15</span>
          <span data-db="-30">-30</span>
          <span data-db="-40">-40</span>
          <span data-db="-inf">\u221E</span>
        </div>
        <div class="mixer-meter-track">
          <div class="mixer-meter-bar-v">
            <div class="mixer-meter-fill-v" data-role="meter-l"></div>
          </div>
          <div class="mixer-meter-bar-v">
            <div class="mixer-meter-fill-v" data-role="meter-r"></div>
          </div>
        </div>
        <div class="mixer-fader-track">
          <input type="range" class="mixer-fader"
                 min="0" max="1" step="0.01"
                 value="${track.volume}"
                 data-role="volume"
                 orient="vertical">
        </div>
        <div class="mixer-clip-dot" data-role="clip"></div>
      </div>

      <div class="mixer-ri-row">
        <button class="mixer-btn ri-btn arm-btn ${track.armed ? 'active' : ''}" data-role="arm">R</button>
        <button class="mixer-btn ri-btn" data-role="input-monitor">I</button>
      </div>

      <div class="mixer-ms-row">
        <button class="mixer-btn ms-btn mute-btn ${track.muted ? 'active' : ''}" data-role="mute">M</button>
        <button class="mixer-btn ms-btn solo-btn ${track.solo ? 'active' : ''}" data-role="solo">S</button>
      </div>

      <div class="mixer-strip-name-pill" style="background:${track.color}">
        <span class="mixer-strip-label">${track.name}</span>
      </div>
    `;

    this._bindStripEvents(strip, track, isMidi);

    const knob = strip.querySelector('[data-role="knob-visual"]');
    if (knob) knob.style.transform = `rotate(${track.pan * 135}deg)`;

    requestAnimationFrame(() => this._drawWaveformThumb(strip, track));

    return strip;
  }

  // ─── Bus Strip ─────────────────────────────────────

  _createBusStrip(bus) {
    const strip = document.createElement('div');
    strip.className = 'mixer-strip mixer-bus-strip';
    strip.dataset.trackId = bus.id;

    const panDisplay = bus.pan === 0 ? 'C'
      : (bus.pan < 0 ? Math.round(Math.abs(bus.pan) * 100) + 'L'
        : Math.round(bus.pan * 100) + 'R');
    const volDb = bus.volume > 0 ? (20 * Math.log10(bus.volume)).toFixed(1) : '-\u221E';

    strip.innerHTML = `
      <div class="mixer-ch-badge" style="background:linear-gradient(180deg,#3a4540,#2a3530);border-color:#4ade80">
        <span class="mixer-ch-number">B</span>
        <span class="mixer-ch-icon">\u{1F4E2}</span>
      </div>

      <div class="mixer-slots-section">
        <div class="mixer-slot-row">
          <span class="mixer-slot-label">Input</span>
          <button class="mixer-slot-btn">Bus In</button>
        </div>
        <div class="mixer-slot-row">
          <span class="mixer-slot-label">Output</span>
          <button class="mixer-slot-btn">St Out</button>
        </div>
      </div>

      ${this._insertSlotsHTML(bus.id, 'bus')}

      <div class="mixer-pan-knob-area">
        <div class="mixer-knob-wrapper">
          <div class="mixer-knob" data-role="knob-visual">
            <div class="mixer-knob-indicator"></div>
          </div>
          <input type="range" class="mixer-knob-input" min="-1" max="1" step="0.01"
                 value="${bus.pan}" data-role="pan">
        </div>
        <span class="mixer-pan-val" data-role="pan-label">${panDisplay}</span>
      </div>

      <div class="mixer-db-display">
        <span class="mixer-db-current" data-role="db-current">${volDb}</span>
        <span class="mixer-db-peak" data-role="db-peak">-\u221E</span>
      </div>

      <div class="mixer-fader-meter-area">
        <div class="mixer-db-scale">
          <span data-db="6">6</span><span data-db="0">0</span><span data-db="-6">-6</span>
          <span data-db="-15">-15</span><span data-db="-30">-30</span><span data-db="-inf">\u221E</span>
        </div>
        <div class="mixer-meter-track">
          <div class="mixer-meter-bar-v"><div class="mixer-meter-fill-v" data-role="meter-l"></div></div>
          <div class="mixer-meter-bar-v"><div class="mixer-meter-fill-v" data-role="meter-r"></div></div>
        </div>
        <div class="mixer-fader-track">
          <input type="range" class="mixer-fader" min="0" max="1" step="0.01"
                 value="${bus.volume}" data-role="volume" orient="vertical">
        </div>
        <div class="mixer-clip-dot" data-role="clip"></div>
      </div>

      <div class="mixer-ms-row">
        <button class="mixer-btn ms-btn mute-btn ${bus.muted ? 'active' : ''}" data-role="mute">M</button>
        <button class="mixer-btn ms-btn solo-btn ${bus.solo ? 'active' : ''}" data-role="solo">S</button>
      </div>

      <div class="mixer-strip-name-pill" style="background:#4ade80;color:#000">
        <span class="mixer-strip-label">${bus.name}</span>
      </div>
    `;

    this._bindBusStripEvents(strip, bus);

    const knob = strip.querySelector('[data-role="knob-visual"]');
    if (knob) knob.style.transform = `rotate(${bus.pan * 135}deg)`;

    return strip;
  }

  // ─── Master Strip (JS-rendered) ────────────────────

  _renderMasterStrip() {
    if (!this.masterContainer) return;
    const transportMaster = document.getElementById('master-volume');
    const masterVol = transportMaster ? parseFloat(transportMaster.value) : 0.8;
    const volDb = masterVol > 0 ? (20 * Math.log10(masterVol)).toFixed(1) : '-\u221E';

    this.masterContainer.innerHTML = `
      <div class="mixer-ch-badge master-badge">
        <span class="mixer-ch-number">M</span>
        <span class="mixer-ch-icon">\u2605</span>
      </div>

      <div class="mixer-slots-section">
        <div class="mixer-slot-row">
          <span class="mixer-slot-label">Output</span>
          <button class="mixer-slot-btn">Stereo</button>
        </div>
      </div>

      ${this._masterInsertSlotsHTML()}

      <div class="mixer-db-display">
        <span class="mixer-db-current" id="mixer-master-vol-label">${volDb}</span>
        <span class="mixer-db-peak" id="mixer-master-peak-db">-\u221E</span>
      </div>

      <div class="mixer-fader-meter-area">
        <div class="mixer-db-scale">
          <span>6</span><span>0</span><span>-6</span><span>-12</span><span>-24</span><span>-48</span><span>\u221E</span>
        </div>
        <div class="mixer-meter-track">
          <div class="mixer-meter-bar-v">
            <div class="mixer-meter-fill-v" id="mixer-master-meter-l"></div>
          </div>
          <div class="mixer-meter-bar-v">
            <div class="mixer-meter-fill-v" id="mixer-master-meter-r"></div>
          </div>
        </div>
        <div class="mixer-fader-track">
          <input type="range" class="mixer-fader" id="mixer-master-fader"
                 min="0" max="1" step="0.01" value="${masterVol}" orient="vertical">
        </div>
        <div class="mixer-clip-dot" id="mixer-master-clip"></div>
      </div>

      <div class="mixer-strip-name-pill" style="background:#666;color:#fff">
        <span class="mixer-strip-label">Stereo Out</span>
      </div>
    `;

    // Re-bind master DOM refs
    this.masterFader = document.getElementById('mixer-master-fader');
    this.masterMeterL = document.getElementById('mixer-master-meter-l');
    this.masterMeterR = document.getElementById('mixer-master-meter-r');
    this.masterPeakDb = document.getElementById('mixer-master-peak-db');

    // Rebind master fader
    if (this.masterFader) {
      this.masterFader.oninput = (e) => {
        this.engine.setMasterVolume(parseFloat(e.target.value));
        const transportMaster = document.getElementById('master-volume');
        if (transportMaster) transportMaster.value = e.target.value;
        const label = document.getElementById('mixer-master-vol-label');
        if (label) {
          const v = parseFloat(e.target.value);
          label.textContent = v > 0 ? (20 * Math.log10(v)).toFixed(1) : '-\u221E';
        }
      };
    }

    // Peak hold reset
    if (this.masterPeakDb) {
      this.masterPeakDb.style.cursor = 'pointer';
      this.masterPeakDb.title = 'Click to reset peak';
      this.masterPeakDb.onclick = () => {
        this._peakHolds.delete('__master__');
        this.masterPeakDb.textContent = '-\u221E';
        this.masterPeakDb.style.color = 'var(--text-dim)';
      };
    }
  }

  // ─── Waveform Thumbnail ────────────────────────────

  _drawWaveformThumb(strip, track) {
    const canvas = strip.querySelector('[data-role="waveform-thumb"]');
    if (!canvas || !track.waveformData) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    const data = track.waveformData;
    const step = Math.max(1, Math.floor(data.length / w));
    ctx.beginPath();
    for (let i = 0; i < w; i++) {
      const idx = Math.min(i * step, data.length - 1);
      const val = Math.abs(data[idx]);
      const y = (h / 2) - (val * h / 2);
      if (i === 0) ctx.moveTo(i, y);
      else ctx.lineTo(i, y);
    }
    for (let i = w - 1; i >= 0; i--) {
      const idx = Math.min(i * step, data.length - 1);
      const val = Math.abs(data[idx]);
      ctx.lineTo(i, (h / 2) + (val * h / 2));
    }
    ctx.closePath();
    ctx.fillStyle = track.color + '66';
    ctx.fill();
    ctx.strokeStyle = track.color;
    ctx.lineWidth = 1;
    ctx.stroke();
  }

  // ─── Strip Events ──────────────────────────────────

  _bindStripEvents(strip, track, isMidi) {
    // Volume fader
    strip.querySelector('[data-role="volume"]').oninput = (e) => {
      track.volume = parseFloat(e.target.value);
      const dbEl = strip.querySelector('[data-role="db-current"]');
      if (dbEl) dbEl.textContent = track.volume > 0 ? (20 * Math.log10(track.volume)).toFixed(1) : '-\u221E';
      this._syncTrackRowVolume(track);
      if (e.target._rafId) cancelAnimationFrame(e.target._rafId);
      e.target._rafId = requestAnimationFrame(() => {
        e.target._rafId = null;
        if (isMidi) this.engine.setMidiTrackVolume(track.id, track.volume);
        else this.engine.setTrackVolume(track.id, track.volume);
      });
    };

    // Peak hold reset on click
    const dbPeakEl = strip.querySelector('[data-role="db-peak"]');
    if (dbPeakEl) {
      dbPeakEl.title = 'Click to reset peak';
      dbPeakEl.onclick = () => {
        this._peakHolds.delete(track.id);
        dbPeakEl.textContent = '-\u221E';
        dbPeakEl.style.color = 'var(--meter-green)';
      };
    }

    // Pan knob
    strip.querySelector('[data-role="pan"]').oninput = (e) => {
      track.pan = parseFloat(e.target.value);
      const d = track.pan === 0 ? 'C'
        : (track.pan < 0 ? Math.round(Math.abs(track.pan) * 100) + 'L'
          : Math.round(track.pan * 100) + 'R');
      strip.querySelector('[data-role="pan-label"]').textContent = d;
      const knob = strip.querySelector('[data-role="knob-visual"]');
      if (knob) knob.style.transform = `rotate(${track.pan * 135}deg)`;
      this._syncTrackRowPan(track);
      if (e.target._rafId) cancelAnimationFrame(e.target._rafId);
      e.target._rafId = requestAnimationFrame(() => {
        e.target._rafId = null;
        if (isMidi) this.engine.setMidiTrackPan(track.id, track.pan);
        else this.engine.setTrackPan(track.id, track.pan);
      });
    };

    // Mute
    strip.querySelector('[data-role="mute"]').onclick = async () => {
      track.muted = !track.muted;
      if (isMidi) await this.engine.setMidiTrackMute(track.id, track.muted);
      else await this.engine.setTrackMute(track.id, track.muted);
      this.render();
      if (typeof renderAllTracks === 'function') renderAllTracks();
    };

    // Solo
    strip.querySelector('[data-role="solo"]').onclick = async () => {
      track.solo = !track.solo;
      if (isMidi) await this.engine.setMidiTrackSolo(track.id, track.solo);
      else await this.engine.setTrackSolo(track.id, track.solo);
      this.render();
      if (typeof renderAllTracks === 'function') renderAllTracks();
    };

    // Arm
    strip.querySelector('[data-role="arm"]').onclick = async () => {
      track.armed = !track.armed;
      if (isMidi) await this.engine.setMidiTrackArmed(track.id, track.armed);
      else await this.engine.setTrackArmed(track.id, track.armed);
      this.render();
      if (typeof renderAllTracks === 'function') renderAllTracks();
    };

    // Input monitor (placeholder)
    const inputMonBtn = strip.querySelector('[data-role="input-monitor"]');
    if (inputMonBtn) {
      inputMonBtn.onclick = () => inputMonBtn.classList.toggle('active');
    }

    // Sends button - route to bus
    const sendsBtn = strip.querySelector('[data-role="sends"]');
    if (sendsBtn) {
      sendsBtn.onclick = () => this._openSendPicker(sendsBtn, track, isMidi);
    }

    // Output button - also route to bus
    const outputBtn = strip.querySelector('[data-role="output"]');
    if (outputBtn) {
      outputBtn.onclick = () => this._openSendPicker(outputBtn, track, isMidi);
    }
  }

  // ─── Bus Strip Events ─────────────────────────────

  _bindBusStripEvents(strip, bus) {
    strip.querySelector('[data-role="volume"]').oninput = (e) => {
      bus.volume = parseFloat(e.target.value);
      const dbEl = strip.querySelector('[data-role="db-current"]');
      if (dbEl) dbEl.textContent = bus.volume > 0 ? (20 * Math.log10(bus.volume)).toFixed(1) : '-\u221E';
      if (e.target._rafId) cancelAnimationFrame(e.target._rafId);
      e.target._rafId = requestAnimationFrame(() => {
        e.target._rafId = null;
        this.engine.setBusTrackVolume(bus.id, bus.volume);
      });
    };

    const dbPeakEl = strip.querySelector('[data-role="db-peak"]');
    if (dbPeakEl) {
      dbPeakEl.title = 'Click to reset peak';
      dbPeakEl.onclick = () => {
        this._peakHolds.delete(bus.id);
        dbPeakEl.textContent = '-\u221E';
        dbPeakEl.style.color = 'var(--meter-green)';
      };
    }

    strip.querySelector('[data-role="pan"]').oninput = (e) => {
      bus.pan = parseFloat(e.target.value);
      const d = bus.pan === 0 ? 'C'
        : (bus.pan < 0 ? Math.round(Math.abs(bus.pan) * 100) + 'L'
          : Math.round(bus.pan * 100) + 'R');
      strip.querySelector('[data-role="pan-label"]').textContent = d;
      const knob = strip.querySelector('[data-role="knob-visual"]');
      if (knob) knob.style.transform = `rotate(${bus.pan * 135}deg)`;
      if (e.target._rafId) cancelAnimationFrame(e.target._rafId);
      e.target._rafId = requestAnimationFrame(() => {
        e.target._rafId = null;
        this.engine.setBusTrackPan(bus.id, bus.pan);
      });
    };

    strip.querySelector('[data-role="mute"]').onclick = async () => {
      bus.muted = !bus.muted;
      await this.engine.setBusTrackMute(bus.id, bus.muted);
      this.render();
    };

    strip.querySelector('[data-role="solo"]').onclick = async () => {
      bus.solo = !bus.solo;
      await this.engine.setBusTrackSolo(bus.id, bus.solo);
      this.render();
    };
  }

  // ─── Send Picker (route track to bus) ──────────────

  _openSendPicker(btn, track, isMidi) {
    // Remove any existing picker
    document.querySelectorAll('.mixer-send-picker').forEach(p => p.remove());

    const picker = document.createElement('div');
    picker.className = 'mixer-send-picker';

    // "No Bus (St Out)" option
    const noneOpt = document.createElement('div');
    noneOpt.className = 'mixer-send-opt' + (track.outputBus < 0 ? ' active' : '');
    noneOpt.textContent = 'Stereo Out';
    noneOpt.onclick = async () => {
      track.outputBus = -1;
      if (isMidi) await this.engine.setMidiTrackOutput(track.id, -1);
      else await this.engine.setTrackOutput(track.id, -1);
      picker.remove();
      this.render();
      if (typeof renderAllTracks === 'function') renderAllTracks();
    };
    picker.appendChild(noneOpt);

    // Bus options
    for (const bus of (this.busTracksRef || [])) {
      const opt = document.createElement('div');
      opt.className = 'mixer-send-opt' + (track.outputBus === bus.id ? ' active' : '');
      opt.textContent = bus.name;
      opt.onclick = async () => {
        track.outputBus = bus.id;
        if (isMidi) await this.engine.setMidiTrackOutput(track.id, bus.id);
        else await this.engine.setTrackOutput(track.id, bus.id);
        picker.remove();
        this.render();
        if (typeof renderAllTracks === 'function') renderAllTracks();
      };
      picker.appendChild(opt);
    }

    // Position near button
    const rect = btn.getBoundingClientRect();
    picker.style.position = 'fixed';
    picker.style.left = rect.left + 'px';
    picker.style.top = (rect.bottom + 2) + 'px';
    document.body.appendChild(picker);

    // Close on outside click
    const close = (e) => {
      if (!picker.contains(e.target) && e.target !== btn) {
        picker.remove();
        document.removeEventListener('mousedown', close);
      }
    };
    setTimeout(() => document.addEventListener('mousedown', close), 0);
  }

  // ─── Track Row Sync ────────────────────────────────

  _syncTrackRowVolume(track) {
    const row = document.querySelector(`.track-row[data-id="${track.id}"]`);
    if (!row) return;
    const volSlider = row.querySelector('[data-action="volume"]');
    if (volSlider) volSlider.value = track.volume;
    const valEl = volSlider?.parentElement?.querySelector('.val');
    if (valEl) valEl.textContent = Math.round(track.volume * 100);
  }

  _syncTrackRowPan(track) {
    const row = document.querySelector(`.track-row[data-id="${track.id}"]`);
    if (!row) return;
    const panSlider = row.querySelector('[data-action="pan"]');
    if (panSlider) panSlider.value = track.pan;
    const valEl = panSlider?.parentElement?.querySelector('.val');
    if (valEl) {
      valEl.textContent = track.pan === 0 ? 'C'
        : (track.pan < 0 ? Math.round(Math.abs(track.pan) * 100) + 'L'
          : Math.round(track.pan * 100) + 'R');
    }
  }

  // ─── Meter Updates ─────────────────────────────────

  _peakToMeterPct(peak) {
    if (peak < 0.001) return 0;
    const db = 20 * Math.log10(peak);
    if (db < -60) return 0;
    if (db > 6) return 100;
    return ((db + 60) / 66) * 100;
  }

  updateMeters(data) {
    if (!this.isOpen) return;

    // Master meters
    const masterPctL = this._peakToMeterPct(data.masterPeakL || 0);
    const masterPctR = this._peakToMeterPct(data.masterPeakR || 0);
    if (this.masterMeterL) this.masterMeterL.style.height = (100 - masterPctL) + '%';
    if (this.masterMeterR) this.masterMeterR.style.height = (100 - masterPctR) + '%';

    // Master peak hold
    const masterPeakVal = Math.max(data.masterPeakL || 0, data.masterPeakR || 0);
    const masterHeld = this._peakHolds.get('__master__') || 0;
    if (masterPeakVal > masterHeld) this._peakHolds.set('__master__', masterPeakVal);
    const masterDisplayPeak = this._peakHolds.get('__master__') || 0;
    const masterPkDb = masterDisplayPeak > 0.001 ? 20 * Math.log10(masterDisplayPeak) : -Infinity;
    if (this.masterPeakDb) {
      this.masterPeakDb.textContent = masterPkDb > -0.5 ? 'CLIP' : (masterPkDb > -60 ? masterPkDb.toFixed(1) + ' dB' : '-\u221E dB');
      this.masterPeakDb.style.color = masterPkDb > -0.5 ? 'var(--meter-red)' : 'var(--text-dim)';
    }

    // Per-track meters
    for (const tm of (data.tracks || [])) {
      const strip = this.stripsContainer?.querySelector(`.mixer-strip[data-track-id="${tm.trackId}"]`);
      if (!strip) continue;
      const meterL = strip.querySelector('[data-role="meter-l"]');
      const meterR = strip.querySelector('[data-role="meter-r"]');
      const clipDot = strip.querySelector('[data-role="clip"]');
      if (!meterL) continue;

      const pctL = this._peakToMeterPct(tm.peakL || 0);
      const pctR = this._peakToMeterPct(tm.peakR || 0);
      meterL.style.height = (100 - pctL) + '%';
      meterR.style.height = (100 - pctR) + '%';

      if (tm.clipping && clipDot) clipDot.classList.add('clip');

      const dbPeakEl = strip.querySelector('[data-role="db-peak"]');
      if (dbPeakEl) {
        const peakVal = Math.max(tm.peakL || 0, tm.peakR || 0);
        const key = tm.trackId;
        const held = this._peakHolds.get(key) || 0;
        if (peakVal > held) this._peakHolds.set(key, peakVal);
        const displayPeak = this._peakHolds.get(key) || 0;
        const pkDb = displayPeak > 0.001 ? (20 * Math.log10(displayPeak)).toFixed(1) : '-\u221E';
        dbPeakEl.textContent = pkDb;
        dbPeakEl.style.color = displayPeak >= 1.0 ? 'var(--meter-red)' :
                                displayPeak > 0.5 ? 'var(--meter-yellow)' : 'var(--meter-green)';
      }
    }

    // Bus meters
    for (const bm of (data.busTracks || [])) {
      const strip = this.stripsContainer?.querySelector(`.mixer-bus-strip[data-track-id="${bm.trackId}"]`);
      if (!strip) continue;
      const meterL = strip.querySelector('[data-role="meter-l"]');
      const meterR = strip.querySelector('[data-role="meter-r"]');
      if (!meterL) continue;
      const pctL = this._peakToMeterPct(bm.peakL || 0);
      const pctR = this._peakToMeterPct(bm.peakR || 0);
      meterL.style.height = (100 - pctL) + '%';
      meterR.style.height = (100 - pctR) + '%';
    }
  }

  // ─── Events ────────────────────────────────────────

  _bindEvents() {
    document.getElementById('mixer-close')?.addEventListener('click', () => this.close());

    window.addEventListener('resize', () => {
      if (this.isOpen) this._sizeFaders();
    });
  }
}
