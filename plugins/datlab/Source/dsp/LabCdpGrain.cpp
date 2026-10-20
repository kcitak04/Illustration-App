// CDP collection, Batch 6 - Grain / brassage / onset-event rhythm.
// CDP's "grain" programs work on grains separated by quiet gaps: here a gate detector (GATE level relative to
// the peak, MIN GAP = shortest gap that separates two grains) finds them. Envelope-trough programs (grev,
// grainex, envspeak...) cut at zero crossings near the troughs of a windowed peak envelope, as CDP does.
#include "LabCdp.h"
#include "LabDspUtil.h"
#include <array>

namespace lab
{
namespace
{
using namespace lab::dsp;

// ---------------------------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------------------------

// growable output, capped at maxLen
struct Out
{
    Chans c; size_t cap, used = 0;
    Out (size_t nch, size_t cap_, size_t reserve = 0) : c (std::max<size_t> (1, nch)), cap (std::max<size_t> (1, cap_))
    { for (auto& b : c) b.resize (std::min (cap, reserve)); }
    void grow (size_t i)
    {
        if (i < c[0].size()) return;
        size_t ns = std::min (cap, std::max (i + 1, c[0].size() + c[0].size() / 2 + 8192));
        for (auto& b : c) b.resize (ns);
    }
    void add (size_t ch, long long i, double v)
    {
        if (i < 0 || (size_t) i >= cap) return;
        grow ((size_t) i);
        c[ch][(size_t) i] += (float) v; used = std::max (used, (size_t) i + 1);
    }
    void touch (long long i) { if (i > 0) used = std::max (used, std::min (cap, (size_t) i)); }   // silence up to i counts as output
    Chans take() { for (auto& b : c) b.resize (std::max<size_t> (1, used)); return std::move (c); }
};

// plays x from source position a (reading srcLen samples at most, backwards when rev) at speed ratio,
// for dur output samples starting at output position at, with linear fades of `fade` samples.
// pan >= 0 places a mono mix in a stereo output (Out must have 2 channels).
void putSeg (Out& o, const Chans& x, double a, double srcLen, long long at, long long dur, double ratio, double fade, double gain,
             double pan = -1, bool rev = false)
{
    ratio = clampd (ratio, 1e-3, 1e3);
    if (srcLen <= 0 || dur <= 0 || at >= (long long) o.cap) return;
    dur = std::min (dur, (long long) std::floor (srcLen / ratio));
    if (dur <= 0) return;
    const double f = std::max (1.0, std::min (fade, (double) dur / 2));
    const size_t oc = o.c.size();
    double gl = gain, gr = gain;
    if (pan >= 0) { auto pg = panG (clampd (pan, 0, 1)); gl *= pg.first; gr *= pg.second; }
    for (long long i = 0; i < dur; ++i)
    {
        const long long op = at + i; if (op < 0) continue; if (op >= (long long) o.cap) break;
        double e = std::min ({ 1.0, (double) (i + 1) / f, (double) (dur - i) / f });
        double pos = rev ? a + srcLen - 1 - (double) i * ratio : a + (double) i * ratio;
        if (pan >= 0)
        {
            double v = 0; for (auto& c : x) v += lerpAt (c, pos); v /= (double) x.size();
            o.add (0, op, v * e * gl); if (oc > 1) o.add (1, op, v * e * gr);
        }
        else for (size_t c = 0; c < oc; ++c) o.add (c, op, lerpAt (chOr (x, c), pos) * e * gain);
    }
}

// silence-separated events (CDP grain gate / retime minsil): s = onset (with a 2 ms pre-roll), e = end of sound
struct Ev { long long s, e; };
std::vector<Ev> gateEvents (const Chans& ch, double gateDb, double minSil, double sr, bool attacks = true)
{
    const long long n = (long long) ch[0].size(); std::vector<Ev> ev;
    if (n <= 0) return ev;
    const double pk = peakAll (ch);
    if (pk <= 1e-9) { ev.push_back ({ 0, n }); return ev; }
    const double thr = pk * std::pow (10.0, clampd (gateDb, -90, 0) / 20) * 0.999;
    const long long mh = std::max (1LL, jr (minSil * sr)), pre = jr (0.002 * sr);
    long long hole = 0, lastLoud = -1; bool in = false;
    for (long long i = 0; i < n; ++i)
    {
        bool loud = false; for (auto& c : ch) if (std::abs (c[(size_t) i]) > thr) { loud = true; break; }
        if (loud)
        {
            if (! in || hole >= mh)
            {
                if (in) ev.back().e = lastLoud + 1;
                long long st = std::max (i - pre, ev.empty() ? 0LL : ev.back().e);
                ev.push_back ({ st, n }); in = true;
            }
            lastLoud = i; hole = 0;
        }
        else ++hole;
    }
    if (in) ev.back().e = std::min (n, lastLoud + 1);
    {   // a sound fading in (or out) flickers around the gate: blips shorter than a splice are part of the next sound, not grains
        const long long minLen = std::max (mh, jr (0.015 * sr));
        std::vector<Ev> m;
        for (size_t k = 0; k < ev.size(); ++k)
        {
            if (! m.empty() && m.back().e - m.back().s < minLen) { m.back().e = ev[k].e; continue; }
            m.push_back (ev[k]);
        }
        if (m.size() > 1 && m.back().e - m.back().s < minLen) { m[m.size() - 2].e = m.back().e; m.pop_back(); }
        ev.swap (m);
    }
    // dense or noisy material rarely falls below the gate: a strong attack (5 ms envelope jumping above twice its
    // 50 ms average, and above half the gate) also starts a new event, so the detector works on loops too
    if (attacks)
    {
        const long long H = std::max (16LL, jr (0.005 * sr)); std::vector<double> e;
        for (long long a = 0; a < n; a += H)
        {
            double s = 0; for (long long i = a; i < std::min (n, a + H); ++i) for (auto& c : ch) s += (double) c[(size_t) i] * c[(size_t) i];
            e.push_back (std::sqrt (s / (double) (H * (long long) ch.size())));
        }
        std::vector<long long> at;
        for (size_t k = 10; k < e.size(); ++k)
        {
            double avg = 0; for (size_t q = k - 10; q < k; ++q) avg += e[q]; avg /= 10;
            if (e[k] > thr * 0.5 && e[k] > 2 * avg + 1e-9 && (e[k - 1] <= 2 * avg || k == 10)) at.push_back (std::max (0LL, (long long) k * H - pre));
        }
        const long long sep = std::max (mh, 4 * H);
        for (long long t : at)
        {
            size_t j = 0; while (j + 1 < ev.size() && ev[j + 1].s <= t) ++j;
            if (ev.empty() || t < ev[j].s + sep || t >= ev[j].e - sep || (j + 1 < ev.size() && t > ev[j + 1].s - sep)) continue;
            Ev nx { t, ev[j].e }; ev[j].e = t; ev.insert (ev.begin() + (long) j + 1, nx);
        }
    }
    if (ev.empty()) ev.push_back ({ 0, n });
    return ev;
}
// grains = onset to next onset (CDP grain programs)
struct Grains
{
    std::vector<long long> on; long long len = 0;
    size_t size() const { return on.size(); }
    long long gap (size_t i) const { return (i + 1 < on.size() ? on[i + 1] : len) - on[i]; }
};
Grains findGrains (const Chans& ch, const Params& p, double sr)
{
    Grains g; g.len = (long long) ch[0].size();
    for (auto& e : gateEvents (ch, p["gate"], p["minsil"] / 1000, sr)) g.on.push_back (e.s);
    return g;
}

// windowed peak envelope (max |x| per window), CDP getmaxsamp
DVec peakEnv (const Buf& x, long long W)
{
    W = std::max (1LL, W); const long long n = (long long) x.size();
    DVec e ((size_t) ((n + W - 1) / W));
    for (long long i = 0; i < n; ++i) { double& d = e[(size_t) (i / W)]; d = std::max (d, (double) std::abs (x[(size_t) i])); }
    return e;
}
// CDP grev/grainex/envspeak: envelope troughs deep enough relative to the neighbouring peaks (trough <= trof * mean of
// the two peaks), each moved to the quietest downward zero crossing within half a window. Returns cut points
// including 0 and n.
std::vector<long long> troughCuts (const Buf& m, double sr, double wsizeSec, double trof)
{
    const long long n = (long long) m.size(), W = std::max (8LL, jr (wsizeSec * sr));
    std::vector<long long> cuts { 0 };
    if (n > 2 * W)
    {
        DVec e = peakEnv (m, W);
        std::vector<size_t> ext; std::vector<int> kind;   // alternating peaks (1) and troughs (-1)
        for (size_t k = 1; k + 1 < e.size(); ++k)
        {
            if (e[k] > e[k - 1] && e[k] >= e[k + 1]) { ext.push_back (k); kind.push_back (1); }
            else if (e[k] < e[k - 1] && e[k] <= e[k + 1]) { ext.push_back (k); kind.push_back (-1); }
        }
        const long long half = W / 2, nb = std::max (2LL, jr (0.001 * sr));
        for (size_t j = 1; j + 1 < ext.size(); ++j)
        {
            if (kind[j] != -1 || kind[j - 1] != 1 || kind[j + 1] != 1) continue;
            if (e[ext[j]] > trof * 0.5 * (e[ext[j - 1]] + e[ext[j + 1]])) continue;
            long long c = (long long) ext[j] * W + W / 2, best = c; double bv = 1e30;
            for (long long i = std::max (1LL, c - half); i < std::min (n - 1, c + half); ++i)
                if (m[(size_t) i - 1] >= 0 && m[(size_t) i] < 0)
                {
                    double v = 0; for (long long q = std::max (0LL, i - nb); q < std::min (n, i + nb); ++q) v += std::abs (m[(size_t) q]);
                    if (v < bv) { bv = v; best = i; }
                }
            if (best > cuts.back() + 16 && best < n - 16) cuts.push_back (best);
        }
    }
    cuts.push_back (n);
    return cuts;
}
// cut points every `len` samples (fallback when nothing is found)
std::vector<long long> equalCuts (long long n, long long len)
{
    len = std::max (16LL, len); std::vector<long long> c;
    for (long long i = 0; i < n; i += len) c.push_back (i);
    c.push_back (n); return c;
}
// random permutation 0..n-1 (Fisher-Yates with the seeded generator)
std::vector<int> perm (int n, Ctx& cx)
{
    std::vector<int> p ((size_t) std::max (0, n)); for (int i = 0; i < n; ++i) p[(size_t) i] = i;
    for (int i = n - 1; i > 0; --i) std::swap (p[(size_t) i], p[(size_t) std::min (i, (int) (cx.rand() * (i + 1)))]);
    return p;
}
inline double ms (const Params& p, const char* id, double sr) { return p[id] / 1000.0 * sr; }
inline double st2r (double st) { return std::pow (2.0, st / 12.0); }
const double SPL = 0.0075;   // CDP grain half-splice (15 ms splices)
const double BUDGET = 1e7;   // max samples written by the event generators (keeps extreme settings fast)
// longest event allowed when about `events` events are generated
// (scaled with the sample rate, so a 96 kHz render keeps the same event lengths as a 44.1 kHz one)
inline long long evCap (double events, double sr) { return std::max (64LL, (long long) (BUDGET * std::max (1.0, sr / 44100) / std::max (1.0, events))); }

// copies the lead-in before the first grain unchanged; returns the output cursor
long long leadIn (Out& o, const Chans& ch, const Grains& g)
{
    if (g.on.empty() || g.on[0] <= 0) return 0;
    putSeg (o, ch, 0, (double) g.on[0], 0, g.on[0], 1, 1, 1);
    o.touch (g.on[0]);
    return g.on[0];
}

// ---------------------------------------------------------------------------------------------
// GRAIN programs (dev/grain)
// ---------------------------------------------------------------------------------------------

// Keep `keep` grains out of every `outof`, closing up the time. after CDP grain omit (grain1.c keep_non_omitted_grains)
Chans grainOmit (const Chans& ch, Ctx& cx, const Params& p)
{
    Grains g = findGrains (ch, p, cx.sr); const int outof = std::max (1, p.i ("outof")), keep = clamp (p.i ("keep"), 1, outof);
    Out o (ch.size(), cx.maxLen, ch[0].size()); long long cur = leadIn (o, ch, g);
    for (size_t i = 0; i < g.size(); ++i)
    {
        if ((int) (i % (size_t) outof) < keep) { putSeg (o, ch, (double) g.on[i], (double) g.gap (i), cur, g.gap (i), 1, SPL * cx.sr, 1); cur += g.gap (i); o.touch (cur); }
        if (! (i & 15)) cx.prog ((double) i / (double) g.size());
    }
    return o.take();
}
// Each grain repeated N times. after CDP grain duplicate (grain1.c duplicate_grain)
Chans grainDup (const Chans& ch, Ctx& cx, const Params& p)
{
    Grains g = findGrains (ch, p, cx.sr); const int N = std::max (1, p.i ("n"));
    Out o (ch.size(), cx.maxLen); long long cur = leadIn (o, ch, g);
    for (size_t i = 0; i < g.size() && cur < (long long) cx.maxLen; ++i)
    {
        for (int r = 0; r < N; ++r) { putSeg (o, ch, (double) g.on[i], (double) g.gap (i), cur, g.gap (i), 1, SPL * cx.sr, 1); cur += g.gap (i); }
        o.touch (cur); if (! (i & 15)) cx.prog ((double) i / (double) g.size());
    }
    return o.take();
}
// Grains re-sequenced by a jump code such as 'adb:c'. after CDP grain reorder (grain1.c do_the_reordered_grains)
const std::vector<std::pair<std::vector<int>, int>> REORDER {
    { { 1, 0 }, 2 }, { { 2, 1, 0 }, 3 }, { { 3, 2, 1, 0 }, 4 }, { { 0, 2, 1 }, 3 }, { { 0, 3, 1 }, 2 },
    { { 0, 0, 1 }, 2 }, { { 0, 1, 0 }, 1 }, { { 0, 2 }, 1 }, { { 1, 0, 2 }, 2 } };
Chans grainReorder (const Chans& ch, Ctx& cx, const Params& p)
{
    Grains g = findGrains (ch, p, cx.sr); const auto& pat = REORDER[(size_t) clamp (p.i ("mode"), 0, (int) REORDER.size() - 1)];
    int mx = 0; for (int k : pat.first) mx = std::max (mx, k);
    Out o (ch.size(), cx.maxLen, ch[0].size()); long long cur = leadIn (o, ch, g);
    for (size_t b = 0; b + (size_t) mx < g.size() && cur < (long long) cx.maxLen; b += (size_t) pat.second)
    {
        for (int k : pat.first) { size_t i = b + (size_t) k; putSeg (o, ch, (double) g.on[i], (double) g.gap (i), cur, g.gap (i), 1, SPL * cx.sr, 1); cur += g.gap (i); }
        o.touch (cur); cx.prog ((double) b / (double) g.size());
    }
    if (g.size() <= (size_t) mx) for (size_t i = 0; i < g.size(); ++i) { putSeg (o, ch, (double) g.on[i], (double) g.gap (i), cur, g.gap (i), 1, SPL * cx.sr, 1); cur += g.gap (i); o.touch (cur); }
    return o.take();
}
// shared by repitch / rerhythm / remotif: grain i through (transposition, onset-gap multiplier) steps, cycling through
// the steps grain by grain (CYCLE) or playing every step on each grain (EACH). The grain is read at the transposed
// speed only for its own length and cut (spliced) or padded with silence to fill the new gap.
Chans grainSteps (const Chans& ch, Ctx& cx, const Params& p, const std::vector<double>& st, const std::vector<double>& mult)
{
    Grains g = findGrains (ch, p, cx.sr); const bool each = p.i ("mode") == 1; const size_t S = st.size();
    Out o (ch.size(), cx.maxLen); long long cur = leadIn (o, ch, g); size_t k = 0;
    const long long minGap = jr (2 * SPL * cx.sr);
    for (size_t i = 0; i < g.size() && cur < (long long) cx.maxLen; ++i)
    {
        for (size_t r = 0; r < (each ? S : 1); ++r)
        {
            size_t s = each ? r : k++ % S;
            long long slot = std::max (minGap, jr ((double) g.gap (i) * mult[s]));
            putSeg (o, ch, (double) g.on[i], (double) g.gap (i), cur, slot, st2r (st[s]), SPL * cx.sr, 1);
            cur += slot; o.touch (cur);
        }
        if (! (i & 15)) cx.prog ((double) i / (double) g.size());
    }
    return o.take();
}
Chans grainRepitch (const Chans& ch, Ctx& cx, const Params& p)
{
    const int n = clamp (p.i ("steps"), 1, 4); std::vector<double> st, m;
    for (int i = 0; i < n; ++i) { st.push_back (p[std::string ("p") + (char) ('1' + i)]); m.push_back (1); }
    return grainSteps (ch, cx, p, st, m);
}
Chans grainRerhythm (const Chans& ch, Ctx& cx, const Params& p)
{
    const int n = clamp (p.i ("steps"), 1, 4); std::vector<double> st, m;
    for (int i = 0; i < n; ++i) { st.push_back (0); m.push_back (p[std::string ("m") + (char) ('1' + i)]); }
    return grainSteps (ch, cx, p, st, m);
}
Chans grainRemotif (const Chans& ch, Ctx& cx, const Params& p)
{
    const int n = clamp (p.i ("steps"), 1, 4); std::vector<double> st, m;
    for (int i = 0; i < n; ++i) { st.push_back (p[std::string ("p") + (char) ('1' + i)]); m.push_back (p[std::string ("m") + (char) ('1' + i)]); }
    return grainSteps (ch, cx, p, st, m);
}
// Inter-grain time stretched (START -> END over the sound), grains themselves untouched. after CDP grain timewarp (grain1.c retime_grain)
Chans grainTimewarp (const Chans& ch, Ctx& cx, const Params& p)
{
    Grains g = findGrains (ch, p, cx.sr); Out o (ch.size(), cx.maxLen); long long cur = leadIn (o, ch, g);
    const long long minGap = jr (2 * SPL * cx.sr);
    for (size_t i = 0; i < g.size() && cur < (long long) cx.maxLen; ++i)
    {
        double t = (double) g.on[i] / (double) std::max (1LL, g.len), r = p["start"] * std::pow (p["end"] / p["start"], t);
        long long slot = std::max (minGap, jr ((double) g.gap (i) * r));
        putSeg (o, ch, (double) g.on[i], (double) g.gap (i), cur, slot, 1, SPL * cx.sr, 1);
        cur += slot; o.touch (cur);
        if (! (i & 15)) cx.prog ((double) i / (double) g.size());
    }
    return o.take();
}
// Grain onsets moved onto the tempo grid: one grain per step (SEQUENCE) or each snapped to the nearest step (QUANTISE).
// after CDP grain reposition (grain1.c synchronise_the_grain), the time file replaced by the host tempo grid
Chans grainReposition (const Chans& ch, Ctx& cx, const Params& p)
{
    Grains g = findGrains (ch, p, cx.sr); const double step = divSec (p.i ("div"), cx.bpm) * cx.sr; const long long off = jr (ms (p, "offset", cx.sr));
    std::vector<long long> at;
    for (size_t i = 0; i < g.size(); ++i)
    {
        long long t = p.i ("mode") == 0 ? jr ((double) i * step) : jr (jsround ((double) g.on[i] / step) * step);
        if (! at.empty() && t <= at.back()) t = at.back() + jr (step);
        at.push_back (t + off);
    }
    Out o (ch.size(), cx.maxLen);
    for (size_t i = 0; i < g.size(); ++i)
    {
        if (at[i] >= (long long) cx.maxLen) break;
        long long slot = i + 1 < g.size() ? at[i + 1] - at[i] : g.gap (i);
        putSeg (o, ch, (double) g.on[i], (double) g.gap (i), at[i], std::min (slot, g.gap (i)), 1, SPL * cx.sr, 1);
        o.touch (at[i] + slot);
        if (! (i & 15)) cx.prog ((double) i / (double) g.size());
    }
    return o.take();
}
// Grains played in reverse order, each one still forwards. after CDP grain reverse (grain1.c do_the_reversing_process)
Chans grainReverse (const Chans& ch, Ctx& cx, const Params& p)
{
    Grains g = findGrains (ch, p, cx.sr); Out o (ch.size(), cx.maxLen, ch[0].size()); long long cur = leadIn (o, ch, g);
    for (size_t j = g.size(); j-- > 0;)
    {
        putSeg (o, ch, (double) g.on[j], (double) g.gap (j), cur, g.gap (j), 1, SPL * cx.sr, 1); cur += g.gap (j); o.touch (cur);
        if (! (j & 15)) cx.prog (1 - (double) j / (double) g.size());
    }
    return o.take();
}

// Grains found at envelope troughs (+ zero crossings), taken GROUP at a time: reversed in order, repeated, deleted,
// silenced, re-spaced without stretching them, or placed on the tempo grid. after CDP grain grev 1-5, 7 (grain1.c grev)
Chans grainGrev (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const long long n = (long long) m.size();
    auto cuts = troughCuts (m, cx.sr, p["wsize"] / 1000, p["trof"]);
    const size_t gp = (size_t) std::max (1, p.i ("group")), U = (cuts.size() - 1 + gp - 1) / gp;   // units of gp grains
    auto ua = [&] (size_t u) { return cuts[std::min (u * gp, cuts.size() - 1)]; };
    auto ub = [&] (size_t u) { return cuts[std::min ((u + 1) * gp, cuts.size() - 1)]; };
    const int mode = p.i ("mode"); const double fd = 0.001 * cx.sr;
    Out o (ch.size(), cx.maxLen, (size_t) n); long long cur = 0;
    const int outof = std::max (1, p.i ("outof")), keep = clamp (p.i ("keep"), 1, outof);
    for (size_t k = 0; k < U && cur < (long long) cx.maxLen; ++k)
    {
        const size_t u = mode == 0 ? U - 1 - k : k; const long long a = ua (u), len = ub (u) - a;
        switch (mode)
        {
            case 1: for (int r = 0; r < std::max (1, p.i ("repets")); ++r) { putSeg (o, ch, (double) a, (double) len, cur, len, 1, fd, 1); cur += len; } break;
            case 2: if ((int) (u % (size_t) outof) < keep) { putSeg (o, ch, (double) a, (double) len, cur, len, 1, fd, 1); cur += len; } break;
            case 3: if ((int) (u % (size_t) outof) < keep) putSeg (o, ch, (double) a, (double) len, cur, len, 1, fd, 1); cur += len; break;
            case 4: putSeg (o, ch, (double) a, (double) len, cur, len, 1, fd, 1); cur += std::max (1LL, jr ((double) len * p["stretch"])); break;
            case 5: { const long long st = jr ((double) k * divSec (p.i ("div"), cx.bpm) * cx.sr); putSeg (o, ch, (double) a, (double) len, st, len, 1, fd, 1); cur = st + len; break; }
            default: putSeg (o, ch, (double) a, (double) len, cur, len, 1, fd, 1); cur += len; break;
        }
        o.touch (cur);
        if (! (k & 15)) cx.prog ((double) k / (double) U);
    }
    return o.take();
}

// Iterative sounds (rolled 'rrr', drum rolls) extended by re-using their iteration units in random order (each unit at
// most REPEATS times in a row), with random level (AMP SCAT) and pitch (PITCH SCAT), optional slowing (RIT, silence
// inserted, growing over the extension) and REGULARising of the slowed rhythm. MARKED: region START-END; FOUND: the
// sounding part above GATE. after CDP grain r_extend 1-2 (grain1.c timestretch_iterative)
Chans grainRExtend (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const long long n = (long long) m.size();
    long long rs = 0, re = n;
    if (p.i ("mode") == 0) { rs = jr (p["start"] * (double) n); re = std::max (rs + 2, jr (p["end"] * (double) n)); }
    else
    {
        const double thr = peak (m) * std::pow (10.0, p["gate"] / 20); long long a = -1, b = -1;
        for (long long i = 0; i < n; ++i) if (std::abs (m[(size_t) i]) > thr) { if (a < 0) a = i; b = i; }
        if (a >= 0) { rs = a; re = b + 1; }
    }
    re = std::min (re, n); rs = clamp (rs, 0LL, std::max (0LL, re - 2));
    Buf reg (m.begin() + rs, m.begin() + re);
    auto cuts = troughCuts (reg, cx.sr, std::max (0.001, p["unit"] / 1000 / 3), 0.9);
    if (cuts.size() < 4) cuts = equalCuts ((long long) reg.size(), jr (ms (p, "unit", cx.sr)));
    for (auto& c : cuts) c += rs;
    const int U = (int) cuts.size() - 1, rep = std::max (1, p.i ("rep"));
    cx.seed (p["seed"]);
    const double fd = 0.001 * cx.sr, need = (double) (re - rs) * p["stretch"];
    Out o (ch.size(), cx.maxLen); long long cur = 0;
    putSeg (o, ch, 0, (double) rs, 0, rs, 1, fd, 1); cur = rs; o.touch (cur);   // spliced (not cut dead) into the re-used units
    double done = 0; int last = -1, run = 0;
    std::vector<int> order; size_t oi = 0;
    while (done < need && cur < (long long) cx.maxLen)
    {
        if (oi >= order.size()) { order.clear(); for (int r = 0; r < rep; ++r) { auto pr = perm (U, cx); order.insert (order.end(), pr.begin(), pr.end()); } oi = 0; }
        int u = order[oi++];
        if (U > 1 && u == last && run >= rep) continue;
        run = u == last ? run + 1 : 1; last = u;
        const long long a = cuts[(size_t) u], len = cuts[(size_t) u + 1] - a;
        const double gain = 1 - cx.rand() * p["ascat"];
        const double tr = st2r ((cx.rand() < 0.5 ? -1 : 1) * cx.rand() * p["pscat"]);
        const long long dur = std::max (1LL, jr ((double) len / tr));
        putSeg (o, ch, (double) a, (double) len, cur, dur, tr, fd, gain);
        // slowing: silence after each unit grows from none to (RIT - 1) x the unit length; REG pulls the step to the average
        const double slow = 1 + (p["rit"] - 1) * clampd (done / std::max (1.0, need), 0, 1);
        const double avg = (double) (re - rs) / std::max (1, U) * slow;
        double step = (double) dur * slow; step += (avg - step) * p["reg"] * (slow > 1.001 ? 1 : 0);
        cur += std::max (dur, jr (step)); done += (double) len; o.touch (cur);
        cx.prog (done / std::max (1.0, need));
    }
    putSeg (o, ch, (double) re, (double) (n - re), cur, n - re, 1, fd, 1); o.touch (cur + n - re);
    return o.take();
}

// The noise part of a sound (wave-cycles shorter than 1/MIN FREQ, lasting at least MIN DUR) is found and stretched to
// DURATION by random reads between its zero crossings. after CDP grain noise_extend (grain1.c grab_noise_and_expand)
Chans grainNoiseExt (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const long long n = (long long) m.size();
    const double pk = peak (m), gate = pk * 0.01; const long long maxCyc = std::max (2LL, jr (cx.sr / p["minfrq"]));
    const long long minDur = jr (ms (p, "mindur", cx.sr)), maxDur = std::max (minDur + 1, jr (p["maxdur"] * cx.sr));
    // wave-cycles (zero crossing to the next crossing of the same direction)
    long long ns = -1, ne = -1, runS = -1, last = 0;
    for (long long i = 1; i < n && ns < 0; ++i)
    {
        if (! (m[(size_t) i - 1] < 0 && m[(size_t) i] >= 0)) continue;
        double mx = 0; for (long long q = last; q < i; ++q) mx = std::max (mx, (double) std::abs (m[(size_t) q]));
        bool noisy = mx >= gate && i - last < maxCyc;
        if (noisy) { if (runS < 0) runS = last; if (i - runS >= maxDur) { ns = runS; ne = i; } }
        else { if (runS >= 0 && last - runS >= minDur) { ns = runS; ne = last; } runS = -1; }
        last = i;
    }
    if (ns < 0 && runS >= 0 && last - runS >= minDur) { ns = runS; ne = last; }
    if (ns < 0)
    {   // nothing qualifies: take the window with the highest zero-crossing rate
        const long long W = std::max (64LL, std::min (n, std::max (minDur, jr (0.05 * cx.sr)))); long long best = 0, bz = -1;
        for (long long a = 0; a + W <= n; a += W / 2)
        {
            long long z = 0; for (long long i = a + 1; i < a + W; ++i) z += (m[(size_t) i - 1] < 0) != (m[(size_t) i] < 0);
            if (z > bz) { bz = z; best = a; }
        }
        ns = best; ne = std::min (n, best + W);
    }
    std::vector<long long> zc;
    for (long long i = std::max (1LL, ns); i < ne; ++i) if ((m[(size_t) i - 1] < 0) != (m[(size_t) i] < 0)) zc.push_back (i);
    if (zc.size() < 3) { zc.clear(); for (long long i = ns; i < ne; i += std::max (1LL, (ne - ns) / 8)) zc.push_back (i); zc.push_back (ne); }
    cx.seed (p["seed"]);
    const bool keepAll = p.i ("keep") == 0; const long long D = jr (p["dur"] * cx.sr), fd = std::max (1LL, jr (0.0005 * cx.sr));
    Out o (ch.size(), cx.maxLen); long long cur = 0;
    if (keepAll) { putSeg (o, ch, 0, (double) ns, 0, ns, 1, (double) fd, 1); cur = ns; }   // spliced into the noise reads
    const long long end = cur + D; const long long maxSeg = std::max (64LL, jr (0.05 * cx.sr));
    while (cur < end && cur < (long long) cx.maxLen)
    {
        size_t a = (size_t) (cx.rand() * (double) zc.size()), b = (size_t) (cx.rand() * (double) zc.size());
        a = std::min (a, zc.size() - 1); b = std::min (b, zc.size() - 1); if (a > b) std::swap (a, b); if (a == b) continue;
        long long len = std::min (zc[b] - zc[a], maxSeg); len = std::min (len, end - cur); if (len < 2) { cur += 1; continue; }
        putSeg (o, ch, (double) zc[a], (double) len, cur, len, 1, (double) fd, 1);
        cur += len; o.touch (cur);
        cx.prog (1 - (double) (end - cur) / (double) std::max (1LL, D));
    }
    o.touch (cur);
    if (keepAll) { putSeg (o, ch, (double) ne, (double) (n - ne), cur, n - ne, 1, (double) fd, 1); o.touch (cur + n - ne); }
    return o.take();
}

// The grainy part START-END is lengthened by ADD seconds: its trough-separated grains are re-used in random order
// (no grain twice in a row). after CDP grainex extend (standalone/grainex.c grevex)
Chans grainexExtend (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const long long n = (long long) m.size();
    const long long rs = jr (p["start"] * (double) n), re = std::max (rs + 2, std::min (n, jr (p["end"] * (double) n)));
    auto all = troughCuts (m, cx.sr, p["wsize"] / 1000, p["trof"]);
    std::vector<long long> c;
    for (long long q : all) if (q >= rs && q <= re) c.push_back (q);
    if (c.size() < 3) { c = equalCuts (re - rs, jr (p["wsize"] / 1000 * cx.sr * 2)); for (auto& q : c) q += rs; }
    const long long a0 = c.front(), a1 = c.back(); const int U = (int) c.size() - 1;
    cx.seed (p["seed"]);
    Out o (ch.size(), cx.maxLen); const double fd = 0.001 * cx.sr;
    putSeg (o, ch, 0, (double) a0, 0, a0, 1, fd, 1); long long cur = a0;
    const double need = (double) (a1 - a0) + p["plus"] * cx.sr; double done = 0; int last = -1;
    std::vector<int> order; size_t oi = 0;
    while (done < need && cur < (long long) cx.maxLen)
    {
        if (oi >= order.size()) { order = perm (U, cx); oi = 0; if (U > 1 && order[0] == last) std::swap (order[0], order[(size_t) U - 1]); }
        int u = order[oi++]; last = u;
        const long long a = c[(size_t) u], len = c[(size_t) u + 1] - a;
        putSeg (o, ch, (double) a, (double) len, cur, len, 1, fd, 1); cur += len; done += (double) len; o.touch (cur);
        cx.prog (done / need);
    }
    putSeg (o, ch, (double) a1, (double) (n - a1), cur, n - a1, 1, fd, 1); o.touch (cur + n - a1);
    return o.take();
}

// ---------------------------------------------------------------------------------------------
// BRASSAGE (dev/modify/granula1.c granula_process) and WRAPPAGE
// ---------------------------------------------------------------------------------------------
// Every parameter is a [lo, hi] range: a grain draws a random value in it (CDP's h* parameters).
struct BrassP
{
    double vlo = 1, vhi = 1;       // velocity: advance in the source per output step (1 / time-stretch)
    double dlo = 2, dhi = 2;       // density: grain overlap (< 1 leaves gaps)
    double glo = 50, ghi = 50;     // grain size, ms
    double plo = 0, phi = 0;       // pitch shift, semitones
    double alo = 1, ahi = 1;       // grain gain
    double bslo = 5, bshi = 5, eslo = 5, eshi = 5;   // start / end splices, ms
    double range = 0;              // search range before 'now' in the source, ms
    double scatter = 0.5;          // jitter of the output position (fraction of the output step)
    bool pan = false;              // spatialise (mono mix placed by panFn)
    std::function<double (double)> panFn;   // pan for a grain at output fraction 0..1
    double outSec = 0;             // nominal output length (0 = until the source is used up)
};
Chans brassEngine (const Chans& ch, Ctx& cx, const BrassP& b)
{
    const double sr = cx.sr; const long long n = (long long) ch[0].size();
    auto rnd = [&] (double lo, double hi) { return hi == lo ? lo : lo + cx.rand() * (hi - lo); };
    const double dmean = 0.5 * (b.dlo + b.dhi), norm = 1 / std::max (1.0, std::pow (dmean, 0.75));
    const double vmean = std::max (0.01, 0.5 * (b.vlo + b.vhi));
    const double nominal = b.outSec > 0 ? b.outSec * sr : (double) n / vmean;
    const long long outCap = (long long) std::min ((double) cx.maxLen, nominal + 0.5 * sr);
    Out o (b.pan ? 2 : ch.size(), (size_t) std::max (1LL, outCap));
    double inpos = 0, outpos = 0; long long cnt = 0;
    while (outpos < (double) outCap)
    {
        const long long g = std::max (8LL, jr (rnd (b.glo, b.ghi) / 1000 * sr));
        const double tr = st2r (rnd (b.plo, b.phi));
        if (b.outSec <= 0 && inpos + (double) g * std::max (1.0, tr) >= (double) n) break;
        const double dens = std::max (0.01, rnd (b.dlo, b.dhi));
        const double ostep = std::max (1.0, (double) g / dens);
        const double scat = b.scatter * cx.rand() * ostep;
        const double istep = rnd (b.vlo, b.vhi) * ostep;
        double rd = inpos - cx.rand() * std::min (b.range / 1000 * sr, inpos);
        if (b.outSec > 0 && n > 0) { rd = std::fmod (rd, (double) n); if (rd < 0) rd += (double) n; }   // fixed length: wrap round the source
        const double amp = rnd (b.alo, b.ahi) * norm;
        const double bs = std::max (1.0, rnd (b.bslo, b.bshi) / 1000 * sr), es = std::max (1.0, rnd (b.eslo, b.eshi) / 1000 * sr);
        const double pan = b.pan ? clampd (b.panFn ? b.panFn (outpos / (double) outCap) : 0.5, 0, 1) : -1;
        double gl = amp, gr = amp; if (pan >= 0) { auto pg = panG (pan); gl *= pg.first; gr *= pg.second; }
        const long long at = jr (outpos + scat);
        for (long long i = 0; i < g; ++i)
        {
            const long long op = at + i; if (op >= outCap) break;
            const double e = std::min ({ 1.0, (double) (i + 1) / std::min (bs, (double) g / 2), (double) (g - i) / std::min (es, (double) g / 2) });
            const double sp = rd + (double) i * tr;
            if (sp >= (double) n) break;
            if (pan >= 0)
            {
                double v = 0; for (auto& c : ch) v += lerpAt (c, sp); v = v / (double) ch.size() * e;
                o.add (0, op, v * gl); o.add (1, op, v * gr);
            }
            else for (size_t c = 0; c < ch.size(); ++c) o.add (c, op, lerpAt (ch[c], sp) * e * amp);
        }
        inpos += istep; outpos += ostep;
        if (b.outSec > 0 && inpos >= (double) n) inpos = std::fmod (inpos, std::max (1.0, (double) n));
        if (! (++cnt & 63)) cx.prog (outpos / (double) outCap);
    }
    o.touch (jr (std::min (outpos, (double) outCap)));
    return o.take();
}
// Granular pitch-shift: grains re-read at the new speed, the length stays. after CDP modify brassage 1 (granula1.c)
Chans brass1 (const Chans& ch, Ctx& cx, const Params& p)
{ BrassP b; cx.seed (p["seed"]); b.plo = b.phi = p["pitch"]; b.glo = b.ghi = p["grain"]; return brassEngine (ch, cx, b); }
// Granular time-stretch: the source is read VELOCITY times as fast as the output runs. after CDP modify brassage 2
Chans brass2 (const Chans& ch, Ctx& cx, const Params& p)
{ BrassP b; cx.seed (p["seed"]); b.vlo = b.vhi = p["vel"]; b.glo = b.ghi = p["grain"]; return brassEngine (ch, cx, b); }
// Granular 'reverb': dense grains from a search range behind 'now', with random pitch and level spread. after CDP modify brassage 3
Chans brass3 (const Chans& ch, Ctx& cx, const Params& p)
{
    BrassP b; cx.seed (p["seed"]); b.dlo = b.dhi = p["dens"]; b.plo = -p["pitch"]; b.phi = p["pitch"]; b.alo = 1 - p["amp"]; b.ahi = 1;
    b.range = p["range"]; b.glo = b.ghi = p["grain"]; return brassEngine (ch, cx, b);
}
// Granular scramble: each grain is drawn from anywhere within RANGE before the current position. after CDP modify brassage 4
Chans brass4 (const Chans& ch, Ctx& cx, const Params& p)
{ BrassP b; cx.seed (p["seed"]); b.glo = b.ghi = p["grain"]; b.range = p["range"]; return brassEngine (ch, cx, b); }
// Granulate: grain DENSITY changed, below 1 the grains separate into a stream with gaps. after CDP modify brassage 5
Chans brass5 (const Chans& ch, Ctx& cx, const Params& p)
{ BrassP b; cx.seed (p["seed"]); b.dlo = b.dhi = p["dens"]; b.glo = b.ghi = p["grain"]; return brassEngine (ch, cx, b); }
// Full brassage with fixed values. after CDP modify brassage 6
Chans brass6 (const Chans& ch, Ctx& cx, const Params& p)
{
    BrassP b; cx.seed (p["seed"]); b.vlo = b.vhi = p["vel"]; b.dlo = b.dhi = p["dens"]; b.glo = b.ghi = p["grain"]; b.plo = b.phi = p["pitch"];
    b.alo = b.ahi = p["amp"]; b.bslo = b.bshi = p["bspl"]; b.eslo = b.eshi = p["espl"]; b.range = p["range"]; b.scatter = p["jitter"];
    b.pan = true; const double sp = p["space"]; b.panFn = [sp] (double) { return sp; };
    return brassEngine (ch, cx, b);
}
// 'Full monty': every parameter drawn per grain from a LOW-HIGH range. after CDP modify brassage 7
Chans brass7 (const Chans& ch, Ctx& cx, const Params& p)
{
    BrassP b; cx.seed (p["seed"]);
    b.vlo = std::min (p["vlo"], p["vhi"]); b.vhi = std::max (p["vlo"], p["vhi"]); b.dlo = std::min (p["dlo"], p["dhi"]); b.dhi = std::max (p["dlo"], p["dhi"]);
    b.glo = std::min (p["glo"], p["ghi"]); b.ghi = std::max (p["glo"], p["ghi"]); b.plo = std::min (p["plo"], p["phi"]); b.phi = std::max (p["plo"], p["phi"]);
    b.alo = p["amin"]; b.ahi = 1; b.scatter = p["jitter"]; b.pan = true;
    const double w = p["spread"]; Ctx* c = &cx; b.panFn = [w, c] (double) { return 0.5 + (c->rand() - 0.5) * w; };
    return brassEngine (ch, cx, b);
}
// Brassage spread over the stereo field around a CENTRE that moves from START to END: grains land at random within
// SPREAD of it, DEPTH fills in the middle (0 = only the two edges of the spread). after CDP wrappage (standalone/wrappage.c granulate), folded to stereo
Chans wrappage (const Chans& ch, Ctx& cx, const Params& p)
{
    BrassP b; cx.seed (p["seed"]); b.vlo = b.vhi = p["vel"]; b.dlo = b.dhi = p["dens"]; b.glo = b.ghi = p["grain"];
    b.plo = std::min (p["plo"], p["phi"]); b.phi = std::max (p["plo"], p["phi"]); b.scatter = p["jitter"]; b.pan = true;
    const double c0 = p["cstart"], c1 = p["cend"], w = p["spread"], d = p["depth"]; Ctx* c = &cx;
    b.panFn = [=] (double t)
    {
        double ctr = c0 + (c1 - c0) * t, u = c->rand() * 2 - 1;   // -1..1 across the spread
        double edge = u < 0 ? -1 : 1; u = edge + (u - edge) * d;   // depth 0 pins grains to the edges
        return ctr + 0.5 * w * u;
    };
    return brassEngine (ch, cx, b);
}
// ---------------------------------------------------------------------------------------------
// ENVNU PEAKCHOP and RETIME (silence-separated events)
// ---------------------------------------------------------------------------------------------

// Envelope peaks isolated (PEAK WIDTH of sound with RISE splices, SKEW places the peak in that window) and replayed one
// per tempo step, with SCATTERed timing, level NORMalising, REPEATs and MISSed peaks. after CDP envnu peakchop 1
// (standalone/envnu.c peakchop), the MM tempo replaced by the host tempo
Chans peakchop (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const long long n = (long long) m.size(), W = std::max (8LL, jr (ms (p, "wsize", cx.sr)));
    DVec e = peakEnv (m, W); std::vector<long long> pk; std::vector<double> pv;
    const double mx = peak (m), gate = p["gate"] * mx;
    for (size_t k = 0; k < e.size(); ++k)
    {
        bool isMax = (k == 0 || e[k] > e[k - 1]) && (k + 1 == e.size() || e[k] >= e[k + 1]);
        if (! isMax || e[k] <= 0 || e[k] < gate) continue;
        long long a = (long long) k * W, best = a;
        for (long long i = a; i < std::min (n, a + W); ++i) if (std::abs (m[(size_t) i]) > std::abs (m[(size_t) best])) best = i;
        pk.push_back (best); pv.push_back (e[k]);
    }
    if (pk.empty()) { pk.push_back (0); pv.push_back (std::max (1e-9, mx)); }
    cx.seed (p["seed"]);
    const double step = divSec (p.i ("div"), cx.bpm) * cx.sr; const long long width = jr (ms (p, "width", cx.sr)), rise = std::max (1LL, jr (ms (p, "rise", cx.sr)));
    const int rep = std::max (0, p.i ("repeat")), miss = std::max (0, p.i ("miss"));
    std::vector<double> times; std::vector<size_t> which;
    for (size_t slot = 0;; ++slot)
    {
        size_t mi = (slot / (size_t) (rep + 1)) * (size_t) (miss + 1);
        if (mi >= pk.size() || (double) slot * step >= (double) cx.maxLen) break;
        times.push_back ((double) slot * step); which.push_back (mi);
    }
    for (size_t k = 1; k + 1 < times.size(); ++k)   // scatter about the exact positions
    {
        double s = (cx.rand() * 2 - 1) * p["scatter"];
        times[k] += s > 0 ? s * (times[k + 1] - times[k]) * 0.5 : s * (times[k] - times[k - 1]) * 0.5;
    }
    Out o (ch.size(), cx.maxLen);
    for (size_t k = 0; k < times.size(); ++k)
    {
        const size_t i = which[k]; const double norm = 1 + (std::min (mx / pv[i], 100.0) - 1) * p["norm"];
        const long long flat = pk[i] - jr (p["skew"] * (double) width), a = std::max (0LL, flat - rise);
        const long long len = std::min ({ n - a, (flat - a) + width + rise, evCap ((double) times.size(), cx.sr) });
        if (len <= 0) continue;
        const long long at = jr (times[k]);
        for (long long j = 0; j < len; ++j)
        {
            const long long sp = a + j; double env = 1;
            if (sp < flat) env = (double) (sp - a) / (double) std::max (1LL, flat - a);
            else if (sp >= flat + width) env = 1 - (double) (sp - flat - width) / (double) rise;
            for (size_t c = 0; c < ch.size(); ++c) o.add (c, at + j, ch[c][(size_t) sp] * env * norm * p["gain"]);
        }
        o.touch (at + jr (step));
        if (! (k & 15)) cx.prog ((double) k / (double) times.size());
    }
    return o.take();
}

std::vector<Ev> retimeEvents (const Chans& ch, const Params& p, double sr) { return gateEvents (ch, p["gate"], p["minsil"] / 1000, sr); }
long long evPeak (const Chans& ch, const Ev& e)
{
    long long b = e.s; double v = -1;
    for (long long i = e.s; i < e.e; ++i) for (auto& c : ch) if (std::abs (c[(size_t) i]) > v) { v = std::abs (c[(size_t) i]); b = i; }
    return b;
}
const double RSPL = 0.003;

// Events placed one per tempo step: their onsets (ALIGN ONSETS) or their loudest points (ALIGN PEAKS) land on the beat.
// after CDP retime retime 4 and 1 (standalone/retime.c retime / retime_found_peaks), user-marked peaks replaced by detection
Chans retimeTempo (const Chans& ch, Ctx& cx, const Params& p)
{
    auto ev = retimeEvents (ch, p, cx.sr); const double step = divSec (p.i ("div"), cx.bpm) * cx.sr; const bool peaks = p.i ("mode") == 1;
    long long lead = 0; std::vector<long long> pkOff;
    for (auto& e : ev) { long long o = peaks ? evPeak (ch, e) - e.s : 0; pkOff.push_back (o); lead = std::max (lead, o); }
    Out o (ch.size(), cx.maxLen);
    for (size_t k = 0; k < ev.size(); ++k)
    {
        const long long at = lead + jr ((double) k * step) - pkOff[k], len = ev[k].e - ev[k].s;
        if (at >= (long long) cx.maxLen) break;
        putSeg (o, ch, (double) ev[k].s, (double) len, at, len, 1, RSPL * cx.sr, 1);
        o.touch (lead + jr ((double) (k + 1) * step));
        if (! (k & 15)) cx.prog ((double) k / (double) ev.size());
    }
    return o.take();
}
// Events longer than IN WIDTH are cut down to OUT WIDTH (with a SPLICE), in place: tightens a phrase into staccato.
// after CDP retime retime 3 (standalone/retime.c shave)
Chans retimeShave (const Chans& ch, Ctx& cx, const Params& p)
{
    auto ev = retimeEvents (ch, p, cx.sr); const long long inW = jr (ms (p, "inw", cx.sr)), outW = jr (ms (p, "outw", cx.sr)), spl = std::max (1LL, jr (ms (p, "splice", cx.sr)));
    Chans out = ch; const long long n = (long long) ch[0].size();
    for (size_t k = 0; k < ev.size(); ++k)
    {
        const long long len = (k + 1 < ev.size() ? ev[k + 1].s : n) - ev[k].s;
        if (len < inW || outW >= len) continue;
        const long long cut = ev[k].s + outW, stop = k + 1 < ev.size() ? ev[k + 1].s : n;
        for (auto& c : out) for (long long i = cut; i < stop; ++i) c[(size_t) i] = (float) (c[(size_t) i] * std::max (0.0, 1 - (double) (i - cut) / (double) spl));
    }
    cx.prog (1);
    return out;
}
// The event sequence plays FACTOR times faster or slower (START -> END over the sound) without changing the events.
// after CDP retime retime 5 (standalone/retime.c retime)
Chans retimeSpeed (const Chans& ch, Ctx& cx, const Params& p)
{
    auto ev = retimeEvents (ch, p, cx.sr); const double n = (double) ch[0].size();
    Out o (ch.size(), cx.maxLen); double at = (double) ev[0].s;
    for (size_t k = 0; k < ev.size() && at < (double) cx.maxLen; ++k)
    {
        if (k > 0)
        {
            double t = (double) ev[k - 1].s / std::max (1.0, n), f = p["start"] * std::pow (p["end"] / p["start"], t);
            at += (double) (ev[k].s - ev[k - 1].s) / f;
        }
        const long long len = ev[k].e - ev[k].s;
        putSeg (o, ch, (double) ev[k].s, (double) len, jr (at), len, 1, RSPL * cx.sr, 1);
        o.touch (jr (at) + len);
        if (! (k & 15)) cx.prog ((double) k / (double) ev.size());
    }
    return o.take();
}
// rhythm patterns: positions (in steps) inside a cycle of C steps
struct RPat { std::vector<double> pos; double C; };
const std::vector<RPat>& rpats()
{
    static const std::vector<RPat> P {
        { { 0 }, 1 }, { { 0, 2.0 / 3 }, 1 }, { { 0, 0.5, 0.75 }, 1 }, { { 0, 0.75, 1.5 }, 2 }, { { 0, 0.75, 1.5, 2.5, 3 }, 4 },
        { { 0, 0.5, 1.5, 2, 2.75, 3.5 }, 4 }, { { 0, 1, 1.75, 2.375, 2.875, 3.25, 3.5, 3.75 }, 4 }, { { 0, 0.25, 0.5, 0.75, 1.125, 1.625, 2.25, 3 }, 4 },
        { { 0, 0.5, 1, 1.5, 3 }, 4 } };
    return P;
}
// Events placed at the beat positions of a rhythm PATTERN (repeating), STEP long beats at the host tempo, after an OFFSET.
// after CDP retime retime 6-7 (standalone/retime.c retempo_events), the beat / time lists replaced by patterns
Chans retimePattern (const Chans& ch, Ctx& cx, const Params& p)
{
    auto ev = retimeEvents (ch, p, cx.sr); const auto& rp = rpats()[(size_t) clamp (p.i ("pattern"), 0, (int) rpats().size() - 1)];
    const double step = divSec (p.i ("div"), cx.bpm) * cx.sr; const long long off = jr (ms (p, "offset", cx.sr));
    Out o (ch.size(), cx.maxLen);
    for (size_t k = 0; k < ev.size(); ++k)
    {
        const size_t cyc = k / rp.pos.size(), j = k % rp.pos.size();
        const long long at = off + jr (((double) cyc * rp.C + rp.pos[j]) * step), len = ev[k].e - ev[k].s;
        if (at >= (long long) cx.maxLen) break;
        putSeg (o, ch, (double) ev[k].s, (double) len, at, len, 1, RSPL * cx.sr, 1);
        o.touch (at + len);
        if (! (k & 15)) cx.prog ((double) k / (double) ev.size());
    }
    return o.take();
}
// A group of COUNT events found near WHERE is repeated REPEATS times on the tempo grid inside the sound; the rest follows.
// after CDP retime retime 8 (standalone/retime.c repeat_beats)
Chans retimeRepeat (const Chans& ch, Ctx& cx, const Params& p)
{
    auto ev = retimeEvents (ch, p, cx.sr); const long long n = (long long) ch[0].size(); const long long where = jr (p["where"] * (double) n);
    size_t a = 0; for (size_t k = 0; k < ev.size(); ++k) if (ev[k].s <= where) a = k;
    const size_t b = std::min (ev.size(), a + (size_t) std::max (1, p.i ("cnt")));
    const long long gs = ev[a].s, ge = b < ev.size() ? ev[b].s : n;
    const double step = divSec (p.i ("div"), cx.bpm) * cx.sr;
    const long long P = std::max (1LL, jr (std::ceil ((double) (ge - gs) / step - 1e-6) * step)); const int R = std::max (1, p.i ("repeats"));
    Out o (ch.size(), cx.maxLen);
    putSeg (o, ch, 0, (double) gs, 0, gs, 1, 1, 1);
    for (int r = 0; r <= R; ++r)
    {
        const long long at = gs + (long long) r * P; if (at >= (long long) cx.maxLen) break;
        putSeg (o, ch, (double) gs, (double) (ge - gs), at, std::min (ge - gs, P), 1, RSPL * cx.sr, 1);
        cx.prog ((double) r / (double) (R + 1));
    }
    const long long after = gs + (long long) (R + 1) * P;
    putSeg (o, ch, (double) ge, (double) (n - ge), after, n - ge, 1, RSPL * cx.sr, 1);
    o.touch (after + n - ge);
    return o.take();
}
// Events silenced by a repeating on/off pattern of STEPS steps. after CDP retime retime 9 (standalone/retime.c mask_events)
Chans retimeMask (const Chans& ch, Ctx& cx, const Params& p)
{
    auto ev = retimeEvents (ch, p, cx.sr); const int S = clamp (p.i ("steps"), 1, 8); Chans out = ch; const long long n = (long long) ch[0].size();
    const long long f = std::max (1LL, jr (RSPL * cx.sr));
    for (size_t k = 0; k < ev.size(); ++k)
    {
        if (p.i (std::string ("s") + (char) ('1' + (int) (k % (size_t) S)))) continue;
        const long long a = ev[k].s, b = k + 1 < ev.size() ? ev[k + 1].s : n;
        for (auto& c : out) for (long long i = a; i < b; ++i)
        {
            double g = 1 - std::min ({ 1.0, (double) (i - a) / (double) f, (double) (b - i) / (double) f });
            c[(size_t) i] = (float) (c[(size_t) i] * g);
        }
    }
    cx.prog (1);
    return out;
}
// Event levels evened out (EVENING 0-1), or every METER-th event accented with the others at EVENING of its level.
// after CDP retime retime 10 (standalone/retime.c equalise_event_levels)
Chans retimeLevel (const Chans& ch, Ctx& cx, const Params& p)
{
    auto ev = retimeEvents (ch, p, cx.sr); const long long n = (long long) ch[0].size(); const int meter = p.i ("meter");
    std::vector<double> lv; double mx = 0;
    for (auto& e : ev) { double v = 0; for (auto& c : ch) for (long long i = e.s; i < e.e; ++i) v = std::max (v, (double) std::abs (c[(size_t) i])); lv.push_back (v); mx = std::max (mx, v); }
    std::vector<double> g (ev.size(), 1.0);
    for (size_t k = 0; k < ev.size(); ++k)
    {
        if (lv[k] <= 1e-9) continue;
        const double full = std::min (50.0, mx / lv[k]);
        if (meter == 0) g[k] = std::pow (full, p["evening"]);
        else if (meter == 1) g[k] = full;
        else g[k] = full * (k % (size_t) meter == 0 ? 1.0 : p["evening"]);
    }
    Chans out = ch; const long long f = std::max (1LL, jr (RSPL * cx.sr));
    for (size_t k = 0; k < ev.size(); ++k)
    {
        const long long a = ev[k].s, b = k + 1 < ev.size() ? ev[k + 1].s : n; const double gp = k > 0 ? g[k - 1] : g[k];
        for (auto& c : out) for (long long i = a; i < b; ++i)
        {
            double t = std::min (1.0, (double) (i - a) / (double) f);
            c[(size_t) i] = (float) (c[(size_t) i] * (gp + (g[k] - gp) * t));
        }
    }
    cx.prog (1);
    return out;
}
// ---------------------------------------------------------------------------------------------
// Syllable / element re-sequencers: envspeak, fturanal synth, packet, dvdwind, sorter, stutter, motor
// ---------------------------------------------------------------------------------------------
struct Pc { double a, len; bool rev; double gain; double ratio = 1; };
// concatenation with overlapping splices (CDP envspeak: each new piece starts one splice before the last one ends)
Chans joinPieces (const Chans& ch, Ctx& cx, const std::vector<Pc>& pcs, double spl, double p0 = 0, double p1 = 1)
{
    Out o (ch.size(), cx.maxLen); long long cur = 0;
    for (size_t k = 0; k < pcs.size() && cur < (long long) cx.maxLen; ++k)
    {
        const Pc& q = pcs[k]; const long long len = jr (q.len / q.ratio); if (len <= 0) continue;
        const double f = std::max (1.0, std::min (spl, (double) len / 2));
        const long long at = std::max (0LL, cur - (k ? jr (f) : 0));
        putSeg (o, ch, q.a, q.len, at, len, q.ratio, f, q.gain, -1, q.rev);
        cur = at + len; o.touch (cur);
        if (! (k & 31)) cx.prog (p0 + (p1 - p0) * (double) k / (double) pcs.size());
    }
    return o.take();
}
// segmentation shared by the syllable programs: envelope troughs, onsets, or a tempo grid
std::vector<long long> segCuts (const Chans& ch, Ctx& cx, int how, double wsizeSec)
{
    const long long n = (long long) ch[0].size();
    std::vector<long long> c;
    switch (how)
    {
        case 0: c = troughCuts (monoOf (ch), cx.sr, wsizeSec, 0.8); break;
        case 1: { for (auto& e : gateEvents (ch, -20, 0.01, cx.sr)) c.push_back (e.s); c[0] = 0; c.push_back (n); break; }
        default: c = equalCuts (n, jr (divSec (how == 2 ? 3 : 4, cx.bpm) * cx.sr)); break;
    }
    if (c.size() < 2) c = { 0, n };
    return c;
}
const defs::O CUTS { "ENVELOPE", "ONSETS", "1/8 GRID", "1/16 GRID" };

// Speech 'syllables' (found at envelope troughs, or ONSETS / a tempo GRID) are repeated, reverse-repeated, attenuated in
// patterns, repeated while shrinking or lopped, repeated as a fraction, shuffled or reversed N at a time.
// after CDP envspeak 1-24 (standnew/envspeak.c envspeak), the cut-times file of modes 13-24 replaced by CUT
Chans envspeak (const Chans& ch, Ctx& cx, const Params& p)
{
    auto c = segCuts (ch, cx, p.i ("cut"), p["wsize"] / 1000);
    const int mode = p.i ("mode"), N = std::max (1, p.i ("n")), off = std::max (0, p.i ("offset"));
    const size_t S = c.size() - 1; cx.seed (p["seed"]);
    const double att = std::pow (10.0, p["atten"] / 20), rnd = p["rand"], ratio = p["ratio"];
    std::vector<size_t> order (S); for (size_t i = 0; i < S; ++i) order[i] = i;
    if (mode == 9) { auto pr = perm ((int) (S - std::min (S, (size_t) off)), cx); for (size_t i = 0; i < pr.size(); ++i) order[(size_t) off + i] = (size_t) off + (size_t) pr[i]; }
    if (mode == 10) for (size_t g = (size_t) off; g < S; g += (size_t) N) std::reverse (order.begin() + (long) g, order.begin() + (long) std::min (S, g + (size_t) N));
    std::vector<Pc> pcs;
    for (size_t k = 0; k < S; ++k)
    {
        const size_t s = order[k]; const double a = (double) c[s], len = (double) (c[s + 1] - c[s]);
        if ((int) k < off || mode >= 9) { pcs.push_back ({ a, len, false, 1 }); continue; }
        const size_t i = k - (size_t) off;
        switch (mode)
        {
            case 0: pcs.push_back ({ a, len, false, 1 }); for (int r = 1; r < N; ++r) pcs.push_back ({ a, len * (1 - rnd * cx.rand() * 0.9), false, 1 }); break;
            case 1: pcs.push_back ({ a, len, false, 1 }); pcs.push_back ({ a, len, true, 1 }); break;
            case 2: pcs.push_back ({ a, len, false, (int) (i % (size_t) (N + 1)) < N ? att : 1.0 }); break;
            case 3: pcs.push_back ({ a, len, false, (int) (i % (size_t) (N + 1)) < N ? 1.0 : att }); break;
            case 4: for (int r = 0; r < N; ++r) pcs.push_back ({ a, len * (double) (N - r) / N, false, 1 }); break;
            case 5: for (int r = 0; r < N; ++r) pcs.push_back ({ a + len * r / N, len * (double) (N - r) / N, false, 1 }); break;
            case 6:
            {
                const int w = clamp (p.i ("which"), 1, N) - 1; const double part = len / N;
                for (int r = 0; r < N; ++r) pcs.push_back ({ a + part * w, part * (1 - rnd * cx.rand() * 0.5), false, 1 });
                break;
            }
            case 7: { double l = len; for (int r = 0; r < N && l > 32; ++r, l *= ratio) pcs.push_back ({ a, l * (1 - rnd * cx.rand() * 0.5), false, 1 }); break; }
            case 8: { double l = len; for (int r = 0; r < N && l > 32; ++r, l *= ratio) pcs.push_back ({ a + len - l, l, false, 1 }); break; }
            default: pcs.push_back ({ a, len, false, 1 }); break;
        }
    }
    return joinPieces (ch, cx, pcs, ms (p, "splice", cx.sr));
}

// Segments re-assembled in order of rising or falling formant (F1, F2, F3), brightness, or summed formants, measured
// internally. after CDP fturanal synth 1-10 (standnew/fturanal.c ftursynth), the feature file replaced by analysis
Chans fturSynth (const Chans& ch, Ctx& cx, const Params& p)
{
    auto c = segCuts (ch, cx, p.i ("cut"), p["wsize"] / 1000); const Buf m = monoOf (ch);
    const int mode = p.i ("mode"); const size_t S = c.size() - 1;
    std::vector<double> key (S, 0.0);
    for (size_t s = 0; s < S; ++s)
    {
        const long long a = c[s], len = c[s + 1] - c[s];
        size_t N = 256; while ((long long) N * 2 <= len && N < 4096) N <<= 1;
        const DVec w = hann (N); DVec re (N), im (N);
        analyze (m, a + len / 2 - (long long) N / 2, w, re, im);
        const size_t half = N / 2; DVec mag (half + 1), env (half + 1); double sum = 0, cen = 0;
        for (size_t k = 0; k <= half; ++k) { mag[k] = std::hypot (re[k], im[k]); sum += mag[k]; cen += mag[k] * (double) k; }
        const double bin = cx.sr / (double) N; envelope (mag, 120 / bin, env);
        auto fmt = [&] (double lo, double hi)
        {
            size_t best = 0; double bv = -1;
            for (size_t k = (size_t) (lo / bin); k <= std::min (half, (size_t) (hi / bin)); ++k) if (env[k] > bv) { bv = env[k]; best = k; }
            return (double) best * bin;
        };
        const double f1 = fmt (200, 900), f2 = fmt (900, 2500), f3 = fmt (2500, 3800), br = sum > 1e-9 ? cen / sum * bin : 0;
        const double v[5] = { f1, f2, f3, br, f1 + f2 + f3 };
        key[s] = v[mode / 2];
        if (! (s & 15)) cx.prog (0.5 * (double) s / (double) S);
    }
    std::vector<size_t> ord (S); for (size_t i = 0; i < S; ++i) ord[i] = i;
    const bool down = mode % 2 == 1;
    std::stable_sort (ord.begin(), ord.end(), [&] (size_t x, size_t y) { return down ? key[x] > key[y] : key[x] < key[y]; });
    std::vector<Pc> pcs; for (size_t s : ord) pcs.push_back ({ (double) c[s], (double) (c[s + 1] - c[s]), false, 1 });
    return joinPieces (ch, cx, pcs, ms (p, "splice", cx.sr), 0.5, 1);
}

// raised-cosine packet envelope ^ NARROWING, its peak moved by CENTRING (-1 start .. 1 end)
inline double packetEnv (double u, double narrow, double ctr)
{
    const double pk = clampd (0.5 * (1 + ctr), 0.02, 0.98);
    const double v = u < pk ? 0.5 * u / pk : 0.5 + 0.5 * (u - pk) / (1 - pk);
    return std::pow (std::max (0.0, 0.5 - 0.5 * std::cos (TAU * v)), narrow);
}
// COUNT wave-packets taken between START and END, either FOUND between signal minima (at least DUR long) or FORCED to
// DUR, shaped by a NARROWING, CENTRING envelope (FORCE pushes every wave peak to the contour), joined with GAPs.
// after CDP packet packet 1-2 (new/packet.c packet), the times file replaced by COUNT evenly spaced times
Chans packet (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const long long n = (long long) m.size(); const int N = std::max (1, p.i ("count"));
    const long long D = std::max (8LL, std::min (n, jr (ms (p, "dur", cx.sr)))), gap = jr (ms (p, "gap", cx.sr));
    const DVec e = peakEnv (m, std::max (8LL, jr (0.002 * cx.sr))); const long long EW = std::max (8LL, jr (0.002 * cx.sr));
    const int level = p.i ("level");
    Out o (ch.size(), cx.maxLen); long long cur = 0;
    for (int k = 0; k < N; ++k)
    {
        const double fr = N == 1 ? p["start"] : p["start"] + (p["end"] - p["start"]) * k / (N - 1);
        long long t = clamp (jr (fr * (double) n), 0LL, n - 1), a = std::max (0LL, t - D / 2), b = std::min (n, a + D);
        if (p.i ("mode") == 0 && ! e.empty())
        {   // edges at envelope minima beyond half the minimum duration on each side
            auto minFrom = [&] (long long from, int dir)
            {
                long long i = clamp (from / EW, 0LL, (long long) e.size() - 1), lim = std::max (1LL, 2 * D / EW);
                for (long long s = 0; s < lim; ++s)
                {
                    long long j = i + dir; if (j < 0 || j >= (long long) e.size()) break;
                    if (e[(size_t) j] > e[(size_t) i]) break;
                    i = j;
                }
                return i * EW + EW / 2;
            };
            a = clamp (minFrom (t - D / 2, -1), 0LL, std::max (0LL, n - 8)); b = clamp (minFrom (t + D / 2, 1), std::min (a + 8, n), n);
        }
        const long long len = b - a; if (len < 8) continue;
        // per-half-cycle peaks for FORCE
        std::vector<double> hc ((size_t) len, 1.0);
        if (level == 2)
        {
            long long s0 = 0;
            for (long long i = 1; i <= len; ++i)
                if (i == len || (m[(size_t) (a + i - 1)] < 0) != (m[(size_t) (a + i)] < 0))
                {
                    double pk = 0; for (long long q = s0; q < i; ++q) pk = std::max (pk, (double) std::abs (m[(size_t) (a + q)]));
                    for (long long q = s0; q < i; ++q) hc[(size_t) q] = pk > 1e-6 ? 1 / pk : 0;
                    s0 = i;
                }
        }
        double norm = 1;
        if (level == 1) { double pk = 0; for (long long i = a; i < b; ++i) pk = std::max (pk, (double) std::abs (m[(size_t) i])); norm = pk > 1e-9 ? 0.9 / pk : 1; }
        for (long long i = 0; i < len; ++i)
        {
            const double env = packetEnv ((double) i / (double) len, p["narrow"], p["centre"]) * norm * hc[(size_t) i] * (level == 2 ? 0.9 : 1);
            for (size_t c = 0; c < ch.size(); ++c) o.add (c, cur + i, ch[c][(size_t) (a + i)] * env);
        }
        cur += len + gap; o.touch (cur);
        cx.prog ((double) (k + 1) / N);
    }
    return o.take();
}

// Fast-forward 'DVD wind': read a CLIP, skip ahead, read the next clip... so the sound plays CONTRACTION times faster
// at its own pitch. after CDP dvdwind (science/dvdwind.c)
Chans dvdwind (const Chans& ch, Ctx& cx, const Params& p)
{
    const long long n = (long long) ch[0].size(), clip = std::max (16LL, jr (ms (p, "clip", cx.sr)));
    const double step = (double) clip * std::max (1.0, p["contract"]); const double f = std::max (1.0, std::min (0.003 * cx.sr, clip / 4.0));
    Out o (ch.size(), cx.maxLen); long long cur = 0; long long k = 0;
    for (double a = 0; a < (double) n; a += step, ++k)
    {
        const long long len = std::min (clip, n - (long long) a);
        putSeg (o, ch, a, (double) len, cur, len, 1, f, 1); cur += len; o.touch (cur);
        if (! (k & 63)) cx.prog (a / (double) n);
    }
    return o.take();
}

// Elements (about SIZE long, or single wavesets when SIZE is 0) re-ordered into a crescendo, decrescendo, accelerando
// (longest first), ritardando or random order; OUT PITCH sets their onsets at a regular pitched rate.
// after CDP sorter sorter 1-5 (science/sorter.c do_sorter)
Chans sorter (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const long long n = (long long) m.size(); const double size = p["size"];
    std::vector<long long> c;
    if (size <= 0.0005) { auto ws = wsets (ch, 1); c.push_back (0); for (auto& w : ws) if (w.first > 0) c.push_back ((long long) w.first); c.push_back (n); }
    else
    {   // cuts at the upward zero crossing nearest to each multiple of SIZE (so lengths vary a little)
        const long long L = std::max (16LL, jr (size * cx.sr)); c.push_back (0);
        for (long long t = L; t < n - 8; t += L)
        {
            long long best = t, bd = L;
            for (long long i = std::max (1LL, t - L / 3); i < std::min (n, t + L / 3); ++i)
                if (m[(size_t) i - 1] < 0 && m[(size_t) i] >= 0 && std::abs (i - t) < bd) { bd = std::abs (i - t); best = i; }
            if (best > c.back() + 8) c.push_back (best);
        }
        c.push_back (n);
    }
    const size_t S = c.size() - 1; std::vector<double> loud (S), len (S);
    for (size_t s = 0; s < S; ++s) { double e = 0; for (long long i = c[s]; i < c[s + 1]; ++i) e += (double) m[(size_t) i] * m[(size_t) i]; len[s] = (double) (c[s + 1] - c[s]); loud[s] = std::sqrt (e / std::max (1.0, len[s])); }
    std::vector<size_t> ord (S); for (size_t i = 0; i < S; ++i) ord[i] = i;
    const int mode = p.i ("mode"); cx.seed (p["seed"]);
    if (mode == 4) { auto pr = perm ((int) S, cx); for (size_t i = 0; i < S; ++i) ord[i] = (size_t) pr[i]; }
    else std::stable_sort (ord.begin(), ord.end(), [&] (size_t x, size_t y)
    {
        switch (mode) { case 0: return loud[x] < loud[y]; case 1: return loud[x] > loud[y]; case 2: return len[x] > len[y]; default: return len[x] < len[y]; }
    });
    const double f = size <= 0.0005 ? 1.0 : std::max (1.0, ms (p, "smooth", cx.sr));
    const int opch = p.i ("opch");
    if (opch <= 0)
    {
        std::vector<Pc> pcs; for (size_t s : ord) pcs.push_back ({ (double) c[s], len[s], false, 1 });
        return joinPieces (ch, cx, pcs, size <= 0.0005 ? 0 : f);
    }
    const double per = cx.sr / (440.0 * std::pow (2.0, (opch - 69) / 12.0));
    Out o (ch.size(), cx.maxLen); double at = 0;
    for (size_t k = 0; k < S && at < (double) cx.maxLen; ++k, at += per)
    {
        putSeg (o, ch, (double) c[ord[k]], len[ord[k]], jr (at), jr (len[ord[k]]), 1, f, 1);
        o.touch (jr (at + len[ord[k]]));
        if (! (k & 63)) cx.prog ((double) k / (double) S);
    }
    return o.take();
}

// The source is sliced into elements (ONSETS, ENVELOPE troughs or equal slices); segments cut from element starts
// (optionally from up to JOINS elements joined) play in random (or PERMUTEd) order, with random SILENCEs between them,
// random TRANSposition and ATTENuation, sizes BIASed short or long. after CDP stutter (science/stutter.c)
Chans stutter (const Chans& ch, Ctx& cx, const Params& p)
{
    const long long n = (long long) ch[0].size(); std::vector<long long> c;
    switch (p.i ("cut"))
    {
        case 0: for (auto& e : gateEvents (ch, -20, 0.01, cx.sr)) c.push_back (e.s); c[0] = 0; c.push_back (n); break;
        case 1: c = troughCuts (monoOf (ch), cx.sr, 0.03, 0.8); break;
        case 2: c = equalCuts (n, std::max (16LL, n / 8)); break;
        default: c = equalCuts (n, std::max (16LL, n / 16)); break;
    }
    if (c.size() < 2) c = { 0, n };
    const int E = (int) c.size() - 1, J = std::max (1, p.i ("joins")); cx.seed (p["seed"]);
    const long long D = jr (p["dur"] * cx.sr), minD = std::max (8LL, jr (ms (p, "mindur", cx.sr)));
    const double bias = p["bias"], fd = 0.003 * cx.sr;
    Out o (ch.size(), (size_t) std::max (1LL, std::min ((long long) cx.maxLen, D))); long long cur = 0; std::vector<int> order; size_t oi = 0;
    while (cur < D && cur < (long long) cx.maxLen)
    {
        int el;
        if (p.i ("perm")) { if (oi >= order.size()) { order = perm (E, cx); oi = 0; } el = order[oi++]; }
        else el = std::min (E - 1, (int) (cx.rand() * E));
        const int j = std::min (E - el, 1 + (int) (cx.rand() * J));
        const long long a = c[(size_t) el], b = c[(size_t) (el + j)];
        const long long lo = j > 1 ? c[(size_t) (el + j - 1)] - a + 1 : std::min (minD, b - a);
        double u = std::pow (cx.rand(), std::pow (2.0, -2 * bias));   // bias > 0: longer cuts
        long long len = std::max (std::min (lo, b - a), lo + jr (u * (double) (b - a - lo)));
        len = std::max (1LL, std::min (len, b - a));
        const double tr = st2r ((cx.rand() * 2 - 1) * p["trans"]), g = 1 - cx.rand() * p["atten"];
        const long long dur = std::max (1LL, jr ((double) len / tr));
        putSeg (o, ch, (double) a, (double) len, cur, dur, tr, fd, g);
        cur += dur;
        if (cx.rand() < p["silprop"]) cur += jr ((p["silmin"] + cx.rand() * std::max (0.0, p["silmax"] - p["silmin"])) * cx.sr);
        o.touch (std::min (cur, D));
        cx.prog ((double) cur / (double) std::max (1LL, D));
    }
    return o.take();
}

// A fast inner pulse stream (FREQ, on-time FRATIO) inside slower outer pulses (PULSE, on-time PRATIO, peak at SYM,
// shape BITE): inner events are cut successively from the source as the outer envelope rises and in reverse order as
// it falls (or only ADVANCE, or EITHER way), with pitch JITTER and level TREMOR. The source is the WHOLE sound or its
// onset SLICES in turn. after CDP motor motor 1-9 (science/motor.c), slice file replaced by onset detection
Chans motor (const Chans& ch, Ctx& cx, const Params& p)
{
    const long long n = (long long) ch[0].size(); cx.seed (p["seed"]);
    std::vector<long long> c;
    if (p.i ("source") == 1) { for (auto& e : gateEvents (ch, -20, 0.01, cx.sr)) c.push_back (e.s); c[0] = 0; }
    else c.push_back (0);
    c.push_back (n);
    const long long D = std::min ((long long) cx.maxLen, jr (p["dur"] * cx.sr));
    const double P = cx.sr / p["pulse"], I = cx.sr / p["freq"], on = p["pratio"] * P;
    const int K = std::max (2, (int) std::floor (on / I)), kp = clamp ((int) jr (p["sym"] * (K - 1)), 0, K - 1);
    const long long L = std::max (8LL, jr (p["fratio"] * I)); const int mode = p.i ("mode");
    Out o (ch.size(), (size_t) std::max (1LL, D)); size_t slice = 0; long long q = 0;
    for (double t0 = 0; t0 < (double) D; t0 += P, ++q)
    {
        const long long sa = c[slice % (c.size() - 1)], sb = c[slice % (c.size() - 1) + 1]; ++slice;
        const double span = (double) std::max (0LL, sb - sa - L);
        const bool regress = mode == 2 && cx.rand() < 0.5;
        for (int k = 0; k < K; ++k)
        {
            double env;
            if (k <= kp) env = std::pow ((double) (k + 1) / (double) (kp + 1), p["bite"]);
            else env = std::pow (1 - (double) (k - kp) / (double) (K - kp), p["bite"]);
            double fr;   // read position as a fraction of the source
            if (mode == 0) fr = k <= kp ? (double) k / std::max (1, kp) : (double) (K - 1 - k) / std::max (1, K - 1 - kp) * (kp ? 1.0 : 0.0);
            else fr = (double) k / (double) (K - 1);
            if (regress) fr = 1 - fr;
            const double a = (double) sa + clampd (fr, 0, 1) * span, g = env * (1 - cx.rand() * p["tremor"]);
            const double tr = st2r ((cx.rand() * 2 - 1) * p["jitter"]);
            putSeg (o, ch, a, (double) (sb - (long long) a), jr (t0 + k * I), L, tr, std::min (0.002 * cx.sr, L / 4.0), g);
        }
        o.touch (jr (t0 + P));
        if (! (q & 7)) cx.prog (t0 / (double) D);
    }
    return o.take();
}
// ---------------------------------------------------------------------------------------------
// Event textures: fracture, newtex, brownian, crystal, pulser, rotor (multichannel CDP programs folded to stereo)
// ---------------------------------------------------------------------------------------------

// a short single-cycle-ish wavetable taken at the loudest point of the source (CDP brownian / pulser read the source as
// a waveform; a whole long sample read once per cycle would only be noise, so a few wave-cycles are used)
Buf makeTable (const Buf& m, double sr)
{
    const long long n = (long long) m.size(); if (n < 4) return Buf (64, 0.0f);
    long long pk = 0; for (long long i = 0; i < n; ++i) if (std::abs (m[(size_t) i]) > std::abs (m[(size_t) pk])) pk = i;
    const long long want = std::max (16LL, jr (0.01 * sr));
    long long a = -1, b = -1;
    for (long long i = std::max (1LL, pk - want * 2); i < std::min (n, pk + want * 2); ++i)
        if (m[(size_t) i - 1] < 0 && m[(size_t) i] >= 0) { if (a < 0) a = i; else if (i - a >= want) { b = i; break; } }
    if (a < 0 || b < 0) { a = std::max (0LL, pk - want / 2); b = std::min (n, a + want); }
    Buf t (m.begin() + a, m.begin() + b);
    {   // a lopsided cycle (a decaying kick) would put a DC offset under every event read from the table
        double mean = 0; for (float v : t) mean += v; mean /= (double) std::max<size_t> (1, t.size());
        for (auto& v : t) v = (float) (v - mean);
    }
    double p = peak (t);
    if (p > 1e-9) for (auto& v : t) v = (float) (v / p);
    if (t.size() < 4) t.assign (64, 0.0f);
    return t;
}
// MIDI pitch of the source (median of the tracked f0 around its loudest second), 60 when nothing is pitched
double srcMidi (const Buf& m, double sr, Ctx& cx)
{
    const long long n = (long long) m.size(); if (n < 64) return 60;
    long long pk = 0; for (long long i = 0; i < n; ++i) if (std::abs (m[(size_t) i]) > std::abs (m[(size_t) pk])) pk = i;
    const long long a = std::max (0LL, pk - jr (0.1 * sr)), b = std::min (n, a + jr (0.6 * sr));
    Buf seg (m.begin() + a, m.begin() + b);
    auto pt = trackPitch (seg, sr, 80, 1200, 0.15);
    std::vector<double> f; for (double v : pt.f0) if (v > 0) f.push_back (v);
    (void) cx;
    if (f.size() < 3) return 60;
    std::nth_element (f.begin(), f.begin() + (long) (f.size() / 2), f.end());
    return 69 + 12 * std::log2 (f[f.size() / 2] / 440);
}
inline double hz (double midi) { return 440.0 * std::pow (2.0, (midi - 69) / 12); }

// fragment envelope shapes (CDP fracture envelope table)
inline double fracShape (int s, double u)
{
    switch (s)
    {
        case 1: return u < 0.08 ? u / 0.08 : std::pow ((1 - u) / 0.92, 1.5);
        case 2: return u < 0.85 ? std::pow (u / 0.85, 2) : (1 - u) / 0.15;
        case 3: return std::abs (std::sin (TAU * u));
        default: return 0.5 - 0.5 * std::cos (TAU * u);
    }
}
// The sound is dispersed into enveloped fragments over STREAMS positions across the stereo field, a set every PULSE,
// with read, pulse and stream-timing (DISP) randomisation, random level and TRANSposition; DEPTH > 1 stacks transposed
// copies (STACK INT). RECEDING moves the image away (narrower, quieter, duller). after CDP fracture 1-2 (new/fracture.c)
Chans fracture (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const long long n = (long long) m.size(); cx.seed (p["seed"]);
    const int S = std::max (2, p.i ("streams")), shape = p.i ("env"); const bool recede = p.i ("mode") == 1;
    const double P = std::max (0.01, p["pulse"]) * cx.sr, d = p["depth"];
    const long long outLen = std::min ((long long) cx.maxLen, n + jr (2 * P));
    Out o (2, (size_t) std::max (1LL, outLen)); Buf frag;
    long long cnt = 0;
    for (double t = 0; t < (double) n; t += P * (1 + p["prnd"] * (cx.rand() - 0.5)), ++cnt)
    {
        const double dist = recede ? t / std::max (1.0, (double) n) : 0;
        for (int s = 0; s < S; ++s)
        {
            double pan = (double) s / (S - 1); pan = 0.5 + (pan - 0.5) * (1 - 0.8 * dist);
            const double ot = t + p["disp"] * (cx.rand() - 0.5) * P, rd = clampd (t + p["rrnd"] * (cx.rand() - 0.5) * P, 0, (double) n - 1);
            const long long L = std::max (16LL, jr (P)); const double tr = st2r ((cx.rand() * 2 - 1) * p["trnd"]);
            const double lev = (1 - p["lrnd"] * cx.rand()) * (1 / (1 + 3 * dist)) / std::sqrt ((double) S);
            frag.assign ((size_t) L, 0.0f);
            const int stacks = d > 1 ? (int) std::ceil (d - 1 - 1e-9) : 0;
            for (int j = 0; j <= stacks; ++j)
            {
                double sl = j == 0 ? 1.0 : clampd (d - 1 - (j - 1), 0, 1) * (1 - p["srnd"] * cx.rand());
                double r = tr * (j ? st2r (p["stkint"] * j) : 1.0);
                for (long long i = 0; i < L; ++i)
                {
                    double sp = rd + (double) (i - L / 2) * r + (double) (L / 2) * tr;   // stacked copies synced at the centre
                    if (sp < 0 || sp >= (double) n) continue;
                    frag[(size_t) i] = (float) (frag[(size_t) i] + lerpAt (m, sp) * sl);
                }
            }
            OnePole lp; const double fc = recede ? 16000 * std::pow (300 / 16000.0, dist) : 20000;
            auto [gl, gr] = panG (clampd (pan, 0, 1));
            for (long long i = 0; i < L; ++i)
            {
                const double sh = fracShape (shape, (double) i / (double) L), e = 1 - std::min (1.0, d) * (1 - sh);
                double v = frag[(size_t) i] * e * lev; if (recede) v = lp.lp (v, fc, cx.sr);
                const long long op = jr (ot) + i; o.add (0, op, v * gl); o.add (1, op, v * gr);
            }
        }
        if (! (cnt & 7)) cx.prog (t / (double) n);
    }
    o.touch (outLen);
    return o.take();
}

const std::vector<std::vector<double>> TEXCHORDS { { 0 }, { 0, 7 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 2, 4, 7, 9 }, { 0, 5, 10 }, { 0, 4, 7, 11 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };
// A texture of looping components of the source that fade in and out every STEP (NUMBER at a time, spread in space):
// TRANSPOSED copies over RANGE octaves of a CHORD, copies at the ORIGINAL rate offset by DELAY, or DRUNKen-walk reads
// around LOCUS within AMBITUS. after CDP newtex 1-3 (new/newtex.c), spatialisation folded to stereo
Chans newtex (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const double n = (double) m.size(); cx.seed (p["seed"]);
    const int mode = p.i ("mode"), range = std::max (1, p.i ("range")), num = std::max (1, p.i ("number")), space = p.i ("space");
    std::vector<double> comp;   // per component: transposition (semitones) in mode 0, delay index otherwise
    if (mode == 0) { const auto& cd = TEXCHORDS[(size_t) clamp (p.i ("chord"), 0, (int) TEXCHORDS.size() - 1)]; for (int o = 0; o < range; ++o) for (double iv : cd) comp.push_back (iv + 12.0 * o - 6.0 * (range - 1)); }
    else for (int c = 0; c < std::max (range, num); ++c) comp.push_back (c);
    const long long D = std::min ((long long) cx.maxLen, jr (p["dur"] * cx.sr)), spl0 = std::max (8LL, jr (ms (p, "splice", cx.sr)));
    Out o (2, (size_t) std::max (1LL, D));
    std::vector<double> rpos (comp.size(), 0.0); for (auto& r : rpos) r = cx.rand() * n;
    double walk = p["loc"] * n; std::vector<int> prev; int ev = 0;
    for (double t = 0; t < (double) D; ++ev)
    {
        const double len = std::max (0.02, p["step"] * (0.5 + cx.rand())) * cx.sr; const long long spl = std::min (spl0, jr (len));
        std::vector<int> pick; auto pr = perm ((int) comp.size(), cx);
        for (int q : pr) { if ((int) pick.size() >= num) break; if (std::find (prev.begin(), prev.end(), q) == prev.end() || comp.size() <= (size_t) num) pick.push_back (q); }
        for (int q : pr) { if ((int) pick.size() >= num) break; if (std::find (pick.begin(), pick.end(), q) == pick.end()) pick.push_back (q); }
        for (size_t k = 0; k < pick.size(); ++k)
        {
            const int c = pick[k]; double ratio = 1, a;
            if (mode == 0) { ratio = st2r (comp[(size_t) c]); a = rpos[(size_t) c]; rpos[(size_t) c] = std::fmod (a + len * ratio, std::max (1.0, n)); }
            else if (mode == 1) { a = std::fmod (t + (double) c * p["delay"] * cx.sr, std::max (1.0, n)); }
            else
            {
                const double amb = p["amb"] * cx.sr; walk += (cx.rand() * 2 - 1) * amb * 0.5;
                walk = clampd (walk, p["loc"] * n - amb, p["loc"] * n + amb); a = clampd (walk, 0, std::max (0.0, n - 1));
            }
            double pan;
            switch (space)
            {
                case 1: pan = (ev % 2 ? 0.5 : 0.0) + cx.rand() * 0.5; break;
                case 2: pan = 0.5 + 0.45 * std::sin (TAU * (t / cx.sr * 0.25 + (double) k / (double) pick.size())); break;
                case 3: pan = ev % 2 ? 0.9 : 0.1; break;
                default: pan = cx.rand(); break;
            }
            const long long L = jr (len) + spl;
            // looping read: wrap the source for long components
            long long done = 0;
            while (done < L)
            {
                const double avail = n - a; if (avail < 2) { a = 0; if (n < 2) break; continue; }
                const long long seg = std::min (L - done, (long long) std::floor (avail / ratio));
                if (seg <= 0) { a = 0; continue; }
                for (long long i = 0; i < seg; ++i)
                {
                    const long long j = done + i; const double e = std::min ({ 1.0, (double) (j + 1) / (double) spl, (double) (L - j) / (double) spl });
                    const double v = lerpAt (m, a + (double) i * ratio) * e / std::sqrt ((double) num);
                    auto [gl, gr] = panG (pan); o.add (0, jr (t) + j, v * gl); o.add (1, jr (t) + j, v * gr);
                }
                done += seg; a = 0;
            }
        }
        prev = pick; t += len;
        cx.prog (t / (double) D);
    }
    o.touch (D);
    return o.take();
}

// Events (the source read as a WAVEFORM at the event pitch with an attack/decay envelope, or the WHOLE source
// transposed) follow brownian walks in pitch (PITCH LO-HI, max STEP), stereo position (SPACE STEP) and level (AMP STEP),
// one every TICK on average. after CDP brownian motion 1-2 (science/brownian.c do_brownian), folded to stereo
Chans brownian (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const double n = (double) m.size(); cx.seed (p["seed"]);
    const bool wave = p.i ("mode") == 0; const Buf tab = makeTable (m, cx.sr);
    const double lo = std::min (p["plo"], p["phi"]), hi = std::max (p["plo"], p["phi"]);
    const long long D = std::min ((long long) cx.maxLen, jr (p["dur"] * cx.sr));
    Out o (2, (size_t) std::max (1LL, D));
    double pitch = 0.5 * (lo + hi), sp = 0.5, db = 0; long long k = 0;
    const long long cap = evCap ((double) D / (std::max (0.005, p["tick"] * 0.5) * cx.sr), cx.sr);
    const double base = wave ? 0 : srcMidi (m, cx.sr, cx);
    auto reflect = [] (double v, double a, double b) { if (b <= a) return a; for (int i = 0; i < 8 && (v < a || v > b); ++i) v = v < a ? 2 * a - v : 2 * b - v; return clampd (v, a, b); };
    for (double t = 0; t < (double) D; t += std::max (0.005, p["tick"] * (0.5 + cx.rand())) * cx.sr, ++k)
    {
        auto [gl, gr] = panG (sp); const double g = std::pow (10.0, db / 20) * 0.5;
        if (wave)
        {
            const long long A = std::min (cap / 4, std::max (1LL, jr (ms (p, "att", cx.sr)))), De = std::min (cap, std::max (1LL, jr (ms (p, "dec", cx.sr))));
            const double inc = hz (pitch) * (double) tab.size() / cx.sr; double ph = 0;
            for (long long i = 0; i < A + De; ++i)
            {
                const double e = i < A ? (double) i / (double) A : std::pow (1 - (double) (i - A) / (double) De, 2);
                const double v = lerpAt (tab, ph) * e * g; ph += inc; while (ph >= (double) tab.size()) ph -= (double) tab.size();
                o.add (0, jr (t) + i, v * gl); o.add (1, jr (t) + i, v * gr);
            }
        }
        else
        {
            const double r = st2r (pitch - base); const long long L = std::min ({ (long long) (n / r), D - jr (t), cap });
            const double f = std::min (0.005 * cx.sr, L / 4.0);
            for (long long i = 0; i < L; ++i)
            {
                const double e = std::min ({ 1.0, (double) (i + 1) / f, (double) (L - i) / f }), v = lerpAt (m, (double) i * r) * e * g;
                o.add (0, jr (t) + i, v * gl); o.add (1, jr (t) + i, v * gr);
            }
        }
        pitch = reflect (pitch + (cx.rand() * 2 - 1) * p["step"], lo, hi);
        sp = reflect (sp + (cx.rand() * 2 - 1) * p["sstep"], 0, 1);
        db = reflect (db + (cx.rand() * 2 - 1) * p["arange"], -36, 0);
        if (! (k & 7)) cx.prog (t / (double) D);
    }
    o.touch (D);
    return o.take();
}

// polyhedra vertices (CDP crystal vertex data replaced by presets)
std::vector<std::array<double, 3>> polyVerts (int s)
{
    std::vector<std::array<double, 3>> v; const double g = (1 + std::sqrt (5.0)) / 2;
    switch (s)
    {
        case 0: v = { { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 } }; break;
        case 1: for (int i = 0; i < 8; ++i) v.push_back ({ i & 1 ? 1.0 : -1.0, i & 2 ? 1.0 : -1.0, i & 4 ? 1.0 : -1.0 }); break;
        case 2: v = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } }; break;
        case 3: for (int a = -1; a <= 1; a += 2) for (int b = -1; b <= 1; b += 2) { v.push_back ({ 0, (double) a, b * g }); v.push_back ({ (double) a, b * g, 0 }); v.push_back ({ b * g, 0, (double) a }); } break;
        case 4: for (int i = 0; i < 6; ++i) for (int z = -1; z <= 1; z += 2) v.push_back ({ std::cos (TAU * i / 6), std::sin (TAU * i / 6), 0.8 * z }); break;
        default: for (int i = 0; i < 12; ++i) v.push_back ({ std::cos (TAU * i / 12), std::sin (TAU * i / 12), std::sin (TAU * i / 4) * 0.5 }); break;
    }
    for (auto& q : v) { double r = std::sqrt (q[0] * q[0] + q[1] * q[1] + q[2] * q[2]); if (r > 0) for (auto& c : q) c = c / r * 0.9; }
    return v;
}
// Events from the vertices of a crystal rotating in 3-D (ROT XY, ROT XZ revs/s), sampled every TSTEP: x gives the time
// within TWIDTH and the stereo position, y the pitch (PITCH LO-HI), z the brightness (far: lowpassed below PASS, close:
// an octave stacked on top). after CDP crystal rotate 1-9 (science/crystal.c crystal_rotate), channel pairs folded to stereo
Chans crystal (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const double n = (double) m.size(); cx.seed (1);
    const auto V = polyVerts (p.i ("shape")); const int space = p.i ("mode");
    const double base = srcMidi (m, cx.sr, cx), lo = std::min (p["plo"], p["phi"]), hi = std::max (p["plo"], p["phi"]);
    const long long D = std::min ((long long) cx.maxLen, jr (p["dur"] * cx.sr)); long long E = std::max (16LL, jr (p["evdur"] * cx.sr));
    E = std::min (E, evCap ((double) D / (std::max (0.01, p["tstep"]) * cx.sr) * (double) V.size(), cx.sr));
    Out o (space == 0 ? 1 : 2, (size_t) std::max (1LL, D)); const double gain = 1 / std::sqrt ((double) V.size());
    long long g = 0;
    for (double t = 0; t < (double) D; t += std::max (0.01, p["tstep"]) * cx.sr, ++g)
    {
        const double ts = t / cx.sr, a = TAU * p["rota"] * ts, b = TAU * p["rotb"] * ts;
        const double centre = space == 2 ? t / (double) D : space == 3 ? 1 - t / (double) D : space == 4 ? cx.rand() : 0.5;
        for (auto v : V)
        {
            double x = v[0] * std::cos (a) - v[1] * std::sin (a), y = v[0] * std::sin (a) + v[1] * std::cos (a), z = v[2];
            double x2 = x * std::cos (b) - z * std::sin (b); z = x * std::sin (b) + z * std::cos (b); x = x2;
            const long long at = jr (t + (x + 1) / 2 * p["twidth"] * cx.sr);
            const double r = st2r (lo + (y + 1) / 2 * (hi - lo) - base), far = clampd (-z, 0, 1), close = clampd (z, 0, 1);
            double pan = space == 1 ? (x + 1) / 2 : centre + x * 0.25;
            auto [gl, gr] = panG (clampd (pan, 0, 1)); OnePole lp;
            const long long L = std::min (E, (long long) (n / r));
            for (long long i = 0; i < L; ++i)
            {
                const double u = (double) i / (double) L, e = u < 0.05 ? u / 0.05 : std::pow ((1 - u) / 0.95, 2);
                double s = lerpAt (m, (double) i * r); const double f = lp.lp (s, p["pass"], cx.sr);
                s = s * (1 - far) + f * far + close * lerpAt (m, (double) i * r * 2);
                s *= e * gain;
                if (space == 0) o.add (0, at + i, s); else { o.add (0, at + i, s * gl); o.add (1, at + i, s * gr); }
            }
        }
        if (! (g & 3)) cx.prog (t / (double) D);
    }
    o.touch (D);
    return o.take();
}

// packet envelope: rise (curve EXP R), sustain, decay
inline double pulseEnv (long long i, long long R, long long S, long long Dc)
{
    if (i < R) return (double) i / (double) R;
    if (i < R + S) return 1;
    return std::pow (std::max (0.0, 1 - (double) (i - R - S) / (double) Dc), 2);
}
// A stream of pitched wave-packets (PITCH) that take only the brightness of the source (read as a waveform), each with
// a RISE, SUSTAIN and DECAY (randomised by ENV RAND), one every SPEED (SCATTER), with pitch scatter, a lower OCTAVE and
// an upward BEND. after CDP pulser pulser 1 (science/pulser.c)
Chans pulser1 (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf tab = makeTable (monoOf (ch), cx.sr); cx.seed (p["seed"]);
    const long long D = std::min ((long long) cx.maxLen, jr (p["dur"] * cx.sr)); const double T = (double) tab.size();
    Out o (1, (size_t) std::max (1LL, D)); long long k = 0;
    auto rv = [&] (double v) { return v * (1 - p["erand"] * cx.rand()); };
    const long long cap = evCap ((double) D / (std::max (0.01, p["speed"] * 0.5) * cx.sr), cx.sr);
    for (double t = 0; t < (double) D; t += std::max (0.01, p["speed"] * (1 + p["scatter"] * (cx.rand() - 0.5))) * cx.sr, ++k)
    {
        const long long R = std::max (1LL, jr (rv (p["rise"]) * cx.sr)), S = jr (rv (p["sus"]) * cx.sr), Dc = std::min (cap, std::max (1LL, jr (rv (p["decay"]) * cx.sr))), L = R + S + Dc;
        const double f0 = hz (p["pitch"]) * st2r ((cx.rand() * 2 - 1) * p["pscat"]), f1 = f0 * st2r (p["bend"]);
        double ph = 0, ph2 = 0;
        for (long long i = 0; i < L; ++i)
        {
            const double f = f0 + (f1 - f0) * (double) i / (double) L;
            const double v = (lerpAt (tab, ph) + p["octav"] * lerpAt (tab, ph2)) / (1 + p["octav"]) * pulseEnv (i, R, S, Dc) * 0.7;
            ph += f * T / cx.sr; ph2 += 0.5 * f * T / cx.sr; while (ph >= T) ph -= T; while (ph2 >= T) ph2 -= T;
            o.add (0, jr (t) + i, v);
        }
        if (! (k & 15)) cx.prog (t / (double) D);
    }
    o.touch (D);
    return o.take();
}
// A stream of packets cut from the START of the source (or from RANDOM points), enveloped (RISE, SUSTAIN, DECAY,
// ENV RAND), one every SPEED with SCATTER, pitch scatter and BEND, spread over a stereo WIDTH. after CDP pulser pulser 2-3
Chans pulser2 (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const double n = (double) m.size(); cx.seed (p["seed"]);
    const long long D = std::min ((long long) cx.maxLen, jr (p["dur"] * cx.sr)); const bool rnd = p.i ("mode") == 1;
    Out o (2, (size_t) std::max (1LL, D)); long long k = 0; Buf pk;
    auto rv = [&] (double v) { return v * (1 - p["erand"] * cx.rand()); };
    const long long cap = evCap ((double) D / (std::max (0.01, p["speed"] * 0.5) * cx.sr), cx.sr);
    for (double t = 0; t < (double) D; t += std::max (0.01, p["speed"] * (1 + p["scatter"] * (cx.rand() - 0.5))) * cx.sr, ++k)
    {
        const long long R = std::max (1LL, jr (rv (p["rise"]) * cx.sr)), S = jr (rv (p["sus"]) * cx.sr), Dc = std::min (cap, std::max (1LL, jr (rv (p["decay"]) * cx.sr))), L = R + S + Dc;
        const double r0 = st2r ((cx.rand() * 2 - 1) * p["pscat"]), r1 = r0 * st2r (p["bend"]);
        const double a = rnd ? cx.rand() * std::max (0.0, n - (double) L * r1) : 0;
        pk.assign ((size_t) L, 0.0f); double pos = a, mx = 0;
        for (long long i = 0; i < L; ++i)
        {
            const double v = lerpAt (m, pos) * pulseEnv (i, R, S, Dc); pk[(size_t) i] = (float) v; mx = std::max (mx, std::abs (v));
            pos += r0 + (r1 - r0) * (double) i / (double) L;
        }
        const double g = mx > 1e-6 ? 0.6 / mx : 0, pan = 0.5 + (cx.rand() - 0.5) * p["width"];
        auto [gl, gr] = panG (pan);
        for (long long i = 0; i < L; ++i) { o.add (0, jr (t) + i, pk[(size_t) i] * g * gl); o.add (1, jr (t) + i, pk[(size_t) i] * g * gr); }
        if (! (k & 15)) cx.prog (t / (double) D);
    }
    o.touch (D);
    return o.take();
}
// Note-sets of COUNT events (the source read at different speeds) whose pitch range grows and shrinks over PITCH CYC sets
// and whose speed grows and shrinks over SPEED CYC sets (PHASE apart); sets start every GROUP STEP, straight after the
// previous set, or overlapping its last note; STEREO widens with the pitch range. after CDP rotor rotor 1-3 (science/rotor.c)
Chans rotor (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const double n = (double) m.size(); const double base = srcMidi (m, cx.sr, cx);
    const int cnt = std::max (3, p.i ("cnt")), mode = p.i ("mode"); const bool st = p.i ("stereo") == 1;
    const double lo = std::min (p["minp"], p["maxp"]), hi = std::max (p["minp"], p["maxp"]);
    const long long D = std::min ((long long) cx.maxLen, jr (p["dur"] * cx.sr)); long long E = std::max (16LL, jr (p["evdur"] * cx.sr));
    E = std::min (E, evCap (2 * std::max ((double) D / (std::max (0.01, p["step"] * 0.15) * cx.sr), (double) D / (std::max (0.05, p["gstp"]) * cx.sr) * cnt), cx.sr));
    Out o (st ? 2 : 1, (size_t) std::max (1LL, D)); double setStart = 0;
    for (int j = 0; setStart < (double) D && j < 100000; ++j)
    {
        const double pr = 0.5 - 0.5 * std::cos (TAU * j / std::max (1.0, p["prot"])), tr = 0.5 - 0.5 * std::cos (TAU * (j / std::max (1.0, p["trot"]) + p["phas"]));
        const double stepS = std::max (0.01, p["step"] * (0.15 + 0.85 * tr)) * cx.sr;
        double lastOn = setStart;
        for (int k = 0; k < cnt; ++k)
        {
            const double on = setStart + k * stepS; lastOn = on; if (on >= (double) D) break;
            const double pitch = lo + (hi - lo) * pr * k / (cnt - 1), r = st2r (pitch - base);
            const long long L = std::min (E, (long long) (n / r));
            const double pan = 0.5 + ((double) k / (cnt - 1) - 0.5) * pr; auto [gl, gr] = panG (pan);
            for (long long i = 0; i < L; ++i)
            {
                const double u = (double) i / (double) L, e = (u < 0.02 ? u / 0.02 : std::pow ((1 - u) / 0.98, 1.5)) * 0.5;
                const double v = lerpAt (m, (double) i * r) * e;
                if (st) { o.add (0, jr (on) + i, v * gl); o.add (1, jr (on) + i, v * gr); } else o.add (0, jr (on) + i, v);
            }
        }
        setStart = mode == 0 ? setStart + std::max (0.05, p["gstp"]) * cx.sr : mode == 1 ? lastOn + stepS : lastOn;
        if (setStart <= lastOn - (double) cnt * stepS) setStart = lastOn + 1;
        cx.prog (setStart / (double) D);
    }
    o.touch (D);
    return o.take();
}
} // namespace

