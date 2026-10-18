// Regression tests for DATTRACK plugin bugs, run against the real DATTRACK page in headless Chromium:
//   song-race  the song saved in the set must survive start-up (the page's default song must never overwrite it),
//              and a song edit must reach the plugin state (and count as an edit, so the DAW offers to save)
//   stall      when the page stops producing audio the plugin must go silent, not loop its last buffer
//   count-in   a negative host position (count-in / pre-roll) starts the song at row 0 on the downbeat
//   flush      before the plugin reloads the page, an edit made a moment ago is saved first
//   offline    an offline export (faster than real time) must not lose audio: clicks stay evenly spaced
//   loop       the host loops a region: every beat of every pass is on time, none dropped at the wrap, and
//              DATTRACK really jumps back to the loop start (bar accents follow the looped positions)
//   loop-off   the host reports a loop but plays straight through it: DATTRACK must notice and keep following
// usage: node dattrack_regression_test.mjs <dattrack_host binary> <workdir> [scenario ...]
import { spawn } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);
let pw; try { pw = require('playwright'); } catch (e) { pw = require('/opt/node22/lib/node_modules/playwright'); }
const [bin, work, ...only] = process.argv.slice(2);
const sr = 48000;
const results = [];
const check = (name, ok, detail) => { results.push({ name, ok }); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  -  ' + detail : ''}`); };

async function session(hostArgs, pageFn) {
  const host = spawn(bin, ['--sr', String(sr), ...hostArgs]);
  let log = '', url = null;
  const urlReady = new Promise(res => host.stdout.on('data', d => { log += d; const m = /URL (\S+)/.exec(log); if (m && !url) { url = m[1]; res(); } }));
  const done = new Promise(res => host.on('exit', c => res(c)));
  await urlReady;
  const browser = await pw.chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] });
  const page = await browser.newPage({ viewport: { width: 1100, height: 700 } });
  const errors = []; page.on('pageerror', e => errors.push(e.message));
  await page.goto(url);
  await page.waitForFunction(() => window.DTVST && window.DTVST.started, null, { timeout: 30000 });
  await page.evaluate(() => { window.requestAnimationFrame = () => 0; });   // like a closed plugin window: lighter on this VM
  const extra = await pageFn(page);
  const code = await done;
  await browser.close();
  if (errors.length) console.log('  page errors:', errors.slice(0, 3));
  return { code, log, extra };
}
function readOut(file) { const b = readFileSync(file); return new Float32Array(b.buffer, b.byteOffset, b.length / 4); }
function onsets(f, from = 0) {
  const n = f.length / 2, out = []; let lastLoud = -1e9;
  for (let i = from; i < n; i++) { const v = Math.max(Math.abs(f[2 * i]), Math.abs(f[2 * i + 1])); if (v > 0.02) { if (i - lastLoud > sr * 0.1) out.push(i); lastLoud = i; } }
  return out;
}
const want = s => !only.length || only.includes(s);

// ---------------------------------------------------------------- song-race
if (want('song-race')) {
  // 1. make a real DATTRACK song with a recognisable title, as the "song saved in the Ableton set"
  const songFile = path.join(work, 'host-song.json'), songOut = path.join(work, 'song-out.json');
  await session(['--mode', 'off', '--duration', '3', '--out', path.join(work, 'o.f32')], async page => {
    const json = await page.evaluate(() => { const s = new Song(); s.title = 'HOSTSONG'; s.bpm = 141; setSong(s); return JSON.stringify(songToObj(false)); });
    writeFileSync(songFile, json);
  });
  // 2. a fresh plugin instance restoring that song: the page must load it, and the plugin must still hold it
  const r = await session(['--mode', 'off', '--duration', '6', '--out', path.join(work, 'o.f32'), '--song-file', songFile, '--song-out', songOut], async page => {
    await page.waitForTimeout(2500);
    const before = await page.evaluate(() => ({ title: song.title, bpm: song.bpm }));
    await page.evaluate(() => { song.title = 'EDITED'; markDirty(); });   // a user edit
    return before;
  });
  const out = readFileSync(songOut, 'utf8'), edits = +/edits (\d+)/.exec(r.log)[1];
  check('song-race: page loaded the song saved in the set', r.extra.title === 'HOSTSONG' && r.extra.bpm === 141, JSON.stringify(r.extra));
  check('song-race: an edit reaches the plugin state', out.includes('"EDITED"'), out.includes('"EDITED"') ? '' : 'state title: ' + (/"title":"([^"]*)"/.exec(out) || [])[1]);
  check('song-race: the edit marks the set as changed', edits >= 1, 'edits ' + edits);
  // 3. start-up alone is not an edit
  const r2 = await session(['--mode', 'off', '--duration', '4', '--out', path.join(work, 'o.f32'), '--song-file', songFile, '--song-out', songOut], async page => page.waitForTimeout(500));
  const out2 = readFileSync(songOut, 'utf8'), edits2 = +/edits (\d+)/.exec(r2.log)[1];
  check('song-race: start-up does not overwrite or dirty the set', out2.includes('"HOSTSONG"') && edits2 === 0, 'edits ' + edits2);
}

