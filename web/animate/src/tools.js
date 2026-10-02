// ============================================================================
// tools.js — the canvas view (zoom / pan / stylus input) and every drawing tool
// ============================================================================
const View = (App.View = {});
const stage = () => $("stage");
let vctx = null;
let viewW = 0, viewH = 0, dpr = 1;
let penSeen = false;

View.init = function () {
  vctx = stage().getContext("2d");
  const ro = new ResizeObserver(() => View.resize());
  ro.observe($("canvasWrap"));
  View.resize(); View.fit();
  bindInput();
  requestAnimationFrame(loop);
};
View.resize = function () {
  const wrap = $("canvasWrap"); dpr = window.devicePixelRatio || 1;
  viewW = wrap.clientWidth; viewH = wrap.clientHeight;
  stage().width = Math.round(viewW * dpr); stage().height = Math.round(viewH * dpr);
  View.redraw();
};
View.fit = function () {
  const d = App.doc, pad = 28;
  const z = Math.min((viewW - pad * 2) / d.width, (viewH - pad * 2) / d.height);
  App.state.zoom = Math.max(0.02, z);
  App.state.panX = (viewW - d.width * App.state.zoom) / 2; App.state.panY = (viewH - d.height * App.state.zoom) / 2;
  View.redraw(); App.emit("zoom");
};
View.zoomAt = function (factor, sx = viewW / 2, sy = viewH / 2) {
  const s = App.state, nz = clamp(s.zoom * factor, 0.02, 32);
  s.panX = sx - (sx - s.panX) * (nz / s.zoom); s.panY = sy - (sy - s.panY) * (nz / s.zoom); s.zoom = nz;
  View.redraw(); App.emit("zoom");
};
View.actual = function () { View.zoomAt(1 / App.state.zoom); };
const toDoc = (sx, sy) => [(sx - App.state.panX) / App.state.zoom, (sy - App.state.panY) / App.state.zoom];
const toScreen = (x, y) => [x * App.state.zoom + App.state.panX, y * App.state.zoom + App.state.panY];
const docLen = (px) => px / App.state.zoom;

let needs = true, lastDirty = -1;
View.redraw = () => { needs = true; };
function loop() {
  if (needs || lastDirty !== App.state.dirty) { needs = false; lastDirty = App.state.dirty; paint(); }
  requestAnimationFrame(loop);
}

// cached layers under / over the active one while a stroke is in progress
let strokeCache = null;
function buildStrokeCache() {
  const d = App.doc, f = App.state.frame, ai = App.state.layer;
  // cached at the screen's own resolution (the view can't pan or zoom during a stroke), so nothing goes soft
  const sc = stage(), z = App.state.zoom;
  const mk = () => { const c = document.createElement("canvas"); c.width = sc.width; c.height = sc.height; return c; };
  const under = mk(), over = mk();
  const ug = under.getContext("2d"), og = over.getContext("2d");
  for (const g of [ug, og]) { g.setTransform(dpr * z, 0, 0, dpr * z, dpr * App.state.panX, dpr * App.state.panY); g.beginPath(); g.rect(0, 0, d.width, d.height); g.clip(); g.imageSmoothingQuality = "high"; }
  if (!d.transparent) { ug.fillStyle = d.background; ug.fillRect(0, 0, d.width, d.height); }
  const L = d.layers[ai];
  // with masks / clipping involved, redraw everything live instead (the layers depend on each other)
  if (L && (L.clip || L.mask || L.maskEdit || d.layers[ai + 1]?.clip)) { strokeCache = { full: true }; return; }
  if (ai > 0) Render.frame(ug, f, { background: false, to: ai - 1 });
  Render.frame(og, f, { background: false, from: ai + 1 });
  strokeCache = { under, over };
}

const checker = (() => {
  const c = document.createElement("canvas"); c.width = c.height = 16;
  const g = c.getContext("2d"); g.fillStyle = "#fff"; g.fillRect(0, 0, 16, 16); g.fillStyle = "#d8d8d8"; g.fillRect(0, 0, 8, 8); g.fillRect(8, 8, 8, 8);
  return c;
})();

function paint() {
  const s = App.state, d = App.doc, g = vctx;
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = getComputedStyle(document.documentElement).getPropertyValue("--pasteboard").trim() || "#2b2d33";
  g.fillRect(0, 0, viewW, viewH);
  g.save();
  g.translate(s.panX, s.panY); g.scale(s.zoom, s.zoom);
  g.shadowColor = "rgba(0,0,0,.35)"; g.shadowBlur = 18; g.fillStyle = "#fff"; g.fillRect(0, 0, d.width, d.height); g.shadowBlur = 0;
  if (d.transparent) { g.save(); g.scale(1 / s.zoom, 1 / s.zoom); g.fillStyle = g.createPattern(checker, "repeat"); g.fillRect(0, 0, d.width * s.zoom, d.height * s.zoom); g.restore(); }
  g.beginPath(); g.rect(0, 0, d.width, d.height); g.clip();
  g.imageSmoothingQuality = "high";
  const L = activeLayer();
  if (strokeCache && strokeCache.full && L) {
    Render.frame(g, s.frame, { liveIndex: s.layer, live: (x) => drawActiveContent(x, L) });
  } else if (strokeCache && L) {
    const pix = (c) => { g.save(); g.setTransform(1, 0, 0, 1, 0, 0); g.drawImage(c, 0, 0); g.restore(); };
    pix(strokeCache.under);
    drawActiveLayerLive(g, L);
    pix(strokeCache.over);
  } else {
    Render.frame(g, s.frame);
  }
  if (!s.playing) Render.onion(g, s.frame);
  if (!s.playing) Mask.overlay(g);
  if (!s.playing && L && L.maskEdit && L.mask) drawMaskTint(g, L);
  g.restore();
  // overlays in screen space
  g.save();
  Tools.current()?.overlay?.(g);
  if (!s.playing) drawSymmetryGuides(g);
  drawSelectionOverlay(g);
  drawHoverCursor(g);
  g.restore();
}
const liveBufs = [document.createElement("canvas"), document.createElement("canvas")];
/** Paint strokes are drawn at this many pixels per document pixel, so they stay crisp when zoomed in. */
function paintScale() { const d = App.doc; return Math.max(1, Math.min(2, 4096 / Math.max(d.width, d.height))); }
function liveBuf(i) {
  const c = liveBufs[i], d = App.doc, s = paintScale(), W = Math.round(d.width * s), H = Math.round(d.height * s);
  if (c.width !== W || c.height !== H) { c.width = W; c.height = H; }
  const x = c.getContext("2d"); x.setTransform(1, 0, 0, 1, 0, 0); x.globalAlpha = 1; x.globalCompositeOperation = "source-over"; x.clearRect(0, 0, c.width, c.height);
  x.setTransform(s, 0, 0, s, 0, 0); x.imageSmoothingQuality = "high";
  return x;
}
/** Draws the stroke buffer in document coordinates. */
function drawBuffer(x) { const d = App.doc; x.drawImage(Paint.buffer, 0, 0, d.width, d.height); }
function applyLive(x) { x.save(); x.globalAlpha = Paint.live.alpha; x.globalCompositeOperation = Paint.live.erase ? "destination-out" : Paint.live.blend || "source-over"; drawBuffer(x); x.restore(); }
/** While the layer mask is being edited, its hidden parts are tinted red over the canvas. */
const tintBuf = document.createElement("canvas");
function drawMaskTint(g, L) {
  const d = App.doc; if (tintBuf.width !== d.width || tintBuf.height !== d.height) { tintBuf.width = d.width; tintBuf.height = d.height; }
  const t = tintBuf.getContext("2d"); t.globalCompositeOperation = "source-over"; t.clearRect(0, 0, d.width, d.height);
  t.fillStyle = "rgb(225, 29, 72)"; t.fillRect(0, 0, d.width, d.height);
  t.globalCompositeOperation = "destination-out"; t.drawImage(L.mask.canvas, 0, 0);
  g.save(); g.globalAlpha = 0.4; g.drawImage(tintBuf, 0, 0); g.restore();
}
function drawActiveLayerLive(g, L) {
  g.save(); g.globalAlpha = L.opacity; if (L.blend && L.blend !== "source-over") g.globalCompositeOperation = L.blend;
  drawActiveContent(g, L);
  g.restore();
}
/** The active layer's content with the stroke in progress (no layer opacity / blend). */
function drawActiveContent(g, L) {
  if (L.kind === "draw") {
    const f = App.state.frame, lv = Paint.live && !Paint.live.maskEdit ? Paint.live : null;
    if (!lv) {
      const r = Render.cellRasterAt(L, f); if (r) g.drawImage(r, 0, 0);
      for (const o of Render.cellObjectsAt(L, f)) Render.drawObj(g, o);
    } else {
      // paint into an off-screen copy of the layer so erasing never punches through the layers below
      const d = App.doc, objs = Render.cellObjectsAt(L, f);
      const a = liveBuf(0);
      const r = Render.cellRasterAt(L, f); if (r) a.drawImage(r, 0, 0, d.width, d.height);
      if (lv.pixels && !lv.asObject) applyLive(a);
      g.drawImage(liveBufs[0], 0, 0, d.width, d.height);
      if (lv.vectors && objs.length) {
        const b = liveBuf(1);
        for (const o of objs) Render.drawObj(b, o);
        applyLive(b); g.drawImage(liveBufs[1], 0, 0, d.width, d.height);
      } else for (const o of objs) Render.drawObj(g, o);   // shapes go straight to the screen: always sharp
      if (lv.asObject) applyLive(g);                        // a new paint stroke sits on top, as it will when done
    }
  } else Media.draw(g, L, App.state.frame);
  if (L.mask && L.mask.canvas && L.mask.on !== false) { g.save(); g.globalAlpha = 1; g.globalCompositeOperation = "destination-in"; g.drawImage(L.mask.canvas, 0, 0, App.doc.width, App.doc.height); g.restore(); }
}

// ---------------------------------------------------------------- input routing
let pointers = new Map(), gesture = null, spaceDown = false;
function bindInput() {
  const c = stage();
  c.addEventListener("pointerdown", onDown);
  c.addEventListener("pointermove", onMove);
  c.addEventListener("pointerup", onUp);
  c.addEventListener("pointercancel", onUp);
  c.addEventListener("pointerleave", () => { hover = null; View.redraw(); });
  c.addEventListener("wheel", (e) => {
    e.preventDefault();
    if (e.ctrlKey || e.metaKey) View.zoomAt(Math.exp(-e.deltaY * 0.01), e.offsetX, e.offsetY);
    else { App.state.panX -= e.deltaX; App.state.panY -= e.deltaY; View.redraw(); }
  }, { passive: false });
  c.addEventListener("dblclick", (e) => Tools.current()?.dbl?.(sample(e)));
  c.addEventListener("contextmenu", (e) => e.preventDefault());
  window.addEventListener("keydown", (e) => { if (e.code === "Space" && !isTyping(e)) { spaceDown = true; } });
  window.addEventListener("keyup", (e) => { if (e.code === "Space") spaceDown = false; });
}
const isTyping = (e) => e.target && /INPUT|TEXTAREA|SELECT/.test(e.target.tagName) && e.target.type !== "range";
function sample(e) {
  const r = stage().getBoundingClientRect();
  const sx = e.clientX - r.left, sy = e.clientY - r.top;
  const [x, y] = toDoc(sx, sy);
  let p = e.pointerType === "pen" ? (e.pressure || 0.5) : e.pointerType === "touch" ? 0.7 : 0.75;
  if (e.pointerType === "mouse" && e.buttons === 0) p = 0.75;
  return { x, y, sx, sy, p, shift: e.shiftKey, alt: e.altKey, meta: e.ctrlKey || e.metaKey, type: e.pointerType, button: e.button, barrel: (e.buttons & 2) !== 0 && e.pointerType === "pen", eraserEnd: (e.buttons & 32) !== 0 };
}

function onDown(e) {
  stage().setPointerCapture(e.pointerId);
  if (e.pointerType === "pen") penSeen = true;
  pointers.set(e.pointerId, { x: e.clientX, y: e.clientY, type: e.pointerType });
  const touches = [...pointers.values()].filter((p) => p.type === "touch");
  // two fingers: pinch-zoom / pan (cancels a finger stroke that just started)
  if (touches.length >= 2) {
    if (gesture && gesture.kind === "tool") { Tools.current()?.cancel?.(); strokeCache = null; }
    gesture = { kind: "pinch", start: pinchInfo(), panX: App.state.panX, panY: App.state.panY, zoom: App.state.zoom, t0: performance.now(), moved: false };
    return;
  }
  if (e.button === 1 || spaceDown || App.state.tool === "hand" || (e.pointerType === "touch" && penSeen && App.settings.palm)) {
    gesture = { kind: "pan", x: e.clientX, y: e.clientY, panX: App.state.panX, panY: App.state.panY };
    return;
  }
  if (App.state.playing) App.Player.stop();
  const smp = sample(e);
  let tool = Tools.current();
  if (smp.eraserEnd) tool = Tools.get("eraser");
  else if (smp.barrel || e.button === 2) tool = Tools.get("eyedropper");
  gesture = { kind: "tool", tool, id: e.pointerId };
  tool?.down?.(smp);
}
function onMove(e) {
  if (pointers.has(e.pointerId)) pointers.set(e.pointerId, { x: e.clientX, y: e.clientY, type: e.pointerType });
  const smp = sample(e);
  hover = e.pointerType !== "touch" ? smp : null;
  if (!gesture) { Tools.current()?.hover?.(smp); View.redraw(); return; }
  if (gesture.kind === "pinch") {
    const now = pinchInfo(), st = gesture.start;
    if (!now || !st) return;
    const z = clamp(gesture.zoom * (now.d / st.d), 0.02, 32);
    const r = stage().getBoundingClientRect();
    const cx = st.cx - r.left, cy = st.cy - r.top;
    App.state.zoom = z;
    App.state.panX = cx - (cx - gesture.panX) * (z / gesture.zoom) + (now.cx - st.cx);
    App.state.panY = cy - (cy - gesture.panY) * (z / gesture.zoom) + (now.cy - st.cy);
    if (Math.abs(now.d - st.d) > 10 || Math.hypot(now.cx - st.cx, now.cy - st.cy) > 10) gesture.moved = true;
    View.redraw(); App.emit("zoom"); return;
  }
  if (gesture.kind === "pan") {
    App.state.panX = gesture.panX + e.clientX - gesture.x; App.state.panY = gesture.panY + e.clientY - gesture.y; View.redraw(); return;
  }
  if (gesture.kind === "tool" && e.pointerId === gesture.id) {
    const evs = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
    for (const ce of evs.length ? evs : [e]) gesture.tool?.move?.(sample(ce));
    View.redraw();
  }
}
function onUp(e) {
  pointers.delete(e.pointerId);
  if (!gesture) return;
  if (gesture.kind === "pinch") {
    if (pointers.size === 0) {
      // quick two-finger tap = undo, three-finger tap = redo
      if (!gesture.moved && performance.now() - gesture.t0 < 280) History.doUndo();
      gesture = null;
    }
    return;
  }
  if (gesture.kind === "tool" && e.pointerId === gesture.id) gesture.tool?.up?.(sample(e));
  gesture = null;
  strokeCache = null;
  View.redraw();
}
function pinchInfo() {
  const t = [...pointers.values()].filter((p) => p.type === "touch");
  if (t.length < 2) return null;
  return { cx: (t[0].x + t[1].x) / 2, cy: (t[0].y + t[1].y) / 2, d: Math.hypot(t[0].x - t[1].x, t[0].y - t[1].y) || 1 };
}

