// CDP collection, Batch 3 - Two-input: cross-synthesis, morph, convolution, interleave, switching.
// Every process uses the source plus ONE aux sample (CDP programs that take many sounds use the source and the aux).
#include "LabCdp.h"
#include "LabDspUtil.h"

namespace lab
{
namespace
{
using namespace lab::dsp;

// ===================================================================================================
// shared helpers
// ===================================================================================================

// the aux sample, sanitised (finite values, every channel the same length); throws when there is none
Chans getAux (const Ctx& cx, const char* msg)
{
    if (! cx.aux || cx.aux->empty()) throw LabError (msg);
    Chans a;
    for (auto& c : *cx.aux) if (! c.empty()) a.push_back (c);
    if (a.empty()) throw LabError (msg);
    const size_t n = a[0].size();
    for (auto& b : a) { b.resize (n); for (auto& v : b) if (! std::isfinite (v)) v = 0; }
    return a;
}
const char* AUXMSG = "Choose a SECOND SOUND sample";

inline double princ (double a) { return a - TAU * std::floor ((a + PI) / TAU); }
inline size_t capLen (const Ctx& cx, double n) { return (size_t) std::max (1.0, std::min ((double) cx.maxLen, n)); }

// aux read position (in aux samples) for a position in the main sound: STRETCH spans the main's length, LOOP plays at real speed
inline double auxPos (double mainPos, size_t mainLen, size_t auxLen, int fit)
{
    if (fit == 0) return mainPos * (double) auxLen / (double) std::max ((size_t) 1, mainLen);
    return mainPos;
}

// windowed FFT of a frame starting at pos; with loop the signal repeats
void anaWrap (const Buf& x, long long pos, const DVec& w, DVec& re, DVec& im, bool loop)
{
    const size_t N = w.size(); const long long L = (long long) x.size();
    for (size_t i = 0; i < N; ++i)
    {
        long long j = pos + (long long) i;
        if (loop && L > 0) { j %= L; if (j < 0) j += L; re[i] = x[(size_t) j] * w[i]; }
        else re[i] = j >= 0 && j < L ? x[(size_t) j] * w[i] : 0.0;
        im[i] = 0;
    }
    fft (re, im, false);
}

// one CDP-style analysis window: amplitude and true frequency (in bins) of every channel
struct Spec { DVec amp, frq; void init (size_t n) { amp.assign (n, 0); frq.assign (n, 0); } };

// analysis of a single window at pos (frequency from the phase advance over one hop)
void specAt (const Buf& x, long long pos, const DVec& w, size_t H, bool loop, DVec& re, DVec& im, DVec& ph0, Spec& s)
{
    const size_t N = w.size(), half = N >> 1; const double om = TAU * (double) H / (double) N;
    if (s.amp.size() != half + 1) s.init (half + 1);
    ph0.resize (half + 1);
    anaWrap (x, pos - (long long) H, w, re, im, loop);
    for (size_t k = 0; k <= half; ++k) ph0[k] = std::atan2 (im[k], re[k]);
    anaWrap (x, pos, w, re, im, loop);
    for (size_t k = 0; k <= half; ++k)
    {
        s.amp[k] = std::hypot (re[k], im[k]);
        double d = princ (std::atan2 (im[k], re[k]) - ph0[k] - (double) k * om);
        s.frq[k] = (double) k + d / om;
    }
}

struct XSt { int c = 0; size_t N = 0, H = 0, half = 0; double binHz = 1; bool init = false; DVec a, b, d, e; std::vector<int> iv; };
// A = main window (edit in place), B = aux window, t = frame centre / main length
using XFn = std::function<void (Spec& A, Spec& B, XSt& st, long long f, double t)>;

// Two-input phase-vocoder pass: the main and the aux are analysed window by window (aux STRETCHed over the main or LOOPed),
// fn edits the main window; keepPhase resynthesises with the main's own phases (amplitude-only processes),
// otherwise the phases are accumulated from the edited frequencies (CDP pvoc resynthesis).
Chans xRun (const Chans& ch, const Chans& aux, Ctx& cx, double win, size_t outLen, int fit, bool keepPhase, const XFn& fn, double freezeAt = -1)
{
    const size_t N = pow2 (win * cx.sr), H = N >> 2, half = N >> 1; const DVec w = hann (N);
    const double om = TAU * (double) H / (double) N;
    const long long frames = (long long) std::ceil ((double) outLen / (double) H) + 1;
    const size_t mainLen = ch[0].size();
    DVec re (N), im (N), ph (half + 1), last (half + 1), acc (half + 1), tmp;
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; const Buf& y = aux[c % aux.size()];
        Buf out (outLen); Spec A, B; A.init (half + 1); B.init (half + 1);
        XSt st; st.c = (int) c; st.N = N; st.H = H; st.half = half; st.binHz = cx.sr / (double) N;
        for (long long f = 0; f < frames; ++f)
        {
            const long long pos = f * (long long) H - (long long) half;
            analyze (x, pos, w, re, im);
            for (size_t k = 0; k <= half; ++k)
            {
                A.amp[k] = std::hypot (re[k], im[k]); ph[k] = std::atan2 (im[k], re[k]);
                A.frq[k] = f == 0 ? (double) k : (double) k + princ (ph[k] - last[k] - (double) k * om) / om;
                last[k] = ph[k];
            }
            const double centre = (double) (pos + (long long) half);
            if (freezeAt < 0 || f == 0)
            {
                const long long ap = jr (freezeAt >= 0 ? freezeAt * (double) y.size() : auxPos (centre, mainLen, y.size(), fit)) - (long long) half;
                specAt (y, ap, w, H, fit == 1 && freezeAt < 0, re, im, tmp, B);
            }
            Spec Bf; if (freezeAt >= 0) { Bf = B; }
            Spec& BB = freezeAt >= 0 ? Bf : B;
            fn (A, BB, st, f, centre / (double) std::max ((size_t) 1, mainLen));
            for (size_t k = 0; k <= half; ++k)
            {
                if (keepPhase) acc[k] = ph[k];
                else acc[k] = f == 0 ? ph[k] : acc[k] + A.frq[k] * om;
                double a = std::isfinite (A.amp[k]) ? A.amp[k] : 0.0;
                re[k] = a * std::cos (acc[k]); im[k] = a * std::sin (acc[k]);
            }
            mirror (re, im); synth (out, pos, w, re, im, 2.0 / 3.0);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        res.push_back (std::move (out));
    }
    return res;
}

// pure resynthesis from generated windows (amp + frequency in bins), phase accumulated
using GenFn = std::function<void (Spec& O, int c, long long f)>;
Chans pvGen (size_t nch, Ctx& cx, size_t N, size_t outLen, const GenFn& gen)
{
    const size_t H = N >> 2, half = N >> 1; const DVec w = hann (N); const double om = TAU * (double) H / (double) N;
    const long long frames = (long long) std::ceil ((double) outLen / (double) H) + 1;
    DVec re (N), im (N), acc (half + 1);
    Chans res;
    for (size_t c = 0; c < nch; ++c)
    {
        Buf out (outLen); Spec O; O.init (half + 1); for (size_t k = 0; k <= half; ++k) { double q = (double) k * (double) k * 0.6180339887; acc[k] = TAU * (q - std::floor (q)); }   // spread start phases: no click
        for (long long f = 0; f < frames; ++f)
        {
            const long long pos = f * (long long) H - (long long) half;
            gen (O, (int) c, f);
            for (size_t k = 0; k <= half; ++k)
            {
                acc[k] += O.frq[k] * om; double a = std::isfinite (O.amp[k]) ? O.amp[k] : 0.0;
                re[k] = a * std::cos (acc[k]); im[k] = a * std::sin (acc[k]);
            }
            mirror (re, im); synth (out, pos, w, re, im, 2.0 / 3.0);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) nch);
        }
        res.push_back (std::move (out));
    }
    return res;
}

inline double sumAmp (const DVec& a) { double s = 0; for (double v : a) s += v; return s; }
inline double envAt (const DVec& e, double bin) { return lerpAt (e, clampd (bin, 0, (double) e.size() - 1)); }
// CDP warped cosine interpolation (morph mode 2)
inline double warpCos (double r, double ex)
{
    auto tw = [&] (double v) { v = std::min (v * 2, 1.0); return std::pow (v, ex) / 2; };
    if (r < 0.5) r = tw (r); else if (r > 0.5) r = 1 - tw (1 - r);
    return 0.5 - 0.5 * std::cos (PI * r);
}
const defs::O FIT { "STRETCH", "LOOP" };

// ===================================================================================================
// combine (dev/combine/combine.c)
// ===================================================================================================

// PITCH + FORMANTS: harmonics on the main's pitch, shaped by the aux's spectral envelope (noise when unpitched).
// after CDP specmake (combine make / make2, combine.c)
Chans combMakeImpl (const Chans& ch, Ctx& cx, const Params& p, double freeze)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size();
    const auto pt = trackPitch (monoOf (ch), sr, 50, 1500, 0.15, &cx);
    const int mode = p.i ("mode"); const double noise = p["noise"], tr = std::pow (2.0, p["transp"] / 12);
    cx.seed (p["seed"]);
    const size_t N = pow2 (p["win"] * sr);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), false, [&] (Spec& A, Spec& B, XSt& st, long long, double t)
    {
        const size_t half = st.half;
        if (! st.init) { st.init = true; st.a.assign (half + 1, 0); }
        envelope (B.amp, p["smooth"] / st.binHz, st.a);
        const double own = sumAmp (A.amp);
        double f0 = pt.at (t * (double) len) * tr;
        for (size_t k = 0; k <= half; ++k) { A.amp[k] = 0; A.frq[k] = (double) k; }
        if (own < 1e-6) return;   // silent window
        if (f0 > 0)
        {
            const double spacing = std::max (1.0, f0 / st.binHz);
            for (int n = 1; f0 * n < sr * 0.5 && n < 2000; ++n)
            {
                double b = f0 * n / st.binHz; size_t j = (size_t) jr (b); if (j > half) break;
                double a = envAt (st.a, b) * spacing;
                if (a > A.amp[j]) { A.amp[j] = a; A.frq[j] = b; }
            }
        }
        else
            for (size_t k = 1; k <= half; ++k)
            {
                double b = (double) k + cx.rand() - 0.5;
                A.amp[k] = st.a[k] * cx.rand() * 0.5 * noise; A.frq[k] = b;
            }
        if (mode == 1) { double s = sumAmp (A.amp); if (s > 1e-12) for (auto& v : A.amp) v *= own / s; }
    }, freeze);
}
Chans combMake (const Chans& ch, Ctx& cx, const Params& p) { return combMakeImpl (ch, cx, p, -1); }
// ONEFORM COMBINE: as PITCH + FORMANTS, but with one frozen formant snapshot of the aux (TAKE AT).
// after CDP oneform_combine (oneform combine, oneform.c)
Chans oneCombine (const Chans& ch, Ctx& cx, const Params& p) { return combMakeImpl (ch, cx, p, clampd (p["at"], 0, 1)); }


// SPECTRAL SUM: adds the aux's channel amplitudes (scaled by CROSS) to the main's. after CDP specsum (combine.c)
Chans combSum (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), true, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    { for (size_t k = 0; k <= st.half; ++k) A.amp[k] += p["cross"] * B.amp[k]; });
}

// SPECTRAL SUBTRACT: removes the aux's channel amplitudes from the main's (spectral subtraction / denoise).
// after CDP specdiff (combine.c)
Chans combDiff (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const bool neg = p.i ("neg") == 1;
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), true, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    {
        for (size_t k = 0; k <= st.half; ++k)
        {
            double v = A.amp[k] - p["cross"] * B.amp[k];
            A.amp[k] = neg ? v : std::max (0.0, v);   // a negative amplitude = inverted phase
        }
    });
}

// WINDOW INTERLEAVE: groups of LEAF analysis windows taken alternately from the main and the aux. after CDP specleaf (combine.c)
Chans combLeaf (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const long long L = std::max (1, p.i ("leaf"));
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), false, [&] (Spec& A, Spec& B, XSt&, long long f, double)
    { if ((f / L) % 2 == 1) { A.amp = B.amp; A.frq = B.frq; } });
}

// SPECTRAL MAX: in every channel the louder of the two sounds wins. after CDP specmax (combine.c)
Chans combMax (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), false, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    {
        for (size_t k = 0; k <= st.half; ++k)
            if (B.amp[k] * p["gain2"] > A.amp[k]) { A.amp[k] = B.amp[k] * p["gain2"]; A.frq[k] = B.frq[k]; }
    });
}

// SPECTRAL MEAN: the loudest channels of both sounds are paired by rank; each pair gives one partial with a mean (or
// one-sided / max) amplitude at the mean frequency or mean pitch. after CDP specmean (combine.c)
Chans combMean (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const int mode = p.i ("mode");
    const bool zero = p.i ("outside") == 1;
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), false, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    {
        const size_t half = st.half;
        const size_t lo = (size_t) clampd (std::floor (p["lo"] / st.binHz), 0, (double) half), hi = (size_t) clampd (std::ceil (p["hi"] / st.binHz), (double) lo + 1, (double) half + 1);
        if (zero) { for (size_t k = 0; k < lo; ++k) A.amp[k] = 0; for (size_t k = hi; k <= half; ++k) A.amp[k] = 0; }
        std::vector<int> i1, i2;
        for (size_t k = lo; k < hi; ++k) { if (A.amp[k] > 0) i1.push_back ((int) k); if (B.amp[k] > 0) i2.push_back ((int) k); }
        const size_t n = std::min ({ i1.size(), i2.size(), (size_t) std::max (1, p.i ("chans")) });
        auto byAmp = [] (const DVec& a) { return [&a] (int x, int y) { return a[(size_t) x] > a[(size_t) y]; }; };
        std::partial_sort (i1.begin(), i1.begin() + (long) n, i1.end(), byAmp (A.amp));
        std::partial_sort (i2.begin(), i2.begin() + (long) n, i2.end(), byAmp (B.amp));
        DVec oa (half + 1, 0.0), of = A.frq;
        for (size_t r = 0; r < n; ++r)
        {
            const size_t a = (size_t) i1[r], b = (size_t) i2[r];
            const double f1 = std::abs (A.frq[a]), f2 = std::abs (B.frq[b]), a1 = A.amp[a], a2 = B.amp[b];
            const int am = mode / 2; const bool pitchwise = mode % 2 == 0;
            double na = am == 0 ? (a1 + a2) / 2 : am == 1 ? a1 : am == 2 ? a2 : std::max (a1, a2);
            double nf = pitchwise ? (f1 <= 0 ? f2 : f2 <= 0 ? f1 : std::sqrt (f1 * f2)) : (f1 + f2) / 2;
            long long j = jr (nf);
            if (j >= (long long) lo && j < (long long) hi && na > oa[(size_t) j]) { oa[(size_t) j] = na; of[(size_t) j] = nf; }
        }
        for (size_t k = lo; k < hi; ++k) { A.amp[k] = oa[k]; A.frq[k] = of[k]; }
    });
}

