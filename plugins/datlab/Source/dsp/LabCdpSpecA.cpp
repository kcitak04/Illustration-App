// CDP collection, Batch 1 - Spectral A: classic pvoc programs (spec, blur, focus, strange, stretch, pitch, repitch).
// CDP works on phase-vocoder analysis files: every window holds, per channel, an amplitude and a true frequency.
// Here each process analyses the sound internally into the same representation (SpecReader: amplitude + frequency
// in Hz per bin), edits it window by window like the CDP program, and resynthesises it with running phases
// (Synth: data whose frequency moved is re-assigned to the right bin, the loudest wins, as CDP does).
#include "LabCdp.h"
#include "LabDspUtil.h"
#include <array>

namespace lab
{
namespace
{
using namespace lab::dsp;

// ---------------------------------------------------------------------------------------------
// analysis / resynthesis framework
// ---------------------------------------------------------------------------------------------
inline size_t fftN (double win, double sr) { return pow2 (clampd (win, 0.004, 1.0) * sr); }

// random-access analysis of one channel; frame f is centred at sample f * H. A small cache keeps memory bounded,
// so even long sources or time-reordering processes never hold the whole analysis.
struct SpecReader
{
    const Buf& x; size_t N, H, half; double sr, bin; long long frames;
    DVec w, re, im, ph0, ph1;
    struct Slot { long long f = -1; DVec a, fr, ph; };
    std::array<Slot, 6> cache; size_t next = 0;
    long long lastF = -2;   // frame whose phases are in ph1

    SpecReader (const Buf& src, double srate, size_t n) : x (src), N (n), H (n >> 2), half (n >> 1), sr (srate), bin (srate / (double) n)
    {
        frames = (long long) (x.size() / H) + 2;
        w = hann (N); re.assign (N, 0); im.assign (N, 0); ph0.assign (half + 1, 0); ph1.assign (half + 1, 0);
        for (auto& s : cache) { s.a.assign (half + 1, 0); s.fr.assign (half + 1, 0); s.ph.assign (half + 1, 0); }
    }
    double ft() const { return (double) H / sr; }   // seconds per frame
    void phases (long long f, DVec& ph, DVec* mag)
    {
        analyze (x, f * (long long) H - (long long) half, w, re, im);
        for (size_t k = 0; k <= half; ++k) { ph[k] = std::atan2 (im[k], re[k]); if (mag) (*mag)[k] = std::hypot (re[k], im[k]); }
    }
    const Slot& get (long long f)
    {
        f = clamp (f, 0LL, frames - 1);
        for (auto& s : cache) if (s.f == f) return s;
        Slot& s = cache[next]; next = (next + 1) % cache.size();
        if (f == 0) std::fill (ph0.begin(), ph0.end(), 0.0);
        else if (lastF == f - 1) ph0.swap (ph1);
        else phases (f - 1, ph0, nullptr);
        phases (f, ph1, &s.a); lastF = f;
        const double ex = TAU * (double) H / (double) N;
        for (size_t k = 0; k <= half; ++k)
        {
            double d = ph1[k] - ph0[k] - (double) k * ex; d -= TAU * jsround (d / TAU);
            s.fr[k] = ((double) k + d / ex) * bin;
        }
        s.ph = ph1; s.f = f;
        return s;
    }
};

// overlap-add resynthesis with running phases; put() frames in order 0, 1, 2, ...
struct Synth
{
    size_t N, H, half; double sr, bin; Buf out; DVec w, re, im, acc, na, nf; std::vector<size_t> src, owner;
    Synth (size_t n, double srate, size_t outLen) : N (n), H (n >> 2), half (n >> 1), sr (srate), bin (srate / (double) n), out (outLen)
    { w = hann (N); re.assign (N, 0); im.assign (N, 0); acc.assign (half + 1, 0); na.assign (half + 1, 0); nf.assign (half + 1, 0); src.assign (half + 1, 0); owner.assign (half + 1, 0); }
    long long framesFor() const { return (long long) std::ceil ((double) out.size() / (double) H) + 1; }
    // a/fr: amplitude and frequency (Hz) per bin. Data stays in its bin while its frequency is within the main lobe;
    // otherwise it moves to the bin of its new frequency, keeping its offset inside the lobe when the original
    // frequencies (orig) are known, so a transposed partial keeps its full lobe. The loudest data wins a bin.
    // ph: the analysis phases of the window the data came from (indexed by source bin). When given, the phases are
    // locked to the spectral peaks (identity phase locking, Laroche & Dolson): each peak runs on its own frequency and
    // the bins around it keep their analysed phase offsets, so stretched or reordered windows do not go phasey.
    void put (long long f, const DVec& a, const DVec& fr, const DVec* orig = nullptr, const DVec* ph = nullptr)
    {
        std::fill (na.begin(), na.end(), 0.0);
        for (size_t k = 0; k <= half; ++k)
        {
            double m = a[k], q = fr[k];
            if (! (m > 0) || ! std::isfinite (m) || ! std::isfinite (q)) continue;
            q = std::abs (q); const double pos = q / bin;
            double off = 0;
            if (orig) off = clampd ((double) k - std::abs ((*orig)[k]) / bin, -2, 2);
            else if (std::abs (pos - (double) k) <= 2) off = (double) k - pos;
            long long j = jr (pos + off);
            if (j < 1 || j > (long long) half) continue;
            if (m > na[(size_t) j]) { na[(size_t) j] = m; nf[(size_t) j] = q; src[(size_t) j] = k; }
        }
        const double ts = TAU * (double) H / sr;
        for (size_t k = 0; k <= half; ++k)
        {
            acc[k] += ts * (na[k] > 0 ? nf[k] : (double) k * bin);
            acc[k] -= TAU * std::floor (acc[k] / TAU);
        }
        if (ph)
        {   // each bin belongs to the peak of its region (regions split at the lowest bin between two peaks)
            size_t lastPk = 0; bool have = false;
            for (size_t k = 1; k < half; ++k)
            {
                if (! (na[k] > 0 && na[k] >= na[k - 1] && na[k] >= na[k + 1])) continue;
                if (have) { size_t lo = lastPk; for (size_t j = lastPk; j <= k; ++j) if (na[j] < na[lo]) lo = j; for (size_t j = lastPk; j < lo; ++j) owner[j] = lastPk; for (size_t j = lo; j <= k; ++j) owner[j] = k; }
                else for (size_t j = 0; j <= k; ++j) owner[j] = k;
                lastPk = k; have = true;
            }
            if (have)
            {
                for (size_t j = lastPk; j <= half; ++j) owner[j] = lastPk;
                for (size_t k = 0; k <= half; ++k)
                {
                    const size_t P = owner[k];
                    if (P != k && na[k] > 0 && na[P] > 0) acc[k] = acc[P] + (*ph)[src[k]] - (*ph)[src[P]];
                }
            }
        }
        for (size_t k = 0; k <= half; ++k) { re[k] = na[k] * std::cos (acc[k]); im[k] = na[k] * std::sin (acc[k]); }
        mirror (re, im); synth (out, f * (long long) H - (long long) half, w, re, im, 2.0 / 3.0);
    }
};

// what a frame callback gets besides the data; useOrig = false when the callback put data into bins itself
// ph = analysis phases matching the data (nullptr when the data no longer comes from one window)
struct Fr { int c; long long f; double t; SpecReader& R; bool useOrig = true; const DVec* ph = nullptr; };
using FrameOp = std::function<void (DVec& a, DVec& fr, Fr& info)>;

// the common case: one output window per input window, same length
Chans specMap (const Chans& ch, Ctx& cx, double win, const FrameOp& op, double seed = -1)
{
    const size_t len = ch[0].size(), N = fftN (win, cx.sr);
    const size_t outLen = std::min (len, cx.maxLen);
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        if (seed >= 0) cx.seed (seed);
        SpecReader R (ch[c], cx.sr, N); Synth S (N, cx.sr, outLen);
        DVec a (R.half + 1), fr (R.half + 1), orig (R.half + 1), ph (R.half + 1);
        const long long F = S.framesFor();
        for (long long f = 0; f < F; ++f)
        {
            const auto& s = R.get (f); a = s.a; fr = s.fr; orig = s.fr; ph = s.ph;
            Fr info { (int) c, f, (double) f * R.ft(), R, true, &ph };
            op (a, fr, info);
            S.put (f, a, fr, info.useOrig ? &orig : nullptr, info.ph);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) F) / (double) ch.size());
        }
        res.push_back (std::move (S.out));
    }
    return res;
}

// time-reordering processes: output frame o is built by gen(o, a, fr, R); outFrames decides the length
// gen may fill orig (the frequencies the data had before it was moved) and set useOrig, see Synth::put
struct GenOut { DVec orig, ph; bool useOrig = false, usePh = false; };
using GenOp = std::function<void (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)>;
Chans specGen (const Chans& ch, Ctx& cx, double win, long long outFrames, const GenOp& gen, double seed = -1)
{
    const size_t N = fftN (win, cx.sr), H = N >> 2;
    outFrames = std::max (2LL, outFrames);
    const size_t outLen = std::min (cx.maxLen, (size_t) std::max (1LL, (outFrames - 1) * (long long) H));
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        if (seed >= 0) cx.seed (seed);
        SpecReader R (ch[c], cx.sr, N); Synth S (N, cx.sr, outLen);
        DVec a (R.half + 1), fr (R.half + 1); GenOut g; g.orig.assign (R.half + 1, 0);
        const long long F = std::min (outFrames, S.framesFor());
        for (long long o = 0; o < F; ++o)
        {
            g.useOrig = false; g.usePh = false;
            gen (o, a, fr, R, g);
            S.put (o, a, fr, g.useOrig ? &g.orig : nullptr, g.usePh ? &g.ph : nullptr);
            if (! (o & 15)) cx.prog (((double) c + (double) o / (double) F) / (double) ch.size());
        }
        res.push_back (std::move (S.out));
    }
    return res;
}
inline void copyFrame (SpecReader& R, long long f, DVec& a, DVec& fr, DVec* ph = nullptr) { const auto& s = R.get (f); a = s.a; fr = s.fr; if (ph) *ph = s.ph; }
inline void copyFrame (SpecReader& R, long long f, DVec& a, DVec& fr, GenOut& g) { copyFrame (R, f, a, fr, &g.ph); g.usePh = true; }

inline double total (const DVec& a) { double t = 0; for (double v : a) t += v; return t; }
inline void normTo (DVec& a, double pre)   // CDP normalise(): restore the window's total amplitude
{
    const double post = total (a);
    if (post > 1e-12 && pre > 0) { const double g = std::min (1e4, pre / post); for (double& v : a) v *= g; }
}
// smoothed spectral envelope (CDP extract_specenv equivalent): moving average over +-detailHz
inline void specEnv (const DVec& a, double bin, double detailHz, DVec& env) { env.resize (a.size()); envelope (a, std::max (1.0, detailHz / bin), env); }
inline double envAt (const DVec& env, double frq, double bin)
{
    double p = clampd (std::abs (frq) / bin, 0, (double) env.size() - 1.001);
    return lerpAt (env, p);
}
inline double midiOf (double hz) { return 69 + 12 * std::log2 (std::max (1e-3, hz) / 440); }
inline double hzOf (double midi) { return 440 * std::pow (2.0, (midi - 69) / 12); }

// pitch track of the mono mix, f0 per analysis frame (0 = unvoiced). CDP takes a pitch file made by repitch
// getpitch from the same sound; when nothing in the sound is pitched, every frame gets the fallback f0 instead.
std::vector<double> framePitch (const Chans& ch, Ctx& cx, size_t H, long long frames, double fallback = 110)
{
    const PitchTrack pt = trackPitch (monoOf (ch), cx.sr, 50, 1500, 0.15, &cx);
    std::vector<double> f0 ((size_t) std::max (1LL, frames));
    bool any = false;
    for (size_t i = 0; i < f0.size(); ++i) { f0[i] = pt.at ((double) i * (double) H); any = any || f0[i] > 0; }
    if (! any && fallback > 0) std::fill (f0.begin(), f0.end(), fallback);
    return f0;
}
inline long long framesOf (size_t len, size_t H) { return (long long) (len / H) + 2; }

// ---------------------------------------------------------------------------------------------
// spec
// ---------------------------------------------------------------------------------------------

// Multiplies every channel amplitude by a gain, swept from START to END over the sound.
// after CDP specgain (dev/spec/simple.c)
Chans specGain (const Chans& ch, Ctx& cx, const Params& p)
{
    const double g0 = p["start"], g1 = p["end"], len = (double) std::max<size_t> (1, ch[0].size()) / cx.sr;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr& in)
    {
        const double g = std::pow (10.0, (g0 + (g1 - g0) * clampd (in.t / len, 0, 1)) / 20);
        for (double& v : a) v *= g;
    });
}

// Zeroes every channel quieter than THRESH (relative to the loudest channel anywhere in the sound).
// after CDP speclimit (dev/spec/simple.c)
Chans specGate (const Chans& ch, Ctx& cx, const Params& p)
{
    double mx = 0;
    {   // CDP's threshold is absolute: measure the loudest channel first
        const size_t N = fftN (p["win"], cx.sr);
        for (auto& x : ch) { SpecReader R (x, cx.sr, N); for (long long f = 0; f < R.frames; f += 2) { const auto& s = R.get (f); for (double v : s.a) mx = std::max (mx, v); } }
    }
    const double thr = mx * std::pow (10.0, p["thr"] / 20);
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr&) { for (double& v : a) if (v < thr) v = 0; });
}

