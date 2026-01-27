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

    //tunerCircularBuffer.resize(TUNER_FIFO_SIZE, 0.0f);
    //tunerWorkBuffer.resize(TUNER_PROCESS_SIZE, 0.0f);

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

    //tunerCircularBuffer.assign(TUNER_FIFO_SIZE, 0.0f);
    //tunerWorkBuffer.assign(TUNER_PROCESS_SIZE, 0.0f);
    //tunerFifo.setTotalSize(TUNER_FIFO_SIZE);
    //tunerFifo.reset();
    //currentRMS = 0.0f;
	tunerService.reset();
    startupCounter = 5;

    mainGraph->setPlayConfigDetails(numIn, numOut, sampleRate, samplesPerBlock);
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

    // --- RECONSTRUCCIÓN SI ES NECESARIO ---
    bool missingNewNodes = (inputChainNode == nullptr || stripNodeA == nullptr || stripNodeB == nullptr || outputChainNode == nullptr);
    bool graphEmpty = (inputNode == nullptr || mainGraph->getNodeForId(inputNode->nodeID) == nullptr);

    if (graphEmpty || missingNewNodes)
    {
        mainGraph->clear();

        // 1. Hardware IO
        inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
        outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

        // 2. Procesadores Internos
        inputChainNode = mainGraph->addNode(std::make_unique<InputChainProcessor>());
        stripNodeA = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());
        stripNodeB = mainGraph->addNode(std::make_unique<ChannelStripProcessor>());

        // 3. Output Chain (Mastering)
        outputChainNode = mainGraph->addNode(std::make_unique<OutputChainProcessor>()); // <--- NUEVO
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
        mainGraph->addConnection({ { inputNode->nodeID, 0 }, { inputChainNode->nodeID, 0 } });
        if (numIn > 1) mainGraph->addConnection({ { inputNode->nodeID, 1 }, { inputChainNode->nodeID, 1 } });
        else           mainGraph->addConnection({ { inputNode->nodeID, 0 }, { inputChainNode->nodeID, 1 } });
    }

    rebuildGraph();
    startThread(juce::Thread::Priority::low);
}

