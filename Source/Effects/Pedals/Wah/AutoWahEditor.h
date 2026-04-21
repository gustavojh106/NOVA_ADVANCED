#pragma once

#include <JuceHeader.h>

class AutoWahPedal;

class AutoWahEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    AutoWahEditor(AutoWahPedal& pedal);
    ~AutoWahEditor() override
    {
        stopTimer();
        sensitivitySlider.setLookAndFeel(nullptr);
        attackSlider.setLookAndFeel(nullptr);
        releaseSlider.setLookAndFeel(nullptr);
        rangeSlider.setLookAndFeel(nullptr);
        resonanceSlider.setLookAndFeel(nullptr);
        voiceSlider.setLookAndFeel(nullptr);
        mixSlider.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintSweep(juce::Graphics& g, juce::Rectangle<float> bounds);

    AutoWahPedal& proc;

    juce::Slider sensitivitySlider;
    juce::Slider attackSlider;
    juce::Slider releaseSlider;
    juce::Slider rangeSlider;
    juce::Slider resonanceSlider;
    juce::Slider voiceSlider;
    juce::Slider mixSlider;

    juce::Label sensitivityLabel;
    juce::Label attackLabel;
    juce::Label releaseLabel;
    juce::Label rangeLabel;
    juce::Label resonanceLabel;
    juce::Label voiceLabel;
    juce::Label mixLabel;

    juce::Label sensitivityValue;
    juce::Label attackValue;
    juce::Label releaseValue;
    juce::Label rangeValue;
    juce::Label resonanceValue;
    juce::Label voiceValue;
    juce::Label mixValue;

    juce::Rectangle<float> sweepBounds;
    float displayFreq = 420.0f;
    float displayEnvelope = 0.0f;

    const juce::Colour accent = juce::Colour::fromString("ffD97706");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFBBF24");
    const juce::Colour panel = juce::Colour(0xff0B1118);
    const juce::Colour panelEdge = juce::Colour(0xff243241);
    const juce::Colour textBright = juce::Colour(0xffF6F2E9);
    const juce::Colour textDim = juce::Colour(0xff8B9BB0);

    struct WahKnobLookAndFeel : public juce::LookAndFeel_V4
    {
        juce::Colour accent = juce::Colour::fromString("ffD97706");
        juce::Colour glow = juce::Colour::fromString("ffFBBF24");

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
            float sliderPos, float startAngle, float endAngle,
            juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(5.0f);
            const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float centreX = area.getCentreX();
            const float centreY = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcRadius = radius - 4.0f;

            juce::Path track;
            track.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff1A2532));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.001f)
            {
                juce::Path arc;
                arc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(accent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(accent);
                g.strokePath(arc, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.58f;
            juce::ColourGradient body(juce::Colour(0xff202B38), centreX, centreY - knobRadius,
                juce::Colour(0xff0B1118), centreX, centreY + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff374656));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.74f;
            const float pointX = centreX + std::sin(angle) * pointerLength;
            const float pointY = centreY - std::cos(angle) * pointerLength;
            g.setColour(glow);
            g.drawLine(centreX, centreY, pointX, pointY, 2.0f);
            g.fillEllipse(pointX - 2.8f, pointY - 2.8f, 5.6f, 5.6f);
        }
    } knobLookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoWahEditor)
};

#include "AutoWahPedal.h"

