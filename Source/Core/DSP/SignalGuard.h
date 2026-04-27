#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "../Constants.h"

namespace Nova
{
namespace DSP
{

struct SignalGuardStats
{
    int invalidSamples = 0;     // NaN / Inf replaced with silence
    int clippedSamples = 0;     // |x| > absLimit shaped back below ceiling
    int denormalSamples = 0;    // sub-1e-30 values flushed to zero

    bool hadCorruption() const noexcept
    {
        return invalidSamples > 0 || clippedSamples > 0;
    }

    void merge(const SignalGuardStats& other) noexcept
    {
        invalidSamples += other.invalidSamples;
        clippedSamples += other.clippedSamples;
        denormalSamples += other.denormalSamples;
    }
};

// Per-stage signal scrubber. RT-safe: no allocations, no locks, no atomics.
//   - NaN / Inf samples are replaced with 0
//   - |x| above absLimit is shaped via tanh so chains downstream see a sane peak
//     instead of a runaway value that would otherwise poison delay lines / IIRs
//   - Denormal magnitudes are flushed (defensive — FTZ should already cover this,
//     but cheap to enforce and protects long feedback paths on hosts that
//     re-enter the audio thread with denormal-bearing FP state).
//
// The shape is continuous: above absLimit we taper toward 1.5 * absLimit so the
// downstream limiter still has authority over the final peak.
inline SignalGuardStats scrub(juce::AudioBuffer<float>& buffer,
    float absLimit = Nova::Config::HARD_ABS_LIMIT_LINEAR) noexcept
{
    SignalGuardStats stats;

    constexpr float denormalFloor = 1.0e-30f;
    const float ceiling = juce::jmax(0.5f, absLimit);
    const float headroom = ceiling * 0.5f; // tanh tapers within this range

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels <= 0 || numSamples <= 0)
        return stats;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = buffer.getWritePointer(ch);

        for (int i = 0; i < numSamples; ++i)
        {
            float v = data[i];

            if (! std::isfinite(v))
            {
                data[i] = 0.0f;
                ++stats.invalidSamples;
                continue;
            }

            const float absV = std::abs(v);

            if (absV < denormalFloor)
            {
                data[i] = 0.0f;
                ++stats.denormalSamples;
                continue;
            }

            if (absV > ceiling)
            {
                const float sign = v < 0.0f ? -1.0f : 1.0f;
                const float over = (absV - ceiling) / headroom;
                const float shaped = ceiling + headroom * std::tanh(over);
                data[i] = sign * shaped;
                ++stats.clippedSamples;
            }
        }
    }

    return stats;
}

} // namespace DSP
} // namespace Nova
