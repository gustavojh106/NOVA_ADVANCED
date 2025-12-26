/*
  ==============================================================================
    NOVAAudioProcessor.h
    Arquitectura SOTA: Data-Driven con ValueTree
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "ProcessorBase.h"

class NOVAAudioProcessor : public juce::AudioProcessor,
    public juce::ValueTree::Listener // <--- AHORA ESCUCHAMOS DATOS
{
public:
    //==============================================================================
    NOVAAudioProcessor();
    ~NOVAAudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
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

    //==============================================================================
    // ARQUITECTURA SOTA: GESTIÓN DE ESTADO
    //==============================================================================

    // 1. La Fuente de la Verdad (El Modelo)
    juce::ValueTree pluginState;

    // 2. Función pública para pedir cambios (La UI llama a esto, no toca el grafo)
    void requestAddPedal(const juce::String& pedalType);
    void requestRemovePedal(int index);
    const std::vector<juce::AudioProcessorGraph::Node::Ptr>& getNodes() const
    {
        return nodesChain;
    }

private:
    // 3. Callbacks del Listener (El Processor reacciona a los cambios de datos)
    void valueTreeChildAdded(juce::ValueTree& parentTree, juce::ValueTree& childWhichHasBeenAdded) override;
    void valueTreeChildRemoved(juce::ValueTree& parentTree, juce::ValueTree& childWhichHasBeenRemoved, int moveFromIndex) override;

    // 4. Funciones internas para manejar el Grafo de Audio
    void syncAudioGraph(); // Reconstruye las conexiones
    void connectNodes(juce::AudioProcessorGraph::Node::Ptr source, juce::AudioProcessorGraph::Node::Ptr dest);

    // Grafo Principal
    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

    // Lista interna de Nodos (Solo para el motor de audio, la UI no la ve)
    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessor)
};