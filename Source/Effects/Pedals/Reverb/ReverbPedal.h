#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <vector>

// ============================================================================
//  Nimbus — Professional Reverb Engine
//  8-line FDN · Early Reflections · Two-Band Damping · Shimmer
//  Modes: Spring · Plate · Hall · Room · Shimmer · Cloud
// ============================================================================
namespace Nova { namespace Reverb {

static constexpr int NUM_LINES     = 8;
static constexpr int NUM_DIFFUSERS = 6;
static constexpr int NUM_DISP_AP   = 3;
static constexpr int MAX_ER_TAPS   = 12;

static constexpr float kInvSqrt8 = 0.35355339f;
static constexpr float kTwoPi    = juce::MathConstants<float>::twoPi;

inline float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

// ======================= DSP Primitives ===================================

struct CircularDelay
{
    std::vector<float> buf;
    int writePos = 0;
    int length   = 1;

    void allocate(int maxLen)
    {
        buf.assign((size_t)juce::jmax(4, maxLen), 0.0f);
        writePos = 0;
        length   = (int)buf.size();
    }

    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        writePos = 0;
    }

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
        float cd  = juce::jlimit(0.0f, (float)(len - 1), delay);
        float rp  = (float)writePos - cd;
        if (rp < 0.0f) rp += (float)len;
        int   i0  = ((int)rp) % len;
        int   i1  = (i0 + 1) % len;
        float frc = rp - std::floor(rp);
        return buf[(size_t)i0] + (buf[(size_t)i1] - buf[(size_t)i0]) * frc;
    }

    float readCubic(float delay) const noexcept
    {
        if (buf.empty()) return 0.0f;
        const int len = juce::jmax(1, length);
        float cd  = juce::jlimit(1.0f, (float)(len - 2), delay);
        float rp  = (float)writePos - cd;
        if (rp < 0.0f) rp += (float)len;
        int i1 = ((int)rp) % len;
        int i0 = (i1 - 1 + len) % len;
        int i2 = (i1 + 1) % len;
        int i3 = (i1 + 2) % len;
        float f  = rp - std::floor(rp);
        float y0 = buf[(size_t)i0], y1 = buf[(size_t)i1];
        float y2 = buf[(size_t)i2], y3 = buf[(size_t)i3];
        float a  = y3 - y2 - y0 + y1;
        float b  = y0 - y1 - a;
        float c  = y2 - y0;
        return y1 + f * (c + f * (b + f * a));
    }

    float readNearest(int delay) const noexcept
    {
        if (buf.empty()) return 0.0f;
        const int len = juce::jmax(1, length);
        int rp = writePos - juce::jlimit(1, len - 1, delay);
        if (rp < 0) rp += len;
        return buf[(size_t)rp];
    }
};

struct AllPassStage
{
    std::vector<float> buf;
    int writePos = 0;
    int delay    = 1;
    float gain   = 0.5f;

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
        float wd  = buf[(size_t)rIdx];
        float w   = input + gain * wd;
        buf[(size_t)writePos] = w;
        float out = -gain * w + wd;
        if (++writePos >= (int)buf.size()) writePos = 0;
        return out;
    }
};

struct OnePoleLP
{
    float state = 0.0f, a0 = 0.2f, b1 = 0.8f;

    void setCutoff(float hz, double sr)
    {
        float c = juce::jlimit(20.0f, (float)(sr * 0.45), hz);
        b1 = (float)std::exp(-kTwoPi * (double)c / sr);
        a0 = 1.0f - b1;
    }

    float process(float x) noexcept { state = a0 * x + b1 * state; return state; }
    void  reset() { state = 0.0f; }
};

struct OnePoleHP
{
    float x1 = 0.0f, y1 = 0.0f, pole = 0.99f;

    void setCutoff(float hz, double sr)
    {
        float c = juce::jlimit(10.0f, (float)(sr * 0.45), hz);
        pole = (float)std::exp(-kTwoPi * (double)c / sr);
    }

    float process(float x) noexcept
    {
        float y = x - x1 + pole * y1;
        x1 = x; y1 = y;
        return y;
    }

    void reset() { x1 = y1 = 0.0f; }
};

struct DCBlocker
{
    float x1 = 0.0f, y1 = 0.0f;
    static constexpr float R = 0.9975f;

    float process(float x) noexcept
    {
        float y = x - x1 + R * y1;
        x1 = x;  y1 = y;
        return y;
    }

    void reset() { x1 = y1 = 0.0f; }
};

// Two-band damper: crossover splits into bass/treble with independent attenuation
struct TwoBandDamper
{
    float lpState    = 0.0f;
    float coeff      = 0.15f;
    float bassAtten  = 1.0f;
    float trebAtten  = 1.0f;

    void setCrossover(float hz, double sr)
    {
        coeff = 1.0f - (float)std::exp(-kTwoPi * (double)juce::jlimit(60.0f, 4000.0f, hz) / sr);
    }

    float process(float x) noexcept
    {
        lpState += coeff * (x - lpState);
        return lpState * bassAtten + (x - lpState) * trebAtten;
    }

    void reset() { lpState = 0.0f; }
};

// Grain-based pitch shifter — octave-up for shimmer feedback
struct GrainPitchShifter
{
    std::vector<float> buf;
    int   writePos   = 0;
    int   bufSize    = 1;
    float grainSize  = 2048.0f;
    float delay_[2]  = { 0.0f, 0.0f };
    float pitchRatio = 2.0f;
    float targetRatio = 2.0f;
    OnePoleLP smoothLP;

    void prepare(double sr)
    {
        bufSize   = (int)(sr * 0.25) + 64;
        buf.assign((size_t)bufSize, 0.0f);
        grainSize = (float)(sr * 0.038);          // ~38 ms grains
        delay_[0] = grainSize;
        delay_[1] = grainSize * 0.5f;
        smoothLP.setCutoff(6000.0f, sr);           // de-alias filter
        smoothLP.reset();
    }

    void setPitchRatio(float ratio) { targetRatio = juce::jlimit(0.5f, 4.0f, ratio); }

