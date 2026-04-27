#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class PhaserPedal;

class PhaserEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    PhaserEditor(PhaserPedal& pedal);
    ~PhaserEditor() override
    {
        stopTimer();
        sldRate.setLookAndFeel(nullptr);
        sldDepth.setLookAndFeel(nullptr);
        sldFeedback.setLookAndFeel(nullptr);
        sldStages.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintSweepViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    PhaserPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Purple accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ffC084FC");
    const juce::Colour accentDim  = juce::Colour::fromString("ff9333EA");
    const juce::Colour accentGlow = juce::Colour::fromString("ffD8B4FE");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Knobs
    juce::Slider sldRate, sldDepth, sldFeedback, sldStages, sldMix;
    juce::Label lblRate, lblDepth, lblFeedback, lblStages, lblMix;
    juce::Label valRate, valDepth, valFeedback, valStages, valMix;

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

    struct PhaserKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ffC084FC");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffD8B4FE");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhaserEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline PhaserEditor::PhaserEditor(PhaserPedal& pedal)
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

    initKnob(sldRate,     lblRate,     valRate,     "RATE",     0.05f, 8.0f,  0.65f);
    initKnob(sldDepth,    lblDepth,    valDepth,    "DEPTH",    0.0f,  1.0f,  0.72f);
    initKnob(sldFeedback, lblFeedback, valFeedback, "FEEDBACK", -0.85f, 0.85f, 0.45f);
    initKnob(sldStages,   lblStages,   valStages,   "STAGES",   2.0f,  12.0f, 6.0f, 1.0f);
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
    wireParam(sldStages,   proc.stagesParam);
    wireParam(sldMix,      proc.mixParam);

    startTimerHz(30);
}

