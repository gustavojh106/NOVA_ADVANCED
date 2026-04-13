#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PluginStateModel.h"
#include "OfflineQADiagnostics.h"
#include "PedalRegistry.h"
#include "SessionLogger.h"
#include "../Effects/Pedals/Delay/DelayPedal.h"

#include <cmath>

namespace
{
namespace PluginState = Nova::PluginStateModel;

#if JUCE_DEBUG
juce::String buildUnitTestReport(const juce::UnitTestRunner& runner)
{
    juce::String report;
    int failingResults = 0;
    int totalPasses = 0;
    int totalFailures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult(i);
        if (result == nullptr)
            continue;

        totalPasses += result->passes;
        totalFailures += result->failures;

        if (result->failures <= 0)
            continue;

        ++failingResults;
        report << "FAIL | " << result->unitTestName << " | " << result->subcategoryName
            << " | passes=" << result->passes
            << " failures=" << result->failures << juce::newLine;

        for (const auto& message : result->messages)
            report << "  " << message << juce::newLine;
    }

    juce::String header;
    header << "results=" << runner.getNumResults()
        << " passes=" << totalPasses
        << " failures=" << totalFailures
        << " failingResults=" << failingResults
        << juce::newLine;

    if (failingResults == 0)
        header << "status=PASS" << juce::newLine;
    else
        header << "status=FAIL" << juce::newLine;

    return header + report;
}

void maybeWriteUnitTestReport(const juce::String& report)
{
    const auto reportPath = juce::SystemStats::getEnvironmentVariable("NOVA_TEST_REPORT_PATH", {});
    if (reportPath.isEmpty())
        return;

    const auto reportFile = juce::File(reportPath);
    reportFile.getParentDirectory().createDirectory();
    reportFile.replaceWithText(report);
}

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
    const auto report = buildUnitTestReport(runner);
    NovaDiagnostics::SessionLogger::logEvent("tests.summary", report);
    maybeWriteUnitTestReport(report);
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
        || different(lhs.hostTempoBpm, rhs.hostTempoBpm)
        || lhs.hostTempoValid != rhs.hostTempoValid
        || lhs.hostTransportPlaying != rhs.hostTransportPlaying
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

void applyHostTransportState(juce::AudioProcessor& processor, AudioEngine::RuntimeGlobalParams& snapshot)
{
    snapshot.hostTempoBpm = 120.0f;
    snapshot.hostTempoValid = false;
    snapshot.hostTransportPlaying = false;

    auto* playHead = processor.getPlayHead();
    if (playHead == nullptr)
        return;

    if (auto position = playHead->getPosition())
    {
        if (auto bpm = position->getBpm())
        {
            snapshot.hostTempoBpm = juce::jlimit(20.0f, 320.0f, (float)*bpm);
            snapshot.hostTempoValid = true;
        }

        snapshot.hostTransportPlaying = position->getIsPlaying();
    }
}

juce::String sanitisePresetStem(const juce::String& presetName)
{
    auto safe = presetName.trim()
        .retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_");

    if (safe.isEmpty())
        safe = "Preset";

    return safe;
}

juce::File getUserPresetDirectory()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("NOVA")
        .getChildFile("Presets");

    if (!dir.exists())
        dir.createDirectory();

    return dir;
}

juce::ValueTree createFactoryDelayPresetState(int presetIndex)
{
    DelayPedal delay;
    delay.applyFlagshipPreset(presetIndex);

    juce::MemoryBlock pedalState;
    delay.getStateInformation(pedalState);

    juce::ValueTree state(Nova::IDs::MAIN_STATE);
    PluginState::resetToCleanState(state);

    if (auto settings = PluginState::getSettingsTree(state); settings.isValid())
    {
        settings.setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
        settings.setProperty(Nova::IDs::SWITCH_MODE, (int) Nova::SwitcherMode::LineA_Only, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_MIX, 100.0f, nullptr);
    }

    if (auto lineA = PluginState::getLineTree(state, Nova::ChainID::LineA); lineA.isValid())
    {
        auto pedal = juce::ValueTree(Nova::IDs::PEDAL);
        pedal.setProperty(Nova::IDs::PEDAL_ID, "factory-delay-" + juce::String(presetIndex), nullptr);
        pedal.setProperty(Nova::IDs::PEDAL_TYPE, "Delay", nullptr);
        pedal.setProperty(Nova::IDs::PEDAL_ZONE, (int) Nova::ZoneID::FX, nullptr);
        pedal.setProperty(Nova::IDs::PEDAL_ENABLED, true, nullptr);

        if (pedalState.getSize() > 0)
        {
            pedal.setProperty(Nova::IDs::PEDAL_STATE,
                juce::Base64::toBase64(pedalState.getData(), pedalState.getSize()),
                nullptr);
        }

        lineA.appendChild(pedal, nullptr);
    }

    PluginState::canonicalizeStateTree(state);
    return state;
}

