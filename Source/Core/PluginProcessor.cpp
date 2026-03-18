#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PluginStateModel.h"
#include "OfflineQADiagnostics.h"
#include "PedalRegistry.h"
#include "SessionLogger.h"

#include <cmath>

namespace
{
namespace PluginState = Nova::PluginStateModel;

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
    NovaDiagnostics::SessionLogger::logEvent("tests", "Running NOVA validation suite from NOVA_RUN_AUDIO_TESTS=1");
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runTestsInCategory("NOVA");
    NovaDiagnostics::SessionLogger::logEvent("tests", "Finished NOVA validation suite");
}
#endif

#if JUCE_DEBUG
void maybeRunOfflineQADiagnosticsOnce()
{
    static bool hasRun = false;

    if (hasRun)
        return;

    const auto envValue = juce::SystemStats::getEnvironmentVariable("NOVA_RUN_AUDIO_QA", {});
    if (envValue != "1")
        return;

    hasRun = true;
    NovaDiagnostics::SessionLogger::logEvent("qa.offline", "Running offline QA diagnostics from NOVA_RUN_AUDIO_QA=1");
    NovaDiagnostics::OfflineQADiagnostics::runAndWriteReport();
    NovaDiagnostics::SessionLogger::logEvent("qa.offline", "Finished offline QA diagnostics");
}
#endif

juce::String boolToText(bool value)
{
    return value ? "true" : "false";
}

juce::String chainToText(Nova::ChainID chain)
{
    return chain == Nova::ChainID::LineA ? "LineA" : "LineB";
}

juce::String zoneToText(Nova::ZoneID zone)
{
    switch (zone)
    {
        case Nova::ZoneID::Pre:     return "Pre";
        case Nova::ZoneID::Amp:     return "Amp";
        case Nova::ZoneID::FX:      return "FX";
        case Nova::ZoneID::Cabinet: return "Cabinet";
        default:                    return "Unknown";
    }
}

juce::String switchModeToText(int mode)
{
    switch (static_cast<Nova::SwitcherMode>(mode))
    {
        case Nova::SwitcherMode::LineA_Only: return "LineA_Only";
        case Nova::SwitcherMode::LineB_Only: return "LineB_Only";
        case Nova::SwitcherMode::Dual_Parallel: return "Dual_Parallel";
        default: return "Unknown";
    }
}

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
    NovaDiagnostics::SessionLogger::attachOwner("NOVAAudioProcessor");
#if JUCE_DEBUG
    maybeRunAudioValidationTestsOnce();
    maybeRunOfflineQADiagnosticsOnce();
#endif

    createGlobalParameters();

    resetSessionState(false);
    restoreStartupPresetIfAvailable();

    pluginState.addListener(this);
    logStateSnapshot("processor.constructed");
}

NOVAAudioProcessor::~NOVAAudioProcessor()
{
    logStateSnapshot("processor.destroying");
    pluginState.removeListener(this);
    NovaDiagnostics::SessionLogger::detachOwner("NOVAAudioProcessor");
}

void NOVAAudioProcessor::createGlobalParameters()
{
    addParameter(engineOnParam = new juce::AudioParameterBool(
        Nova::IDs::ENGINE_ON.toString(), "Engine", false));

    addParameter(switchModeParam = new juce::AudioParameterChoice(
        Nova::IDs::SWITCH_MODE.toString(),
        "Switcher",
        juce::StringArray{ "Line A", "Line B", "Dual" },
        (int)Nova::SwitcherMode::LineA_Only));

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
        : (int)Nova::SwitcherMode::LineA_Only;

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
                    (int)Nova::SwitcherMode::LineA_Only))));

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
    logRuntimeSnapshot(force ? "runtime.push.forced" : "runtime.push", current);
}

void NOVAAudioProcessor::refreshEngineEnabledIfNeeded()
{
    const bool current = isEngineOn();
    if (hasPushedEngineEnabled && current == lastEngineEnabled)
        return;

    audioEngine.setEngineEnabled(current);
    lastEngineEnabled = current;
    hasPushedEngineEnabled = true;
    NovaDiagnostics::SessionLogger::logEvent("engine.toggle",
        "Engine state pushed to engineOn=" + boolToText(current));
}

