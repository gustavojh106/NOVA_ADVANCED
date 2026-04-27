#include "AudioEngine.h"
#include "PedalRegistry.h"
#include "SessionLogger.h"
#include "../Effects/Pedals/Base/ProcessorBase.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    constexpr float kEndpointEpsilon = 1.0e-5f;
    constexpr float kDefaultHardLimit = 4.0f;
    constexpr int kGraphRetireGraceBlocks = 8;
    constexpr int kProductionMeterDecimation = 16;
    constexpr int kLightMeterDecimation = 4;
    constexpr int kFullMeterDecimation = 1;
    constexpr int kMinimumScratchBlocks = 8192;
    constexpr double kDefaultMixRampSeconds = 0.008; // 8 ms: fast enough for knobs, slow enough to avoid clicks.

    juce::String boolToText(bool value)
    {
        return value ? "true" : "false";
    }

    juce::String zoneToText(Nova::ZoneID zone)
    {
        switch (zone)
        {
        case Nova::ZoneID::Pre:     return "Pre";
        case Nova::ZoneID::Amp:     return "Amp";
        case Nova::ZoneID::FX:      return "FX";
        case Nova::ZoneID::Cabinet: return "Cabinet";
        default:                    return "Unknown";
        }
    }

    juce::String switchModeToText(int mode)
    {
        switch (static_cast<Nova::SwitcherMode>(mode))
        {
        case Nova::SwitcherMode::LineA_Only:     return "LineA_Only";
        case Nova::SwitcherMode::LineB_Only:     return "LineB_Only";
        case Nova::SwitcherMode::Dual_Parallel:  return "Dual_Parallel";
        default:                                 return "Unknown";
        }
    }

    int zoneRank(Nova::ZoneID zone) noexcept
    {
        switch (zone)
        {
        case Nova::ZoneID::Pre:     return 0;
        case Nova::ZoneID::Amp:     return 1;
        case Nova::ZoneID::FX:      return 2;
        case Nova::ZoneID::Cabinet: return 3;
        default:                    return 4;
        }
    }

    float normalizeMixValue(float raw) noexcept
    {
        if (raw <= 1.0f)
            return juce::jlimit(0.0f, 1.0f, raw);

        return juce::jlimit(0.0f, 100.0f, raw) / 100.0f;
    }

    float safeHardLimit() noexcept
    {
#if defined(Nova_Config_HARD_ABS_LIMIT_LINEAR)
        return Nova::Config::HARD_ABS_LIMIT_LINEAR;
#else
        return kDefaultHardLimit;
#endif
    }

    juce::String formatRuntimeParams(const AudioEngine::RuntimeGlobalParams& snapshot)
    {
        juce::String result;
        result << "inputGainDb=" << snapshot.inputGainDb
            << ", gateThresholdDb=" << snapshot.gateThresholdDb
            << ", forceMono=" << boolToText(snapshot.forceMono)
            << ", hostTempoBpm=" << snapshot.hostTempoBpm
            << ", hostTempoValid=" << boolToText(snapshot.hostTempoValid)
            << ", hostTransportPlaying=" << boolToText(snapshot.hostTransportPlaying)
            << ", outputVolumeDb=" << snapshot.outputVolumeDb
            << ", outputLimiterDb=" << snapshot.outputLimiterDb
            << ", outputMixRaw=" << snapshot.outputMixRaw
            << ", switchMode=" << switchModeToText(snapshot.switchMode)
            << ", gainA=" << snapshot.gainA
            << ", panA=" << snapshot.panA
            << ", widthA=" << snapshot.widthA
            << ", gainB=" << snapshot.gainB
            << ", panB=" << snapshot.panB
            << ", widthB=" << snapshot.widthB;
        return result;
    }

    juce::String formatProfilingLine(const AudioEngine::ProfilingResult& r)
    {
        juce::String line;
        line << "block=" << r.blockSize
            << ", blocks=" << r.processedBlocks
            << ", avgMs=" << juce::String(r.avgMs, 4)
            << ", peakMs=" << juce::String(r.peakMs, 4)
            << ", avgCpu=" << juce::String(r.avgCpuPercent, 2) << "%"
            << ", peakCpu=" << juce::String(r.peakCpuPercent, 2) << "%"
            << ", invalid=" << r.invalidSamples
            << ", clipped=" << r.clippedSamples
            << ", dropouts=" << r.dropoutBlocks
            << ", clickSpikes=" << r.clickSpikeBlocks
            << ", passed=" << boolToText(r.passed);

        if (r.notes.isNotEmpty())
            line << ", notes=" << r.notes;

        return line;
    }
}

// ========================================================== 
// PARAMETER ATOMICS
// ========================================================== 

void AudioEngine::RuntimeParameterAtomics::store(const RuntimeGlobalParams& s) noexcept
{
    inputGainDb.store(s.inputGainDb, std::memory_order_relaxed);
    gateThresholdDb.store(s.gateThresholdDb, std::memory_order_relaxed);
    forceMono.store(s.forceMono, std::memory_order_relaxed);
    hostTempoBpm.store(s.hostTempoBpm, std::memory_order_relaxed);
    hostTempoValid.store(s.hostTempoValid, std::memory_order_relaxed);
    hostTransportPlaying.store(s.hostTransportPlaying, std::memory_order_relaxed);

    outputVolumeDb.store(s.outputVolumeDb, std::memory_order_relaxed);
    outputLimiterDb.store(s.outputLimiterDb, std::memory_order_relaxed);
    outputMixNormalized.store(normalizeMixValue(s.outputMixRaw), std::memory_order_relaxed);

    switchMode.store(s.switchMode, std::memory_order_relaxed);
    gainA.store(s.gainA, std::memory_order_relaxed);
    panA.store(s.panA, std::memory_order_relaxed);
    widthA.store(s.widthA, std::memory_order_relaxed);
    gainB.store(s.gainB, std::memory_order_relaxed);
    panB.store(s.panB, std::memory_order_relaxed);
    widthB.store(s.widthB, std::memory_order_relaxed);

    revision.fetch_add(1, std::memory_order_release);
}

