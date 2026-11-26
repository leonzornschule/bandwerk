#pragma once
#include <cmath>
#include <algorithm>

namespace bw::dsp
{
// Quadratic soft knee around zero: 0 below -k/2, identity above +k/2
inline double softOver (double d, double knee) noexcept
{
    if (knee <= 1.0e-6) return std::max (d, 0.0);
    const double h = 0.5 * knee;
    if (d <= -h) return 0.0;
    if (d >= h) return d;
    const double t = d + h;
    return t * t / (2.0 * knee);
}

struct GainComputer
{
    double thr = -24, range = -18, ratio = 3, knee = 12;
    bool expand = false;

    bool upward() const noexcept { return range > 0.0; }

    // Magnitude (>= 0 dB) of the gain change for a detector level in dB.
    // The sign is given by 'range': negative = downward, positive = upward.
    //   Compress downward: reduce what is above the threshold
    //   Compress upward:   raise what is below the threshold
    //   Expand downward:   reduce what is below the threshold (gate at high ratio)
    //   Expand upward:     raise what is above the threshold
    double magnitude (double levelDb) const noexcept
    {
        const double lim = std::abs (range);
        if (lim < 1.0e-6) return 0.0;
        const bool up = upward();
        double d, factor;
        if (! expand)
        {
            d = up ? thr - levelDb : levelDb - thr;
            factor = ratio >= 99.5 ? 1.0 : 1.0 - 1.0 / ratio;
        }
        else
        {
            d = up ? levelDb - thr : thr - levelDb;
            factor = ratio >= 99.5 ? 1.0e3 : ratio - 1.0;
        }
        return std::min (lim, softOver (d, knee) * factor);
    }
};

// Attack/release smoothing of the gain change in the gain domain, with a slow
// second stage that lengthens the release after sustained gain change
// (program-dependent release).
struct Envelope
{
    double fast = 0, slow = 0;
    double aA = 0, aR = 0, aAs = 0, aRs = 0;

    static double coef (double ms, double fs) { return std::exp (-1.0 / (std::max (ms, 0.01) * 0.001 * fs)); }

    void set (double attackMs, double releaseMs, double fs)
    {
        aA = coef (attackMs, fs);
        aR = coef (releaseMs, fs);
        aAs = coef (attackMs * 8.0 + 40.0, fs);
        aRs = coef (releaseMs * 3.0, fs);
    }
    void reset() { fast = slow = 0; }

    inline double process (double target) noexcept
    {
        fast = target + (fast - target) * (target > fast ? aA : aR);
        slow = target + (slow - target) * (target > slow ? aAs : aRs);
        return std::max (fast, 0.5 * slow);
    }
};
} // namespace bw::dsp