// Keeps only the channels lying on harmonics of the tracked pitch (within SPAN), zeroing everything between;
// LESS BODY keeps only the loudest channel near each harmonic. Unpitched windows pass unchanged.
// after CDP specbare (dev/cdp2k/tklib3.c)
Chans specBare (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr);
    const auto f0 = framePitch (ch, cx, N >> 2, framesOf (ch[0].size(), N >> 2));
    const bool less = p.i ("body") == 1;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double pitch = f0[(size_t) clamp (in.f, 0LL, (long long) f0.size() - 1)];
        if (pitch <= 0) return;
        const double up = std::max (p["span"] * pitch, 1.5 * in.R.bin), dn = up, nyq = in.R.sr / 2;
        const size_t half = a.size() - 1;
        std::vector<int> best ((size_t) (nyq / pitch) + 2, -1);
        for (size_t k = 0; k <= half; ++k)
        {
            const double q = std::abs (fr[k]); const long long n = jr (q / pitch);
            const double h = (double) n * pitch;
            if (n < 1 || q < h - dn || q > h + up) { a[k] = 0; continue; }
            if (less)
            {
                int& b = best[(size_t) std::min<long long> (n, (long long) best.size() - 1)];
                if (b < 0 || a[k] > a[(size_t) b]) { if (b >= 0) a[(size_t) b] = 0; b = (int) k; } else a[k] = 0;
            }
        }
    });
}

// Spectral noise reduction: a noise print (the loudest level each channel reaches in the quietest part of the sound,
// or at its start or end) times GAIN is the floor below which channels are deleted. FROM TIME: once a channel drops
// below the floor (after SKIP) it stays deleted; ANYWHERE: deleted wherever it is below; ABOVE FREQ: only channels
// above FREQ are cleaned; COMPARE: channels never louder than the floor anywhere are removed throughout.
// after CDP specclean (dev/spec/simple.c)
Chans specClean (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"), where = p.i ("print");
    const size_t N = fftN (p["win"], cx.sr), H = N >> 2, half = N >> 1;
    const double sr = cx.sr, bin = sr / (double) N, gain = p["gain"];
    const long long F = framesOf (ch[0].size(), H), pf = std::max (1LL, std::min (F, jr (p["plen"] * sr / (double) H)));
    long long p0 = 0;
    if (where == 1) p0 = std::max (0LL, F - pf);
    else if (where == 0)
    {   // quietest stretch of pf frames (by RMS of the mono mix)
        const Buf m = monoOf (ch); double best = 1e300;
        for (long long s = 0; s + pf <= F; s += std::max (1LL, pf / 4))
        {
            double e = 0; const size_t a0 = (size_t) (s * (long long) H), a1 = std::min (m.size(), (size_t) ((s + pf) * (long long) H));
            for (size_t i = a0; i < a1; ++i) e += (double) m[i] * m[i];
            e /= (double) std::max<size_t> (1, a1 > a0 ? a1 - a0 : 1);
            if (e < best) { best = e; p0 = s; }
        }
    }
    const long long skipF = jr (p["skip"] * sr / (double) H);
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        SpecReader R (ch[c], sr, N);
        DVec floorA (half + 1, 0.0);
        for (long long f = p0; f < p0 + pf && f < F; ++f) { const auto& s = R.get (f); for (size_t k = 0; k <= half; ++k) floorA[k] = std::max (floorA[k], s.a[k]); }
        for (double& v : floorA) v *= gain;
        if (mode == 2) for (size_t k = 0; k <= half; ++k) if (((double) k + 0.5) * bin < p["frq"]) floorA[k] = 0;
        std::vector<uint8_t> kill (half + 1, 0);
        if (mode == 3)
        {   // compare: mark channels that never exceed the noise floor
            std::fill (kill.begin(), kill.end(), 1);
            for (long long f = 0; f < F; ++f) { const auto& s = R.get (f); for (size_t k = 0; k <= half; ++k) if (s.a[k] > floorA[k]) kill[k] = 0; if (! (f & 31)) cx.prog (0.5 * f / (double) F); }
        }
        const size_t outLen = std::min (ch[0].size(), cx.maxLen);
        Synth S (N, sr, outLen); DVec a (half + 1), fr (half + 1);
        const long long FO = S.framesFor();
        for (long long f = 0; f < FO; ++f)
        {
            copyFrame (R, f, a, fr);
            if (mode == 3) { for (size_t k = 0; k <= half; ++k) if (kill[k]) a[k] = 0; }
            else if (mode == 2 || f >= skipF)
                for (size_t k = 0; k <= half; ++k)
                    if (a[k] < floorA[k]) { a[k] = 0; if (mode == 0) floorA[k] = 1e30; }
            S.put (f, a, fr);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) FO) / (double) ch.size());
        }
        res.push_back (std::move (S.out));
    }
    return res;
}

// Grabs the analysis window at TIME and sustains it for DUR: a frozen, perfectly steady spectrum.
// after CDP specgrab_or_magnify (dev/spec/simple.c)
Chans specMagnify (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr), H = N >> 2;
    const long long F = framesOf (ch[0].size(), H), at = clamp (jr (p["pos"] * (double) (F - 2)), 0LL, F - 1);
    const long long outF = jr (p["dur"] * cx.sr / (double) H) + 1;
    return specGen (ch, cx, p["win"], outF, [&] (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)
    {
        copyFrame (R, at, a, fr, g);
        if (o == 0) std::fill (a.begin(), a.end(), 0.0);   // CDP zeroes the amplitudes of window one
    });
}

// ---------------------------------------------------------------------------------------------
// blur
// ---------------------------------------------------------------------------------------------

// Averages each channel's amplitude with its neighbours over a span of N channels (smaller spans at the edges).
// after CDP specavrg (dev/blur/blur.c)
Chans blurAvrg (const Chans& ch, Ctx& cx, const Params& p)
{
    const long long span = std::max (0, p.i ("n") / 2);
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr&)
    {
        if (! span) return;
        const long long n = (long long) a.size(); DVec ps ((size_t) n + 1);
        for (long long i = 0; i < n; ++i) ps[(size_t) i + 1] = ps[(size_t) i] + a[(size_t) i];
        for (long long i = 0; i < n; ++i)
        {
            const long long lo = std::max (0LL, i - span), hi = std::min (n, i + span + 1);
            a[(size_t) i] = (ps[(size_t) hi] - ps[(size_t) lo]) / (double) (hi - lo);
        }
    });
}

// Time-averages the spectrum: keeps every BLUR-th window and interpolates amplitudes and frequencies between them.
// after CDP spec_blur_and_bltr (dev/blur/ap_blur.c)
Chans blurBlur (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr);
    const long long F = framesOf (ch[0].size(), N >> 2), B = std::max (1, p.i ("blur"));
    DVec a2, f2;
    return specGen (ch, cx, p["win"], F, [&] (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)
    {
        const long long s = (o / B) * B, e = std::min (s + B, F - 1);
        copyFrame (R, (o - s) * 2 < (e - s) ? s : e, a, fr, g);
        copyFrame (R, s, a, fr);
        if (e <= s) return;
        const double t = (double) (o - s) / (double) (e - s);
        copyFrame (R, e, a2, f2);
        for (size_t k = 0; k < a.size(); ++k) { a[k] += (a2[k] - a[k]) * t; fr[k] += (f2[k] - fr[k]) * t; }
    });
}

// Removes the N loudest channels of every window: the dominant partials vanish, the rest of the spectrum stays.
// after CDP specsupr (dev/blur/blur.c)
Chans blurSuppress (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t n = (size_t) std::max (1, p.i ("n"));
    std::vector<size_t> idx;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr&)
    {
        idx.resize (a.size()); for (size_t k = 0; k < a.size(); ++k) idx[k] = k;
        const size_t m = std::min (n, a.size());
        std::nth_element (idx.begin(), idx.begin() + (long) (m - 1), idx.end(), [&] (size_t x, size_t y) { return a[x] > a[y]; });
        for (size_t i = 0; i < m; ++i) a[idx[i]] = 0;
    });
}

// Chorus by randomising, in every window, each channel's amplitude (by up to ASPREAD times) and/or frequency
// (by up to FSPREAD times, or only up / only down). Amplitude modes keep the window's total level.
// after CDP specchorus (dev/blur/blur.c)
Chans blurChorus (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode");
    const bool doA = mode == 0 || mode >= 4, doF = mode >= 1;
    const int dir = (mode == 2 || mode == 5) ? 1 : (mode == 3 || mode == 6) ? -1 : 0;
    const double la = std::log (p["aspread"]), lf = std::log (p["fspread"]);
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double pre = total (a), nyq = in.R.sr / 2, lo = in.R.bin / 2;
        for (size_t k = 0; k < a.size(); ++k)
        {
            if (doA) a[k] *= std::exp (la * (cx.rand() * 2 - 1));
            if (doF)
            {
                double r = dir > 0 ? cx.rand() : dir < 0 ? -cx.rand() : cx.rand() * 2 - 1;
                double q = fr[k] * std::exp (lf * r);
                if (q > nyq || q < lo) a[k] = 0; else fr[k] = q;
            }
        }
        if (doA) normTo (a, pre);
    }, p["seed"]);
}

// Drunken walk through the analysis windows: from START, each step moves up to RANGE windows forwards or
// backwards (reflecting at the ends) until the output lasts DURATION. NO REPEATS forbids zero steps.
// after CDP specdrunk (dev/blur/blur.c)
Chans blurDrunk (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr), H = N >> 2;
    const long long F = framesOf (ch[0].size(), H), range = std::max (1, p.i ("range"));
    const long long outF = jr (p["dur"] * cx.sr / (double) H) + 1;
    const bool noZero = p.i ("nozero") == 1;
    long long pos = 0;
    return specGen (ch, cx, p["win"], outF, [&] (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)
    {
        if (o == 0) pos = clamp (jr (p["start"] * (double) (F - 1)), 0LL, F - 1);
        else
        {
            long long step = 0;
            for (int tries = 0; tries < 64; ++tries) { step = (long long) (cx.rand() * (double) (2 * range)) - range; if (! noZero || step) break; }
            if (noZero && ! step) step = 1;
            pos += step;
            if (F > 1) { while (pos < 0 || pos > F - 1) pos = pos < 0 ? -pos : 2 * (F - 1) - pos; } else pos = 0;
        }
        copyFrame (R, pos, a, fr, g);
    }, p["seed"]);
}

// Shuffles groups of GROUP windows by a domain-image pattern: e.g. ABC-CBA plays each run of three groups backwards,
// ABC-ABBABCC repeats and reorders them. RANDOM draws a new image for every domain.
// after CDP specshuffle (dev/blur/blur.c)
Chans blurShuffle (const Chans& ch, Ctx& cx, const Params& p)
{
    static const char* PAT[] = { "ABC-CBA", "ABCD-DCBA", "ABC-ABBABCC", "AB-AAB", "ABCD-BADC", "ABCD-AACC", "ABCDE-EDCBA", "ABCD-RANDOM" };
    const int pat = clamp (p.i ("pattern"), 0, 7);
    const std::string s = PAT[pat]; const size_t dash = s.find ('-');
    const long long D = (long long) dash, G = std::max (1, p.i ("group"));
    std::vector<long long> img; if (pat != 7) for (size_t i = dash + 1; i < s.size(); ++i) img.push_back (s[i] - 'A');
    else img.assign ((size_t) D, 0);
    const size_t N = fftN (p["win"], cx.sr);
    const long long F = framesOf (ch[0].size(), N >> 2), blocks = std::max (1LL, F / (D * G));
    const long long per = (long long) img.size() * G, outF = blocks * per;
    long long lastBlock = -1;
    return specGen (ch, cx, p["win"], outF, [&] (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)
    {
        if (o == 0) lastBlock = -1;
        const long long b = o / per, r = o % per, slot = r / G, w = r % G;
        if (pat == 7 && b != lastBlock) { for (auto& v : img) v = (long long) (cx.rand() * (double) D); }
        lastBlock = b;
        copyFrame (R, b * D * G + img[(size_t) slot] * G + w, a, fr, g);
    }, p["seed"]);
}

// Weaves through the analysis windows with a repeating list of steps (forwards or backwards), never going back
// before the start, until the end of the sound is reached.
// after CDP specweave (dev/blur/blur.c)
Chans blurWeave (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr);
    const long long F = framesOf (ch[0].size(), N >> 2);
    const long long st[3] = { p.i ("s1"), p.i ("s2"), p.i ("s3") };
    // walk the weave once to know the output length (capped at 4x the source)
    std::vector<long long> path; long long pos = 0;
    for (size_t n = 0; pos < F && (long long) path.size() < 4 * F + 8; ++n) { path.push_back (pos); pos = std::max (0LL, pos + st[n % 3]); }
    return specGen (ch, cx, p["win"], (long long) path.size(), [&] (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)
    { copyFrame (R, path[(size_t) clamp (o, 0LL, (long long) path.size() - 1)], a, fr, g); });
}

// Pulls every channel's amplitude towards the window's mean amplitude: 0 leaves the sound, 1 turns it into
// noise shaped only by its loudness.
// after CDP specnoise (dev/blur/blur.c)
Chans blurNoise (const Chans& ch, Ctx& cx, const Params& p)
{
    const double nz = p["noise"];
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr&)
    {
        const double mean = total (a) / (double) a.size();
        for (double& v : a) v += (mean - v) * nz;
    });
}

