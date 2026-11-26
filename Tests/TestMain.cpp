// Measurement program: runs the processor offline and checks the DSP against
// the expected curves. Also renders an editor snapshot.
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <random>
#include <chrono>

using namespace bw;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? " ok " : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

struct Rig
{
    std::unique_ptr<BandwerkProcessor> p;
    double fs;
    int block;

    explicit Rig (double sampleRate = 48000.0, int blockSize = 512, bool stereo = true) : fs (sampleRate), block (blockSize)
    {
        p = std::make_unique<BandwerkProcessor>();
        if (! stereo)
        {
            auto l = p->getBusesLayout();
            l.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
            l.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
            p->setBusesLayout (l);
        }
        p->setRateAndBufferSizeDetails (fs, block);
        p->prepareToPlay (fs, block);
    }
    void set (const juce::String& id, float v)
    {
        auto* prm = p->apvts.getParameter (id);
        jassert (prm != nullptr);
        prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    }
    void band (int b, const char* n, float v) { set (bandId (b, n), v); }
    void reprepare() { p->prepareToPlay (fs, block); }

    // processes main (2 ch) and optional sidechain (2 ch); returns output (2 ch)
    juce::AudioBuffer<float> run (const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>* sc = nullptr)
    {
        const int n = in.getNumSamples();
        const int chMain = p->getMainBusNumInputChannels();
        juce::AudioBuffer<float> out (chMain, n);
        juce::AudioBuffer<float> buf (chMain + 2, block);
        juce::MidiBuffer midi;
        for (int s = 0; s < n; s += block)
        {
            const int len = std::min (block, n - s);
            buf.setSize (chMain + 2, len, false, false, true);
            buf.clear();
            for (int c = 0; c < chMain; ++c) buf.copyFrom (c, 0, in, std::min (c, in.getNumChannels() - 1), s, len);
            if (sc) for (int c = 0; c < 2; ++c) buf.copyFrom (chMain + c, 0, *sc, c, s, len);
            p->processBlock (buf, midi);
            for (int c = 0; c < chMain; ++c) out.copyFrom (c, s, buf, c, 0, len);
        }
        return out;
    }
};

static juce::AudioBuffer<float> sine (double fs, double hz, float db, int n, double phase = 0.0)
{
    juce::AudioBuffer<float> b (2, n);
    const float a = juce::Decibels::decibelsToGain (db);
    for (int i = 0; i < n; ++i)
    {
        const float v = a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / fs + phase);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

static juce::AudioBuffer<float> noise (int n, float db, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> d (0.0f, juce::Decibels::decibelsToGain (db));
    juce::AudioBuffer<float> b (2, n);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) b.setSample (c, i, d (rng));
    return b;
}

static float peakDb (const juce::AudioBuffer<float>& b, int ch, int start, int len)
{
    float m = 0;
    for (int i = start; i < start + len; ++i) m = std::max (m, std::abs (b.getSample (ch, i)));
    return juce::Decibels::gainToDecibels (m, -200.0f);
}

static float rmsDb (const juce::AudioBuffer<float>& b, int ch, int start, int len)
{
    double s = 0;
    for (int i = start; i < start + len; ++i) s += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return juce::Decibels::gainToDecibels ((float) std::sqrt (s / len), -200.0f);
}

// magnitude response of the processed impulse (after removing latency) at hz
struct Response
{
    std::vector<float> mag;
    int n;
    double fs;
    float at (double hz) const
    {
        const double bin = hz * n / fs;
        const int i = (int) bin;
        const double fr = bin - i;
        return (float) (mag[(size_t) i] * (1 - fr) + mag[(size_t) i + 1] * fr);
    }
};

static Response impulseResponse (Rig& r, int ch = 0)
{
    const int order = 16, n = 1 << order; // 65536 samples
    const int lat = r.p->getLatencySamples();
    // let smoothing settle with silence first
    juce::AudioBuffer<float> pre (2, (int) r.fs);
    pre.clear();
    r.run (pre);
    juce::AudioBuffer<float> in (2, n + lat);
    in.clear();
    in.setSample (0, 0, 1.0f);
    in.setSample (1, 0, 1.0f);
    auto out = r.run (in);
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) n * 2, 0.0f);
    for (int i = 0; i < n; ++i) data[(size_t) i] = out.getSample (ch, i); // keeps the pre-ringing of linear-phase filters
    fft.performFrequencyOnlyForwardTransform (data.data());
    Response res;
    res.n = n;
    res.fs = r.fs;
    res.mag.resize ((size_t) n / 2 + 1);
    for (int k = 0; k <= n / 2; ++k) res.mag[(size_t) k] = juce::Decibels::gainToDecibels (data[(size_t) k], -200.0f);
    return res;
}

