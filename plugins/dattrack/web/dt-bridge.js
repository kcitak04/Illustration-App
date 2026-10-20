// DATTRACK plugin bridge. Loaded before DATTRACK's own scripts when the app runs inside the VST/AU.
//  - every AudioContext is created at the host sample rate without a speaker output (Chromium) or muted (WebKit);
//    whatever DATTRACK sends to the speakers goes through a tap worklet to the plugin instead
//  - exported files (WAV/AIFF/FLAC/Opus, .mtk, .MOD) are handed to the plugin through the Android-bridge API DATTRACK already has
//  - SLAVE: Ableton's tempo, play/stop and position drive the tracker; MASTER: the tracker's state is reported for Ableton Link
//  - the song is mirrored into the plugin state, so it is saved inside the Ableton set
(function () {
  'use strict';
  const Q = new URLSearchParams(location.search), INST = Q.get('i') || '0', TOKEN = Q.get('t') || '', SR = +Q.get('sr') || 0;
  const url = (route, extra) => location.origin + '/' + route + '/' + INST + '?t=' + TOKEN + (extra || '');
  const DT = window.DTVST = { inst: INST, ctx: null, host: null, songSeq: -1, jumpSeen: -1, mixBpm: 0, lastSpeed: 0, slaving: false, lockBpm: 0,
    hostSongApplied: false, lastDirty: -1, dirtySeen: -1, dirtyAt: 0, started: false, log: [],
    loop: null, hostRow: 0 };   // loop: the host's loop in rows {s, e}; hostRow: the host row DATTRACK is playing
  const say = m => { try { if (typeof toast === 'function') toast(m, 4000); } catch (e) { } };

  // ---------------------------------------------------------------- exports -> plugin (Music/DATTRACK)
  let saveName = '', saveParts = [];
  window.AndroidBridge = {
    saveBegin(name) { saveName = name; saveParts = []; },
    saveChunk(b64) { saveParts.push(b64); },
    saveEnd() {
      const parts = saveParts.map(b => { const s = atob(b), u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); return u; });
      saveParts = [];
      fetch(url('save', '&name=' + encodeURIComponent(saveName)), { method: 'POST', body: new Blob(parts) })
        .then(r => r.json()).then(j => say('Saved ' + j.name + ' to Music/DATTRACK - drag it from the plugin bar into Ableton'))
        .catch(e => say('Save failed: ' + e.message));
      return true;
    },
    keepScreenOn() { },
  };

  // ---------------------------------------------------------------- audio: AudioContext -> tap -> plugin
  const NativeAC = window.AudioContext || window.webkitAudioContext;
  const origConnect = AudioNode.prototype.connect, origDisconnect = AudioNode.prototype.disconnect;
  const fwd = new Worker(URL.createObjectURL(new Blob([`
    'use strict';
    let port = null, sr = 48000, q = [], busy = false, info = '', base = '';
    onmessage = e => {
      const m = e.data;
      if (m.type === 'port') { port = m.port; sr = m.sr; base = m.base; port.onmessage = ev => { q.push(ev.data); pump(); }; }
      else if (m.type === 'info') info = m.q;
    };
    async function pump() {
      if (busy || !q.length) return;
      busy = true;
      // contiguous chunks go out in one request
      const chunks = q.splice(0, Math.min(q.length, 16)), f0 = chunks[0].frame;
      let n = 0; for (const c of chunks) n += c.data.length;
      const body = new Float32Array(n); let o = 0; for (const c of chunks) { body.set(c.data, o); o += c.data.length; }
      try {
        const r = await fetch(base + '&f=' + f0 + '&sr=' + sr + info, { method: 'POST', body, headers: { 'Content-Type': 'application/octet-stream' } });
        if (r.ok) postMessage(await r.json());
      } catch (e) { await new Promise(res => setTimeout(res, 250)); }
      busy = false;
      if (q.length > 400) q.splice(0, q.length - 40);   // plugin unreachable for a long time: drop the backlog
      if (q.length) pump();
    }
  `], { type: 'text/javascript' })));
  fwd.onmessage = e => { try { onHost(e.data); } catch (err) { DT.log.push(String(err && err.stack || err)); } };

  function hook(ctx) {
    DT.ctx = ctx;
    const bus = ctx.createGain(), mon = ctx.createGain(), zero = ctx.createGain();
    mon.gain.value = 0; zero.gain.value = 0;   // never to the speakers: the host plays it
    ctx.__dtBus = bus;
    origConnect.call(bus, mon); origConnect.call(mon, ctx.destination);
    ctx.audioWorklet.addModule(location.origin + '/dt-tap.js').then(() => {
      const tap = new AudioWorkletNode(ctx, 'dt-tap', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 2, channelCountMode: 'explicit' });
      origConnect.call(bus, tap); origConnect.call(tap, zero); origConnect.call(zero, ctx.destination);
      const ch = new MessageChannel();
      tap.port.postMessage({ port: ch.port1 }, [ch.port1]);
      fwd.postMessage({ type: 'port', port: ch.port2, sr: ctx.sampleRate, base: url('a') }, [ch.port2]);
    }).catch(e => say('Plugin audio tap failed: ' + e.message));
  }
  function PluginAudioContext(opts) {
    const o = Object.assign({}, opts || {}, { latencyHint: 'interactive' });
    if (SR) o.sampleRate = SR;
    let ctx = null;
    try { ctx = new NativeAC(Object.assign({ sinkId: { type: 'none' } }, o)); } catch (e) { ctx = null; }   // Chromium: render without a device
    if (!ctx) { try { ctx = new NativeAC(o); } catch (e) { delete o.sampleRate; ctx = new NativeAC(o); } }
    hook(ctx);
    return ctx;
  }
  PluginAudioContext.prototype = NativeAC.prototype;
  window.AudioContext = PluginAudioContext;
  if (window.webkitAudioContext) window.webkitAudioContext = PluginAudioContext;
  AudioNode.prototype.connect = function (dest, ...rest) {
    if (dest instanceof AudioDestinationNode && dest.context.__dtBus) return origConnect.call(this, dest.context.__dtBus, ...rest);
    return origConnect.call(this, dest, ...rest);
  };
  AudioNode.prototype.disconnect = function (dest, ...rest) {
    if (dest instanceof AudioDestinationNode && dest.context.__dtBus) return origDisconnect.call(this, dest.context.__dtBus, ...rest);
    return origDisconnect.apply(this, arguments);
  };

  // ---------------------------------------------------------------- helpers into DATTRACK
  const ready = () => { try { return engine.ready && !!patView; } catch (e) { return false; } };
  const songRows = () => song.order.reduce((n, p) => n + (song.patterns[p] || song.patterns[0]).rows, 0);
  function rowToPos(r) {
    for (let o = 0; o < song.order.length; o++) {
      const rows = (song.patterns[song.order[o]] || song.patterns[0]).rows;
      if (r < rows) return { order: o, row: r };
      r -= rows;
    }
    return { order: 0, row: 0 };
  }
  function posToRow(order, row) { let r = 0; for (let o = 0; o < order && o < song.order.length; o++) r += (song.patterns[song.order[o]] || song.patterns[0]).rows; return r + row; }
  // DATTRACK beat = 4 rows = 4 * speed * 2.5 / bpm seconds, so its "real" tempo is bpm * 6 / speed
  const effBpm = () => sched.playing ? player.bpm * 6 / player.speed : song.bpm * 6 / song.speed;

  // In SLAVE the player's tempo is locked to the host (Fxx tempo changes are ignored, speed changes keep the beat)
  function lockPlayerTempo() {
    if (player.__dtLocked) return;
    let own = player.bpm;
    Object.defineProperty(player, 'bpm', {
      configurable: true,
      get() { return DT.lockBpm > 0 ? DT.lockBpm * this.speed / 6 : own; },
      set(v) { own = v; },
    });
    player.__dtLocked = true;
  }
  // H = host tempo, C = the same tempo on the page's own clock (C = H / clock rate, see AudioBridge)
  function setHostTempo(H, C) {
    const spd = sched.playing ? player.speed : song.speed;
    DT.lockBpm = C;
    if (Math.abs(C - DT.mixBpm) < 0.005 && spd === DT.lastSpeed) return;
    DT.mixBpm = C; DT.lastSpeed = spd;
    const sb = Math.max(32, Math.min(255, Math.round(H * song.speed / 6)));
    if (song.bpm !== sb) { song.bpm = sb; try { refreshAll(); } catch (e) { } }
    engine.mixer.setTempo(C * spd / 6, spd);   // delays and WARP samples follow too
  }
  // The host's loop (Ableton's loop brace), in rows. Only a loop on the row grid can be followed row-exactly.
  function loopOf(j) {
    if (!j.loop) return null;
    const s = j.loop[0] * 4, e = j.loop[1] * 4, S = Math.round(s), E = Math.round(e);
    if (Math.abs(s - S) > 0.01 || Math.abs(e - E) > 0.01 || E - S < 1) return null;
    if (DT.loopDistrust === S + ':' + E) return null;   // reported but not looping (e.g. Session view): just follow
    return { s: S, e: E };
  }
  // DATTRACK looped on its own: the host must jump at the same moment (its report may come before or after
  // DATTRACK's own jump), or this loop is not really on
  function checkLoopWrap(j) {
    const w = DT.wrapPending; if (!w) return;
    if (DT.hostJumpT != null && Math.abs(DT.hostJumpT - w.t) < 0.15) DT.wrapPending = null;
    else if (j.f / engine.ctx.sampleRate > w.t + 0.08) {
      DT.wrapPending = null;
      if (DT.loop) DT.loopDistrust = DT.loop.s + ':' + DT.loop.e;
      DT.loop = null; DT.log.push('host did not loop');
    }
  }
  // a host row past the loop end is the loop start again (if the host was before the loop end, it loops there)
  function wrapRow(hr, from) {
    const L = DT.loop;
    if (L && from < L.e - 1e-6 && hr >= L.e) hr = L.s + (hr - L.e) % (L.e - L.s);
    return hr;
  }
  // DATTRACK jumps back to the loop start on the same row as the host, ahead of time (the host reports its jump only
  // after it happened, a round trip too late to cue the first row of the next pass)
  function followHostLoop() {
    if (player.__dtLoop) return;
    const next = player.nextRow, step = player.step;
    player.step = function (time, evs) {   // every scheduled row remembers the host row it stands for
      DT.stepTime = time;
      const info = step.call(this, time, evs);
      if (info) info.hr = DT.hostRow;
      return info;
    };
    player.nextRow = function () {
      next.call(this);
      if (!DT.slaving) return;
      DT.hostRow++;
      const L = DT.loop;
      if (L && DT.hostRow === L.e) {
        DT.hostRow = L.s;
        DT.wrapPending = { t: DT.stepTime + this.tickDur };
        const total = songRows(), p = rowToPos(((L.s % total) + total) % total);
        if (this.mode === 'song') this.setOrder(p.order);
        this.row = p.row;
      }
    };
    player.__dtLoop = true;
  }
  // start the song at (order, row) with its first row exactly at context time t
  function startAt(order, row, t) {
    sched.halt();
    const ctx = engine.ctx; ctx.resume();
    player.loop = state.loop;
    player.start(song, order, row, 'song');
    player.metro = state.metro;
    sched.mode = 'song'; sched.next = t; sched.playing = true; sched.endAt = null;
    engine.mixer.setTempo(player.bpm, player.speed);
    sched.worker.postMessage('start');
    sched.pump();
    refreshTransport();
  }
  const hostTime = (beat, j) => j.f / engine.ctx.sampleRate + (beat - j.ppq) * 60 / j.cb;
  function startAtHost(j) {
    const total = songRows(), rowSec = 60 / j.cb / 4, ctx = engine.ctx;
    // count-in / pre-roll: Ableton reports a negative position; the song starts at row 0 on the downbeat
    // from where the host started or jumped to (the latest report may already be a little past it)
    const from = j.jump === DT.jumpCued ? j.ppq : Math.min(j.ppq, j.jp != null ? j.jp : j.ppq);
    let r = Math.max(0, Math.ceil(from * 4 - 1e-6)), t = hostTime(r / 4, j);
    while (t < ctx.currentTime + 0.015) { r++; t += rowSec; }
    r = wrapRow(r, j.ppq * 4);
    DT.jumpCued = j.jump;
    DT.wrapPending = null;
    const p = rowToPos(((r % total) + total) % total);
    startAt(p.order, p.row, t);
    DT.hostRow = r;
    DT.log.push('cue r' + r + ' t' + t.toFixed(3) + ' now' + ctx.currentTime.toFixed(3) + ' j' + j.jump);
    if (DT.log.length > 200) DT.log.shift();
    DT.jumpSeen = j.jump; DT.slaving = true;
  }
  // keep the scheduled rows on the host's grid. jumped: the host just looped or relocated - re-cue unless DATTRACK
  // is already there (it follows the host's loop by itself)
  function correct(j, jumped) {
    const q = sched.rowQueue; if (!q.length) return;
    const e = q[q.length - 1], total = songRows();
    // DATTRACK's position at the moment the report describes (later than its last scheduled row), following the
    // host loop the way DATTRACK does, against the host's reported row there
    const ahead = (j.f / engine.ctx.sampleRate - e.t) * j.cb / 15;
    let R = posToRow(e.order, e.row) + ahead;
    if (e.hr != null) R -= (e.hr + ahead) - wrapRow(e.hr + ahead, e.hr);
    let d = j.ppq * 4 - R;                                                       // host row - DATTRACK row
    d = ((d % total) + total) % total; if (d > total / 2) d -= total;
    const err = d * 60 / j.cb / 4;                                               // seconds DATTRACK is behind
    if (Math.abs(d) > (jumped ? 0.25 : 1)) { DT.log.push('recue d' + d.toFixed(2)); return startAtHost(j); }                 // relocated / pattern jump: re-cue
    // a nudge moves only the rows scheduled after it; until the next row is queued, the error measured on the last one
    // still includes the nudges already made - count them, or every report would correct the same error again
    if (DT.corrRow !== e) { DT.corrRow = e; DT.corrSum = 0; }
    const rest = err - DT.corrSum;
    if (Math.abs(rest) > 0.0015) { const s = Math.max(-0.01, Math.min(0.01, rest)); sched.next -= s; DT.corrSum += s; }
  }

  // ---------------------------------------------------------------- messages from the plugin (every audio post)
  function onHost(j) {
    DT.host = j;
    if (j.song !== DT.songSeq) { const first = DT.songSeq < 0; DT.songSeq = j.song; if (!first) loadHostSong(); }
    if (j.flush && j.flush !== DT.flushSeen) { DT.flushSeen = j.flush; flushForReload(j.flush); }
    if (!ready()) return;
    if (j.midi && j.midi.length) for (const [st, note, vel] of j.midi) {
      const n = note - 12;   // MIDI 60 (C4) = DATTRACK C-4 (48)
      if (st === 0xB0) { for (let k = 0; k < 128; k++) previewUp('midi' + k); continue; }   // all notes off
      previewUp('midi' + note);   // a second note-on of a held note must not leave the first one sounding
      if (st === 0x90 && vel > 0) previewDown('midi' + note, n);
    }
    if (j.stop && sched.playing) stop();
    if (j.mode === 1) {
      lockPlayerTempo(); followHostLoop();
      j.cb = j.bpm / (j.r > 0.5 && j.r < 1.5 ? j.r : 1);
      DT.loop = loopOf(j);
      if (j.jump !== DT.hostJumpN) { DT.hostJumpN = j.jump; DT.hostJumpT = j.f / engine.ctx.sampleRate; }   // when the host jumped (as heard)
      if (j.playing && sched.playing && DT.slaving) checkLoopWrap(j);
      if (j.bpm > 0 && j.playing) setHostTempo(j.bpm, j.cb);
      if (j.playing && j.run === false) { }   // the plugin's buffer is (re)filling (page start or reload): no position yet
      else if (j.playing) {
        if (!sched.playing || !DT.slaving) startAtHost(j);
        else if (j.jump !== DT.jumpSeen) { DT.jumpSeen = j.jump; correct(j, true); }
        else correct(j);
      } else {
        if (DT.slaving && sched.playing) stop();
        DT.slaving = false;
        if (j.bpm > 0) setHostTempo(j.bpm, j.cb);
      }
    } else {
      if (DT.lockBpm) { DT.lockBpm = 0; }
      DT.slaving = false;
    }
    report();
    pushState();
  }
  // tracker state for MASTER (beat anchor = the last scheduled row on the context timeline) and the plugin bar
  function report() {
    let s = '&p=' + (sched.playing ? 1 : 0) + '&b=' + effBpm().toFixed(4) + '&e=' + (DT.hostSongApplied && (dirty !== DT.lastDirty || DT.posting > 0) ? 1 : 0);
    const q = sched.rowQueue;
    if (sched.playing && q.length) { const e = q[q.length - 1]; s += '&ab=' + (posToRow(e.order, e.row) / 4).toFixed(4) + '&at=' + e.t.toFixed(6); }
    fwd.postMessage({ type: 'info', q: s });
  }
  // Mirror the song into the plugin (it is saved with the Ableton set). Never before the song saved in the set has
  // been applied - the default song must not overwrite it - and only once the edits have paused for a moment
  // (serialising a song with samples is not free). force: send now (a reload or a save of the set waits for it);
  // unloading: the page is going away.
  function pushState(force, unloading) {
    if (!DT.hostSongApplied || !ready()) return;
    const now = Date.now();
    if (dirty !== DT.dirtySeen) { DT.dirtySeen = dirty; DT.dirtyAt = now; }
    if (dirty === DT.lastDirty || (!force && now - DT.dirtyAt < 1000)) return DT.inflight;   // (a flush waits for a post on its way)
    let body;
    try { body = JSON.stringify(songToObj(false)); } catch (e) { return DT.inflight; }
    const d = DT.lastDirty = dirty;
    // unloading: a beacon survives the page (but only up to ~64 KB); otherwise a normal POST (which the flush can await)
    if (unloading && body.length < 60000 && navigator.sendBeacon && navigator.sendBeacon(url('state'), new Blob([body], { type: 'application/json' }))) return;
    // one post at a time, in order: an older song must never land after a newer one
    DT.posting = (DT.posting || 0) + 1;
    const send = () => fetch(url('state'), { method: 'POST', body, keepalive: body.length < 60000 })
      .then(r => { if (!r.ok) throw new Error(r.status); }).catch(() => { if (DT.lastDirty === d) DT.lastDirty = -1; })
      .finally(() => { DT.posting--; });
    return DT.inflight = (DT.inflight || Promise.resolve()).then(send);
  }
  // the plugin is about to reload the page (RELOAD, new sample rate) or saves the set: save now, then acknowledge
  async function flushForReload(n) {
    try { await pushState(true); } catch (e) { }
    fetch(url('flushed', '&n=' + n)).catch(() => { });
  }
  setInterval(() => pushState(false), 1000);   // independent of the audio stream (it may be paused)
  window.addEventListener('pagehide', () => pushState(true, true));
  async function loadHostSong() {
    try {
      const r = await fetch(url('state'));
      if (r.status !== 200) return false;
      setSong(songFromObj(await r.json()));
      DT.lastDirty = DT.dirtySeen = dirty;
      return true;
    } catch (e) { say('Could not load the song from the plugin: ' + e.message); return false; }
  }

  // ---------------------------------------------------------------- start automatically (no tap needed inside the plugin)
  async function autostart() {
    const b = document.getElementById('startbtn');
    if (!b) return;
    b.click();
    for (let i = 0; i < 200 && !ready(); i++) await new Promise(r => setTimeout(r, 50));
    if (!ready()) return;
    await loadHostSong();
    DT.lastDirty = DT.dirtySeen = dirty;   // the song as loaded (or the fresh default) is not an edit
    DT.hostSongApplied = true;
    DT.started = true;
    // the browser may still hold the context until the first click (macOS autoplay policy)
    const resume = () => { if (engine.ctx && engine.ctx.state !== 'running') engine.ctx.resume(); };
    document.addEventListener('pointerdown', resume, true);
    document.addEventListener('keydown', resume, true);
  }
  window.addEventListener('load', () => setTimeout(autostart, 50));
})();
