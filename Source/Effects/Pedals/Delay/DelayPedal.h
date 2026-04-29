#pragma once

#include "../../../Core/PedalSignalTelemetry.h"
#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cmath>
#include <vector>

namespace Nova { namespace DelayDSP {

inline float lerp(float a, float b, float t) noexcept
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

    void setLowPass(float freq, float q, double sr) noexcept
    {
        const float clampedFreq = juce::jlimit(20.0f, (float)(sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clampedFreq / (float)sr;
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
        const float clampedFreq = juce::jlimit(20.0f, (float)(sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clampedFreq / (float)sr;
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
    std::vector<float> buf;
    int writePos = 0;
    int size = 1;

    void allocate(int maxSamples)
    {
        size = juce::jmax(8, maxSamples);
        buf.assign((size_t) size, 0.0f);
        writePos = 0;
    }

    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        writePos = 0;
    }

    void write(float sample) noexcept
    {
        buf[(size_t) writePos] = sample;
        if (++writePos >= size)
            writePos = 0;
    }

    float readCubic(float delaySamples) const noexcept
    {
        const float clampedDelay = juce::jlimit(1.0f, (float) (size - 3), delaySamples);
        float rp = (float) writePos - clampedDelay;
        while (rp < 0.0f)
            rp += (float) size;

        const int i1 = ((int) rp) % size;
        const int i0 = (i1 - 1 + size) % size;
        const int i2 = (i1 + 1) % size;
        const int i3 = (i1 + 2) % size;
        const float f = rp - std::floor(rp);

        const float y0 = buf[(size_t) i0];
        const float y1 = buf[(size_t) i1];
        const float y2 = buf[(size_t) i2];
        const float y3 = buf[(size_t) i3];

        const float a = y3 - y2 - y0 + y1;
        const float b = y0 - y1 - a;
        const float c = y2 - y0;
        return y1 + f * (c + f * (b + f * a));
    }

    float readLinear(float delaySamples) const noexcept
    {
        const float clampedDelay = juce::jlimit(1.0f, (float) (size - 3), delaySamples);
        float rp = (float) writePos - clampedDelay;
        while (rp < 0.0f)
            rp += (float) size;

        const int i0 = ((int) std::floor(rp)) % size;
        const int i1 = (i0 + 1) % size;
        const float frac = rp - std::floor(rp);
        const float a = buf[(size_t) i0];
        const float b = buf[(size_t) i1];
        return a + (b - a) * frac;
    }
};

struct AllPassFilter
{
    std::vector<float> buf;
    int writePos = 0;
    int delay = 1;
    float coeff = 0.5f;

    void allocate(int maxDelay)
    {
        buf.assign((size_t) juce::jmax(4, maxDelay + 1), 0.0f);
        writePos = 0;
    }

    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        writePos = 0;
    }

    void setDelay(int newDelay) noexcept
    {
        delay = juce::jlimit(1, (int) buf.size() - 1, newDelay);
    }

    float process(float input) noexcept
    {
        int readPos = writePos - delay;
        if (readPos < 0)
            readPos += (int) buf.size();

        const float delayed = buf[(size_t) readPos];
        const float w = input + coeff * delayed;
        buf[(size_t) writePos] = w;

        const float output = -coeff * w + delayed;
        if (++writePos >= (int) buf.size())
            writePos = 0;

        return output;
    }
};

struct EnvelopeFollower
{
    float state = 0.0f;
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;

    void prepare(double sr, float attackMs, float releaseMs) noexcept
    {
        attackCoeff = coefficient(sr, attackMs);
        releaseCoeff = coefficient(sr, releaseMs);
    }

    void reset() noexcept
    {
        state = 0.0f;
    }

    float process(float input) noexcept
    {
        const float x = std::abs(input);
        const float coeff = x > state ? attackCoeff : releaseCoeff;
        state = x + coeff * (state - x);
        return state;
    }

private:
    static float coefficient(double sr, float ms) noexcept
    {
        const float seconds = juce::jmax(0.0001f, ms * 0.001f);
        return std::exp(-1.0f / (seconds * (float) sr));
    }
};

inline float softSaturate(float x, float drive) noexcept
{
    return std::tanh(x * drive) / juce::jmax(1.0f, drive);
}

inline float reverseWindowEnvelope(float phase, float windowSamples, float bloomBias) noexcept
{
    if (windowSamples <= 1.0f)
        return 0.0f;

    const float normalised = juce::jlimit(0.0f, 1.0f, phase / windowSamples);
    const float attackShape = juce::jlimit(0.55f, 2.80f, 1.35f + bloomBias * 1.20f);
    const float releaseShape = juce::jlimit(0.42f, 1.55f, 0.92f - bloomBias * 0.24f);
    const float attack = std::pow(normalised, attackShape);
    const float release = std::pow(1.0f - normalised, releaseShape);
    const float normaliser = 4.0f + bloomBias * 1.7f;
    return juce::jlimit(0.0f, 1.18f, attack * release * normaliser);
}

inline float renderReverseWindow(const DelayLine& line,
    float baseDelaySamples,
    float windowSamples,
    float phaseA,
    float phaseB,
    float bloomBias,
    float secondaryMix,
    float toneBlend) noexcept
{
    const auto renderVoice = [&](float phase, float delayScale, float localToneBlend)
    {
        const float delay = baseDelaySamples + phase * delayScale;
        const float primary = line.readCubic(delay);
        const float smearDelay = delay + windowSamples * (0.035f + 0.085f * localToneBlend);
        const float smeared = line.readLinear(smearDelay);
        const float sample = lerp(primary, smeared, juce::jlimit(0.0f, 1.0f, localToneBlend));
        return sample * reverseWindowEnvelope(phase, windowSamples, bloomBias);
    };

    return renderVoice(phaseA, 1.0f, toneBlend)
        + renderVoice(phaseB, 0.92f + bloomBias * 0.12f, toneBlend * 0.58f) * secondaryMix;
}

}} // namespace Nova::DelayDSP

class DelayPedal final : public ProcessorBase, public TempoSyncable
{
public:
    DelayPedal()
    {
        addParameter(modeParam = new juce::AudioParameterChoice("delayMode",
            "Mode",
            juce::StringArray{ "Analog", "Tape", "Digital", "Reverse" },
            0));
        addParameter(timeParam     = new juce::AudioParameterFloat("delayTime",     "Time",     35.0f, 2500.0f, 480.0f));
        addParameter(syncParam     = new juce::AudioParameterBool ("delaySync",     "Sync",     false));
        addParameter(syncDivisionParam = new juce::AudioParameterChoice("delaySyncDivision",
            "Division",
            juce::StringArray{ "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/2D", "1 Bar" },
            7));
        addParameter(feedbackParam = new juce::AudioParameterFloat("delayFeedback", "Feedback", 0.0f, 0.97f,   0.46f));
        addParameter(toneParam     = new juce::AudioParameterFloat("delayTone",     "Tone",     600.0f, 14000.0f, 5800.0f));
        addParameter(lowCutParam   = new juce::AudioParameterFloat("delayLowCut",   "Low Cut",  20.0f, 2200.0f, 70.0f));
        addParameter(spreadParam   = new juce::AudioParameterFloat("delaySpread",   "Spread",   0.0f,  1.0f,    0.42f));
        addParameter(textureParam  = new juce::AudioParameterFloat("delayTexture",  "Texture",  0.0f,  1.0f,    0.45f));
        addParameter(modDepthParam = new juce::AudioParameterFloat("delayModDepth", "Mod Depth",0.0f,  1.0f,    0.42f));
        addParameter(modRateParam  = new juce::AudioParameterFloat("delayModRate",  "Mod Rate", 0.05f, 6.0f,    1.35f));
        addParameter(mixParam      = new juce::AudioParameterFloat("delayMix",      "Mix",      0.0f,  1.0f,    0.32f));
        addParameter(duckParam     = new juce::AudioParameterFloat("delayDuck",     "Duck",     0.0f,  1.0f,    0.0f));
        addParameter(swellParam    = new juce::AudioParameterFloat("delaySwell",    "Swell",    0.0f,  1.0f,    0.0f));
        addParameter(reverseParam  = new juce::AudioParameterFloat("delayReverse",  "Reverse",  0.0f,  1.0f,    0.0f));
        addParameter(freezeParam   = new juce::AudioParameterBool ("delayFreeze",   "Freeze",   false));
    }

