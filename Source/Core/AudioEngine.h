#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>

#include "Constants.h"
#include "DSP/Global/InputChain.h"
#include "DSP/Global/OutputChain.h"
#include "DSP/Global/ChannelStrip.h"
#include "DSP/Services/TunerService.h"

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
    int getLatencyNumSamples() const;
    float getLastInputPeak() const { return audioPlane.lastInputPeak.load(); }
    float getLastOutputPeak() const { return audioPlane.lastOutputPeak.load(); }
    int getAutoHealCount() const { return audioPlane.autoHealCount.load(); }
    juce::String buildDiagnosticReport() const;

    void setTunerEnabled(bool shouldEnable);

    bool isTunerEnabled() const { return audioPlane.tunerEnabled.load(); }
    bool getTunerEnabled() const { return audioPlane.tunerEnabled.load(); }

    float getTunerPitch() const { return audioPlane.tunerService.getCurrentPitch(); }
    float getTunerClarity() const { return audioPlane.tunerService.getCurrentClarity(); }
    float getTunerRMS() const { return audioPlane.tunerService.getCurrentRMS(); }
    void  setTunerReferencePitch(float hz) { audioPlane.tunerService.setReferencePitch(hz); }
    float getTunerReferencePitch() const { return audioPlane.tunerService.getReferencePitch(); }

    void setPedalBypassed(Nova::ChainID chain, int index, bool bypassed);
    void synchronizeProcessingState();

    void run() override;

    void setTuningOffset(int semitones) { audioPlane.tuningOffset = semitones; }
    int  getTuningOffset() const { return audioPlane.tuningOffset.load(); }

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
        MovePedal,
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
        int toIndex = -1;
        bool flag = false;
    };

    struct ControlPlane
    {
        juce::CriticalSection graphCommandLock;
        std::deque<GraphCommand> pendingGraphCommands;
        std::atomic<bool> graphCommandsPending{ false };

        mutable juce::SpinLock globalParamsLock;
        RuntimeGlobalParams pendingGlobalParams;
        std::atomic<uint32_t> globalParamsRevision{ 0 };
        std::atomic<uint32_t> appliedGlobalParamsRevision{ 0 };
    };

    struct SignalBlockMetrics
    {
        int numChannels = 0;
        int numSamples = 0;
        float peak = 0.0f;
        float rms = 0.0f;
        float dcAbs = 0.0f;
        float sampleDeltaPeak = 0.0f;
        int nearClipSamples = 0;
        std::array<float, 2> channelPeak{ { 0.0f, 0.0f } };
        std::array<float, 2> channelRms{ { 0.0f, 0.0f } };
        std::array<float, 2> channelDcAbs{ { 0.0f, 0.0f } };
        std::array<float, 2> channelSampleDeltaPeak{ { 0.0f, 0.0f } };
    };

    struct SanitizerStats
    {
        bool hadCorruption = false;
        int invalidSamples = 0;
        int clippedSamples = 0;
    };

    struct ProcessTimingMetrics
    {
        double cpuMeasuredMs = 0.0;
        double inputAnalyzeMs = 0.0;
        double applyParamsMs = 0.0;
        double mixMs = 0.0;
        double graphProcessMs = 0.0;
        double graphAnalyzeMs = 0.0;
        double preSanitizeAnalyzeMs = 0.0;
        double sanitizeMs = 0.0;
        double outputAnalyzeMs = 0.0;
        double untrackedMs = 0.0;
    };

    enum class ProcessPathKind
    {
        EngineOffPassThrough,
        StartupPassThrough,
        TunerMuted,
        GraphMissingMuted,
        DryOnly,
        WetOnly,
        Blended
    };

    struct SignalWindowAccumulator
    {
        int blocks = 0;
        int totalSamples = 0;
        int graphSignalBlocks = 0;
        int preSanitizeBlocks = 0;
        int engineOffBlocks = 0;
        int startupPassThroughBlocks = 0;
        int tunerMutedBlocks = 0;
        int graphMissingMutedBlocks = 0;
        int dryOnlyBlocks = 0;
        int wetOnlyBlocks = 0;
        int blendedBlocks = 0;
        int inputActiveBlocks = 0;
        int outputSilentBlocks = 0;
        int suspiciousSilentBlocks = 0;
        int spikeBlocks = 0;
        int dcAlertBlocks = 0;
        int nearClipSamples = 0;
        int invalidSamples = 0;
        int clippedSamples = 0;
        float cpuPeak = 0.0f;
        float wetMixMin = 1.0f;
        float wetMixMax = 0.0f;
        std::array<float, 2> inputPeakMax{ { 0.0f, 0.0f } };
        std::array<float, 2> inputRmsMax{ { 0.0f, 0.0f } };
        std::array<float, 2> inputDcMax{ { 0.0f, 0.0f } };
        std::array<float, 2> inputDeltaMax{ { 0.0f, 0.0f } };
        std::array<float, 2> graphPeakMax{ { 0.0f, 0.0f } };
        std::array<float, 2> graphRmsMax{ { 0.0f, 0.0f } };
        std::array<float, 2> graphDcMax{ { 0.0f, 0.0f } };
        std::array<float, 2> graphDeltaMax{ { 0.0f, 0.0f } };
        std::array<float, 2> preSanitizePeakMax{ { 0.0f, 0.0f } };
        std::array<float, 2> preSanitizeRmsMax{ { 0.0f, 0.0f } };
        std::array<float, 2> preSanitizeDcMax{ { 0.0f, 0.0f } };
        std::array<float, 2> preSanitizeDeltaMax{ { 0.0f, 0.0f } };
        std::array<float, 2> outputPeakMax{ { 0.0f, 0.0f } };
        std::array<float, 2> outputRmsMax{ { 0.0f, 0.0f } };
        std::array<float, 2> outputDcMax{ { 0.0f, 0.0f } };
        std::array<float, 2> outputDeltaMax{ { 0.0f, 0.0f } };
        std::array<double, 2> inputRmsSum{ { 0.0, 0.0 } };
        std::array<double, 2> graphRmsSum{ { 0.0, 0.0 } };
        std::array<double, 2> preSanitizeRmsSum{ { 0.0, 0.0 } };
        std::array<double, 2> outputRmsSum{ { 0.0, 0.0 } };
        double cpuMeasuredMsSum = 0.0;
        double inputAnalyzeMsSum = 0.0;
        double applyParamsMsSum = 0.0;
        double mixMsSum = 0.0;
        double graphProcessMsSum = 0.0;
        double graphAnalyzeMsSum = 0.0;
        double preSanitizeAnalyzeMsSum = 0.0;
        double sanitizeMsSum = 0.0;
        double outputAnalyzeMsSum = 0.0;
        double untrackedMsSum = 0.0;
        float cpuMeasuredMsPeak = 0.0f;
        float inputAnalyzeMsPeak = 0.0f;
        float applyParamsMsPeak = 0.0f;
        float mixMsPeak = 0.0f;
        float graphProcessMsPeak = 0.0f;
        float graphAnalyzeMsPeak = 0.0f;
        float preSanitizeAnalyzeMsPeak = 0.0f;
        float sanitizeMsPeak = 0.0f;
        float outputAnalyzeMsPeak = 0.0f;
        float untrackedMsPeak = 0.0f;

        void reset() noexcept
        {
            *this = {};
            wetMixMin = 1.0f;
        }

        bool hasData() const noexcept
        {
            return blocks > 0;
        }
    };

    struct AudioPlane
    {
        double currentSampleRate = 44100.0;
        int currentBlockSize = 512;
        int numInputChannels = 2;
        int numOutputChannels = 2;

        std::atomic<bool> isEngineOn{ false };
        std::atomic<double> cpuUsage{ 0.0 };

        double currentRate = 0.0;
        int startupCounter = 0;

        std::atomic<float> currentGlobalMix{ 1.0f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMixSmooth;
        juce::dsp::DryWetMixer<float> dryWetMixer{ Nova::Config::MAX_GRAPH_LATENCY_SAMPLES };

        TunerService tunerService;
        std::atomic<bool> tunerEnabled{ false };
        std::atomic<int> tuningOffset{ 0 };

        int consecutiveCorruptBlocks = 0;
        int recoveryCooldownBlocks = 0;
        juce::Thread::ThreadID audioThreadID = {};

        std::atomic<float> lastInputPeak{ 0.0f };
        std::atomic<float> lastOutputPeak{ 0.0f };
        std::atomic<float> lastInputRms{ 0.0f };
        std::atomic<float> lastOutputRms{ 0.0f };
        std::atomic<float> lastInputDcAbs{ 0.0f };
        std::atomic<float> lastOutputDcAbs{ 0.0f };
        std::atomic<float> lastInputSampleDeltaPeak{ 0.0f };
        std::atomic<float> lastOutputSampleDeltaPeak{ 0.0f };
        std::atomic<int> silentOutputBlockCounter{ 0 };
        std::atomic<bool> silentOutputIncidentActive{ false };
        std::atomic<bool> pendingSilentOutputLog{ false };
        std::atomic<bool> pendingSilentOutputRecoveryLog{ false };
        std::atomic<bool> pendingAutoHealLog{ false };
        std::atomic<int> autoHealCount{ 0 };
        bool mixPathBypassed = true;

        juce::SpinLock signalTelemetryLock;
        SignalWindowAccumulator signalTelemetryWindow;
        uint32_t signalTelemetryWindowStartMs = 0;
        uint32_t lastSignalAlertMs = 0;
        std::array<float, 2> previousInputSamples{ { 0.0f, 0.0f } };
        std::array<float, 2> previousGraphSamples{ { 0.0f, 0.0f } };
        std::array<float, 2> previousPreSanitizeSamples{ { 0.0f, 0.0f } };
        std::array<float, 2> previousOutputSamples{ { 0.0f, 0.0f } };
        bool hasPreviousInputSamples = false;
        bool hasPreviousGraphSamples = false;
        bool hasPreviousPreSanitizeSamples = false;
        bool hasPreviousOutputSamples = false;
    };

    void rebuildGraph();
    void connectChainToGain(const std::vector<ChainNodeSlot>& nodes,
        juce::AudioProcessorGraph::NodeID targetStripID);

    void enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe);
    void flushPendingGraphCommands(bool suspendGraph);
    void applyGraphCommandNow(const GraphCommand& cmd,
        bool& topologyChanged,
        bool& renderSequenceInvalidated,
        bool& resetRequested,
        bool& refreshRequested);
    void applyPendingGlobalParams();
    void applyGlobalParamsNow(const RuntimeGlobalParams& snapshot);
    void propagateTempoContextToProcessors(const RuntimeGlobalParams& snapshot);
    void refreshSignalPathNow();
    void resetGraphStateNow();
    void resetSignalTelemetryStateNow();
    void accumulateSignalTelemetry(const SignalBlockMetrics& inputMetrics,
        const SignalBlockMetrics* graphMetrics,
        const SignalBlockMetrics* preSanitizeMetrics,
        const SignalBlockMetrics& outputMetrics,
        const SanitizerStats& sanitizerStats,
        ProcessPathKind path,
        float wetMix,
        double blockCpuPercent,
        const ProcessTimingMetrics& timingMetrics,
        bool suspiciousSilence);
    void emitSignalTelemetryIfNeeded();
    juce::String buildSignalTelemetryReport(const SignalWindowAccumulator& window, uint32_t elapsedMs) const;
    SignalBlockMetrics analyzeBuffer(const juce::AudioBuffer<float>& buffer,
        std::array<float, 2>& previousSamples,
        bool& hasPreviousSamples) const;
    SanitizerStats sanitizeAudioBuffer(juce::AudioBuffer<float>& buffer);
    void updateDryWetLatencyCompensation();
    juce::String describeChainState(const std::vector<ChainNodeSlot>& chain) const;

    float calculateFrequency(const float* signal, int numSamples, double sampleRate);
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate);
    void handleAsyncUpdate() override;

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

    ControlPlane controlPlane;
    AudioPlane audioPlane;
};
