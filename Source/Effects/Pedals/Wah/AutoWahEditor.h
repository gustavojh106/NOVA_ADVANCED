#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class AutoWahPedal;

class AutoWahEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    AutoWahEditor(AutoWahPedal& pedal);
    ~AutoWahEditor() override
    {
        stopTimer();
        sldSens.setLookAndFeel(nullptr);
        sldAttack.setLookAndFeel(nullptr);
        sldDecay.setLookAndFeel(nullptr);
        sldRange.setLookAndFeel(nullptr);
        sldReso.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintSweepViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    AutoWahPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Orange accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ffF97316");
    const juce::Colour accentDim  = juce::Colour::fromString("ffEA580C");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFDBA74");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Knobs (6 total: 2 rows of 3)
    juce::Slider sldSens, sldAttack, sldDecay, sldRange, sldReso, sldMix;
    juce::Label lblSens, lblAttack, lblDecay, lblRange, lblReso, lblMix;
    juce::Label valSens, valAttack, valDecay, valRange, valReso, valMix;

    // Visualization
    juce::Rectangle<float> vizBounds;
    float displayFreq = 200.0f;
    float displayEnv  = 0.0f;

    // Log-frequency to x-position mapping
    static float freqToNorm(float freq)
    {
        constexpr float logMin = 1.69897f;  // log10(50)
        constexpr float logMax = 4.30103f;  // log10(20000)
        float logF = std::log10(juce::jlimit(50.0f, 20000.0f, freq));
        return (logF - logMin) / (logMax - logMin);
    }

    struct WahKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ffF97316");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffFDBA74");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoWahEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline AutoWahEditor::AutoWahEditor(AutoWahPedal& pedal)
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

    initKnob(sldSens,   lblSens,   valSens,   "SENSITIVITY", 0.0f,  1.0f,   0.6f);
    initKnob(sldAttack, lblAttack, valAttack, "ATTACK",      0.5f,  30.0f,  2.0f, 0.1f);
    initKnob(sldDecay,  lblDecay,  valDecay,  "DECAY",       10.0f, 800.0f, 120.0f, 1.0f);
    initKnob(sldRange,  lblRange,  valRange,  "RANGE",       0.0f,  1.0f,   0.72f);
    initKnob(sldReso,   lblReso,   valReso,   "RESONANCE",   0.5f,  10.0f,  3.2f, 0.1f);
    initKnob(sldMix,    lblMix,    valMix,    "MIX",         0.0f,  1.0f,   1.0f);

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
    wireParam(sldSens,   proc.sensitivityParam);
    wireParam(sldAttack, proc.attackParam);
    wireParam(sldDecay,  proc.decayParam);
    wireParam(sldRange,  proc.rangeParam);
    wireParam(sldReso,   proc.resonanceParam);
    wireParam(sldMix,    proc.mixParam);

    startTimerHz(30);
}