    const juce::String getName() const override { return "Delay"; }
    double getTailLengthSeconds() const override { return 5.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    static int getNumFlagshipPresets() noexcept { return 5; }

    static juce::String getFlagshipPresetName(int presetIndex)
    {
        switch (presetIndex)
        {
            case 0:  return "Sunset Slap";
            case 1:  return "Tape Bloom";
            case 2:  return "Crystal Grid";
            case 3:  return "Reverse Bloom";
            case 4:  return "Swell Cathedral";
            default: return "Sunset Slap";
        }
    }

    static juce::String getFlagshipPresetShortName(int presetIndex)
    {
        switch (presetIndex)
        {
            case 0:  return "SLAP";
            case 1:  return "BLOOM";
            case 2:  return "GRID";
            case 3:  return "REVERSE";
            case 4:  return "PAD";
            default: return "SLAP";
        }
    }

    void applyFlagshipPreset(int presetIndex)
    {
        const auto setChoice = [](juce::AudioParameterChoice* param, int index)
        {
            if (param == nullptr || param->choices.size() <= 1)
                return;

            const int clamped = juce::jlimit(0, param->choices.size() - 1, index);
            param->setValueNotifyingHost((float) clamped / (float) (param->choices.size() - 1));
        };

        const auto setFloat = [](juce::AudioParameterFloat* param, float value)
        {
            if (param != nullptr)
                param->setValueNotifyingHost(param->convertTo0to1(value));
        };

        const auto setBool = [](juce::AudioParameterBool* param, bool value)
        {
            if (param != nullptr)
                param->setValueNotifyingHost(value ? 1.0f : 0.0f);
        };

        switch (presetIndex)
        {
            case 0:
                setChoice(modeParam, 0);
                setFloat(timeParam, 118.0f);
                setBool(syncParam, false);
                setChoice(syncDivisionParam, 4);
                setFloat(feedbackParam, 0.34f);
                setFloat(toneParam, 4200.0f);
                setFloat(lowCutParam, 120.0f);
                setFloat(spreadParam, 0.22f);
                setFloat(textureParam, 0.40f);
                setFloat(modDepthParam, 0.18f);
                setFloat(modRateParam, 1.08f);
                setFloat(mixParam, 0.27f);
                setFloat(duckParam, 0.12f);
                setFloat(swellParam, 0.0f);
                setFloat(reverseParam, 0.0f);
                setBool(freezeParam, false);
                break;

            case 1:
                setChoice(modeParam, 1);
                setFloat(timeParam, 620.0f);
                setBool(syncParam, true);
                setChoice(syncDivisionParam, 8);
                setFloat(feedbackParam, 0.76f);
                setFloat(toneParam, 4700.0f);
                setFloat(lowCutParam, 130.0f);
                setFloat(spreadParam, 0.80f);
                setFloat(textureParam, 0.84f);
                setFloat(modDepthParam, 0.68f);
                setFloat(modRateParam, 1.64f);
                setFloat(mixParam, 0.38f);
                setFloat(duckParam, 0.18f);
                setFloat(swellParam, 0.14f);
                setFloat(reverseParam, 0.0f);
                setBool(freezeParam, false);
                break;

            case 2:
                setChoice(modeParam, 2);
                setFloat(timeParam, 340.0f);
                setBool(syncParam, true);
                setChoice(syncDivisionParam, 5);
                setFloat(feedbackParam, 0.63f);
                setFloat(toneParam, 11200.0f);
                setFloat(lowCutParam, 60.0f);
                setFloat(spreadParam, 1.0f);
                setFloat(textureParam, 0.24f);
                setFloat(modDepthParam, 0.15f);
                setFloat(modRateParam, 0.92f);
                setFloat(mixParam, 0.34f);
                setFloat(duckParam, 0.10f);
                setFloat(swellParam, 0.0f);
                setFloat(reverseParam, 0.0f);
                setBool(freezeParam, false);
                break;

            case 3:
                setChoice(modeParam, 3);
                setFloat(timeParam, 520.0f);
                setBool(syncParam, true);
                setChoice(syncDivisionParam, 7);
                setFloat(feedbackParam, 0.74f);
                setFloat(toneParam, 6100.0f);
                setFloat(lowCutParam, 120.0f);
                setFloat(spreadParam, 0.90f);
                setFloat(textureParam, 0.86f);
                setFloat(modDepthParam, 0.48f);
                setFloat(modRateParam, 1.22f);
                setFloat(mixParam, 0.40f);
                setFloat(duckParam, 0.06f);
                setFloat(swellParam, 0.24f);
                setFloat(reverseParam, 0.78f);
                setBool(freezeParam, false);
                break;

            case 4:
            default:
                setChoice(modeParam, 1);
                setFloat(timeParam, 760.0f);
                setBool(syncParam, true);
                setChoice(syncDivisionParam, 10);
                setFloat(feedbackParam, 0.83f);
                setFloat(toneParam, 5200.0f);
                setFloat(lowCutParam, 155.0f);
                setFloat(spreadParam, 0.88f);
                setFloat(textureParam, 0.78f);
                setFloat(modDepthParam, 0.56f);
                setFloat(modRateParam, 0.88f);
                setFloat(mixParam, 0.44f);
                setFloat(duckParam, 0.04f);
                setFloat(swellParam, 0.82f);
                setFloat(reverseParam, 0.22f);
                setBool(freezeParam, false);
                break;
        }

        lastAppliedFlagshipPreset.store(juce::jlimit(-1, getNumFlagshipPresets() - 1, presetIndex), std::memory_order_relaxed);
    }

    int getLastAppliedFlagshipPreset() const noexcept
    {
        return lastAppliedFlagshipPreset.load(std::memory_order_relaxed);
    }

    void setTempoSyncContext(float bpm, bool tempoValid, bool transportPlaying) override
    {
        hostTempoBpm.store(juce::jlimit(20.0f, 320.0f, bpm), std::memory_order_relaxed);
        hostTempoValid.store(tempoValid, std::memory_order_relaxed);
        hostTransportPlaying.store(transportPlaying, std::memory_order_relaxed);
    }

    float getDisplayTempoBpm() const noexcept { return hostTempoBpm.load(std::memory_order_relaxed); }
    bool isHostTempoValid() const noexcept { return hostTempoValid.load(std::memory_order_relaxed); }
    bool isHostTransportPlaying() const noexcept { return hostTransportPlaying.load(std::memory_order_relaxed); }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;

        const int maxDelaySamples = (int) std::ceil(sr * 4.5) + juce::jmax(1, samplesPerBlock) + 16;
        delayL.allocate(maxDelaySamples);
        delayR.allocate(maxDelaySamples);

        initialiseSmoother(timeSmooth, timeParam, 480.0f, 0.05);
        initialiseSmoother(feedbackSmooth, feedbackParam, 0.46f, 0.05);
        initialiseSmoother(lowCutSmooth, lowCutParam, 70.0f, 0.05);
        initialiseSmoother(spreadSmooth, spreadParam, 0.42f, 0.05);
        initialiseSmoother(textureSmooth, textureParam, 0.45f, 0.05);
        initialiseSmoother(modDepthSmooth, modDepthParam, 0.42f, 0.07);
        initialiseSmoother(modRateSmooth, modRateParam, 1.35f, 0.07);
        initialiseSmoother(mixSmooth, mixParam, 0.32f, 0.04);
        initialiseSmoother(duckSmooth, duckParam, 0.0f, 0.06);
        initialiseSmoother(swellSmooth, swellParam, 0.0f, 0.06);
        initialiseSmoother(reverseSmooth, reverseParam, 0.0f, 0.06);

        duckFollower.prepare(sr, 4.0f, 140.0f);
        swellFollower.prepare(sr, 1.0f, 140.0f);

        for (auto& lp : toneLPF) lp.reset();
        for (auto& hp : fbHighPass) hp.reset();
        for (auto& hp : dcBlock) hp.reset();
        for (auto& lp : freezeLPF) lp.reset();

        const double ratio = sr / 48000.0;
        for (auto& ap : diffusionL) ap.allocate(128);
        for (auto& ap : diffusionR) ap.allocate(128);
        diffusionL[0].setDelay(juce::jmax(1, (int) (11.0 * ratio)));
        diffusionL[1].setDelay(juce::jmax(1, (int) (23.0 * ratio)));
        diffusionR[0].setDelay(juce::jmax(1, (int) (17.0 * ratio)));
        diffusionR[1].setDelay(juce::jmax(1, (int) (31.0 * ratio)));

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        signalTelemetry.prepare(sampleRate);
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
        delayL.clear();
        delayR.clear();

        initialiseSmoother(timeSmooth, timeParam, 480.0f, 0.05);
        initialiseSmoother(feedbackSmooth, feedbackParam, 0.46f, 0.05);
        initialiseSmoother(lowCutSmooth, lowCutParam, 70.0f, 0.05);
        initialiseSmoother(spreadSmooth, spreadParam, 0.42f, 0.05);
        initialiseSmoother(textureSmooth, textureParam, 0.45f, 0.05);
        initialiseSmoother(modDepthSmooth, modDepthParam, 0.42f, 0.07);
        initialiseSmoother(modRateSmooth, modRateParam, 1.35f, 0.07);
        initialiseSmoother(mixSmooth, mixParam, 0.32f, 0.04);
        initialiseSmoother(duckSmooth, duckParam, 0.0f, 0.06);
        initialiseSmoother(swellSmooth, swellParam, 0.0f, 0.06);
        initialiseSmoother(reverseSmooth, reverseParam, 0.0f, 0.06);

        for (auto& lp : toneLPF) lp.reset();
        for (auto& hp : fbHighPass) hp.reset();
        for (auto& hp : dcBlock) hp.reset();
        for (auto& lp : freezeLPF) lp.reset();
        for (auto& ap : diffusionL) ap.clear();
        for (auto& ap : diffusionR) ap.clear();

        duckFollower.reset();
        swellFollower.reset();

        lfoPhase = 0.0f;
        lfoPhase2 = 0.0f;
        flutterPhase = 0.0f;
        flutterPhase2 = 0.0f;
        reversePhaseA = 0.0f;
        reversePhaseB = 72.0f;
        swellHoldSamples = 0;

        holdStateL = 0.0f;
        holdStateR = 0.0f;
        freezeActive = false;
        freezeAgeSamples = 0;
        cachedModeIndex = -1;
        cachedTexture = -1.0f;
        cachedToneCutoff = -1.0f;
        cachedLowCut = -1.0f;

        lastWetL = 0.0f;
        lastWetR = 0.0f;
        signalTelemetry.reset();
        debugTelemetry.resetWindow();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        signalTelemetry.captureInput(buffer);

        setTargetFromParam(feedbackSmooth, feedbackParam, 0.46f);
        setTargetFromParam(lowCutSmooth, lowCutParam, 70.0f);
        setTargetFromParam(spreadSmooth, spreadParam, 0.42f);
        setTargetFromParam(textureSmooth, textureParam, 0.45f);
        setTargetFromParam(modDepthSmooth, modDepthParam, 0.42f);
        setTargetFromParam(modRateSmooth, modRateParam, 1.35f);
        setTargetFromParam(mixSmooth, mixParam, 0.32f);
        setTargetFromParam(duckSmooth, duckParam, 0.0f);
        setTargetFromParam(swellSmooth, swellParam, 0.0f);
        setTargetFromParam(reverseSmooth, reverseParam, 0.0f);

        const int modeIndex = modeParam != nullptr ? modeParam->getIndex() : 0;
        const float toneCutoff = toneParam != nullptr ? toneParam->get() : 5800.0f;
        const float lowCutHz = lowCutParam != nullptr ? lowCutParam->get() : 70.0f;
        const float previewTexture = textureParam != nullptr ? textureParam->get() : 0.45f;
        updateFeedbackFilters(modeIndex, previewTexture, toneCutoff, lowCutHz);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();
        const float maxDelaySamples = (float) (delayL.size - 4);
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        const bool syncEnabled = syncParam != nullptr && syncParam->get();
        const float syncTimeMs = computeSyncedTimeMs(syncDivisionParam != nullptr ? syncDivisionParam->getIndex() : 7);
        timeSmooth.setTargetValue(syncEnabled ? syncTimeMs : (timeParam != nullptr ? timeParam->get() : 480.0f));

        float blockWetPeakL = 0.0f;
        float blockWetPeakR = 0.0f;
        const bool freezeRequested = freezeParam != nullptr && freezeParam->get();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float timeMs = timeSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float lowCut = lowCutSmooth.getNextValue();
            const float spread = spreadSmooth.getNextValue();
            const float texture = textureSmooth.getNextValue();
            const float modDepth = modDepthSmooth.getNextValue();
            const float modRate = modRateSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();
            const float duck = duckSmooth.getNextValue();
            const float swell = swellSmooth.getNextValue();
            const float reverseAmount = reverseSmooth.getNextValue();

            if ((sample & 7) == 0)
                updateFeedbackFilters(modeIndex, texture, toneCutoff, lowCut);

            const float inL = buffer.getSample(0, sample);
            const float inR = numChannels > 1 ? buffer.getSample(1, sample) : inL;
            const float inputSense = 0.5f * (std::abs(inL) + std::abs(inR));

            const auto style = getModeStyle(modeIndex, texture, reverseAmount);
            const float baseDelay = juce::jlimit(1.0f, maxDelaySamples, timeMs * (float) sr * 0.001f);

            const float modDepthScale = computeModDepthScale(modDepth);
            const float modRateScale = computeModRateScale(modRate);
            const float modDepth1 = style.modDepthMs1 * modDepthScale * (float) sr * 0.001f;
            const float modDepth2 = style.modDepthMs2 * modDepthScale * (float) sr * 0.001f;
            const float flutterDepth = style.flutterDepthMs * modDepthScale * (float) sr * 0.001f;

            const float drift1 = std::sin(twoPi * lfoPhase);
            const float drift2 = std::sin(twoPi * lfoPhase2);
            const float flutter1 = std::sin(twoPi * flutterPhase);
            const float flutter2 = std::sin(twoPi * flutterPhase2);

            const float modL = drift1 * modDepth1 + drift2 * modDepth2 + flutter1 * flutterDepth;
            const float modR = drift1 * modDepth1 * 0.72f - drift2 * modDepth2 * 0.58f + flutter2 * flutterDepth * 0.84f;
            const float stereoOffset = baseDelay * style.stereoSkew * spread;

            const float delayLSamples = juce::jlimit(1.0f, maxDelaySamples, baseDelay - stereoOffset + modL);
            const float delayRSamples = juce::jlimit(1.0f, maxDelaySamples, baseDelay + stereoOffset + modR);

            const float wetNormalL = delayL.readCubic(delayLSamples);
            const float wetNormalR = delayR.readCubic(delayRSamples);

            const float reverseWindow = juce::jlimit(48.0f,
                juce::jmin(maxDelaySamples * 0.55f, baseDelay * (0.55f + 0.25f * texture)),
                style.reverseWindowMs * (float) sr * 0.001f + baseDelay * 0.18f);
            const float reverseBaseDelay = juce::jlimit(8.0f, maxDelaySamples * 0.82f, baseDelay * style.reverseDelayScale);

            const float reverseWetL = Nova::DelayDSP::renderReverseWindow(delayL,
                reverseBaseDelay,
                reverseWindow,
                reversePhaseA,
                reversePhaseB,
                style.reverseBloom,
                style.reverseSecondaryMix,
                style.reverseToneBlend) * style.reverseTrim;
            const float reverseWetR = Nova::DelayDSP::renderReverseWindow(delayR,
                reverseBaseDelay,
                reverseWindow,
                reversePhaseA,
                reversePhaseB,
                style.reverseBloom,
                style.reverseSecondaryMix,
                style.reverseToneBlend) * style.reverseTrim;

            const float reverseBlend = juce::jlimit(0.0f, 1.0f, style.reverseBase + reverseAmount * style.reverseDepth);
            const float wetL = wetNormalL * (1.0f - reverseBlend) + reverseWetL * reverseBlend;
            const float wetR = wetNormalR * (1.0f - reverseBlend) + reverseWetR * reverseBlend;
            float wetOutL = wetL;
            float wetOutR = wetR;

            const float duckEnv = juce::jlimit(0.0f, 1.0f, duckFollower.process(inputSense) * 3.8f);
            const float swellEnv = juce::jlimit(0.0f, 1.0f, swellFollower.process(inputSense) * 4.3f);
            const float duckGain = 1.0f - duck * duckEnv * 0.82f;
            const float swellHoldScale = style.swellHoldScale + reverseBlend * 0.18f;
            const int targetSwellHold = juce::jlimit(0,
                (int) (sr * 1.25),
                (int) std::round(baseDelay * (0.18f + swellHoldScale * swell)));
            if (swell > 0.001f && inputSense > 0.015f)
                swellHoldSamples = juce::jmax(swellHoldSamples, targetSwellHold);
            if (swellHoldSamples > 0)
                --swellHoldSamples;

            const float timedSwellNormalised = targetSwellHold > 0
                ? juce::jlimit(0.0f, 1.0f, (float) swellHoldSamples / (float) targetSwellHold)
                : 0.0f;
            const float reverseSwellWeight = reverseBlend * swell;
            const float timedSwellGain = 1.0f - swell * std::pow(timedSwellNormalised, 0.32f)
                * (style.swellDuckDepth + reverseBlend * 0.34f);
            const float followerSwellGain = 1.0f - swell * std::pow(swellEnv, 0.78f)
                * (0.58f + reverseBlend * 0.24f);
            const float swellGain = juce::jlimit(0.02f, 1.0f, juce::jmin(timedSwellGain, followerSwellGain));
            const float swellBloom = 1.0f + swell * std::pow(1.0f - timedSwellNormalised, 0.92f)
                * (style.swellBloomGain + reverseSwellWeight * 0.30f);
            const float reverseSwellGate = 1.0f - reverseSwellWeight
                * juce::jlimit(0.0f, 1.0f, 0.14f + timedSwellNormalised * 0.86f) * 0.60f;
            const float earlyReverseSwellTrim = 1.0f - reverseSwellWeight
                * std::pow(timedSwellNormalised, 0.55f) * 0.64f;
            const float performanceGain = juce::jlimit(0.05f, 1.08f,
                duckGain * swellGain * reverseSwellGate * earlyReverseSwellTrim);

            const float cross = juce::jlimit(0.0f, 1.0f, spread * style.crossfeed);
            const float straight = 1.0f - cross;
            float fbL = wetL * straight + wetR * cross;
            float fbR = wetR * straight + wetL * cross;

            fbL = toneLPF[0].process(fbL);
            fbR = toneLPF[1].process(fbR);
            fbL = fbHighPass[0].process(fbL);
            fbR = fbHighPass[1].process(fbR);

            const float rawFbL = fbL;
            const float rawFbR = fbR;
            for (auto& ap : diffusionL) fbL = ap.process(fbL);
            for (auto& ap : diffusionR) fbR = ap.process(fbR);
            fbL = Nova::DelayDSP::lerp(rawFbL, fbL, style.diffusionBlend);
            fbR = Nova::DelayDSP::lerp(rawFbR, fbR, style.diffusionBlend);

            const float loopTapL = Nova::DelayDSP::softSaturate(fbL, style.drive) * style.feedbackTrim;
            const float loopTapR = Nova::DelayDSP::softSaturate(fbR, style.drive) * style.feedbackTrim;
            fbL = loopTapL * feedback;
            fbR = loopTapR * feedback;
            float delayWriteL = inL + fbL;
            float delayWriteR = inR + fbR;

            float freezeBlend = 0.0f;

            if (freezeRequested)
            {
                if (!freezeActive)
                {
                    holdStateL = wetL;
                    holdStateR = wetR;
                    freezeAgeSamples = 0;
                    freezeActive = true;
                }

                const float freezeRampSamples = juce::jmax(1.0f, (float) (sr * 0.075f));
                freezeBlend = juce::jlimit(0.0f, 1.0f, (float) (freezeAgeSamples + 1) / freezeRampSamples);
                const float freezeFeedback = 0.9987f + texture * 0.0009f + reverseBlend * 0.0004f;
                const float freezeWriteTrim = 1.05f + texture * 0.08f + reverseBlend * 0.04f;
                const float freezeMonitorTrim = 1.18f + texture * 0.12f + reverseBlend * 0.08f;
                const float freezeCaptureBlend = 0.42f + freezeBlend * 0.28f;

                const float frozenInputL = Nova::DelayDSP::lerp(holdStateL, wetL, freezeCaptureBlend);
                const float frozenInputR = Nova::DelayDSP::lerp(holdStateR, wetR, freezeCaptureBlend);
                holdStateL = freezeLPF[0].process(frozenInputL);
                holdStateR = freezeLPF[1].process(frozenInputR);

                const float writeL = Nova::DelayDSP::softSaturate(holdStateL * freezeFeedback * freezeWriteTrim, 1.06f);
                const float writeR = Nova::DelayDSP::softSaturate(holdStateR * freezeFeedback * freezeWriteTrim, 1.06f);
                delayWriteL = writeL;
                delayWriteR = writeR;
                delayL.write(writeL);
                delayR.write(writeR);

                wetOutL = Nova::DelayDSP::lerp(wetL, holdStateL * freezeMonitorTrim, 0.45f + freezeBlend * 0.40f);
                wetOutR = Nova::DelayDSP::lerp(wetR, holdStateR * freezeMonitorTrim, 0.45f + freezeBlend * 0.40f);
                freezeAgeSamples = juce::jmin(freezeAgeSamples + 1, (int) sr);
            }
            else
            {
                freezeActive = false;
                freezeAgeSamples = 0;
                delayL.write(delayWriteL);
                delayR.write(delayWriteR);
            }

            const float reverseOutputComp = 1.0f + reverseBlend * 0.42f;
            const float outputPerformance = Nova::DelayDSP::lerp(performanceGain, 1.0f, freezeBlend);
            const float outputBloom = Nova::DelayDSP::lerp(swellBloom, 1.0f, freezeBlend * 0.85f);
            const float outWetL = dcBlock[0].process(wetOutL * outputPerformance * outputBloom * style.outputTrim * reverseOutputComp);
            const float outWetR = dcBlock[1].process(wetOutR * outputPerformance * outputBloom * style.outputTrim * reverseOutputComp);

            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);
            debugTelemetry.captureChannel(0,
                delayLSamples * 1000.0f / (float) sr,
                wetNormalL,
                reverseWetL,
                loopTapL,
                delayWriteL,
                outWetL,
                freezeRequested ? delayWriteL : 0.0f);
            debugTelemetry.captureChannel(1,
                delayRSamples * 1000.0f / (float) sr,
                wetNormalR,
                reverseWetR,
                loopTapR,
                delayWriteR,
                outWetR,
                freezeRequested ? delayWriteR : 0.0f);
            debugTelemetry.captureControlWindow(inputSense,
                duckGain,
                swellGain,
                performanceGain,
                reverseBlend,
                freezeBlend,
                feedback,
                mix,
                dryGain,
                wetGain);

