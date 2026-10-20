// CDP collection, Batch 4 - Waveset / sample-level distortion + pitch-synchronous grains.
// Waveset processes work on each channel separately, like CDP's mono programs run once per channel.
// A "wavecycle" follows CDP's definition (dev/distort/distortion.c): from the first non-zero sample, a run of
// samples of the initial sign followed by a run of the opposite sign.
#include "LabCdp.h"
#include "LabDspUtil.h"

namespace lab
{
namespace
{
using namespace lab::dsp;

// ---------------------------------------------------------------- shared helpers

// boundaries of whole wavecycles: b[0] = 0 ... b.back() = n (cycle i = [b[i], b[i+1]))
std::vector<size_t> cycB (const Buf& x)
{
    const size_t n = x.size(); std::vector<size_t> b { 0 };
    size_t i = 0; while (i < n && x[i] == 0) ++i;
    if (i >= n) { if (n > 0) b.push_back (n); return b; }
    const bool pos = x[i] > 0;
    while (i < n)
    {
        if (pos) { while (i < n && x[i] >= 0) ++i; while (i < n && x[i] <= 0) ++i; }
        else { while (i < n && x[i] <= 0) ++i; while (i < n && x[i] >= 0) ++i; }
        b.push_back (i);
    }
    return b;
}
// half-cycles: runs of one strict sign (zeros between them belong to no half-cycle)
struct Half { size_t a, b; int sg; float pk; };
std::vector<Half> halves (const Buf& x)
{
    std::vector<Half> h; const size_t n = x.size(); size_t i = 0;
    while (i < n)
    {
        while (i < n && x[i] == 0) ++i;
        if (i >= n) break;
        Half q { i, i, x[i] > 0 ? 1 : -1, 0 };
        while (i < n && x[i] * (float) q.sg > 0) { q.pk = std::max (q.pk, std::abs (x[i])); ++i; }
        q.b = i; h.push_back (q);
    }
    return h;
}
inline double cycPeak (const Buf& x, size_t a, size_t b) { double p = 0; for (size_t i = a; i < b; ++i) p = std::max (p, (double) std::abs (x[i])); return p; }
// append x[a, b) to y; false once y is full
inline bool app (Buf& y, const Buf& x, size_t a, size_t b, size_t cap)
{
    for (size_t i = a; i < b; ++i) { if (y.size() >= cap) return false; y.push_back (x[i]); }
    return y.size() < cap;
}
inline bool app (Buf& y, const Buf& seg, size_t cap) { return app (y, seg, 0, seg.size(), cap); }
// runs f on every channel (the same seed per channel), caps the length
template <typename F> Chans eachCh (const Chans& ch, Ctx& cx, F f)
{
    Chans o;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        cx.prog ((double) c / (double) ch.size());
        Buf y = f (ch[c], (int) c);
        if (y.size() > cx.maxLen) y.resize (cx.maxLen);
        if (y.empty()) y.assign (1, 0.f);
        o.push_back (std::move (y));
    }
    return o;
}
inline double semis (double st) { return std::pow (2.0, st / 12.0); }
inline long long lenOf (double sec, double sr) { return std::max (1LL, jr (sec * sr)); }

// ================================================================ DISTORT (dev/distort)

// Each half-cycle is replaced by a square, triangle, inverted half, click burst or sine of the same length and peak.
// after CDP distort reform 1-7 (do_distort/distort_func, distort.c)
Chans reform (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double FIX = 0.7325; const long long pulse = std::max (1LL, jr (0.001 * cx.sr));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); Buf y = x;
        for (auto& h : halves (x))
        {
            const long long L = (long long) (h.b - h.a); const double pk = h.pk * h.sg, fx = FIX * h.sg;
            switch (mode)
            {
                case 0: for (size_t j = h.a; j < h.b; ++j) y[j] = (float) fx; break;
                case 1: for (size_t j = h.a; j < h.b; ++j) y[j] = (float) pk; break;
                case 2: case 3:
                {
                    if (L <= 3) break;
                    const double top = mode == 2 ? fx : pk; const long long hc = (L + 1) / 2;
                    for (long long j = 0; j < L; ++j) { double u = j < hc ? (double) j / (double) hc : (double) (L - j) / (double) (L - hc); y[h.a + (size_t) j] = (float) (top * clampd (u, 0, 1)); }
                    break;
                }
                case 4: for (size_t j = h.a; j < h.b; ++j) y[j] = (float) (pk - x[j]); break;
                case 5:
                {   // burst of random-width alternating pulses, then silence (genpulse)
                    long long step = std::min (pulse, L), sum = 0; double v = pk; size_t j = h.a;
                    while (sum < step) { long long w = (long long) (cx.rand() * 6.0) + 1; for (long long m = 0; m < w && sum < step; ++m, ++sum) y[j++] = (float) v; v = -v; }
                    for (; j < h.b; ++j) y[j] = 0;
                    break;
                }
                default: if (L > 5) for (long long j = 0; j < L; ++j) y[h.a + (size_t) j] = (float) (std::sin (PI * (double) j / (double) L) * pk); break;
            }
        }
        return y;
    });
}

// Exaggerates the contour inside every half-cycle: samples below the local peak envelope are pushed down by a power.
// after CDP distort reform 8 (exaggerate, distort.c)
Chans exagg (const Chans& ch, Ctx& cx, const Params& p)
{
    const double pf = p["exag"];
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y = x; std::vector<size_t> pks;
        for (auto& h : halves (x))
        {
            pks.clear();
            for (size_t j = h.a; j < h.b; ++j)
            {
                double v = std::abs (x[j]), l = j > h.a ? std::abs (x[j - 1]) : 0, r = j + 1 < h.b ? std::abs (x[j + 1]) : 0;
                if (v >= l && v >= r) pks.push_back (j);
            }
            if (pks.empty()) continue;
            size_t k = 0;
            for (size_t j = h.a; j < h.b; ++j)
            {
                while (k + 1 < pks.size() && pks[k + 1] <= j) ++k;
                double env;
                if (j <= pks[0]) env = std::abs (x[pks[0]]);
                else if (k + 1 >= pks.size()) env = std::abs (x[pks.back()]);
                else { double a = std::abs (x[pks[k]]), b = std::abs (x[pks[k + 1]]); env = a + (b - a) * (double) (j - pks[k]) / (double) (pks[k + 1] - pks[k]); }
                if (env < 1e-12) continue;
                double r = std::min (std::abs (x[j]) / env, 1.0);
                y[j] = (float) (x[j] * std::pow (r, pf));
            }
        }
        return y;
    });
}

// Imposes a rising, falling, troughed or drawn envelope over every group of CYCLES wavecycles.
// after CDP distort envel 1-4 (distort_env, distorte.c)
Chans envel (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"), shape = p.i ("shape"), G = std::max (1, p.i ("cycles")); const double tr = p["trough"], ex = p["expon"];
    auto user = [&] (double u) {
        switch (shape)
        {
            case 0: return std::sin (PI * u);                                       // ARCH
            case 1: return u < 0.05 ? u / 0.05 : std::exp (-(u - 0.05) * 6);      // SPIKE
            case 2: return std::abs (std::sin (TAU * u));                         // DOUBLE
            default: return std::floor (u * 4 + 1) / 4;                            // STEPS
        }
    };
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y = x; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); i += (size_t) G)
        {
            size_t a = b[i], e = b[std::min (b.size() - 1, i + (size_t) G)]; const double L = (double) (e - a);
            for (size_t j = a; j < e; ++j)
            {
                double u = (double) (j - a) / L, z;
                switch (mode)
                {
                    case 0: z = std::pow (u, ex) * (1 - tr) + tr; break;
                    case 1: z = std::pow (1 - u, ex) * (1 - tr) + tr; break;
                    case 2: z = std::pow (u < 0.5 ? 1 - 2 * u : 2 * u - 1, ex) * (1 - tr) + tr; break;
                    default: z = user (u); break;
                }
                y[j] = (float) (x[j] * z);
            }
        }
        return y;
    });
}

// Every group of CYCLES wavecycles is replaced by their average waveshape, repeated CYCLES times.
// after CDP distort average (distort_avg, distorta.c)
Chans average (const Chans& ch, Ctx& cx, const Params& p)
{
    const int G = std::max (2, p.i ("cycles")); const size_t maxW = (size_t) lenOf (p["maxwav"], cx.sr);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; y.reserve (x.size()); auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); i += (size_t) G)
        {
            size_t k = std::min (b.size() - 1 - i, (size_t) G); double tot = 0; bool tooLong = false;
            for (size_t n = 0; n < k; ++n) { size_t l = b[i + n + 1] - b[i + n]; tot += (double) l; if (l > maxW) tooLong = true; }
            if (tooLong || k < 2) { if (! app (y, x, b[i], b[i + k], cx.maxLen)) break; continue; }
            size_t avg = (size_t) std::max (1.0, jsround (tot / (double) k)); Buf cyc (avg);
            for (size_t m = 0; m < avg; ++m)
            {
                double s = 0, z = (double) m / (double) avg;
                for (size_t n = 0; n < k; ++n) { size_t l = b[i + n + 1] - b[i + n]; s += x[b[i + n] + std::min (l - 1, (size_t) jr (z * (double) l))]; }
                cyc[m] = (float) (s / (double) k);
            }
            bool ok = true; for (size_t n = 0; n < k && ok; ++n) ok = app (y, cyc, cx.maxLen);
            if (! ok) break;
        }
        return y;
    });
}

// A out of every B wavecycles become silence.
// after CDP distort omit (distort_omt, distorto.c)
Chans omit (const Chans& ch, Ctx& cx, const Params& p)
{
    const int B = std::max (2, p.i ("b")), A = clamp (p.i ("a"), 1, B - 1);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y = x; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); ++i) if ((int) (i % (size_t) B) >= B - A) for (size_t j = b[i]; j < b[i + 1]; ++j) y[j] = 0;
        return y;
    });
}

// Every half-cycle is squeezed N times into its own length (alternating signs): the waveset frequency is multiplied by N.
// after CDP distort multiply (do_multiply, distortm.c)
Chans multiply (const Chans& ch, Ctx& cx, const Params& p)
{
    const int N = std::max (2, p.i ("n")); const bool smooth = p.i ("smooth") != 0;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y = x; double sgn = 1;
        for (auto& h : halves (x))
        {
            const double L = (double) (h.b - h.a);
            if (L < N) { for (size_t j = h.a; j < h.b; ++j) y[j] = (float) (std::abs (x[j]) * sgn); if (N & 1) sgn = -sgn; continue; }
            for (size_t j = h.a; j < h.b; ++j)
            {
                double pos = (double) (j - h.a) * N / L; long long k = (long long) pos; double f = pos - (double) k;
                double v = smooth ? std::abs (lerpAt (x, (double) h.a + f * L)) : std::abs (x[h.a + std::min ((size_t) (L - 1), (size_t) jr (f * L))]);
                y[j] = (float) (v * ((k & 1) ? -sgn : sgn));
            }
            if (N & 1) sgn = -sgn;   // N odd: the next half-cycle continues with the other sign (N even: it already ended there)
        }
        return y;
    });
}

// Every N half-cycles are replaced by the first of them stretched over their whole length: the frequency drops N times.
// after CDP distort divide (do_the_division/do_divide/do_idivide, distortm.c)
Chans divide (const Chans& ch, Ctx& cx, const Params& p)
{
    const int N = std::max (2, p.i ("n")); const bool interp = p.i ("interp") != 0;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y = x; auto hs = halves (x); double sgn = 1;
        for (size_t g = 0; g + (size_t) N <= hs.size(); g += (size_t) N)
        {
            const Half& h0 = hs[g]; size_t a = h0.a, e = g + (size_t) N < hs.size() ? hs[g + (size_t) N].a : hs.back().b;
            const double L0 = (double) (h0.b - h0.a), span = (double) (e - a);
            for (size_t j = a; j < e; ++j)
            {
                double sp = (double) (j - a) * L0 / span;
                double v = interp ? std::abs (lerpAt (x, (double) a + sp)) : std::abs (x[a + std::min ((size_t) L0 - 1, (size_t) jr (sp))]);
                y[j] = (float) (v * sgn);
            }
            sgn = -sgn;
        }
        return y;
    });
}

// Adds 'harmonics' to every wavecycle: copies of the cycle read 2, 3, 4... times faster, at the given levels.
// after CDP distort harmonic (distorth/do_harmonic, distorth.c); the harmonics file becomes six level sliders
Chans harmonic (const Chans& ch, Ctx& cx, const Params& p)
{
    const int H[] = { 2, 3, 4, 5, 6, 8 }; const char* ids[] = { "h2", "h3", "h4", "h5", "h6", "h8" }; const double pre = p["pre"];
    return eachCh (ch, cx, [&] (const Buf& x0, int) {
        Buf x = x0; for (auto& v : x) v = (float) (v * pre);
        Buf y = x; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); ++i)
        {
            const size_t a = b[i], L = b[i + 1] - a;
            for (int n = 0; n < 6; ++n)
            {
                const double amp = p[ids[n]]; const size_t h = (size_t) H[n];
                if (amp <= 0 || h >= L / 4) continue;   // foldover
                size_t k = h;
                for (size_t j = a; j < a + L; ++j) { y[j] = (float) (y[j] + x[a + k] * amp); k += h; while (k >= L) k -= L; }
            }
        }
        return y;
    });
}

// Superimposes miniature copies of each group of SCALE wavecycles (the group squeezed into one cycle) onto every cycle of the group.
// after CDP distort fractal (distortf/do_fractal, distortf.c)
Chans fractal (const Chans& ch, Ctx& cx, const Params& p)
{
    const int S = std::max (2, p.i ("scale")); const double amp = p["loud"], pre = p["pre"];
    return eachCh (ch, cx, [&] (const Buf& x0, int) {
        Buf x = x0; for (auto& v : x) v = (float) (v * pre);
        Buf y = x, mini; auto b = cycB (x);
        for (size_t i = 0; i + (size_t) S < b.size(); i += (size_t) S)
        {
            mini.clear(); for (size_t j = b[i]; j < b[i + (size_t) S]; j += (size_t) S) mini.push_back (x[j]);
            if (mini.empty()) continue;
            const double gl = (double) mini.size();
            for (size_t n = 0; n < (size_t) S; ++n)
            {
                size_t a = b[i + n], e = b[i + n + 1]; double ratio = gl / (double) (e - a);
                for (size_t j = a; j < e; ++j) y[j] = (float) (y[j] + amp * mini[std::min (mini.size() - 1, (size_t) jr (ratio * (double) (j - a)))]);
            }
        }
        return y;
    });
}

// Wavecycles are reversed in groups of CYCLES.
// after CDP distort reverse (distort_rev, distortr.c)
Chans wreverse (const Chans& ch, Ctx& cx, const Params& p)
{
    const int G = std::max (1, p.i ("cycles"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y = x; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); i += (size_t) G) { size_t a = b[i], e = b[std::min (b.size() - 1, i + (size_t) G)]; std::reverse (y.begin() + (long) a, y.begin() + (long) e); }
        return y;
    });
}

// Groups of wavecycles are re-ordered by a domain-image pattern (abc-cba ...): items may be reordered, omitted or doubled.
// after CDP distort shuffle (distort_shuf, distorts.c)
const std::vector<std::pair<std::string, std::string>> SHUF_PAT {
    { "ab", "ba" }, { "abc", "cba" }, { "abc", "bca" }, { "abcd", "dcba" }, { "abcd", "badc" }, { "abcd", "cdab" },
    { "abc", "aabbcc" }, { "ab", "aab" }, { "abcd", "abbd" }, { "abcd", "aadd" }, { "abcdef", "fbdcea" } };
Chans shuffle (const Chans& ch, Ctx& cx, const Params& p)
{
    const int G = std::max (1, p.i ("cycles")); const auto& pat = SHUF_PAT[(size_t) clamp (p.i ("pattern"), 0, (int) SHUF_PAT.size() - 1)];
    const size_t D = pat.first.size();
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; y.reserve (x.size() * 2); auto b = cycB (x); const size_t nc = b.size() - 1;
        size_t i = 0;
        for (; i + D * (size_t) G <= nc; i += D * (size_t) G)
        {
            bool ok = true;
            for (char c : pat.second)
            {
                size_t g = (size_t) (c - 'a'); size_t a = b[i + g * (size_t) G], e = b[i + (g + 1) * (size_t) G];
                if (! (ok = app (y, x, a, e, cx.maxLen))) break;
            }
            if (! ok) return y;
        }
        app (y, x, b[i], x.size(), cx.maxLen);
        return y;
    });
}

// Every group of CYCLES wavecycles is repeated N times. STRETCH: the sound gets longer; SKIP (repeat2): the repeats
// replace what follows, so the length stays the same.
// after CDP distort repeat / repeat2 (distort_rpt, distrpt.c)
Chans repeatImpl (const Chans& ch, Ctx& cx, const Params& p, bool skip)
{
    const int N = std::max (1, p.i ("mult")), G = std::max (1, p.i ("cycles"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); i += (size_t) G * (skip ? (size_t) N : 1))
        {
            size_t a = b[i], e = b[std::min (b.size() - 1, i + (size_t) G)]; bool ok = true;
            for (int r = 0; r < N && ok; ++r) ok = app (y, x, a, e, cx.maxLen);
            if (! ok) break;
        }
        return y;
    });
}
Chans repeat (const Chans& ch, Ctx& cx, const Params& p) { return repeatImpl (ch, cx, p, false); }
Chans repeat2 (const Chans& ch, Ctx& cx, const Params& p) { return repeatImpl (ch, cx, p, true); }

