#pragma once

#include "../Base/ProcessorBase.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

// ============================================================================
//  Germanium Fuzz — two-stage asymmetric clip, voltage sag, gated gate
//  Reference: Fuzz Face sag/sputter, Tonebender Mk II body
// ============================================================================
class FuzzPedal final : public ProcessorBase
{
public:
    FuzzPedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(fuzzParam  = new juce::AudioParameterFloat("fuzzAmount", "Fuzz",  0.0f, 100.0f, 65.0f));
        addParameter(toneParam  = new juce::AudioParameterFloat("fuzzTone",   "Tone",  0.0f, 1.0f, 0.5f));
        addParameter(gateParam  = new juce::AudioParameterFloat("fuzzGate",   "Gate",  0.0f, 1.0f, 0.3f));
        addParameter(mixParam   = new juce::AudioParameterFloat("fuzzMix",    "Mix",   0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("fuzzLevel",  "Level", 0.0f, 1.0f, 0.55f));
    }

    const juce::String getName() const override { return "Fuzz"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.reset();
        oversampler.initProcessing((size_t)juce::jmax(1, samplesPerBlock));

        currentInnerRate = sampleRate * 4.0;

        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate = currentInnerRate;
        innerSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock * 4);
        innerSpec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        inputFilter.prepare(innerSpec);
        toneFilter.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        fuzzSmooth.reset(sampleRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        gateSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        mixSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        fuzzSmooth.setCurrentAndTargetValue(fuzzParam != nullptr ? *fuzzParam : 65.0f);
        gateSmooth.setCurrentAndTargetValue(gateParam != nullptr ? *gateParam : 0.3f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.55f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        cachedTone = std::numeric_limits<float>::quiet_NaN();

        setProcessingLatency((int)oversampler.getLatencyInSamples());
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        oversampler.reset();
        inputFilter.reset();
        toneFilter.reset();
        dcBlock.reset();
        inputEnvelope = 0.0f;
        sagEnvelope = 0.0f;
        gateOpen = false;

        fuzzSmooth.setCurrentAndTargetValue(fuzzParam != nullptr ? *fuzzParam : 65.0f);
        gateSmooth.setCurrentAndTargetValue(gateParam != nullptr ? *gateParam : 0.3f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.55f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        if (scratchBuffer.getNumChannels() < buffer.getNumChannels()
            || scratchBuffer.getNumSamples() < buffer.getNumSamples())
        {
            scratchBuffer.setSize(buffer.getNumChannels(), buffer.getNumSamples(), false, false, true);
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch), buffer.getNumSamples());

        updateToneIfNeeded();
        fuzzSmooth.setTargetValue(fuzzParam != nullptr ? *fuzzParam : 65.0f);
        gateSmooth.setTargetValue(gateParam != nullptr ? *gateParam : 0.3f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 0.55f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);

        // Input high-pass (remove sub rumble before clipping)
        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            inputFilter.process(ctx);
        }

        // ---- Germanium fuzz processing at 4x ----
        {
            const int channels = (int)upsampled.getNumChannels();
            const int samples = (int)upsampled.getNumSamples();

            for (int sample = 0; sample < samples; ++sample)
            {
                const float fuzzAmount = fuzzSmooth.getNextValue();
                const float gateAmount = gateSmooth.getNextValue();
                const float fuzzNorm = fuzzAmount * 0.01f;  // 0-1

                const float drive = 2.0f + fuzzAmount * 0.48f;
                const float bias = 0.06f + fuzzAmount * 0.002f;

                // Octave-up blend (subtle at low fuzz, stronger at high)
                const float octaveBlend = fuzzNorm * fuzzNorm * 0.35f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = upsampled.getChannelPointer((size_t)ch);
                    float x = data[sample];
                    const float absX = std::abs(x);

                    // ---- Envelope followers ----
                    // Fast envelope for gate (attack instant, release ~8ms at 192kHz)
                    inputEnvelope = juce::jmax(absX, inputEnvelope * 0.9994f);

                    // Slow envelope for voltage sag (~300ms decay, simulates capacitor drain)
                    sagEnvelope = juce::jmax(absX, sagEnvelope * 0.99998f);

                    // ---- Noise gate with hysteresis ----
                    const float gateThreshOpen  = gateAmount * 0.025f;
                    const float gateThreshClose = gateAmount * 0.012f;  // Lower close for hysteresis

                    if (inputEnvelope > gateThreshOpen)
                        gateOpen = true;
                    else if (inputEnvelope < gateThreshClose)
                        gateOpen = false;

                    float gateGain;
                    if (gateAmount < 0.01f)
                        gateGain = 1.0f;  // Gate off
                    else if (gateOpen)
                        gateGain = 1.0f;
                    else
                        gateGain = inputEnvelope / juce::jmax(gateThreshClose, 0.0001f);

                    gateGain = juce::jlimit(0.0f, 1.0f, gateGain);

                    // ---- Voltage sag (germanium power supply droop) ----
                    float sagDrop = 1.0f - sagEnvelope * fuzzNorm * 0.4f;
                    sagDrop = juce::jmax(0.3f, sagDrop);

                    // ---- Octave-up via full-wave rectification ----
                    float octaveSignal = std::abs(x * drive) * octaveBlend;

                    // ---- Two-stage germanium clipping ----
                    x = x * drive * sagDrop + bias;

                    // Asymmetric exponential clip (germanium characteristic)
                    // Positive: soft forward bias (~0.2V threshold)
                    // Negative: harder saturation, compressed at 78%
                    if (x >= 0.0f)
                        x = 1.0f - std::exp(-x * 1.2f);
                    else
                        x = -(1.0f - std::exp(x * 1.6f)) * 0.78f;

                    // Mix in octave harmonics (clipped separately for cleaner blend)
                    x += std::tanh(octaveSignal * 1.5f) * 0.28f;

                    data[sample] = x * gateGain;
                }
            }
        }

        // Tone filter + DC block
        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            toneFilter.process(ctx);
            dcBlock.process(ctx);
        }

        oversampler.processSamplesDown(block);

        // ---- Dry/wet mix + output level ----
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();
            const float level = juce::jmap(juce::jlimit(0.0f, 1.0f, levelSmooth.getNextValue()), 0.08f, 1.6f);

            // Equal-power mix
            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float clean = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample);
                buffer.setSample(ch, sample, (clean * dryGain + wet * wetGain) * level);
            }
        }

        endBypassProcess(buffer);
    }

    // Public accessors for editor
    juce::AudioParameterFloat* fuzzParam  = nullptr;
    juce::AudioParameterFloat* toneParam  = nullptr;
    juce::AudioParameterFloat* gateParam  = nullptr;
    juce::AudioParameterFloat* mixParam   = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;
    float sagEnvelope = 0.0f;  // Exposed for editor sag indicator

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void updateToneIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.5f;
        if (std::isfinite(cachedTone) && std::abs(cachedTone - tone) <= 1.0e-4f)
            return;

        cachedTone = tone;
        const float cutoff = juce::jmap(tone, 800.0f, 6500.0f);

        *inputFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 60.0f);
        *toneFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentInnerRate, cutoff, 0.65f);
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 18.0f);
    }

    juce::dsp::Oversampling<float> oversampler;
    Filter inputFilter;
    Filter toneFilter;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> fuzzSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    double currentInnerRate = 176400.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    float inputEnvelope = 0.0f;
    bool gateOpen = false;
    bool isPrepared = false;
};

#include "FuzzEditor.h"
