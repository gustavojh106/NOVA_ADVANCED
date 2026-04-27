#pragma once

#include <JuceHeader.h>

class NeuralPedal;

class NeuralEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    NeuralEditor(NeuralPedal& pedal);
    ~NeuralEditor() override
    {
        stopTimer();
        driveSlider.setLookAndFeel(nullptr);
        focusSlider.setLookAndFeel(nullptr);
        detailSlider.setLookAndFeel(nullptr);
        compSlider.setLookAndFeel(nullptr);
        mixSlider.setLookAndFeel(nullptr);
        levelSlider.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintCurve(juce::Graphics& g, juce::Rectangle<float> bounds);

    NeuralPedal& proc;

    juce::Slider driveSlider, focusSlider, detailSlider, compSlider, mixSlider, levelSlider;
    juce::Label driveLabel, focusLabel, detailLabel, compLabel, mixLabel, levelLabel;
    juce::Label driveValue, focusValue, detailValue, compValue, mixValue, levelValue;
    juce::Rectangle<float> curveBounds;

    const juce::Colour accent = juce::Colour::fromString("ff22D3EE");
    const juce::Colour accentGlow = juce::Colour::fromString("ff67E8F9");
    const juce::Colour panel = juce::Colour(0xff07141A);
    const juce::Colour panelEdge = juce::Colour(0xff17313B);
    const juce::Colour textBright = juce::Colour(0xffE6FBFF);
    const juce::Colour textDim = juce::Colour(0xff86A6AE);

    struct NeuralKnobLookAndFeel : public juce::LookAndFeel_V4
    {
        juce::Colour accent = juce::Colour::fromString("ff22D3EE");
        juce::Colour glow = juce::Colour::fromString("ff67E8F9");

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
            g.setColour(juce::Colour(0xff15333C));
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
            juce::ColourGradient body(juce::Colour(0xff163540), centreX, centreY - knobRadius,
                juce::Colour(0xff08161B), centreX, centreY + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2D5662));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.74f;
            const float pointX = centreX + std::sin(angle) * pointerLength;
            const float pointY = centreY - std::cos(angle) * pointerLength;
            g.setColour(glow);
            g.drawLine(centreX, centreY, pointX, pointY, 2.0f);
            g.fillEllipse(pointX - 2.8f, pointY - 2.8f, 5.6f, 5.6f);
        }
    } knobLookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NeuralEditor)
};

#include "NeuralPedal.h"

inline NeuralEditor::NeuralEditor(NeuralPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(760, 420);

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
        name.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        name.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(name);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(juce::FontOptions(11.0f)));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(driveSlider, driveLabel, driveValue, "DRIVE", 0.0, 100.0, 54.0, 0.1);
    initKnob(focusSlider, focusLabel, focusValue, "FOCUS", 0.0, 1.0, 0.58, 0.01);
    initKnob(detailSlider, detailLabel, detailValue, "DETAIL", 0.0, 1.0, 0.54, 0.01);
    initKnob(compSlider, compLabel, compValue, "COMP", 0.0, 1.0, 0.42, 0.01);
    initKnob(mixSlider, mixLabel, mixValue, "MIX", 0.0, 1.0, 1.0, 0.01);
    initKnob(levelSlider, levelLabel, levelValue, "LEVEL", 0.0, 1.0, 0.75, 0.01);

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

    wireFloat(driveSlider, proc.driveParam);
    wireFloat(focusSlider, proc.focusParam);
    wireFloat(detailSlider, proc.detailParam);
    wireFloat(compSlider, proc.compParam);
    wireFloat(mixSlider, proc.mixParam);
    wireFloat(levelSlider, proc.levelParam);

    startTimerHz(30);
}