// SPECTRAL CROSS: the main's channel amplitudes move towards the aux's by INTERP (1 = full cross-synthesis).
// after CDP speccross (combine.c)
Chans combCross (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), true, [&] (Spec& A, Spec& B, XSt& st, long long, double t)
    {
        const double ip = p["interp"] + (p["interp2"] - p["interp"]) * clampd (t, 0, 1);
        for (size_t k = 0; k <= st.half; ++k) A.amp[k] += (B.amp[k] - A.amp[k]) * ip;
    });
}

// ===================================================================================================
// formants (dev/formants/formants.c)
// ===================================================================================================

// FORMANT PUT: the aux's spectral envelope (formants) replaces the main's own (REPLACE) or is laid on top of it (IMPOSE).
// after CDP specform (formants put 1-2, formants.c)
Chans formPutImpl (const Chans& ch, Ctx& cx, const Params& p, double freeze)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const int mode = p.i ("mode");
    const double g = std::pow (10.0, p["gain"] / 20);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), true, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    {
        const size_t half = st.half;
        if (! st.init) { st.init = true; st.a.assign (half + 1, 0); st.b.assign (half + 1, 0); }
        const double Bw = p["smooth"] / st.binHz;
        envelope (A.amp, Bw, st.a); envelope (B.amp, Bw, st.b);
        double mx = 1e-12; for (double v : st.b) mx = std::max (mx, v);
        double mxa = 1e-12; for (double v : st.a) mxa = std::max (mxa, v);
        for (size_t k = 0; k <= half; ++k)
        {
            const double fb = std::abs (A.frq[k]), hz = fb * st.binHz;
            double e1 = envAt (st.b, fb);
            if (mode == 0) A.amp[k] = A.amp[k] * e1 / std::max (envAt (st.a, fb), mxa * 1e-2);
            else A.amp[k] *= e1 / mx * 4;
            if (hz < p["lo"] || hz > p["hi"]) A.amp[k] = 0;
            A.amp[k] *= g;
        }
    }, freeze);
}
Chans formPut (const Chans& ch, Ctx& cx, const Params& p) { return formPutImpl (ch, cx, p, -1); }
// ONEFORM PUT: one frozen formant snapshot of the aux (TAKE AT) replaces or is imposed on the main's formants.
// after CDP do_specform (oneform put 1-2, oneform.c)
Chans onePut (const Chans& ch, Ctx& cx, const Params& p) { return formPutImpl (ch, cx, p, clampd (p["at"], 0, 1)); }


// VOCODER: the aux's spectral envelope replaces the main's, level kept, band limited to LOW..HIGH.
// after CDP specvocode (formants vocode, formants.c)
Chans formVocode (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr);
    const double g = std::pow (10.0, p["gain"] / 20);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), true, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    {
        const size_t half = st.half;
        if (! st.init) { st.init = true; st.a.assign (half + 1, 0); st.b.assign (half + 1, 0); }
        const double Bw = p["smooth"] / st.binHz;
        envelope (A.amp, Bw, st.a); envelope (B.amp, Bw, st.b);
        const double pre = sumAmp (A.amp);
        double mxa = 1e-12; for (double v : st.a) mxa = std::max (mxa, v);
        for (size_t k = 0; k <= half; ++k)
        {
            const double fb = std::abs (A.frq[k]), hz = fb * st.binHz;
            A.amp[k] = A.amp[k] * envAt (st.b, std::abs (B.frq[k])) / std::max (envAt (st.a, fb), mxa * 1e-2);
            if (hz < p["lo"] || hz > p["hi"]) A.amp[k] = 0;
        }
        const double post = sumAmp (A.amp);
        const double nrm = post > 1e-12 ? pre / post : 0;
        for (auto& v : A.amp) v *= nrm * g;
    });
}

// ===================================================================================================
// morph (dev/morph/morph.c)
// ===================================================================================================

// SPECTRAL GLIDE: one analysis window of the main (at MAIN AT) glides into one window of the aux (AUX AT) over DURATION:
// amplitudes linearly, frequencies exponentially. after CDP specglide (morph glide, morph.c)
Chans morphGlide (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t N = pow2 (p["win"] * sr), H = N >> 2, half = N >> 1;
    const size_t outLen = capLen (cx, p["dur"] * sr); const DVec w = hann (N); DVec re (N), im (N), tmp;
    std::vector<Spec> s0 (ch.size()), s1 (ch.size());
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; const Buf& y = aux[c % aux.size()];
        specAt (x, jr (p["t1"] * (double) x.size()) - (long long) half, w, H, false, re, im, tmp, s0[c]);
        specAt (y, jr (p["t2"] * (double) y.size()) - (long long) half, w, H, false, re, im, tmp, s1[c]);
    }
    const double frames = std::max (1.0, (double) outLen / (double) H);
    return pvGen (ch.size(), cx, N, outLen, [&] (Spec& O, int c, long long f)
    {
        const Spec& a = s0[(size_t) c]; const Spec& b = s1[(size_t) c]; const double u = clampd ((double) f / frames, 0, 1);
        for (size_t k = 0; k <= half; ++k)
        {
            double f1 = std::abs (a.frq[k]), f2 = std::abs (b.frq[k]);
            if (f1 < 1e-6 || f2 < 1e-6) { O.amp[k] = 0; O.frq[k] = (double) k; continue; }
            O.amp[k] = a.amp[k] + u * (b.amp[k] - a.amp[k]);
            O.frq[k] = f1 * std::pow (f2 / f1, u);
        }
    });
}

// SPECTRAL BRIDGE: between START and END the output moves from (start frq/amp fractions of the aux) to (end fractions),
// with six level-following modes. after CDP specbridge (morph bridge 1-6, morph.c)
Chans morphBridge (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const int mode = p.i ("mode");
    const double t0 = p["start"], t1 = std::max (p["start"] + 1e-3, p["end"]);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), false, [&] (Spec& A, Spec& B, XSt& st, long long, double t)
    {
        const double u = clampd ((t - t0) / (t1 - t0), 0, 1);
        const double sf = p["sf2"] + (p["ef2"] - p["sf2"]) * u, sa = p["sa2"] + (p["ea2"] - p["sa2"]) * u;
        double s1 = sumAmp (A.amp), s2 = sumAmp (B.amp), g1 = 1, g2 = 1;
        auto rt = [] (double a, double b) { return b > 1e-12 ? a / b : 0.0; };
        switch (mode)
        {
            case 1: if (s1 > s2) g1 = rt (s2, s1); else g2 = rt (s1, s2); break;   // to the quieter
            case 2: g2 = rt (s1, s2); break;                                       // to the main's level
            case 3: g1 = rt (s2, s1); break;                                       // to the aux's level
            case 4: case 5:
            {
                double v = mode == 4 ? u : 1 - u;   // level glides main -> aux (4) or aux -> main (5)
                double T = s1 > 1e-12 && s2 > 1e-12 ? std::pow (s1, 1 - v) * std::pow (s2, v) : 0;
                g1 = rt (T, s1); g2 = rt (T, s2); break;
            }
            default: break;
        }
        for (size_t k = 0; k <= st.half; ++k)
        {
            A.amp[k] = B.amp[k] * sa * g2 + A.amp[k] * (1 - sa) * g1;
            A.frq[k] = B.frq[k] * sf + A.frq[k] * (1 - sf);
        }
    });
}

// SPECTRAL MORPH: amplitudes morph from main to aux between AMP START..END, frequencies between FRQ START..END,
// each with its own curve exponent; LINEAR or COSINE interpolation. after CDP specmorph (morph morph 1-2, morph.c)
Chans morphMorph (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const int mode = p.i ("mode");
    auto ratio = [&] (double t, double a, double b, double ex)
    {
        b = std::max (a + 1e-3, b);
        if (t <= a) return 0.0;
        if (t >= b) return 1.0;
        double r = (t - a) / (b - a);
        return mode == 0 ? std::pow (r, ex) : warpCos (r, ex);
    };
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), false, [&] (Spec& A, Spec& B, XSt& st, long long, double t)
    {
        const double ra = ratio (t, p["as"], p["ae"], p["expa"]), rf = ratio (t, p["fs"], p["fe"], p["expf"]);
        for (size_t k = 0; k <= st.half; ++k) { A.amp[k] += (B.amp[k] - A.amp[k]) * ra; A.frq[k] += (B.frq[k] - A.frq[k]) * rf; }
    });
}

// ===================================================================================================
// time-domain helpers: wavecycles, grains, splices
// ===================================================================================================

// upward zero crossings of x, cycles at least minLen long; returns boundaries (first 0, last x.size())
std::vector<size_t> upCross (const Buf& x, size_t minLen)
{
    std::vector<size_t> z { 0 }; minLen = std::max ((size_t) 2, minLen);
    for (size_t i = 1; i < x.size(); ++i) if (x[i - 1] < 0 && x[i] >= 0 && i - z.back() >= minLen) z.push_back (i);
    if (x.size() > z.back()) z.push_back (x.size());
    return z;
}
inline double sampAt (const Buf& x, double p, bool loop)
{
    const double L = (double) x.size(); if (L < 1) return 0;
    if (loop) { p = std::fmod (p, L); if (p < 0) p += L; double fi = std::floor (p); size_t i = (size_t) fi; double f = p - fi; return x[i] + (x[(i + 1) % x.size()] - x[i]) * f; }
    return lerpAt (x, p);
}
// grains = stretches above GATE (fraction of the peak) separated by holes of at least minHole seconds (CDP grain detection)
struct Grain { size_t a, b; };
std::vector<Grain> findGrains (const Buf& x, double sr, double gate, double minHole)
{
    std::vector<Grain> g; const size_t blk = (size_t) std::max (1.0, sr * 0.002), nb = x.size() / blk + 1;
    DVec env (nb, 0.0);
    for (size_t i = 0; i < x.size(); ++i) env[i / blk] = std::max (env[i / blk], (double) std::abs (x[i]));
    double pk = 0; for (double v : env) pk = std::max (pk, v);
    if (pk <= 1e-9) return g;
    const double thr = pk * clampd (gate, 0.001, 0.99); const size_t hole = (size_t) std::max (1.0, minHole * sr / (double) blk);
    bool in = false; size_t start = 0, below = 0, belowAt = 0;
    for (size_t b = 0; b < nb; ++b)
    {
        if (env[b] >= thr) { if (! in) { in = true; start = b * blk; } below = 0; }
        else if (in) { if (below == 0) belowAt = b; if (++below >= hole) { g.push_back ({ start, std::min (x.size(), belowAt * blk) }); in = false; below = 0; } }
    }
    if (in) g.push_back ({ start, x.size() });
    g.erase (std::remove_if (g.begin(), g.end(), [] (const Grain& q) { return q.b <= q.a + 1; }), g.end());
    return g;
}
// adds src[a, a+n) (looped when asked) into out at position at, with linear splices of spl samples, gain g
void addSeg (Buf& out, long long at, const Buf& src, double a, long long n, long long spl, double g, bool loop, double rate = 1)
{
    spl = std::max (1LL, std::min (spl, n / 2));
    for (long long i = 0; i < n; ++i)
    {
        long long o = at + i; if (o < 0) continue; if (o >= (long long) out.size()) break;
        double e = 1; if (i < spl) e = (double) i / (double) spl; if (n - 1 - i < spl) e = std::min (e, (double) (n - 1 - i) / (double) spl);
        out[(size_t) o] = (float) (out[(size_t) o] + sampAt (src, a + (double) i * rate, loop) * e * g);
    }
}
// windowed abs-peak envelope (CDP envel extraction), one value per window, read with linear interpolation
struct Env { DVec v; double wsz = 1; double at (double pos) const { if (v.empty()) return 0; double q = pos / wsz - 0.5; return lerpAt (v, clampd (q, 0, (double) v.size() - 1)); } };
Env envOf (const Buf& x, double wsz)
{
    Env e; e.wsz = std::max (1.0, wsz); const size_t n = (size_t) std::ceil ((double) x.size() / e.wsz); e.v.assign (std::max ((size_t) 1, n), 0.0);
    for (size_t i = 0; i < x.size(); ++i) { size_t k = std::min (e.v.size() - 1, (size_t) ((double) i / e.wsz)); e.v[k] = std::max (e.v[k], (double) std::abs (x[i])); }
    return e;
}

// ===================================================================================================
// two-sound time-domain processes
// ===================================================================================================

// WAVECYCLE INTERACT: INTERLEAVE alternates GROUP wavecycles from each sound; RESIZE plays the aux's wavecycles
// at the lengths of the main's (the aux's timbre on the main's pitch and rhythm).
// after CDP two_infiles_interleave_process (distintlv.c) / two_infiles_resize_process (distresize.c), distort interact 1-2
Chans distInteract (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const int mode = p.i ("mode"), G = std::max (1, p.i ("group"));
    const size_t minLen = (size_t) std::max (2.0, cx.sr / std::max (20.0, p["cutoff"]));
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; const Buf& y = aux[c % aux.size()];
        auto zx = upCross (x, minLen), zy = upCross (y, minLen);
        const size_t nx = zx.size() - 1, ny = std::max ((size_t) 1, zy.size() - 1);
        Buf out;
        if (mode == 0)
        {
            out.reserve (x.size() * 2); size_t j = 0;
            for (size_t i = 0; i < nx && out.size() < std::min (cx.maxLen, 3 * x.size() + 4096); i += (size_t) G)
            {
                for (size_t q = i; q < std::min (nx, i + (size_t) G); ++q) for (size_t s = zx[q]; s < zx[q + 1]; ++s) out.push_back (x[s]);
                for (int q = 0; q < G && ! y.empty(); ++q, ++j) { size_t k = j % ny; for (size_t s = zy[k]; s < zy[k + 1] && s < y.size(); ++s) out.push_back (y[s]); }
                if (! (i & 255)) cx.prog (((double) c + (double) i / (double) nx) / (double) ch.size());
            }
        }
        else
        {
            out.assign (x.size(), 0.0f);
            for (size_t i = 0; i < nx; ++i)
            {
                const size_t k = (i / (size_t) G) % ny, a = zy[k], b = std::min (y.size(), zy[k + 1]), L0 = zx[i + 1] - zx[i];
                if (b <= a) continue;
                const double r = (double) (b - a) / (double) L0;
                for (size_t s = 0; s < L0; ++s) out[zx[i] + s] = (float) lerpAt (y, (double) a + (double) s * r);
                if (! (i & 255)) cx.prog (((double) c + (double) i / (double) nx) / (double) ch.size());
            }
        }
        if (out.size() > cx.maxLen) out.resize (cx.maxLen);
        if (out.empty()) out.assign (16, 0.0f);
        res.push_back (std::move (out));
    }
    size_t L = 0; for (auto& b : res) L = std::max (L, b.size()); for (auto& b : res) b.resize (L);
    return res;
}

