#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "Constants.h"
#include "DSP/Global/InputChain.h"
#include "DSP/Global/OutputChain.h"
#include "DSP/Global/ChannelStrip.h"
#include "DSP/Services/TunerService.h"

class ProcessorBase;
class TempoSyncable;

class AudioEngine : public juce::Thread,
    private juce::AsyncUpdater
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
        float hostTempoBpm = 120.0f;
        bool hostTempoValid = false;
        bool hostTransportPlaying = false;

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

    enum class DiagnosticsMode
    {
        Production = 0,   // meters decimated, no deep timing per block
        Light = 1,        // input/output meters more often
        Full = 2          // deep telemetry/profiling, not for live production
    };

    struct ProfilingResult
    {
        int blockSize = 0;
        int processedBlocks = 0;
        double sampleRate = 0.0;
        double avgMs = 0.0;
        double peakMs = 0.0;
        double avgCpuPercent = 0.0;
        double peakCpuPercent = 0.0;
        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
        int invalidSamples = 0;
        int clippedSamples = 0;
        int dropoutBlocks = 0;
        int clickSpikeBlocks = 0;
        bool passed = false;
        juce::String notes;
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
    void movePedal(Nova::ChainID chain, int fromIndex, int toIndex);
    void clearAll();

    void setEngineEnabled(bool enabled);
    void updateMixer(float gainA, float gainB, Nova::SwitcherMode mode);

    std::vector<ChainNodeView> getNodes(Nova::ChainID chain) const;
    juce::AudioProcessor* getProcessorForPedal(Nova::ChainID chain, int index);

    double getCpuLoad() const;
    double getLastProcessTimeMs() const;
    double getAverageProcessTimeMs() const;
    double getPeakProcessTimeMs() const;
    int getLatencyNumSamples() const;
    float getLastInputPeak() const;
    float getLastOutputPeak() const;
    int getAutoHealCount() const;
    juce::String buildDiagnosticReport() const;

    void setDiagnosticsMode(DiagnosticsMode mode);
    DiagnosticsMode getDiagnosticsMode() const;

    std::vector<ProfilingResult> runRealtimeProfilingSuite(int secondsPerBlockSize = 3);
    static juce::String formatProfilingResults(const std::vector<ProfilingResult>& results);

    void setTunerEnabled(bool shouldEnable);

    bool isTunerEnabled() const;
    bool getTunerEnabled() const;

    float getTunerPitch() const;
    float getTunerClarity() const;
    float getTunerRMS() const;
    void  setTunerReferencePitch(float hz);
    float getTunerReferencePitch() const;

    void setPedalBypassed(Nova::ChainID chain, int index, bool bypassed);
    void synchronizeProcessingState();

    void run() override;

    void setTuningOffset(int semitones);
    int  getTuningOffset() const;

    void updateGlobalParams(const RuntimeGlobalParams& snapshot);
    void updateGlobalParams(const juce::ValueTree& settings,
        const juce::ValueTree& lineA,
        const juce::ValueTree& lineB);

private:
    struct ChainNodeSpec
    {
        juce::String pedalType;
        juce::String pedalID;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
        bool bypassed = false;
    };

    struct ChainRuntimeSlot
    {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::String pedalType;
        juce::String pedalID;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
        bool bypassed = false;

        juce::AudioProcessor* processor = nullptr;
        ProcessorBase* baseProcessor = nullptr;
        TempoSyncable* tempoSyncProcessor = nullptr;
    };

    enum class GraphCommandType
    {
        AddPedal,
        RemovePedal,
        MovePedal,
        ClearAll,
        SetPedalBypass,
        SetEngineEnabled,
        RebuildGraph
    };

    struct GraphCommand
    {
        GraphCommandType type = GraphCommandType::ClearAll;
        juce::String pedalType;
        juce::String pedalID;
        Nova::ChainID chain = Nova::ChainID::LineA;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
        int index = -1;
        int toIndex = -1;
        bool flag = false;
    };

    struct RuntimeParameterAtomics
    {
        std::atomic<float> inputGainDb{ 0.0f };
        std::atomic<float> gateThresholdDb{ -100.0f };
        std::atomic<bool> forceMono{ false };
        std::atomic<float> hostTempoBpm{ 120.0f };
        std::atomic<bool> hostTempoValid{ false };
        std::atomic<bool> hostTransportPlaying{ false };

        std::atomic<float> outputVolumeDb{ 0.0f };
        std::atomic<float> outputLimiterDb{ 0.0f };
        std::atomic<float> outputMixNormalized{ 1.0f };

        std::atomic<int> switchMode{ (int)Nova::SwitcherMode::LineA_Only };

        std::atomic<float> gainA{ 1.0f };
        std::atomic<float> panA{ 0.0f };
        std::atomic<float> widthA{ 1.0f };

        std::atomic<float> gainB{ 1.0f };
        std::atomic<float> panB{ 0.0f };
        std::atomic<float> widthB{ 1.0f };

        std::atomic<uint32_t> revision{ 0 };

        void store(const RuntimeGlobalParams& snapshot) noexcept;
        RuntimeGlobalParams load() const noexcept;
    };

    struct SampleAccurateRamp
    {
        double sampleRate = 44100.0;
        float current = 1.0f;
        float target = 1.0f;
        float step = 0.0f;
        int samplesRemaining = 0;
        int defaultRampSamples = 64;

        void prepare(double newSampleRate, double seconds) noexcept;
        void reset(float value) noexcept;
        void setTarget(float value) noexcept;
        float getNext() noexcept;
        float getCurrent() const noexcept { return current; }
        bool isSmoothing() const noexcept { return samplesRemaining > 0; }
    };

    struct GraphRuntime
    {
        std::unique_ptr<juce::AudioProcessorGraph> graph;

        juce::AudioProcessorGraph::Node::Ptr inputNode;
        juce::AudioProcessorGraph::Node::Ptr outputNode;
        juce::AudioProcessorGraph::Node::Ptr inputChainNode;
        juce::AudioProcessorGraph::Node::Ptr stripNodeA;
        juce::AudioProcessorGraph::Node::Ptr stripNodeB;
        juce::AudioProcessorGraph::Node::Ptr outputChainNode;

        InputChainProcessor* inputChain = nullptr;
        ChannelStripProcessor* stripA = nullptr;
        ChannelStripProcessor* stripB = nullptr;
        OutputChainProcessor* outputChain = nullptr;

        std::vector<ChainRuntimeSlot> chainA;
        std::vector<ChainRuntimeSlot> chainB;

        double sampleRate = 44100.0;
        int blockSize = 512;
        int numInputs = 2;
        int numOutputs = 2;
        int latencySamples = 0;
        uint64_t generation = 0;
        uint32_t appliedParamRevision = 0;
    };

    struct RetiredGraph
    {
        std::shared_ptr<GraphRuntime> graph;
        uint64_t releaseAfterAudioBlock = 0;
    };

    struct ControlPlane
    {
        juce::CriticalSection commandLock;
        std::deque<GraphCommand> pendingCommands;
        std::atomic<bool> commandsPending{ false };

        mutable juce::CriticalSection modelLock;
        std::vector<ChainNodeSpec> modelChainA;
        std::vector<ChainNodeSpec> modelChainB;

        mutable juce::CriticalSection activeOwnerLock;
        std::shared_ptr<GraphRuntime> activeOwner;
        std::vector<RetiredGraph> retiredGraphs;
        uint64_t nextGeneration = 1;
    };

    struct AudioPlane
    {
        std::atomic<GraphRuntime*> activeGraphRaw{ nullptr };
        std::atomic<uint64_t> audioBlockCounter{ 0 };
        std::atomic<bool> isEngineOn{ false };
        std::atomic<bool> tunerEnabled{ false };
        std::atomic<int> tuningOffset{ 0 };
        std::atomic<double> cpuUsage{ 0.0 };
        std::atomic<double> lastProcessTimeMs{ 0.0 };
        std::atomic<double> averageProcessTimeMs{ 0.0 };
        std::atomic<double> peakProcessTimeMs{ 0.0 };

        double currentSampleRate = 44100.0;
        int currentBlockSize = 512;
        int numInputChannels = 2;
        int numOutputChannels = 2;
        int scratchBlockCapacity = 0;
        int scratchChannelCapacity = 0;

        juce::Thread::ThreadID audioThreadID = {};

        juce::AudioBuffer<float> dryScratch;
        juce::AudioBuffer<float> delayedDryScratch;
        juce::MidiBuffer emptyMidi;

        std::vector<std::vector<float>> dryDelay;
        int dryDelayWriteIndex = 0;
        int dryDelayBufferSize = 0;
        std::atomic<int> currentDryLatencySamples{ 0 };
        std::atomic<bool> audioRuntimeResetRequested{ false };

        SampleAccurateRamp wetMixRamp;

        TunerService tunerService;

        std::atomic<float> lastInputPeak{ 0.0f };
        std::atomic<float> lastOutputPeak{ 0.0f };
        std::atomic<float> lastInputRms{ 0.0f };
        std::atomic<float> lastOutputRms{ 0.0f };
        std::atomic<float> lastInputDcAbs{ 0.0f };
        std::atomic<float> lastOutputDcAbs{ 0.0f };
        std::atomic<float> lastInputSampleDeltaPeak{ 0.0f };
        std::atomic<float> lastOutputSampleDeltaPeak{ 0.0f };
        std::atomic<int> autoHealCount{ 0 };

        std::atomic<bool> pendingSilentOutputLog{ false };
        std::atomic<bool> pendingSilentOutputRecoveryLog{ false };
        std::atomic<bool> pendingAutoHealLog{ false };
        std::atomic<bool> graphResetRequested{ false };
        std::atomic<int> silentOutputBlockCounter{ 0 };
        std::atomic<bool> silentOutputIncidentActive{ false };

        int consecutiveCorruptBlocks = 0;
        int recoveryCooldownBlocks = 0;
        int startupCounter = 0;
        uint32_t meterDecimator = 0;
    };

    struct BlockHealthStats
    {
        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
        int invalidSamples = 0;
        int clippedSamples = 0;
        int nearClipSamples = 0;
        int clickSpikeSamples = 0;
        bool hadCorruption = false;
    };

    void enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe);
    void flushPendingGraphCommands();
    bool applyGraphCommandToModel(const GraphCommand& cmd, bool& topologyChanged, bool& paramsChangedOnly);

    std::shared_ptr<GraphRuntime> buildGraphFromModelLocked(uint64_t generation);
    void connectRuntimeChain(GraphRuntime& runtime,
        const std::vector<ChainRuntimeSlot>& chain,
        juce::AudioProcessorGraph::NodeID targetStripID);
    void publishGraph(std::shared_ptr<GraphRuntime> newGraph);
    void cleanupRetiredGraphs();
    void requestControlGraphRebuild();

    void applyRuntimeParamsToGraph(GraphRuntime& runtime, const RuntimeGlobalParams& snapshot, uint32_t revision);
    void applyPedalBypassToActiveGraph(Nova::ChainID chain, int index, bool bypassed);
    RuntimeGlobalParams loadRuntimeParams() const noexcept;

    void prepareScratchBuffers(double sampleRate, int samplesPerBlock, int numChannels);
    void resetAudioRuntimeState(bool resetMeters);
    void resetRuntimeGraph(GraphRuntime& runtime);
    void resetDryDelayLine();
    void updateDryDelayLatency(int latencySamples) noexcept;

    void processWithSampleAccurateDryWet(GraphRuntime& runtime,
        juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midi,
        BlockHealthStats& health);
    void mixWetDrySampleAccurate(juce::AudioBuffer<float>& wetBuffer,
        const juce::AudioBuffer<float>& dryBuffer,
        int numChannels,
        int numSamples);
    void copyDryThroughLatency(const juce::AudioBuffer<float>& dryIn,
        juce::AudioBuffer<float>& dryOut,
        int numChannels,
        int numSamples) noexcept;

    float updateInputMeterLight(const juce::AudioBuffer<float>& buffer) noexcept;
    BlockHealthStats sanitizeAndMeterOutput(juce::AudioBuffer<float>& buffer,
        float inputPeak,
        bool deepScan) noexcept;
    void handleHealthAfterBlock(const BlockHealthStats& health,
        int numSamples,
        bool engineActuallyProcessed) noexcept;
    void updateRealtimeTimingMeters(double elapsedMs, int numSamples) noexcept;

    void handleAsyncUpdate() override;

    juce::String describeModelChain(const std::vector<ChainNodeSpec>& chain) const;
    juce::String describeRuntimeChain(const std::vector<ChainRuntimeSlot>& chain) const;

    float calculateFrequency(const float* signal, int numSamples, double sampleRate);
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate);

private:
    RuntimeParameterAtomics params;
    ControlPlane controlPlane;
    AudioPlane audioPlane;
    std::atomic<int> diagnosticsMode{ (int)DiagnosticsMode::Production };
};
