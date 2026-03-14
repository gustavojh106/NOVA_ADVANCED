#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <vector>

#include "ProcessorBase.h"

namespace Nova::PedalUI
{
struct ParameterBinding
{
    juce::String label;
    juce::RangedAudioParameter* parameter = nullptr;
    std::function<juce::String(float)> formatter;
};

class PedalLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    explicit PedalLookAndFeel(juce::Colour accentColour)
        : accent(accentColour)
    {
        setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour::fromString("ff11151a"));
        setColour(juce::Label::textColourId, juce::Colours::white);
    }

    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox(slider);
        label->setFont(juce::Font(10.0f, juce::Font::bold));
        label->setJustificationType(juce::Justification::centred);
        label->setBorderSize(juce::BorderSize<int>(1, 4, 1, 4));
        label->setColour(juce::Label::backgroundColourId, juce::Colour::fromString("ff12161b"));
        label->setColour(juce::Label::outlineColourId, juce::Colour::fromString("ff2c3742"));
        label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
        return label;
    }

    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        auto bounds = label.getLocalBounds().toFloat().reduced(0.5f, 1.0f);
        g.setColour(juce::Colour::fromString("ff171c22"));
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colour::fromString("ff252d36"));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
        g.setColour(label.findColour(juce::Label::textColourId));
        g.setFont(label.getFont());
        g.drawFittedText(label.getText(), label.getLocalBounds().reduced(4, 1), label.getJustificationType(), 1);
    }

    void drawRotarySlider(juce::Graphics& g,
        int x,
        int y,
        int width,
        int height,
        float sliderPos,
        float rotaryStartAngle,
        float rotaryEndAngle,
        juce::Slider& slider) override
    {
        const auto alpha = slider.isEnabled() ? 1.0f : 0.35f;
        const auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(8.0f, 6.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        g.setColour(juce::Colour::fromString("ff171c22").withMultipliedAlpha(alpha));
        g.fillEllipse(bounds);

        const auto face = bounds.reduced(radius * 0.12f);
        g.setColour(juce::Colour::fromString("ff242b33").withMultipliedAlpha(alpha));
        g.fillEllipse(face);
        g.setColour(juce::Colour::fromString("ff303a45").withMultipliedAlpha(alpha));
        g.drawEllipse(face, 1.0f);
        g.setColour(juce::Colour::fromString("ff0f1318").withMultipliedAlpha(alpha));
        g.drawEllipse(bounds, 1.0f);

        juce::Path track;
        track.addCentredArc(centre.x,
            centre.y,
            radius + 2.5f,
            radius + 2.5f,
            0.0f,
            rotaryStartAngle,
            rotaryEndAngle,
            true);

        g.setColour(juce::Colour::fromString("ff2a3139").withMultipliedAlpha(alpha));
        g.strokePath(track, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path valueArc;
        valueArc.addCentredArc(centre.x,
            centre.y,
            radius + 2.5f,
            radius + 2.5f,
            0.0f,
            rotaryStartAngle,
            angle,
            true);

        g.setColour(accent.withMultipliedAlpha(alpha));
        g.strokePath(valueArc, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float pointerLength = radius * 0.68f;
        const float pointerThickness = 2.0f;
        juce::Path pointer;
        pointer.addRoundedRectangle(-pointerThickness * 0.5f, -pointerLength * 0.84f, pointerThickness, pointerLength, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.95f * alpha));
        g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));

        g.setColour(juce::Colour::fromString("ff0f1318").withMultipliedAlpha(alpha));
        g.fillEllipse(centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f);
        g.setColour(accent.withAlpha(0.9f * alpha));
        g.drawEllipse(centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f, 1.0f);
    }

private:
    juce::Colour accent;
};

class PremiumPedalEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    PremiumPedalEditor(juce::AudioProcessor& processor,
        juce::String pedalType,
        juce::String titleText,
        juce::Colour accentColour,
        std::initializer_list<ParameterBinding> parameterBindings,
        int width = 206,
        int height = 170)
        : juce::AudioProcessorEditor(&processor),
          owner(processor),
          category(std::move(pedalType)),
          title(std::move(titleText)),
          accent(accentColour),
          lookAndFeel(accentColour)
    {
        setLookAndFeel(&lookAndFeel);

        for (const auto& binding : parameterBindings)
            addControl(binding);

        setSize(width, height);
        startTimerHz(18);
    }

    ~PremiumPedalEditor() override
    {
        stopTimer();
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool bypassed = isBypassed();

        g.setColour(bypassed ? juce::Colour::fromString("ff0c0f12") : juce::Colour::fromString("ff11161b"));
        g.fillRoundedRectangle(bounds, 10.0f);
        g.setColour(bypassed ? juce::Colour::fromString("ff1a1f24") : juce::Colour::fromString("ff29323b"));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.0f);

        auto badge = juce::Rectangle<float>(10.0f, 10.0f, bounds.getWidth() - 20.0f, 24.0f);
        g.setColour(bypassed ? juce::Colour::fromString("ff12161a") : juce::Colour::fromString("ff151c22"));
        g.fillRoundedRectangle(badge, 6.0f);
        g.setColour(bypassed ? juce::Colour::fromString("ff1d2328") : accent.withAlpha(0.30f));
        g.drawRoundedRectangle(badge, 6.0f, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.66f));
        g.setFont(juce::Font(10.0f, juce::Font::bold));
        g.drawText((category + "  " + title).toUpperCase(), badge.toNearestInt().reduced(8, 0), juce::Justification::centredLeft);

        g.setColour(bypassed ? juce::Colour::fromString("ff6f3b3b") : accent);
        g.fillRoundedRectangle(badge.removeFromRight(8.0f).reduced(1.0f, 5.0f), 2.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        area.removeFromTop(34);

        if (controls.empty())
            return;

        const int total = (int)controls.size();
        const int topCount = total <= 3 ? total : (total + 1) / 2;
        const int bottomCount = total - topCount;

        auto topRow = area.removeFromTop(bottomCount > 0 ? 60 : area.getHeight());
        layoutRow(topRow, 0, topCount);

        if (bottomCount > 0)
        {
            area.removeFromTop(2);
            layoutRow(area.removeFromTop(60), topCount, bottomCount);
        }
    }

private:
    struct Control
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;
    };

    void timerCallback() override
    {
        const bool currentBypassed = isBypassed();
        if (currentBypassed != lastBypassState)
        {
            lastBypassState = currentBypassed;
            repaint();
        }
    }

    void addControl(const ParameterBinding& binding)
    {
        if (binding.parameter == nullptr)
            return;

        auto control = std::make_unique<Control>();
        control->slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        control->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 14);
        control->slider.setRotaryParameters(juce::degreesToRadians(210.0f),
            juce::degreesToRadians(510.0f),
            true);
        control->slider.setDoubleClickReturnValue(true,
            binding.parameter->convertFrom0to1(binding.parameter->getDefaultValue()));

        control->slider.textFromValueFunction = [formatter = binding.formatter](double value)
        {
            if (formatter)
                return formatter((float)value);

            return juce::String(value, 2);
        };

        control->slider.valueFromTextFunction = [](const juce::String& text)
        {
            return text.retainCharacters("0123456789-+.").getDoubleValue();
        };

        control->label.setText(binding.label.toUpperCase(), juce::dontSendNotification);
        control->label.setFont(juce::Font(9.0f, juce::Font::bold));
        control->label.setJustificationType(juce::Justification::centred);
        control->label.setInterceptsMouseClicks(false, false);

        control->attachment = std::make_unique<juce::SliderParameterAttachment>(*binding.parameter, control->slider);

        addAndMakeVisible(control->slider);
        addAndMakeVisible(control->label);
        controls.push_back(std::move(control));
    }

    void layoutRow(juce::Rectangle<int> row, int startIndex, int count)
    {
        if (count <= 0)
            return;

        const int cellWidth = row.getWidth() / count;
        for (int i = 0; i < count; ++i)
        {
            auto cell = row.withTrimmedLeft(i * cellWidth).removeFromLeft(cellWidth).reduced(2, 1);
            auto& control = *controls[(size_t)(startIndex + i)];
            control.label.setBounds(cell.removeFromTop(14));
            control.slider.setBounds(cell);
        }
    }

    bool isBypassed() const
    {
        if (auto* base = dynamic_cast<const ProcessorBase*>(&owner))
            return base->getBypassed();

        return false;
    }

    juce::AudioProcessor& owner;
    juce::String category;
    juce::String title;
    juce::Colour accent;
    PedalLookAndFeel lookAndFeel;
    std::vector<std::unique_ptr<Control>> controls;
    bool lastBypassState = false;
};

inline juce::String formatPercent(float value)
{
    return juce::String(juce::roundToInt(value * 100.0f)) + "%";
}

inline juce::String formatPercentFromHundred(float value)
{
    return juce::String(juce::roundToInt(value)) + "%";
}

inline juce::String formatDecibels(float value)
{
    return juce::String(value, 1) + " dB";
}

inline juce::String formatGain(float value)
{
    return juce::String(value, 2) + "x";
}

inline juce::String formatMilliseconds(float value)
{
    return juce::String(value, value >= 100.0f ? 0 : 1) + " ms";
}

inline juce::String formatHertz(float value)
{
    return value >= 1000.0f
        ? juce::String(value / 1000.0f, 2) + " kHz"
        : juce::String(value, 0) + " Hz";
}
}