static const char* modeName (int m) { return m == 0 ? "Dynamic" : m == 1 ? "Linear" : "Minimum"; }

static float expectedStatic (const BandParams& p, float db, float hz) // for dynamic phase
{
    return ui::BandDisplay::bandResponseDb (p, db, hz, 48000.0);
}

//==============================================================================
static void testTransparency()
{
    std::printf ("\n1. Transparenz (Baender aktiv, aber ohne Gain-Aenderung)\n");
    for (int m = 0; m < 3; ++m)
    {
        Rig r;
        r.set (id::phase, (float) m);
        for (int b = 0; b < kNumBands; ++b)
        {
            r.band (b, "on", 1);
            r.band (b, "thr", 0);
        }
        r.reprepare();
        auto in = noise (48000 * 3, -40.0f);
        auto out = r.run (in);
        const int lat = r.p->getLatencySamples();
        if (m == 2)
        {
            auto resp = impulseResponse (r);
            float mn = 100, mx = -100;
            for (double f = 20; f < 20000; f *= 1.05) { mn = std::min (mn, resp.at (f)); mx = std::max (mx, resp.at (f)); }
            check (mx - mn < 0.05f, juce::String (modeName (m)) + ": Betragsfrequenzgang flach, Abweichung "
                                       + juce::String (mx - mn, 4) + " dB (Allpass-Phasenverschiebung erwartet)");
            continue;
        }
        double err = 0;
        for (int i = 48000; i < in.getNumSamples() - lat; ++i)
            err = std::max (err, (double) std::abs (out.getSample (0, i + lat) - in.getSample (0, i)));
        check (err < 1.0e-6, juce::String (modeName (m)) + ": Ausgang = Eingang, max. Fehler "
                                 + juce::String (juce::Decibels::gainToDecibels ((float) err, -200.0f), 1)
                                 + " dB re Signal -40 dB, Latenz " + juce::String (lat) + " Samples");
    }
}

static void testStaticShape()
{
    std::printf ("\n2. Bandform bei statischem Gain (500 Hz - 2 kHz, -12 dB, 24 dB/oct)\n");
    for (int m = 0; m < 3; ++m)
    {
        Rig r;
        r.set (id::phase, (float) m);
        r.band (0, "on", 1);
        r.band (0, "lo", 500);
        r.band (0, "hi", 2000);
        r.band (0, "range", 0);
        r.band (0, "gain", -12);
        r.reprepare();
        auto resp = impulseResponse (r);
        juce::String row;
        for (double f : { 100.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 10000.0 })
            row << freqText ((float) f) << ": " << juce::String (resp.at (f), 2) << "   ";
        std::printf ("  %s  %s\n", modeName (m), row.toRawUTF8());
        // Dynamic phase reaches the set gain; crossover-based modes reach 1 + (G-1) * M with M = 0.886
        // in the middle of a two-octave band at 24 dB/oct (-9.48 dB)
        const float centre = m == 0 ? -12.0f : -9.48f;
        check (std::abs (resp.at (1000) - centre) < 0.3f && std::abs (resp.at (100)) < 0.5f && std::abs (resp.at (10000)) < 0.5f,
               juce::String (modeName (m)) + ": Bandmitte " + juce::String (centre, 2) + " dB, ausserhalb ~0 dB");
        if (m == 0)
        {
            BandParams p; p.lo = 500; p.hi = 2000; p.slopeIdx = 3;
            float maxDev = 0;
            for (double f = 30; f < 18000; f *= 1.1) maxDev = std::max (maxDev, std::abs (resp.at (f) - expectedStatic (p, -12, (float) f)));
            check (maxDev < 0.5f, "Dynamic: Messung folgt der Anzeige-Kurve, max. Abweichung " + juce::String (maxDev, 2) + " dB");
        }
    }

    std::printf ("\n3. Zwei angrenzende Baender mit gleichem Gain (10 Hz-1 kHz und 1 kHz-30 kHz, je -6 dB)\n");
    for (int m = 0; m < 3; ++m)
    {
        Rig r;
        r.set (id::phase, (float) m);
        for (int b = 0; b < 2; ++b) { r.band (b, "on", 1); r.band (b, "range", 0); r.band (b, "gain", -6); }
        r.band (0, "lo", 10); r.band (0, "hi", 1000);
        r.band (1, "lo", 1000); r.band (1, "hi", 30000);
        r.reprepare();
        auto resp = impulseResponse (r);
        float mn = 100, mx = -100;
        for (double f = 30; f < 18000; f *= 1.03) { mn = std::min (mn, resp.at (f)); mx = std::max (mx, resp.at (f)); }
        check (mx - mn < (m == 2 ? 1.5f : 0.1f), juce::String (modeName (m)) + ": Summe flach bei -6 dB, Bereich "
                                                    + juce::String (mn, 2) + " .. " + juce::String (mx, 2) + " dB");
    }
}

