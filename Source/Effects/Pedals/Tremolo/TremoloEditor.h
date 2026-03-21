#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class TremoloPedal;

class TremoloEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    TremoloEditor(TremoloPedal& pedal);
    ~TremoloEditor() override
    {
        stopTimer();
        sldRate.setLookAndFeel(nullptr);
        sldDepth.setLookAndFeel(nullptr);
        sldShape.setLookAndFeel(nullptr);
        sldStereo.setLookAndFeel(nullptr);
        sldHarmonic.setLookAndFeel(nullptr);
        sldLevel.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintLfoViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    TremoloPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Yellow accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ffFBBF24");
    const juce::Colour accentDim  = juce::Colour::fromString("ffD97706");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFDE68A");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    juce::Slider sldRate, sldDepth, sldShape, sldStereo, sldHarmonic, sldLevel;
    juce::Label lblRate, lblDepth, lblShape, lblStereo, lblHarmonic, lblLevel;
    juce::Label valRate, valDepth, valShape, valStereo, valHarmonic, valLevel;

    juce::Rectangle<float> vizBounds;

    struct TremoloKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ffFBBF24");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffFDE68A");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TremoloEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline TremoloEditor::TremoloEditor(TremoloPedal& pedal)
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

    initKnob(sldRate,     lblRate,     valRate,     "RATE",     0.5f, 15.0f, 4.5f, 0.1f);
    initKnob(sldDepth,    lblDepth,    valDepth,    "DEPTH",    0.0f, 1.0f,  0.65f);
    initKnob(sldShape,    lblShape,    valShape,    "SHAPE",    0.0f, 1.0f,  0.3f);
    initKnob(sldStereo,   lblStereo,   valStereo,   "STEREO",   0.0f, 1.0f,  0.0f);
    initKnob(sldHarmonic, lblHarmonic, valHarmonic, "HARMONIC", 0.0f, 1.0f,  0.0f);
    initKnob(sldLevel,    lblLevel,    valLevel,    "LEVEL",    0.5f, 2.0f,  1.0f);

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
    wireParam(sldShape,    proc.shapeParam);
    wireParam(sldStereo,   proc.stereoParam);
    wireParam(sldHarmonic, proc.harmonicParam);
    wireParam(sldLevel,    proc.levelParam);

    startTimerHz(30);
}

