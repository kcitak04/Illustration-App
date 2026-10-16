// CDP collection, Batch 2 - Spectral B: hilite + Release 7/8 spectral.
// CDP works on phase-vocoder analysis files: per window and per channel an amplitude and a TRUE frequency.
// The helpers below reproduce that representation (analysis -> amp/frq frames -> resynthesis by phase
// accumulation), so each process can follow CDP's channel logic directly.
#include "LabCdp.h"
#include "LabDspUtil.h"

namespace lab
{
namespace
{
using namespace lab::dsp;

// ===================== phase-vocoder frames (CDP analysis-file equivalent) =====================
struct Spec
{
    size_t N = 0, H = 0, half = 0, frames = 0; double sr = 44100, chw = 1, nyq = 22050, ft = 0.01;   // ft = frame time (s)
    std::vector<float> amp, frq; DVec ph0;
    float* A (size_t f) { return amp.data() + f * (half + 1); }
    float* F (size_t f) { return frq.data() + f * (half + 1); }
    const float* A (size_t f) const { return amp.data() + f * (half + 1); }
    const float* F (size_t f) const { return frq.data() + f * (half + 1); }
    size_t C() const { return half + 1; }
    void resize (size_t nf) { frames = nf; amp.resize (nf * C(), 0.f); frq.resize (nf * C(), 0.f); }
    int chan (double hz) const { return (int) clamp ((long long) jr (hz / chw), 0LL, (long long) half); }
};

inline Spec pvAnal (const Buf& x, Ctx& cx, size_t N, size_t H, double p0, double p1)
{
    Spec s; s.N = N; s.H = H; s.half = N >> 1; s.sr = cx.sr; s.chw = cx.sr / (double) N; s.nyq = cx.sr / 2; s.ft = (double) H / cx.sr;
    const DVec w = hann (N);
    s.resize ((size_t) std::ceil ((double) x.size() / (double) H) + 1);
    DVec re (N), im (N), last (s.half + 1, 0.0); s.ph0.assign (s.half + 1, 0.0);
    const double ex = TAU * (double) H / (double) N;
    for (size_t f = 0; f < s.frames; ++f)
    {
        analyze (x, (long long) (f * H) - (long long) s.half, w, re, im);
        float* a = s.A (f); float* fr = s.F (f);
        for (size_t k = 0; k <= s.half; ++k)
        {
            double ph = std::atan2 (im[k], re[k]);
            a[k] = (float) std::hypot (re[k], im[k]);
            if (f == 0) { s.ph0[k] = ph; fr[k] = (float) ((double) k * s.chw); }
            else { double d = ph - last[k] - (double) k * ex; d -= TAU * jsround (d / TAU); fr[k] = (float) (((double) k + d / ex) * s.chw); }
            last[k] = ph;
        }
        if (! (f & 31)) cx.prog (p0 + (p1 - p0) * (double) f / (double) s.frames);
    }
    return s;
}

// resynthesis: each channel's phase advances by its (possibly changed) frequency
inline Buf pvSynth (const Spec& s, Ctx& cx, size_t outLen, double p0, double p1)
{
    const size_t N = s.N, H = s.H, half = s.half; const DVec w = hann (N);
    double sw = 0; for (double v : w) sw += v * v;
    const double g = (double) H / sw;
    Buf out (outLen); DVec re (N), im (N), acc = s.ph0;
    if (acc.size() != half + 1) acc.assign (half + 1, 0.0);
    const double inc = TAU * (double) H / s.sr;
    for (size_t f = 0; f < s.frames; ++f)
    {
        long long pos = (long long) (f * H) - (long long) half;
        if (pos >= (long long) outLen) break;
        const float* a = s.A (f); const float* fr = s.F (f);
        for (size_t k = 0; k <= half; ++k)
        {
            if (f > 0) { acc[k] += inc * (double) fr[k]; if (std::abs (acc[k]) > 1e6) acc[k] = std::fmod (acc[k], TAU); }
            double m = std::isfinite (a[k]) ? a[k] : 0.0;
            re[k] = m * std::cos (acc[k]); im[k] = m * std::sin (acc[k]);
        }
        mirror (re, im); synth (out, pos, w, re, im, g);
        if (! (f & 31)) cx.prog (p0 + (p1 - p0) * (double) f / (double) s.frames);
    }
    return out;
}

inline size_t winN (Ctx& cx, double win) { return std::min ((size_t) 16384, pow2 (clampd (win, 0.002, 1.0) * cx.sr)); }

// runs fn (spec, channel) on every channel's analysis and resynthesises. If fn changes the frame count,
// the output length follows it (capped at maxLen).
template <typename Fn> Chans specFx (const Chans& ch, Ctx& cx, double win, Fn fn, size_t hopDiv = 4)
{
    const size_t N = winN (cx, win), H = std::max ((size_t) 1, N / hopDiv), len = ch[0].size();
    Chans res; const double nc = (double) ch.size();
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const double b = (double) c / nc, sp = 1.0 / nc;
        Spec s = pvAnal (ch[c], cx, N, H, b, b + sp * 0.35);
        const size_t f0 = s.frames;
        fn (s, (int) c);
        cx.prog (b + sp * 0.6);
        size_t outLen = s.frames == f0 ? len : (size_t) std::max (1.0, (double) s.frames * (double) H - (double) (N >> 1));
        outLen = std::min (outLen, std::max ((size_t) 1, cx.maxLen));
        res.push_back (pvSynth (s, cx, outLen, b + sp * 0.6, b + sp));
    }
    return res;
}

// ---- frame helpers shared by many CDP programs ----
inline double totAmp (const float* a, size_t C) { double t = 0; for (size_t k = 0; k < C; ++k) t += a[k]; return t; }
inline void normaliseTo (float* a, size_t C, double pre)   // CDP normalise(): restore a window's total amplitude
{
    double post = totAmp (a, C);
    if (post > 1e-12 && pre > 0) { double g = std::min (1e4, pre / post); for (size_t k = 0; k < C; ++k) a[k] = (float) (a[k] * g); }
}
// spectral envelope (CDP specenv): maximum amplitude in bands a sixth of an octave wide (wider at the bottom),
// interpolated linearly in frequency
struct SpecEnv
{
    DVec cf, val, tmp; double width = 0;
    // minW: narrowest band (Hz); bands are then dilated by one on each side so the envelope bridges the gaps
    // between the harmonics instead of dipping into them
    void build (const float* a, const float* fr, const Spec& s, double minW = 50)
    {
        minW = std::max ({ minW, s.chw * 2, 20.0 });
        if (cf.empty() || std::abs (minW - width) > 0.1 * width)
        {
            width = minW; cf.clear(); double lo = 0;
            while (lo < s.nyq) { double hi = std::max (lo + minW, lo * std::pow (2.0, 1.0 / 6)); cf.push_back (std::min (s.nyq, hi)); lo = hi; }
            val.assign (cf.size(), 0); tmp.assign (cf.size(), 0);
        }
        std::fill (tmp.begin(), tmp.end(), 0.0);
        for (size_t k = 0; k <= s.half; ++k)
        {
            double f = std::abs ((double) fr[k]); size_t b = (size_t) (std::lower_bound (cf.begin(), cf.end(), f) - cf.begin());
            if (b >= cf.size()) b = cf.size() - 1;
            tmp[b] = std::max (tmp[b], (double) a[k]);
        }
        for (size_t b = 0; b < cf.size(); ++b) val[b] = std::max ({ tmp[b], tmp[b ? b - 1 : 0], tmp[std::min (b + 1, cf.size() - 1)] });
    }
    double at (double f) const
    {
        if (cf.empty()) return 0;
        // band b spans (cf[b-1], cf[b]]: its value sits at the band centre
        auto centre = [&] (size_t b) { double lo = b ? cf[b - 1] : 0.0; return 0.5 * (lo + cf[b]); };
        if (f <= centre (0)) return val[0];
        size_t b = (size_t) (std::lower_bound (cf.begin(), cf.end(), f) - cf.begin());
        if (b >= cf.size()) return val.back();
        size_t a = centre (b) > f ? (b ? b - 1 : 0) : b, c = std::min (a + 1, cf.size() - 1);
        double ca = centre (a), cc = centre (c);
        if (cc <= ca) return val[a];
        double u = clampd ((f - ca) / (cc - ca), 0, 1);
        return val[a] + (val[c] - val[a]) * u;
    }
};
// move every channel's data to the channel matching its (new) frequency, the louder one winning (CDP reposition)
inline void reposition (Spec& s, float* a, float* fr, std::vector<float>& ta, std::vector<float>& tf)
{
    ta.assign (s.C(), 0.f); tf.resize (s.C());
    for (size_t k = 0; k <= s.half; ++k) tf[k] = (float) ((double) k * s.chw);
    for (size_t k = 0; k <= s.half; ++k)
    {
        double f = fr[k]; if (! (f > 0) || f >= s.nyq || ! (a[k] > 0)) continue;
        int j = s.chan (f);
        if (a[k] > ta[(size_t) j]) { ta[(size_t) j] = a[k]; tf[(size_t) j] = (float) f; }
    }
    std::copy (ta.begin(), ta.end(), a); std::copy (tf.begin(), tf.end(), fr);
}
// transpose one window by ratio, optionally keeping the formant envelope (CDP spectrnsf / tranpose_within_formant_envelope)
inline void transposeFrame (Spec& s, float* a, float* fr, double ratio, bool keepFormants, SpecEnv& env, std::vector<float>& ta, std::vector<float>& tf)
{
    if (std::abs (ratio - 1) < 1e-9) return;
    const double pre = totAmp (a, s.C());
    if (keepFormants) env.build (a, fr, s);
    for (size_t k = 0; k <= s.half; ++k)
    {
        double f0 = std::abs ((double) fr[k]), f1 = f0 * ratio;
        if (keepFormants && f1 < s.nyq)
        {
            double e0 = env.at (f0), e1 = env.at (f1);
            a[k] = e0 < 1e-12 || e1 < 1e-12 ? 0.f : (float) (a[k] * std::min (100.0, e1 / e0));
        }
        fr[k] = (float) f1;
    }
    reposition (s, a, fr, ta, tf);
    normaliseTo (a, s.C(), pre);
}
// the n loudest channels (indices)
inline std::vector<size_t> loudest (const float* a, size_t C, size_t n, const std::vector<uint8_t>* allowed = nullptr)
{
    std::vector<size_t> idx; idx.reserve (C);
    for (size_t k = 0; k < C; ++k) if (! allowed || (*allowed)[k]) idx.push_back (k);
    n = std::min (n, idx.size());
    std::partial_sort (idx.begin(), idx.begin() + (long) n, idx.end(), [&] (size_t x, size_t y) { return a[x] > a[y]; });
    idx.resize (n); return idx;
}
inline double mtof (double m) { return 440.0 * std::pow (2.0, (m - 69) / 12); }
inline double ftom (double f) { return 69 + 12 * std::log2 (std::max (1e-6, f) / 440); }

// chords / scales used where CDP reads a tuning or pitch-template file (semitones above the root)
inline const std::vector<std::string> CHORDS { "MAJOR", "MINOR", "MAJ7", "MIN7", "DOM7", "SUS4", "MIN9", "PENTATONIC", "WHOLE TONE", "DIMINISHED", "HARMONICS", "CHROMATIC" };
inline std::vector<double> chordSemis (int i)
{
    static const std::vector<std::vector<double>> T {
        { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 4, 7, 10 }, { 0, 5, 7 }, { 0, 3, 7, 10, 14 },
        { 0, 2, 4, 7, 9 }, { 0, 2, 4, 6, 8, 10 }, { 0, 3, 6, 9 },
        { 0, 12, 19.02, 24, 27.86, 31.02, 33.69, 36 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };
    return T[(size_t) clamp (i, 0, (int) T.size() - 1)];
}
// every frequency of a chord on root (MIDI), in all octaves when field = true, sorted
inline DVec chordFreqs (int chord, double rootMidi, bool field, double nyq)
{
    DVec out; auto sem = chordSemis (chord);
    if (field)
    {
        for (int o = -10; o <= 10; ++o) for (double s : sem) { double f = mtof (rootMidi + s + 12 * o); if (f >= 16 && f < nyq) out.push_back (f); }
    }
    else for (double s : sem) { double f = mtof (rootMidi + s); if (f < nyq) out.push_back (f); }
    std::sort (out.begin(), out.end());
    out.erase (std::unique (out.begin(), out.end(), [] (double x, double y) { return std::abs (x - y) < 1e-6; }), out.end());
    if (out.empty()) out.push_back (std::min (nyq * 0.5, mtof (rootMidi)));
    return out;
}
inline double nearestIn (const DVec& set, double f)
{
    if (set.empty()) return f;
    auto it = std::lower_bound (set.begin(), set.end(), f);
    if (it == set.end()) return set.back();
    if (it == set.begin()) return *it;
    double hi = *it, lo = *(it - 1);
    return std::log (f / std::max (1e-9, lo)) < std::log (hi / std::max (1e-9, f)) ? lo : hi;
}

// ===================== hilite =====================
// SPECTRAL FILTER: brick-wall-with-skirts filter on each channel's true frequency (hi/lo/band/notch),
// optionally re-normalised window by window. After CDP hilite filter (specfilt, dev/hilite/hilite.c)
Chans hiliteFilter (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const bool norm = p.i ("norm") == 1;
    double f1 = p["frq1"], f2 = p["frq2"]; if (f1 > f2) std::swap (f1, f2);
    const double Q = std::max (1.0, p["q"]);
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const double fl = mode == 0 || mode == 1 ? f1 : f1, nyq = s.nyq;
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); const float* fr = s.F (f); const double pre = totAmp (a, s.C());
            for (size_t k = 0; k <= s.half; ++k)
            {
                double x = std::abs ((double) fr[k]), g = 1;
                switch (mode)
                {
                    case 0: { double lo = std::max (0.0, fl - Q); g = x < lo ? 0 : x < fl ? (x - lo) / std::max (1e-9, fl - lo) : 1; break; }   // high pass
                    case 1: { double hi = std::min (nyq, fl + Q); g = x > hi ? 0 : x > fl ? (hi - x) / std::max (1e-9, hi - fl) : 1; break; }   // low pass
                    case 2: { double lo = std::max (0.0, f1 - Q), hi = std::min (nyq, f2 + Q);                                                   // band pass
                              g = x < lo || x > hi ? 0 : x < f1 ? (x - lo) / std::max (1e-9, f1 - lo) : x > f2 ? (hi - x) / std::max (1e-9, hi - f2) : 1; break; }
                    default: { double lo = std::max (0.0, f1 - Q), hi = std::min (nyq, f2 + Q);                                                  // notch
                              g = x < lo || x > hi ? 1 : x < f1 ? 1 - (x - lo) / std::max (1e-9, f1 - lo) : x > f2 ? 1 - (hi - x) / std::max (1e-9, hi - f2) : 0; break; }
                }
                a[k] = (float) (a[k] * clampd (g, 0, 1));
            }
            if (norm) normaliseTo (a, s.C(), pre);
        }
    });
}

