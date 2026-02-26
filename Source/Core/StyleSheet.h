#pragma once
#include <JuceHeader.h>
#include <cmath>

namespace UI
{
    namespace Colors
    {
        const juce::Colour Background = juce::Colour::fromString("ff161616");
        const juce::Colour Panel = juce::Colour::fromString("ff252525");
        const juce::Colour Accent = juce::Colour::fromString("ff00d99f");
        const juce::Colour RingOff = juce::Colour::fromString("ff3a3a3a");
        const juce::Colour Text = juce::Colour::fromString("ffececec");
        const juce::Colour Shadow = juce::Colours::black.withAlpha(0.55f);
    }

    class ModernKnobLnF : public juce::LookAndFeel_V4
    {
    public:
        ModernKnobLnF()
        {
            setColour(juce::Label::textColourId, Colors::Text);
        }

        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
            const float rotaryStartAngle, const float rotaryEndAngle, juce::Slider& slider) override
        {
            juce::ignoreUnused(rotaryStartAngle, rotaryEndAngle);

            const float alpha = slider.isEnabled() ? 1.0f : 0.45f;
            const float radius = juce::jmax(0.0f, (float)juce::jmin(width, height) * 0.5f - 6.0f);
            if (radius <= 0.0f)
                return;

            const float startAngle = juce::MathConstants<float>::pi;      // 180°
            const float endAngle = juce::MathConstants<float>::twoPi;     // 360° == 0°

            const float cx = (float)x + (float)width * 0.5f;
            const float cy = (float)y + (float)height * 0.5f;
            const float angle = startAngle + sliderPos * (endAngle - startAngle);

            juce::Rectangle<float> knobBounds(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

            g.setColour(Colors::Shadow.withMultipliedAlpha(alpha));
            g.fillEllipse(knobBounds.translated(0.0f, 2.0f));

            juce::ColourGradient outerGrad(juce::Colour::fromString("ff2f2f2f"), cx, cy - radius,
                juce::Colour::fromString("ff111111"), cx, cy + radius, false);
            g.setGradientFill(outerGrad);
            g.fillEllipse(knobBounds);

            juce::Rectangle<float> inner = knobBounds.reduced(radius * 0.14f);
            juce::ColourGradient faceGrad(juce::Colour::fromString("ff4a4a4a"), cx, inner.getY(),
                juce::Colour::fromString("ff1d1d1d"), cx, inner.getBottom(), false);
            g.setGradientFill(faceGrad);
            g.fillEllipse(inner);

            g.setColour(juce::Colours::black.withAlpha(0.65f * alpha));
            g.drawEllipse(knobBounds, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.14f * alpha));
            g.drawEllipse(inner.reduced(0.5f), 1.0f);

            const float trackRadius = radius + 3.0f;

            const int scaleSteps = 10;
            const float tickInnerRadius = trackRadius + 1.0f;
            const float tickOuterRadius = trackRadius + 5.0f;
            const float labelRadius = trackRadius + 13.0f;
            const float fontSize = juce::jlimit(6.0f, 10.0f, radius * 0.26f);
            g.setFont(juce::Font(fontSize, juce::Font::bold));

            for (int i = 0; i <= scaleSteps; ++i)
            {
                const float t = (float)i / (float)scaleSteps;
                const float markAngle = startAngle + t * (endAngle - startAngle);
                const bool isActive = (t <= sliderPos + 0.0001f);
                const bool isMajor = (i == 0 || i == 5 || i == 10);

                const float oRadius = tickOuterRadius + (isMajor ? 1.8f : 0.0f);
                const juce::Point<float> pIn(cx + std::cos(markAngle) * tickInnerRadius,
                    cy + std::sin(markAngle) * tickInnerRadius);
                const juce::Point<float> pOut(cx + std::cos(markAngle) * oRadius,
                    cy + std::sin(markAngle) * oRadius);

                g.setColour((isActive ? Colors::Accent : Colors::RingOff).withMultipliedAlpha(alpha));
                g.drawLine({ pIn, pOut }, isMajor ? 1.8f : 1.2f);

                const juce::Point<float> pLbl(cx + std::cos(markAngle) * labelRadius,
                    cy + std::sin(markAngle) * labelRadius);
                const float lblW = (i == 10) ? 14.0f : 10.0f;
                juce::Rectangle<int> lbl((int)std::round(pLbl.x - lblW * 0.5f),
                    (int)std::round(pLbl.y - 5.0f),
                    (int)std::round(lblW),
                    10);

                g.setColour((isActive ? Colors::Text : Colors::Text.withAlpha(0.55f)).withMultipliedAlpha(alpha));
                g.drawFittedText(juce::String(i), lbl, juce::Justification::centred, 1);
            }

            const float pointerLen = radius * 0.72f;
            const juce::Point<float> p0(cx + std::cos(angle) * (radius * 0.12f),
                cy + std::sin(angle) * (radius * 0.12f));
            const juce::Point<float> p1(cx + std::cos(angle) * pointerLen,
                cy + std::sin(angle) * pointerLen);
            g.setColour(juce::Colours::white.withAlpha(0.95f * alpha));
            g.drawLine({ p0, p1 }, 2.6f);

            g.setColour(Colors::Accent.withAlpha(0.9f * alpha));
            g.fillEllipse(cx - 2.2f, cy - 2.2f, 4.4f, 4.4f);
        }
    };
}
