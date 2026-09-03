#include "AudioEngine.h"
#include "Audio/DryWetMixer.h"
#include "Audio/GraphBuilder.h"
#include "SessionLogger.h"
#include "../Effects/Pedals/Base/ProcessorBase.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    constexpr float kDefaultHardLimit = 4.0f;
    constexpr int kProductionMeterDecimation = 16;
    constexpr int kLightMeterDecimation = 4;
    constexpr int kFullMeterDecimation = 1;
    constexpr int kMinimumScratchBlocks = 8192;
    constexpr double kDefaultMixRampSeconds = 0.008; // 8 ms: fast enough for knobs, slow enough to avoid clicks.

    juce::String boolToText(bool value)
    {
        return value ? "true" : "false";
    }

    float safeHardLimit() noexcept
    {
#if defined(Nova_Config_HARD_ABS_LIMIT_LINEAR)
        return Nova::Config::HARD_ABS_LIMIT_LINEAR;
#else
        return kDefaultHardLimit;
#endif
    }

    // Marks the realtime callback as active for the duration of AudioEngine::process().
    struct ScopedProcessCallbackFlag
    {
        explicit ScopedProcessCallbackFlag(std::atomic<bool>& flagToSet) noexcept : flag(flagToSet)
        {
            flag.store(true, std::memory_order_relaxed);
        }

        ~ScopedProcessCallbackFlag() noexcept
        {
            flag.store(false, std::memory_order_relaxed);
        }

        std::atomic<bool>& flag;
    };

}

// ========================================================== 
// LIFECYCLE
// ========================================================== 

AudioEngine::AudioEngine()
    : juce::Thread("AudioEngineControlThread")
{
    params.store(RuntimeGlobalParams{});
    audioPlane.isEngineOn.store(false, std::memory_order_relaxed);
    NovaDiagnostics::SessionLogger::logEvent("engine.lifecycle", "AudioEngine constructed with immutable graph swapping");
    startThread(juce::Thread::Priority::low);
}

AudioEngine::~AudioEngine()
{
    NovaDiagnostics::SessionLogger::logEvent("engine.lifecycle", "AudioEngine shutting down");
    cancelPendingUpdate();
    stopThread(4000);

    runtimeGraphs.clear();
}

void AudioEngine::prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int block = juce::jmax(1, samplesPerBlock);
    const int inputs = juce::jmax(1, numIn);
    const int outputs = juce::jmax(1, numOut);

    audioPlane.currentSampleRate = sr;
    audioPlane.currentBlockSize = block;
    audioPlane.numInputChannels = inputs;
    audioPlane.numOutputChannels = outputs;
    audioPlane.audioThreadID = {};
    audioPlane.audioBlockCounter.store(0, std::memory_order_relaxed);
    audioPlane.startupCounter = Nova::Config::STARTUP_COUNTER_INIT;
    audioPlane.tunerService.setSampleRate(sr);
    audioPlane.tunerService.reset();

    prepareScratchBuffers(sr, block, outputs);
    resetAudioRuntimeState(true);

    std::shared_ptr<GraphRuntime> newGraph;
    {
        const juce::ScopedLock modelLock(controlPlane.modelLock);
        newGraph = buildGraphFromModelLocked(controlPlane.nextGeneration++);
    }

    publishGraph(std::move(newGraph));

    NovaDiagnostics::SessionLogger::logEvent("engine.prepare",
        "Prepared immutable-swap engine: sampleRate=" + juce::String(sr)
        + ", blockSize=" + juce::String(block)
        + ", inputs=" + juce::String(inputs)
        + ", outputs=" + juce::String(outputs)
        + juce::newLine + buildDiagnosticReport());
}

// ========================================================== 
// GRAPH BUILDING / SWAPPING
// ========================================================== 

std::shared_ptr<AudioEngine::GraphRuntime> AudioEngine::buildGraphFromModelLocked(uint64_t generation)
{
    Nova::Audio::GraphBuildRequest request;
    request.sampleRate = audioPlane.currentSampleRate;
    request.blockSize = audioPlane.currentBlockSize;
    request.numInputs = audioPlane.numInputChannels;
    request.numOutputs = audioPlane.numOutputChannels;
    request.generation = generation;
    request.runtimeParams = params.load();
    request.runtimeParamRevision = params.getRevision();
    request.modelChainA = controlPlane.modelChainA;
    request.modelChainB = controlPlane.modelChainB;

    Nova::Audio::GraphBuilder builder;
    return builder.build(request).runtime;
}

void AudioEngine::publishGraph(std::shared_ptr<GraphRuntime> newGraph)
{
    if (newGraph == nullptr)
        return;

    const int latencySamples = newGraph->latencySamples;
    const uint64_t nowBlock = audioPlane.audioBlockCounter.load(std::memory_order_acquire);
    runtimeGraphs.publish(std::move(newGraph), nowBlock,
        [this](int latencySamples) noexcept
        {
            updateDryDelayLatency(latencySamples);
        });
    notifyLatencyChanged(latencySamples);

    cleanupRetiredGraphs();
}

void AudioEngine::cleanupRetiredGraphs()
{
    const uint64_t nowBlock = audioPlane.audioBlockCounter.load(std::memory_order_acquire);
    runtimeGraphs.cleanupRetired(nowBlock);
}

void AudioEngine::requestControlGraphRebuild()
{
    GraphCommand cmd;
    cmd.type = GraphCommandType::RebuildGraph;
    enqueueGraphCommand(cmd, true);
}

// ========================================================== 
// PARAMETERS
// ========================================================== 