// Randomly keeps only KEEP blocks of BLOCK width in every window, deleting the rest: a thinned, sparkling spectrum.
// RANDOM COUNT keeps between 1 and KEEP blocks; NORMALIZE restores each window's level.
// after CDP specscat (dev/blur/blur.c)
Chans blurScatter (const Chans& ch, Ctx& cx, const Params& p)
{
    const bool rnd = p.i ("randcnt") == 1, norm = p.i ("norm") == 1;
    std::vector<uint8_t> keep;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr& in)
    {
        const size_t bs = (size_t) std::max (1LL, jr (p["block"] / in.R.bin)), nb = (a.size() + bs - 1) / bs;
        long long k = std::min<long long> (p.i ("keep"), (long long) nb - 1);
        if (k < 1) k = 1;
        if (rnd) k = std::min (k, (long long) (cx.rand() * (double) k) + 1);
        const double pre = total (a);
        keep.assign (nb, 0);
        for (long long n = 0; n < k; )
        {
            size_t b = std::min (nb - 1, (size_t) (cx.rand() * (double) nb));
            if (! keep[b]) { keep[b] = 1; ++n; }
        }
        for (size_t i = 0; i < a.size(); ++i) if (! keep[i / bs]) a[i] = 0;
        if (norm) normTo (a, pre);
    }, p["seed"]);
}

// Spreads the spectral peaks towards the spectral envelope (resolution DETAIL): SPREAD 1 flattens the partials into
// formant-shaped noisiness, lower values add just a haze.
// after CDP specspread (dev/blur/blur.c)
Chans blurSpread (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sp = p["spread"]; DVec env;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        specEnv (a, in.R.bin, p["detail"], env);
        const double pre = total (a);
        for (size_t k = 0; k < a.size(); ++k) a[k] += (envAt (env, fr[k], in.R.bin) - a[k]) * sp;
        normTo (a, pre);
    });
}

// ---------------------------------------------------------------------------------------------
// focus
// ---------------------------------------------------------------------------------------------

// Spectral accumulation: every channel is sustained until louder data arrives in it; the held data fades by DECAY
// per second and glides by GLIS octaves per second, so the sound leaves a decaying (or glissing) spectral trail.
// after CDP specaccu (dev/focus/focus.c)
Chans focusAccu (const Chans& ch, Ctx& cx, const Params& p)
{
    DVec ha, hf; int lastC = -1;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        if (in.c != lastC) { lastC = in.c; ha.assign (a.size(), 0); hf.assign (a.size(), 0); }
        const double d = std::pow (p["decay"], in.R.ft()), g = std::pow (2.0, p["glis"] * in.R.ft()), nyq = in.R.sr / 2;
        for (size_t k = 0; k < a.size(); ++k)
        {
            ha[k] *= d; hf[k] *= g;
            if (hf[k] > nyq) ha[k] = 0;
            if (a[k] > ha[k]) { ha[k] = a[k]; hf[k] = fr[k]; }
            else { a[k] = ha[k]; fr[k] = hf[k]; }
        }
        in.useOrig = false; in.ph = nullptr;
    });
}

// Exaggerates the spectral contour: each window is normalised to its loudest channel and raised to the power
// 1/EXAG, then restored to its level. Below 1 the troughs deepen and the peaks stand out; above 1 they fill in.
// after CDP specexag (dev/focus/focus.c)
Chans focusExag (const Chans& ch, Ctx& cx, const Params& p)
{
    const double e = 1.0 / p["exag"];
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr&)
    {
        const double pre = total (a); double mx = 0; for (double v : a) mx = std::max (mx, v);
        if (mx <= 0) return;
        for (double& v : a) v = std::pow (v / mx, e);
        normTo (a, pre);
    });
}

// Finds up to PEAKS peaks of the spectral envelope between BOTTOM and TOP and filters the window with peak-centred
// bands BANDWIDTH octaves wide: energy is focused onto the formants. STABLE > 1 keeps the peaks that recur over
// that many windows, so the filters do not flicker.
// after CDP specfocus (dev/focus/focus.c)
Chans focusFocus (const Chans& ch, Ctx& cx, const Params& p)
{
    const int pk = std::max (1, p.i ("peaks")), stable = std::max (1, p.i ("stable"));
    const double hbw = std::max (0.01, p["bw"] / 2), lo = std::min (p["bot"], p["top"]), hi = std::max (p["bot"], p["top"]);
    DVec env, score, gain; std::vector<std::vector<size_t>> hist; int lastC = -1;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double bin = in.R.bin; const size_t n = a.size();
        if (in.c != lastC) { lastC = in.c; score.assign (n, 0); hist.clear(); }
        specEnv (a, bin, 60, env);
        std::vector<size_t> pks;
        for (size_t k = 1; k + 1 < n; ++k)
        {
            const double f = (double) k * bin;
            if (f < lo || f > hi) continue;
            if (env[k] > env[k - 1] && env[k] >= env[k + 1]) pks.push_back (k);
        }
        std::sort (pks.begin(), pks.end(), [&] (size_t x, size_t y) { return env[x] > env[y]; });
        if (pks.size() > (size_t) pk * 2) pks.resize ((size_t) pk * 2);
        // stability: score the peaks of the last STABLE windows (a peak counts for its +-1 bin neighbourhood)
        hist.push_back (pks); for (size_t k : pks) score[k] += 1;
        if ((int) hist.size() > stable) { for (size_t k : hist.front()) score[k] -= 1; hist.erase (hist.begin()); }
        std::vector<size_t> use;
        if (stable > 1)
        {
            std::vector<size_t> cand; for (size_t k = 0; k < n; ++k) if (score[k] > 0) cand.push_back (k);
            std::sort (cand.begin(), cand.end(), [&] (size_t x, size_t y) { return score[x] != score[y] ? score[x] > score[y] : env[x] > env[y]; });
            for (size_t k : cand) { bool near = false; for (size_t u : use) if ((k > u ? k - u : u - k) < 3) near = true; if (! near) use.push_back (k); if ((int) use.size() >= pk) break; }
        }
        else use.assign (pks.begin(), pks.begin() + (long) std::min (pks.size(), (size_t) pk));
        gain.assign (n, 0.0);
        for (size_t u : use)
        {
            const double cf = (double) u * bin;
            for (size_t k = 0; k < n; ++k)
            {
                const double q = std::abs (fr[k]); if (q <= 0) continue;
                const double oc = std::abs (std::log2 (q / cf));
                if (oc < hbw) gain[k] = std::max (gain[k], 0.5 + 0.5 * std::cos (PI * oc / hbw));
            }
        }
        for (size_t k = 0; k < n; ++k) a[k] *= gain[k];
    });
}

// Octave-transposes every component lying outside LOW..HIGH into that range (the loudest wins a channel):
// the whole spectrum is folded into one octave band. FULLER finds a free neighbouring channel instead of losing data.
// after CDP specfold (dev/focus/focus.c)
Chans focusFold (const Chans& ch, Ctx& cx, const Params& p)
{
    const double lo = std::min (p["lo"], p["hi"]), hi = std::max (p["lo"], p["hi"]);
    const bool fuller = p.i ("fuller") == 1;
    DVec na, nf;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double bin = in.R.bin; const long long n = (long long) a.size();
        na = a; nf = fr;
        for (long long k = 0; k < n; ++k)
        {
            double q = std::abs (fr[(size_t) k]);
            if (q < 1) { na[(size_t) k] = 0; continue; }
            if (q >= lo && q <= hi) continue;
            na[(size_t) k] = 0;
            if (q < lo) { while (q < lo) q *= 2; if (q >= hi) continue; }
            else { while (q > hi) q /= 2; if (q <= lo) continue; }
            long long j = jr (q / bin);
            if (j < 1 || j >= n) continue;
            const double m = a[(size_t) k];
            if (m > na[(size_t) j]) { na[(size_t) j] = m; nf[(size_t) j] = q; continue; }
            if (! fuller) continue;
            for (long long d = 1; d <= 4; ++d)
            {
                long long cands[2] = { j - d, j + d };
                bool done = false;
                for (long long c2 : cands) if (c2 >= 1 && c2 < n && m > na[(size_t) c2]) { na[(size_t) c2] = m; nf[(size_t) c2] = q; done = true; break; }
                if (done) break;
            }
        }
        a = na; fr = nf; in.useOrig = false;
    });
}

// Freezes the channel amplitudes, frequencies or both at regular freeze points: every EVERY seconds the window at
// the start (AFTER) or at the end (BEFORE) of a HOLD-long stretch is held across that stretch.
// after CDP specfreeze (dev/focus/focus.c)
Chans focusFreeze (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const bool before = p.i ("use") == 1;
    const double every = p["every"], hold = p["hold"] * every;
    DVec za, zf, zp;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double ts = std::floor (in.t / every) * every;
        if (in.t - ts >= hold) return;
        const long long zfr = jr ((before ? ts + hold : ts) / in.R.ft());
        copyFrame (in.R, zfr, za, zf, &zp);
        if (mode != 1) a = za;
        if (mode != 0) { fr = zf; in.ph = &zp; }
        in.useOrig = false;
    });
}

// Holds the spectrum at COUNT points (from POSITION on, evenly spread) for HOLD seconds each before going on:
// the sound stops and sustains at those instants, and gets longer.
// after CDP specfreeze2 (dev/focus/focus.c)
Chans focusHold (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr), H = N >> 2;
    const long long F = framesOf (ch[0].size(), H), cnt = std::max (1, p.i ("count")), hf = jr (p["dur"] * cx.sr / (double) H);
    std::vector<long long> map;
    long long next = 0;
    for (long long i = 0; i < cnt; ++i)
    {
        const double pos = p["pos"] + (double) i * (1 - p["pos"]) / (double) cnt;
        const long long at = clamp (jr (pos * (double) (F - 1)), next, F - 1);
        for (long long f = next; f < at; ++f) map.push_back (f);
        for (long long k = 0; k < hf; ++k) map.push_back (at);
        next = at;
    }
    for (long long f = next; f < F; ++f) map.push_back (f);
    return specGen (ch, cx, p["win"], (long long) map.size(), [&] (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)
    { copyFrame (R, map[(size_t) clamp (o, 0LL, (long long) map.size() - 1)], a, fr, g); });
}

// Step-frame: every STEP seconds a window is frozen and repeated until the next step, a spectral sample-and-hold.
// after CDP specstep (dev/focus/focus.c)
Chans focusStep (const Chans& ch, Ctx& cx, const Params& p)
{
    DVec za, zf, zp;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const long long S = std::max (2LL, jr (p["step"] / in.R.ft()));
        if (in.f % S == 0) return;
        copyFrame (in.R, (in.f / S) * S, za, zf, &zp);
        a = za; fr = zf; in.useOrig = false; in.ph = &zp;
    });
}

// ---------------------------------------------------------------------------------------------
// strange
// ---------------------------------------------------------------------------------------------

// Linear frequency shift by SHIFT Hz of the whole spectrum, only above or below DIVIDE, or only inside or outside
// LOW..HIGH: harmonic sounds turn inharmonic, metallic and bell-like.
// after CDP specshift (dev/strange/strange.c)
Chans strangeShift (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double sh = p["shift"], dv = p["divide"];
    const double lo = std::min (p["lo"], p["hi"]), hi = std::max (p["lo"], p["hi"]);
    return specMap (ch, cx, p["win"], [&] (DVec&, DVec& fr, Fr&)
    {
        for (double& q : fr)
        {
            const bool on = mode == 0 || (mode == 1 && q > dv) || (mode == 2 && q < dv) || (mode == 3 && q > lo && q < hi) || (mode == 4 && (q < lo || q > hi));
            if (on) q += sh;
        }
    });
}

// Glissandi inside the sound's changing spectral envelope: SHEPARD tones (endlessly rising or falling), an
// INHARMONIC glide of partials HZ STEP apart, or SELF, where the sound's own partials glide in octave-wrapped
// copies. RATE in semitones per second (negative falls); nothing above TOP.
// after CDP specglis (dev/strange/strange.c)
Chans strangeGlis (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double rate = p["rate"];
    DVec env, na, nf; double base = 0; int lastC = -1;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double bin = in.R.bin, nyq = in.R.sr / 2, ref = std::max (20.0, 4 * bin);
        if (in.c != lastC) { lastC = in.c; base = ref; }
        specEnv (a, bin, p["detail"], env);
        const double pre = total (a);
        base *= std::pow (2.0, rate * in.R.ft() / 12);
        const bool up = rate >= 0;
        if (up) while (base > ref * 2) base /= 2; else while (base < ref / 2) base *= 2;
        const double frac = up ? std::log2 (base / ref) : std::log2 (base / (ref / 2));   // 0..1 fade of the odd partials
        const size_t n = a.size();
        na.assign (n, 0); nf = fr;
        auto place = [&] (double q, double m) { long long j = jr (q / bin); if (j >= 1 && j < (long long) n && m > na[(size_t) j]) { na[(size_t) j] = m; nf[(size_t) j] = q; } };
        if (mode == 2)
        {   // self-glissando
            const double t = base / ref;
            for (size_t k = 0; k < n; ++k)
            {
                const double q0 = std::abs (fr[k]), e0 = envAt (env, q0, bin);
                if (e0 <= 1e-12 || a[k] <= 0) continue;
                const double ratio = std::min (20.0, a[k] / e0), q = q0 * t;
                if (up)
                {
                    if (q < nyq) place (q, envAt (env, q, bin) * ratio * std::sqrt (std::max (0.0, 2 - t)));
                    place (q / 2, envAt (env, q / 2, bin) * ratio * std::sqrt (std::max (0.0, t - 1)));
                }
                else
                {
                    if (q < nyq) place (q, envAt (env, q, bin) * ratio * std::sqrt (std::max (0.0, 2 * t - 1)));
                    if (q * 2 < nyq) place (q * 2, envAt (env, q * 2, bin) * ratio * std::sqrt (std::max (0.0, 2 - 2 * t)));
                }
            }
        }
        else
        {
            const double fund = up ? base / 2 : base, step = up ? p["hzstep"] / 2 : p["hzstep"];
            double q = up ? base : base; int h = up ? 2 : 1;
            for (int guard = 0; q < nyq && guard < 100000; ++guard)
            {
                double m = envAt (env, q, bin);
                if ((h & 1) || (up && h == 2)) m *= frac;
                place (q, m);
                ++h;
                q = mode == 0 ? fund * (double) h : q + step;
            }
        }
        const double top = p["top"], roll = std::min (nyq - top, 500.0);
        for (size_t k = 0; k < n; ++k)
            if (nf[k] > top) na[k] = (roll <= 0 || nf[k] >= top + roll) ? 0 : na[k] * (top + roll - nf[k]) / roll;
        normTo (na, pre);
        a = na; fr = nf; in.useOrig = false; in.ph = nullptr;
    });
}

