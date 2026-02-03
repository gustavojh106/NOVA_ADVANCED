#pragma once

#include <JuceHeader.h>
#include <atomic>

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

        // Optional: clear internal state when re-enabling to avoid stale tails
        if (!shouldBypass)
            reset();
    }

    bool getBypassed() const { return isBypassed.load(); }

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
    // Returns true if the derived processor should process.
    // If false, the caller should return immediately (true-bypass behavior).
    bool shouldProcess(juce::AudioBuffer<float>& /*buffer*/)
    {
        return !isBypassed.load();
    }

private:
    std::atomic<bool> isBypassed{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProcessorBase)
};
