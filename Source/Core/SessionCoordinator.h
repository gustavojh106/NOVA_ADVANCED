#pragma once

#include <JuceHeader.h>
#include <atomic>

#include "SessionLogger.h"
#include "SessionPersistence.h"
#include "SessionStore.h"

class SessionCoordinator : private juce::AsyncUpdater
{
public:
    using ParameterBindings = SessionStore::ParameterBindings;

    ~SessionCoordinator() override
    {
        cancelPendingUpdate();
    }

    void bindParameters(const ParameterBindings& newBindings)
    {
        store.bindParameters(newBindings);
    }

    juce::ValueTree& state() noexcept { return store.state(); }
    const juce::ValueTree& state() const noexcept { return store.state(); }

    AudioEngine::RuntimeGlobalParams getRuntimeGlobalParams() const
    {
        return store.getRuntimeGlobalParams();
    }

    bool isEngineEnabled() const noexcept
    {
        return store.isEngineEnabled();
    }

    void noteParameterValueChanged(const juce::String& paramID, float normalizedValue)
    {
        if (!store.noteParameterValueChanged(paramID, normalizedValue))
            return;

        pendingStateMirror.store(true, std::memory_order_release);
        triggerAsyncUpdate();
    }

    void resetSessionState()
    {
        cancelPendingUpdate();
        pendingStateMirror.store(false, std::memory_order_release);
        store.applyCommand(SessionStore::Command::makeReset());
    }

    bool restoreStateTree(const juce::ValueTree& loadedState)
    {
        cancelPendingUpdate();
        pendingStateMirror.store(false, std::memory_order_release);
        return store.applyCommand(SessionStore::Command::makeRestoreState(loadedState)).changed;
    }

    void captureLivePedalStates(AudioEngine& engine)
    {
        SessionPersistence::captureLivePedalStates(store.state(), engine);
    }

    void rebuildEngineFromState(AudioEngine& engine) const
    {
        SessionPersistence::rebuildEngineFromState(engine, store.state());
    }

    juce::ValueTree createSerializableState(AudioEngine& engine) const
    {
        return SessionPersistence::createSerializableState(store, engine);
    }

    bool savePresetToFile(const juce::File& file, AudioEngine& engine)
    {
        return SessionPersistence::savePresetToFile(file, store, engine);
    }

    bool loadPresetFromFile(const juce::File& file, AudioEngine& engine)
    {
        cancelPendingUpdate();
        pendingStateMirror.store(false, std::memory_order_release);
        return SessionPersistence::loadPresetFromFile(file, store, engine);
    }

    bool restoreStartupPresetIfAvailable(AudioEngine& engine)
    {
        cancelPendingUpdate();
        pendingStateMirror.store(false, std::memory_order_release);
        return SessionPersistence::restoreStartupPresetIfAvailable(store, engine);
    }

    void forgetStartupPreset()
    {
        SessionPersistence::forgetStartupPreset();
    }

    Nova::PluginStateModel::PedalInsertResult insertPedal(const juce::String& type,
        Nova::ChainID chain,
        Nova::ZoneID zone,
        int insertIndex)
    {
        auto result = store.applyCommand(SessionStore::Command::makeAddPedal(type, chain, zone, insertIndex));
        if (!result.insertResult.inserted)
            NovaDiagnostics::SessionLogger::logEvent("coordinator.insertPedal.failed",
                "type=" + type + ", chain=" + juce::String((int)chain)
                + ", zone=" + juce::String((int)zone) + ", index=" + juce::String(insertIndex));
        return result.insertResult;
    }

    bool removePedal(Nova::ChainID chain, int index)
    {
        const bool ok = store.applyCommand(SessionStore::Command::makeRemovePedal(chain, index)).changed;
        if (!ok)
            NovaDiagnostics::SessionLogger::logEvent("coordinator.removePedal.failed",
                "chain=" + juce::String((int)chain) + ", index=" + juce::String(index));
        return ok;
    }

    bool movePedal(Nova::ChainID chain, int fromIndex, int toIndex, Nova::ZoneID targetZone)
    {
        const bool ok = store.applyCommand(SessionStore::Command::makeMovePedal(chain, fromIndex, toIndex, targetZone)).changed;
        if (!ok)
            NovaDiagnostics::SessionLogger::logEvent("coordinator.movePedal.failed",
                "chain=" + juce::String((int)chain) + ", from=" + juce::String(fromIndex)
                + ", to=" + juce::String(toIndex) + ", zone=" + juce::String((int)targetZone));
        return ok;
    }

    bool setPedalEnabled(Nova::ChainID chain, int index, bool enabled)
    {
        const bool ok = store.applyCommand(SessionStore::Command::makeSetPedalEnabled(chain, index, enabled)).changed;
        if (!ok)
            NovaDiagnostics::SessionLogger::logEvent("coordinator.setPedalEnabled.failed",
                "chain=" + juce::String((int)chain) + ", index=" + juce::String(index)
                + ", enabled=" + juce::String(enabled ? "true" : "false"));
        return ok;
    }

private:
    void handleAsyncUpdate() override
    {
        if (pendingStateMirror.exchange(false, std::memory_order_acq_rel))
            store.syncStateFromBindingsNow();
    }

    SessionStore store;
    std::atomic<bool> pendingStateMirror{ false };
};
