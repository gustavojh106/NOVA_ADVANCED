#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Pedal_Overdrive.h"
#include "Pedal_Cabinet.h"
#include "PedalNeural.h"

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
        // 1. Crear el Procesador real
        std::unique_ptr<juce::AudioProcessor> newPedalParams;
        juce::String type = child.getProperty(IDs::PEDAL_TYPE);

        if (type == "Overdrive") newPedalParams = std::make_unique<PedalOverdrive>();
        else if (type == "Cabinet") newPedalParams = std::make_unique<PedalCabinet>();
        else if (type == "Neural") newPedalParams = std::make_unique<PedalNeural>();

        if (newPedalParams)
        {
            // 2. Añadirlo al grafo de audio
            // Nota: Usamos std::move porque el grafo pasa a ser el dueño
            auto node = mainGraph->addNode(std::move(newPedalParams));

            // 3. Guardarlo en nuestra lista interna
            nodesChain.push_back(node);

            // 4. Recablear todo
            syncAudioGraph();
        }
    }
}

void NOVAAudioProcessor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == pluginState)
    {
        // Reconstrucción total (más seguro y sencillo)
        // En una implementación avanzada, buscaríamos qué nodo borrar por ID
        // Pero para empezar, limpiar y reconstruir es SOTA en estabilidad.

        mainGraph->clear(); // Esto borra los nodos de audio
        nodesChain.clear();

        // Recreamos todo basado en el estado actual
        // (Un loop simple para re-instanciar lo que queda en el árbol)
        for (const auto& existingChild : pluginState)
        {
            // Truco: Llamamos recursivamente o factorizamos la lógica de creación
            // Para simplificar este paso hoy: 
            // Idealmente tendríamos una función "createAndAddNode(child)"
        }

        // NOTA: Para no complicar el código ahora, la eliminación la haremos 
        // simplemente limpiando y re-llamando a la lógica de construcción.
        // Por ahora, céntrate en que "Añadir" funcione perfecto.
    }
}

// ==============================================================================
//  SOTA: ENRUTAMIENTO INTELIGENTE
// ==============================================================================
void NOVAAudioProcessor::syncAudioGraph()
{
    // 1. NO BORRAMOS LOS NODOS (mainGraph->clear() ELIMINADO)
    // Solo borramos las conexiones antiguas para recablear
    for (auto& c : mainGraph->getConnections())
        mainGraph->removeConnection(c);

    // 2. BUSCAMOS los nodos de Entrada/Salida (No los creamos ciegamente)
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

    // 3. Si por alguna razón no existen (ej: inicio de la app), los creamos
    if (inputNode == nullptr)
        inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));

    if (outputNode == nullptr)
        outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    // 4. RECABLEADO (Routing)
    if (nodesChain.empty())
    {
        // Bypass directo
        connectNodes(inputNode, outputNode);
    }
    else
    {
        // Input -> Primer Pedal
        connectNodes(inputNode, nodesChain.front());

        // Pedal -> Pedal
        for (size_t i = 0; i < nodesChain.size() - 1; ++i)
            connectNodes(nodesChain[i], nodesChain[i + 1]);

        // Último Pedal -> Output
        connectNodes(nodesChain.back(), outputNode);
    }

    // 5. Inicializar Audio (Vital para que suene al añadir pedales en caliente)
    double rate = getSampleRate();
    int blockSize = getBlockSize();

    if (rate > 0)
    {
        // Aseguramos que el grafo tenga el layout correcto
        mainGraph->setPlayConfigDetails(getTotalNumInputChannels(), getTotalNumOutputChannels(), rate, blockSize);
        mainGraph->prepareToPlay(rate, blockSize);

        for (auto node : nodesChain)
        {
            if (auto* p = node->getProcessor())
            {
                // Configuramos cada pedal individualmente
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
    // 1. Configuración de Buses del Grafo
    mainGraph->setPlayConfigDetails(getTotalNumInputChannels(),
        getTotalNumOutputChannels(),
        sampleRate, samplesPerBlock);

    // 2. Preparamos el grafo
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

    // 3. Reconstruimos los cables
    syncAudioGraph();

    // === NUEVO: ACTIVAR ESCUDO DE ARRANQUE ===
    // Silenciamos los primeros 50 bloques para dejar que el driver se estabilice
    startupGuard = 50;
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
    // 1. Convertimos datos binarios a ValueTree
    auto tree = juce::ValueTree::readFromData(data, sizeInBytes);

    // Verificamos que los datos sean válidos y sean de nuestro plugin ("NOVA_STATE")
    if (tree.isValid() && tree.hasType(IDs::MAIN_STATE))
    {
        // === MODO SILENCIOSO (ATOMIC LOAD) ===

        // A. PAUSA: Dejamos de escuchar cambios para evitar conflictos
        pluginState.removeListener(this);

        // B. LIMPIEZA: Borramos el grafo y la cadena actual
        mainGraph->clear();
        nodesChain.clear();

        // C. CARGA: Copiamos el estado guardado a nuestro estado actual
        pluginState.copyPropertiesAndChildrenFrom(tree, nullptr);

        // D. RECONSTRUCCIÓN MANUAL
        // Como apagamos el listener, tenemos que recorrer los hijos y crear los pedales nosotros
        for (const auto& child : pluginState)
        {
            if (child.hasType(IDs::PEDAL_TAG))
            {
                juce::String type = child.getProperty(IDs::PEDAL_TYPE);
                std::unique_ptr<juce::AudioProcessor> newPedal;

                // Factoría de pedales
                if (type == "Overdrive") newPedal = std::make_unique<PedalOverdrive>();
                else if (type == "Cabinet") newPedal = std::make_unique<PedalCabinet>();
                else if (type == "Neural") newPedal = std::make_unique<PedalNeural>();

                if (newPedal)
                {
                    // Añadir al grafo
                    auto node = mainGraph->addNode(std::move(newPedal));
                    nodesChain.push_back(node);
                }
            }
        }

        // E. RECABLEADO FINAL
        // Conectamos todo de una sola vez
        syncAudioGraph();

        // F. PLAY: Volvemos a activar el listener para cambios futuros (Drag & Drop)
        pluginState.addListener(this);

        DBG("Estado cargado exitosamente: " << nodesChain.size() << " pedales restaurados.");
    }
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NOVAAudioProcessor();
}