#include "Engine.h"

namespace bw
{
static inline double dbToGain (double db) { return std::pow (10.0, db * 0.05); }

Engine::Engine()
{
    for (auto& m : meterGainDb) m.store (0.0f);
    for (auto& m : meterDynDb) m.store (0.0f);
    for (auto& m : meterLevelDb) m.store (-120.0f);
    preData.assign (kFifoSize, 0.0f);
    postData.assign (kFifoSize, 0.0f);
}

int Engine::latencyFor (PhaseMode mode, bool lookahead) const
{
    return (lookahead ? maxLook : 0) + (mode == PhaseMode::Linear ? firLen / 2 : 0);
}

void Engine::prepare (double sampleRate, int maxBlockIn, int numChannels)
{
    fs = sampleRate;
    maxBlock = std::max (16, maxBlockIn);
    numCh = juce::jlimit (1, 2, numChannels);
    maxLook = (int) std::ceil (kMaxLookaheadMs * 0.001 * fs);
    firLen = fs <= 50000.0 ? 8192 : (fs <= 100000.0 ? 16384 : 32768);

    const int maxDelay = maxLook + firLen / 2 + maxBlock + 4;
    for (auto& d : inDelay) d.prepare (maxDelay);
    for (auto& d : scDelay) d.prepare (maxDelay);

    work.setSize (2, maxBlock);
    detIn.setSize (2, maxBlock);
    scIn.setSize (2, maxBlock);
    dry.setSize (2, maxBlock);
    soloAcc.setSize (2, maxBlock);

    juce::dsp::ProcessSpec spec { fs, (juce::uint32) maxBlock, 2 };
    for (auto& b : bands)
    {
        b.gainDb[0].assign ((size_t) maxBlock, 0.0f);
        b.gainDb[1].assign ((size_t) maxBlock, 0.0f);
        for (auto& l : b.lane) l.gainDelay.prepare (maxLook + 2);
        b.conv = std::make_unique<juce::dsp::Convolution>();
        b.conv->prepare (spec);
        b.convOut.setSize (2, maxBlock);
        b.irState.store (0);
        b.irWarmup = 0;
        b.irActive = false;
    }
    outGain.reset (fs, 0.02);
    reset();
}

void Engine::reset()
{
    for (auto& d : inDelay) d.reset();
    for (auto& d : scDelay) d.reset();
    for (auto& b : bands)
    {
        for (auto& l : b.lane)
        {
            l.detHp.reset(); l.detLp.reset();
            l.env.reset();
            l.peak = 0.0;
            l.gainDelay.reset();
            l.shelfLo.reset(); l.shelfHi.reset();
            l.lastG = -1.0;
            l.soloHp.reset(); l.soloLp.reset();
        }
        b.wasOn = false;
        if (b.conv) b.conv->reset();
    }
    for (auto& m : mp) m.reset();
    mpLastEdges.clear();
}

//==============================================================================
float Engine::bandMagnitudeLinear (const BandParams& p, double hz, double sr)
{
    const double e = p.slopeDb() / 6.0;
    double m = 1.0;
    if (p.lo > 10.5f)
    {
        const double x = std::pow (hz / p.lo, e);
        m *= x / (1.0 + x);
    }
    if (p.hi < 0.45 * sr && p.hi < 29000.0f)
        m *= 1.0 / (1.0 + std::pow (hz / p.hi, e));
    return (float) m;
}

bool Engine::requestFirUpdate (int bi, const BandParams& p)
{
    auto& b = bands[(size_t) bi];
    if (b.irState.load() != 0) return false; // previous IR not yet picked up

    const int n = firLen;
    const int order = juce::roundToInt (std::log2 ((double) n));
    juce::dsp::FFT fft (order);
    std::vector<float> spec ((size_t) n * 2, 0.0f);
    for (int k = 0; k <= n / 2; ++k)
        spec[(size_t) k * 2] = bandMagnitudeLinear (p, k * fs / n, fs);
    fft.performRealOnlyInverseTransform (spec.data());

    juce::AudioBuffer<float> ir (1, n);
    auto* h = ir.getWritePointer (0);
    for (int i = 0; i < n; ++i)
    {
        const int src = (i - n / 2 + n) % n;
        const double w = 0.42 - 0.5 * std::cos (2.0 * dsp::kPi * i / n) + 0.08 * std::cos (4.0 * dsp::kPi * i / n);
        h[i] = (float) (spec[(size_t) src] * w);
    }
    b.pendingIr = std::move (ir);
    b.irState.store (1);
    return true;
}

void Engine::primeLinearPhase()
{
    juce::AudioBuffer<float> zin (2, maxBlock), zout (2, maxBlock);
    zin.clear();
    juce::dsp::AudioBlock<float> ib (zin), ob (zout);
    for (auto& b : bands)
        if (b.irState.load() == 1)
        {
            b.conv->loadImpulseResponse (std::move (b.pendingIr), fs, juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
            b.irState.store (0);
        }
    for (int tries = 0; tries < 400; ++tries)
    {
        bool all = true;
        for (auto& b : bands)
        {
            b.conv->process (juce::dsp::ProcessContextNonReplacing<float> (ib, ob));
            all &= b.conv->getCurrentIRSize() == firLen;
        }
        if (all) break;
        juce::Thread::sleep (1);
    }
    // let the internal crossfade finish, then clear the history
    for (int done = 0; done < (int) (0.08 * fs); done += maxBlock)
        for (auto& b : bands) b.conv->process (juce::dsp::ProcessContextNonReplacing<float> (ib, ob));
    for (auto& b : bands)
    {
        b.conv->reset();
        b.irActive = b.conv->getCurrentIRSize() == firLen;
    }
}

//==============================================================================
void Engine::process (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>* sidechain,
                      const GlobalParams& g, const std::array<BandParams, kNumBands>& bp)
{
    const int total = main.getNumSamples();
    const int nCh = std::min (numCh, main.getNumChannels());
    if (nCh <= 0 || total <= 0) return;

    if (g.phase != lastMode || g.lookahead != lastLookahead)
    {
        reset();
        lastMode = g.phase;
        lastLookahead = g.lookahead;
    }

    // hand new linear-phase IRs to the convolution engines (wait-free call)
    for (auto& b : bands)
        if (b.irState.load() == 1)
        {
            b.conv->loadImpulseResponse (std::move (b.pendingIr), fs, juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
            b.irState.store (0);
        }

    const bool linear = g.phase == PhaseMode::Linear;
    const int la = g.lookahead ? maxLook : 0;
    const int firHalf = linear ? firLen / 2 : 0;
    const bool haveSc = sidechain != nullptr && sidechain->getNumChannels() > 0;
    outGain.setTargetValue ((float) dbToGain (g.outDb));

    float peakL = 0, peakR = 0;
    std::array<float, kNumBands> mGain {}, mDyn {}, mLev;
    mLev.fill (-120.0f);

    for (int start = 0; start < total; start += maxBlock)
    {
        const int n = std::min (maxBlock, total - start);
        float* ch[2] = { main.getWritePointer (0, start), main.getWritePointer (nCh > 1 ? 1 : 0, start) };

        {
            const float* pre[2] = { ch[0], ch[1] };
            pushFifo (preFifo, preData, pre, nCh, n);
        }

        // 1. delay lines
        for (int c = 0; c < nCh; ++c)
        {
            for (int i = 0; i < n; ++i) inDelay[(size_t) c].push (ch[c][i]);
            if (haveSc)
            {
                const float* s = sidechain->getReadPointer (std::min (c, sidechain->getNumChannels() - 1), start);
                for (int i = 0; i < n; ++i) scDelay[(size_t) c].push (s[i]);
            }
            auto* w = work.getWritePointer (c);
            auto* d = detIn.getWritePointer (c);
            auto* s = scIn.getWritePointer (c);
            auto* dr = dry.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                const int age = n - 1 - i;
                w[i] = inDelay[(size_t) c].get (age + la);
                d[i] = inDelay[(size_t) c].get (age + firHalf);
                s[i] = haveSc ? scDelay[(size_t) c].get (age + firHalf) : 0.0f;
                if (linear) dr[i] = inDelay[(size_t) c].get (age + la + firHalf);
            }
        }

        // 2. detection -> per-lane gain curves
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& band = bands[(size_t) b];
            const auto& p = bp[(size_t) b];
            if (! p.on)
            {
                band.wasOn = false;
                continue;
            }
            // smooth band edges in the log domain
            if (! band.wasOn)
            {
                band.smLo = p.lo; band.smHi = p.hi;
                for (auto& l : band.lane) { l.env.reset(); l.peak = 0.0; l.lastG = -1.0; }
                band.irWarmup = firLen + (int) (0.05 * fs);
            }
            else
            {
                const float a = 1.0f - std::exp (-(float) n / (0.03f * (float) fs));
                band.smLo = std::exp (std::log (band.smLo) + (std::log (p.lo) - std::log (band.smLo)) * a);
                band.smHi = std::exp (std::log (band.smHi) + (std::log (p.hi) - std::log (band.smHi)) * a);
            }
            band.wasOn = true;

            const float* det[2];
            if (p.scExt && haveSc) { det[0] = scIn.getReadPointer (0); det[1] = scIn.getReadPointer (nCh > 1 ? 1 : 0); }
            else                   { det[0] = detIn.getReadPointer (0); det[1] = detIn.getReadPointer (nCh > 1 ? 1 : 0); }
            processDetection (b, p, n, la, g.mix, det, nCh);

            float gmax = 0, dmax = 0;
            for (int l = 0; l < 2; ++l)
                for (int i = 0; i < n; ++i)
                    if (std::abs (band.gainDb[l][(size_t) i]) > std::abs (gmax)) gmax = band.gainDb[l][(size_t) i];
            dmax = (float) (band.lane[0].env.fast > band.lane[1].env.fast ? band.lane[0].env.fast : band.lane[1].env.fast);
            mGain[(size_t) b] = gmax;
            mDyn[(size_t) b] = (p.range < 0 ? -1.0f : 1.0f) * dmax;
        }

        // 3. audio path
        if (linear)
        {
            float* w[2] = { work.getWritePointer (0), work.getWritePointer (1) };
            if (nCh == 1) juce::FloatVectorOperations::copy (w[1], w[0], n);
            processLinearPhase (bp, ch, nCh, n);
        }
        else
        {
            for (int c = 0; c < nCh; ++c)
                juce::FloatVectorOperations::copy (ch[c], work.getReadPointer (c), n);
            if (g.phase == PhaseMode::Dynamic) processDynamicPhase (bp, ch, nCh, n);
            else                               processMinimumPhase (bp, ch, nCh, n);
        }

        applySolo (bp, ch, nCh, n);

        // 4. output gain + metering
        for (int i = 0; i < n; ++i)
        {
            const float og = outGain.getNextValue();
            ch[0][i] *= og;
            peakL = std::max (peakL, std::abs (ch[0][i]));
            if (nCh > 1)
            {
                ch[1][i] *= og;
                peakR = std::max (peakR, std::abs (ch[1][i]));
            }
        }
        if (nCh == 1) peakR = peakL;

        {
            const float* post[2] = { ch[0], ch[1] };
            pushFifo (postFifo, postData, post, nCh, n);
        }
    }

    for (int b = 0; b < kNumBands; ++b)
    {
        meterGainDb[(size_t) b].store (mGain[(size_t) b]);
        meterDynDb[(size_t) b].store (mDyn[(size_t) b]);
    }
    meterOutL.store (juce::Decibels::gainToDecibels (peakL, -120.0f));
    meterOutR.store (juce::Decibels::gainToDecibels (peakR, -120.0f));

    // copy mono to second channel for mono-in/stereo-out layouts
    for (int c = nCh; c < main.getNumChannels(); ++c)
        main.copyFrom (c, 0, main, 0, 0, total);
}

//==============================================================================
void Engine::processDetection (int b, const BandParams& p, int n, int laGlobal, float mix,
                               const float* const* det, int nCh)
{
    auto& band = bands[(size_t) b];
    const auto sm = (StereoMode) p.stereo;
    const bool stereo = nCh > 1;
    const bool ms = stereo && (sm == StereoMode::Mid || sm == StereoMode::Side);
    bool active[2] { true, stereo };
    if (stereo)
    {
        if (sm == StereoMode::Mid || sm == StereoMode::Left) active[1] = false;
        if (sm == StereoMode::Side || sm == StereoMode::Right) active[0] = false;
    }

    const float lo = p.scFree ? p.scLo : band.smLo;
    const float hi = p.scFree ? std::max (p.scHi, p.scLo * 1.01f) : band.smHi;
    const int ord = p.detOrder();
    for (auto& l : band.lane)
    {
        l.detHp.setup (true, ord, lo, fs, lo > 10.5f);
        l.detLp.setup (false, ord, hi, fs, hi < 0.45 * fs && hi < 29000.0f);
        l.env.set (p.atkMs, p.relMs, fs);
    }

    auto& gc = band.gc;
    gc.thr = p.thr; gc.range = p.range; gc.ratio = p.ratio; gc.knee = p.knee;
    gc.expand = p.mode == (int) DynMode::Expand;
    const double sign = p.range < 0 ? -1.0 : 1.0;
    const double link = p.link * 0.01;
    const bool useLink = stereo && sm == StereoMode::Stereo;
    const int gDelay = laGlobal > 0 ? std::clamp (laGlobal - (int) std::round (p.lookMs * 0.001 * fs), 0, laGlobal) : 0;
    const double staticDb = p.gainDb;
    float levMax = -120.0f;
    // instant-attack peak follower so the level does not drop in the troughs of a waveform;
    // its release follows the lowest frequency of the detector band
    const double tau = std::clamp (3.0 / std::max (lo, 10.0f), 0.001, 0.03);
    const double rPk = std::exp (-1.0 / (tau * fs));

    for (int i = 0; i < n; ++i)
    {
        double a = det[0][i], c = stereo ? det[1][i] : a;
        double x0 = a, x1 = c;
        if (ms) { x0 = 0.5 * (a + c); x1 = 0.5 * (a - c); }

        double lev[2];
        {
            auto& l0 = band.lane[0];
            const double v0 = std::abs (l0.detLp.process (l0.detHp.process (x0)));
            l0.peak = v0 > l0.peak ? v0 : l0.peak * rPk;
            lev[0] = l0.peak;
            if (stereo)
            {
                auto& l1 = band.lane[1];
                const double v1 = std::abs (l1.detLp.process (l1.detHp.process (x1)));
                l1.peak = v1 > l1.peak ? v1 : l1.peak * rPk;
                lev[1] = l1.peak;
            }
            else lev[1] = lev[0];
        }
        if (useLink)
        {
            const double m = std::max (lev[0], lev[1]);
            lev[0] = link * m + (1.0 - link) * lev[0];
            lev[1] = link * m + (1.0 - link) * lev[1];
        }
        for (int l = 0; l < 2; ++l)
        {
            const double ldb = 20.0 * std::log10 (std::max (lev[l], 1.0e-7));
            if (active[l]) levMax = std::max (levMax, (float) ldb);
            const double mag = band.lane[(size_t) l].env.process (gc.magnitude (ldb));
            double tot = 0.0;
            if (active[l] && ! p.bypass) tot = std::clamp (mix * (sign * mag + staticDb), -80.0, 48.0);
            auto& gd = band.lane[(size_t) l].gainDelay;
            gd.push ((float) tot);
            band.gainDb[l][(size_t) i] = gd.get (gDelay);
        }
    }
    meterLevelDb[(size_t) b].store (levMax);
}

//==============================================================================
void Engine::processDynamicPhase (const std::array<BandParams, kNumBands>& bp, float* const* ch, int nCh, int n)
{
    constexpr int kSub = 4;
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& p = bp[(size_t) b];
        if (! p.on || p.bypass) continue;
        auto& band = bands[(size_t) b];
        const float lo = band.smLo, hi = std::max (band.smHi, band.smLo * 1.01f);
        const bool useLo = lo > 10.5f;
        const bool useHi = hi < 0.45 * fs && hi < 29000.0f;
        const bool stereo = nCh > 1;
        const auto sm = (StereoMode) p.stereo;
        const bool ms = stereo && (sm == StereoMode::Mid || sm == StereoMode::Side);
        const int order = p.shelfOrder();
        for (auto& l : band.lane) { l.shelfLo.setOrder (order); l.shelfHi.setOrder (order); }

        for (int s0 = 0; s0 < n; s0 += kSub)
        {
            const int s1 = std::min (n, s0 + kSub);
            double G[2];
            for (int l = 0; l < (stereo ? 2 : 1); ++l)
            {
                auto& ln = band.lane[(size_t) l];
                // use the strongest gain change inside the sub-block so fast attacks are not missed
                float gdb = band.gainDb[l][(size_t) s0];
                for (int i = s0 + 1; i < s1; ++i)
                    if (std::abs (band.gainDb[l][(size_t) i]) > std::abs (gdb)) gdb = band.gainDb[l][(size_t) i];
                G[l] = dbToGain (gdb);
                if (std::abs (G[l] - ln.lastG) > 1.0e-7 || lo != ln.lastLo || hi != ln.lastHi)
                {
                    if (useLo) ln.shelfLo.setup (lo, G[l], fs);
                    if (useHi)
                    {
                        if (useLo) ln.shelfHi.setup (hi, 1.0 / G[l], fs);
                        else       ln.shelfHi.setupLow (hi, G[l], fs); // low band: plain low shelf
                    }
                    ln.lastG = G[l]; ln.lastLo = lo; ln.lastHi = hi;
                }
            }
            for (int i = s0; i < s1; ++i)
            {
                double x[2] = { ch[0][i], stereo ? ch[1][i] : 0.0 };
                if (ms) { const double m = 0.5 * (x[0] + x[1]), s = 0.5 * (x[0] - x[1]); x[0] = m; x[1] = s; }
                for (int l = 0; l < (stereo ? 2 : 1); ++l)
                {
                    auto& ln = band.lane[(size_t) l];
                    if (ln.lastG == 1.0 && band.gainDb[l][(size_t) i] == 0.0f) continue; // exact identity
                    double y = x[l];
                    if (useLo) y = ln.shelfLo.process (y);
                    else if (! useHi) y *= dbToGain (band.gainDb[l][(size_t) i]);
                    if (useHi) y = ln.shelfHi.process (y);
                    x[l] = y;
                }
                if (ms) { const double L = x[0] + x[1], R = x[0] - x[1]; x[0] = L; x[1] = R; }
                ch[0][i] = (float) x[0];
                if (stereo) ch[1][i] = (float) x[1];
            }
        }
    }
}