// SOUND SEQUENCER: plays the main and the second sound as notes on the tempo grid (PATTERN chooses which), each
// transposed by a MELODY within RANGE, cut to NOTE LEN with a SPLICE. after CDP do_sequence2 (extend sequence2, iterate.c)
Chans extSeq2 (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = stereoOf (getAux (cx, AUXMSG)); Chans src = stereoOf (ch);
    const double sr = cx.sr, step = divSec (p.i ("div"), cx.bpm); const int steps = std::max (2, p.i ("steps")), pat = p.i ("pattern"), mel = p.i ("melody");
    const long long total = (long long) std::max (1.0, std::round (p["bars"] * 4 * 60 / std::max (20.0, cx.bpm) / step));
    const size_t outLen = capLen (cx, ((double) total * step + 2) * sr);
    Chans out (2, Buf (outLen)); cx.seed (p["seed"]);
    static const int PENT[] = { 0, 3, 5, 7, 10 };
    auto snap = [] (double st) { long long o = (long long) std::floor (st / 12); int r = (int) jr (st - (double) o * 12); int best = 0; for (int q : PENT) if (std::abs (q - r) < std::abs (best - r)) best = q; if (std::abs (12 - r) < std::abs (best - r)) best = 12; return (double) (o * 12 + best); };
    std::vector<double> seqSt ((size_t) steps);
    for (int i = 0; i < steps; ++i)
    {
        double u = (double) i / (double) (steps - 1), v = 0;
        switch (mel) { case 1: v = u; break; case 2: v = 1 - u; break; case 3: v = 1 - std::abs (2 * u - 1); break; case 4: v = cx.rand(); break; default: v = 0; }
        seqSt[(size_t) i] = mel == 0 ? 0 : snap ((mel == 4 ? 2 * v - 1 : v) * p["range"]);
    }
    const long long spl = std::max (2LL, jr (p["splice"] / 1000 * sr));
    for (long long e = 0; e < total; ++e)
    {
        bool useAux = false; const long long i = e;
        switch (pat) { case 0: useAux = i % 2; break; case 1: useAux = (i / 2) % 2; break; case 2: useAux = (i % 4 == 1 || i % 4 == 2); break; case 3: useAux = i % 4 == 3; break; default: useAux = cx.rand() < 0.5; }
        const Chans& s = useAux ? aux : src; const double rate = std::pow (2.0, seqSt[(size_t) (e % steps)] / 12);
        const long long at = jr ((double) e * step * sr);
        const long long n = std::min (jr (p["notelen"] * step * sr), (long long) ((double) s[0].size() / rate));
        if (n < 4) continue;
        for (size_t c = 0; c < 2; ++c) addSeg (out[c], at, s[c], 0, n, spl, p["level"], false, rate);
        if (! (e & 7)) cx.prog ((double) e / (double) total);
    }
    return out;
}

// GRAIN ALIGN: the grains of the second sound are retimed onto the grain onsets of the main (its rhythm, their sound).
// after CDP grain align (process_grains GRAIN_ALIGN -> GRAIN_POSITION, grain.c)
Chans grainAlign (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size();
    auto g1 = findGrains (monoOf (ch), sr, p["gate"], p["hole"] / 1000), g2 = findGrains (monoOf (aux), sr, p["gate2"], p["hole"] / 1000);
    if (g1.empty()) g1.push_back ({ 0, len });
    if (g2.empty()) g2.push_back ({ 0, aux[0].size() });
    const long long off = jr (p["offset"] / 1000 * sr), spl = std::max (2LL, jr (p["splice"] / 1000 * sr));
    Chans src = ch.size() >= 2 || aux.size() >= 2 ? stereoOf (aux) : Chans { aux[0] };
    Chans out (src.size(), Buf (capLen (cx, (double) len + (double) std::max (0LL, off))));
    for (size_t i = 0; i < g1.size(); ++i)
    {
        const Grain& q = g2[i % g2.size()];
        const long long gap = (i + 1 < g1.size() ? (long long) g1[i + 1].a : (long long) len) - (long long) g1[i].a;
        const long long n = std::min ((long long) (q.b - q.a) + spl, std::max (4LL, gap));
        for (size_t c = 0; c < src.size(); ++c) addSeg (out[c], std::max (0LL, (long long) g1[i].a + off), src[c], (double) q.a, n, spl, 1, false);
        if (! (i & 15)) cx.prog ((double) i / (double) g1.size());
    }
    return out;
}

// ENVELOPE TRANSFER: IMPOSE multiplies the main by the second sound's loudness contour; REPLACE first divides out the
// main's own contour. after CDP impose_envel_on_sndfile / envreplace (envel impose 1, envel replace 1, envimpos.c)
Chans envImpose (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr, wsz = p["wsize"] / 1000 * sr; const size_t len = ch[0].size(); const int mode = p.i ("mode"), fit = p.i ("fit");
    const Env ea = envOf (monoOf (aux), wsz); double amx = 1e-9; for (double v : ea.v) amx = std::max (amx, v);
    const Buf mono = monoOf (ch); const Env em = envOf (mono, wsz); double mmx = 1e-9; for (double v : em.v) mmx = std::max (mmx, v);
    const size_t al = aux[0].size();
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        Buf o (len);
        for (size_t i = 0; i < len; ++i)
        {
            double ap = auxPos ((double) i, len, al, fit); if (fit == 1) ap = std::fmod (ap, (double) std::max ((size_t) 1, al));
            double e = ea.at (ap), g;
            if (mode == 0) g = e / amx;
            else g = std::min (100.0, e / std::max (em.at ((double) i), mmx * 0.003));
            g = 1 + (g - 1) * p["depth"];
            o[i] = (float) (ch[c][i] * g);
        }
        cx.prog ((c + 1.0) / (double) ch.size());
        res.push_back (std::move (o));
    }
    return res;
}

// LEVEL MATCH: scales the main so its peak (or RMS) equals the second sound's. after CDP modify loudness 5 (gain.c)
Chans loudMatch (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG);
    auto rms = [] (const Chans& c) { double s = 0, n = 0; for (auto& b : c) for (float v : b) { s += (double) v * v; n += 1; } return n > 0 ? std::sqrt (s / n) : 0.0; };
    const bool useRms = p.i ("measure") == 1;
    const double a = useRms ? rms (ch) : peakAll (ch), b = useRms ? rms (aux) : peakAll (aux);
    const double g = a > 1e-9 ? b / a * std::pow (10.0, p["trim"] / 20) : 0;
    Chans res = ch; for (auto& c : res) for (auto& v : c) v = (float) (v * g);
    cx.prog (1);
    return res;
}

// CROSS MODULATE: multiplies the two sounds sample by sample (ring modulation of one sound by the other).
// after CDP cross_modulate (modify radical 6, radical.c)
Chans crossMod (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), al = aux[0].size(); const int fit = p.i ("fit");
    const double pk = std::max (1e-9, peakAll (aux));
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& y = aux[c % aux.size()]; Buf o (len);
        for (size_t i = 0; i < len; ++i)
        {
            double m = sampAt (y, auxPos ((double) i, len, al, fit), fit == 1) / pk;
            o[i] = (float) (ch[c][i] * (1 - p["mix"] + p["mix"] * m));
        }
        res.push_back (std::move (o));
    }
    cx.prog (1);
    return res;
}

// SAUSAGE: brassage of both sounds scrambled together: grains (GRAIN size, DENSITY overlap) taken from the main or
// the second sound (AUX SHARE), advancing at VELOCITY, with random PITCH, AMP, SPACE and SCATTER.
// after CDP granula_process (modify sausage, granula1.c)
Chans sausage (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size();
    const Buf m0 = monoOf (ch), m1 = monoOf (aux);
    const long long G = std::max (16LL, jr (p["grain"] / 1000 * sr)), hop = std::max (4LL, jr ((double) G / std::max (0.05, p["density"])));
    const size_t outLen = capLen (cx, (double) len * p["length"]);
    Chans out (2, Buf (outLen)); cx.seed (p["seed"]);
    const long long spl = std::max (2LL, G / 4);
    for (long long t = 0; t < (long long) outLen; t += hop)
    {
        const bool useAux = cx.rand() < p["share"]; const Buf& s = useAux ? m1 : m0;
        const double rate = std::pow (2.0, (cx.rand() * 2 - 1) * p["pitch"] / 12), amp = 1 - p["amp"] * cx.rand();
        const double pan = 0.5 + (cx.rand() - 0.5) * p["space"]; auto [gl, gr] = panG (pan);
        const long long at = t + jr ((cx.rand() - 0.5) * p["scatter"] * (double) hop);
        const double from = (double) t * p["velocity"] + cx.rand() * p["scatter"] * (double) G;
        addSeg (out[0], at, s, from, G, spl, amp * gl, true, rate);
        addSeg (out[1], at, s, from, G, spl, amp * gr, true, rate);
        if (! (t & 4095)) cx.prog ((double) t / (double) outLen);
    }
    return out;
}

// CONVOLVE: the main convolved with the second sound as impulse response (NORMAL), or with an IR whose pitch glides
// from T START to T END over the length (TRANSPOSING). MIX is fastconv's dry/wet balance.
// after CDP do_convolve (modify convolve 1-2, delay.c) and fastconv (fconv.cpp)
Chans convolveX (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, "Choose an IMPULSE sample"); const double sr = cx.sr; const size_t len = ch[0].size(); const int mode = p.i ("mode");
    auto np2 = [] (size_t n) { size_t q = 1; while (q < n) q <<= 1; return q; };
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; const double p0 = (double) c / (double) ch.size(), p1 = (c + 1.0) / (double) ch.size();
        Buf h = aux[c % aux.size()]; Buf wet;
        if (mode == 0)
        {
            h.resize (std::min (h.size(), (size_t) (12 * sr)));
            wet = convolve (x, h, &cx, p0, p1);
        }
        else
        {
            h.resize (std::min ({ h.size(), (size_t) (4 * sr), (size_t) 131072 }));
            const size_t Lh = h.size(), B = clamp (Lh / 4, (size_t) 2048, (size_t) 32768), maxH = 262144;
            const double rmin = std::pow (2.0, std::min (p["t0"], p["t1"]) / 12);
            const size_t longest = std::min (maxH, (size_t) std::ceil ((double) Lh / rmin) + 1);
            DVec acc (len + longest + 1, 0.0);
            for (size_t a = 0; a < len; a += B)
            {
                const double u = ((double) a + (double) B / 2) / (double) len, r = std::pow (2.0, (p["t0"] + (p["t1"] - p["t0"]) * clampd (u, 0, 1)) / 12);
                Buf hr = resamp (h, std::max (1LL, std::min ((long long) longest, jr ((double) Lh / r))));
                const size_t M = np2 (B + hr.size());
                DVec ar (M), ai (M), br (M), bi (M);
                for (size_t i = 0; i < B && a + i < len; ++i) ar[i] = x[a + i];
                for (size_t i = 0; i < hr.size(); ++i) br[i] = hr[i] * std::sqrt (r);   // keep the IR's energy roughly constant
                fft (ar, ai, false); fft (br, bi, false);
                for (size_t i = 0; i < M; ++i) { double q = ar[i] * br[i] - ai[i] * bi[i]; ai[i] = ar[i] * bi[i] + ai[i] * br[i]; ar[i] = q; }
                fft (ar, ai, true);
                for (size_t i = 0; i < M && a + i < acc.size(); ++i) acc[a + i] += ar[i];
                cx.prog (p0 + (p1 - p0) * (double) a / (double) len);
            }
            wet.resize (acc.size()); for (size_t i = 0; i < acc.size(); ++i) wet[i] = (float) acc[i];
        }
        const double g = peak (x) / std::max (1e-9, peak (wet)), mx = p["mix"];
        const double gw = std::sin (mx * PI / 2), gd = std::cos (mx * PI / 2);
        for (size_t i = 0; i < wet.size(); ++i) wet[i] = (float) (wet[i] * g * gw + (i < x.size() ? x[i] : 0) * gd);
        if (wet.size() > cx.maxLen) wet.resize (cx.maxLen);
        res.push_back (std::move (wet));
    }
    size_t L = 0; for (auto& b : res) L = std::max (L, b.size()); for (auto& b : res) b.resize (L);
    return res;
}

// file order for twixt / sphinx: with WEIGHT 1 and cyclic order the two sounds alternate; otherwise permutations
// of (2 + WEIGHT - 1) slots where the extra slots belong to the main (CDP Getnextfile)
struct FileOrder
{
    std::vector<int> perm; int permcnt = 2; bool rnd = false, first = true;
    int next (int n, Ctx& cx)
    {
        if (permcnt == 2 && ! rnd) return n % 2;
        const int m = n % permcnt;
        if (m == 0)
        {
            const int last = perm.empty() ? -1 : perm.back();
            perm.resize ((size_t) permcnt);
            for (int i = 0; i < permcnt; ++i) perm[(size_t) i] = i;
            for (int i = permcnt - 1; i > 0; --i) std::swap (perm[(size_t) i], perm[(size_t) std::min (i, (int) (cx.rand() * (i + 1)))]);
            for (auto& q : perm) if (q >= 2) q = 0;
            if (first) { for (size_t j = 1; j < perm.size(); ++j) if (perm[j] == 0 && perm[0] != 0) std::swap (perm[0], perm[j]); first = false; }
            else if (perm[0] == last && (last != 0 || permcnt == 2)) { for (size_t j = 1; j < perm.size(); ++j) if (perm[j] != last) { std::swap (perm[0], perm[j]); break; } }
        }
        return perm[(size_t) m];
    }
};

