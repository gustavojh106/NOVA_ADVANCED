#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class EQPedal;

class EQEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    EQEditor(EQPedal& pedal);
    ~EQEditor() override
    {
        stopTimer();
        for (auto* s : allSliders)
            s->setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintEqCurve(juce::Graphics& g, juce::Rectangle<float> bounds);

    EQPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Green accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ff4ADE80");
    const juce::Colour accentDim  = juce::Colour::fromString("ff22C55E");
    const juce::Colour accentGlow = juce::Colour::fromString("ff86EFAC");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    juce::Slider sldLow, sldLowMid, sldMid, sldPresence, sldHigh, sldMidFreq, sldLevel;
    juce::Label lblLow, lblLowMid, lblMid, lblPresence, lblHigh, lblMidFreq, lblLevel;
    juce::Label valLow, valLowMid, valMid, valPresence, valHigh, valMidFreq, valLevel;

    std::array<juce::Slider*, 7> allSliders = {
        &sldLow, &sldLowMid, &sldMid, &sldPresence, &sldHigh, &sldMidFreq, &sldLevel
    };

    juce::Rectangle<float> vizBounds;

    // Helpers for log frequency mapping
    static float freqToNorm(float freq)
    {
        static const float logMin = std::log(20.0f);
        static const float logMax = std::log(20000.0f);
        return (std::log(juce::jlimit(20.0f, 20000.0f, freq)) - logMin) / (logMax - logMin);
    }

    static float normToFreq(float norm)
    {
        static const float logMin = std::log(20.0f);
        static const float logMax = std::log(20000.0f);
        return std::exp(logMin + norm * (logMax - logMin));
    }

    struct EQKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ff4ADE80");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff86EFAC");

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline EQEditor::EQEditor(EQPedal& pedal)
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

    initKnob(sldLow,      lblLow,      valLow,      "LOW",       -12.0f, 12.0f, 0.0f, 0.1f);
    initKnob(sldLowMid,   lblLowMid,   valLowMid,   "LOW-MID",   -12.0f, 12.0f, 0.0f, 0.1f);
    initKnob(sldMid,      lblMid,      valMid,      "MID",       -12.0f, 12.0f, 0.0f, 0.1f);
    initKnob(sldPresence,  lblPresence,  valPresence,  "PRESENCE",  -12.0f, 12.0f, 0.0f, 0.1f);
    initKnob(sldHigh,     lblHigh,     valHigh,     "HIGH",      -12.0f, 12.0f, 0.0f, 0.1f);
    initKnob(sldMidFreq,  lblMidFreq,  valMidFreq,  "MID FREQ",  200.0f, 5000.0f, 800.0f, 1.0f);
    initKnob(sldLevel,    lblLevel,    valLevel,    "LEVEL",     0.0f, 2.0f, 1.0f);

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
    wireParam(sldLow,      proc.lowParam);
    wireParam(sldLowMid,   proc.lowMidParam);
    wireParam(sldMid,      proc.midParam);
    wireParam(sldPresence,  proc.presenceParam);
    wireParam(sldHigh,     proc.highParam);
    wireParam(sldMidFreq,  proc.midFreqParam);
    wireParam(sldLevel,    proc.levelParam);

    startTimerHz(30);
}

inline void EQEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldLow,      proc.lowParam);
    syncSlider(sldLowMid,   proc.lowMidParam);
    syncSlider(sldMid,      proc.midParam);
    syncSlider(sldPresence,  proc.presenceParam);
    syncSlider(sldHigh,     proc.highParam);
    syncSlider(sldMidFreq,  proc.midFreqParam);
    syncSlider(sldLevel,    proc.levelParam);

    // Format dB values for band knobs
    auto fmtDb = [](juce::Label& v, float db)
    {
        juce::String sign = db >= 0.0f ? "+" : "";
        v.setText(sign + juce::String(db, 1) + " dB", juce::dontSendNotification);
    };
    fmtDb(valLow,      (float)sldLow.getValue());
    fmtDb(valLowMid,   (float)sldLowMid.getValue());
    fmtDb(valMid,      (float)sldMid.getValue());
    fmtDb(valPresence, (float)sldPresence.getValue());
    fmtDb(valHigh,     (float)sldHigh.getValue());

    // Mid freq
    {
        float freq = (float)sldMidFreq.getValue();
        if (freq >= 1000.0f)
            valMidFreq.setText(juce::String(freq / 1000.0f, 1) + " kHz", juce::dontSendNotification);
        else
            valMidFreq.setText(juce::String((int)freq) + " Hz", juce::dontSendNotification);
    }

    // Level (gain to dB)
    {
        float gain = (float)sldLevel.getValue();
        float db = 20.0f * std::log10(juce::jmax(gain, 0.001f));
        valLevel.setText((db >= 0 ? "+" : "") + juce::String(db, 1) + " dB", juce::dontSendNotification);
    }

    repaint(vizBounds.toNearestInt());
}