// Oscillates VIB RATE times per second between the harmonic spectrum and a stretched, inharmonic one: channels above
// BOTTOM are stretched by up to STRETCH (growing with frequency; EXPONENT shapes the curve in mode 2).
// after CDP specwaver (dev/strange/strange.c)
Chans strangeWaver (const Chans& ch, Ctx& cx, const Params& p)
{
    const bool spec = p.i ("mode") == 1;
    return specMap (ch, cx, p["win"], [&] (DVec&, DVec& fr, Fr& in)
    {
        const double ph = p["vib"] * in.t, tri = 1 - std::abs (2 * (ph - std::floor (ph)) - 1);   // 0 harmonic .. 1 stretched
        const double s = 1 + (p["stretch"] - 1) * tri;
        const long long half = (long long) fr.size() - 1, b0 = clamp (jr (p["bot"] / in.R.bin), 0LL, half - 1);
        for (long long k = b0; k <= half; ++k)
        {
            double loc = (double) (k - b0) / (double) (half - b0);
            if (spec) loc = std::pow (loc, p["expon"]);
            fr[(size_t) k] *= 1 + loc * (s - 1);
        }
    });
}

// Makes an approximate copy of the sound: at the turning points of its pitch contour (found by scanning SCAN ms)
// the pitch is randomly displaced by up to PITCH semitones and the time by up to TIME ms; pitch and timing are
// warped smoothly between them.
// after CDP specwarp: warp_pitch / warp_time (dev/strange/strange.c)
Chans strangeWarp (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr), H = N >> 2;
    const long long F = framesOf (ch[0].size(), H);
    const double ft = (double) H / cx.sr;
    auto f0 = framePitch (ch, cx, H, F);
    // midi contour with unvoiced gaps filled by the nearest pitch
    std::vector<double> m ((size_t) F, -1);
    for (long long i = 0; i < F; ++i) if (f0[(size_t) i] > 0) m[(size_t) i] = midiOf (f0[(size_t) i]);
    { double last = -1; for (auto& v : m) { if (v >= 0) last = v; else v = last; } last = -1; for (auto it = m.rbegin(); it != m.rend(); ++it) { if (*it >= 0) last = *it; else *it = last; } }
    const long long scan = std::max (2LL, jr (p["scan"] / 1000 / ft));
    // turning points: where the contour, scanned over SCAN, changes direction by at least a semitone
    std::vector<long long> tp { 0 };
    if (m[0] >= 0)
    {
        int dir = 0; long long ext = 0;
        for (long long i = scan; i < F; i += scan)
        {
            const double d = m[(size_t) i] - m[(size_t) ext];
            const int nd = d > 1 ? 1 : d < -1 ? -1 : 0;
            if (nd == 0) continue;
            if (dir != 0 && nd != dir && ext > tp.back()) tp.push_back (ext);
            if (nd != dir || (nd > 0 ? m[(size_t) i] > m[(size_t) ext] : m[(size_t) i] < m[(size_t) ext])) ext = i;
            dir = nd;
        }
    }
    if ((long long) tp.size() < 3) { tp.assign (1, 0); for (long long i = scan * 2; i < F - 1; i += scan * 2) tp.push_back (i); }   // no contour: regular points
    tp.push_back (F - 1);
    cx.seed (p["seed"]);
    const long long tr = jr (p["trange"] / 1000 / ft);
    std::vector<double> po (tp.size()), to (tp.size());
    for (size_t i = 0; i < tp.size(); ++i)
    {
        po[i] = (cx.rand() * 2 - 1) * p["prange"];
        to[i] = (double) tp[i];
        if (i > 0 && i + 1 < tp.size()) to[i] = clampd ((double) tp[i] + (cx.rand() * 2 - 1) * (double) tr, (double) tp[i - 1] + 1, (double) tp[i + 1] - 1);
    }
    for (size_t i = 1; i < to.size(); ++i) to[i] = std::max (to[i], to[i - 1]);
    // output frame o -> source position and transposition
    std::vector<double> src ((size_t) F), tsp ((size_t) F);
    for (long long o = 0; o < F; ++o)
    {
        size_t i = 0; while (i + 2 < to.size() && (double) o > to[i + 1]) ++i;
        const double span = std::max (1e-9, to[i + 1] - to[i]), u = clampd (((double) o - to[i]) / span, 0, 1);
        src[(size_t) o] = (double) tp[i] + u * (double) (tp[i + 1] - tp[i]);
        tsp[(size_t) o] = std::pow (2.0, (po[i] + u * (po[i + 1] - po[i])) / 12);
    }
    DVec a2, f2;
    return specGen (ch, cx, p["win"], F, [&] (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)
    {
        const double s = src[(size_t) clamp (o, 0LL, F - 1)]; const long long i0 = (long long) std::floor (s); const double u = s - (double) i0;
        copyFrame (R, u < 0.5 ? i0 : i0 + 1, a, fr, g);
        copyFrame (R, i0, a, fr);
        if (u > 0) { copyFrame (R, i0 + 1, a2, f2); for (size_t k = 0; k < a.size(); ++k) { a[k] += (a2[k] - a[k]) * u; fr[k] += (f2[k] - fr[k]) * u; } }
        g.orig = fr; g.useOrig = true;
        const double t = tsp[(size_t) clamp (o, 0LL, F - 1)];
        for (double& q : fr) q *= t;
    });
}

// Inverts the spectrum's amplitudes: each channel becomes its loudest level in the whole sound minus its current
// level, so what was quiet sounds loud. KEEP LEVEL inverts each channel's share of the window instead and keeps
// the source's loudness envelope.
// after CDP specinvert (dev/strange/strange.c)
Chans strangeInvert (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const size_t N = fftN (p["win"], cx.sr);
    std::vector<DVec> ref;
    for (auto& x : ch)
    {
        SpecReader R (x, cx.sr, N); DVec r (R.half + 1, 0.0);
        for (long long f = 0; f < R.frames; ++f)
        {
            const auto& s = R.get (f); const double tot = total (s.a);
            if (mode == 0) { for (size_t k = 0; k <= R.half; ++k) r[k] = std::max (r[k], s.a[k]); }
            else if (tot > 1e-9) { for (size_t k = 0; k <= R.half; ++k) r[k] = std::max (r[k], s.a[k] / tot); }
            if (! (f & 31)) cx.prog (0.3 * (double) f / (double) R.frames);
        }
        ref.push_back (std::move (r));
    }
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr& in)
    {
        const DVec& r = ref[(size_t) in.c];
        if (mode == 0) { for (size_t k = 0; k < a.size(); ++k) a[k] = r[k] - a[k]; return; }
        const double tot = total (a); if (tot <= 1e-9) return;
        for (size_t k = 0; k < a.size(); ++k) a[k] = (r[k] - a[k] / tot) * tot;
    });
}

// ---------------------------------------------------------------------------------------------
// stretch, pvoc
// ---------------------------------------------------------------------------------------------

// Stretches the partial frequencies above (or below) DIVIDE by a ratio that grows towards the top (bottom) of the
// spectrum up to MAX STRETCH, along a curve set by EXPONENT; DEPTH scales the effect. Harmonic sounds go inharmonic.
// after CDP specstretch (dev/stretch/stretch.c)
Chans stretchSpectrum (const Chans& ch, Ctx& cx, const Params& p)
{
    const bool below = p.i ("mode") == 1; const double dv = p["divide"], ex = p["expon"];
    const double sh = (p["max"] - 1) * p["depth"] + 1;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double nyq = in.R.sr / 2;
        for (size_t k = 0; k < fr.size(); ++k)
        {
            const double q = std::abs (fr[k]);
            if (! below && q > dv && nyq > dv) { const double loc = std::min (1.0, (q - dv) / (nyq - dv)); fr[k] = q * (1 + std::pow (loc, ex) * (sh - 1)); }
            else if (below && q < dv && dv > 0) { const double loc = q / dv; fr[k] = q * (sh + std::pow (loc, ex) * (1 - sh)); }
            if (fr[k] >= nyq) a[k] = 0;
        }
    });
}

// Phase-vocoder time stretch without pitch change: STRETCH sweeps from START to END along the source (values below
// 1 compress); windows are interpolated in amplitude and frequency.
// after CDP spectstretch (dev/stretch/stretch.c)
Chans stretchTime (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr), H = N >> 2;
    const long long F = framesOf (ch[0].size(), H), maxF = (long long) (cx.maxLen / H) + 2;
    const double s0 = p["start"], s1 = p["end"];
    std::vector<double> pos;
    for (double x = 0; x <= (double) (F - 1) && (long long) pos.size() < maxF; )
    {
        pos.push_back (x);
        const double u = x / (double) std::max (1LL, F - 1), s = s0 * std::pow (s1 / s0, u);
        x += 1.0 / std::max (0.01, s);
    }
    DVec a2, f2;
    return specGen (ch, cx, p["win"], (long long) pos.size(), [&] (long long o, DVec& a, DVec& fr, SpecReader& R, GenOut& g)
    {
        const double s = pos[(size_t) clamp (o, 0LL, (long long) pos.size() - 1)]; const long long i0 = (long long) std::floor (s); const double u = s - (double) i0;
        copyFrame (R, u < 0.5 ? i0 : i0 + 1, a, fr, g);
        copyFrame (R, i0, a, fr);
        if (u > 0) { copyFrame (R, i0 + 1, a2, f2); for (size_t k = 0; k < a.size(); ++k) { a[k] += (a2[k] - a[k]) * u; fr[k] += (f2[k] - fr[k]) * u; } }
    });
}

// Analyses and resynthesises keeping only the ODD or EVEN analysis channels (or all) between LOW and HIGH:
// hollow, comb-filtered timbres.
// after CDP pvoc extract: pvoc_process (dev/pv/pvoc.c)
Chans pvocExtract (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double lo = std::min (p["lo"], p["hi"]), hi = std::max (p["lo"], p["hi"]);
    const size_t len = ch[0].size();
    return stftRun (ch, cx, p["win"], std::min (len, cx.maxLen), [&] (DVec& mag, DVec&, St& st, long long, long long)
    {
        const double bin = cx.sr / (double) st.N;
        for (size_t k = 0; k <= st.half; ++k)
        {
            const double f = (double) k * bin;
            const bool keep = f >= lo && f <= hi && (mode == 0 || (mode == 1 && (k & 1)) || (mode == 2 && ! (k & 1)));
            if (! keep) mag[k] = 0;
        }
    });
}

// ---------------------------------------------------------------------------------------------
// pitch
// ---------------------------------------------------------------------------------------------
const double CHORDS[][6] = { { 0, 4, 7, 99 }, { 0, 3, 7, 99 }, { -12, 0, 12, 99 }, { 0, 7, 12, 99 }, { 0, 5, 7, 99 }, { 0, 4, 7, 11, 99 }, { 0, 3, 7, 10, 99 },
                             { 0, 4, 7, 10, 14, 99 }, { 0, 2, 4, 6, 8, 99 }, { 0, 1, 2, 3, 99 }, { -12, -5, 0, 99 } };
const defs::O CHORD_NAMES { "MAJOR", "MINOR", "OCTAVES", "FIFTHS", "SUS4", "MAJ7", "MIN7", "DOM9", "WHOLE TONE", "CLUSTER", "LOW STACK" };
std::vector<double> chordOf (int idx) { std::vector<double> c; for (double v : CHORDS[clamp (idx, 0, 10)]) { if (v > 98) break; c.push_back (v); } return c; }

// harmonic number of q over pitch, or 0 if q is further than tol from it
inline long long harmOf (double q, double pitch, double tol) { const long long n = jr (q / pitch); return n >= 1 && std::abs (q - (double) n * pitch) <= tol ? n : 0; }

// Deletes the odd (mode 1: the octave above, formants unchanged) or even harmonics of the tracked pitch; everything
// off the harmonics goes too, as in CDP (which bares the spectrum first). Unpitched windows pass unchanged.
// after CDP specalt (dev/pitch/pitch.c)
Chans pitchAltharms (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr);
    const auto f0 = framePitch (ch, cx, N >> 2, framesOf (ch[0].size(), N >> 2));
    const bool odd = p.i ("mode") == 0, less = p.i ("body") == 1;
    std::vector<int> best;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double pitch = f0[(size_t) clamp (in.f, 0LL, (long long) f0.size() - 1)];
        if (pitch <= 0) return;
        const double tol = std::max (0.25 * pitch, 1.5 * in.R.bin);
        best.assign ((size_t) (in.R.sr / 2 / pitch) + 3, -1);
        for (size_t k = 0; k < a.size(); ++k)
        {
            const long long n = harmOf (std::abs (fr[k]), pitch, tol);
            if (! n || ((n & 1) == (odd ? 1 : 0))) { a[k] = 0; continue; }
            if (less)
            {
                int& b = best[(size_t) std::min<long long> (n, (long long) best.size() - 1)];
                if (b < 0 || a[k] > a[(size_t) b]) { if (b >= 0) a[(size_t) b] = 0; b = (int) k; } else a[k] = 0;
            }
        }
    });
}

