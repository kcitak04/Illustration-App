#include "PluginEditor.h"

using namespace dat;

static juce::String U (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
static juce::String U (const char* s) { return juce::String::fromUTF8 (s); }

// ======================= WaveView =======================
void WaveView::setSample (const LabSample* s, juce::Colour c, juce::String t)
{
    colour = c; title = std::move (t);
    sample = s && ! s->empty() ? s : nullptr;
    const void* ptr = sample ? (const void*) sample->ch[0].data() : nullptr;
    size_t len = sample ? sample->ch[0].size() : 0;
    if (ptr != cachedPtr || len != cachedLen) { cachedPtr = ptr; cachedLen = len; rebuild(); }
    repaint();
}

void WaveView::rebuild()
{
    for (auto& p : peaks) p.clear();
    if (! sample) return;
    const int W = 1200; const size_t len = sample->ch[0].size();
    for (size_t c = 0; c < juce::jmin<size_t> (2, sample->ch.size()); ++c)
    {
        const auto& x = sample->ch[c];
        for (int i = 0; i < W; ++i)
        {
            size_t a = len * (size_t) i / W, b = juce::jmax (a + 1, len * (size_t) (i + 1) / W);
            float lo = 0, hi = 0; for (size_t k = a; k < b && k < len; ++k) { lo = juce::jmin (lo, x[k]); hi = juce::jmax (hi, x[k]); }
            peaks[c].push_back ({ lo, hi });
        }
    }
}

void WaveView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (col::panel); g.fillRoundedRectangle (r, 5);
    g.setColour (col::line); g.drawRoundedRectangle (r.reduced (0.5f), 5, 1);
    auto inner = r.reduced (4);
    if (! sample)
    {
        g.setColour (col::dim); g.setFont (LookAndFeel::font (12));
        g.drawFittedText (title + (showRegion ? U ("  \xc2\xb7  drop an audio file here, LOAD one or CAPTURE the track") : U ("  \xc2\xb7  empty")), inner.toNearestInt(), juce::Justification::centred, 2);
        return;
    }
    const int nch = peaks[1].empty() ? 1 : 2;
    for (int c = 0; c < nch; ++c)
    {
        auto lane = inner.withHeight (inner.getHeight() / (float) nch).withY (inner.getY() + inner.getHeight() / (float) nch * (float) c);
        float mid = lane.getCentreY(), hh = lane.getHeight() / 2;
        juce::Path p; const auto& pk = peaks[c];
        for (size_t i = 0; i < pk.size(); ++i)
        {
            float x = lane.getX() + lane.getWidth() * (float) i / (float) pk.size();
            p.addRectangle (x, mid - pk[i].second * hh, juce::jmax (1.0f, lane.getWidth() / (float) pk.size()), juce::jmax (0.5f, (pk[i].second - pk[i].first) * hh));
        }
        g.setColour (colour.withAlpha (0.85f)); g.fillPath (p);
    }
    if (showRegion)
    {
        float xa = inner.getX() + (float) ra * inner.getWidth(), xb = inner.getX() + (float) rb * inner.getWidth();
        g.setColour (col::bg.withAlpha (0.6f));
        g.fillRect (inner.withRight (xa)); g.fillRect (inner.withLeft (xb));
        g.setColour (col::text);
        g.fillRect (xa - 1, inner.getY(), 2.0f, inner.getHeight()); g.fillRect (xb - 1, inner.getY(), 2.0f, inner.getHeight());
        g.fillRect (xa - 1, inner.getY(), 8.0f, 8.0f); g.fillRect (xb - 7, inner.getY(), 8.0f, 8.0f);
    }
    if (ph >= 0 && ph <= 1)
    {
        float x0 = showRegion ? inner.getX() + (float) ra * inner.getWidth() : inner.getX(), w = showRegion ? (float) (rb - ra) * inner.getWidth() : inner.getWidth();
        g.setColour (col::text); g.fillRect (x0 + ph * w, inner.getY(), 1.5f, inner.getHeight());
    }
    g.setColour (col::text.withAlpha (0.9f)); g.setFont (LookAndFeel::font (11.5f, true));
    g.drawText (title, inner.toNearestInt().reduced (4, 2), juce::Justification::topLeft);
    g.setColour (col::dim); g.setFont (LookAndFeel::font (11));
    g.drawText (juce::String::formatted ("%.2f s  %d Hz  %s", sample->seconds(), (int) sample->sr, sample->ch.size() > 1 ? "stereo" : "mono"), inner.toNearestInt().reduced (4, 2), juce::Justification::topRight);
}

