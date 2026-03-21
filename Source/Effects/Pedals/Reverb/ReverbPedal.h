#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <vector>

// ============================================================================
//  FDN Reverb Engine — 8-line Feedback Delay Network
//  Modes: Spring (Fender-style drip), Plate (EMT 140 density), Hall (concert space)
// ============================================================================
namespace Nova { namespace FDN {

static constexpr int NUM_LINES     = 8;
static constexpr int NUM_DIFFUSERS = 4;
static constexpr int NUM_DISP_AP   = 3;   // Spring dispersion all-passes

// ---- Mode tuning data (delay times in samples at 48 kHz reference) ----
struct ModeTuning
{
    std::array<int, NUM_LINES>     fdnDelays;
    std::array<int, NUM_DIFFUSERS> diffuserDelays;
    std::array<int, NUM_DISP_AP>   dispersionDelays;  // Spring only
    float diffuserGain;
    float dispersionGain;
    float hpCutoff;        // Hz
    float lpCutoffMin;     // Hz at tone = 0
    float lpCutoffMax;     // Hz at tone = 1
    float feedbackMin;     // at decay = 0
    float feedbackMax;     // at decay = 1
    float modDepthRef;     // modulation depth in samples at 48 kHz
    bool  useDispersion;
};

static constexpr ModeTuning kSpring = {
    { 1013, 1259, 1423, 1567, 1733, 1907, 2089, 2239 },
    { 113, 199, 307, 421 },
    { 13, 23, 37 },
    0.58f, 0.50f,
    160.0f,
    1800.0f, 5500.0f,
    0.55f, 0.91f,
    4.0f, true
};

static constexpr ModeTuning kPlate = {
    { 1327, 1583, 1811, 2069, 2311, 2557, 2803, 3089 },
    { 151, 263, 389, 547 },
    { 0, 0, 0 },
    0.70f, 0.0f,
    120.0f,
    2500.0f, 10000.0f,
    0.58f, 0.945f,
    6.0f, false
};

static constexpr ModeTuning kHall = {
    { 2017, 2389, 2801, 3209, 3607, 4001, 4409, 4801 },
    { 211, 379, 557, 769 },
    { 0, 0, 0 },
    0.63f, 0.0f,
    80.0f,
    2000.0f, 8000.0f,
    0.62f, 0.968f,
    8.0f, false
};

inline const ModeTuning& tuningForMode(int mode)
{
    switch (mode)
    {
        case 0:  return kSpring;
        case 1:  return kPlate;
        default: return kHall;
    }
}

// LFO rates per line (Hz) — incommensurate to avoid beating
static constexpr std::array<float, NUM_LINES> kModRates =
    { 0.31f, 0.43f, 0.55f, 0.67f, 0.79f, 0.91f, 1.07f, 1.19f };

// Stereo output taps (Hadamard rows, normalized)
static constexpr float kInvSqrt8 = 0.35355339f;
static constexpr std::array<float, NUM_LINES> kTapL = { +1, +1, -1, -1, +1, +1, -1, -1 };
static constexpr std::array<float, NUM_LINES> kTapR = { +1, -1, +1, -1, +1, -1, +1, -1 };

// ---- DSP primitives ----

struct CircularDelay
{
    std::vector<float> buf;
    int writePos = 0;
    int length = 1;

    void allocate(int maxLen)
    {
        buf.assign((size_t)juce::jmax(4, maxLen), 0.0f);
        writePos = 0;
        length = (int)buf.size();
    }

    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); writePos = 0; }

    void setLength(int len)
    {
        length = juce::jlimit(1, (int)buf.size(), len);
        if (writePos >= length) writePos = 0;
    }

    void write(float sample)
    {
        buf[(size_t)writePos] = sample;
        if (++writePos >= length) writePos = 0;
    }

    float readLinear(float delay) const noexcept
    {
        if (buf.empty()) return 0.0f;
        const int len = juce::jmax(1, length);
        float clampedDelay = juce::jlimit(0.0f, (float)(len - 1), delay);
        float rp = (float)writePos - clampedDelay;
        while (rp < 0.0f) rp += (float)len;
        int i0 = ((int)rp) % len;
        int i1 = (i0 + 1) % len;
        float frac = rp - std::floor(rp);
        return buf[(size_t)i0] + (buf[(size_t)i1] - buf[(size_t)i0]) * frac;
    }

