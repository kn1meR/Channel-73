// Channel73 DSP measurement suite (no framework needed)
//   g++ -O2 -std=c++17 dsp_tests.cpp -o dsp_tests && ./dsp_tests
#include "../Source/dsp/Channel73Dsp.h"
#include <chrono>
#include <functional>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace c73;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf ("  FAIL: " __VA_ARGS__); std::printf ("\n"); } } while (0)

// Hann-windowed single-bin DFT amplitude (sine amplitude units)
static double tone (const std::vector<double>& v, double f, double fs, size_t start)
{
    double re = 0, im = 0, wsum = 0;
    const size_t N = v.size() - start;
    for (size_t i = 0; i < N; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2 * kPi * i / N);
        const double ph = 2 * kPi * f * double (i + start) / fs;
        re += w * v[i + start] * std::cos (ph);
        im += w * v[i + start] * std::sin (ph);
        wsum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

static std::vector<double> run (Params p, double fs, const std::vector<double>& in, int block = 256)
{
    Channel73Dsp d;
    d.setParams (p);
    d.prepare (fs, 1);
    std::vector<float> buf (in.begin(), in.end());
    for (size_t pos = 0; pos < buf.size(); pos += (size_t) block)
    {
        float* ptr = buf.data() + pos;
        d.process (&ptr, 1, (int) std::min<size_t> ((size_t) block, buf.size() - pos));
    }
    return std::vector<double> (buf.begin(), buf.end());
}

static std::vector<double> sine (double f, double ampDbfs, double fs, double seconds)
{
    std::vector<double> v ((size_t) (fs * seconds));
    const double a = dbToLin (ampDbfs);
    for (size_t i = 0; i < v.size(); ++i) v[i] = a * std::sin (2 * kPi * f * double (i) / fs);
    return v;
}

static double gainAt (Params p, double f, double fs, double lvl = -40.0)
{
    p.oversampling = 0; // section [2] is specified at 1x
    auto in = sine (f, lvl, fs, 0.5);
    auto out = run (p, fs, in);
    const size_t st = (size_t) (fs * 0.25);
    return 20 * std::log10 (tone (out, f, fs, st) / tone (in, f, fs, st));
}

//==============================================================================
static void testFilterDesign()
{
    std::printf ("\n[1] EQ filter designs vs analog prototype (max |error| dB, 20 Hz .. 16 kHz / .. 20 kHz)\n");
    struct Case { const char* name; Analog2 h; double fc; std::function<BiquadCoefs (double)> design; };
    const Case cases[] = {
        { "high shelf +18 @12k", Analog2::highShelf (12000, 18, 0.55), 12000, [] (double fs) { return designShelf (true, 12000, 18, 0.55, fs); } },
        { "high shelf -18 @12k", Analog2::highShelf (12000, -18, 0.55), 12000, [] (double fs) { return designShelf (true, 12000, -18, 0.55, fs); } },
        { "high shelf +6 @12k",  Analog2::highShelf (12000, 6, 0.55), 12000, [] (double fs) { return designShelf (true, 12000, 6, 0.55, fs); } },
        { "mid +18 @7.2k",       Analog2::peaking (7200, 18, 1.6 * 1.3), 7200, [] (double fs) { return designPeaking (7200, 18, 1.6 * 1.3, fs); } },
        { "mid -18 @7.2k",       Analog2::peaking (7200, -18, 1.6 * 1.3), 7200, [] (double fs) { return designPeaking (7200, -18, 1.6 * 1.3, fs); } },
        { "mid -18 @4.8k",       Analog2::peaking (4800, -18, 1.35 * 1.3), 4800, [] (double fs) { return designPeaking (4800, -18, 1.35 * 1.3, fs); } },
        { "mid +12 @360",        Analog2::peaking (360, 12, 0.75 * 1.1), 360, [] (double fs) { return designPeaking (360, 12, 0.75 * 1.1, fs); } },
        { "low shelf +15 @35",   Analog2::lowShelf (35, 15, 0.95), 35, [] (double fs) { return designShelf (false, 35, 15, 0.95, fs); } },
        { "low shelf -15 @220",  Analog2::lowShelf (220, -15, 0.95), 220, [] (double fs) { return designShelf (false, 220, -15, 0.95, fs); } },
        { "low cut 2nd @300",    Analog2::highPass (300, 0.85), 300, [] (double fs) { return designHighPass (300, 0.85, fs); } },
        { "in-xfmr LP 56k",      Analog2::lowPass (56000, 0.9), 56000, [] (double fs) { return designLowPass (56000, 0.9, fs); } },
    };
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        std::printf ("  fs=%.0f\n", fs);
        for (auto& c : cases)
        {
            auto bq = c.design (fs);
            auto bil = designBilinear (c.h, fs, 2 * kPi * std::min (c.fc, 0.45 * fs));
            double e16 = 0, e20 = 0, b16 = 0;
            for (double f = 20; f <= 20000; f *= 1.02)
            {
                auto mag = [&] (const BiquadCoefs& q) {
                    using cd = std::complex<double>;
                    const cd z1 = std::polar (1.0, -2 * kPi * f / fs);
                    return std::abs ((q.b0 + q.b1 * z1 + q.b2 * z1 * z1) / (1.0 + q.a1 * z1 + q.a2 * z1 * z1));
                };
                const double ref = 10 * std::log10 (c.h.magSq (2 * kPi * f));
                const double err = std::fabs (20 * std::log10 (mag (bq)) - ref);
                const double eb  = std::fabs (20 * std::log10 (mag (bil)) - ref);
                if (f <= 16000) { e16 = std::max (e16, err); b16 = std::max (b16, eb); }
                e20 = std::max (e20, err);
            }
            std::printf ("    %-22s %.2f / %.2f dB   (plain bilinear: %.2f dB to 16k)\n", c.name, e16, e20, b16);
            CHECK (e20 < 0.85, "%s error %.2f dB at fs %.0f", c.name, e20, fs);
        }
    }
}

