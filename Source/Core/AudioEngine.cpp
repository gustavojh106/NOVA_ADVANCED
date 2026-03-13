#include "AudioEngine.h"
#include "PedalRegistry.h"
#include "../Effects/Pedals/Base/ProcessorBase.h"

#include <cmath>
#include <algorithm>

// ==========================================================
// IMPLEMENTACION DE AUDIO ENGINE
// ==========================================================

AudioEngine::AudioEngine()
    : juce::Thread("AudioEngineThread")
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();
    dryWetMixer.setMixingRule(juce::dsp::DryWetMixingRule::linear);
    dryWetMixer.setWetMixProportion(1.0f);
    isEngineOn = false;
    startThread(juce::Thread::Priority::high);
}

AudioEngine::~AudioEngine()
{
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

    currentRate = sampleRate;
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    numInputChannels = numIn;

    audioThreadID = {};
    consecutiveCorruptBlocks = 0;
    recoveryCooldownBlocks = 0;

    tunerService.setSampleRate(sampleRate);
    tunerService.reset();
    startupCounter = 5;

    {
        const juce::ScopedLock sl(vectorLock);

        mainGraph->suspendProcessing(true);
        mainGraph->setPlayConfigDetails(numIn, numOut, sampleRate, samplesPerBlock);
        mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

        juce::dsp::ProcessSpec dryWetSpec;
        dryWetSpec.sampleRate = sampleRate;
        dryWetSpec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
        dryWetSpec.numChannels = static_cast<juce::uint32>(juce::jmax(1, numOut));
        dryWetMixer.prepare(dryWetSpec);
        dryWetMixer.setWetMixProportion(currentGlobalMix.load());
        wetMixSmooth.reset(sampleRate, 0.02);
        wetMixSmooth.setCurrentAndTargetValue(currentGlobalMix.load());

        const bool missingNodes = (inputChainNode == nullptr ||
            stripNodeA == nullptr ||
            stripNodeB == nullptr ||
            outputChainNode == nullptr ||
            outputNode == nullptr);

        const bool graphEmpty = (inputNode == nullptr ||
            mainGraph->getNodeForId(inputNode->nodeID) == nullptr);

        if (graphEmpty || missingNodes)
        {
            mainGraph->clear();
            nodesChainA.clear();
            nodesChainB.clear();

            // 1) Hardware I/O
            inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));

            outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

            // 2) Internal processors
            inputChainNode = mainGraph->addNode(std::make_unique<InputChainProcessor>());
            stripNodeA = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());
            stripNodeB = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());

            // 3) Output chain
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
            (int)Nova::SwitcherMode::Dual_Parallel);
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
        const juce::SpinLock::ScopedLockType lock(globalParamsLock);
        pendingGlobalParams = snapshot;
    }

    globalParamsDirty = true;
}

void AudioEngine::applyPendingGlobalParams()
{
    if (!globalParamsDirty.exchange(false))
        return;

    RuntimeGlobalParams snapshot;
    {
        const juce::SpinLock::ScopedLockType lock(globalParamsLock);
        snapshot = pendingGlobalParams;
    }

    const juce::ScopedLock sl(vectorLock);
    applyGlobalParamsNow(snapshot);
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

            currentGlobalMix = mixNormalized;
            wetMixSmooth.setTargetValue(mixNormalized);
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
    dryWetMixer.setWetLatency(static_cast<float>(juce::jlimit(0,
        Nova::Config::MAX_GRAPH_LATENCY_SAMPLES,
        getLatencyNumSamples())));
}

void AudioEngine::enqueueGraphCommand(const GraphCommand& cmd, bool flushIfSafe)
{
    {
        const juce::ScopedLock sl(graphCommandLock);
        pendingGraphCommands.push_back(cmd);
    }

    if (!flushIfSafe)
        return;

    const auto currentThread = juce::Thread::getCurrentThreadId();
    if (currentThread != audioThreadID)
        flushPendingGraphCommands(true);
}

void AudioEngine::flushPendingGraphCommands(bool suspendGraph)
{
    std::deque<GraphCommand> commands;

    {
        const juce::ScopedLock sl(graphCommandLock);
        if (pendingGraphCommands.empty())
            return;

        commands.swap(pendingGraphCommands);
    }

    const juce::ScopedLock sl(vectorLock);

    if (suspendGraph && mainGraph)
        mainGraph->suspendProcessing(true);

    bool topologyChanged = false;
    bool resetRequested = false;

    for (const auto& cmd : commands)
        applyGraphCommandNow(cmd, topologyChanged, resetRequested);

    if (topologyChanged)
    {
        rebuildGraph();
        updateDryWetLatencyCompensation();
    }

    if (topologyChanged || resetRequested)
    {
        resetGraphStateNow();
        startupCounter = juce::jmax(startupCounter, 6);
    }

    if (suspendGraph && mainGraph)
        mainGraph->suspendProcessing(false);
}

