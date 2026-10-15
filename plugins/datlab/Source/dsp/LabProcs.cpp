// The 31 original DATTRACK Sample Lab processes, ported line by line from the JS worker.
#include "LabCore.h"

namespace lab::procs_orig
{
// extreme time stretch with random phases (Paul's stretch)
Chans paul (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, stretch = p["stretch"];
    const size_t N = pow2 (p["win"] * sr), H = N >> 2; const DVec w = hann (N); const size_t len = ch[0].size();
    const size_t outLen = std::min (cx.maxLen, (size_t) jr ((double) len * stretch) + N);
    const long long frames = (long long) std::ceil ((double) outLen / (double) H) + 1;
    DVec re (N), im (N); Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        Buf out (outLen); cx.seed (p["seed"]);
        for (long long f = 0; f < frames; ++f)
        {
            long long opos = f * (long long) H - (long long) (N >> 1);
            long long ipos = jr ((double) (opos + (long long) (N >> 1)) / stretch - (double) (N >> 1));
            analyze (ch[c], ipos, w, re, im);
            for (size_t k = 0; k <= (N >> 1); ++k) { double m = std::hypot (re[k], im[k]), ph = cx.rand() * TAU; re[k] = m * std::cos (ph); im[k] = m * std::sin (ph); }
            mirror (re, im); synth (out, opos, w, re, im, 2.0 / 3.0);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        res.push_back (std::move (out));
    }
    return res;
}

// hold one moment of the sound forever: an endless drone of its spectrum
Chans freeze (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr;
    const size_t N = pow2 (0.19 * sr), H = N >> 2, len = ch[0].size(), half = N >> 1; const DVec w = hann (N);
    const size_t outLen = std::min (cx.maxLen, (size_t) jr (p["len"] * sr));
    DVec re (N), im (N);
    const double span = (double) std::max<long long> (0, (long long) len - (long long) N);
    const long long at = jr (clampd (p["pos"], 0, 1) * span), frames = (long long) std::ceil ((double) outLen / (double) H) + 1;
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c];
        DVec mag (half + 1);
        for (long long o : { -(long long) H, 0LL, (long long) H }) { analyze (x, at + o, w, re, im); for (size_t k = 0; k <= half; ++k) mag[k] += std::hypot (re[k], im[k]) / 3; }
        Buf out (outLen); DVec drift (half + 1, 1.0); cx.seed (p["seed"]);
        for (long long f = 0; f < frames; ++f)
        {
            if (p["scan"] > 0)
            {
                long long pf = jr (clampd (p["pos"] + p["scan"] * (1 - p["pos"]) * (double) f / (double) frames, 0, 1) * span);
                analyze (x, pf, w, re, im);
                for (size_t k = 0; k <= half; ++k) mag[k] = mag[k] * 0.85 + std::hypot (re[k], im[k]) * 0.15;
            }
            for (size_t k = 0; k <= half; ++k)
            {
                drift[k] += ((1 + (cx.rand() * 2 - 1) * p["shimmer"] * 1.5) - drift[k]) * 0.25;
                double m = mag[k] * std::max (0.0, drift[k]), ph = cx.rand() * TAU; re[k] = m * std::cos (ph); im[k] = m * std::sin (ph);
            }
            mirror (re, im); synth (out, f * (long long) H - (long long) half, w, re, im, 2.0 / 3.0);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        fades (out, jsround (0.08 * sr), jsround (std::min (1.5, p["len"] / 3) * sr));
        res.push_back (std::move (out));
    }
    return res;
}

// smear the spectrum over time: sounds melt into washes
Chans blur (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, amount = p["amount"];
    const size_t N = pow2 (p["win"] * sr), H = N >> 2, len = ch[0].size(), half = N >> 1; const DVec w = hann (N);
    const double a = std::pow (amount, 0.25) * 0.995;
    const size_t tail = (size_t) jr (std::min (10.0, -std::log (1000.0) * (double) H / sr / std::log (std::max (1e-6, a))) * sr);
    const size_t outLen = std::min (cx.maxLen, len + tail + N); const long long frames = (long long) std::ceil ((double) outLen / (double) H);
    DVec re (N), im (N); Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        Buf out (outLen); DVec M (half + 1); cx.seed (p["seed"]);
        for (long long f = 0; f < frames; ++f)
        {
            long long pos = f * (long long) H - (long long) half;
            analyze (ch[c], pos, w, re, im);
            for (size_t k = 0; k <= half; ++k)
            {
                double m = std::hypot (re[k], im[k]);
                M[k] = std::max (M[k] * a + m * (1 - a), m * (1 - amount));
                double ph = cx.rand() < p["diffuse"] ? cx.rand() * TAU : std::atan2 (im[k], re[k]);
                re[k] = M[k] * std::cos (ph); im[k] = M[k] * std::sin (ph);
            }
            mirror (re, im); synth (out, pos, w, re, im, 2.0 / 3.0);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        res.push_back (std::move (out));
    }
    return res;
}

// phase-vocoder spectral shift (inharmonic, in Hz) and spectral stretch (harmonic warp)
Chans shift (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr;
    const size_t N = pow2 (p["win"] * sr), H = N >> 2, len = ch[0].size(), half = N >> 1; const DVec w = hann (N);
    const long long frames = (long long) std::ceil ((double) (len + N) / (double) H);
    DVec re (N), im (N); const double binHz = sr / (double) N, exp = TAU * (double) H / (double) N;
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        Buf out (len + N); DVec last (half + 1), acc (half + 1), nm (half + 1), nf (half + 1);
        for (long long f = 0; f < frames; ++f)
        {
            long long pos = f * (long long) H - (long long) half;
            analyze (ch[c], pos, w, re, im);
            std::fill (nm.begin(), nm.end(), 0.0); std::fill (nf.begin(), nf.end(), 0.0);
            for (size_t k = 0; k <= half; ++k)
            {
                double m = std::hypot (re[k], im[k]), ph = std::atan2 (im[k], re[k]);
                double d = ph - last[k] - (double) k * exp; last[k] = ph;
                d -= TAU * jsround (d / TAU);
                double tf = (double) k + d / exp, nfreq = tf * p["stretch"] + p["hz"] / binHz; long long j = jr (nfreq);
                if (j > 0 && j <= (long long) half && m > nm[(size_t) j]) { nm[(size_t) j] = m; nf[(size_t) j] = nfreq; }
            }
            for (size_t k = 0; k <= half; ++k) { acc[k] += nf[k] * exp; re[k] = nm[k] * std::cos (acc[k]); im[k] = nm[k] * std::sin (acc[k]); }
            mirror (re, im); synth (out, pos, w, re, im, 2.0 / 3.0);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        res.push_back (std::move (out));
    }
    return res;
}