void AudioEngine::updateGlobalParams(const juce::ValueTree& settings,
    const juce::ValueTree& lineA,
    const juce::ValueTree& lineB)
{
    RuntimeGlobalParams snapshot;

    if (settings.isValid())
    {
        snapshot.inputGainDb = (float)settings.getProperty(Nova::IDs::INPUT_GAIN, 0.0f);
        snapshot.gateThresholdDb = (float)settings.getProperty(Nova::IDs::INPUT_GATE, -100.0f);
        snapshot.forceMono = (bool)settings.getProperty(Nova::IDs::FORCE_MONO, false);

        snapshot.outputVolumeDb = (float)settings.getProperty(Nova::IDs::OUTPUT_VOL, 0.0f);
        snapshot.outputLimiterDb = (float)settings.getProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f);
        snapshot.outputMixRaw = (float)settings.getProperty(Nova::IDs::OUTPUT_MIX, 100.0f);

        snapshot.switchMode = (int)settings.getProperty(Nova::IDs::SWITCH_MODE,
            (int)Nova::SwitcherMode::LineA_Only);
    }

    if (lineA.isValid())
    {
        snapshot.gainA = (float)lineA.getProperty(Nova::IDs::MIXER_GAIN_A, 1.0f);
        snapshot.panA = (float)lineA.getProperty(Nova::IDs::MIXER_PAN_A, 0.0f);
        snapshot.widthA = (float)lineA.getProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f);
    }

    if (lineB.isValid())
    {
        snapshot.gainB = (float)lineB.getProperty(Nova::IDs::MIXER_GAIN_B, 1.0f);
        snapshot.panB = (float)lineB.getProperty(Nova::IDs::MIXER_PAN_B, 0.0f);
        snapshot.widthB = (float)lineB.getProperty(Nova::IDs::MIXER_WIDTH_B, 1.0f);
    }

    updateGlobalParams(snapshot);
}

void AudioEngine::updateGlobalParams(const RuntimeGlobalParams& snapshot)
{
    params.store(snapshot);

    // Parameter application to processors is control-thread work. The audio callback only reads atomics
    // and uses the sample-accurate wet/dry ramp.
    if (!isCalledFromRealtimeCallback())
        triggerAsyncUpdate();
}

AudioEngine::RuntimeGlobalParams AudioEngine::loadRuntimeParams() const noexcept
{
    return params.load();
}

void AudioEngine::applyRuntimeParamsToGraph(GraphRuntime& runtime, const RuntimeGlobalParams& snapshot, uint32_t revision)
{
    Nova::Audio::GraphBuilder::applyRuntimeParamsToGraph(runtime, snapshot, revision);
}

void AudioEngine::applyPedalBypassToActiveGraph(Nova::ChainID chain, int index, bool bypassed)
{
    GraphRuntime* runtime = runtimeGraphs.getActiveRaw();
    if (runtime == nullptr)
        return;

    auto& list = (chain == Nova::ChainID::LineA) ? runtime->chainA : runtime->chainB;
    if (!juce::isPositiveAndBelow(index, (int)list.size()))
        return;

    auto& slot = list[(size_t)index];
    slot.bypassed = bypassed;

    if (slot.baseProcessor != nullptr)
        slot.baseProcessor->setBypassed(bypassed);
    else if (slot.processor != nullptr)
    {
        slot.processor->suspendProcessing(bypassed);
        if (!bypassed)
            slot.processor->reset();
    }

    if (runtime->graph != nullptr)
    {
        runtime->graph->rebuild();
        runtime->latencySamples = juce::jlimit(0,
            Nova::Config::MAX_GRAPH_LATENCY_SAMPLES,
            runtime->graph->getLatencySamples());
        updateDryDelayLatency(runtime->latencySamples);
        notifyLatencyChanged(runtime->latencySamples);
    }
}

void AudioEngine::setLatencyListener(LatencyListener* listener) noexcept
{
    latencyListener.store(listener, std::memory_order_release);
}

void AudioEngine::notifyLatencyChanged(int latencySamples)
{
    if (auto* listener = latencyListener.load(std::memory_order_acquire))
        listener->audioEngineLatencyChanged(latencySamples);
}

// ========================================================== 
// CONTROL COMMANDS
// ========================================================== 

void AudioEngine::enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe)
{
    controlPlane.commandQueue.enqueue(cmd);

    if (!flushIfSafe)
        return;

    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && mm->isThisTheMessageThread())
        flushPendingGraphCommands();
    else
        triggerAsyncUpdate();
}

