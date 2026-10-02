// ============================================================================
// ui.js — toolbar, properties panel, media clip settings, import, dialogs, keys
// ============================================================================
const ICON = {
  select: '<path d="M6 3v16l4-4 3 6 2.5-1-3-6h5.5z" fill="currentColor" stroke="none"/>',
  direct: '<path d="M6 3v16l4-4 3 6 2.5-1-3-6h5.5z"/>',
  pen: '<path d="M12 3l6 10-3 7H9l-3-7z"/><path d="M12 3v8"/><circle cx="12" cy="12.5" r="1.4" fill="currentColor"/>',
  pencil: '<path d="M4 20l1-5L16 4l4 4L9 19z"/><path d="M14 6l4 4"/>',
  vbrush: '<path d="M20 3l-9 10"/><path d="M10 13c-4 0-3 5-6 7 5 1 9-1 8.5-5z" fill="currentColor"/>',
  paint: '<path d="M18 3l3 3-9 9-3-3z"/><path d="M9 12c-3 0-4 2-4 4s-1 3-2 4c3 0 7 0 8-3"/>',
  eraser: '<path d="M8 20l-4.5-4.5L14 5l6.5 6.5L12 20z"/><path d="M8.5 10.5L15 17"/><path d="M8 20h12"/>',
  bucket: '<path d="M11 3l8 8-8 8-8-8z"/><path d="M3 11h16"/><path d="M20 14c2 3 2 5 0 5s-2-2 0-5z" fill="currentColor"/>',
  rect: '<rect x="4" y="6" width="16" height="12"/>',
  ellipse: '<ellipse cx="12" cy="12" rx="8.5" ry="6.5"/>',
  poly: '<path d="M12 3.5l2.6 5.3 5.8.8-4.2 4.1 1 5.8-5.2-2.7-5.2 2.7 1-5.8-4.2-4.1 5.8-.8z"/>',
  line: '<path d="M5 19L19 5"/>',
  text: '<path d="M5 5h14M12 5v15M9 20h6"/>',
  eyedropper: '<path d="M4 20l9-9"/><path d="M11 9l4 4"/><circle cx="17.5" cy="6.5" r="3.5" fill="currentColor"/>',
  hand: '<path d="M8 13V6M11 11V4.5M14 11V5M17 12V7"/><path d="M17 12v3c0 4-3 6-6 6s-5-3-6.5-7l3.5-1"/>',
  eye: '<path d="M2 12c4-7 16-7 20 0-4 7-16 7-20 0z"/><circle cx="12" cy="12" r="2.8" fill="currentColor"/>',
  eyeOff: '<path d="M2 12c4-7 16-7 20 0-4 7-16 7-20 0z"/><path d="M4 4l16 16"/>',
  lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 018 0v3"/>',
  mask: '<rect x="3" y="3" width="18" height="18" rx="2" fill="currentColor" opacity=".35"/><circle cx="12" cy="12" r="5.5" fill="var(--panel, #fff)" stroke="currentColor"/>',
  sym: '<path d="M12 2v20" stroke-dasharray="2 2.2"/><path d="M9 6L3 12l6 6z"/><path d="M15 6l6 6-6 6z" fill="currentColor"/>',
  unlock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 018-1"/>'
};
for (const k of Object.keys(ICON)) ICON[k] = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICON[k]}</svg>`;

const TOOLS = [
  ["select", "Select & transform (V)", "v"], ["direct", "Direct selection: edit anchor points (A)", "a"],
  ["pen", "Pen: click for corners, drag for curves (P)", "p"], ["pencil", "Pencil: freehand vector path (N)", "n"],
  ["vbrush", "Vector brush with pressure (B)", "b"], ["paint", "Paint brush: pixels (K)", "k"],
  ["eraser", "Eraser (E)", "e"], ["bucket", "Fill bucket (G)", "g"],
  ["rect", "Rectangle (M)", "m"], ["ellipse", "Ellipse (L)", "l"], ["poly", "Polygon / star (O)", "o"], ["line", "Line (\\)", "\\"],
  ["text", "Type (T)", "t"], ["eyedropper", "Eyedropper (I)", "i"], ["hand", "Hand: pan the canvas (H)", "h"]
];
const BLENDS = [["source-over", "Normal"], ["multiply", "Multiply"], ["screen", "Screen"], ["overlay", "Overlay"], ["darken", "Darken"], ["lighten", "Lighten"], ["color-dodge", "Color dodge"], ["color-burn", "Color burn"], ["hard-light", "Hard light"], ["soft-light", "Soft light"], ["difference", "Difference"], ["exclusion", "Exclusion"], ["hue", "Hue"], ["saturation", "Saturation"], ["color", "Color"], ["luminosity", "Luminosity"]];
const FONTS = ["Anton", "Bebas Neue", "Inter Tight", "Playfair Display", "Caveat", "Permanent Marker", "Space Mono", "Arial", "Georgia", "Courier New"];

App.settings = { palm: true };
const UI = (App.UI = {});

// ---------------------------------------------------------------- small DOM builders
const h = (tag, attrs = {}, ...kids) => {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") el.className = v; else if (k === "html") el.innerHTML = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v); else if (v !== undefined && v !== null && v !== false) el.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) if (c !== null && c !== undefined && c !== false) el.append(c.nodeType ? c : document.createTextNode(c));
  return el;
};
/** Undo-friendly live control: records one history step per drag / edit. */
function live(el, apply, { record = true } = {}) {
  let armed = true;
  el.addEventListener("input", () => { if (armed && record) { History.push(); armed = false; } apply(); });
  el.addEventListener("change", () => { if (armed && record) { History.push(); } apply(); armed = true; });
  return el;
}
let ctlId = 0;
function slider(label, min, max, step, get, set, { fmt = (v) => v, record = true, unit = "" } = {}) {
  const id = "c" + (++ctlId);
  const out = h("output", { for: id }, fmt(get()) + unit);
  const inp = h("input", { type: "range", id, min, max, step, value: get() });
  live(inp, () => { set(parseFloat(inp.value)); out.textContent = fmt(parseFloat(inp.value)) + unit; }, { record });
  return h("div", { class: "row slider" }, h("label", { for: id }, label), inp, out);
}
function numberField(label, get, set, { min, max, step = 1, record = true } = {}) {
  const id = "c" + (++ctlId);
  const inp = h("input", { type: "number", id, value: get(), min, max, step });
  inp.addEventListener("change", () => { const v = parseFloat(inp.value); if (!isFinite(v)) return; if (record) History.push(); set(clamp(v, min ?? -Infinity, max ?? Infinity)); });
  return h("div", { class: "row" }, h("label", { for: id }, label), inp);
}
function select(label, options, get, set, { record = true } = {}) {
  const id = "c" + (++ctlId);
  const sel = h("select", { id }, options.map(([v, t]) => h("option", { value: v, selected: String(get()) === String(v) }, t)));
  sel.addEventListener("change", () => { if (record) History.push(); set(sel.value); });
  return h("div", { class: "row" }, h("label", { for: id }, label), sel);
}
function check(label, get, set, { record = true } = {}) {
  const id = "c" + (++ctlId);
  const inp = h("input", { type: "checkbox", id, checked: get() });
  inp.addEventListener("change", () => { if (record) History.push(); set(inp.checked); });
  return h("label", { class: "check", for: id }, inp, label);
}
function btn(text, onclick, cls = "") { return h("button", { class: "btn " + cls, type: "button", onclick }, text); }
function section(title, ...kids) { return h("section", { class: "psec" }, h("h3", {}, title), ...kids); }
function colorRow(label, get, set, { allowNone = false, record = true } = {}) {
  const id = "c" + (++ctlId);
  const cur = get();
  const inp = h("input", { type: "color", id, value: cur ? Color.hex(cur) : "#000000" });
  const a = h("input", { type: "range", min: 0, max: 1, step: 0.01, value: cur ? Color.alpha(cur) : 1, "aria-label": label + " opacity", class: "alpha" });
  const apply = () => set(Color.withAlpha(inp.value, parseFloat(a.value)));
  live(inp, apply, { record }); live(a, apply, { record });
  const row = h("div", { class: "row color" }, h("label", { for: id }, label), inp, a);
  if (allowNone) {
    const none = h("button", { class: "btn tiny" + (cur ? "" : " on"), type: "button", title: "No " + label.toLowerCase(), onclick: () => { if (record) History.push(); set(null); UI.buildProps(); } }, "None");
    row.append(none);
  }
  return row;
}

// ---------------------------------------------------------------- toolbar
UI.buildToolbar = function () {
  const bar = $("toolbar"); bar.textContent = "";
  for (const [name, title] of TOOLS) {
    const b = h("button", { class: "tool" + (App.state.tool === name ? " on" : ""), type: "button", title, "aria-label": title, html: ICON[name], onclick: () => App.setTool(name) });
    b.dataset.tool = name; bar.append(b);
  }
  const sw = h("div", { class: "swatches", title: "Fill / paint colour and stroke colour" },
    h("span", { class: "sw fill", style: `background:${typeof App.style.fill === "string" ? App.style.fill : "#888"}` }),
    h("span", { class: "sw stroke", style: `border-color:${App.style.strokeOn ? App.style.stroke : "transparent"}` }));
  sw.onclick = () => UI.showTab("colour");
  const sb = h("button", { class: "tool symbtn", type: "button", "aria-label": "Symmetry", html: ICON.sym, onclick: (e) => { const r = e.currentTarget.getBoundingClientRect(); window.innerWidth <= 900 ? UI.symmetryMenu(r.left, r.bottom + 6) : UI.symmetryMenu(r.right + 6, r.top); } });
  const mb = h("button", { class: "tool maskbtn", type: "button", "aria-label": "Mask", html: ICON.mask, onclick: (e) => { const r = e.currentTarget.getBoundingClientRect(); window.innerWidth <= 900 ? UI.maskMenu(r.left, r.bottom + 6) : UI.maskMenu(r.right + 6, r.top); } });
  bar.append(sb, mb, sw);
  UI.syncMaskButton();
  UI.syncSymButton();
};
App.setTool = function (name) {
  if (App.state.tool === "pen" && name !== "pen") Tools.get("pen").finish();
  TextTool.commit();
  App.state.tool = name;
  document.querySelectorAll(".tool").forEach((b) => b.classList.toggle("on", b.dataset.tool === name));
  $("stage").style.cursor = name === "hand" ? "grab" : ["select", "direct"].includes(name) ? "default" : "crosshair";
  UI.buildProps(); View.redraw();
};

// ---------------------------------------------------------------- properties panel
UI.openProps = () => { document.body.classList.add("props-open"); };
UI.closeProps = () => { document.body.classList.remove("props-open"); };
UI.buildProps = function () {
  const box = $("propsBody"); if (!box) return;
  box.textContent = "";
  const t = App.state.tool, s = App.style, L = activeLayer();
  const sel = selectedObjects();
  const editSel = sel.length && ["select", "direct"].includes(t);

  // imported clips: their settings come first
  if (L && L.kind === "media") { box.append(...UI.mediaSections(L)); }

  // --- colour
  const applyFill = (v) => {
    s.grad = null; s.fill = v; if (typeof v === "string") s.paint.color = v;
    if (editSel) { const cell = App.selCell(); const ids = new Set(App.state.selection); cell.objects.forEach((o) => { if (ids.has(o.id)) o.fill = v; }); changed("fill"); }
    UI.syncSwatch();
  };
  const applyStroke = (v) => { s.strokeOn = !!v; if (v) s.stroke = v; if (editSel) { const cell = App.selCell(); const ids = new Set(App.state.selection); cell.objects.forEach((o) => { if (ids.has(o.id)) { o.stroke = v; if (v && !o.sw) o.sw = s.sw; } }); changed("stroke"); } UI.syncSwatch(); };
  const first = editSel ? sel[0] : null;
  const fillNow = first ? (typeof first.fill === "string" ? first.fill : null) : (s.grad ? null : s.fill);
  UI.buildColour({ t, s, editSel, first, fillNow, applyFill, applyStroke });
  const dot = (c, label) => h("span", { class: "dot", title: label, style: c ? `background:${typeof c === "string" ? c : `linear-gradient(90deg, ${c.c1}, ${c.c2})`}` : "background: repeating-linear-gradient(45deg, transparent 0 4px, var(--rule) 4px 6px)" });
  const isPaint = t === "paint" || t === "bucket";
  box.append(h("section", { class: "psec" }, h("div", { class: "cur-colours" },
    dot(isPaint ? s.paint.color : first ? first.fill : s.grad || s.fill, isPaint ? "Paint colour" : "Fill"), dot(first ? first.stroke : s.strokeOn ? s.stroke : null, "Stroke"),
    h("span", {}, "Colours are in the"), btn("Colour tab", () => UI.showTab("colour"), "tiny"))));
  const colorSec = section("Colour",
    colorRow(t === "paint" || t === "bucket" ? "Paint" : "Fill", () => (t === "paint" || t === "bucket" ? s.paint.color : fillNow), (v) => { if (t === "paint" || t === "bucket") { s.paint.color = v || s.paint.color; s.fill = v || s.fill; UI.syncSwatch(); } else applyFill(v); }, { allowNone: !["paint", "bucket", "vbrush"].includes(t), record: !!editSel }),
    colorRow("Stroke", () => (first ? first.stroke : s.strokeOn ? s.stroke : null), applyStroke, { allowNone: true, record: !!editSel }),
    slider("Stroke width", 0, 80, 0.5, () => (first ? first.sw || 0 : s.sw), (v) => { s.sw = v; if (editSel) { const cell = App.selCell(); const ids = new Set(App.state.selection); cell.objects.forEach((o) => { if (ids.has(o.id)) o.sw = v; }); changed("sw"); } }, { record: !!editSel }),
    h("div", { class: "row" }, btn("Gradient…", () => UI.gradientDialog()), s.grad ? h("span", { class: "hint" }, `${s.grad.type} gradient on`) : null),
    UI.recentSwatches()
  );
  void colorSec;   // built for the Colour tab instead

  // --- tool options
  const opts = [];
  if (t === "vbrush") {
    const st = s.brush.style || "pressure";
    opts.push(UI.brushPicker(Render.VBRUSHES, st, (id) => { s.brush.style = id; }, (cv, id) => UI.vbrushPreview(cv, id, id === st ? s.brush : null)),
      slider("Size", 1, 300, 1, () => s.brush.size, (v) => (s.brush.size = v), { record: false }),
      ...UI.vbrushParams(st, (k) => s.brush[k], (k, v) => { s.brush[k] = v; UI.refreshPreview(); }, false),
      slider("Pressure", 0, 1, 0.01, () => s.brush.pressure, (v) => (s.brush.pressure = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }),
      slider("Smoothing", 0, 1, 0.01, () => s.brush.smooth, (v) => (s.brush.smooth = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }),
      check("Tapered ends", () => s.brush.taper, (v) => (s.brush.taper = v), { record: false }),
      btn("Reset style settings", () => { Object.assign(s.brush, { sp: 1, amt: 0.5, ang: 45, freq: 1, dot: 1 }); UI.buildProps(); }));
  }
  if (t === "paint") {
    const kind = s.paint.brush || "round";
    opts.push(UI.brushPicker(Object.entries(App.PAINT_BRUSHES).map(([id, b]) => [id, b.name]), kind, (id) => App.setPaintBrush(id), (cv, id) => App.paintPreview(cv, id, s.paint.color, id === kind ? s.paint : (s.paintSets || {})[id])));
    opts.push(...UI.brushSettings(s.paint, kind, false));
  }
  if (t === "eraser") {
    const hints = {
      all: "Rubs out painted pixels and cuts through vector shapes and lines where you erase.",
      pixels: "Only rubs out painted pixels; vector shapes stay untouched.",
      cut: "Cuts away just the part of vector shapes and lines you erase over. Painted pixels stay.",
      objects: "Removes every vector shape or line you touch, as a whole."
    };
    const hint = h("p", { class: "hint" }, hints[s.eraser.mode] || hints.all);
    const kind = App.ERASER_BRUSHES.includes(s.eraser.brush) ? s.eraser.brush : "round";
    opts.push(select("Erases", [["all", "Everything (pixels + vectors)"], ["pixels", "Painted pixels only"], ["cut", "Parts of vectors"], ["objects", "Whole vector objects"]], () => s.eraser.mode, (v) => { s.eraser.mode = v; hint.textContent = hints[v]; UI.syncSymButton(); }, { record: false }), hint);
    if (s.eraser.mode !== "objects") {
      opts.push(h("p", { class: "hint" }, "Eraser brush — rubs out in the brush's shape and texture:"),
        UI.brushPicker(App.ERASER_BRUSHES.map((id) => [id, App.PAINT_BRUSHES[id].name]), kind, (id) => App.setPaintBrush(id, true), (cv, id) => App.paintPreview(cv, id, "#000", id === kind ? s.eraser : (s.eraserSets || {})[id], true)));
      if ((s.eraser.mode === "all" || s.eraser.mode === "cut") && !["round", "ink"].includes(kind))
        opts.push(select("On vector shapes", [["texture", "Erase with the texture (touched shapes turn into pixels)"], ["cut", "Clean cut (shapes stay editable vectors)"]], () => s.eraser.vectorMode || "texture", (v) => (s.eraser.vectorMode = v), { record: false }));
      opts.push(...UI.brushSettings(s.eraser, kind, true));
    } else opts.push(slider("Size", 2, 400, 1, () => s.eraser.size, (v) => (s.eraser.size = v), { record: false }));
    opts.push(h("p", { class: "hint" }, "Tip: flip your stylus to use its eraser end with any tool."));
  }
  if (t === "bucket") opts.push(slider("Tolerance", 0, 128, 1, () => s.bucket.tolerance, (v) => (s.bucket.tolerance = v), { record: false }), h("p", { class: "hint" }, "Click inside a vector shape to recolour it, or anywhere else to flood-fill pixels. Lines drawn with any tool act as borders."));
  if (t === "rect") opts.push(slider("Corner radius", 0, 400, 1, () => s.shape.radius, (v) => { s.shape.radius = v; if (editSel) Cmd.editSelected((o) => { if (o.type === "rect") o.r = v; }); }, { record: false }));
  if (t === "poly") opts.push(slider("Points", 3, 24, 1, () => s.shape.points, (v) => (s.shape.points = v), { record: false }), check("Star", () => s.shape.star, (v) => (s.shape.star = v), { record: false }), slider("Inner radius", 0.1, 0.95, 0.01, () => s.shape.inner, (v) => (s.shape.inner = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }));
  if (["rect", "ellipse", "poly", "line"].includes(t)) opts.push(check("Constrain (or hold Shift)", () => s.shape.constrain, (v) => (s.shape.constrain = v), { record: false }), h("p", { class: "hint" }, "Hold Alt to draw from the centre."));
  if (t === "pen") opts.push(check("Fill open paths", () => !!s.penFill, (v) => (s.penFill = v), { record: false }), btn("Finish path (Enter)", () => Tools.get("pen").finish()), h("p", { class: "hint" }, "Click for a corner, drag for a curve, click the first point to close."));
  if (t === "pencil") opts.push(check("Fill closed shapes", () => !!s.pencilFill, (v) => (s.pencilFill = v), { record: false }), h("p", { class: "hint" }, "Draws a smooth path you can edit with Direct selection."));
  if (t === "text") opts.push(select("Font", FONTS.map((f) => [f, f]), () => s.text.font, (v) => { s.text.font = v; if (editSel) Cmd.editSelected((o) => { if (o.type === "text") o.font = v; }); }, { record: false }), slider("Size", 8, 600, 1, () => s.text.size, (v) => { s.text.size = v; }, { record: false }), select("Weight", [[400, "Regular"], [700, "Bold"], [900, "Black"]], () => s.text.weight, (v) => (s.text.weight = +v), { record: false }), select("Align", [["left", "Left"], ["center", "Center"], ["right", "Right"]], () => s.text.align, (v) => (s.text.align = v), { record: false }));
  if (t === "direct") opts.push(h("div", { class: "btnrow" }, btn("Add point", () => Cmd.addNode()), btn("Delete point", () => Cmd.deleteNode()), btn("Open / close", () => Cmd.toggleClosed())), h("p", { class: "hint" }, "Double-click a point to switch between smooth and corner."));
  if (t === "select") opts.push(check("Keep proportions when scaling", () => !!s.keepRatio, (v) => (s.keepRatio = v), { record: false }));
  if (["rect", "ellipse", "poly", "line", "pen", "pencil", "vbrush", "text"].includes(t)) opts.push(slider("Opacity", 0, 1, 0.01, () => s.opacity, (v) => (s.opacity = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }));
  if (L && L.maskEdit && L.mask && (t === "paint" || t === "eraser")) opts.unshift(h("p", { class: "hint maskhint" }, t === "paint" ? "Painting on the LAYER MASK: strokes hide this layer." : "Erasing on the LAYER MASK: strokes bring this layer back."));
  if (opts.length) box.append(section("Tool", ...opts));
  if (SYM_TOOLS.includes(t)) box.append(UI.symmetrySection());
  if (App.Mask.current && MASK_TOOLS.includes(t)) box.append(section("Mask", h("p", { class: "hint" }, App.Mask.current.invert ? "On: you can only paint and erase outside the masked shapes (shaded red)." : "On: you can only paint and erase inside the masked shapes (outside is shaded red)."), btn("Turn mask off", () => App.Mask.clear())));

  // --- selection
  if (editSel) {
    const o = first;
    const kids = [
      slider("Opacity", 0, 1, 0.01, () => o.opacity ?? 1, (v) => { const c = App.selCell(); const ids = new Set(App.state.selection); c.objects.forEach((x) => { if (ids.has(x.id)) x.opacity = v; }); changed("op"); }, { fmt: (v) => Math.round(v * 100) + "%" }),
      select("Blend", BLENDS, () => o.blend || "source-over", (v) => { const c = App.selCell(); const ids = new Set(App.state.selection); c.objects.forEach((x) => { if (ids.has(x.id)) x.blend = v; }); changed("blend"); }),
      h("div", { class: "btnrow" }, btn("Duplicate", () => Cmd.duplicate()), btn("Repeat…", () => UI.repeatDialog()), btn("Delete", () => Cmd.delete())),
      h("div", { class: "btnrow" }, btn("Mirror copy ↔", () => Cmd.mirrorCopy(true)), btn("Mirror copy ↕", () => Cmd.mirrorCopy(false)), btn("To path", () => Cmd.toPath())),
      h("div", { class: "btnrow" }, btn("Group", () => Cmd.group()), btn("Ungroup", () => Cmd.ungroup())),
      h("div", { class: "btnrow" }, btn("Mask: paint outside", () => App.Mask.fromSelection(true)), btn("Mask: paint inside", () => App.Mask.fromSelection(false))),
      h("p", { class: "hint" }, "Merge shapes into one:"),
      h("div", { class: "btnrow" }, btn("Unite", () => Cmd.merge("unite")), btn("Subtract", () => Cmd.merge("subtract")), btn("Intersect", () => Cmd.merge("intersect")), btn("Exclude", () => Cmd.merge("exclude"))),
      h("div", { class: "btnrow" }, btn("Front", () => Cmd.arrange("front")), btn("Forward", () => Cmd.arrange("forward")), btn("Backward", () => Cmd.arrange("backward")), btn("Back", () => Cmd.arrange("back"))),
      h("div", { class: "btnrow" }, btn("Flip ↔", () => Cmd.flip(true)), btn("Flip ↕", () => Cmd.flip(false)), btn("Rotate 90°", () => Cmd.rotate(90))),
      h("div", { class: "btnrow" }, btn("Align left", () => Cmd.align("l", null)), btn("Centre", () => Cmd.align("c", "m")), btn("Align right", () => Cmd.align("r", null))),
      h("div", { class: "btnrow" }, btn("Align top", () => Cmd.align(null, "t")), btn("Middle", () => Cmd.align(null, "m")), btn("Align bottom", () => Cmd.align(null, "b")))
    ];
    const fb = sel.find((x) => x.type === "brush");
    if (fb) {
      const setSel = (k, v) => { const c = App.selCell(); const ids = new Set(App.state.selection); c.objects.forEach((x) => { if (ids.has(x.id) && x.type === "brush") x[k] = v; }); changed("brush-param"); };
      kids.unshift(...UI.vbrushParams(fb.style || "pressure", (k) => fb[k], setSel, true));
    }
    if (fb) kids.unshift(select("Brush style", Render.VBRUSHES, () => (sel.find((x) => x.type === "brush").style || "pressure"), (v) => { const c = App.selCell(); const ids = new Set(App.state.selection); c.objects.forEach((x) => { if (ids.has(x.id) && x.type === "brush") { x.style = v; if (!x.seed) x.seed = 1 + Math.floor(Math.random() * 9999); } }); changed("style"); UI.buildProps(); }));
    if (o.type === "text") kids.unshift(btn("Edit text", () => TextTool.edit(o)), slider("Text size", 8, 600, 1, () => o.size, (v) => { const c = App.selCell(); c.objects.forEach((x) => { if (x.id === o.id) x.size = v; }); changed("size"); }));
    const grouped = sel.length > 1 && sel[0].group && sel.every((x) => x.group === sel[0].group);
    box.append(section(`Selection · ${grouped ? "group of " : ""}${sel.length} ${sel.length === 1 ? "object" : "objects"}`, ...kids));
  }

  // --- layer
  if (L) {
    const kids = [
      slider("Layer opacity", 0, 1, 0.01, () => L.opacity, (v) => { L.opacity = v; changed("layer"); }, { fmt: (v) => Math.round(v * 100) + "%" }),
      select("Layer blend", BLENDS, () => L.blend || "source-over", (v) => { L.blend = v; changed("layer"); }),
      h("div", { class: "btnrow" }, btn("New layer", () => Layers.add()), btn("Duplicate", () => Layers.duplicate()), btn("Delete", () => Layers.remove())),
      h("div", { class: "btnrow" }, btn("Move up", () => Layers.move(1)), btn("Move down", () => Layers.move(-1))),
      check("Clipping mask: clip to the layer below", () => !!L.clip, () => Layers.toggleClip(), { record: false })
    ];
    if (L.lgroup) {
      const n = App.doc.layers.filter((M) => M.lgroup === L.lgroup).length;
      kids.push(h("p", { class: "hint" }, `⛓ Layer group ${L.lgroup} (${n} layers): the select tool picks and moves shapes on all of them together.`),
        h("div", { class: "btnrow" }, btn("Edit group…", () => UI.layerGroupDialog()), btn("Select all in group", () => { App.setTool("select"); Cmd.selectAll(); }), btn("Leave group", () => Layers.leaveGroup())));
    } else kids.push(btn("Group layers…", () => UI.layerGroupDialog()));
    if (L.clip) kids.push(h("p", { class: "hint" }, "This layer only shows where the layer below it has paint or shapes."));
    if (!L.mask) kids.push(h("div", { class: "btnrow" }, btn("Add layer mask", () => Layers.addMask(false)), btn("Mask from selection", () => Layers.addMask(true))));
    else {
      kids.push(
        h("div", { class: "seg2" }, btn("Edit layer", () => Layers.editMask(false), !L.maskEdit ? "on" : ""), btn("Edit mask", () => Layers.editMask(true), L.maskEdit ? "on" : "")),
        h("p", { class: "hint" }, L.maskEdit ? "Editing the mask: the paint brush hides parts of this layer (red), the eraser brings them back. Any brush works." : "The mask hides parts of this layer. Choose Edit mask to paint on it."),
        check("Mask on", () => L.mask.on !== false, () => Layers.maskOp("toggle"), { record: false }),
        h("div", { class: "btnrow" }, btn("Invert", () => Layers.maskOp("invert")), btn("Show all", () => Layers.maskOp("reveal")), btn("Hide all", () => Layers.maskOp("hide"))),
        btn("Delete mask", () => Layers.maskOp("delete"))
      );
    }
    box.append(section(`Layer · ${L.name}`, ...kids));
  }
  const d = App.doc;
  box.append(section(`Canvas · ${d.width} × ${d.height}`, h("div", { class: "btnrow" }, btn("Change size…", () => UI.canvasSizeDialog()), btn("Animation settings…", () => UI.docSettings()))));
};
// ---------------------------------------------------------------- symmetry & repeat copies
const SYM_MODES = [["off", "Off"], ["x", "Left ↔ right"], ["y", "Top ↕ bottom"], ["xy", "Four-way"], ["radial", "Radial (rotate)"], ["kaleido", "Kaleidoscope (rotate + mirror)"]];
UI.symmetrySection = function () {
  const sy = App.style.sym, d = App.doc;
  const set = (fn) => { fn(); UI.syncSymButton(); View.redraw(); UI.buildProps(); };
  const kids = [select("Symmetry", SYM_MODES, () => sy.mode, (v) => set(() => (sy.mode = v)), { record: false })];
  if (sy.mode === "radial" || sy.mode === "kaleido") kids.push(slider("Segments", 2, 24, 1, () => sy.n, (v) => { sy.n = v; View.redraw(); }, { record: false }));
  if (sy.mode !== "off") {
    kids.push(
      numberField("Centre X", () => Math.round(sy.cx ?? d.width / 2), (v) => { sy.cx = v; View.redraw(); }, { record: false }),
      numberField("Centre Y", () => Math.round(sy.cy ?? d.height / 2), (v) => { sy.cy = v; View.redraw(); }, { record: false }),
      h("div", { class: "btnrow" }, btn("Canvas centre", () => set(() => { sy.cx = sy.cy = null; })), btn("Selection centre", () => { const b = App.selBounds(); if (!b) { toast("Select something first."); return; } set(() => { sy.cx = b.x + b.w / 2; sy.cy = b.y + b.h / 2; }); })),
      h("p", { class: "hint" }, "Everything you draw with the pen, pencil, brushes, shapes and eraser is repeated across the dashed guides.")
    );
  }
  return section("Symmetry", ...kids);
};
UI.syncSymButton = function () {
  const b = document.querySelector(".tool.symbtn"); if (!b) return;
  const on = App.style.sym.mode !== "off";
  b.classList.toggle("active", on);
  b.title = on ? `Symmetry: ${SYM_MODES.find((m) => m[0] === App.style.sym.mode)[1]}` : "Symmetry: off";
};
UI.symmetryMenu = function (x, y) {
  const sy = App.style.sym;
  const pick = (mode, n) => () => { sy.mode = mode; if (n) sy.n = n; UI.syncSymButton(); View.redraw(); UI.buildProps(); if (mode !== "off" && !SYM_TOOLS.includes(App.state.tool)) toast("Symmetry works with the pen, pencil, brushes, shapes and eraser."); };
  UI.popup(x, y, [
    [(sy.mode === "off" ? "✓ " : "") + "Off", pick("off")], ["-"],
    [(sy.mode === "x" ? "✓ " : "") + "Mirror left ↔ right", pick("x")],
    [(sy.mode === "y" ? "✓ " : "") + "Mirror top ↕ bottom", pick("y")],
    [(sy.mode === "xy" ? "✓ " : "") + "Four-way mirror", pick("xy")], ["-"],
    ["Radial × 3", pick("radial", 3)], ["Radial × 6", pick("radial", 6)], ["Radial × 8", pick("radial", 8)],
    ["Kaleidoscope × 6", pick("kaleido", 6)], ["Kaleidoscope × 8", pick("kaleido", 8)], ["-"],
    ["More options…", () => UI.openProps()]
  ]);
};
UI.syncMaskButton = function () {
  const b = document.querySelector(".tool.maskbtn"); if (!b) return;
  const m = App.Mask.current;
  b.classList.toggle("active", !!m);
  b.title = m ? (m.invert ? "Mask on: paint outside the shapes (Q to turn off)" : "Mask on: paint inside the shapes (Q to turn off)") : "Mask: paint only outside / inside selected shapes (Q)";
};
UI.maskMenu = function (x, y) {
  const m = App.Mask.current;
  UI.popup(x, y, [
    [(m && m.invert ? "✓ " : "") + "Paint outside the selection (inverted)", () => App.Mask.fromSelection(true)],
    [(m && !m.invert ? "✓ " : "") + "Paint inside the selection", () => App.Mask.fromSelection(false)],
    ["-"],
    [m ? "Turn mask off" : "Mask is off", () => App.Mask.clear()]
  ]);
};
App.on("mask", () => { UI.syncMaskButton(); UI.buildProps(); });

UI.layerGroupDialog = function () {
  const d = App.doc, ai = App.state.layer, L = d.layers[ai];
  const picks = new Set(d.layers.map((M, i) => i).filter((i) => i === ai || (L.lgroup && d.layers[i].lgroup === L.lgroup)));
  const list = h("div", { class: "grouplist" });
  for (let i = d.layers.length - 1; i >= 0; i--) {
    const M = d.layers[i], id = "lg" + i;
    const box = h("input", { type: "checkbox", id, checked: picks.has(i), disabled: i === ai || M.kind !== "draw" });
    box.addEventListener("change", () => (box.checked ? picks.add(i) : picks.delete(i)));
    const note = M.kind !== "draw" ? " (imported media)" : M.lgroup && M.lgroup !== L.lgroup ? ` (now in group ${M.lgroup})` : i === ai ? " (this layer)" : "";
    list.append(h("label", { class: "check", for: id }, box, M.name + note));
  }
  const body = h("div", {}, h("p", { class: "hint" }, "Tick the layers to link with this one. With the select tool, clicking or dragging a box then picks shapes on all of them, and moving, scaling, rotating, flipping, aligning, recolouring, duplicating or deleting works on them together. Each shape stays on its own layer."), list);
  const dlg = UI.dialog(L.lgroup ? `Layer group ${L.lgroup}` : "Group layers", body, [
    L.lgroup ? btn("Ungroup all", () => { dlg.close(); Layers.setGroup([]); }) : null,
    btn("Cancel", () => dlg.close()),
    btn("Link layers", () => { dlg.close(); Layers.setGroup([...picks]); }, "primary")
  ].filter(Boolean));
};

UI.repeatDialog = function () {
  if (!App.state.selection.length) { toast("Select the objects to repeat first."); return; }
  const r = App.style.repeat;
  const body = h("div", {},
    numberField("Copies", () => r.count, (v) => (r.count = v), { min: 1, max: 500, record: false }),
    numberField("Move X (px)", () => r.dx, (v) => (r.dx = v), { record: false }),
    numberField("Move Y (px)", () => r.dy, (v) => (r.dy = v), { record: false }),
    numberField("Rotate each (°)", () => r.rotate, (v) => (r.rotate = v), { min: -360, max: 360, record: false }),
    numberField("Scale each (%)", () => r.scale, (v) => (r.scale = v), { min: 1, max: 1000, record: false }),
    numberField("Opacity each (%)", () => r.fade, (v) => (r.fade = v), { min: 0, max: 100, record: false }),
    select("Turn & scale around", [["selection", "Selection centre"], ["canvas", "Canvas centre"]], () => r.pivot, (v) => (r.pivot = v), { record: false }),
    h("p", { class: "hint" }, "Each copy moves, turns and scales one more step than the one before. For a ring of copies, use Rotate = 360 ÷ (copies + 1) around the canvas centre.")
  );
  const presets = h("div", { class: "btnrow" },
    btn("Row", () => { Object.assign(r, { count: 4, dx: 60, dy: 0, rotate: 0, scale: 100, fade: 100, pivot: "selection" }); dlg.close(); UI.repeatDialog(); }),
    btn("Ring of 6", () => { Object.assign(r, { count: 5, dx: 0, dy: 0, rotate: 60, scale: 100, fade: 100, pivot: "canvas" }); dlg.close(); UI.repeatDialog(); }),
    btn("Spiral", () => { Object.assign(r, { count: 12, dx: 0, dy: 0, rotate: 30, scale: 92, fade: 100, pivot: "canvas" }); dlg.close(); UI.repeatDialog(); }),
    btn("Motion trail", () => { Object.assign(r, { count: 5, dx: -30, dy: 0, rotate: 0, scale: 100, fade: 70, pivot: "selection" }); dlg.close(); UI.repeatDialog(); }));
  body.prepend(h("p", { class: "hint" }, "Presets"), presets);
  const dlg = UI.dialog("Repeat copies", body, [btn("Cancel", () => dlg.close()), btn("Make copies", () => { dlg.close(); Cmd.repeat(r); }, "primary")]);
};

// ---------------------------------------------------------------- brush pickers
/** A grid of brush tiles, each with a little sample stroke. */
UI.brushPicker = function (items, current, pick, draw) {
  const grid = h("div", { class: "brushgrid", role: "radiogroup", "aria-label": "Brush" });
  for (const [id, name] of items) {
    const cv = h("canvas", { width: 132, height: 40, "aria-hidden": "true" });
    const b = h("button", { type: "button", class: "brushtile" + (id === current ? " on" : ""), role: "radio", "aria-checked": String(id === current), title: name, onclick: () => { pick(id); UI.buildProps(); } }, cv, h("span", {}, name));
    grid.append(b);
    cv._draw = () => draw(cv, id);
    requestAnimationFrame(() => { try { draw(cv, id); } catch (e) { /* preview only */ } });
  }
  return grid;
};
/** The settings every paint / eraser brush offers, plus the ones only some brushes use. */
UI.brushSettings = function (tgt, kind, erase) {
  const pct = { record: false, fmt: (v) => Math.round(v * 100) + "%" }, deg = { record: false, fmt: (v) => Math.round(v) + "°" };
  const redraw = () => UI.refreshPreview();
  const sl = (label, key, min, max, step, o = pct) => slider(label, min, max, step, () => tgt[key] ?? 0, (v) => { tgt[key] = v; redraw(); }, o);
  const B = App.PAINT_BRUSHES[kind], textured = ["chalk", "pencil", "crayon", "charcoal", "oil", "marker", "watercolor", "hatching"].includes(kind);
  const out = [sl("Size", "size", 1, 400, 1, { record: false })];
  if (!erase && !B.direct) out.push(check("Each stroke is its own object (select, move, delete it)", () => tgt.asObject !== false, (v) => { tgt.asObject = v; }, { record: false }));
  if (["round", "ink", "marker", "glow"].includes(kind)) out.push(sl("Hardness", "hardness", 0, 1, 0.01));
  if (!B.direct) out.push(sl(erase ? "Strength" : "Opacity", "opacity", 0.05, 1, 0.01));
  out.push(sl(kind === "smudge" || kind === "blur" ? "Strength" : "Flow", "flow", 0.02, 1, 0.01));
  out.push(sl("Spacing", "spacing", 0.01, 1.5, 0.01));
  if (kind === "spray" || kind === "dots") out.push(sl("Density", "density", 0.1, 4, 0.05, { record: false, fmt: (v) => v.toFixed(2) + "×" }), sl("Dot size", "dotSize", 0.2, 6, 0.05, { record: false, fmt: (v) => v.toFixed(2) + "×" }));
  if (kind === "oil") out.push(sl("Colour mixing", "mix", 0, 1, 0.01));
  out.push(sl("Size jitter", "sizeJitter", 0, 1, 0.01), sl("Scatter", "scatter", 0, 2, 0.01));
  if (textured) {
    out.push(sl("Angle", "angle", -180, 180, 1, deg), sl("Angle jitter", "angleJitter", 0, 1, 0.01),
      check("Turn with the stroke", () => !!tgt.follow, (v) => { tgt.follow = v; redraw(); }, { record: false }));
  }
  if (!erase && !["smudge", "blur"].includes(kind)) out.push(sl("Colour jitter", "colorJitter", 0, 1, 0.01));
  out.push(sl("Stabiliser", "smoothing", 0, 1, 0.01),
    check("Pressure changes size", () => tgt.pressureSize, (v) => { tgt.pressureSize = v; redraw(); }, { record: false }));
  if (tgt.pressureSize) out.push(sl("Thinnest (light press)", "pressureMin", 0, 1, 0.01));
  if (!B.direct) out.push(check(erase ? "Pressure changes strength" : "Pressure changes opacity", () => tgt.pressureOpacity, (v) => { tgt.pressureOpacity = v; redraw(); }, { record: false }));
  const hints = {
    oil: "Picks up the paint already on this layer and blends it into the stroke.", smudge: "Drags the painted pixels on this layer along with your stroke.",
    blur: "Softens the painted pixels on this layer where you brush.", watercolor: "Build it up in layers: each stroke is translucent with a darker rim.",
    marker: "Overlapping in one stroke stays even; a new stroke on top darkens it.", glow: "Adds light: looks best on dark colours. Colours brighten where strokes overlap.",
    hatching: "Draws parallel lines at the chosen angle. Cross-hatch by changing the angle and going over it again."
  };
  if (hints[kind] && !erase) out.push(h("p", { class: "hint" }, hints[kind]));
  out.push(btn("Reset this brush", () => { App.resetPaintBrush(erase); UI.buildProps(); }));
  return out;
};
/** Re-draws the selected brush tile's sample after a setting changes. */
UI.refreshPreview = function () {
  const tile = document.querySelector(".brushtile.on canvas"); if (!tile || !tile._draw) return;
  cancelAnimationFrame(UI._pv); UI._pv = requestAnimationFrame(() => { try { tile._draw(); } catch (e) { /* preview only */ } });
};
/** Sliders for the extra settings of a vector brush style (on the tool, or on selected strokes). */
UI.vbrushParams = function (style, get, set, record) {
  const P = Render.VBRUSH_PARAMS[style] || {}, out = [];
  const o = (fmt) => ({ record, fmt });
  if (P.sp) out.push(slider(P.sp, 0.3, 3, 0.05, () => get("sp") ?? 1, (v) => set("sp", v), o((v) => v.toFixed(2) + "×")));
  if (P.amt) out.push(slider(P.amt, 0, 1, 0.01, () => get("amt") ?? 0.5, (v) => set("amt", v), o((v) => Math.round(v * 100) + "%")));
  if (P.ang) out.push(slider(P.ang, 0, 180, 1, () => get("ang") ?? 45, (v) => set("ang", v), o((v) => Math.round(v) + "°")));
  if (P.dot) out.push(slider(P.dot, 0.2, 6, 0.05, () => get("dot") ?? 1, (v) => set("dot", v), o((v) => v.toFixed(2) + "×")));
  if (P.freq) out.push(slider(P.freq, 0.3, 4, 0.05, () => get("freq") ?? 1, (v) => set("freq", v), o((v) => v.toFixed(2) + "×")));
  return out;
};
UI.vbrushPreview = function (cv, style, params) {
  const g = cv.getContext("2d"), W = cv.width, H = cv.height;
  g.clearRect(0, 0, W, H);
  const pts = [];
  for (let i = 0; i <= 40; i++) { const t = i / 40; pts.push(8 + t * (W - 16), H / 2 + Math.sin(t * Math.PI * 2) * H * 0.22, 0.3 + 0.7 * Math.sin(t * Math.PI)); }
  const fill = App.style.grad ? { ...App.style.grad } : (App.style.fill || App.style.stroke || "#111");
  const q = params || {};
  Render.drawObj(g, { type: "brush", pts, size: 8, pressure: 0.8, taper: true, style, seed: 7, sp: q.sp ?? 1, amt: q.amt ?? 0.5, ang: q.ang ?? 45, freq: q.freq ?? 1, dot: q.dot ?? 1, m: M.id(), fill, opacity: 1 });
};

UI.syncSwatch = function () {
  const f = document.querySelector(".sw.fill"), s = document.querySelector(".sw.stroke");
  if (f) f.style.background = App.state.tool === "paint" ? App.style.paint.color : typeof App.style.fill === "string" ? App.style.fill : "#888";
  if (s) s.style.borderColor = App.style.strokeOn ? App.style.stroke : "transparent";
};
const recent = ["#111827", "#ffffff", "#e11d48", "#f97316", "#facc15", "#22c55e", "#06b6d4", "#3b82f6", "#8b5cf6", "#ec4899", "#78350f", "#94a3b8"];
// ---------------------------------------------------------------- the Colour tab
const RECENT_KEY = "frame-studio-recent-colours";
let recentUsed = [];
try { recentUsed = JSON.parse(localStorage.getItem(RECENT_KEY) || "[]").filter((c) => typeof c === "string").slice(0, 8); } catch (e) { recentUsed = []; }
/** Remembers a colour you used (newest first, 8 at most, kept between sessions on this device). */
App.rememberColor = function (c) {
  if (!c || typeof c !== "string") return;
  let key; try { const p = Color.parse(c); key = p[3] < 1 ? Color.withAlpha(Color.hex(c), p[3]) : Color.hex(c).toLowerCase(); } catch (e) { return; }
  if (recentUsed[0] === key) return;
  recentUsed = [key, ...recentUsed.filter((x) => x !== key)].slice(0, 8);
  try { localStorage.setItem(RECENT_KEY, JSON.stringify(recentUsed)); } catch (e) { /* storage blocked: memory lasts this session */ }
  if (UI.tab === "colour") UI.renderRecent();
};
UI.tab = "props";
UI.showTab = function (which) {
  UI.tab = which;
  $("colourBody").hidden = which !== "colour"; $("propsBody").hidden = which === "colour";
  $("tabColour").setAttribute("aria-selected", String(which === "colour")); $("tabProps").setAttribute("aria-selected", String(which !== "colour"));
  UI.openProps();
};
UI.applyTarget = "fill";   // what a swatch click sets: fill (paint) or stroke
/** Applies a swatch colour to the fill / paint colour (or the stroke) and to any selected shapes. */
function applySwatch(c) {
  const t = App.state.tool, sel = ["select", "direct"].includes(t) && App.state.selection.length;
  App.rememberColor(c);
  if (UI.applyTarget === "stroke") {
    App.style.strokeOn = true; App.style.stroke = c;
    if (sel) Cmd.editSelected((o) => { o.stroke = c; if (!o.sw) o.sw = App.style.sw; });
  } else {
    if (sel) Cmd.editSelected((o) => { o.fill = c; });
    App.style.fill = c; App.style.grad = null; App.style.paint.color = c;
  }
  UI.syncSwatch(); UI.buildProps();
}
UI.renderRecent = function () {
  const box = document.getElementById("recentBox"); if (!box) return;
  box.textContent = "";
  for (let i = 0; i < 8; i++) {
    const c = recentUsed[i];
    box.append(c ? h("button", { class: "chip", type: "button", title: c, "aria-label": "Recent colour " + c, style: `background:${c}`, onclick: () => applySwatch(c) }) : h("span", { class: "empty", "aria-hidden": "true" }));
  }
};
UI.buildColour = function ({ t, s, editSel, first, fillNow, applyFill, applyStroke }) {
  const box = $("colourBody"); if (!box) return;
  box.textContent = "";
  const isPaint = t === "paint" || t === "bucket";
  const cur = isPaint ? s.paint.color : fillNow;
  const setMain = (v) => { if (isPaint) { s.paint.color = v || s.paint.color; s.fill = v || s.fill; UI.syncSwatch(); } else applyFill(v); };
  const hex = h("input", { type: "text", value: cur && typeof cur === "string" ? Color.hex(cur) : "", placeholder: "#1d4ed8", spellcheck: "false", "aria-label": "Hex colour" });
  hex.addEventListener("change", () => { const v = hex.value.trim(); if (!/^#?[0-9a-f]{3}([0-9a-f]{3})?$/i.test(v)) { toast("Type a colour like #ff5a36."); return; } const c = v[0] === "#" ? v : "#" + v; if (editSel) History.push(); setMain(c); App.rememberColor(c); UI.buildProps(); });
  const main = colorRow(isPaint ? "Paint" : "Fill", () => cur, setMain, { allowNone: !["paint", "bucket", "vbrush"].includes(t), record: !!editSel });
  // picking in the colour dialog counts as using that colour
  main.querySelector("input[type=color]").addEventListener("change", (e) => App.rememberColor(e.target.value));
  const stroke = colorRow("Stroke", () => (first ? first.stroke : s.strokeOn ? s.stroke : null), applyStroke, { allowNone: true, record: !!editSel });
  stroke.querySelector("input[type=color]").addEventListener("change", (e) => App.rememberColor(e.target.value));
  const target = h("div", { class: "seg2" },
    btn("Swatches set fill", () => { UI.applyTarget = "fill"; UI.buildProps(); }, UI.applyTarget === "fill" ? "on" : ""),
    btn("Swatches set stroke", () => { UI.applyTarget = "stroke"; UI.buildProps(); }, UI.applyTarget === "stroke" ? "on" : ""));
  box.append(
    section(editSel ? `Colour · ${App.state.selection.length} selected` : isPaint ? "Paint colour" : "Colour",
      main, h("div", { class: "hexrow" }, h("label", {}, "Hex"), hex), stroke,
      slider("Stroke width", 0, 80, 0.5, () => (first ? first.sw || 0 : s.sw), (v) => { s.sw = v; if (editSel) { const cell = App.selCell(); const ids = new Set(App.state.selection); cell.objects.forEach((o) => { if (ids.has(o.id)) o.sw = v; }); changed("sw"); } }, { record: !!editSel }),
      h("div", { class: "btnrow" }, btn("Gradient…", () => UI.gradientDialog()), btn("Pick from canvas", () => App.setTool("eyedropper"))),
      s.grad ? h("p", { class: "hint" }, `${s.grad.type} gradient fill is on — pick a solid colour to turn it off.`) : null),
    section("Recent colours", h("div", { class: "recent", id: "recentBox" }),
      h("p", { class: "hint" }, "The last 8 colours you drew or filled with. They stay here next time you open the app."),
      btn("Clear recent", () => { recentUsed = []; try { localStorage.removeItem(RECENT_KEY); } catch (e) { /* ignore */ } UI.renderRecent(); })),
    section("Swatches", target, UI.recentSwatches())
  );
  UI.renderRecent();
};
UI.recentSwatches = function () {
  return h("div", { class: "palette" }, recent.map((c) => h("button", { class: "chip", type: "button", title: c, style: `background:${c}`, onclick: () => applySwatch(c) })));
};

// ---------------------------------------------------------------- media clip settings (video / image / sequence / sound)
UI.mediaSections = function (L) {
  const s = L.settings, m = L.media, d = App.doc;
  const upd = () => { changed("media"); Media.sync(App.state.frame, App.state.playing); };
  const secs = [];
  const fmtS = (v) => v.toFixed(2) + "s";
  const info = [
    `${m.type === "video" ? "Video" : m.type === "audio" ? "Sound" : m.type === "sequence" ? "Image sequence" : "Image"} · ${m.name || ""}`,
    m.type !== "audio" ? `${m.w}×${m.h}px` : null,
    m.duration ? `${m.duration.toFixed(2)}s` : null,
    m.type === "sequence" ? `${m.frames.length} frames` : null,
    m.size ? `${(m.size / 1e6).toFixed(1)} MB` : null
  ].filter(Boolean).join(" · ");
  secs.push(section("Clip info", h("p", { class: "hint mono" }, info)));

  if (m.type !== "image") {
    const dur = m.duration || 0;
    secs.push(section("Timing",
      numberField("Starts at frame", () => s.start + 1, (v) => { s.start = Math.max(0, Math.round(v) - 1); upd(); }, { min: 1, max: d.frames }),
      slider("Trim start", 0, Math.max(0.01, dur), 0.01, () => s.trimIn, (v) => { s.trimIn = Math.min(v, s.trimOut - 0.01); upd(); }, { fmt: fmtS }),
      slider("Trim end", 0, Math.max(0.01, dur), 0.01, () => s.trimOut, (v) => { s.trimOut = Math.max(v, s.trimIn + 0.01); upd(); }, { fmt: fmtS }),
      slider("Speed", 0.1, 4, 0.05, () => s.speed, (v) => { s.speed = v; upd(); }, { fmt: (v) => v.toFixed(2) + "×" }),
      m.type === "sequence" ? numberField("Sequence fps (0 = project)", () => s.seqFps, (v) => { s.seqFps = Math.max(0, v); upd(); }, { min: 0, max: 120 }) : null,
      select("Frame rate feel", [[1, "Smooth (every frame)"], [2, "On twos (hold 2)"], [3, "On threes"], [4, "Hold 4 (choppy)"], [6, "Hold 6 (stop-motion)"]], () => s.hold, (v) => { s.hold = +v; upd(); }),
      check("Play backwards", () => s.reverse, (v) => { s.reverse = v; upd(); }),
      check("Loop the clip", () => s.loop, (v) => { s.loop = v; upd(); }),
      h("p", { class: "hint" }, `On the timeline: ${isFinite(Media.lengthFrames(L)) ? Media.lengthFrames(L) + " frames (" + (Media.lengthFrames(L) / d.fps).toFixed(2) + "s)" : "loops to the end"}. Drag the clip bar in the timeline to move it.`)
    ));
  }
  if (m.type !== "audio") {
    secs.push(section("Placement",
      select("Fit", [["contain", "Fit inside canvas"], ["cover", "Fill canvas (crop)"], ["stretch", "Stretch to canvas"], ["none", "Original size"]], () => s.fit, (v) => { s.fit = v; upd(); }),
      slider("Scale", 0.05, 4, 0.01, () => s.scale, (v) => { s.scale = v; upd(); }, { fmt: (v) => Math.round(v * 100) + "%" }),
      slider("Move X", -d.width, d.width, 1, () => s.x, (v) => { s.x = v; upd(); }),
      slider("Move Y", -d.height, d.height, 1, () => s.y, (v) => { s.y = v; upd(); }),
      slider("Rotate", -180, 180, 1, () => s.rotation, (v) => { s.rotation = v; upd(); }, { unit: "°" }),
      h("div", { class: "btnrow" }, check("Flip horizontal", () => s.flipX, (v) => { s.flipX = v; upd(); }), check("Flip vertical", () => s.flipY, (v) => { s.flipY = v; upd(); })),
      btn("Reset placement", () => { History.push(); Object.assign(s, { fit: "contain", x: 0, y: 0, scale: 1, rotation: 0, flipX: false, flipY: false }); upd(); UI.buildProps(); })
    ));
    secs.push(section("Look",
      slider("Brightness", 0, 200, 1, () => s.brightness, (v) => { s.brightness = v; upd(); }, { unit: "%" }),
      slider("Contrast", 0, 200, 1, () => s.contrast, (v) => { s.contrast = v; upd(); }, { unit: "%" }),
      slider("Saturation", 0, 300, 1, () => s.saturation, (v) => { s.saturation = v; upd(); }, { unit: "%" }),
      slider("Hue", -180, 180, 1, () => s.hue, (v) => { s.hue = v; upd(); }, { unit: "°" }),
      slider("Blur", 0, 40, 0.5, () => s.blur, (v) => { s.blur = v; upd(); }, { unit: "px" }),
      slider("Greyscale", 0, 100, 1, () => s.grayscale, (v) => { s.grayscale = v; upd(); }, { unit: "%" }),
      slider("Sepia", 0, 100, 1, () => s.sepia, (v) => { s.sepia = v; upd(); }, { unit: "%" }),
      slider("Invert", 0, 100, 1, () => s.invert, (v) => { s.invert = v; upd(); }, { unit: "%" }),
      btn("Reset look", () => { History.push(); Object.assign(s, { brightness: 100, contrast: 100, saturation: 100, hue: 0, blur: 0, grayscale: 0, sepia: 0, invert: 0 }); upd(); UI.buildProps(); })
    ));
  }
  if (m.type === "video" || m.type === "audio") {
    secs.push(section("Sound",
      check("Mute", () => s.muted, (v) => { s.muted = v; upd(); }),
      slider("Volume", 0, 1, 0.01, () => s.volume, (v) => { s.volume = v; upd(); }, { fmt: (v) => Math.round(v * 100) + "%" }),
      h("p", { class: "hint" }, "Sound is included when you export MP4 or WebM.")
    ));
  }
  if (m.type === "video" || m.type === "sequence") {
    secs.push(section("Draw over it", h("p", { class: "hint" }, "Bake the clip into drawable pixel frames on a new layer (rotoscoping). The original stays, hidden."), btn("Convert to drawable frames", () => UI.rotoscope(), "primary")));
  }
  return secs;
};
UI.rotoscope = async function () {
  const L = activeLayer(); if (!L || L.kind !== "media" || L.media.type === "audio") return toast("Select a video or image-sequence layer first.");
  const prog = UI.progress("Converting to frames…");
  try { await Media.toDrawFrames(L, (p) => prog.set(p)); toast("Done. Draw on the new layer; the clip is hidden underneath."); }
  catch (e) { toast("Conversion failed: " + e.message); }
  prog.close(); UI.buildProps();
};

// ---------------------------------------------------------------- dialogs & popups
UI.popup = function (x, y, items) {
  UI.closePopup();
  const menu = h("div", { class: "popup", role: "menu" });
  for (const [label, fn] of items) {
    if (label === "-") { menu.append(h("hr")); continue; }
    menu.append(h("button", { type: "button", role: "menuitem", onclick: () => { UI.closePopup(); fn(); } }, label));
  }
  document.body.append(menu);
  const r = menu.getBoundingClientRect();
  menu.style.left = Math.max(8, Math.min(window.innerWidth - r.width - 8, x)) + "px";
  menu.style.top = Math.max(8, Math.min(window.innerHeight - r.height - 8, y)) + "px";
  setTimeout(() => document.addEventListener("pointerdown", UI._outside = (e) => { if (!menu.contains(e.target)) UI.closePopup(); }), 0);
};
UI.closePopup = function () { document.querySelectorAll(".popup").forEach((p) => p.remove()); if (UI._outside) document.removeEventListener("pointerdown", UI._outside); };
UI.dialog = function (title, body, actions) {
  const dlg = h("dialog", { class: "dlg" }, h("div", { class: "dlg-head" }, h("h2", {}, title), h("button", { class: "btn tiny", type: "button", onclick: () => dlg.close(), "aria-label": "Close" }, "✕")), h("div", { class: "dlg-body" }, body), h("div", { class: "dlg-actions" }, actions));
  dlg.addEventListener("close", () => dlg.remove());
  document.body.append(dlg); dlg.showModal(); return dlg;
};
UI.progress = function (label) {
  const bar = h("div", { class: "bar" });
  const txt = h("p", { class: "hint mono" }, label);
  let cancelled = false;
  const dlg = h("dialog", { class: "dlg small" }, h("h2", {}, label), h("div", { class: "progress" }, bar), txt, h("div", { class: "dlg-actions" }, h("button", { class: "btn", type: "button", onclick: () => { cancelled = true; txt.textContent = "Cancelling…"; } }, "Cancel")));
  dlg.addEventListener("cancel", (e) => e.preventDefault());
  document.body.append(dlg); dlg.showModal();
  return { set: (p, msg) => { bar.style.width = Math.round(clamp(p, 0, 1) * 100) + "%"; if (msg) txt.textContent = msg; }, get cancelled() { return cancelled; }, close: () => { dlg.close(); dlg.remove(); } };
};

UI.gradientDialog = function () {
  const g = App.style.grad ? { ...App.style.grad } : { type: "linear", c1: typeof App.style.fill === "string" ? Color.hex(App.style.fill) : "#ff5a36", c2: "#1f1d5c", angle: 90 };
  const body = h("div", {},
    select("Type", [["linear", "Linear"], ["radial", "Radial"]], () => g.type, (v) => (g.type = v), { record: false }),
    colorRow("Start", () => g.c1, (v) => (g.c1 = v), { record: false }),
    colorRow("End", () => g.c2, (v) => (g.c2 = v), { record: false }),
    slider("Angle", 0, 360, 1, () => g.angle, (v) => (g.angle = v), { record: false, unit: "°" })
  );
  const dlg = UI.dialog("Gradient fill", body, [
    btn("Solid colour", () => { App.style.grad = null; dlg.close(); UI.buildProps(); }),
    btn("Apply", () => {
      App.style.grad = g;
      if (App.state.selection.length) Cmd.editSelected((o) => { if (o.type !== "image") o.fill = { ...g }; });
      dlg.close(); UI.buildProps();
    }, "primary")
  ]);
};

UI.canvasSizeDialog = function () {
  const d = App.doc, v = { w: d.width, h: d.height, fit: "scale", ax: 0.5, ay: 0.5, lock: false };
  const ratio = d.width / d.height;
  const presets = [["", "Custom size"], ["1080x1080", "Square 1080 × 1080"], ["1920x1080", "HD 1920 × 1080"], ["1080x1920", "Vertical / story 1080 × 1920"], ["1080x1350", "Portrait post 1080 × 1350"], ["3000x3000", "Album cover 3000 × 3000"], ["1280x720", "720p 1280 × 720"], ["3840x2160", "4K 3840 × 2160"], ["800x600", "800 × 600"], ["512x512", "Sticker 512 × 512"]];
  const wIn = h("input", { type: "number", value: v.w, min: 16, max: 4096, id: "csW" }), hIn = h("input", { type: "number", value: v.h, min: 16, max: 4096, id: "csH" });
  // preview of the current frame in the new canvas
  const snap = document.createElement("canvas"); snap.width = d.width; snap.height = d.height; Render.frame(snap.getContext("2d"), App.state.frame);
  const pv = h("canvas", { width: 260, height: 200, class: "cs-preview", "aria-label": "Preview of the new canvas" });
  const anchor = h("div", { class: "anchor", role: "group", "aria-label": "Anchor" });
  const draw = () => {
    const g = pv.getContext("2d"); g.clearRect(0, 0, pv.width, pv.height);
    const nw = clamp(+wIn.value || v.w, 16, 4096), nh = clamp(+hIn.value || v.h, 16, 4096), W = d.width, H = d.height;
    const k = Math.min((pv.width - 20) / Math.max(nw, W), (pv.height - 20) / Math.max(nh, H)), ox = (pv.width - nw * k) / 2, oy = (pv.height - nh * k) / 2;
    let T;
    if (v.fit === "stretch") T = [nw / W, 0, 0, nh / H, 0, 0];
    else if (v.fit === "scale" || v.fit === "fill") { const u = v.fit === "scale" ? Math.min(nw / W, nh / H) : Math.max(nw / W, nh / H); T = [u, 0, 0, u, (nw - W * u) / 2, (nh - H * u) / 2]; }
    else T = [1, 0, 0, 1, (nw - W) * v.ax, (nh - H) * v.ay];
    g.save(); g.translate(ox, oy); g.scale(k, k);
    g.fillStyle = d.transparent ? "#ddd" : d.background; g.fillRect(0, 0, nw, nh);
    g.save(); g.beginPath(); g.rect(0, 0, nw, nh); g.clip(); g.transform(...T); g.drawImage(snap, 0, 0); g.restore();
    g.save(); g.transform(...T); g.setLineDash([4 / k, 3 / k]); g.strokeStyle = "rgba(225,29,72,.9)"; g.lineWidth = 1.5 / k / Math.max(T[0], 0.01); g.strokeRect(0, 0, W, H); g.restore();
    g.strokeStyle = "#2f80ff"; g.lineWidth = 2 / k; g.strokeRect(0, 0, nw, nh); g.restore();
    anchorRow.style.display = v.fit === "none" ? "" : "none";
    info.textContent = `${d.width} × ${d.height} → ${nw} × ${nh}. Dashed red = your current artwork area.`;
  };
  const info = h("p", { class: "hint" });
  for (const ay of [0, 0.5, 1]) for (const ax of [0, 0.5, 1]) {
    const b = h("button", { type: "button", class: "btn tiny" + (ax === v.ax && ay === v.ay ? " on" : ""), "aria-label": `Anchor ${ay === 0 ? "top" : ay === 1 ? "bottom" : "middle"} ${ax === 0 ? "left" : ax === 1 ? "right" : "centre"}`, onclick: () => { v.ax = ax; v.ay = ay; anchor.querySelectorAll("button").forEach((x) => x.classList.remove("on")); b.classList.add("on"); draw(); } }, "●");
    anchor.append(b);
  }
  const anchorRow = h("div", { class: "row" }, h("label", {}, "Anchor"), anchor);
  const syncW = () => { if (v.lock) hIn.value = Math.round(+wIn.value / ratio); draw(); };
  const syncH = () => { if (v.lock) wIn.value = Math.round(+hIn.value * ratio); draw(); };
  wIn.addEventListener("input", syncW); hIn.addEventListener("input", syncH);
  const body = h("div", {},
    select("Preset", presets, () => `${v.w}x${v.h}`, (p) => { if (!p) return; const [a, b] = p.split("x").map(Number); wIn.value = a; hIn.value = b; draw(); }, { record: false }),
    h("div", { class: "row" }, h("label", { for: "csW" }, "Width (px)"), wIn), h("div", { class: "row" }, h("label", { for: "csH" }, "Height (px)"), hIn),
    check("Keep proportions", () => v.lock, (x) => { v.lock = x; syncW(); }, { record: false }),
    select("Your artwork", [["scale", "Scale to fit (keep proportions)"], ["fill", "Scale to fill (keep proportions, may crop)"], ["stretch", "Stretch to the new size"], ["none", "Keep its size — crop or add space"]], () => v.fit, (x) => { v.fit = x; draw(); }, { record: false }),
    anchorRow,
    pv, info,
    h("p", { class: "hint" }, "Every frame, layer, mask and imported clip is adjusted. You can undo this.")
  );
  const dlg = UI.dialog("Canvas size", body, [btn("Cancel", () => dlg.close()), btn("Resize", () => { const nw = +wIn.value, nh = +hIn.value; dlg.close(); App.resizeCanvas(nw, nh, v.fit, v.ax, v.ay); toast(`Canvas is now ${App.doc.width} × ${App.doc.height}.`); }, "primary")]);
  draw();
};

UI.docSettings = function () {
  const d = App.doc, v = { name: d.name, w: d.width, h: d.height, fps: d.fps, frames: d.frames, speed: d.speed, bg: d.background, transparent: d.transparent };
  const body = h("div", {},
    h("div", { class: "row" }, h("label", { for: "dsName" }, "Name"), h("input", { id: "dsName", value: v.name, oninput: (e) => (v.name = e.target.value) })),
    h("div", { class: "row" }, h("label", {}, "Canvas size"), btn(`${d.width} × ${d.height} — change…`, () => { dlg.close(); UI.canvasSizeDialog(); })),
    numberField("Frame rate (fps)", () => v.fps, (x) => (v.fps = x), { min: 1, max: 60, record: false }),
    numberField("Length (frames)", () => v.frames, (x) => (v.frames = x), { min: 1, max: 7200, record: false }),
    select("Playback speed", [[0.25, "0.25×"], [0.5, "0.5×"], [0.75, "0.75×"], [1, "1× (normal)"], [1.25, "1.25×"], [1.5, "1.5×"], [2, "2×"], [3, "3×"], [4, "4×"]], () => v.speed, (x) => (v.speed = +x), { record: false }),
    colorRow("Background", () => v.bg, (x) => (v.bg = x || v.bg), { record: false }),
    check("Transparent background", () => v.transparent, (x) => (v.transparent = x), { record: false })
  );
  const dlg = UI.dialog("Animation settings", body, [btn("Cancel", () => dlg.close()), btn("Apply", () => {
    History.push();
    d.name = v.name.trim() || d.name; d.fps = clamp(Math.round(v.fps), 1, 60); d.speed = v.speed; d.background = v.bg; d.transparent = v.transparent;
    if (v.frames !== d.frames) Frames.setLength(v.frames);
    changed("doc"); View.fit(); UI.syncTimelineControls(); dlg.close();
  }, "primary")]);
};

UI.help = function () {
  const rows = [["Space + drag / two fingers", "Pan"], ["Ctrl + wheel / pinch", "Zoom"], ["Enter", "Play / stop"], [", and .", "Previous / next frame"], ["Shift + , and .", "Previous / next key"],
    ["F6", "New blank key"], ["F7", "Duplicate key"], ["F5", "Hold one more frame"], ["Ctrl+Z / Ctrl+Shift+Z", "Undo / redo"], ["Two-finger tap", "Undo"],
    ["Ctrl+C / V / D", "Copy / paste / duplicate objects"], ["Ctrl+Shift+D", "Repeat copies…"], ["Ctrl+G / Ctrl+Shift+G", "Group / ungroup"], ["Ctrl+M", "Merge (unite) shapes"], ["Q", "Mask on (paint outside selection) / off"], ["Delete", "Delete selection"], ["Arrows (Shift = 10px)", "Nudge"], ["Stylus eraser end", "Erase"], ["Stylus side button", "Eyedropper"]];
  const tbl = h("table", { class: "keys" }, rows.map(([k, v]) => h("tr", {}, h("td", {}, h("kbd", {}, k)), h("td", {}, v))));
  const tools = h("table", { class: "keys" }, TOOLS.map(([, t]) => h("tr", {}, h("td", {}, t))));
  UI.dialog("Shortcuts", h("div", { class: "cols" }, tbl, tools), []);
};

// ---------------------------------------------------------------- importing files
UI.importFiles = async function (files) {
  files = [...files]; if (!files.length) return;
  const groups = { image: [], gif: [], svg: [], video: [], audio: [], project: [], unknown: [] };
  for (const f of files) groups[Media.fileKind(f)].push(f);
  if (groups.project.length) {
    try { const data = JSON.parse(await groups.project[0].text()); await Project.load(data); View.fit(); UI.refreshAll(); toast("Project opened."); } catch (e) { toast(e.message || "Couldn't open that project."); }
    return;
  }
  const choice = { image: groups.image.length > 1 ? "sequence" : "object", video: "layer", gif: "sequence", extend: true, seqFps: 0 };
  const body = h("div", {});
  const list = (arr) => h("p", { class: "hint mono" }, arr.map((f) => f.name).join(", "));
  if (groups.image.length || groups.svg.length) {
    const n = groups.image.length + groups.svg.length;
    body.append(h("h3", {}, `${n} ${n === 1 ? "image" : "images"}`), list([...groups.image, ...groups.svg]),
      select("Add as", [["object", n === 1 ? "An object in this frame (movable, tweenable)" : "Objects in this frame"], ["layer", "A new image layer"], ...(groups.image.length > 1 ? [["sequence", "An image sequence (one image per frame)"]] : [])], () => choice.image, (v) => (choice.image = v), { record: false }));
    if (groups.image.length > 1) body.append(numberField("Sequence fps (0 = project)", () => choice.seqFps, (v) => (choice.seqFps = v), { min: 0, max: 120, record: false }));
  }
  if (groups.gif.length) body.append(h("h3", {}, "Animated GIF"), list(groups.gif), select("Add as", [["sequence", "An animated layer (all frames)"], ["object", "First frame as an object"]], () => choice.gif, (v) => (choice.gif = v), { record: false }));
  if (groups.video.length) body.append(h("h3", {}, `${groups.video.length} ${groups.video.length === 1 ? "video" : "videos"}`), list(groups.video), select("Add as", [["layer", "A video layer (keeps sound, adjustable)"], ["frames", "Drawable frames (rotoscope)"]], () => choice.video, (v) => (choice.video = v), { record: false }));
  if (groups.audio.length) body.append(h("h3", {}, "Sound"), list(groups.audio), h("p", { class: "hint" }, "Added as a sound layer you can trim, move and mix into MP4 / WebM exports."));
  if (groups.video.length || groups.audio.length || groups.gif.length || groups.image.length > 1) body.append(check("Make the timeline long enough for the clip", () => choice.extend, (v) => (choice.extend = v), { record: false }));
  if (groups.unknown.length) body.append(h("p", { class: "warn" }, `Can't import: ${groups.unknown.map((f) => f.name).join(", ")}`));
  body.append(h("p", { class: "hint" }, `Clips start at the current frame (${App.state.frame + 1}).`));
  const dlg = UI.dialog("Import", body, [btn("Cancel", () => dlg.close()), btn("Import", async () => {
    dlg.close();
    const prog = UI.progress("Importing…");
    try {
      if (groups.image.length || groups.svg.length) {
        const imgs = [...groups.image, ...groups.svg];
        if (choice.image === "sequence") await Media.addSequenceLayer(groups.image[0].name.replace(/[\d_\-.]*\.[^.]+$/, "") || "Sequence", await Media.imagesToFrames(groups.image), { fps: choice.seqFps, extend: choice.extend });
        else for (const f of imgs) choice.image === "layer" ? await Media.addImageLayer(f) : await Media.addImageObject(f);
      }
      for (const f of groups.gif) {
        const g = choice.gif === "sequence" ? await Media.gifFrames(f) : null;
        if (g) await Media.addSequenceLayer(f.name.replace(/\.[^.]+$/, ""), g.frames, { fps: Math.round(g.fps * 100) / 100, extend: choice.extend });
        else { await Media.addImageObject(f); if (choice.gif === "sequence") toast("This browser can't decode GIF animation, so the first frame was added."); }
      }
      for (const f of groups.video) {
        const L = await Media.addVideoLayer(f, { extend: choice.extend });
        if (choice.video === "frames") await Media.toDrawFrames(L, (p) => prog.set(p, "Converting video to frames…"));
      }
      for (const f of groups.audio) await Media.addAudioLayer(f, { extend: choice.extend });
      toast("Imported.");
    } catch (e) { toast(e.message || "Import failed."); }
    prog.close(); UI.refreshAll();
  }, "primary")]);
};

