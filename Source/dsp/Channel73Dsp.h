// Channel73 - class-A channel amplifier model (1073-style mic/line pre + 3-band EQ)
//
// Signal chain, following the stages described in the reference manual:
//
//   input transformer (impedance-dependent HF resonance + LF core saturation)
//   -> preamp stage 1 (class-A, asymmetric, dynamic bias shift)   [mic only]
//   -> preamp stage 2 (class-A, headroom reduced by EQ loading)
//   -> EQ: low cut (18 dB/oct) -> low shelf -> mid peak -> 12 kHz high shelf   [EQL in]
//   -> EQ amplifier (always in path, as on the hardware)
//   -> Level fader -> post-fader output amplifier (class-A) -> output transformer
//   -> Output trim (clean) -> polarity
//
// Everything is processed sample by sample with no look-ahead and no FIR
// filters. At 1x the latency is 0 samples. 2x/4x use IIR half-band filters
// whose low-frequency group delay is ~3 / ~4 samples; that integer delay is
// reported to the host (and applied to the dry/bypass path) so parallel
// tracks stay phase-aligned. Works at any buffer size, including 1 sample.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "Filters.h"
#include "Oversampler.h"

namespace c73 {

//==============================================================================
struct Params
{
    bool   micMode       = false;
    int    micIndex      = 7;     // 0 = OFF, 1..13 -> sensitivity -20 .. -80
    int    lineIndex     = 4;     // 0..6 -> sensitivity -20 .. +10, 7 = OFF (4 = "0")
    bool   micZHi        = true;
    bool   pad           = false;
    double highGainDb    = 0.0;   // +/-18
    int    midFreqIndex  = 0;     // 0 = OFF, 360, 700, 1.6k, 3.2k, 4.8k, 7.2k
    double midGainDb     = 0.0;   // +/-18
    int    lowFreqIndex  = 0;     // 0 = OFF, 35, 60, 110, 220
    double lowGainDb     = 0.0;   // +/-15
    int    lowCutIndex   = 0;     // 0 = OFF, 50, 80, 160, 300
    bool   phaseInvert   = false;
    bool   eqIn          = false; // EQL (default out, as in the manual)
    double levelDb       = 0.0;   // -60 (= -inf) .. +10
    double outputDb      = 0.0;   // +/-24
    bool   power         = true;
    int    oversampling  = 1;     // 0 = 1x (0 samples), 1 = 2x (3 samples), 2 = 4x (4 samples)
};

namespace tables {
constexpr int    kNumMic = 14, kNumLine = 8;
constexpr double kMidFreqs[]  = { 0, 360, 700, 1600, 3200, 4800, 7200 };
constexpr double kMidQ[]      = { 1, 0.75, 0.85, 1.0, 1.15, 1.35, 1.6 }; // Q rises with frequency
constexpr double kLowFreqs[]  = { 0, 35, 60, 110, 220 };
constexpr double kLowCut[]    = { 0, 50, 80, 160, 300 };

inline double micSensitivity (int idx)  { return -20.0 - 5.0 * (idx - 1); } // idx 1..13
inline double lineSensitivity (int idx) { return -20.0 + 5.0 * idx; }       // idx 0..6
inline bool   micOff (int idx)          { return idx <= 0; }
inline bool   lineOff (int idx)         { return idx >= 7; }

// Mic Z: LO = 300 ohm; HI = 1.2k for -20..-50, 600 ohm for -55..-80
inline double micImpedance (bool hi, int micIdx)
{
    if (! hi) return 300.0;
    return micSensitivity (micIdx) >= -50.0 ? 1200.0 : 600.0;
}
} // namespace tables

//==============================================================================
// Headroom / voicing constants. Internal level unit: 1.0 = 0 dBu, with the
// digital calibration 0 dBFS = +18 dBu (so -18 dBFS = 0 dBu nominal).
namespace voicing {
constexpr double kCalDb      = 18.0;
inline const double kRef     = dbToLin (kCalDb);
inline const double kTxIn    = dbToLin (20.0);  // input transformer LF core saturation
inline const double kPre1    = dbToLin (16.0);  // preamp stage 1 swing
inline const double kPre2    = dbToLin (24.0);  // preamp stage 2 swing
inline const double kEqAmp   = dbToLin (25.0);  // EQ amplifier swing
inline const double kOutAmp  = dbToLin (26.0);  // post-fader output amp swing
inline const double kTxOut   = dbToLin (23.0);  // output transformer LF core saturation
constexpr double kH2Pre1 = 0.010, kH2Pre2 = 0.008, kH2Eq = 0.003, kH2Out = 0.006; // class-A 2nd-order term
constexpr double kNegKnee = 1.35;               // asymmetric clipping: negative side softer/later
constexpr double kBiasPre = 0.18, kBiasOut = 0.10; // dynamic bias shift (duty-cycle clipping)
} // namespace voicing

//==============================================================================
// One class-A gain stage: soft asymmetric clip + quadratic (2nd harmonic) term,
// with an operating point that drifts with signal envelope (coupling capacitors
// charging) -> level-dependent duty-cycle asymmetry, then DC-correct.
struct ClassAStage
{
    double env = 0.0, atk = 0.0, rel = 0.0;
    double k = 1.0, h2 = 0.0, bias = 0.0;

