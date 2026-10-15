#pragma once
#include "PluginProcessor.h"
#include "../../common/DatLookAndFeel.h"
#include "../../common/DragFileTile.h"

class DatTrackEditor : public juce::AudioProcessorEditor, private juce::Timer, private juce::ChangeListener
{
public:
    explicit DatTrackEditor (DatTrackProcessor&);
    ~DatTrackEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();

    DatTrackProcessor& proc;
    dat::LookAndFeel lnf { dat::col::accent };
    juce::TooltipWindow tips { this, 600 };
    juce::TextButton offBtn { "FREE" }, slaveBtn { "SLAVE" }, masterBtn { "MASTER" }, midiBtn { "MIDI IN" }, reloadBtn { "RELOAD" };
    juce::ComboBox latency;
    juce::Slider offset;
    juce::Label status, offsetLabel;
    dat::DragFileTile exportTile;
    juce::WebBrowserComponent* web = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DatTrackEditor)
};
