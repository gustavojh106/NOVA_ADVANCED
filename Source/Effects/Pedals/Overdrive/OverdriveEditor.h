#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class OverdrivePedal;

class OverdriveEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    OverdriveEditor(OverdrivePedal& pedal);
    ~OverdriveEditor() override
    {
        stopTimer();
        sldDrive.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldTexture.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
        sldLevel.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintCurveViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    OverdrivePedal& proc;

    static constexpr int kWidth = 730;
    static constexpr int kHeight = 445;

    const juce::Colour accent      = juce::Colour::fromString("ffF06848");
    const juce::Colour accentDim   = juce::Colour::fromString("ffC44E30");
    const juce::Colour accentGlow  = juce::Colour::fromString("ffFF9B70");
    const juce::Colour bgTop       = juce::Colour(0xff120A06);
    const juce::Colour bgBottom    = juce::Colour(0xff1E1008);
    const juce::Colour panel       = juce::Colour(0xff241A10);
    const juce::Colour panelEdge   = juce::Colour(0xff4A3424);
    const juce::Colour textBright  = juce::Colour(0xffF8F0E8);
    const juce::Colour textDim     = juce::Colour(0xffA89888);

    juce::Slider sldDrive, sldTone, sldTexture, sldMix, sldLevel;
    juce::Label lblDrive, lblTone, lblTexture, lblMix, lblLevel;
    juce::Label valDrive, valTone, valTexture, valMix, valLevel;

    juce::Label driveDescLabel;
    juce::Label voiceHintLabel;

    juce::Rectangle<float> vizBounds;

    struct ODKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour accent    = juce::Colour::fromString("ffF06848");
        juce::Colour accentGlow = juce::Colour::fromString("ffFF9B70");

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

            // Track background
            juce::Path track;
            track.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff3A2218));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            // Value arc
            if (sliderPos > 0.001f)
            {
                juce::Path arc;
                arc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(accent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(accent);
                g.strokePath(arc, juce::PathStrokeType(3.4f, juce::PathStrokeType::curved));
            }

            // Knob body
            const float knobRadius = radius * 0.58f;
            juce::ColourGradient body(juce::Colour(0xff4A2A18), centreX, centreY - knobRadius,
                juce::Colour(0xff1A0E08), centreX, centreY + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff6B4430));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            // Pointer
            const float pointerLength = knobRadius * 0.74f;
            const float pointX = centreX + std::sin(angle) * pointerLength;
            const float pointY = centreY - std::cos(angle) * pointerLength;
            g.setColour(accentGlow);
            g.drawLine(centreX, centreY, pointX, pointY, 2.0f);
            g.fillEllipse(pointX - 2.8f, pointY - 2.8f, 5.6f, 5.6f);
        }
    } knobLnF;

    juce::String describeDrive(float value) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OverdriveEditor)
};

#include "OverdrivePedal.h"

inline OverdriveEditor::OverdriveEditor(OverdrivePedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& slider, juce::Label& name, juce::Label& value,
        const juce::String& text, double min, double max, double defaultValue, double step)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider.setRange(min, max, step);
        slider.setValue(defaultValue, juce::dontSendNotification);
        slider.setLookAndFeel(&knobLnF);
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

    initKnob(sldDrive, lblDrive, valDrive, "DRIVE", 0.0, 100.0, 30.0, 0.1);
    initKnob(sldTone, lblTone, valTone, "TONE", 0.0, 1.0, 0.58, 0.01);
    initKnob(sldTexture, lblTexture, valTexture, "TEXTURE", 0.0, 1.0, 0.42, 0.01);
    initKnob(sldMix, lblMix, valMix, "MIX", 0.0, 1.0, 1.0, 0.01);
    initKnob(sldLevel, lblLevel, valLevel, "LEVEL", 0.0, 1.0, 0.74, 0.01);

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

    wireFloat(sldDrive, proc.getDriveParam());
    wireFloat(sldTone, proc.getToneParam());
    wireFloat(sldTexture, proc.getTextureParam());
    wireFloat(sldMix, proc.getMixParam());
    wireFloat(sldLevel, proc.getLevelParam());

    driveDescLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    driveDescLabel.setColour(juce::Label::textColourId, accentGlow.withAlpha(0.88f));
    driveDescLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(driveDescLabel);

    voiceHintLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    voiceHintLabel.setColour(juce::Label::textColourId, accentGlow.withAlpha(0.86f));
    voiceHintLabel.setJustificationType(juce::Justification::centredRight);
    voiceHintLabel.setText("Texture shapes grain, Mix blends parallel dry signal", juce::dontSendNotification);
    addAndMakeVisible(voiceHintLabel);

    startTimerHz(30);
}

