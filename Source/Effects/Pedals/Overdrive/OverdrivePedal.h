#pragma once

#include "../Base/ProcessorBase.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

class OverdrivePedal final : public ProcessorBase
{
public:
    OverdrivePedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("drive", "Drive", 0.0f, 100.0f, 30.0f));
        addParameter(toneParam = new juce::AudioParameterFloat("tone", "Tone", 0.0f, 1.0f, 0.58f));
        addParameter(textureParam = new juce::AudioParameterFloat("texture", "Texture", 0.0f, 1.0f, 0.42f));
        addParameter(mixParam = new juce::AudioParameterFloat("mix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("level", "Level", 0.0f, 1.0f, 0.74f));
    }

    const juce::String getName() const override { return "Overdrive"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    juce::AudioParameterFloat* getDriveParam() const { return driveParam; }
    juce::AudioParameterFloat* getToneParam() const { return toneParam; }
    juce::AudioParameterFloat* getTextureParam() const { return textureParam; }
    juce::AudioParameterFloat* getMixParam() const { return mixParam; }
    juce::AudioParameterFloat* getLevelParam() const { return levelParam; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.reset();
        oversampler.initProcessing((size_t) juce::jmax(1, samplesPerBlock));

        currentInnerSampleRate = sampleRate * (double) oversamplingFactor();
        const auto numChannels = (size_t) juce::jmax(1, getTotalNumOutputChannels());

        inputTighten.prepare(currentInnerSampleRate, numChannels);
        presenceSplit.prepare(currentInnerSampleRate, numChannels);
        bodyFilter.prepare(currentInnerSampleRate, numChannels);
        outputLowPassA.prepare(currentInnerSampleRate, numChannels);
        outputLowPassB.prepare(currentInnerSampleRate, numChannels);
        dcBlock.prepare(currentInnerSampleRate, numChannels);
        driveStage.prepare(currentInnerSampleRate, numChannels);

        driveControlSmooth.reset(sampleRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        toneControlSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        textureControlSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        mixSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        wetTrimSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        const float drive = driveParam != nullptr ? *driveParam : 30.0f;
        const float tone = toneParam != nullptr ? *toneParam : 0.58f;
        const float texture = textureParam != nullptr ? *textureParam : 0.42f;
        const float mix = mixParam != nullptr ? *mixParam : 1.0f;
        const float level = levelParam != nullptr ? levelFromControl(*levelParam) : 1.0f;

        driveControlSmooth.setCurrentAndTargetValue(drive);
        toneControlSmooth.setCurrentAndTargetValue(tone);
        textureControlSmooth.setCurrentAndTargetValue(texture);
        mixSmooth.setCurrentAndTargetValue(mix);
        levelSmooth.setCurrentAndTargetValue(level);
        wetTrimSmooth.setCurrentAndTargetValue(wetTrimFromControls(drive, texture));

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock),
            false,
            false,
            true);

        updateToneModel(drive, tone, texture);

        setProcessingLatency((int) oversampler.getLatencyInSamples());
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
        presenceSplit.reset();
        bodyFilter.reset();
        outputLowPassA.reset();
        outputLowPassB.reset();
        dcBlock.reset();
        driveStage.reset();

        const float drive = driveParam != nullptr ? *driveParam : 30.0f;
        const float tone = toneParam != nullptr ? *toneParam : 0.58f;
        const float texture = textureParam != nullptr ? *textureParam : 0.42f;

        driveControlSmooth.setCurrentAndTargetValue(drive);
        toneControlSmooth.setCurrentAndTargetValue(tone);
        textureControlSmooth.setCurrentAndTargetValue(texture);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelFromControl(*levelParam) : 1.0f);
        wetTrimSmooth.setCurrentAndTargetValue(wetTrimFromControls(drive, texture));

        updateToneModel(drive, tone, texture);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;

        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        if (scratchBuffer.getNumChannels() < numChannels
            || scratchBuffer.getNumSamples() < numSamples)
        {
            scratchBuffer.setSize(numChannels,
                numSamples,
                false,
                false,
                true);
        }

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), numSamples);

        const float targetDrive = driveParam != nullptr ? *driveParam : 30.0f;
        const float targetTone = toneParam != nullptr ? *toneParam : 0.58f;
        const float targetTexture = textureParam != nullptr ? *textureParam : 0.42f;
        driveControlSmooth.setTargetValue(targetDrive);
        toneControlSmooth.setTargetValue(targetTone);
        textureControlSmooth.setTargetValue(targetTexture);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelFromControl(*levelParam) : 1.0f);
        wetTrimSmooth.setTargetValue(wetTrimFromControls(targetDrive, targetTexture));

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        const int innerSamples = (int) upsampled.getNumSamples();
        const int oversampleRatio = juce::jmax(1, innerSamples / juce::jmax(1, numSamples));

        inputTighten.ensureChannels((size_t) upsampled.getNumChannels());
        presenceSplit.ensureChannels((size_t) upsampled.getNumChannels());
        bodyFilter.ensureChannels((size_t) upsampled.getNumChannels());
        outputLowPassA.ensureChannels((size_t) upsampled.getNumChannels());
        outputLowPassB.ensureChannels((size_t) upsampled.getNumChannels());
        dcBlock.ensureChannels((size_t) upsampled.getNumChannels());
        driveStage.ensureChannels((size_t) upsampled.getNumChannels());

        float currentDrive = driveControlSmooth.getCurrentValue();
        float currentTone = toneControlSmooth.getCurrentValue();
        float currentTexture = textureControlSmooth.getCurrentValue();
        updateToneModel(currentDrive, currentTone, currentTexture);

        for (int sample = 0; sample < innerSamples; ++sample)
        {
            if ((sample % oversampleRatio) == 0)
            {
                currentDrive = driveControlSmooth.getNextValue();
                currentTone = toneControlSmooth.getNextValue();
                currentTexture = textureControlSmooth.getNextValue();
                updateToneModel(currentDrive, currentTone, currentTexture);
            }

            for (int ch = 0; ch < (int) upsampled.getNumChannels(); ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t) ch);

                float x = data[sample];
                x = inputTighten.processHighPass(ch, x);

                const float lowPresence = presenceSplit.processLowPass(ch, x);
                x += (x - lowPresence) * toneModel.presenceAmount;

                x = driveStage.processSample(ch, x, currentDrive, currentTexture);

                const float body = bodyFilter.processLowPass(ch, x);
                x += body * toneModel.bodyAmount;

                x = outputLowPassA.processLowPass(ch, x);
                x = outputLowPassB.processLowPass(ch, x);
                x = dcBlock.processHighPass(ch, x);
                data[sample] = x;
            }
        }

        oversampler.processSamplesDown(block);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float mix = juce::jlimit(0.0f, 1.0f, mixSmooth.getNextValue());
            const float dryGain = std::cos(juce::MathConstants<float>::halfPi * mix);
            const float wetGain = std::sin(juce::MathConstants<float>::halfPi * mix);
            const float wetTrim = wetTrimSmooth.getNextValue();
            const float level = levelSmooth.getNextValue();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float clean = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample) * wetTrim;
                buffer.setSample(ch, sample, ((clean * dryGain) + (wet * wetGain)) * level);
            }
        }

        endBypassProcess(buffer);
    }

