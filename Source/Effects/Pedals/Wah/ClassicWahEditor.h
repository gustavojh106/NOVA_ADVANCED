#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class ClassicWahPedal;

class ClassicWahEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    ClassicWahEditor(ClassicWahPedal& pedal);
    ~ClassicWahEditor() override
    {
        stopTimer();
        sldSweep.setLookAndFeel(nullptr);
        sldRange.setLookAndFeel(nullptr);
        sldReso.setLookAndFeel(nullptr);
        sldVoice.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintWahViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    ClassicWahPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Amber accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ffD97706");
    const juce::Colour accentDim  = juce::Colour::fromString("ffB45309");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFBBF24");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    juce::Slider sldSweep, sldRange, sldReso, sldVoice, sldMix;
    juce::Label lblSweep, lblRange, lblReso, lblVoice, lblMix;
    juce::Label valSweep, valRange, valReso, valVoice, valMix;

    juce::Rectangle<float> vizBounds;
    float displayFreq  = 400.0f;
    float displaySweep = 0.46f;

    static float freqToNorm(float freq)
    {
        constexpr float logMin = 2.17609f;  // log10(150)
        constexpr float logMax = 3.77815f;  // log10(6000)
        float logF = std::log10(juce::jlimit(150.0f, 6000.0f, freq));
        return (logF - logMin) / (logMax - logMin);
    }

    struct WahKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ffD97706");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffFBBF24");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClassicWahEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline ClassicWahEditor::ClassicWahEditor(ClassicWahPedal& pedal)
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

    initKnob(sldSweep, lblSweep, valSweep, "SWEEP",     0.0f, 1.0f, 0.46f);
    initKnob(sldRange, lblRange, valRange, "RANGE",      0.0f, 1.0f, 0.74f);
    initKnob(sldReso,  lblReso,  valReso,  "RESONANCE",  0.5f, 8.0f, 4.0f, 0.1f);
    initKnob(sldVoice, lblVoice, valVoice, "VOICE",      0.0f, 1.0f, 0.34f);
    initKnob(sldMix,   lblMix,   valMix,   "MIX",        0.0f, 1.0f, 1.0f);

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
    wireParam(sldSweep, proc.sweepParam);
    wireParam(sldRange, proc.rangeParam);
    wireParam(sldReso,  proc.resonanceParam);
    wireParam(sldVoice, proc.voiceParam);
    wireParam(sldMix,   proc.mixParam);

    startTimerHz(30);
}

