#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PedalRegistry.h"

#include <cmath>

namespace
{
#if JUCE_DEBUG
void maybeRunAudioValidationTestsOnce()
{
    static bool hasRun = false;

    if (hasRun)
        return;

    const auto envValue = juce::SystemStats::getEnvironmentVariable("NOVA_RUN_AUDIO_TESTS", {});
    if (envValue != "1")
        return;

    // Run the NOVA audio-engine validation suite on demand without affecting normal debug startup.
    hasRun = true;
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runTestsInCategory("NOVA");
}
#endif

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

int zoneSortRank(Nova::ZoneID zone)
{
    switch (zone)
    {
        case Nova::ZoneID::Pre:     return 0;
        case Nova::ZoneID::Amp:     return 1;
        case Nova::ZoneID::FX:      return 2;
        case Nova::ZoneID::Cabinet: return 3;
        default:                    return 4;
    }
}

bool runtimeParamsDiffer(const AudioEngine::RuntimeGlobalParams& lhs,
    const AudioEngine::RuntimeGlobalParams& rhs) noexcept
{
    const auto different = [](float a, float b) noexcept
    {
        return std::abs(a - b) > 1.0e-6f;
    };

    return different(lhs.inputGainDb, rhs.inputGainDb)
        || different(lhs.gateThresholdDb, rhs.gateThresholdDb)
        || lhs.forceMono != rhs.forceMono
        || lhs.inputTranspose != rhs.inputTranspose
        || different(lhs.outputVolumeDb, rhs.outputVolumeDb)
        || different(lhs.outputLimiterDb, rhs.outputLimiterDb)
        || different(lhs.outputMixRaw, rhs.outputMixRaw)
        || lhs.switchMode != rhs.switchMode
        || different(lhs.gainA, rhs.gainA)
        || different(lhs.panA, rhs.panA)
        || different(lhs.widthA, rhs.widthA)
        || different(lhs.gainB, rhs.gainB)
        || different(lhs.panB, rhs.panB)
        || different(lhs.widthB, rhs.widthB);
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
#if JUCE_DEBUG
    maybeRunAudioValidationTestsOnce();
#endif

    createGlobalParameters();

    // Arranque limpio siempre: mismo comportamiento que boton CLEAR.
    clearSessionAndForgetStartupPreset();

    pluginState.addListener(this);
}

NOVAAudioProcessor::~NOVAAudioProcessor()
{
    pluginState.removeListener(this);
}

void NOVAAudioProcessor::createGlobalParameters()
{
    addParameter(engineOnParam = new juce::AudioParameterBool(
        Nova::IDs::ENGINE_ON.toString(), "Engine", false));

    addParameter(switchModeParam = new juce::AudioParameterChoice(
        Nova::IDs::SWITCH_MODE.toString(),
        "Switcher",
        juce::StringArray{ "Line A", "Line B", "Dual" },
        (int)Nova::SwitcherMode::Dual_Parallel));

    addParameter(inputGainParam = new juce::AudioParameterFloat(
        Nova::IDs::INPUT_GAIN.toString(), "Input Gain", -60.0f, 24.0f, 0.0f));
    addParameter(inputGateParam = new juce::AudioParameterFloat(
        Nova::IDs::INPUT_GATE.toString(), "Input Gate", -100.0f, 0.0f, -100.0f));
    addParameter(inputTransposeParam = new juce::AudioParameterInt(
        Nova::IDs::INPUT_TRANS.toString(), "Input Transpose", -12, 12, 0));
    addParameter(forceMonoParam = new juce::AudioParameterBool(
        Nova::IDs::FORCE_MONO.toString(), "Force Mono", false));

    addParameter(gainAParam = new juce::AudioParameterFloat(
        Nova::IDs::MIXER_GAIN_A.toString(), "Level A", 0.0f, 2.0f, 1.0f));
    addParameter(panAParam = new juce::AudioParameterFloat(
        Nova::IDs::MIXER_PAN_A.toString(), "Pan A", -1.0f, 1.0f, 0.0f));
    addParameter(widthAParam = new juce::AudioParameterFloat(
        Nova::IDs::MIXER_WIDTH_A.toString(), "Width A", 0.0f, 2.0f, 1.0f));

    addParameter(gainBParam = new juce::AudioParameterFloat(
        Nova::IDs::MIXER_GAIN_B.toString(), "Level B", 0.0f, 2.0f, 1.0f));
    addParameter(panBParam = new juce::AudioParameterFloat(
        Nova::IDs::MIXER_PAN_B.toString(), "Pan B", -1.0f, 1.0f, 0.0f));
    addParameter(widthBParam = new juce::AudioParameterFloat(
        Nova::IDs::MIXER_WIDTH_B.toString(), "Width B", 0.0f, 2.0f, 1.0f));

    addParameter(outputVolParam = new juce::AudioParameterFloat(
        Nova::IDs::OUTPUT_VOL.toString(), "Output Volume", -60.0f, 12.0f, 0.0f));
    addParameter(outputLimiterParam = new juce::AudioParameterFloat(
        Nova::IDs::OUTPUT_LIMITER.toString(), "Output Limiter", -20.0f, 0.0f, 0.0f));
    addParameter(outputMixParam = new juce::AudioParameterFloat(
        Nova::IDs::OUTPUT_MIX.toString(), "Output Mix", 0.0f, 100.0f, 100.0f));
}

AudioEngine::RuntimeGlobalParams NOVAAudioProcessor::makeRuntimeGlobalParams() const
{
    AudioEngine::RuntimeGlobalParams snapshot;
    snapshot.inputGainDb = inputGainParam != nullptr ? inputGainParam->get() : 0.0f;
    snapshot.gateThresholdDb = inputGateParam != nullptr ? inputGateParam->get() : -100.0f;
    snapshot.forceMono = forceMonoParam != nullptr ? forceMonoParam->get() : false;
    snapshot.inputTranspose = inputTransposeParam != nullptr ? inputTransposeParam->get() : 0;

    snapshot.outputVolumeDb = outputVolParam != nullptr ? outputVolParam->get() : 0.0f;
    snapshot.outputLimiterDb = outputLimiterParam != nullptr ? outputLimiterParam->get() : 0.0f;
    snapshot.outputMixRaw = outputMixParam != nullptr ? outputMixParam->get() : 100.0f;

    snapshot.switchMode = switchModeParam != nullptr ? switchModeParam->getIndex()
        : (int)Nova::SwitcherMode::Dual_Parallel;

    snapshot.gainA = gainAParam != nullptr ? gainAParam->get() : 1.0f;
    snapshot.panA = panAParam != nullptr ? panAParam->get() : 0.0f;
    snapshot.widthA = widthAParam != nullptr ? widthAParam->get() : 1.0f;

    snapshot.gainB = gainBParam != nullptr ? gainBParam->get() : 1.0f;
    snapshot.panB = panBParam != nullptr ? panBParam->get() : 0.0f;
    snapshot.widthB = widthBParam != nullptr ? widthBParam->get() : 1.0f;

    return snapshot;
}

void NOVAAudioProcessor::writeParameterStateToTree(juce::ValueTree settings,
    juce::ValueTree lineA,
    juce::ValueTree lineB) const
{
    if (settings.isValid())
    {
        settings.setProperty(Nova::IDs::ENGINE_ON, isEngineOn(), nullptr);
        settings.setProperty(Nova::IDs::SWITCH_MODE, (int)getSwitcherMode(), nullptr);
        settings.setProperty(Nova::IDs::INPUT_GAIN, inputGainParam != nullptr ? inputGainParam->get() : 0.0f, nullptr);
        settings.setProperty(Nova::IDs::INPUT_GATE, inputGateParam != nullptr ? inputGateParam->get() : -100.0f, nullptr);
        settings.setProperty(Nova::IDs::FORCE_MONO, forceMonoParam != nullptr ? forceMonoParam->get() : false, nullptr);
        settings.setProperty(Nova::IDs::INPUT_TRANS, inputTransposeParam != nullptr ? inputTransposeParam->get() : 0, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_VOL, outputVolParam != nullptr ? outputVolParam->get() : 0.0f, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_LIMITER, outputLimiterParam != nullptr ? outputLimiterParam->get() : 0.0f, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_MIX, outputMixParam != nullptr ? outputMixParam->get() : 100.0f, nullptr);
    }

    if (lineA.isValid())
    {
        lineA.setProperty(Nova::IDs::MIXER_GAIN_A, gainAParam != nullptr ? gainAParam->get() : 1.0f, nullptr);
        lineA.setProperty(Nova::IDs::MIXER_PAN_A, panAParam != nullptr ? panAParam->get() : 0.0f, nullptr);
        lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, widthAParam != nullptr ? widthAParam->get() : 1.0f, nullptr);
    }

