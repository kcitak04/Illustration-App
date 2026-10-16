// CDP collection, Batch 7 - Envelope / dynamics / filter.
// Envelope processes follow CDP's envel programs (dev/env): the amplitude envelope is read as the peak level
// of consecutive windows (WINDOW ms), changed, and re-applied with linear interpolation between windows.
#include "LabCdp.h"
#include "LabDspUtil.h"
#include <array>

namespace lab
{
namespace
{
using namespace lab::dsp;

// ===================================================================================================
// shared helpers
// ===================================================================================================
inline void tick (Ctx& cx, size_t i, size_t n, double a = 0, double b = 1)
{
    if (! (i & 32767)) cx.prog (a + (b - a) * (double) i / (double) std::max<size_t> (1, n));
}
// tiny offset (DC + Nyquist) on filter inputs: keeps decaying filter states out of the slow denormal range
inline double adn (size_t i) { return (i & 1) ? 2e-20 : 0.0; }
inline size_t msS (double ms, double sr) { return (size_t) std::max (1.0, std::round (ms / 1000.0 * sr)); }
inline double dbG (double db) { return std::pow (10.0, db / 20.0); }
inline double midiHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

// multiplies every channel by g(i)
template <typename G> Chans applyGain (const Chans& ch, Ctx& cx, G g, size_t len = 0)
{
    if (! len) len = ch[0].size();
    len = std::min (len, ch[0].size());
    Chans out (ch.size(), Buf (len));
    for (size_t i = 0; i < len; ++i)
    {
        tick (cx, i, len);
        const double v = g (i);
        for (size_t c = 0; c < ch.size(); ++c) out[c][i] = (float) (ch[c][i] * v);
    }
    return out;
}

// CDP envelope: the largest absolute sample (any channel) in each window of W samples
DVec winEnv (const Chans& ch, size_t W)
{
    const size_t len = ch[0].size(), n = (len + W - 1) / W;
    DVec e (std::max<size_t> (1, n), 0.0);
    for (auto& x : ch)
        for (size_t i = 0; i < len; ++i) e[i / W] = std::max (e[i / W], (double) std::abs (x[i]));
    return e;
}
// envelope value at sample i (each value sits at its window centre, linear in between)
inline double envAt (const DVec& e, size_t W, double i)
{
    if (e.empty()) return 1;
    double p = clampd ((i - (double) W * 0.5) / (double) W, 0, (double) e.size() - 1);
    size_t k = (size_t) p; double f = p - (double) k;
    return k + 1 < e.size() ? e[k] + (e[k + 1] - e[k]) * f : e[k];
}
// CDP envreplace: gain = new / original, holding the previous gain where the original touches zero
DVec envRatio (const DVec& ne, const DVec& oe)
{
    const double gate = 1.0 / 32767.0; const size_t n = std::min (ne.size(), oe.size());
    DVec g (std::max<size_t> (1, n), 0.0);
    for (size_t k = 0; k < n; ++k)
    {
        if (oe[k] <= gate) g[k] = (ne[k] <= gate || k == 0) ? 0.0 : g[k - 1];
        else g[k] = std::min (ne[k] / oe[k], 3000.0);
    }
    return g;
}
inline Chans applyEnv (const Chans& ch, Ctx& cx, const DVec& g, size_t W)
{
    return applyGain (ch, cx, [&] (size_t i) { return envAt (g, W, (double) i); });
}

// CDP breakpoint envelopes (create_envelope): a segment is linear, exponential (power 1.7),
// steep (2.0) or doubly exponential (3.4), always curving towards the lower level.
struct BP { double t, v; int type; };
inline double curveSeg (double v0, double v1, double u, int type)
{
    u = clampd (u, 0, 1);
    if (type == 0) return v0 + (v1 - v0) * u;
    const double a = type == 1 ? 1.7 : type == 2 ? 2.0 : 3.4;
    if (v1 <= v0) return v1 + (v0 - v1) * std::pow (1 - u, a);
    return v0 + (v1 - v0) * std::pow (u, a);
}
struct BPCursor
{
    const std::vector<BP>& b; size_t k = 1;
    explicit BPCursor (const std::vector<BP>& bb) : b (bb) {}
    double at (double t)   // t must not decrease between calls
    {
        if (b.empty()) return 1;
        if (t <= b[0].t) return b[0].v;
        while (k < b.size() && t > b[k].t) ++k;
        if (k >= b.size()) return b.back().v;
        const double d = b[k].t - b[k - 1].t;
        return curveSeg (b[k - 1].v, b[k].v, d > 0 ? (t - b[k - 1].t) / d : 1, b[k].type);
    }
};
Chans applyBP (const Chans& ch, Ctx& cx, const std::vector<BP>& b, size_t len = 0)
{
    BPCursor cur (b); const double sr = cx.sr;
    return applyGain (ch, cx, [&] (size_t i) { return cur.at ((double) i / sr); }, len);
}

// generated envelope shapes (stand in for CDP's envelope / breakpoint files), u = 0..1 -> level 0..1
const defs::O SHAPES { "ADSR", "FADE IN", "FADE OUT", "SWELL", "DIP", "PULSES", "STAIRS", "RANDOM" };
struct ShapeGen
{
    int shape = 0, count = 4; double atk = 0.1, rel = 0.3, sus = 0.5, curve = 1; DVec rnd;
    ShapeGen (const Params& p, Ctx& cx)
    {
        shape = p.i ("shape"); count = std::max (1, p.i ("count")); atk = clampd (p["attack"], 0.001, 1); rel = clampd (p["release"], 0.001, 1);
        sus = clampd (p["sustain"], 0, 1); curve = clampd (p["curve"], 0.1, 10);
        cx.seed (p["seed"]); rnd.resize ((size_t) count + 2); for (auto& r : rnd) r = sus + (1 - sus) * cx.rand();
    }
    double raw (double u) const
    {
        u = clampd (u, 0, 1);
        switch (shape)
        {
            case 0:   // ADSR: attack, decay (same length) to sustain, hold, release
            {
                double a = atk * 0.5, r = rel;
                if (a + a + r > 1) { double s = 1 / (a + a + r); a *= s; r *= s; }
                if (u < a) return u / a;
                if (u < 2 * a) return 1 - (1 - sus) * (u - a) / a;
                if (u < 1 - r) return sus;
                return sus * (1 - u) / r;
            }
            case 1: return u;
            case 2: return 1 - u;
            case 3: { double pk = clampd (atk, 0.01, 0.99); return u < pk ? u / pk : (1 - u) / (1 - pk); }
            case 4: return 1 - (1 - sus) * (1 - std::abs (2 * u - 1));
            case 5:
            {
                double ph = u * count; ph -= std::floor (ph);
                double a = clampd (atk * 0.5, 0.005, 0.9);
                return ph < a ? ph / a : sus + (1 - sus) * (1 - (ph - a) / (1 - a));
            }
            case 6: { double st = std::floor (u * count); return sus + (1 - sus) * std::min (1.0, (st + 1) / count); }
            default:
            {
                double p = u * count; size_t k = (size_t) p; double f = p - (double) k;
                return rnd[k] + (rnd[std::min (k + 1, rnd.size() - 1)] - rnd[k]) * f;
            }
        }
    }
    double at (double u) const { return std::pow (clampd (raw (u), 0, 1), curve); }
};
inline std::vector<ParamDef> shapeParams()
{
    using namespace defs;
    return { Opt ("shape", "SHAPE", SHAPES), Pct ("attack", "ATTACK", 0.1), Pct ("release", "RELEASE", 0.3), Pct ("sustain", "SUSTAIN", 0.5),
             P ("count", "COUNT", 1, 64, 8, PInt), P ("curve", "CURVE", 0.25, 4, 1, PExp, "x") };
}
// level 0..1 -> gain, in GAIN mode directly, in dB mode over a RANGE of dB (CDP's dB breakpoint files)
inline double shapeGain (double v, bool db, double range) { return db ? (v <= 0 ? 0 : dbG ((v - 1) * range)) : v; }

// ===================================================================================================
// ENVELOPE
// ===================================================================================================

// SHAPED ENVELOPE: imposes a generated envelope (gain or dB) on the region START..END, which becomes the output.
// after CDP envel impose 2-4 / envel scaled / modify loudness 11-12 (envimpos.c:impose_envel_on_sndfile, envel.c:apply_brkpnt_envelope)
Chans envImpose (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t len = ch[0].size();
    size_t a = (size_t) std::floor (clampd (p["start"], 0, 1) * (double) len), b = (size_t) std::ceil (clampd (p["end"], 0, 1) * (double) len);
    if (b <= a + 16) { a = 0; b = len; }
    b = std::min (b, a + cx.maxLen);
    Chans seg; for (auto& x : ch) seg.push_back (Buf (x.begin() + (long) a, x.begin() + (long) b));
    ShapeGen sg (p, cx); const bool db = p.i ("mode") == 1; const double n = (double) (b - a), range = p["range"];
    return applyGain (seg, cx, [&] (size_t i) { return shapeGain (sg.at ((double) i / std::max (1.0, n - 1)), db, range); });
}

// REPLACE ENVELOPE: divides out the sound's own envelope and applies a generated one instead.
// after CDP envel replace 2-4 (envfuncs.c:envreplace, envimpos.c)
Chans envReplace (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t W = msS (p["wsize"], cx.sr); const DVec oe = winEnv (ch, W);
    ShapeGen sg (p, cx); const bool db = p.i ("mode") == 1; const double range = p["range"];
    double mx = 0; for (double v : oe) mx = std::max (mx, v);
    DVec ne (oe.size());
    for (size_t k = 0; k < ne.size(); ++k) ne[k] = mx * shapeGain (sg.at ((double) k / std::max (1.0, (double) ne.size() - 1)), db, range);
    return applyEnv (ch, cx, envRatio (ne, oe), W);
}

// ---- envel warp: one process per CDP mode ----
template <typename F> Chans warpWith (const Chans& ch, Ctx& cx, const Params& p, F f)
{
    const size_t W = msS (p["wsize"], cx.sr); const DVec oe = winEnv (ch, W);
    DVec ne = oe; f (ne);
    return applyEnv (ch, cx, envRatio (ne, oe), W);
}
inline double maxOf (const DVec& e) { double m = 0; for (double v : e) m = std::max (m, v); return m; }

// ENV NORMALISE: scales the envelope so its highest point reaches full level. after CDP envel warp 1 (envfuncs.c:envnorm)
Chans warpNorm (const Chans& ch, Ctx& cx, const Params& p)
{
    return warpWith (ch, cx, p, [] (DVec& e) { double m = maxOf (e); if (m > 0) for (auto& v : e) v = std::min (1.0, v / m); });
}
// ENV REVERSE: the loudness contour runs backwards while the sound plays forwards. after CDP envel warp 2 (envfuncs.c:envrevers)
Chans warpRev (const Chans& ch, Ctx& cx, const Params& p)
{
    return warpWith (ch, cx, p, [] (DVec& e) { std::reverse (e.begin(), e.end()); });
}
// ENV EXAGGERATE: level^EXAGGERATE (>1 deepens the contour, <1 lifts quiet parts). after CDP envel warp 3 (envfuncs.c:envexagg)
Chans warpExag (const Chans& ch, Ctx& cx, const Params& p)
{
    const double x = p["exag"];
    return warpWith (ch, cx, p, [x] (DVec& e) { for (auto& v : e) v = std::pow (std::min (1.0, v), x); });
}
// ENV ATTENUATE: scales the envelope by ATTENUATION, which may sweep from START to END. after CDP envel warp 4 (envfuncs.c:envatten)
Chans warpAtten (const Chans& ch, Ctx& cx, const Params& p)
{
    const double a0 = p["atten"], a1 = p["atten2"];
    return warpWith (ch, cx, p, [&] (DVec& e) { for (size_t k = 0; k < e.size(); ++k) e[k] *= a0 + (a1 - a0) * (double) k / std::max (1.0, (double) e.size() - 1); });
}
// ENV LIFT: adds a fixed LIFT to the envelope (clipped at full level), bringing quiet parts up. after CDP envel warp 5 (envfuncs.c:envlift)
Chans warpLift (const Chans& ch, Ctx& cx, const Params& p)
{
    const double l = p["lift"];
    return warpWith (ch, cx, p, [l] (DVec& e) { for (auto& v : e) v = std::min (1.0, v + l); });
}
// ENV TIMESTRETCH: the envelope (not the sound) is stretched or squeezed in time. after CDP envel warp 6 (envfuncs.c:envtstretch)
Chans warpStretch (const Chans& ch, Ctx& cx, const Params& p)
{
    const double ts = p["stretch"];
    return warpWith (ch, cx, p, [ts] (DVec& e)
    {
        const DVec o = e; const size_t n = o.size();
        for (size_t k = 0; k < n; ++k)
        {
            double src = (double) k / ts; size_t j = (size_t) src; double f = src - (double) j;
            e[k] = j + 1 < n ? o[j] + (o[j + 1] - o[j]) * f : o[n - 1];   // beyond the stretched end the last level is held
        }
    });
}
// ENV FLATTEN: averages the envelope over FLATTEN windows, smoothing the contour (slow compression). after CDP envel warp 7 (envfuncs.c:envflatn)
Chans warpFlat (const Chans& ch, Ctx& cx, const Params& p)
{
    const long long N = std::max (1, p.i ("flatten"));
    return warpWith (ch, cx, p, [N] (DVec& e)
    {
        const DVec o = e; const long long n = (long long) o.size(), back = N / 2;
        DVec ps ((size_t) n + 1); for (long long i = 0; i < n; ++i) ps[(size_t) i + 1] = ps[(size_t) i] + o[(size_t) i];
        auto val = [&] (long long i) { return o[(size_t) clamp (i, 0LL, n - 1)]; };
        for (long long i = 0; i < n; ++i)
        {
            double s = 0; long long a = i - back, b = a + N;   // window [a, b), edge values held
            long long ca = clamp (a, 0LL, n), cb = clamp (b, 0LL, n);
            s = ps[(size_t) cb] - ps[(size_t) ca];
            if (a < 0) s += val (0) * (double) (std::min (b, 0LL) - a);
            if (b > n) s += val (n - 1) * (double) (b - std::max (a, n));
            e[(size_t) i] = s / (double) N;
        }
    });
}
// gate with CDP smoothing: a gated stretch only re-opens when the average of the next SMOOTH windows is above the gate
void smoothGate (DVec& e, double gate, int smooth, const std::function<double (double)>& pass)
{
    bool gating = true;
    for (size_t k = 0; k < e.size(); ++k)
    {
        bool open;
        if (smooth <= 0) open = e[k] >= gate;
        else
        {
            open = false;
            if (e[k] >= gate)
            {
                if (! gating) open = true;
                else
                {
                    size_t n = std::min ((size_t) smooth, e.size() - k); double av = 0;
                    for (size_t j = 0; j < n; ++j) av += e[k + j];
                    open = av / (double) n > gate;
                }
            }
        }
        e[k] = open ? pass (e[k]) : 0.0; gating = ! open;
    }
}
// ENV GATE: windows below GATE are silenced; SMOOTHING ignores openings shorter than that many windows. after CDP envel warp 8 (envfuncs.c:envgate)
Chans warpGate (const Chans& ch, Ctx& cx, const Params& p)
{
    const double g = dbG (p["gate"]); const int sm = p.i ("smooth");
    return warpWith (ch, cx, p, [&] (DVec& e) { smoothGate (e, g, sm, [] (double v) { return v; }); });
}
// ENV INVERT: below GATE -> silence, other levels mirrored around MIRROR (loud becomes quiet). after CDP envel warp 9 (envfuncs.c:envinvert)
Chans warpInvert (const Chans& ch, Ctx& cx, const Params& p)
{
    const double mx0 = p["mirror"], g = std::min (p["gate"], mx0 * 0.99), m = std::max (mx0, g + 1e-3);
    const double hi = 1 - m, lo = m - g, up = hi / std::max (1e-6, lo), dn = lo / std::max (1e-6, hi);
    return warpWith (ch, cx, p, [&] (DVec& e)
    {
        for (auto& v : e)
        {
            if (v < g) v = 0;
            else if (v <= m) v = (m - v) * up + m;
            else v = std::max (0.0, m - (v - m) * dn);
        }
    });
}
// ENV LIMIT: levels above THRESHOLD are squeezed so the loudest reaches LIMIT. after CDP envel warp 10 (envfuncs.c:envlimit)
Chans warpLimit (const Chans& ch, Ctx& cx, const Params& p)
{
    const double th = p["thresh"], lim = std::max (p["limit"], th), sq = (lim - th) / std::max (1e-6, 1 - th);
    return warpWith (ch, cx, p, [&] (DVec& e) { for (auto& v : e) if (v > th) v = th + (v - th) * sq; });
}
// ENV CORRUGATE: every envelope trough is dug out to silence (TROUGH windows wide), leaving the peaks at full gain. after CDP envel warp 11 (envfuncs.c:envkorrug)
Chans warpCorrugate (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t W = msS (p["wsize"], cx.sr); const DVec e = winEnv (ch, W);
    const int width = std::max (2, p.i ("peaksep")), del = std::max (1, std::min (p.i ("trofdel"), width - 1));
    const long long n = (long long) e.size();
    auto extreme = [&] (long long q, bool peak)
    {
        if (width < 2) return true;
        long long up = width / 2, dn = width % 2 == 0 ? up - 1 : up;
        for (long long r = std::max (0LL, q - dn); r <= std::min (n - 1, q + up); ++r) if (peak ? e[(size_t) q] < e[(size_t) r] : e[(size_t) q] > e[(size_t) r]) return false;
        return true;
    };
    DVec ne = e, shadow (e.size(), 0.0);
    if (n >= 2)
    {
        bool upw = e[1] > e[0];
        for (long long q = 0; q + 1 < n; ++q)
        {
            if (upw) { if (e[(size_t) q + 1] <= e[(size_t) q]) { if (extreme (q, true)) shadow[(size_t) q] = 1; upw = false; } }
            else if (e[(size_t) q + 1] > e[(size_t) q]) upw = true;
        }
        for (long long q = 0; q + 1 < n; ++q)
        {
            if (upw) { if (e[(size_t) q + 1] <= e[(size_t) q]) upw = false; }
            else if (e[(size_t) q + 1] > e[(size_t) q])
            {
                if (extreme (q, false))
                {
                    long long up = del / 2, dn = del % 2 == 0 ? up - 1 : up;
                    long long lo = std::max (0LL, q - dn), hi = std::min (n - 1, q + up);
                    if (del < 2) lo = hi = q;
                    for (long long r = q; r >= lo; --r) if (shadow[(size_t) r] > 0) { lo = r + 1; break; }
                    for (long long r = q; r <= hi; ++r) if (shadow[(size_t) r] > 0) { hi = r - 1; break; }
                    for (long long r = lo; r <= hi; ++r) ne[(size_t) r] = 0;
                }
                upw = true;
            }
        }
    }
    for (auto& v : ne) v = v > 0 ? 1.0 : 0.0;   // warping: the generated gate is applied directly
    return applyEnv (ch, cx, ne, W);
}
// ENV EXPAND: below GATE -> silence, the rest squeezed upwards so the quietest level becomes THRESHOLD. after CDP envel warp 12 (envfuncs.c:envexpand)
Chans warpExpand (const Chans& ch, Ctx& cx, const Params& p)
{
    const double g = p["gate"], th = std::max (p["thresh"], g), sq = (1 - th) / std::max (1e-6, 1 - g); const int sm = p.i ("smooth");
    return warpWith (ch, cx, p, [&] (DVec& e) { smoothGate (e, g + 1e-9, sm, [sq] (double v) { return 1.0 - (1.0 - std::min (1.0, v)) * sq; }); });
}
// ENV TRIGGER: fast rises of the envelope (RISE within DUR, mean level above GATE) fire new bursts shaped by ATTACK / DECAY / CURVE.
// after CDP envel warp 13 (envfuncs.c:envtrig, locate_and_size_triggers, gen_reapplied_trigger_envelope)
Chans warpTrigger (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t W = msS (p["wsize"], sr); const DVec e = winEnv (ch, W);
    const double ts = (double) W / sr, gate = p["gate"], rise = p["rise"];
    const long long scan = std::max (1LL, (long long) (p["dur"] / 1000.0 / ts));
    std::vector<double> trig;   // trigger times (s)
    const long long n = (long long) e.size();
    bool triggered = false;
    for (long long k = 0; k + scan + 1 < n;)
    {
        double mr = -1e9; for (long long j = 1; j <= scan; ++j) mr = std::max (mr, e[(size_t) (k + j)] - e[(size_t) k]);
        if (e[(size_t) k] + mr / 2 > gate && std::abs (mr) >= rise)
        {
            if (! triggered) trig.push_back ((double) (k + scan) * ts);
            triggered = true; k += scan;
        }
        else { triggered = false; ++k; }
    }
    const double atk = std::max (0.003, p["attack"] / 1000.0), dec = std::max (0.005, p["decay"] / 1000.0), cv = p["curve"];
    const size_t len = ch[0].size(); size_t t = 0;
    return applyGain (ch, cx, [&] (size_t i)
    {
        const double s = (double) i / sr;
        while (t + 1 < trig.size() && trig[t + 1] - atk <= s) ++t;
        if (trig.empty()) return 0.0;
        double d = s - (trig[t] - atk), nxt = t + 1 < trig.size() ? trig[t + 1] - atk : 1e30;
        if (d < 0 || s >= nxt) return 0.0;
        if (d < atk) return d / atk;   // ramps cannot rise faster than 3 ms (brkdeglitch)
        double u = (d - atk) / dec; return u >= 1 ? 0.0 : std::pow (1 - u, cv);
    }, len);
}
// ENV CEILING: the envelope is forced to its maximum everywhere: all dynamics flattened. after CDP envel warp 14 (envfuncs.c:envceiling)
Chans warpCeiling (const Chans& ch, Ctx& cx, const Params& p)
{
    return warpWith (ch, cx, p, [] (DVec& e) { double m = maxOf (e); for (auto& v : e) v = m; });
}
// ENV DUCK: wherever the envelope exceeds THRESHOLD it is pulled down to the GATE level. after CDP envel warp 15 (envfuncs.c:envduck)
Chans warpDuck (const Chans& ch, Ctx& cx, const Params& p)
{
    const double th = p["thresh"], g = p["gate"];
    return warpWith (ch, cx, p, [&] (DVec& e) { for (auto& v : e) if (v > th) v = g; });
}

// DOVETAIL: fades in and out at the edges (linear or exponential, or doubly exponential).
// after CDP envel dovetail 1-2 (envprepro.c:create_initial_envelope_params, envel.c:apply_brkpnt_envelope)
Chans envDovetail (const Chans& ch, Ctx& cx, const Params& p)
{
    const double dur = (double) ch[0].size() / cx.sr; double fi = p["fin"], fo = p["fout"];
    if (fi + fo > dur) { double s = dur / std::max (1e-9, fi + fo); fi *= s; fo *= s; }
    const bool dbl = p.i ("mode") == 1; const int ti = dbl ? 3 : p.i ("intype"), to = dbl ? 3 : p.i ("outtype");
    std::vector<BP> b;
    if (fi > 0) { b.push_back ({ 0, 0, 0 }); b.push_back ({ fi, 1, ti }); } else b.push_back ({ 0, 1, 0 });
    if (fo > 0) { b.push_back ({ dur - fo, 1, 0 }); b.push_back ({ dur, 0, to }); }
    return applyBP (ch, cx, b);
}
// CURTAIL: fades to silence from FADE START (to an end time, over a duration, or to the end) and cuts the rest.
// after CDP envel curtail 1-6 (envprepro.c, envel.c:apply_brkpnt_envelope)
Chans envCurtail (const Chans& ch, Ctx& cx, const Params& p)
{
    const double dur = (double) ch[0].size() / cx.sr; const int m = p.i ("mode");
    const double st = clampd (p["fstart"], 0, 0.999) * dur; double en;
    switch (m % 3)
    {
        case 0: en = clampd (p["fend"], 0, 1) * dur; break;
        case 1: en = st + p["fdur"]; break;
        default: en = dur; break;
    }
    en = clampd (en, st + 1.0 / cx.sr * 8, dur);
    const int type = m >= 3 ? 3 : p.i ("envtype");
    std::vector<BP> b { { 0, 1, 0 }, { st, 1, 0 }, { en, 0, type } };
    return applyBP (ch, cx, b, (size_t) std::ceil (en * cx.sr));
}
// SWELL: fades in to a PEAK moment and out again. after CDP envel swell (envprepro.c, envel.c:apply_brkpnt_envelope)
Chans envSwell (const Chans& ch, Ctx& cx, const Params& p)
{
    const double dur = (double) ch[0].size() / cx.sr, pk = clampd (p["peak"], 0, 1) * dur; const int t = p.i ("type");
    std::vector<BP> b { { 0, 0, 0 }, { pk, 1, t }, { dur, 0, t } };
    if (pk <= 0) b.erase (b.begin());
    return applyBP (ch, cx, b);
}
// ATTACK: emphasises one attack (first over GATE, max near TIME, exactly at TIME, or the loudest point) by GAIN,
// with ONSET and DECAY ramps. after CDP envel attack 1-4 (envprepro.c:find_and_test_attack_peak, create_initial_envelope_params)
Chans envAttack (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const double dur = (double) len / sr; const int mode = p.i ("mode");
    auto lvl = [&] (size_t i) { double v = 0; for (auto& x : ch) v = std::max (v, (double) std::abs (x[i])); return v; };
    size_t at = 0; double mx = -1;
    auto maxIn = [&] (size_t a, size_t b) { size_t best = a; double bv = -1; for (size_t i = a; i < b && i < len; ++i) { double v = lvl (i); if (v > bv) { bv = v; best = i; } } return best; };
    if (mode == 0)
    {
        const double g = dbG (p["gate"]) * peakAll (ch); bool f = false;
        for (size_t i = 0; i < len; ++i) if (lvl (i) > g) { at = i; f = true; break; }
        if (! f) at = maxIn (0, len);
    }
    else if (mode == 1)
    {
        size_t c = (size_t) (clampd (p["time"], 0, 1) * (double) (len - 1)), r = msS (200, sr);
        at = maxIn (c > r ? c - r : 0, c + r + 1);
    }
    else if (mode == 2) at = (size_t) (clampd (p["time"], 0, 1) * (double) (len - 1));
    else at = maxIn (0, len);
    (void) mx;
    const double t = (double) at / sr, on = p["onset"] / 1000, dc = p["decay"] / 1000, gain = p["gain"];
    const int et = p.i ("envtype") ? 2 : 0;
    std::vector<BP> b;
    const double as = std::max (0.0, t - on), ae = std::min (dur, t + dc);
    if (t > 0) { b.push_back ({ 0, 1, 0 }); if (as > 0) b.push_back ({ as, 1, 0 }); b.push_back ({ t, gain, et }); }
    else b.push_back ({ 0, gain, 0 });
    if (ae < dur) { b.push_back ({ ae, 1, et }); b.push_back ({ dur, 1, 0 }); }
    else b.push_back ({ dur, 1, et });
    return applyBP (ch, cx, b);
}
// PLUCK: builds a plucked attack from CYCLES copies of the first stable wave cycle (found automatically at the first onset),
// with noise that dies away at rate DECAY, and puts it in front of the sound from that cycle on. after CDP envel pluck (pluck.c:pluck)
Chans envPluck (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const Buf mono = monoOf (ch);
    const double pk = peak (mono); size_t from = (size_t) (clampd (p["pos"], 0, 1) * (double) (len - 1)), on = from;
    for (size_t i = from; i < len; ++i) if (std::abs (mono[i]) > pk * 0.3) { on = i; break; }
    size_t z = std::min (len - 1, on + msS (15, sr));
    { size_t k = z; while (k + 1 < len && ! (mono[k] < 0 && mono[k + 1] >= 0)) ++k; if (k + 1 < len) z = k + 1; }
    double wl = 0;
    {   // wavelength: pitch tracker around z, else next upward zero crossing
        size_t a = z > msS (60, sr) ? z - msS (60, sr) : 0, b2 = std::min (len, z + msS (120, sr));
        Buf seg (mono.begin() + (long) a, mono.begin() + (long) b2);
        if (seg.size() > 64) { auto pt = trackPitch (seg, sr, 50, 2000, 0.2); double f = pt.at ((double) (z - a)); if (f > 0) wl = sr / f; }
        if (wl <= 0) { size_t k = z + 2; while (k + 1 < len && ! (mono[k] < 0 && mono[k + 1] >= 0)) ++k; wl = (double) (k + 1 - z); }
        wl = clampd (wl, sr / 2000, sr / 20);
    }
    const size_t WL = (size_t) std::max (2.0, std::round (wl)); const int cyc = std::max (2, p.i ("cycles")); const double decay = p["decay"];
    const size_t plen = std::min ((size_t) cyc * WL, cx.maxLen / 2), outLen = std::min (cx.maxLen, plen + (len - z));
    cx.seed (p["seed"]);
    Chans out (ch.size(), Buf (outLen));
    const size_t startup = std::min (plen, msS (4, sr));
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf& o = out[c];
        for (size_t j = 0; j < plen; ++j)
        {   // written backwards from the end of the pluck, like CDP: noise grows towards the start
            size_t inv = plen - 1 - j, src = z + WL - 1 - (inv % WL);
            double nl = std::pow ((double) inv / (double) plen, decay), s = src < len ? x[src] : 0.0;
            double v = s + (cx.rand() * 2 - 1) * nl * std::max (0.25, pk);
            if (j < startup) v *= (double) j / (double) startup;
            o[j] = (float) v;
        }
        for (size_t i = plen; i < outLen; ++i) o[i] = x[z + (i - plen)];
        cx.prog ((double) (c + 1) / (double) ch.size());
    }
    return out;
}
// tremolo LFO in CDP's form: ((sin + 1) / 2) * depth + (1 - depth); frequency swept FREQ -> END linearly or logarithmically
struct Trem
{
    double ph = 0, f0, f1, len, sr; bool lg;
    Trem (double a, double b, bool log, double n, double s) : f0 (a), f1 (b), len (std::max (1.0, n)), sr (s), lg (log) {}
    double next (double i)
    {
        double u = i / len, f = lg ? f0 * std::pow (std::max (0.01, f1) / std::max (0.01, f0), u) : f0 + (f1 - f0) * u;
        double v = (std::sin (TAU * ph) + 1) * 0.5; ph += f / sr; ph -= std::floor (ph); return v;
    }
};
// ENVEL TREMOLO: classic amplitude tremolo; FREQ glides to END FREQ (LINEAR or LOG), DEPTH and GAIN. after CDP envel tremolo 1-2 (envfuncs.c:envelope_tremol)
Chans envTremolo (const Chans& ch, Ctx& cx, const Params& p)
{
    Trem t (p["frq"], p["frq2"], p.i ("mode") == 1, (double) ch[0].size(), cx.sr); const double d = p["depth"], g = p["gain"];
    return applyGain (ch, cx, [&] (size_t i) { return (t.next ((double) i) * d + 1 - d) * g; });
}

// ===================================================================================================
// FILTER
// ===================================================================================================

// CDP's filter-bank resonator (ap_filter.c:get_coeffs1/2, filters0.c:filtering): y = x + a*y1 + b*y2 with
// r = exp(-w / 2Q), output scaled by (1 + b) * sin(w) (unity peak), optionally run twice (DOUBLE)
struct Bank
{
    struct F { double a = 0, b = 0, ampl = 0; };
    std::vector<F> f; bool dbl = false;
    void set (size_t k, double frq, double amp, double Q, double sr)
    {
        F& x = f[k];
        if (! (frq >= 10 && frq < sr * 0.45) || amp <= 0) { x = F(); return; }
        const double w = TAU * frq / sr, r = std::exp (-w / (2 * std::max (0.05, Q))), g = 1.0 / ((1 - r * r) * std::sin (w));
        x.a = 2 * r * std::cos (w); x.b = -r * r; x.ampl = amp / g; if (dbl) x.ampl /= g;
    }
};
// runs a bank over every channel; upd (i, bank) is called every 64 samples to (re)set the coefficients
// largest filter count that keeps a bank render within a fixed amount of work
inline size_t bankCap (const Chans& ch, const Ctx& cx, double tailSec, bool dbl)
{
    const double n = (double) std::min (cx.maxLen, ch[0].size() + (size_t) (std::max (0.0, tailSec) * cx.sr)) * (double) ch.size() * (dbl ? 2 : 1);
    return (size_t) clampd (2.5e8 / std::max (1.0, n), 1, 240);
}
template <typename Upd> Chans bankRun (const Chans& ch, Ctx& cx, size_t nf, bool dbl, double tailSec, Upd upd)
{
    const size_t len = ch[0].size(), outLen = std::min (cx.maxLen, len + (size_t) (std::max (0.0, tailSec) * cx.sr));
    Bank bk; bk.dbl = dbl; bk.f.resize (std::max<size_t> (1, nf));
    Chans out;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf o (outLen);
        DVec y (nf), z (nf), d (nf), e (nf);
        for (size_t i = 0; i < outLen; ++i)
        {
            if (! (i & 63)) { upd (i, bk); if (! (i & 32767)) cx.prog (((double) c + (double) i / (double) outLen) / (double) ch.size()); }
            const double in = (i < len ? x[i] : 0.0) + adn (i); double sum = 0;
            for (size_t k = 0; k < nf; ++k)
            {
                const Bank::F& F = bk.f[k]; if (F.ampl == 0) continue;
                double xx = in + F.a * y[k] + F.b * z[k]; z[k] = y[k]; y[k] = xx;
                if (dbl) { xx += F.a * d[k] + F.b * e[k]; e[k] = d[k]; d[k] = xx; }
                sum += xx * F.ampl;
            }
            o[i] = (float) sum;
        }
        out.push_back (std::move (o));
    }
    return out;
}