void AudioEngine::flushPendingGraphCommands()
{
    if (!controlPlane.commandQueue.hasPending()
        && !audioPlane.graphResetRequested.load(std::memory_order_acquire))
    {
        const auto revision = params.getRevision();
        GraphRuntime* active = runtimeGraphs.getActiveRaw();
        if (active != nullptr && active->appliedParamRevision != revision)
            applyRuntimeParamsToGraph(*active, params.load(), revision);

        cleanupRetiredGraphs();
        return;
    }

    auto commands = controlPlane.commandQueue.drain();

    bool topologyChanged = false;
    bool paramsChangedOnly = false;
    bool explicitRebuild = false;

    {
        const juce::ScopedLock modelLock(controlPlane.modelLock);

        for (const auto& cmd : commands)
        {
            const auto result = ModelCommandApplier::apply(cmd,
                controlPlane.modelChainA,
                controlPlane.modelChainB);

            if (result.bypassChanged)
                applyPedalBypassToActiveGraph(cmd.chain, cmd.index, cmd.flag);

            if (result.engineStateChanged)
            {
                const bool previous = audioPlane.isEngineOn.exchange(cmd.flag, std::memory_order_acq_rel);
                if (cmd.flag && !previous)
                {
                    audioPlane.audioRuntimeResetRequested.store(true, std::memory_order_release);
                    topologyChanged = true; // rebuild fresh graph before enabling processing
                }
            }

            topologyChanged = topologyChanged || result.topologyChanged;
            paramsChangedOnly = paramsChangedOnly || result.paramsOnly;
            explicitRebuild = explicitRebuild || result.explicitRebuild;
        }

        if (audioPlane.graphResetRequested.exchange(false, std::memory_order_acq_rel))
            explicitRebuild = true;

        if (topologyChanged || explicitRebuild)
        {
            auto rebuilt = buildGraphFromModelLocked(controlPlane.nextGeneration++);
            publishGraph(std::move(rebuilt));
            audioPlane.audioRuntimeResetRequested.store(true, std::memory_order_release);
        }
    }

    const auto revision = params.getRevision();
    GraphRuntime* active = runtimeGraphs.getActiveRaw();
    if (active != nullptr)
        applyRuntimeParamsToGraph(*active, params.load(), revision);

    if (paramsChangedOnly && !topologyChanged)
        cleanupRetiredGraphs();
}

// ========================================================== 
// PUBLIC PEDAL API
// ========================================================== 

void AudioEngine::addPedal(const juce::String& type,
    Nova::ChainID chain,
    int index,
    Nova::ZoneID zone,
    juce::String pedalID)
{
    GraphCommand cmd;
    cmd.type = GraphCommandType::AddPedal;
    cmd.pedalType = type;
    cmd.chain = chain;
    cmd.index = index;
    cmd.zone = zone;
    cmd.pedalID = std::move(pedalID);
    enqueueGraphCommand(cmd, true);
}

void AudioEngine::removePedal(Nova::ChainID chain, int index)
{
    GraphCommand cmd;
    cmd.type = GraphCommandType::RemovePedal;
    cmd.chain = chain;
    cmd.index = index;
    enqueueGraphCommand(cmd, true);
}

void AudioEngine::movePedal(Nova::ChainID chain, int fromIndex, int toIndex)
{
    GraphCommand cmd;
    cmd.type = GraphCommandType::MovePedal;
    cmd.chain = chain;
    cmd.index = fromIndex;
    cmd.toIndex = toIndex;
    enqueueGraphCommand(cmd, true);
}

void AudioEngine::clearAll()
{
    GraphCommand cmd;
    cmd.type = GraphCommandType::ClearAll;
    enqueueGraphCommand(cmd, true);
}

void AudioEngine::setPedalBypassed(Nova::ChainID chain, int index, bool bypassed)
{
    GraphCommand cmd;
    cmd.type = GraphCommandType::SetPedalBypass;
    cmd.chain = chain;
    cmd.index = index;
    cmd.flag = bypassed;
    enqueueGraphCommand(cmd, true);
}

void AudioEngine::setEngineEnabled(bool enabled)
{
    GraphCommand cmd;
    cmd.type = GraphCommandType::SetEngineEnabled;
    cmd.flag = enabled;
    enqueueGraphCommand(cmd, true);
    NovaDiagnostics::SessionLogger::logEvent("engine.state", "Requested engineEnabled=" + boolToText(enabled));
}

void AudioEngine::updateMixer(float gainA, float gainB, Nova::SwitcherMode mode)
{
    auto snapshot = params.load();
    snapshot.gainA = gainA;
    snapshot.gainB = gainB;
    snapshot.switchMode = (int)mode;
    updateGlobalParams(snapshot);
}

bool AudioEngine::isCalledFromRealtimeCallback() const noexcept
{
    return audioPlane.insideProcessCallback.load(std::memory_order_relaxed)
        && audioPlane.audioThreadID != juce::Thread::ThreadID()
        && juce::Thread::getCurrentThreadId() == audioPlane.audioThreadID;
}

void AudioEngine::synchronizeProcessingState()
{
    // Do not ever lock or flush from the realtime callback. Outside of process() this is
    // safe even on the audio thread itself: offline renders and the test harness drive
    // audio synchronously from the message thread and rely on a deterministic flush here
    // instead of waiting for the control thread to poll.
    if (isCalledFromRealtimeCallback())
        return;

    flushPendingGraphCommands();
}

// ========================================================== 
// SCRATCH / RESET
// ========================================================== 

void AudioEngine::prepareScratchBuffers(double sampleRate, int samplesPerBlock, int numChannels)
{
    audioPlane.dryWetMixer.prepareScratch(samplesPerBlock, numChannels, kMinimumScratchBlocks);
    audioPlane.dryWetMixer.prepareDryDelay(Nova::Config::MAX_GRAPH_LATENCY_SAMPLES);
    audioPlane.dryWetMixer.prepareMix(sampleRate, kDefaultMixRampSeconds);
    audioPlane.dryWetMixer.resetMix(params.getOutputMixNormalized());
}

