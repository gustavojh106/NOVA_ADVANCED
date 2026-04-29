#pragma once

#include "../Base/ProcessorBase.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>

namespace Nova { namespace FuzzDSP {

inline float blend(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
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

    void setLowShelf(float freq, float gainDb, float q, double sr) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;
        const float a0inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha);

        b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha) * a0inv;
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0) * a0inv;
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0) * a0inv;
        a2 = ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha) * a0inv;
    }

    void setPeak(float freq, float gainDb, float q, double sr) noexcept
    {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * juce::jlimit(20.0f, (float) (sr * 0.45), freq) / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * q);
        const float a0inv = 1.0f / (1.0f + alpha / A);

        b0 = (1.0f + alpha * A) * a0inv;
        b1 = -2.0f * cosW0 * a0inv;
        b2 = (1.0f - alpha * A) * a0inv;
        a1 = b1;
        a2 = (1.0f - alpha / A) * a0inv;
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

inline float asymExpClip(float x, float posShape, float negShape, float negScale) noexcept
{
    if (x >= 0.0f)
        return 1.0f - std::exp(-x * posShape);
    return -(1.0f - std::exp(x * negShape)) * negScale;
}

inline float starvedClip(float x, float threshold, float slope) noexcept
{
    if (x > threshold)
        return threshold + std::tanh((x - threshold) * slope) * 0.65f;
    if (x < -threshold)
        return -threshold + std::tanh((x + threshold) * slope) * 0.55f;
    return x;
}

inline float smoothSquare(float x, float drive) noexcept
{
    return std::tanh(x * drive);
}

struct ModeConfig
{
    float preMidFreq = 900.0f;
    float preMidGainDb = 2.0f;
    float bodyFreq = 150.0f;
    float bodyGainDb = 0.0f;
    float contourFreq = 1000.0f;
    float contourGainDb = 0.0f;
    float octaveBlend = 0.0f;
    float sagAmount = 0.0f;
    float gateBias = 0.0f;
    float sputterAmount = 0.0f;
    float outputTrim = 1.0f;
};

inline ModeConfig makeVintageConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 1100.0f;
    config.preMidGainDb = 2.2f;
    config.bodyFreq = 160.0f;
    config.bodyGainDb = 1.2f;
    config.contourFreq = 1200.0f;
    config.contourGainDb = 1.4f;
    config.octaveBlend = 0.10f;
    config.sagAmount = 0.22f;
    config.gateBias = 0.06f;
    config.sputterAmount = 0.05f;
    config.outputTrim = 0.92f;
    return config;
}

inline ModeConfig makeMuffConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 720.0f;
    config.preMidGainDb = 1.0f;
    config.bodyFreq = 130.0f;
    config.bodyGainDb = 2.6f;
    config.contourFreq = 950.0f;
    config.contourGainDb = -3.0f;
    config.octaveBlend = 0.03f;
    config.sagAmount = 0.08f;
    config.gateBias = -0.05f;
    config.sputterAmount = 0.0f;
    config.outputTrim = 0.98f;
    return config;
}

inline ModeConfig makeVelcroConfig() noexcept
{
    ModeConfig config;
    config.preMidFreq = 1450.0f;
    config.preMidGainDb = 3.4f;
    config.bodyFreq = 180.0f;
    config.bodyGainDb = -1.4f;
    config.contourFreq = 1600.0f;
    config.contourGainDb = 2.2f;
    config.octaveBlend = 0.24f;
    config.sagAmount = 0.34f;
    config.gateBias = 0.20f;
    config.sputterAmount = 0.18f;
    config.outputTrim = 0.88f;
    return config;
}

inline ModeConfig getModeConfig(int modeIndex) noexcept
{
    switch (modeIndex)
    {
        case 1:  return makeMuffConfig();
        case 2:  return makeVelcroConfig();
        default: return makeVintageConfig();
    }
}

}} // namespace Nova::FuzzDSP

