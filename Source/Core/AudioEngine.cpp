#include "AudioEngine.h"
#include "PedalRegistry.h"

// ==========================================================
// IMPLEMENTACIÓN DE SIMPLE GAIN (Aquí mismo para no crear más archivos)
// ==========================================================

SimpleGainProcessor::SimpleGainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo())
        .withOutput("Output", juce::AudioChannelSet::stereo()))
{
    // IMPORTANTE: Default a 1.0 (Sonido ON)
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
    // Suavizado para evitar clicks al cambiar de canal (A/B)
    if (std::abs(currentGain - targetGain) > 0.001f)
        currentGain += (targetGain - currentGain) * 0.1f;
    else
        currentGain = targetGain;

    // Si la ganancia es casi cero, silenciamos para ahorrar procesamiento
    if (currentGain < 0.001f)
        buffer.clear();
    else
        buffer.applyGain(currentGain);
}

// ==========================================================
// IMPLEMENTACIÓN DE AUDIO ENGINE
// ==========================================================

AudioEngine::AudioEngine()
{
    mainGraph = std::make_unique<juce::AudioProcessorGraph>();
}

AudioEngine::~AudioEngine() { mainGraph->releaseResources(); }

void AudioEngine::prepare(double sampleRate, int samplesPerBlock, int numIn, int numOut)
{
    currentRate = sampleRate; 
    tunerBuffer.setSize(1, (int)(sampleRate * 0.1)); // ~100ms de audio
    tunerBuffer.clear();
    tunerWriteIndex = 0;
    currentBlockSize = samplesPerBlock;
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    numInputChannels = numIn;


    if (sampleRate > 0) {
        // CAMBIO: De 8192 a 4096. 
        // 4096 samples a 44.1kHz son ~92ms. Es el punto dulce entre velocidad y precisión de bajos.
        tunerBuffer.setSize(1, 4096);
        tunerBuffer.clear();
    }
    tunerWriteIndex = 0;
    currentRMS = 0.0f;

    mainGraph->setPlayConfigDetails(numIn, numOut, sampleRate, samplesPerBlock);
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);
    mainGraph->clear();

    // 1. Crear Nodos Fijos
    inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
    outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    gainNodeA = mainGraph->addNode(std::make_unique<SimpleGainProcessor>());
    gainNodeB = mainGraph->addNode(std::make_unique<SimpleGainProcessor>());

    // 2. Conectar las SALIDAS de las cadenas al OUTPUT del sistema
    // Esto mezcla A y B al final.
    for (auto* gainNode : { gainNodeA.get(), gainNodeB.get() })
    {
        // Conectamos L del Gain -> L del Output
        if (numOut > 0)
            mainGraph->addConnection({ { gainNode->nodeID, 0 }, { outputNode->nodeID, 0 } });

        // Conectamos R del Gain -> R del Output
        if (numOut > 1)
            mainGraph->addConnection({ { gainNode->nodeID, 1 }, { outputNode->nodeID, 1 } });
    }

    rebuildGraph();
}

void AudioEngine::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // 1. INICIO CRONÓMETRO
    // Tomamos el tiempo exacto antes de empezar a procesar
    auto startTime = juce::Time::getMillisecondCounterHiRes();
    if (tunerEnabled)
    {
        // Punteros de lectura
        auto* inL = buffer.getReadPointer(0);
        auto* inR = (buffer.getNumChannels() > 1) ? buffer.getReadPointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            // Sumamos mono para asegurar que escuchamos la guitarra esté donde esté
            float inputSample = inL[i];
            if (inR) inputSample += inR[i];

            // Escribir en buffer circular
            tunerBuffer.setSample(0, tunerWriteIndex, inputSample);
            tunerWriteIndex++;

            if (tunerWriteIndex >= tunerBuffer.getNumSamples())
            {
                tunerWriteIndex = 0;
                processTunerAlgorithm();
            }
        }

        buffer.clear(); // Silenciar salida
        // IMPORTANTE: NO hacemos return aquí para permitir que se calcule el CPU Usage abajo si quieres, 
        // pero para el afinador solemos hacer return. Dejémoslo con return y cpu=0.
        cpuUsage = 0.0;
        return;
    }
    // 2. Tu lógica original (Si está apagado, limpiar y salir)
    if (!isEngineOn)
    {
        buffer.clear();
        // Nota: Si el motor está apagado, no calculamos CPU (sería 0%)
        cpuUsage = 0.0;
        return;
    }
    if (startupCounter > 0)
    {
        startupCounter--;
        buffer.clear();
        return;
    }
    // 3. Procesamiento del Grafo (Lo pesado)
    mainGraph->processBlock(buffer, midi);

    // 4. FIN CRONÓMETRO Y CÁLCULO
    auto endTime = juce::Time::getMillisecondCounterHiRes();

    // Tiempo que tardó el procesador en hacer el trabajo (en milisegundos)
    double timeTakenMs = endTime - startTime;

    // Tiempo total disponible para este bloque de audio 
    // (Ej: si el buffer es de 512 samples a 44100Hz, tenemos ~11.6ms para procesar sin glitches)
    // Aseguramos que currentRate sea mayor a 0 para evitar división por cero
    if (currentRate > 0)
    {
        double blockDurationMs = (buffer.getNumSamples() / currentRate) * 1000.0;

        if (blockDurationMs > 0.0)
        {
            // Fórmula de carga: (Tiempo Tardado / Tiempo Disponible) * 100
            double currentLoad = (timeTakenMs / blockDurationMs) * 100.0;

            // 5. Suavizado (Smoothing)
            // Usamos un filtro simple (90% valor anterior + 10% valor nuevo) 
            // para que el número en pantalla no baile tan rápido y sea legible.
            cpuUsage = (cpuUsage * 0.9) + (currentLoad * 0.1);
        }
    }
}