static void testStaticCurve()
{
    std::printf ("\n4. Kennlinie (Breitband, Thr -20 dB, Ratio 4:1, Knee 0, Attack 1 ms, Release 80 ms)\n");
    Rig r;
    r.band (0, "on", 1); r.band (0, "lo", 10); r.band (0, "hi", 30000);
    r.band (0, "thr", -20); r.band (0, "ratio", 4); r.band (0, "knee", 0); r.band (0, "range", -60);
    r.band (0, "atk", 1); r.band (0, "rel", 80);
    juce::String row;
    float worst = 0;
    for (float in : { -40.0f, -30.0f, -20.0f, -15.0f, -10.0f, -5.0f, 0.0f })
    {
        auto s = sine (48000, 1000, in, 48000);
        auto out = r.run (s);
        const float o = peakDb (out, 0, 36000, 12000);
        const float expect = in <= -20.0f ? in : -20.0f + (in + 20.0f) / 4.0f;
        worst = std::max (worst, std::abs (o - expect));
        row << juce::String (in, 0) << "->" << juce::String (o, 2) << "  ";
    }
    std::printf ("  %s\n", row.toRawUTF8());
    check (worst < 0.6f, "Abweichung von der Sollkurve max. " + juce::String (worst, 2) + " dB");

    std::printf ("\n5. Range begrenzt die Reduktion (Range -6 dB, Eingang 0 dB)\n");
    r.band (0, "range", -6);
    auto out = r.run (sine (48000, 1000, 0.0f, 48000));
    const float o = peakDb (out, 0, 36000, 12000);
    check (std::abs (o + 6.0f) < 0.3f, "Ausgang " + juce::String (o, 2) + " dB (Soll -6)");
}

static void testTiming()
{
    std::printf ("\n6. Attack/Release (Sprung -40 -> -10 dB -> -40 dB, Thr -30, Ratio 100, Range -60)\n");
    for (auto [atk, rel] : { std::pair<float, float> { 5.0f, 100.0f }, { 30.0f, 500.0f } })
    {
        Rig r;
        r.band (0, "on", 1); r.band (0, "lo", 10); r.band (0, "hi", 30000);
        r.band (0, "thr", -30); r.band (0, "ratio", 100); r.band (0, "knee", 0); r.band (0, "range", -60);
        r.band (0, "atk", atk); r.band (0, "rel", rel);
        const int n = 48000 * 4;
        juce::AudioBuffer<float> in (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float a = (i >= 48000 && i < 96000) ? juce::Decibels::decibelsToGain (-10.0f) : juce::Decibels::decibelsToGain (-40.0f);
            const float v = a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 2000.0 * i / 48000.0);
            in.setSample (0, i, v); in.setSample (1, i, v);
        }
        auto out = r.run (in);
        // gain = out/in envelope per 1 ms window (peak)
        auto gainAt = [&] (int i) { return peakDb (out, 0, i, 48) - peakDb (in, 0, i, 48); };
        const float final = gainAt (95000);
        int tA = -1;
        for (int i = 48000; i < 96000; i += 24)
            if (gainAt (i) <= final * 0.632f) { tA = i - 48000; break; }
        int tR = -1;
        for (int i = 96000; i < n - 48; i += 24)
            if (gainAt (i) >= final * 0.368f) { tR = i - 96000; break; }
        const float aMs = tA / 48.0f, rMs = tR / 48.0f;
        std::printf ("  Attack %.0f ms: 63%% nach %.1f ms | Release %.0f ms: 63%% Erholung nach %.1f ms | GR %.1f dB\n",
                     atk, aMs, rel, rMs, final);
        check (aMs > atk * 0.5f && aMs < atk * 1.6f, "Attack-Zeit im erwarteten Bereich");
        check (rMs > rel * 0.5f && rMs < rel * 2.0f, "Release-Zeit im erwarteten Bereich (programmabhaengig verlaengert)");
    }
}

