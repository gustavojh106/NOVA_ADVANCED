#pragma once

#include <JuceHeader.h>
#include <atomic>

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
        return store.applyCommand(SessionStore::Command::makeAddPedal(type, chain, zone, insertIndex)).insertResult;
    }

    bool removePedal(Nova::ChainID chain, int index)
    {
        return store.applyCommand(SessionStore::Command::makeRemovePedal(chain, index)).changed;
    }

    bool movePedal(Nova::ChainID chain, int fromIndex, int toIndex, Nova::ZoneID targetZone)
    {
        return store.applyCommand(SessionStore::Command::makeMovePedal(chain, fromIndex, toIndex, targetZone)).changed;
    }

    bool setPedalEnabled(Nova::ChainID chain, int index, bool enabled)
    {
        return store.applyCommand(SessionStore::Command::makeSetPedalEnabled(chain, index, enabled)).changed;
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
