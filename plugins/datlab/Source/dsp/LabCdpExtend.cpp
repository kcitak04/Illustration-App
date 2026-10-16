// CDP collection, Batch 5 - Extend / loop / iterate / repeat / texture.
// Processes modelled on CDP8's extend, hfperm, texture, freeze, hover, mchiter, mchzig, frfractal, ceracu, echo,
// iterfof, iterline, madrid, shifter, shrink, bounce, cascade, repeater and tesselate programs (LGPL-2.1).
#include "LabCdp.h"
#include "LabDspUtil.h"

namespace lab
{
namespace
{
using namespace lab::dsp;

// total channel-samples an event-based render may write (keeps extreme settings fast)
constexpr double BUDGET = 4e7;

inline long long ms2s (double ms, double sr) { return jr (ms / 1000 * sr); }
inline size_t capLen (const Ctx& cx, double n) { return (size_t) clampd (n, 1, (double) std::max<size_t> (1, cx.maxLen)); }
inline double semis (double st) { return std::pow (2.0, st / 12); }

// adds n output samples of x, read from source position a at speed ratio (negative = backwards), with linear fades
void addPiece (Buf& o, const Buf& x, double a, double ratio, long long at, long long n, double g, long long fi, long long fo)
{
    if (n <= 0 || g == 0) return;
    fi = std::min (fi, n / 2); fo = std::min (fo, n / 2);
    const long long L = (long long) o.size();
    for (long long i = std::max (0LL, -at); i < n; ++i)
    {
        const long long j = at + i; if (j >= L) break;
        double e = g;
        if (i < fi) e *= (double) i / (double) fi;
        if (n - 1 - i < fo) e *= (double) (n - 1 - i) / (double) fo;
        o[(size_t) j] = (float) (o[(size_t) j] + lerpAt (x, a + (double) i * ratio) * e);
    }
}

// ---- segment player: source spans played one after another (backwards when b < a), crossfaded at the joins ----
struct Seg { double a, b; double pan = -1; };
// gains for a pan position: equal power for a mono source, balance for a stereo one
inline std::pair<double, double> panGains (double pan, size_t srcCh)
{
    if (pan < 0) return { 1, 1 };
    pan = clampd (pan, 0, 1);
    if (srcCh == 1) return panG (pan);
    return { std::min (1.0, 2 * (1 - pan)), std::min (1.0, 2 * pan) };
}
Chans playSegs (const Chans& ch, std::vector<Seg> in, double spl, Ctx& cx)
{
    std::vector<Seg> s;   // join contiguous moves in the same direction
    bool stereo = false;
    for (auto& g : in)
    {
        if (std::abs (g.b - g.a) < 2) continue;
        if (g.pan >= 0) stereo = true;
        if (! s.empty() && std::abs (s.back().b - g.a) < 0.5 && ((s.back().b - s.back().a) > 0) == ((g.b - g.a) > 0) && s.back().pan == g.pan) s.back().b = g.b;
        else s.push_back (g);
    }
    if (s.empty()) s.push_back ({ 0, (double) ch[0].size() });
    const size_t n = s.size();
    std::vector<double> J (n + 1, 0.0);   // splice length at each join
    for (size_t i = 1; i < n; ++i) J[i] = std::floor (std::min ({ spl, std::abs (s[i - 1].b - s[i - 1].a) / 3, std::abs (s[i].b - s[i].a) / 3 }));
    double total = 0; for (size_t i = 0; i < n; ++i) total += std::abs (s[i].b - s[i].a) - J[i + 1];
    const size_t outLen = capLen (cx, total + 1);
    const size_t nch = stereo ? 2 : ch.size();
    Chans out (nch, Buf (outLen));
    long long w = 0;
    for (size_t i = 0; i < n && w < (long long) outLen; ++i)
    {
        const double len = std::abs (s[i].b - s[i].a), dir = s[i].b > s[i].a ? 1 : -1;
        const long long N = (long long) std::floor (len);
        auto g = panGains (s[i].pan, ch.size());
        for (size_t c = 0; c < nch; ++c) addPiece (out[c], chOr (ch, c), dir > 0 ? s[i].a : s[i].a - 1, dir, w, N, stereo ? (c == 0 ? g.first : g.second) : 1, (long long) J[i], (long long) J[i + 1]);
        w += N - (long long) J[i + 1];
        if (! (i & 31)) cx.prog ((double) w / (double) outLen);
    }
    return out;
}

// ---- event renderer: transposed copies of (part of) the source placed in time, optional stereo pan ----
struct Hit { double at; double ratio = 1; double gain = 1; double pan = -1; double src0 = 0; double len = -1; double fade = -1; bool rev = false; };
Chans renderHits (const Chans& ch, std::vector<Hit>& hits, size_t outLen, bool stereo, Ctx& cx, double fadeSec = 0.005)
{
    const double sr = cx.sr, L = (double) ch[0].size();
    const size_t nch = stereo ? 2 : ch.size();
    auto outN = [&] (const Hit& h) { double len = h.len < 0 ? L - h.src0 : std::min (h.len, L - h.src0); return std::max (0.0, len / clampd (h.ratio, 1e-3, 1e3)); };
    double work = 0;
    for (auto& h : hits) work += outN (h) * (double) nch;
    const double shrink = work > BUDGET ? BUDGET / work : 1;   // very dense settings: every copy is shortened alike
    if (outLen == 0)
    {
        double e = 1;
        for (auto& h : hits) e = std::max (e, h.at + outN (h) * shrink);
        outLen = capLen (cx, e + 1);
    }
    Chans out (nch, Buf (outLen));
    const long long fd = std::max (1LL, jr (fadeSec * sr));
    for (size_t k = 0; k < hits.size(); ++k)
    {
        const Hit& h = hits[k]; const double r = clampd (h.ratio, 1e-3, 1e3);
        if (h.at >= (double) outLen) continue;
        double len = h.len < 0 ? L - h.src0 : std::min (h.len, L - h.src0);
        long long n = (long long) std::floor (len / r * shrink); if (n < 2) continue;
        const long long f = h.fade >= 0 ? std::max (1LL, jr (h.fade * sr)) : fd;
        const long long at = jr (h.at);
        const double a0 = h.rev ? h.src0 + len - 1 : h.src0, rr = h.rev ? -r : r;
        if (stereo)
        {
            auto g = panGains (h.pan, ch.size());
            addPiece (out[0], ch[0], a0, rr, at, n, h.gain * g.first, f, f);
            addPiece (out[1], chOr (ch, 1), a0, rr, at, n, h.gain * g.second, f, f);
        }
        else for (size_t c = 0; c < nch; ++c) addPiece (out[c], ch[c], a0, rr, at, n, h.gain, f, f);
        if (! (k & 15)) cx.prog ((double) k / (double) hits.size());
    }
    return out;
}
inline void scaleAll (Chans& c, double g) { for (auto& x : c) for (auto& v : x) v = (float) (v * g); }
inline double peakOf (const Chans& c) { return peakAll (c); }

// ======================= extend =======================

// ZIGZAG READ: the read head runs forward and backward between turning points, from the file start to its end
// after CDP extend zigzag (dev/extend/zigzag.c: zigzag, extprepro.c: generate_zigzag_table)
Chans zigCore (const Chans& ch, Ctx& cx, const Params& p, bool pan)
{
    const double sr = cx.sr, L = (double) ch[0].size();
    double a = clampd (p["start"], 0, 1) * L, b = clampd (p["end"], 0, 1) * L; if (b < a) std::swap (a, b);
    const double span = b - a, goal = std::max (span, p["dur"] * sr - (L - b));
    double mn = std::min ((double) ms2s (p["minzig"], sr), span * 0.3), mx = std::max (mn * 1.01, std::min ((double) ms2s (p["maxzig"], sr), span));
    std::vector<double> t { 0 };
    cx.seed (p["seed"]);
    if (span > 64)
    {
        if (p.i ("mode") == 0)
        {
            double here = a, total = a; int dir = 1;
            for (int it = 0; it < 200000 && total < goal; ++it)
            {
                double diff = std::min (mx, dir > 0 ? b - here : here - a);
                dir = -dir;
                if (diff <= mn) continue;
                double r = mn + cx.rand() * (diff - mn);
                here += dir < 0 ? r : -r; total += r; t.push_back (here);
            }
        }
        else
        {   // STEPPED: evenly spaced points visited two forward, one back (and the mirror on the way back)
            const int N = std::max (3, p.i ("points"));
            auto P = [&] (int k) { return a + span * (double) k / (double) (N - 1); };
            t.push_back (P (0)); double total = a; int k = 0, dir = 1, phase = 0;
            for (int it = 0; it < 200000 && total < goal; ++it)
            {
                int nk = phase == 0 ? k + 2 * dir : k - dir;
                if (nk < 0 || nk >= N) { dir = -dir; nk = k + 2 * dir; phase = 0; if (nk < 0 || nk >= N) break; }
                total += std::abs (P (nk) - P (k)); k = nk; phase ^= 1; t.push_back (P (k));
            }
        }
    }
    t.push_back (L);
    std::vector<Seg> s; for (size_t i = 1; i < t.size(); ++i) s.push_back ({ t[i - 1], t[i], pan ? clampd (0.5 + (cx.rand() - 0.5) * p["spread"], 0, 1) : -1 });
    return playSegs (ch, s, (double) ms2s (p["splice"], sr), cx);
}
Chans zigzag (const Chans& ch, Ctx& cx, const Params& p) { return zigCore (ch, cx, p, false); }
// ZIGZAG + PAN: as ZIGZAG READ, each zig thrown to a random position across the stereo field (multichannel version folded to stereo)
// after CDP mchzig zag (dev/standalone/mchzig.c: zigzag)
Chans mchzig (const Chans& ch, Ctx& cx, const Params& p) { return zigCore (ch, cx, p, true); }

// LOOP: a segment of LENGTH is looped, each repeat starting STEP further on (to the end, to a duration or N times)
// after CDP extend loop (dev/extend/extprepro.c: generate_loop_table, played by zigzag.c)
Chans loop (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(); const int mode = p.i ("mode");
    const double spl = (double) ms2s (p["splice"], sr);
    const double len = std::max (16.0, (double) ms2s (p["len"], sr)) + spl;
    double step = (double) ms2s (p["step"], sr); if (mode == 0) step = std::max (step, 0.002 * sr);
    const double scat = (double) ms2s (p["scat"], sr), outGoal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const int reps = mode == 2 ? std::max (1, p.i ("count")) : 1000000;
    cx.seed (p["seed"]);
    std::vector<Seg> s; double start = clampd (p["start"], 0, 1) * L, tmp = start, total = 0;
    for (int i = 0; i < reps; ++i)
    {
        double a = (i == 0 && p.i ("frombeg")) ? 0 : tmp;
        double e = tmp + len;
        if (e >= L) { s.push_back ({ a, L }); break; }
        s.push_back ({ a, e }); total += e - a - spl;
        if (mode == 1 && total >= outGoal) break;
        if (total >= (double) cx.maxLen) break;
        start += step; tmp = start + (scat > 0 ? std::floor (cx.rand() * scat) : 0);
    }
    if (mode == 1 && total < outGoal && ! s.empty() && s.back().b < L) s.back().b = std::min (L, s.back().b + outGoal - total);
    return playSegs (ch, s, spl, cx);
}

// SCRAMBLE: random chunks spliced end to end (RANDOM) or the file cut into chunks and re-permuted again and again (SHRED)
// after CDP extend scramble (dev/extend/extprepro.c: scramble_rand, scramble_shred, permute_chunks)
Chans scramble (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(), spl = (double) ms2s (p["splice"], sr);
    const double outGoal = std::min ((double) cx.maxLen, p["dur"] * sr);
    double mn = (double) ms2s (p["minlen"], sr), mx = (double) ms2s (p["maxlen"], sr); if (mx < mn) std::swap (mn, mx);
    mn = std::max (mn, 3 * spl + 8); mx = std::max (mx, mn);
    cx.seed (p["seed"]);
    std::vector<Seg> s; double out = 0;
    if (p.i ("mode") == 0)
    {
        for (int it = 0; it < 100000 && out < outGoal; ++it)
        {
            double len = std::min (L, mn + std::round (cx.rand() * (mx - mn)));
            double pos = std::round (cx.rand() * std::max (0.0, L - spl - len));
            s.push_back ({ pos, std::min (L, pos + len + spl) }); out += len;
        }
    }
    else
    {
        const int cnt = (int) clampd (std::floor (L / std::max (1.0, mx)), 2, 2000);
        const double unit = std::floor (L / cnt);
        std::vector<double> ptr ((size_t) cnt); std::vector<int> perm ((size_t) cnt);
        for (int it = 0; it < 10000 && out < outGoal; ++it)
        {
            ptr[0] = 0;
            for (int n = 1; n < cnt; ++n) ptr[(size_t) n] = clampd (unit * n + (cx.rand() - 0.5) * p["scatter"] * unit, ptr[(size_t) n - 1] + 2, L - 2);
            for (int n = 0; n < cnt; ++n) { int t = (int) (cx.rand() * (n + 1)); for (int k = n; k > t; --k) perm[(size_t) k] = perm[(size_t) k - 1]; perm[(size_t) t] = n; }
            for (int n = 0; n < cnt && out < outGoal; ++n)
            {
                const int q = perm[(size_t) n]; double a = ptr[(size_t) q], e = q + 1 < cnt ? ptr[(size_t) q + 1] : L;
                double b = std::min (L, e + spl); s.push_back ({ a, b }); out += e - a;
            }
            cx.prog (out / outGoal * 0.2);
        }
    }
    const int keep = p.i ("keep");
    if (! s.empty() && (keep & 1)) { double len = s[0].b - s[0].a; s[0] = { 0, len }; }
    if (! s.empty() && (keep & 2)) { double len = s.back().b - s.back().a; s.back() = { L - len, L }; }
    return playSegs (ch, s, spl, cx);
}

// ITERATE: the whole sound repeated with (randomised) DELAY, random pitch shift, amplitude cut and fade per repeat
// after CDP extend iterate (dev/extend/iterate.c: do_iteration, get_next_writestart, get_gain, get_pshift)
Chans iterCore (const Chans& ch, Ctx& cx, const Params& p, bool pan)
{
    const double sr = cx.sr, L = (double) ch[0].size();
    const double delay = std::max (1.0, (double) ms2s (p["delay"], sr)), outGoal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const int reps = p.i ("mode") == 1 ? std::max (1, p.i ("reps")) : 1 << 30;
    cx.seed (p["seed"]);
    std::vector<Hit> h; h.push_back ({ 0 }); if (pan) h[0].pan = 0.5;
    double w = 0, level = 1, fade = 1 - p["fade"];
    for (int cnt = 1; cnt <= reps && h.size() < 20000; ++cnt)
    {
        w += std::max (1.0, std::round (delay * (1 + (cx.rand() * 2 - 1) * p["rand"])));
        if (p.i ("mode") == 0 && w >= outGoal) break;
        if (w >= (double) cx.maxLen) break;
        level *= fade;
        Hit e { w }; e.ratio = semis ((cx.rand() * 2 - 1) * p["pshift"]); e.gain = (1 - cx.rand() * p["ampcut"]) * level;
        if (pan) e.pan = clampd (0.5 + (cx.rand() - 0.5) * p["spread"], 0, 1);
        h.push_back (e);
    }
    (void) L;
    return renderHits (ch, h, 0, pan, cx, 0.002);
}
Chans iterate (const Chans& ch, Ctx& cx, const Params& p) { return iterCore (ch, cx, p, false); }
// ITERATE + PAN: as FLUID ITERATE, every repeat placed at a random position across the stereo field (multichannel version folded)
// after CDP mchiter iter (dev/standalone/mchiter.c: do_iteration)
Chans mchiter (const Chans& ch, Ctx& cx, const Params& p) { return iterCore (ch, cx, p, true); }

// FREEZE: the sound plays up to START, the START-END segment is iterated fluidly (random delay, pitch, level), then the rest plays on
// after CDP extend freeze / freeze freeze (dev/extend/iterate.c: do_iteration_extend; dev/standalone/freeze.c)
Chans freezeIt (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size();
    double a = clampd (p["start"], 0, 1) * L, b = clampd (p["end"], 0, 1) * L; if (b < a) std::swap (a, b);
    const double spl = std::min ((double) ms2s (15, sr), std::max (1.0, (b - a) / 4));
    if (b - a < 8) { b = std::min (L, a + std::max (8.0, 0.1 * sr)); a = std::max (0.0, b - std::max (8.0, 0.1 * sr)); }
    const double seg = b - a, delay = std::max (2.0, p["delay"] * seg);
    const double outGoal = std::min ((double) cx.maxLen, p["dur"] * sr) - (L - b);
    const int reps = p.i ("mode") == 1 ? std::max (1, p.i ("reps")) : 1 << 30;
    cx.seed (p["seed"]);
    std::vector<Hit> h;
    Hit pre { 0 }; pre.src0 = 0; pre.len = b; pre.fade = spl / sr; h.push_back (pre);   // up to the end of the first iteration
    double w = a + delay - 2 * spl;
    for (int cnt = 1; cnt < reps && h.size() < 20000; ++cnt)
    {
        if (p.i ("mode") == 0 && w >= outGoal) break;
        if (w >= (double) cx.maxLen) break;
        Hit e { w }; e.src0 = a; e.len = seg; e.fade = spl / sr;
        e.ratio = semis ((cx.rand() * 2 - 1) * p["pshift"]); e.gain = 1 - cx.rand() * p["ampcut"];
        h.push_back (e);
        w += std::max (2.0, std::round (delay * (1 + (cx.rand() * 2 - 1) * p["rand"])));
    }
    Hit last { w }; last.src0 = a; last.len = L - a; last.fade = spl / sr; h.push_back (last);   // final copy runs on into the rest of the file
    // the first and final copies must start / end cleanly: give them no fade on the outer edges by re-adding the edges
    Chans out = renderHits (ch, h, 0, false, cx, spl / sr);
    // level of the frozen region matched to the segment's own peak, times GAIN
    double segPk = 0; for (auto& x : ch) for (size_t i = (size_t) a; i < (size_t) b && i < x.size(); ++i) segPk = std::max (segPk, (double) std::abs (x[i]));
    double pk = peakOf (out); if (pk > 1e-9 && segPk > 1e-9) scaleAll (out, segPk / pk * p["gain"]);
    return out;
}

// DRUNKEN WALK: segments of CLOCK length spliced end to end, read positions wandering randomly (STEP) around LOCUS within AMBITUS;
// SOBER mode now and then plays the source straight for a while
// after CDP extend drunk (dev/extend/drunk.c: do_drunken_walk, get_new_pos, get_thisdur, is_paus)
Chans drunkWalk (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size();
    const double spl = std::min ((double) ms2s (15, sr), L / 8);
    const double clock = std::max (2 * spl + 4, p["clock"] * sr), amb = std::max (0.0, p["ambitus"] * sr), stepMax = p["step"] * sr;
    const double locus = clampd (p["locus"], 0, 1) * L, goal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const double ov = p["overlap"];
    cx.seed (p["seed"]);
    const bool sober = p.i ("mode") == 1;
    const int maxDr = std::max (1, p.i ("drunks")), minDr = std::max (1, maxDr / 3);
    int goalCnt = minDr + (int) std::round (cx.rand() * (maxDr - minDr)), pcnt = 0;
    std::vector<Hit> h; double here = locus, w = 0;
    for (int it = 0; it < 50000 && w < goal; ++it)
    {
        double dur;
        bool pause = sober && pcnt++ >= goalCnt;
        if (pause)
        {
            pcnt = 0; goalCnt = minDr + (int) std::round (cx.rand() * (maxDr - minDr));
            dur = p["sober"] >= 30 ? L - here - spl : std::min (p["sober"] * sr * (0.5 + cx.rand() * 0.5), L - here - spl);
            dur = std::max (dur, 2 * spl + 4);
        }
        else
        {
            double rv = cx.rand() * p["clokrand"];
            dur = clock * (1 + rv); if (dur < spl * 2) dur = clock * (1 - rv);
            if (amb > 0)
            {   // drunken step, kept inside the ambitus around the locus
                double step = std::round ((cx.rand() * 2 - 1) * std::min (stepMax, amb));
                double stray = here - locus, ns = stray + step;
                if (ns > amb || ns < -amb) { double other = stray - step; here = (other >= -amb && other <= amb) ? locus + other : locus + std::fmod (std::abs (ns), amb) * (step >= 0 ? 1 : -1); }
                else here += step;
                if (here < 0) here = -here;
            }
            if (here + dur + spl > L) here = std::max (0.0, L - dur - spl);   // bounce off the end
        }
        dur = std::min (dur, L - here); if (dur < 4) { here = 0; dur = std::min (L, clock); }
        Hit e { w }; e.src0 = here; e.len = dur; e.fade = spl / sr; h.push_back (e);
        w += dur - spl - (pause ? 0 : std::round (dur * ov));
        if (pause) here = std::min (L - 1, here + dur);
    }
    return renderHits (ch, h, capLen (cx, w + spl), false, cx, spl / sr);
}

// STEP SEQUENCER: the source replayed on a tempo grid, each step at its own transposition (tape-style: pitch and speed)
// after CDP extend sequence (dev/extend/iterate.c: do_sequence) - the sequence file becomes 8 step sliders
Chans sequence (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, step = divSec (p.i ("div"), cx.bpm) * sr;
    const int steps = clamp (p.i ("steps"), 1, 8), cycles = std::max (1, p.i ("cycles"));
    std::vector<Hit> h;
    for (int c = 0, k = 0; c < cycles; ++c)
        for (int s = 0; s < steps; ++s, ++k)
        {
            Hit e { (double) k * step }; e.ratio = semis (p["s" + std::to_string (s + 1)]);
            e.len = p["gate"] * step * e.ratio; e.gain = s == 0 ? 1 : 0.8;
            if (e.at < (double) cx.maxLen) h.push_back (e);
        }
    return renderHits (ch, h, 0, false, cx, 0.004);
}

// BACK TO BACK: a time-reversed copy of the sound from its end back to JOIN, then the sound forward from JOIN to the end
// after CDP extend baktobak (dev/extend/zigzag.c: do_btob)
Chans baktobak (const Chans& ch, Ctx& cx, const Params& p)
{
    const double L = (double) ch[0].size(), cut = clampd (p["join"], 0, 0.98) * L;
    const double spl = std::min ((double) ms2s (p["splice"], cx.sr), (L - cut) / 3);
    return playSegs (ch, { { L, cut }, { cut, L } }, spl, cx);
}

// DOUBLETS: the sound cut into segments of SEGMENT length (sweeping to END SEG), each repeated REPEATS times;
// SYNC keeps the result in step with the original by skipping ahead
// after CDP extend doublets (dev/extend/iterate.c: do_doubling)
Chans doublets (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(), spl = std::max (2.0, (double) ms2s (15, sr));
    const int reps = std::max (1, p.i ("repets")); const bool sync = p.i ("sync") == 1;
    std::vector<Seg> s; double pos = 0;
    for (int it = 0; it < 100000 && pos < L - 4; ++it)
    {
        double f = pos / L, seg = std::max (3 * spl + 4, (p["seg"] * std::pow (p["segend"] / p["seg"], f)) / 1000 * sr);
        double e = std::min (L, pos + seg);
        for (int r = 0; r < (sync ? std::min (reps, 1 + (int) ((L - pos) / std::max (1.0, seg))) : reps); ++r) s.push_back ({ pos, e });
        pos = sync ? std::min (L, pos + seg * reps) : e;
        if ((double) s.size() * seg > (double) cx.maxLen) break;
    }
    return playSegs (ch, s, spl, cx);
}

// REPETITIONS: the whole source restarted at a series of times (first GAP, each gap ACCEL times the last), fading from LEVEL to END LEVEL
// after CDP extend repetitions (dev/extend/iterate.c: accent_stream) - the times file becomes a generated accelerando/ritardando
Chans repetitions (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const int n = std::max (1, p.i ("count"));
    double gap = (p.i ("sync") ? divSec (p.i ("sync") - 1, cx.bpm) : p["gap"] / 1000) * sr;
    std::vector<Hit> h; double t = 0;
    for (int i = 0; i < n && t < (double) cx.maxLen; ++i)
    {
        Hit e { t }; double f = n > 1 ? (double) i / (n - 1) : 0; e.gain = p["level"] + (p["endlevel"] - p["level"]) * f; e.fade = 0.002;
        h.push_back (e); t += std::max (1.0, gap); gap *= p["accel"];
    }
    return renderHits (ch, h, 0, false, cx, 0.002);
}

// FRACTAL PERMUTATION: each note becomes a sequence of transposed, shortened copies (the permutation), recursively for CYCLES levels;
// CASCADE delays each level by one note, STACK layers them
// after CDP hfperm delperm2 (dev/hfperm/hfperm.c: gen_dp_output, gen_note3) - the permfile becomes 4 (transposition, time-share) pairs
Chans delperm (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const Buf x = monoOf (ch); const double L = (double) x.size();
    const int N = clamp (p.i ("elems"), 2, 4), cyc = clamp (p.i ("cycles"), 1, 4);
    double tr[4], fr[4], fs = 0;
    for (int i = 0; i < N; ++i) { tr[i] = p["t" + std::to_string (i + 1)]; fr[i] = std::max (0.02, p["w" + std::to_string (i + 1)]); fs += fr[i]; }
    for (int i = 0; i < N; ++i) fr[i] /= fs;
    const double D = std::min (L, std::max (0.05 * sr, p["note"] * sr)), spl = std::min ((double) ms2s (3, sr), D / 4);
    std::vector<Hit> h;
    const bool cascade = p.i ("mode") == 0;
    struct E { double off, len, st; };
    std::vector<E> lev { { 0, D, 0 } };
    for (int k = 0; k <= cyc; ++k)
    {
        const double base = cascade ? k * D : 0;
        for (auto& e : lev)
        {
            Hit q { base + e.off }; q.ratio = semis (e.st); q.len = std::min (e.len, D) * q.ratio; q.gain = 1.0 / (cyc + 1); q.fade = std::min (spl, e.len / 3) / sr;
            if (e.len > 4) h.push_back (q);
        }
        if (k == cyc) break;
        std::vector<E> nx;
        for (auto& e : lev) { double o = e.off; for (int i = 0; i < N; ++i) { nx.push_back ({ o, e.len * fr[i], e.st + tr[i] }); o += e.len * fr[i]; } }
        lev.swap (nx);
    }
    Chans m { x };
    return renderHits (m, h, 0, false, cx, 0.002);
}

// ======================= texture =======================
// CDP's texture programs place transposed copies of the source as "notes". The notedata file (source pitch, timing
// motifs, lines, ornaments, motifs, harmonic fields/sets) becomes parameters: the source pitch is tracked, harmony is a
// CHORD on ROOT, timing motifs are rhythm PATTERNs on the tempo grid, lines are random walks on the field, and
// ornaments / motifs are preset shapes.
const std::vector<std::vector<int>> CHORDS { { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 4, 7, 10 }, { 0, 5, 7 }, { 0, 2, 4, 7, 9 },
                                             { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 4, 6, 8, 10 }, { 0, 3, 6, 9 } };
const defs::O CHORD_NAMES { "MAJOR", "MINOR", "MAJ7", "MIN7", "DOM7", "SUS4", "PENTATONIC", "MAJOR SCALE", "MINOR SCALE", "WHOLE TONE", "DIM7" };
const defs::O HARM_MODES { "FIELD", "FIELDS MOVE", "SET", "SETS MOVE", "FREE" };
const int PROG[4] = { 0, 5, 7, -3 };   // root movement of the changing fields / sets (one bar each)

// timing motifs (onsets in grid steps, cycle length in steps)
struct Rhythm { std::vector<double> on; double cycle; };
const std::vector<Rhythm> RHYTHMS { { { 0, 1, 2, 3 }, 4 }, { { 0, 3, 6 }, 8 }, { { 0, 3, 6, 10, 12 }, 16 }, { { 0, 2, 3 }, 4 }, { { 0, 1.5, 2, 3.5 }, 4 },
                                    { { 0, 4, 7, 9, 10, 10.5, 11 }, 12 }, { { 0, 0.5, 1, 4, 4.5, 5 }, 8 }, {} };
const defs::O RHYTHM_NAMES { "STRAIGHT", "TRESILLO", "CLAVE 3-2", "GALLOP", "DOTTED", "ACCEL", "BURSTS", "RANDOM" };

// ornament / motif shapes: (time unit, degree or semitone offset)
struct Shape { std::vector<std::pair<double, double>> n; };
const std::vector<Shape> ORNS { { { { 0, 0 }, { 1, 1 }, { 2, 0 }, { 3, 1 }, { 4, 0 }, { 5, 1 }, { 6, 0 } } }, { { { 0, 0 }, { 1, -1 }, { 2, 0 } } }, { { { 0, 0 }, { 1, 1 }, { 2, 0 } } },
                                { { { 0, 1 }, { 1, 0 }, { 2, -1 }, { 3, 0 } } }, { { { 0, -3 }, { 1, -2 }, { 2, -1 }, { 3, 0 } } }, { { { 0, 0 }, { 1, 2 }, { 2, 4 }, { 3, 7 } } },
                                { { { 0, 0 }, { 1, -1 }, { 2, -2 }, { 3, -3 }, { 4, -4 } } }, { { { 0, -1 }, { 0.5, 0 } } } };
const defs::O ORN_NAMES { "TRILL", "MORDENT", "UP MORDENT", "TURN", "SLIDE", "ARPEGGIO", "RUN DOWN", "GRACE" };
const std::vector<Shape> MOTIFS { { { { 0, 0 }, { 2, -1 } } }, { { { 0, 0 }, { 1, 4 }, { 2, 2 }, { 3, 7 } } }, { { { 0, 0 }, { 1, 4 }, { 1.5, 7 }, { 2, 12 } } },
                                  { { { 0, 7 }, { 1, 4 }, { 2, 2 }, { 3, 0 }, { 4, -3 } } }, { { { 0, 0 }, { 1, 3 }, { 2, 1 }, { 3, 4 }, { 4, 2 }, { 5, 5 } } }, { { { 0, 0 }, { 0.5, 0 }, { 1, 0 }, { 2, 5 } } }, {} };
const defs::O MOTIF_NAMES { "SIGH", "CALL", "FANFARE", "FALL", "ZIGZAG", "KNOCK", "RANDOM" };

// random choice without immediate repetition of the same band (CDP's doperm)
struct Bag
{
    std::vector<int> v; size_t i = 0;
    int draw (int n, Ctx& cx)
    {
        if ((int) v.size() != n || i >= v.size())
        {
            v.resize ((size_t) n); for (int k = 0; k < n; ++k) v[(size_t) k] = k;
            for (int k = n - 1; k > 0; --k) std::swap (v[(size_t) k], v[(size_t) (cx.rand() * (k + 1))]);
            i = 0;
        }
        return v[i++];
    }
};
// CDP pscat: weighted band scatter (outer bands half as wide)
double pscat (double range, double bottom, Bag& b, Ctx& cx)
{
    const int k = b.draw (5, cx); double bw = range / 8, bb;
    if (k == 0) bb = 0; else if (k == 4) bb = 7 * bw; else { bb = bw + (k - 1) * 2 * bw; bw *= 2; }
    return bottom + bb + cx.rand() * bw;
}

struct Harmony
{
    int mode = 4; std::vector<int> iv { 0 }; double root = 60, bar = 2;
    int shift (double t) const { return (mode == 1 || mode == 3) ? PROG[(int) std::floor (std::max (0.0, t) / bar) % 4] : 0; }
    std::vector<double> field (double t) const   // all chord pitches, every octave
    {
        std::vector<double> f; const int s = shift (t);
        for (int o = -1; o < 12; ++o) for (int i : iv) { double q = (double) (o * 12) + std::fmod (root + s + i + 1200, 12.0); if (q >= 0 && q <= 127) f.push_back (q); }
        std::sort (f.begin(), f.end()); return f;
    }
    double snap (double pitch, double t) const
    {
        if (mode == 4) return pitch;
        std::vector<double> f;
        if (mode == 2 || mode == 3) { const int s = shift (t); for (int i : iv) f.push_back (root + s + i); f.push_back (root + s + 12); }
        else f = field (t);
        double best = f[0]; for (double q : f) if (std::abs (q - pitch) < std::abs (best - pitch)) best = q;
        return best;
    }
    double step (double pitch, double k, double t) const   // k degrees along the field (semitones when free)
    {
        if (mode == 4) return pitch + k;
        auto f = field (t); size_t j = 0;
        for (size_t i = 0; i < f.size(); ++i) if (std::abs (f[i] - pitch) < std::abs (f[j] - pitch)) j = i;
        long long q = clamp ((long long) j + (long long) std::llround (k), 0LL, (long long) f.size() - 1);
        return f[(size_t) q];
    }
};

struct TNote { double t, pitch, amp, dur, pos; };

// tracked MIDI pitch of the source (median of the voiced frames of the first 3 s), 60 when unpitched
double sourceMidi (const Chans& ch, Ctx& cx)
{
    Buf m = monoOf (ch); if (m.size() > (size_t) (3 * cx.sr)) m.resize ((size_t) (3 * cx.sr));
    auto pt = trackPitch (m, cx.sr, 50, 1500, 0.15, nullptr);
    std::vector<double> v; for (double f : pt.f0) if (f > 0) v.push_back (69 + 12 * std::log2 (f / 440));
    if (v.size() < 3) return 60;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end()); return clampd (v[v.size() / 2], 20, 110);
}

Chans renderNotes (const Chans& ch, std::vector<TNote>& notes, Ctx& cx, double srcMidi)
{
    std::vector<Hit> h; h.reserve (notes.size());
    const double sr = cx.sr;
    for (auto& n : notes)
    {
        if (! (n.t >= 0) || n.amp <= 0) continue;
        Hit e { n.t * sr }; e.ratio = semis (clampd (n.pitch - srcMidi, -60, 60)); e.gain = n.amp; e.pan = clampd (n.pos, 0, 1);
        e.len = std::max (0.01, n.dur) * sr * e.ratio;
        h.push_back (e);
    }
    std::sort (h.begin(), h.end(), [] (const Hit& a, const Hit& b) { return a.at < b.at; });
    return renderHits (ch, h, 0, true, cx, 0.004);
}

struct TexCommon
{
    Harmony H; double lo = 48, hi = 72, len = 10; double srcMidi = 60;
};
TexCommon texCommon (const Chans& ch, Ctx& cx, const Params& p)
{
    TexCommon c; c.H.mode = clamp (p.i ("mode"), 0, 4); c.H.iv = CHORDS[(size_t) clamp (p.i ("chord"), 0, (int) CHORDS.size() - 1)];
    c.H.root = p["root"]; c.H.bar = 4 * 60.0 / std::max (20.0, cx.bpm);
    c.lo = p["root"]; c.hi = std::min (127.0, p["root"] + std::max (0.0, p["span"])); c.len = std::max (0.2, p["len"]);
    c.srcMidi = sourceMidi (ch, cx);
    cx.seed (p["seed"]);
    return c;
}
// onsets every PACKING seconds, SCATTERed (above 1: groups of round(scatter) onsets placed at random in their joint span)
std::vector<double> packedTimes (double len, double pack, double scat, Ctx& cx)
{
    std::vector<double> t; pack = std::max (0.005, pack);
    for (double x = 0; x < len && t.size() < 20000; x += pack) t.push_back (x);
    if (scat <= 1) { for (size_t i = 1; i < t.size(); ++i) t[i] -= cx.rand() * scat * pack; }
    else
    {
        const size_t g = (size_t) std::max (2.0, std::round (scat));
        for (size_t i = 1; i < t.size(); i += g)
        {
            size_t e = std::min (t.size(), i + g); double base = t[i - 1], span = (e < t.size() ? t[e] : len) - base;
            std::vector<double> r; for (size_t k = i; k < e; ++k) r.push_back (base + cx.rand() * span);
            std::sort (r.begin(), r.end()); for (size_t k = i; k < e; ++k) t[k] = r[k - i];
        }
    }
    std::sort (t.begin(), t.end());
    return t;
}
// onsets from a timing motif repeated every cycle (the skip time)
std::vector<double> timedTimes (double len, int pat, double stepSec, Ctx& cx)
{
    Rhythm r = RHYTHMS[(size_t) clamp (pat, 0, (int) RHYTHMS.size() - 1)];
    if (r.on.empty()) { r.cycle = 16; for (int k = 0; k < 16; ++k) if (k == 0 || cx.rand() < 0.4) r.on.push_back (k); }
    std::vector<double> t; stepSec = std::max (0.01, stepSec);
    for (double c = 0; c < len && t.size() < 20000; c += r.cycle * stepSec) for (double o : r.on) { double x = c + o * stepSec; if (x < len) t.push_back (x); }
    return t;
}
double spreadPos (double spread, Ctx& cx) { return clampd (0.5 + (cx.rand() - 0.5) * spread, 0, 1); }

// SIMPLE / TIMED TEXTURE: one note per onset, random pitch within the range (forced onto the harmony), level, duration and position
// after CDP texture simple / timed and texmchan (dev/texture/texture1.c: make_texture, texture2.c: do_texture, texprepro.c: generate_timeset)
Chans texSimple (const Chans& ch, Ctx& cx, const Params& p, bool timed)
{
    TexCommon c = texCommon (ch, cx, p);
    auto times = timed ? timedTimes (c.len, p.i ("pattern"), divSec (p.i ("div"), cx.bpm), cx) : packedTimes (c.len, p["pack"], p["scatter"], cx);
    Bag bp, bd, ba; std::vector<TNote> notes;
    double dmn = std::min (p["dmin"], p["dmax"]), dmx = std::max (p["dmin"], p["dmax"]);
    for (double t : times)
    {
        double pitch = c.H.snap (pscat (c.hi - c.lo, c.lo, bp, cx), t);
        notes.push_back ({ t, pitch, 1 - p["gainvar"] * pscat (1, 0, ba, cx), pscat (dmx - dmn, dmn, bd, cx), spreadPos (p["spread"], cx) });
    }
    return renderNotes (ch, notes, cx, c.srcMidi);
}
Chans texSimpleFn (const Chans& ch, Ctx& cx, const Params& p) { return texSimple (ch, cx, p, false); }
Chans texTimedFn (const Chans& ch, Ctx& cx, const Params& p) { return texSimple (ch, cx, p, true); }

// amplitude contour of a group: FLAT, CRESC or DECRESC chosen from the CONTOUR set
double groupAmpStep (int contour, int gsize, double amp, Bag& b, Ctx& cx, int& type)
{
    static const int SETS[7][3] = { { 0, 1, -1 }, { 1, -1, -1 }, { 0, -1, -1 }, { 2, -1, -1 }, { 0, 1, -1 }, { 1, 2, -1 }, { 0, 2, -1 } };
    const int* s = SETS[clamp (contour, 0, 6)]; int n = 0; while (n < 3 && s[n] >= 0) ++n;
    type = s[b.draw (n, cx)];
    if (type == 0) return 0;
    double rise = std::min (0.7, amp); double st = rise / (gsize + 1);
    return type == 2 ? -st : st;
}
// group spatialisation: STILL, SCATTER, INWARD, OUTWARD, FOLLOWING, CONTRARY (relative to the texture's motion)
double groupPos (int space, double tpos, double dir, int k, int gsize, Bag& b, Ctx& cx)
{
    double r = 0.6;
    switch (space)
    {
        case 0: return tpos;
        case 1: return clampd (pscat (r, clampd (tpos - r / 2, 0, 1 - r), b, cx), 0, 1);
        case 2: r = tpos > 0.5 ? -std::min (r, tpos - 0.5) : std::min (r, 0.5 - tpos); break;
        case 3: r = tpos >= 0.5 ? r : -r; break;
        case 4: r = dir == 0 ? 0 : r * dir; break;
        default: r = dir == 0 ? 0 : -r * dir; break;
    }
    if (tpos + r > 1) r = 1 - tpos;
    if (tpos + r < 0) r = -tpos;
    return clampd (tpos + r / gsize * k, 0, 1);
}

// GROUPED / TIMED GROUPED TEXTURE: each onset starts a group of up to SIZE notes, SPEED apart, within RANGE semitones,
// with a level CONTOUR and spatial motion (SPACE)
// after CDP texture grouped / tgrouped (dev/texture/texture2.c: do_texture, get_gpparams, texture4.c: set_group_amp, post_place)
Chans texGrouped (const Chans& ch, Ctx& cx, const Params& p, bool timed)
{
    TexCommon c = texCommon (ch, cx, p);
    const double stepSec = divSec (p.i ("div"), cx.bpm);
    auto times = timed ? timedTimes (c.len, p.i ("pattern"), stepSec, cx) : packedTimes (c.len, p["pack"], 0.5, cx);
    const int gmax = std::max (1, p.i ("gsize")), gmin = std::max (1, (gmax + 1) / 2);
    Bag bp, bg, bs, bc, ba, bsp; std::vector<TNote> notes;
    std::vector<double> tpos; for (size_t i = 0; i < times.size(); ++i) tpos.push_back (spreadPos (0.8, cx));
    for (size_t i = 0; i < times.size() && notes.size() < 40000; ++i)
    {
        const double t = times[i], pitch = c.H.snap (pscat (c.hi - c.lo, c.lo, bp, cx), t);
        double amp = 0.6 + 0.4 * cx.rand();
        const int gs = gmin + bs.draw (gmax - gmin + 1, cx);
        double dense = timed ? stepSec / (gs + 1) : clampd (pscat (p["gspeed"] * 0.6, p["gspeed"] * 0.7, bg, cx), 5, 5000) / 1000;
        double range = p["grange"], top = std::min (pitch + range / 2, c.hi), low = std::max (top - range, c.lo); range = top - low;
        int type = 0; double ast = groupAmpStep (p.i ("contour"), gs, amp, bc, cx, type);
        if (type == 1) amp -= gs * ast;
        double dir = i + 1 < times.size() ? (tpos[i + 1] > tpos[i] ? 1 : tpos[i + 1] < tpos[i] ? -1 : 0) : 0;
        double tt = t;
        notes.push_back ({ t, pitch, std::max (0.05, amp), std::max (0.08, dense * 2.5), tpos[i] });
        for (int k = 1; k < gs; ++k)
        {
            tt += dense; amp += ast;
            double q = c.H.snap (range > 0 ? pscat (range, low, ba, cx) : pitch, tt);
            notes.push_back ({ tt, q, clampd (amp, 0.05, 1), std::max (0.08, dense * 2.5), groupPos (p.i ("space"), tpos[i], dir, k, gs, bsp, cx) });
        }
    }
    return renderNotes (ch, notes, cx, c.srcMidi);
}
Chans texGroupedFn (const Chans& ch, Ctx& cx, const Params& p) { return texGrouped (ch, cx, p, false); }
Chans texTGroupedFn (const Chans& ch, Ctx& cx, const Params& p) { return texGrouped (ch, cx, p, true); }

// a melodic line on the tempo grid: a random walk along the harmony inside the range
std::vector<TNote> makeLine (const TexCommon& c, double stepSec, Ctx& cx)
{
    std::vector<TNote> line; double pitch = c.H.snap ((c.lo + c.hi) / 2, 0);
    stepSec = std::max (0.02, stepSec);
    for (double t = 0; t < c.len && line.size() < 5000; t += stepSec)
    {
        double k = std::round ((cx.rand() * 2 - 1) * 2.4); if (k == 0) k = cx.rand() < 0.5 ? 1 : -1;
        double q = c.H.step (pitch, k * (c.H.mode == 4 ? 2 : 1), t);
        if (q > c.hi || q < c.lo) q = c.H.step (pitch, -k * (c.H.mode == 4 ? 2 : 1), t);
        pitch = c.H.snap (clampd (q, c.lo, c.hi), t);
        line.push_back ({ t, pitch, 0.9, stepSec * 0.9, 0.5 });
    }
    return line;
}

// DECORATED LINE: a line on the tempo grid, each note decorated by a group of up to SIZE notes, SPEED apart, within RANGE,
// placed BEFORE, AFTER or EITHER side, pitched CENTRED on, ABOVE or BELOW the line note
// after CDP texture decorated / predecor / postdecor (dev/texture/texture2.c: dec_centre, orientrange; texture4.c: position_and_size_decoration)
Chans texDecor (const Chans& ch, Ctx& cx, const Params& p)
{
    TexCommon c = texCommon (ch, cx, p);
    auto line = makeLine (c, divSec (p.i ("div"), cx.bpm), cx);
    static const int CEN[7][3] = { { 0, -1, -1 }, { 1, -1, -1 }, { 2, -1, -1 }, { 0, 1, -1 }, { 0, 2, -1 }, { 1, 2, -1 }, { 0, 1, 2 } };
    const int* cs = CEN[clamp (p.i ("centre"), 0, 6)]; int ncs = 0; while (ncs < 3 && cs[ncs] >= 0) ++ncs;
    const int gmax = std::max (1, p.i ("gsize")), gmin = std::max (1, (gmax + 1) / 2), place = p.i ("place");
    Bag bo, bs, bpre, bp, bg; std::vector<TNote> notes;
    for (auto& n : line)
    {
        notes.push_back (n); notes.back().pos = spreadPos (0.5, cx);
        const int gs = gmin + bs.draw (gmax - gmin + 1, cx);
        const double dense = clampd (pscat (p["gspeed"] * 0.6, p["gspeed"] * 0.7, bg, cx), 5, 5000) / 1000;
        bool pre = place == 1 || (place == 0 && bpre.draw (2, cx) == 1);
        double range = p["grange"], low;
        int o = cs[bo.draw (ncs, cx)];
        if (o == 0) { double top = n.pitch + range / 2; low = top - range; }
        else if (o == 1) low = n.pitch;
        else low = n.pitch - range;
        double t = pre ? n.t - dense * (gs + 1) : n.t;
        for (int k = 0; k < gs; ++k)
        {
            t += dense;
            double q = pscat (range, low, bp, cx);
            if (o == 0 && range > 0) { bool above = (k & 1) == 0; if (above && q < n.pitch) q = 2 * n.pitch - q; if (! above && q > n.pitch) q = 2 * n.pitch - q; }
            double amp = n.amp * (pre ? 0.4 + 0.5 * (k + 1) / gs : 0.9 - 0.5 * (k + 1) / gs);
            notes.push_back ({ t, c.H.snap (clampd (q, 0, 127), t), amp, std::max (0.08, dense * 2), clampd (notes[notes.size() - 1 - (size_t) k].pos + (cx.rand() - 0.5) * 0.3, 0, 1) });
        }
    }
    return renderNotes (ch, notes, cx, c.srcMidi);
}

// ORNAMENTED LINE: every line note gets a preset ORNAMENT (trill, mordent, turn, slide...), each note SPEED apart (times a random
// multiplier up to VARIATION), placed AFTER the note, BEFORE it (ending on it) or EITHER; steps follow the harmony
// after CDP texture ornate / preornate / postornate (dev/texture/texture2.c: gen_orn_pitch, texture4.c: setup_ornament)
Chans texOrnate (const Chans& ch, Ctx& cx, const Params& p)
{
    TexCommon c = texCommon (ch, cx, p);
    auto line = makeLine (c, divSec (p.i ("div"), cx.bpm), cx);
    const Shape& sh = ORNS[(size_t) clamp (p.i ("shape"), 0, (int) ORNS.size() - 1)];
    const int place = p.i ("place"); Bag bpre; std::vector<TNote> notes;
    const double first = sh.n.front().second, last = sh.n.back().second, tlast = sh.n.back().first;
    for (auto& n : line)
    {
        const double unit = p["speed"] / 1000 * (1 + cx.rand() * (p["mvar"] - 1)), pos = spreadPos (0.6, cx);
        bool pre = place == 1 || (place == 0 && bpre.draw (2, cx) == 1);
        const bool isGrace = p.i ("shape") == 7;
        if (isGrace) pre = true;
        for (size_t k = 0; k < sh.n.size(); ++k)
        {
            double off = sh.n[k].second - (pre ? last : first);
            double t = pre ? n.t - (tlast - sh.n[k].first) * unit : n.t + sh.n[k].first * unit;
            bool main = pre ? k + 1 == sh.n.size() : k == 0;
            double amp = main ? n.amp : n.amp * 0.75;
            notes.push_back ({ t, c.H.step (n.pitch, off, t), amp, main ? n.dur : std::max (0.06, unit * 1.6), pos });
        }
    }
    return renderNotes (ch, notes, cx, c.srcMidi);
}

// MOTIF TEXTURE: each onset (PACKING/SCATTER, or a timing PATTERN) plays a preset MOTIF, SPEED per step; FIRST forces only the
// motif's first note onto the harmony (texture motifs), ALL forces every note (texture motifsin)
// after CDP texture motifs / motifsin / tmotifs / tmotifsin (dev/texture/texture2.c: gen_mtf_pitch, texture3.c: generate_motifin_note_pitch)
Chans texMotifs (const Chans& ch, Ctx& cx, const Params& p, bool timed)
{
    TexCommon c = texCommon (ch, cx, p);
    auto times = timed ? timedTimes (c.len, p.i ("pattern"), divSec (p.i ("div"), cx.bpm), cx) : packedTimes (c.len, p["pack"], p["scatter"], cx);
    const int si = clamp (p.i ("shape"), 0, (int) MOTIFS.size() - 1); const bool all = p.i ("force") == 1;
    Bag bp; std::vector<TNote> notes;
    for (double t0 : times)
    {
        Shape sh = MOTIFS[(size_t) si];
        if (sh.n.empty()) { int m = 3 + (int) (cx.rand() * 4); double q = 0; for (int k = 0; k < m; ++k) { sh.n.push_back ({ (double) k, q }); q += std::round ((cx.rand() * 2 - 1) * 5); } }
        const double unit = p["speed"] / 1000 * (0.75 + cx.rand() * 0.5), base = c.H.snap (pscat (c.hi - c.lo, c.lo, bp, cx), t0), pos = spreadPos (p["spread"], cx);
        const double amp = 0.6 + 0.4 * cx.rand();
        for (size_t k = 0; k < sh.n.size() && notes.size() < 40000; ++k)
        {
            double t = t0 + sh.n[k].first * unit, q = base + sh.n[k].second - sh.n[0].second;
            while (q > 127) q -= 12;
            while (q < 0) q += 12;
            if (all) q = c.H.snap (q, t);
            double nextT = k + 1 < sh.n.size() ? sh.n[k + 1].first * unit : unit * 2;
            notes.push_back ({ t, q, amp * (k == 0 ? 1 : 0.85), std::max (0.06, (nextT - sh.n[k].first * unit) * 1.5 + 0.03), pos });
        }
    }
    return renderNotes (ch, notes, cx, c.srcMidi);
}
Chans texMotifsFn (const Chans& ch, Ctx& cx, const Params& p) { return texMotifs (ch, cx, p, false); }
Chans texTMotifsFn (const Chans& ch, Ctx& cx, const Params& p) { return texMotifs (ch, cx, p, true); }

// ======================= hover, fractal, repeat streams =======================

// HOVER: zigzag reading around a location at a rate FREQ (half a cycle forward, half back), the location sweeping from LOCATION
// to END LOC; FREQ RAND / LOC RAND randomise each cycle. Low rates rock, audio rates turn into pitched buzz
// after CDP hover hover (dev/standalone/hover.c: hover)
Chans hover (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(), goal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const double T0 = sr / std::max (0.1, p["frq"]), spl = (double) ms2s (p["splice"], sr);
    cx.seed (p["seed"]);
    std::vector<Seg> s; double w = 0;
    for (int it = 0; it < 400000 && w < goal; ++it)
    {
        double T = clampd (T0 * (1 + p["frqrand"] * (cx.rand() * 2 - 1)), 8, 2 * L);
        double f = w / std::max (1.0, goal), loc = (p["loc"] + (p["locend"] - p["loc"]) * f) * L + T * p["locrand"] * (cx.rand() * 2 - 1);
        double a = clampd (loc - T / 4, 0, std::max (0.0, L - T / 2 - 1)), b = std::min (L, a + T / 2);
        s.push_back ({ a, b }); s.push_back ({ b, a });
        w += 2 * (b - a) - 2 * std::min (spl, (b - a) / 3);
    }
    return playSegs (ch, s, spl, cx);
}

// HOVER WAVES: from a zero crossing near the location, a quarter cycle is read forward, back, then the same inverted:
// symmetrical zero-centred waveforms at FREQ; NORMALISE evens their level
// after CDP hover2 hover2 (dev/standnew/hover2.c: hover2)
Chans hover2 (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const Buf m = monoOf (ch); const long long L = (long long) m.size();
    const size_t outLen = capLen (cx, p["dur"] * sr);
    Chans out (ch.size(), Buf (outLen));
    cx.seed (p["seed"]);
    const long long spl0 = std::max (1LL, ms2s (2, sr));
    size_t w = 0;
    std::vector<double> k;
    for (int it = 0; it < 1000000 && w < outLen; ++it)
    {
        double T = sr / std::max (0.1, p["frq"]); T *= 1 + p["frqrand"] * (cx.rand() * 2 - 1);
        long long q = std::max (1LL, (long long) std::round (T / 4));
        double f = (double) w / (double) outLen;
        long long loc = (long long) ((p["loc"] + (p["locend"] - p["loc"]) * f) * (double) L + T * p["locrand"] * (cx.rand() * 2 - 1));
        loc = clamp (loc, 0LL, L - 1);
        long long i = loc;   // find a downward zero crossing
        while (i < L && m[(size_t) i] <= 0) ++i;
        while (i < L && m[(size_t) i] > 0) ++i;
        if (i + q >= L) { i = 0; while (i < L && m[(size_t) i] <= 0) ++i; while (i < L && m[(size_t) i] > 0) ++i; if (i + q >= L) i = std::max (0LL, L - q - 1); }
        if (q >= L) q = std::max (1LL, L - 1);
        const long long sp = std::min (q / 2, spl0);
        for (size_t c = 0; c < ch.size(); ++c)
        {
            k.assign ((size_t) q + 1, 0.0);
            double mx = 0;
            for (long long n = 0; n <= q; ++n) { k[(size_t) n] = (i + n < L) ? ch[c][(size_t) (i + n)] : 0; mx = std::max (mx, std::abs (k[(size_t) n])); }
            if (p.i ("norm") && mx > 1e-9) for (auto& v : k) v *= 0.9 / mx;
            for (long long n = 0; n < sp; ++n) { double g = (double) n / (double) sp; k[(size_t) n] *= g; k[(size_t) (q - n)] *= g; }
            size_t o = w;
            auto put = [&] (double v) { if (o < outLen) out[c][o] = (float) v; ++o; };
            for (long long n = 0; n <= q; ++n) put (k[(size_t) n]);
            for (long long n = q - 1; n > 0; --n) put (k[(size_t) n]);
            for (long long n = 0; n <= q; ++n) put (-k[(size_t) n]);
            for (long long n = q - 1; n > 0; --n) put (-k[(size_t) n]);
        }
        w += (size_t) (4 * q);
        if (! (it & 255)) cx.prog ((double) w / (double) outLen);
    }
    return out;
}

// FRACTALISE: the sound is cut into 2^(LAYERS-1) segments; at every layer each group is followed by its mirror image (segment order
// reversed), halving the group size each time, so the sound is folded into itself at ever smaller scales
// after CDP frfractal / fractal fractal (dev/standnew/frfractal.c: generate_fractal_pattern)
Chans frfractal (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(); const int depth = clamp (p.i ("layers"), 1, 10);
    const double spl = (double) ms2s (p["splice"], sr);
    int segcnt = 1; for (int n = 1; n < depth; ++n) segcnt *= 2;
    while (segcnt > 1 && L / segcnt < 3 * spl + 8) segcnt /= 2;
    const double seg = std::floor (L / segcnt);
    std::vector<int> p1 ((size_t) segcnt), p2;
    for (int n = 0; n < segcnt; ++n) p1[(size_t) n] = n;
    int grp = segcnt; size_t cuts = 1;
    for (int n = 0; n < depth && grp >= 1; ++n)
    {
        p2.clear();
        for (size_t j = 0, at = 0; j < cuts && at + (size_t) grp <= p1.size(); ++j, at += (size_t) grp)
        {
            for (int m = 0; m < grp; ++m) p2.push_back (p1[at + (size_t) m]);
            for (int m = grp - 1; m >= 0; --m) p2.push_back (p1[at + (size_t) m]);
        }
        p1.swap (p2);
        if ((double) p1.size() * seg > (double) cx.maxLen * 2) break;
        if (grp == 1) break;
        grp /= 2; cuts *= 4;
    }
    std::vector<Seg> s;
    for (int q : p1) s.push_back ({ q * seg, (q + 1) * seg });
    if (seg * segcnt < L) s.push_back ({ seg * segcnt, L });
    return playSegs (ch, s, spl, cx);
}

// POLY CYCLES: the sound repeats in up to 4 streams spread across the stereo field, stream N repeating COUNT N times per resync
// cycle (e.g. 10:12:15), the fastest every CYCLE; whole cycles are output until LENGTH. ECHO adds a delayed copy, mirrored if wanted
// after CDP ceracu ceracu (dev/new/ceracu.c: ceracu) - multichannel folded to stereo
Chans ceracu (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size();
    std::vector<int> c; for (int i = 1; i <= 4; ++i) if (p.i ("c" + std::to_string (i)) > 0) c.push_back (p.i ("c" + std::to_string (i)));
    if (c.empty()) c.push_back (1);
    const int M = *std::max_element (c.begin(), c.end());
    double minc = p.i ("sync") ? divSec (p.i ("sync") - 1, cx.bpm) * sr : p["mincyc"] / 1000 * sr; if (minc < 16) minc = L;
    const double cyc = minc * M; int ncyc = std::max (1, (int) std::ceil (p["dur"] * sr / cyc));
    while (ncyc > 1 && (double) ncyc * cyc > (double) cx.maxLen) --ncyc;
    std::vector<Hit> h; const double echo = p["echo"] * sr;
    for (size_t si = 0; si < c.size(); ++si)
    {
        const double per = cyc / c[si], pan = c.size() == 1 ? 0.5 : (double) si / (double) (c.size() - 1);
        for (long long k = 0; k < (long long) ncyc * c[si] && h.size() < 40000; ++k)
        {
            Hit e { (double) k * per }; e.pan = pan; h.push_back (e);
            if (echo > 0) { Hit q = e; q.at += echo; q.pan = p.i ("epan") ? 1 - pan : pan; q.gain = 0.7; h.push_back (q); }
        }
    }
    return renderHits (ch, h, 0, true, cx, 0.003);
}

// DISCRETE ECHO: the whole sound repeated after DELAY (never overlapping: at least the sound's length), each repeat ATTEN quieter,
// RANDOM varies the gaps; stops at LENGTH or when the echoes fall below CUTOFF
// after CDP sfecho echo (dev/new/echo.c: echo)
Chans sfecho (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(), goal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const double cut = std::pow (10.0, p["cutoff"] / 20), pk = std::max (1e-9, peakAll (ch));
    cx.seed (p["seed"]);
    std::vector<Hit> h; double t = 0, att = 1;
    for (int k = 0; k < 10000; ++k)
    {
        if (k > 0) { att *= p["atten"]; if (att * pk <= cut * pk || att <= 1e-5) break; }
        if (k > 0 && t + L > goal) break;
        Hit e { t }; e.gain = att; e.fade = 0.001; h.push_back (e);
        double gap = std::max (0.0, p["delay"] * sr - L) * (1 + (cx.rand() * 2 - 1) * p["rand"]);
        t += L + std::max (0.0, gap);
    }
    return renderHits (ch, h, 0, false, cx, 0.001);
}

// pitch line from 4 points (MIDI or semitones) spread over the length: GLIDE interpolates, STEPS holds
double lineAt (const double* v, int n, double f, bool glide)
{
    double x = clampd (f, 0, 1) * (n - 1); int i = (int) std::floor (x);
    if (i >= n - 1) return v[n - 1];
    return glide ? v[i] + (v[i + 1] - v[i]) * (x - i) : v[(int) std::floor (clampd (f, 0, 0.9999) * n)];
}

// FOF LINE: a grain (POSITION, GRAIN size) iterated as a pulse train whose rate follows a melody (NOTE 1-4, one per RATE step,
// GLIDE or STEPS), with VIBRATO and random AMP CUT: any grain becomes a pitched, sung line
// after CDP iterfof iterfof modes 3-4 (dev/new/iterfof.c: iterfof) - the pitch line becomes 4 note sliders on the tempo grid
Chans iterfof (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(), goal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const double a = clampd (p["pos"], 0, 1) * (L - 1), g = std::max (8.0, std::min (L - a, (double) ms2s (p["size"], sr)));
    const double step = divSec (p.i ("div"), cx.bpm) * sr; const bool glide = p.i ("mode") == 0;
    const double notes[4] = { p["n1"], p["n2"], p["n3"], p["n4"] };
    cx.seed (p["seed"]);
    std::vector<Hit> h; double t = 0, vr = 4 + cx.rand() * 3, ph = 0; int lastStep = -1;
    for (int it = 0; it < 400000 && t < goal; ++it)
    {
        const double sp = t / std::max (1.0, step); const int si = (int) std::floor (sp);
        if (si != lastStep) { lastStep = si; vr = 4 + cx.rand() * 3; }
        const int i0 = si % 4, i1 = (si + 1) % 4; const double fr = sp - si;
        double midi = glide ? notes[i0] + (notes[i1] - notes[i0]) * std::pow (fr, 4) : notes[i0];
        midi += p["vib"] * std::sin (ph);
        const double f0 = 440 * std::pow (2.0, (midi - 69) / 12), per = std::max (4.0, sr / f0);
        Hit e { t }; e.src0 = a; e.len = g; e.gain = 1 - cx.rand() * p["ampcut"]; e.fade = std::min (0.002, g / sr / 4); h.push_back (e);
        t += per; ph += TAU * vr * per / sr;
    }
    return renderHits (ch, h, capLen (cx, goal + g), false, cx, 0.001);
}

// ITERATE ON A LINE: the sound iterated every DELAY (RANDOM varies it), its transposition following a line through TRANSP 1-4
// over the length (GLIDE or STEPS), plus random PITCH scatter and AMP CUT
// after CDP iterline iterline / iterlinef (dev/new/iterline.c, iterlinef.c: iterline)
Chans iterline (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, goal = std::min ((double) cx.maxLen, p["dur"] * sr), delay = std::max (1.0, (double) ms2s (p["delay"], sr));
    const double tr[4] = { p["t1"], p["t2"], p["t3"], p["t4"] };
    cx.seed (p["seed"]);
    std::vector<Hit> h; double w = 0;
    for (int it = 0; it < 20000 && w < goal; ++it)
    {
        Hit e { w }; e.ratio = semis (lineAt (tr, 4, w / goal, p.i ("mode") == 0) + (cx.rand() * 2 - 1) * p["pshift"]);
        e.gain = 1 - cx.rand() * p["ampcut"]; h.push_back (e);
        w += std::max (1.0, std::round (delay * (1 + (cx.rand() * 2 - 1) * p["rand"])));
    }
    return renderHits (ch, h, 0, false, cx, 0.002);
}

// SPATIAL SYNCOPATION: the sound repeats every STEP in STREAMS positions across the stereo field; at each step events are randomly
// deleted from the streams (DELETE; above 1 more singles, EMPTY allows silent steps), so accents and syncopations emerge
// after CDP madrid madrid (dev/new/madrid.c: madrid) - multichannel folded to stereo
Chans madrid (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, goal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const int S = clamp (p.i ("streams"), 2, 8); const double del = p["delfact"];
    const double step = std::max (0.005 * sr, p.i ("sync") ? divSec (p.i ("sync") - 1, cx.bpm) * sr : p["step"] / 1000 * sr);
    cx.seed (p["seed"]);
    std::vector<Hit> h; std::vector<int> keep;
    for (double t = 0; t < goal && h.size() < 40000; t += std::max (1.0, step * (1 + (cx.rand() * 2 - 1) * p["rand"])))
    {
        keep.clear();
        for (int s = 0; s < S; ++s) if (cx.rand() >= std::min (1.0, del)) keep.push_back (s);
        if (del > 1 && cx.rand() < del - 1)
        {
            if (p.i ("empty") && cx.rand() < 0.5) keep.clear();
            else { int one = (int) (cx.rand() * S); keep.assign (1, std::min (one, S - 1)); }
        }
        if (keep.empty() && ! p.i ("empty")) keep.push_back (std::min (S - 1, (int) (cx.rand() * S)));
        for (int s : keep) { Hit e { t }; e.pan = (double) s / (S - 1); h.push_back (e); }
    }
    return renderHits (ch, h, 0, true, cx, 0.003);
}

// FOCUS SHIFTER: up to 4 simultaneous repetition streams with BEATS 1-4 per CYCLE, spread across the stereo field; the loudness
// focus (BOOST) rests on one stream for LINGER cycles then moves to the next over TRANSIT cycles, in turn, zigzag or random order
// after CDP shifter shifter (dev/new/shifter.c: shifter) - multichannel folded to stereo
Chans shifter (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, goal = std::min ((double) cx.maxLen, p["dur"] * sr);
    std::vector<int> c; for (int i = 1; i <= 4; ++i) if (p.i ("c" + std::to_string (i)) > 0) c.push_back (p.i ("c" + std::to_string (i)));
    if (c.empty()) c.push_back (4);
    const int n = (int) c.size();
    const double cyc = std::max (0.05 * sr, p.i ("sync") ? (double) (1 << (p.i ("sync") - 1)) * 4 * 60 / std::max (20.0, cx.bpm) * sr : p["cycdur"] * sr);
    const double linger = std::max (0.0, p["linger"]), transit = std::max (0.0, p["transit"]), per = std::max (0.25, linger + transit);
    cx.seed (p["seed"]);
    std::vector<int> order; int zig = 1, cur = 0;
    auto nextFocus = [&] ()
    {
        switch (p.i ("order"))
        {
            case 1: if (n > 1) { if (cur + zig >= n || cur + zig < 0) zig = -zig; cur += zig; } return cur;
            case 2:
                if (order.empty()) { for (int i = 0; i < n; ++i) order.push_back (i); for (int i = n - 1; i > 0; --i) std::swap (order[(size_t) i], order[(size_t) (cx.rand() * (i + 1))]); }
                cur = order.back(); order.pop_back(); return cur;
            default: cur = (cur + 1) % n; return cur;
        }
    };
    std::vector<int> foci { 0 }; for (int k = 0; k < 4096 && (double) k * per * cyc < goal + cyc * per; ++k) foci.push_back (nextFocus());
    auto focusW = [&] (int s, double tc)   // weight of stream s at time tc (in cycles)
    {
        int k = (int) std::floor (tc / per); double r = tc - k * per;
        k = clamp (k, 0, (int) foci.size() - 2);
        if (r < linger || transit <= 0) return foci[(size_t) k] == s ? 1.0 : 0.0;
        double x = (r - linger) / transit;
        return (foci[(size_t) k] == s ? 1 - x : 0.0) + (foci[(size_t) k + 1] == s ? x : 0.0);
    };
    std::vector<Hit> h;
    for (int s = 0; s < n; ++s)
    {
        const double beat = cyc / c[(size_t) s], pan = n == 1 ? 0.5 : (double) s / (n - 1);
        for (double t = 0; t < goal && h.size() < 40000; t += beat)
        { Hit e { t }; e.pan = pan; e.gain = (1 + p["boost"] * focusW (s, t / cyc)) / (1 + p["boost"]); h.push_back (e); }
    }
    return renderHits (ch, h, 0, true, cx, 0.003);
}

// SHRINK: the sound repeated, each copy shortened by SHRINK (cut from the END, around the MIDDLE, from the START or around TIME)
// and the gaps contracting by CONTRACT: an accelerating, shrinking stream. Below SMALLEST / MIN GAP lengths and gaps hold
// (RANDOM then jitters the timing); INVERT reverses every copy
// after CDP shrink shrink 1-4 (dev/new/shrink.c: shrink)
Chans shrink (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(), goal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const double spl = std::max (1.0, (double) ms2s (p["splice"], sr));
    const double small = std::max (2 * spl + 4, (double) ms2s (p["small"], sr)), minGap = std::max (4.0, (double) ms2s (p["mingap"], sr));
    const int mode = p.i ("mode");
    cx.seed (p["seed"]);
    double len = L, gap = std::max (minGap, p["gap"] * sr), t = 0;
    const double centre = mode == 1 ? L / 2 : clampd (p["time"], 0, 1) * L;
    std::vector<Hit> h;
    for (int k = 0; k < 20000 && t < goal; ++k)
    {
        double a;
        switch (mode) { case 0: a = 0; break; case 2: a = L - len; break; default: a = clampd (centre - len / 2, 0, L - len); break; }
        // events never overlap: an element longer than its gap is cut to the gap (keeps long sources usable)
        const double el = std::max (2 * spl + 4, std::min (len, gap));
        if (mode == 2) a = L - el; else if (mode != 0) a = clampd (centre - el / 2, 0, L - el);
        Hit e { t }; e.src0 = a; e.len = el; e.fade = spl / sr; e.rev = p.i ("inverse") == 1; h.push_back (e);
        double g = gap;
        if (g <= minGap) g = minGap * (1 + (cx.rand() * 2 - 1) * p["rnd"] * 0.5);
        t += std::max (4.0, g);
        len = std::max (std::min (small, L), len * p["shrinkage"]);
        gap = std::max (minGap, gap * p["contract"]);
    }
    return renderHits (ch, h, 0, false, cx, spl / sr);
}

// BOUNCE: the sound repeated COUNT times like a dropped ball: the first after START GAP, each gap SHORTEN times the last, level falling
// to END LEVEL (WARP shapes the decay); MIN DUR shrinks the elements with the gaps, CUT stops them overlapping
// after CDP bounce bounce (dev/science/bounce.c: bounce)
Chans bounce (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(); const int N = std::max (1, p.i ("count"));
    const double minstep = std::max (8.0, (double) ms2s (6, sr));
    std::vector<Hit> h; double t = 0, ratio = p["startgap"];
    Hit first { 0 }; h.push_back (first);
    double gap = ratio * sr;
    for (int b = 1; b <= N; ++b)
    {
        t += gap; if (t >= (double) cx.maxLen) break;
        double lev = (1 - p["endlevel"]) * std::pow ((double) (N - b) / N, p["ewarp"]) + p["endlevel"];
        Hit e { t }; e.gain = lev;
        ratio *= p["shorten"]; gap = ratio * sr;
        if (p["mindur"] > 0) e.len = std::max (p["mindur"] * sr, L * std::pow (p["shorten"], b));
        if (p.i ("cut")) e.len = std::min (e.len < 0 ? L : e.len, gap);
        if (e.len >= 0 && p.i ("trimstart")) { double l = std::min (e.len, L); e.src0 = L - l; e.len = l; }
        h.push_back (e);
        if (gap < minstep) break;
    }
    if (p.i ("cut")) h[0].len = std::min (L, p["startgap"] * sr);
    return renderHits (ch, h, 0, false, cx, 0.003);
}

// simple onset detector: spectral-flux-like energy rise in 10 ms frames, returns sample positions (always starts with 0)
std::vector<double> onsets (const Buf& m, double sr, double minGap)
{
    const size_t hop = (size_t) std::max (32.0, 0.01 * sr); std::vector<double> e;
    for (size_t i = 0; i < m.size(); i += hop) { double s = 0; for (size_t k = i; k < std::min (m.size(), i + hop); ++k) s += (double) m[k] * m[k]; e.push_back (std::sqrt (s / (double) hop)); }
    std::vector<double> o { 0 }; double avg = 0;
    for (size_t i = 1; i < e.size(); ++i)
    {
        avg = avg * 0.9 + e[i - 1] * 0.1;
        double t = (double) (i * hop);
        if (e[i] > 1.6 * avg + 1e-4 && e[i] > e[i - 1] * 1.3 && t - o.back() >= minGap) o.push_back (t);
    }
    return o;
}

// CASCADE: successive CLIP-long segments (random up to CLIP MAX, or cut at ONSETS) each echoed ECHOES times (up to ECHO MAX), the
// echo sets overlapping; RANDOM shifts the echoes, decay LOG or LINEAR; panned: SAME channels, LEFT>RIGHT, CENTRE out alternately,
// STEP RIGHT or STEP ALT across 8 positions (8-channel modes folded). SHRED re-cuts each echo into SHRED N shuffled parts
// after CDP cascade cascade 1-10 (dev/science/cascade.c: cascade)
Chans cascade (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const int mode = p.i ("mode");
    const Chans src = mode == 0 ? ch : Chans { monoOf (ch) };
    const double L = (double) src[0].size(), spl = std::max (1.0, (double) ms2s (5, sr));
    const double cmin = std::max (2 * spl + 2, (double) ms2s (p["clip"], sr)), cmax = p["clipmax"] > 0 ? std::max (cmin, (double) ms2s (p["clipmax"], sr)) : cmin;
    const int emin = std::max (1, p.i ("echos")), emax = p.i ("echosmax") > 0 ? std::max (emin, p.i ("echosmax")) : emin;
    const int shredN = p.i ("shredno"), shredC = p.i ("shredcnt");
    cx.seed (p["seed"]);
    std::vector<double> cuts;
    if (p.i ("cuts") == 1) { cuts = onsets (monoOf (ch), sr, cmin); }
    else { for (double t = 0; t < L - 2 && cuts.size() < 100000; ) { cuts.push_back (t); t += cmin + cx.rand() * (cmax - cmin); } }
    cuts.push_back (L);
    // output length: last clip start + its echoes
    double total = 0;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) total = std::max (total, cuts[i] + (cuts[i + 1] - cuts[i] + spl) * (emax + 1));
    const size_t outLen = capLen (cx, total + 1), nch = mode == 0 ? src.size() : 2;
    Chans out (nch, Buf (outLen));
    double work = 0; for (size_t i = 0; i + 1 < cuts.size(); ++i) work += (cuts[i + 1] - cuts[i] + spl) * (emax + 1) * (double) nch;
    const double budgetCut = work > BUDGET ? BUDGET / work : 1;
    std::vector<Buf> clip (src.size());
    int panDir = 1;
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
    {
        const double a = cuts[i], clen = std::min (L - a, cuts[i + 1] - a + spl);
        if (clen < 2 * spl + 2) continue;
        const int E = emin + (int) std::floor (cx.rand() * (emax - emin + 1));
        const int Euse = std::max (1, (int) std::floor (E * budgetCut));
        const double atten = p.i ("linear") ? (1 - 0.01) / E : std::pow (0.01, 1.0 / E);
        const long long n = (long long) clen;
        for (size_t c = 0; c < src.size(); ++c) { clip[c].assign ((size_t) n, 0.f); for (long long k = 0; k < n; ++k) clip[c][(size_t) k] = src[c][(size_t) std::min ((long long) L - 1, (long long) a + k)]; fades (clip[c], spl, spl); }
        double level = 1, pos = a;
        for (int e = 0; e <= Euse; ++e)
        {
            if (e > 0 && shredN >= 2 && shredC >= 1)
                for (int sc = 0; sc < shredC; ++sc)
                {   // cut the previous echo into shredN parts and shuffle them
                    std::vector<long long> cp { 0 }; for (int k = 1; k < shredN; ++k) cp.push_back ((long long) (cx.rand() * (double) n)); cp.push_back (n);
                    std::sort (cp.begin(), cp.end());
                    std::vector<int> perm ((size_t) shredN); for (int k = 0; k < shredN; ++k) perm[(size_t) k] = k;
                    for (int k = shredN - 1; k > 0; --k) std::swap (perm[(size_t) k], perm[(size_t) (cx.rand() * (k + 1))]);
                    for (auto& b : clip)
                    {
                        Buf nb; nb.reserve ((size_t) n);
                        for (int q : perm) { size_t s0 = (size_t) cp[(size_t) q], s1 = (size_t) cp[(size_t) q + 1]; Buf piece (b.begin() + (long) s0, b.begin() + (long) s1); fades (piece, 16, 16); nb.insert (nb.end(), piece.begin(), piece.end()); }
                        nb.resize ((size_t) n); b.swap (nb);
                    }
                }
            double pan = -1;
            const double u = Euse > 0 ? std::pow ((double) e / Euse, 0.4) : 0;
            switch (mode)
            {
                case 1: pan = u; break;
                case 2: pan = 0.5 + 0.5 * u * panDir; break;
                case 3: pan = (double) (e % 8) / 7; break;
                case 4: pan = panDir > 0 ? (double) (e % 8) / 7 : 1 - (double) (e % 8) / 7; break;
                default: break;
            }
            const long long at = (long long) pos;
            for (size_t c = 0; c < nch; ++c)
            {
                const Buf& b = clip[std::min (c, clip.size() - 1)];
                double g = level;
                if (pan >= 0) g *= c == 0 ? std::cos (pan * PI / 2) : std::sin (pan * PI / 2);
                for (long long k = 0; k < n; ++k) { long long j = at + k; if (j >= 0 && j < (long long) outLen) out[c][(size_t) j] = (float) (out[c][(size_t) j] + b[(size_t) k] * g); }
            }
            double base = a + (double) (e + 1) * (clen - spl);
            pos = (e + 1 < Euse && p["rand"] > 0) ? base + (cx.rand() * 2 - 1) * p["rand"] * (clen - spl - 1) * 0.5 : base;
            level = p.i ("linear") ? std::max (0.0, level - atten) : level * atten;
        }
        if (mode == 2 || mode == 4) panDir = -panDir;
        cx.prog ((double) i / (double) cuts.size());
    }
    return out;
}