void AudioEngine::resetAudioRuntimeState(bool resetMeters)
{
    audioPlane.dryWetMixer.resetMix(params.getOutputMixNormalized());
    resetDryDelayLine();
    audioPlane.tunerService.reset();
    if (!resetMeters)
        audioPlane.startupCounter = juce::jmax(audioPlane.startupCounter, Nova::Config::STARTUP_COUNTER_GRAPH_CHANGE);
    audioPlane.healthMonitor.reset(resetMeters);

    if (resetMeters)
    {
        audioPlane.cpuMeter.reset();
        audioPlane.lastInputPeak.store(0.0f, std::memory_order_relaxed);
        audioPlane.lastInputRms.store(0.0f, std::memory_order_relaxed);
        audioPlane.lastOutputRms.store(0.0f, std::memory_order_relaxed);
        audioPlane.lastInputDcAbs.store(0.0f, std::memory_order_relaxed);
        audioPlane.lastOutputDcAbs.store(0.0f, std::memory_order_relaxed);
        audioPlane.lastInputSampleDeltaPeak.store(0.0f, std::memory_order_relaxed);
        audioPlane.lastOutputSampleDeltaPeak.store(0.0f, std::memory_order_relaxed);
    }
}

void AudioEngine::resetRuntimeGraph(GraphRuntime& runtime)
{
    auto resetNode = [](juce::AudioProcessorGraph::Node::Ptr& node)
        {
            if (node != nullptr && node->getProcessor() != nullptr)
                node->getProcessor()->reset();
        };

    resetNode(runtime.inputChainNode);
    resetNode(runtime.stripNodeA);
    resetNode(runtime.stripNodeB);
    resetNode(runtime.outputChainNode);

    for (auto& n : runtime.chainA)
        resetNode(n.node);
    for (auto& n : runtime.chainB)
        resetNode(n.node);
}

void AudioEngine::resetDryDelayLine()
{
    audioPlane.dryWetMixer.resetDryDelayLine();
}

void AudioEngine::updateDryDelayLatency(int latencySamples) noexcept
{
    audioPlane.dryWetMixer.setLatencySamples(latencySamples);
}

// ========================================================== 
// PROCESSING
// ========================================================== 

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (audioPlane.audioThreadID == juce::Thread::ThreadID())
        audioPlane.audioThreadID = juce::Thread::getCurrentThreadId();

    const ScopedProcessCallbackFlag callbackScope(audioPlane.insideProcessCallback);

    audioPlane.audioBlockCounter.fetch_add(1, std::memory_order_release);

    if (audioPlane.audioRuntimeResetRequested.exchange(false, std::memory_order_acq_rel))
        resetAudioRuntimeState(false);

    const auto mode = static_cast<DiagnosticsMode>(diagnosticsMode.load(std::memory_order_relaxed));

    // Lightweight realtime timing meter.
    // This replaces the old heavy per-block diagnostic timing, but keeps CPU/process-time
    // visible in the UI in every diagnostics mode. It only reads the high-resolution timer
    // at block entry/exit and updates atomics; no strings, no locks, no allocation.
    const double timingStartMs = audioPlane.cpuMeter.beginBlock();
    const auto commitTiming = [this, timingStartMs, &buffer]() noexcept
        {
            audioPlane.cpuMeter.endBlock(timingStartMs,
                buffer.getNumSamples(),
                audioPlane.currentSampleRate);
        };

    const float mixTarget = params.getOutputMixNormalized();
    audioPlane.dryWetMixer.setTargetMix(mixTarget);

    const float inputPeak = updateInputMeterLight(buffer);

    if (!audioPlane.isEngineOn.load(std::memory_order_acquire))
    {
        const auto health = audioPlane.healthMonitor.sanitizeAndMeterOutput(buffer, inputPeak, mode != DiagnosticsMode::Production);
        handleHealthAfterBlock(health, buffer.getNumSamples(), false);
        commitTiming();
        return;
    }

    if (audioPlane.startupCounter > 0)
    {
        --audioPlane.startupCounter;
        const auto health = audioPlane.healthMonitor.sanitizeAndMeterOutput(buffer, inputPeak, mode != DiagnosticsMode::Production);
        handleHealthAfterBlock(health, buffer.getNumSamples(), false);
        commitTiming();
        return;
    }

    if (audioPlane.tunerEnabled.load(std::memory_order_acquire))
    {
        audioPlane.tunerService.pushBuffer(buffer);
        buffer.clear();
        const auto health = audioPlane.healthMonitor.sanitizeAndMeterOutput(buffer, inputPeak, false);
        handleHealthAfterBlock(health, buffer.getNumSamples(), false);
        commitTiming();
        return;
    }

    GraphRuntime* runtime = runtimeGraphs.getActiveRaw();
    if (runtime == nullptr || runtime->graph == nullptr)
    {
        buffer.clear();
        const auto health = audioPlane.healthMonitor.sanitizeAndMeterOutput(buffer, inputPeak, true);
        handleHealthAfterBlock(health, buffer.getNumSamples(), false);
        commitTiming();
        return;
    }

    HealthMonitor::BlockStats health;
    health.inputPeak = inputPeak;
    processWithSampleAccurateDryWet(*runtime, buffer, midi, health);

    const auto outputHealth = audioPlane.healthMonitor.sanitizeAndMeterOutput(buffer, inputPeak, mode != DiagnosticsMode::Production);
    health.outputPeak = outputHealth.outputPeak;
    health.invalidSamples += outputHealth.invalidSamples;
    health.clippedSamples += outputHealth.clippedSamples;
    health.nearClipSamples += outputHealth.nearClipSamples;
    health.clickSpikeSamples += outputHealth.clickSpikeSamples;
    health.hadCorruption = health.hadCorruption || outputHealth.hadCorruption;

    handleHealthAfterBlock(health, buffer.getNumSamples(), true);
    commitTiming();
}

