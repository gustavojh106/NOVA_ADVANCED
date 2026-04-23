#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <array>

// ============================================================================
//  Synthetic IR Generator
//
//  Generates unique impulse responses for each cabinet type by filtering a
//  unit impulse through a speaker/cabinet model. The result is a short IR
//  (~23 ms at the current sample rate) that captures the frequency response and phase
//  characteristics of the speaker+cabinet combination.
//
//  Approach:
//    1. Start with a unit impulse (delta)
//    2. Apply cascaded biquad filters modeling the speaker frequency response
//    3. Add short comb-filter reflections for cabinet box resonance
//    4. Apply exponential decay envelope
//    5. Normalize to peak
// ============================================================================

namespace Nova::CabinetIR
{

static constexpr double kIRDurationSeconds = 0.023;

// -------------------------------------------------------------------
//  Single biquad filter section (direct form II transposed)
// -------------------------------------------------------------------
struct BiquadSection
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    // Design functions
    static BiquadSection makeLowPass(double sr, float freq, float Q)
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double cosw = std::cos(w0);
        const double a0 = 1.0 + alpha;
        BiquadSection s;
        s.b0 = (float)((1.0 - cosw) / 2.0 / a0);
        s.b1 = (float)((1.0 - cosw) / a0);
        s.b2 = s.b0;
        s.a1 = (float)(-2.0 * cosw / a0);
        s.a2 = (float)((1.0 - alpha) / a0);
        return s;
    }

    static BiquadSection makeHighPass(double sr, float freq, float Q)
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double cosw = std::cos(w0);
        const double a0 = 1.0 + alpha;
        BiquadSection s;
        s.b0 = (float)((1.0 + cosw) / 2.0 / a0);
        s.b1 = (float)(-(1.0 + cosw) / a0);
        s.b2 = s.b0;
        s.a1 = (float)(-2.0 * cosw / a0);
        s.a2 = (float)((1.0 - alpha) / a0);
        return s;
    }

    static BiquadSection makePeak(double sr, float freq, float Q, float gainDb)
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double A = std::pow(10.0, gainDb / 40.0);
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double cosw = std::cos(w0);
        const double a0 = 1.0 + alpha / A;
        BiquadSection s;
        s.b0 = (float)((1.0 + alpha * A) / a0);
        s.b1 = (float)(-2.0 * cosw / a0);
        s.b2 = (float)((1.0 - alpha * A) / a0);
        s.a1 = (float)(-2.0 * cosw / a0);
        s.a2 = (float)((1.0 - alpha / A) / a0);
        return s;
    }

    static BiquadSection makeLowShelf(double sr, float freq, float Q, float gainDb)
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double A = std::pow(10.0, gainDb / 40.0);
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double cosw = std::cos(w0);
        const double ap1 = A + 1.0, am1 = A - 1.0;
        const double sqrtA2alpha = 2.0 * std::sqrt(A) * alpha;
        const double a0 = ap1 + am1 * cosw + sqrtA2alpha;
        BiquadSection s;
        s.b0 = (float)(A * (ap1 - am1 * cosw + sqrtA2alpha) / a0);
        s.b1 = (float)(2.0 * A * (am1 - ap1 * cosw) / a0);
        s.b2 = (float)(A * (ap1 - am1 * cosw - sqrtA2alpha) / a0);
        s.a1 = (float)(-2.0 * (am1 + ap1 * cosw) / a0);
        s.a2 = (float)((ap1 + am1 * cosw - sqrtA2alpha) / a0);
        return s;
    }

    static BiquadSection makeHighShelf(double sr, float freq, float Q, float gainDb)
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double A = std::pow(10.0, gainDb / 40.0);
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double cosw = std::cos(w0);
        const double ap1 = A + 1.0, am1 = A - 1.0;
        const double sqrtA2alpha = 2.0 * std::sqrt(A) * alpha;
        const double a0 = ap1 - am1 * cosw + sqrtA2alpha;
        BiquadSection s;
        s.b0 = (float)(A * (ap1 + am1 * cosw + sqrtA2alpha) / a0);
        s.b1 = (float)(-2.0 * A * (am1 + ap1 * cosw) / a0);
        s.b2 = (float)(A * (ap1 + am1 * cosw - sqrtA2alpha) / a0);
        s.a1 = (float)(2.0 * (am1 - ap1 * cosw) / a0);
        s.a2 = (float)((ap1 - am1 * cosw - sqrtA2alpha) / a0);
        return s;
    }
};

