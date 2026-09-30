// ============================================================================
// media.js — imported images, videos, GIFs, image sequences and audio;
// per-clip settings (timing, placement, look, sound); project save / load
// ============================================================================
const Media = (App.Media = {});

Media.defaults = (start = 0, duration = 0) => ({
  start,                // timeline frame where the clip begins
  trimIn: 0, trimOut: duration,
  speed: 1, reverse: false, loop: false,
  hold: 1,              // show each sampled frame for N timeline frames ("on twos" = 2)
  seqFps: 0,            // image sequences: playback fps (0 = project fps)
  fit: "contain", x: 0, y: 0, scale: 1, rotation: 0, flipX: false, flipY: false,
  brightness: 100, contrast: 100, saturation: 100, hue: 0, blur: 0, grayscale: 0, sepia: 0, invert: 0,
  volume: 1, muted: false
});

// ---------------------------------------------------------------- timing
/** Seconds into the source that timeline frame f shows, or null when the clip isn't on screen. */
Media.timeAt = function (L, f) {
  const s = L.settings, m = L.media, fps = App.doc.fps;
  if (f < s.start) return null;
  let fr = f - s.start;
  if (s.hold > 1) fr = Math.floor(fr / s.hold) * s.hold;
  let local = fr / fps;
  const len = Math.max(0, s.trimOut - s.trimIn) / Math.max(0.01, s.speed);
  if (m.type === "image") return 0;
  if (len <= 0) return null;
  if (local >= len - 1e-6) { if (!s.loop) return null; local %= len; }
  return s.reverse ? Math.max(s.trimIn, s.trimOut - local * s.speed - 1e-3) : s.trimIn + local * s.speed;
};
/** Number of timeline frames the clip covers (Infinity when looping). */
Media.lengthFrames = function (L) {
  const s = L.settings, m = L.media;
  if (m.type === "image") return Infinity;
  if (s.loop) return Infinity;
  return Math.ceil(Math.max(0, s.trimOut - s.trimIn) / Math.max(0.01, s.speed) * App.doc.fps);
};

// ---------------------------------------------------------------- look (filters)
let canvasFilterOk = null;
function supportsCanvasFilter() {
  if (canvasFilterOk !== null) return canvasFilterOk;
  try {
    const c = document.createElement("canvas"); c.width = c.height = 2;
    const g = c.getContext("2d"); g.fillStyle = "#fff"; g.fillRect(0, 0, 2, 2);
    g.filter = "invert(1)"; g.drawImage(c, 0, 0);
    canvasFilterOk = g.getImageData(0, 0, 1, 1).data[0] < 128;
  } catch (e) { canvasFilterOk = false; }
  return canvasFilterOk;
}
function filterString(s) {
  const parts = [];
  if (s.brightness !== 100) parts.push(`brightness(${s.brightness}%)`);
  if (s.contrast !== 100) parts.push(`contrast(${s.contrast}%)`);
  if (s.saturation !== 100) parts.push(`saturate(${s.saturation}%)`);
  if (s.hue) parts.push(`hue-rotate(${s.hue}deg)`);
  if (s.blur) parts.push(`blur(${s.blur}px)`);
  if (s.grayscale) parts.push(`grayscale(${s.grayscale}%)`);
  if (s.sepia) parts.push(`sepia(${s.sepia}%)`);
  if (s.invert) parts.push(`invert(${s.invert}%)`);
  return parts.join(" ") || "none";
}
/** Pixel fallback for browsers without canvas filters (Safari): brightness / contrast / saturation / greyscale / sepia / invert. */
function manualAdjust(c, s) {
  const g = c.getContext("2d"); const img = g.getImageData(0, 0, c.width, c.height); const p = img.data;
  const br = s.brightness / 100, ct = s.contrast / 100, sat = s.saturation / 100, gr = s.grayscale / 100, sp = s.sepia / 100, iv = s.invert / 100;
  for (let i = 0; i < p.length; i += 4) {
    let r = p[i] * br, gg = p[i + 1] * br, b = p[i + 2] * br;
    r = (r - 128) * ct + 128; gg = (gg - 128) * ct + 128; b = (b - 128) * ct + 128;
    const l = 0.2126 * r + 0.7152 * gg + 0.0722 * b;
    r = l + (r - l) * sat * (1 - gr); gg = l + (gg - l) * sat * (1 - gr); b = l + (b - l) * sat * (1 - gr);
    if (sp) { const sr = r * .393 + gg * .769 + b * .189, sg = r * .349 + gg * .686 + b * .168, sb = r * .272 + gg * .534 + b * .131; r = lerp(r, sr, sp); gg = lerp(gg, sg, sp); b = lerp(b, sb, sp); }
    if (iv) { r = lerp(r, 255 - r, iv); gg = lerp(gg, 255 - gg, iv); b = lerp(b, 255 - b, iv); }
    p[i] = r; p[i + 1] = gg; p[i + 2] = b;
  }
  g.putImageData(img, 0, 0);
}

