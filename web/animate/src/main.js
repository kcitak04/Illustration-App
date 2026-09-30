// ============================================================================
// main.js — start-up: demo animation, autosave restore, top bar
// ============================================================================
function demoDoc() {
  const d = newDoc(1080, 1080, 12, 24);
  d.name = "Bouncing ball (example)";
  d.background = "#f4efe6";
  const ground = { id: uid(), type: "rect", w: 1080, h: 260, r: 0, m: M.translate(0, 820), fill: { type: "linear", c1: "#2b2d42", c2: "#1b1c2b", angle: 90 }, stroke: null, sw: 0, opacity: 1 };
  const title = { id: uid(), type: "text", text: "Frame Studio", font: "Anton", size: 120, weight: 400, align: "center", m: M.translate(540, 110), fill: "#2b2d42", stroke: null, sw: 0, opacity: 1 };
  const bg = d.layers[0]; bg.name = "Background"; bg.cells[0].objects = [ground, title]; bg.locked = true;
  const ball = newDrawLayer("Ball", d.frames);
  const id = uid(), sid = uid();
  const mk = (x, y, sx, sy) => ({ id, type: "ellipse", w: 200, h: 200, m: M.mul(M.translate(x, y), M.scale(sx, sy, 100, 200)), fill: { type: "radial", c1: "#ff8a5b", c2: "#e63946", angle: 0 }, stroke: "#1b1c2b", sw: 6, opacity: 1 });
  const sh = (w, o) => ({ id: sid, type: "ellipse", w, h: 40, m: M.translate(540 - w / 2, 800), fill: "rgba(0,0,0,0.25)", stroke: null, sw: 0, opacity: o });
  ball.cells[0] = { key: true, objects: [sh(90, 0.4), mk(440, 180, 1, 1)], raster: null, tween: "easeIn" };
  ball.cells[11] = { key: true, objects: [sh(260, 1), mk(440, 620, 1.25, 0.8)], raster: null, tween: "easeOut" };
  ball.cells[12] = { key: true, objects: [sh(240, 1), mk(440, 610, 1.15, 0.85)], raster: null, tween: "easeOut" };
  ball.cells[23] = { key: true, objects: [sh(90, 0.4), mk(440, 180, 1, 1)], raster: null, tween: null };
  d.layers.push(ball);
  return d;
}

function newAnimation() {
  const sel = { size: "1080x1080", fps: 12 };
  const dlg = UI.dialog("New animation", h("div", {},
    h("p", { class: "hint" }, "This replaces what's open. Save the project first if you want to keep it."),
    select("Size", [["1080x1080", "Square 1080 × 1080"], ["1920x1080", "HD 1920 × 1080"], ["1080x1920", "Vertical 1080 × 1920"], ["1280x720", "720p"], ["3000x3000", "Album cover 3000 × 3000"], ["512x512", "Sticker 512 × 512"]], () => "1080x1080", (v) => (sel.size = v), { record: false }),
    select("Frame rate", [[8, "8 fps"], [12, "12 fps (animation on twos)"], [24, "24 fps (film)"], [30, "30 fps"], [60, "60 fps"]], () => 12, (v) => (sel.fps = +v), { record: false })
  ), [btn("Cancel", () => dlg.close()), btn("Create", () => {
    const [w, hh] = sel.size.split("x").map(Number);
    for (const L of App.doc.layers) if (L.media && L.media.el && L.media.el.pause) L.media.el.pause();
    App.doc = newDoc(w, hh, sel.fps, sel.fps * 2); App.state.frame = 0; App.state.layer = 0; App.state.selection = []; History.clear();
    changed("new"); View.fit(); UI.refreshAll(); App.setTool("vbrush"); dlg.close();
  }, "primary")]);
}

function fileMenu(e) {
  UI.popup(e.clientX, e.clientY + 10, [
    ["New animation…", newAnimation],
    ["Open project…", () => $("projectFile").click()],
    ["Save project (Ctrl+S)", () => Exporter.saveProject()],
    ["-"],
    ["Import files…", () => $("importFile").click()],
    ["Export… (Ctrl+E)", () => Exporter.open()],
    ["-"],
    ["Animation settings…", () => UI.docSettings()],
    ["Stylus: " + (App.settings.palm ? "fingers only pan (on)" : "fingers draw too"), () => { App.settings.palm = !App.settings.palm; toast(App.settings.palm ? "Once a stylus is used, fingers only pan and zoom." : "Fingers draw as well as the stylus."); }],
    ["Keyboard shortcuts", () => UI.help()]
  ]);
}

