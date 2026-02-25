#pragma once
#include <JuceHeader.h>
#include <cmath>

// ==============================================================================
// EDITOR (GUI DEL AMPLIFICADOR)
// ==============================================================================
class ClassicAmpEditor : public juce::AudioProcessorEditor
{
public:
    ClassicAmpEditor(juce::AudioProcessor& p) : AudioProcessorEditor(&p)
    {
        setSize(120, 180); // Medida estándar de tus pedales
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // Fondo oscuro estilo "cabezal de amplificador"
        g.fillAll(juce::Colour::fromString("ff1a1a1a"));

        // Borde sutil
        g.setColour(juce::Colours::grey.withAlpha(0.3f));
        g.drawRoundedRectangle(bounds.reduced(2.0f), 6.0f, 2.0f);

        // Título central
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(16.0f, juce::Font::bold));
        g.drawFittedText("CLASSIC\nAMP", bounds.reduced(10).toNearestInt(), juce::Justification::centredTop, 2);

        // Indicador LED de encendido
        g.setColour(juce::Colours::red);
        g.fillEllipse(bounds.getCentreX() - 4.0f, 60.0f, 8.0f, 8.0f);

        // Brillo del LED
        g.setColour(juce::Colours::red.withAlpha(0.3f));
        g.fillEllipse(bounds.getCentreX() - 8.0f, 56.0f, 16.0f, 16.0f);
    }
};

// ==============================================================================
// PROCESSOR (DSP DEL AMPLIFICADOR)
// ==============================================================================
class ClassicAmp : public juce::AudioProcessor
{
public:
    ClassicAmp() : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    }

    ~ClassicAmp() override = default;

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        // DSP de prueba: Simulación muy básica de tubo (Soft-Clipping con tangente hiperbólica)
        const float drive = 2.5f; // Cantidad de saturación fija

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* channelData = buffer.getWritePointer(channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                float input = channelData[sample];
                channelData[sample] = std::tanh(input * drive);
            }
        }
    }

    // Funciones vitales para la interfaz gráfica
    juce::AudioProcessorEditor* createEditor() override { return new ClassicAmpEditor(*this); }
    bool hasEditor() const override { return true; }

    // Funciones genéricas de JUCE
    const juce::String getName() const override { return "Classic Amp"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};