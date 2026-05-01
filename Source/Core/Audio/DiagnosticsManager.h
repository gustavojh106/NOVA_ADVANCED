#pragma once

#include <JuceHeader.h>
#include <cstdint>
#include <vector>

#include "RuntimeParameterSnapshot.h"
#include "../Constants.h"

namespace Nova
{
namespace Audio
{
    class DiagnosticsManager
    {
    public:
        struct ModelChainEntry
        {
            juce::String pedalType;
            juce::String pedalID;
            Nova::ZoneID zone = Nova::ZoneID::Pre;
            bool bypassed = false;
        };

        struct RuntimeChainEntry : public ModelChainEntry
        {
            juce::String processorName;
            bool hasProcessor = false;
            bool cachedBase = false;
            bool cachedTempo = false;
        };

        struct ReportData
        {
            bool engineOn = false;
            bool tunerEnabled = false;
            double sampleRate = 0.0;
            int blockSize = 0;
            int inputChannels = 0;
            int outputChannels = 0;
            bool activeGraph = false;
            uint64_t generation = 0;
            int graphLatencySamples = 0;
            float wetMix = 0.0f;
            float wetMixTarget = 0.0f;
            float wetMixCurrent = 0.0f;
            double cpuLoad = 0.0;
            double lastProcessMs = 0.0;
            double avgProcessMs = 0.0;
            double peakProcessMs = 0.0;
            int autoHealCount = 0;
            uint64_t audioBlocks = 0;
            RuntimeGlobalParamsSnapshot runtimeParams;
            std::vector<RuntimeChainEntry> runtimeChainA;
            std::vector<RuntimeChainEntry> runtimeChainB;
            std::vector<ModelChainEntry> modelChainA;
            std::vector<ModelChainEntry> modelChainB;
        };

        static juce::String buildDiagnosticReport(const ReportData& data)
        {
            juce::String report;
            report << "engineOn=" << boolToText(data.engineOn)
                << ", tunerEnabled=" << boolToText(data.tunerEnabled)
                << ", sampleRate=" << data.sampleRate
                << ", blockSize=" << data.blockSize
                << ", inputChannels=" << data.inputChannels
                << ", outputChannels=" << data.outputChannels
                << ", activeGraph=" << boolToText(data.activeGraph)
                << ", generation=" << (juce::int64)data.generation
                << ", graphLatencySamples=" << data.graphLatencySamples
                << ", wetMix=" << data.wetMix
                << ", wetMixTarget=" << data.wetMixTarget
                << ", wetMixCurrent=" << data.wetMixCurrent
                << ", cpuLoad=" << data.cpuLoad
                << ", lastProcessMs=" << data.lastProcessMs
                << ", avgProcessMs=" << data.avgProcessMs
                << ", peakProcessMs=" << data.peakProcessMs
                << ", autoHealCount=" << data.autoHealCount
                << ", audioBlocks=" << (juce::int64)data.audioBlocks
                << juce::newLine
                << "runtimeParams: " << formatRuntimeParams(data.runtimeParams) << juce::newLine;

            if (data.activeGraph)
            {
                report << "runtime.chainA:" << juce::newLine << describeRuntimeChain(data.runtimeChainA) << juce::newLine
                    << "runtime.chainB:" << juce::newLine << describeRuntimeChain(data.runtimeChainB) << juce::newLine;
            }

            report << "model.chainA:" << juce::newLine << describeModelChain(data.modelChainA) << juce::newLine
                << "model.chainB:" << juce::newLine << describeModelChain(data.modelChainB);

            return report;
        }

        template <typename ProfilingResultLike>
        static juce::String formatProfilingLine(const ProfilingResultLike& r)
        {
            juce::String line;
            line << "block=" << r.blockSize
                << ", blocks=" << r.processedBlocks
                << ", avgMs=" << juce::String(r.avgMs, 4)
                << ", peakMs=" << juce::String(r.peakMs, 4)
                << ", avgCpu=" << juce::String(r.avgCpuPercent, 2) << "%"
                << ", peakCpu=" << juce::String(r.peakCpuPercent, 2) << "%"
                << ", invalid=" << r.invalidSamples
                << ", clipped=" << r.clippedSamples
                << ", dropouts=" << r.dropoutBlocks
                << ", clickSpikes=" << r.clickSpikeBlocks
                << ", passed=" << boolToText(r.passed);

            if (r.notes.isNotEmpty())
                line << ", notes=" << r.notes;

            return line;
        }