inline void PhaserEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldRate,     proc.rateParam);
    syncSlider(sldDepth,    proc.depthParam);
    syncSlider(sldFeedback, proc.feedbackParam);
    syncSlider(sldStages,   proc.stagesParam);
    syncSlider(sldMix,      proc.mixParam);

    // Value readouts
    valRate.setText(juce::String((float)sldRate.getValue(), 2) + " Hz", juce::dontSendNotification);
    valDepth.setText(juce::String((int)(sldDepth.getValue() * 100.0)) + "%", juce::dontSendNotification);
    {
        int fbPct = juce::roundToInt(sldFeedback.getValue() * 100.0);
        valFeedback.setText((fbPct >= 0 ? "+" : "") + juce::String(fbPct) + "%", juce::dontSendNotification);
    }
    valStages.setText(juce::String((int)sldStages.getValue()) + " stg", juce::dontSendNotification);
    valMix.setText(juce::String((int)(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void PhaserEditor::paint(juce::Graphics& g)
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
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("PHASER", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("Vortex", 28, 33, 200, 18, juce::Justification::centredLeft);

    // Sweep visualization
    paintSweepViz(g, vizBounds);
}

inline void PhaserEditor::paintSweepViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Panel background
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float midY = inner.getCentreY();

    const float depth    = (float)sldDepth.getValue();
    const float feedback = (float)sldFeedback.getValue();
    const int numStages  = (int)sldStages.getValue();
    const float phase    = proc.lfoPhase;

    constexpr float kMinFreq = 80.0f;
    constexpr float kMaxFreq = 5500.0f;
    constexpr float kStagger = 0.12f;
    constexpr float twoPi = juce::MathConstants<float>::twoPi;

    // Compute LFO value (tri-sine blend, same as processor)
    auto computeLfo = [twoPi](float p) -> float
    {
        float tri = (p < 0.5f) ? (p * 4.0f - 1.0f) : (3.0f - p * 4.0f);
        float sine = std::sin(twoPi * p);
        return (tri * 0.6f + sine * 0.4f) * 0.5f + 0.5f;
    };

    float lfoL = computeLfo(phase);
    float lfoR = computeLfo(std::fmod(phase + 0.25f, 1.0f));

    float baseFreqL = kMinFreq + (kMaxFreq - kMinFreq) * lfoL * depth;
    float baseFreqR = kMinFreq + (kMaxFreq - kMinFreq) * lfoR * depth;

    // Frequency axis grid lines
    const float gridFreqs[] = { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
    const char* gridLabels[] = { "100", "200", "500", "1k", "2k", "5k", "10k" };
    g.setFont(juce::Font(juce::FontOptions(8.0f)));

    for (int i = 0; i < 7; ++i)
    {
        float xNorm = freqToNorm(gridFreqs[i]);
        float x = inner.getX() + xNorm * w;

        g.setColour(juce::Colour(0xff1A2535));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);

        g.setColour(textDim.withAlpha(0.3f));
        g.drawText(gridLabels[i], (int)(x - 15), (int)(inner.getBottom() + 1), 30, 10, juce::Justification::centred);
    }

    // Center line
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), midY, inner.getRight(), midY, 0.5f);

    float fbIntensity = std::abs(feedback);

    // Draw notch markers for each stage
    for (int stage = 0; stage < numStages && stage < 12; ++stage)
    {
        float stagger = 1.0f + (float)stage * kStagger;

        // L channel notch
        float freqL = juce::jlimit(50.0f, 20000.0f, baseFreqL * stagger);
        float xNormL = freqToNorm(freqL);
        float xL = inner.getX() + xNormL * w;

        // Marker height: taller with more feedback (resonance)
        float markerH = h * (0.3f + fbIntensity * 0.5f);
        float alpha = 0.5f + fbIntensity * 0.4f;

        // L marker (bright)
        {
            juce::ColourGradient glow(accentGlow.withAlpha(alpha * 0.15f), xL, midY - markerH * 0.5f,
                accent.withAlpha(0.0f), xL, midY, false);
            g.setGradientFill(glow);
            g.fillRect(xL - 6.0f, midY - markerH * 0.5f, 12.0f, markerH * 0.5f);
        }
        {
            juce::ColourGradient glow(accentGlow.withAlpha(alpha * 0.15f), xL, midY + markerH * 0.5f,
                accent.withAlpha(0.0f), xL, midY, false);
            g.setGradientFill(glow);
            g.fillRect(xL - 6.0f, midY, 12.0f, markerH * 0.5f);
        }
        g.setColour(accentGlow.withAlpha(alpha * 0.8f));
        g.drawLine(xL, midY - markerH * 0.5f, xL, midY + markerH * 0.5f, 1.5f);

        // Dot at center
        g.setColour(accentGlow.withAlpha(alpha));
        g.fillEllipse(xL - 2.5f, midY - 2.5f, 5.0f, 5.0f);

        // R channel notch (dimmer, offset)
        float freqR = juce::jlimit(50.0f, 20000.0f, baseFreqR * stagger);
        float xNormR = freqToNorm(freqR);
        float xR = inner.getX() + xNormR * w;

        float rAlpha = alpha * 0.45f;
        g.setColour(accent.withAlpha(rAlpha * 0.7f));
        g.drawLine(xR, midY - markerH * 0.35f, xR, midY + markerH * 0.35f, 1.0f);
        g.setColour(accent.withAlpha(rAlpha));
        g.fillEllipse(xR - 2.0f, midY - 2.0f, 4.0f, 4.0f);
    }

    // L/R legend
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.setColour(accentGlow.withAlpha(0.5f));
    g.drawText("L", (int)inner.getX() + 2, (int)inner.getY() + 2, 12, 12, juce::Justification::centredLeft);
    g.setColour(accent.withAlpha(0.35f));
    g.drawText("R", (int)inner.getX() + 14, (int)inner.getY() + 2, 12, 12, juce::Justification::centredLeft);
}

inline void PhaserEditor::resized()
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
        { sldStages,   lblStages,   valStages },
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
inline juce::AudioProcessorEditor* PhaserPedal::createEditor()
{
    return new PhaserEditor(*this);
}
