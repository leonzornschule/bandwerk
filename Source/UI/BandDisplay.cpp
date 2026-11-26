#include "BandDisplay.h"
#include "Look.h"

namespace bw::ui
{
static constexpr float kLogSpan = 8.0063801f; // ln (30000 / 10)
static constexpr float kGainRange = 30.0f;
static constexpr float kSpecRange = 100.0f;

BandDisplay::BandDisplay (BandwerkProcessor& p) : proc (p)
{
    preRing.assign (kFftSize, 0.0f);
    postRing.assign (kFftSize, 0.0f);
    preDb.assign (kFftSize / 2 + 1, -120.0f);
    postDb.assign (kFftSize / 2 + 1, -120.0f);
    fftBuf.assign (kFftSize * 2, 0.0f);
    setOpaque (true);
    startTimerHz (30);
}

BandDisplay::~BandDisplay() { stopTimer(); }

void BandDisplay::resized()
{
    plot = getLocalBounds().toFloat().withTrimmedRight (36.0f).withTrimmedBottom (20.0f).withTrimmedTop (6.0f);
}

float BandDisplay::freqToX (float hz) const { return plot.getX() + std::log (std::max (hz, 1.0f) / kMinFreq) / kLogSpan * plot.getWidth(); }
float BandDisplay::xToFreq (float x) const { return kMinFreq * std::exp ((x - plot.getX()) / plot.getWidth() * kLogSpan); }
float BandDisplay::gainToY (float db) const { return plot.getCentreY() - db / kGainRange * plot.getHeight() * 0.5f; }
float BandDisplay::specToY (float db) const { return plot.getY() + (-db / kSpecRange) * plot.getHeight(); }
float BandDisplay::yToSpec (float y) const { return -(y - plot.getY()) / plot.getHeight() * kSpecRange; }

//==============================================================================
float BandDisplay::bandResponseDb (const BandParams& p, float db, float hz, double fs)
{
    if (std::abs (db) < 1.0e-4f) return 0.0f;
    const int n = p.shelfOrder();
    const double G = std::pow (10.0, db / 20.0);
    const bool useLo = p.lo > 10.5f;
    const bool useHi = p.hi < 0.45 * fs && p.hi < 29000.0f;
    auto shelf = [n] (double f, double edge, double gain) {
        const double fc = edge * std::pow (gain, 1.0 / (2.0 * n));
        const double x2n = std::pow (f / fc, 2.0 * n);
        return 10.0 * std::log10 ((1.0 + gain * gain * x2n) / (1.0 + x2n));
    };
    double r = useLo ? shelf (hz, p.lo, G) : (double) db;
    if (useHi) r += shelf (hz, p.hi, 1.0 / G);
    return (float) r;
}

//==============================================================================
void BandDisplay::pullAnalyzer (juce::AbstractFifo& f, const std::vector<float>& data, std::vector<float>& ring, int& pos)
{
    const int num = f.getNumReady();
    if (num <= 0) return;
    int s1, n1, s2, n2;
    f.prepareToRead (num, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) { ring[(size_t) pos] = data[(size_t) (s1 + i)]; pos = (pos + 1) % kFftSize; }
    for (int i = 0; i < n2; ++i) { ring[(size_t) pos] = data[(size_t) (s2 + i)]; pos = (pos + 1) % kFftSize; }
    f.finishedRead (n1 + n2);
}

void BandDisplay::computeSpectrum (const std::vector<float>& ring, int pos, std::vector<float>& outDb)
{
    for (int i = 0; i < kFftSize; ++i) fftBuf[(size_t) i] = ring[(size_t) ((pos + i) % kFftSize)];
    std::fill (fftBuf.begin() + kFftSize, fftBuf.end(), 0.0f);
    window.multiplyWithWindowingTable (fftBuf.data(), (size_t) kFftSize);
    fft.performFrequencyOnlyForwardTransform (fftBuf.data(), true);
    const float scale = 4.0f / (float) kFftSize;
    for (int k = 0; k <= kFftSize / 2; ++k)
    {
        const float db = juce::Decibels::gainToDecibels (fftBuf[(size_t) k] * scale, -140.0f);
        auto& s = outDb[(size_t) k];
        s += (db - s) * (db > s ? 0.6f : 0.12f);
    }
}

void BandDisplay::timerCallback()
{
    pullAnalyzer (proc.engine.preFifo, proc.engine.preData, preRing, prePos);
    pullAnalyzer (proc.engine.postFifo, proc.engine.postData, postRing, postPos);
    computeSpectrum (preRing, prePos, preDb);
    computeSpectrum (postRing, postPos, postDb);
    for (int b = 0; b < kNumBands; ++b)
    {
        const float m = proc.engine.meterGainDb[(size_t) b].load();
        shownGain[(size_t) b] += (m - shownGain[(size_t) b]) * 0.5f;
    }
    repaint();
}

//==============================================================================
void BandDisplay::paint (juce::Graphics& g)
{
    g.fillAll (col::display);
    const double fs = proc.engine.getSampleRate() > 0 ? proc.engine.getSampleRate() : 48000.0;
    const float mix = proc.readGlobal().mix;
    const int sel = proc.selectedBand.load();

    // grid
    g.setFont (juce::FontOptions (11.0f));
    const float freqs[] = { 20, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900,
                            1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 20000 };
    for (auto f : freqs)
    {
        const bool major = f == 20 || f == 50 || f == 100 || f == 200 || f == 500 || f == 1000 || f == 2000
                           || f == 5000 || f == 10000 || f == 20000;
        const float x = freqToX (f);
        g.setColour (major ? col::gridHi.withAlpha (0.6f) : col::grid.withAlpha (0.5f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        if (major)
        {
            g.setColour (col::textDim);
            const auto t = f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f);
            g.drawText (t, juce::Rectangle<float> (x - 20, plot.getBottom() + 3, 40, 14), juce::Justification::centred);
        }
    }
    for (int db = -24; db <= 24; db += 6)
    {
        const float y = gainToY ((float) db);
        g.setColour (db == 0 ? col::gridHi : col::grid.withAlpha (0.45f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        if (db % 12 == 0)
        {
            g.setColour (col::textDim);
            g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (plot.getRight() + 2, y - 7, 32, 14),
                        juce::Justification::centredLeft);
        }
    }

    // analyzer
    auto specPath = [&] (const std::vector<float>& dbs, bool closed) {
        juce::Path p;
        bool first = true;
        for (float x = plot.getX(); x <= plot.getRight(); x += 2.0f)
        {
            const float f0 = xToFreq (x), f1 = xToFreq (x + 2.0f);
            const float b0 = f0 * kFftSize / (float) fs, b1 = f1 * kFftSize / (float) fs;
            float v;
            if (b1 - b0 < 1.0f)
            {
                const int i = juce::jlimit (0, kFftSize / 2 - 1, (int) b0);
                const float fr = juce::jlimit (0.0f, 1.0f, b0 - (float) i);
                v = dbs[(size_t) i] * (1.0f - fr) + dbs[(size_t) i + 1] * fr;
            }
            else
            {
                v = -140.0f;
                for (int i = (int) b0; i <= std::min ((int) b1, kFftSize / 2); ++i) v = std::max (v, dbs[(size_t) i]);
            }
            const float y = juce::jlimit (plot.getY(), plot.getBottom(), specToY (v));
            if (first) { p.startNewSubPath (x, closed ? plot.getBottom() : y); if (closed) p.lineTo (x, y); first = false; }
            else p.lineTo (x, y);
        }
        if (closed) { p.lineTo (plot.getRight(), plot.getBottom()); p.closeSubPath(); }
        return p;
    };
    g.setColour (juce::Colour (0xff3d4a5c).withAlpha (0.55f));
    g.fillPath (specPath (postDb, true));
    g.setColour (juce::Colour (0xff8a96a8).withAlpha (0.35f));
    g.strokePath (specPath (preDb, false), juce::PathStrokeType (1.0f));

    // bands
    const int cols = (int) (plot.getWidth() / 2.0f) + 1;
    std::vector<float> totalDb ((size_t) cols, 0.0f);
    bool anyOn = false;

    auto drawBand = [&] (int b) {
        const auto p = proc.readBand (b);
        if (! p.on) return;
        anyOn = true;
        const auto c = col::band (b);
        const bool isSel = b == sel;
        const float xl = freqToX (p.lo <= 10.5f ? kMinFreq : p.lo);
        const float xh = freqToX (p.hi >= 29000.0f ? kMaxFreq : p.hi);
        const float alpha = p.bypass ? 0.4f : 1.0f;

        g.setColour (c.withAlpha ((isSel ? 0.10f : 0.045f) * alpha));
        g.fillRect (juce::Rectangle<float> (xl, plot.getY(), xh - xl, plot.getHeight()));

        juce::Path rangeP, liveP;
        const float rangeDb = mix * (p.range + p.gainDb);
        const float liveDb = shownGain[(size_t) b];
        const float y0 = gainToY (0.0f);
        rangeP.startNewSubPath (plot.getX(), y0);
        liveP.startNewSubPath (plot.getX(), y0);
        for (int i = 0; i < cols; ++i)
        {
            const float x = plot.getX() + 2.0f * (float) i;
            const float f = xToFreq (x);
            rangeP.lineTo (x, gainToY (juce::jlimit (-kGainRange, kGainRange, bandResponseDb (p, rangeDb, f, fs))));
            const float lv = bandResponseDb (p, liveDb, f, fs);
            liveP.lineTo (x, gainToY (juce::jlimit (-kGainRange, kGainRange, lv)));
            if (! p.bypass) totalDb[(size_t) i] += lv;
        }
        rangeP.lineTo (plot.getRight(), y0);
        juce::Path liveFill (liveP);
        liveFill.lineTo (plot.getRight(), y0);
        liveFill.closeSubPath();
        rangeP.closeSubPath();

        g.setColour (c.withAlpha ((isSel ? 0.16f : 0.08f) * alpha));
        g.fillPath (rangeP);
        g.setColour (c.withAlpha (0.35f * alpha));
        g.fillPath (liveFill);
        g.setColour (c.withAlpha (0.95f * alpha));
        g.strokePath (liveP, juce::PathStrokeType (isSel ? 2.0f : 1.5f));

        // edges
        g.setColour (c.withAlpha (isSel ? 0.7f : 0.3f));
        if (p.lo > 10.5f) g.drawVerticalLine ((int) xl, plot.getY(), plot.getBottom());
        if (p.hi < 29000.0f) g.drawVerticalLine ((int) xh, plot.getY(), plot.getBottom());

        // threshold
        const float ty = specToY (p.thr);
        g.setColour (c.withAlpha (isSel ? 0.9f : 0.55f));
        const float dashes[] = { 4.0f, 3.0f };
        g.drawDashedLine (juce::Line<float> (xl + 2, ty, xh - 2, ty), dashes, 2, isSel ? 1.5f : 1.0f);
        const float hx = 0.5f * (xl + xh);
        g.fillEllipse (hx - 5, ty - 5, 10, 10);
        g.setColour (col::display);
        g.drawEllipse (hx - 5, ty - 5, 10, 10, 1.5f);

        if (p.scFree)
        {
            const float sl = freqToX (p.scLo), sh = freqToX (p.scHi);
            g.setColour (c.withAlpha (0.5f));
            g.drawDashedLine (juce::Line<float> (sl, plot.getBottom() - 4, sh, plot.getBottom() - 4), dashes, 2, 3.0f);
        }

        // tag
        auto tag = juce::Rectangle<float> (hx - 26, plot.getY() + 4, 52, 18);
        g.setColour (c.withAlpha (isSel ? 0.95f : 0.6f));
        g.fillRoundedRectangle (tag, 3.0f);
        g.setColour (juce::Colour (0xff101010));
        g.setFont (juce::FontOptions (11.5f).withStyle ("Bold"));
        const float gd = proc.engine.meterDynDb[(size_t) b].load();
        g.drawText (juce::String (b + 1) + "  " + (p.bypass ? juce::String ("byp") : juce::String (gd, 1)), tag,
                    juce::Justification::centred);
        if (p.solo)
        {
            g.setColour (juce::Colours::yellow.withAlpha (0.9f));
            g.fillEllipse (tag.getRight() - 4, tag.getY() - 3, 7, 7);
        }
    };

    for (int b = 0; b < kNumBands; ++b)
        if (b != sel) drawBand (b);
    if (sel >= 0) drawBand (sel);

    if (anyOn)
    {
        juce::Path tp;
        for (int i = 0; i < cols; ++i)
        {
            const float x = plot.getX() + 2.0f * (float) i;
            const float y = gainToY (juce::jlimit (-kGainRange, kGainRange, totalDb[(size_t) i]));
            if (i == 0) tp.startNewSubPath (x, y); else tp.lineTo (x, y);
        }
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.strokePath (tp, juce::PathStrokeType (1.2f));
    }
    else
    {
        g.setColour (col::textDim);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("Doppelklick ins Spektrum erzeugt ein Band", plot, juce::Justification::centred);
    }

    // hover readout
    if (hover.band < 0 && isMouseOver())
    {
        const auto mp = getMouseXYRelative().toFloat();
        if (plot.contains (mp))
        {
            g.setColour (col::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (freqText (xToFreq (mp.x)), juce::Rectangle<float> (mp.x + 8, mp.y - 18, 70, 14),
                        juce::Justification::centredLeft);
        }
    }
}

//==============================================================================
juce::RangedAudioParameter* BandDisplay::param (int band, const char* name) const
{
    return proc.apvts.getParameter (bandId (band, name));
}

float BandDisplay::getParam (int band, const char* name) const
{
    return proc.apvts.getRawParameterValue (bandId (band, name))->load();
}

void BandDisplay::setParam (int band, const char* name, float value)
{
    if (auto* p = param (band, name))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void BandDisplay::beginGesture (int band)
{
    endGesture();
    for (auto* n : { "lo", "hi", "thr", "sclo", "schi" })
        if (auto* p = param (band, n))
        {
            p->beginChangeGesture();
            gestureParams.push_back (p);
        }
}

void BandDisplay::endGesture()
{
    for (auto* p : gestureParams) p->endChangeGesture();
    gestureParams.clear();
}

float BandDisplay::snapFreq (int band, float hz) const
{
    const float x = freqToX (hz);
    for (int b = 0; b < kNumBands; ++b)
    {
        if (b == band || getParam (b, "on") < 0.5f) continue;
        for (auto e : { getParam (b, "lo"), getParam (b, "hi") })
            if (e > 10.5f && e < 29000.0f && std::abs (freqToX (e) - x) < 7.0f) return e;
    }
    return hz;
}

BandDisplay::Hit BandDisplay::findBand (juce::Point<float> pt) const
{
    const int sel = proc.selectedBand.load();
    int order[kNumBands];
    int n = 0;
    if (sel >= 0) order[n++] = sel;
    for (int b = 0; b < kNumBands; ++b) if (b != sel) order[n++] = b;

    for (int i = 0; i < n; ++i) // edges first
    {
        const int b = order[i];
        if (getParam (b, "on") < 0.5f) continue;
        const float lo = getParam (b, "lo"), hi = getParam (b, "hi");
        if (lo > 10.5f && std::abs (pt.x - freqToX (lo)) < 5.0f) return { b, Part::LoEdge };
        if (hi < 29000.0f && std::abs (pt.x - freqToX (hi)) < 5.0f) return { b, Part::HiEdge };
    }
    for (int i = 0; i < n; ++i)
    {
        const int b = order[i];
        if (getParam (b, "on") < 0.5f) continue;
        const float lo = getParam (b, "lo"), hi = getParam (b, "hi");
        if (pt.x > freqToX (lo <= 10.5f ? kMinFreq : lo) && pt.x < freqToX (hi >= 29000.0f ? kMaxFreq : hi))
            return { b, Part::Body };
    }
    return {};
}

void BandDisplay::mouseMove (const juce::MouseEvent& e)
{
    hover = findBand (e.position);
    setMouseCursor (hover.part == Part::LoEdge || hover.part == Part::HiEdge ? juce::MouseCursor::LeftRightResizeCursor
                    : hover.part == Part::Body                               ? juce::MouseCursor::UpDownLeftRightResizeCursor
                                                                             : juce::MouseCursor::NormalCursor);
}

void BandDisplay::mouseExit (const juce::MouseEvent&) { hover = {}; }

void BandDisplay::mouseDown (const juce::MouseEvent& e)
{
    drag = findBand (e.position);
    if (drag.band < 0) return;
    if (onSelectBand) onSelectBand (drag.band);
    dragStart = e.position;
    dragStartLo = getParam (drag.band, "lo");
    dragStartHi = getParam (drag.band, "hi");
    dragStartThr = getParam (drag.band, "thr");
    beginGesture (drag.band);
}

void BandDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (drag.band < 0) return;
    const int b = drag.band;
    const bool fine = e.mods.isShiftDown();
    const float dx = (e.position.x - dragStart.x) * (fine ? 0.2f : 1.0f);
    const float dy = (e.position.y - dragStart.y) * (fine ? 0.2f : 1.0f);

    if (drag.part == Part::Body)
    {
        const bool pinLo = dragStartLo <= 10.5f, pinHi = dragStartHi >= 29000.0f;
        float factor = std::exp (dx / plot.getWidth() * kLogSpan);
        float lo = pinLo ? kMinFreq : dragStartLo * factor;
        float hi = pinHi ? kMaxFreq : dragStartHi * factor;
        if (! pinLo && lo < 10.6f) { lo = 10.6f; if (! pinHi) hi = dragStartHi * (lo / dragStartLo); }
        if (! pinHi && hi > 28900.0f) { hi = 28900.0f; if (! pinLo) lo = dragStartLo * (hi / dragStartHi); }
        if (pinLo && ! pinHi) hi = juce::jlimit (12.0f, 28900.0f, dragStartHi * factor);
        if (pinHi && ! pinLo) lo = juce::jlimit (10.6f, 25000.0f, dragStartLo * factor);
        setParam (b, "lo", lo);
        setParam (b, "hi", hi);
        const float thr = juce::jlimit (-80.0f, 0.0f, dragStartThr - dy / plot.getHeight() * kSpecRange);
        setParam (b, "thr", thr);
    }
    else if (drag.part == Part::LoEdge)
    {
        float f = snapFreq (b, xToFreq (e.position.x));
        const float hi = getParam (b, "hi");
        f = std::min (f, hi / 1.06f);
        setParam (b, "lo", f <= 10.6f ? kMinFreq : f);
    }
    else if (drag.part == Part::HiEdge)
    {
        float f = snapFreq (b, xToFreq (e.position.x));
        const float lo = getParam (b, "lo");
        f = std::max (f, lo * 1.06f);
        setParam (b, "hi", f >= 28900.0f ? kMaxFreq : f);
    }
    // keep the sidechain range attached unless it was set free
    if (getParam (b, "scfree") < 0.5f)
    {
        setParam (b, "sclo", getParam (b, "lo"));
        setParam (b, "schi", getParam (b, "hi"));
    }
}

void BandDisplay::mouseUp (const juce::MouseEvent&)
{
    endGesture();
    drag = {};
}

void BandDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = findBand (e.position);
    if (h.band >= 0 && h.part == Part::Body)
    {
        setParam (h.band, "on", 0.0f);
        setParam (h.band, "solo", 0.0f);
        if (onSelectBand) onSelectBand (-1);
        return;
    }
    if (! plot.contains (e.position)) return;
    for (int b = 0; b < kNumBands; ++b)
    {
        if (getParam (b, "on") > 0.5f) continue;
        const float f = xToFreq (e.position.x);
        const float lo = f / 1.4142f, hi = f * 1.4142f;
        auto* pOn = param (b, "on");
        pOn->beginChangeGesture();
        setParam (b, "lo", lo < 12.0f ? kMinFreq : lo);
        setParam (b, "hi", hi > 28000.0f ? kMaxFreq : hi);
        setParam (b, "sclo", lo < 12.0f ? kMinFreq : lo);
        setParam (b, "schi", hi > 28000.0f ? kMaxFreq : hi);
        setParam (b, "thr", juce::jlimit (-80.0f, 0.0f, yToSpec (e.position.y)));
        setParam (b, "on", 1.0f);
        pOn->endChangeGesture();
        if (onSelectBand) onSelectBand (b);
        return;
    }
}

void BandDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto h = findBand (e.position);
    if (h.band < 0) return;
    auto* p = param (h.band, "range");
    const float r = juce::jlimit (-60.0f, 36.0f, getParam (h.band, "range") + w.deltaY * (e.mods.isShiftDown() ? 1.5f : 6.0f));
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (r));
    p->endChangeGesture();
}
} // namespace bw::ui
