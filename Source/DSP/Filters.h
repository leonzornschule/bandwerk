#pragma once
#include <cmath>
#include <algorithm>

// All filters use the topology-preserving (trapezoidal) state variable form by
// Andrew Simper, which stays stable and click-free when coefficients change per
// sample block. State is kept in double precision.
namespace bw::dsp
{
constexpr double kPi = 3.14159265358979323846;

inline double prewarp (double hz, double fs)
{
    hz = std::clamp (hz, 1.0, 0.49 * fs);
    return std::tan (kPi * hz / fs);
}

struct Svf
{
    double ic1 = 0, ic2 = 0;
    double k = 1.4142, a1 = 0, a2 = 0, a3 = 0;

    void set (double g, double kIn) noexcept
    {
        k = kIn;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() noexcept { ic1 = ic2 = 0; }

    // v0 in, returns lp/bp/hp. bp is the un-normalised band output s/(s^2+ks+1)
    inline void tick (double v0, double& lp, double& bp, double& hp) noexcept
    {
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = v0 - k * v1 - v2;
    }
};

struct OnePole
{
    double s = 0, g1 = 0;
    void set (double g) noexcept { g1 = g / (1.0 + g); }
    void reset() noexcept { s = 0; }
    inline void tick (double x, double& lp, double& hp) noexcept
    {
        const double v = (x - s) * g1;
        lp = v + s;
        s = lp + v;
        hp = x - lp;
    }
};

inline double butterK (int order, int section) // damping of 2nd-order section m (0-based)
{
    const double theta = (2.0 * section + 1.0) * kPi / (2.0 * order);
    return 2.0 * std::sin (theta);
}

// Butterworth high shelf of order 1..8. Response B(a s)/B(s) with a = G^(1/N):
// |H|^2 = (1 + G^2 w^2N) / (1 + w^2N). The corner is placed so that the
// half-gain point (in dB) lands exactly on 'edgeHz' for any gain. At G = 1
// the filter is an exact identity, so there is no static phase shift.
struct HighShelf
{
    static constexpr int kMaxSections = 4;
    Svf sec[kMaxSections];
    OnePole first;
    double chp[kMaxSections] {}, cbp[kMaxSections] {}, clp[kMaxSections] { 1, 1, 1, 1 };
    double c1 = 1.0, c1lp = 1.0;
    int order = 4, nSec = 2;
    bool odd = false, lowShelf = false;

    void reset() noexcept
    {
        for (auto& s : sec) s.reset();
        first.reset();
    }

    // Low shelf variant: gain G below the edge, unity above. The gain enters only the
    // output mix of the low-pass path, so the filter states never see a G-scaled input.
    void setupLow (double edgeHz, double gainLin, double fs) noexcept
    {
        lowShelf = true;
        gainLin = std::clamp (gainLin, 1.0e-4, 1.0e4);
        const double fc = edgeHz * std::pow (gainLin, -1.0 / (2.0 * order));
        const double g = prewarp (fc, fs);
        const double a = std::pow (gainLin, 1.0 / order);
        for (int m = 0; m < nSec; ++m)
        {
            const double k = butterK (order, m);
            sec[m].set (g, k);
            chp[m] = 1.0;
            cbp[m] = k * a;
            clp[m] = a * a;
        }
        if (odd)
        {
            first.set (g);
            c1 = 1.0;
            c1lp = a;
        }
    }

    void setOrder (int n) noexcept
    {
        n = std::clamp (n, 1, 8);
        if (n != order) reset();
        order = n;
        nSec = n / 2;
        odd = (n & 1) != 0;
    }

    void setup (double edgeHz, double gainLin, double fs) noexcept
    {
        lowShelf = false;
        gainLin = std::clamp (gainLin, 1.0e-4, 1.0e4);
        const double fc = edgeHz * std::pow (gainLin, 1.0 / (2.0 * order));
        const double g = prewarp (fc, fs);
        const double a = std::pow (gainLin, 1.0 / order);
        for (int m = 0; m < nSec; ++m)
        {
            const double k = butterK (order, m);
            sec[m].set (g, k);
            chp[m] = a * a;
            cbp[m] = k * a;
            clp[m] = 1.0;
        }
        if (odd)
        {
            first.set (g);
            c1 = a;
            c1lp = 1.0;
        }
    }

