// Shared dark look for the DATTRACK plugins (DATLAB + DATTRACK host).
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace dat
{
namespace col
{
    const juce::Colour bg       { 0xff141417 };
    const juce::Colour panel    { 0xff1e1e23 };
    const juce::Colour panel2   { 0xff26262d };
    const juce::Colour line     { 0xff34343d };
    const juce::Colour text     { 0xffe9e9ef };
    const juce::Colour dim      { 0xff8b8b98 };
    const juce::Colour lab      { 0xffc38bff };   // Sample Lab purple (as in DATTRACK)
    const juce::Colour accent   { 0xffff9a3c };   // DATTRACK Studio orange
    const juce::Colour good     { 0xff6ee7a8 };
    const juce::Colour warn     { 0xffff5f6d };
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit LookAndFeel (juce::Colour accentColour = col::lab) : acc (accentColour)
    {
        setColourScheme ({ col::bg, col::panel, col::panel2, col::line, col::text, col::panel2, col::text, acc, col::text });   // last = menu text: readable on the dark menu
        setColour (juce::ResizableWindow::backgroundColourId, col::bg);
        setColour (juce::Slider::rotarySliderFillColourId, acc);
        setColour (juce::Slider::rotarySliderOutlineColourId, col::line);
        setColour (juce::Slider::thumbColourId, col::text);
        setColour (juce::Slider::textBoxTextColourId, col::text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, col::text);
        setColour (juce::TextButton::buttonColourId, col::panel2);
        setColour (juce::TextButton::buttonOnColourId, acc);
        setColour (juce::TextButton::textColourOffId, col::text);
        setColour (juce::TextButton::textColourOnId, col::bg);
        setColour (juce::ComboBox::backgroundColourId, col::panel2);
        setColour (juce::ComboBox::outlineColourId, col::line);
        setColour (juce::ComboBox::textColourId, col::text);
        setColour (juce::ComboBox::arrowColourId, acc);
        setColour (juce::PopupMenu::backgroundColourId, col::panel);
        setColour (juce::PopupMenu::textColourId, col::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, acc);
        setColour (juce::PopupMenu::highlightedTextColourId, col::bg);
        setColour (juce::ListBox::backgroundColourId, col::panel);
        setColour (juce::ScrollBar::thumbColourId, col::line);
        setColour (juce::ToggleButton::tickColourId, acc);
        setColour (juce::ToggleButton::textColourId, col::text);
        setColour (juce::ProgressBar::foregroundColourId, acc);
        setColour (juce::ProgressBar::backgroundColourId, col::panel2);
        setColour (juce::TooltipWindow::backgroundColourId, col::panel2);
        setColour (juce::TooltipWindow::textColourId, col::text);
        setColour (juce::TextEditor::backgroundColourId, col::panel2);
        setColour (juce::TextEditor::outlineColourId, col::line);
        setColour (juce::TextEditor::textColourId, col::text);
    }

    juce::Colour accent() const { return acc; }

    juce::Font getTextButtonFont (juce::TextButton& b, int h) override
    {   // shrinks to fit a narrow button (small / scaled windows) instead of cutting the text to "RAN..."
        auto f = font (juce::jmin (13.0f, (float) h * 0.48f), true);
        const float textW = juce::GlyphArrangement::getStringWidth (f, b.getButtonText()), room = (float) b.getWidth() - 8.0f;
        return textW > room && room > 0 ? font (juce::jmax (8.0f, f.getHeight() * room / textW), true) : f;
    }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (12.0f, true); }
    juce::Font getPopupMenuFont() override { return font (13.0f, false); }

    static juce::Font font (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::outlineWhenEditingColourId, acc);
        l->setFont (font (11.5f));
        l->setMinimumHorizontalScale (0.5f); l->setBorderSize ({ 1, 1, 1, 1 });   // "20.00 kHz" squeezes into a narrow knob rather than becoming "20.00 ..." 
        return l;
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgc, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bgc;
        if (! b.isEnabled()) c = c.withMultipliedAlpha (0.4f);
        else if (down) c = c.brighter (0.25f);
        else if (over) c = c.brighter (0.1f);
        g.setColour (c); g.fillRoundedRectangle (r, 4.0f);
        g.setColour (col::line.brighter (0.15f)); g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) override
    {
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
        float rad = juce::jmin (r.getWidth(), r.getHeight()) / 2.0f, cx = r.getCentreX(), cy = r.getCentreY(), ang = a0 + pos * (a1 - a0);
        juce::Path bgArc; bgArc.addCentredArc (cx, cy, rad - 2, rad - 2, 0, a0, a1, true);
        g.setColour (col::line); g.strokePath (bgArc, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        bool bipolar = s.getMinimum() < 0 && s.getMaximum() > 0;
        float from = bipolar ? a0 + (float) ((0 - s.getMinimum()) / (s.getMaximum() - s.getMinimum())) * (a1 - a0) : a0;
        if (bipolar) from = a0 + (float) s.valueToProportionOfLength (0.0) * (a1 - a0);
        juce::Path arc; arc.addCentredArc (cx, cy, rad - 2, rad - 2, 0, juce::jmin (from, ang), juce::jmax (from, ang), true);
        auto c = s.findColour (juce::Slider::rotarySliderFillColourId);
        g.setColour (s.isEnabled() ? c : c.withAlpha (0.3f)); g.strokePath (arc, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (col::panel2); g.fillEllipse (cx - rad * 0.62f, cy - rad * 0.62f, rad * 1.24f, rad * 1.24f);
        juce::Path tick; tick.addRoundedRectangle (-1.5f, -rad * 0.62f, 3.0f, rad * 0.42f, 1.5f);
        g.setColour (col::text); g.fillPath (tick, juce::AffineTransform::rotation (ang).translated (cx, cy));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle st, juce::Slider& s) override
    {
        if (st != juce::Slider::LinearHorizontal) return LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, st, s);
        auto r = juce::Rectangle<float> ((float) x, (float) y + (float) h / 2 - 2, (float) w, 4);
        g.setColour (col::line); g.fillRoundedRectangle (r, 2);
        g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId)); g.fillRoundedRectangle (r.withRight (pos), 2);
        g.setColour (col::text); g.fillEllipse (pos - 6, (float) y + (float) h / 2 - 6, 12, 12);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        bool on = b.getToggleState();
        g.setColour (on ? acc.withAlpha (0.22f) : (over ? col::panel2.brighter (0.08f) : col::panel2)); g.fillRoundedRectangle (r, 4);
        g.setColour (on ? acc : col::line); g.drawRoundedRectangle (r, 4, 1);
        g.setColour (on ? acc : col::dim); g.setFont (font (11.5f, true));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0), juce::Justification::centred, 1);
    }

private:
    juce::Colour acc;
};
}
