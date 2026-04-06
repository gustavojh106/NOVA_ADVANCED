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

struct CharacterDelayVoice
{
    CircularDelay delay;
    OnePoleHP hp;
    OnePoleLP lp;
    float readSamples = 1.0f;
    float feedback = 0.0f;

    void prepare(double sr, float maxSeconds)
    {
        delay.allocate((int)(sr * maxSeconds) + 64);
        reset();
    }

    void reset()
    {
        delay.clear();
        hp.reset();
        lp.reset();
    }

    void configure(double sr, float delayMs, float hpHz, float lpHz, float fb)
    {
        readSamples = juce::jlimit(1.0f,
            (float)juce::jmax(4, delay.length - 2),
            delayMs * (float)(sr * 0.001));
        delay.setLength(juce::jmax(4, (int)std::ceil(readSamples) + 4));
        hp.setCutoff(hpHz, sr);
        lp.setCutoff(lpHz, sr);
        feedback = juce::jlimit(0.0f, 0.92f, fb);
    }

    float process(float input) noexcept
    {
        const float tap = delay.readLinear(readSamples);
        delay.write(input + tap * feedback);
        return lp.process(hp.process(tap));
    }
};

struct PerformanceGains
{
    float duckGain = 1.0f;
    float swellGain = 1.0f;
    float gateGain = 1.0f;
    float gateFeedbackScale = 1.0f;
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

// Grain-based pitch shifter — multi-ratio shimmer feedback
// 4 overlapping grains with adaptive grain size for clean transposition
struct GrainPitchShifter
{
    static constexpr int NUM_GRAINS = 4;

    std::vector<float> buf;
    int   writePos   = 0;
    int   bufSize    = 1;
    float baseGrainSize = 2048.0f;
    float grainSize  = 2048.0f;
    float delay_[NUM_GRAINS] = {};
    float pitchRatio = 2.0f;
    float targetRatio = 2.0f;
    float smoothCoeff = 0.0005f;       // ~40ms convergence at 48 kHz
    OnePoleLP smoothLP;
    double sampleRate = 48000.0;

    void prepare(double sr)
    {
        sampleRate = sr;
        bufSize   = (int)(sr * 0.35) + 64;
        buf.assign((size_t)bufSize, 0.0f);
        baseGrainSize = (float)(sr * 0.042);     // ~42 ms base grains
        grainSize = baseGrainSize;
        smoothCoeff = (float)(1.0 / (sr * 0.04));  // 40ms time constant
        smoothLP.setCutoff(8000.0f, sr);
        smoothLP.reset();
        resetGrainPhases();
    }

    void setPitchRatio(float ratio) { targetRatio = juce::jlimit(0.5f, 4.0f, ratio); }

    void resetGrainPhases()
    {
        // Evenly-spaced grain phases for maximum overlap coverage
        for (int g = 0; g < NUM_GRAINS; ++g)
            delay_[g] = grainSize * ((float)g / (float)NUM_GRAINS);
    }

    void reset()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        writePos = 0;
        pitchRatio = targetRatio;
        updateGrainSize();
        resetGrainPhases();
        smoothLP.reset();
    }

    void updateGrainSize()
    {
        // Adapt grain size to pitch ratio — longer grains for subtle shifts,
        // shorter for extreme ratios to reduce latency and improve tracking
        float ratioFactor = (pitchRatio <= 2.0f)
            ? 1.0f
            : 1.0f / std::sqrt(pitchRatio * 0.5f);  // compress for high ratios
        grainSize = juce::jlimit(baseGrainSize * 0.5f, baseGrainSize * 1.5f,
            baseGrainSize * ratioFactor);
    }

