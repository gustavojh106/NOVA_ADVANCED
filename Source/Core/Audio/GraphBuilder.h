#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <memory>
#include <vector>

#include "GraphRuntimeTypes.h"
#include "RoutingMixer.h"
#include "../Constants.h"
#include "../PedalRegistry.h"
#include "../SessionLogger.h"
#include "../DSP/Global/ChannelStrip.h"
#include "../DSP/Global/InputChain.h"
#include "../DSP/Global/OutputChain.h"
#include "../../Effects/Pedals/Base/ProcessorBase.h"

namespace Nova
{
namespace Audio
{
    class GraphBuilder
    {
    public:
        GraphBuildResult build(const GraphBuildRequest& request) const
        {
            GraphBuildResult result;
            auto runtime = std::make_shared<GraphRuntime>();
            runtime->graph = std::make_unique<juce::AudioProcessorGraph>();
            runtime->sampleRate = request.sampleRate > 0.0 ? request.sampleRate : 44100.0;
            runtime->blockSize = juce::jmax(1, request.blockSize);
            runtime->numInputs = juce::jmax(1, request.numInputs);
            runtime->numOutputs = juce::jmax(1, request.numOutputs);
            runtime->generation = request.generation;

            auto& graph = *runtime->graph;
            graph.setPlayConfigDetails(runtime->numInputs, runtime->numOutputs, runtime->sampleRate, runtime->blockSize);

            runtime->inputNode = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
            runtime->outputNode = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

            runtime->inputChainNode = graph.addNode(std::make_unique<InputChainProcessor>());
            runtime->stripNodeA = graph.addNode(std::make_unique<ChannelStripProcessor>());
            runtime->stripNodeB = graph.addNode(std::make_unique<ChannelStripProcessor>());
            runtime->outputChainNode = graph.addNode(std::make_unique<OutputChainProcessor>());

            runtime->inputChain = runtime->inputChainNode != nullptr
                ? static_cast<InputChainProcessor*>(runtime->inputChainNode->getProcessor())
                : nullptr;
            runtime->stripA = runtime->stripNodeA != nullptr
                ? static_cast<ChannelStripProcessor*>(runtime->stripNodeA->getProcessor())
                : nullptr;
            runtime->stripB = runtime->stripNodeB != nullptr
                ? static_cast<ChannelStripProcessor*>(runtime->stripNodeB->getProcessor())
                : nullptr;
            runtime->outputChain = runtime->outputChainNode != nullptr
                ? static_cast<OutputChainProcessor*>(runtime->outputChainNode->getProcessor())
                : nullptr;

            if (runtime->stripA != nullptr)
                runtime->stripA->setTelemetryTag("channel-strip-a");
            if (runtime->stripB != nullptr)
                runtime->stripB->setTelemetryTag("channel-strip-b");
            if (runtime->outputChain != nullptr)
                runtime->outputChain->setTelemetryTag("output-chain");

            if (runtime->inputNode != nullptr && runtime->inputChainNode != nullptr)
            {
                graph.addConnection({ { runtime->inputNode->nodeID, 0 }, { runtime->inputChainNode->nodeID, 0 } });
                if (runtime->numInputs > 1)
                    graph.addConnection({ { runtime->inputNode->nodeID, 1 }, { runtime->inputChainNode->nodeID, 1 } });
                else
                    graph.addConnection({ { runtime->inputNode->nodeID, 0 }, { runtime->inputChainNode->nodeID, 1 } });
            }

            buildChain(graph, request.modelChainA, runtime->chainA, result.warnings);
            buildChain(graph, request.modelChainB, runtime->chainB, result.warnings);

            if (runtime->inputChainNode != nullptr && runtime->stripNodeA != nullptr)
                connectRuntimeChain(*runtime, runtime->chainA, runtime->stripNodeA->nodeID);

            if (runtime->inputChainNode != nullptr && runtime->stripNodeB != nullptr)
                connectRuntimeChain(*runtime, runtime->chainB, runtime->stripNodeB->nodeID);

            if (runtime->outputChainNode != nullptr && runtime->outputNode != nullptr)
            {
                for (auto* strip : { runtime->stripNodeA.get(), runtime->stripNodeB.get() })
                {
                    if (strip == nullptr)
                        continue;

                    graph.addConnection({ { strip->nodeID, 0 }, { runtime->outputChainNode->nodeID, 0 } });
                    graph.addConnection({ { strip->nodeID, 1 }, { runtime->outputChainNode->nodeID, 1 } });
                }

                graph.addConnection({ { runtime->outputChainNode->nodeID, 0 }, { runtime->outputNode->nodeID, 0 } });
                graph.addConnection({ { runtime->outputChainNode->nodeID, 1 }, { runtime->outputNode->nodeID, 1 } });
            }

            applyRuntimeParamsToGraph(*runtime, request.runtimeParams, request.runtimeParamRevision);
            graph.prepareToPlay(runtime->sampleRate, runtime->blockSize);
            graph.rebuild();
            runtime->latencySamples = juce::jlimit(0, Nova::Config::MAX_GRAPH_LATENCY_SAMPLES, graph.getLatencySamples());

            result.runtime = std::move(runtime);
            return result;
        }

