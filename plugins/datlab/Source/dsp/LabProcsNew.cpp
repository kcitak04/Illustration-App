// DATLAB additions: 20 new processes on top of DATTRACK's Sample Lab.
// Tempo-synced processes read the tempo from Ctx::bpm (the host tempo, or the BPM box in the plugin).
#include "LabCore.h"

namespace lab::procs_new
{
namespace
{
// note divisions in beats, shared by the synced processes (same order as DIVS in LabDefs.cpp)
const double DIV_BEATS[] = { 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 0.75, 0.375 };
double divSec (int idx, double bpm) { return DIV_BEATS[clamp (idx, 0, 10)] * 60.0 / std::max (20.0, bpm); }

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
double lfoShape (int shape, double ph, double& sh, double& last, Ctx& cx)
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
Chans stereoOf (const Chans& ch) { return ch.size() >= 2 ? Chans { ch[0], ch[1] } : Chans { ch[0], ch[0] }; }
Buf monoOf (const Chans& ch) { Buf m (ch[0].size()); for (auto& x : ch) for (size_t i = 0; i < m.size(); ++i) m[i] = (float) (m[i] + x[i] / (double) ch.size()); return m; }
// windowed grain overlap-add pitch/time engine: srcAt(t) = read position for output time t, ratioAt(t) = playback speed
template <typename SrcFn, typename RatioFn>
Buf grainEngine (const Buf& x, size_t outLen, double sr, double grainSec, SrcFn srcAt, RatioFn ratioAt, bool loop)
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
} // namespace

// ===== spectral =====
// SPECTRAL DELAY: every frequency bin gets its own echo time (TILT: lows or highs later, RANDOM scatters them), with feedback
Chans sdelay (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const size_t N = pow2 (p["win"] * sr), H = N >> 2, half = N >> 1;
    const double fb = p["feedback"], maxT = p["time"];
    const double tailSec = std::min (20.0, maxT * (1 + 6 / std::max (0.05, 1 - fb)) * (fb > 0 ? 1 : 0.2) + maxT);
    const size_t outLen = std::min (cx.maxLen, len + (size_t) (tailSec * sr) + N);
    const size_t D = (size_t) std::max (1.0, std::ceil (maxT * sr / (double) H)) + 1;
    std::vector<int> dly (half + 1); cx.seed (p["seed"]);
    for (size_t k = 0; k <= half; ++k)
    {
        double u = (double) k / (double) half, tilt = p["tilt"], base = tilt >= 0 ? u : 1 - u;
        double d = (1 - std::abs (tilt)) * 0.5 + std::abs (tilt) * base;
        d = d * (1 - p["random"]) + cx.rand() * p["random"];
        dly[k] = (int) std::max (1.0, jsround (d * maxT * sr / (double) H));
    }
    return stftRun (ch, cx, p["win"], outLen, [&] (DVec& mag, DVec& ph, St& st, long long f, long long)
    {
        if (! st.init) { st.init = true; st.a.assign (D * (half + 1), 0); st.b.assign (D * (half + 1), 0); }
        const size_t slot = (size_t) f % D;
        for (size_t k = 0; k <= half; ++k)
        {
            double ir = mag[k] * std::cos (ph[k]), ii = mag[k] * std::sin (ph[k]);
            size_t rs = ((size_t) f + D - (size_t) std::min ((size_t) dly[k], D - 1)) % D;
            double dr = st.a[rs * (half + 1) + k], di = st.b[rs * (half + 1) + k];
            st.a[slot * (half + 1) + k] = ir + dr * fb; st.b[slot * (half + 1) + k] = ii + di * fb;
            double orr = ir * (1 - p["mix"]) + dr * p["mix"], oi = ii * (1 - p["mix"]) + di * p["mix"];
            mag[k] = std::hypot (orr, oi); ph[k] = std::atan2 (oi, orr);
        }
    });
}

// BIN SCRAMBLE: the spectrum is cut into bands that swap places at random, re-shuffled every HOLD
Chans scram (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const int B = std::max (1, p.i ("bands"));
    return stftRun (ch, cx, p["win"], len + pow2 (p["win"] * sr), [&] (DVec& mag, DVec& ph, St& st, long long f, long long)
    {
        const size_t half = st.half; const size_t nb = (half + 1 + (size_t) B - 1) / (size_t) B;
        const long long hold = std::max (1LL, jr (p["hold"] / 1000 * sr / (double) st.H));
        if (! st.init || f % hold == 0)
        {
            if (! st.init) { st.init = true; cx.seed (p["seed"] + st.c * 101); }
            st.keep.assign (nb, 0); st.a.resize (nb);
            for (size_t i = 0; i < nb; ++i) st.a[i] = (double) i;
            for (size_t i = 1; i < nb; ++i)
                if (cx.rand() < p["amount"])
                {
                    long long range = std::max (1LL, jr (p["range"] * (double) nb));
                    long long j = clamp ((long long) i + jr ((cx.rand() * 2 - 1) * (double) range), 1LL, (long long) nb - 1);
                    std::swap (st.a[i], st.a[(size_t) j]);
                }
        }
        DVec m2 = mag, p2 = ph;
        for (size_t b = 0; b < nb; ++b)
        {
            size_t from = (size_t) st.a[b];
            for (int q = 0; q < B; ++q)
            {
                size_t k = b * (size_t) B + (size_t) q, s = from * (size_t) B + (size_t) q;
                if (k <= half) { mag[k] = s <= half ? m2[s] : 0; ph[k] = s <= half ? p2[s] : 0; }
            }
        }
    });
}

// SPECTRAL ARP: a resonant band steps up and down the spectrum in time with the tempo
Chans sarp (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const int steps = std::max (2, p.i ("steps")), pat = p.i ("pattern");
    const double stepSec = divSec (p.i ("div"), cx.bpm), lo = p["lo"], hi = std::max (p["lo"] * 1.5, p["hi"]);
    std::vector<int> seq; cx.seed (p["seed"]);
    for (int i = 0; i < 64; ++i)
    {
        int s = 0;
        switch (pat)
        {
            case 0: s = i % steps; break;
            case 1: s = steps - 1 - i % steps; break;
            case 2: { int per = 2 * steps - 2; int q = i % per; s = q < steps ? q : per - q; break; }
            case 3: s = (int) std::floor (cx.rand() * steps); break;
            default: s = (i % 2) ? steps - 1 - (i / 2) % steps : (i / 2) % steps; break;
        }
        seq.push_back (s);
    }
    return stftRun (ch, cx, p["win"], len + pow2 (p["win"] * sr), [&] (DVec& mag, DVec&, St& st, long long, long long pos)
    {
        const size_t half = st.half; const double bin = sr / (double) st.N;
        double t = std::max (0.0, (double) (pos + (long long) half) / sr);
        int idx = seq[(size_t) ((long long) std::floor (t / stepSec) % 64)];
        double target = std::log2 (lo) + (std::log2 (hi) - std::log2 (lo)) * idx / (double) (steps - 1);
        if (! st.init) { st.init = true; st.a.assign (1, target); }
        double g = clampd (p["glide"], 0, 0.99); st.a[0] = st.a[0] * g + target * (1 - g);
        double c = st.a[0], wOct = std::max (0.02, p["width"]), boost = 1 + p["reso"] * 6;
        for (size_t k = 1; k <= half; ++k)
        {
            double d = (std::log2 ((double) k * bin) - c) / wOct, gain = std::exp (-d * d) * boost;
            mag[k] *= (1 - p["depth"]) + p["depth"] * gain;
        }
        mag[0] *= 1 - p["depth"];
    });
}

// HARMONIZER: formant-correct pitch-shifted voices layered into a chord, spread across the stereo field
Chans harm (const Chans& ch, Ctx& cx, const Params& p)
{
    static const std::vector<std::vector<double>> CH { { -12, 12 }, { -12, 7 }, { 4, 7 }, { 3, 7 }, { 2, 7 }, { 4, 7, 11 }, { 3, 7, 10 }, { 3, 6, 9 }, { -1, 1, 2 }, { 12, 19, 24 } };
    std::vector<double> iv = p.i ("chord") >= (int) CH.size() ? std::vector<double> { p["a"], p["b"] } : CH[(size_t) p.i ("chord")];
    const double sr = cx.sr; const size_t len = ch[0].size(), outLen = len + pow2 (p["win"] * sr);
    Chans dry = stereoOf (ch); Chans out (2, Buf (outLen));
    for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < len; ++i) out[c][i] = (float) (dry[c][i] * p["dry"]);
    Chans mono { monoOf (ch) };
    for (size_t v = 0; v < iv.size(); ++v)
    {
        double det = (v % 2 ? 1 : -1) * p["detune"] / 100.0, ratio = std::pow (2.0, (iv[v] + det) / 12);
        Ctx sub = cx; sub.progressFn = [&] (double f) { cx.prog (((double) v + f) / (double) iv.size()); };
        Chans y = stftRun (mono, sub, p["win"], outLen, [&] (DVec& mag, DVec& ph, St& st, long long, long long)
        {
            const size_t half = st.half; const double B = 250 / (sr / (double) st.N);
            if (! st.init) { st.init = true; st.a.assign (half + 1, 0); st.b.assign (half + 1, 0); }
            envelope (mag, B, st.a); pvMap (mag, ph, st, [&] (double tf) { return tf * ratio; }); envelope (mag, B, st.b);
            for (size_t k = 0; k <= half; ++k) mag[k] *= std::pow (std::min (30.0, st.a[k] / st.b[k]), p["formant"]);
        });
        double pan = iv.size() == 1 ? 0.5 : 0.5 + (((double) v / (double) (iv.size() - 1)) - 0.5) * p["spread"];
        auto [gl, gr] = panG (pan); double g = p["wet"] / std::sqrt ((double) iv.size());
        for (size_t i = 0; i < outLen; ++i) { out[0][i] = (float) (out[0][i] + y[0][i] * gl * g); out[1][i] = (float) (out[1][i] + y[0][i] * gr * g); }
    }
    return out;
}