let hover = null;
function drawHoverCursor(g) {
  if (!hover) return;
  const t = App.state.tool;
  const r = t === "paint" ? App.style.paint.size / 2 : t === "eraser" ? App.style.eraser.size / 2 : t === "vbrush" ? App.style.brush.size / 2 : 0;
  if (!r) return;
  Sym.all().forEach((T, i) => {
    const [sx, sy] = toScreen(...M.apply(T, hover.x, hover.y));
    g.globalAlpha = i ? 0.5 : 1;
    g.lineWidth = 1; g.strokeStyle = "rgba(0,0,0,.7)"; g.beginPath(); g.arc(sx, sy, r * App.state.zoom + 1, 0, 7); g.stroke();
    g.strokeStyle = "rgba(255,255,255,.9)"; g.beginPath(); g.arc(sx, sy, r * App.state.zoom, 0, 7); g.stroke();
  });
  g.globalAlpha = 1;
}

// ---------------------------------------------------------------- helpers shared by tools
/** Adds colours that were just used for drawing to the "recent colours" memory. */
function remember(...cs) { if (App.rememberColor) for (const c of cs) if (typeof c === "string") App.rememberColor(c); }
function currentObjects() { const L = activeLayer(); return L && L.kind === "draw" ? Render.cellObjectsAt(L, App.state.frame) : []; }
// ---- layer groups: linked layers whose shapes are selected and moved together
/** The active layer plus the layers grouped with it (drawing layers that are shown and unlocked), bottom to top. */
function linkedLayers() {
  const d = App.doc, ai = App.state.layer, L = d.layers[ai]; if (!L) return [];
  if (!L.lgroup) return [ai];
  return d.layers.map((M, i) => i).filter((i) => i === ai || (d.layers[i].lgroup === L.lgroup && d.layers[i].kind === "draw" && d.layers[i].visible && !d.layers[i].locked));
}
App.linkedLayers = linkedLayers;
function withLayer(i, fn) { const s = App.state.layer; App.state.layer = i; try { return fn(); } finally { App.state.layer = s; } }
function objectsOf(i) { const L = App.doc.layers[i]; return L && L.kind === "draw" ? Render.cellObjectsAt(L, App.state.frame) : []; }
/** Everything the select tool can pick: the shapes on the active layer and on layers grouped with it. */
function selectableObjects() { return linkedLayers().flatMap(objectsOf); }
function selectedObjects() { const ids = new Set(App.state.selection); return selectableObjects().filter((o) => ids.has(o.id)); }
/** Editable cells (auto-key aware) of every linked layer that holds part of the selection. */
function selCells() {
  const ids = new Set(App.state.selection), out = [];
  for (const i of linkedLayers()) {
    if (!objectsOf(i).some((o) => ids.has(o.id))) continue;
    const cell = withLayer(i, () => editableCell());
    if (cell) out.push({ cell, i });
  }
  return out;
}
/** One list of the live selected-layer objects, for changing their properties in place. */
App.selCell = () => ({ objects: selCells().flatMap((x) => x.cell.objects) });
/** An object plus everything grouped with it. */
function groupIds(o, objs = selectableObjects()) { return o.group ? objs.filter((x) => x.group === o.group).map((x) => x.id) : [o.id]; }
/** Gives copied objects fresh group ids so they don't join the originals' groups. */
function regroup(objs) { const map = new Map(); for (const o of objs) if (o.group) { if (!map.has(o.group)) map.set(o.group, "g" + uid()); o.group = map.get(o.group); } return objs; }
function layerEditable(msg = true) {
  const L = activeLayer();
  if (!L) return false;
  if (L.kind !== "draw") { if (msg) toast("Select a drawing layer to draw on (this one holds imported media)."); return false; }
  if (L.locked) { if (msg) toast("This layer is locked."); return false; }
  if (!L.visible) { if (msg) toast("This layer is hidden."); return false; }
  return true;
}
function styleProps() {
  const s = App.style;
  return { fill: s.grad ? { ...s.grad } : s.fill, stroke: s.strokeOn ? s.stroke : null, sw: s.sw, opacity: s.opacity, blend: s.blend };
}
function selBounds(objs) {
  if (!objs.length) return null;
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const o of objs) { const b = Render.docBounds(o); x0 = Math.min(x0, b.x); y0 = Math.min(y0, b.y); x1 = Math.max(x1, b.x + b.w); y1 = Math.max(y1, b.y + b.h); }
  return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
}
App.selBounds = () => selBounds(selectedObjects());
function hitTop(x, y, objs = selectableObjects()) {
  const tol = docLen(8);
  for (let i = objs.length - 1; i >= 0; i--) if (Render.hit(objs[i], x, y, tol)) return objs[i];
  return null;
}
/** Applies a matrix to the selected objects in the editable key (auto-key aware). */
function transformSelection(delta, base) {
  const ids = new Set(App.state.selection);
  for (const { cell } of selCells()) for (const o of cell.objects) if (ids.has(o.id)) o.m = M.mul(delta, base ? base.get(o.id) : o.m);
  changed("transform");
}
App.transformSelection = (delta) => { History.push(); transformSelection(delta); };

function handlePoints(b) {
  const rot = docLen(26);
  return [[b.x, b.y], [b.x + b.w, b.y], [b.x + b.w, b.y + b.h], [b.x, b.y + b.h], [b.x + b.w / 2, b.y], [b.x + b.w, b.y + b.h / 2], [b.x + b.w / 2, b.y + b.h], [b.x, b.y + b.h / 2], [b.x + b.w / 2, b.y - rot]];
}
function drawSelectionOverlay(g) {
  const objs = selectedObjects(); if (!objs.length || App.state.playing) return;
  const acc = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#2f80ff";
  g.strokeStyle = acc; g.lineWidth = 1;
  // object outlines
  for (const o of objs) {
    g.save(); g.translate(App.state.panX, App.state.panY); g.scale(App.state.zoom, App.state.zoom); g.transform(...o.m);
    g.lineWidth = 1 / (App.state.zoom * M.scaleFactor(o.m)); g.strokeStyle = acc;
    if (o.type === "text" || o.type === "image") { const b = Render.localBounds(o); g.strokeRect(b.x, b.y, b.w, b.h); }
    else g.stroke(Render.localPath(o));
    g.restore();
  }
  if (App.state.tool !== "select") return;
  const b = selBounds(objs);
  const [x0, y0] = toScreen(b.x, b.y), [x1, y1] = toScreen(b.x + b.w, b.y + b.h);
  g.setLineDash([4, 3]); g.strokeRect(x0, y0, x1 - x0, y1 - y0); g.setLineDash([]);
  const hp = handlePoints(b).map(([x, y]) => toScreen(x, y));
  g.beginPath(); g.moveTo(hp[4][0], hp[4][1]); g.lineTo(hp[8][0], hp[8][1]); g.stroke();
  hp.forEach(([x, y], i) => {
    g.fillStyle = "#fff"; g.strokeStyle = acc;
    if (i === 8) { g.beginPath(); g.arc(x, y, 6, 0, 7); g.fill(); g.stroke(); }
    else { g.fillRect(x - 5, y - 5, 10, 10); g.strokeRect(x - 5, y - 5, 10, 10); }
  });
}

// ---------------------------------------------------------------- symmetry
const SYM_TOOLS = ["pen", "pencil", "vbrush", "paint", "eraser", "rect", "ellipse", "poly", "line"];
const Sym = (App.Sym = {
  on: () => App.style.sym.mode !== "off",
  center() { const s = App.style.sym, d = App.doc; return [s.cx ?? d.width / 2, s.cy ?? d.height / 2]; },
  /** The mirror / rotation matrices to repeat a stroke with (identity not included). */
  transforms() {
    const s = App.style.sym; if (s.mode === "off") return [];
    const [cx, cy] = Sym.center(), out = [];
    if (s.mode === "x") out.push(M.scale(-1, 1, cx, cy));
    else if (s.mode === "y") out.push(M.scale(1, -1, cx, cy));
    else if (s.mode === "xy") out.push(M.scale(-1, 1, cx, cy), M.scale(1, -1, cx, cy), M.scale(-1, -1, cx, cy));
    else {
      const n = clamp(Math.round(s.n) || 6, 2, 32);
      for (let k = 1; k < n; k++) out.push(M.rotate((Math.PI * 2 * k) / n, cx, cy));
      if (s.mode === "kaleido") for (let k = 0; k < n; k++) out.push(M.mul(M.rotate((Math.PI * 2 * k) / n, cx, cy), M.scale(-1, 1, cx, cy)));
    }
    return out;
  },
  all() { return [M.id(), ...Sym.transforms()]; }
});
/** Adds mirrored copies of a freshly drawn object; returns their ids. */
function addMirrors(cell, o) {
  const ids = [];
  for (const T of Sym.transforms()) { const c = { ...cloneObj(o), id: uid(), m: M.mul(T, o.m) }; cell.objects.push(c); ids.push(c.id); }
  return ids;
}
function syncMirrors(cell, o, ids) {
  const Ts = Sym.transforms();
  ids.forEach((id, i) => { const j = cell.objects.findIndex((x) => x.id === id); if (j >= 0 && Ts[i]) cell.objects[j] = { ...cloneObj(o), id, m: M.mul(Ts[i], o.m) }; });
}
function drawSymmetryGuides(g) {
  if (!Sym.on() || !SYM_TOOLS.includes(App.state.tool)) return;
  const s = App.style.sym, d = App.doc, [cx, cy] = Sym.center(), R = Math.hypot(d.width, d.height);
  const acc = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#2f80ff";
  const line = (a) => { const [x0, y0] = toScreen(cx - Math.cos(a) * R, cy - Math.sin(a) * R), [x1, y1] = toScreen(cx + Math.cos(a) * R, cy + Math.sin(a) * R); g.beginPath(); g.moveTo(x0, y0); g.lineTo(x1, y1); g.stroke(); };
  const ray = (a) => { const [x0, y0] = toScreen(cx, cy), [x1, y1] = toScreen(cx + Math.cos(a) * R, cy + Math.sin(a) * R); g.beginPath(); g.moveTo(x0, y0); g.lineTo(x1, y1); g.stroke(); };
  g.save();
  g.beginPath(); const [a0, b0] = toScreen(0, 0), [a1, b1] = toScreen(d.width, d.height); g.rect(a0, b0, a1 - a0, b1 - b0); g.clip();
  g.globalAlpha = 0.75; g.strokeStyle = acc; g.lineWidth = 1; g.setLineDash([6, 5]);
  if (s.mode === "x" || s.mode === "xy") line(Math.PI / 2);
  if (s.mode === "y" || s.mode === "xy") line(0);
  if (s.mode === "radial" || s.mode === "kaleido") { const n = clamp(Math.round(s.n) || 6, 2, 32); for (let k = 0; k < n; k++) ray(-Math.PI / 2 + (Math.PI * 2 * k) / n); }
  g.setLineDash([]); g.globalAlpha = 1;
  const [px, py] = toScreen(cx, cy); g.fillStyle = "#fff"; g.beginPath(); g.arc(px, py, 4, 0, 7); g.fill(); g.stroke();
  g.restore();
}

// ---------------------------------------------------------------- mask: paint / erase only outside (or inside) chosen shapes
const MASK_TOOLS = ["paint", "eraser", "vbrush"];
const Mask = (App.Mask = {
  current: null,   // { invert, area (MultiPolygon, doc coords), ids, allow (canvas: opaque where painting is allowed), guard (Path2D of the protected part) }
  /** The area covered by the selected shapes (MultiPolygon in document coordinates), or null. */
  selectionArea() {
    const sel = selectedObjects(); if (!sel.length || typeof polygonClipping === "undefined") return null;
    const rect = (o) => { const b = Render.docBounds(o); return [[[[b.x, b.y], [b.x + b.w, b.y], [b.x + b.w, b.y + b.h], [b.x, b.y + b.h], [b.x, b.y]]]]; };
    try { const parts = sel.map((o) => Geo.areaOf(o) || rect(o)).filter((a) => a.length); return parts.length > 1 ? polygonClipping.union(...parts) : parts[0] || null; } catch (e) { return null; }
  },
  /** Makes a mask from the selected shapes. invert = true paints outside them. */
  fromSelection(invert) {
    const sel = selectedObjects();
    if (!sel.length) { toast("Select one or more shapes first, then turn on the mask."); return false; }
    if (typeof polygonClipping === "undefined") { toast("The shape library didn't load; check your connection and reload."); return false; }
    const rect = (o) => { const b = Render.docBounds(o); return [[[[b.x, b.y], [b.x + b.w, b.y], [b.x + b.w, b.y + b.h], [b.x, b.y + b.h], [b.x, b.y]]]]; };
    let area;
    try { const parts = sel.map((o) => Geo.areaOf(o) || rect(o)).filter((a) => a.length); area = parts.length > 1 ? polygonClipping.union(...parts) : parts[0] || []; }
    catch (e) { toast("Couldn't make a mask from that selection."); return false; }
    if (!area.length) { toast("That selection has no area to mask with."); return false; }
    const d = App.doc, shape = new Path2D();
    for (const P of area) for (const r of P) { r.forEach(([x, y], i) => (i ? shape.lineTo(x, y) : shape.moveTo(x, y))); shape.closePath(); }
    const allow = document.createElement("canvas"); allow.width = d.width; allow.height = d.height;
    const ag = allow.getContext("2d"); ag.fillStyle = "#000";
    if (invert) { ag.fillRect(0, 0, d.width, d.height); ag.globalCompositeOperation = "destination-out"; ag.fill(shape, "evenodd"); }
    else ag.fill(shape, "evenodd");
    const guard = new Path2D();
    if (invert) guard.addPath(shape); else { guard.rect(0, 0, d.width, d.height); guard.addPath(shape); }
    this.current = { invert, area, ids: new Set(sel.map((o) => o.id)), allow, guard };
    App.emit("mask"); View.redraw();
    toast(invert ? "Mask on: painting and erasing now only reach outside the selected shapes." : "Mask on: painting and erasing now stay inside the selected shapes.");
    return true;
  },
  clear() { if (!this.current) return; this.current = null; App.emit("mask"); View.redraw(); toast("Mask off."); },
  toggle(invert = true) { if (this.current) this.clear(); else this.fromSelection(invert); },
  /** Is painting allowed at this document point? */
  allows(x, y) { const m = this.current; if (!m) return true; return Geo.inside(m.area, x, y) !== m.invert; },
  /** Keeps only the allowed part of a stroke buffer. */
  clipBuffer(g) { const m = this.current; if (!m) return; g.save(); g.setTransform(1, 0, 0, 1, 0, 0); g.globalAlpha = 1; g.globalCompositeOperation = "destination-in"; g.drawImage(m.allow, 0, 0, g.canvas.width, g.canvas.height); g.restore(); },
  /** Puts back the protected pixels (for brushes that paint straight onto the layer). */
  restore(g, before) {
    const m = this.current; if (!m) return;
    g.save(); g.setTransform(1, 0, 0, 1, 0, 0); g.globalAlpha = 1; g.globalCompositeOperation = "source-over";
    g.clip(m.guard, "evenodd"); g.clearRect(0, 0, g.canvas.width, g.canvas.height); if (before) g.drawImage(before, 0, 0);
    g.restore();
  },
  /** Limits an eraser footprint to the allowed area. */
  clipRegion(region) {
    const m = this.current; if (!m || !region.length) return region;
    return m.invert ? polygonClipping.difference(region, m.area) : polygonClipping.intersection(region, m.area);
  },
  /** Trims a finished vector shape to the allowed area; returns the new object, or null if nothing is left. */
  clipObject(o) {
    const m = this.current; if (!m) return o;
    const a = Geo.areaOf(o); if (!a || !a.length) return o;
    const res = m.invert ? polygonClipping.difference(a, m.area) : polygonClipping.intersection(a, m.area);
    if (!res.length) return null;
    return { id: o.id, group: o.group, type: "rings", m: M.id(), fill: o.fill && typeof o.fill === "object" ? { ...o.fill } : o.fill, stroke: null, sw: 0, opacity: o.opacity ?? 1, blend: o.blend || "source-over", polys: res.map((P) => P.map((r) => r.flat())) };
  },
  overlay(g) {
    const m = this.current; if (!m || !MASK_TOOLS.includes(App.state.tool)) return;
    g.save(); g.fillStyle = "rgba(225, 29, 72, 0.22)"; g.fill(m.guard, "evenodd"); g.restore();
  }
});