// ==============================================================================
// JUCE AudioProcessor
// ==============================================================================

void NOVAAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    NovaDiagnostics::SessionLogger::logEvent("processor.prepare",
        "prepareToPlay sampleRate=" + juce::String(sampleRate)
        + ", blockSize=" + juce::String(samplesPerBlock)
        + ", inputs=" + juce::String(getTotalNumInputChannels())
        + ", outputs=" + juce::String(getTotalNumOutputChannels()));
    audioEngine.prepare(sampleRate,
        samplesPerBlock,
        getTotalNumInputChannels(),
        getTotalNumOutputChannels());

    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
    logStateSnapshot("processor.prepared");
}

void NOVAAudioProcessor::releaseResources()
{
    NovaDiagnostics::SessionLogger::logEvent("processor.release", "releaseResources called");
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

bool NOVAAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainInput = layouts.getMainInputChannelSet();
    const auto& mainOutput = layouts.getMainOutputChannelSet();
    return mainInput == juce::AudioChannelSet::stereo()
        && mainOutput == juce::AudioChannelSet::stereo();
}

// ==============================================================================
// State serialization
// ==============================================================================

void NOVAAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    writeParameterStateToTree(PluginState::getSettingsTree(pluginState),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineA),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineB));

    juce::MemoryOutputStream stream(destData, true);
    pluginState.writeToStream(stream);
    NovaDiagnostics::SessionLogger::logEvent("state.serialize",
        "Serialized plugin state. bytes=" + juce::String((int)destData.getSize()));
}

void NOVAAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    NovaDiagnostics::SessionLogger::logEvent("state.restore",
        "Attempting state restore. bytes=" + juce::String(sizeInBytes));
    auto loaded = juce::ValueTree::readFromData(data, sizeInBytes);
    if (!loaded.isValid() || !loaded.hasType(Nova::IDs::MAIN_STATE))
    {
        NovaDiagnostics::SessionLogger::logEvent("state.restore", "Invalid state payload; resetting to clean state");
        clearSessionAndForgetStartupPreset();
        return;
    }

    if (!applyStateTree(loaded, nullptr))
    {
        NovaDiagnostics::SessionLogger::logEvent("state.restore", "State restore failed; resetting to clean state");
        clearSessionAndForgetStartupPreset();
    }
}

