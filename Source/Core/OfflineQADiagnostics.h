#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>

#include "AudioEngine.h"
#include "SessionLogger.h"
#include "../Effects/Pedals/Reverb/ReverbPedal.h"

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

    static bool bufferHasOnlyFiniteSamples(const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (!std::isfinite(buffer.getSample(ch, i)))
                    return false;

        return true;
    }

    static double computeWindowRms(const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
    {
        const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
        const int safeLength = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
        if (safeLength <= 0)
            return 0.0;

        double sumSquares = 0.0;
        int count = 0;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            for (int i = 0; i < safeLength; ++i)
            {
                const double s = buffer.getSample(ch, safeStart + i);
                sumSquares += s * s;
                ++count;
            }
        }

        return count > 0 ? std::sqrt(sumSquares / (double)count) : 0.0;
    }

    static double computeChannelWindowRms(const juce::AudioBuffer<float>& buffer, int channel, int startSample, int numSamples)
    {
        if (!juce::isPositiveAndBelow(channel, buffer.getNumChannels()))
            return 0.0;

        const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
        const int safeLength = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
        if (safeLength <= 0)
            return 0.0;

        double sumSquares = 0.0;
        for (int i = 0; i < safeLength; ++i)
        {
            const double s = buffer.getSample(channel, safeStart + i);
            sumSquares += s * s;
        }

        return std::sqrt(sumSquares / (double)safeLength);
    }

    static double computeStereoCorrelation(const juce::AudioBuffer<float>& buffer, int startSample)
    {
        if (buffer.getNumChannels() < 2)
            return 1.0;

        const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
        const int samples = buffer.getNumSamples() - safeStart;
        if (samples <= 1)
            return 1.0;

        double sumL = 0.0;
        double sumR = 0.0;
        for (int i = safeStart; i < buffer.getNumSamples(); ++i)
        {
            sumL += buffer.getSample(0, i);
            sumR += buffer.getSample(1, i);
        }

        const double meanL = sumL / (double)samples;
        const double meanR = sumR / (double)samples;

        double cov = 0.0;
        double varL = 0.0;
        double varR = 0.0;
        for (int i = safeStart; i < buffer.getNumSamples(); ++i)
        {
            const double l = buffer.getSample(0, i) - meanL;
            const double r = buffer.getSample(1, i) - meanR;
            cov += l * r;
            varL += l * l;
            varR += r * r;
        }

        const double denom = std::sqrt(varL * varR);
        return denom > 1.0e-12 ? cov / denom : 1.0;
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

    static juce::AudioBuffer<float> generateLeftImpulse(int samples, float amplitude)
    {
        juce::AudioBuffer<float> buffer(2, samples);
        buffer.clear();
        buffer.setSample(0, 0, amplitude);
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

    static float normalisedChoiceIndex(const juce::AudioParameterChoice* param, int index)
    {
        if (param == nullptr || param->choices.size() <= 1)
            return 0.0f;

        const int clamped = juce::jlimit(0, param->choices.size() - 1, index);
        return (float)clamped / (float)(param->choices.size() - 1);
    }

    static ReverbPedal* getOfflineReverb(AudioEngine& engine)
    {
        return dynamic_cast<ReverbPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0));
    }

    static void configureFlagshipCloud(ReverbPedal& reverb)
    {
        reverb.modeParam->setValueNotifyingHost(normalisedChoiceIndex(reverb.modeParam, 5));
        reverb.decayParam->setValueNotifyingHost(reverb.decayParam->convertTo0to1(0.88f));
        reverb.toneParam->setValueNotifyingHost(reverb.toneParam->convertTo0to1(0.72f));
        reverb.sizeParam->setValueNotifyingHost(reverb.sizeParam->convertTo0to1(0.86f));
        reverb.dampingParam->setValueNotifyingHost(reverb.dampingParam->convertTo0to1(0.33f));
        reverb.bassCutParam->setValueNotifyingHost(reverb.bassCutParam->convertTo0to1(0.22f));
        reverb.diffusionParam->setValueNotifyingHost(reverb.diffusionParam->convertTo0to1(0.92f));
        reverb.widthParam->setValueNotifyingHost(reverb.widthParam->convertTo0to1(1.0f));
        reverb.modParam->setValueNotifyingHost(reverb.modParam->convertTo0to1(0.42f));
        reverb.predelayParam->setValueNotifyingHost(reverb.predelayParam->convertTo0to1(24.0f));
        reverb.mixParam->setValueNotifyingHost(reverb.mixParam->convertTo0to1(1.0f));
        reverb.duckParam->setValueNotifyingHost(reverb.duckParam->convertTo0to1(0.0f));
        reverb.freezeParam->setValueNotifyingHost(0.0f);
    }

    static void configureFlagshipHall(ReverbPedal& reverb)
    {
        reverb.modeParam->setValueNotifyingHost(normalisedChoiceIndex(reverb.modeParam, 2));
        reverb.decayParam->setValueNotifyingHost(reverb.decayParam->convertTo0to1(0.76f));
        reverb.toneParam->setValueNotifyingHost(reverb.toneParam->convertTo0to1(0.66f));
        reverb.sizeParam->setValueNotifyingHost(reverb.sizeParam->convertTo0to1(0.72f));
        reverb.dampingParam->setValueNotifyingHost(reverb.dampingParam->convertTo0to1(0.30f));
        reverb.bassCutParam->setValueNotifyingHost(reverb.bassCutParam->convertTo0to1(0.18f));
        reverb.diffusionParam->setValueNotifyingHost(reverb.diffusionParam->convertTo0to1(0.84f));
        reverb.widthParam->setValueNotifyingHost(reverb.widthParam->convertTo0to1(1.0f));
        reverb.modParam->setValueNotifyingHost(reverb.modParam->convertTo0to1(0.26f));
        reverb.predelayParam->setValueNotifyingHost(reverb.predelayParam->convertTo0to1(12.0f));
        reverb.mixParam->setValueNotifyingHost(reverb.mixParam->convertTo0to1(1.0f));
        reverb.duckParam->setValueNotifyingHost(reverb.duckParam->convertTo0to1(0.0f));
        reverb.freezeParam->setValueNotifyingHost(0.0f);
    }

    static std::vector<OfflineQAScenarioResult> runAllScenarios()
    {
        std::vector<OfflineQAScenarioResult> results;
        results.push_back(runImpulseTransparencyScenario());
        results.push_back(runDryOnlyNullScenario());
        results.push_back(runDisabledEngineNullScenario());
        results.push_back(runParallelNoiseUnityScenario());
        results.push_back(runEngineReenableRecoveryScenario());
        results.push_back(runReverbTailScenario());
        results.push_back(runReverbStereoFieldScenario());
        results.push_back(runReverbModeDistinctnessScenario());
        results.push_back(runReverbFreezeScenario());
        results.push_back(runReverbDuckingScenario());
        results.push_back(runReverbAutomationStressScenario());
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

        result.passed = output.getSample(0, 0) > 0.99f
            && output.getSample(1, 0) > 0.99f
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
        params.inputGainDb = 0.0f;
        params.outputVolumeDb = 0.0f;
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

        result.passed = nullRms < 1.0e-2;
        result.notes = result.passed ? "Dry-only path stays perceptually transparent" : "Dry-only path altered the input";
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

    static OfflineQAScenarioResult runReverbTailScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_tail_quality";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        engine.addPedal("Reverb", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "offline-reverb-tail");
        engine.setEngineEnabled(true);
        warmUpEngine(engine, 16);

        auto* reverb = getOfflineReverb(engine);
        if (reverb == nullptr)
        {
            result.notes = "Failed to resolve offline reverb processor from graph";
            return result;
        }

        configureFlagshipCloud(*reverb);

        const auto input = generateImpulse((int)(sampleRate * 6.0), 1.0f);
        const double startMs = juce::Time::getMillisecondCounterHiRes();
        const auto output = processBuffer(engine, input);
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;

        const double lateRms = computeWindowRms(output, (int)(sampleRate * 0.8), (int)(sampleRate * 0.5));
        const double endRms = computeWindowRms(output, output.getNumSamples() - (int)(sampleRate * 0.25), (int)(sampleRate * 0.25));
        const auto metrics = analyseBuffer(output);
        const bool finite = bufferHasOnlyFiniteSamples(output);

        result.metrics.push_back({ "render_ms", elapsedMs });
        result.metrics.push_back({ "peak", metrics.peak });
        result.metrics.push_back({ "rms", metrics.rms });
        result.metrics.push_back({ "late_rms_0p8_1p3s", lateRms });
        result.metrics.push_back({ "tail_end_rms_last_250ms", endRms });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && metrics.peak < 1.35
            && lateRms > 1.0e-4
            && endRms < lateRms * 0.50;
        result.notes = result.passed ? "Cloud tail stayed finite, sustained and decayed cleanly"
                                     : "Cloud tail failed finite/decay expectations";
        return result;
    }

    static OfflineQAScenarioResult runReverbStereoFieldScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_stereo_field";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        engine.addPedal("Reverb", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "offline-reverb-stereo");
        engine.setEngineEnabled(true);
        warmUpEngine(engine, 16);

        auto* reverb = getOfflineReverb(engine);
        if (reverb == nullptr)
        {
            result.notes = "Failed to resolve offline reverb processor from graph";
            return result;
        }

        configureFlagshipHall(*reverb);

        const auto output = processBuffer(engine, generateLeftImpulse((int)(sampleRate * 2.5), 1.0f));
        const bool finite = bufferHasOnlyFiniteSamples(output);
        const double corr = computeStereoCorrelation(output, (int)(sampleRate * 0.05));
        const double rmsLeft = computeChannelWindowRms(output, 0, (int)(sampleRate * 0.1), (int)(sampleRate * 1.0));
        const double rmsRight = computeChannelWindowRms(output, 1, (int)(sampleRate * 0.1), (int)(sampleRate * 1.0));
        const double sideRatio = rmsRight / juce::jmax(1.0e-9, rmsLeft);

        result.metrics.push_back({ "corr_after_50ms", corr });
        result.metrics.push_back({ "left_rms_100_1100ms", rmsLeft });
        result.metrics.push_back({ "right_rms_100_1100ms", rmsRight });
        result.metrics.push_back({ "right_to_left_ratio", sideRatio });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && std::abs(corr) < 0.97 && sideRatio > 0.12;
        result.notes = result.passed ? "Hall mode projected a decorrelated stereo tail"
                                     : "Hall mode stereo field collapsed or stayed too imbalanced";
        return result;
    }

    static OfflineQAScenarioResult runReverbFreezeScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_freeze_hold";

        ReverbPedal reverb;
        reverb.prepareToPlay(sampleRate, blockSize);
        configureFlagshipCloud(reverb);

        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, blockSize);
        juce::AudioBuffer<float> output(2, (int)(sampleRate * 3.0));
        output.clear();

        block.clear();
        block.setSample(0, 0, 1.0f);
        block.setSample(1, 0, 1.0f);
        reverb.processBlock(block, midi);
        output.copyFrom(0, 0, block, 0, 0, blockSize);
        output.copyFrom(1, 0, block, 1, 0, blockSize);

        reverb.freezeParam->setValueNotifyingHost(1.0f);

        for (int offset = blockSize; offset < output.getNumSamples(); offset += blockSize)
        {
            const int numSamples = juce::jmin(blockSize, output.getNumSamples() - offset);
            block.clear();
            reverb.processBlock(block, midi);
            output.copyFrom(0, offset, block, 0, 0, numSamples);
            output.copyFrom(1, offset, block, 1, 0, numSamples);
        }

        const bool finite = bufferHasOnlyFiniteSamples(output);
        const double earlyRms = computeWindowRms(output, (int)(sampleRate * 0.8), (int)(sampleRate * 0.4));
        const double heldRms = computeWindowRms(output, output.getNumSamples() - (int)(sampleRate * 0.4), (int)(sampleRate * 0.3));

        result.metrics.push_back({ "early_rms_0p8_1p2s", earlyRms });
        result.metrics.push_back({ "held_rms_last_300ms", heldRms });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && heldRms > earlyRms * 0.60;
        result.notes = result.passed ? "Freeze captured and held a stable ambient pad"
                                     : "Freeze failed to sustain enough of the captured tail";
        return result;
    }

    static OfflineQAScenarioResult runReverbModeDistinctnessScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_mode_distinctness";

        auto renderMode = [](int modeIndex)
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.74f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.66f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.70f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.84f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(1.0f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(16.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.6));
            input.clear();
            input.setSample(0, 0, 1.0f);
            input.setSample(1, 0, 1.0f);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> rendered(2, input.getNumSamples());
            rendered.clear();
            juce::AudioBuffer<float> blockBuf(2, blockSize);

            for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
            {
                const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
                blockBuf.clear();
                for (int ch = 0; ch < 2; ++ch)
                    blockBuf.copyFrom(ch, 0, input, ch, offset, numSamples);
                pedal.processBlock(blockBuf, midi);
                rendered.copyFrom(0, offset, blockBuf, 0, 0, numSamples);
                rendered.copyFrom(1, offset, blockBuf, 1, 0, numSamples);
            }

            return rendered;
        };

        const auto spring = renderMode(0);
        const auto plate = renderMode(1);
        const auto hall = renderMode(2);

        const double springPlateNull = computeNullRms(spring, plate);
        const double plateHallNull = computeNullRms(plate, hall);
        const double springHallNull = computeNullRms(spring, hall);

        result.metrics.push_back({ "spring_plate_null_rms", springPlateNull });
        result.metrics.push_back({ "plate_hall_null_rms", plateHallNull });
        result.metrics.push_back({ "spring_hall_null_rms", springHallNull });

        result.passed = springPlateNull > 8.0e-4
            && plateHallNull > 7.0e-4
            && springHallNull > 8.0e-4;
        result.notes = result.passed ? "Spring, Plate and Hall produce clearly separated impulse signatures"
                                     : "Hero modes still overlap too much in their rendered tails";
        return result;
    }

    static OfflineQAScenarioResult runReverbDuckingScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_ducking_response";

        ReverbPedal baseline;
        baseline.prepareToPlay(sampleRate, blockSize);
        configureFlagshipHall(baseline);

        ReverbPedal ducked;
        ducked.prepareToPlay(sampleRate, blockSize);
        configureFlagshipHall(ducked);
        ducked.duckParam->setValueNotifyingHost(ducked.duckParam->convertTo0to1(0.90f));

        const auto input = generateSine((int)(sampleRate * 1.5), 220.0, 0.22f);
        juce::MidiBuffer midi;

        auto renderPedal = [&](ReverbPedal& pedal)
        {
            juce::AudioBuffer<float> rendered(2, input.getNumSamples());
            rendered.clear();
            juce::AudioBuffer<float> block(2, blockSize);

            for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
            {
                const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
                block.clear();
                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom(ch, 0, input, ch, offset, numSamples);
                pedal.processBlock(block, midi);
                rendered.copyFrom(0, offset, block, 0, 0, numSamples);
                rendered.copyFrom(1, offset, block, 1, 0, numSamples);
            }

            return rendered;
        };

        const auto baselineOut = renderPedal(baseline);
        const auto duckedOut = renderPedal(ducked);
        const bool finite = bufferHasOnlyFiniteSamples(duckedOut);
        const double baselineRms = computeWindowRms(baselineOut, (int)(sampleRate * 0.6), (int)(sampleRate * 0.3));
        const double duckedRms = computeWindowRms(duckedOut, (int)(sampleRate * 0.6), (int)(sampleRate * 0.3));

        result.metrics.push_back({ "baseline_rms", baselineRms });
        result.metrics.push_back({ "ducked_rms", duckedRms });
        result.metrics.push_back({ "rms_ratio", duckedRms / juce::jmax(1.0e-9, baselineRms) });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && duckedRms < baselineRms * 0.75;
        result.notes = result.passed ? "Ducking carved audible space while the source stayed active"
                                     : "Ducking did not reduce the wet bed enough under sustained input";
        return result;
    }

    static OfflineQAScenarioResult runReverbAutomationStressScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_automation_stress";

        ReverbPedal reverb;
        reverb.prepareToPlay(sampleRate, blockSize);
        reverb.mixParam->setValueNotifyingHost(reverb.mixParam->convertTo0to1(0.62f));

        juce::Random rng(0xC10D3);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, blockSize);
        bool finite = true;
        double peak = 0.0;

        const int blocksToRun = (int)((sampleRate * 3.0) / (double)blockSize);
        for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    block.setSample(ch, i, 0.12f * ((rng.nextFloat() * 2.0f) - 1.0f));

            const float phase = (float)blockIndex / (float)juce::jmax(1, blocksToRun - 1);
            const int mode = juce::jlimit(0, 5, (int)std::floor(phase * 6.0f));

            reverb.modeParam->setValueNotifyingHost(normalisedChoiceIndex(reverb.modeParam, mode));
            reverb.decayParam->setValueNotifyingHost(reverb.decayParam->convertTo0to1(0.35f + 0.55f * phase));
            reverb.toneParam->setValueNotifyingHost(reverb.toneParam->convertTo0to1(0.30f + 0.60f * (1.0f - phase)));
            reverb.sizeParam->setValueNotifyingHost(reverb.sizeParam->convertTo0to1(0.25f + 0.70f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
            reverb.dampingParam->setValueNotifyingHost(reverb.dampingParam->convertTo0to1(0.15f + 0.70f * phase));
            reverb.diffusionParam->setValueNotifyingHost(reverb.diffusionParam->convertTo0to1(0.45f + 0.50f * (1.0f - phase)));
            reverb.widthParam->setValueNotifyingHost(reverb.widthParam->convertTo0to1(0.25f + 0.75f * phase));
            reverb.modParam->setValueNotifyingHost(reverb.modParam->convertTo0to1(0.10f + 0.60f * std::abs(std::cos(phase * juce::MathConstants<float>::twoPi))));
            reverb.predelayParam->setValueNotifyingHost(reverb.predelayParam->convertTo0to1(phase * 180.0f));
            reverb.duckParam->setValueNotifyingHost(reverb.duckParam->convertTo0to1(0.75f * (1.0f - phase)));
            reverb.freezeParam->setValueNotifyingHost((blockIndex % 257) == 0 ? 1.0f : 0.0f);

            reverb.processBlock(block, midi);
            peak = juce::jmax(peak, (double)analyseBuffer(block).peak);
            finite = finite && bufferHasOnlyFiniteSamples(block);
        }

        result.metrics.push_back({ "peak", peak });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.metrics.push_back({ "blocks", (double)blocksToRun });

        result.passed = finite && peak < 2.0;
        result.notes = result.passed ? "Aggressive automation stayed finite and inside a sane ceiling"
                                     : "Automation stress produced unstable or excessive output";
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
