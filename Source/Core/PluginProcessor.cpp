#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PedalRegistry.h"

// Identificadores estáticos para evitar erratas
namespace IDs
{
    static const juce::Identifier PEDAL_TAG("PEDAL");
    static const juce::Identifier PEDAL_TYPE("type");
    static const juce::Identifier MAIN_STATE("NOVA_STATE");
}

NOVAAudioProcessor::NOVAAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        // VITAL: Pedimos Stereo explícitamente
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    ),
    pluginState(IDs::MAIN_STATE)
#endif
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();
    pluginState.addListener(this);
}

NOVAAudioProcessor::~NOVAAudioProcessor()
{
    // Limpieza
    pluginState.removeListener(this);
}

// ==============================================================================
//  SOTA: MÉTODOS DE GESTIÓN DE ESTADO (La UI llama a esto)
// ==============================================================================
void NOVAAudioProcessor::requestAddPedal(const juce::String& pedalType)
{
    // Creamos un dato que representa el pedal
    juce::ValueTree newPedal(IDs::PEDAL_TAG);
    newPedal.setProperty(IDs::PEDAL_TYPE, pedalType, nullptr);

    // Lo añadimos al árbol (Esto disparará valueTreeChildAdded)
    pluginState.appendChild(newPedal, nullptr);
}

void NOVAAudioProcessor::requestRemovePedal(int index)
{
    if (index >= 0 && index < pluginState.getNumChildren())
        pluginState.removeChild(index, nullptr);
}

// ==============================================================================
//  SOTA: RESPUESTA A CAMBIOS (El Motor reacciona)
// ==============================================================================
void NOVAAudioProcessor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    // Si se añadió un pedal a nuestro estado principal...
    if (parent == pluginState && child.hasType(IDs::PEDAL_TAG))
    {
        // 1. OBTENER TIPO
        juce::String type = child.getProperty(IDs::PEDAL_TYPE);

        // 2. FACTORÍA AUTOMÁTICA (Zero Hardcode) 🏭
        // Le pedimos al Registry que cree el pedal correspondiente.
        // Si el tipo no existe en el mapa, devolverá nullptr automáticamente.
        std::unique_ptr<juce::AudioProcessor> newPedalParams = PedalRegistry::createPedal(type);

        // 3. SI EL PEDAL SE CREÓ CON ÉXITO...
        if (newPedalParams)
        {
            // A. Añadirlo al grafo de audio (Transferimos la propiedad con std::move)
            auto node = mainGraph->addNode(std::move(newPedalParams));

            // B. Guardarlo en nuestra lista interna
            nodesChain.push_back(node);

            // C. Recablear todo
            syncAudioGraph();
        }
    }
}

void NOVAAudioProcessor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == pluginState)
    {
        // Si borramos un pedal, reconstruimos toda la cadena para cerrar el hueco.
        rebuildChain();
    }
}

