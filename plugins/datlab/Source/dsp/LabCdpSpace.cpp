// CDP collection, Batch 8 - Modify / delay / reverb / spatial.
// Multichannel CDP programs are folded to stereo: a virtual ring of loudspeakers is decoded to L/R.
#include "LabCdp.h"
#include "LabDspUtil.h"

namespace lab
{
namespace
{
using namespace lab::dsp;

inline void tick (Ctx& cx, size_t i, size_t n, double a = 0, double b = 1)
{
    if (! (i & 0x7fff)) cx.prog (a + (b - a) * (double) i / (double) std::max ((size_t) 1, n));
}
inline size_t capLen (double n, const Ctx& cx) { return (size_t) clampd (n, 1, (double) std::max ((size_t) 1, cx.maxLen)); }

// CDP pancalc (modify/pan.c): -1 = left, 1 = right, beyond +-1 = outside the speakers (inverse-square fall-off)
inline void cdpPan (double pos, double& gl, double& gr)
{
    const double ROOT2 = std::sqrt (2.0), rel = std::abs (pos);
    if (rel <= 1)
    {
        double reldist = ROOT2 / std::sqrt (1 + rel * rel), t = (pos + 1) / 2;
        gr = t * reldist; gl = (1 - t) * reldist;
    }
    else
    {
        double rd = std::sqrt (rel * rel + 1) / ROOT2, inv = 1 / (rd * rd);
        if (pos < 0) { gl = inv; gr = 0; } else { gr = inv; gl = 0; }
    }
}
// CDP set_stereo_levels (frame.c): equal-power-ish position 0..1 between a speaker pair, with hole-in-the-middle compensation
inline void pairLevels (double pos, double& ll, double& rl)
{
    double z = std::abs (pos * 2 - 1), hc = std::sqrt (2.0) / std::sqrt (1 + z * z);
    rl = pos * hc; ll = (1 - pos) * hc;
}
// Virtual ring of N loudspeakers (speaker 0 front, clockwise) decoded to stereo.
// pos in speakers (any real), focus 0..1: 1 = all signal in the speaker pair, less spills to the neighbours.
struct RingDecoder
{
    int N = 8; std::vector<double> sl, sr;
    explicit RingDecoder (int n) : N (std::max (2, n)), sl ((size_t) N), sr ((size_t) N)
    {
        for (int k = 0; k < N; ++k)
        {
            double az = TAU * k / N, pan = std::sin (az), rear = 0.5 + 0.5 * std::cos (az);
            double g = 0.72 + 0.28 * rear;   // rear speakers a little quieter
            double t = (pan + 1) / 2;
            sl[(size_t) k] = std::cos (t * PI / 2) * g; sr[(size_t) k] = std::sin (t * PI / 2) * g;
        }
        if (N == 2) { sl[0] = 1; sr[0] = 0; sl[1] = 0; sr[1] = 1; }   // a 2-speaker "ring" is plain stereo
    }
    void gains (double pos, double focus, double& gl, double& gr) const
    {
        double p = std::fmod (pos, (double) N); if (p < 0) p += N; if (! std::isfinite (p)) p = 0;
        int a = (int) std::floor (p) % N, b = (a + 1) % N; double f = p - std::floor (p);
        double la, lb; pairLevels (f, la, lb);
        focus = clampd (focus, 0, 1);
        gl = (la * sl[(size_t) a] + lb * sl[(size_t) b]) * focus; gr = (la * sr[(size_t) a] + lb * sr[(size_t) b]) * focus;
        if (focus < 1)
        {
            int an = (a + N - 1) % N, bn = (b + 1) % N; double s = (1 - focus) * 0.5;
            gl += s * (la * sl[(size_t) an] + lb * sl[(size_t) bn]); gr += s * (la * sr[(size_t) an] + lb * sr[(size_t) bn]);
            gl += s * (la * sl[(size_t) b] + lb * sl[(size_t) a]); gr += s * (la * sr[(size_t) b] + lb * sr[(size_t) a]);
        }
    }
};
inline double semis (double st) { return std::pow (2.0, st / 12.0); }

// ================================================================ PAN
// PAN: places a mono sound in stereo, sweeping from START to END with an optional LFO; positions beyond +-1 sit outside
// the speakers. after CDP modify space 1 dopan (modify/pan.c)
Chans pan (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    Chans out (2, Buf (n));
    double gl = 0, gr = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double t = (double) i / (double) std::max ((size_t) 1, n - 1);
        double pos = p["start"] + (p["end"] - p["start"]) * t + p["depth"] * std::sin (TAU * p["rate"] * (double) i / sr);
        if (! (i & 31)) cdpPan (clampd (pos, -3, 3), gl, gr);
        out[0][i] = (float) (x[i] * gl * p["prescale"]); out[1][i] = (float) (x[i] * gr * p["prescale"]);
        tick (cx, i, n);
    }
    return out;
}

// PAN CURVE: a pan breakpoint shape stretched to the sound's duration. after CDP modify scaledpan (modify/pan.c)
Chans scaledpan (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size();
    const int shape = p.i ("shape"), cyc = std::max (1, p.i ("cycles")); const double w = p["width"];
    std::vector<std::pair<double, double>> bp;
    cx.seed (p["seed"]);
    switch (shape)
    {
        case 0: bp = { { 0, -1 }, { 1, 1 } }; break;
        case 1: bp = { { 0, 1 }, { 1, -1 } }; break;
        case 2: for (int c = 0; c <= 2 * cyc; ++c) bp.push_back ({ (double) c / (2 * cyc), c % 2 ? 1.0 : -1.0 }); break;
        case 3: for (int c = 0; c <= 2 * cyc; ++c) bp.push_back ({ (double) c / (2 * cyc), c % 2 ? -1.0 : 1.0 }); break;
        case 4: for (int c = 0; c <= 4 * cyc; ++c) bp.push_back ({ (double) c / (4 * cyc), (c % 2 ? 1.0 : -1.0) * (double) c / (4 * cyc) }); break;   // widening zigzag
        default: { int st = 4 * cyc; for (int c = 0; c <= st; ++c) { double v = cx.rand() * 2 - 1; bp.push_back ({ (double) c / st, v }); if (c < st) bp.push_back ({ ((double) c + 0.8) / st, v }); } break; }
    }
    Chans out (2, Buf (n)); size_t k = 0; double gl = 0, gr = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double t = (double) i / (double) std::max ((size_t) 1, n - 1);
        while (k + 2 < bp.size() && t > bp[k + 1].first) ++k;
        auto a = bp[k], b = bp[std::min (k + 1, bp.size() - 1)];
        double f = b.first > a.first ? clampd ((t - a.first) / (b.first - a.first), 0, 1) : 0;
        double pos = (a.second + (b.second - a.second) * f) * w;
        if (! (i & 31)) cdpPan (pos, gl, gr);
        out[0][i] = (float) (x[i] * gl * p["prescale"]); out[1][i] = (float) (x[i] * gr * p["prescale"]);
        tick (cx, i, n);
    }
    return out;
}

// STEREO NARROW: pulls the channels together (0 = mono) or through each other (-1 = mirrored L/R).
// after CDP modify space 2 mirroring / modify space 4 narrow_sound (modify/pan.c)
Chans narrow (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans s = stereoOf (ch); const size_t n = s[0].size();
    const double a = (p["narrow"] + 1) / 2, b = 1 - a;
    for (size_t i = 0; i < n; ++i)
    {
        double l = s[0][i], r = s[1][i];
        s[0][i] = (float) (l * a + r * b); s[1][i] = (float) (r * a + l * b);
        tick (cx, i, n);
    }
    return s;
}

// ================================================================ SPEED
// VARISPEED: tape-speed change (pitch and duration together), sweeping from START to END over the sound.
// after CDP modify speed 1-2 / strans multi 1-2 process_varispeed (modify/strans.c)
Chans varispeed (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = ch[0].size(); const bool ratioMode = p.i ("mode") == 0;
    const double r0 = semis (p["start"]), r1 = semis (p["end"]);
    std::vector<double> pos; pos.reserve (n);
    double ph = 0;
    while (ph < (double) n - 1 && pos.size() < cx.maxLen)
    {
        pos.push_back (ph);
        double t = ph / (double) std::max ((size_t) 1, n - 1);
        double r = ratioMode ? r0 + (r1 - r0) * t : semis (p["start"] + (p["end"] - p["start"]) * t);
        ph += std::max (1.0 / 16, r);
        if (! (pos.size() & 0x7fff)) cx.prog (0.5 * t);
    }
    if (pos.empty()) pos.push_back (0);
    Chans out;
    for (auto& x : ch) { Buf y (pos.size()); for (size_t i = 0; i < pos.size(); ++i) y[i] = (float) lerpAt (x, pos[i]); out.push_back (std::move (y)); }
    cx.prog (1);
    return out;
}

// ACCELERATE: plays normally until START, then speeds up (or slows down) to reach GOAL speed after GOAL TIME and keeps going.
// after CDP modify speed 5 / strans multi 3 do_acceleration (modify/strans.c)
Chans accel (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = ch[0].size(); const double sr = cx.sr;
    const double fac = std::pow (p["goal"], 1.0 / std::max (1.0, p["gtime"] * sr));
    const double st = std::min ((double) n - 1, p["stt"] * sr);
    std::vector<double> pos;
    double ph = 0, inc = 1;
    while (ph < (double) n - 1 && pos.size() < cx.maxLen)
    {
        pos.push_back (ph);
        ph += inc;
        if (ph > st) { inc *= fac; if (inc < 0.0001) break; inc = std::min (inc, 256.0); }
        if (! (pos.size() & 0x7fff)) cx.prog (0.5 * ph / (double) n);
    }
    if (pos.empty()) pos.push_back (0);
    Chans out;
    for (auto& x : ch) { Buf y (pos.size()); for (size_t i = 0; i < pos.size(); ++i) y[i] = (float) lerpAt (x, pos[i]); fades (y, 0, std::min ((double) y.size() / 4, 0.02 * sr)); out.push_back (std::move (y)); }
    return out;
}

// TAPE VIBRATO: periodic varispeed, RATE and DEPTH (semitones) moving from start to end values.
// after CDP modify speed 6 / strans multi 4 do_vibrato (modify/strans.c)
Chans vibrato (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = ch[0].size(); const double sr = cx.sr;
    std::vector<double> pos; double ph = 0, sp = 0;
    while (ph < (double) n - 1 && pos.size() < cx.maxLen)
    {
        pos.push_back (ph);
        double t = ph / (double) n, f = p["rate"] + (p["rate2"] - p["rate"]) * t, d = p["depth"] + (p["depth2"] - p["depth"]) * t;
        sp += f / sr; sp -= std::floor (sp);
        ph += std::pow (2.0, std::sin (TAU * sp) * d / 12.0);
        if (! (pos.size() & 0x7fff)) cx.prog (0.5 * t);
    }
    if (pos.empty()) pos.push_back (0);
    Chans out;
    for (auto& x : ch) { Buf y (pos.size()); for (size_t i = 0; i < pos.size(); ++i) y[i] = (float) lerpAt (x, pos[i]); out.push_back (std::move (y)); }
    return out;
}

// ================================================================ DELAY
// ECHO / COMB: delay with feedback and dry/wet mix (a comb resonance at short times); INVERT flips the dry signal.
// after CDP modify revecho 1 do_delay (modify/delay.c) and the Groucho delay (externals/reverb/delay.c)
Chans echo (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t n = ch[0].size();
    const size_t D = (size_t) std::max (1.0, std::round (p["delay"] / 1000 * sr));
    const double fb = clampd (p["feedback"], -0.999, 0.999), mix = p["mix"], inv = p.i ("invert") ? -1 : 1;
    const double rescale = 1 / (std::abs (fb) + 1), pre = rescale, fwd = (1 - mix) / rescale * inv;
    const size_t outLen = capLen ((double) n + p["tail"] * sr, cx);
    Chans out;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        std::vector<double> d (D, 0.0); size_t ip = 0; Buf y (outLen);
        for (size_t i = 0; i < outLen; ++i)
        {
            double in = i < n ? ch[c][i] * pre : 0.0;
            double dv = d[ip];
            y[i] = (float) (in * fwd + dv * mix);
            d[ip] = dv * fb + in; if (++ip >= D) ip = 0;
            tick (cx, i, outLen, (double) c / ch.size(), (c + 1.0) / ch.size());
        }
        out.push_back (std::move (y));
    }
    return out;
}

