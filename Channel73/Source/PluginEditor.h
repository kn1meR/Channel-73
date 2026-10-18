#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class C73LookAndFeel : public juce::LookAndFeel_V4
{
public:
    C73LookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
};

// Fixed-size logical panel (400 x 920); the editor scales it.
class C73Panel : public juce::Component, private juce::Timer
{
public:
    explicit C73Panel (Channel73Processor&);
    ~C73Panel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int kW = 400, kH = 920;

private:
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct ScaleLabel { juce::Rectangle<float> area; juce::String text; const char* paramId; float value; };

    void timerCallback() override;
    void setupKnob (juce::Slider&, const char* style, const char* tip);
    void setupToggle (juce::TextButton&, const juce::String& text, const char* tip);
    void setParam (const char* id, float value);
    float getParam (const char* id) const;
    void addScale (juce::Graphics&, juce::Rectangle<int> knob, float radiusPad, const juce::StringArray& labels,
                   const char* paramId, bool clickable, float fontSize = 11.0f);
    void drawSectionTitle (juce::Graphics&, juce::Rectangle<int>, const juce::String&);

    Channel73Processor& proc;
    C73LookAndFeel lnf;

    juce::Slider micGain, lineGain, highGain, midFreq, midGain, lowFreq, lowGain, lowCut, level, output;
    juce::TextButton micBtn { "MIC" }, lineBtn { "LINE" }, zBtn, padBtn, phaseBtn, eqlBtn, powerBtn;
    juce::ComboBox quality, presets;

    std::unique_ptr<SA> aMic, aLine, aHigh, aMidF, aMidG, aLowF, aLowG, aCut, aLevel, aOut;
    std::unique_ptr<BA> aZ, aPad, aPhase, aEql, aPower;
    std::unique_ptr<CA> aQuality;

    std::vector<ScaleLabel> scaleLabels;
    juce::TooltipWindow tooltips { this, 700 };
    bool lastMic = false;
    int lastProgram = -1;
};

class Channel73Editor : public juce::AudioProcessorEditor
{
public:
    explicit Channel73Editor (Channel73Processor&);
    void resized() override;
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff1b1d21)); }

private:
    C73Panel panel;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Channel73Editor)
};