// ---------------------------------------------------------------- drawing a media layer
const adjBuf = document.createElement("canvas");
Media.placement = function (L) {
  const d = App.doc, s = L.settings, m = L.media;
  const sw = m.w || 1, sh = m.h || 1;
  let k = 1, kx, ky;
  if (s.fit === "contain") k = Math.min(d.width / sw, d.height / sh);
  else if (s.fit === "cover") k = Math.max(d.width / sw, d.height / sh);
  kx = k; ky = k;
  if (s.fit === "stretch") { kx = d.width / sw; ky = d.height / sh; }
  let mtx = M.translate(-sw / 2, -sh / 2);
  mtx = M.mul(M.scale(kx * s.scale * (s.flipX ? -1 : 1), ky * s.scale * (s.flipY ? -1 : 1)), mtx);
  mtx = M.mul(M.rotate(s.rotation * Math.PI / 180), mtx);
  mtx = M.mul(M.translate(d.width / 2 + s.x, d.height / 2 + s.y), mtx);
  return mtx;
};
function sourceFor(L, f) {
  const m = L.media, t = Media.timeAt(L, f);
  if (t === null) return null;
  if (m.type === "image") return m.el;
  if (m.type === "video") return m.el.readyState >= 2 ? m.el : null;
  if (m.type === "sequence") {
    const fps = L.settings.seqFps || App.doc.fps;
    const i = clamp(Math.floor(t * fps + 1e-6), 0, m.frames.length - 1);
    return m.frames[i];
  }
  return null;
}
Media.draw = function (ctx, L, f) {
  if (!L.media || L.media.type === "audio") return;
  const src = sourceFor(L, f); if (!src) return;
  const s = L.settings, m = L.media;
  const mtx = Media.placement(L);
  const needsAdjust = filterString(s) !== "none";
  ctx.save();
  ctx.transform(...mtx);
  if (needsAdjust && supportsCanvasFilter()) { ctx.filter = filterString(s); ctx.drawImage(src, 0, 0, m.w, m.h); }
  else if (needsAdjust) {
    if (adjBuf.width !== m.w || adjBuf.height !== m.h) { adjBuf.width = m.w; adjBuf.height = m.h; }
    const g = adjBuf.getContext("2d"); g.clearRect(0, 0, m.w, m.h); g.drawImage(src, 0, 0, m.w, m.h);
    manualAdjust(adjBuf, s); ctx.drawImage(adjBuf, 0, 0);
  } else ctx.drawImage(src, 0, 0, m.w, m.h);
  ctx.restore();
};

// ---------------------------------------------------------------- live playback sync (preview)
Media.sync = function (f, playing) {
  for (const L of App.doc.layers) {
    const m = L.media; if (!m || !m.el || !(m.type === "video" || m.type === "audio")) continue;
    const s = L.settings, el = m.el, t = Media.timeAt(L, f);
    el.muted = !playing || s.muted || !L.visible;
    el.volume = clamp(s.volume, 0, 1);
    if (t === null) { if (!el.paused) el.pause(); continue; }
    const smooth = playing && !s.reverse && s.hold <= 1 && s.speed >= 0.0625 && s.speed <= 16;
    if (smooth) {
      const rate = s.speed * App.doc.speed;
      if (el.playbackRate !== rate) el.playbackRate = clamp(rate, 0.0625, 16);
      if (el.paused) { el.currentTime = t; el.play().catch(() => {}); }
      else if (Math.abs(el.currentTime - t) > 0.25) el.currentTime = t;
    } else {
      if (!el.paused) el.pause();
      if (!el.seeking && Math.abs(el.currentTime - t) > 1 / 120) el.currentTime = t;
    }
  }
};
Media.pauseAll = function () { for (const L of App.doc.layers) if (L.media && L.media.el && L.media.el.pause) { L.media.el.pause(); L.media.el.muted = true; } };