// Like repeat, but cycles shorter than 1/LIMIT Hz are merged with their neighbours, so only cycles below LIMIT count.
// after CDP distort replim (distort_rpt_frqlim, distrpt.c)
Chans replim (const Chans& ch, Ctx& cx, const Params& p)
{
    const int N = std::max (1, p.i ("mult")), G = std::max (1, p.i ("cycles")); const size_t minL = (size_t) lenOf (1.0 / p["hilim"], cx.sr);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        auto b0 = cycB (x); std::vector<size_t> b { 0 };
        for (size_t i = 1; i < b0.size(); ++i) if (b0[i] - b.back() >= minL || i + 1 == b0.size()) b.push_back (b0[i]);
        Buf y;
        for (size_t i = 0; i + 1 < b.size(); i += (size_t) G)
        {
            size_t a = b[i], e = b[std::min (b.size() - 1, i + (size_t) G)]; bool ok = true;
            for (int r = 0; r < N && ok; ++r) ok = app (y, x, a, e, cx.maxLen);
            if (! ok) break;
        }
        return y;
    });
}

// Time-stretch: between successive wavecycles, N-1 new cycles morph in length and shape from one to the next.
// after CDP distort interpolate (do_interp, distrpt.c)
Chans interp (const Chans& ch, Ctx& cx, const Params& p)
{
    const int M = std::max (2, p.i ("mult"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); ++i)
        {
            const size_t a = b[i], L = b[i + 1] - a;
            if (i > 0)
            {
                const size_t pa = b[i - 1], PL = a - pa;
                for (int n = 1; n < M; ++n)
                {
                    size_t len = (size_t) std::max (1LL, (long long) PL + jr ((double) ((long long) L - (long long) PL) * n / M));
                    for (size_t k = 0; k < len; ++k)
                    {
                        if (y.size() >= cx.maxLen) return y;
                        double r = (double) k / (double) len;
                        double v0 = x[pa + std::min (PL - 1, (size_t) jr (r * (double) PL))], v1 = x[a + std::min (L - 1, (size_t) jr (r * (double) L))];
                        y.push_back ((float) ((v0 * (M - n) + v1 * n) / M));
                    }
                }
            }
            if (! app (y, x, a, a + L, cx.maxLen)) break;
            if (! (i & 255)) cx.prog (0.5 * (double) i / (double) b.size());
        }
        return y;
    });
}

// Time-contract: keep 1 in N cycles, keep only the strongest of each N, or delete only the weakest of each N.
// after CDP distort delete 1-3 (distort_del / distort_del_with_loudness, distdel.c)
Chans wdelete (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"), G = std::max (2, p.i ("cycles"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); i += (size_t) G)
        {
            size_t k = std::min (b.size() - 1 - i, (size_t) G), best = 0, worst = 0; double bp = -1, wp = 1e30;
            for (size_t n = 0; n < k; ++n) { double pk = cycPeak (x, b[i + n], b[i + n + 1]); if (pk > bp) { bp = pk; best = n; } if (pk < wp) { wp = pk; worst = n; } }
            bool ok = true;
            if (mode == 0) ok = app (y, x, b[i], b[i + 1], cx.maxLen);
            else if (mode == 1) ok = app (y, x, b[i + best], b[i + best + 1], cx.maxLen);
            else { ok = app (y, x, b[i], b[i + worst], cx.maxLen) && app (y, x, b[i + worst + 1], b[i + k], cx.maxLen); }
            if (! ok) break;
        }
        return y;
    });
}

// In every group of N wavecycles the strongest one replaces all the others.
// after CDP distort replace (distort_rpl, distrpl.c)
Chans wreplace (const Chans& ch, Ctx& cx, const Params& p)
{
    const int G = std::max (2, p.i ("cycles"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; y.reserve (x.size()); auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); i += (size_t) G)
        {
            size_t k = std::min (b.size() - 1 - i, (size_t) G), best = 0; double bp = -1;
            for (size_t n = 0; n < k; ++n) { double pk = cycPeak (x, b[i + n], b[i + n + 1]); if (pk > bp) { bp = pk; best = n; } }
            bool ok = true; for (size_t n = 0; n < k && ok; ++n) ok = app (y, x, b[i + best], b[i + best + 1], cx.maxLen);
            if (! ok) break;
        }
        return y;
    });
}

// Time-contract: every N wavecycles are telescoped (averaged) into one, as long as the longest or the mean of them.
// after CDP distort telescope (distort_tel/do_cycle_tele, disttel.c)
Chans telescope (const Chans& ch, Ctx& cx, const Params& p)
{
    const int G = std::max (2, p.i ("cycles")); const bool avg = p.i ("mode") == 1;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); i += (size_t) G)
        {
            size_t k = std::min (b.size() - 1 - i, (size_t) G), mx = 0; double tot = 0;
            for (size_t n = 0; n < k; ++n) { size_t l = b[i + n + 1] - b[i + n]; mx = std::max (mx, l); tot += (double) l; }
            size_t ref = avg ? (size_t) std::max (1.0, jsround (tot / (double) k)) : mx;
            for (size_t j = 0; j < ref; ++j)
            {
                if (y.size() >= cx.maxLen) return y;
                double r = (double) j / (double) ref, s = 0;
                for (size_t n = 0; n < k; ++n) { size_t l = b[i + n + 1] - b[i + n]; s += x[b[i + n] + std::min (l - 1, (size_t) jr (r * (double) l))]; }
                y.push_back ((float) (s / (double) k));
            }
        }
        return y;
    });
}

// Wavecycles whose frequency lies below / above / outside the limits are dropped (a time-contracting 'filter').
// after CDP distort filter 1-3 (distort_flt, distflt.c)
Chans wfilter (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); double f1 = p["freq"], f2 = p["freq2"]; if (mode == 2 && f2 < f1) std::swap (f1, f2);
    const double sr = cx.sr;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; auto b = cycB (x);
        for (size_t i = 0; i + 1 < b.size(); ++i)
        {
            double f = sr / (double) (b[i + 1] - b[i]);
            bool out = mode == 0 ? f < f1 : mode == 1 ? f > f1 : (f < f1 || f > f2);
            if (! out && ! app (y, x, b[i], b[i + 1], cx.maxLen)) break;
        }
        return y;
    });
}

// Every wavecycle is resampled by a random transposition that glides between random targets (within +-RANGE octaves)
// every random number (up to CYCLES) of cycles.
// after CDP distort pitch (distort_pitch/do_distrt, distortp.c)
Chans wpitch (const Chans& ch, Ctx& cx, const Params& p)
{
    const double oct = p["octvary"]; const int cmax = std::max (1, p.i ("cycles"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); Buf y; auto b = cycB (x);
        double last = 0, next = 0, step = 0; int pos = 0, glen = 0; bool init = true;
        for (size_t i = 0; i + 1 < b.size(); ++i)
        {
            if (init || ++pos >= glen)
            {
                if (! init) last = next;
                glen = (int) jr (cx.rand() * cmax) + 1; next = (cx.rand() * 2 - 1) * oct; step = (next - last) / glen; pos = 0; init = false;
            }
            else last += step;
            const size_t a = b[i], L = b[i + 1] - a; const long long nl = std::max (1LL, jr ((double) L * std::pow (2.0, last)));
            const double st = (double) L / (double) nl;
            for (long long k = 0; k < nl; ++k)
            {
                if (y.size() >= cx.maxLen) return y;
                double h = (double) k * st; size_t q = (size_t) h; double f = h - (double) q;
                double v0 = x[a + std::min (q, L - 1)], v1 = q + 1 < L ? x[a + q + 1] : 0.0;
                y.push_back ((float) (v0 + (v1 - v0) * f));
            }
        }
        return y;
    });
}

// Clips the signal at LEVEL and fills the clipped stretches with noise (NOISE) or with a waveform of FREQ (WAVE), DEPTH deep.
// after CDP distort overload 1-2 (distort_overload, distort.c)
Chans overload (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double g = p["gate"], depth = p["depth"], frq = p["freq"], norm = 1.0 / (g + 1.0);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); Buf y (x.size()); double ph = 0; int state = 0;
        for (size_t n = 0; n < x.size(); ++n)
        {
            double v = x[n];
            if (mode == 0)
            {
                if (v >= g) v = (1 - cx.rand() * depth) * g;
                else if (v <= -g) v = -(1 - cx.rand() * depth) * g;
            }
            else
            {
                int s = v >= g ? 1 : v <= -g ? -1 : 0;
                if (s != 0 && s != state) ph = 0;
                state = s;
                if (s != 0) v = (1 + std::sin (TAU * ph) * depth) * g * s;
                ph += frq / cx.sr; ph -= std::floor (ph);
            }
            y[n] = (float) (v * norm);
        }
        return y;
    });
}

// An enveloped impulse train: imposed on the source (IMPULSE), or a grabbed waveset loop played inside synthetic
// impulses (WAVESET MS: grab about GRAB ms; WAVESET N: grab GRAB wavesets), gliding by TRANSP over each impulse.
// after CDP distort pulsed 1-3 (do_pulsetrain, pulse.c); the envelope file becomes SHAPE, the transposition contour a glide
Chans pulsed (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"), shape = p.i ("shape"); const double sr = cx.sr;
    static const std::vector<std::vector<std::pair<double, double>>> ENV {
        { { 0, 0 }, { 0.02, 1 }, { 0.3, 0.4 }, { 1, 0 } },                          // PLUCK
        { { 0, 0 }, { 0.5, 1 }, { 1, 0 } },                                         // ARCH
        { { 0, 0 }, { 0.75, 1 }, { 1, 0 } },                                        // SWELL
        { { 0, 0 }, { 0.12, 1 }, { 0.35, 0.15 }, { 0.55, 0.8 }, { 1, 0 } } };       // DOUBLE
    const auto& env0 = ENV[(size_t) clamp (shape, 0, 3)];
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); const size_t n = x.size();
        size_t start = std::min (n - 1, (size_t) (p["pos"] * (double) (n - 1)));
        size_t total = std::min (cx.maxLen, (size_t) lenOf (p["dur"], sr));
        Buf tab; double tpos = (double) start;
        if (mode > 0)
        {   // grab whole wavesets from the start point (extract_waveform)
            auto b = cycB (x); size_t i = 0; while (i + 1 < b.size() && b[i + 1] <= start) ++i;
            size_t a = b[std::min (i, b.size() - 1)], e = a;
            const size_t want = mode == 1 ? (size_t) lenOf (p["grab"] / 1000, sr) : 0; const size_t wcnt = (size_t) std::max (1, p.i ("grab"));
            for (size_t k = i; k + 1 < b.size(); ++k)
            {
                size_t ne = b[k + 1];
                if (mode == 1) { if (ne - a >= want) { e = (want - (e - a) < ne - a - want && e > a) ? e : ne; break; } }
                else if (k - i + 1 >= wcnt) { e = ne; break; }
                e = ne;
            }
            if (e <= a) e = std::min (n, a + 1);
            tab.assign (x.begin() + (long) a, x.begin() + (long) e); tpos = 0;
        }
        if (mode == 0) total = std::min (total, n - start);
        Buf y; y.reserve (total);
        std::vector<std::pair<double, double>> env = env0; double envPos = 0, step = 0, stShift = 0;
        auto newPulse = [&] {
            double f = p["frq"] * semis ((cx.rand() * 2 - 1) * p["frand"]); step = f / sr;
            env = env0;
            for (size_t k = 1; k + 1 < env.size(); ++k)
            {   // randomise level (ARAND) and time (TRAND) of the inner peaks and troughs
                double lv = env0[k].second, r = (cx.rand() * 2 - 1) * p["arand"];
                env[k].second = clampd (lv + (r >= 0 ? (1 - lv) * r : lv * r), 0, 1);
                double lo = env[k - 1].first, hi = env0[k + 1].first, t = env0[k].first, q = (cx.rand() * 2 - 1) * p["trand"] * 0.5;
                env[k].first = q >= 0 ? t + (hi - t) * q : t + (t - lo) * q;
            }
            stShift = p["transp"] * (1 - p["tranrand"] + p["tranrand"] * (cx.rand() * 2 - 1));
        };
        newPulse();
        for (size_t s = 0; s < total; ++s)
        {
            double amp = 0;
            for (size_t k = 0; k + 1 < env.size(); ++k)
                if (envPos <= env[k + 1].first) { double d = env[k + 1].first - env[k].first; amp = d > 1e-12 ? env[k].second + (env[k + 1].second - env[k].second) * (envPos - env[k].first) / d : env[k + 1].second; break; }
            const double rate = semis (stShift * envPos);
            double v;
            if (mode == 0) { v = lerpAt (x, tpos); tpos += rate; if (tpos >= (double) n) break; }
            else { v = lerpAt (tab, tpos); tpos += rate; while (tpos >= (double) tab.size()) tpos -= (double) tab.size(); }
            y.push_back ((float) (v * amp));
            envPos += step;
            if (envPos >= 1) { envPos = 0; newPulse(); if (mode == 0) tpos = std::floor (tpos + 1); else tpos = 0; }
            if (! (s & 65535)) cx.prog ((double) s / (double) total);
        }
        return y;
    });
}

// ================================================================ SFEDIT NOISECUT (dev/editsf/cut.c)

// Finds noisy stretches (wavecycles shorter than 1/NOISE FRQ) and silences them, or keeps only them (KEEP NOISE).
// Noise shorter than MIN NOISE and tone shorter than MIN TONE are ignored; edges are spliced.
// after CDP sfedit noisecut (do_noise_suppression, cut.c)
Chans noisecut (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const Buf m = monoOf (ch); const size_t n = m.size();
    const size_t maxL = (size_t) lenOf (1.0 / p["noisefrq"], sr), minN = (size_t) lenOf (p["minnoise"] / 1000, sr), minT = (size_t) lenOf (p["mintone"] / 1000, sr);
    const size_t spl = (size_t) lenOf (p["splice"] / 1000, sr);
    auto b = cycB (m);
    // a cycle is noisy when shorter than 1/NOISE FRQ; a stretch counts as noise when noisy cycles fill most of
    // the surrounding 4 ms (so a few noisy zero crossings inside a tone don't split it)
    std::vector<float> nz (n, 0.f);
    for (size_t i = 0; i + 1 < b.size(); ++i) if (b[i + 1] - b[i] < maxL) for (size_t j = b[i]; j < b[i + 1]; ++j) nz[j] = 1;
    const size_t W = (size_t) lenOf (0.002, sr); DVec ps (n + 1, 0.0);
    for (size_t j = 0; j < n; ++j) ps[j + 1] = ps[j] + nz[j];
    struct Run { size_t a, e; bool noise; };
    std::vector<Run> runs;
    for (size_t i = 0; i + 1 < b.size(); ++i)
    {
        size_t c = (b[i] + b[i + 1]) / 2, a = c > W ? c - W : 0, e = std::min (n, c + W);
        bool isN = ps[e] - ps[a] > 0.5 * (double) (e - a);
        if (! runs.empty() && runs.back().noise == isN) runs.back().e = b[i + 1]; else runs.push_back ({ b[i], b[i + 1], isN });
    }
    auto merge = [&] (bool which, size_t minLen) {
        for (auto& r : runs) if (r.noise == which && r.e - r.a < minLen) r.noise = ! which;
        std::vector<Run> o; for (auto& r : runs) { if (! o.empty() && o.back().noise == r.noise) o.back().e = r.e; else o.push_back (r); }
        runs.swap (o);
    };
    merge (true, minN); merge (false, minT);
    const bool keepNoise = p.i ("mode") == 1;
    std::vector<float> gate (n, 0.f);
    for (auto& r : runs) if (r.noise == keepNoise) for (size_t j = r.a; j < r.e; ++j) gate[j] = 1;
    // linear splices on both sides of every gate edge
    std::vector<float> g2 = gate;
    for (size_t j = 1; j < n; ++j)
        if (gate[j] != gate[j - 1])
        {
            const bool up = gate[j] > gate[j - 1];
            for (size_t k = 0; k < spl; ++k)
            {
                double v = (double) k / (double) spl;
                if (up) { if (j + k < n) g2[j + k] = (float) std::min ((double) g2[j + k], v); }
                else if (j >= k + 1) g2[j - 1 - k] = (float) std::min ((double) g2[j - 1 - k], v);
            }
        }
    Chans o;
    for (auto& x : ch) { Buf y (n); for (size_t j = 0; j < n; ++j) y[j] = x[j] * g2[j]; o.push_back (std::move (y)); }
    cx.prog (1);
    return o;
}

// ================================================================ PITCH-SYNCHRONOUS GRAINS (psow, fofex, tweet)

