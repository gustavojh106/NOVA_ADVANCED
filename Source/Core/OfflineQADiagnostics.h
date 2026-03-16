#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>

#include "AudioEngine.h"
#include "SessionLogger.h"

namespace NovaDiagnostics
{
struct OfflineQAMetric
{
    juce::String name;
    double value = 0.0;
};

struct OfflineQAScenarioResult
{
    juce::String name;
    bool passed = false;
    juce::String notes;
    std::vector<OfflineQAMetric> metrics;
};

class OfflineQADiagnostics final
{
public:
    static juce::File getReportFile()
    {
        auto logFile = SessionLogger::getLogFile();
        auto parent = logFile.getParentDirectory();
        if (!parent.exists())
            parent.createDirectory();

        return parent.getChildFile("offline-qa-report.txt");
    }

    static void runAndWriteReport()
    {
        const auto results = runAllScenarios();
        writeReport(results);
    }

private:
    struct BufferMetrics
    {
        double peak = 0.0;
        double rms = 0.0;
        double meanAbs = 0.0;
    };

    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 64;

    static BufferMetrics analyseBuffer(const juce::AudioBuffer<float>& buffer)
    {
        BufferMetrics metrics;
        double sumSquares = 0.0;
        double sumAbs = 0.0;
        int sampleCount = 0;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            metrics.peak = juce::jmax(metrics.peak, (double)buffer.getMagnitude(ch, 0, buffer.getNumSamples()));

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const double sample = buffer.getSample(ch, i);
                sumSquares += sample * sample;
                sumAbs += std::abs(sample);
                ++sampleCount;
            }
        }

        if (sampleCount > 0)
        {
            metrics.rms = std::sqrt(sumSquares / (double)sampleCount);
            metrics.meanAbs = sumAbs / (double)sampleCount;
        }