// ---------------------------------------------------------------- the tools
const Tools = (App.Tools = {});
const registry = {};
Tools.get = (n) => registry[n];
Tools.current = () => registry[App.state.tool];
function tool(name, def) { registry[name] = def; }

// ---- Select / transform
tool("select", (() => {
  let mode = null, start = null, base = null, bounds = null, handle = -1, pushed = false, marquee = null;
  return {
    down(p) {
      start = p; pushed = false; marquee = null; mode = null;
      const b = App.selBounds();
      if (b) {
        const hp = handlePoints(b);
        for (let i = hp.length - 1; i >= 0; i--) if (Math.hypot(hp[i][0] - p.x, hp[i][1] - p.y) < docLen(12)) { handle = i; mode = i === 8 ? "rotate" : "scale"; break; }
      }
      if (!mode) {
        let hit = hitTop(p.x, p.y);
        if (!hit) {
          // clicking another layer's artwork switches to that layer
          const d = App.doc;
          for (let i = d.layers.length - 1; i >= 0 && !hit; i--) {
            const L = d.layers[i]; if (linkedLayers().includes(i) || L.kind !== "draw" || !L.visible || L.locked) continue;
            const objs = Render.cellObjectsAt(L, App.state.frame);
            for (let j = objs.length - 1; j >= 0; j--) if (Render.hit(objs[j], p.x, p.y, docLen(8))) { App.state.layer = i; hit = objs[j]; App.emit("layers"); break; }
          }
        }
        if (hit) {
          const members = groupIds(hit);
          if (p.shift) { const s = new Set(App.state.selection); const on = s.has(hit.id); members.forEach((id) => (on ? s.delete(id) : s.add(id))); App.state.selection = [...s]; }
          else if (!App.state.selection.includes(hit.id)) App.state.selection = members;
          mode = "move";
        } else if (b && p.x >= b.x && p.y >= b.y && p.x <= b.x + b.w && p.y <= b.y + b.h) mode = "move";
        else { if (!p.shift) App.state.selection = []; mode = "marquee"; marquee = { x0: p.x, y0: p.y, x1: p.x, y1: p.y }; }
        App.emit("selection");
      }
      bounds = App.selBounds();
      base = new Map(selectedObjects().map((o) => [o.id, o.m.slice()]));
    },
    move(p) {
      if (!mode) return;
      if (mode === "marquee") { marquee.x1 = p.x; marquee.y1 = p.y; return; }
      if (!layerEditable(false)) return;
      const dx = p.x - start.x, dy = p.y - start.y;
      if (!pushed) { if (Math.hypot(dx, dy) < docLen(2)) return; History.push(); pushed = true; }
      const b = bounds; let delta;
      if (mode === "move") {
        let mx = dx, my = dy;
        if (p.shift) { if (Math.abs(dx) > Math.abs(dy)) my = 0; else mx = 0; }
        // snap to artboard centre
        const d = App.doc, tol = docLen(6), cx = b.x + b.w / 2 + mx, cy = b.y + b.h / 2 + my;
        if (Math.abs(cx - d.width / 2) < tol) mx += d.width / 2 - cx;
        if (Math.abs(cy - d.height / 2) < tol) my += d.height / 2 - cy;
        delta = M.translate(mx, my);
      } else if (mode === "rotate") {
        const cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        let a = Math.atan2(p.y - cy, p.x - cx) - Math.atan2(start.y - cy, start.x - cx);
        if (p.shift) a = Math.round(a / (Math.PI / 12)) * (Math.PI / 12);
        delta = M.rotate(a, cx, cy);
      } else {
        const i = handle;
        const ax = [0, 3, 7].includes(i) ? b.x + b.w : [1, 2, 5].includes(i) ? b.x : b.x + b.w / 2;
        const ay = [0, 1, 4].includes(i) ? b.y + b.h : [2, 3, 6].includes(i) ? b.y : b.y + b.h / 2;
        const hx = [0, 3, 7].includes(i) ? b.x : [1, 2, 5].includes(i) ? b.x + b.w : b.x + b.w / 2;
        const hy = [0, 1, 4].includes(i) ? b.y : [2, 3, 6].includes(i) ? b.y + b.h : b.y + b.h / 2;
        let sx = i === 4 || i === 6 ? 1 : (p.x - (p.alt ? b.x + b.w / 2 : ax)) / ((hx - (p.alt ? b.x + b.w / 2 : ax)) || 1e-6);
        let sy = i === 5 || i === 7 ? 1 : (p.y - (p.alt ? b.y + b.h / 2 : ay)) / ((hy - (p.alt ? b.y + b.h / 2 : ay)) || 1e-6);
        if (i < 4 && (p.shift || App.style.keepRatio)) { const k = Math.abs(sx) > Math.abs(sy) ? sx : sy; sx = k; sy = k; }
        delta = M.scale(sx || 1e-3, sy || 1e-3, p.alt ? b.x + b.w / 2 : ax, p.alt ? b.y + b.h / 2 : ay);
      }
      transformSelection(delta, base);
    },
    up() {
      if (mode === "marquee" && marquee) {
        const x0 = Math.min(marquee.x0, marquee.x1), x1 = Math.max(marquee.x0, marquee.x1), y0 = Math.min(marquee.y0, marquee.y1), y1 = Math.max(marquee.y0, marquee.y1);
        if (x1 - x0 > docLen(3) || y1 - y0 > docLen(3)) {
          const ids = selectableObjects().filter((o) => { const b = Render.docBounds(o); return b.x < x1 && b.x + b.w > x0 && b.y < y1 && b.y + b.h > y0; }).flatMap((o) => groupIds(o));
          App.state.selection = [...new Set([...App.state.selection, ...ids])];
          App.emit("selection");
        }
      }
      mode = null; marquee = null;
    },
    dbl(p) { const hit = hitTop(p.x, p.y); if (hit && hit.type === "text") TextTool.edit(hit); },
    overlay(g) {
      if (!marquee) return;
      const [a, b] = toScreen(marquee.x0, marquee.y0), [c, d] = toScreen(marquee.x1, marquee.y1);
      g.strokeStyle = "#2f80ff"; g.setLineDash([5, 4]); g.strokeRect(a, b, c - a, d - b); g.setLineDash([]);
      g.fillStyle = "rgba(47,128,255,.08)"; g.fillRect(a, b, c - a, d - b);
    }
  };
})());

// ---- Direct selection: edit anchor points and handles
function toPathObj(o) {
  if (o.type === "path") return o;
  let nodes;
  const N = (x, y) => ({ x, y, ix: x, iy: y, ox: x, oy: y });
  if (o.type === "rect") nodes = [N(0, 0), N(o.w, 0), N(o.w, o.h), N(0, o.h)];
  else if (o.type === "ellipse") {
    const k = 0.5523, rx = o.w / 2, ry = o.h / 2;
    nodes = [
      { x: rx, y: 0, ix: rx - rx * k, iy: 0, ox: rx + rx * k, oy: 0 }, { x: o.w, y: ry, ix: o.w, iy: ry - ry * k, ox: o.w, oy: ry + ry * k },
      { x: rx, y: o.h, ix: rx + rx * k, iy: o.h, ox: rx - rx * k, oy: o.h }, { x: 0, y: ry, ix: 0, iy: ry + ry * k, ox: 0, oy: ry - ry * k }];
  } else if (o.type === "poly") {
    nodes = []; const n = o.star ? o.n * 2 : o.n;
    for (let i = 0; i < n; i++) { const r = o.star && i % 2 ? o.inner : 1, a = (Math.PI * 2 * i) / n - Math.PI / 2; nodes.push(N(o.w / 2 + Math.cos(a) * r * o.w / 2, o.h / 2 + Math.sin(a) * r * o.h / 2)); }
  } else if (o.type === "rings" || o.type === "brush") {
    try { return Geo.toPath(o); } catch (e) { return null; }
  } else return null;
  const { w, h, r, n, star, inner, ...rest } = o;
  return { ...rest, type: "path", nodes, closed: true };
}
App.toPathObj = toPathObj;
/** First and last node index of the sub-path that node i belongs to. */
function subRange(N, i) { let a = i; while (a > 0 && !N[a].start) a--; let b = i + 1; while (b < N.length && !N[b].start) b++; return [a, b - 1]; }
tool("direct", (() => {
  let drag = null, pushed = false;
  const target = () => { const s = selectedObjects(); return s.length === 1 ? s[0] : null; };
  return {
    activeNode: -1,
    down(p) {
      pushed = false; drag = null;
      let o = target();
      // merged / cut shapes and brush strokes become editable paths the first time you edit them directly
      if (o && (o.type === "rings" || o.type === "brush") && layerEditable(false)) { Cmd.toPath(); o = target(); }
      if (o && (o.type === "path" || ["rect", "ellipse", "poly"].includes(o.type))) {
        const po = toPathObj(o), tol = docLen(10);
        const at = (x, y) => M.apply(o.m, x, y);
        const n = po.nodes[this.activeNode];
        if (n) {
          const [ix, iy] = at(n.ix, n.iy), [ox, oy] = at(n.ox, n.oy);
          if (Math.hypot(ox - p.x, oy - p.y) < tol && (n.ox !== n.x || n.oy !== n.y)) drag = { kind: "out" };
          else if (Math.hypot(ix - p.x, iy - p.y) < tol && (n.ix !== n.x || n.iy !== n.y)) drag = { kind: "in" };
        }
        if (!drag) for (let i = po.nodes.length - 1; i >= 0; i--) { const [x, y] = at(po.nodes[i].x, po.nodes[i].y); if (Math.hypot(x - p.x, y - p.y) < tol) { this.activeNode = i; drag = { kind: "node" }; break; } }
        if (drag) { drag.start = p; drag.id = o.id; App.emit("selection"); return; }
      }
      const hit = hitTop(p.x, p.y, currentObjects());
      App.state.selection = hit ? [hit.id] : []; this.activeNode = -1; App.emit("selection");
    },
    move(p) {
      if (!drag || !layerEditable(false)) return;
      if (!pushed) { if (Math.hypot(p.x - drag.start.x, p.y - drag.start.y) < docLen(2)) return; History.push(); pushed = true; }
      const cell = editableCell(); const idx = cell.objects.findIndex((x) => x.id === drag.id); if (idx < 0) return;
      let o = cell.objects[idx];
      if (o.type !== "path") { o = toPathObj(o); cell.objects[idx] = o; }
      const n = o.nodes[this.activeNode]; if (!n) return;
      const [lx, ly] = M.apply(M.inv(o.m), p.x, p.y);
      if (drag.kind === "node") { const dx = lx - n.x, dy = ly - n.y; n.x += dx; n.y += dy; n.ix += dx; n.iy += dy; n.ox += dx; n.oy += dy; }
      else if (drag.kind === "out") { n.ox = lx; n.oy = ly; if (n.smooth !== false) { n.ix = 2 * n.x - lx; n.iy = 2 * n.y - ly; } }
      else { n.ix = lx; n.iy = ly; if (n.smooth !== false) { n.ox = 2 * n.x - lx; n.oy = 2 * n.y - ly; } }
      changed("nodes");
    },
    up() { drag = null; },
    dbl() {
      const o = target(); if (!o || o.type !== "path") return;
      const n = o.nodes[this.activeNode]; if (!n) return;
      History.push(); const cell = editableCell(); const po = cell.objects.find((x) => x.id === o.id); const m = po.nodes[this.activeNode];
      if (m.ix !== m.x || m.ox !== m.x || m.iy !== m.y || m.oy !== m.y) { m.ix = m.ox = m.x; m.iy = m.oy = m.y; m.smooth = false; }
      else {
        const N = po.nodes, i = this.activeNode, [s0, e0] = subRange(N, i), a = N[i > s0 ? i - 1 : e0], b = N[i < e0 ? i + 1 : s0];
        const tx = (b.x - a.x) / 6, ty = (b.y - a.y) / 6; m.ix = m.x - tx; m.iy = m.y - ty; m.ox = m.x + tx; m.oy = m.y + ty; m.smooth = true;
      }
      changed("nodes");
    },
    overlay(g) {
      const o = target(); if (!o || o.type === "rings" || o.type === "brush") return;
      const po = toPathObj(o); if (!po) return;
      const acc = "#2f80ff";
      po.nodes.forEach((n, i) => {
        const [x, y] = toScreen(...M.apply(o.m, n.x, n.y));
        if (i === this.activeNode) {
          for (const [hx, hy] of [[n.ix, n.iy], [n.ox, n.oy]]) {
            if (hx === n.x && hy === n.y) continue;
            const [sx, sy] = toScreen(...M.apply(o.m, hx, hy));
            g.strokeStyle = acc; g.beginPath(); g.moveTo(x, y); g.lineTo(sx, sy); g.stroke();
            g.fillStyle = "#fff"; g.beginPath(); g.arc(sx, sy, 4, 0, 7); g.fill(); g.stroke();
          }
        }
        g.fillStyle = i === this.activeNode ? acc : "#fff"; g.strokeStyle = acc;
        g.fillRect(x - 4, y - 4, 8, 8); g.strokeRect(x - 4, y - 4, 8, 8);
      });
    }
  };
})());