AudioEngine::RuntimeGlobalParams AudioEngine::RuntimeParameterAtomics::load() const noexcept
{
    RuntimeGlobalParams s;
    s.inputGainDb = inputGainDb.load(std::memory_order_relaxed);
    s.gateThresholdDb = gateThresholdDb.load(std::memory_order_relaxed);
    s.forceMono = forceMono.load(std::memory_order_relaxed);
    s.hostTempoBpm = hostTempoBpm.load(std::memory_order_relaxed);
    s.hostTempoValid = hostTempoValid.load(std::memory_order_relaxed);
    s.hostTransportPlaying = hostTransportPlaying.load(std::memory_order_relaxed);

    s.outputVolumeDb = outputVolumeDb.load(std::memory_order_relaxed);
    s.outputLimiterDb = outputLimiterDb.load(std::memory_order_relaxed);
    s.outputMixRaw = outputMixNormalized.load(std::memory_order_relaxed) * 100.0f;

    s.switchMode = switchMode.load(std::memory_order_relaxed);
    s.gainA = gainA.load(std::memory_order_relaxed);
    s.panA = panA.load(std::memory_order_relaxed);
    s.widthA = widthA.load(std::memory_order_relaxed);
    s.gainB = gainB.load(std::memory_order_relaxed);
    s.panB = panB.load(std::memory_order_relaxed);
    s.widthB = widthB.load(std::memory_order_relaxed);
    return s;
}

// ========================================================== 
// SAMPLE-ACCURATE RAMP
// ========================================================== 

void AudioEngine::SampleAccurateRamp::prepare(double newSampleRate, double seconds) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    defaultRampSamples = juce::jmax(1, juce::roundToInt(sampleRate * juce::jmax(0.001, seconds)));
    reset(current);
}

void AudioEngine::SampleAccurateRamp::reset(float value) noexcept
{
    current = target = juce::jlimit(0.0f, 1.0f, value);
    step = 0.0f;
    samplesRemaining = 0;
}

void AudioEngine::SampleAccurateRamp::setTarget(float value) noexcept
{
    const float newTarget = juce::jlimit(0.0f, 1.0f, value);
    if (std::abs(newTarget - target) < 1.0e-7f)
        return;

    target = newTarget;
    samplesRemaining = defaultRampSamples;
    step = (target - current) / (float)samplesRemaining;
}

float AudioEngine::SampleAccurateRamp::getNext() noexcept
{
    if (samplesRemaining <= 0)
    {
        current = target;
        return current;
    }

    current += step;
    --samplesRemaining;

    if (samplesRemaining <= 0)
    {
        current = target;
        step = 0.0f;
    }

    return current;
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

    audioPlane.activeGraphRaw.store(nullptr, std::memory_order_release);
    {
        const juce::ScopedLock lock(controlPlane.activeOwnerLock);
        controlPlane.activeOwner.reset();
        controlPlane.retiredGraphs.clear();
    }
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
    audioPlane.consecutiveCorruptBlocks = 0;
    audioPlane.recoveryCooldownBlocks = 0;
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
    auto runtime = std::make_shared<GraphRuntime>();
    runtime->graph = std::make_unique<juce::AudioProcessorGraph>();
    runtime->sampleRate = audioPlane.currentSampleRate > 0.0 ? audioPlane.currentSampleRate : 44100.0;
    runtime->blockSize = juce::jmax(1, audioPlane.currentBlockSize);
    runtime->numInputs = juce::jmax(1, audioPlane.numInputChannels);
    runtime->numOutputs = juce::jmax(1, audioPlane.numOutputChannels);
    runtime->generation = generation;

    auto& graph = *runtime->graph;
    graph.setPlayConfigDetails(runtime->numInputs, runtime->numOutputs, runtime->sampleRate, runtime->blockSize);

    runtime->inputNode = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
        juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
    runtime->outputNode = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
        juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    runtime->inputChainNode = graph.addNode(std::make_unique<InputChainProcessor>());
    runtime->stripNodeA = graph.addNode(std::make_unique<ChannelStripProcessor>());
    runtime->stripNodeB = graph.addNode(std::make_unique<ChannelStripProcessor>());
    runtime->outputChainNode = graph.addNode(std::make_unique<OutputChainProcessor>());

    runtime->inputChain = runtime->inputChainNode != nullptr
        ? static_cast<InputChainProcessor*>(runtime->inputChainNode->getProcessor())
        : nullptr;
    runtime->stripA = runtime->stripNodeA != nullptr
        ? static_cast<ChannelStripProcessor*>(runtime->stripNodeA->getProcessor())
        : nullptr;
    runtime->stripB = runtime->stripNodeB != nullptr
        ? static_cast<ChannelStripProcessor*>(runtime->stripNodeB->getProcessor())
        : nullptr;
    runtime->outputChain = runtime->outputChainNode != nullptr
        ? static_cast<OutputChainProcessor*>(runtime->outputChainNode->getProcessor())
        : nullptr;

    if (runtime->stripA != nullptr)
        runtime->stripA->setTelemetryTag("channel-strip-a");
    if (runtime->stripB != nullptr)
        runtime->stripB->setTelemetryTag("channel-strip-b");
    if (runtime->outputChain != nullptr)
        runtime->outputChain->setTelemetryTag("output-chain");

    if (runtime->inputNode != nullptr && runtime->inputChainNode != nullptr)
    {
        graph.addConnection({ { runtime->inputNode->nodeID, 0 }, { runtime->inputChainNode->nodeID, 0 } });
        if (runtime->numInputs > 1)
            graph.addConnection({ { runtime->inputNode->nodeID, 1 }, { runtime->inputChainNode->nodeID, 1 } });
        else
            graph.addConnection({ { runtime->inputNode->nodeID, 0 }, { runtime->inputChainNode->nodeID, 1 } });
    }

    const auto buildChain = [&graph](const std::vector<ChainNodeSpec>& specs,
        std::vector<ChainRuntimeSlot>& runtimeSlots)
        {
            runtimeSlots.clear();
            runtimeSlots.reserve(specs.size());

            for (const auto& spec : specs)
            {
                auto pedal = PedalRegistry::createPedal(spec.pedalType);
                if (!pedal)
                {
                    NovaDiagnostics::SessionLogger::logEvent("engine.addPedal.failed",
                        "PedalRegistry::createPedal returned nullptr for type=\"" + spec.pedalType
                        + "\", pedalID=" + spec.pedalID);
                    continue;
                }

                auto node = graph.addNode(std::move(pedal));
                if (node == nullptr)
                {
                    NovaDiagnostics::SessionLogger::logEvent("engine.addPedal.failed",
                        "graph.addNode returned nullptr for type=\"" + spec.pedalType
                        + "\", pedalID=" + spec.pedalID);
                    continue;
                }

                ChainRuntimeSlot slot;
                slot.node = node;
                slot.pedalType = spec.pedalType;
                slot.pedalID = spec.pedalID;
                slot.zone = spec.zone;
                slot.bypassed = spec.bypassed;
                slot.processor = node->getProcessor();

                // Cached once on the control thread. Never dynamic_cast from process().
                slot.baseProcessor = dynamic_cast<ProcessorBase*>(slot.processor);
                slot.tempoSyncProcessor = dynamic_cast<TempoSyncable*>(slot.processor);

                if (slot.baseProcessor != nullptr)
                    slot.baseProcessor->setBypassed(spec.bypassed);
                else if (slot.processor != nullptr)
                    slot.processor->suspendProcessing(spec.bypassed);

                runtimeSlots.push_back(std::move(slot));
            }
        };

    buildChain(controlPlane.modelChainA, runtime->chainA);
    buildChain(controlPlane.modelChainB, runtime->chainB);

    if (runtime->inputChainNode != nullptr && runtime->stripNodeA != nullptr)
        connectRuntimeChain(*runtime, runtime->chainA, runtime->stripNodeA->nodeID);

    if (runtime->inputChainNode != nullptr && runtime->stripNodeB != nullptr)
        connectRuntimeChain(*runtime, runtime->chainB, runtime->stripNodeB->nodeID);

    if (runtime->outputChainNode != nullptr && runtime->outputNode != nullptr)
    {
        for (auto* strip : { runtime->stripNodeA.get(), runtime->stripNodeB.get() })
        {
            if (strip == nullptr)
                continue;

            graph.addConnection({ { strip->nodeID, 0 }, { runtime->outputChainNode->nodeID, 0 } });
            graph.addConnection({ { strip->nodeID, 1 }, { runtime->outputChainNode->nodeID, 1 } });
        }

        graph.addConnection({ { runtime->outputChainNode->nodeID, 0 }, { runtime->outputNode->nodeID, 0 } });
        graph.addConnection({ { runtime->outputChainNode->nodeID, 1 }, { runtime->outputNode->nodeID, 1 } });
    }

    applyRuntimeParamsToGraph(*runtime, params.load(), params.revision.load(std::memory_order_acquire));
    graph.prepareToPlay(runtime->sampleRate, runtime->blockSize);
    graph.rebuild();
    runtime->latencySamples = juce::jlimit(0, Nova::Config::MAX_GRAPH_LATENCY_SAMPLES, graph.getLatencySamples());

    return runtime;
}

