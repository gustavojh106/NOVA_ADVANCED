#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "SessionCoordinator.h"
#include "Visualizer.h"
#include "Constants.h"

#include <atomic>

class NOVAAudioProcessor final : public juce::AudioProcessor,
    public juce::AudioProcessorParameter::Listener,
    private juce::AsyncUpdater,
    private AudioEngine::LatencyListener
{
public:
    NOVAAudioProcessor();
    ~NOVAAudioProcessor() override;

    // JUCE AudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    bool savePresetToFile(const juce::File& file);
    bool loadPresetFromFile(const juce::File& file);
    void clearSessionAndForgetStartupPreset();

    // == COMANDOS PÚBLICOS PARA EL EDITOR ==
    void requestAddPedal(const juce::String& type,
        Nova::ChainID chain,
        Nova::ZoneID zone,
        int insertIndex = -1);
    void requestRemovePedal(Nova::ChainID chain, int index);
    void requestMovePedal(Nova::ChainID chain,
        int fromIndex,
        int toIndex,
        Nova::ZoneID targetZone);
    void requestBypassPedal(Nova::ChainID chain, int index, bool bypassed);

    void toggleEngine();
    void cycleSwitcher();
    void cycleSwitcherWithMask(int enabledModesBitmask);
    void setSwitcherMode(Nova::SwitcherMode mode);
    void toggleTuner();

private:
    SessionCoordinator sessionCoordinator;

public:

    // == DATOS PÚBLICOS ==
    juce::ValueTree& pluginState;
    SimpleOscilloscope audioVisualizer;

    AudioEngine& getAudioEngine() { return audioEngine; }
    double getCpuUsage() const;
    float getInputPeak() const;
    float getOutputPeak() const;
    juce::RangedAudioParameter* getGlobalParameter(const juce::String& paramID) const;
    bool isEngineOn() const;
    Nova::SwitcherMode getSwitcherMode() const;

private:
    // AudioProcessorParameter::Listener
    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int, bool) override {}

    // AsyncUpdater / AudioEngine::LatencyListener
    void handleAsyncUpdate() override;
    void audioEngineLatencyChanged(int latencySamples) override;

    // Host bridge helpers.
    void createGlobalParameters();
    void refreshEngineEnabledIfNeeded(bool allowLogging = true);
    void refreshEngineGlobalParamsIfNeeded(bool force, bool allowLogging = true);
    void requestHostLatencyRefresh();
    void updateHostLatencyFromEngineNow();
    void applyHostLatencyIfChanged(int graphLatencySamples);
    void logRuntimeSnapshot(const juce::String& context, const AudioEngine::RuntimeGlobalParams& snapshot) const;
    void logStateSnapshot(const juce::String& context) const;
    void synchronizeEngineNow();
    void hardRefreshAudioEngineForCurrentIO();
    void invalidateEnginePushCaches(bool invalidateEngineEnabled, bool invalidateRuntimeGlobals);
    bool shouldPushEngineEnabled(bool current);
    bool shouldPushRuntimeGlobals(const AudioEngine::RuntimeGlobalParams& current, bool force);
    void resetSessionState(bool forgetStartupPreset);
    bool restoreStartupPresetIfAvailable();

    struct RuntimeGlobalParamAtomics
    {
        std::atomic<float> inputGainDb{ 0.0f };
        std::atomic<float> gateThresholdDb{ -100.0f };
        std::atomic<bool> forceMono{ false };
        std::atomic<float> hostTempoBpm{ 120.0f };
        std::atomic<bool> hostTempoValid{ false };
        std::atomic<bool> hostTransportPlaying{ false };
        std::atomic<float> outputVolumeDb{ 0.0f };
        std::atomic<float> outputLimiterDb{ 0.0f };
        std::atomic<float> outputMixRaw{ 100.0f };
        std::atomic<int> switchMode{ (int)Nova::SwitcherMode::LineA_Only };
        std::atomic<float> gainA{ 1.0f };
        std::atomic<float> panA{ 0.0f };
        std::atomic<float> widthA{ 1.0f };
        std::atomic<float> gainB{ 1.0f };
        std::atomic<float> panB{ 0.0f };
        std::atomic<float> widthB{ 1.0f };

        void store(const AudioEngine::RuntimeGlobalParams& snapshot) noexcept;
        AudioEngine::RuntimeGlobalParams load() const noexcept;
    };

    AudioEngine audioEngine;
    std::atomic<bool> hasPushedRuntimeGlobals{ false };
    std::atomic<bool> hasPushedEngineEnabled{ false };
    std::atomic<bool> lastEngineEnabled{ false };
    std::atomic<bool> deferredRuntimeSnapshotLog{ false };
    std::atomic<bool> deferredEngineToggleLog{ false };
    std::atomic<bool> audioEnginePreparedForHostLatency{ false };
    std::atomic<bool> hostLatencyRefreshPending{ false };
    std::atomic<int> lastReportedHostLatencySamples{ -1 };
    std::atomic<int> hostTransportPollCounter{ 0 };
    RuntimeGlobalParamAtomics lastRuntimeGlobalParams;

    juce::AudioParameterBool* engineOnParam = nullptr;
    juce::AudioParameterChoice* switchModeParam = nullptr;
    juce::AudioParameterFloat* inputGainParam = nullptr;
    juce::AudioParameterFloat* inputGateParam = nullptr;
    juce::AudioParameterBool* forceMonoParam = nullptr;
    juce::AudioParameterFloat* gainAParam = nullptr;
    juce::AudioParameterFloat* panAParam = nullptr;
    juce::AudioParameterFloat* widthAParam = nullptr;
    juce::AudioParameterFloat* gainBParam = nullptr;
    juce::AudioParameterFloat* panBParam = nullptr;
    juce::AudioParameterFloat* widthBParam = nullptr;
    juce::AudioParameterFloat* outputVolParam = nullptr;
    juce::AudioParameterFloat* outputLimiterParam = nullptr;
    juce::AudioParameterFloat* outputMixParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessor)
};
