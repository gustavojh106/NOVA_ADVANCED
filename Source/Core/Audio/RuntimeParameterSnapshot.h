#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>

#include "../Constants.h"

namespace Nova
{
namespace Audio
{
    struct RuntimeGlobalParamsSnapshot
    {
        float inputGainDb = 0.0f;
        float gateThresholdDb = -100.0f;
        bool forceMono = false;
        float hostTempoBpm = 120.0f;
        bool hostTempoValid = false;
        bool hostTransportPlaying = false;

        float outputVolumeDb = 0.0f;
        float outputLimiterDb = 0.0f;
        float outputMixRaw = 100.0f;

        int switchMode = (int)Nova::SwitcherMode::LineA_Only;

        float gainA = 1.0f;
        float panA = 0.0f;
        float widthA = 1.0f;

        float gainB = 1.0f;
        float panB = 0.0f;
        float widthB = 1.0f;
    };

    class RuntimeParameterSnapshot
    {
    public:
        void reset() noexcept
        {
            store(RuntimeGlobalParamsSnapshot{});
        }

        void store(const RuntimeGlobalParamsSnapshot& snapshot) noexcept
        {
            inputGainDb.store(snapshot.inputGainDb, std::memory_order_relaxed);
            gateThresholdDb.store(snapshot.gateThresholdDb, std::memory_order_relaxed);
            forceMono.store(snapshot.forceMono, std::memory_order_relaxed);
            hostTempoBpm.store(snapshot.hostTempoBpm, std::memory_order_relaxed);
            hostTempoValid.store(snapshot.hostTempoValid, std::memory_order_relaxed);
            hostTransportPlaying.store(snapshot.hostTransportPlaying, std::memory_order_relaxed);

            outputVolumeDb.store(snapshot.outputVolumeDb, std::memory_order_relaxed);
            outputLimiterDb.store(snapshot.outputLimiterDb, std::memory_order_relaxed);
            outputMixNormalized.store(normalizeMixValue(snapshot.outputMixRaw), std::memory_order_relaxed);

            switchMode.store(snapshot.switchMode, std::memory_order_relaxed);
            gainA.store(snapshot.gainA, std::memory_order_relaxed);
            panA.store(snapshot.panA, std::memory_order_relaxed);
            widthA.store(snapshot.widthA, std::memory_order_relaxed);
            gainB.store(snapshot.gainB, std::memory_order_relaxed);
            panB.store(snapshot.panB, std::memory_order_relaxed);
            widthB.store(snapshot.widthB, std::memory_order_relaxed);

            revision.fetch_add(1, std::memory_order_release);
        }

        RuntimeGlobalParamsSnapshot load() const noexcept
        {
            RuntimeGlobalParamsSnapshot snapshot;
            snapshot.inputGainDb = inputGainDb.load(std::memory_order_relaxed);
            snapshot.gateThresholdDb = gateThresholdDb.load(std::memory_order_relaxed);
            snapshot.forceMono = forceMono.load(std::memory_order_relaxed);
            snapshot.hostTempoBpm = hostTempoBpm.load(std::memory_order_relaxed);
            snapshot.hostTempoValid = hostTempoValid.load(std::memory_order_relaxed);
            snapshot.hostTransportPlaying = hostTransportPlaying.load(std::memory_order_relaxed);

            snapshot.outputVolumeDb = outputVolumeDb.load(std::memory_order_relaxed);
            snapshot.outputLimiterDb = outputLimiterDb.load(std::memory_order_relaxed);
            snapshot.outputMixRaw = outputMixNormalized.load(std::memory_order_relaxed) * 100.0f;

            snapshot.switchMode = switchMode.load(std::memory_order_relaxed);
            snapshot.gainA = gainA.load(std::memory_order_relaxed);
            snapshot.panA = panA.load(std::memory_order_relaxed);
            snapshot.widthA = widthA.load(std::memory_order_relaxed);
            snapshot.gainB = gainB.load(std::memory_order_relaxed);
            snapshot.panB = panB.load(std::memory_order_relaxed);
            snapshot.widthB = widthB.load(std::memory_order_relaxed);
            return snapshot;
        }

        uint32_t getRevision() const noexcept
        {
            return revision.load(std::memory_order_acquire);
        }

        float getOutputMixNormalized() const noexcept
        {
            return outputMixNormalized.load(std::memory_order_relaxed);
        }

    private:
        static float normalizeMixValue(float raw) noexcept
        {
            if (raw <= 1.0f)
                return juce::jlimit(0.0f, 1.0f, raw);

            return juce::jlimit(0.0f, 100.0f, raw) / 100.0f;
        }

        std::atomic<float> inputGainDb{ 0.0f };
        std::atomic<float> gateThresholdDb{ -100.0f };
        std::atomic<bool> forceMono{ false };
        std::atomic<float> hostTempoBpm{ 120.0f };
        std::atomic<bool> hostTempoValid{ false };
        std::atomic<bool> hostTransportPlaying{ false };

        std::atomic<float> outputVolumeDb{ 0.0f };
        std::atomic<float> outputLimiterDb{ 0.0f };
        std::atomic<float> outputMixNormalized{ 1.0f };

        std::atomic<int> switchMode{ (int)Nova::SwitcherMode::LineA_Only };

        std::atomic<float> gainA{ 1.0f };
        std::atomic<float> panA{ 0.0f };
        std::atomic<float> widthA{ 1.0f };

        std::atomic<float> gainB{ 1.0f };
        std::atomic<float> panB{ 0.0f };
        std::atomic<float> widthB{ 1.0f };

        std::atomic<uint32_t> revision{ 0 };
    };
}
}