// SPECTRAL VOWELS: forces each window's spectral envelope onto three-formant vowel shapes (static, gliding
// A to B, or alternating), with ear-sensitivity weighting. After CDP hilite vowels (vowel_filter, dev/hilite/hilite.c)
const std::vector<std::string> VOWELS { "EE (heat)", "I (hit)", "E (pet)", "AI (hate)", "A (pat)", "AR (heart)", "O (hot)", "OR (taught)", "OA (boat)", "U (hood)", "OO (mood)", "XX (hub)", "X (herb)" };
Chans hiliteVowels (const Chans& ch, Ctx& cx, const Params& p)
{
    static const double V[13][5] = { { 300, 2600, 2800, 0.85, 0.85 }, { 360, 2100, 2300, 1, 1 }, { 570, 1970, 2450, 1, 0.75 }, { 465, 2250, 2450, 0.92, 0.25 },
        { 750, 1550, 2450, 1, 0.85 }, { 680, 1100, 2450, 1, 0.92 }, { 600, 900, 2450, 1, 0.5 }, { 450, 860, 2300, 1, 0.82 }, { 410, 810, 2300, 1, 0.82 },
        { 380, 850, 2100, 1, 0.6 }, { 300, 850, 2100, 1, 0.6 }, { 720, 1240, 2300, 0.85, 0.3 }, { 580, 1380, 2450, 1, 0.5 } };
    const int va = clamp (p.i ("va"), 0, 12), vb = clamp (p.i ("vb"), 0, 12), motion = p.i ("motion");
    const double hw = p["hwidth"], curv = p["steep"], rng = p["range"], thr = p["thresh"], rate = p["rate"];
    const double dur = std::max (1e-3, (double) ch[0].size() / cx.sr);
    auto sens = [] (double f)   // ear-sensitivity curve (define_sensitivity_curve)
    {
        static const double P[6][2] = { { 0, 1 }, { 250, 1 }, { 2000, 1 / 2.511 }, { 3000, 1 / 2.511 }, { 4000, 0.4 / 2.511 }, { 96000, 0.4 / 2.511 } };
        for (int i = 1; i < 6; ++i) if (f <= P[i][0]) return P[i - 1][1] + (P[i][1] - P[i - 1][1]) * (f - P[i - 1][0]) / (P[i][0] - P[i - 1][0]);
        return P[5][1];
    };
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        DVec vamp (s.C());
        for (size_t f = 0; f < s.frames; ++f)
        {
            double t = (double) f * s.ft, u = 0;
            if (motion == 1) u = clampd (t / dur, 0, 1);
            else if (motion == 2) u = 0.5 - 0.5 * std::cos (TAU * rate * t);
            double fm[3], at[3] = { 1, 0, 0 };
            for (int i = 0; i < 3; ++i) fm[i] = V[va][i] + (V[vb][i] - V[va][i]) * u;
            at[1] = V[va][3] + (V[vb][3] - V[va][3]) * u; at[2] = V[va][4] + (V[vb][4] - V[va][4]) * u;
            float* a = s.A (f); const float* fr = s.F (f);
            double pre = 0, mx = 0, mxv = 0;
            for (size_t k = 0; k <= s.half; ++k) { pre += a[k]; mx = std::max (mx, (double) a[k]); }
            for (size_t k = 0; k <= s.half; ++k)
            {
                double x = std::abs ((double) fr[k]), e = 0;
                for (int i = 0; i < 3; ++i)   // triangular formant peaks, the largest one wins where they overlap
                {
                    double h = fm[i] * hw; if (h <= 0) continue;
                    double d = 1 - std::abs (x - fm[i]) / h;
                    if (d > 0) e = std::max (e, d * at[i]);
                }
                e = std::pow (e, curv) * rng + (1 - rng);
                vamp[k] = e * sens (x); mxv = std::max (mxv, vamp[k]);
            }
            double sc = mxv > 0 ? mx / mxv : 0;
            for (size_t k = 0; k <= s.half; ++k) { double v = vamp[k] * sc; if (a[k] > v * thr) a[k] = (float) v; }
            normaliseTo (a, s.C(), pre);
        }
    });
}

// SPECTRAL GRAPHIC EQ: band-pass (or notch) filter bands placed on a chord / scale / harmonic series,
// each BANDWIDTH octaves wide. After CDP hilite greq (specgreq, dev/hilite/hilite.c)
Chans hiliteGreq (const Chans& ch, Ctx& cx, const Params& p)
{
    const bool notch = p.i ("mode") == 1; const double bw = std::max (0.01, p["bw"]);
    const DVec cfs = chordFreqs (p.i ("chord"), p["root"], true, cx.sr / 2);
    DVec bands; for (double f : cfs) if (f >= p["lo"] && f <= p["hi"]) bands.push_back (f);
    if (bands.empty()) bands.push_back (nearestIn (cfs, std::sqrt (std::max (1.0, p["lo"]) * std::max (1.0, p["hi"]))));
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); const float* fr = s.F (f);
            for (size_t k = 0; k <= s.half; ++k)
            {
                double x = std::abs ((double) fr[k]), g = 0;
                if (x > 1)
                {
                    double lx = std::log2 (x); auto it = std::lower_bound (bands.begin(), bands.end(), x);
                    for (int q = -1; q <= 0; ++q)
                    {
                        auto j = it + q; if (j < bands.begin() || j >= bands.end()) continue;
                        double d = std::abs (lx - std::log2 (*j)) / (bw * 0.5);
                        if (d < 1) g = std::max (g, 0.5 + 0.5 * std::cos (PI * d));
                    }
                }
                if (notch) g = 1 - g;
                a[k] = (float) (a[k] * g);
            }
        }
    });
}

// SPECTRAL BANDS: splits the spectrum into low / mid / high bands, each with its own level; the mid band can
// also be transposed or frequency-shifted, replacing or adding to the original. After CDP hilite band (specsplit)
Chans hiliteBand (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); double s1 = p["split1"], s2 = p["split2"]; if (s1 > s2) std::swap (s1, s2);
    const double gl = std::pow (10.0, p["glo"] / 20), gm = std::pow (10.0, p["gmid"] / 20), gh = std::pow (10.0, p["ghi"] / 20);
    const double ratio = std::pow (2.0, p["trans"] / 12), shift = p["shift"];
    const bool additive = mode == 2 || mode == 3, addTo = mode == 1 || mode == 3;
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        std::vector<float> na, nf;
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); float* fr = s.F (f);
            na.assign (a, a + s.C()); nf.assign (fr, fr + s.C());
            for (size_t k = 0; k <= s.half; ++k)
            {
                double x = std::abs ((double) fr[k]);
                if (x < s1) na[k] = (float) (a[k] * gl);
                else if (x >= s2) na[k] = (float) (a[k] * gh);
            }
            // mid band: transpose (work downwards when going up so data is not overwritten twice)
            const bool up = additive ? shift > 0 : ratio > 1;
            for (size_t i = 0; i <= s.half; ++i)
            {
                size_t k = up ? s.half - i : i;
                double x = std::abs ((double) fr[k]); if (x < s1 || x >= s2) continue;
                double amp = a[k] * gm, nfq = additive ? x + shift : x * ratio;
                if (nfq > 0 && nfq < s.nyq)
                {
                    size_t j = (size_t) s.chan (nfq);
                    if (j != k && (! addTo || amp > na[j])) { na[j] = (float) amp; nf[j] = (float) nfq; }
                }
                if (addTo) { na[k] = (float) amp; }
                else na[k] = (float) (0.5 * (a[k > 0 ? k - 1 : 0] + a[std::min (k + 1, s.half)]) * gm * 0.25);   // vacated channel: interpolated, faint
            }
            std::copy (na.begin(), na.end(), a); std::copy (nf.begin(), nf.end(), fr);
        }
    });
}

// SPECTRAL ARPEGGIO: a band sweeps through the spectrum (downramp / sine / saw / upramp); partials it touches
// are attacked and sustained, decaying over SUSTAIN windows. Eight CDP modes decide what plays outside the band.
// After CDP hilite arpeg (specarpe, dev/hilite/hilite.c)
Chans hiliteArpeg (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"), wave = p.i ("wave"); const int sust = std::max (1, p.i ("sust"));
    const double rate = p["rate"], ampl = p["amp"], nl = std::max (0.05, p["nonlin"]);
    double lo = p["lo"], hi = p["hi"]; if (lo > hi) std::swap (lo, hi);
    const bool fixedPitch = p.i ("hold") == 0, limitSus = p.i ("retrig") == 0, isNonlin = std::abs (nl - 1) > 1e-3;
    auto tab = [&] (double ph)
    {
        ph -= std::floor (ph);
        switch (wave)
        {
            case 0: return 1 - ph;
            case 1: return (std::sin (PI * 1.5 + TAU * ph) + 1) / 2;
            case 2: return ph < 0.5 ? ph * 2 : 2 - ph * 2;
            default: return ph;
        }
    };
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const double hiF = std::min (hi, s.nyq), range = hiF - lo, hb = std::max (s.chw, p["bw"]) / 2;
        std::vector<int> keep (s.C(), 0); std::vector<float> sa (s.C(), 0.f), sf (s.C(), 0.f);
        double phase = p["phase"], lastPh = phase; bool start = true;
        for (size_t f = 0; f < s.frames; ++f)
        {
            lastPh = phase; phase += s.ft * rate; double pw = phase - std::floor (phase), lw = lastPh - std::floor (lastPh);
            double mid = tab (phase) * range + lo, bHi = std::min (mid + hb, s.nyq), bLo = std::max (mid - hb, 0.0);
            float* a = s.A (f); float* fr = s.F (f);
            if (start)
            {
                if ((wave == 1 || wave == 2) && (mode == 3 || mode == 7)) { if (pw >= 0.5) start = false; }
                else if (pw < lw) start = false;
            }
            auto dec = [&] (size_t k) { double z = (double) keep[k] / (double) sust; return isNonlin ? std::pow (z, nl) : z; };
            auto initiate = [&] (size_t k) { keep[k] = sust; a[k] = (float) (a[k] * ampl); sa[k] = a[k]; sf[k] = fr[k]; keep[k]--; };
            auto sustain = [&] (size_t k) { a[k] = (float) (sa[k] * dec (k)); if (fixedPitch) fr[k] = sf[k]; keep[k]--; };
            auto boostOrOn = [&] (bool on)
            {
                for (size_t k = 0; k <= s.half; ++k)
                {
                    double x = fr[k]; bool in = x >= bLo && x <= bHi;
                    if (limitSus)
                    {
                        if (in)
                        {
                            double ta = sa[k] * dec (k);
                            if (keep[k] > 0 && ta > a[k]) { a[k] = (float) ta; if (fixedPitch) fr[k] = sf[k]; keep[k]--; }
                            else initiate (k);
                        }
                        else if (keep[k] > 0) sustain (k);
                        else if (on) a[k] = 0;
                    }
                    else
                    {
                        if (keep[k] > 0) sustain (k);
                        else if (in) initiate (k);
                        else if (on) a[k] = 0;
                    }
                }
            };
            switch (mode)
            {
                case 0: boostOrOn (true); break;                       // ON
                case 1: boostOrOn (false); break;                      // BOOST
                case 2: case 3:                                        // BELOW / ABOVE then BOOST
                    if (start) for (size_t k = 0; k <= s.half; ++k) { bool keepIt = mode == 2 ? fr[k] <= bHi : fr[k] >= bLo; a[k] = keepIt ? (float) (a[k] * ampl) : 0.f; }
                    else boostOrOn (false);
                    break;
                case 4: for (size_t k = 0; k <= s.half; ++k) a[k] = fr[k] > mid ? 0.f : (float) (a[k] * ampl); break;   // BELOW
                case 5: for (size_t k = 0; k <= s.half; ++k) a[k] = fr[k] < mid ? 0.f : (float) (a[k] * ampl); break;   // ABOVE
                default:                                               // ONCE BELOW / ONCE ABOVE
                    if (start) for (size_t k = 0; k <= s.half; ++k) { bool keepIt = mode == 6 ? fr[k] <= bHi : fr[k] >= bLo; a[k] = keepIt ? (float) (a[k] * ampl) : 0.f; }
                    break;
            }
        }
    });
}

// SPECTRAL PLUCK: channels that switch on (rise above THRESH after being below it) get an attack boost.
// After CDP hilite pluck (specpluck, dev/hilite/hilite.c)
Chans hilitePluck (const Chans& ch, Ctx& cx, const Params& p)
{
    const double gain = p["gain"], thrDb = p["thresh"];
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        std::vector<uint8_t> on (s.C(), 0);
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); double mx = 0; for (size_t k = 0; k <= s.half; ++k) mx = std::max (mx, (double) a[k]);
            const double thr = mx * std::pow (10.0, thrDb / 20);
            for (size_t k = 0; k <= s.half; ++k)
            {
                bool nz = mx > 0 && a[k] > thr;
                if (f == 0) { on[k] = nz; continue; }
                if (nz && ! on[k]) { a[k] = (float) (a[k] * gain); on[k] = 1; }
                else if (! nz) { on[k] = 0; a[k] = (float) (a[k] * 0.5); }
            }
        }
    });
}

// keeps the n loudest channels of a window (optionally only inside lo..hi, or topping up from outside)
inline void traceFrame (Spec& s, float* a, const float* fr, size_t n, int band, double lo, double hi, bool retain, std::vector<uint8_t>& in)
{
    in.assign (s.C(), 1);
    if (band) for (size_t k = 0; k <= s.half; ++k) { double x = fr[k]; in[k] = ! ((band == 1 || band == 3) && x < lo) && ! ((band == 2 || band == 3) && x > hi); }
    std::vector<uint8_t> keepK (s.C(), 0);
    for (size_t k : loudest (a, s.C(), n, &in)) keepK[k] = 1;
    size_t inCnt = 0; for (auto v : in) inCnt += v;
    if (retain && band && n > inCnt)
    {
        std::vector<uint8_t> out (s.C()); for (size_t k = 0; k <= s.half; ++k) out[k] = ! in[k];
        for (size_t k : loudest (a, s.C(), n - inCnt, &out)) keepK[k] = 1;
    }
    for (size_t k = 0; k <= s.half; ++k) if (! keepK[k]) a[k] = 0;
}

// SPECTRAL TRACE: keeps only the N loudest channels of each window (all, above LO, below HI, or between).
// After CDP hilite trace (spectrace, dev/hilite/hilite.c)
Chans hiliteTrace (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const size_t n = (size_t) std::max (1, p.i ("n")); const bool retain = p.i ("retain") == 1;
    double lo = p["lo"], hi = p["hi"]; if (mode == 3 && lo > hi) std::swap (lo, hi);
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        std::vector<uint8_t> in;
        for (size_t f = 0; f < s.frames; ++f) traceFrame (s, s.A (f), s.F (f), n, mode, lo, hi, retain, in);
    });
}

// CDP blur blur: keep every Nth window and interpolate amplitude and frequency linearly in between
inline void blurFrames (Spec& s, size_t B)
{
    if (B < 2 || s.frames < 3) return;
    for (size_t f0 = 0; f0 + 1 < s.frames; f0 += B)
    {
        size_t f1 = std::min (f0 + B, s.frames - 1);
        const float *a0 = s.A (f0), *a1 = s.A (f1), *r0 = s.F (f0), *r1 = s.F (f1);
        for (size_t f = f0 + 1; f < f1; ++f)
        {
            double u = (double) (f - f0) / (double) (f1 - f0); float* a = s.A (f); float* fr = s.F (f);
            for (size_t k = 0; k <= s.half; ++k) { a[k] = (float) (a0[k] + (a1[k] - a0[k]) * u); fr[k] = (float) (r0[k] + (r1[k] - r0[k]) * u); }
        }
    }
}

// BLUR + TRACE: time-averages the spectrum over BLUR, then keeps the N loudest channels.
// After CDP hilite bltr (spec_blur_and_bltr, dev/hilite/ap_hilite.c)
Chans hiliteBltr (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = (size_t) std::max (1, p.i ("n"));
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        blurFrames (s, (size_t) std::max (1.0, std::round (p["blur"] / 1000 / s.ft)));
        std::vector<uint8_t> in;
        for (size_t f = 0; f < s.frames; ++f) traceFrame (s, s.A (f), s.F (f), n, 0, 0, 0, false, in);
    });
}

// ===================== speclean / specnu =====================
// start sample of the noise print: the quietest stretch of LEN seconds, or the start / end of the sound
inline size_t noiseRegion (const Chans& ch, double sr, int where, double lenSec, size_t& nlen)
{
    const size_t n = ch[0].size(); nlen = std::min (n, (size_t) std::max (1.0, lenSec * sr));
    if (where == 1) return 0;
    if (where == 2) return n - nlen;
    Buf m = monoOf (ch); const size_t hop = std::max ((size_t) 1, nlen / 8);
    DVec ps (n + 1, 0.0); for (size_t i = 0; i < n; ++i) ps[i + 1] = ps[i] + (double) m[i] * m[i];
    size_t best = 0; double bv = 1e300;
    for (size_t a = 0; a + nlen <= n; a += hop) { double e = ps[a + nlen] - ps[a]; if (e < bv) { bv = e; best = a; } }
    return best;
}