// MOD DELAY: an LFO sweeps the delay time (chorus / flanger / vibrato echoes); RANDOM makes the LFO speed wander.
// after CDP modify revecho 2 do_vdelay_multi (modify/delay.c)
Chans vdelay (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t n = ch[0].size();
    const double D = std::max (2.0, p["delay"] / 1000 * sr);
    const double range = std::min (D - 1, D * p["mod"]);
    const double fb = clampd (p["feedback"], -0.999, 0.999), mix = p["mix"];
    const double rescale = 1 / (std::abs (fb) + 1), fwd = (1 - mix) / rescale;
    const bool rnd = p.i ("lfotype") == 1; const double frq = p["lfofrq"], modlen = sr / std::max (0.01, frq);
    const size_t outLen = capLen ((double) n + p["tail"] * sr, cx), lfoDelay = (size_t) (p["lfodelay"] * sr);
    cx.seed (p["seed"]);
    std::vector<Ring> rings; for (size_t c = 0; c < ch.size(); ++c) rings.emplace_back ((size_t) D + 4);
    Chans out (ch.size(), Buf (outLen));
    double phase = TAU * p["phase"], rout = 0, diff = 0; long long ik = 0;
    const double incr = TAU * frq / sr;
    for (size_t i = 0; i < outLen; ++i)
    {
        if (i >= lfoDelay)
        {
            double fm = 1;
            if (rnd)
            {
                if (ik == 0) { double a = cx.rand(), b = cx.rand(); diff = rout - 0.5 * (a + b); }
                rout -= diff / modlen; if (++ik >= (long long) modlen) ik = 0;
                fm = rout;
            }
            phase += incr * fm; if (phase >= PI) phase -= TAU;
        }
        double val = 0.5 + 0.5 * std::sin (phase), dl = std::max (1.0, D - range * val);
        for (size_t c = 0; c < ch.size(); ++c)
        {
            double in = i < n ? ch[c][i] * rescale : 0.0, dv = rings[c].read (dl);
            out[c][i] = (float) (in * fwd + dv * mix);
            rings[c].push (dv * fb + in);
        }
        tick (cx, i, outLen);
    }
    return out;
}

// STADIUM ECHO: the CDP stadium: a set of decaying echoes scattered around the stereo field, like a PA in a stadium.
// CENTRE and SPREAD place and size the echo field. after CDP modify revecho 3 do_stadium (modify/delay.c) and
// mchanrev do_multichan_stadium (standalone/mchanrev.c)
Chans stadium (const Chans& ch, Ctx& cx, const Params& p)
{
    static const double T[23] = { 0, 0.131832, 0.215038, 0.322274, 0.414122, 0.504488, 0.637713, 0.730468, 0.808751, 0.910460, 1.027041, 1.132028,
                                  1.244272, 1.336923, 1.427700, 1.528503, 1.618661, 1.715413, 1.814730, 1.914843, 2.000258, 2.135363, 2.230554 };
    static const double G[23] = { 1, 1, 0.354813, 0.354813, 0.251189, 0.125893, 0.125893, 0.063096, 0.063096, 0.031623, 0.031623, 0.012589,
                                  0.012589, 0.005012, 0.005012, 0.002512, 0.002512, 0.002512, 0.002512, 0.002512, 0.002512, 0.002512, 0.002512 };
    static const double PN[23] = { -1, 1, .5, -.5, 0, .7, -.7, .3, -.3, .15, -.15, .85, -.85, .4, -.4, .6, -.6, .225, -.225, .775, -.775, .125, -.125 };
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    const int cnt = clamp (p.i ("count"), 2, 200);
    std::vector<double> td ((size_t) cnt), g ((size_t) cnt), pn ((size_t) cnt);
    for (int k = 0; k < cnt; ++k)
    {
        if (k < 23) { td[(size_t) k] = T[k]; pn[(size_t) k] = PN[k]; }
        else
        {   // beyond CDP's short table: continue the ~0.1 s average spacing with irregular gaps and alternating sides
            double j = std::fmod ((double) k * 0.6180339887, 1.0);
            td[(size_t) k] = td[(size_t) k - 1] + 0.06 + 0.07 * j;
            pn[(size_t) k] = (k % 2 ? 1 : -1) * std::fmod ((double) k * 0.381966, 1.0);
        }
        if (cnt <= 23) g[(size_t) k] = G[k];
        else g[(size_t) k] = std::exp (std::log (0.0001) * (double) k / (cnt - 1));
        if (p["rolloff"] < 1) g[(size_t) k] = std::pow (g[(size_t) k], p["rolloff"]);
    }
    const double size = p["size"];
    const size_t maxD = (size_t) std::round (td.back() * sr * size);
    const size_t outLen = capLen ((double) (n + maxD), cx);
    Chans out (2, Buf (outLen));
    for (int k = 0; k < cnt; ++k)
    {
        double pp = clampd (p["centre"] + pn[(size_t) k] * p["spread"], -1, 1), gk = g[(size_t) k] * p["gain"];
        double gl = pp < 0 ? gk : gk * (1 - pp), gr = pp < 0 ? gk * (1 + pp) : gk;
        size_t d = (size_t) std::round (td[(size_t) k] * sr * size);
        for (size_t i = 0; i < n && i + d < outLen; ++i) { out[0][i + d] += (float) (x[i] * gl); out[1][i + d] += (float) (x[i] * gr); }
        cx.prog ((k + 1.0) / cnt);
    }
    return out;
}

// ================================================================ RADICAL
// REVERSE: plays the sound backwards. after CDP modify radical 1 do_reversing (modify/radical.c)
Chans reverse (const Chans& ch, Ctx& cx, const Params&)
{
    Chans out = ch; for (auto& x : out) std::reverse (x.begin(), x.end()); cx.prog (1); return out;
}

// SHRED: cuts the sound into chunks of about CHUNK length at scattered boundaries and re-joins them in random order,
// REPEATS times; the length stays the same. after CDP modify radical 2 shred (modify/radical.c)
Buf shredOnce (const Buf& x, const std::vector<size_t>& ptr, const std::vector<size_t>& len, const std::vector<int>& perm, size_t S, size_t work)
{
    Buf y (x.size()); size_t pos = 0; const size_t cc = perm.size();
    for (size_t q = 0; q < cc; ++q)
    {
        size_t st = ptr[(size_t) perm[q]], L = len[(size_t) perm[q]];
        for (size_t i = 0; i < L + S; ++i)
        {
            size_t src = st + i, dst = pos + i; if (src >= x.size() || dst >= work + S || dst >= y.size()) break;
            double g = 1;
            if (q > 0 && i < S) g = (double) i / (double) S;
            if (i >= L) { if (q + 1 == cc) break; g = 1 - (double) (i - L) / (double) S; }
            y[dst] = (float) (y[dst] + x[src] * g);
        }
        pos += L;
    }
    return y;
}
// shared by shred (radical 2) and the stereo shred (mchshred): returns, per repeat, chunk starts/lengths/order
struct ShredPlan { std::vector<size_t> ptr, len; std::vector<int> perm; };
ShredPlan shredPlan (size_t n, size_t S, int cc, double scat, Ctx& cx)
{
    ShredPlan sp; const size_t work = n > S ? n - S : n, unit = std::max ((size_t) 1, work / (size_t) cc);
    const size_t raw = unit > S ? unit - S : 1;
    sp.ptr.assign ((size_t) cc, 0);
    if (scat <= 1)
    {
        for (int k = 1; k < cc; ++k)
        {
            double s = (cx.rand() - 0.5) * scat; long long v = (long long) unit * k + (long long) (s * (double) raw);
            sp.ptr[(size_t) k] = (size_t) clamp (v, (long long) sp.ptr[(size_t) k - 1] + (long long) S + 1, (long long) work - 1);
        }
    }
    else
    {   // heavy scatter: chunk starts drawn at random inside groups of SCAT chunk-lengths
        const int grp = std::max (1, (int) std::round (scat));
        for (int k = 1; k < cc; ++k)
        {
            int g0 = (k / grp) * grp; size_t lo = (size_t) g0 * unit, hi = std::min (work - 1, (size_t) std::min (cc, g0 + grp) * unit);
            sp.ptr[(size_t) k] = lo + (size_t) (cx.rand() * (double) (hi > lo ? hi - lo : 1));
        }
        std::sort (sp.ptr.begin() + 1, sp.ptr.end());
        for (int k = 1; k < cc; ++k) sp.ptr[(size_t) k] = std::min (work - 1, std::max (sp.ptr[(size_t) k], sp.ptr[(size_t) k - 1] + S + 1));
    }
    sp.len.resize ((size_t) cc);
    for (int k = 0; k < cc; ++k) { size_t e = k + 1 < cc ? sp.ptr[(size_t) k + 1] : work; sp.len[(size_t) k] = e > sp.ptr[(size_t) k] ? e - sp.ptr[(size_t) k] : 1; }
    sp.perm.resize ((size_t) cc); for (int k = 0; k < cc; ++k) sp.perm[(size_t) k] = k;
    for (int k = cc - 1; k > 0; --k) std::swap (sp.perm[(size_t) k], sp.perm[(size_t) std::min ((double) k, std::floor (cx.rand() * (k + 1)))]);
    return sp;
}
Chans shred (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = ch[0].size(); const double sr = cx.sr;
    const size_t S = std::max ((size_t) 2, (size_t) (0.006 * sr));
    const int cc = (int) std::min (2000.0, std::round ((double) n / sr / p["chunk"]));
    Chans out = ch;
    if (cc < 2 || n < 4 * S) return out;
    cx.seed (p["seed"]);
    const int reps = std::max (1, p.i ("repeats"));
    for (int r = 0; r < reps; ++r)
    {
        ShredPlan sp = shredPlan (n, S, cc, p["scatter"], cx);
        for (auto& x : out) x = shredOnce (x, sp.ptr, sp.len, sp.perm, S, n - S);
        cx.prog ((r + 1.0) / reps);
    }
    return out;
}

// SCRUB: the read head is wound back and forth between random points near the START and near the END, like a tape rocked
// by hand over the head: slow at the turning points, fast in the middle, speeds between DOWN and UP semitones.
// after CDP modify radical 3 do_scrubbing (modify/radical.c)
Chans scrub (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = ch[0].size(); const double sr = cx.sr;
    const size_t total = capLen (p["dur"] * sr, cx);
    const double lo = std::min (p["down"], p["up"]), hi = std::max (p["down"], p["up"]), range = hi - lo;
    const double sRange = p["sttrange"] * (double) (n - 1), eStart = p["endstart"] * (double) (n - 1);
    const int TAB = 256;
    std::vector<double> sintab (TAB + 1);
    for (int k = 0; k < TAB; ++k) sintab[(size_t) k] = std::pow (std::sin (PI * k / TAB), 0.3);
    sintab[TAB] = 0;
    cx.seed (p["seed"]);
    Chans out (ch.size(), Buf (total));
    size_t o = 0; bool fwd = true; double here = std::round (cx.rand() * sRange);
    int guard = 0;
    while (o < total && guard++ < 100000)
    {
        double there = fwd ? eStart + std::round (cx.rand() * ((double) (n - 1) - eStart)) : std::round (cx.rand() * sRange);
        double dist = std::abs (there - here);
        if (dist < 4) { fwd = ! fwd; if (guard > 64 && n < 64) break; continue; }
        double skew = 1 + (cx.rand() * 2 - 1) * 0.01;
        double hs = hi - cx.rand() * range * 0.15, ls = lo + cx.rand() * range * 0.15, sr2 = hs - ls;
        // skewed warped half-sine speed curve in semitones -> ratios
        std::vector<double> tab ((size_t) TAB + 1);
        { double idx = 0, step = std::abs (skew - 1) < 1e-9 ? 1.0 : TAB * (1 - skew) / (1 - std::pow (skew, (double) TAB));
          for (int k = 0; k <= TAB; ++k) { tab[(size_t) k] = semis (ls + sr2 * lerpAt (sintab, std::min ((double) TAB, idx))); idx += step; step *= skew; } }
        double mean = 0; for (double v : tab) mean += v; mean /= (double) tab.size();
        size_t M = (size_t) std::max (8.0, std::min (dist / mean, (double) (total - o) + 1));
        double scale = dist / (mean * (double) M);
        size_t spl = std::min ((size_t) (0.1 * sr), M / 2);
        double ph = here;
        for (size_t j = 0; j < M && o < total; ++j, ++o)
        {
            double sp = lerpAt (tab, (double) j / (double) M * TAB) * scale;
            double g = j < spl ? (double) j / (double) spl : j + spl > M ? (double) (M - j) / (double) spl : 1;
            for (size_t c = 0; c < ch.size(); ++c) out[c][o] = (float) (lerpAt (ch[c], clampd (ph, 0, (double) n - 1)) * g);
            ph += fwd ? sp : -sp;
            tick (cx, o, total);
        }
        o += std::min ((size_t) TAB, total - o);   // a short gap between strokes
        here = there;
        if (p.i ("fwdonly")) { here = std::round (cx.rand() * sRange); }
        else fwd = ! fwd;
    }
    return out;
}

// LOSE RESOLUTION: fewer bits (BITS) and a lower sample rate (each RATE DIV samples averaged and held).
// after CDP modify radical 4 lobit_process / do_lowbit (modify/radical.c)
Chans lobit (const Chans& ch, Ctx& cx, const Params& p)
{
    const int bits = clamp (p.i ("bits"), 1, 16), ts = std::max (1, p.i ("srdiv"));
    const double bres = std::pow (2.0, 16 - bits), mx = 32767.0;
    Chans out = ch;
    for (auto& x : out)
    {
        const size_t n = x.size();
        for (size_t i = 0; i < n; i += (size_t) ts)
        {
            size_t e = std::min (n, i + (size_t) ts); double a = 0;
            for (size_t j = i; j < e; ++j) a += x[j];
            a /= (double) (e - i);
            a = clampd (std::round (a * mx / bres) * bres / mx, -1, 1);
            for (size_t j = i; j < e; ++j) x[j] = (float) a;
            tick (cx, i, n);
        }
    }
    return out;
}