// ---- Pen (Bézier)
tool("pen", (() => {
  let obj = null, node = null, dragging = false;
  const finish = () => {
    if (!obj) return;
    const cell = editableCell();
    if (obj.nodes.length < 2) cell.objects = cell.objects.filter((o) => o.id !== obj.id);
    else { const live = cell.objects.find((o) => o.id === obj.id) || obj; App.state.selection = [obj.id, ...addMirrors(cell, live)]; }
    obj = null; node = null; changed("pen"); App.emit("selection");
  };
  return {
    finish,
    down(p) {
      if (!layerEditable()) return;
      if (obj && obj.nodes.length > 1) {
        const [fx, fy] = obj.nodes[0] ? [obj.nodes[0].x, obj.nodes[0].y] : [0, 0];
        if (Math.hypot(fx - p.x, fy - p.y) < docLen(10)) { obj.closed = true; changed("pen"); finish(); return; }
      }
      if (!obj) {
        History.push();
        const cell = editableCell();
        const st = styleProps();
        obj = { id: uid(), type: "path", nodes: [], closed: false, m: M.id(), ...st };
        remember(st.stroke || App.style.stroke, App.style.penFill ? st.fill : null);
        if (!obj.stroke && !App.style.penFill) { obj.stroke = App.style.stroke; }
        if (!App.style.penFill) obj.fill = null;
        cell.objects.push(obj);
      } else {
        const cell = editableCell(); obj = cell.objects.find((o) => o.id === obj.id) || obj;
      }
      node = { x: p.x, y: p.y, ix: p.x, iy: p.y, ox: p.x, oy: p.y };
      obj.nodes.push(node); dragging = true; changed("pen");
    },
    move(p) {
      if (!dragging || !node) return;
      if (Math.hypot(p.x - node.x, p.y - node.y) > docLen(3)) { node.ox = p.x; node.oy = p.y; node.ix = 2 * node.x - p.x; node.iy = 2 * node.y - p.y; changed("pen"); }
    },
    up() { dragging = false; },
    dbl() { finish(); },
    cancel() { dragging = false; },
    overlay(g) {
      if (!obj || !hover || !obj.nodes.length) return;
      const last = obj.nodes[obj.nodes.length - 1];
      const [a, b] = toScreen(last.x, last.y), [c, d] = toScreen(last.ox, last.oy), [e, f] = toScreen(hover.x, hover.y);
      g.strokeStyle = "#2f80ff"; g.beginPath(); g.moveTo(a, b); g.bezierCurveTo(c, d, e, f, e, f); g.stroke();
      for (const n of obj.nodes) { const [x, y] = toScreen(n.x, n.y); g.fillStyle = "#fff"; g.fillRect(x - 3.5, y - 3.5, 7, 7); g.strokeRect(x - 3.5, y - 3.5, 7, 7); }
      // symmetry preview: the copies appear when the path is finished
      for (const T of Sym.transforms()) {
        g.save(); g.translate(App.state.panX, App.state.panY); g.scale(App.state.zoom, App.state.zoom); g.transform(...T);
        g.globalAlpha = 0.6; g.lineWidth = 1.5 / App.state.zoom; g.setLineDash([4 / App.state.zoom, 3 / App.state.zoom]); g.stroke(Render.localPath(obj)); g.restore();
      }
    }
  };
})());

// ---- Pencil: freehand, simplified into a smooth editable path
/** rdp for a line whose ends meet (a loop): split at the farthest point so it can't collapse to nothing. */
function rdpLoop(pts, eps) {
  const a = pts[0], b = pts[pts.length - 1];
  if (pts.length < 4 || Math.hypot(b[0] - a[0], b[1] - a[1]) > eps * 4) return rdp(pts, eps);
  let far = 1, fd = -1;
  pts.forEach((q, i) => { const d = Math.hypot(q[0] - a[0], q[1] - a[1]); if (d > fd) { fd = d; far = i; } });
  return rdp(pts.slice(0, far + 1), eps).slice(0, -1).concat(rdp(pts.slice(far), eps));
}
function rdp(pts, eps) {
  if (pts.length < 3) return pts;
  const a = pts[0], b = pts[pts.length - 1], len = Math.hypot(b[0] - a[0], b[1] - a[1]) || 1e-9;
  let md = 0, idx = 0;
  for (let i = 1; i < pts.length - 1; i++) { const p = pts[i]; const dd = Math.abs((b[0] - a[0]) * (a[1] - p[1]) - (a[0] - p[0]) * (b[1] - a[1])) / len; if (dd > md) { md = dd; idx = i; } }
  if (md > eps) return rdp(pts.slice(0, idx + 1), eps).slice(0, -1).concat(rdp(pts.slice(idx), eps));
  return [a, b];
}
tool("pencil", (() => {
  let pts = null;
  return {
    down(p) { if (!layerEditable()) return; pts = [[p.x, p.y]]; },
    move(p) { if (pts) { const l = pts[pts.length - 1]; if (Math.hypot(p.x - l[0], p.y - l[1]) > docLen(1.5)) pts.push([p.x, p.y]); } },
    up() {
      if (!pts || pts.length < 3) { pts = null; return; }
      let s = rdpLoop(pts, docLen(2.2));
      const closed = s.length > 3 && Math.hypot(s[0][0] - s[s.length - 1][0], s[0][1] - s[s.length - 1][1]) < docLen(12);
      if (closed) s = s.slice(0, -1);
      const n = s.length;
      const nodes = s.map((q, i) => {
        const prev = s[i - 1] || (closed ? s[n - 1] : null), next = s[i + 1] || (closed ? s[0] : null);
        if (!prev || !next) return { x: q[0], y: q[1], ix: q[0], iy: q[1], ox: q[0], oy: q[1] };
        const tx = (next[0] - prev[0]) / 6, ty = (next[1] - prev[1]) / 6;
        return { x: q[0], y: q[1], ix: q[0] - tx, iy: q[1] - ty, ox: q[0] + tx, oy: q[1] + ty, smooth: true };
      });
      History.push();
      const st = styleProps();
      const o = { id: uid(), type: "path", nodes, closed, m: M.id(), ...st, stroke: App.style.strokeOn ? App.style.stroke : (typeof App.style.fill === "string" ? App.style.fill : "#111"), fill: closed && App.style.pencilFill ? st.fill : null };
      remember(o.stroke, o.fill);
      const cell = editableCell(); cell.objects.push(o); addMirrors(cell, o);
      pts = null; changed("pencil");
    },
    cancel() { pts = null; },
    overlay(g) {
      if (!pts) return;
      g.strokeStyle = App.style.strokeOn ? App.style.stroke : (typeof App.style.fill === "string" ? App.style.fill : "#111");
      g.lineWidth = Math.max(1, App.style.sw * App.state.zoom); g.lineCap = "round"; g.lineJoin = "round";
      for (const T of Sym.all()) { g.beginPath(); pts.forEach(([x, y], i) => { const [a, b] = toScreen(...M.apply(T, x, y)); i ? g.lineTo(a, b) : g.moveTo(a, b); }); g.stroke(); }
    }
  };
})());

// ---- Vector brush: pressure-sensitive filled strokes (stay editable & tweenable)
tool("vbrush", (() => {
  let obj = null, sm = null, ids = [];
  return {
    down(p) {
      if (!layerEditable()) return;
      History.push();
      const b = App.style.brush;
      obj = { id: uid(), type: "brush", pts: [p.x, p.y, p.p], size: b.size, pressure: b.pressure, taper: b.taper, style: b.style || "pressure", dot: b.dot ?? 1, sp: b.sp ?? 1, amt: b.amt ?? 0.5, ang: b.ang ?? 45, freq: b.freq ?? 1, seed: 1 + Math.floor(Math.random() * 9999), m: M.id(), fill: App.style.grad ? { ...App.style.grad } : (App.style.fill || App.style.stroke), stroke: null, opacity: App.style.opacity, blend: App.style.blend };
      remember(typeof obj.fill === "string" ? obj.fill : null);
      sm = { x: p.x, y: p.y, p: p.p };
      const cell = editableCell(); cell.objects.push(obj);
      const copies = Sym.transforms().map((T) => ({ ...obj, id: uid(), m: T, pts: obj.pts }));
      cell.objects.push(...copies); ids = [obj.id, ...copies.map((c) => c.id)];
      changed("brush");
    },
    move(p) {
      if (!obj) return;
      const k = App.style.brush.smooth * 0.9;
      sm.x = sm.x * k + p.x * (1 - k); sm.y = sm.y * k + p.y * (1 - k); sm.p = sm.p * 0.5 + p.p * 0.5;
      const n = obj.pts.length;
      if (Math.hypot(sm.x - obj.pts[n - 3], sm.y - obj.pts[n - 2]) < Math.max(docLen(1), obj.size * 0.05)) return;
      obj.pts.push(sm.x, sm.y, sm.p);
      const cell = editableCell(); for (const o of cell.objects) if (ids.includes(o.id)) o.pts = obj.pts;
      changed("brush");
    },
    up() {
      if (obj && Mask.current) {
        const cell = editableCell(); const out = [];
        for (const o of cell.objects) {
          if (!ids.includes(o.id)) { out.push(o); continue; }
          let r = o; try { r = Mask.clipObject(o); } catch (e) { r = o; }
          if (r) out.push(r);
        }
        cell.objects = out; changed("brush");
      }
      obj = null;
    },
    cancel() { if (obj) { History.doUndo(); obj = null; } }
  };
})());

// ---- Shapes
function shapeTool(kind) {
  let obj = null, start = null, mids = [];
  return {
    down(p) {
      if (!layerEditable()) return;
      start = p; obj = null;
    },
    move(p) {
      if (!start) return;
      let dx = p.x - start.x, dy = p.y - start.y;
      if (!obj && Math.hypot(dx, dy) < docLen(4)) return;
      if (!obj) {
        History.push();
        const st = styleProps(), sh = App.style.shape;
        remember(st.fill, st.stroke);
        obj = { id: uid(), type: kind === "line" ? "path" : kind, m: M.id(), ...st };
        if (kind === "rect") obj.r = sh.radius;
        if (kind === "poly") { obj.n = sh.points; obj.star = sh.star; obj.inner = sh.inner; }
        if (kind === "line") { obj.nodes = []; obj.closed = false; obj.fill = null; obj.stroke = App.style.strokeOn ? App.style.stroke : (typeof App.style.fill === "string" ? App.style.fill : "#111"); obj.sw = Math.max(1, App.style.sw); }
        const cell = editableCell(); cell.objects.push(obj); mids = addMirrors(cell, obj);
      }
      const live = editableCell().objects.find((o) => o.id === obj.id) || obj;
      if (kind === "line") {
        let ex = p.x, ey = p.y;
        if (p.shift || App.style.shape.constrain) { const a = Math.round(Math.atan2(dy, dx) / (Math.PI / 4)) * (Math.PI / 4), l = Math.hypot(dx, dy); ex = start.x + Math.cos(a) * l; ey = start.y + Math.sin(a) * l; }
        live.nodes = [{ x: start.x, y: start.y, ix: start.x, iy: start.y, ox: start.x, oy: start.y }, { x: ex, y: ey, ix: ex, iy: ey, ox: ex, oy: ey }];
      } else {
        if (p.shift || App.style.shape.constrain) { const m = Math.max(Math.abs(dx), Math.abs(dy)); dx = Math.sign(dx || 1) * m; dy = Math.sign(dy || 1) * m; }
        let x0 = Math.min(start.x, start.x + dx), y0 = Math.min(start.y, start.y + dy), w = Math.abs(dx), h = Math.abs(dy);
        if (p.alt) { x0 = start.x - w; y0 = start.y - h; w *= 2; h *= 2; }
        live.w = Math.max(1, w); live.h = Math.max(1, h); live.m = M.translate(x0, y0);
      }
      if (mids.length) syncMirrors(editableCell(), live, mids);
      changed("shape");
    },
    up() { if (obj) { App.state.selection = [obj.id, ...mids]; App.emit("selection"); } obj = null; start = null; mids = []; },
    cancel() { if (obj) History.doUndo(); obj = null; start = null; }
  };
}
tool("rect", shapeTool("rect"));
tool("ellipse", shapeTool("ellipse"));
tool("poly", shapeTool("poly"));
tool("line", shapeTool("line"));

// ---- Text (typed into an overlay box: the viewer blocks prompt dialogs)
const TextTool = (App.TextTool = {
  open(x, y, existing) {
    const box = $("textEditor"), ta = $("textInput");
    const [sx, sy] = existing ? toScreen(...M.apply(existing.m, 0, 0)) : toScreen(x, y);
    box.style.left = Math.max(4, Math.min(viewW - 260, sx)) + "px"; box.style.top = Math.max(4, Math.min(viewH - 120, sy)) + "px";
    box.hidden = false; ta.value = existing ? existing.text : ""; ta.focus();
    this.pending = { x, y, existing };
  },
  edit(o) { this.open(0, 0, o); },
  commit() {
    const box = $("textEditor"), ta = $("textInput"), p = this.pending; box.hidden = true; this.pending = null;
    if (!p) return;
    const text = ta.value.replace(/\s+$/, "");
    if (!text) return;
    History.push();
    const cell = editableCell(); if (!cell) return;
    if (p.existing) { const o = cell.objects.find((x) => x.id === p.existing.id); if (o) o.text = text; App.state.selection = [p.existing.id]; }
    else {
      const t = App.style.text, st = styleProps();
      const o = { id: uid(), type: "text", text, font: t.font, size: t.size, weight: t.weight, italic: t.italic, align: t.align, m: M.translate(p.x, p.y), ...st, fill: st.fill || "#111" };
      remember(o.fill, o.stroke);
      if (!o.stroke) o.sw = 0;
      cell.objects.push(o); App.state.selection = [o.id];
    }
    changed("text"); App.emit("selection");
  },
  cancel() { $("textEditor").hidden = true; this.pending = null; }
});
tool("text", {
  down(p) {
    if (!layerEditable()) return;
    const hit = hitTop(p.x, p.y, currentObjects());
    if (hit && hit.type === "text") TextTool.edit(hit); else TextTool.open(p.x, p.y, null);
  }
});