void WaveView::mouseMove (const juce::MouseEvent& e)
{
    if (showRegion && sample)
    {
        auto inner = getLocalBounds().toFloat().reduced (4);
        float xa = inner.getX() + (float) ra * inner.getWidth(), xb = inner.getX() + (float) rb * inner.getWidth();
        setMouseCursor (std::abs ((float) e.x - xa) < 7 || std::abs ((float) e.x - xb) < 7 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
    else setMouseCursor (dragFile && dragFile().existsAsFile() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void WaveView::mouseDown (const juce::MouseEvent& e)
{
    if (! showRegion || ! sample) return;
    auto inner = getLocalBounds().toFloat().reduced (4);
    float xa = inner.getX() + (float) ra * inner.getWidth(), xb = inner.getX() + (float) rb * inner.getWidth();
    drag = std::abs ((float) e.x - xa) <= std::abs ((float) e.x - xb) ? 1 : 2;
    mouseDrag (e);
}

void WaveView::mouseDrag (const juce::MouseEvent& e)
{
    if (! showRegion)
    {   // result: drag the rendered file out of the plugin
        if (dragging || ! dragFile || e.getDistanceFromDragStart() < 4) return;
        auto f = dragFile(); if (! f.existsAsFile()) return;
        dragging = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
        return;
    }
    if (! drag) return;
    auto inner = getLocalBounds().toFloat().reduced (4);
    double u = juce::jlimit (0.0, 1.0, (double) ((float) e.x - inner.getX()) / inner.getWidth());
    if (drag == 1) ra = juce::jmin (u, rb - 0.002); else rb = juce::jmax (u, ra + 0.002);
    repaint();
    if (onRegion) onRegion (ra, rb);
}

// ======================= ParamControl =======================
ParamControl::ParamControl (const lab::ParamDef& d, double value, std::function<void (double)> changed) : def (d), onChange (std::move (changed))
{
    if (def.isOptions())
    {
        for (size_t i = 0; i < def.options.size(); ++i) combo.addItem (U (def.options[i]), (int) i + 1);
        combo.setSelectedId ((int) std::round (value) + 1, juce::dontSendNotification);
        combo.onChange = [this] { onChange ((double) combo.getSelectedId() - 1); };
        addAndMakeVisible (combo);
    }
    else
    {
        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 16);
        knob.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        knob.setRange (def.min, def.max, def.isInt() ? 1.0 : 0.0);
        if (def.flags & lab::PExp) { if (def.min > 0) knob.setSkewFactorFromMidPoint (std::sqrt (def.min * def.max)); else knob.setSkewFactor (0.4); }
        knob.setDoubleClickReturnValue (true, def.def);
        knob.textFromValueFunction = [this] (double v) { return U (lab::formatValue (def, v)); };
        knob.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("-0123456789.").getDoubleValue(); };
        knob.setValue (value, juce::dontSendNotification);
        knob.onValueChange = [this] { onChange (knob.getValue()); };
        knob.setPopupDisplayEnabled (false, false, nullptr);
        knob.setTooltip (U (def.label) + "  (double-click: default " + U (lab::formatValue (def, def.def)) + ")");
        knob.updateText();
        addAndMakeVisible (knob);
    }
}
void ParamControl::setValue (double v)
{
    if (def.isOptions()) combo.setSelectedId ((int) std::round (v) + 1, juce::dontSendNotification);
    else knob.setValue (v, juce::dontSendNotification);
}
void ParamControl::resized()
{
    auto r = getLocalBounds(); r.removeFromTop (16);
    if (def.isOptions()) combo.setBounds (r.withSizeKeepingCentre (r.getWidth() - 4, 26).withY (r.getY() + 18));
    else knob.setBounds (r);
}
void ParamControl::paint (juce::Graphics& g)
{
    g.setColour (col::dim); g.setFont (LookAndFeel::font (10.5f, true));
    g.drawFittedText (U (def.label), getLocalBounds().removeFromTop (16), juce::Justification::centred, 1);
}

// ======================= ProcList =======================
std::vector<ProcList::Row> ProcList::rows() const
{
    std::vector<Row> v;
    for (auto& [cat, ids] : lab::categories())
    {
        v.push_back ({ true, U (cat), {}, false });
        for (auto& id : ids) { auto* d = lab::findProc (id); v.push_back ({ false, U (d->name), U (d->id), d->isNew }); }
    }
    return v;
}
int ProcList::idealHeight() const { int h = 6; for (auto& r : rows()) h += r.header ? headH : rowH; return h; }
int ProcList::rowAt (int y) const
{
    int t = 4, i = 0;
    for (auto& r : rows()) { int h = r.header ? headH : rowH; if (y >= t && y < t + h) return r.header ? -1 : i; t += h; ++i; }
    return -1;
}
void ProcList::paint (juce::Graphics& g)
{
    g.fillAll (col::panel);
    int y = 4, i = 0;
    for (auto& r : rows())
    {
        if (r.header)
        {
            g.setColour (r.text.startsWith ("NEW") ? col::accent : col::dim); g.setFont (LookAndFeel::font (10.5f, true));
            g.drawText (r.text, 10, y + 8, getWidth() - 14, headH - 8, juce::Justification::centredLeft);
            y += headH;
        }
        else
        {
            bool sel = r.id == selected;
            auto rr = juce::Rectangle<int> (5, y, getWidth() - 10, rowH - 2);
            if (sel) { g.setColour (col::lab); g.fillRoundedRectangle (rr.toFloat(), 3); }
            else if (i == hover) { g.setColour (col::panel2); g.fillRoundedRectangle (rr.toFloat(), 3); }
            g.setColour (sel ? col::bg : col::text); g.setFont (LookAndFeel::font (12.0f, sel));
            g.drawText (r.text, rr.reduced (8, 0), juce::Justification::centredLeft);
            y += rowH;
        }
        ++i;
    }
}
void ProcList::mouseMove (const juce::MouseEvent& e) { int h = rowAt (e.y); if (h != hover) { hover = h; repaint(); } }
void ProcList::mouseDown (const juce::MouseEvent& e)
{
    int i = rowAt (e.y); if (i < 0) return;
    auto r = rows()[(size_t) i];
    selected = r.id; repaint();
    if (onSelect) onSelect (r.id);
}

// ======================= Editor =======================
namespace
{
lab::ParamDef sp (const char* id, const char* label, double mn, double mx, double def, int flags, lab::Fmt fmt, const char* unit = "")
{
    lab::ParamDef d; d.id = id; d.label = label; d.min = mn; d.max = mx; d.def = def; d.flags = flags; d.fmt = fmt; d.unit = unit; return d;
}
const int knobW = 80, knobH = 94;
}

DatLabEditor::DatLabEditor (DatLabProcessor& p) : AudioProcessorEditor (p), proc (p)
{
    setLookAndFeel (&lnf);
    auto btn = [this] (juce::Button& b, std::function<void()> f, juce::String tip = {}) { addAndMakeVisible (b); b.onClick = std::move (f); if (tip.isNotEmpty()) b.setTooltip (tip); };

    btn (loadBtn, [this] { loadDialog (false); }, "Load a WAV / AIFF / FLAC / OGG / MP3 as the source (or just drop a file anywhere on the window)");
    btn (captureBtn, [this] { if (proc.isCapturing()) proc.stopCapture(); else proc.startCapture(); refresh(); }, "Record what plays into this track; press again to stop. The take becomes the source.");
    btn (thruBtn, [this] { proc.thru = ! proc.thru.load(); refresh(); }, "Pass the track's audio through the plugin (off: only the preview is heard)");
    btn (bpmBtn, [this] { proc.bpmFromHost = ! proc.bpmFromHost; refresh(); }, "Tempo for the synced processes: HOST follows Ableton, otherwise set it with the slider");
    bpmSlider.setSliderStyle (juce::Slider::LinearHorizontal); bpmSlider.setRange (40, 250, 0.01); bpmSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
    bpmSlider.setValue (proc.manualBpm, juce::dontSendNotification); bpmSlider.onValueChange = [this] { proc.manualBpm = bpmSlider.getValue(); };
    addAndMakeVisible (bpmSlider);

    procList.onSelect = [this] (const juce::String& id) { selectProc (id); };
    procView.setViewedComponent (&procList, false); procView.setScrollBarsShown (true, false); procView.setScrollBarThickness (8);
    addAndMakeVisible (procView);

    procName.setFont (LookAndFeel::font (19, true)); procName.setColour (juce::Label::textColourId, col::lab); addAndMakeVisible (procName);
    procDesc.setFont (LookAndFeel::font (12.5f)); procDesc.setColour (juce::Label::textColourId, col::dim); procDesc.setJustificationType (juce::Justification::topLeft);
    procDesc.setMinimumHorizontalScale (1.0f); addAndMakeVisible (procDesc);
    addAndMakeVisible (paramArea);
    btn (auxBtn, [this] { loadDialog (true); }, "The second sample this process needs (or drop a file on this row)");
    btn (auxFromResult, [this] { if (! proc.result.empty()) { proc.aux = proc.result; proc.status = "The result is now the second sample"; refresh(); } }, "Use the current result as the second sample");
    auxLabel.setFont (LookAndFeel::font (12)); auxLabel.setColour (juce::Label::textColourId, col::text); addAndMakeVisible (auxLabel);

    // SHAPE controls
    struct S { lab::ParamDef d; double* v; };
    auto& sh = proc.shape;
    std::vector<S> sdefs {
        { sp ("in", "IN GAIN", -24, 24, 0, 0, lab::Fmt::db), &sh.inGain },
        { sp ("mix", "DRY/WET", 0, 1, 1, lab::PPct, lab::Fmt::pct), &sh.mix },
        { sp ("lc", "LOW CUT", 20, 2000, 20, lab::PExp, lab::Fmt::hz), &sh.lowCut },
        { sp ("hc", "HIGH CUT", 500, 20000, 20000, lab::PExp, lab::Fmt::hz), &sh.highCut },
        { sp ("w", "WIDTH", 0, 2, 1, 0, lab::Fmt::pct), &sh.width },
        { sp ("fi", "FADE IN", 0, 2000, 0, lab::PExp, lab::Fmt::unit, "ms"), &sh.fadeIn },
        { sp ("fo", "FADE OUT", 0, 4000, 5, lab::PExp, lab::Fmt::unit, "ms"), &sh.fadeOut },
        { sp ("lx", "LOOP XFADE", 0, 2000, 0, lab::PExp, lab::Fmt::unit, "ms"), &sh.loopXfade },
        { sp ("og", "OUT GAIN", -24, 12, 0, 0, lab::Fmt::db), &sh.outGain },
        { sp ("ml", "MAX LENGTH", 1, 180, 180, lab::PExp, lab::Fmt::unit, "s"), &sh.maxLen } };
    for (auto& s : sdefs)
    {
        auto* v = s.v;
        auto* c = shapeCtl.add (new ParamControl (s.d, *v, [v] (double x) { *v = x; }));
        addAndMakeVisible (c);
    }
    auto tog = [this] (juce::ToggleButton& b, bool* v, juce::String tip) { addAndMakeVisible (b); b.setToggleState (*v, juce::dontSendNotification); b.onClick = [&b, v] { *v = b.getToggleState(); }; b.setTooltip (tip); };
    tog (revIn, &sh.revIn, "Reverse the source before processing"); tog (revOut, &sh.revOut, "Reverse the result");
    tog (trim, &sh.trim, "Cut silence at both ends of the result"); tog (norm, &sh.normalize, "Normalize the result to -0.2 dBFS");

    srcWave.showRegion = true; srcWave.setTooltip ("Source: drag the white handles to process only part of it");
    srcWave.onRegion = [this] (double a, double b) { proc.shape.start = a; proc.shape.end = b; };
    addAndMakeVisible (srcWave);
    resWave.setTooltip ("Result: drag it straight onto an Ableton track");
    resWave.dragFile = [this] { return proc.result.file; };
    addAndMakeVisible (resWave);
    addAndMakeVisible (dragTile);

    processBtn.setColour (juce::TextButton::buttonColourId, col::lab.darker (0.35f));
    btn (processBtn, [this] { if (proc.isBusy()) proc.cancelProcessing(); else proc.startProcessing(); refresh(); }, "Render the process (click again to cancel)");
    btn (playSrc, [this] { proc.play (DatLabProcessor::Play::source); });
    btn (playRes, [this] { proc.play (DatLabProcessor::Play::result); });
    btn (stopBtn, [this] { proc.stopPlay(); });
    btn (loopBtn, [this] { proc.loopPreview = ! proc.loopPreview.load(); refresh(); }, "Loop the preview");
    btn (randBtn, [this] { proc.randomize(); }, "Random settings for this process");
    btn (resetBtn, [this] { proc.resetParams(); }, "Default settings for this process");
    btn (chainBtn, [this] { proc.chainResult(); }, "Use the result as the new source, to stack processes");
    btn (undoBtn, [this] { proc.undoChain(); }, "Go back to the previous source");
    btn (saveBtn, [this]
    {
        if (proc.result.empty()) return;
        chooser = std::make_unique<juce::FileChooser> ("Save result", proc.result.file, "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult(); if (f == juce::File()) return;
            if (proc.result.file.copyFileTo (f.withFileExtension ("wav"))) proc.status = "Saved " + f.getFullPathName();
            refresh();
        });
    }, "Save a copy of the result somewhere else");
    btn (folderBtn, [this] { proc.renderFolder().revealToUser(); }, "Open the folder with all renders (Music/DATLAB)");
    addAndMakeVisible (progressBar); progressBar.setPercentageDisplay (true);
    statusLabel.setFont (LookAndFeel::font (12)); statusLabel.setColour (juce::Label::textColourId, col::dim); addAndMakeVisible (statusLabel);

    setResizable (true, true);
    setResizeLimits (980, 690, 2000, 1400);
    setSize (1140, 760);
    proc.addChangeListener (this);
    procList.selected = proc.procId;
    refresh();
    startTimerHz (30);
}

DatLabEditor::~DatLabEditor() { proc.removeChangeListener (this); setLookAndFeel (nullptr); }

void DatLabEditor::selectProc (const juce::String& id)
{
    proc.procId = id;
    refresh();
}

void DatLabEditor::buildParams()
{
    params.clear();
    const auto& d = proc.proc();
    auto& vals = proc.paramsFor (proc.procId);
    for (auto& pd : d.params)
    {
        auto key = pd.id;
        auto* c = params.add (new ParamControl (pd, vals[key], [this, key] (double v) { proc.paramsFor (proc.procId).v[key] = v; }));
        paramArea.addAndMakeVisible (c);
    }
    builtFor = proc.procId;
    layoutParams();
}

void DatLabEditor::refresh()
{
    const auto& d = proc.proc();
    procList.selected = proc.procId; procList.repaint();
    procName.setText (U (d.name) + (d.isNew ? U ("   \xc2\xb7 NEW") : juce::String()), juce::dontSendNotification);
    procDesc.setText (U (d.desc), juce::dontSendNotification);
    if (builtFor != proc.procId) buildParams();
    else { auto& vals = proc.paramsFor (proc.procId); for (size_t i = 0; i < d.params.size() && i < (size_t) params.size(); ++i) params[(int) i]->setValue (vals[d.params[i].id]); }

    bool needsAux = ! d.aux.empty();
    auxBtn.setVisible (needsAux); auxFromResult.setVisible (needsAux); auxLabel.setVisible (needsAux);
    auxBtn.setButtonText ("LOAD " + U (d.aux) + "...");
    auxLabel.setText (proc.aux.empty() ? "no " + U (d.aux) + " sample yet (drop one on this row)" : U (d.aux) + ": " + proc.aux.name + juce::String::formatted ("  %.2f s", proc.aux.seconds()), juce::dontSendNotification);

    auto& sh = proc.shape; double sv[] = { sh.inGain, sh.mix, sh.lowCut, sh.highCut, sh.width, sh.fadeIn, sh.fadeOut, sh.loopXfade, sh.outGain, sh.maxLen };
    for (int i = 0; i < shapeCtl.size(); ++i) shapeCtl[i]->setValue (sv[i]);
    revIn.setToggleState (sh.revIn, juce::dontSendNotification); revOut.setToggleState (sh.revOut, juce::dontSendNotification);
    trim.setToggleState (sh.trim, juce::dontSendNotification); norm.setToggleState (sh.normalize, juce::dontSendNotification);

    srcWave.setSample (&proc.source, col::text.withAlpha (0.75f), "SOURCE  " + proc.source.name);
    srcWave.setRegion (sh.start, sh.end);
    resWave.setSample (&proc.result, col::good, "RESULT  " + proc.result.name);
    dragTile.setFile (proc.result.file, proc.result.file.getFileName());
    dragTile.setAccent (col::good);

    bool busy = proc.isBusy();
    processBtn.setButtonText (busy ? "CANCEL" : "PROCESS");
    captureBtn.setButtonText (proc.isCapturing() ? "STOP CAPTURE" : "CAPTURE");
    captureBtn.setColour (juce::TextButton::buttonColourId, proc.isCapturing() ? col::warn : col::panel2);
    thruBtn.setToggleState (proc.thru.load(), juce::dontSendNotification);
    loopBtn.setToggleState (proc.loopPreview.load(), juce::dontSendNotification);
    bpmBtn.setToggleState (proc.bpmFromHost, juce::dontSendNotification);
    bpmSlider.setEnabled (! proc.bpmFromHost);
    chainBtn.setEnabled (! proc.result.empty()); undoBtn.setEnabled (! proc.history.empty());
    playRes.setEnabled (! proc.result.empty()); playSrc.setEnabled (! proc.source.empty());
    saveBtn.setEnabled (! proc.result.empty());
    statusLabel.setText (proc.status, juce::dontSendNotification);
    resized();
    repaint();
}

void DatLabEditor::timerCallback()
{
    progressValue = proc.isBusy() ? (double) proc.getProgress() : 0.0;
    progressBar.setVisible (proc.isBusy());
    auto np = proc.nowPlaying();
    srcWave.setPlayhead (np == DatLabProcessor::Play::source ? proc.playPosition() : -1.0f);
    resWave.setPlayhead (np == DatLabProcessor::Play::result ? proc.playPosition() : -1.0f);
    if (proc.isCapturing()) statusLabel.setText (juce::String::formatted ("Capturing... %.1f s  (press STOP CAPTURE)", proc.captureSeconds()), juce::dontSendNotification);
    if (proc.bpmFromHost && proc.hostBpm.load() > 0 && std::abs (bpmSlider.getValue() - proc.hostBpm.load()) > 0.005) bpmSlider.setValue (proc.hostBpm.load(), juce::dontSendNotification);
}

void DatLabEditor::layoutParams()
{
    const int perRow = juce::jmax (1, paramArea.getWidth() / knobW);
    for (int i = 0; i < params.size(); ++i) params[i]->setBounds ((i % perRow) * knobW, (i / perRow) * knobH, knobW - 4, knobH);
}

void DatLabEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);
    auto top = getLocalBounds().removeFromTop (46);
    g.setColour (col::panel); g.fillRect (top);
    g.setColour (col::lab); g.setFont (LookAndFeel::font (21, true));
    g.drawText (juce::CharPointer_UTF8 ("\xe2\x9a\x97 DATLAB"), 14, 0, 160, 46, juce::Justification::centredLeft);
    g.setColour (col::dim); g.setFont (LookAndFeel::font (10.5f));
    g.drawText (U ("SAMPLE LAB  \xc2\xb7  ") + juce::String ((int) lab::procs().size()) + " PROCESSES", 132, 0, 200, 46, juce::Justification::centredLeft);
    if (! shapeArea.isEmpty())
    {
        g.setColour (col::panel); g.fillRoundedRectangle (shapeArea.toFloat(), 6);
        g.setColour (col::accent); g.setFont (LookAndFeel::font (11, true));
        g.drawText (U ("SHAPE  \xc2\xb7  APPLIES TO EVERY PROCESS"), shapeArea.reduced (10, 4).removeFromTop (16), juce::Justification::left);
    }
    if (auxBtn.isVisible() && ! auxZone.isEmpty())
    {
        g.setColour (col::panel); g.fillRoundedRectangle (auxZone.toFloat(), 5);
        g.setColour (col::lab.withAlpha (0.5f)); g.drawRoundedRectangle (auxZone.toFloat().reduced (0.5f), 5, 1);
    }
    if (dropHover) { g.setColour (col::lab.withAlpha (0.12f)); g.fillAll(); g.setColour (col::lab); g.drawRect (getLocalBounds(), 3); }
}

