#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class NoiseGatePedal;

class GateEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    GateEditor(NoiseGatePedal& pedal);
    ~GateEditor() override
    {
        stopTimer();

        for (auto* slider : { &sldThresh, &sldAttack, &sldHold, &sldRelease, &sldRange, &sldHysteresis, &sldFocus })
            slider->setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintGateViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    NoiseGatePedal& proc;

    static constexpr int kWidth = 760;
    static constexpr int kHeight = 462;

    const juce::Colour accent = juce::Colour::fromString("ff10B981");
    const juce::Colour accentDim = juce::Colour::fromString("ff059669");
    const juce::Colour accentGlow = juce::Colour::fromString("ff34D399");
    const juce::Colour bgDark = juce::Colour(0xff0B0E14);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim = juce::Colour(0xff7B8BA0);

    const juce::Colour gateOpenCol = juce::Colour(0xff22C55E);
    const juce::Colour gateTrackCol = juce::Colour(0xffFBBF24);
    const juce::Colour gateClosedCol = juce::Colour(0xffEF4444);

    juce::Slider sldThresh, sldAttack, sldHold, sldRelease, sldRange, sldHysteresis, sldFocus;
    juce::Label lblThresh, lblAttack, lblHold, lblRelease, lblRange, lblHysteresis, lblFocus;
    juce::Label valThresh, valAttack, valHold, valRelease, valRange, valHysteresis, valFocus;
    juce::Label circuitLabel, focusHintLabel;

    juce::Rectangle<float> vizBounds;

    float displayGateGain = 1.0f;
    float displayDetectorDb = -96.0f;
    bool updatingUi = false;

    struct GateKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent = juce::Colour::fromString("ff10B981");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff34D399");

        void drawRotarySlider(juce::Graphics& g,
            int x,
            int y,
            int w,
            int h,
            float sliderPos,
            float startAngle,
            float endAngle,
            juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(6.0f);
            const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float centreX = area.getCentreX();
            const float centreY = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcRadius = radius - 4.0f;

            juce::Path track;
            track.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff1E2A3A));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.56f;
            juce::ColourGradient grad(juce::Colour(0xff1C2838), centreX, centreY - knobRadius,
                juce::Colour(0xff0D1520), centreX, centreY + knobRadius, false);
            g.setGradientFill(grad);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2A3A4C));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.70f;
            const float pointerX = centreX + std::sin(angle) * pointerLength;
            const float pointerY = centreY - std::cos(angle) * pointerLength;
            g.setColour(kAccentGlow);
            g.drawLine(centreX, centreY, pointerX, pointerY, 2.0f);
            g.fillEllipse(pointerX - 3.0f, pointerY - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GateEditor)
};