// ---------------------------------------------------------------- timeline controls
UI.syncTimelineControls = function () {
  const d = App.doc;
  $("fpsIn").value = d.fps; $("speedSel").value = String(d.speed); $("lenIn").value = d.frames;
  $("loopBtn").classList.toggle("on", d.loop); $("pingBtn").classList.toggle("on", d.pingpong);
  $("onionBtn").classList.toggle("on", d.onion.on);
  $("playBtn").innerHTML = App.state.playing ? '<svg viewBox="0 0 24 24"><path d="M7 5h4v14H7zM13 5h4v14h-4z" fill="currentColor"/></svg>' : '<svg viewBox="0 0 24 24"><path d="M7 4l13 8-13 8z" fill="currentColor"/></svg>';
  TL.updateReadout();
};
UI.bindTimelineControls = function () {
  $("playBtn").onclick = () => Player.toggle();
  $("firstBtn").onclick = () => App.goto(0);
  $("lastBtn").onclick = () => App.goto(App.doc.frames - 1);
  $("prevBtn").onclick = () => App.goto(App.state.frame - 1);
  $("nextBtn").onclick = () => App.goto(App.state.frame + 1);
  $("fpsIn").onchange = (e) => { History.push(); App.doc.fps = clamp(Math.round(+e.target.value || 12), 1, 60); changed("fps"); UI.syncTimelineControls(); };
  $("speedSel").onchange = (e) => { App.doc.speed = +e.target.value; changed("speed"); UI.syncTimelineControls(); };
  $("lenIn").onchange = (e) => { Frames.setLength(+e.target.value || App.doc.frames); UI.syncTimelineControls(); };
  $("loopBtn").onclick = () => { App.doc.loop = !App.doc.loop; UI.syncTimelineControls(); };
  $("pingBtn").onclick = () => { App.doc.pingpong = !App.doc.pingpong; UI.syncTimelineControls(); };
  $("onionBtn").onclick = () => { App.doc.onion.on = !App.doc.onion.on; View.redraw(); UI.syncTimelineControls(); };
  $("onionMore").onclick = (e) => UI.popup(e.clientX, e.clientY - 180, [
    ["1 frame each side", () => { App.doc.onion.before = App.doc.onion.after = 1; View.redraw(); }],
    ["2 frames each side", () => { App.doc.onion.before = App.doc.onion.after = 2; View.redraw(); }],
    ["3 frames each side", () => { App.doc.onion.before = App.doc.onion.after = 3; View.redraw(); }],
    ["Only previous frame", () => { App.doc.onion.before = 1; App.doc.onion.after = 0; View.redraw(); }],
    ["Stronger", () => { App.doc.onion.opacity = Math.min(0.8, App.doc.onion.opacity + 0.1); View.redraw(); }],
    ["Fainter", () => { App.doc.onion.opacity = Math.max(0.1, App.doc.onion.opacity - 0.1); View.redraw(); }]
  ]);
  $("keyBtn").onclick = () => Frames.blankKey();
  $("dupBtn").onclick = () => Frames.duplicateKey();
  $("holdBtn").onclick = () => Frames.insertHold();
  $("delFrameBtn").onclick = () => Frames.deleteFrame();
  $("tweenSel").onchange = (e) => Frames.setTween(e.target.value || null);
  $("exposureSel").onchange = (e) => { const v = e.target.value; e.target.value = ""; if (!v) return; if (v.startsWith("all")) Frames.retimeAll(+v.slice(3)); else Frames.setExposure(+v); };
  $("addLayerBtn").onclick = () => Layers.add();
  App.on("playing", UI.syncTimelineControls);
};