// SPECTRAL DENOISE: a noise print (quietest stretch, start or end of the sound) sets a threshold per channel;
// channels must stay above it for PERSIST to pass (fading in and out), SUBTRACT also removes the noise level.
// After CDP speclean clean / specnu clean / specnu subtract (do_speclean, dev/standalone/speclean.c, specnu.c)
Chans specClean (const Chans& ch, Ctx& cx, const Params& p)
{
    const bool subtract = p.i ("mode") == 1; size_t nlen = 0;
    const size_t na = noiseRegion (ch, cx.sr, p.i ("nsrc"), p["nlen"], nlen);
    const double ngain = p["ngain"];
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const int P = std::max (1, (int) jr (p["persist"] / 1000 / s.ft));
        DVec noise (s.C(), 0.0);
        size_t fa = na / s.H, fb = std::max (fa + 1, (na + nlen) / s.H + 1);
        for (size_t f = fa; f < std::min (fb, s.frames); ++f) { const float* a = s.A (f); for (size_t k = 0; k < s.C(); ++k) noise[k] = std::max (noise[k], (double) a[k]); }
        for (auto& v : noise) v *= ngain;
        std::vector<int> pers (s.C(), 0); std::vector<uint8_t> on (s.C(), 0);
        const Spec src = s;   // decisions look ahead P-1 windows into the unprocessed data
        auto above = [&] (size_t f, size_t k) { return f < src.frames && src.A (f)[k] > noise[k]; };
        for (size_t k = 0; k < s.C(); ++k)   // first group: count the windows above threshold backwards from the newest
        {
            for (int j = P - 1; j >= 0 && above ((size_t) j, k); --j) pers[k]++;
            float* a = s.A (0);
            if (pers[k] < P) a[k] = 0; else { on[k] = 1; if (subtract) a[k] = (float) std::max (0.0, a[k] - noise[k]); }
        }
        for (size_t f = 1; f < s.frames; ++f)
        {
            float* a = s.A (f); const size_t nf = f + (size_t) P - 1;
            for (size_t k = 0; k < s.C(); ++k)
            {
                if (subtract) a[k] = (float) std::max (0.0, a[k] - noise[k]);
                if (! above (nf, k))
                {
                    if (on[k])
                    {
                        if (pers[k] > P) pers[k] = P;
                        if (--pers[k] <= 0) { on[k] = 0; a[k] = 0; }
                        else a[k] = (float) (a[k] * (double) pers[k] / P);
                    }
                    else { pers[k] = 0; a[k] = 0; }
                }
                else
                {
                    pers[k]++;
                    int q = pers[k] - P;
                    if (on[k]) {}
                    else if (q < 0) a[k] = 0;
                    else if (pers[k] < 2 * P - 1) a[k] = (float) (a[k] * (double) q / P);
                    else on[k] = 1;
                }
            }
        }
    });
}

// HARMONIC REMOVE: notches out a pitch (MIDI LOW..HIGH) and all its harmonics up to TOP, or keeps only them.
// A range of an octave or more clears everything from the low pitch to TOP. After CDP specnu remove (spec_remove)
Chans specnuRemove (const Chans& ch, Ctx& cx, const Params& p)
{
    const bool keepOnly = p.i ("mode") == 1; const double gain = 1 - p["atten"], top = p["top"];
    double m0 = p["mlo"], m1 = p["mhi"]; if (m0 > m1) std::swap (m0, m1);
    if (p.i ("auto") == 1)   // pitch from the tracker (median of the voiced frames), +-half a semitone
    {
        auto pt = trackPitch (monoOf (ch), cx.sr, 50, 1500, 0.15, &cx); DVec v;
        for (double f : pt.f0) if (f > 0) v.push_back (f);
        double m = 48;
        if (! v.empty()) { std::nth_element (v.begin(), v.begin() + (long) (v.size() / 2), v.end()); m = ftom (v[v.size() / 2]); }
        m0 = m - 0.5; m1 = m + 0.5;
    }
    const double lo = mtof (m0), hi = mtof (m1), tol = std::pow (2.0, p["tol"] / 1200);
    const bool all = lo * 2 <= hi;
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); const float* fr = s.F (f);
            for (size_t k = 0; k <= s.half; ++k)
            {
                double x = std::abs ((double) fr[k]); bool hit = false;
                if (x <= top && x >= lo / tol)
                {
                    if (all) hit = true;
                    else { double h = std::max (1.0, std::floor (x / (lo / tol))); for (double n = std::max (1.0, h - 1); n <= h + 1 && ! hit; ++n) hit = x >= n * lo / tol && x <= n * hi * tol; }
                }
                if (hit != keepOnly) a[k] = (float) (a[k] * gain);
            }
        }
    });
}

// SPECTRAL PIVOT: mirrors the channel amplitudes around PIVOT (lows become highs and vice versa), the partial
// frequencies staying in place. After CDP specnu slice 5 (specpivot, dev/standalone/specnu.c)
Chans specnuPivot (const Chans& ch, Ctx& cx, const Params& p)
{
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const long long pv = jr (p["pivot"] / s.chw), H = (long long) s.half;
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); long long a1 = pv - 1, a2 = pv + 1;
            for (;;)
            {
                if (a1 < 0) { for (; a2 <= H; ++a2) a[a2] = 0; break; }
                if (a2 > H) { for (; a1 >= 0; --a1) a[a1] = 0; break; }
                std::swap (a[a1], a[a2]); --a1; ++a2;
            }
        }
    });
}

// WINDOW SHUFFLE: spectral windows are re-ordered at random inside blocks of TIMESCALE, GROUP windows moving
// together. After CDP specnu rand (specrand, dev/standalone/specnu.c)
Chans specnuRand (const Chans& ch, Ctx& cx, const Params& p)
{
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        cx.seed (p["seed"]);
        const size_t W = s.frames, G = (size_t) std::max (1, p.i ("group"));
        size_t R = std::max ((size_t) 2, (size_t) jr (p["scale"] / s.ft)); R = std::max (R, G);
        std::vector<size_t> order; order.reserve (W);
        for (size_t start = 0; start < W && order.size() < W; start += R)
        {
            size_t sets = std::max ((size_t) 1, R / G); std::vector<size_t> perm (sets);
            for (size_t i = 0; i < sets; ++i) perm[i] = i;
            for (size_t i = sets; i > 1; --i) std::swap (perm[i - 1], perm[(size_t) (cx.rand() * (double) i) % i]);
            for (size_t i = 0; i < sets && order.size() < W; ++i)
                for (size_t m = 0, w = start + perm[i] * G; m < G && w < W && order.size() < W; ++m, ++w) order.push_back (w);
        }
        while (order.size() < W) order.push_back (order.size());
        Spec src = s;
        for (size_t f = 0; f < W; ++f) { std::copy (src.A (order[f]), src.A (order[f]) + s.C(), s.A (f)); std::copy (src.F (order[f]), src.F (order[f]) + s.C(), s.F (f)); }
    });
}

// SPECTRAL SQUEEZE: every partial's frequency is pulled towards CENTRE by SQUEEZE (or pushed away by values > 1).
// After CDP specnu squeeze (specsqz, dev/standalone/specnu.c)
Chans specnuSqueeze (const Chans& ch, Ctx& cx, const Params& p)
{
    const double c = p["centre"], q0 = p["squeeze"], q1 = p["squeeze2"];
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        std::vector<float> oa (s.C()), of (s.C());
        for (size_t f = 0; f < s.frames; ++f)
        {
            double u = s.frames > 1 ? (double) f / (double) (s.frames - 1) : 0, q = q0 * std::pow (q1 / q0, u);
            float* a = s.A (f); float* fr = s.F (f);
            std::fill (oa.begin(), oa.end(), 0.f);
            for (size_t k = 0; k <= s.half; ++k) of[k] = (float) ((double) k * s.chw);
            for (size_t k = 0; k <= s.half; ++k)
            {
                double nf = (std::abs ((double) fr[k]) - c) * q + c;
                if (nf <= 0 || nf >= s.nyq) continue;
                size_t j = (size_t) s.chan (nf);
                if (a[k] > oa[j]) { oa[j] = a[k]; of[j] = (float) nf; }
            }
            std::copy (oa.begin(), oa.end(), a); std::copy (of.begin(), of.end(), fr);
        }
    });
}

// ===================== Release 7/8 =====================
// HIGH BLUR: only the spectrum above ABOVE is time-blurred (averaged over BLUR), the bass stays sharp; LOCUT
// removes the deep bass. After CDP caltrain (caltrain, dev/standnew/caltrain.c)
Chans caltrain (const Chans& ch, Ctx& cx, const Params& p)
{
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const size_t B = std::max ((size_t) 1, (size_t) jr (p["blur"] / s.ft)), k0 = std::min ((size_t) s.chan (p["above"]), s.half);
        const size_t kc = p["locut"] > 0 ? (size_t) s.chan (p["locut"]) : 0;
        const Spec src = s; const size_t C = s.C();
        DVec sa (C, 0.0), sf (C, 0.0);   // running sums over the next B windows (wrapping at the end, as CDP's circular buffer)
        auto addF = [&] (size_t f, double sg) { const float* a = src.A (f % src.frames); const float* r = src.F (f % src.frames); for (size_t k = k0; k < C; ++k) { sa[k] += sg * a[k]; sf[k] += sg * r[k]; } };
        const size_t Bn = std::min (B, src.frames);
        for (size_t j = 0; j < Bn; ++j) addF (j, 1);
        for (size_t f = 0; f < s.frames; ++f)
        {
            const float* ia = src.A (f); float* a = s.A (f); float* fr = s.F (f);
            double mi = 0, mo = 0; for (size_t k = k0; k < C; ++k) { mi = std::max (mi, (double) ia[k]); mo = std::max (mo, sa[k]); }
            double sc = mo > 1e-12 ? mi / mo : 1;
            for (size_t k = k0; k < C; ++k) { a[k] = (float) std::max (0.0, sa[k] * sc); fr[k] = (float) (sf[k] / (double) Bn); }
            for (size_t k = 0; k <= std::min (kc, s.half) && kc > 0; ++k) a[k] = 0;
            addF (f, -1); addF (f + Bn, 1);
            if (! (f & 255)) { std::fill (sa.begin(), sa.end(), 0.0); std::fill (sf.begin(), sf.end(), 0.0); for (size_t j = 1; j <= Bn; ++j) addF (f + j, 1); }
        }
    });
}

// FRACTAL TRANSPOSE: a short transposition contour (SHAPE) is repeated inside each of its own steps, and inside
// theirs, DEPTH levels deep, the whole pattern lasting LENGTH and repeating. After CDP fractal spectrum (fractalise)
Chans fractalSpec (const Chans& ch, Ctx& cx, const Params& p)
{
    static const std::vector<std::vector<std::pair<double, double>>> SH {
        { { 0, 0 }, { 0.5, 5 } }, { { 0, 0 }, { 1.0 / 3, 4 }, { 2.0 / 3, 7 } }, { { 0, 0 }, { 0.25, 7 }, { 0.5, -5 }, { 0.75, 2 } },
        { { 0, 0 }, { 0.5, -3 } }, { { 0, 0 }, { 0.5, 2 } }, { { 0, 0 }, { 0.25, 12 }, { 0.5, 0 }, { 0.75, -12 } } };
    const auto& tpl = SH[(size_t) clamp (p.i ("shape"), 0, (int) SH.size() - 1)];
    const double dur = std::max (0.05, p["len"]), warp = p["warp"]; const bool shrink = p.i ("shrink") == 1, keepF = p.i ("formant") == 0;
    // build the pattern (times in 0..1 of the pattern, values in semitones)
    std::vector<std::pair<double, double>> pat (tpl);
    const double minDur = 0.012 / dur;   // no element shorter than ~ one analysis hop
    for (int d = 1; d < std::max (1, p.i ("depth")); ++d)
    {
        std::vector<std::pair<double, double>> nx;
        for (size_t i = 0; i < pat.size(); ++i)
        {
            double st = pat[i].first, en = i + 1 < pat.size() ? pat[i + 1].first : 1.0, dd = en - st;
            if (dd * tpl[1].first < minDur || nx.size() > 20000) { nx.push_back (pat[i]); continue; }
            for (auto& e : tpl) nx.push_back ({ st + e.first * dd, pat[i].second + e.second * (shrink ? dd : 1.0) });
        }
        pat.swap (nx);
    }
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        SpecEnv env; std::vector<float> ta, tf;
        for (size_t f = 0; f < s.frames; ++f)
        {
            double u = std::fmod ((double) f * s.ft / dur, 1.0);
            size_t i = (size_t) (std::upper_bound (pat.begin(), pat.end(), std::make_pair (u, 1e300)) - pat.begin()); i = i ? i - 1 : 0;
            transposeFrame (s, s.A (f), s.F (f), std::pow (2.0, clampd (pat[i].second * warp, -48, 48) / 12), keepF, env, ta, tf);
        }
    });
}

// CHANGE RINGING: the channel amplitudes between LOW and HIGH are permuted by STEP rounds of "Plain Bob" swaps
// (all adjacent pairs, then all but the outer ones...). EVOLVE rings through the changes over the sound.
// After CDP speculate (speculate, dev/standnew/speculation.c), one result instead of many files
Chans speculate (const Chans& ch, Ctx& cx, const Params& p)
{
    const bool evolve = p.i ("mode") == 1, removeOthers = p.i ("others") == 1;
    double lo = p["lo"], hi = p["hi"]; if (lo > hi) std::swap (lo, hi);
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const size_t c0 = (size_t) std::max (1, s.chan (lo)), c1 = std::max (c0 + 1, (size_t) s.chan (hi));
        const size_t total = (c1 - c0 + 1) * 2;
        std::vector<size_t> perm (s.C()); for (size_t k = 0; k < perm.size(); ++k) perm[k] = k;   // perm[k] = source channel shown at k
        size_t done = 0;
        auto step = [&] (size_t n) { size_t st = (n % 2 == 0) ? c0 : c0 + 1; for (size_t c = st; c + 1 <= c1; c += 2) std::swap (perm[c], perm[c + 1]); };
        const size_t fixedK = (size_t) std::max (0, p.i ("step"));
        std::vector<float> tmp (s.C());
        for (size_t f = 0; f < s.frames; ++f)
        {
            size_t want = evolve ? (size_t) ((double) f / (double) std::max ((size_t) 1, s.frames) * (double) total) : std::min (fixedK, total * 8);
            while (done < want) step (done++);
            float* a = s.A (f); std::copy (a, a + s.C(), tmp.begin());
            for (size_t k = 0; k <= s.half; ++k)
            {
                if (k >= c0 && k <= c1) a[k] = tmp[perm[k]];
                else if (removeOthers) a[k] = 0;
            }
        }
    });
}

// PARTIAL SUPPRESS: removes the COUNT most prominent partials between LOW and HIGH (with their side channels)
// between START and END. After CDP suppress partials (suppress, dev/standnew/suppress.c)
Chans suppressPartials (const Chans& ch, Ctx& cx, const Params& p)
{
    double lo = p["lo"], hi = p["hi"]; if (lo > hi) std::swap (lo, hi);
    const int cnt = std::max (1, p.i ("n")); double t0 = p["start"], t1 = p["end"]; if (t0 > t1) std::swap (t0, t1);
    const double dur = (double) ch[0].size() / cx.sr;
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        std::vector<float> st (s.C());
        for (size_t f = 1; f < s.frames; ++f)
        {
            double t = (double) f * s.ft; if (t < t0 * dur || t > t1 * dur) continue;
            float* a = s.A (f); const float* fr = s.F (f); bool any = false;
            for (size_t k = 0; k <= s.half; ++k) { bool in = fr[k] >= lo && fr[k] <= hi && a[k] > 0; st[k] = in ? a[k] : 0.f; any |= in; }
            if (! any) continue;
            for (int n = 0; n < cnt; ++n)
            {
                size_t mk = 0; float mv = 0; for (size_t k = 0; k <= s.half; ++k) if (st[k] > mv) { mv = st[k]; mk = k; }
                if (mv <= 0) break;
                const double fm = fr[mk];
                for (size_t k = 0; k <= s.half; ++k) if (std::abs (fr[k] - fm) < s.chw * 0.5) { a[k] = 0; st[k] = 0; }
                a[mk] = 0; st[mk] = 0;
            }
        }
    });
}