void AudioEngine::connectRuntimeChain(GraphRuntime& runtime,
    const std::vector<ChainRuntimeSlot>& chain,
    juce::AudioProcessorGraph::NodeID targetStripID)
{
    if (runtime.graph == nullptr || runtime.inputChainNode == nullptr)
        return;

    auto& graph = *runtime.graph;
    if (graph.getNodeForId(targetStripID) == nullptr)
        return;

    juce::AudioProcessorGraph::NodeID currentSource = runtime.inputChainNode->nodeID;

    std::vector<size_t> ordered;
    ordered.reserve(chain.size());
    for (size_t i = 0; i < chain.size(); ++i)
        ordered.push_back(i);

    std::stable_sort(ordered.begin(), ordered.end(), [&chain](size_t a, size_t b)
        {
            return zoneRank(chain[a].zone) < zoneRank(chain[b].zone);
        });

    for (size_t index : ordered)
    {
        const auto& slot = chain[index];
        if (slot.node == nullptr || graph.getNodeForId(slot.node->nodeID) == nullptr)
            continue;

        graph.addConnection({ { currentSource, 0 }, { slot.node->nodeID, 0 } });
        graph.addConnection({ { currentSource, 1 }, { slot.node->nodeID, 1 } });
        currentSource = slot.node->nodeID;
    }

    graph.addConnection({ { currentSource, 0 }, { targetStripID, 0 } });
    graph.addConnection({ { currentSource, 1 }, { targetStripID, 1 } });
}

void AudioEngine::publishGraph(std::shared_ptr<GraphRuntime> newGraph)
{
    if (newGraph == nullptr)
        return;

    const uint64_t nowBlock = audioPlane.audioBlockCounter.load(std::memory_order_acquire);
    std::shared_ptr<GraphRuntime> oldGraph;

    {
        const juce::ScopedLock ownerLock(controlPlane.activeOwnerLock);
        oldGraph = std::move(controlPlane.activeOwner);
        controlPlane.activeOwner = std::move(newGraph);

        audioPlane.activeGraphRaw.store(controlPlane.activeOwner.get(), std::memory_order_release);
        updateDryDelayLatency(controlPlane.activeOwner->latencySamples);

        if (oldGraph != nullptr)
            controlPlane.retiredGraphs.push_back({ std::move(oldGraph), nowBlock + kGraphRetireGraceBlocks });
    }

    cleanupRetiredGraphs();
}

void AudioEngine::cleanupRetiredGraphs()
{
    const uint64_t nowBlock = audioPlane.audioBlockCounter.load(std::memory_order_acquire);
    const juce::ScopedLock ownerLock(controlPlane.activeOwnerLock);

    controlPlane.retiredGraphs.erase(
        std::remove_if(controlPlane.retiredGraphs.begin(), controlPlane.retiredGraphs.end(),
            [nowBlock](const RetiredGraph& retired)
            {
                return retired.releaseAfterAudioBlock <= nowBlock;
            }),
        controlPlane.retiredGraphs.end());

    // Keep memory bounded even if audio is stopped and the block counter is not moving.
    constexpr size_t kMaxRetiredGraphs = 8;
    while (controlPlane.retiredGraphs.size() > kMaxRetiredGraphs)
        controlPlane.retiredGraphs.erase(controlPlane.retiredGraphs.begin());
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
    if (audioPlane.audioThreadID == juce::Thread::ThreadID()
        || juce::Thread::getCurrentThreadId() != audioPlane.audioThreadID)
        triggerAsyncUpdate();
}

