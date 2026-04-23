#pragma once

#include "../Base/ProcessorBase.h"
#include "../../../Core/PedalSignalTelemetry.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

class OverdrivePedal final : public ProcessorBase
{
public:
    OverdrivePedal()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
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
        preShapeFilter.prepare(currentInnerSampleRate, numChannels);
        bodyFilter.prepare(currentInnerSampleRate, numChannels);
        outputLowPassA.prepare(currentInnerSampleRate, numChannels);
        outputLowPassB.prepare(currentInnerSampleRate, numChannels);
        airRestoreFilter.prepare(currentInnerSampleRate, numChannels);
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
        signalTelemetry.reset();
        debugTelemetry.resetWindow();
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
        preShapeFilter.reset();
        bodyFilter.reset();
        outputLowPassA.reset();
        outputLowPassB.reset();
        airRestoreFilter.reset();
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
        signalTelemetry.reset();
        debugTelemetry.resetWindow();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;
        signalTelemetry.captureInput(buffer);

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
        const float targetMix = snapMixTarget(mixParam != nullptr ? *mixParam : 1.0f);
        if (targetMix <= 0.0001f || targetMix >= 0.9999f)
            mixSmooth.setCurrentAndTargetValue(targetMix);
        else
            mixSmooth.setTargetValue(targetMix);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelFromControl(*levelParam) : 1.0f);
        wetTrimSmooth.setTargetValue(wetTrimFromControls(targetDrive, targetTexture));

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        const int innerSamples = (int) upsampled.getNumSamples();
        const int oversampleRatio = juce::jmax(1, innerSamples / juce::jmax(1, numSamples));

        inputTighten.ensureChannels((size_t) upsampled.getNumChannels());
        presenceSplit.ensureChannels((size_t) upsampled.getNumChannels());
        preShapeFilter.ensureChannels((size_t) upsampled.getNumChannels());
        bodyFilter.ensureChannels((size_t) upsampled.getNumChannels());
        outputLowPassA.ensureChannels((size_t) upsampled.getNumChannels());
        outputLowPassB.ensureChannels((size_t) upsampled.getNumChannels());
        airRestoreFilter.ensureChannels((size_t) upsampled.getNumChannels());
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
                const float preShaped = preShapeFilter.processLowPass(ch, x);
                x = juce::jmap(toneModel.preShapeBlend, x, preShaped);
                debugTelemetry.capturePreDrive(x);

                x = driveStage.processSample(ch, x, currentDrive, currentTexture);
                debugTelemetry.captureDriveStage(x);

                const float body = bodyFilter.processLowPass(ch, x);
                x += body * toneModel.bodyAmount;

                x = outputLowPassA.processLowPass(ch, x);
                x = outputLowPassB.processLowPass(ch, x);
                x += airRestoreFilter.processHighPass(ch, x) * toneModel.airRestoreAmount;
                x = dcBlock.processHighPass(ch, x);
                debugTelemetry.capturePostFilter(x);
                data[sample] = x;
            }
        }

        oversampler.processSamplesDown(block);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float mix = snapMixTarget(mixSmooth.getNextValue());
            const float wetTrim = wetTrimSmooth.getNextValue();
            const float level = levelSmooth.getNextValue();
            debugTelemetry.captureControlWindow(wetTrim, level);

            if (mix <= 0.0001f)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.setSample(ch, sample, scratchBuffer.getSample(ch, sample));
                continue;
            }

            const float dryGain = std::cos(juce::MathConstants<float>::halfPi * mix);
            const float wetGain = mix >= 0.9999f ? 1.0f : std::sin(juce::MathConstants<float>::halfPi * mix);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float clean = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample) * wetTrim;
                debugTelemetry.captureWetMix(wet);
                buffer.setSample(ch, sample, ((clean * dryGain) + (wet * wetGain)) * level);
            }
        }

        signalTelemetry.captureOutputAndEmitIfNeeded(buffer,
            [this]()
            {
                return debugTelemetry.buildReport(*this);
            },
            [this]()
            {
                debugTelemetry.resetWindow();
            });

        endBypassProcess(buffer);
    }