inline void AutoWahEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldSens,   proc.sensitivityParam);
    syncSlider(sldAttack, proc.attackParam);
    syncSlider(sldDecay,  proc.decayParam);
    syncSlider(sldRange,  proc.rangeParam);
    syncSlider(sldReso,   proc.resonanceParam);
    syncSlider(sldMix,    proc.mixParam);

    // Value readouts
    valSens.setText(juce::String((int)(sldSens.getValue() * 100.0)) + "%", juce::dontSendNotification);
    {
        float a = (float)sldAttack.getValue();
        valAttack.setText(a < 10.0f ? juce::String(a, 1) + " ms" : juce::String((int)a) + " ms",
            juce::dontSendNotification);
    }
    valDecay.setText(juce::String((int)sldDecay.getValue()) + " ms", juce::dontSendNotification);
    valRange.setText(juce::String((int)(sldRange.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valReso.setText(juce::String((float)sldReso.getValue(), 1) + "x", juce::dontSendNotification);
    valMix.setText(juce::String((int)(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);

    // Smooth display values
    displayFreq += 0.25f * (proc.currentFilterFreq - displayFreq);
    displayEnv  += 0.25f * (proc.currentEnvelope - displayEnv);

    repaint(vizBounds.toNearestInt());
}

inline void AutoWahEditor::paint(juce::Graphics& g)
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
    g.drawText("AUTO WAH", 28, 10, 240, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Envelope Filter", 28, 33, 200, 18, juce::Justification::centredLeft);

    paintSweepViz(g, vizBounds);
}

inline void AutoWahEditor::paintSweepViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();

    const float reso  = (float)sldReso.getValue();
    const float range = (float)sldRange.getValue();
    const float freq  = displayFreq;
    const float env   = displayEnv;

    // Frequency axis grid
    const float gridFreqs[] = { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
    const char* gridLabels[] = { "100", "200", "500", "1k", "2k", "5k", "10k" };
    g.setFont(juce::Font(8.0f));

    for (int i = 0; i < 7; ++i)
    {
        float xNorm = freqToNorm(gridFreqs[i]);
        float x = inner.getX() + xNorm * w;
        g.setColour(juce::Colour(0xff1A2535));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.3f));
        g.drawText(gridLabels[i], (int)(x - 15), (int)(inner.getBottom() + 1), 30, 10,
            juce::Justification::centred);
    }

    // Horizontal reference lines
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), inner.getY() + h * 0.5f, inner.getRight(), inner.getY() + h * 0.5f, 0.5f);

    // ---- Sweep range visualization ----
    // Show the min and max frequency range as a shaded zone
    constexpr float kMinFreq = 200.0f;
    float maxFreq = std::exp(5.298317f + range * (8.613531f - 5.298317f));
    float minNorm = freqToNorm(kMinFreq);
    float maxNorm = freqToNorm(juce::jlimit(200.0f, 20000.0f, maxFreq));
    float minX = inner.getX() + minNorm * w;
    float maxX = inner.getX() + maxNorm * w;

    // Sweep zone shading
    g.setColour(accent.withAlpha(0.04f));
    g.fillRect(minX, inner.getY(), maxX - minX, h);

    // Sweep zone edges
    g.setColour(accent.withAlpha(0.15f));
    g.drawLine(minX, inner.getY(), minX, inner.getBottom(), 0.5f);
    g.drawLine(maxX, inner.getY(), maxX, inner.getBottom(), 0.5f);

    // ---- Resonant peak shape ----
    // Draw the SVF bandpass/lowpass response shape centered on current freq
    juce::Path peakPath;
    constexpr int numPts = 200;
    float q = juce::jmax(0.5f, reso);
    float bpBlend = juce::jlimit(0.0f, 1.0f, (reso - 1.0f) / 6.0f);

    for (int i = 0; i < numPts; ++i)
    {
        float t = (float)i / (float)(numPts - 1);
        float logF = 1.69897f + t * (4.30103f - 1.69897f);
        float f = std::pow(10.0f, logF);

        // Approximate SVF magnitude response
        float ratio = f / juce::jmax(80.0f, freq);
        float logRatio = std::log2(ratio);

        // LP response (rolls off above center)
        float lpMag = 1.0f / std::sqrt(1.0f + std::pow(ratio, 4.0f));

        // BP response (peak at center)
        float bpDenom = 1.0f + q * q * logRatio * logRatio * 4.0f;
        float bpMag = q / bpDenom;

        // Blend
        float mag = lpMag * (1.0f - bpBlend) + bpMag * bpBlend;

        // To dB, normalized for display
        float magDb = 20.0f * std::log10(juce::jmax(mag, 1.0e-6f));
        constexpr float dbRange = 24.0f;
        float yNorm = juce::jlimit(0.0f, 1.0f, 1.0f - (magDb + dbRange) / (2.0f * dbRange));

        float px = inner.getX() + t * w;
        float py = inner.getY() + yNorm * h;

        if (i == 0)
            peakPath.startNewSubPath(px, py);
        else
            peakPath.lineTo(px, py);
    }

    // Filled area under peak
    {
        juce::Path filled(peakPath);
        filled.lineTo(inner.getRight(), inner.getBottom());
        filled.lineTo(inner.getX(), inner.getBottom());
        filled.closeSubPath();

        juce::ColourGradient fillGrad(accent.withAlpha(0.12f), inner.getX(), inner.getY(),
            accent.withAlpha(0.01f), inner.getX(), inner.getBottom(), false);
        g.setGradientFill(fillGrad);
        g.fillPath(filled);
    }

    // Peak curve
    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(peakPath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.8f));
    g.strokePath(peakPath, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved));

    // ---- Current frequency marker (moves with envelope) ----
    float freqNorm = freqToNorm(juce::jlimit(50.0f, 20000.0f, freq));
    float freqX = inner.getX() + freqNorm * w;

    // Glowing vertical line at current frequency
    {
        juce::ColourGradient glow(accentGlow.withAlpha(0.3f), freqX, inner.getY(),
            accent.withAlpha(0.0f), freqX - 20.0f, inner.getCentreY(), false);
        g.setGradientFill(glow);
        g.fillRect(freqX - 20.0f, inner.getY(), 20.0f, h);
    }
    {
        juce::ColourGradient glow(accentGlow.withAlpha(0.3f), freqX, inner.getY(),
            accent.withAlpha(0.0f), freqX + 20.0f, inner.getCentreY(), false);
        g.setGradientFill(glow);
        g.fillRect(freqX, inner.getY(), 20.0f, h);
    }
    g.setColour(accentGlow.withAlpha(0.9f));
    g.drawLine(freqX, inner.getY(), freqX, inner.getBottom(), 2.0f);

    // Frequency readout at top of marker
    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.setColour(accentGlow);
    juce::String freqStr = freq >= 1000.0f
        ? juce::String(freq * 0.001f, 1) + " kHz"
        : juce::String((int)freq) + " Hz";
    g.drawText(freqStr, (int)(freqX - 30), (int)inner.getY() + 3, 60, 14,
        juce::Justification::centred);

    // ---- Envelope level bar ----
    float envBarH = 4.0f;
    float envBarY = inner.getBottom() - envBarH - 2.0f;
    float envBarW = juce::jlimit(0.0f, w, env * w);

    g.setColour(juce::Colour(0xff1A2535));
    g.fillRoundedRectangle(inner.getX(), envBarY, w, envBarH, 2.0f);

    juce::Colour envCol = env > 0.7f ? juce::Colour(0xffFBBF24)
        : env > 0.3f ? accent
        : accent.withAlpha(0.5f);
    g.setColour(envCol.withAlpha(0.7f));
    g.fillRoundedRectangle(inner.getX(), envBarY, juce::jmax(2.0f, envBarW), envBarH, 2.0f);

    // Character label
    g.setFont(juce::Font(9.0f));
    g.setColour(textDim.withAlpha(0.4f));
    const char* character = reso < 2.0f ? "Warm sweep"
        : reso < 4.0f ? "Funky"
        : reso < 7.0f ? "Quack"
        : "Synth wah";
    g.drawText(character, (int)(inner.getRight() - 70), (int)inner.getY() + 3, 65, 12,
        juce::Justification::centredRight);
}

