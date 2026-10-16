#pragma once
#include <set>
#include "PluginProcessor.h"
#include "../../common/DatLookAndFeel.h"
#include "../../common/DragFileTile.h"

// Waveform of a sample; SOURCE view has draggable region handles, RESULT view drags the file out to Ableton
class WaveView : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void (double, double)> onRegion;   // source region changed
    std::function<juce::File()> dragFile;            // result: file to drag out
    std::function<void (const juce::File&)> onDropFile;
    void setSample (const LabSample* s, juce::Colour c, juce::String title);
    void setRegion (double a, double b) { ra = a; rb = b; repaint(); }
    void setPlayhead (float f) { if (std::abs (f - ph) > 0.001f) { ph = f; repaint(); } }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { drag = 0; dragging = false; }
    void mouseMove (const juce::MouseEvent&) override;
    bool showRegion = false;
private:
    void rebuild();
    const LabSample* sample = nullptr; size_t cachedLen = 0; const void* cachedPtr = nullptr;
    std::vector<std::pair<float, float>> peaks[2];
    juce::Colour colour; juce::String title;
    double ra = 0, rb = 1; float ph = -1; int drag = 0; bool dragging = false;
};

// Knob with label (or a combo box for option parameters)
class ParamControl : public juce::Component
{
public:
    ParamControl (const lab::ParamDef& d, double value, std::function<void (double)> changed);
    void resized() override;
    void paint (juce::Graphics&) override;
    void setValue (double v);
private:
    lab::ParamDef def;
    juce::Slider knob; juce::ComboBox combo;
    std::function<void (double)> onChange;
};

// Process list, grouped by category
class ProcList : public juce::Component
{
public:
    ProcList();
    std::function<void (const juce::String&)> onSelect;
    std::function<void()> onLayoutChange;   // rows were added / removed (the list's height changed)
    juce::String selected;
    void setFilter (const juce::String& text);
    void reveal (const juce::String& id);   // expands the category of id
    int idealHeight() const;
    int rowTop (const juce::String& id) const;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
private:
    struct Row { bool header; juce::String text, id, cat; bool isNew; int count; };
    std::vector<Row> rowCache;
    juce::StringArray filterWords;
    std::set<juce::String> collapsed;
    void rebuild();
    int hover = -1;
    static constexpr int rowH = 21, headH = 26;
    int rowAt (int y) const;
};

class DatLabEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::ChangeListener, private juce::Timer
{
public:
    explicit DatLabEditor (DatLabProcessor&);
    ~DatLabEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dropHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropHover = false; repaint(); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override;
    void refresh();
    void buildParams();
    void selectProc (const juce::String& id);
    void loadDialog (bool aux);
    void layoutParams();

    DatLabProcessor& proc;
    dat::LookAndFeel lnf;
    juce::TooltipWindow tips { this, 600 };

    juce::TextButton loadBtn { "LOAD" }, captureBtn { "CAPTURE" }, bpmBtn { "HOST" }, thruBtn { "THRU" };
    juce::Slider bpmSlider;
    juce::TextEditor search; ProcList procList; juce::Viewport procView;
    juce::Label procName, procDesc;
    juce::OwnedArray<ParamControl> params;
    juce::Component paramArea; juce::Viewport paramView;
    juce::Label cdpLabel;
    juce::TextButton auxBtn { "LOAD 2ND SAMPLE..." }, auxFromResult { "USE RESULT" };
    juce::Label auxLabel;

    // SHAPE section (applies to every process)
    juce::OwnedArray<ParamControl> shapeCtl;
    juce::ToggleButton revIn { "REV IN" }, revOut { "REV OUT" }, trim { "TRIM" }, norm { "NORMALIZE" };

    WaveView srcWave, resWave;
    dat::DragFileTile dragTile;
    juce::TextButton processBtn { "PROCESS" }, playSrc { "PLAY SRC" }, playRes { "PLAY RESULT" }, stopBtn { "STOP" }, loopBtn { "LOOP" },
                     randBtn { "RANDOM" }, resetBtn { "RESET" }, chainBtn { "CHAIN" }, undoBtn { "UNDO" }, saveBtn { "SAVE..." }, folderBtn { "FOLDER" };
    double progressValue = 0; juce::ProgressBar progressBar { progressValue };
    juce::Label statusLabel;
    bool dropHover = false;
    juce::Rectangle<int> auxZone, shapeArea;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String builtFor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DatLabEditor)
};