    void prepare (double fs, double swing, double h2Amt, double biasAmt)
    {
        k = swing; h2 = h2Amt; bias = biasAmt;
        atk = 1.0 - std::exp (-1.0 / (0.004 * fs));
        rel = 1.0 - std::exp (-1.0 / (0.080 * fs));
        env = 0.0;
    }
    void reset() { env = 0.0; }

    // x / (1 + x^4)^(1/4): linear until close to the rail, then a firm, smooth knee
    // (class-A stages stay clean and mostly 2nd-order until they run out of swing)
    static inline double knee (double x)
    {
        const double x2 = x * x;
        return x / std::sqrt (std::sqrt (1.0 + x2 * x2));
    }
    static inline double shape (double n, double h2)
    {
        const double s = n >= 0.0 ? knee (n) : voicing::kNegKnee * knee (n / voicing::kNegKnee);
        return s + h2 * s * s;
    }

    inline double process (double x, double headroomScale = 1.0)
    {
        const double kk = k * headroomScale;
        const double n  = x / kk;
        const double a  = std::fabs (n);
        env += (a > env ? atk : rel) * (a - env);
        // operating point drifts toward the earlier-clipping rail as coupling caps charge:
        // the harder the drive, the more lopsided the clipping duty cycle
        const double b = bias * std::min (env, 2.0);
        return kk * (shape (n + b, h2) - shape (b, h2));
    }
};

// Transformer: 1st-order LF roll-off, LF core saturation (low band only, odd
// harmonics), damped HF resonance whose Q depends on source/load impedance.
struct Transformer
{
    OnePole lfRolloff, split;
    Biquad  hf;
    double  sat = 1.0;

    void reset() { lfRolloff.reset(); split.reset(); hf.reset(); }