    void reset()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        writePos   = 0;
        delay_[0]  = grainSize;
        delay_[1]  = grainSize * 0.5f;
        pitchRatio = targetRatio;
        smoothLP.reset();
    }

    float process(float input) noexcept
    {
        // Smooth pitch ratio changes (one-pole, ~5ms)
        pitchRatio += 0.002f * (targetRatio - pitchRatio);

        buf[(size_t)writePos] = input;

        float output  = 0.0f;
        const float advance = pitchRatio - 1.0f;

        for (int g = 0; g < 2; ++g)
        {
            float pos    = 1.0f - delay_[g] / grainSize;
            float window = 0.5f * (1.0f - std::cos(kTwoPi * pos));

            float rd = juce::jlimit(1.0f, (float)(bufSize - 2), delay_[g]);
            float rp = (float)writePos - rd;
            if (rp < 0.0f) rp += (float)bufSize;
            int   i0   = ((int)rp) % bufSize;
            int   i1   = (i0 + 1) % bufSize;
            float frac = rp - std::floor(rp);
            output += (buf[(size_t)i0] + (buf[(size_t)i1] - buf[(size_t)i0]) * frac) * window;

            delay_[g] -= advance;
            if (delay_[g] <= 0.0f)
                delay_[g] += grainSize;
        }

        if (++writePos >= bufSize) writePos = 0;
        return smoothLP.process(output);
    }
};

// ======================= Early Reflections =================================

struct ERTap
{
    float delayMs;
    float gain;
    float pan;          // -1 left … +1 right
};

struct ERTapComputed
{
    int   delaySamples;
    float gainL, gainR;
};

// ---- Per-mode ER tap patterns (reference 48 kHz) ----

static constexpr int kSpringERCount = 6;
static constexpr ERTap kSpringER[kSpringERCount] = {
    { 3.2f, 0.72f, -0.25f }, { 7.5f, 0.55f,  0.40f },
    { 13.1f, 0.40f, -0.55f }, { 19.8f, 0.28f,  0.60f },
    { 28.4f, 0.18f, -0.40f }, { 38.2f, 0.10f,  0.30f }
};

static constexpr int kPlateERCount = 12;
static constexpr ERTap kPlateER[kPlateERCount] = {
    { 0.9f, 0.82f,  0.00f }, { 1.7f, 0.78f, -0.20f },
    { 2.6f, 0.72f,  0.30f }, { 3.8f, 0.66f, -0.40f },
    { 5.2f, 0.60f,  0.50f }, { 7.0f, 0.54f, -0.35f },
    { 9.3f, 0.47f,  0.45f }, { 12.1f, 0.39f, -0.50f },
    { 15.5f, 0.31f,  0.25f }, { 19.7f, 0.24f, -0.35f },
    { 24.8f, 0.17f,  0.40f }, { 31.0f, 0.11f, -0.20f }
};

static constexpr int kHallERCount = 10;
static constexpr ERTap kHallER[kHallERCount] = {
    { 13.2f, 0.62f, -0.70f }, { 19.8f, 0.56f,  0.55f },
    { 26.4f, 0.49f, -0.35f }, { 33.7f, 0.42f,  0.80f },
    { 42.1f, 0.36f, -0.60f }, { 51.8f, 0.30f,  0.45f },
    { 63.2f, 0.24f, -0.50f }, { 76.5f, 0.18f,  0.70f },
    { 91.8f, 0.12f, -0.40f }, { 109.0f, 0.07f,  0.35f }
};

static constexpr int kRoomERCount = 10;
static constexpr ERTap kRoomER[kRoomERCount] = {
    { 2.3f, 0.74f, -0.35f }, { 5.1f, 0.67f,  0.50f },
    { 8.4f, 0.59f, -0.60f }, { 12.2f, 0.51f,  0.35f },
    { 16.8f, 0.43f, -0.50f }, { 22.1f, 0.36f,  0.65f },
    { 28.3f, 0.28f, -0.30f }, { 35.4f, 0.21f,  0.45f },
    { 43.6f, 0.14f, -0.55f }, { 53.0f, 0.08f,  0.25f }
};

static constexpr int kCloudERCount = 4;
static constexpr ERTap kCloudER[kCloudERCount] = {
    { 8.0f, 0.30f, -0.40f }, { 18.0f, 0.22f,  0.50f },
    { 32.0f, 0.15f, -0.30f }, { 50.0f, 0.08f,  0.40f }
};

struct EarlyReflections
{
    CircularDelay buf;
    std::array<ERTapComputed, MAX_ER_TAPS> taps{};
    int numTaps = 0;

    void prepare(double sr)
    {
        buf.allocate((int)(sr * 0.15) + 64);
    }

    void reset() { buf.clear(); }

    void configure(const ERTap* tapData, int count, double sr, float sizeScale, float width)
    {
        numTaps = juce::jmin(count, MAX_ER_TAPS);
        for (int i = 0; i < numTaps; ++i)
        {
            float delayMs = tapData[i].delayMs * sizeScale;
            taps[(size_t)i].delaySamples = juce::jlimit(1, (int)(sr * 0.14),
                (int)std::round(delayMs * 0.001 * sr));
            float p = (juce::jlimit(-1.0f, 1.0f, tapData[i].pan * width) + 1.0f) * 0.5f;
            taps[(size_t)i].gainL = tapData[i].gain * std::sqrt(1.0f - p);
            taps[(size_t)i].gainR = tapData[i].gain * std::sqrt(p);
        }
    }

    void process(float monoIn, float& outL, float& outR) noexcept
    {
        buf.write(monoIn);
        outL = outR = 0.0f;
        for (int i = 0; i < numTaps; ++i)
        {
            float s = buf.readNearest(taps[(size_t)i].delaySamples);
            outL += s * taps[(size_t)i].gainL;
            outR += s * taps[(size_t)i].gainR;
        }
    }
};

// ======================= Mode Tuning Data ==================================

