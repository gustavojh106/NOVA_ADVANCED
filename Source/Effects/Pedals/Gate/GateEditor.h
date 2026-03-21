#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class NoiseGatePedal;

class GateEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    GateEditor(NoiseGatePedal& pedal);
    ~GateEditor() override
    {
        stopTimer();
        sldThresh.setLookAndFeel(nullptr);
        sldAttack.setLookAndFeel(nullptr);
        sldHold.setLookAndFeel(nullptr);
        sldRelease.setLookAndFeel(nullptr);
        sldRange.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintGateViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    NoiseGatePedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Green accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ff10B981");
    const juce::Colour accentDim  = juce::Colour::fromString("ff059669");
    const juce::Colour accentGlow = juce::Colour::fromString("ff34D399");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    const juce::Colour gateOpenCol  = juce::Colour(0xff22C55E);
    const juce::Colour gateClosedCol = juce::Colour(0xffEF4444);

    // Knobs
    juce::Slider sldThresh, sldAttack, sldHold, sldRelease, sldRange;
    juce::Label lblThresh, lblAttack, lblHold, lblRelease, lblRange;
    juce::Label valThresh, valAttack, valHold, valRelease, valRange;

    // Visualization
    juce::Rectangle<float> vizBounds;

    // Smoothed metering values for display
    float displayGateGain = 0.0f;
    float displayEnvDb    = -96.0f;

    struct GateKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ff10B981");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff34D399");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GateEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline GateEditor::GateEditor(NoiseGatePedal& pedal)
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

    initKnob(sldThresh,  lblThresh,  valThresh,  "THRESHOLD", -80.0f,  0.0f,   -45.0f, 0.1f);
    initKnob(sldAttack,  lblAttack,  valAttack,  "ATTACK",      0.1f,  20.0f,    0.5f);
    initKnob(sldHold,    lblHold,    valHold,    "HOLD",         5.0f, 500.0f,   80.0f, 1.0f);
    initKnob(sldRelease, lblRelease, valRelease, "RELEASE",      5.0f, 500.0f,   85.0f, 1.0f);
    initKnob(sldRange,   lblRange,   valRange,   "RANGE",      -96.0f,  0.0f,  -96.0f, 0.1f);

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
    wireParam(sldThresh,  proc.thresholdParam);
    wireParam(sldAttack,  proc.attackParam);
    wireParam(sldHold,    proc.holdParam);
    wireParam(sldRelease, proc.releaseParam);
    wireParam(sldRange,   proc.rangeParam);

    startTimerHz(30);
}

inline void GateEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldThresh,  proc.thresholdParam);
    syncSlider(sldAttack,  proc.attackParam);
    syncSlider(sldHold,    proc.holdParam);
    syncSlider(sldRelease, proc.releaseParam);
    syncSlider(sldRange,   proc.rangeParam);

    // Value readouts
    valThresh.setText(juce::String((float)sldThresh.getValue(), 1) + " dB", juce::dontSendNotification);
    {
        float a = (float)sldAttack.getValue();
        valAttack.setText(a < 1.0f ? juce::String(a, 1) + " ms" : juce::String((int)a) + " ms",
            juce::dontSendNotification);
    }
    valHold.setText(juce::String((int)sldHold.getValue()) + " ms", juce::dontSendNotification);
    valRelease.setText(juce::String((int)sldRelease.getValue()) + " ms", juce::dontSendNotification);
    {
        float r = (float)sldRange.getValue();
        valRange.setText(r <= -95.0f ? "OFF" : juce::String(r, 1) + " dB", juce::dontSendNotification);
    }

    // Smooth metering values
    float targetGG = proc.currentGateGainAtomic;
    float targetEnv = proc.currentEnvelopeDb;
    displayGateGain += 0.3f * (targetGG - displayGateGain);
    displayEnvDb += 0.3f * (targetEnv - displayEnvDb);

    repaint(vizBounds.toNearestInt());
}

inline void GateEditor::paint(juce::Graphics& g)
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
    g.drawText("NOISE GATE", 28, 10, 240, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Silencer", 28, 33, 200, 18, juce::Justification::centredLeft);

    // Gate state indicator
    {
        bool open = displayGateGain > 0.5f;
        g.setColour(open ? gateOpenCol.withAlpha(0.8f) : gateClosedCol.withAlpha(0.5f));
        g.fillRoundedRectangle((float)getWidth() - 90.0f, 16.0f, 10.0f, 10.0f, 5.0f);
        g.setColour(textDim.withAlpha(0.5f));
        g.setFont(juce::Font(10.0f));
        g.drawText(open ? "OPEN" : "CLOSED",
            getWidth() - 76, 14, 50, 14, juce::Justification::centredLeft);
    }

    paintGateViz(g, vizBounds);
}

