// DSP building blocks shared by DATLAB's own processes (LabProcsNew.cpp) and the CDP collection (LabCdp*.cpp).
#pragma once
#include "LabCore.h"

namespace lab::dsp
{
// note divisions in beats, shared by the synced processes (same order as DIVS in LabDefs.cpp)
inline const double DIV_BEATS[] = { 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 0.75, 0.375 };
inline double divSec (int idx, double bpm) { return DIV_BEATS[clamp (idx, 0, 10)] * 60.0 / std::max (20.0, bpm); }

// Zavalishin TPT state-variable filter
struct SVF
{
    double ic1 = 0, ic2 = 0, lp = 0, bp = 0, hp = 0;
    void run (double v0, double fc, double Q, double sr)
    {
        double g = std::tan (PI * clampd (fc, 5, sr * 0.49) / sr), k = 1 / std::max (0.05, Q);
        double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        double v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2; lp = v2; bp = v1; hp = v0 - k * v1 - v2;
        if (! std::isfinite (ic1) || ! std::isfinite (ic2)) ic1 = ic2 = 0;
    }
};
struct OnePole { double z = 0; double lp (double x, double fc, double sr) { double a = std::exp (-TAU * clampd (fc, 1, sr * 0.49) / sr); z = x * (1 - a) + z * a; return z; } };

// fractional read from a ring buffer of size power of two
struct Ring
{
    Buf b; size_t mask = 0; size_t w = 0;
    explicit Ring (size_t minSize) { size_t n = 1; while (n < minSize + 4) n <<= 1; b.assign (n, 0); mask = n - 1; }
    void push (double v) { b[w & mask] = (float) v; ++w; }
    double read (double delay) const   // delay in samples behind the last written sample (>= 1)
    {
        double rp = (double) w - delay; double fi = std::floor (rp); long long i = (long long) fi; double f = rp - fi;
        return b[(size_t) i & mask] * (1 - f) + b[(size_t) (i + 1) & mask] * f;
    }
};
inline double lfoShape (int shape, double ph, double& sh, double& last, Ctx& cx)
{
    ph -= std::floor (ph);
    switch (shape)
    {
        case 0: return std::sin (TAU * ph);
        case 1: return 1 - 4 * std::abs (ph - 0.5);
        case 2: return 2 * ph - 1;
        case 3: return 1 - 2 * ph;
        case 4: return ph < 0.5 ? 1 : -1;
        default: if (ph < last) sh = cx.rand() * 2 - 1; last = ph; return sh;
    }
}
inline Chans stereoOf (const Chans& ch) { return ch.size() >= 2 ? Chans { ch[0], ch[1] } : Chans { ch[0], ch[0] }; }
inline Buf monoOf (const Chans& ch) { Buf m (ch[0].size()); for (auto& x : ch) for (size_t i = 0; i < m.size(); ++i) m[i] = (float) (m[i] + x[i] / (double) ch.size()); return m; }
// windowed grain overlap-add pitch/time engine: srcAt(t) = read position for output time t, ratioAt(t) = playback speed
template <typename SrcFn, typename RatioFn>
inline Buf grainEngine (const Buf& x, size_t outLen, double sr, double grainSec, SrcFn srcAt, RatioFn ratioAt, bool loop)
{
    const long long G = std::max (32LL, jr (grainSec * sr)), hop = std::max (8LL, G / 4); const DVec win = hann ((size_t) G);
    Buf out (outLen); const double len = (double) x.size();
    for (long long t0 = -G; t0 < (long long) outLen; t0 += hop)
    {
        double tc = (double) std::max (0LL, t0 + G / 2), src = srcAt (tc), ratio = ratioAt (tc);
        for (long long i = 0; i < G; ++i)
        {
            long long o = t0 + i; if (o < 0 || o >= (long long) outLen) continue;
            double sp = src + ((double) i - (double) G / 2) * ratio;
            if (loop && len > 0) { sp = std::fmod (sp, len); if (sp < 0) sp += len; }
            out[(size_t) o] = (float) (out[(size_t) o] + lerpAt (x, sp) * win[(size_t) i] * 0.5);
        }
    }
    return out;
}
// ---- pitch tracking (YIN, de Cheveigne & Kawahara 2002) ----
// f0 per hop (Hz, 0 = unvoiced / silent), frame i centred at i * hop samples.
struct PitchTrack { std::vector<double> f0; size_t hop = 512; double at (double sample) const   // f0 at a sample position (nearest frame, 0 if unvoiced)
    { if (f0.empty()) return 0; long long i = jr (sample / (double) hop); return f0[(size_t) clamp (i, 0LL, (long long) f0.size() - 1)]; } };
inline PitchTrack trackPitch (const Buf& x, double sr, double minHz = 50, double maxHz = 1500, double thresh = 0.15, Ctx* cx = nullptr)
{
    PitchTrack pt; pt.hop = (size_t) std::max (64.0, std::round (sr * 0.01));
    const size_t tauMax = (size_t) std::max (4.0, std::ceil (sr / std::max (20.0, minHz))), tauMin = (size_t) std::max (2.0, std::floor (sr / std::max (minHz * 1.01, maxHz)));
    const size_t W = tauMax;   // integration window
    const size_t n = x.size(), frames = n / pt.hop + 1;
    pt.f0.assign (frames, 0.0);
    std::vector<double> d (tauMax + 1), cm (tauMax + 1);
    for (size_t fi = 0; fi < frames; ++fi)
    {
        if (cx && ! (fi & 63)) cx->prog ((double) fi / (double) frames * 0.5);
        const long long c = (long long) (fi * pt.hop), a = c - (long long) W;   // frame spans [a, a + 2W + tauMax)
        auto s = [&] (long long i) { return i >= 0 && i < (long long) n ? (double) x[(size_t) i] : 0.0; };
        double energy = 0; for (size_t j = 0; j < W; ++j) energy += s (a + (long long) j) * s (a + (long long) j);
        if (energy / (double) W < 1e-7) continue;
        for (size_t tau = 1; tau <= tauMax; ++tau)
        {
            double acc = 0;
            for (size_t j = 0; j < W; j += 2) { double dd = s (a + (long long) j) - s (a + (long long) (j + tau)); acc += dd * dd; }
            d[tau] = acc;
        }
        double run = 0; cm[0] = 1;
        for (size_t tau = 1; tau <= tauMax; ++tau) { run += d[tau]; cm[tau] = run > 0 ? d[tau] * (double) tau / run : 1; }
        size_t best = 0;
        for (size_t tau = tauMin; tau < tauMax; ++tau)
            if (cm[tau] < thresh) { while (tau + 1 < tauMax && cm[tau + 1] < cm[tau]) ++tau; best = tau; break; }
        if (! best) continue;
        // Octave check: a strong 2nd / 3rd harmonic (e.g. a sung vowel whose first formant sits on it) makes a dip at a
        // fraction of the period cross the threshold first. If the dip at a multiple of the lag is much deeper, that
        // multiple is the real period. A clean periodic sound has both dips near 0, so it never moves down an octave.
        for (bool moved = (double) best >= sr / 1000; moved;)   // (not above 1 kHz: short lags are too coarse to compare)
        {
            moved = false;
            for (size_t k = 2; k <= 3 && ! moved; ++k)
            {
                const size_t c0 = best * k; if (c0 + 2 >= tauMax) break;
                size_t m = c0 - std::min (c0 - 1, k + 1);
                for (size_t tau = m; tau <= c0 + k + 1 && tau < tauMax; ++tau) if (cm[tau] < cm[m]) m = tau;
                if (cm[best] > 0.05 && cm[m] < cm[best] * 0.2) { best = m; moved = true; }
            }
        }
        double t = (double) best;   // parabolic interpolation
        if (best > 1 && best < tauMax) { double y0 = cm[best - 1], y1 = cm[best], y2 = cm[best + 1], den = y0 - 2 * y1 + y2; if (std::abs (den) > 1e-12) t += 0.5 * (y0 - y2) / den; }
        pt.f0[fi] = sr / t;
    }
    // 5-frame median of the voiced neighbours: removes isolated octave jumps that survive the check above
    {
        const auto raw = pt.f0;
        for (size_t i = 0; i < frames; ++i)
        {
            if (raw[i] <= 0) continue;
            double v[5]; int n = 0;
            for (size_t j = i >= 2 ? i - 2 : 0; j <= i + 2 && j < frames; ++j) if (raw[j] > 0 && n < 5) v[n++] = raw[j];
            for (int a = 1; a < n; ++a) for (int b = a; b > 0 && v[b] < v[b - 1]; --b) std::swap (v[b], v[b - 1]);   // (std::sort trips GCC's -Warray-bounds here)
            pt.f0[i] = n % 2 ? v[n / 2] : (std::abs (std::log2 (v[n / 2 - 1] / raw[i])) < std::abs (std::log2 (v[n / 2] / raw[i])) ? v[n / 2 - 1] : v[n / 2]);
        }
    }
    // remove single-frame octave glitches / dropouts
    for (size_t i = 1; i + 1 < frames; ++i)
    {
        double p = pt.f0[i - 1], q = pt.f0[i + 1], v = pt.f0[i];
        if (p > 0 && q > 0 && std::abs (std::log2 (p / q)) < 0.1 && (v == 0 || std::abs (std::log2 (v / p)) > 0.4)) pt.f0[i] = 0.5 * (p + q);
    }
    return pt;
}
} // namespace lab::dsp
