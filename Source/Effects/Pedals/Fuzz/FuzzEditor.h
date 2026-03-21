#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class FuzzPedal;

class FuzzEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    FuzzEditor(FuzzPedal& pedal);
    ~FuzzEditor() override
    {
        stopTimer();
        sldFuzz.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldGate.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
        sldLevel.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintTransferCurve(juce::Graphics& g, juce::Rectangle<float> bounds);

    FuzzPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Purple accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ffA855F7");
    const juce::Colour accentDim  = juce::Colour::fromString("ff7C3AED");
    const juce::Colour accentGlow = juce::Colour::fromString("ffC4B5FD");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Knobs
    juce::Slider sldFuzz, sldTone, sldGate, sldMix, sldLevel;
    juce::Label lblFuzz, lblTone, lblGate, lblMix, lblLevel;
    juce::Label valFuzz, valTone, valGate, valMix, valLevel;

    // Visualization
    juce::Rectangle<float> vizBounds;

    struct FuzzKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ffA855F7");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffC4B5FD");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FuzzEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline FuzzEditor::FuzzEditor(FuzzPedal& pedal)
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

    initKnob(sldFuzz,  lblFuzz,  valFuzz,  "FUZZ",  0.0f, 100.0f, 65.0f, 1.0f);
    initKnob(sldTone,  lblTone,  valTone,  "TONE",  0.0f, 1.0f, 0.5f);
    initKnob(sldGate,  lblGate,  valGate,  "GATE",  0.0f, 1.0f, 0.3f);
    initKnob(sldMix,   lblMix,   valMix,   "MIX",   0.0f, 1.0f, 1.0f);
    initKnob(sldLevel, lblLevel, valLevel, "LEVEL", 0.0f, 1.0f, 0.55f);

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
    wireParam(sldFuzz,  proc.fuzzParam);
    wireParam(sldTone,  proc.toneParam);
    wireParam(sldGate,  proc.gateParam);
    wireParam(sldMix,   proc.mixParam);
    wireParam(sldLevel, proc.levelParam);

    startTimerHz(30);
}