/** Seeks every visible video to its exact time for frame f (used by export and rotoscoping). */
Media.prepareFrame = async function (f) {
  const jobs = [];
  for (const L of App.doc.layers) {
    const m = L.media; if (!m || m.type !== "video" || !L.visible) continue;
    const t = Media.timeAt(L, f); if (t === null) continue;
    jobs.push(seekExact(m.el, t));
  }
  await Promise.all(jobs);
};
function seekExact(el, t) {
  return new Promise((res) => {
    if (!el.paused) el.pause();
    const target = clamp(t, 0, Math.max(0, (el.duration || 0) - 1e-3));
    if (Math.abs(el.currentTime - target) < 1e-4 && el.readyState >= 2) return res();
    let done = false;
    const finish = () => { if (done) return; done = true; el.removeEventListener("seeked", onSeek); clearTimeout(to); res(); };
    const onSeek = () => { if (el.requestVideoFrameCallback) el.requestVideoFrameCallback(() => finish()); setTimeout(finish, 60); };
    const to = setTimeout(finish, 3000);
    el.addEventListener("seeked", onSeek);
    el.currentTime = target;
  });
}

// ---------------------------------------------------------------- importing
function fileKind(f) {
  const t = f.type || "", n = (f.name || "").toLowerCase();
  if (t === "image/gif" || n.endsWith(".gif")) return "gif";
  if (t === "image/svg+xml" || n.endsWith(".svg")) return "svg";
  if (t.startsWith("image/") || /\.(png|jpe?g|webp|avif|bmp|heic|heif)$/.test(n)) return "image";
  if (t.startsWith("video/") || /\.(mp4|mov|webm|m4v|mkv|ogv)$/.test(n)) return "video";
  if (t.startsWith("audio/") || /\.(mp3|wav|m4a|aac|flac|ogg|oga|opus|aiff?)$/.test(n)) return "audio";
  if (t === "application/json" || n.endsWith(".json")) return "project";
  return "unknown";
}
Media.fileKind = fileKind;

async function loadImageEl(blob) {
  const url = URL.createObjectURL(blob);
  const im = new Image(); im.decoding = "async";
  await new Promise((res, rej) => { im.onload = res; im.onerror = () => rej(new Error("Couldn't read that image.")); im.src = url; });
  return im;
}
function toCanvas(src, w, h, max = 4096) {
  const k = Math.min(1, max / Math.max(w, h));
  const c = document.createElement("canvas"); c.width = Math.max(1, Math.round(w * k)); c.height = Math.max(1, Math.round(h * k));
  const g = c.getContext("2d"); g.imageSmoothingQuality = "high"; g.drawImage(src, 0, 0, c.width, c.height); return c;
}

/** Places an image as a movable object in the current frame of the active draw layer. */
Media.addImageObject = async function (file) {
  const im = await loadImageEl(file);
  const c = toCanvas(im, im.naturalWidth || 512, im.naturalHeight || 512, 2048);
  const d = App.doc;
  const k = Math.min(1, 0.8 * Math.min(d.width / c.width, d.height / c.height));
  History.push();
  ensureDrawLayer();
  const cell = editableCell();
  const o = { id: uid(), type: "image", src: c.toDataURL("image/png"), w: c.width, h: c.height, m: M.mul(M.translate((d.width - c.width * k) / 2, (d.height - c.height * k) / 2), M.scale(k, k)), opacity: 1 };
  cell.objects.push(o);
  App.state.selection = [o.id]; App.setTool && App.setTool("select");
  changed("import");
};
function ensureDrawLayer() {
  if (activeLayer() && activeLayer().kind === "draw") return;
  const i = App.doc.layers.findIndex((L) => L.kind === "draw");
  if (i >= 0) App.state.layer = i;
  else { App.doc.layers.push(newDrawLayer("Layer " + (App.doc.layers.length + 1), App.doc.frames)); App.state.layer = App.doc.layers.length - 1; }
}