static void testModes()
{
    std::printf ("\n7. Upward Compression, Expansion, Gate\n");
    {
        Rig r; // upward: below thr -20 boost, ratio 2, range +12
        r.band (0, "on", 1); r.band (0, "lo", 10); r.band (0, "hi", 30000);
        r.band (0, "thr", -20); r.band (0, "ratio", 2); r.band (0, "knee", 0); r.band (0, "range", 12);
        r.band (0, "atk", 1); r.band (0, "rel", 80);
        auto o1 = peakDb (r.run (sine (48000, 1000, -30.0f, 48000)), 0, 36000, 12000);
        auto o2 = peakDb (r.run (sine (48000, 1000, -60.0f, 48000)), 0, 36000, 12000);
        check (std::abs (o1 + 25.0f) < 0.6f, "Upward -30 dB -> " + juce::String (o1, 2) + " dB (Soll -25)");
        check (std::abs (o2 + 48.0f) < 0.6f, "Upward -60 dB -> " + juce::String (o2, 2) + " dB (Soll -48, Range +12)");
    }
    {
        Rig r; // downward expansion ratio 2 below -30
        r.band (0, "on", 1); r.band (0, "lo", 10); r.band (0, "hi", 30000); r.band (0, "mode", 1);
        r.band (0, "thr", -30); r.band (0, "ratio", 2); r.band (0, "knee", 0); r.band (0, "range", -40);
        r.band (0, "atk", 1); r.band (0, "rel", 80);
        auto o1 = peakDb (r.run (sine (48000, 1000, -40.0f, 48000)), 0, 36000, 12000);
        auto o2 = peakDb (r.run (sine (48000, 1000, -20.0f, 48000)), 0, 36000, 12000);
        check (std::abs (o1 + 50.0f) < 0.6f, "Expander -40 dB -> " + juce::String (o1, 2) + " dB (Soll -50)");
        check (std::abs (o2 + 20.0f) < 0.3f, "Expander -20 dB -> " + juce::String (o2, 2) + " dB (unveraendert)");
        r.band (0, "ratio", 100);
        auto o3 = peakDb (r.run (sine (48000, 1000, -40.0f, 48000)), 0, 36000, 12000);
        check (std::abs (o3 + 80.0f) < 0.6f, "Gate (Ratio inf, Range -40) -40 dB -> " + juce::String (o3, 2) + " dB (Soll -80)");
    }
}

static void testFrequencySelectivity()
{
    std::printf ("\n8. De-Esser-Szenario: Band 5-10 kHz, Thr -30, Ratio 8\n");
    Rig r;
    r.band (0, "on", 1); r.band (0, "lo", 5000); r.band (0, "hi", 10000);
    r.band (0, "thr", -30); r.band (0, "ratio", 8); r.band (0, "range", -24); r.band (0, "atk", 1); r.band (0, "rel", 60);
    auto s7 = sine (48000, 7000, -10.0f, 48000);
    auto s1 = sine (48000, 1000, -10.0f, 48000);
    auto o7 = peakDb (r.run (s7), 0, 36000, 12000);
    auto o1 = peakDb (r.run (s1), 0, 36000, 12000);
    check (o7 < -25.0f, "7 kHz bei -10 dB wird reduziert auf " + juce::String (o7, 2) + " dB");
    check (std::abs (o1 + 10.0f) < 0.2f, "1 kHz bei -10 dB bleibt bei " + juce::String (o1, 2) + " dB");

    std::printf ("\n9. Freier Sidechain-Bereich: Band 200-500 Hz, Trigger 5-10 kHz\n");
    r.band (0, "lo", 200); r.band (0, "hi", 500); r.band (0, "scfree", 1); r.band (0, "sclo", 5000); r.band (0, "schi", 10000);
    r.band (0, "thr", -30); r.band (0, "ratio", 100); r.band (0, "range", -12);
    juce::AudioBuffer<float> mixd (2, 48000);
    auto a = sine (48000, 300, -12.0f, 48000);
    auto b = sine (48000, 7000, -10.0f, 48000);
    for (int c = 0; c < 2; ++c) { mixd.copyFrom (c, 0, a, c, 0, 48000); mixd.addFrom (c, 0, b, c, 0, 48000); }
    auto out = r.run (mixd);
    // isolate 300 Hz by measuring with 7 kHz removed: process 300 Hz alone too
    auto alone = r.run (a);
    const float withTrig = rmsDb (out, 0, 36000, 12000), noTrig = rmsDb (alone, 0, 36000, 12000);
    check (noTrig > -15.5f, "300 Hz allein unveraendert (" + juce::String (noTrig, 2) + " dB RMS)");
    // 300 Hz component in out: subtract 7 kHz (unaffected) -> compare total RMS with expectation
    juce::AudioBuffer<float> diff (2, 48000);
    diff.copyFrom (0, 0, out, 0, 0, 48000);
    diff.addFrom (0, 0, b, 0, 0, 48000, -1.0f);
    const float lowPart = rmsDb (diff, 0, 36000, 12000);
    check (lowPart < -26.0f, "300 Hz mit 7-kHz-Trigger um ~12 dB reduziert (" + juce::String (lowPart, 2) + " dB RMS, vorher "
                                 + juce::String (noTrig, 2) + ")");
    juce::ignoreUnused (withTrig);
}

