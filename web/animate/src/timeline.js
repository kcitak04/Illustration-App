// ============================================================================
// timeline.js — playback, frame operations, and the layer × frame timeline
// ============================================================================
const Player = (App.Player = {
  dir: 1, raf: 0, last: 0, acc: 0,
  play() {
    if (App.state.playing) return;
    TextTool.cancel(); Tools.get("pen").finish();
    App.state.playing = true; this.dir = 1; this.last = performance.now(); this.acc = 0;
    Media.sync(App.state.frame, true);
    const step = (now) => {
      if (!App.state.playing) return;
      const d = App.doc, spf = 1000 / (d.fps * d.speed);
      this.acc += now - this.last; this.last = now;
      let moved = false;
      while (this.acc >= spf) {
        this.acc -= spf; moved = true;
        let f = App.state.frame + this.dir;
        if (f >= d.frames || f < 0) {
          if (d.pingpong) { this.dir *= -1; f = App.state.frame + this.dir; f = clamp(f, 0, d.frames - 1); }
          else if (d.loop) f = f < 0 ? d.frames - 1 : 0;
          else { this.stop(); return; }
        }
        App.state.frame = f;
      }
      if (moved) { Media.sync(App.state.frame, true); App.emit("frame"); View.redraw(); }
      this.raf = requestAnimationFrame(step);
    };
    this.raf = requestAnimationFrame(step);
    App.emit("playing");
  },
  stop() {
    App.state.playing = false; cancelAnimationFrame(this.raf); Media.pauseAll(); Media.sync(App.state.frame, false);
    App.emit("playing"); View.redraw();
  },
  toggle() { App.state.playing ? this.stop() : this.play(); }
});

App.goto = function (f) {
  const d = App.doc;
  f = d.loop ? ((f % d.frames) + d.frames) % d.frames : clamp(f, 0, d.frames - 1);
  if (f === App.state.frame) return;
  Tools.get("pen").finish?.();
  App.state.frame = f; App.state.selection = App.state.selection.filter((id) => currentObjects().some((o) => o.id === id));
  if (!App.state.playing) Media.sync(f, false);
  App.emit("frame"); View.redraw();
};

