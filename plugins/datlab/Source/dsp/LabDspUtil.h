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
} // namespace lab::dsp