// SHEPARD RISE: endless rising (or falling) pitch illusion built from octave-spaced layers of the looped source
Chans shepard (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t outLen = std::min (cx.maxLen, (size_t) jr (p["len"] * sr));
    const int L = std::max (2, p.i ("layers")); const double dir = p.i ("dir") ? -1 : 1, cyc = std::max (0.5, p["cycle"]);
    Chans src = stereoOf (ch); Chans out (2, Buf (outLen));
    for (int l = 0; l < L; ++l)
    {
        auto octAt = [&] (double t) { double o = std::fmod ((double) l + dir * t / sr / cyc, (double) L); if (o < 0) o += L; return o; };
        auto ratioAt = [&] (double t) { return std::pow (2.0, octAt (t) - L / 2.0 + p["center"] / 12); };
        auto srcAt = [&] (double t) { return t * p["speed"]; };
        for (int c = 0; c < 2; ++c)
        {
            Buf y = grainEngine (src[(size_t) c], outLen, sr, 0.09, srcAt, ratioAt, true);
            double pan = 0.5 + (((double) l / (double) (L - 1)) - 0.5) * p["spread"]; auto [gl, gr] = panG (pan);
            double pg = c == 0 ? gl : gr;
            for (size_t i = 0; i < outLen; ++i)
            {
                double o = octAt ((double) i), w = 0.5 - 0.5 * std::cos (TAU * o / L);
                out[(size_t) c][i] = (float) (out[(size_t) c][i] + y[i] * w * pg * 1.4);
            }
            cx.prog (((double) l + (c + 1) / 2.0) / L);
        }
    }
    fades (out[0], jsround (0.05 * sr), jsround (0.3 * sr)); fades (out[1], jsround (0.05 * sr), jsround (0.3 * sr));
    return out;
}