// ==============================================================================
//  SOTA: ENRUTAMIENTO INTELIGENTE
// ==============================================================================
void NOVAAudioProcessor::syncAudioGraph()
{
    // 1. LIMPIEZA DE CABLES (No borramos nodos, solo conexiones)
    // Esto es vital para que los pedales no desaparezcan al recablear.
    for (auto& c : mainGraph->getConnections())
        mainGraph->removeConnection(c);

    // 2. GESTIÓN DE NODOS DE SISTEMA (Input/Output)
    // Buscamos si ya existen para no duplicarlos
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

    // Si no existen (arranque limpio), los creamos
    if (inputNode == nullptr)
        inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));

    if (outputNode == nullptr)
        outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    // 3. RECABLEADO (Routing)
    if (nodesChain.empty())
    {
        // Bypass directo: Entrada -> Salida
        connectNodes(inputNode, outputNode);
    }
    else
    {
        // Cadena: Entrada -> Pedal 1
        connectNodes(inputNode, nodesChain.front());

        // Pedal -> Pedal
        for (size_t i = 0; i < nodesChain.size() - 1; ++i)
            connectNodes(nodesChain[i], nodesChain[i + 1]);

        // Último Pedal -> Salida
        connectNodes(nodesChain.back(), outputNode);
    }

    // 4. INICIALIZACIÓN SEGURA (LA SOLUCIÓN AL CRASH)
    double rate = getSampleRate();
    int blockSize = getBlockSize();

    // --- ESCUDO CRÍTICO ---
    // Solo llamamos a setPlayConfigDetails si el motor de audio YA arrancó.
    // Si rate es 0 (al abrir la app), saltamos esto. JUCE llamará a prepareToPlay
    // automáticamente unos milisegundos después.
    if (rate > 0.0 && blockSize > 0)
    {
        // A. Configurar Grafo Principal
        mainGraph->setPlayConfigDetails(getTotalNumInputChannels(),
            getTotalNumOutputChannels(),
            rate, blockSize);
        mainGraph->prepareToPlay(rate, blockSize);

        // B. Configurar y Despertar Pedales Hijos
        for (auto node : nodesChain)
        {
            if (auto* p = node->getProcessor())
            {
                p->setPlayConfigDetails(mainGraph->getMainBusNumInputChannels(),
                    mainGraph->getMainBusNumOutputChannels(),
                    rate, blockSize);
                p->prepareToPlay(rate, blockSize);
            }
        }
    }
}
void NOVAAudioProcessor::connectNodes(juce::AudioProcessorGraph::Node::Ptr source, juce::AudioProcessorGraph::Node::Ptr dest)
{
    if (!source || !dest) return;

    // Detectar si es el Nodo de Entrada del Sistema
    bool isSystemInput = (dynamic_cast<juce::AudioProcessorGraph::AudioGraphIOProcessor*>(source->getProcessor()) != nullptr &&
        source->getProcessor()->getTotalNumInputChannels() == 0);

    if (isSystemInput)
    {
        // === LA CORRECCIÓN ===
        // En lugar de conectar 1->1 y 2->2, conectamos "TODOS contra TODOS".
        // Esto suma la señal de la Entrada 2 en la entrada Izquierda del pedal.

        int numInputChannels = source->getProcessor()->getTotalNumOutputChannels();
        int numPedalInputs = dest->getProcessor()->getTotalNumInputChannels();

        for (int inCh = 0; inCh < numInputChannels; ++inCh)
        {
            // Conectar este canal de entrada al Canal L del pedal (0)
            // (JUCE suma las señales automáticamente si hay varios cables al mismo pin)
            mainGraph->addConnection({ { source->nodeID, inCh }, { dest->nodeID, 0 } });

            // Si el pedal es estéreo, conectamos también al Canal R (1)
            if (numPedalInputs > 1)
            {
                mainGraph->addConnection({ { source->nodeID, inCh }, { dest->nodeID, 1 } });
            }
        }
    }
    else
    {
        // Conexión normal entre pedales (L->L, R->R)
        mainGraph->addConnection({ { source->nodeID, 0 }, { dest->nodeID, 0 } });

        // Si ambos son estéreo, conectamos el derecho
        if (source->getProcessor()->getTotalNumOutputChannels() > 1 &&
            dest->getProcessor()->getTotalNumInputChannels() > 1)
        {
            mainGraph->addConnection({ { source->nodeID, 1 }, { dest->nodeID, 1 } });
        }
    }
}

void NOVAAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    DBG("--- PREPARE TO PLAY (El Driver arrancó) ---");
    DBG("Nuevo Rate: " << sampleRate);

    // 1. Configurar Grafo Principal
    mainGraph->setPlayConfigDetails(getTotalNumInputChannels(),
        getTotalNumOutputChannels(),
        sampleRate, samplesPerBlock);
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

    // 2. DESPERTAR A LOS PEDALES DORMIDOS
    // Iteramos sobre los pedales que cargamos en rebuildChain y los inicializamos ahora que es seguro.
    for (auto node : nodesChain)
    {
        if (auto* p = node->getProcessor())
        {
            p->setPlayConfigDetails(mainGraph->getMainBusNumInputChannels(),
                mainGraph->getMainBusNumOutputChannels(),
                sampleRate, samplesPerBlock);
            p->prepareToPlay(sampleRate, samplesPerBlock);
        }
    }

    // 3. Reconectar y proteger
    syncAudioGraph();
    startupGuard = 200; // 100 bloques de silencio para estabilizar el sonido sucio
}
void NOVAAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // 1. Limpieza de seguridad de canales extra
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // === NUEVO: LÓGICA DE CALENTAMIENTO ===
    if (startupGuard > 0)
    {
        // Disminuimos el contador
        startupGuard--;

        // Silencio absoluto (Mute)
        buffer.clear();

        // Importante: No llamamos al grafo todavía.
        // Esto evita que los pedales procesen la "basura" del arranque.
        return;
    }

    // 2. EL MOTOR (Solo corre si el startupGuard llegó a 0)
    mainGraph->processBlock(buffer, midiMessages);
}
// ==============================================================================
//  BOILERPLATE DE JUCE (CÓDIGO ESTÁNDAR OBLIGATORIO)
//  Pega esto en PluginProcessor.cpp para solucionar los LNK2001
// ==============================================================================

const juce::String NOVAAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NOVAAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool NOVAAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool NOVAAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double NOVAAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int NOVAAudioProcessor::getNumPrograms()
{
    return 1;   // NB: Algunos hosts fallan si dices 0, así que devuelve 1.
}

int NOVAAudioProcessor::getCurrentProgram()
{
    return 0;
}

void NOVAAudioProcessor::setCurrentProgram(int index)
{
}

const juce::String NOVAAudioProcessor::getProgramName(int index)
{
    return {};
}

void NOVAAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
}

void NOVAAudioProcessor::releaseResources()
{
    // Aquí puedes liberar memoria cuando el audio se detiene, si fuera necesario.
}

bool NOVAAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    // Solo soportamos Mono o Stereo
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Input debe coincidir con Output (excepto en sintes)
#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
#endif
}

bool NOVAAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* NOVAAudioProcessor::createEditor()
{
    return new NOVAAudioProcessorEditor(*this);
}

void NOVAAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // GUARDADO AUTOMÁTICO
    // Convertimos el árbol a binario y listo.
    juce::MemoryOutputStream stream(destData, true);
    pluginState.writeToStream(stream);
}

void NOVAAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto tree = juce::ValueTree::readFromData(data, sizeInBytes);
    if (tree.isValid() && tree.hasType(IDs::MAIN_STATE))
    {
        pluginState.removeListener(this); // Pausa
        pluginState.copyPropertiesAndChildrenFrom(tree, nullptr); // Carga datos
        rebuildChain(); // Reconstruye audio
        pluginState.addListener(this); // Play
    }
}
void NOVAAudioProcessor::rebuildChain()
{
    // 1. Limpieza
    mainGraph->clear();
    nodesChain.clear();

    // 2. Solo reconstruimos la lista de nodos (Sin configurar audio todavía)
    for (const auto& child : pluginState)
    {
        if (child.hasType(IDs::PEDAL_TAG))
        {
            juce::String type = child.getProperty(IDs::PEDAL_TYPE);

            // --- ZERO HARDCODE ---
            // Usamos la factoría. Si mañana inventas 100 pedales, 
            // solo tocas PedalRegistry.h, no este archivo.
            std::unique_ptr<juce::AudioProcessor> newPedal = PedalRegistry::createPedal(type);

            if (newPedal)
            {
                auto node = mainGraph->addNode(std::move(newPedal));
                nodesChain.push_back(node);
            }
        }
    }

    // 3. Delegamos la conexión y el encendido al experto
    syncAudioGraph();
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NOVAAudioProcessor();
}
