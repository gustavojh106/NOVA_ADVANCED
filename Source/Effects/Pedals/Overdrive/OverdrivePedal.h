#pragma once

#include "../Base/ProcessorBase.h"
#include "../../../Core/PedalSignalTelemetry.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <array>
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
        preparedBlockSize = juce::jmax(1, samplesPerBlock);
        preparedChannels = juce::jmax(2, getTotalNumOutputChannels());
        const auto numChannels = (size_t) preparedChannels;

        inputTighten.prepare(currentInnerSampleRate, numChannels);
        presenceSplit.prepare(currentInnerSampleRate, numChannels);
        preShapeFilter.prepare(currentInnerSampleRate, numChannels);
        bodyFilter.prepare(currentInnerSampleRate, numChannels);
        outputLowPassA.prepare(currentInnerSampleRate, numChannels);
        outputLowPassB.prepare(currentInnerSampleRate, numChannels);
        airRestoreFilter.prepare(currentInnerSampleRate, numChannels);
        dcBlock.prepare(currentInnerSampleRate, numChannels);
        finalDcBlock.prepare(sampleRate, numChannels);
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

        scratchBuffer.setSize(preparedChannels,
            preparedBlockSize,
            false,
            false,
            true);

        updateToneModel(drive, tone, texture);

        setProcessingLatency((int) oversampler.getLatencyInSamples());
        prepareBypassSmoother(sampleRate, samplesPerBlock);

        reset();
        signalTelemetry.prepare(sampleRate);
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
        finalDcBlock.reset();
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

        if (numChannels > preparedChannels || numSamples > preparedBlockSize)
        {
            ++debugTelemetry.blockOverflowCount;
            endBypassProcess(buffer);
            return;
        }

        const auto guardStats = scrubBuffer(buffer);
        debugTelemetry.guardInvalidSamples += guardStats.invalidSamples;
        debugTelemetry.guardClippedSamples += guardStats.clippedSamples;
        debugTelemetry.guardDenormalSamples += guardStats.denormalSamples;

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), numSamples);

        const float inputPeak = measurePeak(buffer);
        const float inputTrim = wetFeedTrimForPeak(inputPeak);
        debugTelemetry.captureInputTrim(inputTrim);
        multiplyBuffer(buffer, inputTrim);

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
                x = softCeiling(x, 1.15f);
                debugTelemetry.capturePostFilter(x);
                data[sample] = x;
            }
        }

        oversampler.processSamplesDown(block);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const float original = sanitizeAudioSample(data[sample]);
                float cleaned = finalDcBlock.processHighPass(ch, original);

                // DC cleanup is not counted as limiting. Only count true overs.
                if (std::abs(cleaned) > 1.15f)
                {
                    const float safe = softCeiling(cleaned, 1.20f);
                    debugTelemetry.captureSafety(cleaned, safe);
                    cleaned = safe;
                }

                data[sample] = cleaned;
            }
        }

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

                const float original = sanitizeAudioSample(((clean * dryGain) + (wet * wetGain)) * level);
                const float safe = std::abs(original) > 1.08f ? softCeiling(original, 1.18f) : original;
                debugTelemetry.captureSafety(original, safe);
                buffer.setSample(ch, sample, safe);
            }
        }

        if (signalTelemetry.captureOutputAndPublishIfNeeded(buffer))
            debugTelemetry.resetWindow();

        endBypassProcess(buffer);
    }