    if (lineB.isValid())
    {
        lineB.setProperty(Nova::IDs::MIXER_GAIN_B, gainBParam != nullptr ? gainBParam->get() : 1.0f, nullptr);
        lineB.setProperty(Nova::IDs::MIXER_PAN_B, panBParam != nullptr ? panBParam->get() : 0.0f, nullptr);
        lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, widthBParam != nullptr ? widthBParam->get() : 1.0f, nullptr);
    }
}

void NOVAAudioProcessor::applyTreeStateToParameters(juce::ValueTree settings,
    juce::ValueTree lineA,
    juce::ValueTree lineB)
{
    const juce::ScopedValueSetter<bool> sv(suppressParamSync, true);

    if (settings.isValid())
    {
        if (engineOnParam != nullptr)
            engineOnParam->setValueNotifyingHost(engineOnParam->convertTo0to1(
                (bool)settings.getProperty(Nova::IDs::ENGINE_ON, false)));

        if (switchModeParam != nullptr)
            switchModeParam->setValueNotifyingHost(switchModeParam->convertTo0to1(
                static_cast<float>((int)settings.getProperty(Nova::IDs::SWITCH_MODE,
                    (int)Nova::SwitcherMode::Dual_Parallel))));

        if (inputGainParam != nullptr)
            inputGainParam->setValueNotifyingHost(inputGainParam->convertTo0to1(
                (float)settings.getProperty(Nova::IDs::INPUT_GAIN, 0.0f)));

        if (inputGateParam != nullptr)
            inputGateParam->setValueNotifyingHost(inputGateParam->convertTo0to1(
                (float)settings.getProperty(Nova::IDs::INPUT_GATE, -100.0f)));

        if (forceMonoParam != nullptr)
            forceMonoParam->setValueNotifyingHost(forceMonoParam->convertTo0to1(
                (bool)settings.getProperty(Nova::IDs::FORCE_MONO, false)));

        if (inputTransposeParam != nullptr)
            inputTransposeParam->setValueNotifyingHost(inputTransposeParam->convertTo0to1(
                static_cast<float>((int)settings.getProperty(Nova::IDs::INPUT_TRANS, 0))));

        if (outputVolParam != nullptr)
            outputVolParam->setValueNotifyingHost(outputVolParam->convertTo0to1(
                (float)settings.getProperty(Nova::IDs::OUTPUT_VOL, 0.0f)));

        if (outputLimiterParam != nullptr)
            outputLimiterParam->setValueNotifyingHost(outputLimiterParam->convertTo0to1(
                (float)settings.getProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f)));

        if (outputMixParam != nullptr)
            outputMixParam->setValueNotifyingHost(outputMixParam->convertTo0to1(
                (float)settings.getProperty(Nova::IDs::OUTPUT_MIX, 100.0f)));
    }

    if (lineA.isValid())
    {
        if (gainAParam != nullptr)
            gainAParam->setValueNotifyingHost(gainAParam->convertTo0to1(
                (float)lineA.getProperty(Nova::IDs::MIXER_GAIN_A, 1.0f)));
        if (panAParam != nullptr)
            panAParam->setValueNotifyingHost(panAParam->convertTo0to1(
                (float)lineA.getProperty(Nova::IDs::MIXER_PAN_A, 0.0f)));
        if (widthAParam != nullptr)
            widthAParam->setValueNotifyingHost(widthAParam->convertTo0to1(
                (float)lineA.getProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f)));
    }

    if (lineB.isValid())
    {
        if (gainBParam != nullptr)
            gainBParam->setValueNotifyingHost(gainBParam->convertTo0to1(
                (float)lineB.getProperty(Nova::IDs::MIXER_GAIN_B, 1.0f)));
        if (panBParam != nullptr)
            panBParam->setValueNotifyingHost(panBParam->convertTo0to1(
                (float)lineB.getProperty(Nova::IDs::MIXER_PAN_B, 0.0f)));
        if (widthBParam != nullptr)
            widthBParam->setValueNotifyingHost(widthBParam->convertTo0to1(
                (float)lineB.getProperty(Nova::IDs::MIXER_WIDTH_B, 1.0f)));
    }

    lastRuntimeGlobalParams = makeRuntimeGlobalParams();
    hasPushedRuntimeGlobals = false;
    lastEngineEnabled = isEngineOn();
    hasPushedEngineEnabled = false;
}