// pitch sets that replace CDP's filter data files
const defs::O CHORDS { "MAJOR", "MINOR", "MAJ7", "MIN7", "DOM7", "SUS4", "DIM7", "AUGMENTED", "FIFTHS", "OCTAVES", "CLUSTER", "PENTATONIC", "MAJOR SCALE", "MINOR SCALE", "WHOLE TONE", "HARMONICS" };
std::vector<double> chordIv (int c)
{
    static const std::vector<std::vector<double>> T { { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 4, 7, 10 }, { 0, 5, 7 }, { 0, 3, 6, 9 }, { 0, 4, 8 },
        { 0, 7 }, { 0 }, { 0, 1, 2, 3 }, { 0, 2, 4, 7, 9 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 4, 6, 8, 10 }, {} };
    return T[(size_t) clamp (c, 0, (int) T.size() - 1)];
}
// frequencies of a chord / scale on ROOT (midi) spread over OCT octaves; HARMONICS = harmonic series of the root
std::vector<double> chordHz (int chord, double root, int oct)
{
    std::vector<double> f; oct = clamp (oct, 1, 6);
    if (chord == 15) { for (int n = 1; n <= 6 * oct; ++n) f.push_back (midiHz (root) * n); return f; }
    for (int o = 0; o < oct; ++o) for (double iv : chordIv (chord)) f.push_back (midiHz (root + 12.0 * o + iv));
    return f;
}
const defs::O PROGS { "STATIC", "I-IV-V-I", "i-VI-III-VII", "ii-V-I", "RISING", "CIRCLE 5THS" };
const std::vector<double>& progSteps (int p)
{
    static const std::vector<std::vector<double>> T { { 0 }, { 0, 5, 7, 0 }, { 0, 8, 3, 10 }, { 2, 7, 0, 0 }, { 0, 2, 4, 5, 7, 9, 11, 12 }, { 0, 5, 10, 3, 8, 1, 6, 11 } };
    return T[(size_t) clamp (p, 0, (int) T.size() - 1)];
}
inline ParamDef QP (double def = 60) { return defs::P ("q", "Q", 1, 1000, def, PExp); }
inline ParamDef TailP (double def = 1) { return defs::P ("tail", "TAIL", 0, 10, def, 0, {}, Fmt::time); }
inline ParamDef DblP() { return defs::Opt ("dbl", "DOUBLE", { "OFF", "ON" }); }
// Q may sweep from Q to Q END (CDP: Q may vary over time), exponentially
struct QSweep { double q0, q1, n; double at (size_t i) const { return q0 * std::pow (q1 / q0, std::min (1.0, (double) i / n)); } };