// ---------------------------------------------------------------- frame operations (per layer, like Animate)
const Frames = (App.Frames = {
  pad() {
    const d = App.doc;
    const len = Math.max(d.frames, ...d.layers.filter((L) => L.cells).map((L) => L.cells.length));
    for (const L of d.layers) if (L.cells) while (L.cells.length < len) L.cells.push(null);
    d.frames = len;
  },
  layer() { const L = activeLayer(); if (!L || L.kind !== "draw") { toast("Pick a drawing layer for frame edits."); return null; } return L; },
  /** New empty key on the next frame (inserting a frame when that spot is already used). */
  blankKey() {
    const L = this.layer(); if (!L) return; History.push();
    const f = App.state.frame, at = L.cells[f] || keyIndexAt(L, f) >= 0 ? f + 1 : f;
    if (at > f && at < L.cells.length && L.cells[at]) L.cells.splice(at, 0, newCell());
    else if (at >= L.cells.length) L.cells.push(newCell());
    else L.cells[at] = newCell();
    this.pad(); App.state.frame = at; App.state.selection = []; changed("frames"); App.emit("frame");
  },
  /** Copies what's on screen into a new key on the next frame (for drawing the next pose). */
  duplicateKey() {
    const L = this.layer(); if (!L) return; History.push();
    const f = App.state.frame;
    const cell = { key: true, objects: Render.cellObjectsAt(L, f).map(cloneObj), raster: cloneCanvas(Render.cellRasterAt(L, f)), tween: L.cells[keyIndexAt(L, f)]?.tween || null };
    const at = f + 1;
    if (at < L.cells.length && L.cells[at]) L.cells.splice(at, 0, cell);
    else if (at >= L.cells.length) L.cells.push(cell);
    else L.cells[at] = cell;
    this.pad(); App.state.frame = at; changed("frames"); App.emit("frame");
  },
  /** Holds the current drawing one frame longer. */
  insertHold() {
    const L = this.layer(); if (!L) return; History.push();
    L.cells.splice(App.state.frame + 1, 0, null); this.pad(); changed("frames"); App.emit("frame");
  },
  deleteFrame() {
    const L = this.layer(); if (!L) return; History.push();
    const f = App.state.frame, c = L.cells[f];
    if (c && f + 1 < L.cells.length && L.cells[f + 1] === null) { L.cells[f + 1] = c; L.cells.splice(f, 1); }
    else L.cells.splice(f, 1);
    L.cells.push(null);
    if (keyIndexAt(L, 0) < 0 && L.cells.every((x) => !x)) L.cells[0] = newCell();
    changed("frames"); App.emit("frame");
  },
  clearKey() {
    const L = this.layer(); if (!L) return; const f = App.state.frame; if (!L.cells[f]) return;
    History.push(); if (f === 0) L.cells[0] = newCell(); else L.cells[f] = null; App.state.selection = []; changed("frames"); App.emit("frame");
  },
  setTween(type) {
    const L = this.layer(); if (!L) return; const k = keyIndexAt(L, App.state.frame); if (k < 0) return;
    History.push(); L.cells[k].tween = type || null; changed("tween"); App.emit("frame");
    if (type && nextKeyIndex(L, k) < 0) toast("Tweens need a following key. Add one further along the timeline.");
  },
  /** Sets how many frames the current drawing stays on screen (1 = ones, 2 = twos…). */
  setExposure(n) {
    const L = this.layer(); if (!L) return; const k = keyIndexAt(L, App.state.frame); if (k < 0) return;
    History.push();
    let len = 1; while (k + len < L.cells.length && L.cells[k + len] === null) len++;
    const next = nextKeyIndex(L, k);
    if (next < 0) { // last key: just set the tail
      for (let i = k + 1; i < L.cells.length; i++) L.cells[i] = null;
      if (k + n > L.cells.length) { L.cells.length = k + n; L.cells.fill(null, k + 1); }
    } else if (n > len) L.cells.splice(k + len, 0, ...new Array(n - len).fill(null));
    else if (n < len) L.cells.splice(k + n, len - n);
    this.pad(); changed("frames"); App.emit("frame");
  },
  /** Re-times every key on the layer to the same exposure. */
  retimeAll(n) {
    const L = this.layer(); if (!L) return; History.push();
    const keys = L.cells.filter(Boolean);
    const cells = []; for (const k of keys) { cells.push(k); for (let i = 1; i < n; i++) cells.push(null); }
    L.cells = cells; this.pad(); changed("frames"); App.emit("frame");
  },
  clipboard: null,
  copyCell() { const L = this.layer(); if (!L) return; const f = App.state.frame; this.clipboard = { objects: Render.cellObjectsAt(L, f).map(cloneObj), raster: cloneCanvas(Render.cellRasterAt(L, f)) }; toast("Frame copied."); },
  pasteCell() {
    const L = this.layer(); if (!L || !this.clipboard) return; History.push();
    L.cells[App.state.frame] = { key: true, objects: this.clipboard.objects.map((o) => ({ ...cloneObj(o), id: uid() })), raster: cloneCanvas(this.clipboard.raster), tween: null };
    changed("frames"); App.emit("frame");
  },
  setLength(n) {
    const d = App.doc; n = clamp(Math.round(n), 1, 7200); History.push();
    for (const L of d.layers) if (L.cells) { if (L.cells.length > n) L.cells.length = n; while (L.cells.length < n) L.cells.push(null); if (!L.cells.some(Boolean)) L.cells[0] = newCell(); }
    d.frames = n; App.state.frame = Math.min(App.state.frame, n - 1); changed("frames"); App.emit("frame");
  }
});