            buffer.setSample(0, sample, inL * dryGain + outWetL * wetGain);
            if (numChannels > 1)
                buffer.setSample(1, sample, inR * dryGain + outWetR * wetGain);

            blockWetPeakL = juce::jmax(blockWetPeakL, std::abs(outWetL));
            blockWetPeakR = juce::jmax(blockWetPeakR, std::abs(outWetR));

            advancePhase(lfoPhase, style.modRate1 * modRateScale);
            advancePhase(lfoPhase2, style.modRate2 * modRateScale);
            advancePhase(flutterPhase, style.flutterRate1 * modRateScale);
            advancePhase(flutterPhase2, style.flutterRate2 * modRateScale);
            advanceReversePhases(reverseWindow);
        }

        lastWetL = blockWetPeakL;
        lastWetR = blockWetPeakR;

        if (signalTelemetry.captureOutputAndPublishIfNeeded(buffer))
            debugTelemetry.resetWindow();

        endBypassProcess(buffer);
    }

    juce::AudioParameterChoice* modeParam = nullptr;
    juce::AudioParameterFloat* timeParam = nullptr;
    juce::AudioParameterBool* syncParam = nullptr;
    juce::AudioParameterChoice* syncDivisionParam = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* lowCutParam = nullptr;
    juce::AudioParameterFloat* spreadParam = nullptr;
    juce::AudioParameterFloat* textureParam = nullptr;
    juce::AudioParameterFloat* modDepthParam = nullptr;
    juce::AudioParameterFloat* modRateParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* duckParam = nullptr;
    juce::AudioParameterFloat* swellParam = nullptr;
    juce::AudioParameterFloat* reverseParam = nullptr;
    juce::AudioParameterBool* freezeParam = nullptr;