inline void EQEditor::paint(juce::Graphics& g)
{
    // Background gradient
    {
        juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
            bgDark, 0.0f, (float)getHeight(), false);
        g.setGradientFill(bg);
        g.fillAll();
    }

    // Border
    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    // Top accent line
    {
        juce::ColourGradient glow(accent.withAlpha(0.30f), (float)getWidth() * 0.2f, 0.0f,
            accent.withAlpha(0.0f), (float)getWidth() * 0.8f, 0.0f, false);
        g.setGradientFill(glow);
        g.fillRect(0.0f, 0.0f, (float)getWidth(), 2.0f);
    }

    // Title
    g.setColour(textBright);
    g.setFont(juce::Font(22.0f, juce::Font::bold));
    g.drawText("EQ", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("5-Band Parametric", 28, 33, 200, 18, juce::Justification::centredLeft);

    paintEqCurve(g, vizBounds);
}

inline void EQEditor::paintEqCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Background
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(36.0f, 12.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float dbMin = -15.0f;
    const float dbMax = 15.0f;
    const float dbRange = dbMax - dbMin;

    auto dbToY = [&](float db) -> float
    {
        return inner.getBottom() - ((db - dbMin) / dbRange) * h;
    };

    // dB grid lines
    g.setFont(juce::Font(9.0f));
    for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
    {
        float y = dbToY(db);
        g.setColour(db == 0.0f ? juce::Colour(0xff2A3A50) : juce::Colour(0xff161E2C));
        g.drawLine(inner.getX(), y, inner.getRight(), y, db == 0.0f ? 1.0f : 0.5f);
        g.setColour(textDim.withAlpha(0.5f));
        juce::String label = (db >= 0 ? "+" : "") + juce::String((int)db);
        g.drawText(label, (int)(bounds.getX() + 4), (int)(y - 6), 28, 12,
            juce::Justification::centredRight);
    }

    // Frequency grid lines
    const float gridFreqs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    const char* gridLabels[] = { "50", "100", "200", "500", "1k", "2k", "5k", "10k" };
    for (int i = 0; i < 8; ++i)
    {
        float x = inner.getX() + freqToNorm(gridFreqs[i]) * w;
        g.setColour(juce::Colour(0xff161E2C));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.4f));
        g.drawText(gridLabels[i], (int)(x - 16), (int)(inner.getBottom() + 2), 32, 12,
            juce::Justification::centred);
    }

    // Get current param values
    float lo = (float)sldLow.getValue();
    float lm = (float)sldLowMid.getValue();
    float md = (float)sldMid.getValue();
    float mf = (float)sldMidFreq.getValue();
    float pr = (float)sldPresence.getValue();
    float hi = (float)sldHigh.getValue();

    double sr = proc.getSampleRate();
    if (sr <= 0.0) sr = 48000.0;

    // Precompute band biquads for magnitude evaluation
    using namespace Nova::EqDSP;
    Biquad bands[5];
    bands[0].setLowShelf(kLowFreq, lo, kShelfQ, sr);
    bands[1].setPeak(kLowMidFreq, lm, kPeakQ, sr);
    bands[2].setPeak(juce::jlimit(200.0f, 5000.0f, mf), md, kMidQ, sr);
    bands[3].setPeak(kPresenceFreq, pr, kPeakQ, sr);
    bands[4].setHighShelf(kHighFreq, hi, kShelfQ, sr);

    constexpr int numPoints = 300;

    // Individual band curves (thin, semi-transparent)
    const float bandAlphas[5] = { 0.15f, 0.15f, 0.25f, 0.15f, 0.15f };
    for (int b = 0; b < 5; ++b)
    {
        float gain = 0.0f;
        switch (b)
        {
            case 0: gain = lo; break;
            case 1: gain = lm; break;
            case 2: gain = md; break;
            case 3: gain = pr; break;
            case 4: gain = hi; break;
        }
        if (std::abs(gain) < 0.2f) continue; // Skip flat bands

        juce::Path bandPath;
        for (int i = 0; i < numPoints; ++i)
        {
            float xNorm = (float)i / (float)(numPoints - 1);
            float freq = normToFreq(xNorm);
            float mag = bands[b].magnitudeAt(freq, sr);
            float db = 20.0f * std::log10(juce::jmax(mag, 1.0e-6f));
            float px = inner.getX() + xNorm * w;
            float py = dbToY(juce::jlimit(dbMin, dbMax, db));

            if (i == 0) bandPath.startNewSubPath(px, py);
            else bandPath.lineTo(px, py);
        }

        g.setColour(accent.withAlpha(bandAlphas[b]));
        g.strokePath(bandPath, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved));
    }

    // Combined response curve
    juce::Path curvePath;
    juce::Path fillPath;
    float zeroDbY = dbToY(0.0f);

    fillPath.startNewSubPath(inner.getX(), zeroDbY);

    for (int i = 0; i < numPoints; ++i)
    {
        float xNorm = (float)i / (float)(numPoints - 1);
        float freq = normToFreq(xNorm);

        float totalMag = 1.0f;
        for (int b = 0; b < 5; ++b)
            totalMag *= bands[b].magnitudeAt(freq, sr);

        float db = 20.0f * std::log10(juce::jmax(totalMag, 1.0e-6f));
        db = juce::jlimit(dbMin, dbMax, db);

        float px = inner.getX() + xNorm * w;
        float py = dbToY(db);

        if (i == 0) curvePath.startNewSubPath(px, py);
        else curvePath.lineTo(px, py);

        fillPath.lineTo(px, py);
    }

    fillPath.lineTo(inner.getRight(), zeroDbY);
    fillPath.closeSubPath();

    // Fill between curve and 0dB line
    g.setColour(accent.withAlpha(0.08f));
    g.fillPath(fillPath);

    // Curve glow + stroke
    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(curvePath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.85f));
    g.strokePath(curvePath, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));

    // Band markers (dots on the combined curve at band center frequencies)
    const float bandFreqs[5] = { kLowFreq, kLowMidFreq, mf, kPresenceFreq, kHighFreq };
    const float bandGains[5] = { lo, lm, md, pr, hi };

    for (int b = 0; b < 5; ++b)
    {
        float totalMag = 1.0f;
        for (int j = 0; j < 5; ++j)
            totalMag *= bands[j].magnitudeAt(bandFreqs[b], sr);

        float db = 20.0f * std::log10(juce::jmax(totalMag, 1.0e-6f));
        db = juce::jlimit(dbMin, dbMax, db);

        float px = inner.getX() + freqToNorm(bandFreqs[b]) * w;
        float py = dbToY(db);

        bool isMid = (b == 2);
        float dotR = isMid ? 5.0f : 3.5f;

        // Glow for active bands
        if (std::abs(bandGains[b]) > 0.5f)
        {
            g.setColour(accent.withAlpha(isMid ? 0.25f : 0.12f));
            g.fillEllipse(px - dotR * 2.0f, py - dotR * 2.0f, dotR * 4.0f, dotR * 4.0f);
        }

        g.setColour(isMid ? accentGlow : accent);
        g.fillEllipse(px - dotR, py - dotR, dotR * 2.0f, dotR * 2.0f);

        // Dark center for depth
        g.setColour(juce::Colour(0xff0D1520).withAlpha(0.5f));
        g.fillEllipse(px - dotR * 0.4f, py - dotR * 0.4f, dotR * 0.8f, dotR * 0.8f);
    }
}