bool writePresetStateToFile(const juce::File& file, const juce::ValueTree& state)
{
    juce::MemoryOutputStream stream;
    state.writeToStream(stream);
    return file.replaceWithData(stream.getData(), stream.getDataSize());
}

void seedBundledDelayPresetsIfMissing()
{
    static bool attempted = false;
    if (attempted)
        return;

    attempted = true;
    const auto presetDirectory = getUserPresetDirectory();

    for (int i = 0; i < DelayPedal::getNumFlagshipPresets(); ++i)
    {
        const auto presetName = "Factory - Orbit " + DelayPedal::getFlagshipPresetName(i);
        const auto presetFile = presetDirectory.getChildFile(sanitisePresetStem(presetName) + ".nova-preset");

        if (presetFile.existsAsFile())
            continue;

        const bool written = writePresetStateToFile(presetFile, createFactoryDelayPresetState(i));
        NovaDiagnostics::SessionLogger::logEvent(
            written ? "preset.seeded" : "preset.seed.failed",
            presetName + (written ? " -> " : " !! ") + presetFile.getFullPathName());
    }
}
}

// ==============================================================================
// Constructor / Destructor
// ==============================================================================

NOVAAudioProcessor::NOVAAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
    , pluginState(sessionCoordinator.state())
{
    NovaDiagnostics::SessionLogger::attachOwner("NOVAAudioProcessor");
#if JUCE_DEBUG
    maybeRunAudioValidationTestsOnce();
    maybeRunOfflineQADiagnosticsOnce();
#endif

    createGlobalParameters();
    sessionCoordinator.bindParameters({
        engineOnParam,
        switchModeParam,
        inputGainParam,
        inputGateParam,
        inputTransposeParam,
        forceMonoParam,
        gainAParam,
        panAParam,
        widthAParam,
        gainBParam,
        panBParam,
        widthBParam,
        outputVolParam,
        outputLimiterParam,
        outputMixParam
        });

    for (auto* parameter : getParameters())
        parameter->addListener(this);

    seedBundledDelayPresetsIfMissing();
    resetSessionState(false);
    restoreStartupPresetIfAvailable();
    logStateSnapshot("processor.constructed");
}

NOVAAudioProcessor::~NOVAAudioProcessor()
{
    logStateSnapshot("processor.destroying");

    for (auto* parameter : getParameters())
        parameter->removeListener(this);

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

void NOVAAudioProcessor::parameterValueChanged(int parameterIndex, float newValue)
{
    if (!juce::isPositiveAndBelow(parameterIndex, getParameters().size()))
        return;

    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(getParameters()[parameterIndex]))
        sessionCoordinator.noteParameterValueChanged(ranged->paramID, newValue);
}

void NOVAAudioProcessor::refreshEngineGlobalParamsIfNeeded(bool force)
{
    auto current = sessionCoordinator.getRuntimeGlobalParams();
    applyHostTransportState(*this, current);

    if (!force && hasPushedRuntimeGlobals && !runtimeParamsDiffer(current, lastRuntimeGlobalParams))
        return;

    audioEngine.updateGlobalParams(current);
    lastRuntimeGlobalParams = current;
    hasPushedRuntimeGlobals = true;
    logRuntimeSnapshot(force ? "runtime.push.forced" : "runtime.push", current);
}