inline void NeuralEditor::timerCallback()
{
    const auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(driveSlider, proc.driveParam);
    syncSlider(focusSlider, proc.focusParam);
    syncSlider(detailSlider, proc.detailParam);
    syncSlider(compSlider, proc.compParam);
    syncSlider(mixSlider, proc.mixParam);
    syncSlider(levelSlider, proc.levelParam);

    driveValue.setText(juce::String(juce::roundToInt(driveSlider.getValue())) + "%", juce::dontSendNotification);
    focusValue.setText(juce::String((int) std::round(focusSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);
    detailValue.setText(juce::String((int) std::round(detailSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);
    compValue.setText(juce::String((int) std::round(compSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);
    mixValue.setText(juce::String((int) std::round(mixSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);
    levelValue.setText(juce::String((int) std::round(levelSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(curveBounds.toNearestInt());
}

inline void NeuralEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff061018), 0.0f, 0.0f, juce::Colour(0xff0C1B24), 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.16f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    juce::ColourGradient topGlow(accent.withAlpha(0.34f), 0.0f, 0.0f, accent.withAlpha(0.0f), (float) getWidth(), 0.0f, false);
    g.setGradientFill(topGlow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 3.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
    g.drawText("NEURAL", 26, 12, 180, 30, juce::Justification::centredLeft);

    g.setColour(accentGlow);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("Adaptive preamp", 26, 38, 180, 18, juce::Justification::centredLeft);

    paintCurve(g, curveBounds);
}

inline void NeuralEditor::paintCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(panel.withAlpha(0.95f));
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(panelEdge);
    g.drawRoundedRectangle(bounds, 10.0f, 1.0f);

    auto inner = bounds.reduced(18.0f, 12.0f);
    const float width = inner.getWidth();

    for (int i = 0; i < 5; ++i)
    {
        const float x = juce::jmap((float) i / 4.0f, inner.getX(), inner.getRight());
        g.setColour(juce::Colour(0xff17313B).withAlpha(0.35f));
        g.drawVerticalLine((int) std::round(x), inner.getY(), inner.getBottom());
    }

    for (int i = 0; i < 4; ++i)
    {
        const float y = juce::jmap((float) i / 3.0f, inner.getBottom(), inner.getY());
        g.setColour(juce::Colour(0xff17313B).withAlpha(0.35f));
        g.drawHorizontalLine((int) std::round(y), inner.getX(), inner.getRight());
    }

    g.setColour(juce::Colour(0xff17313B));
    g.drawLine(inner.getX(), inner.getCentreY(), inner.getRight(), inner.getCentreY(), 0.8f);

    const float drive = (float) driveSlider.getValue();
    const float detail = (float) detailSlider.getValue();
    const float comp = (float) compSlider.getValue();
    const float mix = (float) mixSlider.getValue();

    juce::Path curve;
    for (int i = 0; i <= 96; ++i)
    {
        const float t = (float) i / 96.0f;
        const float input = juce::jmap(t, -1.0f, 1.0f);
        const float shaped = juce::jlimit(-1.0f, 1.0f,
            NeuralPedal::computeDisplayCurve(input, drive, detail, comp) * (0.62f + mix * 0.38f));
        const float x = inner.getX() + t * width;
        const float y = juce::jmap(shaped, -1.0f, 1.0f, inner.getBottom(), inner.getY());

        if (i == 0)
            curve.startNewSubPath(x, y);
        else
            curve.lineTo(x, y);
    }

    g.setColour(accent.withAlpha(0.08f));
    g.strokePath(curve, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.14f));
    g.strokePath(curve, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.88f));
    g.strokePath(curve, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));

    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.setColour(accentGlow.withAlpha(0.74f));
    g.drawText("TRANSFER", (int) inner.getX() + 4, (int) inner.getY() + 2, 80, 14, juce::Justification::centredLeft);
}

inline void NeuralEditor::resized()
{
    curveBounds = juce::Rectangle<float>(26.0f, 84.0f, (float) (getWidth() - 52), 150.0f);

    const int knobAreaY = 252;
    const int knobSize = 82;
    const int labelHeight = 16;
    const int valueHeight = 16;
    const int slotWidth = getWidth() / 6;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup knobs[] = {
        { driveSlider, driveLabel, driveValue },
        { focusSlider, focusLabel, focusValue },
        { detailSlider, detailLabel, detailValue },
        { compSlider, compLabel, compValue },
        { mixSlider, mixLabel, mixValue },
        { levelSlider, levelLabel, levelValue }
    };

    for (int i = 0; i < 6; ++i)
    {
        const int centreX = slotWidth * i + slotWidth / 2;
        knobs[i].label.setBounds(centreX - 54, knobAreaY, 108, labelHeight);
        knobs[i].slider.setBounds(centreX - knobSize / 2, knobAreaY + labelHeight + 6, knobSize, knobSize);
        knobs[i].value.setBounds(centreX - 58, knobAreaY + labelHeight + 6 + knobSize + 4, 116, valueHeight);
    }
}

inline juce::AudioProcessorEditor* NeuralPedal::createEditor()
{
    return new NeuralEditor(*this);
}
