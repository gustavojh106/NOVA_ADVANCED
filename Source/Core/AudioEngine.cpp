#include "AudioEngine.h"
#include "PedalRegistry.h"
#include "SessionLogger.h"
#include "../Effects/Pedals/Base/ProcessorBase.h"

#include <cmath>
#include <algorithm>

namespace
{
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
        case Nova::SwitcherMode::LineA_Only:  return "LineA_Only";
        case Nova::SwitcherMode::LineB_Only:  return "LineB_Only";
        case Nova::SwitcherMode::Dual_Parallel: return "Dual_Parallel";
        default: return "Unknown";
    }
}

juce::String formatRuntimeParams(const AudioEngine::RuntimeGlobalParams& snapshot)
{
    juce::String result;
    result << "inputGainDb=" << snapshot.inputGainDb
        << ", gateThresholdDb=" << snapshot.gateThresholdDb
        << ", forceMono=" << boolToText(snapshot.forceMono)
        << ", inputTranspose=" << snapshot.inputTranspose
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
}

// ==========================================================
// IMPLEMENTACION DE AUDIO ENGINE
// ==========================================================

AudioEngine::AudioEngine()
    : juce::Thread("AudioEngineThread")
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();
    audioPlane.dryWetMixer.setMixingRule(juce::dsp::DryWetMixingRule::linear);
    audioPlane.dryWetMixer.setWetMixProportion(1.0f);
    audioPlane.isEngineOn = false;
    NovaDiagnostics::SessionLogger::logEvent("engine.lifecycle", "AudioEngine constructed");
    startThread(juce::Thread::Priority::high);
}

AudioEngine::~AudioEngine()
{
    NovaDiagnostics::SessionLogger::logEvent("engine.lifecycle", "AudioEngine shutting down");
    cancelPendingUpdate();
    stopThread(4000);

    if (mainGraph)
    {
        mainGraph->releaseResources();
        mainGraph->clear();
    }
}