// SPECTRAL MATRIX: every FFT frame (as a complex vector) is multiplied by a random anti-Hermitian matrix that mixes
// all channels into all others; CYCLE adds the matrix in again each window (CDP -c). Or the real and imaginary
// parts are swapped, or the phases inverted. After CDP matrix (matrix_process, dev/standnew/unitary_matrix.c)
Chans matrixFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const bool cycle = p.i ("cycle") == 1;
    size_t chans = 4; while ((double) chans < p["chans"] - 0.5 && chans < 512) chans <<= 1;
    const size_t N = chans * 2, C = chans + 1, len = ch[0].size();
    size_t H = std::max ((size_t) 1, N / (2 * (size_t) clamp (p.i ("overlap"), 1, 4)));
    if (mode == 0) while (H < N / 2 && (double) len / (double) H * (double) (C * C) > 3e8) H <<= 1;   // keep the O(C^2) mixing affordable
    const DVec w = hann (N); double sw = 0; for (double x : w) sw += x * x;
    const double g = (double) H / sw;
    DVec mr (C * C, 0.0), mi (C * C, 0.0);
    if (mode == 0)
    {
        cx.seed (p["seed"]);
        for (size_t r = 0; r < C; ++r) for (size_t c = r + 1; c < C; ++c)
        {
            double re = 0; while (re <= 1e-6) re = cx.rand();
            double im = cx.rand();
            mr[r * C + c] = re; mi[r * C + c] = im; mr[c * C + r] = -re; mi[c * C + r] = im;
        }
    }
    const size_t frames = (len + N) / H + 1; Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        Buf out (len); DVec re (N), im (N), yr (C), yi (C);
        for (size_t f = 0; f < frames; ++f)
        {
            long long pos = (long long) (f * H) - (long long) (N / 2);
            analyze (ch[c], pos, w, re, im);
            if (mode == 1) for (size_t k = 0; k < C; ++k) std::swap (re[k], im[k]);
            else if (mode == 2) for (size_t k = 0; k < C; ++k) im[k] = -im[k];
            else
            {
                const double kf = cycle ? (double) (f + 1) : 1.0;
                std::fill (yr.begin(), yr.end(), 0.0); std::fill (yi.begin(), yi.end(), 0.0);
                for (size_t y = 0; y < C; ++y)
                {
                    const double xr = re[y], xi = im[y]; if (xr == 0 && xi == 0) continue;
                    const double* rr = &mr[y * C]; const double* ii = &mi[y * C];
                    for (size_t x = 0; x < C; ++x) { yr[x] += xr * rr[x] - xi * ii[x]; yi[x] += xr * ii[x] + xi * rr[x]; }
                }
                for (size_t k = 0; k < C; ++k) { re[k] = yr[k] * kf; im[k] = yi[k] * kf; }
            }
            mirror (re, im); synth (out, pos, w, re, im, g);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        res.push_back (std::move (out));
    }
    return res;
}

// GLISTEN: the channels are dealt at random into SETS groups; each group plays alone for SET TIME, then the next,
// each with its own random pitch shift; then the channels are dealt again. After CDP glisten (glisten, dev/new/glisten.c)
Chans glistenFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const int sets = std::max (2, p.i ("sets")); const double shiftR = p["shift"], durR = p["durrand"], divR = p["divrand"];
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        cx.seed (p["seed"]);
        const int setDur = std::max (1, (int) jr (p["setdur"] / 1000 / s.ft));
        const size_t P = s.half;   // channels 0..half-1 are dealt; the Nyquist channel always passes
        std::vector<size_t> perm (P), starts ((size_t) sets + 1);
        std::vector<float> oa (s.C()), of (s.C());
        int setNo = sets, cnt = 0, dur = setDur; double ratio = 1;
        for (size_t f = 0; f < s.frames; ++f)
        {
            if (setNo >= sets)   // new random partition
            {
                for (size_t k = 0; k < P; ++k) perm[k] = k;
                for (size_t k = P; k > 1; --k) std::swap (perm[k - 1], perm[(size_t) (cx.rand() * (double) k) % k]);
                DVec cut ((size_t) sets - 1);
                for (int i = 0; i < sets - 1; ++i) { double even = (double) (i + 1) / sets; cut[(size_t) i] = even * (1 - divR) + cx.rand() * divR; }
                std::sort (cut.begin(), cut.end());
                starts[0] = 0; for (int i = 0; i < sets - 1; ++i) starts[(size_t) i + 1] = (size_t) (cut[(size_t) i] * (double) P); starts[(size_t) sets] = P;
                setNo = 0; cnt = 0;
            }
            if (cnt == 0)
            {
                dur = durR > 0 ? std::max (1, setDur + (int) std::ceil (setDur * cx.rand() * durR) - setDur / 2) : setDur;
                ratio = std::pow (2.0, (cx.rand() * 2 - 1) * shiftR / 12);
            }
            float* a = s.A (f); float* fr = s.F (f);
            std::fill (oa.begin(), oa.end(), 0.f);
            for (size_t k = 0; k <= s.half; ++k) of[k] = (float) ((double) k * s.chw);
            oa[s.half] = a[s.half]; of[s.half] = fr[s.half];
            for (size_t n = starts[(size_t) setNo]; n < starts[(size_t) setNo + 1]; ++n)
            {
                size_t k = perm[n]; double nf = std::min (s.nyq, std::abs (fr[k] * ratio));
                size_t j = std::min ((size_t) s.chan (nf), P - 1);
                if (a[k] > oa[j]) { oa[j] = a[k]; of[j] = (float) nf; }
            }
            std::copy (oa.begin(), oa.end(), a); std::copy (of.begin(), of.end(), fr);
            if (++cnt >= dur) { cnt = 0; ++setNo; }
        }
    });
}

// SELF-SIMILAR: each window (from the loudest down) replaces the INDEX windows most similar to it, scaled to their
// loudness, so the sound keeps returning to its strongest spectra. After CDP selfsim (do_selfsim, dev/new/selfsim.c)
Chans selfsimFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const int K = std::max (1, p.i ("index"));
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const size_t D = std::max ((size_t) 1, (s.frames + 2499) / 2500), W = (s.frames + D - 1) / D, NB = 40;   // blocks of D windows
        DVec loud (W, 0.0); std::vector<float> feat (W * NB, 0.f);
        std::vector<size_t> band (s.C());
        for (size_t k = 0; k <= s.half; ++k) band[k] = std::min (NB - 1, (size_t) (NB * std::log2 (1 + (double) k) / std::log2 (1 + (double) s.half)));
        for (size_t b = 0; b < W; ++b)
            for (size_t f = b * D; f < std::min (s.frames, (b + 1) * D); ++f)
            { const float* a = s.A (f); for (size_t k = 0; k <= s.half; ++k) { loud[b] += a[k]; feat[b * NB + band[k]] += a[k]; } }
        double mx = 0; for (double l : loud) mx = std::max (mx, l);
        if (mx <= 0) return;
        for (size_t b = 0; b < W; ++b) { loud[b] /= mx; double inv = loud[b] > 1e-9 ? 1.0 / (loud[b] * mx) : 0; for (size_t j = 0; j < NB; ++j) feat[b * NB + j] = (float) (feat[b * NB + j] * inv); }
        std::vector<size_t> order (W), winorder (W); DVec scale (W, 1.0); std::vector<uint8_t> used (W, 0);
        for (size_t b = 0; b < W; ++b) { order[b] = b; winorder[b] = b; }
        std::stable_sort (order.begin(), order.end(), [&] (size_t x, size_t y) { return loud[x] > loud[y]; });
        DVec row (W); size_t set = 0;
        for (size_t n = 0; n < W && set < W; ++n)
        {
            const size_t z = order[n], jj = used[z] ? winorder[z] : z;
            for (size_t m = 0; m < W; ++m)
            {
                double d = 0; for (size_t j = 0; j < NB; ++j) d += std::abs (feat[z * NB + j] - feat[m * NB + j]);
                row[m] = m == z ? 1e300 : d;
            }
            for (int k = 0; k < K && set < W; ++k)
            {
                size_t best = 0; double bv = 1e300; for (size_t m = 0; m < W; ++m) if (row[m] < bv) { bv = row[m]; best = m; }
                if (bv >= 1e300) break;
                winorder[best] = jj; scale[best] = loud[jj] > 1e-9 ? loud[best] / loud[jj] : 0; used[best] = 1; row[best] = 1e300; ++set;
            }
            used[z] = 1; ++set;
            if (! (n & 63)) cx.prog (0.4);
        }
        const Spec src = s;
        for (size_t f = 0; f < s.frames; ++f)
        {
            size_t b = f / D, sb = winorder[b], sf = std::min (src.frames - 1, sb * D + f % D);
            const float* a = src.A (sf); const float* r = src.F (sf); float* oa = s.A (f); float* of = s.F (f);
            for (size_t k = 0; k <= s.half; ++k) { oa[k] = (float) (a[k] * scale[b]); of[k] = r[k]; }
        }
    });
}

const std::vector<std::string> TUNEMODES { "FREE", "TEMPERED", "CHORD", "CHORD FIELD" };
// SUPER ACCUMULATE: every channel sustains until louder data arrives, DECAYing per second and GLISSing in octaves
// per second; new resonances can start tuned to the tempered scale, a CHORD or that chord in all octaves.
// After CDP superaccu (superaccu, dev/new/superaccu.c)
Chans superaccuFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double decay = p["decay"], glis = p["glis"]; const bool reass = p.i ("reassign") == 1;
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        DVec tun;
        if (mode == 1) { for (int m = 0; m < 128; ++m) tun.push_back (mtof (m)); }
        else if (mode >= 2) tun = chordFreqs (p.i ("chord"), p["root"], mode == 3, s.nyq);
        const double dpf = std::pow (decay, s.ft), gpf = std::pow (2.0, glis * s.ft);
        const size_t C = s.C(), inFrames = s.frames;
        // tail: keep going after the input while the accumulator decays (up to 20 s)
        if (decay < 1)
        {
            size_t extra = (size_t) std::min (20.0 / s.ft, std::log (1e-4) / std::log (std::max (1e-9, dpf)) + 1);
            extra = std::min (extra, cx.maxLen / s.H + 2);
            s.resize (inFrames + extra);
        }
        std::vector<float> wa (C, 0.f), wf (C, 0.f), store (2 * C);
        double peakSum = 0;
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); float* fr = s.F (f);
            if (reass) { std::copy (a, a + C, store.begin()); std::copy (fr, fr + C, store.begin() + (long) C); }
            for (size_t k = 0; k < C; ++k)
            {
                if (dpf < 1) wa[k] = (float) (wa[k] * dpf);
                if (glis != 0) wf[k] = (float) std::min (s.nyq, wf[k] * gpf);
                if (a[k] > wa[k]) { wa[k] = a[k]; wf[k] = tun.empty() ? fr[k] : (float) nearestIn (tun, fr[k]); }
                else { a[k] = wa[k]; fr[k] = wf[k]; }
            }
            if (reass && glis != 0)
            {
                bool moved = false;
                for (size_t k = 0; k < C; ++k)
                {
                    double lo = ((double) k - 0.5) * s.chw, hi = lo + s.chw, x = fr[k];
                    if (x >= lo && x <= hi) continue;
                    size_t j = (size_t) s.chan (x);
                    if (j != k && a[j] < a[k]) { a[j] = a[k]; fr[j] = (float) x; a[k] = store[k]; fr[k] = store[C + k]; moved = true; }
                }
                if (moved)
                    for (size_t k = 1; k + 1 < C; ++k)
                        if (a[k] > 3 * a[k - 1] && a[k] > 3 * a[k + 1]) { fr[k - 1] = fr[k]; fr[k + 1] = fr[k]; }
            }
            double sum = totAmp (a, C); peakSum = std::max (peakSum, sum);
            if (f >= inFrames && sum < peakSum * 1e-4) { s.resize (f + 1); break; }
        }
    });
}

const std::vector<std::string> PROGS { "STATIC", "I-IV-V-I", "i-VI-III-VII", "I-vi-IV-V", "RISING", "FALLING" };
inline double progOffset (int prog, long long step)
{
    static const std::vector<std::vector<double>> P { { 0 }, { 0, 5, 7, 0 }, { 0, 8, 3, 10 }, { 0, 9, 5, 7 } };
    if (prog == 4) return (double) ((step % 6) * 2);
    if (prog == 5) return (double) (-(step % 6) * 2);
    const auto& v = P[(size_t) clamp (prog, 0, 3)]; return v[(size_t) (step % (long long) v.size())];
}
// TUNE VARYING: partial frequencies are pulled (FOCUS) onto the harmonics of a chord that steps through a
// progression with the tempo; CLARITY fades the rest, TRACE retunes only the loudest partials.
// After CDP tunevary (spectune, dev/new/tunevary.c)
Chans tunevaryFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const double focus = p["focus"], clar = 1 - p["clarity"], bcut = p["bcut"], stepSec = divSec (p.i ("div"), cx.bpm), glide = p["glide"];
    const int prog = p.i ("prog"), trace = p.i ("trace"); const auto sem = chordSemis (p.i ("chord"));
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        std::vector<float> tmpl (s.C());
        for (size_t f = 0; f < s.frames; ++f)
        {
            double t = (double) f * s.ft, u = t / stepSec; long long st = (long long) std::floor (u); double fr0 = u - (double) st;
            double off = progOffset (prog, st);
            if (glide > 0 && fr0 > 1 - glide) { double v = (fr0 - (1 - glide)) / glide; off += (progOffset (prog, st + 1) - off) * v; }
            std::fill (tmpl.begin(), tmpl.end(), -1.f);
            for (double sm : sem)
            {
                double f0 = mtof (p["root"] + off + sm);
                for (double h = f0; h < s.nyq; h += f0) { size_t k = (size_t) s.chan (h); if (tmpl[k] < 0) tmpl[k] = (float) h; }
            }
            float* a = s.A (f); float* fr = s.F (f);
            if (bcut > 0) for (size_t k = 0; k <= s.half; ++k) if (fr[k] < bcut) a[k] = 0;
            if (trace > 0)
            {
                std::vector<uint8_t> loud (s.C(), 0); for (size_t k : loudest (a, s.C(), (size_t) trace)) loud[k] = 1;
                for (size_t k = 0; k <= s.half; ++k)
                {
                    if (! loud[k]) { a[k] = (float) (a[k] * clar); continue; }
                    if (tmpl[k] > 0) { fr[k] = (float) (fr[k] + (tmpl[k] - fr[k]) * focus); continue; }
                    long long lo = (long long) k, hi = (long long) k;
                    while (lo >= 0 && tmpl[(size_t) lo] <= 0) --lo;
                    while (hi <= (long long) s.half && tmpl[(size_t) hi] <= 0) ++hi;
                    long long j = lo < 0 ? hi : hi > (long long) s.half ? lo : (tmpl[(size_t) hi] / std::max (1e-6f, fr[k]) < std::max (1e-6f, fr[k]) / tmpl[(size_t) lo] ? hi : lo);
                    if (j < 0 || j > (long long) s.half) continue;
                    fr[j] = (float) (fr[j] + (tmpl[(size_t) j] - fr[j]) * focus); a[j] = a[k]; a[k] = 0;
                }
            }
            else
                for (size_t k = 0; k <= s.half; ++k)
                {
                    if (tmpl[k] > 0) fr[k] = (float) (fr[k] + (tmpl[k] - fr[k]) * focus);
                    else a[k] = (float) (a[k] * clar);
                }
        }
    });
}

