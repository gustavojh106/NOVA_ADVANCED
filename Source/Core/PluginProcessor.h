#pragma once

#include <JuceHeader.h>
#include "AudioEngine.h"

// Definiciones de IDs
namespace IDs
{
    static const juce::Identifier PEDAL_TAG("PEDAL");
    static const juce::Identifier PEDAL_TYPE("type");
    static const juce::Identifier MAIN_STATE("NOVA_STATE");
}

class NOVAAudioProcessor : public juce::AudioProcessor,
    public juce::ValueTree::Listener
{
public:
    NOVAAudioProcessor();
    ~NOVAAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Estado y Gestión
    juce::ValueTree pluginState;
    void requestAddPedal(const juce::String& pedalType);
    void requestRemovePedal(int index);

    // Getter para la UI
    AudioEngine& getAudioEngine() { return audioEngine; }

private:
    void valueTreeChildAdded(juce::ValueTree& parentTree, juce::ValueTree& childWhichHasBeenAdded) override;
    void valueTreeChildRemoved(juce::ValueTree& parentTree, juce::ValueTree& childWhichHasBeenRemoved, int moveFromIndex) override;

    void rebuildChain();

    AudioEngine audioEngine; // El Motor SOTA

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessor)
};