static void testStereo()
{
    std::printf ("\n10. Mid/Side und Stereo-Link\n");
    Rig r;
    r.band (0, "on", 1); r.band (0, "lo", 10); r.band (0, "hi", 30000);
    r.band (0, "thr", -30); r.band (0, "ratio", 100); r.band (0, "range", -12); r.band (0, "knee", 0);
    r.band (0, "stereo", 1); // mid
    const int n = 48000;
    juce::AudioBuffer<float> side (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float v = 0.3f * (float) std::sin (2 * juce::MathConstants<double>::pi * 1000 * i / 48000.0);
        side.setSample (0, i, v); side.setSample (1, i, -v);
    }
    auto o = r.run (side);
    check (std::abs (peakDb (o, 0, 36000, 12000) - juce::Decibels::gainToDecibels (0.3f)) < 0.05f,
           "Mid-Modus: reines Seitensignal bleibt unveraendert");
    r.band (0, "stereo", 2); // side
    o = r.run (side);
    check (std::abs (peakDb (o, 0, 36000, 12000) - (juce::Decibels::gainToDecibels (0.3f) - 12.0f)) < 0.3f,
           "Side-Modus: Seitensignal um Range (-12 dB) reduziert: " + juce::String (peakDb (o, 0, 36000, 12000), 2) + " dB");

    Rig r2;
    r2.band (0, "on", 1); r2.band (0, "lo", 10); r2.band (0, "hi", 30000);
    r2.band (0, "thr", -30); r2.band (0, "ratio", 100); r2.band (0, "range", -12); r2.band (0, "knee", 0);
    r2.band (0, "stereo", 0); r2.band (0, "link", 0);
    juce::AudioBuffer<float> lr (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float v = (float) std::sin (2 * juce::MathConstants<double>::pi * 1000 * i / 48000.0);
        lr.setSample (0, i, 0.3f * v); lr.setSample (1, i, 0.003f * v);
    }
    o = r2.run (lr);
    check (std::abs (peakDb (o, 1, 36000, 12000) - juce::Decibels::gainToDecibels (0.003f)) < 0.1f,
           "Link 0 %: leiser rechter Kanal bleibt unberuehrt (" + juce::String (peakDb (o, 1, 36000, 12000), 2) + " dB)");
    r2.band (0, "link", 100);
    o = r2.run (lr);
    check (peakDb (o, 1, 36000, 12000) < juce::Decibels::gainToDecibels (0.003f) - 11.0f,
           "Link 100 %: rechter Kanal folgt dem linken (" + juce::String (peakDb (o, 1, 36000, 12000), 2) + " dB)");
}

static void testSidechainAndLookahead()
{
    std::printf ("\n11. Externer Sidechain\n");
    {
        Rig r;
        r.band (0, "on", 1); r.band (0, "lo", 10); r.band (0, "hi", 30000); r.band (0, "scext", 1);
        r.band (0, "thr", -30); r.band (0, "ratio", 100); r.band (0, "range", -10); r.band (0, "knee", 0);
        auto main = sine (48000, 1000, -40.0f, 48000);
        auto sc = sine (48000, 1000, -10.0f, 48000);
        auto o = r.run (main, &sc);
        check (std::abs (peakDb (o, 0, 36000, 12000) + 50.0f) < 0.3f,
               "Leises Signal wird vom lauten Sidechain um 10 dB gedrueckt: " + juce::String (peakDb (o, 0, 36000, 12000), 2));
    }
    std::printf ("\n12. Lookahead (20 ms Latenz, Band-Lookahead 5 ms)\n");
    {
        Rig r;
        r.set (id::lookahead, 1);
        r.band (0, "on", 1); r.band (0, "lo", 10); r.band (0, "hi", 30000);
        r.band (0, "thr", -20); r.band (0, "ratio", 100); r.band (0, "range", -20); r.band (0, "knee", 0);
        r.band (0, "atk", 1); r.band (0, "rel", 100); r.band (0, "look", 5);
        r.reprepare();
        const int lat = r.p->getLatencySamples();
        check (lat == 960, "Gemeldete Latenz " + juce::String (lat) + " Samples (20 ms bei 48 kHz)");
        const int n = 48000;
        juce::AudioBuffer<float> in (2, n);
        in.clear();
        for (int i = 24000; i < n; ++i)
        {
            const float v = (float) std::sin (2 * juce::MathConstants<double>::pi * 1000 * i / 48000.0);
            in.setSample (0, i, v); in.setSample (1, i, v);
        }
        auto o = r.run (in);
        const float firstMs = peakDb (o, 0, 24000 + lat, 48);
        check (firstMs < -8.0f, "Transiente im ersten 1 ms bereits reduziert: " + juce::String (firstMs, 1) + " dB (ohne Lookahead ~0 dB)");
    }
}