// ---------------------------------------------------------------- layers
const Layers = (App.Layers = {
  add() {
    History.push(); const d = App.doc;
    const L = newDrawLayer("Layer " + (d.layers.length + 1), d.frames);
    d.layers.splice(App.state.layer + 1, 0, L); App.state.layer++; App.state.selection = [];
    changed("layers"); App.emit("layers");
  },
  remove() {
    const d = App.doc; if (d.layers.length <= 1) { toast("An animation needs at least one layer."); return; }
    History.push(); const L = d.layers[App.state.layer];
    if (L.media && L.media.el && L.media.el.pause) L.media.el.pause();
    d.layers.splice(App.state.layer, 1); App.state.layer = clamp(App.state.layer - 1, 0, d.layers.length - 1);
    App.state.selection = []; changed("layers"); App.emit("layers");
  },
  move(dir) {
    const d = App.doc, i = App.state.layer, j = i + dir; if (j < 0 || j >= d.layers.length) return;
    History.push(); [d.layers[i], d.layers[j]] = [d.layers[j], d.layers[i]]; App.state.layer = j; changed("layers"); App.emit("layers");
  },
  duplicate() {
    const d = App.doc, L = d.layers[App.state.layer]; History.push();
    let C;
    if (L.kind === "draw") C = { ...L, id: uid(), name: L.name + " copy", cells: L.cells.map((c) => (c ? { key: true, objects: c.objects.map(cloneObj), raster: cloneCanvas(c.raster), tween: c.tween } : null)) };
    else C = { ...L, id: uid(), name: L.name + " copy", settings: { ...L.settings } };
    d.layers.splice(App.state.layer + 1, 0, C); App.state.layer++; changed("layers"); App.emit("layers");
  },
  // ---- layer masks: a greyscale-free alpha canvas per layer (opaque = shown)
  addMask(fromSelection) {
    const d = App.doc, L = d.layers[App.state.layer]; if (!L) return;
    let area = null;
    if (fromSelection) { area = App.Mask.selectionArea(); if (!area) { toast("Select the shapes to use as the mask first."); return; } }
    History.push();
    const c = document.createElement("canvas"); c.width = d.width; c.height = d.height;
    const g = c.getContext("2d"); g.fillStyle = "#fff";
    if (area) { const p = new Path2D(); for (const P of area) for (const r of P) { r.forEach(([x, y], k) => (k ? p.lineTo(x, y) : p.moveTo(x, y))); p.closePath(); } g.fill(p, "evenodd"); }
    else g.fillRect(0, 0, d.width, d.height);
    L.mask = { canvas: c, on: true }; L.maskEdit = true;
    changed("mask"); App.emit("layers");
    toast(area ? "Layer mask made from the selection. Paint brush hides, eraser reveals." : "Layer mask added. Paint brush hides parts of the layer, eraser brings them back.");
  },
  maskOp(op) {
    const d = App.doc, L = d.layers[App.state.layer]; if (!L || !L.mask) return;
    History.push();
    if (op === "delete") { delete L.mask; L.maskEdit = false; }
    else if (op === "toggle") L.mask = { ...L.mask, on: L.mask.on === false };
    else {
      const c = document.createElement("canvas"); c.width = d.width; c.height = d.height; const g = c.getContext("2d");
      if (op === "invert") { g.fillStyle = "#fff"; g.fillRect(0, 0, c.width, c.height); g.globalCompositeOperation = "destination-out"; g.drawImage(L.mask.canvas, 0, 0); }
      else if (op === "reveal") { g.fillStyle = "#fff"; g.fillRect(0, 0, c.width, c.height); }
      // "hide" leaves it empty
      L.mask = { ...L.mask, canvas: c };
    }
    changed("mask"); App.emit("layers");
  },
  editMask(on) { const L = App.doc.layers[App.state.layer]; if (!L || !L.mask) return; L.maskEdit = on; changed("mask"); App.emit("layers"); },
  // ---- layer groups (linked layers: their shapes are selected and moved together)
  setGroup(indices) {
    const d = App.doc, L = d.layers[App.state.layer]; if (!L) return;
    History.push();
    let gid = L.lgroup;
    if (!gid) { const used = new Set(d.layers.map((x) => x.lgroup).filter(Boolean)); gid = "ABCDEFGHIJKLMNOPQRSTUVWXYZ".split("").find((c) => !used.has(c)) || "g" + uid(); }
    d.layers.forEach((M, i) => { if (indices.includes(i)) M.lgroup = gid; else if (M.lgroup === gid) delete M.lgroup; });
    const n = d.layers.filter((M) => M.lgroup === gid).length;
    if (n < 2) d.layers.forEach((M) => { if (M.lgroup === gid) delete M.lgroup; });
    changed("layers"); App.emit("layers");
    toast(n < 2 ? "A layer group needs at least two layers." : `Layer group ${gid}: ${n} layers linked. Their shapes are selected and moved together.`);
  },
  leaveGroup() {
    const d = App.doc, L = d.layers[App.state.layer]; if (!L || !L.lgroup) return;
    History.push(); const gid = L.lgroup; delete L.lgroup;
    if (d.layers.filter((M) => M.lgroup === gid).length < 2) d.layers.forEach((M) => { if (M.lgroup === gid) delete M.lgroup; });
    App.state.selection = []; changed("layers"); App.emit("layers"); App.emit("selection");
  },
  toggleClip() {
    const L = App.doc.layers[App.state.layer]; if (!L) return;
    if (App.state.layer === 0 && !L.clip) { toast("The bottom layer has nothing below it to clip to."); return; }
    History.push(); L.clip = !L.clip; changed("clip"); App.emit("layers");
  },
  select(i) { if (i === App.state.layer) return; TextTool.cancel(); Tools.get("pen").finish?.(); App.state.layer = i; App.state.selection = []; App.emit("layers"); App.emit("selection"); View.redraw(); }
});