void AudioEngine::processWithSampleAccurateDryWet(GraphRuntime& runtime,
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi,
    HealthMonitor::BlockStats&)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = audioPlane.dryWetMixer.getProcessChannelCount(buffer.getNumChannels());

    if (numSamples <= 0 || numChannels <= 0)
        return;

    // Realtime safety: never allocate here. If a host breaks the prepared block contract,
    // process wet-only instead of allocating from the audio thread.
    if (audioPlane.dryWetMixer.shouldUseOversizedFallback(numSamples))
    {
        runtime.graph->processBlock(buffer, midi);
        return;
    }

    const auto endpoint = audioPlane.dryWetMixer.classifyEndpoint();

    if (endpoint == Nova::Audio::DryWetMixer::EndpointPath::Dry)
    {
        // Exact dry path. Still consume the ramp for consistency, but do not touch the graph.
        audioPlane.dryWetMixer.processDryEndpoint(numSamples);
        return;
    }

    audioPlane.dryWetMixer.captureDry(buffer, numChannels, numSamples);

    runtime.graph->processBlock(buffer, midi);

    if (endpoint == Nova::Audio::DryWetMixer::EndpointPath::Wet)
    {
        audioPlane.dryWetMixer.processWetEndpoint(numSamples);
        return;
    }

    audioPlane.dryWetMixer.mixCapturedDryWithWet(buffer, numChannels, numSamples);
}

float AudioEngine::updateInputMeterLight(const juce::AudioBuffer<float>& buffer) noexcept
{
    const auto mode = static_cast<DiagnosticsMode>(diagnosticsMode.load(std::memory_order_relaxed));
    const int decimation = mode == DiagnosticsMode::Full ? kFullMeterDecimation
        : mode == DiagnosticsMode::Light ? kLightMeterDecimation
        : kProductionMeterDecimation;

    if ((audioPlane.meterDecimator++ % (uint32_t)decimation) != 0)
        return audioPlane.lastInputPeak.load(std::memory_order_relaxed);

    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(ch), buffer.getNumSamples());
        peak = juce::jmax(peak, juce::jmax(std::abs(range.getStart()), std::abs(range.getEnd())));
    }

    audioPlane.lastInputPeak.store(peak, std::memory_order_relaxed);
    return peak;
}

void AudioEngine::handleHealthAfterBlock(const HealthMonitor::BlockStats& health,
    int numSamples,
    bool engineActuallyProcessed) noexcept
{
    HealthMonitor::Context context;
    context.engineOn = audioPlane.isEngineOn.load(std::memory_order_relaxed);
    context.tunerEnabled = audioPlane.tunerEnabled.load(std::memory_order_relaxed);
    context.outputMixNormalized = params.getOutputMixNormalized();
    context.sampleRate = audioPlane.currentSampleRate;

    applyHealthActions(audioPlane.healthMonitor.afterBlock(health,
        numSamples,
        engineActuallyProcessed,
        context));
}

void AudioEngine::applyHealthActions(const HealthMonitor::Actions& actions) noexcept
{
    if (actions.requestGraphReset)
        audioPlane.graphResetRequested.store(true, std::memory_order_release);

    if (actions.logAutoHeal)
        audioPlane.pendingAutoHealLog.store(true, std::memory_order_release);

    if (actions.logSilentOutput)
        audioPlane.pendingSilentOutputLog.store(true, std::memory_order_release);

    if (actions.logSilentOutputRecovery)
        audioPlane.pendingSilentOutputRecoveryLog.store(true, std::memory_order_release);
}


// ========================================================== 
// INFO / DIAGNOSTICS
// ========================================================== 

std::vector<AudioEngine::ChainNodeView> AudioEngine::getNodes(Nova::ChainID chain) const
{
    std::vector<ChainNodeView> result;

    auto runtime = runtimeGraphs.getActiveOwnerForControl();
    if (runtime == nullptr)
        return result;

    const auto& source = (chain == Nova::ChainID::LineA) ? runtime->chainA : runtime->chainB;
    result.reserve(source.size());
    for (const auto& slot : source)
        result.push_back({ slot.node, slot.pedalID, slot.zone });

    return result;
}

juce::AudioProcessor* AudioEngine::getProcessorForPedal(Nova::ChainID chain, int index)
{
    GraphRuntime* runtime = runtimeGraphs.getActiveRaw();
    if (runtime == nullptr)
        return nullptr;

    auto& list = (chain == Nova::ChainID::LineA) ? runtime->chainA : runtime->chainB;
    if (!juce::isPositiveAndBelow(index, (int)list.size()))
        return nullptr;

    return list[(size_t)index].processor;
}

double AudioEngine::getCpuLoad() const
{
    return audioPlane.cpuMeter.getCpuLoad();
}

double AudioEngine::getLastProcessTimeMs() const
{
    return audioPlane.cpuMeter.getLastProcessTimeMs();
}

double AudioEngine::getAverageProcessTimeMs() const
{
    return audioPlane.cpuMeter.getAverageProcessTimeMs();
}

double AudioEngine::getPeakProcessTimeMs() const
{
    return audioPlane.cpuMeter.getPeakProcessTimeMs();
}

int AudioEngine::getLatencyNumSamples() const
{
    return runtimeGraphs.getLatencySamples();
}

OutputChainProcessor::DebugSnapshot AudioEngine::getOutputChainDebugSnapshot() const
{
    GraphRuntime* runtime = runtimeGraphs.getActiveRaw();
    return runtime != nullptr && runtime->outputChain != nullptr
        ? runtime->outputChain->getDebugSnapshot()
        : OutputChainProcessor::DebugSnapshot{};
}

float AudioEngine::getLastInputPeak() const
{
    return audioPlane.lastInputPeak.load(std::memory_order_relaxed);
}

float AudioEngine::getLastOutputPeak() const
{
    return audioPlane.healthMonitor.getLastOutputPeak();
}

