#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include "../Params.h"
#include "Filters.h"
#include "Dynamics.h"

namespace bw
{
struct BandParams
{
    bool on = false, solo = false, bypass = false, scExt = false, scFree = false;
    float lo = 100, hi = 1000, scLo = 100, scHi = 1000;
    float thr = -24, range = -18, ratio = 3, knee = 12;
    float atkMs = 10, relMs = 150, lookMs = 0, gainDb = 0, link = 100;
    int mode = 0, stereo = 0, slopeIdx = 3;

    int slopeDb() const { return 6 * (slopeIdx + 1); }
    int shelfOrder() const { return slopeIdx + 1; }                    // 6 dB/oct per order
    int detOrder() const { return std::max (2, slopeIdx + 1); }
    int lrOrder() const { return slopeIdx <= 3 ? 4 : 8; }
};

struct GlobalParams
{
    PhaseMode phase = PhaseMode::Dynamic;
    bool lookahead = false;
    float mix = 1.0f, outDb = 0.0f;
};

// Simple per-channel delay line with integer delay
class Delay
{
public:
    void prepare (int maxDelay)
    {
        size = juce::nextPowerOfTwo (maxDelay + 2);
        buf.assign ((size_t) size, 0.0f);
        mask = size - 1;
        w = 0;
    }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); }
    inline void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }
    // delay 0 = most recent pushed sample
    inline float get (int d) const noexcept { return buf[(size_t) ((w - 1 - d) & mask)]; }

private:
    std::vector<float> buf;
    int size = 0, mask = 0, w = 0;
};

class Engine
{
public:
    Engine();

    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset();
    void process (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>* sidechain,
                  const GlobalParams& g, const std::array<BandParams, kNumBands>& bands);

    int firLength() const { return firLen; }
    int latencyFor (PhaseMode mode, bool lookahead) const;
    double getSampleRate() const { return fs; }

    // Linear phase FIR design, called from the message thread
    bool requestFirUpdate (int band, const BandParams& p);
    static float bandMagnitudeLinear (const BandParams& p, double hz, double fs);
    // Hands pending IRs to the convolution engines and waits until they are active.
    // Only call while the audio thread is not processing (prepareToPlay).
    void primeLinearPhase();
    const juce::AudioBuffer<float>& debugPendingIr (int b) const { return bands[(size_t) b].pendingIr; }

    // Metering (read by the editor)
    std::array<std::atomic<float>, kNumBands> meterGainDb;  // applied gain change (incl. band gain, mix)
    std::array<std::atomic<float>, kNumBands> meterDynDb;   // dynamic part only
    std::array<std::atomic<float>, kNumBands> meterLevelDb; // detector level
    std::atomic<float> meterOutL { -120 }, meterOutR { -120 };

    // Analyzer feed (mono sums of input and output)
    static constexpr int kFifoSize = 1 << 15;
    juce::AbstractFifo preFifo { kFifoSize }, postFifo { kFifoSize };
    std::vector<float> preData, postData;

private:
    struct Lane
    {
        dsp::Butterworth detHp, detLp;
        dsp::Envelope env;
        double peak = 0.0;
        Delay gainDelay;
        dsp::HighShelf shelfLo, shelfHi;
        double lastG = -1.0;
        float lastLo = -1, lastHi = -1;
        dsp::Butterworth soloHp, soloLp;
    };

    struct Band
    {
        std::array<Lane, 2> lane;
        dsp::GainComputer gc;
        float smLo = 100, smHi = 1000;
        bool wasOn = false;
        std::vector<float> gainDb[2];
        // linear phase
        std::unique_ptr<juce::dsp::Convolution> conv;
        juce::AudioBuffer<float> convOut;
        juce::AudioBuffer<float> pendingIr;
        std::atomic<int> irState { 0 }; // 0 = slot free (message thread may write), 1 = ready for audio thread
        int irWarmup = 0;
        bool irActive = false;
    };

    void processDetection (int b, const BandParams& p, int n, int lookSamplesGlobal, float mix,
                           const float* const* det, int numCh);
    void processDynamicPhase (const std::array<BandParams, kNumBands>& bp, float* const* ch, int numCh, int n);
    void processMinimumPhase (const std::array<BandParams, kNumBands>& bp, float* const* ch, int numCh, int n);
    void processLinearPhase (const std::array<BandParams, kNumBands>& bp, float* const* ch, int numCh, int n);
    void applySolo (const std::array<BandParams, kNumBands>& bp, float* const* ch, int numCh, int n);
    void pushFifo (juce::AbstractFifo& f, std::vector<float>& data, const float* const* ch, int numCh, int n);

    // Minimum phase: one Linkwitz-Riley crossover tree built from all band edges
    static constexpr int kMaxEdges = 2 * kNumBands;
    struct MpChannel
    {
        dsp::LinkwitzRiley split[kMaxEdges];
        dsp::LRAllpass ap[kMaxEdges][kMaxEdges];
        void reset()
        {
            for (auto& s : split) s.reset();
            for (auto& row : ap) for (auto& a : row) a.reset();
        }
    };
    std::array<MpChannel, 2> mp;
    std::vector<std::pair<float, int>> mpLastEdges;

    double fs = 48000;
    int maxBlock = 512, numCh = 2, firLen = 8192, maxLook = 960;
    PhaseMode lastMode = PhaseMode::Dynamic;
    bool lastLookahead = false;
    std::array<Band, kNumBands> bands;
    std::array<Delay, 2> inDelay, scDelay;
    juce::AudioBuffer<float> work, detIn, scIn, dry, soloAcc;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outGain;
};
} // namespace bw