AudioEngine::RuntimeGlobalParams AudioEngine::loadRuntimeParams() const noexcept
{
    return params.load();
}

void AudioEngine::applyRuntimeParamsToGraph(GraphRuntime& runtime, const RuntimeGlobalParams& snapshot, uint32_t revision)
{
    if (runtime.appliedParamRevision == revision)
        return;

    if (runtime.inputChain != nullptr)
    {
        float gateThreshold = snapshot.gateThresholdDb;
        if (gateThreshold == 0.0f)
            gateThreshold = -100.0f;

        runtime.inputChain->setParams(snapshot.inputGainDb, gateThreshold, snapshot.forceMono);
    }

    if (runtime.outputChain != nullptr)
        runtime.outputChain->setParams(snapshot.outputVolumeDb, snapshot.outputLimiterDb);

    const bool muteA = (snapshot.switchMode == (int)Nova::SwitcherMode::LineB_Only);
    const bool muteB = (snapshot.switchMode == (int)Nova::SwitcherMode::LineA_Only);
    const bool dualParallel = (snapshot.switchMode == (int)Nova::SwitcherMode::Dual_Parallel);
    constexpr float kParallelGainComp = 0.5f;

    if (runtime.stripA != nullptr)
    {
        float gain = muteA ? 0.0f : snapshot.gainA;
        if (!muteA && gain <= 0.001f)
            gain = 1.0f;
        if (!muteA && dualParallel)
            gain *= kParallelGainComp;

        runtime.stripA->setParams(gain, snapshot.panA, snapshot.widthA);
    }

    if (runtime.stripB != nullptr)
    {
        float gain = muteB ? 0.0f : snapshot.gainB;
        if (!muteB && gain <= 0.001f)
            gain = 1.0f;
        if (!muteB && dualParallel)
            gain *= kParallelGainComp;

        runtime.stripB->setParams(gain, snapshot.panB, snapshot.widthB);
    }

    const auto applyTempo = [&snapshot](std::vector<ChainRuntimeSlot>& chain)
        {
            for (auto& slot : chain)
            {
                if (slot.tempoSyncProcessor != nullptr)
                    slot.tempoSyncProcessor->setTempoSyncContext(snapshot.hostTempoBpm,
                        snapshot.hostTempoValid,
                        snapshot.hostTransportPlaying);
            }
        };

    applyTempo(runtime.chainA);
    applyTempo(runtime.chainB);
    runtime.appliedParamRevision = revision;
}

void AudioEngine::applyPedalBypassToActiveGraph(Nova::ChainID chain, int index, bool bypassed)
{
    GraphRuntime* runtime = audioPlane.activeGraphRaw.load(std::memory_order_acquire);
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
}

// ========================================================== 
// CONTROL COMMANDS
// ========================================================== 

