#include "AudioEngine.h"
#include "PedalRegistry.h"
#include "GlobalProcessors.h"
#include "../Effects/Pedals/Base/ProcessorBase.h"

// ==========================================================
// IMPLEMENTACION DE AUDIO ENGINE
// ==========================================================

AudioEngine::AudioEngine()
    : juce::Thread("AudioEngineThread")
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();

    // Nota: en tu codigo original lo ponias en true aqui.
    // Lo dejo igual para no cambiar tu estado inicial real.
    isEngineOn = true;

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

    tunerService.setSampleRate(sampleRate);
    tunerService.reset();
    startupCounter = 5;

    mainGraph->setPlayConfigDetails(numIn, numOut, sampleRate, samplesPerBlock);
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);
    dryBuffer.setSize(juce::jmax(1, numOut), samplesPerBlock, false, false, true);

    const bool missingNodes = (inputChainNode == nullptr ||
        stripNodeA == nullptr ||
        stripNodeB == nullptr ||
        outputChainNode == nullptr);

    const bool graphEmpty = (inputNode == nullptr ||
        mainGraph->getNodeForId(inputNode->nodeID) == nullptr);

    if (graphEmpty || missingNodes)
    {
        mainGraph->clear();

        // 1) Hardware I/O
        inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));

        outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

        // 2) Internal processors
        inputChainNode = mainGraph->addNode(std::make_unique<InputChainProcessor>());
        stripNodeA = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());
        stripNodeB = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());

        // 3) Output chain (mastering)
        outputChainNode = mainGraph->addNode(std::make_unique<OutputChainProcessor>());
    }

    // =========================================================================
    // INPUT ROUTING (input summing)
    // =========================================================================
    // 1) Remove old connections coming from hardware input
    for (auto c : mainGraph->getConnections())
        if (c.source.nodeID == inputNode->nodeID)
            mainGraph->removeConnection(c);

    // 2) Send input to both channels of InputChain
    if (inputChainNode)
    {
        mainGraph->addConnection({ { inputNode->nodeID, 0 }, { inputChainNode->nodeID, 0 } });

        if (numIn > 1)
            mainGraph->addConnection({ { inputNode->nodeID, 1 }, { inputChainNode->nodeID, 1 } });
        else
            mainGraph->addConnection({ { inputNode->nodeID, 0 }, { inputChainNode->nodeID, 1 } });
    }

    rebuildGraph();

    startThread(juce::Thread::Priority::low);
}

void AudioEngine::rebuildGraph()
{
    // Remove all non-hardware-input outgoing connections
    for (auto c : mainGraph->getConnections())
    {
        const bool isHardwareInput = (c.source.nodeID == inputNode->nodeID);
        if (!isHardwareInput)
            mainGraph->removeConnection(c);
    }

    // 1) InputChain -> Pedals -> Strip
    if (inputChainNode && stripNodeA) connectChainToGain(nodesChainA, stripNodeA->nodeID);
    if (inputChainNode && stripNodeB) connectChainToGain(nodesChainB, stripNodeB->nodeID);

    // 2) Strips -> OutputChain -> Hardware Output
    if (!outputChainNode)
        return;

    for (auto* strip : { stripNodeA.get(), stripNodeB.get() })
    {
        if (!strip) continue;

        mainGraph->addConnection({ { strip->nodeID, 0 }, { outputChainNode->nodeID, 0 } });
        mainGraph->addConnection({ { strip->nodeID, 1 }, { outputChainNode->nodeID, 1 } });
    }

    mainGraph->addConnection({ { outputChainNode->nodeID, 0 }, { outputNode->nodeID, 0 } });
    mainGraph->addConnection({ { outputChainNode->nodeID, 1 }, { outputNode->nodeID, 1 } });
}