static void testCpu()
{
    std::printf ("\n13. Rechenzeit (6 Baender, Stereo, 48 kHz, Puffer 128)\n");
    for (int m = 0; m < 3; ++m)
    {
        Rig r (48000.0, 128);
        r.set (id::phase, (float) m);
        r.set (id::lookahead, 1);
        for (int b = 0; b < kNumBands; ++b)
        {
            r.band (b, "on", 1); r.band (b, "thr", -40); r.band (b, "ratio", 4); r.band (b, "slope", 7);
        }
        r.reprepare();
        auto in = noise (48000 * 10, -12.0f);
        const auto t0 = std::chrono::steady_clock::now();
        auto o = r.run (in);
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  %-8s %.1f %% eines Kerns (10 s Audio in %.3f s)\n", modeName (m), sec / 10.0 * 100.0, sec);
        bool finite = true;
        for (int i = 0; i < o.getNumSamples(); ++i) finite &= std::isfinite (o.getSample (0, i));
        check (finite, juce::String (modeName (m)) + ": keine NaN/Inf");
    }
}

static void testRobustness()
{
    std::printf ("\n14. Zustand speichern/laden\n");
    {
        Rig a;
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        for (auto* prm : a.p->getParameters()) prm->setValueNotifyingHost (u (rng));
        juce::MemoryBlock mb;
        a.p->getStateInformation (mb);
        Rig b;
        b.p->setStateInformation (mb.getData(), (int) mb.getSize());
        int diff = 0;
        auto pa = a.p->getParameters(), pb = b.p->getParameters();
        for (int i = 0; i < pa.size(); ++i)
            if (pa[i]->getCurrentValueAsText() != pb[i]->getCurrentValueAsText())
            {
                if (++diff < 6) std::printf ("    %s: %f vs %f\n", pa[i]->getName (40).toRawUTF8(), pa[i]->getValue(), pb[i]->getValue());
            }
        check (diff == 0 && pa.size() > 100, juce::String (pa.size()) + " Parameter, " + juce::String (diff) + " Abweichungen nach Laden");
    }

    std::printf ("\n15. Stresstest: zufaellige Automation, Blockgroessen 1-2048, 44.1/96 kHz, Moduswechsel, Mono\n");
    for (double sr : { 44100.0, 96000.0 })
        for (bool st : { true, false })
        {
            Rig r (sr, 2048, st);
            std::mt19937 rng ((unsigned) sr + st);
            std::uniform_real_distribution<float> u (0.0f, 1.0f);
            std::uniform_int_distribution<int> blk (1, 2048);
            auto params = r.p->getParameters();
            const int total = (int) (sr * 12);
            auto in = noise (total, -6.0f, 11);
            auto sc = noise (total, -10.0f, 12);
            const int chMain = r.p->getMainBusNumInputChannels();
            juce::AudioBuffer<float> buf (chMain + 2, 2048);
            juce::MidiBuffer midi;
            bool finite = true;
            float peak = 0;
            for (int pos = 0; pos < total;)
            {
                const int n = std::min (blk (rng), total - pos);
                if (u (rng) < 0.3f)
                    for (int k = 0; k < 4; ++k) params[(int) (u (rng) * (params.size() - 1))]->setValueNotifyingHost (u (rng));
                for (int b = 0; b < kNumBands; ++b) if (u (rng) < 0.02f) r.band (b, "on", 1);
                buf.setSize (chMain + 2, n, false, false, true);
                for (int c = 0; c < chMain; ++c) buf.copyFrom (c, 0, in, c, pos, n);
                for (int c = 0; c < 2; ++c) buf.copyFrom (chMain + c, 0, sc, c, pos, n);
                r.p->processBlock (buf, midi);
                for (int c = 0; c < chMain; ++c)
                    for (int i = 0; i < n; ++i)
                    {
                        const float v = buf.getSample (c, i);
                        finite &= std::isfinite (v);
                        peak = std::max (peak, std::abs (v));
                    }
                pos += n;
            }
            // afterwards: neutral settings must give back the input (no lingering instability)
            for (int b = 0; b < kNumBands; ++b) { r.band (b, "gain", 0); r.band (b, "range", 0); r.band (b, "solo", 0); }
            r.set (id::output, 0); r.set (id::mix, 100);
            juce::AudioBuffer<float> rest (2, (int) sr);
            for (int c = 0; c < 2; ++c) rest.copyFrom (c, 0, in, c, 0, (int) sr);
            auto o = r.run (rest);
            const float lvlIn = rmsDb (rest, 0, (int) sr / 2, (int) sr / 2), lvlOut = rmsDb (o, 0, (int) sr / 2, (int) sr / 2);
            check (finite && std::abs (lvlIn - lvlOut) < 0.5f,
                   juce::String (sr / 1000.0, 1) + " kHz " + (st ? "Stereo" : "Mono") + ": endlich (Spitze bei Extremwerten "
                       + juce::String (juce::Decibels::gainToDecibels (peak), 1) + " dBFS), danach neutral: "
                       + juce::String (lvlOut - lvlIn, 2) + " dB");
        }

    std::printf ("\n17. Schnelle Regelung ohne Ueberschwinger (Release 5 ms, Ratio inf, 6 Baender)\n");
    for (auto [rng_, atk_] : { std::pair<float, float> { -60.0f, 0.05f }, { -18.0f, 1.0f } })
    for (int m = 0; m < 3; ++m)
    {
        Rig r;
        r.set (id::phase, (float) m);
        const float e[] = { 10, 120, 400, 1500, 4000, 9000, 30000 };
        for (int b = 0; b < kNumBands; ++b)
        {
            r.band (b, "on", 1); r.band (b, "lo", e[b]); r.band (b, "hi", e[b + 1]); r.band (b, "slope", 7);
            r.band (b, "thr", -40); r.band (b, "ratio", 100); r.band (b, "range", rng_); r.band (b, "knee", 0);
            r.band (b, "atk", atk_); r.band (b, "rel", 5);
        }
        r.reprepare();
        const int n = 48000 * 4;
        auto in = noise (n, -10.0f, 21);
        for (int i = 0; i < n; ++i) // bursts
        {
            const float env = ((i / 2400) % 3 == 0) ? 1.0f : 0.02f;
            in.setSample (0, i, in.getSample (0, i) * env); in.setSample (1, i, in.getSample (1, i) * env);
        }
        auto o = r.run (in);
        const float pin = peakDb (in, 0, 0, n), pout = peakDb (o, 0, 0, n);
        check (pout < pin + 3.0f, juce::String (modeName (m)) + " Range " + juce::String (rng_, 0) + " Attack " + juce::String (atk_, 2) + ": Ausgangsspitze " + juce::String (pout, 1) + " dBFS, Eingang "
                                      + juce::String (pin, 1) + " dBFS");
    }

    std::printf ("\n16. Editor oeffnen/schliessen waehrend der Verarbeitung\n");
    {
        Rig r;
        for (int k = 0; k < 20; ++k)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (r.p->createEditor());
            r.p->selectedBand = k % 7 - 1;
            r.run (noise (4096, -12.0f, (unsigned) k));
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        }
        check (true, "20 Zyklen ohne Absturz");
    }
}

