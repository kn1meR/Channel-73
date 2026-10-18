// Channel73 - EQ derived from the 1073 EQ board circuits (AMS Neve drawings, 1073/1084 manual issue 5)
//
//   BA205  (D/10042)  HF 12 kHz + LF shelves around EQ amp A   (RV1 10K, RV2 50K, S3)
//   BA211  (D/10048)  presence (mid) around EQ amp B            (RV3 10K, S5, T1280/T1530)
//   BA182C (D/10019C) passive 18 dB/oct high-pass, fed through R37 1K2, terminated by 5K1
//
// Each board is solved by nodal analysis from its component values to get the exact analog
// transfer function for the current switch and pot positions. That rational function is
// factored into poles/zeros, split into first/second-order sections and digitised with the
// decramped designs in Filters.h, so the curves hold up at 44.1/48 kHz without oversampling.
// The EQ amps are treated as ideal inverting virtual-earth amplifiers (two in cascade, so
// the EQ is non-inverting overall).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include "Filters.h"

namespace c73::circuit {

using cd = std::complex<double>;
constexpr double kPotEnd = 20.0;          // residual resistance at a pot's end stop (ohms)
constexpr double kNormW  = 2.0 * kPi * 1000.0; // frequency scaling for polynomial work

//==============================================================================
// Polynomials (ascending powers), max degree 8
struct Poly
{
    std::array<double, 9> c {};
    int deg = 0;

    static Poly constant (double v) { Poly p; p.c[0] = v; return p; }
    static Poly linear (double c0, double c1) { Poly p; p.c[0] = c0; p.c[1] = c1; p.deg = 1; return p; }

    Poly operator* (const Poly& o) const
    {
        Poly r; r.deg = std::min (8, deg + o.deg);
        for (int i = 0; i <= deg; ++i)
            for (int j = 0; j <= o.deg && i + j <= 8; ++j)
                r.c[(size_t) (i + j)] += c[(size_t) i] * o.c[(size_t) j];
        return r;
    }
    Poly operator+ (const Poly& o) const
    {
        Poly r; r.deg = std::max (deg, o.deg);
        for (int i = 0; i <= r.deg; ++i) r.c[(size_t) i] = c[(size_t) i] + o.c[(size_t) i];
        return r;
    }
    cd eval (cd x) const
    {
        cd r = 0.0;
        for (int i = deg; i >= 0; --i) r = r * x + c[(size_t) i];
        return r;
    }
    void trim (double relTol = 1e-11)
    {
        double mx = 0.0;
        for (int i = 0; i <= deg; ++i) mx = std::max (mx, std::fabs (c[(size_t) i]));
        while (deg > 0 && std::fabs (c[(size_t) deg]) <= relTol * mx) c[(size_t) deg--] = 0.0;
    }
};

// Roots of a real polynomial (Aberth-Ehrlich). Returns the number of roots written.
inline int polyRoots (const Poly& p, std::array<cd, 8>& roots)
{
    const int n = p.deg;
    if (n < 1) return 0;
    // monic copy
    std::array<cd, 9> a {};
    for (int i = 0; i <= n; ++i) a[(size_t) i] = p.c[(size_t) i] / p.c[(size_t) n];
    // initial guesses on a circle around the root radius bound
    double rad = 0.0;
    for (int i = 0; i < n; ++i) rad = std::max (rad, std::pow (std::abs (a[(size_t) i]), 1.0 / (n - i)));
    rad = std::max (rad, 1e-6);
    for (int k = 0; k < n; ++k) roots[(size_t) k] = std::polar (rad, 2.0 * kPi * (k + 0.25) / n + 0.4);

    auto evalBoth = [&] (cd x, cd& f, cd& df) {
        f = a[(size_t) n]; df = 0.0;
        for (int i = n - 1; i >= 0; --i) { df = df * x + f; f = f * x + a[(size_t) i]; }
    };
    for (int it = 0; it < 200; ++it)
    {
        double maxStep = 0.0;
        for (int k = 0; k < n; ++k)
        {
            cd f, df;
            evalBoth (roots[(size_t) k], f, df);
            if (! (std::norm (f) > 0.0)) continue;
            const cd ratio = f / df;
            cd sum = 0.0;
            for (int j = 0; j < n; ++j)
                if (j != k) sum += 1.0 / (roots[(size_t) k] - roots[(size_t) j]);
            const cd step = ratio / (1.0 - ratio * sum);
            roots[(size_t) k] -= step;
            maxStep = std::max (maxStep, std::abs (step) / std::max (1.0, std::abs (roots[(size_t) k])));
        }
        if (maxStep < 1e-14) break;
    }
    // clean up tiny imaginary parts of real roots
    for (int k = 0; k < n; ++k)
        if (std::fabs (roots[(size_t) k].imag()) < 1e-9 * std::max (1.0, std::abs (roots[(size_t) k])))
            roots[(size_t) k] = roots[(size_t) k].real();
    return n;
}

//==============================================================================
// A cascade of up to 6 digital sections plus a gain
struct Cascade
{
    static constexpr int kMaxSections = 6;
    std::array<BiquadCoefs, kMaxSections> s {};
    int n = 0;
    double gain = 1.0;

