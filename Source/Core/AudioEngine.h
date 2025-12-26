#pragma once
#include <JuceHeader.h>
#include "PedalRegistry.h"

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    // --- 1. CICLO DE VIDA ---
    // CAMBIO: Ahora pedimos entradas y salidas por separado para precisión quirúrgica
    void prepare(double sampleRate, int samplesPerBlock, int numInputChannels, int numOutputChannels);

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);
    void reset();

    // --- 2. GESTIÓN ---
    void addPedal(const juce::String& pedalType);
    void removePedal(int index);
    void clearChain();

    const std::vector<juce::AudioProcessorGraph::Node::Ptr>& getActiveNodes() const;

private:
    void rebuildGraph();
    void connectNodes(juce::AudioProcessorGraph::Node::Ptr source, juce::AudioProcessorGraph::Node::Ptr dest);
    bool isStreamHealthy(const juce::AudioBuffer<float>& buffer);

    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;
    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChain;

    double currentRate = 0.0;
    int currentBlockSize = 0;

    // CAMBIO: Guardamos la configuración real del hardware
    int hardwareInputs = 2;
    int hardwareOutputs = 2;

    int startupCounter = 0;
    bool inPanicState = false;
};