static void snapshot (const juce::File& file)
{
    Rig r;
    r.set (id::phase, 0);
    const float bands[][2] = { { 10, 180 }, { 180, 600 }, { 2800, 4500 }, { 5500, 11000 } };
    for (int b = 0; b < 4; ++b)
    {
        r.band (b, "on", 1); r.band (b, "lo", bands[b][0]); r.band (b, "hi", bands[b][1]);
        r.band (b, "sclo", bands[b][0]); r.band (b, "schi", bands[b][1]);
        r.band (b, "thr", -38.0f + 4 * b); r.band (b, "ratio", 4);
    }
    r.band (1, "range", 6); r.band (1, "thr", -50);
    r.band (2, "mode", 0);
    r.p->selectedBand = 3;
    std::unique_ptr<juce::AudioProcessorEditor> ed (r.p->createEditor());
    ed->setVisible (true);
    // pink-ish test signal so the analyzer shows something
    juce::AudioBuffer<float> sig (2, 48000);
    {
        std::mt19937 rng (3);
        std::normal_distribution<float> d (0, 0.1f);
        float b0 = 0, b1 = 0, b2 = 0;
        for (int i = 0; i < 48000; ++i)
        {
            const float w = d (rng);
            b0 = 0.99765f * b0 + w * 0.0990460f; b1 = 0.96300f * b1 + w * 0.2965164f; b2 = 0.57000f * b2 + w * 1.0526913f;
            const float v = (b0 + b1 + b2 + w * 0.1848f) * 0.6f
                            + 0.25f * (float) std::sin (2 * juce::MathConstants<double>::pi * 220 * i / 48000.0)
                            + 0.08f * (float) std::sin (2 * juce::MathConstants<double>::pi * 7000 * i / 48000.0);
            sig.setSample (0, i, v); sig.setSample (1, i, v);
        }
    }
    for (int k = 0; k < 12; ++k)
    {
        juce::AudioBuffer<float> chunk (2, 4096);
        for (int c = 0; c < 2; ++c) chunk.copyFrom (c, 0, sig, c, (k * 4096) % 40000, 4096);
        r.run (chunk);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
    }
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    juce::FileOutputStream os (file);
    os.setPosition (0);
    os.truncate();
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("\nScreenshot: %s\n", file.getFullPathName().toRawUTF8());
}