/** Android back button: close the top-most popup / dialog / panel; returns false to leave the app. */
window.__androidBack = function () {
  if (document.querySelector(".popup")) { UI.closePopup(); return true; }
  const dlgs = [...document.querySelectorAll("dialog[open]")];
  if (dlgs.length) { dlgs[dlgs.length - 1].close(); return true; }
  if (!$("textEditor").hidden) { TextTool.cancel(); return true; }
  if (document.body.classList.contains("props-open")) { UI.closeProps(); return true; }
  if (App.state.playing) { Player.stop(); return true; }
  Autosave.save();
  return false;
};
/** Called by the Android app when it goes to the background. */
window.__androidPause = function () { if (App.state.playing) Player.stop(); Autosave.save(); };

async function start() {
  App.doc = demoDoc();
  App.state.layer = 1; App.state.tool = "select";
  View.init(); UI.buildToolbar(); UI.bindTimelineControls(); UI.bindKeys(); TL.init();
  App.on("selection", () => { UI.buildProps(); View.redraw(); });
  App.on("layers", () => { UI.buildProps(); TL.updateReadout(); });
  App.on("style", () => { UI.buildProps(); UI.syncSwatch(); });
  App.on("frame", () => { if (!App.state.playing) UI.buildPropsSoon(); });
  App.on("changed", (w) => { $("undoBtn").disabled = !History.undo.length; $("redoBtn").disabled = !History.redo.length; if (["history", "load", "layers", "frames", "import", "rotoscope", "new"].includes(w)) { UI.buildPropsSoon(); UI.syncTimelineControls(); $("docName").textContent = App.doc.name; } });
  App.on("zoom", () => { $("zoomOut").textContent = Math.round(App.state.zoom * 100) + "%"; });
  App.on("saved", () => { $("saveState").textContent = "Saved in this browser"; });
  let t = 0; UI.buildPropsSoon = () => { clearTimeout(t); t = setTimeout(() => UI.buildProps(), 60); };

  $("fileBtn").onclick = fileMenu;
  $("undoBtn").onclick = () => History.doUndo();
  $("redoBtn").onclick = () => History.doRedo();
  $("importBtn").onclick = () => $("importFile").click();
  $("exportBtn").onclick = () => Exporter.open();
  $("settingsBtn").onclick = () => UI.docSettings();
  $("zoomOut").onclick = () => View.fit();
  $("zoomMinus").onclick = () => View.zoomAt(0.8);
  $("zoomPlus").onclick = () => View.zoomAt(1.25);
  $("propsToggle").onclick = () => document.body.classList.toggle("props-open");
  $("propsClose").onclick = () => UI.closeProps();
  $("importFile").onchange = (e) => { UI.importFiles(e.target.files || []); e.target.value = ""; };
  $("projectFile").onchange = async (e) => {
    const f = e.target.files && e.target.files[0]; e.target.value = ""; if (!f) return;
    const prog = UI.progress("Opening project…");
    try { await Project.load(JSON.parse(await f.text())); View.fit(); UI.refreshAll(); toast("Project opened."); } catch (err) { toast(err.message || "Couldn't open that file."); }
    prog.close();
  };
  $("textOk").onclick = () => TextTool.commit();
  $("textCancel").onclick = () => TextTool.cancel();
  // drop files anywhere
  document.addEventListener("dragover", (e) => { e.preventDefault(); document.body.classList.add("dropping"); });
  document.addEventListener("dragleave", (e) => { if (!e.relatedTarget) document.body.classList.remove("dropping"); });
  document.addEventListener("drop", (e) => { e.preventDefault(); document.body.classList.remove("dropping"); if (e.dataTransfer.files.length) UI.importFiles(e.dataTransfer.files); });
  window.addEventListener("paste", (e) => { const files = [...(e.clipboardData?.files || [])]; if (files.length) UI.importFiles(files); });

  UI.refreshAll();
  App.setTool("select");
  App.state.selection = [];
  // offer to bring back the last session
  const saved = await Autosave.load();
  if (saved && saved.doc) {
    const bar = $("restoreBar"); bar.hidden = false;
    $("restoreName").textContent = saved.doc.name || "your animation";
    $("restoreYes").onclick = async () => { bar.hidden = true; const p = UI.progress("Restoring…"); try { await Project.load(saved); View.fit(); UI.refreshAll(); } catch (e) { toast("Couldn't restore: " + e.message); } p.close(); };
    $("restoreNo").onclick = () => { bar.hidden = true; };
  }
  if (document.fonts) document.fonts.ready.then(() => changed("fonts"));
}
start();
