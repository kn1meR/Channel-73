// Channel73 - filter building blocks (no framework dependencies)
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>

namespace c73 {

constexpr double kPi = 3.14159265358979323846;

inline double dbToLin (double db) { return std::pow (10.0, db / 20.0); }

// Rational tanh approximation, C1-continuous at the clamp points (+/-3).
inline double fastTanh (double x)
{
    if (x > 3.0)  return 1.0;
    if (x < -3.0) return -1.0;
    const double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

//==============================================================================
// Second-order analog prototype H(s) = (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0), s in rad/s
struct Analog2
{
    double n0 = 1, n1 = 0, n2 = 0, d0 = 1, d1 = 0, d2 = 0;

    double magSq (double w) const
    {
        const double rn = n0 - n2 * w * w, in = n1 * w;
        const double rd = d0 - d2 * w * w, id = d1 * w;
        return (rn * rn + in * in) / (rd * rd + id * id);
    }

    static Analog2 peaking (double f0, double gainDb, double q)
    {
        const double w = 2 * kPi * f0, A = std::pow (10.0, gainDb / 40.0);
        return { w * w, w * A / q, 1.0, w * w, w / (A * q), 1.0 };
    }
    static Analog2 lowShelf (double f0, double gainDb, double q)
    {
        const double w = 2 * kPi * f0, A = std::pow (10.0, gainDb / 40.0), sA = std::sqrt (A);
        return { A * A * w * w, A * sA / q * w, A, w * w, sA / q * w, A };
    }
    static Analog2 highShelf (double f0, double gainDb, double q)
    {
        const double w = 2 * kPi * f0, A = std::pow (10.0, gainDb / 40.0), sA = std::sqrt (A);
        return { A * w * w, A * sA / q * w, A * A, A * w * w, sA / q * w, 1.0 };
    }
    static Analog2 highPass (double f0, double q)
    {
        const double w = 2 * kPi * f0;
        return { 0.0, 0.0, 1.0, w * w, w / q, 1.0 };
    }
    static Analog2 lowPass (double f0, double q)
    {
        const double w = 2 * kPi * f0;
        return { w * w, 0.0, 0.0, w * w, w / q, 1.0 };
    }
};

struct BiquadCoefs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

// Bilinear transform, pre-warped so the response is exact at wWarp (rad/s).
inline BiquadCoefs designBilinear (const Analog2& h, double fs, double wWarp)
{
    wWarp = std::min (wWarp, 0.95 * kPi * fs);
    const double K = wWarp / std::tan (wWarp / (2.0 * fs)), K2 = K * K;
    const double b0 = h.n2 * K2 + h.n1 * K + h.n0;
    const double b1 = 2.0 * (h.n0 - h.n2 * K2);
    const double b2 = h.n2 * K2 - h.n1 * K + h.n0;
    const double a0 = h.d2 * K2 + h.d1 * K + h.d0;
    const double a1 = 2.0 * (h.d0 - h.d2 * K2);
    const double a2 = h.d2 * K2 - h.d1 * K + h.d0;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

// Magnitude-matched ("decramped") design after M. Vicanek, "Matched Second Order
// Digital Filters" (2016): poles by impulse invariance, numerator solved so the
// digital magnitude equals the analog one at DC, Nyquist and wMatch. Keeps the
// 12 kHz shelf and 7.2 kHz peak shaped like the analog circuit at 44.1/48 kHz
// without oversampling (oversampling would cost latency or phase).
inline BiquadCoefs designMatched (const Analog2& h, double fs, double wMatch)
{
    using cd = std::complex<double>;
    if (std::fabs (h.d2) < 1e-300) return designBilinear (h, fs, wMatch);

    const cd disc = std::sqrt (cd (h.d1 * h.d1 - 4.0 * h.d2 * h.d0, 0.0));
    const cd p1 = (-h.d1 + disc) / (2.0 * h.d2), p2 = (-h.d1 - disc) / (2.0 * h.d2);
    const cd z1 = std::exp (p1 / fs), z2 = std::exp (p2 / fs);
    const double a1 = -(z1 + z2).real(), a2 = (z1 * z2).real();

    const double A0 = (1 + a1 + a2) * (1 + a1 + a2);
    const double A1 = (1 - a1 + a2) * (1 - a1 + a2);
    const double A2 = -4.0 * a2;

    const double wm  = std::clamp (wMatch / fs, 0.05, 0.9 * kPi);
    const double ph0 = std::cos (wm * 0.5) * std::cos (wm * 0.5);
    const double ph1 = 1.0 - ph0;
    const double ph2 = 4.0 * ph0 * ph1;

    const double B0 = h.magSq (0.0) * A0;
    const double B1 = h.magSq (kPi * fs) * A1;
    const double Aw = A0 * ph0 + A1 * ph1 + A2 * ph2;
    const double B2 = (h.magSq (wm * fs) * Aw - B0 * ph0 - B1 * ph1) / ph2;

    const double sB0 = std::sqrt (B0), sB1 = std::sqrt (B1);
    const double W = 0.5 * (sB0 + sB1);
    const double d = W * W + B2;
    if (d < 0.0 || ! std::isfinite (d)) return designBilinear (h, fs, wMatch);

    const double b0 = 0.5 * (W + std::sqrt (d));
    const double b1 = 0.5 * (sB0 - sB1);
    const double b2 = -B2 / (4.0 * b0);
    return { b0, b1, b2, a1, a2 };
}

// Numerator-only magnitude fit for given digital poles (a1, a2): match |H| at DC, Nyquist and wMatch.
inline bool fitNumerator (const Analog2& h, double fs, double wMatch, double a1, double a2, BiquadCoefs& out)
{
    const double A0 = (1 + a1 + a2) * (1 + a1 + a2), A1 = (1 - a1 + a2) * (1 - a1 + a2), A2 = -4.0 * a2;
    const double wm = std::clamp (wMatch / fs, 0.05, 0.9 * kPi);
    const double ph0 = std::cos (wm * 0.5) * std::cos (wm * 0.5), ph1 = 1.0 - ph0, ph2 = 4.0 * ph0 * ph1;
    const double B0 = h.magSq (0.0) * A0, B1 = h.magSq (kPi * fs) * A1;
    const double B2 = (h.magSq (wm * fs) * (A0 * ph0 + A1 * ph1 + A2 * ph2) - B0 * ph0 - B1 * ph1) / ph2;
    const double sB0 = std::sqrt (B0), sB1 = std::sqrt (B1), W = 0.5 * (sB0 + sB1), d = W * W + B2;
    if (d < 0.0 || ! std::isfinite (d)) return false;
    const double b0 = 0.5 * (W + std::sqrt (d));
    out = { b0, 0.5 * (sB0 - sB1), -B2 / (4.0 * b0), a1, a2 };
    return true;
}

// Swap numerator/denominator (numerators from the fits above are minimum phase, so this is stable).
// RBJ peaking and shelving cuts are exact reciprocals of the matching boosts.
inline BiquadCoefs invert (const BiquadCoefs& c)
{
    return { 1.0 / c.b0, c.a1 / c.b0, c.a2 / c.b0, c.b1 / c.b0, c.b2 / c.b0 };
}

// The design rules below were chosen from measured error against the analog
// prototypes (see tests/dsp_tests.cpp, section [1]).

// Bell: matched design for boosts, cut = inverse of the equivalent boost.
inline BiquadCoefs designPeaking (double f0, double gainDb, double q, double fs)
{
    const auto boost = Analog2::peaking (f0, std::fabs (gainDb), q);
    const BiquadCoefs c = f0 > fs / 12.0 ? designMatched (boost, fs, 2 * kPi * f0)
                                         : designBilinear (boost, fs, 2 * kPi * f0);
    return gainDb < 0.0 ? invert (c) : c;
}

// Shelves: poles from a bilinear transform warped at <= 0.2 fs, numerator fitted to the
// analog magnitude ("hybrid"); cut = inverse of the equivalent boost.
inline BiquadCoefs designShelf (bool high, double f0, double gainDb, double q, double fs)
{
    const auto boost = high ? Analog2::highShelf (f0, std::fabs (gainDb), q)
                            : Analog2::lowShelf  (f0, std::fabs (gainDb), q);
    BiquadCoefs c = designBilinear (boost, fs, 2 * kPi * std::min (f0, 0.2 * fs));
    if (f0 > fs / 12.0)
    {
        BiquadCoefs fitted;
        if (fitNumerator (boost, fs, 2 * kPi * f0, c.a1, c.a2, fitted)) c = fitted;
    }
    return gainDb < 0.0 ? invert (c) : c;
}

inline BiquadCoefs designLowPass (double f0, double q, double fs)
{
    return designBilinear (Analog2::lowPass (f0, q), fs, 2 * kPi * std::min (f0, 0.45 * fs));
}

inline BiquadCoefs designHighPass (double f0, double q, double fs)
{
    return designBilinear (Analog2::highPass (f0, q), fs, 2 * kPi * f0);
}

struct Biquad
{
    BiquadCoefs c;
    double s1 = 0, s2 = 0;

    void reset() { s1 = s2 = 0; }
    inline double process (double x)
    {
        const double y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

// First-order sections (bilinear, used only well below Nyquist)
struct OnePole
{
    double b0 = 1, b1 = 0, a1 = 0, s = 0;

    void setHighPass (double fc, double fs)
    {
        const double k = std::tan (kPi * fc / fs);
        const double n = 1.0 / (1.0 + k);
        b0 = n; b1 = -n; a1 = (k - 1.0) * n;
    }
    void setLowPass (double fc, double fs)
    {
        const double k = std::tan (kPi * fc / fs);
        const double n = 1.0 / (1.0 + k);
        b0 = k * n; b1 = k * n; a1 = (k - 1.0) * n;
    }
    void reset() { s = 0; }
    inline double process (double x)
    {
        const double y = b0 * x + s;
        s = b1 * x - a1 * y;
        return y;
    }
};

} // namespace c73