    cd response (double f, double fs) const
    {
        const cd z1 = std::polar (1.0, -2.0 * kPi * f / fs);
        cd h = gain;
        for (int i = 0; i < n; ++i)
        {
            const auto& q = s[(size_t) i];
            h *= (q.b0 + q.b1 * z1 + q.b2 * z1 * z1) / (1.0 + q.a1 * z1 + q.a2 * z1 * z1);
        }
        return h;
    }
};

// Frequencies where digitised sections are compared with the analog circuit
struct ErrGrid
{
    static constexpr int kMax = 128;
    std::array<double, kMax> f {};
    std::array<cd, kMax> z1 {};
    int n = 0;
    explicit ErrGrid (double fs)
    {
        for (double x = 20.0; x < std::min (20000.0, 0.45 * fs) && n < kMax; x *= 1.12)
        {
            f[(size_t) n] = x;
            z1[(size_t) n] = std::polar (1.0, -2.0 * kPi * x / fs);
            ++n;
        }
    }
};

// Digitise one analog section. Candidate designs (bilinear warped at the pole, zero or in
// between; poles warped low with a magnitude-fitted numerator; matched) are scored against
// the analog magnitude on the grid, *including the error already accumulated by earlier
// sections* (ctx, dB per grid point), so errors cancel rather than stack. ctx is updated.
inline BiquadCoefs digitiseSection (const Analog2& h, double fp, double fs, const ErrGrid& g,
                                    std::array<double, ErrGrid::kMax>& ctx)
{
    std::array<BiquadCoefs, 8> cand {};
    int nc = 0;
    const double w = 2 * kPi * std::max (1.0, std::min (fp, 0.45 * fs));
    cand[(size_t) nc++] = designBilinear (h, fs, w);
    const double wz = std::sqrt (std::fabs (h.n2) > 0.0 ? std::fabs (h.n0 / h.n2) : (std::fabs (h.n1) > 0.0 ? (h.n0 / h.n1) * (h.n0 / h.n1) : 0.0));
    if (wz > 2 * kPi * 1.0)
        for (double t : { 0.5, 1.0 })
            cand[(size_t) nc++] = designBilinear (h, fs, std::min (std::exp (std::log (w) * (1 - t) + std::log (wz) * t), 0.9 * kPi * fs));
    if (std::fabs (h.d2) > 0.0)
    {
        for (double warp : { 0.12, 0.2, 0.3 })
        {
            const BiquadCoefs c = designBilinear (h, fs, 2 * kPi * std::min (fp, warp * fs));
            BiquadCoefs fitted;
            if (fitNumerator (h, fs, w, c.a1, c.a2, fitted)) cand[(size_t) nc++] = fitted;
        }
        cand[(size_t) nc++] = designMatched (h, fs, w);
    }

    int best = 0; double bestErr = 1e300;
    std::array<double, ErrGrid::kMax> e {}, bestE {};
    for (int c = 0; c < nc; ++c)
    {
        const auto& q = cand[(size_t) c];
        if (! std::isfinite (q.b0 + q.b1 + q.b2 + q.a1 + q.a2)) continue;
        double worst = 0.0;
        for (int i = 0; i < g.n; ++i)
        {
            const cd z1 = g.z1[(size_t) i];
            const double d = std::abs ((q.b0 + q.b1 * z1 + q.b2 * z1 * z1) / (1.0 + q.a1 * z1 + q.a2 * z1 * z1));
            const double a = std::sqrt (h.magSq (2.0 * kPi * g.f[(size_t) i]));
            e[(size_t) i] = (d > 0.0 && a > 0.0) ? 20.0 * std::log10 (d / a) : 0.0;
            worst = std::max (worst, std::fabs (ctx[(size_t) i] + e[(size_t) i]));
        }
        if (worst < bestErr) { bestErr = worst; best = c; bestE = e; }
    }
    for (int i = 0; i < g.n; ++i) ctx[(size_t) i] += bestE[(size_t) i];
    return cand[(size_t) best];
}

inline BiquadCoefs digitiseSection (const Analog2& h, double fp, double fs)
{
    const ErrGrid g (fs);
    std::array<double, ErrGrid::kMax> ctx {};
    return digitiseSection (h, fp, fs, g, ctx);
}

// Analog H(s) = num(s/kNormW) / den(s/kNormW)  ->  digital cascade, magnitude-locked at fRef.
inline Cascade digitiseRational (Poly num, Poly den, double fs, double fRef)
{
    num.trim(); den.trim();
    std::array<cd, 8> zr {}, pr {};
    int nz = polyRoots (num, zr), np = polyRoots (den, pr);
    // cancel pole/zero pairs that coincide (e.g. a common factor of s left by the circuit algebra)
    for (int i = 0; i < nz; ++i)
        for (int j = 0; j < np; ++j)
            if (std::abs (zr[(size_t) i] - pr[(size_t) j]) < 1e-7 * std::max (1e-3, std::abs (pr[(size_t) j])))
            {
                zr[(size_t) i] = zr[(size_t) --nz]; pr[(size_t) j] = pr[(size_t) --np];
                --i; break;
            }

    struct Factor { double c0, c1, c2; double w; int order; };   // c2 s^2 + c1 s + c0 (real), natural freq w
    auto group = [] (std::array<cd, 8>& r, int n, std::array<Factor, 8>& out) {
        int m = 0;
        std::array<double, 8> reals {}; int nr = 0;
        for (int i = 0; i < n; ++i)
        {
            const cd x = r[(size_t) i] * kNormW;
            if (x.imag() > 0.0) out[(size_t) m++] = { std::norm (x), -2.0 * x.real(), 1.0, std::abs (x), 2 };
            else if (! (std::fabs (x.imag()) > 0.0)) reals[(size_t) nr++] = x.real();
        }
        for (int i = 0; i < nr; ++i) out[(size_t) m++] = { -reals[(size_t) i], 1.0, 0.0, std::fabs (reals[(size_t) i]), 1 };
        for (int i = 1; i < m; ++i)      // insertion sort by natural frequency
            for (int j = i; j > 0 && out[(size_t) j].w < out[(size_t) j - 1].w; --j)
                std::swap (out[(size_t) j], out[(size_t) j - 1]);
        return m;
    };
    std::array<Factor, 8> pf {}, zf {};
    const int npf = group (pr, np, pf), nzf = group (zr, nz, zf);

    bool merged = false;
    auto build = [&] (int strategy) -> Cascade {
        // Sections: each holds a pole polynomial (order 1 or 2) and the zeros assigned to it.
        struct Sec { Poly pole, zero; int slots = 0, used = 0; double w = 0.0; };
        std::array<Sec, 8> secs {};
        int ns = 0;
        for (int i = 0; i < npf; ++i)
        {
            const auto& f = pf[(size_t) i];
            Sec sc;
            sc.pole.c[0] = f.c0; sc.pole.c[1] = f.c1; sc.pole.c[2] = f.c2; sc.pole.deg = f.order;
            sc.zero = Poly::constant (1.0);
            sc.slots = f.order; sc.w = f.w;
            secs[(size_t) ns++] = sc;
        }
        auto mergeSecs = [&] (int a, int b) {            // b into a, then remove b
            auto& A = secs[(size_t) a]; const auto& B = secs[(size_t) b];
            A.pole = A.pole * B.pole; A.zero = A.zero * B.zero;
            A.slots += B.slots; A.used += B.used; A.w = std::sqrt (A.w * B.w);
            for (int k = b; k < ns - 1; ++k) secs[(size_t) k] = secs[(size_t) k + 1];
            --ns;
        };
        auto logDist = [] (double x, double y) { return std::fabs (std::log (std::max (x, 1e-3)) - std::log (std::max (y, 1e-3))); };
        auto addZero = [&] (int k, const Factor& z) {
            Poly f; f.c[0] = z.c0; f.c[1] = z.c1; f.c[2] = z.c2; f.deg = z.order;
            secs[(size_t) k].zero = secs[(size_t) k].zero * f;
            secs[(size_t) k].used += z.order;
        };
        // complex zero pairs: nearest second-order section with room, else merge two first-order ones
        std::array<bool, 8> taken {};
        for (int j = 0; j < nzf; ++j)
        {
            const auto& z = zf[(size_t) j];
            if (z.order != 2) continue;
            int bestK = -1; double bestD = 1e300;
            for (int k = 0; k < ns; ++k)
                if (secs[(size_t) k].slots - secs[(size_t) k].used >= 2 && logDist (z.w, secs[(size_t) k].w) < bestD)
                    { bestD = logDist (z.w, secs[(size_t) k].w); bestK = k; }
            if (bestK < 0)
            {
                // merge the two first-order sections with free slots closest to this zero pair
                // (strategy 1 keeps sections above fs/12 out of the merge when it can, so the
                //  frequency-critical HF poles stay in their own, accurately warped section)
                int k1 = -1, k2 = -1; double d1 = 1e300, d2 = 1e300;
                int lowCount = 0;
                for (int k = 0; k < ns; ++k)
                    if (secs[(size_t) k].slots == 1 && secs[(size_t) k].used == 0 && secs[(size_t) k].w < 2 * kPi * fs / 12.0) ++lowCount;
                for (int k = 0; k < ns; ++k)
                {
                    if (secs[(size_t) k].slots != 1 || secs[(size_t) k].used != 0) continue;
                    if (strategy == 1 && lowCount >= 2 && secs[(size_t) k].w >= 2 * kPi * fs / 12.0) continue;
                    const double d = logDist (z.w, secs[(size_t) k].w);
                    if (d < d1) { d2 = d1; k2 = k1; d1 = d; k1 = k; }
                    else if (d < d2) { d2 = d; k2 = k; }
                }
                if (k1 < 0 || k2 < 0) continue;
                mergeSecs (std::min (k1, k2), std::max (k1, k2));
                merged = true;
                bestK = std::min (k1, k2);
            }
            addZero (bestK, z);
            taken[(size_t) j] = true;
        }
        // real zeros: global greedy on frequency distance
        for (;;)
        {
            int bestJ = -1, bestK = -1; double bestD = 1e300;
            for (int j = 0; j < nzf; ++j)
            {
                if (taken[(size_t) j] || zf[(size_t) j].order != 1) continue;
                for (int k = 0; k < ns; ++k)
                    if (secs[(size_t) k].used < secs[(size_t) k].slots && logDist (zf[(size_t) j].w, secs[(size_t) k].w) < bestD)
                        { bestD = logDist (zf[(size_t) j].w, secs[(size_t) k].w); bestJ = j; bestK = k; }
            }
            if (bestJ < 0) break;
            addZero (bestK, zf[(size_t) bestJ]);
            taken[(size_t) bestJ] = true;
        }
        // keep within the cascade size: merge neighbouring first-order sections
        while (ns > Cascade::kMaxSections)
        {
            int best = -1; double bd = 1e300;
            for (int k = 0; k + 1 < ns; ++k)
                if (secs[(size_t) k].slots == 1 && secs[(size_t) k + 1].slots == 1 && logDist (secs[(size_t) k].w, secs[(size_t) k + 1].w) < bd)
                    { bd = logDist (secs[(size_t) k].w, secs[(size_t) k + 1].w); best = k; }
            if (best < 0) break;
            mergeSecs (best, best + 1);
        }

        Cascade out;
        out.n = ns;
        const ErrGrid grid (fs);
        std::array<double, ErrGrid::kMax> ctx {};
        // section gains are arbitrary (monic polynomials), so compare shapes: normalise each
        // section's error at the reference frequency before accumulating
        for (int k = 0; k < ns; ++k)
        {
            const auto& sc = secs[(size_t) k];
            Analog2 h { sc.zero.c[0], sc.zero.c[1], sc.zero.c[2], sc.pole.c[0], sc.pole.c[1], sc.pole.c[2] };
            const double href = std::sqrt (h.magSq (2 * kPi * fRef));
            if (href > 0.0) { h.n0 /= href; h.n1 /= href; h.n2 /= href; }
            out.s[(size_t) k] = digitiseSection (h, sc.w / (2 * kPi), fs, grid, ctx);
        }

        // lock the overall magnitude to the analog function at fRef
        const cd sRef (0.0, 2 * kPi * fRef / kNormW);
        const double ha = std::abs (num.eval (sRef) / den.eval (sRef));
        const double hd = std::abs (out.response (fRef, fs));
        out.gain = hd > 0.0 ? ha / hd : 1.0;
        return out;
        };

    // try both zero-placement strategies, keep the cascade closest to the analog function
    const ErrGrid g (fs);
    auto score = [&] (const Cascade& c) {
        double worst = 0.0;
        for (int i = 0; i < g.n; ++i)
        {
            const cd sa (0.0, 2 * kPi * g.f[(size_t) i] / kNormW);
            const double a = std::abs (num.eval (sa) / den.eval (sa)), d = std::abs (c.response (g.f[(size_t) i], fs));
            if (a > 0.0 && d > 0.0) worst = std::max (worst, std::fabs (20.0 * std::log10 (d / a)));
        }
        return worst;
    };
    Cascade best = build (0);
    if (merged)
    {
        const Cascade alt = build (1);
        if (score (alt) < score (best)) best = alt;
    }
    return best;
}

//==============================================================================
// Tiny nodal-analysis engine with an ideal inverting op-amp.
// Node ids: >= 0 unknown, kGnd = 0 V, kSrc = 1 V input, kVg = virtual earth (0 V, KCL row).
constexpr int kGnd = -1, kSrc = -2, kVg = -3;

template <size_t N>
struct Mna
{
    std::array<std::array<cd, N>, N> a {};
    std::array<cd, N> b {};
    int vgRow = (int) N - 1;      // the op-amp output node's KCL row is replaced by the virtual-earth row

