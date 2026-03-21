#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class DelayPedal;

class DelayEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    DelayEditor(DelayPedal& pedal);
    ~DelayEditor() override
    {
        stopTimer();
        sldTime.setLookAndFeel(nullptr);
        sldFeedback.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldSpread.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintEchoViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    DelayPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Blue accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ff60A5FA");
    const juce::Colour accentDim  = juce::Colour::fromString("ff3B82F6");
    const juce::Colour accentGlow = juce::Colour::fromString("ff93C5FD");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Knobs
    juce::Slider sldTime, sldFeedback, sldTone, sldSpread, sldMix;
    juce::Label lblTime, lblFeedback, lblTone, lblSpread, lblMix;
    juce::Label valTime, valFeedback, valTone, valSpread, valMix;

    // Visualization
    juce::Rectangle<float> vizBounds;
    float animPhase = 0.0f;

    struct DelayKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ff60A5FA");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff93C5FD");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DelayEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline DelayEditor::DelayEditor(DelayPedal& pedal)
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

    initKnob(sldTime,     lblTime,     valTime,     "TIME",     40.0f, 1400.0f, 420.0f, 1.0f);
    initKnob(sldFeedback, lblFeedback, valFeedback, "FEEDBACK", 0.0f,  0.95f,   0.42f);
    initKnob(sldTone,     lblTone,     valTone,     "TONE",     600.0f, 12000.0f, 4800.0f, 1.0f);
    initKnob(sldSpread,   lblSpread,   valSpread,   "SPREAD",   0.0f,  1.0f,    0.42f);
    initKnob(sldMix,      lblMix,      valMix,      "MIX",      0.0f,  1.0f,    0.3f);

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
    wireParam(sldTime,     proc.timeParam);
    wireParam(sldFeedback, proc.feedbackParam);
    wireParam(sldTone,     proc.toneParam);
    wireParam(sldSpread,   proc.spreadParam);
    wireParam(sldMix,      proc.mixParam);

    startTimerHz(30);
}

