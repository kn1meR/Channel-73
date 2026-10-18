#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {

using APVTS = juce::AudioProcessorValueTreeState;

juce::String dbText (float v, int decimals = 1)
{
    return (v > 0 ? "+" : "") + juce::String (v, decimals) + " dB";
}

struct Preset
{
    const char* name;
    bool mic; int micIdx, lineIdx; bool zHi, pad;
    float high; int midF; float mid; int lowF; float low; int cut;
    bool eq; float level, out; int quality;
};

// Original starting points (not reproductions of anyone's presets).
const Preset kPresets[] = {
    { "Default (clean line)",       false, 7, 4, true,  false,  0, 0,  0, 0,  0, 0, false, 0,   0, 1 },
    { "Tracking - dynamic mic",     true,  7, 4, true,  false,  0, 0,  0, 0,  0, 1, false, 0,   0, 0 },
    { "Tracking - ribbon mic (Z LO)", true, 9, 4, false, false,  2, 0,  0, 0,  0, 1, true,  0,   0, 0 },
    { "Vocal sheen",                false, 7, 3, true,  false,  4, 4,  2, 0,  0, 2, true,  0,  -5, 1 },
    { "Acoustic air",               false, 7, 4, true,  false,  5, 1, -3, 0,  0, 2, true,  0,   0, 1 },
    { "Kick weight",                false, 7, 3, true,  false,  0, 1, -4, 2,  5, 0, true,  0,  -5, 1 },
    { "Snare crack",                false, 7, 2, true,  false,  2, 5,  4, 3,  2, 2, true,  0, -10, 1 },
    { "Bass DI grit",               false, 7, 0, true,  false,  0, 2,  3, 3,  3, 1, true,  0, -19, 2 },
    { "Drum bus drive",             false, 7, 1, true,  false,  3, 2, -2, 2,  3, 0, true,  0, -15, 2 },
    { "Output stage push",          false, 7, 4, true,  false,  0, 0,  0, 0,  0, 0, false, 8,  -8, 2 },
};

} // namespace

//==============================================================================
APVTS::ParameterLayout Channel73Processor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    // Two-position switches are 2-step choice parameters: their normalised value is always
    // exactly 0 or 1, so hosts and state restore never see an in-between value.
    auto twoState = [] (const char* id, const char* name, const char* off, const char* on, bool def) {
        return std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, StringArray { off, on }, def ? 1 : 0);
    };
    auto dbAttr = [] (int decimals) {
        return AudioParameterFloatAttributes().withLabel ("dB")
            .withStringFromValueFunction ([=] (float v, int) { return dbText (v, decimals); });
    };

    p.push_back (twoState (ParamIDs::inputMic, "Input Select", "LINE", "MIC", false));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::micGain, 1 },  "Mic Gain",  Labels::mic, 7));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::lineGain, 1 }, "Line Gain", Labels::line, 4));
    p.push_back (twoState (ParamIDs::micZHi, "Mic Z", "LO", "HI", true));
    p.push_back (twoState (ParamIDs::pad, "Pad", "OFF", "ON", false));

    p.push_back (std::make_unique<AudioParameterFloat>  (ParameterID { ParamIDs::highGain, 1 }, "High 12k", NormalisableRange<float> (-16.f, 16.f, 0.1f), 0.f, dbAttr (1)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::midFreq, 1 },  "Mid Freq", Labels::midFreq, 0));
    p.push_back (std::make_unique<AudioParameterFloat>  (ParameterID { ParamIDs::midGain, 1 },  "Mid Gain", NormalisableRange<float> (-18.f, 18.f, 0.1f), 0.f, dbAttr (1)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::lowFreq, 1 },  "Low Freq", Labels::lowFreq, 0));
    p.push_back (std::make_unique<AudioParameterFloat>  (ParameterID { ParamIDs::lowGain, 1 },  "Low Gain", NormalisableRange<float> (-16.f, 16.f, 0.1f), 0.f, dbAttr (1)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::lowCut, 1 },   "Low Cut", Labels::lowCut, 0));
    p.push_back (twoState (ParamIDs::phase, "Phase", "NORMAL", "INVERT", false));
    p.push_back (twoState (ParamIDs::eql, "EQL", "OUT", "IN", false));

    NormalisableRange<float> levelRange (-60.f, 10.f, 0.1f);
    levelRange.setSkewForCentre (-6.f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::level, 1 }, "Level", levelRange, 0.f,
        AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction ([] (float v, int) {
            return v <= -59.95f ? String ("-inf dB") : dbText (v); })));
    p.push_back (std::make_unique<AudioParameterFloat>  (ParameterID { ParamIDs::output, 1 },  "Output", NormalisableRange<float> (-24.f, 24.f, 0.1f), 0.f, dbAttr (1)));
    p.push_back (twoState (ParamIDs::power, "Power", "OFF", "ON", true));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::quality, 1 }, "Oversampling", Labels::quality, 1));

    return { p.begin(), p.end() };
}

