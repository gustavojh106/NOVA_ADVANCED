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
        for (auto* slider : allSliders())
            slider->setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintEqCurve(juce::Graphics& g, juce::Rectangle<float> bounds);

    std::array<juce::Slider*, 10> allSliders()
    {
        return {
            &sldLowCut, &sldLow, &sldLowMid, &sldMid, &sldMidFreq,
            &sldMidQ, &sldPresence, &sldHigh, &sldHighCut, &sldLevel
        };
    }

    EQPedal& proc;

    static constexpr int kWidth = 720;
    static constexpr int kHeight = 450;

    const juce::Colour accent = juce::Colour::fromString("ff4ADE80");
    const juce::Colour accentGlow = juce::Colour::fromString("ff86EFAC");
    const juce::Colour bgDark = juce::Colour(0xff0B0E14);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim = juce::Colour(0xff7B8BA0);

    juce::Slider sldLowCut, sldLow, sldLowMid, sldMid, sldMidFreq, sldMidQ, sldPresence, sldHigh, sldHighCut, sldLevel;
    juce::Label lblLowCut, lblLow, lblLowMid, lblMid, lblMidFreq, lblMidQ, lblPresence, lblHigh, lblHighCut, lblLevel;
    juce::Label valLowCut, valLow, valLowMid, valMid, valMidFreq, valMidQ, valPresence, valHigh, valHighCut, valLevel;

    juce::Rectangle<float> vizBounds;

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
        juce::Colour kAccent = juce::Colour::fromString("ff4ADE80");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff86EFAC");

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
            float sliderPos, float startAngle, float endAngle,
            juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(6.0f);
            const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float cx = area.getCentreX();
            const float cy = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcRadius = radius - 4.0f;

            juce::Path track;
            track.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff1E2A3A));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.56f;
            juce::ColourGradient grad(juce::Colour(0xff1C2838), cx, cy - knobRadius,
                juce::Colour(0xff0D1520), cx, cy + knobRadius, false);
            g.setGradientFill(grad);
            g.fillEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2A3A4C));
            g.drawEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLen = knobRadius * 0.70f;
            const float px = cx + std::sin(angle) * pointerLen;
            const float py = cy - std::cos(angle) * pointerLen;
            g.setColour(kAccentGlow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQEditor)
};

inline EQEditor::EQEditor(EQPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& slider, juce::Label& label, juce::Label& value,
        const juce::String& name, float min, float max, float def, float step = 0.01f)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider.setRange(min, max, step);
        slider.setValue(def, juce::dontSendNotification);
        slider.setLookAndFeel(&knobLnF);
        addAndMakeVisible(slider);

        label.setText(name, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(label);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(juce::FontOptions(11.0f)));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(sldLowCut,   lblLowCut,   valLowCut,   "LOW CUT",  20.0f,   400.0f,   20.0f,    1.0f);
    initKnob(sldLow,      lblLow,      valLow,      "LOW",     -12.0f,    12.0f,    0.0f,    0.1f);
    initKnob(sldLowMid,   lblLowMid,   valLowMid,   "LOW-MID", -12.0f,    12.0f,    0.0f,    0.1f);
    initKnob(sldMid,      lblMid,      valMid,      "MID",     -12.0f,    12.0f,    0.0f,    0.1f);
    initKnob(sldMidFreq,  lblMidFreq,  valMidFreq,  "MID FREQ", 200.0f, 5000.0f,  800.0f,    1.0f);
    initKnob(sldMidQ,     lblMidQ,     valMidQ,     "MID Q",     0.35f,    2.5f,    1.0f,    0.01f);
    initKnob(sldPresence, lblPresence, valPresence, "PRESENCE", -12.0f,    12.0f,    0.0f,    0.1f);
    initKnob(sldHigh,     lblHigh,     valHigh,     "HIGH",    -12.0f,    12.0f,    0.0f,    0.1f);
    initKnob(sldHighCut,  lblHighCut,  valHighCut,  "HIGH CUT", 2500.0f, 20000.0f, 20000.0f, 10.0f);
    initKnob(sldLevel,    lblLevel,    valLevel,    "LEVEL",     0.0f,     2.0f,    1.0f,    0.01f);

    auto wireParam = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param == nullptr)
            return;

        slider.setValue(param->get(), juce::dontSendNotification);
        slider.onValueChange = [&slider, param]
        {
            param->beginChangeGesture();
            *param = (float) slider.getValue();
            param->endChangeGesture();
        };
    };

    wireParam(sldLowCut,   proc.lowCutParam);
    wireParam(sldLow,      proc.lowParam);
    wireParam(sldLowMid,   proc.lowMidParam);
    wireParam(sldMid,      proc.midParam);
    wireParam(sldMidFreq,  proc.midFreqParam);
    wireParam(sldMidQ,     proc.midQParam);
    wireParam(sldPresence, proc.presenceParam);
    wireParam(sldHigh,     proc.highParam);
    wireParam(sldHighCut,  proc.highCutParam);
    wireParam(sldLevel,    proc.levelParam);

    startTimerHz(30);
}