void AudioEngine::connectChainToGain(const std::vector<juce::AudioProcessorGraph::Node::Ptr>& nodes,
    juce::AudioProcessorGraph::NodeID targetStripID)
{
    juce::AudioProcessorGraph::NodeID currentSource = inputChainNode->nodeID;

    for (const auto& node : nodes)
    {
        if (!node) continue;

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
    // 1) Input chain
    if (inputChainNode)
    {
        if (auto* p = dynamic_cast<InputChainProcessor*>(inputChainNode->getProcessor()))
        {
            const float inGain = (float)settings.getProperty(Nova::IDs::INPUT_GAIN);
            float gateThresh = (float)settings.getProperty(Nova::IDs::INPUT_GATE);
            const bool mono = (bool)settings.getProperty(Nova::IDs::FORCE_MONO);
            const int  trans = (int)settings.getProperty(Nova::IDs::INPUT_TRANS);

            if (gateThresh == 0.0f) gateThresh = -100.0f;

            p->setParams(inGain, gateThresh, mono, trans);
        }
    }

    // 2) Output chain (master)
    if (outputChainNode)
    {
        if (auto* p = dynamic_cast<OutputChainProcessor*>(outputChainNode->getProcessor()))
        {
            const float outVol = (float)settings.getProperty(Nova::IDs::OUTPUT_VOL, 0.0f);
            const float limit = (float)settings.getProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f);

            p->setParams(outVol, limit);

            // Mix slider: 0..100 -> 0..1
            const float mixPercent = (float)settings.getProperty(Nova::IDs::OUTPUT_MIX, 100.0f);
            currentGlobalMix = juce::jlimit(0.0f, 100.0f, mixPercent) / 100.0f;
        }
    }

    // 3) Strip A
    if (stripNodeA)
    {
        if (auto* p = dynamic_cast<ChannelStripProcessor*>(stripNodeA->getProcessor()))
        {
            const int mode = (int)settings.getProperty(Nova::IDs::SWITCH_MODE);
            const bool muted = (mode == (int)Nova::SwitcherMode::LineB_Only);

            float gain = muted ? 0.0f : (float)lineA.getProperty(Nova::IDs::MIXER_GAIN_A);
            const float pan = (float)lineA.getProperty(Nova::IDs::MIXER_PAN_A);
            const float width = (float)lineA.getProperty(Nova::IDs::MIXER_WIDTH_A);

            if (!muted && gain <= 0.001f) gain = 1.0f; // keep original safeguard

            p->setParams(gain, pan, width);
        }
    }

    // 4) Strip B
    if (stripNodeB)
    {
        if (auto* p = dynamic_cast<ChannelStripProcessor*>(stripNodeB->getProcessor()))
        {
            const int mode = (int)settings.getProperty(Nova::IDs::SWITCH_MODE);
            const bool muted = (mode == (int)Nova::SwitcherMode::LineA_Only);

            float gain = muted ? 0.0f : (float)lineB.getProperty(Nova::IDs::MIXER_GAIN_B);
            const float pan = (float)lineB.getProperty(Nova::IDs::MIXER_PAN_B);
            const float width = (float)lineB.getProperty(Nova::IDs::MIXER_WIDTH_B);

            if (!muted && gain <= 0.001f) gain = 1.0f;

            p->setParams(gain, pan, width);
        }
    }
}

// Legacy wrapper (tu comportamiento original: no hace nada)
void AudioEngine::updateMixer(float, float, Nova::SwitcherMode) {}

// ==========================================================
// PEDAL MANAGEMENT
// ==========================================================

void AudioEngine::addPedal(const juce::String& type, Nova::ChainID chain, int index)
{
    const juce::ScopedLock sl(vectorLock);
    mainGraph->suspendProcessing(true);

    if (auto pedal = PedalRegistry::createPedal(type))
    {
        pedal->setPlayConfigDetails(2, 2, currentSampleRate, currentBlockSize);
        pedal->prepareToPlay(currentSampleRate, currentBlockSize);

        auto node = mainGraph->addNode(std::move(pedal));
        auto& list = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;

        if (index >= 0 && index <= (int)list.size())
            list.insert(list.begin() + index, node);
        else
            list.push_back(node);

        rebuildGraph();
    }

    mainGraph->suspendProcessing(false);
}