// RING MOD: multiplies the sound by a sine whose frequency glides from START to END. after CDP modify radical 5 ring_modulate (modify/radical.c)
Chans ringmod (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = ch[0].size(); const double sr = cx.sr; Chans out = ch; double ph = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double t = (double) i / (double) std::max ((size_t) 1, n - 1), f = p["frq"] * std::pow (p["frq2"] / p["frq"], t);
        ph += f / sr; ph -= std::floor (ph); double m = std::sin (TAU * ph);
        for (auto& x : out) x[i] = (float) (x[i] * m);
        tick (cx, i, n);
    }
    return out;
}

// QUANTISE: mid-rise quantisation to BITS bits (hiss-free, gritty). after CDP modify radical 7 do_lowbit2 (modify/radical.c)
Chans quant (const Chans& ch, Ctx& cx, const Params& p)
{
    const double fac = std::pow (2.0, 16 - clamp (p.i ("bits"), 1, 16));
    Chans out = ch;
    for (auto& x : out)
        for (size_t i = 0; i < x.size(); ++i)
        {
            double s = std::max (-32767.0, x[i] * 32767.0) / fac;
            s = std::round (s + 0.5) - 0.5;
            x[i] = (float) (s * fac / 32768.0);
            tick (cx, i, x.size());
        }
    return out;
}

// STACK: COUNT tape-transposed copies, each INTERVAL higher, layered with their attacks aligned (ATTACK = time of the
// attack in the source); LEAN sets the level of the lowest copy against the highest. after CDP modify stack do_stack (modify/radical.c)
Chans stack (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = ch[0].size(); const double sr = cx.sr; const int cnt = clamp (p.i ("count"), 2, 16);
    const double mult = semis (p["interval"]);
    std::vector<double> tr ((size_t) cnt), amp ((size_t) cnt);
    if (mult >= 1) { double s = 1; for (int k = 0; k < cnt; ++k) { tr[(size_t) k] = s; s *= mult; } }
    else { double s = 1; for (int k = cnt - 1; k >= 0; --k) { tr[(size_t) k] = s; s *= mult; } }
    for (auto& t : tr) t = clampd (t, 1.0 / 16, 16);   // keep the slowest copy within 4 octaves down
    const double minT = *std::min_element (tr.begin(), tr.end());
    double lean = p["lean"], step = (1 - lean) / (cnt - 1), sum = 0;
    for (int k = 0; k < cnt; ++k) { amp[(size_t) k] = lean + step * k; sum += amp[(size_t) k]; }
    for (auto& a : amp) a /= sum;
    double offOut = p["attack"] * sr; if (minT < 1) offOut /= minT;
    size_t outLen = (size_t) ((double) n / minT * p["dur"]) + 1;
    outLen = capLen ((double) outLen, cx);
    Chans out (ch.size(), Buf (outLen));
    for (int k = 0; k < cnt; ++k)
    {
        double rel = tr[(size_t) k] / minT; size_t skip = std::abs (rel - 1) < 1e-9 ? 0 : (size_t) std::round (offOut * (1 - 1 / rel));
        for (size_t c = 0; c < ch.size(); ++c)
        {
            double ip = 0;
            for (size_t o = skip; o < outLen && ip < (double) n - 1; ++o, ip += tr[(size_t) k]) out[c][o] = (float) (out[c][o] + lerpAt (ch[c], ip) * amp[(size_t) k]);
        }
        cx.prog ((k + 1.0) / cnt);
    }
    for (auto& y : out) fades (y, 0, std::min ((double) y.size() / 4, 0.01 * sr));
    return out;
}

// ================================================================ MULTICHANNEL -> STEREO
// FLUTTER: a tremolo that moves between channel sets on every cycle (left, right, both...), RANDOM reshuffles the order.
// after CDP flutter (standalone/flutter.c)
Chans flutter (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans s = stereoOf (ch); const size_t n = s[0].size(); const double sr = cx.sr;
    static const std::vector<std::vector<int>> SEQ[] = { { { 0 }, { 1 } }, { { 0 }, { 0, 1 }, { 1 } }, { { 0 }, { 1 }, { 0, 1 } }, { { 0, 1 }, { 0 }, { 0, 1 }, { 1 } } };
    const auto& seq = SEQ[clamp (p.i ("seq"), 0, 3)];
    const double depthP = p["depth"], depth = std::min (1.0, depthP), base = std::max (0.0, 1 - depth);
    std::vector<int> perm (seq.size()); for (size_t k = 0; k < perm.size(); ++k) perm[k] = (int) k;
    cx.seed (p["seed"]);
    double ph = 0; size_t set = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double v = 0.5 + 0.5 * std::cos (TAU * ph);
        if (depthP > 1) v = std::pow (v, depthP);
        v *= depth;
        double last = ph; ph += p["rate"] / sr; ph -= std::floor (ph);
        if (ph < last)
            if (++set >= seq.size()) { set = 0; if (p.i ("random")) for (size_t k = perm.size() - 1; k > 0; --k) std::swap (perm[k], perm[(size_t) std::floor (cx.rand() * (double) (k + 1)) % (k + 1)]); }
        const auto& cs = seq[(size_t) perm[set]];
        double l = s[0][i], r = s[1][i];
        s[0][i] = (float) (l * base); s[1][i] = (float) (r * base);
        for (int c : cs) s[(size_t) c][i] = (float) (s[(size_t) c][i] + (c ? r : l) * v);
        tick (cx, i, n);
    }
    return s;
}

// FRAME ROTATE: the whole stereo frame rotates, so left and right travel through each other at RATE (negative = the other way);
// ODD/EVEN rotates the two channels independently at RATE and RATE 2. SMEAR bleeds each channel into its neighbour.
// after CDP frame shift 1-2 rotate (standalone/frame.c)
Chans frame (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans s = stereoOf (ch); const size_t n = s[0].size(); const double sr = cx.sr;
    const bool indep = p.i ("mode") == 1; const double sm = p["smear"], comp = 1 - 2 * sm;
    Chans out (2, Buf (n));
    for (size_t i = 0; i < n; ++i)
    {
        double t = (double) i / sr;
        for (int c = 0; c < 2; ++c)
        {
            double rate = (indep && c == 1) ? p["rate2"] : p["rate"];
            double pos = c + rate * t * 2;   // one rotation = two speaker steps on a 2-speaker ring
            double fl = std::floor (pos), f = pos - fl; int a = ((int) std::fmod (fl, 2.0) + 2) % 2;
            double ll, rl; pairLevels (f, ll, rl);
            double sig = s[(size_t) c][i];
            double toA = sig * ll * comp + sig * rl * 2 * sm, toB = sig * rl * comp + sig * ll * 2 * sm;
            out[(size_t) a][i] += (float) toA; out[(size_t) (1 - a)][i] += (float) toB;
        }
        tick (cx, i, n);
    }
    return out;
}

// ================================================================ EVENTS (for the event-switching pan programs)
// Start samples of events: a rise above THRESH after at least MINSIL of quiet, or a sharp onset (CDP uses true silences
// between events; an onset test makes the idea work on continuous material too). The first event starts at 0.
std::vector<size_t> findEvents (const Buf& x, double sr, double thrDb, double minsilMs)
{
    const size_t W = std::max ((size_t) 16, (size_t) (sr * 0.005)), nw = x.size() / W + 1;
    std::vector<double> env (nw, 0.0); double pk = 0;
    for (size_t w = 0; w < nw; ++w)
    {
        double a = 0; size_t c = 0;
        for (size_t i = w * W; i < std::min (x.size(), (w + 1) * W); ++i, ++c) a += (double) x[i] * x[i];
        env[w] = c ? std::sqrt (a / (double) c) : 0; pk = std::max (pk, env[w]);
    }
    const double thr = pk * std::pow (10.0, thrDb / 20);
    const size_t need = std::max ((size_t) 1, (size_t) std::ceil (minsilMs / 1000 * sr / (double) W));
    const size_t minGap = std::max (need, (size_t) std::ceil (0.06 * sr / (double) W));
    std::vector<size_t> st { 0 }; size_t quiet = 0, last = 0; double avg = 0; bool started = false;
    for (size_t w = 0; w < nw; ++w)
    {
        double e = env[w];
        if (e < thr || e <= 0) { ++quiet; avg = avg * 0.7 + e * 0.3; continue; }
        bool onset = (quiet >= need) || (e > 3.0 * avg + 1e-9 && w - last >= minGap);
        if (onset && started && w - last >= std::min (need, minGap)) { st.push_back (w * W); last = w; }
        if (! started) { started = true; last = w; }
        quiet = 0; avg = avg * 0.7 + e * 0.3;
    }
    return st;
}
inline size_t eventAt (const std::vector<size_t>& ev, size_t i, size_t k) { while (k + 1 < ev.size() && i >= ev[k + 1]) ++k; return k; }

// RING PAN: the sound travels around a virtual ring of SPEAKERS (decoded to stereo) from one random position to the next,
// STEPS times, holding at each for HOLD; DIRECT crossfades, CLOCK / ANTICLOCK travel round the ring.
// after CDP mchanpan 1 panprocess (standalone/mchanpan.c)
Chans ringpan (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const int N = clamp (p.i ("spk"), 2, 16), K = std::max (1, p.i ("steps"));
    RingDecoder rd (N); cx.seed (p["seed"]);
    std::vector<double> tg ((size_t) K + 1);
    tg[0] = 0;
    for (int k = 1; k <= K; ++k)
    {
        int s = (int) std::floor (cx.rand() * N) % N;
        if (s == (int) tg[(size_t) k - 1]) s = (s + 1 + (int) std::floor (cx.rand() * (N - 1))) % N;
        tg[(size_t) k] = s;
    }
    const int type = p.i ("type"); const double hold = p["hold"], seg = (double) n / K;
    Chans out (2, Buf (n));
    for (size_t i = 0; i < n; ++i)
    {
        double u = (double) i / seg; int j = std::min (K - 1, (int) u); double f = clampd ((u - j - hold) / std::max (1e-9, 1 - hold), 0, 1);
        double a = tg[(size_t) j], b = tg[(size_t) j + 1], gl, gr;
        if (type == 0) { double l1, r1, l2, r2; rd.gains (a, p["focus"], l1, r1); rd.gains (b, p["focus"], l2, r2); gl = l1 + (l2 - l1) * f; gr = r1 + (r2 - r1) * f; }
        else
        {
            double d = b - a; if (type == 1) { while (d < 0) d += N; } else { while (d > 0) d -= N; }
            rd.gains (a + d * f, p["focus"], gl, gr);
        }
        out[0][i] = (float) (x[i] * gl); out[1][i] = (float) (x[i] * gr);
        tick (cx, i, n);
    }
    return out;
}

// EVENT SWITCH: every event (separated by quiet or a sharp attack) jumps to another position on a virtual speaker ring:
// stepping round the ring by STEP, or at RANDOM; GROUP events stay together. after CDP mchanpan 2 / 10 (standalone/mchanpan.c)
Chans evswitch (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    const int N = clamp (p.i ("spk"), 2, 16); RingDecoder rd (N); cx.seed (p["seed"]);
    const auto ev = findEvents (x, sr, p["thresh"], p["minsil"]);
    std::vector<double> pos (ev.size()); int cur = 0, left = 0;
    for (size_t k = 0; k < ev.size(); ++k)
    {
        if (left <= 0)
        {
            left = p.i ("rndgrp") ? 1 + (int) std::floor (cx.rand() * std::max (1, p.i ("group"))) : std::max (1, p.i ("group"));
            if (k > 0)
            {
                if (p.i ("mode") == 0) cur = (cur + p.i ("step")) % N;
                else
                    for (int tries = 0; tries < 50; ++tries)
                    {
                        int s = (int) std::floor (cx.rand() * N) % N, d = std::abs (s - cur); d = std::min (d, N - d);
                        if (s != cur && (! p.i ("noadj") || d > 1 || N <= 3)) { cur = s; break; }
                    }
            }
        }
        --left; pos[k] = cur;
    }
    Chans out (2, Buf (n)); size_t k = 0; double sl = 0, srr = 0; const double a = std::exp (-1 / (0.004 * sr));
    for (size_t i = 0; i < n; ++i)
    {
        k = eventAt (ev, i, k); double gl, gr; rd.gains (pos[k], p["focus"], gl, gr);
        if (i == 0) { sl = gl; srr = gr; }
        sl = gl + (sl - gl) * a; srr = gr + (srr - gr) * a;
        out[0][i] = (float) (x[i] * sl); out[1][i] = (float) (x[i] * srr);
        tick (cx, i, n);
    }
    return out;
}