inline void EQEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldLowCut,   proc.lowCutParam);
    syncSlider(sldLow,      proc.lowParam);
    syncSlider(sldLowMid,   proc.lowMidParam);
    syncSlider(sldMid,      proc.midParam);
    syncSlider(sldMidFreq,  proc.midFreqParam);
    syncSlider(sldMidQ,     proc.midQParam);
    syncSlider(sldPresence, proc.presenceParam);
    syncSlider(sldHigh,     proc.highParam);
    syncSlider(sldHighCut,  proc.highCutParam);
    syncSlider(sldLevel,    proc.levelParam);

    auto fmtDb = [](juce::Label& label, float db)
    {
        const juce::String sign = db >= 0.0f ? "+" : "";
        label.setText(sign + juce::String(db, 1) + " dB", juce::dontSendNotification);
    };

    auto fmtFreq = [](juce::Label& label, float freq)
    {
        if (freq >= 1000.0f)
            label.setText(juce::String(freq / 1000.0f, 1) + " kHz", juce::dontSendNotification);
        else
            label.setText(juce::String((int) freq) + " Hz", juce::dontSendNotification);
    };

    fmtFreq(valLowCut, (float) sldLowCut.getValue());
    fmtDb(valLow, (float) sldLow.getValue());
    fmtDb(valLowMid, (float) sldLowMid.getValue());
    fmtDb(valMid, (float) sldMid.getValue());
    fmtFreq(valMidFreq, (float) sldMidFreq.getValue());
    valMidQ.setText(juce::String((float) sldMidQ.getValue(), 2), juce::dontSendNotification);
    fmtDb(valPresence, (float) sldPresence.getValue());
    fmtDb(valHigh, (float) sldHigh.getValue());
    fmtFreq(valHighCut, (float) sldHighCut.getValue());

    const float levelGain = (float) sldLevel.getValue();
    const float levelDb = 20.0f * std::log10(juce::jmax(levelGain, 0.001f));
    valLevel.setText((levelDb >= 0.0f ? "+" : "") + juce::String(levelDb, 1) + " dB", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void EQEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
        bgDark, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    juce::ColourGradient glow(accent.withAlpha(0.30f), (float) getWidth() * 0.2f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth() * 0.8f, 0.0f, false);
    g.setGradientFill(glow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 2.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("EQ", 28, 10, 180, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("Studio Semi-Parametric", 28, 33, 220, 18, juce::Justification::centredLeft);

    paintEqCurve(g, vizBounds);
}

inline void EQEditor::paintEqCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(36.0f, 12.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();
    const float dbMin = -18.0f;
    const float dbMax = 18.0f;
    const float dbRange = dbMax - dbMin;

    auto dbToY = [&](float db)
    {
        return inner.getBottom() - ((db - dbMin) / dbRange) * height;
    };

    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
    {
        const float y = dbToY(db);
        g.setColour(db == 0.0f ? juce::Colour(0xff2A3A50) : juce::Colour(0xff161E2C));
        g.drawLine(inner.getX(), y, inner.getRight(), y, db == 0.0f ? 1.0f : 0.5f);
        g.setColour(textDim.withAlpha(0.5f));
        const juce::String label = (db >= 0.0f ? "+" : "") + juce::String((int) db);
        g.drawText(label, (int) (bounds.getX() + 4), (int) (y - 6), 28, 12, juce::Justification::centredRight);
    }

    const float gridFreqs[] = { 30.0f, 60.0f, 120.0f, 250.0f, 500.0f, 1000.0f, 2500.0f, 5000.0f, 10000.0f };
    const char* gridLabels[] = { "30", "60", "120", "250", "500", "1k", "2.5k", "5k", "10k" };
    for (int i = 0; i < 9; ++i)
    {
        const float x = inner.getX() + freqToNorm(gridFreqs[i]) * width;
        g.setColour(juce::Colour(0xff161E2C));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.4f));
        g.drawText(gridLabels[i], (int) (x - 16), (int) (inner.getBottom() + 2), 32, 12, juce::Justification::centred);
    }

    const float lowCutHz = (float) sldLowCut.getValue();
    const float lowDb = (float) sldLow.getValue();
    const float lowMidDb = (float) sldLowMid.getValue();
    const float midDb = (float) sldMid.getValue();
    const float midFreqHz = (float) sldMidFreq.getValue();
    const float midQ = (float) sldMidQ.getValue();
    const float presenceDb = (float) sldPresence.getValue();
    const float highDb = (float) sldHigh.getValue();
    const float highCutHz = (float) sldHighCut.getValue();
    const float outputLevelDb = 20.0f * std::log10(juce::jmax((float) sldLevel.getValue(), 0.001f));

    double sampleRate = proc.getSampleRate();
    if (sampleRate <= 0.0)
        sampleRate = 48000.0;

    using namespace Nova::EqDSP;
    Biquad stages[7];

    if (lowCutHz <= 21.0f)
        stages[0].setIdentity();
    else
        stages[0].setHighPass(lowCutHz, kCutQ, sampleRate);

    if (std::abs(lowDb) < 0.01f)
        stages[1].setIdentity();
    else
        stages[1].setLowShelf(kLowShelfFreq, lowDb, kShelfQ, sampleRate);

    if (std::abs(lowMidDb) < 0.01f)
        stages[2].setIdentity();
    else
        stages[2].setPeak(kLowMidFreq, lowMidDb, proportionalQ(kLowMidBaseQ, kLowMidMaxQ, lowMidDb), sampleRate);

    if (std::abs(midDb) < 0.01f)
        stages[3].setIdentity();
    else
        stages[3].setPeak(midFreqHz, midDb,
            juce::jlimit(0.35f, 2.80f, midQ * juce::jmap(std::abs(midDb), 0.0f, 12.0f, 1.0f, 1.18f)),
            sampleRate);

    if (std::abs(presenceDb) < 0.01f)
        stages[4].setIdentity();
    else
        stages[4].setPeak(kPresenceFreq, presenceDb, proportionalQ(kPresenceBaseQ, kPresenceMaxQ, presenceDb), sampleRate);

    if (std::abs(highDb) < 0.01f)
        stages[5].setIdentity();
    else
        stages[5].setHighShelf(kHighShelfFreq, highDb, kShelfQ, sampleRate);

    if (highCutHz >= 19950.0f)
        stages[6].setIdentity();
    else
        stages[6].setLowPass(highCutHz, kCutQ, sampleRate);

    juce::Path curvePath;
    juce::Path fillPath;
    const float zeroDbY = dbToY(0.0f);
    fillPath.startNewSubPath(inner.getX(), zeroDbY);

    constexpr int numPoints = 320;
    for (int i = 0; i < numPoints; ++i)
    {
        const float xNorm = (float) i / (float) (numPoints - 1);
        const float freq = normToFreq(xNorm);

        float totalMag = 1.0f;
        for (auto& stage : stages)
            totalMag *= stage.magnitudeAt(freq, sampleRate);

        float db = 20.0f * std::log10(juce::jmax(totalMag, 1.0e-6f)) + outputLevelDb;
        db = juce::jlimit(dbMin, dbMax, db);

        const float px = inner.getX() + xNorm * width;
        const float py = dbToY(db);

        if (i == 0)
            curvePath.startNewSubPath(px, py);
        else
            curvePath.lineTo(px, py);

        fillPath.lineTo(px, py);
    }

    fillPath.lineTo(inner.getRight(), zeroDbY);
    fillPath.closeSubPath();

    g.setColour(accent.withAlpha(0.08f));
    g.fillPath(fillPath);

    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(curvePath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.85f));
    g.strokePath(curvePath, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));

    for (float markerFreq : { lowCutHz, midFreqHz, highCutHz })
    {
        const float x = inner.getX() + freqToNorm(markerFreq) * width;
        g.setColour(accent.withAlpha(0.22f));
        for (float y = inner.getY(); y < inner.getBottom(); y += 6.0f)
            g.drawLine(x, y, x, juce::jmin(y + 3.0f, inner.getBottom()), 0.8f);
    }
}

