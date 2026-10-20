#include "PluginEditor.h"

using namespace dat;

DatTrackEditor::DatTrackEditor (DatTrackProcessor& p) : AudioProcessorEditor (p), proc (p)
{
    setLookAndFeel (&lnf);
    auto modeBtn = [this] (juce::TextButton& b, dat::SyncMode m, const char* tip)
    {
        addAndMakeVisible (b);
        b.setClickingTogglesState (false);
        b.setTooltip (tip);
        b.onClick = [this, m] { proc.core.mode = (int) m; proc.core.updateLink(); proc.markDirty(); refresh(); };
    };
    modeBtn (offBtn, SyncMode::off, "DATTRACK plays on its own clock and tempo");
    modeBtn (slaveBtn, SyncMode::slave, "Ableton is the master: DATTRACK follows its tempo, play/stop and position (1 beat = 4 rows)");
    modeBtn (masterBtn, SyncMode::master, "DATTRACK is the master: its tempo and play/stop drive Ableton through Ableton Link. "
                                          "In Live: Preferences > Link, Tempo & MIDI > Link ON and Start Stop Sync ON, then switch on LINK in the control bar.");
    addAndMakeVisible (midiBtn);
    midiBtn.setTooltip ("Play the selected DATTRACK instrument from the MIDI notes of this track");
    midiBtn.onClick = [this] { proc.core.midiToTracker = ! proc.core.midiToTracker.load(); proc.markDirty(); refresh(); };

    addAndMakeVisible (latency);
    latency.setTooltip ("Audio buffer between DATTRACK and Ableton. Ableton compensates it automatically; raise it if you hear dropouts.");
    latency.addItem ("150 ms", 1); latency.addItem ("250 ms", 2); latency.addItem ("400 ms", 3); latency.addItem ("700 ms", 4);
    latency.onChange = [this] { const double v[] = { 0.15, 0.25, 0.4, 0.7 }; proc.setLatency (v[juce::jlimit (0, 3, latency.getSelectedId() - 1)]); proc.markDirty(); };

    addAndMakeVisible (offset);
    offset.setSliderStyle (juce::Slider::LinearHorizontal);
    offset.setRange (-100, 100, 0.5);
    offset.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    offset.setTextValueSuffix (" ms");
    offset.setDoubleClickReturnValue (true, 0);
    offset.setTooltip ("Fine-tune the sync: positive = DATTRACK earlier");
    offset.onValueChange = [this] { proc.core.offsetMs = offset.getValue(); proc.markDirty(); };
    offsetLabel.setText ("NUDGE", juce::dontSendNotification); offsetLabel.setFont (LookAndFeel::font (10.5f, true));
    offsetLabel.setColour (juce::Label::textColourId, col::dim); addAndMakeVisible (offsetLabel);

    addAndMakeVisible (reloadBtn);
    reloadBtn.setTooltip ("Reload the DATTRACK page (the song is kept)");
    reloadBtn.onClick = [this] { proc.reloadPage(); };

    status.setFont (LookAndFeel::font (12)); status.setColour (juce::Label::textColourId, col::dim);
    status.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (status);
    exportTile.setAccent (col::accent);
    exportTile.setEmptyText ("NO EXPORT YET", "PREF > EXPORT in DATTRACK");
    exportTile.setTooltip ("Your last DATTRACK export (PREF > EXPORT, or SAVE). Drag it onto an Ableton track. Double-click: show in folder.");
    addAndMakeVisible (exportTile);

    proc.attachBrowser (*this);
    proc.core.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (900, 560, 2400, 1600);
    // fit small / scaled laptop screens (Ableton can't show a plugin window bigger than the display)
    auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                    ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea : juce::Rectangle<int> (0, 0, 1400, 900);
    setSize (juce::jlimit (900, 1200, area.getWidth() - 60), juce::jlimit (560, 800, area.getHeight() - 110));
    refresh();
    startTimerHz (4);
}

DatTrackEditor::~DatTrackEditor()
{
    proc.core.removeChangeListener (this);
    proc.detachBrowser();
    setLookAndFeel (nullptr);
}

void DatTrackEditor::changeListenerCallback (juce::ChangeBroadcaster*) { refresh(); }
void DatTrackEditor::timerCallback() { refresh(); }

void DatTrackEditor::refresh()
{
    auto m = (SyncMode) proc.core.mode.load();
    offBtn.setToggleState (m == SyncMode::off, juce::dontSendNotification);
    slaveBtn.setToggleState (m == SyncMode::slave, juce::dontSendNotification);
    masterBtn.setToggleState (m == SyncMode::master, juce::dontSendNotification);
    midiBtn.setToggleState (proc.core.midiToTracker.load(), juce::dontSendNotification);
    const double lat = proc.core.latencySec.load();
    latency.setSelectedId (lat < 0.2 ? 1 : lat < 0.3 ? 2 : lat < 0.5 ? 3 : 4, juce::dontSendNotification);
    if (! offset.isMouseButtonDown()) offset.setValue (proc.core.offsetMs.load(), juce::dontSendNotification);
    status.setText (proc.core.getSyncText(), juce::dontSendNotification);
    auto f = proc.core.getLastExport();
    exportTile.setFile (f, f.getFileName());
}

void DatTrackEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);
    g.setColour (col::panel); g.fillRect (getLocalBounds().removeFromTop (56 + (status.getY() >= 56 ? stripHeight : 0)));
    g.setColour (col::accent); g.setFont (LookAndFeel::font (20, true));
    g.drawText ("DATTRACK", 14, 0, 130, 56, juce::Justification::centredLeft);
    g.setColour (col::dim); g.setFont (LookAndFeel::font (10.5f, true));
    g.drawText ("SYNC", 146, 0, 40, 56, juce::Justification::centredLeft);
}

void DatTrackEditor::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (56).reduced (10, 8);
    bar.removeFromLeft (176);
    auto modes = bar.removeFromLeft (230);
    offBtn.setBounds (modes.removeFromLeft (70)); modes.removeFromLeft (4);
    slaveBtn.setBounds (modes.removeFromLeft (74)); modes.removeFromLeft (4);
    masterBtn.setBounds (modes.removeFromLeft (78));
    exportTile.setBounds (bar.removeFromRight (180).withTrimmedTop (-4).withTrimmedBottom (-4));
    bar.removeFromRight (8);
    reloadBtn.setBounds (bar.removeFromRight (64)); bar.removeFromRight (4);
    midiBtn.setBounds (bar.removeFromRight (68)); bar.removeFromRight (4);
    latency.setBounds (bar.removeFromRight (112)); bar.removeFromRight (6);
    // NUDGE and the status line need ~460 px; on a narrower window they move to a strip under the bar
    // (squeezed into the bar they would vanish: the status at the minimum width had no room at all)
    if (bar.getWidth() < 460)
    {
        bar = r.removeFromTop (stripHeight).reduced (10, 2);
        bar.removeFromLeft (166);
    }
    auto off = bar.removeFromRight (150);
    offsetLabel.setBounds (off.removeFromLeft (42)); offset.setBounds (off);
    bar.removeFromRight (6); bar.removeFromLeft (10);
    status.setBounds (bar);
    if (auto* w = proc.getBrowser()) w->setBounds (r);
}