void addCdpGrain (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CAT = "CDP \xc2\xb7 GRAIN \xc2\xb7 RHYTHM";
    const ParamDef GATE = P ("gate", "GATE", -60, -3, -20, 0, {}, Fmt::db), MINSIL = P ("minsil", "MIN GAP", 2, 500, 10, PExp, "ms");
    const O STEPMODE { "CYCLE", "EACH" };
    auto stP = [] (int i, double d) { return P (std::string ("p") + (char) ('0' + i), "PITCH " + std::to_string (i), -24, 24, d, 0, "st"); };
    auto stM = [] (int i, double d) { return P (std::string ("m") + (char) ('0' + i), "TIME " + std::to_string (i), 0.1, 4, d, PExp, "x"); };

    add (v, CAT, "cdp_grain_omit", "GRAIN OMIT", "GOMT", "Finds the grains (sounds separated by quiet gaps below GATE lasting MIN GAP) and keeps only KEEP out of every OUT OF of them, closing up the time.",
         { P ("keep", "KEEP", 1, 16, 1, PInt), P ("outof", "OUT OF", 1, 16, 2, PInt), GATE, MINSIL }, grainOmit, {}, false, "grain omit");
    add (v, CAT, "cdp_grain_duplicate", "GRAIN DUPLICATE", "GDUP", "Every grain (sound separated by gaps below GATE) is played REPEATS times before moving on: a grain-aware stutter.",
         { P ("n", "REPEATS", 1, 16, 2, PInt), GATE, MINSIL }, grainDup, {}, false, "grain duplicate");
    add (v, CAT, "cdp_grain_reorder", "GRAIN REORDER", "GROR", "Re-sequences the grains by a CDP jump code: 'BA:C' swaps pairs, 'CBA:D' reverses threes, 'ADB:C' plays grains 1, 4, 2 then restarts from grain 3, and so on.",
         { Opt ("mode", "CODE", { "BA:C", "CBA:D", "DCBA:E", "ACB:D", "ADB:C", "AAB:C", "ABA:B", "AC:B", "BAC:C" }, 4), GATE, MINSIL }, grainReorder, {}, false, "grain reorder");
    add (v, CAT, "cdp_grain_repitch", "GRAIN REPITCH", "GRPT", "Transposes successive grains through a cycle of STEPS pitches (CYCLE), or plays every grain at each pitch in turn (EACH). The rhythm of the onsets is kept.",
         { Opt ("mode", "MODE", STEPMODE), P ("steps", "STEPS", 1, 4, 4, PInt), stP (1, 0), stP (2, 7), stP (3, 12), stP (4, -5), GATE, MINSIL }, grainRepitch, {}, false, "grain repitch 1-2");
    add (v, CAT, "cdp_grain_rerhythm", "GRAIN RERHYTHM", "GRRH", "Multiplies the time from each grain to the next by a cycle of STEPS factors (CYCLE), or plays each grain at every timing (EACH). Grains are cut or followed by silence.",
         { Opt ("mode", "MODE", STEPMODE), P ("steps", "STEPS", 1, 4, 3, PInt), stM (1, 1), stM (2, 0.5), stM (3, 1.5), stM (4, 2), GATE, MINSIL }, grainRerhythm, {}, false, "grain rerhythm 1-2");
    add (v, CAT, "cdp_grain_remotif", "GRAIN REMOTIF", "GRMO", "Repitch and rerhythm together: each step pairs a PITCH with a TIME factor for the gap to the next grain, cycled over the grains (CYCLE) or all applied to every grain (EACH).",
         { Opt ("mode", "MODE", STEPMODE), P ("steps", "STEPS", 1, 4, 4, PInt), stP (1, 0), stM (1, 1), stP (2, 5), stM (2, 0.5), stP (3, 7), stM (3, 0.5), stP (4, 12), stM (4, 2), GATE, MINSIL },
         grainRemotif, {}, false, "grain remotif 1-2");
    add (v, CAT, "cdp_grain_timewarp", "GRAIN TIMEWARP", "GTWP", "Stretches or squeezes the time between grains (from START to END ratio over the sound) without stretching the grains themselves.",
         { P ("start", "START", 0.25, 4, 2, PExp, "x"), P ("end", "END", 0.25, 4, 2, PExp, "x"), GATE, MINSIL }, grainTimewarp, {}, false, "grain timewarp");
    add (v, CAT, "cdp_grain_reposition", "GRAIN REPOSITION", "GRPS", "Moves the grain onsets onto the tempo grid: one grain per STEP (SEQUENCE) or each grain snapped to the nearest step (QUANTISE), plus an OFFSET.",
         { Opt ("mode", "MODE", { "SEQUENCE", "QUANTISE" }), Opt ("div", "STEP", DIVS, 3), P ("offset", "OFFSET", 0, 500, 0, 0, "ms"), GATE, MINSIL }, grainReposition, {}, false, "grain reposition");
    add (v, CAT, "cdp_grain_reverse", "GRAIN REVERSE", "GREV", "Plays the grains in reverse order while each grain still plays forwards: the rhythm runs backwards, the sounds do not.",
         { GATE, MINSIL }, grainReverse, {}, false, "grain reverse");
    add (v, CAT, "cdp_grain_grev", "TROUGH GRAINS", "GRVX", "Cuts the sound at envelope troughs (WINDOW, TROUGH depth) into grains, taken GROUP at a time: REVERSE their order, REPEAT, DELETE or SILENCE (KEEP of OUT OF), STRETCH the spacing, or place them on the tempo GRID.",
         { Opt ("mode", "MODE", { "REVERSE", "REPEAT", "DELETE", "SILENCE", "STRETCH", "GRID" }), P ("wsize", "WINDOW", 2, 200, 20, PExp, "ms"), Pct ("trof", "TROUGH", 0.6),
           P ("group", "GROUP", 1, 16, 1, PInt), P ("repets", "REPEATS", 1, 16, 2, PInt), P ("keep", "KEEP", 1, 16, 2, PInt), P ("outof", "OUT OF", 1, 16, 3, PInt),
           P ("stretch", "STRETCH", 0.25, 8, 2, PExp, "x"), Opt ("div", "GRID STEP", DIVS, 4) }, grainGrev, {}, false, "grain grev 1-5, 7");
    add (v, CAT, "cdp_grain_r_extend", "ITERATIVE EXTEND", "RRRX", "Lengthens rolled or iterative sounds (rrr, drum rolls) by STRETCH, re-using their iteration units (about UNIT long) in random order with AMP and PITCH scatter, an optional RIT and REGularised rhythm.",
         { Opt ("mode", "REGION", { "MARKED", "FOUND" }), Pct ("start", "START", 0.2), Pct ("end", "END", 0.8), P ("unit", "UNIT", 5, 500, 60, PExp, "ms"), P ("stretch", "STRETCH", 1, 10, 3, PExp, "x"),
           P ("rep", "REPEATS", 1, 4, 1, PInt), Pct ("ascat", "AMP SCAT", 0.3), P ("pscat", "PITCH SCAT", 0, 12, 1, 0, "st"), P ("rit", "RIT", 1, 4, 1, PExp, "x"), Pct ("reg", "REGULARISE", 0),
           GATE, Seed() }, grainRExtend, {}, false, "grain r_extend 1-2");
    add (v, CAT, "cdp_grain_noise_extend", "NOISE EXTEND", "NOIX", "Finds the noisy part of the sound (wave-cycles faster than MIN FREQ lasting MIN DUR to MAX DUR) and stretches it to DURATION by random reads between its zero crossings.",
         { P ("dur", "DURATION", 0.2, 30, 4, PExp, {}, Fmt::time), P ("minfrq", "MIN FREQ", 500, 10000, 2000, PExp, {}, Fmt::hz), P ("mindur", "MIN DUR", 5, 500, 30, PExp, "ms"),
           P ("maxdur", "MAX DUR", 0.05, 5, 1, PExp, {}, Fmt::time), Opt ("keep", "KEEP", { "ALL", "NOISE ONLY" }), Seed() }, grainNoiseExt, {}, false, "grain noise_extend");
    add (v, CAT, "cdp_grainex_extend", "GRAIN EXTEND", "GREX", "Finds the grains (envelope troughs, WINDOW and TROUGH) between START and END and lengthens that grainy region by ADD seconds, re-using its grains in random order.",
         { P ("wsize", "WINDOW", 2, 200, 15, PExp, "ms"), Pct ("trof", "TROUGH", 0.6), P ("plus", "ADD", 0.1, 30, 3, PExp, {}, Fmt::time), Pct ("start", "START", 0), Pct ("end", "END", 1), Seed() },
         grainexExtend, {}, false, "grainex extend");
    const ParamDef RGATE = P ("gate", "GATE", -60, -3, -20, 0, {}, Fmt::db), RSIL = P ("minsil", "MIN GAP", 2, 500, 10, PExp, "ms");
    add (v, CAT, "cdp_envnu_peakchop", "PEAK CHOP", "PKCH", "Isolates the envelope peaks (found with WINDOW, above GATE): a PEAK WIDTH slice with RISE splices, placed by SKEW, replayed one per tempo STEP with SCATTER, NORMalised levels, REPEATs and MISSed peaks.",
         { P ("wsize", "WINDOW", 1, 64, 30, PExp, "ms"), P ("width", "PEAK WIDTH", 1, 1000, 40, PExp, "ms"), P ("rise", "RISE", 1, 200, 10, PExp, "ms"), Opt ("div", "STEP", DIVS, 3),
           Pct ("gain", "GAIN", 1), Pct ("gate", "GATE", 0.1), Pct ("skew", "SKEW", 0.25), Pct ("scatter", "SCATTER", 0), Pct ("norm", "NORMALISE", 0), P ("repeat", "REPEATS", 0, 16, 0, PInt),
           P ("miss", "MISS", 0, 16, 0, PInt), Seed() }, peakchop, {}, false, "envnu peakchop 1");
    add (v, CAT, "cdp_retime_tempo", "EVENTS TO TEMPO", "RTMP", "Finds the events (sounds separated by gaps below GATE) and puts one on every tempo STEP, aligning their ONSETS or their loudest PEAKS with the beat.",
         { Opt ("mode", "ALIGN", { "ONSETS", "PEAKS" }), Opt ("div", "STEP", DIVS, 3), RGATE, RSIL }, retimeTempo, {}, false, "retime retime 1, 4");
    add (v, CAT, "cdp_retime_shave", "EVENT SHAVE", "RSHV", "Cuts every event longer than IN WIDTH down to OUT WIDTH with a SPLICE, leaving its timing alone: a tight, staccato version of the phrase.",
         { P ("inw", "IN WIDTH", 1, 1000, 30, PExp, "ms"), P ("outw", "OUT WIDTH", 1, 1000, 60, PExp, "ms"), P ("splice", "SPLICE", 1, 200, 15, PExp, "ms"), RGATE, RSIL }, retimeShave, {}, false, "retime retime 3");
    add (v, CAT, "cdp_retime_speed", "EVENT SPEED", "RSPD", "Plays the sequence of events faster or slower by a FACTOR (START to END over the sound) while every event keeps its own speed and pitch.",
         { P ("start", "START", 0.25, 4, 1.5, PExp, "x"), P ("end", "END", 0.25, 4, 1.5, PExp, "x"), RGATE, RSIL }, retimeSpeed, {}, false, "retime retime 5");
    add (v, CAT, "cdp_retime_pattern", "EVENT PATTERN", "RPAT", "Re-places the events on the beats of a rhythm PATTERN (straight, shuffle, gallop, tresillo, clave, accelerando...) with STEP-long beats at the host tempo, after an OFFSET.",
         { Opt ("pattern", "PATTERN", { "STRAIGHT", "SHUFFLE", "GALLOP", "TRESILLO", "CLAVE 3-2", "BOSSA", "ACCEL", "RIT", "STOP" }, 4), Opt ("div", "STEP", DIVS, 2),
           P ("offset", "OFFSET", 0, 1000, 0, 0, "ms"), RGATE, RSIL }, retimePattern, {}, false, "retime retime 6-7");
    add (v, CAT, "cdp_retime_repeat", "EVENT REPEAT", "RRPT", "Captures COUNT events starting near WHERE and repeats them REPEATS times on the tempo grid (STEP) inside the sound; the rest of the sound follows after.",
         { Pct ("where", "WHERE", 0.25), P ("cnt", "COUNT", 1, 24, 1, PInt), P ("repeats", "REPEATS", 1, 64, 3, PInt), Opt ("div", "STEP", DIVS, 3), RGATE, RSIL }, retimeRepeat, {}, false, "retime retime 8");
    add (v, CAT, "cdp_retime_mask", "EVENT MASK", "RMSK", "Silences events by a repeating on/off pattern of STEPS steps (S1-S8): an event-based gate that follows the playing rather than a grid.",
         { P ("steps", "STEPS", 1, 8, 4, PInt), Opt ("s1", "S1", { "OFF", "ON" }, 1), Opt ("s2", "S2", { "OFF", "ON" }, 0), Opt ("s3", "S3", { "OFF", "ON" }, 1), Opt ("s4", "S4", { "OFF", "ON" }, 1),
           Opt ("s5", "S5", { "OFF", "ON" }, 1), Opt ("s6", "S6", { "OFF", "ON" }, 0), Opt ("s7", "S7", { "OFF", "ON" }, 1), Opt ("s8", "S8", { "OFF", "ON" }, 0), RGATE, RSIL }, retimeMask, {}, false, "retime retime 9");
    add (v, CAT, "cdp_retime_level", "EVENT LEVELS", "RLVL", "Evens out the levels of the events (EVENING 0-1), or accents every METER-th event at full level with the others at EVENING of it.",
         { Pct ("evening", "EVENING", 0.7), P ("meter", "METER", 0, 16, 0, PInt), RGATE, RSIL }, retimeLevel, {}, false, "retime retime 10");
    add (v, CAT, "cdp_envspeak", "SYLLABLE MACHINE", "ESPK", "Cuts speech (or any sound) into syllables at envelope troughs (or ONSETS / a GRID) and REPEATs, reverse-repeats, attenuates N in N+1, shrinks, lops, fractions, shuffles or N-wise reverses them. OFFSET syllables pass unchanged.",
         { Opt ("mode", "MODE", { "REPEAT", "REV REPEAT", "ATTEN N/N+1", "ATTEN 1/N+1", "SHRINK END", "SHRINK START", "FRACTION", "LOP END", "LOP START", "SHUFFLE", "N-WISE REV" }),
           Opt ("cut", "CUT", CUTS), P ("wsize", "WINDOW", 5, 200, 50, PExp, "ms"), P ("splice", "SPLICE", 1, 50, 8, PExp, "ms"), P ("offset", "OFFSET", 0, 16, 0, PInt),
           P ("n", "N", 1, 16, 3, PInt), Pct ("rand", "RAND", 0), P ("atten", "ATTEN", -60, 0, -18, 0, {}, Fmt::db), P ("which", "WHICH", 1, 16, 1, PInt), P ("ratio", "RATIO", 0.2, 0.95, 0.7, 0, "x"), Seed() },
         envspeak, {}, false, "envspeak 1-24");
    add (v, CAT, "cdp_fturanal_synth", "FORMANT SORT", "FSRT", "Cuts the sound into segments (CUT) and re-assembles them in order of rising or falling first, second or third formant, brightness, or all formants summed.",
         { Opt ("mode", "ORDER", { "F1 UP", "F1 DOWN", "F2 UP", "F2 DOWN", "F3 UP", "F3 DOWN", "BRIGHT UP", "BRIGHT DOWN", "FORMANTS UP", "FORMANTS DN" }, 6), Opt ("cut", "CUT", CUTS, 1),
           P ("wsize", "WINDOW", 5, 200, 40, PExp, "ms"), P ("splice", "SPLICE", 2, 15, 5, 0, "ms") }, fturSynth, {}, false, "fturanal synth 1-10");
    add (v, CAT, "cdp_packet", "WAVE PACKETS", "PAKT", "Extracts COUNT wave-packets between START and END, FOUND between signal minima or FORCED to DUR, shaped by a NARROWING envelope with its peak at CENTRE, and strings them together with GAPs.",
         { Opt ("mode", "MODE", { "FOUND", "FORCED" }, 1), P ("count", "COUNT", 1, 64, 12, PInt), Pct ("start", "START", 0.05), Pct ("end", "END", 0.95), P ("dur", "DUR", 2, 500, 60, PExp, "ms"),
           P ("narrow", "NARROWING", 0.1, 100, 2, PExp, "x"), P ("centre", "CENTRING", -1, 1, -0.5), Opt ("level", "LEVEL", { "AS IS", "NORMALISE", "FORCE" }), P ("gap", "GAP", 0, 500, 40, 0, "ms") },
         packet, {}, false, "packet packet 1-2");
    add (v, CAT, "cdp_dvdwind", "DVD WIND", "DVDW", "Fast-forward like a scanning DVD: plays a CLIP, skips ahead, plays the next clip, so the sound runs CONTRACTION times faster at its own pitch.",
         { P ("contract", "CONTRACTION", 1.1, 20, 4, PExp, "x"), P ("clip", "CLIP", 5, 500, 60, PExp, "ms") }, dvdwind, {}, false, "dvdwind dvdwind");
    add (v, CAT, "cdp_sorter", "ELEMENT SORTER", "SORT", "Chops the sound into elements of about SIZE (0 = single wavesets) and sorts them into a CRESCENDO, DECRESCENDO, ACCELERANDO, RITARDANDO or RANDOM order. OUT PITCH re-spaces them at a pitched rate.",
         { Opt ("mode", "MODE", { "CRESCENDO", "DECRESC", "ACCEL", "RIT", "RANDOM" }), P ("size", "SIZE", 0, 1, 0.08, 0, {}, Fmt::time), P ("smooth", "SMOOTH", 1, 50, 5, PExp, "ms"),
           P ("opch", "OUT PITCH", 0, 96, 0, PInt, {}, Fmt::note), Seed() }, sorter, {}, false, "sorter sorter 1-5");
    add (v, CAT, "cdp_stutter", "STUTTER", "STUT", "Slices the sound into elements (CUT) and plays segments cut from their starts in random order for DUR, sometimes JOINing elements, with random SILENCEs, TRANSposition and ATTENuation; BIAS favours short or long cuts.",
         { Opt ("cut", "CUT", { "ONSETS", "ENVELOPE", "8 SLICES", "16 SLICES" }), P ("dur", "DUR", 0.5, 60, 6, PExp, {}, Fmt::time), P ("joins", "JOINS", 1, 8, 2, PInt), Pct ("silprop", "SILENCES", 0.3),
           P ("silmin", "SIL MIN", 0, 1, 0.02, 0, {}, Fmt::time), P ("silmax", "SIL MAX", 0, 2, 0.15, 0, {}, Fmt::time), P ("trans", "TRANS", 0, 12, 0, 0, "st"), Pct ("atten", "ATTEN", 0.3),
           P ("bias", "BIAS", -1, 1, 0), P ("mindur", "MIN DUR", 8, 500, 30, PExp, "ms"), Opt ("perm", "ORDER", { "RANDOM", "PERMUTE" }), Seed() }, stutter, {}, false, "stutter stutter");
    add (v, CAT, "cdp_motor", "MOTOR", "MOTR", "A fast inner pulse (FREQ, on-time FRATIO) inside slow outer swells (PULSE, PRATIO, peak at SYM, shape BITE): inner events step through the source as each swell rises and back as it falls, with JITTER and TREMOR.",
         { Opt ("mode", "READ", { "UP-DOWN", "ADVANCE", "EITHER" }), Opt ("source", "SOURCE", { "WHOLE", "SLICES" }), P ("dur", "DUR", 1, 60, 8, PExp, {}, Fmt::time), P ("freq", "FREQ", 2, 100, 16, PExp, "Hz"),
           P ("pulse", "PULSE", 0.1, 10, 0.8, PExp, "Hz"), Pct ("fratio", "FRATIO", 0.6), Pct ("pratio", "PRATIO", 0.8), Pct ("sym", "SYM", 0.5), P ("jitter", "JITTER", 0, 3, 0.2, 0, "st"),
           Pct ("tremor", "TREMOR", 0.2), P ("bite", "BITE", 0.1, 10, 3, PExp, "x"), Seed() }, motor, {}, false, "motor motor 1-9");
    const std::string BCAT = "CDP \xc2\xb7 GRAIN \xc2\xb7 BRASSAGE";
    const ParamDef GR = P ("grain", "GRAIN", 5, 500, 50, PExp, "ms");
    add (v, BCAT, "cdp_modify_brassage1", "BRASSAGE PITCH", "BRS1", "Granular pitch shift: overlapping GRAIN-sized grains are re-read at the new PITCH while the length stays the same. Small grains buzz, large ones echo.",
         { P ("pitch", "PITCH", -24, 24, 7, 0, "st"), GR, Seed() }, brass1, {}, false, "modify brassage 1");
    add (v, BCAT, "cdp_modify_brassage2", "BRASSAGE STRETCH", "BRS2", "Granular time stretch: grains advance through the source VELOCITY times as fast as the output (0.5 = twice as long), the pitch stays.",
         { P ("vel", "VELOCITY", 0.05, 4, 0.5, PExp, "x"), GR, Seed() }, brass2, {}, false, "modify brassage 2");
    add (v, BCAT, "cdp_modify_brassage3", "BRASSAGE REVERB", "BRS3", "Granular reverb: DENSITY overlapping grains drawn from a RANGE behind the playhead, each with random PITCH spread and AMP spread: a shimmering smear of the sound.",
         { P ("dens", "DENSITY", 1, 32, 8, PExp, "x"), P ("pitch", "PITCH", 0, 12, 0.5, 0, "st"), Pct ("amp", "AMP SPREAD", 0.5), P ("range", "RANGE", 0, 2000, 150, 0, "ms"), GR, Seed() },
         brass3, {}, false, "modify brassage 3");
    add (v, BCAT, "cdp_modify_brassage4", "BRASSAGE SCRAMBLE", "BRS4", "Granular scramble: each GRAIN is taken from anywhere within RANGE before the current position, jumbling the sound locally while it keeps moving forward.",
         { P ("grain", "GRAIN", 5, 500, 80, PExp, "ms"), P ("range", "RANGE", 0, 5000, 500, 0, "ms"), Seed() }, brass4, {}, false, "modify brassage 4");
    add (v, BCAT, "cdp_modify_brassage5", "BRASSAGE GRANULATE", "BRS5", "Granulate: changes the DENSITY of the grain stream. Below 1 the grains pull apart with silence between them, above 1 they pile up.",
         { P ("dens", "DENSITY", 0.1, 16, 0.5, PExp, "x"), GR, Seed() }, brass5, {}, false, "modify brassage 5");
    add (v, BCAT, "cdp_modify_brassage6", "BRASSAGE FULL", "BRS6", "The complete brassage: VELOCITY (time stretch), DENSITY, GRAIN size, PITCH, AMP, stereo SPACE, start and end SPLICEs, search RANGE and JITTER of grain times.",
         { P ("vel", "VELOCITY", 0.05, 4, 0.7, PExp, "x"), P ("dens", "DENSITY", 0.1, 16, 2, PExp, "x"), GR, P ("pitch", "PITCH", -24, 24, -5, 0, "st"), Pct ("amp", "AMP", 1),
           Pct ("space", "SPACE", 0.5), P ("bspl", "B SPLICE", 1, 50, 5, PExp, "ms"), P ("espl", "E SPLICE", 1, 50, 5, PExp, "ms"), P ("range", "RANGE", 0, 2000, 100, 0, "ms"),
           Pct ("jitter", "JITTER", 0.5), Seed() }, brass6, {}, false, "modify brassage 6");
    add (v, BCAT, "cdp_modify_brassage7", "BRASSAGE MONTY", "BRS7", "The 'full monty': every grain draws its VELOCITY, DENSITY, GRAIN size and PITCH at random between LOW and HIGH, its level above AMP MIN and its position within the stereo SPREAD.",
         { P ("vlo", "VEL LOW", 0.05, 4, 0.3, PExp, "x"), P ("vhi", "VEL HIGH", 0.05, 4, 1.2, PExp, "x"), P ("dlo", "DENS LOW", 0.1, 16, 1, PExp, "x"), P ("dhi", "DENS HIGH", 0.1, 16, 4, PExp, "x"),
           P ("glo", "GRAIN LOW", 5, 500, 20, PExp, "ms"), P ("ghi", "GRAIN HIGH", 5, 500, 150, PExp, "ms"), P ("plo", "PITCH LOW", -24, 24, -7, 0, "st"), P ("phi", "PITCH HIGH", -24, 24, 7, 0, "st"),
           Pct ("amin", "AMP MIN", 0.4), Pct ("spread", "SPREAD", 1), Pct ("jitter", "JITTER", 0.5), Seed() }, brass7, {}, false, "modify brassage 7");
    add (v, BCAT, "cdp_wrappage", "WRAPPAGE", "WRAP", "Brassage spread across the stereo field around a CENTRE moving from START to END: grains land within SPREAD of it (DEPTH 0 = only at its edges), with VELOCITY, DENSITY, GRAIN size and a PITCH range.",
         { Pct ("cstart", "CENTRE STRT", 0.2), Pct ("cend", "CENTRE END", 0.8), Pct ("spread", "SPREAD", 0.6), Pct ("depth", "DEPTH", 0.5), P ("vel", "VELOCITY", 0.05, 4, 0.6, PExp, "x"),
           P ("dens", "DENSITY", 0.1, 16, 3, PExp, "x"), GR, P ("plo", "PITCH LOW", -24, 24, -2, 0, "st"), P ("phi", "PITCH HIGH", -24, 24, 2, 0, "st"), Pct ("jitter", "JITTER", 0.5), Seed() },
         wrappage, {}, false, "wrappage wrappage");
    const std::string TCAT = "CDP \xc2\xb7 GRAIN \xc2\xb7 TEXTURE";
    add (v, TCAT, "cdp_fracture", "FRACTURE", "FRAC", "Disperses the sound into enveloped fragments over STREAMS stereo positions, a set every PULSE, with read, pulse and stream-timing (DISP) scatter, random level and TRANSposition. DEPTH above 1 stacks transposed copies. RECEDING moves it away.",
         { Opt ("mode", "MODE", { "SPREAD", "RECEDING" }), Opt ("env", "ENVELOPE", { "SWELL", "ATTACK", "CRESC", "DOUBLE" }), P ("streams", "STREAMS", 2, 16, 6, PInt), P ("pulse", "PULSE", 0.02, 2, 0.15, PExp, {}, Fmt::time),
           P ("depth", "DEPTH", 0, 3, 1.5), P ("stkint", "STACK INT", 1, 12, 12, 0, "st"), Pct ("rrnd", "READ RAND", 0.5), Pct ("prnd", "PULSE RAND", 0.3), Pct ("disp", "DISPERSAL", 0.5),
           Pct ("lrnd", "LEVEL RAND", 0.5), P ("trnd", "TRANS RAND", 0, 12, 0.5, 0, "st"), Seed() }, fracture, {}, false, "fracture fracture 1-2");
    add (v, TCAT, "cdp_newtex", "NEW TEXTURE", "NTEX", "A texture of looping components fading in and out every STEP, NUMBER at a time across the stereo field: TRANSPOSED over RANGE octaves of a CHORD, at the ORIGINAL rate offset by DELAY, or DRUNKen reads around LOCUS.",
         { Opt ("mode", "MODE", { "TRANSPOSED", "ORIGINAL", "DRUNK" }), P ("dur", "DUR", 1, 60, 10, PExp, {}, Fmt::time), P ("range", "RANGE", 1, 4, 2, PInt), P ("step", "STEP", 0.05, 4, 0.5, PExp, {}, Fmt::time),
           P ("number", "NUMBER", 1, 8, 3, PInt), P ("splice", "SPLICE", 2, 500, 60, PExp, "ms"), Opt ("space", "SPACE", { "RANDOM", "ALTERNATE", "ROTATE", "LEFT-RIGHT" }),
           Opt ("chord", "CHORD", { "UNISON", "FIFTHS", "MAJOR", "MINOR", "PENTATONIC", "QUARTAL", "MAJ7", "CHROMATIC" }, 2), P ("delay", "DELAY", 0.01, 2, 0.25, PExp, {}, Fmt::time),
           Pct ("loc", "LOCUS", 0.5), P ("amb", "AMBITUS", 0.01, 2, 0.3, PExp, {}, Fmt::time), Seed() }, newtex, {}, false, "newtex newtex 1-3");
    add (v, TCAT, "cdp_brownian", "BROWNIAN", "BRWN", "Events wander by brownian motion in pitch (within PITCH LO-HI, max STEP), stereo position (SPACE STEP) and level (AMP STEP), one every TICK: the source read as a WAVEFORM with ATTACK and DECAY, or the WHOLE source transposed.",
         { Opt ("mode", "MODE", { "WAVEFORM", "WHOLE" }), P ("dur", "DUR", 1, 60, 10, PExp, {}, Fmt::time), P ("att", "ATTACK", 1, 500, 10, PExp, "ms"), P ("dec", "DECAY", 10, 4000, 250, PExp, "ms"),
           P ("plo", "PITCH LO", 24, 108, 48, PInt, {}, Fmt::note), P ("phi", "PITCH HI", 24, 108, 84, PInt, {}, Fmt::note), P ("step", "STEP", 0, 24, 3, 0, "st"), Pct ("sstep", "SPACE STEP", 0.2),
           P ("tick", "TICK", 0.01, 2, 0.12, PExp, {}, Fmt::time), P ("arange", "AMP STEP", 0, 24, 4, 0, {}, Fmt::db), Seed() }, brownian, {}, false, "brownian motion 1-2");
    add (v, TCAT, "cdp_crystal_rotate", "CRYSTAL ROTATE", "CRYR", "A crystal (SHAPE) rotates in 3-D (ROT XY, ROT XZ); every TSTEP each vertex sounds the source: x sets time in TWIDTH and position, y the pitch (PITCH LO-HI), z the brightness (far = lowpass below PASS, near = octave added).",
         { Opt ("shape", "SHAPE", { "TETRA", "CUBE", "OCTA", "ICOSA", "PRISM", "HELIX" }, 1), Opt ("mode", "SPACE", { "MONO", "STEREO", "SWEEP L>R", "SWEEP R>L", "RANDOM" }, 1),
           P ("rota", "ROT XY", 0.01, 4, 0.13, PExp, "Hz"), P ("rotb", "ROT XZ", 0.01, 4, 0.07, PExp, "Hz"), P ("twidth", "TWIDTH", 0, 2, 0.4, 0, {}, Fmt::time), P ("tstep", "TSTEP", 0.05, 4, 0.6, PExp, {}, Fmt::time),
           P ("dur", "DUR", 1, 60, 10, PExp, {}, Fmt::time), P ("plo", "PITCH LO", 24, 108, 48, PInt, {}, Fmt::note), P ("phi", "PITCH HI", 24, 108, 72, PInt, {}, Fmt::note),
           P ("evdur", "EVENT DUR", 0.05, 4, 0.6, PExp, {}, Fmt::time), P ("pass", "PASS", 100, 8000, 800, PExp, {}, Fmt::hz) }, crystal, {}, false, "crystal rotate 1-9");
    add (v, TCAT, "cdp_pulser_pulser1", "PULSER SYNTH", "PLS1", "A stream of pitched wave-packets at PITCH that borrow only the brightness of the source: RISE, SUSTAIN and DECAY (ENV RAND), one every SPEED with SCATTER, pitch scatter, a lower OCTAVE and an upward BEND.",
         { P ("dur", "DUR", 1, 60, 8, PExp, {}, Fmt::time), P ("pitch", "PITCH", 24, 96, 55, PInt, {}, Fmt::note), P ("rise", "RISE", 0.002, 0.2, 0.01, PExp, {}, Fmt::time), P ("sus", "SUSTAIN", 0, 0.2, 0.02, 0, {}, Fmt::time),
           P ("decay", "DECAY", 0.02, 2, 0.2, PExp, {}, Fmt::time), Pct ("erand", "ENV RAND", 0.5), P ("speed", "SPEED", 0.05, 1, 0.15, PExp, {}, Fmt::time), Pct ("scatter", "SCATTER", 0.4),
           P ("pscat", "PITCH SCAT", 0, 1, 0.2, 0, "st"), Pct ("octav", "OCTAVE", 0.3), P ("bend", "BEND", 0, 1, 0.3, 0, "st"), Seed() }, pulser1, {}, false, "pulser pulser 1");
    add (v, TCAT, "cdp_pulser_pulser2", "PULSER STREAM", "PLS2", "A stream of enveloped packets cut from the START of the source or from RANDOM points: RISE, SUSTAIN, DECAY (ENV RAND), one every SPEED with SCATTER, pitch scatter and BEND, spread over a stereo WIDTH.",
         { Opt ("mode", "MODE", { "START", "RANDOM" }, 1), P ("dur", "DUR", 1, 60, 8, PExp, {}, Fmt::time), P ("rise", "RISE", 0.002, 0.2, 0.005, PExp, {}, Fmt::time), P ("sus", "SUSTAIN", 0, 0.2, 0.03, 0, {}, Fmt::time),
           P ("decay", "DECAY", 0.02, 2, 0.15, PExp, {}, Fmt::time), Pct ("erand", "ENV RAND", 0.5), P ("speed", "SPEED", 0.05, 1, 0.12, PExp, {}, Fmt::time), Pct ("scatter", "SCATTER", 0.5),
           P ("pscat", "PITCH SCAT", 0, 1, 0.3, 0, "st"), P ("bend", "BEND", 0, 1, 0, 0, "st"), Pct ("width", "WIDTH", 0.8), Seed() }, pulser2, {}, false, "pulser pulser 2-3");
    add (v, TCAT, "cdp_rotor", "ROTOR", "ROTR", "Sets of COUNT notes (the source at different speeds) whose pitch range (PITCH CYC sets) and speed (SPEED CYC sets, PHASE apart) grow and shrink; sets start every GROUP STEP, after the last, or overlapping it.",
         { Opt ("mode", "MODE", { "GROUP STEP", "FOLLOW", "OVERLAP" }), P ("cnt", "COUNT", 3, 64, 8, PInt), P ("minp", "PITCH LO", 24, 108, 55, PInt, {}, Fmt::note), P ("maxp", "PITCH HI", 24, 108, 79, PInt, {}, Fmt::note),
           P ("step", "STEP", 0.01, 2, 0.12, PExp, {}, Fmt::time), P ("prot", "PITCH CYC", 4, 64, 6, PInt), P ("trot", "SPEED CYC", 4, 64, 9, PInt), Pct ("phas", "PHASE", 0.25),
           P ("dur", "DUR", 1, 60, 10, PExp, {}, Fmt::time), P ("gstp", "GROUP STEP", 0.1, 10, 1.5, PExp, {}, Fmt::time), P ("evdur", "EVENT DUR", 0.05, 4, 0.4, PExp, {}, Fmt::time),
           Opt ("stereo", "OUTPUT", { "MONO", "STEREO" }, 1) }, rotor, {}, false, "rotor rotor 1-3");
}
} // namespace lab