inline void GateEditor::paintGateViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();

    const float threshDb = (float)sldThresh.getValue();
    const float rangeDb  = (float)sldRange.getValue();
    const float envDb    = displayEnvDb;
    const float gg       = displayGateGain;

    // dB scale: -80 to 0
    constexpr float dbMin = -80.0f;
    constexpr float dbMax = 0.0f;

    auto dbToX = [&](float db) -> float
    {
        float norm = (db - dbMin) / (dbMax - dbMin);
        return inner.getX() + juce::jlimit(0.0f, 1.0f, norm) * w;
    };

    // Grid lines
    g.setFont(juce::Font(8.0f));
    const float gridDbs[] = { -72.0f, -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, 0.0f };
    const char* gridLabels[] = { "-72", "-60", "-48", "-36", "-24", "-12", "0" };
    for (int i = 0; i < 7; ++i)
    {
        float x = dbToX(gridDbs[i]);
        g.setColour(juce::Colour(0xff1A2535));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.3f));
        g.drawText(gridLabels[i], (int)(x - 15), (int)(inner.getBottom() + 1), 30, 10,
            juce::Justification::centred);
    }

    // ---- Gate response curve ----
    // Below threshold: output = input + range (in dB, clamped)
    // Above threshold: output = input (unity)
    // Hysteresis zone between thresh and thresh-6dB
    juce::Path curvePath;
    constexpr int numPts = 200;
    const float hystDb = 6.0f;

    for (int i = 0; i < numPts; ++i)
    {
        float t = (float)i / (float)(numPts - 1);
        float inDb = dbMin + t * (dbMax - dbMin);
        float outGainDb;

        if (inDb >= threshDb)
            outGainDb = 0.0f;  // Full open
        else if (inDb >= threshDb - hystDb)
        {
            // Hysteresis blend zone
            float blend = (inDb - (threshDb - hystDb)) / hystDb;
            outGainDb = rangeDb * (1.0f - blend);
        }
        else
            outGainDb = rangeDb;

        float outDb = inDb + outGainDb;

        float px = inner.getX() + t * w;
        float outNorm = (outDb - dbMin) / (dbMax - dbMin);
        float py = inner.getBottom() - juce::jlimit(0.0f, 1.0f, outNorm) * h;

        if (i == 0)
            curvePath.startNewSubPath(px, py);
        else
            curvePath.lineTo(px, py);
    }

    // Unity reference diagonal
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), inner.getBottom(), inner.getRight(), inner.getY(), 0.4f);

    // Curve glow
    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(curvePath, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.15f));
    g.strokePath(curvePath, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.85f));
    g.strokePath(curvePath, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved));

    // Threshold marker (vertical line)
    float threshX = dbToX(threshDb);
    g.setColour(accent.withAlpha(0.35f));
    g.drawLine(threshX, inner.getY(), threshX, inner.getBottom(), 1.0f);

    // Hysteresis zone shading
    float hystX = dbToX(threshDb - hystDb);
    g.setColour(accent.withAlpha(0.04f));
    g.fillRect(hystX, inner.getY(), threshX - hystX, h);

    // Threshold label
    g.setColour(accent.withAlpha(0.5f));
    g.setFont(juce::Font(8.0f));
    g.drawText("T", (int)(threshX - 4), (int)inner.getY() + 2, 10, 10,
        juce::Justification::centred);

    // ---- Level meter bar ----
    float meterH = 6.0f;
    float meterY = inner.getY() + h - meterH - 4.0f;
    float envNorm = juce::jlimit(0.0f, 1.0f, (envDb - dbMin) / (dbMax - dbMin));
    float envX = inner.getX() + envNorm * w;

    // Meter background
    g.setColour(juce::Colour(0xff1A2535));
    g.fillRoundedRectangle(inner.getX(), meterY, w, meterH, 2.0f);

    // Meter fill — green when open, red when closed
    juce::Colour meterCol = gg > 0.5f ? gateOpenCol : gateClosedCol;
    g.setColour(meterCol.withAlpha(0.6f));
    g.fillRoundedRectangle(inner.getX(), meterY, juce::jmax(2.0f, envX - inner.getX()), meterH, 2.0f);

    // Gain reduction indicator
    float grBarW = 50.0f;
    float grBarH = h - 20.0f;
    float grBarX = inner.getRight() - grBarW - 4.0f;
    float grBarY = inner.getY() + 4.0f;

    g.setColour(juce::Colour(0xff0D1520));
    g.fillRoundedRectangle(grBarX, grBarY, grBarW, grBarH, 3.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(grBarX, grBarY, grBarW, grBarH, 3.0f, 0.5f);

    // GR fill (from top, inverted — full = no reduction)
    float grFillH = gg * grBarH;
    juce::Colour grCol = gg > 0.8f ? gateOpenCol : gg > 0.3f ? juce::Colour(0xffFBBF24) : gateClosedCol;
    g.setColour(grCol.withAlpha(0.5f));
    g.fillRoundedRectangle(grBarX + 2.0f, grBarY + (grBarH - grFillH),
        grBarW - 4.0f, grFillH, 2.0f);

    g.setColour(textDim.withAlpha(0.4f));
    g.setFont(juce::Font(8.0f));
    g.drawText("GR", (int)grBarX, (int)(grBarY + grBarH + 2), (int)grBarW, 10,
        juce::Justification::centred);

    // dB readout
    g.setColour(textDim.withAlpha(0.4f));
    g.setFont(juce::Font(9.0f));
    float grDb = 20.0f * std::log10(juce::jmax(gg, 1.0e-6f));
    g.drawText(grDb > -0.5f ? "0 dB" : juce::String(grDb, 1) + " dB",
        (int)grBarX, (int)(grBarY + 2), (int)grBarW, 12,
        juce::Justification::centred);
}

inline void GateEditor::resized()
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
        { sldThresh,  lblThresh,  valThresh },
        { sldAttack,  lblAttack,  valAttack },
        { sldHold,    lblHold,    valHold },
        { sldRelease, lblRelease, valRelease },
        { sldRange,   lblRange,   valRange }
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
inline juce::AudioProcessorEditor* NoiseGatePedal::createEditor()
{
    return new GateEditor(*this);
}
