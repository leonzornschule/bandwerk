#include "PluginEditor.h"

using namespace bw;
using namespace bw::ui;

BandwerkEditor::BandwerkEditor (BandwerkProcessor& p) : AudioProcessorEditor (p), proc (p), display (p)
{
    setLookAndFeel (&look);
    addAndMakeVisible (display);
    display.onSelectBand = [this] (int b) { selectBand (b); };

    for (int b = 0; b < kNumBands; ++b)
    {
        auto& t = tabs[(size_t) b];
        t.setButtonText (juce::String (b + 1));
        t.setClickingTogglesState (false);
        t.onClick = [this, b] { selectBand (b); };
        addAndMakeVisible (t);
    }

    setupToggle (onBtn, "ON");
    setupToggle (soloBtn, "SOLO");
    setupToggle (bypBtn, "BYPASS");
    setupToggle (extBtn, "EXT SC");
    setupToggle (freeBtn, "FREE SC");
    setupToggle (lookBtn, "LOOKAHEAD");
    lookBtn.setColour (juce::TextButton::buttonOnColourId, col::accent);

    setupKnob (thr, "THRESHOLD", "thr");
    setupKnob (range, "RANGE", "range", true);
    setupKnob (ratio, "RATIO", "ratio");
    setupKnob (knee, "KNEE", "knee");
    setupKnob (atk, "ATTACK", "atk");
    setupKnob (rel, "RELEASE", "rel");
    setupKnob (look_, "LOOKAHEAD", "look");
    setupKnob (gain, "GAIN", "gain", true);
    setupKnob (mix, "MIX", nullptr);
    setupKnob (out, "OUTPUT", nullptr, true);
    setupBar (loBar, "LOW", "lo", false);
    setupBar (hiBar, "HIGH", "hi", false);
    setupBar (linkBar, "LINK", "link");
    setupBar (scLoBar, "SC LOW", "sclo", false);
    setupBar (scHiBar, "SC HIGH", "schi", false);

    for (auto* k : { &thr, &range, &ratio, &knee, &atk, &rel, &look_, &gain, &loBar, &hiBar, &linkBar, &scLoBar, &scHiBar })
        bandKnobs.push_back (k);

    modeBox.addItemList ({ "Compress", "Expand" }, 1);
    stereoBox.addItemList ({ "Stereo", "Mid", "Side", "Left", "Right" }, 1);
    slopeBox.addItemList (slopeNames, 1);
    phaseBox.addItemList ({ "Dynamic Phase", "Linear Phase", "Minimum Phase" }, 1);
    for (auto* c : { &modeBox, &stereoBox, &slopeBox, &phaseBox })
        addAndMakeVisible (*c);

    mix.att = std::make_unique<SA> (proc.apvts, id::mix, mix.s);
    out.att = std::make_unique<SA> (proc.apvts, id::output, out.s);
    mix.s.setDoubleClickReturnValue (true, 100.0);
    out.s.setDoubleClickReturnValue (true, 0.0);
    phaseAtt = std::make_unique<CA> (proc.apvts, id::phase, phaseBox);
    lookAtt = std::make_unique<BA> (proc.apvts, id::lookahead, lookBtn);

    setSize (1080, 700);
    sendLookAndFeelChange();
    selectBand (proc.selectedBand.load());
    startTimerHz (30);
}

BandwerkEditor::~BandwerkEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void BandwerkEditor::setupKnob (Knob& k, const juce::String& label, const char* pid, bool bipolar)
{
    k.label = label;
    k.pid = pid != nullptr ? pid : "";
    k.s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 16);
    k.s.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    k.s.getProperties().set ("bipolar", bipolar);
    k.s.setVelocityBasedMode (false);
    k.s.setMouseDragSensitivity (220);
    addAndMakeVisible (k.s);
}

void BandwerkEditor::setupBar (Knob& k, const juce::String& label, const char* pid, bool fill)
{
    k.label = label;
    k.pid = pid;
    k.s.setSliderStyle (juce::Slider::LinearBar);
    k.s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    k.s.getProperties().set ("fill", fill);
    addAndMakeVisible (k.s);
}

void BandwerkEditor::setupToggle (juce::TextButton& b, const juce::String& text)
{
    b.setButtonText (text);
    b.setClickingTogglesState (true);
    addAndMakeVisible (b);
}

void BandwerkEditor::selectBand (int b)
{
    proc.selectedBand.store (b);
    bindBand();
}