    float process(float input) noexcept
    {
        // Smooth pitch ratio changes
        pitchRatio += smoothCoeff * (targetRatio - pitchRatio);

        buf[(size_t)writePos] = input;

        float output = 0.0f;
        const float advance = pitchRatio - 1.0f;
        const float invGrainSize = 1.0f / grainSize;

        for (int g = 0; g < NUM_GRAINS; ++g)
        {
            // Normalized position within grain [0, 1)
            float pos = 1.0f - delay_[g] * invGrainSize;
            // Hanning window — smooth overlap-add
            float window = 0.5f * (1.0f - std::cos(kTwoPi * pos));

            // Read from circular buffer with linear interpolation
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
            else if (delay_[g] >= grainSize)
                delay_[g] -= grainSize;
        }

        // Normalize by number of grains / 2 (Hanning overlap-add gain)
        output *= (2.0f / (float)NUM_GRAINS);

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
    false, true, 0.62f, 2.0f,       // shimmerMix bumped: 0.45 → 0.62 for clear pitch distinction
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
        springDripVoice.prepare(sr, 0.08f);
        plateSheenVoice.prepare(sr, 0.05f);
        hallBloomVoice.prepare(sr, 0.22f);
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
        duckAmountSmooth.reset(sr, 0.03);
        duckAmountSmooth.setCurrentAndTargetValue(0.0f);
        swellAmountSmooth.reset(sr, 0.04);
        swellAmountSmooth.setCurrentAndTargetValue(0.0f);
        gateAmountSmooth.reset(sr, 0.04);
        gateAmountSmooth.setCurrentAndTargetValue(0.0f);
        freezeSmooth.reset(sr, 0.02);
        freezeSmooth.setCurrentAndTargetValue(0.0f);

        duckAttackCoeff = (float)std::exp(-1.0 / (sr * 0.012));
        duckReleaseCoeff = (float)std::exp(-1.0 / (sr * 0.180));
        swellAttackCoeff = (float)std::exp(-1.0 / (sr * 0.085));
        swellReleaseCoeff = (float)std::exp(-1.0 / (sr * 0.110));
        gateAttackCoeff = (float)std::exp(-1.0 / (sr * 0.006));
        gateReleaseCoeff = (float)std::exp(-1.0 / (sr * 0.145));
        bloomLP.setCutoff(1800.0f, sr);

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
        springDripVoice.reset();
        plateSheenVoice.reset();
        hallBloomVoice.reset();
        outputToneL.reset();
        outputToneR.reset();
        bloomLP.reset();
        dcL.reset();  dcR.reset();
        modPhases.fill(0.0f);
        duckEnv = 0.0f;
        swellEnv = 0.0f;
        gateEnv = 0.0f;
    }

    // Call when any parameter changes (cheap — not per-sample)
    void configure(int mode, float decay01, float size01, float tone01,
                   float damping01, float bassCut01, float diffusion01,
                   float width01, float modAmount01, float predelayMs,
                   float duckAmount01, float swellAmount01, float gateAmount01, bool freezeEnabled)
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
        duckAmountSmooth.setTargetValue(duckAmount01);
        swellAmountSmooth.setTargetValue(swellAmount01);
        gateAmountSmooth.setTargetValue(gateAmount01);
        freezeSmooth.setTargetValue(freezeEnabled ? 1.0f : 0.0f);

        // ---- Pre-delay ----
        float pdSamples = juce::jlimit(0.0f, (float)(predelayMid.length - 4),
            predelayMs * (float)(sr * 0.001));
        predelaySmooth.setTargetValue(pdSamples);

        // ---- Shimmer ----
        useShimmer  = t.useShimmer;
        shimmerMix_ = t.shimmerMix;
        // NOTE: shimmer pitch ratio is set externally by the user parameter,
        // NOT by mode tuning — avoids fighting between configure() and the override.
        shimmer.updateGrainSize();
        // Higher LP cutoff lets the pitch-shifted harmonics through clearly
        shimmerLP.setCutoff(lerp(5000.0f, 12000.0f, tone01), sr);
        outToneHzSmooth.setTargetValue(lerp(2800.0f, t.dampLpMin * 1.15f, tone01));

        modeInputDrive = 1.0f;
        modeStereoExcite = 0.35f;
        modeSideScale = 1.0f;
        modeCrossfeed = 0.0f;
        modeBloomMix = 0.0f;
        modeAttackMix = 0.0f;
        springDripMix = 0.0f;
        plateSheenMix = 0.0f;
        hallBloomVoiceMix = 0.0f;
        float bloomCutoffHz = 1800.0f;