// ---- Paint brush (pixels) & eraser
const Paint = (App.Paint = { buffer: document.createElement("canvas"), live: null });
const stampCache = new Map();
function stamp(color, hardness) {
  const key = color + "|" + Math.round(hardness * 20);
  let c = stampCache.get(key); if (c) return c;
  c = document.createElement("canvas"); c.width = c.height = 128;
  const g = c.getContext("2d"), grd = g.createRadialGradient(64, 64, 0, 64, 64, 64);
  const [r, gg, b] = Color.parse(color);
  grd.addColorStop(0, Color.str(r, gg, b, 1)); grd.addColorStop(clamp(hardness, 0, 0.98), Color.str(r, gg, b, 1)); grd.addColorStop(1, Color.str(r, gg, b, 0));
  g.fillStyle = grd; g.fillRect(0, 0, 128, 128);
  if (stampCache.size > 40) stampCache.clear();
  stampCache.set(key, c); return c;
}
// ---- Knife: draw a line across shapes to slice them into separate pieces
/** Splits a picture object in two along the cut line; null if the line doesn't divide it. */
function splitImage(o, side, line, gap) {
  const im = Render.imageCanvas ? Render.imageCanvas(o.src) : null; if (!im) return null;
  const W = im.width || im.naturalWidth, H = im.height || im.naturalHeight; if (!W || !H) return null;
  const inv = M.mul(M.scale(W / o.w, H / o.h), M.inv(o.m));   // document -> picture pixels
  const toPath = (pts, close) => { const path = new Path2D(); pts.forEach(([x, y], i) => { const [u, v] = M.apply(inv, x, y); i ? path.lineTo(u, v) : path.moveTo(u, v); }); if (close) path.closePath(); return path; };
  const path = toPath(side[0][0], true), cutLine = gap > 0 ? toPath(line, false) : null, px = (W / o.w) * M.scaleFactor(M.inv(o.m));
  const make = (keepInside) => {
    const c = document.createElement("canvas"); c.width = W; c.height = H; const g = c.getContext("2d", { willReadFrequently: true });
    g.drawImage(im, 0, 0); g.globalCompositeOperation = keepInside ? "destination-in" : "destination-out"; g.fill(path);
    if (cutLine) { g.globalCompositeOperation = "destination-out"; g.lineWidth = gap * px; g.lineCap = "round"; g.lineJoin = "round"; g.stroke(cutLine); }
    const k = Math.max(1, Math.floor(Math.max(W, H) / 256)), d = g.getImageData(0, 0, W, H).data;
    for (let i = 3; i < d.length; i += 4 * k) if (d[i] > 8) return c;
    return null;
  };
  const a = make(true), b = make(false);
  if (!a || !b) return null;
  return [a, b].map((c, i) => { const src = c.toDataURL("image/png"); Render.cacheImage(src, c); return { ...o, id: i === 0 ? o.id : uid(), group: undefined, src, _side: i === 0 ? 1 : -1 }; });
}
/** Points of the knife's shape for the current mode (document coordinates). closed = a cookie-cutter outline. */
function knifeShape(mode, a, b, snapDeg) {
  if (mode === "rect" || mode === "ellipse") {
    const x0 = Math.min(a[0], b[0]), y0 = Math.min(a[1], b[1]), x1 = Math.max(a[0], b[0]), y1 = Math.max(a[1], b[1]);
    if (mode === "rect") return { pts: [[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]], closed: true };
    const cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2, out = [];
    for (let i = 0; i <= 72; i++) { const t = (i / 72) * Math.PI * 2; out.push([cx + Math.cos(t) * rx, cy + Math.sin(t) * ry]); }
    return { pts: out, closed: true };
  }
  return { pts: [a, snapPoint(a, b, snapDeg)], closed: false };
}
/** b moved so the line a→b sits on the nearest multiple of snapDeg (0 = free angle). */
function snapPoint(a, b, snapDeg) {
  if (!snapDeg) return b;
  const st = (snapDeg * Math.PI) / 180, ang = Math.round(Math.atan2(b[1] - a[1], b[0] - a[0]) / st) * st, len = Math.hypot(b[0] - a[0], b[1] - a[1]);
  return [a[0] + Math.cos(ang) * len, a[1] + Math.sin(ang) * len];
}
App.knifeModes = [["free", "Freehand"], ["line", "Straight line"], ["poly", "Multi-point line"], ["rect", "Rectangle cutter"], ["ellipse", "Circle / oval cutter"]];
tool("knife", (() => {
  let pts = null, start = null, cur = null, poly = null;
  const K = () => App.style.knife;
  const snapOf = (p) => (p && p.shift ? 45 : +K().snap || 0);
  /** Slices everything the cut line (or cookie-cutter outline) crosses. */
  function performCut(line, closed) {
    if (line.length < 2) return;
    const span = Math.max(...line.map((q) => Math.hypot(q[0] - line[0][0], q[1] - line[0][1])));
    if (span < docLen(8)) return;
    if (typeof polygonClipping === "undefined") { toast("The shape library didn't load; check your connection and reload."); return; }
    const k = K(), gap = Math.max(0, +k.gap || 0);
    const side = closed ? [[line.concat([line[0]]).map(([x, y]) => [x, y])]] : Geo.knifeSide(line);
    const outline = closed ? line.concat([line[0]]) : line;
    const opts = {
      thinBand: Geo.eraserRegion([outline.map(([x, y]) => [x, y, 1.6])]),
      gapBand: gap > 0 ? Geo.eraserRegion([outline.map(([x, y]) => [x, y, gap / 2])]) : null
    };
    const sel = App.state.selection.length ? new Set(App.state.selection) : null;
    const plan = [];
    const collect = (only) => {
      for (const i of linkedLayers()) {
        const objs = objectsOf(i).filter((o) => !only || only.has(o.id));
        const cutMap = new Map();
        for (const o of objs) {
          let r = null;
          try { r = o.type === "image" ? splitImage(o, side, outline, gap) : Geo.knife(o, outline, side, opts); } catch (e) { r = null; }
          if (r) cutMap.set(o.id, r);
        }
        if (cutMap.size) plan.push([i, cutMap]);
      }
    };
    if (k.target === "selection") { if (!sel) { toast("Select what to cut first (or set “Cut” to everything under the line)."); return; } collect(sel); }
    else if (k.target === "all") collect(null);
    else { if (sel) collect(sel); if (!plan.length) collect(null); }   // the selection if the line crosses it, otherwise everything
    if (!plan.length) { toast(closed ? "The cutter didn't overlap anything — draw it across the edge of a shape." : "The cut line didn't go all the way across anything. Draw it right through a shape."); return; }
    // pull the pieces apart along the cut (a cookie cutter lifts its inside pieces up and to the right)
    const spread = Math.max(0, +k.spread || 0);
    let nx = 0, ny = 0;
    if (closed) { nx = Math.SQRT1_2; ny = -Math.SQRT1_2; }
    else { const a = line[0], b = line[line.length - 1], l = Math.hypot(b[0] - a[0], b[1] - a[1]) || 1; nx = -(b[1] - a[1]) / l; ny = (b[0] - a[0]) / l; }
    History.push();
    const made = [];
    for (const [i, cutMap] of plan) {
      const cell = withLayer(i, () => editableCell());
      for (const r of cutMap.values()) for (const piece of r) {
        if (spread) { const d = closed ? (piece._side > 0 ? spread : 0) : (piece._side * spread) / 2; piece.m = M.mul(M.translate(nx * d, ny * d), piece.m); }
        delete piece._side;
      }
      cell.objects = cell.objects.flatMap((o) => (cutMap.has(o.id) ? cutMap.get(o.id) : [o]));
      for (const r of cutMap.values()) made.push(...r.map((x) => x.id));
    }
    App.state.selection = made;
    changed("knife"); App.emit("selection");
    toast(`Cut into ${made.length} pieces — they're selected; drag them with the Select tool (V).`);
  }
  return {
    down(p) {
      if (!layerEditable()) return;
      const m = K().mode;
      if (m === "poly") {
        if (!poly) poly = [[p.x, p.y]];
        else {
          const last = poly[poly.length - 1], q = snapPoint(last, [p.x, p.y], snapOf(p));
          // tapping the first point again closes the shape into a cookie cutter
          if (poly.length > 2 && Math.hypot(q[0] - poly[0][0], q[1] - poly[0][1]) < docLen(12)) { const pl = poly; poly = null; cur = null; performCut(pl, true); return; }
          poly.push(q);
        }
        cur = [p.x, p.y]; return;
      }
      start = [p.x, p.y]; cur = start; pts = m === "free" ? [[p.x, p.y]] : null;
    },
    move(p) {
      if (K().mode === "poly") { if (poly) cur = [p.x, p.y]; return; }
      if (!start) return;
      cur = [p.x, p.y];
      if (pts) { const l = pts[pts.length - 1]; if (Math.hypot(p.x - l[0], p.y - l[1]) > docLen(3)) pts.push([p.x, p.y]); }
    },
    hover(p) { if (K().mode === "poly" && poly) { cur = [p.x, p.y]; this._shift = p.shift; } },
    up(p) {
      const m = K().mode;
      if (m === "poly") return;
      if (!start) return;
      const a = start, b = p ? [p.x, p.y] : cur; start = null; cur = null;
      if (m === "free") { const line = rdp(pts.concat([b]), docLen(1.5)); pts = null; performCut(line, false); return; }
      if (m === "rect" || m === "ellipse") { let bb = b; if (p && (p.shift || K().snap)) { const s = Math.max(Math.abs(b[0] - a[0]), Math.abs(b[1] - a[1])); bb = [a[0] + Math.sign(b[0] - a[0] || 1) * s, a[1] + Math.sign(b[1] - a[1] || 1) * s]; } const sh = knifeShape(m, a, bb); performCut(sh.pts.slice(0, -1), true); return; }
      const sh = knifeShape(m, a, b, snapOf(p)); performCut(sh.pts, false);
    },
    dbl() { this.finish(); },
    /** Multi-point line: cut along the points placed so far. */
    finish() { if (poly && poly.length > 1) { const pl = poly; poly = null; cur = null; performCut(pl, false); } else { poly = null; cur = null; } View.redraw(); },
    cancel() { pts = null; start = null; cur = null; poly = null; },
    overlay(g) {
      const m = K().mode, line = [];
      if (m === "poly" && poly) { line.push(...poly); if (cur) line.push(snapPoint(poly[poly.length - 1], cur, this._shift ? 45 : +K().snap || 0)); }
      else if (start && cur) {
        if (m === "free" && pts) line.push(...pts);
        else if (m === "rect" || m === "ellipse") line.push(...knifeShape(m, start, cur).pts);
        else line.push(...knifeShape(m, start, cur, +K().snap || 0).pts);
      }
      if (line.length < 2) return;
      g.save(); g.lineWidth = 2; g.strokeStyle = "#e11d48"; g.setLineDash([7, 5]); g.beginPath();
      line.forEach(([x, y], i) => { const [a, b] = toScreen(x, y); i ? g.lineTo(a, b) : g.moveTo(a, b); }); g.stroke();
      // gap preview
      if (+K().gap > 0) { g.globalAlpha = 0.18; g.setLineDash([]); g.lineWidth = +K().gap * App.state.zoom; g.lineCap = "round"; g.lineJoin = "round"; g.stroke(); }
      g.restore();
      if (m === "poly" && poly) for (const [x, y] of poly) { const [a, b] = toScreen(x, y); g.fillStyle = "#fff"; g.strokeStyle = "#e11d48"; g.lineWidth = 1.5; g.fillRect(a - 4, b - 4, 8, 8); g.strokeRect(a - 4, b - 4, 8, 8); }
    }
  };
})());

// ---- Paint brush presets & engine
/**
 * Each brush's starting settings. Every brush understands the common settings (size, opacity, flow,
 * spacing, jitter, scatter, angle…); a few add their own (hardness, density, mixing).
 */
const PAINT_BRUSHES = {
  round:      { name: "Round",        size: 24, hardness: 0.7,  opacity: 1,    flow: 1,    spacing: 0.08 },
  ink:        { name: "Ink pen",      size: 8,  hardness: 0.98, opacity: 1,    flow: 1,    spacing: 0.03, pressureMin: 0.03 },
  airbrush:   { name: "Airbrush",     size: 90, hardness: 0,    opacity: 1,    flow: 0.1,  spacing: 0.05 },
  spray:      { name: "Spray paint",  size: 70, hardness: 1,    opacity: 1,    flow: 0.8,  spacing: 0.18, density: 1 },
  dots:       { name: "Dotted spray", size: 80, hardness: 1,    opacity: 1,    flow: 1,    spacing: 0.45, density: 1 },
  oil:        { name: "Oil paint",    size: 44, hardness: 0.8,  opacity: 1,    flow: 0.9,  spacing: 0.05, follow: true, mix: 0.35, direct: true },
  smudge:     { name: "Smudge",       size: 44, hardness: 0.5,  opacity: 1,    flow: 0.75, spacing: 0.08, direct: true },
  blur:       { name: "Blur",         size: 60, hardness: 0.5,  opacity: 1,    flow: 0.5,  spacing: 0.15, direct: true },
  chalk:      { name: "Chalk",        size: 40, hardness: 0.9,  opacity: 1,    flow: 0.85, spacing: 0.14, angleJitter: 1 },
  charcoal:   { name: "Charcoal",     size: 34, hardness: 0.9,  opacity: 1,    flow: 0.7,  spacing: 0.08, follow: true, angleJitter: 0.08 },
  crayon:     { name: "Crayon",       size: 18, hardness: 0.95, opacity: 1,    flow: 0.9,  spacing: 0.1,  angleJitter: 1 },
  pencil:     { name: "Pencil",       size: 6,  hardness: 0.9,  opacity: 0.95, flow: 0.7,  spacing: 0.12, angleJitter: 1 },
  marker:     { name: "Marker",       size: 30, hardness: 0.95, opacity: 0.6,  flow: 1,    spacing: 0.04, angle: -45 },
  watercolor: { name: "Watercolour",  size: 70, hardness: 0.3,  opacity: 0.8,  flow: 0.07, spacing: 0.03, sizeJitter: 0.08 },
  hatching:   { name: "Hatching",     size: 40, hardness: 1,    opacity: 1,    flow: 1,    spacing: 0.22, angle: 45 },
  glow:       { name: "Glow / neon",  size: 40, hardness: 0.2,  opacity: 0.9,  flow: 0.35, spacing: 0.06, blend: "lighter" }
};
App.PAINT_BRUSHES = PAINT_BRUSHES;
/** Brushes the eraser can use (the ones that mix or blur existing paint make no sense there). */
App.ERASER_BRUSHES = Object.keys(PAINT_BRUSHES).filter((k) => !PAINT_BRUSHES[k].direct && k !== "glow");
const BRUSH_KEYS = ["dotSize", "size", "hardness", "opacity", "flow", "spacing", "sizeJitter", "scatter", "angle", "angleJitter", "follow", "colorJitter", "density", "mix", "smoothing", "pressureSize", "pressureOpacity", "pressureMin"];
function brushDefaults(kind) {
  const B = PAINT_BRUSHES[kind] || PAINT_BRUSHES.round;
  return { dotSize: 1, size: B.size, hardness: B.hardness, opacity: B.opacity, flow: B.flow, spacing: B.spacing, sizeJitter: B.sizeJitter || 0, scatter: 0, angle: B.angle || 0, angleJitter: B.angleJitter || 0, follow: !!B.follow, colorJitter: 0, density: B.density || 1, mix: B.mix ?? 0.35, smoothing: 0, pressureSize: true, pressureOpacity: false, pressureMin: B.pressureMin ?? 0.15 };
}
App.brushDefaults = brushDefaults;