static void testUnityAndEq()
{
    std::printf ("\n[2] Full channel, low level (-40 dBFS), 48 kHz, 1x\n");
    Params p; // Line, 0, EQL out
    const double fs = 48000;
    std::printf ("  Line 0, EQ out:  ");
    for (double f : { 20.0, 50.0, 100.0, 1000.0, 10000.0, 20000.0 }) std::printf ("%gHz %+.2f  ", f, gainAt (p, f, fs));
    std::printf ("\n");
    CHECK (std::fabs (gainAt (p, 1000, fs)) < 0.3, "unity gain at 1 kHz");

    p.lineIndex = 0; std::printf ("  Line -20 gain @1k: %+.2f dB (expect +20)\n", gainAt (p, 1000, fs, -60));
    CHECK (std::fabs (gainAt (p, 1000, fs, -60) - 20) < 0.5, "line -20 gives +20 dB");
    p.lineIndex = 6; std::printf ("  Line +10 gain @1k: %+.2f dB (expect -10)\n", gainAt (p, 1000, fs));
    p = Params(); p.micMode = true; p.micIndex = 7; // -50
    std::printf ("  Mic -50, Z HI (1.2k) gain @1k: %+.2f dB (expect +50)\n", gainAt (p, 1000, fs, -80));
    p.pad = true; std::printf ("  Mic -50 + pad: %+.2f dB (expect +30)\n", gainAt (p, 1000, fs, -80));
    p.pad = false; p.micZHi = false; std::printf ("  Mic -50, Z LO (300R): %+.2f dB (loading loss with 150R source)\n", gainAt (p, 1000, fs, -80));
    p = Params(); p.levelDb = -60; std::printf ("  Level -inf: %+.1f dB\n", gainAt (p, 1000, fs));
    p = Params(); p.lineIndex = 7; std::printf ("  Line OFF: %+.1f dB (mutes)\n", gainAt (p, 1000, fs));
    CHECK (gainAt (p, 1000, fs) < -90, "line OFF mutes");
    p = Params(); p.outputDb = -12; std::printf ("  Output -12: %+.2f dB\n", gainAt (p, 1000, fs));

    std::printf ("  EQ curves (EQL in):\n");
    auto curve = [&] (const char* name, Params q) {
        std::printf ("    %-26s", name);
        for (double f : { 30.0, 60.0, 100.0, 200.0, 400.0, 1000.0, 3200.0, 7200.0, 12000.0, 16000.0, 20000.0 })
            std::printf (" %5.1f", gainAt (q, f, fs));
        std::printf ("\n");
    };
    std::printf ("    %-26s", "freq (Hz)");
    for (int f : { 30, 60, 100, 200, 400, 1000, 3200, 7200, 12000, 16000, 20000 }) std::printf (" %5d", f);
    std::printf ("\n");
    Params q; q.eqIn = true;
    curve ("flat", q);
    q.highGainDb = 18;  curve ("high +18", q);
    q.highGainDb = -18; curve ("high -18", q);
    q = Params(); q.eqIn = true; q.midFreqIndex = 6; q.midGainDb = 18; curve ("mid 7.2k +18", q);
    q.midFreqIndex = 3; q.midGainDb = -12; curve ("mid 1.6k -12", q);
    q.midFreqIndex = 1; q.midGainDb = 10;  curve ("mid 360 +10", q);
    q = Params(); q.eqIn = true; q.lowFreqIndex = 2; q.lowGainDb = 15; curve ("low 60 +15", q);
    q.lowFreqIndex = 4; q.lowGainDb = -15; curve ("low 220 -15", q);
    q = Params(); q.eqIn = true; q.lowCutIndex = 4; curve ("low cut 300", q);
    const double s1 = gainAt (q, 75, fs), s2 = gainAt (q, 37.5, fs);
    std::printf ("    low cut 300 slope (75 -> 37.5 Hz): %.1f dB/oct (expect ~18)\n", s1 - s2);
    CHECK (std::fabs ((s1 - s2) - 18) < 2.5, "low cut slope");
    q = Params(); q.highGainDb = 18; // EQL out: EQ knobs do not shape the response
    CHECK (std::fabs (gainAt (q, 16000, fs) - gainAt (Params(), 16000, fs)) < 0.1, "EQL out bypasses EQ response");
    CHECK (std::fabs (gainAt (Params(), 20000, fs)) < 1.0 && std::fabs (gainAt (Params(), 10000, fs)) < 0.5, "flat response with EQ out");
}

