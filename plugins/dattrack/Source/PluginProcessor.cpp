#include "PluginProcessor.h"
#include "PluginEditor.h"

DatTrackProcessor::DatTrackProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    startTimerHz (10);   // creates the web view on the message thread, keeps Link in step with the sync mode
}

DatTrackProcessor::~DatTrackProcessor()
{
    stopTimer();
    web.reset();
    keepAlive.reset();
}

bool DatTrackProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void DatTrackProcessor::prepareToPlay (double sampleRate, int)
{
    hostSr = sampleRate;
    setLatencySamples (core.latencySamples (sampleRate));
}

void DatTrackProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals nd;
    dat::HostTransport t;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            t.valid = pos->getBpm().hasValue();
            t.bpm = pos->getBpm().orFallback (120.0);
            t.playing = pos->getIsPlaying();
            t.ppq = pos->getPpqPosition().orFallback (0.0);
            if (auto lp = pos->getLoopPoints(); pos->getIsLooping() && lp.hasValue() && lp->ppqEnd > lp->ppqStart + 0.01)
                t.looping = true, t.loopStart = lp->ppqStart, t.loopEnd = lp->ppqEnd;
        }
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : L;
    core.process (L, R, buffer.getNumSamples(), getSampleRate() > 0 ? getSampleRate() : 48000.0, t, &midi, isNonRealtime());
    midi.clear();
    audioRunning = true;
}

void DatTrackProcessor::createBrowser()
{
    using O = juce::WebBrowserComponent::Options;
    auto data = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("DATTRACK").getChildFile ("WebView");
    data.createDirectory();
    auto opts = O{}.withBackend (O::Backend::webview2)
                   .withKeepPageLoadedWhenBrowserIsHidden()
                   .withWinWebView2Options (O::WinWebView2{}.withUserDataFolder (data).withStatusBarDisabled().withBackgroundColour (juce::Colours::black))
                   .withAppleWkWebViewOptions (O::AppleWkWebView{});
    keepAlive = std::make_unique<KeepAlive>();
    keepAlive->setSize (1180, 760);
    keepAlive->setOpaque (true);
    web = std::make_unique<juce::WebBrowserComponent> (opts);
    keepAlive->addAndMakeVisible (*web);
    keepAlive->addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresKeyPresses);
    keepAlive->setVisible (false);
    pageSr = hostSr.load() > 0 ? hostSr.load() : 48000.0;
    core.browserStartedMs = juce::Time::currentTimeMillis();
    navigate();
}

void DatTrackProcessor::recreateBrowser()
{
    // the page never came up (WebView2 failed to start, or drew into a window that is gone): start a fresh web view
    auto* editor = attachedTo.getComponent();
    web.reset();
    keepAlive.reset();
    createBrowser();
    if (editor != nullptr) { editor->addAndMakeVisible (*web); editor->resized(); }
}

void DatTrackProcessor::reloadPage()
{
    if (! web) return;
    if (! core.pageConnected.load()) { recreateBrowser(); return; }
    pageSr = hostSr.load() > 0 ? hostSr.load() : 48000.0;
    if (core.pageConnected.load() && juce::Time::currentTimeMillis() - core.lastPacketMs.load() < 1000)
    {   // a running page saves its song first (the timer navigates once it has, or after 1 s)
        ++core.flushRequest;
        reloadAt = juce::Time::getMillisecondCounter() + 1000;
        return;
    }
    navigate();
}

void DatTrackProcessor::navigate()
{
    reloadAt = 0;
    if (web) web->goToURL (core.pageUrl (pageSr));
}

juce::WebBrowserComponent* DatTrackProcessor::attachBrowser (juce::Component& editor)
{
    if (! web) createBrowser();
    attachedTo = &editor;
    editor.addAndMakeVisible (*web);
    return web.get();
}

void DatTrackProcessor::detachBrowser()
{
    attachedTo = nullptr;
    if (web && keepAlive) keepAlive->addAndMakeVisible (*web), web->setBounds (keepAlive->getLocalBounds());
}

void DatTrackProcessor::markDirty()
{
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void DatTrackProcessor::setLatency (double seconds)
{
    core.latencySec = seconds;
    if (hostSr.load() > 0) setLatencySamples (core.latencySamples (hostSr.load()));
}

void DatTrackProcessor::timerCallback()
{
    // Start the web view only once the host really runs audio through us (or the window opens), not while
    // Ableton's plugin scanner briefly instantiates the plugin - that would launch browser processes on every rescan.
    if (! web && audioRunning.load()) createBrowser();
    if (reloadAt != 0 && (core.flushAck.load() == core.flushRequest.load() || juce::Time::getMillisecondCounter() > reloadAt)) navigate();
    // edits made in DATTRACK (or sync settings) mark the Ableton set as changed, so closing Live asks to save
    const int edits = core.songEditsFromPage.load();
    if (edits != editsSeen) { editsSeen = edits; markDirty(); }
    // a new host sample rate: restart the page so the AudioContext runs at it (the song survives through the plugin state)
    if (reloadAt == 0 && hostSr.load() > 0 && std::abs (hostSr.load() - pageSr) > 1 && core.pageConnected.load()) reloadPage();
    core.updateLink();
}

// ---- state: settings + the whole song (.mtk JSON), saved in the Ableton set ----
void DatTrackProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement x ("DATTRACK");
    x.setAttribute ("version", 1);
    x.setAttribute ("mode", core.mode.load());
    x.setAttribute ("latency", core.latencySec.load());
    x.setAttribute ("offset", core.offsetMs.load());
    x.setAttribute ("midi", core.midiToTracker.load());
    auto song = core.getSongJson();
    if (song.isNotEmpty()) x.createNewChildElement ("Song")->addTextElement (song);
    copyXmlToBinary (x, dest);
}

void DatTrackProcessor::setStateInformation (const void* data, int size)
{
    auto x = getXmlFromBinary (data, size);
    if (! x || ! x->hasTagName ("DATTRACK")) return;
    core.mode = juce::jlimit (0, 2, x->getIntAttribute ("mode", 1));
    core.offsetMs = x->getDoubleAttribute ("offset", 0);
    core.midiToTracker = x->getBoolAttribute ("midi", true);
    setLatency (juce::jlimit (0.1, 1.0, x->getDoubleAttribute ("latency", 0.25)));
    if (auto* s = x->getChildByName ("Song")) core.setSongFromHost (s->getAllSubText());
}

juce::AudioProcessorEditor* DatTrackProcessor::createEditor() { return new DatTrackEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new DatTrackProcessor(); }
