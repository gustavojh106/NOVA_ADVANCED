#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

namespace Nova::TremoloDSP
{
constexpr float kDefaultCrossoverHz = 800.0f;
constexpr int kCoefficientUpdateInterval = 12;

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept
    {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    void setIdentity() noexcept
    {
        b0 = 1.0f;
        b1 = 0.0f;
        b2 = 0.0f;
        a1 = 0.0f;
        a2 = 0.0f;
    }

    void setLowPass(float frequencyHz, float q, double sampleRate)
    {
        const float w0 = juce::MathConstants<float>::twoPi
            * juce::jlimit(20.0f, (float) (sampleRate * 0.45), frequencyHz)
            / (float) sampleRate;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * juce::jmax(0.05f, q));
        const float invA0 = 1.0f / (1.0f + alpha);

        b0 = 0.5f * (1.0f - cosW0) * invA0;
        b1 = (1.0f - cosW0) * invA0;
        b2 = b0;
        a1 = -2.0f * cosW0 * invA0;
        a2 = (1.0f - alpha) * invA0;
    }

    void setHighPass(float frequencyHz, float q, double sampleRate)
    {
        const float w0 = juce::MathConstants<float>::twoPi
            * juce::jlimit(20.0f, (float) (sampleRate * 0.45), frequencyHz)
            / (float) sampleRate;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * juce::jmax(0.05f, q));
        const float invA0 = 1.0f / (1.0f + alpha);

        b0 = 0.5f * (1.0f + cosW0) * invA0;
        b1 = -(1.0f + cosW0) * invA0;
        b2 = b0;
        a1 = -2.0f * cosW0 * invA0;
        a2 = (1.0f - alpha) * invA0;
    }

    float process(float input) noexcept
    {
        const float output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;
        return output;
    }
};

inline float warpPhase(float phase, float bias) noexcept
{
    phase -= std::floor(phase);

    const float pivot = juce::jlimit(0.08f, 0.92f, 0.5f + (bias - 0.5f) * 0.76f);
    if (phase < pivot)
        return 0.5f * phase / juce::jmax(pivot, 1.0e-4f);

    return 0.5f + 0.5f * (phase - pivot) / juce::jmax(1.0f - pivot, 1.0e-4f);
}

inline float triangleFromPhase(float phase) noexcept
{
    const float wrapped = phase - std::floor(phase);
    return 2.0f * std::abs(2.0f * wrapped - 1.0f) - 1.0f;
}

inline float shapedLfo(float phase, float shape, float bias) noexcept
{
    const float warped = warpPhase(phase, bias);
    const float sine = std::sin(juce::MathConstants<float>::twoPi * warped);
    const float triangle = triangleFromPhase(warped);
    const float softSquare = std::tanh(sine * 4.5f);

    if (shape <= 0.5f)
    {
        const float morph = shape * 2.0f;
        return sine * (1.0f - morph) + triangle * morph;
    }

    const float morph = (shape - 0.5f) * 2.0f;
    return triangle * (1.0f - morph) + softSquare * morph;
}

inline float modulationGain(float lfoValue, float depth) noexcept
{
    const float unipolar = 0.5f * (lfoValue + 1.0f);
    return juce::jlimit(0.0f, 1.25f, (1.0f - depth) + depth * unipolar);
}

inline float equalPowerDry(float mix) noexcept
{
    return std::cos(juce::MathConstants<float>::halfPi * juce::jlimit(0.0f, 1.0f, mix));
}

inline float equalPowerWet(float mix) noexcept
{
    return std::sin(juce::MathConstants<float>::halfPi * juce::jlimit(0.0f, 1.0f, mix));
}
} // namespace Nova::TremoloDSP

class TremoloPedal final : public ProcessorBase
{
public:
    TremoloPedal()
    {
        addParameter(rateParam      = new juce::AudioParameterFloat("tremoloRate",      "Rate",      0.5f,    15.0f,   4.5f));
        addParameter(depthParam     = new juce::AudioParameterFloat("tremoloDepth",     "Depth",     0.0f,     1.0f,   0.65f));
        addParameter(shapeParam     = new juce::AudioParameterFloat("tremoloShape",     "Shape",     0.0f,     1.0f,   0.30f));
        addParameter(biasParam      = new juce::AudioParameterFloat("tremoloBias",      "Bias",      0.0f,     1.0f,   0.50f));
        addParameter(stereoParam    = new juce::AudioParameterFloat("tremoloStereo",    "Stereo",    0.0f,     1.0f,   0.0f));
        addParameter(harmonicParam  = new juce::AudioParameterFloat("tremoloHarmonic",  "Harmonic",  0.0f,     1.0f,   0.0f));
        addParameter(crossoverParam = new juce::AudioParameterFloat("tremoloCrossover", "Crossover", 250.0f, 2200.0f, 800.0f));
        addParameter(mixParam       = new juce::AudioParameterFloat("tremoloMix",       "Mix",       0.0f,     1.0f,   1.0f));
        addParameter(levelParam     = new juce::AudioParameterFloat("tremoloLevel",     "Level",     0.5f,     2.0f,   1.0f));
    }

