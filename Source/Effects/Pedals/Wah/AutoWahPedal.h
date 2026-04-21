#pragma once

#include "../Base/ProcessorBase.h"
#include "ClassicWahPedal.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

class AutoWahPedal final : public ProcessorBase
{
public:
    AutoWahPedal()
    {
        addParameter(sensitivityParam = new juce::AudioParameterFloat("autoWahSens", "Sensitivity", 0.0f, 1.0f, 0.62f));
        addParameter(attackParam = new juce::AudioParameterFloat("autoWahAttack", "Attack", 0.5f, 30.0f, 2.5f));
        addParameter(releaseParam = new juce::AudioParameterFloat("autoWahRelease", "Release", 15.0f, 900.0f, 180.0f));
        addParameter(rangeParam = new juce::AudioParameterFloat("autoWahRange", "Range", 0.0f, 1.0f, 0.78f));
        addParameter(resonanceParam = new juce::AudioParameterFloat("autoWahResonance", "Resonance", 0.6f, 9.0f, 4.2f));
        addParameter(voiceParam = new juce::AudioParameterFloat("autoWahVoice", "Voice", 0.0f, 1.0f, 0.42f));
        addParameter(mixParam = new juce::AudioParameterFloat("autoWahMix", "Mix", 0.0f, 1.0f, 1.0f));
    }

    const juce::String getName() const override { return "Auto Wah"; }
    double getTailLengthSeconds() const override { return 0.18; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        piOverSr = juce::MathConstants<float>::pi / (float) sr;

        sensitivitySmooth.reset(sr, 0.040);
        attackSmooth.reset(sr, 0.040);
        releaseSmooth.reset(sr, 0.040);
        rangeSmooth.reset(sr, 0.040);
        resonanceSmooth.reset(sr, 0.040);
        voiceSmooth.reset(sr, 0.040);
        mixSmooth.reset(sr, 0.040);

        syncSmoothedTargets(true);

        for (auto& filter : detectorBandPass)
        {
            filter.reset();
            filter.setBandPass(920.0f, 0.82f, sr);
        }

        for (auto& filter : stage1)
            filter.reset();
        for (auto& filter : stage2)
            filter.reset();

        envelope = 0.0f;
        envelopeSmoothed = 0.0f;
        currentFreq = 420.0f;
        currentEnvelope = 0.0f;

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock),
            false,
            false,
            true);

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
        envelope = 0.0f;
        envelopeSmoothed = 0.0f;
        currentFreq = 420.0f;
        currentEnvelope = 0.0f;

        for (auto& filter : detectorBandPass)
            filter.reset();
        for (auto& filter : stage1)
            filter.reset();
        for (auto& filter : stage2)
            filter.reset();

        syncSmoothedTargets(true);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        if (scratchBuffer.getNumChannels() < numChannels
            || scratchBuffer.getNumSamples() < numSamples)
        {
            scratchBuffer.setSize(numChannels, numSamples, false, false, true);
        }

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), numSamples);

        syncSmoothedTargets(false);
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            const float sensitivity = sensitivitySmooth.getNextValue();
            const float attackMs = attackSmooth.getNextValue();
            const float releaseMs = releaseSmooth.getNextValue();
            const float range = rangeSmooth.getNextValue();
            const float resonance = resonanceSmooth.getNextValue();
            const float voice = voiceSmooth.getNextValue();
            const float mix = snapMix(mixSmooth.getNextValue());

            float detector = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float raw = scratchBuffer.getSample(ch, sampleIndex);
                const float focused = detectorBandPass[(size_t) ch].process(raw);
                detector = juce::jmax(detector, std::abs(focused));
            }

            const float attackCoeff = std::exp(-1.0f / (juce::jmax(0.1f, attackMs) * 0.001f * (float) sr));
            const float releaseCoeff = std::exp(-1.0f / (juce::jmax(1.0f, releaseMs) * 0.001f * (float) sr));
            if (detector > envelope)
                envelope = detector + attackCoeff * (envelope - detector);
            else
                envelope = detector + releaseCoeff * (envelope - detector);

            const float smoothCoeff = std::exp(-1.0f / (0.0028f * (float) sr));
            envelopeSmoothed += (1.0f - smoothCoeff) * (envelope - envelopeSmoothed);

            const float detectorDrive = juce::jmap(sensitivity, 5.0f, 17.0f);
            const float envelopeCurve = juce::jmap(voice, 0.88f, 0.60f);
            const float touchSweep = juce::jlimit(0.0f,
                1.0f,
                std::pow(envelopeSmoothed * detectorDrive, envelopeCurve));

            const float minFreq = juce::jmap(voice, 320.0f, 520.0f);
            const float maxFreq = juce::jmap(voice, 1650.0f, 2600.0f) + range * 1900.0f;
            const float freq = juce::jlimit(110.0f,
                (float) (sr * 0.40),
                std::exp(std::log(minFreq) + touchSweep * (std::log(maxFreq) - std::log(minFreq))));

            const float q = juce::jmax(0.60f, resonance * (0.84f + touchSweep * 0.52f));
            const float k = 1.0f / q;
            const float g = std::tan(piOverSr * freq);
            const float bodyBlend = juce::jlimit(0.08f, 0.34f, juce::jmap(voice, 0.28f, 0.12f));
            const float gainComp = 1.0f / (1.0f + juce::jmax(0.0f, resonance - 1.0f) * 0.09f);

            currentFreq = freq;
            currentEnvelope = touchSweep;

            const float dryGain = mix <= 0.0001f ? 1.0f : std::cos(mix * halfPi);
            const float wetGain = mix <= 0.0001f ? 0.0f : (mix >= 0.9999f ? 1.0f : std::sin(mix * halfPi));

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = scratchBuffer.getSample(ch, sampleIndex);
                const float input = buffer.getSample(ch, sampleIndex);
                const float feedbackDrive = stage1[(size_t) ch].ic1eq * (0.06f + resonance * 0.018f);
                const float saturatedInput = Nova::WahDSP::jfetSaturate(input * 1.68f + feedbackDrive) * 0.60f;

                const auto first = stage1[(size_t) ch].process(saturatedInput, g, k);
                const auto second = stage2[(size_t) ch].process(first.bp, g, k * 2.1f);

                const float vocalPeak = first.bp * q * 0.44f;
                const float body = second.lp * (1.15f + (1.0f - touchSweep) * 0.32f);
                float wet = Nova::WahDSP::blend(vocalPeak, body, bodyBlend);
                wet = Nova::WahDSP::blend(wet, first.bp * q * 0.46f, touchSweep * 0.18f + voice * 0.08f);
                wet = Nova::WahDSP::smoothClip(wet * 0.96f) * gainComp * 1.05f;

                buffer.setSample(ch, sampleIndex, dry * dryGain + wet * wetGain);
            }
        }

        endBypassProcess(buffer);
    }

    juce::AudioParameterFloat* sensitivityParam = nullptr;
    juce::AudioParameterFloat* attackParam = nullptr;
    juce::AudioParameterFloat* releaseParam = nullptr;
    juce::AudioParameterFloat* rangeParam = nullptr;
    juce::AudioParameterFloat* resonanceParam = nullptr;
    juce::AudioParameterFloat* voiceParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    float currentFreq = 420.0f;
    float currentEnvelope = 0.0f;

