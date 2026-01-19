#include "AudioEngine.h"
#include "PedalRegistry.h"
#include "GlobalProcessors.h" 

// ==========================================================
// IMPLEMENTACIÓN DE AUDIO ENGINE
// ==========================================================

AudioEngine::AudioEngine()
    : juce::Thread("AudioEngineThread")
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();

    tunerCircularBuffer.resize(TUNER_FIFO_SIZE, 0.0f);
    tunerWorkBuffer.resize(TUNER_PROCESS_SIZE, 0.0f);

    isEngineOn = true;

    startThread(juce::Thread::Priority::high);
}

AudioEngine::~AudioEngine()
{
    stopThread(4000);
    mainGraph->releaseResources();
    mainGraph->clear();
}

void AudioEngine::prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut)
{
    stopThread(5000);

    currentRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    currentSampleRate = sampleRate;
    numInputChannels = numIn;

    // Reset Tuner
    tunerCircularBuffer.assign(TUNER_FIFO_SIZE, 0.0f);
    tunerWorkBuffer.assign(TUNER_PROCESS_SIZE, 0.0f);
    tunerFifo.setTotalSize(TUNER_FIFO_SIZE);
    tunerFifo.reset();

    currentRMS = 0.0f;
    startupCounter = 5;

    mainGraph->setPlayConfigDetails(numIn, numOut, sampleRate, samplesPerBlock);
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

    // --- RECONSTRUCCIÓN SI ES NECESARIO ---
    bool missingNewNodes = (inputChainNode == nullptr || stripNodeA == nullptr || stripNodeB == nullptr);
    bool graphEmpty = (inputNode == nullptr || mainGraph->getNodeForId(inputNode->nodeID) == nullptr);

    if (graphEmpty || missingNewNodes)
    {
        mainGraph->clear();

        // 1. Hardware IO
        inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
        outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

        // 2. Input Chain
        inputChainNode = mainGraph->addNode(std::make_unique<InputChainProcessor>());

        // 3. Channel Strips
        stripNodeA = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());
        stripNodeB = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());
    }

    // =========================================================================================
    // CORRECCIÓN DE RUTEO: SUMA DE CANALES (INPUT SUMMING)
    // =========================================================================================

    // 1. Limpiamos conexiones viejas saliendo del input
    for (auto c : mainGraph->getConnections())
        if (c.source.nodeID == inputNode->nodeID)
            mainGraph->removeConnection(c);

    // 2. Conexión "Omnidireccional": Enviamos TODO a AMBOS lados del InputChain
    if (inputChainNode)
    {
        // Conexión Estándar: Lo que entra en 0 va a 0.
        mainGraph->addConnection({ { inputNode->nodeID, 0 }, { inputChainNode->nodeID, 0 } });

        // Si hay entrada física 1, va a 1.
        if (numIn > 1)
        {
            mainGraph->addConnection({ { inputNode->nodeID, 1 }, { inputChainNode->nodeID, 1 } });
        }
        else
        {
            // Caso borde: Interfaz puramente Mono (raro en DAWs, común en micros USB).
            // Aquí sí duplicamos por necesidad física.
            mainGraph->addConnection({ { inputNode->nodeID, 0 }, { inputChainNode->nodeID, 1 } });
        }
    }

    rebuildGraph();
    startThread(juce::Thread::Priority::low);
}

void AudioEngine::rebuildGraph()
{
    // Limpieza interna (Respetamos la conexión de entrada que acabamos de hacer arriba)
    for (auto c : mainGraph->getConnections())
    {
        bool isInputConnection = (c.destination.nodeID == inputChainNode->nodeID);
        if (!isInputConnection)
            mainGraph->removeConnection(c);
    }

    // 1. Conectar InputChain -> Pedales -> Strip
    if (inputChainNode && stripNodeA) connectChainToGain(nodesChainA, stripNodeA->nodeID);
    if (inputChainNode && stripNodeB) connectChainToGain(nodesChainB, stripNodeB->nodeID);

    // 2. Conectar Strips -> Hardware Output
    for (auto* strip : { stripNodeA.get(), stripNodeB.get() })
    {
        if (strip)
        {
            mainGraph->addConnection({ { strip->nodeID, 0 }, { outputNode->nodeID, 0 } });
            mainGraph->addConnection({ { strip->nodeID, 1 }, { outputNode->nodeID, 1 } });
        }
    }
}

