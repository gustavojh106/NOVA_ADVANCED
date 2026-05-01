#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "GraphRetirementQueue.h"

namespace Nova
{
namespace Audio
{
    template <typename GraphRuntime>
    class RuntimeGraphManager
    {
    public:
        GraphRuntime* getActiveRaw() const noexcept
        {
            return activeGraphRaw.load(std::memory_order_acquire);
        }

        template <typename LatencyChanged>
        bool publish(std::shared_ptr<GraphRuntime> newGraph,
            uint64_t currentAudioBlock,
            LatencyChanged&& onLatencyChanged)
        {
            if (newGraph == nullptr)
                return false;

            std::shared_ptr<GraphRuntime> oldGraph;

            {
                const juce::ScopedLock ownerLock(activeOwnerLock);
                oldGraph = std::move(activeOwner);
                activeOwner = std::move(newGraph);

                activeGraphRaw.store(activeOwner.get(), std::memory_order_release);
                std::forward<LatencyChanged>(onLatencyChanged)(activeOwner->latencySamples);

                if (oldGraph != nullptr)
                    retiredGraphs.retire(std::move(oldGraph), currentAudioBlock + kGraphRetireGraceBlocks);
            }

            return true;
        }

        std::shared_ptr<GraphRuntime> getActiveOwnerForControl() const
        {
            const juce::ScopedLock ownerLock(activeOwnerLock);
            return activeOwner;
        }

        void cleanupRetired(uint64_t currentAudioBlock)
        {
            const juce::ScopedLock ownerLock(activeOwnerLock);
            retiredGraphs.cleanup(currentAudioBlock);
        }

        void clear()
        {
            activeGraphRaw.store(nullptr, std::memory_order_release);

            const juce::ScopedLock ownerLock(activeOwnerLock);
            activeOwner.reset();
            retiredGraphs.clear();
        }

        int getLatencySamples() const noexcept
        {
            const auto* runtime = getActiveRaw();
            return runtime != nullptr ? runtime->latencySamples : 0;
        }

        size_t retiredSize() const noexcept
        {
            const juce::ScopedLock ownerLock(activeOwnerLock);
            return retiredGraphs.size();
        }

    private:
        static constexpr uint64_t kGraphRetireGraceBlocks = 8;

        mutable juce::CriticalSection activeOwnerLock;
        std::shared_ptr<GraphRuntime> activeOwner;
        GraphRetirementQueue<GraphRuntime> retiredGraphs;
        std::atomic<GraphRuntime*> activeGraphRaw{ nullptr };
    };
}
}