private:
    static float snapMix(float mix) noexcept
    {
        if (mix <= 1.0e-4f)
            return 0.0f;
        if (mix >= 0.9999f)
            return 1.0f;
        return juce::jlimit(0.0f, 1.0f, mix);
    }

    void syncSmoothedTargets(bool snap)
    {
        auto sync = [snap](auto& smoother, juce::AudioParameterFloat* parameter, float fallback)
        {
            const float value = parameter != nullptr ? parameter->get() : fallback;
            if (snap)
                smoother.setCurrentAndTargetValue(value);
            else
                smoother.setTargetValue(value);
        };

        sync(sensitivitySmooth, sensitivityParam, 0.62f);
        sync(attackSmooth, attackParam, 2.5f);
        sync(releaseSmooth, releaseParam, 180.0f);
        sync(rangeSmooth, rangeParam, 0.78f);
        sync(resonanceSmooth, resonanceParam, 4.2f);
        sync(voiceSmooth, voiceParam, 0.42f);

        const float mix = mixParam != nullptr ? mixParam->get() : 1.0f;
        if (snap || mix <= 0.0001f || mix >= 0.9999f)
            mixSmooth.setCurrentAndTargetValue(mix);
        else
            mixSmooth.setTargetValue(mix);
    }

    double sr = 44100.0;
    float piOverSr = juce::MathConstants<float>::pi / 44100.0f;

    std::array<Nova::WahDSP::Biquad, 2> detectorBandPass;
    std::array<Nova::WahDSP::SVF, 2> stage1;
    std::array<Nova::WahDSP::SVF, 2> stage2;
    float envelope = 0.0f;
    float envelopeSmoothed = 0.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sensitivitySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> attackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> releaseSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> resonanceSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> voiceSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    juce::AudioBuffer<float> scratchBuffer;
    bool isPrepared = false;
};

#include "AutoWahEditor.h"