//==============================================================================
void Engine::processMinimumPhase (const std::array<BandParams, kNumBands>& bp, float* const* ch, int nCh, int n)
{
    const float hiLimit = (float) std::min (0.45 * fs, 29000.0);
    // collect and merge crossover edges of all active bands
    std::vector<std::pair<float, int>> edges;
    edges.reserve (kMaxEdges);
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& p = bp[(size_t) b];
        if (! p.on || p.bypass) continue;
        const auto& band = bands[(size_t) b];
        const float lo = band.smLo, hi = std::max (band.smHi, band.smLo * 1.01f);
        if (lo > 10.5f) edges.push_back ({ lo, p.lrOrder() });
        if (hi < hiLimit) edges.push_back ({ hi, p.lrOrder() });
    }
    std::sort (edges.begin(), edges.end());
    std::vector<std::pair<float, int>> merged;
    for (auto& e : edges)
    {
        if (! merged.empty() && e.first < merged.back().first * 1.004f)
            merged.back().second = std::max (merged.back().second, e.second);
        else
            merged.push_back (e);
    }
    const int K = (int) merged.size();
    // reset the tree when its structure changes (band switched, edges crossed)
    bool topo = (int) mpLastEdges.size() != K;
    for (int k = 0; ! topo && k < K; ++k)
        topo = mpLastEdges[(size_t) k].second != merged[(size_t) k].second
               || std::abs (std::log (mpLastEdges[(size_t) k].first / merged[(size_t) k].first)) > 0.7f;
    if (topo) for (auto& m : mp) m.reset();
    mpLastEdges = merged;

    for (int c = 0; c < nCh; ++c)
        for (int k = 0; k < K; ++k)
        {
            mp[(size_t) c].split[k].setup (merged[(size_t) k].second, merged[(size_t) k].first, fs);
            for (int j = k + 1; j < K; ++j)
                mp[(size_t) c].ap[k][j].setup (merged[(size_t) j].second, merged[(size_t) j].first, fs);
        }

    // which bands cover which segment (segment s lies between edge s-1 and edge s)
    struct Cover { int band; bool ms; };
    std::vector<Cover> cover[kMaxEdges + 1];
    for (int sgi = 0; sgi <= K; ++sgi)
    {
        const float segLo = sgi == 0 ? 0.0f : merged[(size_t) sgi - 1].first;
        const float segHi = sgi == K ? 1.0e9f : merged[(size_t) sgi].first;
        for (int b = 0; b < kNumBands; ++b)
        {
            const auto& p = bp[(size_t) b];
            if (! p.on || p.bypass) continue;
            const auto& band = bands[(size_t) b];
            const float lo = band.smLo <= 10.5f ? 0.0f : band.smLo;
            const float hi = std::max (band.smHi, band.smLo * 1.01f) >= hiLimit ? 1.0e9f : std::max (band.smHi, band.smLo * 1.01f);
            if (lo <= segLo * 1.005f && hi >= segHi * 0.995f)
            {
                const auto sm = (StereoMode) p.stereo;
                cover[sgi].push_back ({ b, nCh > 1 && (sm == StereoMode::Mid || sm == StereoMode::Side) });
            }
        }
    }

    const bool stereo = nCh > 1;
    double seg[2][kMaxEdges + 1];
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < nCh; ++c)
        {
            auto& m = mp[(size_t) c];
            double rest = ch[c][i], low, high;
            for (int k = 0; k < K; ++k)
            {
                m.split[k].process (rest, low, high);
                for (int j = k + 1; j < K; ++j) low = m.ap[k][j].process (low);
                seg[c][k] = low;
                rest = high;
            }
            seg[c][K] = rest;
        }
        double outL = 0, outR = 0;
        for (int sgi = 0; sgi <= K; ++sgi)
        {
            double a = seg[0][sgi], c = stereo ? seg[1][sgi] : 0.0;
            for (auto& cv : cover[sgi])
            {
                const auto& band = bands[(size_t) cv.band];
                const double g0 = dbToGain (band.gainDb[0][(size_t) i]);
                const double g1 = stereo ? dbToGain (band.gainDb[1][(size_t) i]) : 1.0;
                if (cv.ms)
                {
                    const double mid = 0.5 * (a + c) * g0, side = 0.5 * (a - c) * g1;
                    a = mid + side;
                    c = mid - side;
                }
                else
                {
                    a *= g0;
                    c *= g1;
                }
            }
            outL += a;
            outR += c;
        }
        ch[0][i] = (float) outL;
        if (stereo) ch[1][i] = (float) outR;
    }
}