    float readCubic(float delay) const noexcept
    {
        if (buf.empty()) return 0.0f;
        const int len = juce::jmax(1, length);
        float clampedDelay = juce::jlimit(1.0f, (float)(len - 2), delay);
        float rp = (float)writePos - clampedDelay;
        while (rp < 0.0f) rp += (float)len;
        int i1 = ((int)rp) % len;
        int i0 = (i1 - 1 + len) % len;
        int i2 = (i1 + 1) % len;
        int i3 = (i1 + 2) % len;
        float f = rp - std::floor(rp);

        float y0 = buf[(size_t)i0], y1 = buf[(size_t)i1];
        float y2 = buf[(size_t)i2], y3 = buf[(size_t)i3];
        float a = y3 - y2 - y0 + y1;
        float b = y0 - y1 - a;
        float c = y2 - y0;
        return y1 + f * (c + f * (b + f * a));
    }
};

struct AllPassStage
{
    std::vector<float> buf;
    int writePos = 0;
    int delay = 1;
    float gain = 0.5f;

    void allocate(int maxLen)
    {
        buf.assign((size_t)juce::jmax(4, maxLen), 0.0f);
        writePos = 0;
    }

    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); writePos = 0; }

    void setDelay(int d) { delay = juce::jlimit(1, (int)buf.size() - 1, d); }

    float process(float input) noexcept
    {
        int rIdx = writePos - delay;
        if (rIdx < 0) rIdx += (int)buf.size();
        float wd = buf[(size_t)rIdx];
        float w = input + gain * wd;
        buf[(size_t)writePos] = w;
        float output = -gain * w + wd;
        if (++writePos >= (int)buf.size()) writePos = 0;
        return output;
    }
};

struct OnePoleLP
{
    float state = 0.0f, a0 = 0.2f, b1 = 0.8f;

    void setCutoff(float hz, double sr)
    {
        float c = juce::jlimit(20.0f, (float)(sr * 0.45), hz);
        b1 = (float)std::exp(-juce::MathConstants<double>::twoPi * c / sr);
        a0 = 1.0f - b1;
    }

    float process(float x) noexcept { state = a0 * x + b1 * state; return state; }
    void reset() { state = 0.0f; }
};

struct OnePoleHP
{
    float x1 = 0.0f, y1 = 0.0f, pole = 0.99f;

    void setCutoff(float hz, double sr)
    {
        float c = juce::jlimit(10.0f, (float)(sr * 0.45), hz);
        pole = (float)std::exp(-juce::MathConstants<double>::twoPi * c / sr);
    }

    float process(float x) noexcept
    {
        float y = x - x1 + pole * y1;
        x1 = x; y1 = y;
        return y;
    }

    void reset() { x1 = y1 = 0.0f; }
};

// ---- Main FDN Engine ----
class Engine
{
public:
    void prepare(double sampleRate)
    {
        sr = juce::jmax(1.0, sampleRate);
        ratioToRef = sr / 48000.0;

        // Pre-delay: up to 200 ms
        predelay.allocate((int)(sr * 0.22) + 64);
        predelayTarget.reset(sr, 0.04);
        predelayTarget.setCurrentAndTargetValue(0.0f);

        // Diffusers
        for (auto& d : diffusers)
            d.allocate((int)(1024 * ratioToRef) + 16);

        // Spring dispersion
        for (auto& d : dispersion)
            d.allocate((int)(64 * ratioToRef) + 16);

        // FDN lines
        for (auto& line : lines)
            line.allocate((int)(6144 * ratioToRef) + 64);

        // Damping
        for (auto& lp : dampLP) lp.reset();
        for (auto& hp : dampHP) hp.reset();

        // Tone output
        toneOutL.reset(); toneOutR.reset();

        // Mod phases
        modPhases.fill(0.0f);

        // Smoothers
        feedbackSmooth.reset(sr, 0.06);
        feedbackSmooth.setCurrentAndTargetValue(0.7f);
        diffGainSmooth.reset(sr, 0.06);
        diffGainSmooth.setCurrentAndTargetValue(0.6f);

        reset();
    }

    void reset()
    {
        predelay.clear();
        for (auto& d : diffusers) d.clear();
        for (auto& d : dispersion) d.clear();
        for (auto& line : lines) line.clear();
        for (auto& lp : dampLP) lp.reset();
        for (auto& hp : dampHP) hp.reset();
        toneOutL.reset(); toneOutR.reset();
        modPhases.fill(0.0f);
    }

