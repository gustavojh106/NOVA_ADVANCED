#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class DistortionPedal;

class DistortionEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    DistortionEditor(DistortionPedal& pedal);
    ~DistortionEditor() override
    {
        stopTimer();
        sldGain.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldBody.setLookAndFeel(nullptr);
        sldTight.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
        sldLevel.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintCurveViz(juce::Graphics& g, juce::Rectangle<float> bounds);
    void syncModeFromProcessor();
    void refreshModeSummary();

    DistortionPedal& proc;

    static constexpr int kWidth = 730;
    static constexpr int kHeight = 445;

    const juce::Colour accent = juce::Colour::fromString("ffF97316");
    const juce::Colour accentDim = juce::Colour::fromString("ffEA580C");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFDBA74");
    const juce::Colour bgTop = juce::Colour(0xff110A06);
    const juce::Colour bgBottom = juce::Colour(0xff22120A);
    const juce::Colour panel = juce::Colour(0xff2A170D);
    const juce::Colour panelEdge = juce::Colour(0xff53301A);
    const juce::Colour textBright = juce::Colour(0xffF8EEE7);
    const juce::Colour textDim = juce::Colour(0xffBA9B87);

    juce::Slider sldGain, sldTone, sldBody, sldTight, sldMix, sldLevel;
    juce::Label lblGain, lblTone, lblBody, lblTight, lblMix, lblLevel;
    juce::Label valGain, valTone, valBody, valTight, valMix, valLevel;

    juce::Label modeLabel;
    juce::ComboBox modeBox;
    juce::Label modeSummaryLabel;
    juce::Label attackHintLabel;

    juce::Rectangle<float> vizBounds;
    int cachedMode = -1;

    struct DistKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour accent = juce::Colour::fromString("ffF97316");
        juce::Colour accentGlow = juce::Colour::fromString("ffFDBA74");

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
            g.setColour(juce::Colour(0xff3D2618));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.001f)
            {
                juce::Path arc;
                arc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(accent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(accent);
                g.strokePath(arc, juce::PathStrokeType(3.4f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.58f;
            juce::ColourGradient body(juce::Colour(0xff4A2D1D), centreX, centreY - knobRadius,
                juce::Colour(0xff1B100A), centreX, centreY + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff6B4227));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.74f;
            const float pointX = centreX + std::sin(angle) * pointerLength;
            const float pointY = centreY - std::cos(angle) * pointerLength;
            g.setColour(accentGlow);
            g.drawLine(centreX, centreY, pointX, pointY, 2.0f);
            g.fillEllipse(pointX - 2.8f, pointY - 2.8f, 5.6f, 5.6f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DistortionEditor)
};

inline DistortionEditor::DistortionEditor(DistortionPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& slider, juce::Label& label, juce::Label& value,
        const juce::String& text, double min, double max, double defaultValue, double step)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider.setRange(min, max, step);
        slider.setValue(defaultValue, juce::dontSendNotification);
        slider.setLookAndFeel(&knobLnF);
        addAndMakeVisible(slider);

        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(label);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(juce::FontOptions(11.0f)));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(sldGain, lblGain, valGain, "GAIN", 0.0, 100.0, 58.0, 0.1);
    initKnob(sldTone, lblTone, valTone, "TONE", 0.0, 1.0, 0.54, 0.01);
    initKnob(sldBody, lblBody, valBody, "BODY", 0.0, 1.0, 0.52, 0.01);
    initKnob(sldTight, lblTight, valTight, "TIGHT", 0.0, 1.0, 0.42, 0.01);
    initKnob(sldMix, lblMix, valMix, "MIX", 0.0, 1.0, 1.0, 0.01);
    initKnob(sldLevel, lblLevel, valLevel, "LEVEL", 0.0, 1.0, 0.64, 0.01);

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

    wireFloat(sldGain, proc.gainParam);
    wireFloat(sldTone, proc.toneParam);
    wireFloat(sldBody, proc.bodyParam);
    wireFloat(sldTight, proc.tightParam);
    wireFloat(sldMix, proc.mixParam);
    wireFloat(sldLevel, proc.levelParam);

    modeLabel.setText("MODE", juce::dontSendNotification);
    modeLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    modeLabel.setColour(juce::Label::textColourId, textDim);
    addAndMakeVisible(modeLabel);

    modeBox.addItemList(proc.modeParam != nullptr ? proc.modeParam->choices : juce::StringArray{ "Vintage" }, 1);
    modeBox.setColour(juce::ComboBox::backgroundColourId, panel);
    modeBox.setColour(juce::ComboBox::outlineColourId, panelEdge);
    modeBox.setColour(juce::ComboBox::textColourId, textBright);
    modeBox.setColour(juce::ComboBox::arrowColourId, accentGlow);
    modeBox.onChange = [this]
    {
        if (proc.modeParam == nullptr)
            return;

        proc.modeParam->beginChangeGesture();
        proc.modeParam->setValueNotifyingHost((float) modeBox.getSelectedItemIndex()
            / (float) juce::jmax(1, proc.modeParam->choices.size() - 1));
        proc.modeParam->endChangeGesture();
    };
    addAndMakeVisible(modeBox);

    modeSummaryLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    modeSummaryLabel.setColour(juce::Label::textColourId, textDim.withAlpha(0.96f));
    modeSummaryLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(modeSummaryLabel);

    attackHintLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    attackHintLabel.setColour(juce::Label::textColourId, accentGlow.withAlpha(0.86f));
    attackHintLabel.setJustificationType(juce::Justification::centredRight);
    attackHintLabel.setText("Tight controls attack, bloom and palm-mute weight", juce::dontSendNotification);
    addAndMakeVisible(attackHintLabel);

    syncModeFromProcessor();
    refreshModeSummary();
    startTimerHz(30);
}