// FIXED EQ: low shelf, high shelf or peaking band (BWIDTH Hz) with BOOST/CUT dB. after CDP filter fixed 1-3 (filters0.c:do_eq_filter, fltpcon.c:shelve/presence)
Chans filtFixed (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, f = clampd (p["frq"], 10, sr * 0.45), A = std::pow (10.0, p["boost"] / 40), w = TAU * f / sr, cs = std::cos (w), sn = std::sin (w);
    const int m = p.i ("mode"); double b0, b1, b2, a0, a1, a2;
    if (m == 2)
    {
        const double Q = f / std::max (1.0, p["bw"]), al = sn / (2 * Q);
        b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A;
    }
    else
    {
        const double al = sn / 2 * std::sqrt (2.0), sA = 2 * std::sqrt (A) * al;
        if (m == 0) { b0 = A * ((A + 1) - (A - 1) * cs + sA); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sA);
                      a0 = (A + 1) + (A - 1) * cs + sA; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sA; }
        else { b0 = A * ((A + 1) + (A - 1) * cs + sA); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sA);
               a0 = (A + 1) - (A - 1) * cs + sA; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sA; }
    }
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    const size_t len = ch[0].size(), outLen = std::min (cx.maxLen, len + (size_t) (p["tail"] * sr));
    Chans out;
    for (auto& x : ch)
    {
        Buf o (outLen); double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (size_t i = 0; i < outLen; ++i)
        {
            tick (cx, i, outLen); const double in = (i < len ? x[i] : 0.0) + adn (i), y = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in; y2 = y1; y1 = y; o[i] = (float) y;
        }
        out.push_back (std::move (o));
    }
    return out;
}
// LO/HI PASS: steep Butterworth low or high pass; the order follows from PASS band, STOP band and ATTENUATION as in CDP.
// after CDP filter lohi 1-2 (fltprepro.c:establish_order_of_filter, filters0.c:do_lphp_filter)
Chans filtLohi (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, nyq = sr / 2; const bool hp = p.i ("mode") == 1;
    double fp = clampd (p["pass"], 20, nyq * 0.95), fs = clampd (p["stop"], 20, nyq * 0.95);
    if (! hp && fs <= fp * 1.01) fs = std::min (nyq * 0.97, fp * 1.2);
    if (hp && fs >= fp / 1.01) fs = std::max (10.0, fp / 1.2);
    double tp = std::tan (PI * (hp ? nyq - fp : fp) / sr), tc = std::tan (PI * (hp ? nyq - fs : fs) / sr), tt = (tc / tp) * (tc / tp);
    double g = std::exp (std::abs (p["atten"]) * std::log (10.0) / 10.0) - 1.0, xx = std::log (std::max (1e-9, g)) / std::log (std::max (1.0001, tt));
    int order = (int) std::floor (xx) + 1; order = clamp (order, 2, 24); order += order & 1;
    std::vector<std::array<double, 5>> sec;
    const double w = TAU * fp / sr, cs = std::cos (w), sn = std::sin (w);
    for (int k = 1; k <= order / 2; ++k)
    {
        const double Q = 1.0 / (2 * std::cos (PI * (2 * k - 1) / (2.0 * order))), al = sn / (2 * Q), a0 = 1 + al;
        double b0 = hp ? (1 + cs) / 2 : (1 - cs) / 2, b1 = hp ? -(1 + cs) : 1 - cs;
        sec.push_back ({ b0 / a0, b1 / a0, b0 / a0, -2 * cs / a0, (1 - al) / a0 });
    }
    const size_t len = ch[0].size(), outLen = std::min (cx.maxLen, len + (size_t) (p["tail"] * sr));
    Chans out;
    for (auto& x : ch)
    {
        Buf o (outLen); std::vector<std::array<double, 4>> st (sec.size(), { 0, 0, 0, 0 });
        for (size_t i = 0; i < outLen; ++i)
        {
            tick (cx, i, outLen); double v = (i < len ? x[i] : 0.0) + adn (i);
            for (size_t s = 0; s < sec.size(); ++s)
            {
                auto& q = sec[s]; auto& z = st[s];
                double y = q[0] * v + q[1] * z[0] + q[2] * z[1] - q[3] * z[2] - q[4] * z[3];
                z[1] = z[0]; z[0] = v; z[3] = z[2]; z[2] = y; v = y;
            }
            o[i] = (float) v;
        }
        out.push_back (std::move (o));
    }
    return out;
}
// state-variable filter shared by VARIABLE and SWEEPING: mode 0 notch, 1 band, 2 low, 3 high; acuity = 1/Q
template <typename FrqAt, typename AcuAt> Chans svfRun (const Chans& ch, Ctx& cx, int mode, double tailSec, FrqAt frq, AcuAt acu)
{
    const size_t len = ch[0].size(), outLen = std::min (cx.maxLen, len + (size_t) (std::max (0.0, tailSec) * cx.sr));
    Chans out;
    for (auto& x : ch)
    {
        Buf o (outLen); SVF f;
        for (size_t i = 0; i < outLen; ++i)
        {
            tick (cx, i, outLen);
            f.run ((i < len ? x[i] : 0.0) + adn (i), frq (i), 1.0 / std::max (1e-4, acu (i)), cx.sr);
            o[i] = (float) (mode == 0 ? f.lp + f.hp : mode == 1 ? f.bp : mode == 2 ? f.lp : f.hp);
        }
        out.push_back (std::move (o));
    }
    return out;
}
// VARIABLE FILTER: notch / band / low / high pass whose FREQ glides (exponentially) to END FREQ and ACUITY to END ACUITY.
// after CDP filter variable 1-4 (filters0.c:do_varifilter, multifilter)
Chans filtVariable (const Chans& ch, Ctx& cx, const Params& p)
{
    const double n = std::max (1.0, (double) ch[0].size()), f0 = p["frq"], f1 = p["frq2"], a0 = p["acuity"], a1 = p["acuity2"];
    return svfRun (ch, cx, p.i ("mode"), p["tail"],
                   [&] (size_t i) { return f0 * std::pow (f1 / f0, std::min (1.0, (double) i / n)); },
                   [&] (size_t i) { return a0 * std::pow (a1 / a0, std::min (1.0, (double) i / n)); });
}
// SWEEPING FILTER: notch / band / low / high pass swept between LOW and HIGH by a cosine at SWEEP rate from PHASE (0 = low, 0.5 = high).
// after CDP filter sweeping 1-4 (filters0.c:do_sweep_filter, getfrq)
Chans filtSweep (const Chans& ch, Ctx& cx, const Params& p)
{
    const double lo = p["lo"], hi = p["hi"], acu = p["acuity"], sr = cx.sr;
    const double sw = p.i ("sync") > 0 ? 1.0 / divSec (p.i ("sync") - 1, cx.bpm) : p["sweep"];
    const double ph0 = std::fmod (p["phase"] + 0.5, 1.0) * TAU;
    return svfRun (ch, cx, p.i ("mode"), p["tail"],
                   [&] (size_t i) { double pos = (std::cos (ph0 + TAU * sw * (double) i / sr) + 1) * 0.5; return lo + (hi - lo) * pos; },
                   [&] (size_t) { return acu; });
}
// FILTER BANK: resonant band-pass filters on a harmonic, alternate-harmonic, subharmonic, offset-harmonic or equal-step series
// between LOW and HIGH, with Q (gliding to Q END), random SCATTER of the frequencies and DOUBLE filtering.
// after CDP filter bank 1-6 (fltprepro.c:calc_frqs, randomise_frqs, filters0.c:do_qvary_filters)
Chans filtBank (const Chans& ch, Ctx& cx, const Params& p)
{
    const int m = p.i ("mode"); double lo = p["lo"], hi = std::max (p["hi"], lo * 1.01);
    std::vector<double> f; const size_t MAXF = 200;
    switch (m)
    {
        case 0: for (int n = 1; lo * n < hi && f.size() < MAXF; ++n) f.push_back (lo * n); break;
        case 1: for (int n = 0; lo * (2 * n + 1) < hi && f.size() < MAXF; ++n) f.push_back (lo * (2 * n + 1)); break;
        case 2: for (int n = 1; hi / n > lo && f.size() < MAXF; ++n) f.push_back (hi / n); break;
        case 3: f.push_back (lo); for (int n = 2; lo * n + p["offset"] < hi && f.size() < MAXF; ++n) f.push_back (lo * n + p["offset"]); break;
        case 4: { int c = std::max (1, p.i ("count")); for (int n = 0; n < c; ++n) f.push_back (lo * std::pow (hi / lo, (double) n / c)); break; }
        default: { double r = std::pow (2.0, std::max (0.1, p["step"]) / 12); for (double x = lo; x < hi && f.size() < MAXF; x *= r) f.push_back (x); break; }
    }
    if (f.empty()) f.push_back (lo);
    f.resize (std::min (f.size(), bankCap (ch, cx, p["tail"], p.i ("dbl") == 1)));
    cx.seed (p["seed"]); const double sc = p["scat"];
    if (sc > 0)
        for (size_t n = 1; n + 1 < f.size(); ++n)
        {
            double r = cx.rand();
            if (r >= 0.5) f[n] *= std::exp (std::log (f[n + 1] / f[n]) * (r - 0.5) * sc);
            else f[n] = f[n - 1] * std::exp (std::log (f[n] / f[n - 1]) * (1 - r * sc));
        }
    const QSweep qs { p["q"], p["q2"], std::max (1.0, (double) ch[0].size()) }; const double sr = cx.sr;
    return bankRun (ch, cx, f.size(), p.i ("dbl") == 1, p["tail"], [&] (size_t i, Bank& b) { if ((i & 255) || (i > 0 && qs.q0 == qs.q1)) return; double q = qs.at (i); for (size_t k = 0; k < f.size(); ++k) b.set (k, f[k], 1, q, sr); });
}
// USER BANK: filter bank tuned to a CHORD or scale on ROOT over OCTAVES (instead of CDP's pitch/amp data file), TILT dB per note.
// after CDP filter userbank 1-2 (filters0.c:do_qvary_filters)
Chans filtUser (const Chans& ch, Ctx& cx, const Params& p)
{
    auto f = chordHz (p.i ("chord"), p["root"], p.i ("oct")); const double tilt = p["tilt"], sr = cx.sr;
    f.resize (std::min (f.size(), bankCap (ch, cx, p["tail"], p.i ("dbl") == 1)));
    const QSweep qs { p["q"], p["q2"], std::max (1.0, (double) ch[0].size()) };
    return bankRun (ch, cx, f.size(), p.i ("dbl") == 1, p["tail"], [&] (size_t i, Bank& b)
    { if ((i & 255) || (i > 0 && qs.q0 == qs.q1)) return; double q = qs.at (i); for (size_t k = 0; k < f.size(); ++k) b.set (k, f[k], dbG (-tilt * (double) k / std::max<size_t> (1, f.size() - 1)), q, sr); });
}
// chord frequencies at sample i for a tempo-synced progression with GLIDE into each change
struct ProgFrq
{
    std::vector<double> base; const std::vector<double>* steps; double stepLen, glide;
    double ratioAt (size_t i) const
    {
        const auto& s = *steps; const double pos = (double) i / stepLen; const size_t k = (size_t) pos; const double fr = pos - (double) k;
        double a = s[k % s.size()], b = s[(k + 1) % s.size()];
        double u = glide > 0 ? clampd ((fr - (1 - glide)) / glide, 0, 1) : 0;
        return std::pow (2.0, (a + (b - a) * u) / 12.0);
    }
};
// VARIBANK: a time-varying filter bank: the CHORD on ROOT moves through a PROGRESSION (one chord per RATE, GLIDE into each change);
// every note gets HARMONICS partials falling ROLLOFF dB each. after CDP filter varibank 1-2 (fltpcon.c, filters0.c:do_fvary_filters)
Chans filtVari (const Chans& ch, Ctx& cx, const Params& p)
{
    ProgFrq pf { chordHz (p.i ("chord"), p["root"], p.i ("oct")), &progSteps (p.i ("prog")), std::max (64.0, divSec (p.i ("div"), cx.bpm) * cx.sr), p["glide"] };
    const int hc = std::max (1, p.i ("hcnt")); const double ro = p["rolloff"], sr = cx.sr, q = p["q"];
    const size_t nf = std::min (bankCap (ch, cx, p["tail"], p.i ("dbl") == 1), pf.base.size() * (size_t) hc);
    double lastR = -1;
    return bankRun (ch, cx, nf, p.i ("dbl") == 1, p["tail"], [&] (size_t i, Bank& b)
    {
        const double r = pf.ratioAt (i);
        if (i == 0) lastR = -1;
        if (r == lastR) return;
        lastR = r;
        for (size_t k = 0; k < nf; ++k) { size_t note = k / (size_t) hc, h = k % (size_t) hc; b.set (k, pf.base[note] * r * (double) (h + 1), dbG (-ro * (double) h), q, sr); }
    });
}
// partial patterns that replace varibank2's partial-data lines
const defs::O PARTS { "HARMONIC", "ODD", "STRETCHED", "BELL", "BAR", "OCTAVES" };
double partialRatio (int pat, int n)   // n = 0..
{
    static const double bell[] = { 0.5, 1, 1.19, 1.5, 2, 2.52, 2.99, 4.05, 5.0, 6.1, 7.2, 8.4 };
    static const double bar[] = { 1, 2.76, 5.40, 8.93, 13.34, 18.64, 24.8, 31.9, 39.9, 48.8, 58.6, 69.3 };
    switch (pat)
    {
        case 0: return n + 1.0;
        case 1: return 2.0 * n + 1.0;
        case 2: return std::pow (n + 1.0, 1.1);
        case 3: return bell[clamp (n, 0, 11)];
        case 4: return bar[clamp (n, 0, 11)];
        default: return std::pow (2.0, n);
    }
}
// VARIBANK 2: filter bank on a CHORD whose every note carries a set of PARTIALS that morph from PATTERN A to PATTERN B over the length,
// with ROLLOFF dB per partial. after CDP filter varibank2 1-2 (filters0.c:do_fvary2_filters, newfval2)
Chans filtVari2 (const Chans& ch, Ctx& cx, const Params& p)
{
    const auto base = chordHz (p.i ("chord"), p["root"], p.i ("oct")); const int pc = std::max (1, p.i ("parts")), pa = p.i ("pata"), pb = p.i ("patb");
    const double ro = p["rolloff"], sr = cx.sr, q = p["q"], n = std::max (1.0, (double) ch[0].size());
    const size_t nf = std::min (bankCap (ch, cx, p["tail"], p.i ("dbl") == 1), base.size() * (size_t) pc);
    return bankRun (ch, cx, nf, p.i ("dbl") == 1, p["tail"], [&] (size_t i, Bank& b)
    {
        if (i & 255) return;
        const double u = std::min (1.0, (double) i / n);
        for (size_t k = 0; k < nf; ++k)
        {
            int h = (int) (k % (size_t) pc); double ra = partialRatio (pa, h), rb = partialRatio (pb, h);
            b.set (k, base[k / (size_t) pc] * (ra + (rb - ra) * u), dbG (-ro * h), q, sr);
        }
    });
}
// ITERATED FILTER: the sound is repeated every DELAY (RANDOM scatter) for LENGTH, each repeat filtered again by a CHORD filter bank
// (cumulative), re-normalised, randomly PITCH-shifted and AMP-reduced, optionally DECAYing. after CDP filter iterated 1-2 (filters1.c:iterating_filter)
Chans filtIter (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(), nc = ch.size();
    const auto f = chordHz (p.i ("chord"), p["root"], p.i ("oct")); const double q = p["q"];
    size_t step = std::max<size_t> (16, (size_t) std::round (p["delay"] * sr)); const size_t dur = std::max (len, (size_t) (p["len"] * sr));
    const bool dbl = p.i ("dbl") == 1, norm = p.i ("norm") == 0, dec = p.i ("decay") == 1;
    const size_t outCap = std::min (cx.maxLen, dur + 4 * len + 16);
    step = std::max (step, (size_t) ((double) std::min (dur, outCap) * (double) len * (double) nc / 4e7));   // bounded amount of overlap work
    Chans out (nc, Buf (outCap)); size_t outEnd = len;
    for (size_t c = 0; c < nc; ++c) std::copy (ch[c].begin(), ch[c].end(), out[c].begin());
    const double origMax = std::max (1e-9, peakAll (ch));
    const int maxOv = std::max (2, (int) std::round ((double) len / (double) step + 1));
    const double budget = 1e8, perPass = (double) len * (double) nc * (double) f.size() * (dbl ? 2 : 1);
    const int maxFilt = std::max (1, (int) (budget / std::max (1.0, perPass)));
    const size_t spl = std::min (len / 2, msS (15, sr));
    cx.seed (p["seed"]);
    Chans cur = ch; Bank bk; bk.dbl = dbl; bk.f.resize (f.size());
    for (size_t k = 0; k < f.size(); ++k) bk.set (k, f[k], 1, q, sr);
    size_t ws = 0; int it = 0;
    auto nextWs = [&] (size_t w) { double s = 1 + (cx.rand() * 2 - 1) * p["rand"]; return w + (size_t) std::max (1.0, std::round ((double) step * s)); };
    ws = nextWs (ws);
    while (ws < dur && ws < outCap)
    {
        ++it;
        cx.prog ((double) ws / (double) dur);
        double ps = (cx.rand() * 2 - 1) * p["pitch"] / 12.0, ratio = std::pow (2.0, ps);
        double g = 1 - cx.rand() * p["amp"];
        if (it <= maxFilt)
        {   // filter the previous filtered version again (state reset each pass)
            for (size_t c = 0; c < nc; ++c)
            {
                Buf& x = cur[c]; DVec y (f.size()), z (f.size()), d (f.size()), e (f.size());
                for (size_t i = 0; i < len; ++i)
                {
                    double in = x[i] + adn (i), sum = 0;
                    for (size_t k = 0; k < f.size(); ++k)
                    {
                        const auto& F = bk.f[k]; if (F.ampl == 0) continue;
                        double xx = in + F.a * y[k] + F.b * z[k]; z[k] = y[k]; y[k] = xx;
                        if (dbl) { xx += F.a * d[k] + F.b * e[k]; e[k] = d[k]; d[k] = xx; }
                        sum += xx * F.ampl;
                    }
                    x[i] = (float) sum;
                }
            }
            if (norm) { double m = peakAll (cur); if (m > 1e-12) for (auto& x : cur) for (auto& v : x) v = (float) (v * origMax / m); }
            else { double m = peakAll (cur); if (m > 1e6) for (auto& x : cur) for (auto& v : x) v = (float) (v / m); }
        }
        double att = dec ? std::pow (1.0 / maxOv, (double) std::min (it, maxOv) / (double) (maxOv - 1)) : 1.0;
        const size_t segLen = (size_t) std::floor ((double) len / ratio);
        const double dstep = dec ? std::pow (1.0 / maxOv, ratio / (double) std::max<size_t> (1, len)) : 1.0;   // in-segment exponential decay
        for (size_t c = 0; c < nc; ++c)
        {
            double dg = g * att;
            for (size_t j = 0; j < segLen && ws + j < outCap; ++j, dg *= dstep)
            {
                double sp = (double) j * ratio, v = lerpAt (cur[c], sp);
                size_t fromEnd = len - std::min (len, (size_t) sp);
                if (fromEnd < spl) v *= (double) fromEnd / (double) spl;   // end splice
                out[c][ws + j] = (float) (out[c][ws + j] + v * dg);
            }
        }
        outEnd = std::max (outEnd, std::min (outCap, ws + segLen));
        ws = nextWs (ws);
    }
    for (auto& x : out) x.resize (outEnd);
    return out;
}
// PHASING: allpass phase shift (ALLPASS) or phasing (allpass mixed with the dry sound) with GAIN -1..1 and a DELAY that glides
// from DELAY to END DELAY (LOG or LINEAR). after CDP filter phasing 1-2 (filters0.c:do_allpass_filter, varidelay_allpass)
Chans filtPhasing (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, g = p["gain"], d0 = p["delay"] / 1000 * sr, d1 = p["delay2"] / 1000 * sr; const bool lin = p.i ("interp") == 1, ph = p.i ("mode") == 1;
    const size_t len = ch[0].size(), outLen = std::min (cx.maxLen, len + (size_t) (p["tail"] * sr)); const double n = std::max (1.0, (double) len);
    Chans out;
    for (auto& x : ch)
    {
        Ring ri ((size_t) std::max (d0, d1) + 8), ro ((size_t) std::max (d0, d1) + 8); Buf o (outLen);
        for (size_t i = 0; i < outLen; ++i)
        {
            tick (cx, i, outLen);
            const double u = std::min (1.0, (double) i / n), d = std::max (1.0, lin ? d0 + (d1 - d0) * u : std::max (1.0, d0) * std::pow (std::max (1.0, d1) / std::max (1.0, d0), u));
            const double ip = (i < len ? x[i] : 0.0) + adn (i), op = -g * ip + ri.read (d) + g * ro.read (d);
            ri.push (ip); ro.push (op);
            o[i] = (float) (ph ? (op + ip) * 0.5 : op);
        }
        out.push_back (std::move (o));
    }
    return out;
}

