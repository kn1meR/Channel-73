// Channel73 - class-A channel amplifier model (1073-style mic/line pre + 3-band EQ)
//
// Signal chain and node levels follow Neve drawings E/10006 (level diagram) and
// EH10023 (1073 circuit), for 0 dBm out at nominal input:
//
//   MIC:  10468 transformer (1200 R -> 4k8 = +6 dB, 300 R -> 4k8 = +12 dB)
//         -> [-55..-80 only] first amp (BA184/BA284), output -42 dBm
//         -> BA283AM (+28 dB mic)                                   -> -14 dBm
//   LINE: 31267 transformer (10k:600, -12 dB) -> attenuator -> BA283AM (+18 dB) -> -14 dBm
//   EQ (EQL in): BA205 LF + HF shelves -> EQ amp A -> BA211 presence -> EQ amp B
//         -> BA182 passive high-pass (last, terminated by the fader)
//   (the EQ input stays connected to the BA283 output with EQL out: it still loads it)
//   -> output level control -> BA283AV output amp -> LO1166 transformer -> 0 dBm
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
#include "CircuitEq.h"

namespace c73 {

//==============================================================================
struct Params
{
    bool   micMode       = false;
    int    micIndex      = 7;     // 0 = OFF, 1..13 -> sensitivity -20 .. -80
    int    lineIndex     = 4;     // 0..6 -> sensitivity -20 .. +10, 7 = OFF (4 = "0")
    bool   micZHi        = true;
    bool   pad           = false;
    double highGainDb    = 0.0;   // +/-16 (datasheet)
    int    midFreqIndex  = 0;     // 0 = OFF, 360, 700, 1.6k, 3.2k, 4.8k, 7.2k
    double midGainDb     = 0.0;   // +/-18
    int    lowFreqIndex  = 0;     // 0 = OFF, 35, 60, 110, 220
    double lowGainDb     = 0.0;   // +/-16 (datasheet)
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
constexpr double kLowFreqs[]  = { 0, 35, 60, 110, 220 };
constexpr double kLowCut[]    = { 0, 50, 80, 160, 300 };

inline double micSensitivity (int idx)  { return -20.0 - 5.0 * (idx - 1); } // idx 1..13
inline double lineSensitivity (int idx) { return -20.0 + 5.0 * idx; }       // idx 0..6
inline bool   micOff (int idx)          { return idx <= 0; }
inline bool   lineOff (int idx)         { return idx >= 7; }

// The first mic amp is switched in only for -55..-80; -20..-50 go via an attenuator
// straight to the BA283 (E/10006). That change of load on the 10468 secondary is why
// the HI impedance differs between the two ranges.
inline bool micUsesFirstAmp (int idx) { return micSensitivity (idx) <= -55.0; }

// Mic Z: LO = 300 ohm; HI = 1.2k for -20..-50, 600 ohm for -55..-80
inline double micImpedance (bool hi, int micIdx)
{
    if (! hi) return 300.0;
    return micSensitivity (micIdx) >= -50.0 ? 1200.0 : 600.0;
}
} // namespace tables

//==============================================================================
// Headroom / voicing constants. Internal level unit: 1.0 = 0 dBu, with the
// digital calibration 0 dBFS = +18 dBu (so -18 dBFS = 0 dBu nominal; the
// hardware's nominal line in/out level is 0 dBu).
//
// Calibrated against the AMS Neve 1073 datasheet (user manual issue 5.1):
//   THD <= 0.07 % from 50 Hz to 10 kHz at +20 dBu out, max output > +26 dBu,
//   response +/-0.5 dB 20 Hz-20 kHz and -3 dB at 40 kHz (EQ out).
// tests/dsp_tests.cpp section [9] checks every one of these.
namespace voicing {
constexpr double kCalDb      = 18.0;
inline const double kRef     = dbToLin (kCalDb);
// Stage swings are absolute levels at each node. Nominal node levels (E/10006) for 0 dBu
// out: first amp out -42, BA283AM out / EQ -14, output 0. So the output stage clips first;
// the BA283AM and EQ amps only clip with EQ boost or abuse; the first amp almost never.
constexpr double kStage1NodeDb = -42.0, kEqNodeDb = -14.0;
inline const double kPre1    = dbToLin (16.0);  // first mic amp (BA184/BA284)
inline const double kPre2    = dbToLin (20.0);  // BA283AM
inline const double kEqAmp   = dbToLin (18.0);  // EQ amps (BA284 sections); EQ boosts eat into this
inline const double kOutAmp  = dbToLin (30.5);  // BA283AV output amp + LO1166 -> ~+27 dBu at 1 % THD
// Transformer core: flux (integral of voltage) normalised so a 50 Hz sine of amplitude A gives
// flux A. Saturation flux chosen so +20 dBu at 50 Hz stays inside the 0.07 % spec, while
// lower frequencies and hotter levels saturate progressively (odd harmonics, LF compression).
inline const double kTxLineSat = dbToLin (48.0);  // 31267 line input (10k:600)
inline const double kTxMicSat  = dbToLin (38.0);  // 10468 mic input: smaller core, only stressed by line-level sources
inline const double kTxOutSat  = dbToLin (47.0);  // LO1166 output
constexpr double kH2Pre1 = 0.0010, kH2Pre2 = 0.0010, kH2Eq = 0.0004, kH2Out = 0.0012; // class-A 2nd-order term
constexpr double kNegKnee = 1.25;               // asymmetric clipping: negative side softer/later
constexpr double kBiasPre = 0.15, kBiasOut = 0.10; // dynamic bias shift (duty-cycle clipping)
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