void AudioEngine::rebuildGraph()
{
    // Limpieza interna (Respetamos entrada y salida de hardware por seguridad, 
    // aunque la salida la re-conectaremos abajo)
    for (auto c : mainGraph->getConnections())
    {
        bool isHardwareInput = (c.source.nodeID == inputNode->nodeID);
        if (!isHardwareInput)
            mainGraph->removeConnection(c);
    }

    // 1. Conectar InputChain -> Pedales -> Strip (Igual que antes)
    if (inputChainNode && stripNodeA) connectChainToGain(nodesChainA, stripNodeA->nodeID);
    if (inputChainNode && stripNodeB) connectChainToGain(nodesChainB, stripNodeB->nodeID);

    // 2. Conectar Strips -> OutputChain (Mezcla de buses)
    if (outputChainNode)
    {
        for (auto* strip : { stripNodeA.get(), stripNodeB.get() })
        {
            if (strip) {
                // Sumamos ambos carriles a la entrada del Master
                mainGraph->addConnection({ { strip->nodeID, 0 }, { outputChainNode->nodeID, 0 } });
                mainGraph->addConnection({ { strip->nodeID, 1 }, { outputChainNode->nodeID, 1 } });
            }
        }

        // 3. Conectar OutputChain -> Hardware Output (Salida final)
        mainGraph->addConnection({ { outputChainNode->nodeID, 0 }, { outputNode->nodeID, 0 } });
        mainGraph->addConnection({ { outputChainNode->nodeID, 1 }, { outputNode->nodeID, 1 } });
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
    // 1. INPUT CHAIN (Igual)
    if (inputChainNode) { 
        if (auto* p = dynamic_cast<InputChainProcessor*>(inputChainNode->getProcessor())) {
            float inGain = (float)settings.getProperty(Nova::IDs::INPUT_GAIN);
            float gateThresh = (float)settings.getProperty(Nova::IDs::INPUT_GATE);
            if (gateThresh == 0.0f) gateThresh = -100.0f;
            bool mono = (bool)settings.getProperty(Nova::IDs::FORCE_MONO);
            int trans = (int)settings.getProperty(Nova::IDs::INPUT_TRANS);
            p->setParams(inGain, gateThresh, mono, trans);
        }
    }

    // 2. OUTPUT CHAIN (NUEVO)
    if (outputChainNode)
    {
        if (auto* p = dynamic_cast<OutputChainProcessor*>(outputChainNode->getProcessor()))
        {
            float outVol = (float)settings.getProperty(Nova::IDs::OUTPUT_VOL, 0.0f);
            float limit = (float)settings.getProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f);

            p->setParams(outVol, limit);

            // --- NUEVO: CAPTURAR EL MIX ---
            // El slider va de 0 a 100, lo convertimos a 0.0 - 1.0
            float mixPercent = (float)settings.getProperty(Nova::IDs::OUTPUT_MIX, 100.0f);
            currentGlobalMix = juce::jlimit(0.0f, 100.0f, mixPercent) / 100.0f;
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
void AudioEngine::setTunerEnabled(bool shouldEnable) { 
    tunerEnabled = shouldEnable; 
    if (shouldEnable) 
        //tunerFifo.reset(); 
		tunerService.reset();
}

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    auto startTime = juce::Time::getMillisecondCounterHiRes();

    // 1. LÓGICA DEL AFINADOR
    if (tunerEnabled)
    {
        //// A. Capturamos la señal para el afinador
        //const int numSamples = buffer.getNumSamples();
        //const auto* inL = buffer.getReadPointer(0);
        //const auto* inR = (buffer.getNumChannels() > 1) ? buffer.getReadPointer(1) : nullptr;

        //int start1, size1, start2, size2;
        //tunerFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

        //if (size1 > 0)
        //{
        //    for (int i = 0; i < size1; ++i)
        //    {
        //        float val = inL[i];
        //        if (inR) val += inR[i];
        //        tunerCircularBuffer[start1 + i] = val * 0.5f;
        //    }
        //}
        //if (size2 > 0)
        //{
        //    for (int i = 0; i < size2; ++i)
        //    {
        //        float val = inL[size1 + i];
        //        if (inR) val += inR[size1 + i];
        //        tunerCircularBuffer[start2 + i] = val * 0.5f;
        //    }
        //}
        //tunerFifo.finishedWrite(size1 + size2);

        //// B. MUTE DE SALIDA (LO QUE PEDISTE)
        //// Silenciamos el buffer de audio para que no suene nada en los altavoces
        //// mientras el afinador analiza la señal de entrada.
        //buffer.clear();

        //cpuUsage = 0.0;
        //return; // Salimos aquí para no procesar efectos

        // Enviamos datos al servicio
        tunerService.pushBuffer(buffer);

        // Mute de Salida
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

    // ==============================================================================
    // LÓGICA DE MIX (DRY/WET) CON CENTRADO INTELIGENTE
    // ==============================================================================

    float mix = currentGlobalMix;
    bool isMixActive = (mix < 0.99f);

    // A. CAPTURA DRY (CENTRADA)
    if (isMixActive)
    {
        if (dryBuffer.getNumChannels() < buffer.getNumChannels() || dryBuffer.getNumSamples() != buffer.getNumSamples())
            dryBuffer.setSize(buffer.getNumChannels(), buffer.getNumSamples());

        int numSamples = buffer.getNumSamples();
        const auto* inL = buffer.getReadPointer(0);
        const auto* inR = (buffer.getNumChannels() > 1) ? buffer.getReadPointer(1) : nullptr;

        for (int ch = 0; ch < dryBuffer.getNumChannels(); ++ch)
        {
            auto* dryCh = dryBuffer.getWritePointer(ch);
            if (inR != nullptr)
                for (int i = 0; i < numSamples; ++i) dryCh[i] = (inL[i] + inR[i]) * 0.5f;
            else
                for (int i = 0; i < numSamples; ++i) dryCh[i] = inL[i];
        }
    }

    // B. PROCESAMIENTO WET
    mainGraph->processBlock(buffer, midi);

    // C. MEZCLA FINAL
    if (isMixActive)
    {
        int numSamples = buffer.getNumSamples();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* wetData = buffer.getWritePointer(ch);
            int dryCh = (ch < dryBuffer.getNumChannels()) ? ch : 0;
            const auto* dryData = dryBuffer.getReadPointer(dryCh);

            for (int i = 0; i < numSamples; ++i)
                wetData[i] = (wetData[i] * mix) + (dryData[i] * (1.0f - mix));
        }
    }

    // ==============================================================================
    // CPU METER
    // ==============================================================================
    auto endTime = juce::Time::getMillisecondCounterHiRes();
    double timeTakenMs = endTime - startTime;
    if (currentRate > 0)
    {
        double blockDurationMs = (buffer.getNumSamples() / currentRate) * 1000.0;
        if (blockDurationMs > 0.0)
            cpuUsage = (cpuUsage * 0.9) + ((timeTakenMs / blockDurationMs) * 100.0 * 0.1);
    }
}
// ==========================================================
// THREAD: TUNER ANALYSIS
// ==========================================================

void AudioEngine::run()
{
    while (!threadShouldExit())
    {
        /*if (tunerFifo.getNumReady() >= TUNER_PROCESS_SIZE)
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
        else { wait(10); }*/
        while (!threadShouldExit())
        {
            if (tunerEnabled)
            {
                // El servicio se encarga de ver si tiene datos y procesarlos
                tunerService.process();

                // Dormimos un poco para no quemar CPU esperando datos
                wait(5);
            }
            else
            {
                // Si el afinador está apagado, el hilo duerme más tiempo
                wait(100);
            }
        }

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