// SOUND SWITCH (twixt): both sounds run in parallel and the output switches between them on the tempo grid (EVERY):
// IN SEQUENCE, segments PERMUTED, or RANDOM segments. SPHINX: each sound is cut at its own onsets, the Nth onset of one
// matching the Nth of the other. after CDP do_twixt (sfedit twixt 1-3 / sfedit sphinx 1-3, twixt.c)
Chans switcher (const Chans& ch, Ctx& cx, const Params& p, bool sph)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size(), al = aux[0].size(); const int mode = p.i ("mode");
    const size_t nch = std::max (ch.size(), aux.size());
    std::vector<std::vector<size_t>> T (2);
    if (! sph)
    {
        const size_t step = (size_t) std::max (16.0, divSec (p.i ("div"), cx.bpm) * sr);
        for (size_t t = 0; t < len; t += step) { T[0].push_back (t); T[1].push_back (t); }
        T[0].push_back (len); T[1].push_back (len);
    }
    else
    {
        auto g0 = findGrains (monoOf (ch), sr, p["gate"], p["hole"] / 1000), g1 = findGrains (monoOf (aux), sr, p["gate"], p["hole"] / 1000);
        const size_t K = std::min (g0.size(), g1.size());
        if (K < 2)
            for (size_t i = 0; i <= 8; ++i) { T[0].push_back (len * i / 8); T[1].push_back (al * i / 8); }
        else
        {
            for (size_t i = 0; i < K; ++i) { T[0].push_back (i == 0 ? 0 : g0[i].a); T[1].push_back (i == 0 ? 0 : g1[i].a); }
            T[0].push_back (len); T[1].push_back (al);
        }
    }
    const int span = std::max (1, (int) T[0].size() - 1);
    const int limit = mode == 0 ? span : std::max (1, p.i ("segs"));
    FileOrder fo; fo.permcnt = 2 + std::max (1, p.i ("weight")) - 1; fo.rnd = p.i ("order") == 1;
    cx.seed (p["seed"]);
    std::vector<int> sp; int lastSeg = -1;
    const long long spl = std::max (2LL, jr (p["splice"] / 1000 * sr));
    Chans out (nch); size_t pos = 0;
    for (int n = 0; n < limit && pos < cx.maxLen; ++n)
    {
        const int f = fo.next (n, cx); int k = 0;
        if (mode == 0) k = n;
        else if (mode == 1)
        {
            if (n % span == 0)
            {
                sp.resize ((size_t) span); for (int i = 0; i < span; ++i) sp[(size_t) i] = i;
                for (int i = span - 1; i > 0; --i) std::swap (sp[(size_t) i], sp[(size_t) std::min (i, (int) (cx.rand() * (i + 1)))]);
                if (span > 1 && sp[0] == lastSeg) std::swap (sp[0], sp[1]);
            }
            k = sp[(size_t) (n % span)];
        }
        else k = std::min (span - 1, (int) (cx.rand() * span));
        lastSeg = k;
        const auto& tt = T[(size_t) f];
        if ((size_t) k + 1 >= tt.size()) continue;
        const long long a = (long long) tt[(size_t) k], b = (long long) tt[(size_t) k + 1];
        const long long sl = std::max (4LL, std::min (b - a, (long long) (cx.maxLen - pos)));
        for (size_t c = 0; c < nch; ++c)
        {
            out[c].resize (pos + (size_t) sl, 0.0f);
            const Buf& s = f == 0 ? chOr (ch, c) : chOr (aux, c);
            addSeg (out[c], (long long) pos, s, (double) a, sl, spl, 1, f == 1 && ! sph);
        }
        pos += (size_t) sl;
        if (! (n & 7)) cx.prog ((double) n / (double) limit);
    }
    for (auto& b : out) if (b.size() < 16) b.resize (16, 0.0f);
    return out;
}
Chans twixt (const Chans& ch, Ctx& cx, const Params& p) { return switcher (ch, cx, p, false); }
Chans sphinx (const Chans& ch, Ctx& cx, const Params& p) { return switcher (ch, cx, p, true); }

// BALANCE MIX: mixes the two sounds with a balance that moves from START to END, plus an optional LFO.
// after CDP cross_stitch (submix balance, mixmerge.c)
Chans balanceMix (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), al = aux[0].size(), nch = std::max (ch.size(), aux.size()); const int fit = p.i ("fit");
    Chans out (nch, Buf (len));
    for (size_t c = 0; c < nch; ++c)
    {
        const Buf& x = chOr (ch, c); const Buf& y = chOr (aux, c);
        for (size_t i = 0; i < len; ++i)
        {
            const double u = (double) i / (double) len, t = (double) i / cx.sr;
            const double b = clampd (p["start"] + (p["end"] - p["start"]) * u + p["depth"] * 0.5 * std::sin (TAU * p["rate"] * t), 0, 1);
            out[c][i] = (float) (x[i] * (1 - b) + sampAt (y, auxPos ((double) i, len, al, fit), fit == 1) * b);
        }
    }
    cx.prog (1);
    return out;
}

// CROSSFADE: the main fades into the second sound (which starts STAGGER later) between BEGIN and END, LINEAR or
// COSINE with a SKEW; the second sound then plays on. after CDP mix_cross (submix crossfade 1-2, mixmerge.c)
Chans crossfade (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size(), al = aux[0].size(), nch = std::max (ch.size(), aux.size());
    const long long stag = jr (p["stagger"] * sr);
    const double b0 = p["begin"] * (double) len, b1 = std::max (b0 + 1, p["end"] * (double) len);
    const size_t outLen = capLen (cx, std::max ((double) len, (double) stag + (double) al));
    Chans out (nch, Buf (outLen));
    for (size_t c = 0; c < nch; ++c)
    {
        const Buf& x = chOr (ch, c); const Buf& y = chOr (aux, c);
        for (size_t i = 0; i < outLen; ++i)
        {
            double u = clampd (((double) i - b0) / (b1 - b0), 0, 1);
            if (p.i ("mode") == 1) u = 0.5 - 0.5 * std::cos (PI * std::pow (u, p["skew"]));
            else u = std::pow (u, p["skew"]);
            const double xv = i < len ? x[i] : 0.0; const long long j = (long long) i - stag;
            const double yv = j >= 0 && j < (long long) al ? y[(size_t) j] : 0.0;
            out[c][i] = (float) (xv * (1 - u) + yv * u);
        }
    }
    cx.prog (1);
    return out;
}

// WAVESET MORPH: zero-crossing-pegged interpolation of the two sounds, cycle by cycle: each output cycle has a length
// and shape between the main's and the aux's, the weight moving from START to END. after CDP cyclesync_inbetweening
// (submix inbetween2, inbetwee.c)
Chans inbetween2 (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t minLen = (size_t) std::max (2.0, cx.sr / std::max (20.0, p["cutoff"]));
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; const Buf& y = aux[c % aux.size()];
        auto zx = upCross (x, minLen), zy = upCross (y, minLen);
        const size_t nx = zx.size() - 1, ny = std::max ((size_t) 1, zy.size() - 1);
        Buf out; out.reserve (x.size() * 2);
        for (size_t n = 0; n < nx && out.size() < std::min (cx.maxLen, 3 * x.size() + 4096); ++n)
        {
            const double r = p["start"] + (p["end"] - p["start"]) * (double) zx[n] / (double) std::max ((size_t) 1, x.size());
            const size_t k = n % ny, a0 = zx[n], l0 = zx[n + 1] - zx[n];
            const size_t a1 = std::min (y.size(), zy[k]), l1 = std::max ((size_t) 1, std::min (y.size(), zy[std::min (k + 1, zy.size() - 1)]) - a1);
            const size_t L = (size_t) clamp (jr ((double) l0 + ((double) l1 - (double) l0) * r), 1LL, (long long) cx.maxLen);
            for (size_t i = 0; i < L; ++i)
            {
                const double i0 = (double) i * (double) l0 / (double) L, i1 = (double) i * (double) l1 / (double) L;
                const double v0 = lerpAt (x, (double) a0 + std::min (i0, (double) l0 - 1)), v1 = lerpAt (y, (double) a1 + std::min (i1, (double) l1 - 1));
                out.push_back ((float) (v0 * (1 - r) + v1 * r));
            }
            if (! (n & 255)) cx.prog (((double) c + (double) n / (double) nx) / (double) ch.size());
        }
        if (out.size() > cx.maxLen) out.resize (cx.maxLen);
        if (out.size() < 16) out.resize (16, 0.0f);
        res.push_back (std::move (out));
    }
    size_t L = 0; for (auto& b : res) L = std::max (L, b.size()); for (auto& b : res) b.resize (L);
    return res;
}

// SUBTRACT: subtracts the (mono) second sound, delayed by DELAY and scaled by GAIN, from the chosen channels: null tests,
// phase cancellation, difference signals. after CDP subtract (subtract.c)
Chans subtractX (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const Buf y = monoOf (aux); const size_t len = ch[0].size(); const int which = p.i ("chan"), fit = p.i ("fit");
    const double d = p["delay"] / 1000 * cx.sr;
    Chans res = ch;
    for (size_t c = 0; c < res.size(); ++c)
    {
        if (which == 1 && c != 0) continue;
        if (which == 2 && c != 1 && res.size() > 1) continue;
        for (size_t i = 0; i < len; ++i)
        {
            const double q = (double) i - d; if (q < 0) continue;
            res[c][i] = (float) (res[c][i] - p["gain"] * sampAt (y, auxPos (q, len, y.size(), fit), fit == 1));
        }
    }
    cx.prog (1);
    return res;
}

// ===================================================================================================
// spectral envelopes, peaks, partials (Release 7/8 two-input programs)
// ===================================================================================================

// band envelope: mean amplitude in bands WIDTH bins wide (or WIDTH octaves wide when pitchwise), read linearly between centres
void bandEnv (const DVec& amp, double width, bool pitchwise, DVec& out)
{
    const size_t n = amp.size(); out.assign (n, 0.0);
    std::vector<double> e { 0 };
    if (! pitchwise) { const double st = std::max (1.0, width); for (double b = st; b < (double) n; b += st) e.push_back (b); }
    else { double b = 1; const double r = std::pow (2.0, clampd (width, 0.02, 4)); while (b < (double) n) { e.push_back (b); b = std::max (b + 1, b * r); } }
    e.push_back ((double) n);
    std::vector<double> c, val;
    for (size_t i = 0; i + 1 < e.size(); ++i)
    {
        size_t a = (size_t) e[i], b = std::max (a + 1, (size_t) e[i + 1]); double s = 0;
        for (size_t k = a; k < b && k < n; ++k) s += amp[k];
        c.push_back ((e[i] + e[i + 1]) / 2); val.push_back (s / (double) (b - a));
    }
    size_t j = 0;
    for (size_t k = 0; k < n; ++k)
    {
        const double x = (double) k;
        while (j + 1 < c.size() && c[j + 1] < x) ++j;
        if (x <= c.front()) out[k] = val.front();
        else if (j + 1 >= c.size()) out[k] = val.back();
        else { double u = (x - c[j]) / std::max (1e-9, c[j + 1] - c[j]); out[k] = val[j] + (val[j + 1] - val[j]) * clampd (u, 0, 1); }
    }
}

// the P strongest local maxima of a spectrum, returned in ascending order (CDP newmorph peak lists)
std::vector<size_t> topPeaks (const DVec& m, size_t P)
{
    std::vector<size_t> pk;
    for (size_t k = 1; k + 1 < m.size(); ++k) if (m[k] > m[k - 1] && m[k] >= m[k + 1] && m[k] > 0) pk.push_back (k);
    const size_t n = std::min (P, pk.size());
    std::partial_sort (pk.begin(), pk.begin() + (long) n, pk.end(), [&] (size_t a, size_t b) { return m[a] > m[b]; });
    pk.resize (n); std::sort (pk.begin(), pk.end());
    return pk;
}
// average magnitude spectrum of a whole sound (mono), for the averaged-peak modes
DVec avgSpectrum (const Buf& x, size_t N, Ctx& cx)
{
    const size_t half = N >> 1; const DVec w = hann (N); DVec re (N), im (N), acc (half + 1, 0.0);
    const size_t hop = std::max (N >> 1, x.size() / 400 + 1);
    for (size_t pos = 0; pos < x.size() || pos == 0; pos += hop)
    {
        analyze (x, (long long) pos - (long long) half, w, re, im);
        for (size_t k = 0; k <= half; ++k) acc[k] += std::hypot (re[k], im[k]);
        if (pos >= x.size()) break;
        cx.prog (0);
    }
    return acc;
}
// parabolic refinement of a peak bin
inline double refine (const DVec& m, size_t k)
{
    if (k == 0 || k + 1 >= m.size()) return (double) k;
    const double a = m[k - 1], b = m[k], c = m[k + 1], d = a - 2 * b + c;
    return std::abs (d) > 1e-12 ? (double) k + clampd (0.5 * (a - c) / d, -0.5, 0.5) : (double) k;
}

// PARTIAL MORPH: the main's partials (on its tracked pitch) move towards the harmonic profile of the second sound;
// non-partial energy fades out. TUNING sets how close a channel must be to a harmonic, INTERP the amount (START to END).
// after CDP specross partials (spectral_interp_on_pitch / spec_interp, specross.c)
Chans specross (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size(), al = aux[0].size(); const int fit = p.i ("fit");
    const auto pm = trackPitch (monoOf (ch), sr, 50, 1500, 0.15, &cx), pa = trackPitch (monoOf (aux), sr, 50, 1500, 0.15, &cx);
    const double tune = std::pow (2.0, p["tuning"] / 12), g = std::pow (10.0, p["level"] / 20);
    const size_t N = pow2 (p["win"] * sr);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), fit, true, [&] (Spec& A, Spec& B, XSt& st, long long, double t)
    {
        const size_t half = st.half; const double ip = clampd (p["interp"] + (p["interp2"] - p["interp"]) * clampd (t, 0, 1), 0, 1);
        const double f0 = pm.at (t * (double) len) / st.binHz;
        if (f0 <= 0.5) { for (auto& v : A.amp) v *= g; return; }   // unpitched: left as it is
        double apos = auxPos (t * (double) len, len, al, fit); if (fit == 1 && al > 0) apos = std::fmod (apos, (double) al);
        const double af0 = pa.at (apos) / st.binHz;
        const int H = (int) std::min (400.0, std::floor ((double) half / f0));
        if (! st.init) { st.init = true; st.a.assign (half + 1, 0); }
        envelope (B.amp, std::max (1.0, f0 / 2), st.a);
        DVec mh ((size_t) H + 1, 0.0), ah ((size_t) H + 1, 0.0);
        auto peakNear = [&] (const DVec& m, double c) { double best = 0; long long lo = jr (c / tune), hi = jr (c * tune); for (long long k = std::max (1LL, lo); k <= std::min ((long long) half, hi); ++k) best = std::max (best, m[(size_t) k]); return best; };
        double mmx = 0, amx = 0;
        for (int h = 1; h <= H; ++h)
        {
            mh[(size_t) h] = peakNear (A.amp, f0 * h); mmx = std::max (mmx, mh[(size_t) h]);
            ah[(size_t) h] = af0 > 0.5 && af0 * h <= (double) half ? peakNear (B.amp, af0 * h) : envAt (st.a, f0 * h); amx = std::max (amx, ah[(size_t) h]);
        }
        const double norm = amx > 1e-12 ? mmx / amx : 0;
        for (size_t k = 1; k <= half; ++k)
        {
            const double fr = std::abs (A.frq[k]); const int h = (int) jr (fr / f0);
            const bool partial = h >= 1 && h <= H && std::max (fr, f0 * h) / std::max (1e-9, std::min (fr, f0 * h)) < tune && mh[(size_t) h] > p["thresh"] * mmx;
            if (partial) A.amp[k] *= 1 + (std::min (100.0, ah[(size_t) h] * norm / std::max (1e-12, mh[(size_t) h])) - 1) * ip;
            else A.amp[k] *= 1 - ip;
            A.amp[k] *= g;
        }
        A.amp[0] *= 1 - ip;
    });
}