// ===================================================================================================
// DYNAMICS
// ===================================================================================================

// peaks of the loudness contour (10 ms windows): local maxima above THRESH * loudest, at least GAP apart; returns sample positions
std::vector<size_t> findPeaks (const Chans& ch, double sr, double thresh, double gapSec)
{
    const size_t W = msS (10, sr), len = ch[0].size(); const DVec e = winEnv (ch, W);
    const double mx = maxOf (e), thr = std::max (1e-6, thresh * mx); const long long g = std::max (1LL, (long long) (gapSec * sr / (double) W) / 2), n = (long long) e.size();
    std::vector<size_t> pk;
    for (long long k = 0; k < n; ++k)
    {
        if (e[(size_t) k] < thr) continue;
        bool best = true;
        for (long long j = std::max (0LL, k - g); j <= std::min (n - 1, k + g) && best; ++j)
            if (e[(size_t) j] > e[(size_t) k] || (e[(size_t) j] == e[(size_t) k] && j < k)) best = false;
        if (! best) continue;
        size_t a = (size_t) k * W, b = std::min (len, a + W), at = a; double bv = -1;
        for (size_t i = a; i < b; ++i) for (auto& x : ch) if (std::abs (x[i]) > bv) { bv = std::abs (x[i]); at = i; }
        pk.push_back (at);
    }
    return pk;
}