// ELEMENT REPEATER: the sound plays, but the element START-END is repeated COUNT times every DELAY (0 = its length) before the
// rest follows. OFFSET mode adds a gap after each repeat instead; BOUNCE accelerates (ACCEL, WARP) and fades (FADE) the repeats.
// RANDOM stretches delays, PITCH scatters the repeats
// after CDP repeater repeater 1-3 (dev/science/repeater.c: repeater) - the element data file becomes one element's sliders
Chans repeater (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, L = (double) ch[0].size(); const int mode = p.i ("mode"), N = std::max (1, p.i ("count"));
    double a = clampd (p["start"], 0, 1) * L, b = clampd (p["end"], 0, 1) * L; if (b < a) std::swap (a, b);
    if (b - a < 0.01 * sr) b = std::min (L, a + std::max (8.0, 0.01 * sr));
    if (b - a < 8) { a = 0; b = L; }
    const double seg = b - a, d0 = p["delay"] > 0 ? (double) ms2s (p["delay"], sr) : seg;
    cx.seed (p["seed"]);
    std::vector<Hit> h; Hit pre { 0 }; pre.len = b; h.push_back (pre);
    double t = a;
    for (int k = 1; k <= N; ++k)
    {
        double f = (double) k / N, rnd = 1 + cx.rand() * (std::max (1.0, p["rand"]) - 1);
        double step = mode == 1 ? seg + p["delay"] / 1000 * sr * rnd : mode == 2 ? d0 * (1 - (1 - 1 / std::max (1.0, p["accel"])) * std::pow (f, p["warp"])) * rnd : d0 * rnd;
        t += std::max (4.0, step);
        if (t >= (double) cx.maxLen) break;
        Hit e { t }; e.src0 = a; e.len = seg; e.ratio = semis ((cx.rand() * 2 - 1) * p["prand"]);
        if (mode == 2) { e.gain = std::pow (1 - f * 0.95, p["fade"]); e.len = seg * (1 - (1 - 1 / std::max (1.0, p["accel"])) * std::pow (f, p["warp"])); }
        h.push_back (e);
    }
    Hit rest { h.back().at + h.back().len / h.back().ratio }; rest.src0 = b; rest.len = L - b; if (rest.len > 8) h.push_back (rest);
    return renderHits (ch, h, 0, false, cx, 0.003);
}