class FuzzPedal final : public ProcessorBase
{
public:
    FuzzPedal()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(fuzzParam = new juce::AudioParameterFloat("fuzzAmount", "Fuzz", 0.0f, 100.0f, 67.0f));
        addParameter(toneParam = new juce::AudioParameterFloat("fuzzTone", "Tone", 0.0f, 1.0f, 0.48f));
        addParameter(gateParam = new juce::AudioParameterFloat("fuzzGate", "Gate", 0.0f, 1.0f, 0.28f));
        addParameter(mixParam = new juce::AudioParameterFloat("fuzzMix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("fuzzLevel", "Level", 0.0f, 1.0f, 0.58f));
        addParameter(biasParam = new juce::AudioParameterFloat("fuzzBias", "Bias", 0.0f, 1.0f, 0.56f));
        addParameter(modeParam = new juce::AudioParameterChoice("fuzzMode",
            "Mode",
            juce::StringArray{ "Vintage", "Muff", "Velcro" },
            0));
    }

    const juce::String getName() const override { return "Fuzz"; }
    double getTailLengthSeconds() const override { return 0.07; }

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

        innerRate = sampleRate * 8.0;

        fuzzSmooth.reset(sampleRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        gateSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        mixSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        biasSmooth.reset(sampleRate, 0.05);

        fuzzSmooth.setCurrentAndTargetValue(fuzzParam != nullptr ? fuzzParam->get() : 67.0f);
        gateSmooth.setCurrentAndTargetValue(gateParam != nullptr ? gateParam->get() : 0.28f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? snapMixTarget(mixParam->get()) : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 0.58f);
        biasSmooth.setCurrentAndTargetValue(biasParam != nullptr ? biasParam->get() : 0.56f);

        scratchBuffer.setSize(preparedChannels,
            preparedBlockSize,
            false,
            false,
            true);

        for (auto& filter : inputHP)
            filter.reset();
        for (auto& filter : preContour)
            filter.reset();
        for (auto& filter : bodyShelf)
            filter.reset();
        for (auto& filter : toneLP)
            filter.reset();
        for (auto& filter : contourPeak)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();

        inputEnvelope.fill(0.0f);
        sagEnvelope.fill(0.0f);
        gateGain.fill(0.0f);
        gateOpen.fill(false);
        displaySag = 0.0f;
        driftPhase = 0.0f;
        sputterPhase = 0.0f;

        envAttackCoeff = std::exp(-1.0f / ((float) innerRate * 0.0008f));
        envReleaseCoeff = std::exp(-1.0f / ((float) innerRate * 0.018f));
        sagAttackCoeff = std::exp(-1.0f / ((float) innerRate * 0.0040f));
        sagReleaseCoeff = std::exp(-1.0f / ((float) innerRate * 0.220f));

        cachedTone = -999.0f;
        cachedMode = -1;
        cachedBias = -999.0f;

        setProcessingLatency((int) std::round(oversampler.getLatencyInSamples()));
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        oversampler.reset();
        for (auto& filter : inputHP)
            filter.reset();
        for (auto& filter : preContour)
            filter.reset();
        for (auto& filter : bodyShelf)
            filter.reset();
        for (auto& filter : toneLP)
            filter.reset();
        for (auto& filter : contourPeak)
            filter.reset();
        for (auto& filter : dcBlock)
            filter.reset();

        inputEnvelope.fill(0.0f);
        sagEnvelope.fill(0.0f);
        gateGain.fill(0.0f);
        gateOpen.fill(false);
        displaySag = 0.0f;
        driftPhase = 0.0f;
        sputterPhase = 0.0f;

        fuzzSmooth.setCurrentAndTargetValue(fuzzParam != nullptr ? fuzzParam->get() : 67.0f);
        gateSmooth.setCurrentAndTargetValue(gateParam != nullptr ? gateParam->get() : 0.28f);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? snapMixTarget(mixParam->get()) : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelParam->get() : 0.58f);
        biasSmooth.setCurrentAndTargetValue(biasParam != nullptr ? biasParam->get() : 0.56f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        scrubInvalidSamples(buffer);

        if (!canProcessBlock(buffer))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            endBypassProcess(buffer);
            return;
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch),
                buffer.getNumSamples());

        fuzzSmooth.setTargetValue(fuzzParam != nullptr ? fuzzParam->get() : 67.0f);
        gateSmooth.setTargetValue(gateParam != nullptr ? gateParam->get() : 0.28f);
        const float requestedMix = mixParam != nullptr ? snapMixTarget(mixParam->get()) : 1.0f;
        if (requestedMix <= 1.0e-4f || requestedMix >= 0.9999f)
            mixSmooth.setCurrentAndTargetValue(requestedMix);
        else
            mixSmooth.setTargetValue(requestedMix);
        levelSmooth.setTargetValue(levelParam != nullptr ? levelParam->get() : 0.58f);
        biasSmooth.setTargetValue(biasParam != nullptr ? biasParam->get() : 0.56f);
        updateFilters();

        const auto config = Nova::FuzzDSP::getModeConfig(modeParam != nullptr ? modeParam->getIndex() : 0);
        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        const int channels = (int) upsampled.getNumChannels();
        const int samples = (int) upsampled.getNumSamples();

