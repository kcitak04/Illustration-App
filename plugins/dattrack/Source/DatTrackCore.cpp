#include "DatTrackCore.h"
#include "BinaryData.h"
#include <array>

namespace dat
{
namespace
{
juce::String json (double v) { return juce::String (v, 6); }
juce::String bin (const char* d, int n) { return juce::String::fromUTF8 (d, n); }
}

void installStaticFiles (LocalServer& s)
{
    // DATTRACK's index.html with the plugin bridge loaded before any of its own scripts
    static const juce::String page = []
    {
        auto html = bin (BinaryData::dattrack_html, BinaryData::dattrack_htmlSize);
        auto at = html.indexOfIgnoreCase ("<head>");
        jassert (at >= 0);
        return html.substring (0, at + 6) + "\n<script src=\"/dt-bridge.js\"></script>\n" + html.substring (at + 6);
    }();
    s.setStaticHandler ([] (const HttpRequest& r) -> HttpResponse
    {
        if (r.method != "GET") return HttpResponse::empty (404);
        if (r.path == "/" || r.path == "/index.html") return HttpResponse::text (page, "text/html; charset=utf-8");
        if (r.path == "/dt-bridge.js") return HttpResponse::data (BinaryData::dtbridge_js, (size_t) BinaryData::dtbridge_jsSize, "text/javascript; charset=utf-8");
        if (r.path == "/dt-tap.js") return HttpResponse::data (BinaryData::dttap_js, (size_t) BinaryData::dttap_jsSize, "text/javascript; charset=utf-8");
        return HttpResponse::empty (404);
    });
}

DatTrackCore::DatTrackCore()
{
    token = juce::Uuid().toDashedString().removeCharacters ("-");
    installStaticFiles (*server);
    id = server->registerInstance (token, [this] (const HttpRequest& r) { return handle (r); });
}

DatTrackCore::~DatTrackCore()
{
    server->unregisterInstance (id);
   #if DAT_WITH_LINK
    linkRt = nullptr;
    if (link) link->enable (false);
   #endif
}

bool DatTrackCore::serverOk() const { return server->isRunning(); }

juce::String DatTrackCore::pageUrl (double hostSr) const
{
    return "http://127.0.0.1:" + juce::String (server->getPort()) + "/?i=" + juce::String (id) + "&t=" + token + "&sr=" + juce::String ((int) std::round (hostSr));
}

juce::File DatTrackCore::exportFolder()
{
    auto f = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("DATTRACK");
    f.createDirectory();
    return f;
}

void DatTrackCore::setSongFromHost (const juce::String& j)
{
    { std::lock_guard<std::mutex> l (songLock); songJson = j; songSeq++; }
}

// ---------------- HTTP (server threads) ----------------
HttpResponse DatTrackCore::handle (const HttpRequest& r)
{
    auto route = r.path.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("/", false, false);
    if (route == "a" && r.method == "POST") return onAudio (r);
    if (route == "state")
    {
        if (r.method == "POST")
        {
            if (r.body.getSize() < 2) return HttpResponse::empty (400);
            { std::lock_guard<std::mutex> l (songLock); songJson = juce::String::fromUTF8 ((const char*) r.body.getData(), (int) r.body.getSize()); }
            ++songEditsFromPage;
            return HttpResponse::empty();
        }
        std::lock_guard<std::mutex> l (songLock);
        return songJson.isEmpty() ? HttpResponse::empty() : HttpResponse::text (songJson);
    }
    if (route == "flushed") { flushAck = flushRequest.load(); return HttpResponse::empty(); }
    if (route == "save" && r.method == "POST")
    {
        auto name = juce::File::createLegalFileName (r.q ("name", "export.wav"));
        if (name.isEmpty()) name = "export.wav";
        auto f = exportFolder().getNonexistentChildFile (juce::File (name).getFileNameWithoutExtension(), juce::File (name).getFileExtension(), true);
        if (! f.replaceWithData (r.body.getData(), r.body.getSize())) return HttpResponse::text ("{\"error\":\"write\"}", "application/json", 500);
        { std::lock_guard<std::mutex> l (songLock); lastExport = f; }
        sendChangeMessage();
        return HttpResponse::text ("{\"path\":" + juce::JSON::toString (f.getFullPathName()) + ",\"name\":" + juce::JSON::toString (f.getFileName()) + "}");
    }
    return HttpResponse::empty (404);
}

HttpResponse DatTrackCore::onAudio (const HttpRequest& r)
{
    const auto frame = r.q ("f").getLargeIntValue();
    const double sr = juce::jlimit (8000.0, 384000.0, r.q ("sr", "48000").getDoubleValue());
    const int frames = (int) (r.body.getSize() / (sizeof (float) * 2));
    if (frames > 0) bridge.push (frame, (const float*) r.body.getData(), frames, sr);
    pageConnected = true;
    lastPacketMs = juce::Time::currentTimeMillis();

    // what the page reports about DATTRACK (used by MASTER and the GUI)
    {
        PageState p; p.playing = r.q ("p") == "1"; p.bpm = r.q ("b").getDoubleValue(); p.beat = r.q ("ab").getDoubleValue(); p.at = r.q ("at").getDoubleValue();
        const juce::SpinLock::ScopedLockType l (pageLock); p.seq = page.seq + 1; page = p;
        dtPlaying = p.playing; dtBpm = p.bpm;
    }

    Anchor a; { const juce::SpinLock::ScopedLockType l (anchorLock); a = anchor; }
    juce::String midi;
    {
        int s1, n1, s2, n2; midiFifo.prepareToRead (midiFifo.getNumReady(), s1, n1, s2, n2);
        auto add = [&] (int s, int n) { for (int i = 0; i < n; ++i) { auto m = midiData[(size_t) (s + i)]; midi << (midi.isEmpty() ? "" : ",") << "[" << (int) ((m >> 16) & 0xff) << "," << (int) ((m >> 8) & 0xff) << "," << (int) (m & 0xff) << "]"; } };
        add (s1, n1); add (s2, n2); midiFifo.finishedRead (n1 + n2);
    }
    int seq; { std::lock_guard<std::mutex> l (songLock); seq = songSeq; }
    juce::String out;
    out << "{\"mode\":" << mode.load() << ",\"playing\":" << (a.playing ? "true" : "false") << ",\"bpm\":" << json (a.bpm)
        << ",\"r\":" << json (a.rate) << ",\"f\":" << json (a.f) << ",\"ppq\":" << json (a.ppq) << ",\"jump\":" << a.jump << ",\"jp\":" << json (a.jumpPpq) << ",\"song\":" << seq
        << ",\"midi\":[" << midi << "],\"stop\":" << pendingStop.exchange (0) << ",\"flush\":" << flushRequest.load();
    if (a.looping) out << ",\"loop\":[" << json (a.ls) << "," << json (a.le) << "]";
    out << "}";
    return HttpResponse::text (out);
}

// ---------------- audio thread ----------------
void DatTrackCore::process (float* L, float* R, int n, double hostSr, const HostTransport& t, const juce::MidiBuffer* midi, bool offline)
{
    // the buffer gets the latency minus 150 ms; those 150 ms are the page's head start for cueing rows on time
    // (pressing play in the host: the page hears about it one round trip later and must still catch the first row)
    bridge.setTargetFill (juce::jmax (0.05, latencySec.load() - 0.15));
    if (offline && juce::Time::currentTimeMillis() - lastPacketMs.load() < 2000)
    {   // the export runs at the page's (real-time) pace instead of skipping audio; give up after 3 s if the page stalls
        const auto t0 = juce::Time::getMillisecondCounter();
        while (! bridge.hasAhead (n, hostSr) && juce::Time::getMillisecondCounter() - t0 < 3000) juce::Thread::sleep (1);
    }
    auto st = bridge.pull (L, R, n, hostSr);
    audioRunning = st.running; fillSec = st.fillSec; underruns = bridge.underruns;
    hostBpm = t.valid ? t.bpm : 0.0;

    if (midi != nullptr && midiToTracker.load())
        for (const auto m : *midi)
        {
            auto msg = m.getMessage();
            if (! msg.isNoteOnOrOff()) continue;
            int s1, n1, s2, n2; midiFifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 + n2 == 0) break;
            midiData[(size_t) (n1 ? s1 : s2)] = ((juce::uint32) (msg.isNoteOn() ? 0x90 : 0x80) << 16) | ((juce::uint32) msg.getNoteNumber() << 8) | (juce::uint32) msg.getVelocity();
            midiFifo.finishedWrite (1);
        }