    // x / (1 + x^6)^(1/6): essentially linear until close to the rail, then a firm,
    // smooth knee. (The datasheet's 0.07 % at +20 dBu, only ~7 dB below clipping,
    // rules out a gentle tanh-style curve.)
    static inline double knee (double x)
    {
        const double x2 = x * x, u = x2 * x2 * x2;
        if (u < 1e-3)   // well below the rail (the usual case): series for (1+u)^(-1/6), error < 1e-10
            return x * (1.0 + u * (-1.0 / 6.0 + u * (7.0 / 72.0 - u * (91.0 / 1296.0))));
        return x / std::cbrt (std::sqrt (1.0 + u));
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

// Transformer: 1st-order LF roll-off, core saturation driven by flux (the
// integral of the voltage, so distortion rises ~12 dB per octave going down in
// frequency, odd-order), and an HF resonance whose damping depends on the
// source/load impedance.
struct Transformer
{
    OnePole lfRolloff, flux;
    Biquad  hf;
    double  fluxGain = 1.0, invSat2 = 0.0;

    void prepare (double fs, double lfCorner, double satFlux)
    {
        lfRolloff.setHighPass (lfCorner, fs);
        constexpr double fLeak = 2.0, fRef = 50.0;  // leaky integrator, unity flux/volt at 50 Hz
        flux.setLowPass (fLeak, fs);
        fluxGain = fRef / fLeak;
        invSat2 = 1.0 / (satFlux * satFlux);
    }
    void reset() { lfRolloff.reset(); flux.reset(); hf.reset(); }

    inline double process (double x)
    {
        x = lfRolloff.process (x);
        const double phi = flux.process (x) * fluxGain;
        x /= 1.0 + 0.5 * phi * phi * invSat2;       // magnetising current loads the source as the core saturates
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
        buildWiperMaps();
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
        Transformer txMic, txLine, txOut;
        ClassAStage pre1, pre2, eqAmpA, eqAmpB, outAmp;
        std::array<Biquad, circuit::Cascade::kMaxSections> ba205, ba182;   // circuit-derived EQ sections (CircuitEq.h)
        Biquad presence, eqTone;
        OnePole dcBlock;
    };

    static constexpr int kChunk = 32;
    // HF response: input transformer + output transformer -> -3 dB at ~40 kHz, +/-0.5 dB to 20 kHz
    static constexpr double kTxInHf = 40000.0, kTxInQLine = 0.70;
    static constexpr double kOutStageGain = 14.0; // EQ node (-14 dBm) -> output (0 dBm)
    static constexpr double kTxOutHf = 95000.0, kTxOutQ = 0.70;

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
    int    ba205N = 0, ba182N = 0;
    unsigned eqUpdateCounter = 0;
    // knob -> pot wiper tables (analog, sample-rate independent; built once, off the hot path)
    circuit::WiperMap hfMap;
    std::array<circuit::WiperMap, 5> lfMap;
    std::array<circuit::WiperMap, 7> presMap;
    void buildWiperMaps()
    {
        if (hfMap.valid) return;
        hfMap.build ([] (double w) { return circuit::Ba205::hfGainDb (w); });
        for (int i = 1; i < 5; ++i) lfMap[(size_t) i].build ([i] (double w) { return circuit::Ba205::lfGainDb (w, i); });
        for (int i = 1; i < 7; ++i) presMap[(size_t) i].build ([i] (double w) { return circuit::Ba211::centreGainDb (w, i); });
    }
    double ba205Gain = 1.0, ba182Gain = 1.0;
    bool   ba205Flat = true;
    bool   builtEq = false, builtMicMode = false; double builtZ = -1;
    bool   eqActive = false, midOn = false, lowOn = false, cutOn = false, useFirstAmp = false;

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
            c.eqAmpA.prepare (fsOs, voicing::kEqAmp,  voicing::kH2Eq,   0.0);
            c.eqAmpB.prepare (fsOs, voicing::kEqAmp,  voicing::kH2Eq,   0.0);
            c.outAmp.prepare (fsOs, voicing::kOutAmp, voicing::kH2Out,  voicing::kBiasOut);
            c.txMic.prepare  (fsOs, 2.5, voicing::kTxMicSat);
            c.txLine.prepare (fsOs, 2.5, voicing::kTxLineSat);
            c.txOut.prepare (fsOs, 3.5, voicing::kTxOutSat);
            c.txOut.hf.c = designLowPass (kTxOutHf, kTxOutQ, fsOs);
            c.dcBlock.setHighPass (1.5, fs);
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
            c.txMic.reset(); c.txLine.reset(); c.txOut.reset();
            c.pre1.reset(); c.pre2.reset(); c.eqAmpA.reset(); c.eqAmpB.reset(); c.outAmp.reset();
            for (auto& b : c.ba205) b.reset();
            for (auto& b : c.ba182) b.reset();
            c.presence.reset(); c.eqTone.reset();
            c.dcBlock.reset();
        }
    }

