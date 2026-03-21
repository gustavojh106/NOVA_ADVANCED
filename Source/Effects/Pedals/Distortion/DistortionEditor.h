#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class DistortionPedal;

class DistortionEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    DistortionEditor(DistortionPedal& pedal);
    ~DistortionEditor() override
    {
        stopTimer();
        sldGain.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldBody.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
        sldLevel.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintCurveViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    DistortionPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Red accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ffEF4444");
    const juce::Colour accentDim  = juce::Colour::fromString("ffDC2626");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFCA5A5");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Knobs
    juce::Slider sldGain, sldTone, sldBody, sldMix, sldLevel;
    juce::Label lblGain, lblTone, lblBody, lblMix, lblLevel;
    juce::Label valGain, valTone, valBody, valMix, valLevel;

    // Visualization
    juce::Rectangle<float> vizBounds;

    struct DistKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ffEF4444");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffFCA5A5");

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
            float sliderPos, float startAngle, float endAngle,
            juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(6.0f);
            const float r = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float cx = area.getCentreX();
            const float cy = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcR = r - 4.0f;

            {
                juce::Path track;
                track.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
                g.setColour(juce::Colour(0xff1E2A3A));
                g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
            }

            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            const float kr = r * 0.56f;
            {
                juce::ColourGradient grad(juce::Colour(0xff1C2838), cx, cy - kr,
                    juce::Colour(0xff0D1520), cx, cy + kr, false);
                g.setGradientFill(grad);
                g.fillEllipse(cx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
                g.setColour(juce::Colour(0xff2A3A4C));
                g.drawEllipse(cx - kr, cy - kr, kr * 2.0f, kr * 2.0f, 1.0f);
            }

            const float pLen = kr * 0.70f;
            const float px = cx + std::sin(angle) * pLen;
            const float py = cy - std::cos(angle) * pLen;
            g.setColour(kAccentGlow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DistortionEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline DistortionEditor::DistortionEditor(DistortionPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& s, juce::Label& lbl, juce::Label& val,
        const juce::String& name, float min, float max, float def, float step = 0.01f)
    {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        s.setRange(min, max, step);
        s.setValue(def, juce::dontSendNotification);
        s.setLookAndFeel(&knobLnF);
        addAndMakeVisible(s);

        lbl.setText(name, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centred);
        lbl.setFont(juce::Font(11.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(lbl);

        val.setJustificationType(juce::Justification::centred);
        val.setFont(juce::Font(11.0f));
        val.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(val);
    };

    initKnob(sldGain,  lblGain,  valGain,  "GAIN",  0.0f,  100.0f, 55.0f, 0.1f);
    initKnob(sldTone,  lblTone,  valTone,  "TONE",  0.0f,  1.0f,   0.52f);
    initKnob(sldBody,  lblBody,  valBody,  "BODY",  0.0f,  1.0f,   0.5f);
    initKnob(sldMix,   lblMix,   valMix,   "MIX",   0.0f,  1.0f,   1.0f);
    initKnob(sldLevel, lblLevel, valLevel, "LEVEL", 0.0f,  1.0f,   0.62f);

    auto wireParam = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p == nullptr) return;
        s.setValue(p->get(), juce::dontSendNotification);
        s.onValueChange = [&s, p]
        {
            p->beginChangeGesture();
            *p = (float)s.getValue();
            p->endChangeGesture();
        };
    };
    wireParam(sldGain,  proc.gainParam);
    wireParam(sldTone,  proc.toneParam);
    wireParam(sldBody,  proc.bodyParam);
    wireParam(sldMix,   proc.mixParam);
    wireParam(sldLevel, proc.levelParam);

    startTimerHz(30);
}

inline void DistortionEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldGain,  proc.gainParam);
    syncSlider(sldTone,  proc.toneParam);
    syncSlider(sldBody,  proc.bodyParam);
    syncSlider(sldMix,   proc.mixParam);
    syncSlider(sldLevel, proc.levelParam);

    valGain.setText(juce::String(juce::roundToInt(sldGain.getValue())) + "%", juce::dontSendNotification);
    valTone.setText(juce::String((int)(sldTone.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valBody.setText(juce::String((int)(sldBody.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valMix.setText(juce::String((int)(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valLevel.setText(juce::String((int)(sldLevel.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void DistortionEditor::paint(juce::Graphics& g)
{
    {
        juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
            bgDark, 0.0f, (float)getHeight(), false);
        g.setGradientFill(bg);
        g.fillAll();
    }

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    {
        juce::ColourGradient glow(accent.withAlpha(0.30f), (float)getWidth() * 0.2f, 0.0f,
            accent.withAlpha(0.0f), (float)getWidth() * 0.8f, 0.0f, false);
        g.setGradientFill(glow);
        g.fillRect(0.0f, 0.0f, (float)getWidth(), 2.0f);
    }

    g.setColour(textBright);
    g.setFont(juce::Font(22.0f, juce::Font::bold));
    g.drawText("DISTORTION", 28, 10, 240, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Shred", 28, 33, 200, 18, juce::Justification::centredLeft);

    paintCurveViz(g, vizBounds);
}

inline void DistortionEditor::paintCurveViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float cx = inner.getCentreX();
    const float cy = inner.getCentreY();

    const float gain = (float)sldGain.getValue();

    // Grid lines
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), cy, inner.getRight(), cy, 0.5f);
    g.drawLine(cx, inner.getY(), cx, inner.getBottom(), 0.5f);
    // Diagonal reference (linear = no distortion)
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), inner.getBottom(), inner.getRight(), inner.getY(), 0.4f);

    // Axis labels
    g.setFont(juce::Font(8.0f));
    g.setColour(textDim.withAlpha(0.3f));
    g.drawText("IN", (int)inner.getX() + 2, (int)(inner.getBottom() - 12), 20, 10,
        juce::Justification::centredLeft);
    g.drawText("OUT", (int)(inner.getRight() - 24), (int)inner.getY() + 2, 22, 10,
        juce::Justification::centredRight);

    // Transfer curve: input -1..+1 → output via computeClipCurve
    juce::Path curvePath;
    constexpr int numPoints = 200;

    for (int i = 0; i < numPoints; ++i)
    {
        float t = (float)i / (float)(numPoints - 1);
        float input = t * 2.0f - 1.0f;  // -1 to +1

        float output = DistortionPedal::computeClipCurve(input, gain);
        output = juce::jlimit(-1.0f, 1.0f, output);

        // Map: input → x, output → y (inverted y axis)
        float px = inner.getX() + (input * 0.5f + 0.5f) * w;
        float py = inner.getY() + (1.0f - (output * 0.5f + 0.5f)) * h;

        if (i == 0)
            curvePath.startNewSubPath(px, py);
        else
            curvePath.lineTo(px, py);
    }

    // Glow
    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(curvePath, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.12f));
    g.strokePath(curvePath, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    // Main line
    g.setColour(accentGlow.withAlpha(0.85f));
    g.strokePath(curvePath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));

    // Asymmetry indicator: show positive vs negative clip points
    float posClip = DistortionPedal::computeClipCurve(1.0f, gain);
    float negClip = std::abs(DistortionPedal::computeClipCurve(-1.0f, gain));
    float asymmetry = std::abs(posClip - negClip) / juce::jmax(0.01f, posClip + negClip) * 2.0f;

    // Character label
    g.setFont(juce::Font(9.0f));
    g.setColour(textDim.withAlpha(0.4f));
    const char* character = gain < 20.0f ? "Crunch"
        : gain < 45.0f ? "Drive"
        : gain < 70.0f ? "Distortion"
        : "Shred";
    g.drawText(character, (int)(inner.getRight() - 60), (int)(inner.getBottom() - 14), 55, 12,
        juce::Justification::centredRight);

    // Asymmetry readout
    if (asymmetry > 0.02f)
    {
        g.setColour(accent.withAlpha(0.3f));
        g.setFont(juce::Font(8.0f));
        g.drawText("Asym " + juce::String((int)(asymmetry * 100.0f)) + "%",
            (int)inner.getX() + 2, (int)inner.getY() + 2, 60, 10,
            juce::Justification::centredLeft);
    }
}

inline void DistortionEditor::resized()
{
    const int w = getWidth();

    vizBounds = juce::Rectangle<float>(28.0f, 58.0f, (float)(w - 56), 140.0f);

    const int knobSize = 80;
    const int knobY = 218;
    const int labelH = 16;
    const int valH = 16;
    const int slotW = w / 5;

    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup knobs[] = {
        { sldGain,  lblGain,  valGain },
        { sldTone,  lblTone,  valTone },
        { sldBody,  lblBody,  valBody },
        { sldMix,   lblMix,   valMix },
        { sldLevel, lblLevel, valLevel }
    };

    for (int i = 0; i < 5; ++i)
    {
        int cx = slotW * i + slotW / 2;
        knobs[i].lbl.setBounds(cx - 50, knobY, 100, labelH);
        knobs[i].s.setBounds(cx - knobSize / 2, knobY + labelH + 4, knobSize, knobSize);
        knobs[i].val.setBounds(cx - 50, knobY + labelH + 4 + knobSize + 2, 100, valH);
    }
}

// ---- Wire createEditor ----
inline juce::AudioProcessorEditor* DistortionPedal::createEditor()
{
    return new DistortionEditor(*this);
}
