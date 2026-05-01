#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace Nova
{
namespace Audio
{
    template <typename GraphRuntime>
    class GraphRetirementQueue
    {
    public:
        void retire(std::shared_ptr<GraphRuntime> graph, uint64_t releaseAfterBlock)
        {
            if (graph != nullptr)
                retiredGraphs.push_back({ std::move(graph), releaseAfterBlock });
        }

        void cleanup(uint64_t currentAudioBlock)
        {
            retiredGraphs.erase(
                std::remove_if(retiredGraphs.begin(), retiredGraphs.end(),
                    [currentAudioBlock](const RetiredGraph& retired)
                    {
                        return retired.releaseAfterAudioBlock <= currentAudioBlock;
                    }),
                retiredGraphs.end());

            // Keep memory bounded even if audio is stopped and the block counter is not moving.
            while (retiredGraphs.size() > kMaxRetiredGraphs)
                retiredGraphs.erase(retiredGraphs.begin());
        }

        void clear()
        {
            retiredGraphs.clear();
        }

        size_t size() const noexcept
        {
            return retiredGraphs.size();
        }

    private:
        struct RetiredGraph
        {
            std::shared_ptr<GraphRuntime> graph;
            uint64_t releaseAfterAudioBlock = 0;
        };

        static constexpr size_t kMaxRetiredGraphs = 8;

        std::vector<RetiredGraph> retiredGraphs;
    };
}
}