static void harmonics (const char* name, Params p, double inDb, double fs = 48000)
{
    const double f = 1000;
    auto in = sine (f, inDb, fs, 1.0);
    auto out = run (p, fs, in);
    const size_t st = (size_t) (fs * 0.5);
    const double h1 = tone (out, f, fs, st);
    double rest = 0;
    double hd[6] = {};
    for (int k = 2; k <= 9; ++k) { const double a = tone (out, f * k, fs, st); rest += a * a; if (k <= 5) hd[k] = a; }
    std::printf ("  %-34s out %+6.1f dBFS  H2 %6.1f  H3 %6.1f  H4 %6.1f  H5 %6.1f dBc  THD %.3f%%\n", name,
                 20 * std::log10 (h1 / std::sqrt (1.0)), 20 * std::log10 (hd[2] / h1 + 1e-12), 20 * std::log10 (hd[3] / h1 + 1e-12),
                 20 * std::log10 (hd[4] / h1 + 1e-12), 20 * std::log10 (hd[5] / h1 + 1e-12), 100 * std::sqrt (rest) / h1);
}

static void testHarmonics()
{
    std::printf ("\n[3] Harmonic signature, 1 kHz sine, 48 kHz, 2x  (0 dBu = -18 dBFS)\n");
    Params p; p.oversampling = 1;
    harmonics ("Line 0, -18 dBFS (0 dBu)", p, -18);
    harmonics ("Line 0, -6 dBFS (+12 dBu)", p, -6);
    harmonics ("Line 0, 0 dBFS (+18 dBu)", p, 0);
    p.lineIndex = 0; harmonics ("Line -20 (+20 dB), -18 dBFS in", p, -18);
    p.lineIndex = 0; p.outputDb = -20; harmonics ("Line -20, -10 dBFS in, Output -20", p, -10);
    p = Params(); p.oversampling = 1; p.levelDb = 10; p.outputDb = -10; harmonics ("Line 0, Level +10, -12 dBFS in", p, -12);
    p = Params(); p.oversampling = 1; p.micMode = true; p.micIndex = 7; p.outputDb = 0; harmonics ("Mic -50, -68 dBFS in (-50 dBu mic)", p, -68);
    p.micIndex = 13; harmonics ("Mic -80, -78 dBFS in (hot gain)", p, -78);
    p = Params(); p.oversampling = 1; p.micMode = true; p.micIndex = 3; p.outputDb = -24; harmonics ("Line signal into Mic -30 (creative)", p, -18);

    // EQ settings change clipping even with EQL out (EQ loading)
    p = Params(); p.oversampling = 1; p.lineIndex = 0;
    Params q = p; q.highGainDb = 18; q.midFreqIndex = 3; q.midGainDb = 18; q.lowFreqIndex = 2; q.lowGainDb = 15;
    harmonics ("Line -20, -14 dBFS, EQ flat (EQL out)", p, -14);
    harmonics ("Line -20, -14 dBFS, EQ boosted (EQL out)", q, -14);

    // sanity: 2nd harmonic dominant at moderate drive (class-A signature)
    p = Params(); p.oversampling = 1;
    auto out = run (p, 48000, sine (1000, -6, 48000, 1.0));
    const double h2 = tone (out, 2000, 48000, 24000), h3 = tone (out, 3000, 48000, 24000);
    CHECK (h2 > h3, "2nd harmonic should dominate at moderate drive");
}

