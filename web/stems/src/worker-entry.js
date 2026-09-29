// Background worker for Stem Loop: HTDemucs separation (ONNX Runtime Web), tempo analysis,
// and the warp engine (phase-vocoder time-stretch, pitch shift, re-pitch).
// Built with esbuild into ../worker.js; `self.ort` is loaded by the banner via importScripts.
import { ONNXHTDemucs } from "demucs-dist/onnx-htdemucs.js";
import { separateTracks } from "demucs-dist/apply.js";

const ort = self.ort;
let model = null;

const post = (msg, transfer) => self.postMessage(msg, transfer || []);

// ------------------------------------------------------------------ model loading (cached in IndexedDB)

function idb() {
  return new Promise((resolve, reject) => {
    const r = indexedDB.open("stem-loop", 1);
    r.onupgradeneeded = () => r.result.createObjectStore("files");
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}
async function idbGet(key) {
  try {
    const db = await idb();
    return await new Promise((res) => {
      const t = db.transaction("files").objectStore("files").get(key);
      t.onsuccess = () => res(t.result || null); t.onerror = () => res(null);
    });
  } catch (e) { return null; }
}
async function idbPut(key, value) {
  try {
    const db = await idb();
    await new Promise((res) => {
      const t = db.transaction("files", "readwrite"); t.objectStore("files").put(value, key);
      t.oncomplete = res; t.onerror = res; t.onabort = res;
    });
  } catch (e) { /* storage full or blocked: fine, we just download next time */ }
}

async function fetchParts(urls, label, totalHint, onBytes) {
  const parts = [];
  for (const u of urls) {
    const res = await fetch(u);
    if (!res.ok) throw new Error(`Couldn't download ${label} (${res.status}).`);
    const reader = res.body.getReader();
    const chunks = [];
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value); onBytes(value.byteLength);
    }
    parts.push(...chunks);
  }
  const size = parts.reduce((n, c) => n + c.byteLength, 0);
  const out = new Uint8Array(size);
  let o = 0; for (const c of parts) { out.set(c, o); o += c.byteLength; }
  return out;
}

async function loadModel({ wasmUrls, wasmBytes, mjsUrl, modelUrls, modelBytes, cacheKey }) {
  let wasm = await idbGet(cacheKey + ":wasm");
  let weights = await idbGet(cacheKey + ":model");
  const cached = !!(wasm && weights);
  const total = (wasm ? 0 : wasmBytes) + (weights ? 0 : modelBytes);
  let loaded = 0;
  const onBytes = (n) => { loaded += n; post({ type: "progress", stage: "download", loaded, total }); };
  if (!wasm) { wasm = await fetchParts(wasmUrls, "the audio engine", wasmBytes, onBytes); await idbPut(cacheKey + ":wasm", wasm); }
  if (!weights) { weights = await fetchParts(modelUrls, "the separation model", modelBytes, onBytes); await idbPut(cacheKey + ":model", weights); }

  post({ type: "progress", stage: "init" });
  ort.env.wasm.numThreads = 1;               // no cross-origin isolation in the viewer, so single-threaded
  ort.env.wasm.proxy = false;
  ort.env.wasm.wasmBinary = wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength);
  ort.env.wasm.wasmPaths = { mjs: mjsUrl };

  let ep = "cpu";
  let session = null;
  if (self.navigator && navigator.gpu) {
    try {
      const adapter = await navigator.gpu.requestAdapter();
      if (adapter) {
        session = await ort.InferenceSession.create(weights, { executionProviders: ["webgpu"] });
        ep = "gpu";
      }
    } catch (e) { session = null; }
  }
  if (!session) session = await ort.InferenceSession.create(weights, { executionProviders: ["wasm"] });
  const m = new ONNXHTDemucs();
  m.session = session; m.inputNames = session.inputNames; m.outputNames = session.outputNames;
  model = m;
  post({ type: "ready", ep, cached });
}

async function separate({ left, right, sampleRate }) {
  if (!model) throw new Error("Model not loaded.");
  const t0 = performance.now();
  const tracks = await separateTracks(model, { channelData: [left, right], sampleRate }, (done, total) => {
    post({ type: "progress", stage: "separate", done, total, elapsed: (performance.now() - t0) / 1000 });
  }, 0.25);
  const names = Object.keys(tracks);
  const out = {}; const transfer = [];
  for (const n of names) {
    const [l, r] = tracks[n].channelData;
    const L = new Float32Array(l), R = new Float32Array(r);   // copy out of the shared result buffer
    out[n] = [L, R]; transfer.push(L.buffer, R.buffer);
  }
  post({ type: "stems", stems: out }, transfer);
}