// ROBOT: every frame's phases zeroed, frames spaced one pitch period apart. WHISPER: random phases, tiny frames
Chans robot (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const bool whisper = p.i ("mode") == 1;
    const size_t N = whisper ? pow2 (0.012 * sr) : pow2 (p["win"] * sr);
    const size_t H = whisper ? N >> 3 : (size_t) std::max (8LL, jr (sr / p["pitch"]));
    const DVec w = hann (N); const size_t len = ch[0].size();
    const long long frames = (long long) std::ceil ((double) (len + N) / (double) H);
    const double g = std::min (1.0, (double) H / (0.375 * (double) N));
    DVec re (N), im (N), t (N); Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        Buf out (len + N); cx.seed (p["seed"]);
        for (long long f = 0; f < frames; ++f)
        {
            long long pos = f * (long long) H - (long long) (N >> 1);
            analyze (ch[c], pos, w, re, im);
            for (size_t k = 0; k <= (N >> 1); ++k)
            {
                double m = std::hypot (re[k], im[k]);
                if (whisper) { double ph = cx.rand() * TAU; re[k] = m * std::cos (ph); im[k] = m * std::sin (ph); }
                else { re[k] = m; im[k] = 0; }
            }
            mirror (re, im);
            if (! whisper)
            {   // zero phase puts the grain's energy at the frame edges: rotate it to the centre
                fft (re, im, true);
                t = re;
                for (size_t i = 0; i < N; ++i) { re[i] = t[(i + (N >> 1)) % N]; im[i] = 0; }
                for (size_t i = 0; i < N; ++i) { long long j = pos + (long long) i; if (j >= 0 && j < (long long) out.size()) out[(size_t) j] = (float) (out[(size_t) j] + re[i] * w[i] * g); }
            }
            else synth (out, pos, w, re, im, g);
            if (! (f & 63)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        res.push_back (std::move (out));
    }
    return res;
}

// cloud of tiny grains scattered over time, pitch and stereo
Chans grain (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size();
    const size_t outLen = std::min (cx.maxLen, (size_t) jr ((double) len * p["lenx"]));
    const long long n = std::max (1LL, jr (p["density"] * (double) outLen / sr));
    Buf L (outLen), R (outLen); const Buf &xl = ch[0], &xr = chOr (ch, 1);
    cx.seed (p["seed"]);
    for (long long g = 0; g < n; ++g)
    {
        long long t0 = jr (((double) g + cx.rand()) / (double) n * (double) outLen);
        long long gs = std::max (16LL, jr (p["size"] / 1000 * sr * (0.5 + cx.rand())));
        double ratio = std::pow (2.0, (cx.rand() * 2 - 1) * p["pitch"] / 12), dir = cx.rand() < p["rev"] ? -1 : 1;
        double src = ((double) t0 / (double) outLen) * (double) len + (cx.rand() * 2 - 1) * p["jitter"] * (double) len * 0.5;
        double pan = 0.5 + (cx.rand() - 0.5) * 0.9, gl = std::cos (pan * PI / 2), gr = std::sin (pan * PI / 2);
        for (long long i = 0; i < gs; ++i)
        {
            long long o = t0 - (gs >> 1) + i; if (o < 0 || o >= (long long) outLen) continue;
            double e = 0.5 - 0.5 * std::cos (TAU * (double) i / (double) gs), sp = src + ((double) i - (double) gs / 2) * ratio * dir;
            L[(size_t) o] = (float) (L[(size_t) o] + lerpAt (xl, sp) * e * gl); R[(size_t) o] = (float) (R[(size_t) o] + lerpAt (xr, sp) * e * gr);
        }
        if (! (g & 255)) cx.prog ((double) g / (double) n);
    }
    return { L, R };
}

// chop into steps, re-trigger / reverse / pitch-drop some of them
Chans stutter (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const long long len = (long long) ch[0].size();
    const long long sl = std::max (64LL, (long long) std::floor ((double) len / p["div"])); const double fd = jsround (0.002 * sr);
    struct Pl { bool on; int r; bool rev, drop; };
    std::vector<Pl> plan; cx.seed (p["seed"]);
    for (long long s = 0; s * sl < len; ++s)
    {
        if (cx.rand() < p["chance"])
        {
            Pl q { true, 0, false, false };
            q.r = 2 + (int) std::floor (cx.rand() * (p["reps"] - 1)); q.rev = cx.rand() < p["rev"]; q.drop = cx.rand() < p["drop"];
            plan.push_back (q);
        }
        else plan.push_back ({ false, 0, false, false });
    }
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out = x;
        for (size_t s = 0; s < plan.size(); ++s)
        {
            const auto& pl = plan[s]; if (! pl.on) continue;
            long long a = (long long) s * sl, b = std::min (len, a + sl), pl2 = std::max (16LL, (long long) std::floor ((double) (b - a) / pl.r));
            for (long long j = 0; j * pl2 < b - a; ++j)
            {
                double speed = pl.drop ? 1 - 0.6 * (double) j / pl.r : 1;
                for (long long i = 0; i < pl2 && a + j * pl2 + i < b; ++i)
                {
                    double e = std::min ({ 1.0, (double) i / fd, (double) (pl2 - 1 - i) / fd }); long long si = pl.rev ? pl2 - 1 - i : i;
                    out[(size_t) (a + j * pl2 + i)] = (float) (lerpAt (x, (double) a + (double) si * speed) * e);
                }
            }
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// turntable / tape motor stop (and spin-up)
Chans tape (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const double len = (double) ch[0].size(); const int mode = p.i ("mode");
    const long long T = std::max (16LL, jr (p["time"] * sr)); const double stopAt = mode == 1 ? len : jsround (p["at"] * len);
    auto speedAt = [&] (double u, bool up) { return up ? std::pow (u, p["curve"]) : std::pow (1 - u, p["curve"]); };
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out; double pos = 0;
        if (mode >= 1) for (long long t = 0; t < T && pos < len; ++t) { out.push_back ((float) lerpAt (x, pos)); pos += speedAt ((double) t / (double) T, true); }
        while (pos < stopAt && pos < len) { out.push_back ((float) lerpAt (x, pos)); pos += 1; }
        if (mode != 1) for (long long t = 0; t < T && pos < len; ++t) { out.push_back ((float) lerpAt (x, pos)); pos += speedAt ((double) t / (double) T, false); }
        cx.prog ((double) (c + 1) / (double) ch.size());
        fades (out, 0, jsround (0.004 * sr));
        res.push_back (std::move (out));
    }
    return res;
}

// bit depth, sample-rate decimation and random bit flips
Chans bits (const Chans& ch, Ctx& cx, const Params& p)
{
    const double q = std::pow (2.0, p["bits"] - 1); const long long D = std::max (1LL, jr (p["down"]));
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (x.size()); double hold = 0; cx.seed (p["seed"]);
        for (size_t i = 0; i < x.size(); ++i)
        {
            if ((long long) i % D == 0)
            {
                int32_t v = (int32_t) jr (clampd (x[i], -1, 1) * (q - 1));
                if (cx.rand() < p["flip"] * 0.05) v ^= (int32_t) (1u << (unsigned) std::floor (cx.rand() * std::max (1.0, p["bits"] - 1)));
                hold = clampd (v / q, -1, 1);
            }
            out[i] = (float) (hold * p["mix"] + x[i] * (1 - p["mix"]));
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// reverse -> reverb -> reverse: the reverb swells INTO each sound
Chans revrev (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t n = (size_t) std::max (1LL, jr (p["size"] * sr));
    Chans res; cx.seed (p["seed"]);
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c];
        Buf ir (n); double lp = 0;
        for (size_t i = 0; i < n; ++i) { double u = (double) i / (double) n, k = 1 - p["damp"] * 0.95 * u; lp += ((cx.rand() * 2 - 1) - lp) * k; ir[i] = (float) (lp * std::pow (0.001, u)); }
        Buf xr (x.rbegin(), x.rend()); Buf wet = convolve (xr, ir, &cx, (double) c / (double) ch.size(), (c + 1.0) / (double) ch.size()); std::reverse (wet.begin(), wet.end());
        Buf dry (wet.size()); std::copy (x.begin(), x.end(), dry.begin() + (long) (n - 1));
        double g = peak (x) / std::max (1e-9, peak (wet));
        for (size_t i = 0; i < wet.size(); ++i) wet[i] = (float) (wet[i] * g * p["mix"] + dry[i] * (1 - p["mix"] * 0.6));
        res.push_back (std::move (wet)); cx.prog ((double) (c + 1) / (double) ch.size());
    }
    return res;
}

// cut into equal slices and play them in a random order
Chans shuffle (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size();
    const long long n = std::max (2LL, jr (p["slices"])); const size_t sl = (size_t) std::floor ((double) len / (double) n);
    const double fd = jsround (0.003 * sr); const long long kp = p.i ("keep") ? 1 : 0;
    cx.seed (p["seed"]);
    std::vector<long long> ord ((size_t) n); for (long long i = 0; i < n; ++i) ord[(size_t) i] = i;
    for (long long i = n - 1; i > kp; --i) { long long j = kp + (long long) std::floor (cx.rand() * (double) (i + 1 - kp)); std::swap (ord[(size_t) i], ord[(size_t) j]); }
    std::vector<bool> rev; for (size_t i = 0; i < ord.size(); ++i) rev.push_back (cx.rand() < p["rev"]);
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (sl * (size_t) n);
        for (size_t k = 0; k < ord.size(); ++k)
            for (size_t i = 0; i < sl; ++i)
            {
                double e = std::min ({ 1.0, (double) i / fd, (double) (sl - 1 - i) / fd });
                out[k * sl + i] = (float) (x[(size_t) ord[k] * sl + (rev[k] ? sl - 1 - i : i)] * e);
            }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// convolve with another sample: impulse responses, textures, "sound A played through sound B"
Chans conv (const Chans& ch, Ctx& cx, const Params& p)
{
    if (! cx.aux || cx.aux->empty()) throw LabError ("Choose an impulse sample");
    Chans h0; for (auto& a : *cx.aux) h0.emplace_back (a.begin(), a.begin() + (long) std::min (a.size(), (size_t) (12 * cx.sr)));
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c];
        Buf wet = convolve (x, h0[c % h0.size()], &cx, (double) c / (double) ch.size(), (c + 1.0) / (double) ch.size()); double g = peak (x) / std::max (1e-9, peak (wet));
        for (size_t i = 0; i < wet.size(); ++i) wet[i] = (float) (wet[i] * g * p["mix"] + (i < x.size() ? x[i] : 0) * (1 - p["mix"]));
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (wet));
    }
    return res;
}

