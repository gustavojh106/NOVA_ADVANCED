#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class OctavePedal;

class OctaveEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    OctaveEditor(OctavePedal& pedal);
    ~OctaveEditor() override
    {
        stopTimer();
        sldSub.setLookAndFeel(nullptr);
        sldUpper.setLookAndFeel(nullptr);
        sldDry.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldLevel.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintBlendViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    OctavePedal& proc;

    static constexpr int kWidth  = 620;
    static constexpr int kHeight = 400;

    // Green accent palette
    const juce::Colour accent     = juce::Colour::fromString("ff22C55E");
    const juce::Colour accentDim  = juce::Colour::fromString("ff16A34A");
    const juce::Colour accentGlow = juce::Colour::fromString("ff4ADE80");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Knobs
    juce::Slider sldSub, sldUpper, sldDry, sldTone, sldLevel;
    juce::Label lblSub, lblUpper, lblDry, lblTone, lblLevel;
    juce::Label valSub, valUpper, valDry, valTone, valLevel;

    // Visualization bounds
    juce::Rectangle<float> vizBounds;

    // Knob LookAndFeel
    struct OctaveKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ff22C55E");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff4ADE80");

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

            // Background track
            {
                juce::Path track;
                track.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
                g.setColour(juce::Colour(0xff1E2A3A));
                g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
            }

            // Value arc + glow
            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            // Knob body
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OctaveEditor)
};

// ============================================================================
//  Inline implementation — included AFTER OctavePedal is fully defined
// ============================================================================

inline OctaveEditor::OctaveEditor(OctavePedal& pedal)
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
        lbl.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        lbl.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(lbl);

        val.setJustificationType(juce::Justification::centred);
        val.setFont(juce::Font(juce::FontOptions(11.0f)));
        val.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(val);
    };

    initKnob(sldSub,   lblSub,   valSub,   "SUB",   0.0f, 1.0f, 0.72f);
    initKnob(sldUpper, lblUpper, valUpper, "UPPER", 0.0f, 1.0f, 0.28f);
    initKnob(sldDry,   lblDry,   valDry,   "DRY",   0.0f, 1.0f, 0.82f);
    initKnob(sldTone,  lblTone,  valTone,  "TONE",  0.0f, 1.0f, 0.50f);
    initKnob(sldLevel, lblLevel, valLevel, "LEVEL", 0.5f, 2.0f, 1.0f);

    // Wire sliders to parameters
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
    wireParam(sldSub,   proc.subParam);
    wireParam(sldUpper, proc.upperParam);
    wireParam(sldDry,   proc.dryParam);
    wireParam(sldTone,  proc.toneParam);
    wireParam(sldLevel, proc.levelParam);

    startTimerHz(20);
}

