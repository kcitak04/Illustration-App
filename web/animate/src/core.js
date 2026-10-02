// ============================================================================
// core.js — document model, matrices, colours, history, autosave
// ============================================================================
"use strict";
const App = (window.App = {});
const $ = (id) => document.getElementById(id);
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const lerp = (a, b, t) => a + (b - a) * t;
let _uid = Date.now() % 1e9;
const uid = () => "o" + (++_uid).toString(36);

// ---------------------------------------------------------------- matrices [a b c d e f]
const M = {
  id: () => [1, 0, 0, 1, 0, 0],
  mul: (m, n) => [
    m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1],
    m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
    m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]
  ],
  inv: (m) => {
    const d = m[0] * m[3] - m[1] * m[2] || 1e-9;
    return [m[3] / d, -m[1] / d, -m[2] / d, m[0] / d, (m[2] * m[5] - m[3] * m[4]) / d, (m[1] * m[4] - m[0] * m[5]) / d];
  },
  apply: (m, x, y) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]],
  translate: (x, y) => [1, 0, 0, 1, x, y],
  scale: (sx, sy, cx = 0, cy = 0) => [sx, 0, 0, sy, cx - sx * cx, cy - sy * cy],
  rotate: (a, cx = 0, cy = 0) => {
    const c = Math.cos(a), s = Math.sin(a);
    return [c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy];
  },
  // translate / rotate / scale / skew decomposition, used for tweening
  decompose: (m) => {
    const sx = Math.hypot(m[0], m[1]);
    const rot = Math.atan2(m[1], m[0]);
    const det = m[0] * m[3] - m[1] * m[2];
    const sy = det / (sx || 1e-9);
    const skew = Math.atan2(m[0] * m[2] + m[1] * m[3], sx * sx);
    return { tx: m[4], ty: m[5], rot, sx, sy, skew };
  },
  compose: (d) => {
    const c = Math.cos(d.rot), s = Math.sin(d.rot);
    const k = Math.tan(d.skew || 0);
    // R * [sx, sx*k; 0, sy]
    return [c * d.sx, s * d.sx, c * d.sx * k - s * d.sy, s * d.sx * k + c * d.sy, d.tx, d.ty];
  },
  scaleFactor: (m) => Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) || 1
};

// ---------------------------------------------------------------- colours
const Color = {
  parse(c) {
    if (!c) return null;
    if (c[0] === "#") {
      let h = c.slice(1);
      if (h.length === 3) h = h.split("").map((x) => x + x).join("");
      const n = parseInt(h.slice(0, 6), 16);
      const a = h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1;
      return [(n >> 16) & 255, (n >> 8) & 255, n & 255, a];
    }
    const m = c.match(/rgba?\(([^)]+)\)/);
    if (m) { const p = m[1].split(",").map(Number); return [p[0], p[1], p[2], p[3] ?? 1]; }
    return [0, 0, 0, 1];
  },
  str(r, g, b, a = 1) { return `rgba(${Math.round(r)},${Math.round(g)},${Math.round(b)},${+a.toFixed(3)})`; },
  hex(c) { const p = Color.parse(c) || [0, 0, 0, 1]; return "#" + p.slice(0, 3).map((v) => Math.round(v).toString(16).padStart(2, "0")).join(""); },
  alpha(c) { const p = Color.parse(c); return p ? p[3] : 1; },
  withAlpha(c, a) { const p = Color.parse(c) || [0, 0, 0, 1]; return Color.str(p[0], p[1], p[2], a); },
  mix(a, b, t) {
    const p = Color.parse(a), q = Color.parse(b);
    if (!p || !q) return t < 0.5 ? a : b;
    return Color.str(lerp(p[0], q[0], t), lerp(p[1], q[1], t), lerp(p[2], q[2], t), lerp(p[3], q[3], t));
  }
};

