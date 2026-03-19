#pragma once

#include <JuceHeader.h>
#include <atomic>

#include "AudioEngine.h"
#include "PluginStateModel.h"

class SessionStore
{
public:
    struct ParameterBindings
    {
        juce::AudioParameterBool* engineOn = nullptr;
        juce::AudioParameterChoice* switchMode = nullptr;
        juce::AudioParameterFloat* inputGain = nullptr;
        juce::AudioParameterFloat* inputGate = nullptr;
        juce::AudioParameterInt* inputTranspose = nullptr;
        juce::AudioParameterBool* forceMono = nullptr;
        juce::AudioParameterFloat* gainA = nullptr;
        juce::AudioParameterFloat* panA = nullptr;
        juce::AudioParameterFloat* widthA = nullptr;
        juce::AudioParameterFloat* gainB = nullptr;
        juce::AudioParameterFloat* panB = nullptr;
        juce::AudioParameterFloat* widthB = nullptr;
        juce::AudioParameterFloat* outputVolume = nullptr;
        juce::AudioParameterFloat* outputLimiter = nullptr;
        juce::AudioParameterFloat* outputMix = nullptr;
    };

    struct Command
    {
        enum class Type
        {
            Reset,
            RestoreStateTree,
            AddPedal,
            RemovePedal,
            MovePedal,
            SetPedalEnabled
        };

        Type type = Type::Reset;
        juce::ValueTree stateTree;
        juce::String pedalType;
        Nova::ChainID chain = Nova::ChainID::LineA;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
        int index = -1;
        int toIndex = -1;
        bool enabled = true;

        static Command makeReset()
        {
            return {};
        }

        static Command makeRestoreState(const juce::ValueTree& tree)
        {
            Command command;
            command.type = Type::RestoreStateTree;
            command.stateTree = tree;
            return command;
        }

        static Command makeAddPedal(const juce::String& pedalType, Nova::ChainID chain, Nova::ZoneID zone, int index)
        {
            Command command;
            command.type = Type::AddPedal;
            command.pedalType = pedalType;
            command.chain = chain;
            command.zone = zone;
            command.index = index;
            return command;
        }

        static Command makeRemovePedal(Nova::ChainID chain, int index)
        {
            Command command;
            command.type = Type::RemovePedal;
            command.chain = chain;
            command.index = index;
            return command;
        }

        static Command makeMovePedal(Nova::ChainID chain, int fromIndex, int toIndex, Nova::ZoneID targetZone)
        {
            Command command;
            command.type = Type::MovePedal;
            command.chain = chain;
            command.index = fromIndex;
            command.toIndex = toIndex;
            command.zone = targetZone;
            return command;
        }

        static Command makeSetPedalEnabled(Nova::ChainID chain, int index, bool enabled)
        {
            Command command;
            command.type = Type::SetPedalEnabled;
            command.chain = chain;
            command.index = index;
            command.enabled = enabled;
            return command;
        }
    };

    struct CommandResult
    {
        bool changed = false;
        Nova::PluginStateModel::PedalInsertResult insertResult;
    };

    SessionStore()
        : sessionState(Nova::IDs::MAIN_STATE)
    {
        Nova::PluginStateModel::resetToCleanState(sessionState);
        syncRuntimeCacheFromState(sessionState);
    }

    void bindParameters(const ParameterBindings& newBindings)
    {
        bindings = newBindings;
        syncRuntimeCacheFromBindings();
        syncStateFromBindingsNow();
    }

    juce::ValueTree& state() noexcept { return sessionState; }
    const juce::ValueTree& state() const noexcept { return sessionState; }

    AudioEngine::RuntimeGlobalParams getRuntimeGlobalParams() const
    {
        const juce::SpinLock::ScopedLockType lock(runtimeCacheLock);
        return runtimeParamsCache;
    }

    bool isEngineEnabled() const noexcept
    {
        return engineEnabledCache.load(std::memory_order_acquire);
    }

    bool noteParameterValueChanged(const juce::String& paramID, float normalizedValue)
    {
        if (suppressParameterMirroring.load(std::memory_order_acquire))
            return false;

        updateRuntimeCacheFromParameter(paramID, normalizedValue);
        return true;
    }