int AudioEngine::getAutoHealCount() const
{
    return audioPlane.healthMonitor.getAutoHealCount();
}

void AudioEngine::setDiagnosticsMode(DiagnosticsMode mode)
{
    diagnosticsMode.store((int)mode, std::memory_order_release);
}

AudioEngine::DiagnosticsMode AudioEngine::getDiagnosticsMode() const
{
    return static_cast<DiagnosticsMode>(diagnosticsMode.load(std::memory_order_acquire));
}

juce::String AudioEngine::buildDiagnosticReport() const
{
    const auto snapshot = params.load();
    const GraphRuntime* runtime = runtimeGraphs.getActiveRaw();
    Nova::Audio::DiagnosticsManager::ReportData reportData;
    reportData.engineOn = audioPlane.isEngineOn.load(std::memory_order_relaxed);
    reportData.tunerEnabled = audioPlane.tunerEnabled.load(std::memory_order_relaxed);
    reportData.sampleRate = audioPlane.currentSampleRate;
    reportData.blockSize = audioPlane.currentBlockSize;
    reportData.inputChannels = audioPlane.numInputChannels;
    reportData.outputChannels = audioPlane.numOutputChannels;
    reportData.activeGraph = (runtime != nullptr);
    reportData.generation = runtime != nullptr ? runtime->generation : 0;
    reportData.graphLatencySamples = runtime != nullptr ? runtime->latencySamples : 0;
    reportData.wetMix = audioPlane.dryWetMixer.getCurrentMix();
    reportData.wetMixTarget = params.getOutputMixNormalized();
    reportData.wetMixCurrent = audioPlane.dryWetMixer.getCurrentMix();
    reportData.cpuLoad = audioPlane.cpuMeter.getCpuLoad();
    reportData.lastProcessMs = audioPlane.cpuMeter.getLastProcessTimeMs();
    reportData.avgProcessMs = audioPlane.cpuMeter.getAverageProcessTimeMs();
    reportData.peakProcessMs = audioPlane.cpuMeter.getPeakProcessTimeMs();
    reportData.autoHealCount = audioPlane.healthMonitor.getAutoHealCount();
    reportData.audioBlocks = audioPlane.audioBlockCounter.load(std::memory_order_relaxed);
    reportData.runtimeParams = snapshot;

    if (runtime != nullptr)
    {
        reportData.runtimeChainA.reserve(runtime->chainA.size());
        for (const auto& slot : runtime->chainA)
        {
            Nova::Audio::DiagnosticsManager::RuntimeChainEntry entry;
            entry.pedalType = slot.pedalType;
            entry.pedalID = slot.pedalID;
            entry.zone = slot.zone;
            entry.bypassed = slot.bypassed;
            entry.hasProcessor = (slot.processor != nullptr);
            entry.processorName = entry.hasProcessor ? slot.processor->getName() : juce::String{};
            entry.cachedBase = (slot.baseProcessor != nullptr);
            entry.cachedTempo = (slot.tempoSyncProcessor != nullptr);
            reportData.runtimeChainA.push_back(std::move(entry));
        }

        reportData.runtimeChainB.reserve(runtime->chainB.size());
        for (const auto& slot : runtime->chainB)
        {
            Nova::Audio::DiagnosticsManager::RuntimeChainEntry entry;
            entry.pedalType = slot.pedalType;
            entry.pedalID = slot.pedalID;
            entry.zone = slot.zone;
            entry.bypassed = slot.bypassed;
            entry.hasProcessor = (slot.processor != nullptr);
            entry.processorName = entry.hasProcessor ? slot.processor->getName() : juce::String{};
            entry.cachedBase = (slot.baseProcessor != nullptr);
            entry.cachedTempo = (slot.tempoSyncProcessor != nullptr);
            reportData.runtimeChainB.push_back(std::move(entry));
        }
    }

    {
        const juce::ScopedLock modelLock(controlPlane.modelLock);
        reportData.modelChainA.reserve(controlPlane.modelChainA.size());
        for (const auto& slot : controlPlane.modelChainA)
        {
            Nova::Audio::DiagnosticsManager::ModelChainEntry entry;
            entry.pedalType = slot.pedalType;
            entry.pedalID = slot.pedalID;
            entry.zone = slot.zone;
            entry.bypassed = slot.bypassed;
            reportData.modelChainA.push_back(std::move(entry));
        }

        reportData.modelChainB.reserve(controlPlane.modelChainB.size());
        for (const auto& slot : controlPlane.modelChainB)
        {
            Nova::Audio::DiagnosticsManager::ModelChainEntry entry;
            entry.pedalType = slot.pedalType;
            entry.pedalID = slot.pedalID;
            entry.zone = slot.zone;
            entry.bypassed = slot.bypassed;
            reportData.modelChainB.push_back(std::move(entry));
        }
    }

    return Nova::Audio::DiagnosticsManager::buildDiagnosticReport(reportData);
}

// ========================================================== 
// TUNER
// ========================================================== 

void AudioEngine::setTunerEnabled(bool shouldEnable)
{
    audioPlane.tunerEnabled.store(shouldEnable, std::memory_order_release);
    if (shouldEnable)
        audioPlane.tunerService.reset();
}

bool AudioEngine::isTunerEnabled() const
{
    return audioPlane.tunerEnabled.load(std::memory_order_acquire);
}

bool AudioEngine::getTunerEnabled() const
{
    return isTunerEnabled();
}

float AudioEngine::getTunerPitch() const
{
    return audioPlane.tunerService.getCurrentPitch();
}