// ===================== specfnu: formant-based processing =====================
// pitch of every analysis window (Hz), from the YIN tracker on the mono mix; unvoiced stretches are bridged by
// the neighbouring pitches (CDP interpolates through them), a sound with no pitch at all gets 130.8 Hz
inline DVec framePitches (const Chans& ch, Ctx& cx, double win, size_t hopDiv = 4)
{
    const size_t N = winN (cx, win), H = std::max ((size_t) 1, N / hopDiv);
    auto pt = trackPitch (monoOf (ch), cx.sr, 50, 1500, 0.15, &cx);
    const size_t F = (size_t) std::ceil ((double) ch[0].size() / (double) H) + 1;
    DVec p (F, 0.0);
    for (size_t f = 0; f < F; ++f) p[f] = pt.at ((double) (f * H));
    long long last = -1;
    for (size_t f = 0; f < F; ++f)
        if (p[f] > 0)
        {
            if (last < 0) for (size_t g = 0; g < f; ++g) p[g] = p[f];
            else for (size_t g = (size_t) last + 1; g < f; ++g) { double u = (double) (g - (size_t) last) / (double) (f - (size_t) last); p[g] = std::exp (std::log (p[(size_t) last]) * (1 - u) + std::log (p[f]) * u); }
            last = (long long) f;
        }
    if (last < 0) std::fill (p.begin(), p.end(), 130.81);
    else for (size_t g = (size_t) last + 1; g < F; ++g) p[g] = p[(size_t) last];
    return p;
}
inline double pitchAt (const DVec& p, size_t f) { return p.empty() ? 130.81 : p[std::min (f, p.size() - 1)]; }

// formant structure of a window: troughs T0..T4 and peaks P1..P4 (as channels), CDP's "Trough PEAK Trough ..."
struct Formants { int tr[5] = { 0, 0, 0, 0, 0 }, pk[4] = { 0, 0, 0, 0 }; bool ok = false; };
struct FormantFinder
{
    Formants prev; DVec band, sm;
    // coarse spectral envelope (bands about one and a half harmonics wide), lightly smoothed, scanned upwards
    Formants find (const float* a, const Spec& s, double f0)
    {
        const double bw = clampd ((f0 > 0 ? f0 * 1.5 : 150.0), 90, 300);
        const size_t nb = std::max ((size_t) 12, (size_t) (std::min (s.nyq, 8000.0) / bw));
        band.assign (nb, 0.0); sm.assign (nb, 0.0);
        for (size_t k = 0; k <= s.half; ++k) { size_t b = (size_t) ((double) k * s.chw / bw); if (b < nb) band[b] = std::max (band[b], (double) a[k]); }
        for (size_t b = 0; b < nb; ++b) sm[b] = (band[b > 0 ? b - 1 : 0] + 2 * band[b] + band[std::min (b + 1, nb - 1)]) / 4;
        int pos[9]; int cnt = 0; bool rising = false;
        for (size_t b = 1; b < nb && cnt < 9; ++b)
        {
            if (b == 1) { if (sm[1] > sm[0]) { pos[cnt++] = 0; rising = true; } continue; }
            if (rising && sm[b] < sm[b - 1]) { pos[cnt++] = (int) b - 1; rising = false; }
            else if (! rising && sm[b] > sm[b - 1]) { pos[cnt++] = (int) b - 1; rising = true; }
        }
        Formants F;
        if (cnt == 9)
        {
            auto chOf = [&] (int b0, int b1, bool mx)   // loudest / quietest channel in band range
            {
                int c0 = (int) clamp ((long long) std::floor (b0 * bw / s.chw), 0LL, (long long) s.half), c1 = (int) clamp ((long long) std::ceil ((b1 + 1) * bw / s.chw), 0LL, (long long) s.half);
                int best = c0; for (int c = c0; c <= c1; ++c) if (mx ? a[c] > a[best] : a[c] < a[best]) best = c;
                return best;
            };
            for (int i = 0; i < 5; ++i) F.tr[i] = chOf (pos[2 * i], pos[2 * i], false);
            for (int i = 0; i < 4; ++i) F.pk[i] = chOf (pos[2 * i] + 1, pos[2 * i + 2] - 1 >= pos[2 * i] + 1 ? pos[2 * i + 2] - 1 : pos[2 * i] + 1, true);
            for (int i = 0; i < 4; ++i) { F.pk[i] = clamp (F.pk[i], F.tr[i], F.tr[i + 1]); }
            F.ok = true; prev = F; return F;
        }
        if (prev.ok) return prev;
        static const double DEF[9] = { 100, 500, 900, 1400, 2000, 2600, 3100, 3600, 4300 };   // neutral voice
        for (int i = 0; i < 5; ++i) F.tr[i] = s.chan (std::min (s.nyq * 0.9, DEF[2 * i]));
        for (int i = 1; i < 5; ++i) F.tr[i] = std::max (F.tr[i], std::min ((int) s.half, F.tr[i - 1] + 1));
        for (int i = 0; i < 4; ++i) F.pk[i] = clamp (s.chan (std::min (s.nyq * 0.9, DEF[2 * i + 1])), F.tr[i], F.tr[i + 1]);
        F.ok = true; return F;
    }
};
// keeps only harmonics of f0 (1) or removes them (2): CDP -x / -k
inline void harmonicFilter (float* a, const float* fr, const Spec& s, double f0, int how)
{
    if (! how || f0 <= 0) return;
    for (size_t k = 0; k <= s.half; ++k)
    {
        double x = fr[k], n = std::round (x / f0); bool harm = n >= 1 && std::abs (x - n * f0) < std::max (s.chw * 0.75, f0 * 0.06);
        if ((how == 1 && ! harm) || (how == 2 && harm)) a[k] = 0;
    }
}
const std::vector<std::string> HARMOPT { "ALL", "HARMONICS", "NO HARMONICS" };

// FORMANT NARROW (mode 1): the skirts of the four formant peaks are steepened (only 1/NARROW of each skirt survives,
// ramping from zero), OFF formants can be removed and the top above formant 4 cleared. After CDP specfnu 1 (formants_narrow)
// FORMANT SQUEEZE (mode 2), INVERT (3), ROTATE (4), NEGATIVE (5), SUPPRESS (6), MOVE BY / TO (8, 9) follow below.
enum FnuShape { FN_NARROW, FN_SQUEEZE, FN_INVERT, FN_ROTATE, FN_NEGATE, FN_SUPPRESS, FN_MOVE };
const std::vector<std::string> FSETS { "NONE", "1", "2", "3", "4", "1+2", "2+3", "3+4", "1+3", "2+4", "1+2+3", "2+3+4" };
inline int fsetMask (int i) { static const int M[] = { 0, 1, 2, 4, 8, 3, 6, 12, 5, 10, 7, 14 }; return M[clamp (i, 0, 11)]; }

Chans fnuShape (const Chans& ch, Ctx& cx, const Params& p, int kind)
{
    const DVec pitches = framePitches (ch, cx, p["win"]);
    const int harm = p.i ("harm"); const bool zeroTop = p.i ("top") == 1;
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        FormantFinder ff; std::vector<float> na (s.C()), nf (s.C());
        double phase = 0; const double dur = (double) s.frames * s.ft;
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); float* fr = s.F (f); const double f0 = pitchAt (pitches, f), pre = totAmp (a, s.C());
            Formants F = ff.find (a, s, f0);
            const double t = (double) f * s.ft, u = dur > 0 ? t / dur : 0;
            switch (kind)
            {
                case FN_NARROW: case FN_SUPPRESS:
                {
                    const int off = fsetMask (p.i ("off"));
                    for (int i = 0; i < 4; ++i) if (off & (1 << i)) for (int c = F.tr[i]; c < F.tr[i + 1]; ++c) a[c] = 0;
                    if (kind == FN_SUPPRESS) break;
                    const double keepFrac = 1.0 / std::max (1.0, p["narrow"] * std::pow (p["narrow2"] / p["narrow"], u));
                    for (int c = 0; c < F.tr[0]; ++c) a[c] = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        if (off & (1 << i)) continue;
                        // rising skirt trof -> peak
                        int c0 = F.tr[i], c1 = F.pk[i], d = c1 - c0, keep = (int) jr (d * keepFrac);
                        for (int c = c0; c < c1 - keep; ++c) a[c] = 0;
                        for (int c = std::max (c0, c1 - keep), k = 0; c < c1; ++c, ++k) a[c] = (float) (a[c] * (double) k / std::max (1, keep));
                        // falling skirt peak -> next trof
                        c0 = F.pk[i]; c1 = F.tr[i + 1]; d = c1 - c0; keep = (int) jr (d * keepFrac);
                        for (int c = c0 + keep + 1; c <= c1; ++c) a[c] = 0;
                        for (int c = c0 + 1, k = keep - 1; c <= std::min (c1, c0 + keep); ++c, --k) a[c] = (float) (a[c] * (double) std::max (0, k) / std::max (1, keep));
                    }
                    break;
                }
                case FN_SQUEEZE:
                {
                    const int at = clamp (p.i ("centre"), 1, 4) - 1; const bool trof = p.i ("at") == 1;
                    const int cc = trof ? F.tr[at + 1] : F.pk[at];
                    const double cf = fr[cc], q = 1.0 / std::max (1.0, p["squeeze"]);
                    std::fill (na.begin(), na.end(), 0.f); for (size_t k = 0; k <= s.half; ++k) nf[k] = (float) ((double) k * s.chw);
                    for (size_t k = 0; k <= s.half; ++k)
                    {
                        double x = cf + (fr[k] - cf) * q; if (x <= 0 || x >= s.nyq) continue;
                        size_t j = (size_t) s.chan (x); if (a[k] > na[j]) { na[j] = a[k]; nf[j] = (float) x; }
                    }
                    std::copy (na.begin(), na.end(), a); std::copy (nf.begin(), nf.end(), fr);
                    break;
                }
                case FN_INVERT: case FN_NEGATE:
                {
                    // two envelopes: the formant envelope and a much smoother contour; peaks and troughs swap around the contour
                    SpecEnv e1; e1.build (a, fr, s);
                    DVec env (s.C()), con (s.C());
                    for (size_t k = 0; k <= s.half; ++k) env[k] = e1.at (std::abs ((double) fr[k]));
                    envelope (env, std::max (2.0, 900.0 / s.chw), con);
                    if (kind == FN_INVERT)
                    {
                        const double vib = p["vib"]; double w = 1;
                        if (vib > 0) { w = std::sin (phase); phase = std::fmod (phase + TAU * vib * s.ft, TAU); }
                        for (size_t k = 0; k <= s.half; ++k)
                        {
                            double e = env[k], c = con[k], target = std::max (0.0, c - (e - c) * w);
                            a[k] = e > 1e-12 ? (float) (a[k] * std::min (50.0, target / e)) : 0.f;
                        }
                    }
                    else
                    {
                        double mn = 1e300, mx = 0; for (size_t k = 0; k <= s.half; ++k) { mn = std::min (mn, (double) a[k]); mx = std::max (mx, (double) a[k]); }
                        for (size_t k = 0; k <= s.half; ++k) a[k] = (float) (mx + mn - a[k]);
                        if (p.i ("flat") == 0)
                        {
                            SpecEnv e2; e2.build (a, fr, s); DVec en (s.C()), cn (s.C());
                            for (size_t k = 0; k <= s.half; ++k) en[k] = e2.at (std::abs ((double) fr[k]));
                            envelope (en, std::max (2.0, 900.0 / s.chw), cn);
                            for (size_t k = 0; k <= s.half; ++k) a[k] = cn[k] > 1e-12 ? (float) (a[k] * con[k] / cn[k]) : 0.f;
                        }
                    }
                    break;
                }
                case FN_ROTATE:
                {
                    const double lo = fr[F.tr[0]] > 0 ? fr[F.tr[0]] : F.tr[0] * s.chw, hi = std::max (lo + s.chw, (double) (fr[F.tr[4]] > 0 ? fr[F.tr[4]] : F.tr[4] * s.chw)), rg = hi - lo;
                    phase = std::fmod (phase + p["rspeed"] * s.ft, 1.0); if (phase < 0) phase += 1;
                    std::fill (na.begin(), na.end(), 0.f); for (size_t k = 0; k <= s.half; ++k) nf[k] = (float) ((double) k * s.chw);
                    for (size_t k = 0; k <= s.half; ++k)
                    {
                        double x = fr[k];
                        if (x >= lo && x <= hi) { x = lo + std::fmod (x - lo + phase * rg, rg); }
                        if (x <= 0 || x >= s.nyq) continue;
                        size_t j = (size_t) s.chan (x); if (a[k] > na[j]) { na[j] = a[k]; nf[j] = (float) x; }
                    }
                    std::copy (na.begin(), na.end(), a); std::copy (nf.begin(), nf.end(), fr);
                    break;
                }
                case FN_MOVE:
                {
                    const bool to = p.i ("mode") == 1;
                    std::copy (a, a + s.C(), na.begin()); std::copy (fr, fr + s.C(), nf.begin());
                    for (int c = F.tr[0]; c <= F.tr[4]; ++c) na[(size_t) c] = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        double v = p["f" + std::to_string (i + 1)], off = to ? v - fr[F.pk[i]] : v;
                        for (int c = F.tr[i]; c < F.tr[i + 1]; ++c)
                        {
                            double x = fr[c] + off; if (x <= 0 || x >= s.nyq) continue;
                            size_t j = (size_t) s.chan (x); if (a[c] > na[j]) { na[j] = a[c]; nf[j] = (float) x; }
                        }
                    }
                    std::copy (na.begin(), na.end(), a); std::copy (nf.begin(), nf.end(), fr);
                    break;
                }
                default: break;
            }
            if (zeroTop) for (size_t k = (size_t) F.tr[4]; k <= s.half; ++k) a[k] = 0;
            harmonicFilter (a, fr, s, f0, harm);
            if (totAmp (a, s.C()) > pre) normaliseTo (a, s.C(), pre);
        }
    });
}
Chans fnuNarrow (const Chans& ch, Ctx& cx, const Params& p) { return fnuShape (ch, cx, p, FN_NARROW); }
Chans fnuSqueeze (const Chans& ch, Ctx& cx, const Params& p) { return fnuShape (ch, cx, p, FN_SQUEEZE); }
Chans fnuInvert (const Chans& ch, Ctx& cx, const Params& p) { return fnuShape (ch, cx, p, FN_INVERT); }
Chans fnuRotate (const Chans& ch, Ctx& cx, const Params& p) { return fnuShape (ch, cx, p, FN_ROTATE); }
Chans fnuNegate (const Chans& ch, Ctx& cx, const Params& p) { return fnuShape (ch, cx, p, FN_NEGATE); }
Chans fnuSuppress (const Chans& ch, Ctx& cx, const Params& p) { return fnuShape (ch, cx, p, FN_SUPPRESS); }
Chans fnuMove (const Chans& ch, Ctx& cx, const Params& p) { return fnuShape (ch, cx, p, FN_MOVE); }

// ---- specfnu 10-19: partials are changed (arpeggiated, transposed, shifted, respaced, randomised, re-pitched),
// then the ORIGINAL spectral envelope is put back on them (CDP formants_recolor) ----
enum FnuRecolor { RC_ARPEG, RC_OCT, RC_TRANS, RC_SHIFT, RC_RESPACE, RC_RAND, RC_PINVERT, RC_PEXAGG, RC_PQUANT, RC_PRAND };
const std::vector<std::string> ARPDIR { "UP", "DOWN", "UP + DOWN" };
const std::vector<std::string> QSCALES { "CHROMATIC", "MAJOR", "MINOR", "PENTATONIC", "WHOLE TONE", "BLUES", "QUARTER TONE", "MAJ TRIAD", "MIN TRIAD" };
inline double quantiseMidi (double m, int scale, double root)
{
    static const std::vector<std::vector<double>> S { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 4, 7, 9 },
        { 0, 2, 4, 6, 8, 10 }, { 0, 3, 5, 6, 7, 10 }, { 0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5, 10, 10.5, 11, 11.5 }, { 0, 4, 7 }, { 0, 3, 7 } };
    const auto& sc = S[(size_t) clamp (scale, 0, (int) S.size() - 1)];
    double r = m - root, oct = std::floor (r / 12), within = r - oct * 12, best = 0, bd = 1e9;
    for (double v : sc) for (double o : { -12.0, 0.0, 12.0 }) { double d = std::abs (within - (v + o)); if (d < bd) { bd = d; best = v + o; } }
    return root + oct * 12 + best;
}