function mediaLayer(name, media, settings) {
  return { id: uid(), name: name.slice(0, 40), kind: "media", visible: true, locked: false, opacity: 1, blend: "source-over", media, settings };
}
function insertLayer(L) {
  const at = Math.min(App.doc.layers.length, App.state.layer + 1);
  App.doc.layers.splice(at, 0, L);
  App.state.layer = at;
}
function extendTo(frames) {
  const d = App.doc;
  if (frames <= d.frames) return false;
  frames = Math.min(frames, 7200);
  for (const L of d.layers) if (L.cells) while (L.cells.length < frames) L.cells.push(null);
  d.frames = frames; return true;
}

Media.addVideoLayer = async function (file, { extend = true } = {}) {
  const url = URL.createObjectURL(file);
  const el = document.createElement("video");
  el.muted = true; el.playsInline = true; el.preload = "auto"; el.crossOrigin = "anonymous"; el.src = url;
  await new Promise((res, rej) => { el.onloadeddata = res; el.onerror = () => rej(new Error("This browser can't play that video format. MP4 (H.264) and WebM work best.")); });
  const media = { type: "video", name: file.name, mime: file.type, blob: file, url, el, w: el.videoWidth, h: el.videoHeight, duration: el.duration, size: file.size, hasAudio: undefined };
  el.addEventListener("seeked", () => changed("media"));
  History.push();
  const start = App.state.frame;
  const L = mediaLayer(file.name.replace(/\.[^.]+$/, ""), media, Media.defaults(start, el.duration));
  insertLayer(L);
  if (extend && isFinite(el.duration)) { if (extendTo(start + Math.ceil(el.duration * App.doc.fps))) toast(`Timeline extended to ${App.doc.frames} frames to fit the video.`); }
  changed("import");
  return L;
};

Media.addAudioLayer = async function (file, { extend = true } = {}) {
  const url = URL.createObjectURL(file);
  const el = document.createElement("audio"); el.preload = "auto"; el.src = url; el.muted = true;
  await new Promise((res, rej) => { el.onloadedmetadata = res; el.onerror = () => rej(new Error("This browser can't play that audio format.")); });
  const media = { type: "audio", name: file.name, mime: file.type, blob: file, url, el, w: 1, h: 1, duration: el.duration, size: file.size };
  History.push();
  const L = mediaLayer(file.name.replace(/\.[^.]+$/, ""), media, Media.defaults(App.state.frame, el.duration));
  insertLayer(L);
  if (extend && isFinite(el.duration)) extendTo(App.state.frame + Math.ceil(el.duration * App.doc.fps));
  changed("import");
};

Media.addImageLayer = async function (file) {
  const im = await loadImageEl(file);
  const c = toCanvas(im, im.naturalWidth || 512, im.naturalHeight || 512);
  History.push();
  const L = mediaLayer(file.name.replace(/\.[^.]+$/, ""), { type: "image", name: file.name, mime: file.type, blob: file, el: c, w: c.width, h: c.height, duration: 0, size: file.size }, Media.defaults(0, 0));
  insertLayer(L); changed("import");
};

