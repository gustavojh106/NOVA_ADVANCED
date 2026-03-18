#pragma once
#include <JuceHeader.h>
#include <cmath>
#include "Constants.h"

namespace UI
{
    namespace KnobGeometry
    {
        static constexpr float knobMinDegrees = 180.0f;
        static constexpr float knobMaxDegrees = 0.0f;

        inline float knobStartAngleRadians() noexcept { return juce::degreesToRadians(knobMinDegrees); }
        inline float knobEndAngleRadians() noexcept { return juce::degreesToRadians(knobMaxDegrees); }

        inline float upperArcAngle(float rotaryStartAngle,
            float rotaryEndAngle,
            float sliderPos) noexcept
        {
            return rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        }

        inline juce::Point<float> pointOnUpperArc(float cx,
            float cy,
            float radius,
            float angle) noexcept
        {
            return { cx + std::cos(angle) * radius, cy - std::sin(angle) * radius };
        }

        inline juce::Path createUpperArc(float cx,
            float cy,
            float radius,
            float startAngle,
            float endAngle,
            int segments = 48)
        {
            juce::Path path;
            const int safeSegments = juce::jmax(1, segments);

            for (int i = 0; i <= safeSegments; ++i)
            {
                const float t = (float)i / (float)safeSegments;
                const float angle = startAngle + t * (endAngle - startAngle);
                const auto point = pointOnUpperArc(cx, cy, radius, angle);

                if (i == 0)
                    path.startNewSubPath(point);
                else
                    path.lineTo(point);
            }

            return path;
        }
    }

    namespace Colors
    {
        const juce::Colour Background = Nova::Colors::Background;
        const juce::Colour Panel      = Nova::Colors::Panel;
        const juce::Colour Accent     = Nova::Colors::Accent;
        const juce::Colour RingOff    = juce::Colour::fromString("ff2A3548");
        const juce::Colour Text       = Nova::Colors::Text;
        const juce::Colour Shadow     = juce::Colours::black.withAlpha(0.55f);
    }

    class ModernKnobLnF : public juce::LookAndFeel_V4
    {
    public:
        ModernKnobLnF()
        {
            setColour(juce::Label::textColourId, Colors::Text);
            setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
            setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        }