void AudioEngine::removePedal(Nova::ChainID chain, int index)
{
    const juce::ScopedLock sl(vectorLock);
    mainGraph->suspendProcessing(true);

    auto& list = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;

    if (index >= 0 && index < (int)list.size())
    {
        mainGraph->removeNode(list[index]->nodeID);
        list.erase(list.begin() + index);
        rebuildGraph();
    }

    mainGraph->suspendProcessing(false);
}

void AudioEngine::clearAll()
{
    const juce::ScopedLock sl(vectorLock);
    mainGraph->suspendProcessing(true);

    auto removeChainNodes = [this](std::vector<juce::AudioProcessorGraph::Node::Ptr>& chain)
        {
            for (auto& node : chain)
            {
                if (node != nullptr && mainGraph->getNodeForId(node->nodeID) != nullptr)
                    mainGraph->removeNode(node->nodeID);
            }

            chain.clear();
        };

    removeChainNodes(nodesChainA);
    removeChainNodes(nodesChainB);
    rebuildGraph();

    mainGraph->suspendProcessing(false);
}

void AudioEngine::setPedalBypassed(Nova::ChainID chain, int index, bool bypassed)
{
    const juce::ScopedLock sl(vectorLock);
    auto& list = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
    if (!juce::isPositiveAndBelow(index, (int)list.size()))
        return;

    auto node = list[(size_t)index];
    if (node == nullptr || node->getProcessor() == nullptr)
        return;

    auto* processor = node->getProcessor();
    if (auto* base = dynamic_cast<ProcessorBase*>(processor))
    {
        base->setBypassed(bypassed);
        return;
    }

    // Fallback for processors not derived from ProcessorBase.
    processor->suspendProcessing(bypassed);
}

// ==========================================================
// INFO / CONTROL
// ==========================================================

const std::vector<juce::AudioProcessorGraph::Node::Ptr>& AudioEngine::getNodes(Nova::ChainID chain) const
{
    return (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
}

juce::AudioProcessor* AudioEngine::getProcessorForPedal(Nova::ChainID chain, int index)
{
    const juce::ScopedLock sl(vectorLock);

    auto& list = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
    if (!juce::isPositiveAndBelow(index, (int)list.size()))
        return nullptr;

    auto node = list[(size_t)index];
    return node != nullptr ? node->getProcessor() : nullptr;
}

void AudioEngine::setEngineEnabled(bool enabled) { isEngineOn = enabled; }
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

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const auto startTime = juce::Time::getMillisecondCounterHiRes();

    // 1) Tuner mode: capture + mute
    if (tunerEnabled)
    {
        tunerService.pushBuffer(buffer);
        buffer.clear();
        cpuUsage = 0.0;
        return;
    }

    // 2) Engine off or warming up
    if (!isEngineOn || startupCounter > 0)
    {
        if (startupCounter > 0) --startupCounter;

        buffer.clear();
        cpuUsage = 0.0;
        return;
    }

    // 3) Dry/Wet mix
    const float mix = currentGlobalMix.load();
    const bool isMixRequested = (mix < 0.99f);
    const bool hasDryCapacity = dryBuffer.getNumChannels() >= buffer.getNumChannels()
        && dryBuffer.getNumSamples() >= buffer.getNumSamples();
    const bool isMixActive = isMixRequested && hasDryCapacity;

    if (isMixActive)
    {
        const int numSamples = buffer.getNumSamples();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* src = buffer.getReadPointer(ch);
            auto* dryCh = dryBuffer.getWritePointer(ch);
            juce::FloatVectorOperations::copy(dryCh, src, numSamples);
        }
    }

    // 4) Process wet
    mainGraph->processBlock(buffer, midi);

    // 5) Mix final
    if (isMixActive)
    {
        const int numSamples = buffer.getNumSamples();
        const float dryMix = 1.0f - mix;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* wetData = buffer.getWritePointer(ch);
            const int dryChIndex = (ch < dryBuffer.getNumChannels()) ? ch : 0;
            const auto* dryData = dryBuffer.getReadPointer(dryChIndex);

            for (int i = 0; i < numSamples; ++i)
                wetData[i] = (wetData[i] * mix) + (dryData[i] * dryMix);
        }
    }

    // 6) CPU meter
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