Chans fnuRecolor (const Chans& ch, Ctx& cx, const Params& p, int kind)
{
    const DVec pitches = framePitches (ch, cx, p["win"]);
    // the new pitch line for the pitch modes
    DVec target (pitches.size());
    double meanM = 0; for (double f : pitches) meanM += ftom (f); meanM /= std::max ((size_t) 1, pitches.size());
    {
        const size_t N = winN (cx, p["win"]); const double ft = (double) (N / 4) / cx.sr;
        cx.seed (p["seed"]);
        double sm = -1, rTarget = 0, rLast = 0, rPh = 1;
        for (size_t f = 0; f < pitches.size(); ++f)
        {
            double m = ftom (pitches[f]), nm = m;
            switch (kind)
            {
                case RC_PINVERT: { double ab = p["about"] > 0 ? p["about"] : meanM; nm = 2 * ab - m; break; }
                case RC_PEXAGG:
                {
                    double ab = p["about"] > 0 ? p["about"] : meanM; int side = p.i ("side");
                    if ((m >= ab && side != 2) || (m < ab && side != 1)) nm = ab + (m - ab) * p["rang"];
                    break;
                }
                case RC_PQUANT: nm = quantiseMidi (m, p.i ("scale"), p["root"]); break;
                case RC_PRAND:
                {
                    double base = p.i ("scale") > 0 ? quantiseMidi (m, p.i ("scale") - 1, p["root"]) : m;
                    rPh += p["rate"] * ft;
                    if (rPh >= 1) { rPh -= std::floor (rPh); rLast = rTarget; double up = p["range"] * p["slew"] / (1 + p["slew"]), dn = p["range"] / (1 + p["slew"]); rTarget = -dn + cx.rand() * (up + dn); }
                    nm = base + rLast + (rTarget - rLast) * std::min (1.0, rPh * 4);
                    break;
                }
                default: break;
            }
            nm = clampd (nm, 12, 120);
            if (kind == RC_PQUANT || kind == RC_PRAND)
            {   // glide between quantised pitches (CDP smooths transitions unless -n)
                double g = p["glide"] > 0 ? std::exp (-ft / (p["glide"] / 1000)) : 0;
                sm = sm < 0 ? nm : sm * g + nm * (1 - g); nm = sm;
            }
            target[f] = mtof (nm);
        }
    }
    const double arate = p["arprate"]; const int adir = p.i ("arpdir");
    return specFx (ch, cx, p["win"], [&] (Spec& s, int c)
    {
        cx.seed (p["seed"] + 17);
        (void) c;
        SpecEnv eo, en; std::vector<float> na (s.C()), nf (s.C());
        double phase = 0; bool up = true;
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); float* fr = s.F (f); const double f0 = pitchAt (pitches, f), pre = totAmp (a, s.C());
            eo.build (a, fr, s, f0 * 1.2);
            if (arate != 0 || kind == RC_ARPEG)   // a 4-partial-wide raised-cosine window sweeps over harmonics 1..8
            {
                double nx = phase + std::abs (arate) * s.ft; bool nu = up; if (nx > 1) { nu = ! up; nx -= 1; }
                double ctr = (adir == 1 || (adir == 2 && ! up) || arate < 0) ? (1 - phase) * 7 : phase * 7;
                double foot = ctr - 2, top = ctr + 2, bw = 4 * f0;
                for (size_t k = 0; k <= s.half; ++k)
                {
                    double x = fr[k], w = 0;
                    auto shp = [&] (double b0, double b1) { if (x > b0 && x < b1) { double o = (x - b0) / bw; w = std::max (w, std::min (1.0, std::max (0.0, 1 - std::cos (o * TAU)) / 2)); } };
                    if (top > 7) { shp (f0, (top - 7 + 1) * f0); shp ((foot + 1) * f0, 8 * f0); }
                    else if (foot < 0) { shp (f0, (top + 1) * f0); shp ((foot + 7 + 1) * f0, 8 * f0); }
                    else shp ((foot + 1) * f0, (top + 1) * f0);
                    a[k] = (float) (a[k] * w);
                }
                phase = nx; up = nu;
            }
            if (kind != RC_ARPEG)
            {
                double ratio = 1, shift = 0;
                switch (kind)
                {
                    case RC_OCT: ratio = std::pow (2.0, p.i ("oct")); break;
                    case RC_TRANS: ratio = std::pow (2.0, p["trans"] / 12); break;
                    case RC_SHIFT: shift = p["shift"]; break;
                    case RC_PINVERT: case RC_PEXAGG: case RC_PQUANT: case RC_PRAND: ratio = pitchAt (target, f) / std::max (1.0, f0); break;
                    default: break;
                }
                std::fill (na.begin(), na.end(), 0.f);
                for (size_t k = 0; k <= s.half; ++k) nf[k] = (float) ((double) k * s.chw);
                for (size_t k = 0; k <= s.half; ++k)
                {
                    double x = fr[k];
                    if (kind == RC_SHIFT) x += shift;
                    else if (kind == RC_RESPACE) { if (x >= f0) x = f0 + (x - f0) * (p["respace"] / f0); }
                    else if (kind == RC_RAND) x += f0 * (cx.rand() * 2 - 1) / 2 * p["rand"];
                    else x *= ratio;
                    if (x <= 0 || x >= s.nyq) continue;
                    size_t j = (size_t) s.chan (x); if (a[k] > na[j]) { na[j] = a[k]; nf[j] = (float) x; }
                }
                // re-impose the original envelope (vocoding ratio)
                en.build (na.data(), nf.data(), s, f0 * 1.2);
                for (size_t k = 0; k <= s.half; ++k)
                {
                    double e1 = en.at (nf[k]), e0 = eo.at (nf[k]);
                    na[k] = e1 <= 0 ? 0.f : (float) (na[k] * std::min (100.0, e0 / e1));
                }
                std::copy (na.begin(), na.end(), a); std::copy (nf.begin(), nf.end(), fr);
            }
            if (p["locut"] > 0) for (size_t k = 0; k <= s.half; ++k) if (fr[k] < p["locut"]) a[k] = 0;
            if (p["hicut"] < 20000) for (size_t k = 0; k <= s.half; ++k) if (fr[k] > p["hicut"]) a[k] = 0;
            if (totAmp (a, s.C()) > pre) normaliseTo (a, s.C(), pre);
        }
    });
}
Chans fnuArpeg (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_ARPEG); }
Chans fnuOct (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_OCT); }
Chans fnuTrans (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_TRANS); }
Chans fnuShift (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_SHIFT); }
Chans fnuRespace (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_RESPACE); }
Chans fnuRand (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_RAND); }
Chans fnuPInvert (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_PINVERT); }
Chans fnuPExagg (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_PEXAGG); }
Chans fnuPQuant (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_PQUANT); }
Chans fnuPRand (const Chans& ch, Ctx& cx, const Params& p) { return fnuRecolor (ch, cx, p, RC_PRAND); }

// SINE SPEECH: each of the four formants collapses into one sine at its peak frequency carrying the formant's
// energy (AMP 1-4), optionally pulled onto a SCALE (DEPTH); SINING mixes original and sines.
// After CDP specfnu 23 (sine speech, dev/science/specfnu.c)
Chans fnuSinus (const Chans& ch, Ctx& cx, const Params& p)
{
    const DVec pitches = framePitches (ch, cx, p["win"]);
    const double sing = p["sining"]; const int scale = p.i ("scale");
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        FormantFinder ff; std::vector<float> na (s.C()), nf (s.C());
        double lastF[4] = { 0, 0, 0, 0 };
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f); float* fr = s.F (f);
            Formants F = ff.find (a, s, pitchAt (pitches, f));
            for (size_t k = 0; k <= s.half; ++k) { na[k] = (float) (a[k] * (1 - sing)); nf[k] = fr[k]; }
            for (int i = 0; i < 4; ++i)
            {
                double e = 0; for (int c = F.tr[i]; c < F.tr[i + 1]; ++c) e += a[c];
                double x = fr[F.pk[i]]; if (! (x > 0)) x = F.pk[i] * s.chw;
                if (scale > 0) { double q = mtof (quantiseMidi (ftom (x), scale - 1, p["root"])); x = x + (q - x) * p["qdep"]; }
                if (p.i ("smooth") == 1 && lastF[i] > 0) x = lastF[i] * 0.7 + x * 0.3;
                lastF[i] = x;
                if (x <= 0 || x >= s.nyq) continue;
                size_t j = (size_t) s.chan (x); double amp = e * 0.5 * sing * p["amp" + std::to_string (i + 1)];
                na[j] = (float) (na[j] * (1 - sing) + amp); nf[j] = (float) x;
                if (j > 0) { na[j - 1] = (float) (na[j - 1] + amp * 0.5); nf[j - 1] = (float) x; }
                if (j < s.half) { na[j + 1] = (float) (na[j + 1] + amp * 0.5); nf[j + 1] = (float) x; }
            }
            std::copy (na.begin(), na.end(), a); std::copy (nf.begin(), nf.end(), fr);
        }
    });
}

// ===================== specfold / spectstr / spectune =====================
// SPECTRAL FOLD: the channels from START, COUNT wide, are FOLDed (first half to every other channel, second half
// into the gaps going down, FOLDS times), INVERTed or RANDOMly permuted; AMP ONLY leaves the frequencies in place.
// After CDP specfold 1-3 (do_specfold, dev/science/specfold.c)
Chans specfoldFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const bool ampOnly = p.i ("what") == 1;
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const size_t st = std::min ((size_t) s.chan (p["start"]), s.half - 1);
        size_t len = std::min ((size_t) std::max (4, p.i ("len")), s.half + 1 - st); if (mode == 0) len &= ~(size_t) 1;
        if (len < 2) return;
        std::vector<size_t> perm (len), tmp (len);
        for (size_t n = 0; n < len; ++n) perm[n] = n;
        if (mode == 0)
        {
            const size_t hf = len / 2; const int folds = clamp (p.i ("folds"), 1, (int) len - 1);
            for (int i = 0; i < folds; ++i)
            {
                size_t n = 0;
                for (size_t m = 1; n < hf; m += 2) tmp[std::min (m, len - 1)] = perm[n++];
                for (long long m = (long long) len - 2; n < len; m -= 2) tmp[(size_t) std::max (0LL, m)] = perm[n++];
                perm.swap (tmp);
            }
        }
        else if (mode == 1) for (size_t n = 0; n < len; ++n) perm[n] = len - 1 - n;
        else { cx.seed (p["seed"]); for (size_t n = len; n > 1; --n) std::swap (perm[n - 1], perm[(size_t) (cx.rand() * (double) n) % n]); }
        std::vector<float> na (len), nf (len);
        for (size_t f = 0; f < s.frames; ++f)
        {
            float* a = s.A (f) + st; float* fr = s.F (f) + st;
            std::copy (a, a + len, na.begin()); std::copy (fr, fr + len, nf.begin());
            for (size_t c = 0; c < len; ++c)
            {
                size_t j = perm[c]; na[j] = a[c];
                if (! ampOnly) { double off = fr[c] - (double) (st + c) * s.chw, x = (double) (st + j) * s.chw + off; nf[j] = (float) clampd (x, 0, s.nyq); }
            }
            std::copy (na.begin(), na.end(), a); if (! ampOnly) std::copy (nf.begin(), nf.end(), fr);
        }
    });
}

// DECOHERED STRETCH: phase-vocoder time stretch (STRETCH moving to END STRETCH) where the quietest DECOHERE share of
// the channels get randomised frequencies (RANDOM), breaking up the metallic phasiness of long stretches.
// After CDP spectstr stretch time (spectstretch / decohere, dev/science/spectstr.c)
Chans spectstrFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const double s0 = p["stretch"], s1 = p["stretch2"], dr = p["dratio"], drand = p["drand"];
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        cx.seed (p["seed"]);
        const Spec src = s; const size_t W = src.frames, C = s.C();
        if (W < 2) return;
        // output window positions in the input (stretch varies exponentially over the input)
        DVec pos; double here = 0; const size_t cap = cx.maxLen / s.H + 2;
        while (here < (double) (W - 1) && pos.size() < cap)
        {
            pos.push_back (here);
            double u = here / (double) (W - 1), st = s0 * std::pow (s1 / s0, u);
            here += 1.0 / std::max (0.01, st);
        }
        if (pos.empty()) pos.push_back (0);
        s.resize (pos.size());
        const size_t nd = (size_t) jr (dr * (double) C);
        std::vector<size_t> idx (C);
        for (size_t f = 0; f < pos.size(); ++f)
        {
            size_t w0 = std::min ((size_t) pos[f], W - 1), w1 = std::min (w0 + 1, W - 1); double fr0 = pos[f] - (double) w0;
            float* a = s.A (f); float* fr = s.F (f);
            for (size_t k = 0; k < C; ++k)
            {
                a[k] = (float) (src.A (w0)[k] + (src.A (w1)[k] - src.A (w0)[k]) * fr0);
                fr[k] = (float) (src.F (w0)[k] + (src.F (w1)[k] - src.F (w0)[k]) * fr0);
            }
            if (nd > 0)
            {
                for (size_t k = 0; k < C; ++k) idx[k] = k;
                std::nth_element (idx.begin(), idx.begin() + (long) std::min (nd, C - 1), idx.end(), [&] (size_t x, size_t y) { return a[x] < a[y]; });
                for (size_t q = 0; q < std::min (nd, C); ++q)
                {
                    size_t k = idx[q]; double cf = (double) k * s.chw, r = (cx.rand() - 0.5) * drand * s.chw, x = cf + r;
                    if (x < 1 || x >= s.nyq) x = cf - r;
                    fr[k] = (float) x;
                }
            }
            if (! (f & 255)) cx.prog (0.4);
        }
    });
}

// AUTO TUNE (STATIC): finds the most prominent pitch of the whole sound and transposes everything (keeping the
// formants unless told not to) onto the nearest TEMPERED pitch, the nearest CHORD note, or a chord note in any
// octave. After CDP spectune tune 1-3 (do_spectune, dev/science/spectune.c)
Chans spectuneFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const bool keepF = p.i ("formant") == 0;
    auto pt = trackPitch (monoOf (ch), cx.sr, mtof (std::min (p["lop"], p["hip"] - 1)), mtof (std::max (p["hip"], p["lop"] + 1)), 0.15, &cx);
    // loudness-weighted histogram of quarter-tones: the most prominent pitch
    Buf m = monoOf (ch); std::map<int, double> hist;
    for (size_t i = 0; i < pt.f0.size(); ++i)
    {
        if (pt.f0[i] <= 0) continue;
        double e = 0; size_t a = i * pt.hop, b = std::min (m.size(), a + pt.hop); for (size_t j = a; j < b; ++j) e += (double) m[j] * m[j];
        hist[(int) jr (ftom (pt.f0[i]) * 4)] += p.i ("weight") == 0 ? std::sqrt (e) + 1e-9 : 1.0;
    }
    double found = 0, bestW = 0;
    for (auto& [q, w] : hist) { double ww = w + 0.5 * (hist.count (q - 1) ? hist[q - 1] : 0) + 0.5 * (hist.count (q + 1) ? hist[q + 1] : 0); if (ww > bestW) { bestW = ww; found = q / 4.0; } }
    double ratio = 1;
    if (found > 0)
    {
        double target = mode == 0 ? std::round (found) : ftom (nearestIn (chordFreqs (p.i ("chord"), p["root"], mode == 2, 1e9), mtof (found)));
        ratio = std::pow (2.0, clampd (target - found, -36, 36) / 12);
    }
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        SpecEnv env; std::vector<float> ta, tf;
        for (size_t f = 0; f < s.frames; ++f) transposeFrame (s, s.A (f), s.F (f), ratio, keepF, env, ta, tf);
    });
}

