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
        label->setFont(juce::Font(11.0f, juce::Font::bold));
        label->setJustificationType(juce::Justification::centred);
        label->setBorderSize(juce::BorderSize<int>(2, 6, 2, 6));
        label->setColour(juce::Label::backgroundColourId, juce::Colour::fromString("ff11151a"));
        label->setColour(juce::Label::outlineColourId, accent.withAlpha(0.22f));
        label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.92f));
        return label;
    }

    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        auto bounds = label.getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(juce::Colour::fromString("ff0f1217"));
        g.fillRoundedRectangle(bounds, 7.0f);
        g.setColour(accent.withAlpha(0.18f));
        g.drawRoundedRectangle(bounds, 7.0f, 1.0f);
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
        const auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(7.0f, 5.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        g.setColour(juce::Colours::black.withAlpha(0.45f * alpha));
        g.fillEllipse(bounds.translated(0.0f, 3.0f));

        juce::ColourGradient outer(juce::Colour::fromString("ff343b45"), centre.x, bounds.getY(),
            juce::Colour::fromString("ff0f1318"), centre.x, bounds.getBottom(), false);
        g.setGradientFill(outer);
        g.fillEllipse(bounds);

        const auto ring = bounds.reduced(radius * 0.1f);
        juce::ColourGradient inner(juce::Colour::fromString("ff69707a"), centre.x, ring.getY(),
            juce::Colour::fromString("ff242930"), centre.x, ring.getBottom(), false);
        g.setGradientFill(inner);
        g.fillEllipse(ring);

        g.setColour(juce::Colours::white.withAlpha(0.08f * alpha));
        g.drawEllipse(ring.reduced(1.2f), 1.0f);
        g.setColour(juce::Colours::black.withAlpha(0.6f * alpha));
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

        g.setColour(juce::Colour::fromString("ff20252b").withMultipliedAlpha(alpha));
        g.strokePath(track, juce::PathStrokeType(5.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path valueArc;
        valueArc.addCentredArc(centre.x,
            centre.y,
            radius + 2.5f,
            radius + 2.5f,
            0.0f,
            rotaryStartAngle,
            angle,
            true);

        juce::ColourGradient glow(accent.brighter(0.4f), centre.x, bounds.getY(),
            accent.darker(0.35f), centre.x, bounds.getBottom(), false);
        g.setGradientFill(glow);
        g.strokePath(valueArc, juce::PathStrokeType(5.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float pointerLength = radius * 0.8f;
        const float pointerThickness = 3.0f;
        juce::Path pointer;
        pointer.addRoundedRectangle(-pointerThickness * 0.5f, -pointerLength * 0.8f, pointerThickness, pointerLength, 1.3f);

        g.setColour(juce::Colours::white.withAlpha(0.95f * alpha));
        g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));

        g.setColour(accent.withAlpha(0.95f * alpha));
        g.fillEllipse(centre.x - 3.6f, centre.y - 3.6f, 7.2f, 7.2f);
        g.setColour(juce::Colours::white.withAlpha(0.35f * alpha));
        g.drawEllipse(centre.x - 3.6f, centre.y - 3.6f, 7.2f, 7.2f, 1.0f);
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
        int width = 236,
        int height = 228)
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

        juce::ColourGradient shell(juce::Colour::fromString("ff323842"), bounds.getCentreX(), bounds.getY(),
            juce::Colour::fromString("ff15191e"), bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(shell);
        g.fillRoundedRectangle(bounds, 18.0f);

        g.setColour(juce::Colours::black.withAlpha(0.36f));
        g.drawRoundedRectangle(bounds.reduced(1.0f), 18.0f, 1.8f);

        g.setGradientFill(juce::ColourGradient(accent.withAlpha(0.24f), 0.0f, 0.0f,
            juce::Colours::transparentBlack, 0.0f, 72.0f, false));
        g.fillRoundedRectangle(bounds.reduced(8.0f, 6.0f), 14.0f);

        auto badge = juce::Rectangle<float>(16.0f, 14.0f, bounds.getWidth() - 32.0f, 42.0f);
        g.setColour(juce::Colour::fromString("aa090c10"));
        g.fillRoundedRectangle(badge, 14.0f);
        g.setColour(accent.withAlpha(0.26f));
        g.drawRoundedRectangle(badge, 14.0f, 1.1f);

        g.setColour(juce::Colours::white.withAlpha(0.66f));
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawText(category.toUpperCase(), badge.removeFromTop(18.0f), juce::Justification::centred);

        g.setColour(juce::Colours::white.withAlpha(0.98f));
        g.setFont(juce::Font(16.0f, juce::Font::bold));
        g.drawText(title.toUpperCase(), badge, juce::Justification::centred);

        drawStatusLight(g);
        drawHardware(g);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        area.removeFromTop(60);

        if (controls.empty())
            return;

        const int total = (int)controls.size();
        const int topCount = total <= 3 ? total : (total + 1) / 2;
        const int bottomCount = total - topCount;

        auto topRow = area.removeFromTop(bottomCount > 0 ? 76 : area.getHeight());
        layoutRow(topRow, 0, topCount);

        if (bottomCount > 0)
        {
            area.removeFromTop(4);
            layoutRow(area.removeFromTop(76), topCount, bottomCount);
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
        control->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 16);
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
        control->label.setFont(juce::Font(10.0f, juce::Font::bold));
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
            auto cell = row.withTrimmedLeft(i * cellWidth).removeFromLeft(cellWidth).reduced(3, 1);
            auto& control = *controls[(size_t)(startIndex + i)];
            control.label.setBounds(cell.removeFromTop(18));
            control.slider.setBounds(cell);
        }
    }

    bool isBypassed() const
    {
        if (auto* base = dynamic_cast<const ProcessorBase*>(&owner))
            return base->getBypassed();

        return false;
    }

    void drawStatusLight(juce::Graphics& g) const
    {
        const auto led = juce::Rectangle<float>((float)getWidth() - 32.0f, 18.0f, 10.0f, 10.0f);
        const auto colour = isBypassed() ? juce::Colour::fromString("ff5b1f1f") : accent;

        g.setColour(colour.withAlpha(0.22f));
        g.fillEllipse(led.expanded(8.0f));
        g.setColour(colour);
        g.fillEllipse(led);
        g.setColour(juce::Colours::white.withAlpha(0.38f));
        g.fillEllipse(led.reduced(4.0f));
    }

    void drawHardware(juce::Graphics& g) const
    {
        const auto drawBolt = [&g](float x, float y)
        {
            g.setColour(juce::Colour::fromString("ff0e1115"));
            g.fillEllipse(x - 4.0f, y - 4.0f, 8.0f, 8.0f);
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.drawEllipse(x - 4.0f, y - 4.0f, 8.0f, 8.0f, 1.0f);
            g.drawLine(x - 1.5f, y, x + 1.5f, y, 1.1f);
            g.drawLine(x, y - 1.5f, x, y + 1.5f, 1.1f);
        };

        drawBolt(12.0f, 12.0f);
        drawBolt((float)getWidth() - 12.0f, 12.0f);
        drawBolt(12.0f, (float)getHeight() - 12.0f);
        drawBolt((float)getWidth() - 12.0f, (float)getHeight() - 12.0f);
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
