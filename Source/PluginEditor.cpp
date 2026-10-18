#include "PluginEditor.h"

namespace {
using juce::Colour;
const Colour kPanelTop   (0xff2b2f35);
const Colour kPanelBot   (0xff1f2226);
const Colour kInk        (0xffe9e1cf);   // cream legend
const Colour kInkDim     (0xff9d978a);
const Colour kAccent     (0xffc8553d);   // brick red
const Colour kLedOn      (0xffffb347);
const Colour kLedOff     (0xff4a3a2a);

constexpr float kStart = juce::MathConstants<float>::pi * 1.25f;
constexpr float kEnd   = juce::MathConstants<float>::pi * 2.75f;

juce::Point<float> polar (juce::Point<float> c, float r, float a)
{
    return { c.x + r * std::sin (a), c.y - r * std::cos (a) };
}

juce::Font font (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

// Layout (logical 400 x 920 panel)
const juce::Rectangle<int> rRed      { 128, 118, 144, 144 };
const juce::Rectangle<int> rHigh     {  52, 388,  76,  76 };
const juce::Rectangle<int> rCut      { 272, 388,  76,  76 };
const juce::Rectangle<int> rMidRing  {  49, 528, 112, 112 };
const juce::Rectangle<int> rLowRing  { 239, 528, 112, 112 };
const juce::Rectangle<int> rLevel    {  52, 752,  76,  76 };
const juce::Rectangle<int> rOut      { 272, 752,  76,  76 };

juce::Rectangle<int> innerOf (juce::Rectangle<int> ring) { return ring.withSizeKeepingCentre (58, 58); }
} // namespace

//==============================================================================
C73LookAndFeel::C73LookAndFeel()
{
    setColour (juce::ComboBox::backgroundColourId, Colour (0xff17191c));
    setColour (juce::ComboBox::outlineColourId, Colour (0xff4a4f57));
    setColour (juce::ComboBox::textColourId, kInk);
    setColour (juce::ComboBox::arrowColourId, kInkDim);
    setColour (juce::PopupMenu::backgroundColourId, Colour (0xff17191c));
    setColour (juce::PopupMenu::textColourId, kInk);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kAccent.darker (0.3f));
    setColour (juce::BubbleComponent::backgroundColourId, Colour (0xf017191c));
    setColour (juce::BubbleComponent::outlineColourId, kAccent);
    setColour (juce::TooltipWindow::backgroundColourId, Colour (0xf017191c));
    setColour (juce::TooltipWindow::textColourId, kInk);
    setColour (juce::TooltipWindow::outlineColourId, Colour (0xff4a4f57));
}

