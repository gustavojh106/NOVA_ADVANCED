#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class MetalDistortionPedal;

class MetalEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    MetalEditor(MetalDistortionPedal& pedal);
    ~MetalEditor() override
    {
        stopTimer();
        sldGain.setLookAndFeel(nullptr);
        sldLow.setLookAndFeel(nullptr);
        sldMid.setLookAndFeel(nullptr);
        sldMidFreq.setLookAndFeel(nullptr);
        sldHigh.setLookAndFeel(nullptr);
        sldLevel.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintEqViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    MetalDistortionPedal& proc;

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

    // Knobs
    juce::Slider sldGain, sldLow, sldMid, sldMidFreq, sldHigh, sldLevel;
    juce::Label lblGain, lblLow, lblMid, lblMidFreq, lblHigh, lblLevel;
    juce::Label valGain, valLow, valMid, valMidFreq, valHigh, valLevel;

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

    struct MetalKnobLnF : public juce::LookAndFeel_V4
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetalEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline MetalEditor::MetalEditor(MetalDistortionPedal& pedal)
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

    initKnob(sldGain,    lblGain,    valGain,    "GAIN",     0.0f,   100.0f,  64.0f, 0.1f);
    initKnob(sldLow,     lblLow,     valLow,     "LOW",     -12.0f,  12.0f,   3.0f);
    initKnob(sldMid,     lblMid,     valMid,     "MID",     -15.0f,  15.0f,  -3.5f);
    initKnob(sldMidFreq, lblMidFreq, valMidFreq, "MID FREQ", 250.0f, 5000.0f, 950.0f, 1.0f);
    initKnob(sldHigh,    lblHigh,    valHigh,    "HIGH",    -12.0f,  12.0f,   4.0f);
    initKnob(sldLevel,   lblLevel,   valLevel,   "LEVEL",    0.0f,   1.0f,    0.68f);

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
    wireParam(sldGain,    proc.gainParam);
    wireParam(sldLow,     proc.lowParam);
    wireParam(sldMid,     proc.midParam);
    wireParam(sldMidFreq, proc.midFreqParam);
    wireParam(sldHigh,    proc.highParam);
    wireParam(sldLevel,   proc.levelParam);

    startTimerHz(30);
}

