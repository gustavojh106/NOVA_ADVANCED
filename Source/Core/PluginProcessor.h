#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "Visualizer.h"
#include "Constants.h"

class NOVAAudioProcessor final : public juce::AudioProcessor,
    public juce::ValueTree::Listener
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
    void requestAddPedal(const juce::String& type, Nova::ChainID chain, Nova::ZoneID zone);
    void requestRemovePedal(Nova::ChainID chain, int index);
    void requestBypassPedal(Nova::ChainID chain, int index, bool bypassed);

    void toggleEngine();
    void cycleSwitcher();
    void toggleTuner();

    // == DATOS PÚBLICOS ==
    juce::ValueTree pluginState;
    SimpleOscilloscope audioVisualizer;

    AudioEngine& getAudioEngine() { return audioEngine; }
    double getCpuUsage() const;
    float getInputPeak() const;
    float getOutputPeak() const;
    juce::RangedAudioParameter* getGlobalParameter(const juce::String& paramID) const;
    bool isEngineOn() const;
    Nova::SwitcherMode getSwitcherMode() const;

private:
    // ValueTree Listener
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int index) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;

    // Host bridge helpers.
    void createGlobalParameters();
    AudioEngine::RuntimeGlobalParams makeRuntimeGlobalParams() const;
    void writeParameterStateToTree(juce::ValueTree settings, juce::ValueTree lineA, juce::ValueTree lineB) const;
    void applyTreeStateToParameters(juce::ValueTree settings, juce::ValueTree lineA, juce::ValueTree lineB);
    void refreshEngineEnabledIfNeeded();
    void refreshEngineGlobalParamsIfNeeded(bool force);
    bool applyStateTree(const juce::ValueTree& loadedState, const juce::File* presetFile);
    void updateGlobalParamsFromState();
    void updateMixerFromState();
    void logRuntimeSnapshot(const juce::String& context, const AudioEngine::RuntimeGlobalParams& snapshot) const;
    void logStateSnapshot(const juce::String& context) const;
    void synchronizeEngineNow();
    void resetSessionState(bool forgetStartupPreset);
    bool restoreStartupPresetIfAvailable();

    AudioEngine audioEngine;
    bool suppressStateCallbacks = false;
    bool suppressParamSync = false;
    bool hasPushedRuntimeGlobals = false;
    bool hasPushedEngineEnabled = false;
    bool lastEngineEnabled = false;
    AudioEngine::RuntimeGlobalParams lastRuntimeGlobalParams;

    juce::AudioParameterBool* engineOnParam = nullptr;
    juce::AudioParameterChoice* switchModeParam = nullptr;
    juce::AudioParameterFloat* inputGainParam = nullptr;
    juce::AudioParameterFloat* inputGateParam = nullptr;
    juce::AudioParameterInt* inputTransposeParam = nullptr;
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