void AudioEngine::prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut)
{
    stopThread(5000);

    audioPlane.currentRate = sampleRate;
    audioPlane.currentSampleRate = sampleRate;
    audioPlane.currentBlockSize = samplesPerBlock;
    audioPlane.numInputChannels = numIn;

    audioPlane.audioThreadID = {};
    audioPlane.consecutiveCorruptBlocks = 0;
    audioPlane.recoveryCooldownBlocks = 0;
    controlPlane.appliedGlobalParamsRevision.store(0, std::memory_order_relaxed);

    audioPlane.tunerService.setSampleRate(sampleRate);
    audioPlane.tunerService.reset();
    audioPlane.startupCounter = Nova::Config::STARTUP_COUNTER_INIT;
    audioPlane.silentOutputBlockCounter.store(0, std::memory_order_relaxed);
    audioPlane.silentOutputIncidentActive.store(false, std::memory_order_relaxed);
    audioPlane.pendingSilentOutputLog.store(false, std::memory_order_relaxed);
    audioPlane.pendingSilentOutputRecoveryLog.store(false, std::memory_order_relaxed);
    audioPlane.pendingAutoHealLog.store(false, std::memory_order_relaxed);

    {
        const juce::ScopedLock sl(vectorLock);

        mainGraph->suspendProcessing(true);
        mainGraph->setPlayConfigDetails(numIn, numOut, sampleRate, samplesPerBlock);
        mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

        juce::dsp::ProcessSpec dryWetSpec;
        dryWetSpec.sampleRate = sampleRate;
        dryWetSpec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
        dryWetSpec.numChannels = static_cast<juce::uint32>(juce::jmax(1, numOut));
        audioPlane.dryWetMixer.prepare(dryWetSpec);
        audioPlane.dryWetMixer.setWetMixProportion(audioPlane.currentGlobalMix.load());
        audioPlane.wetMixSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        audioPlane.wetMixSmooth.setCurrentAndTargetValue(audioPlane.currentGlobalMix.load());

        const bool missingNodes = (inputChainNode == nullptr ||
            stripNodeA == nullptr ||
            stripNodeB == nullptr ||
            outputChainNode == nullptr ||
            outputNode == nullptr);

        const bool graphEmpty = (inputNode == nullptr ||
            mainGraph->getNodeForId(inputNode->nodeID) == nullptr);

        if (graphEmpty || missingNodes)
        {
            const bool preservePedalTopology = (!nodesChainA.empty() || !nodesChainB.empty());

            if (!preservePedalTopology)
            {
                mainGraph->clear();
                nodesChainA.clear();
                nodesChainB.clear();
            }

            // 1) Hardware I/O
            if (inputNode == nullptr || mainGraph->getNodeForId(inputNode->nodeID) == nullptr)
            {
                inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                    juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
            }

            if (outputNode == nullptr || mainGraph->getNodeForId(outputNode->nodeID) == nullptr)
            {
                outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                    juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
            }

            // 2) Internal processors
            if (inputChainNode == nullptr || mainGraph->getNodeForId(inputChainNode->nodeID) == nullptr)
                inputChainNode = mainGraph->addNode(std::make_unique<InputChainProcessor>());

            if (stripNodeA == nullptr || mainGraph->getNodeForId(stripNodeA->nodeID) == nullptr)
                stripNodeA = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());

            if (stripNodeB == nullptr || mainGraph->getNodeForId(stripNodeB->nodeID) == nullptr)
                stripNodeB = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());

            // 3) Output chain
            if (outputChainNode == nullptr || mainGraph->getNodeForId(outputChainNode->nodeID) == nullptr)
                outputChainNode = mainGraph->addNode(std::make_unique<OutputChainProcessor>());
        }

        // INPUT ROUTING: remove old connections from hardware input.
        juce::Array<juce::AudioProcessorGraph::Connection> toRemove;
        for (const auto& c : mainGraph->getConnections())
            if (inputNode != nullptr && c.source.nodeID == inputNode->nodeID)
                toRemove.add(c);

        for (const auto& c : toRemove)
            mainGraph->removeConnection(c);

        // Send input to both channels of InputChain.
        if (inputNode != nullptr && inputChainNode != nullptr)
        {
            mainGraph->addConnection({ { inputNode->nodeID, 0 }, { inputChainNode->nodeID, 0 } });

            if (numIn > 1)
                mainGraph->addConnection({ { inputNode->nodeID, 1 }, { inputChainNode->nodeID, 1 } });
            else
                mainGraph->addConnection({ { inputNode->nodeID, 0 }, { inputChainNode->nodeID, 1 } });
        }

        rebuildGraph();
        updateDryWetLatencyCompensation();
        resetGraphStateNow();

        mainGraph->suspendProcessing(false);
    }

    applyPendingGlobalParams();
    NovaDiagnostics::SessionLogger::logEvent("engine.prepare",
        "Prepared engine: sampleRate=" + juce::String(sampleRate)
        + ", blockSize=" + juce::String(samplesPerBlock)
        + ", inputs=" + juce::String(numIn)
        + ", outputs=" + juce::String(numOut)
        + juce::newLine + buildDiagnosticReport());
    startThread(juce::Thread::Priority::low);
}

void AudioEngine::rebuildGraph()
{
    if (mainGraph == nullptr)
        return;

    juce::Array<juce::AudioProcessorGraph::Connection> toRemove;

    for (const auto& c : mainGraph->getConnections())
    {
        const bool isHardwareInput = (inputNode != nullptr && c.source.nodeID == inputNode->nodeID);
        if (!isHardwareInput)
            toRemove.add(c);
    }

    for (const auto& c : toRemove)
        mainGraph->removeConnection(c);

    // 1) InputChain -> Pedals -> Strip
    if (inputChainNode && stripNodeA)
        connectChainToGain(nodesChainA, stripNodeA->nodeID);

    if (inputChainNode && stripNodeB)
        connectChainToGain(nodesChainB, stripNodeB->nodeID);

    // 2) Strips -> OutputChain -> Hardware Output
    if (!outputChainNode || !outputNode)
        return;

    for (auto* strip : { stripNodeA.get(), stripNodeB.get() })
    {
        if (strip == nullptr)
            continue;

        mainGraph->addConnection({ { strip->nodeID, 0 }, { outputChainNode->nodeID, 0 } });
        mainGraph->addConnection({ { strip->nodeID, 1 }, { outputChainNode->nodeID, 1 } });
    }

    mainGraph->addConnection({ { outputChainNode->nodeID, 0 }, { outputNode->nodeID, 0 } });
    mainGraph->addConnection({ { outputChainNode->nodeID, 1 }, { outputNode->nodeID, 1 } });
}