    void reset() { for (auto& r : a) r.fill (0.0); b.fill (0.0); }

    void add (int n1, int n2, cd y)
    {
        auto fixedV = [] (int n) { return n == kSrc ? 1.0 : 0.0; };
        auto stampRow = [&] (int row, int other) {
            if (other >= 0) a[(size_t) row][(size_t) other] -= y;
            else b[(size_t) row] += y * fixedV (other);
        };
        for (int pass = 0; pass < 2; ++pass)
        {
            const int p = pass ? n2 : n1, q = pass ? n1 : n2;
            if (p >= 0 && p != vgOwner)
            {
                a[(size_t) p][(size_t) p] += y;
                stampRow (p, q);
            }
            if (p == kVg)   // current into the virtual earth from q
            {
                if (q >= 0) a[(size_t) vgRow][(size_t) q] += y;
                else b[(size_t) vgRow] -= y * fixedV (q);
            }
        }
    }
    int vgOwner = -100;   // set to the op-amp output node index (its own KCL is not used)

    // Gaussian elimination; returns determinant and solution
    cd solve (std::array<cd, N>& x)
    {
        auto m = a; auto r = b; cd det = 1.0;
        constexpr int n = (int) N;
        for (int c = 0; c < n; ++c)
        {
            int piv = c; double best = std::abs (m[(size_t) c][(size_t) c]);
            for (int i = c + 1; i < n; ++i)
                if (std::abs (m[(size_t) i][(size_t) c]) > best) { best = std::abs (m[(size_t) i][(size_t) c]); piv = i; }
            if (! (best > 0.0)) { x.fill (0.0); return 0.0; }
            if (piv != c) { std::swap (m[(size_t) piv], m[(size_t) c]); std::swap (r[(size_t) piv], r[(size_t) c]); det = -det; }
            det *= m[(size_t) c][(size_t) c];
            for (int i = c + 1; i < n; ++i)
            {
                const cd f = m[(size_t) i][(size_t) c] / m[(size_t) c][(size_t) c];
                if (! (std::norm (f) > 0.0)) continue;
                for (int j = c; j < n; ++j) m[(size_t) i][(size_t) j] -= f * m[(size_t) c][(size_t) j];
                r[(size_t) i] -= f * r[(size_t) c];
            }
        }
        for (int i = n - 1; i >= 0; --i)
        {
            cd acc = r[(size_t) i];
            for (int j = i + 1; j < n; ++j) acc -= m[(size_t) i][(size_t) j] * x[(size_t) j];
            x[(size_t) i] = acc / m[(size_t) i][(size_t) i];
        }
        return det;
    }
};

// Sample num/den polynomials of a circuit H(s) = out(s) on a circle and interpolate (exact for deg < 16).
template <typename EvalFn>   // EvalFn(cd s, cd& det, cd& numTimesDet)
inline void interpolateRational (EvalFn&& eval, Poly& num, Poly& den)
{
    constexpr int M = 16;
    std::array<cd, M> vn {}, vd {};
    for (int m = 0; m < M; ++m)
    {
        const cd z = std::polar (1.0, 2.0 * kPi * m / M);
        eval (z * kNormW, vd[(size_t) m], vn[(size_t) m]);
    }
    num = {}; den = {};
    num.deg = den.deg = 8;
    for (int k = 0; k <= 8; ++k)
    {
        cd sn = 0.0, sd = 0.0;
        for (int m = 0; m < M; ++m)
        {
            const cd w = std::polar (1.0, -2.0 * kPi * k * m / M);
            sn += vn[(size_t) m] * w; sd += vd[(size_t) m] * w;
        }
        num.c[(size_t) k] = sn.real() / M; den.c[(size_t) k] = sd.real() / M;
    }
}

//==============================================================================
// Knob (dB at the reference frequency) <-> pot wiper position, tabulated once.
// The gain falls monotonically from the boost end (wiper 0) to the cut end (wiper 1).
struct WiperMap
{
    static constexpr int kN = 257;
    std::array<double, kN> db {};
    bool valid = false;

