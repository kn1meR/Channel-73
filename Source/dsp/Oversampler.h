// Channel73 - zero-latency oversampling
//
// Polyphase IIR half-band filters (two parallel first-order allpass chains),
// coefficient design after Laurent de Soras' HIIR library (WTFPL). These are
// minimum-phase-like: there is no FIR pre-ringing and nothing for the host to
// compensate, so the plug-in reports 0 samples of latency at every setting.
// The only cost is a small, frequency-dependent group delay (measured in the
// test suite, well under 1 ms) -- the same kind of phase shift an analog
// transformer or coupling capacitor adds.
#pragma once
#include <array>
#include <cmath>
#include "Filters.h"

namespace c73 {

namespace halfband_design {

inline void transitionParams (double& k, double& q, double transition)
{
    k = std::tan ((1.0 - transition * 2.0) * kPi / 4.0);
    k *= k;
    const double kksqrt = std::pow (1.0 - k * k, 0.25);
    const double e  = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    const double e2 = e * e, e4 = e2 * e2;
    q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
}

inline double accNum (double q, int order, int c)
{
    int i = 0, j = 1;
    double acc = 0.0, term;
    do
    {
        term = std::pow (q, double (i * (i + 1))) * std::sin ((i * 2 + 1) * c * kPi / order) * j;
        acc += term;
        j = -j; ++i;
    } while (std::fabs (term) > 1e-100);
    return acc;
}

inline double accDen (double q, int order, int c)
{
    int i = 1, j = -1;
    double acc = 0.0, term;
    do
    {
        term = std::pow (q, double (i * i)) * std::cos (i * 2 * c * kPi / order) * j;
        acc += term;
        j = -j; ++i;
    } while (std::fabs (term) > 1e-100);
    return acc;
}

template <size_t N>
std::array<double, N> design (double transition)
{
    std::array<double, N> coefs {};
    double k, q;
    transitionParams (k, q, transition);
    const int order = int (N) * 2 + 1;
    for (size_t index = 0; index < N; ++index)
    {
        const int c = int (index) + 1;
        const double num = accNum (q, order, c) * std::pow (q, 0.25);
        const double den = accDen (q, order, c) + 0.5;
        const double ww = num / den, wwsq = ww * ww;
        const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        coefs[index] = (1.0 - x) / (1.0 + x);
    }
    return coefs;
}

} // namespace halfband_design

// One 2x stage. N = number of allpass coefficients (even).
template <size_t N>
struct HalfbandStage
{
    static_assert (N % 2 == 0, "N must be even");
    std::array<double, N> c {};
    std::array<double, N> ux {}, uy {}, dx {}, dy {};

    void setTransition (double tbw) { c = halfband_design::design<N> (tbw); reset(); }
    void reset() { ux.fill (0); uy.fill (0); dx.fill (0); dy.fill (0); }

    static inline double ap (double coef, double x, double& x1, double& y1)
    {
        const double y = coef * (x - y1) + x1;
        x1 = x; y1 = y;
        return y;
    }

    // one low-rate sample in -> two high-rate samples out
    inline void up (double in, double& o0, double& o1)
    {
        double even = in, odd = in;
        for (size_t i = 0; i < N; i += 2)
        {
            even = ap (c[i],     even, ux[i],     uy[i]);
            odd  = ap (c[i + 1], odd,  ux[i + 1], uy[i + 1]);
        }
        o0 = even; o1 = odd;
    }

    // two high-rate samples in -> one low-rate sample out
    inline double down (double in0, double in1)
    {
        double s0 = in1, s1 = in0;
        for (size_t i = 0; i < N; i += 2)
        {
            s0 = ap (c[i],     s0, dx[i],     dy[i]);
            s1 = ap (c[i + 1], s1, dx[i + 1], dy[i + 1]);
        }
        return 0.5 * (s0 + s1);
    }
};

} // namespace c73