    float lastWetL = 0.0f;
    float lastWetR = 0.0f;

private:
    struct DebugTelemetry
    {
        std::array<float, 2> delayMsMin{};
        std::array<float, 2> delayMsMax{};
        std::array<float, 2> wetPeak{};
        std::array<float, 2> reversePeak{};
        std::array<float, 2> loopTapPeak{};
        std::array<float, 2> writePeak{};
        std::array<float, 2> outputPeak{};
        std::array<float, 2> freezeWritePeak{};
        float inputSensePeak = 0.0f;
        float duckGainMin = 1.0e9f;
        float duckGainMax = 0.0f;
        float swellGainMin = 1.0e9f;
        float swellGainMax = 0.0f;
        float performanceMin = 1.0e9f;
        float performanceMax = 0.0f;
        float reverseBlendMax = 0.0f;
        float freezeBlendMax = 0.0f;
        float feedbackMax = 0.0f;
        float mixMin = 1.0e9f;
        float mixMax = 0.0f;
        float dryGainMin = 1.0e9f;
        float dryGainMax = 0.0f;
        float wetGainMin = 1.0e9f;
        float wetGainMax = 0.0f;

        void resetWindow() noexcept
        {
            delayMsMin = { { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() } };
            delayMsMax = {};
            wetPeak = {};
            reversePeak = {};
            loopTapPeak = {};
            writePeak = {};
            outputPeak = {};
            freezeWritePeak = {};
            inputSensePeak = 0.0f;
            duckGainMin = 1.0e9f;
            duckGainMax = 0.0f;
            swellGainMin = 1.0e9f;
            swellGainMax = 0.0f;
            performanceMin = 1.0e9f;
            performanceMax = 0.0f;
            reverseBlendMax = 0.0f;
            freezeBlendMax = 0.0f;
            feedbackMax = 0.0f;
            mixMin = 1.0e9f;
            mixMax = 0.0f;
            dryGainMin = 1.0e9f;
            dryGainMax = 0.0f;
            wetGainMin = 1.0e9f;
            wetGainMax = 0.0f;
        }