// ===== drive / digital =====
// DRIVE / FOLD: tube-ish saturation, wave folding, hard clip, rectify or fuzz, with 4x oversampling and tone control
Chans fold (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, drive = std::pow (10.0, p["drive"] / 20), bias = p["bias"]; const int mode = p.i ("mode"), stages = std::max (1, p.i ("stages"));
    auto shape = [&] (double v)
    {
        for (int s = 0; s < stages; ++s)
        {
            double x = v * drive + bias;
            switch (mode)
            {
                case 0: v = std::tanh (x); break;
                case 1: v = std::sin (x * PI / 2); break;
                case 2: v = clampd (x, -1, 1); break;
                case 3: v = std::tanh (std::abs (x) * 1.5) * 2 - 1; break;   // full-wave rectifier: octave-up fuzz
                default: v = x > 0 ? 1 - std::exp (-x) : -1 + std::exp (x * 0.6) * 0.8 - 0.2 * std::tanh (x); break;
            }
            v -= std::tanh (bias) * (mode == 3 ? 0 : 1) * 0.5;
        }
        return v;
    };
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (x.size()); SVF up, dn, tone; double prev = 0, dc = 0, dcy = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            double acc = 0;
            for (int o = 0; o < 4; ++o)
            {
                double v = prev + (x[i] - prev) * (o + 1) / 4.0;
                up.run (v, sr * 0.45, 0.7, sr * 4); v = shape (up.lp);
                dn.run (v, sr * 0.45, 0.7, sr * 4); acc = dn.lp;
            }
            prev = x[i];
            tone.run (acc, p["tone"], 0.7, sr);
            double y = tone.lp; dcy = y - dc + 0.995 * dcy; dc = y;   // DC blocker
            out[i] = (float) (dcy * p["mix"] + x[i] * (1 - p["mix"]));
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// RING MOD: multiplies the sound with a carrier (sine / triangle / square / saw / noise) that can sweep and wobble
Chans ring (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const int wave = p.i ("wave"); const bool am = p.i ("mode") == 1;
    Chans src = stereoOf (ch), res; cx.seed (p["seed"]);
    for (size_t c = 0; c < 2; ++c)
    {
        Buf out (len); double ph = c * p["stereo"] * 0.25, nz = 0, lph = 0;
        for (size_t i = 0; i < len; ++i)
        {
            double u = (double) i / (double) std::max<size_t> (1, len - 1);
            double f = p["freq"] * std::pow (p["end"] / p["freq"], u) * std::pow (2.0, p["lfodepth"] / 12 * std::sin (TAU * lph));
            lph += p["lforate"] / sr; ph += f / sr; ph -= std::floor (ph);
            double m;
            switch (wave)
            {
                case 0: m = std::sin (TAU * ph); break;
                case 1: m = 1 - 4 * std::abs (ph - 0.5); break;
                case 2: m = ph < 0.5 ? 1 : -1; break;
                case 3: m = 2 * ph - 1; break;
                default: nz += ((cx.rand() * 2 - 1) - nz) * std::min (1.0, f / sr * 4); m = nz * 2.5; break;
            }
            if (am) m = 0.5 + 0.5 * m;
            out[i] = (float) (src[c][i] * (m * p["mix"] + (1 - p["mix"])));
        }
        cx.prog ((double) (c + 1) / 2);
        res.push_back (std::move (out));
    }
    return res;
}

// DATAMOSH: the audio is treated as raw 16-bit data and corrupted: endian swaps, bit rotation, dropped bytes,
// stuck blocks, sorted samples and XOR noise
Chans mosh (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const long long B = std::max (8LL, jr (p["block"] / 1000 * sr));
    const int op = p.i ("op"); const double inten = p["intensity"];
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; std::vector<int16_t> d (len);
        for (size_t i = 0; i < len; ++i) d[i] = (int16_t) jr (clampd (x[i], -1, 1) * 32767);
        cx.seed (p["seed"] + (double) c * 7);
        std::vector<int16_t> lastBlock;
        for (long long a = 0; a < (long long) len; a += B)
        {
            long long b = std::min ((long long) len, a + B);
            std::vector<int16_t> keep (d.begin() + a, d.begin() + b);
            if (cx.rand() < p["chance"])
            {
                int o = op == 0 ? 1 + (int) std::floor (cx.rand() * 6) : op;
                switch (o)
                {
                    case 1: for (long long i = a; i < b; ++i) { uint16_t u = (uint16_t) d[(size_t) i]; d[(size_t) i] = (int16_t) (uint16_t) ((u >> 8) | (u << 8)); } break;
                    case 2: { int r = 1 + (int) (inten * 7); for (long long i = a; i < b; ++i) { uint16_t u = (uint16_t) d[(size_t) i]; d[(size_t) i] = (int16_t) (uint16_t) ((u << r) | (u >> (16 - r))); } break; }
                    case 3: for (long long i = a; i < b; ++i) if (cx.rand() < inten) d[(size_t) i] = (int16_t) (d[(size_t) i] & (cx.rand() < 0.5 ? 0xFF00 : 0x00FF)); break;
                    case 4: if (! lastBlock.empty()) for (long long i = a; i < b; ++i) d[(size_t) i] = lastBlock[(size_t) (i - a) % lastBlock.size()]; break;
                    case 5: { long long m = a + jr ((double) (b - a) * inten); std::sort (d.begin() + a, d.begin() + std::max (a, m)); break; }
                    default: { uint16_t mask = (uint16_t) (cx.rand() * 65535 * inten); for (long long i = a; i < b; ++i) d[(size_t) i] = (int16_t) ((uint16_t) d[(size_t) i] ^ (uint16_t) (mask & (uint16_t) (cx.rand() * 65535))); break; }
                }
            }
            lastBlock = keep;
        }
        Buf out (len);
        for (size_t i = 0; i < len; ++i) out[i] = (float) (d[i] / 32768.0 * p["mix"] + x[i] * (1 - p["mix"]));
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// ===== rhythm / time =====
// BEAT REPEAT: every INTERVAL, with CHANCE, a GRID-long slice is captured and repeated for GATE of the interval,
// each repeat pitched down and quieter (Ableton Beat Repeat style, rendered)
Chans beatrep (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size();
    const double iv = divSec (p.i ("interval"), cx.bpm) * sr, off = p["offset"] * 0.25 * 60 / cx.bpm * sr;
    Chans res; Chans out (ch.begin(), ch.end());
    cx.seed (p["seed"]);
    struct Hit { long long at, grid, end; };
    std::vector<Hit> hits;
    for (double t = off; t < (double) len; t += iv)
    {
        if (cx.rand() >= p["chance"]) continue;
        int gi = p.i ("grid"); if (cx.rand() < p["variation"]) gi = clamp (gi + (cx.rand() < 0.5 ? -1 : 1), 2, 5);
        long long grid = std::max (64LL, jr (divSec (gi, cx.bpm) * sr));
        hits.push_back ({ jr (t), grid, std::min ((long long) len, jr (t + iv * p["gate"])) });
    }
    const double fd = 0.002 * sr;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf& o = out[c];
        for (auto& h : hits)
        {
            int n = 0;
            for (long long s = h.at; s < h.end; s += h.grid, ++n)
            {
                double speed = std::pow (2.0, -p["pdecay"] * n / 12), g = std::pow (1 - p["vdecay"], n);
                for (long long i = 0; s + i < h.end && i < h.grid; ++i)
                {
                    double e = std::min ({ 1.0, (double) i / fd, (double) (h.grid - 1 - i) / fd });
                    o[(size_t) (s + i)] = (float) (lerpAt (x, (double) h.at + (double) i * speed) * g * e * p["mix"] + x[(size_t) (s + i)] * (1 - p["mix"]) * (p["insert"] > 0.5 ? 0 : 1));
                }
            }
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
    }
    return out;
}

// SHIMMER ECHO: tempo-synced ping-pong delay with a pitch shifter, filter and saturation inside the feedback loop
Chans echo (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size();
    const double dT = (p.i ("sync") == 0 ? p["time"] / 1000 : divSec (p.i ("sync") - 1, cx.bpm)) * sr, fb = p["feedback"];
    const size_t outLen = std::min (cx.maxLen, len + (size_t) (p["tail"] * sr));
    Chans src = stereoOf (ch); Chans out (2, Buf (outLen));
    Ring dl[2] { Ring ((size_t) (dT * 2 + sr * 0.2)), Ring ((size_t) (dT * 2 + sr * 0.2)) };
    Ring ps[2] { Ring ((size_t) (sr * 0.2)), Ring ((size_t) (sr * 0.2)) };
    OnePole lp[2], hpz[2]; const double ratio = std::pow (2.0, p["pitch"] / 12), win = 0.06 * sr; double phs = 0;
    for (size_t i = 0; i < outLen; ++i)
    {
        double in[2] = { i < len ? src[0][i] : 0.0, i < len ? src[1][i] : 0.0 }, wet[2];
        phs += (1 - ratio) / win; phs -= std::floor (phs);
        for (int c = 0; c < 2; ++c)
        {
            double d = dl[c].read (std::max (1.0, dT));
            // pitch shift of the repeat (two crossfaded sweeping taps)
            ps[c].push (d);
            double t1 = phs * win + 1, t2 = std::fmod (phs + 0.5, 1.0) * win + 1, g1 = std::sin (PI * phs), g2 = std::sin (PI * std::fmod (phs + 0.5, 1.0));
            double sh = p["pitch"] == 0 ? d : ps[c].read (t1) * g1 + ps[c].read (t2) * g2;
            double f = lp[c].lp (sh, 20000 * std::pow (0.02, p["damp"]), sr); f -= hpz[c].lp (f, 120, sr);
            wet[c] = std::tanh (f * 1.2) / 1.2;
        }
        double pp = p["pingpong"];
        for (int c = 0; c < 2; ++c)
        {
            double cross = wet[1 - c] * pp + wet[c] * (1 - pp), inj = c == 0 ? (in[0] + in[1]) * 0.5 * pp + in[0] * (1 - pp) : in[1] * (1 - pp);
            dl[c].push (inj + cross * fb);
            out[(size_t) c][i] = (float) (in[c] * (1 - p["mix"] * 0.5) + wet[c] * p["mix"]);
        }
        if (! (i & 65535)) cx.prog ((double) i / (double) outLen);
    }
    return out;
}

// TAPE WEAR: wow, flutter, saturation, hiss, dropouts and age (lost highs) of a worn cassette
Chans wear (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); Chans res;
    cx.seed (p["seed"]);
    DVec drift (len), drop (len);
    { double dv = 0, dd = 1, target = 1, cnt = 0;
      for (size_t i = 0; i < len; ++i)
      {
          if (! (i & 1023)) dv += ((cx.rand() * 2 - 1) - dv) * 0.05;
          drift[i] = dv;
          if (cnt <= 0) { target = cx.rand() < p["dropouts"] * 0.15 ? 0.05 + cx.rand() * 0.4 : 1; cnt = sr * (0.03 + cx.rand() * 0.15); }
          cnt--; dd += (target - dd) * 0.003; drop[i] = dd;
      } }
    const double maxD = (p["wow"] + p["flutter"]) / 1000 * sr + 8;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (len); Ring r ((size_t) (maxD * 2 + 16)); SVF age; double hiss = 0;
        for (size_t i = 0; i < len; ++i)
        {
            double t = (double) i / sr;
            double mod = p["wow"] / 1000 * sr * 0.5 * (1 + std::sin (TAU * p["wowrate"] * t) * 0.7 + drift[i] * 0.3)
                         + p["flutter"] / 1000 * sr * 0.5 * (1 + std::sin (TAU * p["flutrate"] * t + c));
            r.push (x[i]);
            double v = r.read (2 + mod);
            v = std::tanh (v * (1 + p["sat"] * 4)) / (1 + p["sat"] * 1.5);
            age.run (v, 18000 * std::pow (0.12, p["age"]), 0.6, sr); v = age.lp;
            hiss += ((cx.rand() * 2 - 1) - hiss) * 0.6;
            out[i] = (float) (v * drop[i] + hiss * std::pow (10.0, p["hiss"] / 20));
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// SCRATCH: turntablism on the sample - baby, chirp, transformer, tear, flare and random DJ patterns synced to the tempo
Chans scratch (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const double len = (double) ch[0].size();
    const double cycle = divSec (p.i ("div"), cx.bpm) * sr; const size_t outLen = std::min (cx.maxLen, (size_t) jr (p["bars"] * 4 * 60 / cx.bpm * sr));
    const double start = p["pos"] * len, range = p["range"] / 1000 * sr; const int pat = p.i ("pattern");
    DVec pos (outLen), fader (outLen); cx.seed (p["seed"]);
    double amp = 1, spd = 1;
    for (size_t i = 0; i < outLen; ++i)
    {
        double ci = std::floor ((double) i / cycle), u = (double) i / cycle - ci;
        if (u * cycle < 1) { amp = pat == 5 ? 0.3 + cx.rand() * 0.9 : 1; spd = pat == 5 ? (cx.rand() < 0.5 ? 1 : 2) : 1; }
        double m = 0, f = 1;
        switch (pat)
        {
            case 0: m = 0.5 - 0.5 * std::cos (TAU * u); break;                                         // BABY: forward and back
            case 1: m = 0.5 - 0.5 * std::cos (TAU * u); f = u < 0.5 ? 1 : (u < 0.55 ? 0 : 0); break;     // CHIRP: fader closes on the way back
            case 2: m = 0.5 - 0.5 * std::cos (TAU * u); f = std::fmod (u * 8, 1.0) < 0.5 ? 1 : 0; break; // TRANSFORMER: fader gating
            case 3: m = u < 0.33 ? u / 0.33 * 0.5 : u < 0.5 ? 0.5 + (u - 0.33) / 0.17 * 0.5 : 1 - (u - 0.5) * 2; break;   // TEAR: 2-step push, 1 pull
            case 4: m = 0.5 - 0.5 * std::cos (TAU * u); f = (u > 0.2 && u < 0.3) || (u > 0.7 && u < 0.8) ? 0 : 1; break; // FLARE: clicks open/close
            default: m = 0.5 - 0.5 * std::cos (TAU * std::fmod (u * spd, 1.0)); f = std::fmod (u * 4, 1.0) < 0.7 ? 1 : 0; break;
        }
        pos[i] = clampd (start + m * range * amp, 0, len - 1); fader[i] = f;
    }
    Chans res; double fs = std::exp (-1.0 / (0.002 * sr));
    for (auto& x : ch)
    {
        Buf out (outLen); double g = 0;
        for (size_t i = 0; i < outLen; ++i) { g = fader[i] * (1 - fs) + g * fs; out[i] = (float) (lerpAt (x, pos[i]) * g); }
        res.push_back (std::move (out));
    }
    return res;
}

// PITCH RAMP: glide the pitch from START to END semitones like a riser / dive bomb (TAPE changes the length, KEEP LENGTH doesn't)
Chans ramp (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const double len = (double) ch[0].size();
    auto stAt = [&] (double u) { return p["start"] + (p["end"] - p["start"]) * std::pow (clampd (u, 0, 1), p["curve"]); };
    Chans res;
    if (p.i ("mode") == 0)
    {
        DVec path; double pos = 0;
        while (pos < len && path.size() < cx.maxLen) { path.push_back (pos); pos += std::pow (2.0, stAt (pos / len) / 12); }
        for (auto& x : ch) { Buf o (path.size()); for (size_t i = 0; i < o.size(); ++i) o[i] = (float) lerpAt (x, path[i]); fades (o, 0, jsround (0.005 * sr)); res.push_back (std::move (o)); }
        return res;
    }
    for (size_t c = 0; c < ch.size(); ++c)
    {
        res.push_back (grainEngine (ch[c], (size_t) len, sr, p["grain"] / 1000, [&] (double t) { return t; }, [&] (double t) { return std::pow (2.0, stAt (t / len) / 12); }, false));
        cx.prog ((double) (c + 1) / (double) ch.size());
    }
    return res;
}

// TRANCE GATE: rhythmic volume chopping from step patterns or Euclidean rhythms, with swing and shaped steps
Chans tgate (const Chans& ch, Ctx& cx, const Params& p)
{
    static const char* PAT[] = { "x.xx.xx.x.xx.x.x", "..x...x...x...x.", "xxx.xxx.xxx.x.x.", "x.......x.......", "xx.xx.xx.xx.x.xx" };
    const double sr = cx.sr; const size_t len = ch[0].size(); const int pat = p.i ("pattern");
    const double step = divSec (p.i ("div"), cx.bpm) * sr; std::vector<bool> on (16);
    cx.seed (p["seed"]);
    if (pat < 5) for (int i = 0; i < 16; ++i) on[(size_t) i] = PAT[pat][i] == 'x';
    else if (pat == 5) for (int i = 0; i < 16; ++i) on[(size_t) i] = cx.rand() < 0.55;
    else { int k = clamp (p.i ("hits"), 1, 16); for (int i = 0; i < 16; ++i) on[(size_t) i] = ((i * k) % 16) < k; }
    Chans res; const double atk = std::max (1.0, p["attack"] / 1000 * sr), rel = std::max (1.0, p["release"] / 1000 * sr);
    DVec env (len);
    for (size_t i = 0; i < len; ++i)
    {
        double s = (double) i / step; long long si = (long long) std::floor (s);
        double sw = (si % 2) ? p["swing"] * 0.5 : 0, u = (s - (double) si - sw) * step;
        if (u < 0) { si -= 1; u += step; }
        bool o = on[(size_t) (((si % 16) + 16) % 16)];
        double g = o ? std::min (1.0, u / atk) * std::exp (-std::max (0.0, u - atk) / rel) : 0;
        env[i] = 1 - p["depth"] + p["depth"] * g;
    }
    for (auto& x : ch) { Buf o (len); for (size_t i = 0; i < len; ++i) o[i] = (float) (x[i] * env[i]); res.push_back (std::move (o)); }
    return res;
}

// ===== space / filter =====
// CLOUD REVERB: 8-line feedback delay network with modulation, damping, pre-delay and an octave-up SHIMMER in the loop
Chans cloud (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size();
    const size_t outLen = std::min (cx.maxLen, len + (size_t) (std::min (20.0, p["decay"] * 1.2 + p["predelay"] / 1000) * sr));
    static const double BASE[8] = { 0.0297, 0.0371, 0.0411, 0.0437, 0.0533, 0.0617, 0.0713, 0.0797 };
    double dly[8]; std::vector<Ring> lines; OnePole damp[8];
    for (int k = 0; k < 8; ++k) { dly[k] = BASE[k] * sr * p["size"] * 2.2; lines.emplace_back ((size_t) (dly[k] + sr * 0.02 + 8)); }
    Ring pre ((size_t) (p["predelay"] / 1000 * sr + 8)); Ring shim ((size_t) (sr * 0.2));
    Chans src = stereoOf (ch); Chans out (2, Buf (outLen)); double shPh = 0; const double shWin = 0.08 * sr;
    for (size_t i = 0; i < outLen; ++i)
    {
        double inL = i < len ? src[0][i] : 0, inR = i < len ? src[1][i] : 0;
        pre.push ((inL + inR) * 0.5); double in = pre.read (std::max (1.0, p["predelay"] / 1000 * sr));
        double v[8], sum = 0;
        for (int k = 0; k < 8; ++k)
        {
            double mod = std::sin (TAU * (0.11 + k * 0.07) * (double) i / sr + k) * p["mod"] * 0.004 * sr;
            v[k] = lines[(size_t) k].read (dly[k] + mod + 2);
            double g = std::pow (0.001, (dly[k] / sr) / std::max (0.1, p["decay"]));
            v[k] = damp[k].lp (v[k], 18000 * std::pow (0.05, p["damp"]), sr) * g; sum += v[k];
        }
        // shimmer: octave-up copy of the network output fed back in
        shim.push (sum * 0.25); shPh += (1 - 2.0) / shWin; shPh -= std::floor (shPh);
        double s2 = shim.read (shPh * shWin + 1) * std::sin (PI * shPh) + shim.read (std::fmod (shPh + 0.5, 1.0) * shWin + 1) * std::sin (PI * std::fmod (shPh + 0.5, 1.0));
        // Hadamard mix
        double h[8]; for (int k = 0; k < 8; ++k) h[k] = v[k];
        for (int s = 1; s < 8; s <<= 1) for (int k = 0; k < 8; ++k) if (! (k & s)) { double a = h[k], b = h[k + s]; h[k] = a + b; h[k + s] = a - b; }
        for (int k = 0; k < 8; ++k) lines[(size_t) k].push (h[k] / std::sqrt (8.0) + in * 0.35 + s2 * p["shimmer"] * 0.5);
        double wl = (v[0] + v[2] + v[4] + v[6]) * 0.5, wr = (v[1] + v[3] + v[5] + v[7]) * 0.5;
        double mid = (wl + wr) * 0.5, side = (wl - wr) * 0.5 * p["width"];
        out[0][i] = (float) (inL * (1 - p["mix"]) + (mid + side) * p["mix"]);
        out[1][i] = (float) (inR * (1 - p["mix"]) + (mid - side) * p["mix"]);
        if (! (i & 65535)) cx.prog ((double) i / (double) outLen);
    }
    return out;
}

// FLANGE / PHASE: flanger, through-zero flanger, multi-stage phaser or wide chorus with feedback
Chans flange (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const int mode = p.i ("mode"); Chans src = stereoOf (ch), res;
    for (size_t c = 0; c < 2; ++c)
    {
        const Buf& x = src[c]; Buf out (len); Ring r ((size_t) (0.05 * sr)); double fbv = 0; DVec ap (24, 0.0);
        for (size_t i = 0; i < len; ++i)
        {
            double l = 0.5 + 0.5 * std::sin (TAU * p["rate"] * (double) i / sr + c * p["spread"] * PI), y;
            if (mode == 2)
            {
                double fc = 200 * std::pow (2.0, l * p["depth"] * 6), g = std::tan (PI * std::min (fc, sr * 0.45) / sr), a = (g - 1) / (g + 1);
                double v = x[i] + fbv * p["feedback"];
                for (int s = 0; s < p.i ("stages"); ++s) { double o = a * v + ap[(size_t) s]; ap[(size_t) s] = v - a * o; v = o; }
                fbv = v; y = (x[i] + v) * 0.5;
            }
            else
            {
                double base = mode == 3 ? 0.012 * sr : 0.0005 * sr, d = base + l * p["depth"] * (mode == 3 ? 0.012 : 0.005) * sr;
                r.push (x[i] + fbv * p["feedback"]);
                double del = r.read (std::max (1.0, d));
                if (mode == 1) { double dz = r.read (std::max (1.0, 0.005 * sr * p["depth"] * 0.5 + 1)); y = (dz - del) * 0.7; }
                else y = (x[i] + del) * 0.5;
                fbv = del;
            }
            out[i] = (float) (y * p["mix"] + x[i] * (1 - p["mix"]));
        }
        cx.prog ((double) (c + 1) / 2);
        res.push_back (std::move (out));
    }
    return res;
}

// WOBBLE FILTER: resonant LP / BP / HP / notch with a tempo-synced LFO (sine..sample & hold), envelope follower and drive
Chans wobble (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const int type = p.i ("type"), shape = p.i ("shape");
    const double rate = p.i ("sync") == 0 ? p["rate"] : 1.0 / divSec (p.i ("sync") - 1, cx.bpm);
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (len); SVF f; double env = 0, sh = 0, last = 1; cx.seed (p["seed"]);
        for (size_t i = 0; i < len; ++i)
        {
            double a = std::abs (x[i]); env += (a - env) * (a > env ? 0.01 : 0.0005);
            double l = lfoShape (shape, rate * (double) i / sr, sh, last, cx);
            double fc = p["cutoff"] * std::pow (2.0, l * p["depth"] * 0.5 + p["env"] * env * 6);
            double in = std::tanh (x[i] * (1 + p["drive"] * 6)) / (1 + p["drive"] * 2);
            f.run (in, fc, 0.5 + p["reso"] * 14, sr);
            double y = type == 0 ? f.lp : type == 1 ? f.bp : type == 2 ? f.hp : f.lp + f.hp;
            out[i] = (float) (y * p["mix"] + x[i] * (1 - p["mix"]));
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// VOWEL / TALKBOX: formant filter bank that sings A-E-I-O-U, static, gliding A->B, wobbling or stepping to the tempo
Chans vowel (const Chans& ch, Ctx& cx, const Params& p)
{
    // formant frequencies (Hz) and gains for A E I O U, male and female voices
    static const double F[2][5][3] = { { { 730, 1090, 2440 }, { 530, 1840, 2480 }, { 270, 2290, 3010 }, { 570, 840, 2410 }, { 300, 870, 2240 } },
                                       { { 850, 1220, 2810 }, { 610, 2330, 2990 }, { 310, 2790, 3310 }, { 590, 920, 2710 }, { 370, 950, 2670 } } };
    static const double G[3] = { 1.0, 0.5, 0.25 };
    const double sr = cx.sr; const size_t len = ch[0].size(); const int va = p.i ("a"), vb = p.i ("b"), mode = p.i ("motion"), voice = p.i ("voice");
    const double stepSec = divSec (p.i ("div"), cx.bpm);
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (len); SVF bank[3];
        for (size_t i = 0; i < len; ++i)
        {
            double t = (double) i / sr, u;
            switch (mode)
            {
                case 0: u = 0; break;
                case 1: u = (double) i / (double) std::max<size_t> (1, len); break;
                case 2: u = 0.5 - 0.5 * std::cos (TAU * t / stepSec / 2); break;
                default: u = ((long long) std::floor (t / stepSec) % 2) ? 1 : 0; break;
            }
            double y = 0;
            for (int k = 0; k < 3; ++k)
            {
                double fa = F[voice][va][k], fb = F[voice][vb][k], fc = std::exp (std::log (fa) + (std::log (fb) - std::log (fa)) * u) * p["shift"];
                bank[k].run (x[i], fc, 2 + p["reso"] * 25, sr); y += bank[k].bp * G[k];
            }
            out[i] = (float) (y * 1.6 * p["mix"] + x[i] * (1 - p["mix"]));
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// SQUASH: OTT-style 3-band upward + downward compression: everything loud, detailed and in your face
Chans squash (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const double depth = p["depth"];
    const double atk = std::exp (-1 / (0.003 * p["time"] * sr)), rel = std::exp (-1 / (0.08 * p["time"] * sr));
    const double bandG[3] = { std::pow (10.0, p["low"] / 20), std::pow (10.0, p["midg"] / 20), std::pow (10.0, p["high"] / 20) };
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (len); SVF x1a, x1b, x2a, x2b; double env[3] = { 0, 0, 0 }, gs[3] = { 0, 0, 0 };
        const double look = std::max (1.0, 0.004 * p["time"] * sr);   // lookahead: gain moves before the transient arrives
        std::vector<Ring> dl; for (int k = 0; k < 3; ++k) dl.emplace_back ((size_t) look + 8);
        const double gsm = std::exp (-1 / (0.0015 * sr));
        for (size_t i = 0; i < len; ++i)
        {
            x1a.run (x[i], p["xlo"], 0.5, sr); x1b.run (x1a.lp, p["xlo"], 0.5, sr);
            double low = x1b.lp, rest = x[i] - low;
            x2a.run (rest, p["xhi"], 0.5, sr); x2b.run (x2a.lp, p["xhi"], 0.5, sr);
            double mid = x2b.lp, high = rest - mid, b[3] = { low, mid, high }, y = 0;
            for (int k = 0; k < 3; ++k)
            {
                double a = std::abs (b[k]); env[k] = a > env[k] ? a + (env[k] - a) * atk : a + (env[k] - a) * rel;
                double db = 20 * std::log10 (env[k] + 1e-9), gdb = 0;
                if (db > -18) gdb -= (db + 18) * (1 - 1 / 6.0) * p["down"];              // downward 6:1 above -18 dB
                else if (db > -70) gdb += std::min (30.0, (-18 - db) * 0.6) * p["up"];   // upward below -18 dB
                gdb *= depth;
                gs[k] = gdb < gs[k] ? gdb : gdb + (gs[k] - gdb) * gsm;                  // instant reduction, smooth release
                dl[(size_t) k].push (b[k]);
                y += dl[(size_t) k].read (look) * std::pow (10.0, gs[k] / 20) * bandG[k];
            }
            y = std::tanh (y);
            out[i] = (float) (y * std::pow (10.0, p["out"] / 20));
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// CRYSTALLIZE: reverse, pitch-shifted grain echoes that feed back into themselves: glittering, backwards, octave-climbing tails
Chans crystal (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size();
    const size_t outLen = std::min (cx.maxLen, len + (size_t) (p["tail"] * sr));
    const long long G = std::max (64LL, jr (p["size"] / 1000 * sr)), hop = G / 2; const double dT = p["delay"] / 1000 * sr, ratio = std::pow (2.0, p["pitch"] / 12);
    const DVec win = hann ((size_t) G);
    Chans src = stereoOf (ch); Chans out (2, Buf (outLen)); cx.seed (p["seed"]);
    std::vector<Buf> buf (2, Buf (outLen + (size_t) G * 4));
    struct Gr { long long start; double from; bool rev; double pan; };
    std::vector<Gr> active;
    for (size_t i = 0; i < outLen; ++i)
    {
        if ((long long) i % hop == 0 && (double) i > dT)
        {
            double from = (double) i - dT - (double) G * ratio; if (from < 0) from = 0;
            active.push_back ({ (long long) i, from, cx.rand() < p["reverse"], 0.5 + (cx.rand() - 0.5) * p["spread"] });
        }
        double wet[2] = { 0, 0 };
        for (auto& g : active)
        {
            long long k = (long long) i - g.start; if (k >= G) continue;
            double rp = g.rev ? g.from + (double) (G - 1 - k) * ratio : g.from + (double) k * ratio;
            auto [gl, gr] = panG (g.pan);
            for (int c = 0; c < 2; ++c) wet[c] += lerpAt (buf[(size_t) c], rp) * win[(size_t) k] * (c ? gr : gl) * 1.2;
        }
        active.erase (std::remove_if (active.begin(), active.end(), [&] (const Gr& g) { return (long long) i - g.start >= G - 1; }), active.end());
        for (int c = 0; c < 2; ++c)
        {
            double in = i < len ? src[(size_t) c][i] : 0;
            buf[(size_t) c][i] = (float) std::tanh (in + wet[c] * p["feedback"]);
            out[(size_t) c][i] = (float) (in * (1 - p["mix"] * 0.5) + wet[c] * p["mix"]);
        }
        if (! (i & 16383)) cx.prog ((double) i / (double) outLen);
    }
    return out;
}
} // namespace lab::procs_new