// SPREAD: the sound spreads from CENTRE over more and more speakers of a virtual ring (each fed with a tiny different delay
// so the stereo fold opens up): gradually from SPREAD to SPREAD 2, or a step wider at each event. DEPTH leaves a hole in the
// middle, ROLLOFF lowers the level as it spreads. after CDP mchanpan 3-4 do_span / panspread (standalone/mchanpan.c)
Chans spread (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    const int N = clamp (p.i ("spk"), 2, 16); RingDecoder rd (N);
    const bool stepwise = p.i ("mode") == 0;
    std::vector<size_t> ev; if (stepwise) ev = findEvents (x, sr, p["thresh"], p["minsil"]);
    std::vector<size_t> dl ((size_t) N); for (int k = 0; k < N; ++k) dl[(size_t) k] = (size_t) (sr * 0.0023 * ((k * 5) % N));
    const double c = p["centre"] - 1, depth = p["depth"], s0 = p["spread"], s1 = p["spread2"];
    Chans out (2, Buf (n)); std::vector<double> g ((size_t) N), gs ((size_t) N, 0.0); size_t ek = 0;
    const double a = std::exp (-1 / (0.01 * sr));
    for (size_t i = 0; i < n; ++i)
    {
        double s;
        if (stepwise) { ek = eventAt (ev, i, ek); s = s1 >= s0 ? std::min (s1, s0 + (double) ek) : std::max (s1, s0 - (double) ek); }
        else s = s0 + (s1 - s0) * (double) i / (double) std::max ((size_t) 1, n - 1);
        if (! (i & 15))
        {
            double half = s / 2, sum = 0;
            for (int k = 0; k < N; ++k)
            {
                double d = std::fmod (k - c + 1.5 * N, (double) N) - N / 2.0, ad = std::abs (d), v = 0;
                if (ad <= half) v = 1; else if (ad < half + 1) v = half + 1 - ad;
                if (ad < half - depth) v = 0;   // hole behind the leading edges
                g[(size_t) k] = v; sum += v;
            }
            double norm = 1 / std::pow (std::max (1.0, sum), 0.5 * p["rolloff"]);
            for (auto& v : g) v *= norm;
        }
        double L = 0, R = 0;
        for (int k = 0; k < N; ++k)
        {
            gs[(size_t) k] = g[(size_t) k] + (gs[(size_t) k] - g[(size_t) k]) * a;
            if (gs[(size_t) k] < 1e-6) continue;
            double v = i >= dl[(size_t) k] ? x[i - dl[(size_t) k]] : 0.0;
            L += v * gs[(size_t) k] * rd.sl[(size_t) k]; R += v * gs[(size_t) k] * rd.sr[(size_t) k];
        }
        out[0][i] = (float) L; out[1][i] = (float) R;
        tick (cx, i, n);
    }
    return out;
}

// ANTIPHONY: switches between a left and a right group: by EVENTS, or by cutting the sound into EVENT DUR pieces
// separated by GAPs (tempo-syncable). after CDP mchanpan 5-6 antiphony (standalone/mchanpan.c)
Chans antiphony (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    double la, ra, lb, rb; cdpPan (-p["width"], la, ra); cdpPan (p["width"], lb, rb);
    if (p.i ("mode") == 0)
    {
        const auto ev = findEvents (x, sr, p["thresh"], p["minsil"]);
        Chans out (2, Buf (n)); size_t k = 0; double sl = la, srr = ra; const double a = std::exp (-1 / (0.004 * sr));
        for (size_t i = 0; i < n; ++i)
        {
            k = eventAt (ev, i, k); double gl = k % 2 ? lb : la, gr = k % 2 ? rb : ra;
            sl = gl + (sl - gl) * a; srr = gr + (srr - gr) * a;
            out[0][i] = (float) (x[i] * sl); out[1][i] = (float) (x[i] * srr);
            tick (cx, i, n);
        }
        return out;
    }
    const int sync = p.i ("sync");
    const double ed = sync > 0 ? divSec (sync - 1, cx.bpm) : p["evdur"];
    const size_t E = std::max ((size_t) 32, (size_t) (ed * sr)), G = (size_t) (p["gap"] * sr), S = std::min (E / 2, (size_t) (p["splice"] / 1000 * sr) + 1);
    const size_t pieces = (n + E - 1) / E, outLen = capLen ((double) (pieces * (E + G)), cx);
    Chans out (2, Buf (outLen));
    for (size_t k = 0; k < pieces; ++k)
    {
        double gl = k % 2 ? lb : la, gr = k % 2 ? rb : ra; size_t o0 = k * (E + G);
        for (size_t j = 0; j < E && k * E + j < n && o0 + j < outLen; ++j)
        {
            double env = std::min (1.0, std::min ((double) j / (double) S, (double) (E - j) / (double) S));
            double v = x[k * E + j] * env;
            out[0][o0 + j] = (float) (v * gl); out[1][o0 + j] = (float) (v * gr);
        }
        if (! (k & 63)) cx.prog ((k + 1.0) / (double) pieces);
    }
    return out;
}

// RING ROTATE: the sound circles a virtual ring of SPEAKERS from START SPK at SPEED cycles/s (gliding to SPEED 2); the rear
// half is quieter in the stereo fold. after CDP mchanpan 9 (standalone/mchanpan.c)
Chans ringrot (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    const int N = clamp (p.i ("spk"), 2, 16); RingDecoder rd (N); const double dir = p.i ("dir") ? -1 : 1;
    Chans out (2, Buf (n)); double pos = p["stt"] - 1, gl = 0, gr = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double t = (double) i / (double) std::max ((size_t) 1, n - 1), sp = p["speed"] + (p["speed2"] - p["speed"]) * t;
        if (! (i & 15)) rd.gains (pos, p["focus"], gl, gr);
        pos += dir * sp * N / sr; if (std::abs (pos) > 1e6) pos = std::fmod (pos, (double) N);
        out[0][i] = (float) (x[i] * gl); out[1][i] = (float) (x[i] * gr);
        tick (cx, i, n);
    }
    return out;
}

// SPACE SHRED: shreds the sound (random cuts re-spliced in random order) and deals the chunks out over a random
// permutation of virtual SPEAKERS, so the fragments scatter around the stereo field. STEREO mode keeps both channels and
// swaps them at random. after CDP mchshred shred 1-2 (standalone/mchshred.c)
Chans mchshred (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const bool keep = p.i ("mode") == 1;
    Chans src = keep ? stereoOf (ch) : Chans { monoOf (ch) };
    const size_t n = src[0].size(), S = std::max ((size_t) 2, (size_t) (0.006 * sr));
    const int cc = (int) std::min (2000.0, std::round ((double) n / sr / p["chunk"]));
    const int N = clamp (p.i ("spk"), 2, 16); RingDecoder rd (N);
    cx.seed (p["seed"]);
    if (cc < 2 || n < 4 * S) return stereoOf (src);
    const int reps = std::max (1, p.i ("repeats"));
    ShredPlan sp;
    for (int r = 0; r < reps; ++r)
    {
        sp = shredPlan (n, S, cc, p["scatter"], cx);
        if (r + 1 < reps) for (auto& x : src) x = shredOnce (x, sp.ptr, sp.len, sp.perm, S, n - S);
        cx.prog (0.8 * (r + 1.0) / reps);
    }
    // last pass: each chunk goes to its own speaker (or channel order)
    std::vector<int> spk; for (int k = 0; k < N; ++k) spk.push_back (k);
    Chans out (2, Buf (n)); size_t pos = 0;
    for (size_t q = 0; q < sp.perm.size(); ++q)
    {
        if (q % (size_t) N == 0) for (int k = N - 1; k > 0; --k) std::swap (spk[(size_t) k], spk[(size_t) std::min ((double) k, std::floor (cx.rand() * (k + 1)))]);
        double gl, gr; rd.gains (spk[q % (size_t) N], 1, gl, gr); const bool swapLR = cx.rand() < 0.5;
        size_t st = sp.ptr[(size_t) sp.perm[q]], L = sp.len[(size_t) sp.perm[q]];
        for (size_t i = 0; i < L + S; ++i)
        {
            size_t s = st + i, d = pos + i; if (s >= n || d >= n) break;
            double g = 1; if (q > 0 && i < S) g = (double) i / (double) S;
            if (i >= L) { if (q + 1 == sp.perm.size()) break; g = 1 - (double) (i - L) / (double) S; }
            if (keep) { out[0][d] += (float) (src[swapLR ? 1 : 0][s] * g); out[1][d] += (float) (src[swapLR ? 0 : 1][s] * g); }
            else { out[0][d] += (float) (src[0][s] * g * gl); out[1][d] += (float) (src[0][s] * g * gr); }
        }
        pos += L;
    }
    cx.prog (1);
    return out;
}

// PHASE INVERT: flips the polarity of both channels or of one (pseudo-stereo / M-S tricks; try it with the dry mix).
// after CDP phase phase 1 (standalone/phase.c) and chanphase (new/chanphase.c)
Chans phinv (const Chans& ch, Ctx& cx, const Params& p)
{
    const int m = p.i ("mode"); Chans out = m == 0 ? ch : stereoOf (ch);
    for (size_t c = 0; c < out.size(); ++c)
        if (m == 0 || (m == 1 && c == 0) || (m == 2 && c == 1)) for (auto& v : out[c]) v = -v;
    cx.prog (1);
    return out;
}

// STEREO ENHANCE: widens the image by subtracting TRANSFER of each channel from the other (cross-channel phase cancellation).
// after CDP phase phase 2 (standalone/phase.c)
Chans phenh (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans s = stereoOf (ch); const double t = p["transfer"];
    for (size_t i = 0; i < s[0].size(); ++i) { double l = s[0][i], r = s[1][i]; s[0][i] = (float) (l - t * r); s[1][i] = (float) (r - t * l); tick (cx, i, s[0].size()); }
    return s;
}

// MONO TO STEREO: the stereo sound starts folded to mono and opens out to full stereo between START and END
// (END before START: from stereo closing to mono). MIX LEVEL is each channel's level in the mono fold.
// after CDP tostereo (standnew/tostereo.c)
Chans tostereo (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans s = stereoOf (ch); const size_t n = s[0].size();
    const double st = p["stt"], en = p["end"], ml = p["mixlev"]; const bool conv = en < st;
    const double a = std::min (st, en), span = std::max (1e-6, std::abs (en - st));
    for (size_t i = 0; i < n; ++i)
    {
        double t = (double) i / (double) std::max ((size_t) 1, n - 1), f = clampd ((t - a) / span, 0, 1);
        double tr = conv ? 1 - f : f;   // 0 = mono, 1 = stereo
        double ll = 0.5 + 0.5 * tr, lr = 1 - ll, boost = 1 + (ml * 2 - 1) * (1 - tr);
        double l = s[0][i], r = s[1][i];
        s[0][i] = (float) ((l * ll + r * lr) * boost); s[1][i] = (float) ((r * ll + l * lr) * boost);
        tick (cx, i, n);
    }
    return s;
}

// PHASOR: several copies of the sound drift ahead and behind each other in pitch packets (up and back, then down and back,
// PACKET times a second, at most SHIFT semitones) so they keep phasing and re-syncing; copies are spread across the stereo
// field and can be OFFSET in time. after CDP phasor (standnew/phasor.c)
Chans phasor (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    const int S = clamp (p.i ("streams"), 2, 8); const double frq = p["frq"], shift = p["shift"];
    const size_t maxOff = (size_t) (p["offset"] / 1000 * sr), outLen = capLen ((double) (n + maxOff), cx);
    Chans out (2, Buf (outLen));
    auto prof = [] (double ph) { ph -= std::floor (ph); double q = ph < 0.5 ? ph : ph - 0.5, v = q < 0.25 ? q / 0.25 : (0.5 - q) / 0.25; return ph < 0.5 ? v : -v; };
    for (int k = 0; k < S; ++k)
    {
        double mx = std::min (shift, k * shift / (S - 1)) / 12.0;   // octaves
        double mean = 0; const int M = 4096; for (int j = 0; j < M; ++j) mean += std::pow (2.0, prof ((j + 0.5) / M) * mx); mean /= M;
        double pan = -p["spread"] + 2 * p["spread"] * k / (S - 1), gl, gr; cdpPan (pan, gl, gr);
        size_t off = (size_t) std::round ((double) maxOff * k / (S - 1));
        double pos = 0, ph = 0, g = 1.0 / std::sqrt ((double) S);
        for (size_t i = 0; off + i < outLen && pos < (double) n; ++i)
        {
            double v = lerpAt (x, pos) * g;
            out[0][off + i] += (float) (v * gl); out[1][off + i] += (float) (v * gr);
            pos += k == 0 ? 1.0 : std::pow (2.0, prof (ph) * mx) - (mean - 1);
            ph += frq / sr; if (ph >= 1) ph -= 1;
        }
        cx.prog ((k + 1.0) / S);
    }
    return out;
}