inline void MetalEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldGain,    proc.gainParam);
    syncSlider(sldLow,     proc.lowParam);
    syncSlider(sldMid,     proc.midParam);
    syncSlider(sldMidFreq, proc.midFreqParam);
    syncSlider(sldHigh,    proc.highParam);
    syncSlider(sldLevel,   proc.levelParam);

    // Value readouts
    valGain.setText(juce::String(juce::roundToInt(sldGain.getValue())) + "%", juce::dontSendNotification);
    {
        float v = (float)sldLow.getValue();
        valLow.setText((v >= 0.0f ? "+" : "") + juce::String(v, 1) + " dB", juce::dontSendNotification);
    }
    {
        float v = (float)sldMid.getValue();
        valMid.setText((v >= 0.0f ? "+" : "") + juce::String(v, 1) + " dB", juce::dontSendNotification);
    }
    {
        float mf = (float)sldMidFreq.getValue();
        valMidFreq.setText(mf >= 1000.0f
            ? juce::String(mf * 0.001f, 1) + " kHz"
            : juce::String((int)mf) + " Hz", juce::dontSendNotification);
    }
    {
        float v = (float)sldHigh.getValue();
        valHigh.setText((v >= 0.0f ? "+" : "") + juce::String(v, 1) + " dB", juce::dontSendNotification);
    }
    valLevel.setText(juce::String((int)(sldLevel.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void MetalEditor::paint(juce::Graphics& g)
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
    g.drawText("METAL DISTORTION", 28, 10, 280, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Metal Stack", 28, 33, 200, 18, juce::Justification::centredLeft);

    // Gate indicator
    {
        float gateGain = proc.currentGateGain;
        juce::Colour gateCol = gateGain > 0.5f
            ? juce::Colour(0xff22C55E).withAlpha(0.7f)  // green = open
            : juce::Colour(0xffEF4444).withAlpha(0.5f);  // red = closed
        g.setColour(gateCol);
        g.fillRoundedRectangle((float)getWidth() - 80.0f, 16.0f, 8.0f, 8.0f, 4.0f);
        g.setColour(textDim.withAlpha(0.5f));
        g.setFont(juce::Font(10.0f));
        g.drawText("GATE", getWidth() - 68, 13, 40, 14, juce::Justification::centredLeft);
    }

    // EQ visualization
    paintEqViz(g, vizBounds);
}

inline void MetalEditor::paintEqViz(juce::Graphics& g, juce::Rectangle<float> bounds)
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

    const float low     = (float)sldLow.getValue();
    const float mid     = (float)sldMid.getValue();
    const float midFreq = (float)sldMidFreq.getValue();
    const float high    = (float)sldHigh.getValue();
    const float gain    = (float)sldGain.getValue();

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

    // dB reference lines
    const float dbLines[] = { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f };
    constexpr float dbRange = 18.0f;
    for (float db : dbLines)
    {
        float yNorm = 0.5f - (db / (2.0f * dbRange));
        float y = inner.getY() + yNorm * h;
        g.setColour(juce::Colour(0xff1A2535));
        g.drawLine(inner.getX(), y, inner.getRight(), y, db == 0.0f ? 0.8f : 0.4f);
    }

    // 0dB label
    g.setColour(textDim.withAlpha(0.3f));
    g.setFont(juce::Font(8.0f));
    g.drawText("0", (int)inner.getX() - 14, (int)(midY - 5), 12, 10, juce::Justification::centredRight);

    // Compute and draw EQ response curve
    juce::Path eqPath;
    constexpr int numPoints = 256;

    for (int i = 0; i < numPoints; ++i)
    {
        float xNorm = (float)i / (float)(numPoints - 1);
        float logFreq = 1.69897f + xNorm * (4.30103f - 1.69897f);
        float freq = std::pow(10.0f, logFreq);

        float db = MetalDistortionPedal::computeEqResponse(freq, low, mid, midFreq, high, gain, 44100.0);

        float yNorm = 0.5f - (db / (2.0f * dbRange));
        yNorm = juce::jlimit(0.02f, 0.98f, yNorm);

        float x = inner.getX() + xNorm * w;
        float y = inner.getY() + yNorm * h;

        if (i == 0)
            eqPath.startNewSubPath(x, y);
        else
            eqPath.lineTo(x, y);
    }

    // Filled area under curve
    {
        juce::Path filled(eqPath);
        filled.lineTo(inner.getRight(), midY);
        filled.lineTo(inner.getX(), midY);
        filled.closeSubPath();

        juce::ColourGradient fillGrad(accent.withAlpha(0.15f), inner.getX(), inner.getY(),
            accent.withAlpha(0.02f), inner.getX(), inner.getBottom(), false);
        g.setGradientFill(fillGrad);
        g.fillPath(filled);
    }

    // Glow
    g.setColour(accent.withAlpha(0.08f));
    g.strokePath(eqPath, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.15f));
    g.strokePath(eqPath, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    // Main line
    g.setColour(accentGlow.withAlpha(0.85f));
    g.strokePath(eqPath, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved));

    // Mid freq marker
    float mfNorm = freqToNorm(midFreq);
    float mfX = inner.getX() + mfNorm * w;
    g.setColour(accent.withAlpha(0.25f));
    g.drawLine(mfX, inner.getY(), mfX, inner.getBottom(), 1.0f);
    g.setColour(accent.withAlpha(0.5f));
    g.fillEllipse(mfX - 3.0f, midY - 3.0f, 6.0f, 6.0f);

    // Gain character label
    g.setFont(juce::Font(9.0f));
    g.setColour(textDim.withAlpha(0.4f));
    const char* character = gain < 25.0f ? "Crunch" : gain < 55.0f ? "High gain"
        : gain < 80.0f ? "Metal" : "Brutal";
    g.drawText(character, (int)(inner.getRight() - 55), (int)inner.getY() + 2, 50, 12,
        juce::Justification::centredRight);
}

inline void MetalEditor::resized()
{
    const int w = getWidth();

    // Viz panel
    vizBounds = juce::Rectangle<float>(28.0f, 58.0f, (float)(w - 56), 140.0f);

    // 6 knobs in 2 rows: top row 3 (Gain, Low, Mid), bottom row 3 (Mid Freq, High, Level)
    const int knobSize = 74;
    const int labelH = 16;
    const int valH = 16;
    const int rowSpacing = knobSize + labelH + valH + 12;

    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup topRow[] = {
        { sldGain, lblGain, valGain },
        { sldLow,  lblLow,  valLow },
        { sldMid,  lblMid,  valMid }
    };
    KnobGroup bottomRow[] = {
        { sldMidFreq, lblMidFreq, valMidFreq },
        { sldHigh,    lblHigh,    valHigh },
        { sldLevel,   lblLevel,   valLevel }
    };

    const int topY = 214;
    const int topSlotW = w / 3;
    for (int i = 0; i < 3; ++i)
    {
        int cx = topSlotW * i + topSlotW / 2;
        topRow[i].lbl.setBounds(cx - 50, topY, 100, labelH);
        topRow[i].s.setBounds(cx - knobSize / 2, topY + labelH + 2, knobSize, knobSize);
        topRow[i].val.setBounds(cx - 50, topY + labelH + 2 + knobSize + 2, 100, valH);
    }

    const int bottomY = topY + rowSpacing;
    const int bottomSlotW = w / 3;
    for (int i = 0; i < 3; ++i)
    {
        int cx = bottomSlotW * i + bottomSlotW / 2;
        bottomRow[i].lbl.setBounds(cx - 50, bottomY, 100, labelH);
        bottomRow[i].s.setBounds(cx - knobSize / 2, bottomY + labelH + 2, knobSize, knobSize);
        bottomRow[i].val.setBounds(cx - 50, bottomY + labelH + 2 + knobSize + 2, 100, valH);
    }
}

// ---- Wire createEditor ----
inline juce::AudioProcessorEditor* MetalDistortionPedal::createEditor()
{
    return new MetalEditor(*this);
}
