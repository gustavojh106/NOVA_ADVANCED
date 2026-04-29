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
        return runtimeParamsCache.load();
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
    struct RuntimeGlobalParamAtomics
    {
        std::atomic<float> inputGainDb{ 0.0f };
        std::atomic<float> gateThresholdDb{ -100.0f };
        std::atomic<bool> forceMono{ false };
        std::atomic<float> hostTempoBpm{ 120.0f };
        std::atomic<bool> hostTempoValid{ false };
        std::atomic<bool> hostTransportPlaying{ false };
        std::atomic<float> outputVolumeDb{ 0.0f };
        std::atomic<float> outputLimiterDb{ 0.0f };
        std::atomic<float> outputMixRaw{ 100.0f };
        std::atomic<int> switchMode{ (int)Nova::SwitcherMode::LineA_Only };
        std::atomic<float> gainA{ 1.0f };
        std::atomic<float> panA{ 0.0f };
        std::atomic<float> widthA{ 1.0f };
        std::atomic<float> gainB{ 1.0f };
        std::atomic<float> panB{ 0.0f };
        std::atomic<float> widthB{ 1.0f };

        void store(const AudioEngine::RuntimeGlobalParams& snapshot) noexcept
        {
            inputGainDb.store(snapshot.inputGainDb, std::memory_order_release);
            gateThresholdDb.store(snapshot.gateThresholdDb, std::memory_order_release);
            forceMono.store(snapshot.forceMono, std::memory_order_release);
            hostTempoBpm.store(snapshot.hostTempoBpm, std::memory_order_release);
            hostTempoValid.store(snapshot.hostTempoValid, std::memory_order_release);
            hostTransportPlaying.store(snapshot.hostTransportPlaying, std::memory_order_release);
            outputVolumeDb.store(snapshot.outputVolumeDb, std::memory_order_release);
            outputLimiterDb.store(snapshot.outputLimiterDb, std::memory_order_release);
            outputMixRaw.store(snapshot.outputMixRaw, std::memory_order_release);
            switchMode.store(snapshot.switchMode, std::memory_order_release);
            gainA.store(snapshot.gainA, std::memory_order_release);
            panA.store(snapshot.panA, std::memory_order_release);
            widthA.store(snapshot.widthA, std::memory_order_release);
            gainB.store(snapshot.gainB, std::memory_order_release);
            panB.store(snapshot.panB, std::memory_order_release);
            widthB.store(snapshot.widthB, std::memory_order_release);
        }

        AudioEngine::RuntimeGlobalParams load() const noexcept
        {
            AudioEngine::RuntimeGlobalParams snapshot;
            snapshot.inputGainDb = inputGainDb.load(std::memory_order_acquire);
            snapshot.gateThresholdDb = gateThresholdDb.load(std::memory_order_acquire);
            snapshot.forceMono = forceMono.load(std::memory_order_acquire);
            snapshot.hostTempoBpm = hostTempoBpm.load(std::memory_order_acquire);
            snapshot.hostTempoValid = hostTempoValid.load(std::memory_order_acquire);
            snapshot.hostTransportPlaying = hostTransportPlaying.load(std::memory_order_acquire);
            snapshot.outputVolumeDb = outputVolumeDb.load(std::memory_order_acquire);
            snapshot.outputLimiterDb = outputLimiterDb.load(std::memory_order_acquire);
            snapshot.outputMixRaw = outputMixRaw.load(std::memory_order_acquire);
            snapshot.switchMode = switchMode.load(std::memory_order_acquire);
            snapshot.gainA = gainA.load(std::memory_order_acquire);
            snapshot.panA = panA.load(std::memory_order_acquire);
            snapshot.widthA = widthA.load(std::memory_order_acquire);
            snapshot.gainB = gainB.load(std::memory_order_acquire);
            snapshot.panB = panB.load(std::memory_order_acquire);
            snapshot.widthB = widthB.load(std::memory_order_acquire);
            return snapshot;
        }
    };

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

        runtimeParamsCache.store(snapshot);
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

        runtimeParamsCache.store(snapshot);
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

        if (paramID == Nova::IDs::ENGINE_ON.toString())
        {
            engineEnabledCache.store(convert(bindings.engineOn) >= 0.5f, std::memory_order_release);
            return;
        }

        if (paramID == Nova::IDs::SWITCH_MODE.toString())
            runtimeParamsCache.switchMode.store(juce::roundToInt(convert(bindings.switchMode)), std::memory_order_release);
        else if (paramID == Nova::IDs::INPUT_GAIN.toString())
            runtimeParamsCache.inputGainDb.store(convert(bindings.inputGain), std::memory_order_release);
        else if (paramID == Nova::IDs::INPUT_GATE.toString())
            runtimeParamsCache.gateThresholdDb.store(convert(bindings.inputGate), std::memory_order_release);
        else if (paramID == Nova::IDs::FORCE_MONO.toString())
            runtimeParamsCache.forceMono.store(convert(bindings.forceMono) >= 0.5f, std::memory_order_release);
        else if (paramID == Nova::IDs::MIXER_GAIN_A.toString())
            runtimeParamsCache.gainA.store(convert(bindings.gainA), std::memory_order_release);
        else if (paramID == Nova::IDs::MIXER_PAN_A.toString())
            runtimeParamsCache.panA.store(convert(bindings.panA), std::memory_order_release);
        else if (paramID == Nova::IDs::MIXER_WIDTH_A.toString())
            runtimeParamsCache.widthA.store(convert(bindings.widthA), std::memory_order_release);
        else if (paramID == Nova::IDs::MIXER_GAIN_B.toString())
            runtimeParamsCache.gainB.store(convert(bindings.gainB), std::memory_order_release);
        else if (paramID == Nova::IDs::MIXER_PAN_B.toString())
            runtimeParamsCache.panB.store(convert(bindings.panB), std::memory_order_release);
        else if (paramID == Nova::IDs::MIXER_WIDTH_B.toString())
            runtimeParamsCache.widthB.store(convert(bindings.widthB), std::memory_order_release);
        else if (paramID == Nova::IDs::OUTPUT_VOL.toString())
            runtimeParamsCache.outputVolumeDb.store(convert(bindings.outputVolume), std::memory_order_release);
        else if (paramID == Nova::IDs::OUTPUT_LIMITER.toString())
            runtimeParamsCache.outputLimiterDb.store(convert(bindings.outputLimiter), std::memory_order_release);
        else if (paramID == Nova::IDs::OUTPUT_MIX.toString())
            runtimeParamsCache.outputMixRaw.store(convert(bindings.outputMix), std::memory_order_release);
    }

    juce::ValueTree sessionState;
    ParameterBindings bindings;

    RuntimeGlobalParamAtomics runtimeParamsCache;
    std::atomic<bool> engineEnabledCache{ false };
    std::atomic<bool> suppressParameterMirroring{ false };
};
