#pragma once
#include "../Base/ProcessorBase.h" // Ajusta la ruta según tu estructura
#include "OverdriveEditor.h"          // Incluimos el editor hermano
#include <juce_dsp/juce_dsp.h>

class OverdrivePedal : public ProcessorBase
{
public:
    OverdrivePedal() :
        oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("drive", "Drive", 0.0f, 100.0f, 25.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("level", "Level", 0.0f, 1.0f, 0.1f));

        auto& waveshaper = processorChain.get<1>();
        waveshaper.functionToUse = [](float x) { return std::tanh(x); };
    }

    // Regla de Estructura: La lógica llama al editor correspondiente
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new OverdriveEditor(*this, driveParam, levelParam);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0) return;

        oversampler.initProcessing(static_cast<size_t>(samplesPerBlock));

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate * 4.0;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock) * 4;
        spec.numChannels = 2;

        processorChain.prepare(spec);
        setLatencySamples(oversampler.getLatencyInSamples());

        oversampler.reset();
        processorChain.reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared) return;

        juce::dsp::AudioBlock<float> block(buffer);

        // Actualizamos parámetros del DSP con los valores de los parámetros
        processorChain.get<0>().setGainLinear(*driveParam);
        processorChain.get<2>().setGainLinear(*levelParam);

        // Proceso
        juce::dsp::AudioBlock<float> upsampledBlock = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampledBlock);
        processorChain.process(context);
        oversampler.processSamplesDown(block);
    }

    const juce::String getName() const override { return "Overdrive"; }

private:
    // Cadena DSP: Gain -> Distorsión (Tanh) -> Gain (Level)
    using Chain = juce::dsp::ProcessorChain<juce::dsp::Gain<float>, juce::dsp::WaveShaper<float>, juce::dsp::Gain<float>>;
    Chain processorChain;
    juce::dsp::Oversampling<float> oversampler;

    // Parámetros (Punteros gestionados por AudioProcessor)
    juce::AudioParameterFloat* driveParam;
    juce::AudioParameterFloat* levelParam;

    bool isPrepared = false;
};