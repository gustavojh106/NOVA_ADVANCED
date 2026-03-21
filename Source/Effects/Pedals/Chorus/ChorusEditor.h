#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class ChorusPedal;

class ChorusEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    ChorusEditor(ChorusPedal& pedal);
    ~ChorusEditor() override
    {
        stopTimer();
        sldRate.setLookAndFeel(nullptr);
        sldDepth.setLookAndFeel(nullptr);
        sldWidth.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintLfoViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    ChorusPedal& proc;

    static constexpr int kWidth  = 640;
    static constexpr int kHeight = 410;

    // Indigo/purple accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ff818CF8");
    const juce::Colour accentDim  = juce::Colour::fromString("ff6366F1");
    const juce::Colour accentGlow = juce::Colour::fromString("ffA5B4FC");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Knobs
    juce::Slider sldRate, sldDepth, sldWidth, sldTone, sldMix;
    juce::Label lblRate, lblDepth, lblWidth, lblTone, lblMix;
    juce::Label valRate, valDepth, valWidth, valTone, valMix;

    // Visualization
    juce::Rectangle<float> vizBounds;

    struct ChorusKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ff818CF8");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffA5B4FC");

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

            // Track
            {
                juce::Path track;
                track.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
                g.setColour(juce::Colour(0xff1E2A3A));
                g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
            }

            // Value arc
            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            // Body
            const float kr = r * 0.56f;
            {
                juce::ColourGradient grad(juce::Colour(0xff1C2838), cx, cy - kr,
                    juce::Colour(0xff0D1520), cx, cy + kr, false);
                g.setGradientFill(grad);
                g.fillEllipse(cx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
                g.setColour(juce::Colour(0xff2A3A4C));
                g.drawEllipse(cx - kr, cy - kr, kr * 2.0f, kr * 2.0f, 1.0f);
            }

            // Pointer
            const float pLen = kr * 0.70f;
            const float px = cx + std::sin(angle) * pLen;
            const float py = cy - std::cos(angle) * pLen;
            g.setColour(kAccentGlow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChorusEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline ChorusEditor::ChorusEditor(ChorusPedal& pedal)
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

    initKnob(sldRate,  lblRate,  valRate,  "RATE",  0.05f, 5.0f, 0.85f);
    initKnob(sldDepth, lblDepth, valDepth, "DEPTH", 0.0f, 1.0f, 0.56f);
    initKnob(sldWidth, lblWidth, valWidth, "WIDTH", 0.0f, 1.0f, 0.68f);
    initKnob(sldTone,  lblTone,  valTone,  "TONE",  0.0f, 1.0f, 0.55f);
    initKnob(sldMix,   lblMix,   valMix,   "MIX",   0.0f, 1.0f, 0.34f);

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
    wireParam(sldRate,  proc.rateParam);
    wireParam(sldDepth, proc.depthParam);
    wireParam(sldWidth, proc.widthParam);
    wireParam(sldTone,  proc.toneParam);
    wireParam(sldMix,   proc.mixParam);

    startTimerHz(30);
}

inline void ChorusEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldRate,  proc.rateParam);
    syncSlider(sldDepth, proc.depthParam);
    syncSlider(sldWidth, proc.widthParam);
    syncSlider(sldTone,  proc.toneParam);
    syncSlider(sldMix,   proc.mixParam);

    // Value readouts
    valRate.setText(juce::String((float)sldRate.getValue(), 2) + " Hz", juce::dontSendNotification);
    valDepth.setText(juce::String((int)(sldDepth.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valWidth.setText(juce::String((int)(sldWidth.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valTone.setText(juce::String((int)(sldTone.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valMix.setText(juce::String((int)(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void ChorusEditor::paint(juce::Graphics& g)
{
    // Background
    {
        juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
            bgDark, 0.0f, (float)getHeight(), false);
        g.setGradientFill(bg);
        g.fillAll();
    }

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    // Top glow
    {
        juce::ColourGradient glow(accent.withAlpha(0.30f), (float)getWidth() * 0.2f, 0.0f,
            accent.withAlpha(0.0f), (float)getWidth() * 0.8f, 0.0f, false);
        g.setGradientFill(glow);
        g.fillRect(0.0f, 0.0f, (float)getWidth(), 2.0f);
    }

    // Header
    g.setColour(textBright);
    g.setFont(juce::Font(22.0f, juce::Font::bold));
    g.drawText("CHORUS", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Shimmer", 28, 33, 200, 18, juce::Justification::centredLeft);

    // LFO visualization
    paintLfoViz(g, vizBounds);
}

inline void ChorusEditor::paintLfoViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Panel
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 14.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float midY = inner.getCentreY();

    const float rate  = (float)sldRate.getValue();
    const float depth = (float)sldDepth.getValue();
    const float width = (float)sldWidth.getValue();
    const float phase = proc.lfoPhase;

    constexpr float twoPi = juce::MathConstants<float>::twoPi;

    // Draw L channel LFO wave
    juce::Path pathL, pathR;
    const float stereoOffset = 0.25f + width * 0.20f;
    const float amp = h * 0.35f * depth;

    for (int px = 0; px < (int)w; ++px)
    {
        float t = (float)px / w;
        float displayPhase = t * 2.5f;  // Show ~2.5 cycles

        float lfoL = std::sin(twoPi * (displayPhase + phase));
        float lfoR = std::sin(twoPi * (displayPhase + phase + stereoOffset));

        float yL = midY - lfoL * amp;
        float yR = midY - lfoR * amp;

        if (px == 0) { pathL.startNewSubPath(inner.getX(), yL); pathR.startNewSubPath(inner.getX(), yR); }
        else { pathL.lineTo(inner.getX() + (float)px, yL); pathR.lineTo(inner.getX() + (float)px, yR); }
    }

    // L wave (bright)
    g.setColour(accent.withAlpha(0.12f));
    g.strokePath(pathL, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.70f));
    g.strokePath(pathL, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));

    // R wave (dimmer, offset)
    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(pathR, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.40f));
    g.strokePath(pathR, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));

    // Center line
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawLine(inner.getX(), midY, inner.getRight(), midY, 0.5f);

    // Labels
    g.setFont(juce::Font(9.0f, juce::Font::bold));
    g.setColour(accentGlow.withAlpha(0.50f));
    g.drawText("L", (int)inner.getX() + 4, (int)inner.getY() + 2, 20, 12, juce::Justification::centredLeft);
    g.setColour(accent.withAlpha(0.35f));
    g.drawText("R", (int)inner.getX() + 18, (int)inner.getY() + 2, 20, 12, juce::Justification::centredLeft);
}

inline void ChorusEditor::resized()
{
    const int w = getWidth();

    // Viz
    vizBounds = juce::Rectangle<float>(28.0f, 58.0f, (float)(w - 56), 130.0f);

    // Knobs
    const int knobSize = 80;
    const int knobY = 208;
    const int labelH = 16;
    const int valH = 16;
    const int slotW = w / 5;

    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup knobs[] = {
        { sldRate,  lblRate,  valRate },
        { sldDepth, lblDepth, valDepth },
        { sldWidth, lblWidth, valWidth },
        { sldTone,  lblTone,  valTone },
        { sldMix,   lblMix,   valMix }
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
inline juce::AudioProcessorEditor* ChorusPedal::createEditor()
{
    return new ChorusEditor(*this);
}