// envelope through the harmonic peaks of one window (CDP extract_specenv_over_partials)
void partialEnv (const DVec& a, const DVec& fr, double pitch, double bin, std::vector<double>& pk)
{
    const size_t nh = (size_t) std::max (1.0, (double) (a.size() - 1) * bin / pitch) + 2;
    pk.assign (nh, 0.0);
    for (size_t k = 0; k < a.size(); ++k) { const long long n = jr (std::abs (fr[k]) / pitch); if (n >= 1 && (size_t) n < nh) pk[(size_t) n] = std::max (pk[(size_t) n], a[k]); }
    pk[0] = pk[1];
}
inline double partialEnvAt (const std::vector<double>& pk, double q, double pitch) { return lerpAt (pk, clampd (q / pitch, 0, (double) pk.size() - 1.001)); }

// Octave (harmonic-ratio) transposition without formant shift, using the tracked pitch. UP keeps only every
// TRANSPOSITION-th harmonic; DOWN inserts the missing harmonics of f0/TRANSPOSITION under the formant envelope;
// DOWN + BASS also reinforces the new bass by BASS BOOST.
// after CDP specoct (dev/pitch/pitch.c)
Chans pitchOctmove (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr);
    const auto f0 = framePitch (ch, cx, N >> 2, framesOf (ch[0].size(), N >> 2));
    const int mode = p.i ("mode"); const long long T = std::max (2, p.i ("transp"));
    std::vector<double> pk;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double pitch = f0[(size_t) clamp (in.f, 0LL, (long long) f0.size() - 1)], bin = in.R.bin, nyq = in.R.sr / 2;
        if (pitch <= 0) return;
        const double pre = total (a);
        if (mode == 0)
        {
            const double np = pitch * (double) T, tol = std::max (np * 0.0595, 1.5 * bin);
            for (size_t k = 0; k < a.size(); ++k) if (! harmOf (std::abs (fr[k]), np, tol)) a[k] = 0;
        }
        else
        {
            partialEnv (a, fr, pitch, bin, pk);
            const double base = pitch / (double) T;
            for (long long n = 1; base * (double) n < nyq; ++n)
            {
                if (n % T == 0) continue;
                const double q = base * (double) n; const long long j = jr (q / bin);
                if (j < 1 || j >= (long long) a.size()) continue;
                double m = partialEnvAt (pk, q, pitch);
                if (mode == 2 && q < pitch) m *= p["boost"] - (p["boost"] - 1) * ((double) n / (double) T);
                a[(size_t) j] = m; fr[(size_t) j] = q;
            }
            in.useOrig = false; in.ph = nullptr;
        }
        normTo (a, pre);
    });
}

// Transposes the spectrum above and/or below SPLIT: by an octave (modes 1-3) or by UP / DOWN semitones (4-6),
// with DEPTH blending the transposition in.
// after CDP specshiftp (dev/pitch/pitch.c)
Chans pitchTransp (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double split = p["split"], d = p["depth"];
    double ru = 1, rd = 1;
    if (mode == 0 || mode == 2) ru = 2;
    if (mode == 1 || mode == 2) rd = 0.5;
    if (mode == 3 || mode == 5) ru = std::pow (2.0, p["up"] / 12);
    if (mode == 4 || mode == 5) rd = std::pow (2.0, p["down"] / 12);
    ru = 1 + (ru - 1) * d; rd = 1 + (rd - 1) * d;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double nyq = in.R.sr / 2;
        for (size_t k = 0; k < fr.size(); ++k)
        {
            const double q = std::abs (fr[k]);
            fr[k] = q * (q >= split ? ru : rd);
            if (fr[k] >= nyq) a[k] = 0;
        }
    });
}

// Pulls the spectrum onto the harmonics of a chord template (ROOT + CHORD): channels near a template frequency are
// focused onto it by FOCUS, the others are suppressed by CLARITY; TRACE moves the N loudest channels onto the nearest
// template frequency; nothing below BASS CUT.
// after CDP spectune (dev/pitch/pitch.c)
Chans pitchTune (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr), half = N >> 1; const double bin = cx.sr / (double) N, nyq = cx.sr / 2;
    // template: every harmonic of every chord note, each assigned to the bins within +-1.5 of it
    DVec tmpl (half + 1, -1.0);
    for (double iv : chordOf (p.i ("chord")))
    {
        const double f = hzOf (p["note"] + iv);
        for (double h = f; h < nyq; h += f)
            for (long long k = jr (h / bin - 1.5); k <= jr (h / bin + 1.5); ++k)
                if (k >= 0 && k <= (long long) half && (tmpl[(size_t) k] < 0 || std::abs (h - (double) k * bin) < std::abs (tmpl[(size_t) k] - (double) k * bin))) tmpl[(size_t) k] = h;
    }
    const double foc = p["focus"], keep = 1 - p["clarity"], bcut = p["bcut"]; const int trace = p.i ("trace");
    std::vector<size_t> idx;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        for (size_t k = 0; k < a.size(); ++k) if (std::abs (fr[k]) < bcut) a[k] = 0;
        if (trace > 0)
        {
            idx.resize (a.size()); for (size_t k = 0; k < a.size(); ++k) idx[k] = k;
            const size_t m = std::min ((size_t) trace, a.size());
            std::partial_sort (idx.begin(), idx.begin() + (long) m, idx.end(), [&] (size_t x, size_t y) { return a[x] > a[y]; });
            std::vector<uint8_t> loud (a.size(), 0); for (size_t i = 0; i < m; ++i) loud[idx[i]] = 1;
            for (size_t k = 0; k < a.size(); ++k)
            {
                if (! loud[k]) { a[k] *= keep; continue; }
                if (tmpl[k] > 0) { fr[k] += (tmpl[k] - fr[k]) * foc; continue; }
                long long lo = (long long) k, hi = (long long) k;
                while (lo >= 0 && tmpl[(size_t) lo] <= 0) --lo;
                while (hi < (long long) a.size() && tmpl[(size_t) hi] <= 0) ++hi;
                long long t = lo < 0 ? hi : hi >= (long long) a.size() ? lo : ((double) hi - (double) k < (double) k - (double) lo ? hi : lo);
                if (t < 0 || t >= (long long) a.size()) continue;
                a[(size_t) t] = std::max (a[(size_t) t], a[k]); fr[(size_t) t] = fr[(size_t) t] + (tmpl[(size_t) t] - fr[(size_t) t]) * foc; a[k] = 0;
            }
            in.useOrig = false;
        }
        else
            for (size_t k = 0; k < a.size(); ++k)
            {
                if (tmpl[k] > 0) fr[k] += (tmpl[k] - fr[k]) * foc;
                else a[k] *= keep;
            }
    });
}

// Keeps only the channels that could hold the chosen partials of FUNDAMENTAL: the harmonic series, octaves, odd
// harmonics, a linear series STEP Hz apart, or harmonics displaced by STEP. CLARITY sets how far the rest is cut.
// after CDP specpick (dev/pitch/pitch.c)
Chans pitchPick (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr), half = N >> 1; const double bin = cx.sr / (double) N, nyq = cx.sr / 2;
    const int mode = p.i ("mode"); const double fund = p["fund"], step = std::max (1.0, p["step"]);
    std::vector<uint8_t> mark (half + 1, 0);
    double q = fund; long long n = 1;
    for (int guard = 0; q < nyq && guard < 200000; ++guard)
    {
        if (q > 0) { const long long k = jr (q / bin); for (long long j = k - 1; j <= k + 1; ++j) if (j >= 0 && j <= (long long) half) mark[(size_t) j] = 1; }
        switch (mode)
        {
            case 0: ++n; q = fund * (double) n; break;
            case 1: q *= 2; break;
            case 2: n += 2; q = fund * (double) n; break;
            case 3: q += step; break;
            default: ++n; q = fund * (double) n + step; break;
        }
    }
    const double g = std::pow (1 - p["clarity"], 2);
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec&, Fr&) { for (size_t k = 0; k < a.size(); ++k) if (! mark[k]) a[k] *= g; });
}

// chord of transposed copies; withEnv keeps each copy inside the original spectral envelope (chordf)
Chans chordImpl (const Chans& ch, Ctx& cx, const Params& p, bool withEnv)
{
    std::vector<double> tr; for (double iv : chordOf (p.i ("chord"))) tr.push_back (std::pow (2.0, (iv + p["shift"]) / 12));
    const double lo = std::min (p["lo"], p["hi"]), hi = std::max (p["lo"], p["hi"]); const bool fuller = p.i ("fuller") == 1;
    DVec env, na, nf;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double bin = in.R.bin, nyq = in.R.sr / 2; const long long n = (long long) a.size();
        if (withEnv) specEnv (a, bin, p["detail"], env);
        const double pre = total (a);
        na.assign (a.size(), 0); nf = fr;
        for (long long k = 0; k < n; ++k)
        {
            const double q0 = std::abs (fr[(size_t) k]), m0 = a[(size_t) k];
            if (m0 <= 0 || q0 <= 0) continue;
            const double e0 = withEnv ? envAt (env, q0, bin) : 1;
            if (e0 < 1e-12) continue;
            const double off = clampd ((double) k - q0 / bin, -2, 2);
            for (double t : tr)
            {
                const double q = q0 * t; if (q >= nyq) continue;
                const double m = withEnv ? m0 * envAt (env, q, bin) / e0 : m0;
                const long long j = jr (q / bin + off);
                if (j < 1 || j >= n) continue;
                if (m > na[(size_t) j]) { na[(size_t) j] = m; nf[(size_t) j] = q; continue; }
                if (fuller) for (long long d2 : { j - 1, j + 1 }) if (d2 >= 1 && d2 < n && m > na[(size_t) d2]) { na[(size_t) d2] = m; nf[(size_t) d2] = q; break; }
            }
        }
        for (long long k = 0; k < n; ++k) if (nf[(size_t) k] < lo || nf[(size_t) k] > hi) na[(size_t) k] = 0;
        normTo (na, pre);
        a = na; fr = nf; in.useOrig = false; in.ph = nullptr;
    });
}
// Superimposes transposed copies of the spectrum (a CHORD, moved by SHIFT) inside the original spectral envelope,
// so the formants stay put; LOW/HIGH filter the result, FULLER keeps more data.
// after CDP specchord (dev/pitch/pitch.c)
Chans pitchChordf (const Chans& ch, Ctx& cx, const Params& p) { return chordImpl (ch, cx, p, true); }
// Superimposes transposed copies of the spectrum (a CHORD, moved by SHIFT) onto the original; LOW/HIGH filter it.
// after CDP specchord2 (dev/pitch/pitch.c)
Chans pitchChord (const Chans& ch, Ctx& cx, const Params& p) { return chordImpl (ch, cx, p, false); }

// ---------------------------------------------------------------------------------------------
// repitch
// ---------------------------------------------------------------------------------------------

// transposition of one window by ratio r, optionally inside the window's own spectral envelope (CDP spectrnsf)
void transposeWin (DVec& a, DVec& fr, double r, double bin, double nyq, const DVec* env)
{
    for (size_t k = 0; k < a.size(); ++k)
    {
        const double q0 = std::abs (fr[k]), q = q0 * r;
        fr[k] = q;
        if (q >= nyq) { a[k] = 0; continue; }
        if (env)
        {
            const double e0 = envAt (*env, q0, bin), e1 = envAt (*env, q, bin);
            a[k] = (e0 < 1e-12 || e1 < 1e-12) ? 0 : a[k] * e1 / e0;
        }
    }
}
Chans transposeImpl (const Chans& ch, Ctx& cx, const Params& p, bool formants)
{
    const double s0 = p["start"], s1 = p["end"], len = (double) std::max<size_t> (1, ch[0].size()) / cx.sr;
    const double lo = std::min (p["lo"], p["hi"]), hi = std::max (p["lo"], p["hi"]);
    DVec env;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double r = std::pow (2.0, (s0 + (s1 - s0) * clampd (in.t / len, 0, 1)) / 12), pre = total (a);
        if (formants) specEnv (a, in.R.bin, p["detail"], env);
        transposeWin (a, fr, r, in.R.bin, in.R.sr / 2, formants ? &env : nullptr);
        for (size_t k = 0; k < a.size(); ++k) if (fr[k] < lo || fr[k] > hi) a[k] = 0;
        normTo (a, pre);
    });
}
// Transposes the whole spectrum, formants included (chipmunk / giant), by START semitones gliding to END;
// data outside LOW..HIGH is removed.
// after CDP spectrnsp (dev/repitch/repitch.c)
Chans repitchTranspose (const Chans& ch, Ctx& cx, const Params& p) { return transposeImpl (ch, cx, p, false); }
// Transposes the partials but keeps the original spectral envelope (formants, resolution DETAIL): a natural-sounding
// pitch shift from START to END semitones; data outside LOW..HIGH is removed.
// after CDP spectrnsf (dev/repitch/repitch.c)
Chans repitchTransposef (const Chans& ch, Ctx& cx, const Params& p) { return transposeImpl (ch, cx, p, true); }

// per-frame f0 with unvoiced windows resolved: hold = last (or next) pitched value, otherwise 0
std::vector<double> heldPitch (std::vector<double> f0, bool hold)
{
    if (! hold) return f0;
    double last = 0; for (auto& v : f0) { if (v > 0) last = v; else v = last; }
    last = 0; for (auto it = f0.rbegin(); it != f0.rend(); ++it) { if (*it > 0) last = *it; else *it = last; }
    return f0;
}
// noise window for unpitched sound (CDP generate_tone: random frequencies inside each channel, random levels)
void noiseWin (DVec& a, DVec& fr, double bin, Ctx& cx)
{
    for (size_t k = 1; k < a.size(); ++k) { fr[k] = ((double) k + cx.rand() - 0.5) * bin; a[k] = 0.2 + 0.8 * cx.rand(); }
    a[0] = 0;
}