        void captureChannel(int channel,
            float delayMs,
            float wet,
            float reverseWet,
            float loopTap,
            float delayWrite,
            float outputWet,
            float freezeWrite) noexcept
        {
            const auto idx = (size_t) juce::jlimit(0, 1, channel);
            delayMsMin[idx] = juce::jmin(delayMsMin[idx], delayMs);
            delayMsMax[idx] = juce::jmax(delayMsMax[idx], delayMs);
            wetPeak[idx] = juce::jmax(wetPeak[idx], std::abs(wet));
            reversePeak[idx] = juce::jmax(reversePeak[idx], std::abs(reverseWet));
            loopTapPeak[idx] = juce::jmax(loopTapPeak[idx], std::abs(loopTap));
            writePeak[idx] = juce::jmax(writePeak[idx], std::abs(delayWrite));
            outputPeak[idx] = juce::jmax(outputPeak[idx], std::abs(outputWet));
            freezeWritePeak[idx] = juce::jmax(freezeWritePeak[idx], std::abs(freezeWrite));
        }

        void captureControlWindow(float inputSense,
            float duckGain,
            float swellGain,
            float performanceGain,
            float reverseBlend,
            float freezeBlend,
            float feedback,
            float mix,
            float dryGain,
            float wetGain) noexcept
        {
            inputSensePeak = juce::jmax(inputSensePeak, inputSense);
            duckGainMin = juce::jmin(duckGainMin, duckGain);
            duckGainMax = juce::jmax(duckGainMax, duckGain);
            swellGainMin = juce::jmin(swellGainMin, swellGain);
            swellGainMax = juce::jmax(swellGainMax, swellGain);
            performanceMin = juce::jmin(performanceMin, performanceGain);
            performanceMax = juce::jmax(performanceMax, performanceGain);
            reverseBlendMax = juce::jmax(reverseBlendMax, reverseBlend);
            freezeBlendMax = juce::jmax(freezeBlendMax, freezeBlend);
            feedbackMax = juce::jmax(feedbackMax, feedback);
            mixMin = juce::jmin(mixMin, mix);
            mixMax = juce::jmax(mixMax, mix);
            dryGainMin = juce::jmin(dryGainMin, dryGain);
            dryGainMax = juce::jmax(dryGainMax, dryGain);
            wetGainMin = juce::jmin(wetGainMin, wetGain);
            wetGainMax = juce::jmax(wetGainMax, wetGain);
        }