void C73LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                       float startAngle, float endAngle, juce::Slider& s)
{
    const auto style = s.getProperties()["style"].toString();
    const auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
    const auto c = b.getCentre();
    const float R = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float alpha = s.isEnabled() ? 1.0f : 0.45f;

    auto shadow = [&] (float radius) {
        g.setColour (juce::Colours::black.withAlpha (0.45f * alpha));
        g.fillEllipse (juce::Rectangle<float> (radius * 2, radius * 2).withCentre (c.translated (0, 3)));
    };
    auto pointer = [&] (float r0, float r1, float thick, Colour col) {
        juce::Path p;
        p.startNewSubPath (polar (c, r0, angle));
        p.lineTo (polar (c, r1, angle));
        g.setColour (col.withMultipliedAlpha (alpha));
        g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    auto knurl = [&] (float radius, int n, Colour col) {
        g.setColour (col.withMultipliedAlpha (alpha));
        for (int i = 0; i < n; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) n;
            g.drawLine (juce::Line<float> (polar (c, radius - 3.5f, a), polar (c, radius - 0.5f, a)), 1.0f);
        }
    };

    if (style == "red")
    {
        shadow (R);
        // skirt
        g.setGradientFill (juce::ColourGradient (Colour (0xff3a3d42), c.x, c.y - R, Colour (0xff15171a), c.x, c.y + R, false));
        g.fillEllipse (b.withSizeKeepingCentre (R * 2, R * 2));
        for (int i = 0; i <= 26; ++i)
        {
            const float a = startAngle + (endAngle - startAngle) * (float) i / 26.0f;
            g.setColour (kInkDim.withAlpha (0.35f * alpha));
            g.drawLine (juce::Line<float> (polar (c, R - 6, a), polar (c, R - 2, a)), 1.0f);
        }
        // cap
        const float r = R * 0.72f;
        g.setGradientFill (juce::ColourGradient (Colour (0xffd2533f).withMultipliedAlpha (alpha), c.x - r * 0.4f, c.y - r * 0.6f,
                                                 Colour (0xff6b1a14).withMultipliedAlpha (alpha), c.x + r * 0.5f, c.y + r, true));
        g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (c));
        knurl (r, 40, Colour (0x55000000));
        g.setColour (Colour (0x30ffffff));
        g.drawEllipse (juce::Rectangle<float> (r * 2 - 2, r * 2 - 2).withCentre (c), 1.0f);
        pointer (r * 0.25f, R - 3, 3.5f, kInk);
    }
    else if (style == "ring")
    {
        shadow (R);
        g.setGradientFill (juce::ColourGradient (Colour (0xff545a63).withMultipliedAlpha (alpha), c.x, c.y - R,
                                                 Colour (0xff23262b).withMultipliedAlpha (alpha), c.x, c.y + R, false));
        g.fillEllipse (b.withSizeKeepingCentre (R * 2, R * 2));
        knurl (R, 48, Colour (0x66000000));
        g.setColour (Colour (0x22ffffff));
        g.drawEllipse (b.withSizeKeepingCentre (R * 2 - 1, R * 2 - 1), 1.0f);
        // pointer notch on the ring's edge
        pointer (R - 13, R - 3, 4.0f, kInk);
    }
    else // "alu", "inner", "innerLow"
    {
        const bool inner = style.startsWith ("inner");
        if (! inner) shadow (R);
        Colour top = Colour (0xffd9dcdf), bot = Colour (0xff7d8288), ptr = Colour (0xff1b1d21);
        if (style == "inner")    { top = Colour (0xffeee5cf); bot = Colour (0xffa69a7c); }
        if (style == "innerLow") { top = Colour (0xffd99a63); bot = Colour (0xff7a4524); ptr = kInk; }
        if (inner) { g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillEllipse (b.expanded (2.0f).translated (0, 2)); }
        g.setGradientFill (juce::ColourGradient (top.withMultipliedAlpha (alpha), c.x - R * 0.5f, c.y - R * 0.7f,
                                                 bot.withMultipliedAlpha (alpha), c.x + R * 0.5f, c.y + R, true));
        g.fillEllipse (b.withSizeKeepingCentre (R * 2, R * 2));
        knurl (R, inner ? 28 : 36, Colour (0x40000000));
        g.setColour (Colour (0x40ffffff));
        g.drawEllipse (b.withSizeKeepingCentre (R * 2 - 2, R * 2 - 2), 1.0f);
        pointer (R * 0.2f, R - 4, inner ? 3.0f : 3.5f, ptr);
    }
}

void C73LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& btn, const Colour&, bool over, bool down)
{
    auto b = btn.getLocalBounds().toFloat().reduced (1.0f);
    const float alpha = btn.isEnabled() ? 1.0f : 0.35f;
    g.setColour (juce::Colours::black.withAlpha (0.5f * alpha));
    g.fillRoundedRectangle (b.translated (0, 2), 4.0f);
    g.setGradientFill (juce::ColourGradient (Colour (down ? 0xff2b2e33 : 0xff4b5058).withMultipliedAlpha (alpha), 0, b.getY(),
                                             Colour (0xff25282d).withMultipliedAlpha (alpha), 0, b.getBottom(), false));
    g.fillRoundedRectangle (b, 4.0f);
    g.setColour ((over ? kAccent.withAlpha (0.7f) : Colour (0xff15171a)).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (b, 4.0f, 1.0f);
    // LED
    const auto led = juce::Rectangle<float> (7, 7).withCentre ({ b.getX() + 10, b.getCentreY() });
    const bool on = btn.getToggleState();
    if (on) { g.setColour (kLedOn.withAlpha (0.35f * alpha)); g.fillEllipse (led.expanded (3)); }
    g.setColour ((on ? kLedOn : kLedOff).withMultipliedAlpha (alpha));
    g.fillEllipse (led);
}

void C73LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& btn, bool, bool)
{
    g.setFont (font (11.5f, true));
    g.setColour ((btn.getToggleState() ? kInk : kInkDim).withMultipliedAlpha (btn.isEnabled() ? 1.0f : 0.4f));
    g.drawText (btn.getButtonText(), btn.getLocalBounds().withTrimmedLeft (16), juce::Justification::centred);
}