// keep only the loudest partials (crystalline) or only what is left beneath them (ghostly residue)
Chans gate (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr;
    const size_t N = pow2 (p["win"] * sr), H = N >> 2, len = ch[0].size(), half = N >> 1; const DVec w = hann (N);
    const long long frames = (long long) std::ceil ((double) (len + N) / (double) H); const double thr = std::pow (10.0, p["thr"] / 20);
    DVec re (N), im (N); Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        Buf out (len + N);
        for (long long f = 0; f < frames; ++f)
        {
            long long pos = f * (long long) H - (long long) half;
            analyze (ch[c], pos, w, re, im);
            double mx = 0; for (size_t k = 0; k <= half; ++k) mx = std::max (mx, std::hypot (re[k], im[k]));
            for (size_t k = 0; k <= half; ++k)
            {
                double m = std::hypot (re[k], im[k]), r = m / (mx * thr + 1e-12);
                double g = clampd ((r - 0.7) / 0.6, 0, 1); if (p.i ("mode")) g = 1 - g;
                re[k] *= g; im[k] *= g;
            }
            mirror (re, im); synth (out, pos, w, re, im, 2.0 / 3.0);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        res.push_back (std::move (out));
    }
    return res;
}

// bank of tuned resonating combs: any sound rings like strings tuned to a chord
Chans reso (const Chans& ch, Ctx& cx, const Params& p)
{
    static const std::vector<std::vector<int>> CHS { { 0 }, { 0, 12 }, { 0, 7, 12 }, { 0, 4, 7, 12 }, { 0, 3, 7, 12 }, { 0, 5, 7, 12 }, { 0, 4, 7, 10 }, { 0, 1, 2, 3 } };
    const auto& CH = CHS[(size_t) clamp (p.i ("chord"), 0, 7)];
    const double sr = cx.sr; const size_t len = ch[0].size();
    const size_t tail = (size_t) jr (std::min (10.0, p["decay"]) * sr), outLen = std::min (cx.maxLen, len + tail);
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (outLen);
        for (size_t v = 0; v < CH.size(); ++v)
        {
            double f = 440 * std::pow (2.0, (p["note"] + CH[v] - 57) / 12), D = sr / f, g = std::pow (0.001, D / (p["decay"] * sr));
            long long bs = 1LL << (long long) std::ceil (std::log2 (D + 4));
            Buf buf ((size_t) bs); double k = 0.1 + p["bright"] * 0.9; long long wpos = 0; double lp = 0;
            for (size_t i = 0; i < outLen; ++i)
            {
                double rp = (double) wpos - D; double fi0 = std::floor (rp); long long i0 = (long long) fi0; double fr = rp - fi0;
                double d = buf[(size_t) (i0 & (bs - 1))] * (1 - fr) + buf[(size_t) ((i0 + 1) & (bs - 1))] * fr;
                lp += (d - lp) * k;
                double y = (i < len ? x[i] : 0) * 0.3 + lp * g;
                buf[(size_t) (wpos & (bs - 1))] = (float) y; wpos++;
                out[i] = (float) (out[i] + lp / (double) CH.size());
            }
            cx.prog (((double) c + (double) (v + 1) / (double) CH.size()) / (double) ch.size());
        }
        double g2 = peak (x) / std::max (1e-9, peak (out));
        for (size_t i = 0; i < outLen; ++i) out[i] = (float) (out[i] * g2 * p["mix"] + (i < len ? x[i] : 0) * (1 - p["mix"]));
        res.push_back (std::move (out));
    }
    return res;
}

