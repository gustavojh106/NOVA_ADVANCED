#pragma once
#include <JuceHeader.h>
#include "Common.h"
#include "AudioEngine.h"
#include "Visualizer.h" // Asumo que tienes este archivo del paso anterior

class NOVAAudioProcessor : public juce::AudioProcessor, public juce::ValueTree::Listener
{
public:
    NOVAAudioProcessor();
    ~NOVAAudioProcessor() override;

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
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    // == COMANDOS PÚBLICOS PARA EL EDITOR ==
    void requestAddPedal(const juce::String& type, Nova::ChainID chain, Nova::ZoneID zone);
    void requestRemovePedal(Nova::ChainID chain, int index);
    void toggleEngine();
    void cycleSwitcher();

    // == DATOS PÚBLICOS ==
    juce::ValueTree pluginState;
    SimpleOscilloscope audioVisualizer;
    AudioEngine& getAudioEngine() { return audioEngine; }
    double getCpuUsage() const; 
private:
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;

    void updateMixerFromState();

    AudioEngine audioEngine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessor)
};