void AudioEngine::connectChainToGain(const std::vector<juce::AudioProcessorGraph::Node::Ptr>& nodes,
    juce::AudioProcessorGraph::NodeID targetStripID)
{
    // La señal viene del InputChain
    juce::AudioProcessorGraph::NodeID currentSource = inputChainNode->nodeID;

    for (int i = 0; i < (int)nodes.size(); ++i)
    {
        auto node = nodes[i];
        if (node)
        {
            mainGraph->addConnection({ { currentSource, 0 }, { node->nodeID, 0 } });
            mainGraph->addConnection({ { currentSource, 1 }, { node->nodeID, 1 } });
            currentSource = node->nodeID;
        }
    }

    // Conexión final al Strip
    mainGraph->addConnection({ { currentSource, 0 }, { targetStripID, 0 } });
    mainGraph->addConnection({ { currentSource, 1 }, { targetStripID, 1 } });
}

void AudioEngine::updateGlobalParams(const juce::ValueTree& settings, const juce::ValueTree& lineA, const juce::ValueTree& lineB)
{
    // 1. INPUT CHAIN
    if (inputChainNode)
    {
        if (auto* p = dynamic_cast<InputChainProcessor*>(inputChainNode->getProcessor()))
        {
            float inGain = (float)settings.getProperty(Nova::IDs::INPUT_GAIN);
            float gateThresh = (float)settings.getProperty(Nova::IDs::INPUT_GATE);

            // SEGURIDAD: Si el Gate viene a 0 o -inf raro, aseguramos que esté abierto (-100dB) al inicio
            // o respetamos el valor si es válido.
            if (gateThresh == 0.0f) gateThresh = -100.0f;

            bool mono = (bool)settings.getProperty(Nova::IDs::FORCE_MONO);
            int trans = (int)settings.getProperty(Nova::IDs::INPUT_TRANS);
            p->setParams(inGain, gateThresh, mono, trans);
        }
    }

    // 2. LINE A STRIP
    if (stripNodeA)
    {
        if (auto* p = dynamic_cast<ChannelStripProcessor*>(stripNodeA->getProcessor()))
        {
            int mode = (int)settings.getProperty(Nova::IDs::SWITCH_MODE);
            bool muted = (mode == (int)Nova::SwitcherMode::LineB_Only);

            float gain = muted ? 0.0f : (float)lineA.getProperty(Nova::IDs::MIXER_GAIN_A);
            float pan = (float)lineA.getProperty(Nova::IDs::MIXER_PAN_A);
            float width = (float)lineA.getProperty(Nova::IDs::MIXER_WIDTH_A);

            if (!muted && gain <= 0.001f) gain = 1.0f; // Force Unity Gain if bugged
            p->setParams(gain, pan, width);
        }
    }

    // 3. LINE B STRIP
    if (stripNodeB)
    {
        if (auto* p = dynamic_cast<ChannelStripProcessor*>(stripNodeB->getProcessor()))
        {
            int mode = (int)settings.getProperty(Nova::IDs::SWITCH_MODE);
            bool muted = (mode == (int)Nova::SwitcherMode::LineA_Only);

            float gain = muted ? 0.0f : (float)lineB.getProperty(Nova::IDs::MIXER_GAIN_B);
            float pan = (float)lineB.getProperty(Nova::IDs::MIXER_PAN_B);
            float width = (float)lineB.getProperty(Nova::IDs::MIXER_WIDTH_B);

            if (!muted && gain <= 0.001f) gain = 1.0f;
            p->setParams(gain, pan, width);
        }
    }
}

// Legacy wrapper
void AudioEngine::updateMixer(float gainA, float gainB, Nova::SwitcherMode mode) {}

// ==========================================================
// GESTIÓN DE PEDALES
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
        if (index >= 0 && index <= (int)list.size()) list.insert(list.begin() + index, node);
        else list.push_back(node);

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
    nodesChainA.clear();
    nodesChainB.clear();
    rebuildGraph();
    mainGraph->suspendProcessing(false);
}

void AudioEngine::setPedalBypassed(Nova::ChainID chain, int index, bool bypassed)
{
    const juce::ScopedLock sl(vectorLock);
    const auto& nodes = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
    if (index >= 0 && index < (int)nodes.size())
    {
        if (auto node = nodes[index])
        {
            // Lógica de bypass (requiere soporte en la clase base de pedales)
        }
    }
}

