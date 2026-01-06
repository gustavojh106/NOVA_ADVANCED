#include "PluginProcessor.h"
#include "PluginEditor.h"

NOVAAudioProcessor::NOVAAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo()).withOutput("Out", juce::AudioChannelSet::stereo())),
    pluginState(Nova::IDs::MAIN_STATE)
{
    // Estructura Inicial
    pluginState.appendChild(juce::ValueTree(Nova::IDs::SETTINGS), nullptr);
    pluginState.appendChild(juce::ValueTree(Nova::IDs::LINE_A), nullptr);
    pluginState.appendChild(juce::ValueTree(Nova::IDs::LINE_B), nullptr);

    // Valores por defecto
    auto settings = pluginState.getChildWithName(Nova::IDs::SETTINGS);

    // === REQUISITO 3: SIEMPRE STOP AL INICIO ===
    settings.setProperty(Nova::IDs::ENGINE_ON, false, nullptr);

    settings.setProperty(Nova::IDs::SWITCH_MODE, (int)Nova::SwitcherMode::Dual_Parallel, nullptr);
    settings.setProperty(Nova::IDs::MIXER_GAIN_A, 1.0f, nullptr);
    settings.setProperty(Nova::IDs::MIXER_GAIN_B, 1.0f, nullptr);

    pluginState.addListener(this);
}

NOVAAudioProcessor::~NOVAAudioProcessor() { pluginState.removeListener(this); }

void NOVAAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // IMPORTANTE: Pasamos los canales reales para que el AudioEngine sepa si es Mono o Stereo
    audioEngine.prepare(sampleRate, samplesPerBlock, getTotalNumInputChannels(), getTotalNumOutputChannels());
    updateMixerFromState();
}

void NOVAAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // IMPORTANTE: NO borrar el buffer aquí manualmente antes de procesar,
    // el AudioEngine ya decide si silencia (stop) o procesa.
    // Si haces buffer.clear() aquí antes, borras la entrada de guitarra.

    audioEngine.process(buffer, midi);

    // Manda copia al visualizador
    audioVisualizer.pushBuffer(buffer);
}

// === LÓGICA DE NEGOCIO ===
double NOVAAudioProcessor::getCpuUsage() const
{
    // Llama al método del AudioEngine que creamos anteriormente
    return audioEngine.getCpuLoad();
}
void NOVAAudioProcessor::requestAddPedal(const juce::String& type, Nova::ChainID chain, Nova::ZoneID zone)
{
    juce::ValueTree newPedal(Nova::IDs::PEDAL_TAG);
    newPedal.setProperty(Nova::IDs::PEDAL_TYPE, type, nullptr);
    newPedal.setProperty(Nova::IDs::PEDAL_ZONE, (int)zone, nullptr);

    auto listID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
    auto list = pluginState.getChildWithName(listID);

    // Insertar Ordenado por Zona
    int insertIndex = list.getNumChildren();
    for (int i = 0; i < list.getNumChildren(); ++i)
    {
        int z = list.getChild(i).getProperty(Nova::IDs::PEDAL_ZONE);
        if (z > (int)zone) { insertIndex = i; break; }
    }
    list.addChild(newPedal, insertIndex, nullptr);
}

void NOVAAudioProcessor::requestRemovePedal(Nova::ChainID chain, int index)
{
    auto listID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
    pluginState.getChildWithName(listID).removeChild(index, nullptr);
}

void NOVAAudioProcessor::toggleEngine()
{
    auto settings = pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (settings.isValid())
    {
        bool isOn = settings.getProperty(Nova::IDs::ENGINE_ON);
        settings.setProperty(Nova::IDs::ENGINE_ON, !isOn, nullptr);
    }
}

void NOVAAudioProcessor::cycleSwitcher()
{
    auto s = pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (s.isValid())
    {
        int mode = s.getProperty(Nova::IDs::SWITCH_MODE);
        mode = (mode + 1) % 3;
        s.setProperty(Nova::IDs::SWITCH_MODE, mode, nullptr);
    }
}