// LEVEL / PHASE: change the level by a GAIN factor or dB, NORMALISE (only up), FORCE LEVEL (up or down) to LEVEL, or INVERT the phase.
// after CDP modify loudness 1-4, 6 (gain.c:loudness_process)
Chans dynLoud (const Chans& ch, Ctx& cx, const Params& p)
{
    const int m = p.i ("mode"); const double pk = peakAll (ch), lv = p["level"]; double g = 1;
    switch (m)
    {
        case 0: g = p["gain"]; break;
        case 1: g = dbG (p["db"]); break;
        case 2: g = pk > 0 && pk < lv ? lv / pk : 1; break;
        case 3: g = pk > 0 ? lv / pk : 1; break;
        default: g = -1; break;
    }
    return applyGain (ch, cx, [g] (size_t) { return g; });
}

// SHUDDER: random stereo amplitude bursts from START at an average FREQ (SCATTER in time), each with a random DEPTH and WIDTH
// within the ranges, placed alternately left and right by SPREAD. after CDP modify shudder (pan.c:do_shudder)
Chans dynShudder (const Chans& ch, Ctx& cx, const Params& p)
{
    const Chans st = stereoOf (ch); const double sr = cx.sr, dur = (double) st[0].size() / sr;
    static const double EE[8] = { -7, -5, -3, -1, 1, 3, 5, 7 }, FF[8] = { 0, 0.2, 0.8, 1.0, 0.8, 0.2, 0.1, 0 };
    struct Ev { double t, w, om, l[2]; };
    std::vector<Ev> ev; cx.seed (p["seed"]);
    const double frq = std::max (0.05, p["frq"]), wmin = std::min (p["minw"], p["maxw"]), wmax = std::max (p["minw"], p["maxw"]);
    const double dmin = std::min (p["mind"], p["maxd"]), dmax = std::max (p["mind"], p["maxd"]);
    double left = cx.rand() * 2 - 1 <= 0 ? 1 : -1, maxl = 1e-9, last = -1;
    auto make = [&] (double t, double w)
    {
        Ev e; e.t = t; e.w = w; double pos = p["spread"] * cx.rand() * left; left = -left;
        auto g = panG ((pos + 1) * 0.5); double d = dmin + (dmax - dmin) * cx.rand();
        e.om = 1 - d; e.l[0] = d * g.first; e.l[1] = d * g.second; maxl = std::max ({ maxl, e.l[0], e.l[1] }); ev.push_back (e);
    };
    double t = clampd (p["start"], 0, 1) * dur, w = wmin + (wmax - wmin) * cx.rand();
    t = std::max (t, w / 2); make (t, w); last = t;
    while (t < dur && ev.size() < 200000)
    {
        double lt = t; t += 1 / frq; double nt = t + 1 / frq, sc = (cx.rand() * 2 - 1) * p["scat"];
        double rt = sc >= 0 ? (nt - t) * sc + t : t - (t - lt) * sc;
        w = wmin + (wmax - wmin) * cx.rand();
        if (rt <= w / 2 || rt <= last) continue;
        last = rt; make (rt, w);
    }
    for (auto& e : ev) { e.l[0] /= maxl; e.l[1] /= maxl; }
    const size_t len = st[0].size(); Chans out (2, Buf (len));
    for (int c = 0; c < 2; ++c)
    {
        size_t k = 0;   // first event that may still matter
        for (size_t i = 0; i < len; ++i)
        {
            tick (cx, i, len, c * 0.5, c * 0.5 + 0.5);
            const double s = (double) i / sr;
            while (k + 1 < ev.size() && ev[k].t + ev[k].w / 2 < s) ++k;
            double v = -1;
            for (size_t j = k; j < ev.size() && ev[j].t - ev[j].w / 2 <= s; ++j)
            {
                const Ev& e = ev[j]; double u = (s - e.t) / e.w * 14.0;   // -7..7
                if (u < -7 || u > 7) continue;
                int q = clamp ((int) std::floor ((u + 7) / 2), 0, 6); double f = (u - EE[q]) / 2;
                v = std::max (v, e.l[c] * (FF[q] + (FF[q + 1] - FF[q]) * f) + e.om);
            }
            if (v < 0)
            {   // between bursts: glide from the previous burst's floor to the next one's
                const Ev* a = nullptr; const Ev* b = nullptr;
                for (size_t j = k; j < ev.size(); ++j) { if (ev[j].t <= s) a = &ev[j]; else { b = &ev[j]; break; } }
                if (! a && k > 0) a = &ev[k - 1];
                if (a && b) { double ta = a->t + a->w / 2, tb = b->t - b->w / 2; v = tb > ta ? a->om + (b->om - a->om) * clampd ((s - ta) / (tb - ta), 0, 1) : b->om; }
                else v = a ? a->om : b ? b->om : 1;
            }
            out[(size_t) c][i] = (float) (st[(size_t) c][i] * v);
        }
    }
    return out;
}

// DEGLITCH: finds short bursts (shorter than GLITCH) with near-silence (below THRESH for SILENCE) either side, judged over WINDOW,
// and cuts them out with SPLICE fades. after CDP housekeep deglitch (clean.c:house_gate2)
Chans dynDeglitch (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, lvl = dbG (p["thresh"]); const size_t len = ch[0].size(), nc = ch.size();
    const size_t filt = msS (p["window"], sr), zeros = msS (p["sil"], sr), gdur = msS (p["glitch"], sr), spl = std::min (msS (p["splice"], sr), std::max<size_t> (1, zeros / 2));
    std::vector<std::pair<size_t, size_t>> cuts;
    DVec ring (filt, 0.0); double rsum = 0; size_t ri = 0, cnt = 0, zg = 0, nz = 0, possible = 0, sc = 0, ec = 0; int state = 0;
    for (size_t i = 0; i < len; ++i)
    {
        tick (cx, i, len, 0, 0.5);
        double s = 0; for (auto& x : ch) s += std::abs (x[i]); s /= (double) nc;
        rsum += s - ring[ri]; ring[ri] = s; ri = (ri + 1) % filt; cnt = std::min (cnt + 1, filt);
        bool zero = rsum / (double) cnt <= lvl;
        if (s > lvl) { possible = i; zero = false; }
        if (zero)
        {
            nz = 0; ++zg;
            if (state == 0 && zg >= zeros) state = 1;
            else if (state == 2) { ec = i; state = 3; }
            else if (state == 3 && zg >= zeros) { cuts.push_back ({ sc, ec }); state = 1; }
        }
        else
        {
            ++nz; zg = 0;
            if (state == 1) { sc = possible; nz += i - possible; state = 2; }
            else if (state == 2 && nz > gdur) state = 0;
            else if (state == 3) state = 0;
        }
    }
    if (state == 3) cuts.push_back ({ sc, len });
    DVec g (len, 1.0);
    for (auto& c : cuts)
    {
        size_t a = c.first > spl ? c.first - spl : 0, b = c.first, e = c.second, f = std::min (len, c.second + spl);
        for (size_t i = a; i < b; ++i) g[i] = std::min (g[i], 1.0 - (double) (i - a + 1) / (double) std::max<size_t> (1, b - a));
        for (size_t i = b; i < e; ++i) g[i] = 0;
        for (size_t i = e; i < f; ++i) g[i] = std::min (g[i], (double) (i - e + 1) / (double) std::max<size_t> (1, f - e));
    }
    return applyGain (ch, cx, [&] (size_t i) { return g[i]; });
}

// CONSTRICT: shortens the silent gaps (below THRESHOLD for at least MIN GAP) by CONSTRICTION %; above 100 % the sounds either side
// start to overlap. after CDP constrict (constrict.c:constrict)
Chans dynConstrict (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, thr = dbG (p["thresh"]), con = p["con"]; const size_t len = ch[0].size(), nc = ch.size(), mg = msS (p["mingap"], sr), fd = msS (2, sr);
    std::vector<std::pair<size_t, size_t>> gaps;   // quiet runs
    for (size_t i = 0; i < len;)
    {
        auto q = [&] (size_t k) { for (auto& x : ch) if (std::abs (x[k]) > thr) return false; return true; };
        if (! q (i)) { ++i; continue; }
        size_t j = i; while (j < len && q (j)) ++j;
        if (j - i >= mg) gaps.push_back ({ i, j });
        i = j;
    }
    Chans out (nc, Buf (std::min (cx.maxLen, len + 16)));
    size_t wp = 0, rp = 0, end = 0;
    auto copy = [&] (size_t a, size_t b, size_t fadeOutAt)
    {
        for (size_t i = a; i < b && wp < out[0].size(); ++i, ++wp)
        {
            double g = 1; if (fadeOutAt < b && i >= fadeOutAt) g = (double) (b - i) / (double) (b - fadeOutAt);
            for (size_t c = 0; c < nc; ++c) out[c][wp] = (float) (out[c][wp] + ch[c][i] * g);
        }
        end = std::max (end, wp);
    };
    for (auto& gp : gaps)
    {
        cx.prog ((double) gp.first / (double) std::max<size_t> (1, len));
        copy (rp, gp.first, len);
        const size_t L = gp.second - gp.first;
        if (con <= 100)
        {
            size_t keep = (size_t) std::round ((double) L * (1 - con / 100.0));
            copy (gp.first, gp.first + keep, gp.first + keep > fd ? gp.first + keep - std::min (fd, keep) : gp.first);
        }
        else
        {
            size_t back = (size_t) std::round ((double) L * std::min (1.0, (con - 100) / 100.0));
            wp = wp > back ? wp - back : 0;
        }
        rp = gp.second;
    }
    copy (rp, len, len);
    for (auto& x : out) x.resize (std::max<size_t> (1, end));
    return out;
}

// EXP DECAY: an exponential decay to (near) silence from START to END; the sound stops at END. after CDP envnu expdecay (envnu.c:expdecay)
Chans dynExpDecay (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t len = ch[0].size(); size_t s = (size_t) (clampd (p["start"], 0, 1) * (double) len), e = (size_t) (clampd (p["end"], 0, 1) * (double) len);
    if (e <= s + 16) e = std::min (len, s + 16);
    const double mlt = std::pow (0.00001, 1.0 / (double) std::max<size_t> (1, e - s));
    return applyGain (ch, cx, [&] (size_t i) { return i < s ? 1.0 : std::pow (mlt, (double) (i - s)); }, std::max (e, (size_t) 1));
}

// GATE: removes sound below LEVEL (every channel), replacing it by SILENCE or EDITING it OUT (shorter result), with 1 ms splices.
// after CDP gate gate 1-2 (gate.c:gate)
Chans dynGate (const Chans& ch, Ctx& cx, const Params& p)
{
    const double lv = dbG (p["level"]); const size_t len = ch[0].size(), sp = msS (1, cx.sr); const bool edit = p.i ("mode") == 1;
    std::vector<std::pair<size_t, size_t>> cuts;
    auto q = [&] (size_t k) { for (auto& x : ch) if (std::abs (x[k]) >= lv) return false; return true; };
    for (size_t i = 0; i < len;)
    {
        if (! q (i)) { ++i; continue; }
        size_t j = i; while (j < len && q (j)) ++j;
        size_t L = j - i;
        if ((i == 0 || j == len) ? L > sp : L > 2 * sp) cuts.push_back ({ i, j });
        i = j;
    }
    DVec g (len, 1.0); std::vector<uint8_t> drop (len, 0);
    for (auto& c : cuts)
    {
        size_t a = c.first, b = c.second, da = a == 0 ? a : a + sp, db = b == len ? b : b - sp;
        for (size_t i = a; i < da; ++i) g[i] = 1 - (double) (i - a + 1) / (double) sp;
        for (size_t i = db; i < b; ++i) g[i] = (double) (i - db + 1) / (double) sp;
        for (size_t i = da; i < db; ++i) { g[i] = 0; drop[i] = 1; }
    }
    if (! edit) return applyGain (ch, cx, [&] (size_t i) { return g[i]; });
    Chans out (ch.size());
    for (size_t c = 0; c < ch.size(); ++c) for (size_t i = 0; i < len; ++i) if (! drop[i]) out[c].push_back ((float) (ch[c][i] * g[i]));
    if (out[0].empty()) for (auto& x : out) x.assign (16, 0.0f);
    return out;
}