    inline double process (double x)
    {
        x = lfRolloff.process (x);
        const double lo = split.process (x);
        const double hi = x - lo;
        x = hi + sat * fastTanh (lo / sat);
        return hf.process (x);
    }
};

//==============================================================================
class Channel73Dsp
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        os1.setTransition (0.04);   // ~100 dB image rejection, ~0.06 ms group delay
        os2.setTransition (0.13);
        for (auto& c : ch) { c.os1 = os1; c.os2 = os2; }
        configureRate();
        // jump smoothers to target
        cur = target;
        snapSmoothers();
        updateFilters (true);
        resetState();
    }

    void setParams (const Params& p)
    {
        const bool osChanged = p.oversampling != target.oversampling;
        target = p;
        if (osChanged && fs > 0) { configureRate(); updateFilters (true); resetState(); }
    }

    int getOversamplingFactor() const { return 1 << std::clamp (target.oversampling, 0, 2); }

    // Integer latency to report to the host for a given oversampling setting
    // (low-frequency group delay of the half-band filters, measured in tests [5]).
    static int latencyForOversampling (int os) { return os <= 0 ? 0 : (os == 1 ? 3 : 4); }
    int getLatencySamples() const { return latencyForOversampling (target.oversampling); }

    // In-place processing of non-interleaved float buffers.
    void process (float* const* data, int numChannels, int numSamples)
    {
        numChannels = std::min (numChannels, channels);
        int pos = 0;
        while (pos < numSamples)
        {
            const int n = std::min (kChunk, numSamples - pos);
            advanceSmoothers (n);
            updateFilters (false);

            // fully bypassed and settled: pass audio untouched, skip the model (CPU saved)
            if (wet.cur <= 0.0 && wet.tgt <= 0.0)
            {
                if (! stateCleared) { resetState(); stateCleared = true; }
                if (latency > 0)
                    for (int c = 0; c < numChannels; ++c)
                        for (int i = 0; i < n; ++i)
                            data[c][pos + i] = ch[(size_t) c].dryDelay.process (data[c][pos + i], latency);
                pos += n;
                continue;
            }
            stateCleared = false;

            for (int c = 0; c < numChannels; ++c)
            {
                auto& st = ch[(size_t) c];
                float* d = data[c] + pos;
                Smoother g1 = sPre1, g2 = sPre2, lvl = sLevel, out = sOut, w = wet, mute = sMute, pol = sPol, zl = sZLoss;
                for (int i = 0; i < n; ++i)
                {
                    const double dry = st.dryDelay.process (d[i], latency);
                    const double x0 = d[i];
                    g1.tick(); g2.tick(); lvl.tick(); out.tick(); w.tick(); mute.tick(); pol.tick(); zl.tick();
                    const double x = x0 * voicing::kRef * zl.cur;

                    double y;
                    if (osFactor == 1)
                        y = processModel (st, x, g1.cur, g2.cur, lvl.cur);
                    else if (osFactor == 2)
                    {
                        double a, b;
                        st.os1.up (x, a, b);
                        a = processModel (st, a, g1.cur, g2.cur, lvl.cur);
                        b = processModel (st, b, g1.cur, g2.cur, lvl.cur);
                        y = st.os1.down (a, b);
                    }
                    else
                    {
                        double a, b, q[4];
                        st.os1.up (x, a, b);
                        st.os2.up (a, q[0], q[1]);
                        st.os2.up (b, q[2], q[3]);
                        for (double& s : q) s = processModel (st, s, g1.cur, g2.cur, lvl.cur);
                        a = st.os2.down (q[0], q[1]);
                        b = st.os2.down (q[2], q[3]);
                        y = st.os1.down (a, b);
                    }

                    y = st.dcBlock.process (y) * (out.cur * mute.cur * pol.cur / voicing::kRef);
                    d[i] = (float) (dry + w.cur * (y - dry));
                }
                if (c == numChannels - 1) { sPre1 = g1; sPre2 = g2; sLevel = lvl; sOut = out; wet = w; sMute = mute; sPol = pol; sZLoss = zl; }
            }
            for (Smoother* s : { &sPre1, &sPre2, &sLevel, &sOut, &wet, &sMute, &sPol, &sZLoss })
                if (std::fabs (s->cur - s->tgt) < 1e-7) s->cur = s->tgt;
            pos += n;
        }
    }

    // Diagnostics for tests
    double getSampleRate() const { return fs; }