Channel73Processor::Channel73Processor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Channel73", createLayout())
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    raw = { get (ParamIDs::inputMic), get (ParamIDs::micGain), get (ParamIDs::lineGain), get (ParamIDs::micZHi),
            get (ParamIDs::pad), get (ParamIDs::highGain), get (ParamIDs::midFreq), get (ParamIDs::midGain),
            get (ParamIDs::lowFreq), get (ParamIDs::lowGain), get (ParamIDs::lowCut), get (ParamIDs::phase),
            get (ParamIDs::eql), get (ParamIDs::level), get (ParamIDs::output), get (ParamIDs::power), get (ParamIDs::quality) };

    reportedLatency = c73::Channel73Dsp::latencyForOversampling ((int) raw.quality->load());
    setLatencySamples (reportedLatency);
}

bool Channel73Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet() == out;
}

c73::Params Channel73Processor::readParams (bool forceBypass) const
{
    c73::Params p;
    p.micMode      = raw.inputMic->load() > 0.5f;
    p.micIndex     = (int) raw.micGain->load();
    p.lineIndex    = (int) raw.lineGain->load();
    p.micZHi       = raw.micZHi->load() > 0.5f;
    p.pad          = raw.pad->load() > 0.5f;
    p.highGainDb   = raw.highGain->load();
    p.midFreqIndex = (int) raw.midFreq->load();
    p.midGainDb    = raw.midGain->load();
    p.lowFreqIndex = (int) raw.lowFreq->load();
    p.lowGainDb    = raw.lowGain->load();
    p.lowCutIndex  = (int) raw.lowCut->load();
    p.phaseInvert  = raw.phase->load() > 0.5f;
    p.eqIn         = raw.eql->load() > 0.5f;
    p.levelDb      = raw.level->load();
    p.outputDb     = raw.output->load();
    p.power        = ! forceBypass && raw.power->load() > 0.5f;
    p.oversampling = (int) raw.quality->load();
    return p;
}

void Channel73Processor::prepareToPlay (double sampleRate, int)
{
    dsp.setParams (readParams (false));
    dsp.prepare (sampleRate, juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    reportedLatency = dsp.getLatencySamples();
    setLatencySamples (reportedLatency);
}

void Channel73Processor::run (juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    juce::ScopedNoDenormals noDenormals;
    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    dsp.setParams (readParams (forceBypass));
    const int lat = dsp.getLatencySamples();
    if (lat != reportedLatency)
    {
        reportedLatency = lat;
        setLatencySamples (lat); // host is notified asynchronously
    }
    dsp.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

void Channel73Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, false);
}

// Host bypass: keep the dry signal delayed by the reported latency so nothing shifts in time.
void Channel73Processor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, true);
}

//==============================================================================
int Channel73Processor::getNumPrograms() { return (int) std::size (kPresets); }

const juce::String Channel73Processor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, getNumPrograms()) ? juce::String (kPresets[index].name) : juce::String();
}

void Channel73Processor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPrograms())) return;
    currentProgram = index;
    const auto& pr = kPresets[index];
    auto set = [this] (const char* id, float value) {
        if (auto* prm = apvts.getParameter (id))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (value));
            prm->endChangeGesture();
        }
    };
    set (ParamIDs::inputMic, pr.mic ? 1.f : 0.f);
    set (ParamIDs::micGain, (float) pr.micIdx);
    set (ParamIDs::lineGain, (float) pr.lineIdx);
    set (ParamIDs::micZHi, pr.zHi ? 1.f : 0.f);
    set (ParamIDs::pad, pr.pad ? 1.f : 0.f);
    set (ParamIDs::highGain, pr.high);
    set (ParamIDs::midFreq, (float) pr.midF);
    set (ParamIDs::midGain, pr.mid);
    set (ParamIDs::lowFreq, (float) pr.lowF);
    set (ParamIDs::lowGain, pr.low);
    set (ParamIDs::lowCut, (float) pr.cut);
    set (ParamIDs::phase, 0.f);
    set (ParamIDs::eql, pr.eq ? 1.f : 0.f);
    set (ParamIDs::level, pr.level);
    set (ParamIDs::output, pr.out);
    set (ParamIDs::power, 1.f);
    set (ParamIDs::quality, (float) pr.quality);
}

void Channel73Processor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("program", currentProgram, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void Channel73Processor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentProgram = tree.getProperty ("program", 0);
            apvts.replaceState (tree);
        }
}

juce::AudioProcessorEditor* Channel73Processor::createEditor() { return new Channel73Editor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Channel73Processor(); }