// SPECTRAL ENVELOPE: the second sound's band envelope (BAND channels wide, or octave steps) REPLACEs or is IMPOSEd on the
// main's; level follows the main (or the second sound), BALANCE mixes back either original.
// after CDP do_specenv (specenv specenv, specenv.c)
Chans specenvX (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const bool pitchwise = p.i ("scale") == 1, impose = p.i ("mode") == 1, keepAux = p.i ("level") == 1;
    const double bal = p["bal"];
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), true, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    {
        const size_t half = st.half;
        bandEnv (A.amp, pitchwise ? p["band"] / 12 : p["band"], pitchwise, st.a); bandEnv (B.amp, pitchwise ? p["band"] / 12 : p["band"], pitchwise, st.b);
        double mi = 0, mi2 = 0, mo = 0, emx = 1e-12; for (double v : st.b) emx = std::max (emx, v);
        const DVec orig = A.amp;
        for (size_t k = 0; k <= half; ++k)
        {
            const double fb = std::abs (A.frq[k]), a2 = envAt (st.b, fb);
            if (impose) A.amp[k] = orig[k] * a2 / emx;
            else { const double a1 = envAt (st.a, fb); A.amp[k] = a1 > 1e-9 ? orig[k] * a2 / a1 : orig[k]; }
            mi = std::max (mi, orig[k]); mi2 = std::max (mi2, B.amp[k]); mo = std::max (mo, A.amp[k]);
        }
        const double target = keepAux ? mi2 : mi;
        if (mi2 < 1e-9 && mi > 1e-9 && ! keepAux) A.amp = orig;
        else if (mo > 1e-12) for (auto& v : A.amp) v *= target / mo;
        if (bal > 0) for (size_t k = 0; k <= half; ++k) A.amp[k] = A.amp[k] * (1 - bal) + orig[k] * bal;
        else if (bal < 0) for (size_t k = 0; k <= half; ++k) A.amp[k] = A.amp[k] * (1 + bal) - B.amp[k] * bal;
    });
}

// PEAK MORPH: morph between dissimilar spectra by moving their PEAKS strongest peaks into each other (averaged over the whole
// sounds, or found window by window), interpolating everything else; or TUNE the main to the second sound's peak field.
// after CDP newmorph_core (newmorph newmorph 1-6, newmorph.c)
Chans newmorph (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size(), N = pow2 (p["win"] * sr), half = N >> 1;
    const int mode = p.i ("mode"), what = p.i ("what"); const bool cosine = mode % 2 == 1, avg = mode < 2, tune = mode >= 4, keep = p.i ("keep") == 1;
    const size_t P = (size_t) std::max (1, p.i ("peaks"));
    std::vector<size_t> pk1, pk2; DVec goal;
    if (avg || tune)
    {
        const DVec s1 = avgSpectrum (monoOf (ch), N, cx), s2 = avgSpectrum (monoOf (aux), N, cx);
        pk1 = topPeaks (s1, P); pk2 = topPeaks (s2, P);
        for (size_t k : pk2) goal.push_back (refine (s2, k));
        const size_t n = std::min (pk1.size(), pk2.size()); pk1.resize (n); pk2.resize (n);
    }
    const double t0 = p["start"], t1 = std::max (p["start"] + 1e-3, p["end"]);
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), false, [&] (Spec& A, Spec& B, XSt&, long long, double t)
    {
        double r = clampd ((t - t0) / (t1 - t0), 0, 1); r = cosine ? warpCos (r, p["exp"]) : std::pow (r, p["exp"]);
        if (tune)
        {
            if (goal.empty()) return;
            for (size_t k = 0; k <= half; ++k)
            {
                const double f = A.frq[k]; double best = goal[0];
                for (double gq : goal) if (std::abs (gq - f) < std::abs (best - f)) best = gq;
                A.frq[k] = f + (best - f) * r;
            }
            return;
        }
        std::vector<size_t> q1 = pk1, q2 = pk2;
        if (! avg) { q1 = topPeaks (A.amp, P); q2 = topPeaks (B.amp, P); const size_t n = std::min (q1.size(), q2.size()); q1.resize (n); q2.resize (n); }
        const double before = sumAmp (A.amp);
        struct NP { size_t c; double a, f; }; std::vector<NP> np;
        for (size_t n = 0; n < q1.size(); ++n)
        {
            const double a0 = A.amp[q1[n]], f0 = A.frq[q1[n]], a1 = B.amp[q2[n]], f1 = B.frq[q2[n]];
            const double f = f0 + (f1 - f0) * r; const long long c = jr (f);
            if (c >= 0 && c <= (long long) half) np.push_back ({ (size_t) c, what == 2 ? a0 : a0 + (a1 - a0) * r, f });
        }
        const DVec a0v = A.amp;
        for (size_t k = 0; k <= half; ++k)
        {
            double am0 = a0v[k], am1 = B.amp[k]; bool done = false;
            for (size_t n = 0; n < q1.size(); ++n)
            {
                if (k == q1[n] && k != q2[n]) { am0 = ((k > 0 ? a0v[k - 1] : 0) + (k < half ? a0v[k + 1] : 0)) / 2; done = true; break; }
                if (k == q2[n] && k != q1[n]) { am1 = ((k > 0 ? B.amp[k - 1] : 0) + (k < half ? B.amp[k + 1] : 0)) / 2; done = true; break; }
            }
            if (what == 1 && ! done) continue;   // PEAKS ONLY: other channels keep the main's values
            A.amp[k] = am0 + (am1 - am0) * r;
            A.frq[k] = A.frq[k] + (B.frq[k] - A.frq[k]) * r;
        }
        for (auto& q : np) { A.amp[q.c] = q.a; A.frq[q.c] = q.f; }
        if (keep) { const double after = sumAmp (A.amp); if (after > 1e-12) for (auto& v : A.amp) v *= before / after; }
    });
}

// PEAK TUNE: the main is gradually tuned (START..END, curve EXP, LINEAR or COSINE) to a field of goal frequencies: the
// second sound's PEAKS strongest peaks, or a chord on ROOT repeated over the octaves; RANDOM detunes the goals.
// after CDP newmorph2 2-3 (newmorph.c), with the peaks file from newmorph2 1 on the aux
Chans newmorph2 (const Chans& ch, Ctx& cx, const Params& p)
{
    static const std::vector<std::vector<int>> CH { {}, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 7 }, { 0, 2, 4, 6, 8, 10 }, {} };
    const double sr = cx.sr; const size_t len = ch[0].size(), N = pow2 (p["win"] * sr), half = N >> 1; const double binHz = sr / (double) N;
    const int src = p.i ("source"); std::vector<double> goal;
    Chans aux;
    if (src == 0)
    {
        aux = getAux (cx, AUXMSG);
        const DVec s2 = avgSpectrum (monoOf (aux), N, cx);
        for (size_t k : topPeaks (s2, (size_t) std::max (1, p.i ("peaks")))) goal.push_back (refine (s2, k));
    }
    else
    {
        aux = { Buf (16, 0.0f) };
        const double root = 440.0 * std::pow (2.0, (p["root"] - 69) / 12);
        if (src == 5) { for (int h = 1; root * h < sr / 2 && h < 200; ++h) goal.push_back (root * h / binHz); }
        else for (double o = root / 8; o < sr / 2; o *= 2) for (int iv : CH[(size_t) src]) { double f = o * std::pow (2.0, iv / 12.0); if (f > 20 && f < sr / 2) goal.push_back (f / binHz); }
        std::sort (goal.begin(), goal.end());
    }
    if (goal.empty()) goal.push_back (440 / binHz);
    cx.seed (p["seed"]);
    const double t0 = p["start"], t1 = std::max (p["start"] + 1e-3, p["end"]); const bool cosine = p.i ("mode") == 1;
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), 1, false, [&] (Spec& A, Spec&, XSt&, long long, double t)
    {
        double r = clampd ((t - t0) / (t1 - t0), 0, 1); r = cosine ? warpCos (r, p["exp"]) : std::pow (r, p["exp"]);
        for (size_t k = 0; k <= half; ++k)
        {
            const double f = A.frq[k];
            size_t j = (size_t) (std::lower_bound (goal.begin(), goal.end(), f) - goal.begin());
            if (j >= goal.size()) j = goal.size() - 1;
            if (j > 0 && std::abs (goal[j - 1] - f) < std::abs (goal[j] - f)) --j;
            double gq = goal[j];
            if (p["rand"] > 0)
            {
                const double up = j + 1 < goal.size() ? goal[j + 1] - goal[j] : goal[j] * 0.06, dn = j > 0 ? goal[j] - goal[j - 1] : goal[j] * 0.06;
                const double rr = (cx.rand() * 2 - 1) * 0.5; gq += rr >= 0 ? rr * p["rand"] * up : rr * p["rand"] * dn;
            }
            A.frq[k] = f + (gq - f) * r;
        }
    });
}

// SPECTRAL SPHINX: IMPOSE gives the main's frequencies the second sound's channel amplitudes (AMP BAL keeps some of the
// main's, FRQ BAL injects the aux's most prominent frequencies); MULTIPLY multiplies the two spectra (BIAS adds back an
// original, GAIN). after CDP spec_sphinx / specvolve (specsphinx 1-2, specsphinx.c)
Chans specsphinx (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const int mode = p.i ("mode");
    const double g = std::pow (10.0, p["gain"] / 20), bias = p["bias"], ref = (double) N / 4;
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), mode == 1, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    {
        const size_t half = st.half;
        if (mode == 1)
        {
            for (size_t k = 0; k <= half; ++k)
            {
                double v = A.amp[k] * B.amp[k] / ref * g;
                if (bias > 0) v = v * (1 - bias) + B.amp[k] * bias;
                else if (bias < 0) v = v * (1 + bias) - A.amp[k] * bias;
                A.amp[k] = v;
            }
            return;
        }
        const DVec a1 = A.amp;
        for (size_t k = 0; k <= half; ++k) A.amp[k] = (1 - p["ampbal"]) * B.amp[k] + p["ampbal"] * a1[k];
        if (p["frqbal"] <= 0) return;
        std::vector<size_t> order (half + 1); for (size_t k = 0; k <= half; ++k) order[k] = k;
        std::sort (order.begin(), order.end(), [&] (size_t x, size_t y) { return B.amp[x] > B.amp[y]; });
        std::vector<uint8_t> used (half + 1, 0); std::vector<std::vector<size_t>> sets;
        for (size_t k : order)
        {
            if (used[k] || B.amp[k] <= 0) continue;
            std::vector<size_t> set { k }; used[k] = 1; const double fh = std::abs (B.frq[k]);
            auto same = [&] (size_t j) { double a = std::abs (B.frq[j]); return a > 0 && fh > 0 && std::max (a, fh) / std::min (a, fh) < 8.0 / 7.0; };
            for (size_t j = k; j-- > 1 && set.size() < 5 && same (j);) { set.push_back (j); used[j] = 1; }
            for (size_t j = k + 1; j <= half && set.size() < 5 && same (j); ++j) { set.push_back (j); used[j] = 1; }
            sets.push_back (set);
        }
        const size_t take = (size_t) jr (p["frqbal"] * (double) sets.size());
        for (size_t s = 0; s < take && s < sets.size(); ++s) for (size_t j : sets[s]) A.frq[j] = B.frq[j];
    });
}

// SPECTRAL TWIN: amplitudes from the two sounds' formant (smoothed) or total envelopes, mixed by ENV INTERP; frequencies
// mixed by FRQ INTERP; optionally the main is first DUPLICATEd at rising pitches (STEP) with decaying level (DECAY).
// after CDP spectwin (spectwin 1-4, spectwin.c)
Chans spectwin (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const size_t len = ch[0].size(), N = pow2 (p["win"] * cx.sr); const int mode = p.i ("mode"), dupl = p.i ("dupl");
    const double fi = p["frqint"], ei = p["envint"];
    return xRun (ch, aux, cx, p["win"], capLen (cx, (double) (len + N)), p.i ("fit"), false, [&] (Spec& A, Spec& B, XSt& st, long long, double)
    {
        const size_t half = st.half;
        if (dupl > 0)
        {
            Spec acc = A;
            for (size_t k = 0; k <= half; ++k)
            {
                double roll = p["dec"];
                for (int n = 1; n <= dupl; ++n)
                {
                    const double f = A.frq[k] * std::pow (2.0, p["step"] * n / 12); const size_t c = (size_t) std::max (0.0, std::floor (f + 0.5));
                    if (c > half) break;
                    if (A.amp[k] * roll > acc.amp[c]) { acc.amp[c] = A.amp[k] * roll; acc.frq[c] = f; }
                    roll *= p["dec"];
                }
            }
            A = acc;
        }
        const bool f1 = mode == 0 || mode == 1, f2 = mode == 0 || mode == 2;
        if (f1) bandEnv (A.amp, p["band"] / 12, true, st.a);
        if (f2) bandEnv (B.amp, p["band"] / 12, true, st.b);
        for (size_t k = 0; k <= half; ++k)
        {
            const double a0 = f1 ? envAt (st.a, std::abs (A.frq[k])) : A.amp[k], a1 = f2 ? envAt (st.b, std::abs (B.frq[k])) : B.amp[k];
            A.amp[k] = a0 * (1 - ei) + a1 * ei;
            A.frq[k] = A.frq[k] * (1 - fi) + B.frq[k] * fi;
        }
    });
}

// ===================================================================================================
// pitch-synchronous grains (FOFs), syllables, packets, spinning images
// ===================================================================================================

// pitch-synchronous cut points (one per period, at upward zero crossings) of a mono sound; unvoiced stretches use DEFAULT
// period lengths. After CDP psow's cut finding (find_min_energy_downward_zero_crossing_point, psow.c)
std::vector<size_t> fofCuts (const Buf& x, double sr, Ctx& cx)
{
    const auto pt = trackPitch (x, sr, 50, 1500, 0.15, &cx);
    std::vector<size_t> c { 0 }; const size_t n = x.size();
    size_t pos = 0;
    while (pos < n)
    {
        double f0 = pt.at ((double) pos); const double T = f0 > 0 ? sr / f0 : sr * 0.01;
        const long long target = (long long) pos + jr (T), lo = target - jr (T / 4), hi = target + jr (T / 4);
        long long best = -1; double bd = 1e300;
        for (long long i = std::max ((long long) pos + 8, lo); i <= hi && i < (long long) n; ++i)
            if (i > 0 && x[(size_t) i - 1] < 0 && x[(size_t) i] >= 0) { double d = std::abs ((double) (i - target)); if (d < bd) { bd = d; best = i; } }
        size_t next = best > 0 ? (size_t) best : (size_t) std::max ((long long) pos + 16, target);
        if (next >= n) break;
        c.push_back (next); pos = next;
    }
    c.push_back (n);
    return c;
}

// FOF IMPOSE: the pitch-pulse envelopes of the main's vocal FOFs (one per period) are imposed on the second sound, which is
// first flattened by its own envelope (WINDOW, below GATE = silence). DEPTH = how much of the FOF shape is applied.
// after CDP superimpose_fofs_on_input / create_normalising_envelope (psow impose, psow.c)
Chans psowImpose (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size(), al = aux[0].size(); const int fit = p.i ("fit");
    const Buf mono = monoOf (ch); const auto cuts = fofCuts (mono, sr, cx);
    DVec fe (len, 0.0); double mx = 1e-9;
    for (size_t s = 0; s + 1 < cuts.size(); ++s)
    {
        const size_t a = cuts[s], b = cuts[s + 1], L = b - a, blk = std::max ((size_t) 2, L / 8);
        std::vector<double> pk; for (size_t q = a; q < b; q += blk) { double m = 0; for (size_t i = q; i < std::min (b, q + blk); ++i) m = std::max (m, (double) std::abs (mono[i])); pk.push_back (m); }
        for (size_t i = a; i < b; ++i) { fe[i] = lerpAt (pk, std::max (0.0, ((double) (i - a) - (double) blk / 2) / (double) blk)); mx = std::max (mx, fe[i]); }
    }
    const Env ea = envOf (monoOf (aux), p["wsize"] / 1000 * sr); const double gate = std::pow (10.0, p["gate"] / 20) * std::max (1e-9, peakAll (aux));
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& y = aux[c % aux.size()]; Buf o (len);
        for (size_t i = 0; i < len; ++i)
        {
            double ap = auxPos ((double) i, len, al, fit); if (fit == 1 && al > 0) ap = std::fmod (ap, (double) al);
            const double e = ea.at (ap), v = e < gate ? 0.0 : sampAt (y, ap, false) / e;
            o[i] = (float) (v * (1 - p["depth"]) + v * p["depth"] * fe[i] / mx);
        }
        res.push_back (std::move (o));
        cx.prog ((c + 1.0) / (double) ch.size());
    }
    return res;
}

