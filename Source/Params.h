#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace bw
{
constexpr int kNumBands = 6;
constexpr float kMinFreq = 10.0f;
constexpr float kMaxFreq = 30000.0f;
constexpr float kMaxLookaheadMs = 20.0f;

enum class PhaseMode { Dynamic = 0, Linear = 1, Minimum = 2 };
enum class DynMode { Compress = 0, Expand = 1 };
enum class StereoMode { Stereo = 0, Mid = 1, Side = 2, Left = 3, Right = 4 };

inline juce::String bandId (int band, const char* name) { return "b" + juce::String (band + 1) + "_" + name; }

namespace id
{
    inline const juce::String phase = "phase", lookahead = "lookahead", mix = "mix", output = "output";
}

inline const juce::StringArray slopeNames { "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct",
                                            "30 dB/oct", "36 dB/oct", "42 dB/oct", "48 dB/oct" };

inline juce::String freqText (float hz)
{
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz"
                         : juce::String (juce::roundToInt (hz)) + " Hz";
}

inline float freqFromText (const juce::String& t)
{
    auto v = t.getFloatValue();
    return t.containsIgnoreCase ("k") ? v * 1000.0f : v;
}

inline juce::NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float s, float e, float n) { return s * std::pow (e / s, n); },
             [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
             [] (float, float, float v) { return v; } };
}

// Default band layout for a vocal: low mud, body, presence, sibilance, air, extra
inline void defaultBandFreqs (int b, float& lo, float& hi)
{
    static const float d[kNumBands][2] = { { 10.0f, 160.0f },  { 160.0f, 500.0f },  { 500.0f, 2500.0f },
                                           { 2500.0f, 5000.0f }, { 5000.0f, 10000.0f }, { 10000.0f, 30000.0f } };
    lo = d[b][0];
    hi = d[b][1];
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto dbAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) {
        return String (v, 1) + " dB"; });
    auto msAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) {
        return v < 10.0f ? String (v, 2) + " ms" : (v < 1000.0f ? String (v, 1) + " ms" : String (v / 1000.0f, 2) + " s"); });
    auto hzAttr = AudioParameterFloatAttributes()
                      .withStringFromValueFunction ([] (float v, int) { return freqText (v); })
                      .withValueFromStringFunction ([] (const String& t) { return freqFromText (t); });
    auto ratioAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) {
        return v >= 99.5f ? String ("inf : 1") : String (v, v < 10.0f ? 2 : 1) + " : 1"; });
    auto pctAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) {
        return String (roundToInt (v)) + " %"; });

    int ver = 1;
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id::phase, ver }, "Processing Mode",
                                                        StringArray { "Dynamic Phase", "Linear Phase", "Minimum Phase" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { id::lookahead, ver }, "Lookahead Enabled", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id::mix, ver }, "Mix",
                                                       NormalisableRange<float> (0.0f, 200.0f, 0.1f), 100.0f, pctAttr));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id::output, ver }, "Output Gain",
                                                       NormalisableRange<float> (-36.0f, 36.0f, 0.01f), 0.0f, dbAttr));

    for (int b = 0; b < kNumBands; ++b)
    {
        auto grp = std::make_unique<AudioProcessorParameterGroup> ("band" + String (b + 1), "Band " + String (b + 1), " | ");
        auto P = [&] (const char* n) { return ParameterID { bandId (b, n), ver }; };
        auto N = [&] (const char* n) { return "Band " + String (b + 1) + " " + n; };
        float lo, hi;
        defaultBandFreqs (b, lo, hi);

        auto thrRange = NormalisableRange<float> (-80.0f, 0.0f, 0.01f);
        auto rangeRange = NormalisableRange<float> (-60.0f, 36.0f, 0.01f);
        auto ratioRange = NormalisableRange<float> (1.0f, 100.0f, 0.01f);
        ratioRange.setSkewForCentre (4.0f);
        auto kneeRange = NormalisableRange<float> (0.0f, 72.0f, 0.01f);
        kneeRange.setSkewForCentre (12.0f);
        auto atkRange = NormalisableRange<float> (0.05f, 250.0f, 0.01f);
        atkRange.setSkewForCentre (10.0f);
        auto relRange = NormalisableRange<float> (5.0f, 5000.0f, 0.1f);
        relRange.setSkewForCentre (200.0f);

        grp->addChild (std::make_unique<AudioParameterBool> (P ("on"), N ("Enabled"), false));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("lo"), N ("Low Freq"), logRange (kMinFreq, kMaxFreq), lo, hzAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("hi"), N ("High Freq"), logRange (kMinFreq, kMaxFreq), hi, hzAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("thr"), N ("Threshold"), thrRange, -24.0f, dbAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("range"), N ("Range"), rangeRange, -18.0f, dbAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("ratio"), N ("Ratio"), ratioRange, 3.0f, ratioAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("knee"), N ("Knee"), kneeRange, 12.0f, dbAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("atk"), N ("Attack"), atkRange, 10.0f, msAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("rel"), N ("Release"), relRange, 150.0f, msAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("look"), N ("Lookahead"),
                                                              NormalisableRange<float> (0.0f, kMaxLookaheadMs, 0.01f), 5.0f, msAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("gain"), N ("Gain"),
                                                              NormalisableRange<float> (-36.0f, 36.0f, 0.01f), 0.0f, dbAttr));
        grp->addChild (std::make_unique<AudioParameterChoice> (P ("mode"), N ("Mode"), StringArray { "Compress", "Expand" }, 0));
        grp->addChild (std::make_unique<AudioParameterChoice> (P ("stereo"), N ("Stereo Mode"),
                                                               StringArray { "Stereo", "Mid", "Side", "Left", "Right" }, 0));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("link"), N ("Stereo Link"),
                                                              NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, pctAttr));
        grp->addChild (std::make_unique<AudioParameterChoice> (P ("slope"), N ("Slope"), slopeNames, 3));
        grp->addChild (std::make_unique<AudioParameterBool> (P ("scext"), N ("External Sidechain"), false));
        grp->addChild (std::make_unique<AudioParameterBool> (P ("scfree"), N ("Free Sidechain Range"), false));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("sclo"), N ("Sidechain Low"), logRange (kMinFreq, kMaxFreq), lo, hzAttr));
        grp->addChild (std::make_unique<AudioParameterFloat> (P ("schi"), N ("Sidechain High"), logRange (kMinFreq, kMaxFreq), hi, hzAttr));
        grp->addChild (std::make_unique<AudioParameterBool> (P ("solo"), N ("Solo"), false));
        grp->addChild (std::make_unique<AudioParameterBool> (P ("byp"), N ("Bypass"), false));
        layout.add (std::move (grp));
    }
    return layout;
}
} // namespace bw
