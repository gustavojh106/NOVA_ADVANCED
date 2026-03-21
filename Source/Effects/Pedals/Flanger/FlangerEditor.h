#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class FlangerPedal;

class FlangerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    FlangerEditor(FlangerPedal& pedal);
    ~FlangerEditor() override
    {
        stopTimer();
        sldRate.setLookAndFeel(nullptr);
        sldDepth.setLookAndFeel(nullptr);
        sldFeedback.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintCombViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    FlangerPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Sky blue accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ff38BDF8");
    const juce::Colour accentDim  = juce::Colour::fromString("ff0284C7");
    const juce::Colour accentGlow = juce::Colour::fromString("ff7DD3FC");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Knobs
    juce::Slider sldRate, sldDepth, sldFeedback, sldTone, sldMix;
    juce::Label lblRate, lblDepth, lblFeedback, lblTone, lblMix;
    juce::Label valRate, valDepth, valFeedback, valTone, valMix;

    // Visualization
    juce::Rectangle<float> vizBounds;

    // Log-frequency to x-position mapping
    static float freqToNorm(float freq)
    {
        constexpr float logMin = 1.69897f;  // log10(50)
        constexpr float logMax = 4.30103f;  // log10(20000)
        float logF = std::log10(juce::jlimit(50.0f, 20000.0f, freq));
        return (logF - logMin) / (logMax - logMin);
    }

    struct FlangerKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ff38BDF8");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff7DD3FC");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FlangerEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline FlangerEditor::FlangerEditor(FlangerPedal& pedal)
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

    initKnob(sldRate,     lblRate,     valRate,     "RATE",     0.05f, 5.0f,   0.35f);
    initKnob(sldDepth,    lblDepth,    valDepth,    "DEPTH",    0.0f,  1.0f,   0.64f);
    initKnob(sldFeedback, lblFeedback, valFeedback, "FEEDBACK", -0.95f, 0.95f, 0.55f);
    initKnob(sldTone,     lblTone,     valTone,     "TONE",     1000.0f, 14000.0f, 8000.0f, 1.0f);
    initKnob(sldMix,      lblMix,      valMix,      "MIX",      0.0f,  1.0f,  0.5f);

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
    wireParam(sldRate,     proc.rateParam);
    wireParam(sldDepth,    proc.depthParam);
    wireParam(sldFeedback, proc.feedbackParam);
    wireParam(sldTone,     proc.toneParam);
    wireParam(sldMix,      proc.mixParam);

    startTimerHz(30);
}

