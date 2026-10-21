// Everything the DATTRACK plugin does besides the GUI: the page's audio stream, the song state, exports,
// MIDI notes, and the two sync directions:
//   SLAVE  - DATTRACK follows Ableton's transport (tempo, play/stop, position: 1 beat = 4 rows)
//   MASTER - DATTRACK's tempo and play/stop drive Ableton through Ableton Link (Live: Link + Start Stop Sync on)
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include <array>
#include "LocalServer.h"
#include "AudioBridge.h"

#if DAT_WITH_LINK
 #include <ableton/Link.hpp>
#endif

namespace dat
{
enum class SyncMode { off = 0, slave = 1, master = 2 };

struct HostTransport
{
    bool valid = false, playing = false;
    double bpm = 0, ppq = 0;
    bool looping = false; double loopStart = 0, loopEnd = 0;   // the host's loop (cycle) in beats, when it is on
};

class DatTrackCore : public juce::ChangeBroadcaster
{
public:
    DatTrackCore();
    ~DatTrackCore() override;

    // page URL for the web view (the context runs at the host sample rate)
    juce::String pageUrl (double hostSr) const;
    bool serverOk() const;

    // audio thread: renders the page's audio and runs the sync logic. offline = the DAW renders faster than
    // real time (export / freeze): then each block waits until the page has made its audio, so nothing is lost.
    void process (float* L, float* R, int n, double hostSr, const HostTransport&, const juce::MidiBuffer* midi, bool offline = false);
    int latencySamples (double hostSr) const { return (int) std::round (latencySec.load() * hostSr); }

    // settings (message thread)
    std::atomic<int> mode { (int) SyncMode::slave };
    std::atomic<double> latencySec { 0.25 }, offsetMs { 0 };
    std::atomic<bool> midiToTracker { true };
    void updateLink();   // enables / disables Link to match the mode (call on the message thread)

    // song (JSON .mtk project) - saved in the Ableton set
    juce::String getSongJson() const { std::lock_guard<std::mutex> l (songLock); return songJson; }
    // the song for saving the set: if the page has edits it has not sent yet (it waits for a pause in the
    // editing), ask it for them and wait up to timeoutMs (any thread; the page answers through the server)
    juce::String getSongJsonFlushed (int timeoutMs);
    std::atomic<bool> pageEditsPending { false };
    void setSongFromHost (const juce::String& json);
    std::atomic<int> songEditsFromPage { 0 };   // bumped when the page saves an edited song (the set needs saving)
    // before a page reload: ask the page to save its song; flushAck catches up with flushRequest when it has
    std::atomic<int> flushRequest { 0 }, flushAck { 0 };

    // status for the GUI
    std::atomic<bool> pageConnected { false }, dtPlaying { false }, audioRunning { false };
    std::atomic<double> dtBpm { 0 }, fillSec { 0 }, hostBpm { 0 };
    std::atomic<int> linkPeers { 0 }, underruns { 0 };
    std::atomic<juce::int64> lastPacketMs { 0 }, browserStartedMs { 0 };
    juce::File getLastExport() const { std::lock_guard<std::mutex> l (songLock); return lastExport; }
    juce::String getSyncText() const;
    static juce::File exportFolder();
    juce::int64 debugFramesWritten() const { return bridge.framesWritten(); }
    juce::String debugBridgeStats() const { return "skips " + juce::String (bridge.skips) + " stalls " + juce::String (bridge.stalls) + " resets " + juce::String (bridge.resets); }

private:
    HttpResponse handle (const HttpRequest&);
    HttpResponse onAudio (const HttpRequest&);
    void runLink (double hostSr, const AudioBridge::Status&, int n);

    juce::SharedResourcePointer<LocalServer> server;
    int id = 0; juce::String token;
    AudioBridge bridge;

    // audio thread -> page: where the host's musical time sits on the context timeline
    struct Anchor { double f = 0, rd = 0, ppq = 0, bpm = 120, rate = 1; bool playing = false, running = false; int epoch = 0, jump = 0; double jumpPpq = 0; bool looping = false; double ls = 0, le = 0; };
    juce::SpinLock anchorLock; Anchor anchor;
    double lastPpq = 0, lastPpqStep = 0, jumpPpq = 0; bool lastPlaying = false; int jumps = 0;

    // page -> audio thread: what DATTRACK is doing (for MASTER)
    struct PageState { bool playing = false; double bpm = 0, beat = 0, at = 0; juce::int64 seq = 0; };
    juce::SpinLock pageLock; PageState page;

    // MIDI notes from the host, drained into the next reply
    juce::AbstractFifo midiFifo { 512 }; std::array<juce::uint32, 512> midiData {};
    std::atomic<int> pendingStop { 0 };

    mutable std::mutex songLock;
    juce::String songJson; int songSeq = 0; juce::File lastExport;

   #if DAT_WITH_LINK
    std::unique_ptr<ableton::Link> link;
    std::atomic<ableton::Link*> linkRt { nullptr };
    bool linkWasPlayingByUs = false;
   #endif

    JUCE_DECLARE_NON_COPYABLE (DatTrackCore)
};

// static page files (patched DATTRACK + bridge scripts), shared by all instances
void installStaticFiles (LocalServer&);
}
