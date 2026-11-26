#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "../PluginProcessor.h"

namespace bw::ui
{
class BandDisplay : public juce::Component, private juce::Timer
{
public:
    explicit BandDisplay (BandwerkProcessor& p);
    ~BandDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override;

    std::function<void (int)> onSelectBand;

    // response of one band in dB at 'hz' for an applied gain change 'db'
    static float bandResponseDb (const BandParams& p, float db, float hz, double fs);

private:
    enum class Part { None, Body, LoEdge, HiEdge };
    struct Hit { int band = -1; Part part = Part::None; };

    void timerCallback() override;
    void pullAnalyzer (juce::AbstractFifo& f, const std::vector<float>& data, std::vector<float>& ring, int& pos);
    void computeSpectrum (const std::vector<float>& ring, int pos, std::vector<float>& outDb);
    Hit findBand (juce::Point<float> p) const;

    float freqToX (float hz) const;
    float xToFreq (float x) const;
    float gainToY (float db) const;
    float specToY (float db) const;
    float yToSpec (float y) const;

    void setParam (int band, const char* name, float value);
    float getParam (int band, const char* name) const;
    juce::RangedAudioParameter* param (int band, const char* name) const;
    void beginGesture (int band);
    void endGesture();
    float snapFreq (int band, float hz) const;

    BandwerkProcessor& proc;
    juce::Rectangle<float> plot;

    static constexpr int kFftOrder = 12, kFftSize = 1 << kFftOrder;
    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) kFftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> preRing, postRing, preDb, postDb, fftBuf;
    int prePos = 0, postPos = 0;

    Hit hover, drag;
    float dragStartLo = 0, dragStartHi = 0, dragStartThr = 0;
    juce::Point<float> dragStart;
    std::vector<juce::RangedAudioParameter*> gestureParams;
    std::array<float, kNumBands> shownGain {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandDisplay)
};
} // namespace bw::ui