        switch (mode)
        {
            case 0:
                modeInputDrive = 1.12f;
                modeStereoExcite = 0.42f;
                modeSideScale = 0.88f;
                modeAttackMix = 0.08f;
                springDripMix = 0.24f + size01 * 0.10f;
                bloomCutoffHz = 2600.0f;
                springDripVoice.configure(sr, lerp(11.0f, 19.0f, size01),
                    lerp(1400.0f, 2400.0f, tone01),
                    lerp(3600.0f, 6200.0f, tone01),
                    0.38f + decay01 * 0.16f);
                break;

            case 1:
                modeInputDrive = 1.08f;
                modeStereoExcite = 0.28f;
                modeSideScale = 0.92f;
                modeCrossfeed = 0.05f;
                modeAttackMix = 0.15f;
                modeBloomMix = 0.05f;
                plateSheenMix = 0.16f + tone01 * 0.10f;
                bloomCutoffHz = 3400.0f;
                plateSheenVoice.configure(sr, lerp(13.0f, 23.0f, size01),
                    lerp(1900.0f, 3200.0f, tone01),
                    lerp(7000.0f, 12500.0f, tone01),
                    0.44f + decay01 * 0.18f);
                break;

            case 2:
                modeInputDrive = 0.98f;
                modeStereoExcite = 0.34f;
                modeSideScale = 1.02f;
                modeCrossfeed = 0.07f;
                modeBloomMix = 0.14f;
                hallBloomVoiceMix = 0.18f + size01 * 0.10f;
                bloomCutoffHz = 1600.0f;
                hallBloomVoice.configure(sr, lerp(62.0f, 108.0f, size01),
                    140.0f,
                    lerp(1200.0f, 2600.0f, tone01),
                    0.54f + decay01 * 0.18f);
                break;

            case 3:
                modeInputDrive = 1.04f;
                modeStereoExcite = 0.24f;
                modeSideScale = 0.78f;
                modeAttackMix = 0.06f;
                bloomCutoffHz = 3000.0f;
                break;

            case 4:
                modeInputDrive = 1.00f;
                modeStereoExcite = 0.36f;
                modeSideScale = 1.05f;
                modeCrossfeed = 0.08f;
                modeBloomMix = 0.18f;
                bloomCutoffHz = 2200.0f;
                break;

            case 5:
            default:
                modeInputDrive = 0.94f;
                modeStereoExcite = 0.46f;
                modeSideScale = 1.18f;
                modeCrossfeed = 0.12f;
                modeBloomMix = 0.28f;
                bloomCutoffHz = 1100.0f;
                break;
        }

