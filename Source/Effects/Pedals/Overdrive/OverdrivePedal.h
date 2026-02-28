#pragma once

#include "../Base/ProcessorBase.h"
#include "OverdriveEditor.h"
#include <juce_dsp/juce_dsp.h>

// -----------------------------------------------------------------------------
// High-performance clipper (fast tanh approximation + slight asymmetry)
// -----------------------------------------------------------------------------
struct SotaClipper
{
    void prepare(const juce::dsp::ProcessSpec&) {}
    void reset() {}

    inline float fastTanh(float x) const noexcept
    {
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    template <typename ProcessContext>
    void process(const ProcessContext& context) noexcept
    {
        auto&& in = context.getInputBlock();
        auto&& out = context.getOutputBlock();

        const size_t numChannels = in.getNumChannels();
        const size_t numSamples = in.getNumSamples();

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* src = in.getChannelPointer(ch);
            auto* dst = out.getChannelPointer(ch);

            for (size_t i = 0; i < numSamples; ++i)
            {
                const float x = src[i];
                const float isNegative = (float)(x < 0.0f);
                const float driveFactor = 1.0f + (0.2f * isNegative);

                dst[i] = fastTanh(x / driveFactor) * driveFactor;
            }
        }
    }
};

// -----------------------------------------------------------------------------
// Overdrive pedal
// -----------------------------------------------------------------------------
class OverdrivePedal final : public ProcessorBase
{
public:
    OverdrivePedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("drive", "Drive", 0.0f, 100.0f, 25.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("level", "Level", 0.0f, 1.0f, 0.1f));
    }

    const juce::String getName() const override { return "Overdrive"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new OverdriveEditor(*this, driveParam, levelParam);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.initProcessing(static_cast<size_t>(samplesPerBlock));
        const double innerSampleRate = sampleRate * 4.0;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = innerSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock) * 4;
        spec.numChannels = 2;

        processorChain.prepare(spec);

        *processorChain.get<0>().state =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerSampleRate, 300.0f);

        processorChain.get<1>().setRampDurationSeconds(0.05); // Drive
        processorChain.get<4>().setRampDurationSeconds(0.05); // Level

        *processorChain.get<3>().state =
            *juce::dsp::IIR::Coefficients<float>::makeLowPass(innerSampleRate, 3500.0f);
        *processorChain.get<5>().state =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerSampleRate, 20.0f);

        setLatencySamples(oversampler.getLatencyInSamples());
        prepareBypassSmoother(sampleRate, samplesPerBlock);

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

    void releaseResources() override
    {
        isPrepared = false;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::dsp::AudioBlock<float> block(buffer);

        processorChain.get<1>().setGainLinear(
            juce::Decibels::decibelsToGain(static_cast<float>(*driveParam) * 0.6f));

        processorChain.get<4>().setGainLinear(*levelParam);

        auto upsampledBlock = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampledBlock);

        processorChain.process(context);
        oversampler.processSamplesDown(block);
        endBypassProcess(buffer);
    }

private:
    using Chain = juce::dsp::ProcessorChain<
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>, // Pre
        juce::dsp::Gain<float>,       // Drive
        SotaClipper,                  // Clip
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>, // Post
        juce::dsp::Gain<float>,       // Level
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> // DC blocker
    >;

    Chain processorChain;
    juce::dsp::Oversampling<float> oversampler;

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    bool isPrepared = false;
};