struct ModeTuning
{
    std::array<int, NUM_LINES>     fdnDelays;
    std::array<int, NUM_DIFFUSERS> diffuserDelays;
    std::array<int, NUM_DISP_AP>   dispersionDelays;
    float diffuserGain;
    float dispersionGain;
    float crossoverHz;
    float hpCutoff;
    float dampLpMin;           // bright end (damping = 0)
    float dampLpMax;           // dark end   (damping = 1)
    float feedbackMin;
    float feedbackMax;
    float bassFeedbackScale;   // relative bass attenuation in loop
    float modDepthRef;         // samples at 48 kHz
    float sizeMin, sizeMax;    // delay multiplier range
    bool  useDispersion;
    bool  useShimmer;
    float shimmerMix;
    float shimmerRatio;    // pitch shift ratio (2.0 = octave up, 1.5 = fifth)
    const ERTap* erTaps;
    int   erTapCount;
};

static const ModeTuning kSpring = {
    { 997, 1153, 1327, 1487, 1657, 1823, 2003, 2179 },
    { 107, 191, 293, 409, 521, 647 },
    { 13, 23, 37 },
    0.58f, 0.50f,
    800.0f, 160.0f,
    2000.0f, 8000.0f,
    0.50f, 0.88f, 0.85f,
    3.5f, 0.6f, 1.2f,
    true, false, 0.0f, 2.0f,
    kSpringER, kSpringERCount
};

static const ModeTuning kPlate = {
    { 1301, 1559, 1789, 2039, 2293, 2539, 2791, 3067 },
    { 149, 257, 383, 541, 701, 881 },
    { 0, 0, 0 },
    0.70f, 0.0f,
    1000.0f, 100.0f,
    3000.0f, 12000.0f,
    0.55f, 0.93f, 0.92f,
    5.0f, 0.5f, 1.5f,
    false, false, 0.0f, 2.0f,
    kPlateER, kPlateERCount
};

static const ModeTuning kHall = {
    { 2003, 2381, 2791, 3203, 3593, 3989, 4397, 4793 },
    { 211, 373, 547, 761, 971, 1187 },
    { 0, 0, 0 },
    0.63f, 0.0f,
    600.0f, 60.0f,
    2200.0f, 9000.0f,
    0.60f, 0.95f, 1.05f,
    7.0f, 0.4f, 2.0f,
    false, false, 0.0f, 2.0f,
    kHallER, kHallERCount
};

static const ModeTuning kRoom = {
    { 601, 787, 941, 1097, 1259, 1423, 1583, 1741 },
    { 79, 151, 233, 337, 449, 571 },
    { 0, 0, 0 },
    0.65f, 0.0f,
    900.0f, 120.0f,
    3500.0f, 10000.0f,
    0.40f, 0.82f, 0.90f,
    2.5f, 0.3f, 1.4f,
    false, false, 0.0f, 2.0f,
    kRoomER, kRoomERCount
};

static const ModeTuning kShimmer = {
    { 2003, 2381, 2791, 3203, 3593, 3989, 4397, 4793 },
    { 211, 373, 547, 761, 971, 1187 },
    { 0, 0, 0 },
    0.65f, 0.0f,
    700.0f, 80.0f,
    2000.0f, 7500.0f,
    0.55f, 0.92f, 0.88f,
    8.0f, 0.5f, 1.8f,
    false, true, 0.45f, 2.0f,
    kHallER, kHallERCount
};

static const ModeTuning kCloud = {
    { 1747, 2111, 2473, 2837, 3203, 3571, 3931, 4297 },
    { 181, 331, 487, 661, 853, 1049 },
    { 0, 0, 0 },
    0.72f, 0.0f,
    500.0f, 80.0f,
    1800.0f, 6500.0f,
    0.65f, 0.998f, 1.08f,
    10.0f, 0.6f, 2.2f,
    false, false, 0.0f, 2.0f,
    kCloudER, kCloudERCount
};

inline const ModeTuning& tuningForMode(int mode)
{
    switch (mode)
    {
        case 0:  return kSpring;
        case 1:  return kPlate;
        case 2:  return kHall;
        case 3:  return kRoom;
        case 4:  return kShimmer;
        default: return kCloud;
    }
}

// Incommensurate LFO rates per line (Hz) — avoids beating
static constexpr std::array<float, NUM_LINES> kModRates =
    { 0.31f, 0.43f, 0.57f, 0.71f, 0.83f, 0.97f, 1.13f, 1.29f };

// ---- In-place Fast Walsh-Hadamard Transform (N = 8) ----
inline void hadamard8(std::array<float, NUM_LINES>& x) noexcept
{
    // Stage 1 — pairs
    for (int i = 0; i < 8; i += 2)
    {
        float a = x[(size_t)i], b = x[(size_t)(i + 1)];
        x[(size_t)i]       = a + b;
        x[(size_t)(i + 1)] = a - b;
    }
    // Stage 2 — quads
    for (int i = 0; i < 8; i += 4)
    {
        float a0 = x[(size_t)i],       a2 = x[(size_t)(i + 2)];
        float a1 = x[(size_t)(i + 1)], a3 = x[(size_t)(i + 3)];
        x[(size_t)i]       = a0 + a2;
        x[(size_t)(i + 2)] = a0 - a2;
        x[(size_t)(i + 1)] = a1 + a3;
        x[(size_t)(i + 3)] = a1 - a3;
    }
    // Stage 3 — octets
    for (int i = 0; i < 4; ++i)
    {
        float a = x[(size_t)i], b = x[(size_t)(i + 4)];
        x[(size_t)i]       = a + b;
        x[(size_t)(i + 4)] = a - b;
    }
    // Normalize (unitary)
    for (int i = 0; i < 8; ++i)
        x[(size_t)i] *= kInvSqrt8;
}

// Stereo output tap signs (Hadamard rows 2 & 3 for decorrelation)
static constexpr std::array<float, NUM_LINES> kTapL =
    { +1, -1, +1, -1, +1, -1, +1, -1 };
static constexpr std::array<float, NUM_LINES> kTapR =
    { +1, +1, -1, -1, +1, +1, -1, -1 };

