#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "../../../Core/Constants.h"

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
    void setBypassed(bool shouldBypass)
    {
        const bool changed = (isBypassed.load() != shouldBypass);
        if (!changed)
            return;

        isBypassed = shouldBypass;
        bypassChanged = true;
        // Report 0 latency when bypassed so the graph's dry-path compensation
        // stays accurate. The active latency is restored when bypass is lifted.
        setLatencySamples(shouldBypass ? 0 : activeLatency);
    }

    bool getBypassed() const { return isBypassed.load(); }

    // Call this instead of setLatencySamples() from prepareToPlay() in derived
    // classes. It stores the value and respects the current bypass state.
    void setProcessingLatency(int latencySamples)
    {
        activeLatency = latencySamples;
        setLatencySamples(isBypassed.load() ? 0 : activeLatency);
    }

    // -----------------------------------------------------------------------------
    // Layout support
    // -----------------------------------------------------------------------------
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
            return false;

        const auto inLayout = layouts.getMainInputChannelSet();
        if (inLayout != juce::AudioChannelSet::mono()
            && inLayout != juce::AudioChannelSet::stereo())
            return false;

        return true;
    }

    // -----------------------------------------------------------------------------
    // Generic state recall (AudioProcessorParameterWithID)
    // Stores normalized values [0..1] for all registered parameters.
    // -----------------------------------------------------------------------------
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        juce::XmlElement xml("PLUGIN_STATE");

        for (auto* param : getParameters())
        {
            if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
            {
                auto* e = xml.createNewChildElement("PARAM");
                e->setAttribute("id", p->paramID);
                e->setAttribute("value", p->getValue()); // normalized 0..1
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
            const float value = (float)child->getDoubleAttribute("value");

            for (auto* param : getParameters())
            {
                if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
                {
                    if (p->paramID == paramID)
                    {
                        // Notifies host + updates DSP/UI
                        p->setValueNotifyingHost(value);
                        break;
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------------
    // Boilerplate (kept simple)
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
    // Call from prepareToPlay() of derived processors.
    void prepareBypassSmoother(double sampleRate, int maxBlockSize)
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        const int maxSamplesPerBlock = juce::jmax(1, maxBlockSize);

        wetMix.reset(sr, Nova::Config::SMOOTH_BYPASS_SECONDS);
        const float initialWet = isBypassed.load() ? 0.0f : 1.0f;
        wetMix.setCurrentAndTargetValue(initialWet);
        transitionActive = false;
        dryBuffer.setSize(juce::jmax(1, getTotalNumOutputChannels()),
            maxSamplesPerBlock,
            false,
            false,
            true);
        bypassPrepared = true;
    }

    // Call at the beginning of processBlock() in derived processors.
    // Returns false when fully bypassed and no transition is active.
    bool beginBypassProcess(juce::AudioBuffer<float>& buffer)
    {
        if (!bypassPrepared)
            prepareBypassSmoother(getSampleRate(), buffer.getNumSamples());

        if (dryBuffer.getNumChannels() < buffer.getNumChannels()
            || dryBuffer.getNumSamples() < buffer.getNumSamples())
        {
            dryBuffer.setSize(buffer.getNumChannels(),
                buffer.getNumSamples(),
                false,
                false,
                true);
        }

        if (bypassChanged.exchange(false))
        {
            const bool shouldBypass = isBypassed.load();
            if (!shouldBypass)
                reset();

            wetMix.setTargetValue(shouldBypass ? 0.0f : 1.0f);
            transitionActive = true;
        }

        const bool shouldBypass = isBypassed.load();
        const bool fullyBypassed = shouldBypass
            && !wetMix.isSmoothing()
            && wetMix.getCurrentValue() <= 0.0001f;

        if (fullyBypassed)
        {
            transitionActive = false;
            return false;
        }

        const int numSamples = buffer.getNumSamples();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* src = buffer.getReadPointer(ch);
            auto* dst = dryBuffer.getWritePointer(ch);
            juce::FloatVectorOperations::copy(dst, src, numSamples);
        }

        return true;
    }

    // Call at the end of processBlock() in derived processors.
    void endBypassProcess(juce::AudioBuffer<float>& buffer)
    {
        if (!transitionActive && !wetMix.isSmoothing())
            return;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            const float wet = wetMix.getNextValue();
            const float dry = 1.0f - wet;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* out = buffer.getWritePointer(ch);
                const auto* in = dryBuffer.getReadPointer(ch);
                out[i] = (out[i] * wet) + (in[i] * dry);
            }
        }

        if (!wetMix.isSmoothing())
            transitionActive = false;
    }

    // Backward-compatible helper (legacy callers).
    bool shouldProcess(juce::AudioBuffer<float>& buffer)
    {
        return beginBypassProcess(buffer);
    }

private:
    std::atomic<bool> isBypassed{ false };
    std::atomic<bool> bypassChanged{ false };
    int activeLatency = 0;
    bool bypassPrepared = false;
    bool transitionActive = false;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMix;
    juce::AudioBuffer<float> dryBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProcessorBase)
};
