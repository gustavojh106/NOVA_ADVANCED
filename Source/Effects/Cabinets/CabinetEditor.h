#pragma once
#include <JuceHeader.h>

class CabinetEditor : public juce::AudioProcessorEditor
{
public:
    CabinetEditor(juce::AudioProcessor& p) : AudioProcessorEditor(&p)
    {
        setSize(200, 300);
    }

    void paint(juce::Graphics& g) override
    {
        // Fondo estilo Rejilla
        g.fillAll(juce::Colour::fromFloatRGBA(0.15f, 0.15f, 0.15f, 1.0f));

        g.setColour(juce::Colours::black);
        for (int i = 0; i < getHeight(); i += 10)
            g.drawHorizontalLine(i, 0, (float)getWidth());

        g.setColour(juce::Colours::silver);
        g.drawRect(getLocalBounds(), 4);

        // Placa
        g.setColour(juce::Colours::white);
        g.setFont(20.0f);
        g.drawRect(20, 40, 160, 60, 2);
        g.fillRect(20, 40, 160, 60);
        g.setColour(juce::Colours::black);
        g.drawText("CABINET 4x12", 20, 40, 160, 60, juce::Justification::centred, true);
    }
};