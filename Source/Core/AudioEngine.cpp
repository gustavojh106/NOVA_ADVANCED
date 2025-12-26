#include "AudioEngine.h"

AudioEngine::AudioEngine()
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();
}

AudioEngine::~AudioEngine()
{
    reset();
}

// ==============================================================================
//  PREPARACIÓN Y CONFIGURACIÓN (Cimientos Sólidos)
// ==============================================================================
void AudioEngine::prepare(double sampleRate, int samplesPerBlock, int numInputChannels, int numOutputChannels)
{
    currentRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    hardwareInputs = numInputChannels;
    hardwareOutputs = numOutputChannels;

    if (currentRate <= 0) return;

    // 1. Limpieza total para empezar de cero
    mainGraph->releaseResources();
    mainGraph->clear();
    nodesChain.clear();

    // 2. Configuración del Grafo
    mainGraph->setPlayConfigDetails(hardwareInputs, hardwareOutputs, sampleRate, samplesPerBlock);
    mainGraph->setBusesLayout({
        juce::AudioChannelSet::canonicalChannelSet(hardwareInputs),
        juce::AudioChannelSet::canonicalChannelSet(hardwareOutputs)
        });
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

    // 3. Creación de Nodos de Sistema (INMUTABLES)
    auto inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
    auto outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    if (inputNode && outputNode)
    {
        systemInputID = inputNode->nodeID;
        systemOutputID = outputNode->nodeID;

        // Conexión inicial: Entrada -> Salida (Bypass total)
        connectNodes(systemInputID, systemOutputID);
    }

    inPanicState = false;
    startupCounter = 100; // Silencio breve al inicio para estabilizar
}

// ==============================================================================
//  PROCESO DE AUDIO (NO TRY-CATCH)
// ==============================================================================
void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // 1. Validaciones rápidas
    if (currentRate <= 0 || inPanicState)
    {
        buffer.clear();
        return;
    }

    if (startupCounter > 0)
    {
        startupCounter--;
        buffer.clear();
        return;
    }

    // 2. Protección PRE-Proceso (Check de sanidad de la entrada)
    if (!isStreamHealthy(buffer))
    {
        buffer.clear();
        return;
    }

    // 3. Procesamiento Real (Sin excepciones)
    // El grafo maneja internamente la seguridad de punteros nulos.
    mainGraph->processBlock(buffer, midi);

    // 4. Protección POST-Proceso (Check de sanidad de la salida)
    if (!isStreamHealthy(buffer))
    {
        buffer.clear();
        DBG("AudioEngine: Panic! NaN detected in output. Silencing.");
        inPanicState = true;
        // Aquí podrías notificar a la UI en el futuro
    }
}

// ==============================================================================
//  GESTIÓN QUIRÚRGICA DE PEDALES (Smart Routing)
// ==============================================================================
void AudioEngine::addPedal(const juce::String& pedalType)
{
    // Suspendemos el audio para evitar clicks mientras reconectamos
    mainGraph->suspendProcessing(true);

    auto newPedalProcessor = PedalRegistry::createPedal(pedalType);
    if (newPedalProcessor)
    {
        // Configuración Stereo Interna
        if (currentRate > 0)
        {
            newPedalProcessor->setPlayConfigDetails(2, 2, currentRate, currentBlockSize);
            newPedalProcessor->prepareToPlay(currentRate, currentBlockSize);
        }

        auto newNode = mainGraph->addNode(std::move(newPedalProcessor));

        // LÓGICA DE INSERCIÓN:
        // Siempre insertamos al FINAL de la cadena de usuario, justo antes del Output del sistema.

        juce::AudioProcessorGraph::NodeID sourceID;
        juce::AudioProcessorGraph::NodeID destID = systemOutputID;

        if (nodesChain.empty())
        {
            // Si no hay pedales: InputSistema -> [Nuevo] -> OutputSistema
            sourceID = systemInputID;
        }
        else
        {
            // Si ya hay pedales: ÚltimoPedal -> [Nuevo] -> OutputSistema
            sourceID = nodesChain.back()->nodeID;
        }

        // 1. Romper la conexión existente (Source -> Dest)
        disconnectNodes(sourceID, destID);

        // 2. Crear las nuevas conexiones (Source -> Nuevo -> Dest)
        connectNodes(sourceID, newNode->nodeID);
        connectNodes(newNode->nodeID, destID);

        // 3. Registrar el nodo
        nodesChain.push_back(newNode);
    }

    mainGraph->suspendProcessing(false);
}

void AudioEngine::removePedal(int index)
{
    if (index < 0 || index >= nodesChain.size()) return;

    mainGraph->suspendProcessing(true);

    auto nodeToRemove = nodesChain[index];

    // Identificar vecinos
    juce::AudioProcessorGraph::NodeID prevID;
    juce::AudioProcessorGraph::NodeID nextID;

    // ¿Quién está antes?
    if (index == 0) prevID = systemInputID;
    else            prevID = nodesChain[index - 1]->nodeID;

    // ¿Quién está después?
    if (index == nodesChain.size() - 1) nextID = systemOutputID;
    else                                nextID = nodesChain[index + 1]->nodeID;

    // 1. Desconectar el nodo a eliminar de sus vecinos
    disconnectNodes(prevID, nodeToRemove->nodeID);
    disconnectNodes(nodeToRemove->nodeID, nextID);

    // 2. Eliminar el nodo del grafo
    mainGraph->removeNode(nodeToRemove->nodeID);

    // 3. Coser la herida (Conectar Prev -> Next)
    connectNodes(prevID, nextID);

    // 4. Actualizar registro
    nodesChain.erase(nodesChain.begin() + index);

    mainGraph->suspendProcessing(false);
}