void AudioEngine::enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe)
{
    {
        const juce::ScopedLock lock(controlPlane.commandLock);
        controlPlane.pendingCommands.push_back(cmd);
        controlPlane.commandsPending.store(true, std::memory_order_release);
    }

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
    if (!controlPlane.commandsPending.load(std::memory_order_acquire)
        && !audioPlane.graphResetRequested.load(std::memory_order_acquire))
    {
        const auto revision = params.revision.load(std::memory_order_acquire);
        GraphRuntime* active = audioPlane.activeGraphRaw.load(std::memory_order_acquire);
        if (active != nullptr && active->appliedParamRevision != revision)
            applyRuntimeParamsToGraph(*active, params.load(), revision);

        cleanupRetiredGraphs();
        return;
    }

    std::deque<GraphCommand> commands;
    {
        const juce::ScopedLock lock(controlPlane.commandLock);
        commands.swap(controlPlane.pendingCommands);
        controlPlane.commandsPending.store(false, std::memory_order_release);
    }

    bool topologyChanged = false;
    bool paramsChangedOnly = false;
    bool explicitRebuild = false;

    {
        const juce::ScopedLock modelLock(controlPlane.modelLock);

        for (const auto& cmd : commands)
        {
            if (cmd.type == GraphCommandType::RebuildGraph)
                explicitRebuild = true;

            applyGraphCommandToModel(cmd, topologyChanged, paramsChangedOnly);
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

    const auto revision = params.revision.load(std::memory_order_acquire);
    GraphRuntime* active = audioPlane.activeGraphRaw.load(std::memory_order_acquire);
    if (active != nullptr)
        applyRuntimeParamsToGraph(*active, params.load(), revision);

    if (paramsChangedOnly && !topologyChanged)
        cleanupRetiredGraphs();
}

bool AudioEngine::applyGraphCommandToModel(const GraphCommand& cmd, bool& topologyChanged, bool& paramsChangedOnly)
{
    auto& chain = (cmd.chain == Nova::ChainID::LineA) ? controlPlane.modelChainA : controlPlane.modelChainB;

    switch (cmd.type)
    {
    case GraphCommandType::AddPedal:
    {
        ChainNodeSpec spec;
        spec.pedalType = cmd.pedalType;
        spec.pedalID = cmd.pedalID;
        spec.zone = cmd.zone;
        spec.bypassed = false;

        if (cmd.index >= 0 && cmd.index <= (int)chain.size())
            chain.insert(chain.begin() + cmd.index, std::move(spec));
        else
            chain.push_back(std::move(spec));

        topologyChanged = true;
        return true;
    }

    case GraphCommandType::RemovePedal:
    {
        if (cmd.index >= 0 && cmd.index < (int)chain.size())
        {
            chain.erase(chain.begin() + cmd.index);
            topologyChanged = true;
            return true;
        }
        return false;
    }

    case GraphCommandType::MovePedal:
    {
        const int from = cmd.index;
        const int to = cmd.toIndex;

        if (from >= 0 && from < (int)chain.size()
            && to >= 0 && to <= (int)chain.size()
            && from != to)
        {
            auto slot = std::move(chain[(size_t)from]);
            chain.erase(chain.begin() + from);

            int adjustedTo = to;
            if (from < to)
                --adjustedTo;

            adjustedTo = juce::jlimit(0, (int)chain.size(), adjustedTo);
            chain.insert(chain.begin() + adjustedTo, std::move(slot));
            topologyChanged = true;
            return true;
        }
        return false;
    }

    case GraphCommandType::ClearAll:
    {
        controlPlane.modelChainA.clear();
        controlPlane.modelChainB.clear();
        topologyChanged = true;
        return true;
    }

    case GraphCommandType::SetPedalBypass:
    {
        if (!juce::isPositiveAndBelow(cmd.index, (int)chain.size()))
            return false;

        chain[(size_t)cmd.index].bypassed = cmd.flag;
        applyPedalBypassToActiveGraph(cmd.chain, cmd.index, cmd.flag);
        paramsChangedOnly = true;
        return true;
    }

    case GraphCommandType::SetEngineEnabled:
    {
        const bool previous = audioPlane.isEngineOn.exchange(cmd.flag, std::memory_order_acq_rel);
        if (cmd.flag && !previous)
        {
            audioPlane.audioRuntimeResetRequested.store(true, std::memory_order_release);
            topologyChanged = true; // rebuild fresh graph before enabling processing
        }
        return true;
    }

    case GraphCommandType::RebuildGraph:
    {
        topologyChanged = true;
        return true;
    }

    default:
        return false;
    }
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

void AudioEngine::synchronizeProcessingState()
{
    // Do not ever lock or flush from the realtime callback.
    if (audioPlane.audioThreadID != juce::Thread::ThreadID()
        && juce::Thread::getCurrentThreadId() == audioPlane.audioThreadID)
    {
        return;
    }

    flushPendingGraphCommands();
}

// ========================================================== 
// SCRATCH / RESET
// ========================================================== 

void AudioEngine::prepareScratchBuffers(double sampleRate, int samplesPerBlock, int numChannels)
{
    const int capacitySamples = juce::jmax(kMinimumScratchBlocks, samplesPerBlock * 4);
    const int channels = juce::jmax(2, numChannels);

    audioPlane.scratchBlockCapacity = capacitySamples;
    audioPlane.scratchChannelCapacity = channels;
    audioPlane.dryScratch.setSize(channels, capacitySamples, false, false, true);
    audioPlane.delayedDryScratch.setSize(channels, capacitySamples, false, false, true);
    audioPlane.dryScratch.clear();
    audioPlane.delayedDryScratch.clear();

    const int delaySize = Nova::Config::MAX_GRAPH_LATENCY_SAMPLES + capacitySamples + 8;
    audioPlane.dryDelay.assign((size_t)channels, std::vector<float>((size_t)delaySize, 0.0f));
    audioPlane.dryDelayBufferSize = delaySize;
    audioPlane.dryDelayWriteIndex = 0;
    audioPlane.currentDryLatencySamples.store(0, std::memory_order_relaxed);

    audioPlane.wetMixRamp.prepare(sampleRate, kDefaultMixRampSeconds);
    audioPlane.wetMixRamp.reset(params.outputMixNormalized.load(std::memory_order_relaxed));
}

void AudioEngine::resetAudioRuntimeState(bool resetMeters)
{
    audioPlane.wetMixRamp.reset(params.outputMixNormalized.load(std::memory_order_relaxed));
    resetDryDelayLine();
    audioPlane.tunerService.reset();
    if (!resetMeters)
        audioPlane.startupCounter = juce::jmax(audioPlane.startupCounter, Nova::Config::STARTUP_COUNTER_GRAPH_CHANGE);
    audioPlane.consecutiveCorruptBlocks = 0;
    audioPlane.recoveryCooldownBlocks = 0;
    audioPlane.silentOutputBlockCounter.store(0, std::memory_order_relaxed);
    audioPlane.silentOutputIncidentActive.store(false, std::memory_order_relaxed);

    if (resetMeters)
    {
        audioPlane.cpuUsage.store(0.0, std::memory_order_relaxed);
        audioPlane.lastProcessTimeMs.store(0.0, std::memory_order_relaxed);
        audioPlane.averageProcessTimeMs.store(0.0, std::memory_order_relaxed);
        audioPlane.peakProcessTimeMs.store(0.0, std::memory_order_relaxed);
        audioPlane.lastInputPeak.store(0.0f, std::memory_order_relaxed);
        audioPlane.lastOutputPeak.store(0.0f, std::memory_order_relaxed);
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
    for (auto& channel : audioPlane.dryDelay)
        std::fill(channel.begin(), channel.end(), 0.0f);

    audioPlane.dryDelayWriteIndex = 0;
}

void AudioEngine::updateDryDelayLatency(int latencySamples) noexcept
{
    audioPlane.currentDryLatencySamples.store(juce::jlimit(0,
        Nova::Config::MAX_GRAPH_LATENCY_SAMPLES,
        latencySamples), std::memory_order_release);
}

// ========================================================== 
// PROCESSING
// ========================================================== 

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (audioPlane.audioThreadID == juce::Thread::ThreadID())
        audioPlane.audioThreadID = juce::Thread::getCurrentThreadId();

    audioPlane.audioBlockCounter.fetch_add(1, std::memory_order_release);

    if (audioPlane.audioRuntimeResetRequested.exchange(false, std::memory_order_acq_rel))
        resetAudioRuntimeState(false);

    const auto mode = static_cast<DiagnosticsMode>(diagnosticsMode.load(std::memory_order_relaxed));

    // Lightweight realtime timing meter.
    // This replaces the old heavy per-block diagnostic timing, but keeps CPU/process-time
    // visible in the UI in every diagnostics mode. It only reads the high-resolution timer
    // at block entry/exit and updates atomics; no strings, no locks, no allocation.
    const double timingStartMs = juce::Time::getMillisecondCounterHiRes();
    const auto commitTiming = [this, timingStartMs, &buffer]() noexcept
        {
            updateRealtimeTimingMeters(juce::Time::getMillisecondCounterHiRes() - timingStartMs,
                buffer.getNumSamples());
        };

    const float mixTarget = params.outputMixNormalized.load(std::memory_order_relaxed);
    audioPlane.wetMixRamp.setTarget(mixTarget);

    const float inputPeak = updateInputMeterLight(buffer);

    if (!audioPlane.isEngineOn.load(std::memory_order_acquire))
    {
        const auto health = sanitizeAndMeterOutput(buffer, inputPeak, mode != DiagnosticsMode::Production);
        handleHealthAfterBlock(health, buffer.getNumSamples(), false);
        commitTiming();
        return;
    }

    if (audioPlane.startupCounter > 0)
    {
        --audioPlane.startupCounter;
        const auto health = sanitizeAndMeterOutput(buffer, inputPeak, mode != DiagnosticsMode::Production);
        handleHealthAfterBlock(health, buffer.getNumSamples(), false);
        commitTiming();
        return;
    }

    if (audioPlane.tunerEnabled.load(std::memory_order_acquire))
    {
        audioPlane.tunerService.pushBuffer(buffer);
        buffer.clear();
        const auto health = sanitizeAndMeterOutput(buffer, inputPeak, false);
        handleHealthAfterBlock(health, buffer.getNumSamples(), false);
        commitTiming();
        return;
    }

    GraphRuntime* runtime = audioPlane.activeGraphRaw.load(std::memory_order_acquire);
    if (runtime == nullptr || runtime->graph == nullptr)
    {
        buffer.clear();
        const auto health = sanitizeAndMeterOutput(buffer, inputPeak, true);
        handleHealthAfterBlock(health, buffer.getNumSamples(), false);
        commitTiming();
        return;
    }

    BlockHealthStats health;
    health.inputPeak = inputPeak;
    processWithSampleAccurateDryWet(*runtime, buffer, midi, health);

    const auto outputHealth = sanitizeAndMeterOutput(buffer, inputPeak, mode != DiagnosticsMode::Production);
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
    BlockHealthStats& health)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), audioPlane.scratchChannelCapacity);

    if (numSamples <= 0 || numChannels <= 0)
        return;

    // Realtime safety: never allocate here. If a host breaks the prepared block contract,
    // process wet-only instead of allocating from the audio thread.
    if (numSamples > audioPlane.scratchBlockCapacity)
    {
        runtime.graph->processBlock(buffer, midi);
        return;
    }

    const float currentMix = audioPlane.wetMixRamp.getCurrent();
    const bool mixSettled = !audioPlane.wetMixRamp.isSmoothing();

    if (mixSettled && currentMix <= kEndpointEpsilon)
    {
        // Exact dry path. Still consume the ramp for consistency, but do not touch the graph.
        for (int i = 0; i < numSamples; ++i)
            (void)audioPlane.wetMixRamp.getNext();
        return;
    }

    audioPlane.dryScratch.setSize(numChannels, numSamples, false, false, true);
    audioPlane.delayedDryScratch.setSize(numChannels, numSamples, false, false, true);

    for (int ch = 0; ch < numChannels; ++ch)
        audioPlane.dryScratch.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    runtime.graph->processBlock(buffer, midi);

    if (mixSettled && currentMix >= (1.0f - kEndpointEpsilon))
    {
        for (int i = 0; i < numSamples; ++i)
            (void)audioPlane.wetMixRamp.getNext();
        return;
    }

    copyDryThroughLatency(audioPlane.dryScratch, audioPlane.delayedDryScratch, numChannels, numSamples);
    mixWetDrySampleAccurate(buffer, audioPlane.delayedDryScratch, numChannels, numSamples);
}