// white alpha-mask textures, tinted per colour on demand
const texCache = new Map();
function texture(kind) {
  if (texCache.has(kind)) return texCache.get(kind);
  const S = 128, c = document.createElement("canvas"); c.width = c.height = S;
  const g = c.getContext("2d");
  if (kind === "chalk" || kind === "pencil" || kind === "crayon") {
    // grain: coarse (chalk), fine (pencil) or dense waxy (crayon) speckle inside a disc
    const R = kind === "chalk" ? 40 : kind === "crayon" ? 64 : 128, t = document.createElement("canvas"); t.width = t.height = R;
    const tg = t.getContext("2d"), im = tg.createImageData(R, R), dens = kind === "chalk" ? 0.6 : kind === "crayon" ? 0.78 : 0.5;
    for (let y = 0; y < R; y++) for (let x = 0; x < R; x++) {
      const d = Math.hypot(x - R / 2 + 0.5, y - R / 2 + 0.5) / (R / 2);
      const keep = Math.random() < dens * (1 - Math.pow(Math.min(1, d), kind === "crayon" ? 8 : 4));
      im.data[(y * R + x) * 4 + 3] = keep ? (kind === "crayon" ? 220 : 140) + Math.random() * 35 : 0;
      im.data[(y * R + x) * 4] = im.data[(y * R + x) * 4 + 1] = im.data[(y * R + x) * 4 + 2] = 255;
    }
    tg.putImageData(im, 0, 0); g.imageSmoothingEnabled = kind !== "chalk"; g.drawImage(t, 0, 0, S, S);
  } else if (kind === "charcoal") {
    // long broken streaks, soft ends
    g.save(); g.beginPath(); g.ellipse(64, 64, 62, 40, 0, 0, Math.PI * 2); g.clip();
    for (let k = 0; k < 90; k++) {
      const y = 20 + Math.random() * 88, x = Math.random() * 90, w = 8 + Math.random() * 50;
      g.globalAlpha = 0.25 + Math.random() * 0.6; g.fillStyle = "#fff"; g.fillRect(x, y, w, 0.8 + Math.random() * 2.2);
    }
    g.restore();
  } else if (kind === "bristle") {
    // streaks running along x inside an oval, like a loaded flat brush
    g.save(); g.beginPath(); g.ellipse(64, 64, 62, 52, 0, 0, Math.PI * 2); g.clip();
    for (let k = 0; k < 34; k++) {
      const y = 12 + Math.random() * 104, h = 1 + Math.random() * 4;
      g.globalAlpha = 0.45 + Math.random() * 0.55; g.fillStyle = "#fff";
      g.fillRect(Math.random() * 14, y, 100 + Math.random() * 28, h);
    }
    g.restore();
  } else if (kind === "watercolor") {
    // pale middle, darker pooled rim
    const grd = g.createRadialGradient(64, 64, 0, 64, 64, 64);
    grd.addColorStop(0, "rgba(255,255,255,.35)"); grd.addColorStop(0.72, "rgba(255,255,255,.55)"); grd.addColorStop(0.9, "rgba(255,255,255,.9)"); grd.addColorStop(1, "rgba(255,255,255,0)");
    g.fillStyle = grd; g.beginPath(); g.arc(64, 64, 64, 0, Math.PI * 2); g.fill();
  } else if (kind === "chisel") {
    g.fillStyle = "#fff"; g.beginPath(); g.ellipse(64, 64, 63, 20, 0, 0, Math.PI * 2); g.fill();
  } else if (kind === "soft") {
    const grd = g.createRadialGradient(64, 64, 0, 64, 64, 64);
    grd.addColorStop(0, "rgba(255,255,255,1)"); grd.addColorStop(1, "rgba(255,255,255,0)");
    g.fillStyle = grd; g.fillRect(0, 0, S, S);
  }
  texCache.set(kind, c); return c;
}
const tintCache = new Map();
function tinted(kind, color) {
  const key = kind + "|" + color;
  let c = tintCache.get(key); if (c) return c;
  const t = texture(kind); c = document.createElement("canvas"); c.width = t.width; c.height = t.height;
  const g = c.getContext("2d"); g.drawImage(t, 0, 0); g.globalCompositeOperation = "source-in"; g.fillStyle = color; g.fillRect(0, 0, c.width, c.height);
  if (tintCache.size > 120) tintCache.clear();
  tintCache.set(key, c); return c;
}
const rotDraw = (g, img, x, y, w, h, a) => { g.save(); g.translate(x, y); g.rotate(a); g.drawImage(img, -w / 2, -h / 2, w, h); g.restore(); };
/** A random nearby colour (hue and lightness), rounded so the texture caches stay small. */
function jitterColor(rgb, amt) {
  let [r, g, b] = rgb.map((v) => v / 255);
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b); let h = 0, l = (mx + mn) / 2, s = 0;
  if (mx !== mn) { const d = mx - mn; s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn); h = mx === r ? (g - b) / d + (g < b ? 6 : 0) : mx === g ? (b - r) / d + 2 : (r - g) / d + 4; h /= 6; }
  h = (h + (Math.random() - 0.5) * amt * 0.5 + 1) % 1; l = clamp(l + (Math.random() - 0.5) * amt * 0.5, 0, 1);
  const q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  const f = (t) => { t = (t + 1) % 1; return t < 1 / 6 ? p + (q - p) * 6 * t : t < 1 / 2 ? q : t < 2 / 3 ? p + (q - p) * (2 / 3 - t) * 6 : p; };
  const Q = (v) => Math.round(v * 255 / 8) * 8;
  return `rgb(${Q(f(h + 1 / 3))},${Q(f(h))},${Q(f(h - 1 / 3))})`;
}

/**
 * One stroke of a paint brush. `g` is where dabs land: the stroke buffer, or the layer's pixels
 * themselves for brushes that change what's already painted (oil, smudge, blur).
 * c holds the brush settings; `erase` makes every dab solid black (the buffer is then cut out).
 */
function makePainter(g, kind, c, color, erase) {
  const B = PAINT_BRUSHES[kind] || PAINT_BRUSHES.round;
  const base = Color.parse(color);
  const oil = [], smudges = [];
  let tmp = null;
  // oil reads the colours underneath from one snapshot taken at the start of the stroke (fast on phones)
  let snap = null;
  const sample = (x, y) => {
    if (!snap) { try { snap = g.getImageData(0, 0, g.canvas.width, g.canvas.height); } catch (e) { snap = { width: 0 }; } }
    const px = Math.round(x), py = Math.round(y);
    if (px < 0 || py < 0 || px >= snap.width || py >= snap.height) return null;
    const i = (py * snap.width + px) * 4; return snap.data.subarray(i, i + 4);
  };
  return {
    lastSize: c.size,
    get spacing() { return c.spacing ?? B.spacing; },
    dab(x, y, p, dir, i) {
      const pm = c.pressureMin ?? 0.15;
      let size = Math.max(0.5, c.size * (c.pressureSize ? pm + (1 - pm) * p : 1));
      if (c.sizeJitter) size *= 1 - c.sizeJitter * Math.random();
      if (c.scatter) { const a = Math.random() * Math.PI * 2, d = Math.random() * c.scatter * c.size; x += Math.cos(a) * d; y += Math.sin(a) * d; }
      this.lastSize = size;
      const rot = ((c.angle || 0) * Math.PI) / 180 + (c.follow ? dir : 0) + (c.angleJitter ? (Math.random() - 0.5) * Math.PI * 2 * c.angleJitter : 0);
      const alpha = clamp(c.flow * (c.pressureOpacity ? p : 1), 0.01, 1);
      const col = erase ? "#000" : c.colorJitter ? jitterColor(base, c.colorJitter) : color;
      g.globalAlpha = alpha; g.globalCompositeOperation = "source-over";
      switch (kind) {
        case "spray": case "dots": {
          const dots = kind === "dots", dn = c.density ?? 1;
          const n = Math.max(1, Math.round((dots ? clamp(size / 12, 2, 9) : clamp((size * size) / 50, 8, 220)) * dn));
          g.fillStyle = col; g.beginPath();
          for (let k = 0; k < n; k++) {
            const a = Math.random() * Math.PI * 2, d = (size / 2) * (dots ? Math.sqrt(Math.random()) : Math.pow(Math.random(), 0.75));
            const ds = c.dotSize ?? 1;
            const r = Math.max(0.4, (dots ? size * (0.035 + Math.random() * 0.07) : size * 0.012 * (0.5 + Math.random())) * ds);
            const px = x + Math.cos(a) * d, py = y + Math.sin(a) * d; g.moveTo(px + r, py); g.arc(px, py, r, 0, Math.PI * 2);
          }
          g.fill(); break;
        }
        case "chalk": case "pencil": case "crayon": rotDraw(g, tinted(kind, col), x, y, size, size, rot); break;
        case "charcoal": rotDraw(g, tinted("charcoal", col), x, y, size, size * 0.8, rot); break;
        case "watercolor": rotDraw(g, tinted("watercolor", col), x, y, size, size, rot); break;
        case "marker": rotDraw(g, tinted("chisel", col), x, y, size, size, rot); break;
        case "airbrush": case "glow": g.drawImage(kind === "glow" ? stamp(col, c.hardness) : tinted("soft", col), x - size / 2, y - size / 2, size, size); break;
        case "hatching": {
          // one line per dab, all at the same angle: a hatched band
          const len = size, w = Math.max(0.6, size * 0.035), ca = Math.cos(rot), sa = Math.sin(rot);
          g.strokeStyle = col; g.lineWidth = w; g.lineCap = "round";
          g.beginPath(); g.moveTo(x - ca * len / 2, y - sa * len / 2); g.lineTo(x + ca * len / 2, y + sa * len / 2); g.stroke();
          break;
        }
        case "oil": {
          // the brush picks up some of the colour underneath and carries it along
          let cur = oil[i] || (oil[i] = base.slice(0, 3));
          const u = sample(x, y), mix = c.mix ?? 0.35;
          if (u && u[3] > 20) for (let k = 0; k < 3; k++) cur[k] += (u[k] - cur[k]) * mix * 0.22;
          for (let k = 0; k < 3; k++) cur[k] += (base[k] - cur[k]) * 0.015;
          const q = (v) => Math.round(v / 6) * 6;   // fewer distinct tints to cache
          rotDraw(g, tinted("bristle", c.colorJitter ? jitterColor(cur, c.colorJitter) : `rgb(${q(cur[0])},${q(cur[1])},${q(cur[2])})`), x, y, size, size * 0.8, rot);
          break;
        }
        case "smudge": {
          // carry a patch of paint and lay it down again a little further on
          const D = Math.max(4, Math.ceil(c.size));
          let sm = smudges[i];
          if (!sm) {
            const cc = document.createElement("canvas"); cc.width = cc.height = D;
            const t = document.createElement("canvas"); t.width = t.height = D;
            sm = smudges[i] = { c: cc, g: cc.getContext("2d"), t, tg: t.getContext("2d") };
            sm.g.drawImage(g.canvas, x - D / 2, y - D / 2, D, D, 0, 0, D, D);
            break;
          }
          sm.tg.globalCompositeOperation = "source-over"; sm.tg.clearRect(0, 0, D, D); sm.tg.drawImage(sm.c, 0, 0);
          sm.tg.globalCompositeOperation = "destination-in"; sm.tg.drawImage(texture("soft"), 0, 0, D, D);
          g.globalAlpha = clamp(c.flow, 0.05, 0.98); g.drawImage(sm.t, x - D / 2, y - D / 2);
          sm.g.globalAlpha = 1 - clamp(c.flow, 0.05, 0.98) * 0.85; sm.g.drawImage(g.canvas, x - D / 2, y - D / 2, D, D, 0, 0, D, D); sm.g.globalAlpha = 1;
          break;
        }
        case "blur": {
          // soften: shrink the patch and scale it back up (works in every browser), laid down through a soft edge
          const D = Math.max(4, Math.ceil(size)), k = Math.max(2, Math.round(2 + c.flow * 6));
          if (!tmp) { tmp = [document.createElement("canvas"), document.createElement("canvas")]; }
          const [a, b2] = tmp; if (a.width !== D) { a.width = a.height = D; } const sw = Math.max(1, Math.round(D / k)); if (b2.width !== sw) { b2.width = b2.height = sw; }
          const ag = a.getContext("2d"), bg = b2.getContext("2d");
          bg.clearRect(0, 0, sw, sw); bg.imageSmoothingQuality = "high"; bg.drawImage(g.canvas, x - D / 2, y - D / 2, D, D, 0, 0, sw, sw);
          ag.globalCompositeOperation = "source-over"; ag.clearRect(0, 0, D, D); ag.imageSmoothingQuality = "high"; ag.drawImage(b2, 0, 0, D, D);
          ag.globalCompositeOperation = "destination-in"; ag.drawImage(texture("soft"), 0, 0, D, D);
          g.globalAlpha = 0.85; g.drawImage(a, x - D / 2, y - D / 2);
          break;
        }
        default: g.drawImage(stamp(col, c.hardness), x - size / 2, y - size / 2, size, size);
      }
      g.globalAlpha = 1;
    }
  };
}
/** Draws a sample stroke of a brush into a small canvas (for the brush pickers). */
App.paintPreview = function (canvas, kind, color, settings, erase) {
  const g = canvas.getContext("2d"), W = canvas.width, H = canvas.height;
  g.clearRect(0, 0, W, H);
  const B = PAINT_BRUSHES[kind], c = { ...brushDefaults(kind), ...(settings || {}) };
  c.size = ["spray", "dots", "airbrush", "watercolor", "glow", "blur"].includes(kind) ? H * 0.75 : Math.min(H * 0.55, Math.max(3, c.size * 0.3));
  if (erase) { g.fillStyle = "#9aa3ae"; g.fillRect(4, 6, W - 8, H - 12); }
  else if (kind === "smudge" || kind === "blur") { ["#e11d48", "#f59e0b", "#2563eb"].forEach((col, k) => { g.fillStyle = col; g.fillRect(W * (0.12 + k * 0.28), 4, W * 0.18, H - 8); }); }
  else if (kind === "glow") { g.fillStyle = "#111827"; g.fillRect(0, 0, W, H); }
  const direct = B.direct && !erase, buf = direct ? null : document.createElement("canvas");
  if (buf) { buf.width = W; buf.height = H; }
  const P = makePainter(direct ? g : buf.getContext("2d"), kind, c, color, erase);
  let last = null;
  for (let i = 0; i <= 60; i++) {
    const t = i / 60, x = 8 + t * (W - 16), y = H / 2 + Math.sin(t * Math.PI * 2) * H * 0.22, p = 0.35 + 0.65 * Math.sin(t * Math.PI);
    if (last) {
      const dist = Math.hypot(x - last.x, y - last.y), step = Math.max(0.5, c.size * P.spacing), dir = Math.atan2(y - last.y, x - last.x);
      for (let d = step; d <= dist; d += step) { const k = d / dist; P.dab(last.x + (x - last.x) * k, last.y + (y - last.y) * k, p, dir, 0); }
    } else P.dab(x, y, p, 0, 0);
    last = { x, y };
  }
  if (buf) { g.save(); g.globalAlpha = c.opacity; g.globalCompositeOperation = erase ? "destination-out" : B.blend || "source-over"; g.drawImage(buf, 0, 0); g.restore(); }
};
/** Switches brush for the paint brush or the eraser; each brush remembers its own settings. */
App.setPaintBrush = function (kind, forEraser) {
  if (!PAINT_BRUSHES[kind]) return;
  const tgt = forEraser ? App.style.eraser : App.style.paint;
  const store = forEraser ? (App.style.eraserSets = App.style.eraserSets || {}) : (App.style.paintSets = App.style.paintSets || {});
  const cur = tgt.brush || "round";
  store[cur] = Object.fromEntries(BRUSH_KEYS.map((k) => [k, tgt[k]]));
  Object.assign(tgt, store[kind] || brushDefaults(kind), { brush: kind });
};
App.resetPaintBrush = function (forEraser) {
  const tgt = forEraser ? App.style.eraser : App.style.paint;
  Object.assign(tgt, brushDefaults(tgt.brush || "round"));
};
// start both tools with a full set of settings
Object.assign(App.style.paint, { ...brushDefaults(App.style.paint.brush || "round"), brush: App.style.paint.brush || "round" });
Object.assign(App.style.eraser, { ...brushDefaults("round"), hardness: 0.8, size: App.style.eraser.size || 40, brush: "round" });