// FOF INTERLEAVE: groups of GROUP FOFs taken alternately from the two sounds (WEIGHT = how many more from the main),
// each group's spacing BIASed towards one sound's pitch, BALANCE = relative level. after CDP psow interleave (psoa_weird, psow.c)
Chans psowInterleave (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t G = (size_t) std::max (1, p.i ("group"));
    const auto c0 = fofCuts (monoOf (ch), sr, cx), c1 = fofCuts (monoOf (aux), sr, cx);
    const size_t n0 = (c0.size() - 1) / G, n1 = std::max ((size_t) 1, (c1.size() - 1) / G);
    const size_t nch = std::max (ch.size(), aux.size());
    Chans out (nch, Buf (capLen (cx, (double) ch[0].size() * 3 + sr)));
    const double bias = p["bias"], bal = p["bal"], w = p["weight"];
    long long opos = 0; size_t cnt0 = 0, cnt1 = 0, lastEnd = 0;
    for (size_t n = 0; n < n0 && opos < (long long) out[0].size(); ++n)
    {
        const size_t m = n % n1;
        const long long s0 = (long long) c0[(n + 1) * G] - (long long) c0[n * G], s1 = std::max (1LL, (long long) c1[std::min ((m + 1) * G, c1.size() - 1)] - (long long) c1[std::min (m * G, c1.size() - 1)]), diff = s1 - s0;
        const bool useAux = n > 0 && (double) cnt0 >= w * (double) (cnt1 + 1);   // WEIGHT : 1 share of FOF groups
        if (useAux) ++cnt1; else ++cnt0;
        double gain = 1; long long adv;
        if (! useAux) { if (bal < 1) gain = bal; adv = bias > 0 ? s0 : s0 - jr ((double) diff * bias); }
        else { if (bal > 1) gain = 1 / bal; adv = bias < 0 ? s1 : s1 - jr ((double) diff * bias); }
        for (size_t c = 0; c < nch; ++c)
        {
            const Buf& s = useAux ? chOr (aux, c) : chOr (ch, c); const size_t a = useAux ? c1[std::min (m * G, c1.size() - 1)] : c0[n * G]; const long long L = useAux ? s1 : s0;
            for (long long i = 0; i < L && opos + i < (long long) out[c].size(); ++i) out[c][(size_t) (opos + i)] += (float) (s[a + (size_t) i] * gain);
        }
        lastEnd = std::max (lastEnd, (size_t) (opos + (useAux ? s1 : s0)));
        opos += std::max (8LL, adv);
        if (! (n & 63)) cx.prog ((double) n / (double) n0);
    }
    for (auto& b : out) b.resize (std::max ((size_t) 16, std::min (b.size(), lastEnd)));
    return out;
}

// FOF REPLACE: the main's FOFs (in groups of GROUP) are re-spaced at the second sound's FOF periods: the main's timbre on the
// second sound's pitch line. after CDP psow replace (psoa_weird, psow.c)
Chans psowReplace (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t G = (size_t) std::max (1, p.i ("group"));
    const auto c0 = fofCuts (monoOf (ch), sr, cx), c1 = fofCuts (monoOf (aux), sr, cx);
    const size_t n0 = (c0.size() - 1) / G, n1 = std::max ((size_t) 1, (c1.size() - 1) / G);
    Chans out (ch.size(), Buf (capLen (cx, (double) ch[0].size() * 4 + sr)));
    long long opos = 0; size_t lastEnd = 0;
    for (size_t n = 0; n < n0 && opos < (long long) out[0].size(); ++n)
    {
        const size_t m = n % n1; const size_t a = c0[n * G], b = c0[(n + 1) * G];
        for (size_t c = 0; c < ch.size(); ++c)
            for (size_t i = a; i < b && opos + (long long) (i - a) < (long long) out[c].size(); ++i) out[c][(size_t) opos + (i - a)] += ch[c][i];
        lastEnd = std::max (lastEnd, (size_t) opos + (b - a));
        opos += std::max (8LL, (long long) c1[std::min ((m + 1) * G, c1.size() - 1)] - (long long) c1[std::min (m * G, c1.size() - 1)]);
        if (! (n & 63)) cx.prog ((double) n / (double) n0);
    }
    for (auto& b : out) b.resize (std::max ((size_t) 16, std::min (b.size(), lastEnd)));
    return out;
}

// syllables: cuts at the troughs of a WINDOW-sized RMS envelope that lie clearly below the peaks around them (CDP extspeak's
// automatic syllable finder)
std::vector<size_t> syllables (const Buf& x, double sr, double wsec)
{
    const size_t W = (size_t) std::max (16.0, wsec * sr), nw = x.size() / W + 1;
    DVec e (nw, 0.0);
    for (size_t i = 0; i < x.size(); ++i) e[i / W] += (double) x[i] * x[i];
    for (auto& v : e) v = std::sqrt (v / (double) W);
    std::vector<size_t> cuts { 0 }; size_t last = 0;
    for (size_t i = 2; i + 2 < nw; ++i)
    {
        if (! (e[i] <= e[i - 1] && e[i] <= e[i + 1] && e[i] <= e[i - 2] && e[i] <= e[i + 2])) continue;
        double before = 0, after = 0;
        for (size_t j = last; j < i; ++j) before = std::max (before, e[j]);
        for (size_t j = i + 1; j < std::min (nw, i + 12); ++j) after = std::max (after, e[j]);
        if (before > e[i] * 1.5 + 1e-6 && after > e[i] * 1.5 + 1e-6 && i - last >= 2) { cuts.push_back (i * W + W / 2); last = i; }
    }
    if (x.size() > cuts.back() + 16) cuts.push_back (x.size()); else cuts.back() = x.size();
    if (cuts.size() < 2) cuts = { 0, x.size() };
    return cuts;
}

// SYLLABLE SWAP: the main is cut into syllables; after OFFSET untouched ones, N syllables in every N+1 are REPLACEd by, have
// INJECTed after them, or get MIXed INTO them syllables of the second sound (taken IN TURN, at RANDOM or PERMUTED);
// KEEP N reverses the count. FIT: cut / pad, transpose to size, or original size. after CDP extspeak 1-18 (extspeak.c)
Chans extspeak (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const int mode = p.i ("mode"), order = p.i ("order"), fitm = p.i ("size");
    const auto s0 = syllables (monoOf (ch), sr, p["wsize"] / 1000), s1 = syllables (monoOf (aux), sr, p["wsize"] / 1000);
    const size_t n0 = s0.size() - 1, n1 = s1.size() - 1, nch = std::max (ch.size(), aux.size());
    const int N = std::max (1, p.i ("n")); const bool keepN = p.i ("keep") == 1; const int off = std::max (0, p.i ("offset"));
    const long long spl = std::max (2LL, jr (p["splice"] / 1000 * sr)); const double g = std::pow (10.0, p["gain"] / 20);
    cx.seed (p["seed"]);
    std::vector<size_t> perm; size_t pi = 0, turn = 0;
    auto nextAux = [&]
    {
        if (order == 1) return std::min (n1 - 1, (size_t) (cx.rand() * (double) n1));
        if (order == 2)
        {
            if (pi % n1 == 0) { perm.resize (n1); for (size_t i = 0; i < n1; ++i) perm[i] = i; for (size_t i = n1 - 1; i > 0; --i) std::swap (perm[i], perm[std::min (i, (size_t) (cx.rand() * (double) (i + 1)))]); }
            return perm[pi++ % n1];
        }
        return turn++ % n1;
    };
    auto peakOf = [] (const Chans& c, size_t a, size_t b) { double m = 0; for (auto& x : c) for (size_t i = a; i < b && i < x.size(); ++i) m = std::max (m, (double) std::abs (x[i])); return m; };
    Chans out (nch); size_t pos = 0;
    auto put = [&] (const Chans& src, size_t a, long long L, double gain, double rate, long long at)
    {
        for (size_t c = 0; c < nch; ++c) { if (out[c].size() < (size_t) (at + L)) out[c].resize ((size_t) (at + L), 0.0f); addSeg (out[c], at, chOr (src, c), (double) a, L, spl, gain, false, rate); }
    };
    for (size_t k = 0; k < n0 && pos < cx.maxLen; ++k)
    {
        const size_t a = s0[k], b = s0[k + 1]; const long long L0 = (long long) (b - a);
        bool target = false;
        if ((int) k >= off) { const int q = ((int) k - off) % (N + 1); target = keepN ? q == N : q != 0; }
        const bool replace = target && mode == 0, mixIn = target && mode == 2, inject = target && mode == 1;
        if (! replace) put (ch, a, L0, 1, 1, (long long) pos);
        if (replace || mixIn)
        {
            const size_t j = nextAux(); const size_t ja = s1[j]; const long long L1 = std::max (4LL, (long long) (s1[j + 1] - ja));
            double gain = g; if (p.i ("env") == 1) { double pa = peakOf (aux, ja, ja + (size_t) L1), pm = peakOf (ch, a, b); if (pa > 1e-9) gain *= pm / pa; }
            long long L = L0; double rate = 1;
            if (fitm == 1) rate = (double) L1 / (double) std::max (1LL, L0);
            else if (fitm == 2 && replace) L = L1;
            else L = std::min (L0, L1);
            put (aux, ja, std::max (4LL, L), gain, rate, (long long) pos);
            pos += (size_t) (replace ? std::max (4LL, (fitm == 2 ? L : L0)) : L0);
        }
        else pos += (size_t) L0;
        if (inject)
            for (int r = 0; r < (keepN ? 1 : N) && pos < cx.maxLen; ++r)
            {
                const size_t j = nextAux(); const size_t ja = s1[j]; const long long L1 = std::max (4LL, (long long) (s1[j + 1] - ja));
                double gain = g; if (p.i ("env") == 1) { double pa = peakOf (aux, ja, ja + (size_t) L1), pm = peakOf (ch, a, b); if (pa > 1e-9) gain *= pm / pa; }
                put (aux, ja, L1, gain, 1, (long long) pos); pos += (size_t) L1;
            }
        if (! (k & 7)) cx.prog ((double) k / (double) n0);
    }
    for (auto& b : out) { if (b.size() > cx.maxLen) b.resize (cx.maxLen); if (b.size() < 16) b.resize (16, 0.0f); }
    size_t L = 0; for (auto& b : out) L = std::max (L, b.size()); for (auto& b : out) b.resize (L);
    return out;
}

// PACKET STREAM: a stream of enveloped packets (RISE, SUSTAIN, DECAY, each randomised +-50%) every SPEED (SCATTER), built
// from the main and the second sound in permuted order: PITCHED iterates the source's first 30 ms at PITCH, FROM START /
// RANDOM START read the source itself. PITCH SCAT jitters, BEND glides each packet up, WIDTH scatters it in the stereo field.
// after CDP pulser multi 1-3 (pulser.c, generate_packet)
Chans pulserMulti (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const int mode = p.i ("mode");
    const Buf srcs[2] = { monoOf (ch), monoOf (aux) };
    const size_t outLen = capLen (cx, p["dur"] * sr + 3 * sr);
    Chans out (2, Buf (outLen)); cx.seed (p["seed"]);
    int perm[2] = { 0, 1 }; int pc = 0;
    Buf pk;
    for (double t = 0; t < p["dur"] && (size_t) (t * sr) < outLen;)
    {
        if (pc % 2 == 0) { if (cx.rand() < 0.5) std::swap (perm[0], perm[1]); }
        const Buf& src = srcs[perm[pc++ % 2]];
        auto rnd = [&] (double v) { return v * (0.5 + cx.rand()); };
        const long long rise = std::max (2LL, jr (rnd (p["rise"]) / 1000 * sr)), sus = jr (rnd (p["sus"]) / 1000 * sr), dec = std::max (2LL, jr (rnd (p["decay"]) / 1000 * sr));
        const long long size = rise + sus + dec;
        double frq = mode == 0 ? 440.0 * std::pow (2.0, (p["pitch"] - 69) / 12) : 1.0;
        frq *= std::pow (2.0, (cx.rand() * 2 - 1) * p["pscat"] / 12);
        const double upfrq = frq * std::pow (2.0, p["bend"] / 12);
        pk.assign ((size_t) size, 0.0f);
        if (mode == 0)
        {
            const size_t ex = std::min (src.size(), (size_t) (0.03 * sr));
            const double d0 = sr / frq, d1 = sr / upfrq, step = (d0 - d1) / std::max (1.0, (double) size / d1);
            double del = d0; long long n = 0;
            while (n < size) { for (size_t m = 0; m < ex && n + (long long) m < size; ++m) pk[(size_t) n + m] += src[m]; n += std::max (1LL, jr (del)); del = std::max (2.0, del - step); }
        }
        else
        {
            const double fs = (upfrq - frq) / (double) size; double f = frq, sp = 0;
            if (mode == 2) { const double range = (double) src.size() - (double) size * upfrq; sp = range > 0 ? cx.rand() * range : 0; }
            for (long long k = 0; k < size; ++k) { pk[(size_t) k] = (float) lerpAt (src, sp); sp += f; f += fs; }
        }
        for (long long n = 0; n < rise; ++n) pk[(size_t) n] = (float) (pk[(size_t) n] * std::pow ((double) n / (double) rise, 1.0));
        for (long long m = 0; m < dec; ++m) pk[(size_t) (size - 1 - m)] = (float) (pk[(size_t) (size - 1 - m)] * std::pow ((double) m / (double) dec, 1.0));
        const double pmx = peak (pk), nrm = pmx > 1e-9 ? 0.9 / pmx : 0;
        auto [gl, gr] = panG (0.5 + (cx.rand() * 2 - 1) * p["width"] * 0.5);
        const size_t at = (size_t) (t * sr);
        for (size_t k = 0; k < pk.size() && at + k < outLen; ++k) { out[0][at + k] += (float) (pk[k] * nrm * gl); out[1][at + k] += (float) (pk[k] * nrm * gr); }
        double step = p["speed"] * (1 + (cx.rand() * 2 - 1) * 0.5 * p["scatter"]);
        t += std::max (0.005, step);
        cx.prog (t / p["dur"]);
    }
    return out;
}