inline GateEditor::GateEditor(NoiseGatePedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& slider,
        juce::Label& label,
        juce::Label& value,
        const juce::String& name,
        float min,
        float max,
        float def,
        float step)
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

    initKnob(sldThresh, lblThresh, valThresh, "THRESH", -80.0f, 0.0f, -46.0f, 0.1f);
    initKnob(sldAttack, lblAttack, valAttack, "ATTACK", 0.02f, 25.0f, 0.25f, 0.01f);
    initKnob(sldHold, lblHold, valHold, "HOLD", 0.0f, 400.0f, 70.0f, 1.0f);
    initKnob(sldRelease, lblRelease, valRelease, "RELEASE", 10.0f, 700.0f, 115.0f, 1.0f);
    initKnob(sldRange, lblRange, valRange, "RANGE", -96.0f, 0.0f, -96.0f, 0.1f);
    initKnob(sldHysteresis, lblHysteresis, valHysteresis, "HYST", 1.0f, 18.0f, 8.0f, 0.1f);
    initKnob(sldFocus, lblFocus, valFocus, "FOCUS", 0.0f, 1.0f, 0.55f, 0.01f);

    circuitLabel.setJustificationType(juce::Justification::centredLeft);
    circuitLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    circuitLabel.setColour(juce::Label::textColourId, textDim.withAlpha(0.94f));
    circuitLabel.setText("Linked studio gate with lookahead, hybrid detector and adaptive closing", juce::dontSendNotification);
    addAndMakeVisible(circuitLabel);

    focusHintLabel.setJustificationType(juce::Justification::centredLeft);
    focusHintLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    focusHintLabel.setColour(juce::Label::textColourId, accentGlow.withAlpha(0.90f));
    addAndMakeVisible(focusHintLabel);

    auto wireParam = [this](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param == nullptr)
            return;

        updatingUi = true;
        slider.setValue(param->get(), juce::dontSendNotification);
        updatingUi = false;

        slider.onDragStart = [param]
        {
            param->beginChangeGesture();
        };

        slider.onDragEnd = [param]
        {
            param->endChangeGesture();
        };

        slider.onValueChange = [this, &slider, param]
        {
            if (updatingUi)
                return;

            param->setValueNotifyingHost(param->convertTo0to1((float) slider.getValue()));
        };
    };

    wireParam(sldThresh, proc.thresholdParam);
    wireParam(sldAttack, proc.attackParam);
    wireParam(sldHold, proc.holdParam);
    wireParam(sldRelease, proc.releaseParam);
    wireParam(sldRange, proc.rangeParam);
    wireParam(sldHysteresis, proc.hysteresisParam);
    wireParam(sldFocus, proc.focusParam);

    startTimerHz(30);
}

inline void GateEditor::timerCallback()
{
    auto syncSlider = [this](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param == nullptr)
            return;

        updatingUi = true;
        slider.setValue(param->get(), juce::dontSendNotification);
        updatingUi = false;
    };

    syncSlider(sldThresh, proc.thresholdParam);
    syncSlider(sldAttack, proc.attackParam);
    syncSlider(sldHold, proc.holdParam);
    syncSlider(sldRelease, proc.releaseParam);
    syncSlider(sldRange, proc.rangeParam);
    syncSlider(sldHysteresis, proc.hysteresisParam);
    syncSlider(sldFocus, proc.focusParam);

    valThresh.setText(juce::String((float) sldThresh.getValue(), 1) + " dB", juce::dontSendNotification);

    const float attackMs = (float) sldAttack.getValue();
    valAttack.setText(attackMs < 1.0f ? juce::String(attackMs, 2) + " ms" : juce::String(attackMs, 1) + " ms",
        juce::dontSendNotification);
    valHold.setText(juce::String((int) std::round(sldHold.getValue())) + " ms", juce::dontSendNotification);
    valRelease.setText(juce::String((int) std::round(sldRelease.getValue())) + " ms", juce::dontSendNotification);

    const float rangeDb = (float) sldRange.getValue();
    valRange.setText(rangeDb <= -95.0f ? "MUTE" : juce::String(rangeDb, 1) + " dB", juce::dontSendNotification);
    valHysteresis.setText(juce::String((float) sldHysteresis.getValue(), 1) + " dB", juce::dontSendNotification);
    valFocus.setText(juce::String((int) std::round((float) sldFocus.getValue() * 100.0f)) + "%", juce::dontSendNotification);

    focusHintLabel.setText(NoiseGatePedal::getFocusDescription((float) sldFocus.getValue()), juce::dontSendNotification);

    const float targetGateGain = proc.currentGateGainAtomic.load();
    const float targetDetectorDb = proc.currentDetectorDbAtomic.load();
    displayGateGain += 0.28f * (targetGateGain - displayGateGain);
    displayDetectorDb += 0.28f * (targetDetectorDb - displayDetectorDb);

    repaint(vizBounds.toNearestInt());
}

