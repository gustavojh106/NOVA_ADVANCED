#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

#include "../../../Core/Constants.h"

struct TempoSyncable
{
    virtual ~TempoSyncable() = default;
    virtual void setTempoSyncContext(float bpm, bool tempoValid, bool transportPlaying) = 0;
};

class ProcessorBase : public juce::AudioProcessor
{
public:
    ProcessorBase()
        : juce::AudioProcessor(juce::AudioProcessor::BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo())
            .withOutput("Output", juce::AudioChannelSet::stereo()))
    {
    }

    // -----------------------------------------------------------------------------
    // Bypass
    // -----------------------------------------------------------------------------
    void setBypassed(bool shouldBypass) noexcept
    {
        const bool previous = isBypassed.exchange(shouldBypass, std::memory_order_acq_rel);
        if (previous == shouldBypass)
            return;

        bypassChanged.store(true, std::memory_order_release);
    }

    bool getBypassed() const noexcept
    {
        return isBypassed.load(std::memory_order_acquire);
    }

    // Call this instead of setLatencySamples() from derived processors.
    // Important: latency remains stable even when bypassed.
    // The bypass path is delayed internally so the graph does not need to rebuild
    // or change latency when bypass is toggled.
    void setProcessingLatency(int newLatencySamples)
    {
        const int safeLatency = juce::jmax(0, newLatencySamples);
        activeLatency.store(safeLatency, std::memory_order_release);
        setLatencySamples(safeLatency);

        // This should normally be called during prepareToPlay(), not from processBlock().
        if (bypassPrepared)
            prepareBypassLatencyLines(safeLatency);
    }

    int getProcessingLatency() const noexcept
    {
        return activeLatency.load(std::memory_order_acquire);
    }

    bool hadBypassScratchOverflow() const noexcept
    {
        return bypassScratchOverflow.load(std::memory_order_relaxed);
    }

    void clearBypassScratchOverflowFlag() noexcept
    {
        bypassScratchOverflow.store(false, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------------
    // Layout support
    // -----------------------------------------------------------------------------
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
            return false;

        const auto inLayout = layouts.getMainInputChannelSet();
        return inLayout == juce::AudioChannelSet::mono()
            || inLayout == juce::AudioChannelSet::stereo();
    }

    // -----------------------------------------------------------------------------
    // Generic state recall (AudioProcessorParameterWithID)
    // Stores normalized values [0..1] for all registered parameters.
    // Not used on the realtime audio path.
    // -----------------------------------------------------------------------------
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        juce::XmlElement xml("PLUGIN_STATE");
        xml.setAttribute("version", 1);

        for (auto* param : getParameters())
        {
            if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
            {
                auto* e = xml.createNewChildElement("PARAM");
                e->setAttribute("id", p->paramID);
                e->setAttribute("value", juce::jlimit(0.0f, 1.0f, p->getValue()));
            }
        }

        copyXmlToBinary(xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        auto xmlState = std::unique_ptr<juce::XmlElement>(getXmlFromBinary(data, sizeInBytes));
        if (xmlState == nullptr || !xmlState->hasTagName("PLUGIN_STATE"))
            return;

        for (auto* child : xmlState->getChildIterator())
        {
            if (!child->hasTagName("PARAM"))
                continue;

            const juce::String paramID = child->getStringAttribute("id");
            const float value = juce::jlimit(0.0f, 1.0f,
                static_cast<float>(child->getDoubleAttribute("value", 0.0)));

            for (auto* param : getParameters())
            {
                if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
                {
                    if (p->paramID == paramID)
                    {
                        p->setValueNotifyingHost(value);
                        break;
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------------
    // Boilerplate
    // -----------------------------------------------------------------------------
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "Base"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

protected:
    // -----------------------------------------------------------------------------
    // Bypass lifecycle
    // -----------------------------------------------------------------------------
    // Call from prepareToPlay() of every derived processor.
    void prepareBypassSmoother(double sampleRate, int maxBlockSize)
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        const int maxSamplesPerBlock = juce::jmax(1, maxBlockSize);
        const int channels = juce::jlimit(1, kMaxBypassChannels, getMainBusNumOutputChannels());

        preparedSampleRate = sr;
        preparedMaxBlockSize = maxSamplesPerBlock;
        preparedNumChannels = channels;

        wetMix.reset(sr, Nova::Config::SMOOTH_BYPASS_SECONDS);
        wetMix.setCurrentAndTargetValue(isBypassed.load(std::memory_order_acquire) ? 0.0f : 1.0f);

        dryBuffer.setSize(channels,
            maxSamplesPerBlock,
            false,
            false,
            true);
        dryBuffer.clear();

        transitionActive = false;
        dryBufferValidForCurrentBlock = false;
        bypassScratchOverflow.store(false, std::memory_order_relaxed);

        prepareBypassLatencyLines(activeLatency.load(std::memory_order_acquire));
        bypassPrepared = true;
    }

    void resetBypassState()
    {
        wetMix.setCurrentAndTargetValue(isBypassed.load(std::memory_order_acquire) ? 0.0f : 1.0f);
        transitionActive = false;
        dryBufferValidForCurrentBlock = false;
        resetBypassLatencyLines();
    }

    // Call at the beginning of processBlock() in derived processors.
    // Returns false when the processor can skip its DSP because it is fully bypassed.
    // In that case the input buffer has already been left as the correct bypass output.
    bool beginBypassProcess(juce::AudioBuffer<float>& buffer)
    {
        // This object only protects this helper section. Derived processors should still
        // create their own ScopedNoDenormals at the top of processBlock() if they do DSP.
        juce::ScopedNoDenormals noDenormals;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxBypassChannels);

        if (!bypassPrepared)
        {
            // Do not allocate from the audio thread. Derived classes must call
            // prepareBypassSmoother() from prepareToPlay().
            jassertfalse;
            return !isBypassed.load(std::memory_order_acquire);
        }

        if (numSamples <= 0 || numChannels <= 0)
            return false;

        if (numSamples > preparedMaxBlockSize || numChannels > preparedNumChannels)
        {
            // Never resize here. This is the realtime path.
            // Fallback: process wet when enabled, or hard bypass when bypassed.
            bypassScratchOverflow.store(true, std::memory_order_relaxed);
            dryBufferValidForCurrentBlock = false;
            return !isBypassed.load(std::memory_order_acquire);
        }

        const bool changed = bypassChanged.exchange(false, std::memory_order_acq_rel);
        if (changed)
        {
            const bool shouldBypass = isBypassed.load(std::memory_order_acquire);

            // When coming back from bypass, reset the wet DSP state so tails/stale state
            // do not jump back in. Derived reset() must remain realtime-safe.
            if (!shouldBypass)
                reset();

            wetMix.setTargetValue(shouldBypass ? 0.0f : 1.0f);
            transitionActive = true;
        }

        const bool shouldBypass = isBypassed.load(std::memory_order_acquire);
        const bool smoothing = wetMix.isSmoothing();
        const bool latencyStableBypassNeeded = getProcessingLatency() > 0;

        const bool needDryCopy = transitionActive || smoothing || shouldBypass || latencyStableBypassNeeded;
        dryBufferValidForCurrentBlock = false;

        if (needDryCopy)
        {
            copyInputToDryBuffer(buffer, numChannels, numSamples);

            if (latencyStableBypassNeeded)
                applyBypassLatencyToDryBuffer(numChannels, numSamples);

            dryBufferValidForCurrentBlock = true;
        }
        else if (latencyStableBypassNeeded)
        {
            // Defensive only. The branch above should already catch this.
            feedBypassLatencyLines(buffer, numChannels, numSamples);
        }

        const bool fullyBypassed = shouldBypass
            && !wetMix.isSmoothing()
            && wetMix.getCurrentValue() <= 0.0001f;

        if (fullyBypassed)
        {
            transitionActive = false;

            if (dryBufferValidForCurrentBlock)
                copyDryBufferToOutput(buffer, numChannels, numSamples);

            return false;
        }

        return true;
    }

    // Call at the end of processBlock() in derived processors.
    void endBypassProcess(juce::AudioBuffer<float>& buffer)
    {
        juce::ScopedNoDenormals noDenormals;

        if (!transitionActive && !wetMix.isSmoothing())
            return;

        if (!dryBufferValidForCurrentBlock)
        {
            // No allocation/fallback dry buffer available. Avoid unsafe work.
            if (!wetMix.isSmoothing())
                transitionActive = false;
            return;
        }

        const int numSamples = buffer.getNumSamples();
        const int numChannels = juce::jmin(buffer.getNumChannels(), preparedNumChannels);

        if (numSamples <= 0 || numChannels <= 0)
            return;

        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        std::array<float*, kMaxBypassChannels> out{};
        std::array<const float*, kMaxBypassChannels> dry{};

        for (int ch = 0; ch < numChannels; ++ch)
        {
            out[(size_t)ch] = buffer.getWritePointer(ch);
            dry[(size_t)ch] = dryBuffer.getReadPointer(ch);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float wet = juce::jlimit(0.0f, 1.0f, wetMix.getNextValue());
            const float phase = wet * halfPi;
            const float dryGain = wet <= 1.0e-5f ? 1.0f : std::cos(phase);
            const float wetGain = wet >= 0.99999f ? 1.0f : std::sin(phase);

            for (int ch = 0; ch < numChannels; ++ch)
                out[(size_t)ch][i] = (out[(size_t)ch][i] * wetGain) + (dry[(size_t)ch][i] * dryGain);
        }

        if (!wetMix.isSmoothing())
            transitionActive = false;
    }

    // Backward-compatible helper for older pedals.
    bool shouldProcess(juce::AudioBuffer<float>& buffer)
    {
        return beginBypassProcess(buffer);
    }

    // Optional helper for derived processors.
    static float sanitizeParameter(float value, float fallback = 0.0f) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    }

private:
    static constexpr int kMaxBypassChannels = 2;

    void prepareBypassLatencyLines(int latencySamples)
    {
        const int safeLatency = juce::jmax(0, latencySamples);
        const int maxDelay = juce::jmax(1, safeLatency + juce::jmax(1, preparedMaxBlockSize) + 1);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = preparedSampleRate > 0.0 ? preparedSampleRate : 44100.0;
        spec.maximumBlockSize = static_cast<juce::uint32>(juce::jmax(1, preparedMaxBlockSize));
        spec.numChannels = 1;

        for (auto& line : bypassLatencyLines)
        {
            line.setMaximumDelayInSamples(maxDelay);
            line.prepare(spec);
            line.setDelay(static_cast<float>(safeLatency));
            line.reset();
        }
    }

    void resetBypassLatencyLines()
    {
        for (auto& line : bypassLatencyLines)
            line.reset();
    }

    void copyInputToDryBuffer(const juce::AudioBuffer<float>& buffer, int numChannels, int numSamples)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::FloatVectorOperations::copy(dryBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch),
                numSamples);
        }
    }

    void copyDryBufferToOutput(juce::AudioBuffer<float>& buffer, int numChannels, int numSamples)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::FloatVectorOperations::copy(buffer.getWritePointer(ch),
                dryBuffer.getReadPointer(ch),
                numSamples);
        }
    }

    void applyBypassLatencyToDryBuffer(int numChannels, int numSamples)
    {
        const int latency = getProcessingLatency();
        if (latency <= 0)
            return;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = dryBuffer.getWritePointer(ch);
            auto& delay = bypassLatencyLines[(size_t)ch];

            for (int i = 0; i < numSamples; ++i)
            {
                const float delayed = delay.popSample(0);
                delay.pushSample(0, data[i]);
                data[i] = delayed;
            }
        }
    }

    void feedBypassLatencyLines(const juce::AudioBuffer<float>& buffer, int numChannels, int numSamples)
    {
        const int latency = getProcessingLatency();
        if (latency <= 0)
            return;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* data = buffer.getReadPointer(ch);
            auto& delay = bypassLatencyLines[(size_t)ch];

            for (int i = 0; i < numSamples; ++i)
            {
                (void)delay.popSample(0);
                delay.pushSample(0, data[i]);
            }
        }
    }

private:
    std::atomic<bool> isBypassed{ false };
    std::atomic<bool> bypassChanged{ false };
    std::atomic<int> activeLatency{ 0 };
    std::atomic<bool> bypassScratchOverflow{ false };

    bool bypassPrepared = false;
    bool transitionActive = false;
    bool dryBufferValidForCurrentBlock = false;

    double preparedSampleRate = 44100.0;
    int preparedMaxBlockSize = 0;
    int preparedNumChannels = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMix;
    juce::AudioBuffer<float> dryBuffer;

    std::array<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>, kMaxBypassChannels> bypassLatencyLines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProcessorBase)
};
