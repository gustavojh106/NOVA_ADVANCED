#include "AudioEngine.h"
#include "PedalRegistry.h"

// ==========================================================
// IMPLEMENTACIÓN DE SIMPLE GAIN
// ==========================================================

SimpleGainProcessor::SimpleGainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo())
        .withOutput("Output", juce::AudioChannelSet::stereo()))
{
    currentGain = 1.0f;
    targetGain = 1.0f;
}

void SimpleGainProcessor::setGain(float gain)
{
    targetGain = gain;
}

void SimpleGainProcessor::prepareToPlay(double, int)
{
    currentGain = targetGain;
}

void SimpleGainProcessor::releaseResources() {}

void SimpleGainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (std::abs(currentGain - targetGain) > 0.001f)
        currentGain += (targetGain - currentGain) * 0.1f;
    else
        currentGain = targetGain;

    if (currentGain < 0.001f)
        buffer.clear();
    else
        buffer.applyGain(currentGain);
}

// ==========================================================
// IMPLEMENTACIÓN DE AUDIO ENGINE
// ==========================================================

AudioEngine::AudioEngine()
    : juce::Thread("TunerThread")
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();

    // Inicializamos vectores
    tunerCircularBuffer.resize(TUNER_FIFO_SIZE, 0.0f);
    tunerWorkBuffer.resize(TUNER_PROCESS_SIZE, 0.0f);

    startThread(juce::Thread::Priority::low);
}

AudioEngine::~AudioEngine()
{
    stopThread(4000);
    mainGraph->releaseResources();
    mainGraph->clear();
}

void AudioEngine::prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut)
{
    stopThread(5000); // Pausar tuner para evitar crash por resize

    currentRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    currentSampleRate = sampleRate;
    numInputChannels = numIn;

    // Reiniciar Buffers y FIFO
    tunerCircularBuffer.assign(TUNER_FIFO_SIZE, 0.0f);
    tunerWorkBuffer.assign(TUNER_PROCESS_SIZE, 0.0f);
    tunerFifo.setTotalSize(TUNER_FIFO_SIZE);
    tunerFifo.reset();

    currentRMS = 0.0f;
    startupCounter = 5;

    mainGraph->setPlayConfigDetails(numIn, numOut, sampleRate, samplesPerBlock);
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

    if (inputNode == nullptr || mainGraph->getNodeForId(inputNode->nodeID) == nullptr)
    {
        mainGraph->clear();
        inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
        outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

        gainNodeA = mainGraph->addNode(std::make_unique<SimpleGainProcessor>());
        gainNodeB = mainGraph->addNode(std::make_unique<SimpleGainProcessor>());
    }

    // Reconectar Outputs
    for (auto* gainNode : { gainNodeA.get(), gainNodeB.get() })
    {
        if (!gainNode) continue;
        for (auto c : mainGraph->getConnections())
            if (c.source.nodeID == gainNode->nodeID && c.destination.nodeID == outputNode->nodeID)
                mainGraph->removeConnection(c);

        if (numOut > 0) mainGraph->addConnection({ { gainNode->nodeID, 0 }, { outputNode->nodeID, 0 } });
        if (numOut > 1) mainGraph->addConnection({ { gainNode->nodeID, 1 }, { outputNode->nodeID, 1 } });
    }

    rebuildGraph();
    startThread(juce::Thread::Priority::low);
}

void AudioEngine::setPedalBypassed(Nova::ChainID chain, int index, bool bypassed)
{
    const juce::ScopedLock sl(vectorLock);
    const auto& nodes = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;

    if (index >= 0 && index < (int)nodes.size())
    {
        auto node = nodes[index];
        if (node != nullptr && node->getProcessor() != nullptr)
        {
            if (auto* processor = dynamic_cast<ProcessorBase*>(node->getProcessor()))
            {
                processor->setBypassed(bypassed);
            }
        }
    }
}

// EN AUDIOENGINE.CPP - Reemplaza el método run()