void AudioEngine::connectChainToGain(const std::vector<ChainNodeSlot>& nodes,
    juce::AudioProcessorGraph::NodeID targetStripID)
{
    if (mainGraph == nullptr || inputChainNode == nullptr)
        return;

    if (mainGraph->getNodeForId(targetStripID) == nullptr)
        return;

    juce::AudioProcessorGraph::NodeID currentSource = inputChainNode->nodeID;

    std::vector<size_t> orderedIndices;
    orderedIndices.reserve(nodes.size());

    for (size_t i = 0; i < nodes.size(); ++i)
        orderedIndices.push_back(i);

    const auto zoneRank = [](Nova::ZoneID zone) noexcept
    {
        switch (zone)
        {
            case Nova::ZoneID::Pre:     return 0;
            case Nova::ZoneID::Amp:     return 1;
            case Nova::ZoneID::FX:      return 2;
            case Nova::ZoneID::Cabinet: return 3;
            default:                    return 4;
        }
    };

    std::stable_sort(orderedIndices.begin(), orderedIndices.end(),
        [&nodes, &zoneRank](size_t lhs, size_t rhs)
        {
            return zoneRank(nodes[lhs].zone) < zoneRank(nodes[rhs].zone);
        });

    for (const auto index : orderedIndices)
    {
        const auto& slot = nodes[index];
        const auto& node = slot.node;

        if (!node)
            continue;

        if (mainGraph->getNodeForId(node->nodeID) == nullptr)
            continue;

        mainGraph->addConnection({ { currentSource, 0 }, { node->nodeID, 0 } });
        mainGraph->addConnection({ { currentSource, 1 }, { node->nodeID, 1 } });
        currentSource = node->nodeID;
    }

    mainGraph->addConnection({ { currentSource, 0 }, { targetStripID, 0 } });
    mainGraph->addConnection({ { currentSource, 1 }, { targetStripID, 1 } });
}

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
        snapshot.inputTranspose = (int)settings.getProperty(Nova::IDs::INPUT_TRANS, 0);

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
    {
        const juce::SpinLock::ScopedLockType lock(controlPlane.globalParamsLock);
        controlPlane.pendingGlobalParams = snapshot;
    }

    controlPlane.globalParamsRevision.fetch_add(1, std::memory_order_release);
}

juce::String AudioEngine::describeChainState(const std::vector<ChainNodeSlot>& chain) const
{
    juce::StringArray lines;

    for (size_t i = 0; i < chain.size(); ++i)
    {
        const auto& slot = chain[i];
        juce::String line;
        line << "#" << (int)i
            << " zone=" << zoneToText(slot.zone)
            << " pedalID=" << (slot.pedalID.isEmpty() ? "<none>" : slot.pedalID);

        if (slot.node == nullptr || slot.node->getProcessor() == nullptr)
        {
            line << " processor=<null>";
            lines.add(line);
            continue;
        }

        auto* processor = slot.node->getProcessor();
        line << " processor=" << processor->getName()
            << " suspended=" << boolToText(processor->isSuspended());

        if (auto* base = dynamic_cast<ProcessorBase*>(processor))
            line << " bypassed=" << boolToText(base->getBypassed());

        lines.add(line);
    }

    if (lines.isEmpty())
        lines.add("<empty>");

    return lines.joinIntoString(juce::newLine);
}

juce::String AudioEngine::buildDiagnosticReport() const
{
    RuntimeGlobalParams params;
    {
        const juce::SpinLock::ScopedLockType lock(controlPlane.globalParamsLock);
        params = controlPlane.pendingGlobalParams;
    }

    const juce::ScopedLock sl(vectorLock);

    juce::String report;
    report << "engineOn=" << boolToText(audioPlane.isEngineOn.load())
        << ", tunerEnabled=" << boolToText(audioPlane.tunerEnabled.load())
        << ", sampleRate=" << audioPlane.currentSampleRate
        << ", blockSize=" << audioPlane.currentBlockSize
        << ", inputChannels=" << audioPlane.numInputChannels
        << ", startupCounter=" << audioPlane.startupCounter
        << ", graphLatencySamples=" << getLatencyNumSamples()
        << ", wetMix=" << audioPlane.currentGlobalMix.load()
        << ", cpuLoad=" << audioPlane.cpuUsage.load()
        << ", pendingGlobalRevision=" << (int)controlPlane.globalParamsRevision.load()
        << ", appliedGlobalRevision=" << (int)controlPlane.appliedGlobalParamsRevision.load()
        << ", consecutiveCorruptBlocks=" << audioPlane.consecutiveCorruptBlocks
        << ", recoveryCooldownBlocks=" << audioPlane.recoveryCooldownBlocks
        << ", lastInputPeak=" << audioPlane.lastInputPeak.load()
        << ", lastOutputPeak=" << audioPlane.lastOutputPeak.load()
        << juce::newLine
        << "runtimeParams: " << formatRuntimeParams(params) << juce::newLine
        << "chainA:" << juce::newLine << describeChainState(nodesChainA) << juce::newLine
        << "chainB:" << juce::newLine << describeChainState(nodesChainB);

    return report;
}

