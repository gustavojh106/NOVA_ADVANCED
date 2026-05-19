#pragma once

#include "../Base/ProcessorBase.h"
#include "../../../Core/PedalSignalTelemetry.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace Nova { namespace FlangerDSP {

inline float wrapPhase(float phase) noexcept
{
    phase -= std::floor(phase);
    return phase < 0.0f ? phase + 1.0f : phase;
}

inline float triangleLfo(float phase) noexcept
{
    const float wrapped = wrapPhase(phase);
    if (wrapped < 0.25f)
        return wrapped * 4.0f;
    if (wrapped < 0.75f)
        return 2.0f - wrapped * 4.0f;
    return wrapped * 4.0f - 4.0f;
}

inline float shapedLfo(float phase, float triangleBlend, float harmonicBlend, float contour) noexcept
{
    const float wrapped = wrapPhase(phase);
    const float sine = std::sin(juce::MathConstants<float>::twoPi * wrapped);
    const float triangle = triangleLfo(wrapped);
    const float harmonic = std::sin(juce::MathConstants<float>::twoPi * (wrapped * 2.0f + 0.17f));
    const float raw = juce::jlimit(-1.0f,
        1.0f,
        sine * (1.0f - triangleBlend)
            + triangle * triangleBlend * 0.90f
            + harmonic * harmonicBlend * 0.14f);

    const float drive = 1.0f + juce::jlimit(0.0f, 1.0f, contour) * 1.7f;
    return std::tanh(raw * drive) / std::tanh(drive);
}

inline float msToSamples(float milliseconds, double sampleRate) noexcept
{
    return milliseconds * (float) (sampleRate * 0.001);
}