static void testAliasing()
{
    std::printf ("\n[4] Aliasing: 7.013 kHz sine, heavy drive (Line -20, -6 dBFS), 48 kHz\n");
    const double fs = 48000, f = 7013;
    for (int os = 0; os < 3; ++os)
    {
        Params p; p.lineIndex = 0; p.oversampling = os; p.outputDb = -20;
        auto out = run (p, fs, sine (f, -6, fs, 1.0));
        const size_t st = 24000;
        // harmonic frequencies after folding are excluded; sum energy elsewhere on a fine grid
        const double h1 = tone (out, f, fs, st);
        double worst = 0;
        for (double g = 100; g < 23900; g += 50)
        {
            bool harmonic = false;
            for (int k = 1; k * f < fs / 2; ++k)
                if (std::fabs (g - k * f) < 120) harmonic = true;
            if (! harmonic) worst = std::max (worst, tone (out, g, fs, st));
        }
        std::printf ("  %dx: worst alias component %.1f dBc\n", 1 << os, 20 * std::log10 (worst / h1));
        if (os == 1) CHECK (20 * std::log10 (worst / h1) < -60, "2x aliasing");
    }
}

static void testAliasModerate()
{
    std::printf ("\n[4b] Aliasing at normal drive: 7.013 kHz, Line 0, -6 dBFS (+12 dBu), 48 kHz\n");
    const double fs = 48000, f = 7013;
    for (int os = 0; os < 3; ++os)
    {
        Params p; p.oversampling = os;
        auto out = run (p, fs, sine (f, -6, fs, 1.0));
        const double h1 = tone (out, f, fs, 24000);
        double worst = 0;
        for (double g = 100; g < 23900; g += 50)
        {
            bool harmonic = false;
            for (int k = 1; k * f < fs / 2; ++k) if (std::fabs (g - k * f) < 120) harmonic = true;
            if (! harmonic) worst = std::max (worst, tone (out, g, fs, 24000));
        }
        std::printf ("  %dx: worst alias component %.1f dBc\n", 1 << os, 20 * std::log10 (worst / h1));
    }
}

static void testLatency()
{
    std::printf ("\n[5] Latency. Reported: 1x = 0, 2x = 3, 4x = 4 samples. Measured group delay of the whole channel (EQ out, low level)\n");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (int os = 0; os < 3; ++os)
        {
            Params p; p.oversampling = os;
            const int L = 1 << 16;
            std::vector<double> imp (L, 0.0); imp[0] = 1e-3;
            auto h = run (p, fs, imp);
            auto phase = [&] (double f) {
                double re = 0, im = 0;
                for (int n = 0; n < L; ++n) { re += h[n] * std::cos (2 * kPi * f * n / fs); im -= h[n] * std::sin (2 * kPi * f * n / fs); }
                return std::atan2 (im, re);
            };
            auto gd = [&] (double f) {
                double d = phase (f * 1.005) - phase (f * 0.995);
                while (d > kPi) d -= 2 * kPi;
            while (d < -kPi) d += 2 * kPi;
                return -d / (2 * kPi * f * 0.01) * 1000.0; // ms
            };
            int peak = 0; for (int n = 0; n < 512; ++n) if (std::fabs (h[n]) > std::fabs (h[peak])) peak = n;
            std::printf ("  fs %6.0f %dx: impulse peak at sample %d | group delay 1 kHz %.3f ms (%.2f smp), 5 kHz %.3f ms, 15 kHz %.3f ms\n",
                         fs, 1 << os, peak, gd (1000), gd (1000) * fs / 1000, gd (5000), gd (15000));
            CHECK (std::fabs (gd (1000) * fs / 1000 - Channel73Dsp::latencyForOversampling (os)) < 1.5, "reported latency matches measured delay");
            if (os == 0) CHECK (peak <= 1, "1x impulse peak must be immediate");
        }
}