inline void AutoWahEditor::resized()
{
    const int w = getWidth();

    vizBounds = juce::Rectangle<float>(28.0f, 58.0f, (float)(w - 56), 140.0f);

    // 6 knobs in 2 rows of 3
    const int knobSize = 74;
    const int labelH = 16;
    const int valH = 16;
    const int rowSpacing = knobSize + labelH + valH + 12;

    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup topRow[] = {
        { sldSens,   lblSens,   valSens },
        { sldAttack, lblAttack, valAttack },
        { sldDecay,  lblDecay,  valDecay }
    };
    KnobGroup bottomRow[] = {
        { sldRange, lblRange, valRange },
        { sldReso,  lblReso,  valReso },
        { sldMix,   lblMix,   valMix }
    };

    const int topY = 214;
    const int slotW = w / 3;
    for (int i = 0; i < 3; ++i)
    {
        int cx = slotW * i + slotW / 2;
        topRow[i].lbl.setBounds(cx - 50, topY, 100, labelH);
        topRow[i].s.setBounds(cx - knobSize / 2, topY + labelH + 2, knobSize, knobSize);
        topRow[i].val.setBounds(cx - 50, topY + labelH + 2 + knobSize + 2, 100, valH);
    }

    const int bottomY = topY + rowSpacing;
    for (int i = 0; i < 3; ++i)
    {
        int cx = slotW * i + slotW / 2;
        bottomRow[i].lbl.setBounds(cx - 50, bottomY, 100, labelH);
        bottomRow[i].s.setBounds(cx - knobSize / 2, bottomY + labelH + 2, knobSize, knobSize);
        bottomRow[i].val.setBounds(cx - 50, bottomY + labelH + 2 + knobSize + 2, 100, valH);
    }
}

// ---- Wire createEditor ----
inline juce::AudioProcessorEditor* AutoWahPedal::createEditor()
{
    return new AutoWahEditor(*this);
}
