#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/Channel73Dsp.h"

namespace ParamIDs {
inline constexpr auto inputMic  = "inputMic";
inline constexpr auto micGain   = "micGain";
inline constexpr auto lineGain  = "lineGain";
inline constexpr auto micZHi    = "micZHi";
inline constexpr auto pad       = "pad";
inline constexpr auto highGain  = "highGain";
inline constexpr auto midFreq   = "midFreq";
inline constexpr auto midGain   = "midGain";
inline constexpr auto lowFreq   = "lowFreq";
inline constexpr auto lowGain   = "lowGain";
inline constexpr auto lowCut    = "lowCut";
inline constexpr auto phase     = "phase";
inline constexpr auto eql       = "eql";
inline constexpr auto level     = "level";
inline constexpr auto output    = "output";
inline constexpr auto power     = "power";
inline constexpr auto quality   = "quality";
}

namespace Labels {
const juce::StringArray mic     { "OFF", "-20", "-25", "-30", "-35", "-40", "-45", "-50", "-55", "-60", "-65", "-70", "-75", "-80" };
const juce::StringArray line    { "-20", "-15", "-10", "-5", "0", "+5", "+10", "OFF" };
const juce::StringArray midFreq { "OFF", "360", "700", "1.6k", "3.2k", "4.8k", "7.2k" };
const juce::StringArray lowFreq { "OFF", "35", "60", "110", "220" };
const juce::StringArray lowCut  { "OFF", "50", "80", "160", "300" };
const juce::StringArray quality { "1x  (0 samples)", "2x  (3 samples)", "4x  (4 samples)" };
}

class Channel73Processor : public juce::AudioProcessor
{
public:
    Channel73Processor();
    ~Channel73Processor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    c73::Params readParams (bool forceBypass) const;
    void run (juce::AudioBuffer<float>& buffer, bool forceBypass);

    c73::Channel73Dsp dsp;
    int currentProgram = 0;
    int reportedLatency = -1;

    struct Raw
    {
        std::atomic<float>* inputMic, *micGain, *lineGain, *micZHi, *pad, *highGain, *midFreq, *midGain,
                          *lowFreq, *lowGain, *lowCut, *phase, *eql, *level, *output, *power, *quality;
    } raw {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Channel73Processor)
};
