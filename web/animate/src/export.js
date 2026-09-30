// ============================================================================
// export.js — the export dialog and encoders (MP4, WebM, GIF, PNG sequence,
// sprite sheet, SVG frame) plus project files
// ============================================================================
const GIFENC_URL = "https://cdn.jsdelivr.net/npm/gifenc@1.0.3/dist/gifenc.esm.js";
const Exporter = (App.Exporter = {});
let downloads = null;
if (window.claude && typeof window.claude.use === "function") window.claude.use("downloads").then((d) => { downloads = d; }).catch(() => {});

const FORMATS = [
  ["mp4", "MP4 video (H.264, plays everywhere)"],
  ["webm", "WebM video (VP9, smaller, web)"],
  ["gif", "Animated GIF"],
  ["png", "PNG sequence (ZIP)"],
  ["sprite", "Sprite sheet (PNG)"],
  ["svg", "SVG of the current frame (vector)"],
  ["frame", "PNG of the current frame"]
];

Exporter.settings = null;
Exporter.defaults = function () {
  const d = App.doc;
  return {
    format: "mp4", scale: 1, width: d.width, height: d.height, lockAspect: true,
    fps: d.fps, rangeMode: "all", from: 1, to: d.frames, repeat: 1, boomerang: false,
    quality: "high", bitrate: 0, keyInterval: 2, codec: "auto", audio: true, audioKbps: 160,
    transparent: d.transparent, bg: d.background,
    gifColors: 256, gifDither: "ordered", gifLoop: 0,
    pngPrefix: (d.name || "frame").replace(/[^\w-]+/g, "_"), startNumber: 1,
    spriteCols: 0, spritePad: 0, spriteJson: true,
    name: (d.name || "animation").replace(/[\\/:*?"<>|]+/g, "_")
  };
};

// ---------------------------------------------------------------- frame plan
function framePlan(o) {
  const d = App.doc;
  let a = 0, b = d.frames - 1;
  if (o.rangeMode === "custom") { a = clamp(o.from - 1, 0, d.frames - 1); b = clamp(o.to - 1, a, d.frames - 1); }
  if (o.rangeMode === "current") { a = b = App.state.frame; }
  const count = b - a + 1;
  const passSec = count / (d.fps * d.speed);
  const outN = Math.max(1, Math.round(passSec * o.fps));
  const pass = [];
  for (let i = 0; i < outN; i++) pass.push(clamp(a + Math.floor((i / o.fps) * d.fps * d.speed + 1e-6), a, b));
  let seq = pass.slice();
  if (o.boomerang && pass.length > 2) seq = seq.concat(pass.slice(1, -1).reverse());
  const one = seq.slice();
  for (let r = 1; r < o.repeat; r++) seq = seq.concat(one);
  return { frames: seq, a, b, passSec, passFrames: pass.length, seconds: seq.length / o.fps };
}
const evenize = (v) => Math.max(2, Math.round(v / 2) * 2);
const needsEven = (f) => f === "mp4" || f === "webm";

function renderInto(ctx, W, H, f, o) {
  const d = App.doc;
  ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.globalAlpha = 1; ctx.globalCompositeOperation = "source-over";
  ctx.clearRect(0, 0, W, H);
  if (!o.transparent) { ctx.fillStyle = o.bg; ctx.fillRect(0, 0, W, H); }
  ctx.save(); ctx.scale(W / d.width, H / d.height);
  ctx.imageSmoothingQuality = "high";
  Render.frame(ctx, f, { background: false });
  ctx.restore();
}
const hasVideo = () => App.doc.layers.some((L) => L.visible && L.media && L.media.type === "video");

// ---------------------------------------------------------------- audio mix (48 kHz stereo)
async function mixAudio(o, plan) {
  const d = App.doc, SR = 48000;
  const layers = d.layers.filter((L) => L.media && (L.media.type === "video" || L.media.type === "audio") && L.visible && !L.settings.muted && L.settings.volume > 0);
  if (!layers.length) return null;
  const passLen = Math.round(plan.passSec * SR);
  const outL = new Float32Array(passLen), outR = new Float32Array(passLen);
  let any = false;
  for (const L of layers) {
    const a = await Media.decodeAudio(L, SR); if (!a) continue;
    any = true;
    const s = L.settings, clipLen = Math.max(0, s.trimOut - s.trimIn) / Math.max(0.01, s.speed);
    for (let j = 0; j < passLen; j++) {
      const projT = plan.a / d.fps + (j / SR) * d.speed;
      let local = projT - s.start / d.fps;
      if (local < 0) continue;
      if (local >= clipLen) { if (!s.loop || clipLen <= 0) continue; local %= clipLen; }
      const mt = s.reverse ? s.trimOut - local * s.speed : s.trimIn + local * s.speed;
      const p = mt * SR, i = Math.floor(p), fr = p - i;
      if (i < 0 || i + 1 >= a.left.length) continue;
      outL[j] += (a.left[i] + (a.left[i + 1] - a.left[i]) * fr) * s.volume;
      outR[j] += (a.right[i] + (a.right[i + 1] - a.right[i]) * fr) * s.volume;
    }
  }
  if (!any) return null;
  let segL = outL, segR = outR;
  if (o.boomerang) { segL = concatF(outL, outL.slice().reverse()); segR = concatF(outR, outR.slice().reverse()); }
  let L = segL, R = segR;
  for (let r = 1; r < o.repeat; r++) { L = concatF(L, segL); R = concatF(R, segR); }
  const total = Math.round(plan.seconds * SR);
  const fit = (x) => { if (x.length === total) return x; const y = new Float32Array(total); y.set(x.subarray(0, total)); return y; };
  L = fit(L); R = fit(R);
  for (let i = 0; i < L.length; i++) { L[i] = clamp(L[i], -1, 1); R[i] = clamp(R[i], -1, 1); }
  return { sampleRate: SR, left: L, right: R };
}
function concatF(a, b) { const c = new Float32Array(a.length + b.length); c.set(a); c.set(b, a.length); return c; }

// ---------------------------------------------------------------- video (WebCodecs + muxers)
async function pickVideoCodec(fmt, W, H, fps, bitrate, alpha, pref) {
  const avc = ["avc1.640034", "avc1.640033", "avc1.64002A", "avc1.640028", "avc1.4D4028", "avc1.42E028", "avc1.42001F"];
  const vp9 = ["vp09.00.40.08", "vp09.00.31.08", "vp09.00.10.08"];
  const list = fmt === "mp4" ? (pref === "vp9" ? [...vp9, ...avc] : [...avc, ...vp9]) : [...vp9, "vp8"];
  for (const codec of list) {
    const cfg = { codec, width: W, height: H, bitrate, framerate: fps, latencyMode: "quality" };
    if (codec.startsWith("avc")) cfg.avc = { format: "avc" };
    if (alpha && !codec.startsWith("avc")) cfg.alpha = "keep";
    try { const r = await VideoEncoder.isConfigSupported(cfg); if (r.supported) return cfg; } catch (e) { /* next */ }
    if (cfg.alpha) { delete cfg.alpha; try { const r = await VideoEncoder.isConfigSupported(cfg); if (r.supported) return { ...cfg, _noAlpha: true }; } catch (e) { /* next */ } }
  }
  return null;
}
async function pickAudioCodec(fmt, kbps) {
  const list = fmt === "mp4" ? ["mp4a.40.2", "opus"] : ["opus"];
  for (const codec of list) {
    const cfg = { codec, sampleRate: 48000, numberOfChannels: 2, bitrate: kbps * 1000 };
    try { const r = await AudioEncoder.isConfigSupported(cfg); if (r.supported) return cfg; } catch (e) { /* next */ }
  }
  return null;
}
const qualityBits = { low: 0.05, medium: 0.1, high: 0.17, max: 0.3 };
async function exportVideo(o, plan, prog) {
  const fmt = o.format, W = evenize(o.width), H = evenize(o.height);
  if (!window.VideoEncoder) return exportRecorder(o, plan, prog, W, H);
  const bitrate = o.bitrate > 0 ? o.bitrate * 1e6 : Math.round(clamp(W * H * o.fps * qualityBits[o.quality], 1e6, 60e6));
  const vcfg = await pickVideoCodec(fmt, W, H, o.fps, bitrate, o.transparent && fmt === "webm", o.codec);
  if (!vcfg) return exportRecorder(o, plan, prog, W, H);
  if (vcfg._noAlpha && o.transparent) toast("This browser can't keep transparency in video, so the background colour is used.");
  const transparent = o.transparent && fmt === "webm" && !vcfg._noAlpha;
  let audio = null, acfg = null;
  if (o.audio) {
    prog.set(0, "Mixing sound…");
    audio = await mixAudio(o, plan);
    if (audio && window.AudioEncoder) acfg = await pickAudioCodec(fmt, o.audioKbps);
    if (audio && !acfg) toast("This browser can't encode audio, so the video is silent.");
  }
  const vMux = vcfg.codec.startsWith("avc") ? "avc" : vcfg.codec.startsWith("vp09") ? "vp9" : "vp8";
  let muxer, target;
  if (fmt === "mp4") {
    target = new Mp4Muxer.ArrayBufferTarget();
    muxer = new Mp4Muxer.Muxer({ target, video: { codec: vMux, width: W, height: H, frameRate: o.fps }, audio: acfg ? { codec: acfg.codec.startsWith("mp4a") ? "aac" : "opus", sampleRate: 48000, numberOfChannels: 2 } : undefined, fastStart: "in-memory", firstTimestampBehavior: "offset" });
  } else {
    target = new WebMMuxer.ArrayBufferTarget();
    muxer = new WebMMuxer.Muxer({ target, video: { codec: vMux === "vp9" ? "V_VP9" : "V_VP8", width: W, height: H, frameRate: o.fps, alpha: transparent }, audio: acfg ? { codec: "A_OPUS", sampleRate: 48000, numberOfChannels: 2 } : undefined, firstTimestampBehavior: "offset" });
  }
  let err = null;
  const venc = new VideoEncoder({ output: (c, m) => muxer.addVideoChunk(c, m), error: (e) => (err = e) });
  const { _noAlpha, ...cleanCfg } = vcfg;
  venc.configure(cleanCfg);
  const c = document.createElement("canvas"); c.width = W; c.height = H;
  const g = c.getContext("2d", { alpha: true });
  const vid = hasVideo();
  const ro = { ...o, transparent };
  const n = plan.frames.length, us = 1e6 / o.fps, keyEvery = Math.max(1, Math.round(o.keyInterval * o.fps));
  for (let i = 0; i < n; i++) {
    if (prog.cancelled || err) break;
    if (vid) await Media.prepareFrame(plan.frames[i]);
    renderInto(g, W, H, plan.frames[i], ro);
    const vf = new VideoFrame(c, { timestamp: Math.round(i * us), duration: Math.round(us), alpha: transparent ? "keep" : "discard" });
    venc.encode(vf, { keyFrame: i % keyEvery === 0 }); vf.close();
    while (venc.encodeQueueSize > 8) await new Promise((r) => setTimeout(r, 4));
    if (i % 3 === 0) { prog.set(i / n, `Frame ${i + 1} of ${n}`); await new Promise((r) => setTimeout(r, 0)); }
  }
  if (prog.cancelled) { try { venc.close(); } catch (e) { /* closed */ } return null; }
  await venc.flush(); venc.close();
  if (err) throw err;
  if (acfg && audio) {
    prog.set(0.98, "Encoding sound…");
    let aerr = null;
    const aenc = new AudioEncoder({ output: (ch, m) => muxer.addAudioChunk(ch, m), error: (e) => (aerr = e) });
    aenc.configure(acfg);
    const block = 4800, total = audio.left.length;
    for (let s = 0; s < total; s += block) {
      const len = Math.min(block, total - s), buf = new Float32Array(len * 2);
      buf.set(audio.left.subarray(s, s + len), 0); buf.set(audio.right.subarray(s, s + len), len);
      const ad = new AudioData({ format: "f32-planar", sampleRate: 48000, numberOfFrames: len, numberOfChannels: 2, timestamp: Math.round((s / 48000) * 1e6), data: buf });
      aenc.encode(ad); ad.close();
    }
    await aenc.flush(); aenc.close();
    if (aerr) toast("Sound couldn't be encoded; the video is silent.");
  }
  muxer.finalize();
  return new Blob([target.buffer], { type: fmt === "mp4" ? "video/mp4" : "video/webm" });
}
/** Fallback for browsers without WebCodecs: records the canvas in real time (no sound). */
async function exportRecorder(o, plan, prog, W, H) {
  if (!window.MediaRecorder) throw new Error("This browser can't make video files. Try Chrome, Edge or Safari.");
  const types = o.format === "mp4" ? ["video/mp4;codecs=avc1", "video/mp4", "video/webm"] : ["video/webm;codecs=vp9", "video/webm"];
  const type = types.find((t) => MediaRecorder.isTypeSupported(t)); if (!type) throw new Error("This browser can't record video.");
  const c = document.createElement("canvas"); c.width = W; c.height = H; const g = c.getContext("2d");
  renderInto(g, W, H, plan.frames[0], o);
  const rec = new MediaRecorder(c.captureStream(o.fps), { mimeType: type, videoBitsPerSecond: Math.round(clamp(W * H * o.fps * qualityBits[o.quality], 1e6, 40e6)) });
  const chunks = []; rec.ondataavailable = (e) => e.data.size && chunks.push(e.data);
  const done = new Promise((r) => (rec.onstop = r));
  rec.start(250);
  const t0 = performance.now();
  for (let i = 0; i < plan.frames.length; i++) {
    if (prog.cancelled) break;
    if (hasVideo()) await Media.prepareFrame(plan.frames[i]);
    renderInto(g, W, H, plan.frames[i], o);
    prog.set(i / plan.frames.length, `Recording frame ${i + 1} of ${plan.frames.length} (real time)`);
    const wait = t0 + ((i + 1) * 1000) / o.fps - performance.now(); if (wait > 0) await new Promise((r) => setTimeout(r, wait));
  }
  rec.stop(); await done;
  if (prog.cancelled) return null;
  toast("Recorded in real time without sound (this browser lacks WebCodecs).");
  return new Blob(chunks, { type: type.split(";")[0] });
}

// ---------------------------------------------------------------- GIF
const BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5].map((v) => v / 16 - 0.5);
async function exportGif(o, plan, prog) {
  let lib;
  try { lib = await import(GIFENC_URL); } catch (e) { throw new Error("The GIF encoder couldn't load. Check your connection and try again."); }
  const { GIFEncoder, quantize, applyPalette } = lib;
  const W = Math.round(o.width), H = Math.round(o.height);
  const c = document.createElement("canvas"); c.width = W; c.height = H;
  const g = c.getContext("2d", { willReadFrequently: true });
  const gif = GIFEncoder();
  const delay = Math.max(20, Math.round(1000 / o.fps / 10) * 10);
  const n = plan.frames.length, vid = hasVideo();
  const strength = o.gifDither === "ordered" ? 255 / Math.max(2, Math.cbrt(o.gifColors)) * 0.9 : 0;
  const format = o.transparent ? "rgba4444" : "rgb565";
  for (let i = 0; i < n; i++) {
    if (prog.cancelled) return null;
    if (vid) await Media.prepareFrame(plan.frames[i]);
    renderInto(g, W, H, plan.frames[i], o);
    const data = g.getImageData(0, 0, W, H).data;
    if (strength) for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const k = (y * W + x) * 4, t = BAYER[(y & 3) * 4 + (x & 3)] * strength;
      data[k] = clamp(data[k] + t, 0, 255); data[k + 1] = clamp(data[k + 1] + t, 0, 255); data[k + 2] = clamp(data[k + 2] + t, 0, 255);
    }
    const palette = quantize(data, o.gifColors, { format, oneBitAlpha: o.transparent ? 127 : false, clearAlpha: o.transparent });
    const index = applyPalette(data, palette, format);
    const ti = o.transparent ? palette.findIndex((p) => p[3] === 0) : -1;
    gif.writeFrame(index, W, H, { palette, delay, repeat: o.gifLoop, transparent: ti >= 0, transparentIndex: Math.max(0, ti), dispose: o.transparent ? 2 : -1 });
    if (i % 2 === 0) { prog.set(i / n, `Frame ${i + 1} of ${n}`); await new Promise((r) => setTimeout(r, 0)); }
  }
  gif.finish();
  return new Blob([gif.bytes()], { type: "image/gif" });
}

