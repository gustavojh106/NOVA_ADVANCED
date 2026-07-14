#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "NovaSettingsService.h"
#include "SessionStore.h"

class SessionPersistence
{
public:
    static void captureLivePedalStates(juce::ValueTree targetState, AudioEngine& engine)
    {
        auto savePedalStates = [&engine, &targetState](Nova::ChainID chain)
        {
            auto line = Nova::PluginStateModel::getLineTree(targetState, chain);
            if (!line.isValid())
                return;

            for (int i = 0; i < line.getNumChildren(); ++i)
            {
                auto child = line.getChild(i);
                if (!child.hasType(Nova::IDs::PEDAL))
                    continue;

                if (auto* proc = engine.getProcessorForPedal(chain, i))
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
    }

    static void rebuildEngineFromState(AudioEngine& engine, const juce::ValueTree& state)
    {
        engine.clearAll();
        engine.synchronizeProcessingState();

        auto rebuildLine = [&engine, &state](Nova::ChainID chain)
        {
            auto line = Nova::PluginStateModel::getLineTree(state, chain);
            if (!line.isValid())
                return;

            for (int i = 0; i < line.getNumChildren(); ++i)
            {
                auto child = line.getChild(i);
                if (!child.hasType(Nova::IDs::PEDAL))
                    continue;

                const auto zone = static_cast<Nova::ZoneID>(
                    (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));

                engine.addPedal(child.getProperty(Nova::IDs::PEDAL_TYPE).toString(),
                    chain,
                    i,
                    zone,
                    child.getProperty(Nova::IDs::PEDAL_ID).toString());

                const bool enabled = (bool)child.getProperty(Nova::IDs::PEDAL_ENABLED, true);
                engine.setPedalBypassed(chain, i, !enabled);
            }
        };

        rebuildLine(Nova::ChainID::LineA);
        rebuildLine(Nova::ChainID::LineB);
        engine.synchronizeProcessingState();

        auto restorePedalStates = [&engine, &state](Nova::ChainID chain)
        {
            auto line = Nova::PluginStateModel::getLineTree(state, chain);
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
                {
                    NovaDiagnostics::SessionLogger::logEvent("preset.pedal.decode.failed",
                        "Base64 decode failed for pedal index=" + juce::String(i));
                    continue;
                }

                // Reject suspiciously small or oversized decoded pedal state
                if (decoded.getDataSize() < 4 || decoded.getDataSize() > 2 * 1024 * 1024)
                {
                    NovaDiagnostics::SessionLogger::logEvent("preset.pedal.decode.rejected",
                        "Decoded pedal state out of bounds (" + juce::String((int)decoded.getDataSize())
                        + " bytes) for pedal index=" + juce::String(i));
                    continue;
                }

                if (auto* proc = engine.getProcessorForPedal(chain, i))
                    proc->setStateInformation(decoded.getData(), (int)decoded.getDataSize());
            }
        };

        restorePedalStates(Nova::ChainID::LineA);
        restorePedalStates(Nova::ChainID::LineB);
    }

    static juce::ValueTree createSerializableState(const SessionStore& store, AudioEngine& engine)
    {
        auto snapshot = Nova::PluginStateModel::makeCanonicalCopy(store.state());
        writeRuntimeStateToTree(snapshot, store.getRuntimeGlobalParams(), store.isEngineEnabled());
        captureLivePedalStates(snapshot, engine);
        return snapshot;
    }

    static bool savePresetToFile(const juce::File& file, const SessionStore& store, AudioEngine& engine)
    {
        auto stateToSave = createSerializableState(store, engine);
        juce::MemoryOutputStream stream;
        stateToSave.writeToStream(stream);

        auto target = file;
        if (target.getFileExtension().isEmpty())
            target = target.withFileExtension(".nova-preset");

        if (auto parent = target.getParentDirectory(); !parent.exists())
            parent.createDirectory();

        return target.replaceWithData(stream.getData(), stream.getDataSize());
    }

    static bool loadPresetFromFile(const juce::File& file, SessionStore& store, AudioEngine& engine)
    {
        if (!file.existsAsFile())
            return false;

        // P0: reject files larger than 50 MB to prevent OOM
        if (file.getSize() > 50 * 1024 * 1024)
        {
            NovaDiagnostics::SessionLogger::logEvent("preset.load.rejected",
                "File too large: " + file.getFullPathName()
                + " (" + juce::String(file.getSize() / (1024 * 1024)) + " MB)");
            return false;
        }

        juce::MemoryBlock data;
        if (!file.loadFileAsData(data))
            return false;

        auto loaded = juce::ValueTree::readFromData(data.getData(), (int)data.getSize());

        // P0: validate structure — must have NOVA_STATE root with SETTINGS + LINE_A + LINE_B
        if (!loaded.isValid() || !loaded.hasType(Nova::IDs::MAIN_STATE))
        {
            NovaDiagnostics::SessionLogger::logEvent("preset.load.invalid",
                "Invalid or corrupt preset structure: " + file.getFileName());
            return false;
        }

        if (!loaded.getChildWithName(Nova::IDs::SETTINGS).isValid()
            || !loaded.getChildWithName(Nova::IDs::LINE_A).isValid()
            || !loaded.getChildWithName(Nova::IDs::LINE_B).isValid())
        {
            NovaDiagnostics::SessionLogger::logEvent("preset.load.invalid",
                "Preset missing required sections (SETTINGS/LINE_A/LINE_B): " + file.getFileName());
            return false;
        }

        if (!store.applyCommand(SessionStore::Command::makeRestoreState(loaded)).changed)
            return false;

        rebuildEngineFromState(engine, store.state());
        return true;
    }

    static bool restoreStartupPresetIfAvailable(SessionStore& store, AudioEngine& engine)
    {
        auto pointerFile = Nova::SettingsService::getStartupPresetPointerFile();
        if (!pointerFile.existsAsFile())
            return false;

        juce::File presetFile;
        if (!Nova::SettingsService::readStartupPresetPointer(pointerFile, presetFile, true))
            return false;

        const bool restored = loadPresetFromFile(presetFile, store, engine);
        if (!restored && pointerFile.existsAsFile())
            Nova::SettingsService::clearStartupPresetPointer(pointerFile);

        return restored;
    }

    static void forgetStartupPreset()
    {
        Nova::SettingsService::clearStartupPresetPointer();
    }

private:
    static void writeRuntimeStateToTree(juce::ValueTree stateTree,
        const AudioEngine::RuntimeGlobalParams& snapshot,
        bool engineEnabled)
    {
        auto settings = Nova::PluginStateModel::getSettingsTree(stateTree);
        auto lineA = Nova::PluginStateModel::getLineTree(stateTree, Nova::ChainID::LineA);
        auto lineB = Nova::PluginStateModel::getLineTree(stateTree, Nova::ChainID::LineB);

        if (settings.isValid())
        {
            settings.setProperty(Nova::IDs::ENGINE_ON, engineEnabled, nullptr);
            settings.setProperty(Nova::IDs::SWITCH_MODE, snapshot.switchMode, nullptr);
            settings.setProperty(Nova::IDs::INPUT_GAIN, snapshot.inputGainDb, nullptr);
            settings.setProperty(Nova::IDs::INPUT_GATE, snapshot.gateThresholdDb, nullptr);
            settings.setProperty(Nova::IDs::FORCE_MONO, snapshot.forceMono, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_VOL, snapshot.outputVolumeDb, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_LIMITER, snapshot.outputLimiterDb, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_MIX, snapshot.outputMixRaw, nullptr);
        }

        if (lineA.isValid())
        {
            lineA.setProperty(Nova::IDs::MIXER_GAIN_A, snapshot.gainA, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_PAN_A, snapshot.panA, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, snapshot.widthA, nullptr);
        }

        if (lineB.isValid())
        {
            lineB.setProperty(Nova::IDs::MIXER_GAIN_B, snapshot.gainB, nullptr);
            lineB.setProperty(Nova::IDs::MIXER_PAN_B, snapshot.panB, nullptr);
            lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, snapshot.widthB, nullptr);
        }
    }

};