/** Many images (or GIF frames) become one layer that shows a frame per timeline frame. */
Media.addSequenceLayer = async function (name, frames, { fps = 0, extend = true, blobs = null } = {}) {
  if (!frames.length) return;
  const w = frames[0].width, h = frames[0].height;
  History.push();
  const f0 = App.state.frame;
  const seqFps = fps || App.doc.fps;
  const settings = Media.defaults(f0, frames.length / seqFps);
  settings.seqFps = fps;
  const L = mediaLayer(name, { type: "sequence", name, frames, w, h, duration: frames.length / seqFps, blobs }, settings);
  insertLayer(L);
  if (extend) extendTo(f0 + Math.ceil(frames.length / seqFps * App.doc.fps));
  changed("import");
};
Media.imagesToFrames = async function (files) {
  const sorted = [...files].sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
  const frames = [];
  let w = 0, h = 0;
  for (const f of sorted) {
    const im = await loadImageEl(f);
    if (!w) { const k = Math.min(1, 2048 / Math.max(im.naturalWidth, im.naturalHeight)); w = Math.round(im.naturalWidth * k); h = Math.round(im.naturalHeight * k); }
    const c = document.createElement("canvas"); c.width = w; c.height = h;
    c.getContext("2d").drawImage(im, 0, 0, w, h); frames.push(c);
  }
  return frames;
};
/** Decodes an animated GIF into frames (Chrome/Edge/Firefox via ImageDecoder); returns null when unsupported. */
Media.gifFrames = async function (file) {
  if (!window.ImageDecoder) return null;
  try {
    const dec = new ImageDecoder({ data: file.stream(), type: "image/gif" });
    await dec.tracks.ready;
    const n = dec.tracks.selectedTrack.frameCount;
    const frames = []; let totalMs = 0;
    for (let i = 0; i < n; i++) {
      const { image } = await dec.decode({ frameIndex: i });
      const c = document.createElement("canvas"); c.width = image.displayWidth; c.height = image.displayHeight;
      c.getContext("2d").drawImage(image, 0, 0); totalMs += (image.duration || 100000) / 1000; image.close();
      frames.push(c);
    }
    return { frames, fps: n / Math.max(0.01, totalMs / 1000) };
  } catch (e) { return null; }
};

/** Rotoscope: bakes a media layer into drawable pixel frames on a new layer above it. */
Media.toDrawFrames = async function (L, onProgress) {
  const d = App.doc, idx = d.layers.indexOf(L);
  const first = L.settings.start, count = Math.min(d.frames - first, isFinite(Media.lengthFrames(L)) ? Media.lengthFrames(L) : d.frames - first);
  History.push();
  const layer = newDrawLayer(L.name + " (frames)", d.frames);
  layer.cells = new Array(d.frames).fill(null);
  const step = Math.max(1, L.settings.hold || 1);
  for (let f = first; f < first + count; f += step) {
    if (L.media.type === "video") await seekExact(L.media.el, Media.timeAt(L, f));
    const c = document.createElement("canvas"); c.width = d.width; c.height = d.height;
    Media.draw(c.getContext("2d"), L, f);
    layer.cells[f] = { key: true, objects: [], raster: c, tween: null };
    onProgress && onProgress((f - first) / count);
  }
  if (!layer.cells[0]) layer.cells[0] = newCell();
  d.layers.splice(idx + 1, 0, layer);
  L.visible = false;
  App.state.layer = idx + 1;
  changed("rotoscope");
};

/** Decoded audio of a media layer at the given sample rate (cached). */
Media.decodeAudio = async function (L, sampleRate) {
  const m = L.media;
  if (!m.blob) return null;
  if (m._audio && m._audio.sampleRate === sampleRate) return m._audio;
  try {
    const buf = await m.blob.arrayBuffer();
    const ctx = new OfflineAudioContext(2, 1, sampleRate);
    const ab = await ctx.decodeAudioData(buf);
    const L0 = ab.getChannelData(0), R0 = ab.numberOfChannels > 1 ? ab.getChannelData(1) : L0;
    m._audio = { sampleRate, left: new Float32Array(L0), right: new Float32Array(R0), duration: ab.duration };
    m.hasAudio = true;
    return m._audio;
  } catch (e) { m.hasAudio = false; return null; }
};

// ---------------------------------------------------------------- project files
const Project = (App.Project = {});
function canvasToBlob(c) { return new Promise((res) => c.toBlob(res, "image/png")); }
function blobToDataURL(b) { return new Promise((res) => { const r = new FileReader(); r.onload = () => res(r.result); r.readAsDataURL(b); }); }
async function dataToBlob(d) { if (!d) return null; if (d instanceof Blob) return d; return (await fetch(d)).blob(); }