    void configure(int mode, float decay01, float tone01, float predelayMs)
    {
        const auto& t = tuningForMode(mode);
        currentMode = mode;

        // Set FDN delay lengths (scaled to sample rate)
        for (int i = 0; i < NUM_LINES; ++i)
            lines[i].setLength((int)std::round(t.fdnDelays[(size_t)i] * ratioToRef));

        // Set diffuser delays and gain
        for (int i = 0; i < NUM_DIFFUSERS; ++i)
        {
            diffusers[i].setDelay((int)std::round(t.diffuserDelays[(size_t)i] * ratioToRef));
            diffusers[i].gain = t.diffuserGain;
        }

        diffGainSmooth.setTargetValue(t.diffuserGain);

        // Spring dispersion
        if (t.useDispersion)
        {
            for (int i = 0; i < NUM_DISP_AP; ++i)
            {
                dispersion[i].setDelay((int)std::round(t.dispersionDelays[(size_t)i] * ratioToRef));
                dispersion[i].gain = t.dispersionGain;
            }
        }
        useDisp = t.useDispersion;

        // Feedback gain from decay
        float fb = t.feedbackMin + (t.feedbackMax - t.feedbackMin) * decay01;
        feedbackSmooth.setTargetValue(fb);

        // Damping filters
        float lpFreq = t.lpCutoffMin + (t.lpCutoffMax - t.lpCutoffMin) * tone01;
        for (auto& lp : dampLP) lp.setCutoff(lpFreq, sr);
        for (auto& hp : dampHP) hp.setCutoff(t.hpCutoff, sr);

        // Output tone
        toneOutL.setCutoff(lpFreq * 1.3f, sr);
        toneOutR.setCutoff(lpFreq * 1.3f, sr);

        // Mod depth
        modDepth = t.modDepthRef * (float)ratioToRef;

        // Pre-delay
        float pdSamples = juce::jlimit(0.0f, (float)(predelay.length - 4),
            predelayMs * (float)sr * 0.001f);
        predelayTarget.setTargetValue(pdSamples);
    }

    void processSample(float inL, float inR, float& outL, float& outR)
    {
        const float fb = feedbackSmooth.getNextValue();

        // Mono sum input
        float monoIn = (inL + inR) * 0.5f;

        // Pre-delay
        predelay.write(monoIn);
        float pdSamples = juce::jlimit(0.0f, (float)(predelay.length - 2),
            predelayTarget.getNextValue());
        float delayed = predelay.readLinear(juce::jmax(1.0f, pdSamples));

        // Input diffusion
        float diffused = delayed;
        for (auto& d : diffusers)
            diffused = d.process(diffused);

        // Spring dispersion (chirp)
        if (useDisp)
        {
            for (auto& d : dispersion)
                diffused = d.process(diffused);
        }

        // Read FDN lines (with modulation + cubic interpolation)
        std::array<float, NUM_LINES> lineOut{};
        for (int i = 0; i < NUM_LINES; ++i)
        {
            float mod = std::sin(juce::MathConstants<float>::twoPi * modPhases[(size_t)i]) * modDepth;
            float baseDelay = juce::jmax(4.0f, (float)lines[i].length - 1.0f);
            float readDelay = juce::jlimit(2.0f, baseDelay - 2.0f, baseDelay + mod);
            lineOut[(size_t)i] = lines[i].readCubic(readDelay);

            // Advance mod LFO
            modPhases[(size_t)i] += kModRates[(size_t)i] / (float)sr;
            if (modPhases[(size_t)i] >= 1.0f) modPhases[(size_t)i] -= 1.0f;
        }

        // Apply damping to each line output
        for (int i = 0; i < NUM_LINES; ++i)
        {
            lineOut[(size_t)i] = dampLP[i].process(lineOut[(size_t)i]);
            lineOut[(size_t)i] = dampHP[i].process(lineOut[(size_t)i]);
        }

        // Householder mixing: y[i] = x[i] - (2/N) * sum(x)
        float sum = 0.0f;
        for (int i = 0; i < NUM_LINES; ++i) sum += lineOut[(size_t)i];
        float mixFactor = sum * 0.25f; // 2/8 = 0.25

        for (int i = 0; i < NUM_LINES; ++i)
        {
            float mixed = lineOut[(size_t)i] - mixFactor;
            // Write back: input diffused + feedback * mixed
            lines[i].write(diffused * (1.0f / (float)NUM_LINES) + fb * mixed);
        }

        // Stereo output taps (Hadamard decorrelation)
        float rawL = 0.0f, rawR = 0.0f;
        for (int i = 0; i < NUM_LINES; ++i)
        {
            rawL += kTapL[(size_t)i] * lineOut[(size_t)i];
            rawR += kTapR[(size_t)i] * lineOut[(size_t)i];
        }
        rawL *= kInvSqrt8;
        rawR *= kInvSqrt8;

        // Output tone
        outL = toneOutL.process(rawL);
        outR = toneOutR.process(rawR);
    }

