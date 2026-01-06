#pragma once
#include <JuceHeader.h>
#include "Common.h"

// ==========================================================
// PROCESADOR DE GANANCIA (Control de Volumen Independiente)
// ==========================================================
class SimpleGainProcessor : public juce::AudioProcessor
{
public:
    SimpleGainProcessor();

    void setGain(float gain);
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    // Boilerplate de JUCE
    const juce::String getName() const override { return "Gain"; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 0; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

private:
    float currentGain = 1.0f;
    float targetGain = 1.0f;
};

// ==========================================================
// MOTOR DE AUDIO PRINCIPAL
// ==========================================================
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    void prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut);
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // Gestión de Cadenas
    void addPedal(const juce::String& type, Nova::ChainID chain, int index);
    void removePedal(Nova::ChainID chain, int index);
    void clearAll();

    // Control
    void setEngineEnabled(bool enabled);
    void updateMixer(float gainA, float gainB, Nova::SwitcherMode mode);

    // Introspección (para la UI)
    const std::vector<juce::AudioProcessorGraph::Node::Ptr>& getNodes(Nova::ChainID chain) const;
    double getCpuLoad() const;
    int getLatencyNumSamples() const;
    //TUNER
    void setTunerEnabled(bool enabled);
    bool isTunerEnabled() const;

    // Getters para la UI (Thread-Safe)
    float getTunerPitch() const; // Frecuencia Hz
    int getTunerNote() const;    // Nota MIDI (0-127)
    float getTunerCents() const; // Desviación (-50 a +50)
    float getTunerRMS() const { return currentRMS; }
    void setPedalBypassed(Nova::ChainID chain, int index, bool bypassed);


private:
    void rebuildGraph();
    std::atomic<float> currentRMS{ 0.0f };
    // Función Core de Conexionado
    void connectChainToGain(const std::vector<juce::AudioProcessorGraph::Node::Ptr>& nodes,
        juce::AudioProcessorGraph::NodeID gainNodeID);

    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChainA;
    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChainB;

    juce::AudioProcessorGraph::Node::Ptr inputNode;
    juce::AudioProcessorGraph::Node::Ptr outputNode;

    // Ganancias Independientes (Faders internos)
    juce::AudioProcessorGraph::Node::Ptr gainNodeA;
    juce::AudioProcessorGraph::Node::Ptr gainNodeB;

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    int numInputChannels = 2;
    bool isEngineOn = false; // Empieza en falso por seguridad
    // --- VARIABLES ESENCIALES PARA EL MOTOR ---
    double currentRate = 0.0;      // <--- ESTA FALTABA. Guarda la frecuencia de muestreo (44100, 48000, etc.)
    //int currentBlockSize = 0;      // Tamaño del buffer

    // Variables de estado
    bool inPanicState = false;     // Para protección contra ruidos fuertes
    int startupCounter = 0;        // Para silenciar el inicio

    // Variable para medición de CPU
    double cpuUsage = 0.0;         // El valor final del consumo

    //TUNER
    std::atomic<bool> tunerEnabled{ false };
    std::atomic<float> currentPitch{ 0.0f };
    std::atomic<int> currentNote{ 0 };
    std::atomic<float> currentCents{ 0.0f };
    float lastDetectedFreq = 0.0f;
    int stabilityCounter = 0;
    // Buffer circular para análisis (4096 es buen tamaño para detectar graves)
    juce::AudioBuffer<float> tunerBuffer{ 1, 4096 };
    int tunerWriteIndex = 0;

    // Método interno de cálculo
    void processTunerAlgorithm();
    float calculateFrequency(const float* signal, int numSamples, double sampleRate);
};