// SPIKE: envelopes the sound into sharp spikes at the peak(s) (loudest point, TIME, or every detected peak) with power-law slopes
// UPSLOPE / DOWNSLOPE, approaches limited to MAX UP / MAX DOWN; silence elsewhere. NORMALISE raises every spike to the loudest.
// after CDP spike spike (spike.c:spike)
Chans dynSpike (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const int m = p.i ("mode");
    std::vector<size_t> pk;
    if (m == 2) pk = findPeaks (ch, sr, p["thresh"], p["gap"] / 1000);
    if (m == 1) pk.push_back ((size_t) (clampd (p["time"], 0, 1) * (double) (len - 1)));
    if (pk.empty()) { size_t at = 0; double bv = -1; for (size_t i = 0; i < len; ++i) for (auto& x : ch) if (std::abs (x[i]) > bv) { bv = std::abs (x[i]); at = i; } pk.push_back (at); }
    const size_t mu = p["maxup"] > 0 ? msS (p["maxup"] * 1000, sr) : len, md = p["maxdown"] > 0 ? msS (p["maxdown"] * 1000, sr) : len;
    struct Sp { size_t a, k, b; double norm; };
    std::vector<Sp> sp;
    const size_t W = msS (10, sr); const DVec e = winEnv (ch, W);
    for (size_t n = 0; n < pk.size(); ++n)
    {   // troughs: envelope minimum between neighbouring peaks (or the file edges)
        size_t lo = n == 0 ? 0 : pk[n - 1], hi = n + 1 < pk.size() ? pk[n + 1] : len;
        auto trof = [&] (size_t a, size_t b)
        {
            if (a == 0 && n == 0) return (size_t) 0;
            if (b == len) return len;
            size_t best = (a + b) / 2; double bv = 1e30;
            for (size_t k = a / W; k <= b / W && k < e.size(); ++k) if (e[k] < bv) { bv = e[k]; best = k * W + W / 2; }
            return clamp (best, a, b);
        };
        size_t a = n == 0 ? 0 : trof (lo, pk[n]), b = n + 1 < pk.size() ? trof (pk[n], hi) : len;
        a = std::max (a, pk[n] > mu ? pk[n] - mu : 0); b = std::min (b, pk[n] + md);
        sp.push_back ({ a, pk[n], std::max (b, pk[n] + 1), 1.0 });
    }
    const double up = p["up"], dn = p["down"];
    auto shape = [&] (const Sp& s, size_t i)
    {
        if (i < s.a || i >= s.b) return 0.0;
        if (i < s.k) return std::pow ((double) (i - s.a) / (double) std::max<size_t> (1, s.k - s.a), up);
        return std::pow (1 - (double) (i - s.k) / (double) std::max<size_t> (1, s.b - s.k), dn);
    };
    if (p.i ("norm") == 1)
    {
        DVec mx (sp.size(), 0.0); double all = 0;
        for (size_t n = 0; n < sp.size(); ++n)
            for (size_t i = sp[n].a; i < sp[n].b && i < len; ++i) { double g = shape (sp[n], i); for (auto& x : ch) mx[n] = std::max (mx[n], std::abs (x[i]) * g); }
        for (double v : mx) all = std::max (all, v);
        for (size_t n = 0; n < sp.size(); ++n) sp[n].norm = mx[n] > 1e-9 ? std::min (100.0, all / mx[n]) : 1.0;
    }
    size_t k = 0;
    return applyGain (ch, cx, [&] (size_t i)
    {
        while (k + 1 < sp.size() && i >= sp[k].b) ++k;
        double g = 0;
        for (size_t j = (k > 0 ? k - 1 : 0); j < std::min (sp.size(), k + 2); ++j) g = std::max (g, shape (sp[j], i) * sp[j].norm);
        return g;
    });
}

// CANTOR HOLES: outputs a sequence of copies of the sound; in each, a hole in the middle third (HOLE SIZE % or fixed HOLE DUR) is dug
// a little deeper (HOLE DIG per copy, SPLICE wide) and once it is deeper than TRIGGER the remaining two sides get holes of their own,
// and so on: a fractal gating. after CDP cantor set 1-2 (cantor.c:cantor, cut_hole)
Chans dynCantor (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(), nc = ch.size(); const bool fixed = p.i ("mode") == 1;
    const int steps = (int) std::ceil (1.0 / std::max (0.001, p["dig"]));
    size_t splen = msS (p["splice"], sr); const size_t dig = std::max<size_t> (1, (splen + (size_t) steps - 1) / (size_t) steps); splen = dig * (size_t) steps;
    const double hole = p["holesize"] / 100.0, trig = p["trig"]; const size_t hdur = msS (p["holedur"] * 1000, sr);
    const size_t maxOut = std::min (cx.maxLen, std::max (len, (size_t) (p["maxdur"] * sr)));
    std::vector<int> digs (64, 0);
    Chans out (nc); DVec g (len);
    for (int copy = 0; copy < 100000; ++copy)
    {
        std::fill (g.begin(), g.end(), 1.0);
        std::vector<uint8_t> used (64, 0); bool progress = false;
        std::function<void (size_t, size_t, int)> cut = [&] (size_t a, size_t b, int L)
        {
            if (L >= 63 || b <= a) return;
            size_t hs, he, seg = b - a;
            if (! fixed) { hs = a + (size_t) std::round ((double) seg * (1 - hole) / 2); he = a + (size_t) std::round ((double) seg * (1 + hole) / 2); }
            else { if (seg <= hdur) return; hs = a + seg / 2 - hdur / 2; he = hs + hdur; }
            if (he <= hs || he - hs <= 2 * splen) return;
            used[(size_t) L] = 1;
            const int d = digs[(size_t) L] + 1; const size_t dm = std::min (splen, (size_t) d * dig);
            if (dm < splen) progress = true;
            const double inc = 1.0 / (double) splen, centre = std::max (0.0, 1.0 - (double) dm * inc);
            for (size_t i = hs; i < he; ++i)
            {
                double v = centre;
                if (i - hs < dm) v = std::max (centre, 1.0 - (double) (i - hs + 1) * inc);
                if (he - 1 - i < dm) v = std::max (centre, 1.0 - (double) (he - i) * inc);
                g[i] = v;
            }
            if (centre < trig) { cut (a, hs, L + 1); cut (he, b, L + 1); }
        };
        cut (0, len, 0);
        for (size_t L = 0; L < 64; ++L) if (used[L]) { if (digs[L] == 0 && L > 0) progress = true; digs[L]++; }
        for (size_t c = 0; c < nc; ++c)
            for (size_t i = 0; i < len && out[c].size() < maxOut; ++i) out[c].push_back ((float) (ch[c][i] * g[i]));
        cx.prog ((double) out[0].size() / (double) maxOut);
        if (! progress || out[0].size() >= maxOut) break;
    }
    return out;
}

// CANTOR WOBBLE: copies of the sound with superimposed vibrato-like envelope layers (LAYERS, each LAYER DECAY shallower and
// faster) that deepen over DIG STEPS copies down to HOLE LEVEL. after CDP cantor set 3 (cantor.c:woblwobl)
Chans dynWobble (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t len = ch[0].size(), nc = ch.size(); const int layers = std::max (1, p.i ("layers")), steps = std::max (1, p.i ("steps"));
    const double maxat = 1 - p["holelev"], dec = p["layerdec"];
    const size_t maxOut = std::min (cx.maxLen, std::max (len, (size_t) (p["maxdur"] * cx.sr)));
    Chans out (nc); DVec g (len);
    const size_t TS = 4096; std::vector<DVec> tabs ((size_t) layers, DVec (TS + 1));   // inverted-cosine tables, as CDP's
    for (int t = 0; t < layers; ++t)
        for (size_t k = 0; k <= TS; ++k) tabs[(size_t) t][k] = std::pow (1 - (std::cos (TAU * (double) k / (double) TS) + 1) * 0.5, t < layers - 1 ? (double) (layers - t) : 1.0);
    for (int w = 0; out[0].size() < maxOut; ++w)
    {
        std::fill (g.begin(), g.end(), 1.0); double factor = 1;
        for (int t = 0; t < layers; ++t)
        {
            const double fac = 2.0 * t + 1; const DVec& tb = tabs[(size_t) t];
            double iv = std::min (1.0, (std::pow (2.0, (double) w / steps) - 1) * factor) * maxat;
            if (iv > 0)
                for (size_t i = 0; i < len; ++i)
                {
                    double ph = (double) i / (double) len * fac; ph = (ph - std::floor (ph)) * (double) TS;
                    size_t k = std::min (TS - 1, (size_t) ph); double tab = tb[k] + (tb[k + 1] - tb[k]) * (ph - (double) k);
                    g[i] *= 1 - tab * iv;
                }
            factor *= dec;
        }
        for (size_t c = 0; c < nc; ++c) for (size_t i = 0; i < len && out[c].size() < maxOut; ++i) out[c].push_back ((float) (ch[c][i] * g[i]));
        cx.prog ((double) out[0].size() / (double) maxOut);
    }
    return out;
}

// SHARP TREMOLO: tremolo whose peaks are narrowed by FINENESS (the LFO raised to that power); FREQ glides to END FREQ, LINEAR or LOG.
// after CDP tremolo tremolo 1-2 (tremolo.c:tremolo)
Chans dynTremolo (const Chans& ch, Ctx& cx, const Params& p)
{
    Trem t (p["frq"], p["frq2"], p.i ("mode") == 1, (double) ch[0].size(), cx.sr); const double d = p["depth"], g = p["gain"]; const int fn = std::max (1, p.i ("fine"));
    return applyGain (ch, cx, [&] (size_t i) { return std::pow (t.next ((double) i) * d + 1 - d, fn) * g; });
}
// DECAY TREMOLO: the same sharp tremolo, starting only after the loudness peak (found with WINDOW ms), so the attack stays intact.
// after CDP tremenv (tremenv.c:trem_env)
Chans dynTremEnv (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t W = msS (p["wsize"], cx.sr), len = ch[0].size(); const DVec e = winEnv (ch, W);
    size_t pk = 0; for (size_t k = 0; k < e.size(); ++k) if (e[k] > e[pk]) pk = k;
    size_t at = std::min (len ? len - 1 : 0, (pk + 2) * W);
    Trem t (p["frq"], p["frq"], false, (double) len, cx.sr); const double d = p["depth"]; const int fn = std::max (1, p.i ("fine"));
    return applyGain (ch, cx, [&] (size_t i) { return i < at ? 1.0 : std::pow (t.next ((double) (i - at)) * d + 1 - d, fn); });
}

// LEVELLER: cuts the sound into elements (syllables, hits) of about ELEMENT size at envelope troughs and brings every element to the
// level of the loudest (at most MAX BOOST), with SHOULDER ramps between levels; the last TAIL is one element.
// after CDP flatten flatten (flatten.c:flatten)
Chans dynFlatten (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const Buf mono = monoOf (ch);
    const size_t W = std::max<size_t> (8, msS (p["elem"] * 1000 / 3, sr)), dove = msS (p["shoulder"], sr);
    const size_t tailAt = len - std::min (len, (size_t) (p["tail"] * (double) len));
    DVec env; for (size_t a = 0; a < len; a += W) { double m = 0; for (size_t i = a; i < std::min (len, a + W); ++i) m = std::max (m, (double) std::abs (mono[i])); env.push_back (m); }
    std::vector<size_t> tr { 0 };
    {
        size_t n = 0; double last = -1; const size_t ne = env.size();
        for (;;)
        {
            while (n < ne && env[n] >= last) last = env[n++];
            if (n >= ne) break;
            while (n < ne && env[n] <= last) last = env[n++];
            if (n >= ne) break;
            tr.push_back (n - 1);
        }
    }
    std::vector<size_t> cut { 0 };
    for (size_t t = 1; t < tr.size(); ++t)
    {   // refine: quietest third of the window, then the nearest zero crossing in it
        size_t a = tr[t] * W; if (a >= tailAt) break;
        size_t sub = std::max<size_t> (1, W / 3), best = a; double bv = 1e30;
        for (size_t s = 0; s < 3; ++s) { double m = 0; for (size_t i = a + s * sub; i < std::min (len, a + (s + 1) * sub); ++i) m = std::max (m, (double) std::abs (mono[i])); if (m < bv) { bv = m; best = a + s * sub; } }
        size_t z = best + sub / 2;
        for (size_t i = best; i + 1 < std::min (len, best + sub); ++i) if ((mono[i] <= 0) != (mono[i + 1] <= 0)) { z = i + 1; break; }
        if (z > cut.back() + 8 && z < len) cut.push_back (z);
    }
    const size_t ns = cut.size(); DVec mx (ns, 0.0); std::vector<size_t> pl (ns, 0);
    for (size_t n = 0; n < ns; ++n)
    {
        size_t a = cut[n], b = n + 1 < ns ? cut[n + 1] : len; pl[n] = a;
        for (size_t i = a; i < b; ++i) if (std::abs (mono[i]) > mx[n]) { mx[n] = std::abs (mono[i]); pl[n] = i; }
    }
    double mm = 0; for (double v : mx) mm = std::max (mm, v);
    const double cap = dbG (p["maxboost"]); DVec gn (ns);
    for (size_t n = 0; n < ns; ++n) gn[n] = mx[n] > 1e-9 ? std::min (cap, mm / mx[n]) : 1.0;
    DVec g (len, 1.0);
    for (size_t n = 0; n < ns; ++n)
    {
        size_t a = cut[n], b = n + 1 < ns ? cut[n + 1] : len, du = std::min (dove, pl[n] - a), dd = std::min (dove, b - pl[n]);
        for (size_t i = a; i < b; ++i)
        {
            double v = gn[n]; size_t j = i - a, m = b - 1 - i;
            if (n > 0 && j < du) v = (gn[n - 1] + gn[n]) / 2 + (gn[n] - gn[n - 1]) / 2 * (double) j / (double) du;
            else if (n + 1 < ns && m < dd) v = (gn[n + 1] + gn[n]) / 2 + (gn[n] - gn[n + 1]) / 2 * (double) m / (double) dd;
            g[i] = v;
        }
    }
    return applyGain (ch, cx, [&] (size_t i) { return g[i]; });
}

// VERGES: at every detected attack (above THRESH, at least GAP apart) the sound briefly glisses from TRANSPOSE back to normal pitch over
// DUR (EXP = curve), skipping ahead (or lagging) as it does; BOOST raises the verges, VERGES ONLY silences the rest.
// after CDP verges verges (verges.c:verges, write_verge)
Chans dynVerges (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(), nc = ch.size(); const int mode = p.i ("mode");
    auto pk = findPeaks (ch, sr, p["thresh"], p["gap"] / 1000);
    const double ratio = std::pow (2.0, p["transp"] / 12.0), ex = p["exp"]; const size_t vs = msS (p["dur"], sr), edge = 16;
    Chans out (nc); out[0].reserve (len + len / 4);
    size_t rp = 0;
    auto push = [&] (double pos, double g) { for (size_t c = 0; c < nc; ++c) out[c].push_back ((float) (lerpAt (ch[c], pos) * g)); };
    for (size_t v : pk)
    {
        size_t at = v > msS (5, sr) ? v - msS (5, sr) : 0;
        if (at < rp) continue;
        for (; rp < at && out[0].size() < cx.maxLen; ++rp) push ((double) rp, mode == 2 ? 0.0 : 1.0);
        double boost = 1;
        if (mode > 0)
        {
            double m = 0; for (size_t i = at; i < std::min (len, at + vs); ++i) for (auto& x : ch) m = std::max (m, (double) std::abs (x[i]));
            boost = m > 1e-9 ? std::max (1.0, 0.95 / m) : 1;
        }
        double dm = (double) at;
        for (size_t k = vs + 1; k-- > 0 && out[0].size() < cx.maxLen;)
        {
            double inc = 1 + (ratio - 1) * std::pow ((double) k / (double) vs, ex);
            size_t mpos = vs - k; double g = boost;
            if (mpos < edge) g = 1 + (boost - 1) * (double) mpos / edge;
            else if (k < edge) g = 1 + (boost - 1) * (double) k / edge;
            if (mode == 2) g *= mpos < edge * 2 ? (double) mpos / (edge * 2) : k < edge * 2 ? (double) k / (edge * 2) : 1.0;
            push (dm, g); dm += inc;
            if (dm >= (double) len) break;
        }
        rp = (size_t) std::ceil (dm);
        cx.prog ((double) rp / (double) std::max<size_t> (1, len));
    }
    for (; rp < len && out[0].size() < cx.maxLen; ++rp) push ((double) rp, mode == 2 ? 0.0 : 1.0);
    if (out[0].empty()) for (auto& x : out) x.assign (16, 0.0f);
    return out;
}

} // namespace