// ===== spectral =====
// MORPH: interpolate magnitudes + phases with a second sound; GLIDE: A turns into B over the length; VOCODE: impose B's envelope/formants on A
Chans morph (const Chans& ch, Ctx& cx, const Params& p)
{
    if (! cx.aux || cx.aux->empty()) throw LabError ("Choose a MODULATOR sample");
    const double sr = cx.sr; const size_t len = ch[0].size(); const auto& aux = *cx.aux; const int mode = p.i ("mode");
    return stftRun (ch, cx, p["win"], len + pow2 (p["win"] * sr), [&] (DVec& mag, DVec& ph, St& st, long long, long long pos)
    {
        const size_t N = st.N, half = st.half;
        if (! st.init) { st.init = true; st.re2.assign (N, 0); st.im2.assign (N, 0); st.a.assign (half + 1, 0); st.b.assign (half + 1, 0); st.d.assign (half + 1, 0); }
        const Buf& y = aux[(size_t) st.c % aux.size()]; double t = clampd ((double) (pos + (long long) half) / (double) len, 0, 1);
        analyze (y, jr (t * (double) y.size()) - (long long) half, *st.w, st.re2, st.im2);
        for (size_t k = 0; k <= half; ++k) st.a[k] = std::hypot (st.re2[k], st.im2[k]);
        if (mode == 2)
        {
            double B = p["detail"] / (sr / (double) N);
            envelope (mag, B, st.b); envelope (st.a, B, st.d);
            for (size_t k = 0; k <= half; ++k) mag[k] *= std::pow (std::min (1000.0, st.d[k] / st.b[k]), p["amount"]);
        }
        else
        {
            double a = mode == 1 ? t : p["amount"];
            for (size_t k = 0; k <= half; ++k)
            {
                double p2 = std::atan2 (st.im2[k], st.re2[k]);
                mag[k] = std::pow (mag[k] + 1e-12, 1 - a) * std::pow (st.a[k] + 1e-12, a);
                ph[k] = std::atan2 ((1 - a) * std::sin (ph[k]) + a * std::sin (p2), (1 - a) * std::cos (ph[k]) + a * std::cos (p2));
            }
        }
    });
}

// SPEC TUNE: a comb of pitch templates (scale / chord / harmonic series), optionally stepping through a progression
Chans tune (const Chans& ch, Ctx& cx, const Params& p)
{
    static const std::vector<std::vector<int>> SCS { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 4, 7, 9 },
                                                    { 0, 2, 4, 6, 8, 10 }, { 0, 4, 7, 11 }, { 0, 2, 3, 7, 10 }, {}, {} };
    static const std::vector<std::vector<int>> PRS { { 0 }, { 0, 5, 7, 0 }, { 0, 8, 3, 10 }, { 0, 2, 4, 5, 7, 9, 11, 12 }, { 12, 11, 9, 7, 5, 4, 2, 0 } };
    const int scale = clamp (p.i ("scale"), 0, 8); const auto& SC = SCS[(size_t) scale]; const auto& PR = PRS[(size_t) clamp (p.i ("prog"), 0, 4)];
    const double sr = cx.sr; const size_t len = ch[0].size();
    return stftRun (ch, cx, 0.19, len + pow2 (0.19 * sr), [&] (DVec& mag, DVec&, St& st, long long, long long pos)
    {
        const size_t N = st.N, half = st.half; const double bin = sr / (double) N;
        double t = clampd ((double) (pos + (long long) half) / (double) len, 0, 0.9999);
        double f0 = 440 * std::pow (2.0, (p["note"] + PR[(size_t) std::floor (t * (double) PR.size())] - 69) / 12);
        for (size_t k = 1; k <= half; ++k)
        {
            double fr = (double) k * bin, cents;
            if (scale == 8) { double n = std::max (1.0, jsround (fr / f0)); cents = 1200 * std::abs (std::log2 (fr / (n * f0))); }
            else
            {
                double rel = 12 * std::log2 (fr / f0), pc = std::fmod (std::fmod (rel, 12.0) + 12, 12.0), best = 99;
                if (SC.empty()) best = std::abs (pc * 2 - jsround (pc * 2)) / 2;
                else for (int d : SC) { double dd = std::abs (pc - d); best = std::min ({ best, dd, 12 - dd }); }
                cents = best * 100;
            }
            mag[k] *= 1 - p["amount"] + p["amount"] * std::exp (-std::pow (cents / p["width"], 2));
        }
        mag[0] *= 1 - p["amount"];
    });
}