        bloomLP.setCutoff(bloomCutoffHz, sr);
    }

    void processSample(float inL, float inR, float& outL, float& outR) noexcept
    {
        const float fb       = feedbackSmooth.getNextValue();
        const float modDepth = modDepthSmooth.getNextValue();
        const float erLevel  = erLevelSmooth.getNextValue();
        const float width    = widthSmooth.getNextValue();
        const float toneHz   = outToneHzSmooth.getNextValue();
        const float duckAmount = duckAmountSmooth.getNextValue();
        const float swellAmount = swellAmountSmooth.getNextValue();
        const float gateAmount = gateAmountSmooth.getNextValue();
        const float freeze = freezeSmooth.getNextValue();
        outputToneL.setCutoff(toneHz, sr);
        outputToneR.setCutoff(toneHz, sr);

        const float midIn  = (inL + inR) * 0.5f;
        const float sideIn = (inL - inR) * 0.5f;
        updatePerformanceEnvelopes(midIn, sideIn);
        const auto performance = computePerformanceGains(duckAmount, swellAmount, gateAmount, freeze);

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
        erL = (erL + delayedSide * 0.08f * width) * performance.swellGain;
        erR = (erR - delayedSide * 0.08f * width) * performance.swellGain;

        // ---- Input diffusion (6 cascaded all-pass stages) ----
        float diffused = delayedMid;
        for (auto& d : diffusers)
            diffused = d.process(diffused);

        // Spring dispersion (chirp)
        if (useDisp)
            for (auto& d : dispersion)
                diffused = d.process(diffused);

        const float inputInject = 1.0f - freeze * 0.985f;
        const float loopFeedback = lerp(fb, juce::jmax(fb, 0.9992f), freeze);
        const float gatedFeedback = loopFeedback * performance.gateFeedbackScale;
        const float attackExcite = std::tanh(diffused * modeInputDrive * 0.7f)
            * modeAttackMix * performance.swellGain;
        diffused *= modeInputDrive * inputInject * performance.swellGain;

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

        const auto rawLineOut = lineOut;

        // ---- Two-band damping + HP ----
        for (int i = 0; i < NUM_LINES; ++i)
        {
            lineOut[(size_t)i] = dampers[i].process(lineOut[(size_t)i]);
            lineOut[(size_t)i] = dampHP[i].process(lineOut[(size_t)i]);
            lineOut[(size_t)i] = dampLP[i].process(lineOut[(size_t)i]);
            if (freeze > 0.0f)
                lineOut[(size_t)i] = lerp(lineOut[(size_t)i], rawLineOut[(size_t)i], freeze * 0.94f);
        }

        // ---- Stereo output taps (before Hadamard — taps from damped lines) ----
        float lateMid = 0.0f, lateSide = 0.0f;
        for (int i = 0; i < NUM_LINES; ++i)
        {
            lateMid  += kTapL[(size_t)i] * lineOut[(size_t)i];
            lateSide += kTapR[(size_t)i] * lineOut[(size_t)i];
        }
        float lateL = (lateMid + lateSide * width * modeSideScale) * kInvSqrt8;
        float lateR = (lateMid - lateSide * width * modeSideScale) * kInvSqrt8;

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
            float fbSig = gatedFeedback * lineOut[(size_t)i];
            // Soft-limit feedback (transparent below ±2.5, compresses above)
            fbSig = std::tanh(fbSig * 0.4f) * 2.5f;
            const float stereoExcite = delayedSide * width * modeStereoExcite * kTapR[(size_t)i]
                * performance.swellGain;
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

        lateL += attackExcite + delayedSide * modeAttackMix * 0.08f;
        lateR += attackExcite - delayedSide * modeAttackMix * 0.08f;
        applyDedicatedModeCharacter(delayedMid, delayedSide, width, freeze, lateL, lateR);
        applyModeBloomAndCrossfeed(lateL, lateR);

        lateL = outputToneL.process(lateL);
        lateR = outputToneR.process(lateR);

        // ---- Blend ER + late reverb ----
        const float erMix = erLevel * (1.0f - freeze);
        const float wetGain = performance.duckGain * performance.gateGain;
        outL = (erL * erMix + lateL) * wetGain;
        outR = (erR * erMix + lateR) * wetGain;

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
            const float duckAmount = duckAmountSmooth.getNextValue();
            const float swellAmount = swellAmountSmooth.getNextValue();
            const float gateAmount = gateAmountSmooth.getNextValue();
            const float freeze = freezeSmooth.getNextValue();
            outputToneL.setCutoff(toneHz, sr);
            outputToneR.setCutoff(toneHz, sr);

            const float sL = inL[s];
            const float sR = inR[s];
            const float midIn  = (sL + sR) * 0.5f;
            const float sideIn = (sL - sR) * 0.5f;
            updatePerformanceEnvelopes(midIn, sideIn);
            const auto performance = computePerformanceGains(duckAmount, swellAmount, gateAmount, freeze);

            predelayMid.write(midIn);
            predelaySide.write(sideIn);
            float pdSamp = juce::jlimit(0.0f, (float)(predelayMid.length - 2),
                               predelaySmooth.getNextValue());
            const float delayedMid  = predelayMid.readLinear(juce::jmax(1.0f, pdSamp));
            const float delayedSide = predelaySide.readLinear(juce::jmax(1.0f, pdSamp));

            float erL = 0.0f, erR = 0.0f;
            er.process(delayedMid, erL, erR);
            erL = (erL + delayedSide * 0.08f * width) * performance.swellGain;
            erR = (erR - delayedSide * 0.08f * width) * performance.swellGain;

            float diffused = delayedMid;
            for (auto& d : diffusers)
                diffused = d.process(diffused);
            if (useDisp)
                for (auto& d : dispersion)
                    diffused = d.process(diffused);

            const float inputInject = 1.0f - freeze * 0.985f;
            const float loopFeedback = lerp(fb, juce::jmax(fb, 0.9992f), freeze);
            const float gatedFeedback = loopFeedback * performance.gateFeedbackScale;
            const float attackExcite = std::tanh(diffused * modeInputDrive * 0.7f)
                * modeAttackMix * performance.swellGain;
            diffused *= modeInputDrive * inputInject * performance.swellGain;

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

            const auto rawLineOut = lineOut;

            for (int i = 0; i < NUM_LINES; ++i)
            {
                lineOut[(size_t)i] = dampers[i].process(lineOut[(size_t)i]);
                lineOut[(size_t)i] = dampHP[i].process(lineOut[(size_t)i]);
                lineOut[(size_t)i] = dampLP[i].process(lineOut[(size_t)i]);
                if (freeze > 0.0f)
                    lineOut[(size_t)i] = lerp(lineOut[(size_t)i], rawLineOut[(size_t)i], freeze * 0.94f);
            }

            float lateMid = 0.0f, lateSide = 0.0f;
            for (int i = 0; i < NUM_LINES; ++i)
            {
                lateMid  += kTapL[(size_t)i] * lineOut[(size_t)i];
                lateSide += kTapR[(size_t)i] * lineOut[(size_t)i];
            }
            float lateL = (lateMid + lateSide * width * modeSideScale) * kInvSqrt8;
            float lateR = (lateMid - lateSide * width * modeSideScale) * kInvSqrt8;

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
                float fbSig = std::tanh(gatedFeedback * lineOut[(size_t)i] * 0.4f) * 2.5f;
                const float stereoExcite = delayedSide * width * modeStereoExcite * kTapR[(size_t)i]
                    * performance.swellGain;
                float inp = (diffused + stereoExcite) * kInvSqrt8 + fbSig;
                if (useShimmer) inp += shimSig * kInvSqrt8;
                lines[i].write(inp);
            }

            if (isSpring)
            {
                lateL = std::tanh(lateL * 1.15f) * 0.8695652f;  // 1/1.15
                lateR = std::tanh(lateR * 1.15f) * 0.8695652f;
            }

            lateL += attackExcite + delayedSide * modeAttackMix * 0.08f;
            lateR += attackExcite - delayedSide * modeAttackMix * 0.08f;
            applyDedicatedModeCharacter(delayedMid, delayedSide, width, freeze, lateL, lateR);
            applyModeBloomAndCrossfeed(lateL, lateR);

            lateL = outputToneL.process(lateL);
            lateR = outputToneR.process(lateR);

            const float erMix = erLevel * (1.0f - freeze);
            const float wetGain = performance.duckGain * performance.gateGain;
            outL[s] = dcL.process((erL * erMix + lateL) * wetGain);
            outR[s] = dcR.process((erR * erMix + lateR) * wetGain);
        }
    }

    int currentMode = 0;