void addCdpEnvFilter (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CE = "CDP \xc2\xb7 ENVELOPE";
    auto W = [] (double def = 50) { return P ("wsize", "WINDOW", 5, 1000, def, PExp, "ms"); };
    auto withShape = [] (std::vector<ParamDef> head, std::vector<ParamDef> tail)
    { auto s = shapeParams(); head.insert (head.end(), s.begin(), s.end()); head.insert (head.end(), tail.begin(), tail.end()); return head; };

    add (v, CE, "cdp_envel_impose", "SHAPED ENVELOPE", "EIMP", "Imposes a generated envelope (ADSR, fades, swell, dip, PULSES, STAIRS, RANDOM) on the part between START and END, which becomes the output. dB mode maps the shape over RANGE dB; CURVE bends it.",
         withShape ({ Opt ("mode", "LEVELS", { "GAIN", "DECIBELS" }) }, { P ("range", "dB RANGE", 6, 96, 48, 0, {}, Fmt::db), Pct ("start", "START", 0), Pct ("end", "END", 1), Seed() }),
         envImpose, {}, false, "envel impose 3");
    add (v, CE, "cdp_envel_replace", "REPLACE ENVELOPE", "EREP", "Divides out the sound's own loudness contour (read in WINDOW ms steps) and puts a generated one in its place: a drum loop can swell like a pad, a pad can pulse.",
         withShape ({ Opt ("mode", "LEVELS", { "GAIN", "DECIBELS" }) }, { P ("range", "dB RANGE", 6, 96, 48, 0, {}, Fmt::db), W (20), Seed() }),
         envReplace, {}, false, "envel replace 3");
    add (v, CE, "cdp_envel_warp_normalise", "ENV NORMALISE", "ENRM", "Expands the envelope (read in WINDOW ms steps) so its highest point reaches full level.", { W() }, warpNorm, {}, false, "envel warp 1");
    add (v, CE, "cdp_envel_warp_reverse", "ENV REVERSE", "EREV", "Time-reverses the loudness contour while the sound itself plays forwards: decays become swells.", { W() }, warpRev, {}, false, "envel warp 2");
    add (v, CE, "cdp_envel_warp_exaggerate", "ENV EXAGGERATE", "EXAG", "Raises the envelope to the power EXAGGERATE: above 1 the loud parts dominate and tails sink, below 1 quiet parts are boosted.",
         { W(), P ("exag", "EXAGGERATE", 0.1, 10, 2.5, PExp, "x") }, warpExag, {}, false, "envel warp 3");
    add (v, CE, "cdp_envel_warp_attenuate", "ENV ATTENUATE", "EATN", "Scales the envelope by ATTENUATION, gliding to END over the length.",
         { W(), Pct ("atten", "ATTENUATE", 1), Pct ("atten2", "END", 0.1) }, warpAtten, {}, false, "envel warp 4");
    add (v, CE, "cdp_envel_warp_lift", "ENV LIFT", "ELFT", "Adds LIFT to the envelope level everywhere (clipped at full level): quiet tails and noise floors come up.",
         { W(), Pct ("lift", "LIFT", 0.2) }, warpLift, {}, false, "envel warp 5");
    add (v, CE, "cdp_envel_warp_timestretch", "ENV TIMESTRETCH", "ETST", "Stretches (or squeezes, below 1) the loudness contour in time while the sound keeps its speed: hits land in the wrong places.",
         { W(), P ("stretch", "STRETCH", 0.1, 10, 2, PExp, "x") }, warpStretch, {}, false, "envel warp 6");
    add (v, CE, "cdp_envel_warp_flatten", "ENV FLATTEN", "EFLT", "Averages the envelope over FLATTEN windows: transients are smoothed into the surrounding level, like slow compression.",
         { W(), P ("flatten", "FLATTEN", 1, 200, 8, PInt) }, warpFlat, {}, false, "envel warp 7");
    add (v, CE, "cdp_envel_warp_gate", "ENV GATE", "EGAT", "Windows below GATE are silenced; SMOOTHING keeps the gate shut unless the next SMOOTHING windows average above it.",
         { W(), P ("gate", "GATE", -60, 0, -18, 0, {}, Fmt::db), P ("smooth", "SMOOTHING", 0, 50, 2, PInt) }, warpGate, {}, false, "envel warp 8");
    add (v, CE, "cdp_envel_warp_invert", "ENV INVERT", "EINV", "Turns the contour upside down around MIRROR: loud becomes quiet and quiet loud; below GATE is silenced.",
         { W(), Pct ("gate", "GATE", 0.01), Pct ("mirror", "MIRROR", 0.3) }, warpInvert, {}, false, "envel warp 9");
    add (v, CE, "cdp_envel_warp_limit", "ENV LIMIT", "ELIM", "Envelope-domain limiter: levels above THRESHOLD are squeezed so the loudest point reaches LIMIT.",
         { W(20), Pct ("limit", "LIMIT", 0.3), Pct ("thresh", "THRESHOLD", 0.1) }, warpLimit, {}, false, "envel warp 10");
    add (v, CE, "cdp_envel_warp_corrugate", "ENV CORRUGATE", "ECOR", "Digs every trough of the envelope out to silence (TROUGH windows wide) and opens the peaks fully: rhythmic chopping that follows the sound. PEAK SEP = minimum windows between peaks.",
         { W(20), P ("trofdel", "TROUGH", 1, 64, 3, PInt), P ("peaksep", "PEAK SEP", 2, 128, 6, PInt) }, warpCorrugate, {}, false, "envel warp 11");
    add (v, CE, "cdp_envel_warp_expand", "ENV EXPAND", "EEXP", "Expander: below GATE is silenced, all other levels are squeezed upwards so the quietest becomes THRESHOLD. SMOOTHING as for the gate.",
         { W(), Pct ("gate", "GATE", 0.05), Pct ("thresh", "THRESHOLD", 0.5), P ("smooth", "SMOOTHING", 0, 50, 2, PInt) }, warpExpand, {}, false, "envel warp 12");
    add (v, CE, "cdp_envel_warp_trigger", "ENV TRIGGER", "ETRG", "Every fast rise of the envelope (RISE within DUR, above GATE) fires a fresh burst with its own ATTACK, DECAY and CURVE; everything else is silent.",
         { W(10), Pct ("gate", "GATE", 0.05), Pct ("rise", "RISE", 0.1), P ("dur", "DUR", 5, 500, 40, PExp, "ms"), P ("attack", "ATTACK", 3, 200, 5, PExp, "ms"),
           P ("decay", "DECAY", 10, 4000, 180, PExp, "ms"), P ("curve", "CURVE", 0.25, 8, 2, PExp, "x") }, warpTrigger, {}, false, "envel warp 13");
    add (v, CE, "cdp_envel_warp_ceiling", "ENV CEILING", "ECEI", "Forces the envelope to its maximum everywhere: all dynamics flattened, tails and noise floors pulled up to full level.",
         { W() }, warpCeiling, {}, false, "envel warp 14");
    add (v, CE, "cdp_envel_warp_ducked", "ENV DUCK", "EDCK", "Wherever the envelope exceeds THRESHOLD it is pulled down to the GATE level: the loud parts duck, the rest is untouched.",
         { W(), Pct ("gate", "GATE", 0.05), Pct ("thresh", "THRESHOLD", 0.2) }, warpDuck, {}, false, "envel warp 15");
    add (v, CE, "cdp_envel_dovetail", "DOVETAIL", "DOVE", "Fades the start in (FADE IN) and the end out (FADE OUT), each LINEAR or EXPONENTIAL; DOUBLE EXP makes both slopes steeper.",
         { Opt ("mode", "MODE", { "SHAPED", "DOUBLE EXP" }), P ("fin", "FADE IN", 0, 10, 0.3, 0, {}, Fmt::time), P ("fout", "FADE OUT", 0, 10, 1, 0, {}, Fmt::time),
           Opt ("intype", "IN SHAPE", { "LINEAR", "EXPONENTIAL" }, 1), Opt ("outtype", "OUT SHAPE", { "LINEAR", "EXPONENTIAL" }, 1) }, envDovetail, {}, false, "envel dovetail 1");
    add (v, CE, "cdp_envel_curtail", "CURTAIL", "CURT", "Fades to silence from FADE START (to FADE END, over FADE DUR, or to the end) and cuts the sound there; DBL modes have a steeper slope.",
         { Opt ("mode", "MODE", { "START-END", "START+DUR", "TO END", "START-END DBL", "START+DUR DBL", "TO END DBL" }), Pct ("fstart", "FADE START", 0.4), Pct ("fend", "FADE END", 0.8),
           P ("fdur", "FADE DUR", 0.01, 10, 1, PExp, {}, Fmt::time), Opt ("envtype", "SHAPE", { "LINEAR", "EXPONENTIAL" }, 1) }, envCurtail, {}, false, "envel curtail 1");
    add (v, CE, "cdp_envel_swell", "SWELL", "SWEL", "Fades in to the PEAK moment and out again from there, LINEAR or EXPONENTIAL.",
         { Pct ("peak", "PEAK", 0.5), Opt ("type", "SHAPE", { "LINEAR", "EXPONENTIAL" }, 1) }, envSwell, {}, false, "envel swell");
    add (v, CE, "cdp_envel_attack", "ATTACK BOOST", "ATCK", "Emphasises one attack, found by GATE, as the peak near TIME, exactly at TIME or at the loudest point, by GAIN with ONSET and DECAY ramps.",
         { Opt ("mode", "MODE", { "GATE", "NEAR TIME", "AT TIME", "AT MAX" }), P ("gate", "GATE", -60, 0, -12, 0, {}, Fmt::db), Pct ("time", "TIME", 0.25), P ("gain", "GAIN", 1, 20, 4, PExp, "x"),
           P ("onset", "ONSET", 5, 1000, 20, PExp, "ms"), P ("decay", "DECAY", 5, 5000, 300, PExp, "ms"), Opt ("envtype", "SHAPE", { "LINEAR", "EXPONENTIAL" }, 1) }, envAttack, {}, false, "envel attack 1");
    add (v, CE, "cdp_envel_pluck", "PLUCK", "PLUK", "Builds a plucked attack from CYCLES copies of the first stable wave cycle after the onset (searched from POSITION), with noise that dies at rate DECAY, and plays the sound on from there.",
         { Pct ("pos", "POSITION", 0), P ("cycles", "CYCLES", 2, 256, 32, PInt), P ("decay", "DECAY", 1, 64, 6, PExp), Seed() }, envPluck, {}, false, "envel pluck");
    add (v, CE, "cdp_envel_tremolo", "ENV TREMOLO", "ETRM", "Classic amplitude tremolo at FREQ, gliding to END FREQ (LINEAR or LOG, like pitch), with DEPTH and overall GAIN.",
         { Opt ("mode", "GLIDE", { "LINEAR", "LOG" }), P ("frq", "FREQ", 0.1, 500, 8, PExp, "Hz"), P ("frq2", "END FREQ", 0.1, 500, 8, PExp, "Hz"), Pct ("depth", "DEPTH", 0.6), Pct ("gain", "GAIN", 1) },
         envTremolo, {}, false, "envel tremolo 1");
    const std::string CF = "CDP \xc2\xb7 FILTER";
    const O FTYPES { "NOTCH", "BAND PASS", "LOW PASS", "HIGH PASS" };
    add (v, CF, "cdp_filter_fixed", "FIXED EQ", "FEQ", "Boosts or cuts BOOST dB below (LOW SHELF) or above (HIGH SHELF) FREQ, or a band of BWIDTH Hz around it (PEAK).",
         { Opt ("mode", "MODE", { "LOW SHELF", "HIGH SHELF", "PEAK" }), P ("boost", "BOOST/CUT", -48, 24, 12, 0, {}, Fmt::db), P ("frq", "FREQ", 20, 16000, 300, PExp, {}, Fmt::hz),
           P ("bw", "BWIDTH", 10, 8000, 200, PExp, {}, Fmt::hz), TailP (0) }, filtFixed, {}, false, "filter fixed 1");
    add (v, CF, "cdp_filter_lohi", "LO/HI PASS", "LOHI", "Steep low or high pass: the slope is set by the PASS band, the STOP band and the ATTENUATION reached at the stop band (more dB = steeper).",
         { Opt ("mode", "MODE", { "LOW PASS", "HIGH PASS" }), P ("atten", "ATTENUATE", 6, 96, 60, 0, {}, Fmt::db), P ("pass", "PASS BAND", 20, 18000, 800, PExp, {}, Fmt::hz),
           P ("stop", "STOP BAND", 20, 18000, 1200, PExp, {}, Fmt::hz), TailP (0) }, filtLohi, {}, false, "filter lohi 1");
    add (v, CF, "cdp_filter_variable", "VARIABLE FILTER", "FVAR", "Notch, band, low or high pass whose FREQ glides to END FREQ and whose ACUITY (smaller = sharper, 1/Q) glides to END ACUITY over the sound.",
         { Opt ("mode", "MODE", FTYPES, 1), P ("frq", "FREQ", 20, 16000, 200, PExp, {}, Fmt::hz), P ("frq2", "END FREQ", 20, 16000, 4000, PExp, {}, Fmt::hz),
           P ("acuity", "ACUITY", 0.001, 1, 0.05, PExp), P ("acuity2", "END ACUITY", 0.001, 1, 0.05, PExp), TailP (0.2) }, filtVariable, {}, false, "filter variable 2");
    add (v, CF, "cdp_filter_sweeping", "SWEEP FILTER", "FSWP", "Notch, band, low or high pass swept between LOW and HIGH at SWEEP Hz (or tempo SYNC), starting at PHASE (0 = low, 0.5 = high). Best as band or low pass with small ACUITY.",
         { Opt ("mode", "MODE", FTYPES, 1), P ("acuity", "ACUITY", 0.001, 1, 0.08, PExp), P ("lo", "LOW", 20, 16000, 150, PExp, {}, Fmt::hz), P ("hi", "HIGH", 20, 16000, 3000, PExp, {}, Fmt::hz),
           P ("sweep", "SWEEP", 0.01, 50, 0.5, PExp, "Hz"), Opt ("sync", "SYNC", SYNC_FREE), Pct ("phase", "PHASE", 0.25), TailP (0.2) }, filtSweep, {}, false, "filter sweeping 2");
    add (v, CF, "cdp_filter_bank", "FILTER BANK", "FBNK", "Resonant filters on a HARMONIC, ALTERNATE, SUBHARMONIC, OFFSET-harmonic or equal-step series between LOW and HIGH. Q glides to Q END; SCATTER detunes; DOUBLE sharpens.",
         { Opt ("mode", "SERIES", { "HARMONIC", "ALTERNATE", "SUBHARMONIC", "OFFSET", "EQUAL COUNT", "EQUAL STEP" }), QP(), P ("q2", "Q END", 1, 1000, 60, PExp),
           P ("lo", "LOW", 20, 8000, 110, PExp, {}, Fmt::hz), P ("hi", "HIGH", 40, 16000, 4000, PExp, {}, Fmt::hz), P ("offset", "OFFSET", 0, 1000, 30, 0, "Hz"),
           P ("count", "COUNT", 1, 200, 12, PInt), P ("step", "STEP", 0.1, 12, 3, 0, "st"), Pct ("scat", "SCATTER", 0), DblP(), TailP(), Seed() }, filtBank, {}, false, "filter bank 1");
    add (v, CF, "cdp_filter_userbank", "CHORD BANK", "FUSR", "A filter bank tuned to a CHORD or scale on ROOT spread over OCTAVES: noise, drums and voices start to sing the chord. Q glides to Q END, TILT darkens the upper notes.",
         { Opt ("chord", "CHORD", CHORDS, 2), P ("root", "ROOT", 24, 84, 48, PInt, {}, Fmt::note), P ("oct", "OCTAVES", 1, 4, 2, PInt), QP (80), P ("q2", "Q END", 1, 1000, 80, PExp),
           P ("tilt", "TILT", 0, 24, 3, 0, {}, Fmt::db), DblP(), TailP() }, filtUser, {}, false, "filter userbank 1");
    add (v, CF, "cdp_filter_varibank", "PROGRESSION BANK", "FVBK", "Time-varying filter bank: the CHORD on ROOT moves through a PROGRESSION, one chord per RATE (tempo), GLIDE into each change. Each note rings HARMONICS partials, ROLLOFF dB apart.",
         { Opt ("chord", "CHORD", CHORDS, 0), P ("root", "ROOT", 24, 84, 45, PInt, {}, Fmt::note), P ("oct", "OCTAVES", 1, 4, 1, PInt), Opt ("prog", "PROGRESSION", PROGS, 1),
           Opt ("div", "RATE", DIVS, 1), Pct ("glide", "GLIDE", 0.1), P ("hcnt", "HARMONICS", 1, 16, 3, PInt), P ("rolloff", "ROLLOFF", 0, 24, 6, 0, {}, Fmt::db), QP (80), DblP(), TailP() },
         filtVari, {}, false, "filter varibank 1");
    add (v, CF, "cdp_filter_varibank2", "PARTIAL BANK", "FVB2", "Filter bank on a CHORD whose every note carries PARTIALS partials that morph from PATTERN A (harmonic, odd, stretched, bell, bar, octaves) to PATTERN B over the length.",
         { Opt ("chord", "CHORD", CHORDS, 9), P ("root", "ROOT", 24, 84, 41, PInt, {}, Fmt::note), P ("oct", "OCTAVES", 1, 4, 1, PInt), Opt ("pata", "PATTERN A", PARTS, 0),
           Opt ("patb", "PATTERN B", PARTS, 3), P ("parts", "PARTIALS", 1, 12, 8, PInt), P ("rolloff", "ROLLOFF", 0, 24, 3, 0, {}, Fmt::db), QP (100), DblP(), TailP() },
         filtVari2, {}, false, "filter varibank2 1");
    add (v, CF, "cdp_filter_iterated", "ITERATED FILTER", "FITR", "Repeats the sound every DELAY (RANDOM scatter) for LENGTH, each repeat filtered again by a CHORD bank so it grows ever more resonant, with random PITCH and AMP shifts. DECAY fades the repeats.",
         { Opt ("chord", "CHORD", CHORDS, 3), P ("root", "ROOT", 24, 84, 50, PInt, {}, Fmt::note), P ("oct", "OCTAVES", 1, 4, 2, PInt), QP (30), P ("delay", "DELAY", 0.02, 4, 0.4, PExp, {}, Fmt::time),
           P ("len", "LENGTH", 0.5, 60, 8, PExp, {}, Fmt::time), Pct ("rand", "RANDOM", 0.2), P ("pitch", "PITCH", 0, 12, 0.5, 0, "st"), Pct ("amp", "AMP", 0.3), Opt ("decay", "DECAY", { "OFF", "ON" }),
           Opt ("norm", "NORMALISE", { "ON", "OFF" }), Seed() }, filtIter, {}, false, "filter iterated 1");
    add (v, CF, "cdp_filter_phasing", "PHASING", "FPHS", "ALLPASS shifts the phases only; PHASING mixes the allpass with the dry sound for the classic sweep. GAIN -1..1 sets the strength, DELAY glides to END DELAY (LOG or LINEAR).",
         { Opt ("mode", "MODE", { "ALLPASS", "PHASING" }, 1), P ("gain", "GAIN", -1, 1, 0.7), P ("delay", "DELAY", 0.05, 50, 0.5, PExp, "ms"), P ("delay2", "END DELAY", 0.05, 50, 8, PExp, "ms"),
           Opt ("interp", "GLIDE", { "LOG", "LINEAR" }), TailP (0.1) }, filtPhasing, {}, false, "filter phasing 2");

    const std::string CD = "CDP \xc2\xb7 DYNAMICS";
    add (v, CD, "cdp_modify_loudness", "LEVEL / PHASE", "LOUD", "Changes the level by a GAIN factor or by dB, NORMALISEs (only up) or FORCEs the peak to LEVEL, or INVERTs the phase.",
         { Opt ("mode", "MODE", { "GAIN", "dB GAIN", "NORMALISE", "FORCE LEVEL", "INVERT PHASE" }, 1), P ("gain", "GAIN", 0.01, 8, 0.5, PExp, "x"), P ("db", "dB GAIN", -96, 24, -6, 0, {}, Fmt::db),
           Pct ("level", "LEVEL", 0.9) }, dynLoud, {}, false, "modify loudness 2");
    add (v, CD, "cdp_modify_shudder", "SHUDDER", "SHUD", "Random stereo amplitude bursts from START at an average FREQ (SCATTER in time), each with a DEPTH and WIDTH chosen between MIN and MAX, thrown left and right by SPREAD.",
         { Pct ("start", "START", 0), P ("frq", "FREQ", 0.5, 40, 7, PExp, "Hz"), Pct ("scat", "SCATTER", 0.5), Pct ("spread", "SPREAD", 1), Pct ("mind", "MIN DEPTH", 0.5), Pct ("maxd", "MAX DEPTH", 1),
           P ("minw", "MIN WIDTH", 0.01, 1, 0.03, PExp, {}, Fmt::time), P ("maxw", "MAX WIDTH", 0.01, 1, 0.12, PExp, {}, Fmt::time), Seed() }, dynShudder, {}, false, "modify shudder");
    add (v, CD, "cdp_housekeep_deglitch", "DEGLITCH", "DGLT", "Finds bursts shorter than GLITCH with near-silence (below THRESH for SILENCE) either side, judged over WINDOW, and cuts them out with SPLICE fades: clicks between notes vanish.",
         { P ("glitch", "GLITCH", 1, 200, 15, PExp, "ms"), P ("sil", "SILENCE", 2, 500, 30, PExp, "ms"), P ("thresh", "THRESH", -90, -10, -40, 0, {}, Fmt::db),
           P ("splice", "SPLICE", 0.1, 20, 2, PExp, "ms"), P ("window", "WINDOW", 0.1, 20, 1, PExp, "ms") }, dynDeglitch, {}, false, "housekeep deglitch");
    add (v, CD, "cdp_constrict", "CONSTRICT", "CNST", "Shortens every silent gap (below THRESHOLD for at least MIN GAP) by CONSTRICTION %; beyond 100 % the sounds either side overlap. Tightens loose takes and gated material.",
         { P ("con", "CONSTRICT", 0, 200, 80, 0, "%"), P ("thresh", "THRESHOLD", -90, -10, -40, 0, {}, Fmt::db), P ("mingap", "MIN GAP", 1, 1000, 20, PExp, "ms") }, dynConstrict, {}, false, "constrict");
    add (v, CD, "cdp_envnu_expdecay", "EXP DECAY", "EXPD", "Imposes an exponential decay to near-silence between START and END; the sound stops at END.",
         { Pct ("start", "START", 0.05), Pct ("end", "END", 0.9) }, dynExpDecay, {}, false, "envnu expdecay");
    add (v, CD, "cdp_gate", "GATE", "GATE", "Removes everything below LEVEL (in every channel): replaced by SILENCE, or EDITED OUT so the gaps close up, with 1 ms splices.",
         { Opt ("mode", "MODE", { "SILENCE", "EDIT OUT" }), P ("level", "LEVEL", -96, 0, -40, 0, {}, Fmt::db) }, dynGate, {}, false, "gate gate 1");
    add (v, CD, "cdp_spike", "SPIKE", "SPIK", "Envelopes the sound into sharp spikes at the loudest point, at TIME, or at every PEAK (above THRESH, GAP apart): UPSLOPE and DOWNSLOPE powers, approaches limited to MAX UP / MAX DOWN, NORMALISE evens the spikes.",
         { Opt ("mode", "PEAKS", { "LOUDEST", "AT TIME", "EVERY PEAK" }, 2), Pct ("time", "TIME", 0.5), P ("up", "UPSLOPE", 1, 100, 4, PExp), P ("down", "DOWNSLOPE", 1, 100, 6, PExp),
           P ("maxup", "MAX UP", 0, 1, 0.05, 0, {}, Fmt::time), P ("maxdown", "MAX DOWN", 0, 1, 0.3, 0, {}, Fmt::time), Pct ("thresh", "THRESH", 0.3), P ("gap", "GAP", 20, 2000, 150, PExp, "ms"),
           Opt ("norm", "NORMALISE", { "OFF", "ON" }) }, dynSpike, {}, false, "spike spike");
    add (v, CD, "cdp_cantor_set", "CANTOR HOLES", "CNTR", "Plays copies of the sound one after another; in each a hole in the middle third (HOLE SIZE % or HOLE DUR) is dug deeper by HOLE DIG, and once past TRIGGER both sides get holes of their own: fractal gating, up to MAX DUR.",
         { Opt ("mode", "MODE", { "PERCENT", "FIXED" }), P ("holesize", "HOLE SIZE", 1, 99, 33.3, 0, "%"), P ("holedur", "HOLE DUR", 0.01, 10, 0.3, PExp, {}, Fmt::time), P ("dig", "HOLE DIG", 0.01, 1, 0.34, PExp),
           Pct ("trig", "TRIGGER", 0.5), P ("splice", "SPLICE", 3, 50, 15, 0, "ms"), P ("maxdur", "MAX DUR", 1, 120, 24, PExp, {}, Fmt::time) }, dynCantor, {}, false, "cantor set 1");
    add (v, CD, "cdp_cantor_wobble", "CANTOR WOBBLE", "CWOB", "Plays copies of the sound with superimposed vibrato-like envelope LAYERS (each LAYER DECAY shallower and faster) that deepen over DIG STEPS copies down to HOLE LEVEL, up to MAX DUR.",
         { Pct ("holelev", "HOLE LEVEL", 0), P ("steps", "DIG STEPS", 1, 256, 4, PInt), P ("layers", "LAYERS", 1, 8, 3, PInt), Pct ("layerdec", "LAYER DECAY", 0.7),
           P ("maxdur", "MAX DUR", 1, 120, 24, PExp, {}, Fmt::time) }, dynWobble, {}, false, "cantor set 3");
    add (v, CD, "cdp_tremolo", "SHARP TREMOLO", "TREM", "Tremolo whose pulses are narrowed by FINENESS: from a soft wobble to a hard stutter. FREQ glides to END FREQ (LINEAR or LOG), with DEPTH and GAIN.",
         { Opt ("mode", "GLIDE", { "LINEAR", "LOG" }), P ("frq", "FREQ", 0.1, 500, 10, PExp, "Hz"), P ("frq2", "END FREQ", 0.1, 500, 10, PExp, "Hz"), Pct ("depth", "DEPTH", 1), Pct ("gain", "GAIN", 1),
           P ("fine", "FINENESS", 1, 32, 4, PInt) }, dynTremolo, {}, false, "tremolo tremolo 1");
    add (v, CD, "cdp_tremenv", "DECAY TREMOLO", "TREN", "A sharp tremolo (FREQ, DEPTH, FINENESS) that only starts after the loudness peak (found with WINDOW), so the attack stays intact and the decay flutters.",
         { P ("frq", "FREQ", 0.1, 500, 12, PExp, "Hz"), Pct ("depth", "DEPTH", 1), P ("wsize", "WINDOW", 1, 40, 10, 0, "ms"), P ("fine", "FINENESS", 1, 32, 3, PInt) }, dynTremEnv, {}, false, "tremenv");
    add (v, CD, "cdp_flatten", "LEVELLER", "FLAT", "Cuts the sound into elements of about ELEMENT size (syllables, hits) at envelope troughs and raises each to the level of the loudest (up to MAX BOOST), with SHOULDER ramps. The last TAIL counts as one element.",
         { P ("elem", "ELEMENT", 0.02, 4, 0.2, PExp, {}, Fmt::time), P ("shoulder", "SHOULDER", 1, 500, 20, PExp, "ms"), Pct ("tail", "TAIL", 0), P ("maxboost", "MAX BOOST", 0, 60, 30, 0, {}, Fmt::db) },
         dynFlatten, {}, false, "flatten flatten");
    add (v, CD, "cdp_verges", "VERGES", "VERG", "At every attack (above THRESH, GAP apart) the sound briefly glisses from TRANSPOSE back to its own pitch over DUR (EXP bends the glide). BOOST lifts the verges, VERGES ONLY silences everything else.",
         { Opt ("mode", "MODE", { "NORMAL", "BOOST", "VERGES ONLY" }), P ("transp", "TRANSPOSE", -24, 24, 7, 0, "st"), P ("exp", "EXP", 1, 8, 2), P ("dur", "DUR", 10, 1000, 120, PExp, "ms"),
           Pct ("thresh", "THRESH", 0.3), P ("gap", "GAP", 20, 2000, 150, PExp, "ms") }, dynVerges, {}, false, "verges verges");

}
} // namespace lab