// ---------------------------------------------------------------- PNG sequence (ZIP) & sprite sheet
const crcTable = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
function crc32(d) { let c = 0xffffffff; for (let i = 0; i < d.length; i++) c = crcTable[(c ^ d[i]) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
function zip(files) {
  const enc = new TextEncoder(), parts = [], central = []; let off = 0;
  for (const f of files) {
    const name = enc.encode(f.name), crc = crc32(f.data), size = f.data.length;
    const hd = new DataView(new ArrayBuffer(30));
    hd.setUint32(0, 0x04034b50, true); hd.setUint16(4, 20, true); hd.setUint16(6, 0x0800, true);
    hd.setUint32(14, crc, true); hd.setUint32(18, size, true); hd.setUint32(22, size, true); hd.setUint16(26, name.length, true);
    parts.push(new Uint8Array(hd.buffer), name, f.data);
    const cd = new DataView(new ArrayBuffer(46));
    cd.setUint32(0, 0x02014b50, true); cd.setUint16(4, 20, true); cd.setUint16(6, 20, true); cd.setUint16(8, 0x0800, true);
    cd.setUint32(16, crc, true); cd.setUint32(20, size, true); cd.setUint32(24, size, true); cd.setUint16(28, name.length, true); cd.setUint32(42, off, true);
    central.push(new Uint8Array(cd.buffer), name); off += 30 + name.length + size;
  }
  const cs = central.reduce((n, p) => n + p.length, 0), end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, files.length, true); end.setUint16(10, files.length, true); end.setUint32(12, cs, true); end.setUint32(16, off, true);
  return new Blob([...parts, ...central, new Uint8Array(end.buffer)], { type: "application/zip" });
}
const pngBytes = async (c) => new Uint8Array(await (await new Promise((r) => c.toBlob(r, "image/png"))).arrayBuffer());
async function exportPngZip(o, plan, prog) {
  const W = Math.round(o.width), H = Math.round(o.height);
  const c = document.createElement("canvas"); c.width = W; c.height = H; const g = c.getContext("2d");
  const files = [], n = plan.frames.length, pad = String(o.startNumber + n).length, vid = hasVideo();
  for (let i = 0; i < n; i++) {
    if (prog.cancelled) return null;
    if (vid) await Media.prepareFrame(plan.frames[i]);
    renderInto(g, W, H, plan.frames[i], o);
    files.push({ name: `${o.pngPrefix}_${String(o.startNumber + i).padStart(Math.max(4, pad), "0")}.png`, data: await pngBytes(c) });
    prog.set(i / n, `Frame ${i + 1} of ${n}`);
  }
  return zip(files);
}
async function exportSprite(o, plan, prog) {
  const W = Math.round(o.width), H = Math.round(o.height), n = plan.frames.length, p = o.spritePad;
  const cols = o.spriteCols > 0 ? o.spriteCols : Math.ceil(Math.sqrt(n)), rows = Math.ceil(n / cols);
  const SW = cols * W + (cols - 1) * p, SH = rows * H + (rows - 1) * p;
  if (SW > 16384 || SH > 16384 || SW * SH > 268e6) throw new Error(`The sheet would be ${SW}×${SH}px, too large for a browser. Lower the size or frame count.`);
  const sheet = document.createElement("canvas"); sheet.width = SW; sheet.height = SH; const sg = sheet.getContext("2d");
  const c = document.createElement("canvas"); c.width = W; c.height = H; const g = c.getContext("2d"); const vid = hasVideo();
  const frames = {};
  for (let i = 0; i < n; i++) {
    if (prog.cancelled) return null;
    if (vid) await Media.prepareFrame(plan.frames[i]);
    renderInto(g, W, H, plan.frames[i], o);
    const x = (i % cols) * (W + p), y = Math.floor(i / cols) * (H + p);
    sg.drawImage(c, x, y);
    frames[`${o.name}_${i}`] = { frame: { x, y, w: W, h: H }, duration: Math.round(1000 / o.fps) };
    prog.set(i / n, `Frame ${i + 1} of ${n}`);
  }
  const png = await pngBytes(sheet);
  if (!o.spriteJson) return new Blob([png], { type: "image/png" });
  const meta = { frames, meta: { app: "Frame Studio", image: `${o.name}.png`, size: { w: SW, h: SH }, frameSize: { w: W, h: H }, columns: cols, rows, fps: o.fps, frameCount: n } };
  return zip([{ name: `${o.name}.png`, data: png }, { name: `${o.name}.json`, data: new TextEncoder().encode(JSON.stringify(meta, null, 1)) }]);
}

// ---------------------------------------------------------------- SVG of the current frame
class SvgPath {
  constructor() { this.d = []; this.x = 0; this.y = 0; this.sx = 0; this.sy = 0; }
  n(v) { return +v.toFixed(2); }
  moveTo(x, y) { this.d.push(`M${this.n(x)} ${this.n(y)}`); this.x = this.sx = x; this.y = this.sy = y; }
  lineTo(x, y) { this.d.push(`L${this.n(x)} ${this.n(y)}`); this.x = x; this.y = y; }
  quadraticCurveTo(a, b, x, y) { this.d.push(`Q${this.n(a)} ${this.n(b)} ${this.n(x)} ${this.n(y)}`); this.x = x; this.y = y; }
  bezierCurveTo(a, b, c, e, x, y) { this.d.push(`C${this.n(a)} ${this.n(b)} ${this.n(c)} ${this.n(e)} ${this.n(x)} ${this.n(y)}`); this.x = x; this.y = y; }
  closePath() { this.d.push("Z"); this.x = this.sx; this.y = this.sy; }
  rect(x, y, w, h) { this.moveTo(x, y); this.lineTo(x + w, y); this.lineTo(x + w, y + h); this.lineTo(x, y + h); this.closePath(); }
  ellipse(cx, cy, rx, ry, rot, a0, a1) { this.moveTo(cx + rx, cy); this.d.push(`A${this.n(rx)} ${this.n(ry)} 0 1 1 ${this.n(cx - rx)} ${this.n(cy)}`, `A${this.n(rx)} ${this.n(ry)} 0 1 1 ${this.n(cx + rx)} ${this.n(cy)}`); this.closePath(); }
  roundRect(x, y, w, h, r) {
    r = Math.min(r, w / 2, h / 2);
    this.moveTo(x + r, y); this.lineTo(x + w - r, y); this.d.push(`A${r} ${r} 0 0 1 ${this.n(x + w)} ${this.n(y + r)}`);
    this.lineTo(x + w, y + h - r); this.d.push(`A${r} ${r} 0 0 1 ${this.n(x + w - r)} ${this.n(y + h)}`);
    this.lineTo(x + r, y + h); this.d.push(`A${r} ${r} 0 0 1 ${this.n(x)} ${this.n(y + h - r)}`);
    this.lineTo(x, y + r); this.d.push(`A${r} ${r} 0 0 1 ${this.n(x + r)} ${this.n(y)}`); this.closePath();
  }
  arc(cx, cy, r, a0, a1, ccw) {
    const sx = cx + r * Math.cos(a0), sy = cy + r * Math.sin(a0);
    if (!this.d.length) this.moveTo(sx, sy); else if (Math.hypot(sx - this.x, sy - this.y) > 0.01) this.lineTo(sx, sy);
    let sweep = ccw ? a0 - a1 : a1 - a0;
    if (Math.abs(sweep) >= Math.PI * 2 - 1e-6) { const m = a0 + (ccw ? -Math.PI : Math.PI); this.arc(cx, cy, r, a0, m, ccw); this.arc(cx, cy, r, m, a1, ccw); return; }
    sweep = ((sweep % (Math.PI * 2)) + Math.PI * 2) % (Math.PI * 2);
    const ex = cx + r * Math.cos(a1), ey = cy + r * Math.sin(a1);
    this.d.push(`A${this.n(r)} ${this.n(r)} 0 ${sweep > Math.PI ? 1 : 0} ${ccw ? 0 : 1} ${this.n(ex)} ${this.n(ey)}`); this.x = ex; this.y = ey;
  }
  toString() { return this.d.join(""); }
}
function exportSvgFrame() {
  const d = App.doc, f = App.state.frame, esc = (t) => String(t).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");
  let defs = "", body = "", gid = 0;
  const paint = (fill) => {
    if (!fill) return 'fill="none"';
    if (typeof fill === "string") { const p = Color.parse(fill); return `fill="${Color.hex(fill)}"${p[3] < 1 ? ` fill-opacity="${+p[3].toFixed(3)}"` : ""}`; }
    const id = "g" + gid++;
    const s = (c) => `stop-color="${Color.hex(c)}" stop-opacity="${+Color.alpha(c).toFixed(3)}"`;
    if (fill.type === "radial") defs += `<radialGradient id="${id}" gradientUnits="objectBoundingBox"><stop offset="0" ${s(fill.c1)}/><stop offset="1" ${s(fill.c2)}/></radialGradient>`;
    else { const a = (fill.angle || 0) * Math.PI / 180; defs += `<linearGradient id="${id}" x1="${0.5 - Math.cos(a) / 2}" y1="${0.5 - Math.sin(a) / 2}" x2="${0.5 + Math.cos(a) / 2}" y2="${0.5 + Math.sin(a) / 2}"><stop offset="0" ${s(fill.c1)}/><stop offset="1" ${s(fill.c2)}/></linearGradient>`; }
    return `fill="url(#${id})"`;
  };
  if (!d.transparent) body += `<rect width="100%" height="100%" fill="${Color.hex(d.background)}"/>\n`;
  for (const L of d.layers) {
    if (!L.visible) continue;
    body += `<g id="${esc(L.name)}"${L.opacity < 1 ? ` opacity="${L.opacity}"` : ""}${L.blend && L.blend !== "source-over" ? ` style="mix-blend-mode:${L.blend}"` : ""}>\n`;
    if (L.kind === "media") {
      const c = document.createElement("canvas"); c.width = d.width; c.height = d.height; Media.draw(c.getContext("2d"), L, f);
      body += `<image width="${d.width}" height="${d.height}" href="${c.toDataURL("image/png")}"/>\n`;
    } else {
      const r = Render.cellRasterAt(L, f); if (r) body += `<image width="${d.width}" height="${d.height}" href="${r.toDataURL("image/png")}"/>\n`;
      for (const o of Render.cellObjectsAt(L, f)) {
        const tf = `transform="matrix(${o.m.map((v) => +v.toFixed(4)).join(" ")})"`;
        const common = `${o.opacity < 1 ? ` opacity="${o.opacity}"` : ""}${o.blend && o.blend !== "source-over" ? ` style="mix-blend-mode:${o.blend}"` : ""}`;
        const stroke = o.stroke && o.sw && o.type !== "brush" ? ` stroke="${Color.hex(o.stroke)}" stroke-width="${o.sw}" stroke-linecap="round" stroke-linejoin="round"${Color.alpha(o.stroke) < 1 ? ` stroke-opacity="${Color.alpha(o.stroke)}"` : ""}` : "";
        if (o.type === "image") body += `<image ${tf} width="${o.w}" height="${o.h}" href="${esc(o.src)}"${common}/>\n`;
        else if (o.type === "text") {
          const b = Render.textBox(o), anchor = o.align === "center" ? "middle" : o.align === "right" ? "end" : "start";
          body += `<text ${tf} font-family="${esc(o.font)}" font-size="${o.size}" font-weight="${o.weight || 400}"${o.italic ? ' font-style="italic"' : ""} text-anchor="${anchor}" dominant-baseline="text-before-edge" ${paint(o.fill)}${stroke}${common}>` +
            b.lines.map((l, i) => `<tspan x="0" y="${+(i * b.lh).toFixed(2)}">${esc(l)}</tspan>`).join("") + "</text>\n";
        } else { const p = new SvgPath(); Render.localPath(o, p); body += `<path ${tf} d="${p}" ${paint(o.fill)}${stroke}${common}/>\n`; }
      }
    }
    body += "</g>\n";
  }
  const svg = `<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="${d.width}" height="${d.height}" viewBox="0 0 ${d.width} ${d.height}">\n${defs ? `<defs>${defs}</defs>\n` : ""}${body}</svg>\n`;
  return new Blob([svg], { type: "image/svg+xml" });
}

// ---------------------------------------------------------------- saving files
/** In the Android app: stream the file to the phone's storage through the native bridge. */
async function saveViaAndroid(filename, blob) {
  const bridge = window.AndroidBridge;
  const id = bridge.begin(filename, blob.type || "application/octet-stream");
  if (!id) throw new Error("Couldn't create the file on this phone.");
  const CH = 768 * 1024;
  for (let o = 0; o < blob.size; o += CH) {
    const b64 = await new Promise((res, rej) => { const r = new FileReader(); r.onload = () => res(String(r.result).split(",")[1] || ""); r.onerror = rej; r.readAsDataURL(blob.slice(o, o + CH)); });
    if (!bridge.append(id, b64)) throw new Error("Writing the file failed.");
  }
  const where = bridge.finish(id);
  if (!where) throw new Error("Saving failed.");
  const [uri, folder] = where.split("|");
  UI.dialog("Saved", h("div", {}, h("p", { class: "hint", style: "margin:12px" }, `${filename} (${(blob.size / 1e6).toFixed(2)} MB) is in ${folder} on your phone.`)), [
    btn("Share…", () => bridge.share(uri, blob.type || "application/octet-stream")),
    btn("Done", (e) => e.target.closest("dialog").close(), "primary")
  ]);
}
async function offer(filename, blob) {
  if (window.AndroidBridge) { try { await saveViaAndroid(filename, blob); } catch (e) { toast(e.message || "Saving failed."); } return; }
  if (downloads) {
    try { await downloads.save({ filename, data: blob }); toast("Saved " + filename); return; }
    catch (e) {
      if (e && e.code === "declined") { toast("Download cancelled."); return; }
      if (e && e.code === "rate_limited") { toast("A save prompt is already open."); return; }
      if (e && e.code === "too_large") { toast("That file is too large for this save method. Try a smaller size or fewer frames."); return; }
    }
  }
  const url = URL.createObjectURL(blob), type = blob.type;
  const media = type.startsWith("video/") ? h("video", { src: url, controls: true, loop: true, playsinline: true }) : type.startsWith("image/") ? h("img", { src: url, alt: filename }) : null;
  UI.dialog("Your file", h("div", {}, h("p", { class: "hint" }, `${filename} · ${(blob.size / 1e6).toFixed(2)} MB. Downloads aren't available in this view: right-click the preview and choose Save, or open this page in Chrome or Safari.`), media), []);
}
Exporter.saveProject = async function () {
  const prog = UI.progress("Saving project…");
  try {
    const data = await Project.serialize({ embedMedia: true, asBlobs: false });
    const blob = new Blob([JSON.stringify(data)], { type: "application/json" });
    prog.close();
    await offer(`${(App.doc.name || "animation").replace(/[\\/:*?"<>|]+/g, "_")}.json`, blob);
  } catch (e) { prog.close(); toast("Saving failed: " + e.message); }
};

// ---------------------------------------------------------------- the export dialog
Exporter.open = function () {
  const d = App.doc;
  const prev = Exporter.settings;
  const o = (Exporter.settings = { ...Exporter.defaults(), ...(prev || {}) });
  if (!prev || prev._docW !== d.width || prev._docH !== d.height) { o.width = d.width; o.height = d.height; o.transparent = d.transparent; o.bg = d.background; }
  o._docW = d.width; o._docH = d.height;
  if (o.to > d.frames || o.rangeMode === "all") { o.from = 1; o.to = d.frames; }
  const body = h("div", { class: "export" });
  const summary = h("div", { class: "summary mono" });
  const aspect = d.width / d.height;
  const build = () => {
    body.textContent = "";
    const f = o.format, video = f === "mp4" || f === "webm", anim = !["svg", "frame"].includes(f);
    body.append(section("Format", select("File type", FORMATS, () => o.format, (v) => { o.format = v; build(); }, { record: false })));
    if (f !== "svg") {
      const wIn = h("input", { type: "number", id: "exW", min: 16, max: 7680, value: Math.round(o.width) }), hIn = h("input", { type: "number", id: "exH", min: 16, max: 7680, value: Math.round(o.height) });
      wIn.onchange = () => { o.width = clamp(+wIn.value || d.width, 16, 7680); if (o.lockAspect) { o.height = Math.round(o.width / aspect); hIn.value = o.height; } upd(); };
      hIn.onchange = () => { o.height = clamp(+hIn.value || d.height, 16, 7680); if (o.lockAspect) { o.width = Math.round(o.height * aspect); wIn.value = o.width; } upd(); };
      const scales = [[0.25, "25%"], [0.5, "50%"], [0.75, "75%"], [1, "100%"], [1.5, "150%"], [2, "200%"], [4, "400%"]];
      body.append(section("Size",
        h("div", { class: "chips" }, scales.map(([k, l]) => h("button", { type: "button", class: "chip-btn", onclick: () => { o.width = Math.round(d.width * k); o.height = Math.round(d.height * k); build(); } }, l)),
          h("button", { type: "button", class: "chip-btn", onclick: () => { const k = 1080 / Math.min(d.width, d.height); o.width = Math.round(d.width * k); o.height = Math.round(d.height * k); build(); } }, "1080p"),
          h("button", { type: "button", class: "chip-btn", onclick: () => { const k = 720 / Math.min(d.width, d.height); o.width = Math.round(d.width * k); o.height = Math.round(d.height * k); build(); } }, "720p")),
        h("div", { class: "row two" }, h("label", { for: "exW" }, "Width"), wIn, h("label", { for: "exH" }, "Height"), hIn),
        check("Keep proportions", () => o.lockAspect, (v) => (o.lockAspect = v), { record: false }),
        needsEven(f) ? h("p", { class: "hint" }, "Video sizes are rounded to even numbers.") : null
      ));
    }
    if (anim) {
      body.append(section("Timing",
        h("div", { class: "row" }, h("label", { for: "exFps" }, "Frame rate"), h("input", { type: "number", id: "exFps", min: 1, max: 60, value: o.fps, onchange: (e) => { o.fps = clamp(+e.target.value || d.fps, 1, 60); upd(); } }),
          h("div", { class: "chips" }, [8, 12, 15, 24, 25, 30, 60].map((v) => h("button", { type: "button", class: "chip-btn", onclick: () => { o.fps = v; build(); } }, String(v))))),
        select("Frames", [["all", `All (1–${d.frames})`], ["custom", "Custom range"], ["current", "Current frame only"]], () => o.rangeMode, (v) => { o.rangeMode = v; build(); }, { record: false }),
        o.rangeMode === "custom" ? h("div", { class: "row two" }, h("label", { for: "exFrom" }, "From"), h("input", { id: "exFrom", type: "number", min: 1, max: d.frames, value: o.from, onchange: (e) => { o.from = clamp(+e.target.value, 1, d.frames); upd(); } }), h("label", { for: "exTo" }, "To"), h("input", { id: "exTo", type: "number", min: 1, max: d.frames, value: o.to, onchange: (e) => { o.to = clamp(+e.target.value, 1, d.frames); upd(); } })) : null,
        numberField("Repeat (times)", () => o.repeat, (v) => { o.repeat = clamp(Math.round(v), 1, 100); upd(); }, { min: 1, max: 100, record: false }),
        check("Boomerang (play forward, then back)", () => o.boomerang, (v) => { o.boomerang = v; upd(); }, { record: false }),
        h("p", { class: "hint" }, `Playback speed ${d.speed}× from the timeline is applied. Exporting at a different frame rate re-times the frames without changing the length.`)
      ));
    }
    if (video) {
      body.append(section("Video",
        select("Quality", [["low", "Low (small file)"], ["medium", "Medium"], ["high", "High"], ["max", "Maximum"]], () => o.quality, (v) => { o.quality = v; o.bitrate = 0; build(); }, { record: false }),
        numberField("Bitrate Mbps (0 = auto)", () => o.bitrate, (v) => { o.bitrate = Math.max(0, v); upd(); }, { min: 0, max: 100, step: 0.5, record: false }),
        numberField("Keyframe every (s)", () => o.keyInterval, (v) => { o.keyInterval = clamp(v, 0.1, 10); }, { min: 0.1, max: 10, step: 0.1, record: false }),
        f === "mp4" ? select("Codec", [["auto", "H.264 (most compatible)"], ["vp9", "VP9 in MP4"]], () => o.codec, (v) => (o.codec = v), { record: false }) : null,
        check("Include sound from video / sound layers", () => o.audio, (v) => { o.audio = v; build(); }, { record: false }),
        o.audio ? select("Sound quality", [[96, "96 kbps"], [128, "128 kbps"], [160, "160 kbps"], [192, "192 kbps"], [256, "256 kbps"]], () => o.audioKbps, (v) => (o.audioKbps = +v), { record: false }) : null
      ));
    }
    if (f === "gif") {
      body.append(section("GIF",
        slider("Colours", 2, 256, 1, () => o.gifColors, (v) => { o.gifColors = v; upd(); }, { record: false }),
        select("Dithering", [["ordered", "Ordered (smoother gradients)"], ["none", "None (flat, smaller)"]], () => o.gifDither, (v) => (o.gifDither = v), { record: false }),
        select("Loop", [[0, "Forever"], [-1, "Play once"], [1, "Repeat once"], [2, "Repeat twice"], [4, "Repeat 4 times"]], () => o.gifLoop, (v) => (o.gifLoop = +v), { record: false }),
        o.fps > 50 ? h("p", { class: "warn" }, "Most browsers play GIFs slower than 50 fps. Use 50 fps or less.") : null
      ));
    }
    if (f === "png") body.append(section("Files", h("div", { class: "row" }, h("label", { for: "exPre" }, "Name prefix"), h("input", { id: "exPre", value: o.pngPrefix, onchange: (e) => (o.pngPrefix = e.target.value.replace(/[^\w-]+/g, "_") || "frame") })), numberField("First number", () => o.startNumber, (v) => (o.startNumber = Math.max(0, Math.round(v))), { min: 0, record: false })));
    if (f === "sprite") body.append(section("Sheet", numberField("Columns (0 = auto)", () => o.spriteCols, (v) => { o.spriteCols = Math.max(0, Math.round(v)); upd(); }, { min: 0, max: 200, record: false }), numberField("Padding (px)", () => o.spritePad, (v) => { o.spritePad = Math.max(0, Math.round(v)); upd(); }, { min: 0, max: 64, record: false }), check("Include JSON frame data (as ZIP)", () => o.spriteJson, (v) => (o.spriteJson = v), { record: false })));
    if (f !== "svg") {
      const alphaOk = ["gif", "png", "sprite", "frame", "webm"].includes(f);
      body.append(section("Background",
        alphaOk ? check(f === "webm" ? "Transparent (Chrome keeps it in WebM)" : "Transparent", () => o.transparent, (v) => { o.transparent = v; build(); }, { record: false }) : null,
        !o.transparent || !alphaOk ? colorRow("Colour", () => o.bg, (v) => (o.bg = v || o.bg), { record: false }) : null
      ));
    }
    body.append(section("File", h("div", { class: "row" }, h("label", { for: "exName" }, "Name"), h("input", { id: "exName", value: o.name, onchange: (e) => (o.name = e.target.value.replace(/[\\/:*?"<>|]+/g, "_") || "animation") }))));
    body.append(summary);
    upd();
  };
  const upd = () => {
    const f = o.format;
    if (f === "svg") { summary.textContent = `Vector SVG of frame ${App.state.frame + 1}. Shapes, paths, brushes and text stay editable in Illustrator; painted pixels and media come in as images.`; return; }
    if (f === "frame") { summary.textContent = `${Math.round(o.width)}×${Math.round(o.height)} PNG of frame ${App.state.frame + 1}.`; return; }
    const plan = framePlan(o), W = needsEven(f) ? evenize(o.width) : Math.round(o.width), H = needsEven(f) ? evenize(o.height) : Math.round(o.height);
    let size = 0;
    if (f === "mp4" || f === "webm") size = ((o.bitrate > 0 ? o.bitrate * 1e6 : clamp(W * H * o.fps * qualityBits[o.quality], 1e6, 60e6)) * (f === "webm" ? 0.8 : 1) + (o.audio ? o.audioKbps * 1000 : 0)) * plan.seconds / 8;
    else if (f === "gif") size = W * H * plan.frames.length * 0.05 * (o.gifColors / 256 + 0.3) * (o.gifDither === "ordered" ? 1.6 : 1);
    else size = W * H * plan.frames.length * 0.12;
    summary.textContent = `${W}×${H} · ${plan.frames.length} frames at ${o.fps} fps · ${plan.seconds.toFixed(2)} s · roughly ${size > 1e6 ? (size / 1e6).toFixed(1) + " MB" : Math.max(1, Math.round(size / 1e3)) + " KB"}`;
  };
  build();
  const dlg = UI.dialog("Export", body, [btn("Cancel", () => dlg.close()), btn("Export", () => { dlg.close(); Exporter.run(o); }, "primary")]);
  dlg.classList.add("wide");
};

Exporter.run = async function (o) {
  Player.stop(); TextTool.commit();
  const ext = { mp4: "mp4", webm: "webm", gif: "gif", png: "zip", sprite: o.spriteJson ? "zip" : "png", svg: "svg", frame: "png" }[o.format];
  const name = `${o.name}.${ext}`;
  if (o.format === "svg") { await offer(name, exportSvgFrame()); return; }
  const prog = UI.progress("Exporting…");
  const wasFrame = App.state.frame;
  window.AndroidBridge?.keepAwake(true);
  try {
    let blob;
    if (o.format === "frame") {
      const c = document.createElement("canvas"); c.width = Math.round(o.width); c.height = Math.round(o.height);
      if (hasVideo()) await Media.prepareFrame(App.state.frame);
      renderInto(c.getContext("2d"), c.width, c.height, App.state.frame, o);
      blob = await new Promise((r) => c.toBlob(r, "image/png"));
    } else {
      const plan = framePlan(o);
      if (o.format === "mp4" || o.format === "webm") blob = await exportVideo(o, plan, prog);
      else if (o.format === "gif") blob = await exportGif(o, plan, prog);
      else if (o.format === "png") blob = await exportPngZip(o, plan, prog);
      else if (o.format === "sprite") blob = await exportSprite(o, plan, prog);
    }
    prog.close();
    App.state.frame = wasFrame; Media.sync(wasFrame, false); View.redraw();
    if (!blob) { toast("Export cancelled."); return; }
    const realExt = blob.type === "video/webm" && o.format === "mp4" ? "webm" : ext;
    window.AndroidBridge?.keepAwake(false);
    await offer(`${o.name}.${realExt}`, blob);
  } catch (e) {
    window.AndroidBridge?.keepAwake(false);
    prog.close(); App.state.frame = wasFrame; Media.sync(wasFrame, false);
    toast("Export failed: " + (e.message || e), 6000);
  }
};
