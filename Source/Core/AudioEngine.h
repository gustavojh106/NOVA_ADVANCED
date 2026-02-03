#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <vector>

#include "Constants.h"
#include "DSP/Global/InputChain.h"
#include "DSP/Global/OutputChain.h"
#include "DSP/Global/ChannelStrip.h"
#include "DSP/Services/TunerService.h"

// ==========================================================
// MOTOR DE AUDIO PRINCIPAL
// ==========================================================
class AudioEngine : public juce::Thread
{
public:
    AudioEngine();
    ~AudioEngine() override;

    void prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut);
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // Gestión de cadenas (thread-safe)
    void addPedal(const juce::String& type, Nova::ChainID chain, int index);
    void removePedal(Nova::ChainID chain, int index);
    void clearAll();

    // Control global
    void setEngineEnabled(bool enabled);
    void updateMixer(float gainA, float gainB, Nova::SwitcherMode mode); // legacy wrapper (no-op)

    // Introspección para UI
    const std::vector<juce::AudioProcessorGraph::Node::Ptr>& getNodes(Nova::ChainID chain) const;

    double getCpuLoad() const;
    int getLatencyNumSamples() const;

    // Tuner
    void setTunerEnabled(bool shouldEnable);

    // Alias (compatibilidad con código existente)
    bool isTunerEnabled() const { return tunerEnabled.load(); }
    bool getTunerEnabled() const { return tunerEnabled.load(); }

    // Tuner data (delegado al servicio)
    float getTunerPitch() const { return tunerService.getCurrentPitch(); }
    float getTunerClarity() const { return tunerService.getCurrentClarity(); }
    float getTunerRMS() const { return tunerService.getCurrentRMS(); }

    // Pedal bypass (placeholder)
    void setPedalBypassed(Nova::ChainID chain, int index, bool bypassed);

    // Thread
    void run() override;

    // Params
    void setTuningOffset(int semitones) { tuningOffset = semitones; }
    int  getTuningOffset() const { return tuningOffset.load(); }

    void updateGlobalParams(const juce::ValueTree& settings,
        const juce::ValueTree& lineA,
        const juce::ValueTree& lineB);

private:
    // Graph build
    void rebuildGraph();
    void connectChainToGain(const std::vector<juce::AudioProcessorGraph::Node::Ptr>& nodes,
        juce::AudioProcessorGraph::NodeID targetStripID);

    // DSP / tuner helpers (legacy kept)
    float calculateFrequency(const float* signal, int numSamples, double sampleRate);
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate);

private:
    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

    // Concurrency protection
    juce::CriticalSection vectorLock;
    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChainA;
    std::vector<juce::AudioProcessorGraph::Node::Ptr> nodesChainB;

    // Hardware I/O nodes
    juce::AudioProcessorGraph::Node::Ptr inputNode;
    juce::AudioProcessorGraph::Node::Ptr outputNode;

    // Internal chain nodes
    juce::AudioProcessorGraph::Node::Ptr inputChainNode;
    juce::AudioProcessorGraph::Node::Ptr stripNodeA;
    juce::AudioProcessorGraph::Node::Ptr stripNodeB;
    juce::AudioProcessorGraph::Node::Ptr outputChainNode;

    // Runtime
    double currentSampleRate = 44100.0;
    int    currentBlockSize = 512;
    int    numInputChannels = 2;

    std::atomic<bool>   isEngineOn{ false };
    std::atomic<double> cpuUsage{ 0.0 };

    double currentRate = 0.0;
    int startupCounter = 0;

    // Global mix (dry/wet)
    std::atomic<float> currentGlobalMix{ 1.0f }; // 1.0 = 100% wet
    juce::AudioBuffer<float> dryBuffer;

    // Tuner
    TunerService tunerService;
    std::atomic<bool> tunerEnabled{ false };
    std::atomic<int>  tuningOffset{ 0 };
};