        return metrics;
    }

    static juce::AudioBuffer<float> concatenateBlocks(const std::vector<juce::AudioBuffer<float>>& blocks)
    {
        int totalSamples = 0;
        int numChannels = 0;

        for (const auto& block : blocks)
        {
            totalSamples += block.getNumSamples();
            numChannels = juce::jmax(numChannels, block.getNumChannels());
        }

        juce::AudioBuffer<float> merged(juce::jmax(1, numChannels), juce::jmax(1, totalSamples));
        merged.clear();

        int writeOffset = 0;
        for (const auto& block : blocks)
        {
            for (int ch = 0; ch < block.getNumChannels(); ++ch)
                merged.copyFrom(ch, writeOffset, block, ch, 0, block.getNumSamples());

            writeOffset += block.getNumSamples();
        }

        return merged;
    }

    static double computeNullRms(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        const int channels = juce::jmin(a.getNumChannels(), b.getNumChannels());
        const int samples = juce::jmin(a.getNumSamples(), b.getNumSamples());

        double sumSquares = 0.0;
        int sampleCount = 0;

        for (int ch = 0; ch < channels; ++ch)
        {
            for (int i = 0; i < samples; ++i)
            {
                const double delta = (double)a.getSample(ch, i) - (double)b.getSample(ch, i);
                sumSquares += delta * delta;
                ++sampleCount;
            }
        }

        return sampleCount > 0 ? std::sqrt(sumSquares / (double)sampleCount) : 0.0;
    }

    static int findPeakSampleIndex(const juce::AudioBuffer<float>& buffer, int channel)
    {
        int bestIndex = 0;
        float bestMagnitude = 0.0f;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const float magnitude = std::abs(buffer.getSample(channel, i));
            if (magnitude > bestMagnitude)
            {
                bestMagnitude = magnitude;
                bestIndex = i;
            }
        }

        return bestIndex;
    }

    static juce::AudioBuffer<float> generateSine(int samples, double frequency, float amplitude)
    {
        juce::AudioBuffer<float> buffer(2, samples);
        float phase = 0.0f;
        const float increment = (float)(2.0 * juce::MathConstants<double>::pi * frequency / sampleRate);

        for (int i = 0; i < samples; ++i)
        {
            const float value = amplitude * std::sin(phase);
            buffer.setSample(0, i, value);
            buffer.setSample(1, i, value);
            phase += increment;
        }

        return buffer;
    }

    static juce::AudioBuffer<float> generateNoise(int samples, uint32_t seed, float amplitude)
    {
        juce::Random rng((int)seed);
        juce::AudioBuffer<float> buffer(2, samples);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            for (int i = 0; i < samples; ++i)
                buffer.setSample(ch, i, amplitude * ((rng.nextFloat() * 2.0f) - 1.0f));
        }

        return buffer;
    }

    static juce::AudioBuffer<float> generateImpulse(int samples, float amplitude)
    {
        juce::AudioBuffer<float> buffer(2, samples);
        buffer.clear();
        buffer.setSample(0, 0, amplitude);
        buffer.setSample(1, 0, amplitude);
        return buffer;
    }

    static void warmUpEngine(AudioEngine& engine, int blocks = 12)
    {
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> scratch(2, blockSize);

        for (int i = 0; i < blocks; ++i)
        {
            scratch.clear();
            engine.process(scratch, midi);
        }
    }

    static juce::AudioBuffer<float> processBuffer(AudioEngine& engine, const juce::AudioBuffer<float>& input)
    {
        juce::MidiBuffer midi;
        std::vector<juce::AudioBuffer<float>> blocks;
        const int totalSamples = input.getNumSamples();

        for (int offset = 0; offset < totalSamples; offset += blockSize)
        {
            const int numSamples = juce::jmin(blockSize, totalSamples - offset);
            juce::AudioBuffer<float> block(2, blockSize);
            block.clear();

            for (int ch = 0; ch < juce::jmin(2, input.getNumChannels()); ++ch)
                block.copyFrom(ch, 0, input, ch, offset, numSamples);

            engine.process(block, midi);

            juce::AudioBuffer<float> trimmed(2, numSamples);
            for (int ch = 0; ch < trimmed.getNumChannels(); ++ch)
                trimmed.copyFrom(ch, 0, block, ch, 0, numSamples);

            blocks.push_back(std::move(trimmed));
        }

        return concatenateBlocks(blocks);
    }

    static std::vector<OfflineQAScenarioResult> runAllScenarios()
    {
        std::vector<OfflineQAScenarioResult> results;
        results.push_back(runImpulseTransparencyScenario());
        results.push_back(runDryOnlyNullScenario());
        results.push_back(runDisabledEngineNullScenario());
        results.push_back(runParallelNoiseUnityScenario());
        results.push_back(runEngineReenableRecoveryScenario());
        results.push_back(runGraphDiagnosticsScenario());
        return results;
    }

    static OfflineQAScenarioResult runImpulseTransparencyScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "impulse_transparency_line_a";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        engine.setEngineEnabled(true);
        warmUpEngine(engine);

        const auto input = generateImpulse(512, 1.0f);
        const auto output = processBuffer(engine, input);
        const auto metrics = analyseBuffer(output);
        const int peakIndexL = findPeakSampleIndex(output, 0);
        const int peakIndexR = findPeakSampleIndex(output, 1);

        result.metrics.push_back({ "output_peak", metrics.peak });
        result.metrics.push_back({ "output_rms", metrics.rms });
        result.metrics.push_back({ "peak_index_left", (double)peakIndexL });
        result.metrics.push_back({ "peak_index_right", (double)peakIndexR });
        result.metrics.push_back({ "first_sample_left", output.getSample(0, 0) });
        result.metrics.push_back({ "first_sample_right", output.getSample(1, 0) });

        result.passed = std::abs(output.getSample(0, 0) - 1.0f) < 2.0e-4f
            && std::abs(output.getSample(1, 0) - 1.0f) < 2.0e-4f
            && peakIndexL == 0
            && peakIndexR == 0;
        result.notes = result.passed ? "Impulse stayed transparent" : "Impulse response deviated from clean pass-through";
        return result;
    }

    static OfflineQAScenarioResult runDryOnlyNullScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "dry_only_null_test";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.inputGainDb = 12.0f;
        params.outputVolumeDb = -9.0f;
        params.outputMixRaw = 0.0f;
        engine.updateGlobalParams(params);
        engine.setEngineEnabled(true);
        warmUpEngine(engine);

        const auto input = generateSine(4096, 440.0, 0.35f);
        const auto output = processBuffer(engine, input);
        const double nullRms = computeNullRms(input, output);

        result.metrics.push_back({ "null_rms", nullRms });
        result.metrics.push_back({ "input_peak", analyseBuffer(input).peak });
        result.metrics.push_back({ "output_peak", analyseBuffer(output).peak });

        result.passed = nullRms < 2.0e-4;
        result.notes = result.passed ? "Dry-only path nulls against input" : "Dry-only path altered the input";
        return result;
    }

    static OfflineQAScenarioResult runDisabledEngineNullScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "engine_disabled_null_test";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.inputGainDb = 18.0f;
        params.outputVolumeDb = -12.0f;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        warmUpEngine(engine);

        const auto input = generateNoise(4096, 0x51414u, 0.25f);
        const auto output = processBuffer(engine, input);
        const double nullRms = computeNullRms(input, output);

        result.metrics.push_back({ "null_rms", nullRms });
        result.metrics.push_back({ "input_rms", analyseBuffer(input).rms });
        result.metrics.push_back({ "output_rms", analyseBuffer(output).rms });

        result.passed = nullRms < 2.0e-4;
        result.notes = result.passed ? "Disabled engine preserved dry input" : "Disabled engine altered input unexpectedly";
        return result;
    }

    static OfflineQAScenarioResult runParallelNoiseUnityScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "parallel_noise_unity";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::Dual_Parallel;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        engine.setEngineEnabled(true);
        warmUpEngine(engine);

        const auto input = generateNoise(4096, 0xBEEF11u, 0.2f);
        const auto output = processBuffer(engine, input);
        const auto inMetrics = analyseBuffer(input);
        const auto outMetrics = analyseBuffer(output);
        const double rmsRatio = outMetrics.rms / juce::jmax(1.0e-9, inMetrics.rms);

        result.metrics.push_back({ "input_rms", inMetrics.rms });
        result.metrics.push_back({ "output_rms", outMetrics.rms });
        result.metrics.push_back({ "rms_ratio", rmsRatio });

        result.passed = std::abs(rmsRatio - 1.0) < 0.02;
        result.notes = result.passed ? "Parallel clean path stays near unity" : "Parallel clean path drifted away from unity";
        return result;
    }

    static OfflineQAScenarioResult runEngineReenableRecoveryScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "engine_reenable_recovery";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        engine.setEngineEnabled(true);
        warmUpEngine(engine);

        const auto input = generateSine(4096, 220.0, 0.4f);
        const auto before = processBuffer(engine, input);

        engine.setEngineEnabled(false);
        warmUpEngine(engine, 4);
        const auto disabled = processBuffer(engine, input);

        engine.setEngineEnabled(true);
        warmUpEngine(engine, 12);
        const auto after = processBuffer(engine, input);

        const double disabledNullRms = computeNullRms(input, disabled);
        const double reenabledNullRms = computeNullRms(before, after);

        result.metrics.push_back({ "disabled_null_rms", disabledNullRms });
        result.metrics.push_back({ "reenabled_null_rms", reenabledNullRms });

        result.passed = disabledNullRms < 2.0e-4 && reenabledNullRms < 2.0e-4;
        result.notes = result.passed ? "Engine recovered cleanly after re-enable" : "Engine state changed after disable/re-enable";
        return result;
    }

    static OfflineQAScenarioResult runGraphDiagnosticsScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "graph_diagnostic_report";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        engine.addPedal("Delay", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "offline-delay");
        engine.addPedal("Reverb", Nova::ChainID::LineA, 1, Nova::ZoneID::FX, "offline-reverb");
        engine.setEngineEnabled(true);
        warmUpEngine(engine, 16);

        const auto report = engine.buildDiagnosticReport();
        const bool hasDelay = report.contains("processor=Delay");
        const bool hasReverb = report.contains("processor=Reverb");
        const bool hasIDs = report.contains("pedalID=offline-delay") && report.contains("pedalID=offline-reverb");

        result.metrics.push_back({ "report_length_chars", (double)report.length() });
        result.metrics.push_back({ "contains_delay", hasDelay ? 1.0 : 0.0 });
        result.metrics.push_back({ "contains_reverb", hasReverb ? 1.0 : 0.0 });
        result.metrics.push_back({ "contains_ids", hasIDs ? 1.0 : 0.0 });

        result.passed = hasDelay && hasReverb && hasIDs;
        result.notes = result.passed ? "Diagnostic report reflects graph topology" : "Diagnostic report missed expected topology entries";
        return result;
    }

    static void writeReport(const std::vector<OfflineQAScenarioResult>& results)
    {
        auto reportFile = getReportFile();
        if (reportFile.existsAsFile())
            reportFile.deleteFile();

        auto stream = reportFile.createOutputStream();
        if (stream == nullptr)
            return;

        juce::String text;
        text << "NOVA Offline QA Report" << juce::newLine;
        text << "Generated: " << juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S") << juce::newLine;
        text << "Scenarios: " << results.size() << juce::newLine << juce::newLine;

        int passedCount = 0;
        for (const auto& result : results)
        {
            if (result.passed)
                ++passedCount;

            text << "[" << (result.passed ? "PASS" : "FAIL") << "] " << result.name << juce::newLine;
            text << "notes=" << result.notes << juce::newLine;
            for (const auto& metric : result.metrics)
                text << metric.name << "=" << juce::String(metric.value, 8) << juce::newLine;
            text << juce::newLine;
        }

        text << "summary.passed=" << passedCount << juce::newLine;
        text << "summary.failed=" << (int)results.size() - passedCount << juce::newLine;

        stream->writeText(text, false, false, "\n");
        stream->flush();

        SessionLogger::logEvent("qa.offline",
            "Offline QA report written to " + reportFile.getFullPathName()
            + juce::newLine + "passed=" + juce::String(passedCount)
            + ", failed=" + juce::String((int)results.size() - passedCount));
    }
};
}
