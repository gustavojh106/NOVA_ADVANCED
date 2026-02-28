#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PedalRegistry.h"

namespace
{
juce::File getStartupPresetPointerFile()
{
    auto appDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("NOVA");

    if (!appDir.exists())
        appDir.createDirectory();

    return appDir.getChildFile("startup-preset.txt");
}

void writeStartupPresetFile(const juce::File& presetFile)
{
    auto pointerFile = getStartupPresetPointerFile();
    pointerFile.replaceWithText(presetFile.getFullPathName());
}
}

// ==============================================================================
// Constructor / Destructor
// ==============================================================================

NOVAAudioProcessor::NOVAAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
    , pluginState(Nova::IDs::MAIN_STATE)
{
    // Arranque limpio siempre: mismo comportamiento que botón CLEAR.
    clearSessionAndForgetStartupPreset();

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
    audioEngine.process(buffer, midi);
    audioVisualizer.pushBuffer(buffer);
}

// ==============================================================================
// Editor / Metadata
// ==============================================================================

juce::AudioProcessorEditor* NOVAAudioProcessor::createEditor()
{
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
    // Regla del sistema: no restaurar automaticamente la ultima sesion del host.
    // Mismo comportamiento que botón CLEAR en cada apertura/restauración.
    juce::ignoreUnused(data, sizeInBytes);
    clearSessionAndForgetStartupPreset();
}

bool NOVAAudioProcessor::savePresetToFile(const juce::File& file)
{
    auto stateToSave = pluginState.createCopy();
    sanitizeLine(stateToSave.getChildWithName(Nova::IDs::LINE_A));
    sanitizeLine(stateToSave.getChildWithName(Nova::IDs::LINE_B));
    
    auto savePedalStates = [this, &stateToSave](Nova::ChainID chain)
        {
            auto line = (chain == Nova::ChainID::LineA)
                ? stateToSave.getChildWithName(Nova::IDs::LINE_A)
                : stateToSave.getChildWithName(Nova::IDs::LINE_B);

            if (!line.isValid())
                return;

            for (int i = 0; i < line.getNumChildren(); ++i)
            {
                auto child = line.getChild(i);
                if (!child.hasType(Nova::IDs::PEDAL))
                    continue;

                if (auto* proc = audioEngine.getProcessorForPedal(chain, i))
                {
                    juce::MemoryBlock pedalState;
                    proc->getStateInformation(pedalState);

                    if (pedalState.getSize() > 0)
                    {
                        child.setProperty(Nova::IDs::PEDAL_STATE,
                            juce::Base64::toBase64(pedalState.getData(), pedalState.getSize()),
                            nullptr);
                    }
                    else
                    {
                        child.removeProperty(Nova::IDs::PEDAL_STATE, nullptr);
                    }
                }
            }
        };

    savePedalStates(Nova::ChainID::LineA);
    savePedalStates(Nova::ChainID::LineB);

    juce::MemoryOutputStream stream;
    stateToSave.writeToStream(stream);

    auto target = file;
    if (target.getFileExtension().isEmpty())
        target = target.withFileExtension(".nova-preset");

    if (auto parent = target.getParentDirectory(); !parent.exists())
        parent.createDirectory();

    const bool ok = target.replaceWithData(stream.getData(), stream.getDataSize());
    if (ok)
        writeStartupPresetFile(target);

    return ok;
}

bool NOVAAudioProcessor::loadPresetFromFile(const juce::File& file)
{
    juce::MemoryBlock data;
    if (!file.existsAsFile() || !file.loadFileAsData(data))
        return false;

    auto loaded = juce::ValueTree::readFromData(data.getData(), (int)data.getSize());
    if (!loaded.isValid() || !loaded.hasType(Nova::IDs::MAIN_STATE))
        return false;

    sanitizeLine(loaded.getChildWithName(Nova::IDs::LINE_A));
    sanitizeLine(loaded.getChildWithName(Nova::IDs::LINE_B));

    {
        const juce::ScopedValueSetter<bool> sv(suppressStateCallbacks, true);
        pluginState.copyPropertiesAndChildrenFrom(loaded, nullptr);
        ensureStateStructure();
        applyDefaultStateIfNeeded();
    }

    audioEngine.clearAll();

    auto rebuildLine = [this](Nova::ChainID chain)
        {
            auto line = getLineTree(chain);
            if (!line.isValid())
                return;

            for (int i = 0; i < line.getNumChildren(); ++i)
            {
                auto child = line.getChild(i);
                if (!child.hasType(Nova::IDs::PEDAL))
                    continue;

                audioEngine.addPedal(child.getProperty(Nova::IDs::PEDAL_TYPE).toString(), chain, i);
            }
        };

    rebuildLine(Nova::ChainID::LineA);
    rebuildLine(Nova::ChainID::LineB);

    auto restorePedalStates = [this](Nova::ChainID chain)
        {
            auto line = getLineTree(chain);
            if (!line.isValid())
                return;

            for (int i = 0; i < line.getNumChildren(); ++i)
            {
                auto child = line.getChild(i);
                if (!child.hasType(Nova::IDs::PEDAL))
                    continue;

                const auto encodedState = child.getProperty(Nova::IDs::PEDAL_STATE).toString();
                if (encodedState.isEmpty())
                    continue;

                juce::MemoryOutputStream decoded;
                if (!juce::Base64::convertFromBase64(decoded, encodedState))
                    continue;

                if (auto* proc = audioEngine.getProcessorForPedal(chain, i))
                    proc->setStateInformation(decoded.getData(), (int)decoded.getDataSize());
            }
        };

    restorePedalStates(Nova::ChainID::LineA);
    restorePedalStates(Nova::ChainID::LineB);

    updateGlobalParamsFromState();
    updateMixerFromState();
    audioEngine.setEngineEnabled((bool)getSettingsTree().getProperty(Nova::IDs::ENGINE_ON, false));
    writeStartupPresetFile(file);
    return true;
}