inline void FlangerEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldRate,     proc.rateParam);
    syncSlider(sldDepth,    proc.depthParam);
    syncSlider(sldFeedback, proc.feedbackParam);
    syncSlider(sldTone,     proc.toneParam);
    syncSlider(sldMix,      proc.mixParam);

    // Value readouts
    valRate.setText(juce::String((float)sldRate.getValue(), 2) + " Hz", juce::dontSendNotification);
    valDepth.setText(juce::String((int)(sldDepth.getValue() * 100.0)) + "%", juce::dontSendNotification);
    {
        int fbPct = juce::roundToInt(sldFeedback.getValue() * 100.0);
        valFeedback.setText((fbPct >= 0 ? "+" : "") + juce::String(fbPct) + "%", juce::dontSendNotification);
    }
    {
        float tone = (float)sldTone.getValue();
        valTone.setText(tone >= 1000.0f
            ? juce::String(tone * 0.001f, 1) + " kHz"
            : juce::String((int)tone) + " Hz", juce::dontSendNotification);
    }
    valMix.setText(juce::String((int)(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void FlangerEditor::paint(juce::Graphics& g)
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
    g.drawText("FLANGER", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Jet Engine", 28, 33, 200, 18, juce::Justification::centredLeft);

    // Comb filter visualization
    paintCombViz(g, vizBounds);
}

inline void FlangerEditor::paintCombViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Panel background
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();

    const float depth    = (float)sldDepth.getValue();
    const float feedback = (float)sldFeedback.getValue();
    const float phase    = proc.lfoPhase;

    constexpr float kBaseDelayMs  = 1.0f;
    constexpr float kMaxExcursMs  = 7.0f;
    constexpr float twoPi = juce::MathConstants<float>::twoPi;

    // Compute current delay for L/R channels using same LFO as processor
    float lfoL = 0.5f + 0.5f * std::sin(twoPi * phase);
    float lfoR = 0.5f + 0.5f * std::sin(twoPi * std::fmod(phase + 0.333f, 1.0f));

    float delayMsL = kBaseDelayMs + kMaxExcursMs * depth * lfoL;
    float delayMsR = kBaseDelayMs + kMaxExcursMs * depth * lfoR;

    // Frequency axis grid lines
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

    // dB axis reference lines
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), inner.getY() + h * 0.25f, inner.getRight(), inner.getY() + h * 0.25f, 0.5f);
    g.drawLine(inner.getX(), inner.getY() + h * 0.5f,  inner.getRight(), inner.getY() + h * 0.5f,  0.5f);
    g.drawLine(inner.getX(), inner.getY() + h * 0.75f, inner.getRight(), inner.getY() + h * 0.75f, 0.5f);

    // Compute comb filter magnitude response: |H(f)| = 1 / sqrt(1 + fb^2 + 2*fb*cos(2*pi*f*delay))
    // For display, we convert to dB and normalize
    auto computeCombResponse = [&](float delayMs, int numPoints, juce::Path& path, bool startPath)
    {
        const float delaySec = delayMs * 0.001f;
        const float fb = feedback;

        for (int i = 0; i < numPoints; ++i)
        {
            float xNorm = (float)i / (float)(numPoints - 1);
            float logFreq = 1.69897f + xNorm * (4.30103f - 1.69897f);  // log10(50) to log10(20000)
            float freq = std::pow(10.0f, logFreq);

            // Comb filter transfer function magnitude
            float phase_angle = twoPi * freq * delaySec;
            float mag2 = 1.0f + fb * fb + 2.0f * fb * std::cos(phase_angle);
            float magDb = 10.0f * std::log10(juce::jmax(mag2, 1.0e-6f));

            // Map dB to y: 0 dB at center, +/-12 dB range
            constexpr float dbRange = 14.0f;
            float yNorm = 0.5f - (magDb / (2.0f * dbRange));
            yNorm = juce::jlimit(0.0f, 1.0f, yNorm);

            float x = inner.getX() + xNorm * w;
            float y = inner.getY() + yNorm * h;

            if (i == 0 && startPath)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }
    };

    // L channel response (bright)
    {
        juce::Path pathL;
        computeCombResponse(delayMsL, 256, pathL, true);

        // Glow
        g.setColour(accentGlow.withAlpha(0.06f));
        g.strokePath(pathL, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
        g.setColour(accent.withAlpha(0.12f));
        g.strokePath(pathL, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
        g.setColour(accentGlow.withAlpha(0.85f));
        g.strokePath(pathL, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved));
    }

    // R channel response (dimmer)
    {
        juce::Path pathR;
        computeCombResponse(delayMsR, 256, pathR, true);

        g.setColour(accent.withAlpha(0.05f));
        g.strokePath(pathR, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
        g.setColour(accent.withAlpha(0.45f));
        g.strokePath(pathR, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved));
    }

    // L/R legend
    g.setFont(juce::Font(9.0f, juce::Font::bold));
    g.setColour(accentGlow.withAlpha(0.5f));
    g.drawText("L", (int)inner.getX() + 2, (int)inner.getY() + 2, 12, 12, juce::Justification::centredLeft);
    g.setColour(accent.withAlpha(0.35f));
    g.drawText("R", (int)inner.getX() + 14, (int)inner.getY() + 2, 12, 12, juce::Justification::centredLeft);

    // Delay readout
    g.setFont(juce::Font(9.0f));
    g.setColour(textDim.withAlpha(0.5f));
    g.drawText(juce::String(delayMsL, 1) + " ms",
        (int)(inner.getRight() - 60), (int)inner.getY() + 2, 56, 12,
        juce::Justification::centredRight);
}

inline void FlangerEditor::resized()
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
        { sldRate,     lblRate,     valRate },
        { sldDepth,    lblDepth,    valDepth },
        { sldFeedback, lblFeedback, valFeedback },
        { sldTone,     lblTone,     valTone },
        { sldMix,      lblMix,      valMix }
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
inline juce::AudioProcessorEditor* FlangerPedal::createEditor()
{
    return new FlangerEditor(*this);
}
