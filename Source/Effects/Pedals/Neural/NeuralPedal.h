#pragma once

#include "../Base/ProcessorBase.h"

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace Nova::NeuralDSP
{
// P7C: max channels preallocated for OnePoleFilterBank/EnvelopeFollower so the
// realtime path never has to resize. Matches NeuralPedal::kMaxProcessingChannels
// (stereo) plus the 2 channels the oversampler can publish at most.
static constexpr size_t kMaxHelperChannels = 2;

inline float blend(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

inline float denseClip(float x, float drive, float asymmetry) noexcept
{
    const float biased = x + asymmetry;
    const float clipped = std::tanh(biased * drive);
    return clipped - std::tanh(asymmetry * drive);
}

inline float silkyClip(float x, float drive) noexcept
{
    const float pushed = x * drive;
    return std::atan(pushed) * (2.0f / juce::MathConstants<float>::pi);
}

class OnePoleFilterBank
{
public:
    void prepare(double newSampleRate, size_t numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        preparedChannels = juce::jmin(numChannels, kMaxHelperChannels);
        states.fill(0.0f);
        setCutoff(1000.0f);
    }

    bool hasChannels(size_t numChannels) const noexcept
    {
        return numChannels <= preparedChannels;
    }

    void reset()
    {
        states.fill(0.0f);
    }

    void setCutoff(float cutoffHz)
    {
        const double maxCutoff = juce::jmax(20.0, sampleRate * 0.45);
        const double clamped = juce::jlimit(5.0, maxCutoff, (double) cutoffHz);
        coefficient = (float) (1.0 - std::exp((-juce::MathConstants<double>::twoPi * clamped) / sampleRate));
    }

    float processLowPass(int channel, float input) noexcept
    {
        auto& state = states[(size_t) channel];
        state += coefficient * (input - state);
        return state;
    }

    float processHighPass(int channel, float input) noexcept
    {
        return input - processLowPass(channel, input);
    }

private:
    double sampleRate = 44100.0;
    float coefficient = 0.1f;
    size_t preparedChannels = 0;
    std::array<float, kMaxHelperChannels> states {};
};

class EnvelopeFollower
{
public:
    void prepare(double newSampleRate, size_t numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        preparedChannels = juce::jmin(numChannels, kMaxHelperChannels);
        states.fill(0.0f);
    }

    bool hasChannels(size_t numChannels) const noexcept
    {
        return numChannels <= preparedChannels;
    }

    void reset()
    {
        states.fill(0.0f);
    }

    float process(int channel, float input, float attackMs, float releaseMs) noexcept
    {
        const float attack = coefficient(attackMs);
        const float release = coefficient(releaseMs);
        auto& state = states[(size_t) channel];
        const float detector = std::abs(input);
        const float coeff = detector > state ? attack : release;
        state += (detector - state) * coeff;
        return state;
    }

private:
    float coefficient(float ms) const noexcept
    {
        const float seconds = juce::jmax(0.0001f, ms * 0.001f);
        return 1.0f - std::exp(-1.0f / (seconds * (float) sampleRate));
    }

    double sampleRate = 44100.0;
    size_t preparedChannels = 0;
    std::array<float, kMaxHelperChannels> states {};
};

} // namespace Nova::NeuralDSP

class NeuralPedal final : public ProcessorBase
{
public:
    NeuralPedal()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("neuralDrive", "Drive", 0.0f, 100.0f, 54.0f));
        addParameter(focusParam = new juce::AudioParameterFloat("neuralFocus", "Focus", 0.0f, 1.0f, 0.58f));
        addParameter(detailParam = new juce::AudioParameterFloat("neuralDetail", "Detail", 0.0f, 1.0f, 0.54f));
        addParameter(compParam = new juce::AudioParameterFloat("neuralComp", "Comp", 0.0f, 1.0f, 0.42f));
        addParameter(mixParam = new juce::AudioParameterFloat("neuralMix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("neuralLevel", "Level", 0.0f, 1.0f, 0.75f));
    }

    const juce::String getName() const override { return "Neural"; }
    double getTailLengthSeconds() const override { return 0.06; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.reset();
        preparedBlockSize = juce::jmax(1, samplesPerBlock);
        preparedChannels = juce::jlimit(1, kMaxProcessingChannels, juce::jmax(2, getTotalNumOutputChannels()));
        oversampler.initProcessing((size_t) preparedBlockSize);
        innerSampleRate = sampleRate * (double) oversamplingFactor();

        const auto numChannels = (size_t) preparedChannels;
        inputTighten.prepare(innerSampleRate, numChannels);
        focusBody.prepare(innerSampleRate, numChannels);
        toneBody.prepare(innerSampleRate, numChannels);
        airLowPass.prepare(innerSampleRate, numChannels);
        speakerHighPass.prepare(innerSampleRate, numChannels);
        speakerLowPass.prepare(innerSampleRate, numChannels);
        dcBlock.prepare(innerSampleRate, numChannels);
        envelopeFollower.prepare(innerSampleRate, numChannels);

        driveSmooth.reset(sampleRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        focusSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        detailSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        compSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        mixSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        wetTrimSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        const float drive = driveParam != nullptr ? driveParam->get() : 54.0f;
        const float focus = focusParam != nullptr ? focusParam->get() : 0.58f;
        const float detail = detailParam != nullptr ? detailParam->get() : 0.54f;
        const float comp = compParam != nullptr ? compParam->get() : 0.42f;
        const float mix = mixParam != nullptr ? mixParam->get() : 1.0f;
        const float level = levelParam != nullptr ? levelFromControl(levelParam->get()) : 1.0f;

        driveSmooth.setCurrentAndTargetValue(drive);
        focusSmooth.setCurrentAndTargetValue(focus);
        detailSmooth.setCurrentAndTargetValue(detail);
        compSmooth.setCurrentAndTargetValue(comp);
        mixSmooth.setCurrentAndTargetValue(mix);
        levelSmooth.setCurrentAndTargetValue(level);
        wetTrimSmooth.setCurrentAndTargetValue(wetTrimFromControls(drive, focus, detail, comp));

        scratchBuffer.setSize(preparedChannels,
            preparedBlockSize,
            false,
            false,
            true);

        updateToneModel(drive, focus, detail, comp);
        setProcessingLatency((int) std::round(oversampler.getLatencyInSamples()));
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
        oversampler.reset();
        inputTighten.reset();
        focusBody.reset();
        toneBody.reset();
        airLowPass.reset();
        speakerHighPass.reset();
        speakerLowPass.reset();
        dcBlock.reset();
        envelopeFollower.reset();

        const float drive = driveParam != nullptr ? driveParam->get() : 54.0f;
        const float focus = focusParam != nullptr ? focusParam->get() : 0.58f;
        const float detail = detailParam != nullptr ? detailParam->get() : 0.54f;
        const float comp = compParam != nullptr ? compParam->get() : 0.42f;

        driveSmooth.setCurrentAndTargetValue(drive);
        focusSmooth.setCurrentAndTargetValue(focus);
        detailSmooth.setCurrentAndTargetValue(detail);
        compSmooth.setCurrentAndTargetValue(comp);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelFromControl(levelParam->get()) : 1.0f);
        wetTrimSmooth.setCurrentAndTargetValue(wetTrimFromControls(drive, focus, detail, comp));

        updateToneModel(drive, focus, detail, comp);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;

        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        scrubInvalidSamples(buffer);

        if (!canProcessBlock(buffer))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            endBypassProcess(buffer);
            return;
        }

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), numSamples);

        const float targetDrive = driveParam != nullptr ? driveParam->get() : 54.0f;
        const float targetFocus = focusParam != nullptr ? focusParam->get() : 0.58f;
        const float targetDetail = detailParam != nullptr ? detailParam->get() : 0.54f;
        const float targetComp = compParam != nullptr ? compParam->get() : 0.42f;
        const float targetMix = snapMixTarget(mixParam != nullptr ? mixParam->get() : 1.0f);
        driveSmooth.setTargetValue(targetDrive);
        focusSmooth.setTargetValue(targetFocus);
        detailSmooth.setTargetValue(targetDetail);
        compSmooth.setTargetValue(targetComp);
        if (targetMix <= 0.0001f || targetMix >= 0.9999f)
            mixSmooth.setCurrentAndTargetValue(targetMix);
        else
            mixSmooth.setTargetValue(targetMix);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelFromControl(levelParam->get()) : 1.0f);
        wetTrimSmooth.setTargetValue(wetTrimFromControls(targetDrive, targetFocus, targetDetail, targetComp));

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        const int innerSamples = (int) upsampled.getNumSamples();
        const int oversampleRatio = juce::jmax(1, innerSamples / juce::jmax(1, numSamples));

        const auto upsampledChannels = (size_t) upsampled.getNumChannels();
        if (!inputTighten.hasChannels(upsampledChannels)
            || !focusBody.hasChannels(upsampledChannels)
            || !toneBody.hasChannels(upsampledChannels)
            || !airLowPass.hasChannels(upsampledChannels)
            || !speakerHighPass.hasChannels(upsampledChannels)
            || !speakerLowPass.hasChannels(upsampledChannels)
            || !dcBlock.hasChannels(upsampledChannels)
            || !envelopeFollower.hasChannels(upsampledChannels))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            oversampler.processSamplesDown(block);
            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::copy(buffer.getWritePointer(ch), scratchBuffer.getReadPointer(ch), numSamples);
            endBypassProcess(buffer);
            return;
        }

        float currentDrive = driveSmooth.getCurrentValue();
        float currentFocus = focusSmooth.getCurrentValue();
        float currentDetail = detailSmooth.getCurrentValue();
        float currentComp = compSmooth.getCurrentValue();
        updateToneModel(currentDrive, currentFocus, currentDetail, currentComp);

        for (int sample = 0; sample < innerSamples; ++sample)
        {
            if ((sample % oversampleRatio) == 0)
            {
                currentDrive = driveSmooth.getNextValue();
                currentFocus = focusSmooth.getNextValue();
                currentDetail = detailSmooth.getNextValue();
                currentComp = compSmooth.getNextValue();
                updateToneModel(currentDrive, currentFocus, currentDetail, currentComp);
            }

            for (int ch = 0; ch < (int) upsampled.getNumChannels(); ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t) ch);

                float x = data[sample];
                x = inputTighten.processHighPass(ch, x);

                const float envelope = envelopeFollower.process(ch, x, 0.55f + currentComp * 4.0f, 22.0f + currentComp * 90.0f);
                const float focusLow = focusBody.processLowPass(ch, x);
                x += (x - focusLow) * toneModel.focusEdge;
                x -= focusLow * toneModel.focusLowTrim;

                const float inputDrive = juce::Decibels::decibelsToGain(7.0f + juce::jlimit(0.0f, 1.0f, currentDrive * 0.01f) * 22.0f);
                const float compPush = 1.0f + envelope * (0.24f + currentComp * 0.72f);
                const float dynamicSag = 1.0f / (1.0f + envelope * (0.14f + currentComp * 0.58f));
                const float asymmetry = toneModel.asymmetry + envelope * (0.015f + currentDetail * 0.045f);
                const float dense = Nova::NeuralDSP::denseClip(x * inputDrive * dynamicSag, toneModel.stageDriveA * compPush, asymmetry);
                const float silky = Nova::NeuralDSP::silkyClip((x + dense * toneModel.feedbackBlend) * toneModel.stageDriveB,
                    1.0f + currentDetail * 1.6f + currentComp * 0.55f);

                float shaped = Nova::NeuralDSP::blend(dense, silky, toneModel.voiceBlend);
                const float lowBody = toneBody.processLowPass(ch, shaped);
                shaped += lowBody * toneModel.bodyLift;
                const float airBase = airLowPass.processLowPass(ch, shaped);
                shaped = airBase + (shaped - airBase) * toneModel.airLift;
                const float speakerShaped = speakerLowPass.processLowPass(ch,
                    speakerHighPass.processHighPass(ch, shaped));
                shaped = Nova::NeuralDSP::blend(shaped, speakerShaped, toneModel.speakerBlend);
                shaped = dcBlock.processHighPass(ch, shaped);
                data[sample] = shaped * toneModel.outputTrim;
            }
        }

        oversampler.processSamplesDown(block);

        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float mix = snapMixTarget(mixSmooth.getNextValue());
            const float level = levelSmooth.getNextValue();
            const float wetTrim = wetTrimSmooth.getNextValue();
            const float dryGain = mix <= 0.0001f ? 1.0f : std::cos(halfPi * mix);
            const float wetGain = mix <= 0.0001f ? 0.0f : (mix >= 0.9999f ? 1.0f : std::sin(halfPi * mix));

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample) * wetTrim;
                buffer.setSample(ch, sample, ((dry * dryGain) + (wet * wetGain)) * level);
            }
        }

        endBypassProcess(buffer);
    }

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* focusParam = nullptr;
    juce::AudioParameterFloat* detailParam = nullptr;
    juce::AudioParameterFloat* compParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    int getRealtimeFallbackCount() const noexcept
    {
        return fallbackBlockCount.load(std::memory_order_relaxed);
    }

    void clearRealtimeFallbackCount() noexcept
    {
        fallbackBlockCount.store(0, std::memory_order_relaxed);
    }

    static float computeDisplayCurve(float input, float drive, float detail, float comp)
    {
        const float driveNorm = juce::jlimit(0.0f, 1.0f, drive * 0.01f);
        const float dense = Nova::NeuralDSP::denseClip(input, 1.2f + driveNorm * 4.0f + comp * 0.8f, (detail - 0.5f) * 0.15f);
        const float silky = Nova::NeuralDSP::silkyClip(input + dense * 0.16f, 1.0f + detail * 2.2f + driveNorm * 1.0f);
        return juce::jlimit(-1.2f, 1.2f, Nova::NeuralDSP::blend(dense, silky, 0.34f + detail * 0.44f + comp * 0.10f));
    }