// ======================= Main Reverb Engine ================================

class Engine
{
public:
    void prepare(double sampleRate)
    {
        sr         = juce::jmax(1.0, sampleRate);
        ratioToRef = sr / 48000.0;

        predelayMid.allocate((int)(sr * 0.32) + 64);
        predelaySide.allocate((int)(sr * 0.32) + 64);
        predelaySmooth.reset(sr, 0.04);
        predelaySmooth.setCurrentAndTargetValue(0.0f);

        for (auto& d : diffusers)
            d.allocate((int)(1500 * ratioToRef) + 16);
        for (auto& d : dispersion)
            d.allocate((int)(64 * ratioToRef) + 16);
        for (auto& line : lines)
            line.allocate((int)(12000 * ratioToRef) + 64);

        for (auto& d : dampers) d.reset();
        for (auto& hp : dampHP) hp.reset();
        for (auto& lp : dampLP) lp.reset();

        er.prepare(sr);
        shimmer.prepare(sr);
        shimmerLP.setCutoff(5500.0f, sr);
        outputToneL.setCutoff(7500.0f, sr);
        outputToneR.setCutoff(7500.0f, sr);

        dcL.reset();  dcR.reset();
        modPhases.fill(0.0f);

        feedbackSmooth.reset(sr, 0.06);
        feedbackSmooth.setCurrentAndTargetValue(0.7f);
        modDepthSmooth.reset(sr, 0.06);
        modDepthSmooth.setCurrentAndTargetValue(0.0f);
        erLevelSmooth.reset(sr, 0.04);
        erLevelSmooth.setCurrentAndTargetValue(0.5f);
        widthSmooth.reset(sr, 0.05);
        widthSmooth.setCurrentAndTargetValue(1.0f);
        outToneHzSmooth.reset(sr, 0.05);
        outToneHzSmooth.setCurrentAndTargetValue(7500.0f);

        reset();
    }

    void reset()
    {
        predelayMid.clear();
        predelaySide.clear();
        for (auto& d : diffusers)  d.clear();
        for (auto& d : dispersion) d.clear();
        for (auto& line : lines)   line.clear();
        for (auto& d : dampers)    d.reset();
        for (auto& hp : dampHP)    hp.reset();
        for (auto& lp : dampLP)    lp.reset();
        er.reset();
        shimmer.reset();
        shimmerLP.reset();
        outputToneL.reset();
        outputToneR.reset();
        dcL.reset();  dcR.reset();
        modPhases.fill(0.0f);
    }

    // Call when any parameter changes (cheap — not per-sample)
    void configure(int mode, float decay01, float size01, float tone01,
                   float damping01, float bassCut01, float diffusion01,
                   float width01, float modAmount01, float predelayMs)
    {
        const auto& t = tuningForMode(mode);
        currentMode   = mode;

        // ---- Size scaling ----
        float sizeMult = t.sizeMin + (t.sizeMax - t.sizeMin) * size01;

        for (int i = 0; i < NUM_LINES; ++i)
            lines[i].setLength(juce::jmax(4,
                (int)std::round(t.fdnDelays[(size_t)i] * ratioToRef * sizeMult)));

        // ---- Diffusion ----
        float diffSizeScale = juce::jlimit(0.7f, 1.3f, sizeMult);
        float diffuserGain = juce::jlimit(0.45f, 0.82f,
            t.diffuserGain * lerp(0.78f, 1.18f, diffusion01));
        for (int i = 0; i < NUM_DIFFUSERS; ++i)
        {
            diffusers[i].setDelay(juce::jmax(1,
                (int)std::round(t.diffuserDelays[(size_t)i] * ratioToRef * diffSizeScale)));
            diffusers[i].gain = diffuserGain;
        }

        // ---- Spring dispersion ----
        useDisp = t.useDispersion;
        if (useDisp)
        {
            for (int i = 0; i < NUM_DISP_AP; ++i)
            {
                dispersion[i].setDelay(
                    (int)std::round(t.dispersionDelays[(size_t)i] * ratioToRef));
                dispersion[i].gain = t.dispersionGain;
            }
        }

        // ---- Feedback ----
        float fb = t.feedbackMin + (t.feedbackMax - t.feedbackMin) * decay01;
        feedbackSmooth.setTargetValue(fb);

        // ---- Two-band damping (independent bass/treble decay) ----
        // trebleAtten: 1.0 at bright end → 0.40 at dark end
        float trebAtten = 1.0f - damping01 * 0.60f;
        // bassAtten: mode baseline, reduced further by bassCut
        float bassAtten = t.bassFeedbackScale * (1.0f - bassCut01 * 0.50f);

        for (int i = 0; i < NUM_LINES; ++i)
        {
            dampers[i].setCrossover(t.crossoverHz, sr);
            dampers[i].bassAtten  = bassAtten;
            dampers[i].trebAtten  = trebAtten;
            dampHP[i].setCutoff(t.hpCutoff, sr);
            dampLP[i].setCutoff(lerp(t.dampLpMax, t.dampLpMin, tone01), sr);
        }

        // ---- Modulation depth ----
        modDepthSmooth.setTargetValue(
            t.modDepthRef * (float)ratioToRef * modAmount01);

        // ---- Early reflections ----
        er.configure(t.erTaps, t.erTapCount, sr, sizeMult, juce::jlimit(0.25f, 1.25f, width01));
        float erLevel = (mode == 5) ? 0.25f :      // Cloud — minimal ER
                        (mode == 0) ? 0.55f : 0.5f; // Spring — strong ER drip
        erLevelSmooth.setTargetValue(erLevel);
        widthSmooth.setTargetValue(width01);

        // ---- Pre-delay ----
        float pdSamples = juce::jlimit(0.0f, (float)(predelayMid.length - 4),
            predelayMs * (float)(sr * 0.001));
        predelaySmooth.setTargetValue(pdSamples);

        // ---- Shimmer ----
        useShimmer  = t.useShimmer;
        shimmerMix_ = t.shimmerMix;
        shimmer.setPitchRatio(t.shimmerRatio);
        shimmerLP.setCutoff(lerp(3500.0f, 8200.0f, tone01), sr);
        outToneHzSmooth.setTargetValue(lerp(2800.0f, t.dampLpMin * 1.15f, tone01));
    }

