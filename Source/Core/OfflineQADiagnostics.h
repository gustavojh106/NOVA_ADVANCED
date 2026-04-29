#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <functional>
#include <type_traits>
#include <vector>

#include "AudioEngine.h"
#include "DSP/Global/OutputChain.h"
#include "SessionLogger.h"
#include "../Effects/Amplifiers/HighGainAmp.h"
#include "../Effects/Amplifiers/CleanAmp.h"
#include "../Effects/Cabinets/CabinetPedal.h"
#include "../Effects/Cabinets/Modern4x12Cabinet.h"
#include "../Effects/Cabinets/Vintage2x12Cabinet.h"
#include "../Effects/Pedals/Boost/BoostPedal.h"
#include "../Effects/Pedals/Chorus/ChorusPedal.h"
#include "../Effects/Pedals/Compressor/CompressorPedal.h"
#include "../Effects/Pedals/Delay/DelayPedal.h"
#include "../Effects/Pedals/Distortion/DistortionPedal.h"
#include "../Effects/Pedals/EQ/EQPedal.h"
#include "../Effects/Pedals/Flanger/FlangerPedal.h"
#include "../Effects/Pedals/Fuzz/FuzzPedal.h"
#include "../Effects/Pedals/Gate/NoiseGatePedal.h"
#include "../Effects/Pedals/Neural/NeuralPedal.h"
#include "../Effects/Pedals/Octave/OctavePedal.h"
#include "../Effects/Pedals/Overdrive/OverdrivePedal.h"
#include "../Effects/Pedals/Phaser/PhaserPedal.h"
#include "../Effects/Pedals/Reverb/ReverbPedal.h"
#include "../Effects/Pedals/Tremolo/TremoloPedal.h"
#include "../Effects/Pedals/Wah/ClassicWahPedal.h"

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
        if (const auto overridePath = juce::SystemStats::getEnvironmentVariable("NOVA_QA_REPORT_PATH", {});
            overridePath.isNotEmpty())
        {
            auto file = juce::File::getCurrentWorkingDirectory().getChildFile(overridePath);
            if (juce::File::isAbsolutePath(overridePath))
                file = juce::File(overridePath);

            auto parent = file.getParentDirectory();
            if (!parent.exists())
                parent.createDirectory();

            return file;
        }

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

    static void runRtProfileAndWriteReport()
    {
        const auto results = runRtProfileScenarios();
        writeRtProfileReport(results);
    }