    int currentMode = 0;

private:
    double sr = 48000.0;
    double ratioToRef = 1.0;

    CircularDelay predelay;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> predelayTarget;

    std::array<AllPassStage, NUM_DIFFUSERS> diffusers;
    std::array<AllPassStage, NUM_DISP_AP> dispersion;
    bool useDisp = false;

    std::array<CircularDelay, NUM_LINES> lines;
    std::array<OnePoleLP, NUM_LINES> dampLP;
    std::array<OnePoleHP, NUM_LINES> dampHP;
    std::array<float, NUM_LINES> modPhases{};
    float modDepth = 4.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> diffGainSmooth;

    OnePoleLP toneOutL, toneOutR;
};

}} // namespace Nova::FDN


// ============================================================================
//  ReverbPedal — Processor
// ============================================================================
class ReverbPedal final : public ProcessorBase
{
public:
    ReverbPedal()
    {
        addParameter(modeParam = new juce::AudioParameterChoice(
            "reverbMode", "Mode", { "Spring", "Plate", "Hall" }, 0));
        addParameter(decayParam = new juce::AudioParameterFloat(
            "reverbDecay", "Decay", 0.0f, 1.0f, 0.50f));
        addParameter(toneParam = new juce::AudioParameterFloat(
            "reverbTone", "Tone", 0.0f, 1.0f, 0.50f));
        addParameter(predelayParam = new juce::AudioParameterFloat(
            "reverbPredelay", "Predelay", 0.0f, 150.0f, 20.0f));
        addParameter(mixParam = new juce::AudioParameterFloat(
            "reverbMix", "Mix", 0.0f, 1.0f, 0.28f));
    }

    const juce::String getName() const override { return "Reverb"; }
    double getTailLengthSeconds() const override { return 8.0; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        currentSampleRate = sampleRate;
        engine.prepare(sampleRate);

        mixSmooth.reset(sampleRate, 0.04);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? mixParam->get() : 0.28f);

        prepareBypassSmoother(sampleRate, samplesPerBlock);

        lastMode = -1; // Force configure on first block
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        engine.reset();
        if (mixParam != nullptr) mixSmooth.setCurrentAndTargetValue(mixParam->get());
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        // Read parameters
        const int mode = modeParam != nullptr ? modeParam->getIndex() : 0;
        const float decay = decayParam != nullptr ? decayParam->get() : 0.5f;
        const float tone = toneParam != nullptr ? toneParam->get() : 0.5f;
        const float pdMs = predelayParam != nullptr ? predelayParam->get() : 20.0f;

        // Reconfigure engine if anything changed (cheap check)
        if (mode != lastMode || std::abs(decay - lastDecay) > 1.0e-4f
            || std::abs(tone - lastTone) > 1.0e-4f || std::abs(pdMs - lastPD) > 0.1f)
        {
            engine.configure(mode, decay, tone, pdMs);
            lastMode = mode;
            lastDecay = decay;
            lastTone = tone;
            lastPD = pdMs;
        }

        mixSmooth.setTargetValue(mixParam != nullptr ? mixParam->get() : 0.28f);

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float inL = buffer.getSample(0, sample);
            const float inR = numChannels > 1 ? buffer.getSample(1, sample) : inL;

            float wetL, wetR;
            engine.processSample(inL, inR, wetL, wetR);

            const float mix = mixSmooth.getNextValue();
            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            buffer.setSample(0, sample, inL * dryGain + wetL * wetGain);
            if (numChannels > 1)
                buffer.setSample(1, sample, inR * dryGain + wetR * wetGain);
        }

        endBypassProcess(buffer);
    }

    // Public accessors for the editor
    juce::AudioParameterChoice* modeParam = nullptr;
    juce::AudioParameterFloat*  decayParam = nullptr;
    juce::AudioParameterFloat*  toneParam = nullptr;
    juce::AudioParameterFloat*  predelayParam = nullptr;
    juce::AudioParameterFloat*  mixParam = nullptr;

private:
    Nova::FDN::Engine engine;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    double currentSampleRate = 44100.0;
    int   lastMode = -1;
    float lastDecay = -1.0f;
    float lastTone = -1.0f;
    float lastPD = -1.0f;
    bool  isPrepared = false;
};

#include "ReverbEditor.h"