// Resynthesises the sound as a tone following its own pitch contour, with harmonic levels H1..H8; unpitched parts
// become NOISE, HOLD the last pitch, or SILENCE. Each window keeps the source's level.
// after CDP repitch synth: generate_tone (dev/repitch/repitch.c)
Chans repitchSynth (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr);
    const int unv = p.i ("unvoiced");
    const auto f0 = heldPitch (framePitch (ch, cx, N >> 2, framesOf (ch[0].size(), N >> 2)), unv == 1);
    double hv[8]; for (int i = 0; i < 8; ++i) hv[i] = p["h" + std::to_string (i + 1)];
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double pre = total (a), pitch = f0[(size_t) clamp (in.f, 0LL, (long long) f0.size() - 1)], bin = in.R.bin;
        in.useOrig = false; in.ph = nullptr;
        if (pitch <= 0)
        {
            if (unv == 0 && pre > 1e-9) { noiseWin (a, fr, bin, cx); normTo (a, pre * 0.5); }
            else std::fill (a.begin(), a.end(), 0.0);
            return;
        }
        std::fill (a.begin(), a.end(), 0.0);
        for (int h = 0; h < 8; ++h)
        {
            const double q = pitch * (h + 1); const long long j = jr (q / bin);
            if (j < 1 || j >= (long long) a.size() || hv[h] <= 0) continue;
            a[(size_t) j] = hv[h]; fr[(size_t) j] = q;
        }
        normTo (a, pre);
    }, p["seed"]);
}

struct Vowel { double f1, f2, f3, a2, a3; };
const Vowel VOWELS[] = { { 300, 2600, 2800, .85, .85 }, { 360, 2100, 2300, 1, 1 }, { 465, 2250, 2450, .92, .25 }, { 500, 2250, 2450, .92, .25 }, { 570, 1970, 2450, 1, .75 },
    { 750, 1550, 2450, 1, .85 }, { 680, 1100, 2450, 1, .92 }, { 600, 900, 2450, 1, .5 }, { 450, 860, 2300, 1, .82 }, { 410, 810, 2300, 1, .82 }, { 380, 850, 2100, 1, .6 },
    { 380, 850, 2800, .85, .85 }, { 380, 1000, 2450, .92, .25 }, { 300, 850, 2100, 1, .6 }, { 720, 1240, 2300, .85, .3 }, { 580, 1380, 2450, 1, .5 },
    { 580, 1380, 2625, 0, .1 }, { 580, 850, 2100, 0, 0 }, { 880, 2100, 2450, .2, 0 }, { 400, 1000, 2450, .3, .2 } };
const defs::O VOWEL_NAMES { "EE (HEAT)", "I (HIT)", "AI (HATE)", "AII", "E (PET)", "A (PAT)", "AR (HEART)", "O (HOT)", "OR", "OA (LOAD)", "U (HOOD)", "UU", "UI", "OO (MOOD)", "XX (HUB)", "X (HERB)", "N", "M", "R", "TH" };
// ear-sensitivity compensation of CDP define_sensitivity_curve
inline double sensitivity (double f)
{
    static const double T[][2] = { { 0, 1 }, { 250, 1 }, { 2000, 1 / 2.511 }, { 3000, 1 / 2.511 }, { 4000, 0.4 / 2.511 }, { 1e9, 0.4 / 2.511 } };
    size_t i = 1; while (i < 5 && f > T[i][0]) ++i;
    const double u = clampd ((f - T[i - 1][0]) / (T[i][0] - T[i - 1][0]), 0, 1);
    return T[i - 1][1] + (T[i][1] - T[i - 1][1]) * u;
}

// Resynthesises the sound as a sung vowel following its own pitch: harmonics are shaped by three formant bands
// (HALFWIDTH as a fraction of each formant, CURVE = steepness, PEAK RANGE = formant contrast, FUND = level of the
// fundamental, OFFSET = random detuning of the harmonics). The vowel glides from VOWEL to TO VOWEL.
// after CDP repitch vowels: generate_vowels (dev/repitch/repitch.c)
Chans repitchVowels (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr);
    const auto f0 = heldPitch (framePitch (ch, cx, N >> 2, framesOf (ch[0].size(), N >> 2)), true);
    const Vowel v0 = VOWELS[clamp (p.i ("vowel"), 0, 19)], v1 = VOWELS[clamp (p.i ("vowel2"), 0, 19)];
    const double hw = p["hwidth"], curve = p["curve"], pkr = p["pkrange"], fw = p["fweight"], off = p["offset"];
    const double len = (double) std::max<size_t> (1, ch[0].size()) / cx.sr;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double pre = total (a), pitch = f0[(size_t) clamp (in.f, 0LL, (long long) f0.size() - 1)], bin = in.R.bin, nyq = in.R.sr / 2;
        in.useOrig = false; in.ph = nullptr;
        std::fill (a.begin(), a.end(), 0.0);
        if (pitch <= 0 || pre <= 1e-9) return;
        const double u = clampd (in.t / len, 0, 1);
        const double F[3] = { v0.f1 + (v1.f1 - v0.f1) * u, v0.f2 + (v1.f2 - v0.f2) * u, v0.f3 + (v1.f3 - v0.f3) * u };
        const double AT[3] = { 1, v0.a2 + (v1.a2 - v0.a2) * u, v0.a3 + (v1.a3 - v0.a3) * u };
        const double top = F[2] * (1 + hw);
        double harm = pitch, q = pitch;
        for (int guard = 0; q < top && q < nyq && guard < 4000; ++guard)
        {
            double amp = 0;
            for (int i = 0; i < 3; ++i)
            {
                const double h = F[i] * hw;
                if (q > F[i] - h && q < F[i] + h) amp = std::max (amp, (1 - std::abs (q - F[i]) / h) * AT[i]);
            }
            if (amp <= 0 && q < F[0] * (1 - hw) && std::abs (q - pitch) < 1e-6) amp = fw;
            amp = std::pow (std::max (0.0, amp), curve) * pkr + (1 - pkr);
            amp *= sensitivity (q);
            const long long j = jr (q / bin);
            if (j >= 1 && j < (long long) a.size() && amp > a[(size_t) j]) { a[(size_t) j] = amp; fr[(size_t) j] = q; }
            harm += pitch;
            q = harm + (off > 0 ? (cx.rand() - 0.5) * off * pitch : 0.0);
        }
        normTo (a, pre);
    }, p["seed"]);
}

// Formant-preserving melodic transformations of a monophonic sound: its pitch contour is tracked, transformed
// (EXAGGERATE intervals by FACTOR and CONTOUR around the mean, INVERT around the mean, QUANTISE to SCALE in KEY,
// RANDOMISE by up to RANGE every TIME, SMOOTH over TIME, VIBRATO of RANGE at RATE, APPROX random copy) and the
// sound is retuned to the new contour inside its own spectral envelope.
// after CDP repitch getpitch -> exag / invert / quantise / randomise / smooth / vibrato / approx -> transposef
// (dev/repitch/repitch.c: specpitch, specpexag, specpinvert, specpquantise, specprand, specpsmooth, specpvib, specpapprox, spectrnsf)
Chans repitchContour (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t N = fftN (p["win"], cx.sr), H = N >> 2;
    const long long F = framesOf (ch[0].size(), H);
    const double ft = (double) H / cx.sr;
    const int mode = p.i ("mode");
    auto f0 = framePitch (ch, cx, H, F, 220);
    std::vector<double> m ((size_t) F, 0.0), n ((size_t) F, 0.0);
    double mean = 0, mn = 1e9, mx = -1e9; int cnt = 0;
    for (long long i = 0; i < F; ++i) if (f0[(size_t) i] > 0) { m[(size_t) i] = midiOf (f0[(size_t) i]); mean += m[(size_t) i]; ++cnt; mn = std::min (mn, m[(size_t) i]); mx = std::max (mx, m[(size_t) i]); }
    mean = cnt ? mean / cnt : 57;
    cx.seed (p["seed"]);
    const double range = p["range"], tf = std::max (ft, p["time"] / 1000);
    // offsets over time for the random / smoothing modes: values at block points, interpolated between them
    auto interp = [&] (const std::vector<std::pair<double, double>>& pts, double t)
    {
        if (pts.empty()) return 0.0;
        if (t <= pts.front().first) return pts.front().second;
        for (size_t i = 1; i < pts.size(); ++i) if (t <= pts[i].first) { const double u = (t - pts[i - 1].first) / std::max (1e-9, pts[i].first - pts[i - 1].first); return pts[i - 1].second + (pts[i].second - pts[i - 1].second) * u; }
        return pts.back().second;
    };
    std::vector<std::pair<double, double>> pts;
    const double dur = (double) F * ft;
    if (mode == 3) { for (double t = 0; t <= dur + tf; t += std::max (ft, cx.rand() * tf)) pts.push_back ({ t, (cx.rand() * 2 - 1) * range }); }
    if (mode == 4 || mode == 6)
        for (double t = 0; t <= dur; t += tf)
        {   // mean pitch of each TIME block (approx: displaced at random)
            double s = 0; int c = 0;
            for (long long i = (long long) (t / ft); i < (long long) ((t + tf) / ft) && i < F; ++i) if (f0[(size_t) i] > 0) { s += m[(size_t) i]; ++c; }
            if (c) pts.push_back ({ t + tf / 2, s / c + (mode == 6 ? (cx.rand() * 2 - 1) * range : 0.0) });
        }
    static const int SC[][8] = { { 0, 2, 4, 5, 7, 9, 11, -1 }, { 0, 2, 3, 5, 7, 8, 10, -1 }, { 0, 2, 4, 7, 9, -1 }, { 0, 3, 5, 7, 10, -1 }, { 0, 2, 4, 6, 8, 10, -1 } };
    const int sc = clamp (p.i ("scale"), 0, 5), key = p.i ("key");
    for (long long i = 0; i < F; ++i)
    {
        const double x = m[(size_t) i], t = (double) i * ft;
        if (f0[(size_t) i] <= 0) { n[(size_t) i] = x; continue; }
        double y = x;
        switch (mode)
        {
            case 0:
            {   // exag: contour first (variance ^ CONTOUR towards the extremes), then range around the mean
                const double maxint = std::max (1e-6, (mx - mean) / 12), minint = std::max (1e-6, (mean - mn) / 12);
                double var = x >= mean ? ((x - mean) / 12) / maxint : ((mean - x) / 12) / minint;
                if (var > 0) var = std::pow (std::min (1.0, var), p["contour"]);
                y = x >= mean ? mean + var * (mx - mean) : mean - var * (mean - mn);
                y = mean + (y - mean) * p["factor"];
                break;
            }
            case 1: y = clampd (2 * mean - x, 0, 127); break;
            case 2:
            {
                double best = std::round (x), bd = 1e9;   // CHROMATIC: nearest semitone
                if (sc < 5)
                    for (int o = -1; o <= 11; ++o)
                        for (int j = 0; j < 8 && SC[sc][j] >= 0; ++j)
                        {
                            const double c = 12.0 * o + key + SC[sc][j];
                            if (std::abs (c - x) < bd) { bd = std::abs (c - x); best = c; }
                        }
                y = best;
                break;
            }
            case 3: y = x + interp (pts, t); break;
            case 4: case 6: y = pts.empty() ? x : interp (pts, t); break;
            default: y = x + range * std::sin (TAU * p["rate"] * t); break;
        }
        n[(size_t) i] = clampd (y, 0, 135);
    }
    // transposition ratio per frame, lightly smoothed against clicks
    std::vector<double> r ((size_t) F, 0.0);
    for (long long i = 0; i < F; ++i) r[(size_t) i] = f0[(size_t) i] > 0 ? n[(size_t) i] - m[(size_t) i] : 0.0;
    std::vector<double> rs (r);
    for (long long i = 0; i < F; ++i) { double s = 0; int c = 0; for (long long j = i - 2; j <= i + 2; ++j) if (j >= 0 && j < F) { s += r[(size_t) j]; ++c; } rs[(size_t) i] = s / c; }
    DVec env;
    return specMap (ch, cx, p["win"], [&] (DVec& a, DVec& fr, Fr& in)
    {
        const double st = rs[(size_t) clamp (in.f, 0LL, F - 1)];
        if (std::abs (st) < 1e-4) return;
        const double pre = total (a);
        specEnv (a, in.R.bin, p["detail"], env);
        transposeWin (a, fr, std::pow (2.0, st / 12), in.R.bin, in.R.sr / 2, &env);
        normTo (a, pre);
    });
}
} // namespace