Project.serialize = async function ({ embedMedia = true, asBlobs = false } = {}) {
  const d = App.doc;
  const enc = async (c) => (c ? (asBlobs ? await canvasToBlob(c) : c.toDataURL("image/png")) : null);
  const layers = [];
  for (const L of d.layers) {
    const base = { id: L.id, name: L.name, kind: L.kind, visible: L.visible, locked: L.locked, opacity: L.opacity, blend: L.blend, clip: !!L.clip, lgroup: L.lgroup || undefined };
    if (L.mask && L.mask.canvas) base.mask = { on: L.mask.on !== false, data: await enc(L.mask.canvas) };
    if (L.kind === "draw") {
      base.cells = [];
      for (const c of L.cells) base.cells.push(c ? { objects: c.objects, tween: c.tween, raster: await enc(c.raster) } : null);
    } else {
      const m = L.media;
      base.settings = L.settings;
      base.media = { type: m.type, name: m.name, mime: m.mime, w: m.w, h: m.h, duration: m.duration };
      if (embedMedia) {
        if (m.type === "sequence") { base.media.frames = []; for (const c of m.frames) base.media.frames.push(await enc(c)); }
        else if (m.type === "image" && !m.blob) base.media.data = await enc(m.el);
        else if (m.blob) base.media.data = asBlobs ? m.blob : await blobToDataURL(m.blob);
      }
    }
    layers.push(base);
  }
  const { layers: _l, ...meta } = d;
  return { app: "frame-studio", version: 1, doc: { ...meta, layers }, state: { frame: App.state.frame, layer: App.state.layer } };
};

Project.load = async function (data) {
  if (!data || data.app !== "frame-studio") throw new Error("That isn't a Frame Studio project file.");
  const src = data.doc;
  const doc = { ...newDoc(src.width, src.height, src.fps, src.frames), ...src, layers: [] };
  const toCanvasFrom = async (x) => {
    const b = await dataToBlob(x); if (!b) return null;
    const im = await loadImageEl(b); const c = document.createElement("canvas"); c.width = im.naturalWidth; c.height = im.naturalHeight;
    c.getContext("2d").drawImage(im, 0, 0); return c;
  };
  let skipped = 0;
  for (const L of src.layers) {
    if (L.kind === "draw") {
      const cells = [];
      for (const c of L.cells) cells.push(c ? { key: true, objects: c.objects || [], tween: c.tween || null, raster: await toCanvasFrom(c.raster) } : null);
      while (cells.length < doc.frames) cells.push(null);
      doc.layers.push({ ...L, cells });
    } else {
      const m = L.media; let media = null;
      try {
        if (m.type === "sequence" && m.frames) { const fr = []; for (const x of m.frames) fr.push(await toCanvasFrom(x)); media = { ...m, frames: fr }; }
        else if (m.data) {
          const blob = await dataToBlob(m.data);
          const file = new File([blob], m.name || "media", { type: m.mime || blob.type });
          if (m.type === "image") { const im = await loadImageEl(file); media = { ...m, blob: file, el: toCanvas(im, im.naturalWidth, im.naturalHeight) }; }
          else {
            const url = URL.createObjectURL(file);
            const el = document.createElement(m.type === "audio" ? "audio" : "video");
            el.muted = true; el.playsInline = true; el.preload = "auto"; el.src = url;
            await new Promise((res) => { el.onloadeddata = res; el.onloadedmetadata = m.type === "audio" ? res : null; el.onerror = res; });
            if (m.type === "video") el.addEventListener("seeked", () => changed("media"));
            media = { ...m, blob: file, url, el };
          }
        }
      } catch (e) { media = null; }
      if (!media) { skipped++; continue; }
      delete media.data;
      doc.layers.push({ ...L, media, settings: { ...Media.defaults(), ...L.settings } });
    }
  }
  for (const L of doc.layers) {
    if (L.mask && L.mask.data) { const c = await toCanvasFrom(L.mask.data); L.mask = c ? { on: L.mask.on !== false, canvas: c } : undefined; }
    else if (L.mask && !L.mask.canvas) delete L.mask;
    delete L.maskEdit;
  }
  if (!doc.layers.length) doc.layers.push(newDrawLayer("Layer 1", doc.frames));
  App.doc = doc;
  App.state.frame = clamp(data.state?.frame || 0, 0, doc.frames - 1);
  App.state.layer = clamp(data.state?.layer || 0, 0, doc.layers.length - 1);
  App.state.selection = [];
  History.clear();
  changed("load");
  if (skipped) toast(`${skipped} media ${skipped === 1 ? "layer wasn't" : "layers weren't"} saved in this project, so ${skipped === 1 ? "it was" : "they were"} left out.`);
};