// ---------------------------------------------------------------- flush before reload
if (want('flush')) {
  const songOut = path.join(work, 'song-out.json');
  // edit, then the plugin asks for a reload right away - inside the 1 s "edits paused" window
  const r = await session(['--mode', 'off', '--duration', '7', '--out', path.join(work, 'o.f32'), '--song-out', songOut, '--flush-at', '4.3'], async page => {
    await page.waitForFunction(() => DTVST.host && DTVST.hostSongApplied, null, { timeout: 20000 });
    const t = await page.evaluate(() => DTVST.ctx.currentTime);
    // the host clock started when audio connected; edit just before the flush request
    await page.waitForTimeout(Math.max(0, (4.1 - t) * 1000));
    await page.evaluate(() => { song.title = 'JUSTEDITED'; markDirty(); });
  });
  const out = readFileSync(songOut, 'utf8'), fl = /flush (\d+)\/(\d+)/.exec(r.log);
  check('flush: an edit made just before a reload is saved', out.includes('"JUSTEDITED"'));
  check('flush: the page acknowledges the flush', fl && fl[1] === fl[2] && fl[1] !== '0', fl ? fl[0] : 'no flush line');
}

// ---------------------------------------------------------------- stall
if (want('stall')) {
  const file = path.join(work, 'stall.f32');
  await session(['--mode', 'off', '--duration', '12', '--out', file], async page => {
    await page.evaluate(() => { setSong(new Song()); state.metro = true; play('song'); });
    await page.waitForTimeout(4000);
    await page.evaluate(() => engine.ctx.suspend());     // the page stops producing audio
    await page.waitForTimeout(3000);
    await page.evaluate(() => engine.ctx.resume());
  });
  const f = readOut(file), n = f.length / 2, ons = onsets(f);
  // find the longest stretch without clicks: that is the stall; inside it the output must be silent
  let gapStart = 0, gapLen = 0;
  for (let i = 1; i < ons.length; i++) if (ons[i] - ons[i - 1] > gapLen) { gapLen = ons[i] - ons[i - 1]; gapStart = ons[i - 1]; }
  let maxInGap = 0;
  for (let i = gapStart + Math.round(sr * 0.6); i < gapStart + gapLen - Math.round(sr * 0.1); i++) maxInGap = Math.max(maxInGap, Math.abs(f[2 * i]), Math.abs(f[2 * i + 1]));
  check('stall: output goes silent while the page is stalled', gapLen > sr * 2 && maxInGap < 1e-4, `gap ${(gapLen / sr).toFixed(2)} s, max level in gap ${maxInGap.toExponential(1)}`);
  check('stall: audio comes back after the stall', ons.some(o => o > gapStart + gapLen), `${ons.filter(o => o > gapStart + gapLen).length} clicks after`);
}

// ---------------------------------------------------------------- count-in
if (want('count-in')) {
  const file = path.join(work, 'countin.f32'), bpm = 128, playAt = 3, startPpq = -4;
  const r = await session(['--mode', 'slave', '--bpm', String(bpm), '--play-at', String(playAt), '--start-ppq', String(startPpq), '--duration', '10', '--follow-page', '--out', file], async page => {
    await page.evaluate(() => { setSong(new Song()); state.metro = true; });
  });
  const latency = +/LATENCY (\d+)/.exec(r.log)[1];
  const f = readOut(file), ons = onsets(f);
  const beat0 = (playAt + (0 - startPpq) * 60 / bpm) * sr + latency;   // downbeat of the song, as heard
  const early = ons.filter(o => o < beat0 - sr * 0.05);
  const first = ons.find(o => o >= beat0 - sr * 0.05);
  check('count-in: nothing plays during the count-in', early.length === 0, `${early.length} early clicks`);
  check('count-in: the song starts on the downbeat', first !== undefined && Math.abs(first - beat0) / sr < 0.003, first !== undefined ? `first click ${((first - beat0) / sr * 1000).toFixed(2)} ms from the downbeat` : 'no click');
}

// ---------------------------------------------------------------- offline export
if (want('offline')) {
  const file = path.join(work, 'offline.f32');
  const r = await session(['--mode', 'off', '--duration', '8', '--offline', '--out', file], async page => {
    await page.evaluate(() => { setSong(new Song()); state.metro = true; play('song'); });
  });
  const f = readOut(file), ons = onsets(f);
  const gaps = ons.slice(1).map((o, i) => (o - ons[i]) / sr), mean = gaps.reduce((a, b) => a + b, 0) / Math.max(1, gaps.length);
  const worst = Math.max(...gaps.map(g => Math.abs(g - mean)));
  const wall = +/wall ([\d.]+)/.exec(r.log)[1];
  check('offline: clicks stay evenly spaced (no lost audio)', gaps.length >= 8 && worst < 0.01, `${ons.length} clicks, interval ${mean.toFixed(4)} s, worst deviation ${(worst * 1000).toFixed(1)} ms`);
  check('offline: the export waits for the page instead of racing ahead', wall > 6, `8 s of audio took ${wall.toFixed(1)} s`);
}