    template <typename Fn> void build (Fn&& gainDbAt)
    {
        for (int i = 0; i < kN; ++i) db[(size_t) i] = gainDbAt (double (i) / (kN - 1));
        valid = true;
    }
    double wiperFor (double target) const
    {
        if (target >= db[0]) return 0.0;
        if (target <= db[kN - 1]) return 1.0;
        int lo = 0, hi = kN - 1;
        while (hi - lo > 1) { const int m = (lo + hi) / 2; (db[(size_t) m] > target ? lo : hi) = m; }
        const double t = (db[(size_t) lo] - target) / (db[(size_t) lo] - db[(size_t) hi]);
        return (lo + t) / (kN - 1);
    }
};

//==============================================================================
// BA205: HF + LF shelving around EQ amp A
struct Ba205
{
    // hf: RV1 wiper, 0 = input end (full boost), 1 = output end (full cut)
    // lf: RV2 wiper, 0 = input (V) end (full boost), 1 = output (A) end; lfPos 0 = OFF, 1..4 = 35/60/110/220 Hz
    static constexpr double kLfCaps[5] = { 0.0, 200e-9, 100e-9, 47e-9, 15e-9 };
    static constexpr double kLfFreqs[5] = { 0.0, 35.0, 60.0, 110.0, 220.0 };