    void processSample(float inL, float inR, float& outL, float& outR) noexcept
    {
        const float fb       = feedbackSmooth.getNextValue();
        const float modDepth = modDepthSmooth.getNextValue();
        const float erLevel  = erLevelSmooth.getNextValue();
        const float width    = widthSmooth.getNextValue();
        const float toneHz   = outToneHzSmooth.getNextValue();
        outputToneL.setCutoff(toneHz, sr);
        outputToneR.setCutoff(toneHz, sr);

        const float midIn  = (inL + inR) * 0.5f;
        const float sideIn = (inL - inR) * 0.5f;

        // ---- Pre-delay (shared by ER + late) ----
        predelayMid.write(midIn);
        predelaySide.write(sideIn);
        float pdSamp  = juce::jlimit(0.0f, (float)(predelayMid.length - 2),
                            predelaySmooth.getNextValue());
        const float delayedMid  = predelayMid.readLinear(juce::jmax(1.0f, pdSamp));
        const float delayedSide = predelaySide.readLinear(juce::jmax(1.0f, pdSamp));

        // ---- Early reflections ----
        float erL = 0.0f, erR = 0.0f;
        er.process(delayedMid, erL, erR);
        erL += delayedSide * 0.08f * width;
        erR -= delayedSide * 0.08f * width;

        // ---- Input diffusion (6 cascaded all-pass stages) ----
        float diffused = delayedMid;
        for (auto& d : diffusers)
            diffused = d.process(diffused);

        // Spring dispersion (chirp)
        if (useDisp)
            for (auto& d : dispersion)
                diffused = d.process(diffused);

        // ---- Read FDN lines (modulated, cubic interpolation) ----
        std::array<float, NUM_LINES> lineOut{};
        for (int i = 0; i < NUM_LINES; ++i)
        {
            float mod = std::sin(kTwoPi * modPhases[(size_t)i]) * modDepth;
            float baseD = juce::jmax(4.0f, (float)lines[i].length - 1.0f);
            float readD = juce::jlimit(2.0f, baseD - 2.0f, baseD + mod);
            lineOut[(size_t)i] = lines[i].readCubic(readD);

            modPhases[(size_t)i] += kModRates[(size_t)i] / (float)sr;
            if (modPhases[(size_t)i] >= 1.0f) modPhases[(size_t)i] -= 1.0f;
        }

        // ---- Two-band damping + HP ----
        for (int i = 0; i < NUM_LINES; ++i)
        {
            lineOut[(size_t)i] = dampers[i].process(lineOut[(size_t)i]);
            lineOut[(size_t)i] = dampHP[i].process(lineOut[(size_t)i]);
            lineOut[(size_t)i] = dampLP[i].process(lineOut[(size_t)i]);
        }

        // ---- Stereo output taps (before Hadamard — taps from damped lines) ----
        float lateMid = 0.0f, lateSide = 0.0f;
        for (int i = 0; i < NUM_LINES; ++i)
        {
            lateMid  += kTapL[(size_t)i] * lineOut[(size_t)i];
            lateSide += kTapR[(size_t)i] * lineOut[(size_t)i];
        }
        float lateL = (lateMid + lateSide * width) * kInvSqrt8;
        float lateR = (lateMid - lateSide * width) * kInvSqrt8;

        // ---- Hadamard mixing (feedback path) ----
        hadamard8(lineOut);

        // ---- Shimmer (pitch shift in feedback) ----
        float shimSig = 0.0f;
        if (useShimmer)
        {
            float monoFdn = 0.0f;
            for (int i = 0; i < NUM_LINES; ++i)
                monoFdn += lineOut[(size_t)i];
            monoFdn *= kInvSqrt8;
            shimSig  = shimmerLP.process(shimmer.process(monoFdn)) * shimmerMix_;
        }

        // ---- Write back into FDN lines ----
        for (int i = 0; i < NUM_LINES; ++i)
        {
            float fbSig = fb * lineOut[(size_t)i];
            // Soft-limit feedback (transparent below ±2.5, compresses above)
            fbSig = std::tanh(fbSig * 0.4f) * 2.5f;
            const float stereoExcite = delayedSide * width * 0.35f * kTapR[(size_t)i];
            float inp = (diffused + stereoExcite) * kInvSqrt8 + fbSig;
            if (useShimmer) inp += shimSig * kInvSqrt8;
            lines[i].write(inp);
        }

        // ---- Spring saturation (subtle warmth) ----
        if (currentMode == 0)
        {
            lateL = std::tanh(lateL * 1.15f) / 1.15f;
            lateR = std::tanh(lateR * 1.15f) / 1.15f;
        }

        lateL = outputToneL.process(lateL);
        lateR = outputToneR.process(lateR);

        // ---- Blend ER + late reverb ----
        outL = erL * erLevel + lateL;
        outR = erR * erLevel + lateR;

        // ---- DC blocking ----
        outL = dcL.process(outL);
        outR = dcR.process(outR);
    }

    void setShimmerRatio(float ratio) { shimmer.setPitchRatio(ratio); }

