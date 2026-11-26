#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "DSP/Engine.h"

class BandwerkProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    BandwerkProcessor();
    ~BandwerkProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Bandwerk"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    bw::BandParams readBand (int b) const;
    bw::GlobalParams readGlobal() const;

    juce::AudioProcessorValueTreeState apvts;
    bw::Engine engine;
    std::atomic<int> selectedBand { -1 };
    std::atomic<bool> sidechainConnected { false };

private:
    void timerCallback() override;
    void updateFirs (bool force);

    struct BandPtrs
    {
        std::atomic<float>*on, *lo, *hi, *thr, *range, *ratio, *knee, *atk, *rel, *look, *gain, *mode, *stereo, *link,
            *slope, *scext, *scfree, *sclo, *schi, *solo, *byp;
    };
    std::array<BandPtrs, bw::kNumBands> bp;
    std::atomic<float>*pPhase, *pLook, *pMix, *pOut;

    std::array<juce::int64, bw::kNumBands> firHash {};
    std::array<bool, bw::kNumBands> firDirty {};
    std::atomic<bool> prepared { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandwerkProcessor)
};