inline void OverdriveEditor::timerCallback()
{
    const auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldDrive, proc.getDriveParam());
    syncSlider(sldTone, proc.getToneParam());
    syncSlider(sldTexture, proc.getTextureParam());
    syncSlider(sldMix, proc.getMixParam());
    syncSlider(sldLevel, proc.getLevelParam());

    valDrive.setText(juce::String(juce::roundToInt(sldDrive.getValue())) + "%", juce::dontSendNotification);
    valTone.setText(juce::String((int) std::round(sldTone.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valTexture.setText(juce::String((int) std::round(sldTexture.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valMix.setText(juce::String((int) std::round(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valLevel.setText(juce::String((int) std::round(sldLevel.getValue() * 100.0)) + "%", juce::dontSendNotification);

    driveDescLabel.setText(describeDrive((float) sldDrive.getValue()), juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void OverdriveEditor::paint(juce::Graphics& g)
{
    // Background gradient
    juce::ColourGradient bg(bgTop, 0.0f, 0.0f, bgBottom, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    // Border
    g.setColour(accent.withAlpha(0.16f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    // Top accent glow
    juce::ColourGradient topGlow(accent.withAlpha(0.34f), 0.0f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth(), 0.0f, false);
    g.setGradientFill(topGlow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 3.0f);

    // Title
    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
    g.drawText("OVERDRIVE", 26, 12, 250, 30, juce::Justification::centredLeft);

    // Subtitle - current drive character
    g.setColour(accentGlow);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText(describeDrive((float) sldDrive.getValue()),
        26, 38, 220, 18, juce::Justification::centredLeft);

    // Visualization
    paintCurveViz(g, vizBounds);
}

inline void OverdriveEditor::paintCurveViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(panel.withAlpha(0.95f));
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(panelEdge);
    g.drawRoundedRectangle(bounds, 10.0f, 1.0f);

    auto inner = bounds.reduced(18.0f, 12.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();

    const float drive = (float) juce::jlimit(0.0, 100.0, sldDrive.getValue());
    const float tone = (float) juce::jlimit(0.0, 1.0, sldTone.getValue());
    const float texture = (float) juce::jlimit(0.0, 1.0, sldTexture.getValue());
    const float mix = (float) juce::jlimit(0.0, 1.0, sldMix.getValue());

    // Grid lines
    for (int i = 0; i < 4; ++i)
    {
        const float y = juce::jmap((float) i / 3.0f, inner.getBottom(), inner.getY());
        g.setColour(juce::Colour(0xff3A2218).withAlpha(0.35f));
        g.drawHorizontalLine((int) std::round(y), inner.getX(), inner.getRight());
    }

    // Centre line
    g.setColour(juce::Colour(0xff3A2218));
    g.drawLine(inner.getX(), inner.getCentreY(), inner.getRight(), inner.getCentreY(), 0.6f);

    // Response curve (same algorithm as original)
    juce::Path curve;
    for (int i = 0; i <= 42; ++i)
    {
        const float t = (float) i / 42.0f;
        const float x = inner.getX() + t * width;

        const float tilt = juce::jmap(tone, -0.22f, 0.22f) * (t - 0.5f);
        const float bump = std::sin(t * juce::MathConstants<float>::pi) * juce::jmap(texture, 0.06f, 0.24f);
        const float driveLift = juce::jmap(drive / 100.0f, 0.10f, 0.26f);
        const float blendFloor = juce::jmap(mix, -0.04f, 0.05f);
        const float normalized = juce::jlimit(0.08f, 0.92f, 0.62f - driveLift + blendFloor - bump + tilt);
        const float y = inner.getY() + normalized * height;

        if (i == 0)
            curve.startNewSubPath(x, y);
        else
            curve.lineTo(x, y);
    }

    // Glow + main curve
    g.setColour(accent.withAlpha(0.08f));
    g.strokePath(curve, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.14f));
    g.strokePath(curve, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.88f));
    g.strokePath(curve, juce::PathStrokeType(1.9f, juce::PathStrokeType::curved));

    // Character label
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.setColour(textDim.withAlpha(0.56f));
    const juce::String character = drive < 28.0f ? "Edge push"
        : drive < 58.0f ? "Focused crunch"
        : drive < 82.0f ? "Saturated lead"
        : "Full molten";
    g.drawText(character, (int) inner.getRight() - 90, (int) inner.getBottom() - 14, 86, 12,
        juce::Justification::centredRight);

    // Labels
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.setColour(accentGlow.withAlpha(0.75f));
    g.drawText("RESPONSE", (int) inner.getX() + 4, (int) inner.getY() + 2, 80, 14, juce::Justification::centredLeft);
}

inline void OverdriveEditor::resized()
{
    const int width = getWidth();

    driveDescLabel.setBounds(26, 58, width / 2 - 26, 18);
    voiceHintLabel.setBounds(width / 2, 60, width / 2 - 26, 16);

    vizBounds = juce::Rectangle<float>(26.0f, 84.0f, (float) (width - 52), 150.0f);

    const int knobAreaY = 255;
    const int knobSize = 84;
    const int labelHeight = 16;
    const int valueHeight = 16;
    const int slotWidth = width / 5;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup knobs[] = {
        { sldDrive, lblDrive, valDrive },
        { sldTone, lblTone, valTone },
        { sldTexture, lblTexture, valTexture },
        { sldMix, lblMix, valMix },
        { sldLevel, lblLevel, valLevel }
    };

    for (int i = 0; i < 5; ++i)
    {
        const int centreX = slotWidth * i + slotWidth / 2;
        knobs[i].label.setBounds(centreX - 54, knobAreaY, 108, labelHeight);
        knobs[i].slider.setBounds(centreX - knobSize / 2, knobAreaY + labelHeight + 6, knobSize, knobSize);
        knobs[i].value.setBounds(centreX - 58, knobAreaY + labelHeight + 6 + knobSize + 4, 116, valueHeight);
    }
}

inline juce::String OverdriveEditor::describeDrive(float value) const
{
    if (value < 28.0f)
        return "Edge push";
    if (value < 58.0f)
        return "Focused crunch";
    if (value < 82.0f)
        return "Saturated lead";
    return "Full molten";
}

inline juce::AudioProcessorEditor* OverdrivePedal::createEditor()
{
    return new OverdriveEditor(*this);
}