void NOVAAudioProcessor::refreshEngineEnabledIfNeeded()
{
    const bool current = sessionCoordinator.isEngineEnabled();
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
double NOVAAudioProcessor::getTailLengthSeconds() const
{
    // Report a conservative tail so DAWs don't cut reverb/delay tails on stop.
    // Nimbus Cloud/Shimmer can ring well beyond a short FX tail.
    return 16.0;
}

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
    auto stateToSave = sessionCoordinator.createSerializableState(audioEngine);
    juce::MemoryOutputStream stream(destData, true);
    stateToSave.writeToStream(stream);
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

    if (!sessionCoordinator.restoreStateTree(loaded))
    {
        NovaDiagnostics::SessionLogger::logEvent("state.restore", "State restore failed; resetting to clean state");
        clearSessionAndForgetStartupPreset();
        return;
    }

    sessionCoordinator.rebuildEngineFromState(audioEngine);
    hasPushedEngineEnabled = false;
    hasPushedRuntimeGlobals = false;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
    synchronizeEngineNow();
    NovaDiagnostics::SessionLogger::logEvent("state.apply",
        "Applied state tree, incomingSchemaVersion=" + juce::String(PluginState::getStateSchemaVersion(loaded))
        + ", storedSchemaVersion=" + juce::String(Nova::Config::STATE_SCHEMA_VERSION)
        + " from host state");
    logStateSnapshot("state.applied");
}

bool NOVAAudioProcessor::savePresetToFile(const juce::File& file)
{
    const bool ok = sessionCoordinator.savePresetToFile(file, audioEngine);
    auto target = file;
    if (target.getFileExtension().isEmpty())
        target = target.withFileExtension(".nova-preset");

    NovaDiagnostics::SessionLogger::logEvent("preset.save",
        juce::String(ok ? "Saved" : "Failed to save")
        + " preset: " + target.getFullPathName());

    return ok;
}

bool NOVAAudioProcessor::loadPresetFromFile(const juce::File& file)
{
    NovaDiagnostics::SessionLogger::logEvent("preset.load",
        "Loading preset: " + file.getFullPathName());
    const bool restored = sessionCoordinator.loadPresetFromFile(file, audioEngine);
    if (!restored)
        return false;

    hasPushedEngineEnabled = false;
    hasPushedRuntimeGlobals = false;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
    synchronizeEngineNow();
    NovaDiagnostics::SessionLogger::logEvent("state.apply",
        juce::String("Applied state tree")
        + ", incomingSchemaVersion=" + juce::String(PluginState::getStateSchemaVersion(pluginState))
        + ", storedSchemaVersion=" + juce::String(Nova::Config::STATE_SCHEMA_VERSION)
        + " from preset: " + file.getFullPathName());
    logStateSnapshot("state.applied");
    return restored;
}

void NOVAAudioProcessor::clearSessionAndForgetStartupPreset()
{
    resetSessionState(true);
}

void NOVAAudioProcessor::resetSessionState(bool forgetStartupPreset)
{
    NovaDiagnostics::SessionLogger::logEvent("session.reset",
        forgetStartupPreset ? "Clearing session and forgetting startup preset" : "Clearing session");

    sessionCoordinator.resetSessionState();
    sessionCoordinator.rebuildEngineFromState(audioEngine);
    hasPushedEngineEnabled = false;
    hasPushedRuntimeGlobals = false;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
    synchronizeEngineNow();
    logStateSnapshot(forgetStartupPreset ? "state.clean" : "state.clean.startup");

    if (forgetStartupPreset)
        sessionCoordinator.forgetStartupPreset();
}

bool NOVAAudioProcessor::restoreStartupPresetIfAvailable()
{
    const bool restored = sessionCoordinator.restoreStartupPresetIfAvailable(audioEngine);
    NovaDiagnostics::SessionLogger::logEvent("preset.startup",
        restored ? "Restored startup preset from pointer" : "No startup preset restored");

    if (restored)
    {
        hasPushedEngineEnabled = false;
        hasPushedRuntimeGlobals = false;
        refreshEngineEnabledIfNeeded();
        refreshEngineGlobalParamsIfNeeded(true);
        synchronizeEngineNow();
        logStateSnapshot("state.applied");
    }

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
    return sessionCoordinator.isEngineEnabled();
}

