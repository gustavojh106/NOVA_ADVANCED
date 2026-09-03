#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Constants.h"
#include "Audio/AudioEngineCommandQueue.h"
#include "Audio/CpuMeter.h"
#include "Audio/DiagnosticsManager.h"
#include "Audio/DryWetMixer.h"
#include "Audio/GraphCommandApplier.h"
#include "Audio/GraphRetirementQueue.h"
#include "Audio/GraphRuntimeTypes.h"
#include "Audio/HealthMonitor.h"
#include "Audio/RuntimeParameterSnapshot.h"
#include "Audio/RuntimeGraphManager.h"
#include "DSP/Global/InputChain.h"
#include "DSP/Global/OutputChain.h"
#include "DSP/Global/ChannelStrip.h"
#include "DSP/Services/TunerService.h"

class ProcessorBase;
struct TempoSyncable;

class AudioEngine : public juce::Thread,
    private juce::AsyncUpdater
{
public:
    class LatencyListener
    {
    public:
        virtual ~LatencyListener() = default;
        virtual void audioEngineLatencyChanged(int latencySamples) = 0;
    };

    struct ChainNodeView
    {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::String pedalID;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
    };

    using RuntimeGlobalParams = Nova::Audio::RuntimeGlobalParamsSnapshot;

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
    void setLatencyListener(LatencyListener* listener) noexcept;

    std::vector<ChainNodeView> getNodes(Nova::ChainID chain) const;
    juce::AudioProcessor* getProcessorForPedal(Nova::ChainID chain, int index);

    double getCpuLoad() const;
    double getLastProcessTimeMs() const;
    double getAverageProcessTimeMs() const;
    double getPeakProcessTimeMs() const;
    int getLatencyNumSamples() const;
    // Offline diagnostics only; do not poll this from UI/control code while audio is running.
    OutputChainProcessor::DebugSnapshot getOutputChainDebugSnapshot() const;
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
    using ChainNodeSpec = Nova::Audio::GraphChainNodeSpec;
    using ChainRuntimeSlot = Nova::Audio::GraphRuntimeSlot;
    using GraphRuntime = Nova::Audio::GraphRuntime;

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

    struct GraphCommandTraits
    {
        static bool usesLineA(const GraphCommand& command) noexcept { return command.chain == Nova::ChainID::LineA; }
        static bool isAddPedal(const GraphCommand& command) noexcept { return command.type == GraphCommandType::AddPedal; }
        static bool isRemovePedal(const GraphCommand& command) noexcept { return command.type == GraphCommandType::RemovePedal; }
        static bool isMovePedal(const GraphCommand& command) noexcept { return command.type == GraphCommandType::MovePedal; }
        static bool isClearAll(const GraphCommand& command) noexcept { return command.type == GraphCommandType::ClearAll; }
        static bool isSetPedalBypass(const GraphCommand& command) noexcept { return command.type == GraphCommandType::SetPedalBypass; }
        static bool isSetEngineEnabled(const GraphCommand& command) noexcept { return command.type == GraphCommandType::SetEngineEnabled; }
        static bool isRebuildGraph(const GraphCommand& command) noexcept { return command.type == GraphCommandType::RebuildGraph; }
        static int index(const GraphCommand& command) noexcept { return command.index; }
        static int toIndex(const GraphCommand& command) noexcept { return command.toIndex; }
        static bool flag(const GraphCommand& command) noexcept { return command.flag; }

        static ChainNodeSpec makeNode(const GraphCommand& command)
        {
            ChainNodeSpec spec;
            spec.pedalType = command.pedalType;
            spec.pedalID = command.pedalID;
            spec.zone = command.zone;
            spec.bypassed = false;
            return spec;
        }
    };

    using ModelCommandApplier = Nova::Audio::GraphCommandApplier<GraphCommand,
        ChainNodeSpec,
        std::vector<ChainNodeSpec>,
        GraphCommandTraits>;

    struct ControlPlane
    {
        Nova::Audio::AudioEngineCommandQueue<GraphCommand> commandQueue;

        mutable juce::CriticalSection modelLock;
        std::vector<ChainNodeSpec> modelChainA;
        std::vector<ChainNodeSpec> modelChainB;

        uint64_t nextGeneration = 1;
    };

    struct AudioPlane
    {
        std::atomic<uint64_t> audioBlockCounter{ 0 };
        std::atomic<bool> isEngineOn{ false };
        std::atomic<bool> tunerEnabled{ false };
        std::atomic<int> tuningOffset{ 0 };
        CpuMeter cpuMeter;

        double currentSampleRate = 44100.0;
        int currentBlockSize = 512;
        int numInputChannels = 2;
        int numOutputChannels = 2;

        juce::Thread::ThreadID audioThreadID = {};
        // True only while process() is executing on audioThreadID. Combined with the
        // thread ID this distinguishes "inside the realtime callback" from "on the
        // thread that happens to drive the audio": offline renders and the test
        // harness process audio synchronously from the message thread.
        std::atomic<bool> insideProcessCallback{ false };

        juce::MidiBuffer emptyMidi;

        std::atomic<bool> audioRuntimeResetRequested{ false };

        Nova::Audio::DryWetMixer dryWetMixer;

        TunerService tunerService;

        std::atomic<float> lastInputPeak{ 0.0f };
        std::atomic<float> lastInputRms{ 0.0f };
        std::atomic<float> lastOutputRms{ 0.0f };
        std::atomic<float> lastInputDcAbs{ 0.0f };
        std::atomic<float> lastOutputDcAbs{ 0.0f };
        std::atomic<float> lastInputSampleDeltaPeak{ 0.0f };
        std::atomic<float> lastOutputSampleDeltaPeak{ 0.0f };
        HealthMonitor healthMonitor;

        std::atomic<bool> pendingSilentOutputLog{ false };
        std::atomic<bool> pendingSilentOutputRecoveryLog{ false };
        std::atomic<bool> pendingAutoHealLog{ false };
        std::atomic<bool> graphResetRequested{ false };

        int startupCounter = 0;
        uint32_t meterDecimator = 0;
    };

    void enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe);
    void flushPendingGraphCommands();
    bool isCalledFromRealtimeCallback() const noexcept;

    std::shared_ptr<GraphRuntime> buildGraphFromModelLocked(uint64_t generation);
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
    void notifyLatencyChanged(int latencySamples);

    void processWithSampleAccurateDryWet(GraphRuntime& runtime,
        juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midi,
        HealthMonitor::BlockStats&);

    float updateInputMeterLight(const juce::AudioBuffer<float>& buffer) noexcept;
    void handleHealthAfterBlock(const HealthMonitor::BlockStats& health,
        int numSamples,
        bool engineActuallyProcessed) noexcept;
    void applyHealthActions(const HealthMonitor::Actions& actions) noexcept;

    void handleAsyncUpdate() override;

    float calculateFrequency(const float* signal, int numSamples, double sampleRate);
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate);

private:
    Nova::Audio::RuntimeParameterSnapshot params;
    Nova::Audio::RuntimeGraphManager<GraphRuntime> runtimeGraphs;
    ControlPlane controlPlane;
    AudioPlane audioPlane;
    std::atomic<LatencyListener*> latencyListener{ nullptr };
    std::atomic<int> diagnosticsMode{ (int)DiagnosticsMode::Production };
};