private:
    struct Smoother
    {
        double cur = 0, tgt = 0, coef = 0;
        inline void tick() { cur += coef * (tgt - cur); }
    };

    // Short integer delay so the dry/bypass path lines up with the oversampled path.
    struct DryDelay
    {
        std::array<float, 16> buf {};
        int w = 0;
        inline float process (float x, int d)
        {
            buf[(size_t) w] = x;
            const float y = buf[(size_t) ((w - d) & 15)];
            w = (w + 1) & 15;
            return y;
        }
        void reset() { buf.fill (0.0f); w = 0; }
    };

    struct ChannelState
    {
        DryDelay dryDelay;
        HalfbandStage<8> os1;
        HalfbandStage<4> os2;
        Transformer txIn, txOut;
        ClassAStage pre1, pre2, eqAmp, outAmp;
        OnePole lowCut1;
        Biquad lowCut2, lowShelf, mid, high, eqTone;
        OnePole dcBlock;
    };

    static constexpr int kChunk = 32;

    double fs = 0.0, fsOs = 0.0;
    int channels = 2, osFactor = 1, latency = 0;
    bool stateCleared = false;
    Params target, cur;
    HalfbandStage<8> os1;
    HalfbandStage<4> os2;
    std::array<ChannelState, kMaxChannels> ch;

    Smoother sPre1, sPre2, sLevel, sOut, wet, sMute, sPol, sZLoss;
    double eqHigh = 0, eqMid = 0, eqLow = 0, eqLoad = 1.0;   // smoothed EQ gains (dB)
    double eqHighA = 1e9, eqMidA = 1e9, eqLowA = 1e9;        // values filters were last built for
    int    builtMidF = -1, builtLowF = -1, builtLowCut = -1;
    bool   builtEq = false, builtMicMode = false; double builtZ = -1;
    bool   eqActive = false, midOn = false, lowOn = false, cutOn = false;

    //--------------------------------------------------------------------------
    void configureRate()
    {
        osFactor = 1 << std::clamp (target.oversampling, 0, 2);
        latency  = latencyForOversampling (target.oversampling);
        for (auto& c : ch) c.dryDelay.reset();
        fsOs = fs * osFactor;
        for (auto& c : ch)
        {
            c.pre1.prepare   (fsOs, voicing::kPre1,   voicing::kH2Pre1, voicing::kBiasPre);
            c.pre2.prepare   (fsOs, voicing::kPre2,   voicing::kH2Pre2, voicing::kBiasPre);
            c.eqAmp.prepare  (fsOs, voicing::kEqAmp,  voicing::kH2Eq,   0.0);
            c.outAmp.prepare (fsOs, voicing::kOutAmp, voicing::kH2Out,  voicing::kBiasOut);
            c.txIn.lfRolloff.setHighPass (4.0, fsOs);
            c.txIn.split.setLowPass (90.0, fsOs);
            c.txIn.sat = voicing::kTxIn;
            c.txOut.lfRolloff.setHighPass (6.0, fsOs);
            c.txOut.split.setLowPass (110.0, fsOs);
            c.txOut.sat = voicing::kTxOut;
            c.txOut.hf.c = designLowPass (48000.0, 0.62, fsOs);
            c.dcBlock.setHighPass (2.0, fs);
        }
        const double tau = 0.015, chunkTau = 0.020;
        for (Smoother* s : { &sPre1, &sPre2, &sLevel, &sOut, &wet, &sMute, &sPol, &sZLoss })
            s->coef = 1.0 - std::exp (-1.0 / (tau * fs));
        eqCoef = 1.0 - std::exp (-double (kChunk) / (chunkTau * fs));
        builtEq = false; builtZ = -1; builtMidF = builtLowF = builtLowCut = -1;
        eqHighA = eqMidA = eqLowA = 1e9;
    }
    double eqCoef = 0.1;

    void resetState()
    {
        for (auto& c : ch)
        {
            c.os1.reset(); c.os2.reset();
            c.txIn.reset(); c.txOut.reset();
            c.pre1.reset(); c.pre2.reset(); c.eqAmp.reset(); c.outAmp.reset();
            c.lowCut1.reset(); c.lowCut2.reset(); c.lowShelf.reset(); c.mid.reset(); c.high.reset(); c.eqTone.reset();
            c.dcBlock.reset();
        }
    }

    // Preamp gain staging derived from the switch positions.
    void computeTargets()
    {
        const auto& p = target;
        double gainDb = 0.0; bool off;
        if (p.micMode)
        {
            off = tables::micOff (p.micIndex);
            gainDb = off ? 0.0 : -tables::micSensitivity (p.micIndex) - (p.pad ? 20.0 : 0.0);
            // the mic gain is shared between the two class-A stages
            const double g1 = 0.55 * gainDb;
            sPre1.tgt = dbToLin (g1);
            sPre2.tgt = dbToLin (gainDb - g1);
            const double z = tables::micImpedance (p.micZHi, p.micIndex);
            // loading loss of a typical 150-ohm mic source into the selected input impedance (rel. to 1.2k)
            sZLoss.tgt = (z / (z + 150.0)) / (1200.0 / 1350.0);
        }
        else
        {
            off = tables::lineOff (p.lineIndex);
            gainDb = off ? 0.0 : -tables::lineSensitivity (p.lineIndex);
            sPre1.tgt = 1.0;              // stage 1 not used on the line input
            sPre2.tgt = dbToLin (gainDb);
            sZLoss.tgt = 1.0;
        }
        sMute.tgt  = off ? 0.0 : 1.0;   // gain switch OFF mutes, like the hardware
        sLevel.tgt = p.levelDb <= -59.99 ? 0.0 : dbToLin (p.levelDb);
        sOut.tgt   = dbToLin (p.outputDb);
        wet.tgt    = p.power ? 1.0 : 0.0;
        sPol.tgt   = p.phaseInvert ? -1.0 : 1.0;
    }

    void snapSmoothers()
    {
        computeTargets();
        for (Smoother* s : { &sPre1, &sPre2, &sLevel, &sOut, &wet, &sMute, &sPol, &sZLoss }) s->cur = s->tgt;
        eqHigh = target.highGainDb; eqMid = target.midGainDb; eqLow = target.lowGainDb;
    }

    void advanceSmoothers (int)
    {
        computeTargets();
        eqHigh += eqCoef * (target.highGainDb - eqHigh);
        eqMid  += eqCoef * (target.midGainDb - eqMid);
        eqLow  += eqCoef * (target.lowGainDb - eqLow);
        if (std::fabs (eqHigh - target.highGainDb) < 1e-3) eqHigh = target.highGainDb;
        if (std::fabs (eqMid  - target.midGainDb)  < 1e-3) eqMid  = target.midGainDb;
        if (std::fabs (eqLow  - target.lowGainDb)  < 1e-3) eqLow  = target.lowGainDb;
    }

    void updateFilters (bool force)
    {
        const auto& p = target;
        auto setAll = [this] (auto member, const BiquadCoefs& c) { for (auto& s : ch) (s.*member).c = c; };

        // input transformer HF resonance: damping depends on the impedance seen by the transformer
        const double z = p.micMode ? tables::micImpedance (p.micZHi, p.micIndex) : 0.0;
        if (force || std::fabs (z - builtZ) > 0.5 || p.micMode != builtMicMode)
        {
            const double q = p.micMode ? 0.52 + 0.38 * std::min (z, 1200.0) / 1200.0 : 0.70;
            for (auto& s : ch) s.txIn.hf.c = designLowPass (56000.0, q, fsOs);
            builtZ = z; builtMicMode = p.micMode;
        }

        // EQ loading of the preamp output: boosts lower stage-2 headroom even with EQL out
        const double boost = std::max (0.0, eqHigh) + (p.midFreqIndex ? std::max (0.0, eqMid) : 0.0)
                           + (p.lowFreqIndex ? std::max (0.0, eqLow) : 0.0);
        eqLoad = 1.0 / (1.0 + 0.25 * boost / 51.0);   // up to ~2 dB less headroom

        eqActive = p.eqIn;
        midOn = p.eqIn && p.midFreqIndex > 0;
        lowOn = p.eqIn && p.lowFreqIndex > 0;
        cutOn = p.eqIn && p.lowCutIndex > 0;

        if (force || std::fabs (eqHigh - eqHighA) > 1e-4)
        {
            setAll (&ChannelState::high, designShelf (true, 12000.0, eqHigh, 0.55, fsOs));
            eqHighA = eqHigh;
        }
        if (p.midFreqIndex > 0 && (force || builtMidF != p.midFreqIndex || std::fabs (eqMid - eqMidA) > 1e-4))
        {
            const double f = tables::kMidFreqs[p.midFreqIndex];
            // proportional Q: narrower as gain increases, and higher at higher centre frequencies
            const double q = tables::kMidQ[p.midFreqIndex] * (0.7 + 0.6 * std::fabs (eqMid) / 18.0);
            setAll (&ChannelState::mid, designPeaking (f, eqMid, q, fsOs));
            builtMidF = p.midFreqIndex; eqMidA = eqMid;
        }
        if (p.lowFreqIndex > 0 && (force || builtLowF != p.lowFreqIndex || std::fabs (eqLow - eqLowA) > 1e-4))
        {
            const double f = tables::kLowFreqs[p.lowFreqIndex];
            // inductor shelf: slight overshoot (bump/dip) just above the corner
            setAll (&ChannelState::lowShelf, designShelf (false, f, eqLow, 0.95, fsOs));
            builtLowF = p.lowFreqIndex; eqLowA = eqLow;
        }
        if (p.lowCutIndex > 0 && (force || builtLowCut != p.lowCutIndex))
        {
            const double f = tables::kLowCut[p.lowCutIndex];
            for (auto& s : ch) s.lowCut1.setHighPass (f, fsOs);                  // 6 dB/oct
            setAll (&ChannelState::lowCut2, designHighPass (f, 0.85, fsOs)); // +12 = 18 dB/oct
            builtLowCut = p.lowCutIndex;
        }
        if (force || ! builtEq)
        {
            // residual tone of the EQ circuitry, present even with EQL out (-0.25 dB @ 20 kHz)
            setAll (&ChannelState::eqTone, designLowPass (90000.0, 0.6, fsOs));
            builtEq = true;
        }
    }

    // One oversampled-rate sample through the analog chain.
    inline double processModel (ChannelState& s, double x, double g1, double g2, double lvl)
    {
        x = s.txIn.process (x);
        if (target.micMode)
            x = s.pre1.process (x * g1);
        x = s.pre2.process (x * g2, eqLoad);

        if (eqActive)
        {
            if (cutOn) x = s.lowCut2.process (s.lowCut1.process (x));
            if (lowOn) x = s.lowShelf.process (x);
            if (midOn) x = s.mid.process (x);
            x = s.high.process (x);
        }
        x = s.eqAmp.process (s.eqTone.process (x));

        x = s.outAmp.process (x * lvl);
        return s.txOut.process (x);
    }
};

} // namespace c73