//==============================================================================
void Engine::processLinearPhase (const std::array<BandParams, kNumBands>& bp, float* const* ch, int nCh, int n)
{
    const bool stereo = nCh > 1;
    for (int c = 0; c < nCh; ++c)
        juce::FloatVectorOperations::copy (ch[c], dry.getReadPointer (c), n);

    juce::dsp::AudioBlock<float> inBlock (work.getArrayOfWritePointers(), 2, 0, (size_t) n);

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& p = bp[(size_t) b];
        if (! p.on) continue;
        auto& band = bands[(size_t) b];

        juce::dsp::AudioBlock<float> outBlock (band.convOut.getArrayOfWritePointers(), 2, 0, (size_t) n);
        band.conv->process (juce::dsp::ProcessContextNonReplacing<float> (inBlock, outBlock));
        if (! band.irActive && band.conv->getCurrentIRSize() == firLen)
        {
            band.irActive = true;
            band.irWarmup = std::max (band.irWarmup, (int) (0.06 * fs)); // internal crossfade from the default IR
        }
        if (! band.irActive) continue;
        if (band.irWarmup > 0)
        {
            band.irWarmup -= n;
            continue;
        }
        if (p.bypass) continue;

        const auto sm = (StereoMode) p.stereo;
        const bool ms = stereo && (sm == StereoMode::Mid || sm == StereoMode::Side);
        const float* s0 = band.convOut.getReadPointer (0);
        const float* s1 = band.convOut.getReadPointer (1);
        for (int i = 0; i < n; ++i)
        {
            double a = s0[i], c = stereo ? s1[i] : a;
            if (ms) { const double m = 0.5 * (a + c), s = 0.5 * (a - c); a = m; c = s; }
            const double d0 = (dbToGain (band.gainDb[0][(size_t) i]) - 1.0) * a;
            const double d1 = stereo ? (dbToGain (band.gainDb[1][(size_t) i]) - 1.0) * c : 0.0;
            if (ms)
            {
                ch[0][i] += (float) (d0 + d1);
                ch[1][i] += (float) (d0 - d1);
            }
            else
            {
                ch[0][i] += (float) d0;
                if (stereo) ch[1][i] += (float) d1;
            }
        }
    }
}

