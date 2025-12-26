#pragma once
#include "../Pedals/Base/ProcessorBase.h" // Ruta relativa hacia Base
#include "CabinetEditor.h"
#include <juce_dsp/juce_dsp.h>

class CabinetPedal : public ProcessorBase
{
public:
    CabinetPedal()
    {
        // Lógica de carga de IR
        auto irFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory().getChildFile("demo.wav");

        // Fallback para desarrollo
        if (!irFile.existsAsFile())
            irFile = juce::File(__FILE__).getParentDirectory().getParentDirectory()
            .getParentDirectory().getChildFile("Assets/Audio/demo.wav");
        // Nota: Ajusta los .getParentDirectory() según la profundidad de esta carpeta

        if (irFile.existsAsFile())
        {
            convolution.loadImpulseResponse(irFile,
                juce::dsp::Convolution::Stereo::yes,
                juce::dsp::Convolution::Trim::yes,
                0,
                juce::dsp::Convolution::Normalise::yes);
        }
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0) return;
        juce::dsp::ProcessSpec spec{ sampleRate, static_cast<juce::uint32>(samplesPerBlock), 2 };
        convolution.prepare(spec);
        convolution.reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (isPrepared && convolution.getCurrentIRSize() > 0)
        {
            juce::dsp::AudioBlock<float> block(buffer);
            juce::dsp::ProcessContextReplacing<float> context(block);
            convolution.process(context);
        }
    }

    const juce::String getName() const override { return "Cabinet"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new CabinetEditor(*this);
    }

private:
    juce::dsp::Convolution convolution;
    bool isPrepared = false;
};