// TESSELATE: up to 4 voices of the sound, entering staggered over SPREAD of a CYCLE, each repeating every cycle on the left and a little
// slower on the right, so left and right drift apart and resynchronise after REPEATS (+ voice number) cycles: Reich-style phasing
// after CDP tesselate tesselate (dev/science/tesselate.c: tesselate) - channel pairs folded to left / right
Chans tesselate (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, goal = std::min ((double) cx.maxLen, p["dur"] * sr);
    const double cyc = std::max (0.02 * sr, p.i ("sync") ? divSec (p.i ("sync") - 1, cx.bpm) * sr : p["cycle"] * sr);
    const int V = clamp (p.i ("voices"), 1, 4), R = std::max (1, p.i ("repeats")); const double from = std::max (0, p.i ("from")) * cyc;
    std::vector<Hit> h;
    for (int v = 0; v < V; ++v)
    {
        const double entry = (double) v / V * p["spread"] * cyc, slow = cyc * (double) (R + v + 1) / (double) (R + v);
        for (int side = 0; side < 2; ++side)
        {
            const double per = side == 0 ? cyc : slow;
            for (double k = std::ceil (std::max (0.0, from - entry) / per); h.size() < 40000; k += 1)
            {
                double t = entry + k * per - from; if (t >= goal) break;
                Hit e { t }; e.pan = side == 0 ? 0.0 : 1.0; h.push_back (e);
            }
        }
    }
    return renderHits (ch, h, 0, true, cx, 0.003);
}
} // namespace