void AudioEngine::applyGraphCommandNow(const GraphCommand& cmd, bool& topologyChanged, bool& resetRequested)
{
    if (mainGraph == nullptr)
        return;

    auto& chain = (cmd.chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;

    switch (cmd.type)
    {
        case GraphCommandType::AddPedal:
        {
            if (auto pedal = PedalRegistry::createPedal(cmd.pedalType))
            {
                pedal->setPlayConfigDetails(2, 2, currentSampleRate, currentBlockSize);
                pedal->prepareToPlay(currentSampleRate, currentBlockSize);

                auto node = mainGraph->addNode(std::move(pedal));
                if (node != nullptr)
                {
                    ChainNodeSlot slot;
                    slot.node = node;
                    slot.pedalID = cmd.pedalID;
                    slot.zone = cmd.zone;

                    if (cmd.index >= 0 && cmd.index <= (int)chain.size())
                        chain.insert(chain.begin() + cmd.index, std::move(slot));
                    else
                        chain.push_back(std::move(slot));

                    topologyChanged = true;
                }
            }
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
            const bool previous = isEngineOn.exchange(cmd.flag);
            if (cmd.flag && !previous)
                resetRequested = true;

            if (!cmd.flag)
                startupCounter = 0;

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

    dryWetMixer.reset();

    tunerService.reset();
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
}

double AudioEngine::getCpuLoad() const { return cpuUsage.load(); }
int AudioEngine::getLatencyNumSamples() const { return mainGraph ? mainGraph->getLatencySamples() : 0; }

void AudioEngine::setTunerEnabled(bool shouldEnable)
{
    tunerEnabled = shouldEnable;

    if (shouldEnable)
        tunerService.reset();
}

// ==========================================================
// PROCESS
// ==========================================================

bool AudioEngine::sanitizeAudioBuffer(juce::AudioBuffer<float>& buffer)
{
    bool hadInvalid = false;
    constexpr float kHardAbsLimit = 24.0f;

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

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (audioThreadID == juce::Thread::ThreadID())
        audioThreadID = juce::Thread::getCurrentThreadId();

    const auto startTime = juce::Time::getMillisecondCounterHiRes();

    // Apply queued topology/state changes at a block boundary on the audio thread.
    flushPendingGraphCommands(false);
    applyPendingGlobalParams();

    // 1) Tuner mode: capture + mute
    if (tunerEnabled)
    {
        tunerService.pushBuffer(buffer);
        buffer.clear();
        cpuUsage = 0.0;
        return;
    }

    // 2) Engine off or warming up: keep a transparent dry path.
    if (!isEngineOn || startupCounter > 0)
    {
        if (startupCounter > 0)
            --startupCounter;

        cpuUsage = 0.0;
        return;
    }

    if (mainGraph == nullptr)
    {
        buffer.clear();
        cpuUsage = 0.0;
        return;
    }

    // 3) Dry/Wet mix with latency compensation
    dryWetMixer.setWetMixProportion(wetMixSmooth.getCurrentValue());
    dryWetMixer.pushDrySamples(juce::dsp::AudioBlock<const float>(buffer));

    // 4) Process wet
    mainGraph->processBlock(buffer, midi);

    // 5) Mix final
    dryWetMixer.mixWetSamples(juce::dsp::AudioBlock<float>(buffer));

    if (wetMixSmooth.isSmoothing())
        wetMixSmooth.skip(buffer.getNumSamples());

    // 6) Safety net + auto-heal
    const bool hadCorruption = sanitizeAudioBuffer(buffer);

    if (hadCorruption)
        ++consecutiveCorruptBlocks;
    else
        consecutiveCorruptBlocks = 0;

    if (recoveryCooldownBlocks > 0)
        --recoveryCooldownBlocks;

    if (consecutiveCorruptBlocks >= 2 && recoveryCooldownBlocks == 0)
    {
        const juce::ScopedLock sl(vectorLock);
        resetGraphStateNow();

        startupCounter = juce::jmax(startupCounter, 8);
        recoveryCooldownBlocks = 256;
        consecutiveCorruptBlocks = 0;

        buffer.clear();
    }

    // 7) CPU meter
    const auto endTime = juce::Time::getMillisecondCounterHiRes();
    const double timeTakenMs = endTime - startTime;

    if (currentRate > 0.0)
    {
        const double blockDurationMs = (buffer.getNumSamples() / currentRate) * 1000.0;
        if (blockDurationMs > 0.0)
        {
            cpuUsage = (cpuUsage.load() * 0.9) +
                ((timeTakenMs / blockDurationMs) * 100.0 * 0.1);
        }
    }
}

// ==========================================================
// THREAD: TUNER ANALYSIS
// ==========================================================

void AudioEngine::run()
{
    while (!threadShouldExit())
    {
        if (tunerEnabled)
        {
            tunerService.process();
            wait(5);    // no quemar CPU
        }
        else
        {
            wait(100);  // tuner apagado => dormir mas
        }
    }
}

// ==========================================================
// LEGACY FREQUENCY DETECTION (kept as-is)
// ==========================================================

std::pair<float, float> AudioEngine::calculateFrequencyWithClarity(const float* signal, int numSamples, double sampleRate)
{
    if (sampleRate <= 0.0) return { 0.0f, 0.0f };

    int minPeriod = (int)(sampleRate / 1500.0);
    int maxPeriod = (int)(sampleRate / 40.0);
    if (maxPeriod > numSamples / 2) maxPeriod = numSamples / 2;

    float bestCorrelation = 0.0f;
    int bestPeriod = 0;

    for (int lag = minPeriod; lag < maxPeriod; ++lag)
    {
        float sum = 0.0f;
        int limit = numSamples - lag;
        for (int i = 0; i < limit; ++i) sum += signal[i] * signal[i + lag];

        float sumSq = 0.0f;
        for (int i = 0; i < limit; ++i) sumSq += signal[i] * signal[i];

        float correlation = 0.0f;
        if (sumSq > 0.00001f) correlation = sum / sumSq;

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
        float prevCorr = 0.0f;
        float nextCorr = 0.0f;

        int limitPrev = numSamples - (bestPeriod - 1);
        int limitNext = numSamples - (bestPeriod + 1);

        for (int i = 0; i < limitPrev; ++i) prevCorr += signal[i] * signal[i + (bestPeriod - 1)];
        prevCorr /= (float)limitPrev;

        for (int i = 0; i < limitNext; ++i) nextCorr += signal[i] * signal[i + (bestPeriod + 1)];
        nextCorr /= (float)limitNext;

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
