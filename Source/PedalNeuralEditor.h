#pragma once
#include <JuceHeader.h>
#include "PedalNeural.h"

class PedalNeuralEditor : public juce::AudioProcessorEditor
{
public:
    // Recibimos una referencia al procesador para poder controlar sus variables
    PedalNeuralEditor(PedalNeural& p);
    ~PedalNeuralEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    PedalNeural& audioProcessor;

    // Elementos de la UI
    juce::Slider driveKnob;
    juce::Slider levelKnob;

    juce::Label driveLabel;
    juce::Label levelLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalNeuralEditor)
};