// DOUBLE SPIN: the main's and the second sound's stereo images spin around the listener, a quarter turn apart (5-channel
// CDP image folded to stereo): RATE (negative = anticlockwise), DOPPLER shift of the moving edges, BOOST in front and cut
// behind, ATTEN when the edges cross the centre; CENTRE ONLY collapses the crossing to a mono centre at CENTRE level.
// after CDP spinqdopl (spin quad 1-2, spin.c)
Chans spinQuad (const Chans& ch, Ctx& cx, const Params& p)
{
    Chans aux = getAux (cx, AUXMSG); const double sr = cx.sr; const size_t len = ch[0].size(), al = aux[0].size(); const int fit = p.i ("fit"), mode = p.i ("mode");
    const Chans a = stereoOf (ch), b = stereoOf (aux);
    const double w = TAU * p["rate"] / sr, D = std::min (sr, 2 * (1 - std::pow (2.0, -p["dopl"] / 12)) * sr / std::max (1e-3, TAU * std::abs (p["rate"])));
    Chans out (2, Buf (len + (size_t) D + 2));
    for (size_t i = 0; i < out[0].size(); ++i)
    {
        const double base = w * (double) i;
        for (int e = 0; e < 4; ++e)
        {
            // edges: main L at 0, main R at pi, aux L at pi/2, aux R at 3pi/2
            const double th = base + (e == 0 ? 0 : e == 1 ? PI : e == 2 ? PI / 2 : 1.5 * PI);
            const double x = std::cos (th), depth = std::sin (th), d = D * (1 - depth) / 2;
            const double rp = (double) i - d; if (rp < 0) continue;
            double v;
            if (e < 2) v = rp < (double) len ? lerpAt (a[(size_t) e], rp) : 0.0;
            else { double ap = auxPos (rp, len, al, fit); if (fit == 0 && rp >= (double) len) continue; v = sampAt (b[(size_t) e - 2], ap, fit == 1); }
            double g = std::pow (p["boost"], depth) * (1 - (1 - p["att"]) * std::abs (depth));
            double c = mode == 1 ? std::max (0.0, 1 - std::abs (x) * 3) : 0.0;
            auto [gl, gr] = panG ((x + 1) / 2);
            const double cg = c * p["centre"] * 0.7071;
            out[0][i] += (float) (v * g * ((1 - c) * gl + cg)); out[1][i] += (float) (v * g * ((1 - c) * gr + cg));
        }
        if (! (i & 65535)) cx.prog ((double) i / (double) out[0].size());
    }
    return out;
}

// @@PROCS@@
} // namespace