// ---------------------------------------------------------------- document
function newDoc(w = 1080, h = 1080, fps = 12, frames = 24) {
  return {
    version: 1, name: "Untitled animation", width: w, height: h, fps, speed: 1, frames,
    background: "#ffffff", transparent: false, loop: true, pingpong: false,
    onion: { on: true, before: 1, after: 1, opacity: 0.35 },
    layers: [newDrawLayer("Layer 1", frames)]
  };
}
function newDrawLayer(name, frames) {
  const cells = new Array(frames).fill(null);
  cells[0] = newCell();
  return { id: uid(), name, kind: "draw", visible: true, locked: false, opacity: 1, blend: "source-over", cells };
}
const newCell = () => ({ key: true, objects: [], raster: null, tween: null });

App.doc = newDoc();
App.state = {
  frame: 0, layer: 0, tool: "brush", playing: false,
  selection: [],          // selected object ids (current cell)
  zoom: 1, panX: 0, panY: 0,
  autoKey: true,
  dirty: 0                // bumped on every change (render cache key)
};
App.style = {
  fill: "#1d4ed8", stroke: "#111827", strokeOn: false, sw: 6, opacity: 1, blend: "source-over",
  grad: null,             // {type:'linear'|'radial', c1, c2, angle}
  brush: { size: 18, pressure: 0.8, smooth: 0.5, taper: true, style: "pressure" },
  paint: { brush: "round", size: 24, hardness: 0.7, opacity: 1, flow: 1, mix: 0.35, pressureSize: true, pressureOpacity: false, color: "#111827" },
  eraser: { size: 40, mode: "all" },   // all | pixels | cut (parts of vectors) | objects (whole vectors)
  sym: { mode: "off", n: 6, cx: null, cy: null },   // off | x | y | xy | radial | kaleido; centre null = canvas centre
  knife: { mode: "free", snap: 0, gap: 0, spread: 0, target: "auto" },
  repeat: { count: 3, dx: 40, dy: 0, rotate: 0, scale: 100, fade: 100, pivot: "selection" },
  shape: { radius: 0, points: 5, star: true, inner: 0.45, constrain: false },
  text: { font: "Anton", size: 120, weight: 700, italic: false, align: "left" },
  bucket: { tolerance: 24 }
};

/** Index of the key cell that frame f shows on layer L (or -1). */
function keyIndexAt(L, f) { for (let i = Math.min(f, L.cells.length - 1); i >= 0; i--) if (L.cells[i]) return i; return -1; }
function nextKeyIndex(L, k) { for (let i = k + 1; i < L.cells.length; i++) if (L.cells[i]) return i; return -1; }
const activeLayer = () => App.doc.layers[App.state.layer];

function changed(what) {
  App.state.dirty++;
  App.emit && App.emit("changed", what);
  Autosave.schedule();
}

// ---------------------------------------------------------------- cloning & history
function cloneObj(o) { return JSON.parse(JSON.stringify(o)); }
function cloneCanvas(c) {
  if (!c) return null;
  const n = document.createElement("canvas"); n.width = c.width; n.height = c.height;
  n.getContext("2d").drawImage(c, 0, 0); return n;
}
function snapshot() {
  const d = App.doc;
  return {
    meta: { width: d.width, height: d.height, fps: d.fps, speed: d.speed, frames: d.frames, background: d.background, transparent: d.transparent, loop: d.loop, pingpong: d.pingpong },
    layers: d.layers.map((L) => ({
      ...L,
      settings: L.settings ? { ...L.settings } : undefined,
      cells: L.cells ? L.cells.map((c) => (c ? { key: true, objects: c.objects.map(cloneObj), raster: c.raster, tween: c.tween } : null)) : undefined
    })),
    frame: App.state.frame, layer: App.state.layer
  };
}
function restore(s) {
  Object.assign(App.doc, s.meta);
  App.doc.layers = s.layers.map((L) => ({
    ...L,
    settings: L.settings ? { ...L.settings } : undefined,
    cells: L.cells ? L.cells.map((c) => (c ? { key: true, objects: c.objects.map(cloneObj), raster: c.raster, tween: c.tween } : null)) : undefined
  }));
  App.state.frame = clamp(s.frame, 0, App.doc.frames - 1);
  App.state.layer = clamp(s.layer, 0, App.doc.layers.length - 1);
  App.state.selection = [];
}
const History = {
  undo: [], redo: [],
  push() { this.undo.push(snapshot()); if (this.undo.length > 60) this.undo.shift(); this.redo = []; },
  doUndo() { if (!this.undo.length) return; this.redo.push(snapshot()); restore(this.undo.pop()); changed("history"); },
  doRedo() { if (!this.redo.length) return; this.undo.push(snapshot()); restore(this.redo.pop()); changed("history"); },
  clear() { this.undo = []; this.redo = []; }
};