juce::Font C73LookAndFeel::getComboBoxFont (juce::ComboBox&) { return font (12.5f); }
juce::Font C73LookAndFeel::getPopupMenuFont() { return font (13.5f); }

//==============================================================================
C73Panel::C73Panel (Channel73Processor& p) : proc (p)
{
    setLookAndFeel (&lnf);
    auto& st = proc.apvts;

    setupKnob (micGain,  "red",      "Mic gain (sensitivity in dB). Clockwise = more gain. OFF mutes.");
    setupKnob (lineGain, "red",      "Line gain (sensitivity in dB). Counter-clockwise = more gain. OFF mutes.");
    setupKnob (highGain, "alu",      "High shelf at 12 kHz, +/-16 dB. Double-click for 0.");
    setupKnob (lowCut,   "alu",      "Low cut (18 dB/octave): OFF, 50, 80, 160, 300 Hz");
    setupKnob (midFreq,  "ring",     "Mid frequency (outer ring): OFF, 360, 700, 1.6k, 3.2k, 4.8k, 7.2k");
    setupKnob (midGain,  "inner",    "Mid gain (inner knob), +/-18 dB. Double-click for 0.");
    setupKnob (lowFreq,  "ring",     "Low shelf frequency (outer ring): OFF, 35, 60, 110, 220 Hz");
    setupKnob (lowGain,  "innerLow", "Low shelf gain (inner knob), +/-16 dB. Double-click for 0.");
    setupKnob (level,    "alu",      "Level: output-stage fader. Above 0 dB drives the output amplifier.");
    setupKnob (output,   "alu",      "Output: clean trim after the model (+/-24 dB). Double-click for 0.");

    aMic   = std::make_unique<SA> (st, ParamIDs::micGain, micGain);
    aLine  = std::make_unique<SA> (st, ParamIDs::lineGain, lineGain);
    aHigh  = std::make_unique<SA> (st, ParamIDs::highGain, highGain);
    aCut   = std::make_unique<SA> (st, ParamIDs::lowCut, lowCut);
    aMidF  = std::make_unique<SA> (st, ParamIDs::midFreq, midFreq);
    aMidG  = std::make_unique<SA> (st, ParamIDs::midGain, midGain);
    aLowF  = std::make_unique<SA> (st, ParamIDs::lowFreq, lowFreq);
    aLowG  = std::make_unique<SA> (st, ParamIDs::lowGain, lowGain);
    aLevel = std::make_unique<SA> (st, ParamIDs::level, level);
    aOut   = std::make_unique<SA> (st, ParamIDs::output, output);

    for (auto* s : { &highGain, &midGain, &lowGain, &level, &output })
        s->setDoubleClickReturnValue (true, 0.0);

    // Rings first so the inner knobs sit on top and receive clicks in the centre
    micGain.setBounds (rRed);   lineGain.setBounds (rRed);
    highGain.setBounds (rHigh); lowCut.setBounds (rCut);
    midFreq.setBounds (rMidRing); midGain.setBounds (innerOf (rMidRing));
    lowFreq.setBounds (rLowRing); lowGain.setBounds (innerOf (rLowRing));
    level.setBounds (rLevel);   output.setBounds (rOut);
    midGain.toFront (false); lowGain.toFront (false);

    for (auto* b : { &micBtn, &lineBtn })
    {
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    micBtn.setTooltip ("Mic input: transformer + two class-A stages, 20..80 dB of gain");
    lineBtn.setTooltip ("Line input: -10..+20 dB of gain");
    micBtn.onClick  = [this] { setParam (ParamIDs::inputMic, 1.0f); };
    lineBtn.onClick = [this] { setParam (ParamIDs::inputMic, 0.0f); };

    setupToggle (zBtn,     "Z HI",  "Mic input impedance. HI: 1.2k (-20..-50) / 600 ohm (-55..-80). LO: 300 ohm with 6 dB more transformer step-up (louder, more loading; e.g. ribbon mics).");
    setupToggle (padBtn,   "PAD",   "-20 dB mic pad");
    setupToggle (phaseBtn, "PHASE", "Polarity invert");
    setupToggle (eqlBtn,   "EQL",   "EQ in/out. The EQ circuit still colours and loads the preamp when out.");
    setupToggle (powerBtn, "POWER", "Power off = bypass (dry signal, latency-aligned)");
    aZ     = std::make_unique<BA> (st, ParamIDs::micZHi, zBtn);
    aPad   = std::make_unique<BA> (st, ParamIDs::pad, padBtn);
    aPhase = std::make_unique<BA> (st, ParamIDs::phase, phaseBtn);
    aEql   = std::make_unique<BA> (st, ParamIDs::eql, eqlBtn);
    aPower = std::make_unique<BA> (st, ParamIDs::power, powerBtn);

    quality.addItemList (Labels::quality, 1);
    quality.setTooltip ("Oversampling. Latency is reported to the host. 1x = zero latency; 2x/4x reduce aliasing when driven hard.");
    addAndMakeVisible (quality);
    aQuality = std::make_unique<CA> (st, ParamIDs::quality, quality);

    for (int i = 0; i < proc.getNumPrograms(); ++i) presets.addItem (proc.getProgramName (i), i + 1);
    presets.setTextWhenNothingSelected ("Presets");
    presets.onChange = [this] {
        const int id = presets.getSelectedId();
        if (id > 0) { proc.setCurrentProgram (id - 1); lastProgram = id - 1; }
    };
    addAndMakeVisible (presets);

    timerCallback();
    startTimerHz (20);
}

C73Panel::~C73Panel()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void C73Panel::setupKnob (juce::Slider& s, const char* style, const char* tip)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    s.setRotaryParameters (kStart, kEnd, true);
    s.setPopupDisplayEnabled (true, true, this, 1200);
    s.getProperties().set ("style", style);
    s.setTooltip (tip);
    s.setMouseDragSensitivity (180);
    addAndMakeVisible (s);
}