private:

    struct LocalGuardStats
    {
        int invalidSamples = 0;
        int clippedSamples = 0;
        int denormalSamples = 0;
    };

    static float sanitizeAudioSample(float x) noexcept
    {
        if (!std::isfinite(x))
            return 0.0f;

        if (std::abs(x) < 1.0e-30f)
            return 0.0f;

        return juce::jlimit(-8.0f, 8.0f, x);
    }

    static float softCeiling(float x, float ceiling) noexcept
    {
        if (!std::isfinite(x))
            return 0.0f;

        ceiling = juce::jmax(0.001f, ceiling);

        // V2: transparent safety.
        // V1 used an 88% threshold and touched too much normal guitar signal.
        // This only catches real overs close to the requested ceiling.
        const float threshold = ceiling * 0.985f;
        const float mag = std::abs(x);

        if (mag <= threshold)
            return x;

        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float knee = juce::jmax(0.0001f, ceiling - threshold);
        const float shaped = threshold + knee * std::tanh((mag - threshold) / knee);

        return sign * juce::jmin(ceiling, shaped);
    }

    static float emergencyCeiling(float x) noexcept
    {
        // Last-resort protection only. Normal overdrive tone should not live here.
        return softCeiling(x, 1.20f);
    }

    static float measurePeak(const juce::AudioBuffer<float>& buffer) noexcept
    {
        float peak = 0.0f;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* data = buffer.getReadPointer(ch);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
                peak = juce::jmax(peak, std::abs(data[i]));
        }

        return peak;
    }

    static float wetFeedTrimForPeak(float peak) noexcept
    {
        if (!std::isfinite(peak))
            return 1.0f;

        peak = std::abs(peak);

        // V2: do not choke the pedal on normal amp-chain levels.
        // Only trim when the wet engine is being hit unusually hard.
        if (peak <= 1.25f)
            return 1.0f;

        return juce::jlimit(0.78f, 1.0f, 1.25f / peak);
    }

    static LocalGuardStats scrubBuffer(juce::AudioBuffer<float>& buffer) noexcept
    {
        LocalGuardStats stats;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float original = data[i];
                const float safe = sanitizeAudioSample(original);

                if (!std::isfinite(original))
                    ++stats.invalidSamples;
                else if (original != 0.0f && std::abs(original) < 1.0e-30f)
                    ++stats.denormalSamples;
                else if (std::abs(original) > 8.0f)
                    ++stats.clippedSamples;

                data[i] = safe;
            }
        }

        return stats;
    }

    static void multiplyBuffer(juce::AudioBuffer<float>& buffer, float gain) noexcept
    {
        if (std::abs(gain - 1.0f) <= 1.0e-6f)
            return;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::multiply(buffer.getWritePointer(ch), gain, buffer.getNumSamples());
    }

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
        float inputTrimMin = 1.0e9f;
        float inputTrimMax = 0.0f;
        float safetyDeltaPeak = 0.0f;
        int safetyTouchedSamples = 0;
        int blockOverflowCount = 0;
        int guardInvalidSamples = 0;
        int guardClippedSamples = 0;
        int guardDenormalSamples = 0;

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
            inputTrimMin = 1.0e9f;
            inputTrimMax = 0.0f;
            safetyDeltaPeak = 0.0f;
            safetyTouchedSamples = 0;
            blockOverflowCount = 0;
            guardInvalidSamples = 0;
            guardClippedSamples = 0;
            guardDenormalSamples = 0;
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

        void captureInputTrim(float value) noexcept
        {
            inputTrimMin = juce::jmin(inputTrimMin, value);
            inputTrimMax = juce::jmax(inputTrimMax, value);
        }

        void captureSafety(float original, float safe) noexcept
        {
            const float delta = std::abs(original - safe);
            safetyDeltaPeak = juce::jmax(safetyDeltaPeak, delta);
            if (delta >= 1.0e-5f)
                ++safetyTouchedSamples;
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
                   << ", levelMax=" << NovaDiagnostics::formatTelemetryScalar(levelMax)
                   << juce::newLine
                   << "safety.window: inputTrimMin=" << NovaDiagnostics::formatTelemetryScalar(inputTrimMin == 1.0e9f ? 0.0f : inputTrimMin)
                   << ", inputTrimMax=" << NovaDiagnostics::formatTelemetryScalar(inputTrimMax)
                   << ", safetyDeltaPeak=" << NovaDiagnostics::formatTelemetryScalar(safetyDeltaPeak)
                   << ", safetyTouchedSamples=" << safetyTouchedSamples
                   << ", blockOverflow=" << blockOverflowCount
                   << juce::newLine
                   << "guard: invalid=" << guardInvalidSamples
                   << ", clipped=" << guardClippedSamples
                   << ", denormals=" << guardDenormalSamples;
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

        void ensureChannels(size_t) noexcept
        {
            // Intentionally no-op in the audio path.
            // Channel count must be allocated in prepareToPlay().
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

    // Two-stage soft tanh saturator, modelled after a Tube-Screamer-style
    // op-amp clipper followed by a gentler diode-style stage. The signal
    // is hard-bounded by tanh at every stage so the output magnitude can
    // never exceed ~1.0 regardless of input or drive — this is what kills
    // the "dirty / clipping at high drive" behaviour of the legacy stage.
    //
    // Tone character comes from:
    //   * a small DC bias added before stage 1 → asymmetric clipping →
    //     even-order harmonics (warm, tube-like) without DC drift
    //   * a fast envelope that gently sags the pre-gain on transients →
    //     touch-sensitive feel, prevents instantaneous overshoot
    //   * a per-channel DC blocker that removes the bias residue so the
    //     downstream graph never sees a sustained offset
    class ResponsiveDriveStage
    {
    public:
        void prepare(double newSampleRate, size_t numChannels)
        {
            sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
            channelState.assign(numChannels, {});
            // 8 Hz one-pole DC trap (innocuous on guitar fundamentals)
            dcCoeff = (float) (1.0 - std::exp(-juce::MathConstants<double>::twoPi * 8.0 / sampleRate));
            reset();
        }

        void ensureChannels(size_t) noexcept
        {
            // Intentionally no-op in the audio path.
            // Channel count must be allocated in prepareToPlay().
        }

        void reset()
        {
            for (auto& state : channelState)
                state = {};
        }

        float processSample(int channel, float input, float driveControl, float textureControl) noexcept
        {
            auto& state = channelState[(size_t) channel];
            const float drive   = juce::jlimit(0.0f, 1.0f, driveControl / 100.0f);
            const float texture = juce::jlimit(0.0f, 1.0f, textureControl);

            // ---- Touch-sensitive envelope (controls supply sag) ----
            const float detector = std::abs(input);
            const float atkCoeff = detector > state.envelope ? 0.20f : 0.005f;
            state.envelope += atkCoeff * (detector - state.envelope);

            // ---- Stage 1: clean op-amp gain into soft tanh ----
            // Cap the pre-gain at +20 dB. Anything beyond that just pushes
            // tanh further into saturation without adding musical content.
            // The sag term shaves up to ~3 dB during loud transients to
            // keep the saturator out of brick-wall territory.
            const float stage1GainDb = 4.0f + drive * 16.0f;          // 4..20 dB
            const float stage1Gain   = juce::Decibels::decibelsToGain(stage1GainDb);
            const float sag          = 1.0f / (1.0f + state.envelope * (0.18f + texture * 0.22f) * (0.6f + drive * 1.4f));

            // Asymmetric, signal-dependent bias for tube-style even harmonics.
            // It tracks the envelope so a silent input still produces silence
            // (the bias modulates the active signal, it does not inject DC).
            const float biasAmount = 0.026f + texture * 0.050f + drive * 0.014f;
            const float bias = biasAmount * state.envelope;

            float x = (input * stage1Gain * sag) + bias;
            x = std::tanh(x);                                          // bounded [-1, 1]

            // ---- Stage 2: secondary diode-style soft clip ----
            // Stage 1 already brings the signal close to the saturator's
            // shoulder. Stage 2 applies a smaller secondary curve to add
            // upper harmonics without re-clipping the signal hard.
            const float stage2Drive = 1.0f + drive * 1.4f + texture * 0.4f; // 1.0..2.8
            x = std::tanh(x * stage2Drive);                           // bounded [-1, 1]

            // ---- DC trap: remove the asymmetry residue ----
            state.dcAvg += dcCoeff * (x - state.dcAvg);
            x -= state.dcAvg;

            // ---- Internal makeup gain ----
            // Compensates for the level loss caused by tanh saturation so
            // that the wet stage feeds the rest of the chain at a roughly
            // unity perceived level across the whole drive range.
            const float makeup = 0.90f - drive * 0.14f;               // 0.76..0.90
            return OverdrivePedal::softCeiling(x * makeup, 1.05f);
        }

    private:
        struct ChannelState
        {
            float envelope = 0.0f;
            float dcAvg    = 0.0f;
        };

        double sampleRate = 44100.0;
        float dcCoeff = 0.001f;
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
        const float levelDb = juce::jmap(juce::jlimit(0.0f, 1.0f, control), -18.0f, 4.5f);
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
        // The new two-stage saturator already produces a roughly unity-level
        // wet signal, so the trim is mostly cosmetic now: a hair of attenuation
        // at high drive / texture to keep cumulative tone-stack peaks below
        // 0 dBFS even with bright EQ and the post-drive body/air boosts.
        const float drive = juce::jlimit(0.0f, 1.0f, driveControl / 100.0f);
        const float texture = juce::jlimit(0.0f, 1.0f, textureControl);
        const float compensationDb = juce::jmap(drive, -0.35f, -2.20f) + juce::jmap(texture, -0.05f, -0.70f);
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

        // Tone-stack amounts trimmed so the cumulative post-drive boost
        // (body + air + presence) cannot push the saturator's [-1, 1]
        // output above 0 dBFS even when all three pile up in phase.
        // Worst-case headroom: 1.0 + 0.18 (body) + 0.16 (air) ≈ 1.34, still
        // safely tamed by the downstream output-chain limiter / soft ceiling.
        // V2 musical-safe tone stack:
        // more open than V1, still less explosive than the original.
        toneModel.presenceAmount = juce::jmap(juce::jlimit(0.0f, 1.0f, tone * 0.76f + texture * 0.13f), -0.07f, 0.32f);
        toneModel.bodyAmount = juce::jmap(juce::jlimit(0.0f, 1.0f, texture * 0.58f + drive * 0.14f - tone * 0.13f), -0.055f, 0.14f);
        toneModel.preShapeBlend = juce::jmap(juce::jlimit(0.0f, 1.0f,
            drive * 0.47f + texture * 0.17f - tone * 0.06f), 0.035f, 0.17f);
        toneModel.airRestoreAmount = juce::jmap(juce::jlimit(0.0f, 1.0f,
            tone * 0.66f + texture * 0.09f + drive * 0.06f), 0.012f, 0.12f);
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
    OnePoleFilterBank finalDcBlock;
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
    int preparedBlockSize = 512;
    int preparedChannels = 2;
    ToneModel toneModel;
    bool isPrepared = false;
    NovaDiagnostics::PedalSignalTelemetry signalTelemetry { "overdrive" };
    DebugTelemetry debugTelemetry;
};

#include "OverdriveEditor.h"
