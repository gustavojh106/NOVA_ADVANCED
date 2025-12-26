#include "AudioEngine.h"

AudioEngine::AudioEngine()
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();
}

AudioEngine::~AudioEngine()
{
    mainGraph->releaseResources();
}

void AudioEngine::prepare(double sampleRate, int samplesPerBlock, int numInputChannels, int numOutputChannels)
{
    currentRate = sampleRate;
    currentBlockSize = samplesPerBlock;

    // CORRECCIÓN: Respetamos EXACTAMENTE lo que dice el Hardware
    hardwareInputs = numInputChannels;
    hardwareOutputs = numOutputChannels;

    if (currentRate <= 0) return;

    // 1. Configuramos el Grafo con la realidad del Hardware
    mainGraph->setPlayConfigDetails(hardwareInputs, hardwareOutputs, sampleRate, samplesPerBlock);

    // 2. Definimos el Layout de Buses explícito (Evita ambigüedades de JUCE)
    mainGraph->setBusesLayout({
        juce::AudioChannelSet::canonicalChannelSet(hardwareInputs),
        juce::AudioChannelSet::canonicalChannelSet(hardwareOutputs)
        });

    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

    // 3. Configuramos los Pedales INTERNOS (Siempre Stereo para mejor calidad)
    for (auto node : nodesChain)
    {
        if (auto* p = node->getProcessor())
        {
            // Forzamos Stereo dentro de los pedales aunque la entrada sea Mono
            p->setPlayConfigDetails(2, 2, sampleRate, samplesPerBlock);
            p->prepareToPlay(sampleRate, samplesPerBlock);
        }
    }

    rebuildGraph();

    inPanicState = false;
    startupCounter = 100;
}

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (currentRate <= 0) { buffer.clear(); return; }

    if (startupCounter > 0)
    {
        startupCounter--;
        buffer.clear();
        return;
    }

    if (inPanicState) inPanicState = false;

    try
    {
        // El grafo se encarga de mapear el buffer hardware a los nodos internos
        mainGraph->processBlock(buffer, midi);
    }
    catch (...)
    {
        buffer.clear();
    }

    if (!isStreamHealthy(buffer))
    {
        buffer.clear();
        DBG("WARN: Audio corruption. Silencing.");
    }
}

// ... (reset, isStreamHealthy, removePedal, clearChain, getActiveNodes IGUAL QUE ANTES) ...
// Copia esas funciones de tu versión anterior o la que te pasé antes, no cambian.

void AudioEngine::reset()
{
    mainGraph->releaseResources();
    inPanicState = false;
}

bool AudioEngine::isStreamHealthy(const juce::AudioBuffer<float>& buffer)
{
    if (buffer.getNumChannels() > 0)
    {
        auto* data = buffer.getReadPointer(0);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            if (!std::isfinite(data[i])) return false;
            if (std::abs(data[i]) > 100.0f) return false;
        }
    }
    return true;
}

void AudioEngine::removePedal(int index)
{
    if (index >= 0 && index < nodesChain.size())
    {
        auto nodePtr = nodesChain[index];
        if (nodePtr != nullptr) mainGraph->removeNode(nodePtr->nodeID);
        nodesChain.erase(nodesChain.begin() + index);
        rebuildGraph();
    }
}

void AudioEngine::clearChain()
{
    mainGraph->clear();
    nodesChain.clear();
}

const std::vector<juce::AudioProcessorGraph::Node::Ptr>& AudioEngine::getActiveNodes() const
{
    return nodesChain;
}

// ==============================================================================
//  ADD PEDAL: Configuramos siempre en Stereo (2,2)
// ==============================================================================
void AudioEngine::addPedal(const juce::String& pedalType)
{
    auto newPedal = PedalRegistry::createPedal(pedalType);
    if (newPedal)
    {
        if (currentRate > 0)
        {
            // Los pedales viven en un mundo estéreo ideal
            newPedal->setPlayConfigDetails(2, 2, currentRate, currentBlockSize);
            newPedal->prepareToPlay(currentRate, currentBlockSize);
        }

        auto node = mainGraph->addNode(std::move(newPedal));
        nodesChain.push_back(node);
        rebuildGraph();
    }
}

// ==============================================================================
//  ROUTING INTELIGENTE (MONO -> STEREO -> OUTPUT)
// ==============================================================================
void AudioEngine::rebuildGraph()
{
    for (auto& c : mainGraph->getConnections())
        mainGraph->removeConnection(c);

    juce::AudioProcessorGraph::Node::Ptr inputNode;
    juce::AudioProcessorGraph::Node::Ptr outputNode;

    for (auto* node : mainGraph->getNodes())
    {
        if (auto* io = dynamic_cast<juce::AudioProcessorGraph::AudioGraphIOProcessor*>(node->getProcessor()))
        {
            if (io->getType() == juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode)
                inputNode = node;
            else if (io->getType() == juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode)
                outputNode = node;
        }
    }

    if (!inputNode) inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
    if (!outputNode) outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    if (nodesChain.empty())
    {
        connectNodes(inputNode, outputNode);
    }
    else
    {
        connectNodes(inputNode, nodesChain.front());
        for (size_t i = 0; i < nodesChain.size() - 1; ++i)
            connectNodes(nodesChain[i], nodesChain[i + 1]);
        connectNodes(nodesChain.back(), outputNode);
    }
}

void AudioEngine::connectNodes(juce::AudioProcessorGraph::Node::Ptr source, juce::AudioProcessorGraph::Node::Ptr dest)
{
    if (!source || !dest) return;

    int sourceCh = source->getProcessor()->getTotalNumOutputChannels();
    int destCh = dest->getProcessor()->getTotalNumInputChannels();

    // SOTA CHECK: ¿Es este el nodo de entrada físico del sistema?
    bool isSystemInput = (dynamic_cast<juce::AudioProcessorGraph::AudioGraphIOProcessor*>(source->getProcessor()) != nullptr &&
        source->getProcessor()->getTotalNumInputChannels() == 0);

    if (isSystemInput)
    {
        // === MODO OMNI-INPUT (Guitar Friendly) ===
        // Conectamos TODOS los canales de entrada física (1, 2, etc.) 
        // a AMBOS canales de entrada del primer pedal.
        // JUCE suma las señales automáticamente en el destino.
        // Resultado: Input 1 (Silencio) + Input 2 (Guitarra) = Guitarra en el centro.

        for (int i = 0; i < sourceCh; ++i)
        {
            // Conectar al Canal L del pedal
            mainGraph->addConnection({ { source->nodeID, i }, { dest->nodeID, 0 } });

            // Conectar al Canal R del pedal (si existe)
            if (destCh > 1)
            {
                mainGraph->addConnection({ { source->nodeID, i }, { dest->nodeID, 1 } });
            }
        }
    }
    else
    {
        // === CONEXIÓN ESTÁNDAR ENTRE PEDALES ===
        // (Pedal -> Pedal) Aquí mantenemos el estéreo separado L->L, R->R

        // L -> L
        if (sourceCh > 0 && destCh > 0)
            mainGraph->addConnection({ { source->nodeID, 0 }, { dest->nodeID, 0 } });

        // R -> R
        if (sourceCh > 1 && destCh > 1)
            mainGraph->addConnection({ { source->nodeID, 1 }, { dest->nodeID, 1 } });
    }
}