        juce::String buildReport(const DelayPedal& pedal) const
        {
            juce::String report;
            report << "params: mode="
                   << (pedal.modeParam != nullptr ? pedal.modeParam->getCurrentChoiceName() : "Analog")
                   << ", timeMs=" << NovaDiagnostics::formatTelemetryScalar(pedal.timeParam != nullptr ? pedal.timeParam->get() : 480.0f)
                   << ", sync=" << (pedal.syncParam != nullptr && pedal.syncParam->get() ? "true" : "false")
                   << ", feedback=" << NovaDiagnostics::formatTelemetryScalar(pedal.feedbackParam != nullptr ? pedal.feedbackParam->get() : 0.46f)
                   << ", tone=" << NovaDiagnostics::formatTelemetryScalar(pedal.toneParam != nullptr ? pedal.toneParam->get() : 5800.0f)
                   << ", lowCut=" << NovaDiagnostics::formatTelemetryScalar(pedal.lowCutParam != nullptr ? pedal.lowCutParam->get() : 70.0f)
                   << ", spread=" << NovaDiagnostics::formatTelemetryScalar(pedal.spreadParam != nullptr ? pedal.spreadParam->get() : 0.42f)
                   << ", texture=" << NovaDiagnostics::formatTelemetryScalar(pedal.textureParam != nullptr ? pedal.textureParam->get() : 0.45f)
                   << ", modDepth=" << NovaDiagnostics::formatTelemetryScalar(pedal.modDepthParam != nullptr ? pedal.modDepthParam->get() : 0.42f)
                   << ", modRate=" << NovaDiagnostics::formatTelemetryScalar(pedal.modRateParam != nullptr ? pedal.modRateParam->get() : 1.35f)
                   << ", mix=" << NovaDiagnostics::formatTelemetryScalar(pedal.mixParam != nullptr ? pedal.mixParam->get() : 0.32f)
                   << ", duck=" << NovaDiagnostics::formatTelemetryScalar(pedal.duckParam != nullptr ? pedal.duckParam->get() : 0.0f)
                   << ", swell=" << NovaDiagnostics::formatTelemetryScalar(pedal.swellParam != nullptr ? pedal.swellParam->get() : 0.0f)
                   << ", reverse=" << NovaDiagnostics::formatTelemetryScalar(pedal.reverseParam != nullptr ? pedal.reverseParam->get() : 0.0f)
                   << ", freeze=" << (pedal.freezeParam != nullptr && pedal.freezeParam->get() ? "true" : "false")
                   << juce::newLine
                   << "delay.window: delayLMinMs=" << NovaDiagnostics::formatTelemetryScalar(delayMsMin[0] == std::numeric_limits<float>::max() ? 0.0f : delayMsMin[0])
                   << ", delayLMaxMs=" << NovaDiagnostics::formatTelemetryScalar(delayMsMax[0])
                   << ", delayRMinMs=" << NovaDiagnostics::formatTelemetryScalar(delayMsMin[1] == std::numeric_limits<float>::max() ? 0.0f : delayMsMin[1])
                   << ", delayRMaxMs=" << NovaDiagnostics::formatTelemetryScalar(delayMsMax[1])
                   << juce::newLine
                   << "feedback.loop: wetLMax=" << NovaDiagnostics::formatTelemetryScalar(wetPeak[0])
                   << ", wetRMax=" << NovaDiagnostics::formatTelemetryScalar(wetPeak[1])
                   << ", reverseLMax=" << NovaDiagnostics::formatTelemetryScalar(reversePeak[0])
                   << ", reverseRMax=" << NovaDiagnostics::formatTelemetryScalar(reversePeak[1])
                   << ", loopTapLMax=" << NovaDiagnostics::formatTelemetryScalar(loopTapPeak[0])
                   << ", loopTapRMax=" << NovaDiagnostics::formatTelemetryScalar(loopTapPeak[1])
                   << ", writeLMax=" << NovaDiagnostics::formatTelemetryScalar(writePeak[0])
                   << ", writeRMax=" << NovaDiagnostics::formatTelemetryScalar(writePeak[1])
                   << ", freezeWriteLMax=" << NovaDiagnostics::formatTelemetryScalar(freezeWritePeak[0])
                   << ", freezeWriteRMax=" << NovaDiagnostics::formatTelemetryScalar(freezeWritePeak[1])
                   << juce::newLine
                   << "output.path: outWetLMax=" << NovaDiagnostics::formatTelemetryScalar(outputPeak[0])
                   << ", outWetRMax=" << NovaDiagnostics::formatTelemetryScalar(outputPeak[1])
                   << ", inputSensePeak=" << NovaDiagnostics::formatTelemetryScalar(inputSensePeak)
                   << juce::newLine
                   << "control.window: duckMin=" << NovaDiagnostics::formatTelemetryScalar(duckGainMin == 1.0e9f ? 0.0f : duckGainMin)
                   << ", duckMax=" << NovaDiagnostics::formatTelemetryScalar(duckGainMax)
                   << ", swellMin=" << NovaDiagnostics::formatTelemetryScalar(swellGainMin == 1.0e9f ? 0.0f : swellGainMin)
                   << ", swellMax=" << NovaDiagnostics::formatTelemetryScalar(swellGainMax)
                   << ", performanceMin=" << NovaDiagnostics::formatTelemetryScalar(performanceMin == 1.0e9f ? 0.0f : performanceMin)
                   << ", performanceMax=" << NovaDiagnostics::formatTelemetryScalar(performanceMax)
                   << ", reverseBlendMax=" << NovaDiagnostics::formatTelemetryScalar(reverseBlendMax)
                   << ", freezeBlendMax=" << NovaDiagnostics::formatTelemetryScalar(freezeBlendMax)
                   << ", feedbackMax=" << NovaDiagnostics::formatTelemetryScalar(feedbackMax)
                   << ", mixMin=" << NovaDiagnostics::formatTelemetryScalar(mixMin == 1.0e9f ? 0.0f : mixMin)
                   << ", mixMax=" << NovaDiagnostics::formatTelemetryScalar(mixMax)
                   << ", dryGainMin=" << NovaDiagnostics::formatTelemetryScalar(dryGainMin == 1.0e9f ? 0.0f : dryGainMin)
                   << ", dryGainMax=" << NovaDiagnostics::formatTelemetryScalar(dryGainMax)
                   << ", wetGainMin=" << NovaDiagnostics::formatTelemetryScalar(wetGainMin == 1.0e9f ? 0.0f : wetGainMin)
                   << ", wetGainMax=" << NovaDiagnostics::formatTelemetryScalar(wetGainMax);
            return report;
        }
    };