    enum { P1, P, J, N, N1, V, A, L, K, H, kNodes };

    static cd evaluate (cd s, double hf, double lf, int lfPos, cd* detOut = nullptr)
    {
        Mna<kNodes> m;
        m.vgOwner = H; m.vgRow = H;
        const auto R = [] (double r) { return cd (1.0 / r); };
        const double ra = std::max (kPotEnd, hf * 10e3), rb = std::max (kPotEnd, (1.0 - hf) * 10e3);
        m.add (kSrc, P1, R (620.0)); m.add (P1, P, s * 22e-9); m.add (P, J, R (ra));
        m.add (J, N, R (rb));        m.add (N, N1, s * 22e-9); m.add (N1, H, R (620.0));
        m.add (J, kVg, s * 15e-9);
        m.add (kSrc, V, R (6.2e3));  m.add (H, A, R (6.2e3));
        m.add (L, kVg, R (12e3));    m.add (K, L, s * 10e-9); m.add (K, kGnd, R (12e3));
        if (lfPos <= 0)
        {
            m.add (V, L, R (kPotEnd)); m.add (L, A, R (kPotEnd));
        }
        else
        {
            const double rv = std::max (kPotEnd, lf * 50e3), rA = std::max (kPotEnd, (1.0 - lf) * 50e3);
            const double c = kLfCaps[lfPos];
            m.add (V, L, R (rv)); m.add (L, A, R (rA));
            m.add (A, K, s * c);  m.add (V, K, s * c);
        }
        std::array<cd, kNodes> x {};
        const cd det = m.solve (x);
        if (detOut) *detOut = det;
        return -x[H];   // two inverting EQ amps in cascade: report the non-inverted shape
    }