// TUNED DELAY: a feedback delay tuned to a PITCH (gliding to PITCH 2): the sound resonates like a plucked string at that note.
// after CDP newdelay (new/newdelay.c)
Chans newdelay (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t n = ch[0].size();
    auto m2h = [] (double m) { return 440.0 * std::pow (2.0, (m - 69) / 12); };
    const double fb = clampd (p["feedback"], 0, 0.999), mix = p["mix"], rescale = 1 / (fb + 1), fwd = (1 - mix) / rescale;
    const double dmax = sr / std::min (m2h (p["pitch"]), m2h (p["pitch2"]));
    const double tail = std::min (20.0, fb > 0.001 ? dmax / sr * std::log (0.001) / std::log (fb) : 0.0);
    const size_t outLen = capLen ((double) n + tail * sr, cx);
    Chans out;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        Ring r ((size_t) dmax + 8); Buf y (outLen);
        for (size_t i = 0; i < outLen; ++i)
        {
            double t = (double) i / (double) std::max ((size_t) 1, n - 1), m = p["pitch"] + (p["pitch2"] - p["pitch"]) * std::min (1.0, t);
            double d = std::max (1.0, sr / m2h (m)), in = i < n ? ch[c][i] * rescale : 0.0, dv = r.read (d);
            y[i] = (float) (in * fwd + dv * mix); r.push (dv * fb + in);
            tick (cx, i, outLen, (double) c / ch.size(), (c + 1.0) / ch.size());
        }
        out.push_back (std::move (y));
    }
    return out;
}

// TANGENT: repetitions of the sound fly along a tangent towards (or away from) a focal point: they get louder, less
// distant (less filtered) and their angle closes in; an 8-speaker ring decoded to stereo.
// after CDP tangent onefile / twofiles / sequence / list (new/tangent.c)
Chans tangent (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    const int K = clamp (p.i ("steps"), 2, 200); const bool recede = p.i ("mode") == 1; const double side = p.i ("side") ? -1 : 1;
    const double dur = p["dur"], iv = dur / K;
    const size_t outLen = capLen (dur * sr + (double) n, cx);
    Buf far (n); { OnePole lp; for (size_t i = 0; i < n; ++i) far[i] = (float) lp.lp (x[i], 1200, sr); }
    RingDecoder rd (8); cx.seed (p["seed"]);
    Chans out (2, Buf (outLen));
    for (int k = 0; k < K; ++k)
    {
        double u = (double) k / (K - 1), e = 2 * std::max (0.05, p["slow"]);
        double w = recede ? 1 - std::pow (1 - u, 1 / e) : std::pow (1 - u, e);   // 1 = far away, 0 = at the focus
        int lev = recede ? k : K - 1 - k;
        double amp = std::pow (p["dec"], lev), ang = p["angle"] * w, pos = (p["focus"] - 1) + side * ang / 45.0, gl, gr;
        rd.gains (pos, 1, gl, gr);
        double t = k * iv + (cx.rand() * 2 - 1) * p["jitter"] * iv * 0.5; size_t o = (size_t) std::max (0.0, t * sr);
        double fm = p["bal"] * w;
        for (size_t i = 0; i < n && o + i < outLen; ++i)
        {
            double v = (x[i] * (1 - fm) + far[i] * fm) * amp;
            out[0][o + i] += (float) (v * gl); out[1][o + i] += (float) (v * gr);
        }
        cx.prog ((k + 1.0) / K);
    }
    return out;
}

// CRUMBLE: the sound starts whole (on all speakers), then from START it is cut into segments dealt to 2, then 4 (then 8)
// groups of a virtual speaker ring, and finally to single speakers: it crumbles into ever smaller fragments around the
// stereo field. after CDP crumble sound 1-2 (science/crumble.c)
Chans crumble (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size(); const double sr = cx.sr;
    const int N = p.i ("mode") ? 16 : 8; RingDecoder rd (N); cx.seed (p["seed"]);
    const double stt = std::min (p["stt"] * sr, (double) n);
    std::vector<double> bounds { stt, stt + p["dur1"] * sr, stt + (p["dur1"] + p["dur2"]) * sr };
    if (N == 16) bounds.push_back (bounds.back() + p["dur3"] * sr);
    const size_t outLen = capLen ((double) n * std::max (1.0, p["ostrch"]) * 1.2 + sr, cx);
    Chans out (2, Buf (outLen));
    double cg = 0; for (int k = 0; k < N; ++k) cg += rd.sl[(size_t) k] + rd.sr[(size_t) k]; cg /= N;
    for (size_t i = 0; i < (size_t) stt && i < outLen; ++i) { out[0][i] = (float) (x[i] * cg); out[1][i] = (float) (x[i] * cg); }
    double cut = stt, opos = stt; int guard = 0; const double spl = 0.015 * sr;
    while (cut < (double) n - 1 && opos < (double) outLen && guard++ < 200000)
    {
        int stage = 0; while (stage + 1 < (int) bounds.size() && opos >= bounds[(size_t) stage + 1]) ++stage;
        int groups = opos >= bounds.back() ? N : std::min (N, 2 << stage);
        double seg = std::max (0.005, p["size"] * (1 + (cx.rand() * 2 - 1) * 0.5 * p["rand"])) * sr;
        double ratio = semis ((cx.rand() * 2 - 1) * p["pscat"]);
        int gsel = (int) std::floor (cx.rand() * groups) % groups, per = N / groups;
        double gl = 0, gr = 0; for (int k = 0; k < per; ++k) { gl += rd.sl[(size_t) (gsel * per + k)]; gr += rd.sr[(size_t) (gsel * per + k)]; }
        gl /= std::sqrt ((double) per); gr /= std::sqrt ((double) per);
        size_t L = (size_t) seg; double sp = std::max (1.0, std::min (spl, seg / 3));
        for (size_t j = 0; j < L; ++j)
        {
            size_t o = (size_t) opos + j; double sp2 = cut + (double) j * ratio; if (o >= outLen || sp2 >= (double) n - 1) break;
            double e = std::min (1.0, std::min ((double) j / sp, (double) (L - j) / sp));
            double v = lerpAt (x, sp2) * e;
            out[0][o] += (float) (v * gl); out[1][o] += (float) (v * gr);
        }
        cut += std::max (1.0, seg * ratio * (1 - p["iscat"] * cx.rand()));
        opos += std::max (1.0, seg * (1 - p["oscat"] * cx.rand() * 0.999) * p["ostrch"]);
        if (! (guard & 63)) cx.prog (cut / (double) n);
    }
    size_t end = std::min (outLen, (size_t) opos + (size_t) (p["size"] * sr * 2) + 1);
    for (auto& y : out) y.resize (std::max ((size_t) 1, end));
    return out;
}

// SPIN: a wide stereo image spins around the listener: the two edges swap through the centre at RATE, the edge moving across
// the front is BOOSTed and the rear one dimmed, ATTEN dips both as they cross the middle, DOPPLER bends their pitch.
// after CDP spin stereo 1-3 spindopl (science/spin.c)
Chans spin (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans s = stereoOf (ch); const size_t n = s[0].size(); const double sr = cx.sr;
    Chans out (2, Buf (n)); double th = 0, il = 0, ir = 0;
    const double rate = p["rate"], dop = p["dopl"] / 12, boostP = p["boost"], attP = p["atten"];
    for (size_t i = 0; i < n; ++i)
    {
        double pos = -std::cos (TAU * th), vel = std::sin (TAU * th) * (rate >= 0 ? 1 : -1);
        double lg, rg; cdpPan (pos, lg, rg);
        if (attP > 0) { double a = 1 - (1 - std::abs (pos)) * attP; lg *= a; rg *= a; }
        double ll = lg, lr = rg, rl = rg, rr = lg;
        if (boostP > 0)
        {
            double b = 1 + (1 - std::abs (pos)) * boostP;
            if (vel > 0) { ll *= b; lr *= b; rl /= b; rr /= b; } else { ll /= b; lr /= b; rl *= b; rr *= b; }
        }
        double lv = lerpAt (s[0], il), rv = lerpAt (s[1], ir);
        out[0][i] = (float) (lv * ll + rv * rl); out[1][i] = (float) (lv * lr + rv * rr);
        double inc = std::pow (2.0, dop * vel);
        il += inc; ir += 2 - inc;
        th += std::abs (rate) / sr; if (th >= 1) th -= 1;
        tick (cx, i, n);
    }
    return out;
}

// B-FORMAT PAN: pans the mono sound with first-order Ambisonics from START to END position (in revolutions: a fixed pan,
// or an orbit, negative = anticlockwise) and decodes it to stereo with two virtual cardioid mics WIDTH apart.
// after CDP abfpan (externals/mctools/abfpan.cpp)
Chans abfpan (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf x = monoOf (ch); const size_t n = x.size();
    const double a0 = -TAU * p["start"], a1 = -TAU * p["end"], phi = p["width"] * PI / 2;
    Chans out (2, Buf (n));
    for (size_t i = 0; i < n; ++i)
    {
        double t = (double) i / (double) std::max ((size_t) 1, n - 1), a = a0 + (a1 - a0) * t, s = x[i];
        double W = s * 0.7071, X = s * std::cos (a), Y = s * std::sin (a);
        out[0][i] = (float) (0.5 * (1.4142 * W + X * std::cos (phi) + Y * std::sin (phi)));
        out[1][i] = (float) (0.5 * (1.4142 * W + X * std::cos (phi) - Y * std::sin (phi)));
        tick (cx, i, n);
    }
    return out;
}

// ================================================================ REVERB building blocks (after CDP reverberator.cpp)
inline size_t toPrime (double dur, double sr)
{
    long long t = std::max (3LL, (long long) (dur * sr)); if (t % 2 == 0) ++t;
    auto prime = [] (long long v) { for (long long i = 3; i * i <= v; i += 2) if (v % i == 0) return false; return true; };
    while (! prime (t)) t += 2;
    return (size_t) t;
}
struct DLine { std::vector<double> b; size_t i = 0; explicit DLine (size_t n) : b (std::max ((size_t) 1, n), 0.0) {} double tick (double x) { double y = b[i]; b[i] = x; if (++i >= b.size()) i = 0; return y; } };
struct AP   // CDP allpassfilter, with an optional predelay before the write
{
    std::vector<double> b; size_t i = 0; double g; std::unique_ptr<DLine> pre;
    AP (size_t len, double gain, size_t predelay = 0) : b (std::max ((size_t) 1, len), 0.0), g (gain) { if (predelay) pre = std::make_unique<DLine> (predelay); }
    double tick (double x)
    {
        double out = -g * x + b[i], in = x + g * out;
        b[i] = pre ? pre->tick (in) : in; if (++i >= b.size()) i = 0;
        return out;
    }
};
struct CdpOnePole   // CDP onepole lowpass (0 Hz = bypass)
{
    double a1 = 1, a2 = 0, y = 0;
    CdpOnePole (double f, double sr) { if (f > 0) { double b = 2 - std::cos (TAU * std::min (f, sr * 0.45) / sr); a2 = std::sqrt (b * b - 1) - b; a1 = 1 + a2; } }
    double tick (double x) { y = a1 * x - a2 * y; return y; }
};
struct LPComb   // CDP lpcomb: a comb with a leaky integrator (lowpass1) in the loop
{
    std::vector<double> b; size_t i = 0; double g, lg, tmp = 0;
    LPComb (size_t len, double fb, double lpg) : b (std::max ((size_t) 1, len), 0.0), g (fb), lg (lpg) {}
    double tick (double x)
    {
        double out = b[i], lp = tmp; tmp = out + lp * lg;
        b[i] = x + g * lp; if (++i >= b.size()) i = 0;
        return out;
    }
};
struct Nested   // CDP nested_allpass: an outer allpass whose delay holds one or two inner allpasses
{
    std::vector<double> b; size_t i = 0; double g; std::unique_ptr<AP> a1, a2;
    Nested (size_t outer, size_t t1, size_t t2, double gain, double g1, double g2) : b (std::max ((size_t) 1, outer), 0.0), g (gain)
    { if (t1 && g1 > 0) a1 = std::make_unique<AP> (t1, g1); if (t2 && g2 > 0) a2 = std::make_unique<AP> (t2, g2); }
    double tick (double x)
    {
        double out = -g * x + b[i], in = x + g * out;
        double nest = a1 ? a1->tick (in) : in; if (a2) nest = a2->tick (nest);
        b[i] = nest; if (++i >= b.size()) i = 0;
        return out;
    }
};
// early reflections: CDP's small / medium / large tap tables are measured room responses; here a comparable irregular
// tap pattern is generated over the same time spans, with decaying gains
struct Earlies
{
    std::vector<size_t> d; std::vector<double> g, buf; size_t w = 0;
    Earlies (int room, double predelay, double sr)
    {
        const double T0[] = { 0.0082, 0.0332, 0.0729 }, T1[] = { 0.0975, 0.1540, 0.1800 };
        const int nt = 40; const double t0 = T0[clamp (room, 0, 2)], t1 = T1[clamp (room, 0, 2)], adj = predelay > 0 ? predelay - t0 : 0;
        for (int k = 0; k < nt; ++k)
        {
            double u = (double) k / (nt - 1), j = std::fmod (k * 0.6180339887, 1.0);
            double t = t0 + (t1 - t0) * std::pow (u, 1.4) + (k ? (j - 0.5) * 0.6 * (t1 - t0) / nt : 0);
            d.push_back ((size_t) std::max (1.0, (t + adj) * sr));
            g.push_back ((k ? 0.45 : 1.0) * std::exp (-2.3 * u) * (0.6 + 0.4 * j));
        }
        buf.assign (*std::max_element (d.begin(), d.end()) + 2, 0.0);
    }
    double tick (double x)
    {
        const size_t L = buf.size(); buf[w] = x; double v = 0;
        for (size_t k = 0; k < d.size(); ++k) v += buf[(w + L - d[k]) % L] * g[k];
        if (++w >= L) w = 0;
        return v / (double) d.size() * 4;
    }
};