void AudioEngine::updateMixer(float gA, float gB, Nova::SwitcherMode mode)
{
    if (!gainNodeA || !gainNodeB) return;

    float finalA = gA;
    float finalB = gB;

    // Lógica del Switcher: Silenciamos la ganancia de la línea inactiva
    if (mode == Nova::SwitcherMode::LineB_Only) finalA = 0.0f;
    if (mode == Nova::SwitcherMode::LineA_Only) finalB = 0.0f;

    // Actualizamos los procesadores
    if (auto* p = dynamic_cast<SimpleGainProcessor*>(gainNodeA->getProcessor())) p->setGain(finalA);
    if (auto* p = dynamic_cast<SimpleGainProcessor*>(gainNodeB->getProcessor())) p->setGain(finalB);
}

void AudioEngine::addPedal(const juce::String& type, Nova::ChainID chain, int index)
{
    mainGraph->suspendProcessing(true);
    if (auto pedal = PedalRegistry::createPedal(type))
    {
        pedal->setPlayConfigDetails(2, 2, currentSampleRate, currentBlockSize);
        pedal->prepareToPlay(currentSampleRate, currentBlockSize);
        auto node = mainGraph->addNode(std::move(pedal));

        auto& list = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;

        if (index >= 0 && index <= list.size())
            list.insert(list.begin() + index, node);
        else
            list.push_back(node);

        rebuildGraph();
    }
    mainGraph->suspendProcessing(false);
}

