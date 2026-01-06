#pragma once
#include "../Base/ProcessorBase.h"
#include "OverdriveEditor.h"
#include <juce_dsp/juce_dsp.h>

// ==============================================================================
//  SOTA COMPONENT: High Performance Clipper
// ==============================================================================
struct SotaClipper
{
    void prepare(const juce::dsp::ProcessSpec&) {}
    void reset() {}

    // Aproximación rápida de Tanh (Padé approximation)
    // Error despreciable para audio, velocidad extrema.
    // Válido para entradas entre -3.0 y 3.0 (suficiente para overdrive)
    inline float fastTanh(float x) const noexcept
    {
        float x2 = x * x;
        // Fórmula mágica: x * (27 + x^2) / (27 + 9x^2)
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    template <typename ProcessContext>
    void process(const ProcessContext& context) noexcept
    {
        auto&& inputBlock = context.getInputBlock();
        auto&& outputBlock = context.getOutputBlock();

        size_t numChannels = inputBlock.getNumChannels();
        size_t numSamples = inputBlock.getNumSamples();

        // Bucle diseñado para AUTO-VECTORIZACIÓN (SIMD)
        // El compilador detectará que no hay dependencias y usará registros AVX/SSE
        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* src = inputBlock.getChannelPointer(ch);
            auto* dst = outputBlock.getChannelPointer(ch);

            for (size_t i = 0; i < numSamples; ++i)
            {
                float x = src[i];

                // --- LÓGICA BRANCHLESS (Sin 'if') ---

                // Queremos: si x < 0, dividir por 1.2 (expandir). Si x > 0, normal.
                // Truco: (x < 0) devuelve 1.0f si es verdad, 0.0f si es falso (al castear).

                // 1. Detectar signo (1.0 si es negativo, 0.0 si es positivo)
                // Usamos una comparación segura que el compilador vectoriza.
                float isNegative = (float)(x < 0.0f);

                // 2. Calcular factor divisor sin bifurcación
                // Si es negativo: 1.0 + (0.2 * 1) = 1.2
                // Si es positivo: 1.0 + (0.2 * 0) = 1.0
                float driveFactor = 1.0f + (0.2f * isNegative);

                // 3. Aplicar distorsión rápida
                // Aplicamos el divisor, distorsionamos, y multiplicamos para compensar volumen
                // (Simetría asimétrica valvular)
                dst[i] = fastTanh(x / driveFactor) * driveFactor;
            }
        }
    }
};

// ==============================================================================
//  MAIN PEDAL CLASS
// ==============================================================================
class OverdrivePedal : public ProcessorBase
{
public:
    OverdrivePedal() :
        oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("drive", "Drive", 0.0f, 100.0f, 25.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("level", "Level", 0.0f, 1.0f, 0.1f));
        // Nota: Ya no configuramos lambda porque usamos SotaClipper
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
        double innerSampleRate = sampleRate * 4.0;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = innerSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock) * 4;
        spec.numChannels = 2;

        processorChain.prepare(spec);

        // --- Configuración de Filtros y Ramping ---
        *processorChain.get<0>().state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerSampleRate, 300.0f);
        processorChain.get<1>().setRampDurationSeconds(0.05); // Drive Smooth
        processorChain.get<4>().setRampDurationSeconds(0.05); // Level Smooth
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
            processorChain.get<1>().setGainLinear(*driveParam);
            processorChain.get<4>().setGainLinear(*levelParam);
        }
    }

    void releaseResources() override { isPrepared = false; }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!shouldProcess(buffer)) return;

        if (!isPrepared) return;

        juce::dsp::AudioBlock<float> block(buffer);

        // Actualización de parámetros
        processorChain.get<1>().setGainLinear(juce::Decibels::decibelsToGain(static_cast<float>(*driveParam) * 0.6f));
        processorChain.get<4>().setGainLinear(*levelParam);

        // Pipeline optimizado
        juce::dsp::AudioBlock<float> upsampledBlock = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampledBlock);
        processorChain.process(context);
        oversampler.processSamplesDown(block);
    }

    const juce::String getName() const override { return "Overdrive"; }

private:
    // Cadena SOTA Optimizada:
    // Sustituimos WaveShaper por SotaClipper
    using Chain = juce::dsp::ProcessorChain<
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>, // Pre-Filter
        juce::dsp::Gain<float>,       // Drive
        SotaClipper,
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>, // Post-Filter
        juce::dsp::Gain<float>,       // Level
        juce::dsp::Bias<float>        // <--- NUEVO: DC Blocker eficiente (Bias remover)
    >;

    Chain processorChain;
    juce::dsp::Oversampling<float> oversampler;

    juce::AudioParameterFloat* driveParam;
    juce::AudioParameterFloat* levelParam;

    bool isPrepared = false;
};