// ---------------------------------------------------------------- canvas size
/**
 * Changes the animation's size at any time. fit: "scale" (keep proportions, fit inside), "fill"
 * (keep proportions, fill and crop), "stretch", or "none" (keep artwork size; crop / add space
 * around it from the anchor, ax / ay = 0 left/top, 0.5 centre, 1 right/bottom).
 */
App.resizeCanvas = function (nw, nh, fit = "scale", ax = 0.5, ay = 0.5) {
  const d = App.doc, W = d.width, H = d.height;
  nw = clamp(Math.round(nw), 16, 4096); nh = clamp(Math.round(nh), 16, 4096);
  if (nw === W && nh === H) return;
  let T;
  if (fit === "stretch") T = M.scale(nw / W, nh / H);
  else if (fit === "scale" || fit === "fill") {
    const u = fit === "scale" ? Math.min(nw / W, nh / H) : Math.max(nw / W, nh / H);
    T = M.mul(M.translate((nw - W * u) / 2, (nh - H * u) / 2), M.scale(u, u));
  } else T = M.translate((nw - W) * ax, (nh - H) * ay);
  const ux = Math.hypot(T[0], T[1]), uy = Math.hypot(T[2], T[3]);
  History.push();
  const remap = (c, fillNew) => {
    const n = document.createElement("canvas"); n.width = nw; n.height = nh;
    const g = n.getContext("2d"); g.imageSmoothingQuality = "high";
    if (fillNew) { g.fillStyle = "#fff"; g.fillRect(0, 0, nw, nh); g.setTransform(...T); g.clearRect(0, 0, W, H); }
    g.setTransform(...T); g.drawImage(c, 0, 0); return n;
  };
  for (const L of d.layers) {
    if (L.cells) for (const c of L.cells) if (c) {
      if (c.raster) c.raster = remap(c.raster);
      for (const o of c.objects) o.m = M.mul(T, o.m);
    }
    // layer masks: newly added space stays visible
    if (L.mask && L.mask.canvas) L.mask = { ...L.mask, canvas: remap(L.mask.canvas, true) };
    if (L.kind === "media" && L.settings && L.media) {
      const s = L.settings;
      if (!(fit !== "none" && ["contain", "cover", "stretch"].includes(s.fit))) {
        // keep the clip where it was relative to the artwork
        const sw = L.media.w || 1, sh = L.media.h || 1;
        const k = s.fit === "contain" ? Math.min(W / sw, H / sh) : s.fit === "cover" ? Math.max(W / sw, H / sh) : s.fit === "stretch" ? Math.sqrt((W / sw) * (H / sh)) : 1;
        const [cx, cy] = M.apply(T, W / 2 + s.x, H / 2 + s.y);
        s.scale = s.scale * k * Math.sqrt(ux * uy); s.fit = "none"; s.x = cx - nw / 2; s.y = cy - nh / 2;
      } else { s.x *= ux; s.y *= uy; }
    }
  }
  d.width = nw; d.height = nh;
  const sy = App.style.sym; sy.cx = sy.cy = null;
  if (App.Mask) App.Mask.current = null;
  App.state.selection = [];
  changed("doc"); App.emit("layers"); App.emit("selection"); App.View.fit(); App.UI.syncTimelineControls();
};