void C73Panel::setupToggle (juce::TextButton& b, const juce::String& text, const char* tip)
{
    b.setButtonText (text);
    b.setClickingTogglesState (true);
    b.setTooltip (tip);
    addAndMakeVisible (b);
}

void C73Panel::setParam (const char* id, float value)
{
    if (auto* p = proc.apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}

float C73Panel::getParam (const char* id) const
{
    return proc.apvts.getRawParameterValue (id)->load();
}

void C73Panel::resized()
{
    presets.setBounds (236, 30, 148, 24);
    micBtn.setBounds  (22, 288, 66, 28);
    lineBtn.setBounds (92, 288, 66, 28);
    zBtn.setBounds    (174, 288, 66, 28);
    padBtn.setBounds  (244, 288, 62, 28);
    phaseBtn.setBounds (310, 288, 72, 28);
    eqlBtn.setBounds  (162, 412, 76, 30);
    powerBtn.setBounds (158, 775, 84, 30);
    quality.setBounds (150, 876, 210, 24);
}

void C73Panel::timerCallback()
{
    const bool mic = getParam (ParamIDs::inputMic) > 0.5f;
    micGain.setVisible (mic);
    lineGain.setVisible (! mic);
    micBtn.setToggleState (mic, juce::dontSendNotification);
    lineBtn.setToggleState (! mic, juce::dontSendNotification);
    // Mic Z and Pad cannot be switched on the line input (as in the manual)
    zBtn.setEnabled (mic);
    padBtn.setEnabled (mic);
    zBtn.setButtonText (getParam (ParamIDs::micZHi) > 0.5f ? "Z HI" : "Z LO");

    const bool eqOn = getParam (ParamIDs::eql) > 0.5f;
    for (auto* s : { &highGain, &midFreq, &midGain, &lowFreq, &lowGain, &lowCut })
        s->setAlpha (eqOn ? 1.0f : 0.8f);

    if (proc.getCurrentProgram() != lastProgram)
    {
        lastProgram = proc.getCurrentProgram();
        presets.setSelectedId (lastProgram + 1, juce::dontSendNotification);
    }
    if (mic != lastMic) lastMic = mic;
    repaint();
}

void C73Panel::drawSectionTitle (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& t)
{
    g.setFont (font (11.0f, true));
    const int tw = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), t) + 16;
    g.setColour (kInkDim.withAlpha (0.5f));
    const float y = (float) r.getCentreY();
    g.drawLine ((float) r.getX(), y, (float) (r.getCentreX() - tw / 2), y, 1.0f);
    g.drawLine ((float) (r.getCentreX() + tw / 2), y, (float) r.getRight(), y, 1.0f);
    g.setColour (kInkDim);
    g.drawText (t, r, juce::Justification::centred);
}