// -------------------------------------------------------------------
//  Generate IR by filtering a delta impulse through a filter chain,
//  then adding cabinet box reflections and decay envelope
// -------------------------------------------------------------------
inline juce::AudioBuffer<float> generateIR(
    double sampleRate,
    std::vector<BiquadSection>& filters,
    float decayRate,        // exponential decay per sample (e.g., 0.994)
    float boxReflectDelay,  // samples for first box reflection
    float boxReflectGain,   // amplitude of cabinet box reflection
    float secondReflectDelay = 0.0f,
    float secondReflectGain  = 0.0f)
{
    const int irLength = juce::jmax(256, juce::roundToInt(sampleRate * kIRDurationSeconds));
    juce::AudioBuffer<float> ir(1, irLength);
    ir.clear();

    auto* data = ir.getWritePointer(0);

    // Unit impulse
    data[0] = 1.0f;

    // Process through filter chain
    for (auto& f : filters)
    {
        f.reset();
        for (int i = 0; i < irLength; ++i)
            data[i] = f.process(data[i]);
    }

    // Add cabinet box reflections (comb filter effect)
    // This creates the subtle resonance and "boxiness" of a real cabinet
    if (boxReflectDelay > 0.0f && std::abs(boxReflectGain) > 0.001f)
    {
        const int delay1 = juce::jlimit(1, irLength - 1, (int)std::round(boxReflectDelay));
        for (int i = irLength - 1; i >= delay1; --i)
            data[i] += data[i - delay1] * boxReflectGain;
    }

    if (secondReflectDelay > 0.0f && std::abs(secondReflectGain) > 0.001f)
    {
        const int delay2 = juce::jlimit(1, irLength - 1, (int)std::round(secondReflectDelay));
        for (int i = irLength - 1; i >= delay2; --i)
            data[i] += data[i - delay2] * secondReflectGain;
    }

    // Exponential decay envelope
    float env = 1.0f;
    for (int i = 0; i < irLength; ++i)
    {
        data[i] *= env;
        env *= decayRate;
    }

    // Normalize to peak
    const float peak = ir.getMagnitude(0, irLength);
    if (peak > 0.0001f)
        ir.applyGain(0.85f / peak);

    return ir;
}

// ===================================================================
//  ATLAS 4x12 — Celestion G12T-75 in closed-back 4x12
//
//  Character: Full low end (closed back), broad presence peak 2-4kHz,
//  steep high rolloff above 5kHz, slight mid-scoop around 400Hz.
//  The "standard" rock/metal cabinet sound.
// ===================================================================
inline juce::AudioBuffer<float> generateAtlas4x12(double sampleRate)
{
    std::vector<BiquadSection> filters;

    // Speaker cone resonance: 75 Hz low-end bump
    filters.push_back(BiquadSection::makePeak(sampleRate, 75.0f, 1.8f, 5.0f));

    // Closed-back bass reinforcement
    filters.push_back(BiquadSection::makeLowShelf(sampleRate, 120.0f, 0.7f, 3.0f));

    // Slight mid-scoop (G12T-75 character)
    filters.push_back(BiquadSection::makePeak(sampleRate, 420.0f, 1.0f, -2.5f));

    // Presence peak — the "bite" of the G12T-75
    filters.push_back(BiquadSection::makePeak(sampleRate, 2800.0f, 0.8f, 4.0f));

    // Secondary brightness peak
    filters.push_back(BiquadSection::makePeak(sampleRate, 4200.0f, 1.5f, 2.0f));

    // Speaker cone breakup rolloff — steep LP above 5kHz
    filters.push_back(BiquadSection::makeLowPass(sampleRate, 5200.0f, 0.65f));
    filters.push_back(BiquadSection::makeLowPass(sampleRate, 6800.0f, 0.72f));

    // Subsonic rolloff
    filters.push_back(BiquadSection::makeHighPass(sampleRate, 60.0f, 0.7f));

    // Box reflections: closed 4x12 has tight, short reflections
    const float boxDelay1 = (float)(sampleRate * 0.00035);  // ~0.35ms
    const float boxDelay2 = (float)(sampleRate * 0.00072);  // ~0.72ms

    return generateIR(sampleRate, filters, 0.993f, boxDelay1, -0.18f, boxDelay2, 0.10f);
}