inline AutoWahEditor::AutoWahEditor(AutoWahPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(820, 420);

    auto initKnob = [this](juce::Slider& slider, juce::Label& name, juce::Label& value,
        const juce::String& text, double min, double max, double defaultValue, double step)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider.setRange(min, max, step);
        slider.setValue(defaultValue, juce::dontSendNotification);
        slider.setLookAndFeel(&knobLookAndFeel);
        addAndMakeVisible(slider);

        name.setText(text, juce::dontSendNotification);
        name.setJustificationType(juce::Justification::centred);
        name.setFont(juce::Font(11.0f, juce::Font::bold));
        name.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(name);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(11.0f));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(sensitivitySlider, sensitivityLabel, sensitivityValue, "SENS", 0.0, 1.0, 0.62, 0.01);
    initKnob(attackSlider, attackLabel, attackValue, "ATTACK", 0.5, 30.0, 2.5, 0.1);
    initKnob(releaseSlider, releaseLabel, releaseValue, "RELEASE", 15.0, 900.0, 180.0, 1.0);
    initKnob(rangeSlider, rangeLabel, rangeValue, "RANGE", 0.0, 1.0, 0.78, 0.01);
    initKnob(resonanceSlider, resonanceLabel, resonanceValue, "Q", 0.6, 9.0, 4.2, 0.1);
    initKnob(voiceSlider, voiceLabel, voiceValue, "VOICE", 0.0, 1.0, 0.42, 0.01);
    initKnob(mixSlider, mixLabel, mixValue, "MIX", 0.0, 1.0, 1.0, 0.01);

    const auto wireFloat = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param == nullptr)
            return;

        slider.setValue(param->get(), juce::dontSendNotification);
        slider.onValueChange = [&slider, param]
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->convertTo0to1((float) slider.getValue()));
            param->endChangeGesture();
        };
    };

    wireFloat(sensitivitySlider, proc.sensitivityParam);
    wireFloat(attackSlider, proc.attackParam);
    wireFloat(releaseSlider, proc.releaseParam);
    wireFloat(rangeSlider, proc.rangeParam);
    wireFloat(resonanceSlider, proc.resonanceParam);
    wireFloat(voiceSlider, proc.voiceParam);
    wireFloat(mixSlider, proc.mixParam);

    startTimerHz(30);
}