private:
    void updatePerformanceEnvelopes(float midIn, float sideIn) noexcept
    {
        const float detector = std::abs(midIn) + 0.35f * std::abs(sideIn);
        const float duckCoeff = detector > duckEnv ? duckAttackCoeff : duckReleaseCoeff;
        duckEnv = detector + duckCoeff * (duckEnv - detector);

        const float swellCoeff = detector > swellEnv ? swellAttackCoeff : swellReleaseCoeff;
        swellEnv = detector + swellCoeff * (swellEnv - detector);

        const float gateCoeff = detector > gateEnv ? gateAttackCoeff : gateReleaseCoeff;
        gateEnv = detector + gateCoeff * (gateEnv - detector);
    }

    PerformanceGains computePerformanceGains(float duckAmount, float swellAmount,
        float gateAmount, float freeze) const noexcept
    {
        PerformanceGains gains;

        float duckControl = juce::jlimit(0.0f, 1.0f, (duckEnv - 0.015f) * 8.0f);
        gains.duckGain = 1.0f - duckAmount * duckControl * 0.82f;
        gains.duckGain = juce::jmax(0.18f, gains.duckGain * (0.85f + 0.15f * gains.duckGain));
        gains.duckGain = lerp(gains.duckGain, 1.0f, freeze);

        const float swellThreshold = 0.004f + swellAmount * 0.010f;
        float swellOpen = juce::jlimit(0.0f, 1.0f, (swellEnv - swellThreshold) / (0.11f + swellThreshold));
        swellOpen = swellOpen * swellOpen * (3.0f - 2.0f * swellOpen);
        gains.swellGain = lerp(1.0f, 0.18f + swellOpen * 0.82f, swellAmount);
        gains.swellGain = lerp(gains.swellGain, 1.0f, freeze);

        const float gateThreshold = 0.010f + gateAmount * 0.040f;
        float gateOpen = juce::jlimit(0.0f, 1.0f, (gateEnv - gateThreshold) / (0.11f + gateThreshold));
        gateOpen = gateOpen * gateOpen * (3.0f - 2.0f * gateOpen);
        const float gateFloor = juce::jmax(0.02f, 1.0f - gateAmount * 0.97f);
        gains.gateGain = lerp(1.0f, gateFloor + gateOpen * (1.0f - gateFloor), gateAmount);

        const float tightFeedback = juce::jmax(0.64f, 0.92f - gateAmount * 0.22f);
        gains.gateFeedbackScale = lerp(1.0f, tightFeedback + gateOpen * (1.0f - tightFeedback), gateAmount);
        gains.gateGain = lerp(gains.gateGain, 1.0f, freeze);
        gains.gateFeedbackScale = lerp(gains.gateFeedbackScale, 1.0f, freeze);
        return gains;
    }

    void applyDedicatedModeCharacter(float delayedMid, float delayedSide,
        float width, float freeze, float& lateL, float& lateR) noexcept
    {
        const float freezeInputScale = 1.0f - freeze * 0.92f;

        if (springDripMix > 0.0f)
        {
            const float drip = springDripVoice.process((delayedMid + delayedSide * 0.42f) * freezeInputScale);
            lateL += drip * springDripMix;
            lateR -= drip * springDripMix * 0.82f;
        }

        if (plateSheenMix > 0.0f)
        {
            const float plateMono = (lateL + lateR) * 0.5f;
            const float sheen = plateSheenVoice.process((plateMono + delayedMid * 0.45f) * freezeInputScale);
            lateL += sheen * plateSheenMix * (0.92f + width * 0.08f);
            lateR += sheen * plateSheenMix * (0.86f + width * 0.06f);
        }

        if (hallBloomVoiceMix > 0.0f)
        {
            const float hallSeed = ((lateL + lateR) * 0.5f + delayedMid * 0.22f) * freezeInputScale;
            const float bloom = hallBloomVoice.process(hallSeed);
            const float side = bloom * width * 0.28f;
            lateL += bloom * hallBloomVoiceMix + side;
            lateR += bloom * hallBloomVoiceMix - side;
        }
    }

    void applyModeBloomAndCrossfeed(float& lateL, float& lateR) noexcept
    {
        if (modeBloomMix > 0.0f)
        {
            const float bloom = bloomLP.process((lateL + lateR) * 0.5f) * modeBloomMix;
            lateL += bloom;
            lateR += bloom;
        }

        if (modeCrossfeed > 0.0f)
        {
            const float prevL = lateL;
            const float prevR = lateR;
            lateL = prevL + prevR * modeCrossfeed;
            lateR = prevR + prevL * modeCrossfeed;
        }
    }

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
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> duckAmountSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> swellAmountSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gateAmountSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> freezeSmooth;

    EarlyReflections   er;
    GrainPitchShifter  shimmer;
    OnePoleLP          shimmerLP;
    CharacterDelayVoice springDripVoice;
    CharacterDelayVoice plateSheenVoice;
    CharacterDelayVoice hallBloomVoice;
    OnePoleLP          bloomLP;
    OnePoleLP          outputToneL, outputToneR;
    bool  useShimmer   = false;
    float shimmerMix_  = 0.0f;
    float duckEnv = 0.0f;
    float duckAttackCoeff = 0.98f;
    float duckReleaseCoeff = 0.999f;
    float swellEnv = 0.0f;
    float swellAttackCoeff = 0.999f;
    float swellReleaseCoeff = 0.999f;
    float gateEnv = 0.0f;
    float gateAttackCoeff = 0.98f;
    float gateReleaseCoeff = 0.999f;
    float modeInputDrive = 1.0f;
    float modeStereoExcite = 0.35f;
    float modeSideScale = 1.0f;
    float modeCrossfeed = 0.0f;
    float modeBloomMix = 0.0f;
    float modeAttackMix = 0.0f;
    float springDripMix = 0.0f;
    float plateSheenMix = 0.0f;
    float hallBloomVoiceMix = 0.0f;

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
        addParameter(duckParam = new juce::AudioParameterFloat(
            "reverbDuck", "Duck", 0.0f, 1.0f, 0.0f));
        addParameter(swellParam = new juce::AudioParameterFloat(
            "reverbSwell", "Swell", 0.0f, 1.0f, 0.0f));
        addParameter(gateParam = new juce::AudioParameterFloat(
            "reverbGate", "Gate", 0.0f, 1.0f, 0.0f));
        addParameter(freezeParam = new juce::AudioParameterBool(
            "reverbFreeze", "Freeze", false));
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
        lastBass = lastDiff = lastWidth = lastMod = lastPD = lastDuck = lastSwell = lastGate = -1.0f;
        lastFreeze = false;
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
        xml.setAttribute("version", 5);
        xml.setAttribute("modeIndex", modeParam ? modeParam->getIndex() : 0);
        xml.setAttribute("shimmerPitchIndex", shimmerPitchParam ? shimmerPitchParam->getIndex() : 1);
        xml.setAttribute("reverbFreeze", freezeParam != nullptr && freezeParam->get());
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
        writeFloat(xml, "reverbDuck", duckParam);
        writeFloat(xml, "reverbSwell", swellParam);
        writeFloat(xml, "reverbGate", gateParam);
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
            restoreBool(*xml, "reverbFreeze", freezeParam);
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
            restoreFloat(*xml, "reverbDuck", duckParam);
            restoreFloat(*xml, "reverbSwell", swellParam);
            restoreFloat(*xml, "reverbGate", gateParam);
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

                if (id == "reverbFreeze")
                {
                    if (freezeParam != nullptr)
                        freezeParam->setValueNotifyingHost(value >= 0.5f ? 1.0f : 0.0f);
                    continue;
                }

                restoreLegacyFloat(id, value);
            }
        }

        lastMode = -1;
        lastDecay = lastTone = lastSize = lastDamp = -1.0f;
        lastBass = lastDiff = lastWidth = lastMod = lastPD = lastDuck = lastSwell = lastGate = -1.0f;
        lastFreeze = false;
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
        const float duck    = duckParam    ? duckParam->get()       : 0.0f;
        const float swell   = swellParam   ? swellParam->get()      : 0.0f;
        const float gate    = gateParam    ? gateParam->get()       : 0.0f;
        const bool  freeze  = freezeParam  ? freezeParam->get()     : false;

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
            || std::abs(pdMs  - lastPD)    > 0.1f
            || std::abs(duck  - lastDuck)  > 1e-4f
            || std::abs(swell - lastSwell) > 1e-4f
            || std::abs(gate  - lastGate)  > 1e-4f
            || freeze != lastFreeze)
        {
            engine.configure(mode, decay, size, tone, damp, bass, diff, width, mod, pdMs, duck, swell, gate, freeze);
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
            lastDuck  = duck;
            lastSwell = swell;
            lastGate  = gate;
            lastFreeze = freeze;
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
    juce::AudioParameterFloat*  duckParam     = nullptr;
    juce::AudioParameterFloat*  swellParam    = nullptr;
    juce::AudioParameterFloat*  gateParam     = nullptr;
    juce::AudioParameterBool*   freezeParam   = nullptr;
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

    static void restoreBool(const juce::XmlElement& xml, const char* name, juce::AudioParameterBool* param)
    {
        if (param != nullptr && xml.hasAttribute(name))
            param->setValueNotifyingHost(xml.getBoolAttribute(name) ? 1.0f : 0.0f);
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
        else if (id == "reverbDuck") apply(duckParam);
        else if (id == "reverbSwell") apply(swellParam);
        else if (id == "reverbGate") apply(gateParam);
    }

    Nova::Reverb::Engine engine;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    double currentSampleRate = 44100.0;
    int    lastMode  = -1;
    float  lastDecay = -1.0f, lastTone = -1.0f, lastSize = -1.0f, lastDamp = -1.0f;
    float  lastBass  = -1.0f, lastDiff = -1.0f, lastWidth = -1.0f, lastMod  = -1.0f, lastPD   = -1.0f, lastDuck = -1.0f;
    float  lastSwell = -1.0f, lastGate = -1.0f;
    bool   lastFreeze = false;
    bool   isPrepared = false;
};

#include "ReverbEditor.h"