    CommandResult applyCommand(const Command& command)
    {
        CommandResult result;

        switch (command.type)
        {
            case Command::Type::Reset:
                resetSessionState();
                result.changed = true;
                break;

            case Command::Type::RestoreStateTree:
                result.changed = restoreStateTree(command.stateTree);
                break;

            case Command::Type::AddPedal:
                result.insertResult = Nova::PluginStateModel::insertPedal(
                    sessionState, command.pedalType, command.chain, command.zone, command.index);
                result.changed = result.insertResult.inserted;
                break;

            case Command::Type::RemovePedal:
                result.changed = Nova::PluginStateModel::removePedal(sessionState, command.chain, command.index);
                break;

            case Command::Type::MovePedal:
                result.changed = Nova::PluginStateModel::movePedal(
                    sessionState, command.chain, command.index, command.toIndex, command.zone);
                break;

            case Command::Type::SetPedalEnabled:
                result.changed = Nova::PluginStateModel::setPedalEnabled(
                    sessionState, command.chain, command.index, command.enabled);
                break;
        }

        if (result.changed && command.type != Command::Type::RestoreStateTree && command.type != Command::Type::Reset)
            syncRuntimeCacheFromState(sessionState);

        return result;
    }

    void syncStateFromBindingsNow()
    {
        Nova::PluginStateModel::ensureStructure(sessionState);

        auto settings = Nova::PluginStateModel::getSettingsTree(sessionState);
        auto lineA = Nova::PluginStateModel::getLineTree(sessionState, Nova::ChainID::LineA);
        auto lineB = Nova::PluginStateModel::getLineTree(sessionState, Nova::ChainID::LineB);

        if (settings.isValid())
        {
            settings.setProperty(Nova::IDs::ENGINE_ON, bindings.engineOn != nullptr ? bindings.engineOn->get() : false, nullptr);
            settings.setProperty(Nova::IDs::SWITCH_MODE, bindings.switchMode != nullptr ? bindings.switchMode->getIndex() : (int)Nova::SwitcherMode::LineA_Only, nullptr);
            settings.setProperty(Nova::IDs::INPUT_GAIN, bindings.inputGain != nullptr ? bindings.inputGain->get() : 0.0f, nullptr);
            settings.setProperty(Nova::IDs::INPUT_GATE, bindings.inputGate != nullptr ? bindings.inputGate->get() : -100.0f, nullptr);
            settings.setProperty(Nova::IDs::FORCE_MONO, bindings.forceMono != nullptr ? bindings.forceMono->get() : false, nullptr);
            settings.setProperty(Nova::IDs::INPUT_TRANS, bindings.inputTranspose != nullptr ? bindings.inputTranspose->get() : 0, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_VOL, bindings.outputVolume != nullptr ? bindings.outputVolume->get() : 0.0f, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_LIMITER, bindings.outputLimiter != nullptr ? bindings.outputLimiter->get() : 0.0f, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_MIX, bindings.outputMix != nullptr ? bindings.outputMix->get() : 100.0f, nullptr);
        }

        if (lineA.isValid())
        {
            lineA.setProperty(Nova::IDs::MIXER_GAIN_A, bindings.gainA != nullptr ? bindings.gainA->get() : 1.0f, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_PAN_A, bindings.panA != nullptr ? bindings.panA->get() : 0.0f, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, bindings.widthA != nullptr ? bindings.widthA->get() : 1.0f, nullptr);
        }

        if (lineB.isValid())
        {
            lineB.setProperty(Nova::IDs::MIXER_GAIN_B, bindings.gainB != nullptr ? bindings.gainB->get() : 1.0f, nullptr);
            lineB.setProperty(Nova::IDs::MIXER_PAN_B, bindings.panB != nullptr ? bindings.panB->get() : 0.0f, nullptr);
            lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, bindings.widthB != nullptr ? bindings.widthB->get() : 1.0f, nullptr);
        }

