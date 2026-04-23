#pragma once

#include <JuceHeader.h>
#include "../../PedalSignalTelemetry.h"

class ChannelStripProcessor final : public juce::AudioProcessor
{
public:
    ChannelStripProcessor();
    ~ChannelStripProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // gainVal: linear gain (0.0 .. 2.0)
    // panVal: -1.0 (L) .. +1.0 (R)
    // widthVal: 0.0 (mono) .. 2.0 (extra wide), 1.0 = normal
    void setParams(float gainVal, float panVal, float widthVal);
    void setTelemetryTag(const juce::String& newTag);

    // Boilerplate (JUCE)
    const juce::String getName() const override { return "ChannelStrip"; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 0; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

private:
    struct DebugTelemetry
    {
        NovaDiagnostics::SignalStageWindowMetrics postGainStage;
        float midPeak = 0.0f;
        float sideInputPeak = 0.0f;
        float sideOutputPeak = 0.0f;
        float widthCompMin = 1.0e9f;
        float widthCompMax = 0.0f;
        float panGainLMin = 1.0e9f;
        float panGainLMax = 0.0f;
        float panGainRMin = 1.0e9f;
        float panGainRMax = 0.0f;
        float muteLeakPeak = 0.0f;
        int muteLeakSamples = 0;

        void resetWindow() noexcept
        {
            postGainStage.reset();
            midPeak = 0.0f;
            sideInputPeak = 0.0f;
            sideOutputPeak = 0.0f;
            widthCompMin = 1.0e9f;
            widthCompMax = 0.0f;
            panGainLMin = 1.0e9f;
            panGainLMax = 0.0f;
            panGainRMin = 1.0e9f;
            panGainRMax = 0.0f;
            muteLeakPeak = 0.0f;
            muteLeakSamples = 0;
        }

        void captureStereo(float mid,
            float sideInput,
            float sideOutput,
            float widthComp,
            float gainL,
            float gainR,
            float outL,
            float outR,
            bool expectMuted) noexcept
        {
            midPeak = juce::jmax(midPeak, std::abs(mid));
            sideInputPeak = juce::jmax(sideInputPeak, std::abs(sideInput));
            sideOutputPeak = juce::jmax(sideOutputPeak, std::abs(sideOutput));
            widthCompMin = juce::jmin(widthCompMin, widthComp);
            widthCompMax = juce::jmax(widthCompMax, widthComp);
            panGainLMin = juce::jmin(panGainLMin, gainL);
            panGainLMax = juce::jmax(panGainLMax, gainL);
            panGainRMin = juce::jmin(panGainRMin, gainR);
            panGainRMax = juce::jmax(panGainRMax, gainR);

            if (expectMuted)
            {
                const float leakPeak = juce::jmax(std::abs(outL), std::abs(outR));
                muteLeakPeak = juce::jmax(muteLeakPeak, leakPeak);
                if (leakPeak >= 1.0e-4f)
                    ++muteLeakSamples;
            }
        }
    };

    juce::dsp::Gain<float> gain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> panSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmooth;
    NovaDiagnostics::PedalSignalTelemetry signalTelemetry { "channel-strip" };
    DebugTelemetry debugTelemetry;
    juce::String telemetryTag { "channel-strip" };

    float targetGain = 1.0f;
    float targetPan = 0.0f;
    float targetWidth = 1.0f;
    bool hardSyncParams = true;
};
