// End-to-end test of the DATTRACK plugin core: the real DATTRACK page (headless Chromium) streams its audio
// into DatTrackCore, which a fake host (dattrack_host) drives in real time with a scripted transport.
// The song is emptied and the metronome switched on, so every beat is one click. In SLAVE mode every click
// must land on the host's beat grid, delayed by exactly the latency the plugin reports to the DAW.
// usage: node dattrack_sync_test.mjs <dattrack_host binary> <workdir> [slave|master|off] [extra host args...]
import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);
let pw; try { pw = require('playwright'); } catch (e) { pw = require('/opt/node22/lib/node_modules/playwright'); }

const [bin, work, mode = 'slave', ...extra] = process.argv.slice(2);
const sr = 48000, bpm = 128, playAt = 4, jumpAt = 10, jumpTo = 33, dur = 15;
const out = path.join(work, 'dattrack_out.f32');
const hostArgs = ['--mode', mode, '--sr', String(sr), '--bpm', String(bpm), '--play-at', String(playAt), '--jump-at', String(jumpAt), '--jump-to', String(jumpTo), '--duration', String(dur), '--out', out, ...extra];
const host = spawn(bin, hostArgs);
let url = null, latency = 0, log = '';
const urlReady = new Promise(res => host.stdout.on('data', d => {
  log += d;
  const m = /URL (\S+)/.exec(log); if (m && !url) { url = m[1]; res(); }
  const l = /LATENCY (\d+)/.exec(log); if (l) latency = +l[1];
}));
const done = new Promise(res => host.on('exit', code => res(code)));
await urlReady;

const browser = await pw.chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] });
const page = await browser.newPage({ viewport: { width: 1200, height: 760 } });
const errors = [];
page.on('pageerror', e => errors.push(e.message));
page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
await page.goto(url);
await page.waitForFunction(() => window.DTVST && window.DTVST.started, null, { timeout: 30000 });
// empty song, metronome on
await page.evaluate(() => { setSong(new Song()); state.metro = true; });
if (process.env.NO_UI) await page.evaluate(() => { window.requestAnimationFrame = () => 0; });   // like a closed plugin window
const info = await page.evaluate(() => ({ sr: DTVST.ctx.sampleRate, sink: String(DTVST.ctx.sinkId && DTVST.ctx.sinkId.type || DTVST.ctx.sinkId), state: DTVST.ctx.state }));
console.log('page context:', JSON.stringify(info));
if (mode === 'master') {   // DATTRACK starts itself and reports its beat grid for Link
  await page.waitForTimeout(3000);
  await page.evaluate(() => { song.bpm = 140; play('song'); });
}
await page.evaluate(() => saveProject());   // DATTRACK's own SAVE goes to the plugin (Music/DATTRACK, drag tile)
const c0 = await page.evaluate(() => [DTVST.ctx.currentTime, performance.now()]);
await page.waitForTimeout(6000);
const c1 = await page.evaluate(() => [DTVST.ctx.currentTime, performance.now()]);
console.log('context clock rate vs wall clock:', ((c1[0] - c0[0]) / ((c1[1] - c0[1]) / 1000)).toFixed(4));
const code = await done;
const tail = await page.evaluate(() => ({ log: DTVST.log.slice(-40), host: DTVST.host, playing: sched.playing, bpm: song.bpm }));
await page.screenshot({ path: path.join(work, 'dattrack_page.png') });
await browser.close();
console.log(log);
console.log('page:', JSON.stringify(tail.host), tail.log.join('\n'));
if (errors.length) console.log('page errors:', errors.slice(0, 5));
if (code !== 0) { console.log('host exited with', code); process.exit(1); }

// ---- analysis ----
const raw = readFileSync(out), f = new Float32Array(raw.buffer, raw.byteOffset, raw.length / 4), n = f.length / 2;
const onsets = [];
let lastLoud = -1e9;
for (let i = 0; i < n; i++) {
  const v = Math.max(Math.abs(f[2 * i]), Math.abs(f[2 * i + 1]));
  if (v > 0.02) { if (i - lastLoud > sr * 0.1) onsets.push(i); lastLoud = i; }
}
console.log(`latency reported to the host: ${latency} samples (${(latency / sr * 1000).toFixed(1)} ms), ${onsets.length} clicks detected`);
if (mode !== 'slave') {
  const gaps = onsets.slice(1).map((o, i) => (o - onsets[i]) / sr);
  console.log('click intervals (s):', gaps.slice(0, 12).map(g => g.toFixed(4)).join(' '));
  process.exit(onsets.length > 4 ? 0 : 1);
}
// expected: host sample of every integer beat, plus the reported latency
const block = 256, expected = [];
let ppq = 0, jumped = false;
for (let s = 0; s < dur * sr; s += block) {
  const t = s / sr, playing = t >= playAt;
  if (playing && !jumped && t >= jumpAt) { jumped = true; ppq = jumpTo; }
  if (!playing) continue;
  const p1 = ppq + block / sr * bpm / 60;
  for (let k = Math.ceil(ppq - 1e-9); k < p1; k++) expected.push({ s: s + (k - ppq) / (bpm / 60) * sr + latency, beat: k, afterJump: jumped });
  ppq = p1;
}
const errs = [];
for (const e of expected) {
  if (e.s > n - sr * 0.05) continue;
  let best = null; for (const o of onsets) if (best === null || Math.abs(o - e.s) < Math.abs(best - e.s)) best = o;
  const err = best === null ? Infinity : (best - e.s) / sr * 1000;
  errs.push({ ...e, err });
}
const settled = errs.filter((e, i) => i > 0 && !(e.afterJump && e.beat === jumpTo));   // first beat after start / relocation may be skipped while cueing
for (const e of errs) console.log(`beat ${String(e.beat).padStart(3)}${e.afterJump ? ' (after jump)' : '             '}  click error ${Number.isFinite(e.err) ? e.err.toFixed(2).padStart(7) + ' ms' : 'missing'}`);
const worst = Math.max(...settled.map(e => Math.abs(e.err)));
console.log(`\nworst error over ${settled.length} beats: ${worst.toFixed(2)} ms`);
process.exit(worst < 3 ? 0 : 1);