// STRETCH: partials f -> c*(f/c)^s (inharmonic). INVERT: spectrum mirrored around c in log frequency (f -> c^2/f)
Chans sinv (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, fc = p["center"]; const size_t len = ch[0].size(); const int mode = p.i ("mode");
    return stftRun (ch, cx, p["win"], len + pow2 (p["win"] * sr), [&] (DVec& mag, DVec& ph, St& st, long long, long long)
    {
        const double bin = sr / (double) st.N;
        pvMap (mag, ph, st, [&] (double tf)
        {
            double fr = tf * bin; if (fr <= 1) return -1.0;
            if (mode != 1) fr = fc * std::pow (fr / fc, p["stretch"]);
            if (mode != 0) fr = fc * fc / fr;
            return fr / bin;
        });
    });
}

// FOCUS: keep only the strongest spectral peaks; RING lets them sustain (resonate) after the input moves on
Chans focus (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(), tail = (size_t) jr (std::min (8.0, p["ring"]) * sr);
    const int peaks = p.i ("peaks"), width = p.i ("width");
    return stftRun (ch, cx, p["win"], std::min (cx.maxLen, len + tail + pow2 (p["win"] * sr)), [&] (DVec& mag, DVec& ph, St& st, long long, long long)
    {
        const size_t half = st.half, N = st.N, H = st.H;
        if (! st.init) { st.init = true; st.a.assign (half + 1, 0); st.b.assign (half + 1, 0); st.keep.assign (half + 1, 0); }
        auto pk = peakBins (mag, half);
        std::fill (st.keep.begin(), st.keep.end(), 0);
        for (size_t i = 0; i < std::min ((size_t) std::max (0, peaks), pk.size()); ++i)
            for (int d = -width; d <= width; ++d) { long long j = (long long) pk[i] + d; if (j >= 0 && j <= (long long) half) st.keep[(size_t) j] = 1; }
        double dec = p["ring"] > 0.01 ? std::exp (-(double) H / sr / (p["ring"] / 6.9)) : 0, exp = TAU * (double) H / (double) N;
        for (size_t k = 0; k <= half; ++k)
        {
            double m = st.keep[k] ? mag[k] : mag[k] * (1 - p["depth"]);
            if (m >= st.a[k] * dec) { st.a[k] = m; st.b[k] = ph[k]; } else { st.a[k] *= dec; st.b[k] += (double) k * exp; }
            mag[k] = st.a[k]; ph[k] = st.b[k];
        }
    });
}

// MASK: filter against the spectrum profile of another sample · STRIP PEAKS: remove the strongest partials
// TRANSIENTS / SUSTAIN: keep only the rising (onset) or the steady part of every bin
Chans mask (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(), N = pow2 (p["win"] * sr), half = N >> 1; const int mode = p.i ("mode");
    std::vector<DVec> prof;
    if (mode == 0)
    {
        if (! cx.aux || cx.aux->empty()) throw LabError ("Choose a MASK sample");
        for (auto& y : *cx.aux)
        {
            DVec w = hann (N), re (N), im (N), pr (half + 1);
            for (size_t pos = 0; pos < y.size(); pos += N >> 1) { analyze (y, (long long) pos, w, re, im); for (size_t k = 0; k <= half; ++k) pr[k] += std::hypot (re[k], im[k]); }
            double mx = 0; for (double v : pr) mx = std::max (mx, std::abs (v)); if (mx == 0) mx = 1;
            for (auto& v : pr) v /= mx;
            prof.push_back (pr);
        }
    }
    return stftRun (ch, cx, p["win"], len + N, [&] (DVec& mag, DVec&, St& st, long long, long long)
    {
        if (mode == 0) { const auto& pr = prof[(size_t) st.c % prof.size()]; for (size_t k = 0; k <= half; ++k) mag[k] *= std::max (0.0, 1 - p["amount"] * std::sqrt (pr[k])); }
        else if (mode == 1)
        {
            auto pk = peakBins (mag, half); long long n = jr (1 + p["amount"] * 31);
            for (size_t i = 0; i < std::min ((size_t) n, pk.size()); ++i) for (int d = -1; d <= 1; ++d) { long long j = (long long) pk[i] + d; if (j >= 0 && j <= (long long) half) mag[(size_t) j] *= 0.02; }
        }
        else
        {
            if (! st.init) { st.init = true; st.a.assign (half + 1, 0); }
            for (size_t k = 0; k <= half; ++k)
            {
                double m = mag[k], g = clampd (1 - st.a[k] * 1.3 / (m + 1e-9), 0, 1); st.a[k] = m;
                mag[k] = m * (1 - p["amount"] + p["amount"] * (mode == 2 ? g : 1 - g));
            }
        }
    });
}

// ===== waveset (zero-crossing cycles) =====
Chans wdist (const Chans& ch, Ctx& cx, const Params& p)
{
    auto segs = wsets (ch, p.i ("group")); const long long R = jr (p["amt"]); const int mode = p.i ("mode");
    cx.seed (p["seed"]);
    std::vector<bool> plan; for (size_t i = 0; i < segs.size(); ++i) plan.push_back (cx.rand() < p["chance"]);
    std::vector<size_t> order (segs.size()); for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (mode == 5)
        for (size_t i = 0; i < order.size(); ++i)
            if (plan[i]) { long long j = clamp ((long long) i + jr ((cx.rand() * 2 - 1) * 4), 0LL, (long long) order.size() - 1); std::swap (order[i], order[(size_t) j]); }
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf o; o.reserve (x.size());
        for (size_t i = 0; i < order.size(); ++i)
        {
            auto [a, b] = segs[order[i]];
            Buf s (x.begin() + (long) a, x.begin() + (long) b);
            if (! plan[i]) { o.insert (o.end(), s.begin(), s.end()); continue; }
            switch (mode)
            {
                case 0: for (long long r = 0; r < R; ++r) o.insert (o.end(), s.begin(), s.end()); break;
                case 1: o.insert (o.end(), s.size(), 0.0f); break;
                case 2: for (float v : s) o.push_back (-v); break;
                case 3: o.insert (o.end(), s.rbegin(), s.rend()); break;
                case 4: { Buf q = resamp (s, jr ((double) s.size() / (double) R)); for (long long r = 0; r < R; ++r) o.insert (o.end(), q.begin(), q.end()); break; }
                default: o.insert (o.end(), s.begin(), s.end());
            }
            if (o.size() > cx.maxLen * 2) break;
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        if (o.size() > cx.maxLen) o.resize (cx.maxLen);
        res.push_back (std::move (o));
    }
    return res;
}

