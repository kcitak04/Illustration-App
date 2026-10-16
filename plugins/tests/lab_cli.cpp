// Command-line runner for the DATLAB DSP (used by the parity and smoke tests).
// lab_cli <proc> <in.f32> <channels> <sr> <out.f32> [aux.f32 auxChannels] [key=value ...]
// .f32 files are planar: channel 0 samples, then channel 1 samples. Output header: one line "channels length".
#include "../datlab/Source/dsp/LabEngine.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

static lab::Chans readF32 (const char* path, int nch)
{
    std::ifstream f (path, std::ios::binary); std::vector<char> b ((std::istreambuf_iterator<char> (f)), {});
    size_t n = b.size() / 4 / (size_t) nch; lab::Chans ch ((size_t) nch, lab::Buf (n));
    for (int c = 0; c < nch; ++c) std::memcpy (ch[(size_t) c].data(), b.data() + (size_t) c * n * 4, n * 4);
    return ch;
}

int main (int argc, char** argv)
{
    if (argc == 2 && std::string (argv[1]) == "--defs")
    {   // every process with its parameter ranges, as JSON
        std::cout << "{";
        bool first = true;
        for (auto& d : lab::procs())
        {
            std::cout << (first ? "" : ",") << "\"" << d.id << "\":{\"new\":" << (d.isNew ? 1 : 0) << ",\"aux\":\"" << d.aux << "\",\"p\":{";
            first = false; bool f2 = true;
            for (auto& p : d.params) { std::cout << (f2 ? "" : ",") << "\"" << p.id << "\":[" << p.min << "," << p.max << "," << p.def << "," << (p.isInt() ? 1 : 0) << "]"; f2 = false; }
            std::cout << "}}";
        }
        std::cout << "}\n";
        return 0;
    }
    if (argc >= 2 && std::string (argv[1]) == "--fuzz")
    {   // robustness: every process with odd inputs (tiny, mono, silent, DC, full-scale noise) and random / extreme parameters
        const int rounds = argc > 2 ? std::atoi (argv[2]) : 20;
        const std::string only = argc > 3 ? argv[3] : "";   // optional: only processes whose id starts with this
        uint32_t s = 7; auto rnd = [&] { s = s * 1664525u + 1013904223u; return (double) s / 4294967296.0; };
        int runs = 0, errors = 0;
        for (int r = 0; r < rounds; ++r)
        {
            const double sr = r % 3 == 0 ? 22050 : r % 3 == 1 ? 44100 : 96000;
            const size_t lens[] = { 16, 17, 64, 100, 513, 1000, 4096, 22050, 60000 };
            const size_t len = lens[(size_t) (rnd() * 9)];
            const int nch = rnd() < 0.5 ? 1 : 2, kind = (int) (rnd() * 6);
            lab::Chans src ((size_t) nch, lab::Buf (len));
            for (auto& c : src) for (size_t i = 0; i < len; ++i)
                c[i] = (float) (kind == 0 ? 0.0 : kind == 1 ? 0.5 : kind == 2 ? (rnd() * 2 - 1) : kind == 3 ? std::sin ((double) i * 0.05) : kind == 4 ? (i == len / 2 ? 1.0 : 0.0) : (i % 7 == 0 ? NAN : i % 11 == 0 ? INFINITY : 0.3));
            for (auto& d : lab::procs())
            {
                if (! only.empty() && d.id.rfind (only, 0) != 0) continue;
                lab::Params p = lab::defaults (d);
                for (auto& pd : d.params)
                {
                    double u = rnd(); double v = u < 0.2 ? pd.min : u < 0.4 ? pd.max : pd.min + (pd.max - pd.min) * rnd();
                    p.v[pd.id] = pd.isInt() ? std::round (v) : v;
                    if (rnd() < 0.03) p.v[pd.id] = rnd() < 0.5 ? NAN : (rnd() < 0.5 ? INFINITY : -1e300);   // damaged saved set
                }
                lab::Job job; job.src = src; job.sr = sr; job.procId = d.id; job.params = p; job.bpm = 40 + rnd() * 200;
                job.shape.maxLen = 20;   // keep fuzzing fast
                job.shape.mix = rnd(); job.shape.revIn = rnd() < 0.3; job.shape.revOut = rnd() < 0.3; job.shape.trim = rnd() < 0.3;
                job.shape.loopXfade = rnd() < 0.3 ? rnd() * 2000 : 0; job.shape.lowCut = rnd() < 0.3 ? 20 + rnd() * 1000 : 20; job.shape.highCut = rnd() < 0.3 ? 500 + rnd() * 10000 : 20000;
                job.shape.width = rnd() * 2; job.shape.start = rnd() < 0.3 ? rnd() * 0.5 : 0; job.shape.end = rnd() < 0.3 ? 0.5 + rnd() * 0.5 : 1;
                if (rnd() < 0.15) { job.shape.start = 0.5; job.shape.end = 0.5 + rnd() * 0.02; }   // tiny region (a few samples)
                if (rnd() < 0.05) { job.shape.maxLen = NAN; job.shape.width = INFINITY; job.shape.fadeOut = NAN; job.shape.start = NAN; }
                if (rnd() < 0.05) job.bpm = NAN;
                if (! d.aux.empty()) { size_t al = lens[(size_t) (rnd() * 9)]; job.aux = lab::Chans ((size_t) (rnd() < 0.5 ? 1 : 2), lab::Buf (al, 0.25f)); }
                auto res = lab::run (job, nullptr);
                ++runs;
                if (! res.error.empty() && res.error.find ("silent") == std::string::npos && res.error.find ("empty") == std::string::npos) { ++errors; std::printf ("error %s len %zu ch %d kind %d: %s\n", d.id.c_str(), len, nch, kind, res.error.c_str()); }
                for (auto& c : res.ch) for (float v : c) if (! std::isfinite (v) || std::abs (v) > 1.0001f) { std::printf ("BAD SAMPLE %s len %zu\n", d.id.c_str(), len); ++errors; break; }
            }
            std::printf ("round %d done (sr %.0f len %zu ch %d kind %d)\n", r, sr, len, nch, kind); std::fflush (stdout);
        }
        std::printf ("%d runs, %d problems\n", runs, errors);
        return errors ? 1 : 0;
    }
    if (argc >= 2 && std::string (argv[1]) == "--smoke")
    {   // every process x every MODE option through the full engine, on a 2 s stereo test signal
        const double sr = 44100; const size_t n = (size_t) (sr * 2);
        lab::Chans src (2, lab::Buf (n)); uint32_t s = 1;
        for (size_t i = 0; i < n; ++i)
        {
            double t = (double) i / sr, env = std::exp (-std::fmod (t * 4, 1.0) * 5); s = s * 1103515245u + 12345u;
            src[0][i] = (float) (0.5 * std::sin (6.2831853 * 110 * t) * env + 0.1 * ((double) s / 4294967296.0 - 0.5));
            src[1][i] = (float) (0.4 * std::sin (6.2831853 * 165 * t) * env);
        }
        int bad = 0;
        const std::string only = argc > 2 ? argv[2] : "";   // optional: only processes whose id starts with this
        for (auto& d : lab::procs())
        {
            if (! only.empty() && d.id.rfind (only, 0) != 0) continue;
            std::vector<lab::Params> variants { lab::defaults (d) };
            for (auto& pd : d.params)
                if (pd.isOptions() && (pd.id == "mode" || pd.id == "op" || pd.id == "pattern" || pd.id == "type" || pd.id == "shape" || pd.id == "chord" || pd.id == "motion" || pd.id == "wave" || pd.id == "sync"))
                    for (int o = 0; o < (int) pd.options.size(); ++o) if (o != (int) pd.def) { auto p = lab::defaults (d); p.v[pd.id] = o; variants.push_back (p); }
            for (auto& p : variants)
            {
                lab::Job job; job.src = src; job.sr = sr; job.procId = d.id; job.params = p; job.bpm = 128;
                if (! d.aux.empty()) job.aux = { src[1] };
                auto t0 = std::chrono::steady_clock::now();
                auto r = lab::run (job, nullptr);
                double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
                bool ok = r.error.empty() && ! r.ch.empty();
                double pk = ok ? lab::peakAll (r.ch) : 0, rms = 0; size_t len = ok ? r.ch[0].size() : 0;
                if (ok) { for (auto& c : r.ch) for (float v : c) { if (! std::isfinite (v)) ok = false; rms += (double) v * v; } rms = std::sqrt (rms / (double) (len * r.ch.size())); }
                if (! ok || rms < 1e-4) bad++;
                std::string desc; for (auto& pd : d.params) if (pd.isOptions() && p[pd.id] != pd.def) desc += pd.id + "=" + pd.options[(size_t) p.i (pd.id)] + " ";
                std::printf ("%s %-8s %-28s ch %zu  len %6.2fs  peak %.2f  rms %.3f  %6.0f ms %s\n", ok && rms >= 1e-4 ? "ok  " : "FAIL", d.id.c_str(), desc.c_str(),
                             ok ? r.ch.size() : (size_t) 0, (double) len / sr, pk, rms, ms, r.error.c_str());
            }
        }
        std::printf ("%d failures\n", bad);
        return bad ? 1 : 0;
    }
    if (argc < 6) { std::cerr << "usage: lab_cli proc in.f32 channels sr out.f32 [aux=file:channels] [raw=1] [key=value...]\n"; return 2; }
    lab::Job job; job.procId = argv[1]; job.src = readF32 (argv[2], std::atoi (argv[3])); job.sr = std::atof (argv[4]);
    bool raw = false;
    for (int i = 6; i < argc; ++i)
    {
        std::string a = argv[i]; auto eq = a.find ('='); if (eq == std::string::npos) continue;
        std::string k = a.substr (0, eq), v = a.substr (eq + 1);
        if (k == "aux") { auto c = v.find (':'); job.aux = readF32 (v.substr (0, c).c_str(), std::stoi (v.substr (c + 1))); }
        else if (k == "raw") raw = v == "1";
        else if (k == "bpm") job.bpm = std::stod (v);
        else job.params.v[k] = std::stod (v);
    }
    lab::Chans out;
    if (raw)
    {   // the bare process, exactly like the DATTRACK worker (no SHAPE section, no normalize)
        const auto* def = lab::findProc (job.procId); if (! def) { std::cerr << "unknown proc\n"; return 1; }
        lab::Ctx cx; cx.sr = job.sr; cx.maxLen = (size_t) (180 * job.sr); cx.aux = job.aux.empty() ? nullptr : &job.aux; cx.bpm = job.bpm;
        lab::Params p = lab::defaults (*def); for (auto& [k, v] : job.params.v) p.v[k] = v;
        try { out = def->fn (job.src, cx, p); } catch (const std::exception& e) { std::cerr << "error: " << e.what() << "\n"; return 1; }
        for (auto& a : out) for (auto& v : a) if (! std::isfinite (v)) v = 0;
    }
    else
    {
        auto r = lab::run (job, nullptr);
        if (! r.error.empty()) { std::cerr << "error: " << r.error << "\n"; return 1; }
        out = r.ch;
    }
    std::ofstream o (argv[5], std::ios::binary);
    o << out.size() << " " << out[0].size() << "\n";
    for (auto& c : out) o.write ((const char*) c.data(), (std::streamsize) (c.size() * 4));
    return 0;
}