inline void FuzzEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldFuzz,  proc.fuzzParam);
    syncSlider(sldTone,  proc.toneParam);
    syncSlider(sldGate,  proc.gateParam);
    syncSlider(sldMix,   proc.mixParam);
    syncSlider(sldLevel, proc.levelParam);

    valFuzz.setText(juce::String((int)sldFuzz.getValue()) + "%", juce::dontSendNotification);
    {
        float tone = (float)sldTone.getValue();
        float hz = juce::jmap(tone, 800.0f, 6500.0f);
        if (hz >= 1000.0f)
            valTone.setText(juce::String(hz * 0.001f, 1) + " kHz", juce::dontSendNotification);
        else
            valTone.setText(juce::String((int)hz) + " Hz", juce::dontSendNotification);
    }
    valGate.setText(juce::String((int)(sldGate.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valMix.setText(juce::String((int)(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valLevel.setText(juce::String((int)(sldLevel.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void FuzzEditor::paint(juce::Graphics& g)
{
    // Background gradient
    {
        juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
            bgDark, 0.0f, (float)getHeight(), false);
        g.setGradientFill(bg);
        g.fillAll();
    }

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    // Top glow accent line
    {
        juce::ColourGradient glow(accent.withAlpha(0.30f), (float)getWidth() * 0.2f, 0.0f,
            accent.withAlpha(0.0f), (float)getWidth() * 0.8f, 0.0f, false);
        g.setGradientFill(glow);
        g.fillRect(0.0f, 0.0f, (float)getWidth(), 2.0f);
    }

    // Header
    g.setColour(textBright);
    g.setFont(juce::Font(22.0f, juce::Font::bold));
    g.drawText("FUZZ", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Velvet", 28, 33, 200, 18, juce::Justification::centredLeft);

    // Transfer curve
    paintTransferCurve(g, vizBounds);
}

inline void FuzzEditor::paintTransferCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Panel background
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float midX = inner.getCentreX();
    const float midY = inner.getCentreY();
    const float halfH = h * 0.45f;

    const float fuzzAmount = (float)sldFuzz.getValue();
    const float drive = 2.0f + fuzzAmount * 0.48f;
    const float bias = 0.06f + fuzzAmount * 0.002f;
    const float fuzzNorm = fuzzAmount * 0.01f;
    const float octaveBlend = fuzzNorm * fuzzNorm * 0.35f;

    // Grid: axes
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), midY, inner.getRight(), midY, 0.5f);  // horizontal
    g.drawLine(midX, inner.getY(), midX, inner.getBottom(), 0.5f);  // vertical

    // Linear reference line (diagonal, dim)
    g.setColour(juce::Colour(0xff1A2535).withAlpha(0.6f));
    g.drawLine(inner.getX(), inner.getBottom(), inner.getRight(), inner.getY(), 0.5f);

    // Compute the transfer curve
    juce::Path curvePath;
    const int steps = (int)w;

    for (int px = 0; px <= steps; ++px)
    {
        float t = (float)px / (float)steps;
        float inputVal = t * 2.0f - 1.0f;  // -1 to +1

        // Apply the same clipping function as the processor
        float x = inputVal * drive + bias;

        // Germanium asymmetric clip
        float clipped;
        if (x >= 0.0f)
            clipped = 1.0f - std::exp(-x * 1.2f);
        else
            clipped = -(1.0f - std::exp(x * 1.6f)) * 0.78f;

        // Add octave harmonics
        float octave = std::abs(inputVal * drive) * octaveBlend;
        clipped += std::tanh(octave * 1.5f) * 0.28f;

        // Clamp
        clipped = juce::jlimit(-1.2f, 1.2f, clipped);

        float plotX = inner.getX() + t * w;
        float plotY = midY - clipped * halfH;

        if (px == 0)
            curvePath.startNewSubPath(plotX, plotY);
        else
            curvePath.lineTo(plotX, plotY);
    }

    // Glow behind curve
    g.setColour(accent.withAlpha(0.08f));
    g.strokePath(curvePath, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));

    // Main curve
    g.setColour(accentGlow.withAlpha(0.75f));
    g.strokePath(curvePath, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));

    // Highlight the asymmetry region (positive brighter)
    g.setColour(accent.withAlpha(0.12f));
    g.strokePath(curvePath, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));

    // Axis labels
    g.setFont(juce::Font(8.0f, juce::Font::bold));
    g.setColour(textDim.withAlpha(0.4f));
    g.drawText("IN", (int)inner.getRight() - 16, (int)midY + 2, 16, 10, juce::Justification::centredRight);
    g.drawText("OUT", (int)midX + 3, (int)inner.getY(), 22, 10, juce::Justification::centredLeft);

    // Sag indicator (small bar in top-right corner)
    {
        float sag = proc.sagEnvelope;
        float sagBarW = 4.0f;
        float sagBarMaxH = 30.0f;
        float sagBarX = inner.getRight() - 20.0f;
        float sagBarY = inner.getY() + 8.0f;
        float sagBarH = sag * sagBarMaxH;

        g.setColour(juce::Colour(0xff1E2A3A));
        g.fillRoundedRectangle(sagBarX, sagBarY, sagBarW, sagBarMaxH, 1.5f);

        juce::Colour sagColour = accent.interpolatedWith(juce::Colour(0xffEF4444), juce::jlimit(0.0f, 1.0f, sag * 2.0f));
        g.setColour(sagColour.withAlpha(0.7f));
        g.fillRoundedRectangle(sagBarX, sagBarY + sagBarMaxH - sagBarH, sagBarW, sagBarH, 1.5f);

        g.setFont(juce::Font(7.0f));
        g.setColour(textDim.withAlpha(0.4f));
        g.drawText("SAG", (int)(sagBarX - 8), (int)(sagBarY + sagBarMaxH + 2), 20, 8, juce::Justification::centred);
    }

    // Character label based on fuzz amount
    {
        g.setFont(juce::Font(9.0f));
        g.setColour(accent.withAlpha(0.5f));
        juce::String character;
        if (fuzzAmount < 25.0f)      character = "Clean edge";
        else if (fuzzAmount < 50.0f) character = "Warm growl";
        else if (fuzzAmount < 75.0f) character = "Germanium roar";
        else                         character = "Full velvet";
        g.drawText(character, (int)inner.getX(), (int)inner.getY() + 2, 100, 12, juce::Justification::centredLeft);
    }
}

inline void FuzzEditor::resized()
{
    const int w = getWidth();

    // Viz panel
    vizBounds = juce::Rectangle<float>(28.0f, 58.0f, (float)(w - 56), 140.0f);

    // Knobs
    const int knobSize = 80;
    const int knobY = 218;
    const int labelH = 16;
    const int valH = 16;
    const int slotW = w / 5;

    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup knobs[] = {
        { sldFuzz,  lblFuzz,  valFuzz },
        { sldTone,  lblTone,  valTone },
        { sldGate,  lblGate,  valGate },
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
inline juce::AudioProcessorEditor* FuzzPedal::createEditor()
{
    return new FuzzEditor(*this);
}