void AudioEngine::mixWetDrySampleAccurate(juce::AudioBuffer<float>& wetBuffer,
    const juce::AudioBuffer<float>& dryBuffer,
    int numChannels,
    int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float wet = audioPlane.wetMixRamp.getNext();
        const float dry = 1.0f - wet;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* wetData = wetBuffer.getWritePointer(ch);
            const auto* dryData = dryBuffer.getReadPointer(ch);
            wetData[i] = dryData[i] * dry + wetData[i] * wet;
        }
    }
}

void AudioEngine::copyDryThroughLatency(const juce::AudioBuffer<float>& dryIn,
    juce::AudioBuffer<float>& dryOut,
    int numChannels,
    int numSamples) noexcept
{
    const int latency = juce::jlimit(0,
        juce::jmax(0, audioPlane.dryDelayBufferSize - numSamples - 1),
        audioPlane.currentDryLatencySamples.load(std::memory_order_acquire));

    if (latency == 0 || audioPlane.dryDelayBufferSize <= 0)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            dryOut.copyFrom(ch, 0, dryIn, ch, 0, numSamples);
        return;
    }

    int writeIndex = audioPlane.dryDelayWriteIndex;
    const int delaySize = audioPlane.dryDelayBufferSize;

    for (int i = 0; i < numSamples; ++i)
    {
        const int readIndex = (writeIndex + delaySize - latency) % delaySize;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& delay = audioPlane.dryDelay[(size_t)ch];
            dryOut.setSample(ch, i, delay[(size_t)readIndex]);
            delay[(size_t)writeIndex] = dryIn.getSample(ch, i);
        }

        ++writeIndex;
        if (writeIndex >= delaySize)
            writeIndex = 0;
    }

    audioPlane.dryDelayWriteIndex = writeIndex;
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