bool NOVAAudioProcessor::savePresetToFile(const juce::File& file)
{
    auto stateToSave = PluginState::makeCanonicalCopy(pluginState);
    writeParameterStateToTree(PluginState::getSettingsTree(stateToSave),
        PluginState::getLineTree(stateToSave, Nova::ChainID::LineA),
        PluginState::getLineTree(stateToSave, Nova::ChainID::LineB));
    
    auto savePedalStates = [this, &stateToSave](Nova::ChainID chain)
        {
            auto line = PluginState::getLineTree(stateToSave, chain);

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

    NovaDiagnostics::SessionLogger::logEvent("preset.save",
        juce::String(ok ? "Saved" : "Failed to save")
        + " preset: " + target.getFullPathName());

    return ok;
}

bool NOVAAudioProcessor::loadPresetFromFile(const juce::File& file)
{
    NovaDiagnostics::SessionLogger::logEvent("preset.load",
        "Loading preset: " + file.getFullPathName());
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
    const auto incomingSchemaVersion = PluginState::getStateSchemaVersion(loadedState);
    auto loaded = PluginState::makeCanonicalCopy(loadedState);

    {
        const juce::ScopedValueSetter<bool> sv(suppressStateCallbacks, true);
        pluginState.copyPropertiesAndChildrenFrom(loaded, nullptr);
        PluginState::canonicalizeStateTree(pluginState);
    }

    applyTreeStateToParameters(PluginState::getSettingsTree(pluginState),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineA),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineB));

    audioEngine.clearAll();
    synchronizeEngineNow();

    auto rebuildLine = [this](Nova::ChainID chain)
        {
            auto line = PluginState::getLineTree(pluginState, chain);
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
    synchronizeEngineNow();

    auto restorePedalStates = [this](Nova::ChainID chain)
        {
            auto line = PluginState::getLineTree(pluginState, chain);
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
    synchronizeEngineNow();
    if (presetFile != nullptr)
        writeStartupPresetFile(*presetFile);

    NovaDiagnostics::SessionLogger::logEvent("state.apply",
        juce::String("Applied state tree")
        + ", incomingSchemaVersion=" + juce::String(incomingSchemaVersion)
        + ", storedSchemaVersion=" + juce::String(Nova::Config::STATE_SCHEMA_VERSION)
        + juce::String(presetFile != nullptr ? " from preset: " + presetFile->getFullPathName() : " from host state"));
    logStateSnapshot("state.applied");

    return true;
}

void NOVAAudioProcessor::clearSessionAndForgetStartupPreset()
{
    resetSessionState(true);
}

void NOVAAudioProcessor::resetSessionState(bool forgetStartupPreset)
{
    NovaDiagnostics::SessionLogger::logEvent("session.reset",
        forgetStartupPreset ? "Clearing session and forgetting startup preset" : "Clearing session");

    {
        const juce::ScopedValueSetter<bool> sv(suppressStateCallbacks, true);
        PluginState::resetToCleanState(pluginState);
    }

    auto settings = PluginState::getSettingsTree(pluginState);
    auto lineA = PluginState::getLineTree(pluginState, Nova::ChainID::LineA);
    auto lineB = PluginState::getLineTree(pluginState, Nova::ChainID::LineB);

    applyTreeStateToParameters(settings, lineA, lineB);
    writeParameterStateToTree(settings, lineA, lineB);

    audioEngine.clearAll();
    hasPushedEngineEnabled = false;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
    synchronizeEngineNow();
    logStateSnapshot(forgetStartupPreset ? "state.clean" : "state.clean.startup");

    if (forgetStartupPreset)
    {
        auto pointerFile = getStartupPresetPointerFile();
        if (pointerFile.existsAsFile())
            pointerFile.deleteFile();
    }
}

bool NOVAAudioProcessor::restoreStartupPresetIfAvailable()
{
    auto pointerFile = getStartupPresetPointerFile();
    if (!pointerFile.existsAsFile())
        return false;

    const auto presetPath = pointerFile.loadFileAsString().trim();
    const auto presetFile = juce::File(presetPath);
    if (!presetFile.existsAsFile())
    {
        NovaDiagnostics::SessionLogger::logEvent("preset.startup",
            "Startup preset pointer is stale: " + presetPath);
        pointerFile.deleteFile();
        return false;
    }

    const bool restored = loadPresetFromFile(presetFile);
    NovaDiagnostics::SessionLogger::logEvent("preset.startup",
        juce::String(restored ? "Restored" : "Failed to restore")
        + " startup preset: " + presetFile.getFullPathName());

    if (!restored && pointerFile.existsAsFile())
        pointerFile.deleteFile();

    return restored;
}

// ==============================================================================
// Public API (Editor commands)
// ==============================================================================

double NOVAAudioProcessor::getCpuUsage() const
{
    return audioEngine.getCpuLoad();
}

float NOVAAudioProcessor::getInputPeak() const
{
    return audioEngine.getLastInputPeak();
}

float NOVAAudioProcessor::getOutputPeak() const
{
    return audioEngine.getLastOutputPeak();
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
        : (int)Nova::SwitcherMode::LineA_Only;
    return static_cast<Nova::SwitcherMode>(juce::jlimit(0, 2, mode));
}

void NOVAAudioProcessor::requestAddPedal(const juce::String& type,
    Nova::ChainID chain,
    Nova::ZoneID zone,
    int insertIndex)
{
    const auto canonicalType = PedalRegistry::canonicalType(type);
    if (!PedalRegistry::isTypeSupported(canonicalType))
    {
        NovaDiagnostics::SessionLogger::logEvent("pedal.add",
            "Rejected unsupported pedal type: " + type);
        return;
    }

    const auto insert = PluginState::insertPedal(pluginState, type, chain, zone, insertIndex);
    if (!insert.inserted)
        return;

    synchronizeEngineNow();
    NovaDiagnostics::SessionLogger::logEvent("pedal.add",
        "Added pedal type=" + insert.canonicalType
        + ", chain=" + chainToText(chain)
        + ", zone=" + zoneToText(insert.zone)
        + ", insertIndex=" + juce::String(insert.index)
        + ", pedalID=" + insert.pedalID);
    logStateSnapshot("pedal.add");
}

void NOVAAudioProcessor::requestRemovePedal(Nova::ChainID chain, int index)
{
    NovaDiagnostics::SessionLogger::logEvent("pedal.remove",
        "Removing pedal chain=" + chainToText(chain)
        + ", index=" + juce::String(index));
    PluginState::removePedal(pluginState, chain, index);
    synchronizeEngineNow();
    logStateSnapshot("pedal.remove");
}

void NOVAAudioProcessor::requestMovePedal(Nova::ChainID chain,
    int fromIndex,
    int toIndex,
    Nova::ZoneID targetZone)
{
    if (!PluginState::movePedal(pluginState, chain, fromIndex, toIndex, targetZone))
        return;

    synchronizeEngineNow();
    NovaDiagnostics::SessionLogger::logEvent("pedal.move",
        "Moved pedal chain=" + chainToText(chain)
        + ", from=" + juce::String(fromIndex)
        + ", to=" + juce::String(toIndex)
        + ", zone=" + zoneToText(targetZone));
    logStateSnapshot("pedal.move");
}

void NOVAAudioProcessor::requestBypassPedal(Nova::ChainID chain, int index, bool bypassed)
{
    if (!PluginState::setPedalEnabled(pluginState, chain, index, !bypassed))
        return;

    synchronizeEngineNow();
    NovaDiagnostics::SessionLogger::logEvent("pedal.bypass",
        "Set pedal bypass chain=" + chainToText(chain)
        + ", index=" + juce::String(index)
        + ", bypassed=" + boolToText(bypassed));
}

void NOVAAudioProcessor::toggleEngine()
{
    if (engineOnParam == nullptr)
        return;

    const bool newState = !engineOnParam->get();
    engineOnParam->setValueNotifyingHost(engineOnParam->convertTo0to1(newState));
    writeParameterStateToTree(PluginState::getSettingsTree(pluginState),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineA),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineB));
    refreshEngineEnabledIfNeeded();
    synchronizeEngineNow();
    logStateSnapshot("engine.toggled");
}