    static Cascade design (double hf, double lf, int lfPos, double fs)
    {
        Poly num, den;
        interpolateRational ([&] (cd s, cd& det, cd& nd) { const cd h = evaluate (s, hf, lf, lfPos, &det); nd = h * det; }, num, den);
        return digitiseRational (num, den, fs, 1000.0);
    }

    // knob dB -> wiper: HF measured at 12 kHz, LF at half the selected frequency
    static double hfGainDb (double w)            { return 20 * std::log10 (std::abs (evaluate (cd (0, 2 * kPi * 12000.0), w, 0.5, 0))); }
    static double lfGainDb (double w, int lfPos) { return 20 * std::log10 (std::abs (evaluate (cd (0, 2 * kPi * 0.5 * kLfFreqs[lfPos]), 0.5, w, lfPos))); }
    static double hfWiperFor (double db) { return solveWiper ([] (double w) { return hfGainDb (w); }, db); }
    static double lfWiperFor (double db, int lfPos)
    {
        if (lfPos <= 0) return 0.5;
        return solveWiper ([=] (double w) { return lfGainDb (w, lfPos); }, db);
    }

    template <typename Fn>
    static double solveWiper (Fn&& gainAt, double db)
    {
        // gain falls monotonically from the boost end (0) to the cut end (1)
        double lo = 0.0, hi = 1.0;
        if (db >= gainAt (0.0)) return 0.0;
        if (db <= gainAt (1.0)) return 1.0;
        for (int i = 0; i < 40; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            (gainAt (mid) > db ? lo : hi) = mid;
        }
        return 0.5 * (lo + hi);
    }
};

//==============================================================================
// BA211: presence. Closed-form solution of the board (derivation in the README):
//   H(s) = 1 + k (Ga - Gb) C s / (A2 s^2 + A1 s + A0),  k = G4 (G1 + G2 + G3) / (G2 G3)
struct Ba211
{
    // panel frequency index 1..6 -> capacitor, inductance (T1530 taps solved from the panel
    // frequency; T1280 is a single ~0.21 H winding, so its positions land near 3.1/5.1/7.1 kHz)
    static constexpr double kCaps[7] = { 0.0, 100e-9, 47e-9, 22e-9, 12.2e-9, 4.7e-9, 2.42e-9 };
    static constexpr double kPanel[7] = { 0.0, 360.0, 700.0, 1600.0, 3200.0, 4800.0, 7200.0 };
    static constexpr double kT1280 = 0.21;