Chans wavg (const Chans& ch, Ctx& cx, const Params& p)
{
    auto segs = wsets (ch, p.i ("group")); const long long K = jr (p["span"]); const int mode = p.i ("mode");
    double gpk = peakAll (ch);
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c]; Buf out (x.size());
        for (size_t i = 0; i < segs.size(); ++i)
        {
            auto [a, b] = segs[i]; size_t L = b - a; Buf s (x.begin() + (long) a, x.begin() + (long) b);
            if (mode == 0)
            {
                double pk = peak (s), g = pk > gpk * 0.002 ? 1 + p["amount"] * (std::min (10.0, gpk * 0.8 / pk) - 1) : 1;
                for (size_t q = 0; q < L; ++q) out[a + q] = (float) (s[q] * g);
                continue;
            }
            Buf avg (L); int n = 0;
            for (long long j = std::max (0LL, (long long) i - K); j <= std::min ((long long) segs.size() - 1, (long long) i + K); ++j)
            {
                Buf sj (x.begin() + (long) segs[(size_t) j].first, x.begin() + (long) segs[(size_t) j].second);
                Buf r = resamp (sj, (long long) L); for (size_t q = 0; q < L; ++q) avg[q] += r[q];
                n++;
            }
            for (size_t q = 0; q < L; ++q)
            {
                double v = (double) avg[q] / n;
                out[a + q] = (float) (mode == 1 ? s[q] + (v - s[q]) * p["amount"] : s[q] + (s[q] - v) * p["amount"] * 2);
            }
        }
        cx.prog ((double) (c + 1) / (double) ch.size());
        res.push_back (std::move (out));
    }
    return res;
}

// ===== granular / time =====
// BRASSAGE: overlapping grains re-read at a new speed and pitch, with scatter and shuffle
Chans brass (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size();
    const size_t outLen = std::min (cx.maxLen, (size_t) jr ((double) len * p["stretch"]));
    const long long G = std::max (16LL, jr (p["size"] / 1000 * sr)), hop = std::max (4LL, jr ((double) G / p["density"]));
    const double ratio = std::pow (2.0, p["pitch"] / 12), gn = 2 / p["density"]; const DVec win = hann ((size_t) G);
    const long long n = (long long) std::ceil ((double) outLen / (double) hop) + 1;
    Chans out (ch.size(), Buf (outLen)); cx.seed (p["seed"]);
    for (long long g = 0; g < n; ++g)
    {
        long long t0 = g * hop; double src = (double) t0 / p["stretch"] + (cx.rand() - 0.5) * p["scatter"] * (double) G * 4;
        if (cx.rand() < p["shuffle"]) src += (cx.rand() - 0.5) * sr * 2;
        src = clampd (src, 0, (double) len - 1);
        for (size_t c = 0; c < ch.size(); ++c)
        {
            auto& o = out[c];
            for (long long i = 0; i < G; ++i) { long long j = t0 - (G >> 1) + i; if (j >= 0 && j < (long long) outLen) o[(size_t) j] = (float) (o[(size_t) j] + lerpAt (ch[c], src + ((double) i - (double) G / 2) * ratio) * win[(size_t) i] * gn); }
        }
        if (! (g & 127)) cx.prog ((double) g / (double) n);
    }
    return out;
}

// REPEATS: one grain bounces like a ball: intervals shrink (accelerando) or grow, each hit quieter
Chans bounce (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const long long len = (long long) ch[0].size();
    const long long a = jr (p["pos"] * (double) (len - 1));
    const long long G = std::max (32LL, std::min (len - a, jr (p["size"] / 1000 * sr))), fd = std::max (2LL, std::min (G >> 2, jr (0.003 * sr)));
    std::vector<long long> times; double t = 0, iv = p["interval"] / 1000 * sr;
    for (int i = 0; i < p.i ("count"); ++i) { times.push_back (jr (t)); t += iv; iv *= p["accel"]; if (iv < 32 || t > (double) cx.maxLen) break; }
    const long long gesture = jr ((double) times.back() + (double) G);
    const size_t outLen = std::min ((long long) cx.maxLen, a + gesture + (p.i ("keep") ? std::max (0LL, len - a - G) : 0));
    Chans res;
    for (auto& x : ch)
    {
        Buf out (outLen); std::copy (x.begin(), x.begin() + (long) std::min ((size_t) a, outLen), out.begin());
        for (size_t n = 0; n < times.size(); ++n)
        {
            double g = std::pow (1 - p["decay"], (double) n);
            for (long long i = 0; i < G; ++i)
            {
                long long o = a + times[n] + i;
                if (o < (long long) outLen)
                {
                    double xv = a + i < len ? x[(size_t) (a + i)] : NAN;
                    out[(size_t) o] = (float) (out[(size_t) o] + xv * g * std::min ({ 1.0, (double) (i + fd * (n ? 0 : 1)) / (double) fd, (double) (G - 1 - i) / (double) fd }));
                }
            }
        }
        if (p.i ("keep")) for (long long i = 0, s0 = a + gesture; s0 + i < (long long) outLen && a + G + i < len; ++i) out[(size_t) (s0 + i)] += x[(size_t) (a + G + i)];
        res.push_back (std::move (out));
    }
    return res;
}

// DRUNK: segments read by a random walk (with a slight forward drift) · SCRAMBLE: random segments from anywhere
Chans drunk (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const long long len = (long long) ch[0].size();
    const long long S = std::max (64LL, jr (p["seg"] / 1000 * sr)), nseg = std::max (1LL, len / S);
    const long long outLen = (long long) std::min (cx.maxLen, (size_t) jr ((double) len * p["lenx"])), fd = std::max (2LL, std::min (S >> 2, jr (0.004 * sr)));
    std::vector<std::pair<long long, long long>> plan; long long pos = 0, o = 0; cx.seed (p["seed"]);
    while (o < outLen)
    {
        if (p.i ("mode") == 0) { plan.push_back ({ pos * S, S }); pos = clamp (pos + jr ((cx.rand() * 2 - 1) * p["step"] + 0.3), 0LL, nseg - 1); o += S; }
        else { long long L = jr ((double) S * (0.5 + cx.rand())); plan.push_back ({ (long long) std::floor (cx.rand() * (double) std::max (1LL, len - L)), L }); o += L; }
    }
    Chans res;
    for (auto& x : ch)
    {
        Buf out ((size_t) outLen); long long w = 0;
        for (auto [a, L] : plan)
        {
            for (long long i = 0; i < L && w + i < outLen; ++i)
                out[(size_t) (w + i)] = (float) ((a + i < len ? x[(size_t) (a + i)] : 0) * std::min ({ 1.0, (double) i / (double) fd, (double) (L - 1 - i) / (double) fd }));
            w += L;
        }
        res.push_back (std::move (out));
    }
    return res;
}