static void testFuzz()
{
    std::printf ("\n[6] Stability fuzz: random parameters + block sizes (1..2048) at 44.1-192 kHz\n");
    std::mt19937 rng (1234);
    std::uniform_real_distribution<double> u (0, 1);
    int runs = 0; bool ok = true;
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        Channel73Dsp d; Params p; d.prepare (fs, 2);
        std::vector<float> L (4096), R (4096);
        for (int it = 0; it < 400; ++it)
        {
            p.micMode = u (rng) < 0.5; p.micIndex = int (u (rng) * 14); p.lineIndex = int (u (rng) * 8);
            p.micZHi = u (rng) < 0.5; p.pad = u (rng) < 0.3;
            p.highGainDb = -18 + 36 * u (rng); p.midGainDb = -18 + 36 * u (rng); p.lowGainDb = -15 + 30 * u (rng);
            p.midFreqIndex = int (u (rng) * 7); p.lowFreqIndex = int (u (rng) * 5); p.lowCutIndex = int (u (rng) * 5);
            p.eqIn = u (rng) < 0.7; p.phaseInvert = u (rng) < 0.5; p.power = u (rng) < 0.9;
            p.levelDb = -60 + 70 * u (rng); p.outputDb = -24 + 48 * u (rng); p.oversampling = int (u (rng) * 3);
            d.setParams (p);
            const int n = it % 7 == 0 ? 1 : 1 + int (u (rng) * 2047);
            const double amp = it % 5 == 0 ? 4.0 : 1.0; // occasional +12 dBFS abuse
            for (int i = 0; i < n; ++i) { L[(size_t) i] = float (amp * (2 * u (rng) - 1)); R[(size_t) i] = i % 64 == 0 ? float (amp) : 0.0f; }
            float* chans[2] = { L.data(), R.data() };
            d.process (chans, 2, n);
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (L[(size_t) i]) || ! std::isfinite (R[(size_t) i]) || std::fabs (L[(size_t) i]) > 1e4) ok = false;
            ++runs;
        }
    }
    std::printf ("  %d randomized blocks: %s\n", runs, ok ? "all finite and bounded" : "PROBLEM");
    CHECK (ok, "fuzz produced non-finite or unbounded output");
}

static void testBypass()
{
    std::printf ("\n[7] Power OFF = bit-transparent after the fade\n");
    for (int os = 0; os < 3; ++os)
    {
        Params p; p.power = false; p.lineIndex = 0; p.highGainDb = 18; p.eqIn = true; p.oversampling = os;
        const int lat = Channel73Dsp::latencyForOversampling (os);
        auto in = sine (1000, -6, 48000, 0.3);
        auto out = run (p, 48000, in);
        double maxDiff = 0;
        for (size_t i = (size_t) lat; i < in.size(); ++i) maxDiff = std::max (maxDiff, std::fabs (out[i] - (double) (float) in[i - (size_t) lat]));
        std::printf ("  %dx: output = input delayed by the reported %d samples, max diff %g\n", 1 << os, lat, maxDiff);
        CHECK (maxDiff == 0.0, "bypass should be a bit-exact (delayed) copy");
    }
}

static void testCpu()
{
    std::printf ("\n[8] CPU cost, stereo, 48 kHz, 256-sample blocks, EQ fully engaged (this machine)\n");
    for (int os = 0; os < 3; ++os)
    {
        Channel73Dsp d; Params p; p.eqIn = true; p.midFreqIndex = 3; p.lowFreqIndex = 2; p.lowCutIndex = 1; p.oversampling = os;
        p.micMode = true; d.setParams (p); d.prepare (48000, 2);
        std::vector<float> L (256), R (256); std::mt19937 rng (1);
        std::uniform_real_distribution<float> u (-0.01f, 0.01f);
        const int blocks = 48000 * 10 / 256;
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 256; ++i) { L[(size_t) i] = u (rng); R[(size_t) i] = u (rng); }
            float* c[2] = { L.data(), R.data() };
            d.process (c, 2, 256);
        }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  %dx: %.2f%% of one core (10 s of audio in %.0f ms)\n", 1 << os, 100 * sec / 10.0, sec * 1000);
    }
}

int main()
{
    testFilterDesign();
    testUnityAndEq();
    testHarmonics();
    testAliasing();
    testAliasModerate();
    testLatency();
    testFuzz();
    testBypass();
    testCpu();
    std::printf ("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL CHECKS PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