// ---------------------------------------------------------------- host loop
if (want('loop')) {
  const file = path.join(work, 'loop.f32'), bpm = 128, playAt = 3, startPpq = 2, ls = 4, le = 10, dur = 14;
  const r = await session(['--mode', 'slave', '--bpm', String(bpm), '--play-at', String(playAt), '--start-ppq', String(startPpq),
                           '--loop-start', String(ls), '--loop-end', String(le), '--duration', String(dur), ...(process.env.REALTIME ? [] : ['--follow-page']), '--out', file], async page => {
    await page.evaluate(() => { setSong(new Song()); state.metro = true; });
    if (process.env.DBG) { await page.waitForTimeout((dur - 1.5) * 1000); console.log(await page.evaluate(() => DTVST.log.join('\n'))); }
  });
  const latency = +/LATENCY (\d+)/.exec(r.log)[1];
  const f = readOut(file), ons = onsets(f);
  const beat = 60 / bpm * sr, first = playAt * sr + latency;
  const expected = [];
  for (let k = 0; first + k * beat < f.length / 2 - sr * 0.2; k++) {
    let u = startPpq + k; if (u >= le) u = ls + (u - le) % (le - ls);
    expected.push({ at: first + k * beat, pos: u });
  }
  let worst = 0, missing = 0;
  const peaks = [];
  for (const e of expected) {
    const o = ons.find(x => Math.abs(x - e.at) < sr * 0.03);
    if (o === undefined) { missing++; if (process.env.DBG) console.log("  missing beat", expected.indexOf(e), "pos", e.pos, "nearest", ((ons.reduce((b, x) => Math.abs(x - e.at) < Math.abs(b - e.at) ? x : b, 0) - e.at) / sr * 1000).toFixed(1), "ms"); continue; }
    worst = Math.max(worst, Math.abs(o - e.at) / sr); if (process.env.DBG) console.log("  beat", expected.indexOf(e), "pos", e.pos, ((o - e.at) / sr * 1000).toFixed(2), "ms");
    let pk = 0; for (let i = o; i < o + sr * 0.02 && i < f.length / 2; i++) pk = Math.max(pk, Math.abs(f[2 * i]));
    peaks.push({ pk, pos: e.pos });
  }
  const extra = ons.filter(o => !expected.some(e => Math.abs(o - e.at) < sr * 0.03)).length;
  const wraps = expected.filter((e, i) => i > 0 && e.pos < expected[i - 1].pos).length;
  check('loop: every beat of every pass is on time', missing === 0 && extra === 0 && worst < 0.005,
        `${expected.length} beats over ${wraps} loop wraps, ${missing} missing, ${extra} extra, worst ${(worst * 1000).toFixed(2)} ms`);
  const mx = Math.max(...peaks.map(p => p.pk)), mn = Math.min(...peaks.map(p => p.pk)), thr = (mx + mn) / 2;
  const wrong = peaks.filter(p => (p.pk > thr) !== (p.pos % 4 === 0)).length;
  check('loop: DATTRACK jumps back to the loop start with the host', wraps >= 2 && mx > mn * 1.3 && wrong === 0,
        `${wrong} of ${peaks.length} beats at the wrong song position (accent check)`);
}

// ---------------------------------------------------------------- host reports a loop but does not wrap
if (want('loop-off')) {
  const file = path.join(work, 'loopoff.f32'), bpm = 128, playAt = 3, startPpq = 2, ls = 4, le = 10, dur = 12;
  const r = await session(['--mode', 'slave', '--bpm', String(bpm), '--play-at', String(playAt), '--start-ppq', String(startPpq),
                           '--loop-start', String(ls), '--loop-end', String(le), '--loop-report-only', '--duration', String(dur), '--follow-page', '--out', file], async page => {
    await page.evaluate(() => { setSong(new Song()); state.metro = true; });
  });
  const latency = +/LATENCY (\d+)/.exec(r.log)[1];
  const f = readOut(file), ons = onsets(f);
  const beat = 60 / bpm * sr, first = playAt * sr + latency;
  // beats from one beat after the (unreal) loop end on must be back on the plain host grid
  const settle = first + (le - startPpq + 1) * beat, expected = [];
  for (let k = 0; first + k * beat < f.length / 2 - sr * 0.2; k++) if (first + k * beat >= settle) expected.push(first + k * beat);
  const hits = expected.filter(at => ons.some(o => Math.abs(o - at) < sr * 0.005)).length;
  check('loop-off: a loop the host does not play is not followed', expected.length >= 6 && hits === expected.length, `${hits}/${expected.length} beats on the host grid after the loop end`);
}

const failed = results.filter(r => !r.ok).length;
console.log(`\n${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