        template <typename ProfilingResultLike>
        static juce::String formatProfilingResults(const std::vector<ProfilingResultLike>& results)
        {
            juce::StringArray lines;
            lines.add("AudioEngine realtime profiling results:");
            for (const auto& r : results)
                lines.add(formatProfilingLine(r));
            return lines.joinIntoString(juce::newLine);
        }

    private:
        static juce::String boolToText(bool value)
        {
            return value ? "true" : "false";
        }

        static juce::String zoneToText(Nova::ZoneID zone)
        {
            switch (zone)
            {
            case Nova::ZoneID::Pre:     return "Pre";
            case Nova::ZoneID::Amp:     return "Amp";
            case Nova::ZoneID::FX:      return "FX";
            case Nova::ZoneID::Cabinet: return "Cabinet";
            default:                    return "Unknown";
            }
        }

        static juce::String switchModeToText(int mode)
        {
            switch (static_cast<Nova::SwitcherMode>(mode))
            {
            case Nova::SwitcherMode::LineA_Only:     return "LineA_Only";
            case Nova::SwitcherMode::LineB_Only:     return "LineB_Only";
            case Nova::SwitcherMode::Dual_Parallel:  return "Dual_Parallel";
            default:                                 return "Unknown";
            }
        }

        static juce::String formatRuntimeParams(const RuntimeGlobalParamsSnapshot& snapshot)
        {
            juce::String result;
            result << "inputGainDb=" << snapshot.inputGainDb
                << ", gateThresholdDb=" << snapshot.gateThresholdDb
                << ", forceMono=" << boolToText(snapshot.forceMono)
                << ", hostTempoBpm=" << snapshot.hostTempoBpm
                << ", hostTempoValid=" << boolToText(snapshot.hostTempoValid)
                << ", hostTransportPlaying=" << boolToText(snapshot.hostTransportPlaying)
                << ", outputVolumeDb=" << snapshot.outputVolumeDb
                << ", outputLimiterDb=" << snapshot.outputLimiterDb
                << ", outputMixRaw=" << snapshot.outputMixRaw
                << ", switchMode=" << switchModeToText(snapshot.switchMode)
                << ", gainA=" << snapshot.gainA
                << ", panA=" << snapshot.panA
                << ", widthA=" << snapshot.widthA
                << ", gainB=" << snapshot.gainB
                << ", panB=" << snapshot.panB
                << ", widthB=" << snapshot.widthB;
            return result;
        }

        static juce::String describeModelChain(const std::vector<ModelChainEntry>& chain)
        {
            juce::StringArray lines;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const auto& slot = chain[i];
                juce::String line;
                line << "#" << (int)i
                    << " type=" << slot.pedalType
                    << " zone=" << zoneToText(slot.zone)
                    << " pedalID=" << (slot.pedalID.isEmpty() ? "<none>" : slot.pedalID)
                    << " bypassed=" << boolToText(slot.bypassed);
                lines.add(line);
            }

            if (lines.isEmpty())
                lines.add("<empty>");

            return lines.joinIntoString(juce::newLine);
        }

        static juce::String describeRuntimeChain(const std::vector<RuntimeChainEntry>& chain)
        {
            juce::StringArray lines;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const auto& slot = chain[i];
                juce::String line;
                line << "#" << (int)i
                    << " type=" << slot.pedalType
                    << " zone=" << zoneToText(slot.zone)
                    << " pedalID=" << (slot.pedalID.isEmpty() ? "<none>" : slot.pedalID)
                    << " bypassed=" << boolToText(slot.bypassed)
                    << " processor=" << (slot.hasProcessor ? slot.processorName : juce::String("<null>"))
                    << " cachedBase=" << boolToText(slot.cachedBase)
                    << " cachedTempo=" << boolToText(slot.cachedTempo);
                lines.add(line);
            }

            if (lines.isEmpty())
                lines.add("<empty>");

            return lines.joinIntoString(juce::newLine);
        }
    };
}
}