// ------------------------------------------------------------------ FFT (radix-2, in place)

const fftCache = new Map();
function fftTables(n) {
  let t = fftCache.get(n);
  if (t) return t;
  const rev = new Uint32Array(n);
  const bits = Math.log2(n);
  for (let i = 0; i < n; i++) { let r = 0, x = i; for (let b = 0; b < bits; b++) { r = (r << 1) | (x & 1); x >>= 1; } rev[i] = r; }
  const cos = new Float64Array(n / 2), sin = new Float64Array(n / 2);
  for (let i = 0; i < n / 2; i++) { cos[i] = Math.cos(2 * Math.PI * i / n); sin[i] = Math.sin(2 * Math.PI * i / n); }
  t = { rev, cos, sin }; fftCache.set(n, t); return t;
}
function fft(re, im, inverse) {
  const n = re.length, { rev, cos, sin } = fftTables(n);
  for (let i = 0; i < n; i++) { const j = rev[i]; if (j > i) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
  for (let size = 2; size <= n; size <<= 1) {
    const half = size >> 1, step = n / size;
    for (let i = 0; i < n; i += size) {
      for (let j = 0, k = 0; j < half; j++, k += step) {
        const wr = cos[k], wi = inverse ? sin[k] : -sin[k];
        const a = i + j, b = a + half;
        const tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
        re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
      }
    }
  }
  if (inverse) for (let i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

// ------------------------------------------------------------------ warp engine

/**
 * Phase-vocoder time stretch with identity phase locking and transient phase reset.
 * alpha > 1 makes the audio longer (slower); pitch is preserved.
 */
function stretch(x, alpha, onProgress) {
  const N = 4096, Hs = 1024, Ha = Hs / alpha;
  const outLen = Math.max(1, Math.round(x.length * alpha));
  const y = new Float32Array(outLen + N);
  const norm = new Float32Array(outLen + N);
  const win = new Float64Array(N);
  for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N);
  const bins = N / 2 + 1;
  const re = new Float64Array(N), im = new Float64Array(N);
  const mag = new Float64Array(bins), ph = new Float64Array(bins);
  const prevPh = new Float64Array(bins), synPh = new Float64Array(bins), prevMag = new Float64Array(bins);
  const peakOf = new Int32Array(bins);
  const frames = Math.ceil(outLen / Hs) + 1;
  let prevPos = 0;
  for (let m = 0; m < frames; m++) {
    const pos = Math.round(m * Ha) - N / 2, sPos = m * Hs - N / 2;
    for (let i = 0; i < N; i++) { const s = pos + i; re[i] = (s >= 0 && s < x.length ? x[s] : 0) * win[i]; im[i] = 0; }
    fft(re, im, false);
    let flux = 0, energy = 0;
    for (let k = 0; k < bins; k++) {
      mag[k] = Math.hypot(re[k], im[k]); ph[k] = Math.atan2(im[k], re[k]);
      const d = mag[k] - prevMag[k]; if (d > 0) flux += d; energy += mag[k];
    }
    const transient = m === 0 || flux > 0.55 * energy + 1e-9 && energy > 1e-3;
    if (transient) {
      for (let k = 0; k < bins; k++) synPh[k] = ph[k];
    } else {
      const ha = pos - prevPos;
      // find spectral peaks, then lock every bin's phase to its nearest peak
      const peaks = [];
      for (let k = 2; k < bins - 2; k++) if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > mag[k - 2] && mag[k] >= mag[k + 2]) peaks.push(k);
      if (!peaks.length) peaks.push(0);
      let pi = 0;
      for (let k = 0; k < bins; k++) {
        while (pi < peaks.length - 1 && Math.abs(peaks[pi + 1] - k) <= Math.abs(peaks[pi] - k)) pi++;
        peakOf[k] = peaks[pi];
      }
      const newPh = new Float64Array(peaks.length);
      for (let q = 0; q < peaks.length; q++) {
        const k = peaks[q], w = 2 * Math.PI * k / N;
        let d = ph[k] - prevPh[k] - w * ha;
        d -= 2 * Math.PI * Math.round(d / (2 * Math.PI));
        newPh[q] = synPh[k] + (w + d / Math.max(1, ha)) * Hs;
      }
      const peakSyn = new Map(); peaks.forEach((k, q) => peakSyn.set(k, newPh[q]));
      for (let k = 0; k < bins; k++) { const p = peakOf[k]; synPh[k] = peakSyn.get(p) + (ph[k] - ph[p]); }
    }
    for (let k = 0; k < bins; k++) { prevPh[k] = ph[k]; prevMag[k] = mag[k]; }
    prevPos = pos;
    for (let k = 0; k < bins; k++) { re[k] = mag[k] * Math.cos(synPh[k]); im[k] = mag[k] * Math.sin(synPh[k]); }
    for (let k = 1; k < N / 2; k++) { re[N - k] = re[k]; im[N - k] = -im[k]; }
    fft(re, im, true);
    for (let i = 0; i < N; i++) {
      const s = sPos + i; if (s < 0 || s >= y.length) continue;
      y[s] += re[i] * win[i]; norm[s] += win[i] * win[i];
    }
    if (onProgress && (m & 63) === 0) onProgress(m / frames);
  }
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) out[i] = norm[i] > 1e-6 ? y[i] / norm[i] : 0;
  return out;
}