        float sagAccumulator = 0.0f;

        for (int sample = 0; sample < samples; ++sample)
        {
            const float fuzzAmount = fuzzSmooth.getNextValue();
            const float gateAmount = gateSmooth.getNextValue();
            const float biasAmount = biasSmooth.getNextValue();
            const float fuzzNorm = juce::jlimit(0.0f, 1.0f, fuzzAmount * 0.01f);
            const float octaveBlend = config.octaveBlend * (0.35f + fuzzNorm * 0.95f);
            const float starve = 1.0f - biasAmount;
            const float driftRateHz = 0.018f + config.sputterAmount * 0.11f;
            const float sputterRateHz = 0.13f + config.sputterAmount * 0.42f;
            driftPhase += driftRateHz / (float) innerRate;
            sputterPhase += sputterRateHz / (float) innerRate;
            driftPhase -= std::floor(driftPhase);
            sputterPhase -= std::floor(sputterPhase);
            const float driftLfo = std::sin(driftPhase * juce::MathConstants<float>::twoPi)
                + 0.43f * std::sin((driftPhase * 0.37f + 0.19f) * juce::MathConstants<float>::twoPi);
            const float sputterLfo = std::sin(sputterPhase * juce::MathConstants<float>::twoPi)
                * std::sin((sputterPhase * 1.73f + 0.11f) * juce::MathConstants<float>::twoPi);
            const float effectiveBias = juce::jlimit(0.0f,
                1.0f,
                biasAmount + driftLfo * (0.010f + config.sputterAmount * 0.045f));

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = upsampled.getChannelPointer((size_t) ch);
                float x = data[sample];

                x = inputHP[(size_t) ch].process(x);
                x = preContour[(size_t) ch].process(x);

                const float absolute = std::abs(x);

                inputEnvelope[(size_t) ch] = absolute > inputEnvelope[(size_t) ch]
                    ? absolute + envAttackCoeff * (inputEnvelope[(size_t) ch] - absolute)
                    : absolute + envReleaseCoeff * (inputEnvelope[(size_t) ch] - absolute);

                sagEnvelope[(size_t) ch] = absolute > sagEnvelope[(size_t) ch]
                    ? absolute + sagAttackCoeff * (sagEnvelope[(size_t) ch] - absolute)
                    : absolute + sagReleaseCoeff * (sagEnvelope[(size_t) ch] - absolute);

                const float gateJitter = config.sputterAmount * (0.0012f + 0.0036f * std::abs(sputterLfo));
                const float gateOpenThresh = gateAmount * 0.030f + config.gateBias * 0.012f + starve * 0.006f + gateJitter;
                const float gateCloseThresh = gateOpenThresh * 0.55f;

                if (inputEnvelope[(size_t) ch] > gateOpenThresh)
                    gateOpen[(size_t) ch] = true;
                else if (inputEnvelope[(size_t) ch] < gateCloseThresh)
                    gateOpen[(size_t) ch] = false;

                const float targetGate = gateAmount < 0.01f ? 1.0f : (gateOpen[(size_t) ch] ? 1.0f : 0.0f);
                const float gateCoeff = gateOpen[(size_t) ch] ? 0.18f : 0.05f;
                gateGain[(size_t) ch] += (targetGate - gateGain[(size_t) ch]) * gateCoeff;
                const float gateScale = juce::jlimit(0.0f, 1.0f,
                    gateGain[(size_t) ch] + (!gateOpen[(size_t) ch] ? inputEnvelope[(size_t) ch] / juce::jmax(0.0001f, gateCloseThresh) * 0.18f : 0.0f));

                const float sag = 1.0f - sagEnvelope[(size_t) ch] * config.sagAmount * (0.22f + fuzzNorm * 0.62f);
                const float sagGain = juce::jlimit(0.28f, 1.0f, sag);
                sagAccumulator += 1.0f - sagGain;
                const float cleanup = juce::jlimit(0.58f,
                    1.0f,
                    0.58f + inputEnvelope[(size_t) ch] * (2.2f + fuzzNorm * 0.95f));
                const float stage1Drive = (1.4f + fuzzNorm * 7.8f) * cleanup;
                const float stage2Drive = (1.0f + fuzzNorm * 5.2f) * (0.76f + cleanup * 0.24f);

                float stage = x * stage1Drive * sagGain;
                stage = Nova::FuzzDSP::asymExpClip(stage + (effectiveBias - 0.5f) * 0.16f,
                    1.2f + effectiveBias * 0.7f,
                    1.5f + starve * 0.8f,
                    0.74f + effectiveBias * 0.16f);

                const float octave = std::tanh(std::abs(stage * stage2Drive) * (1.1f + fuzzNorm * 0.7f));

                float out = 0.0f;
                if ((modeParam != nullptr ? modeParam->getIndex() : 0) == 1)
                {
                    const float squareA = Nova::FuzzDSP::smoothSquare(stage * stage2Drive * 0.72f, 1.45f + fuzzNorm * 1.2f);
                    const float squareB = Nova::FuzzDSP::smoothSquare(squareA * (1.38f + fuzzNorm * 0.9f), 1.18f + effectiveBias * 0.8f);
                    out = squareB * 0.84f + stage * 0.16f;
                }
                else if ((modeParam != nullptr ? modeParam->getIndex() : 0) == 2)
                {
                    const float threshold = 0.50f + effectiveBias * 0.32f;
                    const float velcro = Nova::FuzzDSP::starvedClip(stage * stage2Drive * (1.0f + starve * 0.55f),
                        threshold,
                        0.85f + fuzzNorm * 1.05f);
                    const float sputter = config.sputterAmount * starve * (1.0f - gateScale) * std::sin(velcro * juce::MathConstants<float>::twoPi * 0.8f);
                    out = velcro + sputter;
                }
                else
                {
                    const float vintage = Nova::FuzzDSP::asymExpClip(stage * stage2Drive,
                        1.05f + effectiveBias * 0.55f,
                        1.25f + starve * 0.65f,
                        0.76f + effectiveBias * 0.12f);
                    out = vintage;
                }

                out += octave * octaveBlend;
                out *= gateScale;

                out = bodyShelf[(size_t) ch].process(out);
                out = contourPeak[(size_t) ch].process(out);
                out = toneLP[(size_t) ch].process(out);
                out = dcBlock[(size_t) ch].process(out);

                data[sample] = out * config.outputTrim;
            }
        }

        displaySag = sagAccumulator / juce::jmax(1, channels * samples);
        oversampler.processSamplesDown(block);

        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();
            const float level = juce::jmap(juce::jlimit(0.0f, 1.0f, levelSmooth.getNextValue()), 0.08f, 1.6f);

            if (mix <= 1.0e-4f)
            {
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.setSample(ch, sample, scratchBuffer.getSample(ch, sample));
                continue;
            }

            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float dry = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample);
                buffer.setSample(ch, sample, (dry * dryGain + wet * wetGain) * level);
            }
        }

        endBypassProcess(buffer);
    }

    static float toneToCutoffHz(float tone) noexcept
    {
        return 700.0f + juce::jlimit(0.0f, 1.0f, tone) * 7200.0f;
    }

    static float snapMixTarget(float mix) noexcept
    {
        if (mix <= 1.0e-4f)
            return 0.0f;
        if (mix >= 0.9999f)
            return 1.0f;
        return mix;
    }

    static float biasToDescriptor(float bias) noexcept
    {
        return juce::jmap(juce::jlimit(0.0f, 1.0f, bias), -1.0f, 1.0f);
    }

    static juce::String getModeDescription(int modeIndex)
    {
        switch (modeIndex)
        {
            case 1:  return "Sustained wall-of-fuzz with heavier lows and the familiar mid scoop";
            case 2:  return "Starved velcro texture with sputter, octave edge and more aggressive gating";
            default: return "Vintage transistor fuzz with cleanup feel, sag and chewy upper mids";
        }
    }

    static float computeTransferCurve(float input, float fuzzAmount, float biasAmount, int modeIndex)
    {
        const auto config = Nova::FuzzDSP::getModeConfig(modeIndex);
        const float fuzzNorm = juce::jlimit(0.0f, 1.0f, fuzzAmount * 0.01f);
        const float stage1Drive = 1.4f + fuzzNorm * 7.8f;
        const float stage2Drive = 1.0f + fuzzNorm * 5.2f;
        const float starve = 1.0f - biasAmount;
        const float octaveBlend = config.octaveBlend * (0.35f + fuzzNorm * 0.95f);
        float stage = Nova::FuzzDSP::asymExpClip(input * stage1Drive + (biasAmount - 0.5f) * 0.16f,
            1.2f + biasAmount * 0.7f,
            1.5f + starve * 0.8f,
            0.74f + biasAmount * 0.16f);
        const float octave = std::tanh(std::abs(stage * stage2Drive) * (1.1f + fuzzNorm * 0.7f));
        float out = 0.0f;

        if (modeIndex == 1)
        {
            const float squareA = Nova::FuzzDSP::smoothSquare(stage * stage2Drive * 0.72f, 1.45f + fuzzNorm * 1.2f);
            const float squareB = Nova::FuzzDSP::smoothSquare(squareA * (1.38f + fuzzNorm * 0.9f), 1.18f + biasAmount * 0.8f);
            out = squareB * 0.84f + stage * 0.16f;
        }
        else if (modeIndex == 2)
        {
            out = Nova::FuzzDSP::starvedClip(stage * stage2Drive * (1.0f + starve * 0.55f),
                0.50f + biasAmount * 0.32f,
                0.85f + fuzzNorm * 1.05f);
        }
        else
        {
            out = Nova::FuzzDSP::asymExpClip(stage * stage2Drive,
                1.05f + biasAmount * 0.55f,
                1.25f + starve * 0.65f,
                0.76f + biasAmount * 0.12f);
        }

        out += octave * octaveBlend;
        return juce::jlimit(-1.25f, 1.25f, out * config.outputTrim);
    }

    juce::AudioParameterFloat* fuzzParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* gateParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;
    juce::AudioParameterFloat* biasParam = nullptr;
    juce::AudioParameterChoice* modeParam = nullptr;
    float displaySag = 0.0f;

    int getRealtimeFallbackCount() const noexcept
    {
        return fallbackBlockCount.load(std::memory_order_relaxed);
    }

    void clearRealtimeFallbackCount() noexcept
    {
        fallbackBlockCount.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr int kMaxProcessingChannels = 2;

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

    void updateFilters()
    {
        const float tone = toneParam != nullptr ? toneParam->get() : 0.48f;
        const float bias = biasParam != nullptr ? biasParam->get() : 0.56f;
        const int mode = modeParam != nullptr ? modeParam->getIndex() : 0;

        if (std::abs(cachedTone - tone) < 1.0e-4f
            && std::abs(cachedBias - bias) < 1.0e-4f
            && cachedMode == mode)
            return;

        cachedTone = tone;
        cachedBias = bias;
        cachedMode = mode;

        const auto config = Nova::FuzzDSP::getModeConfig(mode);
        const float toneCutoff = toneToCutoffHz(tone);
        const float biasOffset = biasToDescriptor(bias);

        for (auto& filter : inputHP)
            filter.setHighPass(mode == 1 ? 28.0f : 52.0f, 0.72f, innerRate);
        for (auto& filter : preContour)
            filter.setPeak(config.preMidFreq, config.preMidGainDb + biasOffset * 1.3f, 0.90f, innerRate);
        for (auto& filter : bodyShelf)
            filter.setLowShelf(config.bodyFreq, config.bodyGainDb + biasOffset * 1.0f, 0.78f, innerRate);
        for (auto& filter : contourPeak)
            filter.setPeak(config.contourFreq, config.contourGainDb, 0.85f, innerRate);
        for (auto& filter : toneLP)
            filter.setLowPass(toneCutoff, mode == 1 ? 0.62f : 0.72f, innerRate);
        for (auto& filter : dcBlock)
            filter.setHighPass(18.0f, 0.707f, innerRate);
    }

    juce::dsp::Oversampling<float> oversampler;
    std::array<Nova::FuzzDSP::Biquad, 2> inputHP;
    std::array<Nova::FuzzDSP::Biquad, 2> preContour;
    std::array<Nova::FuzzDSP::Biquad, 2> bodyShelf;
    std::array<Nova::FuzzDSP::Biquad, 2> toneLP;
    std::array<Nova::FuzzDSP::Biquad, 2> contourPeak;
    std::array<Nova::FuzzDSP::Biquad, 2> dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> fuzzSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> biasSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    double innerRate = 352800.0;
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    std::array<float, 2> inputEnvelope {};
    std::array<float, 2> sagEnvelope {};
    std::array<float, 2> gateGain {};
    std::array<bool, 2> gateOpen {};
    float envAttackCoeff = 0.0f;
    float envReleaseCoeff = 0.0f;
    float sagAttackCoeff = 0.0f;
    float sagReleaseCoeff = 0.0f;
    float cachedTone = -999.0f;
    float cachedBias = -999.0f;
    int cachedMode = -1;
    float driftPhase = 0.0f;
    float sputterPhase = 0.0f;
    std::atomic<int> fallbackBlockCount{ 0 };
    bool isPrepared = false;
};

#include "FuzzEditor.h"
