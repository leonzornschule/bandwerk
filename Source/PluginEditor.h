#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "UI/Look.h"
#include "UI/BandDisplay.h"

class BandwerkEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BandwerkEditor (BandwerkProcessor&);
    ~BandwerkEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void selectBand (int b);

private:
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct Knob
    {
        juce::Slider s;
        juce::String label, pid;
        std::unique_ptr<SA> att;
    };

    void timerCallback() override;
    void setupKnob (Knob& k, const juce::String& label, const char* pid, bool bipolar = false);
    void setupBar (Knob& k, const juce::String& label, const char* pid, bool fill = true);
    void setupToggle (juce::TextButton& b, const juce::String& text);
    void bindBand();

    BandwerkProcessor& proc;
    bw::ui::Look look;
    bw::ui::BandDisplay display;

    std::array<juce::TextButton, bw::kNumBands> tabs;
    juce::TextButton onBtn, soloBtn, bypBtn, extBtn, freeBtn, lookBtn;
    juce::ComboBox modeBox, stereoBox, slopeBox, phaseBox;
    Knob thr, range, ratio, knee, atk, rel, look_, gain, mix, out;
    Knob loBar, hiBar, linkBar, scLoBar, scHiBar;
    std::unique_ptr<BA> onAtt, soloAtt, bypAtt, extAtt, freeAtt, lookAtt;
    std::unique_ptr<CA> modeAtt, stereoAtt, slopeAtt, phaseAtt;

    std::vector<Knob*> bandKnobs;
    juce::Rectangle<int> panelArea, globalArea, meterArea;
    float meterL = -60, meterR = -60;
    int boundBand = -2;
    std::array<bool, bw::kNumBands> lastOn {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandwerkEditor)
};
