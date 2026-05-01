#pragma once

#include <JuceHeader.h>
#include <utility>

namespace Nova
{
namespace Audio
{
    template <typename Command, typename ChainNode, typename ChainList, typename Traits>
    class GraphCommandApplier
    {
    public:
        struct Result
        {
            bool applied = false;
            bool topologyChanged = false;
            bool explicitRebuild = false;
            bool paramsOnly = false;
            bool bypassChanged = false;
            bool engineStateChanged = false;
        };

        static Result apply(const Command& command,
            ChainList& modelChainA,
            ChainList& modelChainB)
        {
            Result result;
            auto& chain = Traits::usesLineA(command) ? modelChainA : modelChainB;

            if (Traits::isAddPedal(command))
            {
                ChainNode spec = Traits::makeNode(command);
                const int index = Traits::index(command);

                if (index >= 0 && index <= (int)chain.size())
                    chain.insert(chain.begin() + index, std::move(spec));
                else
                    chain.push_back(std::move(spec));

                result.applied = true;
                result.topologyChanged = true;
                return result;
            }

            if (Traits::isRemovePedal(command))
            {
                const int index = Traits::index(command);
                if (index >= 0 && index < (int)chain.size())
                {
                    chain.erase(chain.begin() + index);
                    result.applied = true;
                    result.topologyChanged = true;
                }

                return result;
            }

            if (Traits::isMovePedal(command))
            {
                const int from = Traits::index(command);
                const int to = Traits::toIndex(command);

                if (from >= 0 && from < (int)chain.size()
                    && to >= 0 && to <= (int)chain.size()
                    && from != to)
                {
                    auto slot = std::move(chain[(size_t)from]);
                    chain.erase(chain.begin() + from);

                    int adjustedTo = to;
                    if (from < to)
                        --adjustedTo;

                    adjustedTo = juce::jlimit(0, (int)chain.size(), adjustedTo);
                    chain.insert(chain.begin() + adjustedTo, std::move(slot));
                    result.applied = true;
                    result.topologyChanged = true;
                }

                return result;
            }

            if (Traits::isClearAll(command))
            {
                modelChainA.clear();
                modelChainB.clear();
                result.applied = true;
                result.topologyChanged = true;
                return result;
            }

            if (Traits::isSetPedalBypass(command))
            {
                const int index = Traits::index(command);
                if (juce::isPositiveAndBelow(index, (int)chain.size()))
                {
                    chain[(size_t)index].bypassed = Traits::flag(command);
                    result.applied = true;
                    result.paramsOnly = true;
                    result.bypassChanged = true;
                }

                return result;
            }

            if (Traits::isSetEngineEnabled(command))
            {
                result.applied = true;
                result.engineStateChanged = true;
                return result;
            }

            if (Traits::isRebuildGraph(command))
            {
                result.applied = true;
                result.topologyChanged = true;
                result.explicitRebuild = true;
                return result;
            }

            return result;
        }
    };
}
}