/** Erases the stroke buffer out of every vector object it touches, turning those objects into images. */
function textureErase(cell, trails, alpha, only) {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const t of trails) for (const [x, y, r] of t) { x0 = Math.min(x0, x - r); y0 = Math.min(y0, y - r); x1 = Math.max(x1, x + r); y1 = Math.max(y1, y + r); }
  const d = App.doc, bg = Paint.buffer.getContext("2d"), S = Paint.buffer.width / d.width;
  const skip = Mask.current ? Mask.current.ids : null;
  let n = 0;
  cell.objects = cell.objects.map((o) => {
    if ((skip && skip.has(o.id)) || (only && !only(o))) return o;
    const b = Render.docBounds(o);
    const ix0 = Math.max(0, Math.floor(Math.max(b.x, x0))), iy0 = Math.max(0, Math.floor(Math.max(b.y, y0)));
    const ix1 = Math.min(d.width, Math.ceil(Math.min(b.x + b.w, x1))), iy1 = Math.min(d.height, Math.ceil(Math.min(b.y + b.h, y1)));
    if (ix1 <= ix0 || iy1 <= iy0) return o;
    const px = bg.getImageData(Math.floor(ix0 * S), Math.floor(iy0 * S), Math.max(1, Math.round((ix1 - ix0) * S)), Math.max(1, Math.round((iy1 - iy0) * S))).data;
    let hit = false; for (let k = 3; k < px.length; k += 16) if (px[k]) { hit = true; break; }
    if (!hit) return o;
    const bx = Math.floor(b.x) - 1, by = Math.floor(b.y) - 1, bw = Math.max(1, Math.ceil(b.w) + 3), bh = Math.max(1, Math.ceil(b.h) + 3);
    const c = document.createElement("canvas"); c.width = Math.ceil(bw * S); c.height = Math.ceil(bh * S);
    const g = c.getContext("2d"); g.imageSmoothingQuality = "high"; g.setTransform(S, 0, 0, S, -bx * S, -by * S);
    Render.drawObj(g, { ...o, opacity: 1, blend: "source-over" });
    g.setTransform(1, 0, 0, 1, 0, 0); g.globalAlpha = alpha; g.globalCompositeOperation = "destination-out"; g.drawImage(Paint.buffer, -bx * S, -by * S);
    const src = c.toDataURL("image/png"); Render.cacheImage(src, c); n++;
    return { id: o.id, group: o.group, type: "image", src, w: bw, h: bh, m: M.translate(bx, by), fill: null, stroke: null, sw: 0, opacity: o.opacity ?? 1, blend: o.blend || "source-over" };
  });
  if (n) App.emit("selection");
}

/** Clears the stroke buffer at paint resolution and returns its context (document coordinates). */
function prepBuffer() {
  const d = App.doc, b = Paint.buffer, s = paintScale(), W = Math.round(d.width * s), H = Math.round(d.height * s);
  if (b.width !== W || b.height !== H) { b.width = W; b.height = H; }
  const g = b.getContext("2d"); g.setTransform(1, 0, 0, 1, 0, 0); g.globalAlpha = 1; g.globalCompositeOperation = "source-over"; g.clearRect(0, 0, W, H);
  g.setTransform(s, 0, 0, s, 0, 0);
  return g;
}
/** Turns the finished paint stroke into its own picture object, so it can be selected, moved and deleted. */
function strokeObject(box, lv) {
  if (!box) return null;
  const d = App.doc, S = Paint.buffer.width / d.width;
  const x0 = Math.max(0, Math.floor(box[0])), y0 = Math.max(0, Math.floor(box[1]));
  const x1 = Math.min(d.width, Math.ceil(box[2])), y1 = Math.min(d.height, Math.ceil(box[3]));
  if (x1 <= x0 || y1 <= y0) return null;
  const w = x1 - x0, h = y1 - y0, c = document.createElement("canvas");
  c.width = Math.max(1, Math.round(w * S)); c.height = Math.max(1, Math.round(h * S));
  c.getContext("2d").drawImage(Paint.buffer, Math.round(x0 * S), Math.round(y0 * S), c.width, c.height, 0, 0, c.width, c.height);
  const src = c.toDataURL("image/png"); Render.cacheImage(src, c);
  return { id: uid(), type: "image", paint: true, src, w, h, m: M.translate(x0, y0), fill: null, stroke: null, sw: 0, opacity: lv.alpha, blend: lv.blend || "source-over" };
}

function paintTool(erase) {
  let last = null, cell = null, leftover = 0, trails = null, painter = null, before = null, maskLayer = null, maskBefore = null;
  const cfg = () => (erase ? App.style.eraser : App.style.paint);
  let sm = null;   // stabiliser state
  const mode = () => (erase ? App.style.eraser.mode || "all" : "pixels");
  // a mirrored copy of a stroke also mirrors its direction
  const turn = (T, a) => Math.atan2(T[1] * Math.cos(a) + T[3] * Math.sin(a), T[0] * Math.cos(a) + T[2] * Math.sin(a));
  let box = null;   // what the stroke covers (document coordinates)
  const dab = (g, x, y, p, dir = 0) => {
    Sym.all().forEach((T, i) => {
      const [px, py] = M.apply(T, x, y);
      painter.dab(px, py, p, turn(T, dir), i);
      const c = cfg(), pad = c.size * (0.6 + (c.scatter || 0)) + 3;
      if (!box) box = [px - pad, py - pad, px + pad, py + pad];
      else { box[0] = Math.min(box[0], px - pad); box[1] = Math.min(box[1], py - pad); box[2] = Math.max(box[2], px + pad); box[3] = Math.max(box[3], py + pad); }
      if (trails) (trails[i] = trails[i] || []).push([px, py, painter.lastSize / 2]);
    });
  };
  return {
    down(p) {
      const AL = activeLayer();
      if (AL && AL.maskEdit && AL.mask) { this.maskDown(p, AL); return; }
      if (erase && mode() === "objects") { this.eraseObjects(p, true); return; }
      if (!layerEditable()) return;
      History.push();
      cell = editableCell();
      const m = mode(), pixels = m === "pixels" || m === "all", vectors = m === "cut" || m === "all";
      before = cell.raster || null;   // the untouched pixels (history keeps them), used by the mask
      if (pixels && (cell.raster || !erase)) rasterForPaint(cell);
      buildStrokeCache();
      const g = prepBuffer();
      box = null;
      let kind = cfg().brush || "round";
      if (erase && !App.ERASER_BRUSHES.includes(kind)) kind = "round";
      if (!erase && !PAINT_BRUSHES[kind].direct) remember(cfg().color);
      const direct = !erase && !!PAINT_BRUSHES[kind].direct;
      // oil, smudge & blur work straight on the layer's pixels (they change what's there)
      if (direct && !cell.raster) rasterForPaint(cell);
      painter = makePainter(direct ? cell.raster.getContext("2d") : g, kind, cfg(), erase ? "#000" : cfg().color, erase);
      Paint.live = { alpha: direct ? 1 : cfg().opacity, erase, pixels: pixels && !direct, vectors, direct, blend: erase ? null : PAINT_BRUSHES[kind].blend, asObject: !erase && !direct && App.style.paint.asObject !== false };
      trails = vectors || erase ? [] : null;   // the eraser always records its path (pictures on the layer are erased too)
      sm = { x: p.x, y: p.y, p: p.p };
      last = p; leftover = 0; dab(g, p.x, p.y, p.p); this.masked(); changed("paint");
    },
    move(p) {
      if (erase && mode() === "objects" && !maskLayer) { if (this._erasing) this.eraseObjects(p, false); return; }
      if (!last || (!cell && !maskLayer)) return;
      const k0 = (cfg().smoothing || 0) * 0.92;
      if (k0 > 0) { sm.x = sm.x * k0 + p.x * (1 - k0); sm.y = sm.y * k0 + p.y * (1 - k0); sm.p = sm.p * k0 + p.p * (1 - k0); p = { ...p, x: sm.x, y: sm.y, p: sm.p }; }
      const g = Paint.buffer.getContext("2d");
      const dist = Math.hypot(p.x - last.x, p.y - last.y);
      const step = Math.max(0.5, cfg().size * Math.max(0.01, painter.spacing));
      const dir = Math.atan2(p.y - last.y, p.x - last.x);
      let t = leftover;
      while (t < dist) { const k = t / dist; dab(g, lerp(last.x, p.x, k), lerp(last.y, p.y, k), lerp(last.p, p.p, k), dir); t += step; }
      leftover = t - dist; last = p; this.masked(); if (maskLayer) this.maskUpdate(); changed("paint");
    },
    /** With a mask on, keeps the stroke out of the protected area as it is drawn. */
    masked() {
      if (!Mask.current || !Paint.live) return;
      if (Paint.live.direct) Mask.restore(cell.raster.getContext("2d"), before);
      else Mask.clipBuffer(Paint.buffer.getContext("2d"));
    },
    /** Painting on a layer mask: the paint brush hides, the eraser shows the layer again. */
    maskDown(p, L) {
      if (L.locked) { toast("This layer is locked."); return; }
      History.push();
      maskLayer = L; maskBefore = L.mask.canvas;
      L.mask = { ...L.mask, canvas: cloneCanvas(maskBefore) };   // copy-on-write keeps undo intact
      buildStrokeCache();
      const g = prepBuffer();
      let kind = cfg().brush || "round"; if (PAINT_BRUSHES[kind].direct) kind = "round";
      painter = makePainter(g, kind, cfg(), "#000", true);
      Paint.live = { alpha: cfg().opacity, erase, pixels: false, vectors: false, maskEdit: true };
      trails = null; sm = { x: p.x, y: p.y, p: p.p };
      last = p; leftover = 0; dab(g, p.x, p.y, p.p); this.masked(); this.maskUpdate(); changed("paint");
    },
    maskUpdate() {
      const mg = maskLayer.mask.canvas.getContext("2d");
      mg.save(); mg.globalAlpha = 1; mg.globalCompositeOperation = "copy"; mg.drawImage(maskBefore, 0, 0);
      mg.globalAlpha = Paint.live.alpha; mg.globalCompositeOperation = erase ? "source-over" : "destination-out"; drawBuffer(mg); mg.restore();
    },
    up() {
      this._erasing = false;
      if (maskLayer) { maskLayer = null; maskBefore = null; Paint.live = null; painter = null; last = null; changed("mask"); return; }
      if (!cell || !Paint.live) { cell = null; return; }
      const lv = Paint.live;
      if (lv.asObject) {
        const o = strokeObject(box, lv);
        if (o) { cell.objects.push(o); }
      } else if (lv.pixels && cell.raster) {
        const g = cell.raster.getContext("2d");
        g.save(); g.globalAlpha = lv.alpha; g.globalCompositeOperation = erase ? "destination-out" : lv.blend || "source-over";
        drawBuffer(g); g.restore();
      }
      const ek = App.style.eraser.brush || "round";
      if (lv.vectors && trails && cell.objects.length && erase && App.style.eraser.vectorMode !== "cut" && !["round", "ink"].includes(ek)) {
        // textured erasers: the touched shapes become pixels so the texture can show
        textureErase(cell, trails, lv.alpha);
      } else {
        if (lv.vectors && trails && cell.objects.length) {
          try {
            const res = Geo.cutCell(cell, Mask.clipRegion(Geo.eraserRegion(trails)), Mask.current ? Mask.current.ids : null);
            if (res.removed.length) App.state.selection = App.state.selection.filter((id) => !res.removed.includes(id));
            if (res.changed) App.emit("selection");
          } catch (e) { console.warn("vector erase failed", e); }
        }
        // pictures (imported images, and shapes a textured eraser already turned into pixels) can't be cut
        // as vectors, so every eraser rubs them out pixel by pixel; text too when vectors are being erased
        if (erase && trails && cell.objects.length) textureErase(cell, trails, lv.alpha, (o) => o.type === "image" || (lv.vectors && o.type === "text"));
      }
      Paint.live = null; cell = null; last = null; trails = null; painter = null; changed("paint");
    },
    cancel() {
      if (maskLayer) { maskLayer = null; maskBefore = null; Paint.live = null; History.doUndo(); return; }
      if (cell) { Paint.live = null; cell = null; trails = null; History.doUndo(); }
    },
    eraseObjects(p, first) {
      if (!layerEditable(first)) return;
      const tol = Math.max(App.style.eraser.size / 2, docLen(4));
      const spots = Sym.all().map((T) => M.apply(T, p.x, p.y));
      const hits = currentObjects().filter((o) => !(Mask.current && Mask.current.ids.has(o.id)) && spots.some(([x, y]) => Mask.allows(x, y) && Render.hit(o, x, y, tol))).map((o) => o.id);
      if (first) this._erasing = true;
      if (!hits.length) return;
      if (!this._pushed || first) { History.push(); this._pushed = true; }
      const c = editableCell(); c.objects = c.objects.filter((o) => !hits.includes(o.id));
      App.state.selection = App.state.selection.filter((id) => !hits.includes(id));
      changed("erase");
    }
  };
}
tool("paint", paintTool(false));
tool("eraser", paintTool(true));