void C73Panel::addScale (juce::Graphics& g, juce::Rectangle<int> knob, float radiusPad, const juce::StringArray& labels,
                         const char* paramId, bool clickable, float fontSize)
{
    auto* prm = proc.apvts.getParameter (paramId);
    const auto c = knob.toFloat().getCentre();
    const float r = (float) knob.getWidth() * 0.5f + radiusPad;
    g.setFont (font (fontSize, true));
    const float current = getParam (paramId);

    for (int i = 0; i < labels.size(); ++i)
    {
        // label text "value|caption" lets continuous knobs place captions at real values
        auto text = labels[i];
        float value = (float) i;
        if (text.containsChar ('|')) { value = text.upToFirstOccurrenceOf ("|", false, false).getFloatValue(); text = text.fromFirstOccurrenceOf ("|", false, false); }
        const float a = kStart + prm->convertTo0to1 (value) * (kEnd - kStart);
        const auto pt = polar (c, r, a);
        const float tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 4.0f;
        const auto area = juce::Rectangle<float> (tw, fontSize + 4).withCentre (pt);
        const bool active = std::abs (current - value) < 0.05f;
        g.setColour (active ? kInk : kInkDim);
        g.drawText (text, area, juce::Justification::centred);
        // tick
        g.setColour (kInkDim.withAlpha (0.45f));
        g.drawLine (juce::Line<float> (polar (c, (float) knob.getWidth() * 0.5f + 1, a), polar (c, (float) knob.getWidth() * 0.5f + 5, a)), 1.0f);
        if (clickable) scaleLabels.push_back ({ area.expanded (2), text, paramId, value });
    }
}