void AudioEngine::run()
{
    while (!threadShouldExit())
    {
        if (tunerFifo.getNumReady() >= TUNER_PROCESS_SIZE)
        {
            // ... (Lectura del buffer igual que antes) ...
            int start1, size1, start2, size2;
            tunerFifo.prepareToRead(TUNER_PROCESS_SIZE, start1, size1, start2, size2);
            if (size1 > 0) juce::FloatVectorOperations::copy(tunerWorkBuffer.data(), tunerCircularBuffer.data() + start1, size1);
            if (size2 > 0) juce::FloatVectorOperations::copy(tunerWorkBuffer.data() + size1, tunerCircularBuffer.data() + start2, size2);
            tunerFifo.finishedRead(size1 + size2);

            // --- CORRECCIÓN DE GHOST NOTES ---
            //float sumSq = 0.0f;
            //for (float s : tunerWorkBuffer) sumSq += s * s;
            //float rms = std::sqrt(sumSq / (float)TUNER_PROCESS_SIZE);
            //currentRMS = rms;

            // Calcular RMS
            float sumSq = 0.0f;
            for (float s : tunerWorkBuffer) sumSq += s * s;
            float rms = std::sqrt(sumSq / (float)TUNER_PROCESS_SIZE);
            currentRMS = rms;

            // UMBRAL HIPER-BAJO (Permitimos señales muy débiles si la claridad es alta después)
            if (rms > 0.0002f)
            {
                auto result = calculateFrequencyWithClarity(tunerWorkBuffer.data(), TUNER_PROCESS_SIZE, currentRate);
                float freq = result.first;
                float clarity = result.second;

                // FILTRO DE CLARIDAD: Solo aceptamos la nota si el algoritmo está 85% seguro
                // Esto elimina casi todo el ruido fluctuante.
                if (clarity > 0.85f && freq > 25.0f && freq < 1500.0f)
                {
                    currentPitch = freq;
                    currentClarity = clarity; // Guardamos claridad para la UI

                    // Solo para debug interno, la UI hace el resto
                    float midiNote = 69.0f + 12.0f * std::log2(freq / 440.0f);
                    currentNote = (int)std::round(midiNote);
                }
                else
                {
                    // Si la nota es confusa, NO actualizamos el pitch a 0 inmediatamente.
                    // Mantenemos el último valor válido un instante (Persistence)
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

    // Autocorrelación (Igual que antes)
    for (int lag = minPeriod; lag < maxPeriod; ++lag)
    {
        float sum = 0.0f;
        int limit = numSamples - lag;
        for (int i = 0; i < limit; ++i) sum += signal[i] * signal[i + lag];

        // Normalización importante para obtener "Claridad" (0.0 a 1.0)
        // Necesitamos la energía de la señal en ese segmento para normalizar
        float sumSq = 0.0f;
        for (int i = 0; i < limit; ++i) sumSq += signal[i] * signal[i];

        float correlation = 0.0f;
        if (sumSq > 0.00001f) correlation = sum / sumSq;

        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestPeriod = lag;
        }
    }

    // Si la claridad es basura, retornamos 0
    if (bestCorrelation < 0.2f) return { 0.0f, 0.0f };

    // Refinamiento Parabólico (Igual que antes)
    float finalPeriod = (float)bestPeriod;
    if (bestPeriod > minPeriod && bestPeriod < maxPeriod - 1)
    {
        // ... (Tu código de interpolación existente) ...
        // Copia aquí la lógica de interpolación parabólica que ya tenías
        // para calcular 'finalPeriod' con precisión.
        // ...

        // Recalculo rápido para el ejemplo:
        float prevCorr = 0.0f, nextCorr = 0.0f;
        // ... (cálculo de vecinos)
        // ...
        // float delta = ...
        // finalPeriod = bestPeriod - delta;
    }

    return { (float)(sampleRate / finalPeriod), bestCorrelation };
}
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

        buffer.clear();
        cpuUsage = 0.0;
        return;
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
        if (blockDurationMs > 0.0)
        {
            double currentLoad = (timeTakenMs / blockDurationMs) * 100.0;
            cpuUsage = (cpuUsage * 0.9) + (currentLoad * 0.1);
        }
    }
}

void AudioEngine::updateMixer(float gA, float gB, Nova::SwitcherMode mode)
{
    if (!gainNodeA || !gainNodeB) return;

    float finalA = gA;
    float finalB = gB;

    if (mode == Nova::SwitcherMode::LineB_Only) finalA = 0.0f;
    if (mode == Nova::SwitcherMode::LineA_Only) finalB = 0.0f;

    if (auto* p = dynamic_cast<SimpleGainProcessor*>(gainNodeA->getProcessor())) p->setGain(finalA);
    if (auto* p = dynamic_cast<SimpleGainProcessor*>(gainNodeB->getProcessor())) p->setGain(finalB);
}

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
    nodesChainA.clear();
    nodesChainB.clear();
    rebuildGraph();
    mainGraph->suspendProcessing(false);
}

