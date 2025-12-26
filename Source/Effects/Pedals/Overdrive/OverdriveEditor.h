#pragma once
#include <JuceHeader.h>

class OverdriveEditor : public juce::AudioProcessorEditor
{
public:
    // Recibimos referencias directas a los parámetros para no depender de la clase del pedal
    OverdriveEditor(juce::AudioProcessor& p, juce::AudioParameterFloat* drive, juce::AudioParameterFloat* level)
        : AudioProcessorEditor(&p)
    {
        // Configuración de Drive
        addAndMakeVisible(driveSlider);
        driveSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        driveSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        driveAttachment.reset(new juce::SliderParameterAttachment(*drive, driveSlider));

        // Configuración de Level
        addAndMakeVisible(levelSlider);
        levelSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        levelSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        levelAttachment.reset(new juce::SliderParameterAttachment(*level, levelSlider));

        // Etiquetas
        addAndMakeVisible(driveLabel);
        driveLabel.setText("DRIVE", juce::dontSendNotification);
        driveLabel.setJustificationType(juce::Justification::centred);

        addAndMakeVisible(levelLabel);
        levelLabel.setText("LEVEL", juce::dontSendNotification);
        levelLabel.setJustificationType(juce::Justification::centred);

        setSize(200, 300);
    }

    void paint(juce::Graphics& g) override
    {
        // Aquí podrías cargar una imagen desde "Resources/Fondo.png"
        // Por ahora mantenemos el código vectorial:
        g.fillAll(juce::Colours::darkgreen);
        g.setColour(juce::Colours::white);
        g.drawRect(getLocalBounds(), 4);
        g.setFont(20.0f);
        g.drawText("PRO OVERDRIVE", getLocalBounds().removeFromTop(40), juce::Justification::centred, true);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(20);
        auto topArea = area.removeFromTop(area.getHeight() / 2);

        driveLabel.setBounds(topArea.removeFromTop(20));
        driveSlider.setBounds(topArea);

        levelLabel.setBounds(area.removeFromTop(20));
        levelSlider.setBounds(area);
    }

private:
    juce::Slider driveSlider, levelSlider;
    juce::Label driveLabel, levelLabel;
    std::unique_ptr<juce::SliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> levelAttachment;
};