private:
    struct DebugTelemetry
    {
        float preDrivePeak = 0.0f;
        float driveStagePeak = 0.0f;
        float postFilterPeak = 0.0f;
        float wetMixPeak = 0.0f;
        float wetTrimMin = 1.0e9f;
        float wetTrimMax = 0.0f;
        float levelMin = 1.0e9f;
        float levelMax = 0.0f;

        void resetWindow() noexcept
        {
            preDrivePeak = 0.0f;
            driveStagePeak = 0.0f;
            postFilterPeak = 0.0f;
            wetMixPeak = 0.0f;
            wetTrimMin = 1.0e9f;
            wetTrimMax = 0.0f;
            levelMin = 1.0e9f;
            levelMax = 0.0f;
        }

        void capturePreDrive(float value) noexcept
        {
            preDrivePeak = juce::jmax(preDrivePeak, std::abs(value));
        }

        void captureDriveStage(float value) noexcept
        {
            driveStagePeak = juce::jmax(driveStagePeak, std::abs(value));
        }

        void capturePostFilter(float value) noexcept
        {
            postFilterPeak = juce::jmax(postFilterPeak, std::abs(value));
        }

        void captureWetMix(float value) noexcept
        {
            wetMixPeak = juce::jmax(wetMixPeak, std::abs(value));
        }

        void captureControlWindow(float wetTrim, float level) noexcept
        {
            wetTrimMin = juce::jmin(wetTrimMin, wetTrim);
            wetTrimMax = juce::jmax(wetTrimMax, wetTrim);
            levelMin = juce::jmin(levelMin, level);
            levelMax = juce::jmax(levelMax, level);
        }

        juce::String buildReport(const OverdrivePedal& pedal) const
        {
            juce::String report;
            report << "params: drive=" << NovaDiagnostics::formatTelemetryScalar(pedal.driveParam != nullptr ? pedal.driveParam->get() : 30.0f)
                   << ", tone=" << NovaDiagnostics::formatTelemetryScalar(pedal.toneParam != nullptr ? pedal.toneParam->get() : 0.58f)
                   << ", texture=" << NovaDiagnostics::formatTelemetryScalar(pedal.textureParam != nullptr ? pedal.textureParam->get() : 0.42f)
                   << ", mix=" << NovaDiagnostics::formatTelemetryScalar(pedal.mixParam != nullptr ? pedal.mixParam->get() : 1.0f)
                   << ", level=" << NovaDiagnostics::formatTelemetryScalar(pedal.levelParam != nullptr ? pedal.levelParam->get() : 0.74f)
                   << ", innerSampleRate=" << NovaDiagnostics::formatTelemetryScalar((float) pedal.currentInnerSampleRate)
                   << ", oversamplingFactor=" << OverdrivePedal::oversamplingFactor()
                   << juce::newLine
                   << "stages: preDrivePeak=" << NovaDiagnostics::formatTelemetryScalar(preDrivePeak)
                   << ", driveStagePeak=" << NovaDiagnostics::formatTelemetryScalar(driveStagePeak)
                   << ", postFilterPeak=" << NovaDiagnostics::formatTelemetryScalar(postFilterPeak)
                   << ", wetMixPeak=" << NovaDiagnostics::formatTelemetryScalar(wetMixPeak)
                   << juce::newLine
                   << "gain.window: wetTrimMin=" << NovaDiagnostics::formatTelemetryScalar(wetTrimMin == 1.0e9f ? 0.0f : wetTrimMin)
                   << ", wetTrimMax=" << NovaDiagnostics::formatTelemetryScalar(wetTrimMax)
                   << ", levelMin=" << NovaDiagnostics::formatTelemetryScalar(levelMin == 1.0e9f ? 0.0f : levelMin)
                   << ", levelMax=" << NovaDiagnostics::formatTelemetryScalar(levelMax);
            return report;
        }
    };

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
            const float fastCoeff = detector > state.envelopeFast
                ? (0.20f + drive * 0.04f)
                : (0.0032f + drive * 0.0012f);
            const float slowCoeff = detector > state.envelopeSlow ? 0.022f : 0.0009f;
            state.envelopeFast += (detector - state.envelopeFast) * fastCoeff;
            state.envelopeSlow += (detector - state.envelopeSlow) * slowCoeff;
            const float sagEnvelope = state.envelopeFast * 0.68f + state.envelopeSlow * 0.32f;

            const float inputGain = juce::Decibels::decibelsToGain(5.0f + drive * 22.0f);
            const float sag = 1.0f / (1.0f + sagEnvelope * (0.16f + texture * 0.38f) * (1.0f + drive * 1.22f));
            float x = input * inputGain * sag;

            const float bias = (0.018f + texture * 0.085f + drive * 0.03f) * std::tanh(x * 0.70f);
            const float soft = std::tanh((x + bias) * (1.10f + texture * 0.85f + drive * 0.25f));
            const float dense = std::atan((x + bias * 0.45f) * (1.30f + texture * 2.10f + drive * 0.55f))
                * (2.0f / juce::MathConstants<float>::pi);
            const float polynomialInput = juce::jlimit(-1.65f, 1.65f, x + bias * (0.65f + texture * 0.25f));
            const float polynomial = polynomialInput - 0.185f * polynomialInput * polynomialInput * polynomialInput;

            const float denseBlend = soft * (0.48f - texture * 0.12f) + dense * (0.24f + texture * 0.18f);
            const float richer = juce::jmap(texture, denseBlend + polynomial * 0.28f, denseBlend + polynomial * 0.56f);
            float y = juce::jmap(texture, soft, richer);
            y = std::tanh(y * (1.02f + drive * 0.14f));

            state.dcOffset = (state.dcOffset * Nova::Config::DC_OFFSET_DECAY) + (y * Nova::Config::DC_OFFSET_ATTACK);
            return y - state.dcOffset;
        }

    private:
        struct ChannelState
        {
            float envelopeFast = 0.0f;
            float envelopeSlow = 0.0f;
            float dcOffset = 0.0f;
        };

        double sampleRate = 44100.0;
        std::vector<ChannelState> channelState;
    };

    struct ToneModel
    {
        float presenceAmount = 0.0f;
        float bodyAmount = 0.0f;
        float preShapeBlend = 0.0f;
        float airRestoreAmount = 0.0f;
    };

    static int oversamplingFactor() noexcept
    {
        return 8;
    }

    static float levelFromControl(float control) noexcept
    {
        const float levelDb = juce::jmap(juce::jlimit(0.0f, 1.0f, control), -18.0f, 6.0f);
        return juce::Decibels::decibelsToGain(levelDb);
    }

    static float snapMixTarget(float mix) noexcept
    {
        if (mix <= 1.0e-4f)
            return 0.0f;
        if (mix >= 0.9999f)
            return 1.0f;
        return juce::jlimit(0.0f, 1.0f, mix);
    }

    static float wetTrimFromControls(float driveControl, float textureControl) noexcept
    {
        const float drive = juce::jlimit(0.0f, 1.0f, driveControl / 100.0f);
        const float texture = juce::jlimit(0.0f, 1.0f, textureControl);
        const float compensationDb = juce::jmap(drive, -0.35f, -4.60f) + juce::jmap(texture, 0.0f, -1.35f);
        return juce::Decibels::decibelsToGain(compensationDb);
    }

    void updateToneModel(float driveControl, float toneControl, float textureControl)
    {
        const float drive = juce::jlimit(0.0f, 1.0f, driveControl / 100.0f);
        const float tone = juce::jlimit(0.0f, 1.0f, toneControl);
        const float texture = juce::jlimit(0.0f, 1.0f, textureControl);

        inputTighten.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, drive * 0.78f + texture * 0.22f), 36.0f, 96.0f));
        presenceSplit.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, 0.14f + tone * 0.72f), 780.0f, 2600.0f));
        preShapeFilter.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f,
            0.24f + tone * 0.48f - drive * 0.10f + texture * 0.08f), 2900.0f, 7600.0f));
        bodyFilter.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f, 0.24f + texture * 0.46f + drive * 0.10f), 145.0f, 360.0f));

        const float topCutControl = juce::jlimit(0.0f, 1.0f, 0.05f + tone * 0.90f - drive * 0.10f + texture * 0.05f);
        const float topCutHz = juce::jmap(topCutControl, 2200.0f, 11800.0f);
        outputLowPassA.setCutoff(topCutHz);
        outputLowPassB.setCutoff(topCutHz);
        airRestoreFilter.setCutoff(juce::jmap(juce::jlimit(0.0f, 1.0f,
            0.20f + tone * 0.62f + texture * 0.10f), 1500.0f, 4200.0f));
        dcBlock.setCutoff(18.0f);

        toneModel.presenceAmount = juce::jmap(juce::jlimit(0.0f, 1.0f, tone * 0.82f + texture * 0.16f), -0.10f, 0.54f);
        toneModel.bodyAmount = juce::jmap(juce::jlimit(0.0f, 1.0f, texture * 0.66f + drive * 0.16f - tone * 0.16f), -0.08f, 0.24f);
        toneModel.preShapeBlend = juce::jmap(juce::jlimit(0.0f, 1.0f,
            drive * 0.52f + texture * 0.20f - tone * 0.08f), 0.04f, 0.24f);
        toneModel.airRestoreAmount = juce::jmap(juce::jlimit(0.0f, 1.0f,
            tone * 0.72f + texture * 0.12f + drive * 0.08f), 0.02f, 0.22f);
    }

    juce::dsp::Oversampling<float> oversampler;
    OnePoleFilterBank inputTighten;
    OnePoleFilterBank presenceSplit;
    OnePoleFilterBank preShapeFilter;
    OnePoleFilterBank bodyFilter;
    OnePoleFilterBank outputLowPassA;
    OnePoleFilterBank outputLowPassB;
    OnePoleFilterBank airRestoreFilter;
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
    NovaDiagnostics::PedalSignalTelemetry signalTelemetry { "overdrive" };
    DebugTelemetry debugTelemetry;
};

#include "OverdriveEditor.h"