inline void TremoloEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldRate,     proc.rateParam);
    syncSlider(sldDepth,    proc.depthParam);
    syncSlider(sldShape,    proc.shapeParam);
    syncSlider(sldStereo,   proc.stereoParam);
    syncSlider(sldHarmonic, proc.harmonicParam);
    syncSlider(sldLevel,    proc.levelParam);

    valRate.setText(juce::String((float)sldRate.getValue(), 1) + " Hz", juce::dontSendNotification);
    valDepth.setText(juce::String((int)(sldDepth.getValue() * 100.0)) + "%", juce::dontSendNotification);
    {
        float sh = (float)sldShape.getValue();
        const char* shapeName = sh < 0.33f ? "Sine" : sh < 0.66f ? "Triangle" : "Square";
        valShape.setText(shapeName, juce::dontSendNotification);
    }
    valStereo.setText(juce::String((int)(sldStereo.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valHarmonic.setText(juce::String((int)(sldHarmonic.getValue() * 100.0)) + "%", juce::dontSendNotification);
    {
        float lv = (float)sldLevel.getValue();
        float db = 20.0f * std::log10(juce::jmax(lv, 0.001f));
        valLevel.setText((db >= 0 ? "+" : "") + juce::String(db, 1) + " dB", juce::dontSendNotification);
    }

    repaint(vizBounds.toNearestInt());
}

inline void TremoloEditor::paint(juce::Graphics& g)
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
    g.drawText("TREMOLO", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    {
        float harm = (float)sldHarmonic.getValue();
        g.drawText(harm > 0.5f ? "Harmonic" : "Pulse", 28, 33, 200, 18, juce::Justification::centredLeft);
    }

    paintLfoViz(g, vizBounds);
}

inline void TremoloEditor::paintLfoViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float midY = inner.getCentreY();

    const float shape   = (float)sldShape.getValue();
    const float depth   = (float)sldDepth.getValue();
    const float stereo  = (float)sldStereo.getValue();
    const float harm    = (float)sldHarmonic.getValue();
    const float phase   = proc.lfoPhase;

    // Center line
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), midY, inner.getRight(), midY, 0.5f);

    // Draw 2 cycles of the LFO waveform for L and R
    constexpr int numPts = 300;

    auto drawWaveform = [&](float stereoOffset, juce::Colour col, float thickness)
    {
        juce::Path path;
        for (int i = 0; i < numPts; ++i)
        {
            float t = (float)i / (float)(numPts - 1);
            float p = std::fmod(t * 2.0f + stereoOffset * 0.5f, 1.0f); // Show 2 cycles

            float lfo = Nova::TremoloDSP::shapedLfo(p, shape);
            float gain = 1.0f - depth * 0.5f * (1.0f - lfo);

            float px = inner.getX() + t * w;
            float py = midY - (gain - 0.5f) * h * 1.5f;

            if (i == 0)
                path.startNewSubPath(px, py);
            else
                path.lineTo(px, py);
        }

        g.setColour(col.withAlpha(0.06f));
        g.strokePath(path, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
        g.setColour(col.withAlpha(0.7f));
        g.strokePath(path, juce::PathStrokeType(thickness, juce::PathStrokeType::curved));
    };

    // L channel (bright)
    drawWaveform(0.0f, accentGlow, 1.5f);

    // R channel (dimmer, only if stereo > 0)
    if (stereo > 0.01f)
        drawWaveform(stereo, accent.withAlpha(0.5f), 1.0f);

    // Harmonic tremolo indicator: show inverted waveform for high band
    if (harm > 0.1f)
    {
        juce::Path hiPath;
        for (int i = 0; i < numPts; ++i)
        {
            float t = (float)i / (float)(numPts - 1);
            float p = std::fmod(t * 2.0f, 1.0f);
            float lfo = Nova::TremoloDSP::shapedLfo(p, shape);
            float gain = 1.0f - depth * 0.5f * (1.0f + lfo);  // Inverted

            float px = inner.getX() + t * w;
            float py = midY - (gain - 0.5f) * h * 1.5f;

            if (i == 0)
                hiPath.startNewSubPath(px, py);
            else
                hiPath.lineTo(px, py);
        }

        g.setColour(juce::Colour(0xff60A5FA).withAlpha(harm * 0.4f));
        g.strokePath(hiPath, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved));

        // Labels
        g.setFont(juce::Font(8.0f, juce::Font::bold));
        g.setColour(accentGlow.withAlpha(0.4f));
        g.drawText("LOW", (int)inner.getX() + 2, (int)inner.getY() + 2, 24, 10,
            juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff60A5FA).withAlpha(harm * 0.4f));
        g.drawText("HIGH", (int)inner.getX() + 28, (int)inner.getY() + 2, 28, 10,
            juce::Justification::centredLeft);
    }

    // Phase marker (current position)
    float markerX = inner.getX() + std::fmod(phase * 2.0f, 1.0f) * w;
    g.setColour(accentGlow.withAlpha(0.3f));
    g.drawLine(markerX, inner.getY() + 4.0f, markerX, inner.getBottom() - 4.0f, 1.0f);

    // L/R legend
    if (stereo > 0.01f)
    {
        g.setFont(juce::Font(9.0f, juce::Font::bold));
        g.setColour(accentGlow.withAlpha(0.4f));
        g.drawText("L", (int)(inner.getRight() - 22), (int)inner.getY() + 2, 10, 10,
            juce::Justification::centred);
        g.setColour(accent.withAlpha(0.3f));
        g.drawText("R", (int)(inner.getRight() - 10), (int)inner.getY() + 2, 10, 10,
            juce::Justification::centred);
    }
}

inline void TremoloEditor::resized()
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
        { sldRate,  lblRate,  valRate },
        { sldDepth, lblDepth, valDepth },
        { sldShape, lblShape, valShape }
    };
    KnobGroup bottomRow[] = {
        { sldStereo,   lblStereo,   valStereo },
        { sldHarmonic, lblHarmonic, valHarmonic },
        { sldLevel,    lblLevel,    valLevel }
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
inline juce::AudioProcessorEditor* TremoloPedal::createEditor()
{
    return new TremoloEditor(*this);
}