void BandwerkEditor::bindBand()
{
    const int b = proc.selectedBand.load();
    boundBand = b;
    const bool valid = b >= 0 && b < kNumBands;

    for (auto* k : bandKnobs) k->att.reset();
    onAtt.reset(); soloAtt.reset(); bypAtt.reset(); extAtt.reset(); freeAtt.reset();
    modeAtt.reset(); stereoAtt.reset(); slopeAtt.reset();

    const auto c = valid ? col::band (b) : col::textDim;
    for (int i = 0; i < kNumBands; ++i)
    {
        auto& t = tabs[(size_t) i];
        const bool on = proc.apvts.getRawParameterValue (bandId (i, "on"))->load() > 0.5f;
        t.setColour (juce::TextButton::buttonColourId, i == b ? col::band (i) : (on ? col::band (i).withAlpha (0.35f) : col::panelHi));
        t.setColour (juce::TextButton::textColourOffId, i == b ? juce::Colour (0xff111111) : col::text);
    }

    for (auto* k : bandKnobs)
    {
        k->s.setEnabled (valid);
        k->s.setColour (juce::Slider::rotarySliderFillColourId, c);
        k->s.setColour (juce::Slider::trackColourId, c);
    }
    for (auto* btn : { &onBtn, &soloBtn, &bypBtn, &extBtn, &freeBtn })
    {
        btn->setEnabled (valid);
        btn->setColour (juce::TextButton::buttonOnColourId, c);
    }
    for (auto* cb : { &modeBox, &stereoBox, &slopeBox }) cb->setEnabled (valid);

    if (valid)
    {
        auto& s = proc.apvts;
        for (auto* k : bandKnobs)
        {
            k->att = std::make_unique<SA> (s, bandId (b, k->pid.toRawUTF8()), k->s);
            if (auto* p = s.getParameter (bandId (b, k->pid.toRawUTF8())))
                k->s.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
        }
        onAtt = std::make_unique<BA> (s, bandId (b, "on"), onBtn);
        soloAtt = std::make_unique<BA> (s, bandId (b, "solo"), soloBtn);
        bypAtt = std::make_unique<BA> (s, bandId (b, "byp"), bypBtn);
        extAtt = std::make_unique<BA> (s, bandId (b, "scext"), extBtn);
        freeAtt = std::make_unique<BA> (s, bandId (b, "scfree"), freeBtn);
        modeAtt = std::make_unique<CA> (s, bandId (b, "mode"), modeBox);
        stereoAtt = std::make_unique<CA> (s, bandId (b, "stereo"), stereoBox);
        slopeAtt = std::make_unique<CA> (s, bandId (b, "slope"), slopeBox);
    }
    repaint();
}

void BandwerkEditor::timerCallback()
{
    if (proc.selectedBand.load() != boundBand) bindBand();

    // refresh tab colours when bands are switched on/off from the display or automation
    bool changed = false;
    for (int i = 0; i < kNumBands; ++i)
    {
        const bool on = proc.apvts.getRawParameterValue (bandId (i, "on"))->load() > 0.5f;
        if (on != lastOn[(size_t) i]) { lastOn[(size_t) i] = on; changed = true; }
    }
    if (changed) bindBand();

    const bool sc = proc.apvts.getRawParameterValue (bandId (std::max (0, boundBand), "scfree"))->load() > 0.5f;
    scLoBar.s.setEnabled (boundBand >= 0 && sc);
    scHiBar.s.setEnabled (boundBand >= 0 && sc);

    auto fall = [] (float cur, float v) { return v > cur ? v : std::max (v, cur - 1.5f); };
    meterL = fall (meterL, proc.engine.meterOutL.load());
    meterR = fall (meterR, proc.engine.meterOutR.load());
    repaint (meterArea);
    repaint (globalArea.withHeight (40));
}

void BandwerkEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    // header
    auto header = getLocalBounds().removeFromTop (40);
    g.setColour (col::panel);
    g.fillRect (header);
    g.setColour (col::text);
    g.setFont (juce::FontOptions (19.0f).withStyle ("Bold"));
    g.drawText ("BANDWERK", header.withTrimmedLeft (16), juce::Justification::centredLeft);
    g.setColour (col::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("multiband dynamics", header.withTrimmedLeft (122), juce::Justification::centredLeft);

    const int lat = proc.getLatencySamples();
    const double fs = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
    juce::String info = "Latency " + juce::String (lat / fs * 1000.0, 1) + " ms";
    g.drawText (info, header.withTrimmedRight (440), juce::Justification::centredRight);

    // panel
    g.setColour (col::panel);
    g.fillRect (panelArea);
    g.setColour (col::bg);
    g.fillRect (globalArea.getX() - 6, panelArea.getY(), 2, panelArea.getHeight());

    g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
    g.setColour (col::textDim);
    auto labelAbove = [&] (juce::Component& c, const juce::String& t) {
        g.drawText (t, c.getX() - 10, c.getY() - 15, c.getWidth() + 20, 13, juce::Justification::centred);
    };
    for (auto* k : { &thr, &range, &ratio, &knee, &atk, &rel, &look_, &gain, &mix, &out })
        labelAbove (k->s, k->label);
    for (auto* k : { &linkBar, &scLoBar, &scHiBar })
        labelAbove (k->s, k->label);
    for (auto* k : { &loBar, &hiBar })
        g.drawText (k->label, k->s.getX() - 40, k->s.getY(), 36, k->s.getHeight(), juce::Justification::centredRight);
    labelAbove (modeBox, "MODE");
    labelAbove (stereoBox, "STEREO");
    labelAbove (slopeBox, "SLOPE");

    const int b = proc.selectedBand.load();
    if (b < 0)
    {
        g.setColour (col::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("Kein Band ausgew\xc3\xa4hlt")), tabs[5].getRight() + 290, tabs[0].getY(), 240, tabs[0].getHeight(),
                    juce::Justification::centredLeft);
    }

    // output meter
    auto m = meterArea.toFloat();
    g.setColour (col::display);
    g.fillRoundedRectangle (m, 2.0f);
    auto bar = [&] (juce::Rectangle<float> r, float db) {
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
        auto fillR = r.withTop (r.getBottom() - r.getHeight() * norm);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4fd6a0), 0, r.getBottom(), juce::Colour (0xffe8473d), 0,
                                                 r.getY(), false));
        g.fillRect (fillR);
    };
    auto inner = m.reduced (3.0f);
    bar (inner.removeFromLeft (inner.getWidth() * 0.5f - 1.0f), meterL);
    inner.removeFromLeft (2.0f);
    bar (inner, meterR);
    g.setColour (col::textDim);
    g.setFont (juce::FontOptions (10.5f));
    g.drawText (juce::String (std::max (meterL, meterR), 1), meterArea.getX() - 16, meterArea.getBottom() + 2,
                meterArea.getWidth() + 32, 14, juce::Justification::centred);
}

void BandwerkEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (40);
    {
        auto h = header.reduced (8, 7);
        lookBtn.setBounds (h.removeFromRight (100));
        h.removeFromRight (8);
        phaseBox.setBounds (h.removeFromRight (150));
    }

    panelArea = r.removeFromBottom (236);
    display.setBounds (r);

    auto p = panelArea.reduced (12, 10);
    globalArea = p.removeFromRight (220);
    p.removeFromRight (12);

    // row 1: band tabs, switches, frequency
    auto row1 = p.removeFromTop (26);
    for (auto& t : tabs) { t.setBounds (row1.removeFromLeft (30)); row1.removeFromLeft (4); }
    row1.removeFromLeft (10);
    onBtn.setBounds (row1.removeFromLeft (44)); row1.removeFromLeft (4);
    soloBtn.setBounds (row1.removeFromLeft (52)); row1.removeFromLeft (4);
    bypBtn.setBounds (row1.removeFromLeft (64)); row1.removeFromLeft (16);
    hiBar.s.setBounds (row1.removeFromRight (90));
    row1.removeFromRight (46);
    loBar.s.setBounds (row1.removeFromRight (90));

    p.removeFromTop (22);
    auto row2 = p.removeFromTop (100);
    const int kw = row2.getWidth() / 8;
    for (auto* k : { &thr, &range, &ratio, &knee, &atk, &rel, &look_, &gain })
        k->s.setBounds (row2.removeFromLeft (kw).reduced (6, 0));

    p.removeFromTop (22);
    auto row3 = p.removeFromTop (24);
    auto place = [&] (juce::Component& c, int w) { c.setBounds (row3.removeFromLeft (w)); row3.removeFromLeft (10); };
    place (modeBox, 98);
    place (stereoBox, 86);
    place (slopeBox, 98);
    place (linkBar.s, 80);
    place (extBtn, 62);
    place (freeBtn, 68);
    place (scLoBar.s, 86);
    place (scHiBar.s, 86);

    // global section
    auto gsec = globalArea;
    meterArea = gsec.removeFromRight (26).withTrimmedTop (18).withTrimmedBottom (34);
    gsec.removeFromRight (10);
    gsec.removeFromTop (48);
    const int gw = gsec.getWidth() / 2;
    mix.s.setBounds (gsec.removeFromLeft (gw).removeFromTop (100).reduced (4, 0));
    out.s.setBounds (gsec.removeFromLeft (gw).removeFromTop (100).reduced (4, 0));
}