inline float softClip(float x, float drive) noexcept
{
    return std::tanh(x * drive) / juce::jmax(1.0f, drive);
}

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

    void setLowPass(float freq, float q, double sr) noexcept
    {
        const float clamped = juce::jlimit(20.0f, (float) (sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clamped / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);

        b0 = (1.0f - cosW0) * 0.5f * a0inv;
        b1 = (1.0f - cosW0) * a0inv;
        b2 = b0;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    void setHighPass(float freq, float q, double sr) noexcept
    {
        const float clamped = juce::jlimit(20.0f, (float) (sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clamped / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha);

        b0 = (1.0f + cosW0) * 0.5f * a0inv;
        b1 = -(1.0f + cosW0) * a0inv;
        b2 = b0;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct DelayLine
{
    std::vector<float> buffer;
    int writePos = 0;
    int size = 1;

    void allocate(int maxSamples)
    {
        size = juce::jmax(8, maxSamples);
        buffer.assign((size_t) size, 0.0f);
        writePos = 0;
    }

    void clear()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
    }

    void write(float sample) noexcept
    {
        buffer[(size_t) writePos] = sample;
        if (++writePos >= size)
            writePos = 0;
    }

    float readCubic(float delaySamples) const noexcept
    {
        const float clampedDelay = juce::jlimit(1.0f, (float) (size - 3), delaySamples);
        float readPos = (float) writePos - clampedDelay;
        while (readPos < 0.0f)
            readPos += (float) size;

        const int i1 = ((int) readPos) % size;
        const int i0 = (i1 - 1 + size) % size;
        const int i2 = (i1 + 1) % size;
        const int i3 = (i1 + 2) % size;
        const float frac = readPos - std::floor(readPos);

        const float y0 = buffer[(size_t) i0];
        const float y1 = buffer[(size_t) i1];
        const float y2 = buffer[(size_t) i2];
        const float y3 = buffer[(size_t) i3];

        const float a = y3 - y2 - y0 + y1;
        const float b = y0 - y1 - a;
        const float c = y2 - y0;
        return y1 + frac * (c + frac * (b + frac * a));
    }
};

struct ModeConfig
{
    float triangleBlend = 0.0f;
    float harmonicBlend = 0.0f;
    float contour = 0.0f;
    float excursionScale = 1.0f;
    float centreOffsetMs = 0.0f;
    float stereoPhase = 0.0f;
    float stereoDelayMs = 0.0f;
    float inputHighPassHz = 70.0f;
    float regenLowPassScale = 0.80f;
    float wetTrim = 1.0f;
    float sideGain = 0.5f;
    float crossFeedback = 0.0f;
    float wetPolarity = 1.0f;
    float writeDrive = 0.0f;
    float wetDrive = 0.0f;
};

inline ModeConfig makeClassicConfig() noexcept
{
    ModeConfig config;
    config.triangleBlend = 0.10f;
    config.harmonicBlend = 0.06f;
    config.contour = 0.12f;
    config.excursionScale = 1.00f;
    config.centreOffsetMs = 0.00f;
    config.stereoPhase = 0.10f;
    config.stereoDelayMs = 0.16f;
    config.inputHighPassHz = 70.0f;
    config.regenLowPassScale = 0.82f;
    config.wetTrim = 0.98f;
    config.sideGain = 0.52f;
    config.crossFeedback = 0.04f;
    config.wetPolarity = 1.0f;
    config.writeDrive = 0.16f;
    config.wetDrive = 0.10f;
    return config;
}

inline ModeConfig makeJetConfig() noexcept
{
    ModeConfig config;
    config.triangleBlend = 0.26f;
    config.harmonicBlend = 0.14f;
    config.contour = 0.30f;
    config.excursionScale = 1.18f;
    config.centreOffsetMs = 0.28f;
    config.stereoPhase = 0.18f;
    config.stereoDelayMs = 0.42f;
    config.inputHighPassHz = 82.0f;
    config.regenLowPassScale = 0.72f;
    config.wetTrim = 1.02f;
    config.sideGain = 0.78f;
    config.crossFeedback = 0.08f;
    config.wetPolarity = 1.0f;
    config.writeDrive = 0.22f;
    config.wetDrive = 0.15f;
    return config;
}

inline ModeConfig makeZeroConfig() noexcept
{
    ModeConfig config;
    config.triangleBlend = 0.08f;
    config.harmonicBlend = 0.20f;
    config.contour = 0.42f;
    config.excursionScale = 1.28f;
    config.centreOffsetMs = -0.18f;
    config.stereoPhase = 0.14f;
    config.stereoDelayMs = 0.26f;
    config.inputHighPassHz = 95.0f;
    config.regenLowPassScale = 0.67f;
    config.wetTrim = 0.94f;
    config.sideGain = 0.60f;
    config.crossFeedback = 0.10f;
    config.wetPolarity = -1.0f;
    config.writeDrive = 0.24f;
    config.wetDrive = 0.14f;
    return config;
}

inline ModeConfig getModeConfig(int modeIndex) noexcept
{
    switch (modeIndex)
    {
        case 1:  return makeJetConfig();
        case 2:  return makeZeroConfig();
        default: return makeClassicConfig();
    }
}

}} // namespace Nova::FlangerDSP

class FlangerPedal final : public ProcessorBase
{
public:
    FlangerPedal()
    {
        addParameter(rateParam = new juce::AudioParameterFloat("flangerRate", "Rate", 0.03f, 5.5f, 0.32f));
        addParameter(depthParam = new juce::AudioParameterFloat("flangerDepth", "Depth", 0.0f, 1.0f, 0.72f));
        addParameter(manualParam = new juce::AudioParameterFloat("flangerManual", "Manual", 0.0f, 1.0f, 0.34f));
        addParameter(feedbackParam = new juce::AudioParameterFloat("flangerFeedback", "Feedback", -0.95f, 0.95f, 0.42f));
        addParameter(widthParam = new juce::AudioParameterFloat("flangerWidth", "Width", 0.0f, 1.0f, 0.68f));
        addParameter(toneParam = new juce::AudioParameterFloat("flangerTone", "Tone", 1000.0f, 14000.0f, 7800.0f));
        addParameter(mixParam = new juce::AudioParameterFloat("flangerMix", "Mix", 0.0f, 1.0f, 0.46f));
        addParameter(modeParam = new juce::AudioParameterChoice("flangerMode",
            "Mode",
            juce::StringArray{ "Classic", "Jet", "Zero" },
            0));
    }

    const juce::String getName() const override { return "Flanger"; }
    double getTailLengthSeconds() const override { return 0.35; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        const int maxDelaySamples = (int) Nova::FlangerDSP::msToSamples(12.5f, sr) + 128;
        for (auto& delay : delayLines)
            delay.allocate(maxDelaySamples);

        rateSmooth.reset(sr, 0.04);
        depthSmooth.reset(sr, 0.05);
        manualSmooth.reset(sr, 0.05);
        feedbackSmooth.reset(sr, 0.05);
        widthSmooth.reset(sr, 0.05);
        mixSmooth.reset(sr, 0.04);

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? rateParam->get() : 0.32f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? depthParam->get() : 0.72f);
        manualSmooth.setCurrentAndTargetValue(manualParam != nullptr ? manualParam->get() : 0.34f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? feedbackParam->get() : 0.42f);
        widthSmooth.setCurrentAndTargetValue(widthParam != nullptr ? widthParam->get() : 0.68f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 0.46f);

        for (auto& filter : inputHPF)
            filter.reset();
        for (auto& filter : regenLPF)
            filter.reset();
        for (auto& filter : wetLPF)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();
        for (auto& filter : feedbackLoopDCBlock)
            filter.reset();

        updateFilters(true);
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
        for (auto& delay : delayLines)
            delay.clear();
        for (auto& filter : inputHPF)
            filter.reset();
        for (auto& filter : regenLPF)
            filter.reset();
        for (auto& filter : wetLPF)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();
        for (auto& filter : feedbackLoopDCBlock)
            filter.reset();

        feedbackState.fill(0.0f);
        lastDelayMs.fill(manualToCentreDelayMs(manualParam != nullptr ? manualParam->get() : 0.34f));
        lfoPhase = 0.0f;
        driftPhase = 0.0f;

        rateSmooth.setCurrentAndTargetValue(rateParam != nullptr ? rateParam->get() : 0.32f);
        depthSmooth.setCurrentAndTargetValue(depthParam != nullptr ? depthParam->get() : 0.72f);
        manualSmooth.setCurrentAndTargetValue(manualParam != nullptr ? manualParam->get() : 0.34f);
        feedbackSmooth.setCurrentAndTargetValue(feedbackParam != nullptr ? feedbackParam->get() : 0.42f);
        widthSmooth.setCurrentAndTargetValue(widthParam != nullptr ? widthParam->get() : 0.68f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 0.46f);

        signalTelemetry.reset();
        debugTelemetry.resetWindow();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        signalTelemetry.captureInput(buffer);
        scrubInvalidSamples(buffer);

        rateSmooth.setTargetValue(rateParam != nullptr ? rateParam->get() : 0.32f);
        depthSmooth.setTargetValue(depthParam != nullptr ? depthParam->get() : 0.72f);
        manualSmooth.setTargetValue(manualParam != nullptr ? manualParam->get() : 0.34f);
        feedbackSmooth.setTargetValue(feedbackParam != nullptr ? feedbackParam->get() : 0.42f);
        widthSmooth.setTargetValue(widthParam != nullptr ? widthParam->get() : 0.68f);
        mixSmooth.setTargetValue(mixParam != nullptr ? mixParam->get() : 0.46f);
        updateFilters();

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();
        const auto config = Nova::FlangerDSP::getModeConfig(modeParam != nullptr ? modeParam->getIndex() : 0);
        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float rate = rateSmooth.getNextValue();
            const float depth = depthSmooth.getNextValue();
            const float manual = manualSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float width = widthSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();
            const float centreDelayMs = manualToCentreDelayMs(manual) + config.centreOffsetMs;
            const float excursionMs = (0.18f + depth * 4.75f) * config.excursionScale;

            std::array<float, 2> dry { 0.0f, 0.0f };
            std::array<float, 2> preFiltered { 0.0f, 0.0f };
            std::array<float, 2> wetRaw { 0.0f, 0.0f };
            std::array<float, 2> dampedWet { 0.0f, 0.0f };
            std::array<float, 2> feedbackSeed { 0.0f, 0.0f };
            std::array<float, 2> feedbackInject { 0.0f, 0.0f };
            std::array<float, 2> writeSignal { 0.0f, 0.0f };

            for (int ch = 0; ch < numChannels; ++ch)
            {
                dry[(size_t) ch] = buffer.getSample(ch, sample);
                preFiltered[(size_t) ch] = inputHPF[(size_t) ch].process(dry[(size_t) ch]);
            }

            if (numChannels == 1)
            {
                dry[1] = dry[0];
                preFiltered[1] = preFiltered[0];
            }

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float stereoSign = ch == 0 ? -1.0f : 1.0f;
                const float phase = Nova::FlangerDSP::wrapPhase(lfoPhase + stereoSign * width * config.stereoPhase);
                const float drift = 0.08f * std::sin(twoPi
                    * (driftPhase * (0.43f + 0.05f * (float) ch) + 0.11f * (float) ch));
                const float flutter = 0.022f * std::sin(twoPi
                    * (phase * (2.10f + 0.17f * (float) ch) + 0.19f));
                const float lfo = Nova::FlangerDSP::shapedLfo(
                    phase,
                    config.triangleBlend,
                    config.harmonicBlend,
                    config.contour);
                const float delayMs = juce::jlimit(0.35f,
                    12.0f,
                    centreDelayMs
                        + stereoSign * width * config.stereoDelayMs
                        + excursionMs * (lfo + drift + flutter));
                const float delaySamples = Nova::FlangerDSP::msToSamples(delayMs, sr);
                const float tapped = delayLines[(size_t) ch].readCubic(delaySamples);
                const float damped = regenLPF[(size_t) ch].process(tapped);
                const float dcSafeFeedback = feedbackLoopDCBlock[(size_t) ch].process(damped);
                float wet = wetLPF[(size_t) ch].process(tapped);
                wet = dcBlock[(size_t) ch].process(Nova::FlangerDSP::softClip(
                    wet * config.wetTrim,
                    1.0f + config.wetDrive + std::abs(feedback) * 0.18f + depth * 0.12f));

                const float localFeedback = feedbackState[(size_t) ch] * feedback;
                const float crossFeedback = (numChannels > 1
                    ? feedbackState[(size_t) (1 - ch)]
                    : feedbackState[(size_t) ch])
                    * config.crossFeedback
                    * width
                    * feedback;

                const float injectedFeedback = localFeedback + crossFeedback;
                const float delayWrite = Nova::FlangerDSP::softClip(
                    preFiltered[(size_t) ch] + localFeedback + crossFeedback,
                    1.02f + config.writeDrive + std::abs(feedback) * 0.24f + depth * 0.16f);
                delayLines[(size_t) ch].write(delayWrite);

                dampedWet[(size_t) ch] = damped;
                feedbackSeed[(size_t) ch] = juce::jlimit(-1.15f, 1.15f, dcSafeFeedback);
                feedbackInject[(size_t) ch] = injectedFeedback;
                writeSignal[(size_t) ch] = delayWrite;
                wetRaw[(size_t) ch] = wet * config.wetPolarity;
                lastDelayMs[(size_t) ch] = delayMs;
            }

            if (numChannels > 1)
            {
                const float mid = 0.5f * (wetRaw[0] + wetRaw[1]);
                float side = 0.5f * (wetRaw[0] - wetRaw[1]);
                side *= 0.34f + width * config.sideGain;
                wetRaw[0] = mid + side;
                wetRaw[1] = mid - side;
            }
            else
            {
                wetRaw[1] = wetRaw[0];
            }

            for (int ch = 0; ch < numChannels; ++ch)
            {
                feedbackState[(size_t) ch] = feedbackSeed[(size_t) ch] * (0.88f - 0.12f * mix);
                debugTelemetry.captureChannel(ch,
                    lastDelayMs[(size_t) ch],
                    wetRaw[(size_t) ch],
                    dampedWet[(size_t) ch],
                    feedbackSeed[(size_t) ch],
                    feedbackState[(size_t) ch],
                    feedbackInject[(size_t) ch],
                    writeSignal[(size_t) ch]);
            }

            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample(ch, sample, dry[(size_t) ch] * dryGain + wetRaw[(size_t) ch] * wetGain);

            lfoPhase += rate / (float) sr;
            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;

            driftPhase += (0.032f + rate * 0.018f) / (float) sr;
            if (driftPhase >= 1.0f)
                driftPhase -= 1.0f;
        }

        if (signalTelemetry.captureOutputAndPublishIfNeeded(buffer))
            debugTelemetry.resetWindow();

        endBypassProcess(buffer);
    }

    static float manualToCentreDelayMs(float manual) noexcept
    {
        return 0.45f + juce::jlimit(0.0f, 1.0f, manual) * 5.15f;
    }

    static juce::String getModeDescription(int modeIndex)
    {
        switch (modeIndex)
        {
            case 1:  return "Aggressive jet sweep with hotter stereo spread and sharper comb motion";
            case 2:  return "Null-heavy zero style voicing with inverted wet polarity and deeper hollow sweep";
            default: return "Balanced analog-inspired flange with musical feedback and mono-safe centre";
        }
    }

    juce::AudioParameterFloat* rateParam = nullptr;
    juce::AudioParameterFloat* depthParam = nullptr;
    juce::AudioParameterFloat* manualParam = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* widthParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterChoice* modeParam = nullptr;
    float lfoPhase = 0.0f;
    std::array<float, 2> lastDelayMs {};

