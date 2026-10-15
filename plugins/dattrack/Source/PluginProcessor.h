// DATTRACK as an instrument plugin. The tracker runs in a web view that lives as long as the plugin
// (in a hidden window while the editor is closed), so the music keeps playing with the window closed.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "DatTrackCore.h"

class DatTrackProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    DatTrackProcessor();
    ~DatTrackProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "DATTRACK"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    dat::DatTrackCore core;

    // the web view moves between the editor and a hidden keep-alive window
    juce::WebBrowserComponent* attachBrowser (juce::Component& editor);
    void detachBrowser();
    void reloadPage();
    void setLatency (double seconds);
    void markDirty();   // tells the host the plugin state changed (Ableton then offers to save the set)

private:
    void timerCallback() override;
    void createBrowser();
    void navigate();
    juce::uint32 reloadAt = 0;   // pending reload (waiting for the page to save its song)

    struct KeepAlive : juce::Component { void resized() override { for (auto* c : getChildren()) c->setBounds (getLocalBounds()); } };
    std::unique_ptr<KeepAlive> keepAlive;
    std::unique_ptr<juce::WebBrowserComponent> web;
    std::atomic<double> hostSr { 0 };
    std::atomic<bool> audioRunning { false };
    int editsSeen = 0;
    double pageSr = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DatTrackProcessor)
};
