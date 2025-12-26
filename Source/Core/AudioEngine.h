#pragma once
#include <JuceHeader.h>
#include "PedalRegistry.h"

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    // Configuración Inicial
    void prepare(double sampleRate, int samplesPerBlock, int numInputChannels, int numOutputChannels);

    // Proceso de Audio (Real-Time Safe)
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // Gestión de Recursos
    void reset();

    // Gestión de Pedales (SOTA: Surgical Graph Mutation)
    void addPedal(const juce::String& pedalType);
    void removePedal(int index);
    void clearChain();

    // Introspección
    const std::vector<juce::AudioProcessorGraph::Node::Ptr>& getActiveNodes() const;

    // SOTA: Reporte de Latencia para el Host (DAW)
    int getLatencyNumSamples() const;

private:
    // Helpers internos de conexión
    bool isStreamHealthy(const juce::AudioBuffer<float>& buffer);

    // Conecta dos nodos gestionando la lógica Mono/Stereo y Omni-Input
    void connectNodes(juce::AudioProcessorGraph::NodeID sourceID, juce::AudioProcessorGraph::NodeID destID);

    // Desconecta todo entre dos nodos (para limpiar antes de insertar)
    void disconnectNodes(juce::AudioProcessorGraph::NodeID sourceID, juce::AudioProcessorGraph::NodeID destID);

    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

    // Cadena de pedales de usuario (excluye I/O del sistema)
    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChain;

    // IDs Persistentes del Sistema (Nunca cambian después del prepare)
    juce::AudioProcessorGraph::NodeID systemInputID;
    juce::AudioProcessorGraph::NodeID systemOutputID;

    double currentRate = 0.0;
    int currentBlockSize = 0;
    int hardwareInputs = 2;
    int hardwareOutputs = 2;

    int startupCounter = 0;
    bool inPanicState = false;
};