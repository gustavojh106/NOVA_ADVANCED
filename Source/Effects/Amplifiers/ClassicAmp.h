#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <memory>

#include "../Pedals/Base/ProcessorBase.h"

class ClassicAmpEditor final : public juce::AudioProcessorEditor
{
public:
    ClassicAmpEditor(juce::AudioProcessor& processor,
        juce::AudioParameterFloat* drive,
        juce::AudioParameterFloat* level)
        : juce::AudioProcessorEditor(&processor)
    {
        configureSlider(driveSlider);
        configureSlider(levelSlider);

        driveAttachment = std::make_unique<juce::SliderParameterAttachment>(*drive, driveSlider);
        levelAttachment = std::make_unique<juce::SliderParameterAttachment>(*level, levelSlider);

        title.setText("CLASSIC AMP", juce::dontSendNotification);
        title.setJustificationType(juce::Justification::centred);
        title.setFont(juce::Font(13.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(title);

        driveLabel.setText("DRIVE", juce::dontSendNotification);
        driveLabel.setJustificationType(juce::Justification::centred);
        driveLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(driveLabel);

        levelLabel.setText("LEVEL", juce::dontSendNotification);
        levelLabel.setJustificationType(juce::Justification::centred);
        levelLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(levelLabel);

        addAndMakeVisible(driveSlider);
        addAndMakeVisible(levelSlider);

        setSize(140, 190);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();

        g.fillAll(juce::Colour::fromString("ff1a1a1a"));
        g.setColour(juce::Colours::grey.withAlpha(0.35f));
        g.drawRoundedRectangle(bounds.reduced(2.0f), 6.0f, 2.0f);

        g.setColour(juce::Colours::red.withAlpha(0.7f));
        g.fillEllipse(bounds.getCentreX() - 3.0f, 24.0f, 6.0f, 6.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        title.setBounds(area.removeFromTop(20));
        area.removeFromTop(8);

        auto row = area.removeFromTop(120);
        auto left = row.removeFromLeft(row.getWidth() / 2);

        driveSlider.setBounds(left.reduced(6));
        levelSlider.setBounds(row.reduced(6));

        auto labels = area.removeFromTop(26);
        driveLabel.setBounds(labels.removeFromLeft(labels.getWidth() / 2));
        levelLabel.setBounds(labels);
    }

private:
    static void configureSlider(juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRotaryParameters(juce::MathConstants<float>::pi,
            juce::MathConstants<float>::twoPi,
            true);
    }

    juce::Slider driveSlider;
    juce::Slider levelSlider;

    juce::Label title;
    juce::Label driveLabel;
    juce::Label levelLabel;

    std::unique_ptr<juce::SliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> levelAttachment;
};

class ClassicAmp final : public ProcessorBase
{
public:
    ClassicAmp()
    {
        addParameter(driveParam = new juce::AudioParameterFloat("ampDrive", "Drive", 0.5f, 6.0f, 2.5f));
        addParameter(levelParam = new juce::AudioParameterFloat("ampLevel", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Classic Amp"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new ClassicAmpEditor(*this, driveParam, levelParam);
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!shouldProcess(buffer))
            return;

        const float drive = driveParam != nullptr ? *driveParam : 2.5f;
        const float level = levelParam != nullptr ? *levelParam : 1.0f;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* channelData = buffer.getWritePointer(channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                channelData[sample] = std::tanh(channelData[sample] * drive) * level;
        }
    }

private:
    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;
};