void addCdpCross (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CAT = "CDP \xc2\xb7 TWO SOUNDS";
    const std::string AUX = "SECOND SOUND";
    auto fit = [] { return Opt ("fit", "AUX TIME", FIT); };
    auto win = [] { return Win (0.01, 0.37, 0.046); };

    // ---- combine ----
    add (v, CAT, "cdp_combine_make", "PITCH + FORMANTS", "MAKE", "Rebuilds the sound from its pitch: harmonics on the main's tracked pitch (TRANSPOSE) shaped by the second sound's formants, NOISE when unpitched. MAKE2 follows the main's level.",
         { Opt ("mode", "MODE", { "MAKE", "MAKE2 (LEVEL)" }), P ("transp", "TRANSPOSE", -24, 24, 0, 0, "st"), P ("smooth", "FORMANT BW", 40, 1500, 250, PExp, {}, Fmt::hz),
           Pct ("noise", "NOISE", 0.5), fit(), win(), Seed() }, combMake, AUX, false, "combine make");
    add (v, CAT, "cdp_combine_sum", "SPECTRAL SUM", "SSUM", "Adds the second sound's spectrum (channel amplitudes, scaled by CROSS) to the main's, on the main's frequencies.",
         { P ("cross", "CROSS", 0, 2, 1), fit(), win() }, combSum, AUX, false, "combine sum");
    add (v, CAT, "cdp_combine_diff", "SPECTRAL SUBTRACT", "SDIF", "Subtracts the second sound's spectrum (times CROSS) from the main's: removes shared tones or noise. KEEP NEG lets negative amplitudes through as phase-inverted grit.",
         { P ("cross", "CROSS", 0, 4, 1), Opt ("neg", "NEGATIVES", { "ZERO", "KEEP" }), fit(), win() }, combDiff, AUX, false, "combine diff");
    add (v, CAT, "cdp_combine_interleave", "WINDOW INTERLEAVE", "LEAF", "Alternates groups of LEAF analysis windows from the main and the second sound: a fast spectral stutter between the two.",
         { P ("leaf", "LEAF", 1, 64, 4, PInt), fit(), win() }, combLeaf, AUX, false, "combine interleave");
    add (v, CAT, "cdp_combine_max", "SPECTRAL MAX", "SMAX", "In every frequency channel the louder sound wins (AUX GAIN weights the second sound): a dense spectral union of both.",
         { P ("gain2", "AUX GAIN", 0.1, 4, 1, PExp, "x"), fit(), win() }, combMax, AUX, false, "combine max");
    add (v, CAT, "cdp_combine_mean", "SPECTRAL MEAN", "MEAN", "Pairs the loudest channels of both sounds and makes partials with mean (or one sound's / max) amplitude at the mean frequency or mean pitch, between LOW and HIGH.",
         { Opt ("mode", "MODE", { "MEAN AMP, PITCH", "MEAN AMP, FRQ", "MAIN AMP, PITCH", "MAIN AMP, FRQ", "AUX AMP, PITCH", "AUX AMP, FRQ", "MAX AMP, PITCH", "MAX AMP, FRQ" }),
           P ("lo", "LOW", 20, 5000, 20, PExp, {}, Fmt::hz), P ("hi", "HIGH", 200, 22000, 16000, PExp, {}, Fmt::hz), P ("chans", "CHANNELS", 1, 1024, 256, PInt | PExp),
           Opt ("outside", "OUTSIDE", { "KEEP", "ZERO" }), fit(), win() }, combMean, AUX, false, "combine mean");
    add (v, CAT, "cdp_combine_cross", "SPECTRAL CROSS", "CROS", "Cross-synthesis: the main's channel amplitudes move towards the second sound's (INTERP START to END over the length), keeping the main's frequencies.",
         { Pct ("interp", "INTERP", 1), Pct ("interp2", "INTERP END", 1), fit(), win() }, combCross, AUX, false, "combine cross");

    // ---- formants ----
    add (v, CAT, "cdp_formants_put", "FORMANT PUT", "FPUT", "Puts the second sound's formants on the main: REPLACE swaps the spectral envelope, IMPOSE filters through it. LOW / HIGH limit the band, FORMANT BW sets the envelope detail.",
         { Opt ("mode", "MODE", { "REPLACE", "IMPOSE" }), P ("smooth", "FORMANT BW", 40, 1500, 200, PExp, {}, Fmt::hz), P ("lo", "LOW", 20, 5000, 20, PExp, {}, Fmt::hz),
           P ("hi", "HIGH", 200, 22000, 20000, PExp, {}, Fmt::hz), P ("gain", "GAIN", -24, 24, 0, 0, {}, Fmt::db), fit(), win() }, formPut, AUX, false, "formants put");
    add (v, CAT, "cdp_formants_vocode", "FORMANT VOCODER", "VOCO", "Classic vocoder: the second sound's spectral envelope replaces the main's, level preserved, band limited to LOW..HIGH. FORMANT BW sets the band detail.",
         { P ("smooth", "FORMANT BW", 40, 1500, 160, PExp, {}, Fmt::hz), P ("lo", "LOW", 20, 5000, 20, PExp, {}, Fmt::hz), P ("hi", "HIGH", 200, 22000, 20000, PExp, {}, Fmt::hz),
           P ("gain", "GAIN", -24, 24, 0, 0, {}, Fmt::db), fit(), win() }, formVocode, AUX, false, "formants vocode");

    // ---- morph ----
    add (v, CAT, "cdp_morph_glide", "SPECTRAL GLIDE", "GLID", "Freezes one moment of each sound (MAIN AT, AUX AT) and glides from the first spectrum into the second over DURATION: amplitudes linearly, frequencies exponentially.",
         { Pct ("t1", "MAIN AT", 0.25), Pct ("t2", "AUX AT", 0.25), P ("dur", "DURATION", 0.2, 30, 4, PExp, {}, Fmt::time), win() }, morphGlide, AUX, false, "morph glide");
    add (v, CAT, "cdp_morph_bridge", "SPECTRAL BRIDGE", "BRDG", "Between START and END the sound moves from the start mix (FRQ / AMP fractions of the second sound) to the end mix. MODE chooses how levels are matched.",
         { Opt ("mode", "LEVELS", { "AS IS", "TO QUIETER", "TO MAIN", "TO AUX", "MAIN\xe2\x86\x92" "AUX", "AUX\xe2\x86\x92" "MAIN" }), Pct ("start", "START", 0.2), Pct ("end", "END", 0.8),
           Pct ("sf2", "START FRQ", 0), Pct ("sa2", "START AMP", 0), Pct ("ef2", "END FRQ", 1), Pct ("ea2", "END AMP", 1), fit(), win() }, morphBridge, AUX, false, "morph bridge");
    add (v, CAT, "cdp_morph_morph", "SPECTRAL MORPH", "MRPH", "Morphs the main into the second sound: amplitudes between AMP START..END, frequencies between FRQ START..END, each with its own curve. LINEAR or COSINE.",
         { Opt ("mode", "MODE", { "LINEAR", "COSINE" }), Pct ("as", "AMP START", 0.2), Pct ("ae", "AMP END", 0.7), Pct ("fs", "FRQ START", 0.3), Pct ("fe", "FRQ END", 0.8),
           P ("expa", "AMP CURVE", 0.1, 10, 1, PExp, "x"), P ("expf", "FRQ CURVE", 0.1, 10, 1, PExp, "x"), fit(), win() }, morphMorph, AUX, false, "morph morph");
    // ---- two sounds in the time domain ----
    add (v, CAT, "cdp_distort_interact", "WAVECYCLE INTERACT", "WINT", "Two sounds meet cycle by cycle: INTERLEAVE alternates GROUP wavecycles of each, RESIZE plays the second sound's wavecycles at the main's cycle lengths (its timbre, the main's pitch). CUTOFF skips tiny cycles.",
         { Opt ("mode", "MODE", { "INTERLEAVE", "RESIZE" }), P ("group", "GROUP", 1, 16, 1, PInt), P ("cutoff", "CUTOFF", 200, 20000, 2500, PExp, {}, Fmt::hz) }, distInteract, AUX, false, "distort interact");
    add (v, CAT, "cdp_extend_sequence2", "SOUND SEQUENCER", "SEQ2", "Plays the main and the second sound as notes on the tempo grid: PATTERN picks which sound, MELODY transposes them within RANGE (pentatonic), NOTE LEN and SPLICE shape each note.",
         { Opt ("pattern", "PATTERN", { "ABAB", "AABB", "ABBA", "AAAB", "RANDOM" }), Opt ("div", "STEP", DIVS, 3), P ("bars", "LENGTH", 1, 16, 2, PInt, "bars"),
           Opt ("melody", "MELODY", { "NONE", "UP", "DOWN", "UP-DOWN", "RANDOM" }, 3), P ("steps", "STEPS", 2, 16, 8, PInt), P ("range", "RANGE", 0, 24, 12, 0, "st"),
           P ("notelen", "NOTE LEN", 0.1, 4, 0.9, PExp, "x"), P ("splice", "SPLICE", 1, 50, 5, PExp, "ms"), P ("level", "LEVEL", 0.05, 1, 0.8, PPct, {}, Fmt::pct), Seed() }, extSeq2, AUX, false, "extend sequence2");
    add (v, CAT, "cdp_grain_align", "GRAIN ALIGN", "GALN", "Rhythm transfer: grains found in the second sound (GATE 2) are moved onto the grain onsets of the main (GATE), plus OFFSET. MIN HOLE is the gap that separates grains.",
         { Pct ("gate", "GATE", 0.2), Pct ("gate2", "GATE 2", 0.2), P ("hole", "MIN HOLE", 2, 200, 20, PExp, "ms"), P ("offset", "OFFSET", -100, 100, 0, 0, "ms"),
           P ("splice", "SPLICE", 1, 50, 4, PExp, "ms") }, grainAlign, AUX, false, "grain align");
    add (v, CAT, "cdp_envel_transfer", "ENVELOPE TRANSFER", "ENVT", "Gives the main the loudness contour of the second sound: IMPOSE multiplies by it, REPLACE first removes the main's own contour. WINDOW sets the envelope detail.",
         { Opt ("mode", "MODE", { "IMPOSE", "REPLACE" }), P ("wsize", "WINDOW", 2, 1000, 30, PExp, "ms"), Pct ("depth", "DEPTH", 1), fit() }, envImpose, AUX, false, "envel impose 1 / envel replace 1");
    add (v, CAT, "cdp_modify_loudness5", "LEVEL MATCH", "LMAT", "Scales the main so its PEAK (or RMS) level equals the second sound's, plus TRIM.",
         { Opt ("measure", "MEASURE", { "PEAK", "RMS" }), P ("trim", "TRIM", -24, 24, 0, 0, {}, Fmt::db) }, loudMatch, AUX, false, "modify loudness 5");
    add (v, CAT, "cdp_modify_radical6", "CROSS MODULATE", "XMOD", "Multiplies the main by the second sound sample by sample: ring modulation where both sounds are carriers. MIX blends in the dry main.",
         { Pct ("mix", "MIX", 1), fit() }, crossMod, AUX, false, "modify radical 6");
    add (v, CAT, "cdp_modify_sausage", "SAUSAGE", "SAUS", "Brassage of both sounds scrambled together: GRAIN-sized pieces of the main or the second sound (AUX SHARE), advancing at VELOCITY, with random PITCH, AMP, SPACE and SCATTER.",
         { P ("velocity", "VELOCITY", 0, 4, 1, 0, "x"), P ("density", "DENSITY", 0.2, 8, 2, PExp, "x"), P ("grain", "GRAIN", 5, 500, 60, PExp, "ms"), P ("pitch", "PITCH", 0, 24, 3, 0, "st"),
           Pct ("amp", "AMP RANGE", 0.3), Pct ("space", "SPACE", 0.8), Pct ("scatter", "SCATTER", 0.5), Pct ("share", "AUX SHARE", 0.5), P ("length", "LENGTH", 0.25, 8, 1, PExp, "x"), Seed() },
         sausage, AUX, false, "modify sausage");
    add (v, CAT, "cdp_modify_convolve", "CONVOLVE", "CONV", "Convolves the main with the second sound as impulse response. TRANSPOSING glides the IR's pitch from T START to T END over the length. MIX is the dry/wet balance (fastconv).",
         { Opt ("mode", "MODE", { "NORMAL", "TRANSPOSING" }), P ("t0", "T START", -24, 24, 0, 0, "st"), P ("t1", "T END", -24, 24, 12, 0, "st"), Pct ("mix", "MIX", 1) },
         convolveX, "IMPULSE", false, "modify convolve 1-2 / fastconv");
    add (v, CAT, "cdp_sfedit_twixt", "SOUND SWITCH", "TWIX", "Both sounds run in parallel and the output switches between them EVERY grid step: IN SEQUENCE, PERMUTED or RANDOM segments (SEGMENTS of them). WEIGHT favours the main.",
         { Opt ("mode", "MODE", { "IN SEQUENCE", "PERMUTED", "RANDOM" }), Opt ("div", "EVERY", DIVS, 3), P ("segs", "SEGMENTS", 2, 256, 32, PInt), P ("splice", "SPLICE", 1, 50, 4, PExp, "ms"),
           P ("weight", "WEIGHT", 1, 8, 1, PInt), Opt ("order", "ORDER", { "CYCLIC", "RANDOM" }), Seed() }, twixt, AUX, false, "sfedit twixt");
    add (v, CAT, "cdp_sfedit_sphinx", "ONSET SWITCH", "SPHX", "Switches between the sounds at their own onsets (GATE, MIN HOLE): the Nth event of one swaps with the Nth of the other, IN SEQUENCE, PERMUTED or RANDOM.",
         { Opt ("mode", "MODE", { "IN SEQUENCE", "PERMUTED", "RANDOM" }), Pct ("gate", "GATE", 0.2), P ("hole", "MIN HOLE", 2, 300, 30, PExp, "ms"), P ("segs", "SEGMENTS", 2, 256, 32, PInt),
           P ("splice", "SPLICE", 1, 50, 4, PExp, "ms"), P ("weight", "WEIGHT", 1, 8, 1, PInt), Opt ("order", "ORDER", { "CYCLIC", "RANDOM" }), Seed() }, sphinx, AUX, false, "sfedit sphinx");
    add (v, CAT, "cdp_submix_balance", "BALANCE MIX", "BALM", "Mixes the two sounds with a balance moving from START to END (0 = main, 1 = second sound), wobbled by an LFO (RATE, DEPTH).",
         { Pct ("start", "START", 0), Pct ("end", "END", 1), P ("rate", "LFO RATE", 0.05, 20, 0.5, PExp, "Hz"), Pct ("depth", "LFO DEPTH", 0.4), fit() }, balanceMix, AUX, false, "submix balance");
    add (v, CAT, "cdp_submix_crossfade", "CROSSFADE", "XFAD", "Crossfades from the main into the second sound (starting STAGGER later) between BEGIN and END: LINEAR or COSINE, shaped by SKEW. The second sound then plays to its end.",
         { Opt ("mode", "MODE", { "LINEAR", "COSINE" }), P ("stagger", "STAGGER", 0, 10, 0, 0, {}, Fmt::time), Pct ("begin", "BEGIN", 0.3), Pct ("end", "END", 0.7),
           P ("skew", "SKEW", 0.2, 5, 1, PExp, "x") }, crossfade, AUX, false, "submix crossfade");
    add (v, CAT, "cdp_submix_inbetween2", "WAVESET MORPH", "INBT", "Morphs cycle by cycle between the sounds, pegged to zero crossings: each wavecycle takes a length and shape between the main's and the second sound's, weight START to END.",
         { Pct ("start", "START", 0), Pct ("end", "END", 1), P ("cutoff", "CUTOFF", 200, 20000, 1500, PExp, {}, Fmt::hz) }, inbetween2, AUX, false, "submix inbetween2");
    add (v, CAT, "cdp_subtract", "SUBTRACT", "SUBT", "Subtracts the second sound (DELAYed, times GAIN) from ALL, LEFT or RIGHT channels: null tests, phase cancellation, difference signals.",
         { P ("gain", "GAIN", 0, 2, 1), P ("delay", "DELAY", 0, 500, 0, 0, "ms"), Opt ("chan", "CHANNEL", { "ALL", "LEFT", "RIGHT" }), fit() }, subtractX, AUX, false, "subtract");
    // ---- one-moment formants, partials, envelopes, peaks ----
    add (v, CAT, "cdp_oneform_put", "FROZEN FORMANT", "1FPT", "Takes one snapshot of the second sound's formants (TAKE AT) and puts it on the whole main: REPLACE swaps the main's own envelope, IMPOSE filters through it.",
         { Opt ("mode", "MODE", { "REPLACE", "IMPOSE" }), Pct ("at", "TAKE AT", 0.3), P ("smooth", "FORMANT BW", 40, 1500, 200, PExp, {}, Fmt::hz), P ("lo", "LOW", 20, 5000, 20, PExp, {}, Fmt::hz),
           P ("hi", "HIGH", 200, 22000, 20000, PExp, {}, Fmt::hz), P ("gain", "GAIN", -24, 24, 0, 0, {}, Fmt::db), win() }, onePut, AUX, false, "oneform put");
    add (v, CAT, "cdp_oneform_combine", "PITCH + ONE FORMANT", "1FCB", "Harmonics on the main's tracked pitch (TRANSPOSE), all shaped by one frozen formant snapshot of the second sound (TAKE AT); NOISE when unpitched.",
         { Opt ("mode", "MODE", { "PLAIN", "MAIN LEVEL" }), Pct ("at", "TAKE AT", 0.3), P ("transp", "TRANSPOSE", -24, 24, 0, 0, "st"), P ("smooth", "FORMANT BW", 40, 1500, 250, PExp, {}, Fmt::hz),
           Pct ("noise", "NOISE", 0.5), win(), Seed() }, oneCombine, AUX, false, "oneform combine");
    add (v, CAT, "cdp_specross_partials", "PARTIAL MORPH", "SPXP", "The main's partials (on its tracked pitch) take on the harmonic profile of the second sound, non-partials fade: INTERP START to END. TUNING = harmonic tolerance, THRESHOLD drops weak partials.",
         { Pct ("interp", "INTERP", 0.3), Pct ("interp2", "INTERP END", 1), P ("tuning", "TUNING", 0.1, 3, 1, 0, "st"), Pct ("thresh", "THRESHOLD", 0.01),
           P ("level", "LEVEL", -24, 12, 0, 0, {}, Fmt::db), fit(), win() }, specross, AUX, false, "specross partials");
    add (v, CAT, "cdp_specenv", "SPECTRAL ENVELOPE", "SENV", "Applies the second sound's band envelope to the main: REPLACE or IMPOSE, bands BAND channels wide (or BAND semitones in PITCH scale). LEVEL follows either sound; BALANCE mixes back an original (+ main, - aux).",
         { Opt ("mode", "MODE", { "REPLACE", "IMPOSE" }), P ("band", "BAND", 1, 48, 6, PExp), Opt ("scale", "SCALE", { "FREQUENCY", "PITCH" }), Opt ("level", "LEVEL", { "MAIN", "AUX" }),
           P ("bal", "BALANCE", -1, 1, 0), fit(), win() }, specenvX, AUX, false, "specenv specenv");
    add (v, CAT, "cdp_newmorph", "PEAK MORPH", "NMPH", "Morphs dissimilar spectra by moving their PEAKS strongest peaks into each other (AVERAGED over the sounds or PER WINDOW), or TUNEs the main to the second sound's peaks. START..END, curve EXP.",
         { Opt ("mode", "MODE", { "LINEAR AVERAGED", "COSINE AVERAGED", "LINEAR PER WINDOW", "COSINE PER WINDOW", "TUNE LINEAR", "TUNE COSINE" }), Pct ("start", "START", 0.1), Pct ("end", "END", 0.8),
           P ("exp", "EXP", 0.2, 5, 1, PExp, "x"), P ("peaks", "PEAKS", 1, 64, 8, PInt), Opt ("what", "MORPH", { "ALL", "PEAKS ONLY", "FRQ ONLY" }), Opt ("keep", "LEVEL", { "MORPH", "KEEP MAIN" }),
           fit(), win() }, newmorph, AUX, false, "newmorph newmorph");
    add (v, CAT, "cdp_newmorph2", "PEAK TUNE", "NMP2", "Gradually tunes the main (START..END, curve EXP, LINEAR or COSINE) to a field of goal frequencies: the second sound's PEAKS strongest peaks, or a chord / harmonic series on ROOT. RANDOM detunes the goals.",
         { Opt ("mode", "MODE", { "LINEAR", "COSINE" }), Opt ("source", "GOALS", { "AUX PEAKS", "MAJOR", "MINOR", "FIFTHS", "WHOLE TONE", "HARMONICS" }), P ("root", "ROOT", 24, 84, 45, PInt, {}, Fmt::note),
           P ("peaks", "PEAKS", 1, 64, 12, PInt), Pct ("start", "START", 0.1), Pct ("end", "END", 0.7), P ("exp", "EXP", 0.2, 5, 1, PExp, "x"), Pct ("rand", "RANDOM", 0), win(), Seed() },
         newmorph2, AUX, false, "newmorph newmorph2");
    add (v, CAT, "cdp_specsphinx", "SPECTRAL SPHINX", "SSPX", "IMPOSE: the main's frequencies with the second sound's channel amplitudes (AMP BAL keeps the main's, FRQ BAL takes the aux's strongest frequencies). MULTIPLY: the spectra are multiplied (BIAS, GAIN).",
         { Opt ("mode", "MODE", { "IMPOSE", "MULTIPLY" }), Pct ("ampbal", "AMP BAL", 0), Pct ("frqbal", "FRQ BAL", 0.2), P ("bias", "BIAS", -1, 1, 0), P ("gain", "GAIN", -12, 48, 12, 0, {}, Fmt::db),
           fit(), win() }, specsphinx, AUX, false, "specsphinx specsphinx");
    add (v, CAT, "cdp_spectwin", "SPECTRAL TWIN", "TWIN", "Twins two spectra: amplitudes from their formant or total envelopes (ENV INTERP), frequencies mixed by FRQ INTERP. DUPLICATES stacks the main at rising pitches (STEP, DECAY). BAND = formant detail.",
         { Opt ("mode", "MODE", { "FORMANT/FORMANT", "FORMANT/TOTAL", "TOTAL/FORMANT", "TOTAL/TOTAL" }), Pct ("frqint", "FRQ INTERP", 0.5), Pct ("envint", "ENV INTERP", 0.7),
           P ("dupl", "DUPLICATES", 0, 8, 0, PInt), P ("step", "STEP", 1, 24, 12, 0, "st"), Pct ("dec", "DECAY", 0.5), P ("band", "BAND", 1, 12, 2, PExp, "st"), fit(), win() }, spectwin, AUX, false, "spectwin spectwin");
    // ---- FOFs, syllables, packets, space ----
    add (v, CAT, "cdp_psow_impose", "FOF IMPOSE", "FOFI", "Voice imprint: the pitch-pulse envelopes of the main's FOFs (one per period) are imposed on the second sound, flattened first by its own envelope (WINDOW, GATE). DEPTH = amount.",
         { Pct ("depth", "DEPTH", 1), P ("wsize", "WINDOW", 2, 200, 20, PExp, "ms"), P ("gate", "GATE", -90, -10, -50, 0, {}, Fmt::db), fit() }, psowImpose, AUX, false, "psow impose");
    add (v, CAT, "cdp_psow_interleave", "FOF INTERLEAVE", "FOFL", "Alternates groups of GROUP pitch-synchronous grains (FOFs) from the two sounds: WEIGHT = groups of the main per aux group, BIAS pulls the pitch to the main (+) or aux (-), BALANCE = levels.",
         { P ("group", "GROUP", 1, 16, 2, PInt), P ("bias", "BIAS", -1, 1, 0), P ("bal", "BALANCE", 0.1, 8, 1, PExp, "x"), P ("weight", "WEIGHT", 0.25, 8, 1, PExp, "x") },
         psowInterleave, AUX, false, "psow interleave");
    add (v, CAT, "cdp_psow_replace", "FOF REPLACE", "FOFR", "The main's FOFs (in GROUPs) are re-spaced at the second sound's periods: the main's voice timbre sings the second sound's pitch line.",
         { P ("group", "GROUP", 1, 16, 1, PInt) }, psowReplace, AUX, false, "psow replace");
    add (v, CAT, "cdp_extspeak", "SYLLABLE SWAP", "XSPK", "Cuts both sounds into syllables (WINDOW) and REPLACEs, INJECTs after or MIXes INTO N in every N+1 of the main's syllables (KEEP N reverses) those of the second sound, IN TURN, RANDOM or PERMUTED.",
         { Opt ("mode", "MODE", { "REPLACE", "INJECT", "MIX INTO" }), Opt ("order", "ORDER", { "IN TURN", "RANDOM", "PERMUTED" }), P ("wsize", "WINDOW", 10, 300, 50, PExp, "ms"),
           P ("n", "N", 1, 8, 1, PInt), Opt ("keep", "COUNT", { "NEW N", "KEEP N" }), P ("offset", "OFFSET", 0, 16, 0, PInt), Opt ("size", "FIT", { "CUT / PAD", "TRANSPOSE", "ORIGINAL" }),
           Opt ("env", "ENVELOPE", { "OWN", "FOLLOW" }), P ("gain", "GAIN", -48, 0, 0, 0, {}, Fmt::db), P ("splice", "SPLICE", 1, 50, 15, PExp, "ms"), Seed() }, extspeak, AUX, false, "extspeak 1-18");
    add (v, CAT, "cdp_pulser_multi", "PACKET STREAM", "PULS", "A stream of enveloped packets (RISE, SUSTAIN, DECAY) every SPEED (SCATTER) from both sounds in turn: PITCHED combs the start of each at PITCH, or packets read FROM START / RANDOM START. BEND, WIDTH.",
         { Opt ("mode", "MODE", { "PITCHED", "FROM START", "RANDOM START" }), P ("dur", "DURATION", 1, 60, 6, PExp, {}, Fmt::time), P ("pitch", "PITCH", 24, 96, 57, PInt, {}, Fmt::note),
           P ("rise", "RISE", 2, 200, 10, PExp, "ms"), P ("sus", "SUSTAIN", 0, 200, 30, 0, "ms"), P ("decay", "DECAY", 20, 2000, 200, PExp, "ms"), P ("speed", "SPEED", 0.05, 1, 0.15, PExp, {}, Fmt::time),
           Pct ("scatter", "SCATTER", 0.3), P ("pscat", "PITCH SCAT", 0, 1, 0.2, 0, "st"), P ("bend", "BEND", 0, 12, 0.5, 0, "st"), Pct ("width", "WIDTH", 0.7), Seed() },
         pulserMulti, AUX, false, "pulser multi");
    add (v, CAT, "cdp_spin_quad", "DOUBLE SPIN", "SPNQ", "Both sounds' stereo images spin around you a quarter turn apart: RATE (negative = anticlockwise), DOPPLER on the moving edges, BOOST in front, ATTEN at the crossing. CENTRE ONLY collapses the crossing to a mono centre.",
         { Opt ("mode", "MODE", { "OUTER CENTRE", "CENTRE ONLY" }), P ("rate", "RATE", -8, 8, 0.4, 0, "Hz"), P ("dopl", "DOPPLER", 0, 12, 1, 0, "st"), P ("boost", "BOOST", 1, 4, 1.5, PExp, "x"),
           Pct ("att", "ATTEN", 0.8), Pct ("centre", "CENTRE", 0.7), fit() }, spinQuad, AUX, false, "spin quad");
    // @@DEFS@@
}
} // namespace lab
