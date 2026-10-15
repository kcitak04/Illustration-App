// Parity test: DATTRACK's own Sample Lab JS (from the vendored dattrack.html) vs the DATLAB C++ port.
// 1. the C++ parameter table (ids, ranges, defaults) must match LAB_PROCS
// 2. every original process, with defaults and alternative modes, must render (near) identical audio
// usage: node parity.mjs <lab_cli binary> <workdir>
import { readFileSync, writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import vm from 'node:vm';
import path from 'node:path';

const [cli, work] = process.argv.slice(2);
const html = readFileSync(new URL('../dattrack/web/dattrack.html', import.meta.url), 'utf8');

// ---- the JS worker ----
const labSrc = html.match(/<script type="text\/plain" id="lab-src">([\s\S]*?)<\/script>/)[1];
const ctx = { Math, Float32Array, Float64Array, Uint32Array, Uint8Array, Map, Set, Array, Error, Date, isFinite, Number, Object, console };
let lastMsg = null;
ctx.postMessage = m => { if (!m.progress && m.progress !== 0) lastMsg = m; };
vm.createContext(ctx);
vm.runInContext(labSrc + '\n;globalThis.__onm = onmessage;', ctx);
const runJS = (proc, p, sr, ch, aux) => { lastMsg = null; ctx.__onm({ data: { id: 1, proc, p, sr, ch: ch.map(c => c.slice()), aux: aux ? aux.map(c => c.slice()) : null } }); return lastMsg; };

// ---- JS parameter table (LAB_PROCS) ----
const a = html.indexOf('const pctP = '), b = html.indexOf('const LAB_CATS');
const P = (k, l, min, max, def, o = {}) => Object.assign({ k, min, max, def }, o);
const jsProcs = new Function('P', 'pct', html.slice(a, b) + '; return LAB_PROCS;')(P, v => v);
const cpp = JSON.parse(execFileSync(cli, ['--defs']).toString());

let fail = 0;
const bad = m => { fail++; console.log('FAIL ' + m); };
for (const pr of jsProcs) {
  const c = cpp[pr.id];
  if (!c) { bad(pr.id + ' missing in C++'); continue; }
  if (c.new) bad(pr.id + ' flagged as new');
  if ((pr.aux || '') !== c.aux) bad(pr.id + ' aux ' + pr.aux + ' vs ' + c.aux);
  for (const pd of pr.params) {
    const q = c.p[pd.k];
    if (!q) { bad(pr.id + '.' + pd.k + ' missing'); continue; }
    const max = pd.options ? pd.options.length - 1 : pd.max;
    if (q[0] !== pd.min || q[1] !== max || q[2] !== pd.def || q[3] !== ((pd.int || pd.options) ? 1 : 0)) bad(`${pr.id}.${pd.k} [${q}] vs [${pd.min},${max},${pd.def},${pd.int || pd.options ? 1 : 0}]`);
  }
  if (Object.keys(c.p).length !== pr.params.length) bad(pr.id + ' param count');
}
const nNew = Object.values(cpp).filter(x => x.new).length;
console.log(`param table: ${jsProcs.length} original processes checked, ${nNew} new processes in C++`);

// ---- audio parity ----
const sr = 22050, n = Math.round(sr * 1.2);
let s = 12345; const rnd = () => { s = (s * 1103515245 + 12345) >>> 0; return s / 4294967296 * 2 - 1; };
const L = new Float32Array(n), R = new Float32Array(n);
for (let i = 0; i < n; i++) {
  const t = i / sr, env = Math.exp(-((t * 4) % 1) * 5);
  L[i] = 0.5 * Math.sin(2 * Math.PI * 220 * t) * env + 0.25 * ((t * 110) % 1 - 0.5) + 0.1 * rnd() * (t > 0.6 ? 1 : 0.2);
  R[i] = 0.45 * Math.sin(2 * Math.PI * 330 * t + 0.3) * env + 0.15 * rnd();
}
const auxN = Math.round(sr * 0.5), A = new Float32Array(auxN);
for (let i = 0; i < auxN; i++) A[i] = Math.exp(-i / sr * 8) * (0.6 * Math.sin(2 * Math.PI * 660 * i / sr) + 0.3 * rnd());
const writePlanar = (file, chans) => { const buf = Buffer.alloc(chans.length * chans[0].length * 4); chans.forEach((c, k) => Buffer.from(c.buffer, c.byteOffset, c.byteLength).copy(buf, k * c.length * 4)); writeFileSync(file, buf); };
const inFile = path.join(work, 'in.f32'), auxFile = path.join(work, 'aux.f32'), outFile = path.join(work, 'out.f32');
writePlanar(inFile, [L, R]); writePlanar(auxFile, [A]);
const readOut = file => {
  const b = readFileSync(file), nl = b.indexOf(10), [nc, len] = b.subarray(0, nl).toString().split(' ').map(Number);
  const body = b.subarray(nl + 1), out = [];
  for (let c = 0; c < nc; c++) out.push(new Float32Array(body.buffer.slice(body.byteOffset + c * len * 4, body.byteOffset + (c + 1) * len * 4)));
  return out;
};

const cases = [];
for (const pr of jsProcs) {
  const def = {}; for (const pd of pr.params) def[pd.k] = pd.def;
  cases.push([pr.id, def]);
  const mode = pr.params.find(pd => pd.k === 'mode');
  if (mode) for (let m = 0; m < mode.options.length; m++) if (m !== mode.def) cases.push([pr.id, Object.assign({}, def, { mode: m })]);
}
cases.push(['tune', { note: 50, scale: 8, prog: 2, width: 40, amount: 0.8 }], ['tune', { note: 50, scale: 7, prog: 3, width: 40, amount: 0.8 }]);
cases.push(['reso', { note: 40, chord: 6, decay: 1, bright: 0.8, mix: 0.5 }], ['freeze', { pos: 0.5, len: 2, shimmer: 0.6, scan: 0.5, seed: 7 }]);

let worst = 0;
for (const [proc, p] of cases) {
  const pr = jsProcs.find(x => x.id === proc);
  const useAux = pr.aux && !(proc === 'mask' && p.mode !== 0);
  const js = runJS(proc, p, sr, [L, R], useAux ? [A] : null);
  if (js.error) { bad(`${proc} JS error ${js.error}`); continue; }
  const args = [proc, inFile, '2', String(sr), outFile, 'raw=1', ...(useAux ? ['aux=' + auxFile + ':1'] : []), ...Object.entries(p).map(([k, v]) => k + '=' + v)];
  try { execFileSync(cli, args); } catch (e) { bad(`${proc} C++ failed ${e.stderr}`); continue; }
  const cc = readOut(outFile);
  const jsc = js.ch;
  if (cc.length !== jsc.length || cc[0].length !== jsc[0].length) { bad(`${proc} ${JSON.stringify(p)} shape C++ ${cc.length}x${cc[0].length} vs JS ${jsc.length}x${jsc[0].length}`); continue; }
  let pk = 0, err = 0;
  for (let c = 0; c < jsc.length; c++) for (let i = 0; i < jsc[c].length; i++) { pk = Math.max(pk, Math.abs(jsc[c][i])); err = Math.max(err, Math.abs(jsc[c][i] - cc[c][i])); }
  const rel = err / Math.max(1e-9, pk);
  worst = Math.max(worst, rel);
  const tag = rel < 1e-3 ? 'ok  ' : 'FAIL';
  if (rel >= 1e-3) fail++;
  console.log(`${tag} ${proc.padEnd(8)} ${JSON.stringify(p).slice(0, 70).padEnd(72)} len ${String(jsc[0].length).padStart(7)}  max err ${rel.toExponential(2)}`);
}
console.log(`\n${cases.length} audio cases, worst relative error ${worst.toExponential(2)}, ${fail} failures`);
process.exit(fail ? 1 : 0);