    static void lcr (int pos, double& l, double& c, double& rl)
    {
        c = kCaps[pos];
        if (pos <= 3) { const double w = 2 * kPi * kPanel[pos]; l = 1.0 / (w * w * c); rl = 300.0 * std::sqrt (l / 1.95); }
        else { l = kT1280; rl = 40.0; }
    }

    static Analog2 analog (double wiper, int pos)
    {
        double l, c, rl; lcr (pos, l, c, rl);
        const double G1 = 1 / 6.8e3, G2 = 1 / 6.8e3, G3 = 1 / 8.2e3, G4 = 1 / 2.7e3;
        const double Ga = 1.0 / std::max (kPotEnd, wiper * 10e3), Gb = 1.0 / std::max (kPotEnd, (1.0 - wiper) * 10e3);
        const double k = G4 * (G1 + G2 + G3) / (G2 * G3), P = Ga + Gb;
        const double A2 = P * G4 * l * c, A1 = c * (Gb * k + P + G4 + P * G4 * rl), A0 = P * G4;
        return { A0, A1 + k * (Ga - Gb) * c, A2, A0, A1, A2 };
    }
    static double centreGainDb (double wiper, int pos)
    {
        double l, c, rl; lcr (pos, l, c, rl);
        return 10 * std::log10 (analog (wiper, pos).magSq (1.0 / std::sqrt (l * c)));
    }
    static double wiperFor (double db, int pos)
    {
        return Ba205::solveWiper ([=] (double w) { return centreGainDb (w, pos); }, db);
    }
    static BiquadCoefs design (double wiper, int pos, double fs)
    {
        if (pos <= 0) return {};
        double l, c, rl; lcr (pos, l, c, rl);
        const double f0 = 1.0 / (2 * kPi * std::sqrt (l * c));
        // boost and cut are exact reciprocals by symmetry of the bridge: design the boost, invert for cut
        const bool cut = wiper > 0.5;
        const auto h = analog (cut ? 1.0 - wiper : wiper, pos);
        const BiquadCoefs b = digitiseSection (h, f0, fs);
        return cut ? invert (b) : b;
    }
};

//==============================================================================
// BA182/C: passive C - L - C T-section high-pass, T1295 tapped 10 H inductor,
// driven through R37 (1K2) and terminated by 5K1. Unity passband (the fixed
// 1K2/5K1 loss is part of the module's level plan, present with the filter OFF too).
struct Ba182
{
    static constexpr double kC[5] = { 0.0, 1.0e-6, 0.47e-6, 0.22e-6, 0.122e-6 };
    static constexpr double kL[5] = { 0.0, 10.0, 7.0, 3.0, 1.3 };
    static constexpr double kRs = 1.2e3, kRl = 5.1e3, kRwPerHenry = 60.0;   // winding resistance is not on the drawing; 60 R/H keeps the 50/80 Hz positions within 0.35 dB of flat (the manual calls the terminated filter maximally flat) without softening the slope much