    // Block-based processing — hoists mode branches and pre-computes mod increments
    void processBlock(const float* inL, const float* inR, float* outL, float* outR,
                      int numSamples) noexcept
    {
        // Pre-compute per-line mod phase increments (constant across block)
        std::array<float, NUM_LINES> modInc{};
        for (int i = 0; i < NUM_LINES; ++i)
            modInc[(size_t)i] = kModRates[(size_t)i] / (float)sr;

        const bool isSpring = (currentMode == 0);

        for (int s = 0; s < numSamples; ++s)
        {
            const float fb       = feedbackSmooth.getNextValue();
            const float modDepth = modDepthSmooth.getNextValue();
            const float erLevel  = erLevelSmooth.getNextValue();
            const float width    = widthSmooth.getNextValue();
            const float toneHz   = outToneHzSmooth.getNextValue();
            outputToneL.setCutoff(toneHz, sr);
            outputToneR.setCutoff(toneHz, sr);

            const float sL = inL[s];
            const float sR = inR[s];
            const float midIn  = (sL + sR) * 0.5f;
            const float sideIn = (sL - sR) * 0.5f;

            predelayMid.write(midIn);
            predelaySide.write(sideIn);
            float pdSamp = juce::jlimit(0.0f, (float)(predelayMid.length - 2),
                               predelaySmooth.getNextValue());
            const float delayedMid  = predelayMid.readLinear(juce::jmax(1.0f, pdSamp));
            const float delayedSide = predelaySide.readLinear(juce::jmax(1.0f, pdSamp));

            float erL = 0.0f, erR = 0.0f;
            er.process(delayedMid, erL, erR);
            erL += delayedSide * 0.08f * width;
            erR -= delayedSide * 0.08f * width;

            float diffused = delayedMid;
            for (auto& d : diffusers)
                diffused = d.process(diffused);
            if (useDisp)
                for (auto& d : dispersion)
                    diffused = d.process(diffused);

            // FDN read + damping (unrolled inner loop)
            std::array<float, NUM_LINES> lineOut{};
            for (int i = 0; i < NUM_LINES; ++i)
            {
                const float mod = std::sin(kTwoPi * modPhases[(size_t)i]) * modDepth;
                const float baseD = juce::jmax(4.0f, (float)lines[i].length - 1.0f);
                lineOut[(size_t)i] = lines[i].readCubic(
                    juce::jlimit(2.0f, baseD - 2.0f, baseD + mod));
                modPhases[(size_t)i] += modInc[(size_t)i];
                if (modPhases[(size_t)i] >= 1.0f) modPhases[(size_t)i] -= 1.0f;
            }

            for (int i = 0; i < NUM_LINES; ++i)
            {
                lineOut[(size_t)i] = dampers[i].process(lineOut[(size_t)i]);
                lineOut[(size_t)i] = dampHP[i].process(lineOut[(size_t)i]);
                lineOut[(size_t)i] = dampLP[i].process(lineOut[(size_t)i]);
            }

            float lateMid = 0.0f, lateSide = 0.0f;
            for (int i = 0; i < NUM_LINES; ++i)
            {
                lateMid  += kTapL[(size_t)i] * lineOut[(size_t)i];
                lateSide += kTapR[(size_t)i] * lineOut[(size_t)i];
            }
            float lateL = (lateMid + lateSide * width) * kInvSqrt8;
            float lateR = (lateMid - lateSide * width) * kInvSqrt8;

            hadamard8(lineOut);

            float shimSig = 0.0f;
            if (useShimmer)
            {
                float monoFdn = 0.0f;
                for (int i = 0; i < NUM_LINES; ++i)
                    monoFdn += lineOut[(size_t)i];
                monoFdn *= kInvSqrt8;
                shimSig  = shimmerLP.process(shimmer.process(monoFdn)) * shimmerMix_;
            }

            for (int i = 0; i < NUM_LINES; ++i)
            {
                float fbSig = std::tanh(fb * lineOut[(size_t)i] * 0.4f) * 2.5f;
                const float stereoExcite = delayedSide * width * 0.35f * kTapR[(size_t)i];
                float inp = (diffused + stereoExcite) * kInvSqrt8 + fbSig;
                if (useShimmer) inp += shimSig * kInvSqrt8;
                lines[i].write(inp);
            }

            if (isSpring)
            {
                lateL = std::tanh(lateL * 1.15f) * 0.8695652f;  // 1/1.15
                lateR = std::tanh(lateR * 1.15f) * 0.8695652f;
            }

            lateL = outputToneL.process(lateL);
            lateR = outputToneR.process(lateR);

            outL[s] = dcL.process(erL * erLevel + lateL);
            outR[s] = dcR.process(erR * erLevel + lateR);
        }
    }

    int currentMode = 0;

private:
    double sr         = 48000.0;
    double ratioToRef = 1.0;

    CircularDelay predelayMid;
    CircularDelay predelaySide;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> predelaySmooth;

    std::array<AllPassStage, NUM_DIFFUSERS> diffusers;
    std::array<AllPassStage, NUM_DISP_AP>   dispersion;
    bool useDisp = false;

    std::array<CircularDelay, NUM_LINES>  lines;
    std::array<TwoBandDamper, NUM_LINES>  dampers;
    std::array<OnePoleHP, NUM_LINES>      dampHP;
    std::array<OnePoleLP, NUM_LINES>      dampLP;
    std::array<float, NUM_LINES>          modPhases{};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> modDepthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> erLevelSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outToneHzSmooth;

    EarlyReflections   er;
    GrainPitchShifter  shimmer;
    OnePoleLP          shimmerLP;
    OnePoleLP          outputToneL, outputToneR;
    bool  useShimmer   = false;
    float shimmerMix_  = 0.0f;

    DCBlocker dcL, dcR;
};

}} // namespace Nova::Reverb


