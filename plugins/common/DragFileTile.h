// A tile you drag out of the plugin window and drop onto an Ableton track (or the desktop / browser):
// it performs a native, external drag of a file on disk.
#pragma once
#include "DatLookAndFeel.h"

namespace dat
{
class DragFileTile : public juce::Component, public juce::SettableTooltipClient
{
public:
    DragFileTile() { setMouseCursor (juce::MouseCursor::DraggingHandCursor); setTooltip ("Drag the result onto an Ableton track, clip slot or the browser"); }

    void setFile (const juce::File& f, juce::String caption = {})
    {
        file = f; sub = caption;
        setMouseCursor (file.existsAsFile() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
    juce::File getFile() const { return file; }
    void setAccent (juce::Colour c) { accent = c; repaint(); }
    void setEmptyText (juce::String title, juce::String subtitle) { emptyTitle = std::move (title); emptySub = std::move (subtitle); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        bool ok = file.existsAsFile();
        g.setColour (ok ? accent.withAlpha (dragging ? 0.45f : (hover ? 0.32f : 0.2f)) : col::panel2); g.fillRoundedRectangle (r, 6);
        juce::Path dash; dash.addRoundedRectangle (r, 6);
        juce::PathStrokeType st (1.5f); float d[] = { 5.0f, 4.0f }; juce::Path dashed; st.createDashedStroke (dashed, dash, d, 2);
        g.setColour (ok ? accent : col::line); g.fillPath (dashed);
        const bool compact = r.getHeight() < 60;
        auto ic = compact ? r.removeFromLeft (r.getHeight()) : r.removeFromTop (r.getHeight() * 0.55f);
        // drag arrow icon
        juce::Path arrow; float cx = ic.getCentreX(), cy = ic.getCentreY() + 2, s = juce::jmin (ic.getWidth(), ic.getHeight()) * 0.28f;
        arrow.startNewSubPath (cx - s, cy); arrow.lineTo (cx + s, cy); arrow.startNewSubPath (cx + s * 0.4f, cy - s * 0.6f); arrow.lineTo (cx + s, cy); arrow.lineTo (cx + s * 0.4f, cy + s * 0.6f);
        g.setColour (ok ? col::text : col::dim); g.strokePath (arrow, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setFont (LookAndFeel::font (12.0f, true));
        if (compact) r = r.withTrimmedTop (r.getHeight() * 0.5f - 15);
        g.drawFittedText (ok ? juce::String ("DRAG TO ABLETON") : emptyTitle, r.removeFromTop (16).toNearestInt(), juce::Justification::centred, 1);
        g.setColour (col::dim); g.setFont (LookAndFeel::font (10.5f));
        g.drawFittedText (ok ? (sub.isNotEmpty() ? sub : file.getFileName()) : emptySub, r.toNearestInt().reduced (4, 0), juce::Justification::centredTop, 2);
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || ! file.existsAsFile() || e.getDistanceFromDragStart() < 4) return;
        dragging = true; repaint();
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this, [sp = juce::Component::SafePointer<DragFileTile> (this)]
        {
            if (sp != nullptr) { sp->dragging = false; sp->repaint(); }
        });
    }
    void mouseUp (const juce::MouseEvent&) override { dragging = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (file.existsAsFile()) file.revealToUser(); }

private:
    juce::File file; juce::String sub, emptyTitle { "NO RESULT YET" }, emptySub { "process something first" }; juce::Colour accent = col::lab;
    bool hover = false, dragging = false;
};
}