// ---------------------------------------------------------------- timeline UI
const TL = (App.TL = { cw: 16, rh: 30, ruler: 22 });
TL.init = function () {
  const scroller = $("tlScroll"), cvs = $("tlCanvas");
  const redraw = () => TL.draw();
  scroller.addEventListener("scroll", () => { $("tlHeads").scrollTop = scroller.scrollTop; redraw(); });
  new ResizeObserver(redraw).observe(scroller);
  App.on("changed", () => { TL.buildHeads(); redraw(); });
  App.on("frame", () => { TL.follow(); redraw(); TL.updateReadout(); });
  App.on("layers", () => { TL.buildHeads(); redraw(); });
  let drag = null;
  cvs.addEventListener("pointerdown", (e) => {
    cvs.setPointerCapture(e.pointerId);
    const { f, row, y } = TL.hit(e);
    if (y < TL.ruler) { drag = { kind: "scrub" }; App.goto(f); return; }
    if (row >= 0 && row < App.doc.layers.length) {
      const L = App.doc.layers[TL.rowToLayer(row)];
      if (L.kind === "media" && f >= L.settings.start && f < L.settings.start + Math.min(Media.lengthFrames(L), App.doc.frames)) {
        drag = { kind: "clip", L, f0: f, start: L.settings.start, pushed: false };
      } else drag = { kind: "scrub" };
      Layers.select(TL.rowToLayer(row));
    }
    App.goto(f);
    if (e.button === 2) { TL.menu(e); drag = null; }
  });
  cvs.addEventListener("pointermove", (e) => {
    if (!drag) return; const { f } = TL.hit(e);
    if (drag.kind === "scrub") App.goto(f);
    else if (drag.kind === "clip") {
      const ns = Math.max(0, drag.start + f - drag.f0);
      if (ns !== drag.L.settings.start) { if (!drag.pushed) { History.push(); drag.pushed = true; } drag.L.settings.start = ns; changed("clip"); }
    }
  });
  cvs.addEventListener("pointerup", () => { drag = null; });
  cvs.addEventListener("contextmenu", (e) => { e.preventDefault(); TL.menu(e); });
  let press = 0;
  cvs.addEventListener("touchstart", (e) => { press = setTimeout(() => TL.menu(e.touches[0]), 550); }, { passive: true });
  cvs.addEventListener("touchend", () => clearTimeout(press));
  cvs.addEventListener("touchmove", () => clearTimeout(press), { passive: true });
  TL.buildHeads(); redraw(); TL.updateReadout();
};
// rows are drawn top = top-most layer
TL.rowToLayer = (row) => App.doc.layers.length - 1 - row;
TL.hit = function (e) {
  const r = $("tlCanvas").getBoundingClientRect();
  const x = e.clientX - r.left + $("tlScroll").scrollLeft, y = e.clientY - r.top;
  return { f: clamp(Math.floor(x / TL.cw), 0, App.doc.frames - 1), row: Math.floor((y - TL.ruler + $("tlScroll").scrollTop) / TL.rh), y };
};
TL.follow = function () {
  const sc = $("tlScroll"), x = App.state.frame * TL.cw;
  if (x < sc.scrollLeft + 20 || x > sc.scrollLeft + sc.clientWidth - 40) sc.scrollLeft = Math.max(0, x - sc.clientWidth / 2);
};
TL.updateReadout = function () {
  const d = App.doc, f = App.state.frame;
  $("frameNo").textContent = `${f + 1} / ${d.frames}`;
  const t = f / d.fps, total = d.frames / d.fps / d.speed;
  $("frameTime").textContent = `${(t / d.speed).toFixed(2)}s of ${total.toFixed(2)}s`;
  const L = activeLayer();
  if (L && L.kind === "draw") {
    const k = keyIndexAt(L, f); let len = 1; if (k >= 0) while (k + len < L.cells.length && L.cells[k + len] === null) len++;
    $("tweenSel").value = k >= 0 ? (L.cells[k].tween || "") : "";
    $("exposureOut").textContent = k >= 0 ? `held ${len} ${len === 1 ? "frame" : "frames"}` : "empty";
  } else { $("exposureOut").textContent = L ? "media layer" : ""; }
};