void NOVAAudioProcessor::cycleSwitcher()
{
    if (switchModeParam == nullptr)
        return;

    const int currentMode = switchModeParam->getIndex();
    int mode = (int)Nova::SwitcherMode::LineA_Only;

    switch (static_cast<Nova::SwitcherMode>(currentMode))
    {
        case Nova::SwitcherMode::LineA_Only:
            mode = (int)Nova::SwitcherMode::Dual_Parallel;
            break;

        case Nova::SwitcherMode::Dual_Parallel:
            mode = (int)Nova::SwitcherMode::LineB_Only;
            break;

        case Nova::SwitcherMode::LineB_Only:
        default:
            mode = (int)Nova::SwitcherMode::LineA_Only;
            break;
    }

    switchModeParam->setValueNotifyingHost(switchModeParam->convertTo0to1(static_cast<float>(mode)));
    writeParameterStateToTree(PluginState::getSettingsTree(pluginState),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineA),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineB));
    hasPushedRuntimeGlobals = false;
    synchronizeEngineNow();
    NovaDiagnostics::SessionLogger::logEvent("switcher",
        "Cycled switcher to " + switchModeToText((int)getSwitcherMode()));
}

void NOVAAudioProcessor::toggleTuner()
{
    const bool newState = !audioEngine.getTunerEnabled();
    audioEngine.setTunerEnabled(newState);
    NovaDiagnostics::SessionLogger::logEvent("tuner",
        "Tuner enabled=" + boolToText(newState));
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
    NovaDiagnostics::SessionLogger::logEvent("state.child.added",
        "ValueTree child added chain=" + chainToText(chain)
        + ", index=" + juce::String(index)
        + ", type=" + child.getProperty(Nova::IDs::PEDAL_TYPE).toString()
        + ", pedalID=" + child.getProperty(Nova::IDs::PEDAL_ID).toString());
}

