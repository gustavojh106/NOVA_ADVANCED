#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>
#include <vector>

#include "../../Core/PedalSignalTelemetry.h"
#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"

class CleanAmp final : public ProcessorBase
{
public:
    CleanAmp()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("cleanDrive", "Drive", 0.0f, 1.0f, 0.25f));
        addParameter(bassParam = new juce::AudioParameterFloat("cleanBass", "Bass", 0.0f, 1.0f, 0.52f));
        addParameter(trebleParam = new juce::AudioParameterFloat("cleanTreble", "Treble", 0.0f, 1.0f, 0.55f));
        addParameter(reverbParam = new juce::AudioParameterFloat("cleanReverb", "Reverb", 0.0f, 1.0f, 0.2f));
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
                { "Drive", driveParam, [](float value) { return formatPercent(value); } },
                { "Bass", bassParam, [](float value) { return formatPercent(value); } },
                { "Treble", trebleParam, [](float value) { return formatPercent(value); } },
                { "Reverb", reverbParam, [](float value) { return formatPercent(value); } },
                { "Master", levelParam, [](float value) { return formatGain(value); } }
            },
            214,
            178);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;

        oversampler.reset();
        oversampler.initProcessing((size_t)juce::jmax(1, samplesPerBlock));

        currentInnerRate = sampleRate * 8.0;

        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate = currentInnerRate;
        innerSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock * 8);
        innerSpec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        inputHighPass.prepare(innerSpec);
        bassShelf.prepare(innerSpec);
        trebleShelf.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        juce::dsp::ProcessSpec baseSpec;
        baseSpec.sampleRate = sampleRate;
        baseSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock);
        baseSpec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());
        reverb.prepare(baseSpec);

        driveSmooth.reset(currentInnerRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        reverbSmooth.reset(sampleRate, 0.04);
        masterSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 0.25f);
        reverbSmooth.setCurrentAndTargetValue(reverbParam != nullptr ? *reverbParam : 0.2f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        wetBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock), false, false, true);

        cachedBass = std::numeric_limits<float>::quiet_NaN();
        cachedTreble = std::numeric_limits<float>::quiet_NaN();

        setProcessingLatency((int)oversampler.getLatencyInSamples());
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        signalTelemetry.reset();
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
        dcBlock.reset();
        reverb.reset();

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 0.25f);
        reverbSmooth.setCurrentAndTargetValue(reverbParam != nullptr ? *reverbParam : 0.2f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
        signalTelemetry.reset();
        debugTelemetry.resetWindow();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;
        signalTelemetry.captureInput(buffer);

        updateToneIfNeeded();
        driveSmooth.setTargetValue(driveParam != nullptr ? *driveParam : 0.25f);
        reverbSmooth.setTargetValue(reverbParam != nullptr ? *reverbParam : 0.2f);
        masterSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
        updateReverbIfNeeded();

        // --- Oversampled saturation + tone ---
        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);

        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            inputHighPass.process(ctx);
        }

        // Gentle tube warmth — soft single-stage saturation at 4x rate
        const int uChannels = (int)upsampled.getNumChannels();
        const int uSamples = (int)upsampled.getNumSamples();
        std::vector<float*> channelData((size_t)uChannels);
        for (int ch = 0; ch < uChannels; ++ch)
            channelData[(size_t) ch] = upsampled.getChannelPointer((size_t) ch);

        for (int sample = 0; sample < uSamples; ++sample)
        {
            const float drive = 1.0f + driveSmooth.getNextValue() * 3.0f;
            debugTelemetry.captureControlWindow(drive,
                reverbSmooth.getCurrentValue(),
                masterSmooth.getCurrentValue());

            for (int ch = 0; ch < uChannels; ++ch)
            {
                auto* data = channelData[(size_t) ch];
                debugTelemetry.capturePreDrive(data[sample]);
                float x = data[sample] * drive;
                x = std::tanh(x * 0.8f) * 1.15f;
                debugTelemetry.captureSaturation(x);
                data[sample] = x;
            }
        }

        {
            juce::dsp::ProcessContextReplacing<float> ctx(upsampled);
            bassShelf.process(ctx);
            trebleShelf.process(ctx);
            dcBlock.process(ctx);
        }

        for (int ch = 0; ch < uChannels; ++ch)
        {
            auto* data = upsampled.getChannelPointer((size_t) ch);
            for (int sample = 0; sample < uSamples; ++sample)
                debugTelemetry.captureToneOutput(data[sample]);
        }

        oversampler.processSamplesDown(block);

        // --- Spring reverb (at base rate, post-oversampling) ---
        if (wetBuffer.getNumSamples() < buffer.getNumSamples())
            wetBuffer.setSize(buffer.getNumChannels(), buffer.getNumSamples(), false, false, true);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(wetBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch), buffer.getNumSamples());

        juce::dsp::AudioBlock<float> wetBlock(wetBuffer);
        auto wetSub = wetBlock.getSubBlock(0, (size_t)buffer.getNumSamples());
        juce::dsp::ProcessContextReplacing<float> wetContext(wetSub);
        reverb.process(wetContext);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float reverbMix = reverbSmooth.getNextValue();
            const float master = masterSmooth.getNextValue();

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float drySignal = buffer.getSample(ch, sample);
                const float wetSignal = wetBuffer.getSample(ch, sample);
                debugTelemetry.captureReverbReturn(wetSignal);
                const float output = (drySignal + wetSignal * reverbMix) * master;
                debugTelemetry.captureOutput(output);
                buffer.setSample(ch, sample, output);
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
        float saturationPeak = 0.0f;
        float tonePeak = 0.0f;
        float reverbReturnPeak = 0.0f;
        float outputPeak = 0.0f;
        float driveMin = 1.0e9f;
        float driveMax = 0.0f;
        float reverbMixMin = 1.0e9f;
        float reverbMixMax = 0.0f;
        float masterMin = 1.0e9f;
        float masterMax = 0.0f;

        void resetWindow() noexcept
        {
            preDrivePeak = 0.0f;
            saturationPeak = 0.0f;
            tonePeak = 0.0f;
            reverbReturnPeak = 0.0f;
            outputPeak = 0.0f;
            driveMin = 1.0e9f;
            driveMax = 0.0f;
            reverbMixMin = 1.0e9f;
            reverbMixMax = 0.0f;
            masterMin = 1.0e9f;
            masterMax = 0.0f;
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

        void captureControlWindow(float drive, float reverbMix, float master) noexcept
        {
            driveMin = juce::jmin(driveMin, drive);
            driveMax = juce::jmax(driveMax, drive);
            reverbMixMin = juce::jmin(reverbMixMin, reverbMix);
            reverbMixMax = juce::jmax(reverbMixMax, reverbMix);
            masterMin = juce::jmin(masterMin, master);
            masterMax = juce::jmax(masterMax, master);
        }

        juce::String buildReport(const CleanAmp& amp) const
        {
            juce::String report;
            report << "params: drive=" << NovaDiagnostics::formatTelemetryScalar(amp.driveParam != nullptr ? amp.driveParam->get() : 0.25f)
                   << ", bass=" << NovaDiagnostics::formatTelemetryScalar(amp.bassParam != nullptr ? amp.bassParam->get() : 0.52f)
                   << ", treble=" << NovaDiagnostics::formatTelemetryScalar(amp.trebleParam != nullptr ? amp.trebleParam->get() : 0.55f)
                   << ", reverb=" << NovaDiagnostics::formatTelemetryScalar(amp.reverbParam != nullptr ? amp.reverbParam->get() : 0.2f)
                   << ", level=" << NovaDiagnostics::formatTelemetryScalar(amp.levelParam != nullptr ? amp.levelParam->get() : 1.0f)
                   << ", innerSampleRate=" << NovaDiagnostics::formatTelemetryScalar((float) amp.currentInnerRate)
                   << ", oversamplingFactor=8"
                   << juce::newLine
                   << "amp.path: preDrivePeak=" << NovaDiagnostics::formatTelemetryScalar(preDrivePeak)
                   << ", saturationPeak=" << NovaDiagnostics::formatTelemetryScalar(saturationPeak)
                   << ", tonePeak=" << NovaDiagnostics::formatTelemetryScalar(tonePeak)
                   << ", outputPeak=" << NovaDiagnostics::formatTelemetryScalar(outputPeak)
                   << juce::newLine
                   << "reverb.path: returnPeak=" << NovaDiagnostics::formatTelemetryScalar(reverbReturnPeak)
                   << juce::newLine
                   << "control.window: driveMin=" << NovaDiagnostics::formatTelemetryScalar(driveMin == 1.0e9f ? 0.0f : driveMin)
                   << ", driveMax=" << NovaDiagnostics::formatTelemetryScalar(driveMax)
                   << ", reverbMixMin=" << NovaDiagnostics::formatTelemetryScalar(reverbMixMin == 1.0e9f ? 0.0f : reverbMixMin)
                   << ", reverbMixMax=" << NovaDiagnostics::formatTelemetryScalar(reverbMixMax)
                   << ", masterMin=" << NovaDiagnostics::formatTelemetryScalar(masterMin == 1.0e9f ? 0.0f : masterMin)
                   << ", masterMax=" << NovaDiagnostics::formatTelemetryScalar(masterMax);
            return report;
        }
    };

    using IIRFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void updateToneIfNeeded()
    {
        const float bass = bassParam != nullptr ? *bassParam : 0.52f;
        const float treble = trebleParam != nullptr ? *trebleParam : 0.55f;

        const bool bassChanged = !std::isfinite(cachedBass) || std::abs(cachedBass - bass) > 1.0e-4f;
        const bool trebleChanged = !std::isfinite(cachedTreble) || std::abs(cachedTreble - treble) > 1.0e-4f;
        if (!bassChanged && !trebleChanged)
            return;

        cachedBass = bass;
        cachedTreble = treble;

        const float bassGain = juce::jmap(bass, -6.0f, 8.0f);
        const float trebleGain = juce::jmap(treble, -6.0f, 8.0f);

        *inputHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 40.0f);
        *bassShelf.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentInnerRate,
            280.0f, 0.72f, juce::Decibels::decibelsToGain(bassGain));
        *trebleShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentInnerRate,
            2800.0f, 0.68f, juce::Decibels::decibelsToGain(trebleGain));
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 18.0f);
    }

    void updateReverbIfNeeded()
    {
        const float reverbAmount = reverbParam != nullptr ? *reverbParam : 0.2f;

        juce::dsp::Reverb::Parameters params;
        params.roomSize = 0.35f + reverbAmount * 0.25f;
        params.damping = 0.6f;
        params.width = 0.7f;
        params.freezeMode = 0.0f;
        params.wetLevel = 1.0f;
        params.dryLevel = 0.0f;
        reverb.setParameters(params);
    }

    juce::dsp::Oversampling<float> oversampler;
    IIRFilter inputHighPass;
    IIRFilter bassShelf;
    IIRFilter trebleShelf;
    IIRFilter dcBlock;
    juce::dsp::Reverb reverb;
    juce::AudioBuffer<float> wetBuffer;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterSmooth;

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* bassParam = nullptr;
    juce::AudioParameterFloat* trebleParam = nullptr;
    juce::AudioParameterFloat* reverbParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    double currentInnerRate = 352800.0;
    float cachedBass = std::numeric_limits<float>::quiet_NaN();
    float cachedTreble = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
    NovaDiagnostics::PedalSignalTelemetry signalTelemetry { "clean-amp" };
    DebugTelemetry debugTelemetry;
};
