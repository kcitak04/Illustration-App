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
  sw.onclick = () => UI.openProps();
  const sb = h("button", { class: "tool symbtn", type: "button", "aria-label": "Symmetry", html: ICON.sym, onclick: (e) => { const r = e.currentTarget.getBoundingClientRect(); window.innerWidth <= 900 ? UI.symmetryMenu(r.left, r.bottom + 6) : UI.symmetryMenu(r.right + 6, r.top); } });
  bar.append(sb, sw);
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
    if (editSel) { const cell = editableCell(); const ids = new Set(App.state.selection); cell.objects.forEach((o) => { if (ids.has(o.id)) o.fill = v; }); changed("fill"); }
    UI.syncSwatch();
  };
  const applyStroke = (v) => { s.strokeOn = !!v; if (v) s.stroke = v; if (editSel) { const cell = editableCell(); const ids = new Set(App.state.selection); cell.objects.forEach((o) => { if (ids.has(o.id)) { o.stroke = v; if (v && !o.sw) o.sw = s.sw; } }); changed("stroke"); } UI.syncSwatch(); };
  const first = editSel ? sel[0] : null;
  const fillNow = first ? (typeof first.fill === "string" ? first.fill : null) : (s.grad ? null : s.fill);
  const colorSec = section("Colour",
    colorRow(t === "paint" || t === "bucket" ? "Paint" : "Fill", () => (t === "paint" || t === "bucket" ? s.paint.color : fillNow), (v) => { if (t === "paint" || t === "bucket") { s.paint.color = v || s.paint.color; s.fill = v || s.fill; UI.syncSwatch(); } else applyFill(v); }, { allowNone: !["paint", "bucket", "vbrush"].includes(t), record: !!editSel }),
    colorRow("Stroke", () => (first ? first.stroke : s.strokeOn ? s.stroke : null), applyStroke, { allowNone: true, record: !!editSel }),
    slider("Stroke width", 0, 80, 0.5, () => (first ? first.sw || 0 : s.sw), (v) => { s.sw = v; if (editSel) { const cell = editableCell(); const ids = new Set(App.state.selection); cell.objects.forEach((o) => { if (ids.has(o.id)) o.sw = v; }); changed("sw"); } }, { record: !!editSel }),
    h("div", { class: "row" }, btn("Gradient…", () => UI.gradientDialog()), s.grad ? h("span", { class: "hint" }, `${s.grad.type} gradient on`) : null),
    UI.recentSwatches()
  );
  box.append(colorSec);

  // --- tool options
  const opts = [];
  if (t === "vbrush") opts.push(slider("Size", 1, 300, 1, () => s.brush.size, (v) => (s.brush.size = v), { record: false }), slider("Pressure", 0, 1, 0.01, () => s.brush.pressure, (v) => (s.brush.pressure = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }), slider("Smoothing", 0, 1, 0.01, () => s.brush.smooth, (v) => (s.brush.smooth = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }), check("Tapered ends", () => s.brush.taper, (v) => (s.brush.taper = v), { record: false }));
  if (t === "paint") opts.push(slider("Size", 1, 400, 1, () => s.paint.size, (v) => (s.paint.size = v), { record: false }), slider("Hardness", 0, 1, 0.01, () => s.paint.hardness, (v) => (s.paint.hardness = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }), slider("Opacity", 0.05, 1, 0.01, () => s.paint.opacity, (v) => (s.paint.opacity = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }), slider("Flow", 0.02, 1, 0.01, () => s.paint.flow, (v) => (s.paint.flow = v), { record: false, fmt: (v) => Math.round(v * 100) + "%" }), check("Pressure changes size", () => s.paint.pressureSize, (v) => (s.paint.pressureSize = v), { record: false }), check("Pressure changes opacity", () => s.paint.pressureOpacity, (v) => (s.paint.pressureOpacity = v), { record: false }));
  if (t === "eraser") {
    const hints = {
      all: "Rubs out painted pixels and cuts through vector shapes and lines where you erase.",
      pixels: "Only rubs out painted pixels; vector shapes stay untouched.",
      cut: "Cuts away just the part of vector shapes and lines you erase over. Painted pixels stay.",
      objects: "Removes every vector shape or line you touch, as a whole."
    };
    const hint = h("p", { class: "hint" }, hints[s.eraser.mode] || hints.all);
    opts.push(slider("Size", 2, 400, 1, () => s.eraser.size, (v) => (s.eraser.size = v), { record: false }),
      select("Erases", [["all", "Everything (pixels + vectors)"], ["pixels", "Painted pixels only"], ["cut", "Parts of vectors"], ["objects", "Whole vector objects"]], () => s.eraser.mode, (v) => { s.eraser.mode = v; hint.textContent = hints[v]; UI.syncSymButton(); }, { record: false }),
      hint, h("p", { class: "hint" }, "Tip: flip your stylus to use its eraser end with any tool."));
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
  if (opts.length) box.append(section("Tool", ...opts));
  if (SYM_TOOLS.includes(t)) box.append(UI.symmetrySection());

  // --- selection
  if (editSel) {
    const o = first;
    const kids = [
      slider("Opacity", 0, 1, 0.01, () => o.opacity ?? 1, (v) => { const c = editableCell(); const ids = new Set(App.state.selection); c.objects.forEach((x) => { if (ids.has(x.id)) x.opacity = v; }); changed("op"); }, { fmt: (v) => Math.round(v * 100) + "%" }),
      select("Blend", BLENDS, () => o.blend || "source-over", (v) => { const c = editableCell(); const ids = new Set(App.state.selection); c.objects.forEach((x) => { if (ids.has(x.id)) x.blend = v; }); changed("blend"); }),
      h("div", { class: "btnrow" }, btn("Duplicate", () => Cmd.duplicate()), btn("Repeat…", () => UI.repeatDialog()), btn("Delete", () => Cmd.delete())),
      h("div", { class: "btnrow" }, btn("Mirror copy ↔", () => Cmd.mirrorCopy(true)), btn("Mirror copy ↕", () => Cmd.mirrorCopy(false)), btn("To path", () => Cmd.toPath())),
      h("div", { class: "btnrow" }, btn("Group", () => Cmd.group()), btn("Ungroup", () => Cmd.ungroup())),
      h("p", { class: "hint" }, "Merge shapes into one:"),
      h("div", { class: "btnrow" }, btn("Unite", () => Cmd.merge("unite")), btn("Subtract", () => Cmd.merge("subtract")), btn("Intersect", () => Cmd.merge("intersect")), btn("Exclude", () => Cmd.merge("exclude"))),
      h("div", { class: "btnrow" }, btn("Front", () => Cmd.arrange("front")), btn("Forward", () => Cmd.arrange("forward")), btn("Backward", () => Cmd.arrange("backward")), btn("Back", () => Cmd.arrange("back"))),
      h("div", { class: "btnrow" }, btn("Flip ↔", () => Cmd.flip(true)), btn("Flip ↕", () => Cmd.flip(false)), btn("Rotate 90°", () => Cmd.rotate(90))),
      h("div", { class: "btnrow" }, btn("Align left", () => Cmd.align("l", null)), btn("Centre", () => Cmd.align("c", "m")), btn("Align right", () => Cmd.align("r", null))),
      h("div", { class: "btnrow" }, btn("Align top", () => Cmd.align(null, "t")), btn("Middle", () => Cmd.align(null, "m")), btn("Align bottom", () => Cmd.align(null, "b")))
    ];
    if (o.type === "text") kids.unshift(btn("Edit text", () => TextTool.edit(o)), slider("Text size", 8, 600, 1, () => o.size, (v) => { const c = editableCell(); c.objects.forEach((x) => { if (x.id === o.id) x.size = v; }); changed("size"); }));
    const grouped = sel.length > 1 && sel[0].group && sel.every((x) => x.group === sel[0].group);
    box.append(section(`Selection · ${grouped ? "group of " : ""}${sel.length} ${sel.length === 1 ? "object" : "objects"}`, ...kids));
  }

  // --- layer
  if (L) {
    const kids = [
      slider("Layer opacity", 0, 1, 0.01, () => L.opacity, (v) => { L.opacity = v; changed("layer"); }, { fmt: (v) => Math.round(v * 100) + "%" }),
      select("Layer blend", BLENDS, () => L.blend || "source-over", (v) => { L.blend = v; changed("layer"); }),
      h("div", { class: "btnrow" }, btn("New layer", () => Layers.add()), btn("Duplicate", () => Layers.duplicate()), btn("Delete", () => Layers.remove())),
      h("div", { class: "btnrow" }, btn("Move up", () => Layers.move(1)), btn("Move down", () => Layers.move(-1)))
    ];
    box.append(section(`Layer · ${L.name}`, ...kids));
  }
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

UI.syncSwatch = function () {
  const f = document.querySelector(".sw.fill"), s = document.querySelector(".sw.stroke");
  if (f) f.style.background = App.state.tool === "paint" ? App.style.paint.color : typeof App.style.fill === "string" ? App.style.fill : "#888";
  if (s) s.style.borderColor = App.style.strokeOn ? App.style.stroke : "transparent";
};
const recent = ["#111827", "#ffffff", "#e11d48", "#f97316", "#facc15", "#22c55e", "#06b6d4", "#3b82f6", "#8b5cf6", "#ec4899", "#78350f", "#94a3b8"];
UI.recentSwatches = function () {
  return h("div", { class: "palette" }, recent.map((c) => h("button", { class: "chip", type: "button", title: c, style: `background:${c}`, onclick: () => {
    const t = App.state.tool;
    if (["select", "direct"].includes(t) && App.state.selection.length) Cmd.editSelected((o) => { o.fill = c; });
    App.style.fill = c; App.style.grad = null; App.style.paint.color = c; UI.syncSwatch(); UI.buildProps();
  } })));
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

UI.docSettings = function () {
  const d = App.doc, v = { name: d.name, w: d.width, h: d.height, fps: d.fps, frames: d.frames, speed: d.speed, bg: d.background, transparent: d.transparent };
  const presets = [["", "Custom"], ["1080x1080", "Square 1080 × 1080"], ["1920x1080", "HD 1920 × 1080"], ["1080x1920", "Vertical 1080 × 1920"], ["3000x3000", "Album cover 3000 × 3000"], ["1280x720", "720p 1280 × 720"], ["800x600", "800 × 600"], ["512x512", "Sticker 512 × 512"]];
  const wIn = h("input", { type: "number", value: v.w, min: 16, max: 4096, id: "dsW" }), hIn = h("input", { type: "number", value: v.h, min: 16, max: 4096, id: "dsH" });
  const body = h("div", {},
    h("div", { class: "row" }, h("label", { for: "dsName" }, "Name"), h("input", { id: "dsName", value: v.name, oninput: (e) => (v.name = e.target.value) })),
    select("Canvas preset", presets, () => `${v.w}x${v.h}`, (p) => { if (!p) return; const [a, b] = p.split("x").map(Number); wIn.value = a; hIn.value = b; }, { record: false }),
    h("div", { class: "row" }, h("label", { for: "dsW" }, "Width"), wIn), h("div", { class: "row" }, h("label", { for: "dsH" }, "Height"), hIn),
    numberField("Frame rate (fps)", () => v.fps, (x) => (v.fps = x), { min: 1, max: 60, record: false }),
    numberField("Length (frames)", () => v.frames, (x) => (v.frames = x), { min: 1, max: 7200, record: false }),
    select("Playback speed", [[0.25, "0.25×"], [0.5, "0.5×"], [0.75, "0.75×"], [1, "1× (normal)"], [1.25, "1.25×"], [1.5, "1.5×"], [2, "2×"], [3, "3×"], [4, "4×"]], () => v.speed, (x) => (v.speed = +x), { record: false }),
    colorRow("Background", () => v.bg, (x) => (v.bg = x || v.bg), { record: false }),
    check("Transparent background", () => v.transparent, (x) => (v.transparent = x), { record: false })
  );
  const dlg = UI.dialog("Animation settings", body, [btn("Cancel", () => dlg.close()), btn("Apply", () => {
    History.push();
    const nw = clamp(+wIn.value || d.width, 16, 4096), nh = clamp(+hIn.value || d.height, 16, 4096);
    if (nw !== d.width || nh !== d.height) {
      for (const L of d.layers) if (L.cells) for (const c of L.cells) if (c && c.raster) { const n = document.createElement("canvas"); n.width = nw; n.height = nh; n.getContext("2d").drawImage(c.raster, 0, 0); c.raster = n; }
      d.width = nw; d.height = nh;
    }
    d.name = v.name.trim() || d.name; d.fps = clamp(Math.round(v.fps), 1, 60); d.speed = v.speed; d.background = v.bg; d.transparent = v.transparent;
    if (v.frames !== d.frames) Frames.setLength(v.frames);
    changed("doc"); View.fit(); UI.syncTimelineControls(); dlg.close();
  }, "primary")]);
};

UI.help = function () {
  const rows = [["Space + drag / two fingers", "Pan"], ["Ctrl + wheel / pinch", "Zoom"], ["Enter", "Play / stop"], [", and .", "Previous / next frame"], ["Shift + , and .", "Previous / next key"],
    ["F6", "New blank key"], ["F7", "Duplicate key"], ["F5", "Hold one more frame"], ["Ctrl+Z / Ctrl+Shift+Z", "Undo / redo"], ["Two-finger tap", "Undo"],
    ["Ctrl+C / V / D", "Copy / paste / duplicate objects"], ["Ctrl+Shift+D", "Repeat copies…"], ["Ctrl+G / Ctrl+Shift+G", "Group / ungroup"], ["Ctrl+M", "Merge (unite) shapes"], ["Delete", "Delete selection"], ["Arrows (Shift = 10px)", "Nudge"], ["Stylus eraser end", "Erase"], ["Stylus side button", "Eyedropper"]];
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