// ITERATE: one tiny segment multiplied hundreds of times with pitch / level / pan scatter
Chans iter (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const long long len = (long long) ch[0].size();
    const long long a = jr (p["pos"] * (double) (len - 1)), G = std::max (16LL, std::min (len - a, jr (p["size"] / 1000 * sr)));
    const long long outLen = (long long) std::min (cx.maxLen, (size_t) jr (p["len"] * sr));
    const double fd = (double) std::max (2LL, std::min (G >> 2, jr (0.002 * sr)));
    Buf L ((size_t) outLen), R ((size_t) outLen); const Buf &xl = ch[0], &xr = chOr (ch, 1);
    cx.seed (p["seed"]);
    for (long long t = 0; t < outLen; t += std::max (1LL, jr (sr / p["density"] * (0.5 + cx.rand()))))
    {
        double ratio = std::pow (2.0, (cx.rand() * 2 - 1) * p["pitch"] / 12), g = 1 - p["gain"] * cx.rand();
        auto [gl, gr] = panG (0.5 + (cx.rand() - 0.5) * 0.7);
        long long n = (long long) std::floor ((double) G / ratio);
        for (long long i = 0; i < n && t + i < outLen; ++i)
        {
            double e = std::min ({ 1.0, (double) i / fd, (double) (n - 1 - i) / fd }) * g, sp = (double) a + (double) i * ratio;
            L[(size_t) (t + i)] = (float) (L[(size_t) (t + i)] + lerpAt (xl, sp) * e * gl); R[(size_t) (t + i)] = (float) (R[(size_t) (t + i)] + lerpAt (xr, sp) * e * gr);
        }
        cx.prog ((double) t / (double) outLen);
    }
    fades (L, 0, jsround (0.05 * sr)); fades (R, 0, jsround (0.05 * sr));
    return { L, R };
}

// ZIGZAG: the read head runs forward and backward between random turning points · SCRUB: tape shuttle on a sine speed curve
Chans zig (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const double len = (double) ch[0].size();
    const size_t outLen = std::min (cx.maxLen, (size_t) jr (len * p["lenx"])); const double span = std::max (64.0, p["span"] * sr);
    DVec path (outLen); cx.seed (p["seed"]);
    if (p.i ("mode") == 1) for (size_t i = 0; i < outLen; ++i) path[i] = clampd (((double) i / (double) outLen) * len + span * 0.5 * std::sin (TAU * p["rate"] * (double) i / sr), 0, len - 1);
    else
    {
        double r = len / (double) outLen, bf = r < 1 ? (1 - r) / (1 + r) : 0, spd = std::max (1.0, r);
        double pos = 0, dir = 1, fwd = span * (0.5 + cx.rand()), left = fwd;
        for (size_t i = 0; i < outLen; ++i)
        {
            path[i] = pos; pos = clampd (pos + dir * spd, 0, len - 1); left -= spd;
            if (left <= 0) { if (dir > 0 && bf > 0) { dir = -1; left = fwd * bf * (0.5 + cx.rand()); } else { dir = 1; fwd = span * (0.5 + cx.rand()); left = fwd; } }
        }
    }
    Chans res;
    for (auto& x : ch) { Buf out (outLen); for (size_t i = 0; i < outLen; ++i) out[i] = (float) lerpAt (x, path[i]); fades (out, jsround (0.003 * sr), jsround (0.01 * sr)); res.push_back (std::move (out)); }
    return res;
}

// ===== texture =====
// TEXTURE: the whole (short) source scattered over time, pitch and stereo: random cloud, rhythmic grid or chord tones
Chans tex (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const int mode = p.i ("mode");
    const double len = std::min ((double) ch[0].size(), 4 * sr);
    const long long outLen = (long long) std::min (cx.maxLen, (size_t) jr (p["len"] * sr)), n = std::max (1LL, jr (p["density"] * (double) outLen / sr));
    Buf L ((size_t) outLen), R ((size_t) outLen); const Buf &xl = ch[0], &xr = chOr (ch, 1); const double fd = jsround (0.004 * sr);
    std::vector<int> chord; for (int s : { 0, 4, 7, 11, 12, 14, 16, 19, -5, -8, -12 }) if (std::abs (s) <= p["pitch"]) chord.push_back (s);
    if (chord.empty()) chord.push_back (0);
    cx.seed (p["seed"]);
    for (long long e = 0; e < n; ++e)
    {
        long long t = mode == 1 ? jr ((double) e * sr / p["density"]) : (long long) std::floor (cx.rand() * (double) outLen);
        if (mode == 1 && (e % 4) && cx.rand() < 0.35) continue;
        double st = mode == 2 ? chord[(size_t) std::floor (cx.rand() * (double) chord.size())] : (cx.rand() * 2 - 1) * p["pitch"], ratio = std::pow (2.0, st / 12);
        double g = mode == 1 ? ((e % 4) ? 0.6 : 1) : 0.4 + cx.rand() * 0.6;
        auto [gl, gr] = panG (0.5 + (cx.rand() - 0.5) * p["width"]);
        long long m = (long long) std::floor (len / ratio);
        for (long long i = 0; i < m && t + i < outLen; ++i)
        {
            double k = std::min (1.0, (double) (m - 1 - i) / fd) * g, sp = (double) i * ratio;
            L[(size_t) (t + i)] = (float) (L[(size_t) (t + i)] + lerpAt (xl, sp) * k * gl); R[(size_t) (t + i)] = (float) (R[(size_t) (t + i)] + lerpAt (xr, sp) * k * gr);
        }
        if (! (e & 15)) cx.prog ((double) e / (double) n);
    }
    return { L, R };
}

