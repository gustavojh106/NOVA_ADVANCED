#pragma once

#include <JuceHeader.h>
#include "../../PedalSignalTelemetry.h"
#include "../SignalGuard.h"

#include <array>
#include <atomic>
#include <cmath>

/*
    NOVA ChannelStripProcessor - professional line strip

    Goals:
    - No locks and no allocations in processBlock().
    - Thread-safe parameter handoff through atomics.
    - Sample-accurate smoothing for gain, pan and width.
    - Reliable mute behavior with early clear once fully muted.
    - Safer stereo width law with high-width compensation.
    - Better mono/non-stereo fallback.
*/

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

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto in = layouts.getMainInputChannelSet();
        const auto out = layouts.getMainOutputChannelSet();

        if (in != out)
            return false;

        return in == juce::AudioChannelSet::mono()
            || in == juce::AudioChannelSet::stereo();
    }

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
        int mutedFastPathBlocks = 0;
        int guardInvalidSamples = 0;
        int guardClippedSamples = 0;
        int guardDenormalSamples = 0;
        float gainMin = 1.0e9f;
        float gainMax = 0.0f;
        float panMin = 1.0e9f;
        float panMax = -1.0e9f;
        float widthMin = 1.0e9f;
        float widthMax = 0.0f;

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
            mutedFastPathBlocks = 0;
            guardInvalidSamples = 0;
            guardClippedSamples = 0;
            guardDenormalSamples = 0;
            gainMin = 1.0e9f;
            gainMax = 0.0f;
            panMin = 1.0e9f;
            panMax = -1.0e9f;
            widthMin = 1.0e9f;
            widthMax = 0.0f;
        }

        void captureControls(float gain, float pan, float width) noexcept
        {
            gainMin = juce::jmin(gainMin, gain);
            gainMax = juce::jmax(gainMax, gain);
            panMin = juce::jmin(panMin, pan);
            panMax = juce::jmax(panMax, pan);
            widthMin = juce::jmin(widthMin, width);
            widthMax = juce::jmax(widthMax, width);
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

    static float sanitizeGain(float value) noexcept;
    static float sanitizePan(float value) noexcept;
    static float sanitizeWidth(float value) noexcept;
    static float calculateWidthCompensation(float width) noexcept;
    static void calculateBalanceGains(float pan, float& gainL, float& gainR) noexcept;

    void applyParameterTargets(bool forceSync) noexcept;
    void applyGainOnly(juce::AudioBuffer<float>& buffer, bool expectMuted) noexcept;
    void emitTelemetry(juce::AudioBuffer<float>& buffer);

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> panSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmooth;

    NovaDiagnostics::PedalSignalTelemetry signalTelemetry{ "channel-strip" };
    DebugTelemetry debugTelemetry;
    juce::String telemetryTag{ "channel-strip" };

    std::atomic<float> targetGain{ 1.0f };
    std::atomic<float> targetPan{ 0.0f };
    std::atomic<float> targetWidth{ 1.0f };

    float currentTargetGain = 1.0f;
    float currentTargetPan = 0.0f;
    float currentTargetWidth = 1.0f;

    bool hardSyncParams = true;
};