        static void applyRuntimeParamsToGraph(GraphRuntime& runtime,
            const RuntimeGlobalParamsSnapshot& snapshot,
            uint32_t revision)
        {
            if (runtime.appliedParamRevision == revision)
                return;

            if (runtime.inputChain != nullptr)
            {
                float gateThreshold = snapshot.gateThresholdDb;
                if (gateThreshold == 0.0f)
                    gateThreshold = -100.0f;

                runtime.inputChain->setParams(snapshot.inputGainDb, gateThreshold, snapshot.forceMono);
            }

            if (runtime.outputChain != nullptr)
                runtime.outputChain->setParams(snapshot.outputVolumeDb, snapshot.outputLimiterDb);

            const auto routingTargets = RoutingMixer::makeTargets(static_cast<Nova::SwitcherMode>(snapshot.switchMode),
                { snapshot.gainA, snapshot.panA, snapshot.widthA },
                { snapshot.gainB, snapshot.panB, snapshot.widthB });

            if (runtime.stripA != nullptr)
                runtime.stripA->setParams(routingTargets.lineA.gain,
                    routingTargets.lineA.pan,
                    routingTargets.lineA.width);

            if (runtime.stripB != nullptr)
                runtime.stripB->setParams(routingTargets.lineB.gain,
                    routingTargets.lineB.pan,
                    routingTargets.lineB.width);

            const auto applyTempo = [&snapshot](std::vector<GraphRuntimeSlot>& chain)
                {
                    for (auto& slot : chain)
                    {
                        if (slot.tempoSyncProcessor != nullptr)
                            slot.tempoSyncProcessor->setTempoSyncContext(snapshot.hostTempoBpm,
                                snapshot.hostTempoValid,
                                snapshot.hostTransportPlaying);
                    }
                };

            applyTempo(runtime.chainA);
            applyTempo(runtime.chainB);
            runtime.appliedParamRevision = revision;
        }

    private:
        static int zoneRank(Nova::ZoneID zone) noexcept
        {
            switch (zone)
            {
            case Nova::ZoneID::Pre:     return 0;
            case Nova::ZoneID::Amp:     return 1;
            case Nova::ZoneID::FX:      return 2;
            case Nova::ZoneID::Cabinet: return 3;
            default:                    return 4;
            }
        }

        static void addWarning(std::vector<GraphBuildWarning>& warnings,
            juce::String code,
            const GraphChainNodeSpec& spec,
            juce::String message)
        {
            NovaDiagnostics::SessionLogger::logEvent("engine.addPedal.failed", message);

            GraphBuildWarning warning;
            warning.code = std::move(code);
            warning.pedalType = spec.pedalType;
            warning.pedalID = spec.pedalID;
            warning.message = std::move(message);
            warnings.push_back(std::move(warning));
        }

        static void buildChain(juce::AudioProcessorGraph& graph,
            const std::vector<GraphChainNodeSpec>& specs,
            std::vector<GraphRuntimeSlot>& runtimeSlots,
            std::vector<GraphBuildWarning>& warnings)
        {
            runtimeSlots.clear();
            runtimeSlots.reserve(specs.size());

            for (const auto& spec : specs)
            {
                auto pedal = PedalRegistry::createPedal(spec.pedalType);
                if (!pedal)
                {
                    addWarning(warnings,
                        "createPedal.null",
                        spec,
                        "PedalRegistry::createPedal returned nullptr for type=\"" + spec.pedalType
                            + "\", pedalID=" + spec.pedalID);
                    continue;
                }

                auto node = graph.addNode(std::move(pedal));
                if (node == nullptr)
                {
                    addWarning(warnings,
                        "graph.addNode.null",
                        spec,
                        "graph.addNode returned nullptr for type=\"" + spec.pedalType
                            + "\", pedalID=" + spec.pedalID);
                    continue;
                }

                GraphRuntimeSlot slot;
                slot.node = node;
                slot.pedalType = spec.pedalType;
                slot.pedalID = spec.pedalID;
                slot.zone = spec.zone;
                slot.bypassed = spec.bypassed;
                slot.processor = node->getProcessor();

                // Cached once on the control thread. Never dynamic_cast from process().
                slot.baseProcessor = dynamic_cast<ProcessorBase*>(slot.processor);
                slot.tempoSyncProcessor = dynamic_cast<TempoSyncable*>(slot.processor);

                if (slot.baseProcessor != nullptr)
                    slot.baseProcessor->setBypassed(spec.bypassed);
                else if (slot.processor != nullptr)
                    slot.processor->suspendProcessing(spec.bypassed);

                runtimeSlots.push_back(std::move(slot));
            }
        }

        static void connectRuntimeChain(GraphRuntime& runtime,
            const std::vector<GraphRuntimeSlot>& chain,
            juce::AudioProcessorGraph::NodeID targetStripID)
        {
            if (runtime.graph == nullptr || runtime.inputChainNode == nullptr)
                return;

            auto& graph = *runtime.graph;
            if (graph.getNodeForId(targetStripID) == nullptr)
                return;

            juce::AudioProcessorGraph::NodeID currentSource = runtime.inputChainNode->nodeID;

            std::vector<size_t> ordered;
            ordered.reserve(chain.size());
            for (size_t i = 0; i < chain.size(); ++i)
                ordered.push_back(i);

            std::stable_sort(ordered.begin(), ordered.end(), [&chain](size_t a, size_t b)
                {
                    return zoneRank(chain[a].zone) < zoneRank(chain[b].zone);
                });

            for (size_t index : ordered)
            {
                const auto& slot = chain[index];
                if (slot.node == nullptr || graph.getNodeForId(slot.node->nodeID) == nullptr)
                    continue;

                graph.addConnection({ { currentSource, 0 }, { slot.node->nodeID, 0 } });
                graph.addConnection({ { currentSource, 1 }, { slot.node->nodeID, 1 } });
                currentSource = slot.node->nodeID;
            }

            graph.addConnection({ { currentSource, 0 }, { targetStripID, 0 } });
            graph.addConnection({ { currentSource, 1 }, { targetStripID, 1 } });
        }
    };
}
}