void AudioEngine::applyPendingGlobalParams()
{
    const auto pendingRevision = controlPlane.globalParamsRevision.load(std::memory_order_acquire);
    if (pendingRevision == controlPlane.appliedGlobalParamsRevision.load(std::memory_order_relaxed))
        return;

    RuntimeGlobalParams snapshot;
    uint32_t capturedRevision = pendingRevision;
    const bool runningOnAudioThread = (audioPlane.audioThreadID != juce::Thread::ThreadID()
        && juce::Thread::getCurrentThreadId() == audioPlane.audioThreadID);

    {
        bool acquired = false;
        if (runningOnAudioThread)
        {
            // Try up to 3 times with a brief spin to avoid silently dropping updates
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                if (controlPlane.globalParamsLock.tryEnter())
                {
                    acquired = true;
                    break;
                }
                // Minimal spin: yield to the other thread holding the lock
                for (volatile int spin = 0; spin < 32; ++spin) {}
            }

            if (!acquired)
                return; // Still contended — will catch up next block
        }
        else
        {
            controlPlane.globalParamsLock.enter();
            acquired = true;
        }

        snapshot = controlPlane.pendingGlobalParams;
        capturedRevision = controlPlane.globalParamsRevision.load(std::memory_order_relaxed);
        controlPlane.globalParamsLock.exit();
    }

    applyGlobalParamsNow(snapshot);
    controlPlane.appliedGlobalParamsRevision.store(capturedRevision, std::memory_order_release);
}

void AudioEngine::applyGlobalParamsNow(const RuntimeGlobalParams& snapshot)
{
    if (inputChainNode)
    {
        if (auto* p = dynamic_cast<InputChainProcessor*>(inputChainNode->getProcessor()))
        {
            float gateThreshold = snapshot.gateThresholdDb;
            if (gateThreshold == 0.0f)
                gateThreshold = -100.0f;

            p->setParams(snapshot.inputGainDb,
                gateThreshold,
                snapshot.forceMono,
                snapshot.inputTranspose);
        }
    }

    if (outputChainNode)
    {
        if (auto* p = dynamic_cast<OutputChainProcessor*>(outputChainNode->getProcessor()))
        {
            p->setParams(snapshot.outputVolumeDb, snapshot.outputLimiterDb);

            // Backward-compatible normalization:
            // older presets may carry 0..1, current UI stores 0..100.
            const float mixRaw = snapshot.outputMixRaw;
            const float mixNormalized = (mixRaw <= 1.0f)
                ? juce::jlimit(0.0f, 1.0f, mixRaw)
                : juce::jlimit(0.0f, 100.0f, mixRaw) / 100.0f;

            audioPlane.currentGlobalMix = mixNormalized;
            audioPlane.wetMixSmooth.setTargetValue(mixNormalized);
        }
    }

    const bool muteA = (snapshot.switchMode == (int)Nova::SwitcherMode::LineB_Only);
    const bool muteB = (snapshot.switchMode == (int)Nova::SwitcherMode::LineA_Only);
    const bool dualParallel = (snapshot.switchMode == (int)Nova::SwitcherMode::Dual_Parallel);
    constexpr float kParallelGainComp = 0.5f; // Unity amplitude when both lines carry the same source

    if (stripNodeA)
    {
        if (auto* p = dynamic_cast<ChannelStripProcessor*>(stripNodeA->getProcessor()))
        {
            float gain = muteA ? 0.0f : snapshot.gainA;
            if (!muteA && gain <= 0.001f)
                gain = 1.0f;
            if (!muteA && dualParallel)
                gain *= kParallelGainComp;

            p->setParams(gain, snapshot.panA, snapshot.widthA);
        }
    }

    if (stripNodeB)
    {
        if (auto* p = dynamic_cast<ChannelStripProcessor*>(stripNodeB->getProcessor()))
        {
            float gain = muteB ? 0.0f : snapshot.gainB;
            if (!muteB && gain <= 0.001f)
                gain = 1.0f;
            if (!muteB && dualParallel)
                gain *= kParallelGainComp;

            p->setParams(gain, snapshot.panB, snapshot.widthB);
        }
    }
}

