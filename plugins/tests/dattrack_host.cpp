// Fake DAW for testing the DATTRACK plugin core without a DAW: runs DatTrackCore's audio callback in real time
// with a scripted transport, while a browser (Playwright / headless Chromium) runs the real DATTRACK page.
//   dattrack_host --mode slave|master|off --sr 48000 --bpm 128 --play-at 4 --stop-at 14 --jump-at 9 --jump-to 32 --duration 16 --out out.f32
// Prints "URL <page url>" first, then "READY" lines; writes the captured stereo output (interleaved float32)
// and a JSON summary (latency, anchors, stats) at the end.
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "../dattrack/Source/DatTrackCore.h"
#include <ableton/Link.hpp>
#include <fstream>
#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::StringArray args; for (int i = 1; i < argc; ++i) args.add (argv[i]);
    auto arg = [&] (const char* k, const char* def) { int i = args.indexOf (k); return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String (def); };
    const double sr = arg ("--sr", "48000").getDoubleValue(), bpm = arg ("--bpm", "128").getDoubleValue();
    const double playAt = arg ("--play-at", "4").getDoubleValue(), stopAt = arg ("--stop-at", "1e9").getDoubleValue(), dur = arg ("--duration", "14").getDoubleValue();
    const double jumpAt = arg ("--jump-at", "1e9").getDoubleValue(), jumpTo = arg ("--jump-to", "0").getDoubleValue();
    const double drift = arg ("--drift-ppm", "0").getDoubleValue();
    const double loopStart = arg ("--loop-start", "0").getDoubleValue(), loopEnd = arg ("--loop-end", "0").getDoubleValue();   // host loop (beats)
    const bool looping = loopEnd > loopStart;
    const bool loopReportOnly = args.contains ("--loop-report-only");   // reports a loop but never wraps (Live's Session view?)
    const auto modeS = arg ("--mode", "slave");
    const int block = 256;

    dat::DatTrackCore core;
    // a song saved in the "Ableton set" (as setStateInformation would hand it over before the page loads)
    if (auto songFile = arg ("--song-file", ""); songFile.isNotEmpty()) core.setSongFromHost (juce::File (songFile).loadFileAsString());
    core.mode = modeS == "master" ? 2 : modeS == "off" ? 0 : 1;
    core.latencySec = arg ("--latency", "0.25").getDoubleValue();
    core.updateLink();
    std::cout << "URL " << core.pageUrl (sr) << std::endl;
    std::cout << "LATENCY " << core.latencySamples (sr) << std::endl;

    std::vector<float> out; out.reserve ((size_t) (dur * sr * 2) + 4096);
    std::vector<float> L (block), R (block);
    // wait for the page's audio before starting the clock (like opening a set)
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (! core.pageConnected.load() && juce::Time::getMillisecondCounterHiRes() - t0 < 60000) juce::Thread::sleep (20);
    std::cout << "CONNECTED " << (core.pageConnected.load() ? 1 : 0) << std::endl;

    const bool followProducer = args.contains ("--follow-page");
    const juce::int64 firstWritten = core.debugFramesWritten();
    // a second Link peer standing in for Ableton Live (Link + Start Stop Sync on)
    std::unique_ptr<ableton::Link> peer;
    if (args.contains ("--link-peer")) { peer = std::make_unique<ableton::Link> (100.0); peer->enableStartStopSync (true); peer->enable (true); }
    const bool offline = args.contains ("--offline");     // like Ableton's export: no real-time pacing
    double ppq = arg ("--start-ppq", "0").getDoubleValue(); juce::int64 samples = 0; const auto start = juce::Time::getMillisecondCounterHiRes();
    bool jumped = false, flushed = false;
    const double flushAt = arg ("--flush-at", "1e9").getDoubleValue();   // like RELOAD: ask the page to save before reloading
    juce::String transportLog;
    const double clock = 1.0 + drift * 1e-6;   // host audio clock vs. wall clock
    while ((double) samples / sr < dur)
    {
        const double t = (double) samples / sr;
        dat::HostTransport tr; tr.valid = true; tr.bpm = bpm; tr.playing = t >= playAt && t < stopAt;
        if (tr.playing && ! jumped && t >= jumpAt) { jumped = true; ppq = jumpTo; transportLog << "jump " << t << "\n"; }
        tr.ppq = ppq;
        if (looping) { tr.looping = true; tr.loopStart = loopStart; tr.loopEnd = loopEnd; }
        if (! flushed && t >= flushAt) { flushed = true; ++core.flushRequest; std::cout << "FLUSH requested at " << t << std::endl; }
        core.process (L.data(), R.data(), block, sr, tr, nullptr, offline);
        if (tr.playing) ppq += block / sr * bpm / 60.0;
        if (tr.playing && looping && ! loopReportOnly && ppq >= loopEnd && ppq - block / sr * bpm / 60.0 < loopEnd) ppq = loopStart + (ppq - loopEnd);   // wraps inside the block, like a DAW
        for (int i = 0; i < block; ++i) { out.push_back (L[(size_t) i]); out.push_back (R[(size_t) i]); }
        samples += block;
        if (offline) {}
        else if (followProducer)
        {   // the host clock follows the page's clock (ideal clock domains): wait until the page has rendered ahead
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            while (core.debugFramesWritten() - firstWritten < samples + (juce::int64) ((core.latencySec.load() - 0.1) * sr) + block
                   && juce::Time::getMillisecondCounterHiRes() - t1 < 2000) juce::Thread::sleep (1);
        }
        else
        {   // real-time pacing
            const double due = start + (double) samples / sr * 1000.0 / clock;
            while (juce::Time::getMillisecondCounterHiRes() < due) juce::Thread::sleep (1);
        }
        if (peer && (samples / block) % 188 == 0)
        {
            auto st = peer->captureAppSessionState(); auto now = peer->clock().micros();
            std::cout << "PEER t " << t << " peers " << peer->numPeers() << " tempo " << st.tempo() << " playing " << st.isPlaying()
                      << " beat " << st.beatAtTime (now, 4.0) << " dtPlaying " << core.dtPlaying.load() << " dtBpm " << core.dtBpm.load() << std::endl;
            core.updateLink();
        }
        if ((samples / block) % 100 == 0) std::cout << "T " << t << " fill " << core.fillSec.load() << " run " << core.audioRunning.load() << " written " << core.debugFramesWritten() << " dt " << core.dtPlaying.load() << " " << core.dtBpm.load() << std::endl;
    }
    std::ofstream o (arg ("--out", "out.f32").toStdString(), std::ios::binary);
    o.write ((const char*) out.data(), (std::streamsize) (out.size() * sizeof (float)));
    std::cout << "DONE underruns " << core.underruns.load() << " song " << core.getSongJson().length() << " export " << core.getLastExport().getFullPathName()
              << " edits " << core.songEditsFromPage.load() << " flush " << core.flushRequest.load() << "/" << core.flushAck.load() << " wall " << (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0 << " " << core.debugBridgeStats() << std::endl;
    if (auto out2 = arg ("--song-out", ""); out2.isNotEmpty()) juce::File (out2).replaceWithText (core.getSongJson());
    return 0;
}