void AudioEngine::removePedal(Nova::ChainID chain, int index)
{
    mainGraph->suspendProcessing(true);
    auto& list = (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
    if (index >= 0 && index < list.size())
    {
        mainGraph->removeNode(list[index]->nodeID);
        list.erase(list.begin() + index);
        rebuildGraph();
    }
    mainGraph->suspendProcessing(false);
}

void AudioEngine::clearAll()
{
    mainGraph->suspendProcessing(true);
    nodesChainA.clear();
    nodesChainB.clear();
    rebuildGraph();
    mainGraph->suspendProcessing(false);
}

void AudioEngine::rebuildGraph()
{
    // Limpiamos SOLO las conexiones internas (dejamos los Gain->Output quietos)
    for (auto c : mainGraph->getConnections())
    {
        // Si el destino NO es el OutputNode, borramos la conexión para rehacerla
        if (c.destination.nodeID != outputNode->nodeID)
            mainGraph->removeConnection(c);
    }

    // Reconstruimos las dos líneas independientemente
    connectChainToGain(nodesChainA, gainNodeA->nodeID);
    connectChainToGain(nodesChainB, gainNodeB->nodeID);
}

void AudioEngine::connectChainToGain(const std::vector<juce::AudioProcessorGraph::Node::Ptr>& nodes,
    juce::AudioProcessorGraph::NodeID gainNodeID)
{
    juce::AudioProcessorGraph::NodeID currentSource = inputNode->nodeID;

    // Iteramos los pedales de esta cadena
    for (int i = 0; i < nodes.size(); ++i)
    {
        auto node = nodes[i];

        // CONEXIÓN INICIAL (Input -> Primer Pedal)
        if (i == 0)
        {
            // ESTRATEGIA "GREEDY INPUT" (Asegura sonido)
            // Conectamos TODAS las entradas de hardware disponibles a las entradas del pedal.

            // Entrada 0 (L) -> Pedal L y R
            mainGraph->addConnection({ { currentSource, 0 }, { node->nodeID, 0 } });
            mainGraph->addConnection({ { currentSource, 0 }, { node->nodeID, 1 } });

            // Si hay Entrada 1 (R), TAMBIÉN la conectamos (para sumar señales si es stereo o si la guitarra está en el 2)
            if (numInputChannels > 1)
            {
                mainGraph->addConnection({ { currentSource, 1 }, { node->nodeID, 0 } });
                mainGraph->addConnection({ { currentSource, 1 }, { node->nodeID, 1 } });
            }
        }
        else
        {
            // Pedal Intermedio -> Pedal Siguiente (Siempre Stereo L->L, R->R)
            mainGraph->addConnection({ { currentSource, 0 }, { node->nodeID, 0 } });
            mainGraph->addConnection({ { currentSource, 1 }, { node->nodeID, 1 } });
        }
        currentSource = node->nodeID;
    }

    // CONEXIÓN FINAL (Último nodo -> GainNode)
    // Si la cadena estaba vacía, currentSource sigue siendo el InputNode
    if (nodes.empty())
    {
        // Dry Signal directa
        mainGraph->addConnection({ { currentSource, 0 }, { gainNodeID, 0 } });
        mainGraph->addConnection({ { currentSource, 0 }, { gainNodeID, 1 } });

        if (numInputChannels > 1)
        {
            mainGraph->addConnection({ { currentSource, 1 }, { gainNodeID, 0 } });
            mainGraph->addConnection({ { currentSource, 1 }, { gainNodeID, 1 } });
        }
    }
    else
    {
        // Salida normal del último pedal
        mainGraph->addConnection({ { currentSource, 0 }, { gainNodeID, 0 } });
        mainGraph->addConnection({ { currentSource, 1 }, { gainNodeID, 1 } });
    }
}
int AudioEngine::getLatencyNumSamples() const
{
    // Si el grafo existe, preguntamos su latencia. Si no, devolvemos 0.
    return mainGraph ? mainGraph->getLatencySamples() : 0;
}
void AudioEngine::setEngineEnabled(bool enabled) { isEngineOn = enabled; }
double AudioEngine::getCpuLoad() const
{
    return cpuUsage;
}
const std::vector<juce::AudioProcessorGraph::Node::Ptr>& AudioEngine::getNodes(Nova::ChainID chain) const
{
    return (chain == Nova::ChainID::LineA) ? nodesChainA : nodesChainB;
}

//TUNER
void AudioEngine::setTunerEnabled(bool enabled) { tunerEnabled = enabled; }
bool AudioEngine::isTunerEnabled() const { return tunerEnabled; }
float AudioEngine::getTunerPitch() const { return currentPitch; }
int AudioEngine::getTunerNote() const { return currentNote; }
float AudioEngine::getTunerCents() const { return currentCents; }

// ALGORITMO DE AUTOCORRELACIÓN (YIN SIMPLIFICADO)
float AudioEngine::calculateFrequency(const float* signal, int numSamples, double sampleRate)
{
    // 1. Calcular Energía Total (Correlación en lag 0)
    // Esto nos sirve de referencia: ¿Qué tan fuerte es la señal consigo misma?
    float signalEnergy = 0.0f;
    for (int i = 0; i < numSamples / 2; ++i)
        signalEnergy += signal[i] * signal[i];

    // Si la energía es casi nula, salimos (Evita división por cero)
    if (signalEnergy < 0.00001f) return 0.0f;

    // Rango de búsqueda (Guitarra: 40Hz - 1000Hz)
    int minPeriod = (int)(sampleRate / 1000.0);
    int maxPeriod = (int)(sampleRate / 40.0);

    float maxCorrelation = 0.0f;
    int bestPeriod = 0;

    // 2. Autocorrelación
    for (int lag = minPeriod; lag < maxPeriod; ++lag)
    {
        float correlation = 0.0f;
        for (int i = 0; i < numSamples / 2; ++i)
            correlation += signal[i] * signal[i + lag];

        if (correlation > maxCorrelation)
        {
            maxCorrelation = correlation;
            bestPeriod = lag;
        }
    }

    // 3. FACTOR DE CLARIDAD (The "Magic Check")
    // Comparamos el mejor pico encontrado contra la energía total.
    // Una onda seno pura tendría claridad cercana a 1.0.
    // El ruido blanco suele tener claridad menor a 0.5.
    float clarity = maxCorrelation / signalEnergy;

    // UMBRAL DE CLARIDAD:
    // Si la claridad es menor a 0.6, es probable que sea ruido, no una nota musical.
    // Ajusta esto entre 0.5 (permisivo) y 0.8 (muy estricto).
    if (clarity < 0.6f) return 0.0f;

    // 4. Interpolación Parabólica (Solo si pasó el filtro de claridad)
    if (bestPeriod > 0 && bestPeriod < maxPeriod - 1)
    {
        float c1 = 0.0f, c2 = maxCorrelation, c3 = 0.0f;

        // Calcular vecinos para interpolar
        for (int i = 0; i < numSamples / 2; ++i) c1 += signal[i] * signal[i + bestPeriod - 1];
        for (int i = 0; i < numSamples / 2; ++i) c3 += signal[i] * signal[i + bestPeriod + 1];

        float denominator = c1 - 2 * c2 + c3;
        float delta = 0.0f;
        if (std::abs(denominator) > 0.0001f)
            delta = (c1 - c3) / (2.0f * denominator);

        return (float)(sampleRate / (bestPeriod - delta));
    }

    return 0.0f;
}
void AudioEngine::processTunerAlgorithm()
{
    // 1. "UNWRAP" del Buffer Circular (Esto se mantiene igual)
    int numSamples = tunerBuffer.getNumSamples();
    float rms = tunerBuffer.getRMSLevel(0, 0, numSamples);

    // Guardamos RMS para la UI
    currentRMS = rms;

    // Umbral de Silencio (Gate absoluto)
    if (rms < 0.001f) {
        currentPitch = 0.0f;
        stabilityCounter = 0; // Resetear estabilidad al silencio
        return;
    }

    std::vector<float> linearBuffer(numSamples);
    int samplesToEnd = numSamples - tunerWriteIndex;
    if (samplesToEnd > 0)
        juce::FloatVectorOperations::copy(linearBuffer.data(), tunerBuffer.getReadPointer(0, tunerWriteIndex), samplesToEnd);
    if (tunerWriteIndex > 0)
        juce::FloatVectorOperations::copy(linearBuffer.data() + samplesToEnd, tunerBuffer.getReadPointer(0, 0), tunerWriteIndex);


    // 2. Calcular Frecuencia (Usando el nuevo filtro de Claridad)
    float newFreq = calculateFrequency(linearBuffer.data(), numSamples, currentRate);

    // 3. LÓGICA DE ESTABILIDAD (DEBOUNCE)
    // Si calculateFrequency devolvió 0.0 (ruido), reseteamos.
    if (newFreq <= 0.0f)
    {
        stabilityCounter = 0;
        // Opcional: Si llevamos mucho tiempo sin señal clara, borramos la nota
        // currentPitch = 0.0f; 
        return;
    }

    // Comparamos con la última frecuencia detectada (con un margen de error del 5%)
    float ratio = newFreq / (lastDetectedFreq + 0.001f);
    bool isSameNote = (ratio > 0.95f && ratio < 1.05f);

    if (isSameNote)
    {
        // Si es la misma nota, aumentamos confianza
        stabilityCounter++;
    }
    else
    {
        // Si cambió drásticamente, reseteamos el contador y guardamos esta como posible candidata
        stabilityCounter = 0;
        lastDetectedFreq = newFreq;
    }

    // 4. ACTUALIZAR UI SOLO SI ES ESTABLE
    // Solo actualizamos la aguja si hemos detectado la misma nota 2 veces seguidas
    if (stabilityCounter >= 2)
    {
        // Suavizado final (LPF) para la aguja
        float smoothedPitch = currentPitch.load();
        if (smoothedPitch <= 1.0f) smoothedPitch = newFreq; // Primera vez
        else smoothedPitch = smoothedPitch * 0.6f + newFreq * 0.4f;

        currentPitch = smoothedPitch;

        // Math MIDI
        float midiNoteFloat = 69.0f + 12.0f * std::log2(smoothedPitch / 440.0f);
        int noteIndex = (int)std::round(midiNoteFloat);
        float cents = (midiNoteFloat - noteIndex) * 100.0f;

        currentNote = noteIndex;
        currentCents = cents;

        // Evitamos que el contador crezca infinito
        stabilityCounter = 3;
    }
}