void C73Panel::paint (juce::Graphics& g)
{
    scaleLabels.clear();
    const auto bounds = getLocalBounds().toFloat();

    // panel
    g.setGradientFill (juce::ColourGradient (kPanelTop, 0, 0, kPanelBot, 0, (float) kH, false));
    g.fillRect (bounds);
    juce::Random rng (73);
    for (int i = 0; i < 260; ++i)   // faint brushed texture
    {
        const float y = rng.nextFloat() * (float) kH;
        g.setColour (juce::Colours::white.withAlpha (0.012f + 0.012f * rng.nextFloat()));
        g.drawHorizontalLine ((int) y, 0.0f, (float) kW);
    }
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRect (bounds, 2.0f);
    for (auto pt : { juce::Point<float> (14, 14), { (float) kW - 14, 14 }, { 14, (float) kH - 14 }, { (float) kW - 14, (float) kH - 14 } })
    {
        g.setGradientFill (juce::ColourGradient (Colour (0xffa0a4a8), pt.x - 4, pt.y - 4, Colour (0xff3a3d42), pt.x + 4, pt.y + 4, false));
        g.fillEllipse (juce::Rectangle<float> (10, 10).withCentre (pt));
        g.setColour (Colour (0xff202226));
        g.drawLine (pt.x - 3.5f, pt.y + 2, pt.x + 3.5f, pt.y - 2, 1.4f);
    }

    // header
    g.setColour (kInk);
    g.setFont (font (26.0f, true));
    g.drawText ("CHANNEL 73", 26, 22, 220, 30, juce::Justification::left);
    g.setFont (font (10.0f));
    g.setColour (kInkDim);
    g.drawText ("CLASS-A MIC / LINE AMPLIFIER  |  3-BAND EQ", 27, 52, 300, 14, juce::Justification::left);
    g.setColour (kAccent);
    g.fillRect (26, 70, 348, 2);

    // INPUT
    drawSectionTitle (g, { 22, 80, 356, 16 }, "INPUT");
    const bool mic = getParam (ParamIDs::inputMic) > 0.5f;
    if (mic) addScale (g, rRed, 17.0f, Labels::mic, ParamIDs::micGain, true, 10.5f);
    else     addScale (g, rRed, 20.0f, Labels::line, ParamIDs::lineGain, true, 11.5f);
    g.setFont (font (10.0f, true));
    g.setColour (kAccent.brighter (0.2f));
    g.drawText (mic ? "MIC  dB" : "LINE  dB", rRed.getX(), rRed.getBottom() + 3, rRed.getWidth(), 14, juce::Justification::centred);

    // EQUALISER
    drawSectionTitle (g, { 22, 344, 356, 16 }, "EQUALISER");
    addScale (g, rHigh, 14.0f, { "-16|-16", "-8|-8", "0|0", "8|+8", "16|+16" }, ParamIDs::highGain, true, 10.0f);
    addScale (g, rCut, 14.0f, Labels::lowCut, ParamIDs::lowCut, true, 10.5f);
    addScale (g, rMidRing, 14.0f, Labels::midFreq, ParamIDs::midFreq, true, 10.5f);
    addScale (g, rLowRing, 14.0f, Labels::lowFreq, ParamIDs::lowFreq, true, 10.5f);

    g.setFont (font (11.0f, true));
    g.setColour (kInk);
    g.drawText ("HIGH  12 kHz", rHigh.getX() - 20, rHigh.getBottom() + 20, rHigh.getWidth() + 40, 14, juce::Justification::centred);
    g.drawText ("LOW CUT  Hz", rCut.getX() - 20, rCut.getBottom() + 20, rCut.getWidth() + 40, 14, juce::Justification::centred);

    auto gainReadout = [&] (juce::Rectangle<int> ring, const char* title, const char* gainId) {
        const float v = getParam (gainId);
        const auto area = juce::Rectangle<int> (ring.getX() - 20, ring.getBottom() + 22, ring.getWidth() + 40, 14);
        g.setColour (kInk);
        g.setFont (font (11.0f, true));
        g.drawText (juce::String (title) + "  " + (v > 0 ? "+" : "") + juce::String (v, 1) + " dB", area, juce::Justification::centred);
        scaleLabels.push_back ({ area.toFloat(), "0", gainId, 0.0f }); // click readout -> 0 dB
    };
    gainReadout (rMidRing, "MID", ParamIDs::midGain);
    gainReadout (rLowRing, "LOW", ParamIDs::lowGain);
    g.setFont (font (9.5f));
    g.setColour (kInkDim);
    g.drawText ("outer ring: kHz  |  centre: gain", rMidRing.getX() - 30, rMidRing.getBottom() + 38, rMidRing.getWidth() + 60, 12, juce::Justification::centred);
    g.drawText ("outer ring: Hz  |  centre: gain", rLowRing.getX() - 30, rLowRing.getBottom() + 38, rLowRing.getWidth() + 60, 12, juce::Justification::centred);

    // OUTPUT
    drawSectionTitle (g, { 22, 704, 356, 16 }, "OUTPUT");
    addScale (g, rLevel, 14.0f, { "-60|-inf", "-20|-20", "-6|-6", "0|0", "10|+10" }, ParamIDs::level, true, 10.0f);
    addScale (g, rOut, 14.0f, { "-24|-24", "-12|-12", "0|0", "12|+12", "24|+24" }, ParamIDs::output, true, 10.0f);
    g.setFont (font (11.0f, true));
    g.setColour (kInk);
    g.drawText ("LEVEL", rLevel.getX() - 20, rLevel.getBottom() + 22, rLevel.getWidth() + 40, 14, juce::Justification::centred);
    g.drawText ("OUTPUT", rOut.getX() - 20, rOut.getBottom() + 22, rOut.getWidth() + 40, 14, juce::Justification::centred);
    g.setFont (font (10.0f, true));
    g.setColour (kInkDim);
    g.drawText ("OVERSAMPLING", 40, 876, 106, 24, juce::Justification::centredLeft);
}

void C73Panel::mouseDown (const juce::MouseEvent& e)
{
    // Click a printed value to jump there (as on the original plug-in's legends)
    for (const auto& l : scaleLabels)
        if (l.area.contains (e.position))
        {
            if (std::string (l.paramId) == ParamIDs::micGain && getParam (ParamIDs::inputMic) < 0.5f) continue;
            setParam (l.paramId, l.value);
            return;
        }
}

//==============================================================================
Channel73Editor::Channel73Editor (Channel73Processor& p) : AudioProcessorEditor (p), panel (p)
{
    addAndMakeVisible (panel);
    setResizable (true, true);
    setResizeLimits (240, 552, 640, 1472);
    getConstrainer()->setFixedAspectRatio ((double) C73Panel::kW / (double) C73Panel::kH);
    setSize (340, 782);
}

void Channel73Editor::resized()
{
    const float s = (float) getWidth() / (float) C73Panel::kW;
    panel.setBounds (0, 0, C73Panel::kW, C73Panel::kH);
    panel.setTransform (juce::AffineTransform::scale (s));
}