// ---- Fill bucket: fills a clicked vector shape, otherwise flood-fills pixels
tool("bucket", {
  down(p) {
    if (!layerEditable()) return;
    const hit = currentObjects().slice().reverse().find((o) => o.fill !== undefined && o.type !== "text" && o.type !== "image" && Render.hit(o, p.x, p.y, 0));
    if (hit && hit.type !== "brush") {
      History.push(); const c = editableCell(); const o = c.objects.find((x) => x.id === hit.id); o.fill = App.style.grad ? { ...App.style.grad } : App.style.paint.color; remember(o.fill); changed("fill"); return;
    }
    const d = App.doc, x0 = Math.floor(p.x), y0 = Math.floor(p.y);
    if (x0 < 0 || y0 < 0 || x0 >= d.width || y0 >= d.height) return;
    // sample what the layer looks like (pixels + shapes) so lines drawn with either tool act as borders
    const s = document.createElement("canvas"); s.width = d.width; s.height = d.height;
    const sg = s.getContext("2d", { willReadFrequently: true });
    const L = activeLayer();
    const r = Render.cellRasterAt(L, App.state.frame); if (r) sg.drawImage(r, 0, 0);
    for (const o of Render.cellObjectsAt(L, App.state.frame)) Render.drawObj(sg, o);
    const px = new Uint32Array(sg.getImageData(0, 0, d.width, d.height).data.buffer);
    const W = d.width, H = d.height, target = px[y0 * W + x0], tol = App.style.bucket.tolerance;
    const same = (c) => { if (c === target) return true; for (let s2 = 0; s2 < 32; s2 += 8) if (Math.abs(((c >>> s2) & 255) - ((target >>> s2) & 255)) > tol) return false; return true; };
    const mask = new Uint8Array(W * H), stack = [y0 * W + x0];
    while (stack.length) {
      const i = stack.pop(); if (mask[i]) continue;
      const y = (i / W) | 0; let x = i - y * W;
      while (x > 0 && !mask[y * W + x - 1] && same(px[y * W + x - 1])) x--;
      let up = false, dn = false;
      for (; x < W && !mask[y * W + x] && same(px[y * W + x]); x++) {
        mask[y * W + x] = 1;
        if (y > 0) { const u = !mask[(y - 1) * W + x] && same(px[(y - 1) * W + x]); if (u && !up) stack.push((y - 1) * W + x); up = u; }
        if (y < H - 1) { const v = !mask[(y + 1) * W + x] && same(px[(y + 1) * W + x]); if (v && !dn) stack.push((y + 1) * W + x); dn = v; }
      }
    }
    // grow the fill by 1px so it tucks under anti-aliased line edges
    const grown = new Uint8Array(mask);
    for (let y = 1; y < H - 1; y++) for (let x = 1; x < W - 1; x++) { const i = y * W + x; if (!mask[i] && (mask[i - 1] || mask[i + 1] || mask[i - W] || mask[i + W])) grown[i] = 1; }
    const [R, G, B, A] = Color.parse(App.style.paint.color);
    remember(App.style.paint.color);
    const fill = sg.createImageData(W, H);
    for (let i = 0; i < grown.length; i++) if (grown[i]) { fill.data[i * 4] = R; fill.data[i * 4 + 1] = G; fill.data[i * 4 + 2] = B; fill.data[i * 4 + 3] = A * 255; }
    History.push();
    const cell = editableCell(); const rc = rasterForPaint(cell);
    const tmp = document.createElement("canvas"); tmp.width = W; tmp.height = H; tmp.getContext("2d").putImageData(fill, 0, 0);
    const rg = rc.getContext("2d"); rg.globalCompositeOperation = "destination-over"; rg.drawImage(tmp, 0, 0); rg.globalCompositeOperation = "source-over";
    changed("bucket");
  }
});

// ---- Eyedropper
tool("eyedropper", {
  down(p) { this.pick(p); }, move(p) { this.pick(p); }, up() { if (this._last) remember(this._last); this._last = null; },
  pick(p) {
    const d = App.doc; if (p.x < 0 || p.y < 0 || p.x >= d.width || p.y >= d.height) return;
    const c = document.createElement("canvas"); c.width = c.height = 1;
    const g = c.getContext("2d"); g.translate(-Math.floor(p.x), -Math.floor(p.y)); Render.frame(g, App.state.frame);
    const [r, gg, b] = g.getImageData(0, 0, 1, 1).data;
    const hex = "#" + [r, gg, b].map((v) => v.toString(16).padStart(2, "0")).join("");
    App.style.fill = hex; App.style.grad = null; App.style.paint.color = hex;
    this._last = hex;
    App.emit("style");
  }
});
tool("hand", {});

// ---------------------------------------------------------------- object commands (menus & shortcuts)
const Cmd = (App.Cmd = {
  editSelected(fn) {
    if (!App.state.selection.length || !layerEditable()) return;
    History.push(); const ids = new Set(App.state.selection);
    for (const { cell } of selCells()) cell.objects.forEach((o, i) => { if (ids.has(o.id)) { const r = fn(o); if (r) cell.objects[i] = r; } });
    changed("edit");
  },
  delete() {
    if (!App.state.selection.length || !layerEditable()) return;
    History.push(); const ids = new Set(App.state.selection);
    for (const { cell } of selCells()) cell.objects = cell.objects.filter((o) => !ids.has(o.id));
    App.state.selection = []; changed("delete"); App.emit("selection");
  },
  /** Adds copies of the selection, each copy on the same layer as its original. make(selectedInCell) returns the copies. */
  copyInPlace(make) {
    const ids = new Set(App.state.selection), made = [];
    History.push();
    for (const { cell } of selCells()) { const copies = make(cell.objects.filter((o) => ids.has(o.id))); cell.objects.push(...copies); made.push(...copies); }
    App.state.selection = made.map((o) => o.id); return made;
  },
  clipboard: [],
  copy() { Cmd.clipboard = selectedObjects().map(cloneObj); if (Cmd.clipboard.length) toast(`Copied ${Cmd.clipboard.length} ${Cmd.clipboard.length === 1 ? "object" : "objects"}.`); },
  paste(inPlace = true) {
    if (!Cmd.clipboard.length || !layerEditable()) return;
    History.push(); const cell = editableCell();
    const copies = regroup(Cmd.clipboard.map((o) => ({ ...cloneObj(o), id: uid(), m: inPlace ? o.m.slice() : M.mul(M.translate(20, 20), o.m) })));
    cell.objects.push(...copies); App.state.selection = copies.map((o) => o.id); changed("paste"); App.emit("selection");
  },
  /** Simplify: fewer points, same shape. strength 0..1; repeated calls re-run from the original selection. */
  simplify(strength) {
    const sel = selectedObjects().filter(Geo.canSimplify);
    if (!sel.length) { toast("Select a path, pencil line, brush stroke or merged / cut shape to simplify."); return null; }
    const ids = sel.map((o) => o.id).sort().join(",");
    if (!Cmd._simp || Cmd._simp.ids !== ids) { History.push(); Cmd._simp = { ids, base: new Map(sel.map((o) => [o.id, cloneObj(o)])) }; }
    const err = 0.12 + strength * strength * 2.4;   // at most ~2.5 document px of drift
    let before = 0, after = 0;
    for (const { cell } of selCells()) cell.objects = cell.objects.map((o) => {
      const b = Cmd._simp.base.get(o.id); if (!b) return o;
      const r = strength > 0 ? Geo.simplify(cloneObj(b), err) : cloneObj(b);
      before += Geo.pointCount(b); after += Geo.pointCount(r);
      return r;
    });
    changed("simplify");
    return { before, after };
  },
  cut() {
    if (!App.state.selection.length) { toast("Select something to cut first."); return; }
    Cmd.copy(); Cmd.delete();
    if (Cmd.clipboard.length) toast(`Cut ${Cmd.clipboard.length} ${Cmd.clipboard.length === 1 ? "object" : "objects"} — paste with Ctrl+V or the Paste button.`);
  },
  duplicate() {
    if (!App.state.selection.length || !layerEditable()) return;
    Cmd.copyInPlace((sel) => regroup(sel.map((o) => ({ ...cloneObj(o), id: uid(), m: M.mul(M.translate(20, 20), o.m) }))));
    changed("duplicate"); App.emit("selection");
  },
  /**
   * Repeat copies: each copy i is moved by i·(dx, dy), turned by i·rotate and scaled by scale^i
   * around the pivot (selection centre or canvas centre); fade multiplies the opacity each step.
   */
  repeat({ count = 3, dx = 40, dy = 0, rotate = 0, scale = 100, fade = 100, pivot = "selection" } = {}) {
    const sel = selectedObjects(); if (!sel.length || !layerEditable()) return;
    const b = selBounds(sel), d = App.doc;
    const [px, py] = pivot === "canvas" ? [d.width / 2, d.height / 2] : [b.x + b.w / 2, b.y + b.h / 2];
    const made = Cmd.copyInPlace((sel) => {
      const out = [];
      for (let i = 1; i <= clamp(Math.round(count), 1, 500); i++) {
        const k = Math.pow(scale / 100, i);
        const T = M.mul(M.translate(dx * i, dy * i), M.mul(M.rotate((rotate * i * Math.PI) / 180, px, py), M.scale(k, k, px, py)));
        const step = regroup(sel.map((o) => ({ ...cloneObj(o), id: uid(), m: M.mul(T, o.m) })));
        step.forEach((c, j) => { if (fade !== 100) c.opacity = (sel[j].opacity ?? 1) * Math.pow(fade / 100, i); });
        out.push(...step);
      }
      return out;
    });
    changed("repeat"); App.emit("selection");
    toast(`Made ${made.length} ${made.length === 1 ? "copy" : "copies"}.`);
  },
  /** Mirrored copy: flipped across the selection's edge (so it sits beside it) or the canvas centre line. */
  mirrorCopy(horizontal, pivot = "edge") {
    const sel = selectedObjects(); if (!sel.length || !layerEditable()) return;
    const b = selBounds(sel), d = App.doc;
    const cx = pivot === "canvas" ? d.width / 2 : pivot === "centre" ? b.x + b.w / 2 : b.x + b.w;
    const cy = pivot === "canvas" ? d.height / 2 : pivot === "centre" ? b.y + b.h / 2 : b.y + b.h;
    const T = horizontal ? M.scale(-1, 1, cx, cy) : M.scale(1, -1, cx, cy);
    Cmd.copyInPlace((part) => regroup(part.map((o) => ({ ...cloneObj(o), id: uid(), m: M.mul(T, o.m) }))));
    changed("mirror"); App.emit("selection");
  },
  selectAll() { App.state.selection = selectableObjects().map((o) => o.id); App.emit("selection"); View.redraw(); },
  arrange(dir) {
    if (!App.state.selection.length || !layerEditable()) return;
    History.push(); const ids = new Set(App.state.selection);
    for (const { cell } of selCells()) {
      const sel = cell.objects.filter((o) => ids.has(o.id)), rest = cell.objects.filter((o) => !ids.has(o.id));
      if (dir === "front") cell.objects = [...rest, ...sel];
      else if (dir === "back") cell.objects = [...sel, ...rest];
      else {
        const a = cell.objects;
        if (dir === "forward") for (let i = a.length - 2; i >= 0; i--) { if (ids.has(a[i].id) && !ids.has(a[i + 1].id)) [a[i], a[i + 1]] = [a[i + 1], a[i]]; }
        else for (let i = 1; i < a.length; i++) { if (ids.has(a[i].id) && !ids.has(a[i - 1].id)) [a[i], a[i - 1]] = [a[i - 1], a[i]]; }
      }
    }
    changed("arrange");
  },
  flip(h) { const b = App.selBounds(); if (!b) return; App.transformSelection(M.scale(h ? -1 : 1, h ? 1 : -1, b.x + b.w / 2, b.y + b.h / 2)); },
  rotate(deg) { const b = App.selBounds(); if (!b) return; App.transformSelection(M.rotate(deg * Math.PI / 180, b.x + b.w / 2, b.y + b.h / 2)); },
  align(h, v) {
    const b = App.selBounds(); if (!b) return; const d = App.doc;
    const dx = h === "l" ? -b.x : h === "c" ? d.width / 2 - (b.x + b.w / 2) : h === "r" ? d.width - (b.x + b.w) : 0;
    const dy = v === "t" ? -b.y : v === "m" ? d.height / 2 - (b.y + b.h / 2) : v === "b" ? d.height - (b.y + b.h) : 0;
    App.transformSelection(M.translate(dx, dy));
  },
  nudge(dx, dy) { if (App.state.selection.length) App.transformSelection(M.translate(dx, dy)); },
  toPath() {
    const sel = selectedObjects();
    if (sel.length && !sel.some((o) => toPathObj(o))) { toast("Text and images can't become paths."); return; }
    Cmd.editSelected((o) => toPathObj(o) || undefined);
    App.emit("selection");
    if (sel.some((o) => o.type === "rings" || o.type === "brush")) toast("Converted to an editable path — use Direct selection (A) to move its points.");
  },
  addNode() {
    const t = registry.direct, o = selectedObjects()[0]; if (!o || o.type !== "path") return;
    Cmd.editSelected((po) => {
      const i = Math.max(0, t.activeNode), [s0, e0] = subRange(po.nodes, i), j = i < e0 ? i + 1 : s0; if (!po.closed && j === s0) return;
      const a = po.nodes[i], b = po.nodes[j], mid = (u, v) => (u + v) / 2;
      const q0x = mid(a.x, a.ox), q0y = mid(a.y, a.oy), q1x = mid(a.ox, b.ix), q1y = mid(a.oy, b.iy), q2x = mid(b.ix, b.x), q2y = mid(b.iy, b.y);
      const r0x = mid(q0x, q1x), r0y = mid(q0y, q1y), r1x = mid(q1x, q2x), r1y = mid(q1y, q2y);
      const n = { x: mid(r0x, r1x), y: mid(r0y, r1y), ix: r0x, iy: r0y, ox: r1x, oy: r1y, smooth: true };
      a.ox = q0x; a.oy = q0y; b.ix = q2x; b.iy = q2y;
      po.nodes.splice(i + 1, 0, n); t.activeNode = i + 1;
    });
  },
  deleteNode() {
    const t = registry.direct; const o = selectedObjects()[0]; if (!o || o.type !== "path" || t.activeNode < 0) return;
    Cmd.editSelected((po) => {
      const N = po.nodes, i = t.activeNode;
      if (N[i].start && N[i + 1] && !N[i + 1].start) N[i + 1].start = true;
      N.splice(i, 1); t.activeNode = -1;
    });
  },
  toggleClosed() { Cmd.editSelected((o) => { if (o.type === "path") o.closed = !o.closed; }); },
  group() {
    if (App.state.selection.length < 2) { toast("Select two or more objects to group."); return; }
    const g = "g" + uid(); Cmd.editSelected((o) => { o.group = g; });
    toast(`Grouped ${App.state.selection.length} objects.`); App.emit("selection");
  },
  ungroup() {
    if (!selectedObjects().some((o) => o.group)) { toast("Nothing grouped in the selection."); return; }
    Cmd.editSelected((o) => { delete o.group; }); toast("Ungrouped."); App.emit("selection");
  },
  /** Merges the selected shapes into one: unite, subtract (back minus front), intersect or exclude overlaps. */
  merge(op = "unite") {
    if (!layerEditable()) return;
    const sel = selectedObjects();
    if (sel.length < 2) { toast("Select two or more shapes to merge."); return; }
    if (selCells().length > 1) { toast("Merging works on shapes from one layer. Move them onto the same layer first."); return; }
    const shapes = sel.filter((o) => o.type !== "text" && o.type !== "image");
    if (shapes.length < 2) { toast("Text and images can't be merged — convert or remove them from the selection."); return; }
    if (typeof polygonClipping === "undefined") { toast("The shape library didn't load; check your connection and reload."); return; }
    let merged;
    try { merged = Geo.combine(shapes, op); } catch (e) { console.warn(e); toast("Those shapes couldn't be merged."); return; }
    History.push(); const cell = editableCell();
    const ids = new Set(shapes.map((o) => o.id));
    const top = Math.max(...cell.objects.map((o, i) => (ids.has(o.id) ? i : -1)));
    const out = [];
    cell.objects.forEach((o, i) => { if (!ids.has(o.id)) out.push(o); if (i === top && merged.polys.length) out.push(merged); });
    cell.objects = out;
    App.state.selection = merged.polys.length ? [merged.id] : [];
    if (!merged.polys.length) toast("Nothing was left after merging.");
    if (shapes.length < sel.length) toast("Text and images were left as they are.");
    changed("merge"); App.emit("selection");
  }
});