    static void polys (int pos, Poly& num, Poly& den)
    {
        const double C = kC[pos], L = kL[pos], Rw = kRwPerHenry * L, w = kNormW;
        // with s = w z:  Num = RL C^2 s^2 (Ls + Rw);  Den = (Rs sC+1)(Rw+sL) sC + (Rs sC+1)(RL sC+1) + (Rw+sL)(RL sC+1) sC
        const Poly sC = Poly::linear (0.0, C * w);
        const Poly rsC1 = Poly::linear (1.0, kRs * C * w), rlC1 = Poly::linear (1.0, kRl * C * w);
        const Poly zl = Poly::linear (Rw, L * w);
        num = Poly::constant (kRl) * sC * sC * zl * Poly::constant (1.0 / (C * C));   // RL C^2 s^2 (..) / C^2 scaled below
        num = Poly::constant (C * C) * num;
        den = rsC1 * zl * sC + rsC1 * rlC1 + zl * rlC1 * sC;
        // normalise passband (s -> inf) to unity: lim H = RL / (Rs + RL)
        const double hInf = kRl / (kRs + kRl);
        num = Poly::constant (1.0 / hInf) * num;
    }
    static cd evaluate (cd s, int pos)
    {
        if (pos <= 0) return 1.0;
        Poly n, d; polys (pos, n, d);
        return n.eval (s / kNormW) / d.eval (s / kNormW);
    }
    static Cascade design (int pos, double fs)
    {
        if (pos <= 0) { Cascade c; c.n = 0; return c; }
        Poly n, d; polys (pos, n, d);
        return digitiseRational (n, d, fs, 5000.0);
    }
};

} // namespace c73::circuit