// PEDAL TUNE: in every window the PEAKS loudest spectral peaks (and the channels sharing their frequency) are
// retuned to the nearest note of the pedal CHORD on ROOT in any octave. After CDP spectune tune 5 (do_specpedal)
Chans specpedalFx (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t pkcnt = (size_t) std::max (1, p.i ("peaks"));
    return specFx (ch, cx, p["win"], [&] (Spec& s, int)
    {
        const DVec set = chordFreqs (p.i ("chord"), p["root"], true, s.nyq);
        std::vector<uint8_t> peak (s.C()); std::vector<float> pk;
        for (size_t f = 1; f < s.frames; ++f)
        {
            float* a = s.A (f); float* fr = s.F (f);
            if (totAmp (a, s.C()) <= 0) continue;
            std::fill (peak.begin(), peak.end(), 0);
            pk.assign (s.C(), 0.f);
            for (size_t k = 0; k <= s.half; ++k)
            {
                double l = k ? a[k - 1] : -1, r = k < s.half ? a[k + 1] : -1;
                if (a[k] > l && a[k] >= r) pk[k] = a[k];
            }
            for (size_t k : loudest (pk.data(), s.C(), pkcnt)) if (pk[k] > 0) peak[k] = 1;
            // channels carrying the same partial as a peak are peaks too
            for (size_t k = 0; k <= s.half; ++k)
                if (peak[k] == 1)
                    for (size_t j = k > 3 ? k - 3 : 0; j <= std::min (s.half, k + 3); ++j)
                        if (! peak[j] && std::abs (ftom (std::max (1.f, fr[j])) - ftom (std::max (1.f, fr[k]))) < 0.25) peak[j] = 2;
            for (size_t k = 0; k <= s.half; ++k)
            {
                if (! peak[k]) continue;
                double nf = nearestIn (set, std::max (1.0, (double) fr[k])), lo = ((double) k - 0.5) * s.chw, hi = ((double) k + 0.5) * s.chw;
                if (nf >= lo && nf <= hi) { fr[k] = (float) nf; continue; }
                size_t j = (size_t) s.chan (nf);
                if (! peak[j]) { std::swap (a[j], a[k]); fr[j] = (float) nf; peak[j] = 3; peak[k] = 0; }
                else fr[k] = (float) nf;
            }
        }
    });
}
} // namespace