AudioEngine::BlockHealthStats AudioEngine::sanitizeAndMeterOutput(juce::AudioBuffer<float>& buffer,
    float inputPeak,
    bool deepScan) noexcept
{
    BlockHealthStats stats;
    stats.inputPeak = inputPeak;

    constexpr float nearClip = Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD;
    constexpr float spikeDelta = Nova::Config::SIGNAL_SPIKE_DELTA_THRESHOLD;
    const float hardLimit = Nova::Config::HARD_ABS_LIMIT_LINEAR;

    float outputPeak = 0.0f;
    float previousL = 0.0f;
    float previousR = 0.0f;
    bool havePrev = false;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        float previous = ch == 0 ? previousL : previousR;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            float v = data[i];

            if (!std::isfinite(v))
            {
                v = 0.0f;
                data[i] = 0.0f;
                stats.hadCorruption = true;
                ++stats.invalidSamples;
            }
            else if (std::abs(v) > hardLimit)
            {
                v = juce::jlimit(-hardLimit, hardLimit, v);
                data[i] = v;
                stats.hadCorruption = true;
                ++stats.clippedSamples;
            }

            const float absV = std::abs(v);
            outputPeak = juce::jmax(outputPeak, absV);

            if (deepScan)
            {
                if (absV >= nearClip)
                    ++stats.nearClipSamples;

                if (havePrev && std::abs(v - previous) >= spikeDelta)
                    ++stats.clickSpikeSamples;
            }

            previous = v;
            havePrev = true;
        }

        if (ch == 0) previousL = previous;
        if (ch == 1) previousR = previous;
    }

    stats.outputPeak = outputPeak;
    audioPlane.lastOutputPeak.store(outputPeak, std::memory_order_relaxed);
    return stats;
}

void AudioEngine::handleHealthAfterBlock(const BlockHealthStats& health,
    int numSamples,
    bool engineActuallyProcessed) noexcept
{
    if (health.hadCorruption)
        ++audioPlane.consecutiveCorruptBlocks;
    else
        audioPlane.consecutiveCorruptBlocks = 0;

    if (audioPlane.recoveryCooldownBlocks > 0)
        --audioPlane.recoveryCooldownBlocks;

    if (audioPlane.consecutiveCorruptBlocks >= Nova::Config::CORRUPT_BLOCKS_BEFORE_HEAL
        && audioPlane.recoveryCooldownBlocks == 0)
    {
        audioPlane.graphResetRequested.store(true, std::memory_order_release);
        audioPlane.pendingAutoHealLog.store(true, std::memory_order_release);
        audioPlane.autoHealCount.fetch_add(1, std::memory_order_relaxed);
        audioPlane.recoveryCooldownBlocks = Nova::Config::RECOVERY_COOLDOWN_BLOCKS;
        audioPlane.consecutiveCorruptBlocks = 0;
    }

    const bool suspiciousSilence = engineActuallyProcessed
        && audioPlane.isEngineOn.load(std::memory_order_relaxed)
        && !audioPlane.tunerEnabled.load(std::memory_order_relaxed)
        && params.outputMixNormalized.load(std::memory_order_relaxed) > 0.01f
        && health.inputPeak >= Nova::Config::INPUT_ACTIVE_THRESHOLD
        && health.outputPeak <= Nova::Config::OUTPUT_SILENT_THRESHOLD;

    if (suspiciousSilence)
    {
        const int blocks = audioPlane.silentOutputBlockCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        const int triggerBlocks = juce::jmax(8, juce::roundToInt(
            (audioPlane.currentSampleRate / juce::jmax(1, numSamples)) * Nova::Config::SILENT_OUTPUT_TRIGGER_SECONDS));

        if (blocks >= triggerBlocks && !audioPlane.silentOutputIncidentActive.exchange(true, std::memory_order_relaxed))
            audioPlane.pendingSilentOutputLog.store(true, std::memory_order_release);
    }
    else
    {
        audioPlane.silentOutputBlockCounter.store(0, std::memory_order_relaxed);
        if (audioPlane.silentOutputIncidentActive.exchange(false, std::memory_order_relaxed))
            audioPlane.pendingSilentOutputRecoveryLog.store(true, std::memory_order_release);
    }
}


void AudioEngine::updateRealtimeTimingMeters(double elapsedMs, int numSamples) noexcept
{
    if (!std::isfinite(elapsedMs))
        return;

    elapsedMs = juce::jlimit(0.0, 10000.0, elapsedMs);

    audioPlane.lastProcessTimeMs.store(elapsedMs, std::memory_order_relaxed);

    const double previousAverage = audioPlane.averageProcessTimeMs.load(std::memory_order_relaxed);
    const double average = previousAverage <= 0.0
        ? elapsedMs
        : (previousAverage * 0.90) + (elapsedMs * 0.10);
    audioPlane.averageProcessTimeMs.store(average, std::memory_order_relaxed);

    const double previousPeak = audioPlane.peakProcessTimeMs.load(std::memory_order_relaxed);
    const double decayedPeak = previousPeak * 0.995;
    audioPlane.peakProcessTimeMs.store(juce::jmax(elapsedMs, decayedPeak), std::memory_order_relaxed);

    const double sampleRate = audioPlane.currentSampleRate;
    if (sampleRate <= 0.0 || numSamples <= 0)
        return;

    const double blockMs = ((double)numSamples / sampleRate) * 1000.0;
    if (blockMs <= 0.0)
        return;

    const double blockCpu = juce::jlimit(0.0, 10000.0, (elapsedMs / blockMs) * 100.0);
    const double previousCpu = audioPlane.cpuUsage.load(std::memory_order_relaxed);
    const double smoothedCpu = previousCpu <= 0.0
        ? blockCpu
        : (previousCpu * 0.90) + (blockCpu * 0.10);

    audioPlane.cpuUsage.store(smoothedCpu, std::memory_order_relaxed);
}

// ========================================================== 
// INFO / DIAGNOSTICS
// ========================================================== 

std::vector<AudioEngine::ChainNodeView> AudioEngine::getNodes(Nova::ChainID chain) const
{
    std::vector<ChainNodeView> result;

    const juce::ScopedLock ownerLock(controlPlane.activeOwnerLock);
    auto runtime = controlPlane.activeOwner;
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
    GraphRuntime* runtime = audioPlane.activeGraphRaw.load(std::memory_order_acquire);
    if (runtime == nullptr)
        return nullptr;

    auto& list = (chain == Nova::ChainID::LineA) ? runtime->chainA : runtime->chainB;
    if (!juce::isPositiveAndBelow(index, (int)list.size()))
        return nullptr;

    return list[(size_t)index].processor;
}

double AudioEngine::getCpuLoad() const
{
    return audioPlane.cpuUsage.load(std::memory_order_relaxed);
}

double AudioEngine::getLastProcessTimeMs() const
{
    return audioPlane.lastProcessTimeMs.load(std::memory_order_relaxed);
}