static void burstProbe()
{
    // single band, compare how fast the gain change reaches the audio per mode
    for (auto [lo, hi] : { std::pair<float, float> { 1500, 4000 }, { 10, 120 }, { 10, 30000 } })
        for (int m = 0; m < 3; ++m)
        {
            Rig r;
            r.set (id::phase, (float) m);
            r.band (0, "on", 1); r.band (0, "lo", lo); r.band (0, "hi", hi); r.band (0, "slope", 3);
            r.band (0, "thr", -40); r.band (0, "ratio", 100); r.band (0, "range", -18); r.band (0, "knee", 0);
            r.band (0, "atk", 1); r.band (0, "rel", 5);
            r.reprepare();
            const int n = 48000 * 2;
            juce::AudioBuffer<float> in (2, n);
            const double f = std::sqrt ((double) std::max (lo, 20.0f) * std::min (hi, 20000.0f));
            for (int i = 0; i < n; ++i)
            {
                const float env = ((i / 2400) % 3 == 0) ? 1.0f : 0.01f;
                const float v = env * 0.5f * (float) std::sin (2 * juce::MathConstants<double>::pi * f * i / 48000.0);
                in.setSample (0, i, v); in.setSample (1, i, v);
            }
            auto o = r.run (in);
            const int lat = r.p->getLatencySamples();
            // average gain over the first 3 ms of each burst, and steady at the end of the burst
            double a3 = 0, aEnd = 0; int cnt = 0;
            for (int bi = 3; bi < 12; ++bi)
            {
                const int st = bi * 3 * 2400;
                a3 += rmsDb (o, 0, st + lat, 144) - rmsDb (in, 0, st, 144);
                aEnd += rmsDb (o, 0, st + lat + 2000, 300) - rmsDb (in, 0, st + 2000, 300);
                ++cnt;
            }
            std::printf ("  %5.0f-%5.0f Hz %-8s erste 3 ms: %6.2f dB   Ende Burst: %6.2f dB   Spitze %6.2f\n", lo, hi, modeName (m),
                         a3 / cnt, aEnd / cnt, peakDb (o, 0, 0, n));
        }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String (argv[1]) == "burst") { burstProbe(); return 0; }
    if (argc > 1 && juce::String (argv[1]) == "ir")
    {
        Engine e; e.prepare (48000, 512, 2);
        BandParams p; p.lo = 500; p.hi = 2000; p.slopeIdx = 3;
        e.requestFirUpdate (0, p);
        auto& ir = e.debugPendingIr (0);
        juce::FileOutputStream os (juce::File::getCurrentWorkingDirectory().getChildFile ("ir.txt"));
        os.setPosition (0); os.truncate();
        for (int i = 0; i < ir.getNumSamples(); ++i) os << juce::String (ir.getSample (0, i), 9) << "\n";
        return 0;
    }
    const juce::String only = argc > 1 ? juce::String (argv[1]) : juce::String();
    if (only.isEmpty() || only == "dsp")
    {
        testTransparency();
        testStaticShape();
        testStaticCurve();
        testTiming();
        testModes();
        testFrequencySelectivity();
        testStereo();
        testSidechainAndLookahead();
        testCpu();
        testRobustness();
        std::printf ("\n%d Fehler\n", failures);
    }
    if (only.isEmpty() || only == "shot")
        snapshot (juce::File::getCurrentWorkingDirectory().getChildFile ("snapshot.png"));
    return failures == 0 ? 0 : 1;
}