// Pitch-synchronous grain ("FOF") boundaries of the mono mix: one grain per pitch period, cut at the upward zero
// crossing nearest each period (CDP finds them with autocorrelation guided by a pitch file; here the YIN tracker
// supplies the period). Unvoiced stretches are cut at the nearest voiced period (150 Hz if nothing is pitched).
struct PSG { std::vector<size_t> cut; std::vector<double> f0; size_t n = 0; size_t count() const { return cut.size() - 1; } };
PSG psGrains (const Chans& ch, Ctx& cx)
{
    PSG g; const Buf m = monoOf (ch); const size_t n = m.size(); g.n = n; const double sr = cx.sr;
    PitchTrack pt = trackPitch (m, sr, 60, 1200, 0.15, &cx);
    std::vector<double> f = pt.f0; double fill = 0;
    for (double v : f) if (v > 0) { fill = v; break; }
    if (fill <= 0) fill = 150;
    for (auto& v : f) { if (v > 0) fill = v; else v = fill; }
    g.cut.push_back (0);
    size_t t = 0;
    while (true)
    {
        double fr = f[(size_t) clamp (jr ((double) t / (double) pt.hop), 0LL, (long long) f.size() - 1)];
        double T = clampd (sr / fr, 16, sr / 20); size_t target = t + (size_t) T;
        if (target + 8 >= n) break;
        size_t lo = target - (size_t) (T / 4), hi = std::min (n - 1, target + (size_t) (T / 4)), best = target, bd = (size_t) -1;
        for (size_t j = std::max ((size_t) 1, lo); j <= hi; ++j) if (m[j - 1] < 0 && m[j] >= 0) { size_t d = j > target ? j - target : target - j; if (d < bd) { bd = d; best = j; } }
        g.cut.push_back (best); g.f0.push_back (sr / (double) (best - t)); t = best;
    }
    g.cut.push_back (n); g.f0.push_back (g.f0.empty() ? fill : g.f0.back());
    return g;
}
inline bool zeroSeg (const Buf& x, size_t a, size_t b) { for (size_t i = a; i < b; ++i) if (x[i] != 0) return false; return true; }
// adds g * x[a, b) at output position pos (grows y up to cap)
inline void addAt (Buf& y, const Buf& x, size_t a, size_t b, long long pos, double g, size_t cap)
{
    if (pos < 0) { a += (size_t) (-pos); pos = 0; }
    if (a >= b || (size_t) pos >= cap) return;
    const size_t end = std::min (cap, (size_t) pos + (b - a));
    if (y.size() < end) y.resize (end);
    for (size_t i = (size_t) pos; i < end; ++i) y[i] = (float) (y[i] + x[a + i - (size_t) pos] * g);
}
// CDP's grain vibrato: a positional shift of each grain following a sine of VIBFRQ, DEPTH semitones
struct Vib
{
    double pos = 0; long long val = 0;
    long long shift (long long seglen, double frq, double dep, double sr)
    {
        if (dep <= 0 || frq <= 0) return 0;
        double len = std::max (1.0, sr / frq);
        if (val) pos = std::fmod (pos + TAU * (double) seglen / len, TAU);
        val += seglen;
        return jr (std::sin (pos) * (semis (dep) - 1) * (double) seglen);
    }
};
// half-cycle-peak envelope of a grain, zero at both ends (getfofenv)
Buf fofEnv (const Buf& x, size_t a, size_t b)
{
    const size_t L = b - a; Buf env (L, 0.f); std::vector<std::pair<size_t, double>> pts { { 0, 0.0 } };
    size_t i = a;
    while (i < b)
    {
        int s = x[i] > 0 ? 1 : x[i] < 0 ? -1 : 0; double mx = 0; size_t at = i;
        if (s == 0) { ++i; continue; }
        while (i < b && x[i] * (float) s >= 0) { if (std::abs (x[i]) > mx) { mx = std::abs (x[i]); at = i; } ++i; }
        pts.push_back ({ at - a, mx });
    }
    pts.push_back ({ L, 0.0 });
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        for (size_t j = pts[k].first; j < pts[k + 1].first && j < L; ++j)
            env[j] = (float) (pts[k].second + (pts[k + 1].second - pts[k].second) * (double) (j - pts[k].first) / (double) std::max ((size_t) 1, pts[k + 1].first - pts[k].first));
    return env;
}
inline size_t chunkEnd (const PSG& g, size_t k) { return g.cut[std::min (k, g.count())]; }

