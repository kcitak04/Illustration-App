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
};

class DatTrackCore : public juce::ChangeBroadcaster
{
public:
    DatTrackCore();
    ~DatTrackCore() override;

    // page URL for the web view (the context runs at the host sample rate)
    juce::String pageUrl (double hostSr) const;
    bool serverOk() const;

    // audio thread: renders the page's audio and runs the sync logic
    void process (float* L, float* R, int n, double hostSr, const HostTransport&, const juce::MidiBuffer* midi);
    int latencySamples (double hostSr) const { return (int) std::round (latencySec.load() * hostSr); }

    // settings (message thread)
    std::atomic<int> mode { (int) SyncMode::slave };
    std::atomic<double> latencySec { 0.25 }, offsetMs { 0 };
    std::atomic<bool> midiToTracker { true };
    void updateLink();   // enables / disables Link to match the mode (call on the message thread)

    // song (JSON .mtk project) - saved in the Ableton set
    juce::String getSongJson() const { std::lock_guard<std::mutex> l (songLock); return songJson; }
    void setSongFromHost (const juce::String& json);

    // status for the GUI
    std::atomic<bool> pageConnected { false }, dtPlaying { false }, audioRunning { false };
    std::atomic<double> dtBpm { 0 }, fillSec { 0 }, hostBpm { 0 };
    std::atomic<int> linkPeers { 0 }, underruns { 0 };
    std::atomic<juce::int64> lastPacketMs { 0 };
    juce::File getLastExport() const { std::lock_guard<std::mutex> l (songLock); return lastExport; }
    juce::String getSyncText() const;
    static juce::File exportFolder();
    juce::int64 debugFramesWritten() const { return bridge.framesWritten(); }

private:
    HttpResponse handle (const HttpRequest&);
    HttpResponse onAudio (const HttpRequest&);
    void runLink (double hostSr, const AudioBridge::Status&, int n);

    juce::SharedResourcePointer<LocalServer> server;
    int id = 0; juce::String token;
    AudioBridge bridge;

    // audio thread -> page: where the host's musical time sits on the context timeline
    struct Anchor { double f = 0, ppq = 0, bpm = 120, rate = 1; bool playing = false; int jump = 0; };
    juce::SpinLock anchorLock; Anchor anchor;
    double lastPpq = 0, lastPpqStep = 0; bool lastPlaying = false; int jumps = 0;

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