/** Cubic (Catmull-Rom) resampling: reads x at position i*step. */
function resample(x, step) {
  const len = Math.max(1, Math.floor(x.length / step));
  const y = new Float32Array(len);
  for (let i = 0; i < len; i++) {
    const p = i * step, j = Math.floor(p), t = p - j;
    const a = x[j - 1] ?? x[0] ?? 0, b = x[j] ?? 0, c = x[j + 1] ?? b, d = x[j + 2] ?? c;
    y[i] = b + 0.5 * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
  }
  return y;
}

/** Gentle low-pass before shrinking to reduce aliasing when pitching up / speeding up. */
function lowpass(x, cutoffRatio) {
  if (cutoffRatio >= 0.98) return x;
  const rc = 1 / (2 * Math.PI * cutoffRatio * 0.5), dt = 1, a = dt / (rc + dt);
  const y = new Float32Array(x.length);
  let s1 = 0, s2 = 0;
  for (let i = 0; i < x.length; i++) { s1 += a * (x[i] - s1); s2 += a * (s1 - s2); y[i] = s2; }
  return y;
}

/**
 * mode "warp": change tempo by `speed` (1.25 = 25% faster) keeping pitch, then transpose by `semitones`.
 * mode "repitch": tape-style, pitch follows speed.
 */
function warpChannel(x, mode, speed, semitones, onProgress) {
  if (mode === "repitch") {
    if (Math.abs(speed - 1) < 1e-6) return x.slice();
    return resample(speed > 1 ? lowpass(x, 1 / speed) : x, speed);
  }
  const r = Math.pow(2, semitones / 12);
  if (Math.abs(speed - 1) < 1e-6 && Math.abs(r - 1) < 1e-6) return x.slice();
  const s = stretch(x, r / speed, onProgress);
  if (Math.abs(r - 1) < 1e-6) return s;
  return resample(r > 1 ? lowpass(s, 1 / r) : s, r);
}

function warp({ id, stems, mode, speed, semitones }) {
  const names = Object.keys(stems);
  const total = names.length * 2;
  let done = 0;
  const out = {}; const transfer = [];
  for (const n of names) {
    out[n] = stems[n].map((ch) => {
      const y = warpChannel(ch, mode, speed, semitones, (p) => post({ type: "progress", stage: "warp", id, done: done + p, total }));
      done++; post({ type: "progress", stage: "warp", id, done, total });
      transfer.push(y.buffer); return y;
    });
  }
  post({ type: "warped", id, stems: out }, transfer);
}

// ------------------------------------------------------------------ tempo detection