void AudioEngine::clearChain()
{
    mainGraph->suspendProcessing(true);

    // Borramos solo los pedales de usuario, NO los nodos de sistema
    for (auto& node : nodesChain)
    {
        mainGraph->removeNode(node->nodeID);
    }
    nodesChain.clear();

    // Restauramos el bypass limpio: Input -> Output
    disconnectNodes(systemInputID, systemOutputID); // Por seguridad
    connectNodes(systemInputID, systemOutputID);

    mainGraph->suspendProcessing(false);
    inPanicState = false;
}

// ==============================================================================
//  RUTEO INTELIGENTE (Omni-Input Logic)
// ==============================================================================
void AudioEngine::connectNodes(juce::AudioProcessorGraph::NodeID sourceID, juce::AudioProcessorGraph::NodeID destID)
{
    auto sourceNode = mainGraph->getNodeForId(sourceID);
    auto destNode = mainGraph->getNodeForId(destID);

    if (!sourceNode || !destNode) return;

    int sourceCh = sourceNode->getProcessor()->getTotalNumOutputChannels();
    int destCh = destNode->getProcessor()->getTotalNumInputChannels();

    // ¿Es el nodo fuente la Entrada Física del Sistema?
    bool isSystemInput = (sourceID == systemInputID);

    if (isSystemInput)
    {
        // === MODO OMNI-INPUT (Guitar Friendly) ===
        // Sumamos todas las entradas de hardware al L y R del primer pedal
        for (int i = 0; i < sourceCh; ++i)
        {
            // Conectar input i -> Canal 0 del destino (L)
            mainGraph->addConnection({ { sourceID, i }, { destID, 0 } });

            // Conectar input i -> Canal 1 del destino (R) si existe
            if (destCh > 1)
                mainGraph->addConnection({ { sourceID, i }, { destID, 1 } });
        }
    }
    else
    {
        // === CONEXIÓN ESTÁNDAR (Pedal a Pedal / Pedal a Output) ===
        // L -> L
        if (sourceCh > 0 && destCh > 0)
            mainGraph->addConnection({ { sourceID, 0 }, { destID, 0 } });

        // R -> R
        if (sourceCh > 1 && destCh > 1)
            mainGraph->addConnection({ { sourceID, 1 }, { destID, 1 } });
    }
}

void AudioEngine::disconnectNodes(juce::AudioProcessorGraph::NodeID sourceID, juce::AudioProcessorGraph::NodeID destID)
{
    // 1. Obtenemos una REFERENCIA a la lista actual de conexiones
    const auto& connections = mainGraph->getConnections();

    // 2. Creamos una lista temporal de las que queremos borrar.
    // (No podemos borrar mientras iteramos directamente sobre 'connections' porque invalidaría el iterador)
    std::vector<juce::AudioProcessorGraph::Connection> connectionsToRemove;

    for (const auto& c : connections)
    {
        if (c.source.nodeID == sourceID && c.destination.nodeID == destID)
        {
            connectionsToRemove.push_back(c);
        }
    }

    // 3. Ahora sí, las borramos del grafo una por una
    for (const auto& c : connectionsToRemove)
    {
        mainGraph->removeConnection(c);
    }
}

// ==============================================================================
//  UTILIDADES
// ==============================================================================
void AudioEngine::reset()
{
    mainGraph->releaseResources();
    inPanicState = false;
}

bool AudioEngine::isStreamHealthy(const juce::AudioBuffer<float>& buffer)
{
    // Optimización: Solo chequear si hay datos
    if (buffer.getNumChannels() == 0) return true;

    // Chequeo rápido solo del primer sample para evitar CPU hit en release,
    // o chequeo completo solo en Debug.
    // Para producción segura (SOTA), chequeamos todo pero optimizado.

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getReadPointer(ch);
        // Solo revisamos el primer y último sample por eficiencia, 
        // o iteramos si la seguridad es prioridad máxima.
        // Aquí priorizamos seguridad:
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            if (!std::isfinite(data[i])) return false;
            if (std::abs(data[i]) > 50.0f) return false; // Umbral de seguridad (volumen explosivo)
        }
    }
    return true;
}

const std::vector<juce::AudioProcessorGraph::Node::Ptr>& AudioEngine::getActiveNodes() const
{
    return nodesChain;
}

int AudioEngine::getLatencyNumSamples() const
{
    // El método correcto en JUCE es getLatencySamples(), no getLatencyNumSamples()
    return mainGraph ? mainGraph->getLatencySamples() : 0;
}