    struct ModeStyle
    {
        float toneScale = 1.0f;
        float hpCutoff = 40.0f;
        float diffusionBlend = 0.15f;
        float drive = 1.1f;
        float feedbackTrim = 0.86f;
        float crossfeed = 0.35f;
        float stereoSkew = 0.04f;
        float modRate1 = 0.33f;
        float modRate2 = 0.61f;
        float modDepthMs1 = 0.10f;
        float modDepthMs2 = 0.06f;
        float flutterRate1 = 1.2f;
        float flutterRate2 = 1.9f;
        float flutterDepthMs = 0.02f;
        float reverseBase = 0.0f;
        float reverseDepth = 0.55f;
        float reverseDelayScale = 1.08f;
        float reverseWindowMs = 120.0f;
        float reverseTrim = 0.70f;
        float reverseBloom = 0.42f;
        float reverseSecondaryMix = 0.18f;
        float reverseToneBlend = 0.10f;
        float swellHoldScale = 0.42f;
        float swellDuckDepth = 0.92f;
        float swellBloomGain = 0.14f;
        float outputTrim = 1.0f;
    };

    static float syncDivisionToQuarterNotes(int divisionIndex) noexcept
    {
        switch (divisionIndex)
        {
            case 0:  return 0.125f;
            case 1:  return 1.0f / 6.0f;
            case 2:  return 0.25f;
            case 3:  return 1.0f / 3.0f;
            case 4:  return 0.5f;
            case 5:  return 0.75f;
            case 6:  return 2.0f / 3.0f;
            case 7:  return 1.0f;
            case 8:  return 1.5f;
            case 9:  return 2.0f;
            case 10: return 3.0f;
            case 11: return 4.0f;
            default: return 1.0f;
        }
    }

    float computeSyncedTimeMs(int divisionIndex) const noexcept
    {
        const float bpm = hostTempoValid.load(std::memory_order_relaxed)
            ? hostTempoBpm.load(std::memory_order_relaxed)
            : 120.0f;
        const float clampedBpm = juce::jlimit(20.0f, 320.0f, bpm);
        const float quarterMs = 60000.0f / clampedBpm;
        return juce::jlimit(35.0f, 2500.0f, quarterMs * syncDivisionToQuarterNotes(divisionIndex));
    }

    static float computeModDepthScale(float modDepth) noexcept
    {
        return 2.6f * std::pow(juce::jlimit(0.0f, 1.0f, modDepth), 0.92f);
    }

    static float computeModRateScale(float modRateHz) noexcept
    {
        return juce::jlimit(0.12f, 8.0f, modRateHz);
    }

    template <typename Smoother, typename ParamType>
    void initialiseSmoother(Smoother& smoother, ParamType* param, float fallback, double seconds) const
    {
        smoother.reset(sr, seconds);
        smoother.setCurrentAndTargetValue(param != nullptr ? param->get() : fallback);
    }

    template <typename Smoother, typename ParamType>
    static void setTargetFromParam(Smoother& smoother, ParamType* param, float fallback)
    {
        smoother.setTargetValue(param != nullptr ? param->get() : fallback);
    }

    ModeStyle getModeStyle(int modeIndex, float texture, float reverseAmount) const noexcept
    {
        ModeStyle style;

        switch (modeIndex)
        {
            case 0: // Analog
                style.toneScale = 0.78f - 0.08f * texture;
                style.hpCutoff = 44.0f;
                style.diffusionBlend = 0.12f + 0.18f * texture;
                style.drive = 1.35f + 0.55f * texture;
                style.feedbackTrim = 0.82f;
                style.crossfeed = 0.32f + 0.14f * texture;
                style.stereoSkew = 0.030f;
                style.modRate1 = 0.31f;
                style.modRate2 = 0.58f;
                style.modDepthMs1 = 0.16f + 0.14f * texture;
                style.modDepthMs2 = 0.09f + 0.11f * texture;
                style.flutterRate1 = 1.1f;
                style.flutterRate2 = 1.7f;
                style.flutterDepthMs = 0.018f + 0.030f * texture;
                style.reverseBase = 0.0f;
                style.reverseDepth = 0.42f;
                style.reverseDelayScale = 1.02f;
                style.reverseWindowMs = 96.0f + 30.0f * texture;
                style.reverseTrim = 0.62f;
                style.reverseBloom = 0.18f + 0.12f * texture;
                style.reverseSecondaryMix = 0.14f + 0.06f * texture;
                style.reverseToneBlend = 0.08f + 0.04f * texture;
                style.swellHoldScale = 0.34f + 0.18f * texture;
                style.swellDuckDepth = 0.94f;
                style.swellBloomGain = 0.10f + 0.05f * texture;
                style.outputTrim = 0.98f;
                break;

            case 1: // Tape
                style.toneScale = 0.68f - 0.10f * texture;
                style.hpCutoff = 52.0f;
                style.diffusionBlend = 0.28f + 0.30f * texture;
                style.drive = 1.55f + 0.75f * texture;
                style.feedbackTrim = 0.84f;
                style.crossfeed = 0.42f + 0.20f * texture;
                style.stereoSkew = 0.038f;
                style.modRate1 = 0.37f;
                style.modRate2 = 0.77f;
                style.modDepthMs1 = 0.24f + 0.24f * texture;
                style.modDepthMs2 = 0.15f + 0.18f * texture;
                style.flutterRate1 = 1.8f;
                style.flutterRate2 = 2.9f;
                style.flutterDepthMs = 0.052f + 0.070f * texture;
                style.reverseBase = 0.05f;
                style.reverseDepth = 0.46f;
                style.reverseDelayScale = 1.10f;
                style.reverseWindowMs = 122.0f + 55.0f * texture;
                style.reverseTrim = 0.68f;
                style.reverseBloom = 0.42f + 0.18f * texture;
                style.reverseSecondaryMix = 0.18f + 0.08f * texture;
                style.reverseToneBlend = 0.16f + 0.06f * texture;
                style.swellHoldScale = 0.42f + 0.24f * texture;
                style.swellDuckDepth = 0.96f;
                style.swellBloomGain = 0.15f + 0.08f * texture;
                style.outputTrim = 0.94f;
                break;

            case 2: // Digital
                style.toneScale = 1.15f + 0.12f * texture;
                style.hpCutoff = 34.0f;
                style.diffusionBlend = 0.02f + 0.08f * texture;
                style.drive = 1.02f + 0.12f * texture;
                style.feedbackTrim = 0.90f;
                style.crossfeed = 0.48f + 0.18f * texture;
                style.stereoSkew = 0.050f;
                style.modRate1 = 0.28f;
                style.modRate2 = 0.49f;
                style.modDepthMs1 = 0.06f + 0.06f * texture;
                style.modDepthMs2 = 0.03f + 0.05f * texture;
                style.flutterRate1 = 1.0f;
                style.flutterRate2 = 1.6f;
                style.flutterDepthMs = 0.008f + 0.012f * texture;
                style.reverseBase = 0.0f;
                style.reverseDepth = 0.34f;
                style.reverseDelayScale = 1.16f;
                style.reverseWindowMs = 138.0f + 45.0f * texture;
                style.reverseTrim = 0.82f;
                style.reverseBloom = 0.30f + 0.14f * texture;
                style.reverseSecondaryMix = 0.20f + 0.06f * texture;
                style.reverseToneBlend = 0.12f + 0.05f * texture;
                style.swellHoldScale = 0.70f + 0.26f * texture;
                style.swellDuckDepth = 1.04f;
                style.swellBloomGain = 0.12f + 0.05f * texture;
                style.outputTrim = 1.03f;
                break;

            case 3: // Reverse
            default:
                style.toneScale = 0.88f - 0.06f * texture;
                style.hpCutoff = 42.0f;
                style.diffusionBlend = 0.36f + 0.28f * texture;
                style.drive = 1.18f + 0.26f * texture;
                style.feedbackTrim = 0.80f;
                style.crossfeed = 0.46f + 0.16f * texture;
                style.stereoSkew = 0.048f;
                style.modRate1 = 0.34f;
                style.modRate2 = 0.63f;
                style.modDepthMs1 = 0.18f + 0.18f * texture;
                style.modDepthMs2 = 0.10f + 0.14f * texture;
                style.flutterRate1 = 1.3f;
                style.flutterRate2 = 2.2f;
                style.flutterDepthMs = 0.026f + 0.030f * texture;
                style.reverseBase = 0.40f + 0.18f * texture;
                style.reverseDepth = 0.48f + 0.18f * reverseAmount;
                style.reverseDelayScale = 1.22f;
                style.reverseWindowMs = 160.0f + 70.0f * texture;
                style.reverseTrim = 0.92f;
                style.reverseBloom = 0.72f + 0.18f * texture + 0.12f * reverseAmount;
                style.reverseSecondaryMix = 0.28f + 0.12f * texture;
                style.reverseToneBlend = 0.22f + 0.10f * texture;
                style.swellHoldScale = 0.90f + 0.42f * texture;
                style.swellDuckDepth = 1.20f;
                style.swellBloomGain = 0.34f + 0.14f * texture;
                style.outputTrim = 0.96f;
                break;
        }

        return style;
    }