void NOVAAudioProcessor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int index)
{
    if (suppressStateCallbacks)
        return;

    if (parent.hasType(Nova::IDs::LINE_A))
        audioEngine.removePedal(Nova::ChainID::LineA, index);
    else if (parent.hasType(Nova::IDs::LINE_B))
        audioEngine.removePedal(Nova::ChainID::LineB, index);

    NovaDiagnostics::SessionLogger::logEvent("state.child.removed",
        "ValueTree child removed index=" + juce::String(index)
        + ", parent=" + parent.getType().toString());
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
        NovaDiagnostics::SessionLogger::logEvent("state.property",
            "PEDAL_ENABLED changed chain=" + chainToText(chain)
            + ", index=" + juce::String(index)
            + ", enabled=" + boolToText(enabled));
        return;
    }

    NovaDiagnostics::SessionLogger::logEvent("state.property",
        "Property changed tree=" + tree.getType().toString()
        + ", property=" + property.toString());
}

// ==============================================================================
// Internal helpers
// ==============================================================================

void NOVAAudioProcessor::updateGlobalParamsFromState()
{
    writeParameterStateToTree(PluginState::getSettingsTree(pluginState),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineA),
        PluginState::getLineTree(pluginState, Nova::ChainID::LineB));
    refreshEngineGlobalParamsIfNeeded(true);
    logStateSnapshot("runtime.from.state");
}

void NOVAAudioProcessor::updateMixerFromState()
{
    // Keep API for compatibility; mixer/global params are handled in one path.
    updateGlobalParamsFromState();
}

void NOVAAudioProcessor::logRuntimeSnapshot(const juce::String& context, const AudioEngine::RuntimeGlobalParams& snapshot) const
{
    juce::String message;
    message << "context=" << context
        << ", engineOn=" << boolToText(isEngineOn())
        << ", switchMode=" << switchModeToText(snapshot.switchMode)
        << ", inputGainDb=" << snapshot.inputGainDb
        << ", gateThresholdDb=" << snapshot.gateThresholdDb
        << ", forceMono=" << boolToText(snapshot.forceMono)
        << ", inputTranspose=" << snapshot.inputTranspose
        << ", gainA=" << snapshot.gainA
        << ", panA=" << snapshot.panA
        << ", widthA=" << snapshot.widthA
        << ", gainB=" << snapshot.gainB
        << ", panB=" << snapshot.panB
        << ", widthB=" << snapshot.widthB
        << ", outputVolumeDb=" << snapshot.outputVolumeDb
        << ", outputLimiterDb=" << snapshot.outputLimiterDb
        << ", outputMixRaw=" << snapshot.outputMixRaw;

    NovaDiagnostics::SessionLogger::logEvent("runtime.snapshot", message);
}

void NOVAAudioProcessor::logStateSnapshot(const juce::String& context) const
{
    NovaDiagnostics::SessionLogger::logEvent("state.snapshot",
        "context=" + context + juce::newLine + NovaDiagnostics::SessionLogger::dumpValueTree(pluginState));
    NovaDiagnostics::SessionLogger::logEvent("engine.snapshot",
        "context=" + context + juce::newLine + audioEngine.buildDiagnosticReport());
}

void NOVAAudioProcessor::synchronizeEngineNow()
{
    audioEngine.synchronizeProcessingState();
}

// ==============================================================================
// Factory
// ==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NOVAAudioProcessor();
}