inline void GateEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f, bgDark, 0.0f, (float) getHeight(), false);
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
    g.drawText("NOISE GATE", 28, 10, 260, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("Studio Silencer", 28, 33, 220, 18, juce::Justification::centredLeft);

    const juce::String stateText = displayGateGain > 0.92f ? "OPEN"
        : (displayGateGain > 0.18f ? "TRACK" : "CLAMP");
    const juce::Colour stateColour = displayGateGain > 0.92f ? gateOpenCol
        : (displayGateGain > 0.18f ? gateTrackCol : gateClosedCol);

    g.setColour(stateColour.withAlpha(0.80f));
    g.fillRoundedRectangle((float) getWidth() - 108.0f, 14.0f, 12.0f, 12.0f, 6.0f);
    g.setColour(textDim.withAlpha(0.68f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText(stateText, getWidth() - 88, 12, 62, 16, juce::Justification::centredLeft);

    paintGateViz(g, vizBounds);
}

inline void GateEditor::paintGateViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();

    const float thresholdDb = (float) sldThresh.getValue();
    const float hysteresisDb = (float) sldHysteresis.getValue();
    const float rangeDb = (float) sldRange.getValue();
    const float detectorDb = displayDetectorDb;
    const float gateGain = displayGateGain;
    const float closeThresholdDb = thresholdDb - hysteresisDb;
    const float kneeStartDb = closeThresholdDb - juce::jmax(2.0f, hysteresisDb * 0.6f);

    constexpr float dbMin = -80.0f;
    constexpr float dbMax = 0.0f;

    auto dbToX = [&](float db)
    {
        const float norm = juce::jlimit(0.0f, 1.0f, (db - dbMin) / (dbMax - dbMin));
        return inner.getX() + norm * width;
    };

    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    const float gridDbs[] = { -72.0f, -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, 0.0f };
    const char* gridLabels[] = { "-72", "-60", "-48", "-36", "-24", "-12", "0" };
    for (int i = 0; i < 7; ++i)
    {
        const float x = dbToX(gridDbs[i]);
        g.setColour(juce::Colour(0xff1A2535));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.30f));
        g.drawText(gridLabels[i], (int) (x - 15.0f), (int) (inner.getBottom() + 1.0f), 30, 10, juce::Justification::centred);
    }

    g.setColour(accent.withAlpha(0.04f));
    g.fillRect(dbToX(closeThresholdDb), inner.getY(), dbToX(thresholdDb) - dbToX(closeThresholdDb), height);

    juce::Path curvePath;
    constexpr int numPts = 200;
    for (int i = 0; i < numPts; ++i)
    {
        const float t = (float) i / (float) (numPts - 1);
        const float inputDb = dbMin + t * (dbMax - dbMin);
        const float zone = inputDb >= thresholdDb ? 1.0f : Nova::GateDSP::smoothstep(kneeStartDb, thresholdDb, inputDb);
        const float gainDb = rangeDb * (1.0f - zone);
        const float outputDb = inputDb + gainDb;
        const float x = inner.getX() + t * width;
        const float outputNorm = (outputDb - dbMin) / (dbMax - dbMin);
        const float y = inner.getBottom() - juce::jlimit(0.0f, 1.0f, outputNorm) * height;

        if (i == 0)
            curvePath.startNewSubPath(x, y);
        else
            curvePath.lineTo(x, y);
    }

    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), inner.getBottom(), inner.getRight(), inner.getY(), 0.4f);

    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(curvePath, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.16f));
    g.strokePath(curvePath, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.85f));
    g.strokePath(curvePath, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved));

    const float thresholdX = dbToX(thresholdDb);
    const float closeX = dbToX(closeThresholdDb);
    g.setColour(accent.withAlpha(0.42f));
    g.drawLine(thresholdX, inner.getY(), thresholdX, inner.getBottom(), 1.0f);
    g.setColour(accentDim.withAlpha(0.35f));
    g.drawLine(closeX, inner.getY(), closeX, inner.getBottom(), 1.0f);

    g.setColour(accent.withAlpha(0.55f));
    g.drawText("T", (int) (thresholdX - 5.0f), (int) inner.getY() + 2, 10, 10, juce::Justification::centred);
    g.setColour(accentDim.withAlpha(0.55f));
    g.drawText("C", (int) (closeX - 5.0f), (int) inner.getY() + 14, 10, 10, juce::Justification::centred);

    const float detectorX = dbToX(juce::jlimit(dbMin, dbMax, detectorDb));
    g.setColour(accentGlow.withAlpha(0.90f));
    g.drawLine(detectorX, inner.getY() + 4.0f, detectorX, inner.getBottom() - 14.0f, 1.2f);
    g.fillEllipse(detectorX - 3.0f, inner.getBottom() - 16.0f, 6.0f, 6.0f);

    const float meterY = inner.getBottom() - 8.0f;
    g.setColour(juce::Colour(0xff1A2535));
    g.fillRoundedRectangle(inner.getX(), meterY, width, 5.0f, 2.0f);
    g.setColour((gateGain > 0.92f ? gateOpenCol : (gateGain > 0.18f ? gateTrackCol : gateClosedCol)).withAlpha(0.62f));
    g.fillRoundedRectangle(inner.getX(), meterY, juce::jmax(2.0f, detectorX - inner.getX()), 5.0f, 2.0f);

    const float grBarX = inner.getRight() - 52.0f;
    const float grBarY = inner.getY() + 4.0f;
    const float grBarW = 48.0f;
    const float grBarH = height - 24.0f;
    g.setColour(juce::Colour(0xff0D1520));
    g.fillRoundedRectangle(grBarX, grBarY, grBarW, grBarH, 3.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(grBarX, grBarY, grBarW, grBarH, 3.0f, 0.5f);

    const float fillH = gateGain * grBarH;
    const juce::Colour grColour = gateGain > 0.80f ? gateOpenCol : (gateGain > 0.25f ? gateTrackCol : gateClosedCol);
    g.setColour(grColour.withAlpha(0.58f));
    g.fillRoundedRectangle(grBarX + 2.0f, grBarY + (grBarH - fillH), grBarW - 4.0f, fillH, 2.0f);

    g.setColour(textDim.withAlpha(0.45f));
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    g.drawText("GR", (int) grBarX, (int) (grBarY + grBarH + 2.0f), (int) grBarW, 10, juce::Justification::centred);

    const float grDb = juce::Decibels::gainToDecibels(juce::jmax(gateGain, 1.0e-6f), -96.0f);
    g.drawText(grDb > -0.5f ? "0 dB" : juce::String(grDb, 1) + " dB",
        (int) grBarX, (int) (grBarY + 2.0f), (int) grBarW, 12, juce::Justification::centred);
}