    // anchor: the host's position at this block start plays out 'latency' later, i.e. at context frame readFrame + latency * ratio
    if (t.valid)
    {
        const double latHost = latencySec.load() * hostSr - offsetMs.load() / 1000.0 * hostSr;
        if (t.playing)
        {
            const double expected = lastPpq + lastPpqStep;
            if (! lastPlaying || std::abs (t.ppq - expected) > 0.02) { ++jumps; jumpPpq = t.ppq; }   // started, looped or relocated (and where to)
            lastPpqStep = (double) n / hostSr * t.bpm / 60.0;
        }
        lastPpq = t.ppq; lastPlaying = t.playing;
        const juce::SpinLock::ScopedLockType l (anchorLock);
        anchor.f = st.readFrame + latHost * st.ratio; anchor.ppq = t.ppq; anchor.bpm = t.bpm; anchor.playing = t.playing; anchor.jump = jumps; anchor.jumpPpq = jumpPpq;
        // context seconds per host second at the read head right now (the resampling ratio, drift correction included)
        anchor.rate = st.running ? st.ratio * hostSr / bridge.contextRate() : 1.0;
        anchor.looping = t.looping; anchor.ls = t.loopStart; anchor.le = t.loopEnd;
    }

    if (mode.load() == (int) SyncMode::master) runLink (hostSr, st, n);
}