// ============================================================================
//  ReverbPedal — Processor
// ============================================================================
class ReverbPedal final : public ProcessorBase
{
public:
    ReverbPedal()
    {
        addParameter(modeParam = new juce::AudioParameterChoice(
            "reverbMode", "Mode",
            { "Spring", "Plate", "Hall", "Room", "Shimmer", "Cloud" }, 0));
        addParameter(decayParam = new juce::AudioParameterFloat(
            "reverbDecay", "Decay", 0.0f, 1.0f, 0.50f));
        addParameter(toneParam = new juce::AudioParameterFloat(
            "reverbTone", "Tone", 0.0f, 1.0f, 0.62f));
        addParameter(sizeParam = new juce::AudioParameterFloat(
            "reverbSize", "Size", 0.0f, 1.0f, 0.50f));
        addParameter(dampingParam = new juce::AudioParameterFloat(
            "reverbDamping", "Damping", 0.0f, 1.0f, 0.35f));
        addParameter(bassCutParam = new juce::AudioParameterFloat(
            "reverbBassCut", "Bass Cut", 0.0f, 1.0f, 0.20f));
        addParameter(diffusionParam = new juce::AudioParameterFloat(
            "reverbDiffusion", "Diffusion", 0.0f, 1.0f, 0.75f));
        addParameter(widthParam = new juce::AudioParameterFloat(
            "reverbWidth", "Width", 0.0f, 1.0f, 0.90f));
        addParameter(modParam = new juce::AudioParameterFloat(
            "reverbMod", "Mod", 0.0f, 1.0f, 0.30f));
        addParameter(predelayParam = new juce::AudioParameterFloat(
            "reverbPredelay", "Predelay", 0.0f, 250.0f, 15.0f));
        addParameter(mixParam = new juce::AudioParameterFloat(
            "reverbMix", "Mix", 0.0f, 1.0f, 0.28f));
        addParameter(shimmerPitchParam = new juce::AudioParameterChoice(
            "reverbShimmerPitch", "Shimmer Pitch",
            { "Fifth", "Octave", "Octave+5th", "2 Octaves" }, 1));
    }