TL.buildHeads = function () {
  const box = $("tlHeads"); box.textContent = "";
  const pad = document.createElement("div"); pad.className = "tl-ruler-pad"; pad.textContent = "LAYERS"; box.append(pad);
  const d = App.doc;
  for (let row = 0; row < d.layers.length; row++) {
    const i = TL.rowToLayer(row), L = d.layers[i];
    const el = document.createElement("div"); el.className = "tl-head" + (i === App.state.layer ? " active" : "");
    const eye = document.createElement("button"); eye.className = "ico"; eye.title = L.visible ? "Hide layer" : "Show layer"; eye.innerHTML = L.visible ? ICON.eye : ICON.eyeOff;
    eye.onclick = (e) => { e.stopPropagation(); History.push(); L.visible = !L.visible; changed("layers"); };
    const lock = document.createElement("button"); lock.className = "ico"; lock.title = L.locked ? "Unlock layer" : "Lock layer"; lock.innerHTML = L.locked ? ICON.lock : ICON.unlock;
    lock.onclick = (e) => { e.stopPropagation(); History.push(); L.locked = !L.locked; changed("layers"); };
    const name = document.createElement("span"); name.className = "tl-name";
    name.textContent = (L.lgroup ? `⛓${L.lgroup.length === 1 ? L.lgroup : ""} ` : "") + (L.clip ? "↳ " : "") + (L.kind === "media" ? (L.media.type === "audio" ? "♪ " : "▶ ") : "") + L.name + (L.mask ? (L.maskEdit ? " ◐ (editing mask)" : " ◐") : "");
    if (L.clip) name.title = "Clipped to the layer below";
    name.title = "Double-click to rename";
    name.ondblclick = () => {
      const inp = document.createElement("input"); inp.value = L.name; inp.className = "tl-rename"; name.replaceWith(inp); inp.focus(); inp.select();
      const done = () => { if (inp.value.trim() && inp.value !== L.name) { History.push(); L.name = inp.value.trim().slice(0, 40); } changed("layers"); };
      inp.onblur = done; inp.onkeydown = (ev) => { if (ev.key === "Enter") inp.blur(); if (ev.key === "Escape") { inp.value = L.name; inp.blur(); } };
    };
    el.onclick = () => Layers.select(i);
    el.append(eye, lock, name);
    box.append(el);
  }
};

