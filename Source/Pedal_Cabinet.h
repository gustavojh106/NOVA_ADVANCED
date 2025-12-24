#pragma once
#include "ProcessorBase.h"
#include <juce_dsp/juce_dsp.h>

class PedalCabinet : public ProcessorBase
{
public:
    PedalCabinet()
    {
        // TRUCO PRO:
        // juce::File(__FILE__) obtiene la ruta completa de ESTE archivo de código (.h).
        // .getParentDirectory() nos da la carpeta "Source".
        // .getChildFile("demo.wav") busca el audio ahí mismo.
        auto irFile = juce::File(__FILE__).getParentDirectory().getChildFile("demo.wav");

        // Verificación de seguridad (Evita el crash __debugbreak)
        if (irFile.existsAsFile())
        {
            convolution.loadImpulseResponse(irFile,
                juce::dsp::Convolution::Stereo::yes,
                juce::dsp::Convolution::Trim::yes,
                0,
                juce::dsp::Convolution::Normalise::yes);
        }
        else
        {
            // Si sale esto en el Output de Visual Studio, el archivo no está en la carpeta Source
            DBG("ERROR FATAL: No encuentro el IR en: " << irFile.getFullPathName());
        }
    }
    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        // Protección extra: Si el sampleRate es 0 (error de driver), no inicializamos para evitar división por cero
        if (sampleRate <= 0) return;

        juce::dsp::ProcessSpec spec{ sampleRate, static_cast<juce::uint32>(samplesPerBlock), 2 };
        convolution.prepare(spec);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);

        // Si el IR no cargó, esto simplemente pasa el audio sin procesar (Bypass)
        convolution.process(context);
    }

    const juce::String getName() const override { return "Cabinet"; }

    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

private:
    juce::dsp::Convolution convolution;
};