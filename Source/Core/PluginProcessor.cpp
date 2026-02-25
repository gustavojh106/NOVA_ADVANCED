#include "PluginProcessor.h"
#include "PluginEditor.h"

// ==============================================================================
// Constructor / Destructor
// ==============================================================================

NOVAAudioProcessor::NOVAAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
    , pluginState(Nova::IDs::MAIN_STATE)
{
    // Estructura inicial + defaults
    ensureStateStructure();
    applyDefaultStateIfNeeded();

    pluginState.addListener(this);
}

NOVAAudioProcessor::~NOVAAudioProcessor()
{
    pluginState.removeListener(this);
}

// ==============================================================================
// JUCE AudioProcessor
// ==============================================================================

void NOVAAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Pasamos canales reales (mono/stereo) al motor
    audioEngine.prepare(sampleRate,
        samplesPerBlock,
        getTotalNumInputChannels(),
        getTotalNumOutputChannels());

    updateMixerFromState();
}

void NOVAAudioProcessor::releaseResources()
{
}

void NOVAAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Delegamos al motor (tuner/bypass/processing). No limpiar buffer aquí.
    audioEngine.process(buffer, midi);

    // Visualizador (thread-safe push)
    audioVisualizer.pushBuffer(buffer);
}

// ==============================================================================
// Editor / Metadata (boilerplate)
// ==============================================================================

juce::AudioProcessorEditor* NOVAAudioProcessor::createEditor()
{
    // JUCE requiere puntero crudo en la firma.
    return new NOVAAudioProcessorEditor(*this);
}

bool NOVAAudioProcessor::hasEditor() const { return true; }

const juce::String NOVAAudioProcessor::getName() const { return "NOVA"; }

bool NOVAAudioProcessor::acceptsMidi() const { return false; }
bool NOVAAudioProcessor::producesMidi() const { return false; }
double NOVAAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int NOVAAudioProcessor::getNumPrograms() { return 1; }
int NOVAAudioProcessor::getCurrentProgram() { return 0; }
void NOVAAudioProcessor::setCurrentProgram(int) {}
const juce::String NOVAAudioProcessor::getProgramName(int) { return {}; }
void NOVAAudioProcessor::changeProgramName(int, const juce::String&) {}

bool NOVAAudioProcessor::isBusesLayoutSupported(const BusesLayout&) const
{
    // Aceptamos todo para evitar problemas (tu comportamiento original)
    return true;
}

// ==============================================================================
// State serialization
// ==============================================================================

void NOVAAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream stream(destData, true);
    pluginState.writeToStream(stream);
}

void NOVAAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    return;
    auto loaded = juce::ValueTree::readFromData(data, sizeInBytes);

    if (loaded.isValid() && loaded.hasType(Nova::IDs::MAIN_STATE))
        pluginState.copyPropertiesAndChildrenFrom(loaded, nullptr);

    // Asegura estructura + defaults mínimos
    ensureStateStructure();

    // === REQUISITO 3: FORZAR STOP AL CARGAR ===
    // Ignoramos el archivo guardado, siempre arranca parado.
    auto settings = getSettingsTree();
    if (settings.isValid())
        settings.setProperty(Nova::IDs::ENGINE_ON, false, nullptr);

    updateMixerFromState();
}

// ==============================================================================
// Public API (Editor commands)
// ==============================================================================

double NOVAAudioProcessor::getCpuUsage() const
{
    return audioEngine.getCpuLoad();
}

void NOVAAudioProcessor::requestAddPedal(const juce::String& type, Nova::ChainID chain, Nova::ZoneID zone)
{
    // 1. REGLA DE NEGOCIO ESTRICTA (Protección contra el Overlay y botones mal configurados)
    bool isAmp = type.containsIgnoreCase("Amp");
    bool isCab = type.containsIgnoreCase("Cab");

    Nova::ZoneID finalZone = zone;

    // Si el motor detecta qué es realmente, fuerza su zona correcta
    if (isAmp) finalZone = Nova::ZoneID::Amp;
    else if (isCab) finalZone = Nova::ZoneID::Cabinet;
    else if (zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet)
        finalZone = Nova::ZoneID::Pre; // Fallback: si intentan meter un pedal normal en Amp/Cab, lo mandamos al Pre

    // 2. Guardado seguro
    juce::ValueTree newPedal(Nova::IDs::PEDAL);
    newPedal.setProperty(Nova::IDs::PEDAL_TYPE, type, nullptr);
    newPedal.setProperty(Nova::IDs::PEDAL_ZONE, static_cast<int>(finalZone), nullptr);

    auto list = getLineTree(chain);

    // Seguimos insertando al final (-1) para mantener sincronizado el Índice visual con el AudioEngine
    list.addChild(newPedal, -1, nullptr);
}

void NOVAAudioProcessor::requestRemovePedal(Nova::ChainID chain, int index)
{
    getLineTree(chain).removeChild(index, nullptr);
}

void NOVAAudioProcessor::requestBypassPedal(Nova::ChainID chain, int index, bool bypassed)
{
    // Instantáneo (sin cortes)
    audioEngine.setPedalBypassed(chain, index, bypassed);

    // Persistencia opcional en ValueTree (igual que tu comentario original, no se implementa aquí)
}

void NOVAAudioProcessor::toggleEngine()
{
    auto settings = getSettingsTree();
    if (!settings.isValid())
        return;

    const bool isOn = (bool)settings.getProperty(Nova::IDs::ENGINE_ON);
    settings.setProperty(Nova::IDs::ENGINE_ON, !isOn, nullptr);
}