void DatLabEditor::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (46).reduced (10, 8);
    top.removeFromLeft (300);
    thruBtn.setBounds (top.removeFromRight (64)); top.removeFromRight (8);
    bpmSlider.setBounds (top.removeFromRight (170)); top.removeFromRight (4);
    bpmBtn.setBounds (top.removeFromRight (90)); top.removeFromRight (16);
    captureBtn.setBounds (top.removeFromRight (120)); top.removeFromRight (6);
    loadBtn.setBounds (top.removeFromRight (80));
    bpmBtn.setButtonText (proc.bpmFromHost ? "BPM: HOST" : "BPM: MANUAL");

    r.reduce (10, 8);
    auto left = r.removeFromLeft (232); r.removeFromLeft (12);
    procView.setBounds (left);
    procList.setSize (left.getWidth() - 10, procList.idealHeight());

    // bottom: status, buttons, waves
    auto status = r.removeFromBottom (22);
    statusLabel.setBounds (status.withTrimmedRight (210)); progressBar.setBounds (status.removeFromRight (200));
    r.removeFromBottom (6);
    auto buttons = r.removeFromBottom (34);
    processBtn.setBounds (buttons.removeFromLeft (120)); buttons.removeFromLeft (8);
    for (auto* b : { &playSrc, &playRes, &stopBtn, &loopBtn }) { b->setBounds (buttons.removeFromLeft (b == &playRes ? 100 : (b == &playSrc ? 80 : 54))); buttons.removeFromLeft (4); }
    buttons.removeFromLeft (10);
    for (auto* b : { &chainBtn, &undoBtn, &randBtn, &resetBtn }) { b->setBounds (buttons.removeFromLeft (66)); buttons.removeFromLeft (4); }
    buttons.removeFromLeft (6);
    const int fw = juce::jmin (72, juce::jmax (40, (buttons.getWidth() - 4) / 2));
    folderBtn.setBounds (buttons.removeFromRight (fw)); buttons.removeFromRight (4);
    saveBtn.setBounds (buttons.removeFromRight (fw));
    r.removeFromBottom (8);
    auto waves = r.removeFromBottom (190);
    auto tile = waves.removeFromRight (160); waves.removeFromRight (8);
    srcWave.setBounds (waves.removeFromTop (91)); waves.removeFromTop (8); resWave.setBounds (waves);
    dragTile.setBounds (tile.withTrimmedTop (99));
    r.removeFromBottom (8);

    // shape section
    shapeArea = r.removeFromBottom (knobH + 26);
    auto sa = shapeArea.reduced (8, 4); sa.removeFromTop (18);
    auto togs = sa.removeFromRight (110);
    int ty = togs.getY();
    for (auto* t : { &revIn, &revOut, &trim, &norm }) { t->setBounds (togs.getX(), ty, 104, 22); ty += 25; }
    int per = juce::jmax (1, sa.getWidth() / shapeCtl.size());
    for (int i = 0; i < shapeCtl.size(); ++i) shapeCtl[i]->setBounds (sa.getX() + i * per, sa.getY(), juce::jmin (per - 2, knobW), knobH - 6);
    r.removeFromBottom (8);

    // process header + params
    procName.setBounds (r.removeFromTop (28));
    procDesc.setBounds (r.removeFromTop (40));
    r.removeFromTop (4);
    if (auxBtn.isVisible())
    {
        auxZone = r.removeFromBottom (34);
        auto az = auxZone.reduced (5, 4);
        auxBtn.setBounds (az.removeFromLeft (190)); az.removeFromLeft (6);
        auxFromResult.setBounds (az.removeFromLeft (100)); az.removeFromLeft (10);
        auxLabel.setBounds (az);
        r.removeFromBottom (6);
    }
    else auxZone = {};
    paramArea.setBounds (r);
    layoutParams();
}

bool DatLabEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;m4a;caf")) return true;
    return false;
}

void DatLabEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropHover = false;
    bool asAux = ! auxZone.isEmpty() && auxBtn.isVisible() && auxZone.contains (x, y);
    juce::String err;
    if (! proc.loadFile (juce::File (files[0]), asAux, err)) proc.status = err;
    refresh();
}

void DatLabEditor::loadDialog (bool aux)
{
    chooser = std::make_unique<juce::FileChooser> (aux ? "Second sample" : "Source sample", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, aux] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult(); if (! f.existsAsFile()) return;
        juce::String err; if (! proc.loadFile (f, aux, err)) proc.status = err;
        refresh();
    });
}