    const juce::String getName() const override { return "Reverb"; }
    double getTailLengthSeconds() const override { return 16.0; }
    bool   hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0) return;
        currentSampleRate = sampleRate;
        engine.prepare(sampleRate);

        mixSmooth.reset(sampleRate, 0.04);
        mixSmooth.setCurrentAndTargetValue(mixParam ? mixParam->get() : 0.28f);

        prepareBypassSmoother(sampleRate, samplesPerBlock);

        lastMode = -1;
        lastDecay = lastTone = lastSize = lastDamp = -1.0f;
        lastBass = lastDiff = lastWidth = lastMod = lastPD = -1.0f;
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        engine.reset();
        if (mixParam) mixSmooth.setCurrentAndTargetValue(mixParam->get());
    }

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        juce::XmlElement xml("NIMBUS_REVERB_STATE");
        xml.setAttribute("version", 3);
        xml.setAttribute("modeIndex", modeParam ? modeParam->getIndex() : 0);
        xml.setAttribute("shimmerPitchIndex", shimmerPitchParam ? shimmerPitchParam->getIndex() : 1);
        writeFloat(xml, "reverbDecay", decayParam);
        writeFloat(xml, "reverbTone", toneParam);
        writeFloat(xml, "reverbSize", sizeParam);
        writeFloat(xml, "reverbDamping", dampingParam);
        writeFloat(xml, "reverbBassCut", bassCutParam);
        writeFloat(xml, "reverbDiffusion", diffusionParam);
        writeFloat(xml, "reverbWidth", widthParam);
        writeFloat(xml, "reverbMod", modParam);
        writeFloat(xml, "reverbPredelay", predelayParam);
        writeFloat(xml, "reverbMix", mixParam);
        copyXmlToBinary(xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        auto xml = std::unique_ptr<juce::XmlElement>(getXmlFromBinary(data, sizeInBytes));
        if (xml == nullptr)
            return;

        if (xml->hasTagName("NIMBUS_REVERB_STATE"))
        {
            setChoiceIndex(modeParam, xml->getIntAttribute("modeIndex", 0));
            setChoiceIndex(shimmerPitchParam, xml->getIntAttribute("shimmerPitchIndex", 1));
            restoreFloat(*xml, "reverbDecay", decayParam);
            restoreFloat(*xml, "reverbTone", toneParam);
            restoreFloat(*xml, "reverbSize", sizeParam);
            restoreFloat(*xml, "reverbDamping", dampingParam);
            restoreFloat(*xml, "reverbBassCut", bassCutParam);
            restoreFloat(*xml, "reverbDiffusion", diffusionParam);
            restoreFloat(*xml, "reverbWidth", widthParam);
            restoreFloat(*xml, "reverbMod", modParam);
            restoreFloat(*xml, "reverbPredelay", predelayParam);
            restoreFloat(*xml, "reverbMix", mixParam);
        }
        else if (xml->hasTagName("PLUGIN_STATE"))
        {
            bool hasLegacyTone = false;
            bool hasSize = false;
            for (auto* child : xml->getChildIterator())
            {
                if (!child->hasTagName("PARAM"))
                    continue;
                const auto id = child->getStringAttribute("id");
                hasLegacyTone = hasLegacyTone || id == "reverbTone";
                hasSize = hasSize || id == "reverbSize";
            }

            const bool legacyThreeMode = hasLegacyTone && !hasSize;
            for (auto* child : xml->getChildIterator())
            {
                if (!child->hasTagName("PARAM"))
                    continue;

                const auto id = child->getStringAttribute("id");
                const float value = (float)child->getDoubleAttribute("value");

                if (id == "reverbMode")
                {
                    if (legacyThreeMode)
                        setChoiceIndex(modeParam, juce::jlimit(0, 2, juce::roundToInt(value * 2.0f)));
                    else if (modeParam)
                        modeParam->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
                    continue;
                }

                restoreLegacyFloat(id, value);
            }
        }

        lastMode = -1;
        lastDecay = lastTone = lastSize = lastDamp = -1.0f;
        lastBass = lastDiff = lastWidth = lastMod = lastPD = -1.0f;
        if (mixParam) mixSmooth.setCurrentAndTargetValue(mixParam->get());
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::ScopedNoDenormals noDenormals;

        // ---- Read parameters ----
        const int   mode    = modeParam    ? modeParam->getIndex()  : 0;
        const float decay   = decayParam   ? decayParam->get()      : 0.5f;
        const float tone    = toneParam    ? toneParam->get()       : 0.62f;
        const float size    = sizeParam    ? sizeParam->get()       : 0.5f;
        const float damp    = dampingParam ? dampingParam->get()    : 0.35f;
        const float bass    = bassCutParam ? bassCutParam->get()    : 0.20f;
        const float diff    = diffusionParam ? diffusionParam->get(): 0.75f;
        const float width   = widthParam   ? widthParam->get()      : 0.90f;
        const float mod     = modParam     ? modParam->get()        : 0.30f;
        const float pdMs    = predelayParam? predelayParam->get()   : 15.0f;

        // Reconfigure only when parameters actually change
        if (mode != lastMode
            || std::abs(decay - lastDecay) > 1e-4f
            || std::abs(tone  - lastTone)  > 1e-4f
            || std::abs(size  - lastSize)  > 1e-4f
            || std::abs(damp  - lastDamp)  > 1e-4f
            || std::abs(bass  - lastBass)  > 1e-4f
            || std::abs(diff  - lastDiff)  > 1e-4f
            || std::abs(width - lastWidth) > 1e-4f
            || std::abs(mod   - lastMod)   > 1e-4f
            || std::abs(pdMs  - lastPD)    > 0.1f)
        {
            engine.configure(mode, decay, size, tone, damp, bass, diff, width, mod, pdMs);
            lastMode  = mode;
            lastDecay = decay;
            lastTone  = tone;
            lastSize  = size;
            lastDamp  = damp;
            lastBass  = bass;
            lastDiff  = diff;
            lastWidth = width;
            lastMod   = mod;
            lastPD    = pdMs;
        }

        // Apply user shimmer pitch override (only affects Shimmer mode)
        if (mode == 4 && shimmerPitchParam)
        {
            static constexpr float kShimmerRatios[] = { 1.4983f, 2.0f, 2.9966f, 4.0f };
            const int pitchIdx = shimmerPitchParam->getIndex();
            engine.setShimmerRatio(kShimmerRatios[juce::jlimit(0, 3, pitchIdx)]);
        }

        mixSmooth.setTargetValue(mixParam ? mixParam->get() : 0.28f);

        const int numSamples  = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        // Copy input to scratch buffers for block processing
        const float* srcL = buffer.getReadPointer(0);
        const float* srcR = numChannels > 1 ? buffer.getReadPointer(1) : srcL;

        // Use the buffer itself as wet output workspace
        auto* dstL = buffer.getWritePointer(0);
        auto* dstR = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;

        // Process wet signal in-place using scratch
        juce::AudioBuffer<float> dryBuf(numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
            dryBuf.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        // Block-process engine
        if (numChannels > 1)
        {
            engine.processBlock(dryBuf.getReadPointer(0), dryBuf.getReadPointer(1),
                                dstL, dstR, numSamples);
        }
        else
        {
            engine.processBlock(dryBuf.getReadPointer(0), dryBuf.getReadPointer(0),
                                dstL, dstL, numSamples);
        }

        // Equal-power dry/wet mix
        const float* dryL = dryBuf.getReadPointer(0);
        const float* dryR = numChannels > 1 ? dryBuf.getReadPointer(1) : dryL;
        for (int s = 0; s < numSamples; ++s)
        {
            const float m   = mixSmooth.getNextValue();
            const float dry = std::cos(m * halfPi);
            const float wet = std::sin(m * halfPi);

            dstL[s] = dryL[s] * dry + dstL[s] * wet;
            if (dstR != nullptr)
                dstR[s] = dryR[s] * dry + dstR[s] * wet;
        }

        endBypassProcess(buffer);
    }

    // Public accessors for the editor
    juce::AudioParameterChoice* modeParam     = nullptr;
    juce::AudioParameterFloat*  decayParam    = nullptr;
    juce::AudioParameterFloat*  toneParam     = nullptr;
    juce::AudioParameterFloat*  sizeParam     = nullptr;
    juce::AudioParameterFloat*  dampingParam  = nullptr;
    juce::AudioParameterFloat*  bassCutParam  = nullptr;
    juce::AudioParameterFloat*  diffusionParam = nullptr;
    juce::AudioParameterFloat*  widthParam    = nullptr;
    juce::AudioParameterFloat*  modParam      = nullptr;
    juce::AudioParameterFloat*  predelayParam = nullptr;
    juce::AudioParameterFloat*  mixParam      = nullptr;
    juce::AudioParameterChoice* shimmerPitchParam = nullptr;

private:
    static void writeFloat(juce::XmlElement& xml, const char* name, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            xml.setAttribute(name, (double)param->get());
    }

    static void restoreFloat(const juce::XmlElement& xml, const char* name, juce::AudioParameterFloat* param)
    {
        if (param != nullptr && xml.hasAttribute(name))
            param->setValueNotifyingHost(param->convertTo0to1((float)xml.getDoubleAttribute(name)));
    }

    static void setChoiceIndex(juce::AudioParameterChoice* param, int index)
    {
        if (param == nullptr)
            return;

        const int clamped = juce::jlimit(0, param->choices.size() - 1, index);
        const float normalised = param->choices.size() > 1
            ? (float)clamped / (float)(param->choices.size() - 1)
            : 0.0f;
        param->setValueNotifyingHost(normalised);
    }

    void restoreLegacyFloat(const juce::String& id, float normalised)
    {
        auto apply = [normalised](juce::AudioParameterFloat* param)
        {
            if (param != nullptr)
                param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normalised));
        };

        if (id == "reverbDecay") apply(decayParam);
        else if (id == "reverbTone") apply(toneParam);
        else if (id == "reverbSize") apply(sizeParam);
        else if (id == "reverbDamping") apply(dampingParam);
        else if (id == "reverbBassCut") apply(bassCutParam);
        else if (id == "reverbDiffusion") apply(diffusionParam);
        else if (id == "reverbWidth") apply(widthParam);
        else if (id == "reverbMod") apply(modParam);
        else if (id == "reverbPredelay") apply(predelayParam);
        else if (id == "reverbMix") apply(mixParam);
    }

    Nova::Reverb::Engine engine;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    double currentSampleRate = 44100.0;
    int    lastMode  = -1;
    float  lastDecay = -1.0f, lastTone = -1.0f, lastSize = -1.0f, lastDamp = -1.0f;
    float  lastBass  = -1.0f, lastDiff = -1.0f, lastWidth = -1.0f, lastMod  = -1.0f, lastPD   = -1.0f;
    bool   isPrepared = false;
};

#include "ReverbEditor.h"