void addCdpSpectralA (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CAT = "CDP \xc2\xb7 SPECTRAL";
    add (v, CAT, "cdp_spec_gain", "SPECTRAL GAIN", "SGAN", "Scales every analysis channel by a gain that sweeps from START to END over the sound: a spectral-domain fade or swell.",
         { P ("start", "START", -40, 24, 6, 0, {}, Fmt::db), P ("end", "END", -40, 24, -18, 0, {}, Fmt::db), Win (0.02, 0.3, 0.09) }, specGain, {}, false, "spec gain");
    add (v, CAT, "cdp_spec_gate", "SPECTRAL LIMIT", "SLIM", "Deletes every analysis channel quieter than THRESH (relative to the loudest channel in the sound): noise and reverb fall away, leaving the strong partials glassy and bare.",
         { P ("thr", "THRESH", -90, 0, -36, 0, {}, Fmt::db), Win (0.02, 0.3, 0.09) }, specGate, {}, false, "spec gate");
    add (v, CAT, "cdp_spec_bare", "HARMONICS ONLY", "BARE", "Tracks the pitch and keeps only the channels sitting on its harmonics (within SPAN times the pitch): breath and noise vanish, a clean harmonic skeleton stays. LESS BODY keeps just one channel per harmonic.",
         { P ("span", "SPAN", 0.02, 0.5, 0.12, PExp, "x f0"), Opt ("body", "LESS BODY", { "NO", "YES" }), Win (0.03, 0.3, 0.09) }, specBare, {}, false, "spec bare");
    add (v, CAT, "cdp_spec_clean", "SPECTRAL CLEAN", "CLEN", "Noise reduction from a noise print (the quietest PRINT LEN of the sound, or its start or end) times GAIN. FROM TIME deletes channels for good once below it, ANYWHERE wherever below, ABOVE FREQ only above FREQ, COMPARE those never above it.",
         { Opt ("mode", "MODE", { "FROM TIME", "ANYWHERE", "ABOVE FREQ", "COMPARE" }, 1), Opt ("print", "PRINT", { "QUIETEST", "END", "START" }), P ("plen", "PRINT LEN", 0.05, 2, 0.3, PExp, {}, Fmt::time),
           P ("gain", "GAIN", 1, 20, 2, PExp, "x"), P ("skip", "SKIP", 0, 5, 0, 0, {}, Fmt::time), P ("frq", "FREQ", 50, 10000, 1000, PExp, {}, Fmt::hz), Win (0.02, 0.3, 0.05) }, specClean, {}, false, "spec clean");
    add (v, CAT, "cdp_spec_magnify", "SPECTRAL MAGNIFY", "MAGN", "Grabs the single analysis window at POSITION and sustains it for DURATION: one instant of the sound magnified into a perfectly steady tone.",
         { Pct ("pos", "POSITION", 0.25), P ("dur", "DURATION", 0.2, 30, 4, PExp, {}, Fmt::time), Win (0.02, 0.3, 0.09) }, specMagnify, {}, false, "spec magnify");
    const std::string BLUR = "CDP \xc2\xb7 SPECTRAL \xc2\xb7 BLUR";
    add (v, BLUR, "cdp_blur_avrg", "SPECTRAL AVERAGE", "AVRG", "Averages the energy of each analysis channel with its neighbours across N channels: partials smear into bands, the sound turns soft and veiled.",
         { P ("n", "N CHANNELS", 1, 255, 31, PInt), Win (0.02, 0.3, 0.09) }, blurAvrg, {}, false, "blur avrg");
    add (v, BLUR, "cdp_blur_blur", "TIME BLUR", "TBLR", "Keeps only every BLUR-th analysis window and glides between them: attacks and detail dissolve into slowly morphing spectra.",
         { P ("blur", "BLUR", 1, 200, 16, PInt | PExp), Win (0.02, 0.3, 0.05) }, blurBlur, {}, false, "blur blur");
    add (v, BLUR, "cdp_blur_suppress", "SUPPRESS LOUDEST", "SUPR", "Removes the N loudest channels of every analysis window: fundamentals and strong partials disappear, leaving the hidden background.",
         { P ("n", "N CHANNELS", 1, 512, 24, PInt | PExp), Win (0.02, 0.3, 0.09) }, blurSuppress, {}, false, "blur suppress");
    add (v, BLUR, "cdp_blur_chorus", "SPECTRAL CHORUS", "CHOR", "Randomises every partial in every window: amplitudes by up to ASPREAD, frequencies by up to FSPREAD (both ways, up or down), or both. A shimmering, unstable chorus.",
         { Opt ("mode", "MODE", { "AMP", "FREQ", "FREQ UP", "FREQ DOWN", "AMP + FREQ", "AMP + FRQ UP", "AMP + FRQ DN" }, 4), P ("aspread", "ASPREAD", 1, 1028, 4, PExp, "x"),
           P ("fspread", "FSPREAD", 1, 4, 1.03, PExp, "x"), Win (0.02, 0.3, 0.09), Seed() }, blurChorus, {}, false, "blur chorus");
    add (v, BLUR, "cdp_blur_drunk", "SPECTRAL DRUNK", "DRUK", "A drunken walk along the analysis windows from START: each step jumps up to RANGE windows back or forth until the output lasts DURATION. NO REPEATS forbids standing still.",
         { P ("range", "RANGE", 1, 100, 6, PInt | PExp), Pct ("start", "START", 0), P ("dur", "DURATION", 0.5, 60, 6, PExp, {}, Fmt::time), Opt ("nozero", "NO REPEATS", { "NO", "YES" }), Win (0.02, 0.3, 0.05), Seed() }, blurDrunk, {}, false, "blur drunk");
    add (v, BLUR, "cdp_blur_shuffle", "WINDOW SHUFFLE", "WSHF", "Shuffles groups of GROUP analysis windows by a domain-image PATTERN (ABC-CBA plays each run of three groups backwards; ABC-ABBABCC repeats and reorders). RANDOM draws a new order every time.",
         { Opt ("pattern", "PATTERN", { "ABC-CBA", "ABCD-DCBA", "ABC-ABBABCC", "AB-AAB", "ABCD-BADC", "ABCD-AACC", "ABCDE-EDCBA", "RANDOM" }, 2), P ("group", "GROUP", 1, 64, 6, PInt | PExp), Win (0.02, 0.3, 0.05), Seed() }, blurShuffle, {}, false, "blur shuffle");
    add (v, BLUR, "cdp_blur_weave", "SPECTRAL WEAVE", "WEAV", "Weaves through the analysis windows with a repeating pattern of STEP 1-3 (in windows, negative = backwards), never before the start: the sound stutters and rewinds as it advances.",
         { P ("s1", "STEP 1", -32, 32, 4, PInt), P ("s2", "STEP 2", -32, 32, -3, PInt), P ("s3", "STEP 3", -32, 32, 2, PInt), Win (0.02, 0.3, 0.05) }, blurWeave, {}, false, "blur weave");
    add (v, BLUR, "cdp_blur_noise", "SPECTRAL NOISE", "SNOI", "Pulls every channel towards the window's mean level: NOISE 0 leaves the sound, 1 saturates it with noise that keeps only its loudness contour.",
         { Pct ("noise", "NOISE", 0.6), Win (0.02, 0.3, 0.09) }, blurNoise, {}, false, "blur noise");
    add (v, BLUR, "cdp_blur_scatter", "SPECTRAL SCATTER", "SCAT", "Keeps only KEEP randomly chosen frequency blocks of BLOCK width in every window: a thinned, glittering spectrum. RANDOM COUNT varies the number kept, NORMALIZE holds the level.",
         { P ("keep", "KEEP", 1, 256, 12, PInt | PExp), P ("block", "BLOCK", 10, 2000, 120, PExp, {}, Fmt::hz), Opt ("randcnt", "RANDOM CNT", { "NO", "YES" }), Opt ("norm", "NORMALIZE", { "NO", "YES" }, 1), Win (0.02, 0.3, 0.05), Seed() }, blurScatter, {}, false, "blur scatter");
    add (v, BLUR, "cdp_blur_spread", "SPECTRAL SPREAD", "SPRD", "Spreads the spectral peaks out to the spectral envelope (resolution DETAIL): SPREAD 1 turns partials into formant-shaped noise, lower values add a breathy haze.",
         { Pct ("spread", "SPREAD", 0.8), P ("detail", "DETAIL", 30, 1000, 150, PExp, {}, Fmt::hz), Win (0.02, 0.3, 0.09) }, blurSpread, {}, false, "blur spread");
    const std::string FOC = "CDP \xc2\xb7 SPECTRAL \xc2\xb7 FOCUS \xc2\xb7 WARP";
    add (v, FOC, "cdp_focus_accu", "SPECTRAL ACCUMULATE", "ACCU", "Sustains every channel until louder data arrives in it: each note leaves a ringing spectral trail that fades by DECAY per second and glides by GLIS octaves per second.",
         { P ("decay", "DECAY", 0.001, 1, 0.3, PExp, "x/s"), P ("glis", "GLIS", -11.7, 11.7, 0, 0, "oct/s"), Win (0.02, 0.3, 0.05) }, focusAccu, {}, false, "focus accu");
    add (v, FOC, "cdp_focus_exag", "SPECTRAL EXAGGERATE", "EXAG", "Exaggerates the spectral contour: each window is raised to the power 1/EXAG. Below 1 the peaks stand out and the troughs deepen (resonant, hollow); above 1 the contour flattens.",
         { P ("exag", "EXAG", 0.05, 10, 0.3, PExp), Win (0.02, 0.3, 0.09) }, focusExag, {}, false, "focus exag");
    add (v, FOC, "cdp_focus_focus", "FOCUS ON PEAKS", "FOCF", "Finds up to PEAKS formant peaks between BOTTOM and TOP and filters each window with bands BANDWIDTH octaves wide around them. STABLE (windows) keeps only recurring peaks for steadier resonances.",
         { P ("peaks", "PEAKS", 1, 16, 5, PInt), P ("bw", "BANDWIDTH", 0.02, 3, 0.3, PExp, "oct"), P ("bot", "BOTTOM", 20, 10000, 100, PExp, {}, Fmt::hz), P ("top", "TOP", 100, 20000, 8000, PExp, {}, Fmt::hz),
           P ("stable", "STABLE", 1, 32, 6, PInt), Win (0.02, 0.3, 0.09) }, focusFocus, {}, false, "focus focus");
    add (v, FOC, "cdp_focus_fold", "OCTAVE FOLD", "FOLD", "Octave-transposes every spectral component lying outside LOW..HIGH into that range: the whole sound folds into one register. FULLER keeps more of the folded data.",
         { P ("lo", "LOW", 20, 8000, 200, PExp, {}, Fmt::hz), P ("hi", "HIGH", 40, 16000, 800, PExp, {}, Fmt::hz), Opt ("fuller", "FULLER", { "NO", "YES" }, 1), Win (0.02, 0.3, 0.09) }, focusFold, {}, false, "focus fold");
    add (v, FOC, "cdp_focus_freeze", "FREEZE POINTS", "FRZP", "Every EVERY seconds the spectrum is frozen for HOLD of the interval: AMP freezes the channel levels, FREQ the frequencies, BOTH everything. USE picks the window AFTER (start) or BEFORE (end) of each hold.",
         { Opt ("mode", "MODE", { "AMP", "FREQ", "BOTH" }, 2), P ("every", "EVERY", 0.05, 4, 0.5, PExp, {}, Fmt::time), Pct ("hold", "HOLD", 0.6), Opt ("use", "USE", { "AFTER", "BEFORE" }), Win (0.02, 0.3, 0.05) }, focusFreeze, {}, false, "focus freeze");
    add (v, FOC, "cdp_focus_hold", "SPECTRAL HOLD", "HOLD", "Stops the sound COUNT times (from POSITION on, evenly spread) and holds its spectrum for HOLD seconds each before carrying on: the result is longer than the source.",
         { Pct ("pos", "POSITION", 0.3), P ("dur", "HOLD", 0.1, 20, 2, PExp, {}, Fmt::time), P ("count", "COUNT", 1, 16, 2, PInt), Win (0.02, 0.3, 0.05) }, focusHold, {}, false, "focus hold");
    add (v, FOC, "cdp_focus_step", "SPECTRAL STEP", "STEP", "Step-frame through the sound: every STEP seconds one window is frozen and held until the next step. A spectral sample-and-hold, from grainy to robotic.",
         { P ("step", "STEP", 0.02, 2, 0.12, PExp, {}, Fmt::time), Win (0.02, 0.3, 0.05) }, focusStep, {}, false, "focus step");
    add (v, FOC, "cdp_strange_shift", "BAND FREQ SHIFT", "BSFT", "Shifts frequencies by SHIFT Hz: the whole spectrum, only above or below DIVIDE, or only inside or outside LOW..HIGH. Harmonic sounds turn metallic and inharmonic.",
         { Opt ("mode", "MODE", { "ALL", "ABOVE", "BELOW", "INSIDE", "OUTSIDE" }), P ("shift", "SHIFT", -2000, 2000, 250, 0, "Hz"), P ("divide", "DIVIDE", 20, 10000, 600, PExp, {}, Fmt::hz),
           P ("lo", "LOW", 20, 10000, 300, PExp, {}, Fmt::hz), P ("hi", "HIGH", 20, 16000, 2000, PExp, {}, Fmt::hz), Win (0.02, 0.3, 0.09) }, strangeShift, {}, false, "strange shift");
    add (v, FOC, "cdp_strange_glis", "ENVELOPE GLISSANDI", "GLIS", "Glissandi inside the sound's own spectral envelope: endless SHEPARD tones, an INHARMONIC glide of partials HZ STEP apart, or SELF, the sound's partials gliding in octave-wrapped copies. RATE in semitones/s.",
         { Opt ("mode", "MODE", { "SHEPARD", "INHARMONIC", "SELF" }), P ("rate", "RATE", -24, 24, 4, 0, "st/s"), P ("hzstep", "HZ STEP", 20, 2000, 150, PExp, {}, Fmt::hz),
           P ("top", "TOP", 500, 22000, 12000, PExp, {}, Fmt::hz), P ("detail", "DETAIL", 30, 1000, 200, PExp, {}, Fmt::hz), Win (0.02, 0.3, 0.09) }, strangeGlis, {}, false, "strange glis");
    add (v, FOC, "cdp_strange_waver", "HARMONIC WAVER", "WAVR", "Oscillates VIB RATE times per second between the harmonic spectrum and a stretched, inharmonic one (up to STRETCH, above BOTTOM). SPECIFIED shapes the stretch with EXPONENT.",
         { Opt ("mode", "MODE", { "STANDARD", "SPECIFIED" }), P ("vib", "VIB RATE", 0.05, 20, 1.5, PExp, "Hz"), P ("stretch", "STRETCH", 1, 3, 1.4, 0, "x"), P ("bot", "BOTTOM", 20, 5000, 250, PExp, {}, Fmt::hz),
           P ("expon", "EXPONENT", 0.1, 10, 2, PExp), Win (0.02, 0.3, 0.09) }, strangeWaver, {}, false, "strange waver");
    add (v, FOC, "cdp_strange_warp", "PITCH/TIME WARP", "WARP", "An approximate copy of the sound: at the turning points of its pitch contour (scanned over SCAN) pitch is displaced by up to PITCH semitones and timing by up to TIME, warping smoothly between them.",
         { P ("prange", "PITCH", 0, 12, 2, 0, "st"), P ("trange", "TIME", 0, 1000, 150, 0, "ms"), P ("scan", "SCAN", 30, 2000, 200, PExp, "ms"), Win (0.02, 0.3, 0.05), Seed() }, strangeWarp, {}, false, "strange warp");
    add (v, FOC, "cdp_strange_invert", "SPECTRAL INVERT", "SINV", "Inverts each channel's level against its loudest moment, so quiet parts of the spectrum become loud and loud ones quiet. KEEP LEVEL inverts within each window and keeps the source's loudness envelope.",
         { Opt ("mode", "MODE", { "NORMAL", "KEEP LEVEL" }), Win (0.02, 0.3, 0.05) }, strangeInvert, {}, false, "strange invert");
    add (v, FOC, "cdp_stretch_spectrum", "SPECTRUM STRETCH", "SSTR", "Stretches the partial frequencies above (or below) DIVIDE by a ratio rising to MAX STRETCH at the edge of the spectrum, curved by EXPONENT and scaled by DEPTH: bells and inharmonic timbres.",
         { Opt ("mode", "MODE", { "ABOVE", "BELOW" }), P ("divide", "DIVIDE", 20, 10000, 200, PExp, {}, Fmt::hz), P ("max", "MAX STRETCH", 0.25, 4, 1.6, PExp, "x"), P ("expon", "EXPONENT", 0.1, 5, 1, PExp),
           Pct ("depth", "DEPTH", 1), Win (0.02, 0.3, 0.09) }, stretchSpectrum, {}, false, "stretch spectrum");
    add (v, FOC, "cdp_stretch_time", "SPECTRAL TIMESTRETCH", "TSTR", "Phase-vocoder time stretch without pitch change; STRETCH sweeps from START to END along the sound (below 1 compresses).",
         { P ("start", "START", 0.1, 16, 2, PExp, "x"), P ("end", "END", 0.1, 16, 2, PExp, "x"), Win (0.02, 0.3, 0.09) }, stretchTime, {}, false, "stretch time 1");
    add (v, FOC, "cdp_pvoc_extract", "BIN EXTRACT", "BEXT", "Resynthesises only the ODD or EVEN analysis channels (or ALL) between LOW and HIGH: hollow, comb-filtered and ghostly versions of the sound.",
         { Opt ("mode", "MODE", { "ALL", "ODD", "EVEN" }, 1), P ("lo", "LOW", 0, 20000, 0, 0, {}, Fmt::hz), P ("hi", "HIGH", 50, 22050, 22050, PExp, {}, Fmt::hz), Win (0.005, 0.3, 0.023) }, pvocExtract, {}, false, "pvoc extract");
    const std::string PCH = "CDP \xc2\xb7 SPECTRAL \xc2\xb7 PITCH";
    add (v, PCH, "cdp_pitch_altharms", "ALTERNATE HARMONICS", "ALTH", "Tracks the pitch and deletes the ODD harmonics (an octave up with the formants unchanged) or the EVEN ones (hollow, clarinet-like). LESS BODY keeps a single channel per harmonic.",
         { Opt ("mode", "MODE", { "DELETE ODD", "DELETE EVEN" }), Opt ("body", "LESS BODY", { "NO", "YES" }), Win (0.03, 0.3, 0.09) }, pitchAltharms, {}, false, "pitch altharms");
    add (v, PCH, "cdp_pitch_octmove", "OCTAVE MOVE", "OCTM", "Transposes by a harmonic ratio (TRANSPOSE 2 = octave, 3 = twelfth) without moving the formants: UP keeps every n-th harmonic, DOWN fills in the new harmonics below, DOWN + BASS boosts the new bass by BASS BOOST.",
         { Opt ("mode", "MODE", { "UP", "DOWN", "DOWN + BASS" }, 1), P ("transp", "TRANSPOSE", 2, 8, 2, PInt), P ("boost", "BASS BOOST", 1, 10, 3, 0, "x"), Win (0.03, 0.3, 0.09) }, pitchOctmove, {}, false, "pitch octmove");
    add (v, PCH, "cdp_pitch_transp", "SPLIT TRANSPOSE", "SPLT", "Transposes only part of the spectrum: an octave up above SPLIT, down below it, or both; or by UP / DOWN semitones. DEPTH blends the effect in. Bass and treble pull apart.",
         { Opt ("mode", "MODE", { "OCT UP ABOVE", "OCT DOWN BELOW", "OCT UP + DOWN", "UP ABOVE", "DOWN BELOW", "UP + DOWN" }, 5), P ("split", "SPLIT", 50, 8000, 600, PExp, {}, Fmt::hz),
           P ("up", "UP", -24, 24, 7, 0, "st"), P ("down", "DOWN", -24, 24, -5, 0, "st"), Pct ("depth", "DEPTH", 1), Win (0.02, 0.3, 0.09) }, pitchTransp, {}, false, "pitch transp");
    add (v, PCH, "cdp_pitch_tune", "HARMONIC TUNE", "HTUN", "Pulls the spectrum onto the harmonics of a chord (ROOT + CHORD): nearby partials are FOCUSed onto them, the rest is suppressed by CLARITY. TRACE moves the N loudest partials to the nearest chord harmonic; nothing below BASS CUT.",
         { P ("note", "ROOT", 24, 72, 45, PInt, {}, Fmt::note), Opt ("chord", "CHORD", CHORD_NAMES), Pct ("focus", "FOCUS", 1), Pct ("clarity", "CLARITY", 0.7), P ("trace", "TRACE", 0, 256, 0, PInt),
           P ("bcut", "BASS CUT", 0, 2000, 0, 0, {}, Fmt::hz), Win (0.03, 0.3, 0.09) }, pitchTune, {}, false, "pitch tune");
    add (v, PCH, "cdp_pitch_pick", "HARMONIC PICK", "PICK", "Keeps only the channels that could hold chosen partials of FUNDAMENTAL: the HARMONIC series, OCTAVES, ODD harmonics, a LINEAR series STEP Hz apart, or harmonics DISPLACED by STEP. CLARITY sets how much of the rest is cut.",
         { Opt ("mode", "MODE", { "HARMONICS", "OCTAVES", "ODD HARMS", "LINEAR", "DISPLACED" }), P ("fund", "FUNDAMENTAL", 20, 2000, 110, PExp, {}, Fmt::hz), P ("step", "STEP", 5, 2000, 70, PExp, {}, Fmt::hz),
           Pct ("clarity", "CLARITY", 1), Win (0.03, 0.3, 0.09) }, pitchPick, {}, false, "pitch pick");
    add (v, PCH, "cdp_pitch_chordf", "FORMANT CHORD", "CHDF", "Superimposes transposed copies of the spectrum (a CHORD, moved by SHIFT) inside the original spectral envelope (resolution DETAIL), so the formants stay put. LOW/HIGH filter the result; FULLER keeps more data.",
         { Opt ("chord", "CHORD", CHORD_NAMES), P ("shift", "SHIFT", -24, 24, 0, 0, "st"), P ("lo", "LOW", 0, 5000, 0, 0, {}, Fmt::hz), P ("hi", "HIGH", 200, 22050, 22050, PExp, {}, Fmt::hz),
           Opt ("fuller", "FULLER", { "NO", "YES" }, 1), P ("detail", "DETAIL", 30, 1000, 150, PExp, {}, Fmt::hz), Win (0.02, 0.3, 0.09) }, pitchChordf, {}, false, "pitch chordf");
    add (v, PCH, "cdp_pitch_chord", "SPECTRAL CHORD", "CHRD", "Superimposes transposed copies of the whole spectrum (a CHORD, moved by SHIFT) onto the original, formants and all. LOW/HIGH filter the result; FULLER keeps more data.",
         { Opt ("chord", "CHORD", CHORD_NAMES), P ("shift", "SHIFT", -24, 24, 0, 0, "st"), P ("lo", "LOW", 0, 5000, 0, 0, {}, Fmt::hz), P ("hi", "HIGH", 200, 22050, 22050, PExp, {}, Fmt::hz),
           Opt ("fuller", "FULLER", { "NO", "YES" }, 1), Win (0.02, 0.3, 0.09) }, pitchChord, {}, false, "pitch chord");
    add (v, PCH, "cdp_repitch_transpose", "SPECTRAL TRANSPOSE", "STRN", "Transposes the whole spectrum, formants included, from START to END semitones over the sound (chipmunk or giant); data outside LOW..HIGH is removed.",
         { P ("start", "START", -36, 36, -7, 0, "st"), P ("end", "END", -36, 36, -7, 0, "st"), P ("lo", "LOW", 0, 5000, 0, 0, {}, Fmt::hz), P ("hi", "HIGH", 200, 22050, 22050, PExp, {}, Fmt::hz),
           Win (0.02, 0.3, 0.09) }, repitchTranspose, {}, false, "repitch transpose");
    add (v, PCH, "cdp_repitch_transposef", "FORMANT TRANSPOSE", "FTRN", "Transposes the partials from START to END semitones but keeps the original spectral envelope (resolution DETAIL): voices stay natural. Data outside LOW..HIGH is removed.",
         { P ("start", "START", -36, 36, 5, 0, "st"), P ("end", "END", -36, 36, 5, 0, "st"), P ("lo", "LOW", 0, 5000, 0, 0, {}, Fmt::hz), P ("hi", "HIGH", 200, 22050, 22050, PExp, {}, Fmt::hz),
           P ("detail", "DETAIL", 30, 1000, 150, PExp, {}, Fmt::hz), Win (0.02, 0.3, 0.09) }, repitchTransposef, {}, false, "repitch transposef");
    add (v, PCH, "cdp_repitch_synth", "PITCH SYNTH", "PSYN", "Replaces the sound by a tone that follows its pitch contour and loudness, with harmonic levels H1..H8. Unpitched parts become NOISE, HOLD the last pitch, or SILENCE.",
         { Pct ("h1", "H1", 1), Pct ("h2", "H2", 0.5), Pct ("h3", "H3", 0.33), Pct ("h4", "H4", 0.25), Pct ("h5", "H5", 0.2), Pct ("h6", "H6", 0.15), Pct ("h7", "H7", 0.1), Pct ("h8", "H8", 0.05),
           Opt ("unvoiced", "UNPITCHED", { "NOISE", "HOLD", "SILENCE" }, 1), Win (0.03, 0.3, 0.09), Seed() }, repitchSynth, {}, false, "repitch synth");
    add (v, PCH, "cdp_repitch_vowels", "PITCH VOWELS", "PVOW", "Replaces the sound by a sung vowel following its pitch and loudness, gliding from VOWEL to TO VOWEL. HALFWIDTH and CURVE shape the formants, PEAK RANGE their contrast, FUND the fundamental, OFFSET detunes the harmonics.",
         { Opt ("vowel", "VOWEL", VOWEL_NAMES, 5), Opt ("vowel2", "TO VOWEL", VOWEL_NAMES, 0), P ("hwidth", "HALFWIDTH", 0.01, 10, 0.3, PExp), P ("curve", "CURVE", 0.1, 10, 1, PExp),
           Pct ("pkrange", "PEAK RANGE", 0.9), Pct ("fweight", "FUND", 0.4), Pct ("offset", "OFFSET", 0), Win (0.03, 0.3, 0.09), Seed() }, repitchVowels, {}, false, "repitch vowels");
    add (v, PCH, "cdp_repitch_contour", "PITCH CONTOUR FX", "PCFX", "Retunes a melody inside its own formants: EXAGGERATE intervals (FACTOR, CONTOUR), INVERT around the mean, QUANTISE to SCALE/KEY, RANDOMISE (RANGE every TIME), SMOOTH over TIME, VIBRATO (RANGE, RATE) or APPROX.",
         { Opt ("mode", "MODE", { "EXAGGERATE", "INVERT", "QUANTISE", "RANDOMISE", "SMOOTH", "VIBRATO", "APPROX" }), P ("factor", "FACTOR", 0, 4, 2.5, 0, "x"), Pct ("contour", "CONTOUR", 1),
           P ("range", "RANGE", 0, 12, 2, 0, "st"), P ("time", "TIME", 20, 2000, 250, PExp, "ms"), P ("rate", "RATE", 0.5, 15, 5.5, PExp, "Hz"),
           Opt ("scale", "SCALE", { "MAJOR", "MINOR", "PENTATONIC", "MINOR PENT", "WHOLE TONE", "CHROMATIC" }), Opt ("key", "KEY", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }),
           P ("detail", "DETAIL", 30, 1000, 150, PExp, {}, Fmt::hz), Win (0.03, 0.3, 0.09), Seed() }, repitchContour, {}, false, "repitch exag/invert/quantise/randomise/smooth/vibrato/approx + transposef");
}
} // namespace lab
