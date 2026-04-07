#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
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

inline float reverseWindowEnvelope(float phase, float windowSamples) noexcept
{
    if (windowSamples <= 1.0f)
        return 0.0f;

    const float normalised = juce::jlimit(0.0f, 1.0f, phase / windowSamples);
    return std::sin(normalised * juce::MathConstants<float>::pi);
}

inline float renderReverseWindow(const DelayLine& line,
    float baseDelaySamples,
    float windowSamples,
    float phaseA,
    float phaseB) noexcept
{
    const auto renderVoice = [&](float phase)
    {
        const float delay = baseDelaySamples + phase;
        const float sample = line.readLinear(delay);
        return sample * reverseWindowEnvelope(phase, windowSamples);
    };

    return renderVoice(phaseA) + renderVoice(phaseB) * 0.18f;
}

}} // namespace Nova::DelayDSP

class DelayPedal final : public ProcessorBase
{
public:
    DelayPedal()
    {
        addParameter(modeParam = new juce::AudioParameterChoice("delayMode",
            "Mode",
            juce::StringArray{ "Analog", "Tape", "Digital", "Reverse" },
            0));
        addParameter(timeParam     = new juce::AudioParameterFloat("delayTime",     "Time",     35.0f, 2500.0f, 480.0f));
        addParameter(feedbackParam = new juce::AudioParameterFloat("delayFeedback", "Feedback", 0.0f, 0.97f,   0.46f));
        addParameter(toneParam     = new juce::AudioParameterFloat("delayTone",     "Tone",     600.0f, 14000.0f, 5800.0f));
        addParameter(spreadParam   = new juce::AudioParameterFloat("delaySpread",   "Spread",   0.0f,  1.0f,    0.42f));
        addParameter(textureParam  = new juce::AudioParameterFloat("delayTexture",  "Texture",  0.0f,  1.0f,    0.45f));
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
        initialiseSmoother(spreadSmooth, spreadParam, 0.42f, 0.05);
        initialiseSmoother(textureSmooth, textureParam, 0.45f, 0.05);
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
        initialiseSmoother(spreadSmooth, spreadParam, 0.42f, 0.05);
        initialiseSmoother(textureSmooth, textureParam, 0.45f, 0.05);
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
        cachedModeIndex = -1;
        cachedTexture = -1.0f;
        cachedToneCutoff = -1.0f;

        lastWetL = 0.0f;
        lastWetR = 0.0f;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        setTargetFromParam(timeSmooth, timeParam, 480.0f);
        setTargetFromParam(feedbackSmooth, feedbackParam, 0.46f);
        setTargetFromParam(spreadSmooth, spreadParam, 0.42f);
        setTargetFromParam(textureSmooth, textureParam, 0.45f);
        setTargetFromParam(mixSmooth, mixParam, 0.32f);
        setTargetFromParam(duckSmooth, duckParam, 0.0f);
        setTargetFromParam(swellSmooth, swellParam, 0.0f);
        setTargetFromParam(reverseSmooth, reverseParam, 0.0f);

        const int modeIndex = modeParam != nullptr ? modeParam->getIndex() : 0;
        const float toneCutoff = toneParam != nullptr ? toneParam->get() : 5800.0f;
        const float previewTexture = textureParam != nullptr ? textureParam->get() : 0.45f;
        updateFeedbackFilters(modeIndex, previewTexture, toneCutoff);

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();
        const float maxDelaySamples = (float) (delayL.size - 4);
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        float blockWetPeakL = 0.0f;
        float blockWetPeakR = 0.0f;
        const bool freezeRequested = freezeParam != nullptr && freezeParam->get();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float timeMs = timeSmooth.getNextValue();
            const float feedback = feedbackSmooth.getNextValue();
            const float spread = spreadSmooth.getNextValue();
            const float texture = textureSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();
            const float duck = duckSmooth.getNextValue();
            const float swell = swellSmooth.getNextValue();
            const float reverseAmount = reverseSmooth.getNextValue();

            const float inL = buffer.getSample(0, sample);
            const float inR = numChannels > 1 ? buffer.getSample(1, sample) : inL;
            const float inputSense = 0.5f * (std::abs(inL) + std::abs(inR));

            const auto style = getModeStyle(modeIndex, texture, reverseAmount);
            const float baseDelay = juce::jlimit(1.0f, maxDelaySamples, timeMs * (float) sr * 0.001f);

            const float modDepth1 = style.modDepthMs1 * (float) sr * 0.001f;
            const float modDepth2 = style.modDepthMs2 * (float) sr * 0.001f;
            const float flutterDepth = style.flutterDepthMs * (float) sr * 0.001f;

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
                reversePhaseB) * style.reverseTrim;
            const float reverseWetR = Nova::DelayDSP::renderReverseWindow(delayR,
                reverseBaseDelay,
                reverseWindow,
                reversePhaseA,
                reversePhaseB) * style.reverseTrim;

            const float reverseBlend = juce::jlimit(0.0f, 1.0f, style.reverseBase + reverseAmount * style.reverseDepth);
            const float wetL = wetNormalL * (1.0f - reverseBlend) + reverseWetL * reverseBlend;
            const float wetR = wetNormalR * (1.0f - reverseBlend) + reverseWetR * reverseBlend;
            float wetOutL = wetL;
            float wetOutR = wetR;