void AudioEngine::updateDryWetLatencyCompensation()
{
    audioPlane.dryWetMixer.setWetLatency(static_cast<float>(juce::jlimit(0,
        Nova::Config::MAX_GRAPH_LATENCY_SAMPLES,
        getLatencyNumSamples())));
}

void AudioEngine::enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe)
{
    {
        const juce::ScopedLock sl(controlPlane.graphCommandLock);
        controlPlane.pendingGraphCommands.push_back(cmd);
        controlPlane.graphCommandsPending.store(true, std::memory_order_release);
    }

    juce::String message;
    message << "type=" << static_cast<int>(cmd.type)
        << ", chain=" << (cmd.chain == Nova::ChainID::LineA ? "A" : "B")
        << ", index=" << cmd.index
        << ", zone=" << zoneToText(cmd.zone)
        << ", pedalType=" << cmd.pedalType
        << ", pedalID=" << cmd.pedalID
        << ", flag=" << boolToText(cmd.flag);
    NovaDiagnostics::SessionLogger::logEvent("engine.graph.enqueue", message);

    if (flushIfSafe)
    {
        auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();

        if (messageManager != nullptr && messageManager->isThisTheMessageThread())
            flushPendingGraphCommands(true);
        else
            triggerAsyncUpdate();
    }
}

void AudioEngine::flushPendingGraphCommands(bool suspendGraph)
{
    if (!controlPlane.graphCommandsPending.load(std::memory_order_acquire))
        return;

    const bool runningOnAudioThread = (!suspendGraph
        && audioPlane.audioThreadID != juce::Thread::ThreadID()
        && juce::Thread::getCurrentThreadId() == audioPlane.audioThreadID);

    std::deque<GraphCommand> commands;

    {
        const bool hasGraphLock = runningOnAudioThread ? controlPlane.graphCommandLock.tryEnter() : (controlPlane.graphCommandLock.enter(), true);
        if (!hasGraphLock)
            return;

        if (controlPlane.pendingGraphCommands.empty())
        {
            controlPlane.graphCommandsPending.store(false, std::memory_order_release);
            controlPlane.graphCommandLock.exit();
            return;
        }

        commands.swap(controlPlane.pendingGraphCommands);
        controlPlane.graphCommandsPending.store(!controlPlane.pendingGraphCommands.empty(), std::memory_order_release);
        controlPlane.graphCommandLock.exit();
    }

    const bool hasVectorLock = runningOnAudioThread ? vectorLock.tryEnter() : (vectorLock.enter(), true);
    if (!hasVectorLock)
    {
        const juce::ScopedLock sl(controlPlane.graphCommandLock);
        for (auto it = commands.rbegin(); it != commands.rend(); ++it)
            controlPlane.pendingGraphCommands.push_front(std::move(*it));

        controlPlane.graphCommandsPending.store(true, std::memory_order_release);
        return;
    }

    if (suspendGraph && mainGraph)
        mainGraph->suspendProcessing(true);

    bool topologyChanged = false;
    bool renderSequenceInvalidated = false;
    bool resetRequested = false;

    for (const auto& cmd : commands)
        applyGraphCommandNow(cmd, topologyChanged, renderSequenceInvalidated, resetRequested);

    if (topologyChanged)
        rebuildGraph();

    if (topologyChanged || renderSequenceInvalidated)
    {
        mainGraph->rebuild();
        updateDryWetLatencyCompensation();
    }

    if (topologyChanged || renderSequenceInvalidated || resetRequested)
    {
        resetGraphStateNow();
        audioPlane.startupCounter = juce::jmax(audioPlane.startupCounter, Nova::Config::STARTUP_COUNTER_GRAPH_CHANGE);
    }

    if (suspendGraph && mainGraph)
        mainGraph->suspendProcessing(false);

    vectorLock.exit();
}

