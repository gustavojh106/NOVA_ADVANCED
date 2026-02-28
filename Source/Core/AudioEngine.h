#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <deque>
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

    // Gestion de cadenas
    void addPedal(const juce::String& type, Nova::ChainID chain, int index);
    void removePedal(Nova::ChainID chain, int index);
    void clearAll();

    // Control global
    void setEngineEnabled(bool enabled);
    void updateMixer(float gainA, float gainB, Nova::SwitcherMode mode); // legacy wrapper (no-op)

    // Introspeccion para UI
    std::vector<juce::AudioProcessorGraph::Node::Ptr> getNodes(Nova::ChainID chain) const;
    juce::AudioProcessor* getProcessorForPedal(Nova::ChainID chain, int index);

    double getCpuLoad() const;
    int getLatencyNumSamples() const;

    // Tuner
    void setTunerEnabled(bool shouldEnable);

    // Alias (compatibilidad con codigo existente)
    bool isTunerEnabled() const { return tunerEnabled.load(); }
    bool getTunerEnabled() const { return tunerEnabled.load(); }

    // Tuner data (delegado al servicio)
    float getTunerPitch() const { return tunerService.getCurrentPitch(); }
    float getTunerClarity() const { return tunerService.getCurrentClarity(); }
    float getTunerRMS() const { return tunerService.getCurrentRMS(); }

    // Pedal bypass
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
    struct GlobalParamsSnapshot
    {
        float inputGainDb = 0.0f;
        float gateThresholdDb = -100.0f;
        bool forceMono = false;
        int inputTranspose = 0;

        float outputVolumeDb = 0.0f;
        float outputLimiterDb = 0.0f;
        float outputMixRaw = 100.0f; // Backward compatible: 0..1 and 0..100

        int switchMode = (int)Nova::SwitcherMode::Dual_Parallel;

        float gainA = 1.0f;
        float panA = 0.0f;
        float widthA = 1.0f;

        float gainB = 1.0f;
        float panB = 0.0f;
        float widthB = 1.0f;
    };

    enum class GraphCommandType
    {
        AddPedal,
        RemovePedal,
        ClearAll,
        SetPedalBypass,
        SetEngineEnabled
    };

    struct GraphCommand
    {
        GraphCommandType type = GraphCommandType::ClearAll;
        juce::String pedalType;
        Nova::ChainID chain = Nova::ChainID::LineA;
        int index = -1;
        bool flag = false;
    };

    // Graph build
    void rebuildGraph();
    void connectChainToGain(const std::vector<juce::AudioProcessorGraph::Node::Ptr>& nodes,
        juce::AudioProcessorGraph::NodeID targetStripID);

    // Command/control plane
    void enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe);
    void flushPendingGraphCommands(bool suspendGraph);
    void applyGraphCommandNow(const GraphCommand& cmd, bool& topologyChanged, bool& resetRequested);
    void applyPendingGlobalParams();
    void applyGlobalParamsNow(const GlobalParamsSnapshot& snapshot);
    void resetGraphStateNow();
    bool sanitizeAudioBuffer(juce::AudioBuffer<float>& buffer);

    // DSP / tuner helpers (legacy kept)
    float calculateFrequency(const float* signal, int numSamples, double sampleRate);
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate);

private:
    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

    // Concurrency protection
    mutable juce::CriticalSection vectorLock;
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

    // Thread-safe control plane
    juce::CriticalSection graphCommandLock;
    std::deque<GraphCommand> pendingGraphCommands;

    juce::SpinLock globalParamsLock;
    GlobalParamsSnapshot pendingGlobalParams;
    std::atomic<bool> globalParamsDirty{ false };

    // Runtime integrity / recovery
    int consecutiveCorruptBlocks = 0;
    int recoveryCooldownBlocks = 0;
    juce::Thread::ThreadID audioThreadID = {};
};