void NOVAAudioProcessor::cycleSwitcher()
{
    auto settings = getSettingsTree();
    if (!settings.isValid())
        return;

    int mode = (int)settings.getProperty(Nova::IDs::SWITCH_MODE);
    mode = (mode + 1) % 3;
    settings.setProperty(Nova::IDs::SWITCH_MODE, mode, nullptr);
}

void NOVAAudioProcessor::toggleTuner()
{
    const bool newState = !audioEngine.getTunerEnabled();
    audioEngine.setTunerEnabled(newState);
}

// ==============================================================================
// ValueTree Listener
// ==============================================================================

void NOVAAudioProcessor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (!child.hasType(Nova::IDs::PEDAL))
        return;

    Nova::ChainID chain;
    if (parent.hasType(Nova::IDs::LINE_A)) chain = Nova::ChainID::LineA;
    else if (parent.hasType(Nova::IDs::LINE_B)) chain = Nova::ChainID::LineB;
    else return;

    audioEngine.addPedal(child.getProperty(Nova::IDs::PEDAL_TYPE),
        chain,
        parent.indexOf(child));
}

void NOVAAudioProcessor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int index)
{
    if (parent.hasType(Nova::IDs::LINE_A))
        audioEngine.removePedal(Nova::ChainID::LineA, index);
    else if (parent.hasType(Nova::IDs::LINE_B))
        audioEngine.removePedal(Nova::ChainID::LineB, index);
}

void NOVAAudioProcessor::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (property == Nova::IDs::ENGINE_ON)
    {
        const bool shouldBeOn = (bool)tree.getProperty(property);
        audioEngine.setEngineEnabled(shouldBeOn);
        return;
    }

    if (property == Nova::IDs::SWITCH_MODE)
    {
        updateGlobalParamsFromState();
        return;
    }

    // Parámetros globales (Input + Mixer + Output)
    if (property == Nova::IDs::INPUT_GAIN ||
        property == Nova::IDs::INPUT_GATE ||
        property == Nova::IDs::FORCE_MONO ||
        property == Nova::IDs::INPUT_TRANS ||

        // MIXER A
        property == Nova::IDs::MIXER_GAIN_A ||
        property == Nova::IDs::MIXER_PAN_A ||
        property == Nova::IDs::MIXER_WIDTH_A ||

        // MIXER B
        property == Nova::IDs::MIXER_GAIN_B ||
        property == Nova::IDs::MIXER_PAN_B ||
        property == Nova::IDs::MIXER_WIDTH_B ||

        // OUTPUT
        property == Nova::IDs::OUTPUT_VOL ||
        property == Nova::IDs::OUTPUT_LIMITER ||
        property == Nova::IDs::OUTPUT_MIX)
    {
        updateGlobalParamsFromState();
        return;
    }
}

// ==============================================================================
// Internal helpers
// ==============================================================================

juce::ValueTree NOVAAudioProcessor::getSettingsTree() const
{
    return pluginState.getChildWithName(Nova::IDs::SETTINGS);
}

juce::ValueTree NOVAAudioProcessor::getLineTree(Nova::ChainID chain) const
{
    const auto id = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
    return pluginState.getChildWithName(id);
}

void NOVAAudioProcessor::ensureStateStructure()
{
    // MAIN_STATE ya existe como raíz
    if (!pluginState.getChildWithName(Nova::IDs::SETTINGS).isValid())
        pluginState.appendChild(juce::ValueTree(Nova::IDs::SETTINGS), nullptr);

    if (!pluginState.getChildWithName(Nova::IDs::LINE_A).isValid())
        pluginState.appendChild(juce::ValueTree(Nova::IDs::LINE_A), nullptr);

    if (!pluginState.getChildWithName(Nova::IDs::LINE_B).isValid())
        pluginState.appendChild(juce::ValueTree(Nova::IDs::LINE_B), nullptr);
}

void NOVAAudioProcessor::applyDefaultStateIfNeeded()
{
    auto settings = getSettingsTree();
    if (!settings.isValid())
        return;

    // === REQUISITO 3: SIEMPRE STOP AL INICIO ===
    settings.setProperty(Nova::IDs::ENGINE_ON, false, nullptr);

    // Defaults
    settings.setProperty(Nova::IDs::SWITCH_MODE, (int)Nova::SwitcherMode::Dual_Parallel, nullptr);
    settings.setProperty(Nova::IDs::MIXER_GAIN_A, 1.0f, nullptr);
    settings.setProperty(Nova::IDs::MIXER_GAIN_B, 1.0f, nullptr);
}

void NOVAAudioProcessor::updateGlobalParamsFromState()
{
    audioEngine.updateGlobalParams(getSettingsTree(),
        pluginState.getChildWithName(Nova::IDs::LINE_A),
        pluginState.getChildWithName(Nova::IDs::LINE_B));
}

void NOVAAudioProcessor::updateMixerFromState()
{
    auto s = getSettingsTree();
    if (!s.isValid())
        return;

    const float gA = (float)s.getProperty(Nova::IDs::MIXER_GAIN_A);
    const float gB = (float)s.getProperty(Nova::IDs::MIXER_GAIN_B);
    const int mode = (int)s.getProperty(Nova::IDs::SWITCH_MODE);

    audioEngine.updateMixer(gA, gB, (Nova::SwitcherMode)mode);
}

// ==============================================================================
// Factory
// ==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NOVAAudioProcessor();
}