/**
 * Returns the editable key cell for the active layer at the current frame.
 * With auto-key on, drawing on a held or tweened frame creates a new key that
 * starts from what is currently shown there.
 */
function editableCell(forRaster) {
  const L = activeLayer();
  if (!L || L.kind !== "draw") return null;
  const f = App.state.frame;
  if (L.cells[f]) return L.cells[f];
  const k = keyIndexAt(L, f);
  if (!App.state.autoKey && k >= 0) return L.cells[k];
  const cell = newCell();
  if (k >= 0) {
    const shown = Render.cellObjectsAt(L, f);
    cell.objects = shown.map(cloneObj);
    cell.raster = cloneCanvas(L.cells[k].raster);
    if (L.cells[k].tween) cell.tween = L.cells[k].tween;
  }
  L.cells[f] = cell;
  return cell;
}

/** Copy-on-write raster for painting (keeps history snapshots intact). */
function rasterForPaint(cell) {
  const d = App.doc;
  if (!cell.raster) { const c = document.createElement("canvas"); c.width = d.width; c.height = d.height; cell.raster = c; }
  else cell.raster = cloneCanvas(cell.raster);
  return cell.raster;
}

// ---------------------------------------------------------------- IndexedDB autosave (per browser convenience)
const Autosave = {
  timer: 0,
  schedule() { clearTimeout(this.timer); this.timer = setTimeout(() => this.save(), 4000); },
  db() {
    return new Promise((res, rej) => {
      try {
        const r = indexedDB.open("frame-studio", 1);
        r.onupgradeneeded = () => r.result.createObjectStore("kv");
        r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
      } catch (e) { rej(e); }
    });
  },
  async save() {
    try {
      const data = await Project.serialize({ embedMedia: true, asBlobs: true });
      const db = await this.db();
      await new Promise((res) => { const t = db.transaction("kv", "readwrite"); t.objectStore("kv").put(data, "autosave"); t.oncomplete = res; t.onerror = res; t.onabort = res; });
      App.emit && App.emit("saved");
    } catch (e) { /* storage unavailable; ignore */ }
  },
  async load() {
    try {
      const db = await this.db();
      return await new Promise((res) => { const t = db.transaction("kv").objectStore("kv").get("autosave"); t.onsuccess = () => res(t.result || null); t.onerror = () => res(null); });
    } catch (e) { return null; }
  },
  async clear() {
    try { const db = await this.db(); db.transaction("kv", "readwrite").objectStore("kv").delete("autosave"); } catch (e) { /* ignore */ }
  }
};

// ---------------------------------------------------------------- tiny event bus
const listeners = {};
App.on = (ev, fn) => (listeners[ev] = listeners[ev] || []).push(fn);
App.emit = (ev, data) => (listeners[ev] || []).forEach((fn) => fn(data));

let toastTimer = 0;
function toast(msg, ms = 3200) {
  const t = $("toast"); t.textContent = msg; t.hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => (t.hidden = true), ms);
}
