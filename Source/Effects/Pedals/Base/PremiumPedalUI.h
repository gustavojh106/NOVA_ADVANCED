#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <vector>

#include "../../../Core/StyleSheet.h"
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
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour::fromString("ff0D1520"));
        setColour(juce::Label::textColourId, juce::Colours::white);
    }

    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox(slider);
        label->setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        label->setJustificationType(juce::Justification::centred);
        label->setBorderSize(juce::BorderSize<int>(1, 4, 1, 4));
        label->setColour(juce::Label::backgroundColourId, juce::Colour::fromString("ff0D1520"));
        label->setColour(juce::Label::outlineColourId, juce::Colour::fromString("ff2A3548"));
        label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
        return label;
    }

    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        auto bounds = label.getLocalBounds().toFloat().reduced(0.5f, 1.0f);
        g.setColour(juce::Colour::fromString("ff111827"));
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colour::fromString("ff2A3548"));
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
        const float angle = UI::KnobGeometry::upperArcAngle(rotaryStartAngle, rotaryEndAngle, sliderPos);

        // Knob body
        g.setColour(juce::Colour::fromString("ff0D1520").withMultipliedAlpha(alpha));
        g.fillEllipse(bounds);

        const auto face = bounds.reduced(radius * 0.12f);
        g.setColour(juce::Colour::fromString("ff1A2332").withMultipliedAlpha(alpha));
        g.fillEllipse(face);
        g.setColour(juce::Colour::fromString("ff2A3548").withMultipliedAlpha(alpha));
        g.drawEllipse(face, 1.0f);
        g.setColour(juce::Colour::fromString("ff0B0E14").withMultipliedAlpha(alpha));
        g.drawEllipse(bounds, 1.0f);

        // Arc track
        auto track = UI::KnobGeometry::createUpperArc(centre.x, centre.y, radius + 2.5f,
            rotaryStartAngle, rotaryEndAngle);
        g.setColour(juce::Colour::fromString("ff2A3548").withMultipliedAlpha(alpha));
        g.strokePath(track, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Value arc
        auto valueArc = UI::KnobGeometry::createUpperArc(centre.x, centre.y, radius + 2.5f,
            rotaryStartAngle, angle);
        g.setColour(accent.withMultipliedAlpha(alpha));
        g.strokePath(valueArc, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Glow on value arc
        g.setColour(accent.withAlpha(0.18f * alpha));
        g.strokePath(valueArc, juce::PathStrokeType(9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Pointer
        const float pointerLength = radius * 0.68f;
        const float pointerThickness = 2.0f;
        const auto innerPoint = UI::KnobGeometry::pointOnUpperArc(centre.x, centre.y, radius * 0.16f, angle);
        const auto outerPoint = UI::KnobGeometry::pointOnUpperArc(centre.x, centre.y, pointerLength, angle);
        g.setColour(juce::Colours::white.withAlpha(0.92f * alpha));
        g.drawLine({ innerPoint, outerPoint }, pointerThickness);

        // Center dot
        g.setColour(juce::Colour::fromString("ff0B0E14").withMultipliedAlpha(alpha));
        g.fillEllipse(centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f);
        g.setColour(accent.withAlpha(0.85f * alpha));
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

        // Body fill
        g.setColour(bypassed ? juce::Colour::fromString("ff080A0E") : juce::Colour::fromString("ff0D1520"));
        g.fillRoundedRectangle(bounds, 10.0f);

        // Border with accent tint when active
        g.setColour(bypassed ? juce::Colour::fromString("ff1A2332") : accent.withAlpha(0.22f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.0f);

        // Badge header
        auto badge = juce::Rectangle<float>(10.0f, 10.0f, bounds.getWidth() - 20.0f, 24.0f);
        g.setColour(bypassed ? juce::Colour::fromString("ff0B0E14") : juce::Colour::fromString("ff111827"));
        g.fillRoundedRectangle(badge, 6.0f);
        g.setColour(bypassed ? juce::Colour::fromString("ff1A2332") : accent.withAlpha(0.30f));
        g.drawRoundedRectangle(badge, 6.0f, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.66f));
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText((category + "  " + title).toUpperCase(), badge.toNearestInt().reduced(8, 0), juce::Justification::centredLeft);

        // Status indicator pill
        g.setColour(bypassed ? Nova::Colors::Error.withAlpha(0.5f) : accent);
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
        control->slider.setRotaryParameters(UI::KnobGeometry::knobStartAngleRadians(),
            UI::KnobGeometry::knobEndAngleRadians(),
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
        control->label.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
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

class PremiumHardwareEditor : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    enum class Skin
    {
        Amplifier,
        Cabinet
    };

    PremiumHardwareEditor(juce::AudioProcessor& processor,
        Skin editorSkin,
        juce::String categoryText,
        juce::String titleText,
        juce::String subtitleText,
        juce::Colour accentColour,
        std::initializer_list<ParameterBinding> parameterBindings,
        int width = 620,
        int height = 326)
        : juce::AudioProcessorEditor(&processor),
          owner(processor),
          skin(editorSkin),
          category(std::move(categoryText)),
          title(std::move(titleText)),
          subtitle(std::move(subtitleText)),
          accent(accentColour),
          lookAndFeel(accentColour)
    {
        setLookAndFeel(&lookAndFeel);

        for (const auto& binding : parameterBindings)
            addControl(binding);

        setSize(width, height);
        startTimerHz(18);
    }

    ~PremiumHardwareEditor() override
    {
        stopTimer();
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        skin == Skin::Amplifier ? paintAmplifier(g) : paintCabinet(g);

        if (isBypassed())
        {
            g.setColour(juce::Colour::fromString("bb05070B"));
            g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 12.0f);
            g.setColour(Nova::Colors::Error.withAlpha(0.82f));
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText("BYPASSED", getLocalBounds().reduced(18), juce::Justification::topRight);
        }
    }

    void resized() override
    {
        skin == Skin::Amplifier ? resizedAmplifier() : resizedCabinet();
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
        control->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
        control->slider.setRotaryParameters(UI::KnobGeometry::knobStartAngleRadians(),
            UI::KnobGeometry::knobEndAngleRadians(),
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
        control->label.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        control->label.setJustificationType(juce::Justification::centred);
        control->label.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.70f));
        control->label.setInterceptsMouseClicks(false, false);

        control->attachment = std::make_unique<juce::SliderParameterAttachment>(*binding.parameter, control->slider);

        addAndMakeVisible(control->slider);
        addAndMakeVisible(control->label);
        controls.push_back(std::move(control));
    }

    void paintAmplifier(juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto body = bounds.reduced(1.0f);

        juce::ColourGradient bg(juce::Colour::fromString("ff070A12"), 0.0f, 0.0f,
            juce::Colour::fromString("ff111827"), 0.0f, bounds.getBottom(), false);
        g.setGradientFill(bg);
        g.fillRoundedRectangle(body, 12.0f);

        g.setColour(accent.withAlpha(0.20f));
        g.drawRoundedRectangle(body, 12.0f, 1.0f);

        auto header = bounds.reduced(16.0f, 14.0f).removeFromTop(72.0f);
        juce::ColourGradient headGrad(juce::Colour::fromString("ff151B2A"), header.getX(), header.getY(),
            juce::Colour::fromString("ff0A0F1B"), header.getRight(), header.getBottom(), false);
        g.setGradientFill(headGrad);
        g.fillRoundedRectangle(header, 9.0f);
        g.setColour(accent.withAlpha(0.28f));
        g.drawRoundedRectangle(header, 9.0f, 1.0f);

        g.setColour(accent.withAlpha(0.32f));
        g.fillRoundedRectangle(header.removeFromLeft(5.0f), 2.5f);

        auto titleArea = header.reduced(18.0f, 10.0f);
        g.setColour(juce::Colours::white.withAlpha(0.46f));
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(category.toUpperCase() + " CHANNEL", titleArea.removeFromTop(15.0f).toNearestInt(),
            juce::Justification::centredLeft);

        g.setColour(juce::Colours::white.withAlpha(0.93f));
        g.setFont(juce::Font(juce::FontOptions(26.0f, juce::Font::bold)));
        g.drawText(title.toUpperCase(), titleArea.removeFromTop(32.0f).toNearestInt(),
            juce::Justification::centredLeft);

        g.setColour(accent.withAlpha(0.82f));
        g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
        g.drawText(subtitle.toUpperCase(), titleArea.toNearestInt(), juce::Justification::centredLeft);

        auto meter = bounds.reduced(24.0f, 0.0f).withY(104.0f).withHeight(12.0f);
        g.setColour(juce::Colour::fromString("ff060A11"));
        g.fillRoundedRectangle(meter, 4.0f);
        for (int i = 0; i < 18; ++i)
        {
            const float x = meter.getX() + 8.0f + (float)i * ((meter.getWidth() - 16.0f) / 17.0f);
            const float alpha = i % 3 == 0 ? 0.34f : 0.18f;
            g.setColour(accent.withAlpha(alpha));
            g.drawVerticalLine((int)std::round(x), meter.getY() + 2.0f, meter.getBottom() - 2.0f);
        }

        paintSectionPanel(g, juce::Rectangle<float>(18.0f, 128.0f, bounds.getWidth() - 36.0f, bounds.getHeight() - 146.0f),
            "PREAMP / TONE STACK / POWER");
    }

    void paintCabinet(juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto body = bounds.reduced(1.0f);

        juce::ColourGradient bg(juce::Colour::fromString("ff050812"), 0.0f, 0.0f,
            juce::Colour::fromString("ff120B21"), 0.0f, bounds.getBottom(), false);
        g.setGradientFill(bg);
        g.fillRoundedRectangle(body, 12.0f);

        g.setColour(accent.withAlpha(0.20f));
        g.drawRoundedRectangle(body, 12.0f, 1.0f);

        auto header = bounds.reduced(16.0f, 14.0f).removeFromTop(56.0f);
        g.setColour(juce::Colour::fromString("ff101725"));
        g.fillRoundedRectangle(header, 8.0f);
        g.setColour(accent.withAlpha(0.24f));
        g.drawRoundedRectangle(header, 8.0f, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.48f));
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("SPEAKER / IR MODULE", header.toNearestInt().reduced(16, 7), juce::Justification::topLeft);

        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.setFont(juce::Font(juce::FontOptions(23.0f, juce::Font::bold)));
        g.drawText(title.toUpperCase(), header.toNearestInt().reduced(16, 17), juce::Justification::centredLeft);

        g.setColour(accent.withAlpha(0.82f));
        g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
        g.drawText(subtitle.toUpperCase(), header.toNearestInt().reduced(16, 8), juce::Justification::bottomRight);

        auto grille = juce::Rectangle<float>(18.0f, 86.0f, bounds.getWidth() - 36.0f, 98.0f);
        g.setColour(juce::Colour::fromString("ff080D17"));
        g.fillRoundedRectangle(grille, 8.0f);
        g.setColour(juce::Colour::fromString("ff273143").withAlpha(0.78f));
        g.drawRoundedRectangle(grille, 8.0f, 1.0f);

        auto inner = grille.reduced(16.0f, 12.0f);
        for (int i = 0; i < 13; ++i)
        {
            const float y = inner.getY() + (float)i * (inner.getHeight() / 12.0f);
            g.setColour(i % 3 == 0 ? accent.withAlpha(0.22f) : juce::Colours::white.withAlpha(0.08f));
            g.drawHorizontalLine((int)std::round(y), inner.getX(), inner.getRight());
        }

        for (int i = 0; i < 4; ++i)
        {
            const float cx = inner.getX() + inner.getWidth() * ((float)i + 0.5f) / 4.0f;
            const auto speaker = juce::Rectangle<float>(cx - 27.0f, inner.getCentreY() - 27.0f, 54.0f, 54.0f);
            g.setColour(juce::Colour::fromString("ff05070C"));
            g.fillEllipse(speaker);
            g.setColour(accent.withAlpha(0.18f));
            g.drawEllipse(speaker, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.drawEllipse(speaker.reduced(10.0f), 1.0f);
        }

        paintSectionPanel(g, juce::Rectangle<float>(18.0f, 200.0f, bounds.getWidth() - 36.0f, bounds.getHeight() - 218.0f),
            "VOICING / CUTS / ROOM");
    }

    void paintSectionPanel(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text)
    {
        g.setColour(juce::Colour::fromString("aa0A101A"));
        g.fillRoundedRectangle(area, 9.0f);
        g.setColour(juce::Colour::fromString("ff273143").withAlpha(0.72f));
        g.drawRoundedRectangle(area, 9.0f, 1.0f);

        g.setColour(accent.withAlpha(0.68f));
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawText(text, area.toNearestInt().reduced(12, 8), juce::Justification::topLeft);
    }

    void resizedAmplifier()
    {
        if (controls.empty())
            return;

        auto area = getLocalBounds().reduced(22);
        area.removeFromTop(126);

        const int total = (int)controls.size();
        const int heroSize = 86;
        auto driveCell = area.removeFromLeft(116).reduced(4, 18);
        layoutControl(0, driveCell, heroSize);

        auto masterCell = area.removeFromRight(116).reduced(4, 18);
        if (total > 1)
            layoutControl(total - 1, masterCell, heroSize);

        area.reduce(8, 14);
        const int middleStart = 1;
        const int middleCount = juce::jmax(0, total - 2);
        if (middleCount <= 0)
            return;

        const int topCount = middleCount <= 3 ? middleCount : (middleCount + 1) / 2;
        const int bottomCount = middleCount - topCount;
        auto topRow = area.removeFromTop(bottomCount > 0 ? 70 : area.getHeight());
        layoutRow(topRow, middleStart, topCount, 64);

        if (bottomCount > 0)
        {
            area.removeFromTop(4);
            layoutRow(area.removeFromTop(70), middleStart + topCount, bottomCount, 64);
        }
    }

    void resizedCabinet()
    {
        if (controls.empty())
            return;

        auto area = getLocalBounds().reduced(22);
        area.removeFromTop(206);

        const int total = (int)controls.size();
        layoutRow(area.reduced(0, 6), 0, total, 58);
    }

    void layoutRow(juce::Rectangle<int> row, int startIndex, int count, int knobSize)
    {
        if (count <= 0)
            return;

        const int cellWidth = row.getWidth() / count;
        for (int i = 0; i < count; ++i)
        {
            auto cell = row.withTrimmedLeft(i * cellWidth).removeFromLeft(cellWidth).reduced(4, 0);
            layoutControl(startIndex + i, cell, knobSize);
        }
    }

    void layoutControl(int index, juce::Rectangle<int> cell, int knobSize)
    {
        if (index < 0 || index >= (int)controls.size())
            return;

        auto& control = *controls[(size_t)index];
        control.label.setBounds(cell.removeFromTop(15));

        const int knobX = cell.getCentreX() - knobSize / 2;
        control.slider.setBounds(knobX, cell.getY(), knobSize, juce::jmin(knobSize + 20, cell.getHeight()));
    }

    bool isBypassed() const
    {
        if (auto* base = dynamic_cast<const ProcessorBase*>(&owner))
            return base->getBypassed();

        return false;
    }

    juce::AudioProcessor& owner;
    Skin skin;
    juce::String category;
    juce::String title;
    juce::String subtitle;
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