    const juce::String getName() const override { return "Tremolo"; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        rateSmooth.reset(sr, 0.05);
        depthSmooth.reset(sr, 0.04);
        shapeSmooth.reset(sr, 0.04);
        biasSmooth.reset(sr, 0.05);
        stereoSmooth.reset(sr, 0.05);
        harmonicSmooth.reset(sr, 0.05);
        crossoverSmooth.reset(sr, 0.06);
        mixSmooth.reset(sr, 0.03);
        levelSmooth.reset(sr, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? rateParam->get() : 4.5f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? depthParam->get() : 0.65f);
        shapeSmooth.setCurrentAndTargetValue(shapeParam != nullptr ? shapeParam->get() : 0.30f);
        biasSmooth.setCurrentAndTargetValue(biasParam != nullptr ? biasParam->get() : 0.50f);
        stereoSmooth.setCurrentAndTargetValue(stereoParam != nullptr ? stereoParam->get() : 0.0f);
        harmonicSmooth.setCurrentAndTargetValue(harmonicParam != nullptr ? harmonicParam->get() : 0.0f);
        crossoverSmooth.setCurrentAndTargetValue(crossoverParam != nullptr ? crossoverParam->get() : Nova::TremoloDSP::kDefaultCrossoverHz);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 1.0f);

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override
    {
        isPrepared = false;
    }