void AudioEngine::applyGraphCommandNow(const GraphCommand& cmd,
    bool& topologyChanged,
    bool& renderSequenceInvalidated,
    bool& resetRequested)
{
    if (mainGraph == nullptr)
        return;

    auto& chain = (cmd.chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;

    switch (cmd.type)
    {
        case GraphCommandType::AddPedal:
        {
            auto pedal = PedalRegistry::createPedal(cmd.pedalType);
            if (!pedal)
            {
                NovaDiagnostics::SessionLogger::logEvent("engine.addPedal.failed",
                    "PedalRegistry::createPedal returned nullptr for type=\"" + cmd.pedalType
                    + "\", pedalID=" + cmd.pedalID);
                break;
            }

            auto node = mainGraph->addNode(std::move(pedal));
            if (node == nullptr)
            {
                NovaDiagnostics::SessionLogger::logEvent("engine.addPedal.failed",
                    "mainGraph->addNode returned nullptr for type=\"" + cmd.pedalType
                    + "\", pedalID=" + cmd.pedalID);
                break;
            }

            ChainNodeSlot slot;
            slot.node = node;
            slot.pedalID = cmd.pedalID;
            slot.zone = cmd.zone;

            if (cmd.index >= 0 && cmd.index <= (int)chain.size())
                chain.insert(chain.begin() + cmd.index, std::move(slot));
            else
                chain.push_back(std::move(slot));

            topologyChanged = true;
            renderSequenceInvalidated = true;
            break;
        }

        case GraphCommandType::RemovePedal:
        {
            if (cmd.index >= 0 && cmd.index < (int)chain.size())
            {
                auto node = chain[(size_t)cmd.index].node;
                if (node != nullptr && mainGraph->getNodeForId(node->nodeID) != nullptr)
                    mainGraph->removeNode(node->nodeID);

                chain.erase(chain.begin() + cmd.index);
                topologyChanged = true;
                renderSequenceInvalidated = true;
            }
            break;
        }

        case GraphCommandType::MovePedal:
        {
            const int from = cmd.index;
            const int to = cmd.toIndex;

            if (from >= 0 && from < (int)chain.size()
                && to >= 0 && to < (int)chain.size()
                && from != to)
            {
                auto slot = std::move(chain[(size_t)from]);
                chain.erase(chain.begin() + from);

                int adjustedTo = to;
                if (from < to)
                    adjustedTo--;

                adjustedTo = juce::jlimit(0, (int)chain.size(), adjustedTo);
                chain.insert(chain.begin() + adjustedTo, std::move(slot));
                topologyChanged = true;
                renderSequenceInvalidated = true;
            }
            break;
        }

        case GraphCommandType::ClearAll:
        {
            auto removeChain = [this](std::vector<ChainNodeSlot>& list)
                {
                    for (auto& node : list)
                    {
                        if (node.node != nullptr && mainGraph->getNodeForId(node.node->nodeID) != nullptr)
                            mainGraph->removeNode(node.node->nodeID);
                    }

                    list.clear();
                };

            removeChain(nodesChainA);
            removeChain(nodesChainB);
            topologyChanged = true;
            renderSequenceInvalidated = true;
            break;
        }

        case GraphCommandType::SetPedalBypass:
        {
            if (!juce::isPositiveAndBelow(cmd.index, (int)chain.size()))
                break;

            auto node = chain[(size_t)cmd.index];
            if (node.node == nullptr || node.node->getProcessor() == nullptr)
                break;

            auto* processor = node.node->getProcessor();
            if (auto* base = dynamic_cast<ProcessorBase*>(processor))
            {
                base->setBypassed(cmd.flag);
                renderSequenceInvalidated = true;
            }
            else
            {
                processor->suspendProcessing(cmd.flag);
                if (!cmd.flag)
                    processor->reset();
            }
            break;
        }

        case GraphCommandType::SetEngineEnabled:
        {
            const bool previous = audioPlane.isEngineOn.exchange(cmd.flag);
            if (cmd.flag && !previous)
                resetRequested = true;

            if (!cmd.flag)
                audioPlane.startupCounter = 0;

            break;
        }

        default:
            break;
    }
}

void AudioEngine::resetGraphStateNow()
{
    auto resetNode = [](juce::AudioProcessorGraph::Node::Ptr& node)
        {
            if (node != nullptr && node->getProcessor() != nullptr)
                node->getProcessor()->reset();
        };

    resetNode(inputChainNode);
    resetNode(stripNodeA);
    resetNode(stripNodeB);
    resetNode(outputChainNode);

    for (auto& n : nodesChainA)
        resetNode(n.node);

    for (auto& n : nodesChainB)
        resetNode(n.node);

    audioPlane.dryWetMixer.reset();

    audioPlane.tunerService.reset();
}

// Legacy wrapper kept for compatibility.
void AudioEngine::updateMixer(float, float, Nova::SwitcherMode) {}

// ==========================================================
// PEDAL MANAGEMENT
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

void AudioEngine::synchronizeProcessingState()
{
    if (controlPlane.graphCommandsPending.load(std::memory_order_acquire))
    {
        auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();

        if (messageManager == nullptr || messageManager->isThisTheMessageThread())
            flushPendingGraphCommands(true);
        else
            triggerAsyncUpdate();
    }

    applyPendingGlobalParams();
}

// ==========================================================
// INFO / CONTROL
// ==========================================================

std::vector<AudioEngine::ChainNodeView> AudioEngine::getNodes(Nova::ChainID chain) const
{
    const juce::ScopedLock sl(vectorLock);
    const auto& source = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
    std::vector<ChainNodeView> result;
    result.reserve(source.size());

    for (const auto& slot : source)
        result.push_back({ slot.node, slot.pedalID, slot.zone });

    return result;
}

juce::AudioProcessor* AudioEngine::getProcessorForPedal(Nova::ChainID chain, int index)
{
    const juce::ScopedLock sl(vectorLock);

    auto& list = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
    if (!juce::isPositiveAndBelow(index, (int)list.size()))
        return nullptr;

    auto node = list[(size_t)index].node;
    if (node == nullptr || mainGraph->getNodeForId(node->nodeID) == nullptr)
        return nullptr;

    return node->getProcessor();
}

void AudioEngine::setEngineEnabled(bool enabled)
{
    GraphCommand cmd;
    cmd.type = GraphCommandType::SetEngineEnabled;
    cmd.flag = enabled;
    enqueueGraphCommand(cmd, true);
    NovaDiagnostics::SessionLogger::logEvent("engine.state",
        "Requested engineEnabled=" + boolToText(enabled));
}

double AudioEngine::getCpuLoad() const { return audioPlane.cpuUsage.load(); }
int AudioEngine::getLatencyNumSamples() const { return mainGraph ? mainGraph->getLatencySamples() : 0; }

void AudioEngine::setTunerEnabled(bool shouldEnable)
{
    audioPlane.tunerEnabled = shouldEnable;

    if (shouldEnable)
        audioPlane.tunerService.reset();
}

// ==========================================================
// PROCESS
// ==========================================================

bool AudioEngine::sanitizeAudioBuffer(juce::AudioBuffer<float>& buffer)
{
    bool hadInvalid = false;
    constexpr float kHardAbsLimit = Nova::Config::HARD_ABS_LIMIT_LINEAR;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        const int numSamples = buffer.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            float v = data[i];

            if (!std::isfinite(v))
            {
                data[i] = 0.0f;
                hadInvalid = true;
                continue;
            }

            if (std::abs(v) > kHardAbsLimit)
            {
                data[i] = juce::jlimit(-kHardAbsLimit, kHardAbsLimit, v);
                hadInvalid = true;
            }
        }
    }

    return hadInvalid;
}