// === VALUE TREE LISTENER ===

void NOVAAudioProcessor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (child.hasType(Nova::IDs::PEDAL_TAG))
    {
        Nova::ChainID chain;
        if (parent.hasType(Nova::IDs::LINE_A)) chain = Nova::ChainID::LineA;
        else if (parent.hasType(Nova::IDs::LINE_B)) chain = Nova::ChainID::LineB;
        else return;

        audioEngine.addPedal(child.getProperty(Nova::IDs::PEDAL_TYPE), chain, parent.indexOf(child));
    }
}

void NOVAAudioProcessor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int index)
{
    if (parent.hasType(Nova::IDs::LINE_A)) audioEngine.removePedal(Nova::ChainID::LineA, index);
    else if (parent.hasType(Nova::IDs::LINE_B)) audioEngine.removePedal(Nova::ChainID::LineB, index);
}

void NOVAAudioProcessor::valueTreePropertyChanged(juce::ValueTree& t, const juce::Identifier& p)
{
    if (t.hasType(Nova::IDs::SETTINGS))
    {
        if (p == Nova::IDs::ENGINE_ON) audioEngine.setEngineEnabled(t.getProperty(p));
        else updateMixerFromState();
    }
}

void NOVAAudioProcessor::updateMixerFromState()
{
    auto s = pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (s.isValid())
    {
        float gA = s.getProperty(Nova::IDs::MIXER_GAIN_A);
        float gB = s.getProperty(Nova::IDs::MIXER_GAIN_B);
        int m = s.getProperty(Nova::IDs::SWITCH_MODE);
        audioEngine.updateMixer(gA, gB, (Nova::SwitcherMode)m);
    }
}

// Boilerplate
juce::AudioProcessorEditor* NOVAAudioProcessor::createEditor() { return new NOVAAudioProcessorEditor(*this); }
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
bool NOVAAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const { return true; } // Aceptamos todo para evitar problemas
void NOVAAudioProcessor::getStateInformation(juce::MemoryBlock& d) { juce::MemoryOutputStream s(d, true); pluginState.writeToStream(s); }

void NOVAAudioProcessor::setStateInformation(const void* d, int s)
{
    auto t = juce::ValueTree::readFromData(d, s);
    if (t.isValid() && t.hasType(Nova::IDs::MAIN_STATE))
    {
        pluginState.copyPropertiesAndChildrenFrom(t, nullptr);
    }

    // REPARACIÓN Y FORZADO DE ESTADO STOP
    auto settings = pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (!settings.isValid())
    {
        pluginState.appendChild(juce::ValueTree(Nova::IDs::SETTINGS), nullptr);
        settings = pluginState.getChildWithName(Nova::IDs::SETTINGS);
        settings.setProperty(Nova::IDs::SWITCH_MODE, 0, nullptr);
    }

    // === REQUISITO 3: FORZAR STOP AL CARGAR ===
    // Ignoramos lo que diga el archivo guardado. Siempre arranca parado.
    settings.setProperty(Nova::IDs::ENGINE_ON, false, nullptr);

    // Asegurar estructura
    if (!pluginState.getChildWithName(Nova::IDs::LINE_A).isValid()) pluginState.appendChild(juce::ValueTree(Nova::IDs::LINE_A), nullptr);
    if (!pluginState.getChildWithName(Nova::IDs::LINE_B).isValid()) pluginState.appendChild(juce::ValueTree(Nova::IDs::LINE_B), nullptr);

    updateMixerFromState();
}
void NOVAAudioProcessor::requestBypassPedal(Nova::ChainID chain, int index, bool bypassed)
{
    // 1. Actualizamos el motor de audio (Instantáneo, sin cortes)
    audioEngine.setPedalBypassed(chain, index, bypassed);

    // 2. (Opcional) Aquí podrías guardar el estado en el ValueTree para persistencia
    // Por ahora lo dejamos solo funcional en el audio.
}
void NOVAAudioProcessor::releaseResources() {}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NOVAAudioProcessor(); }