void DatTrackCore::runLink (double hostSr, const AudioBridge::Status& st, int)
{
   #if DAT_WITH_LINK
    auto* lk = linkRt.load();
    if (lk == nullptr) return;
    PageState p; { const juce::SpinLock::ScopedLockType l (pageLock); p = page; }
    const auto now = lk->clock().micros();
    const double ctxSr = bridge.contextRate();
    // context time -> Link time: frame F is read (st.readFrame - F) / ratio host samples from now, and heard 'latency' after that
    auto ctxToLink = [&] (double ctxTime)
    {
        const double hostSamples = (ctxTime * ctxSr - st.readFrame) / st.ratio - latencySec.load() * hostSr;
        return now + std::chrono::microseconds ((long long) (hostSamples / hostSr * 1e6 + offsetMs.load() * 1000.0));
    };
    auto s = lk->captureAudioSessionState();
    bool changed = false;
    if (p.bpm >= 20 && std::abs (s.tempo() - p.bpm) > 0.005) { s.setTempo (p.bpm, now); changed = true; }
    if (p.playing && st.running)
    {
        const auto at = ctxToLink (p.at);
        if (! s.isPlaying()) { s.forceBeatAtTime (p.beat, at, 4.0); s.setIsPlaying (true, at); linkWasPlayingByUs = true; changed = true; }
        else
        {
            double diff = std::fmod (p.beat - s.beatAtTime (at, 4.0), 4.0);
            if (diff > 2) diff -= 4;
            if (diff < -2) diff += 4;
            if (std::abs (diff) > 0.02) { s.forceBeatAtTime (p.beat, at, 4.0); changed = true; }
        }
    }
    else if (! p.playing && s.isPlaying() && linkWasPlayingByUs) { s.setIsPlaying (false, now); linkWasPlayingByUs = false; changed = true; }
    else if (p.playing && ! s.isPlaying() && linkWasPlayingByUs) { pendingStop = 1; linkWasPlayingByUs = false; }   // stopped from Ableton
    if (changed) lk->commitAudioSessionState (s);
   #else
    juce::ignoreUnused (hostSr, st);
   #endif
}

void DatTrackCore::updateLink()
{
   #if DAT_WITH_LINK
    const bool want = mode.load() == (int) SyncMode::master;
    if (want && ! link)
    {
        link = std::make_unique<ableton::Link> (dtBpm.load() > 0 ? dtBpm.load() : 120.0);
        link->enableStartStopSync (true);
    }
    if (link)
    {
        if (link->isEnabled() != want) link->enable (want);
        linkRt = want ? link.get() : nullptr;
        linkPeers = want ? (int) link->numPeers() : 0;
    }
   #endif
}

juce::String DatTrackCore::getSyncText() const
{
    auto m = (SyncMode) mode.load();
    if (! serverOk()) return "Local server could not start (ports 47811-47850 busy)";
    if (! pageConnected.load())
    {
        const auto started = browserStartedMs.load();
        if (started > 0 && juce::Time::currentTimeMillis() - started > 12000)
            return "DATTRACK did not start: press RELOAD. If the window stays blank, install the Microsoft Edge WebView2 Runtime";
        return "Starting DATTRACK...";
    }
    if (juce::Time::currentTimeMillis() - lastPacketMs.load() > 1500) return "DATTRACK audio is paused - open the window and click once to start it";
    juce::String s;
    if (m == SyncMode::off) s = "FREE: DATTRACK runs on its own clock";
    else if (m == SyncMode::slave) s = hostBpm.load() > 0 ? "SLAVE: following Ableton at " + juce::String (hostBpm.load(), 2) + " BPM" : "SLAVE: waiting for the host transport";
    else
    {
       #if DAT_WITH_LINK
        s = "MASTER via Link: " + juce::String (dtBpm.load(), 2) + " BPM, " + juce::String (linkPeers.load()) + (linkPeers.load() == 1 ? " peer" : " peers");
        if (linkPeers.load() == 0) s << " (turn on LINK + Start Stop Sync in Live)";
       #else
        s = "MASTER needs Ableton Link (not in this build)";
       #endif
    }
    if (underruns.load() > 0) s << "  -  dropouts: " << underruns.load();
    return s;
}
}
