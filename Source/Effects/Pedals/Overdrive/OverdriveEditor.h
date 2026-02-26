#pragma once

#include <JuceHeader.h>
#include "StyleSheet.h"

class OverdriveEditor final : public juce::AudioProcessorEditor
{
public:
    OverdriveEditor(juce::AudioProcessor& p,
        juce::AudioParameterFloat* drive,
        juce::AudioParameterFloat* level)
        : juce::AudioProcessorEditor(&p)
    {
        setLookAndFeel(lnf);

        // Drive
        configureKnob(driveSlider);
        driveAttachment = std::make_unique<juce::SliderParameterAttachment>(*drive, driveSlider);

        // Level
        configureKnob(levelSlider);
        levelAttachment = std::make_unique<juce::SliderParameterAttachment>(*level, levelSlider);

        // Labels
        configureLabel(driveLabel, "DRIVE", 14.0f);
        configureLabel(levelLabel, "LEVEL", 14.0f);

        configureLabel(titleLabel, "OD-808", 22.0f);
        titleLabel.setColour(juce::Label::textColourId, UI::Colors::Accent);

        setSize(180, 280);
    }

    ~OverdriveEditor() override
    {
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();

        // Body
        g.setColour(UI::Colors::Panel);
        g.fillRoundedRectangle(bounds, 10.0f);

        // Subtle bevel
        g.setColour(juce::Colours::white.withAlpha(0.1f));
        g.drawRoundedRectangle(bounds.reduced(1.0f), 10.0f, 2.0f);
        g.setColour(juce::Colours::black.withAlpha(0.3f));
        g.drawRoundedRectangle(bounds.reduced(2.0f), 10.0f, 2.0f);

        // Screws
        drawScrew(g, 10, 10);
        drawScrew(g, getWidth() - 10, 10);
        drawScrew(g, 10, getHeight() - 10);
        drawScrew(g, getWidth() - 10, getHeight() - 10);

        // LED
        drawLed(g);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(15);

        titleLabel.setBounds(area.removeFromTop(30));
        area.removeFromTop(20); // LED space

        auto knobArea = area.removeFromTop(120);
        auto leftCol = knobArea.removeFromLeft(knobArea.getWidth() / 2);

        driveLabel.setBounds(leftCol.removeFromBottom(20));
        driveSlider.setBounds(leftCol.reduced(5));

        levelLabel.setBounds(knobArea.removeFromBottom(20));
        levelSlider.setBounds(knobArea.reduced(5));
    }

private:
    static void configureKnob(juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRotaryParameters(juce::MathConstants<float>::pi,
            juce::MathConstants<float>::twoPi,
            true);
    }

    static void configureLabel(juce::Label& l, const juce::String& text, float fontSize)
    {
        l.setText(text, juce::dontSendNotification);
        l.setJustificationType(juce::Justification::centred);
        l.setFont(juce::Font(fontSize, juce::Font::bold));
    }

    static void drawScrew(juce::Graphics& g, int x, int y)
    {
        g.setColour(juce::Colours::darkgrey.darker());
        g.fillEllipse((float)(x - 4), (float)(y - 4), 8.0f, 8.0f);

        g.setColour(juce::Colours::grey);
        g.drawLine((float)(x - 2), (float)(y - 2), (float)(x + 2), (float)(y + 2), 2.0f);
        g.drawLine((float)(x - 2), (float)(y + 2), (float)(x + 2), (float)(y - 2), 2.0f);
    }

    void drawLed(juce::Graphics& g) const
    {
        const auto ledBounds = juce::Rectangle<float>((float)getWidth() * 0.5f - 4.0f, 45.0f, 8.0f, 8.0f);

        g.setColour(juce::Colours::red);
        g.fillEllipse(ledBounds);

        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.fillEllipse(ledBounds.reduced(2.0f));

        g.setGradientFill(juce::ColourGradient(
            juce::Colours::red.withAlpha(0.5f), ledBounds.getCentreX(), ledBounds.getCentreY(),
            juce::Colours::transparentBlack, ledBounds.getCentreX(), ledBounds.getCentreY() + 10.0f, true));

        g.fillEllipse(ledBounds.expanded(5.0f));
    }

private:
    juce::Slider driveSlider, levelSlider;
    juce::Label  driveLabel, levelLabel, titleLabel;

    std::unique_ptr<juce::SliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> levelAttachment;

    juce::SharedResourcePointer<UI::ModernKnobLnF> lnf;
};
