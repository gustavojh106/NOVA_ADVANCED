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

private:
    // ValueTree Listener
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int index) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;

    // Helpers internos (legibilidad, NO cambian funcionalidad)
    juce::ValueTree getSettingsTree() const;
    juce::ValueTree getLineTree(Nova::ChainID chain) const;
    void ensureStateStructure();
    void applyDefaultStateIfNeeded();
    void updateGlobalParamsFromState();
    void updateMixerFromState();

    AudioEngine audioEngine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessor)
};