    inline double process (double x) noexcept
    {
        double lp, bp, hp;
        for (int m = 0; m < nSec; ++m)
        {
            sec[m].tick (x, lp, bp, hp);
            x = chp[m] * hp + cbp[m] * bp + clp[m] * lp;
        }
        if (odd)
        {
            first.tick (x, lp, hp);
            x = c1 * hp + c1lp * lp;
        }
        return x;
    }
};

// Butterworth low- or high-pass of order 1..8 (used for detector band filters)
struct Butterworth
{
    static constexpr int kMaxSections = 4;
    Svf sec[kMaxSections];
    OnePole first;
    int order = 2, nSec = 1;
    bool odd = false, highpass = false, active = true;

    void reset() noexcept
    {
        for (auto& s : sec) s.reset();
        first.reset();
    }

    void setup (bool isHighpass, int n, double hz, double fs, bool enabled) noexcept
    {
        n = std::clamp (n, 1, 8);
        if (n != order || isHighpass != highpass) reset();
        highpass = isHighpass;
        order = n;
        nSec = n / 2;
        odd = (n & 1) != 0;
        active = enabled;
        const double g = prewarp (hz, fs);
        for (int m = 0; m < nSec; ++m)
            sec[m].set (g, butterK (order, m));
        if (odd) first.set (g);
    }

    inline double process (double x) noexcept
    {
        if (! active) return x;
        double lp, bp, hp;
        for (int m = 0; m < nSec; ++m)
        {
            sec[m].tick (x, lp, bp, hp);
            x = highpass ? hp : lp;
        }
        if (odd)
        {
            first.tick (x, lp, hp);
            x = highpass ? hp : lp;
        }
        return x;
    }
};

// Linkwitz-Riley crossover (LR4 or LR8) with matching all-pass for phase compensation
struct LinkwitzRiley
{
    Svf lpSec[4], hpSec[4];
    int nHalf = 1; // sections per Butterworth half (1 = LR4, 2 = LR8)
    double ks[2] { 1.4142135623730951, 0 };

    void reset() noexcept
    {
        for (auto& s : lpSec) s.reset();
        for (auto& s : hpSec) s.reset();
    }

    void setup (int lrOrder, double hz, double fs) noexcept
    {
        const int nh = lrOrder >= 8 ? 2 : 1;
        if (nh != nHalf) reset();
        nHalf = nh;
        const int bwOrder = 2 * nHalf;
        const double g = prewarp (hz, fs);
        for (int m = 0; m < nHalf; ++m)
            ks[m] = butterK (bwOrder, m);
        for (int i = 0; i < 2 * nHalf; ++i)
        {
            lpSec[i].set (g, ks[i % nHalf]);
            hpSec[i].set (g, ks[i % nHalf]);
        }
    }

    inline void process (double x, double& low, double& high) noexcept
    {
        double lp, bp, hp;
        double l = x, h = x;
        for (int i = 0; i < 2 * nHalf; ++i)
        {
            lpSec[i].tick (l, lp, bp, hp);
            l = lp;
            hpSec[i].tick (h, lp, bp, hp);
            h = hp;
        }
        low = l;
        high = h;
    }
};

struct LRAllpass
{
    Svf sec[2];
    int nHalf = 1;

    void reset() noexcept { sec[0].reset(); sec[1].reset(); }

    void setup (int lrOrder, double hz, double fs) noexcept
    {
        const int nh = lrOrder >= 8 ? 2 : 1;
        if (nh != nHalf) reset();
        nHalf = nh;
        const double g = prewarp (hz, fs);
        for (int m = 0; m < nHalf; ++m)
            sec[m].set (g, butterK (2 * nHalf, m));
    }

    inline double process (double x) noexcept
    {
        double lp, bp, hp;
        for (int m = 0; m < nHalf; ++m)
        {
            sec[m].tick (x, lp, bp, hp);
            x = x - 2.0 * sec[m].k * bp;
        }
        return x;
    }
};
} // namespace bw::dsp