void NOVAAudioProcessor::clearSessionAndForgetStartupPreset()
{
    resetToCleanState();

    auto pointerFile = getStartupPresetPointerFile();
    if (pointerFile.existsAsFile())
        pointerFile.deleteFile();
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
    const auto canonicalType = PedalRegistry::canonicalType(type);
    if (!PedalRegistry::isTypeSupported(canonicalType))
        return;

    const auto finalZone = Nova::PedalCatalog::enforceZone(canonicalType, zone);

    juce::ValueTree newPedal(Nova::IDs::PEDAL);
    newPedal.setProperty(Nova::IDs::PEDAL_TYPE, canonicalType, nullptr);
    newPedal.setProperty(Nova::IDs::PEDAL_ZONE, static_cast<int>(finalZone), nullptr);

    auto list = getLineTree(chain);
    list.addChild(newPedal, -1, nullptr);
}

void NOVAAudioProcessor::requestRemovePedal(Nova::ChainID chain, int index)
{
    getLineTree(chain).removeChild(index, nullptr);
}

void NOVAAudioProcessor::requestBypassPedal(Nova::ChainID chain, int index, bool bypassed)
{
    audioEngine.setPedalBypassed(chain, index, bypassed);
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
    if (suppressStateCallbacks)
        return;

    if (!child.hasType(Nova::IDs::PEDAL))
        return;

    Nova::ChainID chain;
    if (parent.hasType(Nova::IDs::LINE_A)) chain = Nova::ChainID::LineA;
    else if (parent.hasType(Nova::IDs::LINE_B)) chain = Nova::ChainID::LineB;
    else return;

    audioEngine.addPedal(child.getProperty(Nova::IDs::PEDAL_TYPE), chain, parent.indexOf(child));
}

void NOVAAudioProcessor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int index)
{
    if (suppressStateCallbacks)
        return;

    if (parent.hasType(Nova::IDs::LINE_A))
        audioEngine.removePedal(Nova::ChainID::LineA, index);
    else if (parent.hasType(Nova::IDs::LINE_B))
        audioEngine.removePedal(Nova::ChainID::LineB, index);
}