private:
    class OnePoleFilterBank
    {
    public:
        void prepare(double newSampleRate, size_t numChannels)
        {
            sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
            states.assign(numChannels, 0.0f);
            setCutoff(1000.0f);
        }

        void ensureChannels(size_t numChannels)
        {
            if (states.size() < numChannels)
                states.resize(numChannels, 0.0f);
        }

        void reset()
        {
            std::fill(states.begin(), states.end(), 0.0f);
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
        std::vector<float> states;
    };

    class ResponsiveDriveStage
    {
    public:
        void prepare(double newSampleRate, size_t numChannels)
        {
            sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
            channelState.assign(numChannels, {});
            reset();
        }

        void ensureChannels(size_t numChannels)
        {
            if (channelState.size() < numChannels)
                channelState.resize(numChannels, {});
        }

        void reset()
        {
            for (auto& state : channelState)
                state = {};
        }

        float processSample(int channel, float input, float driveControl, float textureControl) noexcept
        {
            auto& state = channelState[(size_t) channel];
            const float drive = juce::jlimit(0.0f, 1.0f, driveControl / 100.0f);
            const float texture = juce::jlimit(0.0f, 1.0f, textureControl);

            const float detector = std::abs(input);
            const float envelopeCoeff = detector > state.envelope
                ? 0.18f
                : (0.0025f + drive * 0.0015f);
            state.envelope += (detector - state.envelope) * envelopeCoeff;

            const float inputGain = juce::Decibels::decibelsToGain(5.0f + drive * 22.0f);
            const float sag = 1.0f / (1.0f + state.envelope * (0.18f + texture * 0.40f) * (1.0f + drive * 1.20f));
            float x = input * inputGain * sag;

            const float bias = (0.018f + texture * 0.085f + drive * 0.03f) * std::tanh(x * 0.70f);
            const float soft = std::tanh((x + bias) * (1.10f + texture * 0.85f + drive * 0.25f));
            const float dense = std::atan((x + bias * 0.45f) * (1.30f + texture * 2.10f + drive * 0.55f))
                * (2.0f / juce::MathConstants<float>::pi);

            float y = juce::jmap(texture, soft, (soft * 0.62f) + (dense * 0.38f));
            y = std::tanh(y * (1.02f + drive * 0.14f));

            state.dcOffset = (state.dcOffset * Nova::Config::DC_OFFSET_DECAY) + (y * Nova::Config::DC_OFFSET_ATTACK);
            return y - state.dcOffset;
        }

    private:
        struct ChannelState
        {
            float envelope = 0.0f;
            float dcOffset = 0.0f;
        };

        double sampleRate = 44100.0;
        std::vector<ChannelState> channelState;
    };

    struct ToneModel
    {
        float presenceAmount = 0.0f;
        float bodyAmount = 0.0f;
    };

    static int oversamplingFactor() noexcept
    {
        return 4;
    }

    static float levelFromControl(float control) noexcept
    {
        const float levelDb = juce::jmap(juce::jlimit(0.0f, 1.0f, control), -18.0f, 6.0f);
        return juce::Decibels::decibelsToGain(levelDb);
    }

    static float wetTrimFromControls(float driveControl, float textureControl) noexcept
    {
        const float drive = juce::jlimit(0.0f, 1.0f, driveControl / 100.0f);
        const float texture = juce::jlimit(0.0f, 1.0f, textureControl);
        const float compensationDb = juce::jmap(drive, -0.35f, -4.25f) + juce::jmap(texture, 0.0f, -1.15f);
        return juce::Decibels::decibelsToGain(compensationDb);
    }

    void updateToneModel(float driveControl, float toneControl, float textureControl)
    {
        const float drive = juce::jlimit(0.0f, 1.0f, driveControl / 100.0f);
        const float tone = juce::jlimit(0.0f, 1.0f, toneControl);
        const float texture = juce::jlimit(0.0f, 1.0f, textureControl);

        inputTighten.setCutoff(juce::jmap(drive, 34.0f, 68.0f));
        presenceSplit.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, 0.20f + tone * 0.60f), 900.0f, 2100.0f));
        bodyFilter.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, 0.30f + texture * 0.45f), 165.0f, 320.0f));

        const float topCutControl = juce::jlimit(0.0f, 1.0f, 0.08f + tone * 0.82f - drive * 0.08f);
        const float topCutHz = juce::jmap(topCutControl, 2500.0f, 9500.0f);
        outputLowPassA.setCutoff(topCutHz);
        outputLowPassB.setCutoff(topCutHz);
        dcBlock.setCutoff(18.0f);

        toneModel.presenceAmount = juce::jmap(juce::jlimit(0.0f, 1.0f, tone * 0.78f + texture * 0.12f), -0.08f, 0.42f);
        toneModel.bodyAmount = juce::jmap(juce::jlimit(0.0f, 1.0f, texture * 0.72f + drive * 0.10f), -0.04f, 0.22f);
    }

    juce::dsp::Oversampling<float> oversampler;
    OnePoleFilterBank inputTighten;
    OnePoleFilterBank presenceSplit;
    OnePoleFilterBank bodyFilter;
    OnePoleFilterBank outputLowPassA;
    OnePoleFilterBank outputLowPassB;
    OnePoleFilterBank dcBlock;
    ResponsiveDriveStage driveStage;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveControlSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> toneControlSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> textureControlSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetTrimSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* textureParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentInnerSampleRate = 176400.0;
    ToneModel toneModel;
    bool isPrepared = false;
};

#include "OverdriveEditor.h"