void NOVAAudioProcessor::refreshEngineGlobalParamsIfNeeded(bool force)
{
    const auto current = makeRuntimeGlobalParams();
    if (!force && hasPushedRuntimeGlobals && !runtimeParamsDiffer(current, lastRuntimeGlobalParams))
        return;

    audioEngine.updateGlobalParams(current);
    lastRuntimeGlobalParams = current;
    hasPushedRuntimeGlobals = true;
}

void NOVAAudioProcessor::refreshEngineEnabledIfNeeded()
{
    const bool current = isEngineOn();
    if (hasPushedEngineEnabled && current == lastEngineEnabled)
        return;

    audioEngine.setEngineEnabled(current);
    lastEngineEnabled = current;
    hasPushedEngineEnabled = true;
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

    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
}

void NOVAAudioProcessor::releaseResources()
{
}

void NOVAAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(false);
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
    writeParameterStateToTree(getSettingsTree(),
        getLineTree(Nova::ChainID::LineA),
        getLineTree(Nova::ChainID::LineB));

    juce::MemoryOutputStream stream(destData, true);
    pluginState.writeToStream(stream);
}

void NOVAAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto loaded = juce::ValueTree::readFromData(data, sizeInBytes);
    if (!loaded.isValid() || !loaded.hasType(Nova::IDs::MAIN_STATE))
    {
        clearSessionAndForgetStartupPreset();
        return;
    }

    if (!applyStateTree(loaded, nullptr))
        clearSessionAndForgetStartupPreset();
}

