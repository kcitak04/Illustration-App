// DATLAB DSP core: helpers shared by every Sample Lab process.
// Ported 1:1 from DATTRACK's Sample Lab worker (same FFT, windows, random generator and rounding),
// so a process with the same parameters and seed renders the same result as the web app.
// No JUCE dependency: the DSP compiles on its own for the command-line tests.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace lab
{
using Buf = std::vector<float>;
using Chans = std::vector<Buf>;
using DVec = std::vector<double>;

constexpr double PI = 3.14159265358979323846;
constexpr double TAU = 2.0 * PI;

template <typename T> inline T clamp (T v, T a, T b) { return v < a ? a : v > b ? b : v; }
inline double clampd (double v, double a, double b) { return v < a ? a : v > b ? b : v; }
// JavaScript Math.round: halves round up (towards +infinity)
inline double jsround (double x) { return std::floor (x + 0.5); }
inline long long jr (double x) { return (long long) std::floor (x + 0.5); }

struct Cancelled : std::exception {};
struct LabError : std::runtime_error { using std::runtime_error::runtime_error; };

// ---- xorshift32, identical bit for bit to the JS generator ----
struct Rng
{
    uint32_t s = 1;
    double operator()()
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s / 4294967296.0;
    }
    void seed (double v)
    {
        s = (uint32_t) (int32_t) (long long) v * 2654435761u;
        if (s == 0) s = 1;
        for (int i = 0; i < 8; ++i) (*this)();
    }
};

// Parameter values by id (options and ints are stored as whole numbers)
struct Params
{
    std::unordered_map<std::string, double> v;
    double operator[] (const std::string& k) const { auto it = v.find (k); return it == v.end() ? 0.0 : it->second; }
    int i (const std::string& k) const { return (int) jr ((*this)[k]); }
};

// Everything a process needs besides the audio
struct Ctx
{
    double sr = 44100;
    size_t maxLen = 0;            // results are capped (3 minutes by default)
    const Chans* aux = nullptr;   // second sample (impulse / modulator / mask)
    double bpm = 120;             // tempo for synced processes (host tempo when available)
    Rng rng;
    std::function<void (double)> progressFn;
    double lastProgress = -1;
    void prog (double f)
    {
        if (progressFn && std::abs (f - lastProgress) > 0.004) { lastProgress = f; progressFn (clampd (f, 0, 1)); }
    }
    double rand() { return rng(); }
    void seed (double s) { rng.seed (s); }
};

inline size_t pow2 (double n) { size_t p = 256; while ((double) p < n && p < 65536) p <<= 1; return p; }

// ---- radix-2 FFT ----
struct FFTTable { std::vector<uint32_t> rev; DVec cs, sn; };
inline const FFTTable& fftT (size_t n)
{
    static thread_local std::map<size_t, std::unique_ptr<FFTTable>> cache;
    auto& t = cache[n];
    if (! t)
    {
        t = std::make_unique<FFTTable>();
        int bits = (int) jr (std::log2 ((double) n));
        t->rev.resize (n); t->cs.resize (n >> 1); t->sn.resize (n >> 1);
        for (size_t i = 0; i < n; ++i) { uint32_t r = 0, x = (uint32_t) i; for (int b = 0; b < bits; ++b) { r = (r << 1) | (x & 1); x >>= 1; } t->rev[i] = r; }
        for (size_t i = 0; i < (n >> 1); ++i) { t->cs[i] = std::cos (TAU * (double) i / (double) n); t->sn[i] = std::sin (TAU * (double) i / (double) n); }
    }
    return *t;
}
inline void fft (double* re, double* im, size_t n, bool inv)
{
    const auto& t = fftT (n);
    for (size_t i = 0; i < n; ++i) { size_t j = t.rev[i]; if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); } }
    for (size_t size = 2; size <= n; size <<= 1)
    {
        size_t half = size >> 1, step = n / size;
        for (size_t i = 0; i < n; i += size)
            for (size_t j = 0, k = 0; j < half; ++j, k += step)
            {
                size_t a = i + j, b = a + half;
                double c = t.cs[k], s = inv ? t.sn[k] : -t.sn[k];
                double tr = re[b] * c - im[b] * s, ti = re[b] * s + im[b] * c;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
            }
    }
    if (inv) { double k = 1.0 / (double) n; for (size_t i = 0; i < n; ++i) { re[i] *= k; im[i] *= k; } }
}
inline void fft (DVec& re, DVec& im, bool inv) { fft (re.data(), im.data(), re.size(), inv); }