// Time-stretch by repositioning chunks of SEGS grains at STRETCH times their original start (the grains themselves keep their length).
// after CDP psow stretch (psoa_weird PSOW_STRETCH, psow.c)
Chans psStretch (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const double st = p["stretch"]; const size_t j = (size_t) std::max (1, p.i ("segs"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; const size_t cap = std::min (cx.maxLen, (size_t) ((double) x.size() * st) + x.size() + 16);
        for (size_t k = 0; k < g.count(); k += j) { size_t a = g.cut[k], b = chunkEnd (g, k + j); addAt (y, x, a, b, jr ((double) a * st), 1, cap); }
        return y;
    });
}
// Time-stretch by playing every chunk of SEGS grains REPEATS times.
// after CDP psow dupl (psoa_weird PSOW_DUPL, psow.c)
Chans psDupl (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const int R = std::max (1, p.i ("reps")); const size_t j = (size_t) std::max (1, p.i ("segs"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y;
        for (size_t k = 0; k < g.count(); k += j) { bool ok = true; for (int r = 0; r < R && ok; ++r) ok = app (y, x, g.cut[k], chunkEnd (g, k + j), cx.maxLen); if (! ok) break; }
        return y;
    });
}
// Time-shrink: keeps one chunk of SEGS grains in every KEEP 1 IN chunks.
// after CDP psow delete (psoa_weird PSOW_DEL, psow.c)
Chans psDelete (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const size_t N = (size_t) std::max (2, p.i ("keep")), j = (size_t) std::max (1, p.i ("segs"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; for (size_t k = 0; k < g.count(); k += j * N) if (! app (y, x, g.cut[k], chunkEnd (g, k + j), cx.maxLen)) break;
        return y;
    });
}
// PSOLA: chunks are placed at STRETCH times their start and repeated every chunk-length / transposition, which shifts the pitch by TRANSPOSE.
// after CDP psow strtrans (psoa_weird PSOW_STRFILL, psow.c)
Chans psStrTrans (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const double st = p["stretch"], tr = semis (p["trans"]); const size_t j = (size_t) std::max (1, p.i ("segs"));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; const size_t cap = std::min (cx.maxLen, (size_t) ((double) x.size() * st) + x.size() + 16);
        double next = 0;   // the copies continue on one grid across chunks, so the new pitch period stays regular
        for (size_t k = 0; k < g.count(); k += j)
        {
            size_t a = g.cut[k], b = chunkEnd (g, k + j); const double seglen = (double) (b - a), end = (double) b * st, step = std::max (1.0, seglen / tr);
            if (next < (double) a * st - seglen) next = (double) a * st;
            for (; next < end && (size_t) next < cap; next += step) addAt (y, x, a, b, jr (next), 0.7, cap);
        }
        return y;
    });
}
// Grabs SEGS grains at POSITION and builds a sound of DURATION from them: DENSITY copies per grain length, spectrally
// transposed by SPEC TRANSP, with RANDOM timing.
// after CDP psow grab (psoa_weird PSOW_FREEZE, psow.c)
Chans psGrab (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const size_t j = (size_t) std::max (1, p.i ("segs")); const double tr = semis (p["spec"]), dens = p["dens"];
    size_t gi = 0; const size_t at = (size_t) (p["pos"] * (double) g.n);
    while (gi + 1 < g.count() && g.cut[gi + 1] <= at) ++gi;
    const size_t a = g.cut[gi], b = chunkEnd (g, gi + j); const size_t total = std::min (cx.maxLen, (size_t) lenOf (p["dur"], cx.sr));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]);
        Buf seg; seg.push_back (x[a]);   // transposed copy of the chunk (its tail stops at the zero crossing, as in CDP)
        for (double q = tr; q < (double) (b - a); q += tr) { double v = lerpAt (x, (double) a + q); if ((double) a + q + 1 >= (double) b && v < 0) v = 0; seg.push_back ((float) v); }
        Buf y; const double seglen = (double) (b - a);
        for (double t = 0; t < (double) total;)
        {
            addAt (y, seg, 0, seg.size(), jr (t), 1, total);
            double d = seglen / dens; d += (cx.rand() - 0.5) * p["rand"] * d; t += std::max (1.0, d);
        }
        return y;
    });
}
// Two grains (at POS A and POS B) as a voice: START sustains grain A, INTERP morphs its shape and pitch into grain B,
// END sustains B; VIBRATO and TREMOLO animate it.
// after CDP psow interp (psoa_interp/do_pitchsync_grain_interp, psow.c); both grains come from the one input
Chans psInterp (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const double sr = cx.sr; const Buf m = monoOf (ch);
    auto grainAt = [&] (double u) {
        size_t at = (size_t) (u * (double) g.n), k = 0; while (k + 1 < g.count() && g.cut[k + 1] <= at) ++k;
        for (size_t d = 0; k + d < g.count(); ++d) if (cycPeak (m, g.cut[k + d], g.cut[k + d + 1]) > 1e-4) return k + d;   // nearest non-silent grain
        return k; };
    const size_t ga = grainAt (p["posa"]), gb = grainAt (p["posb"]);
    const double sd = p["sdur"] * sr, id = p["idur"] * sr, ed = p["edur"] * sr; const size_t total = std::min (cx.maxLen, (size_t) (sd + id + ed) + 1);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        const size_t a0 = g.cut[ga], b0 = g.cut[ga + 1], a1 = g.cut[gb], b1 = g.cut[gb + 1]; const double LA = (double) (b0 - a0), LB = (double) (b1 - a1);
        Buf y; y.reserve (total); double t = 0;
        while (t < (double) total)
        {
            double w = t < sd ? 0 : t < sd + id ? (t - sd) / std::max (1.0, id) : 1;
            double L = (LA + (LB - LA) * w) * semis (-p["vibdep"] * std::sin (TAU * p["vibfrq"] * t / sr)); L = std::max (4.0, L);
            double trem = 1 - p["tremdep"] * 0.5 * (1 - std::cos (TAU * p["tremfrq"] * t / sr)); if (p["tremdep"] > 1) trem /= p["tremdep"];
            const size_t n = (size_t) L;
            for (size_t i = 0; i < n && y.size() < total; ++i)
            {
                double r = (double) i / (double) n;
                y.push_back ((float) (((1 - w) * lerpAt (x, (double) a0 + r * LA) + w * lerpAt (x, (double) a1 + r * LB)) * trem));
            }
            t += (double) n;
        }
        return y;
    });
}
// Vocal features: TRANSPOSE by grain spacing (TIMEWARP: duration changes; KEEP TIME: grains repeat to keep it), VIBRATO,
// SPEC TRANSP (formant shift), HOARSE (random timing), SUBHARM (every Nth grain louder, SUB LEVEL), FOF STRETCH.
// after CDP psow features 1-2 (psoa_weird PSOW_FEATURES / fof_stretch, psow.c)
Chans psFeatures (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const size_t j = (size_t) std::max (1, p.i ("segs")); const int mode = p.i ("mode"), sub = p.i ("sub"), fstr = std::max (1, p.i ("fofstr"));
    const double tr = semis (p["trans"]), spec = semis (p["spec"]), sr = cx.sr;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); Vib vib; Buf y; double opos = 0; long long cnt = 0;
        const size_t cap = std::min (cx.maxLen, (size_t) ((double) x.size() * std::max (1.0, 1.0 / tr) * (fstr > 1 ? 2.0 : 1.0)) + 2 * (size_t) sr);
        for (size_t k = 0; k < g.count() && (size_t) opos < cap;)
        {
            const size_t a = g.cut[k], b = chunkEnd (g, k + j); const double seglen = (double) (b - a);
            double gain = 1; if (sub > 1 && cnt % sub != 0) gain *= 1 - p["subgain"];
            ++cnt;
            if (! zeroSeg (x, a, b))
            {
                long long o = jr (opos);
                if (fstr > 1)
                {   // repeat the waveset holding the grain's peak (fof_stretch)
                    size_t mx = a; for (size_t i = a; i < b; ++i) if (std::abs (x[i]) > std::abs (x[mx])) mx = i;
                    size_t ws = a, we = b;
                    for (size_t i = a + 1; i < b; ++i) if (x[i - 1] >= 0 && x[i] < 0) { if (i <= mx) ws = i; else { we = i; break; } }
                    addAt (y, x, a, ws, o, gain, cap); o += (long long) (ws - a);
                    for (int r = 0; r < fstr; ++r) { addAt (y, x, ws, we, o, gain, cap); o += (long long) (we - ws); }
                    addAt (y, x, we, b, o, gain, cap);
                }
                else if (std::abs (spec - 1) > 1e-9)
                {
                    Buf s; for (double q = 0; q < seglen; q += spec) s.push_back ((float) lerpAt (x, (double) a + q));
                    addAt (y, s, 0, s.size(), o, gain, cap);
                }
                else addAt (y, x, a, b, o, gain, cap);
            }
            const double outlen = std::max (1.0, seglen / tr);
            opos += outlen;
            const bool repeat = mode == 1 && tr > 1 && opos < (double) b;   // KEEP TIME: duplicate the grain until the output catches up
            opos += (double) vib.shift ((long long) outlen, p["vibfrq"], p["vibdep"], sr);
            if (p["hoarse"] > 0) opos += (double) jr ((cx.rand() - 0.5) * p["hoarse"] * outlen);
            if (opos < 0) opos = 0;
            if (! repeat) k += j;
            // KEEP TIME transposing down: grains are dropped while the output is ahead, so the length stays the source's
            if (mode == 1 && tr < 1) while (k < g.count() && (double) g.cut[k] + 0.5 * seglen < opos) k += j;
            if (! (cnt & 1023)) cx.prog (0.5 + 0.5 * (double) k / (double) g.count());
        }
        return y;
    });
}
const std::vector<std::vector<double>> CHORDS { { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 7, 12, 19 }, { 0, 5, 7, 12 }, { 0, 2, 4, 7, 9 } };
// The vocal grains' envelopes are imposed on a synthetic oscillator stream: a CHORD on ROOT, the HARMONIC series,
// a chord that GLIDEs by GLIDE semitones, a chord PROGRESSION, or NOISE. DEPTH mixes in the unshaped tone.
// after CDP psow synth 1-5 (superimpose_fofs_on_synthtones, psow.c); the oscillator data becomes chord options
Chans psSynth (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const int mode = p.i ("mode"); const double sr = cx.sr, depth = p["depth"];
    const auto& chord = CHORDS[(size_t) clamp (p.i ("chord"), 0, (int) CHORDS.size() - 1)];
    const double root = 440 * std::pow (2.0, (p["root"] - 69) / 12); const double prog[] = { 0, 5, 7, 0 };
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); Buf y (x.size(), 0.f); std::vector<double> fr, am, ph (8, 0.0);
        for (size_t k = 0; k < g.count(); ++k)
        {
            const size_t a = g.cut[k], b = g.cut[k + 1], L = b - a;
            const double u = (double) a / (double) std::max ((size_t) 1, x.size());
            fr.clear(); am.clear();
            if (mode == 1) for (int h = 1; h <= 8; ++h) { fr.push_back (root * h); am.push_back (1.0 / h); }
            else
            {
                double shift = mode == 2 ? p["glide"] * u : mode == 3 ? prog[std::min (3, (int) (u * 4))] : 0;
                for (double c : chord) { fr.push_back (root * semis (c + shift)); am.push_back (1); }
            }
            // the oscillators run on as one continuous stream (CDP: FOFs imposed on a stream of synthesized sound); restarting
            // them at every grain (each shorter than one cycle of ROOT) gave a DC-heavy buzz at the voice's pitch, not the chord
            std::vector<double> ph0 (ph.begin(), ph.begin() + (long) fr.size());
            for (size_t o = 0; o < fr.size(); ++o) { ph[o] += fr[o] * (double) L / sr; ph[o] -= std::floor (ph[o]); }
            if (L < 2 || zeroSeg (x, a, b)) continue;
            Buf s (L);
            for (size_t i = 0; i < L; ++i)
            {
                double v = 0;
                if (mode == 4) v = cx.rand() * 2 - 1;
                else for (size_t o = 0; o < fr.size(); ++o) if (fr[o] < sr * 0.45) v += std::sin (TAU * (ph0[o] + fr[o] * (double) i / sr)) * am[o];
                s[i] = (float) v;
            }
            // one fixed scale for the whole stream (a per-grain peak normalisation steps the level at every grain: clicks)
            double mx = mode == 4 ? 1.0 : 0.0; if (mode != 4) for (size_t o = 0; o < fr.size(); ++o) if (fr[o] < sr * 0.45) mx += am[o];
            if (mx < 1e-12) continue;
            Buf env = fofEnv (x, a, b);
            for (size_t i = 0; i < L; ++i) { double v = s[i] / mx; y[a + i] = (float) (v * (1 - depth) * 0.25 + v * depth * env[i]); }
        }
        return y;
    });
}
// Every group of SUBHARM grains is played as a subharmonic (the group keeps its own length) while the grains inside are
// packed closer, transposed up by UP TRANSP. BALANCE weighs the up-transposed grains against the subharmonic.
// after CDP psow split (psoa_weird PSOW_SPLIT, psow.c)
Chans psSplit (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const size_t S = (size_t) std::max (2, p.i ("sub")); const double up = semis (p["up"]), bal = p["bal"];
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; const size_t cap = std::min (cx.maxLen, x.size() + (size_t) cx.sr);
        for (size_t k = 0; k < g.count(); k += S)
        {
            const size_t e = std::min (g.count(), k + S); double avg = 0; int cnt = 0;
            for (size_t q = k; q < e; ++q) if (! zeroSeg (x, g.cut[q], g.cut[q + 1])) { avg += (double) (g.cut[q + 1] - g.cut[q]); ++cnt; }
            if (! cnt) continue;
            const double step = std::max (1.0, avg / cnt / up); double pos = (double) g.cut[k];
            for (size_t q = k; q < e; ++q, pos += step)
            {
                double gain = bal <= 1 ? (q > k ? bal : 1.0) : (q == k ? 1.0 / bal : 1.0);
                addAt (y, x, g.cut[q], g.cut[q + 1], jr (pos), gain, cap);
            }
        }
        return y;
    });
}
// Successive grains go to SUBHARM different stereo positions (SEPARATION apart): the voice splits into a subharmonic
// that moves across space. L/R BAL tips the balance, HI SUPPRESS quietens the later grains of each set.
// after CDP psow space (psoa_weird PSOW_SPACE / spacecalc, psow.c)
Chans psSpace (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const int S = std::max (2, p.i ("sub")); const double sep = p["separ"], rel = p["relev"], sup = p["hisup"];
    const Buf x = monoOf (ch); Buf L (x.size()), R (x.size()); int sw = 0;
    for (size_t k = 0; k < g.count(); ++k)
    {
        double pos = 0;
        switch (sw) { case 0: pos = sep; break; case 1: pos = -sep; break; case 2: pos = 0; break; case 3: pos = sep / 2; break; default: pos = -sep / 2; break; }
        double rp = std::abs (pos), dist = std::sqrt (2.0) / std::sqrt (1 + rp * rp), tt = (pos + 1) / 2, gr = tt * dist, gl = (1 - tt) * dist;
        double s = std::pow (sup * (double) sw / (double) (S - 1), 0.25); gl *= 1 - s; gr *= 1 - s;
        sw = (sw + 1) % S;
        if (rel < 1) gr *= rel; else gl /= rel;
        for (size_t i = g.cut[k]; i < g.cut[k + 1]; ++i) { L[i] = (float) (x[i] * gl); R[i] = (float) (x[i] * gr); }
    }
    cx.prog (1);
    return { L, R };
}
// Plays up to POSITION, then sustains the chunk of SEGS grains found there for SUSTAIN seconds (stepping by TRANSP,
// with VIBRATO; SMOOTH windows the grain), then plays the rest.
// after CDP psow sustain (psoa_weird PSOW_EXTEND, psow.c)
Chans psSustain (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const size_t j = (size_t) std::max (1, p.i ("segs")); const double sr = cx.sr, tr = semis (p["trans"]);
    size_t gi = 0; const size_t at = (size_t) (p["pos"] * (double) g.n);
    while (gi + j < g.count() && g.cut[gi + j] <= at) gi += j;
    const double holdLen = p["dur"] * sr; const bool smooth = p.i ("smooth") != 0;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Vib vib; Buf y; const size_t cap = std::min (cx.maxLen, x.size() + (size_t) holdLen + (size_t) sr); long long opos = 0;
        for (size_t k = 0; k < g.count(); k += j)
        {
            const size_t a = g.cut[k], b = chunkEnd (g, k + j); const long long seglen = (long long) (b - a);
            if (k != gi)
            {
                addAt (y, x, a, b, opos, 1, cap); opos += seglen + vib.shift (seglen, p["vibfrq"], p["vibdep"], sr);
                if (opos < 0) opos = 0;
                continue;
            }
            Buf seg (x.begin() + (long) a, x.begin() + (long) b);
            if (smooth) for (size_t i = 0; i < seg.size(); ++i) seg[i] = (float) (seg[i] * std::sin (PI * (double) i / (double) seg.size()));
            const long long step = std::max (1LL, (long long) std::floor ((double) seglen / tr)), limit = opos + (long long) holdLen;
            while (opos < limit && (size_t) opos < cap) { addAt (y, seg, 0, seg.size(), opos, 1, cap); opos += step + vib.shift (step, p["vibfrq"], p["vibdep"], sr); if (opos < 0) opos = 0; }
        }
        return y;
    });
}
// Sustains one grain cut by hand: POSITION and LENGTH, snapped to zero crossings and moved by NUDGE crossings; it
// repeats for SUSTAIN seconds with VIBRATO between the original start and the rest.
// after CDP psow sustain2 (psoa_weird PSOW_EXTEND2, psow.c)
Chans psSustain2 (const Chans& ch, Ctx& cx, const Params& p)
{
    const Buf m = monoOf (ch); const size_t n = m.size(); const double sr = cx.sr;
    std::vector<size_t> zc; for (size_t i = 1; i < n; ++i) if (m[i - 1] < 0 && m[i] >= 0) zc.push_back (i);
    auto nearZ = [&] (double pos) -> long long {
        if (zc.empty()) return -1;
        size_t t = (size_t) pos; auto it = std::lower_bound (zc.begin(), zc.end(), t); long long i = it - zc.begin();
        if (i > 0 && (it == zc.end() || t - zc[(size_t) i - 1] < *it - t)) --i;
        return i; };
    const double s0 = p["pos"] * (double) (n - 1); long long ia = nearZ (s0), ib = nearZ (s0 + p["len"] / 1000 * sr);
    size_t a, b;
    if (ia < 0 || zc.size() < 2) { a = std::min (n - 1, (size_t) s0); b = std::min (n, a + (size_t) lenOf (p["len"] / 1000, sr)); }
    else
    {
        const long long nz = (long long) zc.size(), nu = p.i ("nudge");
        ia = clamp (ia + nu, 0LL, nz - 2); ib = clamp (ib + nu, ia + 1, nz - 1);
        a = zc[(size_t) ia]; b = zc[(size_t) ib];
    }
    if (b <= a) b = std::min (n, a + 1);
    const double holdLen = p["dur"] * sr;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Vib vib; Buf y; const size_t cap = std::min (cx.maxLen, n + (size_t) holdLen + (size_t) sr);
        app (y, x, 0, a, cap); long long opos = (long long) a; const long long seglen = (long long) (b - a), limit = opos + (long long) holdLen;
        do { addAt (y, x, a, b, opos, 1, cap); opos += std::max (1LL, seglen + vib.shift (seglen, p["vibfrq"], p["vibdep"], sr)); } while (opos < limit && (size_t) opos < cap);
        y.resize (std::min (cap, (size_t) opos)); app (y, x, b, n, cap);
        return y;
    });
}
// Every grain is copied again 2, 3, 4, 5, 6 or 8 times within its own period at the given LEVELS: this reinforces those
// harmonics. DELAY offsets each added harmonic; SKIP SHARED omits copies that coincide with lower harmonics.
// after CDP psow reinforce 1 (psoa_weird PSOW_REINF, psow.c); the harmonics file becomes level sliders
Chans psReinforce (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const int H[] = { 2, 3, 4, 5, 6, 8 }; const char* ids[] = { "h2", "h3", "h4", "h5", "h6", "h8" };
    const double dly = p["delay"] / 1000 * cx.sr; const bool skip = p.i ("skip") != 0;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; const size_t cap = std::min (cx.maxLen, x.size() + (size_t) cx.sr);
        for (size_t k = 0; k < g.count(); ++k)
        {
            const size_t a = g.cut[k], b = g.cut[k + 1]; const double seglen = (double) (b - a);
            addAt (y, x, a, b, (long long) a, 1, cap);
            int m = 0;
            for (int hi = 0; hi < 6; ++hi)
            {
                const double amp = p[ids[hi]]; if (amp <= 0) continue;
                const int h = H[hi]; ++m;
                for (int q = 0; q < h; ++q)
                {
                    if (skip)
                    {   // q/h coincides with a copy of the grain itself or of a lower harmonic
                        bool dup = q == 0; for (int lo = 0; lo < hi && ! dup; ++lo) if (p[ids[lo]] > 0 && (q * H[lo]) % h == 0) dup = true;
                        if (dup) continue;
                    }
                    addAt (y, x, a, b, jr ((double) a + m * dly + q * seglen / h), amp, cap);
                }
            }
        }
        return y;
    });
}
// Inharmonic reinforcement: every grain is copied at intervals of its period / PARTIAL (fractional ratios), the copies
// sustaining for WEIGHT periods: bell-like, inharmonic resonance.
// after CDP psow reinforce 2 (psoa_weird PSOW_REINF mode 2, psow.c); the partials file becomes three ratio/level pairs
Chans psReinforce2 (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const char* pid[] = { "p1", "p2", "p3" }; const char* lid[] = { "l1", "l2", "l3" }; const double w = p["weight"];
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; const size_t cap = std::min (cx.maxLen, x.size() + (size_t) cx.sr);
        for (size_t k = 0; k < g.count(); ++k)
        {
            const size_t a = g.cut[k], b = g.cut[k + 1]; const double seglen = (double) (b - a);
            addAt (y, x, a, b, (long long) a, 1, cap);
            for (int m = 0; m < 3; ++m)
            {
                const double r = p[pid[m]], amp = p[lid[m]]; if (amp <= 0) continue;
                for (int q = 1; (double) q < r * w; ++q) addAt (y, x, a, b, jr ((double) a + q * seglen / r), amp, cap);
            }
        }
        return y;
    });
}
// FOF resynthesis: the input's grains form a bank sorted by pitch; the output places one FOF (or the sum of ALL, LOW, MID
// or HIGH FOFs, or 2-3 FOFs weighted by pitch and level) at every period of the input's pitch (+TRANSP) or a FIXED pitch.
// after CDP fofex extract + construct 1-7 (fofexex/fofexco, fofex.c); the pitch and envelope files come from the input
Chans fofCons (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const int mode = p.i ("mode"); const double sr = cx.sr; const Buf m = monoOf (ch); const size_t n = m.size();
    PitchTrack pt = trackPitch (m, sr, 60, 1200, 0.15, nullptr);
    bool anyV = false; for (double v : pt.f0) if (v > 0) { anyV = true; break; }
    const bool fixed = p.i ("pitch") == 1 || ! anyV;
    const double tr = semis (p["transp"]), ff = 440 * std::pow (2.0, (p["note"] - 69) / 12);
    DVec env (n); { double e = 0, rel = std::exp (-1.0 / (0.02 * sr)); for (size_t i = 0; i < n; ++i) { e = std::max ((double) std::abs (m[i]), e * rel); env[i] = e; } }
    double emax = 0; for (double v : env) emax = std::max (emax, v);
    std::vector<size_t> bank;   // the FOF bank: non-silent grains, ordered by pitch (low to high)
    for (size_t k = 0; k < g.count(); ++k) if (cycPeak (m, g.cut[k], g.cut[k + 1]) > 1e-4 && g.cut[k + 1] - g.cut[k] >= 8) bank.push_back (k);
    if (bank.empty()) for (size_t k = 0; k < g.count(); ++k) bank.push_back (k);
    std::stable_sort (bank.begin(), bank.end(), [&] (size_t a, size_t b) { return g.f0[a] < g.f0[b]; });
    const size_t B = bank.size(); const bool norm = p.i ("norm") != 0;
    auto pick = [&] (double u) { return bank[(size_t) clamp ((long long) (u * (double) (B - 1) + 0.5), 0LL, (long long) B - 1)]; };
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        auto fofOf = [&] (size_t k) {
            Buf f (x.begin() + (long) g.cut[k], x.begin() + (long) g.cut[k + 1]); const size_t L = f.size(), e = std::max ((size_t) 1, L / 16);
            for (size_t i = 0; i < e && i < L; ++i) { double w = (double) i / (double) e; f[i] = (float) (f[i] * w); f[L - 1 - i] = (float) (f[L - 1 - i] * w); }
            if (norm) { double pk = peak (f); if (pk > 1e-9) for (auto& v : f) v = (float) (v / pk); }
            return f; };
        auto sumFofs = [&] (size_t lo, size_t hi) {
            Buf s; const size_t step = std::max ((size_t) 1, (hi - lo) / 256);   // at most ~256 FOFs summed
            for (size_t i = lo; i < hi; i += step) { Buf f = fofOf (bank[i]); if (s.size() < f.size()) s.resize (f.size()); for (size_t q = 0; q < f.size(); ++q) s[q] += f[q]; }
            double pk = peak (s); if (pk > 1e-9) for (auto& v : s) v = (float) (v / pk * 0.9);
            return s; };
        Buf f1, f2, f3;
        switch (mode)
        {
            case 0: f1 = fofOf (pick (p["f1"])); break;
            case 1: f1 = sumFofs (0, B); break;
            case 2: f1 = sumFofs (0, std::max ((size_t) 1, B / 3)); break;
            case 3: f1 = sumFofs (B / 3, std::max (B / 3 + 1, 2 * B / 3)); break;
            case 4: f1 = sumFofs (std::min (B - 1, 2 * B / 3), B); break;
            default: f1 = fofOf (pick (p["f1"])); f2 = fofOf (pick (p["f2"])); f3 = fofOf (pick (p["f3"])); break;
        }
        const double pmin = std::log2 (std::min (p["minf"], p["maxf"])), pmax = std::log2 (std::max (p["minf"], p["maxf"])), lmin = std::min (p["minl"], p["maxl"]), lmax = std::max (p["minl"], p["maxl"]);
        Buf y; const size_t cap = std::min (cx.maxLen, n + (size_t) (0.1 * sr)); double t = 0;
        while (t < (double) n)
        {
            double f = fixed ? ff : pt.at (t) * tr;
            if (f <= 0) { t += 0.0025 * sr; continue; }
            f = clampd (f, 20, sr / 4);
            const double amp = emax > 0 ? env[std::min (n - 1, (size_t) t)] / emax : 0; const long long o = (long long) t;
            if (mode <= 4) addAt (y, f1, 0, f1.size(), o, amp, cap);
            else
            {
                double gh = clampd ((std::log2 (f) - pmin) / std::max (1e-6, pmax - pmin), 0, 1) * 0.75, gl = 1 - gh, g3 = 0;
                if (mode == 6) { g3 = clampd ((amp - lmin) / std::max (1e-6, lmax - lmin), 0, 1) * 0.75; gh *= 1 - g3; gl *= 1 - g3; }
                addAt (y, f1, 0, f1.size(), o, amp * gl, cap); addAt (y, f2, 0, f2.size(), o, amp * gh, cap);
                if (mode == 6) addAt (y, f3, 0, f3.size(), o, amp * g3, cap);
            }
            t += sr / f;
        }
        return y;
    });
}
// Every vocal grain is replaced by a synthetic chirped 'tweet' of PEAKS cycles (or at FREQ) or by NOISE, shaped by the
// grain's own formant envelope. Grains more than MIN LEVEL below the loudest are silenced; WINDOW smooths the edges.
// after CDP tweet 1-3 (tweet/extract_formant_shape/impulse, tweet.c)
Chans tweet (const Chans& ch, Ctx& cx, const Params& p)
{
    const PSG g = psGrains (ch, cx); const int mode = p.i ("mode"); const double sr = cx.sr, sharp = 1.0 / std::max (0.05, p["chirp"]);
    const Buf m = monoOf (ch); double loud = 0; for (size_t k = 0; k < g.count(); ++k) loud = std::max (loud, cycPeak (m, g.cut[k], g.cut[k + 1]));
    const double minLev = p["minlev"] >= 0 ? 0 : loud * std::pow (10.0, p["minlev"] / 20);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); Buf y (x.size(), 0.f);
        for (size_t k = 0; k < g.count(); ++k)
        {
            const size_t a = g.cut[k], b = g.cut[k + 1], L = b - a;
            if (L < 4 || cycPeak (m, a, b) < std::max (minLev, 1e-6)) continue;
            Buf f (x.begin() + (long) a, x.begin() + (long) b);
            if (p.i ("win"))
            {
                size_t sp = std::min (L / 2, (size_t) std::max (1LL, jr (0.002 * sr)));
                for (size_t i = 0; i < sp; ++i) { double w = 0.5 - 0.5 * std::cos (PI * (double) (i + 1) / (double) sp); f[i] = (float) (f[i] * w); f[L - 1 - i] = (float) (f[L - 1 - i] * w); }
            }
            Buf env = fofEnv (f, 0, L);
            Buf imp (L, 0.f); const size_t hl = L / 2;
            if (mode == 2) for (size_t i = 0; i < 2 * hl; ++i) imp[i] = (float) (cx.rand() * 2 - 1);
            else
            {
                double pk = mode == 0 ? p["pkcnt"] : std::max (1.0, jsround (p["frq"] * (double) L / sr));
                double range = ((double) L < pk * 4 ? (double) (L / 4) : pk) * PI;
                for (size_t i = 0; i < hl; ++i) { double v = std::sin (std::pow ((double) i / (double) hl, sharp) * range); imp[hl + i] = (float) -v; imp[hl - 1 - i] = (float) v; }
            }
            double mx = peak (imp); if (mx < 1e-12) continue;
            for (size_t i = 0; i < L; ++i) y[a + i] = (float) (imp[i] / mx * env[i]);
        }
        return y;
    });
}