void AudioEngine::rebuildGraph()
{
    // Solo borramos conexiones internas (no las de salida de ganancia)
    for (auto c : mainGraph->getConnections())
    {
        if (c.destination.nodeID != outputNode->nodeID)
            mainGraph->removeConnection(c);
    }

    connectChainToGain(nodesChainA, gainNodeA->nodeID);
    connectChainToGain(nodesChainB, gainNodeB->nodeID);
}

void AudioEngine::connectChainToGain(const std::vector<juce::AudioProcessorGraph::Node::Ptr>& nodes,
    juce::AudioProcessorGraph::NodeID gainNodeID)
{
    juce::AudioProcessorGraph::NodeID currentSource = inputNode->nodeID;

    for (int i = 0; i < (int)nodes.size(); ++i)
    {
        auto node = nodes[i];
        if (i == 0)
        {
            mainGraph->addConnection({ { currentSource, 0 }, { node->nodeID, 0 } });
            mainGraph->addConnection({ { currentSource, 0 }, { node->nodeID, 1 } });
            if (numInputChannels > 1)
            {
                mainGraph->addConnection({ { currentSource, 1 }, { node->nodeID, 0 } });
                mainGraph->addConnection({ { currentSource, 1 }, { node->nodeID, 1 } });
            }
        }
        else
        {
            mainGraph->addConnection({ { currentSource, 0 }, { node->nodeID, 0 } });
            mainGraph->addConnection({ { currentSource, 1 }, { node->nodeID, 1 } });
        }
        currentSource = node->nodeID;
    }

    // Conexión Final
    mainGraph->addConnection({ { currentSource, 0 }, { gainNodeID, 0 } });
    mainGraph->addConnection({ { currentSource, 1 }, { gainNodeID, 1 } });
    if (nodes.empty() && numInputChannels > 1)
    {
        mainGraph->addConnection({ { currentSource, 1 }, { gainNodeID, 0 } });
        mainGraph->addConnection({ { currentSource, 1 }, { gainNodeID, 1 } });
    }
}

int AudioEngine::getLatencyNumSamples() const
{
    return mainGraph ? mainGraph->getLatencySamples() : 0;
}

void AudioEngine::setEngineEnabled(bool enabled) { isEngineOn = enabled; }
double AudioEngine::getCpuLoad() const { return cpuUsage; }

const std::vector<juce::AudioProcessorGraph::Node::Ptr>& AudioEngine::getNodes(Nova::ChainID chain) const
{
    return (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
}

void AudioEngine::setTunerEnabled(bool shouldEnable)
{
    tunerEnabled = shouldEnable;
    if (shouldEnable) tunerFifo.reset();
}

float AudioEngine::calculateFrequency(const float* signal, int numSamples, double sampleRate)
{
    if (sampleRate <= 0.0) return 0.0f;

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
        float correlation = sum / (float)limit;

        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            bestPeriod = lag;
        }
    }

    if (bestCorrelation < 0.00001f) return 0.0f;

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

    return (float)(sampleRate / finalPeriod);
}