inline void GateEditor::resized()
{
    const int width = getWidth();
    vizBounds = juce::Rectangle<float>(28.0f, 74.0f, (float) (width - 56), 138.0f);

    circuitLabel.setBounds(28, 48, width - 56, 18);
    focusHintLabel.setBounds(28, 220, width - 56, 18);

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    auto layoutRow = [](juce::Rectangle<int> row, std::initializer_list<KnobGroup> controls)
    {
        const int count = (int) controls.size();
        const int slotW = row.getWidth() / juce::jmax(1, count);
        const int knobSize = 70;
        const int labelH = 16;
        const int valueH = 16;

        int index = 0;
        for (auto& control : controls)
        {
            const int centreX = row.getX() + slotW * index + slotW / 2;
            const int labelY = row.getY();
            const int knobY = labelY + labelH + 4;
            const int valueY = knobY + knobSize + 2;

            control.label.setBounds(centreX - 50, labelY, 100, labelH);
            control.slider.setBounds(centreX - knobSize / 2, knobY, knobSize, knobSize);
            control.value.setBounds(centreX - 50, valueY, 100, valueH);
            ++index;
        }
    };

    layoutRow({ 24, 246, width - 48, 96 },
        { { sldThresh, lblThresh, valThresh },
          { sldAttack, lblAttack, valAttack },
          { sldHold, lblHold, valHold },
          { sldRelease, lblRelease, valRelease } });

    layoutRow({ 72, 352, width - 144, 96 },
        { { sldRange, lblRange, valRange },
          { sldHysteresis, lblHysteresis, valHysteresis },
          { sldFocus, lblFocus, valFocus } });
}

inline juce::AudioProcessorEditor* NoiseGatePedal::createEditor()
{
    return new GateEditor(*this);
}