void addCdpSpectralB (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CAT = "CDP \xc2\xb7 SPECTRAL II";
    auto W = [] { return Win (0.01, 0.2, 0.046); };

    add (v, CAT, "cdp_hilite_filter", "SPECTRAL FILTER", "HFLT", "Steep FFT filter on every partial's true frequency: HIGH, LOW, BAND pass or NOTCH between FREQ 1 and FREQ 2, with skirts SKIRT Hz wide. NORMALISED restores each window's level (a thin band gets as loud as the whole).",
         { Opt ("mode", "TYPE", { "HIGH PASS", "LOW PASS", "BAND PASS", "NOTCH" }, 2), P ("frq1", "FREQ 1", 20, 18000, 400, PExp, {}, Fmt::hz), P ("frq2", "FREQ 2", 20, 18000, 1800, PExp, {}, Fmt::hz),
           P ("q", "SKIRT", 1, 2000, 100, PExp, {}, Fmt::hz), Opt ("norm", "LEVEL", { "PLAIN", "NORMALISED" }), W() }, hiliteFilter, {}, false, "hilite filter");
    add (v, CAT, "cdp_hilite_vowels", "SPECTRAL VOWELS", "HVOW", "Pushes the spectrum onto three-formant vowel shapes: VOWEL A, a GLIDE to VOWEL B, or ALTERNATE at RATE. WIDTH and STEEP shape the formant peaks, RANGE their height, THRESH how much of the spectrum is forced.",
         { Opt ("va", "VOWEL A", VOWELS, 0), Opt ("vb", "VOWEL B", VOWELS, 10), Opt ("motion", "MOTION", { "STATIC A", "GLIDE A\xe2\x86\x92" "B", "ALTERNATE" }, 1), P ("rate", "RATE", 0.05, 20, 1, PExp, "Hz"),
           P ("hwidth", "WIDTH", 0.01, 2, 0.25, PExp, "x"), P ("steep", "STEEP", 0.1, 10, 3, PExp), Pct ("range", "RANGE", 0.95), Pct ("thresh", "THRESH", 0.5), W() }, hiliteVowels, {}, false, "hilite vowels");
    add (v, CAT, "cdp_hilite_greq", "SPECTRAL GRAPHIC EQ", "HGEQ", "Graphic EQ in the spectrum: bands BANDWIDTH octaves wide on every note of CHORD over ROOT between LOW and HIGH, as band-pass filters (resonant chord) or NOTCH filters (the chord carved out).",
         { Opt ("mode", "MODE", { "BANDPASS", "NOTCH" }), Opt ("chord", "CHORD", CHORDS, 0), P ("root", "ROOT", 24, 72, 48, PInt, {}, Fmt::note), P ("bw", "BANDWIDTH", 0.02, 1, 0.1, PExp, "oct"),
           P ("lo", "LOW", 20, 2000, 60, PExp, {}, Fmt::hz), P ("hi", "HIGH", 500, 20000, 8000, PExp, {}, Fmt::hz), W() }, hiliteGreq, {}, false, "hilite greq 1-2");
    add (v, CAT, "cdp_hilite_band", "SPECTRAL BANDS", "HBND", "Splits the spectrum at SPLIT 1 and SPLIT 2 into three bands with their own GAIN. The partials of the middle band are TRANSPOSEd or SHIFTed in Hz, replacing the band or ADDed to the original.",
         { Opt ("mode", "MODE", { "TRANSPOSE", "TRANSP ADD", "SHIFT", "SHIFT ADD" }, 1), P ("split1", "SPLIT 1", 50, 5000, 300, PExp, {}, Fmt::hz), P ("split2", "SPLIT 2", 200, 16000, 3000, PExp, {}, Fmt::hz),
           P ("glo", "LOW GAIN", -48, 12, 0, 0, {}, Fmt::db), P ("gmid", "MID GAIN", -48, 12, 0, 0, {}, Fmt::db), P ("ghi", "HIGH GAIN", -48, 12, -6, 0, {}, Fmt::db),
           P ("trans", "TRANSPOSE", -24, 24, 7, 0, "st"), P ("shift", "SHIFT", -2000, 2000, 150, 0, "Hz"), W() }, hiliteBand, {}, false, "hilite band");
    add (v, CAT, "cdp_hilite_arpeg", "SPECTRAL ARPEGGIO", "HARP", "A band WIDTH Hz wide sweeps between LOW and HIGH at RATE (WAVE shape); partials it touches ring out, boosted by AMP, over SUSTAIN windows (CURVE bends the decay). MODE sets what plays outside the band.",
         { Opt ("mode", "MODE", { "ON", "BOOST", "BELOW>BOOST", "ABOVE>BOOST", "BELOW", "ABOVE", "ONCE BELOW", "ONCE ABOVE" }), Opt ("wave", "WAVE", { "DOWNRAMP", "SINE", "SAW", "UPRAMP" }, 1),
           P ("rate", "RATE", 0.05, 30, 2, PExp, "Hz"), Pct ("phase", "PHASE", 0), P ("lo", "LOW", 0, 5000, 100, 0, {}, Fmt::hz), P ("hi", "HIGH", 200, 20000, 6000, PExp, {}, Fmt::hz),
           P ("bw", "WIDTH", 10, 2000, 150, PExp, {}, Fmt::hz), P ("amp", "AMP", 1, 30, 10, PExp, "x"), P ("sust", "SUSTAIN", 1, 64, 6, PInt), P ("nonlin", "CURVE", 0.1, 10, 1, PExp),
           Opt ("hold", "PITCH", { "HOLD START", "TRACK" }), Opt ("retrig", "RETRIGGER", { "EARLY", "AFTER DECAY" }) }, hiliteArpeg, {}, false, "hilite arpeg 1-8");
    add (v, CAT, "cdp_hilite_pluck", "SPECTRAL PLUCK", "HPLK", "Every partial that newly appears (rises above THRESH below the window peak) gets a GAIN boost, so changing spectra sparkle with pluck-like attacks. Great after an arpeggio or trace.",
         { P ("gain", "GAIN", 1, 30, 6, PExp, "x"), P ("thresh", "THRESH", -80, -6, -30, 0, {}, Fmt::db), W() }, hilitePluck, {}, false, "hilite pluck");
    add (v, CAT, "cdp_hilite_trace", "SPECTRAL TRACE", "HTRC", "Keeps only the N loudest partials of each window: from the whole spectrum, ABOVE LOW, BELOW HIGH or BETWEEN them. RETAIN tops up with loud partials from outside the band when the band has too few.",
         { Opt ("mode", "MODE", { "ALL", "ABOVE LOW", "BELOW HIGH", "BETWEEN" }), P ("n", "PARTIALS", 1, 256, 12, PInt | PExp), P ("lo", "LOW", 20, 5000, 200, PExp, {}, Fmt::hz),
           P ("hi", "HIGH", 100, 20000, 4000, PExp, {}, Fmt::hz), Opt ("retain", "RETAIN", { "NO", "YES" }), W() }, hiliteTrace, {}, false, "hilite trace 1-4");
    add (v, CAT, "cdp_hilite_bltr", "BLUR + TRACE", "HBLT", "Time-averages the spectrum over BLUR, then keeps only the N loudest partials of each window: a smooth, ghostly line drawing of the sound.",
         { P ("blur", "BLUR", 10, 2000, 150, PExp, "ms"), P ("n", "PARTIALS", 1, 256, 16, PInt | PExp), W() }, hiliteBltr, {}, false, "hilite bltr");

    add (v, CAT, "cdp_specnu_clean", "SPECTRAL DENOISE", "SCLN", "Takes a noise print (QUIETEST stretch, START or END, NOISE LEN long) and removes every channel that does not rise above NOISE GAIN times it for PERSIST. SUBTRACT also takes the noise level off what remains.",
         { Opt ("mode", "MODE", { "CLEAN", "SUBTRACT" }), Opt ("nsrc", "NOISE AT", { "QUIETEST", "START", "END" }), P ("nlen", "NOISE LEN", 0.02, 5, 0.1, PExp, {}, Fmt::time),
           P ("persist", "PERSIST", 0, 1000, 50, 0, "ms"), P ("ngain", "NOISE GAIN", 1, 40, 2, PExp, "x"), W() }, specClean, {}, false, "speclean clean / specnu clean / specnu subtract");
    add (v, CAT, "cdp_specnu_remove", "HARMONIC REMOVE", "SRMV", "Notches out a pitch (TRACKED from the sound, or MANUAL between LOW and HIGH) and all its harmonics up to TOP, or KEEPs only them. ATTEN sets how deep; TOLERANCE widens each harmonic band. A range of an octave or more clears everything up to TOP.",
         { Opt ("mode", "MODE", { "REMOVE", "KEEP ONLY" }), Opt ("auto", "PITCH", { "MANUAL", "TRACKED" }, 1), P ("mlo", "LOW", 24, 96, 49.5, 0, {}, Fmt::note), P ("mhi", "HIGH", 24, 96, 50.5, 0, {}, Fmt::note),
           P ("top", "TOP", 200, 20000, 8000, PExp, {}, Fmt::hz), Pct ("atten", "ATTEN", 1), P ("tol", "TOLERANCE", 0, 100, 20, 0, "ct"), W() }, specnuRemove, {}, false, "specnu remove 1-2");
    add (v, CAT, "cdp_specnu_pivot", "SPECTRAL PIVOT", "SPIV", "Mirrors the spectrum's amplitudes around PIVOT: what was below rises above it and vice versa, while the partial frequencies stay put. Strange, inside-out timbres.",
         { P ("pivot", "PIVOT", 50, 10000, 1200, PExp, {}, Fmt::hz), W() }, specnuPivot, {}, false, "specnu slice 5");
    add (v, CAT, "cdp_specnu_rand", "WINDOW SHUFFLE", "SRND", "Shuffles the order of the spectral windows inside blocks of TIMESCALE, GROUP windows staying together: from shimmering smears (short) to scrambled phrases (long).",
         { P ("scale", "TIMESCALE", 0.02, 10, 0.5, PExp, {}, Fmt::time), P ("group", "GROUP", 1, 64, 2, PInt | PExp), Seed(), W() }, specnuRand, {}, false, "specnu rand");
    add (v, CAT, "cdp_specnu_squeeze", "SPECTRAL SQUEEZE", "SSQZ", "Pulls every partial towards CENTRE by SQUEEZE (at the start) moving to END SQUEEZE: values below 1 crush the spectrum into a narrow band, above 1 spread it out.",
         { P ("centre", "CENTRE", 50, 8000, 600, PExp, {}, Fmt::hz), P ("squeeze", "SQUEEZE", 0.05, 2, 0.3, PExp, "x"), P ("squeeze2", "END SQUEEZE", 0.05, 2, 0.3, PExp, "x"), W() }, specnuSqueeze, {}, false, "specnu squeeze");
    add (v, CAT, "cdp_caltrain", "HIGH BLUR", "CALT", "Time-blurs only the spectrum above ABOVE over BLUR, so the bass and attacks stay sharp while the top turns into a wash. LOCUT removes the deep bass (0 = off).",
         { P ("blur", "BLUR", 0.02, 4, 0.5, PExp, {}, Fmt::time), P ("above", "ABOVE", 100, 10000, 1500, PExp, {}, Fmt::hz), P ("locut", "LOCUT", 0, 500, 0, 0, {}, Fmt::hz), W() }, caltrain, {}, false, "caltrain caltrain");
    add (v, CAT, "cdp_fractal_spectrum", "FRACTAL TRANSPOSE", "FRCT", "A transposition SHAPE is repeated inside each of its own steps, DEPTH levels deep, over LENGTH (then repeats). WARP scales the intervals, SHRINK makes smaller levels smaller, FORMANTS keeps the timbre in place.",
         { Opt ("shape", "SHAPE", { "UP 4TH", "MAJOR ARP", "ZIGZAG", "DOWN 3RD", "TRILL", "OCTAVES" }, 1), P ("len", "LENGTH", 0.1, 20, 2, PExp, {}, Fmt::time), P ("depth", "DEPTH", 1, 6, 3, PInt),
           P ("warp", "WARP", -2, 2, 1, 0, "x"), Opt ("shrink", "SHRINK", { "NO", "YES" }), Opt ("formant", "FORMANTS", { "KEEP", "MOVE" }), W() }, fractalSpec, {}, false, "fractal spectrum");
    add (v, CAT, "cdp_speculate", "CHANGE RINGING", "SPEC", "Permutes the channel amplitudes between LOW and HIGH by STEP rounds of bell-ringing (Plain Bob) swaps; EVOLVE rings through all the changes over the sound. OTHERS can remove what lies outside the range.",
         { Opt ("mode", "MODE", { "FIXED STEP", "EVOLVE" }), P ("step", "STEP", 0, 400, 24, PInt), P ("lo", "LOW", 20, 5000, 200, PExp, {}, Fmt::hz), P ("hi", "HIGH", 200, 16000, 4000, PExp, {}, Fmt::hz),
           Opt ("others", "OTHERS", { "KEEP", "REMOVE" }), W() }, speculate, {}, false, "speculate speculate");
    add (v, CAT, "cdp_suppress_partials", "PARTIAL SUPPRESS", "SUPP", "Removes the COUNT most prominent partials between LOW and HIGH (fundamentals, whistles, hum) between START and END, leaving the rest of the spectrum.",
         { P ("n", "COUNT", 1, 64, 4, PInt), P ("lo", "LOW", 20, 10000, 80, PExp, {}, Fmt::hz), P ("hi", "HIGH", 50, 20000, 2000, PExp, {}, Fmt::hz),
           Pct ("start", "START", 0), Pct ("end", "END", 1), W() }, suppressPartials, {}, false, "suppress partials");
    add (v, CAT, "cdp_matrix", "SPECTRAL MATRIX", "MTRX", "Multiplies every FFT frame by a random matrix that mixes all CHANNELS into each other (CYCLE adds it in again every window, swelling), or SWAPs real and imaginary parts, or INVERTs the phases. Fewer channels = grainier.",
         { Opt ("mode", "MODE", { "MATRIX", "SWAP RE/IM", "INVERT PHASE" }), P ("chans", "CHANNELS", 4, 512, 64, PInt | PExp), P ("overlap", "OVERLAP", 1, 4, 2, PInt),
           Opt ("cycle", "CYCLE", { "NO", "YES" }), Seed() }, matrixFx, {}, false, "matrix matrix 1-4");
    add (v, CAT, "cdp_glisten", "GLISTEN", "GLIS", "Deals the spectrum's channels at random into SETS groups and plays one group at a time for SET TIME, each with a random PITCH shift: glittering, flickering versions of the sound. DUR RAND and SIZE RAND vary the timing and group sizes.",
         { P ("sets", "SETS", 2, 64, 6, PInt), P ("setdur", "SET TIME", 5, 2000, 80, PExp, "ms"), P ("shift", "PITCH", 0, 12, 0, 0, "st"), Pct ("durrand", "DUR RAND", 0.3),
           Pct ("divrand", "SIZE RAND", 0), Seed(), W() }, glistenFx, {}, false, "glisten glisten");
    add (v, CAT, "cdp_selfsim", "SELF-SIMILAR", "SSIM", "Makes the sound more self-similar: starting from the loudest, each spectral window replaces the INDEX windows that resemble it most (scaled to their level), so the strongest moments keep returning.",
         { P ("index", "INDEX", 1, 32, 2, PInt), W() }, selfsimFx, {}, false, "selfsim selfsim");
    add (v, CAT, "cdp_superaccu", "SUPER ACCUMULATE", "SACU", "Every partial sustains until something louder arrives in its channel, DECAYing per second and GLISSing (octaves per second). New resonances can start TEMPERED, on CHORD over ROOT, or on that chord in all octaves (FIELD).",
         { Opt ("mode", "TUNING", TUNEMODES), P ("decay", "DECAY", 0.001, 1, 0.2, PExp, "x/s"), P ("glis", "GLISS", -4, 4, 0, 0, "oct/s"), Opt ("reassign", "REASSIGN", { "NO", "YES" }, 1),
           Opt ("chord", "CHORD", CHORDS, 0), P ("root", "ROOT", 24, 72, 48, PInt, {}, Fmt::note), W() }, superaccuFx, {}, false, "superaccu superaccu 1-4");
    add (v, CAT, "cdp_tunevary", "TUNE VARYING", "TVAR", "Pulls the partials (FOCUS) onto the harmonics of CHORD on ROOT, stepping through PROGRESSION every STEP of the tempo (GLIDE between chords). CLARITY fades partials off the template, TRACE retunes only the N loudest.",
         { Opt ("chord", "CHORD", CHORDS, 0), P ("root", "ROOT", 24, 72, 45, PInt, {}, Fmt::note), Opt ("prog", "PROGRESSION", PROGS, 1), Opt ("div", "STEP", DIVS, 0),
           Pct ("glide", "GLIDE", 0), Pct ("focus", "FOCUS", 1), Pct ("clarity", "CLARITY", 0.6), P ("trace", "TRACE", 0, 256, 0, PInt), P ("bcut", "BASS CUT", 0, 500, 0, 0, {}, Fmt::hz), W() }, tunevaryFx, {}, false, "tunevary tunevary");

    add (v, CAT, "cdp_specfold", "SPECTRAL FOLD", "SFLD", "Rearranges COUNT channels from START: FOLD deals the lower half onto every other channel and the upper half backwards into the gaps (FOLDS times), INVERT flips them, RANDOM permutes them. AMP ONLY leaves frequencies in place.",
         { Opt ("mode", "MODE", { "FOLD", "INVERT", "RANDOM" }), P ("start", "START", 0, 5000, 100, 0, {}, Fmt::hz), P ("len", "COUNT", 4, 2048, 256, PInt | PExp), P ("folds", "FOLDS", 1, 16, 1, PInt),
           Opt ("what", "MOVES", { "AMP + FREQ", "AMP ONLY" }), Seed(), W() }, specfoldFx, {}, false, "specfold specfold 1-3");
    add (v, CAT, "cdp_spectstr", "DECOHERED STRETCH", "SSTR", "Phase-vocoder time stretch from STRETCH to END STRETCH; the quietest DECOHERE share of the channels get their frequencies scattered by RANDOM, which breaks up the metallic phasiness of long stretches.",
         { P ("stretch", "STRETCH", 0.25, 32, 4, PExp, "x"), P ("stretch2", "END STRETCH", 0.25, 32, 4, PExp, "x"), Pct ("dratio", "DECOHERE", 0.5), Pct ("drand", "RANDOM", 0.8), Seed(), W() },
         spectstrFx, {}, false, "spectstr stretch time");
    add (v, CAT, "cdp_spectune", "STATIC AUTOTUNE", "STUN", "Finds the most prominent pitch of the whole sound (between LOWEST and HIGHEST) and transposes it onto the nearest TEMPERED note, CHORD note, or chord note in any octave (FIELD), keeping the formants unless MOVE.",
         { Opt ("mode", "TARGET", { "TEMPERED", "CHORD", "CHORD FIELD" }, 2), Opt ("chord", "CHORD", CHORDS, 0), P ("root", "ROOT", 24, 72, 48, PInt, {}, Fmt::note),
           P ("lop", "LOWEST", 24, 96, 36, 0, {}, Fmt::note), P ("hip", "HIGHEST", 36, 108, 84, 0, {}, Fmt::note), Opt ("weight", "WEIGHT", { "LOUDNESS", "COUNT" }), Opt ("formant", "FORMANTS", { "KEEP", "MOVE" }), W() },
         spectuneFx, {}, false, "spectune tune 1-3");
    add (v, CAT, "cdp_spectune_pedal", "PEDAL TUNE", "SPED", "Window by window the PEAKS loudest spectral peaks are retuned to the nearest note of CHORD on ROOT (in any octave): the sound is pulled onto a pedal harmony while its noise and detail stay.",
         { Opt ("chord", "CHORD", CHORDS, 0), P ("root", "ROOT", 24, 72, 36, PInt, {}, Fmt::note), P ("peaks", "PEAKS", 1, 128, 24, PInt | PExp), W() }, specpedalFx, {}, false, "spectune tune 5");

    // ---- specfnu ----
    const std::string FCAT = "CDP \xc2\xb7 SPECTRAL II \xc2\xb7 FORMANTS";
    auto FW = [] { return Win (0.02, 0.2, 0.046); };
    auto HM = [] { return Opt ("harm", "PARTIALS", HARMOPT); };
    auto TOP = [] { return Opt ("top", "ZERO TOP", { "NO", "YES" }); };
    auto withArp = [&] (std::vector<ParamDef> ps)
    {
        ps.push_back (P ("arprate", "ARPEGGIO", 0, 50, 0, 0, "Hz")); ps.push_back (Opt ("arpdir", "ARP DIR", ARPDIR));
        ps.push_back (P ("locut", "LOCUT", 0, 10000, 0, 0, {}, Fmt::hz)); ps.push_back (P ("hicut", "HICUT", 50, 20000, 20000, PExp, {}, Fmt::hz));
        ps.push_back (Seed()); ps.push_back (FW()); return ps;
    };
    add (v, FCAT, "cdp_specfnu_narrow", "FORMANT NARROW", "FNRW", "Finds the four formants of each window and steepens their skirts: only 1/NARROW of each skirt survives (sweeping to END NARROW), giving resonant, filtered vowels. OFF removes chosen formants, ZERO TOP clears above formant 4.",
         { P ("narrow", "NARROW", 1, 100, 4, PExp, "x"), P ("narrow2", "END NARROW", 1, 100, 4, PExp, "x"), Opt ("off", "OFF", FSETS), TOP(), HM(), FW() }, fnuNarrow, {}, false, "specfnu specfnu 1");
    add (v, FCAT, "cdp_specfnu_squeeze", "FORMANT SQUEEZE", "FSQZ", "Squeezes the whole spectrum by SQUEEZE towards the peak of formant CENTRE (or the TROUGH above it): the sound collapses into a narrow, nasal band around that formant.",
         { P ("squeeze", "SQUEEZE", 1, 10, 2.5, PExp, "x"), P ("centre", "CENTRE", 1, 4, 2, PInt), Opt ("at", "AT", { "PEAK", "TROUGH" }), TOP(), HM(), FW() }, fnuSqueeze, {}, false, "specfnu specfnu 2");
    add (v, FCAT, "cdp_specfnu_invert", "FORMANT INVERT", "FINV", "Turns formant peaks into troughs and troughs into peaks around the smoothed spectral contour. VIBRATE (Hz) swings back and forth between the original and the inverted formants.",
         { P ("vib", "VIBRATE", 0, 20, 0, 0, "Hz"), HM(), FW() }, fnuInvert, {}, false, "specfnu specfnu 3");
    add (v, FCAT, "cdp_specfnu_rotate", "FORMANT ROTATE", "FROT", "The formant region (between the lowest and highest formant troughs) rotates upwards (or downwards for negative SPEED), what leaves the top reappearing at the bottom: SPEED rotations per second.",
         { P ("rspeed", "SPEED", -50, 50, 0.5, 0, "/s"), HM(), FW() }, fnuRotate, {}, false, "specfnu specfnu 4");
    add (v, FCAT, "cdp_specfnu_negate", "SPECTRAL NEGATIVE", "FNEG", "Spectral negative: every channel's level is inverted between the window's loudest and quietest (loud becomes quiet), then put back under the original contour unless FLAT.",
         { Opt ("flat", "CONTOUR", { "KEEP", "FLAT" }), FW() }, fnuNegate, {}, false, "specfnu specfnu 5");
    add (v, FCAT, "cdp_specfnu_suppress", "FORMANT SUPPRESS", "FSUP", "Removes the chosen FORMANTS (between their troughs) from each window: vowels lose their colour, voices become hollow or muffled.",
         { Opt ("off", "FORMANTS", FSETS, 2), HM(), FW() }, fnuSuppress, {}, false, "specfnu specfnu 6");
    add (v, FCAT, "cdp_specfnu_move", "FORMANT MOVE", "FMOV", "Moves each of the four formants on its own: BY a number of Hz, or TO a fixed frequency (F1..F4), carrying their partials along. Turns one vowel into another or into something inhuman.",
         { Opt ("mode", "MODE", { "MOVE BY", "MOVE TO" }), P ("f1", "F1", -2000, 5000, 200, 0, "Hz"), P ("f2", "F2", -2000, 8000, -300, 0, "Hz"), P ("f3", "F3", -2000, 8000, 400, 0, "Hz"),
           P ("f4", "F4", -2000, 10000, 0, 0, "Hz"), TOP(), HM(), FW() }, fnuMove, {}, false, "specfnu specfnu 8-9");
    add (v, FCAT, "cdp_specfnu_arpeg", "FORMANT ARPEGGIO", "FARP", "A window four partials wide runs over harmonics 1 to 8 of the tracked pitch at RATE, UP, DOWN or both, so the harmonics sound one after another while the formants stay put.",
         { P ("arprate", "RATE", 0.1, 50, 4, PExp, "Hz"), Opt ("arpdir", "DIRECTION", ARPDIR), P ("locut", "LOCUT", 0, 10000, 0, 0, {}, Fmt::hz), P ("hicut", "HICUT", 50, 20000, 20000, PExp, {}, Fmt::hz),
           Seed(), FW() }, fnuArpeg, {}, false, "specfnu specfnu 10");
    add (v, FCAT, "cdp_specfnu_octave", "FORMANT OCTAVER", "FOCT", "Shifts the pitch by OCTAVES while the formant envelope stays where it was: a voice an octave down that is not a monster. ARPEGGIO can also sweep through its harmonics; LOCUT / HICUT trim the result.",
         withArp ({ P ("oct", "OCTAVES", -4, 4, -1, PInt) }), fnuOct, {}, false, "specfnu specfnu 11");
    add (v, FCAT, "cdp_specfnu_transpose", "FORMANT TRANSPOSE", "FTRN", "Transposes the partials by SEMITONES under the original formant envelope (formant-preserving pitch shift). ARPEGGIO can also sweep through the harmonics; LOCUT / HICUT trim the result.",
         withArp ({ P ("trans", "SEMITONES", -48, 48, 5, 0, "st") }), fnuTrans, {}, false, "specfnu specfnu 12");
    add (v, FCAT, "cdp_specfnu_shift", "FORMANT FREQ SHIFT", "FSHF", "Shifts every partial by SHIFT Hz (inharmonic, bell-like) while the formant envelope stays in place, so speech stays intelligible. ARPEGGIO and LOCUT / HICUT as in the other formant processes.",
         withArp ({ P ("shift", "SHIFT", -1000, 1000, 120, 0, "Hz") }), fnuShift, {}, false, "specfnu specfnu 13");
    add (v, FCAT, "cdp_specfnu_respace", "FORMANT RESPACE", "FRSP", "Re-spaces the partials above the fundamental to a new SPACING (Hz) while the formants stay put: harmonic sounds turn inharmonic, stretched or squashed.",
         withArp ({ P ("respace", "SPACING", 1, 1000, 230, PExp, "Hz") }), fnuRespace, {}, false, "specfnu specfnu 14");
    add (v, FCAT, "cdp_specfnu_randpartials", "FORMANT PARTIAL RAND", "FRPT", "Scatters every partial's frequency at random by up to RAND of half the harmonic spacing, under the original formants: rough, breathy or broken voices.",
         withArp ({ Pct ("rand", "RAND", 0.5) }), fnuRand, {}, false, "specfnu specfnu 19");
    add (v, FCAT, "cdp_specfnu_pinvert", "PITCH INVERT", "FPIV", "Mirrors the tracked pitch line around ABOUT (MIDI; 0 = the mean pitch): rising melodies fall, high notes go low. The formants stay put.",
         withArp ({ P ("about", "ABOUT", 0, 96, 0, 0, {}, Fmt::note) }), fnuPInvert, {}, false, "specfnu specfnu 15");
    add (v, FCAT, "cdp_specfnu_pexagg", "PITCH EXAGGERATE", "FPEX", "Scales the tracked pitch line's distance from ABOUT (MIDI; 0 = mean pitch) by RANGE: above 1 exaggerates the melody, below 1 flattens it towards a monotone, on SIDE of the mean.",
         withArp ({ P ("about", "ABOUT", 0, 96, 0, 0, {}, Fmt::note), P ("rang", "RANGE", 0, 4, 2, 0, "x"), Opt ("side", "SIDE", { "BOTH", "ABOVE", "BELOW" }) }), fnuPExagg, {}, false, "specfnu specfnu 16");
    add (v, FCAT, "cdp_specfnu_pquant", "PITCH QUANTISE", "FPQT", "Auto-tune under formants: the tracked pitch is forced onto SCALE over ROOT, GLIDE smoothing the jumps between notes (0 = hard steps).",
         withArp ({ Opt ("scale", "SCALE", QSCALES, 1), P ("root", "ROOT", 0, 11, 0, PInt, "st"), P ("glide", "GLIDE", 0, 300, 20, 0, "ms") }), fnuPQuant, {}, false, "specfnu specfnu 17");
    add (v, FCAT, "cdp_specfnu_prand", "PITCH RANDOMISE", "FPRN", "Randomises the pitch line under the formants: new random offsets within RANGE semitones at RATE (SLEW > 1 favours upward moves), optionally pre-quantised to SCALE over ROOT.",
         withArp ({ P ("range", "RANGE", 0, 24, 5, 0, "st"), P ("slew", "SLEW", 0.1, 10, 1, PExp), P ("rate", "RATE", 0.2, 20, 3, PExp, "Hz"),
                    Opt ("scale", "SCALE", [] { std::vector<std::string> o { "NONE" }; o.insert (o.end(), QSCALES.begin(), QSCALES.end()); return o; }()),
                    P ("root", "ROOT", 0, 11, 0, PInt, "st"), P ("glide", "GLIDE", 0, 300, 30, 0, "ms") }), fnuPRand, {}, false, "specfnu specfnu 18");
    add (v, FCAT, "cdp_specfnu_sinus", "SINE SPEECH", "FSIN", "Sine-wave speech: each of the four formants becomes one sine at its peak frequency carrying its energy (AMP 1-4). SINING mixes original and sines; SCALE / DEPTH pull the sines onto notes.",
         { Pct ("sining", "SINING", 1), P ("amp1", "AMP 1", 0.01, 10, 1, PExp, "x"), P ("amp2", "AMP 2", 0.01, 10, 1, PExp, "x"), P ("amp3", "AMP 3", 0.01, 10, 1, PExp, "x"), P ("amp4", "AMP 4", 0.01, 10, 0.7, PExp, "x"),
           Opt ("scale", "SCALE", [] { std::vector<std::string> o { "NONE" }; o.insert (o.end(), QSCALES.begin(), QSCALES.end()); return o; }()), P ("root", "ROOT", 0, 11, 0, PInt, "st"),
           Pct ("qdep", "DEPTH", 1), Opt ("smooth", "SMOOTH", { "NO", "YES" }, 1), FW() }, fnuSinus, {}, false, "specfnu specfnu 23");

}
} // namespace lab