//==============================================================================
void Engine::applySolo (const std::array<BandParams, kNumBands>& bp, float* const* ch, int nCh, int n)
{
    bool any = false;
    for (auto& p : bp) any |= (p.on && p.solo);
    if (! any) return;

    soloAcc.clear();
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& p = bp[(size_t) b];
        if (! (p.on && p.solo)) continue;
        auto& band = bands[(size_t) b];
        const float lo = band.smLo, hi = std::max (band.smHi, band.smLo * 1.01f);
        for (int c = 0; c < nCh; ++c)
        {
            auto& ln = band.lane[(size_t) c];
            ln.soloHp.setup (true, p.detOrder(), lo, fs, lo > 10.5f);
            ln.soloLp.setup (false, p.detOrder(), hi, fs, hi < 0.45 * fs && hi < 29000.0f);
            auto* acc = soloAcc.getWritePointer (c);
            for (int i = 0; i < n; ++i)
                acc[i] += (float) ln.soloLp.process (ln.soloHp.process (ch[c][i]));
        }
    }
    for (int c = 0; c < nCh; ++c)
        juce::FloatVectorOperations::copy (ch[c], soloAcc.getReadPointer (c), n);
}

void Engine::pushFifo (juce::AbstractFifo& f, std::vector<float>& data, const float* const* ch, int nCh, int n)
{
    if (f.getFreeSpace() < n) return;
    int s1, n1, s2, n2;
    f.prepareToWrite (n, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) data[(size_t) (s1 + i)] = nCh > 1 ? 0.5f * (ch[0][i] + ch[1][i]) : ch[0][i];
    for (int i = 0; i < n2; ++i) data[(size_t) (s2 + i)] = nCh > 1 ? 0.5f * (ch[0][n1 + i] + ch[1][n1 + i]) : ch[0][n1 + i];
    f.finishedWrite (n1 + n2);
}
} // namespace bw
