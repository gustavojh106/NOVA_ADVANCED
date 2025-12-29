#pragma once
#include "../Base/ProcessorBase.h"
#include "OverdriveEditor.h"
#include <juce_dsp/juce_dsp.h>

class OverdrivePedal : public ProcessorBase
{
public:
    OverdrivePedal() :
        oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("drive", "Drive", 0.0f, 100.0f, 25.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("level", "Level", 0.0f, 1.0f, 0.1f));

        // --- 1. CONFIGURACIÓN SOTA DE LA DISTORSIÓN ---
        auto& waveshaper = processorChain.get<2>(); // Índice 2 es el WaveShaper ahora
        waveshaper.functionToUse = [](float x)
            {
                // ALGORITMO: Asymmetrical Soft Clipping
                // Simula un circuito de diodos donde un lado recorta más que el otro.
                // Esto genera armónicos de 2do orden (Sonido "Válvula/Cálido").

                // Rango positivo: Recorte suave (Soft Knee)
                if (x > 0.0f)
                    return std::tanh(x);

                // Rango negativo: Recorte más duro y desplazado (Asimetría)
                // El factor 1.2 estira la curva negativa.
                return std::tanh(x / 1.2f) * 1.2f;
            };
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new OverdriveEditor(*this, driveParam, levelParam);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0) return;

        oversampler.initProcessing(static_cast<size_t>(samplesPerBlock));

        // El procesamiento interno corre a 4x (Oversampling)
        double innerSampleRate = sampleRate * 4.0;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = innerSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock) * 4;
        spec.numChannels = 2;

        processorChain.prepare(spec);

        // --- 2. CONFIGURACIÓN DE FILTROS (EL SECRETO DEL TONO) ---

        // A. PRE-FILTER (Tightness): 
        // Cortamos bajos sucios (HighPass) antes de la distorsión. 
        // 720Hz es el estándar del Tube Screamer, pero 300Hz es más moderno/versátil.
        *processorChain.get<0>().state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerSampleRate, 300.0f);

        // B. RAMPING (Suavizado de controles)
        processorChain.get<1>().setRampDurationSeconds(0.05); // Drive Gain
        processorChain.get<4>().setRampDurationSeconds(0.05); // Output Level

        // C. POST-FILTER (Smoothness):
        // Cortamos el "fizz" digital (LowPass) después de la distorsión.
        // 3500Hz suaviza sin matar el brillo.
        *processorChain.get<3>().state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(innerSampleRate, 3500.0f);

        setLatencySamples(oversampler.getLatencyInSamples());

        reset();
        isPrepared = true;
    }

    void reset() override
    {
        oversampler.reset();
        processorChain.reset();
        if (isPrepared)
        {
            // Forzamos valores iniciales
            processorChain.get<1>().setGainLinear(*driveParam);
            processorChain.get<4>().setGainLinear(*levelParam);
        }
    }

    void releaseResources() override { isPrepared = false; }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared) return;

        juce::dsp::AudioBlock<float> block(buffer);

        // Actualizamos parámetros (Gain maneja el ramping interno)
        // NOTA: Multiplicamos el driveParam para tener más "jugo" ya que filtramos antes
        processorChain.get<1>().setGainLinear(juce::Decibels::decibelsToGain(static_cast<float>(*driveParam) * 0.6f));
        processorChain.get<4>().setGainLinear(*levelParam);

        // Proceso DSP Completo
        juce::dsp::AudioBlock<float> upsampledBlock = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampledBlock);
        processorChain.process(context);
        oversampler.processSamplesDown(block);
    }

    const juce::String getName() const override { return "Overdrive"; }

private:
    // --- 3. TOPOLOGÍA ANALÓGICA VIRTUAL ---
    // Cadena: 
    // [0] Pre-Filter (IIR): Limpia el barro antes de distorsionar.
    // [1] Drive Gain: Sube el volumen para golpear el clipper.
    // [2] WaveShaper: El diodo asimétrico.
    // [3] Post-Filter (IIR): Simula la pérdida de agudos del circuito.
    // [4] Output Level: Volumen final.
    using Chain = juce::dsp::ProcessorChain<
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>, // Pre-Filter
        juce::dsp::Gain<float>,       // Drive
        juce::dsp::WaveShaper<float>, // Clipper
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>, // Post-Filter
        juce::dsp::Gain<float>        // Level
    >;

    Chain processorChain;
    juce::dsp::Oversampling<float> oversampler;

    juce::AudioParameterFloat* driveParam;
    juce::AudioParameterFloat* levelParam;

    bool isPrepared = false;
};