private:
    struct BufferMetrics
    {
        double peak = 0.0;
        double rms = 0.0;
        double meanAbs = 0.0;
    };

    struct RtProfileResult
    {
        juce::String name;
        juce::String status{ "PASS" };
        juce::String warnings;
        double sampleRate = 0.0;
        int blockSize = 0;
        int processedBlocks = 0;
        double avgProcessMs = 0.0;
        double peakProcessMs = 0.0;
        double cpuAvgPercent = 0.0;
        double cpuPeakPercent = 0.0;
        double maxBudgetRatio = 0.0;
        int blocksOver50 = 0;
        int blocksOver75 = 0;
        int blocksOver90 = 0;
        int blocksOver100 = 0;
        int invalidSamples = 0;
        int clippedSamples = 0;
        int nearClipSamples = 0;
        int denormalLikeSamples = 0;
        int fallbackBlockCount = 0;
        int limiterTouchedSamples = 0;
        float limiterMaxReductionDb = 0.0f;
        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
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

    static double computeDcAbs(const juce::AudioBuffer<float>& buffer, int startSample = 0, int numSamples = -1)
    {
        const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
        const int maxLength = buffer.getNumSamples() - safeStart;
        const int safeLength = juce::jlimit(0, maxLength, numSamples < 0 ? maxLength : numSamples);
        if (safeLength <= 0 || buffer.getNumChannels() <= 0)
            return 0.0;

        double dcSum = 0.0;
        int dcChannels = 0;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            double channelSum = 0.0;
            for (int i = 0; i < safeLength; ++i)
                channelSum += buffer.getSample(ch, safeStart + i);

            dcSum += std::abs(channelSum / (double)safeLength);
            ++dcChannels;
        }

        return dcChannels > 0 ? dcSum / (double)dcChannels : 0.0;
    }

    static int countNearClipSamples(const juce::AudioBuffer<float>& buffer)
    {
        int nearClipSamples = 0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (std::abs(buffer.getSample(ch, i)) >= Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD)
                    ++nearClipSamples;

        return nearClipSamples;
    }

    static double computeFrequencyMagnitude(const juce::AudioBuffer<float>& buffer,
        double sr,
        float targetFreq,
        int startSample,
        int numSamples)
    {
        const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
        const int safeLength = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
        if (safeLength <= 0 || sr <= 0.0 || targetFreq <= 0.0f)
            return 0.0;

        const double omega = 2.0 * juce::MathConstants<double>::pi * (double) targetFreq / sr;
        const double coeff = 2.0 * std::cos(omega);
        const double sine = std::sin(omega);
        const double cosine = std::cos(omega);
        const int channels = juce::jmax(1, buffer.getNumChannels());

        double magnitude = 0.0;
        for (int ch = 0; ch < channels; ++ch)
        {
            double q0 = 0.0;
            double q1 = 0.0;
            double q2 = 0.0;

            for (int i = 0; i < safeLength; ++i)
            {
                q0 = coeff * q1 - q2 + (double) buffer.getSample(ch, safeStart + i);
                q2 = q1;
                q1 = q0;
            }

            const double real = q1 - q2 * cosine;
            const double imag = q2 * sine;
            magnitude += std::sqrt(real * real + imag * imag) / (double) safeLength;
        }

        return magnitude / (double) channels;
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

    static juce::AudioBuffer<float> generateBiasedSine(int samples,
        double frequency,
        float amplitude,
        float dcOffset)
    {
        juce::AudioBuffer<float> buffer(2, samples);
        float phase = 0.0f;
        const float increment = (float)(2.0 * juce::MathConstants<double>::pi * frequency / sampleRate);

        for (int i = 0; i < samples; ++i)
        {
            const float value = (amplitude * std::sin(phase)) + dcOffset;
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

    static void fillProfileGuitarBlock(juce::AudioBuffer<float>& buffer,
        int blockIndex,
        double sr,
        float gain)
    {
        const int blockSamples = buffer.getNumSamples();

        for (int i = 0; i < blockSamples; ++i)
        {
            const double t = ((double)blockIndex * blockSamples + i) / sr;
            const float envelope = t < 0.02 ? (float)(t / 0.02) : 1.0f;
            const float sample = gain * envelope * (
                0.70f * (float)std::sin(juce::MathConstants<double>::twoPi * 146.0 * t)
                + 0.22f * (float)std::sin(juce::MathConstants<double>::twoPi * 292.0 * t)
                + 0.08f * (float)std::sin(juce::MathConstants<double>::twoPi * 1170.0 * t));

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample(ch, i, sample);
        }
    }

    static void fillQuietNoiseBlock(juce::AudioBuffer<float>& buffer,
        int blockIndex,
        float gain)
    {
        juce::Random rng(0x2741u + blockIndex * 17);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample(ch, i, gain * ((rng.nextFloat() * 2.0f) - 1.0f));
    }

    static int readFallbackBlockCount(juce::AudioProcessor* processor)
    {
        if (auto* p = dynamic_cast<CabinetPedal*>(processor)) return p->getRealtimeFallbackCount();
        if (auto* p = dynamic_cast<Vintage2x12Cabinet*>(processor)) return p->getRealtimeFallbackCount();
        if (auto* p = dynamic_cast<Modern4x12Cabinet*>(processor)) return p->getRealtimeFallbackCount();
        if (auto* p = dynamic_cast<CompressorPedal*>(processor)) return p->getRealtimeFallbackCount();
        if (auto* p = dynamic_cast<DistortionPedal*>(processor)) return p->getRealtimeFallbackCount();
        if (auto* p = dynamic_cast<FuzzPedal*>(processor)) return p->getRealtimeFallbackCount();
        if (auto* p = dynamic_cast<NeuralPedal*>(processor)) return p->getRealtimeFallbackCount();
        if (auto* p = dynamic_cast<ClassicWahPedal*>(processor)) return p->getRealtimeFallbackCount();
        if (auto* p = dynamic_cast<PhaserPedal*>(processor)) return p->getRealtimeFallbackCount();
        return 0;
    }

    static int collectLineAFallbackBlockCount(AudioEngine& engine)
    {
        int total = 0;
        for (int index = 0; index < 8; ++index)
        {
            auto* processor = engine.getProcessorForPedal(Nova::ChainID::LineA, index);
            if (processor == nullptr)
                break;

            total += readFallbackBlockCount(processor);
        }

        return total;
    }

    using RtProfileSetupFn = std::function<void(AudioEngine&)>;
    using RtProfileFillFn = std::function<void(juce::AudioBuffer<float>&, int, double)>;

    static RtProfileResult runRtProfileScenario(const juce::String& name,
        double sr,
        int scenarioBlockSize,
        const RtProfileSetupFn& setup,
        const RtProfileFillFn& fill,
        bool warnOnLimiterActivity = true)
    {
        RtProfileResult result;
        result.name = name;
        result.sampleRate = sr;
        result.blockSize = scenarioBlockSize;

        AudioEngine engine;
        engine.prepare(sr, scenarioBlockSize, 2, 2);
        engine.setDiagnosticsMode(AudioEngine::DiagnosticsMode::Full);

        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        params.outputLimiterDb = 0.0f;
        engine.updateGlobalParams(params);
        engine.setEngineEnabled(true);

        setup(engine);
        engine.synchronizeProcessingState();

        juce::AudioBuffer<float> buffer(2, scenarioBlockSize);
        juce::MidiBuffer midi;

        for (int block = 0; block < 32; ++block)
        {
            buffer.clear();
            fill(buffer, block, sr);
            engine.process(buffer, midi);
        }

        const int blocks = juce::jmax(96, (int)((sr * 0.45) / (double)scenarioBlockSize));
        const double blockBudgetMs = ((double)scenarioBlockSize / sr) * 1000.0;
        double totalMs = 0.0;

        for (int block = 0; block < blocks; ++block)
        {
            buffer.clear();
            fill(buffer, block + 32, sr);

            const double startMs = juce::Time::getMillisecondCounterHiRes();
            engine.process(buffer, midi);
            const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;

            const double budgetRatio = blockBudgetMs > 0.0 ? elapsedMs / blockBudgetMs : 0.0;
            totalMs += elapsedMs;
            result.peakProcessMs = juce::jmax(result.peakProcessMs, elapsedMs);
            result.maxBudgetRatio = juce::jmax(result.maxBudgetRatio, budgetRatio);

            if (budgetRatio > 0.50) ++result.blocksOver50;
            if (budgetRatio > 0.75) ++result.blocksOver75;
            if (budgetRatio > 0.90) ++result.blocksOver90;
            if (budgetRatio > 1.00) ++result.blocksOver100;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* data = buffer.getReadPointer(ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float v = data[i];
                    const float absV = std::abs(v);

                    if (!std::isfinite(v))
                        ++result.invalidSamples;
                    if (absV >= Nova::Config::HARD_ABS_LIMIT_LINEAR)
                        ++result.clippedSamples;
                    if (absV >= Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD)
                        ++result.nearClipSamples;
                    if (absV > 0.0f && absV < 1.0e-30f)
                        ++result.denormalLikeSamples;

                    result.outputPeak = juce::jmax(result.outputPeak, absV);
                }
            }

            result.inputPeak = juce::jmax(result.inputPeak, engine.getLastInputPeak());
        }

        result.processedBlocks = blocks;
        result.avgProcessMs = totalMs / (double)blocks;
        result.cpuAvgPercent = blockBudgetMs > 0.0 ? (result.avgProcessMs / blockBudgetMs) * 100.0 : 0.0;
        result.cpuPeakPercent = blockBudgetMs > 0.0 ? (result.peakProcessMs / blockBudgetMs) * 100.0 : 0.0;
        result.fallbackBlockCount = collectLineAFallbackBlockCount(engine);

        const auto limiterSnapshot = engine.getOutputChainDebugSnapshot();
        result.limiterTouchedSamples = limiterSnapshot.limiterTouchedSamples;
        result.limiterMaxReductionDb = limiterSnapshot.limiterMaxReductionDb;

        juce::StringArray warnings;
        bool failed = false;
       #if JUCE_DEBUG
        // Debug profiling is intentionally non-blocking for budget overruns because
        // debugger/scheduler noise can dominate timings.
        const int sustainedOverrunFailThreshold = juce::jmax(blocks + 1, 256);
       #else
        const int sustainedOverrunFailThreshold = juce::jmax(24, blocks / 3);
       #endif

        if (result.invalidSamples > 0)
            failed = true;
        if (result.clippedSamples > 0)
            failed = true;
        // Treat >100% block budget as FAIL only when it is clearly sustained.
        // Debug builds are noisy; isolated spikes are handled as WARN.
        if (result.blocksOver100 > sustainedOverrunFailThreshold)
            failed = true;

        if (result.maxBudgetRatio > 0.75)
            warnings.add("peak budget ratio exceeded 75%");
        if (result.blocksOver90 > 0)
            warnings.add("one or more blocks exceeded 90% budget");
        if (result.blocksOver100 > 0)
            warnings.add("one or more blocks exceeded 100% budget");
        if (result.fallbackBlockCount > 0)
            warnings.add("fallback block count was non-zero");
        if (result.denormalLikeSamples > 0)
            warnings.add("denormal-like samples observed");
        if (warnOnLimiterActivity && result.limiterTouchedSamples > 0)
            warnings.add("limiter activity appeared in a clean/nominal scenario");

        result.warnings = warnings.joinIntoString("; ");
        result.status = failed ? "FAIL" : warnings.isEmpty() ? "PASS" : "WARN";
        return result;
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

    static DelayPedal* getOfflineDelay(AudioEngine& engine)
    {
        return dynamic_cast<DelayPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0));
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
        reverb.swellParam->setValueNotifyingHost(reverb.swellParam->convertTo0to1(0.0f));
        reverb.gateParam->setValueNotifyingHost(reverb.gateParam->convertTo0to1(0.0f));
        reverb.reverseParam->setValueNotifyingHost(reverb.reverseParam->convertTo0to1(0.0f));
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
        reverb.swellParam->setValueNotifyingHost(reverb.swellParam->convertTo0to1(0.0f));
        reverb.gateParam->setValueNotifyingHost(reverb.gateParam->convertTo0to1(0.0f));
        reverb.reverseParam->setValueNotifyingHost(reverb.reverseParam->convertTo0to1(0.0f));
        reverb.freezeParam->setValueNotifyingHost(0.0f);
    }

    static void configureFlagshipDelayAnalog(DelayPedal& delay)
    {
        delay.modeParam->setValueNotifyingHost(normalisedChoiceIndex(delay.modeParam, 0));
        delay.timeParam->setValueNotifyingHost(delay.timeParam->convertTo0to1(390.0f));
        delay.syncParam->setValueNotifyingHost(0.0f);
        delay.syncDivisionParam->setValueNotifyingHost(normalisedChoiceIndex(delay.syncDivisionParam, 7));
        delay.feedbackParam->setValueNotifyingHost(delay.feedbackParam->convertTo0to1(0.70f));
        delay.toneParam->setValueNotifyingHost(delay.toneParam->convertTo0to1(5200.0f));
        delay.lowCutParam->setValueNotifyingHost(delay.lowCutParam->convertTo0to1(85.0f));
        delay.spreadParam->setValueNotifyingHost(delay.spreadParam->convertTo0to1(0.62f));
        delay.textureParam->setValueNotifyingHost(delay.textureParam->convertTo0to1(0.66f));
        delay.modDepthParam->setValueNotifyingHost(delay.modDepthParam->convertTo0to1(0.56f));
        delay.modRateParam->setValueNotifyingHost(delay.modRateParam->convertTo0to1(1.25f));
        delay.mixParam->setValueNotifyingHost(delay.mixParam->convertTo0to1(1.0f));
        delay.duckParam->setValueNotifyingHost(delay.duckParam->convertTo0to1(0.0f));
        delay.swellParam->setValueNotifyingHost(delay.swellParam->convertTo0to1(0.0f));
        delay.reverseParam->setValueNotifyingHost(delay.reverseParam->convertTo0to1(0.0f));
        delay.freezeParam->setValueNotifyingHost(0.0f);
    }

    static void configureFlagshipDelayTape(DelayPedal& delay)
    {
        delay.modeParam->setValueNotifyingHost(normalisedChoiceIndex(delay.modeParam, 1));
        delay.timeParam->setValueNotifyingHost(delay.timeParam->convertTo0to1(620.0f));
        delay.syncParam->setValueNotifyingHost(0.0f);
        delay.syncDivisionParam->setValueNotifyingHost(normalisedChoiceIndex(delay.syncDivisionParam, 8));
        delay.feedbackParam->setValueNotifyingHost(delay.feedbackParam->convertTo0to1(0.84f));
        delay.toneParam->setValueNotifyingHost(delay.toneParam->convertTo0to1(4800.0f));
        delay.lowCutParam->setValueNotifyingHost(delay.lowCutParam->convertTo0to1(110.0f));
        delay.spreadParam->setValueNotifyingHost(delay.spreadParam->convertTo0to1(0.76f));
        delay.textureParam->setValueNotifyingHost(delay.textureParam->convertTo0to1(0.82f));
        delay.modDepthParam->setValueNotifyingHost(delay.modDepthParam->convertTo0to1(0.68f));
        delay.modRateParam->setValueNotifyingHost(delay.modRateParam->convertTo0to1(1.55f));
        delay.mixParam->setValueNotifyingHost(delay.mixParam->convertTo0to1(1.0f));
        delay.duckParam->setValueNotifyingHost(delay.duckParam->convertTo0to1(0.0f));
        delay.swellParam->setValueNotifyingHost(delay.swellParam->convertTo0to1(0.0f));
        delay.reverseParam->setValueNotifyingHost(delay.reverseParam->convertTo0to1(0.0f));
        delay.freezeParam->setValueNotifyingHost(0.0f);
    }

    static void configureFlagshipDelayDigital(DelayPedal& delay)
    {
        delay.modeParam->setValueNotifyingHost(normalisedChoiceIndex(delay.modeParam, 2));
        delay.timeParam->setValueNotifyingHost(delay.timeParam->convertTo0to1(340.0f));
        delay.syncParam->setValueNotifyingHost(0.0f);
        delay.syncDivisionParam->setValueNotifyingHost(normalisedChoiceIndex(delay.syncDivisionParam, 4));
        delay.feedbackParam->setValueNotifyingHost(delay.feedbackParam->convertTo0to1(0.68f));
        delay.toneParam->setValueNotifyingHost(delay.toneParam->convertTo0to1(10400.0f));
        delay.lowCutParam->setValueNotifyingHost(delay.lowCutParam->convertTo0to1(55.0f));
        delay.spreadParam->setValueNotifyingHost(delay.spreadParam->convertTo0to1(0.96f));
        delay.textureParam->setValueNotifyingHost(delay.textureParam->convertTo0to1(0.30f));
        delay.modDepthParam->setValueNotifyingHost(delay.modDepthParam->convertTo0to1(0.22f));
        delay.modRateParam->setValueNotifyingHost(delay.modRateParam->convertTo0to1(1.05f));
        delay.mixParam->setValueNotifyingHost(delay.mixParam->convertTo0to1(1.0f));
        delay.duckParam->setValueNotifyingHost(delay.duckParam->convertTo0to1(0.0f));
        delay.swellParam->setValueNotifyingHost(delay.swellParam->convertTo0to1(0.0f));
        delay.reverseParam->setValueNotifyingHost(delay.reverseParam->convertTo0to1(0.0f));
        delay.freezeParam->setValueNotifyingHost(0.0f);
    }

    static void configureFlagshipDelayReverse(DelayPedal& delay)
    {
        delay.modeParam->setValueNotifyingHost(normalisedChoiceIndex(delay.modeParam, 3));
        delay.timeParam->setValueNotifyingHost(delay.timeParam->convertTo0to1(520.0f));
        delay.syncParam->setValueNotifyingHost(0.0f);
        delay.syncDivisionParam->setValueNotifyingHost(normalisedChoiceIndex(delay.syncDivisionParam, 7));
        delay.feedbackParam->setValueNotifyingHost(delay.feedbackParam->convertTo0to1(0.76f));
        delay.toneParam->setValueNotifyingHost(delay.toneParam->convertTo0to1(5600.0f));
        delay.lowCutParam->setValueNotifyingHost(delay.lowCutParam->convertTo0to1(95.0f));
        delay.spreadParam->setValueNotifyingHost(delay.spreadParam->convertTo0to1(0.88f));
        delay.textureParam->setValueNotifyingHost(delay.textureParam->convertTo0to1(0.80f));
        delay.modDepthParam->setValueNotifyingHost(delay.modDepthParam->convertTo0to1(0.58f));
        delay.modRateParam->setValueNotifyingHost(delay.modRateParam->convertTo0to1(1.35f));
        delay.mixParam->setValueNotifyingHost(delay.mixParam->convertTo0to1(1.0f));
        delay.duckParam->setValueNotifyingHost(delay.duckParam->convertTo0to1(0.0f));
        delay.swellParam->setValueNotifyingHost(delay.swellParam->convertTo0to1(0.0f));
        delay.reverseParam->setValueNotifyingHost(delay.reverseParam->convertTo0to1(0.74f));
        delay.freezeParam->setValueNotifyingHost(0.0f);
    }

    static void configureFlagshipCompressor(CompressorPedal& pedal)
    {
        pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-18.5f));
        pedal.ratioParam->setValueNotifyingHost(pedal.ratioParam->convertTo0to1(6.4f));
        pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(7.5f));
        pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(165.0f));
        pedal.kneeParam->setValueNotifyingHost(pedal.kneeParam->convertTo0to1(8.2f));
        pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.68f));
        pedal.blendParam->setValueNotifyingHost(pedal.blendParam->convertTo0to1(0.91f));
        pedal.makeupParam->setValueNotifyingHost(pedal.makeupParam->convertTo0to1(4.4f));
    }

    static void configureFlagshipNoiseGate(NoiseGatePedal& pedal)
    {
        pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-42.5f));
        pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(0.18f));
        pedal.holdParam->setValueNotifyingHost(pedal.holdParam->convertTo0to1(96.0f));
        pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(180.0f));
        pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(-72.0f));
        pedal.hysteresisParam->setValueNotifyingHost(pedal.hysteresisParam->convertTo0to1(9.4f));
        pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.74f));
    }

    static void configureFlagshipEQ(EQPedal& pedal)
    {
        pedal.lowCutParam->setValueNotifyingHost(pedal.lowCutParam->convertTo0to1(85.0f));
        pedal.lowParam->setValueNotifyingHost(pedal.lowParam->convertTo0to1(3.5f));
        pedal.lowMidParam->setValueNotifyingHost(pedal.lowMidParam->convertTo0to1(-4.2f));
        pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(5.8f));
        pedal.midFreqParam->setValueNotifyingHost(pedal.midFreqParam->convertTo0to1(1450.0f));
        pedal.midQParam->setValueNotifyingHost(pedal.midQParam->convertTo0to1(1.6f));
        pedal.presenceParam->setValueNotifyingHost(pedal.presenceParam->convertTo0to1(2.2f));
        pedal.highParam->setValueNotifyingHost(pedal.highParam->convertTo0to1(-1.5f));
        pedal.highCutParam->setValueNotifyingHost(pedal.highCutParam->convertTo0to1(7200.0f));
        pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.25f));
    }

    static void configureFlagshipBoost(BoostPedal& pedal)
    {
        pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(13.5f));
        pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.72f));
        pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.48f));
        pedal.charParam->setValueNotifyingHost(pedal.charParam->convertTo0to1(0.64f));
        pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(2.4f));
        pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.28f));
    }

    static void configureFlagshipNeural(NeuralPedal& pedal)
    {
        pedal.driveParam->setValueNotifyingHost(pedal.driveParam->convertTo0to1(73.0f));
        pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.68f));
        pedal.detailParam->setValueNotifyingHost(pedal.detailParam->convertTo0to1(0.62f));
        pedal.compParam->setValueNotifyingHost(pedal.compParam->convertTo0to1(0.48f));
        pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.86f));
        pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.75f));
    }

    static void configureFlagshipOverdrive(OverdrivePedal& pedal)
    {
        pedal.getDriveParam()->setValueNotifyingHost(pedal.getDriveParam()->convertTo0to1(66.0f));
        pedal.getToneParam()->setValueNotifyingHost(pedal.getToneParam()->convertTo0to1(0.71f));
        pedal.getTextureParam()->setValueNotifyingHost(pedal.getTextureParam()->convertTo0to1(0.63f));
        pedal.getMixParam()->setValueNotifyingHost(pedal.getMixParam()->convertTo0to1(0.84f));
        pedal.getLevelParam()->setValueNotifyingHost(pedal.getLevelParam()->convertTo0to1(0.79f));
    }

    static void configureFlagshipDistortion(DistortionPedal& pedal)
    {
        pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 4));
        pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(73.0f));
        pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.66f));
        pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.63f));
        pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.88f));
        pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.55f));
        pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.81f));
    }

    static void configureFlagshipFuzz(FuzzPedal& pedal)
    {
        pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
        pedal.fuzzParam->setValueNotifyingHost(pedal.fuzzParam->convertTo0to1(81.0f));
        pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.41f));
        pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(0.63f));
        pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.89f));
        pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.54f));
        pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.28f));
    }

    static void configureFlagshipWah(ClassicWahPedal& pedal)
    {
        pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
        pedal.sweepParam->setValueNotifyingHost(pedal.sweepParam->convertTo0to1(0.72f));
        pedal.sensitivityParam->setValueNotifyingHost(pedal.sensitivityParam->convertTo0to1(0.63f));
        pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(6.0f));
        pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(280.0f));
        pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(0.81f));
        pedal.resonanceParam->setValueNotifyingHost(pedal.resonanceParam->convertTo0to1(5.4f));
        pedal.voiceParam->setValueNotifyingHost(pedal.voiceParam->convertTo0to1(0.61f));
        pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.88f));
    }

    static void configureFlagshipOctave(OctavePedal& pedal)
    {
        pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(0.86f));
        pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(0.54f));
        pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(0.38f));
        pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.73f));
        pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.24f));
    }

    static void configureFlagshipChorus(ChorusPedal& pedal)
    {
        pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
        pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(1.85f));
        pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.74f));
        pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.88f));
        pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.46f));
        pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.57f));
        pedal.lagParam->setValueNotifyingHost(pedal.lagParam->convertTo0to1(11.6f));
    }

    static void configureFlagshipPhaser(PhaserPedal& pedal)
    {
        pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
        pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(1.8f));
        pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.81f));
        pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.36f));
        pedal.stagesParam->setValueNotifyingHost(pedal.stagesParam->convertTo0to1(8.0f));
        pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.74f));
    }

    static void configureFlagshipFlanger(FlangerPedal& pedal)
    {
        pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
        pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.91f));
        pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.83f));
        pedal.manualParam->setValueNotifyingHost(pedal.manualParam->convertTo0to1(0.58f));
        pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(-0.36f));
        pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.79f));
        pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(6800.0f));
        pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.53f));
    }

    static void configureFlagshipTremolo(TremoloPedal& pedal)
    {
        pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(6.8f));
        pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.84f));
        pedal.shapeParam->setValueNotifyingHost(pedal.shapeParam->convertTo0to1(0.78f));
        pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.68f));
        pedal.stereoParam->setValueNotifyingHost(pedal.stereoParam->convertTo0to1(0.72f));
        pedal.harmonicParam->setValueNotifyingHost(pedal.harmonicParam->convertTo0to1(0.57f));
        pedal.crossoverParam->setValueNotifyingHost(pedal.crossoverParam->convertTo0to1(1325.0f));
        pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.64f));
        pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.22f));
    }

    template <typename Callback>
    static juce::AudioBuffer<float> renderDelayPedalWithAutomation(DelayPedal& pedal,
        const juce::AudioBuffer<float>& input,
        Callback&& callback)
    {
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> rendered(2, input.getNumSamples());
        rendered.clear();
        juce::AudioBuffer<float> block(2, blockSize);

        for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
        {
            const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
            block.clear();
            for (int ch = 0; ch < 2; ++ch)
                block.copyFrom(ch, 0, input, ch, offset, numSamples);

            callback(offset);
            pedal.processBlock(block, midi);

            rendered.copyFrom(0, offset, block, 0, 0, numSamples);
            rendered.copyFrom(1, offset, block, 1, 0, numSamples);
        }

        return rendered;
    }

    template <typename Pedal, typename Callback>
    static juce::AudioBuffer<float> renderPedalOutput(Pedal& pedal,
        const juce::AudioBuffer<float>& input,
        Callback&& callback)
    {
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> rendered(input.getNumChannels(), input.getNumSamples());
        rendered.clear();
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);

        for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
        {
            const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
            block.clear();
            for (int ch = 0; ch < input.getNumChannels(); ++ch)
                block.copyFrom(ch, 0, input, ch, offset, numSamples);

            callback(offset);
            pedal.processBlock(block, midi);

            for (int ch = 0; ch < rendered.getNumChannels(); ++ch)
                rendered.copyFrom(ch, offset, block, ch, 0, numSamples);
        }

        return rendered;
    }

    template <typename Pedal>
    static juce::AudioBuffer<float> renderPedalOutput(Pedal& pedal, const juce::AudioBuffer<float>& input)
    {
        return renderPedalOutput(pedal, input, [](int) {});
    }

    template <typename Pedal, typename ConfigureFn>
    static OfflineQAScenarioResult runPedalStateRecallScenario(const juce::String& name,
        const juce::String& notes,
        ConfigureFn&& configure,
        const juce::AudioBuffer<float>& input,
        double nullThreshold)
    {
        OfflineQAScenarioResult result;
        result.name = name;

        Pedal source;
        source.prepareToPlay(sampleRate, blockSize);
        configure(source);
        source.reset();

        const auto baseline = renderPedalOutput(source, input);

        juce::MemoryBlock state;
        source.getStateInformation(state);

        Pedal restored;
        restored.prepareToPlay(sampleRate, blockSize);
        restored.setStateInformation(state.getData(), (int) state.getSize());
        restored.reset();

        const auto recalled = renderPedalOutput(restored, input);
        const double nullRms = computeNullRms(baseline, recalled);
        const bool finite = bufferHasOnlyFiniteSamples(recalled);
        const auto baselineMetrics = analyseBuffer(baseline);
        const auto recalledMetrics = analyseBuffer(recalled);

        result.metrics.push_back({ "state_size_bytes", (double) state.getSize() });
        result.metrics.push_back({ "null_rms", nullRms });
        result.metrics.push_back({ "baseline_peak", baselineMetrics.peak });
        result.metrics.push_back({ "recalled_peak", recalledMetrics.peak });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.passed = finite && nullRms <= nullThreshold;
        result.notes = result.passed
            ? notes
            : notes + " | recall drift exceeded threshold";
        return result;
    }

    template <typename Pedal, typename InitialiseFn, typename AutomateFn, typename FillFn>
    static OfflineQAScenarioResult runPedalAutomationStressScenario(const juce::String& name,
        const juce::String& notes,
        InitialiseFn&& initialise,
        AutomateFn&& automate,
        FillFn&& fillBlock,
        double peakLimit,
        int iterations = 120)
    {
        OfflineQAScenarioResult result;
        result.name = name;

        Pedal pedal;
        pedal.prepareToPlay(sampleRate, blockSize);
        initialise(pedal);

        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, blockSize);
        bool finite = true;
        double peak = 0.0;

        for (int iteration = 0; iteration < iterations; ++iteration)
        {
            const float t = iterations > 1 ? (float) iteration / (float) (iterations - 1) : 0.0f;
            automate(pedal, t);
            fillBlock(block, iteration, t);
            pedal.processBlock(block, midi);

            finite = finite && bufferHasOnlyFiniteSamples(block);
            for (int ch = 0; ch < block.getNumChannels(); ++ch)
                peak = juce::jmax(peak, (double) block.getMagnitude(ch, 0, block.getNumSamples()));
        }

        result.metrics.push_back({ "peak", peak });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.metrics.push_back({ "iterations", (double) iterations });
        result.passed = finite && peak < peakLimit;
        result.notes = result.passed
            ? notes
            : notes + " | automation escaped the expected ceiling";
        return result;
    }

    static std::vector<OfflineQAScenarioResult> runAllScenarios()
    {
        std::vector<OfflineQAScenarioResult> results;
        results.push_back(runImpulseTransparencyScenario());
        results.push_back(runOutputChainBiasedDcCleanupScenario());
        results.push_back(runDryOnlyNullScenario());
        results.push_back(runDisabledEngineNullScenario());
        results.push_back(runParallelNoiseUnityScenario());
        results.push_back(runEngineReenableRecoveryScenario());
        results.push_back(runCompressorFlagshipRecallScenario());
        results.push_back(runCompressorGainReductionScenario());
        results.push_back(runCompressorAutomationStressScenario());
        results.push_back(runNoiseGateFlagshipRecallScenario());
        results.push_back(runNoiseGateClampScenario());
        results.push_back(runNoiseGateAutomationStressScenario());
        results.push_back(runEQFlagshipRecallScenario());
        results.push_back(runEQSurgicalResponseScenario());
        results.push_back(runEQAutomationStressScenario());
        results.push_back(runBoostFlagshipRecallScenario());
        results.push_back(runBoostTighteningScenario());
        results.push_back(runBoostAutomationStressScenario());
        results.push_back(runNeuralFlagshipRecallScenario());
        results.push_back(runNeuralFocusScenario());
        results.push_back(runNeuralAutomationStressScenario());
        results.push_back(runOverdriveFlagshipRecallScenario());
        results.push_back(runOverdriveVoiceScenario());
        results.push_back(runOverdriveCleanAmpReverbChainNominalScenario());
        results.push_back(runOverdriveAutomationStressScenario());
        results.push_back(runDistortionFlagshipRecallScenario());
        results.push_back(runDistortionModeDistinctnessScenario());
        results.push_back(runDistortionAutomationStressScenario());
        results.push_back(runFuzzFlagshipRecallScenario());
        results.push_back(runFuzzModeDistinctnessScenario());
        results.push_back(runFuzzAutomationStressScenario());
        results.push_back(runWahFlagshipRecallScenario());
        results.push_back(runWahDynamicSweepScenario());
        results.push_back(runWahAutomationStressScenario());
        results.push_back(runOctaveFlagshipRecallScenario());
        results.push_back(runOctaveVoiceTrackingScenario());
        results.push_back(runOctaveAutomationStressScenario());
        results.push_back(runChorusFlagshipRecallScenario());
        results.push_back(runChorusModeDistinctnessScenario());
        results.push_back(runChorusAutomationStressScenario());
        results.push_back(runPhaserFlagshipRecallScenario());
        results.push_back(runPhaserVoiceScenario());
        results.push_back(runPhaserModeDistinctnessScenario());
        results.push_back(runPhaserAutomationStressScenario());
        results.push_back(runFlangerFlagshipRecallScenario());
        results.push_back(runFlangerModeDistinctnessScenario());
        results.push_back(runFlangerAutomationStressScenario());
        results.push_back(runTremoloFlagshipRecallScenario());
        results.push_back(runTremoloHarmonicScenario());
        results.push_back(runTremoloAutomationStressScenario());
        results.push_back(runReverbTailScenario());
        results.push_back(runReverbStereoFieldScenario());
        results.push_back(runReverbModeDistinctnessScenario());
        results.push_back(runReverbFreezeScenario());
        results.push_back(runReverbDuckingScenario());
        results.push_back(runReverbSwellScenario());
        results.push_back(runReverbGateScenario());
        results.push_back(runReverbReverseScenario());
        results.push_back(runReverbReverseSwellScenario());
        results.push_back(runReverbFreezeReverseScenario());
        results.push_back(runReverbAutomationStressScenario());
        results.push_back(runDelayTailScenario());
        results.push_back(runDelayStereoFieldScenario());
        results.push_back(runDelayModeDistinctnessScenario());
        results.push_back(runDelayFlagshipRecallScenario());
        results.push_back(runDelaySyncTimingScenario());
        results.push_back(runDelayModulationRangeScenario());
        results.push_back(runDelayFreezeScenario());
        results.push_back(runDelayDuckingScenario());
        results.push_back(runDelayReverseScenario());
        results.push_back(runDelaySwellScenario());
        results.push_back(runDelayReverseSwellScenario());
        results.push_back(runDelayAutomationStressScenario());
        results.push_back(runGraphDiagnosticsScenario());
        return results;
    }

    static std::vector<RtProfileResult> runRtProfileScenarios()
    {
        std::vector<RtProfileResult> results;
        results.reserve(16);

        const auto emptyFill = [](juce::AudioBuffer<float>&, int, double) {};
        const auto guitarFill = [](juce::AudioBuffer<float>& buffer, int block, double sr)
        {
            fillProfileGuitarBlock(buffer, block, sr, 0.22f);
        };
        const auto quietNoiseFill = [](juce::AudioBuffer<float>& buffer, int block, double)
        {
            fillQuietNoiseBlock(buffer, block, 0.16f);
        };

        const auto defaultSetup = [](AudioEngine&) {};
        const auto dualSetup = [](AudioEngine& engine)
        {
            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::Dual_Parallel;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
        };
        const auto overdriveSetup = [](AudioEngine& engine)
        {
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "profile-overdrive");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, 4);
            if (auto* overdrive = dynamic_cast<OverdrivePedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0)))
                configureFlagshipOverdrive(*overdrive);
        };
        const auto fullNominalSetup = [](AudioEngine& engine)
        {
            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            params.outputVolumeDb = -1.0f;
            params.outputLimiterDb = -4.0f;
            engine.updateGlobalParams(params);

            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "profile-overdrive");
            engine.addPedal("Clean Amp", Nova::ChainID::LineA, 1, Nova::ZoneID::Amp, "profile-clean-amp");
            engine.addPedal("Reverb", Nova::ChainID::LineA, 2, Nova::ZoneID::FX, "profile-reverb");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, 8);

            if (auto* overdrive = dynamic_cast<OverdrivePedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0)))
                configureFlagshipOverdrive(*overdrive);

            if (auto* reverb = dynamic_cast<ReverbPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 2)))
            {
                configureFlagshipCloud(*reverb);
                reverb->mixParam->setValueNotifyingHost(reverb->mixParam->convertTo0to1(0.52f));
                reverb->decayParam->setValueNotifyingHost(reverb->decayParam->convertTo0to1(0.72f));
            }
        };
        const auto highGainSetup = [](AudioEngine& engine)
        {
            engine.addPedal("High Gain Amp", Nova::ChainID::LineA, 0, Nova::ZoneID::Amp, "profile-high-gain");
        };
        const auto cabinetSetup = [](AudioEngine& engine)
        {
            engine.addPedal("Cabinet", Nova::ChainID::LineA, 0, Nova::ZoneID::Cabinet, "profile-cabinet");
        };
        const auto delaySetup = [](AudioEngine& engine)
        {
            engine.addPedal("Delay", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "profile-delay");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, 4);
            if (auto* delay = dynamic_cast<DelayPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0)))
            {
                configureFlagshipDelayAnalog(*delay);
                delay->mixParam->setValueNotifyingHost(delay->mixParam->convertTo0to1(0.48f));
                delay->feedbackParam->setValueNotifyingHost(delay->feedbackParam->convertTo0to1(0.58f));
            }
        };
        const auto reverbCloudSetup = [](AudioEngine& engine)
        {
            engine.addPedal("Reverb", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "profile-reverb-cloud");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, 4);
            if (auto* reverb = dynamic_cast<ReverbPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0)))
                configureFlagshipCloud(*reverb);
        };
        const auto reverbReverseSwellSetup = [](AudioEngine& engine)
        {
            engine.addPedal("Reverb", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "profile-reverb-reverse-swell");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, 4);
            if (auto* reverb = dynamic_cast<ReverbPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0)))
            {
                configureFlagshipCloud(*reverb);
                reverb->reverseParam->setValueNotifyingHost(reverb->reverseParam->convertTo0to1(0.82f));
                reverb->swellParam->setValueNotifyingHost(reverb->swellParam->convertTo0to1(0.84f));
            }
        };

        results.push_back(runRtProfileScenario("clean_empty_chain", 48000.0, 128, defaultSetup, emptyFill));
        results.push_back(runRtProfileScenario("clean_line_a", 48000.0, 128, defaultSetup, guitarFill));
        results.push_back(runRtProfileScenario("dual_parallel_clean", 48000.0, 128, dualSetup, quietNoiseFill));
        results.push_back(runRtProfileScenario("overdrive_v2_nominal", 48000.0, 128, overdriveSetup, guitarFill));
        results.push_back(runRtProfileScenario("overdrive_cleanamp_reverb_chain_nominal", 48000.0, 128, fullNominalSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("high_gain_amp_nominal", 48000.0, 128, highGainSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("cabinet_nominal", 48000.0, 128, cabinetSetup, guitarFill));
        results.push_back(runRtProfileScenario("delay_feedback_nominal", 48000.0, 128, delaySetup, guitarFill, false));
        results.push_back(runRtProfileScenario("reverb_cloud_tail", 48000.0, 128, reverbCloudSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("reverb_reverse_swell", 48000.0, 128, reverbReverseSwellSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("stress_block_32", 48000.0, 32, fullNominalSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("stress_block_64", 48000.0, 64, fullNominalSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("stress_block_512", 48000.0, 512, fullNominalSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("sample_rate_44100", 44100.0, 128, fullNominalSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("sample_rate_48000", 48000.0, 128, fullNominalSetup, guitarFill, false));
        results.push_back(runRtProfileScenario("sample_rate_96000", 96000.0, 128, fullNominalSetup, guitarFill, false));

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

    static OfflineQAScenarioResult runOutputChainBiasedDcCleanupScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "output_chain_biased_dc_cleanup";

        OutputChainProcessor outputChain;
        outputChain.prepareToPlay(sampleRate, blockSize);
        outputChain.setParams(0.0f, -6.0f);
        outputChain.reset();

        const auto input = generateBiasedSine((int)(sampleRate * 1.20), 997.0, 0.33f, 0.18f);
        const auto inputMetrics = analyseBuffer(input);
        const double inputDc = computeDcAbs(input, (int)(sampleRate * 0.10), (int)(sampleRate * 0.95));

        juce::MidiBuffer midi;
        std::vector<juce::AudioBuffer<float>> blocks;

        for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
        {
            const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
            juce::AudioBuffer<float> block(2, blockSize);
            block.clear();

            for (int ch = 0; ch < 2; ++ch)
                block.copyFrom(ch, 0, input, ch, offset, numSamples);

            outputChain.processBlock(block, midi);

            juce::AudioBuffer<float> trimmed(2, numSamples);
            trimmed.copyFrom(0, 0, block, 0, 0, numSamples);
            trimmed.copyFrom(1, 0, block, 1, 0, numSamples);
            blocks.push_back(std::move(trimmed));
        }

        const auto output = concatenateBlocks(blocks);
        const auto outputMetrics = analyseBuffer(output);
        const double outputDc = computeDcAbs(output, (int)(sampleRate * 0.10), (int)(sampleRate * 0.95));
        const double dcReductionRatio = outputDc / juce::jmax(1.0e-9, inputDc);
        const bool finite = bufferHasOnlyFiniteSamples(output);
        const auto limiterSnapshot = outputChain.getDebugSnapshot();

        const double outputEarlyRms = computeWindowRms(output, (int)(sampleRate * 0.16), (int)(sampleRate * 0.22));
        const double outputLateRms = computeWindowRms(output, (int)(sampleRate * 0.74), (int)(sampleRate * 0.22));
        const double rmsStabilityRatio = outputLateRms / juce::jmax(1.0e-9, outputEarlyRms);

        result.metrics.push_back({ "input_peak", inputMetrics.peak });
        result.metrics.push_back({ "input_dc", inputDc });
        result.metrics.push_back({ "output_peak", outputMetrics.peak });
        result.metrics.push_back({ "output_dc", outputDc });
        result.metrics.push_back({ "dc_reduction_ratio", dcReductionRatio });
        result.metrics.push_back({ "output_rms", outputMetrics.rms });
        result.metrics.push_back({ "output_early_rms", outputEarlyRms });
        result.metrics.push_back({ "output_late_rms", outputLateRms });
        result.metrics.push_back({ "rms_stability_ratio", rmsStabilityRatio });
        result.metrics.push_back({ "limiterTouchedSamples", (double) limiterSnapshot.limiterTouchedSamples });
        result.metrics.push_back({ "limiterMaxReductionDb", limiterSnapshot.limiterMaxReductionDb });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && inputDc > 0.08
            && outputDc < inputDc * 0.35
            && outputMetrics.peak > 0.15
            && outputMetrics.peak < 1.02
            && outputMetrics.rms > 0.08
            && rmsStabilityRatio > 0.70
            && rmsStabilityRatio < 1.30;

        result.notes = result.passed
            ? "OutputChain reduced sustained DC contamination while keeping stable audible output"
            : "OutputChain DC cleanup or stability check fell outside the expected safety band";
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

        result.passed = nullRms < 2.0e-4;
        result.notes = result.passed ? "Dry-only path ignored wet-path gain staging and stayed transparent"
                                     : "Dry-only path leaked wet-path gain changes into the output";
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

    static OfflineQAScenarioResult runCompressorFlagshipRecallScenario()
    {
        return runPedalStateRecallScenario<CompressorPedal>("compressor_flagship_recall",
            "Studio compressor state recalled cleanly",
            [](CompressorPedal& pedal) { configureFlagshipCompressor(pedal); },
            generateNoise((int)(sampleRate * 1.1), 0xC011Au, 0.18f),
            1.0e-6);
    }

    static OfflineQAScenarioResult runCompressorGainReductionScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "compressor_gain_reduction";

        CompressorPedal pedal;
        pedal.prepareToPlay(sampleRate, blockSize);
        pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-30.0f));
        pedal.ratioParam->setValueNotifyingHost(pedal.ratioParam->convertTo0to1(8.0f));
        pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(5.0f));
        pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(145.0f));
        pedal.kneeParam->setValueNotifyingHost(pedal.kneeParam->convertTo0to1(7.5f));
        pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.6f));
        pedal.blendParam->setValueNotifyingHost(pedal.blendParam->convertTo0to1(1.0f));
        pedal.makeupParam->setValueNotifyingHost(pedal.makeupParam->convertTo0to1(0.0f));

        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float t = (float) i / (float) sampleRate;
            float sample = 0.0f;
            if (i >= (int)(sampleRate * 0.04) && i < (int)(sampleRate * 0.82))
            {
                sample += 0.31f * std::sin(juce::MathConstants<float>::twoPi * 130.0f * t);
                sample += 0.14f * std::sin(juce::MathConstants<float>::twoPi * 910.0f * t);
            }

            input.setSample(0, i, sample);
            input.setSample(1, i, sample);
        }

        const auto output = renderPedalOutput(pedal, input);
        const double inputBody = computeWindowRms(input, (int)(sampleRate * 0.18), (int)(sampleRate * 0.32));
        const double outputBody = computeWindowRms(output, (int)(sampleRate * 0.18), (int)(sampleRate * 0.32));
        const double reductionRatio = outputBody / juce::jmax(1.0e-9, inputBody);
        const bool finite = bufferHasOnlyFiniteSamples(output);

        result.metrics.push_back({ "input_body_rms", inputBody });
        result.metrics.push_back({ "output_body_rms", outputBody });
        result.metrics.push_back({ "reduction_ratio", reductionRatio });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.passed = finite && reductionRatio < 0.72;
        result.notes = result.passed
            ? "Compressor applied decisive gain reduction without instability"
            : "Compressor failed to deliver the expected sustained gain reduction";
        return result;
    }

    static OfflineQAScenarioResult runCompressorAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 2.8) / (double) blockSize));
        return runPedalAutomationStressScenario<CompressorPedal>("compressor_automation_stress",
            "Compressor remained finite under full-range parameter motion",
            [](CompressorPedal&) {},
            [](CompressorPedal& pedal, float phase)
            {
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-40.0f + 26.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                pedal.ratioParam->setValueNotifyingHost(pedal.ratioParam->convertTo0to1(1.2f + 9.8f * std::abs(std::cos(phase * juce::MathConstants<float>::twoPi))));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(1.0f + 65.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.3f))));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(25.0f + 300.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 0.85f))));
                pedal.kneeParam->setValueNotifyingHost(pedal.kneeParam->convertTo0to1(1.0f + 11.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.7f))));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.5f))));
                pedal.blendParam->setValueNotifyingHost(pedal.blendParam->convertTo0to1(std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.1f))));
                pedal.makeupParam->setValueNotifyingHost(pedal.makeupParam->convertTo0to1(12.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 0.95f))));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int i = 0; i < block.getNumSamples(); ++i)
                {
                    const float sampleIndex = (float) (iteration * blockSize + i);
                    block.setSample(0, i,
                        0.18f * std::sin(juce::MathConstants<float>::twoPi * 103.0f * sampleIndex / (float) sampleRate)
                        + 0.04f * std::sin(juce::MathConstants<float>::twoPi * 1560.0f * sampleIndex / (float) sampleRate));
                    block.setSample(1, i,
                        0.14f * std::sin(juce::MathConstants<float>::twoPi * 151.0f * sampleIndex / (float) sampleRate)
                        + 0.035f * std::sin(juce::MathConstants<float>::twoPi * 2140.0f * sampleIndex / (float) sampleRate));
                }
            },
            2.0,
            iterations);
    }

    static OfflineQAScenarioResult runNoiseGateFlagshipRecallScenario()
    {
        return runPedalStateRecallScenario<NoiseGatePedal>("noise_gate_flagship_recall",
            "Studio gate state recalled cleanly",
            [](NoiseGatePedal& pedal) { configureFlagshipNoiseGate(pedal); },
            generateNoise((int)(sampleRate * 1.1), 0x9A7E1u, 0.12f),
            1.0e-6);
    }

    static OfflineQAScenarioResult runNoiseGateClampScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "noise_gate_clamp";

        auto renderGate = [](float rangeDb)
        {
            NoiseGatePedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-48.0f));
            pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(0.12f));
            pedal.holdParam->setValueNotifyingHost(pedal.holdParam->convertTo0to1(88.0f));
            pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(130.0f));
            pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(rangeDb));
            pedal.hysteresisParam->setValueNotifyingHost(pedal.hysteresisParam->convertTo0to1(8.8f));
            pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.58f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.6));
            input.clear();

            const int activeSamples = (int)(sampleRate * 0.26);
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                float sample = 0.0026f * std::sin(juce::MathConstants<float>::twoPi * 58.0f * t)
                    + 0.0007f * std::sin(juce::MathConstants<float>::twoPi * 3400.0f * t);

                if (i < activeSamples)
                {
                    sample += 0.18f * std::sin(juce::MathConstants<float>::twoPi * 123.0f * t);
                    sample += 0.05f * std::sin(juce::MathConstants<float>::twoPi * 246.0f * t);
                }

                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto baseline = renderGate(0.0f);
        const auto gated = renderGate(-96.0f);
        const double baselineBody = computeWindowRms(baseline, (int)(sampleRate * 0.05), (int)(sampleRate * 0.18));
        const double gatedBody = computeWindowRms(gated, (int)(sampleRate * 0.05), (int)(sampleRate * 0.18));
        const double baselineTail = computeWindowRms(baseline, (int)(sampleRate * 0.95), (int)(sampleRate * 0.28));
        const double gatedTail = computeWindowRms(gated, (int)(sampleRate * 0.95), (int)(sampleRate * 0.28));
        const double bodyRatio = gatedBody / juce::jmax(1.0e-9, baselineBody);
        const double tailRatio = gatedTail / juce::jmax(1.0e-9, baselineTail);
        const bool finite = bufferHasOnlyFiniteSamples(gated);

        result.metrics.push_back({ "body_ratio", bodyRatio });
        result.metrics.push_back({ "tail_ratio", tailRatio });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.passed = finite && bodyRatio > 0.74 && tailRatio < 0.18;
        result.notes = result.passed
            ? "Gate preserved the played body while decisively clamping the late tail"
            : "Gate body preservation or late-tail clamping missed the acceptance band";
        return result;
    }

    static OfflineQAScenarioResult runNoiseGateAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 2.8) / (double) blockSize));
        return runPedalAutomationStressScenario<NoiseGatePedal>("noise_gate_automation_stress",
            "Noise gate stayed finite under aggressive detector automation",
            [](NoiseGatePedal&) {},
            [](NoiseGatePedal& pedal, float phase)
            {
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-68.0f + 48.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(0.02f + 9.5f * std::abs(std::cos(phase * juce::MathConstants<float>::twoPi))));
                pedal.holdParam->setValueNotifyingHost(pedal.holdParam->convertTo0to1(180.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 0.75f))));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(20.0f + 420.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.35f))));
                pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(-96.0f + 96.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 0.9f))));
                pedal.hysteresisParam->setValueNotifyingHost(pedal.hysteresisParam->convertTo0to1(1.0f + 17.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.6f))));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.9f))));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int i = 0; i < block.getNumSamples(); ++i)
                {
                    const float sampleIndex = (float) (iteration * blockSize + i);
                    block.setSample(0, i,
                        0.16f * std::sin(juce::MathConstants<float>::twoPi * 98.0f * sampleIndex / (float) sampleRate)
                        + 0.015f * std::sin(juce::MathConstants<float>::twoPi * 2200.0f * sampleIndex / (float) sampleRate));
                    block.setSample(1, i,
                        0.13f * std::sin(juce::MathConstants<float>::twoPi * 147.0f * sampleIndex / (float) sampleRate)
                        + 0.012f * std::sin(juce::MathConstants<float>::twoPi * 2700.0f * sampleIndex / (float) sampleRate));
                }
            },
            1.2,
            iterations);
    }

    static OfflineQAScenarioResult runEQFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 0.95));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float t = (float) i / (float) sampleRate;
            input.setSample(0, i,
                0.18f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                + 0.06f * std::sin(juce::MathConstants<float>::twoPi * 900.0f * t)
                + 0.03f * std::sin(juce::MathConstants<float>::twoPi * 4200.0f * t));
            input.setSample(1, i,
                0.14f * std::sin(juce::MathConstants<float>::twoPi * 164.0f * t)
                + 0.05f * std::sin(juce::MathConstants<float>::twoPi * 1300.0f * t)
                + 0.02f * std::sin(juce::MathConstants<float>::twoPi * 6100.0f * t));
        }

        return runPedalStateRecallScenario<EQPedal>("eq_flagship_recall",
            "Studio EQ state recalled cleanly",
            [](EQPedal& pedal) { configureFlagshipEQ(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runEQSurgicalResponseScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "eq_surgical_response";

        auto renderLowCut = [](float lowCutHz)
        {
            EQPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.lowCutParam->setValueNotifyingHost(pedal.lowCutParam->convertTo0to1(lowCutHz));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.1));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                float sample = 0.0f;
                if (i >= (int)(sampleRate * 0.04) && i < (int)(sampleRate * 0.34))
                    sample += 0.17f * std::sin(juce::MathConstants<float>::twoPi * 45.0f * t);
                if (i >= (int)(sampleRate * 0.42) && i < (int)(sampleRate * 0.96))
                {
                    sample += 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                    sample += 0.05f * std::sin(juce::MathConstants<float>::twoPi * 45.0f * t);
                }

                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        auto renderHighCut = [](float highCutHz)
        {
            EQPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.highCutParam->setValueNotifyingHost(pedal.highCutParam->convertTo0to1(highCutHz));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                float sample = 0.0f;
                if (i >= (int)(sampleRate * 0.05) && i < (int)(sampleRate * 0.30))
                    sample += 0.14f * std::sin(juce::MathConstants<float>::twoPi * 7000.0f * t);
                if (i >= (int)(sampleRate * 0.38) && i < (int)(sampleRate * 0.88))
                {
                    sample += 0.18f * std::sin(juce::MathConstants<float>::twoPi * 240.0f * t);
                    sample += 0.06f * std::sin(juce::MathConstants<float>::twoPi * 7000.0f * t);
                }

                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        auto renderMidFocus = [](float midFreqHz)
        {
            EQPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(10.0f));
            pedal.midFreqParam->setValueNotifyingHost(pedal.midFreqParam->convertTo0to1(midFreqHz));
            pedal.midQParam->setValueNotifyingHost(pedal.midQParam->convertTo0to1(1.7f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 0.8));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                const float sample = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 850.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto openLow = renderLowCut(20.0f);
        const auto cleanedLow = renderLowCut(140.0f);
        const auto openHigh = renderHighCut(20000.0f);
        const auto softenedHigh = renderHighCut(4200.0f);
        const auto matchedMid = renderMidFocus(850.0f);
        const auto detunedMid = renderMidFocus(2500.0f);

        const double rumbleRatio = computeWindowRms(cleanedLow, (int)(sampleRate * 0.10), (int)(sampleRate * 0.16))
            / juce::jmax(1.0e-9, computeWindowRms(openLow, (int)(sampleRate * 0.10), (int)(sampleRate * 0.16)));
        const double lowBodyRatio = computeWindowRms(cleanedLow, (int)(sampleRate * 0.54), (int)(sampleRate * 0.22))
            / juce::jmax(1.0e-9, computeWindowRms(openLow, (int)(sampleRate * 0.54), (int)(sampleRate * 0.22)));
        const double fizzRatio = computeWindowRms(softenedHigh, (int)(sampleRate * 0.10), (int)(sampleRate * 0.14))
            / juce::jmax(1.0e-9, computeWindowRms(openHigh, (int)(sampleRate * 0.10), (int)(sampleRate * 0.14)));
        const double highBodyRatio = computeWindowRms(softenedHigh, (int)(sampleRate * 0.52), (int)(sampleRate * 0.20))
            / juce::jmax(1.0e-9, computeWindowRms(openHigh, (int)(sampleRate * 0.52), (int)(sampleRate * 0.20)));
        const double midFocusRatio = computeWindowRms(matchedMid, (int)(sampleRate * 0.18), (int)(sampleRate * 0.24))
            / juce::jmax(1.0e-9, computeWindowRms(detunedMid, (int)(sampleRate * 0.18), (int)(sampleRate * 0.24)));

        result.metrics.push_back({ "rumble_ratio", rumbleRatio });
        result.metrics.push_back({ "low_body_ratio", lowBodyRatio });
        result.metrics.push_back({ "fizz_ratio", fizzRatio });
        result.metrics.push_back({ "high_body_ratio", highBodyRatio });
        result.metrics.push_back({ "mid_focus_ratio", midFocusRatio });
        result.passed = rumbleRatio < 0.45
            && lowBodyRatio > 0.72
            && fizzRatio < 0.38
            && highBodyRatio > 0.75
            && midFocusRatio > 1.8;
        result.notes = result.passed
            ? "EQ cut and mid-focus controls hit the expected surgical targets"
            : "EQ response missed one or more surgical acceptance targets";
        return result;
    }

    static OfflineQAScenarioResult runEQAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 2.8) / (double) blockSize));
        return runPedalAutomationStressScenario<EQPedal>("eq_automation_stress",
            "EQ remained finite under aggressive full-band automation",
            [](EQPedal&) {},
            [](EQPedal& pedal, float phase)
            {
                pedal.lowCutParam->setValueNotifyingHost(pedal.lowCutParam->convertTo0to1(20.0f + 220.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                pedal.lowParam->setValueNotifyingHost(pedal.lowParam->convertTo0to1(-10.0f + 20.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 0.8f))));
                pedal.lowMidParam->setValueNotifyingHost(pedal.lowMidParam->convertTo0to1(-12.0f + 24.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.2f))));
                pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(-12.0f + 24.0f * std::abs(std::cos(phase * juce::MathConstants<float>::twoPi))));
                pedal.midFreqParam->setValueNotifyingHost(pedal.midFreqParam->convertTo0to1(250.0f + 4200.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.35f))));
                pedal.midQParam->setValueNotifyingHost(pedal.midQParam->convertTo0to1(0.35f + 2.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.5f))));
                pedal.presenceParam->setValueNotifyingHost(pedal.presenceParam->convertTo0to1(-12.0f + 24.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.6f))));
                pedal.highParam->setValueNotifyingHost(pedal.highParam->convertTo0to1(-12.0f + 24.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.1f))));
                pedal.highCutParam->setValueNotifyingHost(pedal.highCutParam->convertTo0to1(3200.0f + 16800.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 0.9f))));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.2f + 1.6f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.4f))));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int i = 0; i < block.getNumSamples(); ++i)
                {
                    const float sampleIndex = (float) (iteration * blockSize + i);
                    block.setSample(0, i,
                        0.16f * std::sin(juce::MathConstants<float>::twoPi * 98.0f * sampleIndex / (float) sampleRate)
                        + 0.05f * std::sin(juce::MathConstants<float>::twoPi * 820.0f * sampleIndex / (float) sampleRate)
                        + 0.03f * std::sin(juce::MathConstants<float>::twoPi * 5200.0f * sampleIndex / (float) sampleRate));
                    block.setSample(1, i,
                        0.13f * std::sin(juce::MathConstants<float>::twoPi * 147.0f * sampleIndex / (float) sampleRate)
                        + 0.04f * std::sin(juce::MathConstants<float>::twoPi * 1100.0f * sampleIndex / (float) sampleRate)
                        + 0.025f * std::sin(juce::MathConstants<float>::twoPi * 6400.0f * sampleIndex / (float) sampleRate));
                }
            },
            3.0,
            iterations);
    }

    static OfflineQAScenarioResult runBoostFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
        input.clear();
        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
                input.setSample(ch, i, 0.12f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 196.0 * (double) i / sampleRate) + ch * 0.17f));

        return runPedalStateRecallScenario<BoostPedal>("boost_flagship_recall",
            "Boost flagship state recalled cleanly",
            [](BoostPedal& pedal) { configureFlagshipBoost(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runBoostTighteningScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "boost_tightening";

        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
        input.clear();
        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
                input.setSample(ch, i, 0.18f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 90.0 * (double) i / sampleRate)));

        auto renderTight = [&input](float tightAmount)
        {
            BoostPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(12.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.56f));
            pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(tightAmount));
            pedal.charParam->setValueNotifyingHost(pedal.charParam->convertTo0to1(0.46f));
            pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(0.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.reset();
            return renderPedalOutput(pedal, input);
        };

        const auto loose = renderTight(0.06f);
        const auto tight = renderTight(0.92f);
        const double looseRms = computeWindowRms(loose, (int)(sampleRate * 0.20), (int)(sampleRate * 0.5));
        const double tightRms = computeWindowRms(tight, (int)(sampleRate * 0.20), (int)(sampleRate * 0.5));
        const double ratio = tightRms / juce::jmax(1.0e-9, looseRms);

        result.metrics.push_back({ "loose_rms", looseRms });
        result.metrics.push_back({ "tight_rms", tightRms });
        result.metrics.push_back({ "tight_ratio", ratio });
        result.passed = ratio < 0.35;
        result.notes = result.passed
            ? "Boost tight control materially trimmed low-end energy"
            : "Boost tight control did not cut enough low-end bloom";
        return result;
    }

    static OfflineQAScenarioResult runBoostAutomationStressScenario()
    {
        return runPedalAutomationStressScenario<BoostPedal>("boost_automation_stress",
            "Boost remained finite under aggressive preamp automation",
            [](BoostPedal&) {},
            [](BoostPedal& pedal, float t)
            {
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(juce::jmap(std::sin(t * 5.0f), -1.0f, 1.0f, 0.0f, 22.0f)));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(juce::jmap(std::cos(t * 7.2f), -1.0f, 1.0f, 0.02f, 0.98f)));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(juce::jmap(std::sin(t * 9.1f + 0.6f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.charParam->setValueNotifyingHost(pedal.charParam->convertTo0to1(juce::jmap(std::cos(t * 8.3f + 0.2f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(juce::jmap(std::sin(t * 6.7f + 1.1f), -1.0f, 1.0f, -6.0f, 6.0f)));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(juce::jmap(std::cos(t * 4.6f), -1.0f, 1.0f, 0.7f, 1.7f)));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * blockSize + i);
                        block.setSample(ch, i, 0.16f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 165.0 * sampleIndex / sampleRate) + ch * 0.21f));
                    }
            },
            2.4,
            120);
    }

    static OfflineQAScenarioResult runNeuralFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float phaseL = juce::MathConstants<float>::twoPi * 123.0f * (float) i / (float) sampleRate;
            const float phaseR = juce::MathConstants<float>::twoPi * 247.0f * (float) i / (float) sampleRate;
            input.setSample(0, i, 0.16f * std::sin(phaseL));
            input.setSample(1, i, 0.13f * std::sin(phaseR));
        }

        return runPedalStateRecallScenario<NeuralPedal>("neural_flagship_recall",
            "Adaptive preamp state recalled cleanly",
            [](NeuralPedal& pedal) { configureFlagshipNeural(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runNeuralFocusScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "neural_focus_tightening";

        auto renderFocus = [](float focusAmount)
        {
            NeuralPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.driveParam->setValueNotifyingHost(pedal.driveParam->convertTo0to1(88.0f));
            pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(focusAmount));
            pedal.detailParam->setValueNotifyingHost(pedal.detailParam->convertTo0to1(0.52f));
            pedal.compParam->setValueNotifyingHost(pedal.compParam->convertTo0to1(0.46f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.75f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.4));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 82.0f * (float) i / (float) sampleRate;
                const float sample = 0.21f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto loose = renderFocus(0.08f);
        const auto tight = renderFocus(0.92f);
        const double looseRms = computeWindowRms(loose, (int)(sampleRate * 0.35), (int)(sampleRate * 0.55));
        const double tightRms = computeWindowRms(tight, (int)(sampleRate * 0.35), (int)(sampleRate * 0.55));
        const double ratio = tightRms / juce::jmax(1.0e-9, looseRms);

        result.metrics.push_back({ "loose_rms", looseRms });
        result.metrics.push_back({ "tight_rms", tightRms });
        result.metrics.push_back({ "tight_ratio", ratio });
        result.passed = ratio < 0.70;
        result.notes = result.passed
            ? "Neural focus materially tightened low-end energy"
            : "Neural focus did not tighten the low end enough";
        return result;
    }

    static OfflineQAScenarioResult runNeuralAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 3.0) / (double) blockSize));
        return runPedalAutomationStressScenario<NeuralPedal>("neural_automation_stress",
            "Adaptive preamp remained finite under aggressive automation",
            [](NeuralPedal& pedal)
            {
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.82f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.75f));
            },
            [](NeuralPedal& pedal, float phase)
            {
                pedal.driveParam->setValueNotifyingHost(pedal.driveParam->convertTo0to1(8.0f + 90.0f * phase));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.06f + 0.92f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.detailParam->setValueNotifyingHost(pedal.detailParam->convertTo0to1(0.08f + 0.88f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.compParam->setValueNotifyingHost(pedal.compParam->convertTo0to1(phase));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.12f + 0.86f * phase));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                juce::Random rng(0xA1145 + iteration);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                        block.setSample(ch, i, 0.17f * ((rng.nextFloat() * 2.0f) - 1.0f));
            },
            2.2,
            iterations);
    }

    static OfflineQAScenarioResult runOverdriveFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
        input.clear();
        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float fundamental = std::sin((float) (2.0 * juce::MathConstants<double>::pi * 185.0 * (double) i / sampleRate));
                const float upper = std::sin((float) (2.0 * juce::MathConstants<double>::pi * 1480.0 * (double) i / sampleRate));
                input.setSample(ch, i, 0.15f * fundamental + 0.09f * upper);
            }

        return runPedalStateRecallScenario<OverdrivePedal>("overdrive_flagship_recall",
            "Modern overdrive state recalled cleanly",
            [](OverdrivePedal& pedal) { configureFlagshipOverdrive(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runOverdriveVoiceScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "overdrive_voice_reshape";

        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
        input.clear();
        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float fundamental = std::sin((float) (2.0 * juce::MathConstants<double>::pi * 185.0 * (double) i / sampleRate));
                const float upper = std::sin((float) (2.0 * juce::MathConstants<double>::pi * 1480.0 * (double) i / sampleRate));
                input.setSample(ch, i, 0.15f * fundamental + 0.09f * upper);
            }

        OverdrivePedal darkSmooth;
        darkSmooth.prepareToPlay(sampleRate, blockSize);
        darkSmooth.getDriveParam()->setValueNotifyingHost(darkSmooth.getDriveParam()->convertTo0to1(62.0f));
        darkSmooth.getToneParam()->setValueNotifyingHost(darkSmooth.getToneParam()->convertTo0to1(0.14f));
        darkSmooth.getTextureParam()->setValueNotifyingHost(darkSmooth.getTextureParam()->convertTo0to1(0.12f));
        darkSmooth.getMixParam()->setValueNotifyingHost(darkSmooth.getMixParam()->convertTo0to1(1.0f));
        darkSmooth.getLevelParam()->setValueNotifyingHost(darkSmooth.getLevelParam()->convertTo0to1(0.75f));
        darkSmooth.reset();
        const auto darkOutput = renderPedalOutput(darkSmooth, input);

        OverdrivePedal brightTextured;
        brightTextured.prepareToPlay(sampleRate, blockSize);
        brightTextured.getDriveParam()->setValueNotifyingHost(brightTextured.getDriveParam()->convertTo0to1(62.0f));
        brightTextured.getToneParam()->setValueNotifyingHost(brightTextured.getToneParam()->convertTo0to1(0.92f));
        brightTextured.getTextureParam()->setValueNotifyingHost(brightTextured.getTextureParam()->convertTo0to1(0.88f));
        brightTextured.getMixParam()->setValueNotifyingHost(brightTextured.getMixParam()->convertTo0to1(1.0f));
        brightTextured.getLevelParam()->setValueNotifyingHost(brightTextured.getLevelParam()->convertTo0to1(0.75f));
        brightTextured.reset();
        const auto brightOutput = renderPedalOutput(brightTextured, input);

        const double darkRms = computeWindowRms(darkOutput, (int)(sampleRate * 0.25), (int)(sampleRate * 0.45));
        const double brightRms = computeWindowRms(brightOutput, (int)(sampleRate * 0.25), (int)(sampleRate * 0.45));
        const double nullRms = computeNullRms(darkOutput, brightOutput);

        result.metrics.push_back({ "dark_rms", darkRms });
        result.metrics.push_back({ "bright_rms", brightRms });
        result.metrics.push_back({ "rms_delta", std::abs(brightRms - darkRms) });
        result.metrics.push_back({ "null_rms", nullRms });
        result.passed = std::abs(brightRms - darkRms) > 0.030 && nullRms > 0.050;
        result.notes = result.passed
            ? "Tone and texture controls materially reshaped the overdrive voice"
            : "Overdrive tone/texture changes collapsed toward the same response";
        return result;
    }

    static OfflineQAScenarioResult runOverdriveCleanAmpReverbChainNominalScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "overdrive_cleanamp_reverb_chain_nominal";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);

        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        params.outputVolumeDb = -1.0f;
        params.outputLimiterDb = -4.0f;
        engine.updateGlobalParams(params);

        engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "offline-overdrive");
        engine.addPedal("Clean Amp", Nova::ChainID::LineA, 1, Nova::ZoneID::Amp, "offline-clean-amp");
        engine.addPedal("Reverb", Nova::ChainID::LineA, 2, Nova::ZoneID::FX, "offline-reverb");
        engine.setEngineEnabled(true);
        warmUpEngine(engine, 16);

        if (auto* overdrive = dynamic_cast<OverdrivePedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0)))
            configureFlagshipOverdrive(*overdrive);

        if (auto* reverb = dynamic_cast<ReverbPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 2)))
        {
            configureFlagshipCloud(*reverb);
            reverb->mixParam->setValueNotifyingHost(reverb->mixParam->convertTo0to1(0.52f));
            reverb->decayParam->setValueNotifyingHost(reverb->decayParam->convertTo0to1(0.72f));
            reverb->predelayParam->setValueNotifyingHost(reverb->predelayParam->convertTo0to1(18.0f));
        }

        warmUpEngine(engine, 10);

        juce::AudioBuffer<float> input(2, (int)(sampleRate * 2.0));
        input.clear();
        const int activeSamples = (int)(sampleRate * 0.95);
        for (int i = 0; i < activeSamples; ++i)
        {
            const float t = (float)i / (float)sampleRate;
            const float pick = (i < (int)(sampleRate * 0.06)) ? (0.08f * std::sin(juce::MathConstants<float>::twoPi * 3100.0f * t)) : 0.0f;
            const float tone = 0.14f * std::sin(juce::MathConstants<float>::twoPi * 146.0f * t)
                + 0.06f * std::sin(juce::MathConstants<float>::twoPi * 292.0f * t)
                + 0.03f * std::sin(juce::MathConstants<float>::twoPi * 1180.0f * t);
            const float sample = tone + pick;
            input.setSample(0, i, sample);
            input.setSample(1, i, sample);
        }

        const auto output = processBuffer(engine, input);
        const auto inputMetrics = analyseBuffer(input);
        const auto outputMetrics = analyseBuffer(output);
        const bool finite = bufferHasOnlyFiniteSamples(output);
        const int nearClipCount = countNearClipSamples(output);
        const double outputDc = computeDcAbs(output, (int)(sampleRate * 0.20), (int)(sampleRate * 1.60));
        const double bodyRms = computeWindowRms(output, (int)(sampleRate * 0.16), (int)(sampleRate * 0.48));
        const double tailRms = computeWindowRms(output, (int)(sampleRate * 1.20), (int)(sampleRate * 0.45));
        const double lateTailRms = computeWindowRms(output, (int)(sampleRate * 1.70), (int)(sampleRate * 0.20));
        const double tailDecayRatio = lateTailRms / juce::jmax(1.0e-9, tailRms);

        result.metrics.push_back({ "input_peak", inputMetrics.peak });
        result.metrics.push_back({ "output_peak", outputMetrics.peak });
        result.metrics.push_back({ "output_rms", outputMetrics.rms });
        result.metrics.push_back({ "output_dc", outputDc });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.metrics.push_back({ "near_clip_count", (double)nearClipCount });
        result.metrics.push_back({ "tail_rms", tailRms });
        result.metrics.push_back({ "body_rms", bodyRms });
        result.metrics.push_back({ "late_tail_rms", lateTailRms });
        result.metrics.push_back({ "tail_decay_ratio", tailDecayRatio });

        result.passed = finite
            && outputMetrics.peak > 0.10
            && outputMetrics.peak < 1.02
            && outputMetrics.rms > 0.03
            && outputDc < 0.03
            && nearClipCount < 96
            && tailRms > 1.0e-4
            && tailRms < bodyRms * 0.90
            && tailDecayRatio < 0.85;

        result.notes = result.passed
            ? "Overdrive->CleanAmp->Reverb chain stayed finite, safe and musically stable with a decaying tail"
            : "Nominal Overdrive->CleanAmp->Reverb chain drifted outside the expected safety/tail envelope";
        return result;
    }

    static OfflineQAScenarioResult runOverdriveAutomationStressScenario()
    {
        return runPedalAutomationStressScenario<OverdrivePedal>("overdrive_automation_stress",
            "Overdrive remained finite under aggressive preamp automation",
            [](OverdrivePedal&) {},
            [](OverdrivePedal& pedal, float t)
            {
                pedal.getDriveParam()->setValueNotifyingHost(pedal.getDriveParam()->convertTo0to1(6.0f + 90.0f * std::abs(std::sin(t * 4.8f))));
                pedal.getToneParam()->setValueNotifyingHost(pedal.getToneParam()->convertTo0to1(juce::jmap(std::sin(t * 9.0f), -1.0f, 1.0f, 0.02f, 0.98f)));
                pedal.getTextureParam()->setValueNotifyingHost(pedal.getTextureParam()->convertTo0to1(juce::jmap(std::cos(t * 7.3f + 0.4f), -1.0f, 1.0f, 0.04f, 0.96f)));
                pedal.getMixParam()->setValueNotifyingHost(pedal.getMixParam()->convertTo0to1(juce::jmap(std::sin(t * 10.4f + 0.8f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.getLevelParam()->setValueNotifyingHost(pedal.getLevelParam()->convertTo0to1(juce::jmap(std::cos(t * 6.1f), -1.0f, 1.0f, 0.45f, 0.88f)));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * blockSize + i);
                        block.setSample(ch, i, 0.22f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 147.0 * sampleIndex / sampleRate) + ch * 0.29f));
                    }
            },
            2.2,
            120);
    }

    static OfflineQAScenarioResult runDistortionFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float fundamental = std::sin(juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) sampleRate);
            input.setSample(0, i, 0.19f * fundamental);
            input.setSample(1, i, 0.19f * fundamental);
        }

        return runPedalStateRecallScenario<DistortionPedal>("distortion_flagship_recall",
            "Unified distortion flagship state recalled cleanly",
            [](DistortionPedal& pedal) { configureFlagshipDistortion(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runDistortionModeDistinctnessScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "distortion_mode_distinctness";

        auto renderMode = [](int modeIndex)
        {
            DistortionPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
            pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(modeIndex >= 3 ? 82.0f : modeIndex == 1 ? 69.0f : 76.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(modeIndex >= 3 ? 0.52f : 0.57f));
            pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(modeIndex >= 3 ? 0.60f : 0.56f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.64f));
            pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(modeIndex == 2 ? 0.82f : modeIndex >= 3 ? 0.74f : 0.45f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.8));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float sample = 0.19f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) sampleRate);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        auto renderMetalTight = [](float tightAmount)
        {
            DistortionPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 3));
            pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(94.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.58f));
            pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.62f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.64f));
            pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(tightAmount));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 2.0));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                float sample = 0.0f;
                if (t < 0.18f)
                {
                    sample = 0.24f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                        + 0.07f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                }
                else
                {
                    sample = 0.0016f * std::sin(juce::MathConstants<float>::twoPi * 73.0f * t)
                        + 0.0009f * std::sin(juce::MathConstants<float>::twoPi * 181.0f * t);
                }

                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto vintage = renderMode(0);
        const auto turbo = renderMode(1);
        const auto amp = renderMode(2);
        const auto metal = renderMode(3);
        const auto studio = renderMode(4);
        const auto looseMetal = renderMetalTight(0.12f);
        const auto tightMetal = renderMetalTight(0.90f);

        const double vintageTurboNull = computeNullRms(vintage, turbo);
        const double vintageAmpNull = computeNullRms(vintage, amp);
        const double turboAmpNull = computeNullRms(turbo, amp);
        const double ampMetalNull = computeNullRms(amp, metal);
        const double metalStudioNull = computeNullRms(metal, studio);
        const double vintageStudioNull = computeNullRms(vintage, studio);
        const double metalGateRatio = computeWindowRms(tightMetal, (int)(sampleRate * 0.85), (int)(sampleRate * 0.65))
            / juce::jmax(1.0e-9, computeWindowRms(looseMetal, (int)(sampleRate * 0.85), (int)(sampleRate * 0.65)));

        result.metrics.push_back({ "vintage_turbo_null", vintageTurboNull });
        result.metrics.push_back({ "vintage_amp_null", vintageAmpNull });
        result.metrics.push_back({ "turbo_amp_null", turboAmpNull });
        result.metrics.push_back({ "amp_metal_null", ampMetalNull });
        result.metrics.push_back({ "metal_studio_null", metalStudioNull });
        result.metrics.push_back({ "vintage_studio_null", vintageStudioNull });
        result.metrics.push_back({ "metal_gate_ratio", metalGateRatio });
        result.passed = vintageTurboNull > 1.5e-3
            && vintageAmpNull > 1.5e-3
            && turboAmpNull > 1.2e-3
            && ampMetalNull > 1.8e-3
            && metalStudioNull > 1.2e-3
            && vintageStudioNull > 1.8e-3
            && metalGateRatio < 0.82;
        result.notes = result.passed
            ? "Distortion modes stayed distinct and metal tightness closed the integrated gate harder"
            : "One or more distortion voices collapsed or the metal gate failed to tighten enough";
        return result;
    }

    static OfflineQAScenarioResult runDistortionAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 3.0) / (double) blockSize));
        return runPedalAutomationStressScenario<DistortionPedal>("distortion_automation_stress",
            "Distortion remained finite under aggressive multi-mode automation",
            [](DistortionPedal& pedal)
            {
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.74f));
            },
            [](DistortionPedal& pedal, float phase)
            {
                const int mode = juce::jlimit(0, 4, (int) std::floor(phase * 5.0f));
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(8.0f + 90.0f * phase));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.10f + 0.85f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.08f + 0.84f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.12f + 0.82f * phase));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.18f + 0.70f * (1.0f - phase)));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.02f + 0.96f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                juce::Random rng(0xD1570 + iteration);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                        block.setSample(ch, i, 0.16f * ((rng.nextFloat() * 2.0f) - 1.0f));
            },
            2.3,
            iterations);
    }

    static OfflineQAScenarioResult runFuzzFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float phaseL = juce::MathConstants<float>::twoPi * 110.0f * (float) i / (float) sampleRate;
            const float phaseR = juce::MathConstants<float>::twoPi * 173.0f * (float) i / (float) sampleRate;
            input.setSample(0, i, 0.18f * std::sin(phaseL));
            input.setSample(1, i, 0.13f * std::sin(phaseR));
        }

        return runPedalStateRecallScenario<FuzzPedal>("fuzz_flagship_recall",
            "Commercial fuzz state recalled cleanly",
            [](FuzzPedal& pedal) { configureFlagshipFuzz(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runFuzzModeDistinctnessScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "fuzz_mode_distinctness";

        auto renderMode = [](int modeIndex)
        {
            FuzzPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
            pedal.fuzzParam->setValueNotifyingHost(pedal.fuzzParam->convertTo0to1(modeIndex == 1 ? 76.0f : 84.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.44f));
            pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(modeIndex == 2 ? 0.62f : 0.24f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.58f));
            pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(modeIndex == 2 ? 0.22f : 0.60f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.7));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float sample = 0.19f * std::sin(juce::MathConstants<float>::twoPi * 146.0f * (float) i / (float) sampleRate);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        auto renderVelcroTail = [](float gateAmount, float biasAmount)
        {
            FuzzPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
            pedal.fuzzParam->setValueNotifyingHost(pedal.fuzzParam->convertTo0to1(88.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.36f));
            pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(gateAmount));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.58f));
            pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(biasAmount));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.3));
            input.clear();
            const int burstSamples = (int)(sampleRate * 0.24);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 98.0f * (float) i / (float) sampleRate;
                const float sample = 0.22f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto vintage = renderMode(0);
        const auto muff = renderMode(1);
        const auto velcro = renderMode(2);
        const auto loose = renderVelcroTail(0.10f, 0.72f);
        const auto clamped = renderVelcroTail(0.88f, 0.12f);

        const double vintageMuffNull = computeNullRms(vintage, muff);
        const double vintageVelcroNull = computeNullRms(vintage, velcro);
        const double muffVelcroNull = computeNullRms(muff, velcro);
        const double velcroTailRatio = computeWindowRms(clamped, (int)(sampleRate * 0.86), (int)(sampleRate * 0.18))
            / juce::jmax(1.0e-9, computeWindowRms(loose, (int)(sampleRate * 0.86), (int)(sampleRate * 0.18)));

        result.metrics.push_back({ "vintage_muff_null", vintageMuffNull });
        result.metrics.push_back({ "vintage_velcro_null", vintageVelcroNull });
        result.metrics.push_back({ "muff_velcro_null", muffVelcroNull });
        result.metrics.push_back({ "velcro_tail_ratio", velcroTailRatio });
        result.passed = vintageMuffNull > 1.3e-3
            && vintageVelcroNull > 1.6e-3
            && muffVelcroNull > 1.5e-3
            && velcroTailRatio < 0.78;
        result.notes = result.passed
            ? "Fuzz modes stayed distinct and velcro settings clamped the late tail harder"
            : "Fuzz modes collapsed or the velcro clamp response was too weak";
        return result;
    }

    static OfflineQAScenarioResult runFuzzAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 3.0) / (double) blockSize));
        return runPedalAutomationStressScenario<FuzzPedal>("fuzz_automation_stress",
            "Fuzz remained finite under aggressive mode, gate and bias automation",
            [](FuzzPedal& pedal)
            {
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.78f));
            },
            [](FuzzPedal& pedal, float phase)
            {
                const int mode = juce::jlimit(0, 2, (int) std::floor(phase * 3.0f));
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.fuzzParam->setValueNotifyingHost(pedal.fuzzParam->convertTo0to1(12.0f + 84.0f * phase));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.06f + 0.88f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(0.02f + 0.94f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.12f + 0.82f * phase));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.22f + 0.62f * (1.0f - phase)));
                pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.04f + 0.92f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.15f))));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                juce::Random rng(0xF0222 + iteration);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                        block.setSample(ch, i, 0.15f * ((rng.nextFloat() * 2.0f) - 1.0f));
            },
            2.4,
            iterations);
    }

    static OfflineQAScenarioResult runWahFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float t = (float) i / (float) sampleRate;
            const float sample = 0.14f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t)
                + 0.06f * std::sin(juce::MathConstants<float>::twoPi * 880.0f * t);
            input.setSample(0, i, sample);
            input.setSample(1, i, sample);
        }

        return runPedalStateRecallScenario<ClassicWahPedal>("wah_flagship_recall",
            "Unified wah state recalled cleanly",
            [](ClassicWahPedal& pedal) { configureFlagshipWah(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runWahDynamicSweepScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "wah_dynamic_sweep";

        auto renderTouchResponse = [](float sensitivity)
        {
            ClassicWahPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
            pedal.sweepParam->setValueNotifyingHost(pedal.sweepParam->convertTo0to1(0.50f));
            pedal.sensitivityParam->setValueNotifyingHost(pedal.sensitivityParam->convertTo0to1(sensitivity));
            pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(4.0f));
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(220.0f));
            pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(0.86f));
            pedal.resonanceParam->setValueNotifyingHost(pedal.resonanceParam->convertTo0to1(5.8f));
            pedal.voiceParam->setValueNotifyingHost(pedal.voiceParam->convertTo0to1(0.56f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.3));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                float burstAmp = 0.0f;
                if (i >= (int)(sampleRate * 0.16) && i < (int)(sampleRate * 0.34))
                    burstAmp = 0.09f;
                else if (i >= (int)(sampleRate * 0.66) && i < (int)(sampleRate * 0.84))
                    burstAmp = 0.24f;

                const float sample = burstAmp * (0.62f * std::sin(juce::MathConstants<float>::twoPi * 350.0f * t)
                    + 0.38f * std::sin(juce::MathConstants<float>::twoPi * 1600.0f * t));
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto lowSens = renderTouchResponse(0.12f);
        const auto highSens = renderTouchResponse(0.88f);

        const int quietStart = (int)(sampleRate * 0.18);
        const int loudStart = (int)(sampleRate * 0.68);
        const int window = (int)(sampleRate * 0.10);
        const double quietHigh = computeFrequencyMagnitude(highSens, sampleRate, 1600.0f, quietStart, window);
        const double quietLow = computeFrequencyMagnitude(highSens, sampleRate, 350.0f, quietStart, window);
        const double loudHigh = computeFrequencyMagnitude(highSens, sampleRate, 1600.0f, loudStart, window);
        const double loudLow = computeFrequencyMagnitude(highSens, sampleRate, 350.0f, loudStart, window);
        const double lowSensLoudHigh = computeFrequencyMagnitude(lowSens, sampleRate, 1600.0f, loudStart, window);
        const double lowSensLoudLow = computeFrequencyMagnitude(lowSens, sampleRate, 350.0f, loudStart, window);

        const double quietTilt = quietHigh / juce::jmax(1.0e-9, quietLow);
        const double loudTilt = loudHigh / juce::jmax(1.0e-9, loudLow);
        const double lowSensLoudTilt = lowSensLoudHigh / juce::jmax(1.0e-9, lowSensLoudLow);
        const double responseNull = computeNullRms(lowSens, highSens);

        result.metrics.push_back({ "quiet_tilt", quietTilt });
        result.metrics.push_back({ "loud_tilt", loudTilt });
        result.metrics.push_back({ "low_sens_loud_tilt", lowSensLoudTilt });
        result.metrics.push_back({ "response_null_rms", responseNull });
        result.passed = loudTilt > quietTilt * 1.12
            && loudTilt > lowSensLoudTilt * 1.18
            && responseNull > 0.01;
        result.notes = result.passed
            ? "Touch wah tracked stronger picking with a meaningfully brighter sweep"
            : "Wah dynamics did not move the filter enough under stronger excitation";
        return result;
    }

    static OfflineQAScenarioResult runWahAutomationStressScenario()
    {
        return runPedalAutomationStressScenario<ClassicWahPedal>("wah_automation_stress",
            "Unified wah remained finite under rapid mode and envelope automation",
            [](ClassicWahPedal&) {},
            [](ClassicWahPedal& pedal, float phase)
            {
                const int mode = juce::jlimit(0, 2, (int) std::floor(phase * 3.0f));
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.sweepParam->setValueNotifyingHost(pedal.sweepParam->convertTo0to1(0.02f + 0.96f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                pedal.sensitivityParam->setValueNotifyingHost(pedal.sensitivityParam->convertTo0to1(0.04f + 0.92f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.2f))));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(0.5f + 26.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.5f))));
                pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(20.0f + 720.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 0.9f))));
                pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(0.04f + 0.94f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.7f))));
                pedal.resonanceParam->setValueNotifyingHost(pedal.resonanceParam->convertTo0to1(0.6f + 8.8f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.1f))));
                pedal.voiceParam->setValueNotifyingHost(pedal.voiceParam->convertTo0to1(0.02f + 0.96f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 0.8f))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.08f + 0.90f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.35f))));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * blockSize + i);
                        const float sample = 0.14f * std::sin(juce::MathConstants<float>::twoPi * 123.0f * sampleIndex / (float) sampleRate)
                            + 0.06f * std::sin(juce::MathConstants<float>::twoPi * 492.0f * sampleIndex / (float) sampleRate)
                            + 0.03f * std::sin(juce::MathConstants<float>::twoPi * 1960.0f * sampleIndex / (float) sampleRate);
                        block.setSample(ch, i, sample);
                    }
            },
            2.0,
            480);
    }

    static OfflineQAScenarioResult runOctaveFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float t = (float) i / (float) sampleRate;
            const float sample = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t);
            input.setSample(0, i, sample);
            input.setSample(1, i, sample);
        }

        return runPedalStateRecallScenario<OctavePedal>("octave_flagship_recall",
            "Commercial octave state recalled cleanly",
            [](OctavePedal& pedal) { configureFlagshipOctave(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runOctaveVoiceTrackingScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "octave_voice_tracking";

        auto renderSub = []()
        {
            OctavePedal pedal;
            pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(1.0f));
            pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(0.0f));
            pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(0.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.34f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                const float sample = 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        auto renderUpper = []()
        {
            OctavePedal pedal;
            pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(0.0f));
            pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(1.0f));
            pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(0.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.82f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                const float sample = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        auto renderTone = [](float tone)
        {
            OctavePedal pedal;
            pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(0.48f));
            pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(0.92f));
            pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(0.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(tone));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.1));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                const float sample = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto sub = renderSub();
        const auto upper = renderUpper();
        const auto dark = renderTone(0.08f);
        const auto bright = renderTone(0.92f);

        const int subStart = (int)(sampleRate * 0.35);
        const int subLength = (int)(sampleRate * 0.45);
        const int upperStart = (int)(sampleRate * 0.28);
        const int upperLength = (int)(sampleRate * 0.45);
        const int toneStart = (int)(sampleRate * 0.30);
        const int toneLength = (int)(sampleRate * 0.40);

        const double subRatio = computeFrequencyMagnitude(sub, sampleRate, 110.0f, subStart, subLength)
            / juce::jmax(1.0e-9, computeFrequencyMagnitude(sub, sampleRate, 220.0f, subStart, subLength));
        const double upperRatio = computeFrequencyMagnitude(upper, sampleRate, 440.0f, upperStart, upperLength)
            / juce::jmax(1.0e-9, computeFrequencyMagnitude(upper, sampleRate, 220.0f, upperStart, upperLength));
        const double toneOpenRatio = computeFrequencyMagnitude(bright, sampleRate, 392.0f, toneStart, toneLength)
            / juce::jmax(1.0e-9, computeFrequencyMagnitude(dark, sampleRate, 392.0f, toneStart, toneLength));
        const double toneNull = computeNullRms(dark, bright);

        result.metrics.push_back({ "sub_tracking_ratio", subRatio });
        result.metrics.push_back({ "upper_tracking_ratio", upperRatio });
        result.metrics.push_back({ "tone_open_ratio", toneOpenRatio });
        result.metrics.push_back({ "tone_null_rms", toneNull });
        result.passed = subRatio > 1.70
            && upperRatio > 1.18
            && toneOpenRatio > 1.24
            && toneNull > 0.015;
        result.notes = result.passed
            ? "Octave pedal tracked sub and upper voices while tone materially reshaped the generated voice"
            : "Octave tracking or tone voicing failed one of the acceptance targets";
        return result;
    }

    static OfflineQAScenarioResult runOctaveAutomationStressScenario()
    {
        return runPedalAutomationStressScenario<OctavePedal>("octave_automation_stress",
            "Octave remained finite under linked voice automation",
            [](OctavePedal&) {},
            [](OctavePedal& pedal, float t)
            {
                pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(juce::jmap(std::sin(t * 6.0f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(juce::jmap(std::cos(t * 7.4f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(juce::jmap(std::sin(t * 5.2f + 0.8f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(juce::jmap(std::cos(t * 8.1f + 0.4f), -1.0f, 1.0f, 0.02f, 0.98f)));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(juce::jmap(std::sin(t * 4.8f + 1.1f), -1.0f, 1.0f, 0.7f, 1.5f)));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * blockSize + i);
                        const float fundamental = 0.12f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 123.0 * sampleIndex / sampleRate));
                        const float harmonic = 0.05f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 246.0 * sampleIndex / sampleRate));
                        block.setSample(ch, i, fundamental + harmonic);
                    }
            },
            2.0,
            120);
    }

    static OfflineQAScenarioResult runChorusFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float phaseL = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) sampleRate;
            const float phaseR = juce::MathConstants<float>::twoPi * 247.0f * (float) i / (float) sampleRate;
            input.setSample(0, i, 0.18f * std::sin(phaseL));
            input.setSample(1, i, 0.14f * std::sin(phaseR));
        }

        return runPedalStateRecallScenario<ChorusPedal>("chorus_flagship_recall",
            "Commercial chorus state recalled cleanly",
            [](ChorusPedal& pedal) { configureFlagshipChorus(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runChorusModeDistinctnessScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "chorus_mode_distinctness";

        auto renderMode = [](int modeIndex)
        {
            ChorusPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(modeIndex == 2 ? 2.25f : 1.10f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(modeIndex == 2 ? 0.86f : 0.72f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.94f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.63f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.lagParam->setValueNotifyingHost(pedal.lagParam->convertTo0to1(modeIndex == 1 ? 11.8f : 8.4f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 2.1));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float sample = 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) sampleRate);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto classic = renderMode(0);
        const auto ensemble = renderMode(1);
        const auto vibrato = renderMode(2);
        const double ensembleCorr = computeStereoCorrelation(ensemble, (int)(sampleRate * 0.25));
        const double ensembleRightRms = computeChannelWindowRms(ensemble, 1, (int)(sampleRate * 0.25), (int)(sampleRate * 0.9));
        const double ensembleLeftRms = computeChannelWindowRms(ensemble, 0, (int)(sampleRate * 0.25), (int)(sampleRate * 0.9));
        const double classicEnsembleNull = computeNullRms(classic, ensemble);
        const double classicVibratoNull = computeNullRms(classic, vibrato);
        const double sideRatio = ensembleRightRms / juce::jmax(1.0e-9, ensembleLeftRms);
        const bool finite = bufferHasOnlyFiniteSamples(ensemble);

        result.metrics.push_back({ "ensemble_corr", ensembleCorr });
        result.metrics.push_back({ "ensemble_side_ratio", sideRatio });
        result.metrics.push_back({ "classic_ensemble_null", classicEnsembleNull });
        result.metrics.push_back({ "classic_vibrato_null", classicVibratoNull });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.passed = finite
            && std::abs(ensembleCorr) < 0.97
            && sideRatio > 0.30
            && classicEnsembleNull > 2.5e-3
            && classicVibratoNull > 2.0e-3;
        result.notes = result.passed
            ? "Chorus modes stayed distinct and ensemble mode opened a usable stereo field"
            : "Chorus modes collapsed or the ensemble stereo field stayed too narrow";
        return result;
    }

    static OfflineQAScenarioResult runChorusAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 3.0) / (double) blockSize));
        return runPedalAutomationStressScenario<ChorusPedal>("chorus_automation_stress",
            "Chorus remained finite under aggressive mode and width automation",
            [](ChorusPedal& pedal)
            {
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.58f));
            },
            [](ChorusPedal& pedal, float phase)
            {
                const int mode = juce::jlimit(0, 2, (int) std::floor(phase * 3.0f));
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.08f + 7.4f * phase));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.05f + 0.93f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.05f + 0.95f * (1.0f - phase)));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.12f + 0.82f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.10f + 0.85f * phase));
                pedal.lagParam->setValueNotifyingHost(pedal.lagParam->convertTo0to1(2.4f + 15.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                juce::Random rng(0xC4015 + iteration);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                        block.setSample(ch, i, 0.15f * ((rng.nextFloat() * 2.0f) - 1.0f));
            },
            1.8,
            iterations);
    }

    static OfflineQAScenarioResult runPhaserFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
        input.clear();
        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float phaseA = (float) (2.0 * juce::MathConstants<double>::pi * 247.0 * (double) i / sampleRate);
                const float phaseB = (float) (2.0 * juce::MathConstants<double>::pi * 493.0 * (double) i / sampleRate);
                input.setSample(ch, i, 0.11f * std::sin(phaseA) + 0.06f * std::sin(phaseB + ch * 0.17f));
            }

        return runPedalStateRecallScenario<PhaserPedal>("phaser_flagship_recall",
            "Modulation phaser state recalled cleanly",
            [](PhaserPedal& pedal) { configureFlagshipPhaser(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runPhaserVoiceScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "phaser_voice_response";

        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
        input.clear();
        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float phaseA = (float) (2.0 * juce::MathConstants<double>::pi * 247.0 * (double) i / sampleRate);
                const float phaseB = (float) (2.0 * juce::MathConstants<double>::pi * 493.0 * (double) i / sampleRate);
                input.setSample(ch, i, 0.11f * std::sin(phaseA) + 0.06f * std::sin(phaseB + ch * 0.17f));
            }

        PhaserPedal subtle;
        subtle.prepareToPlay(sampleRate, blockSize);
        subtle.rateParam->setValueNotifyingHost(subtle.rateParam->convertTo0to1(0.45f));
        subtle.depthParam->setValueNotifyingHost(subtle.depthParam->convertTo0to1(0.22f));
        subtle.feedbackParam->setValueNotifyingHost(subtle.feedbackParam->convertTo0to1(0.0f));
        subtle.stagesParam->setValueNotifyingHost(subtle.stagesParam->convertTo0to1(4.0f));
        subtle.mixParam->setValueNotifyingHost(subtle.mixParam->convertTo0to1(0.42f));
        subtle.reset();
        const auto subtleOutput = renderPedalOutput(subtle, input);

        PhaserPedal deep;
        deep.prepareToPlay(sampleRate, blockSize);
        deep.rateParam->setValueNotifyingHost(deep.rateParam->convertTo0to1(1.4f));
        deep.depthParam->setValueNotifyingHost(deep.depthParam->convertTo0to1(0.94f));
        deep.feedbackParam->setValueNotifyingHost(deep.feedbackParam->convertTo0to1(0.68f));
        deep.stagesParam->setValueNotifyingHost(deep.stagesParam->convertTo0to1(10.0f));
        deep.mixParam->setValueNotifyingHost(deep.mixParam->convertTo0to1(0.78f));
        deep.reset();
        const auto deepOutput = renderPedalOutput(deep, input);

        const double nullRms = computeNullRms(subtleOutput, deepOutput);
        const double deepCorr = computeStereoCorrelation(deepOutput, (int)(sampleRate * 0.4));

        result.metrics.push_back({ "voice_null_rms", nullRms });
        result.metrics.push_back({ "deep_corr", deepCorr });
        result.passed = nullRms > 0.022 && deepCorr < 0.992;
        result.notes = result.passed
            ? "Phaser depth and feedback produced a clearly modulated stereo voice"
            : "Phaser voice response was too subtle or remained too mono";
        return result;
    }

    static OfflineQAScenarioResult runPhaserModeDistinctnessScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "phaser_mode_distinctness";

        auto renderMode = [](int modeIndex)
        {
            PhaserPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(modeIndex == 0 ? 0.58f : modeIndex == 1 ? 1.2f : 1.75f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(modeIndex == 0 ? 0.62f : modeIndex == 1 ? 0.78f : 0.90f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(modeIndex == 0 ? 0.18f : modeIndex == 1 ? 0.42f : 0.56f));
            pedal.stagesParam->setValueNotifyingHost(pedal.stagesParam->convertTo0to1(modeIndex == 0 ? 4.0f : modeIndex == 1 ? 8.0f : 10.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.76f));
            pedal.reset();

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.6));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float phaseA = (float) (2.0 * juce::MathConstants<double>::pi * 247.0 * (double) i / sampleRate);
                const float phaseB = (float) (2.0 * juce::MathConstants<double>::pi * 493.0 * (double) i / sampleRate);
                input.setSample(0, i, 0.11f * std::sin(phaseA) + 0.05f * std::sin(phaseB));
                input.setSample(1, i, 0.11f * std::sin(phaseA + 0.12f) + 0.05f * std::sin(phaseB + 0.24f));
            }

            return renderPedalOutput(pedal, input);
        };

        const auto vintage = renderMode(0);
        const auto modern = renderMode(1);
        const auto vibe = renderMode(2);
        const double vintageModernNull = computeNullRms(vintage, modern);
        const double vintageVibeNull = computeNullRms(vintage, vibe);
        const double modernVibeNull = computeNullRms(modern, vibe);
        const double vibeCorr = computeStereoCorrelation(vibe, (int) (sampleRate * 0.35));

        result.metrics.push_back({ "vintage_modern_null", vintageModernNull });
        result.metrics.push_back({ "vintage_vibe_null", vintageVibeNull });
        result.metrics.push_back({ "modern_vibe_null", modernVibeNull });
        result.metrics.push_back({ "vibe_corr", vibeCorr });
        result.passed = vintageModernNull > 0.010
            && vintageVibeNull > 0.018
            && modernVibeNull > 0.014
            && vibeCorr < 0.992;
        result.notes = result.passed
            ? "Phaser voicings stayed meaningfully distinct and the vibe voice opened a wider stereo image"
            : "Phaser voicings collapsed toward the same response";
        return result;
    }

    static OfflineQAScenarioResult runPhaserAutomationStressScenario()
    {
        return runPedalAutomationStressScenario<PhaserPedal>("phaser_automation_stress",
            "Phaser remained finite under rapid rate, stage and feedback automation",
            [](PhaserPedal&) {},
            [](PhaserPedal& pedal, float t)
            {
                const int mode = juce::jlimit(0, 2, (int) std::floor(t * 3.0f));
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(juce::jmap(std::sin(t * 5.4f), -1.0f, 1.0f, 0.08f, 6.8f)));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(juce::jmap(std::cos(t * 7.0f), -1.0f, 1.0f, 0.05f, 0.98f)));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(juce::jmap(std::sin(t * 8.6f + 0.4f), -1.0f, 1.0f, -0.76f, 0.76f)));
                pedal.stagesParam->setValueNotifyingHost(pedal.stagesParam->convertTo0to1(juce::jmap(std::cos(t * 6.1f + 0.7f), -1.0f, 1.0f, 2.0f, 12.0f)));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(juce::jmap(std::sin(t * 9.9f + 0.3f), -1.0f, 1.0f, 0.0f, 1.0f)));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * blockSize + i);
                        block.setSample(ch, i, 0.15f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 207.0 * sampleIndex / sampleRate) + ch * 0.15f));
                    }
            },
            2.0,
            120);
    }

    static OfflineQAScenarioResult runFlangerFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float phaseL = juce::MathConstants<float>::twoPi * 173.0f * (float) i / (float) sampleRate;
            const float phaseR = juce::MathConstants<float>::twoPi * 233.0f * (float) i / (float) sampleRate;
            input.setSample(0, i, 0.18f * std::sin(phaseL));
            input.setSample(1, i, 0.15f * std::sin(phaseR));
        }

        return runPedalStateRecallScenario<FlangerPedal>("flanger_flagship_recall",
            "Commercial flanger state recalled cleanly",
            [](FlangerPedal& pedal) { configureFlagshipFlanger(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runFlangerModeDistinctnessScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "flanger_mode_distinctness";

        auto renderMode = [](int modeIndex)
        {
            FlangerPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(modeIndex == 1 ? 0.54f : 0.86f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(modeIndex == 2 ? 0.90f : 0.80f));
            pedal.manualParam->setValueNotifyingHost(pedal.manualParam->convertTo0to1(modeIndex == 2 ? 0.40f : 0.52f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(modeIndex == 2 ? -0.58f : 0.64f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.92f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(7600.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.reset();

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 2.0));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float sample = 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) sampleRate);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto classic = renderMode(0);
        const auto jet = renderMode(1);
        const auto zero = renderMode(2);
        const double jetCorr = computeStereoCorrelation(jet, (int)(sampleRate * 0.18));
        const double jetRightRms = computeChannelWindowRms(jet, 1, (int)(sampleRate * 0.18), (int)(sampleRate * 0.9));
        const double jetLeftRms = computeChannelWindowRms(jet, 0, (int)(sampleRate * 0.18), (int)(sampleRate * 0.9));
        const double classicJetNull = computeNullRms(classic, jet);
        const double classicZeroNull = computeNullRms(classic, zero);
        const double sideRatio = jetRightRms / juce::jmax(1.0e-9, jetLeftRms);
        const bool finite = bufferHasOnlyFiniteSamples(jet);

        result.metrics.push_back({ "jet_corr", jetCorr });
        result.metrics.push_back({ "jet_side_ratio", sideRatio });
        result.metrics.push_back({ "classic_jet_null", classicJetNull });
        result.metrics.push_back({ "classic_zero_null", classicZeroNull });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.passed = finite
            && std::abs(jetCorr) < 0.985
            && sideRatio > 0.25
            && classicJetNull > 1.5e-3
            && classicZeroNull > 1.5e-3;
        result.notes = result.passed
            ? "Flanger modes stayed distinct and Jet mode projected a wider stereo signature"
            : "Flanger modes collapsed or Jet mode failed to widen enough";
        return result;
    }

    static OfflineQAScenarioResult runFlangerAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 3.0) / (double) blockSize));
        return runPedalAutomationStressScenario<FlangerPedal>("flanger_automation_stress",
            "Flanger remained finite under aggressive mode, delay and feedback automation",
            [](FlangerPedal&) {},
            [](FlangerPedal& pedal, float phase)
            {
                const int mode = juce::jlimit(0, 2, (int) std::floor(phase * 3.0f));
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.05f + 4.8f * phase));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.05f + 0.93f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.manualParam->setValueNotifyingHost(pedal.manualParam->convertTo0to1(0.02f + 0.96f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(juce::jmap(std::sin(phase * juce::MathConstants<float>::pi * 1.3f), -1.0f, 1.0f, -0.90f, 0.90f)));
                pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.04f + 0.94f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.1f))));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(1200.0f + 11800.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 0.9f))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.08f + 0.90f * phase));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                juce::Random rng(0xF1A93 + iteration);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                        block.setSample(ch, i, 0.15f * ((rng.nextFloat() * 2.0f) - 1.0f));
            },
            1.9,
            iterations);
    }

    static OfflineQAScenarioResult runTremoloFlagshipRecallScenario()
    {
        juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.0));
        input.clear();
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const float t = (float) i / (float) sampleRate;
            input.setSample(0, i, 0.18f * std::sin(juce::MathConstants<float>::twoPi * 173.0f * t));
            input.setSample(1, i, 0.15f * std::sin(juce::MathConstants<float>::twoPi * 229.0f * t));
        }

        return runPedalStateRecallScenario<TremoloPedal>("tremolo_flagship_recall",
            "Studio tremolo state recalled cleanly",
            [](TremoloPedal& pedal) { configureFlagshipTremolo(pedal); },
            input,
            1.0e-6);
    }

    static OfflineQAScenarioResult runTremoloHarmonicScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "tremolo_harmonic_response";

        auto renderStereoField = [](float stereoAmount)
        {
            TremoloPedal pedal;
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(4.0f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.88f));
            pedal.shapeParam->setValueNotifyingHost(pedal.shapeParam->convertTo0to1(0.58f));
            pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.50f));
            pedal.stereoParam->setValueNotifyingHost(pedal.stereoParam->convertTo0to1(stereoAmount));
            pedal.harmonicParam->setValueNotifyingHost(pedal.harmonicParam->convertTo0to1(0.0f));
            pedal.crossoverParam->setValueNotifyingHost(pedal.crossoverParam->convertTo0to1(800.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.8));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                const float sample = 0.19f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        auto renderCrossoverFocus = [](float crossoverHz)
        {
            TremoloPedal pedal;
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(4.0f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(1.0f));
            pedal.shapeParam->setValueNotifyingHost(pedal.shapeParam->convertTo0to1(0.25f));
            pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.50f));
            pedal.stereoParam->setValueNotifyingHost(pedal.stereoParam->convertTo0to1(0.0f));
            pedal.harmonicParam->setValueNotifyingHost(pedal.harmonicParam->convertTo0to1(1.0f));
            pedal.crossoverParam->setValueNotifyingHost(pedal.crossoverParam->convertTo0to1(crossoverHz));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 0.9));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                const float sample = 0.20f * std::sin(juce::MathConstants<float>::twoPi * 800.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        auto renderBiasShape = [](float bias)
        {
            TremoloPedal pedal;
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(4.6f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(1.0f));
            pedal.shapeParam->setValueNotifyingHost(pedal.shapeParam->convertTo0to1(1.0f));
            pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(bias));
            pedal.stereoParam->setValueNotifyingHost(pedal.stereoParam->convertTo0to1(0.0f));
            pedal.harmonicParam->setValueNotifyingHost(pedal.harmonicParam->convertTo0to1(0.0f));
            pedal.crossoverParam->setValueNotifyingHost(pedal.crossoverParam->convertTo0to1(800.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(sampleRate, blockSize);

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.1));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sampleRate;
                const float sample = 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderPedalOutput(pedal, input);
        };

        const auto centered = renderStereoField(0.0f);
        const auto widened = renderStereoField(1.0f);
        const auto lowSplit = renderCrossoverFocus(350.0f);
        const auto highSplit = renderCrossoverFocus(1600.0f);
        const auto earlyBias = renderBiasShape(0.18f);
        const auto lateBias = renderBiasShape(0.82f);

        const double centeredCorr = computeStereoCorrelation(centered, (int)(sampleRate * 0.20));
        const double widenedCorr = computeStereoCorrelation(widened, (int)(sampleRate * 0.20));
        const double highSplitWindow = computeWindowRms(highSplit, (int)(sampleRate * 0.055), (int)(sampleRate * 0.035));
        const double lowSplitWindow = computeWindowRms(lowSplit, (int)(sampleRate * 0.055), (int)(sampleRate * 0.035));
        const double splitNull = computeNullRms(lowSplit, highSplit);
        const double contourNull = computeNullRms(earlyBias, lateBias);

        result.metrics.push_back({ "centered_corr", centeredCorr });
        result.metrics.push_back({ "widened_corr", widenedCorr });
        result.metrics.push_back({ "high_low_split_ratio", highSplitWindow / juce::jmax(1.0e-9, lowSplitWindow) });
        result.metrics.push_back({ "split_null_rms", splitNull });
        result.metrics.push_back({ "bias_contour_null_rms", contourNull });
        result.passed = centeredCorr > 0.995
            && widenedCorr < 0.82
            && (centeredCorr - widenedCorr) > 0.12
            && highSplitWindow > lowSplitWindow * 1.4
            && splitNull > 0.02
            && contourNull > 0.025;
        result.notes = result.passed
            ? "Tremolo stereo, harmonic crossover and bias controls all produced strong, measurable voicing changes"
            : "One or more tremolo modulation controls failed to produce the expected voicing separation";
        return result;
    }

    static OfflineQAScenarioResult runTremoloAutomationStressScenario()
    {
        const int iterations = juce::jmax(120, (int) ((sampleRate * 3.0) / (double) blockSize));
        return runPedalAutomationStressScenario<TremoloPedal>("tremolo_automation_stress",
            "Tremolo remained finite under aggressive modulation automation",
            [](TremoloPedal&) {},
            [](TremoloPedal& pedal, float phase)
            {
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.6f + 11.8f * phase));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.05f + 0.95f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.shapeParam->setValueNotifyingHost(pedal.shapeParam->convertTo0to1(0.05f + 0.95f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.2f))));
                pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.10f + 0.80f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.5f))));
                pedal.stereoParam->setValueNotifyingHost(pedal.stereoParam->convertTo0to1(std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.harmonicParam->setValueNotifyingHost(pedal.harmonicParam->convertTo0to1(std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.8f))));
                pedal.crossoverParam->setValueNotifyingHost(pedal.crossoverParam->convertTo0to1(280.0f + 1600.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.1f))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.35f))));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.70f + 0.85f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.4f))));
            },
            [](juce::AudioBuffer<float>& block, int iteration, float)
            {
                for (int i = 0; i < block.getNumSamples(); ++i)
                {
                    const float sampleIndex = (float) (iteration * blockSize + i);
                    block.setSample(0, i,
                        0.17f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * sampleIndex / (float) sampleRate)
                        + 0.05f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * sampleIndex / (float) sampleRate));
                    block.setSample(1, i,
                        0.14f * std::sin(juce::MathConstants<float>::twoPi * 147.0f * sampleIndex / (float) sampleRate)
                        + 0.04f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * sampleIndex / (float) sampleRate));
                }
            },
            2.0,
            iterations);
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

    static OfflineQAScenarioResult runReverbSwellScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_swell_bloom";

        auto renderPedal = [](float swellAmount)
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipCloud(pedal);
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.80f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.84f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.90f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(18.0f));
            pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(swellAmount));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.2));
            input.clear();
            const int burstSamples = (int)(sampleRate * 0.35);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float)i / (float)sampleRate;
                const float sample = 0.20f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            juce::MidiBuffer midi;
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

        const auto baselineOut = renderPedal(0.0f);
        const auto swelledOut = renderPedal(0.92f);
        const bool finite = bufferHasOnlyFiniteSamples(swelledOut);
        const double baselineEarly = computeWindowRms(baselineOut, (int)(sampleRate * 0.03), (int)(sampleRate * 0.11));
        const double swelledEarly = computeWindowRms(swelledOut, (int)(sampleRate * 0.03), (int)(sampleRate * 0.11));
        const double baselineBloom = computeWindowRms(baselineOut, (int)(sampleRate * 0.20), (int)(sampleRate * 0.22));
        const double swelledBloom = computeWindowRms(swelledOut, (int)(sampleRate * 0.20), (int)(sampleRate * 0.22));

        result.metrics.push_back({ "baseline_early_rms", baselineEarly });
        result.metrics.push_back({ "swelled_early_rms", swelledEarly });
        result.metrics.push_back({ "baseline_bloom_rms", baselineBloom });
        result.metrics.push_back({ "swelled_bloom_rms", swelledBloom });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && swelledEarly < baselineEarly * 0.72
            && swelledBloom > swelledEarly * 1.45
            && swelledBloom > baselineBloom * 0.45;
        result.notes = result.passed ? "Swell softened the wet attack and bloomed afterward"
                                     : "Swell did not produce a clear delayed bloom profile";
        return result;
    }

    static OfflineQAScenarioResult runReverbGateScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_gate_clamp";

        auto renderPedal = [](float gateAmount)
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipHall(pedal);
            pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(gateAmount));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.6));
            input.clear();
            const int burstSamples = (int)(sampleRate * 0.22);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 220.0f * (float)i / (float)sampleRate;
                const float sample = 0.22f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            juce::MidiBuffer midi;
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

        const auto baselineOut = renderPedal(0.0f);
        const auto gatedOut = renderPedal(0.92f);
        const bool finite = bufferHasOnlyFiniteSamples(gatedOut);
        const double baselineBody = computeWindowRms(baselineOut, (int)(sampleRate * 0.12), (int)(sampleRate * 0.20));
        const double gatedBody = computeWindowRms(gatedOut, (int)(sampleRate * 0.12), (int)(sampleRate * 0.20));
        const double baselineTail = computeWindowRms(baselineOut, (int)(sampleRate * 0.90), (int)(sampleRate * 0.25));
        const double gatedTail = computeWindowRms(gatedOut, (int)(sampleRate * 0.90), (int)(sampleRate * 0.25));

        result.metrics.push_back({ "baseline_body_rms", baselineBody });
        result.metrics.push_back({ "gated_body_rms", gatedBody });
        result.metrics.push_back({ "baseline_tail_rms", baselineTail });
        result.metrics.push_back({ "gated_tail_rms", gatedTail });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && gatedBody > baselineBody * 0.55
            && gatedTail < baselineTail * 0.42;
        result.notes = result.passed ? "Gate kept the body while clamping the late tail"
                                     : "Gate failed to clamp the tail decisively enough";
        return result;
    }

    static OfflineQAScenarioResult runReverbReverseScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_reverse_bloom";

        auto renderPedal = [](float reverseAmount)
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipCloud(pedal);
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.82f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.88f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.92f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(20.0f));
            pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(reverseAmount));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.4));
            input.clear();
            const int burstSamples = (int)(sampleRate * 0.18);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 246.0f * (float)i / (float)sampleRate;
                const float sample = 0.20f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            juce::MidiBuffer midi;
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

        const auto baselineOut = renderPedal(0.0f);
        const auto reverseOut = renderPedal(0.92f);
        const bool finite = bufferHasOnlyFiniteSamples(reverseOut);
        const double baselineEarly = computeWindowRms(baselineOut, (int)(sampleRate * 0.03), (int)(sampleRate * 0.14));
        const double reverseEarly = computeWindowRms(reverseOut, (int)(sampleRate * 0.03), (int)(sampleRate * 0.14));
        const double baselineLate = computeWindowRms(baselineOut, (int)(sampleRate * 0.24), (int)(sampleRate * 0.30));
        const double reverseLate = computeWindowRms(reverseOut, (int)(sampleRate * 0.24), (int)(sampleRate * 0.30));

        result.metrics.push_back({ "baseline_early_rms", baselineEarly });
        result.metrics.push_back({ "reverse_early_rms", reverseEarly });
        result.metrics.push_back({ "baseline_late_rms", baselineLate });
        result.metrics.push_back({ "reverse_late_rms", reverseLate });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && reverseEarly < baselineEarly * 0.70
            && reverseLate > reverseEarly * 1.60
            && reverseLate > baselineLate * 0.65;
        result.notes = result.passed ? "Reverse ambience delayed the bloom while keeping useful late energy"
                                     : "Reverse ambience did not create a clear delayed bloom profile";
        return result;
    }

    static OfflineQAScenarioResult runReverbReverseSwellScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_reverse_swell_combo";

        auto renderPedal = [](float reverseAmount, float swellAmount)
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipCloud(pedal);
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.84f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.90f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.94f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(24.0f));
            pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(reverseAmount));
            pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(swellAmount));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.5));
            input.clear();
            const int burstSamples = (int)(sampleRate * 0.16);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 174.0f * (float)i / (float)sampleRate;
                const float sample = 0.22f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            juce::MidiBuffer midi;
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

        const auto baselineOut = renderPedal(0.0f, 0.0f);
        const auto comboOut = renderPedal(0.82f, 0.84f);
        const bool finite = bufferHasOnlyFiniteSamples(comboOut);
        const double baselineEarly = computeWindowRms(baselineOut, (int)(sampleRate * 0.03), (int)(sampleRate * 0.14));
        const double comboEarly = computeWindowRms(comboOut, (int)(sampleRate * 0.03), (int)(sampleRate * 0.14));
        const double baselineLate = computeWindowRms(baselineOut, (int)(sampleRate * 0.24), (int)(sampleRate * 0.32));
        const double comboLate = computeWindowRms(comboOut, (int)(sampleRate * 0.24), (int)(sampleRate * 0.32));

        result.metrics.push_back({ "baseline_early_rms", baselineEarly });
        result.metrics.push_back({ "combo_early_rms", comboEarly });
        result.metrics.push_back({ "baseline_late_rms", baselineLate });
        result.metrics.push_back({ "combo_late_rms", comboLate });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && comboEarly < baselineEarly * 0.60
            && comboLate > comboEarly * 2.00
            && comboLate > baselineLate * 0.48;
        result.notes = result.passed ? "Reverse+swell produced a delayed cinematic bloom without collapsing the body"
                                     : "Reverse+swell did not hold together as a usable performance combo";
        return result;
    }

    static OfflineQAScenarioResult runReverbFreezeReverseScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "reverb_freeze_reverse_capture";

        auto renderPedal = [](bool automateFreeze)
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipCloud(pedal);
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.86f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.90f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.94f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(20.0f));
            pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(0.78f));

            juce::AudioBuffer<float> input(2, (int)(sampleRate * 1.8));
            input.clear();
            const int burstSamples = (int)(sampleRate * 0.30);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float)i / (float)sampleRate;
                const float sample = 0.20f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> rendered(2, input.getNumSamples());
            rendered.clear();
            juce::AudioBuffer<float> block(2, blockSize);

            for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
            {
                const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
                if (automateFreeze && offset >= (int)(sampleRate * 0.58))
                    pedal.freezeParam->setValueNotifyingHost(1.0f);
                else
                    pedal.freezeParam->setValueNotifyingHost(0.0f);

                block.clear();
                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom(ch, 0, input, ch, offset, numSamples);
                pedal.processBlock(block, midi);
                rendered.copyFrom(0, offset, block, 0, 0, numSamples);
                rendered.copyFrom(1, offset, block, 1, 0, numSamples);
            }

            return rendered;
        };

        const auto baselineOut = renderPedal(false);
        const auto frozenOut = renderPedal(true);
        const bool finite = bufferHasOnlyFiniteSamples(frozenOut);
        const double captureRms = computeWindowRms(frozenOut, (int)(sampleRate * 0.78), (int)(sampleRate * 0.20));
        const double heldRms = computeWindowRms(frozenOut, (int)(sampleRate * 1.34), (int)(sampleRate * 0.28));
        const double baselineHeld = computeWindowRms(baselineOut, (int)(sampleRate * 1.34), (int)(sampleRate * 0.28));

        result.metrics.push_back({ "capture_rms", captureRms });
        result.metrics.push_back({ "held_rms", heldRms });
        result.metrics.push_back({ "baseline_held_rms", baselineHeld });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && heldRms > captureRms * 0.55
            && heldRms > baselineHeld * 2.50;
        result.notes = result.passed ? "Freeze captured a stable reverse pad instead of letting it collapse"
                                     : "Freeze+reverse did not hold a convincing captured pad";
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
            reverb.swellParam->setValueNotifyingHost(reverb.swellParam->convertTo0to1(0.85f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
            reverb.gateParam->setValueNotifyingHost(reverb.gateParam->convertTo0to1(0.80f * phase));
            reverb.reverseParam->setValueNotifyingHost(reverb.reverseParam->convertTo0to1(0.70f * (1.0f - std::abs(phase * 2.0f - 1.0f))));
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

    static OfflineQAScenarioResult runDelayTailScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_tail_decay";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int) Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        engine.addPedal("Delay", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "offline-delay-tail");
        engine.setEngineEnabled(true);
        warmUpEngine(engine, 16);

        auto* delay = getOfflineDelay(engine);
        if (delay == nullptr)
        {
            result.notes = "Failed to resolve offline delay processor from graph";
            return result;
        }

        configureFlagshipDelayTape(*delay);
        const auto output = processBuffer(engine, generateImpulse((int) (sampleRate * 5.5), 1.0f));
        const bool finite = bufferHasOnlyFiniteSamples(output);
        const double lateRms = computeWindowRms(output, (int) (sampleRate * 1.0), (int) (sampleRate * 0.9));
        const double endRms = computeWindowRms(output, output.getNumSamples() - (int) (sampleRate * 0.35), (int) (sampleRate * 0.3));
        const auto metrics = analyseBuffer(output);

        result.metrics.push_back({ "peak", metrics.peak });
        result.metrics.push_back({ "late_rms_1p0_1p9s", lateRms });
        result.metrics.push_back({ "end_rms_last_300ms", endRms });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && metrics.peak < 1.35 && lateRms > 1.0e-4 && endRms < lateRms * 0.60;
        result.notes = result.passed ? "Tape delay tail stayed finite and decayed cleanly"
                                     : "Delay tail failed finite/decay expectations";
        return result;
    }

    static OfflineQAScenarioResult runDelayStereoFieldScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_stereo_field";

        AudioEngine engine;
        engine.prepare(sampleRate, blockSize, 2, 2);
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int) Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        engine.addPedal("Delay", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "offline-delay-stereo");
        engine.setEngineEnabled(true);
        warmUpEngine(engine, 16);

        auto* delay = getOfflineDelay(engine);
        if (delay == nullptr)
        {
            result.notes = "Failed to resolve offline delay processor from graph";
            return result;
        }

        configureFlagshipDelayDigital(*delay);
        const auto output = processBuffer(engine, generateLeftImpulse((int) (sampleRate * 2.4), 1.0f));
        const bool finite = bufferHasOnlyFiniteSamples(output);
        const double corr = computeStereoCorrelation(output, (int) (sampleRate * 0.08));
        const double rmsLeft = computeChannelWindowRms(output, 0, (int) (sampleRate * 0.08), (int) (sampleRate * 1.1));
        const double rmsRight = computeChannelWindowRms(output, 1, (int) (sampleRate * 0.08), (int) (sampleRate * 1.1));
        const double sideRatio = rmsRight / juce::jmax(1.0e-9, rmsLeft);

        result.metrics.push_back({ "corr_after_80ms", corr });
        result.metrics.push_back({ "left_rms_80_1180ms", rmsLeft });
        result.metrics.push_back({ "right_rms_80_1180ms", rmsRight });
        result.metrics.push_back({ "right_to_left_ratio", sideRatio });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && std::abs(corr) < 0.95 && sideRatio > 0.18;
        result.notes = result.passed ? "Digital delay projected decorrelated stereo repeats"
                                     : "Delay stereo field collapsed or stayed too imbalanced";
        return result;
    }

    static OfflineQAScenarioResult runDelayModeDistinctnessScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_mode_distinctness";

        auto renderMode = [](int modeIndex)
        {
            DelayPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(480.0f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.74f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(6200.0f));
            pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.78f));
            pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.62f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(modeIndex == 3 ? 0.70f : 0.0f));

            return renderDelayPedalWithAutomation(pedal, generateImpulse((int) (sampleRate * 2.2), 1.0f), [](int) {});
        };

        const auto analog = renderMode(0);
        const auto tape = renderMode(1);
        const auto digital = renderMode(2);
        const auto reverse = renderMode(3);

        const double analogTapeNull = computeNullRms(analog, tape);
        const double tapeDigitalNull = computeNullRms(tape, digital);
        const double digitalReverseNull = computeNullRms(digital, reverse);

        result.metrics.push_back({ "analog_tape_null_rms", analogTapeNull });
        result.metrics.push_back({ "tape_digital_null_rms", tapeDigitalNull });
        result.metrics.push_back({ "digital_reverse_null_rms", digitalReverseNull });

        result.passed = analogTapeNull > 9.0e-4
            && tapeDigitalNull > 8.0e-4
            && digitalReverseNull > 9.0e-4;
        result.notes = result.passed ? "Analog, Tape, Digital and Reverse produce clearly separated repeat signatures"
                                     : "Delay hero modes still overlap too much in their rendered repeats";
        return result;
    }

    static OfflineQAScenarioResult runDelaySyncTimingScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_sync_timing";

        DelayPedal pedal;
        pedal.prepareToPlay(sampleRate, blockSize);
        configureFlagshipDelayDigital(pedal);
        pedal.syncParam->setValueNotifyingHost(1.0f);
        pedal.syncDivisionParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.syncDivisionParam, 7));
        pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.38f));
        pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.0f));
        pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.08f));
        pedal.modDepthParam->setValueNotifyingHost(pedal.modDepthParam->convertTo0to1(0.0f));
        pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(0.0f));
        pedal.setTempoSyncContext(120.0f, true, true);

        const auto rendered = renderDelayPedalWithAutomation(pedal,
            generateImpulse((int) (sampleRate * 1.4), 1.0f),
            [](int) {});

        const bool finite = bufferHasOnlyFiniteSamples(rendered);
        const int searchStart = (int) (sampleRate * 0.20);
        int bestIndex = searchStart;
        float bestMagnitude = 0.0f;
        for (int i = searchStart; i < rendered.getNumSamples(); ++i)
        {
            const float magnitude = std::abs(rendered.getSample(0, i));
            if (magnitude > bestMagnitude)
            {
                bestMagnitude = magnitude;
                bestIndex = i;
            }
        }

        const double expectedIndex = sampleRate * 0.5;
        const double deltaMs = std::abs((bestIndex - expectedIndex) * 1000.0 / sampleRate);
        result.metrics.push_back({ "repeat_peak_index", (double) bestIndex });
        result.metrics.push_back({ "repeat_peak_magnitude", bestMagnitude });
        result.metrics.push_back({ "expected_index", expectedIndex });
        result.metrics.push_back({ "delta_ms", deltaMs });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && bestMagnitude > 0.05f && deltaMs < 35.0;
        result.notes = result.passed ? "Tempo sync landed the first quarter-note repeat close to the host grid"
                                     : "Tempo sync missed the expected quarter-note landing";
        return result;
    }

    static OfflineQAScenarioResult runDelayModulationRangeScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_modulation_range";

        auto renderVariant = [](float modDepth, float modRate)
        {
            DelayPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipDelayTape(pedal);
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(520.0f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.74f));
            pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.82f));
            pedal.modDepthParam->setValueNotifyingHost(pedal.modDepthParam->convertTo0to1(modDepth));
            pedal.modRateParam->setValueNotifyingHost(pedal.modRateParam->convertTo0to1(modRate));

            return renderDelayPedalWithAutomation(pedal,
                generateSine((int) (sampleRate * 1.8), 247.0, 0.20f),
                [](int) {});
        };

        const auto restrained = renderVariant(0.02f, 0.35f);
        const auto animated = renderVariant(0.92f, 3.20f);
        const bool finite = bufferHasOnlyFiniteSamples(animated);
        const double nullRms = computeNullRms(restrained, animated);
        const double restrainedCorr = computeStereoCorrelation(restrained, (int) (sampleRate * 0.18));
        const double animatedCorr = computeStereoCorrelation(animated, (int) (sampleRate * 0.18));

        result.metrics.push_back({ "null_rms", nullRms });
        result.metrics.push_back({ "restrained_corr", restrainedCorr });
        result.metrics.push_back({ "animated_corr", animatedCorr });
        result.metrics.push_back({ "corr_delta", std::abs(animatedCorr - restrainedCorr) });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && nullRms > 1.5e-3 && std::abs(animatedCorr - restrainedCorr) > 0.015;
        result.notes = result.passed ? "Modulation controls cover a materially wider movement range"
                                     : "Modulation controls did not open enough sonic range";
        return result;
    }

    static OfflineQAScenarioResult runDelayFlagshipRecallScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_flagship_recall";

        double worstNullRms = 0.0;
        bool finite = true;

        for (int presetIndex = 0; presetIndex < DelayPedal::getNumFlagshipPresets(); ++presetIndex)
        {
            DelayPedal source;
            source.prepareToPlay(sampleRate, blockSize);
            source.applyFlagshipPreset(presetIndex);

            juce::MemoryBlock state;
            source.getStateInformation(state);

            DelayPedal recalled;
            recalled.prepareToPlay(sampleRate, blockSize);
            recalled.setStateInformation(state.getData(), (int) state.getSize());

            const auto input = generateImpulse((int) (sampleRate * 1.8), 1.0f);
            const auto renderedSource = renderDelayPedalWithAutomation(source, input, [](int) {});
            const auto renderedRecalled = renderDelayPedalWithAutomation(recalled, input, [](int) {});

            finite = finite
                && bufferHasOnlyFiniteSamples(renderedSource)
                && bufferHasOnlyFiniteSamples(renderedRecalled);
            worstNullRms = juce::jmax(worstNullRms, computeNullRms(renderedSource, renderedRecalled));
        }

        result.metrics.push_back({ "worst_null_rms", worstNullRms });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && worstNullRms < 1.0e-6;
        result.notes = result.passed ? "Flagship delay presets survive state save/load transparently"
                                     : "At least one flagship delay preset drifted after state recall";
        return result;
    }

    static OfflineQAScenarioResult runDelayFreezeScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_freeze_hold";

        auto renderPedal = [](bool automateFreeze)
        {
            DelayPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipDelayTape(pedal);

            juce::AudioBuffer<float> input(2, (int) (sampleRate * 2.0));
            input.clear();
            const int burstSamples = (int) (sampleRate * 0.32);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) sampleRate;
                const float sample = 0.20f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderDelayPedalWithAutomation(pedal, input,
                [&](int offset)
                {
                    pedal.freezeParam->setValueNotifyingHost(automateFreeze && offset >= (int) (sampleRate * 0.62) ? 1.0f : 0.0f);
                });
        };

        const auto baselineOut = renderPedal(false);
        const auto frozenOut = renderPedal(true);
        const bool finite = bufferHasOnlyFiniteSamples(frozenOut);
        const double captureRms = computeWindowRms(frozenOut, (int) (sampleRate * 0.78), (int) (sampleRate * 0.22));
        const double heldRms = computeWindowRms(frozenOut, (int) (sampleRate * 1.45), (int) (sampleRate * 0.26));
        const double baselineHeld = computeWindowRms(baselineOut, (int) (sampleRate * 1.45), (int) (sampleRate * 0.26));

        result.metrics.push_back({ "capture_rms", captureRms });
        result.metrics.push_back({ "held_rms", heldRms });
        result.metrics.push_back({ "baseline_held_rms", baselineHeld });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && heldRms > captureRms * 0.50 && heldRms > baselineHeld * 2.0;
        result.notes = result.passed ? "Freeze captured and held a stable repeat bed"
                                     : "Freeze failed to hold enough of the captured delay bed";
        return result;
    }

    static OfflineQAScenarioResult runDelayDuckingScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_ducking_response";

        auto configure = [](DelayPedal& pedal, float duckAmount)
        {
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipDelayDigital(pedal);
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(410.0f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.70f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(9600.0f));
            pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.74f));
            pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.28f));
            pedal.duckParam->setValueNotifyingHost(pedal.duckParam->convertTo0to1(duckAmount));
        };

        DelayPedal baseline;
        configure(baseline, 0.0f);
        DelayPedal ducked;
        configure(ducked, 0.90f);

        const auto input = generateSine((int) (sampleRate * 1.6), 220.0, 0.24f);
        const auto baselineOut = renderDelayPedalWithAutomation(baseline, input, [](int) {});
        const auto duckedOut = renderDelayPedalWithAutomation(ducked, input, [](int) {});
        const bool finite = bufferHasOnlyFiniteSamples(duckedOut);
        const double baselineRms = computeWindowRms(baselineOut, (int) (sampleRate * 0.65), (int) (sampleRate * 0.35));
        const double duckedRms = computeWindowRms(duckedOut, (int) (sampleRate * 0.65), (int) (sampleRate * 0.35));

        result.metrics.push_back({ "baseline_rms", baselineRms });
        result.metrics.push_back({ "ducked_rms", duckedRms });
        result.metrics.push_back({ "ratio", duckedRms / juce::jmax(1.0e-9, baselineRms) });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite && duckedRms < baselineRms * 0.78;
        result.notes = result.passed ? "Ducking carved audible space while the source stayed active"
                                     : "Ducking did not reduce the delay bed enough under sustained input";
        return result;
    }

    static OfflineQAScenarioResult runDelayReverseScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_reverse_bloom";

        auto renderPedal = [](float reverseAmount)
        {
            DelayPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipDelayDigital(pedal);
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(440.0f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.72f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(9400.0f));
            pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.82f));
            pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.48f));
            pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(reverseAmount));

            juce::AudioBuffer<float> input(2, (int) (sampleRate * 1.8));
            input.clear();
            const int burstSamples = (int) (sampleRate * 0.18);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 246.0f * (float) i / (float) sampleRate;
                const float sample = 0.20f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderDelayPedalWithAutomation(pedal, input, [](int) {});
        };

        const auto baselineOut = renderPedal(0.0f);
        const auto reverseOut = renderPedal(0.92f);
        const bool finite = bufferHasOnlyFiniteSamples(reverseOut);
        const double baselineEarly = computeWindowRms(baselineOut, (int) (sampleRate * 0.40), (int) (sampleRate * 0.14));
        const double reverseEarly = computeWindowRms(reverseOut, (int) (sampleRate * 0.40), (int) (sampleRate * 0.14));
        const double baselineLate = computeWindowRms(baselineOut, (int) (sampleRate * 0.54), (int) (sampleRate * 0.22));
        const double reverseLate = computeWindowRms(reverseOut, (int) (sampleRate * 0.54), (int) (sampleRate * 0.22));

        result.metrics.push_back({ "baseline_early_rms", baselineEarly });
        result.metrics.push_back({ "reverse_early_rms", reverseEarly });
        result.metrics.push_back({ "baseline_late_rms", baselineLate });
        result.metrics.push_back({ "reverse_late_rms", reverseLate });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && reverseEarly < baselineEarly * 0.90
            && reverseLate > reverseEarly * 0.68
            && reverseLate > baselineLate * 0.70;
        result.notes = result.passed ? "Reverse softened the early repeat body while keeping useful later energy"
                                     : "Reverse did not keep a convincing later body";
        return result;
    }

    static OfflineQAScenarioResult runDelaySwellScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_swell_bloom";

        auto renderPedal = [](float swellAmount)
        {
            DelayPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipDelayAnalog(pedal);
            pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(swellAmount));

            juce::AudioBuffer<float> input(2, (int) (sampleRate * 1.5));
            input.clear();
            const int burstSamples = (int) (sampleRate * 0.24);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 174.0f * (float) i / (float) sampleRate;
                const float sample = 0.22f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderDelayPedalWithAutomation(pedal, input, [](int) {});
        };

        const auto baselineOut = renderPedal(0.0f);
        const auto swelledOut = renderPedal(0.92f);
        const bool finite = bufferHasOnlyFiniteSamples(swelledOut);
        const double baselineEarly = computeWindowRms(baselineOut, (int) (sampleRate * 0.34), (int) (sampleRate * 0.14));
        const double swelledEarly = computeWindowRms(swelledOut, (int) (sampleRate * 0.34), (int) (sampleRate * 0.14));
        const double baselineBloom = computeWindowRms(baselineOut, (int) (sampleRate * 0.48), (int) (sampleRate * 0.18));
        const double swelledBloom = computeWindowRms(swelledOut, (int) (sampleRate * 0.48), (int) (sampleRate * 0.18));

        result.metrics.push_back({ "baseline_early_rms", baselineEarly });
        result.metrics.push_back({ "swelled_early_rms", swelledEarly });
        result.metrics.push_back({ "baseline_bloom_rms", baselineBloom });
        result.metrics.push_back({ "swelled_bloom_rms", swelledBloom });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && swelledEarly < baselineEarly * 0.88
            && swelledBloom > swelledEarly * 1.05
            && swelledBloom > baselineBloom * 0.70;
        result.notes = result.passed ? "Swell softened the early repeat onset and bloomed afterward"
                                     : "Swell did not produce a clear delayed repeat bloom";
        return result;
    }

    static OfflineQAScenarioResult runDelayReverseSwellScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_reverse_swell_combo";

        auto renderPedal = [](float reverseAmount, float swellAmount)
        {
            DelayPedal pedal;
            pedal.prepareToPlay(sampleRate, blockSize);
            configureFlagshipDelayDigital(pedal);
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(460.0f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.72f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(9000.0f));
            pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.84f));
            pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.48f));
            pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(reverseAmount));
            pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(swellAmount));

            juce::AudioBuffer<float> input(2, (int) (sampleRate * 1.9));
            input.clear();
            const int burstSamples = (int) (sampleRate * 0.16);
            for (int i = 0; i < burstSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) sampleRate;
                const float sample = 0.20f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            return renderDelayPedalWithAutomation(pedal, input, [](int) {});
        };

        const auto baselineOut = renderPedal(0.0f, 0.0f);
        const auto comboOut = renderPedal(0.82f, 0.86f);
        const bool finite = bufferHasOnlyFiniteSamples(comboOut);
        const double baselineEarly = computeWindowRms(baselineOut, (int) (sampleRate * 0.40), (int) (sampleRate * 0.14));
        const double comboEarly = computeWindowRms(comboOut, (int) (sampleRate * 0.40), (int) (sampleRate * 0.14));
        const double baselineLate = computeWindowRms(baselineOut, (int) (sampleRate * 0.58), (int) (sampleRate * 0.24));
        const double comboLate = computeWindowRms(comboOut, (int) (sampleRate * 0.58), (int) (sampleRate * 0.24));

        result.metrics.push_back({ "baseline_early_rms", baselineEarly });
        result.metrics.push_back({ "combo_early_rms", comboEarly });
        result.metrics.push_back({ "baseline_late_rms", baselineLate });
        result.metrics.push_back({ "combo_late_rms", comboLate });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });

        result.passed = finite
            && comboEarly < baselineEarly * 0.90
            && comboLate > comboEarly * 0.52
            && comboLate > baselineLate * 0.70;
        result.notes = result.passed ? "Reverse+swell produced a later ambient body without collapsing the mix"
                                     : "Reverse+swell did not hold together as a usable performance combo";
        return result;
    }

    static OfflineQAScenarioResult runDelayAutomationStressScenario()
    {
        OfflineQAScenarioResult result;
        result.name = "delay_automation_stress";

        DelayPedal delay;
        delay.prepareToPlay(sampleRate, blockSize);
        delay.mixParam->setValueNotifyingHost(delay.mixParam->convertTo0to1(0.62f));

        juce::Random rng(0xD31A9);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, blockSize);
        bool finite = true;
        double peak = 0.0;

        const int blocksToRun = (int) ((sampleRate * 3.2) / (double) blockSize);
        for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    block.setSample(ch, i, 0.16f * ((rng.nextFloat() * 2.0f) - 1.0f));

            const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
            const int mode = juce::jlimit(0, 3, (int) std::floor(phase * 4.0f));
            const bool syncOn = (blockIndex % 9) < 4;

            delay.modeParam->setValueNotifyingHost(normalisedChoiceIndex(delay.modeParam, mode));
            delay.syncParam->setValueNotifyingHost(syncOn ? 1.0f : 0.0f);
            delay.syncDivisionParam->setValueNotifyingHost(normalisedChoiceIndex(delay.syncDivisionParam,
                juce::jlimit(0, 11, (int) std::floor(phase * 12.0f))));
            delay.timeParam->setValueNotifyingHost(delay.timeParam->convertTo0to1(120.0f + 1900.0f * phase));
            delay.feedbackParam->setValueNotifyingHost(delay.feedbackParam->convertTo0to1(0.18f + 0.72f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
            delay.toneParam->setValueNotifyingHost(delay.toneParam->convertTo0to1(1800.0f + 10000.0f * (1.0f - phase)));
            delay.lowCutParam->setValueNotifyingHost(delay.lowCutParam->convertTo0to1(35.0f + 900.0f * phase));
            delay.spreadParam->setValueNotifyingHost(delay.spreadParam->convertTo0to1(0.10f + 0.90f * phase));
            delay.textureParam->setValueNotifyingHost(delay.textureParam->convertTo0to1(0.15f + 0.80f * std::abs(std::cos(phase * juce::MathConstants<float>::twoPi))));
            delay.modDepthParam->setValueNotifyingHost(delay.modDepthParam->convertTo0to1(0.10f + 0.85f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
            delay.modRateParam->setValueNotifyingHost(delay.modRateParam->convertTo0to1(0.20f + 4.80f * phase));
            delay.duckParam->setValueNotifyingHost(delay.duckParam->convertTo0to1(0.85f * (1.0f - phase)));
            delay.swellParam->setValueNotifyingHost(delay.swellParam->convertTo0to1(0.80f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
            delay.reverseParam->setValueNotifyingHost(delay.reverseParam->convertTo0to1(0.75f * phase));
            delay.freezeParam->setValueNotifyingHost((blockIndex % 181) == 0 ? 1.0f : 0.0f);
            delay.setTempoSyncContext(88.0f + 72.0f * phase, true, (blockIndex % 5) != 0);

            delay.processBlock(block, midi);
            peak = juce::jmax(peak, (double) analyseBuffer(block).peak);
            finite = finite && bufferHasOnlyFiniteSamples(block);
        }

        result.metrics.push_back({ "peak", peak });
        result.metrics.push_back({ "finite", finite ? 1.0 : 0.0 });
        result.metrics.push_back({ "blocks", (double) blocksToRun });

        result.passed = finite && peak < 2.1;
        result.notes = result.passed ? "Aggressive delay automation stayed finite and inside a sane ceiling"
                                     : "Delay automation stress produced unstable or excessive output";
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

    static juce::File getRtProfileReportFile()
    {
        if (const auto overridePath = juce::SystemStats::getEnvironmentVariable("NOVA_RT_PROFILE_REPORT_PATH", {});
            overridePath.isNotEmpty())
        {
            auto file = juce::File::getCurrentWorkingDirectory().getChildFile(overridePath);
            if (juce::File::isAbsolutePath(overridePath))
                file = juce::File(overridePath);

            auto parent = file.getParentDirectory();
            if (!parent.exists())
                parent.createDirectory();

            return file;
        }

        return getReportFile().getSiblingFile("rt-profile-report.json");
    }

    static juce::String jsonQuote(const juce::String& value)
    {
        juce::String escaped;
        for (auto c : value)
        {
            if (c == '\\') escaped << "\\\\";
            else if (c == '"') escaped << "\\\"";
            else if (c == '\n') escaped << "\\n";
            else if (c == '\r') escaped << "\\r";
            else if (c == '\t') escaped << "\\t";
            else escaped << c;
        }

        return "\"" + escaped + "\"";
    }

    static void writeRtProfileReport(const std::vector<RtProfileResult>& results)
    {
        auto reportFile = getRtProfileReportFile();
        if (reportFile.existsAsFile())
            reportFile.deleteFile();

        auto stream = reportFile.createOutputStream();
        if (stream == nullptr)
            return;

        int passCount = 0;
        int warnCount = 0;
        int failCount = 0;
        for (const auto& result : results)
        {
            if (result.status == "PASS") ++passCount;
            else if (result.status == "WARN") ++warnCount;
            else ++failCount;
        }

        juce::String text;
        text << "{" << juce::newLine;
        text << "  \"generatedAt\": " << jsonQuote(juce::Time::getCurrentTime().toISO8601(true)) << "," << juce::newLine;
        text << "  \"kind\": \"p4b_rt_profile_baseline\"," << juce::newLine;
        text << "  \"summary\": {" << juce::newLine;
        text << "    \"total\": " << (int)results.size() << "," << juce::newLine;
        text << "    \"pass\": " << passCount << "," << juce::newLine;
        text << "    \"warn\": " << warnCount << "," << juce::newLine;
        text << "    \"fail\": " << failCount << juce::newLine;
        text << "  }," << juce::newLine;
        text << "  \"scenarios\": [" << juce::newLine;

        for (size_t i = 0; i < results.size(); ++i)
        {
            const auto& r = results[i];
            text << "    {" << juce::newLine;
            text << "      \"name\": " << jsonQuote(r.name) << "," << juce::newLine;
            text << "      \"status\": " << jsonQuote(r.status) << "," << juce::newLine;
            text << "      \"sampleRate\": " << juce::String(r.sampleRate, 2) << "," << juce::newLine;
            text << "      \"blockSize\": " << r.blockSize << "," << juce::newLine;
            text << "      \"processedBlocks\": " << r.processedBlocks << "," << juce::newLine;
            text << "      \"avgProcessMs\": " << juce::String(r.avgProcessMs, 8) << "," << juce::newLine;
            text << "      \"peakProcessMs\": " << juce::String(r.peakProcessMs, 8) << "," << juce::newLine;
            text << "      \"cpuAvgPercent\": " << juce::String(r.cpuAvgPercent, 8) << "," << juce::newLine;
            text << "      \"cpuPeakPercent\": " << juce::String(r.cpuPeakPercent, 8) << "," << juce::newLine;
            text << "      \"maxBudgetRatio\": " << juce::String(r.maxBudgetRatio, 8) << "," << juce::newLine;
            text << "      \"blocksOver50\": " << r.blocksOver50 << "," << juce::newLine;
            text << "      \"blocksOver75\": " << r.blocksOver75 << "," << juce::newLine;
            text << "      \"blocksOver90\": " << r.blocksOver90 << "," << juce::newLine;
            text << "      \"blocksOver100\": " << r.blocksOver100 << "," << juce::newLine;
            text << "      \"invalidSamples\": " << r.invalidSamples << "," << juce::newLine;
            text << "      \"clippedSamples\": " << r.clippedSamples << "," << juce::newLine;
            text << "      \"nearClipSamples\": " << r.nearClipSamples << "," << juce::newLine;
            text << "      \"denormalLikeSamples\": " << r.denormalLikeSamples << "," << juce::newLine;
            text << "      \"fallbackBlockCount\": " << r.fallbackBlockCount << "," << juce::newLine;
            text << "      \"limiterTouchedSamples\": " << r.limiterTouchedSamples << "," << juce::newLine;
            text << "      \"limiterMaxReductionDb\": " << juce::String(r.limiterMaxReductionDb, 8) << "," << juce::newLine;
            text << "      \"inputPeak\": " << juce::String(r.inputPeak, 8) << "," << juce::newLine;
            text << "      \"outputPeak\": " << juce::String(r.outputPeak, 8) << "," << juce::newLine;
            text << "      \"warnings\": " << jsonQuote(r.warnings) << juce::newLine;
            text << "    }" << (i + 1 < results.size() ? "," : "") << juce::newLine;
        }

        text << "  ]" << juce::newLine;
        text << "}" << juce::newLine;

        stream->writeText(text, false, false, "\n");
        stream->flush();

        SessionLogger::logEvent("rt.profile",
            "RT profile report written to " + reportFile.getFullPathName()
            + juce::newLine + "pass=" + juce::String(passCount)
            + ", warn=" + juce::String(warnCount)
            + ", fail=" + juce::String(failCount));
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