        juce::Label* createSliderTextBox(juce::Slider& slider) override
        {
            auto* label = LookAndFeel_V4::createSliderTextBox(slider);
            label->setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            label->setJustificationType(juce::Justification::centred);
            label->setBorderSize(juce::BorderSize<int>(0));
            label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::textColourId, Colors::Text.withAlpha(0.85f));
            return label;
        }

        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
            const float rotaryStartAngle, const float rotaryEndAngle, juce::Slider& slider) override
        {
            const float alpha = slider.isEnabled() ? 1.0f : 0.45f;
            const float radius = juce::jmax(0.0f, (float)juce::jmin(width, height) * 0.5f - 6.0f);
            if (radius <= 0.0f)
                return;

            const float startAngle = rotaryStartAngle;
            const float endAngle   = rotaryEndAngle;

            const float cx = (float)x + (float)width * 0.5f;
            const float cy = (float)y + (float)height * 0.5f;
            const float angle = KnobGeometry::upperArcAngle(startAngle, endAngle, sliderPos);

            juce::Rectangle<float> knobBounds(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

            // --- Arc track (background) ---
            const float arcRadius = radius + 3.5f;
            const float arcThickness = 3.6f;

            auto trackArc = KnobGeometry::createUpperArc(cx, cy, arcRadius, startAngle, endAngle);
            g.setColour(Colors::RingOff.withMultipliedAlpha(alpha));
            g.strokePath(trackArc, juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // --- Arc value (filled) ---
            if (sliderPos > 0.001f)
            {
                auto valueArc = KnobGeometry::createUpperArc(cx, cy, arcRadius, startAngle, angle);
                g.setColour(Nova::Colors::Accent.withMultipliedAlpha(alpha));
                g.strokePath(valueArc, juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                // Outer glow on value arc
                g.setColour(Nova::Colors::Accent.withAlpha(0.15f * alpha));
                g.strokePath(valueArc, juce::PathStrokeType(arcThickness + 6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            // --- Knob body shadow ---
            g.setColour(Colors::Shadow.withMultipliedAlpha(alpha));
            g.fillEllipse(knobBounds.translated(0.0f, 2.0f));

            // --- Knob body (warm navy gradient) ---
            juce::ColourGradient outerGrad(juce::Colour::fromString("ff1E2A3A"), cx, cy - radius,
                juce::Colour::fromString("ff0D1520"), cx, cy + radius, false);
            g.setGradientFill(outerGrad);
            g.fillEllipse(knobBounds);

            // --- Inner face ---
            juce::Rectangle<float> inner = knobBounds.reduced(radius * 0.14f);
            juce::ColourGradient faceGrad(juce::Colour::fromString("ff2A3A4C"), cx, inner.getY(),
                juce::Colour::fromString("ff151E2A"), cx, inner.getBottom(), false);
            g.setGradientFill(faceGrad);
            g.fillEllipse(inner);

            // --- Subtle borders ---
            g.setColour(juce::Colours::black.withAlpha(0.5f * alpha));
            g.drawEllipse(knobBounds, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.08f * alpha));
            g.drawEllipse(inner.reduced(0.5f), 1.0f);

            // --- Pointer line ---
            const float pointerLen = radius * 0.65f;
            const juce::Point<float> p0 = KnobGeometry::pointOnUpperArc(cx, cy, radius * 0.15f, angle);
            const juce::Point<float> p1 = KnobGeometry::pointOnUpperArc(cx, cy, pointerLen, angle);
            g.setColour(juce::Colours::white.withAlpha(0.92f * alpha));
            g.drawLine({ p0, p1 }, 2.2f);

            // --- Center dot (accent) ---
            g.setColour(Nova::Colors::Accent.withAlpha(0.85f * alpha));
            g.fillEllipse(cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
        }
    };

    class StudioTrimKnobLnF : public juce::LookAndFeel_V4
    {
    public:
        StudioTrimKnobLnF()
        {
            setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
            setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        }

        juce::Label* createSliderTextBox(juce::Slider& slider) override
        {
            auto* label = LookAndFeel_V4::createSliderTextBox(slider);
            label->setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            label->setJustificationType(juce::Justification::centred);
            label->setBorderSize(juce::BorderSize<int>(0));
            label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
            return label;
        }

        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
            const float rotaryStartAngle, const float rotaryEndAngle, juce::Slider& slider) override
        {
            const float alpha = slider.isEnabled() ? 1.0f : 0.45f;
            const auto area = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(5.0f);
            const float radius = juce::jmax(0.0f, juce::jmin(area.getWidth(), area.getHeight()) * 0.5f - 3.0f);
            if (radius <= 0.0f)
                return;

            const auto centre = area.getCentre();
            const float angle = KnobGeometry::upperArcAngle(rotaryStartAngle, rotaryEndAngle, sliderPos);
            const float arcRadius = radius + 4.0f;

            for (int i = 0; i <= 10; ++i)
            {
                const float t = (float)i / 10.0f;
                const float tickAngle = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
                const auto tickOuter = KnobGeometry::pointOnUpperArc(centre.x, centre.y, arcRadius + 2.5f, tickAngle);
                const auto tickInner = KnobGeometry::pointOnUpperArc(centre.x, centre.y, arcRadius - (i % 5 == 0 ? 6.0f : 3.5f), tickAngle);
                g.setColour((i % 5 == 0 ? juce::Colours::white : juce::Colour::fromString("ff6B7280")).withAlpha(alpha * 0.85f));
                g.drawLine({ tickInner, tickOuter }, i % 5 == 0 ? 1.6f : 1.0f);
            }

            auto track = KnobGeometry::createUpperArc(centre.x, centre.y, arcRadius, rotaryStartAngle, rotaryEndAngle);
            g.setColour(juce::Colour::fromString("ff4B5563").withAlpha(alpha * 0.55f));
            g.strokePath(track, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            auto valueArc = KnobGeometry::createUpperArc(centre.x, centre.y, arcRadius, rotaryStartAngle, angle);
            g.setColour(juce::Colour::fromString("ffD4A017").withAlpha(alpha));
            g.strokePath(valueArc, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            juce::Rectangle<float> knobBounds(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
            g.setColour(juce::Colours::black.withAlpha(0.45f * alpha));
            g.fillEllipse(knobBounds.translated(0.0f, 2.0f));

            juce::ColourGradient shell(juce::Colour::fromString("ffD5D7DB"), centre.x, knobBounds.getY(),
                juce::Colour::fromString("ff60646D"), centre.x, knobBounds.getBottom(), false);
            g.setGradientFill(shell);
            g.fillEllipse(knobBounds);

            const auto face = knobBounds.reduced(radius * 0.18f);
            juce::ColourGradient faceGrad(juce::Colour::fromString("ff2A2F38"), centre.x, face.getY(),
                juce::Colour::fromString("ff111827"), centre.x, face.getBottom(), false);
            g.setGradientFill(faceGrad);
            g.fillEllipse(face);

            g.setColour(juce::Colours::white.withAlpha(0.18f * alpha));
            g.drawEllipse(knobBounds.reduced(0.5f), 1.0f);
            g.setColour(juce::Colours::black.withAlpha(0.5f * alpha));
            g.drawEllipse(face, 1.0f);

            const auto innerPoint = KnobGeometry::pointOnUpperArc(centre.x, centre.y, radius * 0.18f, angle);
            const auto outerPoint = KnobGeometry::pointOnUpperArc(centre.x, centre.y, radius * 0.72f, angle);
            g.setColour(juce::Colour::fromString("ffF9FAFB").withAlpha(alpha));
            g.drawLine({ innerPoint, outerPoint }, 2.6f);

            g.setColour(juce::Colour::fromString("ff111827").withAlpha(alpha));
            g.fillEllipse(centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f);
            g.setColour(juce::Colour::fromString("ffD4A017").withAlpha(alpha));
            g.drawEllipse(centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f, 1.0f);
        }
    };
}