TL.draw = function () {
  const sc = $("tlScroll"), cvs = $("tlCanvas"), d = App.doc;
  $("tlSpacer").style.width = d.frames * TL.cw + "px";
  $("tlSpacer").style.height = TL.ruler + d.layers.length * TL.rh + "px";
  const W = sc.clientWidth, H = Math.max(sc.clientHeight, TL.ruler + d.layers.length * TL.rh);
  const dpr = window.devicePixelRatio || 1;
  cvs.style.width = W + "px"; cvs.style.height = sc.clientHeight + "px";
  if (cvs.width !== Math.round(W * dpr) || cvs.height !== Math.round(sc.clientHeight * dpr)) { cvs.width = Math.round(W * dpr); cvs.height = Math.round(sc.clientHeight * dpr); }
  const g = cvs.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0);
  const css = getComputedStyle(document.documentElement), v = (n) => css.getPropertyValue(n).trim();
  const ox = sc.scrollLeft, oy = sc.scrollTop, cw = TL.cw;
  g.fillStyle = v("--panel"); g.fillRect(0, 0, W, H);
  const f0 = Math.floor(ox / cw), f1 = Math.min(d.frames, Math.ceil((ox + W) / cw) + 1);
  // rows
  for (let row = 0; row < d.layers.length; row++) {
    const i = TL.rowToLayer(row), L = d.layers[i], y = TL.ruler + row * TL.rh - oy;
    if (y > H || y + TL.rh < TL.ruler) continue;
    g.fillStyle = i === App.state.layer ? v("--row-active") : row % 2 ? v("--row-alt") : v("--panel");
    g.fillRect(0, y, W, TL.rh);
    for (let f = f0; f < f1; f++) {
      const x = f * cw - ox;
      g.fillStyle = f % 5 === 4 ? v("--cell-5") : v("--cell");
      g.fillRect(x + 1, y + 3, cw - 1, TL.rh - 6);
    }
    if (L.kind === "draw") {
      for (let f = f0; f < f1; f++) {
        const x = f * cw - ox, c = L.cells[f];
        const k = keyIndexAt(L, f);
        if (k < 0) continue;
        const key = L.cells[k];
        const empty = !key.objects.length && !key.raster;
        const next = nextKeyIndex(L, k);
        if (key.tween && next > 0 && f < next) { g.fillStyle = v("--tween"); g.fillRect(x, y + 5, cw, TL.rh - 10); }
        else if (!c) { g.fillStyle = v("--hold"); g.fillRect(x, y + 9, cw, TL.rh - 18); }
        if (c) {
          g.fillStyle = v("--key-bg"); g.fillRect(x + 1, y + 3, cw - 1, TL.rh - 6);
          g.beginPath(); g.arc(x + cw / 2, y + TL.rh / 2, 4, 0, 7);
          if (empty) { g.strokeStyle = v("--ink"); g.lineWidth = 1.5; g.stroke(); } else { g.fillStyle = v("--ink"); g.fill(); }
        }
      }
    } else {
      const s = L.settings, len = Media.lengthFrames(L), end = Math.min(d.frames, s.start + (isFinite(len) ? len : d.frames));
      const x0 = s.start * cw - ox, x1 = end * cw - ox;
      g.fillStyle = L.media.type === "audio" ? v("--clip-audio") : v("--clip"); g.fillRect(x0, y + 4, x1 - x0, TL.rh - 8);
      g.fillStyle = "#fff"; g.font = "11px " + v("--font-ui"); g.textBaseline = "middle";
      g.save(); g.beginPath(); g.rect(x0, y, x1 - x0, TL.rh); g.clip(); g.fillText(L.name, Math.max(x0, 0) + 6, y + TL.rh / 2); g.restore();
    }
  }
  // ruler
  g.fillStyle = v("--panel-2"); g.fillRect(0, 0, W, TL.ruler);
  g.fillStyle = v("--muted"); g.font = "10px " + v("--font-mono"); g.textBaseline = "middle";
  const every = cw >= 14 ? 5 : 10;
  for (let f = f0; f < f1; f++) {
    const x = f * cw - ox;
    if ((f + 1) % every === 0 || f === 0) { g.fillText(String(f + 1), x + 2, TL.ruler / 2); g.fillRect(x + cw - 0.5, TL.ruler - 5, 1, 5); }
    if (f % d.fps === 0 && f) { g.fillStyle = v("--rule"); g.fillRect(x, TL.ruler, 1, H); g.fillStyle = v("--muted"); }
  }
  // playhead
  const px = App.state.frame * cw - ox;
  g.fillStyle = v("--accent-soft"); g.fillRect(px, 0, cw, H);
  g.fillStyle = v("--accent"); g.fillRect(px + cw / 2 - 1, 0, 2, H);
  g.beginPath(); g.moveTo(px, 0); g.lineTo(px + cw, 0); g.lineTo(px + cw / 2, 8); g.fill();
};

TL.menu = function (e) {
  const L = activeLayer();
  const items = L && L.kind === "draw" ? [
    ["New blank key", () => Frames.blankKey()], ["Duplicate key", () => Frames.duplicateKey()], ["Hold one frame longer", () => Frames.insertHold()],
    ["Delete frame", () => Frames.deleteFrame()], ["Clear key", () => Frames.clearKey()], ["-"],
    ["Copy frame", () => Frames.copyCell()], ["Paste frame", () => Frames.pasteCell()], ["-"],
    ["Tween: none", () => Frames.setTween(null)], ["Tween: linear", () => Frames.setTween("linear")], ["Tween: ease in-out", () => Frames.setTween("easeInOut")]
  ] : [["Clip settings are in the panel on the right", () => App.UI.openProps()], ["Bake into drawable frames", () => App.UI.rotoscope()]];
  App.UI.popup(e.clientX, e.clientY, items);
};
