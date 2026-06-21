#pragma once

#include "ProcessorBase.h"
#include "../../../Core/DSP/SignalGuard.h"

#include <atomic>
#include <cmath>

// Opt-in realtime-safety contract for pedal processors.
//
// Derived pedals keep ownership of their DSP and preparation. They must call
// preparePedalProcessing() from prepareToPlay(), call releasePedalProcessing()
// from releaseResources(), and implement processPedalBlock(). This wrapper owns
// the shared audio-thread policy: bypass integration, denormal suppression,
// input scrubbing, and a no-allocation dry fallback for unsupported blocks.
//
// Shared pedal contract guarantees:
//   * processBlock() scrubs the input (NaN/Inf -> 0, denormals flushed, hot
//     values bounded) BEFORE ProcessorBase captures its bypass dry scratch and
//     BEFORE any derived feedback/detector/dry-capture path runs. Corrupt input
//     can therefore never poison a feedback tail, dry mix, or bypass crossfade.
//   * Oversized or over-wide blocks take a no-allocation dry fallback that
//     increments the realtime fallback diagnostic and invokes
//     handleRealtimeFallback() instead of touching derived DSP.
//
// reset() realtime-safety rule:
//   Derived reset() MUST be allocation-free and realtime-safe. ProcessorBase
//   calls reset() from the audio thread on every bypass -> un-bypass transition
//   (see beginBypassProcess) so stale tails do not jump back in. reset() must not
//   resize buffers, allocate, take locks, log, or perform I/O; it may only zero
//   state, reseat smoother targets, and recompute cached coefficients. All
//   buffer/voice allocation must happen in prepareToPlay().
class NovaPedalProcessor : public ProcessorBase
{
public:
    NovaPedalProcessor() = default;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) final
    {
        juce::ScopedNoDenormals noDenormals;

        if (!pedalProcessingPrepared)
            return;

        // Scrub before ProcessorBase captures its bypass dry scratch. Otherwise a
        // bypass transition could mix corrupt input back into an otherwise-safe wet
        // render. Finite nominal samples are unchanged by the shared signal guard.
        Nova::DSP::scrub(buffer);

        if (!beginBypassProcess(buffer))
            return;

        if (!canProcessPedalBlock(buffer))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            handleRealtimeFallback();
            endBypassProcess(buffer);
            return;
        }

        processPedalBlock(buffer, midi);
        endBypassProcess(buffer);
    }

    int getRealtimeFallbackCount() const noexcept
    {
        return fallbackBlockCount.load(std::memory_order_relaxed);
    }

    void clearRealtimeFallbackCount() noexcept
    {
        fallbackBlockCount.store(0, std::memory_order_relaxed);
    }

protected:
    void preparePedalProcessing(double sampleRate,
        int maximumBlockSize,
        int maximumProcessingChannels = 2)
    {
        preparedBlockSize = juce::jmax(1, maximumBlockSize);
        preparedChannels = juce::jlimit(1,
            juce::jmax(1, maximumProcessingChannels),
            juce::jmax(1, getTotalNumOutputChannels()));

        prepareBypassSmoother(sampleRate, preparedBlockSize);
        pedalProcessingPrepared = sampleRate > 0.0;
    }

    void releasePedalProcessing() noexcept
    {
        pedalProcessingPrepared = false;
    }

    bool isPedalPrepared() const noexcept
    {
        return pedalProcessingPrepared;
    }

    // For derived pedals that detect an unsupported realtime condition partway
    // through processPedalBlock() (e.g. an internal helper that was not prepared
    // for the published channel count). Increments the shared fallback counter and
    // runs the fallback hook. The derived pedal is responsible for leaving a finite
    // dry/safe buffer before returning. Allocation-free and realtime-safe.
    void noteRealtimeFallback() noexcept
    {
        fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
        handleRealtimeFallback();
    }

    static float readParameter(const juce::AudioParameterFloat* parameter,
        float fallback,
        float minimum,
        float maximum) noexcept
    {
        const float safeFallback = std::isfinite(fallback)
            ? juce::jlimit(minimum, maximum, fallback)
            : juce::jlimit(minimum, maximum, 0.0f);
        const float value = parameter != nullptr ? parameter->get() : safeFallback;
        return juce::jlimit(minimum, maximum,
            std::isfinite(value) ? value : safeFallback);
    }

    static float mixDryWetEqualPower(float dry, float wet, float mix) noexcept
    {
        const float safeDry = std::isfinite(dry) ? dry : 0.0f;
        const float safeWet = std::isfinite(wet) ? wet : 0.0f;
        const float safeMix = juce::jlimit(0.0f, 1.0f,
            std::isfinite(mix) ? mix : 0.0f);
        const float phase = safeMix * juce::MathConstants<float>::halfPi;
        return (safeDry * std::cos(phase)) + (safeWet * std::sin(phase));
    }

    virtual void processPedalBlock(juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midi) = 0;

    virtual void handleRealtimeFallback() noexcept
    {
    }

private:
    bool canProcessPedalBlock(const juce::AudioBuffer<float>& buffer) const noexcept
    {
        return buffer.getNumSamples() <= preparedBlockSize
            && buffer.getNumChannels() <= preparedChannels;
    }

    std::atomic<int> fallbackBlockCount{ 0 };
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    bool pedalProcessingPrepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NovaPedalProcessor)
};