bool NOVAAudioProcessor::savePresetToFile(const juce::File& file)
{
    auto stateToSave = pluginState.createCopy();
    sanitizeLine(stateToSave.getChildWithName(Nova::IDs::LINE_A));
    sanitizeLine(stateToSave.getChildWithName(Nova::IDs::LINE_B));
    writeParameterStateToTree(stateToSave.getChildWithName(Nova::IDs::SETTINGS),
        stateToSave.getChildWithName(Nova::IDs::LINE_A),
        stateToSave.getChildWithName(Nova::IDs::LINE_B));
    
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

    return applyStateTree(loaded, &file);
}

bool NOVAAudioProcessor::applyStateTree(const juce::ValueTree& loadedState, const juce::File* presetFile)
{
    auto loaded = loadedState.createCopy();
    sanitizeLine(loaded.getChildWithName(Nova::IDs::LINE_A));
    sanitizeLine(loaded.getChildWithName(Nova::IDs::LINE_B));

    {
        const juce::ScopedValueSetter<bool> sv(suppressStateCallbacks, true);
        pluginState.copyPropertiesAndChildrenFrom(loaded, nullptr);
        ensureStateStructure();
        applyDefaultStateIfNeeded();
    }

    applyTreeStateToParameters(getSettingsTree(),
        getLineTree(Nova::ChainID::LineA),
        getLineTree(Nova::ChainID::LineB));

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

                const auto zone = static_cast<Nova::ZoneID>(
                    (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
                audioEngine.addPedal(child.getProperty(Nova::IDs::PEDAL_TYPE).toString(),
                    chain,
                    i,
                    zone,
                    child.getProperty(Nova::IDs::PEDAL_ID).toString());
                const bool enabled = (bool)child.getProperty(Nova::IDs::PEDAL_ENABLED, true);
                audioEngine.setPedalBypassed(chain, i, !enabled);
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

    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
    if (presetFile != nullptr)
        writeStartupPresetFile(*presetFile);

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

juce::RangedAudioParameter* NOVAAudioProcessor::getGlobalParameter(const juce::String& paramID) const
{
    for (auto* param : getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            if (ranged->paramID == paramID)
                return ranged;
    }

    return nullptr;
}

bool NOVAAudioProcessor::isEngineOn() const
{
    return engineOnParam != nullptr ? engineOnParam->get() : false;
}

Nova::SwitcherMode NOVAAudioProcessor::getSwitcherMode() const
{
    const int mode = switchModeParam != nullptr ? switchModeParam->getIndex()
        : (int)Nova::SwitcherMode::Dual_Parallel;
    return static_cast<Nova::SwitcherMode>(juce::jlimit(0, 2, mode));
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
    newPedal.setProperty(Nova::IDs::PEDAL_ENABLED, true, nullptr);
    newPedal.setProperty(Nova::IDs::PEDAL_ID, juce::Uuid().toString(), nullptr);

    auto list = getLineTree(chain);
    if (!list.isValid())
        return;

    if (finalZone == Nova::ZoneID::Amp || finalZone == Nova::ZoneID::Cabinet)
    {
        for (int i = list.getNumChildren(); --i >= 0;)
        {
            auto child = list.getChild(i);
            if (!child.hasType(Nova::IDs::PEDAL))
                continue;

            const auto childZone = static_cast<Nova::ZoneID>(
                (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));

            if (childZone == finalZone)
                list.removeChild(i, nullptr);
        }
    }

    int insertIndex = list.getNumChildren();
    const int targetRank = zoneSortRank(finalZone);

    for (int i = 0; i < list.getNumChildren(); ++i)
    {
        auto child = list.getChild(i);
        if (!child.hasType(Nova::IDs::PEDAL))
            continue;

        const auto childZone = static_cast<Nova::ZoneID>(
            (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));

        if (zoneSortRank(childZone) > targetRank)
        {
            insertIndex = i;
            break;
        }
    }

    list.addChild(newPedal, insertIndex, nullptr);
}

void NOVAAudioProcessor::requestRemovePedal(Nova::ChainID chain, int index)
{
    getLineTree(chain).removeChild(index, nullptr);
}

void NOVAAudioProcessor::requestBypassPedal(Nova::ChainID chain, int index, bool bypassed)
{
    auto line = getLineTree(chain);
    if (!line.isValid() || !juce::isPositiveAndBelow(index, line.getNumChildren()))
        return;

    auto child = line.getChild(index);
    if (!child.hasType(Nova::IDs::PEDAL))
        return;

    child.setProperty(Nova::IDs::PEDAL_ENABLED, !bypassed, nullptr);
}

void NOVAAudioProcessor::toggleEngine()
{
    if (engineOnParam == nullptr)
        return;

    const bool newState = !engineOnParam->get();
    engineOnParam->setValueNotifyingHost(engineOnParam->convertTo0to1(newState));
    writeParameterStateToTree(getSettingsTree(),
        getLineTree(Nova::ChainID::LineA),
        getLineTree(Nova::ChainID::LineB));
    refreshEngineEnabledIfNeeded();
}

void NOVAAudioProcessor::cycleSwitcher()
{
    if (switchModeParam == nullptr)
        return;

    const int mode = (switchModeParam->getIndex() + 1) % 3;
    switchModeParam->setValueNotifyingHost(switchModeParam->convertTo0to1(static_cast<float>(mode)));
    writeParameterStateToTree(getSettingsTree(),
        getLineTree(Nova::ChainID::LineA),
        getLineTree(Nova::ChainID::LineB));
    hasPushedRuntimeGlobals = false;
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

    const auto index = parent.indexOf(child);
    const auto zone = static_cast<Nova::ZoneID>(
        (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
    audioEngine.addPedal(child.getProperty(Nova::IDs::PEDAL_TYPE),
        chain,
        index,
        zone,
        child.getProperty(Nova::IDs::PEDAL_ID).toString());

    const bool enabled = (bool)child.getProperty(Nova::IDs::PEDAL_ENABLED, true);
    audioEngine.setPedalBypassed(chain, index, !enabled);
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

    if (property == Nova::IDs::PEDAL_ENABLED && tree.hasType(Nova::IDs::PEDAL))
    {
        auto parent = tree.getParent();
        if (!parent.isValid())
            return;

        Nova::ChainID chain;
        if (parent.hasType(Nova::IDs::LINE_A)) chain = Nova::ChainID::LineA;
        else if (parent.hasType(Nova::IDs::LINE_B)) chain = Nova::ChainID::LineB;
        else return;

        const int index = parent.indexOf(tree);
        const bool enabled = (bool)tree.getProperty(Nova::IDs::PEDAL_ENABLED, true);
        audioEngine.setPedalBypassed(chain, index, !enabled);
        return;
    }

    juce::ignoreUnused(tree, property);
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
        if (!child.hasProperty(Nova::IDs::PEDAL_ENABLED))
            child.setProperty(Nova::IDs::PEDAL_ENABLED, true, nullptr);
        if (!child.hasProperty(Nova::IDs::PEDAL_ID))
            child.setProperty(Nova::IDs::PEDAL_ID, juce::Uuid().toString(), nullptr);
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

    applyTreeStateToParameters(settings, lineA, lineB);
    writeParameterStateToTree(settings, lineA, lineB);

    audioEngine.clearAll();
    hasPushedEngineEnabled = false;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
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
    writeParameterStateToTree(getSettingsTree(),
        getLineTree(Nova::ChainID::LineA),
        getLineTree(Nova::ChainID::LineB));
    refreshEngineGlobalParamsIfNeeded(true);
}

void NOVAAudioProcessor::updateMixerFromState()
{
    // Keep API for compatibility; mixer/global params are handled in one path.
    updateGlobalParamsFromState();
}

// ==============================================================================
// Factory
// ==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NOVAAudioProcessor();
}