inline DVec hann (size_t n) { DVec w (n); for (size_t i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos (TAU * (double) i / (double) n); return w; }

inline void analyze (const Buf& x, long long pos, const DVec& w, DVec& re, DVec& im)
{
    const size_t N = w.size(); const long long L = (long long) x.size();
    for (size_t i = 0; i < N; ++i) { long long j = pos + (long long) i; re[i] = j >= 0 && j < L ? x[(size_t) j] * w[i] : 0.0; im[i] = 0; }
    fft (re, im, false);
}
// bins 0..N/2 were edited: rebuild the mirrored half so the inverse FFT is real
inline void mirror (DVec& re, DVec& im)
{
    const size_t N = re.size(); im[0] = 0; im[N >> 1] = 0;
    for (size_t k = 1; k < (N >> 1); ++k) { re[N - k] = re[k]; im[N - k] = -im[k]; }
}
inline void synth (Buf& out, long long pos, const DVec& w, DVec& re, DVec& im, double g)
{
    fft (re, im, true);
    const size_t N = w.size(); const long long L = (long long) out.size();
    for (size_t i = 0; i < N; ++i) { long long j = pos + (long long) i; if (j >= 0 && j < L) out[(size_t) j] = (float) (out[(size_t) j] + re[i] * w[i] * g); }
}
inline Buf convolve (const Buf& x, const Buf& h)   // FFT convolution, output length x + h - 1
{
    size_t n = x.size() + h.size() - 1, N = (size_t) 1 << (size_t) std::ceil (std::log2 ((double) n));
    DVec ar (N), ai (N), br (N), bi (N);
    for (size_t i = 0; i < x.size(); ++i) ar[i] = x[i];
    for (size_t i = 0; i < h.size(); ++i) br[i] = h[i];
    fft (ar, ai, false); fft (br, bi, false);
    for (size_t i = 0; i < N; ++i) { double r = ar[i] * br[i] - ai[i] * bi[i]; ai[i] = ar[i] * bi[i] + ai[i] * br[i]; ar[i] = r; }
    fft (ar, ai, true);
    Buf o (n); for (size_t i = 0; i < n; ++i) o[i] = (float) ar[i];
    return o;
}
inline double peak (const Buf& a) { double p = 0; for (float v : a) p = std::max (p, (double) std::abs (v)); return p; }
inline double peakAll (const Chans& c) { double p = 0; for (auto& a : c) p = std::max (p, peak (a)); return p; }
template <typename V> inline double lerpAt (const V& x, double p)
{
    double fi = std::floor (p); long long i = (long long) fi; double f = p - fi;
    if (i < 0 || i >= (long long) x.size()) return 0;
    double a = x[(size_t) i], b = i + 1 < (long long) x.size() ? x[(size_t) i + 1] : 0.0;
    return a + (b - a) * f;
}
inline void fades (Buf& a, double fi, double fo)
{
    for (size_t i = 0; (double) i < fi && i < a.size(); ++i) a[i] = (float) (a[i] * (i / fi));
    for (size_t i = 0; (double) i < fo && i < a.size(); ++i) a[a.size() - 1 - i] = (float) (a[a.size() - 1 - i] * (i / fo));
}

// Per-channel state handed to an STFT frame callback
struct St
{
    int c = 0; size_t N = 0, H = 0, half = 0; const DVec* w = nullptr;
    bool init = false;
    DVec a, b, d, e, f;                  // process-specific scratch
    DVec re2, im2;
    DVec pvLast, pvAcc, pvNm, pvNf;      // phase-vocoder state
    std::vector<uint8_t> keep;
};
using FrameFn = std::function<void (DVec& mag, DVec& ph, St& st, long long f, long long pos)>;

// generic STFT pass: fn edits magnitude/phase of bins 0..N/2 of each frame
inline Chans stftRun (const Chans& ch, Ctx& cx, double win, size_t outLen, const FrameFn& fn)
{
    const size_t N = pow2 (win * cx.sr), H = N >> 2, half = N >> 1;
    const DVec w = hann (N);
    const long long frames = (long long) std::ceil ((double) outLen / (double) H) + 1;
    DVec re (N), im (N), mag (half + 1), ph (half + 1);
    Chans res;
    for (size_t c = 0; c < ch.size(); ++c)
    {
        const Buf& x = ch[c];
        Buf out (outLen);
        St st; st.c = (int) c; st.N = N; st.H = H; st.half = half; st.w = &w;
        for (long long f = 0; f < frames; ++f)
        {
            long long pos = f * (long long) H - (long long) half;
            analyze (x, pos, w, re, im);
            for (size_t k = 0; k <= half; ++k) { mag[k] = std::hypot (re[k], im[k]); ph[k] = std::atan2 (im[k], re[k]); }
            fn (mag, ph, st, f, pos);
            for (size_t k = 0; k <= half; ++k) { re[k] = mag[k] * std::cos (ph[k]); im[k] = mag[k] * std::sin (ph[k]); }
            mirror (re, im); synth (out, pos, w, re, im, 2.0 / 3.0);
            if (! (f & 15)) cx.prog (((double) c + (double) f / (double) frames) / (double) ch.size());
        }
        res.push_back (std::move (out));
    }
    return res;
}
// smoothed spectral envelope (moving average over +-B bins)
inline DVec& envelope (const DVec& m, double Bd, DVec& out)
{
    const long long n = (long long) m.size(); long long B = std::max (1LL, (long long) Bd);
    DVec ps ((size_t) n + 1);
    for (long long i = 0; i < n; ++i) ps[(size_t) i + 1] = ps[(size_t) i] + m[(size_t) i];
    for (long long i = 0; i < n; ++i)
    {
        long long a = std::max (0LL, i - B), b = std::min (n, i + B + 1);
        out[(size_t) i] = (ps[(size_t) b] - ps[(size_t) a]) / (double) (b - a) + 1e-9;
    }
    return out;
}
// phase vocoder re-mapping: every partial's true frequency (in bins) goes through map()
template <typename Map> inline void pvMap (DVec& mag, DVec& ph, St& st, Map map)
{
    const size_t half = st.half; const double exp = TAU * (double) st.H / (double) st.N;
    if (st.pvLast.empty()) { st.pvLast.assign (half + 1, 0); st.pvAcc.assign (half + 1, 0); st.pvNm.assign (half + 1, 0); st.pvNf.assign (half + 1, 0); }
    std::fill (st.pvNm.begin(), st.pvNm.end(), 0.0); std::fill (st.pvNf.begin(), st.pvNf.end(), 0.0);
    for (size_t k = 0; k <= half; ++k)
    {
        double d = ph[k] - st.pvLast[k] - (double) k * exp; st.pvLast[k] = ph[k]; d -= TAU * jsround (d / TAU);
        double nf = map ((double) k + d / exp); long long j = jr (nf);
        if (j > 0 && j <= (long long) half && mag[k] > st.pvNm[(size_t) j]) { st.pvNm[(size_t) j] = mag[k]; st.pvNf[(size_t) j] = nf; }
    }
    for (size_t k = 0; k <= half; ++k) { st.pvAcc[k] += st.pvNf[k] * exp; mag[k] = st.pvNm[k]; ph[k] = st.pvAcc[k]; }
}
inline std::vector<size_t> peakBins (const DVec& mag, size_t half)
{
    std::vector<size_t> pk;
    for (size_t k = 1; k < half; ++k) if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1]) pk.push_back (k);
    std::stable_sort (pk.begin(), pk.end(), [&] (size_t a, size_t b) { return mag[a] > mag[b]; });
    return pk;
}
// wavesets: spans between upward zero crossings of the mono sum, in groups of G
inline std::vector<std::pair<size_t, size_t>> wsets (const Chans& ch, int G)
{
    const size_t n = ch[0].size(); std::vector<size_t> zc { 0 }; double prev = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double s = 0; for (auto& c : ch) s += c[i];
        if (prev < 0 && s >= 0 && i - zc.back() > 1) zc.push_back (i);
        prev = s;
    }
    std::vector<std::pair<size_t, size_t>> segs;
    G = std::max (1, G);
    for (size_t i = 0; i < zc.size(); i += (size_t) G)
    {
        size_t a = zc[i], b = i + (size_t) G < zc.size() ? zc[i + (size_t) G] : n;
        if (b > a) segs.push_back ({ a, b });
    }
    return segs;
}
inline Buf resamp (const Buf& seg, long long n)
{
    Buf o ((size_t) std::max (1LL, n)); double r = (double) seg.size() / (double) o.size();
    for (size_t i = 0; i < o.size(); ++i) o[i] = (float) lerpAt (seg, (double) i * r);
    return o;
}
inline std::pair<double, double> panG (double pan) { return { std::cos (pan * PI / 2), std::sin (pan * PI / 2) }; }
inline const Buf& chOr (const Chans& ch, size_t i) { return ch.size() > i ? ch[i] : ch[0]; }

