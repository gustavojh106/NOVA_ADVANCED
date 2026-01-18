#pragma once
#include <JuceHeader.h>
#include "Common.h"
#include <vector>
#include <atomic>

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
class AudioEngine : public juce::Thread
{
public:
    AudioEngine();
    ~AudioEngine();

    void prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut);
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // Gestión de Cadenas (Thread-Safe)
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

    // TUNER CONTROL
    void setTunerEnabled(bool shouldEnable);

    // Getters Inline (Soluciona tus errores de compilación)
    bool isTunerEnabled() const { return tunerEnabled; }
    // ALIAS DE COMPATIBILIDAD (Para arreglar el error en PluginProcessor.cpp)
    bool getTunerEnabled() const { return tunerEnabled; }

    // TUNER DATA (Getters seguros)
    float getTunerPitch() const { return currentPitch; }
    int getTunerNote() const { return currentNote; }
    float getTunerCents() const { return currentCents; }
    float getTunerRMS() const { return currentRMS; }

    void setPedalBypassed(Nova::ChainID chain, int index, bool bypassed);

    void run() override;

    void setTuningOffset(int semitones) { tuningOffset = semitones; }
    int getTuningOffset() const { return tuningOffset; }
    float getTunerClarity() const { return currentClarity; }
private:

    std::atomic<float> currentClarity{ 0.0f };
    std::atomic<int> tuningOffset{ 0 };

    void rebuildGraph();

    // Core Conexiones
    void connectChainToGain(const std::vector<juce::AudioProcessorGraph::Node::Ptr>& nodes,
        juce::AudioProcessorGraph::NodeID gainNodeID);

    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

    // Protección de concurrencia
    juce::CriticalSection vectorLock;
    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChainA;
    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChainB;

    juce::AudioProcessorGraph::Node::Ptr inputNode;
    juce::AudioProcessorGraph::Node::Ptr outputNode;
    juce::AudioProcessorGraph::Node::Ptr gainNodeA;
    juce::AudioProcessorGraph::Node::Ptr gainNodeB;

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    int numInputChannels = 2;

    std::atomic<bool> isEngineOn{ false };
    double currentRate = 0.0;
    int startupCounter = 0;
    std::atomic<double> cpuUsage{ 0.0 };

    // TUNER DATA
    std::atomic<bool> tunerEnabled{ false };
    std::atomic<float> currentPitch{ 0.0f };
    std::atomic<int> currentNote{ 0 };
    std::atomic<float> currentCents{ 0.0f };
    std::atomic<float> currentRMS{ 0.0f };

    // Buffers para el afinador
    static constexpr int TUNER_FIFO_SIZE = 16384;
    static constexpr int TUNER_PROCESS_SIZE = 4096;

    juce::AbstractFifo tunerFifo{ TUNER_FIFO_SIZE };
    std::vector<float> tunerCircularBuffer;
    std::vector<float> tunerWorkBuffer;

    float calculateFrequency(const float* signal, int numSamples, double sampleRate);
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate);
};