Nova::SwitcherMode NOVAAudioProcessor::getSwitcherMode() const
{
    const int mode = sessionCoordinator.getRuntimeGlobalParams().switchMode;
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

    // Capacity gate for flex zones
    const auto effectiveZone = Nova::PedalCatalog::enforceZone(canonicalType, zone);
    if (effectiveZone == Nova::ZoneID::Pre || effectiveZone == Nova::ZoneID::FX)
    {
        const int count = Nova::PluginStateModel::countPedalsInZone(pluginState, chain, effectiveZone);
        if (count >= Nova::Config::MAX_PEDALS_PER_FLEX_ZONE)
        {
            NovaDiagnostics::SessionLogger::logEvent("pedal.add",
                "Rejected: zone " + zoneToText(effectiveZone) + " at capacity ("
                + juce::String(count) + "/" + juce::String(Nova::Config::MAX_PEDALS_PER_FLEX_ZONE) + ")");
            return;
        }
    }

    sessionCoordinator.captureLivePedalStates(audioEngine);
    const auto insert = sessionCoordinator.insertPedal(type, chain, zone, insertIndex);
    if (!insert.inserted)
        return;

    sessionCoordinator.rebuildEngineFromState(audioEngine);
    hasPushedRuntimeGlobals = false;
    hasPushedEngineEnabled = false;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
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
    sessionCoordinator.captureLivePedalStates(audioEngine);
    if (!sessionCoordinator.removePedal(chain, index))
        return;

    sessionCoordinator.rebuildEngineFromState(audioEngine);
    hasPushedRuntimeGlobals = false;
    hasPushedEngineEnabled = false;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
    synchronizeEngineNow();
    logStateSnapshot("pedal.remove");
}

void NOVAAudioProcessor::requestMovePedal(Nova::ChainID chain,
    int fromIndex,
    int toIndex,
    Nova::ZoneID targetZone)
{
    sessionCoordinator.captureLivePedalStates(audioEngine);
    if (!sessionCoordinator.movePedal(chain, fromIndex, toIndex, targetZone))
        return;

    sessionCoordinator.rebuildEngineFromState(audioEngine);
    hasPushedRuntimeGlobals = false;
    hasPushedEngineEnabled = false;
    refreshEngineEnabledIfNeeded();
    refreshEngineGlobalParamsIfNeeded(true);
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
    if (!sessionCoordinator.setPedalEnabled(chain, index, !bypassed))
        return;

    audioEngine.setPedalBypassed(chain, index, bypassed);
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

    setSwitcherMode(static_cast<Nova::SwitcherMode>(mode));
}

void NOVAAudioProcessor::cycleSwitcherWithMask(int enabledModesBitmask)
{
    if (switchModeParam == nullptr)
        return;

    // Collect enabled modes in order: A(0), Parallel(1), B(2)
    std::vector<int> enabled;
    for (int i = 0; i < 3; ++i)
        if ((enabledModesBitmask >> i) & 1)
            enabled.push_back(i);

    if (enabled.size() < 2)
    {
        cycleSwitcher(); // fallback to normal cycle
        return;
    }

    const int currentMode = switchModeParam->getIndex();

    // Find current mode in the enabled list, then advance to next
    int nextIdx = 0;
    for (int i = 0; i < (int)enabled.size(); ++i)
    {
        if (enabled[(size_t)i] == currentMode)
        {
            nextIdx = (i + 1) % (int)enabled.size();
            break;
        }
    }

    setSwitcherMode(static_cast<Nova::SwitcherMode>(enabled[(size_t)nextIdx]));
}

void NOVAAudioProcessor::setSwitcherMode(Nova::SwitcherMode mode)
{
    if (switchModeParam == nullptr)
        return;

    switchModeParam->setValueNotifyingHost(switchModeParam->convertTo0to1(static_cast<float>(mode)));
    hasPushedRuntimeGlobals = false;
    refreshEngineGlobalParamsIfNeeded(true);
    synchronizeEngineNow();
    NovaDiagnostics::SessionLogger::logEvent("switcher",
        "Set switcher to " + switchModeToText((int)getSwitcherMode()));
}

void NOVAAudioProcessor::toggleTuner()
{
    const bool newState = !audioEngine.getTunerEnabled();
    audioEngine.setTunerEnabled(newState);
    NovaDiagnostics::SessionLogger::logEvent("tuner",
        "Tuner enabled=" + boolToText(newState));
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