// ================================================================ RELEASE 7-8 WAVESET PROGRAMS (dev/standnew, dev/science)

// half-waveset boundaries: every sign change (zeros stay with the run they follow); b[0] = 0, b.back() = n
std::vector<size_t> halfB (const Buf& x)
{
    std::vector<size_t> b { 0 }; int s = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        int t = x[i] > 0 ? 1 : x[i] < 0 ? -1 : 0;
        if (t != 0) { if (s != 0 && t != s) b.push_back (i); s = t; }
    }
    if (b.back() != x.size()) b.push_back (x.size());
    return b;
}
// onset times of the mono mix (rise of 6 dB within 30 ms after a quieter frame, at least MINGAP apart); always starts with 0.
// Replaces CDP's hand-made mark lists; falls back to COUNT even divisions when the sound has no clear onsets.
std::vector<size_t> onsets (const Chans& ch, double sr, size_t fallbackCount, double minGap = 0.08)
{
    const Buf m = monoOf (ch); const size_t n = m.size(), hop = (size_t) std::max (16.0, 0.01 * sr);
    std::vector<double> db;
    for (size_t a = 0; a < n; a += hop) { double s = 0; size_t e = std::min (n, a + hop); for (size_t i = a; i < e; ++i) s += (double) m[i] * m[i]; db.push_back (10 * std::log10 (s / (double) (e - a) + 1e-12)); }
    double top = -200; for (double v : db) top = std::max (top, v);
    std::vector<size_t> on { 0 }; const size_t gap = (size_t) (minGap * sr);
    for (size_t f = 3; f < db.size(); ++f)
    {
        double lo = std::min ({ db[f - 1], db[f - 2], db[f - 3] });
        if (db[f] - lo > 6 && db[f] > top - 40 && db[f] >= db[f - 1] && f * hop - on.back() >= gap) on.push_back ((f - 1) * hop);
    }
    if (on.size() < 2 && fallbackCount > 1) { on.clear(); for (size_t k = 0; k < fallbackCount; ++k) on.push_back (n * k / fallbackCount); }
    return on;
}
// Head/Tail marks (CDP's marklist): every onset starts a HEAD of HEAD ms, the rest until the next onset is its TAIL
std::vector<size_t> headTail (const Chans& ch, double sr, double headMs)
{
    const size_t n = ch[0].size(); auto on = onsets (ch, sr, 8); std::vector<size_t> mk; const size_t hl = (size_t) lenOf (headMs / 1000, sr);
    for (size_t k = 0; k < on.size(); ++k)
    {
        size_t nx = k + 1 < on.size() ? on[k + 1] : n;
        mk.push_back (on[k]); mk.push_back (std::min (nx, on[k] + std::min (hl, (nx - on[k]) / 2)));
    }
    mk.push_back (n);
    std::vector<size_t> o; for (size_t v : mk) if (o.empty() || v > o.back()) o.push_back (v);   // drop empty segments
    return o;
}
// appends x[a, b) (reversed if rev) with SPL-long fades, overlapping the previous segment's fade-out (CDP's splice joins)
void joinSeg (Buf& y, const Buf& x, size_t a, size_t b, bool rev, size_t spl, size_t cap)
{
    if (b <= a) return;
    const size_t L = b - a, s = std::min (spl, L / 2), start = y.size() >= s ? y.size() - s : 0;
    if (start >= cap) return;
    y.resize (std::min (cap, std::max (y.size(), start + L)));
    for (size_t i = 0; i < L && start + i < y.size(); ++i)
    {
        double v = rev ? x[b - 1 - i] : x[a + i], g = 1;
        if (s > 0) { if (i < s) g = (double) i / (double) s; else if (L - 1 - i < s) g = (double) (L - 1 - i) / (double) s; }
        y[start + i] = (float) (y[start + i] + v * g);
    }
}