void NOVAAudioProcessor::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (suppressStateCallbacks)
        return;

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

    if (property == Nova::IDs::INPUT_GAIN ||
        property == Nova::IDs::INPUT_GATE ||
        property == Nova::IDs::FORCE_MONO ||
        property == Nova::IDs::INPUT_TRANS ||
        property == Nova::IDs::MIXER_GAIN_A ||
        property == Nova::IDs::MIXER_PAN_A ||
        property == Nova::IDs::MIXER_WIDTH_A ||
        property == Nova::IDs::MIXER_GAIN_B ||
        property == Nova::IDs::MIXER_PAN_B ||
        property == Nova::IDs::MIXER_WIDTH_B ||
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

void NOVAAudioProcessor::sanitizeLine(juce::ValueTree line)
{
    if (!line.isValid())
        return;

    for (int i = line.getNumChildren(); --i >= 0;)
    {
        auto child = line.getChild(i);
        if (!child.hasType(Nova::IDs::PEDAL))
        {
            line.removeChild(i, nullptr);
            continue;
        }

        const auto canonicalType = PedalRegistry::canonicalType(
            child.getProperty(Nova::IDs::PEDAL_TYPE).toString());

        if (!PedalRegistry::isTypeSupported(canonicalType))
        {
            line.removeChild(i, nullptr);
            continue;
        }

        const auto zone = static_cast<Nova::ZoneID>(
            (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));

        const auto finalZone = Nova::PedalCatalog::enforceZone(canonicalType, zone);

        child.setProperty(Nova::IDs::PEDAL_TYPE, canonicalType, nullptr);
        child.setProperty(Nova::IDs::PEDAL_ZONE, static_cast<int>(finalZone), nullptr);
    }
}

void NOVAAudioProcessor::resetToCleanState()
{
    const juce::ScopedValueSetter<bool> sv(suppressStateCallbacks, true);

    pluginState.removeAllChildren(nullptr);
    ensureStateStructure();

    auto settings = getSettingsTree();
    auto lineA = getLineTree(Nova::ChainID::LineA);
    auto lineB = getLineTree(Nova::ChainID::LineB);

    if (lineA.isValid()) lineA.removeAllChildren(nullptr);
    if (lineB.isValid()) lineB.removeAllChildren(nullptr);

    if (settings.isValid())
    {
        settings.setProperty(Nova::IDs::ENGINE_ON, false, nullptr);
        settings.setProperty(Nova::IDs::SWITCH_MODE, (int)Nova::SwitcherMode::Dual_Parallel, nullptr);
        settings.setProperty(Nova::IDs::INPUT_GAIN, 0.0f, nullptr);
        settings.setProperty(Nova::IDs::INPUT_GATE, -100.0f, nullptr);
        settings.setProperty(Nova::IDs::FORCE_MONO, false, nullptr);
        settings.setProperty(Nova::IDs::INPUT_TRANS, 0, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_VOL, 0.0f, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_MIX, 100.0f, nullptr);
    }

    if (lineA.isValid())
    {
        lineA.setProperty(Nova::IDs::MIXER_GAIN_A, 1.0f, nullptr);
        lineA.setProperty(Nova::IDs::MIXER_PAN_A, 0.0f, nullptr);
        lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f, nullptr);
    }

    if (lineB.isValid())
    {
        lineB.setProperty(Nova::IDs::MIXER_GAIN_B, 1.0f, nullptr);
        lineB.setProperty(Nova::IDs::MIXER_PAN_B, 0.0f, nullptr);
        lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, 1.0f, nullptr);
    }

    audioEngine.clearAll();
    audioEngine.setEngineEnabled(false);
    updateGlobalParamsFromState();
    updateMixerFromState();
}

void NOVAAudioProcessor::ensureStateStructure()
{
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

    if (!settings.hasProperty(Nova::IDs::ENGINE_ON))
        settings.setProperty(Nova::IDs::ENGINE_ON, false, nullptr);
    if (!settings.hasProperty(Nova::IDs::SWITCH_MODE))
        settings.setProperty(Nova::IDs::SWITCH_MODE, (int)Nova::SwitcherMode::Dual_Parallel, nullptr);
    if (!settings.hasProperty(Nova::IDs::INPUT_GAIN))
        settings.setProperty(Nova::IDs::INPUT_GAIN, 0.0f, nullptr);
    if (!settings.hasProperty(Nova::IDs::INPUT_GATE))
        settings.setProperty(Nova::IDs::INPUT_GATE, -100.0f, nullptr);
    if (!settings.hasProperty(Nova::IDs::FORCE_MONO))
        settings.setProperty(Nova::IDs::FORCE_MONO, false, nullptr);
    if (!settings.hasProperty(Nova::IDs::INPUT_TRANS))
        settings.setProperty(Nova::IDs::INPUT_TRANS, 0, nullptr);
    if (!settings.hasProperty(Nova::IDs::OUTPUT_VOL))
        settings.setProperty(Nova::IDs::OUTPUT_VOL, 0.0f, nullptr);
    if (!settings.hasProperty(Nova::IDs::OUTPUT_LIMITER))
        settings.setProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f, nullptr);
    if (!settings.hasProperty(Nova::IDs::OUTPUT_MIX))
        settings.setProperty(Nova::IDs::OUTPUT_MIX, 100.0f, nullptr);

    auto lineA = getLineTree(Nova::ChainID::LineA);
    if (lineA.isValid())
    {
        if (!lineA.hasProperty(Nova::IDs::MIXER_GAIN_A))
            lineA.setProperty(Nova::IDs::MIXER_GAIN_A, 1.0f, nullptr);
        if (!lineA.hasProperty(Nova::IDs::MIXER_PAN_A))
            lineA.setProperty(Nova::IDs::MIXER_PAN_A, 0.0f, nullptr);
        if (!lineA.hasProperty(Nova::IDs::MIXER_WIDTH_A))
            lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f, nullptr);
    }

    auto lineB = getLineTree(Nova::ChainID::LineB);
    if (lineB.isValid())
    {
        if (!lineB.hasProperty(Nova::IDs::MIXER_GAIN_B))
            lineB.setProperty(Nova::IDs::MIXER_GAIN_B, 1.0f, nullptr);
        if (!lineB.hasProperty(Nova::IDs::MIXER_PAN_B))
            lineB.setProperty(Nova::IDs::MIXER_PAN_B, 0.0f, nullptr);
        if (!lineB.hasProperty(Nova::IDs::MIXER_WIDTH_B))
            lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, 1.0f, nullptr);
    }
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