        syncRuntimeCacheFromState(sessionState);
    }

    void syncBindingsFromStateNow()
    {
        if (!hasParameterBindings())
            return;

        suppressParameterMirroring.store(true, std::memory_order_release);
        auto settings = Nova::PluginStateModel::getSettingsTree(sessionState);
        auto lineA = Nova::PluginStateModel::getLineTree(sessionState, Nova::ChainID::LineA);
        auto lineB = Nova::PluginStateModel::getLineTree(sessionState, Nova::ChainID::LineB);

        if (settings.isValid())
        {
            if (bindings.engineOn != nullptr)
                bindings.engineOn->setValueNotifyingHost(bindings.engineOn->convertTo0to1(
                    (bool)settings.getProperty(Nova::IDs::ENGINE_ON, false)));

            if (bindings.switchMode != nullptr)
                bindings.switchMode->setValueNotifyingHost(bindings.switchMode->convertTo0to1(
                    static_cast<float>((int)settings.getProperty(Nova::IDs::SWITCH_MODE,
                        (int)Nova::SwitcherMode::LineA_Only))));

            if (bindings.inputGain != nullptr)
                bindings.inputGain->setValueNotifyingHost(bindings.inputGain->convertTo0to1(
                    (float)settings.getProperty(Nova::IDs::INPUT_GAIN, 0.0f)));

            if (bindings.inputGate != nullptr)
                bindings.inputGate->setValueNotifyingHost(bindings.inputGate->convertTo0to1(
                    (float)settings.getProperty(Nova::IDs::INPUT_GATE, -100.0f)));

            if (bindings.forceMono != nullptr)
                bindings.forceMono->setValueNotifyingHost(bindings.forceMono->convertTo0to1(
                    (bool)settings.getProperty(Nova::IDs::FORCE_MONO, false)));

            if (bindings.inputTranspose != nullptr)
                bindings.inputTranspose->setValueNotifyingHost(bindings.inputTranspose->convertTo0to1(
                    static_cast<float>((int)settings.getProperty(Nova::IDs::INPUT_TRANS, 0))));

            if (bindings.outputVolume != nullptr)
                bindings.outputVolume->setValueNotifyingHost(bindings.outputVolume->convertTo0to1(
                    (float)settings.getProperty(Nova::IDs::OUTPUT_VOL, 0.0f)));

            if (bindings.outputLimiter != nullptr)
                bindings.outputLimiter->setValueNotifyingHost(bindings.outputLimiter->convertTo0to1(
                    (float)settings.getProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f)));

            if (bindings.outputMix != nullptr)
                bindings.outputMix->setValueNotifyingHost(bindings.outputMix->convertTo0to1(
                    (float)settings.getProperty(Nova::IDs::OUTPUT_MIX, 100.0f)));
        }

        if (lineA.isValid())
        {
            if (bindings.gainA != nullptr)
                bindings.gainA->setValueNotifyingHost(bindings.gainA->convertTo0to1(
                    (float)lineA.getProperty(Nova::IDs::MIXER_GAIN_A, 1.0f)));
            if (bindings.panA != nullptr)
                bindings.panA->setValueNotifyingHost(bindings.panA->convertTo0to1(
                    (float)lineA.getProperty(Nova::IDs::MIXER_PAN_A, 0.0f)));
            if (bindings.widthA != nullptr)
                bindings.widthA->setValueNotifyingHost(bindings.widthA->convertTo0to1(
                    (float)lineA.getProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f)));
        }

        if (lineB.isValid())
        {
            if (bindings.gainB != nullptr)
                bindings.gainB->setValueNotifyingHost(bindings.gainB->convertTo0to1(
                    (float)lineB.getProperty(Nova::IDs::MIXER_GAIN_B, 1.0f)));
            if (bindings.panB != nullptr)
                bindings.panB->setValueNotifyingHost(bindings.panB->convertTo0to1(
                    (float)lineB.getProperty(Nova::IDs::MIXER_PAN_B, 0.0f)));
            if (bindings.widthB != nullptr)
                bindings.widthB->setValueNotifyingHost(bindings.widthB->convertTo0to1(
                    (float)lineB.getProperty(Nova::IDs::MIXER_WIDTH_B, 1.0f)));
        }

        suppressParameterMirroring.store(false, std::memory_order_release);
    }

