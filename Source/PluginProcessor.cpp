#include "PluginProcessor.h"
#include "PluginEditor.h"

BandwerkProcessor::BandwerkProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "BANDWERK", bw::createLayout())
{
    auto P = [this] (const juce::String& id) { return apvts.getRawParameterValue (id); };
    for (int b = 0; b < bw::kNumBands; ++b)
    {
        auto B = [&] (const char* n) { return P (bw::bandId (b, n)); };
        bp[(size_t) b] = { B ("on"), B ("lo"), B ("hi"), B ("thr"), B ("range"), B ("ratio"), B ("knee"), B ("atk"),
                           B ("rel"), B ("look"), B ("gain"), B ("mode"), B ("stereo"), B ("link"), B ("slope"),
                           B ("scext"), B ("scfree"), B ("sclo"), B ("schi"), B ("solo"), B ("byp") };
    }
    pPhase = P (bw::id::phase);
    pLook = P (bw::id::lookahead);
    pMix = P (bw::id::mix);
    pOut = P (bw::id::output);
    firDirty.fill (true);
    startTimerHz (20);
}

BandwerkProcessor::~BandwerkProcessor() { stopTimer(); }

bw::BandParams BandwerkProcessor::readBand (int b) const
{
    const auto& p = bp[(size_t) b];
    bw::BandParams r;
    r.on = p.on->load() > 0.5f;
    r.lo = p.lo->load();
    r.hi = std::max (p.hi->load(), r.lo * 1.01f);
    r.thr = p.thr->load();
    r.range = p.range->load();
    r.ratio = p.ratio->load();
    r.knee = p.knee->load();
    r.atkMs = p.atk->load();
    r.relMs = p.rel->load();
    r.lookMs = p.look->load();
    r.gainDb = p.gain->load();
    r.mode = (int) p.mode->load();
    r.stereo = (int) p.stereo->load();
    r.link = p.link->load();
    r.slopeIdx = (int) p.slope->load();
    r.scExt = p.scext->load() > 0.5f;
    r.scFree = p.scfree->load() > 0.5f;
    r.scLo = p.sclo->load();
    r.scHi = p.schi->load();
    r.solo = p.solo->load() > 0.5f;
    r.bypass = p.byp->load() > 0.5f;
    return r;
}

bw::GlobalParams BandwerkProcessor::readGlobal() const
{
    bw::GlobalParams g;
    g.phase = (bw::PhaseMode) (int) pPhase->load();
    g.lookahead = pLook->load() > 0.5f;
    g.mix = pMix->load() * 0.01f;
    g.outDb = pOut->load();
    return g;
}

void BandwerkProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepared = false;
    const int ch = std::max (1, getMainBusNumOutputChannels());
    engine.prepare (sampleRate, samplesPerBlock, ch);
    firDirty.fill (true);
    updateFirs (true);
    const auto g = readGlobal();
    if (g.phase == bw::PhaseMode::Linear) engine.primeLinearPhase();
    setLatencySamples (engine.latencyFor (g.phase, g.lookahead));
    prepared = true;
}

bool BandwerkProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (layouts.getMainInputChannelSet() != out) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void BandwerkProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto mainBuf = getBusBuffer (buffer, true, 0);
    const juce::AudioBuffer<float>* sc = nullptr;
    juce::AudioBuffer<float> scBuf;
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        scBuf = getBusBuffer (buffer, true, 1);
        if (scBuf.getNumChannels() > 0) sc = &scBuf;
    }
    sidechainConnected.store (sc != nullptr);

    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    std::array<bw::BandParams, bw::kNumBands> bands;
    for (int b = 0; b < bw::kNumBands; ++b) bands[(size_t) b] = readBand (b);
    engine.process (mainBuf, sc, readGlobal(), bands);
}

void BandwerkProcessor::updateFirs (bool force)
{
    for (int b = 0; b < bw::kNumBands; ++b)
    {
        const auto p = readBand (b);
        const juce::int64 h = (juce::int64) juce::roundToInt (std::log (p.lo) * 2000.0) * 1000003
                              + (juce::int64) juce::roundToInt (std::log (p.hi) * 2000.0) * 101 + p.slopeIdx
                              + (juce::int64) (engine.getSampleRate()) * 7919;
        if (h != firHash[(size_t) b]) firDirty[(size_t) b] = true;
        if ((firDirty[(size_t) b] || force) && engine.requestFirUpdate (b, p))
        {
            firHash[(size_t) b] = h;
            firDirty[(size_t) b] = false;
        }
    }
}

void BandwerkProcessor::timerCallback()
{
    if (! prepared) return;
    const auto g = readGlobal();
    if (g.phase == bw::PhaseMode::Linear) updateFirs (false);
    const int lat = engine.latencyFor (g.phase, g.lookahead);
    if (lat != getLatencySamples()) setLatencySamples (lat);
}

juce::AudioProcessorEditor* BandwerkProcessor::createEditor() { return new BandwerkEditor (*this); }

void BandwerkProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("selectedBand", selectedBand.load(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void BandwerkProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            selectedBand = (int) tree.getProperty ("selectedBand", -1);
            apvts.replaceState (tree);
        }
}

#if ! BANDWERK_TEST_BUILD
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BandwerkProcessor(); }
#endif