inline void DelayEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldTime,     proc.timeParam);
    syncSlider(sldFeedback, proc.feedbackParam);
    syncSlider(sldTone,     proc.toneParam);
    syncSlider(sldSpread,   proc.spreadParam);
    syncSlider(sldMix,      proc.mixParam);

    // Value readouts
    {
        float ms = (float)sldTime.getValue();
        if (ms >= 1000.0f)
            valTime.setText(juce::String(ms * 0.001f, 2) + " s", juce::dontSendNotification);
        else
            valTime.setText(juce::String((int)ms) + " ms", juce::dontSendNotification);
    }
    valFeedback.setText(juce::String((int)(sldFeedback.getValue() / 0.95f * 100.0f)) + "%", juce::dontSendNotification);
    {
        float hz = (float)sldTone.getValue();
        if (hz >= 1000.0f)
            valTone.setText(juce::String(hz * 0.001f, 1) + " kHz", juce::dontSendNotification);
        else
            valTone.setText(juce::String((int)hz) + " Hz", juce::dontSendNotification);
    }
    {
        float sp = (float)sldSpread.getValue();
        juce::String mode = sp < 0.3f ? "Mono" : (sp < 0.7f ? "Stereo" : "Ping-Pong");
        valSpread.setText(mode, juce::dontSendNotification);
    }
    valMix.setText(juce::String((int)(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);

    // Advance animation phase
    animPhase += 0.03f;
    if (animPhase >= 1.0f) animPhase -= 1.0f;

    repaint(vizBounds.toNearestInt());
}

inline void DelayEditor::paint(juce::Graphics& g)
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
    g.drawText("DELAY", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Orbit", 28, 33, 200, 18, juce::Justification::centredLeft);

    // Echo visualization
    paintEchoViz(g, vizBounds);
}

inline void DelayEditor::paintEchoViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Panel background
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float baseY = inner.getBottom();
    const float midX = inner.getX();

    const float feedback = (float)sldFeedback.getValue();
    const float spread   = (float)sldSpread.getValue();
    const float timeMs   = (float)sldTime.getValue();
    const float toneNorm = ((float)sldTone.getValue() - 600.0f) / 11400.0f; // 0-1

    // Number of visible taps (show until amplitude < 5%)
    const int maxTaps = 10;
    int numTaps = 0;
    for (int i = 1; i <= maxTaps; ++i)
    {
        if (std::pow(feedback, (float)i) < 0.05f) break;
        numTaps = i;
    }
    if (numTaps < 2) numTaps = 2;

    // Total time span for visualization
    const float totalMs = timeMs * (float)(numTaps + 1);
    const float barWidth = juce::jmin(w / (float)(numTaps + 2) * 0.45f, 24.0f);

    // Center line
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), baseY - h * 0.5f, inner.getRight(), baseY - h * 0.5f, 0.5f);

    // Draw echo taps
    for (int tap = 1; tap <= numTaps; ++tap)
    {
        float tapMs = timeMs * (float)tap;
        float x = midX + (tapMs / totalMs) * w;
        float amplitude = std::pow(feedback, (float)tap);

        // Tone darkening effect: each repeat loses highs, shown as alpha reduction
        float toneFade = 1.0f - (1.0f - toneNorm) * (float)tap * 0.08f;
        toneFade = juce::jmax(0.3f, toneFade);

        float barH = h * 0.85f * amplitude;
        float alpha = amplitude * toneFade;

        // Ping-pong: odd taps lean left, even taps lean right
        float pingPongOffset = 0.0f;
        if (spread > 0.3f)
        {
            float ppAmount = juce::jmin(1.0f, (spread - 0.3f) / 0.7f);
            pingPongOffset = (tap % 2 == 0 ? 1.0f : -1.0f) * barWidth * 0.8f * ppAmount;
        }

        float barX = x + pingPongOffset - barWidth * 0.5f;
        float barY = baseY - barH;

        // Glow behind bar
        {
            juce::ColourGradient glow(accent.withAlpha(alpha * 0.15f), barX + barWidth * 0.5f, barY,
                accent.withAlpha(0.0f), barX + barWidth * 0.5f, baseY, false);
            g.setGradientFill(glow);
            g.fillRoundedRectangle(barX - 4.0f, barY, barWidth + 8.0f, barH, 3.0f);
        }

        // Bar body
        {
            juce::ColourGradient barGrad(accentGlow.withAlpha(alpha * 0.9f), barX, barY,
                accent.withAlpha(alpha * 0.5f), barX, baseY, false);
            g.setGradientFill(barGrad);
            g.fillRoundedRectangle(barX, barY, barWidth, barH, 2.0f);
        }

        // Bar border
        g.setColour(accent.withAlpha(alpha * 0.4f));
        g.drawRoundedRectangle(barX, barY, barWidth, barH, 2.0f, 0.8f);

        // Tap number
        if (barWidth > 10.0f)
        {
            g.setColour(textDim.withAlpha(alpha * 0.6f));
            g.setFont(juce::Font(8.0f));
            g.drawText(juce::String(tap), (int)(barX), (int)(baseY + 2.0f),
                (int)barWidth, 10, juce::Justification::centred);
        }

        // L/R labels for ping-pong
        if (spread > 0.5f && tap <= 4)
        {
            g.setFont(juce::Font(7.0f, juce::Font::bold));
            g.setColour(accent.withAlpha(alpha * 0.4f));
            const char* ch = (tap % 2 == 1) ? "L" : "R";
            g.drawText(ch, (int)(barX), (int)(barY - 10.0f),
                (int)barWidth, 10, juce::Justification::centred);
        }
    }

    // "DRY" label at origin
    g.setFont(juce::Font(8.0f, juce::Font::bold));
    g.setColour(textDim.withAlpha(0.5f));
    g.drawText("DRY", (int)inner.getX(), (int)(baseY + 2.0f), 28, 10, juce::Justification::centredLeft);

    // Time axis label
    g.setFont(juce::Font(8.0f));
    g.setColour(textDim.withAlpha(0.35f));
    juce::String timeLabel;
    if (totalMs >= 1000.0f)
        timeLabel = juce::String(totalMs * 0.001f, 1) + "s";
    else
        timeLabel = juce::String((int)totalMs) + "ms";
    g.drawText(timeLabel, (int)inner.getRight() - 40, (int)(baseY + 2.0f), 40, 10, juce::Justification::centredRight);

    // Animated pulse on first tap (subtle glow)
    if (numTaps >= 1)
    {
        float pulseAlpha = (std::sin(animPhase * juce::MathConstants<float>::twoPi) + 1.0f) * 0.5f;
        float firstX = midX + (timeMs / totalMs) * w;
        g.setColour(accentGlow.withAlpha(0.08f * pulseAlpha));
        g.fillEllipse(firstX - 16.0f, baseY - h * 0.85f * feedback - 16.0f, 32.0f, 32.0f);
    }
}

inline void DelayEditor::resized()
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
        { sldTime,     lblTime,     valTime },
        { sldFeedback, lblFeedback, valFeedback },
        { sldTone,     lblTone,     valTone },
        { sldSpread,   lblSpread,   valSpread },
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
inline juce::AudioProcessorEditor* DelayPedal::createEditor()
{
    return new DelayEditor(*this);
}