private:
    static constexpr int kMaxProcessingChannels = 2;

    struct ToneModel
    {
        float stageDriveA = 1.0f;
        float stageDriveB = 1.0f;
        float feedbackBlend = 0.0f;
        float focusEdge = 0.0f;
        float focusLowTrim = 0.0f;
        float bodyLift = 0.0f;
        float airLift = 0.0f;
        float speakerBlend = 0.0f;
        float asymmetry = 0.0f;
        float voiceBlend = 0.5f;
        float outputTrim = 1.0f;
    };

    static int oversamplingFactor() noexcept
    {
        return 8;
    }

    static float snapMixTarget(float mix) noexcept
    {
        if (mix <= 1.0e-4f)
            return 0.0f;
        if (mix >= 0.9999f)
            return 1.0f;
        return juce::jlimit(0.0f, 1.0f, mix);
    }

    static float levelFromControl(float control) noexcept
    {
        const float levelDb = juce::jmap(juce::jlimit(0.0f, 1.0f, control), -18.0f, 6.0f);
        return juce::Decibels::decibelsToGain(levelDb);
    }

    static float wetTrimFromControls(float driveControl, float focusControl, float detailControl, float compControl) noexcept
    {
        const float drive = juce::jlimit(0.0f, 1.0f, driveControl * 0.01f);
        const float focus = juce::jlimit(0.0f, 1.0f, focusControl);
        const float detail = juce::jlimit(0.0f, 1.0f, detailControl);
        const float comp = juce::jlimit(0.0f, 1.0f, compControl);
        const float compensationDb = juce::jmap(drive, -0.55f, -5.20f)
            + juce::jmap(detail, 0.0f, -1.05f)
            + juce::jmap(comp, 0.0f, -1.35f)
            + juce::jmap(focus, 0.15f, -1.05f);
        return juce::Decibels::decibelsToGain(compensationDb);
    }

    bool canProcessBlock(const juce::AudioBuffer<float>& buffer) const noexcept
    {
        return buffer.getNumSamples() <= preparedBlockSize
            && buffer.getNumChannels() <= preparedChannels
            && buffer.getNumChannels() <= kMaxProcessingChannels;
    }

    static void scrubInvalidSamples(juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (!std::isfinite(data[i]))
                    data[i] = 0.0f;
        }
    }

    void updateToneModel(float driveControl, float focusControl, float detailControl, float compControl)
    {
        const float drive = juce::jlimit(0.0f, 1.0f, driveControl * 0.01f);
        const float focus = juce::jlimit(0.0f, 1.0f, focusControl);
        const float detail = juce::jlimit(0.0f, 1.0f, detailControl);
        const float comp = juce::jlimit(0.0f, 1.0f, compControl);

        inputTighten.setCutoff(juce::jmap(focus, 34.0f, 240.0f));
        focusBody.setCutoff(juce::jmap(focus, 95.0f, 340.0f));
        toneBody.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, 0.22f + detail * 0.46f), 135.0f, 340.0f));
        airLowPass.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, 0.28f + detail * 0.58f - drive * 0.12f), 3500.0f, 12500.0f));
        speakerHighPass.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, focus * 0.72f + comp * 0.18f), 52.0f, 145.0f));
        speakerLowPass.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, 0.24f + detail * 0.48f - drive * 0.10f + focus * 0.08f), 3000.0f, 7600.0f));
        dcBlock.setCutoff(18.0f);

        toneModel.stageDriveA = 1.12f + drive * 3.8f + comp * 0.55f;
        toneModel.stageDriveB = 1.04f + detail * 2.7f + drive * 0.88f;
        toneModel.feedbackBlend = 0.08f + detail * 0.20f + comp * 0.08f;
        toneModel.focusEdge = juce::jmap(focus, 0.04f, 0.70f);
        toneModel.focusLowTrim = juce::jmap(focus, 0.0f, 0.46f);
        toneModel.bodyLift = juce::jmap(juce::jlimit(0.0f, 1.0f, detail * 0.56f + comp * 0.16f - focus * 0.22f), -0.04f, 0.18f);
        toneModel.airLift = juce::jmap(detail, 0.42f, 0.98f);
        toneModel.speakerBlend = juce::jlimit(0.12f, 0.86f, 0.26f + drive * 0.18f + comp * 0.20f - detail * 0.08f);
        toneModel.asymmetry = (detail - 0.5f) * 0.12f + comp * 0.02f;
        toneModel.voiceBlend = juce::jlimit(0.18f, 0.92f, 0.32f + detail * 0.46f + comp * 0.12f);
        toneModel.outputTrim = juce::Decibels::decibelsToGain(juce::jmap(drive, -0.25f, -2.10f));
    }

    juce::dsp::Oversampling<float> oversampler;
    Nova::NeuralDSP::OnePoleFilterBank inputTighten;
    Nova::NeuralDSP::OnePoleFilterBank focusBody;
    Nova::NeuralDSP::OnePoleFilterBank toneBody;
    Nova::NeuralDSP::OnePoleFilterBank airLowPass;
    Nova::NeuralDSP::OnePoleFilterBank speakerHighPass;
    Nova::NeuralDSP::OnePoleFilterBank speakerLowPass;
    Nova::NeuralDSP::OnePoleFilterBank dcBlock;
    Nova::NeuralDSP::EnvelopeFollower envelopeFollower;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> focusSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> detailSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> compSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetTrimSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    double innerSampleRate = 176400.0;
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    ToneModel toneModel;
    std::atomic<int> fallbackBlockCount{ 0 };
    bool isPrepared = false;
};

#include "NeuralEditor.h"
