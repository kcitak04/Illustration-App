#include "PluginProcessor.h"
#include "PluginEditor.h"

DatTrackProcessor::DatTrackProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
   #if JUCE_WINDOWS
    // WebView2: Web Audio may start without a click, and the hidden page is not throttled
    _putenv_s ("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", "--autoplay-policy=no-user-gesture-required --disable-background-timer-throttling --disable-renderer-backgrounding --disable-backgrounding-occluded-windows");
   #endif
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
        }
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : L;
    core.process (L, R, buffer.getNumSamples(), getSampleRate() > 0 ? getSampleRate() : 48000.0, t, &midi);
    midi.clear();
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
    reloadPage();
}

void DatTrackProcessor::reloadPage()
{
    if (! web) return;
    pageSr = hostSr.load() > 0 ? hostSr.load() : 48000.0;
    web->goToURL (core.pageUrl (pageSr));
}

juce::WebBrowserComponent* DatTrackProcessor::attachBrowser (juce::Component& editor)
{
    if (! web) createBrowser();
    editor.addAndMakeVisible (*web);
    return web.get();
}

void DatTrackProcessor::detachBrowser()
{
    if (web && keepAlive) keepAlive->addAndMakeVisible (*web), web->setBounds (keepAlive->getLocalBounds());
}

void DatTrackProcessor::setLatency (double seconds)
{
    core.latencySec = seconds;
    if (hostSr.load() > 0) setLatencySamples (core.latencySamples (hostSr.load()));
}

void DatTrackProcessor::timerCallback()
{
    if (! web) createBrowser();
    // a new host sample rate: restart the page so the AudioContext runs at it (the song survives through the plugin state)
    if (hostSr.load() > 0 && std::abs (hostSr.load() - pageSr) > 1 && core.pageConnected.load()) reloadPage();
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