inline void EQEditor::resized()
{
    const int width = getWidth();
    vizBounds = juce::Rectangle<float>(28.0f, 54.0f, (float) (width - 56), 146.0f);

    const int columns = 5;
    const int left = 22;
    const int right = 22;
    const int slotW = (width - left - right) / columns;
    const int knobSize = 66;
    const int labelH = 14;
    const int valH = 14;
    const int row1Y = 214;
    const int row2Y = 330;

    struct KnobGroup { juce::Slider& slider; juce::Label& label; juce::Label& value; };
    KnobGroup row1[] = {
        { sldLowCut,  lblLowCut,  valLowCut },
        { sldLow,     lblLow,     valLow },
        { sldLowMid,  lblLowMid,  valLowMid },
        { sldMid,     lblMid,     valMid },
        { sldMidFreq, lblMidFreq, valMidFreq }
    };
    KnobGroup row2[] = {
        { sldMidQ,     lblMidQ,     valMidQ },
        { sldPresence, lblPresence, valPresence },
        { sldHigh,     lblHigh,     valHigh },
        { sldHighCut,  lblHighCut,  valHighCut },
        { sldLevel,    lblLevel,    valLevel }
    };

    for (int i = 0; i < columns; ++i)
    {
        const int cx = left + slotW * i + slotW / 2;

        row1[i].label.setBounds(cx - 56, row1Y, 112, labelH);
        row1[i].slider.setBounds(cx - knobSize / 2, row1Y + labelH + 2, knobSize, knobSize);
        row1[i].value.setBounds(cx - 56, row1Y + labelH + 2 + knobSize + 2, 112, valH);

        row2[i].label.setBounds(cx - 56, row2Y, 112, labelH);
        row2[i].slider.setBounds(cx - knobSize / 2, row2Y + labelH + 2, knobSize, knobSize);
        row2[i].value.setBounds(cx - 56, row2Y + labelH + 2 + knobSize + 2, 112, valH);
    }
}

inline juce::AudioProcessorEditor* EQPedal::createEditor()
{
    return new EQEditor(*this);
}