// Hard-clips the signal at LEVEL, then restores the original peak level.
// after CDP clip clip 1 (clip, clip.c)
Chans clip1 (const Chans& ch, Ctx& cx, const Params& p)
{
    const double lv = std::max (1e-4, p["level"]);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        double pk = peak (x); Buf y = x; const double g = pk > lv ? pk / lv : 1.0;
        for (auto& v : y) v = (float) (clampd (v, -lv, lv) * g);
        return y;
    });
}
// Clips every half wave cycle at FRACTION of its own peak: level-independent, soft passages are squared off as hard as loud ones.
// after CDP clip clip 2 (clip, clip.c)
Chans clip2 (const Chans& ch, Ctx& cx, const Params& p)
{
    const double fr = p["frac"];
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y = x;
        for (auto& h : halves (x)) { const float lim = (float) (h.pk * fr); for (size_t j = h.a; j < h.b; ++j) y[j] = clamp (y[j], -lim, lim); }
        return y;
    });
}
// Raises sample values to POWER, scaled to each half-waveset's own peak (HALF CYCLE) or to the whole sound's peak (WHOLE).
// after CDP quirk quirk 1-2 (quirk, quirk.c)
Chans quirk (const Chans& ch, Ctx& cx, const Params& p)
{
    const double pf = p["pow"]; const int mode = p.i ("mode");
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y = x; const double gpk = peak (x);
        for (auto& h : halves (x))
        {
            const double ref = mode == 0 ? (double) h.pk : gpk; if (ref <= 0) continue;
            for (size_t j = h.a; j < h.b; ++j) y[j] = (float) (std::pow (std::abs (x[j]) / ref, pf) * ref * h.sg);
        }
        return y;
    });
}
// SHIFT: alternate groups of half-cycles are replaced by the group SHIFT groups earlier (wrapping round). SWAP: alternate
// groups trade places in a 0-3-2-1 pattern. GROUP = 1 half cycle, 2 = one cycle + a half, ...
// after CDP distshift distshift 1-2 (distort_shift / distort_swap, distshift.c)
Chans distshift (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const long long gp = 2LL * std::max (1, p.i ("grp")) - 1, shift = 2LL * std::max (1, p.i ("shift")) * gp;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        auto b = halfB (x); const long long N = (long long) b.size() - 1; Buf y; y.reserve (x.size());
        auto put = [&] (long long h) { h %= N; if (h < 0) h += N; app (y, x, b[(size_t) h], b[(size_t) h + 1], cx.maxLen); };
        if (mode == 0)
            for (long long k = 0; k < N; k += gp * 2)
            {
                for (long long h = k; h < std::min (k + gp, N); ++h) put (h);
                long long j = k + gp - shift; while (j < 0) j += N;
                for (long long h = j; h < j + gp && k + gp < N; ++h) put (h);
            }
        else
        {
            long long j = -3 * gp;
            while (true)
            {
                if ((j += gp * 3) + gp > N) break;
                for (long long h = j; h < j + gp; ++h) put (h);
                if ((j += gp * 3) + gp > N) break;
                for (long long h = j; h < j + gp; ++h) put (h);
                j -= gp; for (long long h = j; h < j + gp; ++h) put (h);
                j -= gp; for (long long h = j; h < j + gp; ++h) put (h);
            }
        }
        return y;
    });
}
// Progressive wavefolding: an offset growing by WARP per sample (PER SAMPLE) or per CYCLES wavesets (PER WAVESET) is added
// and the result folded back into range; the output follows the source's envelope.
// after CDP distwarp distwarp 1-2 (distwarp, distwarp.c)
Chans distwarp (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"), wc = std::max (1, p.i ("cycles")); const double w = p["warp"]; const size_t win = (size_t) lenOf (0.02, cx.sr);
    auto fold = [] (double v) { long long i = (long long) std::floor (v); if (i & 1) ++i; long long j = i / 2; return (j & 1) ? (double) i - v : v - (double) i; };
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        const size_t n = x.size(); std::vector<double> env; double mx = 0;
        for (size_t i = 0; i < n; ++i) { mx = std::max (mx, (double) std::abs (x[i])); if ((i + 1) % win == 0) { env.push_back (mx); mx = 0; } }
        env.push_back (mx); env.push_back (mx);
        Buf y (n); double inc = w; int phase = 0, init = 0, cnt = 0;
        for (size_t i = 0; i < n; ++i)
        {
            size_t e = i / win; double fr = (double) (i - e * win) / (double) win, gain = env[e] + (env[std::min (env.size() - 1, e + 1)] - env[e]) * fr;
            double v = x[i] + inc;
            if (mode == 0) { double f = v - std::floor (v); v = std::floor (v) + std::sin (f * PI / 2); inc += w; }
            else
            {
                if (! init) { if (x[i] != 0) { init = x[i] > 0 ? 1 : -1; phase = init; } }
                else if (phase == init) { if ((init > 0 && x[i] < 0) || (init < 0 && x[i] >= 0)) phase = -phase; }
                else if ((init > 0 && x[i] >= 0) || (init < 0 && x[i] < 0)) { phase = -phase; if (++cnt >= wc) { cnt = 0; inc += w; } }
            }
            y[i] = (float) (fold (v) * gain);
            if (inc > 1e6) inc = std::fmod (inc, 4.0);   // the fold has period 4: keep the offset small
        }
        return y;
    });
}
// Every group of CYCLES wavesets is played MULTIPLY times as one spliced block. STRETCH: the sound gets longer;
// SKIP: playback then jumps to the waveset nearest the output time, so the length stays.
// after CDP distrep distrep 1-2 (distrep, distrep.c)
Chans distrep (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"), R = std::max (1, p.i ("mult")), G = std::max (1, p.i ("cycles")); const size_t spl = (size_t) lenOf (p["splice"] / 1000, cx.sr);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        auto b = cycB (x); Buf y; size_t s = 0; const size_t nb = b.size() - 1;
        while (s < nb && y.size() < cx.maxLen)
        {
            size_t e = std::min (nb, s + (size_t) G); const size_t a = b[s], c = b[e], len = c - a, tot = len * (size_t) R, sp = std::min (spl, tot / 2);
            size_t k = 0;
            for (int r = 0; r < R; ++r) for (size_t i = a; i < c; ++i, ++k)
            {
                if (y.size() >= cx.maxLen) return y;
                double g = k < sp ? (double) k / (double) sp : tot - 1 - k < sp ? (double) (tot - 1 - k) / (double) sp : 1.0;
                y.push_back ((float) (x[i] * g));
            }
            if (mode == 0) s = e;
            else
            {
                size_t w = y.size(), q = (size_t) (std::lower_bound (b.begin(), b.end(), w) - b.begin());
                if (q >= nb) break;
                if (q > 0 && w - b[q - 1] < b[q] - w) --q;
                s = std::max (q, s + 1);
            }
        }
        return y;
    });
}
// After OFFSET, every group of CYCLES wavesets repeats REPEATS times until DURATION is reached; TELESCOPE skips
// wavesets so the output keeps the source's pace.
// after CDP distortt repeat (do_distortt, distortt.c)
Chans distortt (const Chans& ch, Ctx& cx, const Params& p)
{
    const int G = std::max (1, p.i ("cycles")), R = std::max (1, p.i ("reps")); const bool tel = p.i ("tele") != 0;
    const size_t off = (size_t) (p["offset"] / 1000 * cx.sr), total = std::min (cx.maxLen, (size_t) lenOf (p["dur"], cx.sr));
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; const size_t o = std::min (off, x.size()); app (y, x, 0, o, total);
        Buf rest (x.begin() + (long) o, x.end()); auto b = cycB (rest);
        for (size_t i = 0; i + 1 < b.size() && y.size() < total; i += (size_t) G * (tel ? (size_t) R : 1))
        {
            size_t a = b[i], e = b[std::min (b.size() - 1, i + (size_t) G)]; bool ok = true;
            for (int r = 0; r < R && ok; ++r) ok = app (y, rest, a, e, total);
            if (! ok) break;
        }
        return y;
    });
}
// Every waveset is squeezed 2^OCTAVES times and repeated to fill its own length: octaves up, same timing.
// after CDP distmore double (distdouble, distmore.c)
Chans wdouble (const Chans& ch, Ctx& cx, const Params& p)
{
    const size_t g = (size_t) 1 << clamp (p.i ("oct"), 1, 4);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; y.reserve (x.size()); auto b = cycB (x); Buf w;
        for (size_t i = 0; i + 1 < b.size(); ++i)
        {
            const size_t a = b[i], L = b[i + 1] - a;
            if (L <= g) { app (y, x, a, a + L, cx.maxLen); continue; }
            // the squeezed waveset (every g-th sample) repeated to fill exactly the waveset's own length (as CDP's octave
            // case): appending the last sample and rounding the repeat count made every waveset a few samples longer,
            // so the sound drifted longer (+3% at 1 octave, +7% at 2) and the octave went flat
            w.clear(); const size_t wl = (L + g - 1) / g;
            for (size_t k = 0; k < L; ++k) w.push_back (x[a + (k % wl) * g]);
            app (y, w, cx.maxLen);
        }
        return y;
    });
}
// Cuts the sound into Heads (onset + HEAD ms) and Tails and re-orders the segments by brightness (zero-crossing rate),
// INCREASING or DECREASING: heads plus head-sized bits of tails, heads and tails as found, or tails only.
// after CDP distmore bright 1-3 (distbright, distmore.c); the mark list comes from an onset detector
Chans bright (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const bool dec = p.i ("order") == 1; const double sr = cx.sr; const size_t spl = (size_t) lenOf (p["splice"] / 1000, sr);
    auto mk = headTail (ch, sr, p["head"]); std::vector<std::pair<size_t, size_t>> segs;
    for (size_t k = 0; k + 1 < mk.size(); ++k)
    {
        const bool head = (k & 1) == 0; const size_t a = mk[k], b = mk[k + 1];
        if (mode == 2 && head) continue;
        if (mode == 0 && ! head)
        {   // tails cut into pieces about the size of the preceding head
            size_t hl = k > 0 ? std::max ((size_t) 16, mk[k] - mk[k - 1]) : b - a;
            for (size_t s = a; s < b; s += hl) segs.push_back ({ s, std::min (b, s + hl) });
        }
        else segs.push_back ({ a, b });
    }
    const Buf m = monoOf (ch); std::vector<double> zr;
    for (auto& s : segs) { size_t z = 0; for (size_t i = s.first + 1; i < s.second; ++i) if ((m[i - 1] < 0) != (m[i] < 0)) ++z; zr.push_back ((double) z / (double) std::max ((size_t) 1, s.second - s.first)); }
    std::vector<size_t> ord (segs.size()); for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
    std::stable_sort (ord.begin(), ord.end(), [&] (size_t a, size_t b) { return dec ? zr[a] > zr[b] : zr[a] < zr[b]; });
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; for (size_t i : ord) joinSeg (y, x, segs[i].first, std::min (x.size(), segs[i].second + spl), false, spl, cx.maxLen);
        return y;
    });
}
// Reverses certain segments of the Head/Tail chain: TAILS, HEADS, HEAD+TAIL pairs, or sets of 3-8 segments (odd modes keep
// the head forward and reverse the following set, even modes reverse whole sets).
// after CDP distmore segsbkwd 1-9 (segsbkwd, distmore.c); the mark list comes from an onset detector
Chans segsbkwd (const Chans& ch, Ctx& cx, const Params& p)
{
    const int d = p.i ("mode"); const double sr = cx.sr; const size_t spl = (size_t) lenOf (0.015, sr);
    auto mk = headTail (ch, sr, p["head"]); std::vector<size_t> t;
    if (d <= 1) t = mk;
    else if (d == 2) { for (size_t n = 0; n < mk.size(); n += 2) t.push_back (mk[n]); if (t.back() != mk.back()) t.push_back (mk.back()); }
    else
    {
        const size_t modd = (size_t) ((d & 1) ? d + 1 : d);
        for (size_t n = 0; n < mk.size(); ++n) { size_t k = n % modd; if (k == 0 || ((d & 1) && k == 1)) t.push_back (mk[n]); }
        if (t.back() != mk.back()) t.push_back (mk.back());
    }
    bool fwd = d == 0 || (d >= 3 && (d & 1)); const bool alt = d <= 1 || (d >= 3 && (d & 1));
    std::vector<bool> rev; for (size_t k = 0; k + 1 < t.size(); ++k) { rev.push_back (! fwd); if (alt) fwd = ! fwd; }
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; for (size_t k = 0; k + 1 < t.size(); ++k) joinSeg (y, x, t[k], std::min (x.size(), t[k + 1] + spl), rev[k], spl, cx.maxLen);
        return y;
    });
}
// Zigzags back and forth REPEATS times over every Tail (TAILS) or over the whole sound (WHOLE; FILL DUR repeats to reach
// DURATION), each zig shrinking toward SHRINK TO ms (LOG: geometric shrink). PROPORTION limits how much of a tail is used.
// after CDP distmore segszig 1-3 (segszig, distmore.c)
Chans segszig (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double sr = cx.sr; const size_t spl = (size_t) lenOf (0.015, sr), minsz = (size_t) (p["shrink"] / 1000 * sr);
    const bool lg = p.i ("log") != 0; const double prop = p["prop"];
    std::vector<size_t> mk = mode == 0 ? headTail (ch, sr, p["head"]) : std::vector<size_t> { 0, 0, ch[0].size() };
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; const size_t n = x.size();
        auto zig = [&] (size_t a, size_t L, int R) {
            // continuous back-and-forth reading: passes alternate direction, each shorter (turning points move inward)
            const bool shrink = R > 1 && minsz > 0 && minsz <= L;
            const double step = shrink ? (double) (L - minsz) / (2.0 * R) : 0, ratio = shrink ? std::pow ((double) minsz / (double) L, 1.0 / (2.0 * R)) : 1;
            double pos = (double) a, len = (double) L; int dir = 1; Buf seg;
            for (int pass = 0; pass < 2 * R; ++pass)
            {
                const size_t cnt = (size_t) std::max (2.0, len);
                for (size_t i = 0; i < cnt; ++i) { seg.push_back ((float) lerpAt (x, clampd (pos, 0, (double) n - 1))); pos += dir; }
                pos -= dir; dir = -dir;
                if (shrink) len = lg ? len * ratio : len - step;
                if (seg.size() + y.size() > cx.maxLen) break;
            }
            joinSeg (y, seg, 0, seg.size(), false, spl, cx.maxLen);
        };
        if (mode == 0)
        {
            for (size_t k = 0; k + 1 < mk.size() && y.size() < cx.maxLen; ++k)
            {
                const size_t a = mk[k], b = std::min (n, mk[k + 1]);
                if ((k & 1) == 0) joinSeg (y, x, a, std::min (n, b + spl), false, spl, cx.maxLen);
                else { size_t L = std::max ((size_t) 2, (size_t) ((double) (b - a) * prop)); zig (a, std::min (L, n - a), std::max (1, p.i ("reps"))); }
            }
        }
        else
        {
            const size_t L = std::max ((size_t) 2, (size_t) ((double) n * prop)); int R = std::max (1, p.i ("reps"));
            if (mode == 2) R = std::max (1, (int) std::ceil (p["dur"] * sr / (2.0 * (double) L)));
            zig (0, L, std::min (R, 512));
        }
        return y;
    });
}
// Waveset groups of about UNIT ms are taken at MARKS points (evenly spaced or at onsets); between marks the output morphs
// shape and length from one group to the next, over the mark distance times STRETCH. RAND shortens, FLIP inverts alternate
// copies; ALTERNATE interpolates only in every other block. TAIL appends the rest of the source.
// after CDP distmark distmark 1-2 (distmark / distmark2 / get_timeinterpcnt, distmark.c)
Chans distmark (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const double sr = cx.sr, st = p["tstretch"], rnd = p["rand"]; const bool flip = p.i ("flip") != 0, tail = p.i ("tail") != 0;
    const size_t n = ch[0].size(), cnt = (size_t) std::max (2, p.i ("count")), unit = (size_t) lenOf (p["unit"] / 1000, sr);
    std::vector<size_t> marks;
    if (p.i ("marks") == 1) marks = onsets (ch, sr, cnt);
    else for (size_t k = 0; k < cnt; ++k) marks.push_back (n * k / cnt);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); auto b = cycB (x);
        struct Gp { size_t a, len; };
        std::vector<Gp> gps;
        for (size_t mkp : marks)
        {   // whole wavesets from the first waveset start at or after the mark, about UNIT long
            size_t i = (size_t) (std::lower_bound (b.begin(), b.end(), mkp) - b.begin()); if (i + 1 >= b.size()) continue;
            size_t e = i + 1; while (e + 1 < b.size() && b[e] - b[i] < unit) ++e;
            if (gps.empty() || b[i] > gps.back().a) gps.push_back ({ b[i], b[e] - b[i] });
        }
        Buf y; if (gps.empty()) return x;
        app (y, x, 0, gps[0].a, cx.maxLen);
        bool inv = false; Buf ia, ib, ip;
        for (size_t g = 0; g + 1 < gps.size() && y.size() < cx.maxLen; ++g)
        {
            const Gp& A = gps[g]; const Gp& B = gps[g + 1];
            if (mode == 1 && (g & 1)) { app (y, x, A.a, B.a, cx.maxLen); continue; }   // ALTERNATE: this block plays as it was
            app (y, x, A.a, A.a + A.len, cx.maxLen);
            const double avg = 0.5 * (double) (A.len + B.len), outStep = (double) (B.a - A.a) * st;
            long long T = std::max (2LL, jr (outStep / avg)); if (flip && (T & 1)) ++T;
            const size_t big = std::max (A.len, B.len);
            ia = resamp (Buf (x.begin() + (long) A.a, x.begin() + (long) (A.a + A.len)), (long long) big);
            ib = resamp (Buf (x.begin() + (long) B.a, x.begin() + (long) (B.a + B.len)), (long long) big);
            for (long long j = 1; j < T && y.size() < cx.maxLen; ++j)
            {
                ip.resize (big); const double w = (double) j / (double) T;
                for (size_t k = 0; k < big; ++k) ip[k] = (float) (ia[k] * (1 - w) + ib[k] * w);
                double len = (double) A.len + ((double) B.len - (double) A.len) * w;
                if (rnd > 0) len *= 1 - cx.rand() * 0.5 * rnd;
                Buf s = resamp (ip, std::max (2LL, jr (len)));
                if (flip) { inv = ! inv; if (inv) { std::reverse (s.begin(), s.end()); for (auto& v : s) v = -v; } }
                app (y, s, cx.maxLen);
            }
        }
        const Gp& Z = gps.back();
        app (y, x, Z.a, tail ? x.size() : Z.a + Z.len, cx.maxLen);
        return y;
    });
}
// Recursive transposition pattern: the four STEP transpositions (over PATTERN seconds of input) are applied, then applied
// again inside every step, and so on LEVELS deep. Read speed follows the pattern, so time contracts and expands too.
// STRETCH scales the pattern's time, WARP its intervals; SHRINK narrows intervals at smaller scales.
// after CDP fractal wave 1 (create_fractal_pattern / fractalise, fractal.c); the shape file becomes 4 step sliders
Chans fractalWave (const Chans& ch, Ctx& cx, const Params& p)
{
    const double sr = cx.sr; const double steps[] = { p["s1"], p["s2"], p["s3"], p["s4"] };
    const double fdur = p["pdur"] * (p["tstr"] > 0 ? p["tstr"] : 1.0); const bool shr = p.i ("shrink") != 0; const double warp = p["warp"];
    // build the pattern: (start time, semitones) elements, recursively subdivided
    std::vector<std::pair<double, double>> pat { { 0, steps[0] }, { 0.25, steps[1] }, { 0.5, steps[2] }, { 0.75, steps[3] } };
    int levels = std::max (1, p.i ("levels"));
    while (levels > 1 && fdur / std::pow (4.0, levels - 1) * sr < 8) --levels;   // stop at the smallest useful element (as CDP's max fractalisation)
    for (int l = 1; l < levels; ++l)
    {
        std::vector<std::pair<double, double>> nx; const double unit = std::pow (0.25, l);
        for (auto& e : pat) for (int q = 0; q < 4; ++q) nx.push_back ({ e.first + unit * 0.25 * q, e.second + steps[q] * (shr ? unit : 1.0) });
        pat.swap (nx);
    }
    for (auto& e : pat) e.second = std::pow (2.0, e.second * warp / 12);
    const size_t E = pat.size(); const double elem = fdur * sr / (double) E;
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        Buf y; double pos = 0; const double n = (double) x.size();
        while (pos < n - 1 && y.size() < cx.maxLen)
        {
            for (size_t k = 0; k < E && pos < n - 1; ++k)
            {
                const double inc = pat[k].second; double local = 0;
                while (local < elem && pos < n - 1 && y.size() < cx.maxLen) { y.push_back ((float) lerpAt (x, pos)); pos += inc; local += inc; }
            }
            cx.prog (0.5 + 0.5 * pos / n);
        }
        return y;
    });
}
// Re-assembles the wavesets (in groups of GROUP) in RANDOM or PERMUTED order (for DURATION), sorted by SIZE (pitch) or LEVEL,
// rising, falling or rise-then-fall, globally or per onset segment, with random TRANSPOSE and ATTEN per group.
// after CDP scramble scramble 1-14 (scramble / write_waveset, scramble.c); the cut list comes from an onset detector
Chans scramble (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"), G = std::max (1, p.i ("cnt")); const double trn = p["trns"], att = p["atten"], sr = cx.sr;
    const auto cuts = onsets (ch, sr, 4);
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]); auto b0 = cycB (x); std::vector<size_t> b;
        for (size_t i = 0; i < b0.size(); i += (size_t) G) b.push_back (b0[i]);
        if (b.back() != b0.back()) b.push_back (b0.back());
        const size_t U = b.size() - 1; Buf y;
        auto put = [&] (size_t u) {
            const size_t a = b[u], L = b[u + 1] - a; const double inc = semis ((cx.rand() * 2 - 1) * trn), amp = 1 - cx.rand() * att;
            for (double q = 0; q < (double) L && y.size() < cx.maxLen; q += inc) y.push_back ((float) (lerpAt (x, (double) a + q) * amp));
        };
        if (mode <= 1)
        {
            const size_t total = std::min (cx.maxLen, (size_t) lenOf (p["dur"], sr)); std::vector<size_t> deck;
            while (y.size() < total)
            {
                size_t u;
                if (mode == 0) u = std::min (U - 1, (size_t) (cx.rand() * (double) U));
                else
                {
                    if (deck.empty()) { for (size_t i = 0; i < U; ++i) deck.push_back (i); for (size_t i = U - 1; i > 0; --i) std::swap (deck[i], deck[(size_t) (cx.rand() * (double) (i + 1)) % (i + 1)]); }
                    u = deck.back(); deck.pop_back();
                }
                put (u);
            }
            y.resize (std::min (y.size(), total));
            return y;
        }
        const bool byLevel = mode >= 8, perSeg = (mode >= 4 && mode <= 7) || mode >= 10;
        const int kind = mode <= 3 ? mode - 2 : mode <= 7 ? mode - 4 : mode <= 9 ? mode - 8 : mode - 10;   // 0 up, 1 down, 2 up-down, 3 down-up
        std::vector<double> key (U);
        for (size_t u = 0; u < U; ++u) key[u] = byLevel ? cycPeak (x, b[u], b[u + 1]) : (double) (b[u + 1] - b[u]);
        std::vector<std::pair<size_t, size_t>> segs;   // ranges of units
        if (perSeg || kind >= 2)
        {
            size_t s = 0;
            for (size_t c = 1; c <= cuts.size(); ++c)
            {
                size_t e = c < cuts.size() ? (size_t) (std::lower_bound (b.begin(), b.end(), cuts[c]) - b.begin()) : U;
                e = std::min (e, U); if (e > s) { segs.push_back ({ s, e }); s = e; }
            }
            if (! perSeg) segs = { { 0, U } };
        }
        else segs = { { 0, U } };
        for (auto& sg : segs)
        {
            std::vector<size_t> ord; for (size_t u = sg.first; u < sg.second; ++u) ord.push_back (u);
            const bool down = kind == 1 || kind == 3;
            std::stable_sort (ord.begin(), ord.end(), [&] (size_t a, size_t c) { return down ? key[a] > key[c] : key[a] < key[c]; });
            if (kind >= 2)
            {   // rise then fall (or fall then rise): every other element out, the rest back
                std::vector<size_t> o2; for (size_t i = 0; i < ord.size(); i += 2) o2.push_back (ord[i]);
                for (size_t i = ord.size() - (ord.size() & 1 ? 2 : 1); i < ord.size(); i -= 2) o2.push_back (ord[i]);
                ord.swap (o2);
            }
            for (size_t u : ord) put (u);
        }
        return y;
    });
}
// Splinters: the group of CYCLES wavesets at POSITION is repeated as pulses that shrink (PITCH modes: squeezed, pitch rises
// toward 1000/MIN SIZE Hz; LENGTH modes: cut to MIN SIZE ms) over SHRINK COUNT pulses plus EXTRA fully shrunk ones, the pulse
// rate moving from PULSE 1 to PULSE 2. INTO: splinters lead into the original; FROM: they emerge from it.
// after CDP splinter splinter 1-4 (splinter / shrink_waveset / generate_*_set, splinter.c)
Chans splinter (const Chans& ch, Ctx& cx, const Params& p)
{
    const int mode = p.i ("mode"); const bool before = mode == 0 || mode == 2, pitchMode = mode <= 1; const double sr = cx.sr;
    const int S = std::max (1, p.i ("shrcnt")), O = std::max (0, p.i ("ocnt")), E = S + O, W = std::max (1, p.i ("wcnt"));
    const Buf mm = monoOf (ch); auto bm = cycB (mm); const size_t n = mm.size();
    size_t gi = (size_t) (std::lower_bound (bm.begin(), bm.end(), (size_t) (p["pos"] * (double) (n - 1))) - bm.begin());
    gi = std::min (gi, bm.size() >= 2 ? bm.size() - 2 : 0);
    const size_t ga = std::min (bm[gi], n >= 2 ? n - 2 : 0), ge = bm[std::min (bm.size() - 1, gi + (size_t) W)], glen = std::min (n - ga, std::max ((size_t) 2, ge > ga ? ge - ga : 0));
    const double minlen = pitchMode ? std::max (2.0, p["minsz"] / 1000 * sr * W) : std::max (2.0, p["minsz"] / 1000 * sr);
    const double target = std::min (minlen, (double) glen);
    std::vector<double> shr (E), sep (E);   // per splinter, ordered from the original outward
    for (int k = 0; k < E; ++k)
    {
        double f = k < S ? std::pow ((double) (k + 1) / S, p["scv"]) : 1.0;
        shr[(size_t) k] = ((double) glen - f * ((double) glen - target)) / (double) glen;
        double d1 = p["p1"] > 0 ? 1.0 / p["p1"] : (double) glen / sr, d2 = p["p2"] > 0 ? 1.0 / p["p2"] : d1;
        sep[(size_t) k] = d1 + (d2 - d1) * std::pow ((double) (k + 1) / E, p["pcv"]);
    }
    return eachCh (ch, cx, [&] (const Buf& x, int) {
        cx.seed (p["seed"]);
        auto splint = [&] (double s) {
            const double rs = clampd (s * (1 + (cx.rand() * 2 - 1) * p["rand"] * 0.5), 0.01, 1.5);
            if (pitchMode) return resamp (Buf (x.begin() + (long) ga, x.begin() + (long) (ga + glen)), std::max (2LL, jr ((double) glen * rs)));
            size_t mx = ga; for (size_t i = ga; i < ga + glen; ++i) if (std::abs (x[i]) > std::abs (x[mx])) mx = i;   // cut around the peak
            const size_t pre = mx - ga, post = ga + glen - mx, ps = (size_t) ((double) pre * std::min (1.0, rs)), qs = (size_t) ((double) post * std::min (1.0, rs));
            Buf o (x.begin() + (long) (mx - ps), x.begin() + (long) (mx + qs)); const size_t sp = std::min (o.size() / 4, (size_t) lenOf (0.002, sr));
            for (size_t i = 0; i < sp; ++i) { double w = (double) i / (double) sp; o[i] = (float) (o[i] * w); o[o.size() - 1 - i] = (float) (o[o.size() - 1 - i] * w); }
            return o; };
        Buf y; const size_t cap = cx.maxLen;
        if (before)
        {
            double total = 0; for (double d : sep) total += d;
            double t = 0;
            for (int k = E - 1; k >= 0; --k)
            {   // farthest (most shrunk, at PULSE 2) first, accelerating / growing into the original
                Buf s = splint (shr[(size_t) k]); addAt (y, s, 0, s.size(), jr (t * sr), 1, cap);
                t += sep[(size_t) k] * (1 + (cx.rand() * 2 - 1) * p["rand"] * 0.5);
            }
            const long long at = jr (std::max (t, total * 0.5) * sr);
            addAt (y, x, ga, x.size(), at, 1, cap);
        }
        else
        {
            app (y, x, 0, ga + glen, cap); double t = (double) (ga + glen) / sr;
            for (int k = 0; k < E; ++k)
            {
                t += sep[(size_t) k] * (1 + (cx.rand() * 2 - 1) * p["rand"] * 0.5) - (k == 0 ? (double) glen / sr : 0);
                Buf s = splint (shr[(size_t) k]); addAt (y, s, 0, s.size(), jr (std::max (0.0, t) * sr), 1, cap);
            }
        }
        return y;
    });
}
} // namespace