// ===================================================================
//  VINTAGE 2x12 — Celestion Blue / Alnico in open-back 2x12
//
//  Character: Less bass (open back cancellation), warm mid-forward
//  tone, earlier high-end rolloff, "vocal" mid-range, natural room
//  ambience from open back. Think Fender Twin, Vox AC30.
// ===================================================================
inline juce::AudioBuffer<float> generateVintage2x12(double sampleRate)
{
    std::vector<BiquadSection> filters;

    // Speaker resonance: higher than closed-back (~100 Hz)
    filters.push_back(BiquadSection::makePeak(sampleRate, 100.0f, 1.2f, 3.5f));

    // Open-back bass cancellation: significant LF loss below 120 Hz
    filters.push_back(BiquadSection::makeHighPass(sampleRate, 95.0f, 0.55f));
    filters.push_back(BiquadSection::makeLowShelf(sampleRate, 150.0f, 0.7f, -2.0f));

    // Warm "boxy" lower-mid resonance (open-back cabinet body)
    filters.push_back(BiquadSection::makePeak(sampleRate, 340.0f, 1.3f, 2.5f));

    // Vocal mid presence — the Alnico "chime"
    filters.push_back(BiquadSection::makePeak(sampleRate, 1200.0f, 0.9f, 3.0f));

    // Gentle upper presence
    filters.push_back(BiquadSection::makePeak(sampleRate, 2400.0f, 1.2f, 1.5f));

    // Earlier high-end rolloff — Alnico speakers are warmer
    filters.push_back(BiquadSection::makeLowPass(sampleRate, 4400.0f, 0.58f));
    filters.push_back(BiquadSection::makeLowPass(sampleRate, 5800.0f, 0.65f));

    // Subsonic
    filters.push_back(BiquadSection::makeHighPass(sampleRate, 75.0f, 0.65f));

    // Open-back reflections: longer, more diffuse (sound bounces off wall behind)
    const float boxDelay1 = (float)(sampleRate * 0.00065);  // ~0.65ms
    const float boxDelay2 = (float)(sampleRate * 0.0015);   // ~1.5ms (wall reflection)

    return generateIR(sampleRate, filters, 0.991f, boxDelay1, -0.14f, boxDelay2, 0.08f);
}

// ===================================================================
//  MODERN 4x12 — Celestion V30 in closed-back 4x12
//
//  Character: Tight controlled bass, aggressive upper-mid "bark" at
//  2kHz (the V30 signature), mid-scoop around 500-700Hz for chunk,
//  extended but controlled high end, aggressive overall.
// ===================================================================
inline juce::AudioBuffer<float> generateModern4x12(double sampleRate)
{
    std::vector<BiquadSection> filters;

    // Speaker resonance: tight 75 Hz
    filters.push_back(BiquadSection::makePeak(sampleRate, 75.0f, 2.2f, 4.0f));

    // Tight closed-back bass: controlled, not boomy
    filters.push_back(BiquadSection::makeLowShelf(sampleRate, 110.0f, 0.75f, 1.5f));

    // V30 mid-scoop — the "chunk" frequency
    filters.push_back(BiquadSection::makePeak(sampleRate, 600.0f, 1.0f, -4.0f));

    // V30 bark — the aggressive upper-mid presence
    filters.push_back(BiquadSection::makePeak(sampleRate, 2000.0f, 1.4f, 5.5f));

    // Secondary presence peak
    filters.push_back(BiquadSection::makePeak(sampleRate, 3400.0f, 1.0f, 2.5f));

    // Extended high end (V30 is brighter than G12T-75)
    filters.push_back(BiquadSection::makeLowPass(sampleRate, 6200.0f, 0.60f));
    filters.push_back(BiquadSection::makeLowPass(sampleRate, 8000.0f, 0.70f));

    // Tight subsonic
    filters.push_back(BiquadSection::makeHighPass(sampleRate, 65.0f, 0.8f));

    // Closed-back tight reflections
    const float boxDelay1 = (float)(sampleRate * 0.00030);  // ~0.30ms (tight)
    const float boxDelay2 = (float)(sampleRate * 0.00058);  // ~0.58ms

    return generateIR(sampleRate, filters, 0.994f, boxDelay1, -0.20f, boxDelay2, 0.12f);
}

} // namespace Nova::CabinetIR
