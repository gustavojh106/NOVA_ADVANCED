#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <limits>

#include "../../Core/PedalSignalTelemetry.h"
#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"

/*
    NOVA CleanAmp - Studio Rebuild

    This is a full rebuild of the Clean Amp processor while preserving:
    - class name: CleanAmp
    - parameter IDs:
        cleanDrive
        cleanBass
        cleanTreble
        cleanReverb
        cleanLevel
    - editor integration through PremiumPedalEditor
    - ProcessorBase bypass API

    Design goals:
    - No allocations in processBlock().
    - Stable bounded internal reverb. No runaway return peaks.
    - Safer gain staging into and out of the amp.
    - DC protection before/after nonlinear stages.
    - 8x oversampled nonlinear stage.
    - Smoother parameter transitions.
    - Studio-style output safety without destroying dynamics.
*/

class CleanAmp final : public ProcessorBase
{
public:
    CleanAmp()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("cleanDrive", "Drive", 0.0f, 1.0f, 0.25f));
        addParameter(bassParam = new juce::AudioParameterFloat("cleanBass", "Bass", 0.0f, 1.0f, 0.52f));
        addParameter(trebleParam = new juce::AudioParameterFloat("cleanTreble", "Treble", 0.0f, 1.0f, 0.55f));
        addParameter(reverbParam = new juce::AudioParameterFloat("cleanReverb", "Reverb", 0.0f, 1.0f, 0.20f));
        addParameter(levelParam = new juce::AudioParameterFloat("cleanLevel", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Clean Amp"; }

    bool hasEditor() const override { return true; }

    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Amplifier",
            "Crystal",
            juce::Colour::fromString("ff60A5FA"),
            {
                { "Drive",  driveParam,  [](float value) { return formatPercent(value); } },
                { "Bass",   bassParam,   [](float value) { return formatPercent(value); } },
                { "Treble", trebleParam, [](float value) { return formatPercent(value); } },
                { "Reverb", reverbParam, [](float value) { return formatPercent(value); } },
                { "Master", levelParam,  [](float value) { return formatGain(value); } }
            },
            214,
            178);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        juce::ScopedNoDenormals noDenormals;

        currentSampleRate = sampleRate;
        preparedBlockSize = juce::jmax(1, samplesPerBlock);
        preparedChannels = juce::jlimit(1, kMaxChannels, juce::jmax(1, getTotalNumOutputChannels()));
        currentInnerRate = currentSampleRate * (double)kOversamplingFactor;

        oversampler.reset();
        oversampler.initProcessing((size_t)preparedBlockSize);

        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate = currentInnerRate;
        innerSpec.maximumBlockSize = (juce::uint32)(preparedBlockSize * kOversamplingFactor);
        innerSpec.numChannels = (juce::uint32)preparedChannels;

        inputHighPass.prepare(innerSpec);
        bassShelf.prepare(innerSpec);
        trebleShelf.prepare(innerSpec);
        postSaturationHighPass.prepare(innerSpec);

        inputDcBlocker.prepare(currentInnerRate, preparedChannels, 16.0f);
        outputDcBlocker.prepare(currentSampleRate, preparedChannels, 14.0f);
        room.prepare(currentSampleRate, preparedChannels);

        driveSmooth.reset(currentInnerRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        bassSmooth.reset(currentInnerRate, 0.035);
        trebleSmooth.reset(currentInnerRate, 0.035);
        reverbSmooth.reset(currentSampleRate, 0.060);
        masterSmooth.reset(currentSampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        driveSmooth.setCurrentAndTargetValue(readDrive());
        bassSmooth.setCurrentAndTargetValue(readBass());
        trebleSmooth.setCurrentAndTargetValue(readTreble());
        reverbSmooth.setCurrentAndTargetValue(readReverb());
        masterSmooth.setCurrentAndTargetValue(readLevel());

        cachedBass = std::numeric_limits<float>::quiet_NaN();
        cachedTreble = std::numeric_limits<float>::quiet_NaN();

        updateToneFilters(true);

        setProcessingLatency((int)std::ceil(oversampler.getLatencyInSamples()));
        prepareBypassSmoother(sampleRate, samplesPerBlock);

        signalTelemetry.prepare(currentSampleRate);
        debugTelemetry.resetWindow();

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

        inputHighPass.reset();
        bassShelf.reset();
        trebleShelf.reset();
        postSaturationHighPass.reset();

        inputDcBlocker.reset();
        outputDcBlocker.reset();
        room.reset();

        driveSmooth.setCurrentAndTargetValue(readDrive());
        bassSmooth.setCurrentAndTargetValue(readBass());
        trebleSmooth.setCurrentAndTargetValue(readTreble());
        reverbSmooth.setCurrentAndTargetValue(readReverb());
        masterSmooth.setCurrentAndTargetValue(readLevel());

        cachedBass = std::numeric_limits<float>::quiet_NaN();
        cachedTreble = std::numeric_limits<float>::quiet_NaN();
        updateToneFilters(true);

        signalTelemetry.reset();
        debugTelemetry.resetWindow();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;

        const auto guardStats = scrubBuffer(buffer);
        debugTelemetry.guardInvalidSamples += guardStats.invalidSamples;
        debugTelemetry.guardClippedSamples += guardStats.clippedSamples;
        debugTelemetry.guardDenormalSamples += guardStats.denormalSamples;

        signalTelemetry.captureInput(buffer);

        if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        {
            endBypassProcess(buffer);
            return;
        }

        updateParameterTargets();

        if (buffer.getNumSamples() > preparedBlockSize)
        {
            // Real-time safe fallback. Do not allocate in the audio callback.
            // If this ever happens, the host sent a larger block than prepareToPlay declared.
            debugTelemetry.blockOverflowCount++;
            emergencySafePass(buffer);
            emitTelemetry(buffer);
            endBypassProcess(buffer);
            return;
        }

        processOversampledAmp(buffer);
        processBoundedReverbAndMaster(buffer);

        emitTelemetry(buffer);

        endBypassProcess(buffer);
    }

private:
    struct GuardStats
    {
        int invalidSamples = 0;
        int clippedSamples = 0;
        int denormalSamples = 0;
    };

    static GuardStats scrubBuffer(juce::AudioBuffer<float>& buffer) noexcept
    {
        GuardStats stats;

        const int channels = buffer.getNumChannels();
        const int samples = buffer.getNumSamples();

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);

            for (int i = 0; i < samples; ++i)
            {
                float x = data[i];

                if (!std::isfinite(x))
                {
                    data[i] = 0.0f;
                    ++stats.invalidSamples;
                    continue;
                }

                // Prevent absurd upstream values from poisoning filters/reverb.
                // This is not the tone shaper; it is just an emergency safety clamp.
                if (x > 8.0f)
                {
                    x = 8.0f;
                    ++stats.clippedSamples;
                }
                else if (x < -8.0f)
                {
                    x = -8.0f;
                    ++stats.clippedSamples;
                }

                if (std::abs(x) < 1.0e-30f && x != 0.0f)
                {
                    x = 0.0f;
                    ++stats.denormalSamples;
                }

                data[i] = x;
            }
        }

        return stats;
    }

    static constexpr int kMaxChannels = 8;
    static constexpr int kOversamplingFactor = 8;

    using IIRFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    struct MultiChannelDcBlocker
    {
        void prepare(double newSampleRate, int channels, float cutoffHz) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            numChannels = juce::jlimit(1, kMaxChannels, channels);
            setCutoff(cutoffHz);
            reset();
        }

        void reset() noexcept
        {
            x1.fill(0.0f);
            y1.fill(0.0f);
        }

        void setCutoff(float cutoffHz) noexcept
        {
            const float cutoff = juce::jlimit(5.0f, 45.0f, cutoffHz);
            pole = (float)std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sampleRate);
        }

        float process(int channel, float x) noexcept
        {
            channel = juce::jlimit(0, kMaxChannels - 1, channel);

            const float y = x - x1[(size_t)channel] + (pole * y1[(size_t)channel]);
            x1[(size_t)channel] = x;
            y1[(size_t)channel] = y;
            return y;
        }

        void processBuffer(juce::AudioBuffer<float>& buffer) noexcept
        {
            const int channels = juce::jmin(buffer.getNumChannels(), numChannels);
            const int samples = buffer.getNumSamples();

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = buffer.getWritePointer(ch);

                for (int i = 0; i < samples; ++i)
                    data[i] = process(ch, data[i]);
            }
        }

        double sampleRate = 44100.0;
        float pole = 0.995f;
        int numChannels = 2;
        std::array<float, kMaxChannels> x1{};
        std::array<float, kMaxChannels> y1{};
    };

    struct DebugTelemetry
    {
        float preDrivePeak = 0.0f;
        float saturationPeak = 0.0f;
        float tonePeak = 0.0f;
        float reverbReturnPeak = 0.0f;
        float outputPeak = 0.0f;
        float safetyDeltaPeak = 0.0f;

        float driveMin = 1.0e9f;
        float driveMax = 0.0f;
        float bassMin = 1.0e9f;
        float bassMax = 0.0f;
        float trebleMin = 1.0e9f;
        float trebleMax = 0.0f;
        float reverbMixMin = 1.0e9f;
        float reverbMixMax = 0.0f;
        float masterMin = 1.0e9f;
        float masterMax = 0.0f;

        int guardInvalidSamples = 0;
        int guardClippedSamples = 0;
        int guardDenormalSamples = 0;
        int safetyTouchedSamples = 0;
        int blockOverflowCount = 0;

        void resetWindow() noexcept
        {
            preDrivePeak = 0.0f;
            saturationPeak = 0.0f;
            tonePeak = 0.0f;
            reverbReturnPeak = 0.0f;
            outputPeak = 0.0f;
            safetyDeltaPeak = 0.0f;

            driveMin = 1.0e9f;
            driveMax = 0.0f;
            bassMin = 1.0e9f;
            bassMax = 0.0f;
            trebleMin = 1.0e9f;
            trebleMax = 0.0f;
            reverbMixMin = 1.0e9f;
            reverbMixMax = 0.0f;
            masterMin = 1.0e9f;
            masterMax = 0.0f;

            guardInvalidSamples = 0;
            guardClippedSamples = 0;
            guardDenormalSamples = 0;
            safetyTouchedSamples = 0;
            blockOverflowCount = 0;
        }

        void capturePreDrive(float value) noexcept
        {
            preDrivePeak = juce::jmax(preDrivePeak, std::abs(value));
        }

        void captureSaturation(float value) noexcept
        {
            saturationPeak = juce::jmax(saturationPeak, std::abs(value));
        }

        void captureToneOutput(float value) noexcept
        {
            tonePeak = juce::jmax(tonePeak, std::abs(value));
        }

        void captureReverbReturn(float value) noexcept
        {
            reverbReturnPeak = juce::jmax(reverbReturnPeak, std::abs(value));
        }

        void captureOutput(float value) noexcept
        {
            outputPeak = juce::jmax(outputPeak, std::abs(value));
        }

        void captureSafety(float original, float safe) noexcept
        {
            const float delta = std::abs(original - safe);
            safetyDeltaPeak = juce::jmax(safetyDeltaPeak, delta);

            if (delta >= 1.0e-5f)
                ++safetyTouchedSamples;
        }

        void captureControlWindow(float drive,
            float bass,
            float treble,
            float reverbMix,
            float master) noexcept
        {
            driveMin = juce::jmin(driveMin, drive);
            driveMax = juce::jmax(driveMax, drive);
            bassMin = juce::jmin(bassMin, bass);
            bassMax = juce::jmax(bassMax, bass);
            trebleMin = juce::jmin(trebleMin, treble);
            trebleMax = juce::jmax(trebleMax, treble);
            reverbMixMin = juce::jmin(reverbMixMin, reverbMix);
            reverbMixMax = juce::jmax(reverbMixMax, reverbMix);
            masterMin = juce::jmin(masterMin, master);
            masterMax = juce::jmax(masterMax, master);
        }

        juce::String buildReport(const CleanAmp& amp) const
        {
            auto safeMin = [](float value)
                {
                    return value >= 1.0e8f ? 0.0f : value;
                };

            juce::String report;
            report << "params: drive=" << NovaDiagnostics::formatTelemetryScalar(amp.readDrive())
                << ", bass=" << NovaDiagnostics::formatTelemetryScalar(amp.readBass())
                << ", treble=" << NovaDiagnostics::formatTelemetryScalar(amp.readTreble())
                << ", reverb=" << NovaDiagnostics::formatTelemetryScalar(amp.readReverb())
                << ", level=" << NovaDiagnostics::formatTelemetryScalar(amp.readLevel())
                << ", innerSampleRate=" << NovaDiagnostics::formatTelemetryScalar((float) amp.currentInnerRate)
                << ", oversamplingFactor=" << kOversamplingFactor
                << juce::newLine
                << "amp.path: preDrivePeak=" << NovaDiagnostics::formatTelemetryScalar(preDrivePeak)
                << ", saturationPeak=" << NovaDiagnostics::formatTelemetryScalar(saturationPeak)
                << ", tonePeak=" << NovaDiagnostics::formatTelemetryScalar(tonePeak)
                << ", outputPeak=" << NovaDiagnostics::formatTelemetryScalar(outputPeak)
                << juce::newLine
                << "reverb.path: returnPeak=" << NovaDiagnostics::formatTelemetryScalar(reverbReturnPeak)
                << juce::newLine
                << "control.window: driveMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(driveMin))
                << ", driveMax=" << NovaDiagnostics::formatTelemetryScalar(driveMax)
                << ", bassMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(bassMin))
                << ", bassMax=" << NovaDiagnostics::formatTelemetryScalar(bassMax)
                << ", trebleMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(trebleMin))
                << ", trebleMax=" << NovaDiagnostics::formatTelemetryScalar(trebleMax)
                << ", reverbMixMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(reverbMixMin))
                << ", reverbMixMax=" << NovaDiagnostics::formatTelemetryScalar(reverbMixMax)
                << ", masterMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(masterMin))
                << ", masterMax=" << NovaDiagnostics::formatTelemetryScalar(masterMax)
                << juce::newLine
                << "safety: deltaPeak=" << NovaDiagnostics::formatTelemetryScalar(safetyDeltaPeak)
                << ", touchedSamples=" << safetyTouchedSamples
                << ", blockOverflow=" << blockOverflowCount
                << juce::newLine
                << "guard: invalid=" << guardInvalidSamples
                << ", clipped=" << guardClippedSamples
                << ", denormals=" << guardDenormalSamples;
            return report;
        }
    };


    struct BoundedStudioRoom
    {
        static constexpr int kLines = 4;
        static constexpr int kMaxDelaySamples = 32768;

        static float boundedSoftLimit(float x, float ceiling) noexcept
        {
            if (!std::isfinite(x))
                return 0.0f;

            ceiling = juce::jmax(0.001f, ceiling);
            const float threshold = ceiling * 0.88f;
            const float mag = std::abs(x);

            if (mag <= threshold)
                return x;

            const float sign = x < 0.0f ? -1.0f : 1.0f;
            const float knee = ceiling - threshold;
            const float shaped = threshold + (knee * std::tanh((mag - threshold) / knee));

            return sign * juce::jmin(ceiling, shaped);
        }

        void prepare(double newSampleRate, int channels)
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            numChannels = juce::jlimit(1, 2, channels);

            delayStorage.setSize(numChannels * kLines,
                kMaxDelaySamples,
                false,
                false,
                true);

            configureDelays();
            reset();
        }

        void reset() noexcept
        {
            if (delayStorage.getNumSamples() > 0)
                delayStorage.clear();

            writeIndex = 0;
            dampingState.fill(0.0f);
        }

        void process(juce::AudioBuffer<float>& buffer,
            juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>& reverbSmooth,
            DebugTelemetry& telemetry) noexcept
        {
            if (delayStorage.getNumSamples() <= 0)
                return;

            const int channels = juce::jmin(buffer.getNumChannels(), numChannels);
            const int samples = buffer.getNumSamples();

            if (channels <= 0 || samples <= 0)
                return;

            for (int i = 0; i < samples; ++i)
            {
                const float amount = juce::jlimit(0.0f, 1.0f, reverbSmooth.getNextValue());

                // More natural studio-room level. V1 was too safe and the return became nearly inaudible.
                const float feedback = 0.20f + (amount * 0.30f);
                const float send = amount * 0.55f;
                const float returnGain = amount * 0.75f;
                const float damping = 0.30f + (amount * 0.24f);

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = buffer.getWritePointer(ch);
                    const float dry = data[i];

                    const float wet = processOneChannel(ch,
                        dry,
                        feedback,
                        send,
                        returnGain,
                        damping);

                    telemetry.captureReverbReturn(wet);

                    // Studio amp reverb: dry remains stable, wet return is bounded and additive.
                    data[i] = dry + wet;
                }

                advanceWriteIndex();
            }
        }

        double sampleRate = 44100.0;
        int numChannels = 2;
        int writeIndex = 0;

        juce::AudioBuffer<float> delayStorage;
        std::array<int, kLines * 2> delaySamples{};
        std::array<float, kLines * 2> dampingState{};

    private:
        void configureDelays() noexcept
        {
            // Short/medium decorrelated room taps. They intentionally avoid simple multiples.
            constexpr std::array<float, kLines> leftMs{ 17.3f, 29.7f, 41.1f, 58.9f };
            constexpr std::array<float, kLines> rightMs{ 19.1f, 31.9f, 43.7f, 61.3f };

            for (int line = 0; line < kLines; ++line)
            {
                delaySamples[(size_t)line] = msToSamples(leftMs[(size_t)line]);
                delaySamples[(size_t)(kLines + line)] = msToSamples(rightMs[(size_t)line]);
            }
        }

        int msToSamples(float ms) const noexcept
        {
            const int samples = juce::roundToInt((sampleRate * (double)ms) * 0.001);
            return juce::jlimit(1, kMaxDelaySamples - 1, samples);
        }

        float processOneChannel(int channel,
            float dry,
            float feedback,
            float send,
            float returnGain,
            float damping) noexcept
        {
            channel = juce::jlimit(0, numChannels - 1, channel);

            float sum = 0.0f;

            for (int line = 0; line < kLines; ++line)
            {
                const int lineIndex = (channel * kLines) + line;
                const int delay = delaySamples[(size_t)lineIndex];
                const int readIndex = (writeIndex + kMaxDelaySamples - delay) % kMaxDelaySamples;

                auto* delayData = delayStorage.getWritePointer(lineIndex);

                const float delayed = delayData[readIndex];

                float& damp = dampingState[(size_t)lineIndex];
                damp = (damping * damp) + ((1.0f - damping) * delayed);

                const float sign = (line & 1) == 0 ? 1.0f : -1.0f;
                const float input = (dry * send) + (damp * feedback * sign);

                delayData[writeIndex] = boundedSoftLimit(input, 1.00f);
                sum += damp * sign;
            }

            const float wet = (sum * (1.0f / (float)kLines)) * returnGain;
            return boundedSoftLimit(wet, 0.70f);
        }

        void advanceWriteIndex() noexcept
        {
            ++writeIndex;

            if (writeIndex >= kMaxDelaySamples)
                writeIndex = 0;
        }
    };

    static float sanitize01(float value, float fallback) noexcept
    {
        if (!std::isfinite(value))
            return fallback;

        return juce::jlimit(0.0f, 1.0f, value);
    }

    static float sanitizeLevel(float value) noexcept
    {
        if (!std::isfinite(value))
            return 1.0f;

        return juce::jlimit(0.0f, 2.0f, value);
    }

    static float softLimit(float x, float ceiling) noexcept
    {
        if (!std::isfinite(x))
            return 0.0f;

        ceiling = juce::jmax(0.001f, ceiling);
        const float threshold = ceiling * 0.92f;
        const float mag = std::abs(x);

        // Transparent for normal clean-amp levels. Only catches real overs.
        if (mag <= threshold)
            return x;

        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float knee = ceiling - threshold;
        const float shaped = threshold + (knee * std::tanh((mag - threshold) / knee));

        return sign * juce::jmin(ceiling, shaped);
    }

    static float triodeShape(float x) noexcept
    {
        // Studio-clean preamp curve.
        // V1 was stable but too compressed. This keeps most of the clean transient
        // and blends in controlled tube-like asymmetry only as coloration.
        const float shaped = std::tanh((x * 0.72f) + (0.025f * x * x)) * 1.05f;
        return (0.74f * x) + (0.26f * shaped);
    }

    float readDrive() const noexcept
    {
        return sanitize01(driveParam != nullptr ? driveParam->get() : 0.25f, 0.25f);
    }

    float readBass() const noexcept
    {
        return sanitize01(bassParam != nullptr ? bassParam->get() : 0.52f, 0.52f);
    }

    float readTreble() const noexcept
    {
        return sanitize01(trebleParam != nullptr ? trebleParam->get() : 0.55f, 0.55f);
    }

    float readReverb() const noexcept
    {
        return sanitize01(reverbParam != nullptr ? reverbParam->get() : 0.20f, 0.20f);
    }

    float readLevel() const noexcept
    {
        return sanitizeLevel(levelParam != nullptr ? levelParam->get() : 1.0f);
    }

    void updateParameterTargets() noexcept
    {
        driveSmooth.setTargetValue(readDrive());
        bassSmooth.setTargetValue(readBass());
        trebleSmooth.setTargetValue(readTreble());
        reverbSmooth.setTargetValue(readReverb());
        masterSmooth.setTargetValue(readLevel());
    }

    void updateToneFilters(bool force)
    {
        const float bass = bassSmooth.getCurrentValue();
        const float treble = trebleSmooth.getCurrentValue();

        const bool bassChanged = force || !std::isfinite(cachedBass) || std::abs(cachedBass - bass) > 0.0025f;
        const bool trebleChanged = force || !std::isfinite(cachedTreble) || std::abs(cachedTreble - treble) > 0.0025f;

        if (!bassChanged && !trebleChanged)
            return;

        cachedBass = bass;
        cachedTreble = treble;

        const float bassGainDb = juce::jmap(bass, -4.5f, 5.5f);
        const float trebleGainDb = juce::jmap(treble, -5.0f, 5.0f);

        *inputHighPass.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(currentInnerRate,
            38.0f,
            0.70710678f);

        *bassShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf(currentInnerRate,
            250.0f,
            0.64f,
            juce::Decibels::decibelsToGain(bassGainDb));

        *trebleShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf(currentInnerRate,
            3100.0f,
            0.58f,
            juce::Decibels::decibelsToGain(trebleGainDb));

        *postSaturationHighPass.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(currentInnerRate,
            18.0f,
            0.70710678f);
    }

    void processOversampledAmp(juce::AudioBuffer<float>& buffer)
    {
        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);

        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            inputHighPass.process(ctx);
        }

        const int uChannels = (int)upsampled.getNumChannels();
        const int uSamples = (int)upsampled.getNumSamples();
        const int channels = juce::jmin(uChannels, preparedChannels, kMaxChannels);

        std::array<float*, kMaxChannels> channelData{};
        for (int ch = 0; ch < channels; ++ch)
            channelData[(size_t)ch] = upsampled.getChannelPointer((size_t)ch);

        for (int sample = 0; sample < uSamples; ++sample)
        {
            const float driveNorm = sanitize01(driveSmooth.getNextValue(), 0.25f);
            const float bass = sanitize01(bassSmooth.getNextValue(), 0.52f);
            const float treble = sanitize01(trebleSmooth.getNextValue(), 0.55f);

            // Drive is intentionally less explosive than the original version.
            // It still gets warm, but it should not feed the amp/reverb with runaway levels.
            const float drive = 1.0f + (driveNorm * 2.10f);
            const float compensation = 1.0f / std::sqrt(1.0f + (driveNorm * 0.85f));

            debugTelemetry.captureControlWindow(drive,
                bass,
                treble,
                reverbSmooth.getCurrentValue(),
                masterSmooth.getCurrentValue());

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = channelData[(size_t)ch];

                float x = inputDcBlocker.process(ch, data[sample]);
                debugTelemetry.capturePreDrive(x);

                x *= drive;
                x = triodeShape(x);
                x *= compensation;

                debugTelemetry.captureSaturation(x);
                data[sample] = x;
            }
        }

        updateToneFilters(false);

        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            bassShelf.process(ctx);
            trebleShelf.process(ctx);
            postSaturationHighPass.process(ctx);
        }

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = channelData[(size_t)ch];

            for (int sample = 0; sample < uSamples; ++sample)
            {
                const float safe = softLimit(data[sample], 1.28f);
                debugTelemetry.captureToneOutput(safe);
                data[sample] = safe;
            }
        }

        oversampler.processSamplesDown(block);

        outputDcBlocker.processBuffer(buffer);
    }

    void processBoundedReverbAndMaster(juce::AudioBuffer<float>& buffer)
    {
        room.process(buffer, reverbSmooth, debugTelemetry);

        const int channels = juce::jmin(buffer.getNumChannels(), preparedChannels, kMaxChannels);
        const int samples = buffer.getNumSamples();

        for (int i = 0; i < samples; ++i)
        {
            const float master = sanitizeLevel(masterSmooth.getNextValue());

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = buffer.getWritePointer(ch);

                const float original = data[i] * master;
                const float safe = softLimit(original, 1.25f);

                debugTelemetry.captureSafety(original, safe);
                debugTelemetry.captureOutput(safe);

                data[i] = safe;
            }
        }
    }

    void emergencySafePass(juce::AudioBuffer<float>& buffer) noexcept
    {
        outputDcBlocker.processBuffer(buffer);

        const int channels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
        const int samples = buffer.getNumSamples();

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);

            for (int i = 0; i < samples; ++i)
            {
                const float original = data[i];
                const float safe = softLimit(original, 0.98f);
                debugTelemetry.captureSafety(original, safe);
                debugTelemetry.captureOutput(safe);
                data[i] = safe;
            }
        }
    }

    void emitTelemetry(juce::AudioBuffer<float>& buffer)
    {
        if (signalTelemetry.captureOutputAndPublishIfNeeded(buffer))
            debugTelemetry.resetWindow();
    }

    juce::dsp::Oversampling<float> oversampler;

    IIRFilter inputHighPass;
    IIRFilter bassShelf;
    IIRFilter trebleShelf;
    IIRFilter postSaturationHighPass;

    MultiChannelDcBlocker inputDcBlocker;
    MultiChannelDcBlocker outputDcBlocker;
    BoundedStudioRoom room;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bassSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> trebleSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterSmooth;

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* bassParam = nullptr;
    juce::AudioParameterFloat* trebleParam = nullptr;
    juce::AudioParameterFloat* reverbParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    double currentInnerRate = 352800.0;

    int preparedBlockSize = 512;
    int preparedChannels = 2;

    float cachedBass = std::numeric_limits<float>::quiet_NaN();
    float cachedTreble = std::numeric_limits<float>::quiet_NaN();

    bool isPrepared = false;

    NovaDiagnostics::PedalSignalTelemetry signalTelemetry{ "clean-amp" };
    DebugTelemetry debugTelemetry;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CleanAmp)
};