            const float duckEnv = juce::jlimit(0.0f, 1.0f, duckFollower.process(inputSense) * 3.8f);
            const float swellEnv = juce::jlimit(0.0f, 1.0f, swellFollower.process(inputSense) * 4.3f);
            const float duckGain = 1.0f - duck * duckEnv * 0.82f;
            const int targetSwellHold = juce::jlimit(0,
                (int) (sr * 0.85),
                (int) std::round(baseDelay * (0.18f + 0.42f * swell)));
            if (swell > 0.001f && inputSense > 0.015f)
                swellHoldSamples = juce::jmax(swellHoldSamples, targetSwellHold);
            if (swellHoldSamples > 0)
                --swellHoldSamples;

            const float timedSwellNormalised = targetSwellHold > 0
                ? juce::jlimit(0.0f, 1.0f, (float) swellHoldSamples / (float) targetSwellHold)
                : 0.0f;
            const float timedSwellGain = 1.0f - swell * std::pow(timedSwellNormalised, 0.78f) * 0.92f;
            const float followerSwellGain = 1.0f - swell * swellEnv * 0.32f;
            const float swellGain = juce::jlimit(0.06f, 1.0f, juce::jmin(timedSwellGain, followerSwellGain));
            const float performanceGain = juce::jlimit(0.08f, 1.0f, duckGain * swellGain);

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

            fbL = Nova::DelayDSP::softSaturate(fbL, style.drive) * feedback * style.feedbackTrim;
            fbR = Nova::DelayDSP::softSaturate(fbR, style.drive) * feedback * style.feedbackTrim;

            if (freezeRequested)
            {
                if (!freezeActive)
                {
                    holdStateL = wetL;
                    holdStateR = wetR;
                    freezeActive = true;
                }

                holdStateL = freezeLPF[0].process(Nova::DelayDSP::softSaturate(holdStateL * 1.0040f + wetL * 0.050f, 1.62f));
                holdStateR = freezeLPF[1].process(Nova::DelayDSP::softSaturate(holdStateR * 1.0040f + wetR * 0.050f, 1.62f));
                delayL.write(holdStateL);
                delayR.write(holdStateR);
                wetOutL = holdStateL;
                wetOutR = holdStateR;
            }
            else
            {
                freezeActive = false;
                delayL.write(inL + fbL);
                delayR.write(inR + fbR);
            }

            const float reverseOutputComp = 1.0f + reverseBlend * 0.42f;
            const float outWetL = dcBlock[0].process(wetOutL * performanceGain * style.outputTrim * reverseOutputComp);
            const float outWetR = dcBlock[1].process(wetOutR * performanceGain * style.outputTrim * reverseOutputComp);

            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            buffer.setSample(0, sample, inL * dryGain + outWetL * wetGain);
            if (numChannels > 1)
                buffer.setSample(1, sample, inR * dryGain + outWetR * wetGain);

            blockWetPeakL = juce::jmax(blockWetPeakL, std::abs(outWetL));
            blockWetPeakR = juce::jmax(blockWetPeakR, std::abs(outWetR));

            advancePhase(lfoPhase, style.modRate1);
            advancePhase(lfoPhase2, style.modRate2);
            advancePhase(flutterPhase, style.flutterRate1);
            advancePhase(flutterPhase2, style.flutterRate2);
            advanceReversePhases(reverseWindow);
        }

        lastWetL = blockWetPeakL;
        lastWetR = blockWetPeakR;

        endBypassProcess(buffer);
    }

    juce::AudioParameterChoice* modeParam = nullptr;
    juce::AudioParameterFloat* timeParam = nullptr;
    juce::AudioParameterFloat* feedbackParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* spreadParam = nullptr;
    juce::AudioParameterFloat* textureParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* duckParam = nullptr;
    juce::AudioParameterFloat* swellParam = nullptr;
    juce::AudioParameterFloat* reverseParam = nullptr;
    juce::AudioParameterBool* freezeParam = nullptr;

    float lastWetL = 0.0f;
    float lastWetR = 0.0f;

private:
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
        float outputTrim = 1.0f;
    };

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
                style.outputTrim = 0.96f;
                break;
        }

        return style;
    }

    void updateFeedbackFilters(int modeIndex, float texture, float toneCutoff)
    {
        if (modeIndex == cachedModeIndex
            && std::abs(cachedTexture - texture) < 0.0025f
            && std::abs(cachedToneCutoff - toneCutoff) < 2.0f)
        {
            return;
        }

        cachedModeIndex = modeIndex;
        cachedTexture = texture;
        cachedToneCutoff = toneCutoff;

        const auto style = getModeStyle(modeIndex, texture, reverseParam != nullptr ? reverseParam->get() : 0.0f);
        const float cutoff = juce::jlimit(650.0f, 18000.0f, toneCutoff * style.toneScale);

        for (auto& lp : toneLPF)
            lp.setLowPass(cutoff, 0.707f, sr);
        for (auto& hp : fbHighPass)
            hp.setHighPass(style.hpCutoff, 0.707f, sr);
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
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> spreadSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> textureSmooth;
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

    int cachedModeIndex = -1;
    float cachedTexture = -1.0f;
    float cachedToneCutoff = -1.0f;
    bool isPrepared = false;
};

#include "DelayEditor.h"