double AudioEngine::getAverageProcessTimeMs() const
{
    return audioPlane.averageProcessTimeMs.load(std::memory_order_relaxed);
}

double AudioEngine::getPeakProcessTimeMs() const
{
    return audioPlane.peakProcessTimeMs.load(std::memory_order_relaxed);
}

int AudioEngine::getLatencyNumSamples() const
{
    GraphRuntime* runtime = audioPlane.activeGraphRaw.load(std::memory_order_acquire);
    return runtime != nullptr ? runtime->latencySamples : 0;
}

float AudioEngine::getLastInputPeak() const
{
    return audioPlane.lastInputPeak.load(std::memory_order_relaxed);
}

float AudioEngine::getLastOutputPeak() const
{
    return audioPlane.lastOutputPeak.load(std::memory_order_relaxed);
}

int AudioEngine::getAutoHealCount() const
{
    return audioPlane.autoHealCount.load(std::memory_order_relaxed);
}

void AudioEngine::setDiagnosticsMode(DiagnosticsMode mode)
{
    diagnosticsMode.store((int)mode, std::memory_order_release);
}

AudioEngine::DiagnosticsMode AudioEngine::getDiagnosticsMode() const
{
    return static_cast<DiagnosticsMode>(diagnosticsMode.load(std::memory_order_acquire));
}

juce::String AudioEngine::describeModelChain(const std::vector<ChainNodeSpec>& chain) const
{
    juce::StringArray lines;
    for (size_t i = 0; i < chain.size(); ++i)
    {
        const auto& slot = chain[i];
        juce::String line;
        line << "#" << (int)i
            << " type=" << slot.pedalType
            << " zone=" << zoneToText(slot.zone)
            << " pedalID=" << (slot.pedalID.isEmpty() ? "<none>" : slot.pedalID)
            << " bypassed=" << boolToText(slot.bypassed);
        lines.add(line);
    }

    if (lines.isEmpty())
        lines.add("<empty>");

    return lines.joinIntoString(juce::newLine);
}

juce::String AudioEngine::describeRuntimeChain(const std::vector<ChainRuntimeSlot>& chain) const
{
    juce::StringArray lines;
    for (size_t i = 0; i < chain.size(); ++i)
    {
        const auto& slot = chain[i];
        juce::String line;
        line << "#" << (int)i
            << " type=" << slot.pedalType
            << " zone=" << zoneToText(slot.zone)
            << " pedalID=" << (slot.pedalID.isEmpty() ? "<none>" : slot.pedalID)
            << " bypassed=" << boolToText(slot.bypassed)
            << " processor=" << (slot.processor != nullptr ? slot.processor->getName() : "<null>")
            << " cachedBase=" << boolToText(slot.baseProcessor != nullptr)
            << " cachedTempo=" << boolToText(slot.tempoSyncProcessor != nullptr);
        lines.add(line);
    }

    if (lines.isEmpty())
        lines.add("<empty>");

    return lines.joinIntoString(juce::newLine);
}

juce::String AudioEngine::buildDiagnosticReport() const
{
    const auto snapshot = params.load();
    const GraphRuntime* runtime = audioPlane.activeGraphRaw.load(std::memory_order_acquire);

    juce::String report;
    report << "engineOn=" << boolToText(audioPlane.isEngineOn.load(std::memory_order_relaxed))
        << ", tunerEnabled=" << boolToText(audioPlane.tunerEnabled.load(std::memory_order_relaxed))
        << ", sampleRate=" << audioPlane.currentSampleRate
        << ", blockSize=" << audioPlane.currentBlockSize
        << ", inputChannels=" << audioPlane.numInputChannels
        << ", outputChannels=" << audioPlane.numOutputChannels
        << ", activeGraph=" << (runtime != nullptr ? "true" : "false")
        << ", generation=" << (runtime != nullptr ? (juce::int64)runtime->generation : (juce::int64)0)
        << ", graphLatencySamples=" << (runtime != nullptr ? runtime->latencySamples : 0)
        << ", wetMixTarget=" << params.outputMixNormalized.load(std::memory_order_relaxed)
        << ", wetMixCurrent=" << audioPlane.wetMixRamp.getCurrent()
        << ", cpuLoad=" << audioPlane.cpuUsage.load(std::memory_order_relaxed)
        << ", lastProcessMs=" << audioPlane.lastProcessTimeMs.load(std::memory_order_relaxed)
        << ", avgProcessMs=" << audioPlane.averageProcessTimeMs.load(std::memory_order_relaxed)
        << ", peakProcessMs=" << audioPlane.peakProcessTimeMs.load(std::memory_order_relaxed)
        << ", autoHealCount=" << audioPlane.autoHealCount.load(std::memory_order_relaxed)
        << ", audioBlocks=" << (juce::int64)audioPlane.audioBlockCounter.load(std::memory_order_relaxed)
        << juce::newLine
        << "runtimeParams: " << formatRuntimeParams(snapshot) << juce::newLine;

    if (runtime != nullptr)
    {
        report << "runtime.chainA:" << juce::newLine << describeRuntimeChain(runtime->chainA) << juce::newLine
            << "runtime.chainB:" << juce::newLine << describeRuntimeChain(runtime->chainB) << juce::newLine;
    }

    {
        const juce::ScopedLock modelLock(controlPlane.modelLock);
        report << "model.chainA:" << juce::newLine << describeModelChain(controlPlane.modelChainA) << juce::newLine
            << "model.chainB:" << juce::newLine << describeModelChain(controlPlane.modelChainB);
    }

    return report;
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
        if (controlPlane.commandsPending.load(std::memory_order_acquire)
            || audioPlane.graphResetRequested.load(std::memory_order_acquire))
        {
            flushPendingGraphCommands();
        }
        else
        {
            const auto revision = params.revision.load(std::memory_order_acquire);
            GraphRuntime* active = audioPlane.activeGraphRaw.load(std::memory_order_acquire);
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
    juce::StringArray lines;
    lines.add("AudioEngine realtime profiling results:");
    for (const auto& r : results)
        lines.add(formatProfilingLine(r));
    return lines.joinIntoString(juce::newLine);
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