    // Preamp gain staging derived from the switch positions.
    void computeTargets()
    {
        const auto& p = target;
        double gainDb = 0.0; bool off;
        // sPre1: input -> first-amp output; sPre2: (first-amp output | input) -> BA283AM output.
        // Gains put each node at its E/10006 level; the labelled gain is exact at Mic Z HI.
        bool firstAmp = false;
        if (p.micMode)
        {
            off = tables::micOff (p.micIndex);
            gainDb = off ? 0.0 : -tables::micSensitivity (p.micIndex);
            firstAmp = ! off && tables::micUsesFirstAmp (p.micIndex);
            if (firstAmp)
            {
                sPre1.tgt = dbToLin (gainDb + voicing::kStage1NodeDb);              // to -42 dBm
                sPre2.tgt = dbToLin (voicing::kEqNodeDb - voicing::kStage1NodeDb);  // BA283AM +28 dB
            }
            else
            {
                sPre1.tgt = 1.0;
                sPre2.tgt = dbToLin (gainDb + voicing::kEqNodeDb);
            }
            // 10468: LO (300 R) parallels the primaries for 6 dB more step-up than HI, but a
            // 150-ohm mic is loaded harder: net LO vs HI = +6 dB + 20log[(300/450) / (Zhi/(Zhi+150))]
            const double zHi = tables::micImpedance (true, p.micIndex);
            const double loRel = 2.0 * (300.0 / 450.0) / (zHi / (zHi + 150.0));
            sZLoss.tgt = (p.micZHi ? 1.0 : loRel) * (p.pad ? 0.1 : 1.0);   // pad ahead of the transformer
        }
        else
        {
            off = tables::lineOff (p.lineIndex);
            gainDb = off ? 0.0 : -tables::lineSensitivity (p.lineIndex);
            sPre1.tgt = 1.0;
            sPre2.tgt = dbToLin (gainDb + voicing::kEqNodeDb);
            sZLoss.tgt = 1.0;
        }
        if (firstAmp != useFirstAmp)
        {
            // the switch re-routes the signal: jump rather than glide through a wrong gain
            useFirstAmp = firstAmp;
            sPre1.cur = sPre1.tgt; sPre2.cur = sPre2.tgt;
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
            // lower load impedance damps the input transformer's top end (subtle, by design:
            // the datasheet response limits hold at every Mic Z setting)
            const double q = 0.64 + 0.08 * std::min (std::max (z, 300.0), 1200.0) / 1200.0;
            for (auto& s : ch)
            {
                s.txMic.hf.c  = designLowPass (kTxInHf, q, fsOs);
                s.txLine.hf.c = designLowPass (kTxInHf, kTxInQLine, fsOs);
            }
            builtZ = z; builtMicMode = p.micMode;
        }

        // EQ loading of the preamp output: boosts lower stage-2 headroom even with EQL out
        const double boost = std::max (0.0, eqHigh) + (p.midFreqIndex ? std::max (0.0, eqMid) : 0.0)
                           + (p.lowFreqIndex ? std::max (0.0, eqLow) : 0.0);
        eqLoad = 1.0 / (1.0 + 0.25 * boost / 50.0);   // up to ~2 dB less headroom

        eqActive = p.eqIn;
        midOn = p.eqIn && p.midFreqIndex > 0;
        lowOn = p.eqIn && p.lowFreqIndex > 0;
        cutOn = p.eqIn && p.lowCutIndex > 0;

        // BA205 (HF + LF share EQ amp A, so they are solved together and interact)
        // while a gain knob glides, redesign every 4th block (~2.7 ms at 48 kHz); always on arrival
        ++eqUpdateCounter;
        const bool gliding = std::fabs (eqHigh - p.highGainDb) > 0.0 || std::fabs (eqLow - p.lowGainDb) > 0.0 || std::fabs (eqMid - p.midGainDb) > 0.0;
        const bool slot = ! gliding || (eqUpdateCounter & 3) == 0;
        if (force || builtLowF != p.lowFreqIndex
            || (slot && (std::fabs (eqHigh - eqHighA) > 1e-4 || std::fabs (eqLow - eqLowA) > 1e-4)))
        {
            buildWiperMaps();
            const double hfW = hfMap.wiperFor (eqHigh);
            const double lfW = p.lowFreqIndex > 0 ? lfMap[(size_t) p.lowFreqIndex].wiperFor (eqLow) : 0.5;
            ba205Flat = std::fabs (eqHigh) < 1e-6 && (p.lowFreqIndex == 0 || std::fabs (eqLow) < 1e-6);
            const auto cas = circuit::Ba205::design (hfW, lfW, p.lowFreqIndex, fsOs);
            for (auto& st : ch)
                for (int i = 0; i < circuit::Cascade::kMaxSections; ++i)
                    st.ba205[(size_t) i].c = i < cas.n ? cas.s[(size_t) i] : BiquadCoefs {};
            ba205N = cas.n; ba205Gain = cas.gain;
            eqHighA = eqHigh; eqLowA = eqLow; builtLowF = p.lowFreqIndex;
        }
        // BA211 presence
        if (p.midFreqIndex > 0 && (force || builtMidF != p.midFreqIndex || (slot && std::fabs (eqMid - eqMidA) > 1e-4)))
        {
            buildWiperMaps();
            const double w = presMap[(size_t) p.midFreqIndex].wiperFor (eqMid);
            setAll (&ChannelState::presence, circuit::Ba211::design (w, p.midFreqIndex, fsOs));
            builtMidF = p.midFreqIndex; eqMidA = eqMid;
        }
        // BA182 passive high-pass
        if (p.lowCutIndex > 0 && (force || builtLowCut != p.lowCutIndex))
        {
            const auto cas = circuit::Ba182::design (p.lowCutIndex, fsOs);
            for (auto& st : ch)
                for (int i = 0; i < circuit::Cascade::kMaxSections; ++i)
                    st.ba182[(size_t) i].c = i < cas.n ? cas.s[(size_t) i] : BiquadCoefs {};
            ba182N = cas.n; ba182Gain = cas.gain;
            builtLowCut = p.lowCutIndex;
        }
        if (force || ! builtEq)
        {
            // residual tone of the EQ circuitry, present even with EQL out (-0.25 dB @ 20 kHz)
            setAll (&ChannelState::eqTone, designLowPass (150000.0, 0.6, fsOs));
            builtEq = true;
        }
    }

