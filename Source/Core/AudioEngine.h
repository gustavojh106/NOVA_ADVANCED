#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>

#include "Constants.h"
#include "DSP/Global/InputChain.h"
#include "DSP/Global/OutputChain.h"
#include "DSP/Global/ChannelStrip.h"
#include "DSP/Services/TunerService.h"

class AudioEngine : public juce::Thread
{
public:
    struct ChainNodeView
    {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::String pedalID;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
    };

    struct RuntimeGlobalParams
    {
        float inputGainDb = 0.0f;
        float gateThresholdDb = -100.0f;
        bool forceMono = false;
        int inputTranspose = 0;

        float outputVolumeDb = 0.0f;
        float outputLimiterDb = 0.0f;
        float outputMixRaw = 100.0f;

        int switchMode = (int)Nova::SwitcherMode::LineA_Only;

        float gainA = 1.0f;
        float panA = 0.0f;
        float widthA = 1.0f;

        float gainB = 1.0f;
        float panB = 0.0f;
        float widthB = 1.0f;
    };

    AudioEngine();
    ~AudioEngine() override;

    void prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut);
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    void addPedal(const juce::String& type,
        Nova::ChainID chain,
        int index,
        Nova::ZoneID zone = Nova::ZoneID::Pre,
        juce::String pedalID = {});
    void removePedal(Nova::ChainID chain, int index);
    void clearAll();

    void setEngineEnabled(bool enabled);
    void updateMixer(float gainA, float gainB, Nova::SwitcherMode mode);

    std::vector<ChainNodeView> getNodes(Nova::ChainID chain) const;
    juce::AudioProcessor* getProcessorForPedal(Nova::ChainID chain, int index);

    double getCpuLoad() const;
    int getLatencyNumSamples() const;
    float getLastInputPeak() const { return lastInputPeak.load(); }
    float getLastOutputPeak() const { return lastOutputPeak.load(); }
    juce::String buildDiagnosticReport() const;

    void setTunerEnabled(bool shouldEnable);

    bool isTunerEnabled() const { return tunerEnabled.load(); }
    bool getTunerEnabled() const { return tunerEnabled.load(); }

    float getTunerPitch() const { return tunerService.getCurrentPitch(); }
    float getTunerClarity() const { return tunerService.getCurrentClarity(); }
    float getTunerRMS() const { return tunerService.getCurrentRMS(); }

    void setPedalBypassed(Nova::ChainID chain, int index, bool bypassed);
    void synchronizeProcessingState();

    void run() override;

    void setTuningOffset(int semitones) { tuningOffset = semitones; }
    int  getTuningOffset() const { return tuningOffset.load(); }

    void updateGlobalParams(const RuntimeGlobalParams& snapshot);
    void updateGlobalParams(const juce::ValueTree& settings,
        const juce::ValueTree& lineA,
        const juce::ValueTree& lineB);

private:
    struct ChainNodeSlot
    {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::String pedalID;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
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
        juce::String pedalID;
        Nova::ChainID chain = Nova::ChainID::LineA;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
        int index = -1;
        bool flag = false;
    };

    void rebuildGraph();
    void connectChainToGain(const std::vector<ChainNodeSlot>& nodes,
        juce::AudioProcessorGraph::NodeID targetStripID);

    void enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe);
    void flushPendingGraphCommands(bool suspendGraph);
    void applyGraphCommandNow(const GraphCommand& cmd, bool& topologyChanged, bool& resetRequested);
    void applyPendingGlobalParams();
    void applyGlobalParamsNow(const RuntimeGlobalParams& snapshot);
    void resetGraphStateNow();
    bool sanitizeAudioBuffer(juce::AudioBuffer<float>& buffer);
    void updateDryWetLatencyCompensation();
    static float measureBlockPeak(const juce::AudioBuffer<float>& buffer);
    juce::String describeChainState(const std::vector<ChainNodeSlot>& chain) const;

    float calculateFrequency(const float* signal, int numSamples, double sampleRate);
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate);

private:
    std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

    mutable juce::CriticalSection vectorLock;
    std::vector<ChainNodeSlot> nodesChainA;
    std::vector<ChainNodeSlot> nodesChainB;

    juce::AudioProcessorGraph::Node::Ptr inputNode;
    juce::AudioProcessorGraph::Node::Ptr outputNode;

    juce::AudioProcessorGraph::Node::Ptr inputChainNode;
    juce::AudioProcessorGraph::Node::Ptr stripNodeA;
    juce::AudioProcessorGraph::Node::Ptr stripNodeB;
    juce::AudioProcessorGraph::Node::Ptr outputChainNode;

    double currentSampleRate = 44100.0;
    int    currentBlockSize = 512;
    int    numInputChannels = 2;

    std::atomic<bool>   isEngineOn{ false };
    std::atomic<double> cpuUsage{ 0.0 };

    double currentRate = 0.0;
    int startupCounter = 0;

    std::atomic<float> currentGlobalMix{ 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMixSmooth;
    juce::dsp::DryWetMixer<float> dryWetMixer{ Nova::Config::MAX_GRAPH_LATENCY_SAMPLES };

    TunerService tunerService;
    std::atomic<bool> tunerEnabled{ false };
    std::atomic<int>  tuningOffset{ 0 };

    juce::CriticalSection graphCommandLock;
    std::deque<GraphCommand> pendingGraphCommands;
    std::atomic<bool> graphCommandsPending{ false };

    mutable juce::SpinLock globalParamsLock;
    RuntimeGlobalParams pendingGlobalParams;
    std::atomic<uint32_t> globalParamsRevision{ 0 };
    std::atomic<uint32_t> appliedGlobalParamsRevision{ 0 };

    int consecutiveCorruptBlocks = 0;
    int recoveryCooldownBlocks = 0;
    juce::Thread::ThreadID audioThreadID = {};

    std::atomic<float> lastInputPeak{ 0.0f };
    std::atomic<float> lastOutputPeak{ 0.0f };
    std::atomic<int> silentOutputBlockCounter{ 0 };
    std::atomic<bool> silentOutputIncidentActive{ false };
    std::atomic<bool> pendingSilentOutputLog{ false };
    std::atomic<bool> pendingSilentOutputRecoveryLog{ false };
    std::atomic<bool> pendingAutoHealLog{ false };
};