function analyze({ mono, sampleRate }) {
  const N = 1024, hop = Math.round(sampleRate / 100); // 100 onset frames per second
  const frames = Math.floor((mono.length - N) / hop);
  const env = new Float32Array(Math.max(1, frames)), low = new Float32Array(Math.max(1, frames));
  const re = new Float64Array(N), im = new Float64Array(N);
  let prev = new Float64Array(N / 2);
  const win = new Float64Array(N); for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N);
  for (let f = 0; f < frames; f++) {
    for (let i = 0; i < N; i++) { re[i] = mono[f * hop + i] * win[i]; im[i] = 0; }
    fft(re, im, false);
    let flux = 0, lowFlux = 0;
    const cur = new Float64Array(N / 2);
    const lowBins = Math.round(150 * N / sampleRate);
    for (let k = 1; k < N / 2; k++) { cur[k] = Math.log1p(100 * Math.hypot(re[k], im[k])); const d = cur[k] - prev[k]; if (d > 0) { flux += d; if (k <= lowBins) lowFlux += d; } }
    env[f] = flux; low[f] = lowFlux; prev = cur;
  }
  // remove slow trend
  const w = 16; const hp = new Float32Array(env.length);
  for (let i = 0; i < env.length; i++) { let s = 0, c = 0; for (let j = Math.max(0, i - w); j < Math.min(env.length, i + w); j++) { s += env[j]; c++; } hp[i] = Math.max(0, env[i] - s / c); }
  // autocorrelation (normalised by overlap) with harmonic reinforcement: a true beat period also
  // correlates at 2x and 4x (half-bar, bar), which suppresses spurious lags
  const maxLag = Math.min(hp.length - 1, 1700);
  const ac = new Float64Array(maxLag + 1);
  for (let lag = 1; lag <= maxLag; lag++) { let s = 0; for (let i = lag; i < hp.length; i++) s += hp[i] * hp[i - lag]; ac[lag] = s / (hp.length - lag); }
  const at = (l) => { const i = Math.floor(l), f = l - i; return i + 1 <= maxLag ? ac[i] * (1 - f) + ac[i + 1] * f : 0; };
  let best = -1, bestLag = 50;
  for (let lag = 30; lag <= 100; lag += 0.25) {
    const bpm = 6000 / lag;
    const wgt = Math.exp(-0.5 * Math.pow(Math.log2(bpm / 115) / 1.0, 2));
    const s = (at(lag) + 0.5 * at(lag * 2) + 0.35 * at(lag * 4) + 0.25 * at(lag / 2)) * wgt;
    if (s > best) { best = s; bestLag = lag; }
  }
  // refine: the beat period should line up across many beats, so fit it against up to 16 beats of lag
  let lag = bestLag, bestR = -1;
  for (let l = bestLag - 3; l <= bestLag + 3; l += 0.01) {
    let s = 0; for (let k = 1; k <= 16 && k * l < maxLag; k++) s += at(k * l);
    if (s > bestR) { bestR = s; lag = l; }
  }
  let bpm = 6000 / lag;
  bpm = Math.round(bpm * 10) / 10;
  // beat phase: the offset whose comb of beats collects the most onset energy
  const P = 6000 / bpm; let bestOff = 0, bestS = -1;
  for (let o = 0; o < P; o += 0.25) {
    let s = 0; for (let t = o; t < hp.length; t += P) s += hp[Math.round(t)] || 0;
    if (s > bestS) { bestS = s; bestOff = o; }
  }
  // downbeat: of the four beats in a bar, pick the one where the kick / bass lands hardest
  let bestBeat = 0, bestLow = -1;
  for (let b = 0; b < 4; b++) {
    let s = 0; for (let t = bestOff + b * P; t < low.length; t += 4 * P) s += Math.max(low[Math.round(t)] || 0, low[Math.round(t) + 1] || 0);
    if (s > bestLow) { bestLow = s; bestBeat = b; }
  }
  const fps = sampleRate / hop;
  const latency = N / 2 / sampleRate;                  // onsets register about half a window early
  let offset = (bestOff + bestBeat * P) / fps + latency;
  const bar = 4 * 60 / bpm; offset = ((offset % bar) + bar) % bar;
  if (offset > bar - 0.03) offset -= bar;
  offset = Math.max(0, offset);
  post({ type: "tempo", bpm, offset });
}

self.onmessage = async (e) => {
  const m = e.data;
  try {
    if (m.type === "load") await loadModel(m);
    else if (m.type === "separate") await separate(m);
    else if (m.type === "warp") warp(m);
    else if (m.type === "analyze") analyze(m);
  } catch (err) {
    post({ type: "error", stage: m.type, message: (err && err.message) || String(err) });
  }
};