inline void DistortionEditor::timerCallback()
{
    const auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldGain, proc.gainParam);
    syncSlider(sldTone, proc.toneParam);
    syncSlider(sldBody, proc.bodyParam);
    syncSlider(sldTight, proc.tightParam);
    syncSlider(sldMix, proc.mixParam);
    syncSlider(sldLevel, proc.levelParam);
    syncModeFromProcessor();

    valGain.setText(juce::String(juce::roundToInt(sldGain.getValue())) + "%", juce::dontSendNotification);

    const float toneCutoff = DistortionPedal::toneToCutoffHz((float) sldTone.getValue());
    valTone.setText(toneCutoff >= 1000.0f
            ? juce::String(toneCutoff / 1000.0f, 1) + " kHz"
            : juce::String((int) std::round(toneCutoff)) + " Hz",
        juce::dontSendNotification);

    const float bodyDb = DistortionPedal::bodyToGainDb((float) sldBody.getValue());
    valBody.setText((bodyDb >= 0.0f ? "+" : "") + juce::String(bodyDb, 1) + " dB", juce::dontSendNotification);
    valTight.setText(juce::String((int) std::round(sldTight.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valMix.setText(juce::String((int) std::round(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valLevel.setText(juce::String((int) std::round(sldLevel.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void DistortionEditor::syncModeFromProcessor()
{
    if (proc.modeParam == nullptr)
        return;

    const int mode = proc.modeParam->getIndex();
    modeBox.setSelectedItemIndex(mode, juce::dontSendNotification);
    if (mode != cachedMode)
    {
        cachedMode = mode;
        refreshModeSummary();
    }
}

inline void DistortionEditor::refreshModeSummary()
{
    const int mode = cachedMode < 0 ? 0 : cachedMode;
    modeSummaryLabel.setText(DistortionPedal::getModeDescription(mode),
        juce::dontSendNotification);
    attackHintLabel.setText(DistortionPedal::getControlHint(mode), juce::dontSendNotification);
}

inline void DistortionEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(bgTop, 0.0f, 0.0f, bgBottom, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.16f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    juce::ColourGradient topGlow(accent.withAlpha(0.34f), 0.0f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth(), 0.0f, false);
    g.setGradientFill(topGlow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 3.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
    g.drawText("DISTORTION", 26, 12, 250, 30, juce::Justification::centredLeft);

    g.setColour(accentGlow);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText(modeBox.getText().isNotEmpty() ? modeBox.getText() : juce::String("Vintage"),
        26, 38, 220, 18, juce::Justification::centredLeft);

    const int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;
    if (DistortionPedal::modeUsesAdaptiveGate(mode))
    {
        const float gateGain = juce::jlimit(0.0f, 1.0f, proc.currentGateGain);
        const juce::Colour gateColour = gateGain > 0.72f
            ? juce::Colour(0xff22C55E).withAlpha(0.78f)
            : gateGain > 0.32f
                ? accentGlow.withAlpha(0.82f)
                : juce::Colour(0xffEF4444).withAlpha(0.64f);

        g.setColour(gateColour);
        g.fillRoundedRectangle((float) getWidth() - 124.0f, 16.0f, 8.0f, 8.0f, 4.0f);
        g.setColour(textDim.withAlpha(0.72f));
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("ADAPTIVE GATE", getWidth() - 112, 13, 92, 14, juce::Justification::centredLeft);
    }

    paintCurveViz(g, vizBounds);
}

inline void DistortionEditor::paintCurveViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(panel.withAlpha(0.95f));
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(panelEdge);
    g.drawRoundedRectangle(bounds, 10.0f, 1.0f);

    auto inner = bounds.reduced(18.0f, 12.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();
    const float centreX = inner.getCentreX();
    const float centreY = inner.getCentreY();
    const float gain = (float) sldGain.getValue();
    const int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;

    g.setColour(juce::Colour(0xff3D2618));
    g.drawLine(inner.getX(), centreY, inner.getRight(), centreY, 0.6f);
    g.drawLine(centreX, inner.getY(), centreX, inner.getBottom(), 0.6f);
    g.drawLine(inner.getX(), inner.getBottom(), inner.getRight(), inner.getY(), 0.45f);

    g.setFont(juce::Font(juce::FontOptions(8.5f)));
    g.setColour(textDim.withAlpha(0.42f));
    g.drawText("IN", (int) inner.getX() + 2, (int) inner.getBottom() - 12, 20, 10, juce::Justification::centredLeft);
    g.drawText("OUT", (int) inner.getRight() - 28, (int) inner.getY() + 2, 24, 10, juce::Justification::centredRight);

    juce::Path curvePath;
    constexpr int numPoints = 220;
    for (int i = 0; i < numPoints; ++i)
    {
        const float t = (float) i / (float) (numPoints - 1);
        const float input = t * 2.0f - 1.0f;
        const float output = DistortionPedal::computeClipCurve(input, gain, mode);
        const float x = inner.getX() + (input * 0.5f + 0.5f) * width;
        const float y = inner.getY() + (1.0f - (output * 0.5f + 0.5f)) * height;

        if (i == 0)
            curvePath.startNewSubPath(x, y);
        else
            curvePath.lineTo(x, y);
    }

    g.setColour(accent.withAlpha(0.08f));
    g.strokePath(curvePath, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.14f));
    g.strokePath(curvePath, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.88f));
    g.strokePath(curvePath, juce::PathStrokeType(1.9f, juce::PathStrokeType::curved));

    const float topSample = DistortionPedal::computeClipCurve(1.0f, gain, mode);
    const float bottomSample = std::abs(DistortionPedal::computeClipCurve(-1.0f, gain, mode));
    const float asymmetry = std::abs(topSample - bottomSample) / juce::jmax(0.01f, topSample + bottomSample) * 2.0f;

    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.setColour(textDim.withAlpha(0.56f));
    const juce::String character = gain < 22.0f ? "Crunch"
        : gain < 46.0f ? "Drive"
        : gain < 74.0f ? "Distortion"
        : "Wall";
    g.drawText(character, (int) inner.getRight() - 62, (int) inner.getBottom() - 14, 58, 12,
        juce::Justification::centredRight);

    if (asymmetry > 0.02f)
    {
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        g.setColour(accent.withAlpha(0.44f));
        g.drawText("Asym " + juce::String((int) std::round(asymmetry * 100.0f)) + "%",
            (int) inner.getX() + 3, (int) inner.getY() + 2, 62, 10,
            juce::Justification::centredLeft);
    }
}

inline void DistortionEditor::resized()
{
    const int width = getWidth();

    modeLabel.setBounds(width - 210, 16, 70, 16);
    modeBox.setBounds(width - 210, 34, 170, 26);
    modeSummaryLabel.setBounds(26, 58, width - 360, 18);
    attackHintLabel.setBounds(width - 330, 60, 300, 16);

    vizBounds = juce::Rectangle<float>(26.0f, 86.0f, (float) (width - 52), 148.0f);

    const int knobAreaY = 255;
    const int knobSize = 84;
    const int labelHeight = 16;
    const int valueHeight = 16;
    const int slotWidth = width / 6;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup knobs[] = {
        { sldGain, lblGain, valGain },
        { sldTone, lblTone, valTone },
        { sldBody, lblBody, valBody },
        { sldTight, lblTight, valTight },
        { sldMix, lblMix, valMix },
        { sldLevel, lblLevel, valLevel }
    };

    for (int i = 0; i < 6; ++i)
    {
        const int centreX = slotWidth * i + slotWidth / 2;
        knobs[i].label.setBounds(centreX - 54, knobAreaY, 108, labelHeight);
        knobs[i].slider.setBounds(centreX - knobSize / 2, knobAreaY + labelHeight + 6, knobSize, knobSize);
        knobs[i].value.setBounds(centreX - 58, knobAreaY + labelHeight + 6 + knobSize + 4, 116, valueHeight);
    }
}

inline juce::AudioProcessorEditor* DistortionPedal::createEditor()
{
    return new DistortionEditor(*this);
}