private:
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

    struct DebugTelemetry
    {
        std::array<float, 2> delayMsMin{};
        std::array<float, 2> delayMsMax{};
        std::array<float, 2> wetPeak{};
        std::array<float, 2> dampedPeak{};
        std::array<float, 2> feedbackSeedPeak{};
        std::array<float, 2> feedbackStatePeak{};
        std::array<float, 2> feedbackInjectPeak{};
        std::array<float, 2> writePeak{};

        void resetWindow() noexcept
        {
            delayMsMin = { { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() } };
            delayMsMax = {};
            wetPeak = {};
            dampedPeak = {};
            feedbackSeedPeak = {};
            feedbackStatePeak = {};
            feedbackInjectPeak = {};
            writePeak = {};
        }

        void captureChannel(int channel,
            float delayMs,
            float wet,
            float damped,
            float feedbackSeed,
            float feedbackStateValue,
            float feedbackInject,
            float delayWrite) noexcept
        {
            const auto idx = (size_t) juce::jlimit(0, 1, channel);
            delayMsMin[idx] = juce::jmin(delayMsMin[idx], delayMs);
            delayMsMax[idx] = juce::jmax(delayMsMax[idx], delayMs);
            wetPeak[idx] = juce::jmax(wetPeak[idx], std::abs(wet));
            dampedPeak[idx] = juce::jmax(dampedPeak[idx], std::abs(damped));
            feedbackSeedPeak[idx] = juce::jmax(feedbackSeedPeak[idx], std::abs(feedbackSeed));
            feedbackStatePeak[idx] = juce::jmax(feedbackStatePeak[idx], std::abs(feedbackStateValue));
            feedbackInjectPeak[idx] = juce::jmax(feedbackInjectPeak[idx], std::abs(feedbackInject));
            writePeak[idx] = juce::jmax(writePeak[idx], std::abs(delayWrite));
        }

        juce::String buildReport(const FlangerPedal& pedal) const
        {
            juce::String report;
            report << "params: mode="
                   << (pedal.modeParam != nullptr ? pedal.modeParam->getCurrentChoiceName() : "Classic")
                   << ", rate=" << NovaDiagnostics::formatTelemetryScalar(pedal.rateParam != nullptr ? pedal.rateParam->get() : 0.32f)
                   << ", depth=" << NovaDiagnostics::formatTelemetryScalar(pedal.depthParam != nullptr ? pedal.depthParam->get() : 0.72f)
                   << ", manual=" << NovaDiagnostics::formatTelemetryScalar(pedal.manualParam != nullptr ? pedal.manualParam->get() : 0.34f)
                   << ", feedback=" << NovaDiagnostics::formatTelemetryScalar(pedal.feedbackParam != nullptr ? pedal.feedbackParam->get() : 0.42f)
                   << ", width=" << NovaDiagnostics::formatTelemetryScalar(pedal.widthParam != nullptr ? pedal.widthParam->get() : 0.68f)
                   << ", tone=" << NovaDiagnostics::formatTelemetryScalar(pedal.toneParam != nullptr ? pedal.toneParam->get() : 7800.0f)
                   << ", mix=" << NovaDiagnostics::formatTelemetryScalar(pedal.mixParam != nullptr ? pedal.mixParam->get() : 0.46f)
                   << juce::newLine
                   << "delay.window: delayLMinMs=" << NovaDiagnostics::formatTelemetryScalar(delayMsMin[0] == std::numeric_limits<float>::max() ? 0.0f : delayMsMin[0])
                   << ", delayLMaxMs=" << NovaDiagnostics::formatTelemetryScalar(delayMsMax[0])
                   << ", delayRMinMs=" << NovaDiagnostics::formatTelemetryScalar(delayMsMin[1] == std::numeric_limits<float>::max() ? 0.0f : delayMsMin[1])
                   << ", delayRMaxMs=" << NovaDiagnostics::formatTelemetryScalar(delayMsMax[1])
                   << juce::newLine
                   << "feedback.loop: seedLMax=" << NovaDiagnostics::formatTelemetryScalar(feedbackSeedPeak[0])
                   << ", seedRMax=" << NovaDiagnostics::formatTelemetryScalar(feedbackSeedPeak[1])
                   << ", stateLMax=" << NovaDiagnostics::formatTelemetryScalar(feedbackStatePeak[0])
                   << ", stateRMax=" << NovaDiagnostics::formatTelemetryScalar(feedbackStatePeak[1])
                   << ", injectLMax=" << NovaDiagnostics::formatTelemetryScalar(feedbackInjectPeak[0])
                   << ", injectRMax=" << NovaDiagnostics::formatTelemetryScalar(feedbackInjectPeak[1])
                   << ", writeLMax=" << NovaDiagnostics::formatTelemetryScalar(writePeak[0])
                   << ", writeRMax=" << NovaDiagnostics::formatTelemetryScalar(writePeak[1])
                   << juce::newLine
                   << "wet.path: wetLMax=" << NovaDiagnostics::formatTelemetryScalar(wetPeak[0])
                   << ", wetRMax=" << NovaDiagnostics::formatTelemetryScalar(wetPeak[1])
                   << ", dampedLMax=" << NovaDiagnostics::formatTelemetryScalar(dampedPeak[0])
                   << ", dampedRMax=" << NovaDiagnostics::formatTelemetryScalar(dampedPeak[1]);
            return report;
        }
    };

    void updateFilters(bool force = false)
    {
        const float tone = toneParam != nullptr ? toneParam->get() : 7800.0f;
        const int modeIndex = modeParam != nullptr ? modeParam->getIndex() : 0;

        if (!force
            && std::abs(cachedTone - tone) < 0.5f
            && cachedMode == modeIndex)
            return;

        cachedTone = tone;
        cachedMode = modeIndex;

        const auto config = Nova::FlangerDSP::getModeConfig(modeIndex);
        const float wetCutoff = juce::jlimit(900.0f, (float) (sr * 0.45), tone);
        const float regenCutoff = juce::jlimit(600.0f, (float) (sr * 0.40), tone * config.regenLowPassScale);

        for (auto& filter : inputHPF)
            filter.setHighPass(config.inputHighPassHz, 0.707f, sr);
        for (auto& filter : regenLPF)
            filter.setLowPass(regenCutoff, 0.65f, sr);
        for (auto& filter : wetLPF)
            filter.setLowPass(wetCutoff, 0.62f, sr);
        for (auto& filter : dcBlock)
            filter.setHighPass(20.0f, 0.707f, sr);
        for (auto& filter : feedbackLoopDCBlock)
            filter.setHighPass(28.0f, 0.707f, sr);
    }

    double sr = 44100.0;

    std::array<Nova::FlangerDSP::DelayLine, 2> delayLines;
    std::array<Nova::FlangerDSP::Biquad, 2> inputHPF;
    std::array<Nova::FlangerDSP::Biquad, 2> regenLPF;
    std::array<Nova::FlangerDSP::Biquad, 2> wetLPF;
    std::array<Nova::FlangerDSP::Biquad, 2> dcBlock;
    std::array<Nova::FlangerDSP::Biquad, 2> feedbackLoopDCBlock;
    std::array<float, 2> feedbackState {};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> manualSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    float driftPhase = 0.0f;
    float cachedTone = -1.0f;
    int cachedMode = -1;
    bool isPrepared = false;
    NovaDiagnostics::PedalSignalTelemetry signalTelemetry { "flanger" };
    DebugTelemetry debugTelemetry;
};

#include "FlangerEditor.h"