// REVERB: Moorer reverb: early reflections, six lowpass combs and four allpasses, decorrelated to stereo by two allpasses.
// RVB TIME, DAMPING (high-frequency loss in the combs), DIFFUSE gain, input LOWPASS, PREDELAY, MIX and TAIL.
// after CDP reverb (externals/reverb/reverb.cpp, reverberator.cpp moorer)
Chans reverb (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; Chans s = stereoOf (ch); const size_t n = s[0].size();
    const double rt = p["rvbtime"], damp = p["damp"];
    const double ct[6] = { 0.050, 0.056, 0.061, 0.068, 0.072, 0.078 }, fg[6] = { 0.42, 0.46, 0.48, 0.49, 0.50, 0.52 };
    std::vector<LPComb> combs;
    for (int k = 0; k < 6; ++k) { double f = fg[k] * damp; combs.emplace_back (toPrime (ct[k], sr), std::exp (std::log (0.001) * ct[k] / rt) * (1 - f), f); }
    AP a1 (toPrime (0.02, sr), -0.6), a2 (toPrime (0.014, sr), 0.4), a3 (toPrime (0.009, sr), -0.61), a4 (toPrime (0.006, sr), 0.39);
    AP cl (toPrime (0.005, sr), 0.4), cr (toPrime (0.00525, sr), 0.4);
    CdpOnePole lp (p["lpfreq"] < 19999 ? p["lpfreq"] : 0, sr);
    const int room = rt <= 0.6 ? 0 : rt <= 1.3 ? 1 : 2;
    Earlies er (room, p["predelay"] / 1000, sr);
    const double wet = p["mix"], dry = 1 - wet, rg = p["rgain"];
    const size_t outLen = capLen ((double) n + p["tail"] * sr, cx);
    Chans out (2, Buf (outLen));
    for (size_t i = 0; i < outLen; ++i)
    {
        double l = i < n ? s[0][i] : 0.0, r = i < n ? s[1][i] : 0.0, m = lp.tick ((l + r) * 0.5);
        double e = er.tick (m), cin = e + m, acc = 0;
        for (auto& c : combs) acc += c.tick (cin);
        double o = e + a4.tick (a3.tick (a2.tick (a1.tick (acc / 6)))) * rg;
        out[0][i] = (float) (cl.tick (o) * wet + l * dry); out[1][i] = (float) (cr.tick (o) * wet + r * dry);
        tick (cx, i, outLen);
    }
    return out;
}

// ROOM REVERB: Gardner's nested-allpass room reverbs (SMALL, MEDIUM, LARGE) with early reflections; FEEDBACK sets the
// decay, ABSORB the lowpass in the loop, DIFFUSE the level of the dense part, MIX and TAIL.
// after CDP rmverb (externals/reverb/rmverb.cpp, reverberator.cpp small/medium/large_diffuser)
Chans rmverb (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; Chans s = stereoOf (ch); const size_t n = s[0].size();
    const int room = p.i ("room"); const double fbk = clampd (p["feedback"], 0, 0.99);
    CdpOnePole fl (p["absorb"], sr);
    std::unique_ptr<Nested> n1, n2; std::unique_ptr<AP> ap3, ap4; std::unique_ptr<DLine> d1, d2, d3, d4;
    if (room == 0)
    {
        n1 = std::make_unique<Nested> (toPrime (0.0047, sr), toPrime (0.022, sr), toPrime (0.0083, sr), 0.3, 0.4, 0.6);
        n2 = std::make_unique<Nested> (toPrime (0.036, sr), toPrime (0.03, sr), 0, 0.1, 0.4, 0.0);
        d1 = std::make_unique<DLine> ((size_t) (0.024 * sr));
    }
    else if (room == 1)
    {
        n1 = std::make_unique<Nested> (toPrime (0.035, sr), toPrime (0.0083, sr), toPrime (0.022, sr), 0.3, 0.7, 0.5);
        n2 = std::make_unique<Nested> (toPrime (0.039, sr), toPrime (0.0098, sr), 0, 0.3, 0.6, 0.0);
        ap3 = std::make_unique<AP> (toPrime (0.03, sr), 0.5, toPrime (0.005, sr));
        d1 = std::make_unique<DLine> (toPrime (0.108, sr)); d2 = std::make_unique<DLine> (toPrime (0.067, sr)); d3 = std::make_unique<DLine> (toPrime (0.015, sr));
    }
    else
    {
        n1 = std::make_unique<Nested> (toPrime (0.03, sr), toPrime (0.062, sr), 0, 0.5, 0.25, 0.0);
        n2 = std::make_unique<Nested> (toPrime (0.018, sr), toPrime (0.076, sr), toPrime (0.03, sr), 0.5, 0.25, 0.25);
        ap3 = std::make_unique<AP> (toPrime (0.008, sr), 0.3); ap4 = std::make_unique<AP> (toPrime (0.012, sr), 0.3);
        d1 = std::make_unique<DLine> ((size_t) (0.004 * sr)); d2 = std::make_unique<DLine> ((size_t) (0.017 * sr));
        d3 = std::make_unique<DLine> ((size_t) (0.031 * sr)); d4 = std::make_unique<DLine> ((size_t) (0.003 * sr));
    }
    double o1 = 0, o2 = 0, o3 = 0;
    auto diffuse = [&] (double in) -> double
    {
        if (room == 0)
        {
            double ip = d1->tick (in + fbk * fl.tick (o2));
            o1 = n1->tick (ip); o2 = n2->tick (o1); return (o1 + o2) * 0.5;
        }
        if (room == 1)
        {
            double f = fbk * fl.tick (d1->tick (o3));
            o1 = n1->tick (in + f); o2 = d2->tick (ap3->tick (o1)); o3 = n2->tick (in + fbk * d3->tick (o2));
            return 0.5 * (o1 + o2 + o3);
        }
        double f = fbk * fl.tick (o3);
        o1 = d1->tick (ap4->tick (ap3->tick (in + f))); o2 = d3->tick (n1->tick (d2->tick (o1))); o3 = n2->tick (d4->tick (o2));
        return o1 * 0.34 + o2 * 0.14 + o3 * 0.14;
    };
    AP l1 (toPrime (0.02, sr), -0.6), l2 (toPrime (0.014, sr), 0.4), r1 (toPrime (0.025, sr), -0.6), r2 (toPrime (0.0145, sr), 0.4);
    CdpOnePole lp (p["lpfreq"] < 19999 ? p["lpfreq"] : 0, sr);
    Earlies er (room, p["predelay"] / 1000, sr);
    const double wet = p["mix"] * 2, dry = 1 - p["mix"], rg = p["rgain"];
    const size_t outLen = capLen ((double) n + p["tail"] * sr, cx);
    Chans out (2, Buf (outLen));
    for (size_t i = 0; i < outLen; ++i)
    {
        double l = i < n ? s[0][i] : 0.0, r = i < n ? s[1][i] : 0.0, m = lp.tick ((l + r) * 0.5);
        double e = er.tick (m), o = e + diffuse (e + m) * rg * 0.707;
        if (! std::isfinite (o)) { o = 0; o1 = o2 = o3 = 0; }
        out[0][i] = (float) (l2.tick (l1.tick (o)) * wet + l * dry); out[1][i] = (float) (r2.tick (r1.tick (o)) * wet + r * dry);
        tick (cx, i, outLen);
    }
    return out;
}

// TAP DELAY: a stereo multi-tap delay: TAPS echoes spaced by TIME (or a tempo division) in a PATTERN (even, speeding up,
// slowing down, random), each DECAY quieter and placed by PANS; FEEDBACK recirculates the whole pattern.
// after CDP tapdelay / tdelay (externals/reverb/tdelaymain.cpp)
Chans tapdelay (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t n = ch[0].size(); const int K = clamp (p.i ("taps"), 1, 32), sync = p.i ("sync");
    const double T = sync > 0 ? divSec (sync - 1, cx.bpm) : p["time"] / 1000;
    cx.seed (p["seed"]);
    std::vector<size_t> d; std::vector<double> gL, gR; double t = 0, step = T, sumL = 0, sumR = 0;
    for (int k = 0; k < K; ++k)
    {
        switch (p.i ("pattern"))
        {
            case 0: t = T * (k + 1); break;
            case 1: t += step; step *= 0.8; break;
            case 2: t += step; step *= 1.25; break;
            default: t = T * (k + 1) + (cx.rand() - 0.5) * T * 0.9; break;
        }
        double pp = 0;
        switch (p.i ("pans")) { case 0: pp = k % 2 ? 1 : -1; break; case 1: pp = K == 1 ? 0 : -1 + 2.0 * k / (K - 1); break; case 2: pp = cx.rand() * 2 - 1; break; default: pp = 0; }
        double gl, gr, amp = std::pow (p["decay"], k); cdpPan (pp * p["spread"], gl, gr);
        d.push_back ((size_t) std::max (1.0, std::round (t * sr))); gL.push_back (amp * gl); gR.push_back (amp * gr);
        sumL += gL.back(); sumR += gR.back();
    }
    const double norm = std::min (2.0, (double) K) * 0.7 / std::max (1e-9, std::max (sumL, sumR));
    const size_t D = *std::max_element (d.begin(), d.end()) + 2;
    const double fb = clampd (p["feedback"], -0.99, 0.99), wet = p["mix"], dry = 1 - wet;
    const size_t outLen = capLen ((double) n + p["tail"] * sr, cx);
    Chans out (2, Buf (outLen));
    std::vector<double> bl (D, 0.0), br (D, 0.0); size_t w = 0; double lop = 0, rop = 0;
    for (size_t i = 0; i < outLen; ++i)
    {
        double il = i < n ? ch[0][i] : 0.0, ir = i < n ? chOr (ch, 1)[i] : 0.0, ip = (il + ir) * 0.5;
        bl[w] = ip + lop * fb; br[w] = ip + rop * fb;
        double L = 0, R = 0;
        for (int k = 0; k < K; ++k) { size_t j = (w + D - d[(size_t) k]) % D; L += bl[j] * gL[(size_t) k]; R += br[j] * gR[(size_t) k]; }
        lop = L * norm; rop = R * norm;
        out[0][i] = (float) (lop * wet + il * dry); out[1][i] = (float) (rop * wet + ir * dry);
        if (++w >= D) w = 0;
        tick (cx, i, outLen);
    }
    return out;
}

// ALLPASS: one long allpass filter: at short DELAY it smears transients and colours the phase (blend with MIX for comb
// notches), at long DELAY it rings out a train of alternating echoes. after CDP allpass (externals/reverb/allpass.c)
Chans allpass (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t n = ch[0].size();
    const size_t D = (size_t) std::max (1.0, p["delay"] / 1000 * sr); const double g = clampd (p["gain"], -0.99, 0.99);
    const double tail = std::abs (g) > 0.01 ? std::min (20.0, (double) D / sr * std::log (0.001) / std::log (std::abs (g))) : (double) D / sr;
    const size_t outLen = capLen ((double) n + tail * sr, cx);
    Chans out;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        std::vector<double> d1 (D, 0.0), d2 (D, 0.0); size_t k = 0; Buf y (outLen);
        for (size_t i = 0; i < outLen; ++i)
        {
            double ip = i < n ? ch[c][i] : 0.0, op = -g * ip + d1[k] + g * d2[k];
            d1[k] = ip; d2[k] = op; if (++k >= D) k = 0;
            y[i] = (float) (op * p["mix"] + ip * (1 - p["mix"]));
            tick (cx, i, outLen, (double) c / ch.size(), (c + 1.0) / ch.size());
        }
        out.push_back (std::move (y));
    }
    return out;
}

// LP COMB: a comb filter with a lowpass in its loop (the building block of the Moorer reverb): LOOP time sets the pitch
// of the ringing, RVB TIME how long it rings, DAMP how fast the highs die. after CDP lpcomb (externals/reverb/lpcomb.cpp)
Chans lpcomb (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t n = ch[0].size();
    const double loop = p["loop"] / 1000, rt = p["rvbtime"], f = p["damp"];
    const double g2 = std::exp (std::log (0.001) * loop / rt) * (1 - f);
    const size_t L = toPrime (loop, sr), outLen = capLen ((double) n + rt * sr, cx);
    Chans out;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        LPComb cb (L, g2, f); Buf y (outLen);
        for (size_t i = 0; i < outLen; ++i)
        {
            double ip = i < n ? ch[c][i] : 0.0;
            y[i] = (float) (cb.tick (ip) * p["mix"] + ip * (1 - p["mix"]));
            tick (cx, i, outLen, (double) c / ch.size(), (c + 1.0) / ch.size());
        }
        out.push_back (std::move (y));
    }
    return out;
}
} // namespace