float AudioEngine::measureBlockPeak(const juce::AudioBuffer<float>& buffer)
{
    float peak = 0.0f;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));

    return peak;
}

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (audioPlane.audioThreadID == juce::Thread::ThreadID())
        audioPlane.audioThreadID = juce::Thread::getCurrentThreadId();

    const auto startTime = juce::Time::getMillisecondCounterHiRes();
    const float inputPeak = measureBlockPeak(buffer);
    audioPlane.lastInputPeak.store(inputPeak, std::memory_order_relaxed);

    // Graph mutations are committed on the message thread so JUCE can rebuild the
    // render sequence synchronously with latency/layout changes.
    applyPendingGlobalParams();

    // 1) Tuner mode: capture + mute
    if (audioPlane.tunerEnabled)
    {
        audioPlane.tunerService.pushBuffer(buffer);
        buffer.clear();
        audioPlane.cpuUsage = 0.0;
        return;
    }

    // 2) Engine off or warming up: keep a transparent dry path.
    if (!audioPlane.isEngineOn || audioPlane.startupCounter > 0)
    {
        if (audioPlane.startupCounter > 0)
            --audioPlane.startupCounter;

        audioPlane.cpuUsage = 0.0;
        return;
    }

    if (mainGraph == nullptr)
    {
        buffer.clear();
        audioPlane.cpuUsage = 0.0;
        return;
    }

    // 3) Dry/Wet mix with latency compensation
    audioPlane.dryWetMixer.setWetMixProportion(audioPlane.wetMixSmooth.getCurrentValue());
    audioPlane.dryWetMixer.pushDrySamples(juce::dsp::AudioBlock<const float>(buffer));

    // 4) Process wet
    mainGraph->processBlock(buffer, midi);

    // 5) Mix final
    audioPlane.dryWetMixer.mixWetSamples(juce::dsp::AudioBlock<float>(buffer));

    if (audioPlane.wetMixSmooth.isSmoothing())
        audioPlane.wetMixSmooth.skip(buffer.getNumSamples());

    // 6) Safety net + auto-heal
    const bool hadCorruption = sanitizeAudioBuffer(buffer);
    const float outputPeak = measureBlockPeak(buffer);
    audioPlane.lastOutputPeak.store(outputPeak, std::memory_order_relaxed);

    if (audioPlane.isEngineOn.load() && !audioPlane.tunerEnabled.load() && audioPlane.startupCounter == 0 && audioPlane.currentGlobalMix.load() > 0.01f)
    {
        const bool suspiciousSilence = (inputPeak >= Nova::Config::INPUT_ACTIVE_THRESHOLD
                                        && outputPeak <= Nova::Config::OUTPUT_SILENT_THRESHOLD);

        if (suspiciousSilence)
        {
            const int blocks = audioPlane.silentOutputBlockCounter.fetch_add(1, std::memory_order_relaxed) + 1;
            const int triggerBlocks = juce::jmax(8, juce::roundToInt(
                (audioPlane.currentRate / juce::jmax(1, audioPlane.currentBlockSize)) * Nova::Config::SILENT_OUTPUT_TRIGGER_SECONDS));

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
    else
    {
        audioPlane.silentOutputBlockCounter.store(0, std::memory_order_relaxed);
    }

    if (hadCorruption)
        ++audioPlane.consecutiveCorruptBlocks;
    else
        audioPlane.consecutiveCorruptBlocks = 0;

    if (audioPlane.recoveryCooldownBlocks > 0)
        --audioPlane.recoveryCooldownBlocks;

    if (audioPlane.consecutiveCorruptBlocks >= Nova::Config::CORRUPT_BLOCKS_BEFORE_HEAL && audioPlane.recoveryCooldownBlocks == 0)
    {
        if (vectorLock.tryEnter())
        {
            resetGraphStateNow();
            vectorLock.exit();
            audioPlane.startupCounter = juce::jmax(audioPlane.startupCounter, Nova::Config::STARTUP_COUNTER_AUTO_HEAL);
            audioPlane.recoveryCooldownBlocks = Nova::Config::RECOVERY_COOLDOWN_BLOCKS;
            audioPlane.autoHealCount.fetch_add(1, std::memory_order_relaxed);
            audioPlane.pendingAutoHealLog.store(true, std::memory_order_release);
        }
        else
        {
            audioPlane.recoveryCooldownBlocks = Nova::Config::RECOVERY_COOLDOWN_SHORT;
        }

        audioPlane.consecutiveCorruptBlocks = 0;

        buffer.clear();
    }

    // 7) CPU meter
    const auto endTime = juce::Time::getMillisecondCounterHiRes();
    const double timeTakenMs = endTime - startTime;

    if (audioPlane.currentRate > 0.0)
    {
        const double blockDurationMs = (buffer.getNumSamples() / audioPlane.currentRate) * 1000.0;
        if (blockDurationMs > 0.0)
        {
            audioPlane.cpuUsage = (audioPlane.cpuUsage.load() * Nova::Config::CPU_METER_SLOW_COEFF) +
                ((timeTakenMs / blockDurationMs) * 100.0 * Nova::Config::CPU_METER_FAST_COEFF);
        }
    }
}

void AudioEngine::handleAsyncUpdate()
{
    flushPendingGraphCommands(true);
}

// ==========================================================
// THREAD: TUNER ANALYSIS
// ==========================================================

void AudioEngine::run()
{
    while (!threadShouldExit())
    {
        if (controlPlane.graphCommandsPending.load(std::memory_order_acquire)
            || controlPlane.globalParamsRevision.load(std::memory_order_acquire) != controlPlane.appliedGlobalParamsRevision.load(std::memory_order_relaxed))
        {
            synchronizeProcessingState();
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
                "Sanitizer triggered graph reset after corrupt audio detection."
                + juce::newLine + buildDiagnosticReport());
        }

        if (audioPlane.tunerEnabled)
        {
            audioPlane.tunerService.process();
            wait(5);
        }
        else
        {
            wait(40);
        }
    }
}

// ==========================================================
// LEGACY FREQUENCY DETECTION (kept as-is)
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
