#pragma once

#include <JuceHeader.h>
#include <cstdint>
#include <memory>
#include <vector>

#include "RuntimeParameterSnapshot.h"
#include "../Constants.h"

class ChannelStripProcessor;
class InputChainProcessor;
class OutputChainProcessor;
class ProcessorBase;
struct TempoSyncable;

namespace Nova
{
namespace Audio
{
    struct GraphChainNodeSpec
    {
        juce::String pedalType;
        juce::String pedalID;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
        bool bypassed = false;
    };

    struct GraphRuntimeSlot
    {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::String pedalType;
        juce::String pedalID;
        Nova::ZoneID zone = Nova::ZoneID::Pre;
        bool bypassed = false;

        juce::AudioProcessor* processor = nullptr;
        ProcessorBase* baseProcessor = nullptr;
        TempoSyncable* tempoSyncProcessor = nullptr;
    };

    struct GraphRuntime
    {
        std::unique_ptr<juce::AudioProcessorGraph> graph;

        juce::AudioProcessorGraph::Node::Ptr inputNode;
        juce::AudioProcessorGraph::Node::Ptr outputNode;
        juce::AudioProcessorGraph::Node::Ptr inputChainNode;
        juce::AudioProcessorGraph::Node::Ptr stripNodeA;
        juce::AudioProcessorGraph::Node::Ptr stripNodeB;
        juce::AudioProcessorGraph::Node::Ptr outputChainNode;

        InputChainProcessor* inputChain = nullptr;
        ChannelStripProcessor* stripA = nullptr;
        ChannelStripProcessor* stripB = nullptr;
        OutputChainProcessor* outputChain = nullptr;

        std::vector<GraphRuntimeSlot> chainA;
        std::vector<GraphRuntimeSlot> chainB;

        double sampleRate = 44100.0;
        int blockSize = 512;
        int numInputs = 2;
        int numOutputs = 2;
        int latencySamples = 0;
        uint64_t generation = 0;
        uint32_t appliedParamRevision = 0;
    };

    struct GraphBuildWarning
    {
        juce::String code;
        juce::String pedalType;
        juce::String pedalID;
        juce::String message;
    };

    struct GraphBuildRequest
    {
        double sampleRate = 44100.0;
        int blockSize = 512;
        int numInputs = 2;
        int numOutputs = 2;
        uint64_t generation = 0;

        RuntimeGlobalParamsSnapshot runtimeParams;
        uint32_t runtimeParamRevision = 0;

        std::vector<GraphChainNodeSpec> modelChainA;
        std::vector<GraphChainNodeSpec> modelChainB;
    };

    struct GraphBuildResult
    {
        std::shared_ptr<GraphRuntime> runtime;
        std::vector<GraphBuildWarning> warnings;
    };
}
}