    void updateFeedbackFilters(int modeIndex, float texture, float toneCutoff, float lowCutHz)
    {
        if (modeIndex == cachedModeIndex
            && std::abs(cachedTexture - texture) < 0.0025f
            && std::abs(cachedToneCutoff - toneCutoff) < 2.0f)
        {
            if (std::abs(cachedLowCut - lowCutHz) < 2.0f)
                return;
        }

        cachedModeIndex = modeIndex;
        cachedTexture = texture;
        cachedToneCutoff = toneCutoff;
        cachedLowCut = lowCutHz;

        const auto style = getModeStyle(modeIndex, texture, reverseParam != nullptr ? reverseParam->get() : 0.0f);
        const float cutoff = juce::jlimit(650.0f, 18000.0f, toneCutoff * style.toneScale);
        const float highPass = juce::jlimit(style.hpCutoff, 3200.0f, juce::jmax(style.hpCutoff, lowCutHz));

        for (auto& lp : toneLPF)
            lp.setLowPass(cutoff, 0.707f, sr);
        for (auto& hp : fbHighPass)
            hp.setHighPass(highPass, 0.707f, sr);
        for (auto& hp : dcBlock)
            hp.setHighPass(18.0f, 0.707f, sr);
        for (auto& lp : freezeLPF)
            lp.setLowPass(5400.0f + 2600.0f * texture, 0.707f, sr);

        const double ratio = sr / 48000.0;
        const float diffusionSkew = 1.0f + texture * 0.55f + (float) modeIndex * 0.08f;
        const int l0 = juce::jmax(1, (int) std::round(11.0 * ratio * diffusionSkew));
        const int l1 = juce::jmax(1, (int) std::round(23.0 * ratio * (1.0f + texture * 0.38f)));
        const int r0 = juce::jmax(1, (int) std::round(17.0 * ratio * diffusionSkew));
        const int r1 = juce::jmax(1, (int) std::round(31.0 * ratio * (1.0f + texture * 0.34f)));

        diffusionL[0].setDelay(l0);
        diffusionL[1].setDelay(l1);
        diffusionR[0].setDelay(r0);
        diffusionR[1].setDelay(r1);

        const float coeff = juce::jlimit(0.18f, 0.74f, 0.32f + texture * 0.28f + (float) modeIndex * 0.03f);
        for (auto& ap : diffusionL) ap.coeff = coeff;
        for (auto& ap : diffusionR) ap.coeff = coeff - 0.03f;
    }

    void advancePhase(float& phase, float rateHz) noexcept
    {
        phase += rateHz / (float) sr;
        if (phase >= 1.0f)
            phase -= 1.0f;
    }

    void advanceReversePhases(float windowSamples) noexcept
    {
        const float safeWindow = juce::jmax(2.0f, windowSamples);

        reversePhaseA += 1.0f;
        while (reversePhaseA >= safeWindow)
            reversePhaseA -= safeWindow;

        reversePhaseB += 1.0f;
        while (reversePhaseB >= safeWindow)
            reversePhaseB -= safeWindow;
    }

    double sr = 44100.0;

    Nova::DelayDSP::DelayLine delayL, delayR;
    std::array<Nova::DelayDSP::Biquad, 2> toneLPF;
    std::array<Nova::DelayDSP::Biquad, 2> fbHighPass;
    std::array<Nova::DelayDSP::Biquad, 2> dcBlock;
    std::array<Nova::DelayDSP::Biquad, 2> freezeLPF;
    std::array<Nova::DelayDSP::AllPassFilter, 2> diffusionL;
    std::array<Nova::DelayDSP::AllPassFilter, 2> diffusionR;
    Nova::DelayDSP::EnvelopeFollower duckFollower;
    Nova::DelayDSP::EnvelopeFollower swellFollower;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> timeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowCutSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> spreadSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> textureSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> modDepthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> modRateSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> duckSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> swellSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverseSmooth;

    float lfoPhase = 0.0f;
    float lfoPhase2 = 0.0f;
    float flutterPhase = 0.0f;
    float flutterPhase2 = 0.0f;
    float reversePhaseA = 0.0f;
    float reversePhaseB = 72.0f;
    int swellHoldSamples = 0;

    float holdStateL = 0.0f;
    float holdStateR = 0.0f;
    bool freezeActive = false;
    int freezeAgeSamples = 0;

    int cachedModeIndex = -1;
    float cachedTexture = -1.0f;
    float cachedToneCutoff = -1.0f;
    float cachedLowCut = -1.0f;
    std::atomic<float> hostTempoBpm{ 120.0f };
    std::atomic<bool> hostTempoValid{ false };
    std::atomic<bool> hostTransportPlaying{ false };
    std::atomic<int> lastAppliedFlagshipPreset{ -1 };
    bool isPrepared = false;
    NovaDiagnostics::PedalSignalTelemetry signalTelemetry { "delay" };
    DebugTelemetry debugTelemetry;
};

#include "DelayEditor.h"