float AudioEngine::getTunerClarity() const
{
    return audioPlane.tunerService.getCurrentClarity();
}

float AudioEngine::getTunerRMS() const
{
    return audioPlane.tunerService.getCurrentRMS();
}

void AudioEngine::setTunerReferencePitch(float hz)
{
    audioPlane.tunerService.setReferencePitch(hz);
}

float AudioEngine::getTunerReferencePitch() const
{
    return audioPlane.tunerService.getReferencePitch();
}

void AudioEngine::setTuningOffset(int semitones)
{
    audioPlane.tuningOffset.store(semitones, std::memory_order_release);
}

int AudioEngine::getTuningOffset() const
{
    return audioPlane.tuningOffset.load(std::memory_order_acquire);
}

// ========================================================== 
// CONTROL THREAD / ASYNC
// ========================================================== 

void AudioEngine::handleAsyncUpdate()
{
    flushPendingGraphCommands();
}

void AudioEngine::run()
{
    while (!threadShouldExit())
    {
        if (controlPlane.commandQueue.hasPending()
            || audioPlane.graphResetRequested.load(std::memory_order_acquire))
        {
            flushPendingGraphCommands();
        }
        else
        {
            const auto revision = params.getRevision();
            GraphRuntime* active = runtimeGraphs.getActiveRaw();
            if (active != nullptr && active->appliedParamRevision != revision)
                applyRuntimeParamsToGraph(*active, params.load(), revision);

            cleanupRetiredGraphs();
        }

        if (audioPlane.pendingSilentOutputLog.exchange(false, std::memory_order_acq_rel))
        {
            NovaDiagnostics::SessionLogger::logEvent("engine.warning",
                "Detected sustained input-active/output-near-silent condition."
                + juce::newLine + buildDiagnosticReport());
        }

        if (audioPlane.pendingSilentOutputRecoveryLog.exchange(false, std::memory_order_acq_rel))
        {
            NovaDiagnostics::SessionLogger::logEvent("engine.info",
                "Recovered from input-active/output-near-silent condition."
                + juce::newLine + buildDiagnosticReport());
        }

        if (audioPlane.pendingAutoHealLog.exchange(false, std::memory_order_acq_rel))
        {
            NovaDiagnostics::SessionLogger::logEvent("engine.autorecover",
                "Requested immutable graph rebuild after corrupt audio detection."
                + juce::newLine + buildDiagnosticReport());
        }

        if (audioPlane.tunerEnabled.load(std::memory_order_acquire))
        {
            audioPlane.tunerService.process();
            wait(5);
        }
        else
        {
            wait(20);
        }
    }
}

// ========================================================== 
// PROFILING / TESTS
// ========================================================== 

std::vector<AudioEngine::ProfilingResult> AudioEngine::runRealtimeProfilingSuite(int secondsPerBlockSize)
{
    const auto previousDiagnostics = getDiagnosticsMode();
    setDiagnosticsMode(DiagnosticsMode::Full);

    const bool previousEngineState = audioPlane.isEngineOn.load(std::memory_order_acquire);
    audioPlane.isEngineOn.store(true, std::memory_order_release);

    const double sr = audioPlane.currentSampleRate > 0.0 ? audioPlane.currentSampleRate : 44100.0;
    const std::array<int, 5> blockSizes{ { 32, 64, 128, 256, 512 } };
    std::vector<ProfilingResult> results;
    results.reserve(blockSizes.size());

    juce::Random rng(0xC0FFEE);

    for (int blockSize : blockSizes)
    {
        prepare(sr, blockSize, audioPlane.numInputChannels, audioPlane.numOutputChannels);
        audioPlane.isEngineOn.store(true, std::memory_order_release);
        synchronizeProcessingState();

        const int blocks = juce::jmax(1, (int)((sr * juce::jmax(1, secondsPerBlockSize)) / blockSize));
        juce::AudioBuffer<float> testBuffer(audioPlane.numOutputChannels, blockSize);
        juce::MidiBuffer midi;

        ProfilingResult result;
        result.blockSize = blockSize;
        result.sampleRate = sr;
        result.processedBlocks = blocks;

        double totalMs = 0.0;
        double peakMs = 0.0;
        double totalCpu = 0.0;
        double peakCpu = 0.0;
        float lastSampleL = 0.0f;

        for (int b = 0; b < blocks; ++b)
        {
            testBuffer.clear();
            const double frequency = 110.0 + 55.0 * std::sin((double)b * 0.01);
            const float gain = 0.15f;

            for (int i = 0; i < blockSize; ++i)
            {
                const double t = ((double)b * blockSize + i) / sr;
                const float sine = gain * (float)std::sin(juce::MathConstants<double>::twoPi * frequency * t);
                const float noise = (rng.nextFloat() - 0.5f) * 0.002f;
                const float sample = sine + noise;
                for (int ch = 0; ch < testBuffer.getNumChannels(); ++ch)
                    testBuffer.setSample(ch, i, sample);
            }

            const double start = juce::Time::getMillisecondCounterHiRes();
            process(testBuffer, midi);
            const double elapsed = juce::Time::getMillisecondCounterHiRes() - start;

            const double blockMs = ((double)blockSize / sr) * 1000.0;
            const double cpu = blockMs > 0.0 ? (elapsed / blockMs) * 100.0 : 0.0;
            totalMs += elapsed;
            peakMs = juce::jmax(peakMs, elapsed);
            totalCpu += cpu;
            peakCpu = juce::jmax(peakCpu, cpu);

            float outputPeak = 0.0f;
            for (int ch = 0; ch < testBuffer.getNumChannels(); ++ch)
            {
                const auto range = juce::FloatVectorOperations::findMinAndMax(testBuffer.getReadPointer(ch), blockSize);
                outputPeak = juce::jmax(outputPeak, juce::jmax(std::abs(range.getStart()), std::abs(range.getEnd())));

                for (int i = 0; i < blockSize; ++i)
                {
                    const float v = testBuffer.getSample(ch, i);
                    if (!std::isfinite(v))
                        ++result.invalidSamples;
                    if (std::abs(v) >= Nova::Config::HARD_ABS_LIMIT_LINEAR)
                        ++result.clippedSamples;
                }
            }

            result.outputPeak = juce::jmax(result.outputPeak, outputPeak);

            const float firstL = testBuffer.getNumChannels() > 0 && blockSize > 0 ? testBuffer.getSample(0, 0) : 0.0f;
            if (b > 0 && std::abs(firstL - lastSampleL) >= Nova::Config::SIGNAL_SPIKE_DELTA_THRESHOLD)
                ++result.clickSpikeBlocks;

            lastSampleL = testBuffer.getNumChannels() > 0 && blockSize > 0
                ? testBuffer.getSample(0, blockSize - 1)
                : 0.0f;

            if (cpu > 95.0)
                ++result.dropoutBlocks;
        }

        result.avgMs = totalMs / (double)blocks;
        result.peakMs = peakMs;
        result.avgCpuPercent = totalCpu / (double)blocks;
        result.peakCpuPercent = peakCpu;
        result.inputPeak = audioPlane.lastInputPeak.load(std::memory_order_relaxed);
        result.passed = result.invalidSamples == 0
            && result.clippedSamples == 0
            && result.dropoutBlocks == 0
            && result.clickSpikeBlocks == 0
            && result.peakCpuPercent < 90.0;

        if (!result.passed)
            result.notes = "Investigate invalid/clipped/dropout/click or CPU headroom.";

        results.push_back(result);
    }

    audioPlane.isEngineOn.store(previousEngineState, std::memory_order_release);
    setDiagnosticsMode(previousDiagnostics);
    return results;
}