void addCdpWaveset (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CAT = "CDP \xc2\xb7 WAVESET \xc2\xb7 PSOLA";
    auto A = [&] (std::string id, std::string name, std::string tag, std::string desc, std::vector<ParamDef> ps, ProcFn fn, std::string cdp)
    { defs::add (v, CAT, std::move (id), std::move (name), std::move (tag), std::move (desc), std::move (ps), fn, {}, false, std::move (cdp)); };

    A ("cdp_distort_reform", "WAVESET REFORM", "WREF", "Replaces every half wave cycle by another shape of the same length and peak: SQUARE, TRIANGLE (FIXED = full level), INVERTED halves, CLICK bursts or SINE. From fuzz to buzzy chiptune tones.",
       { Opt ("mode", "MODE", { "FIXED SQUARE", "SQUARE", "FIXED TRI", "TRIANGLE", "INVERT HALF", "CLICKS", "SINE" }, 1), Seed() }, reform, "distort reform 1-7");
    A ("cdp_distort_exaggerate", "WAVESET EXAGGERATE", "WEXG", "Exaggerates the contour inside every half cycle: everything below the local peaks is pushed down by EXAGGERATION (above 1 spiky and bright, below 1 rounder and fuller).",
       { P ("exag", "EXAGGERATE", 0.1, 10, 3, PExp) }, exagg, "distort reform 8");
    A ("cdp_distort_envel", "WAVESET ENVELOPE", "WENV", "Imposes an envelope on every group of CYCLES wave cycles: RISING, FALLING, TROUGHED or a drawn SHAPE. TROUGH lifts the lowest point, EXPONENT bends the curve. Buzzing tremolo at audio rate.",
       { Opt ("mode", "MODE", { "RISING", "FALLING", "TROUGHED", "SHAPE" }, 1), P ("cycles", "CYCLES", 1, 64, 6, PInt), Pct ("trough", "TROUGH", 0), P ("expon", "EXPONENT", 0.25, 4, 1, PExp),
         Opt ("shape", "SHAPE", { "ARCH", "SPIKE", "DOUBLE", "STEPS" }) }, envel, "distort envel 1-4");
    A ("cdp_distort_average", "WAVESET AVERAGE", "WAVE", "Every group of CYCLES wave cycles is replaced by their average waveshape, repeated: smoother, more synthetic tone with stepped pitch. Cycles longer than MAX WAVE pass unchanged.",
       { P ("cycles", "CYCLES", 2, 64, 6, PInt), P ("maxwav", "MAX WAVE", 0.005, 0.5, 0.05, PExp, {}, Fmt::time) }, average, "distort average");
    A ("cdp_distort_omit", "WAVESET OMIT", "WOMT", "Silences A out of every B wave cycles: from a gritty grain to a broken, gated buzz.",
       { P ("a", "OMIT A", 1, 63, 2, PInt), P ("b", "OF B", 2, 64, 3, PInt) }, omit, "distort omit");
    A ("cdp_distort_multiply", "WAVESET MULTIPLY", "WMUL", "Squeezes N copies of every half cycle into its own length: the waveform frequency is multiplied, harsh octave-up and harmonic distortion. SMOOTH interpolates.",
       { P ("n", "MULTIPLY", 2, 16, 2, PInt), Opt ("smooth", "SMOOTH", { "OFF", "ON" }) }, multiply, "distort multiply");
    A ("cdp_distort_divide", "WAVESET DIVIDE", "WDIV", "Every N half cycles are replaced by the first one stretched over all of them: rough sub-octaves and growling undertones. INTERP gives a cleaner result.",
       { P ("n", "DIVIDE", 2, 16, 2, PInt), Opt ("interp", "INTERP", { "OFF", "ON" }, 1) }, divide, "distort divide");
    A ("cdp_distort_harmonic", "WAVESET HARMONICS", "WHRM", "Adds 'harmonics' to every wave cycle: copies read 2, 3, 4, 5, 6 or 8 times faster at the given LEVELS. PRE-ATTEN scales the source first.",
       { Pct ("h2", "HARM 2", 0.5), Pct ("h3", "HARM 3", 0.35), Pct ("h4", "HARM 4", 0), Pct ("h5", "HARM 5", 0.25), Pct ("h6", "HARM 6", 0), Pct ("h8", "HARM 8", 0.15), Pct ("pre", "PRE-ATTEN", 1) },
       harmonic, "distort harmonic");
    A ("cdp_distort_fractal", "WAVESET FRACTAL", "WFRC", "Squeezes every group of SCALE wave cycles into one miniature and adds it onto each cycle of the group at LOUDNESS: self-similar, glassy distortion.",
       { P ("scale", "SCALE", 2, 32, 4, PInt), Pct ("loud", "LOUDNESS", 0.5), Pct ("pre", "PRE-ATTEN", 1) }, fractal, "distort fractal");
    A ("cdp_distort_reverse", "WAVESET REVERSE", "WREV", "Reverses wave cycles in groups of CYCLES: subtle phasey grit for small groups, granular backwards textures for large ones.",
       { P ("cycles", "CYCLES", 1, 256, 8, PExp | PInt) }, wreverse, "distort reverse");
    A ("cdp_distort_shuffle", "WAVESET SHUFFLE", "WSHF", "Re-orders groups of CYCLES wave cycles by a domain-image PATTERN (abc-cba ...): groups can be swapped, dropped or doubled.",
       { Opt ("pattern", "PATTERN", { "ab-ba", "abc-cba", "abc-bca", "abcd-dcba", "abcd-badc", "abcd-cdab", "abc-aabbcc", "ab-aab", "abcd-abbd", "abcd-aadd", "abcdef-fbdcea" }, 1),
         P ("cycles", "CYCLES", 1, 64, 4, PInt) }, shuffle, "distort shuffle");
    A ("cdp_distort_repeat", "WAVESET REPEAT", "WRPT", "Time-stretches by playing every group of CYCLES wave cycles MULTIPLY times: buzzy, stepped, robotic stretching.",
       { P ("mult", "MULTIPLY", 2, 32, 3, PInt), P ("cycles", "CYCLES", 1, 64, 1, PInt) }, repeat, "distort repeat");
    A ("cdp_distort_repeat2", "WAVESET REPEAT SKIP", "WRP2", "Plays every group of CYCLES wave cycles MULTIPLY times, then skips ahead so the length stays the same: a jittery, sample-and-hold version of the sound.",
       { P ("mult", "MULTIPLY", 2, 32, 3, PInt), P ("cycles", "CYCLES", 1, 64, 2, PInt) }, repeat2, "distort repeat2");
    A ("cdp_distort_replim", "WAVESET REPEAT LIM", "WRPL", "Like WAVESET REPEAT, but cycles faster than LIMIT are merged with their neighbours, so noise and hiss are repeated in larger chunks.",
       { P ("mult", "MULTIPLY", 2, 32, 3, PInt), P ("cycles", "CYCLES", 1, 64, 1, PInt), P ("hilim", "LIMIT", 20, 10000, 800, PExp, {}, Fmt::hz) }, replim, "distort replim");
    A ("cdp_distort_interpolate", "WAVESET INTERPOLATE", "WINT", "Time-stretches by MULTIPLY: between every two wave cycles, new cycles morph in length and shape from one to the next. Smoother than repeating.",
       { P ("mult", "MULTIPLY", 2, 32, 3, PInt) }, interp, "distort interpolate");
    A ("cdp_distort_delete", "WAVESET DELETE", "WDEL", "Time-contracts the sound: keeps 1 in every CYCLES wave cycles (KEEP 1), only the STRONGEST of each group, or deletes only the WEAKEST. Pitch stays, timbre degrades.",
       { Opt ("mode", "MODE", { "KEEP 1 IN N", "KEEP STRONGEST", "DELETE WEAKEST" }), P ("cycles", "CYCLES", 2, 64, 2, PInt) }, wdelete, "distort delete 1-3");
    A ("cdp_distort_replace", "WAVESET REPLACE", "WRPC", "In every group of CYCLES wave cycles the strongest cycle replaces all the others: hard, stepped, synthetic tone.",
       { P ("cycles", "CYCLES", 2, 64, 6, PInt) }, wreplace, "distort replace");
    A ("cdp_distort_telescope", "WAVESET TELESCOPE", "WTEL", "Time-contracts by averaging every CYCLES wave cycles into one, as long as the LONGEST of them or their AVERAGE length.",
       { Opt ("mode", "LENGTH", { "LONGEST", "AVERAGE" }), P ("cycles", "CYCLES", 2, 64, 3, PInt) }, telescope, "distort telescope");
    A ("cdp_distort_filter", "WAVESET FILTER", "WFLT", "Drops wave cycles whose frequency is BELOW or ABOVE FREQ, or OUTSIDE FREQ..FREQ 2: the sound contracts and keeps only its high or low wave cycles.",
       { Opt ("mode", "MODE", { "OMIT BELOW", "OMIT ABOVE", "OMIT OUTSIDE" }), P ("freq", "FREQ", 20, 10000, 150, PExp, {}, Fmt::hz), P ("freq2", "FREQ 2", 20, 10000, 3000, PExp, {}, Fmt::hz) },
       wfilter, "distort filter 1-3");
    A ("cdp_distort_pitch", "WAVESET PITCH WARP", "WPCH", "Every wave cycle is resampled by a random transposition (up to RANGE octaves) that glides to a new target every random number of cycles (up to CYCLES).",
       { P ("octvary", "RANGE", 0.01, 8, 0.5, PExp, "oct"), P ("cycles", "CYCLES", 1, 256, 64, PExp | PInt), Seed() }, wpitch, "distort pitch");
    A ("cdp_distort_overload", "OVERLOAD", "OVLD", "Clips the signal at LEVEL and fills the clipped stretches with NOISE or a WAVE of FREQ, DEPTH deep: crude fuzz with buzzing or hissing tops.",
       { Opt ("mode", "MODE", { "NOISE", "WAVE" }), Pct ("gate", "LEVEL", 0.2), Pct ("depth", "DEPTH", 0.6), P ("freq", "FREQ", 20, 10000, 800, PExp, {}, Fmt::hz), Seed() }, overload, "distort overload 1-2");
    A ("cdp_distort_pulsed", "PULSE TRAIN", "PULS", "From POSITION, for DURATION: an impulse train of FREQ with a SHAPE envelope on the source (IMPULSE), or a loop of grabbed wavesets (GRAB ms or GRAB count) inside it, gliding by TRANSP. RAND controls vary freq, timing, level, pitch.",
       { Opt ("mode", "MODE", { "IMPULSE", "WAVESET MS", "WAVESET N" }), Opt ("shape", "SHAPE", { "PLUCK", "ARCH", "SWELL", "DOUBLE" }), Pct ("pos", "POSITION", 0.1), P ("dur", "DURATION", 0.1, 30, 3, PExp, {}, Fmt::time),
         P ("frq", "FREQ", 0.1, 50, 8, PExp, {}, Fmt::hz), P ("frand", "FREQ RAND", 0, 12, 1, 0, "st"), Pct ("trand", "TIME RAND", 0.3), Pct ("arand", "LEVEL RAND", 0.3), P ("grab", "GRAB", 1, 200, 20, PExp),
         P ("transp", "TRANSP", -24, 24, -5, 0, "st"), Pct ("tranrand", "TRANSP RND", 0.3), Seed() }, pulsed, "distort pulsed 1-3");
    A ("cdp_sfedit_noisecut", "NOISE CUT", "NCUT", "Finds noisy stretches (wave cycles faster than NOISE FRQ) and silences them, or keeps only them: removes or isolates sibilants, breath and hats. MIN NOISE / MIN TONE ignore short bits; SPLICE smooths the cuts.",
       { Opt ("mode", "MODE", { "CUT NOISE", "KEEP NOISE" }), P ("noisefrq", "NOISE FRQ", 1000, 20000, 6000, PExp, {}, Fmt::hz), P ("minnoise", "MIN NOISE", 1, 500, 30, PExp, "ms"),
         P ("mintone", "MIN TONE", 1, 500, 30, PExp, "ms"), P ("splice", "SPLICE", 1, 100, 10, PExp, "ms") }, noisecut, "sfedit noisecut");

    // ---- pitch-synchronous grains (FOFs) ----
    A ("cdp_fofex_construct", "FOF CONSTRUCT", "FOFC", "Rebuilds the sound from its own pitch-synchronous grains (FOFs), sorted by pitch: ONE FOF (F1), the sum of ALL / LOW / MID / HIGH FOFs, or 2-3 FOFs blended by pitch (MIN-MAX F) and level, following the TRACKED pitch (+TRANSP) or a FIXED note.",
       { Opt ("mode", "MODE", { "ONE FOF", "ALL FOFS", "LOW FOFS", "MID FOFS", "HIGH FOFS", "2 BY PITCH", "3 BY P+LEVEL" }, 1), Opt ("pitch", "PITCH", { "TRACKED", "FIXED" }),
         P ("transp", "TRANSP", -24, 24, 0, 0, "st"), P ("note", "NOTE", 24, 84, 45, PInt, {}, Fmt::note), Pct ("f1", "FOF 1", 0.2), Pct ("f2", "FOF 2", 0.8), Pct ("f3", "FOF 3", 0.5),
         P ("minf", "MIN FREQ", 50, 2000, 100, PExp, {}, Fmt::hz), P ("maxf", "MAX FREQ", 50, 2000, 400, PExp, {}, Fmt::hz), Pct ("minl", "MIN LEVEL", 0.1), Pct ("maxl", "MAX LEVEL", 0.9),
         Opt ("norm", "EQUAL LVL", { "OFF", "ON" }) }, fofCons, "fofex construct 1-7");
    A ("cdp_psow_stretch", "GRAIN STRETCH", "PSTR", "Time-stretches a voice by moving its pitch-synchronous grains (chunks of SEGS) apart by STRETCH without stretching them: gaps, stutter and buzz above 1, overlapping density below.",
       { P ("stretch", "STRETCH", 0.1, 10, 2, PExp, "x"), P ("segs", "SEGS", 1, 64, 1, PInt) }, psStretch, "psow stretch");
    A ("cdp_psow_dupl", "GRAIN DUPLICATE", "PDUP", "Time-stretches by playing every chunk of SEGS pitch-synchronous grains REPEATS times: the pitch stays, the sound gets longer and more mechanical.",
       { P ("reps", "REPEATS", 2, 16, 2, PInt), P ("segs", "SEGS", 1, 64, 2, PInt) }, psDupl, "psow dupl");
    A ("cdp_psow_delete", "GRAIN DELETE", "PDEL", "Time-shrinks by keeping only one chunk of SEGS pitch-synchronous grains in every KEEP 1 IN: faster, the pitch stays.",
       { P ("keep", "KEEP 1 IN", 2, 20, 2, PInt), P ("segs", "SEGS", 1, 64, 1, PInt) }, psDelete, "psow delete");
    A ("cdp_psow_strtrans", "GRAIN STRETCH TRANSP", "PSTT", "PSOLA: pitch-synchronous grains are moved apart by STRETCH and overlapped more or less densely so the pitch moves by TRANSPOSE while the formants stay.",
       { P ("stretch", "STRETCH", 0.25, 10, 1, PExp, "x"), P ("segs", "SEGS", 1, 64, 1, PInt), P ("trans", "TRANSPOSE", -24, 24, 7, 0, "st") }, psStrTrans, "psow strtrans");
    A ("cdp_psow_grab", "GRAIN GRAB", "PGRB", "Grabs SEGS pitch-synchronous grains at POSITION and builds a drone of DURATION from them: DENSITY copies per grain length, SPEC TRANSP shifts the formants, RANDOM loosens the timing.",
       { Pct ("pos", "POSITION", 0.3), P ("dur", "DURATION", 0.2, 30, 4, PExp, {}, Fmt::time), P ("segs", "SEGS", 1, 64, 2, PInt), P ("spec", "SPEC TRANSP", -24, 24, 0, 0, "st"),
         P ("dens", "DENSITY", 0.25, 8, 1.5, PExp, "x"), Pct ("rand", "RANDOM", 0.2), Seed() }, psGrab, "psow grab");
    A ("cdp_psow_interp", "GRAIN INTERPOLATE", "PINT", "Turns two single grains (at POS A and POS B) into a sung tone: grain A for START, morphing for INTERP, grain B for END, with VIBRATO and TREMOLO.",
       { Pct ("posa", "POS A", 0.2), Pct ("posb", "POS B", 0.7), P ("sdur", "START", 0, 10, 1, 0, {}, Fmt::time), P ("idur", "INTERP", 0, 20, 2, 0, {}, Fmt::time), P ("edur", "END", 0, 10, 1, 0, {}, Fmt::time),
         P ("vibfrq", "VIB RATE", 0, 20, 5.5, 0, {}, Fmt::hz), P ("vibdep", "VIB DEPTH", 0, 3, 0.4, 0, "st"), P ("tremfrq", "TREM RATE", 0, 30, 4, 0, {}, Fmt::hz), P ("tremdep", "TREM DEPTH", 0, 2, 0.2) }, psInterp, "psow interp");
    A ("cdp_psow_features", "VOICE FEATURES", "PFTR", "Re-sings a voice grain by grain: TRANSPOSE (TIMEWARP changes the length, KEEP TIME repeats grains), VIBRATO, SPEC TRANSP formant shift, HOARSE timing jitter, SUBHARM undertone, FOF STRETCH.",
       { Opt ("mode", "MODE", { "TIMEWARP", "KEEP TIME" }), P ("segs", "SEGS", 1, 16, 1, PInt), P ("trans", "TRANSPOSE", -24, 24, 0, 0, "st"), P ("vibfrq", "VIB RATE", 0, 20, 5.5, 0, {}, Fmt::hz),
         P ("vibdep", "VIB DEPTH", 0, 3, 0.6, 0, "st"), P ("spec", "SPEC TRANSP", -24, 24, 0, 0, "st"), Pct ("hoarse", "HOARSE", 0.15), P ("sub", "SUBHARM", 0, 8, 2, PInt), Pct ("subgain", "SUB LEVEL", 0.5),
         P ("fofstr", "FOF STRETCH", 1, 64, 1, PExp | PInt), Seed() }, psFeatures, "psow features 1-2");
    A ("cdp_psow_synth", "GRAIN SYNTH", "PSYN", "Imposes each grain's envelope on a synthetic tone: a CHORD on ROOT, the HARMONIC series, a chord GLIDING by GLIDE, a chord PROGRESSION, or NOISE. The voice's rhythm and formants sing the new pitch; DEPTH mixes in the raw tone.",
       { Opt ("mode", "MODE", { "CHORD", "HARMONICS", "GLIDE", "PROGRESSION", "NOISE" }), Opt ("chord", "CHORD", { "MAJOR", "MINOR", "MAJ7", "MIN7", "OPEN 5THS", "SUS", "PENTA" }),
         P ("root", "ROOT", 24, 84, 48, PInt, {}, Fmt::note), P ("glide", "GLIDE", -24, 24, 12, 0, "st"), Pct ("depth", "DEPTH", 1), Seed() }, psSynth, "psow synth 1-5");
    A ("cdp_psow_split", "GRAIN SPLIT", "PSPL", "Splits a voice into a subharmonic (groups of SUBHARM grains repeat at their own length) and an up-transposed copy (grains packed closer by UP TRANSP). BALANCE below 1 quietens the high part, above 1 the subharmonic.",
       { P ("sub", "SUBHARM", 2, 8, 3, PInt), P ("up", "UP TRANSP", 0, 48, 12, 0, "st"), P ("bal", "BALANCE", 0, 8, 1) }, psSplit, "psow split");
    A ("cdp_psow_space", "GRAIN SPACE", "PSPC", "Sends successive pitch-synchronous grains to SUBHARM different stereo positions SEPARATION apart: the voice divides into a spatial subharmonic. L/R BAL tips the balance, HI SUPPRESS fades the later grains.",
       { P ("sub", "SUBHARM", 2, 5, 2, PInt), P ("separ", "SEPARATION", -1, 1, 0.9), P ("relev", "L/R BAL", 0.01, 100, 1, PExp, "x"), Pct ("hisup", "HI SUPPRESS", 0) }, psSpace, "psow space");
    A ("cdp_psow_sustain", "GRAIN SUSTAIN", "PSUS", "Plays up to POSITION, holds the SEGS grains found there for SUSTAIN seconds (stepping by TRANSP, with VIBRATO; SMOOTH windows the grain), then plays the rest: a sung fermata.",
       { Pct ("pos", "POSITION", 0.3), P ("dur", "SUSTAIN", 0.1, 30, 3, PExp, {}, Fmt::time), P ("segs", "SEGS", 1, 32, 1, PInt), P ("vibfrq", "VIB RATE", 0, 20, 5.5, 0, {}, Fmt::hz),
         P ("vibdep", "VIB DEPTH", 0, 3, 0.4, 0, "st"), P ("trans", "TRANSP", -24, 24, 0, 0, "st"), Opt ("smooth", "SMOOTH", { "OFF", "ON" }) }, psSustain, "psow sustain");
    A ("cdp_psow_sustain2", "GRAIN SUSTAIN 2", "PSU2", "Holds one hand-cut grain (POSITION, LENGTH, snapped to zero crossings and moved by NUDGE crossings) for SUSTAIN seconds with VIBRATO, then plays on. No pitch tracking needed.",
       { Pct ("pos", "POSITION", 0.3), P ("len", "LENGTH", 1, 100, 8, PExp, "ms"), P ("dur", "SUSTAIN", 0.1, 30, 3, PExp, {}, Fmt::time), P ("vibfrq", "VIB RATE", 0, 20, 5.5, 0, {}, Fmt::hz),
         P ("vibdep", "VIB DEPTH", 0, 3, 0.3, 0, "st"), P ("nudge", "NUDGE", -24, 24, 0, PInt) }, psSustain2, "psow sustain2");
    A ("cdp_psow_reinforce1", "GRAIN HARMONICS", "PRH1", "Reinforces harmonics 2-8 by adding copies of every pitch-synchronous grain inside its own period at the given LEVELS. DELAY offsets each added harmonic, SKIP SHARED drops coinciding copies.",
       { P ("h2", "HARM 2", 0, 2, 0.6), P ("h3", "HARM 3", 0, 2, 0.4), P ("h4", "HARM 4", 0, 2, 0), P ("h5", "HARM 5", 0, 2, 0.3), P ("h6", "HARM 6", 0, 2, 0), P ("h8", "HARM 8", 0, 2, 0),
         P ("delay", "DELAY", 0, 50, 0, 0, "ms"), Opt ("skip", "SKIP SHARED", { "OFF", "ON" }) }, psReinforce, "psow reinforce 1");
    A ("cdp_psow_reinforce2", "GRAIN INHARMONIC", "PRH2", "Adds inharmonic partials: every grain is copied at its period divided by PARTIAL 1-3 (fractional ratios) at LEVEL 1-3, ringing on for WEIGHT periods. Bells and metallic voices.",
       { P ("p1", "PARTIAL 1", 1.01, 16, 2.31, PExp, "x"), Pct ("l1", "LEVEL 1", 0.5), P ("p2", "PARTIAL 2", 1.01, 16, 3.73, PExp, "x"), Pct ("l2", "LEVEL 2", 0.35),
         P ("p3", "PARTIAL 3", 1.01, 16, 5.42, PExp, "x"), Pct ("l3", "LEVEL 3", 0.2), P ("weight", "WEIGHT", 1, 16, 4, PExp) }, psReinforce2, "psow reinforce 2");
    A ("cdp_tweet_tweet", "TWEET", "TWET", "Replaces every vocal grain by a synthetic chirp of PEAKS cycles (PEAKS), a chirp at FREQ (FREQ) or NOISE, shaped by the grain's formant envelope: speech turns into birdsong. CHIRP bends the glide; MIN LEVEL drops quiet grains.",
       { Opt ("mode", "MODE", { "PEAKS", "FREQ", "NOISE" }), P ("minlev", "MIN LEVEL", -96, 0, -40, 0, {}, Fmt::db), P ("pkcnt", "PEAKS", 1, 200, 6, PExp | PInt), P ("frq", "FREQ", 100, 10000, 2000, PExp, {}, Fmt::hz),
         P ("chirp", "CHIRP", 0.05, 30, 2, PExp), Opt ("win", "WINDOW", { "OFF", "ON" }, 1), Seed() }, tweet, "tweet tweet 1-3");

    // ---- Release 7-8 waveset programs ----
    A ("cdp_clip_clip1", "CLIP", "CLP1", "Hard-clips the signal at LEVEL and brings it back up to the original peak: flat-topped, buzzy distortion.",
       { P ("level", "LEVEL", 0.01, 1, 0.15, PExp) }, clip1, "clip clip 1");
    A ("cdp_clip_clip2", "HALF-WAVE CLIP", "CLP2", "Clips every half wave cycle at FRACTION of its own peak: quiet and loud passages are squared off equally, a level-independent fuzz.",
       { Pct ("frac", "FRACTION", 0.3) }, clip2, "clip clip 2");
    A ("cdp_quirk_quirk", "QUIRK", "QRK", "Raises every sample to POWER, scaled to each HALF CYCLE's own peak or to the WHOLE sound's peak: above 1 thin and spiky, below 1 fat and square.",
       { Opt ("mode", "RANGE", { "HALF CYCLE", "WHOLE" }), P ("pow", "POWER", 0.01, 100, 3, PExp) }, quirk, "quirk quirk 1-2");
    A ("cdp_distshift_distshift", "HALF-CYCLE SHIFT", "DSHF", "SHIFT replaces alternate groups of half cycles (GROUP 1 = one half cycle) by the group SHIFT groups earlier; SWAP trades alternate groups around. Ragged, phasey waveform scrambling.",
       { Opt ("mode", "MODE", { "SHIFT", "SWAP" }), P ("grp", "GROUP", 1, 32, 1, PInt), P ("shift", "SHIFT", 1, 32, 1, PInt) }, distshift, "distshift distshift 1-2");
    A ("cdp_distwarp_distwarp", "PROGRESSIVE FOLD", "DWRP", "Adds an offset that grows by WARP every sample (PER SAMPLE) or every CYCLES wavesets (PER WAVESET) and folds the result back into range, following the source's envelope: ever-changing digital wavefolding.",
       { Opt ("mode", "MODE", { "PER SAMPLE", "PER WAVESET" }, 1), P ("warp", "WARP", 0.0001, 0.1, 0.01, PExp), P ("cycles", "CYCLES", 1, 64, 2, PInt) }, distwarp, "distwarp distwarp 1-2");
    A ("cdp_distrep_distrep", "WAVESET REPEAT SPLICE", "DREP", "Plays every group of CYCLES wavesets MULTIPLY times as one smoothly spliced block. STRETCH makes it longer, SKIP jumps ahead to keep the length.",
       { Opt ("mode", "MODE", { "STRETCH", "SKIP" }), P ("mult", "MULTIPLY", 2, 32, 3, PInt), P ("cycles", "CYCLES", 1, 256, 8, PExp | PInt), P ("splice", "SPLICE", 0.5, 50, 3, PExp, "ms") },
       distrep, "distrep distrep 1-2");
    A ("cdp_distortt_repeat", "WAVESET REPEAT DUR", "DRPT", "Plays the first OFFSET unchanged, then repeats every group of CYCLES wavesets REPEATS times until DURATION. TELESCOPE skips ahead so the source passes at its own pace.",
       { P ("cycles", "CYCLES", 1, 256, 4, PExp | PInt), P ("reps", "REPEATS", 1, 64, 4, PInt), P ("offset", "OFFSET", 0, 5000, 0, 0, "ms"), P ("dur", "DURATION", 0.1, 60, 6, PExp, {}, Fmt::time),
         Opt ("tele", "TELESCOPE", { "OFF", "ON" }) }, distortt, "distortt repeat");
    A ("cdp_distmore_double", "WAVESET OCTAVER", "DDBL", "Every waveset is squeezed and repeated to fill its own length: the sound jumps up 1-4 OCTAVES with the same timing. Gritty digital octaver.",
       { P ("oct", "OCTAVES", 1, 4, 1, PInt) }, wdouble, "distmore double");
    A ("cdp_distmore_bright", "SORT BY BRIGHTNESS", "DBRT", "Cuts the sound into Heads (onset + HEAD) and Tails and re-orders them by brightness (zero-crossing rate), INCREASING or DECREASING: heads + bits of tails, heads and tails, or tails only.",
       { Opt ("mode", "MODE", { "HEADS+BITS", "HEADS+TAILS", "TAILS ONLY" }), Opt ("order", "ORDER", { "INCREASING", "DECREASING" }), P ("head", "HEAD", 5, 500, 60, PExp, "ms"),
         P ("splice", "SPLICE", 1, 50, 15, PExp, "ms") }, bright, "distmore bright 1-3");
    A ("cdp_distmore_segsbkwd", "SEGMENTS BACKWARDS", "DSBK", "Splits the sound into Heads (onset + HEAD) and Tails and reverses some of them: TAILS, HEADS, H+T pairs, or larger sets (odd modes keep each head forward).",
       { Opt ("mode", "MODE", { "TAILS", "HEADS", "H+T PAIRS", "H & T+H+T", "HTHT", "H & THTHT", "HTHTHT", "H & THTHTHT", "HTHTHTHT" }), P ("head", "HEAD", 5, 500, 60, PExp, "ms") },
       segsbkwd, "distmore segsbkwd 1-9");
    A ("cdp_distmore_segszig", "SEGMENT ZIGZAG", "DSZG", "Zigzags back and forth REPEATS times over every Tail (after onset + HEAD) or over the WHOLE sound (FILL DUR: until DURATION), the zigs shrinking toward SHRINK TO (LOG shrink). PROPORTION uses part of each tail.",
       { Opt ("mode", "MODE", { "TAILS", "WHOLE", "FILL DUR" }), P ("reps", "REPEATS", 1, 32, 3, PInt), P ("shrink", "SHRINK TO", 0, 500, 40, 0, "ms"), Pct ("prop", "PROPORTION", 1),
         Opt ("log", "LOG", { "OFF", "ON" }), P ("head", "HEAD", 5, 500, 60, PExp, "ms"), P ("dur", "DURATION", 0.5, 60, 8, PExp, {}, Fmt::time) }, segszig, "distmore segszig 1-3");
    A ("cdp_distmark_distmark", "WAVESET MORPH MARKS", "DMRK", "Takes waveset groups of about UNIT ms at COUNT marks (EVEN or at ONSETS) and morphs shape and length from each to the next over the gap times STRETCH. RAND shortens, FLIP inverts alternate copies, ALTERNATE morphs every other block.",
       { Opt ("mode", "MODE", { "ALL BLOCKS", "ALTERNATE" }), Opt ("marks", "MARKS", { "EVEN", "ONSETS" }), P ("count", "COUNT", 2, 64, 8, PInt), P ("unit", "UNIT", 1, 200, 15, PExp, "ms"),
         P ("tstretch", "STRETCH", 0.25, 8, 1, PExp, "x"), Pct ("rand", "RAND", 0), Opt ("flip", "FLIP", { "OFF", "ON" }), Opt ("tail", "TAIL", { "OFF", "ON" }, 1), Seed() }, distmark, "distmark distmark 1-2");
    A ("cdp_fractal_wave1", "FRACTAL TRANSPOSE", "FRCW", "A four-STEP transposition pattern over PATTERN seconds is applied to the sound, then again inside every step, LEVELS deep: fractal pitch and time warping. STRETCH, WARP and SHRINK reshape it.",
       { P ("s1", "STEP 1", -12, 12, 0, 0, "st"), P ("s2", "STEP 2", -12, 12, 7, 0, "st"), P ("s3", "STEP 3", -12, 12, -5, 0, "st"), P ("s4", "STEP 4", -12, 12, 3, 0, "st"),
         P ("pdur", "PATTERN", 0.05, 10, 1, PExp, {}, Fmt::time), P ("levels", "LEVELS", 1, 6, 3, PInt), P ("tstr", "STRETCH", 0, 8, 0), P ("warp", "WARP", 0, 4, 1), Opt ("shrink", "SHRINK", { "OFF", "ON" }) },
       fractalWave, "fractal wave 1");
    A ("cdp_scramble_scramble", "WAVESET SCRAMBLE", "SCRM", "Re-assembles the wavesets (GROUP at a time) in RANDOM or PERMUTED order for DURATION, or sorted by SIZE (pitch) or LEVEL: rising, falling or arching, over the whole sound or per onset segment. TRANSPOSE and ATTEN add random variation.",
       { Opt ("mode", "MODE", { "RANDOM", "PERMUTED", "SIZE UP", "SIZE DOWN", "SIZE UP/SEG", "SIZE DN/SEG", "SIZE ARCH/SEG", "SIZE DIP/SEG", "LEVEL UP", "LEVEL DOWN", "LEVEL UP/SEG", "LEVEL DN/SEG",
                                  "LEVEL ARCH/SEG", "LEVEL DIP/SEG" }, 3),
         P ("dur", "DURATION", 0.1, 60, 4, PExp, {}, Fmt::time), P ("cnt", "GROUP", 1, 256, 2, PExp | PInt), P ("trns", "TRANSPOSE", 0, 12, 0, 0, "st"), Pct ("atten", "ATTEN", 0), Seed() }, scramble, "scramble scramble 1-14");
    A ("cdp_splinter_splinter", "SPLINTER", "SPLN", "The CYCLES wavesets at POSITION repeat as pulses that shrink over SHRINK CNT pulses plus EXTRA (PITCH: rising toward 1000/MIN SIZE Hz; LENGTH: down to MIN SIZE ms), pulse rate going PULSE 1 to PULSE 2, leading INTO or emerging FROM the original.",
       { Opt ("mode", "MODE", { "PITCH INTO", "PITCH FROM", "LENGTH INTO", "LENGTH FROM" }), Pct ("pos", "POSITION", 0.3), P ("wcnt", "CYCLES", 1, 64, 4, PInt), P ("shrcnt", "SHRINK CNT", 1, 64, 12, PInt),
         P ("ocnt", "EXTRA", 0, 64, 6, PInt), P ("p1", "PULSE 1", 0, 50, 6, 0, {}, Fmt::hz), P ("p2", "PULSE 2", 0, 100, 30, 0, {}, Fmt::hz), P ("scv", "SHRINK CRV", 0.1, 10, 1, PExp),
         P ("pcv", "PULSE CRV", 0.1, 10, 1, PExp), P ("minsz", "MIN SIZE", 0.1, 100, 0.5, PExp, "ms"), Pct ("rand", "RAND", 0.1), Seed() }, splinter, "splinter splinter 1-4");
}
} // namespace lab
