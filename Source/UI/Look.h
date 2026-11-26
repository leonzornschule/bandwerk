#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace bw::ui
{
namespace col
{
    const juce::Colour bg { 0xff15171c }, panel { 0xff1c1f26 }, panelHi { 0xff252932 }, display { 0xff0f1115 },
        grid { 0xff252a33 }, gridHi { 0xff353b46 }, text { 0xffc9cdd6 }, textDim { 0xff7d8491 }, accent { 0xffe8a33d };

    inline juce::Colour band (int b)
    {
        static const juce::uint32 c[] = { 0xffe8a33d, 0xff4fb3e8, 0xff7fd16b, 0xffe8647f, 0xffb08cf0, 0xff4fd6c2 };
        return juce::Colour (c[(b % 6 + 6) % 6]);
    }
}

class Look : public juce::LookAndFeel_V4
{
public:
    Look()
    {
        setColour (juce::ResizableWindow::backgroundColourId, col::bg);
        setColour (juce::Slider::textBoxTextColourId, col::text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, col::accent.withAlpha (0.4f));
        setColour (juce::Slider::rotarySliderFillColourId, col::accent);
        setColour (juce::Slider::trackColourId, col::accent);
        setColour (juce::Slider::backgroundColourId, col::panelHi);
        setColour (juce::ComboBox::backgroundColourId, col::panelHi);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::textColourId, col::text);
        setColour (juce::ComboBox::arrowColourId, col::textDim);
        setColour (juce::PopupMenu::backgroundColourId, col::panel);
        setColour (juce::PopupMenu::textColourId, col::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, col::panelHi);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextButton::buttonColourId, col::panelHi);
        setColour (juce::TextButton::buttonOnColourId, col::accent);
        setColour (juce::TextButton::textColourOffId, col::text);
        setColour (juce::TextButton::textColourOnId, juce::Colour (0xff111111));
        setColour (juce::Label::textColourId, col::text);
        setColour (juce::TextEditor::backgroundColourId, col::panelHi);
        setColour (juce::TextEditor::textColourId, juce::Colours::white);
        setColour (juce::TextEditor::highlightColourId, col::accent.withAlpha (0.4f));
        setColour (juce::CaretComponent::caretColourId, juce::Colours::white);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1,
                           juce::Slider& s) override
    {
        const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
        const float r = std::min (area.getWidth(), area.getHeight()) * 0.5f;
        const auto c = area.getCentre();
        const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);
        const bool on = s.isEnabled();
        const float ang = a0 + pos * (a1 - a0);
        const bool bipolar = (bool) s.getProperties().getWithDefault ("bipolar", false);

        juce::Path track;
        track.addCentredArc (c.x, c.y, r - 2.0f, r - 2.0f, 0.0f, a0, a1, true);
        g.setColour (col::panelHi);
        g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path val;
        const float from = bipolar ? a0 + (a1 - a0) * (float) s.valueToProportionOfLength (0.0) : a0;
        val.addCentredArc (c.x, c.y, r - 2.0f, r - 2.0f, 0.0f, std::min (from, ang), std::max (from, ang), true);
        g.setColour (on ? accent : col::textDim);
        g.strokePath (val, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float kr = r - 7.0f;
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3f4a), c.x, c.y - kr, juce::Colour (0xff22262e), c.x,
                                                 c.y + kr, false));
        g.fillEllipse (c.x - kr, c.y - kr, kr * 2, kr * 2);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawEllipse (c.x - kr, c.y - kr, kr * 2, kr * 2, 1.0f);

        juce::Path tick;
        tick.addRoundedRectangle (-1.25f, -kr + 3.0f, 2.5f, kr * 0.45f, 1.0f);
        g.setColour (on ? juce::Colours::white.withAlpha (0.9f) : col::textDim);
        g.fillPath (tick, juce::AffineTransform::rotation (ang).translated (c.x, c.y));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearBar)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
            return;
        }
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        g.setColour (col::panelHi);
        g.fillRoundedRectangle (r, 3.0f);
        const bool showFill = (bool) s.getProperties().getWithDefault ("fill", true);
        if (showFill)
        {
            g.setColour (s.findColour (juce::Slider::trackColourId).withAlpha (s.isEnabled() ? 0.45f : 0.15f));
            g.fillRoundedRectangle (r.withRight (pos), 3.0f);
        }
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (juce::FontOptions (12.0f));
        l->setJustificationType (juce::Justification::centred);
        return l;
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgc, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = bgc;
        if (over) c = c.brighter (0.08f);
        if (down) c = c.brighter (0.15f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 3.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override { return juce::FontOptions (std::min (13.0f, (float) h * 0.55f)).withStyle ("Bold"); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::FontOptions (13.0f); }
    juce::Font getPopupMenuFont() override { return juce::FontOptions (14.0f); }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 3.0f);
        juce::Path p;
        const float ax = (float) w - 12.0f, ay = (float) h * 0.5f;
        p.addTriangle (ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.fillPath (p);
    }
};
} // namespace bw::ui