juce::String AudioEngine::formatProfilingResults(const std::vector<ProfilingResult>& results)
{
    return Nova::Audio::DiagnosticsManager::formatProfilingResults(results);
}

#if JUCE_UNIT_TESTS
class AudioEngineRealtimeUnitTests final : public juce::UnitTest
{
public:
    AudioEngineRealtimeUnitTests()
        : juce::UnitTest("AudioEngine realtime glitch/click/dropout profiling", "NOVA") {}

    void runTest() override
    {
        beginTest("Block-size profiling: 32/64/128/256/512");
        AudioEngine engine;
        engine.prepare(48000.0, 128, 2, 2);
        engine.setEngineEnabled(true);
        engine.synchronizeProcessingState();

        const auto results = engine.runRealtimeProfilingSuite(1);
        expectEquals((int)results.size(), 5);

        for (const auto& r : results)
        {
            expect(r.invalidSamples == 0, "Invalid samples at block " + juce::String(r.blockSize));
            expect(r.clippedSamples == 0, "Clipped samples at block " + juce::String(r.blockSize));
            expect(r.dropoutBlocks == 0, "Dropout-risk blocks at block " + juce::String(r.blockSize));
        }
    }
};

static AudioEngineRealtimeUnitTests audioEngineRealtimeUnitTests;
#endif

// ========================================================== 
// LEGACY FREQUENCY DETECTION
// ========================================================== 

std::pair<float, float> AudioEngine::calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate)
{
    if (sampleRate <= 0.0) return { 0.0f, 0.0f };

    const auto normalizedCorrelation = [signal, numSamples](int lag) noexcept
        {
            float sum = 0.0f;
            float sumSqA = 0.0f;
            float sumSqB = 0.0f;
            const int limit = numSamples - lag;

            for (int i = 0; i < limit; ++i)
            {
                const float s1 = signal[i];
                const float s2 = signal[i + lag];
                sum += s1 * s2;
                sumSqA += s1 * s1;
                sumSqB += s2 * s2;
            }

            const float denom = std::sqrt(sumSqA * sumSqB);
            if (denom <= 0.00001f)
                return 0.0f;

            return sum / denom;
        };

    int minPeriod = (int)(sampleRate / 1500.0);
    int maxPeriod = (int)(sampleRate / 40.0);
    if (maxPeriod > numSamples / 2) maxPeriod = numSamples / 2;

    float bestCorrelation = 0.0f;
    int bestPeriod = 0;

    for (int lag = minPeriod; lag < maxPeriod; ++lag)
    {
        const float correlation = normalizedCorrelation(lag);

        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            bestPeriod = lag;
        }
    }

    if (bestCorrelation < 0.2f) return { 0.0f, 0.0f };

    float finalPeriod = (float)bestPeriod;
    if (bestPeriod > minPeriod && bestPeriod < maxPeriod - 1)
    {
        const float prevCorr = normalizedCorrelation(bestPeriod - 1);
        const float nextCorr = normalizedCorrelation(bestPeriod + 1);

        float denominator = prevCorr - 2.0f * bestCorrelation + nextCorr;
        if (std::abs(denominator) > 0.00001f)
        {
            float delta = (prevCorr - nextCorr) / (2.0f * denominator);
            finalPeriod = bestPeriod - delta;
        }
    }

    return { (float)(sampleRate / finalPeriod), bestCorrelation };
}

float AudioEngine::calculateFrequency(const float* signal, int numSamples, double sampleRate)
{
    return calculateFrequencyWithClarity(signal, numSamples, sampleRate).first;
}