// ---- process registry ----
using ProcFn = Chans (*) (const Chans&, Ctx&, const Params&);

enum ParamFlags { PExp = 1, PInt = 2, PPct = 4 };
enum class Fmt { num, pct, time, hz, db, note, unit };
struct ParamDef
{
    std::string id, label; double min, max, def; int flags = 0;
    std::vector<std::string> options; std::string unit; Fmt fmt = Fmt::num;
    bool isOptions() const { return ! options.empty(); }
    bool isInt() const { return (flags & PInt) || isOptions(); }
};
struct ProcDef
{
    std::string id, name, tag, desc, aux, category;
    std::vector<ParamDef> params;
    ProcFn fn = nullptr;
    bool isNew = false;   // added by DATLAB (not in DATTRACK's Sample Lab)
};

const std::vector<ProcDef>& procs();
const ProcDef* findProc (const std::string& id);
const std::vector<std::pair<std::string, std::vector<std::string>>>& categories();
Params defaults (const ProcDef&);
std::string formatValue (const ParamDef&, double v);

// Original 31 processes (LabProcs.cpp) and the DATLAB additions (LabProcsNew.cpp)
namespace procs_orig
{
Chans paul (const Chans&, Ctx&, const Params&);     Chans freeze (const Chans&, Ctx&, const Params&);
Chans blur (const Chans&, Ctx&, const Params&);     Chans shift (const Chans&, Ctx&, const Params&);
Chans robot (const Chans&, Ctx&, const Params&);    Chans grain (const Chans&, Ctx&, const Params&);
Chans stutter (const Chans&, Ctx&, const Params&);  Chans tape (const Chans&, Ctx&, const Params&);
Chans bits (const Chans&, Ctx&, const Params&);     Chans revrev (const Chans&, Ctx&, const Params&);
Chans shuffle (const Chans&, Ctx&, const Params&);  Chans conv (const Chans&, Ctx&, const Params&);
Chans gate (const Chans&, Ctx&, const Params&);     Chans reso (const Chans&, Ctx&, const Params&);
Chans morph (const Chans&, Ctx&, const Params&);    Chans tune (const Chans&, Ctx&, const Params&);
Chans sinv (const Chans&, Ctx&, const Params&);     Chans focus (const Chans&, Ctx&, const Params&);
Chans mask (const Chans&, Ctx&, const Params&);     Chans wdist (const Chans&, Ctx&, const Params&);
Chans wavg (const Chans&, Ctx&, const Params&);     Chans brass (const Chans&, Ctx&, const Params&);
Chans bounce (const Chans&, Ctx&, const Params&);   Chans drunk (const Chans&, Ctx&, const Params&);
Chans iter (const Chans&, Ctx&, const Params&);     Chans zig (const Chans&, Ctx&, const Params&);
Chans tex (const Chans&, Ctx&, const Params&);      Chans seg (const Chans&, Ctx&, const Params&);
Chans fpitch (const Chans&, Ctx&, const Params&);   Chans fwarp (const Chans&, Ctx&, const Params&);
Chans space (const Chans&, Ctx&, const Params&);
}
namespace procs_new
{
Chans sdelay (const Chans&, Ctx&, const Params&);   Chans scram (const Chans&, Ctx&, const Params&);
Chans sarp (const Chans&, Ctx&, const Params&);     Chans harm (const Chans&, Ctx&, const Params&);
Chans shepard (const Chans&, Ctx&, const Params&);  Chans fold (const Chans&, Ctx&, const Params&);
Chans ring (const Chans&, Ctx&, const Params&);     Chans mosh (const Chans&, Ctx&, const Params&);
Chans beatrep (const Chans&, Ctx&, const Params&);  Chans echo (const Chans&, Ctx&, const Params&);
Chans wear (const Chans&, Ctx&, const Params&);     Chans scratch (const Chans&, Ctx&, const Params&);
Chans ramp (const Chans&, Ctx&, const Params&);     Chans tgate (const Chans&, Ctx&, const Params&);
Chans cloud (const Chans&, Ctx&, const Params&);    Chans flange (const Chans&, Ctx&, const Params&);
Chans wobble (const Chans&, Ctx&, const Params&);   Chans vowel (const Chans&, Ctx&, const Params&);
Chans squash (const Chans&, Ctx&, const Params&);   Chans crystal (const Chans&, Ctx&, const Params&);
}

// Runs a process (with the common pre/post SHAPE section) - see LabEngine.h
}