    // One oversampled-rate sample through the analog chain.
    inline double processModel (ChannelState& s, double x, double g1, double g2, double lvl)
    {
        if (target.micMode)
        {
            x = s.txMic.process (x);
            if (useFirstAmp) x = s.pre1.process (x * g1);
        }
        else
            x = s.txLine.process (x);
        x = s.pre2.process (x * g2, eqLoad);                 // BA283AM, node -14 dBm

        if (eqActive)
        {
            if (! ba205Flat)                                  // BA205 HF + LF network
            {
                for (int i = 0; i < ba205N; ++i) x = s.ba205[(size_t) i].process (x);
                x *= ba205Gain;
            }
            x = s.eqAmpA.process (x);                         // EQ amp A
            if (midOn) x = s.presence.process (x);            // BA211 presence
            x = s.eqAmpB.process (x);                         // EQ amp B
            if (cutOn)                                        // BA182 passive HPF, last
            {
                for (int i = 0; i < ba182N; ++i) x = s.ba182[(size_t) i].process (x);
                x *= ba182Gain;
            }
        }
        x = s.eqTone.process (x);

        static const double outGain = dbToLin (kOutStageGain);
        x = s.outAmp.process (x * lvl * outGain);             // level control -> BA283AV
        return s.txOut.process (x);                           // LO1166
    }
};

} // namespace c73