// SHRINK: a segment repeats, each copy shorter (accelerating cascade) · POLY: voices loop it at 3:4:5:6:7 across the stereo field
Chans seg (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const long long len = (long long) ch[0].size();
    const long long a = jr (p["pos"] * (double) (len - 1)), S = std::max (64LL, std::min (len - a, jr (p["size"] / 1000 * sr)));
    const double fd = (double) std::max (2LL, std::min (S >> 3, jr (0.003 * sr)));
    struct Ev { long long t, n; double pan; }; std::vector<Ev> ev;
    if (p.i ("mode") == 0) { long long t = 0, L = S; for (int i = 0; i < p.i ("reps") && L >= 32; ++i) { ev.push_back ({ t, L, 0.5 }); t += L; L = jr ((double) L * p["shrink"]); } }
    else
    {
        long long T = S * p.i ("reps"); int voices = p.i ("voices");
        for (int v = 0; v < voices; ++v)
        {
            long long per = jr ((double) S * 3 / (v + 3)); double pan = 0.15 + 0.7 * v / (double) (voices - 1);
            for (long long t = 0; t < T; t += per) ev.push_back ({ t, per, pan });
        }
    }
    long long mx = 0; for (auto& e : ev) mx = std::max (mx, e.t + e.n);
    const long long outLen = std::min ((long long) cx.maxLen, mx);
    Buf L ((size_t) outLen), R ((size_t) outLen); const Buf &xl = ch[0], &xr = chOr (ch, 1);
    for (auto& e : ev)
    {
        auto [gl, gr] = panG (e.pan);
        for (long long i = 0; i < e.n && e.t + i < outLen; ++i)
        {
            double en = std::min ({ 1.0, (double) i / fd, (double) (e.n - 1 - i) / fd }); long long j = a + i; if (j >= len) break;
            L[(size_t) (e.t + i)] = (float) (L[(size_t) (e.t + i)] + xl[(size_t) j] * en * gl); R[(size_t) (e.t + i)] = (float) (R[(size_t) (e.t + i)] + xr[(size_t) j] * en * gr);
        }
    }
    return { L, R };
}

// ===== formant / pitch / space =====
// PITCH: phase-vocoder transposition, then the original spectral envelope (formants) is put back
Chans fpitch (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr, ratio = std::pow (2.0, p["semis"] / 12); const size_t len = ch[0].size();
    return stftRun (ch, cx, p["win"], len + pow2 (p["win"] * sr), [&] (DVec& mag, DVec& ph, St& st, long long, long long)
    {
        const size_t half = st.half; const double B = 250 / (sr / (double) st.N);
        if (! st.init) { st.init = true; st.a.assign (half + 1, 0); st.b.assign (half + 1, 0); }
        envelope (mag, B, st.a);
        pvMap (mag, ph, st, [&] (double tf) { return tf * ratio; });
        envelope (mag, B, st.b);
        for (size_t k = 0; k <= half; ++k) mag[k] *= std::pow (std::min (30.0, st.a[k] / st.b[k]), p["preserve"]);
    });
}

// FORMANT WARP: pitch stays, the envelope is shifted, squeezed around a centre or mirrored
Chans fwarp (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(); const int mode = p.i ("mode");
    return stftRun (ch, cx, 0.05, len + pow2 (0.05 * sr), [&] (DVec& mag, DVec&, St& st, long long, long long)
    {
        const size_t half = st.half; const double bin = sr / (double) st.N, cb = p["center"] / bin;
        if (! st.init) { st.init = true; st.a.assign (half + 1, 0); }
        const DVec& env = envelope (mag, p["detail"] / bin, st.a);
        auto at = [&] (double x)
        {
            if (x <= 0) return env[0];
            if (x >= (double) half) return env[half];
            size_t i = (size_t) std::floor (x); double f = x - (double) i; return env[i] + (env[i + 1] - env[i]) * f;
        };
        for (size_t k = 1; k <= half; ++k)
            mag[k] = mag[k] / env[k] * at (mode == 0 ? (double) k / p["amount"] : mode == 1 ? cb + ((double) k - cb) * p["amount"] : cb * cb / (double) k);
    });
}

// SPATIAL: the sound travels around the listener with level, Doppler delay, interaural time difference and head shadow
Chans space (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const size_t len = ch[0].size(), outLen = std::min (cx.maxLen, len + (size_t) jr (0.12 * sr));
    const double dur = (double) outLen / sr; const int mode = p.i ("mode");
    Buf m (len); for (auto& x : ch) for (size_t i = 0; i < len; ++i) m[i] = (float) (m[i] + x[i] / (double) ch.size());
    Buf L (outLen), R (outLen);
    double hx = 0, hy = 1, vx = 0, vy = 0, lpL = 0, lpR = 0, x = 0, y = 1;
    const double lim = 1 + p["depth"] * 2; cx.seed (p["seed"]);
    for (size_t i = 0; i < outLen; ++i)
    {
        double t = (double) i / sr, a = TAU * p["rate"] * t;
        switch (mode)
        {
            case 0: x = lim * std::sin (a); y = lim * std::cos (a); break;
            case 1: { double r = 0.4 + p["depth"] * 4 * t / dur; x = r * std::sin (a); y = r * std::cos (a); break; }
            case 2: x = lim * std::sin (a * 3); y = lim * std::sin (a * 2 + 0.5); break;
            case 3:
                if (! (i & 255)) { vx = (vx + (cx.rand() - 0.5) * 0.02 * p["rate"]) * 0.97; vy = (vy + (cx.rand() - 0.5) * 0.02 * p["rate"]) * 0.97; hx = clampd (hx + vx, -lim, lim); hy = clampd (hy + vy, -lim, lim); }
                x = hx; y = hy; break;
            default: x = -5 + 10 * t / dur; y = 0.3 + p["depth"] * 2;
        }
        double d = std::max (0.3, std::hypot (x, y)), pan = clampd (x / d, -1, 1), gl = std::sqrt ((1 - pan) / 2), gr = std::sqrt ((1 + pan) / 2), att = 1 / (0.5 + d);
        double dop = p["doppler"] * d * 0.015 * sr;
        double sL = lerpAt (m, (double) i - dop - std::max (0.0, pan) * 0.0006 * sr), sR = lerpAt (m, (double) i - dop - std::max (0.0, -pan) * 0.0006 * sr);
        double k = std::exp (-d * 0.15) * (y < 0 ? 0.6 : 1);
        lpL += (sL - lpL) * k; lpR += (sR - lpR) * k;
        L[i] = (float) (lpL * gl * att); R[i] = (float) (lpR * gr * att);
        if (! (i & 65535)) cx.prog ((double) i / (double) outLen);
    }
    return { L, R };
}
} // namespace lab::procs_orig
