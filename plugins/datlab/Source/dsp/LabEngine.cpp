#include "LabEngine.h"

namespace lab
{
namespace
{
// 2nd-order Butterworth biquad (RBJ)
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (bool high, double f, double sr)
    {
        double w = TAU * clampd (f, 5, sr * 0.49) / sr, cs = std::cos (w), al = std::sin (w) / (2 * 0.70710678), a0 = 1 + al;
        if (high) { b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; }
        else { b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; }
        a1 = -2 * cs; a2 = 1 - al; b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
    double run (double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
void filter (Buf& x, bool high, double f, double sr)
{
    for (int pass = 0; pass < 2; ++pass) { Biquad q; q.set (high, f, sr); for (auto& v : x) v = (float) q.run (v); }   // 24 dB/oct
}
} // namespace

void applyPre (Chans& ch, double sr, const Shape& s)
{
    (void) sr;
    const size_t len = ch[0].size();
    size_t a = (size_t) clampd (std::floor (s.start * (double) len), 0, (double) len - 1);
    size_t b = (size_t) clampd (std::ceil (s.end * (double) len), (double) a + 1, (double) len);
    const double g = std::pow (10.0, s.inGain / 20);
    for (auto& x : ch)
    {
        Buf y (x.begin() + (long) a, x.begin() + (long) b);
        if (g != 1) for (auto& v : y) v = (float) (v * g);
        if (s.revIn) std::reverse (y.begin(), y.end());
        x = std::move (y);
    }
}

void applyPost (Chans& wet, const Chans& dry, double sr, const Shape& s)
{
    size_t len = 0; for (auto& x : wet) len = std::max (len, x.size());
    // stereo out whenever either side is stereo
    if (wet.size() == 1 && dry.size() > 1) wet.push_back (wet[0]);
    for (auto& x : wet) x.resize (len);
    if (s.mix < 1)
    {
        for (size_t c = 0; c < wet.size(); ++c)
        {
            const Buf& d = chOr (dry, c);
            for (size_t i = 0; i < len; ++i) wet[c][i] = (float) (wet[c][i] * s.mix + (i < d.size() ? d[i] : 0) * (1 - s.mix));
        }
    }
    if (s.revOut) for (auto& x : wet) std::reverse (x.begin(), x.end());
    if (s.lowCut > 20.5) for (auto& x : wet) filter (x, true, s.lowCut, sr);
    if (s.highCut < 19999) for (auto& x : wet) filter (x, false, s.highCut, sr);
    if (wet.size() == 2 && std::abs (s.width - 1) > 1e-3)
        for (size_t i = 0; i < len; ++i)
        {
            double m = (wet[0][i] + wet[1][i]) * 0.5, sd = (wet[0][i] - wet[1][i]) * 0.5 * s.width;
            wet[0][i] = (float) (m + sd); wet[1][i] = (float) (m - sd);
        }
    if (s.trim && len > 0)
    {
        double pk = peakAll (wet), thr = std::max (1e-5, pk * 0.0032);   // -50 dB below the peak
        size_t a = len, b = 0;
        for (auto& x : wet) for (size_t i = 0; i < len; ++i) if (std::abs (x[i]) > thr) { a = std::min (a, i); b = std::max (b, i + 1); }
        if (b > a)
        {
            a = (size_t) std::max (0.0, (double) a - 0.002 * sr);
            for (auto& x : wet) x = Buf (x.begin() + (long) a, x.begin() + (long) b);
            len = b - a;
        }
    }
    if (s.loopXfade > 0 && len > 16)
    {
        size_t n = std::min ((size_t) (s.loopXfade / 1000 * sr), len / 2);
        for (auto& x : wet)
        {
            // the last n samples fade out over the first n (equal power), then the tail is cut
            for (size_t i = 0; i < n; ++i)
            {
                double u = (double) i / (double) n, gi = std::sin (u * PI / 2), go = std::cos (u * PI / 2);
                x[i] = (float) (x[i] * gi + x[len - n + i] * go);
            }
            x.resize (len - n);
        }
        len -= n;
    }
    const double fi = s.fadeIn / 1000 * sr, fo = s.fadeOut / 1000 * sr;
    if (s.loopXfade <= 0) for (auto& x : wet) fades (x, fi, fo);
    for (auto& x : wet) for (auto& v : x) if (! std::isfinite (v)) v = 0;
}

Result run (const Job& job, const std::function<void (double)>& progress)
{
    Result r; r.sr = job.sr;
    const ProcDef* def = findProc (job.procId);
    if (! def) { r.error = "Unknown process"; return r; }
    if (job.src.empty() || job.src[0].size() < 16) { r.error = "No source sample"; return r; }
    try
    {
        Chans src = job.src; applyPre (src, job.sr, job.shape);
        Ctx cx; cx.sr = job.sr; cx.maxLen = (size_t) (std::max (1.0, job.shape.maxLen) * job.sr); cx.bpm = job.bpm;
        cx.aux = job.aux.empty() ? nullptr : &job.aux;
        cx.progressFn = [&] (double f) { if (progress) progress (f * 0.97); };
        Params p = defaults (*def);
        for (auto& [k, v] : job.params.v) p.v[k] = v;
        for (auto& pd : def->params) { double v = clampd (p[pd.id], pd.min, pd.max); if (pd.isInt()) v = jsround (v); p.v[pd.id] = v; }
        Chans out = def->fn (src, cx, p);
        for (auto& a : out) for (auto& v : a) if (! std::isfinite (v)) v = 0;
        applyPost (out, src, job.sr, job.shape);
        if (out.empty() || out[0].empty()) { r.error = "The result is empty: try other settings"; return r; }
        double pk = peakAll (out);
        if (pk < 1e-6) { r.error = "The result is silent: try other settings"; return r; }
        if (job.shape.normalize || pk > 1) { double g = 0.98 / pk; for (auto& a : out) for (auto& v : a) v = (float) (v * g); }
        double og = std::pow (10.0, job.shape.outGain / 20);
        if (og != 1) for (auto& a : out) for (auto& v : a) v = (float) clampd (v * og, -1, 1);
        if (progress) progress (1);
        r.ch = std::move (out);
    }
    catch (const Cancelled&) { r.error = "Cancelled"; }
    catch (const std::exception& e) { r.error = e.what(); }
    return r;
}
}