inline void OctaveEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldSub,   proc.subParam);
    syncSlider(sldUpper, proc.upperParam);
    syncSlider(sldDry,   proc.dryParam);
    syncSlider(sldTone,  proc.toneParam);
    syncSlider(sldLevel, proc.levelParam);

    // Value readouts
    auto fmtPct = [](juce::Label& lbl, float v)
    {
        lbl.setText(juce::String((int)(v * 100.0f)) + "%", juce::dontSendNotification);
    };
    fmtPct(valSub,   (float)sldSub.getValue());
    fmtPct(valUpper, (float)sldUpper.getValue());
    fmtPct(valDry,   (float)sldDry.getValue());
    fmtPct(valTone,  (float)sldTone.getValue());

    float lv = (float)sldLevel.getValue();
    float db = 20.0f * std::log10(juce::jmax(0.001f, lv));
    valLevel.setText(juce::String(db, 1) + " dB", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void OctaveEditor::paint(juce::Graphics& g)
{
    // Background
    {
        juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
            bgDark, 0.0f, (float)getHeight(), false);
        g.setGradientFill(bg);
        g.fillAll();
    }

    // Border
    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    // Top accent glow
    {
        juce::ColourGradient glow(accent.withAlpha(0.30f), (float)getWidth() * 0.2f, 0.0f,
            accent.withAlpha(0.0f), (float)getWidth() * 0.8f, 0.0f, false);
        g.setGradientFill(glow);
        g.fillRect(0.0f, 0.0f, (float)getWidth(), 2.0f);
    }

    // Header
    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("OCTAVE", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("Polaris", 28, 33, 200, 18, juce::Justification::centredLeft);

    // Blend visualization
    paintBlendViz(g, vizBounds);
}

inline void OctaveEditor::paintBlendViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Panel background
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    const float inset = 16.0f;
    auto inner = bounds.reduced(inset, inset);
    const float h = inner.getHeight();

    // Three blend bars: SUB (-1 oct), DRY (0), UPPER (+1 oct)
    struct BarInfo { const char* label; float value; juce::Colour color; };
    const float subVal   = (float)sldSub.getValue();
    const float dryVal   = (float)sldDry.getValue();
    const float upperVal = (float)sldUpper.getValue();

    BarInfo bars[] = {
        { "-1 OCT",  subVal,   juce::Colour(0xff16A34A) },
        { "DRY",     dryVal,   juce::Colour(0xff64748B) },
        { "+1 OCT",  upperVal, juce::Colour(0xff4ADE80) }
    };

    const float barW = 36.0f;
    const float gap = (inner.getWidth() - barW * 3.0f) / 4.0f;

    for (int i = 0; i < 3; ++i)
    {
        float bx = inner.getX() + gap + (float)i * (barW + gap);
        float barH = h * bars[i].value * 0.85f;
        float by = inner.getBottom() - barH;

        // Bar background track
        g.setColour(juce::Colour(0xff1A2332));
        g.fillRoundedRectangle(bx, inner.getY(), barW, h, 4.0f);

        // Filled bar
        if (barH > 1.0f)
        {
            juce::ColourGradient barGrad(bars[i].color.withAlpha(0.85f), bx, by,
                bars[i].color.withAlpha(0.35f), bx, inner.getBottom(), false);
            g.setGradientFill(barGrad);
            g.fillRoundedRectangle(bx, by, barW, barH, 4.0f);

            // Top glow
            g.setColour(bars[i].color.withAlpha(0.20f));
            g.fillRoundedRectangle(bx - 3.0f, by - 2.0f, barW + 6.0f, 6.0f, 3.0f);
        }

        // Label below
        g.setColour(textDim);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(bars[i].label, (int)(bx - 8.0f), (int)(inner.getBottom() + 4.0f),
            (int)(barW + 16.0f), 14, juce::Justification::centred);
    }

    // Waveform hints between bars
    auto drawWaveHint = [&](float cx, float cy, float freq, float amp)
    {
        juce::Path wave;
        const float waveW = 30.0f;
        for (int px = 0; px < (int)waveW; ++px)
        {
            float t = (float)px / waveW;
            float y = cy - std::sin(t * freq * juce::MathConstants<float>::twoPi) * amp;
            if (px == 0) wave.startNewSubPath(cx - waveW * 0.5f + (float)px, y);
            else wave.lineTo(cx - waveW * 0.5f + (float)px, y);
        }
        g.setColour(accent.withAlpha(0.25f));
        g.strokePath(wave, juce::PathStrokeType(1.2f));
    };

    float midY = inner.getCentreY();
    float bar0Right = inner.getX() + gap + barW;
    float bar1Left  = inner.getX() + gap * 2.0f + barW;
    float bar1Right = bar1Left + barW;
    float bar2Left  = inner.getX() + gap * 3.0f + barW * 2.0f;

    drawWaveHint((bar0Right + bar1Left) * 0.5f, midY, 1.0f, 8.0f);  // Sub wave (slow)
    drawWaveHint((bar1Right + bar2Left) * 0.5f, midY, 3.0f, 6.0f);  // Upper wave (fast)
}

inline void OctaveEditor::resized()
{
    const int w = getWidth();

    // Visualization
    vizBounds = juce::Rectangle<float>(28.0f, 58.0f, (float)(w - 56), 140.0f);

    // Knobs row
    const int knobSize = 80;
    const int knobY = 218;
    const int labelH = 16;
    const int valH = 16;
    const int slotW = w / 5;

    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup knobs[] = {
        { sldSub,   lblSub,   valSub },
        { sldUpper, lblUpper, valUpper },
        { sldDry,   lblDry,   valDry },
        { sldTone,  lblTone,  valTone },
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

// ---- Wire createEditor back to processor ----
inline juce::AudioProcessorEditor* OctavePedal::createEditor()
{
    return new OctaveEditor(*this);
}