// ---------------------------------------------------------------- keyboard
UI.bindKeys = function () {
  window.addEventListener("keydown", (e) => {
    if (isTyping(e) || document.querySelector("dialog[open]")) {
      if (e.target && e.target.id === "textInput") { if (e.key === "Escape") TextTool.cancel(); if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) TextTool.commit(); }
      return;
    }
    const k = e.key, mod = e.ctrlKey || e.metaKey;
    if (mod && k.toLowerCase() === "z") { e.preventDefault(); e.shiftKey ? History.doRedo() : History.doUndo(); return; }
    if (mod && k.toLowerCase() === "y") { e.preventDefault(); History.doRedo(); return; }
    if (mod && k.toLowerCase() === "c") { Cmd.copy(); return; }
    if (mod && k.toLowerCase() === "v") { e.preventDefault(); Cmd.paste(true); return; }
    if (mod && k.toLowerCase() === "g") { e.preventDefault(); e.shiftKey ? Cmd.ungroup() : Cmd.group(); return; }
    if (mod && k.toLowerCase() === "m") { e.preventDefault(); Cmd.merge("unite"); return; }
    if (mod && e.shiftKey && k.toLowerCase() === "d") { e.preventDefault(); UI.repeatDialog(); return; }
    if (mod && k.toLowerCase() === "d") { e.preventDefault(); Cmd.duplicate(); return; }
    if (mod && k.toLowerCase() === "a") { e.preventDefault(); Cmd.selectAll(); return; }
    if (mod && k.toLowerCase() === "s") { e.preventDefault(); App.Exporter.saveProject(); return; }
    if (mod && k.toLowerCase() === "e") { e.preventDefault(); App.Exporter.open(); return; }
    if (k === "Enter") { e.preventDefault(); if (App.state.tool === "pen") Tools.get("pen").finish(); else Player.toggle(); return; }
    if (!mod && k.toLowerCase() === "q") { e.preventDefault(); App.Mask.current ? App.Mask.clear() : App.Mask.fromSelection(true); return; }
    if (k === "Escape") { Tools.get("pen").finish(); App.state.selection = []; App.emit("selection"); View.redraw(); return; }
    if (k === "Delete" || k === "Backspace") { e.preventDefault(); if (App.state.tool === "direct" && Tools.get("direct").activeNode >= 0) Cmd.deleteNode(); else Cmd.delete(); return; }
    if (k.startsWith("Arrow")) { e.preventDefault(); const s = e.shiftKey ? 10 : 1; Cmd.nudge(k === "ArrowLeft" ? -s : k === "ArrowRight" ? s : 0, k === "ArrowUp" ? -s : k === "ArrowDown" ? s : 0); return; }
    if (k === "," || k === "<") { if (e.shiftKey) jumpKey(-1); else App.goto(App.state.frame - 1); return; }
    if (k === "." || k === ">") { if (e.shiftKey) jumpKey(1); else App.goto(App.state.frame + 1); return; }
    if (k === "F5") { e.preventDefault(); Frames.insertHold(); return; }
    if (k === "F6") { e.preventDefault(); Frames.blankKey(); return; }
    if (k === "F7") { e.preventDefault(); Frames.duplicateKey(); return; }
    if (k === "+" || k === "=") { View.zoomAt(1.25); return; }
    if (k === "-") { View.zoomAt(0.8); return; }
    if (k === "0") { View.fit(); return; }
    const t = TOOLS.find((x) => x[2] === k.toLowerCase());
    if (t && !mod) App.setTool(t[0]);
  });
};
function jumpKey(dir) {
  const L = activeLayer(); if (!L || !L.cells) return;
  let f = App.state.frame + dir;
  while (f >= 0 && f < L.cells.length && !L.cells[f]) f += dir;
  if (f >= 0 && f < L.cells.length) App.goto(f);
}

UI.refreshAll = function () { UI.buildToolbar(); UI.buildProps(); UI.syncTimelineControls(); TL.buildHeads(); TL.draw(); $("docName").textContent = App.doc.name; View.redraw(); };