private:
    void resetSessionState()
    {
        Nova::PluginStateModel::resetToCleanState(sessionState);
        syncRuntimeCacheFromState(sessionState);
        syncBindingsFromStateNow();
    }

    bool restoreStateTree(const juce::ValueTree& loadedState)
    {
        if (!loadedState.isValid() || !loadedState.hasType(Nova::IDs::MAIN_STATE))
            return false;

        auto loaded = Nova::PluginStateModel::makeCanonicalCopy(loadedState);
        sessionState.copyPropertiesAndChildrenFrom(loaded, nullptr);
        Nova::PluginStateModel::canonicalizeStateTree(sessionState);
        syncRuntimeCacheFromState(sessionState);
        syncBindingsFromStateNow();
        return true;
    }

    void syncRuntimeCacheFromBindings()
    {
        AudioEngine::RuntimeGlobalParams snapshot;
        snapshot.inputGainDb = bindings.inputGain != nullptr ? bindings.inputGain->get() : 0.0f;
        snapshot.gateThresholdDb = bindings.inputGate != nullptr ? bindings.inputGate->get() : -100.0f;
        snapshot.forceMono = bindings.forceMono != nullptr ? bindings.forceMono->get() : false;
        snapshot.inputTranspose = bindings.inputTranspose != nullptr ? bindings.inputTranspose->get() : 0;
        snapshot.outputVolumeDb = bindings.outputVolume != nullptr ? bindings.outputVolume->get() : 0.0f;
        snapshot.outputLimiterDb = bindings.outputLimiter != nullptr ? bindings.outputLimiter->get() : 0.0f;
        snapshot.outputMixRaw = bindings.outputMix != nullptr ? bindings.outputMix->get() : 100.0f;
        snapshot.switchMode = bindings.switchMode != nullptr ? bindings.switchMode->getIndex()
            : (int)Nova::SwitcherMode::LineA_Only;
        snapshot.gainA = bindings.gainA != nullptr ? bindings.gainA->get() : 1.0f;
        snapshot.panA = bindings.panA != nullptr ? bindings.panA->get() : 0.0f;
        snapshot.widthA = bindings.widthA != nullptr ? bindings.widthA->get() : 1.0f;
        snapshot.gainB = bindings.gainB != nullptr ? bindings.gainB->get() : 1.0f;
        snapshot.panB = bindings.panB != nullptr ? bindings.panB->get() : 0.0f;
        snapshot.widthB = bindings.widthB != nullptr ? bindings.widthB->get() : 1.0f;

        const juce::SpinLock::ScopedLockType lock(runtimeCacheLock);
        runtimeParamsCache = snapshot;
        engineEnabledCache.store(bindings.engineOn != nullptr ? bindings.engineOn->get() : false,
            std::memory_order_release);
    }

    void syncRuntimeCacheFromState(const juce::ValueTree& stateTree)
    {
        AudioEngine::RuntimeGlobalParams snapshot;
        auto settings = Nova::PluginStateModel::getSettingsTree(stateTree);
        auto lineA = Nova::PluginStateModel::getLineTree(stateTree, Nova::ChainID::LineA);
        auto lineB = Nova::PluginStateModel::getLineTree(stateTree, Nova::ChainID::LineB);

        if (settings.isValid())
        {
            snapshot.inputGainDb = (float)settings.getProperty(Nova::IDs::INPUT_GAIN, 0.0f);
            snapshot.gateThresholdDb = (float)settings.getProperty(Nova::IDs::INPUT_GATE, -100.0f);
            snapshot.forceMono = (bool)settings.getProperty(Nova::IDs::FORCE_MONO, false);
            snapshot.inputTranspose = (int)settings.getProperty(Nova::IDs::INPUT_TRANS, 0);
            snapshot.outputVolumeDb = (float)settings.getProperty(Nova::IDs::OUTPUT_VOL, 0.0f);
            snapshot.outputLimiterDb = (float)settings.getProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f);
            snapshot.outputMixRaw = (float)settings.getProperty(Nova::IDs::OUTPUT_MIX, 100.0f);
            snapshot.switchMode = (int)settings.getProperty(Nova::IDs::SWITCH_MODE,
                (int)Nova::SwitcherMode::LineA_Only);
            engineEnabledCache.store((bool)settings.getProperty(Nova::IDs::ENGINE_ON, false),
                std::memory_order_release);
        }
        else
        {
            engineEnabledCache.store(false, std::memory_order_release);
        }

        if (lineA.isValid())
        {
            snapshot.gainA = (float)lineA.getProperty(Nova::IDs::MIXER_GAIN_A, 1.0f);
            snapshot.panA = (float)lineA.getProperty(Nova::IDs::MIXER_PAN_A, 0.0f);
            snapshot.widthA = (float)lineA.getProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f);
        }

        if (lineB.isValid())
        {
            snapshot.gainB = (float)lineB.getProperty(Nova::IDs::MIXER_GAIN_B, 1.0f);
            snapshot.panB = (float)lineB.getProperty(Nova::IDs::MIXER_PAN_B, 0.0f);
            snapshot.widthB = (float)lineB.getProperty(Nova::IDs::MIXER_WIDTH_B, 1.0f);
        }

        const juce::SpinLock::ScopedLockType lock(runtimeCacheLock);
        runtimeParamsCache = snapshot;
    }

    bool hasParameterBindings() const noexcept
    {
        return bindings.engineOn != nullptr
            || bindings.switchMode != nullptr
            || bindings.inputGain != nullptr
            || bindings.outputVolume != nullptr;
    }

    void updateRuntimeCacheFromParameter(const juce::String& paramID, float normalizedValue)
    {
        auto convert = [normalizedValue](juce::RangedAudioParameter* param) noexcept
        {
            return param != nullptr ? param->convertFrom0to1(normalizedValue) : normalizedValue;
        };

        const juce::SpinLock::ScopedLockType lock(runtimeCacheLock);

        if (paramID == Nova::IDs::ENGINE_ON.toString())
        {
            engineEnabledCache.store(convert(bindings.engineOn) >= 0.5f, std::memory_order_release);
            return;
        }

        if (paramID == Nova::IDs::SWITCH_MODE.toString())
            runtimeParamsCache.switchMode = juce::roundToInt(convert(bindings.switchMode));
        else if (paramID == Nova::IDs::INPUT_GAIN.toString())
            runtimeParamsCache.inputGainDb = convert(bindings.inputGain);
        else if (paramID == Nova::IDs::INPUT_GATE.toString())
            runtimeParamsCache.gateThresholdDb = convert(bindings.inputGate);
        else if (paramID == Nova::IDs::FORCE_MONO.toString())
            runtimeParamsCache.forceMono = convert(bindings.forceMono) >= 0.5f;
        else if (paramID == Nova::IDs::INPUT_TRANS.toString())
            runtimeParamsCache.inputTranspose = juce::roundToInt(convert(bindings.inputTranspose));
        else if (paramID == Nova::IDs::MIXER_GAIN_A.toString())
            runtimeParamsCache.gainA = convert(bindings.gainA);
        else if (paramID == Nova::IDs::MIXER_PAN_A.toString())
            runtimeParamsCache.panA = convert(bindings.panA);
        else if (paramID == Nova::IDs::MIXER_WIDTH_A.toString())
            runtimeParamsCache.widthA = convert(bindings.widthA);
        else if (paramID == Nova::IDs::MIXER_GAIN_B.toString())
            runtimeParamsCache.gainB = convert(bindings.gainB);
        else if (paramID == Nova::IDs::MIXER_PAN_B.toString())
            runtimeParamsCache.panB = convert(bindings.panB);
        else if (paramID == Nova::IDs::MIXER_WIDTH_B.toString())
            runtimeParamsCache.widthB = convert(bindings.widthB);
        else if (paramID == Nova::IDs::OUTPUT_VOL.toString())
            runtimeParamsCache.outputVolumeDb = convert(bindings.outputVolume);
        else if (paramID == Nova::IDs::OUTPUT_LIMITER.toString())
            runtimeParamsCache.outputLimiterDb = convert(bindings.outputLimiter);
        else if (paramID == Nova::IDs::OUTPUT_MIX.toString())
            runtimeParamsCache.outputMixRaw = convert(bindings.outputMix);
    }

    juce::ValueTree sessionState;
    ParameterBindings bindings;

    mutable juce::SpinLock runtimeCacheLock;
    AudioEngine::RuntimeGlobalParams runtimeParamsCache;
    std::atomic<bool> engineEnabledCache{ false };
    std::atomic<bool> suppressParameterMirroring{ false };
};