inline void AutoWahEditor::timerCallback()
{
    const auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sensitivitySlider, proc.sensitivityParam);
    syncSlider(attackSlider, proc.attackParam);
    syncSlider(releaseSlider, proc.releaseParam);
    syncSlider(rangeSlider, proc.rangeParam);
    syncSlider(resonanceSlider, proc.resonanceParam);
    syncSlider(voiceSlider, proc.voiceParam);
    syncSlider(mixSlider, proc.mixParam);

    sensitivityValue.setText(juce::String((int) std::round(sensitivitySlider.getValue() * 100.0)) + "%", juce::dontSendNotification);
    attackValue.setText(juce::String((float) attackSlider.getValue(), attackSlider.getValue() < 10.0 ? 1 : 0) + " ms", juce::dontSendNotification);
    releaseValue.setText(juce::String((int) std::round(releaseSlider.getValue())) + " ms", juce::dontSendNotification);
    rangeValue.setText(juce::String((int) std::round(rangeSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);
    resonanceValue.setText(juce::String((float) resonanceSlider.getValue(), 1) + "x", juce::dontSendNotification);

    const float voice = (float) voiceSlider.getValue();
    const char* voiceName = voice < 0.33f ? "Dark" : voice < 0.66f ? "Classic" : "Bright";
    voiceValue.setText(voiceName, juce::dontSendNotification);
    mixValue.setText(juce::String((int) std::round(mixSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);

    displayFreq += 0.24f * (proc.currentFreq - displayFreq);
    displayEnvelope += 0.24f * (proc.currentEnvelope - displayEnvelope);

    repaint(sweepBounds.toNearestInt());
}

inline void AutoWahEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f, juce::Colour(0xff151B24), 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.16f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    juce::ColourGradient topGlow(accent.withAlpha(0.34f), 0.0f, 0.0f, accent.withAlpha(0.0f), (float) getWidth(), 0.0f, false);
    g.setGradientFill(topGlow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 3.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    g.drawText("AUTO WAH", 26, 12, 220, 30, juce::Justification::centredLeft);

    g.setColour(accentGlow);
    g.setFont(juce::Font(13.0f));
    g.drawText("Envelope-driven vocal filter", 26, 38, 240, 18, juce::Justification::centredLeft);

    paintSweep(g, sweepBounds);
}

inline void AutoWahEditor::paintSweep(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(panel.withAlpha(0.96f));
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(panelEdge);
    g.drawRoundedRectangle(bounds, 10.0f, 1.0f);

    auto inner = bounds.reduced(18.0f, 12.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();
    const float freq = juce::jlimit(120.0f, 6000.0f, displayFreq);
    const float range = proc.rangeParam != nullptr ? proc.rangeParam->get() : 0.78f;
    const float voice = proc.voiceParam != nullptr ? proc.voiceParam->get() : 0.42f;
    const float resonance = proc.resonanceParam != nullptr ? proc.resonanceParam->get() : 4.2f;

    auto freqToX = [inner, width](float frequency)
    {
        constexpr float minLog = 2.07918f;
        constexpr float maxLog = 3.77815f;
        const float t = (std::log10(juce::jlimit(120.0f, 6000.0f, frequency)) - minLog) / (maxLog - minLog);
        return inner.getX() + juce::jlimit(0.0f, 1.0f, t) * width;
    };

    const float gridFreqs[] = { 150.0f, 250.0f, 400.0f, 700.0f, 1200.0f, 2000.0f, 3200.0f, 5000.0f };
    const char* gridLabels[] = { "150", "250", "400", "700", "1.2k", "2k", "3.2k", "5k" };

    g.setFont(juce::Font(8.0f));
    for (int i = 0; i < 8; ++i)
    {
        const float x = freqToX(gridFreqs[i]);
        g.setColour(juce::Colour(0xff1A2532));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.30f));
        g.drawText(gridLabels[i], (int) (x - 16.0f), (int) inner.getBottom() + 3, 32, 10, juce::Justification::centred);
    }

    g.setColour(juce::Colour(0xff1A2532));
    g.drawLine(inner.getX(), inner.getY() + height * 0.50f, inner.getRight(), inner.getY() + height * 0.50f, 0.5f);

    const float minFreq = juce::jmap(voice, 320.0f, 520.0f);
    const float maxFreq = juce::jmap(voice, 1650.0f, 2600.0f) + range * 1900.0f;
    const float leftX = freqToX(minFreq);
    const float rightX = freqToX(maxFreq);

    g.setColour(accent.withAlpha(0.05f));
    g.fillRect(leftX, inner.getY(), rightX - leftX, height);
    g.setColour(accent.withAlpha(0.20f));
    g.drawLine(leftX, inner.getY(), leftX, inner.getBottom(), 0.6f);
    g.drawLine(rightX, inner.getY(), rightX, inner.getBottom(), 0.6f);

    juce::Path response;
    constexpr int numPoints = 220;
    const float q = juce::jmax(0.60f, resonance * (0.84f + displayEnvelope * 0.52f));

    for (int i = 0; i < numPoints; ++i)
    {
        const float t = (float) i / (float) (numPoints - 1);
        const float logF = 2.07918f + t * (3.77815f - 2.07918f);
        const float f = std::pow(10.0f, logF);
        const float ratio = f / juce::jmax(120.0f, freq);
        const float logRatio = std::log2(ratio);
        const float bpMag = q / (1.0f + q * q * logRatio * logRatio * 4.0f);
        const float lpMag = 1.0f / std::sqrt(1.0f + std::pow(ratio, 4.0f));
        const float bodyBlend = juce::jlimit(0.08f, 0.34f, juce::jmap(voice, 0.28f, 0.12f));
        const float mag = bpMag * (1.0f - bodyBlend) + lpMag * bodyBlend;
        const float magDb = 20.0f * std::log10(juce::jmax(mag, 1.0e-6f));
        constexpr float dbRange = 24.0f;
        const float yNorm = juce::jlimit(0.0f, 1.0f, 1.0f - (magDb + dbRange) / (2.0f * dbRange));
        const float x = inner.getX() + t * width;
        const float y = inner.getY() + yNorm * height;

        if (i == 0)
            response.startNewSubPath(x, y);
        else
            response.lineTo(x, y);
    }

    juce::Path fill(response);
    fill.lineTo(inner.getRight(), inner.getBottom());
    fill.lineTo(inner.getX(), inner.getBottom());
    fill.closeSubPath();

    juce::ColourGradient fillGrad(accent.withAlpha(0.14f), inner.getX(), inner.getY(),
        accent.withAlpha(0.02f), inner.getX(), inner.getBottom(), false);
    g.setGradientFill(fillGrad);
    g.fillPath(fill);

    g.setColour(accent.withAlpha(0.10f));
    g.strokePath(response, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.88f));
    g.strokePath(response, juce::PathStrokeType(1.7f, juce::PathStrokeType::curved));

    const float liveX = freqToX(freq);
    juce::ColourGradient freqGlow(accentGlow.withAlpha(0.20f), liveX, inner.getY(),
        accent.withAlpha(0.0f), liveX, inner.getBottom(), false);
    g.setGradientFill(freqGlow);
    g.fillRect(liveX - 16.0f, inner.getY(), 32.0f, height - 18.0f);
    g.setColour(accentGlow);
    g.drawLine(liveX, inner.getY(), liveX, inner.getBottom() - 18.0f, 1.8f);

    const float envBarY = inner.getBottom() - 9.0f;
    g.setColour(juce::Colour(0xff1B2635));
    g.fillRoundedRectangle(inner.getX(), envBarY, width, 5.0f, 2.5f);
    g.setColour(accent.withAlpha(0.76f));
    g.fillRoundedRectangle(inner.getX(), envBarY, juce::jmax(2.0f, displayEnvelope * width), 5.0f, 2.5f);

    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.setColour(accentGlow.withAlpha(0.78f));
    const juce::String freqLabel = freq >= 1000.0f
        ? juce::String(freq * 0.001f, 2) + " kHz"
        : juce::String((int) std::round(freq)) + " Hz";
    g.drawText(freqLabel, (int) liveX - 32, (int) inner.getY() + 2, 64, 14, juce::Justification::centred);
    g.drawText("ENVELOPE", (int) inner.getX() + 2, (int) envBarY - 14, 70, 12, juce::Justification::centredLeft);
}

inline void AutoWahEditor::resized()
{
    sweepBounds = juce::Rectangle<float>(26.0f, 84.0f, (float) (getWidth() - 52), 150.0f);

    const int knobAreaY = 252;
    const int knobSize = 82;
    const int labelHeight = 16;
    const int valueHeight = 16;
    const int slotWidth = getWidth() / 7;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup knobs[] = {
        { sensitivitySlider, sensitivityLabel, sensitivityValue },
        { attackSlider, attackLabel, attackValue },
        { releaseSlider, releaseLabel, releaseValue },
        { rangeSlider, rangeLabel, rangeValue },
        { resonanceSlider, resonanceLabel, resonanceValue },
        { voiceSlider, voiceLabel, voiceValue },
        { mixSlider, mixLabel, mixValue }
    };

    for (int i = 0; i < 7; ++i)
    {
        const int centreX = slotWidth * i + slotWidth / 2;
        knobs[i].label.setBounds(centreX - 54, knobAreaY, 108, labelHeight);
        knobs[i].slider.setBounds(centreX - knobSize / 2, knobAreaY + labelHeight + 6, knobSize, knobSize);
        knobs[i].value.setBounds(centreX - 58, knobAreaY + labelHeight + 6 + knobSize + 4, 116, valueHeight);
    }
}

inline juce::AudioProcessorEditor* AutoWahPedal::createEditor()
{
    return new AutoWahEditor(*this);
}