const std::vector<juce::AudioProcessorGraph::Node::Ptr>& AudioEngine::getNodes(Nova::ChainID chain) const
{
    return (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
}

// ==========================================================
// TUNER & DIAGNOSTICS
// ==========================================================
void AudioEngine::setEngineEnabled(bool enabled) { isEngineOn = enabled; }
double AudioEngine::getCpuLoad() const { return cpuUsage; }
int AudioEngine::getLatencyNumSamples() const { return mainGraph ? mainGraph->getLatencySamples() : 0; }
void AudioEngine::setTunerEnabled(bool shouldEnable) { tunerEnabled = shouldEnable; if (shouldEnable) tunerFifo.reset(); }

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    auto startTime = juce::Time::getMillisecondCounterHiRes();

    // 1. Tuner Logic
    if (tunerEnabled)
    {
        const int numSamples = buffer.getNumSamples();
        const auto* inL = buffer.getReadPointer(0);
        const auto* inR = (buffer.getNumChannels() > 1) ? buffer.getReadPointer(1) : nullptr;

        int start1, size1, start2, size2;
        tunerFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

        if (size1 > 0)
        {
            for (int i = 0; i < size1; ++i)
            {
                float val = inL[i];
                if (inR) val += inR[i];
                tunerCircularBuffer[start1 + i] = val * 0.5f;
            }
        }
        if (size2 > 0)
        {
            for (int i = 0; i < size2; ++i)
            {
                float val = inL[size1 + i];
                if (inR) val += inR[size1 + i];
                tunerCircularBuffer[start2 + i] = val * 0.5f;
            }
        }
        tunerFifo.finishedWrite(size1 + size2);

        // NO SILENCIAMOS EL BUFFER
    }

    if (!isEngineOn || startupCounter > 0)
    {
        if (startupCounter > 0) startupCounter--;
        buffer.clear();
        cpuUsage = 0.0;
        return;
    }

    mainGraph->processBlock(buffer, midi);

    auto endTime = juce::Time::getMillisecondCounterHiRes();
    double timeTakenMs = endTime - startTime;
    if (currentRate > 0)
    {
        double blockDurationMs = (buffer.getNumSamples() / currentRate) * 1000.0;
        if (blockDurationMs > 0.0) cpuUsage = (cpuUsage * 0.9) + ((timeTakenMs / blockDurationMs) * 100.0 * 0.1);
    }
}

// ==========================================================
// THREAD: TUNER ANALYSIS
// ==========================================================

void AudioEngine::run()
{
    while (!threadShouldExit())
    {
        if (tunerFifo.getNumReady() >= TUNER_PROCESS_SIZE)
        {
            int start1, size1, start2, size2;
            tunerFifo.prepareToRead(TUNER_PROCESS_SIZE, start1, size1, start2, size2);
            if (size1 > 0) juce::FloatVectorOperations::copy(tunerWorkBuffer.data(), tunerCircularBuffer.data() + start1, size1);
            if (size2 > 0) juce::FloatVectorOperations::copy(tunerWorkBuffer.data() + size1, tunerCircularBuffer.data() + start2, size2);
            tunerFifo.finishedRead(size1 + size2);

            float sumSq = 0.0f;
            for (float s : tunerWorkBuffer) sumSq += s * s;
            float rms = std::sqrt(sumSq / (float)TUNER_PROCESS_SIZE);
            currentRMS = rms;

            if (rms > 0.0002f)
            {
                auto result = calculateFrequencyWithClarity(tunerWorkBuffer.data(), TUNER_PROCESS_SIZE, currentRate);
                float freq = result.first;
                float clarity = result.second;

                if (clarity > 0.85f && freq > 25.0f && freq < 1500.0f)
                {
                    currentPitch = freq;
                    currentClarity = clarity;
                    float midiNote = 69.0f + 12.0f * std::log2(freq / 440.0f);
                    currentNote = (int)std::round(midiNote);
                }
                else
                {
                    currentClarity = 0.0f;
                }
            }
            else
            {
                currentClarity = 0.0f;
                currentPitch = 0.0f;
            }
        }
        else { wait(10); }
    }
}

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

        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestPeriod = lag;
        }
    }

    if (bestCorrelation < 0.2f) return { 0.0f, 0.0f };

    float finalPeriod = (float)bestPeriod;
    if (bestPeriod > minPeriod && bestPeriod < maxPeriod - 1)
    {
        float prevCorr = 0.0f; float nextCorr = 0.0f;
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