inline void ClassicWahEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldSweep, proc.sweepParam);
    syncSlider(sldRange, proc.rangeParam);
    syncSlider(sldReso,  proc.resonanceParam);
    syncSlider(sldVoice, proc.voiceParam);
    syncSlider(sldMix,   proc.mixParam);

    // Value readouts
    valSweep.setText(juce::String((int)(sldSweep.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valRange.setText(juce::String((int)(sldRange.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valReso.setText(juce::String((float)sldReso.getValue(), 1) + "x", juce::dontSendNotification);
    {
        float v = (float)sldVoice.getValue();
        const char* voiceName = v < 0.33f ? "Dark" : v < 0.66f ? "Classic" : "Bright";
        valVoice.setText(voiceName, juce::dontSendNotification);
    }
    valMix.setText(juce::String((int)(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);

    displayFreq  += 0.3f * (proc.currentFreq - displayFreq);
    displaySweep += 0.3f * (proc.currentSweep - displaySweep);

    repaint(vizBounds.toNearestInt());
}

inline void ClassicWahEditor::paint(juce::Graphics& g)
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
    g.drawText("CLASSIC WAH", 28, 10, 240, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Cry Baby", 28, 33, 200, 18, juce::Justification::centredLeft);

    paintWahViz(g, vizBounds);
}

inline void ClassicWahEditor::paintWahViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();

    const float reso   = (float)sldReso.getValue();
    const float sweep  = displaySweep;
    const float freq   = displayFreq;
    const float voice  = (float)sldVoice.getValue();
    const float range  = (float)sldRange.getValue();

    // Grid
    const float gridFreqs[] = { 200.0f, 300.0f, 500.0f, 800.0f, 1200.0f, 2000.0f, 3000.0f, 5000.0f };
    const char* gridLabels[] = { "200", "300", "500", "800", "1.2k", "2k", "3k", "5k" };
    g.setFont(juce::Font(8.0f));

    for (int i = 0; i < 8; ++i)
    {
        float xNorm = freqToNorm(gridFreqs[i]);
        float x = inner.getX() + xNorm * w;
        g.setColour(juce::Colour(0xff1A2535));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.3f));
        g.drawText(gridLabels[i], (int)(x - 15), (int)(inner.getBottom() + 1), 30, 10,
            juce::Justification::centred);
    }

    // dB reference
    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), inner.getY() + h * 0.5f, inner.getRight(), inner.getY() + h * 0.5f, 0.5f);

    // ---- Sweep range zone ----
    float minFreq = juce::jmap(voice, 300.0f, 480.0f);
    float maxFreq = juce::jmap(voice, 1600.0f, 2800.0f) + range * 1600.0f;
    float mnNorm = freqToNorm(minFreq);
    float mxNorm = freqToNorm(maxFreq);
    float mnX = inner.getX() + mnNorm * w;
    float mxX = inner.getX() + mxNorm * w;

    g.setColour(accent.withAlpha(0.03f));
    g.fillRect(mnX, inner.getY(), mxX - mnX, h);
    g.setColour(accent.withAlpha(0.12f));
    g.drawLine(mnX, inner.getY(), mnX, inner.getBottom(), 0.5f);
    g.drawLine(mxX, inner.getY(), mxX, inner.getBottom(), 0.5f);

    // ---- Filter response curve ----
    // Sweep-dependent Q (inductor characteristic)
    float sweepQ = reso * (0.75f + sweep * 0.5f);
    juce::Path peakPath;
    constexpr int numPts = 200;

    for (int i = 0; i < numPts; ++i)
    {
        float t = (float)i / (float)(numPts - 1);
        float logMin = 2.17609f;  // log10(150)
        float logMax = 3.77815f;  // log10(6000)
        float logF = logMin + t * (logMax - logMin);
        float f = std::pow(10.0f, logF);

        // SVF bandpass magnitude approximation
        float ratio = f / juce::jmax(80.0f, freq);
        float logRatio = std::log2(ratio);
        float bpDenom = 1.0f + sweepQ * sweepQ * logRatio * logRatio * 4.0f;
        float bpMag = sweepQ / bpDenom;

        float magDb = 20.0f * std::log10(juce::jmax(bpMag, 1.0e-6f));
        constexpr float dbRange = 24.0f;
        float yNorm = juce::jlimit(0.0f, 1.0f, 1.0f - (magDb + dbRange) / (2.0f * dbRange));

        float px = inner.getX() + t * w;
        float py = inner.getY() + yNorm * h;

        if (i == 0)
            peakPath.startNewSubPath(px, py);
        else
            peakPath.lineTo(px, py);
    }

    // Fill under curve
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

    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(peakPath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.8f));
    g.strokePath(peakPath, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved));

    // ---- Treadle position indicator ----
    // Arc at bottom showing heel-to-toe position
    float treadleY = inner.getBottom() - 18.0f;
    float treadleW = w * 0.6f;
    float treadleCx = inner.getCentreX();
    float treadleLeft = treadleCx - treadleW * 0.5f;

    // Track
    g.setColour(juce::Colour(0xff1E2A3A));
    g.fillRoundedRectangle(treadleLeft, treadleY, treadleW, 6.0f, 3.0f);

    // Position dot
    float dotX = treadleLeft + sweep * treadleW;
    g.setColour(accentGlow);
    g.fillEllipse(dotX - 5.0f, treadleY - 2.0f, 10.0f, 10.0f);

    // Labels
    g.setFont(juce::Font(7.0f));
    g.setColour(textDim.withAlpha(0.3f));
    g.drawText("HEEL", (int)treadleLeft - 2, (int)treadleY + 8, 30, 10,
        juce::Justification::centredLeft);
    g.drawText("TOE", (int)(treadleLeft + treadleW - 20), (int)treadleY + 8, 22, 10,
        juce::Justification::centredRight);

    // ---- Current frequency marker ----
    float freqNorm = freqToNorm(juce::jlimit(150.0f, 6000.0f, freq));
    float freqX = inner.getX() + freqNorm * w;

    {
        juce::ColourGradient glow(accentGlow.withAlpha(0.25f), freqX, inner.getY(),
            accent.withAlpha(0.0f), freqX - 15.0f, inner.getCentreY(), false);
        g.setGradientFill(glow);
        g.fillRect(freqX - 15.0f, inner.getY(), 15.0f, h - 24.0f);
    }
    {
        juce::ColourGradient glow(accentGlow.withAlpha(0.25f), freqX, inner.getY(),
            accent.withAlpha(0.0f), freqX + 15.0f, inner.getCentreY(), false);
        g.setGradientFill(glow);
        g.fillRect(freqX, inner.getY(), 15.0f, h - 24.0f);
    }
    g.setColour(accentGlow.withAlpha(0.8f));
    g.drawLine(freqX, inner.getY(), freqX, inner.getBottom() - 22.0f, 1.5f);

    // Freq readout
    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.setColour(accentGlow);
    juce::String freqStr = freq >= 1000.0f
        ? juce::String(freq * 0.001f, 1) + " kHz"
        : juce::String((int)freq) + " Hz";
    g.drawText(freqStr, (int)(freqX - 30), (int)inner.getY() + 3, 60, 14,
        juce::Justification::centred);
}

inline void ClassicWahEditor::resized()
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
        { sldSweep, lblSweep, valSweep },
        { sldRange, lblRange, valRange },
        { sldReso,  lblReso,  valReso },
        { sldVoice, lblVoice, valVoice },
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
inline juce::AudioProcessorEditor* ClassicWahPedal::createEditor()
{
    return new ClassicWahEditor(*this);
}