inline void EQEditor::resized()
{
    const int w = getWidth();

    vizBounds = juce::Rectangle<float>(28.0f, 54.0f, (float)(w - 56), 136.0f);

    const int knobSize = 66;
    const int labelH = 14;
    const int valH = 14;

    // Row 1: 5 band gain knobs
    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup row1[] = {
        { sldLow,      lblLow,      valLow },
        { sldLowMid,   lblLowMid,   valLowMid },
        { sldMid,      lblMid,      valMid },
        { sldPresence, lblPresence, valPresence },
        { sldHigh,     lblHigh,     valHigh }
    };

    const int topY = 206;
    const int slotW = w / 5;
    for (int i = 0; i < 5; ++i)
    {
        int cx = slotW * i + slotW / 2;
        row1[i].lbl.setBounds(cx - 50, topY, 100, labelH);
        row1[i].s.setBounds(cx - knobSize / 2, topY + labelH + 2, knobSize, knobSize);
        row1[i].val.setBounds(cx - 50, topY + labelH + 2 + knobSize + 2, 100, valH);
    }

    // Row 2: Mid Freq and Level (centered)
    KnobGroup row2[] = {
        { sldMidFreq, lblMidFreq, valMidFreq },
        { sldLevel,   lblLevel,   valLevel }
    };

    const int bottomY = topY + labelH + knobSize + valH + 18;
    const int r2SlotW = w / 2;
    for (int i = 0; i < 2; ++i)
    {
        int cx = r2SlotW * i + r2SlotW / 2;
        row2[i].lbl.setBounds(cx - 50, bottomY, 100, labelH);
        row2[i].s.setBounds(cx - knobSize / 2, bottomY + labelH + 2, knobSize, knobSize);
        row2[i].val.setBounds(cx - 50, bottomY + labelH + 2 + knobSize + 2, 100, valH);
    }
}

// ---- Wire createEditor ----
inline juce::AudioProcessorEditor* EQPedal::createEditor()
{
    return new EQEditor(*this);
}