    void reset() override
    {
        lfoPhase = 0.0f;
        visualPhase.store(0.0f, std::memory_order_relaxed);

        for (auto& filter : xoverLP1)
            filter.reset();
        for (auto& filter : xoverLP2)
            filter.reset();
        for (auto& filter : xoverHP1)
            filter.reset();
        for (auto& filter : xoverHP2)
            filter.reset();

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? rateParam->get() : 4.5f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? depthParam->get() : 0.65f);
        shapeSmooth.setCurrentAndTargetValue(shapeParam != nullptr ? shapeParam->get() : 0.30f);
        biasSmooth.setCurrentAndTargetValue(biasParam != nullptr ? biasParam->get() : 0.50f);
        stereoSmooth.setCurrentAndTargetValue(stereoParam != nullptr ? stereoParam->get() : 0.0f);
        harmonicSmooth.setCurrentAndTargetValue(harmonicParam != nullptr ? harmonicParam->get() : 0.0f);
        crossoverSmooth.setCurrentAndTargetValue(crossoverParam != nullptr ? crossoverParam->get() : Nova::TremoloDSP::kDefaultCrossoverHz);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 1.0f);

        currentCrossoverHz = crossoverSmooth.getCurrentValue();
        updateCrossoverFilters(currentCrossoverHz);
        coefficientCounter = Nova::TremoloDSP::kCoefficientUpdateInterval;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        const float rateTarget = rateParam != nullptr ? rateParam->get() : 4.5f;
        const float depthTarget = depthParam != nullptr ? depthParam->get() : 0.65f;
        const float shapeTarget = shapeParam != nullptr ? shapeParam->get() : 0.30f;
        const float biasTarget = biasParam != nullptr ? biasParam->get() : 0.50f;
        const float stereoTarget = stereoParam != nullptr ? stereoParam->get() : 0.0f;
        const float harmonicTarget = harmonicParam != nullptr ? harmonicParam->get() : 0.0f;
        const float crossoverTarget = crossoverParam != nullptr ? crossoverParam->get() : Nova::TremoloDSP::kDefaultCrossoverHz;
        const float mixTarget = mixParam != nullptr ? mixParam->get() : 1.0f;
        const float levelTarget = levelParam != nullptr ? levelParam->get() : 1.0f;

        rateSmooth.setTargetValue(rateTarget);
        depthSmooth.setTargetValue(depthTarget);
        shapeSmooth.setTargetValue(shapeTarget);
        biasSmooth.setTargetValue(biasTarget);
        stereoSmooth.setTargetValue(stereoTarget);
        harmonicSmooth.setTargetValue(harmonicTarget);
        crossoverSmooth.setTargetValue(crossoverTarget);
        mixSmooth.setTargetValue(mixTarget);
        levelSmooth.setTargetValue(levelTarget);

        if (mixTarget <= 0.0001f || mixTarget >= 0.9999f)
            mixSmooth.setCurrentAndTargetValue(mixTarget <= 0.0001f ? 0.0f : 1.0f);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            const float rateHz = rateSmooth.getNextValue();
            const float depth = depthSmooth.getNextValue();
            const float shape = shapeSmooth.getNextValue();
            const float bias = biasSmooth.getNextValue();
            const float stereo = stereoSmooth.getNextValue();
            const float harmonic = harmonicSmooth.getNextValue();
            const float crossoverHz = crossoverSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();
            const float level = levelSmooth.getNextValue();

            if (--coefficientCounter <= 0 || std::abs(crossoverHz - currentCrossoverHz) > 1.0f)
            {
                currentCrossoverHz = crossoverHz;
                updateCrossoverFilters(currentCrossoverHz);
                coefficientCounter = Nova::TremoloDSP::kCoefficientUpdateInterval;
            }

            const float phaseLeft = lfoPhase;
            const float phaseRight = std::fmod(lfoPhase + 0.5f * stereo, 1.0f);
            const float lfoLeft = Nova::TremoloDSP::shapedLfo(phaseLeft, shape, bias);
            const float lfoRight = Nova::TremoloDSP::shapedLfo(phaseRight, shape, bias);

            const float standardGainLeft = Nova::TremoloDSP::modulationGain(lfoLeft, depth);
            const float standardGainRight = Nova::TremoloDSP::modulationGain(lfoRight, depth);
            const float dryGain = Nova::TremoloDSP::equalPowerDry(mix);
            const float wetGain = Nova::TremoloDSP::equalPowerWet(mix);
            const float compensation = 1.0f + wetGain * depth * (0.10f + 0.06f * harmonic + 0.04f * std::abs(shape - 0.5f));

            for (int channel = 0; channel < numChannels; ++channel)
            {
                const float input = buffer.getSample(channel, sampleIndex);
                const float lfo = channel == 0 ? lfoLeft : lfoRight;
                const float standardGain = channel == 0 ? standardGainLeft : standardGainRight;

                float wet = input * standardGain;

                if (harmonic > 0.001f)
                {
                    const float lowBand = xoverLP2[(size_t) channel].process(xoverLP1[(size_t) channel].process(input));
                    const float highBand = xoverHP2[(size_t) channel].process(xoverHP1[(size_t) channel].process(input));
                    const float lowGain = Nova::TremoloDSP::modulationGain(lfo, depth);
                    const float highGain = Nova::TremoloDSP::modulationGain(-lfo, depth);
                    const float harmonicWet = lowBand * lowGain + highBand * highGain;

                    wet = juce::jmap(harmonic, wet, harmonicWet);
                }

                const float output = input * dryGain * level + wet * wetGain * level * compensation;
                buffer.setSample(channel, sampleIndex, output);
            }

            lfoPhase += rateHz / (float) sr;
            if (lfoPhase >= 1.0f)
                lfoPhase -= std::floor(lfoPhase);

        }

        visualPhase.store(lfoPhase, std::memory_order_relaxed);
        endBypassProcess(buffer);
    }

    juce::AudioParameterFloat* rateParam = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* shapeParam = nullptr;
    juce::AudioParameterFloat* biasParam = nullptr;
    juce::AudioParameterFloat* stereoParam = nullptr;
    juce::AudioParameterFloat* harmonicParam = nullptr;
    juce::AudioParameterFloat* crossoverParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;
    std::atomic<float> visualPhase { 0.0f };

private:
    void updateCrossoverFilters(float crossoverHz)
    {
        const float clamped = juce::jlimit(250.0f, (float) (sr * 0.42), crossoverHz);
        for (auto& filter : xoverLP1)
            filter.setLowPass(clamped, 0.7071f, sr);
        for (auto& filter : xoverLP2)
            filter.setLowPass(clamped, 0.7071f, sr);
        for (auto& filter : xoverHP1)
            filter.setHighPass(clamped, 0.7071f, sr);
        for (auto& filter : xoverHP2)
            filter.setHighPass(clamped, 0.7071f, sr);
    }

    double sr = 44100.0;
    float lfoPhase = 0.0f;
    float currentCrossoverHz = Nova::TremoloDSP::kDefaultCrossoverHz;
    int coefficientCounter = Nova::TremoloDSP::kCoefficientUpdateInterval;

    std::array<Nova::TremoloDSP::Biquad, 2> xoverLP1, xoverLP2, xoverHP1, xoverHP2;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> shapeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> biasSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stereoSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> harmonicSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> crossoverSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> levelSmooth;

    bool isPrepared = false;
};

#include "TremoloEditor.h"