void addCdpExtend (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CAT = "CDP \xc2\xb7 EXTEND \xc2\xb7 TEXTURE";
    add (v, CAT, "cdp_extend_zigzag", "ZIGZAG READ", "ZIGZ", "Reads the sound back and forth between START and END until it lasts DURATION, then plays on to the end. RANDOM turns between MIN and MAX ZIG apart; STEPPED walks POINTS evenly spaced (two forward, one back).",
         { Opt ("mode", "MODE", { "RANDOM", "STEPPED" }), Pct ("start", "START", 0.1), Pct ("end", "END", 0.7), P ("dur", "DURATION", 1, 60, 12, 0, "s"),
           P ("minzig", "MIN ZIG", 20, 2000, 80, PExp, "ms"), P ("maxzig", "MAX ZIG", 50, 5000, 600, PExp, "ms"), P ("points", "POINTS", 3, 32, 8, PInt),
           P ("splice", "SPLICE", 2, 100, 25, PExp, "ms"), Seed() }, zigzag, {}, false, "extend zigzag");
    add (v, CAT, "cdp_extend_loop", "LOOP ADVANCE", "LOOP", "Loops a LENGTH segment from START, each repeat STEP further into the file: until the file ends, to DURATION or COUNT times. SCATTER makes the advance irregular, FROM BEG plays the opening first.",
         { Opt ("mode", "MODE", { "TO END", "DURATION", "COUNT" }), Pct ("start", "START", 0.1), P ("len", "LENGTH", 10, 2000, 250, PExp, "ms"), P ("step", "STEP", 0, 1000, 60, 0, "ms"),
           P ("dur", "DURATION", 1, 60, 10, 0, "s"), P ("count", "COUNT", 1, 200, 16, PInt), P ("scat", "SCATTER", 0, 500, 0, 0, "ms"), P ("splice", "SPLICE", 2, 100, 15, PExp, "ms"),
           Opt ("frombeg", "FROM BEG", { "NO", "YES" }), Seed() }, loop, {}, false, "extend loop");
    add (v, CAT, "cdp_extend_scramble", "SCRAMBLE EXTEND", "SCRX", "RANDOM cuts chunks between MIN and MAX length from anywhere and splices them end to end; SHRED cuts the file into MAX-length chunks (SCATTER varies them) and plays new permutations until DURATION.",
         { Opt ("mode", "MODE", { "RANDOM", "SHRED" }), P ("minlen", "MIN LEN", 20, 2000, 80, PExp, "ms"), P ("maxlen", "MAX LEN", 20, 4000, 400, PExp, "ms"), P ("scatter", "SCATTER", 0, 1, 0.5),
           P ("dur", "DURATION", 1, 60, 10, 0, "s"), P ("splice", "SPLICE", 2, 100, 15, PExp, "ms"), Opt ("keep", "KEEP", { "NONE", "START", "END", "BOTH" }), Seed() }, scramble, {}, false, "extend scramble");
    add (v, CAT, "cdp_extend_iterate", "FLUID ITERATE", "ITRX", "Repeats the whole sound every DELAY (RANDOM varies it) to a DURATION or REPEATS times, each copy randomly PITCH shifted and AMP CUT, fading by FADE per repeat. Short sounds become swarms, long ones dense washes.",
         { Opt ("mode", "MODE", { "DURATION", "REPEATS" }), P ("dur", "DURATION", 1, 60, 8, 0, "s"), P ("reps", "REPEATS", 1, 200, 12, PInt), P ("delay", "DELAY", 5, 5000, 300, PExp, "ms"),
           Pct ("rand", "RANDOM", 0.3), P ("pshift", "PITCH \xc2\xb1", 0, 12, 1, 0, "st"), Pct ("ampcut", "AMP CUT", 0.3), Pct ("fade", "FADE", 0.05), Seed() }, iterate, {}, false, "extend iterate");
    add (v, CAT, "cdp_extend_freeze", "ITERATIVE FREEZE", "IFRZ", "Plays up to START, then holds the START-END segment by fluid iteration (DELAY as a share of the segment, RANDOM, PITCH and AMP CUT scatter) for DURATION or REPEATS, then continues. GAIN sets the frozen level.",
         { Opt ("mode", "MODE", { "DURATION", "REPEATS" }), Pct ("start", "START", 0.2), Pct ("end", "END", 0.3), P ("dur", "DURATION", 1, 60, 10, 0, "s"), P ("reps", "REPEATS", 1, 200, 20, PInt),
           P ("delay", "DELAY", 0.05, 1, 0.5, 0, "x"), Pct ("rand", "RANDOM", 0.3), P ("pshift", "PITCH \xc2\xb1", 0, 12, 0.5, 0, "st"), Pct ("ampcut", "AMP CUT", 0.3), P ("gain", "GAIN", 0.25, 4, 1, PExp, "x"), Seed() },
         freezeIt, {}, false, "extend freeze");
    add (v, CAT, "cdp_extend_drunk", "DRUNKEN WALK", "DRWK", "Splices CLOCK-long segments end to end, their read times staggering randomly (up to STEP) around LOCUS within AMBITUS. SOBER mode plays the source straight every few DRUNKS for up to SOBER seconds.",
         { Opt ("mode", "MODE", { "DRUNK", "SOBER MOMENTS" }), P ("dur", "DURATION", 1, 60, 10, 0, "s"), Pct ("locus", "LOCUS", 0.3), P ("ambitus", "AMBITUS", 0, 10, 0.5, 0, {}, Fmt::time),
           P ("step", "STEP", 0.002, 5, 0.15, PExp, {}, Fmt::time), P ("clock", "CLOCK", 0.03, 2, 0.12, PExp, {}, Fmt::time), Pct ("clokrand", "CLOCK RAND", 0.2), P ("overlap", "OVERLAP", 0, 0.9, 0, PPct, {}, Fmt::pct),
           P ("drunks", "DRUNKS", 1, 64, 12, PInt), P ("sober", "SOBER", 0.1, 30, 1.5, PExp, "s"), Seed() }, drunkWalk, {}, false, "extend drunk");
    add (v, CAT, "cdp_extend_sequence", "STEP SEQUENCE", "SEQX", "Replays the sound on a tempo grid (RATE) with a transposition per step (STEP 1-8, tape style), over STEPS steps for CYCLES cycles. GATE is the note length in steps; the first step is accented.",
         { P ("s1", "STEP 1", -24, 24, 0, PInt, "st"), P ("s2", "STEP 2", -24, 24, 7, PInt, "st"), P ("s3", "STEP 3", -24, 24, 3, PInt, "st"), P ("s4", "STEP 4", -24, 24, 12, PInt, "st"),
           P ("s5", "STEP 5", -24, 24, -5, PInt, "st"), P ("s6", "STEP 6", -24, 24, 5, PInt, "st"), P ("s7", "STEP 7", -24, 24, 10, PInt, "st"), P ("s8", "STEP 8", -24, 24, -12, PInt, "st"),
           P ("steps", "STEPS", 1, 8, 8, PInt), Opt ("div", "RATE", DIVS, 3), P ("cycles", "CYCLES", 1, 32, 4, PInt), P ("gate", "GATE", 0.25, 32, 2, PExp, "steps") },
         sequence, {}, false, "extend sequence");
    add (v, CAT, "cdp_extend_baktobak", "BACK TO BACK", "BTOB", "Plays the sound backwards from its end to JOIN, then forwards from JOIN to the end again: a palindrome that swells into the cut. SPLICE smooths the join.",
         { Pct ("join", "JOIN", 0.1), P ("splice", "SPLICE", 1, 200, 20, PExp, "ms") }, baktobak, {}, false, "extend baktobak");
    add (v, CAT, "cdp_extend_doublets", "DOUBLETS", "DBLT", "Cuts the sound into SEGMENT-long pieces (sweeping to END SEG over the file) and plays each REPEATS times. SYNC skips ahead so the result stays in step with the original.",
         { P ("seg", "SEGMENT", 10, 2000, 120, PExp, "ms"), P ("segend", "END SEG", 10, 2000, 120, PExp, "ms"), P ("repets", "REPEATS", 1, 16, 2, PInt), Opt ("sync", "SYNC", { "NO", "YES" }) },
         doublets, {}, false, "extend doublets");
    add (v, CAT, "cdp_extend_repetitions", "REPETITIONS", "REPS", "Restarts the whole sound COUNT times: the first GAP (free or tempo SYNC), each next gap ACCEL times the last (< 1 accelerates, > 1 slows), level moving from LEVEL to END LEVEL.",
         { P ("count", "COUNT", 1, 64, 8, PInt), Opt ("sync", "SYNC", SYNC_FREE, 3), P ("gap", "GAP (FREE)", 10, 4000, 400, PExp, "ms"), P ("accel", "ACCEL", 0.5, 2, 0.85, PExp, "x"),
           Pct ("level", "LEVEL", 1), Pct ("endlevel", "END LEVEL", 0.3) }, repetitions, {}, false, "extend repetitions");
    add (v, CAT, "cdp_hfperm_delperm2", "FRACTAL PERMUTE", "DPRM", "Turns the NOTE into ELEMENTS transposed, shorter copies (TRANSP 1-4, time SHARE 1-4), then each of those again, for CYCLES levels: a self-similar melody. CASCADE delays each level by one note, STACK layers them.",
         { Opt ("mode", "MODE", { "CASCADE", "STACK" }), P ("note", "NOTE", 0.1, 8, 1.5, PExp, "s"), P ("elems", "ELEMENTS", 2, 4, 3, PInt), P ("cycles", "CYCLES", 1, 4, 3, PInt),
           P ("t1", "TRANSP 1", -12, 12, 0, 0, "st"), P ("t2", "TRANSP 2", -12, 12, 4, 0, "st"), P ("t3", "TRANSP 3", -12, 12, 7, 0, "st"), P ("t4", "TRANSP 4", -12, 12, -5, 0, "st"),
           Pct ("w1", "SHARE 1", 0.4), Pct ("w2", "SHARE 2", 0.3), Pct ("w3", "SHARE 3", 0.3), Pct ("w4", "SHARE 4", 0.2) }, delperm, {}, false, "hfperm delperm2");
    // ---- texture ----
    auto harm = [] (std::vector<ParamDef> extra)
    {
        std::vector<ParamDef> ps { Opt ("mode", "HARMONY", HARM_MODES), Opt ("chord", "CHORD", CHORD_NAMES, 7), P ("root", "ROOT", 24, 96, 55, PInt, {}, Fmt::note),
                                   P ("span", "SPAN", 0, 48, 19, PInt, "st"), P ("len", "LENGTH", 1, 60, 10, 0, "s") };
        ps.insert (ps.end(), extra.begin(), extra.end()); ps.push_back (Seed()); return ps;
    };
    const std::string HT = " HARMONY: FIELD (chord tones in every octave), SET (one voicing), MOVING (the root follows I-IV-V-vi, a bar each) or FREE.";
    add (v, CAT, "cdp_texture_simple", "TEXTURE SIMPLE", "TXSM", "Scatters transposed copies of the sound every PACKING (SCATTER randomises), pitched from ROOT up SPAN on the CHORD, DUR MIN-MAX long, LEVEL VAR and stereo SPREAD." + HT,
         harm ({ P ("pack", "PACKING", 0.01, 2, 0.12, PExp, {}, Fmt::time), P ("scatter", "SCATTER", 0, 10, 0.7), P ("dmin", "DUR MIN", 0.02, 4, 0.15, PExp, {}, Fmt::time),
                 P ("dmax", "DUR MAX", 0.02, 8, 0.8, PExp, {}, Fmt::time), Pct ("gainvar", "LEVEL VAR", 0.4), Pct ("spread", "SPREAD", 0.9) }), texSimpleFn, {}, false, "texture simple / texmchan");
    add (v, CAT, "cdp_texture_timed", "TEXTURE TIMED", "TXTM", "Notes on a repeating timing PATTERN at RATE on the tempo grid, random pitch from ROOT up SPAN on the CHORD, DUR MIN-MAX, LEVEL VAR and SPREAD." + HT,
         harm ({ Opt ("pattern", "PATTERN", RHYTHM_NAMES, 2), Opt ("div", "RATE", DIVS, 4), P ("dmin", "DUR MIN", 0.02, 4, 0.1, PExp, {}, Fmt::time),
                 P ("dmax", "DUR MAX", 0.02, 8, 0.5, PExp, {}, Fmt::time), Pct ("gainvar", "LEVEL VAR", 0.4), Pct ("spread", "SPREAD", 0.8) }), texTimedFn, {}, false, "texture timed");
    const O CONTOURS { "MIXED", "CRESC", "FLAT", "DECRESC", "FLAT/CRESC", "CRESC/DECR", "FLAT/DECR" }, SPACES { "STILL", "SCATTER", "INWARD", "OUTWARD", "FOLLOWING", "CONTRARY" };
    add (v, CAT, "cdp_texture_grouped", "TEXTURE GROUPS", "TXGR", "Every PACKING starts a flourish of up to SIZE notes SPEED apart within RANGE semitones, with a level CONTOUR and a SPACE motion (still, scattered, inward, outward, following or against the texture)." + HT,
         harm ({ P ("pack", "PACKING", 0.05, 4, 0.5, PExp, {}, Fmt::time), P ("gsize", "SIZE", 1, 16, 6, PInt), P ("gspeed", "SPEED", 10, 1000, 70, PExp, "ms"),
                 P ("grange", "RANGE", 0, 36, 12, 0, "st"), Opt ("contour", "CONTOUR", CONTOURS), Opt ("space", "SPACE", SPACES, 1) }), texGroupedFn, {}, false, "texture grouped");
    add (v, CAT, "cdp_texture_tgrouped", "TEXTURE TIMED GROUPS", "TXTG", "Flourishes of up to SIZE notes start on a timing PATTERN at RATE and fill the step, within RANGE semitones, with a level CONTOUR and SPACE motion." + HT,
         harm ({ Opt ("pattern", "PATTERN", RHYTHM_NAMES, 1), Opt ("div", "RATE", DIVS, 3), P ("gsize", "SIZE", 1, 16, 4, PInt), P ("grange", "RANGE", 0, 36, 12, 0, "st"),
                 Opt ("contour", "CONTOUR", CONTOURS), Opt ("space", "SPACE", SPACES, 4) }), texTGroupedFn, {}, false, "texture tgrouped");
    add (v, CAT, "cdp_texture_decorated", "TEXTURE DECORATED", "TXDC", "A line walks the CHORD at RATE; each note is decorated by up to SIZE notes SPEED apart within RANGE, CENTRED on, ABOVE or BELOW it, placed BEFORE, AFTER or EITHER side." + HT,
         harm ({ Opt ("div", "RATE", DIVS, 2), P ("gsize", "SIZE", 1, 12, 4, PInt), P ("gspeed", "SPEED", 10, 500, 60, PExp, "ms"), P ("grange", "RANGE", 0, 24, 7, 0, "st"),
                 Opt ("centre", "CENTRING", { "CENTRED", "ABOVE", "BELOW", "CEN/ABOVE", "CEN/BELOW", "ABOVE/BELOW", "ALL" }), Opt ("place", "PLACE", { "EITHER", "BEFORE", "AFTER" }) }),
         texDecor, {}, false, "texture decorated / predecor / postdecor");
    add (v, CAT, "cdp_texture_ornate", "TEXTURE ORNATE", "TXOR", "A line walks the CHORD at RATE and every note gets an ornament SHAPE (trill, mordent, turn, slide, arpeggio, run, grace), SPEED per ornament note times up to VARIATION, AFTER, BEFORE (ending on the note) or EITHER." + HT,
         harm ({ Opt ("div", "RATE", DIVS, 2), Opt ("shape", "ORNAMENT", ORN_NAMES, 3), P ("speed", "SPEED", 10, 500, 70, PExp, "ms"), P ("mvar", "VARIATION", 1, 4, 1.5, 0, "x"),
                 Opt ("place", "PLACE", { "EITHER", "BEFORE", "AFTER" }, 2) }), texOrnate, {}, false, "texture ornate / preornate / postornate");
    add (v, CAT, "cdp_texture_motifs", "TEXTURE MOTIFS", "TXMO", "Every PACKING (SCATTER randomises) plays a MOTIF (sigh, call, fanfare, fall...) at SPEED per step from a random pitch; FORCE puts the FIRST or ALL notes on the CHORD. SPREAD pans the motifs." + HT,
         harm ({ P ("pack", "PACKING", 0.05, 4, 0.45, PExp, {}, Fmt::time), P ("scatter", "SCATTER", 0, 10, 0.8), Opt ("shape", "MOTIF", MOTIF_NAMES, 1), P ("speed", "SPEED", 20, 1000, 110, PExp, "ms"),
                 Opt ("force", "FORCE", { "FIRST", "ALL" }), Pct ("spread", "SPREAD", 0.9) }), texMotifsFn, {}, false, "texture motifs / motifsin");
    add (v, CAT, "cdp_texture_tmotifs", "TEXTURE TIMED MOTIFS", "TXTO", "Motifs start on a timing PATTERN at RATE: a MOTIF at SPEED per step from a random pitch; FORCE puts the FIRST or ALL notes on the CHORD. SPREAD pans the motifs." + HT,
         harm ({ Opt ("pattern", "PATTERN", RHYTHM_NAMES, 0), Opt ("div", "RATE", DIVS, 2), Opt ("shape", "MOTIF", MOTIF_NAMES, 2), P ("speed", "SPEED", 20, 1000, 90, PExp, "ms"),
                 Opt ("force", "FORCE", { "FIRST", "ALL" }, 1), Pct ("spread", "SPREAD", 0.9) }), texTMotifsFn, {}, false, "texture tmotifs / tmotifsin");
    // ---- iterate, hover, fractal, repeat streams ----
    add (v, CAT, "cdp_freeze_freeze", "FREEZE ITERATE", "FRZI", "Same as ITERATIVE FREEZE (CDP's standalone freeze): plays to START, holds START-END by fluid iteration (DELAY share, RANDOM, PITCH, AMP CUT) for DURATION or REPEATS, then continues; GAIN adjusts the frozen level.",
         { Opt ("mode", "MODE", { "DURATION", "REPEATS" }), Pct ("start", "START", 0.4), Pct ("end", "END", 0.45), P ("dur", "DURATION", 1, 60, 8, 0, "s"), P ("reps", "REPEATS", 1, 200, 20, PInt),
           P ("delay", "DELAY", 0.05, 1, 0.35, 0, "x"), Pct ("rand", "RANDOM", 0.5), P ("pshift", "PITCH \xc2\xb1", 0, 12, 0.3, 0, "st"), Pct ("ampcut", "AMP CUT", 0.4), P ("gain", "GAIN", 0.25, 4, 1, PExp, "x"), Seed() },
         freezeIt, {}, false, "freeze freeze");
    add (v, CAT, "cdp_mchiter_iter", "ITERATE SPREAD", "MITR", "FLUID ITERATE with every repeat thrown to a random place across the stereo field (SPREAD): DELAY, RANDOM, PITCH scatter, AMP CUT and FADE as before, to a DURATION or REPEATS.",
         { Opt ("mode", "MODE", { "DURATION", "REPEATS" }), P ("dur", "DURATION", 1, 60, 8, 0, "s"), P ("reps", "REPEATS", 1, 200, 12, PInt), P ("delay", "DELAY", 5, 5000, 200, PExp, "ms"),
           Pct ("rand", "RANDOM", 0.4), P ("pshift", "PITCH \xc2\xb1", 0, 12, 1.5, 0, "st"), Pct ("ampcut", "AMP CUT", 0.3), Pct ("fade", "FADE", 0.05), Pct ("spread", "SPREAD", 1), Seed() }, mchiter, {}, false, "mchiter iter");
    add (v, CAT, "cdp_mchzig_zag", "ZIGZAG SPREAD", "MZIG", "ZIGZAG READ with every zig thrown to a random place across the stereo field (SPREAD). RANDOM turns between MIN and MAX ZIG apart, STEPPED walks evenly spaced POINTS.",
         { Opt ("mode", "MODE", { "RANDOM", "STEPPED" }), Pct ("start", "START", 0.1), Pct ("end", "END", 0.7), P ("dur", "DURATION", 1, 60, 12, 0, "s"),
           P ("minzig", "MIN ZIG", 20, 2000, 80, PExp, "ms"), P ("maxzig", "MAX ZIG", 50, 5000, 500, PExp, "ms"), P ("points", "POINTS", 3, 32, 8, PInt),
           P ("splice", "SPLICE", 2, 100, 20, PExp, "ms"), Pct ("spread", "SPREAD", 1), Seed() }, mchzig, {}, false, "mchzig zag");
    add (v, CAT, "cdp_hover_hover", "HOVER", "HOVR", "Reads back and forth around a spot at FREQ (half a cycle forward, half back) while the spot glides from LOCATION to END LOC. Slow rates rock and scrub, audio rates buzz at the pitch FREQ. FREQ RAND / LOC RAND add drift.",
         { P ("frq", "FREQ", 0.5, 2000, 8, PExp, {}, Fmt::hz), Pct ("loc", "LOCATION", 0.2), Pct ("locend", "END LOC", 0.6), Pct ("frqrand", "FREQ RAND", 0.1), Pct ("locrand", "LOC RAND", 0.1),
           P ("splice", "SPLICE", 0.5, 50, 5, PExp, "ms"), P ("dur", "DURATION", 1, 60, 8, 0, "s"), Seed() }, hover, {}, false, "hover hover");
    add (v, CAT, "cdp_hover2_hover2", "HOVER WAVES", "HOV2", "From a zero crossing near the spot, a quarter cycle is read forward, back, then inverted: symmetrical waveforms at FREQ, scanning from LOCATION to END LOC. NORMALISE evens the level of the cycles.",
         { P ("frq", "FREQ", 1, 2000, 110, PExp, {}, Fmt::hz), Pct ("loc", "LOCATION", 0.1), Pct ("locend", "END LOC", 0.9), Pct ("frqrand", "FREQ RAND", 0.02), Pct ("locrand", "LOC RAND", 0.2),
           P ("dur", "DURATION", 1, 60, 6, 0, "s"), Opt ("norm", "NORMALISE", { "NO", "YES" }, 1), Seed() }, hover2, {}, false, "hover2 hover2");
    add (v, CAT, "cdp_frfractal_fractal", "FRACTALISE", "FRAC", "Cuts the sound into segments and, layer by layer, follows each group with its mirror image (segment order reversed) at ever smaller scales: the sound folded into itself. Each LAYER doubles the length.",
         { P ("layers", "LAYERS", 1, 10, 3, PInt), P ("splice", "SPLICE", 1, 50, 5, PExp, "ms") }, frfractal, {}, false, "frfractal (fractal fractal)");
    add (v, CAT, "cdp_ceracu_ceracu", "POLY CYCLES", "CERA", "Repeats the sound in up to 4 streams across the stereo field that resync every cycle: stream N plays COUNT N times per cycle (e.g. 10:12:15), the fastest every CYCLE (free or SYNC). ECHO adds a delayed copy, MIRROR flips its side.",
         { P ("c1", "COUNT 1", 0, 32, 3, PInt), P ("c2", "COUNT 2", 0, 32, 4, PInt), P ("c3", "COUNT 3", 0, 32, 5, PInt), P ("c4", "COUNT 4", 0, 32, 0, PInt), Opt ("sync", "SYNC", SYNC_FREE, 4),
           P ("mincyc", "CYCLE FREE", 0, 4000, 250, 0, "ms"), P ("dur", "LENGTH", 1, 60, 8, 0, "s"), P ("echo", "ECHO", 0, 2, 0, 0, {}, Fmt::time), Opt ("epan", "ECHO PAN", { "SAME", "MIRROR" }, 1) },
         ceracu, {}, false, "ceracu ceracu");
    add (v, CAT, "cdp_sfecho_echo", "DISCRETE ECHO", "SFEC", "Repeats the whole sound after DELAY (never overlapping: at least its own length), each repeat ATTEN as loud, RANDOM varies the gaps, until LENGTH or the echoes fall below CUTOFF.",
         { P ("delay", "DELAY", 0.05, 20, 1, PExp, {}, Fmt::time), P ("atten", "ATTEN", 0, 1, 0.6, PPct, {}, Fmt::pct), P ("dur", "LENGTH", 1, 120, 20, 0, "s"), Pct ("rand", "RANDOM", 0.2),
           P ("cutoff", "CUTOFF", -96, -6, -40, 0, {}, Fmt::db), Seed() }, sfecho, {}, false, "sfecho echo");
    add (v, CAT, "cdp_iterfof_iterfof", "FOF LINE", "IFOF", "Takes a GRAIN at POSITION and fires it as a pulse train pitched by a melody: NOTE 1-4, one per RATE step, GLIDE or STEPS, with VIBRATO and random AMP CUT. Clicks and fragments become sung lines.",
         { Opt ("mode", "MODE", { "GLIDE", "STEPS" }), Pct ("pos", "POSITION", 0.05), P ("size", "GRAIN", 2, 200, 25, PExp, "ms"), P ("n1", "NOTE 1", 24, 96, 48, PInt, {}, Fmt::note),
           P ("n2", "NOTE 2", 24, 96, 55, PInt, {}, Fmt::note), P ("n3", "NOTE 3", 24, 96, 51, PInt, {}, Fmt::note), P ("n4", "NOTE 4", 24, 96, 58, PInt, {}, Fmt::note), Opt ("div", "RATE", DIVS, 2),
           P ("vib", "VIBRATO", 0, 2, 0.25, 0, "st"), Pct ("ampcut", "AMP CUT", 0.2), P ("dur", "DURATION", 1, 60, 8, 0, "s"), Seed() }, iterfof, {}, false, "iterfof iterfof");
    add (v, CAT, "cdp_iterline_iterline", "ITERATE LINE", "ITLN", "Iterates the whole sound every DELAY (RANDOM varies it) while its transposition follows a line through TRANSP 1-4 over the length, GLIDE or STEPS, with random PITCH scatter and AMP CUT.",
         { Opt ("mode", "MODE", { "GLIDE", "STEPS" }), P ("t1", "TRANSP 1", -24, 24, 0, 0, "st"), P ("t2", "TRANSP 2", -24, 24, 7, 0, "st"), P ("t3", "TRANSP 3", -24, 24, -5, 0, "st"),
           P ("t4", "TRANSP 4", -24, 24, 12, 0, "st"), P ("delay", "DELAY", 5, 5000, 150, PExp, "ms"), Pct ("rand", "RANDOM", 0.2), P ("pshift", "PITCH \xc2\xb1", 0, 12, 0.3, 0, "st"),
           Pct ("ampcut", "AMP CUT", 0.3), P ("dur", "DURATION", 1, 60, 8, 0, "s"), Seed() }, iterline, {}, false, "iterline iterline / iterlinef");
    add (v, CAT, "cdp_madrid_madrid", "SPATIAL SYNCOPATE", "MADR", "Repeats the sound every STEP (free or SYNC, RANDOM jitter) in STREAMS positions across the stereo field and randomly DELETEs events per stream, so accents, syncopation and movement emerge. EMPTY allows silent steps.",
         { P ("dur", "LENGTH", 1, 60, 8, 0, "s"), P ("streams", "STREAMS", 2, 8, 4, PInt), P ("delfact", "DELETE", 0, 2, 0.6), Opt ("sync", "SYNC", SYNC_FREE, 5), P ("step", "STEP FREE", 10, 2000, 150, PExp, "ms"),
           Pct ("rand", "RANDOM", 0), Opt ("empty", "EMPTY", { "NO", "YES" }), Seed() }, madrid, {}, false, "madrid madrid");
    add (v, CAT, "cdp_shifter_shifter", "FOCUS SHIFTER", "SHFR", "Up to 4 simultaneous repetition streams of BEATS 1-4 per CYCLE (free or SYNC bars) across the stereo field; the loud focus (BOOST) stays LINGER cycles on one, then moves over TRANSIT cycles to the next: in ORDER, zigzag or random.",
         { P ("c1", "BEATS 1", 0, 32, 3, PInt), P ("c2", "BEATS 2", 0, 32, 4, PInt), P ("c3", "BEATS 3", 0, 32, 5, PInt), P ("c4", "BEATS 4", 0, 32, 7, PInt), Opt ("sync", "SYNC", { "FREE", "1 BAR", "2 BARS", "4 BARS" }, 1),
           P ("cycdur", "CYCLE FREE", 0.2, 20, 2, PExp, {}, Fmt::time), P ("dur", "LENGTH", 1, 60, 16, 0, "s"), P ("linger", "LINGER", 0, 8, 1, 0, "cyc"), P ("transit", "TRANSIT", 0, 8, 1, 0, "cyc"),
           P ("boost", "BOOST", 0, 8, 3, 0, "x"), Opt ("order", "ORDER", { "IN TURN", "ZIGZAG", "RANDOM" }), Seed() }, shifter, {}, false, "shifter shifter");
    add (v, CAT, "cdp_shrink_shrink", "SHRINK", "SHRK", "Repeats the sound, each copy shorter by SHRINK (cut from the END, around the MIDDLE, from the START or around TIME) and each GAP times CONTRACT: an accelerating cascade. SMALLEST / MIN GAP stop it, RANDOM jitters, INVERT reverses copies.",
         { Opt ("mode", "MODE", { "FROM END", "MIDDLE", "FROM START", "AT TIME" }), Pct ("time", "TIME", 0.3), P ("shrinkage", "SHRINK", 0.3, 1, 0.8, 0, "x"), P ("gap", "GAP", 0.02, 20, 0.5, PExp, {}, Fmt::time),
           P ("contract", "CONTRACT", 0.3, 1, 0.85, 0, "x"), P ("dur", "LENGTH", 1, 60, 8, 0, "s"), P ("splice", "SPLICE", 1, 50, 5, PExp, "ms"), P ("small", "SMALLEST", 5, 2000, 40, PExp, "ms"),
           P ("mingap", "MIN GAP", 5, 2000, 60, PExp, "ms"), Pct ("rnd", "RANDOM", 0.3), Opt ("inverse", "INVERT", { "NO", "YES" }), Seed() }, shrink, {}, false, "shrink shrink");
    add (v, CAT, "cdp_bounce_bounce", "BOUNCE", "BNCE", "Bounces the sound COUNT times: first after START GAP, each gap SHORTEN times the last, the level falling to END LEVEL (WARP shapes the fall). MIN DUR shrinks the elements with the gaps, CUT stops overlaps, TRIM cuts from the start instead of the end.",
         { P ("count", "COUNT", 1, 100, 12, PInt), P ("startgap", "START GAP", 0.04, 10, 0.4, PExp, {}, Fmt::time), P ("shorten", "SHORTEN", 0.1, 1, 0.82, 0, "x"), Pct ("endlevel", "END LEVEL", 0.05),
           P ("ewarp", "WARP", 0.1, 10, 1, PExp, "x"), P ("mindur", "MIN DUR", 0, 1, 0.02, 0, {}, Fmt::time), Opt ("cut", "CUT", { "NO", "YES" }, 1), Opt ("trimstart", "TRIM", { "END", "START" }) },
         bounce, {}, false, "bounce bounce");
    add (v, CAT, "cdp_cascade_cascade", "CASCADE", "CASC", "Echoes successive CLIP-long segments (random up to CLIP MAX, or cut at ONSETS) ECHOES times (up to ECHO MAX), the echo sets piling up; RANDOM shifts echoes, LOG or LINEAR decay, panned per MODE. SHRED re-cuts each echo into SHRED N shuffled parts SHRED CNT times.",
         { Opt ("mode", "MODE", { "SAME", "LEFT>RIGHT", "CENTRE ALT", "STEP RIGHT", "STEP ALT" }, 1), Opt ("cuts", "CUTS", { "CLIPS", "ONSETS" }), P ("clip", "CLIP", 5, 4000, 200, PExp, "ms"),
           P ("clipmax", "CLIP MAX", 0, 4000, 0, 0, "ms"), P ("echos", "ECHOES", 1, 64, 6, PInt), P ("echosmax", "ECHO MAX", 0, 64, 0, PInt), Pct ("rand", "RANDOM", 0),
           P ("shredno", "SHRED N", 0, 16, 0, PInt), P ("shredcnt", "SHRED CNT", 0, 16, 1, PInt), Opt ("linear", "DECAY", { "LOG", "LINEAR" }), Seed() }, cascade, {}, false, "cascade cascade 1-10");
    add (v, CAT, "cdp_repeater_repeater", "ELEMENT REPEATER", "RPTR", "Plays the sound with the START-END element repeated COUNT times every DELAY (0 = its length) before the rest follows. OFFSET adds a gap after each repeat, BOUNCE accelerates (ACCEL, WARP) and fades (FADE). RANDOM stretches delays, PITCH scatters.",
         { Opt ("mode", "MODE", { "DELAY", "OFFSET", "BOUNCE" }), Pct ("start", "START", 0.1), Pct ("end", "END", 0.18), P ("count", "COUNT", 1, 64, 6, PInt), P ("delay", "DELAY", 0, 2000, 0, 0, "ms"),
           P ("rand", "RANDOM", 1, 8, 1, 0, "x"), P ("prand", "PITCH \xc2\xb1", 0, 12, 0, 0, "st"), P ("accel", "ACCEL", 1, 8, 3, PExp, "x"), P ("warp", "WARP", 0.2, 5, 1, PExp, "x"), P ("fade", "FADE", 0.2, 5, 1, PExp, "x"), Seed() },
         repeater, {}, false, "repeater repeater 1-3");
    add (v, CAT, "cdp_tesselate_tesselate", "TESSELATE", "TESS", "Up to 4 VOICES of the sound, entering over SPREAD of a CYCLE (free or SYNC), repeat every cycle on the left and slightly slower on the right, drifting apart and resyncing after REPEATS (+ voice) cycles: Reich-like phasing. FROM starts later in the pattern.",
         { P ("voices", "VOICES", 1, 4, 3, PInt), Opt ("sync", "SYNC", SYNC_FREE, 3), P ("cycle", "CYCLE FREE", 0.05, 8, 0.5, PExp, {}, Fmt::time), P ("repeats", "REPEATS", 1, 64, 8, PInt),
           Pct ("spread", "SPREAD", 0.75), P ("dur", "LENGTH", 1, 120, 16, 0, "s"), P ("from", "FROM", 0, 64, 0, PInt, "cyc") }, tesselate, {}, false, "tesselate tesselate");
}
} // namespace lab