void addCdpSpace (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CAT = "CDP \xc2\xb7 SPACE \xc2\xb7 DELAY";
    const std::string PAN = "CDP \xc2\xb7 SPACE \xc2\xb7 PAN", MOD = "CDP \xc2\xb7 SPACE \xc2\xb7 MODIFY";
    auto add = [&] (const std::string& cat, std::string id, std::string name, std::string tag, std::string desc, std::vector<ParamDef> ps, ProcFn fn, std::string cdp, std::string aux = {})
    { defs::add (v, cat, std::move (id), std::move (name), std::move (tag), std::move (desc), std::move (ps), fn, std::move (aux), false, std::move (cdp)); };

    // ---- pan / spatial ----
    add (PAN, "cdp_modify_space", "PAN", "PAN", "Places the (mono-mixed) sound in stereo, gliding from START to END position while an LFO (RATE, DEPTH) swings it; positions beyond \xc2\xb1" "1 sit outside the speakers and fade with distance.",
         { P ("start", "START", -2, 2, 0), P ("end", "END", -2, 2, 0), P ("rate", "LFO RATE", 0.01, 20, 0.5, PExp, "Hz"), P ("depth", "LFO DEPTH", 0, 2, 0.9), P ("prescale", "PRESCALE", 0.1, 1, 0.7) }, pan, "modify space 1");
    add (PAN, "cdp_modify_scaledpan", "PAN CURVE", "SPAN", "Pans along a SHAPE that is stretched over the whole sound: sweeps, back-and-forth CYCLES, a widening zigzag or random jumps. WIDTH scales the positions (above 1 = outside the speakers).",
         { Opt ("shape", "SHAPE", { "L \xe2\x86\x92 R", "R \xe2\x86\x92 L", "L-R-L", "R-L-R", "ZIGZAG OUT", "RANDOM" }, 2), P ("cycles", "CYCLES", 1, 32, 2, PInt), P ("width", "WIDTH", 0, 2, 1), P ("prescale", "PRESCALE", 0.1, 1, 0.7), Seed() }, scaledpan, "modify scaledpan");
    add (PAN, "cdp_modify_narrow", "STEREO NARROW", "NARW", "Narrows the stereo image: 1 leaves it, 0 folds it to mono, negative values push the channels through each other until -1 swaps (mirrors) left and right.",
         { P ("narrow", "NARROW", -1, 1, -0.5) }, narrow, "modify space 2 / modify space 4");
    add (PAN, "cdp_flutter_flutter", "FLUTTER", "FLUT", "A tremolo whose peaks hop between channel sets on every cycle (SEQUENCE: left/right, left/both/right...), so it flutters around the stereo field. DEPTH above 1 sharpens the peaks; RANDOM reshuffles the order.",
         { Opt ("seq", "SEQUENCE", { "L R", "L LR R", "L R LR", "LR L LR R" }), P ("rate", "RATE", 0.2, 40, 6, PExp, "Hz"), P ("depth", "DEPTH", 0, 16, 1), Opt ("random", "RANDOM", { "NO", "YES" }), Seed() }, flutter, "flutter flutter");
    add (PAN, "cdp_frame_shift", "FRAME ROTATE", "FRAM", "Rotates the whole stereo frame: left and right travel through each other at RATE (negative = other direction). ODD/EVEN moves each channel at its own RATE. SMEAR bleeds each channel into its neighbour.",
         { Opt ("mode", "MODE", { "WHOLE FRAME", "ODD/EVEN" }), P ("rate", "RATE", -40, 40, 0.5, 0, "Hz"), P ("rate2", "RATE 2", -40, 40, -0.33, 0, "Hz"), P ("smear", "SMEAR", 0, 0.5, 0.1) }, frame, "frame shift 1-2");
    add (PAN, "cdp_mchanpan_ringpan", "RING PAN", "RPAN", "Moves the sound around a virtual ring of SPEAKERS (folded to stereo, rear quieter) through STEPS random positions, holding at each for HOLD. PATH: DIRECT crossfade, or travel CLOCKWISE / ANTICLOCK round the ring. FOCUS < 1 spills to neighbours.",
         { P ("spk", "SPEAKERS", 2, 16, 8, PInt), P ("steps", "STEPS", 1, 64, 8, PInt), Pct ("hold", "HOLD", 0.3), Opt ("type", "PATH", { "DIRECT", "CLOCKWISE", "ANTICLOCK" }, 1), Pct ("focus", "FOCUS", 1), Seed() }, ringpan, "mchanpan 1");
    add (PAN, "cdp_mchanpan_switch", "EVENT SWITCH", "ESWI", "Each event (after a quiet gap of MINSIL, or a sharp attack above THRESH) jumps to a new spot on a virtual speaker ring: STEPping round it or at RANDOM (NO ADJACENT avoids neighbours). GROUP events share a spot.",
         { Opt ("mode", "MODE", { "STEP", "RANDOM" }, 1), P ("spk", "SPEAKERS", 2, 16, 8, PInt), P ("step", "STEP", 1, 8, 3, PInt), P ("group", "GROUP", 1, 8, 1, PInt), Opt ("rndgrp", "RND GROUP", { "NO", "YES" }),
           Opt ("noadj", "NO ADJACENT", { "NO", "YES" }), Pct ("focus", "FOCUS", 1), P ("thresh", "THRESH", -60, 0, -30, 0, {}, Fmt::db), P ("minsil", "MINSIL", 5, 500, 40, PExp, "ms"), Seed() }, evswitch, "mchanpan 2, 10");
    add (PAN, "cdp_mchanpan_spread", "SPREAD", "SPRD", "Spreads the sound from CENTRE over more speakers of a virtual ring, each fed with a tiny different delay so the stereo image opens: GRADUALLY from SPREAD to SPREAD 2, or one speaker wider per EVENT. DEPTH leaves a hole in the middle, ROLLOFF lowers the level.",
         { Opt ("mode", "MODE", { "PER EVENT", "GRADUAL" }, 1), P ("spk", "SPEAKERS", 2, 16, 8, PInt), P ("centre", "CENTRE", 1, 16, 1), P ("spread", "SPREAD", 0, 16, 0), P ("spread2", "SPREAD 2", 0, 16, 8),
           P ("depth", "DEPTH", 0, 8, 8), Pct ("rolloff", "ROLLOFF", 0.5), P ("thresh", "THRESH", -60, 0, -30, 0, {}, Fmt::db), P ("minsil", "MINSIL", 5, 500, 40, PExp, "ms") }, spread, "mchanpan 3-4");
    add (PAN, "cdp_mchanpan_antiphon", "ANTIPHONY", "ANTI", "Alternates between a left and a right group: event by EVENT, or by cutting the sound into EVENT DUR pieces (or tempo divisions) with GAPs between, SPLICE ms edges. WIDTH sets how far apart the groups are.",
         { Opt ("mode", "MODE", { "EVENTS", "CUT" }), Opt ("sync", "SYNC", SYNC_FREE, 4), P ("evdur", "EVENT DUR", 0.02, 4, 0.25, PExp, {}, Fmt::time), P ("gap", "GAP", 0, 2, 0, 0, {}, Fmt::time), P ("splice", "SPLICE", 1, 50, 5, 0, "ms"),
           P ("width", "WIDTH", 0, 1, 1), P ("thresh", "THRESH", -60, 0, -30, 0, {}, Fmt::db), P ("minsil", "MINSIL", 5, 500, 40, PExp, "ms") }, antiphony, "mchanpan 5-6");
    add (PAN, "cdp_mchanpan_rotate", "RING ROTATE", "RROT", "Circles the sound round a virtual ring of SPEAKERS (folded to stereo: front louder than the rear) from START SPK at SPEED rotations per second, gliding to SPEED 2. FOCUS < 1 blurs the position.",
         { P ("spk", "SPEAKERS", 2, 16, 8, PInt), P ("stt", "START SPK", 1, 16, 1, PInt), P ("speed", "SPEED", 0, 20, 0.25, 0, "Hz"), P ("speed2", "SPEED 2", 0, 20, 1, 0, "Hz"), Pct ("focus", "FOCUS", 1), Opt ("dir", "DIRECTION", { "CLOCKWISE", "ANTICLOCK" }) }, ringrot, "mchanpan 9");
    add (PAN, "cdp_spin_stereo", "SPIN", "SPIN", "Spins the stereo image around the listener: left and right swap through the centre at RATE (negative = other way), the edge crossing the front is BOOSTed and the rear one dimmed, ATTEN dips them at the centre, DOPPLER bends their pitch.",
         { P ("rate", "RATE", -10, 10, 0.3, 0, "Hz"), P ("dopl", "DOPPLER", 0, 12, 0.5, 0, "st"), P ("boost", "BOOST", 0, 2, 0.5), Pct ("atten", "ATTEN", 0.2) }, spin, "spin stereo 1-3");
    add (PAN, "cdp_abfpan", "B-FORMAT PAN", "ABF", "Pans the sound with first-order Ambisonics from START to END (in revolutions: equal = fixed position, a difference = an orbit, negative = anticlockwise) and decodes it with two virtual cardioid mics WIDTH apart.",
         { P ("start", "START", 0, 1, 0), P ("end", "END", -8, 8, 2, 0, "rev"), Pct ("width", "WIDTH", 1) }, abfpan, "abfpan");
    add (PAN, "cdp_tangent", "TANGENT", "TANG", "STEPS repeats fly along a tangent towards (APPROACH) or away from (RECEDE) the FOCUS speaker of a ring, closing in from ANGLE: each step DEC louder, less FILTERED; SLOW shapes the motion, JITTER the timing. DURATION of the train.",
         { Opt ("mode", "MODE", { "APPROACH", "RECEDE" }), P ("dur", "DURATION", 0.5, 30, 6, PExp, {}, Fmt::time), P ("steps", "STEPS", 2, 64, 12, PInt), P ("angle", "ANGLE", 45, 135, 120, 0, "deg"), P ("dec", "DEC", 0.5, 1, 0.85),
           P ("focus", "FOCUS", 1, 8, 1, PInt), Opt ("side", "SIDE", { "RIGHT", "LEFT" }), Pct ("jitter", "JITTER", 0.1), P ("slow", "SLOW", 0.1, 2, 0.5), Pct ("bal", "FILTERED", 0.6), Seed() }, tangent, "tangent onefile / twofiles / sequence / list");
    add (PAN, "cdp_crumble", "CRUMBLE", "CRUM", "The sound starts whole, then from START it is cut into segments dealt to 2 (for DUR 1), 4 (DUR 2), (8, DUR 3) groups of a speaker ring, then single speakers: it crumbles around the field. SIZE, RAND, IN/OUT SCAT, STRETCH, PITCH SCAT.",
         { Opt ("mode", "RING", { "8 SPEAKERS", "16 SPEAKERS" }), P ("stt", "START", 0, 10, 0.3, 0, {}, Fmt::time), P ("dur1", "DUR 1", 0, 10, 0.6, 0, {}, Fmt::time), P ("dur2", "DUR 2", 0, 10, 0.6, 0, {}, Fmt::time), P ("dur3", "DUR 3", 0, 10, 0.6, 0, {}, Fmt::time),
           P ("size", "SIZE", 0.01, 1, 0.08, PExp, {}, Fmt::time), Pct ("rand", "RAND", 0.5), Pct ("iscat", "IN SCAT", 0.3), Pct ("oscat", "OUT SCAT", 0.2), P ("ostrch", "STRETCH", 1, 8, 1.2, PExp, "x"), P ("pscat", "PITCH SCAT", 0, 12, 1, 0, "st"), Seed() }, crumble, "crumble sound 1-2");
    add (PAN, "cdp_mchshred", "SPACE SHRED", "MSHR", "Shreds the sound (random cuts re-spliced in random order, REPEATS times, chunks about CHUNK long, SCATTER) and deals the chunks over a random order of SPEAKERS around the field; STEREO keeps both channels and swaps them at random.",
         { Opt ("mode", "MODE", { "MONO TO RING", "STEREO" }), P ("repeats", "REPEATS", 1, 16, 2, PInt), P ("chunk", "CHUNK", 0.02, 2, 0.12, PExp, {}, Fmt::time), P ("scatter", "SCATTER", 0, 8, 1), P ("spk", "SPEAKERS", 2, 16, 8, PInt), Seed() }, mchshred, "mchshred shred 1-2");
    add (PAN, "cdp_phasor", "PHASOR", "PHSR", "STREAMS copies drift ahead of and behind each other in pitch packets (PACKET per second, up to SHIFT semitones), so they phase and re-sync; spread over SPREAD and OFFSET in time.",
         { P ("streams", "STREAMS", 2, 8, 4, PInt), P ("frq", "PACKET", 0.02, 20, 0.5, PExp, "Hz"), P ("shift", "SHIFT", 0, 12, 0.5, 0, "st"), P ("offset", "OFFSET", 0, 500, 0, 0, "ms"), P ("spread", "SPREAD", 0, 1, 1) }, phasor, "phasor phasor");
    add (PAN, "cdp_tostereo", "MONO TO STEREO", "TOST", "The stereo sound starts folded to mono and opens to full stereo between START and END (END before START closes it to mono). MIX LEVEL sets each channel's level in the mono fold.",
         { Pct ("stt", "START", 0.2), Pct ("end", "END", 0.8), P ("mixlev", "MIX LEVEL", 0.5, 1, 0.6) }, tostereo, "tostereo tostereo");
    add (PAN, "cdp_phase_enhance", "STEREO ENHANCE", "ENHC", "Widens the stereo image by subtracting TRANSFER of each channel from the other: what is common to both gets thinner, the sides stand out.",
         { Pct ("transfer", "TRANSFER", 0.6) }, phenh, "phase phase 2");
    add (PAN, "cdp_phase_invert", "PHASE INVERT", "PINV", "Inverts the polarity of BOTH channels, or of the LEFT or RIGHT only (pseudo-stereo, hollow mid). Most useful with the dry mix.",
         { Opt ("mode", "INVERT", { "BOTH", "LEFT", "RIGHT" }, 2) }, phinv, "phase phase 1 / chanphase");

    // ---- delay ----
    add (CAT, "cdp_modify_revecho", "ECHO / COMB", "ECHC", "Delay with FEEDBACK (negative = inverted repeats) and MIX; short DELAY times become a metallic comb. INVERT flips the dry signal (allpass-like colour). TAIL lets the echoes ring out.",
         { P ("delay", "DELAY", 0.1, 5000, 300, PExp, "ms"), Pct ("mix", "MIX", 0.5), P ("feedback", "FEEDBACK", -1, 1, 0.55), P ("tail", "TAIL", 0, 20, 3, 0, "s"), Opt ("invert", "INVERT DRY", { "NO", "YES" }) }, echo, "modify revecho 1 / delay");
    add (CAT, "cdp_modify_vdelay", "MOD DELAY", "MDLY", "Delay whose time is swept by an LFO: chorus, flanger or seasick echoes. MOD DEPTH is the sweep as a share of DELAY, RANDOM makes the LFO speed wander, LFO DELAY holds it still at the start.",
         { P ("delay", "DELAY", 0.5, 2000, 12, PExp, "ms"), Pct ("mix", "MIX", 0.5), P ("feedback", "FEEDBACK", -1, 1, 0.5), Pct ("mod", "MOD DEPTH", 0.6), P ("lfofrq", "LFO RATE", 0.01, 20, 0.4, PExp, "Hz"),
           Opt ("lfotype", "LFO", { "SINE", "RANDOM" }), Pct ("phase", "LFO PHASE", 0), P ("lfodelay", "LFO DELAY", 0, 10, 0, 0, "s"), P ("tail", "TAIL", 0, 20, 1, 0, "s"), Seed() }, vdelay, "modify revecho 2");
    add (CAT, "cdp_modify_stadium", "STADIUM ECHO", "STAD", "Dozens of decaying echoes scattered around the stereo field, like a PA in a stadium. SIZE scales the echo times, COUNT the number of echoes, ROLLOFF the level loss; CENTRE and SPREAD place the echo field.",
         { P ("gain", "GAIN", 0.1, 1, 0.645654), P ("rolloff", "ROLLOFF", 0, 1, 1), P ("size", "SIZE", 0.1, 10, 1, PExp, "x"), P ("count", "COUNT", 2, 200, 23, PInt), P ("centre", "CENTRE", -1, 1, 0), P ("spread", "SPREAD", 0, 1, 1) }, stadium, "modify revecho 3 / mchanrev");
    add (CAT, "cdp_newdelay", "TUNED DELAY", "TDLY", "A feedback delay tuned to a note: the sound rings at PITCH (gliding to PITCH 2) like a plucked string or metal tube. FEEDBACK sets how long it rings, MIX the balance.",
         { P ("pitch", "PITCH", 24, 108, 45, 0, {}, Fmt::note), P ("pitch2", "PITCH 2", 24, 108, 45, 0, {}, Fmt::note), Pct ("mix", "MIX", 0.6), P ("feedback", "FEEDBACK", 0, 0.999, 0.95) }, newdelay, "newdelay newdelay");
    add (CAT, "cdp_tapdelay", "TAP DELAY", "TAPD", "Stereo multi-tap delay: TAPS echoes spaced by TIME or a tempo SYNC in a PATTERN (even, speeding up, slowing down, random), each DECAY quieter, placed by PANS across SPREAD; FEEDBACK repeats the pattern.",
         { P ("taps", "TAPS", 1, 32, 6, PInt), Opt ("sync", "SYNC", SYNC_FREE, 4), P ("time", "TIME", 5, 2000, 150, PExp, "ms"), Opt ("pattern", "PATTERN", { "EVEN", "SPEED UP", "SLOW DOWN", "RANDOM" }), P ("decay", "DECAY", 0.3, 1, 0.8),
           Opt ("pans", "PANS", { "ALTERNATE", "SWEEP", "RANDOM", "CENTRE" }), Pct ("spread", "SPREAD", 0.9), P ("feedback", "FEEDBACK", -0.99, 0.99, 0.3), Pct ("mix", "MIX", 0.5), P ("tail", "TAIL", 0, 20, 3, 0, "s"), Seed() }, tapdelay, "tapdelay (tdelay)");
    add (CAT, "cdp_allpass", "ALLPASS", "ALLP", "A single long allpass filter: short DELAY smears and phase-colours, long DELAY rings out a train of alternating echoes. GAIN (negative flips them) sets the ringing, MIX with the dry gives comb notches.",
         { P ("delay", "DELAY", 0.1, 1000, 40, PExp, "ms"), P ("gain", "GAIN", -0.99, 0.99, 0.7), Pct ("mix", "MIX", 0.7) }, allpass, "allpass");
    add (CAT, "cdp_lpcomb", "LP COMB", "LPCB", "A comb filter with a lowpass in the loop: LOOP time sets the ringing pitch, RVB TIME how long it rings, DAMP how fast the highs die. The building block of the classic reverb.",
         { P ("loop", "LOOP", 0.5, 500, 8, PExp, "ms"), P ("rvbtime", "RVB TIME", 0.05, 15, 2, PExp, {}, Fmt::time), P ("damp", "DAMP", 0, 0.95, 0.4), Pct ("mix", "MIX", 0.6) }, lpcomb, "lpcomb");
    add (CAT, "cdp_reverb", "REVERB", "RVRB", "Moorer reverb: early reflections, six damped combs and four allpasses, two decorrelated outputs. RVB TIME, DAMPING (darker tail), DIFFUSE (dense level), LOWPASS on the input, PREDELAY, MIX and TAIL.",
         { P ("rvbtime", "RVB TIME", 0.1, 20, 2.5, PExp, {}, Fmt::time), Pct ("damp", "DAMPING", 0.5), Pct ("rgain", "DIFFUSE", 0.8), P ("lpfreq", "LOWPASS", 200, 20000, 20000, PExp, {}, Fmt::hz), P ("predelay", "PREDELAY", 0, 200, 0, 0, "ms"),
           Pct ("mix", "MIX", 0.4), P ("tail", "TAIL", 0, 30, 3, 0, "s") }, reverb, "reverb");
    add (CAT, "cdp_rmverb", "ROOM REVERB", "RMVB", "Gardner nested-allpass room reverb in three ROOM sizes, with early reflections. FEEDBACK sets the decay, ABSORB the lowpass in the loop, DIFFUSE the dense level, LOWPASS the input, PREDELAY, MIX and TAIL.",
         { Opt ("room", "ROOM", { "SMALL", "MEDIUM", "LARGE" }, 1), P ("feedback", "FEEDBACK", 0, 0.99, 0.6), P ("absorb", "ABSORB", 500, 15000, 4000, PExp, {}, Fmt::hz), Pct ("rgain", "DIFFUSE", 0.9), P ("lpfreq", "LOWPASS", 200, 20000, 20000, PExp, {}, Fmt::hz),
           P ("predelay", "PREDELAY", 0, 200, 0, 0, "ms"), Pct ("mix", "MIX", 0.35), P ("tail", "TAIL", 0, 30, 3, 0, "s") }, rmverb, "rmverb");

    // ---- modify ----
    add (MOD, "cdp_modify_speed", "VARISPEED", "VSPD", "Tape-speed change: pitch and length move together, gliding from START to END transposition across the sound. RATIO glides the speed linearly, SEMITONES glides the pitch.",
         { Opt ("mode", "GLIDE", { "RATIO", "SEMITONES" }, 1), P ("start", "START", -36, 36, -5, 0, "st"), P ("end", "END", -36, 36, 7, 0, "st") }, varispeed, "modify speed 1-2 / strans multi 1-2");
    add (MOD, "cdp_modify_accel", "ACCELERATE", "ACCL", "Plays normally until START, then accelerates to GOAL speed after GOAL TIME and keeps accelerating (GOAL below 1 slows down into a crawl).",
         { P ("goal", "GOAL", 0.05, 20, 4, PExp, "x"), P ("gtime", "GOAL TIME", 0.01, 20, 1.5, PExp, {}, Fmt::time), P ("stt", "START", 0, 20, 0.2, 0, {}, Fmt::time) }, accel, "modify speed 5 / strans multi 3");
    add (MOD, "cdp_modify_vibrato", "TAPE VIBRATO", "VIBR", "Vibrato by varispeed, like a warbling tape: RATE and DEPTH (semitones) move to RATE 2 and DEPTH 2 over the sound.",
         { P ("rate", "RATE", 0.1, 120, 6, PExp, "Hz"), P ("depth", "DEPTH", 0, 12, 1, 0, "st"), P ("rate2", "RATE 2", 0.1, 120, 6, PExp, "Hz"), P ("depth2", "DEPTH 2", 0, 12, 1, 0, "st") }, vibrato, "modify speed 6 / strans multi 4");
    add (MOD, "cdp_modify_reverse", "REVERSE", "REV", "Plays the sound backwards.", {}, reverse, "modify radical 1");
    add (MOD, "cdp_modify_shred", "SHRED", "SHRD", "Cuts the sound into chunks of about CHUNK length at scattered boundaries (SCATTER, above 1 = heavy) and re-splices them in random order, REPEATS times over: the length stays, the order dissolves.",
         { P ("repeats", "REPEATS", 1, 32, 3, PInt), P ("chunk", "CHUNK", 0.02, 2, 0.15, PExp, {}, Fmt::time), P ("scatter", "SCATTER", 0, 8, 1), Seed() }, shred, "modify radical 2");
    add (MOD, "cdp_modify_scrub", "SCRUB", "SCRB", "Winds the tape back and forth by hand between random points near the START and near the END: slow at the turns, fast in between, at speeds from DOWN to UP semitones, for DURATION.",
         { P ("dur", "DURATION", 0.5, 60, 8, PExp, {}, Fmt::time), P ("down", "DOWN", -48, 0, -12, 0, "st"), P ("up", "UP", 0, 48, 12, 0, "st"), Pct ("sttrange", "START", 0.3), Pct ("endstart", "END", 0.6), Opt ("fwdonly", "DIRECTION", { "BOTH", "FORWARD" }), Seed() }, scrub, "modify radical 3");
    add (MOD, "cdp_modify_lobit", "LOSE RESOLUTION", "LOBT", "Reduces the bit depth (BITS) and the sample rate (each RATE DIV samples are averaged and held): from warm grit to crushed digital noise.",
         { P ("bits", "BITS", 1, 16, 6, PInt), P ("srdiv", "RATE DIV", 1, 256, 4, PInt | PExp, "x") }, lobit, "modify radical 4");
    add (MOD, "cdp_modify_ringmod", "RING MOD", "RING", "Multiplies the sound by a sine tone gliding from FREQ to FREQ 2: metallic, bell-like sidebands; low settings tremolo.",
         { P ("frq", "FREQ", 1, 5000, 220, PExp, {}, Fmt::hz), P ("frq2", "FREQ 2", 1, 5000, 220, PExp, {}, Fmt::hz) }, ringmod, "modify radical 5");
    add (MOD, "cdp_modify_quantise", "QUANTISE", "QUAN", "Mid-rise quantisation to BITS bits: the waveform turns into stairs, quiet parts break up into buzz.",
         { P ("bits", "BITS", 1, 16, 4, PInt) }, quant, "modify radical 7");
    add (MOD, "cdp_modify_stack", "STACK", "STAK", "Layers COUNT tape-transposed copies, each INTERVAL apart, with their attacks lined up at ATTACK (seconds into the source). LEAN sets the lowest copy's level against the top one; LENGTH trims the result.",
         { P ("interval", "INTERVAL", -24, 24, 7, 0, "st"), P ("count", "COUNT", 2, 16, 4, PInt), P ("lean", "LEAN", 0.05, 4, 1, PExp, "x"), P ("attack", "ATTACK", 0, 2, 0, 0, {}, Fmt::time), P ("dur", "LENGTH", 0.1, 1, 1, 0, "x") }, stack, "modify stack");
}
} // namespace lab
