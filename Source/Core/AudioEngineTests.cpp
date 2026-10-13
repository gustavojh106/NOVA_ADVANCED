#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

#include "AudioEngine.h"
#include "Audio/AudioAutopilot.h"
#include "Audio/CpuMeter.h"
#include "Audio/DiagnosticsManager.h"
#include "Audio/DryWetMixer.h"
#include "Audio/GraphBuilder.h"
#include "Audio/GraphRetirementQueue.h"
#include "Audio/RuntimeGraphManager.h"
#include "Audio/RoutingMixer.h"
#include "NovaSettingsService.h"
#include "PluginProcessor.h"
#include "PluginStateModel.h"
#include "PedalRegistry.h"
#include "SessionPersistence.h"
#include "SessionStore.h"
#include "DSP/Global/ChannelStrip.h"
#include "DSP/Global/InputChain.h"
#include "DSP/Global/OutputChain.h"
#include "DSP/Services/TunerService.h"
#include "../Effects/Cabinets/SyntheticIR.h"
#include "../Effects/Pedals/Base/NovaPedalProcessor.h"

#ifndef NOVA_ENABLE_RELEASE_DIAGNOSTICS
#define NOVA_ENABLE_RELEASE_DIAGNOSTICS 0
#endif

#define NOVA_P14B_MEASUREMENT_HARNESS_ENABLED (JUCE_DEBUG || NOVA_ENABLE_RELEASE_DIAGNOSTICS)

struct AudioEngineTestAccess
{
    static uint32_t getRuntimeParamRevision(const AudioEngine& engine) noexcept
    {
        return engine.params.getRevision();
    }

    static uint32_t getAppliedParamRevision(const AudioEngine& engine) noexcept
    {
        const auto active = engine.runtimeGraphs.getActiveOwnerForControl();
        return active != nullptr
            ? active->appliedParamRevision.load(std::memory_order_relaxed)
            : 0;
    }

    static uint64_t getActiveGraphGeneration(const AudioEngine& engine) noexcept
    {
        const auto active = engine.runtimeGraphs.getActiveOwnerForControl();
        return active != nullptr ? active->generation : 0;
    }

    static std::weak_ptr<void> getRetainedRuntimeWeak(
        const AudioEngine::PedalProcessorHandle& handle) noexcept
    {
        return handle.runtimeOwner;
    }

    static void dispatchPendingMessageThreadWork(AudioEngine& engine)
    {
        engine.handleAsyncUpdate();
    }

    static void forceRetiredGraphCleanup(AudioEngine& engine)
    {
        engine.audioPlane.audioBlockCounter.store(
            std::numeric_limits<uint64_t>::max(),
            std::memory_order_release);
        engine.cleanupRetiredGraphs();
    }
};

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr float kTolerance = 1.0e-4f;

bool approximatelyEqual(float actual, float expected, float tolerance = kTolerance)
{
    return std::abs(actual - expected) <= tolerance;
}

bool bufferHasOnlyFiniteSamples(const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (!std::isfinite(buffer.getSample(ch, i)))
                return false;

    return true;
}

double computeWindowRms(const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
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

double computeBufferPeak(const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
    const int safeLength = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
    if (safeLength <= 0)
        return 0.0;

    double peak = 0.0;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = juce::jmax(peak, (double)buffer.getMagnitude(ch, safeStart, safeLength));

    return peak;
}

double computeChannelWindowRms(const juce::AudioBuffer<float>& buffer, int channel, int startSample, int numSamples)
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

double computeChannelMean(const juce::AudioBuffer<float>& buffer, int channel, int startSample, int numSamples)
{
    if (!juce::isPositiveAndBelow(channel, buffer.getNumChannels()))
        return 0.0;

    const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
    const int safeLength = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
    if (safeLength <= 0)
        return 0.0;

    double sum = 0.0;
    for (int i = 0; i < safeLength; ++i)
        sum += (double) buffer.getSample(channel, safeStart + i);

    return sum / (double) safeLength;
}

struct P8AWindowMetrics
{
    double sumSquares = 0.0;
    double sum = 0.0;
    double peak = 0.0;
    int sampleCount = 0;
    int nearClipSamples = 0;
    int clippedSamples = 0;
    bool finite = true;

    void capture(const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* data = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float sample = data[i];
                if (!std::isfinite(sample))
                {
                    finite = false;
                    continue;
                }

                const double value = (double) sample;
                const double absValue = std::abs(value);
                sumSquares += value * value;
                sum += value;
                peak = juce::jmax(peak, absValue);
                ++sampleCount;

                if (absValue >= (double) Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD)
                    ++nearClipSamples;
                if (absValue > 1.0)
                    ++clippedSamples;
            }
        }
    }

    double rms() const noexcept
    {
        return sampleCount > 0 ? std::sqrt(sumSquares / (double) sampleCount) : 0.0;
    }

    double dc() const noexcept
    {
        return sampleCount > 0 ? std::abs(sum / (double) sampleCount) : 0.0;
    }
};

struct P10CWindowMetrics
{
    double sumSquares = 0.0;
    double highDeltaSumSquares = 0.0;
    double lowSumSquares = 0.0;
    double sum = 0.0;
    double peak = 0.0;
    double adjacentDeltaPeak = 0.0;
    double previousSample = 0.0;
    int sampleCount = 0;
    int invalidSamples = 0;
    int nearClipSamples = 0;
    int clippedSamples = 0;
    bool havePrevious = false;
    bool finite = true;
    std::array<double, 2> lowState { 0.0, 0.0 };

    void capture(const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* data = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float sample = data[i];
                if (!std::isfinite(sample))
                {
                    finite = false;
                    ++invalidSamples;
                    continue;
                }

                const double value = (double) sample;
                const double absValue = std::abs(value);
                sumSquares += value * value;
                sum += value;
                peak = juce::jmax(peak, absValue);
                ++sampleCount;

                if (havePrevious)
                {
                    const double delta = value - previousSample;
                    highDeltaSumSquares += delta * delta;
                    adjacentDeltaPeak = juce::jmax(adjacentDeltaPeak, std::abs(value - previousSample));
                }
                previousSample = value;
                havePrevious = true;

                const size_t lowIndex = (size_t) juce::jlimit(0, 1, ch);
                lowState[lowIndex] += 0.015 * (value - lowState[lowIndex]);
                lowSumSquares += lowState[lowIndex] * lowState[lowIndex];

                if (absValue >= (double) Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD)
                    ++nearClipSamples;
                if (absValue > 1.0)
                    ++clippedSamples;
            }
        }
    }

    double rms() const noexcept
    {
        return sampleCount > 0 ? std::sqrt(sumSquares / (double) sampleCount) : 0.0;
    }

    double dc() const noexcept
    {
        return sampleCount > 0 ? std::abs(sum / (double) sampleCount) : 0.0;
    }

    double brightnessProxy() const noexcept
    {
        const double base = rms();
        return sampleCount > 0 && base > 1.0e-9
            ? std::sqrt(highDeltaSumSquares / (double) sampleCount) / base
            : 0.0;
    }

    double rumbleProxy() const noexcept
    {
        const double base = rms();
        return sampleCount > 0 && base > 1.0e-9
            ? std::sqrt(lowSumSquares / (double) sampleCount) / base
            : 0.0;
    }
};

juce::String p10cMetricsSummary(const P10CWindowMetrics& metrics)
{
    return "peak="
        + juce::String(metrics.peak, 4)
        + ", rms="
        + juce::String(metrics.rms(), 4)
        + ", dc="
        + juce::String(metrics.dc(), 5)
        + ", nearClipSamples="
        + juce::String(metrics.nearClipSamples)
        + ", clippedSamples="
        + juce::String(metrics.clippedSamples)
        + ", invalidSamples="
        + juce::String(metrics.invalidSamples)
        + ", adjacentDeltaPeak="
        + juce::String(metrics.adjacentDeltaPeak, 4)
        + ", brightnessProxy="
        + juce::String(metrics.brightnessProxy(), 4)
        + ", rumbleProxy="
        + juce::String(metrics.rumbleProxy(), 4);
}

juce::String p8aMetricsSummary(const P8AWindowMetrics& metrics)
{
    return "peak="
        + juce::String(metrics.peak, 4)
        + ", rms="
        + juce::String(metrics.rms(), 4)
        + ", dc="
        + juce::String(metrics.dc(), 5)
        + ", nearClipSamples="
        + juce::String(metrics.nearClipSamples)
        + ", clippedSamples="
        + juce::String(metrics.clippedSamples);
}

double computeStereoCorrelation(const juce::AudioBuffer<float>& buffer, int startSample)
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

double computeBufferNullRms(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    const int channels = juce::jmin(a.getNumChannels(), b.getNumChannels());
    const int samples = juce::jmin(a.getNumSamples(), b.getNumSamples());
    if (channels <= 0 || samples <= 0)
        return 0.0;

    double sumSquares = 0.0;
    int count = 0;

    for (int ch = 0; ch < channels; ++ch)
    {
        for (int i = 0; i < samples; ++i)
        {
            const double diff = (double)a.getSample(ch, i) - (double)b.getSample(ch, i);
            sumSquares += diff * diff;
            ++count;
        }
    }

    return count > 0 ? std::sqrt(sumSquares / (double)count) : 0.0;
}

double computeAdjacentDeltaPeakRange(const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
    const int safeLength = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
    if (safeLength <= 1)
        return 0.0;

    double peak = 0.0;
    const int endSample = safeStart + safeLength;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        for (int i = safeStart + 1; i < endSample; ++i)
        {
            const double delta = std::abs((double)buffer.getSample(ch, i)
                - (double)buffer.getSample(ch, i - 1));
            peak = juce::jmax(peak, delta);
        }
    }

    return peak;
}

double computeFrequencyMagnitude(const juce::AudioBuffer<float>& buffer,
    double sampleRate,
    float targetFreq,
    int startSample,
    int numSamples)
{
    const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
    const int safeLength = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
    if (safeLength <= 0 || sampleRate <= 0.0 || targetFreq <= 0.0f)
        return 0.0;

    const double omega = 2.0 * juce::MathConstants<double>::pi * (double) targetFreq / sampleRate;
    const double coeff = 2.0 * std::cos(omega);
    const double sine = std::sin(omega);
    const double cosine = std::cos(omega);

    double magnitude = 0.0;
    const int channels = juce::jmax(1, buffer.getNumChannels());

    for (int ch = 0; ch < channels; ++ch)
    {
        double q0 = 0.0, q1 = 0.0, q2 = 0.0;

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

juce::AudioBuffer<float> renderReverbOutput(ReverbPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderDelayOutput(DelayPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderChorusOutput(ChorusPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderFlangerOutput(FlangerPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderDistortionOutput(DistortionPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderOverdriveOutput(OverdrivePedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderNeuralOutput(NeuralPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderBoostOutput(BoostPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderOctaveOutput(OctavePedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderPhaserOutput(PhaserPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderFuzzOutput(FuzzPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderCompressorOutput(CompressorPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderEQOutput(EQPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderTremoloOutput(TremoloPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

juce::AudioBuffer<float> renderNoiseGateOutput(NoiseGatePedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

template <typename Callback>
juce::AudioBuffer<float> renderReverbOutputWithAutomation(ReverbPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize,
    Callback&& callback)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    int blockIndex = 0;
    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize, ++blockIndex)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        callback(blockIndex, offset, block);
        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

template <typename Callback>
juce::AudioBuffer<float> renderDelayOutputWithAutomation(DelayPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize,
    Callback&& callback)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    int blockIndex = 0;
    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize, ++blockIndex)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        callback(blockIndex, offset, block);
        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

template <typename Callback>
juce::AudioBuffer<float> renderChorusOutputWithAutomation(ChorusPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize,
    Callback&& callback)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    int blockIndex = 0;
    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize, ++blockIndex)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        callback(blockIndex, offset, block);
        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

template <typename Callback>
juce::AudioBuffer<float> renderFlangerOutputWithAutomation(FlangerPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize,
    Callback&& callback)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    int blockIndex = 0;
    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize, ++blockIndex)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        callback(blockIndex, offset, block);
        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

template <typename Callback>
juce::AudioBuffer<float> renderDistortionOutputWithAutomation(DistortionPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize,
    Callback&& callback)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    int blockIndex = 0;
    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize, ++blockIndex)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        callback(blockIndex, offset, block);
        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

template <typename Callback>
juce::AudioBuffer<float> renderFuzzOutputWithAutomation(FuzzPedal& pedal,
    const juce::AudioBuffer<float>& input,
    int blockSize,
    Callback&& callback)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    int blockIndex = 0;
    for (int offset = 0; offset < input.getNumSamples(); offset += blockSize, ++blockIndex)
    {
        const int numSamples = juce::jmin(blockSize, input.getNumSamples() - offset);
        juce::AudioBuffer<float> block(input.getNumChannels(), blockSize);
        block.clear();

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, numSamples);

        callback(blockIndex, offset, block);
        pedal.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, numSamples);
    }

    return output;
}

float normalisedChoiceIndex(const juce::AudioParameterChoice* param, int index)
{
    if (param == nullptr || param->choices.size() <= 1)
        return 0.0f;

    const int clamped = juce::jlimit(0, param->choices.size() - 1, index);
    return (float) clamped / (float) (param->choices.size() - 1);
}

bool setRangedParamById(juce::AudioProcessor& processor, const juce::String& paramId, float plainValue)
{
    for (auto* param : processor.getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
        {
            if (ranged->getParameterID() == paramId)
            {
                ranged->setValueNotifyingHost(ranged->convertTo0to1(plainValue));
                return true;
            }
        }
    }

    return false;
}

#if NOVA_P14B_MEASUREMENT_HARNESS_ENABLED
namespace P14BMeasurement
{
constexpr int kRenderSamples = kBlockSize * 160;
constexpr int kMeasurementStart = kBlockSize * 32;
constexpr int kSweepSteps = 9;
constexpr uint32_t kNoiseSeed = 0x14B10u;

struct Metrics
{
    double rms = 0.0;
    double peak = 0.0;
    double rmsDbfs = -180.0;
    double peakDbfs = -180.0;
    bool finite = true;
};

struct ReferenceResult
{
    juce::String pedal;
    juce::String signal;
    juce::String referenceSetting;
    Metrics input;
    Metrics bypass;
    Metrics engaged;
    double nullRmsDbfs = -180.0;
    double engagedMinusBypassDb = 0.0;
    bool finite = true;
};

struct SweepStep
{
    float value = 0.0f;
    Metrics output;
};

struct SweepResult
{
    juce::String pedal;
    juce::String controlId;
    float minimum = 0.0f;
    float maximum = 0.0f;
    std::vector<SweepStep> steps;
    double maxAdjacentJumpDb = 0.0;
    bool finite = true;
};

struct SpectralResult
{
    juce::String pedal;
    double sineFrequencyHz = 0.0;
    juce::String method;
    Metrics output;
    double fundamentalDbfs = -180.0;
    double trackedHarmonicDbfs = -180.0;
    double inharmonicResidualRatioDb = -180.0;
    bool robustFftAvailable = false;
    bool finite = true;
};

struct GateProbeResult
{
    double openAttenuationDb = 0.0;
    double closedAttenuationDb = 0.0;
    int observedLookaheadSamples = 0;
    Metrics output;
    bool finite = true;
};

double linearToDbfs(double value)
{
    if (!std::isfinite(value) || value <= 0.0)
        return -180.0;

    return juce::jlimit(-180.0, 60.0,
        (double) juce::Decibels::gainToDecibels((float) value, -180.0f));
}

double dbDifference(double numerator, double denominator)
{
    return linearToDbfs(numerator) - linearToDbfs(denominator);
}

Metrics measure(const juce::AudioBuffer<float>& buffer,
    int startSample = kMeasurementStart,
    int numSamples = kRenderSamples - kMeasurementStart)
{
    Metrics metrics;
    metrics.rms = computeWindowRms(buffer, startSample, numSamples);
    metrics.peak = computeBufferPeak(buffer, startSample, numSamples);
    metrics.rmsDbfs = linearToDbfs(metrics.rms);
    metrics.peakDbfs = linearToDbfs(metrics.peak);
    metrics.finite = bufferHasOnlyFiniteSamples(buffer)
        && std::isfinite(metrics.rms)
        && std::isfinite(metrics.peak)
        && std::isfinite(metrics.rmsDbfs)
        && std::isfinite(metrics.peakDbfs);
    return metrics;
}

double computeWindowNullRms(const juce::AudioBuffer<float>& a,
    const juce::AudioBuffer<float>& b,
    int startSample = kMeasurementStart,
    int numSamples = kRenderSamples - kMeasurementStart)
{
    const int channels = juce::jmin(a.getNumChannels(), b.getNumChannels());
    const int safeStart = juce::jlimit(0,
        juce::jmin(a.getNumSamples(), b.getNumSamples()),
        startSample);
    const int safeLength = juce::jlimit(0,
        juce::jmin(a.getNumSamples(), b.getNumSamples()) - safeStart,
        numSamples);

    if (channels <= 0 || safeLength <= 0)
        return 0.0;

    double sumSquares = 0.0;
    int count = 0;

    for (int ch = 0; ch < channels; ++ch)
    {
        for (int sample = 0; sample < safeLength; ++sample)
        {
            const double diff = (double) a.getSample(ch, safeStart + sample)
                - (double) b.getSample(ch, safeStart + sample);
            sumSquares += diff * diff;
            ++count;
        }
    }

    return count > 0 ? std::sqrt(sumSquares / (double) count) : 0.0;
}

void fillSine(juce::AudioBuffer<float>& buffer, double frequencyHz, float amplitude)
{
    buffer.clear();

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const float channelTrim = ch == 0 ? 1.0f : 0.997f;
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const double phase = juce::MathConstants<double>::twoPi
                * frequencyHz
                * (double) sample
                / kSampleRate;
            buffer.setSample(ch, sample, amplitude * channelTrim * (float) std::sin(phase));
        }
    }
}

void fillDeterministicNoise(juce::AudioBuffer<float>& buffer, uint32_t seed, float amplitude)
{
    juce::Random rng((int64_t) seed);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample(ch, sample, amplitude * ((rng.nextFloat() * 2.0f) - 1.0f));
}

juce::AudioBuffer<float> createReferenceSignal()
{
    juce::AudioBuffer<float> buffer(2, kRenderSamples);
    juce::Random rng((int64_t) kNoiseSeed);

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const double t = (double) sample / kSampleRate;
        const float envelope = sample < kBlockSize * 6
            ? (float) sample / (float) (kBlockSize * 6)
            : 1.0f;
        const float deterministicNoise = 0.0015f * ((rng.nextFloat() * 2.0f) - 1.0f);
        const float source = envelope
            * (0.083f * (float) std::sin(juce::MathConstants<double>::twoPi * 110.0 * t)
                + 0.034f * (float) std::sin(juce::MathConstants<double>::twoPi * 220.0 * t)
                + 0.014f * (float) std::sin(juce::MathConstants<double>::twoPi * 1000.0 * t)
                + deterministicNoise);

        buffer.setSample(0, sample, source);
        buffer.setSample(1, sample, source * 0.985f);
    }

    return buffer;
}

juce::AudioBuffer<float> createSineSignal(double frequencyHz)
{
    juce::AudioBuffer<float> buffer(2, kRenderSamples);
    fillSine(buffer, frequencyHz, frequencyHz >= 5000.0 ? 0.080f : 0.120f);
    return buffer;
}

void prepare(juce::AudioProcessor& processor)
{
    processor.setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
    processor.prepareToPlay(kSampleRate, kBlockSize);
}

juce::AudioBuffer<float> render(juce::AudioProcessor& processor,
    const juce::AudioBuffer<float>& input)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
    output.clear();

    for (int offset = 0; offset < input.getNumSamples(); offset += kBlockSize)
    {
        juce::AudioBuffer<float> block(input.getNumChannels(), kBlockSize);
        block.clear();

        const int samplesThisBlock = juce::jmin(kBlockSize, input.getNumSamples() - offset);
        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            block.copyFrom(ch, 0, input, ch, offset, samplesThisBlock);

        processor.processBlock(block, midi);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            output.copyFrom(ch, offset, block, ch, 0, samplesThisBlock);
    }

    return output;
}

juce::String referenceSettingFor(const juce::String& pedal)
{
    if (pedal == "Boost")
        return "boostGain=0, boostTone=0.58, boostTight=0.24, boostChar=0, boostMid=0, boostLevel=1";
    if (pedal == "Compressor")
        return "compThreshold=-20 dB, compRatio=4:1, compAttack=12 ms, compRelease=120 ms, compKnee=6 dB, compFocus=0.55, compBlend=0.82, compMakeup=2.5 dB";
    if (pedal == "Noise Gate")
        return "gateThreshold=-46 dB, gateAttack=0.25 ms, gateHold=70 ms, gateRelease=115 ms, gateRange=-96 dB, gateHysteresis=8 dB, gateFocus=0.55";
    if (pedal == "Overdrive")
        return "drive=30, tone=0.58, texture=0.42, mix=1, level=0.74";
    if (pedal == "Distortion")
        return "distGain=58, distTone=0.54, distBody=0.52, distMix=1, distLevel=0.64, distTight=0.42, distMode=Vintage";
    if (pedal == "Fuzz")
        return "fuzzAmount=67, fuzzTone=0.48, fuzzGate=0.28, fuzzMix=1, fuzzLevel=0.58, fuzzBias=0.56, fuzzMode=Vintage";
    if (pedal == "Neural")
        return "neuralDrive=54, neuralFocus=0.58, neuralDetail=0.54, neuralComp=0.42, neuralMix=1, neuralLevel=0.75";

    return {};
}

void configureReference(juce::AudioProcessor& processor, const juce::String& pedal)
{
    if (pedal == "Boost")
    {
        setRangedParamById(processor, "boostGain", 0.0f);
        setRangedParamById(processor, "boostTone", 0.58f);
        setRangedParamById(processor, "boostTight", 0.24f);
        setRangedParamById(processor, "boostChar", 0.0f);
        setRangedParamById(processor, "boostMid", 0.0f);
        setRangedParamById(processor, "boostLevel", 1.0f);
        return;
    }

    if (pedal == "Compressor")
    {
        setRangedParamById(processor, "compThreshold", -20.0f);
        setRangedParamById(processor, "compRatio", 4.0f);
        setRangedParamById(processor, "compAttack", 12.0f);
        setRangedParamById(processor, "compRelease", 120.0f);
        setRangedParamById(processor, "compKnee", 6.0f);
        setRangedParamById(processor, "compFocus", 0.55f);
        setRangedParamById(processor, "compBlend", 0.82f);
        setRangedParamById(processor, "compMakeup", 2.5f);
        return;
    }

    if (pedal == "Noise Gate")
    {
        setRangedParamById(processor, "gateThreshold", -46.0f);
        setRangedParamById(processor, "gateAttack", 0.25f);
        setRangedParamById(processor, "gateHold", 70.0f);
        setRangedParamById(processor, "gateRelease", 115.0f);
        setRangedParamById(processor, "gateRange", -96.0f);
        setRangedParamById(processor, "gateHysteresis", 8.0f);
        setRangedParamById(processor, "gateFocus", 0.55f);
        return;
    }

    if (pedal == "Overdrive")
    {
        setRangedParamById(processor, "drive", 30.0f);
        setRangedParamById(processor, "tone", 0.58f);
        setRangedParamById(processor, "texture", 0.42f);
        setRangedParamById(processor, "mix", 1.0f);
        setRangedParamById(processor, "level", 0.74f);
        return;
    }

    if (pedal == "Distortion")
    {
        setRangedParamById(processor, "distGain", 58.0f);
        setRangedParamById(processor, "distTone", 0.54f);
        setRangedParamById(processor, "distBody", 0.52f);
        setRangedParamById(processor, "distMix", 1.0f);
        setRangedParamById(processor, "distLevel", 0.64f);
        setRangedParamById(processor, "distTight", 0.42f);
        setRangedParamById(processor, "distMode", 0.0f);
        return;
    }

    if (pedal == "Fuzz")
    {
        setRangedParamById(processor, "fuzzAmount", 67.0f);
        setRangedParamById(processor, "fuzzTone", 0.48f);
        setRangedParamById(processor, "fuzzGate", 0.28f);
        setRangedParamById(processor, "fuzzMix", 1.0f);
        setRangedParamById(processor, "fuzzLevel", 0.58f);
        setRangedParamById(processor, "fuzzBias", 0.56f);
        setRangedParamById(processor, "fuzzMode", 0.0f);
        return;
    }

    if (pedal == "Neural")
    {
        setRangedParamById(processor, "neuralDrive", 54.0f);
        setRangedParamById(processor, "neuralFocus", 0.58f);
        setRangedParamById(processor, "neuralDetail", 0.54f);
        setRangedParamById(processor, "neuralComp", 0.42f);
        setRangedParamById(processor, "neuralMix", 1.0f);
        setRangedParamById(processor, "neuralLevel", 0.75f);
    }
}

std::vector<juce::String> targetPedals()
{
    return { "Boost", "Compressor", "Noise Gate", "Overdrive", "Distortion", "Fuzz", "Neural" };
}

struct SweepSpec
{
    juce::String pedal;
    juce::String controlId;
    float minimum = 0.0f;
    float maximum = 1.0f;
};

std::vector<SweepSpec> sweepSpecs()
{
    return {
        { "Boost", "boostGain", 0.0f, 24.0f },
        { "Boost", "boostLevel", 0.5f, 2.0f },
        { "Compressor", "compMakeup", 0.0f, 18.0f },
        { "Compressor", "compBlend", 0.0f, 1.0f },
        { "Noise Gate", "gateThreshold", -80.0f, 0.0f },
        { "Noise Gate", "gateRange", -96.0f, 0.0f },
        { "Overdrive", "drive", 0.0f, 100.0f },
        { "Overdrive", "level", 0.0f, 1.0f },
        { "Overdrive", "mix", 0.0f, 1.0f },
        { "Distortion", "distGain", 0.0f, 100.0f },
        { "Distortion", "distLevel", 0.0f, 1.0f },
        { "Distortion", "distMix", 0.0f, 1.0f },
        { "Fuzz", "fuzzAmount", 0.0f, 100.0f },
        { "Fuzz", "fuzzLevel", 0.0f, 1.0f },
        { "Fuzz", "fuzzMix", 0.0f, 1.0f },
        { "Neural", "neuralDrive", 0.0f, 100.0f },
        { "Neural", "neuralLevel", 0.0f, 1.0f },
        { "Neural", "neuralMix", 0.0f, 1.0f }
    };
}

ReferenceResult renderReference(const juce::String& pedal)
{
    ReferenceResult result;
    result.pedal = pedal;
    result.signal = "p14b_reference_program_fixed_seed_0x14B10";
    result.referenceSetting = referenceSettingFor(pedal);

    const auto input = createReferenceSignal();
    result.input = measure(input);

    auto bypassed = PedalRegistry::createPedal(pedal);
    auto engaged = PedalRegistry::createPedal(pedal);
    if (bypassed == nullptr || engaged == nullptr)
    {
        result.finite = false;
        return result;
    }

    configureReference(*bypassed, pedal);
    if (auto* base = dynamic_cast<ProcessorBase*>(bypassed.get()))
        base->setBypassed(true);
    prepare(*bypassed);
    const auto bypassOutput = render(*bypassed, input);
    result.bypass = measure(bypassOutput);

    configureReference(*engaged, pedal);
    prepare(*engaged);
    const auto engagedOutput = render(*engaged, input);
    result.engaged = measure(engagedOutput);

    result.nullRmsDbfs = linearToDbfs(computeWindowNullRms(engagedOutput, bypassOutput));
    result.engagedMinusBypassDb = dbDifference(result.engaged.rms, result.bypass.rms);
    result.finite = result.input.finite && result.bypass.finite && result.engaged.finite
        && std::isfinite(result.nullRmsDbfs)
        && std::isfinite(result.engagedMinusBypassDb);
    return result;
}

SweepResult renderSweep(const SweepSpec& spec)
{
    SweepResult result;
    result.pedal = spec.pedal;
    result.controlId = spec.controlId;
    result.minimum = spec.minimum;
    result.maximum = spec.maximum;
    result.steps.reserve((size_t) kSweepSteps);

    const auto input = createReferenceSignal();
    double previousRmsDbfs = 0.0;
    bool havePrevious = false;

    for (int step = 0; step < kSweepSteps; ++step)
    {
        const float t = kSweepSteps <= 1 ? 0.0f : (float) step / (float) (kSweepSteps - 1);
        const float value = spec.minimum + t * (spec.maximum - spec.minimum);

        auto processor = PedalRegistry::createPedal(spec.pedal);
        SweepStep sweepStep;
        sweepStep.value = value;

        if (processor == nullptr)
        {
            result.finite = false;
            result.steps.push_back(sweepStep);
            continue;
        }

        configureReference(*processor, spec.pedal);
        if (!setRangedParamById(*processor, spec.controlId, value))
            result.finite = false;
        prepare(*processor);

        const auto output = render(*processor, input);
        sweepStep.output = measure(output);
        result.finite = result.finite && sweepStep.output.finite;

        if (havePrevious)
            result.maxAdjacentJumpDb = juce::jmax(result.maxAdjacentJumpDb,
                std::abs(sweepStep.output.rmsDbfs - previousRmsDbfs));

        previousRmsDbfs = sweepStep.output.rmsDbfs;
        havePrevious = true;
        result.steps.push_back(sweepStep);
    }

    return result;
}

SpectralResult renderSpectral(const juce::String& pedal, double frequencyHz)
{
    SpectralResult result;
    result.pedal = pedal;
    result.sineFrequencyHz = frequencyHz;
    result.method = "goertzel_tracked_harmonic_proxy_no_fft_no_aliasing_claim";

    const auto input = createSineSignal(frequencyHz);
    auto processor = PedalRegistry::createPedal(pedal);
    if (processor == nullptr)
    {
        result.finite = false;
        return result;
    }

    configureReference(*processor, pedal);
    prepare(*processor);
    const auto output = render(*processor, input);

    result.output = measure(output);
    const int measureSamples = kRenderSamples - kMeasurementStart;
    const double fundamentalMagnitude = computeFrequencyMagnitude(output,
        kSampleRate,
        (float) frequencyHz,
        kMeasurementStart,
        measureSamples);

    double harmonicPower = 0.0;
    for (int harmonic = 2; harmonic <= 10; ++harmonic)
    {
        const double harmonicHz = frequencyHz * (double) harmonic;
        if (harmonicHz >= kSampleRate * 0.45)
            break;

        const double magnitude = computeFrequencyMagnitude(output,
            kSampleRate,
            (float) harmonicHz,
            kMeasurementStart,
            measureSamples);
        harmonicPower += magnitude * magnitude;
    }

    const double totalPower = result.output.rms * result.output.rms;
    const double trackedPower = fundamentalMagnitude * fundamentalMagnitude + harmonicPower;
    const double residualRatio = totalPower > 1.0e-16
        ? juce::jlimit(0.0, 1.0e6, (totalPower - juce::jmin(totalPower, trackedPower)) / totalPower)
        : 0.0;

    result.fundamentalDbfs = linearToDbfs(fundamentalMagnitude);
    result.trackedHarmonicDbfs = linearToDbfs(std::sqrt(harmonicPower));
    result.inharmonicResidualRatioDb = linearToDbfs(std::sqrt(residualRatio));
    result.finite = result.output.finite
        && std::isfinite(result.fundamentalDbfs)
        && std::isfinite(result.trackedHarmonicDbfs)
        && std::isfinite(result.inharmonicResidualRatioDb);
    return result;
}

std::vector<ReferenceResult> buildReferenceBaselines()
{
    std::vector<ReferenceResult> results;
    for (const auto& pedal : targetPedals())
        results.push_back(renderReference(pedal));
    return results;
}

std::vector<SweepResult> buildSweepBaselines()
{
    std::vector<SweepResult> results;
    for (const auto& spec : sweepSpecs())
        results.push_back(renderSweep(spec));
    return results;
}

std::vector<SpectralResult> buildSpectralBaselines()
{
    std::vector<SpectralResult> results;
    for (const auto& pedal : { juce::String("Overdrive"), juce::String("Distortion"), juce::String("Fuzz"), juce::String("Neural") })
    {
        results.push_back(renderSpectral(pedal, 1000.0));
        results.push_back(renderSpectral(pedal, 5000.0));
    }
    return results;
}

int findFirstAbove(const juce::AudioBuffer<float>& buffer, float threshold)
{
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            if (std::abs(buffer.getSample(ch, sample)) >= threshold)
                return sample;

    return -1;
}

GateProbeResult runGateProbe()
{
    constexpr int probeSamples = kBlockSize * 220;
    constexpr int activeStart = kBlockSize * 32;
    constexpr int activeEnd = kBlockSize * 128;

    juce::AudioBuffer<float> input(2, probeSamples);
    input.clear();

    for (int sample = activeStart; sample < activeEnd; ++sample)
    {
        const int local = sample - activeStart;
        const float ramp = juce::jlimit(0.0f, 1.0f, (float) local / (float) (kBlockSize * 8));
        const double phase = juce::MathConstants<double>::twoPi * 1000.0 * (double) sample / kSampleRate;
        const float value = 0.16f * ramp * (float) std::sin(phase);
        input.setSample(0, sample, value);
        input.setSample(1, sample, value * 0.98f);
    }

    NoiseGatePedal gate;
    configureReference(gate, "Noise Gate");
    setRangedParamById(gate, "gateThreshold", -48.0f);
    setRangedParamById(gate, "gateRange", -72.0f);
    setRangedParamById(gate, "gateAttack", 0.20f);
    setRangedParamById(gate, "gateHold", 60.0f);
    setRangedParamById(gate, "gateRelease", 95.0f);
    prepare(gate);

    const auto output = render(gate, input);

    GateProbeResult result;
    result.output = measure(output, 0, output.getNumSamples());
    result.finite = result.output.finite;

    const int firstInput = findFirstAbove(input, 0.025f);
    const int firstOutput = findFirstAbove(output, 0.025f);
    result.observedLookaheadSamples = firstInput >= 0 && firstOutput >= 0 ? firstOutput - firstInput : -1;

    const int lookahead = juce::jlimit(0, 256, result.observedLookaheadSamples);
    const int openStart = activeStart + lookahead + kBlockSize * 20;
    const int openLength = kBlockSize * 48;
    const int closeStart = activeEnd + lookahead + kBlockSize * 56;
    const int closeLength = kBlockSize * 32;

    const double activeInputRms = computeWindowRms(input, activeStart + kBlockSize * 20, openLength);
    const double activeOutputRms = computeWindowRms(output, openStart, openLength);
    const double closedOutputRms = computeWindowRms(output, closeStart, closeLength);

    result.openAttenuationDb = dbDifference(activeOutputRms, activeInputRms);
    result.closedAttenuationDb = dbDifference(closedOutputRms, activeInputRms);
    result.finite = result.finite
        && std::isfinite(result.openAttenuationDb)
        && std::isfinite(result.closedAttenuationDb)
        && result.observedLookaheadSamples >= 0;
    return result;
}

juce::String jsonEscape(juce::String text)
{
    text = text.replace("\\", "\\\\");
    text = text.replace("\"", "\\\"");
    text = text.replace("\r", "\\r");
    text = text.replace("\n", "\\n");
    return text;
}

juce::String quoted(const juce::String& text)
{
    return "\"" + jsonEscape(text) + "\"";
}

juce::String number(double value, int decimals = 6)
{
    if (!std::isfinite(value))
        return "null";

    return juce::String(value, decimals);
}

juce::String envOrFallback(const char* name, const juce::String& fallback)
{
    const auto value = juce::SystemStats::getEnvironmentVariable(name, {});
    return value.isNotEmpty() ? value : fallback;
}

juce::String buildConfigurationName()
{
#if JUCE_DEBUG
    return "Debug";
#else
    return "Release";
#endif
}

juce::String compilerDescription()
{
#if defined(_MSC_FULL_VER)
    return "MSVC " + juce::String(_MSC_VER) + "." + juce::String(_MSC_FULL_VER);
#elif defined(__clang__)
    return "Clang " + juce::String(__clang_major__) + "." + juce::String(__clang_minor__) + "." + juce::String(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "GCC " + juce::String(__GNUC__) + "." + juce::String(__GNUC_MINOR__) + "." + juce::String(__GNUC_PATCHLEVEL__);
#else
    return "unknown";
#endif
}

juce::String platformDescription()
{
#if JUCE_WINDOWS
    const juce::String os = "Windows";
#elif JUCE_MAC
    const juce::String os = "macOS";
#elif JUCE_LINUX
    const juce::String os = "Linux";
#else
    const juce::String os = "unknown";
#endif

    return os + " " + juce::String((int) (sizeof(void*) * 8)) + "-bit";
}

void appendMetadataJson(juce::String& json, bool includeVolatileMetadata)
{
    json << "  \"metadata\": {" << juce::newLine;
    json << "    \"generatedAt\": " << quoted(includeVolatileMetadata
        ? juce::Time::getCurrentTime().toISO8601(true)
        : juce::String("<volatile>")) << "," << juce::newLine;
    json << "    \"commitHash\": " << quoted(envOrFallback("NOVA_BUILD_COMMIT", "unknown")) << "," << juce::newLine;
    json << "    \"branch\": " << quoted(envOrFallback("NOVA_BUILD_BRANCH", "unknown")) << "," << juce::newLine;
    json << "    \"buildConfiguration\": "
        << quoted(envOrFallback("NOVA_BUILD_CONFIGURATION", buildConfigurationName())) << "," << juce::newLine;
    json << "    \"buildPlatform\": "
        << quoted(envOrFallback("NOVA_BUILD_PLATFORM", platformDescription())) << "," << juce::newLine;
    json << "    \"compiler\": " << quoted(compilerDescription()) << "," << juce::newLine;
    json << "    \"osName\": " << quoted(juce::SystemStats::getOperatingSystemName()) << "," << juce::newLine;
    json << "    \"sampleRate\": " << number(kSampleRate, 1) << "," << juce::newLine;
    json << "    \"blockSize\": " << kBlockSize << "," << juce::newLine;
    json << "    \"renderSamples\": " << kRenderSamples << "," << juce::newLine;
    json << "    \"measurementStartSample\": " << kMeasurementStart << "," << juce::newLine;
    json << "    \"noiseSeedHex\": \"0x14B10\"," << juce::newLine;
    json << "    \"sweepSteps\": " << kSweepSteps << juce::newLine;
    json << "  }," << juce::newLine;
}

void appendMetricsJson(juce::String& json, const Metrics& metrics)
{
    json << "{"
        << "\"rms\":" << number(metrics.rms, 9) << ","
        << "\"peak\":" << number(metrics.peak, 9) << ","
        << "\"rmsDbfs\":" << number(metrics.rmsDbfs, 4) << ","
        << "\"peakDbfs\":" << number(metrics.peakDbfs, 4) << ","
        << "\"finite\":" << (metrics.finite ? "true" : "false")
        << "}";
}

juce::String buildReportJson(const std::vector<ReferenceResult>& references,
    const std::vector<SweepResult>& sweeps,
    const std::vector<SpectralResult>& spectrals,
    const GateProbeResult& gateProbe,
    bool includeVolatileMetadata = true)
{
    juce::String json;
    json << "{" << juce::newLine;
    json << "  \"schema\": \"nova.p14b_p1.drive_dynamics_measurement.v1\"," << juce::newLine;
    appendMetadataJson(json, includeVolatileMetadata);
    json << "  \"sampleRate\": " << number(kSampleRate, 1) << "," << juce::newLine;
    json << "  \"blockSize\": " << kBlockSize << "," << juce::newLine;
    json << "  \"renderSamples\": " << kRenderSamples << "," << juce::newLine;
    json << "  \"measurementStartSample\": " << kMeasurementStart << "," << juce::newLine;
    json << "  \"normalUnitTestsWriteFiles\": false," << juce::newLine;
    json << "  \"spectralLimitation\": \"Goertzel harmonic proxy only; no robust FFT and no aliasing claim.\"," << juce::newLine;

    json << "  \"referenceBaselines\": [" << juce::newLine;
    for (size_t i = 0; i < references.size(); ++i)
    {
        const auto& r = references[i];
        json << "    {"
            << "\"pedal\":" << quoted(r.pedal) << ","
            << "\"signal\":" << quoted(r.signal) << ","
            << "\"referenceSetting\":" << quoted(r.referenceSetting) << ","
            << "\"input\":";
        appendMetricsJson(json, r.input);
        json << ",\"bypass\":";
        appendMetricsJson(json, r.bypass);
        json << ",\"engaged\":";
        appendMetricsJson(json, r.engaged);
        json << ",\"nullRmsDbfs\":" << number(r.nullRmsDbfs, 4)
            << ",\"engagedMinusBypassDb\":" << number(r.engagedMinusBypassDb, 4)
            << ",\"finite\":" << (r.finite ? "true" : "false")
            << "}" << (i + 1 == references.size() ? "" : ",") << juce::newLine;
    }
    json << "  ]," << juce::newLine;

    json << "  \"sweeps\": [" << juce::newLine;
    for (size_t i = 0; i < sweeps.size(); ++i)
    {
        const auto& s = sweeps[i];
        json << "    {"
            << "\"pedal\":" << quoted(s.pedal) << ","
            << "\"controlId\":" << quoted(s.controlId) << ","
            << "\"minimum\":" << number(s.minimum, 4) << ","
            << "\"maximum\":" << number(s.maximum, 4) << ","
            << "\"maxAdjacentJumpDb\":" << number(s.maxAdjacentJumpDb, 4)
            << ",\"finite\":" << (s.finite ? "true" : "false")
            << ",\"steps\":[";

        for (size_t stepIndex = 0; stepIndex < s.steps.size(); ++stepIndex)
        {
            const auto& step = s.steps[stepIndex];
            json << "{\"value\":" << number(step.value, 4) << ",\"output\":";
            appendMetricsJson(json, step.output);
            json << "}" << (stepIndex + 1 == s.steps.size() ? "" : ",");
        }

        json << "]}" << (i + 1 == sweeps.size() ? "" : ",") << juce::newLine;
    }
    json << "  ]," << juce::newLine;

    json << "  \"spectralProxies\": [" << juce::newLine;
    for (size_t i = 0; i < spectrals.size(); ++i)
    {
        const auto& s = spectrals[i];
        json << "    {"
            << "\"pedal\":" << quoted(s.pedal) << ","
            << "\"sineFrequencyHz\":" << number(s.sineFrequencyHz, 1) << ","
            << "\"method\":" << quoted(s.method) << ","
            << "\"robustFftAvailable\":" << (s.robustFftAvailable ? "true" : "false")
            << ",\"output\":";
        appendMetricsJson(json, s.output);
        json << ",\"fundamentalDbfs\":" << number(s.fundamentalDbfs, 4)
            << ",\"trackedHarmonicDbfs\":" << number(s.trackedHarmonicDbfs, 4)
            << ",\"inharmonicResidualRatioDb\":" << number(s.inharmonicResidualRatioDb, 4)
            << ",\"finite\":" << (s.finite ? "true" : "false")
            << "}" << (i + 1 == spectrals.size() ? "" : ",") << juce::newLine;
    }
    json << "  ]," << juce::newLine;

    json << "  \"gateProbe\": {"
        << "\"observedLookaheadSamples\":" << gateProbe.observedLookaheadSamples
        << ",\"openAttenuationDb\":" << number(gateProbe.openAttenuationDb, 4)
        << ",\"closedAttenuationDb\":" << number(gateProbe.closedAttenuationDb, 4)
        << ",\"output\":";
    appendMetricsJson(json, gateProbe.output);
    json << ",\"finite\":" << (gateProbe.finite ? "true" : "false") << "}" << juce::newLine;
    json << "}" << juce::newLine;
    return json;
}

juce::String buildReportJson()
{
    return buildReportJson(buildReferenceBaselines(),
        buildSweepBaselines(),
        buildSpectralBaselines(),
        runGateProbe(),
        true);
}
} // namespace P14BMeasurement
#endif

void prepareP10CProcessor(juce::AudioProcessor& processor)
{
    processor.setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
    processor.prepareToPlay(kSampleRate, kBlockSize);
}

void fillP10CPalmMuteBlock(juce::AudioBuffer<float>& block, int blockIndex, float inputScale = 1.0f)
{
    for (int i = 0; i < block.getNumSamples(); ++i)
    {
        const int sampleIndex = blockIndex * block.getNumSamples() + i;
        const float t = (float) sampleIndex / (float) kSampleRate;
        const int pickSample = sampleIndex % (int) (kSampleRate * 0.112);
        const float pick = std::exp(-(float) pickSample / 34.0f);
        const float gate = (sampleIndex % (int) (kSampleRate * 0.448)) < (int) (kSampleRate * 0.260) ? 1.0f : 0.18f;
        const float note = 0.060f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
            + 0.030f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
            + 0.014f * std::sin(juce::MathConstants<float>::twoPi * 329.64f * t);
        const float sample = inputScale * gate * (note + 0.042f * pick);
        block.setSample(0, i, sample);
        block.setSample(1, i, sample * 0.96f);
    }
}

struct P10CStageMetrics
{
    juce::String name;
    P10CWindowMetrics metrics;
};

enum class P10DSignalKind
{
    PalmMuteRepeated,
    StaccatoStrong,
    SustainLong,
    SilenceRecovery,
    LowEBurst,
    StrongChord
};

struct P10DWindowMetrics
{
    P10CWindowMetrics signal;
    double envelopeMeanSum = 0.0;
    double envelopeBand3To20SumSquares = 0.0;
    double highFrequencySumSquares = 0.0;
    double gateDeltaSum = 0.0;
    double gateDeltaPeak = 0.0;
    double previousGateGain = 1.0;
    double envLow3 = 0.0;
    double envLow20 = 0.0;
    std::array<double, 2> highPassReference {};
    std::vector<double> blockRmsValues;
    std::vector<double> blockPeakValues;
    int envelopeSamples = 0;
    int gateTransitions = 0;
    bool haveGateGain = false;

    void capture(const juce::AudioBuffer<float>& buffer, double gateGain = -1.0)
    {
        signal.capture(buffer);

        const double alpha3 = 1.0 - std::exp(-juce::MathConstants<double>::twoPi * 3.0 / kSampleRate);
        const double alpha20 = 1.0 - std::exp(-juce::MathConstants<double>::twoPi * 20.0 / kSampleRate);
        const double alphaHighReference = 1.0 - std::exp(-juce::MathConstants<double>::twoPi * 6500.0 / kSampleRate);

        double blockSumSquares = 0.0;
        double blockPeak = 0.0;
        int blockSamples = 0;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            double absMean = 0.0;
            int finiteChannels = 0;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float sample = buffer.getSample(ch, i);
                if (!std::isfinite(sample))
                    continue;

                const double value = (double) sample;
                const double absValue = std::abs(value);
                const size_t stateIndex = (size_t) juce::jlimit(0, 1, ch);

                highPassReference[stateIndex] += alphaHighReference * (value - highPassReference[stateIndex]);
                const double high = value - highPassReference[stateIndex];
                highFrequencySumSquares += high * high;

                absMean += absValue;
                blockSumSquares += value * value;
                blockPeak = juce::jmax(blockPeak, absValue);
                ++blockSamples;
                ++finiteChannels;
            }

            if (finiteChannels <= 0)
                continue;

            absMean /= (double) finiteChannels;
            envLow3 += alpha3 * (absMean - envLow3);
            envLow20 += alpha20 * (absMean - envLow20);
            const double band = envLow20 - envLow3;
            envelopeBand3To20SumSquares += band * band;
            envelopeMeanSum += absMean;
            ++envelopeSamples;
        }

        if (blockSamples > 0)
        {
            blockRmsValues.push_back(std::sqrt(blockSumSquares / (double) blockSamples));
            blockPeakValues.push_back(blockPeak);
        }

        if (gateGain >= 0.0)
        {
            const double boundedGate = juce::jlimit(0.0, 1.0, gateGain);
            if (haveGateGain)
            {
                const double delta = std::abs(boundedGate - previousGateGain);
                gateDeltaSum += delta;
                gateDeltaPeak = juce::jmax(gateDeltaPeak, delta);
                if (delta > 0.18)
                    ++gateTransitions;
            }

            previousGateGain = boundedGate;
            haveGateGain = true;
        }
    }

    double meanEnvelope() const noexcept
    {
        return envelopeSamples > 0 ? envelopeMeanSum / (double) envelopeSamples : 0.0;
    }

    double modulationDepth3To20() const noexcept
    {
        const double mean = meanEnvelope();
        return envelopeSamples > 0 && mean > 1.0e-9
            ? std::sqrt(envelopeBand3To20SumSquares / (double) envelopeSamples) / mean
            : 0.0;
    }

    double highFrequencyEnergyProxy() const noexcept
    {
        const double base = signal.rms();
        return signal.sampleCount > 0 && base > 1.0e-9
            ? std::sqrt(highFrequencySumSquares / (double) signal.sampleCount) / base
            : 0.0;
    }

    double blockRmsVarianceProxy() const
    {
        if (blockRmsValues.size() < 2)
            return 0.0;

        double sum = 0.0;
        for (double value : blockRmsValues)
            sum += value;

        const double mean = sum / (double) blockRmsValues.size();
        if (mean <= 1.0e-9)
            return 0.0;

        double variance = 0.0;
        for (double value : blockRmsValues)
            variance += (value - mean) * (value - mean);

        return std::sqrt(variance / (double) blockRmsValues.size()) / mean;
    }

    double peakVarianceProxy() const
    {
        if (blockPeakValues.size() < 2)
            return 0.0;

        double sum = 0.0;
        for (double value : blockPeakValues)
            sum += value;

        const double mean = sum / (double) blockPeakValues.size();
        if (mean <= 1.0e-9)
            return 0.0;

        double variance = 0.0;
        for (double value : blockPeakValues)
            variance += (value - mean) * (value - mean);

        return std::sqrt(variance / (double) blockPeakValues.size()) / mean;
    }
};

struct P10DStageMetrics
{
    juce::String name;
    P10DWindowMetrics metrics;
};

juce::String p10dMetricsSummary(const P10DWindowMetrics& metrics)
{
    return p10cMetricsSummary(metrics.signal)
        + ", modulationDepth3To20="
        + juce::String(metrics.modulationDepth3To20(), 4)
        + ", blockRmsVariance="
        + juce::String(metrics.blockRmsVarianceProxy(), 4)
        + ", peakVariance="
        + juce::String(metrics.peakVarianceProxy(), 4)
        + ", highFrequencyEnergyProxy="
        + juce::String(metrics.highFrequencyEnergyProxy(), 4)
        + ", gateTransitions="
        + juce::String(metrics.gateTransitions)
        + ", gateDeltaPeak="
        + juce::String(metrics.gateDeltaPeak, 4);
}

void fillP10DSignalBlock(juce::AudioBuffer<float>& block,
                         int blockIndex,
                         P10DSignalKind signalKind,
                         float inputScale = 1.0f)
{
    for (int i = 0; i < block.getNumSamples(); ++i)
    {
        const int sampleIndex = blockIndex * block.getNumSamples() + i;
        const float t = (float) sampleIndex / (float) kSampleRate;
        float sample = 0.0f;

        if (signalKind == P10DSignalKind::PalmMuteRepeated)
        {
            const int pickSample = sampleIndex % (int) (kSampleRate * 0.104);
            const float pick = std::exp(-(float) pickSample / 28.0f);
            const float phraseGate = (sampleIndex % (int) (kSampleRate * 0.420)) < (int) (kSampleRate * 0.235) ? 1.0f : 0.08f;
            sample = phraseGate * (0.064f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                + 0.023f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
                + 0.052f * pick);
        }
        else if (signalKind == P10DSignalKind::StaccatoStrong)
        {
            const int period = (int) (kSampleRate * 0.180);
            const int phase = sampleIndex % period;
            const float active = phase < (int) (kSampleRate * 0.062) ? 1.0f : 0.0f;
            const float decay = active > 0.0f ? std::exp(-(float) phase / 1550.0f) : 0.0f;
            sample = active * decay * (0.150f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                + 0.070f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
                + 0.035f * std::sin(juce::MathConstants<float>::twoPi * 246.94f * t));
        }
        else if (signalKind == P10DSignalKind::SustainLong)
        {
            const float attack = juce::jlimit(0.0f, 1.0f, (float) sampleIndex / (float) (kSampleRate * 0.030));
            sample = attack * (0.080f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                + 0.040f * std::sin(juce::MathConstants<float>::twoPi * 123.47f * t)
                + 0.026f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t));
        }
        else if (signalKind == P10DSignalKind::SilenceRecovery)
        {
            const int phraseSamples = (int) (kSampleRate * 0.36);
            const int cycle = sampleIndex % phraseSamples;
            const bool active = cycle < (int) (kSampleRate * 0.105);
            const bool silence = cycle >= (int) (kSampleRate * 0.105) && cycle < (int) (kSampleRate * 0.235);
            const float decay = active ? std::exp(-(float) cycle / 2200.0f) : 0.0f;
            sample = silence ? 0.0f : decay * (0.125f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                + 0.052f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t));
        }
        else if (signalKind == P10DSignalKind::LowEBurst)
        {
            const int burstSamples = (int) (kSampleRate * 0.280);
            const int phase = sampleIndex % burstSamples;
            const float decay = std::exp(-(float) phase / 3600.0f);
            sample = decay * (0.135f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                + 0.030f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t));
        }
        else
        {
            const float attack = juce::jlimit(0.0f, 1.0f, (float) sampleIndex / (float) (kSampleRate * 0.020));
            sample = attack * (0.100f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                + 0.075f * std::sin(juce::MathConstants<float>::twoPi * 123.47f * t)
                + 0.055f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
                + 0.026f * std::sin(juce::MathConstants<float>::twoPi * 246.94f * t));
        }

        sample *= inputScale;
        block.setSample(0, i, sample);
        block.setSample(1, i, sample * 0.965f);
    }
}

std::vector<P10DStageMetrics> renderP10DChain(const std::vector<std::pair<juce::String, juce::AudioProcessor*>>& stages,
                                              int blocksToRun,
                                              P10DSignalKind signalKind,
                                              float inputScale = 1.0f,
                                              int warmupBlocks = 24,
                                              OutputChainProcessor* outputChain = nullptr,
                                              int* limiterTouchedSamples = nullptr,
                                              int* limiterActiveBlocks = nullptr,
                                              int* sustainedClampBlocks = nullptr)
{
    std::vector<P10DStageMetrics> reports;
    reports.reserve(stages.size());
    for (const auto& stage : stages)
        reports.push_back({ stage.first, {} });

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> block(2, kBlockSize);
    OutputChainProcessor::DebugSnapshot previousSnapshot;
    if (outputChain != nullptr)
        previousSnapshot = outputChain->getDebugSnapshot();

    int consecutiveLimiterBlocks = 0;
    for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
    {
        fillP10DSignalBlock(block, blockIndex, signalKind, inputScale);

        for (size_t stageIndex = 0; stageIndex < stages.size(); ++stageIndex)
        {
            auto* processor = stages[stageIndex].second;
            if (processor != nullptr)
                processor->processBlock(block, midi);

            if (blockIndex >= warmupBlocks)
            {
                double gateGain = -1.0;
                if (auto* gate = dynamic_cast<NoiseGatePedal*>(processor))
                    gateGain = (double) gate->currentGateGainAtomic.load();
                else if (auto* distortion = dynamic_cast<DistortionPedal*>(processor))
                    gateGain = (double) distortion->currentGateGain;

                reports[stageIndex].metrics.capture(block, gateGain);
            }
        }

        if (outputChain != nullptr)
        {
            const auto snapshot = outputChain->getDebugSnapshot();
            const int touched = snapshot.limiterTouchedSamples >= previousSnapshot.limiterTouchedSamples
                ? snapshot.limiterTouchedSamples - previousSnapshot.limiterTouchedSamples
                : snapshot.limiterTouchedSamples;
            const int active = snapshot.limiterActiveBlocks >= previousSnapshot.limiterActiveBlocks
                ? snapshot.limiterActiveBlocks - previousSnapshot.limiterActiveBlocks
                : snapshot.limiterActiveBlocks;

            if (limiterTouchedSamples != nullptr)
                *limiterTouchedSamples += touched;
            if (limiterActiveBlocks != nullptr)
                *limiterActiveBlocks += active;

            consecutiveLimiterBlocks = active > 0 ? consecutiveLimiterBlocks + 1 : 0;
            if (sustainedClampBlocks != nullptr && consecutiveLimiterBlocks >= 4)
                ++(*sustainedClampBlocks);

            previousSnapshot = snapshot;
        }
    }

    return reports;
}

struct P10ELongStageMetrics
{
    juce::String name;
    P10DWindowMetrics metrics;
    double minActiveRms = std::numeric_limits<double>::max();
    double maxActiveRms = 0.0;
    double finalActiveRms = 0.0;
    double tailRms = 0.0;
    int silentWhileInputBlocks = 0;
    int maxConsecutiveSilentBlocks = 0;
};

double p10eBlockRms(const juce::AudioBuffer<float>& buffer)
{
    double sumSquares = 0.0;
    int count = 0;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* data = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const float sample = data[i];
            if (!std::isfinite(sample))
                continue;

            sumSquares += (double) sample * (double) sample;
            ++count;
        }
    }

    return count > 0 ? std::sqrt(sumSquares / (double) count) : 0.0;
}

void configureP10EHighGainAmp(HighGainAmp& amp)
{
    setRangedParamById(amp, "hgDrive", 7.4f);
    setRangedParamById(amp, "hgTight", 0.82f);
    setRangedParamById(amp, "hgPresence", 0.50f);
    setRangedParamById(amp, "hgTone", 0.48f);
    setRangedParamById(amp, "hgLevel", 0.58f);
}

void configureP10EModernCab(Modern4x12Cabinet& cab)
{
    setRangedParamById(cab, "m4x12Low", 1.0f);
    setRangedParamById(cab, "m4x12Presence", 1.4f);
    setRangedParamById(cab, "m4x12Distance", 0.34f);
    setRangedParamById(cab, "m4x12Level", 0.78f);
    setRangedParamById(cab, "m4x12Mix", 1.0f);
}

void configureP10EBoost(BoostPedal& boost)
{
    setRangedParamById(boost, "boostGain", 6.5f);
    setRangedParamById(boost, "boostTight", 0.78f);
    setRangedParamById(boost, "boostTone", 0.46f);
    setRangedParamById(boost, "boostLevel", 0.66f);
    setRangedParamById(boost, "boostChar", 0.08f);
}

void configureP10EDistortion(DistortionPedal& distortion, int modeIndex = 4)
{
    distortion.modeParam->setValueNotifyingHost(normalisedChoiceIndex(distortion.modeParam, modeIndex));
    distortion.gainParam->setValueNotifyingHost(distortion.gainParam->convertTo0to1(modeIndex >= 3 ? 34.0f : 30.0f));
    distortion.toneParam->setValueNotifyingHost(distortion.toneParam->convertTo0to1(0.43f));
    distortion.bodyParam->setValueNotifyingHost(distortion.bodyParam->convertTo0to1(0.44f));
    distortion.tightParam->setValueNotifyingHost(distortion.tightParam->convertTo0to1(0.70f));
    distortion.levelParam->setValueNotifyingHost(distortion.levelParam->convertTo0to1(0.19f));
    distortion.mixParam->setValueNotifyingHost(distortion.mixParam->convertTo0to1(1.0f));
}

void configureP10EReverb(ReverbPedal& reverb)
{
    reverb.modeParam->setValueNotifyingHost(normalisedChoiceIndex(reverb.modeParam, 1));
    reverb.decayParam->setValueNotifyingHost(reverb.decayParam->convertTo0to1(0.44f));
    reverb.toneParam->setValueNotifyingHost(reverb.toneParam->convertTo0to1(0.46f));
    reverb.sizeParam->setValueNotifyingHost(reverb.sizeParam->convertTo0to1(0.52f));
    reverb.dampingParam->setValueNotifyingHost(reverb.dampingParam->convertTo0to1(0.58f));
    reverb.bassCutParam->setValueNotifyingHost(reverb.bassCutParam->convertTo0to1(0.46f));
    reverb.diffusionParam->setValueNotifyingHost(reverb.diffusionParam->convertTo0to1(0.72f));
    reverb.widthParam->setValueNotifyingHost(reverb.widthParam->convertTo0to1(0.74f));
    reverb.modParam->setValueNotifyingHost(reverb.modParam->convertTo0to1(0.18f));
    reverb.predelayParam->setValueNotifyingHost(reverb.predelayParam->convertTo0to1(14.0f));
    reverb.mixParam->setValueNotifyingHost(reverb.mixParam->convertTo0to1(0.32f));
    reverb.duckParam->setValueNotifyingHost(reverb.duckParam->convertTo0to1(0.12f));
    reverb.swellParam->setValueNotifyingHost(reverb.swellParam->convertTo0to1(0.0f));
    reverb.gateParam->setValueNotifyingHost(reverb.gateParam->convertTo0to1(0.0f));
    reverb.reverseParam->setValueNotifyingHost(reverb.reverseParam->convertTo0to1(0.0f));
    reverb.freezeParam->setValueNotifyingHost(0.0f);
}

void configureP10EFuzz(FuzzPedal& fuzz)
{
    fuzz.modeParam->setValueNotifyingHost(normalisedChoiceIndex(fuzz.modeParam, 1));
    fuzz.fuzzParam->setValueNotifyingHost(fuzz.fuzzParam->convertTo0to1(52.0f));
    fuzz.toneParam->setValueNotifyingHost(fuzz.toneParam->convertTo0to1(0.42f));
    fuzz.gateParam->setValueNotifyingHost(fuzz.gateParam->convertTo0to1(0.12f));
    fuzz.levelParam->setValueNotifyingHost(fuzz.levelParam->convertTo0to1(0.34f));
    fuzz.biasParam->setValueNotifyingHost(fuzz.biasParam->convertTo0to1(0.66f));
    fuzz.mixParam->setValueNotifyingHost(fuzz.mixParam->convertTo0to1(1.0f));
}

std::vector<P10ELongStageMetrics> renderP10ELongChain(
    const std::vector<std::pair<juce::String, juce::AudioProcessor*>>& stages,
    int blocksToRun,
    P10DSignalKind signalKind,
    float inputScale,
    int warmupBlocks = 24,
    int silenceAfterBlock = -1,
    double activeInputThreshold = 0.004,
    double silentOutputThreshold = 0.00020)
{
    std::vector<P10ELongStageMetrics> reports;
    reports.reserve(stages.size());
    for (const auto& stage : stages)
        reports.push_back({ stage.first });

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> block(2, kBlockSize);
    std::vector<int> consecutiveSilent(stages.size(), 0);

    for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
    {
        if (silenceAfterBlock >= 0 && blockIndex >= silenceAfterBlock)
            block.clear();
        else
            fillP10DSignalBlock(block, blockIndex, signalKind, inputScale);

        const double inputRms = p10eBlockRms(block);

        for (size_t stageIndex = 0; stageIndex < stages.size(); ++stageIndex)
        {
            auto* processor = stages[stageIndex].second;
            if (processor != nullptr)
                processor->processBlock(block, midi);

            if (blockIndex < warmupBlocks)
                continue;

            const double outputRms = p10eBlockRms(block);
            auto& report = reports[stageIndex];
            report.metrics.capture(block);

            if (inputRms > activeInputThreshold)
            {
                report.minActiveRms = juce::jmin(report.minActiveRms, outputRms);
                report.maxActiveRms = juce::jmax(report.maxActiveRms, outputRms);
                report.finalActiveRms = outputRms;

                if (outputRms < silentOutputThreshold)
                {
                    ++report.silentWhileInputBlocks;
                    consecutiveSilent[stageIndex] += 1;
                    report.maxConsecutiveSilentBlocks = juce::jmax(report.maxConsecutiveSilentBlocks, consecutiveSilent[stageIndex]);
                }
                else
                {
                    consecutiveSilent[stageIndex] = 0;
                }
            }
            else if (silenceAfterBlock >= 0 && blockIndex > silenceAfterBlock + 24)
            {
                report.tailRms = outputRms;
            }
        }
    }

    for (auto& report : reports)
        if (report.minActiveRms == std::numeric_limits<double>::max())
            report.minActiveRms = 0.0;

    return reports;
}

juce::String p10eLongSummary(const P10ELongStageMetrics& report)
{
    return p10dMetricsSummary(report.metrics)
        + ", minActiveRms="
        + juce::String(report.minActiveRms, 6)
        + ", maxActiveRms="
        + juce::String(report.maxActiveRms, 6)
        + ", finalActiveRms="
        + juce::String(report.finalActiveRms, 6)
        + ", tailRms="
        + juce::String(report.tailRms, 6)
        + ", silentWhileInputBlocks="
        + juce::String(report.silentWhileInputBlocks)
        + ", maxConsecutiveSilentBlocks="
        + juce::String(report.maxConsecutiveSilentBlocks);
}

struct P10FDuckingMetrics
{
    juce::String name;
    P10DWindowMetrics metrics;
    double minActiveInputRms = std::numeric_limits<double>::max();
    double maxActiveInputRms = 0.0;
    double minOutputRmsWhileInputActive = std::numeric_limits<double>::max();
    double maxOutputRmsWhileInputActive = 0.0;
    double finalOutputRmsWhileInputActive = 0.0;
    double inputToOutputRatioSum = 0.0;
    double previousActiveOutputRms = 0.0;
    double previousActiveInputRms = 0.0;
    double previousActiveRatio = 0.0;
    double minActiveRatio = std::numeric_limits<double>::max();
    double maxActiveRatio = 0.0;
    double gainDropDuringActiveInput = 0.0;
    double blockRmsDropRatio = 0.0;
    double envelopeDuckDepth = 0.0;
    double gateGainMin = 1.0;
    double gateGainMaxDelta = 0.0;
    int activeBlocks = 0;
    int consecutiveGainReductionBlocks = 0;
    int maxConsecutiveGainReductionBlocks = 0;
    int recoveryTimeAfterDuck = 0;
    int currentRecoveryBlocks = 0;
    int ceilingTouchedSamples = 0;

    void capture(const juce::AudioBuffer<float>& buffer, double inputRms, double gateGain = -1.0)
    {
        metrics.capture(buffer, gateGain);

        if (gateGain >= 0.0)
            gateGainMin = juce::jmin(gateGainMin, juce::jlimit(0.0, 1.0, gateGain));

        const double outputRms = p10eBlockRms(buffer);
        if (inputRms <= 0.006)
            return;

        ++activeBlocks;
        minActiveInputRms = juce::jmin(minActiveInputRms, inputRms);
        maxActiveInputRms = juce::jmax(maxActiveInputRms, inputRms);
        minOutputRmsWhileInputActive = juce::jmin(minOutputRmsWhileInputActive, outputRms);
        maxOutputRmsWhileInputActive = juce::jmax(maxOutputRmsWhileInputActive, outputRms);
        finalOutputRmsWhileInputActive = outputRms;
        const double activeRatio = outputRms / juce::jmax(1.0e-9, inputRms);
        inputToOutputRatioSum += activeRatio;
        minActiveRatio = juce::jmin(minActiveRatio, activeRatio);
        maxActiveRatio = juce::jmax(maxActiveRatio, activeRatio);
        ceilingTouchedSamples = metrics.signal.nearClipSamples + metrics.signal.clippedSamples;

        if (previousActiveOutputRms > 1.0e-8 && previousActiveInputRms > 1.0e-8 && previousActiveRatio > 1.0e-8)
        {
            const bool inputStillActive = inputRms >= previousActiveInputRms * 0.82;
            const double ratioDrop = activeRatio / previousActiveRatio;
            if (inputStillActive && ratioDrop < 1.0)
            {
                const double dropDepth = 1.0 - ratioDrop;
                blockRmsDropRatio = juce::jmax(blockRmsDropRatio, dropDepth);
                if (dropDepth > 0.50)
                {
                    ++consecutiveGainReductionBlocks;
                    maxConsecutiveGainReductionBlocks = juce::jmax(maxConsecutiveGainReductionBlocks, consecutiveGainReductionBlocks);
                    if (consecutiveGainReductionBlocks >= 4)
                        gainDropDuringActiveInput = juce::jmax(gainDropDuringActiveInput, dropDepth);
                    currentRecoveryBlocks = 0;
                }
                else
                {
                    consecutiveGainReductionBlocks = 0;
                    ++currentRecoveryBlocks;
                }
            }
            else
            {
                consecutiveGainReductionBlocks = 0;
                ++currentRecoveryBlocks;
            }
        }

        if (maxActiveRatio > 1.0e-8 && minActiveRatio != std::numeric_limits<double>::max())
            envelopeDuckDepth = 1.0 - (minActiveRatio / maxActiveRatio);

        recoveryTimeAfterDuck = juce::jmax(recoveryTimeAfterDuck, currentRecoveryBlocks);
        previousActiveOutputRms = outputRms;
        previousActiveInputRms = inputRms;
        previousActiveRatio = activeRatio;
        gateGainMaxDelta = metrics.gateDeltaPeak;
    }

    double activeInputToOutputRmsRatio() const noexcept
    {
        return activeBlocks > 0 ? inputToOutputRatioSum / (double) activeBlocks : 0.0;
    }

    double outputRmsWhileInputActive() const noexcept
    {
        return minOutputRmsWhileInputActive == std::numeric_limits<double>::max() ? 0.0 : minOutputRmsWhileInputActive;
    }
};

juce::String p10fDuckingSummary(const P10FDuckingMetrics& report)
{
    return p10dMetricsSummary(report.metrics)
        + ", activeBlocks="
        + juce::String(report.activeBlocks)
        + ", gainDropDuringActiveInput="
        + juce::String(report.gainDropDuringActiveInput, 4)
        + ", blockRmsDropRatio="
        + juce::String(report.blockRmsDropRatio, 4)
        + ", envelopeDuckDepth="
        + juce::String(report.envelopeDuckDepth, 4)
        + ", recoveryTimeAfterDuck="
        + juce::String(report.recoveryTimeAfterDuck)
        + ", activeInputToOutputRmsRatio="
        + juce::String(report.activeInputToOutputRmsRatio(), 4)
        + ", consecutiveGainReductionBlocks="
        + juce::String(report.maxConsecutiveGainReductionBlocks)
        + ", outputRmsWhileInputActive="
        + juce::String(report.outputRmsWhileInputActive(), 6)
        + ", gateGainProxy="
        + juce::String(report.gateGainMin, 4)
        + ", ceilingTouchedSamples="
        + juce::String(report.ceilingTouchedSamples);
}

void fillP10FActiveHighGainBlock(juce::AudioBuffer<float>& block, int blockIndex, float inputScale = 1.0f)
{
    for (int i = 0; i < block.getNumSamples(); ++i)
    {
        const int sampleIndex = blockIndex * block.getNumSamples() + i;
        const float t = (float) sampleIndex / (float) kSampleRate;
        const int phraseSample = sampleIndex % (int) (kSampleRate * 0.620);
        const float phrase = phraseSample < (int) (kSampleRate * 0.420) ? 1.0f : 0.54f;
        const int pickSample = sampleIndex % (int) (kSampleRate * 0.118);
        const float pick = std::exp(-(float) pickSample / 42.0f);
        const float sustain = 0.070f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
            + 0.043f * std::sin(juce::MathConstants<float>::twoPi * 123.47f * t)
            + 0.030f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
            + 0.016f * std::sin(juce::MathConstants<float>::twoPi * 246.94f * t);
        const float sample = inputScale * phrase * (sustain + 0.035f * pick);
        block.setSample(0, i, sample);
        block.setSample(1, i, sample * 0.965f);
    }
}

std::vector<P10FDuckingMetrics> renderP10FChain(
    const std::vector<std::pair<juce::String, juce::AudioProcessor*>>& stages,
    int blocksToRun,
    float inputScale,
    int warmupBlocks = 32,
    int silenceAfterBlock = -1)
{
    std::vector<P10FDuckingMetrics> reports;
    reports.reserve(stages.size());
    for (const auto& stage : stages)
        reports.push_back({ stage.first });

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> block(2, kBlockSize);

    for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
    {
        if (silenceAfterBlock >= 0 && blockIndex >= silenceAfterBlock)
            block.clear();
        else
            fillP10FActiveHighGainBlock(block, blockIndex, inputScale);

        const double inputRms = p10eBlockRms(block);

        for (size_t stageIndex = 0; stageIndex < stages.size(); ++stageIndex)
        {
            auto* processor = stages[stageIndex].second;
            if (processor != nullptr)
                processor->processBlock(block, midi);

            if (blockIndex < warmupBlocks)
                continue;

            double gateGain = -1.0;
            if (auto* gate = dynamic_cast<NoiseGatePedal*>(processor))
                gateGain = (double) gate->currentGateGainAtomic.load();
            else if (auto* distortion = dynamic_cast<DistortionPedal*>(processor))
                gateGain = (double) distortion->currentGateGain;

            reports[stageIndex].capture(block, inputRms, gateGain);
        }
    }

    for (auto& report : reports)
    {
        if (report.minActiveInputRms == std::numeric_limits<double>::max())
            report.minActiveInputRms = 0.0;
        if (report.minOutputRmsWhileInputActive == std::numeric_limits<double>::max())
            report.minOutputRmsWhileInputActive = 0.0;
        if (report.minActiveRatio == std::numeric_limits<double>::max())
            report.minActiveRatio = 0.0;
    }

    return reports;
}

std::vector<P10CStageMetrics> renderP10CChain(const std::vector<std::pair<juce::String, juce::AudioProcessor*>>& stages,
                                              int blocksToRun,
                                              float inputScale = 1.0f,
                                              OutputChainProcessor* outputChain = nullptr,
                                              int* limiterTouchedSamples = nullptr,
                                              int* limiterActiveBlocks = nullptr,
                                              int* sustainedClampBlocks = nullptr)
{
    std::vector<P10CStageMetrics> reports;
    reports.reserve(stages.size());
    for (const auto& stage : stages)
        reports.push_back({ stage.first, {} });

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> block(2, kBlockSize);
    OutputChainProcessor::DebugSnapshot previousSnapshot;
    if (outputChain != nullptr)
        previousSnapshot = outputChain->getDebugSnapshot();

    int consecutiveLimiterBlocks = 0;
    for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
    {
        fillP10CPalmMuteBlock(block, blockIndex, inputScale);

        for (size_t stageIndex = 0; stageIndex < stages.size(); ++stageIndex)
        {
            if (auto* processor = stages[stageIndex].second)
                processor->processBlock(block, midi);

            reports[stageIndex].metrics.capture(block);
        }

        if (outputChain != nullptr)
        {
            const auto snapshot = outputChain->getDebugSnapshot();
            const int touched = snapshot.limiterTouchedSamples >= previousSnapshot.limiterTouchedSamples
                ? snapshot.limiterTouchedSamples - previousSnapshot.limiterTouchedSamples
                : snapshot.limiterTouchedSamples;
            const int active = snapshot.limiterActiveBlocks >= previousSnapshot.limiterActiveBlocks
                ? snapshot.limiterActiveBlocks - previousSnapshot.limiterActiveBlocks
                : snapshot.limiterActiveBlocks;

            if (limiterTouchedSamples != nullptr)
                *limiterTouchedSamples += touched;
            if (limiterActiveBlocks != nullptr)
                *limiterActiveBlocks += active;

            consecutiveLimiterBlocks = active > 0 ? consecutiveLimiterBlocks + 1 : 0;
            if (sustainedClampBlocks != nullptr && consecutiveLimiterBlocks >= 4)
                ++(*sustainedClampBlocks);

            previousSnapshot = snapshot;
        }
    }

    return reports;
}

void expectStereoSamplesMatch(juce::UnitTest& test,
    const juce::AudioBuffer<float>& buffer,
    const std::vector<float>& expectedLeft,
    const std::vector<float>& expectedRight,
    float tolerance = kTolerance)
{
    test.expectEquals(buffer.getNumChannels(), 2);
    test.expectEquals(buffer.getNumSamples(), (int)expectedLeft.size());

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float actualLeft = buffer.getSample(0, i);
        const float actualRight = buffer.getSample(1, i);
        const float targetLeft = expectedLeft[(size_t)i];
        const float targetRight = expectedRight[(size_t)i];

        test.expect(approximatelyEqual(actualLeft, targetLeft, tolerance),
            "Left channel mismatch at sample " + juce::String(i)
                + " actual=" + juce::String(actualLeft, 8)
                + " expected=" + juce::String(targetLeft, 8));
        test.expect(approximatelyEqual(actualRight, targetRight, tolerance),
            "Right channel mismatch at sample " + juce::String(i)
                + " actual=" + juce::String(actualRight, 8)
                + " expected=" + juce::String(targetRight, 8));
    }
}

void warmUpEngine(AudioEngine& engine, int blockSize, int blocks = 10)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> scratch(2, blockSize);

    for (int i = 0; i < blocks; ++i)
    {
        scratch.clear();
        engine.process(scratch, midi);
    }
}

// P12D helper: the OutputChain limiter is always armed at the transparent
// safety threshold, so the engine has a stable lookahead delay even on the
// clean path. Tests that previously asserted sample-by-sample equality on tiny
// buffers must now reconstruct the output pattern at the latency offset.
void expectStereoSamplesMatchAfterLatency(juce::UnitTest& test,
    AudioEngine& engine,
    const std::vector<float>& expectedLeft,
    const std::vector<float>& expectedRight,
    int blockSize,
    float tolerance = kTolerance)
{
    const int latency = engine.getLatencyNumSamples();
    const int patternSize = (int)expectedLeft.size();
    const int totalSamples = ((latency + patternSize + blockSize - 1) / blockSize + 2) * blockSize;

    juce::AudioBuffer<float> input(2, totalSamples);
    input.clear();
    input.copyFrom(0, 0, expectedLeft.data(), patternSize);
    input.copyFrom(1, 0, expectedRight.data(), patternSize);

    juce::AudioBuffer<float> captured(2, totalSamples);
    captured.clear();
    juce::MidiBuffer midi;

    for (int offset = 0; offset + blockSize <= totalSamples; offset += blockSize)
    {
        juce::AudioBuffer<float> block(2, blockSize);
        for (int ch = 0; ch < 2; ++ch)
            block.copyFrom(ch, 0, input, ch, offset, blockSize);
        engine.process(block, midi);
        for (int ch = 0; ch < 2; ++ch)
            captured.copyFrom(ch, offset, block, ch, 0, blockSize);
    }

    juce::AudioBuffer<float> output(2, patternSize);
    for (int ch = 0; ch < 2; ++ch)
        output.copyFrom(ch, 0, captured, ch, latency, patternSize);

    expectStereoSamplesMatch(test, output, expectedLeft, expectedRight, tolerance);
}

enum class P1PedalSignal
{
    Silence,
    Impulse,
    Sine100,
    Sine1k,
    Sine5k,
    DcOffset,
    StrongPeaks,
    NanInf,
    LowNoise
};

juce::String p1SignalName(P1PedalSignal signal)
{
    switch (signal)
    {
        case P1PedalSignal::Silence:     return "silence";
        case P1PedalSignal::Impulse:     return "impulse";
        case P1PedalSignal::Sine100:     return "sine100";
        case P1PedalSignal::Sine1k:      return "sine1k";
        case P1PedalSignal::Sine5k:      return "sine5k";
        case P1PedalSignal::DcOffset:    return "dc";
        case P1PedalSignal::StrongPeaks: return "strongPeaks";
        case P1PedalSignal::NanInf:      return "nanInf";
        case P1PedalSignal::LowNoise:    return "lowNoise";
    }

    return "unknown";
}

void fillP1PedalSignal(juce::AudioBuffer<float>& buffer,
    P1PedalSignal signal,
    double sampleRate,
    int seed = 0)
{
    buffer.clear();

    auto fillSine = [&](float frequency, float amplitude)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const double phase = juce::MathConstants<double>::twoPi
                    * (double) frequency
                    * (double) sample
                    / sampleRate;
                data[sample] = amplitude * (float) std::sin(phase);
            }
        }
    };

    switch (signal)
    {
        case P1PedalSignal::Silence:
            break;

        case P1PedalSignal::Impulse:
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample(ch, 0, ch == 0 ? 0.8f : -0.8f);
            break;

        case P1PedalSignal::Sine100:
            fillSine(100.0f, 0.35f);
            break;

        case P1PedalSignal::Sine1k:
            fillSine(1000.0f, 0.35f);
            break;

        case P1PedalSignal::Sine5k:
            fillSine(5000.0f, 0.28f);
            break;

        case P1PedalSignal::DcOffset:
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.applyGain(ch, 0, buffer.getNumSamples(), 0.0f);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                    buffer.setSample(ch, sample, ch == 0 ? 0.25f : -0.22f);
            break;

        case P1PedalSignal::StrongPeaks:
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                    buffer.setSample(ch, sample, (sample % 5) == 0 ? (ch == 0 ? 1.8f : -1.8f) : 0.0f);
            break;

        case P1PedalSignal::NanInf:
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                if (buffer.getNumSamples() > 0)
                    buffer.setSample(ch, 0, std::numeric_limits<float>::quiet_NaN());
                if (buffer.getNumSamples() > 1)
                    buffer.setSample(ch, 1, std::numeric_limits<float>::infinity());
                if (buffer.getNumSamples() > 2)
                    buffer.setSample(ch, 2, -std::numeric_limits<float>::infinity());

                for (int sample = 3; sample < buffer.getNumSamples(); ++sample)
                    buffer.setSample(ch, sample, 0.1f * std::sin(0.013f * (float)(sample + 11 * ch)));
            }
            break;

        case P1PedalSignal::LowNoise:
        {
            juce::Random rng(seed);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                    buffer.setSample(ch, sample, 0.025f * ((rng.nextFloat() * 2.0f) - 1.0f));
            break;
        }
    }
}

double p1BufferPeak(const juce::AudioBuffer<float>& buffer)
{
    double peak = 0.0;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = juce::jmax(peak, (double) buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
    return peak;
}

double p1AdjacentDeltaPeak(const juce::AudioBuffer<float>& buffer)
{
    double peak = 0.0;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        for (int sample = 1; sample < buffer.getNumSamples(); ++sample)
        {
            const double delta = std::abs((double) buffer.getSample(ch, sample)
                - (double) buffer.getSample(ch, sample - 1));
            peak = juce::jmax(peak, delta);
        }
    }

    return peak;
}

template <typename ProcessorType>
void runP1PreparedMatrix(juce::UnitTest& test, const juce::String& processorName)
{
    constexpr std::array<int, 6> blockSizes { 32, 64, 128, 256, 480, 512 };
    constexpr std::array<double, 3> sampleRates { 44100.0, 48000.0, 96000.0 };
    constexpr std::array<P1PedalSignal, 9> signals {
        P1PedalSignal::Silence,
        P1PedalSignal::Impulse,
        P1PedalSignal::Sine100,
        P1PedalSignal::Sine1k,
        P1PedalSignal::Sine5k,
        P1PedalSignal::DcOffset,
        P1PedalSignal::StrongPeaks,
        P1PedalSignal::NanInf,
        P1PedalSignal::LowNoise
    };

    juce::MidiBuffer midi;

    for (double sampleRate : sampleRates)
    {
        for (int blockSize : blockSizes)
        {
            ProcessorType processor;
            processor.prepareToPlay(sampleRate, blockSize);

            for (P1PedalSignal signal : signals)
            {
                processor.reset();
                processor.clearRealtimeFallbackCount();

                juce::AudioBuffer<float> buffer(2, blockSize);
                fillP1PedalSignal(buffer, signal, sampleRate, blockSize + (int) sampleRate);

                processor.processBlock(buffer, midi);

                const auto label = processorName
                    + " sr=" + juce::String((int) sampleRate)
                    + " block=" + juce::String(blockSize)
                    + " signal=" + p1SignalName(signal);

                test.expect(bufferHasOnlyFiniteSamples(buffer), label + " must stay finite");
                test.expectEquals(processor.getRealtimeFallbackCount(), 0, label + " must not hit fallback under prepared conditions");
                test.expect(p1BufferPeak(buffer) < 16.0, label + " must stay under the P1 emergency peak ceiling");
            }
        }
    }
}

template <typename ProcessorType>
void runP1OversizedFallback(juce::UnitTest& test, const juce::String& processorName)
{
    constexpr int preparedBlockSize = 32;
    constexpr int hostBlockSize = 64;
    constexpr double sampleRate = 48000.0;

    ProcessorType processor;
    processor.prepareToPlay(sampleRate, preparedBlockSize);
    processor.clearRealtimeFallbackCount();

    juce::AudioBuffer<float> buffer(2, hostBlockSize);
    fillP1PedalSignal(buffer, P1PedalSignal::Sine1k, sampleRate, 0x5151);

    juce::AudioBuffer<float> inputCopy(buffer);
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);

    test.expect(bufferHasOnlyFiniteSamples(buffer), processorName + " oversized fallback must stay finite");
    test.expectEquals(processor.getRealtimeFallbackCount(), 1, processorName + " oversized block must increment the realtime fallback counter");
    test.expect(computeBufferNullRms(buffer, inputCopy) < 1.0e-7,
        processorName + " oversized fallback should preserve dry audio without allocation");
}

template <typename ProcessorType>
void runP1BypassToggleContinuity(juce::UnitTest& test, const juce::String& processorName)
{
    constexpr int blockSize = 128;
    constexpr double sampleRate = 48000.0;

    ProcessorType processor;
    processor.prepareToPlay(sampleRate, blockSize);

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(2, blockSize);

    fillP1PedalSignal(buffer, P1PedalSignal::Sine1k, sampleRate, 0xB1);
    processor.processBlock(buffer, midi);
    const double activeDelta = p1AdjacentDeltaPeak(buffer);

    fillP1PedalSignal(buffer, P1PedalSignal::Sine1k, sampleRate, 0xB2);
    processor.setBypassed(true);
    processor.processBlock(buffer, midi);
    const double bypassDelta = p1AdjacentDeltaPeak(buffer);

    test.expect(bufferHasOnlyFiniteSamples(buffer), processorName + " bypass transition must remain finite");
    test.expect(bypassDelta < juce::jmax(2.5, activeDelta + 2.0),
        processorName + " bypass transition should not introduce a large single-sample jump");
}

juce::File getStartupPresetPointerFileForTest()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("NOVA")
        .getChildFile("startup-preset.txt");
}

struct StartupPresetPointerGuard
{
    StartupPresetPointerGuard()
        : pointerFile(getStartupPresetPointerFileForTest()),
          existed(pointerFile.existsAsFile()),
          originalText(existed ? pointerFile.loadFileAsString() : juce::String())
    {
    }

    ~StartupPresetPointerGuard()
    {
        if (existed)
        {
            pointerFile.getParentDirectory().createDirectory();
            pointerFile.replaceWithText(originalText);
            return;
        }

        if (pointerFile.existsAsFile())
            pointerFile.deleteFile();
    }

    juce::File pointerFile;
    bool existed = false;
    juce::String originalText;
};

juce::ValueTree makeP7DState()
{
    juce::ValueTree state(Nova::IDs::MAIN_STATE);
    Nova::PluginStateModel::canonicalizeStateTree(state);
    return state;
}

juce::ValueTree makeP7DPedal(const juce::String& type,
                             Nova::ZoneID zone,
                             const juce::String& id,
                             bool enabled)
{
    juce::ValueTree pedal(Nova::IDs::PEDAL);
    pedal.setProperty(Nova::IDs::PEDAL_TYPE, type, nullptr);
    pedal.setProperty(Nova::IDs::PEDAL_ZONE, (int) zone, nullptr);
    pedal.setProperty(Nova::IDs::PEDAL_ID, id, nullptr);
    pedal.setProperty(Nova::IDs::PEDAL_ENABLED, enabled, nullptr);
    return pedal;
}

void appendP7DPedal(juce::ValueTree state,
                    Nova::ChainID chain,
                    const juce::String& type,
                    Nova::ZoneID requestedZone,
                    const juce::String& id,
                    bool enabled)
{
    auto line = Nova::PluginStateModel::getLineTree(state, chain);
    if (line.isValid())
        line.appendChild(makeP7DPedal(type, requestedZone, id, enabled), nullptr);
}

juce::String canonicalXmlForP7D(const juce::ValueTree& state)
{
    auto copy = Nova::PluginStateModel::makeCanonicalCopy(state);
    auto xml = copy.createXml();
    return xml != nullptr ? xml->toString() : juce::String();
}

bool writeP7DValueTreeFile(const juce::File& file, const juce::ValueTree& state)
{
    juce::MemoryBlock block;
    juce::MemoryOutputStream stream(block, false);
    state.writeToStream(stream);
    return file.replaceWithData(block.getData(), block.getSize());
}

juce::ValueTree readP7DValueTreeFile(const juce::File& file)
{
    juce::MemoryBlock block;
    if (!file.loadFileAsData(block))
        return {};

    return juce::ValueTree::readFromData(block.getData(), block.getSize());
}

juce::String base64PayloadWithSize(size_t bytes, uint8_t seed)
{
    juce::MemoryBlock payload(bytes);
    auto* data = static_cast<uint8_t*>(payload.getData());

    for (size_t i = 0; i < bytes; ++i)
        data[i] = static_cast<uint8_t>(seed + (uint8_t)(i * 17u));

    return juce::Base64::toBase64(payload.getData(), payload.getSize());
}

juce::String validUnknownParameterPayloadForP7D()
{
    struct Codec final : ProcessorBase
    {
        static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
        {
            copyXmlToBinary(xml, block);
        }

        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    };

    juce::XmlElement xml("PLUGIN_STATE");
    xml.setAttribute("version", 1);
    auto* unknown = xml.createNewChildElement("PARAM");
    unknown->setAttribute("id", "p7d_unknown_parameter");
    unknown->setAttribute("value", 0.5);

    juce::MemoryBlock block;
    Codec::encode(xml, block);
    return juce::Base64::toBase64(block.getData(), block.getSize());
}

bool engineProcessesFiniteAfterP7DRestore(AudioEngine& engine,
                                          const AudioEngine::RuntimeGlobalParams& params,
                                          bool engineEnabled)
{
    engine.updateGlobalParams(params);
    engine.setEngineEnabled(engineEnabled);

    juce::AudioBuffer<float> buffer(2, kBlockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample(ch, i, static_cast<float>(0.08 * std::sin(2.0 * juce::MathConstants<double>::pi
                    * 220.0 * (double)(block * kBlockSize + i) / kSampleRate)));

        engine.process(buffer, midi);

        if (!bufferHasOnlyFiniteSamples(buffer))
            return false;
    }

    return true;
}

struct P9DDraftStep
{
    Nova::ChainID chain = Nova::ChainID::LineA;
    Nova::ZoneID zone = Nova::ZoneID::Pre;
    juce::String typeID;
};

struct P9DProcessMetrics
{
    bool finite = true;
    double peak = 0.0;
    double rms = 0.0;
    double dc = 0.0;
    int samples = 0;
    int invalidSamples = 0;
    int nearClipSamples = 0;
    int clippedSamples = 0;
    int limiterTouchedSamples = 0;
    int limiterActiveBlocks = 0;
    int sustainedClampBlocks = 0;
    float limiterMaxReductionDb = 0.0f;
    float limiterDeltaPeak = 0.0f;
    int softCeilingTouchedSamples = 0;
};

struct P9DDraftResult
{
    juce::String name;
    juce::String filePath;
    juce::String status = "NOT_GENERATED";
    juce::String roundTripStatus = "NOT_RUN";
    juce::String processStatus = "NOT_RUN";
    P9DProcessMetrics metrics;
    juce::StringArray failures;
};

juce::String p9dJsonEscape(const juce::String& text)
{
    juce::String escaped;
    for (auto c : text)
    {
        switch (c)
        {
            case '\\': escaped << "\\\\"; break;
            case '"':  escaped << "\\\""; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default:   escaped << juce::String::charToString(c); break;
        }
    }
    return escaped;
}

juce::String p9dZoneName(Nova::ZoneID zone)
{
    switch (zone)
    {
        case Nova::ZoneID::Pre: return "Pre";
        case Nova::ZoneID::Amp: return "Amp";
        case Nova::ZoneID::FX: return "FX";
        case Nova::ZoneID::Cabinet: return "Cabinet";
        default: return "Unknown";
    }
}

juce::String p9dStemForName(juce::String name)
{
    auto safe = name.trim().retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-");
    safe = safe.replace(" ", "-");
    while (safe.contains("--"))
        safe = safe.replace("--", "-");
    return safe.isNotEmpty() ? safe : "Draft-Preset";
}

bool p9dParseZone(const juce::String& text, Nova::ZoneID& zone)
{
    if (text == "Pre") { zone = Nova::ZoneID::Pre; return true; }
    if (text == "Amp") { zone = Nova::ZoneID::Amp; return true; }
    if (text == "FX") { zone = Nova::ZoneID::FX; return true; }
    if (text == "Cabinet") { zone = Nova::ZoneID::Cabinet; return true; }
    return false;
}

bool p9dParseChain(const juce::String& text, Nova::ChainID& chain)
{
    if (text == "A" || text == "LineA") { chain = Nova::ChainID::LineA; return true; }
    if (text == "B" || text == "LineB") { chain = Nova::ChainID::LineB; return true; }
    return false;
}

juce::String p9dCanonicalXml(const juce::ValueTree& state)
{
    auto canonical = Nova::PluginStateModel::makeCanonicalCopy(state);
    auto xml = canonical.createXml();
    return xml != nullptr ? xml->toString() : juce::String();
}

bool p9dWriteValueTreeFile(const juce::File& file, const juce::ValueTree& state)
{
    juce::MemoryOutputStream stream;
    state.writeToStream(stream);
    file.getParentDirectory().createDirectory();
    return file.replaceWithData(stream.getData(), stream.getDataSize());
}

juce::ValueTree p9dReadValueTreeFile(const juce::File& file)
{
    juce::MemoryBlock data;
    if (!file.loadFileAsData(data))
        return {};

    return juce::ValueTree::readFromData(data.getData(), (int) data.getSize());
}

void p9dApplyDraftRuntimeDefaults(juce::ValueTree state, const juce::String& name)
{
    Nova::PluginStateModel::canonicalizeStateTree(state);

    if (auto settings = Nova::PluginStateModel::getSettingsTree(state); settings.isValid())
    {
        settings.setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
        settings.setProperty(Nova::IDs::SWITCH_MODE, (int) Nova::SwitcherMode::LineA_Only, nullptr);
        settings.setProperty(Nova::IDs::INPUT_GAIN, 0.0f, nullptr);
        settings.setProperty(Nova::IDs::INPUT_GATE, -100.0f, nullptr);
        settings.setProperty(Nova::IDs::FORCE_MONO, false, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_VOL, name == "Tight Modern Rhythm" ? -22.0f : -6.0f, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_MIX, 100.0f, nullptr);
    }

    Nova::PluginStateModel::canonicalizeStateTree(state);
}

bool p9dBuildState(const juce::String& name,
                   const std::vector<P9DDraftStep>& steps,
                   juce::ValueTree& state,
                   juce::StringArray& failures)
{
    SessionStore store;

    for (int i = 0; i < (int) steps.size(); ++i)
    {
        const auto& step = steps[(size_t) i];
        const auto canonicalType = PedalRegistry::canonicalType(step.typeID);

        if (!PedalRegistry::isTypeSupported(canonicalType))
        {
            failures.add("unregistered typeID: " + step.typeID);
            continue;
        }

        if (!Nova::PedalCatalog::canLiveInZone(canonicalType, step.zone))
            failures.add("invalid zone " + p9dZoneName(step.zone) + " for " + canonicalType);

        const auto result = store.applyCommand(SessionStore::Command::makeAddPedal(
            canonicalType, step.chain, step.zone, -1));

        if (!result.changed || !result.insertResult.inserted)
        {
            failures.add("insert failed for " + canonicalType);
            continue;
        }

        auto line = Nova::PluginStateModel::getLineTree(store.state(), step.chain);
        if (line.isValid() && result.insertResult.index >= 0 && result.insertResult.index < line.getNumChildren())
            line.getChild(result.insertResult.index).setProperty(Nova::IDs::PEDAL_ID,
                "p9d-" + p9dStemForName(name).toLowerCase() + "-" + juce::String(i), nullptr);
    }

    state = Nova::PluginStateModel::makeCanonicalCopy(store.state());
    p9dApplyDraftRuntimeDefaults(state, name);
    return failures.isEmpty();
}

bool p9dValidateCanonicalState(const juce::ValueTree& state, juce::StringArray& failures)
{
    if (!state.isValid() || !state.hasType(Nova::IDs::MAIN_STATE))
    {
        failures.add("state root is not NOVA_STATE");
        return false;
    }

    if ((int) state.getProperty(Nova::IDs::STATE_SCHEMA_VERSION, -1) != Nova::Config::STATE_SCHEMA_VERSION)
        failures.add("schemaVersion is not 1");

    for (auto chain : { Nova::ChainID::LineA, Nova::ChainID::LineB })
    {
        auto line = Nova::PluginStateModel::getLineTree(state, chain);
        if (!line.isValid())
        {
            failures.add("missing line");
            continue;
        }

        int previousRank = -1;
        int ampCount = 0;
        int cabinetCount = 0;
        int preCount = 0;
        int fxCount = 0;

        for (int i = 0; i < line.getNumChildren(); ++i)
        {
            auto pedal = line.getChild(i);
            const auto type = pedal.getProperty(Nova::IDs::PEDAL_TYPE).toString();
            const auto zone = static_cast<Nova::ZoneID>((int) pedal.getProperty(Nova::IDs::PEDAL_ZONE, 0));
            const int rank = Nova::PluginStateModel::zoneSortRank(zone);

            if (!PedalRegistry::isTypeSupported(type))
                failures.add("unsupported type after canonicalization: " + type);
            if (!Nova::PedalCatalog::canLiveInZone(type, zone))
                failures.add("invalid zone after canonicalization: " + type);
            if (rank < previousRank)
                failures.add("non-canonical zone order");
            previousRank = rank;

            if (Nova::PedalCatalog::kindFromType(type) == Nova::PedalCatalog::Kind::Amplifier)
                ++ampCount;
            if (Nova::PedalCatalog::kindFromType(type) == Nova::PedalCatalog::Kind::Cabinet)
                ++cabinetCount;
            if (zone == Nova::ZoneID::Pre)
                ++preCount;
            if (zone == Nova::ZoneID::FX)
                ++fxCount;
        }

        if (ampCount > 1)
            failures.add("duplicate amp");
        if (cabinetCount > 1)
            failures.add("duplicate cabinet");
        if (preCount > Nova::Config::MAX_PEDALS_PER_FLEX_ZONE)
            failures.add("Pre zone capacity exceeded");
        if (fxCount > Nova::Config::MAX_PEDALS_PER_FLEX_ZONE)
            failures.add("FX zone capacity exceeded");
    }

    return failures.isEmpty();
}

void p9dRebuildEngineFromState(AudioEngine& engine, const juce::ValueTree& state)
{
    engine.clearAll();
    engine.synchronizeProcessingState();

    for (auto chain : { Nova::ChainID::LineA, Nova::ChainID::LineB })
    {
        auto line = Nova::PluginStateModel::getLineTree(state, chain);
        if (!line.isValid())
            continue;

        for (int i = 0; i < line.getNumChildren(); ++i)
        {
            auto pedal = line.getChild(i);
            const auto zone = static_cast<Nova::ZoneID>((int) pedal.getProperty(Nova::IDs::PEDAL_ZONE, 0));
            engine.addPedal(pedal.getProperty(Nova::IDs::PEDAL_TYPE).toString(),
                chain, i, zone, pedal.getProperty(Nova::IDs::PEDAL_ID).toString());
            engine.setPedalBypassed(chain, i, !(bool) pedal.getProperty(Nova::IDs::PEDAL_ENABLED, true));
        }
    }

    engine.synchronizeProcessingState();
}

P9DProcessMetrics p9dProcessDraft(const juce::String& name, const juce::ValueTree& state)
{
    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize, 2, 2);

    AudioEngine::RuntimeGlobalParams params;
    if (auto settings = Nova::PluginStateModel::getSettingsTree(state); settings.isValid())
    {
        params.outputVolumeDb = (float) settings.getProperty(Nova::IDs::OUTPUT_VOL, -6.0f);
        params.outputLimiterDb = (float) settings.getProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f);
        params.outputMixRaw = (float) settings.getProperty(Nova::IDs::OUTPUT_MIX, 100.0f);
    }
    engine.updateGlobalParams(params);
    engine.setEngineEnabled(true);
    p9dRebuildEngineFromState(engine, state);

    P9DProcessMetrics metrics;
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    juce::MidiBuffer midi;
    double sum = 0.0;
    double sumSquares = 0.0;
    OutputChainProcessor::DebugSnapshot previousSnapshot = engine.getOutputChainDebugSnapshot();

    const int totalBlocks = name == "Wide Ambient Clean" ? 220 : 128;
    for (int block = 0; block < totalBlocks; ++block)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            const double t = (double) (block * kBlockSize + i) / kSampleRate;
            float sample = (float) (0.08 * std::sin(2.0 * juce::MathConstants<double>::pi * 110.0 * t)
                + 0.035 * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * t));

            if (name == "Tight Modern Rhythm")
                sample *= ((block / 8) % 2) == 0 ? 1.0f : 0.20f;
            else if (name == "Funk Comp Clean")
                sample *= (i < 12) ? 1.4f : 0.65f;
            else if (name == "Wide Ambient Clean" && block > 120)
                sample = 0.0f;

            buffer.setSample(0, i, sample);
            buffer.setSample(1, i, sample * 0.97f);
        }

        engine.process(buffer, midi);

        const auto snapshot = engine.getOutputChainDebugSnapshot();
        const int blockLimiterTouchedSamples = snapshot.limiterTouchedSamples >= previousSnapshot.limiterTouchedSamples
            ? snapshot.limiterTouchedSamples - previousSnapshot.limiterTouchedSamples
            : snapshot.limiterTouchedSamples;
        const int blockLimiterActiveBlocks = snapshot.limiterActiveBlocks >= previousSnapshot.limiterActiveBlocks
            ? snapshot.limiterActiveBlocks - previousSnapshot.limiterActiveBlocks
            : snapshot.limiterActiveBlocks;
        const int blockSoftCeilingTouchedSamples = snapshot.softCeilingTouchedSamples >= previousSnapshot.softCeilingTouchedSamples
            ? snapshot.softCeilingTouchedSamples - previousSnapshot.softCeilingTouchedSamples
            : snapshot.softCeilingTouchedSamples;

        metrics.limiterTouchedSamples += blockLimiterTouchedSamples;
        metrics.limiterActiveBlocks += blockLimiterActiveBlocks;
        metrics.softCeilingTouchedSamples += blockSoftCeilingTouchedSamples;
        metrics.limiterMaxReductionDb = juce::jmax(metrics.limiterMaxReductionDb, snapshot.limiterMaxReductionDb);
        metrics.limiterDeltaPeak = juce::jmax(metrics.limiterDeltaPeak, snapshot.limiterDeltaPeak);
        previousSnapshot = snapshot;

        bool blockClampProxy = blockLimiterTouchedSamples > 0 || blockSoftCeilingTouchedSamples > 0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* data = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float sample = data[i];
                if (!std::isfinite(sample))
                {
                    metrics.finite = false;
                    ++metrics.invalidSamples;
                    blockClampProxy = true;
                    continue;
                }

                const double value = (double) sample;
                const double absValue = std::abs(value);
                metrics.peak = juce::jmax(metrics.peak, absValue);
                sum += value;
                sumSquares += value * value;
                ++metrics.samples;

                if (absValue >= (double) Nova::Config::SIGNAL_NEAR_CLIP_THRESHOLD)
                {
                    ++metrics.nearClipSamples;
                    blockClampProxy = true;
                }
                if (absValue > 1.0)
                {
                    ++metrics.clippedSamples;
                    blockClampProxy = true;
                }
            }
        }

        if (blockClampProxy)
            ++metrics.sustainedClampBlocks;
    }

    if (metrics.samples > 0)
    {
        metrics.rms = std::sqrt(sumSquares / (double) metrics.samples);
        metrics.dc = std::abs(sum / (double) metrics.samples);
    }

    return metrics;
}

juce::String p9dBuildReportJson(const juce::String& status,
                                const juce::String& manifestPath,
                                const juce::String& outputDirectory,
                                const std::vector<P9DDraftResult>& results,
                                const juce::StringArray& failures)
{
    juce::String json;
    json << "{" << juce::newLine;
    json << "  \"status\": \"" << p9dJsonEscape(status) << "\"," << juce::newLine;
    json << "  \"manifestPath\": \"" << p9dJsonEscape(manifestPath.replace("\\", "/")) << "\"," << juce::newLine;
    json << "  \"outputDirectory\": \"" << p9dJsonEscape(outputDirectory.replace("\\", "/")) << "\"," << juce::newLine;
    json << "  \"generatedPresetCount\": " << (int) std::count_if(results.begin(), results.end(), [](const auto& r) { return r.status == "GENERATED_DRAFT"; }) << "," << juce::newLine;
    json << "  \"wroteUserPresetDirectory\": false," << juce::newLine;
    json << "  \"wroteStartupPresetPointer\": false," << juce::newLine;
    json << "  \"changedSchema\": false," << juce::newLine;
    json << "  \"presets\": [" << juce::newLine;

    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        json << "    {" << juce::newLine;
        json << "      \"name\": \"" << p9dJsonEscape(r.name) << "\"," << juce::newLine;
        json << "      \"filePath\": \"" << p9dJsonEscape(r.filePath.replace("\\", "/")) << "\"," << juce::newLine;
        json << "      \"generationStatus\": \"" << p9dJsonEscape(r.status) << "\"," << juce::newLine;
        json << "      \"roundTripStatus\": \"" << p9dJsonEscape(r.roundTripStatus) << "\"," << juce::newLine;
        json << "      \"processStatus\": \"" << p9dJsonEscape(r.processStatus) << "\"," << juce::newLine;
        json << "      \"metrics\": { \"finite\": " << (r.metrics.finite ? "true" : "false")
             << ", \"peak\": " << juce::String(r.metrics.peak, 8)
             << ", \"rms\": " << juce::String(r.metrics.rms, 8)
             << ", \"dc\": " << juce::String(r.metrics.dc, 8)
             << ", \"nearClipSamples\": " << r.metrics.nearClipSamples
             << ", \"clippedSamples\": " << r.metrics.clippedSamples
             << ", \"invalidSamples\": " << r.metrics.invalidSamples
             << ", \"limiterTouchedSamples\": " << r.metrics.limiterTouchedSamples
             << ", \"limiterActiveBlocks\": " << r.metrics.limiterActiveBlocks
             << ", \"sustainedClampBlocks\": " << r.metrics.sustainedClampBlocks
             << ", \"limiterMaxReductionDb\": " << juce::String(r.metrics.limiterMaxReductionDb, 8)
             << ", \"limiterDeltaPeak\": " << juce::String(r.metrics.limiterDeltaPeak, 8)
             << ", \"softCeilingTouchedSamples\": " << r.metrics.softCeilingTouchedSamples << " }," << juce::newLine;
        json << "      \"failures\": [";
        for (int f = 0; f < r.failures.size(); ++f)
            json << (f == 0 ? "" : ", ") << "\"" << p9dJsonEscape(r.failures[f]) << "\"";
        json << "]" << juce::newLine;
        json << "    }" << (i + 1 == results.size() ? "" : ",") << juce::newLine;
    }

    json << "  ]," << juce::newLine;
    json << "  \"failures\": [";
    for (int i = 0; i < failures.size(); ++i)
        json << (i == 0 ? "" : ", ") << "\"" << p9dJsonEscape(failures[i]) << "\"";
    json << "]" << juce::newLine;
    json << "}" << juce::newLine;
    return json;
}

#if JUCE_DEBUG || NOVA_ENABLE_RELEASE_DIAGNOSTICS
bool runP9DDraftPresetBuilderTool()
{
    const auto enabled = juce::SystemStats::getEnvironmentVariable("NOVA_RUN_P9D_DRAFT_BUILDER", {});
    if (enabled != "1")
        return false;

    const auto manifestPath = juce::SystemStats::getEnvironmentVariable(
        "NOVA_P9D_MANIFEST_PATH", "Resources/Presets/DraftFactory/factory-bank.draft.json");
    const auto outputDirectory = juce::SystemStats::getEnvironmentVariable(
        "NOVA_P9D_OUTPUT_DIR", "Resources/Presets/DraftFactory/generated");
    const auto reportPath = juce::SystemStats::getEnvironmentVariable(
        "NOVA_P9D_REPORT_PATH", "artifacts/p9d-draft-preset-builder-report.json");

    juce::StringArray failures;
    std::vector<P9DDraftResult> results;

    const juce::File manifestFile(manifestPath);
    const juce::File outputDir(outputDirectory);
    const juce::File reportFile(reportPath);

    const auto manifestJson = juce::JSON::parse(manifestFile);
    if (!manifestJson.isObject())
    {
        failures.add("manifest parse failed");
        reportFile.getParentDirectory().createDirectory();
        reportFile.replaceWithText(p9dBuildReportJson("FAIL", manifestPath, outputDirectory, results, failures));
        return true;
    }

    auto* manifest = manifestJson.getDynamicObject();
    const auto* presets = manifest != nullptr ? manifest->getProperty("presets").getArray() : nullptr;
    if (presets == nullptr)
    {
        failures.add("manifest presets array missing");
        reportFile.getParentDirectory().createDirectory();
        reportFile.replaceWithText(p9dBuildReportJson("FAIL", manifestPath, outputDirectory, results, failures));
        return true;
    }

    for (const auto& presetVar : *presets)
    {
        P9DDraftResult result;
        auto* preset = presetVar.getDynamicObject();
        if (preset == nullptr)
        {
            result.failures.add("preset entry is not an object");
            results.push_back(result);
            continue;
        }

        result.name = preset->getProperty("name").toString();
        result.filePath = outputDir.getChildFile(p9dStemForName(result.name) + ".nova-preset").getFullPathName();

        std::vector<P9DDraftStep> steps;
        if (const auto* chainTemplate = preset->getProperty("chainTemplate").getArray())
        {
            for (const auto& stepVar : *chainTemplate)
            {
                auto* stepObject = stepVar.getDynamicObject();
                if (stepObject == nullptr)
                {
                    result.failures.add("chainTemplate step is not an object");
                    continue;
                }

                P9DDraftStep step;
                if (!p9dParseChain(stepObject->getProperty("line").toString(), step.chain))
                    result.failures.add("invalid chain for " + stepObject->getProperty("typeID").toString());
                if (!p9dParseZone(stepObject->getProperty("zone").toString(), step.zone))
                    result.failures.add("invalid zone for " + stepObject->getProperty("typeID").toString());
                step.typeID = stepObject->getProperty("typeID").toString();
                steps.push_back(step);
            }
        }

        juce::ValueTree state;
        p9dBuildState(result.name, steps, state, result.failures);
        p9dValidateCanonicalState(state, result.failures);

        const auto presetFile = juce::File(result.filePath);
        if (result.failures.isEmpty() && p9dWriteValueTreeFile(presetFile, state))
            result.status = "GENERATED_DRAFT";
        else if (result.failures.isEmpty())
            result.failures.add("write failed: " + presetFile.getFullPathName());

        if (result.status == "GENERATED_DRAFT")
        {
            const auto loaded = p9dReadValueTreeFile(presetFile);
            juce::StringArray roundTripFailures;
            p9dValidateCanonicalState(loaded, roundTripFailures);
            const bool semanticMatch = p9dCanonicalXml(state) == p9dCanonicalXml(loaded);
            if (!semanticMatch)
                roundTripFailures.add("canonical semantic compare failed");

            juce::ValueTree second = Nova::PluginStateModel::makeCanonicalCopy(loaded);
            const auto secondFile = presetFile.getSiblingFile(presetFile.getFileNameWithoutExtension() + ".roundtrip.tmp");
            if (!p9dWriteValueTreeFile(secondFile, second))
                roundTripFailures.add("temporary second write failed");
            else
            {
                const auto secondLoaded = p9dReadValueTreeFile(secondFile);
                if (p9dCanonicalXml(loaded) != p9dCanonicalXml(secondLoaded))
                    roundTripFailures.add("save-load-save semantic compare failed");
                secondFile.deleteFile();
            }

            if (roundTripFailures.isEmpty())
                result.roundTripStatus = "ROUND_TRIP_PASS";
            else
                result.failures.addArray(roundTripFailures);

            result.metrics = p9dProcessDraft(result.name, loaded);
            if (result.metrics.finite
                && result.metrics.invalidSamples == 0
                && result.metrics.clippedSamples == 0
                && result.metrics.nearClipSamples == 0
                && result.metrics.dc < 0.02
                && result.metrics.peak < 0.98)
            {
                result.processStatus = "PROCESS_FINITE_PASS";
            }
            else
            {
                result.failures.add("process finite/gain check failed");
            }
        }

        if (result.failures.size() > 0)
            failures.add(result.name + ": " + result.failures.joinIntoString("; "));

        results.push_back(result);
    }

    const bool ok = failures.isEmpty() && !results.empty();
    reportFile.getParentDirectory().createDirectory();
    reportFile.replaceWithText(p9dBuildReportJson(ok ? "PASS" : "FAIL",
        manifestPath, outputDirectory, results, failures));
    return true;
}
#endif

class AudioEngineValidationTests final : public juce::UnitTest
{
public:
    AudioEngineValidationTests()
        : juce::UnitTest("Audio Engine Validation", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("InputChain forceMono collapses both channels to the same summed image");
        {
            InputChainProcessor input;
            input.prepareToPlay(kSampleRate, kBlockSize);
            input.setParams(0.0f, -100.0f, true);

            juce::AudioBuffer<float> buffer(2, 256);
            juce::MidiBuffer midi;

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float phaseA = juce::MathConstants<float>::twoPi * 110.0f * (float) i / (float) kSampleRate;
                const float phaseB = juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) kSampleRate;
                buffer.setSample(0, i, 0.18f * std::sin(phaseA));
                buffer.setSample(1, i, 0.12f * std::sin(phaseB) + 0.05f);
            }

            input.processBlock(buffer, midi);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                expect(approximatelyEqual(buffer.getSample(0, i), buffer.getSample(1, i), 1.0e-5f),
                    "ForceMono should leave both channels sample-identical");
            }
        }

        beginTest("InputChain preserves single-jack guitar level when input arrives on one channel");
        {
            auto render = [](bool forceMono)
            {
                InputChainProcessor input;
                input.prepareToPlay(kSampleRate, 1024);
                input.setParams(0.0f, -100.0f, forceMono);

                juce::AudioBuffer<float> buffer(2, 1024);
                juce::MidiBuffer midi;
                buffer.clear();

                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 1000.0f * (float) i / (float) kSampleRate;
                    buffer.setSample(1, i, 0.18f * std::sin(phase));
                }

                input.processBlock(buffer, midi);
                return buffer;
            };

            const auto stereoAuto = render(false);
            const auto forcedMono = render(true);
            const double autoLeftRms = computeChannelWindowRms(stereoAuto, 0, 256, 512);
            const double autoRightRms = computeChannelWindowRms(stereoAuto, 1, 256, 512);
            const double forcedLeftRms = computeChannelWindowRms(forcedMono, 0, 256, 512);
            const double forcedRightRms = computeChannelWindowRms(forcedMono, 1, 256, 512);

            expect(bufferHasOnlyFiniteSamples(stereoAuto), "Auto-normalized single-jack input must remain finite");
            expect(autoLeftRms > 0.10 && autoRightRms > 0.10,
                "A one-channel guitar input should be promoted to both channels without a -6 dB loss");
            expect(std::abs(autoLeftRms - autoRightRms) < 0.002,
                "Auto-normalized single-jack input should be level-matched between channels");
            expect(forcedLeftRms > 0.10 && forcedRightRms > 0.10,
                "Force mono should also preserve one-channel guitar level instead of averaging it down");
        }

        beginTest("ChannelStrip center pan preserves unity stereo level");
        {
            ChannelStripProcessor strip;
            strip.prepareToPlay(kSampleRate, kBlockSize);
            strip.setParams(1.0f, 0.0f, 1.0f);

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;

            const std::vector<float> left{ 0.8f, -0.4f, 0.2f, -0.1f };
            const std::vector<float> right{ -0.3f, 0.6f, -0.2f, 0.4f };

            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());

            strip.processBlock(buffer, midi);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                expect(approximatelyEqual(buffer.getSample(0, i), left[(size_t) i], 2.0e-4f),
                    "Left channel should stay unity at center pan");
                expect(approximatelyEqual(buffer.getSample(1, i), right[(size_t) i], 2.0e-4f),
                    "Right channel should stay unity at center pan");
            }
        }

        beginTest("ChannelStrip uses a unity-centered smooth balance curve");
        {
            ChannelStripProcessor strip;
            strip.prepareToPlay(kSampleRate, kBlockSize);
            strip.setParams(1.0f, 0.5f, 1.0f);

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;
            buffer.clear();
            buffer.clear(0, 0, 4);
            buffer.clear(1, 0, 4);

            const std::vector<float> input{ 1.0f, 1.0f, 1.0f, 1.0f };
            buffer.copyFrom(0, 0, input.data(), (int)input.size());
            buffer.copyFrom(1, 0, input.data(), (int)input.size());

            strip.processBlock(buffer, midi);

            const float expectedLeft = std::cos(juce::MathConstants<float>::halfPi * 0.5f);
            const float expectedRight = 1.0f;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                expect(approximatelyEqual(buffer.getSample(0, i), expectedLeft, 2.0e-4f),
                    "Left channel balance mismatch at sample " + juce::String(i));
                expect(approximatelyEqual(buffer.getSample(1, i), expectedRight, 2.0e-4f),
                    "Right channel balance mismatch at sample " + juce::String(i));
            }
        }

        beginTest("ChannelStrip hard balance endpoints do not boost either side");
        {
            auto render = [](float pan)
            {
                ChannelStripProcessor strip;
                strip.prepareToPlay(kSampleRate, kBlockSize);
                strip.setParams(1.0f, pan, 1.0f);

                juce::AudioBuffer<float> buffer(2, 4);
                juce::MidiBuffer midi;
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    buffer.setSample(0, i, 0.5f);
                    buffer.setSample(1, i, 0.5f);
                }

                strip.processBlock(buffer, midi);
                return buffer;
            };

            const auto left = render(-1.0f);
            const auto right = render(1.0f);

            expect(approximatelyEqual(left.getSample(0, 0), 0.5f, 2.0e-4f), "Hard-left should keep left at unity");
            expect(std::abs(left.getSample(1, 0)) < 2.0e-4f, "Hard-left should mute right");
            expect(std::abs(right.getSample(0, 0)) < 2.0e-4f, "Hard-right should mute left");
            expect(approximatelyEqual(right.getSample(1, 0), 0.5f, 2.0e-4f), "Hard-right should keep right at unity");
        }

        beginTest("TunerService preserves mono level and detects concert A");
        {
            TunerService tuner;
            tuner.setSampleRate(kSampleRate);
            tuner.reset();

            juce::AudioBuffer<float> buffer(1, 512);
            const float phaseIncrement = (float)(2.0 * juce::MathConstants<double>::pi * 440.0 / kSampleRate);
            float phase = 0.0f;

            for (int block = 0; block < 8; ++block)
            {
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                {
                    buffer.setSample(0, sample, std::sin(phase));
                    phase += phaseIncrement;
                }

                tuner.pushBuffer(buffer);
            }

            tuner.process();

            expect(std::abs(tuner.getCurrentRMS() - 0.70710678f) < 0.03f,
                "Mono RMS should stay near -3 dBFS for a full-scale sine");
            expect(std::abs(tuner.getCurrentPitch() - 440.0f) < 2.0f,
                "Pitch detector should lock near 440 Hz");
            expect(tuner.getCurrentClarity() > 0.85f, "Pitch clarity should pass the acceptance gate");
        }

        beginTest("OutputChain soft ceiling catches hard overs");
        {
            OutputChainProcessor output;
            output.prepareToPlay(kSampleRate, kBlockSize);
            output.setParams(0.0f, 0.0f);

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;

            const std::vector<float> left{ 1.4f, -1.35f, 1.3f, -1.25f };
            const std::vector<float> right{ -1.4f, 1.35f, -1.3f, 1.25f };

            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());

            output.processBlock(buffer, midi);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                expect(std::abs(buffer.getSample(0, i)) <= 1.0f, "Left channel exceeded digital ceiling");
                expect(std::abs(buffer.getSample(1, i)) <= 1.0f, "Right channel exceeded digital ceiling");
            }
        }

        beginTest("InputChain strips DC while preserving guitar-band fundamentals");
        {
            InputChainProcessor input;
            input.prepareToPlay(kSampleRate, 8192);
            input.setParams(0.0f, -100.0f, false);

            juce::AudioBuffer<float> buffer(2, 8192);
            juce::MidiBuffer midi;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 110.0f * (float) i / (float) kSampleRate;
                const float sample = 0.20f * std::sin(phase);
                buffer.setSample(0, i, sample + 0.18f);
                buffer.setSample(1, i, sample - 0.14f);
            }

            const double inputFundamental = computeFrequencyMagnitude(buffer,
                kSampleRate,
                110.0f,
                2048,
                4096);

            input.processBlock(buffer, midi);

            const double outputFundamental = computeFrequencyMagnitude(buffer,
                kSampleRate,
                110.0f,
                2048,
                4096);
            const double leftDc = std::abs(computeChannelMean(buffer, 0, 2048, 4096));
            const double rightDc = std::abs(computeChannelMean(buffer, 1, 2048, 4096));

            expect(bufferHasOnlyFiniteSamples(buffer), "Input cleanup render must remain finite");
            expect(leftDc < 0.01 && rightDc < 0.01, "InputChain should strongly reduce sustained DC bias on both channels");
            expect(outputFundamental > inputFundamental * 0.78, "InputChain should preserve the 110 Hz fundamental while cleaning subsonic/DC content");
        }

        beginTest("OutputChain clears sustained DC even when limiter is off");
        {
            OutputChainProcessor output;
            output.prepareToPlay(kSampleRate, 4096);
            output.setParams(0.0f, 0.0f);

            juce::AudioBuffer<float> buffer(2, 4096);
            juce::MidiBuffer midi;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    buffer.setSample(ch, i, 0.25f);

            output.processBlock(buffer, midi);

            const double lateRms = computeWindowRms(buffer, 3072, 1024);
            const double leftDc = std::abs(computeChannelMean(buffer, 0, 3072, 1024));
            const double rightDc = std::abs(computeChannelMean(buffer, 1, 3072, 1024));

            expect(bufferHasOnlyFiniteSamples(buffer), "DC cleanup render must remain finite");
            expect(lateRms < 0.02, "Always-on DC blocking should drain a sustained offset before the block ends");
            expect(leftDc < 0.01 && rightDc < 0.01, "OutputChain should not leave a material DC bias in the late window");
        }

        beginTest("OutputChain protects limiter headroom from biased input");
        {
            auto renderMagnitude = [&](float dcOffset)
            {
                OutputChainProcessor output;
                output.prepareToPlay(kSampleRate, 8192);
                output.setParams(0.0f, -6.0f);

                juce::AudioBuffer<float> buffer(2, 8192);
                juce::MidiBuffer midi;
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 1000.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.45f * std::sin(phase) + dcOffset;
                    buffer.setSample(0, i, sample);
                    buffer.setSample(1, i, sample);
                }

                output.processBlock(buffer, midi);
                return std::make_pair(computeFrequencyMagnitude(buffer, kSampleRate, 1000.0f, 2048, 4096),
                    std::abs(computeChannelMean(buffer, 0, 2048, 4096)));
            };

            const auto clean = renderMagnitude(0.0f);
            const auto biased = renderMagnitude(0.25f);

            expect(biased.first > clean.first * 0.82, "Biased input should keep most of its 1 kHz energy after the limiter once DC is removed upstream");
            expect(biased.second < 0.01, "Biased input should still leave the post-chain signal centered");
        }

        beginTest("OutputChain limiter does not add makeup gain below threshold");
        {
            auto render = [&](float limiterDb)
            {
                OutputChainProcessor output;
                output.prepareToPlay(kSampleRate, 4096);
                output.setParams(0.0f, limiterDb);

                juce::AudioBuffer<float> buffer(2, 4096);
                juce::MidiBuffer midi;
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 440.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.02f * std::sin(phase);
                    buffer.setSample(0, i, sample);
                    buffer.setSample(1, i, sample);
                }

                output.processBlock(buffer, midi);
                return buffer;
            };

            const auto bypassed = render(0.0f);
            const auto limited = render(-12.0f);
            const double bypassedRms = computeWindowRms(bypassed, 1024, 2048);
            const double limitedRms = computeWindowRms(limited, 1024, 2048);

            expect(bufferHasOnlyFiniteSamples(limited), "Limited render must remain finite");
            expect(limitedRms > bypassedRms * 0.90 && limitedRms < bypassedRms * 1.10,
                "Limiter should stay effectively transparent for material already below the threshold");
        }

        beginTest("OutputChain limiter reduces hot peaks without runaway expansion");
        {
            auto measurePeak = [](const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
            {
                const int safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
                const int safeLength = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
                float peak = 0.0f;

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < safeLength; ++i)
                        peak = juce::jmax(peak, std::abs(buffer.getSample(ch, safeStart + i)));

                return peak;
            };

            auto render = [&](float limiterDb)
            {
                OutputChainProcessor output;
                output.prepareToPlay(kSampleRate, 8192);
                output.setParams(0.0f, limiterDb);

                juce::AudioBuffer<float> buffer(2, 8192);
                juce::MidiBuffer midi;
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 1000.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.45f * std::sin(phase);
                    buffer.setSample(0, i, sample);
                    buffer.setSample(1, i, sample);
                }

                output.processBlock(buffer, midi);
                return buffer;
            };

            const auto bypassed = render(0.0f);
            const auto limited = render(-12.0f);
            const float bypassedPeak = measurePeak(bypassed, 2048, 4096);
            const float limitedPeak = measurePeak(limited, 2048, 4096);

            expect(bufferHasOnlyFiniteSamples(limited), "Hot limited render must remain finite");
            expect(limitedPeak < bypassedPeak * 0.75f,
                "Limiter should materially reduce sustained peaks above the threshold");
            expect(limitedPeak < 0.32f,
                "Limiter should settle near the requested ceiling instead of re-amplifying the signal");
        }

        beginTest("OutputChain clamps legacy extreme limiter thresholds to an audible floor");
        {
            OutputChainProcessor output;
            output.prepareToPlay(kSampleRate, 4096);
            output.setParams(0.0f, -20.0f);
            expect(output.getLatencySamples() > 0, "Active limiter should report its lookahead latency");

            juce::AudioBuffer<float> buffer(2, 4096);
            juce::MidiBuffer midi;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 1000.0f * (float) i / (float) kSampleRate;
                const float sample = 0.45f * std::sin(phase);
                buffer.setSample(0, i, sample);
                buffer.setSample(1, i, sample);
            }

            output.processBlock(buffer, midi);

            const float peak = juce::jmax(buffer.getMagnitude(0, 1024, 2048),
                buffer.getMagnitude(1, 1024, 2048));

            expect(bufferHasOnlyFiniteSamples(buffer), "Legacy clamped limiter render must remain finite");
            expect(peak > 0.18f && peak < 0.32f,
                "Legacy -20 dB limiter settings should clamp to the safer -12 dB ceiling range");

            output.setParams(0.0f, 0.0f);
            // P12D: limiter is always armed at the transparent safety threshold,
            // so the lookahead delay is stable regardless of user-controlled limiterDb.
            expect(output.getLatencySamples() > 0,
                "Always-armed limiter should report stable lookahead latency at limiterDb=0");
        }

        beginTest("OutputChain limiter catches isolated pick transients with lookahead");
        {
            OutputChainProcessor output;
            output.prepareToPlay(48000.0, 512);
            output.setParams(0.0f, -12.0f);

            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            buffer.clear();
            buffer.setSample(0, 32, 1.20f);
            buffer.setSample(1, 32, -1.05f);
            buffer.setSample(0, 128, -0.95f);
            buffer.setSample(1, 128, 0.90f);

            output.processBlock(buffer, midi);

            float peak = 0.0f;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));

            expect(bufferHasOnlyFiniteSamples(buffer), "Transient-limited render must remain finite");
            expect(peak < 0.31f, "Lookahead limiter should catch isolated transients near the requested ceiling");
            expect(peak > 0.18f, "Limiter should control transients without muting the signal");
        }

        beginTest("OutputChain handles larger-than-prepared blocks without corrupting audio");
        {
            OutputChainProcessor output;
            output.prepareToPlay(kSampleRate, 64);
            output.setParams(0.0f, -12.0f);

            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 700.0f * (float) i / (float) kSampleRate;
                const float sample = 0.75f * std::sin(phase);
                buffer.setSample(0, i, sample);
                buffer.setSample(1, i, sample);
            }

            output.processBlock(buffer, midi);

            expect(bufferHasOnlyFiniteSamples(buffer), "Oversized block render must remain finite");
            expect(buffer.getMagnitude(0, 128, 384) < 0.35f, "Limiter should still constrain oversized blocks");
            expect(buffer.getMagnitude(1, 128, 384) < 0.35f, "Limiter should still constrain oversized blocks on both channels");
        }

        beginTest("Synthetic cabinet IR keeps an approximately constant 23 ms window across sample rates");
        {
            const auto ir44 = Nova::CabinetIR::generateAtlas4x12(44100.0);
            const auto ir96 = Nova::CabinetIR::generateAtlas4x12(96000.0);

            expectEquals(ir44.getNumSamples(), juce::roundToInt(44100.0 * 0.023));
            expectEquals(ir96.getNumSamples(), juce::roundToInt(96000.0 * 0.023));
            expect(ir96.getNumSamples() > ir44.getNumSamples(), "Higher sample rates should carry a proportionally longer IR for equivalent time resolution");
        }

        beginTest("Synthetic cabinet IRs keep the 48 kHz response at 44.1 and 96 kHz (B-01)");
        {
            // Before the fix the decay was a fixed per-sample factor and the IR was
            // peak-normalized, so at 96 kHz the cabinet came out ~6 dB louder.
            const auto magnitudeDb = [](const juce::AudioBuffer<float>& ir, double sr, double freq)
            {
                const auto* data = ir.getReadPointer(0);
                const double w = juce::MathConstants<double>::twoPi * freq / sr;
                double re = 0.0, im = 0.0;
                for (int n = 0; n < ir.getNumSamples(); ++n)
                {
                    re += (double)data[n] * std::cos(w * n);
                    im -= (double)data[n] * std::sin(w * n);
                }
                return 10.0 * std::log10(juce::jmax(1.0e-24, re * re + im * im));
            };

            using Generator = juce::AudioBuffer<float> (*)(double);
            const std::pair<const char*, Generator> cabinets[] = {
                { "Atlas 4x12", &Nova::CabinetIR::generateAtlas4x12 },
                { "Vintage 2x12", &Nova::CabinetIR::generateVintage2x12 },
                { "Modern 4x12", &Nova::CabinetIR::generateModern4x12 }
            };
            static constexpr double freqs[] = { 100.0, 250.0, 500.0, 1000.0, 2000.0, 3000.0, 4000.0, 5000.0 };

            for (const auto& [name, generate] : cabinets)
            {
                const auto reference = generate(48000.0);

                for (double sr : { 44100.0, 96000.0 })
                {
                    const auto ir = generate(sr);
                    juce::String line;
                    line << "B-01 cab " << name << " @ " << juce::String(sr, 0) << " Hz vs 48 kHz:";
                    double worst = 0.0;

                    for (double f : freqs)
                    {
                        const double diff = magnitudeDb(ir, sr, f) - magnitudeDb(reference, 48000.0, f);
                        worst = juce::jmax(worst, std::abs(diff));
                        line << " " << juce::String(f, 0) << "Hz=" << juce::String(diff, 2);
                    }

                    logMessage(line);
                    expect(worst <= 1.0, juce::String(name) + " @ " + juce::String(sr, 0)
                        + " must stay within 1 dB of the 48 kHz response; worst=" + juce::String(worst, 2) + " dB");
                }
            }
        }

        beginTest("Overdrive keeps the opposite channel silent");
        {
            OverdrivePedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.getDriveParam()->setValueNotifyingHost(pedal.getDriveParam()->convertTo0to1(92.0f));
            pedal.getToneParam()->setValueNotifyingHost(pedal.getToneParam()->convertTo0to1(0.88f));
            pedal.getTextureParam()->setValueNotifyingHost(pedal.getTextureParam()->convertTo0to1(0.90f));
            pedal.getMixParam()->setValueNotifyingHost(pedal.getMixParam()->convertTo0to1(1.0f));
            pedal.getLevelParam()->setValueNotifyingHost(pedal.getLevelParam()->convertTo0to1(0.75f));

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            juce::MidiBuffer midi;
            buffer.clear();

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(0, sample, 0.42f * std::sin(0.21f * (float) sample));

            pedal.processBlock(buffer, midi);

            float maxLeft = 0.0f;
            float maxRight = 0.0f;
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                maxLeft = juce::jmax(maxLeft, std::abs(buffer.getSample(0, sample)));
                maxRight = juce::jmax(maxRight, std::abs(buffer.getSample(1, sample)));
            }

            expect(maxLeft > 0.02f, "Driven channel should produce a measurable wet signal");
            expect(maxRight < 1.0e-5f, "Silent channel should remain silent");
        }

        beginTest("Overdrive mix zero keeps the dry path transparent");
        {
            OverdrivePedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.getDriveParam()->setValueNotifyingHost(pedal.getDriveParam()->convertTo0to1(100.0f));
            pedal.getToneParam()->setValueNotifyingHost(pedal.getToneParam()->convertTo0to1(0.1f));
            pedal.getTextureParam()->setValueNotifyingHost(pedal.getTextureParam()->convertTo0to1(1.0f));
            pedal.getMixParam()->setValueNotifyingHost(pedal.getMixParam()->convertTo0to1(0.0f));
            pedal.getLevelParam()->setValueNotifyingHost(pedal.getLevelParam()->convertTo0to1(0.75f));

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;

            const std::vector<float> left{ 0.24f, -0.48f, 0.72f, -0.31f };
            const std::vector<float> right{ -0.18f, 0.27f, -0.36f, 0.45f };
            buffer.copyFrom(0, 0, left.data(), (int) left.size());
            buffer.copyFrom(1, 0, right.data(), (int) right.size());

            pedal.processBlock(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 3.0e-4f);
        }

        beginTest("OverdrivePedal round-trips its modern preamp state");
        {
            OverdrivePedal source;
            source.getDriveParam()->setValueNotifyingHost(source.getDriveParam()->convertTo0to1(66.0f));
            source.getToneParam()->setValueNotifyingHost(source.getToneParam()->convertTo0to1(0.71f));
            source.getTextureParam()->setValueNotifyingHost(source.getTextureParam()->convertTo0to1(0.63f));
            source.getMixParam()->setValueNotifyingHost(source.getMixParam()->convertTo0to1(0.84f));
            source.getLevelParam()->setValueNotifyingHost(source.getLevelParam()->convertTo0to1(0.79f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            OverdrivePedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.getDriveParam()->get(), 66.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.getToneParam()->get(), 0.71f, 1.0e-3f));
            expect(approximatelyEqual(restored.getTextureParam()->get(), 0.63f, 1.0e-3f));
            expect(approximatelyEqual(restored.getMixParam()->get(), 0.84f, 1.0e-3f));
            expect(approximatelyEqual(restored.getLevelParam()->get(), 0.79f, 1.0e-3f));
        }

        beginTest("OverdrivePedal tone reshapes brightness and texture changes the voice");
        {
            juce::AudioBuffer<float> input(2, (int) (kSampleRate * 1.0));
            for (int ch = 0; ch < input.getNumChannels(); ++ch)
            {
                for (int i = 0; i < input.getNumSamples(); ++i)
                {
                    const float fundamental = std::sin((float) (2.0 * juce::MathConstants<double>::pi * 185.0 * (double) i / kSampleRate));
                    const float upper = std::sin((float) (2.0 * juce::MathConstants<double>::pi * 1480.0 * (double) i / kSampleRate));
                    input.setSample(ch, i, 0.15f * fundamental + 0.09f * upper);
                }
            }

            OverdrivePedal darkSmooth;
            darkSmooth.prepareToPlay(kSampleRate, kBlockSize);
            darkSmooth.getDriveParam()->setValueNotifyingHost(darkSmooth.getDriveParam()->convertTo0to1(62.0f));
            darkSmooth.getToneParam()->setValueNotifyingHost(darkSmooth.getToneParam()->convertTo0to1(0.14f));
            darkSmooth.getTextureParam()->setValueNotifyingHost(darkSmooth.getTextureParam()->convertTo0to1(0.12f));
            darkSmooth.getMixParam()->setValueNotifyingHost(darkSmooth.getMixParam()->convertTo0to1(1.0f));
            darkSmooth.getLevelParam()->setValueNotifyingHost(darkSmooth.getLevelParam()->convertTo0to1(0.75f));
            darkSmooth.reset();
            const auto darkOutput = renderOverdriveOutput(darkSmooth, input, kBlockSize);

            OverdrivePedal brightTextured;
            brightTextured.prepareToPlay(kSampleRate, kBlockSize);
            brightTextured.getDriveParam()->setValueNotifyingHost(brightTextured.getDriveParam()->convertTo0to1(62.0f));
            brightTextured.getToneParam()->setValueNotifyingHost(brightTextured.getToneParam()->convertTo0to1(0.92f));
            brightTextured.getTextureParam()->setValueNotifyingHost(brightTextured.getTextureParam()->convertTo0to1(0.88f));
            brightTextured.getMixParam()->setValueNotifyingHost(brightTextured.getMixParam()->convertTo0to1(1.0f));
            brightTextured.getLevelParam()->setValueNotifyingHost(brightTextured.getLevelParam()->convertTo0to1(0.75f));
            brightTextured.reset();
            const auto brightOutput = renderOverdriveOutput(brightTextured, input, kBlockSize);

            const double darkRms = computeWindowRms(darkOutput, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.45));
            const double brightRms = computeWindowRms(brightOutput, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.45));

            expect(std::abs(brightRms - darkRms) > 0.030,
                "Dark and bright settings should not collapse to the same average energy");
            expect(computeBufferNullRms(darkOutput, brightOutput) > 0.050,
                "Tone and texture changes should materially reshape the overdrive voice");
        }

        beginTest("OverdrivePedal automation stress remains finite under aggressive changes");
        {
            OverdrivePedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);

            for (int iteration = 0; iteration < 120; ++iteration)
            {
                const float t = (float) iteration / 119.0f;
                pedal.getDriveParam()->setValueNotifyingHost(pedal.getDriveParam()->convertTo0to1(6.0f + 90.0f * std::abs(std::sin(t * 4.8f))));
                pedal.getToneParam()->setValueNotifyingHost(pedal.getToneParam()->convertTo0to1(juce::jmap(std::sin(t * 9.0f), -1.0f, 1.0f, 0.02f, 0.98f)));
                pedal.getTextureParam()->setValueNotifyingHost(pedal.getTextureParam()->convertTo0to1(juce::jmap(std::cos(t * 7.3f + 0.4f), -1.0f, 1.0f, 0.04f, 0.96f)));
                pedal.getMixParam()->setValueNotifyingHost(pedal.getMixParam()->convertTo0to1(juce::jmap(std::sin(t * 10.4f + 0.8f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.getLevelParam()->setValueNotifyingHost(pedal.getLevelParam()->convertTo0to1(juce::jmap(std::cos(t * 6.1f), -1.0f, 1.0f, 0.45f, 0.88f)));

                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                {
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * kBlockSize + i);
                        const float sample = 0.22f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 147.0 * sampleIndex / kSampleRate) + ch * 0.29f);
                        block.setSample(ch, i, sample);
                    }
                }

                pedal.processBlock(block, midi);
                expect(bufferHasOnlyFiniteSamples(block), "Automation should keep Overdrive output finite");
            }
        }

        beginTest("NeuralPedal round-trips its adaptive preamp state");
        {
            NeuralPedal source;
            source.driveParam->setValueNotifyingHost(source.driveParam->convertTo0to1(73.0f));
            source.focusParam->setValueNotifyingHost(source.focusParam->convertTo0to1(0.68f));
            source.detailParam->setValueNotifyingHost(source.detailParam->convertTo0to1(0.62f));
            source.compParam->setValueNotifyingHost(source.compParam->convertTo0to1(0.48f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.86f));
            source.levelParam->setValueNotifyingHost(source.levelParam->convertTo0to1(0.75f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            NeuralPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.driveParam->get(), 73.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.focusParam->get(), 0.68f, 1.0e-3f));
            expect(approximatelyEqual(restored.detailParam->get(), 0.62f, 1.0e-3f));
            expect(approximatelyEqual(restored.compParam->get(), 0.48f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.86f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 0.75f, 1.0e-3f));
        }

        beginTest("NeuralPedal mix zero keeps the dry path transparent");
        {
            NeuralPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.driveParam->setValueNotifyingHost(pedal.driveParam->convertTo0to1(94.0f));
            pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.82f));
            pedal.detailParam->setValueNotifyingHost(pedal.detailParam->convertTo0to1(0.78f));
            pedal.compParam->setValueNotifyingHost(pedal.compParam->convertTo0to1(0.68f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.75f));

            const int totalSamples = (int) (kSampleRate * 1.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phaseL = juce::MathConstants<float>::twoPi * 123.0f * (float) i / (float) kSampleRate;
                const float phaseR = juce::MathConstants<float>::twoPi * 247.0f * (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.16f * std::sin(phaseL));
                input.setSample(1, i, 0.13f * std::sin(phaseR));
            }

            const auto output = renderNeuralOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Dry-only neural render must stay finite");
            expect(nullRms < 1.0e-5, "Mix at zero should leave the dry path effectively untouched");
        }

        beginTest("NeuralPedal focus tightens the low end under heavy drive");
        {
            auto renderPedal = [&](float focusAmount)
            {
                NeuralPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.driveParam->setValueNotifyingHost(pedal.driveParam->convertTo0to1(88.0f));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(focusAmount));
                pedal.detailParam->setValueNotifyingHost(pedal.detailParam->convertTo0to1(0.52f));
                pedal.compParam->setValueNotifyingHost(pedal.compParam->convertTo0to1(0.46f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.75f));

                const int totalSamples = (int) (kSampleRate * 1.4);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 82.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.21f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderNeuralOutput(pedal, input, kBlockSize);
            };

            const auto loose = renderPedal(0.08f);
            const auto tight = renderPedal(0.92f);
            const double looseRms = computeWindowRms(loose, (int) (kSampleRate * 0.35), (int) (kSampleRate * 0.55));
            const double tightRms = computeWindowRms(tight, (int) (kSampleRate * 0.35), (int) (kSampleRate * 0.55));

            expect(tightRms < looseRms * 0.70, "High focus should materially tighten low-end energy");
        }

        beginTest("NeuralPedal automation stress remains finite under aggressive changes");
        {
            NeuralPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.82f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.75f));

            juce::Random rng(0xA1145);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            bool finite = true;
            double peak = 0.0;

            const int blocksToRun = (int) ((kSampleRate * 3.0) / (double) kBlockSize);
            for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlockSize; ++i)
                        block.setSample(ch, i, 0.17f * ((rng.nextFloat() * 2.0f) - 1.0f));

                const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                pedal.driveParam->setValueNotifyingHost(pedal.driveParam->convertTo0to1(8.0f + 90.0f * phase));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.06f + 0.92f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.detailParam->setValueNotifyingHost(pedal.detailParam->convertTo0to1(0.08f + 0.88f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.compParam->setValueNotifyingHost(pedal.compParam->convertTo0to1(phase));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.12f + 0.86f * phase));

                pedal.processBlock(block, midi);

                finite = finite && bufferHasOnlyFiniteSamples(block);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, kBlockSize));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, kBlockSize));
            }

            expect(finite, "Aggressive neural automation must remain finite");
            expect(peak < 2.2, "Neural automation stress should stay inside a sane peak ceiling");
        }

        beginTest("Global processors preserve active params after reset");
        {
            juce::MidiBuffer midi;

            InputChainProcessor input;
            input.prepareToPlay(kSampleRate, kBlockSize);
            input.setParams(6.0f, -100.0f, false);
            input.reset();

            juce::AudioBuffer<float> inputBuffer(2, 4);
            inputBuffer.clear();
            inputBuffer.setSample(0, 0, 0.5f);
            inputBuffer.setSample(1, 0, 0.5f);
            input.processBlock(inputBuffer, midi);

            expect(inputBuffer.getSample(0, 0) > 0.95f, "Input gain should survive reset");
            expect(inputBuffer.getSample(1, 0) > 0.95f, "Input gain should survive reset on both channels");

            ChannelStripProcessor strip;
            strip.prepareToPlay(kSampleRate, kBlockSize);
            strip.setParams(0.5f, 0.0f, 1.0f);
            strip.reset();

            juce::AudioBuffer<float> stripBuffer(2, 4);
            stripBuffer.clear();
            stripBuffer.setSample(0, 0, 1.0f);
            stripBuffer.setSample(1, 0, 1.0f);
            strip.processBlock(stripBuffer, midi);

            expect(approximatelyEqual(stripBuffer.getSample(0, 0), 0.5f, 2.0e-4f), "Strip gain should survive reset");
            expect(approximatelyEqual(stripBuffer.getSample(1, 0), 0.5f, 2.0e-4f), "Strip gain should survive reset on both channels");

            OutputChainProcessor output;
            output.prepareToPlay(kSampleRate, kBlockSize);
            output.setParams(-6.0f, 0.0f);
            output.reset();

            // P12D: limiter is always armed, so OutputChain has a stable lookahead
            // delay. A 4-sample impulse buffer is shorter than the lookahead, so we
            // drive a sustained sine instead and verify steady-state master gain.
            constexpr int kSettleBlocks = 12;
            juce::AudioBuffer<float> outputBuffer(2, kBlockSize);
            float observedPeakL = 0.0f;
            float observedPeakR = 0.0f;
            for (int blockIndex = 0; blockIndex < kSettleBlocks; ++blockIndex)
            {
                for (int i = 0; i < kBlockSize; ++i)
                {
                    const float t = (float)(blockIndex * kBlockSize + i) / (float)kSampleRate;
                    const float sample = std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                    outputBuffer.setSample(0, i, sample);
                    outputBuffer.setSample(1, i, sample);
                }

                output.processBlock(outputBuffer, midi);

                if (blockIndex >= kSettleBlocks - 2)
                {
                    observedPeakL = juce::jmax(observedPeakL,
                        outputBuffer.getMagnitude(0, 0, kBlockSize));
                    observedPeakR = juce::jmax(observedPeakR,
                        outputBuffer.getMagnitude(1, 0, kBlockSize));
                }
            }

            expect(observedPeakL < 0.55f && observedPeakL > 0.45f,
                "Output gain should survive reset, peakL=" + juce::String(observedPeakL, 6));
            expect(observedPeakR < 0.55f && observedPeakR > 0.45f,
                "Output gain should survive reset on both channels, peakR=" + juce::String(observedPeakR, 6));
        }

        beginTest("GraphRetirementQueue preserves grace period and bounded cleanup");
        {
            struct DummyGraphRuntime
            {
                int tag = 0;
            };

            Nova::Audio::GraphRetirementQueue<DummyGraphRuntime> queue;

            {
                auto graph = std::make_shared<DummyGraphRuntime>();
                graph->tag = 1;
                std::weak_ptr<DummyGraphRuntime> weak = graph;

                queue.retire(std::move(graph), 10);
                expectEquals((int)queue.size(), 1);

                queue.cleanup(9);
                expectEquals((int)queue.size(), 1);
                expect(!weak.expired(), "Graph should stay retained before its release block");

                queue.cleanup(10);
                expectEquals((int)queue.size(), 0);
                expect(weak.expired(), "Graph should be released at its release block");
            }

            std::vector<std::weak_ptr<DummyGraphRuntime>> retired;
            retired.reserve(12);

            for (int i = 0; i < 12; ++i)
            {
                auto graph = std::make_shared<DummyGraphRuntime>();
                graph->tag = i;
                retired.push_back(graph);
                queue.retire(std::move(graph), 100);
            }

            queue.cleanup(0);
            expectEquals((int)queue.size(), 8);

            for (int i = 0; i < 4; ++i)
                expect(retired[(size_t)i].expired(), "Bounded cleanup should release oldest retired graph " + juce::String(i));

            for (int i = 4; i < 12; ++i)
                expect(!retired[(size_t)i].expired(), "Bounded cleanup should keep newest retired graph " + juce::String(i));

            queue.clear();
            expectEquals((int)queue.size(), 0);

            for (const auto& weak : retired)
                expect(weak.expired(), "clear() should release all retired graphs");
        }

        beginTest("RuntimeGraphManager publishes raw pointer before retiring old graph");
        {
            struct DummyGraphRuntime
            {
                int latencySamples = 0;
            };

            Nova::Audio::RuntimeGraphManager<DummyGraphRuntime> manager;

            auto first = std::make_shared<DummyGraphRuntime>();
            first->latencySamples = 7;
            auto* firstRaw = first.get();
            std::weak_ptr<DummyGraphRuntime> firstWeak = first;

            bool callbackSawPublishedRaw = false;
            int callbackLatency = -1;
            expect(manager.publish(std::move(first), 3,
                [&](int latencySamples)
                {
                    callbackSawPublishedRaw = manager.getActiveRaw() == firstRaw;
                    callbackLatency = latencySamples;
                }));

            expect(callbackSawPublishedRaw, "Publish callback should run after raw pointer publication");
            expectEquals(callbackLatency, 7);
            expect(manager.getActiveRaw() == firstRaw, "Active raw pointer should point at the published graph");
            expectEquals(manager.getLatencySamples(), 7);
            expect(!firstWeak.expired(), "Active owner should retain the first graph");

            auto second = std::make_shared<DummyGraphRuntime>();
            second->latencySamples = 11;
            auto* secondRaw = second.get();
            std::weak_ptr<DummyGraphRuntime> secondWeak = second;

            expect(manager.publish(std::move(second), 4,
                [&](int latencySamples)
                {
                    callbackSawPublishedRaw = manager.getActiveRaw() == secondRaw;
                    callbackLatency = latencySamples;
                }));

            expect(callbackSawPublishedRaw, "Second publish callback should see the new raw pointer");
            expectEquals(callbackLatency, 11);
            expect(manager.getActiveRaw() == secondRaw, "Active raw pointer should move to the second graph");
            expectEquals(manager.getLatencySamples(), 11);

            manager.cleanupRetired(11);
            expect(!firstWeak.expired(), "Retired graph should stay alive before current block reaches publish block plus grace");

            manager.cleanupRetired(12);
            expect(firstWeak.expired(), "Retired graph should release after exactly 8 grace blocks");
            expect(!secondWeak.expired(), "Active graph should remain owned after retired cleanup");

            manager.clear();
            expect(manager.getActiveRaw() == nullptr, "clear() should clear the active raw pointer");
            expect(secondWeak.expired(), "clear() should release the active graph owner");
        }

        beginTest("AudioEngine publishes an active graph immediately after prepare");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            auto report = engine.buildDiagnosticReport();
            expect(report.contains("activeGraph=true"), "prepare() should publish a non-null active graph: " + report);
            expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), 0);
            expectEquals((int)engine.getNodes(Nova::ChainID::LineB).size(), 0);

            engine.setEngineEnabled(false);
            engine.synchronizeProcessingState();
            report = engine.buildDiagnosticReport();
            expect(report.contains("activeGraph=true"), "Disabled engine should still keep an active graph owner: " + report);
            expect(report.contains("engineOn=false"), "Disabled engine state should be reflected in diagnostics: " + report);
        }

        beginTest("AudioEngine single-line mode preserves clean input within conditioning tolerance");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize);

            // P12D: keep the test pattern peak below the OutputChain transparent
            // safety threshold (~0.97 linear) so the always-armed limiter does
            // not reduce gain. This preserves the bit-accurate clean-path check.
            const std::vector<float> left{ 0.225f, -0.45f, 0.675f, -0.9f };
            const std::vector<float> right{ -0.18f, 0.36f, -0.54f, 0.72f };

            expectStereoSamplesMatchAfterLatency(*this, engine, left, right, kBlockSize, 3.5e-3f);
        }

        beginTest("AudioEngine disabled state preserves dry input");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.inputGainDb = 18.0f;
            params.outputVolumeDb = -12.0f;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
            warmUpEngine(engine, kBlockSize);

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;

            const std::vector<float> left{ 0.32f, -0.16f, 0.48f, -0.24f };
            const std::vector<float> right{ -0.11f, 0.22f, -0.33f, 0.44f };

            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());

            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 2.0e-4f);
        }

        beginTest("AudioEngine parallel routing keeps practical unity on identical clean lines");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::Dual_Parallel;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize);

            const std::vector<float> left{ 0.1f, 0.2f, -0.3f, 0.4f };
            const std::vector<float> right{ -0.4f, 0.3f, -0.2f, 0.1f };

            expectStereoSamplesMatchAfterLatency(*this, engine, left, right, kBlockSize, 3.5e-3f);
        }

        beginTest("AudioEngine base path stays finite and level-stable across sample rates and block sizes");
        {
            const struct Scenario
            {
                double sampleRate;
                int blockSize;
            } scenarios[] = {
                { 44100.0, 64 },
                { 48000.0, 127 },
                { 96000.0, 513 }
            };

            for (const auto& scenario : scenarios)
            {
                AudioEngine engine;
                engine.prepare(scenario.sampleRate, scenario.blockSize, 2, 2);

                AudioEngine::RuntimeGlobalParams params;
                params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
                params.outputMixRaw = 100.0f;
                engine.updateGlobalParams(params);
                engine.setEngineEnabled(true);
                warmUpEngine(engine, scenario.blockSize, 16);

                juce::AudioBuffer<float> buffer(2, scenario.blockSize);
                juce::MidiBuffer midi;
                double inputSquares = 0.0;
                double outputSquares = 0.0;
                int measuredSamples = 0;
                bool finite = true;
                int globalSample = 0;

                for (int block = 0; block < 96; ++block)
                {
                    for (int i = 0; i < scenario.blockSize; ++i, ++globalSample)
                    {
                        const float t = (float)((double)globalSample / scenario.sampleRate);
                        const float left = 0.18f * std::sin(juce::MathConstants<float>::twoPi * 1000.0f * t);
                        const float right = 0.14f * std::sin(juce::MathConstants<float>::twoPi * 1300.0f * t + 0.37f);
                        buffer.setSample(0, i, left);
                        buffer.setSample(1, i, right);

                        if (block >= 16)
                        {
                            inputSquares += (double)left * (double)left;
                            inputSquares += (double)right * (double)right;
                        }
                    }

                    engine.process(buffer, midi);
                    finite = finite && bufferHasOnlyFiniteSamples(buffer);

                    if (block >= 16)
                    {
                        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        {
                            for (int i = 0; i < buffer.getNumSamples(); ++i)
                            {
                                const double sample = buffer.getSample(ch, i);
                                outputSquares += sample * sample;
                                ++measuredSamples;
                            }
                        }
                    }
                }

                const double inputRms = std::sqrt(inputSquares / juce::jmax(1, measuredSamples));
                const double outputRms = std::sqrt(outputSquares / juce::jmax(1, measuredSamples));
                const double ratio = outputRms / juce::jmax(1.0e-9, inputRms);

                expect(finite, "Base path render must remain finite at sampleRate=" + juce::String(scenario.sampleRate));
                expect(ratio > 0.97 && ratio < 1.03,
                    "Base path should stay near unity at sampleRate=" + juce::String(scenario.sampleRate)
                        + ", blockSize=" + juce::String(scenario.blockSize)
                        + ", ratio=" + juce::String(ratio, 6));
            }
        }

        beginTest("AudioEngine no-pedal base path promotes one-channel guitar input at unity");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, 128, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            params.inputGainDb = 0.0f;
            params.outputVolumeDb = 0.0f;
            params.outputLimiterDb = 0.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, 128, 24);

            juce::AudioBuffer<float> buffer(2, 128);
            juce::MidiBuffer midi;
            double inputRightSquares = 0.0;
            double outputLeftSquares = 0.0;
            double outputRightSquares = 0.0;
            int measuredSamples = 0;
            bool finite = true;
            int globalSample = 0;

            for (int block = 0; block < 160; ++block)
            {
                buffer.clear();
                for (int i = 0; i < buffer.getNumSamples(); ++i, ++globalSample)
                {
                    const float t = (float)((double)globalSample / kSampleRate);
                    const float rightOnly = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 880.0f * t);
                    buffer.setSample(1, i, rightOnly);

                    if (block >= 24)
                        inputRightSquares += (double)rightOnly * (double)rightOnly;
                }

                engine.process(buffer, midi);
                finite = finite && bufferHasOnlyFiniteSamples(buffer);

                if (block < 24)
                    continue;

                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const double left = buffer.getSample(0, i);
                    const double right = buffer.getSample(1, i);
                    outputLeftSquares += left * left;
                    outputRightSquares += right * right;
                    ++measuredSamples;
                }
            }

            const double inputRightRms = std::sqrt(inputRightSquares / juce::jmax(1, measuredSamples));
            const double outputLeftRms = std::sqrt(outputLeftSquares / juce::jmax(1, measuredSamples));
            const double outputRightRms = std::sqrt(outputRightSquares / juce::jmax(1, measuredSamples));
            const double leftRatio = outputLeftRms / juce::jmax(1.0e-9, inputRightRms);
            const double rightRatio = outputRightRms / juce::jmax(1.0e-9, inputRightRms);

            expect(finite, "One-channel guitar base-path render must remain finite");
            expect(leftRatio > 0.92 && leftRatio < 1.08,
                "Left output should recover single-jack guitar level without makeup gain, ratio=" + juce::String(leftRatio, 6));
            expect(rightRatio > 0.92 && rightRatio < 1.08,
                "Right output should preserve single-jack guitar level without makeup gain, ratio=" + juce::String(rightRatio, 6));
        }

        beginTest("AudioEngine no-pedal realistic guitar program stays audible and limiter-protected");
        {
            constexpr double qaSampleRate = 48000.0;
            constexpr int qaBlockSize = 128;
            constexpr int totalBlocks = 520;

            AudioEngine engine;
            engine.prepare(qaSampleRate, qaBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.inputGainDb = 6.0f;
            params.gateThresholdDb = -100.0f;
            params.outputVolumeDb = 0.0f;
            params.outputLimiterDb = -6.0f;
            params.outputMixRaw = 100.0f;
            params.gainA = 1.0f;
            params.panA = 0.0f;
            params.widthA = 1.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, qaBlockSize, 24);

            juce::AudioBuffer<float> buffer(2, qaBlockSize);
            juce::MidiBuffer midi;
            double outputSquares = 0.0;
            double outputMeanL = 0.0;
            double outputMeanR = 0.0;
            float outputPeak = 0.0f;
            bool finite = true;
            int measuredFrames = 0;
            int globalSample = 0;

            for (int block = 0; block < totalBlocks; ++block)
            {
                for (int i = 0; i < qaBlockSize; ++i, ++globalSample)
                {
                    const double t = (double)globalSample / qaSampleRate;
                    const double pluckPhase = std::fmod(t, 0.245);
                    const float envelope = (float)std::exp(-pluckPhase * 18.0);
                    const float pick = pluckPhase < 0.0015
                        ? (float)(1.0 - (pluckPhase / 0.0015))
                        : 0.0f;

                    const float body = (float)(0.19 * (double)envelope * (
                        std::sin(juce::MathConstants<double>::twoPi * 82.41 * t)
                        + 0.43f * std::sin(juce::MathConstants<double>::twoPi * 164.82 * t + 0.12)
                        + 0.21f * std::sin(juce::MathConstants<double>::twoPi * 246.94 * t + 0.31)));
                    const float transient = (float)(0.48 * (double)pick * std::sin(juce::MathConstants<double>::twoPi * 2600.0 * t));
                    const float left = body + transient;
                    const float right = 0.92f * body + 0.76f * transient;

                    buffer.setSample(0, i, left);
                    buffer.setSample(1, i, right);
                }

                engine.process(buffer, midi);
                finite = finite && bufferHasOnlyFiniteSamples(buffer);

                if (block < 32)
                    continue;

                for (int i = 0; i < qaBlockSize; ++i)
                {
                    const float left = buffer.getSample(0, i);
                    const float right = buffer.getSample(1, i);
                    outputPeak = juce::jmax(outputPeak, std::abs(left));
                    outputPeak = juce::jmax(outputPeak, std::abs(right));
                    outputSquares += (double)left * (double)left;
                    outputSquares += (double)right * (double)right;
                    outputMeanL += left;
                    outputMeanR += right;
                    ++measuredFrames;
                }
            }

            const double outputRms = std::sqrt(outputSquares / juce::jmax(1, measuredFrames * 2));
            const double dcL = std::abs(outputMeanL / juce::jmax(1, measuredFrames));
            const double dcR = std::abs(outputMeanR / juce::jmax(1, measuredFrames));

            expect(finite, "No-pedal realistic program must remain finite");
            expect(outputRms > 0.025 && outputRms < 0.26,
                "No-pedal realistic program should stay audible without runaway level, rms=" + juce::String(outputRms, 6));
            expect(outputPeak <= 0.58f,
                "Limiter should keep realistic no-pedal transients below the -6 dBFS ceiling allowance, peak=" + juce::String(outputPeak, 6));
            expect(dcL < 0.012 && dcR < 0.012,
                "No-pedal base path should stay centered after input/output conditioning");
        }

        beginTest("AudioEngine dry-only mix bypasses wet-path gain changes");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.inputGainDb = 12.0f;
            params.outputMixRaw = 0.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize);

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;

            const std::vector<float> left{ 0.15f, -0.25f, 0.35f, -0.45f };
            const std::vector<float> right{ -0.05f, 0.15f, -0.25f, 0.35f };

            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());

            engine.process(buffer, midi);
            expect(engine.buildDiagnosticReport().contains("wetMix=0"),
                "Dry-only engine diagnostic: " + engine.buildDiagnosticReport());
            expectStereoSamplesMatch(*this, buffer, left, right, 2.0e-4f);
        }

        beginTest("AudioEngine dry-only path stays exact with latency-relevant wet chain");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "dryonly-overdrive");
            engine.addPedal("Delay", Nova::ChainID::LineA, -1, Nova::ZoneID::FX, "dryonly-delay");
            engine.synchronizeProcessingState();

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 0.0f;
            params.outputLimiterDb = 0.0f;
            params.outputVolumeDb = 0.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 16);

            const int chainLatency = engine.getLatencyNumSamples();
            expect(chainLatency > 0, "Latency-relevant wet chain should still report positive graph latency");

            juce::AudioBuffer<float> block(2, kBlockSize);
            juce::AudioBuffer<float> inputCopy(2, kBlockSize);
            juce::MidiBuffer midi;

            bool finite = true;
            double maxNullRms = 0.0;
            int globalSample = 0;

            for (int blockIndex = 0; blockIndex < 24; ++blockIndex)
            {
                for (int i = 0; i < kBlockSize; ++i, ++globalSample)
                {
                    const float t = (float)((double)globalSample / kSampleRate);
                    block.setSample(0, i, 0.18f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t));
                    block.setSample(1, i, 0.13f * std::sin(juce::MathConstants<float>::twoPi * 277.0f * t + 0.21f));
                }

                inputCopy.makeCopyOf(block);
                engine.process(block, midi);

                finite = finite && bufferHasOnlyFiniteSamples(block);
                maxNullRms = juce::jmax(maxNullRms, computeBufferNullRms(inputCopy, block));
                expectEquals(engine.getLatencyNumSamples(), chainLatency);
            }

            expect(finite, "Dry-only render with wet chain present must stay finite");
            expect(maxNullRms < 2.5e-4,
                "Dry-only path should remain exact and drift-free, maxNullRms=" + juce::String(maxNullRms, 10));
        }

        beginTest("AudioEngine wet-only path reflects topology changes and stays audible");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            params.outputLimiterDb = 0.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 16);

            juce::AudioBuffer<float> source(2, kBlockSize);
            for (int i = 0; i < kBlockSize; ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                source.setSample(0, i, 0.16f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t));
                source.setSample(1, i, 0.12f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t + 0.19f));
            }

            auto renderOneBlock = [&](const juce::AudioBuffer<float>& input)
            {
                // P12D: OutputChain always reports lookahead latency, so a single
                // block of input would arrive after the buffer ends. Prime the
                // delay line with copies of the same periodic signal first.
                const int latency = engine.getLatencyNumSamples();
                const int primingBlocks = juce::jmax(2,
                    (latency + input.getNumSamples() - 1) / input.getNumSamples() + 1);
                juce::MidiBuffer midi;
                for (int i = 0; i < primingBlocks; ++i)
                {
                    juce::AudioBuffer<float> scratch(input.getNumChannels(), input.getNumSamples());
                    scratch.makeCopyOf(input);
                    engine.process(scratch, midi);
                }
                juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
                output.makeCopyOf(input);
                engine.process(output, midi);
                return output;
            };

            const auto baseline = renderOneBlock(source);
            expect(bufferHasOnlyFiniteSamples(baseline), "Baseline wet-only render must be finite");
            expect(computeWindowRms(baseline, 0, baseline.getNumSamples()) > 1.0e-4,
                "Baseline wet-only render must stay audible");

            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "wet-only-overdrive");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 12);

            const auto withPedal = renderOneBlock(source);
            expect(bufferHasOnlyFiniteSamples(withPedal), "Wet-only render with pedal must be finite");
            expect(computeWindowRms(withPedal, 0, withPedal.getNumSamples()) > 1.0e-4,
                "Wet-only render with pedal must stay audible");
            expect(computeBufferNullRms(baseline, withPedal) > 3.0e-3,
                "Topology change should alter wet-only output");

            engine.removePedal(Nova::ChainID::LineA, 0);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 16);

            const auto restored = renderOneBlock(source);
            expect(bufferHasOnlyFiniteSamples(restored), "Wet-only restored render must be finite");
            expect(computeBufferNullRms(baseline, restored) < 4.5e-3,
                "Removing the pedal should restore the baseline wet-only clean path");
        }

        beginTest("AudioEngine sample-accurate dry-wet ramp avoids abrupt discontinuities");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "ramp-overdrive");
            engine.synchronizeProcessingState();

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 0.0f;
            params.outputLimiterDb = 0.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 16);

            constexpr int totalBlocks = 24;
            constexpr int mixUpAtBlock = 8;
            constexpr int mixDownAtBlock = 16;
            juce::AudioBuffer<float> captured(2, kBlockSize * totalBlocks);
            juce::MidiBuffer midi;

            int globalSample = 0;
            for (int blockIndex = 0; blockIndex < totalBlocks; ++blockIndex)
            {
                if (blockIndex == mixUpAtBlock)
                {
                    params.outputMixRaw = 100.0f;
                    engine.updateGlobalParams(params);
                }
                else if (blockIndex == mixDownAtBlock)
                {
                    params.outputMixRaw = 0.0f;
                    engine.updateGlobalParams(params);
                }

                juce::AudioBuffer<float> block(2, kBlockSize);
                for (int i = 0; i < kBlockSize; ++i, ++globalSample)
                {
                    const float t = (float)((double)globalSample / kSampleRate);
                    block.setSample(0, i, 0.17f * std::sin(juce::MathConstants<float>::twoPi * 247.0f * t));
                    block.setSample(1, i, 0.11f * std::sin(juce::MathConstants<float>::twoPi * 311.0f * t + 0.23f));
                }

                engine.process(block, midi);
                expect(bufferHasOnlyFiniteSamples(block), "Ramp block must stay finite");

                for (int ch = 0; ch < 2; ++ch)
                    captured.copyFrom(ch, blockIndex * kBlockSize, block, ch, 0, kBlockSize);
            }

            const int preStart = 2 * kBlockSize;
            const int preLength = 4 * kBlockSize;
            const int transitionUpStart = (mixUpAtBlock - 1) * kBlockSize;
            const int transitionUpLength = 3 * kBlockSize;
            const int transitionDownStart = (mixDownAtBlock - 1) * kBlockSize;
            const int transitionDownLength = 3 * kBlockSize;
            const int postStart = (mixDownAtBlock + 2) * kBlockSize;
            const int postLength = 4 * kBlockSize;

            const double preDelta = computeAdjacentDeltaPeakRange(captured, preStart, preLength);
            const double transitionUpDelta = computeAdjacentDeltaPeakRange(captured, transitionUpStart, transitionUpLength);
            const double transitionDownDelta = computeAdjacentDeltaPeakRange(captured, transitionDownStart, transitionDownLength);
            const double postDelta = computeAdjacentDeltaPeakRange(captured, postStart, postLength);
            const double steadyReference = juce::jmax(preDelta, postDelta);

            expect(transitionUpDelta < (steadyReference * 30.0 + 0.02) && transitionUpDelta < 0.30,
                "Dry->wet ramp should avoid abrupt jump, transitionDelta="
                    + juce::String(transitionUpDelta, 8)
                    + ", steadyRef="
                    + juce::String(steadyReference, 8));
            expect(transitionDownDelta < (steadyReference * 30.0 + 0.02) && transitionDownDelta < 0.30,
                "Wet->dry ramp should avoid abrupt jump, transitionDelta="
                    + juce::String(transitionDownDelta, 8)
                    + ", steadyRef="
                    + juce::String(steadyReference, 8));
        }

        beginTest("P12D OutputChain transparent safety catches hot master before soft ceiling");
        {
            // Regression for P12C-confirmed escape: outputVolumeDb >> 0 with
            // outputLimiterDb = 0 previously bypassed the lookahead limiter and
            // pinned output at the 0.999 soft ceiling, producing sustained
            // near-clip in manual logs (see "Preset con master alto.txt").
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            params.outputVolumeDb = 10.73f; // match the bad manual log exactly
            params.outputLimiterDb = 0.0f;  // ship-default; was the bypass path
            params.gainA = 1.0f;
            params.panA = 0.0f;
            params.widthA = 1.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 32);

            constexpr int kCaptureBlocks = 64;
            juce::AudioBuffer<float> capture(2, kBlockSize * kCaptureBlocks);
            juce::MidiBuffer midi;
            int globalSample = 0;
            bool finite = true;

            for (int blockIndex = 0; blockIndex < kCaptureBlocks; ++blockIndex)
            {
                juce::AudioBuffer<float> block(2, kBlockSize);
                for (int i = 0; i < kBlockSize; ++i, ++globalSample)
                {
                    const float t = (float)((double)globalSample / kSampleRate);
                    // 0.5 amplitude * +10.73 dB master (~x3.44) -> 1.72 linear,
                    // well above both the 0.97 transparent threshold and the
                    // 0.999 soft ceiling. Without the fix, output peaks at 0.999.
                    block.setSample(0, i, 0.50f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t));
                    block.setSample(1, i, 0.50f * std::sin(juce::MathConstants<float>::twoPi * 277.0f * t + 0.19f));
                }

                engine.process(block, midi);
                finite = finite && bufferHasOnlyFiniteSamples(block);

                for (int ch = 0; ch < 2; ++ch)
                    capture.copyFrom(ch, blockIndex * kBlockSize, block, ch, 0, kBlockSize);
            }

            float outputPeak = 0.0f;
            int aboveTransparentSafety = 0;
            int aboveSoftCeilingThreshold = 0;
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto* data = capture.getReadPointer(ch);
                for (int i = 0; i < capture.getNumSamples(); ++i)
                {
                    const float mag = std::abs(data[i]);
                    outputPeak = juce::jmax(outputPeak, mag);
                    if (mag > 0.975f)
                        ++aboveTransparentSafety;
                    if (mag > 0.985f)
                        ++aboveSoftCeilingThreshold;
                }
            }

            const auto snapshot = engine.getOutputChainDebugSnapshot();

            expect(finite, "P12D regression render must remain finite");
            expect(outputPeak < 0.98f,
                "Lookahead limiter must hold output below the soft-ceiling band; outputPeak="
                    + juce::String(outputPeak, 6));
            expect(aboveSoftCeilingThreshold == 0,
                "No sample should reach the 0.985 soft-ceiling threshold; aboveSoftCeilingThreshold="
                    + juce::String(aboveSoftCeilingThreshold));
            expect(aboveTransparentSafety < capture.getNumSamples() / 256,
                "Output should sit at or below the transparent safety threshold; aboveTransparentSafety="
                    + juce::String(aboveTransparentSafety));
            expect(snapshot.limiterMaxReductionDb > 0.5f,
                "Lookahead limiter must actually reduce gain under hot master; maxReductionDb="
                    + juce::String(snapshot.limiterMaxReductionDb, 4));
            expect(snapshot.softCeilingTouchedSamples == 0,
                "Soft ceiling must not act as the main limiter after P12D; softCeilingTouchedSamples="
                    + juce::String(snapshot.softCeilingTouchedSamples));
        }

        beginTest("P12D OutputChain stays transparent on quiet signal at default limiter");
        {
            // Companion to the P12D hot-master regression: at default limiter
            // (0 dB) with quiet input, the always-armed limiter must not touch
            // gain and must not introduce audible artefacts.
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            params.outputVolumeDb = 0.0f;
            params.outputLimiterDb = 0.0f;
            params.gainA = 1.0f;
            params.panA = 0.0f;
            params.widthA = 1.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 32);

            constexpr int kCaptureBlocks = 48;
            double inSquares = 0.0;
            double outSquares = 0.0;
            int measuredSamples = 0;
            bool finite = true;
            int globalSample = 0;
            juce::MidiBuffer midi;

            for (int blockIndex = 0; blockIndex < kCaptureBlocks; ++blockIndex)
            {
                juce::AudioBuffer<float> block(2, kBlockSize);
                for (int i = 0; i < kBlockSize; ++i, ++globalSample)
                {
                    const float t = (float)((double)globalSample / kSampleRate);
                    const float sample = 0.10f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t);
                    block.setSample(0, i, sample);
                    block.setSample(1, i, sample);
                    if (blockIndex >= 12)
                        inSquares += (double)sample * (double)sample;
                }

                engine.process(block, midi);
                finite = finite && bufferHasOnlyFiniteSamples(block);

                if (blockIndex < 12)
                    continue;

                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto* data = block.getReadPointer(ch);
                    for (int i = 0; i < kBlockSize; ++i)
                        outSquares += (double)data[i] * (double)data[i];
                }
                measuredSamples += kBlockSize;
            }

            const double inRms = std::sqrt(inSquares / juce::jmax(1, measuredSamples));
            const double outRms = std::sqrt(outSquares / juce::jmax(1, measuredSamples * 2));
            const double ratio = outRms / juce::jmax(1.0e-9, inRms);
            const auto snapshot = engine.getOutputChainDebugSnapshot();

            expect(finite, "P12D quiet-signal render must remain finite");
            expect(ratio > 0.94 && ratio < 1.06,
                "Quiet signal must pass through unchanged at default limiter; ratio="
                    + juce::String(ratio, 6));
            expect(snapshot.limiterMaxReductionDb < 0.05f,
                "Always-armed limiter must not reduce gain on quiet signal; maxReductionDb="
                    + juce::String(snapshot.limiterMaxReductionDb, 6));
            expect(snapshot.limiterActiveBlocks == 0,
                "No actual limiter clamping should occur on quiet signal; limiterActiveBlocks="
                    + juce::String(snapshot.limiterActiveBlocks));
            expect(snapshot.softCeilingTouchedSamples == 0,
                "Soft ceiling must not engage on quiet signal; softCeilingTouchedSamples="
                    + juce::String(snapshot.softCeilingTouchedSamples));
        }

        beginTest("P12D OutputChain reports lookahead latency at every limiter setting");
        {
            // Always-on limiter implies a stable, non-zero output-stage latency
            // regardless of user-controlled limiterDb. The previous build only
            // reported latency when limiterDb < -0.0001.
            OutputChainProcessor output;
            output.prepareToPlay(kSampleRate, kBlockSize);

            output.setParams(0.0f, 0.0f);
            const int latencyAtZero = output.getLatencySamples();

            output.setParams(0.0f, -6.0f);
            const int latencyAtMinusSix = output.getLatencySamples();

            output.setParams(0.0f, -12.0f);
            const int latencyAtMinusTwelve = output.getLatencySamples();

            expect(latencyAtZero > 0,
                "Output stage must report lookahead latency even at limiterDb = 0; latency="
                    + juce::String(latencyAtZero));
            expectEquals(latencyAtZero, latencyAtMinusSix);
            expectEquals(latencyAtZero, latencyAtMinusTwelve);
        }

        beginTest("P13A host PDC reports live AudioEngine graph latency");
        {
            NOVAAudioProcessor processor;
            processor.setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
            processor.prepareToPlay(kSampleRate, kBlockSize);

            const int baselineLatency = processor.getAudioEngine().getLatencyNumSamples();
            expect(baselineLatency > 0,
                "Empty graph should report OutputChain lookahead latency after prepare");
            expectEquals(processor.getLatencySamples(), baselineLatency);

            const std::array<float, 3> limiterSettings { 0.0f, -6.0f, -12.0f };
            for (float limiterDb : limiterSettings)
            {
                expect(setRangedParamById(processor, Nova::IDs::OUTPUT_LIMITER.toString(), limiterDb),
                    "Output limiter parameter should be reachable from processor tests");

                juce::AudioBuffer<float> buffer(2, kBlockSize);
                juce::MidiBuffer midi;
                processor.processBlock(buffer, midi);

                expectEquals(processor.getAudioEngine().getLatencyNumSamples(), baselineLatency);
                expectEquals(processor.getLatencySamples(), baselineLatency);
            }

            processor.requestAddPedal("Overdrive", Nova::ChainID::LineA, Nova::ZoneID::Pre, 0);
            const int activeLatency = processor.getAudioEngine().getLatencyNumSamples();
            expect(activeLatency > baselineLatency,
                "Active Overdrive should add latency above the OutputChain baseline");
            expectEquals(processor.getLatencySamples(), activeLatency);

            processor.requestBypassPedal(Nova::ChainID::LineA, 0, true);
            expectEquals(processor.getAudioEngine().getLatencyNumSamples(), baselineLatency);
            expectEquals(processor.getLatencySamples(), baselineLatency);

            processor.requestBypassPedal(Nova::ChainID::LineA, 0, false);
            expectEquals(processor.getAudioEngine().getLatencyNumSamples(), activeLatency);
            expectEquals(processor.getLatencySamples(), activeLatency);
        }

        beginTest("P13A global host bypass preserves reported latency alignment");
        {
            auto verifyBypassAlignment = [this](NOVAAudioProcessor& processor,
                int expectedLatency,
                const juce::String& context)
            {
                expect(expectedLatency > 0, context + " must report positive latency");

                const int totalSamples = expectedLatency + kBlockSize * 3;
                int firstOutputSample = -1;
                int renderedSamples = 0;
                juce::MidiBuffer midi;

                while (renderedSamples < totalSamples)
                {
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    block.clear();
                    if (renderedSamples == 0)
                    {
                        block.setSample(0, 0, 0.5f);
                        block.setSample(1, 0, 0.5f);
                    }

                    processor.processBlockBypassed(block, midi);
                    for (int sample = 0; sample < block.getNumSamples(); ++sample)
                    {
                        if (firstOutputSample < 0
                            && std::abs(block.getSample(0, sample)) > 1.0e-5f)
                        {
                            firstOutputSample = renderedSamples + sample;
                        }
                    }
                    renderedSamples += block.getNumSamples();
                }

                expectEquals(firstOutputSample, expectedLatency,
                    context + " bypassed impulse must arrive at the reported host latency");
            };

            int baselineLatency = 0;
            {
                auto baselineProcessor = std::make_unique<NOVAAudioProcessor>();
                baselineProcessor->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                baselineProcessor->prepareToPlay(kSampleRate, kBlockSize);
                baselineLatency = baselineProcessor->getLatencySamples();
                verifyBypassAlignment(*baselineProcessor,
                    baselineLatency,
                    "OutputChain baseline");
            }

            {
                auto oversampledProcessor = std::make_unique<NOVAAudioProcessor>();
                oversampledProcessor->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                oversampledProcessor->prepareToPlay(kSampleRate, kBlockSize);
                oversampledProcessor->requestAddPedal(
                    "Overdrive", Nova::ChainID::LineA, Nova::ZoneID::Pre, 0);
                const int oversampledLatency = oversampledProcessor->getLatencySamples();
                expect(oversampledLatency > baselineLatency,
                    "Enabled Overdrive must increase latency for the global bypass test");
                verifyBypassAlignment(*oversampledProcessor,
                    oversampledLatency,
                    "Oversampled Overdrive graph");
            }
        }

        beginTest("P13A preset restore preserves latency and bypass latency state");
        {
            StartupPresetPointerGuard startupPointerGuard;

            AudioEngine baselineEngine;
            baselineEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            const int baselineLatency = baselineEngine.getLatencyNumSamples();
            expect(baselineLatency > 0, "Baseline engine should report OutputChain lookahead latency");

            auto state = makeP7DState();
            if (auto settings = Nova::PluginStateModel::getSettingsTree(state); settings.isValid())
            {
                settings.setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
                settings.setProperty(Nova::IDs::SWITCH_MODE, (int) Nova::SwitcherMode::LineA_Only, nullptr);
                settings.setProperty(Nova::IDs::OUTPUT_MIX, 100.0f, nullptr);
                settings.setProperty(Nova::IDs::OUTPUT_LIMITER, 0.0f, nullptr);
            }
            appendP7DPedal(state, Nova::ChainID::LineA, "Overdrive", Nova::ZoneID::Pre, "p13a-pdc-overdrive", true);
            Nova::PluginStateModel::canonicalizeStateTree(state);

            SessionStore sourceStore;
            expect(sourceStore.applyCommand(SessionStore::Command::makeRestoreState(state)).changed,
                "Latency source state should restore into SessionStore");

            AudioEngine sourceEngine;
            sourceEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            SessionPersistence::rebuildEngineFromState(sourceEngine, sourceStore.state());
            const int sourceLatency = sourceEngine.getLatencyNumSamples();
            expect(sourceLatency > baselineLatency,
                "Enabled Overdrive preset should report latency above OutputChain baseline");

            const auto presetFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-p13a-pdc-latency-roundtrip.nova-preset");
            presetFile.deleteFile();

            expect(SessionPersistence::savePresetToFile(presetFile, sourceStore, sourceEngine),
                "Latency preset save should succeed");

            SessionStore restoredStore;
            AudioEngine restoredEngine;
            restoredEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            expect(SessionPersistence::loadPresetFromFile(presetFile, restoredStore, restoredEngine),
                "Latency preset load should succeed");
            expectEquals(restoredEngine.getLatencyNumSamples(), sourceLatency);

            expect(restoredStore.applyCommand(
                SessionStore::Command::makeSetPedalEnabled(Nova::ChainID::LineA, 0, false)).changed,
                "Disabling restored latency pedal should mutate state");
            SessionPersistence::rebuildEngineFromState(restoredEngine, restoredStore.state());
            expectEquals(restoredEngine.getLatencyNumSamples(), baselineLatency,
                "Bypassed restored Overdrive should return to OutputChain baseline latency");

            presetFile.deleteFile();
        }

        beginTest("P13A sample-rate reprepare refreshes limiter and graph latency");
        {
            OutputChainProcessor output48;
            output48.prepareToPlay(48000.0, kBlockSize);
            const int outputLatency48 = output48.getLatencySamples();

            OutputChainProcessor output96;
            output96.prepareToPlay(96000.0, kBlockSize);
            const int outputLatency96 = output96.getLatencySamples();

            expect(outputLatency48 > 0 && outputLatency96 > outputLatency48,
                "OutputChain lookahead latency should scale with sample rate");

            AudioEngine baseline96;
            baseline96.prepare(96000.0, kBlockSize, 2, 2);
            const int baselineLatency96 = baseline96.getLatencyNumSamples();
            expectEquals(baselineLatency96, outputLatency96);

            AudioEngine engine;
            engine.prepare(48000.0, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "p13a-sr-overdrive");
            engine.synchronizeProcessingState();
            const int activeLatency48 = engine.getLatencyNumSamples();
            expect(activeLatency48 > outputLatency48,
                "48 kHz latency chain should exceed OutputChain baseline");

            engine.prepare(96000.0, kBlockSize, 2, 2);
            engine.synchronizeProcessingState();
            const int activeLatency96 = engine.getLatencyNumSamples();
            expect(activeLatency96 > baselineLatency96,
                "96 kHz latency chain should exceed refreshed OutputChain baseline");
            expect(activeLatency96 != activeLatency48,
                "Sample-rate reprepare should refresh reported graph latency");

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            juce::MidiBuffer midi;
            for (int i = 0; i < kBlockSize; ++i)
            {
                const float t = (float)i / 96000.0f;
                buffer.setSample(0, i, 0.08f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t));
                buffer.setSample(1, i, 0.07f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t));
            }

            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 8);
            engine.process(buffer, midi);
            expect(bufferHasOnlyFiniteSamples(buffer),
                "Sample-rate-reprepared latency graph should process finite audio");
        }

        beginTest("DryWetMixer scratch preparation and fallback boundaries remain canonical");
        {
            Nova::Audio::DryWetMixer mixer;
            constexpr int kDryWetMinimumScratchBlocks = 8192;

            mixer.prepareScratch(64, 1, kDryWetMinimumScratchBlocks);
            expectEquals(mixer.getScratchBlockCapacity(), kDryWetMinimumScratchBlocks);
            expectEquals(mixer.getScratchChannelCapacity(), 2);
            mixer.prepareDryDelay(128);
            expectEquals(mixer.getDryDelayBufferSize(), 128 + kDryWetMinimumScratchBlocks + 8);
            mixer.setLatencySamples(512);
            expectEquals(mixer.getLatencySamples(), 128);
            expect(mixer.canUseScratch(kDryWetMinimumScratchBlocks));
            expect(!mixer.shouldUseOversizedFallback(kDryWetMinimumScratchBlocks));
            expect(mixer.shouldUseOversizedFallback(kDryWetMinimumScratchBlocks + 1));

            mixer.prepareScratch(4096, 4, kDryWetMinimumScratchBlocks);
            const int expectedCapacity = juce::jmax(kDryWetMinimumScratchBlocks, 4096 * 4);
            expectEquals(mixer.getScratchBlockCapacity(), expectedCapacity);
            expectEquals(mixer.getScratchChannelCapacity(), 4);
            expect(mixer.canUseScratch(expectedCapacity));
            expect(!mixer.shouldUseOversizedFallback(expectedCapacity));
            expect(mixer.shouldUseOversizedFallback(expectedCapacity + 1));
        }

        beginTest("DryWetMixer capture and mixed output stay deterministic and range-safe");
        {
            Nova::Audio::DryWetMixer mixer;
            constexpr int kDryWetMinimumScratchBlocks = 8192;
            mixer.prepareScratch(64, 2, kDryWetMinimumScratchBlocks);
            mixer.prepareDryDelay(64);
            mixer.resetMix(0.25f);

            constexpr int numSamples = 6;
            constexpr int totalSamples = 12;

            juce::AudioBuffer<float> input(2, totalSamples);
            juce::AudioBuffer<float> wetBuffer(2, totalSamples);
            input.clear();
            wetBuffer.clear();

            for (int i = 0; i < totalSamples; ++i)
            {
                input.setSample(0, i, 0.1f * (float)(i + 1));
                input.setSample(1, i, -0.05f * (float)(i + 1));
                wetBuffer.setSample(0, i, 0.02f * (float)(i + 1));
                wetBuffer.setSample(1, i, 0.03f * (float)(i + 1));
            }

            juce::AudioBuffer<float> wetBefore(wetBuffer.getNumChannels(), wetBuffer.getNumSamples());
            wetBefore.makeCopyOf(wetBuffer);

            const int dryDelayWriteIndex = mixer.getDryDelayWriteIndex();
            mixer.setLatencySamples(0);
            mixer.captureDry(input, 2, numSamples);
            mixer.mixCapturedDryWithWet(wetBuffer, 2, numSamples);

            expect(bufferHasOnlyFiniteSamples(wetBuffer), "DryWetMixer mixed output must remain finite");
            expectEquals(mixer.getDryDelayWriteIndex(), dryDelayWriteIndex);

            for (int i = 0; i < numSamples; ++i)
            {
                const float expectedL = input.getSample(0, i) * 0.75f + wetBefore.getSample(0, i) * 0.25f;
                const float expectedR = input.getSample(1, i) * 0.75f + wetBefore.getSample(1, i) * 0.25f;
                expect(approximatelyEqual(wetBuffer.getSample(0, i), expectedL, 1.0e-6f),
                    "DryWetMixer mixed left sample mismatch at index " + juce::String(i));
                expect(approximatelyEqual(wetBuffer.getSample(1, i), expectedR, 1.0e-6f),
                    "DryWetMixer mixed right sample mismatch at index " + juce::String(i));
            }

            for (int i = numSamples; i < totalSamples; ++i)
            {
                expect(approximatelyEqual(wetBuffer.getSample(0, i), wetBefore.getSample(0, i), 1.0e-7f),
                    "DryWetMixer should not write beyond numSamples on left channel");
                expect(approximatelyEqual(wetBuffer.getSample(1, i), wetBefore.getSample(1, i), 1.0e-7f),
                    "DryWetMixer should not write beyond numSamples on right channel");
            }
        }

        beginTest("DryWetMixer nonzero latency preserves read-before-write and write-index state");
        {
            Nova::Audio::DryWetMixer mixer;
            constexpr int kDryWetMinimumScratchBlocks = 16;
            constexpr int latency = 2;
            constexpr int numSamples = 6;
            mixer.prepareScratch(numSamples, 2, kDryWetMinimumScratchBlocks);
            mixer.prepareDryDelay(8);
            mixer.resetMix(0.0f);
            mixer.setLatencySamples(latency);

            juce::AudioBuffer<float> dryBlock(2, numSamples);
            juce::AudioBuffer<float> wetBlock(2, numSamples);
            dryBlock.clear();
            wetBlock.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                dryBlock.setSample(0, i, 1.0f + (float)i);
                dryBlock.setSample(1, i, -10.0f - (float)i);
            }

            mixer.captureDry(dryBlock, 2, numSamples);
            mixer.mixCapturedDryWithWet(wetBlock, 2, numSamples);

            expectEquals(mixer.getDryDelayWriteIndex(), numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                const float expectedL = i < latency ? 0.0f : dryBlock.getSample(0, i - latency);
                const float expectedR = i < latency ? 0.0f : dryBlock.getSample(1, i - latency);
                expect(approximatelyEqual(wetBlock.getSample(0, i), expectedL, 1.0e-7f),
                    "Nonzero-latency left output mismatch at index " + juce::String(i));
                expect(approximatelyEqual(wetBlock.getSample(1, i), expectedR, 1.0e-7f),
                    "Nonzero-latency right output mismatch at index " + juce::String(i));
            }

            juce::AudioBuffer<float> nextDry(2, numSamples);
            juce::AudioBuffer<float> nextWet(2, numSamples);
            nextDry.clear();
            nextWet.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                nextDry.setSample(0, i, 101.0f + (float)i);
                nextDry.setSample(1, i, -101.0f - (float)i);
            }

            mixer.captureDry(nextDry, 2, numSamples);
            mixer.mixCapturedDryWithWet(nextWet, 2, numSamples);

            expectEquals(mixer.getDryDelayWriteIndex(), numSamples * 2);
            for (int i = 0; i < numSamples; ++i)
            {
                const float expectedL = i < latency ? dryBlock.getSample(0, numSamples - latency + i)
                    : nextDry.getSample(0, i - latency);
                const float expectedR = i < latency ? dryBlock.getSample(1, numSamples - latency + i)
                    : nextDry.getSample(1, i - latency);
                expect(approximatelyEqual(nextWet.getSample(0, i), expectedL, 1.0e-7f),
                    "Second nonzero-latency left output mismatch at index " + juce::String(i));
                expect(approximatelyEqual(nextWet.getSample(1, i), expectedR, 1.0e-7f),
                    "Second nonzero-latency right output mismatch at index " + juce::String(i));
            }
        }

        beginTest("DryWetMixer delay reset and latency clamps remain stable");
        {
            Nova::Audio::DryWetMixer mixer;
            constexpr int kDryWetMinimumScratchBlocks = 16;
            constexpr int numSamples = 5;
            mixer.prepareScratch(numSamples, 2, kDryWetMinimumScratchBlocks);
            mixer.prepareDryDelay(4);
            mixer.resetMix(0.0f);

            expectEquals(mixer.getDryDelayBufferSize(), 4 + mixer.getScratchBlockCapacity() + 8);
            mixer.setLatencySamples(99);
            expectEquals(mixer.getLatencySamples(), 4);
            mixer.setLatencySamples(-5);
            expectEquals(mixer.getLatencySamples(), 0);

            const int blockClamp = Nova::Audio::DryWetMixer::clampLatencyForDelay(99,
                mixer.getDryDelayBufferSize(),
                numSamples);
            expectEquals(blockClamp, mixer.getDryDelayBufferSize() - numSamples - 1);

            juce::AudioBuffer<float> dryBlock(2, numSamples);
            juce::AudioBuffer<float> wetBlock(2, numSamples);
            dryBlock.clear();
            wetBlock.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                dryBlock.setSample(0, i, 0.25f * (float)(i + 1));
                dryBlock.setSample(1, i, -0.125f * (float)(i + 1));
            }

            const int writeIndexBeforeZeroLatency = mixer.getDryDelayWriteIndex();
            mixer.captureDry(dryBlock, 2, numSamples);
            mixer.mixCapturedDryWithWet(wetBlock, 2, numSamples);
            expectEquals(mixer.getDryDelayWriteIndex(), writeIndexBeforeZeroLatency);

            for (int i = 0; i < numSamples; ++i)
            {
                expect(approximatelyEqual(wetBlock.getSample(0, i), dryBlock.getSample(0, i), 1.0e-7f),
                    "Zero-latency reset test left output mismatch at index " + juce::String(i));
                expect(approximatelyEqual(wetBlock.getSample(1, i), dryBlock.getSample(1, i), 1.0e-7f),
                    "Zero-latency reset test right output mismatch at index " + juce::String(i));
            }

            mixer.setLatencySamples(3);
            wetBlock.clear();
            mixer.captureDry(dryBlock, 2, numSamples);
            mixer.mixCapturedDryWithWet(wetBlock, 2, numSamples);
            expectEquals(mixer.getDryDelayWriteIndex(), numSamples);

            mixer.resetDryDelayLine();
            expectEquals(mixer.getDryDelayWriteIndex(), 0);

            wetBlock.clear();
            mixer.captureDry(dryBlock, 2, numSamples);
            mixer.mixCapturedDryWithWet(wetBlock, 2, numSamples);
            expectEquals(mixer.getDryDelayWriteIndex(), numSamples);

            for (int i = 0; i < 3; ++i)
            {
                expect(approximatelyEqual(wetBlock.getSample(0, i), 0.0f, 1.0e-7f),
                    "Reset delay line should clear left delayed sample at index " + juce::String(i));
                expect(approximatelyEqual(wetBlock.getSample(1, i), 0.0f, 1.0e-7f),
                    "Reset delay line should clear right delayed sample at index " + juce::String(i));
            }
        }

        beginTest("DryWetMixer ramp state and endpoint classification remain stable");
        {
            Nova::Audio::DryWetMixer mixer;
            constexpr double kMixRampSeconds = 0.008;
            const int rampSamples = juce::jmax(1, juce::roundToInt(kSampleRate * kMixRampSeconds));
            mixer.prepareMix(kSampleRate, kMixRampSeconds);

            mixer.resetMix(0.0f);
            expect(approximatelyEqual(mixer.getCurrentMix(), 0.0f, 1.0e-7f));
            expectEquals((int)mixer.classifyEndpoint(), (int)Nova::Audio::DryWetMixer::EndpointPath::Dry);

            mixer.setTargetMix(2.0f);
            expectEquals((int)mixer.classifyEndpoint(), (int)Nova::Audio::DryWetMixer::EndpointPath::Mixed);
            mixer.consumeRamp(rampSamples - 1);
            expectEquals((int)mixer.classifyEndpoint(), (int)Nova::Audio::DryWetMixer::EndpointPath::Mixed);
            mixer.consumeRamp(1);
            expect(approximatelyEqual(mixer.getCurrentMix(), 1.0f, 1.0e-5f));
            expectEquals((int)mixer.classifyEndpoint(), (int)Nova::Audio::DryWetMixer::EndpointPath::Wet);

            mixer.resetMix(1.0f);
            expectEquals((int)mixer.classifyEndpoint(), (int)Nova::Audio::DryWetMixer::EndpointPath::Wet);
            mixer.setTargetMix(-1.0f);
            expectEquals((int)mixer.classifyEndpoint(), (int)Nova::Audio::DryWetMixer::EndpointPath::Mixed);
            mixer.processWetEndpoint(rampSamples - 1);
            expectEquals((int)mixer.classifyEndpoint(), (int)Nova::Audio::DryWetMixer::EndpointPath::Mixed);
            mixer.processWetEndpoint(1);
            expect(approximatelyEqual(mixer.getCurrentMix(), 0.0f, 1.0e-5f));
            expectEquals((int)mixer.classifyEndpoint(), (int)Nova::Audio::DryWetMixer::EndpointPath::Dry);

            mixer.resetMix(0.0f);
            mixer.processDryEndpoint(64);
            expect(approximatelyEqual(mixer.getCurrentMix(), 0.0f, 1.0e-7f));

            mixer.resetMix(1.0f);
            mixer.processWetEndpoint(64);
            expect(approximatelyEqual(mixer.getCurrentMix(), 1.0f, 1.0e-7f));
        }

        beginTest("RoutingMixer LineA_Only targets preserve current GraphBuilder policy");
        {
            using Mixer = Nova::Audio::RoutingMixer;

            const auto targets = Mixer::makeTargets(Nova::SwitcherMode::LineA_Only,
                { 0.0005f, -0.25f, 0.75f },
                { 0.0005f, 0.50f, 0.25f });

            expect(!targets.lineA.muted, "LineA_Only should keep line A active");
            expect(targets.lineB.muted, "LineA_Only should mute line B");
            expect(approximatelyEqual(targets.lineA.gain, 1.0f),
                "LineA_Only should apply low-gain fallback to active line A");
            expect(approximatelyEqual(targets.lineA.pan, -0.25f), "LineA_Only should pass line A pan");
            expect(approximatelyEqual(targets.lineA.width, 0.75f), "LineA_Only should pass line A width");
            expect(approximatelyEqual(targets.lineB.gain, 0.0f),
                "LineA_Only should force muted line B gain to zero");
            expect(approximatelyEqual(targets.lineB.pan, 0.50f), "LineA_Only should pass line B pan target");
            expect(approximatelyEqual(targets.lineB.width, 0.25f), "LineA_Only should pass line B width target");

            const auto mutedFallbackProbe = Mixer::makeTargets(Nova::SwitcherMode::LineA_Only,
                { 0.80f, 0.0f, 1.0f },
                { -0.50f, 0.0f, 1.0f });
            expect(approximatelyEqual(mutedFallbackProbe.lineB.gain, 0.0f),
                "LineA_Only should not apply low-gain fallback to muted line B");
        }

        beginTest("RoutingMixer LineB_Only targets preserve current GraphBuilder policy");
        {
            using Mixer = Nova::Audio::RoutingMixer;

            const auto targets = Mixer::makeTargets(Nova::SwitcherMode::LineB_Only,
                { 0.0005f, -0.50f, 0.25f },
                { 0.0f, 0.25f, 0.80f });

            expect(targets.lineA.muted, "LineB_Only should mute line A");
            expect(!targets.lineB.muted, "LineB_Only should keep line B active");
            expect(approximatelyEqual(targets.lineA.gain, 0.0f),
                "LineB_Only should force muted line A gain to zero");
            expect(approximatelyEqual(targets.lineA.pan, -0.50f), "LineB_Only should pass line A pan target");
            expect(approximatelyEqual(targets.lineA.width, 0.25f), "LineB_Only should pass line A width target");
            expect(approximatelyEqual(targets.lineB.gain, 1.0f),
                "LineB_Only should apply low-gain fallback to active line B");
            expect(approximatelyEqual(targets.lineB.pan, 0.25f), "LineB_Only should pass line B pan");
            expect(approximatelyEqual(targets.lineB.width, 0.80f), "LineB_Only should pass line B width");

            const auto mutedFallbackProbe = Mixer::makeTargets(Nova::SwitcherMode::LineB_Only,
                { -0.50f, 0.0f, 1.0f },
                { 0.80f, 0.0f, 1.0f });
            expect(approximatelyEqual(mutedFallbackProbe.lineA.gain, 0.0f),
                "LineB_Only should not apply low-gain fallback to muted line A");
        }

        beginTest("RoutingMixer Dual_Parallel targets preserve fallback then compensation policy");
        {
            using Mixer = Nova::Audio::RoutingMixer;
            const float comp = Mixer::dualParallelCompensation();

            const auto targets = Mixer::makeTargets(Nova::SwitcherMode::Dual_Parallel,
                { 0.0005f, -0.40f, 0.60f },
                { 2.0f, 0.35f, 0.90f });

            expect(approximatelyEqual(comp, 0.5f), "Dual compensation should remain 0.5");
            expect(!targets.lineA.muted, "Dual_Parallel should keep line A active");
            expect(!targets.lineB.muted, "Dual_Parallel should keep line B active");
            expect(approximatelyEqual(targets.lineA.gain, 1.0f * comp),
                "Dual_Parallel should apply line A fallback before compensation");
            expect(approximatelyEqual(targets.lineB.gain, 2.0f * comp),
                "Dual_Parallel should apply line B compensation after preserving gain");
            expect(approximatelyEqual(targets.lineA.pan, -0.40f), "Dual_Parallel should pass line A pan");
            expect(approximatelyEqual(targets.lineA.width, 0.60f), "Dual_Parallel should pass line A width");
            expect(approximatelyEqual(targets.lineB.pan, 0.35f), "Dual_Parallel should pass line B pan");
            expect(approximatelyEqual(targets.lineB.width, 0.90f), "Dual_Parallel should pass line B width");
        }

        beginTest("AudioEngine routing modes and strip controls remain finite and mode-correct");
        {
            constexpr int kRoutingRenderBlocks = 24;
            constexpr int kRoutingSettleBlocks = 16;
            constexpr int kRoutingMeasureStartBlocks = 8;
            constexpr int kRoutingMeasureBlocks = 12;

            AudioEngine::RuntimeGlobalParams params;
            params.outputMixRaw = 100.0f;
            params.outputLimiterDb = 0.0f;
            params.outputVolumeDb = 0.0f;
            params.gainA = 1.0f;
            params.gainB = 1.0f;
            params.panA = 0.0f;
            params.panB = 0.0f;
            params.widthA = 1.0f;
            params.widthB = 1.0f;

            auto makeConfiguredEngine = [&]()
            {
                auto engine = std::make_unique<AudioEngine>();
                engine->prepare(kSampleRate, kBlockSize, 2, 2);
                engine->addPedal("High Gain Amp", Nova::ChainID::LineA, -1, Nova::ZoneID::Amp, "route-a-highgain");
                engine->addPedal("Clean Amp", Nova::ChainID::LineB, -1, Nova::ZoneID::Amp, "route-b-clean");
                engine->synchronizeProcessingState();
                engine->updateGlobalParams(params);
                engine->setEngineEnabled(true);
                warmUpEngine(*engine, kBlockSize, 20);
                return engine;
            };

            juce::AudioBuffer<float> source(2, kBlockSize * kRoutingRenderBlocks);
            for (int i = 0; i < source.getNumSamples(); ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                source.setSample(0, i, 0.20f * std::sin(juce::MathConstants<float>::twoPi * 146.0f * t));
                source.setSample(1, i, 0.15f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t + 0.29f));
            }

            const int routingMeasureStartSample = kBlockSize * kRoutingMeasureStartBlocks;
            const int routingMeasureLengthSamples = kBlockSize * kRoutingMeasureBlocks;
            const int sourceBlockCount = source.getNumSamples() / kBlockSize;

            struct RoutingRender
            {
                juce::AudioBuffer<float> output;
                OutputChainProcessor::DebugSnapshot snapshot;
            };

            auto renderWithParams = [&](const AudioEngine::RuntimeGlobalParams& modeParams)
            {
                auto engine = makeConfiguredEngine();
                engine->updateGlobalParams(modeParams);
                AudioEngineTestAccess::dispatchPendingMessageThreadWork(*engine);
                warmUpEngine(*engine, kBlockSize, 6);

                juce::MidiBuffer settleMidi;
                for (int settleBlock = 0; settleBlock < kRoutingSettleBlocks; ++settleBlock)
                {
                    const int sourceBlock = settleBlock % juce::jmax(1, sourceBlockCount);
                    const int sourceOffset = sourceBlock * kBlockSize;
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom(ch, 0, source, ch, sourceOffset, kBlockSize);
                    engine->process(block, settleMidi);
                }

                juce::AudioBuffer<float> output(source.getNumChannels(), source.getNumSamples());
                output.clear();
                juce::MidiBuffer midi;

                for (int offset = 0; offset < source.getNumSamples(); offset += kBlockSize)
                {
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom(ch, 0, source, ch, offset, kBlockSize);

                    engine->process(block, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        output.copyFrom(ch, offset, block, ch, 0, kBlockSize);
                }

                RoutingRender result{ std::move(output), engine->getOutputChainDebugSnapshot() };
                return result;
            };

            auto modeA = params;
            modeA.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            const auto renderA = renderWithParams(modeA);
            const auto& outA = renderA.output;

            auto modeB = params;
            modeB.switchMode = (int)Nova::SwitcherMode::LineB_Only;
            const auto renderB = renderWithParams(modeB);
            const auto& outB = renderB.output;

            auto modeDual = params;
            modeDual.switchMode = (int)Nova::SwitcherMode::Dual_Parallel;
            const auto renderDual = renderWithParams(modeDual);
            const auto& outDual = renderDual.output;

            expect(bufferHasOnlyFiniteSamples(outA), "LineA-only output must remain finite");
            expect(bufferHasOnlyFiniteSamples(outB), "LineB-only output must remain finite");
            expect(bufferHasOnlyFiniteSamples(outDual), "Dual-parallel output must remain finite");

            const double rmsA = computeWindowRms(outA, routingMeasureStartSample, routingMeasureLengthSamples);
            const double rmsB = computeWindowRms(outB, routingMeasureStartSample, routingMeasureLengthSamples);
            const double rmsDual = computeWindowRms(outDual, routingMeasureStartSample, routingMeasureLengthSamples);
            const double maxSingleRms = juce::jmax(rmsA, rmsB);
            const double dualToSingleRatio = rmsDual / juce::jmax(1.0e-9, maxSingleRms);
            const double dualPeak = computeBufferPeak(outDual, routingMeasureStartSample, routingMeasureLengthSamples);
            const auto dualSnapshot = renderDual.snapshot;

            const juce::String routingMetrics = " metrics: rmsA="
                + juce::String(rmsA, 8)
                + ", rmsB="
                + juce::String(rmsB, 8)
                + ", rmsDual="
                + juce::String(rmsDual, 8)
                + ", dualToMax="
                + juce::String(dualToSingleRatio, 8)
                + ", outputMixRaw="
                + juce::String(modeDual.outputMixRaw, 3)
                + ", switchMode="
                + juce::String(modeDual.switchMode)
                + ", gainA="
                + juce::String(modeDual.gainA, 6)
                + ", gainB="
                + juce::String(modeDual.gainB, 6)
                + ", peakDual="
                + juce::String(dualPeak, 8)
                + ", limiterActiveBlocks="
                + juce::String(dualSnapshot.limiterActiveBlocks);

            expect(rmsA > 1.0e-4 && rmsB > 1.0e-4 && rmsDual > 1.0e-4,
                "Routing modes should remain audible in nominal conditions." + routingMetrics);
            expect(computeBufferNullRms(outA, outB) > 8.0e-4,
                "LineA-only and LineB-only should remain mode-distinct with different chains");
            expect(dualToSingleRatio > 0.40,
                "Dual-parallel should not collapse to near-silence." + routingMetrics);
            expect(dualToSingleRatio < 1.65,
                "Dual-parallel should stay gain-bounded." + routingMetrics);

            auto gainAHigh = modeDual;
            gainAHigh.gainA = 1.0f;
            gainAHigh.gainB = 1.0f;
            const auto outGainAHigh = renderWithParams(gainAHigh).output;

            auto gainALow = modeDual;
            gainALow.gainA = 0.2f;
            gainALow.gainB = 1.0f;
            const auto outGainALow = renderWithParams(gainALow).output;

            auto gainBLow = modeDual;
            gainBLow.gainA = 1.0f;
            gainBLow.gainB = 0.2f;
            const auto outGainBLow = renderWithParams(gainBLow).output;

            expect(computeBufferNullRms(outGainAHigh, outGainALow) > 1.0e-3,
                "GainA changes should alter the dual output when LineA is active");
            expect(computeBufferNullRms(outGainAHigh, outGainBLow) > 1.0e-3,
                "GainB changes should alter the dual output when LineB is active");

            auto panLeft = modeA;
            panLeft.panA = -1.0f;
            const auto outPanLeft = renderWithParams(panLeft).output;

            auto panRight = modeA;
            panRight.panA = 1.0f;
            const auto outPanRight = renderWithParams(panRight).output;

            const double panLeftL = computeChannelWindowRms(outPanLeft, 0, routingMeasureStartSample, routingMeasureLengthSamples);
            const double panLeftR = computeChannelWindowRms(outPanLeft, 1, routingMeasureStartSample, routingMeasureLengthSamples);
            const double panRightL = computeChannelWindowRms(outPanRight, 0, routingMeasureStartSample, routingMeasureLengthSamples);
            const double panRightR = computeChannelWindowRms(outPanRight, 1, routingMeasureStartSample, routingMeasureLengthSamples);
            expect(panLeftL > 1.0e-4 && panLeftR > 1.0e-4 && panRightL > 1.0e-4 && panRightR > 1.0e-4,
                "Pan variants should remain audible and finite");
            expect(computeBufferNullRms(outPanLeft, outPanRight) > 5.0e-4,
                "PanA=-1 and PanA=+1 should produce an observable output difference");

            auto widthMono = modeA;
            widthMono.widthA = 0.0f;
            const auto outWidthMono = renderWithParams(widthMono).output;

            auto widthWide = modeA;
            widthWide.widthA = 1.0f;
            const auto outWidthWide = renderWithParams(widthWide).output;

            expect(bufferHasOnlyFiniteSamples(outWidthMono), "WidthA=0 output must stay finite");
            expect(bufferHasOnlyFiniteSamples(outWidthWide), "WidthA=1 output must stay finite");

            expect(computeBufferNullRms(outWidthMono, outWidthWide) > 5.0e-4,
                "WidthA changes should produce an observable but finite output difference");

            AudioEngine cleanEngine;
            cleanEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            AudioEngine::RuntimeGlobalParams cleanParams;
            cleanParams.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            cleanParams.outputMixRaw = 100.0f;
            cleanParams.outputLimiterDb = 0.0f;
            cleanParams.outputVolumeDb = 0.0f;
            cleanParams.gainA = 1.0f;
            cleanParams.gainB = 1.0f;
            cleanParams.panA = 0.0f;
            cleanParams.panB = 0.0f;
            cleanParams.widthA = 1.0f;
            cleanParams.widthB = 1.0f;
            cleanEngine.updateGlobalParams(cleanParams);
            cleanEngine.setEngineEnabled(true);
            warmUpEngine(cleanEngine, kBlockSize, 16);

            juce::AudioBuffer<float> cleanSource(2, kBlockSize * 20);
            for (int i = 0; i < cleanSource.getNumSamples(); ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                cleanSource.setSample(0, i, 0.07f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t));
                cleanSource.setSample(1, i, 0.06f * std::sin(juce::MathConstants<float>::twoPi * 262.0f * t + 0.17f));
            }

            auto renderCleanMode = [&](int switchModeValue)
            {
                cleanParams.switchMode = switchModeValue;
                cleanEngine.updateGlobalParams(cleanParams);
                cleanEngine.synchronizeProcessingState();
                warmUpEngine(cleanEngine, kBlockSize, 8);

                juce::AudioBuffer<float> output(cleanSource.getNumChannels(), cleanSource.getNumSamples());
                output.clear();
                juce::MidiBuffer cleanMidi;
                for (int offset = 0; offset < cleanSource.getNumSamples(); offset += kBlockSize)
                {
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom(ch, 0, cleanSource, ch, offset, kBlockSize);
                    cleanEngine.process(block, cleanMidi);
                    for (int ch = 0; ch < 2; ++ch)
                        output.copyFrom(ch, offset, block, ch, 0, kBlockSize);
                }

                return output;
            };

            const auto cleanLineA = renderCleanMode((int)Nova::SwitcherMode::LineA_Only);
            const auto cleanDual = renderCleanMode((int)Nova::SwitcherMode::Dual_Parallel);
            const double cleanLineARms = computeWindowRms(cleanLineA, kBlockSize * 2, kBlockSize * 14);
            const double cleanDualRms = computeWindowRms(cleanDual, kBlockSize * 2, kBlockSize * 14);
            const double cleanDualRatio = cleanDualRms / juce::jmax(1.0e-9, cleanLineARms);

            const auto snapshot = cleanEngine.getOutputChainDebugSnapshot();
            expect(snapshot.limiterActiveBlocks == 0,
                "Dual-parallel nominal clean routing should not drive limiter activity");
            expect(cleanDualRatio > 0.90 && cleanDualRatio < 1.12,
                "Dual-parallel clean compensation should stay near unity, ratio=" + juce::String(cleanDualRatio, 6));
        }

        beginTest("AudioEngine dual-parallel nominal does not collapse after settled routing update");
        {
            constexpr int kRoutingRenderBlocks = 24;
            constexpr int kRoutingSettleBlocks = 16;
            constexpr int kRoutingMeasureStartBlocks = 8;
            constexpr int kRoutingMeasureBlocks = 12;

            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("High Gain Amp", Nova::ChainID::LineA, -1, Nova::ZoneID::Amp, "dual-stability-a");
            engine.addPedal("Clean Amp", Nova::ChainID::LineB, -1, Nova::ZoneID::Amp, "dual-stability-b");
            engine.synchronizeProcessingState();

            AudioEngine::RuntimeGlobalParams params;
            params.outputMixRaw = 100.0f;
            params.outputLimiterDb = 0.0f;
            params.outputVolumeDb = 0.0f;
            params.gainA = 1.0f;
            params.gainB = 1.0f;
            params.panA = 0.0f;
            params.panB = 0.0f;
            params.widthA = 1.0f;
            params.widthB = 1.0f;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;

            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 20);

            juce::AudioBuffer<float> source(2, kBlockSize * kRoutingRenderBlocks);
            for (int i = 0; i < source.getNumSamples(); ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                source.setSample(0, i, 0.20f * std::sin(juce::MathConstants<float>::twoPi * 146.0f * t));
                source.setSample(1, i, 0.15f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t + 0.29f));
            }

            const int routingMeasureStartSample = kBlockSize * kRoutingMeasureStartBlocks;
            const int routingMeasureLengthSamples = kBlockSize * kRoutingMeasureBlocks;
            const int sourceBlockCount = source.getNumSamples() / kBlockSize;

            auto renderMode = [&](int switchModeValue)
            {
                auto modeParams = params;
                modeParams.switchMode = switchModeValue;
                engine.updateGlobalParams(modeParams);
                engine.synchronizeProcessingState();
                warmUpEngine(engine, kBlockSize, 6);

                juce::MidiBuffer settleMidi;
                for (int settleBlock = 0; settleBlock < kRoutingSettleBlocks; ++settleBlock)
                {
                    const int sourceBlock = settleBlock % juce::jmax(1, sourceBlockCount);
                    const int sourceOffset = sourceBlock * kBlockSize;
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom(ch, 0, source, ch, sourceOffset, kBlockSize);
                    engine.process(block, settleMidi);
                }

                juce::AudioBuffer<float> output(2, source.getNumSamples());
                output.clear();
                juce::MidiBuffer midi;
                for (int offset = 0; offset < source.getNumSamples(); offset += kBlockSize)
                {
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom(ch, 0, source, ch, offset, kBlockSize);

                    engine.process(block, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        output.copyFrom(ch, offset, block, ch, 0, kBlockSize);
                }

                return output;
            };

            const auto outA = renderMode((int)Nova::SwitcherMode::LineA_Only);
            const auto outDual = renderMode((int)Nova::SwitcherMode::Dual_Parallel);

            const double rmsA = computeWindowRms(outA, routingMeasureStartSample, routingMeasureLengthSamples);
            const double rmsDual = computeWindowRms(outDual, routingMeasureStartSample, routingMeasureLengthSamples);
            const double dualRatio = rmsDual / juce::jmax(1.0e-9, rmsA);
            const double dualPeak = computeBufferPeak(outDual, routingMeasureStartSample, routingMeasureLengthSamples);
            const auto snapshot = engine.getOutputChainDebugSnapshot();

            const juce::String dualMetrics = " dual metrics: rmsA="
                + juce::String(rmsA, 8)
                + ", rmsDual="
                + juce::String(rmsDual, 8)
                + ", ratio="
                + juce::String(dualRatio, 8)
                + ", outputMixRaw="
                + juce::String(params.outputMixRaw, 3)
                + ", gainA="
                + juce::String(params.gainA, 6)
                + ", gainB="
                + juce::String(params.gainB, 6)
                + ", peakDual="
                + juce::String(dualPeak, 8)
                + ", limiterActiveBlocks="
                + juce::String(snapshot.limiterActiveBlocks);

            expect(bufferHasOnlyFiniteSamples(outA), "LineA nominal output should stay finite.");
            expect(bufferHasOnlyFiniteSamples(outDual), "Dual nominal output should stay finite.");
            expect(rmsA > 1.0e-4 && rmsDual > 1.0e-4,
                "Nominal routing should stay audible." + dualMetrics);
            expect(dualRatio > 0.40,
                "Dual-parallel nominal path should not collapse after settled routing update." + dualMetrics);
        }

        beginTest("AudioEngine routing policy low-gain fallback and inactive-line isolation remain stable");
        {
            constexpr int kRoutingRenderBlocks = 18;
            constexpr int kRoutingMeasureStartBlocks = 4;
            constexpr int kRoutingMeasureBlocks = 10;

            juce::AudioBuffer<float> source(2, kBlockSize * kRoutingRenderBlocks);
            for (int i = 0; i < source.getNumSamples(); ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                source.setSample(0, i, 0.09f * std::sin(juce::MathConstants<float>::twoPi * 173.0f * t));
                source.setSample(1, i, 0.07f * std::sin(juce::MathConstants<float>::twoPi * 251.0f * t + 0.31f));
            }

            auto renderPolicy = [&](const AudioEngine::RuntimeGlobalParams& routingParams)
            {
                AudioEngine engine;
                engine.prepare(kSampleRate, kBlockSize, 2, 2);
                engine.updateGlobalParams(routingParams);
                engine.setEngineEnabled(true);
                warmUpEngine(engine, kBlockSize, 16);

                juce::AudioBuffer<float> output(2, source.getNumSamples());
                output.clear();
                juce::MidiBuffer midi;
                for (int offset = 0; offset < source.getNumSamples(); offset += kBlockSize)
                {
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom(ch, 0, source, ch, offset, kBlockSize);

                    engine.process(block, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        output.copyFrom(ch, offset, block, ch, 0, kBlockSize);
                }

                return output;
            };

            AudioEngine::RuntimeGlobalParams base;
            base.outputMixRaw = 100.0f;
            base.outputLimiterDb = 0.0f;
            base.outputVolumeDb = 0.0f;
            base.gainA = 1.0f;
            base.gainB = 1.0f;
            base.panA = 0.0f;
            base.panB = 0.0f;
            base.widthA = 1.0f;
            base.widthB = 1.0f;

            auto lineABaseline = base;
            lineABaseline.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            const auto outLineABaseline = renderPolicy(lineABaseline);

            auto lineALowGain = lineABaseline;
            lineALowGain.gainA = 0.0f;
            const auto outLineALowGain = renderPolicy(lineALowGain);

            auto lineAInactiveBChanged = lineABaseline;
            lineAInactiveBChanged.gainB = 2.0f;
            lineAInactiveBChanged.panB = 1.0f;
            lineAInactiveBChanged.widthB = 0.0f;
            const auto outLineAInactiveBChanged = renderPolicy(lineAInactiveBChanged);

            auto lineBBaseline = base;
            lineBBaseline.switchMode = (int)Nova::SwitcherMode::LineB_Only;
            const auto outLineBBaseline = renderPolicy(lineBBaseline);

            auto lineBLowGain = lineBBaseline;
            lineBLowGain.gainB = 0.0005f;
            const auto outLineBLowGain = renderPolicy(lineBLowGain);

            auto lineBInactiveAChanged = lineBBaseline;
            lineBInactiveAChanged.gainA = 2.0f;
            lineBInactiveAChanged.panA = -1.0f;
            lineBInactiveAChanged.widthA = 0.0f;
            const auto outLineBInactiveAChanged = renderPolicy(lineBInactiveAChanged);

            auto dualLowGain = base;
            dualLowGain.switchMode = (int)Nova::SwitcherMode::Dual_Parallel;
            dualLowGain.gainA = 0.0f;
            dualLowGain.gainB = 0.0005f;
            const auto outDualLowGain = renderPolicy(dualLowGain);

            const int measureStart = kBlockSize * kRoutingMeasureStartBlocks;
            const int measureLength = kBlockSize * kRoutingMeasureBlocks;
            const double baselineRms = computeWindowRms(outLineABaseline, measureStart, measureLength);
            const double dualLowRms = computeWindowRms(outDualLowGain, measureStart, measureLength);
            const double dualLowRatio = dualLowRms / juce::jmax(1.0e-9, baselineRms);

            expect(bufferHasOnlyFiniteSamples(outLineALowGain), "LineA low-gain fallback output must stay finite");
            expect(bufferHasOnlyFiniteSamples(outLineBLowGain), "LineB low-gain fallback output must stay finite");
            expect(bufferHasOnlyFiniteSamples(outDualLowGain), "Dual low-gain fallback output must stay finite");
            expect(computeBufferNullRms(outLineABaseline, outLineALowGain) < 2.0e-5,
                "Active LineA gain <= 0.001 should normalize to unity");
            expect(computeBufferNullRms(outLineBBaseline, outLineBLowGain) < 2.0e-5,
                "Active LineB gain <= 0.001 should normalize to unity");
            expect(computeBufferNullRms(outLineABaseline, outLineAInactiveBChanged) < 2.0e-5,
                "LineA_Only must ignore inactive LineB gain/pan/width changes");
            expect(computeBufferNullRms(outLineBBaseline, outLineBInactiveAChanged) < 2.0e-5,
                "LineB_Only must ignore inactive LineA gain/pan/width changes");
            expect(dualLowRatio > 0.90 && dualLowRatio < 1.12,
                "Dual low-gain fallback should stay near unity, ratio=" + juce::String(dualLowRatio, 6));
        }

        beginTest("AudioEngine routing policy LineB pan and width targets remain isolated");
        {
            constexpr int kRoutingRenderBlocks = 18;
            constexpr int kRoutingMeasureStartBlocks = 4;
            constexpr int kRoutingMeasureBlocks = 10;

            juce::AudioBuffer<float> source(2, kBlockSize * kRoutingRenderBlocks);
            for (int i = 0; i < source.getNumSamples(); ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                source.setSample(0, i, 0.08f * std::sin(juce::MathConstants<float>::twoPi * 211.0f * t));
                source.setSample(1, i, 0.05f * std::sin(juce::MathConstants<float>::twoPi * 307.0f * t + 0.43f));
            }

            auto renderPolicy = [&](const AudioEngine::RuntimeGlobalParams& routingParams)
            {
                AudioEngine engine;
                engine.prepare(kSampleRate, kBlockSize, 2, 2);
                engine.updateGlobalParams(routingParams);
                engine.setEngineEnabled(true);
                warmUpEngine(engine, kBlockSize, 16);

                juce::AudioBuffer<float> output(2, source.getNumSamples());
                output.clear();
                juce::MidiBuffer midi;
                for (int offset = 0; offset < source.getNumSamples(); offset += kBlockSize)
                {
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom(ch, 0, source, ch, offset, kBlockSize);

                    engine.process(block, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        output.copyFrom(ch, offset, block, ch, 0, kBlockSize);
                }

                return output;
            };

            AudioEngine::RuntimeGlobalParams params;
            params.outputMixRaw = 100.0f;
            params.outputLimiterDb = 0.0f;
            params.outputVolumeDb = 0.0f;
            params.switchMode = (int)Nova::SwitcherMode::LineB_Only;
            params.gainA = 1.0f;
            params.gainB = 1.0f;
            params.panA = 0.0f;
            params.panB = 0.0f;
            params.widthA = 1.0f;
            params.widthB = 1.0f;

            auto panLeft = params;
            panLeft.panB = -1.0f;
            const auto outPanLeft = renderPolicy(panLeft);

            auto panRight = params;
            panRight.panB = 1.0f;
            const auto outPanRight = renderPolicy(panRight);

            auto inactivePanA = params;
            inactivePanA.panA = 1.0f;
            const auto outInactivePanA = renderPolicy(inactivePanA);

            auto widthMono = params;
            widthMono.widthB = 0.0f;
            const auto outWidthMono = renderPolicy(widthMono);

            auto widthNormal = params;
            widthNormal.widthB = 1.0f;
            const auto outWidthNormal = renderPolicy(widthNormal);

            const int measureStart = kBlockSize * kRoutingMeasureStartBlocks;
            const int measureLength = kBlockSize * kRoutingMeasureBlocks;
            const double panLeftL = computeChannelWindowRms(outPanLeft, 0, measureStart, measureLength);
            const double panLeftR = computeChannelWindowRms(outPanLeft, 1, measureStart, measureLength);
            const double panRightL = computeChannelWindowRms(outPanRight, 0, measureStart, measureLength);
            const double panRightR = computeChannelWindowRms(outPanRight, 1, measureStart, measureLength);

            expect(bufferHasOnlyFiniteSamples(outPanLeft), "LineB pan-left output must stay finite");
            expect(bufferHasOnlyFiniteSamples(outPanRight), "LineB pan-right output must stay finite");
            expect(bufferHasOnlyFiniteSamples(outWidthMono), "LineB width=0 output must stay finite");
            expect(bufferHasOnlyFiniteSamples(outWidthNormal), "LineB width=1 output must stay finite");
            expect(panLeftL > panLeftR && panRightR > panRightL,
                "LineB pan targets should affect the active LineB balance");
            expect(computeBufferNullRms(outPanLeft, outPanRight) > 5.0e-4,
                "LineB pan target changes should be observable");
            expect(computeBufferNullRms(outWidthMono, outWidthNormal) > 5.0e-4,
                "LineB width target changes should be observable");
            expect(computeBufferNullRms(outWidthNormal, outInactivePanA) < 2.0e-5,
                "LineB_Only must ignore inactive LineA pan target changes");
        }

        beginTest("AudioEngine oversized process blocks stay safe and finite");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "oversized-overdrive");
            engine.synchronizeProcessingState();

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 50.0f;
            params.outputLimiterDb = 0.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 12);

            // P12D: prime OutputChain lookahead with the same periodic signal so
            // the oversized buffer rendering produces non-zero output rather than
            // catching the delay line in its initial empty state.
            constexpr int kOversizedSamples = kBlockSize + 32;
            juce::AudioBuffer<float> oversized(2, kOversizedSamples);
            juce::MidiBuffer midi;
            for (int primingBlock = 0; primingBlock < 4; ++primingBlock)
            {
                juce::AudioBuffer<float> primer(2, kBlockSize);
                for (int i = 0; i < kBlockSize; ++i)
                {
                    const float t = (float)((primingBlock * kBlockSize) + i) / (float)kSampleRate;
                    primer.setSample(0, i, 0.14f * std::sin(juce::MathConstants<float>::twoPi * 183.0f * t));
                    primer.setSample(1, i, 0.10f * std::sin(juce::MathConstants<float>::twoPi * 271.0f * t + 0.22f));
                }
                engine.process(primer, midi);
            }

            for (int i = 0; i < kOversizedSamples; ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                oversized.setSample(0, i, 0.14f * std::sin(juce::MathConstants<float>::twoPi * 183.0f * t));
                oversized.setSample(1, i, 0.10f * std::sin(juce::MathConstants<float>::twoPi * 271.0f * t + 0.22f));
            }

            engine.process(oversized, midi);
            expect(bufferHasOnlyFiniteSamples(oversized),
                "Oversized process buffer should remain finite through fallback path");
            expect(computeWindowRms(oversized, 0, oversized.getNumSamples()) > 1.0e-4,
                "Oversized process buffer should remain audible");
        }

        beginTest("AudioEngine clean path remains stable after topology swaps");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 16);

            juce::AudioBuffer<float> source(2, kBlockSize);
            for (int i = 0; i < kBlockSize; ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                source.setSample(0, i, 0.18f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t));
                source.setSample(1, i, 0.14f * std::sin(juce::MathConstants<float>::twoPi * 510.0f * t + 0.25f));
            }

            auto renderOneBlock = [&](const juce::AudioBuffer<float>& input)
            {
                juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
                output.makeCopyOf(input);
                juce::MidiBuffer midi;
                engine.process(output, midi);
                return output;
            };

            const auto baseline = renderOneBlock(source);
            expect(bufferHasOnlyFiniteSamples(baseline), "Baseline clean path render must be finite");

            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "swap-overdrive");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 8);

            engine.removePedal(Nova::ChainID::LineA, 0);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 16);

            const auto afterSwap = renderOneBlock(source);
            expect(bufferHasOnlyFiniteSamples(afterSwap), "Clean path after graph swap must be finite");
            expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), 0);
            expect(computeBufferNullRms(baseline, afterSwap) < 3.5e-3,
                "Clean path should remain stable after add/remove graph swap");
        }

        beginTest("AudioEngine add and remove during deterministic processing stays finite");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 8);

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            juce::MidiBuffer midi;
            bool finite = true;
            float peak = 0.0f;
            int globalSample = 0;

            for (int block = 0; block < 48; ++block)
            {
                if (block == 6)
                {
                    engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "live-overdrive");
                    engine.synchronizeProcessingState();
                }
                else if (block == 18)
                {
                    engine.removePedal(Nova::ChainID::LineA, 0);
                    engine.synchronizeProcessingState();
                }
                else if (block == 30)
                {
                    engine.addPedal("Delay", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "live-delay");
                    engine.synchronizeProcessingState();
                }
                else if (block == 40)
                {
                    engine.clearAll();
                    engine.synchronizeProcessingState();
                }

                for (int i = 0; i < kBlockSize; ++i, ++globalSample)
                {
                    const float t = (float)((double)globalSample / kSampleRate);
                    buffer.setSample(0, i, 0.16f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t));
                    buffer.setSample(1, i, 0.12f * std::sin(juce::MathConstants<float>::twoPi * 147.0f * t + 0.3f));
                }

                engine.process(buffer, midi);
                finite = finite && bufferHasOnlyFiniteSamples(buffer);
                peak = juce::jmax(peak, buffer.getMagnitude(0, 0, buffer.getNumSamples()));
                peak = juce::jmax(peak, buffer.getMagnitude(1, 0, buffer.getNumSamples()));
            }

            expect(finite, "Processing across add/remove graph swaps must remain finite");
            expect(peak < 4.0f, "Processing across graph swaps should stay bounded");
            expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), 0);
        }

        beginTest("AudioEngine recovers cleanly across engine disable and re-enable within conditioning tolerance");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize);

            juce::MidiBuffer midi;
            const std::vector<float> left{ 0.22f, -0.11f, 0.33f, -0.44f };
            const std::vector<float> right{ -0.15f, 0.25f, -0.35f, 0.45f };

            expectStereoSamplesMatchAfterLatency(*this, engine, left, right, kBlockSize, 3.5e-3f);

            engine.setEngineEnabled(false);
            warmUpEngine(engine, kBlockSize, 4);
            // When the engine is disabled the dry signal bypasses OutputChain
            // entirely, so latency drops to zero and the 4-sample equality check
            // still applies directly.
            juce::AudioBuffer<float> buffer(2, 4);
            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());
            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 3.5e-3f);

            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 10);
            expectStereoSamplesMatchAfterLatency(*this, engine, left, right, kBlockSize, 3.5e-3f);
        }

        beginTest("AudioEngine re-enable refresh re-prepares released pedal processors");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "refresh-overdrive");
            engine.synchronizeProcessingState();

            auto* overdrive = dynamic_cast<OverdrivePedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0));
            expect(overdrive != nullptr, "Expected to retrieve the live Overdrive pedal from the graph");

            if (overdrive != nullptr)
            {
                overdrive->getDriveParam()->setValueNotifyingHost(overdrive->getDriveParam()->convertTo0to1(88.0f));
                overdrive->getToneParam()->setValueNotifyingHost(overdrive->getToneParam()->convertTo0to1(0.74f));
                overdrive->getTextureParam()->setValueNotifyingHost(overdrive->getTextureParam()->convertTo0to1(0.62f));
                overdrive->getMixParam()->setValueNotifyingHost(overdrive->getMixParam()->convertTo0to1(1.0f));
                overdrive->getLevelParam()->setValueNotifyingHost(overdrive->getLevelParam()->convertTo0to1(0.78f));
                overdrive->reset();
            }

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 12);

            juce::AudioBuffer<float> input(2, kBlockSize * 4);
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                input.setSample(0, i, 0.22f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t));
                input.setSample(1, i, 0.18f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t));
            }

            auto renderEngineOutput = [&](const juce::AudioBuffer<float>& source)
            {
                juce::AudioBuffer<float> output(source.getNumChannels(), source.getNumSamples());
                output.clear();
                juce::MidiBuffer midi;

                for (int offset = 0; offset < source.getNumSamples(); offset += kBlockSize)
                {
                    const int numSamples = juce::jmin(kBlockSize, source.getNumSamples() - offset);
                    juce::AudioBuffer<float> block(source.getNumChannels(), kBlockSize);
                    block.clear();

                    for (int ch = 0; ch < source.getNumChannels(); ++ch)
                        block.copyFrom(ch, 0, source, ch, offset, numSamples);

                    engine.process(block, midi);

                    for (int ch = 0; ch < output.getNumChannels(); ++ch)
                        output.copyFrom(ch, offset, block, ch, 0, numSamples);
                }

                return output;
            };

            const auto activeOutput = renderEngineOutput(input);
            const double activeNullRms = computeBufferNullRms(input, activeOutput);
            expect(bufferHasOnlyFiniteSamples(activeOutput), "Prepared overdrive render must stay finite");
            expect(activeNullRms > 1.0e-3, "Active overdrive should audibly alter the signal before refresh");

            if (overdrive != nullptr)
                overdrive->releaseResources();

            engine.setEngineEnabled(false);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 4);

            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 12);

            const auto refreshedOutput = renderEngineOutput(input);
            const double refreshedNullRms = computeBufferNullRms(input, refreshedOutput);
            expect(bufferHasOnlyFiniteSamples(refreshedOutput), "Refreshed overdrive render must stay finite");
            expect(refreshedNullRms > 1.0e-3,
                "Engine re-enable refresh should re-prepare released pedals and restore audible processing");
        }

        beginTest("AudioEngine publishes replacement graph when bypass changes node latency");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.synchronizeProcessingState();

            // P12D: OutputChain always reports its lookahead delay. The "no pedal"
            // graph latency is therefore the OutputChain baseline, not zero.
            const int outputChainBaselineLatency = engine.getLatencyNumSamples();

            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "latency-overdrive");
            engine.synchronizeProcessingState();

            const int activeLatency = engine.getLatencyNumSamples();
            expect(activeLatency > outputChainBaselineLatency,
                "Overdrive should contribute graph latency above the OutputChain baseline");
            auto* activeProcessor = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
            expect(activeProcessor != nullptr, "Expected an active Overdrive processor before bypass");
            auto* activeOverdrive = dynamic_cast<OverdrivePedal*>(activeProcessor);
            expect(activeOverdrive != nullptr, "Expected latency test processor to be Overdrive");
            if (activeOverdrive != nullptr)
                activeOverdrive->getDriveParam()->setValueNotifyingHost(
                    activeOverdrive->getDriveParam()->convertTo0to1(73.0f));

            const auto activeGeneration = AudioEngineTestAccess::getActiveGraphGeneration(engine);

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, true);
            engine.synchronizeProcessingState();
            expectEquals(engine.getLatencyNumSamples(), outputChainBaselineLatency);
            auto* bypassedProcessor = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
            expect(bypassedProcessor != activeProcessor,
                "Latency-affecting bypass must publish a replacement graph instead of rebuilding the active graph in place");
            auto* bypassedOverdrive = dynamic_cast<OverdrivePedal*>(bypassedProcessor);
            expect(bypassedOverdrive != nullptr
                    && approximatelyEqual(bypassedOverdrive->getDriveParam()->get(), 73.0f, 1.0e-3f),
                "Bypass replacement graph must preserve live pedal parameter state");
            expect(AudioEngineTestAccess::getActiveGraphGeneration(engine) > activeGeneration,
                "Latency-affecting bypass must advance the published graph generation");

            const auto bypassedGeneration = AudioEngineTestAccess::getActiveGraphGeneration(engine);
            engine.setPedalBypassed(Nova::ChainID::LineA, 0, false);
            engine.synchronizeProcessingState();
            expectEquals(engine.getLatencyNumSamples(), activeLatency);
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) != bypassedProcessor,
                "Latency-affecting unbypass must publish another replacement graph");
            auto* unbypassedOverdrive = dynamic_cast<OverdrivePedal*>(
                engine.getProcessorForPedal(Nova::ChainID::LineA, 0));
            expect(unbypassedOverdrive != nullptr
                    && approximatelyEqual(unbypassedOverdrive->getDriveParam()->get(), 73.0f, 1.0e-3f),
                "Unbypass replacement graph must preserve live pedal parameter state");
            expect(AudioEngineTestAccess::getActiveGraphGeneration(engine) > bypassedGeneration,
                "Latency-affecting unbypass must advance the published graph generation");
        }

        beginTest("P13E-P3 pedal editor handle invalidates on graph generation change");
        {
            struct OverlayInvalidationProbe final : AudioEngine::TopologyListener
            {
                void audioEngineTopologyChanged(uint64_t graphGeneration) override
                {
                    ++callbackCount;
                    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
                    callbackWasOnMessageThread = messageManager != nullptr
                        && messageManager->isThisTheMessageThread();

                    if (visibleProcessor
                        && visibleProcessor.getGraphGeneration() != graphGeneration)
                    {
                        visibleProcessor = {};
                        invalidated = true;
                    }
                }

                AudioEngine::PedalProcessorHandle visibleProcessor;
                int callbackCount = 0;
                bool callbackWasOnMessageThread = false;
                bool invalidated = false;
            };

            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre,
                "p13e-p3-overlay-overdrive");
            engine.synchronizeProcessingState();

            // Drain initial graph notifications before simulating an open overlay.
            AudioEngineTestAccess::dispatchPendingMessageThreadWork(engine);

            auto processorHandle = engine.getPedalProcessorHandle(Nova::ChainID::LineA, 0);
            expect((bool)processorHandle, "Expected a retained processor handle for the open overlay");
            expectEquals(processorHandle.getGraphGeneration(), engine.getActiveGraphGeneration());

            auto retainedRuntime = AudioEngineTestAccess::getRetainedRuntimeWeak(processorHandle);
            OverlayInvalidationProbe overlay;
            overlay.visibleProcessor = std::move(processorHandle);
            engine.setTopologyListener(&overlay);

            AudioEngine::RuntimeGlobalParams stableParams;
            stableParams.outputMixRaw = 73.0f;
            engine.updateGlobalParams(stableParams);
            engine.synchronizeProcessingState();
            AudioEngineTestAccess::dispatchPendingMessageThreadWork(engine);

            expect((bool)overlay.visibleProcessor,
                "A parameter-only update must preserve an overlay on the current graph");
            expect(!overlay.invalidated,
                "A stable graph generation must not invalidate the pedal editor overlay");

            const auto openedGeneration = overlay.visibleProcessor.getGraphGeneration();
            engine.removePedal(Nova::ChainID::LineA, 0);
            engine.synchronizeProcessingState();

            expect(engine.getActiveGraphGeneration() > openedGeneration,
                "Removing the edited pedal must publish a newer graph generation");
            expect(!retainedRuntime.expired(),
                "The overlay lease must keep the retired graph alive until message-thread invalidation");

            AudioEngineTestAccess::dispatchPendingMessageThreadWork(engine);
            expect(overlay.invalidated,
                "A graph generation change must invalidate the simulated pedal editor overlay");
            expect(!(bool)overlay.visibleProcessor,
                "Invalidation must release the overlay's processor handle before stale access");
            expect(overlay.callbackCount > 0,
                "The graph replacement must emit a topology notification");
            expect(overlay.callbackWasOnMessageThread,
                "Topology invalidation must be delivered on the JUCE message thread");

            AudioEngineTestAccess::forceRetiredGraphCleanup(engine);
            expect(retainedRuntime.expired(),
                "The retired graph must be reclaimable after the overlay releases its lease");

            engine.setTopologyListener(nullptr);
        }

        beginTest("AudioEngine ClearAll followed by add rebuilds to the new topology");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "clear-overdrive");
            engine.addPedal("Delay", Nova::ChainID::LineA, 1, Nova::ZoneID::FX, "clear-delay");
            engine.synchronizeProcessingState();
            expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), 2);

            engine.clearAll();
            engine.synchronizeProcessingState();
            expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), 0);

            engine.addPedal("Reverb", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "clear-reverb");
            engine.synchronizeProcessingState();

            const auto nodes = engine.getNodes(Nova::ChainID::LineA);
            expectEquals((int)nodes.size(), 1);
            if (!nodes.empty())
                expectEquals(nodes.front().pedalID, juce::String("clear-reverb"));

            expect(dynamic_cast<ReverbPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0)) != nullptr,
                "ClearAll followed by add should publish a Reverb processor");

            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 8);

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            buffer.clear();
            buffer.setSample(0, 0, 0.2f);
            buffer.setSample(1, 0, 0.2f);
            juce::MidiBuffer midi;
            engine.process(buffer, midi);
            expect(bufferHasOnlyFiniteSamples(buffer), "Graph rebuilt after ClearAll/add should process finite output");
        }

        beginTest("AudioEngine preserves command order across batched topology edits");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "order-overdrive");
            engine.addPedal("Delay", Nova::ChainID::LineA, -1, Nova::ZoneID::FX, "order-delay");
            engine.addPedal("Reverb", Nova::ChainID::LineA, 1, Nova::ZoneID::FX, "order-reverb");
            engine.movePedal(Nova::ChainID::LineA, 0, 2);
            engine.synchronizeProcessingState();

            const auto nodes = engine.getNodes(Nova::ChainID::LineA);
            expectEquals((int)nodes.size(), 3);
            if (nodes.size() == 3)
            {
                expectEquals(nodes[0].pedalID, juce::String("order-reverb"));
                expectEquals(nodes[1].pedalID, juce::String("order-overdrive"));
                expectEquals(nodes[2].pedalID, juce::String("order-delay"));
            }
        }

        beginTest("AudioEngine diagnostic report reflects queued topology");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Delay", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "qa-delay");
            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 12);

            const auto report = engine.buildDiagnosticReport();
            expect(report.contains("processor=Delay"), "Diagnostic report should list the Delay pedal");
            expect(report.contains("pedalID=qa-delay"), "Diagnostic report should list the queued pedal ID");
            expect(report.contains("chainA:"), "Diagnostic report should include Line A topology");
        }

        beginTest("AudioEngine mixed-zone insertion keeps zone-canonical effective routing");
        {
            const auto unknownZone = static_cast<Nova::ZoneID>(999);

            auto configureEngine = [&](AudioEngine& engine, bool mixedOrder)
            {
                engine.prepare(kSampleRate, kBlockSize, 2, 2);

                AudioEngine::RuntimeGlobalParams params;
                params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
                params.outputMixRaw = 100.0f;
                params.outputLimiterDb = 0.0f;
                engine.updateGlobalParams(params);
                engine.setEngineEnabled(true);

                if (mixedOrder)
                {
                    engine.addPedal("Reverb", Nova::ChainID::LineA, -1, Nova::ZoneID::FX, "mixed-fx");
                    engine.addPedal("Cabinet", Nova::ChainID::LineA, -1, Nova::ZoneID::Cabinet, "mixed-cab");
                    engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, unknownZone, "mixed-unknown");
                    engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "mixed-pre");
                    engine.addPedal("Clean Amp", Nova::ChainID::LineA, -1, Nova::ZoneID::Amp, "mixed-amp");
                }
                else
                {
                    engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "sorted-pre");
                    engine.addPedal("Clean Amp", Nova::ChainID::LineA, -1, Nova::ZoneID::Amp, "sorted-amp");
                    engine.addPedal("Reverb", Nova::ChainID::LineA, -1, Nova::ZoneID::FX, "sorted-fx");
                    engine.addPedal("Cabinet", Nova::ChainID::LineA, -1, Nova::ZoneID::Cabinet, "sorted-cab");
                    engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, unknownZone, "sorted-unknown");
                }

                engine.synchronizeProcessingState();

                // juce::dsp::Convolution loads the cabinet IR on a background thread.
                // Without this wait each engine could render with or without the IR
                // depending on timing, and the null test fails intermittently.
                juce::Thread::sleep(500);
                warmUpEngine(engine, kBlockSize, 24);
            };

            auto renderEngineOutput = [&](AudioEngine& engine, const juce::AudioBuffer<float>& source)
            {
                juce::AudioBuffer<float> output(source.getNumChannels(), source.getNumSamples());
                output.clear();
                juce::MidiBuffer midi;

                for (int offset = 0; offset < source.getNumSamples(); offset += kBlockSize)
                {
                    const int numSamples = juce::jmin(kBlockSize, source.getNumSamples() - offset);
                    juce::AudioBuffer<float> block(source.getNumChannels(), kBlockSize);
                    block.clear();

                    for (int ch = 0; ch < source.getNumChannels(); ++ch)
                        block.copyFrom(ch, 0, source, ch, offset, numSamples);

                    engine.process(block, midi);

                    for (int ch = 0; ch < output.getNumChannels(); ++ch)
                        output.copyFrom(ch, offset, block, ch, 0, numSamples);
                }

                return output;
            };

            AudioEngine mixedEngine;
            AudioEngine sortedEngine;
            configureEngine(mixedEngine, true);
            configureEngine(sortedEngine, false);

            const auto mixedNodes = mixedEngine.getNodes(Nova::ChainID::LineA);
            expectEquals((int)mixedNodes.size(), 5);
            if (mixedNodes.size() == 5)
            {
                expectEquals(mixedNodes[0].pedalID, juce::String("mixed-fx"));
                expectEquals(mixedNodes[1].pedalID, juce::String("mixed-cab"));
                expectEquals(mixedNodes[2].pedalID, juce::String("mixed-unknown"));
                expectEquals(mixedNodes[3].pedalID, juce::String("mixed-pre"));
                expectEquals(mixedNodes[4].pedalID, juce::String("mixed-amp"));
            }

            juce::AudioBuffer<float> source(2, kBlockSize * 96);
            for (int i = 0; i < source.getNumSamples(); ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                source.setSample(0, i, 0.12f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t));
                source.setSample(1, i, 0.10f * std::sin(juce::MathConstants<float>::twoPi * 293.0f * t + 0.27f));
            }

            const auto mixedOutput = renderEngineOutput(mixedEngine, source);
            const auto sortedOutput = renderEngineOutput(sortedEngine, source);

            expect(bufferHasOnlyFiniteSamples(mixedOutput), "Mixed-zone render must stay finite");
            expect(bufferHasOnlyFiniteSamples(sortedOutput), "Sorted-zone render must stay finite");
            expectEquals(mixedEngine.getLatencyNumSamples(), sortedEngine.getLatencyNumSamples());

            const double nullRms = computeBufferNullRms(mixedOutput, sortedOutput);
            expect(nullRms < 4.5e-3,
                "Mixed insertion should keep zone-canonical effective routing, nullRms=" + juce::String(nullRms, 8));
        }

        beginTest("AudioEngine mixed-chain latency stays stable across equivalent rebuilds");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            auto addMixedChain = [&](const juce::String& tag)
            {
                engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, tag + "-pre");
                engine.addPedal("Clean Amp", Nova::ChainID::LineA, -1, Nova::ZoneID::Amp, tag + "-amp");
                engine.addPedal("Delay", Nova::ChainID::LineA, -1, Nova::ZoneID::FX, tag + "-fx");
                engine.addPedal("Cabinet", Nova::ChainID::LineA, -1, Nova::ZoneID::Cabinet, tag + "-cab");
            };

            auto renderOneBlock = [&](float phaseOffset)
            {
                juce::AudioBuffer<float> buffer(2, kBlockSize);
                juce::MidiBuffer midi;
                for (int i = 0; i < kBlockSize; ++i)
                {
                    const float t = ((float)i + phaseOffset) / (float)kSampleRate;
                    buffer.setSample(0, i, 0.14f * std::sin(juce::MathConstants<float>::twoPi * 247.0f * t));
                    buffer.setSample(1, i, 0.11f * std::sin(juce::MathConstants<float>::twoPi * 329.0f * t + 0.19f));
                }
                engine.process(buffer, midi);
                return buffer;
            };

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            params.outputLimiterDb = 0.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);

            addMixedChain("latmix-a");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 24);

            const int baselineLatency = engine.getLatencyNumSamples();
            expect(baselineLatency > 0, "Mixed chain should expose positive graph latency");
            expect(bufferHasOnlyFiniteSamples(renderOneBlock(0.0f)), "Baseline mixed-chain block must stay finite");

            engine.setEngineEnabled(false);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 6);

            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 16);
            expectEquals(engine.getLatencyNumSamples(), baselineLatency);

            engine.clearAll();
            engine.synchronizeProcessingState();
            addMixedChain("latmix-b");
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 24);

            const int rebuiltLatency = engine.getLatencyNumSamples();
            expectEquals(rebuiltLatency, baselineLatency);
            expect(bufferHasOnlyFiniteSamples(renderOneBlock(31.0f)),
                "Rebuilt mixed-chain block must stay finite");

            auto* activeProcessor = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
            expect(activeProcessor != nullptr, "Expected an active pre-zone processor for bypass latency test");

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, true);
            engine.synchronizeProcessingState();
            for (int attempt = 0;
                 attempt < 500 && engine.getProcessorForPedal(Nova::ChainID::LineA, 0) == activeProcessor;
                 ++attempt)
            {
                juce::Thread::sleep(1);
            }

            const int bypassLatency = engine.getLatencyNumSamples();
            expect(bypassLatency <= baselineLatency,
                "Bypass should not increase latency unexpectedly, baseline="
                    + juce::String(baselineLatency)
                    + ", bypass="
                    + juce::String(bypassLatency));
            auto* bypassedProcessor = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
            expect(bypassedProcessor != activeProcessor,
                "Latency-affecting bypass should publish a replacement mixed-chain graph");

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, false);
            engine.synchronizeProcessingState();
            for (int attempt = 0;
                 attempt < 500 && engine.getProcessorForPedal(Nova::ChainID::LineA, 0) == bypassedProcessor;
                 ++attempt)
            {
                juce::Thread::sleep(1);
            }

            expectEquals(engine.getLatencyNumSamples(), baselineLatency);
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) != bypassedProcessor,
                "Latency-affecting unbypass should publish a replacement mixed-chain graph");
        }

        beginTest("AudioEngine global processor path responds after prepare with no pedals");
        {
            struct NoPedalRenderMetrics
            {
                double rms = 0.0;
                bool finite = false;
                int lineANodeCount = -1;
                int lineBNodeCount = -1;
                OutputChainProcessor::DebugSnapshot outputSnapshot;
                juce::String report;
            };

            auto runNoPedalScenario = [&](float inputGainDb, float limiterDb, float amplitude)
            {
                NoPedalRenderMetrics metrics;

                AudioEngine engine;
                engine.prepare(kSampleRate, kBlockSize, 2, 2);

                AudioEngine::RuntimeGlobalParams params;
                params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
                params.outputMixRaw = 100.0f;
                params.inputGainDb = inputGainDb;
                params.outputLimiterDb = limiterDb;
                params.outputVolumeDb = 0.0f;
                engine.updateGlobalParams(params);
                engine.setEngineEnabled(true);
                engine.synchronizeProcessingState();
                warmUpEngine(engine, kBlockSize, 18);

                juce::AudioBuffer<float> buffer(2, kBlockSize * 12);
                juce::MidiBuffer midi;
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float t = (float)i / (float)kSampleRate;
                    buffer.setSample(0, i, amplitude * std::sin(juce::MathConstants<float>::twoPi * 440.0f * t));
                    buffer.setSample(1, i, 0.86f * amplitude * std::sin(juce::MathConstants<float>::twoPi * 660.0f * t));
                }

                for (int offset = 0; offset < buffer.getNumSamples(); offset += kBlockSize)
                {
                    juce::AudioBuffer<float> block(2, kBlockSize);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom(ch, 0, buffer, ch, offset, kBlockSize);
                    engine.process(block, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        buffer.copyFrom(ch, offset, block, ch, 0, kBlockSize);
                }

                metrics.finite = bufferHasOnlyFiniteSamples(buffer);
                metrics.rms = computeWindowRms(buffer, kBlockSize * 2, kBlockSize * 8);
                metrics.lineANodeCount = (int)engine.getNodes(Nova::ChainID::LineA).size();
                metrics.lineBNodeCount = (int)engine.getNodes(Nova::ChainID::LineB).size();
                metrics.outputSnapshot = engine.getOutputChainDebugSnapshot();
                metrics.report = engine.buildDiagnosticReport();
                return metrics;
            };

            const auto baseline = runNoPedalScenario(0.0f, 0.0f, 0.028f);
            const auto boosted = runNoPedalScenario(12.0f, 0.0f, 0.028f);
            // Drive the limiter above the -6 dB threshold (~0.501 linear) so the
            // assertion below exercises actual gain reduction, not just an armed flag.
            const auto limited = runNoPedalScenario(0.0f, -6.0f, 0.70f);

            expect(baseline.finite, "Baseline global processor render must stay finite");
            expect(boosted.finite, "Boosted global processor render must stay finite");
            expect(limited.finite, "Limited global processor render must stay finite");

            expectEquals(baseline.lineANodeCount, 0);
            expectEquals(baseline.lineBNodeCount, 0);

            expect(boosted.rms > baseline.rms * 2.2,
                "Input-chain gain path should be active on empty graph, baseline="
                    + juce::String(baseline.rms, 8)
                    + ", boosted="
                    + juce::String(boosted.rms, 8));

            expect(limited.rms > 0.0, "Output-chain limited render should stay audible");
            expect(limited.outputSnapshot.limiterActiveBlocks > 0,
                "Output-chain limiter telemetry should show active limiting blocks");

            expect(baseline.report.contains("activeGraph=true"),
                "Prepared no-pedal graph should stay active: " + baseline.report);
        }

        beginTest("AudioEngine known ProcessorBase and TempoSyncable pedals remain stable");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "cache-overdrive");
            engine.addPedal("Delay", Nova::ChainID::LineA, -1, Nova::ZoneID::FX, "cache-delay");
            engine.synchronizeProcessingState();

            auto* activeProcessor = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
            engine.setPedalBypassed(Nova::ChainID::LineA, 0, true);
            engine.synchronizeProcessingState();
            auto* bypassedProcessor = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
            expect(bypassedProcessor != activeProcessor,
                "Latency-affecting ProcessorBase bypass should publish a replacement graph");

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, false);
            engine.synchronizeProcessingState();
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) != bypassedProcessor,
                "Latency-affecting ProcessorBase unbypass should publish a replacement graph");

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            params.hostTempoBpm = 173.0f;
            params.hostTempoValid = true;
            params.hostTransportPlaying = true;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, kBlockSize, 16);

            auto* overdrive = dynamic_cast<OverdrivePedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0));
            auto* delay = dynamic_cast<DelayPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 1));
            expect(overdrive != nullptr, "Expected Overdrive pedal to resolve from active graph");
            expect(delay != nullptr, "Expected Delay pedal to resolve from active graph");

            if (delay != nullptr)
            {
                expect(std::abs(delay->getDisplayTempoBpm() - 173.0f) < 0.5f,
                    "Tempo-sync context should reach Delay pedal");
                expect(delay->isHostTempoValid(), "Delay pedal should reflect host tempo valid=true");
                expect(delay->isHostTransportPlaying(), "Delay pedal should reflect host transport playing=true");
            }

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            juce::MidiBuffer midi;
            for (int i = 0; i < kBlockSize; ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                buffer.setSample(0, i, 0.16f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t));
                buffer.setSample(1, i, 0.11f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t));
            }
            engine.process(buffer, midi);
            expect(bufferHasOnlyFiniteSamples(buffer), "Known ProcessorBase/TempoSyncable chain must process finite output");
        }

        beginTest("AudioEngine rapid latency-affecting bypass toggles stay finite and converge");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.synchronizeProcessingState();
            const int outputChainBaselineLatency = engine.getLatencyNumSamples();

            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "rapid-bypass-overdrive");

            AudioEngine::RuntimeGlobalParams params;
            params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            params.outputMixRaw = 100.0f;
            engine.updateGlobalParams(params);
            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();

            const int activeLatency = engine.getLatencyNumSamples();
            const auto initialGeneration = AudioEngineTestAccess::getActiveGraphGeneration(engine);
            expect(activeLatency > outputChainBaselineLatency,
                "Rapid bypass stress requires a pedal that contributes graph latency");

            std::atomic<bool> startProcessing{ false };
            std::atomic<bool> controlDone{ false };
            std::atomic<bool> outputFinite{ true };
            std::atomic<int> processedBlocks{ 0 };

            std::thread audioThread([&]
            {
                juce::AudioBuffer<float> buffer(2, kBlockSize);
                juce::MidiBuffer midi;
                double phase = 0.0;
                const double phaseDelta = juce::MathConstants<double>::twoPi * 220.0 / kSampleRate;

                while (!startProcessing.load(std::memory_order_acquire))
                    std::this_thread::yield();

                for (int block = 0; block < 4096; ++block)
                {
                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        const float sample = 0.16f * (float)std::sin(phase);
                        phase += phaseDelta;
                        if (phase >= juce::MathConstants<double>::twoPi)
                            phase -= juce::MathConstants<double>::twoPi;

                        buffer.setSample(0, i, sample);
                        buffer.setSample(1, i, sample * 0.83f);
                    }

                    engine.process(buffer, midi);
                    if (!bufferHasOnlyFiniteSamples(buffer))
                        outputFinite.store(false, std::memory_order_relaxed);

                    processedBlocks.fetch_add(1, std::memory_order_relaxed);

                    if (block >= 511 && controlDone.load(std::memory_order_acquire))
                        break;

                    if ((block & 15) == 0)
                        std::this_thread::yield();
                }
            });

            startProcessing.store(true, std::memory_order_release);

            for (int toggle = 0; toggle < 48; ++toggle)
            {
                engine.setPedalBypassed(Nova::ChainID::LineA, 0, (toggle & 1) == 0);
                if ((toggle & 3) == 3)
                    juce::Thread::sleep(1);
            }

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, true);
            controlDone.store(true, std::memory_order_release);
            audioThread.join();

            ProcessorBase* finalProcessor = nullptr;
            for (int attempt = 0; attempt < 500; ++attempt)
            {
                engine.synchronizeProcessingState();
                finalProcessor = dynamic_cast<ProcessorBase*>(
                    engine.getProcessorForPedal(Nova::ChainID::LineA, 0));

                if (finalProcessor != nullptr
                    && finalProcessor->getBypassed()
                    && engine.getLatencyNumSamples() == outputChainBaselineLatency)
                {
                    break;
                }

                juce::Thread::sleep(1);
            }

            expect(processedBlocks.load(std::memory_order_relaxed) >= 512,
                "Rapid bypass stress should process many audio blocks");
            expect(outputFinite.load(std::memory_order_relaxed),
                "Rapid bypass toggles during processing must keep output finite");
            expect(finalProcessor != nullptr && finalProcessor->getBypassed(),
                "Final published processor must converge to the requested bypass state");
            expectEquals(engine.getLatencyNumSamples(), outputChainBaselineLatency,
                "Graph latency/PDC must converge after the final bypass request");
            expect(AudioEngineTestAccess::getActiveGraphGeneration(engine) > initialGeneration,
                "Latency-affecting bypass stress must converge through replacement graph publication");
        }

        beginTest("AudioEngine tempo-sync context variants remain stable on Delay");
        {
            auto verifyTempoContext = [&](float bpm, bool valid, bool transportPlaying, const juce::String& tag)
            {
                AudioEngine engine;
                engine.prepare(kSampleRate, kBlockSize, 2, 2);
                engine.addPedal("Delay", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, "tempo-delay-" + tag);
                engine.synchronizeProcessingState();

                AudioEngine::RuntimeGlobalParams params;
                params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
                params.outputMixRaw = 100.0f;
                params.hostTempoBpm = bpm;
                params.hostTempoValid = valid;
                params.hostTransportPlaying = transportPlaying;
                engine.updateGlobalParams(params);
                engine.setEngineEnabled(true);
                engine.synchronizeProcessingState();
                warmUpEngine(engine, kBlockSize, 12);

                auto* delay = dynamic_cast<DelayPedal*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 0));
                expect(delay != nullptr, "Expected Delay pedal to resolve for tempo-sync check (" + tag + ")");

                if (delay != nullptr)
                {
                    expect(std::abs(delay->getDisplayTempoBpm() - bpm) < 0.5f,
                        "Delay tempo should match host context (" + tag + ")");
                    expect(delay->isHostTempoValid() == valid,
                        "Delay host tempo validity should match context (" + tag + ")");
                    expect(delay->isHostTransportPlaying() == transportPlaying,
                        "Delay host transport state should match context (" + tag + ")");
                }
            };

            verifyTempoContext(173.0f, true, true, "valid");
            verifyTempoContext(91.0f, false, false, "invalid");
        }

        beginTest("AudioEngine concurrent control flushes converge to the latest parameter revision");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            AudioEngine::RuntimeGlobalParams initialParams;
            initialParams.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            initialParams.outputMixRaw = 100.0f;
            initialParams.outputLimiterDb = 0.0f;
            engine.updateGlobalParams(initialParams);
            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();

            std::atomic<bool> start{ false };
            std::thread parameterThread([&]
                {
                    while (!start.load(std::memory_order_acquire))
                        std::this_thread::yield();

                    for (int i = 0; i < 256; ++i)
                    {
                        auto updated = initialParams;
                        updated.outputVolumeDb = -6.0f + (float)(i % 7);
                        updated.gainA = 0.75f + 0.01f * (float)(i % 20);
                        updated.panA = -0.25f + 0.05f * (float)(i % 11);
                        engine.updateGlobalParams(updated);

                        if ((i & 3) == 0)
                            engine.synchronizeProcessingState();
                    }
                });

            start.store(true, std::memory_order_release);

            for (int i = 0; i < 24; ++i)
            {
                const auto pedalID = "flush-race-delay-" + juce::String(i);
                engine.addPedal("Delay", Nova::ChainID::LineA, 0, Nova::ZoneID::FX, pedalID);
                engine.synchronizeProcessingState();
                engine.removePedal(Nova::ChainID::LineA, 0);
                engine.synchronizeProcessingState();
            }

            parameterThread.join();

            auto finalParams = initialParams;
            finalParams.outputVolumeDb = -3.0f;
            finalParams.gainA = 0.91f;
            finalParams.panA = 0.12f;
            engine.updateGlobalParams(finalParams);
            const auto expectedRevision = AudioEngineTestAccess::getRuntimeParamRevision(engine);

            for (int attempt = 0;
                 attempt < 500 && AudioEngineTestAccess::getAppliedParamRevision(engine) != expectedRevision;
                 ++attempt)
            {
                engine.synchronizeProcessingState();
                juce::Thread::sleep(1);
            }

            expectEquals(AudioEngineTestAccess::getAppliedParamRevision(engine), expectedRevision,
                "Active runtime must converge to the latest parameter revision after concurrent flushes");
            expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), 0,
                "Concurrent flush serialization must preserve queued add/remove command order");

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            juce::MidiBuffer midi;
            bool finite = true;

            for (int blockIndex = 0; blockIndex < 24; ++blockIndex)
            {
                for (int i = 0; i < kBlockSize; ++i)
                {
                    const int sampleIndex = blockIndex * kBlockSize + i;
                    const float t = (float)sampleIndex / (float)kSampleRate;
                    const float sample = 0.08f
                        * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                    buffer.setSample(0, i, sample);
                    buffer.setSample(1, i, sample * 0.97f);
                }

                engine.process(buffer, midi);
                finite = finite && bufferHasOnlyFiniteSamples(buffer);
            }

            expect(finite, "Audio output must remain finite after concurrent control-path flushing");
        }

        beginTest("GraphBuilder builds runtime with global processors and captured latency");
        {
            Nova::Audio::GraphBuildRequest request;
            request.sampleRate = kSampleRate;
            request.blockSize = kBlockSize;
            request.numInputs = 2;
            request.numOutputs = 2;
            request.generation = 42;
            request.runtimeParamRevision = 1;
            request.runtimeParams.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            request.runtimeParams.outputMixRaw = 100.0f;
            request.runtimeParams.outputLimiterDb = 0.0f;

            Nova::Audio::GraphBuilder builder;
            const auto result = builder.build(request);

            expect(result.runtime != nullptr, "GraphBuilder should return a runtime instance");
            expectEquals((int)result.warnings.size(), 0);

            if (result.runtime != nullptr)
            {
                const auto& runtime = *result.runtime;
                expect(runtime.graph != nullptr, "GraphBuilder should create an AudioProcessorGraph");
                expect(runtime.inputNode != nullptr, "GraphBuilder should create input IO node");
                expect(runtime.outputNode != nullptr, "GraphBuilder should create output IO node");
                expect(runtime.inputChainNode != nullptr, "GraphBuilder should create input-chain node");
                expect(runtime.stripNodeA != nullptr, "GraphBuilder should create strip A node");
                expect(runtime.stripNodeB != nullptr, "GraphBuilder should create strip B node");
                expect(runtime.outputChainNode != nullptr, "GraphBuilder should create output-chain node");
                expect(runtime.inputChain != nullptr, "GraphBuilder should cache InputChain processor pointer");
                expect(runtime.stripA != nullptr, "GraphBuilder should cache strip A processor pointer");
                expect(runtime.stripB != nullptr, "GraphBuilder should cache strip B processor pointer");
                expect(runtime.outputChain != nullptr, "GraphBuilder should cache OutputChain processor pointer");
                expectEquals(runtime.appliedParamRevision.load(std::memory_order_relaxed),
                    request.runtimeParamRevision);

                juce::AudioBuffer<float> buffer(2, kBlockSize);
                juce::MidiBuffer midi;
                for (int i = 0; i < kBlockSize; ++i)
                {
                    const float t = (float)i / (float)kSampleRate;
                    buffer.setSample(0, i, 0.10f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t));
                    buffer.setSample(1, i, 0.08f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t + 0.2f));
                }

                runtime.graph->processBlock(buffer, midi);
                expect(bufferHasOnlyFiniteSamples(buffer), "GraphBuilder runtime should process finite output");

                const int expectedLatency = juce::jlimit(0,
                    Nova::Config::MAX_GRAPH_LATENCY_SAMPLES,
                    runtime.graph->getLatencySamples());
                expectEquals(runtime.latencySamples, expectedLatency);
            }
        }

        beginTest("GraphBuilder preserves canonical zone order for mixed insertion");
        {
            const auto unknownZone = static_cast<Nova::ZoneID>(999);

            Nova::Audio::GraphBuildRequest request;
            request.sampleRate = kSampleRate;
            request.blockSize = kBlockSize;
            request.numInputs = 2;
            request.numOutputs = 2;
            request.generation = 43;
            request.runtimeParamRevision = 1;
            request.runtimeParams.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            request.runtimeParams.outputMixRaw = 100.0f;
            request.runtimeParams.outputLimiterDb = 0.0f;

            request.modelChainA.push_back({ "Reverb", "gb-fx", Nova::ZoneID::FX, false });
            request.modelChainA.push_back({ "Cabinet", "gb-cab", Nova::ZoneID::Cabinet, false });
            request.modelChainA.push_back({ "Overdrive", "gb-unknown", unknownZone, false });
            request.modelChainA.push_back({ "Overdrive", "gb-pre", Nova::ZoneID::Pre, false });
            request.modelChainA.push_back({ "Clean Amp", "gb-amp", Nova::ZoneID::Amp, false });

            Nova::Audio::GraphBuilder builder;
            const auto result = builder.build(request);
            expect(result.runtime != nullptr, "GraphBuilder should build mixed-zone runtime");

            if (result.runtime != nullptr)
            {
                const auto& runtime = *result.runtime;
                expect(runtime.graph != nullptr, "Mixed-zone runtime should own a graph");
                expect(runtime.inputChainNode != nullptr, "Mixed-zone runtime should own input chain node");
                expect(runtime.stripNodeA != nullptr, "Mixed-zone runtime should own strip A node");
                expectEquals((int)runtime.chainA.size(), 5);

                const auto findNodeId = [&runtime](const juce::String& pedalID,
                    juce::AudioProcessorGraph::NodeID& outNodeID)
                {
                    for (const auto& slot : runtime.chainA)
                    {
                        if (slot.pedalID == pedalID && slot.node != nullptr)
                        {
                            outNodeID = slot.node->nodeID;
                            return true;
                        }
                    }

                    return false;
                };

                juce::AudioProcessorGraph::NodeID preNode;
                juce::AudioProcessorGraph::NodeID ampNode;
                juce::AudioProcessorGraph::NodeID fxNode;
                juce::AudioProcessorGraph::NodeID cabNode;
                juce::AudioProcessorGraph::NodeID unknownNode;
                const bool hasPre = findNodeId("gb-pre", preNode);
                const bool hasAmp = findNodeId("gb-amp", ampNode);
                const bool hasFx = findNodeId("gb-fx", fxNode);
                const bool hasCab = findNodeId("gb-cab", cabNode);
                const bool hasUnknown = findNodeId("gb-unknown", unknownNode);

                if (runtime.graph != nullptr
                    && runtime.inputChainNode != nullptr
                    && runtime.stripNodeA != nullptr
                    && hasPre
                    && hasAmp
                    && hasFx
                    && hasCab
                    && hasUnknown)
                {
                    expect(runtime.graph->isConnected(runtime.inputChainNode->nodeID, preNode),
                        "Input chain should feed the pre-zone pedal first");
                    expect(runtime.graph->isConnected(preNode, ampNode),
                        "Pre-zone pedal should feed amp-zone pedal");
                    expect(runtime.graph->isConnected(ampNode, fxNode),
                        "Amp-zone pedal should feed FX-zone pedal");
                    expect(runtime.graph->isConnected(fxNode, cabNode),
                        "FX-zone pedal should feed cabinet-zone pedal");
                    expect(runtime.graph->isConnected(cabNode, unknownNode),
                        "Cabinet-zone pedal should feed unknown-zone pedal");
                    expect(runtime.graph->isConnected(unknownNode, runtime.stripNodeA->nodeID),
                        "Unknown-zone pedal should feed strip A");
                    expect(!runtime.graph->isConnected(runtime.inputChainNode->nodeID, fxNode),
                        "Input chain should not bypass canonical ordering to feed FX directly");
                }
                else
                {
                    expect(false, "Mixed-zone runtime should include all expected pedal nodes");
                }
            }
        }

        beginTest("GraphBuilder captures latency from rebuilt graph on latency-relevant chain");
        {
            Nova::Audio::GraphBuildRequest request;
            request.sampleRate = kSampleRate;
            request.blockSize = kBlockSize;
            request.numInputs = 2;
            request.numOutputs = 2;
            request.generation = 44;
            request.runtimeParamRevision = 1;
            request.runtimeParams.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            request.runtimeParams.outputMixRaw = 100.0f;
            request.runtimeParams.outputLimiterDb = 0.0f;
            request.modelChainA.push_back({ "Overdrive", "gb-latency-overdrive", Nova::ZoneID::Pre, false });

            Nova::Audio::GraphBuilder builder;
            const auto first = builder.build(request);
            expect(first.runtime != nullptr, "Latency chain build should return runtime");

            request.generation = 45;
            const auto second = builder.build(request);
            expect(second.runtime != nullptr, "Equivalent latency chain rebuild should return runtime");

            if (first.runtime != nullptr && second.runtime != nullptr)
            {
                const int expectedFirst = juce::jlimit(0,
                    Nova::Config::MAX_GRAPH_LATENCY_SAMPLES,
                    first.runtime->graph->getLatencySamples());
                const int expectedSecond = juce::jlimit(0,
                    Nova::Config::MAX_GRAPH_LATENCY_SAMPLES,
                    second.runtime->graph->getLatencySamples());

                expectEquals(first.runtime->latencySamples, expectedFirst);
                expectEquals(second.runtime->latencySamples, expectedSecond);
                expect(first.runtime->latencySamples > 0,
                    "Latency-relevant chain should report positive latency after rebuild capture");
                expectEquals(first.runtime->latencySamples, second.runtime->latencySamples);
            }
        }

        beginTest("GraphBuilder skips invalid pedal without fatal build failure");
        {
            Nova::Audio::GraphBuildRequest request;
            request.sampleRate = kSampleRate;
            request.blockSize = kBlockSize;
            request.numInputs = 2;
            request.numOutputs = 2;
            request.generation = 46;
            request.runtimeParamRevision = 1;
            request.runtimeParams.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            request.runtimeParams.outputMixRaw = 100.0f;
            request.runtimeParams.outputLimiterDb = 0.0f;
            request.modelChainA.push_back({ "NotARealPedalType", "gb-invalid", Nova::ZoneID::Pre, false });
            request.modelChainA.push_back({ "Delay", "gb-valid-delay", Nova::ZoneID::FX, false });

            Nova::Audio::GraphBuilder builder;
            const auto result = builder.build(request);

            expect(result.runtime != nullptr, "Invalid pedal should not fail the whole graph build");
            expect(result.warnings.size() >= 1, "Invalid pedal should produce a non-fatal build warning");

            if (result.runtime != nullptr)
            {
                expectEquals((int)result.runtime->chainA.size(), 1);
                if (!result.runtime->chainA.empty())
                    expectEquals(result.runtime->chainA.front().pedalID, juce::String("gb-valid-delay"));
            }

            const bool hasCreatePedalWarning = std::any_of(result.warnings.begin(),
                result.warnings.end(),
                [](const Nova::Audio::GraphBuildWarning& warning)
                {
                    return warning.code == "createPedal.null";
                });
            expect(hasCreatePedalWarning, "Invalid pedal should emit createPedal.null warning");
        }

        beginTest("GraphBuilder preserves ProcessorBase and TempoSyncable caches");
        {
            Nova::Audio::GraphBuildRequest request;
            request.sampleRate = kSampleRate;
            request.blockSize = kBlockSize;
            request.numInputs = 2;
            request.numOutputs = 2;
            request.generation = 47;
            request.runtimeParamRevision = 2;
            request.runtimeParams.switchMode = (int)Nova::SwitcherMode::LineA_Only;
            request.runtimeParams.outputMixRaw = 100.0f;
            request.runtimeParams.hostTempoBpm = 167.0f;
            request.runtimeParams.hostTempoValid = true;
            request.runtimeParams.hostTransportPlaying = true;
            request.modelChainA.push_back({ "Overdrive", "gb-cache-overdrive", Nova::ZoneID::Pre, false });
            request.modelChainA.push_back({ "Delay", "gb-cache-delay", Nova::ZoneID::FX, false });

            Nova::Audio::GraphBuilder builder;
            const auto result = builder.build(request);
            expect(result.runtime != nullptr, "Cache coverage chain should build");

            if (result.runtime != nullptr)
            {
                const auto& chain = result.runtime->chainA;
                expectEquals((int)chain.size(), 2);
                if (chain.size() == 2)
                {
                    const auto& overdrive = chain[0];
                    const auto& delay = chain[1];
                    expect(overdrive.baseProcessor != nullptr, "ProcessorBase cache should be populated for Overdrive");
                    expect(overdrive.tempoSyncProcessor == nullptr, "Overdrive should not expose TempoSyncable cache");
                    expect(delay.baseProcessor != nullptr, "ProcessorBase cache should be populated for Delay");
                    expect(delay.tempoSyncProcessor != nullptr, "TempoSyncable cache should be populated for Delay");
                }
            }
        }

        beginTest("AudioEngine preserves synchronized topology created before prepare");
        {
            AudioEngine engine;
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "pre-prepare-overdrive");
            engine.addPedal("Classic Amp", Nova::ChainID::LineA, 1, Nova::ZoneID::Amp, "pre-prepare-amp");
            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();

            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.synchronizeProcessingState();

            const auto report = engine.buildDiagnosticReport();
            expect(report.contains("processor=Overdrive"), "Pre-prepare Overdrive should survive prepare()");
            expect(report.contains("processor=Classic Amp"), "Pre-prepare amp should survive prepare()");
            expect(report.contains("pedalID=pre-prepare-overdrive"), "Overdrive pedal ID should survive prepare()");
            expect(report.contains("pedalID=pre-prepare-amp"), "Amp pedal ID should survive prepare()");
            expect(report.contains("engineOn=true"), "Engine enable should be materialized without waiting for process()");
        }

        beginTest("Plugin state migration stamps schema and canonicalizes loaded topology");
        {
            NOVAAudioProcessor processor;

            juce::ValueTree legacyState(Nova::IDs::MAIN_STATE);
            auto settings = juce::ValueTree(Nova::IDs::SETTINGS);
            settings.setProperty(Nova::IDs::ENGINE_ON, false, nullptr);
            settings.setProperty(Nova::IDs::SWITCH_MODE, (int)Nova::SwitcherMode::LineA_Only, nullptr);
            legacyState.appendChild(settings, nullptr);

            auto lineA = juce::ValueTree(Nova::IDs::LINE_A);
            lineA.setProperty(Nova::IDs::MIXER_GAIN_A, 1.0f, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_PAN_A, 0.0f, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f, nullptr);

            auto ampA = juce::ValueTree(Nova::IDs::PEDAL);
            ampA.setProperty(Nova::IDs::PEDAL_TYPE, "Classic Amp", nullptr);
            ampA.setProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Amp, nullptr);
            ampA.setProperty(Nova::IDs::PEDAL_ENABLED, true, nullptr);
            lineA.appendChild(ampA, nullptr);

            auto pre = juce::ValueTree(Nova::IDs::PEDAL);
            pre.setProperty(Nova::IDs::PEDAL_TYPE, "Overdrive", nullptr);
            pre.setProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre, nullptr);
            lineA.appendChild(pre, nullptr);

            auto ampB = juce::ValueTree(Nova::IDs::PEDAL);
            ampB.setProperty(Nova::IDs::PEDAL_TYPE, "Classic Amp", nullptr);
            ampB.setProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Amp, nullptr);
            lineA.appendChild(ampB, nullptr);

            legacyState.appendChild(lineA, nullptr);
            legacyState.appendChild(juce::ValueTree(Nova::IDs::LINE_B), nullptr);

            juce::MemoryOutputStream inputStream;
            legacyState.writeToStream(inputStream);
            processor.setStateInformation(inputStream.getData(), (int)inputStream.getDataSize());

            juce::MemoryBlock roundTripped;
            processor.getStateInformation(roundTripped);
            const auto migrated = juce::ValueTree::readFromData(roundTripped.getData(), (int)roundTripped.getSize());

            expect(migrated.isValid(), "Migrated state should deserialize");
            expectEquals((int)migrated.getProperty(Nova::IDs::STATE_SCHEMA_VERSION, -1), Nova::Config::STATE_SCHEMA_VERSION);

            const auto migratedLineA = migrated.getChildWithName(Nova::IDs::LINE_A);
            expectEquals(migratedLineA.getNumChildren(), 2);

            const auto firstPedal = migratedLineA.getChild(0);
            const auto secondPedal = migratedLineA.getChild(1);
            expectEquals(firstPedal.getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Overdrive"));
            expectEquals((int)firstPedal.getProperty(Nova::IDs::PEDAL_ZONE, -1), (int)Nova::ZoneID::Pre);
            expectEquals(secondPedal.getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Classic Amp"));
            expectEquals((int)secondPedal.getProperty(Nova::IDs::PEDAL_ZONE, -1), (int)Nova::ZoneID::Amp);
            expect(firstPedal.hasProperty(Nova::IDs::PEDAL_ID), "Legacy pedal should receive a generated ID");
            expect(secondPedal.hasProperty(Nova::IDs::PEDAL_ID), "Legacy pedal should receive a generated ID");
        }

        beginTest("PluginStateModel reset produces a canonical clean session");
        {
            juce::ValueTree state(Nova::IDs::MAIN_STATE);
            Nova::PluginStateModel::resetToCleanState(state);

            expectEquals(Nova::PluginStateModel::getStateSchemaVersion(state), Nova::Config::STATE_SCHEMA_VERSION);

            const auto settings = Nova::PluginStateModel::getSettingsTree(state);
            const auto lineA = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineA);
            const auto lineB = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineB);

            expect(settings.isValid(), "Reset state should create settings");
            expect(lineA.isValid(), "Reset state should create line A");
            expect(lineB.isValid(), "Reset state should create line B");
            expectEquals(lineA.getNumChildren(), 0);
            expectEquals(lineB.getNumChildren(), 0);
            expect((bool) settings.getProperty(Nova::IDs::ENGINE_ON, true) == false,
                "Reset state should disable the engine");
            expectEquals((int) settings.getProperty(Nova::IDs::SWITCH_MODE, -1), (int) Nova::SwitcherMode::LineA_Only);
            expectEquals((float) lineA.getProperty(Nova::IDs::MIXER_GAIN_A, 0.0f), 1.0f);
            expectEquals((float) lineB.getProperty(Nova::IDs::MIXER_GAIN_B, 0.0f), 1.0f);
        }

        beginTest("PluginStateModel insertPedal keeps canonical order and single amp");
        {
            juce::ValueTree state(Nova::IDs::MAIN_STATE);
            Nova::PluginStateModel::resetToCleanState(state);

            const auto amp1 = Nova::PluginStateModel::insertPedal(state, "Classic Amp", Nova::ChainID::LineA, Nova::ZoneID::Amp);
            const auto pre = Nova::PluginStateModel::insertPedal(state, "Overdrive", Nova::ChainID::LineA, Nova::ZoneID::Pre);
            const auto fx = Nova::PluginStateModel::insertPedal(state, "Reverb", Nova::ChainID::LineA, Nova::ZoneID::FX);
            const auto amp2 = Nova::PluginStateModel::insertPedal(state, "Classic Amp", Nova::ChainID::LineA, Nova::ZoneID::Amp);

            expect(amp1.inserted, "First amp insert should succeed");
            expect(pre.inserted, "Pre insert should succeed");
            expect(fx.inserted, "FX insert should succeed");
            expect(amp2.inserted, "Second amp insert should replace previous amp");

            const auto lineA = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineA);
            expectEquals(lineA.getNumChildren(), 3);
            expectEquals(lineA.getChild(0).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Overdrive"));
            expectEquals((int) lineA.getChild(0).getProperty(Nova::IDs::PEDAL_ZONE, -1), (int) Nova::ZoneID::Pre);
            expectEquals(lineA.getChild(1).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Classic Amp"));
            expectEquals((int) lineA.getChild(1).getProperty(Nova::IDs::PEDAL_ZONE, -1), (int) Nova::ZoneID::Amp);
            expectEquals(lineA.getChild(2).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Reverb"));
            expectEquals((int) lineA.getChild(2).getProperty(Nova::IDs::PEDAL_ZONE, -1), (int) Nova::ZoneID::FX);
            expectEquals(lineA.getChild(1).getProperty(Nova::IDs::PEDAL_ID).toString(), amp2.pedalID);
        }

        beginTest("PedalRegistry exposes the expanded commercial pedal catalog");
        {
            expect(PedalRegistry::isTypeSupported("Compressor"), "Compressor should be registered");
            expect(PedalRegistry::isTypeSupported("Chorus"), "Chorus should be registered");
            expect(PedalRegistry::isTypeSupported("Boost"), "Boost should be registered");
            expect(PedalRegistry::isTypeSupported("Neural"), "Neural should be registered");
            expect(PedalRegistry::isTypeSupported("Wah"), "Wah should be registered");
            expect(PedalRegistry::isTypeSupported("Auto Wah"), "Auto Wah should resolve to Wah for legacy recall");
            expect(PedalRegistry::isTypeSupported("Octave"), "Octave should be registered");
            expect(PedalRegistry::isTypeSupported("Metal Distortion"), "Metal Distortion should resolve to Distortion for legacy recall");
            expectEquals(PedalRegistry::canonicalType("Metal Distortion"), juce::String("Distortion"));
            expectEquals(PedalRegistry::canonicalType("Autowah"), juce::String("Wah"));
            expectEquals(PedalRegistry::canonicalType("Auto Wah"), juce::String("Wah"));

            const auto preTypes = PedalRegistry::getPedalTypesForZone(Nova::ZoneID::Pre);
            const auto fxTypes = PedalRegistry::getPedalTypesForZone(Nova::ZoneID::FX);

            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Compressor")) != preTypes.end(),
                "Compressor should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Boost")) != preTypes.end(),
                "Boost should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Neural")) != preTypes.end(),
                "Neural should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Wah")) != preTypes.end(),
                "Wah should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Auto Wah")) == preTypes.end(),
                "Auto Wah should no longer appear as a dedicated catalog entry");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Octave")) != preTypes.end(),
                "Octave should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Distortion")) != preTypes.end(),
                "Distortion should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Metal Distortion")) == preTypes.end(),
                "Metal Distortion should no longer appear as a dedicated catalog entry");
            expect(std::find(fxTypes.begin(), fxTypes.end(), juce::String("Chorus")) != fxTypes.end(),
                "Chorus should be available in the FX zone");
        }

        beginTest("BoostPedal round-trips its preamp state");
        {
            BoostPedal source;
            source.gainParam->setValueNotifyingHost(source.gainParam->convertTo0to1(13.5f));
            source.toneParam->setValueNotifyingHost(source.toneParam->convertTo0to1(0.72f));
            source.tightParam->setValueNotifyingHost(source.tightParam->convertTo0to1(0.48f));
            source.charParam->setValueNotifyingHost(source.charParam->convertTo0to1(0.64f));
            source.midParam->setValueNotifyingHost(source.midParam->convertTo0to1(2.4f));
            source.levelParam->setValueNotifyingHost(source.levelParam->convertTo0to1(1.28f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            BoostPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.gainParam->get(), 13.5f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 0.72f, 1.0e-3f));
            expect(approximatelyEqual(restored.tightParam->get(), 0.48f, 1.0e-3f));
            expect(approximatelyEqual(restored.charParam->get(), 0.64f, 1.0e-3f));
            expect(approximatelyEqual(restored.midParam->get(), 2.4f, 0.05f));
            expect(approximatelyEqual(restored.levelParam->get(), 1.28f, 0.01f));
        }

        beginTest("BoostPedal gain and character add measurable push without instability");
        {
            juce::AudioBuffer<float> input(2, (int) (kSampleRate * 1.0));
            for (int ch = 0; ch < input.getNumChannels(); ++ch)
            {
                for (int i = 0; i < input.getNumSamples(); ++i)
                {
                    const float phase = (float) (2.0 * juce::MathConstants<double>::pi * 196.0 * (double) i / kSampleRate);
                    input.setSample(ch, i, 0.12f * std::sin(phase + ch * 0.17f));
                }
            }

            BoostPedal clean;
            clean.prepareToPlay(kSampleRate, kBlockSize);
            clean.gainParam->setValueNotifyingHost(clean.gainParam->convertTo0to1(0.0f));
            clean.toneParam->setValueNotifyingHost(clean.toneParam->convertTo0to1(0.55f));
            clean.tightParam->setValueNotifyingHost(clean.tightParam->convertTo0to1(0.18f));
            clean.charParam->setValueNotifyingHost(clean.charParam->convertTo0to1(0.0f));
            clean.midParam->setValueNotifyingHost(clean.midParam->convertTo0to1(0.0f));
            clean.levelParam->setValueNotifyingHost(clean.levelParam->convertTo0to1(1.0f));
            clean.reset();
            const auto cleanOut = renderBoostOutput(clean, input, kBlockSize);

            BoostPedal pushed;
            pushed.prepareToPlay(kSampleRate, kBlockSize);
            pushed.gainParam->setValueNotifyingHost(pushed.gainParam->convertTo0to1(16.0f));
            pushed.toneParam->setValueNotifyingHost(pushed.toneParam->convertTo0to1(0.64f));
            pushed.tightParam->setValueNotifyingHost(pushed.tightParam->convertTo0to1(0.22f));
            pushed.charParam->setValueNotifyingHost(pushed.charParam->convertTo0to1(0.82f));
            pushed.midParam->setValueNotifyingHost(pushed.midParam->convertTo0to1(1.5f));
            pushed.levelParam->setValueNotifyingHost(pushed.levelParam->convertTo0to1(1.0f));
            pushed.reset();
            const auto pushedOut = renderBoostOutput(pushed, input, kBlockSize);

            expect(bufferHasOnlyFiniteSamples(pushedOut), "Driven boost output must stay finite");
            expect(computeWindowRms(pushedOut, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.5))
                > computeWindowRms(cleanOut, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.5)) * 1.35,
                "Higher gain and character should push substantially more signal");
            expect(computeBufferNullRms(cleanOut, pushedOut) > 0.02,
                "Gain and character changes should audibly reshape the boost stage");
        }

        beginTest("BoostPedal tight control trims low-end energy before the preamp");
        {
            juce::AudioBuffer<float> input(2, (int) (kSampleRate * 1.0));
            for (int ch = 0; ch < input.getNumChannels(); ++ch)
            {
                for (int i = 0; i < input.getNumSamples(); ++i)
                {
                    const float phase = (float) (2.0 * juce::MathConstants<double>::pi * 90.0 * (double) i / kSampleRate);
                    input.setSample(ch, i, 0.18f * std::sin(phase));
                }
            }

            auto renderTight = [&](float tight)
            {
                BoostPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(12.0f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.56f));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(tight));
                pedal.charParam->setValueNotifyingHost(pedal.charParam->convertTo0to1(0.46f));
                pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(0.0f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
                pedal.reset();
                return renderBoostOutput(pedal, input, kBlockSize);
            };

            const auto loose = renderTight(0.06f);
            const auto tight = renderTight(0.92f);

            expect(computeWindowRms(tight, (int) (kSampleRate * 0.20), (int) (kSampleRate * 0.5))
                < computeWindowRms(loose, (int) (kSampleRate * 0.20), (int) (kSampleRate * 0.5)) * 0.35,
                "High tight should clearly trim low-end energy");
        }

        beginTest("BoostPedal automation stress remains finite");
        {
            BoostPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);

            for (int iteration = 0; iteration < 120; ++iteration)
            {
                const float t = (float) iteration / 119.0f;
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(juce::jmap(std::sin(t * 5.0f), -1.0f, 1.0f, 0.0f, 22.0f)));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(juce::jmap(std::cos(t * 7.2f), -1.0f, 1.0f, 0.02f, 0.98f)));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(juce::jmap(std::sin(t * 9.1f + 0.6f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.charParam->setValueNotifyingHost(pedal.charParam->convertTo0to1(juce::jmap(std::cos(t * 8.3f + 0.2f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(juce::jmap(std::sin(t * 6.7f + 1.1f), -1.0f, 1.0f, -6.0f, 6.0f)));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(juce::jmap(std::cos(t * 4.6f), -1.0f, 1.0f, 0.7f, 1.7f)));

                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                {
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * kBlockSize + i);
                        block.setSample(ch, i, 0.16f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 165.0 * sampleIndex / kSampleRate) + ch * 0.21f));
                    }
                }

                pedal.processBlock(block, midi);
                expect(bufferHasOnlyFiniteSamples(block), "Boost automation should keep every sample finite");
            }
        }

        beginTest("OctavePedal round-trips its linked tracking state");
        {
            OctavePedal source;
            source.subParam->setValueNotifyingHost(source.subParam->convertTo0to1(0.86f));
            source.upperParam->setValueNotifyingHost(source.upperParam->convertTo0to1(0.54f));
            source.dryParam->setValueNotifyingHost(source.dryParam->convertTo0to1(0.38f));
            source.toneParam->setValueNotifyingHost(source.toneParam->convertTo0to1(0.73f));
            source.levelParam->setValueNotifyingHost(source.levelParam->convertTo0to1(1.24f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            OctavePedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.subParam->get(), 0.86f, 1.0e-3f));
            expect(approximatelyEqual(restored.upperParam->get(), 0.54f, 1.0e-3f));
            expect(approximatelyEqual(restored.dryParam->get(), 0.38f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 0.73f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 1.24f, 0.01f));
        }

        beginTest("OctavePedal dry-only settings stay transparent");
        {
            OctavePedal pedal;
            pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(0.0f));
            pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(0.0f));
            pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(1.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.84f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::AudioBuffer<float> input(2, (int) (kSampleRate * 0.9));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.17f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t));
                input.setSample(1, i, 0.13f * std::sin(juce::MathConstants<float>::twoPi * 247.0f * t));
            }

            const auto output = renderOctaveOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Dry-only octave render must stay finite");
            expect(nullRms <= 1.0e-6, "Dry-only settings should leave the signal effectively untouched");
        }

        beginTest("OctavePedal sanitizes NaN/Inf input and recovers to finite output");
        {
            OctavePedal pedal;
            pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(0.72f));
            pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(0.35f));
            pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(0.45f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.60f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::AudioBuffer<float> dirty(2, kBlockSize * 20);
            dirty.clear();
            for (int i = 0; i < dirty.getNumSamples(); ++i)
            {
                const float clean = 0.14f * std::sin(juce::MathConstants<float>::twoPi * 110.0f
                    * (float) i / (float) kSampleRate);
                const float v = (i % 11 == 0) ? std::numeric_limits<float>::quiet_NaN()
                              : (i % 17 == 0) ? std::numeric_limits<float>::infinity()
                              : (i % 23 == 0) ? -std::numeric_limits<float>::infinity()
                              : clean;
                dirty.setSample(0, i, v);
                dirty.setSample(1, i, v);
            }

            const auto dirtyOutput = renderOctaveOutput(pedal, dirty, kBlockSize);

            juce::AudioBuffer<float> recovery(2, kBlockSize * 20);
            recovery.clear();
            for (int i = 0; i < recovery.getNumSamples(); ++i)
            {
                const float clean = 0.14f * std::sin(juce::MathConstants<float>::twoPi * 146.83f
                    * (float) i / (float) kSampleRate);
                recovery.setSample(0, i, clean);
                recovery.setSample(1, i, clean);
            }

            const auto recoveryOutput = renderOctaveOutput(pedal, recovery, kBlockSize);
            expect(bufferHasOnlyFiniteSamples(dirtyOutput), "Octave must scrub NaN/Inf input to finite output");
            expect(bufferHasOnlyFiniteSamples(recoveryOutput), "Octave state must recover to finite output after invalid input");
        }

        beginTest("OctavePedal sub voice locks below the played note");
        {
            OctavePedal pedal;
            pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(1.0f));
            pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(0.0f));
            pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(0.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.34f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::AudioBuffer<float> input(2, (int) (kSampleRate * 1.2));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                const float sample = 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderOctaveOutput(pedal, input, kBlockSize);
            const int analysisStart = (int) (kSampleRate * 0.35);
            const int analysisLength = (int) (kSampleRate * 0.45);
            const double subMag = computeFrequencyMagnitude(output, kSampleRate, 110.0f, analysisStart, analysisLength);
            const double fundamentalMag = computeFrequencyMagnitude(output, kSampleRate, 220.0f, analysisStart, analysisLength);

            expect(subMag > fundamentalMag * 1.70, "Sub voice should carry substantially more 110 Hz than the original 220 Hz");
            expect(computeWindowRms(output, analysisStart, analysisLength) > 0.03,
                "Tracked sub voice should produce a sustained low octave body");
        }

        beginTest("OctavePedal upper voice emphasizes the octave harmonic");
        {
            OctavePedal pedal;
            pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(0.0f));
            pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(1.0f));
            pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(0.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.82f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::AudioBuffer<float> input(2, (int) (kSampleRate * 1.2));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                const float sample = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderOctaveOutput(pedal, input, kBlockSize);
            const int analysisStart = (int) (kSampleRate * 0.28);
            const int analysisLength = (int) (kSampleRate * 0.45);
            const double upperMag = computeFrequencyMagnitude(output, kSampleRate, 440.0f, analysisStart, analysisLength);
            const double fundamentalMag = computeFrequencyMagnitude(output, kSampleRate, 220.0f, analysisStart, analysisLength);

            expect(upperMag > fundamentalMag * 1.18, "Upper voice should lean toward the generated octave harmonic");
            expect(computeWindowRms(output, analysisStart, analysisLength) > 0.02,
                "Upper voice should produce a usable octave-up sustain");
        }

        beginTest("OctavePedal tone control materially reshapes the generated voice");
        {
            auto renderTone = [&](float tone)
            {
                OctavePedal pedal;
                pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(0.48f));
                pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(0.92f));
                pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(0.0f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(tone));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
                pedal.prepareToPlay(kSampleRate, kBlockSize);

                juce::AudioBuffer<float> input(2, (int) (kSampleRate * 1.1));
                input.clear();
                for (int i = 0; i < input.getNumSamples(); ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    const float sample = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderOctaveOutput(pedal, input, kBlockSize);
            };

            const auto dark = renderTone(0.08f);
            const auto bright = renderTone(0.92f);
            const int analysisStart = (int) (kSampleRate * 0.30);
            const int analysisLength = (int) (kSampleRate * 0.40);
            const double darkUpperMag = computeFrequencyMagnitude(dark, kSampleRate, 392.0f, analysisStart, analysisLength);
            const double brightUpperMag = computeFrequencyMagnitude(bright, kSampleRate, 392.0f, analysisStart, analysisLength);

            expect(brightUpperMag > darkUpperMag * 1.24,
                "Higher tone should noticeably open the generated upper octave");
            expect(computeBufferNullRms(dark, bright) > 0.015,
                "Tone should materially change the octave voicing rather than act as a cosmetic trim");
        }

        beginTest("OctavePedal automation stress remains finite");
        {
            OctavePedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            double peak = 0.0;

            for (int iteration = 0; iteration < 120; ++iteration)
            {
                const float t = (float) iteration / 119.0f;
                pedal.subParam->setValueNotifyingHost(pedal.subParam->convertTo0to1(juce::jmap(std::sin(t * 6.0f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.upperParam->setValueNotifyingHost(pedal.upperParam->convertTo0to1(juce::jmap(std::cos(t * 7.4f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.dryParam->setValueNotifyingHost(pedal.dryParam->convertTo0to1(juce::jmap(std::sin(t * 5.2f + 0.8f), -1.0f, 1.0f, 0.0f, 1.0f)));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(juce::jmap(std::cos(t * 8.1f + 0.4f), -1.0f, 1.0f, 0.02f, 0.98f)));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(juce::jmap(std::sin(t * 4.8f + 1.1f), -1.0f, 1.0f, 0.7f, 1.5f)));

                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                {
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * kBlockSize + i);
                        const float fundamental = 0.12f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 123.0 * sampleIndex / kSampleRate));
                        const float harmonic = 0.05f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 246.0 * sampleIndex / kSampleRate));
                        block.setSample(ch, i, fundamental + harmonic);
                    }
                }

                pedal.processBlock(block, midi);
                expect(bufferHasOnlyFiniteSamples(block), "Octave automation should keep every sample finite");
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
            }

            expect(peak < 2.0, "Octave automation should remain inside a sane peak ceiling");
        }

        beginTest("PhaserPedal round-trips its modulation state");
        {
            PhaserPedal source;
            source.modeParam->setValueNotifyingHost(normalisedChoiceIndex(source.modeParam, 2));
            source.rateParam->setValueNotifyingHost(source.rateParam->convertTo0to1(1.8f));
            source.depthParam->setValueNotifyingHost(source.depthParam->convertTo0to1(0.81f));
            source.feedbackParam->setValueNotifyingHost(source.feedbackParam->convertTo0to1(0.36f));
            source.stagesParam->setValueNotifyingHost(source.stagesParam->convertTo0to1(8.0f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.74f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            PhaserPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 2);
            expect(approximatelyEqual(restored.rateParam->get(), 1.8f, 0.01f));
            expect(approximatelyEqual(restored.depthParam->get(), 0.81f, 1.0e-3f));
            expect(approximatelyEqual(restored.feedbackParam->get(), 0.36f, 0.01f));
            expect(approximatelyEqual(restored.stagesParam->get(), 8.0f, 0.1f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.74f, 1.0e-3f));
        }

        beginTest("PhaserPedal mix zero keeps the dry path transparent");
        {
            PhaserPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(3.2f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.92f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.72f));
            pedal.stagesParam->setValueNotifyingHost(pedal.stagesParam->convertTo0to1(10.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.0f));

            juce::AudioBuffer<float> input(2, 2048);
            for (int ch = 0; ch < input.getNumChannels(); ++ch)
            {
                for (int i = 0; i < input.getNumSamples(); ++i)
                {
                    const float phase = (float) (2.0 * juce::MathConstants<double>::pi * 220.0 * (double) i / kSampleRate);
                    input.setSample(ch, i, 0.17f * std::sin(phase + ch * 0.19f));
                }
            }

            const auto output = renderPhaserOutput(pedal, input, kBlockSize);
            expect(computeBufferNullRms(output, input) <= 1.0e-6,
                "Mix at zero should leave the dry path untouched");
        }

        beginTest("PhaserPedal depth and feedback create a clearly modulated voice");
        {
            juce::AudioBuffer<float> input(2, (int) (kSampleRate * 1.2));
            for (int ch = 0; ch < input.getNumChannels(); ++ch)
            {
                for (int i = 0; i < input.getNumSamples(); ++i)
                {
                    const float phaseA = (float) (2.0 * juce::MathConstants<double>::pi * 247.0 * (double) i / kSampleRate);
                    const float phaseB = (float) (2.0 * juce::MathConstants<double>::pi * 493.0 * (double) i / kSampleRate);
                    input.setSample(ch, i, 0.11f * std::sin(phaseA) + 0.06f * std::sin(phaseB + ch * 0.17f));
                }
            }

            PhaserPedal subtle;
            subtle.prepareToPlay(kSampleRate, kBlockSize);
            subtle.modeParam->setValueNotifyingHost(normalisedChoiceIndex(subtle.modeParam, 0));
            subtle.rateParam->setValueNotifyingHost(subtle.rateParam->convertTo0to1(0.45f));
            subtle.depthParam->setValueNotifyingHost(subtle.depthParam->convertTo0to1(0.22f));
            subtle.feedbackParam->setValueNotifyingHost(subtle.feedbackParam->convertTo0to1(0.0f));
            subtle.stagesParam->setValueNotifyingHost(subtle.stagesParam->convertTo0to1(4.0f));
            subtle.mixParam->setValueNotifyingHost(subtle.mixParam->convertTo0to1(0.42f));
            subtle.reset();
            const auto subtleOutput = renderPhaserOutput(subtle, input, kBlockSize);

            PhaserPedal deep;
            deep.prepareToPlay(kSampleRate, kBlockSize);
            deep.modeParam->setValueNotifyingHost(normalisedChoiceIndex(deep.modeParam, 2));
            deep.rateParam->setValueNotifyingHost(deep.rateParam->convertTo0to1(1.4f));
            deep.depthParam->setValueNotifyingHost(deep.depthParam->convertTo0to1(0.94f));
            deep.feedbackParam->setValueNotifyingHost(deep.feedbackParam->convertTo0to1(0.68f));
            deep.stagesParam->setValueNotifyingHost(deep.stagesParam->convertTo0to1(10.0f));
            deep.mixParam->setValueNotifyingHost(deep.mixParam->convertTo0to1(0.78f));
            deep.reset();
            const auto deepOutput = renderPhaserOutput(deep, input, kBlockSize);

            expect(computeBufferNullRms(subtleOutput, deepOutput) > 0.022,
                "Deeper phaser settings should audibly reshape the signal");
            expect(computeStereoCorrelation(deepOutput, (int) (kSampleRate * 0.4)) < 0.992,
                "The upgraded phaser should introduce measurable stereo decorrelation");
        }

        beginTest("PhaserPedal modes stay distinct");
        {
            auto renderMode = [&](int modeIndex)
            {
                PhaserPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(modeIndex == 0 ? 0.58f : modeIndex == 1 ? 1.2f : 1.75f));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(modeIndex == 0 ? 0.62f : modeIndex == 1 ? 0.78f : 0.90f));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(modeIndex == 0 ? 0.18f : modeIndex == 1 ? 0.42f : 0.56f));
                pedal.stagesParam->setValueNotifyingHost(pedal.stagesParam->convertTo0to1(modeIndex == 0 ? 4.0f : modeIndex == 1 ? 8.0f : 10.0f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.76f));

                juce::AudioBuffer<float> input(2, (int) (kSampleRate * 1.6));
                input.clear();
                for (int i = 0; i < input.getNumSamples(); ++i)
                {
                    const float phaseA = (float) (2.0 * juce::MathConstants<double>::pi * 247.0 * (double) i / kSampleRate);
                    const float phaseB = (float) (2.0 * juce::MathConstants<double>::pi * 493.0 * (double) i / kSampleRate);
                    input.setSample(0, i, 0.11f * std::sin(phaseA) + 0.05f * std::sin(phaseB));
                    input.setSample(1, i, 0.11f * std::sin(phaseA + 0.12f) + 0.05f * std::sin(phaseB + 0.24f));
                }

                return renderPhaserOutput(pedal, input, kBlockSize);
            };

            const auto vintage = renderMode(0);
            const auto modern = renderMode(1);
            const auto vibe = renderMode(2);

            expect(computeBufferNullRms(vintage, modern) > 0.010,
                "Vintage and modern phaser voicings should not collapse together");
            expect(computeBufferNullRms(vintage, vibe) > 0.018,
                "Vintage and vibe phaser voicings should stay clearly distinct");
            expect(computeBufferNullRms(modern, vibe) > 0.014,
                "Modern and vibe phaser voicings should stay clearly distinct");
            expect(computeStereoCorrelation(vibe, (int) (kSampleRate * 0.35)) < 0.992,
                "Vibe mode should open a wider stereo image");
        }

        beginTest("PhaserPedal automation stress remains finite");
        {
            PhaserPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);

            for (int iteration = 0; iteration < 120; ++iteration)
            {
                const float t = (float) iteration / 119.0f;
                const int mode = juce::jlimit(0, 2, (int) std::floor(t * 3.0f));
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(juce::jmap(std::sin(t * 5.4f), -1.0f, 1.0f, 0.08f, 6.8f)));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(juce::jmap(std::cos(t * 7.0f), -1.0f, 1.0f, 0.05f, 0.98f)));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(juce::jmap(std::sin(t * 8.6f + 0.4f), -1.0f, 1.0f, -0.76f, 0.76f)));
                pedal.stagesParam->setValueNotifyingHost(pedal.stagesParam->convertTo0to1(juce::jmap(std::cos(t * 6.1f + 0.7f), -1.0f, 1.0f, 2.0f, 12.0f)));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(juce::jmap(std::sin(t * 9.9f + 0.3f), -1.0f, 1.0f, 0.0f, 1.0f)));

                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                {
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float sampleIndex = (float) (iteration * kBlockSize + i);
                        block.setSample(ch, i, 0.15f * std::sin((float) (2.0 * juce::MathConstants<double>::pi * 207.0 * sampleIndex / kSampleRate) + ch * 0.15f));
                    }
                }

                pedal.processBlock(block, midi);
                expect(bufferHasOnlyFiniteSamples(block), "Phaser automation should keep every sample finite");
            }
        }

        beginTest("PhaserPedal feedback loop rejects DC accumulation under sustained bias");
        {
            PhaserPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.48f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.92f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.78f));
            pedal.stagesParam->setValueNotifyingHost(pedal.stagesParam->convertTo0to1(10.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            juce::AudioBuffer<float> input(2, (int) (kSampleRate * 3.0));
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) kSampleRate;
                const float sample = 0.08f * std::sin(phase) + 0.12f;
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderPhaserOutput(pedal, input, kBlockSize);
            const double leftDc = std::abs(computeChannelMean(output, 0, (int) (kSampleRate * 1.5), (int) (kSampleRate * 1.0)));
            const double rightDc = std::abs(computeChannelMean(output, 1, (int) (kSampleRate * 1.5), (int) (kSampleRate * 1.0)));

            expect(bufferHasOnlyFiniteSamples(output), "Biased phaser render must remain finite");
            expect(leftDc < 0.01 && rightDc < 0.01, "Phaser feedback protection should keep the late wet signal centered");
        }

        beginTest("Unified Wah round-trips modern and legacy state");
        {
            struct WahLegacyStateHelper : ProcessorBase
            {
                static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
                {
                    copyXmlToBinary(xml, block);
                }

                void prepareToPlay(double, int) override {}
                void releaseResources() override {}
                void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            };

            ClassicWahPedal source;
            source.modeParam->setValueNotifyingHost(normalisedChoiceIndex(source.modeParam, 2));
            source.setExternalControlSource(ClassicWahPedal::ExternalControlSource::ShortcutHold);
            source.setShortcutKeyCode('Q');
            source.sweepParam->setValueNotifyingHost(source.sweepParam->convertTo0to1(0.72f));
            source.sensitivityParam->setValueNotifyingHost(source.sensitivityParam->convertTo0to1(0.63f));
            source.attackParam->setValueNotifyingHost(source.attackParam->convertTo0to1(6.0f));
            source.decayParam->setValueNotifyingHost(source.decayParam->convertTo0to1(280.0f));
            source.rangeParam->setValueNotifyingHost(source.rangeParam->convertTo0to1(0.81f));
            source.resonanceParam->setValueNotifyingHost(source.resonanceParam->convertTo0to1(5.4f));
            source.voiceParam->setValueNotifyingHost(source.voiceParam->convertTo0to1(0.61f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.88f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            ClassicWahPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 2);
            expectEquals((int) restored.getExternalControlSource(),
                (int) ClassicWahPedal::ExternalControlSource::ShortcutHold);
            expectEquals(restored.getShortcutKeyCode(), (int) 'Q');
            expect(approximatelyEqual(restored.sweepParam->get(), 0.72f, 1.0e-3f));
            expect(approximatelyEqual(restored.sensitivityParam->get(), 0.63f, 1.0e-3f));
            expect(approximatelyEqual(restored.attackParam->get(), 6.0f, 0.2f));
            expect(approximatelyEqual(restored.decayParam->get(), 280.0f, 1.0f));
            expect(approximatelyEqual(restored.rangeParam->get(), 0.81f, 1.0e-3f));
            expect(approximatelyEqual(restored.resonanceParam->get(), 5.4f, 0.1f));
            expect(approximatelyEqual(restored.voiceParam->get(), 0.61f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.88f, 1.0e-3f));

            juce::XmlElement legacy("PLUGIN_STATE");
            auto addLegacyParam = [&legacy](const juce::String& id, float value)
            {
                auto* child = legacy.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", value);
            };

            addLegacyParam("wahSweep", 0.55f);
            addLegacyParam("wahSens", source.sensitivityParam->convertTo0to1(0.77f));
            addLegacyParam("wahAttack", source.attackParam->convertTo0to1(4.0f));
            addLegacyParam("wahDecay", source.decayParam->convertTo0to1(190.0f));
            addLegacyParam("wahRange", source.rangeParam->convertTo0to1(0.68f));
            addLegacyParam("wahResonance", juce::jlimit(0.0f, 1.0f, (4.9f - 0.5f) / 9.5f));
            addLegacyParam("wahMix", source.mixParam->convertTo0to1(0.91f));

            juce::MemoryBlock legacyState;
            WahLegacyStateHelper::encode(legacy, legacyState);

            ClassicWahPedal restoredLegacy;
            restoredLegacy.setStateInformation(legacyState.getData(), (int) legacyState.getSize());

            expectEquals(restoredLegacy.modeParam->getIndex(), 1);
            expect(approximatelyEqual(restoredLegacy.sweepParam->get(), 0.55f, 1.0e-3f));
            expect(approximatelyEqual(restoredLegacy.sensitivityParam->get(), 0.77f, 1.0e-3f));
            expect(approximatelyEqual(restoredLegacy.attackParam->get(), 4.0f, 0.2f));
            expect(approximatelyEqual(restoredLegacy.decayParam->get(), 190.0f, 1.0f));
            expect(approximatelyEqual(restoredLegacy.rangeParam->get(), 0.68f, 1.0e-3f));
            expect(approximatelyEqual(restoredLegacy.resonanceParam->get(), 4.9f, 0.12f));
            expect(approximatelyEqual(restoredLegacy.mixParam->get(), 0.91f, 1.0e-3f));

            juce::XmlElement legacyAuto("PLUGIN_STATE");
            auto addLegacyAutoParam = [&legacyAuto](const juce::String& id, float value)
            {
                auto* child = legacyAuto.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", value);
            };

            addLegacyAutoParam("autoWahSens", 0.73f);
            addLegacyAutoParam("autoWahAttack", source.attackParam->convertTo0to1(4.5f));
            addLegacyAutoParam("autoWahRelease", juce::jlimit(0.0f, 1.0f, (260.0f - 15.0f) / (900.0f - 15.0f)));
            addLegacyAutoParam("autoWahRange", 0.84f);
            addLegacyAutoParam("autoWahResonance", juce::jlimit(0.0f, 1.0f, (5.6f - 0.6f) / (9.0f - 0.6f)));
            addLegacyAutoParam("autoWahVoice", 0.58f);
            addLegacyAutoParam("autoWahMix", 0.92f);

            juce::MemoryBlock legacyAutoState;
            WahLegacyStateHelper::encode(legacyAuto, legacyAutoState);

            ClassicWahPedal restoredAutoLegacy;
            restoredAutoLegacy.setStateInformation(legacyAutoState.getData(), (int) legacyAutoState.getSize());

            expectEquals(restoredAutoLegacy.modeParam->getIndex(), 1);
            expect(approximatelyEqual(restoredAutoLegacy.sensitivityParam->get(), 0.73f, 1.0e-3f));
            expect(approximatelyEqual(restoredAutoLegacy.attackParam->get(), 4.5f, 0.2f));
            expect(approximatelyEqual(restoredAutoLegacy.decayParam->get(), 260.0f, 2.0f));
            expect(approximatelyEqual(restoredAutoLegacy.rangeParam->get(), 0.84f, 1.0e-3f));
            expect(approximatelyEqual(restoredAutoLegacy.resonanceParam->get(), 5.6f, 0.12f));
            expect(approximatelyEqual(restoredAutoLegacy.voiceParam->get(), 0.58f, 1.0e-3f));
            expect(approximatelyEqual(restoredAutoLegacy.mixParam->get(), 0.92f, 1.0e-3f));
        }

        beginTest("ReverbPedal round-trips its modern commercial state");
        {
            ReverbPedal source;
            source.modeParam->setValueNotifyingHost(0.8f); // Shimmer
            source.decayParam->setValueNotifyingHost(source.decayParam->convertTo0to1(0.82f));
            source.toneParam->setValueNotifyingHost(source.toneParam->convertTo0to1(0.67f));
            source.sizeParam->setValueNotifyingHost(source.sizeParam->convertTo0to1(0.74f));
            source.dampingParam->setValueNotifyingHost(source.dampingParam->convertTo0to1(0.29f));
            source.bassCutParam->setValueNotifyingHost(source.bassCutParam->convertTo0to1(0.21f));
            source.diffusionParam->setValueNotifyingHost(source.diffusionParam->convertTo0to1(0.88f));
            source.widthParam->setValueNotifyingHost(source.widthParam->convertTo0to1(0.93f));
            source.modParam->setValueNotifyingHost(source.modParam->convertTo0to1(0.44f));
            source.predelayParam->setValueNotifyingHost(source.predelayParam->convertTo0to1(86.0f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.37f));
            source.duckParam->setValueNotifyingHost(source.duckParam->convertTo0to1(0.41f));
            source.swellParam->setValueNotifyingHost(source.swellParam->convertTo0to1(0.63f));
            source.gateParam->setValueNotifyingHost(source.gateParam->convertTo0to1(0.28f));
            source.reverseParam->setValueNotifyingHost(source.reverseParam->convertTo0to1(0.54f));
            source.freezeParam->setValueNotifyingHost(1.0f);

            juce::MemoryBlock state;
            source.getStateInformation(state);

            ReverbPedal restored;
            restored.setStateInformation(state.getData(), (int)state.getSize());

            expectEquals(restored.modeParam->getIndex(), 4);
            expect(approximatelyEqual(restored.decayParam->get(), 0.82f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 0.67f, 1.0e-3f));
            expect(approximatelyEqual(restored.sizeParam->get(), 0.74f, 1.0e-3f));
            expect(approximatelyEqual(restored.diffusionParam->get(), 0.88f, 1.0e-3f));
            expect(approximatelyEqual(restored.widthParam->get(), 0.93f, 1.0e-3f));
            expect(approximatelyEqual(restored.predelayParam->get(), 86.0f, 0.5f));
            expect(approximatelyEqual(restored.duckParam->get(), 0.41f, 1.0e-3f));
            expect(approximatelyEqual(restored.swellParam->get(), 0.63f, 1.0e-3f));
            expect(approximatelyEqual(restored.gateParam->get(), 0.28f, 1.0e-3f));
            expect(approximatelyEqual(restored.reverseParam->get(), 0.54f, 1.0e-3f));
            expect(restored.freezeParam->get(), "Freeze state should round-trip in the modern format");
        }

        beginTest("ReverbPedal maps legacy three-mode state to the correct mode");
        {
            struct LegacyStateHelper : ProcessorBase
            {
                static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
                {
                    copyXmlToBinary(xml, block);
                }

                void prepareToPlay(double, int) override {}
                void releaseResources() override {}
                void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            };

            juce::XmlElement xml("PLUGIN_STATE");
            auto addParam = [&xml](const juce::String& id, float normalised)
            {
                auto* child = xml.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", normalised);
            };

            addParam("reverbMode", 1.0f);       // legacy Hall
            addParam("reverbDecay", 0.40f);
            addParam("reverbTone", 0.75f);
            addParam("reverbPredelay", 0.20f);
            addParam("reverbMix", 0.35f);

            juce::MemoryBlock state;
            LegacyStateHelper::encode(xml, state);

            ReverbPedal restored;
            restored.setStateInformation(state.getData(), (int)state.getSize());

            expectEquals(restored.modeParam->getIndex(), 2);
            expect(approximatelyEqual(restored.toneParam->get(), 0.75f, 1.0e-3f));
        }

        beginTest("ReverbPedal produces a long finite tail that decays cleanly");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(1.0f); // Cloud
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.88f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.72f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.86f));
            pedal.dampingParam->setValueNotifyingHost(pedal.dampingParam->convertTo0to1(0.33f));
            pedal.bassCutParam->setValueNotifyingHost(pedal.bassCutParam->convertTo0to1(0.22f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.92f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(1.0f));
            pedal.modParam->setValueNotifyingHost(pedal.modParam->convertTo0to1(0.42f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(24.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int)(kSampleRate * 6.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            input.setSample(0, 0, 1.0f);
            input.setSample(1, 0, 1.0f);

            const auto output = renderReverbOutput(pedal, input, kBlockSize);
            const double lateRms = computeWindowRms(output, (int)(kSampleRate * 0.8), (int)(kSampleRate * 0.5));
            const double endRms = computeWindowRms(output, totalSamples - (int)(kSampleRate * 0.25), (int)(kSampleRate * 0.25));

            expect(bufferHasOnlyFiniteSamples(output), "Tail render must remain finite across the whole buffer");
            expect(output.getMagnitude(0, 0, output.getNumSamples()) < 1.25f, "Cloud tail should stay inside a sane peak ceiling");
            expect(lateRms > 1.0e-4, "Cloud mode should still carry measurable energy after the initial bloom");
            expect(endRms < lateRms * 0.45, "Tail should decay substantially by the end of the render");
        }

        beginTest("ReverbPedal loop rejects DC accumulation under sustained biased playing");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(1.0f); // Cloud
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.88f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.92f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.94f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(18.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 4.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 174.0f * (float) i / (float) kSampleRate;
                const float sample = 0.10f * std::sin(phase) + 0.10f;
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderReverbOutput(pedal, input, kBlockSize);
            const int lateStart = (int) (kSampleRate * 2.5);
            const int lateLength = (int) (kSampleRate * 1.0);
            const double leftDc = std::abs(computeChannelMean(output, 0, lateStart, lateLength));
            const double rightDc = std::abs(computeChannelMean(output, 1, lateStart, lateLength));

            expect(bufferHasOnlyFiniteSamples(output), "Biased reverb render must remain finite");
            expect(leftDc < 0.01 && rightDc < 0.01, "Reverb loop protection should keep the late wet signal centered");
        }

        beginTest("ReverbPedal generates a decorrelated stereo field");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(0.4f); // Hall
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.74f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.66f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.70f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.84f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(1.0f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(12.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int)(kSampleRate * 2.5);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            input.setSample(0, 0, 1.0f);

            const auto output = renderReverbOutput(pedal, input, kBlockSize);
            const double corr = computeStereoCorrelation(output, (int)(kSampleRate * 0.05));
            const double rmsLeft = computeChannelWindowRms(output, 0, (int)(kSampleRate * 0.1), (int)(kSampleRate * 1.0));
            const double rmsRight = computeChannelWindowRms(output, 1, (int)(kSampleRate * 0.1), (int)(kSampleRate * 1.0));
            const double sideRatio = rmsRight / juce::jmax(1.0e-9, rmsLeft);

            expect(bufferHasOnlyFiniteSamples(output), "Stereo render must remain finite");
            expect(std::abs(corr) < 0.97, "Wet field should not collapse into a near-mono correlation");
            expect(sideRatio > 0.12, "Hall mode should project measurable energy into the opposite channel");
        }

        beginTest("Reverb hero modes render measurably different impulse signatures");
        {
            auto renderMode = [](int modeIndex)
            {
                ReverbPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost((float)modeIndex / 5.0f);
                pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.74f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.66f));
                pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.70f));
                pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.84f));
                pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(1.0f));
                pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(16.0f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

                juce::AudioBuffer<float> input(2, (int)(kSampleRate * 1.6));
                input.clear();
                input.setSample(0, 0, 1.0f);
                input.setSample(1, 0, 1.0f);
                return renderReverbOutput(pedal, input, kBlockSize);
            };

            const auto spring = renderMode(0);
            const auto plate = renderMode(1);
            const auto hall = renderMode(2);

            const double springPlateNull = computeBufferNullRms(spring, plate);
            const double plateHallNull = computeBufferNullRms(plate, hall);
            const double springHallNull = computeBufferNullRms(spring, hall);

            expect(springPlateNull > 8.0e-4, "Spring and Plate should no longer collapse into near-identical tails");
            expect(plateHallNull > 7.0e-4, "Plate and Hall should carry distinct signatures");
            expect(springHallNull > 8.0e-4, "Spring and Hall should remain clearly separated");
        }

        beginTest("ReverbPedal freeze holds a stable ambient pad after capture");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(1.0f); // Cloud
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.86f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.88f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.92f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(1.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int)(kSampleRate * 3.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            input.setSample(0, 0, 1.0f);
            input.setSample(1, 0, 1.0f);

            const auto output = renderReverbOutputWithAutomation(pedal, input, kBlockSize,
                [&pedal](int blockIndex, int, juce::AudioBuffer<float>&)
                {
                    if (blockIndex == 1 && pedal.freezeParam != nullptr)
                        pedal.freezeParam->setValueNotifyingHost(1.0f);
                });

            const double lateRms = computeWindowRms(output, (int)(kSampleRate * 0.8), (int)(kSampleRate * 0.4));
            const double heldRms = computeWindowRms(output, totalSamples - (int)(kSampleRate * 0.4), (int)(kSampleRate * 0.3));

            expect(bufferHasOnlyFiniteSamples(output), "Freeze render must remain finite");
            expect(heldRms > lateRms * 0.60, "Freeze should sustain a significant portion of the captured tail");
        }

        beginTest("ReverbPedal ducking clears space while the source is active");
        {
            ReverbPedal baseline;
            baseline.prepareToPlay(kSampleRate, kBlockSize);
            baseline.modeParam->setValueNotifyingHost(0.4f); // Hall
            baseline.decayParam->setValueNotifyingHost(baseline.decayParam->convertTo0to1(0.72f));
            baseline.diffusionParam->setValueNotifyingHost(baseline.diffusionParam->convertTo0to1(0.86f));
            baseline.mixParam->setValueNotifyingHost(baseline.mixParam->convertTo0to1(1.0f));

            ReverbPedal ducked;
            ducked.prepareToPlay(kSampleRate, kBlockSize);
            ducked.modeParam->setValueNotifyingHost(0.4f); // Hall
            ducked.decayParam->setValueNotifyingHost(ducked.decayParam->convertTo0to1(0.72f));
            ducked.diffusionParam->setValueNotifyingHost(ducked.diffusionParam->convertTo0to1(0.86f));
            ducked.mixParam->setValueNotifyingHost(ducked.mixParam->convertTo0to1(1.0f));
            ducked.duckParam->setValueNotifyingHost(ducked.duckParam->convertTo0to1(0.90f));

            const int totalSamples = (int)(kSampleRate * 1.5);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 220.0f * (float)i / (float)kSampleRate;
                const float sample = 0.22f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto baselineOut = renderReverbOutput(baseline, input, kBlockSize);
            const auto duckedOut = renderReverbOutput(ducked, input, kBlockSize);
            const double baselineRms = computeWindowRms(baselineOut, (int)(kSampleRate * 0.6), (int)(kSampleRate * 0.3));
            const double duckedRms = computeWindowRms(duckedOut, (int)(kSampleRate * 0.6), (int)(kSampleRate * 0.3));

            expect(duckedRms < baselineRms * 0.75, "High ducking should noticeably reduce wet energy under sustained playing");
        }

        beginTest("ReverbPedal swell softens the wet attack and blooms afterward");
        {
            auto renderPedal = [&](float swellAmount)
            {
                ReverbPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(1.0f); // Cloud
                pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.80f));
                pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.84f));
                pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.90f));
                pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(18.0f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(swellAmount));

                const int totalSamples = (int)(kSampleRate * 1.2);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int)(kSampleRate * 0.35);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float)i / (float)kSampleRate;
                    const float sample = 0.20f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderReverbOutput(pedal, input, kBlockSize);
            };

            const auto baselineOut = renderPedal(0.0f);
            const auto swelledOut = renderPedal(0.92f);
            const double baselineEarly = computeWindowRms(baselineOut, (int)(kSampleRate * 0.03), (int)(kSampleRate * 0.11));
            const double swelledEarly = computeWindowRms(swelledOut, (int)(kSampleRate * 0.03), (int)(kSampleRate * 0.11));
            const double baselineBloom = computeWindowRms(baselineOut, (int)(kSampleRate * 0.20), (int)(kSampleRate * 0.22));
            const double swelledBloom = computeWindowRms(swelledOut, (int)(kSampleRate * 0.20), (int)(kSampleRate * 0.22));

            expect(swelledEarly < baselineEarly * 0.72, "High swell should noticeably soften the early wet attack");
            expect(swelledBloom > swelledEarly * 1.45, "High swell should bloom after the initial onset");
            expect(swelledBloom > baselineBloom * 0.45, "Swell should delay the wet body, not erase it");
        }

        beginTest("ReverbPedal gate clamps the tail after the source stops");
        {
            auto renderPedal = [&](float gateAmount)
            {
                ReverbPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(0.4f); // Hall
                pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.76f));
                pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.86f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(gateAmount));

                const int totalSamples = (int)(kSampleRate * 1.6);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int)(kSampleRate * 0.22);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 220.0f * (float)i / (float)kSampleRate;
                    const float sample = 0.22f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderReverbOutput(pedal, input, kBlockSize);
            };

            const auto baselineOut = renderPedal(0.0f);
            const auto gatedOut = renderPedal(0.92f);
            const double baselineBody = computeWindowRms(baselineOut, (int)(kSampleRate * 0.12), (int)(kSampleRate * 0.20));
            const double gatedBody = computeWindowRms(gatedOut, (int)(kSampleRate * 0.12), (int)(kSampleRate * 0.20));
            const double baselineTail = computeWindowRms(baselineOut, (int)(kSampleRate * 0.90), (int)(kSampleRate * 0.25));
            const double gatedTail = computeWindowRms(gatedOut, (int)(kSampleRate * 0.90), (int)(kSampleRate * 0.25));

            expect(gatedBody > baselineBody * 0.55, "Gate should preserve a useful body while the source is active");
            expect(gatedTail < baselineTail * 0.42, "High gate should clamp the late tail decisively");
        }

        beginTest("ReverbPedal reverse ambience pushes the bloom later in time");
        {
            auto renderPedal = [&](float reverseAmount)
            {
                ReverbPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(1.0f); // Cloud
                pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.82f));
                pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.88f));
                pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.92f));
                pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(20.0f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(reverseAmount));

                const int totalSamples = (int)(kSampleRate * 1.4);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int)(kSampleRate * 0.18);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 246.0f * (float)i / (float)kSampleRate;
                    const float sample = 0.20f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderReverbOutput(pedal, input, kBlockSize);
            };

            const auto baselineOut = renderPedal(0.0f);
            const auto reverseOut = renderPedal(0.92f);
            const double baselineEarly = computeWindowRms(baselineOut, (int)(kSampleRate * 0.03), (int)(kSampleRate * 0.14));
            const double reverseEarly = computeWindowRms(reverseOut, (int)(kSampleRate * 0.03), (int)(kSampleRate * 0.14));
            const double baselineLate = computeWindowRms(baselineOut, (int)(kSampleRate * 0.24), (int)(kSampleRate * 0.30));
            const double reverseLate = computeWindowRms(reverseOut, (int)(kSampleRate * 0.24), (int)(kSampleRate * 0.30));

            expect(reverseEarly < baselineEarly * 0.70, "Reverse ambience should suppress more of the early wet attack");
            expect(reverseLate > reverseEarly * 1.60, "Reverse ambience should bloom later than its own onset");
            expect(reverseLate > baselineLate * 0.65, "Reverse ambience should keep enough late energy to stay musical");
        }

        beginTest("ReverbPedal reverse and swell create a delayed cinematic bloom");
        {
            auto renderPedal = [&](float reverseAmount, float swellAmount)
            {
                ReverbPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(1.0f); // Cloud
                pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.84f));
                pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.90f));
                pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.94f));
                pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(24.0f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(reverseAmount));
                pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(swellAmount));

                const int totalSamples = (int)(kSampleRate * 1.5);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int)(kSampleRate * 0.16);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 174.0f * (float)i / (float)kSampleRate;
                    const float sample = 0.22f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderReverbOutput(pedal, input, kBlockSize);
            };

            const auto baselineOut = renderPedal(0.0f, 0.0f);
            const auto comboOut = renderPedal(0.82f, 0.84f);
            const double baselineEarly = computeWindowRms(baselineOut, (int)(kSampleRate * 0.03), (int)(kSampleRate * 0.14));
            const double comboEarly = computeWindowRms(comboOut, (int)(kSampleRate * 0.03), (int)(kSampleRate * 0.14));
            const double baselineLate = computeWindowRms(baselineOut, (int)(kSampleRate * 0.24), (int)(kSampleRate * 0.32));
            const double comboLate = computeWindowRms(comboOut, (int)(kSampleRate * 0.24), (int)(kSampleRate * 0.32));

            expect(comboEarly < baselineEarly * 0.60, "Reverse+swell should strongly soften the early wet onset");
            expect(comboLate > comboEarly * 2.00, "Reverse+swell should bloom substantially later than its onset");
            expect(comboLate > baselineLate * 0.48, "Reverse+swell should still retain a usable late body");
        }

        beginTest("ReverbPedal freeze captures a stable reverse pad");
        {
            auto renderPedal = [&](bool automateFreeze)
            {
                ReverbPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(1.0f); // Cloud
                pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.86f));
                pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.90f));
                pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.94f));
                pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(20.0f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(0.78f));

                const int totalSamples = (int)(kSampleRate * 1.8);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int)(kSampleRate * 0.30);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float)i / (float)kSampleRate;
                    const float sample = 0.20f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderReverbOutputWithAutomation(pedal, input, kBlockSize,
                    [&](int, int offset, juce::AudioBuffer<float>&)
                    {
                        if (automateFreeze && offset >= (int)(kSampleRate * 0.58))
                            pedal.freezeParam->setValueNotifyingHost(1.0f);
                        else
                            pedal.freezeParam->setValueNotifyingHost(0.0f);
                    });
            };

            const auto baselineOut = renderPedal(false);
            const auto frozenOut = renderPedal(true);
            const double captureRms = computeWindowRms(frozenOut, (int)(kSampleRate * 0.78), (int)(kSampleRate * 0.20));
            const double heldRms = computeWindowRms(frozenOut, (int)(kSampleRate * 1.34), (int)(kSampleRate * 0.28));
            const double baselineHeld = computeWindowRms(baselineOut, (int)(kSampleRate * 1.34), (int)(kSampleRate * 0.28));

            expect(bufferHasOnlyFiniteSamples(frozenOut), "Freeze+reverse render should stay finite");
            expect(heldRms > captureRms * 0.55,
                "Freeze should preserve most of the captured reverse pad"
                " (held=" + juce::String(heldRms, 6)
                + " capture=" + juce::String(captureRms, 6)
                + " threshold=" + juce::String(captureRms * 0.55, 6) + ")");
            expect(heldRms > baselineHeld * 2.50,
                "Freeze should hold longer than the unfrozen reverse tail"
                " (held=" + juce::String(heldRms, 6)
                + " baseline=" + juce::String(baselineHeld, 6)
                + " threshold=" + juce::String(baselineHeld * 2.50, 6) + ")");
        }

        beginTest("P7H ReverbPedal configure is not called every block under stable params");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(0.4f); // Hall
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.72f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.64f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.78f));
            pedal.dampingParam->setValueNotifyingHost(pedal.dampingParam->convertTo0to1(0.35f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.86f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.94f));
            pedal.modParam->setValueNotifyingHost(pedal.modParam->convertTo0to1(0.32f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(18.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.42f));
            pedal.resetReverbConfigureDiagnostics();

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            auto fillBlock = [&block](int blockIndex)
            {
                for (int i = 0; i < block.getNumSamples(); ++i)
                {
                    const float t = (float)(blockIndex * block.getNumSamples() + i) / (float)kSampleRate;
                    const float sample = 0.12f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t);
                    block.setSample(0, i, sample);
                    block.setSample(1, i, sample);
                }
            };

            fillBlock(0);
            pedal.processBlock(block, midi);
            const int countAfterFirstBlock = pedal.getReverbConfigureCallCount();

            bool finite = bufferHasOnlyFiniteSamples(block);
            for (int blockIndex = 1; blockIndex <= 96; ++blockIndex)
            {
                fillBlock(blockIndex);
                pedal.processBlock(block, midi);
                finite = finite && bufferHasOnlyFiniteSamples(block);
            }

            expectEquals(countAfterFirstBlock, 1, "First stable Reverb block should perform exactly one initial configure");
            expectEquals(pedal.getReverbConfigureCallCount(), countAfterFirstBlock,
                "Stable Reverb parameters must not reconfigure on every block");
            expect(finite, "Stable Reverb configure-count render must remain finite");
        }

        beginTest("P7H ReverbPedal aggressive automation sweep remains finite and bounded");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.70f));

            const int totalSamples = (int)(kSampleRate * 2.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                const float sample = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 174.0f * t)
                    + 0.06f * std::sin(juce::MathConstants<float>::twoPi * 431.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample * 0.94f);
            }

            const auto output = renderReverbOutputWithAutomation(pedal, input, kBlockSize,
                [&pedal](int blockIndex, int, juce::AudioBuffer<float>&)
                {
                    const float p = (float)blockIndex;
                    pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.18f + 0.78f * std::abs(std::sin(p * 0.173f))));
                    pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.12f + 0.86f * std::abs(std::sin(p * 0.137f + 0.4f))));
                    pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.18f + 0.78f * std::abs(std::sin(p * 0.191f + 1.2f))));
                    pedal.dampingParam->setValueNotifyingHost(pedal.dampingParam->convertTo0to1(0.10f + 0.84f * std::abs(std::sin(p * 0.157f + 0.8f))));
                    pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.20f + 0.78f * std::abs(std::sin(p * 0.113f + 2.1f))));
                    pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.18f + 0.80f * std::abs(std::sin(p * 0.149f + 0.6f))));
                    pedal.modParam->setValueNotifyingHost(pedal.modParam->convertTo0to1(0.04f + 0.86f * std::abs(std::sin(p * 0.181f + 1.7f))));
                    pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(2.0f + 220.0f * std::abs(std::sin(p * 0.097f + 0.2f))));
                    pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.20f + 0.72f * std::abs(std::sin(p * 0.071f + 0.9f))));
                });

            const double peak = computeBufferPeak(output, 0, output.getNumSamples());
            const double lateDcL = std::abs(computeChannelMean(output, 0, (int)(kSampleRate * 1.2), (int)(kSampleRate * 0.5)));
            const double lateDcR = std::abs(computeChannelMean(output, 1, (int)(kSampleRate * 1.2), (int)(kSampleRate * 0.5)));
            const double lateRms = computeWindowRms(output, (int)(kSampleRate * 1.2), (int)(kSampleRate * 0.5));

            expect(bufferHasOnlyFiniteSamples(output), "P7H aggressive Reverb automation must remain finite");
            expect(peak < 3.0, "P7H aggressive Reverb automation should stay inside a sane peak ceiling");
            expect(lateRms < 0.80, "P7H aggressive Reverb automation should not produce sustained near-clip energy");
            expect(lateDcL < 0.02 && lateDcR < 0.02, "P7H aggressive Reverb automation should not accumulate DC");
        }

        beginTest("P7H ReverbPedal mode changes remain bounded");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.82f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.88f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.90f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.78f));

            const int totalSamples = (int)(kSampleRate * 2.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                const float env = i < (int)(kSampleRate * 1.2) ? 1.0f : 0.0f;
                const float sample = env * 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderReverbOutputWithAutomation(pedal, input, kBlockSize,
                [&pedal](int blockIndex, int, juce::AudioBuffer<float>&)
                {
                    if ((blockIndex % 7) == 0)
                    {
                        const int modeIndex = (blockIndex / 7) % 6;
                        pedal.modeParam->setValueNotifyingHost(modeIndex / 5.0f);
                    }
                });

            const double peak = computeBufferPeak(output, 0, output.getNumSamples());
            const double tailRms = computeWindowRms(output, (int)(kSampleRate * 1.55), (int)(kSampleRate * 0.35));

            expect(bufferHasOnlyFiniteSamples(output), "P7H Reverb mode-change automation must remain finite");
            expect(peak < 3.0, "P7H Reverb mode-change automation should stay inside a sane peak ceiling");
            expect(tailRms < 0.75, "P7H Reverb mode-change automation should not leave runaway tail energy");
        }

        beginTest("P7H ReverbPedal freeze gate reverse swell automation remains bounded");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(1.0f); // Cloud
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.88f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.90f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.94f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.82f));

            const int totalSamples = (int)(kSampleRate * 2.4);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float)i / (float)kSampleRate;
                const float env = (i % (int)(kSampleRate * 0.42)) < (int)(kSampleRate * 0.20) ? 1.0f : 0.25f;
                const float sample = env * 0.16f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample * 0.97f);
            }

            const auto output = renderReverbOutputWithAutomation(pedal, input, kBlockSize,
                [&pedal](int blockIndex, int, juce::AudioBuffer<float>&)
                {
                    const float p = (float)blockIndex;
                    pedal.duckParam->setValueNotifyingHost(pedal.duckParam->convertTo0to1(0.92f * std::abs(std::sin(p * 0.083f))));
                    pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(0.95f * std::abs(std::sin(p * 0.071f + 0.5f))));
                    pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(0.90f * std::abs(std::sin(p * 0.061f + 1.1f))));
                    pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(0.88f * std::abs(std::sin(p * 0.097f + 0.2f))));
                    pedal.freezeParam->setValueNotifyingHost((blockIndex % 41) > 22 ? 1.0f : 0.0f);
                });

            const double peak = computeBufferPeak(output, 0, output.getNumSamples());
            const double lateRms = computeWindowRms(output, (int)(kSampleRate * 1.7), (int)(kSampleRate * 0.5));

            expect(bufferHasOnlyFiniteSamples(output), "P7H Reverb performance automation must remain finite");
            expect(peak < 3.0, "P7H Reverb performance automation should stay inside a sane peak ceiling");
            expect(lateRms < 0.95, "P7H Reverb performance automation should not create runaway sustained energy");
        }

        beginTest("DelayPedal round-trips its flagship state");
        {
            DelayPedal source;
            source.modeParam->setValueNotifyingHost(normalisedChoiceIndex(source.modeParam, 2));
            source.timeParam->setValueNotifyingHost(source.timeParam->convertTo0to1(742.0f));
            source.feedbackParam->setValueNotifyingHost(source.feedbackParam->convertTo0to1(0.78f));
            source.toneParam->setValueNotifyingHost(source.toneParam->convertTo0to1(9200.0f));
            source.spreadParam->setValueNotifyingHost(source.spreadParam->convertTo0to1(0.88f));
            source.textureParam->setValueNotifyingHost(source.textureParam->convertTo0to1(0.64f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.41f));
            source.duckParam->setValueNotifyingHost(source.duckParam->convertTo0to1(0.36f));
            source.swellParam->setValueNotifyingHost(source.swellParam->convertTo0to1(0.58f));
            source.reverseParam->setValueNotifyingHost(source.reverseParam->convertTo0to1(0.44f));
            source.freezeParam->setValueNotifyingHost(1.0f);

            juce::MemoryBlock state;
            source.getStateInformation(state);

            DelayPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 2);
            expect(approximatelyEqual(restored.timeParam->get(), 742.0f, 0.5f));
            expect(approximatelyEqual(restored.feedbackParam->get(), 0.78f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 9200.0f, 1.0f));
            expect(approximatelyEqual(restored.spreadParam->get(), 0.88f, 1.0e-3f));
            expect(approximatelyEqual(restored.textureParam->get(), 0.64f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.41f, 1.0e-3f));
            expect(approximatelyEqual(restored.duckParam->get(), 0.36f, 1.0e-3f));
            expect(approximatelyEqual(restored.swellParam->get(), 0.58f, 1.0e-3f));
            expect(approximatelyEqual(restored.reverseParam->get(), 0.44f, 1.0e-3f));
            expect(restored.freezeParam->get(), "Freeze should round-trip with the modern delay state");
        }

        beginTest("DelayPedal produces a long finite tail that decays cleanly");
        {
            DelayPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(620.0f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.84f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(4800.0f));
            pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.76f));
            pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.82f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 5.5);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            input.setSample(0, 0, 1.0f);
            input.setSample(1, 0, 1.0f);

            const auto output = renderDelayOutput(pedal, input, kBlockSize);
            const double lateRms = computeWindowRms(output, (int) (kSampleRate * 1.0), (int) (kSampleRate * 0.9));
            const double endRms = computeWindowRms(output, totalSamples - (int) (kSampleRate * 0.35), (int) (kSampleRate * 0.3));

            expect(bufferHasOnlyFiniteSamples(output), "Delay tail render must remain finite");
            expect(output.getMagnitude(0, 0, output.getNumSamples()) < 1.35f, "Tape delay should remain inside a sane peak ceiling");
            expect(lateRms > 1.0e-4, "Tape delay should still carry measurable energy well past the first repeat");
            expect(endRms < lateRms * 0.60, "Delay tail should decay substantially by the end of the render");
        }

        beginTest("DelayPedal generates a decorrelated stereo field");
        {
            DelayPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(340.0f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.68f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(10400.0f));
            pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.96f));
            pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.30f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 2.4);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            input.setSample(0, 0, 1.0f);

            const auto output = renderDelayOutput(pedal, input, kBlockSize);
            const double corr = computeStereoCorrelation(output, (int) (kSampleRate * 0.08));
            const double rmsLeft = computeChannelWindowRms(output, 0, (int) (kSampleRate * 0.08), (int) (kSampleRate * 1.1));
            const double rmsRight = computeChannelWindowRms(output, 1, (int) (kSampleRate * 0.08), (int) (kSampleRate * 1.1));
            const double sideRatio = rmsRight / juce::jmax(1.0e-9, rmsLeft);

            expect(bufferHasOnlyFiniteSamples(output), "Stereo delay render must stay finite");
            expect(std::abs(corr) < 0.95, "Spread-rich digital delay should not collapse into near mono");
            expect(sideRatio > 0.18, "Digital delay should project clear repeat energy into the opposite channel");
        }

        beginTest("Delay hero modes render measurably different impulse signatures");
        {
            auto renderMode = [&](int modeIndex)
            {
                DelayPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
                pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(480.0f));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.74f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(6200.0f));
                pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.78f));
                pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.62f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(modeIndex == 3 ? 0.70f : 0.0f));

                juce::AudioBuffer<float> input(2, (int) (kSampleRate * 2.2));
                input.clear();
                input.setSample(0, 0, 1.0f);
                input.setSample(1, 0, 1.0f);
                return renderDelayOutput(pedal, input, kBlockSize);
            };

            const auto analog = renderMode(0);
            const auto tape = renderMode(1);
            const auto digital = renderMode(2);
            const auto reverse = renderMode(3);

            const double analogTapeNull = computeBufferNullRms(analog, tape);
            const double tapeDigitalNull = computeBufferNullRms(tape, digital);
            const double digitalReverseNull = computeBufferNullRms(digital, reverse);

            expect(analogTapeNull > 9.0e-4, "Analog and Tape should no longer collapse into near-identical repeats");
            expect(tapeDigitalNull > 8.0e-4, "Tape and Digital should carry distinct repeat signatures");
            expect(digitalReverseNull > 9.0e-4, "Digital and Reverse should remain clearly separated");
        }

        beginTest("DelayPedal freeze holds a stable captured repeat bed");
        {
            auto renderPedal = [&](bool automateFreeze)
            {
                DelayPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
                pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(540.0f));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.82f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(5200.0f));
                pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.72f));
                pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.76f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

                const int totalSamples = (int) (kSampleRate * 2.0);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int) (kSampleRate * 0.32);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.20f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderDelayOutputWithAutomation(pedal, input, kBlockSize,
                    [&](int, int offset, juce::AudioBuffer<float>&)
                    {
                        pedal.freezeParam->setValueNotifyingHost(automateFreeze && offset >= (int) (kSampleRate * 0.62) ? 1.0f : 0.0f);
                    });
            };

            const auto baselineOut = renderPedal(false);
            const auto frozenOut = renderPedal(true);
            const double captureRms = computeWindowRms(frozenOut, (int) (kSampleRate * 0.78), (int) (kSampleRate * 0.22));
            const double heldRms = computeWindowRms(frozenOut, (int) (kSampleRate * 1.45), (int) (kSampleRate * 0.26));
            const double baselineHeld = computeWindowRms(baselineOut, (int) (kSampleRate * 1.45), (int) (kSampleRate * 0.26));

            expect(bufferHasOnlyFiniteSamples(frozenOut), "Freeze render must remain finite");
            expect(heldRms > captureRms * 0.50,
                "Freeze should retain a significant part of the captured repeat bed"
                " (held=" + juce::String(heldRms, 6)
                + " capture=" + juce::String(captureRms, 6)
                + " threshold=" + juce::String(captureRms * 0.50, 6) + ")");
            expect(heldRms > baselineHeld * 2.0,
                "Freeze should outlast the unfrozen tail decisively"
                " (held=" + juce::String(heldRms, 6)
                + " baseline=" + juce::String(baselineHeld, 6)
                + " threshold=" + juce::String(baselineHeld * 2.0, 6) + ")");
        }

        beginTest("DelayPedal ducking clears space while the source is active");
        {
            auto configure = [](DelayPedal& pedal, float duckAmount)
            {
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
                pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(410.0f));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.70f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(9600.0f));
                pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.74f));
                pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.28f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.duckParam->setValueNotifyingHost(pedal.duckParam->convertTo0to1(duckAmount));
            };

            DelayPedal baseline;
            configure(baseline, 0.0f);
            DelayPedal ducked;
            configure(ducked, 0.90f);

            const int totalSamples = (int) (kSampleRate * 1.6);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) kSampleRate;
                const float sample = 0.24f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto baselineOut = renderDelayOutput(baseline, input, kBlockSize);
            const auto duckedOut = renderDelayOutput(ducked, input, kBlockSize);
            const double baselineRms = computeWindowRms(baselineOut, (int) (kSampleRate * 0.65), (int) (kSampleRate * 0.35));
            const double duckedRms = computeWindowRms(duckedOut, (int) (kSampleRate * 0.65), (int) (kSampleRate * 0.35));

            expect(duckedRms < baselineRms * 0.78, "High ducking should noticeably reduce wet energy under sustained playing");
        }

        beginTest("DelayPedal reverse pushes the bloom later in time");
        {
            auto renderPedal = [&](float reverseAmount)
            {
                DelayPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
                pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(440.0f));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.72f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(9400.0f));
                pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.82f));
                pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.48f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(reverseAmount));

                const int totalSamples = (int) (kSampleRate * 1.8);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int) (kSampleRate * 0.18);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 246.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.20f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderDelayOutput(pedal, input, kBlockSize);
            };

            const auto baselineOut = renderPedal(0.0f);
            const auto reverseOut = renderPedal(0.92f);
            const double baselineEarly = computeWindowRms(baselineOut, (int) (kSampleRate * 0.40), (int) (kSampleRate * 0.14));
            const double reverseEarly = computeWindowRms(reverseOut, (int) (kSampleRate * 0.40), (int) (kSampleRate * 0.14));
            const double baselineLate = computeWindowRms(baselineOut, (int) (kSampleRate * 0.54), (int) (kSampleRate * 0.22));
            const double reverseLate = computeWindowRms(reverseOut, (int) (kSampleRate * 0.54), (int) (kSampleRate * 0.22));

            expect(reverseEarly < baselineEarly * 0.90, "Reverse should suppress more of the early repeat body");
            expect(reverseLate > reverseEarly * 0.68, "Reverse should retain a useful later body instead of collapsing");
            expect(reverseLate > baselineLate * 0.70, "Reverse should keep enough late energy to stay musical");
        }

        beginTest("DelayPedal swell softens the first repeat and blooms afterward");
        {
            auto renderPedal = [&](float swellAmount)
            {
                DelayPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 0));
                pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(390.0f));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.68f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(5200.0f));
                pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.62f));
                pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.66f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(swellAmount));

                const int totalSamples = (int) (kSampleRate * 1.5);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int) (kSampleRate * 0.24);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 174.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.22f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderDelayOutput(pedal, input, kBlockSize);
            };

            const auto baselineOut = renderPedal(0.0f);
            const auto swelledOut = renderPedal(0.92f);
            const double baselineEarly = computeWindowRms(baselineOut, (int) (kSampleRate * 0.34), (int) (kSampleRate * 0.14));
            const double swelledEarly = computeWindowRms(swelledOut, (int) (kSampleRate * 0.34), (int) (kSampleRate * 0.14));
            const double baselineBloom = computeWindowRms(baselineOut, (int) (kSampleRate * 0.48), (int) (kSampleRate * 0.18));
            const double swelledBloom = computeWindowRms(swelledOut, (int) (kSampleRate * 0.48), (int) (kSampleRate * 0.18));

            expect(swelledEarly < baselineEarly * 0.88, "High swell should noticeably soften the early repeat onset");
            expect(swelledBloom > swelledEarly * 1.05, "High swell should bloom after the initial onset");
            expect(swelledBloom > baselineBloom * 0.70, "Swell should delay the body, not erase it");
        }

        beginTest("DelayPedal reverse and swell create a delayed ambient wash");
        {
            auto renderPedal = [&](float reverseAmount, float swellAmount)
            {
                DelayPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
                pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(460.0f));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.72f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(9000.0f));
                pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.84f));
                pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.48f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(reverseAmount));
                pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(swellAmount));

                const int totalSamples = (int) (kSampleRate * 1.9);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int) (kSampleRate * 0.16);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.20f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderDelayOutput(pedal, input, kBlockSize);
            };

            const auto baselineOut = renderPedal(0.0f, 0.0f);
            const auto comboOut = renderPedal(0.82f, 0.86f);
            const double baselineEarly = computeWindowRms(baselineOut, (int) (kSampleRate * 0.40), (int) (kSampleRate * 0.14));
            const double comboEarly = computeWindowRms(comboOut, (int) (kSampleRate * 0.40), (int) (kSampleRate * 0.14));
            const double baselineLate = computeWindowRms(baselineOut, (int) (kSampleRate * 0.58), (int) (kSampleRate * 0.24));
            const double comboLate = computeWindowRms(comboOut, (int) (kSampleRate * 0.58), (int) (kSampleRate * 0.24));

            expect(comboEarly < baselineEarly * 0.82,
                "Reverse+swell should clearly soften the early delay onset"
                " (comboEarly=" + juce::String(comboEarly, 6)
                + " baselineEarly=" + juce::String(baselineEarly, 6)
                + " threshold=" + juce::String(baselineEarly * 0.82, 6) + ")");
            expect(comboLate > comboEarly * 0.52,
                "Reverse+swell should keep a meaningful later ambient body"
                " (comboLate=" + juce::String(comboLate, 6)
                + " comboEarly=" + juce::String(comboEarly, 6)
                + " threshold=" + juce::String(comboEarly * 0.52, 6) + ")");
            expect(comboLate > baselineLate * 0.70,
                "Reverse+swell should still retain a commercially usable late body"
                " (comboLate=" + juce::String(comboLate, 6)
                + " baselineLate=" + juce::String(baselineLate, 6)
                + " threshold=" + juce::String(baselineLate * 0.70, 6) + ")");
        }

        beginTest("DelayPedal automation stress remains finite under aggressive changes");
        {
            DelayPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.62f));

            juce::Random rng(0xD31A9);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            bool finite = true;
            double peak = 0.0;

            const int blocksToRun = (int) ((kSampleRate * 3.2) / (double) kBlockSize);
            for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlockSize; ++i)
                        block.setSample(ch, i, 0.16f * ((rng.nextFloat() * 2.0f) - 1.0f));

                const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                const int mode = juce::jlimit(0, 3, (int) std::floor(phase * 4.0f));

                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(120.0f + 1900.0f * phase));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.18f + 0.72f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(1800.0f + 10000.0f * (1.0f - phase)));
                pedal.spreadParam->setValueNotifyingHost(pedal.spreadParam->convertTo0to1(0.10f + 0.90f * phase));
                pedal.textureParam->setValueNotifyingHost(pedal.textureParam->convertTo0to1(0.15f + 0.80f * std::abs(std::cos(phase * juce::MathConstants<float>::twoPi))));
                pedal.duckParam->setValueNotifyingHost(pedal.duckParam->convertTo0to1(0.85f * (1.0f - phase)));
                pedal.swellParam->setValueNotifyingHost(pedal.swellParam->convertTo0to1(0.80f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                pedal.reverseParam->setValueNotifyingHost(pedal.reverseParam->convertTo0to1(0.75f * phase));
                pedal.freezeParam->setValueNotifyingHost((blockIndex % 181) == 0 ? 1.0f : 0.0f);

                pedal.processBlock(block, midi);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
                finite = finite && bufferHasOnlyFiniteSamples(block);
            }

            expect(finite, "Aggressive delay automation must remain finite");
            expect(peak < 2.1, "Automation stress should stay inside a sane peak ceiling");
        }

        beginTest("ChorusPedal round-trips its commercial state");
        {
            ChorusPedal source;
            source.modeParam->setValueNotifyingHost(normalisedChoiceIndex(source.modeParam, 1));
            source.rateParam->setValueNotifyingHost(source.rateParam->convertTo0to1(1.85f));
            source.depthParam->setValueNotifyingHost(source.depthParam->convertTo0to1(0.74f));
            source.widthParam->setValueNotifyingHost(source.widthParam->convertTo0to1(0.88f));
            source.toneParam->setValueNotifyingHost(source.toneParam->convertTo0to1(0.46f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.57f));
            source.lagParam->setValueNotifyingHost(source.lagParam->convertTo0to1(11.6f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            ChorusPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 1);
            expect(approximatelyEqual(restored.rateParam->get(), 1.85f, 1.0e-3f));
            expect(approximatelyEqual(restored.depthParam->get(), 0.74f, 1.0e-3f));
            expect(approximatelyEqual(restored.widthParam->get(), 0.88f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 0.46f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.57f, 1.0e-3f));
            expect(approximatelyEqual(restored.lagParam->get(), 11.6f, 1.0e-3f));
        }

        beginTest("ChorusPedal mix zero keeps the dry path transparent");
        {
            ChorusPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 0));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(1.35f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.84f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.92f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.74f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.0f));
            pedal.lagParam->setValueNotifyingHost(pedal.lagParam->convertTo0to1(9.6f));

            const int totalSamples = (int) (kSampleRate * 1.2);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phaseL = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) kSampleRate;
                const float phaseR = juce::MathConstants<float>::twoPi * 247.0f * (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.18f * std::sin(phaseL));
                input.setSample(1, i, 0.14f * std::sin(phaseR));
            }

            const auto output = renderChorusOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Dry-only chorus render must stay finite");
            expect(nullRms < 1.0e-5, "Mix at zero should leave the dry path effectively untouched");
        }

        beginTest("ChorusPedal modes produce distinct stereo signatures");
        {
            auto renderMode = [&](int modeIndex)
            {
                ChorusPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(modeIndex == 2 ? 2.25f : 1.10f));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(modeIndex == 2 ? 0.86f : 0.72f));
                pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.94f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.63f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.lagParam->setValueNotifyingHost(pedal.lagParam->convertTo0to1(modeIndex == 1 ? 11.8f : 8.4f));

                const int totalSamples = (int) (kSampleRate * 2.1);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.18f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderChorusOutput(pedal, input, kBlockSize);
            };

            const auto classic = renderMode(0);
            const auto ensemble = renderMode(1);
            const auto vibrato = renderMode(2);

            const double ensembleCorr = computeStereoCorrelation(ensemble, (int) (kSampleRate * 0.25));
            const double ensembleRightRms = computeChannelWindowRms(ensemble, 1, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.9));
            const double ensembleLeftRms = computeChannelWindowRms(ensemble, 0, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.9));
            const double classicEnsembleNull = computeBufferNullRms(classic, ensemble);
            const double classicVibratoNull = computeBufferNullRms(classic, vibrato);

            expect(bufferHasOnlyFiniteSamples(ensemble), "Ensemble chorus render must remain finite");
            expect(std::abs(ensembleCorr) < 0.97, "Ensemble mode should open the stereo field beyond near-mono correlation");
            expect(ensembleRightRms > ensembleLeftRms * 0.30, "Ensemble mode should project substantial energy into the opposite side");
            expect(classicEnsembleNull > 2.5e-3, "Classic and Ensemble modes should render clearly different modulation signatures");
            expect(classicVibratoNull > 2.0e-3, "Classic and Vibrato modes should remain clearly separated");
        }

        beginTest("ChorusPedal automation stress remains finite under aggressive changes");
        {
            ChorusPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.58f));

            juce::Random rng(0xC4015);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            bool finite = true;
            double peak = 0.0;

            const int blocksToRun = (int) ((kSampleRate * 3.0) / (double) kBlockSize);
            for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlockSize; ++i)
                        block.setSample(ch, i, 0.15f * ((rng.nextFloat() * 2.0f) - 1.0f));

                const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                const int mode = juce::jlimit(0, 2, (int) std::floor(phase * 3.0f));

                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.08f + 7.4f * phase));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.05f + 0.93f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.05f + 0.95f * (1.0f - phase)));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.12f + 0.82f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.10f + 0.85f * phase));
                pedal.lagParam->setValueNotifyingHost(pedal.lagParam->convertTo0to1(2.4f + 15.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));

                pedal.processBlock(block, midi);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
                finite = finite && bufferHasOnlyFiniteSamples(block);
            }

            expect(finite, "Aggressive chorus automation must remain finite");
            expect(peak < 1.8, "Chorus automation stress should stay inside a sane peak ceiling");
        }

        beginTest("FlangerPedal round-trips its commercial state");
        {
            FlangerPedal source;
            source.modeParam->setValueNotifyingHost(normalisedChoiceIndex(source.modeParam, 2));
            source.rateParam->setValueNotifyingHost(source.rateParam->convertTo0to1(0.91f));
            source.depthParam->setValueNotifyingHost(source.depthParam->convertTo0to1(0.83f));
            source.manualParam->setValueNotifyingHost(source.manualParam->convertTo0to1(0.58f));
            source.feedbackParam->setValueNotifyingHost(source.feedbackParam->convertTo0to1(-0.36f));
            source.widthParam->setValueNotifyingHost(source.widthParam->convertTo0to1(0.79f));
            source.toneParam->setValueNotifyingHost(source.toneParam->convertTo0to1(6800.0f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.53f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            FlangerPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 2);
            expect(approximatelyEqual(restored.rateParam->get(), 0.91f, 1.0e-3f));
            expect(approximatelyEqual(restored.depthParam->get(), 0.83f, 1.0e-3f));
            expect(approximatelyEqual(restored.manualParam->get(), 0.58f, 1.0e-3f));
            expect(approximatelyEqual(restored.feedbackParam->get(), -0.36f, 1.0e-3f));
            expect(approximatelyEqual(restored.widthParam->get(), 0.79f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 6800.0f, 1.0e-2f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.53f, 1.0e-3f));
        }

        beginTest("FlangerPedal mix zero keeps the dry path transparent");
        {
            FlangerPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 0));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.72f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.78f));
            pedal.manualParam->setValueNotifyingHost(pedal.manualParam->convertTo0to1(0.44f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.62f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.88f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(8600.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.0f));
            pedal.reset();

            const int totalSamples = (int) (kSampleRate * 1.2);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phaseL = juce::MathConstants<float>::twoPi * 173.0f * (float) i / (float) kSampleRate;
                const float phaseR = juce::MathConstants<float>::twoPi * 233.0f * (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.18f * std::sin(phaseL));
                input.setSample(1, i, 0.15f * std::sin(phaseR));
            }

            const auto output = renderFlangerOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Dry-only flanger render must stay finite");
            expect(nullRms < 1.0e-5, "Mix at zero should leave the dry path effectively untouched");
        }

        beginTest("FlangerPedal modes produce distinct stereo signatures");
        {
            auto renderMode = [&](int modeIndex)
            {
                FlangerPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(modeIndex == 1 ? 0.54f : 0.86f));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(modeIndex == 2 ? 0.90f : 0.80f));
                pedal.manualParam->setValueNotifyingHost(pedal.manualParam->convertTo0to1(modeIndex == 2 ? 0.40f : 0.52f));
                pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(modeIndex == 2 ? -0.58f : 0.64f));
                pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.92f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(7600.0f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.reset();

                const int totalSamples = (int) (kSampleRate * 2.0);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.18f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderFlangerOutput(pedal, input, kBlockSize);
            };

            const auto classic = renderMode(0);
            const auto jet = renderMode(1);
            const auto zero = renderMode(2);

            const double jetCorr = computeStereoCorrelation(jet, (int) (kSampleRate * 0.18));
            const double jetRightRms = computeChannelWindowRms(jet, 1, (int) (kSampleRate * 0.18), (int) (kSampleRate * 0.9));
            const double jetLeftRms = computeChannelWindowRms(jet, 0, (int) (kSampleRate * 0.18), (int) (kSampleRate * 0.9));
            const double classicJetNull = computeBufferNullRms(classic, jet);
            const double classicZeroNull = computeBufferNullRms(classic, zero);
            const double jetZeroNull = computeBufferNullRms(jet, zero);

            expect(bufferHasOnlyFiniteSamples(jet), "Jet flanger render must remain finite");
            expect(std::abs(jetCorr) < 0.985, "Jet mode should open a measurable stereo field");
            expect(jetRightRms > jetLeftRms * 0.25, "Jet mode should project meaningful energy into the opposite side");
            expect(classicJetNull > 1.5e-3, "Classic and Jet modes should render audibly different comb signatures");
            expect(classicZeroNull > 1.5e-3, "Classic and Zero modes should remain clearly separated");
            expect(jetZeroNull > 1.5e-3, "Jet and Zero modes should not collapse into the same modulation voice");
        }

        beginTest("FlangerPedal automation stress remains finite under aggressive changes");
        {
            FlangerPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.56f));

            const int totalSamples = (int) (kSampleRate * 3.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.14f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                    + 0.06f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t));
                input.setSample(1, i, 0.13f * std::sin(juce::MathConstants<float>::twoPi * 146.0f * t)
                    + 0.05f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * t));
            }

            const auto output = renderFlangerOutputWithAutomation(pedal, input, kBlockSize,
                [&](int blockIndex, int, juce::AudioBuffer<float>&)
                {
                    const int totalBlocks = juce::jmax(1, totalSamples / kBlockSize);
                    const float phase = (float) blockIndex / (float) juce::jmax(1, totalBlocks - 1);
                    const int mode = juce::jlimit(0, 2, (int) std::floor(phase * 3.0f));

                    pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                    pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.04f + 5.2f * phase));
                    pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.08f + 0.90f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                    pedal.manualParam->setValueNotifyingHost(pedal.manualParam->convertTo0to1(0.06f + 0.88f * (1.0f - phase)));
                    pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(-0.78f + 1.56f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.5f))));
                    pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.04f + 0.96f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                    pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(1400.0f + 11200.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                    pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.08f + 0.86f * phase));
                });

            expect(bufferHasOnlyFiniteSamples(output), "Aggressive flanger automation must remain finite");
            expect(output.getMagnitude(0, 0, output.getNumSamples()) < 1.9f, "Flanger automation should stay inside a sane peak ceiling");
            expect(output.getMagnitude(1, 0, output.getNumSamples()) < 1.9f, "Flanger automation should stay inside a sane peak ceiling on both channels");
        }

        beginTest("TremoloPedal round-trips its studio state");
        {
            TremoloPedal source;
            source.rateParam->setValueNotifyingHost(source.rateParam->convertTo0to1(6.8f));
            source.depthParam->setValueNotifyingHost(source.depthParam->convertTo0to1(0.84f));
            source.shapeParam->setValueNotifyingHost(source.shapeParam->convertTo0to1(0.78f));
            source.biasParam->setValueNotifyingHost(source.biasParam->convertTo0to1(0.68f));
            source.stereoParam->setValueNotifyingHost(source.stereoParam->convertTo0to1(0.72f));
            source.harmonicParam->setValueNotifyingHost(source.harmonicParam->convertTo0to1(0.57f));
            source.crossoverParam->setValueNotifyingHost(source.crossoverParam->convertTo0to1(1325.0f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.64f));
            source.levelParam->setValueNotifyingHost(source.levelParam->convertTo0to1(1.22f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            TremoloPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.rateParam->get(), 6.8f, 1.0e-3f));
            expect(approximatelyEqual(restored.depthParam->get(), 0.84f, 1.0e-3f));
            expect(approximatelyEqual(restored.shapeParam->get(), 0.78f, 1.0e-3f));
            expect(approximatelyEqual(restored.biasParam->get(), 0.68f, 1.0e-3f));
            expect(approximatelyEqual(restored.stereoParam->get(), 0.72f, 1.0e-3f));
            expect(approximatelyEqual(restored.harmonicParam->get(), 0.57f, 1.0e-3f));
            expect(approximatelyEqual(restored.crossoverParam->get(), 1325.0f, 1.0e-2f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.64f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 1.22f, 1.0e-3f));
        }

        beginTest("TremoloPedal restores legacy six-control state with studio defaults");
        {
            struct TremoloLegacyStateHelper : ProcessorBase
            {
                static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
                {
                    copyXmlToBinary(xml, block);
                }

                void prepareToPlay(double, int) override {}
                void releaseResources() override {}
                void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            };

            juce::XmlElement xml("PLUGIN_STATE");
            auto addParam = [&xml](const juce::String& id, float value)
            {
                auto* child = xml.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", value);
            };

            addParam("tremoloRate", (7.2f - 0.5f) / 14.5f);
            addParam("tremoloDepth", 0.82f);
            addParam("tremoloShape", 0.74f);
            addParam("tremoloStereo", 0.61f);
            addParam("tremoloHarmonic", 0.37f);
            addParam("tremoloLevel", (1.18f - 0.5f) / 1.5f);

            juce::MemoryBlock state;
            TremoloLegacyStateHelper::encode(xml, state);

            TremoloPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.rateParam->get(), 7.2f, 1.0e-3f));
            expect(approximatelyEqual(restored.depthParam->get(), 0.82f, 1.0e-3f));
            expect(approximatelyEqual(restored.shapeParam->get(), 0.74f, 1.0e-3f));
            expect(approximatelyEqual(restored.stereoParam->get(), 0.61f, 1.0e-3f));
            expect(approximatelyEqual(restored.harmonicParam->get(), 0.37f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 1.18f, 1.0e-3f));
            expect(approximatelyEqual(restored.biasParam->get(), 0.50f, 1.0e-3f));
            expect(approximatelyEqual(restored.crossoverParam->get(), 800.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 1.0f, 1.0e-3f));
        }

        beginTest("TremoloPedal mix zero keeps the dry path transparent");
        {
            TremoloPedal pedal;
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(7.4f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(1.0f));
            pedal.shapeParam->setValueNotifyingHost(pedal.shapeParam->convertTo0to1(0.95f));
            pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.28f));
            pedal.stereoParam->setValueNotifyingHost(pedal.stereoParam->convertTo0to1(1.0f));
            pedal.harmonicParam->setValueNotifyingHost(pedal.harmonicParam->convertTo0to1(0.82f));
            pedal.crossoverParam->setValueNotifyingHost(pedal.crossoverParam->convertTo0to1(1180.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.0f));

            const int totalSamples = (int) (kSampleRate * 1.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.18f * std::sin(juce::MathConstants<float>::twoPi * 173.0f * t));
                input.setSample(1, i, 0.15f * std::sin(juce::MathConstants<float>::twoPi * 229.0f * t));
            }

            const auto output = renderTremoloOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Dry-only tremolo render must stay finite");
            expect(nullRms < 1.0e-6, "Mix at zero should leave the dry path effectively untouched");
        }

        beginTest("TremoloPedal stereo width opens the field");
        {
            auto renderStereoField = [&](float stereoAmount)
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
                pedal.prepareToPlay(kSampleRate, kBlockSize);

                const int totalSamples = (int) (kSampleRate * 1.8);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    const float sample = 0.19f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderTremoloOutput(pedal, input, kBlockSize);
            };

            const auto centered = renderStereoField(0.0f);
            const auto widened = renderStereoField(1.0f);
            const double centeredCorr = computeStereoCorrelation(centered, (int) (kSampleRate * 0.20));
            const double widenedCorr = computeStereoCorrelation(widened, (int) (kSampleRate * 0.20));

            expect(centeredCorr > 0.995, "Stereo at zero should keep both channels tightly aligned");
            expect(widenedCorr < 0.82, "Stereo width should noticeably decorrelate the two channels");
            expect((centeredCorr - widenedCorr) > 0.12, "Stereo control should materially widen the modulation field");
        }

        beginTest("TremoloPedal harmonic crossover changes the modulation focus");
        {
            auto renderCrossoverFocus = [&](float crossoverHz)
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
                pedal.prepareToPlay(kSampleRate, kBlockSize);

                const int totalSamples = (int) (kSampleRate * 0.9);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    const float sample = 0.20f * std::sin(juce::MathConstants<float>::twoPi * 800.0f * t);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderTremoloOutput(pedal, input, kBlockSize);
            };

            const auto lowSplit = renderCrossoverFocus(350.0f);
            const auto highSplit = renderCrossoverFocus(1600.0f);
            const double lowSplitWindow = computeWindowRms(lowSplit, (int) (kSampleRate * 0.055), (int) (kSampleRate * 0.035));
            const double highSplitWindow = computeWindowRms(highSplit, (int) (kSampleRate * 0.055), (int) (kSampleRate * 0.035));
            const double splitNull = computeBufferNullRms(lowSplit, highSplit);

            expect(highSplitWindow > lowSplitWindow * 1.4, "Moving the crossover above the note should flip it into the stronger harmonic phase");
            expect(splitNull > 0.02, "Harmonic crossover should create audibly different modulation voicings");
        }

        beginTest("TremoloPedal bias reshapes the chop contour");
        {
            auto renderBiasShape = [&](float bias)
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
                pedal.prepareToPlay(kSampleRate, kBlockSize);

                const int totalSamples = (int) (kSampleRate * 1.1);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    const float sample = 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderTremoloOutput(pedal, input, kBlockSize);
            };

            const auto earlyBias = renderBiasShape(0.18f);
            const auto lateBias = renderBiasShape(0.82f);
            const double contourNull = computeBufferNullRms(earlyBias, lateBias);

            expect(contourNull > 0.025, "Bias should materially reshape the tremolo contour instead of acting like a cosmetic trim");
        }

        beginTest("TremoloPedal automation stress remains finite under aggressive changes");
        {
            TremoloPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            const int totalSamples = (int) (kSampleRate * 3.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.17f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                    + 0.05f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t));
                input.setSample(1, i, 0.14f * std::sin(juce::MathConstants<float>::twoPi * 147.0f * t)
                    + 0.04f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * t));
            }

            juce::MidiBuffer midi;
            bool finite = true;
            double peak = 0.0;
            const int totalBlocks = juce::jmax(1, totalSamples / kBlockSize);

            for (int offset = 0, blockIndex = 0; offset < totalSamples; offset += kBlockSize, ++blockIndex)
            {
                const int numSamples = juce::jmin(kBlockSize, totalSamples - offset);
                juce::AudioBuffer<float> block(2, kBlockSize);
                block.clear();
                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom(ch, 0, input, ch, offset, numSamples);

                const float phase = (float) blockIndex / (float) juce::jmax(1, totalBlocks - 1);
                pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.6f + 11.8f * phase));
                pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.05f + 0.95f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.shapeParam->setValueNotifyingHost(pedal.shapeParam->convertTo0to1(0.05f + 0.95f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.2f))));
                pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.10f + 0.80f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.5f))));
                pedal.stereoParam->setValueNotifyingHost(pedal.stereoParam->convertTo0to1(std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.harmonicParam->setValueNotifyingHost(pedal.harmonicParam->convertTo0to1(std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.8f))));
                pedal.crossoverParam->setValueNotifyingHost(pedal.crossoverParam->convertTo0to1(280.0f + 1600.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.1f))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.35f))));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.70f + 0.85f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.4f))));

                pedal.processBlock(block, midi);
                finite = finite && bufferHasOnlyFiniteSamples(block);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
            }

            expect(finite, "Aggressive tremolo automation must remain finite");
            expect(peak < 2.0, "Tremolo automation should stay inside a sane peak ceiling");
        }

        beginTest("EQPedal round-trips its studio state");
        {
            EQPedal source;
            source.lowCutParam->setValueNotifyingHost(source.lowCutParam->convertTo0to1(85.0f));
            source.lowParam->setValueNotifyingHost(source.lowParam->convertTo0to1(3.5f));
            source.lowMidParam->setValueNotifyingHost(source.lowMidParam->convertTo0to1(-4.2f));
            source.midParam->setValueNotifyingHost(source.midParam->convertTo0to1(5.8f));
            source.midFreqParam->setValueNotifyingHost(source.midFreqParam->convertTo0to1(1450.0f));
            source.midQParam->setValueNotifyingHost(source.midQParam->convertTo0to1(1.6f));
            source.presenceParam->setValueNotifyingHost(source.presenceParam->convertTo0to1(2.2f));
            source.highParam->setValueNotifyingHost(source.highParam->convertTo0to1(-1.5f));
            source.highCutParam->setValueNotifyingHost(source.highCutParam->convertTo0to1(7200.0f));
            source.levelParam->setValueNotifyingHost(source.levelParam->convertTo0to1(1.25f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            EQPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.lowCutParam->get(), 85.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.lowParam->get(), 3.5f, 1.0e-3f));
            expect(approximatelyEqual(restored.lowMidParam->get(), -4.2f, 1.0e-3f));
            expect(approximatelyEqual(restored.midParam->get(), 5.8f, 1.0e-3f));
            expect(approximatelyEqual(restored.midFreqParam->get(), 1450.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.midQParam->get(), 1.6f, 1.0e-3f));
            expect(approximatelyEqual(restored.presenceParam->get(), 2.2f, 1.0e-3f));
            expect(approximatelyEqual(restored.highParam->get(), -1.5f, 1.0e-3f));
            expect(approximatelyEqual(restored.highCutParam->get(), 7200.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 1.25f, 1.0e-3f));
        }

        beginTest("EQPedal restores legacy seven-control state with studio defaults");
        {
            struct EQLegacyStateHelper : ProcessorBase
            {
                static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
                {
                    copyXmlToBinary(xml, block);
                }

                void prepareToPlay(double, int) override {}
                void releaseResources() override {}
                void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            };

            juce::XmlElement xml("PLUGIN_STATE");
            auto addParam = [&xml](const juce::String& id, float value)
            {
                auto* child = xml.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", value);
            };

            addParam("eqLow", (2.4f + 12.0f) / 24.0f);
            addParam("eqLowMid", (-3.0f + 12.0f) / 24.0f);
            addParam("eqMid", (4.5f + 12.0f) / 24.0f);
            addParam("eqMidFreq", (1800.0f - 200.0f) / 4800.0f);
            addParam("eqPresence", (-2.2f + 12.0f) / 24.0f);
            addParam("eqHigh", (5.5f + 12.0f) / 24.0f);
            addParam("eqLevel", 1.35f / 2.0f);

            juce::MemoryBlock state;
            EQLegacyStateHelper::encode(xml, state);

            EQPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.lowParam->get(), 2.4f, 1.0e-3f));
            expect(approximatelyEqual(restored.lowMidParam->get(), -3.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.midParam->get(), 4.5f, 1.0e-3f));
            expect(approximatelyEqual(restored.midFreqParam->get(), 1800.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.presenceParam->get(), -2.2f, 1.0e-3f));
            expect(approximatelyEqual(restored.highParam->get(), 5.5f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 1.35f, 1.0e-3f));
            expect(approximatelyEqual(restored.lowCutParam->get(), 20.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.midQParam->get(), 1.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.highCutParam->get(), 20000.0f, 1.0e-3f));
        }

        beginTest("EQPedal default settings stay transparent");
        {
            EQPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            const int totalSamples = (int) (kSampleRate * 0.9);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.18f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                    + 0.06f * std::sin(juce::MathConstants<float>::twoPi * 900.0f * t)
                    + 0.03f * std::sin(juce::MathConstants<float>::twoPi * 4200.0f * t));
                input.setSample(1, i, 0.14f * std::sin(juce::MathConstants<float>::twoPi * 164.0f * t)
                    + 0.05f * std::sin(juce::MathConstants<float>::twoPi * 1300.0f * t)
                    + 0.02f * std::sin(juce::MathConstants<float>::twoPi * 6100.0f * t));
            }

            const auto output = renderEQOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Default EQ render must remain finite");
            expect(nullRms < 2.0e-6, "Flat EQ settings should leave the signal effectively untouched");
        }

        beginTest("EQPedal low cut rejects rumble while preserving the note body");
        {
            auto renderLowCut = [&](float lowCutHz)
            {
                EQPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.lowCutParam->setValueNotifyingHost(pedal.lowCutParam->convertTo0to1(lowCutHz));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));

                const int totalSamples = (int) (kSampleRate * 1.1);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    float sample = 0.0f;
                    if (i >= (int) (kSampleRate * 0.04) && i < (int) (kSampleRate * 0.34))
                        sample += 0.17f * std::sin(juce::MathConstants<float>::twoPi * 45.0f * t);
                    if (i >= (int) (kSampleRate * 0.42) && i < (int) (kSampleRate * 0.96))
                    {
                        sample += 0.18f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                        sample += 0.05f * std::sin(juce::MathConstants<float>::twoPi * 45.0f * t);
                    }

                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderEQOutput(pedal, input, kBlockSize);
            };

            const auto open = renderLowCut(20.0f);
            const auto cleaned = renderLowCut(140.0f);
            const double openRumble = computeWindowRms(open, (int) (kSampleRate * 0.10), (int) (kSampleRate * 0.16));
            const double cleanedRumble = computeWindowRms(cleaned, (int) (kSampleRate * 0.10), (int) (kSampleRate * 0.16));
            const double openBody = computeWindowRms(open, (int) (kSampleRate * 0.54), (int) (kSampleRate * 0.22));
            const double cleanedBody = computeWindowRms(cleaned, (int) (kSampleRate * 0.54), (int) (kSampleRate * 0.22));

            expect(cleanedRumble < openRumble * 0.45, "Low cut should decisively trim sub-heavy rumble");
            expect(cleanedBody > openBody * 0.72, "Low cut should keep the useful note body intact");
        }

        beginTest("EQPedal high cut tames fizz without collapsing the core tone");
        {
            auto renderHighCut = [&](float highCutHz)
            {
                EQPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.highCutParam->setValueNotifyingHost(pedal.highCutParam->convertTo0to1(highCutHz));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));

                const int totalSamples = (int) (kSampleRate * 1.0);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    float sample = 0.0f;
                    if (i >= (int) (kSampleRate * 0.05) && i < (int) (kSampleRate * 0.30))
                        sample += 0.14f * std::sin(juce::MathConstants<float>::twoPi * 7000.0f * t);
                    if (i >= (int) (kSampleRate * 0.38) && i < (int) (kSampleRate * 0.88))
                    {
                        sample += 0.18f * std::sin(juce::MathConstants<float>::twoPi * 240.0f * t);
                        sample += 0.06f * std::sin(juce::MathConstants<float>::twoPi * 7000.0f * t);
                    }

                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderEQOutput(pedal, input, kBlockSize);
            };

            const auto open = renderHighCut(20000.0f);
            const auto softened = renderHighCut(4200.0f);
            const double openFizz = computeWindowRms(open, (int) (kSampleRate * 0.10), (int) (kSampleRate * 0.14));
            const double softenedFizz = computeWindowRms(softened, (int) (kSampleRate * 0.10), (int) (kSampleRate * 0.14));
            const double openBody = computeWindowRms(open, (int) (kSampleRate * 0.52), (int) (kSampleRate * 0.20));
            const double softenedBody = computeWindowRms(softened, (int) (kSampleRate * 0.52), (int) (kSampleRate * 0.20));

            expect(softenedFizz < openFizz * 0.38, "High cut should strongly reduce fizzy top-end energy");
            expect(softenedBody > openBody * 0.75, "High cut should preserve the main body of the tone");
        }

        beginTest("EQPedal mid sweep targets the selected frequency region");
        {
            auto renderMidFocus = [&](float midFreqHz)
            {
                EQPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.midParam->setValueNotifyingHost(pedal.midParam->convertTo0to1(10.0f));
                pedal.midFreqParam->setValueNotifyingHost(pedal.midFreqParam->convertTo0to1(midFreqHz));
                pedal.midQParam->setValueNotifyingHost(pedal.midQParam->convertTo0to1(1.7f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));

                const int totalSamples = (int) (kSampleRate * 0.8);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    const float sample = 0.16f * std::sin(juce::MathConstants<float>::twoPi * 850.0f * t);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderEQOutput(pedal, input, kBlockSize);
            };

            const auto matched = renderMidFocus(850.0f);
            const auto detuned = renderMidFocus(2500.0f);
            const double matchedRms = computeWindowRms(matched, (int) (kSampleRate * 0.18), (int) (kSampleRate * 0.24));
            const double detunedRms = computeWindowRms(detuned, (int) (kSampleRate * 0.18), (int) (kSampleRate * 0.24));

            expect(matchedRms > detunedRms * 1.8, "Sweeping the mid band should materially change where the boost lands");
        }

        beginTest("EQPedal automation stress remains finite under aggressive changes");
        {
            EQPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            const int totalSamples = (int) (kSampleRate * 2.8);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.16f * std::sin(juce::MathConstants<float>::twoPi * 98.0f * t)
                    + 0.05f * std::sin(juce::MathConstants<float>::twoPi * 820.0f * t)
                    + 0.03f * std::sin(juce::MathConstants<float>::twoPi * 5200.0f * t));
                input.setSample(1, i, 0.13f * std::sin(juce::MathConstants<float>::twoPi * 147.0f * t)
                    + 0.04f * std::sin(juce::MathConstants<float>::twoPi * 1100.0f * t)
                    + 0.025f * std::sin(juce::MathConstants<float>::twoPi * 6400.0f * t));
            }

            juce::MidiBuffer midi;
            bool finite = true;
            double peak = 0.0;
            const int totalBlocks = juce::jmax(1, totalSamples / kBlockSize);

            for (int offset = 0, blockIndex = 0; offset < totalSamples; offset += kBlockSize, ++blockIndex)
            {
                const int numSamples = juce::jmin(kBlockSize, totalSamples - offset);
                juce::AudioBuffer<float> block(2, kBlockSize);
                block.clear();
                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom(ch, 0, input, ch, offset, numSamples);

                const float phase = (float) blockIndex / (float) juce::jmax(1, totalBlocks - 1);
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

                pedal.processBlock(block, midi);
                finite = finite && bufferHasOnlyFiniteSamples(block);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
            }

            expect(finite, "Aggressive EQ automation must remain finite");
            expect(peak < 3.0, "EQ automation should stay inside a sane peak ceiling");
        }

        beginTest("CompressorPedal round-trips its studio state");
        {
            CompressorPedal source;
            source.thresholdParam->setValueNotifyingHost(source.thresholdParam->convertTo0to1(-18.5f));
            source.ratioParam->setValueNotifyingHost(source.ratioParam->convertTo0to1(6.4f));
            source.attackParam->setValueNotifyingHost(source.attackParam->convertTo0to1(7.5f));
            source.releaseParam->setValueNotifyingHost(source.releaseParam->convertTo0to1(165.0f));
            source.kneeParam->setValueNotifyingHost(source.kneeParam->convertTo0to1(8.2f));
            source.focusParam->setValueNotifyingHost(source.focusParam->convertTo0to1(0.68f));
            source.blendParam->setValueNotifyingHost(source.blendParam->convertTo0to1(0.91f));
            source.makeupParam->setValueNotifyingHost(source.makeupParam->convertTo0to1(4.4f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            CompressorPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.thresholdParam->get(), -18.5f, 1.0e-3f));
            expect(approximatelyEqual(restored.ratioParam->get(), 6.4f, 1.0e-3f));
            expect(approximatelyEqual(restored.attackParam->get(), 7.5f, 1.0e-3f));
            expect(approximatelyEqual(restored.releaseParam->get(), 165.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.kneeParam->get(), 8.2f, 1.0e-3f));
            expect(approximatelyEqual(restored.focusParam->get(), 0.68f, 1.0e-3f));
            expect(approximatelyEqual(restored.blendParam->get(), 0.91f, 1.0e-3f));
            expect(approximatelyEqual(restored.makeupParam->get(), 4.4f, 1.0e-3f));
        }

        beginTest("CompressorPedal restores legacy six-control state with studio defaults");
        {
            struct CompressorLegacyStateHelper : ProcessorBase
            {
                static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
                {
                    copyXmlToBinary(xml, block);
                }

                void prepareToPlay(double, int) override {}
                void releaseResources() override {}
                void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            };

            juce::XmlElement xml("PLUGIN_STATE");
            auto addParam = [&xml](const juce::String& id, float value)
            {
                auto* child = xml.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", value);
            };

            addParam("compThreshold", (-24.0f + 42.0f) / 42.0f);
            addParam("compRatio", (5.6f - 1.0f) / 11.0f);
            addParam("compAttack", (9.0f - 1.0f) / 79.0f);
            addParam("compRelease", (185.0f - 25.0f) / 325.0f);
            addParam("compBlend", 0.72f);
            addParam("compMakeup", 3.8f / 18.0f);

            juce::MemoryBlock state;
            CompressorLegacyStateHelper::encode(xml, state);

            CompressorPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.thresholdParam->get(), -24.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.ratioParam->get(), 5.6f, 1.0e-3f));
            expect(approximatelyEqual(restored.attackParam->get(), 9.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.releaseParam->get(), 185.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.blendParam->get(), 0.72f, 1.0e-3f));
            expect(approximatelyEqual(restored.makeupParam->get(), 3.8f, 1.0e-3f));
            expect(approximatelyEqual(restored.kneeParam->get(), 6.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.focusParam->get(), 0.55f, 1.0e-3f));
        }

        beginTest("CompressorPedal blend zero keeps the dry path transparent");
        {
            CompressorPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-36.0f));
            pedal.ratioParam->setValueNotifyingHost(pedal.ratioParam->convertTo0to1(10.0f));
            pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(2.0f));
            pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(250.0f));
            pedal.kneeParam->setValueNotifyingHost(pedal.kneeParam->convertTo0to1(10.0f));
            pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(1.0f));
            pedal.blendParam->setValueNotifyingHost(pedal.blendParam->convertTo0to1(0.0f));
            pedal.makeupParam->setValueNotifyingHost(pedal.makeupParam->convertTo0to1(12.0f));

            const int totalSamples = (int) (kSampleRate * 0.8);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                const float left = 0.28f * std::sin(juce::MathConstants<float>::twoPi * 117.0f * t)
                    + 0.07f * std::sin(juce::MathConstants<float>::twoPi * 1850.0f * t);
                const float right = 0.21f * std::sin(juce::MathConstants<float>::twoPi * 173.0f * t)
                    + 0.06f * std::sin(juce::MathConstants<float>::twoPi * 2410.0f * t);
                input.setSample(0, i, left);
                input.setSample(1, i, right);
            }

            const auto output = renderCompressorOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Dry-only compressor render must remain finite");
            expect(nullRms < 1.0e-6, "Blend at zero should leave the dry signal untouched");
        }

        beginTest("CompressorPedal applies audible gain reduction to strong sustained material");
        {
            CompressorPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-30.0f));
            pedal.ratioParam->setValueNotifyingHost(pedal.ratioParam->convertTo0to1(8.0f));
            pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(5.0f));
            pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(145.0f));
            pedal.kneeParam->setValueNotifyingHost(pedal.kneeParam->convertTo0to1(7.5f));
            pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.6f));
            pedal.blendParam->setValueNotifyingHost(pedal.blendParam->convertTo0to1(1.0f));
            pedal.makeupParam->setValueNotifyingHost(pedal.makeupParam->convertTo0to1(0.0f));

            const int totalSamples = (int) (kSampleRate * 1.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                float sample = 0.0f;
                if (i >= (int) (kSampleRate * 0.04) && i < (int) (kSampleRate * 0.82))
                {
                    sample += 0.31f * std::sin(juce::MathConstants<float>::twoPi * 130.0f * t);
                    sample += 0.14f * std::sin(juce::MathConstants<float>::twoPi * 910.0f * t);
                }

                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderCompressorOutput(pedal, input, kBlockSize);
            const double inputBody = computeWindowRms(input, (int) (kSampleRate * 0.18), (int) (kSampleRate * 0.32));
            const double outputBody = computeWindowRms(output, (int) (kSampleRate * 0.18), (int) (kSampleRate * 0.32));

            expect(bufferHasOnlyFiniteSamples(output), "Compressed render must remain finite");
            expect(outputBody < inputBody * 0.72, "Strong sustained material should receive decisive gain reduction");
        }

        beginTest("CompressorPedal focus rejects low-end pumping more effectively");
        {
            auto renderFocus = [&](float focusAmount)
            {
                CompressorPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-31.0f));
                pedal.ratioParam->setValueNotifyingHost(pedal.ratioParam->convertTo0to1(7.0f));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(6.0f));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(180.0f));
                pedal.kneeParam->setValueNotifyingHost(pedal.kneeParam->convertTo0to1(8.0f));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(focusAmount));
                pedal.blendParam->setValueNotifyingHost(pedal.blendParam->convertTo0to1(1.0f));
                pedal.makeupParam->setValueNotifyingHost(pedal.makeupParam->convertTo0to1(0.0f));

                const int totalSamples = (int) (kSampleRate * 1.3);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();

                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    float sample = 0.0f;
                    if (i >= (int) (kSampleRate * 0.08) && i < (int) (kSampleRate * 0.78))
                    {
                        sample += 0.24f * std::sin(juce::MathConstants<float>::twoPi * 55.0f * t);
                        sample += 0.11f * std::sin(juce::MathConstants<float>::twoPi * 1700.0f * t);
                    }

                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderCompressorOutput(pedal, input, kBlockSize);
            };

            const auto loose = renderFocus(0.0f);
            const auto focused = renderFocus(1.0f);
            const double looseBody = computeWindowRms(loose, (int) (kSampleRate * 0.22), (int) (kSampleRate * 0.24));
            const double focusedBody = computeWindowRms(focused, (int) (kSampleRate * 0.22), (int) (kSampleRate * 0.24));

            expect(focusedBody > looseBody * 1.16, "A focused sidechain should preserve more body when heavy low end is present");
        }

        beginTest("CompressorPedal linked stereo detection compresses both channels from a one-sided transient");
        {
            auto renderLinked = [&](bool includeLeftTransient)
            {
                CompressorPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-29.0f));
                pedal.ratioParam->setValueNotifyingHost(pedal.ratioParam->convertTo0to1(9.0f));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(4.0f));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(150.0f));
                pedal.kneeParam->setValueNotifyingHost(pedal.kneeParam->convertTo0to1(7.0f));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.55f));
                pedal.blendParam->setValueNotifyingHost(pedal.blendParam->convertTo0to1(1.0f));
                pedal.makeupParam->setValueNotifyingHost(pedal.makeupParam->convertTo0to1(0.0f));

                const int totalSamples = (int) (kSampleRate * 1.0);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    float left = 0.0f;
                    const float right = 0.095f * std::sin(juce::MathConstants<float>::twoPi * 430.0f * t);

                    if (includeLeftTransient
                        && i >= (int) (kSampleRate * 0.22)
                        && i < (int) (kSampleRate * 0.40))
                    {
                        left = 0.30f * std::sin(juce::MathConstants<float>::twoPi * 120.0f * t);
                    }

                    input.setSample(0, i, left);
                    input.setSample(1, i, right);
                }

                return renderCompressorOutput(pedal, input, kBlockSize);
            };

            const auto isolated = renderLinked(false);
            const auto linked = renderLinked(true);
            const double isolatedRight = computeChannelWindowRms(isolated, 1, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.14));
            const double linkedRight = computeChannelWindowRms(linked, 1, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.14));

            expect(linkedRight < isolatedRight * 0.78, "A transient on one side should trigger linked gain reduction on the other side");
        }

        beginTest("CompressorPedal automation stress remains finite under aggressive changes");
        {
            CompressorPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            const int totalSamples = (int) (kSampleRate * 2.8);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.18f * std::sin(juce::MathConstants<float>::twoPi * 103.0f * t)
                    + 0.04f * std::sin(juce::MathConstants<float>::twoPi * 1560.0f * t));
                input.setSample(1, i, 0.14f * std::sin(juce::MathConstants<float>::twoPi * 151.0f * t)
                    + 0.035f * std::sin(juce::MathConstants<float>::twoPi * 2140.0f * t));
            }

            juce::MidiBuffer midi;
            bool finite = true;
            double peak = 0.0;
            const int totalBlocks = juce::jmax(1, totalSamples / kBlockSize);

            for (int offset = 0, blockIndex = 0; offset < totalSamples; offset += kBlockSize, ++blockIndex)
            {
                const int numSamples = juce::jmin(kBlockSize, totalSamples - offset);
                juce::AudioBuffer<float> block(2, kBlockSize);
                block.clear();
                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom(ch, 0, input, ch, offset, numSamples);

                const float phase = (float) blockIndex / (float) juce::jmax(1, totalBlocks - 1);
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-40.0f + 26.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                pedal.ratioParam->setValueNotifyingHost(pedal.ratioParam->convertTo0to1(1.2f + 9.8f * std::abs(std::cos(phase * juce::MathConstants<float>::twoPi))));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(1.0f + 65.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.3f))));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(25.0f + 300.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 0.85f))));
                pedal.kneeParam->setValueNotifyingHost(pedal.kneeParam->convertTo0to1(1.0f + 11.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.7f))));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.5f))));
                pedal.blendParam->setValueNotifyingHost(pedal.blendParam->convertTo0to1(std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.1f))));
                pedal.makeupParam->setValueNotifyingHost(pedal.makeupParam->convertTo0to1(12.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 0.95f))));

                pedal.processBlock(block, midi);
                finite = finite && bufferHasOnlyFiniteSamples(block);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
            }

            expect(finite, "Aggressive compressor automation must remain finite");
            expect(peak < 2.0, "Compressor automation should stay inside a sane peak ceiling");
        }

        beginTest("NoiseGatePedal round-trips its studio state");
        {
            NoiseGatePedal source;
            source.thresholdParam->setValueNotifyingHost(source.thresholdParam->convertTo0to1(-42.5f));
            source.attackParam->setValueNotifyingHost(source.attackParam->convertTo0to1(0.18f));
            source.holdParam->setValueNotifyingHost(source.holdParam->convertTo0to1(96.0f));
            source.releaseParam->setValueNotifyingHost(source.releaseParam->convertTo0to1(180.0f));
            source.rangeParam->setValueNotifyingHost(source.rangeParam->convertTo0to1(-72.0f));
            source.hysteresisParam->setValueNotifyingHost(source.hysteresisParam->convertTo0to1(9.4f));
            source.focusParam->setValueNotifyingHost(source.focusParam->convertTo0to1(0.74f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            NoiseGatePedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.thresholdParam->get(), -42.5f, 1.0e-3f));
            expect(approximatelyEqual(restored.attackParam->get(), 0.18f, 1.0e-3f));
            expect(approximatelyEqual(restored.holdParam->get(), 96.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.releaseParam->get(), 180.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.rangeParam->get(), -72.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.hysteresisParam->get(), 9.4f, 1.0e-3f));
            expect(approximatelyEqual(restored.focusParam->get(), 0.74f, 1.0e-3f));
        }

        beginTest("NoiseGatePedal restores legacy five-control state with modern defaults");
        {
            struct NoiseGateLegacyStateHelper : ProcessorBase
            {
                static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
                {
                    copyXmlToBinary(xml, block);
                }

                void prepareToPlay(double, int) override {}
                void releaseResources() override {}
                void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            };

            juce::XmlElement xml("PLUGIN_STATE");
            auto addParam = [&xml](const juce::String& id, float value)
            {
                auto* child = xml.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", value);
            };

            addParam("gateThreshold", (-49.0f + 80.0f) / 80.0f);
            addParam("gateAttack", (0.42f - 0.02f) / 24.98f);
            addParam("gateHold", 132.0f / 400.0f);
            addParam("gateRelease", (210.0f - 10.0f) / 690.0f);
            addParam("gateRange", (-58.0f + 96.0f) / 96.0f);

            juce::MemoryBlock state;
            NoiseGateLegacyStateHelper::encode(xml, state);

            NoiseGatePedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expect(approximatelyEqual(restored.thresholdParam->get(), -49.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.attackParam->get(), 0.42f, 1.5e-2f));
            expect(approximatelyEqual(restored.holdParam->get(), 132.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.releaseParam->get(), 210.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.rangeParam->get(), -58.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.hysteresisParam->get(), 8.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.focusParam->get(), 0.55f, 1.0e-3f));
        }

        beginTest("NoiseGatePedal clamps late noise while preserving the played body");
        {
            auto renderGate = [&](float rangeDb)
            {
                NoiseGatePedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-48.0f));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(0.12f));
                pedal.holdParam->setValueNotifyingHost(pedal.holdParam->convertTo0to1(88.0f));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(130.0f));
                pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(rangeDb));
                pedal.hysteresisParam->setValueNotifyingHost(pedal.hysteresisParam->convertTo0to1(8.8f));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.58f));

                const int totalSamples = (int) (kSampleRate * 1.6);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();

                const int activeSamples = (int) (kSampleRate * 0.26);
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
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

                return renderNoiseGateOutput(pedal, input, kBlockSize);
            };

            const auto baseline = renderGate(0.0f);
            const auto gated = renderGate(-96.0f);
            const double baselineBody = computeWindowRms(baseline, (int) (kSampleRate * 0.05), (int) (kSampleRate * 0.18));
            const double gatedBody = computeWindowRms(gated, (int) (kSampleRate * 0.05), (int) (kSampleRate * 0.18));
            const double baselineTail = computeWindowRms(baseline, (int) (kSampleRate * 0.95), (int) (kSampleRate * 0.28));
            const double gatedTail = computeWindowRms(gated, (int) (kSampleRate * 0.95), (int) (kSampleRate * 0.28));

            expect(bufferHasOnlyFiniteSamples(gated), "Noise gate render must remain finite");
            expect(gatedBody > baselineBody * 0.74, "The played body should stay intact while the source is active");
            expect(gatedTail < baselineTail * 0.18, "The late tail should clamp decisively once the note stops");
        }

        beginTest("NoiseGatePedal focus rejects rumble without erasing the picked note");
        {
            auto renderFocus = [&](float focusAmount)
            {
                NoiseGatePedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-46.5f));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(0.10f));
                pedal.holdParam->setValueNotifyingHost(pedal.holdParam->convertTo0to1(52.0f));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(120.0f));
                pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(-96.0f));
                pedal.hysteresisParam->setValueNotifyingHost(pedal.hysteresisParam->convertTo0to1(9.0f));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(focusAmount));

                const int totalSamples = (int) (kSampleRate * 1.45);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();

                const int burstStart = (int) (kSampleRate * 0.08);
                const int burstEnd = (int) (kSampleRate * 0.24);
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    float sample = 0.010f * std::sin(juce::MathConstants<float>::twoPi * 55.0f * t);

                    if (i >= burstStart && i < burstEnd)
                    {
                        sample += 0.11f * std::sin(juce::MathConstants<float>::twoPi * 1650.0f * t);
                        sample += 0.04f * std::sin(juce::MathConstants<float>::twoPi * 3300.0f * t);
                    }

                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderNoiseGateOutput(pedal, input, kBlockSize);
            };

            const auto loose = renderFocus(0.05f);
            const auto focused = renderFocus(0.95f);
            const double looseBurst = computeWindowRms(loose, (int) (kSampleRate * 0.11), (int) (kSampleRate * 0.10));
            const double focusedBurst = computeWindowRms(focused, (int) (kSampleRate * 0.11), (int) (kSampleRate * 0.10));
            const double looseTail = computeWindowRms(loose, (int) (kSampleRate * 0.88), (int) (kSampleRate * 0.24));
            const double focusedTail = computeWindowRms(focused, (int) (kSampleRate * 0.88), (int) (kSampleRate * 0.24));

            expect(focusedBurst > looseBurst * 0.62, "Focused detection should keep enough of the picked body");
            expect(focusedTail < looseTail * 0.45, "Focused detection should reject more low-frequency rumble at idle");
        }

        beginTest("NoiseGatePedal linked stereo detection opens both channels from a single-sided transient");
        {
            auto renderLinked = [&](bool includeLeftTransient)
            {
                NoiseGatePedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-44.0f));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(0.08f));
                pedal.holdParam->setValueNotifyingHost(pedal.holdParam->convertTo0to1(70.0f));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(135.0f));
                pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(-96.0f));
                pedal.hysteresisParam->setValueNotifyingHost(pedal.hysteresisParam->convertTo0to1(8.5f));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(0.60f));

                const int totalSamples = (int) (kSampleRate * 1.2);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();

                const int transientStart = (int) (kSampleRate * 0.22);
                const int transientEnd = transientStart + (int) (kSampleRate * 0.18);
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    const float rightSample = (i >= transientStart && i < transientEnd)
                        ? 0.0035f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t)
                        : 0.0f;
                    const float leftSample = includeLeftTransient && i >= transientStart && i < transientEnd
                        ? 0.22f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                        : 0.0f;

                    input.setSample(0, i, leftSample);
                    input.setSample(1, i, rightSample);
                }

                return renderNoiseGateOutput(pedal, input, kBlockSize);
            };

            const auto isolated = renderLinked(false);
            const auto linked = renderLinked(true);
            const double isolatedRight = computeChannelWindowRms(isolated, 1, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.12));
            const double linkedRight = computeChannelWindowRms(linked, 1, (int) (kSampleRate * 0.25), (int) (kSampleRate * 0.12));

            expect(linkedRight > isolatedRight * 2.8, "A transient on one side should open the gate for both channels");
        }

        beginTest("NoiseGatePedal automation stress remains finite under aggressive changes");
        {
            NoiseGatePedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            const int totalSamples = (int) (kSampleRate * 2.8);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.16f * std::sin(juce::MathConstants<float>::twoPi * 98.0f * t)
                    + 0.015f * std::sin(juce::MathConstants<float>::twoPi * 2200.0f * t));
                input.setSample(1, i, 0.13f * std::sin(juce::MathConstants<float>::twoPi * 147.0f * t)
                    + 0.012f * std::sin(juce::MathConstants<float>::twoPi * 2700.0f * t));
            }

            juce::MidiBuffer midi;
            bool finite = true;
            double peak = 0.0;
            const int totalBlocks = juce::jmax(1, totalSamples / kBlockSize);

            for (int offset = 0, blockIndex = 0; offset < totalSamples; offset += kBlockSize, ++blockIndex)
            {
                const int numSamples = juce::jmin(kBlockSize, totalSamples - offset);
                juce::AudioBuffer<float> block(2, kBlockSize);
                block.clear();
                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom(ch, 0, input, ch, offset, numSamples);

                const float phase = (float) blockIndex / (float) juce::jmax(1, totalBlocks - 1);
                pedal.thresholdParam->setValueNotifyingHost(pedal.thresholdParam->convertTo0to1(-68.0f + 48.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(0.02f + 9.5f * std::abs(std::cos(phase * juce::MathConstants<float>::twoPi))));
                pedal.holdParam->setValueNotifyingHost(pedal.holdParam->convertTo0to1(180.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 0.75f))));
                pedal.releaseParam->setValueNotifyingHost(pedal.releaseParam->convertTo0to1(20.0f + 420.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.35f))));
                pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(-96.0f + 96.0f * std::abs(std::sin(phase * juce::MathConstants<float>::pi * 0.9f))));
                pedal.hysteresisParam->setValueNotifyingHost(pedal.hysteresisParam->convertTo0to1(1.0f + 17.0f * std::abs(std::cos(phase * juce::MathConstants<float>::pi * 1.6f))));
                pedal.focusParam->setValueNotifyingHost(pedal.focusParam->convertTo0to1(std::abs(std::sin(phase * juce::MathConstants<float>::pi * 1.9f))));

                pedal.processBlock(block, midi);
                finite = finite && bufferHasOnlyFiniteSamples(block);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
            }

            expect(finite, "Aggressive noise gate automation must remain finite");
            expect(peak < 1.2, "Noise gate automation should stay inside a sane peak ceiling");
        }

        beginTest("DistortionPedal round-trips its commercial state");
        {
            DistortionPedal source;
            source.modeParam->setValueNotifyingHost(normalisedChoiceIndex(source.modeParam, 4));
            source.gainParam->setValueNotifyingHost(source.gainParam->convertTo0to1(73.0f));
            source.toneParam->setValueNotifyingHost(source.toneParam->convertTo0to1(0.66f));
            source.bodyParam->setValueNotifyingHost(source.bodyParam->convertTo0to1(0.63f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.88f));
            source.levelParam->setValueNotifyingHost(source.levelParam->convertTo0to1(0.55f));
            source.tightParam->setValueNotifyingHost(source.tightParam->convertTo0to1(0.81f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            DistortionPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 4);
            expect(approximatelyEqual(restored.gainParam->get(), 73.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 0.66f, 1.0e-3f));
            expect(approximatelyEqual(restored.bodyParam->get(), 0.63f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.88f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 0.55f, 1.0e-3f));
            expect(approximatelyEqual(restored.tightParam->get(), 0.81f, 1.0e-3f));
        }

        beginTest("DistortionPedal restores legacy three-mode state without remapping its modes");
        {
            struct DistortionLegacyStateHelper : ProcessorBase
            {
                static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
                {
                    copyXmlToBinary(xml, block);
                }

                void prepareToPlay(double, int) override {}
                void releaseResources() override {}
                void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            };

            juce::XmlElement xml("PLUGIN_STATE");
            auto addParam = [&xml](const juce::String& id, float value)
            {
                auto* child = xml.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", value);
            };

            addParam("distMode", 0.5f);
            addParam("distGain", 0.73f);
            addParam("distTone", 0.61f);
            addParam("distBody", 0.68f);
            addParam("distMix", 0.84f);
            addParam("distLevel", 0.57f);
            addParam("distTight", 0.79f);

            juce::MemoryBlock state;
            DistortionLegacyStateHelper::encode(xml, state);

            DistortionPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 1);
            expect(approximatelyEqual(restored.gainParam->get(), 73.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 0.61f, 1.0e-3f));
            expect(approximatelyEqual(restored.bodyParam->get(), 0.68f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.84f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 0.57f, 1.0e-3f));
            expect(approximatelyEqual(restored.tightParam->get(), 0.79f, 1.0e-3f));
        }

        beginTest("DistortionPedal migrates legacy metal state into the unified circuit");
        {
            struct DistortionLegacyStateHelper : ProcessorBase
            {
                static void encode(const juce::XmlElement& xml, juce::MemoryBlock& block)
                {
                    copyXmlToBinary(xml, block);
                }

                void prepareToPlay(double, int) override {}
                void releaseResources() override {}
                void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            };

            juce::XmlElement xml("PLUGIN_STATE");
            auto addParam = [&xml](const juce::String& id, float value)
            {
                auto* child = xml.createNewChildElement("PARAM");
                child->setAttribute("id", id);
                child->setAttribute("value", value);
            };

            addParam("metalGain", 0.64f);
            addParam("metalLow", 0.625f);
            addParam("metalMid", ( -3.5f + 15.0f) / 30.0f);
            addParam("metalMidFreq", (950.0f - 250.0f) / 4750.0f);
            addParam("metalHigh", (4.0f + 12.0f) / 24.0f);
            addParam("metalLevel", 0.68f);

            juce::MemoryBlock state;
            DistortionLegacyStateHelper::encode(xml, state);

            DistortionPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 3);
            expect(approximatelyEqual(restored.gainParam->get(), 64.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 0.68f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 1.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.bodyParam->get(), 0.636f, 0.02f));
            expect(approximatelyEqual(restored.toneParam->get(), 0.728f, 0.02f));
            expect(approximatelyEqual(restored.tightParam->get(), 0.645f, 0.03f));
        }

        beginTest("DistortionPedal mix zero keeps the dry path transparent");
        {
            DistortionPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
            pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(88.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.71f));
            pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.64f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.66f));
            pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.58f));

            const int totalSamples = (int) (kSampleRate * 1.1);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phaseL = juce::MathConstants<float>::twoPi * 146.0f * (float) i / (float) kSampleRate;
                const float phaseR = juce::MathConstants<float>::twoPi * 233.0f * (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.17f * std::sin(phaseL));
                input.setSample(1, i, 0.13f * std::sin(phaseR));
            }

            const auto output = renderDistortionOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Dry-only distortion render must stay finite");
            expect(nullRms < 1.0e-5, "Mix at zero should leave the dry path effectively untouched");
        }

        beginTest("DistortionPedal modes produce distinct drive signatures");
        {
            auto renderMode = [&](int modeIndex)
            {
                DistortionPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(modeIndex >= 3 ? 82.0f : modeIndex == 1 ? 69.0f : 76.0f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(modeIndex >= 3 ? 0.52f : 0.57f));
                pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(modeIndex >= 3 ? 0.60f : 0.56f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.64f));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(modeIndex == 2 ? 0.82f : modeIndex >= 3 ? 0.74f : 0.45f));

                const int totalSamples = (int) (kSampleRate * 1.8);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.19f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderDistortionOutput(pedal, input, kBlockSize);
            };

            const auto vintage = renderMode(0);
            const auto turbo = renderMode(1);
            const auto amp = renderMode(2);
            const auto metal = renderMode(3);
            const auto studio = renderMode(4);

            const double vintageTurboNull = computeBufferNullRms(vintage, turbo);
            const double vintageAmpNull = computeBufferNullRms(vintage, amp);
            const double turboAmpNull = computeBufferNullRms(turbo, amp);
            const double ampMetalNull = computeBufferNullRms(amp, metal);
            const double metalStudioNull = computeBufferNullRms(metal, studio);
            const double vintageStudioNull = computeBufferNullRms(vintage, studio);

            expect(vintageTurboNull > 1.5e-3, "Vintage and Turbo modes should not collapse into the same response");
            expect(vintageAmpNull > 1.5e-3, "Vintage and Amp modes should remain clearly distinct");
            expect(turboAmpNull > 1.2e-3, "Turbo and Amp modes should remain clearly distinct");
            expect(ampMetalNull > 1.8e-3, "Amp and Metal modes should remain clearly distinct");
            expect(metalStudioNull > 1.2e-3, "Metal and Studio modes should not collapse into the same response");
            expect(vintageStudioNull > 1.8e-3, "Vintage and Studio modes should remain clearly distinct");
        }

        beginTest("DistortionPedal tight control audibly reduces low-end bloom in the modern voices");
        {
            auto renderTight = [&](float tightAmount)
            {
                DistortionPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 4));
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(82.0f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.46f));
                pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.74f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.64f));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(tightAmount));

                const int totalSamples = (int) (kSampleRate * 1.2);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 82.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.23f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderDistortionOutput(pedal, input, kBlockSize);
            };

            const auto loose = renderTight(0.0f);
            const auto tight = renderTight(1.0f);
            const double looseRms = computeWindowRms(loose, (int) (kSampleRate * 0.28), (int) (kSampleRate * 0.48));
            const double tightRms = computeWindowRms(tight, (int) (kSampleRate * 0.28), (int) (kSampleRate * 0.48));

            expect(tightRms < looseRms * 0.95, "High tightness should reduce low-note bloom compared to the loose setting");
        }

        beginTest("DistortionPedal metal mode tight control closes the integrated gate harder");
        {
            auto renderMetal = [&](float tightAmount)
            {
                DistortionPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 3));
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(94.0f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.58f));
                pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.62f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.64f));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(tightAmount));

                const int totalSamples = (int) (kSampleRate * 2.0);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    float sample = 0.0f;
                    if (t < 0.18f)
                        sample = 0.24f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                            + 0.07f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t);
                    else
                        sample = 0.0016f * std::sin(juce::MathConstants<float>::twoPi * 73.0f * t)
                            + 0.0009f * std::sin(juce::MathConstants<float>::twoPi * 181.0f * t);

                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderDistortionOutput(pedal, input, kBlockSize);
            };

            const auto loose = renderMetal(0.12f);
            const auto tight = renderMetal(0.90f);
            const double looseTail = computeWindowRms(loose, (int) (kSampleRate * 0.85), (int) (kSampleRate * 0.65));
            const double tightTail = computeWindowRms(tight, (int) (kSampleRate * 0.85), (int) (kSampleRate * 0.65));

            expect(bufferHasOnlyFiniteSamples(tight), "Metal-mode gate render must remain finite");
            expect(tightTail < looseTail * 0.82, "Higher Tight should close the integrated gate harder in Metal mode");
        }

        beginTest("P8A DistortionPedal metal high-gain output stays bounded before downstream ambience");
        {
            DistortionPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 3));
            pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(100.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.54f));
            pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.52f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(1.0f));
            pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.42f));

            const int totalSamples = (int) (kSampleRate * 1.2);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                const int pickSample = i % (int) (kSampleRate * 0.125);
                const float pick = std::exp(-(float) pickSample / 42.0f);
                const float note = 0.36f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                    + 0.19f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
                    + 0.11f * std::sin(juce::MathConstants<float>::twoPi * 247.23f * t);
                const float sample = juce::jlimit(-0.95f, 0.95f, note + pick * 0.58f);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderDistortionOutput(pedal, input, kBlockSize);
            P8AWindowMetrics metrics;
            metrics.capture(output);

            expect(metrics.finite, "P8A high-gain Distortion render must remain finite");
            expect(metrics.peak <= 1.20, "P8A Distortion output must stay inside the internal containment ceiling: " + p8aMetricsSummary(metrics));
            expect(metrics.dc() < 0.025, "P8A Distortion post-clipping DC should stay subsonic/low: " + p8aMetricsSummary(metrics));
        }

        beginTest("P8A DistortionPedal rejects DC accumulation under biased high-gain input");
        {
            DistortionPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 3));
            pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(92.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.57f));
            pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.68f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.82f));
            pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.35f));

            const int totalSamples = (int) (kSampleRate * 1.4);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                const float sample = 0.23f
                    + 0.27f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                    + 0.09f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample * 0.94f);
            }

            const auto output = renderDistortionOutput(pedal, input, kBlockSize);
            P8AWindowMetrics late;
            const int lateStart = (int) (kSampleRate * 0.80);
            juce::AudioBuffer<float> lateWindow(2, totalSamples - lateStart);
            for (int ch = 0; ch < 2; ++ch)
                lateWindow.copyFrom(ch, 0, output, ch, lateStart, lateWindow.getNumSamples());
            late.capture(lateWindow);

            expect(late.finite, "P8A biased Distortion render must remain finite");
            expect(late.peak <= 1.02, "P8A biased Distortion output should remain contained: " + p8aMetricsSummary(late));
            expect(late.dc() < 0.025, "P8A final DC blocker should drain sustained post-distortion bias: " + p8aMetricsSummary(late));
        }

        beginTest("P8A Distortion Reverb Chorus bypass recovery stays bounded");
        {
            InputChainProcessor input;
            DistortionPedal distortion;
            ReverbPedal reverb;
            ChorusPedal chorus;
            ChannelStripProcessor channelStrip;
            OutputChainProcessor outputChain;

            input.prepareToPlay(kSampleRate, kBlockSize);
            distortion.prepareToPlay(kSampleRate, kBlockSize);
            reverb.prepareToPlay(kSampleRate, kBlockSize);
            chorus.prepareToPlay(kSampleRate, kBlockSize);
            channelStrip.prepareToPlay(kSampleRate, kBlockSize);
            outputChain.prepareToPlay(kSampleRate, kBlockSize);

            input.setParams(-11.28f, -100.0f, true);
            channelStrip.setParams(2.0f, 0.0f, 2.0f);
            outputChain.setParams(-2.81f, -12.0f);

            distortion.setBypassed(true);
            distortion.modeParam->setValueNotifyingHost(normalisedChoiceIndex(distortion.modeParam, 3));
            distortion.gainParam->setValueNotifyingHost(distortion.gainParam->convertTo0to1(58.0f));
            distortion.toneParam->setValueNotifyingHost(distortion.toneParam->convertTo0to1(0.54f));
            distortion.bodyParam->setValueNotifyingHost(distortion.bodyParam->convertTo0to1(0.52f));
            distortion.mixParam->setValueNotifyingHost(distortion.mixParam->convertTo0to1(1.0f));
            distortion.levelParam->setValueNotifyingHost(distortion.levelParam->convertTo0to1(0.64f));
            distortion.tightParam->setValueNotifyingHost(distortion.tightParam->convertTo0to1(0.42f));

            reverb.modeParam->setValueNotifyingHost(normalisedChoiceIndex(reverb.modeParam, 2));
            reverb.decayParam->setValueNotifyingHost(reverb.decayParam->convertTo0to1(0.62f));
            reverb.toneParam->setValueNotifyingHost(reverb.toneParam->convertTo0to1(0.58f));
            reverb.sizeParam->setValueNotifyingHost(reverb.sizeParam->convertTo0to1(0.55f));
            reverb.dampingParam->setValueNotifyingHost(reverb.dampingParam->convertTo0to1(0.38f));
            reverb.widthParam->setValueNotifyingHost(reverb.widthParam->convertTo0to1(0.90f));
            reverb.mixParam->setValueNotifyingHost(reverb.mixParam->convertTo0to1(0.36f));

            chorus.rateParam->setValueNotifyingHost(chorus.rateParam->convertTo0to1(0.85f));
            chorus.depthParam->setValueNotifyingHost(chorus.depthParam->convertTo0to1(0.58f));
            chorus.widthParam->setValueNotifyingHost(chorus.widthParam->convertTo0to1(0.72f));
            chorus.toneParam->setValueNotifyingHost(chorus.toneParam->convertTo0to1(0.62f));
            chorus.mixParam->setValueNotifyingHost(chorus.mixParam->convertTo0to1(0.36f));

            constexpr int stableBlocks = 96;
            constexpr int activeBlocks = 96;
            constexpr int recoveryBlocks = 192;
            constexpr int lateRecoveryStart = stableBlocks + activeBlocks + 96;
            constexpr int totalBlocks = stableBlocks + activeBlocks + recoveryBlocks;

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            P8AWindowMetrics stableOutput;
            P8AWindowMetrics distortionOutput;
            P8AWindowMetrics reverbInput;
            P8AWindowMetrics reverbOutput;
            P8AWindowMetrics outputInput;
            P8AWindowMetrics activeOutput;
            P8AWindowMetrics lateRecoveryOutput;

            int stableLimiterActiveBlocks = 0;
            int stableLimiterTouchedSamples = 0;
            int recoveryLimiterActiveBlocks = 0;
            int recoveryLimiterTouchedSamples = 0;
            auto previousSnapshot = outputChain.getDebugSnapshot();

            for (int blockIndex = 0; blockIndex < totalBlocks; ++blockIndex)
            {
                if (blockIndex == stableBlocks)
                    distortion.setBypassed(false);
                if (blockIndex == stableBlocks + activeBlocks)
                    distortion.setBypassed(true);

                for (int i = 0; i < kBlockSize; ++i)
                {
                    const int sampleIndex = blockIndex * kBlockSize + i;
                    const float t = (float) sampleIndex / (float) kSampleRate;
                    const int pickSample = sampleIndex % (int) (kSampleRate * 0.125);
                    const float pick = std::exp(-(float) pickSample / 54.0f);
                    const float note = 0.55f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                        + 0.23f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
                        + 0.08f * std::sin(juce::MathConstants<float>::twoPi * 246.94f * t);
                    const float sample = juce::jlimit(-0.98f, 0.98f, note + pick * 0.42f);
                    block.setSample(0, i, sample);
                    block.setSample(1, i, sample * 0.96f);
                }

                input.processBlock(block, midi);
                distortion.processBlock(block, midi);

                if (blockIndex >= stableBlocks && blockIndex < stableBlocks + activeBlocks)
                    distortionOutput.capture(block);
                if (blockIndex >= stableBlocks)
                    reverbInput.capture(block);

                reverb.processBlock(block, midi);
                if (blockIndex >= stableBlocks && blockIndex < stableBlocks + activeBlocks)
                    reverbOutput.capture(block);

                chorus.processBlock(block, midi);
                channelStrip.processBlock(block, midi);
                if (blockIndex >= stableBlocks)
                    outputInput.capture(block);

                outputChain.processBlock(block, midi);

                if (blockIndex < stableBlocks)
                    stableOutput.capture(block);
                else if (blockIndex < stableBlocks + activeBlocks)
                    activeOutput.capture(block);
                else if (blockIndex >= lateRecoveryStart)
                    lateRecoveryOutput.capture(block);

                const auto snapshot = outputChain.getDebugSnapshot();
                if (blockIndex < stableBlocks)
                {
                    stableLimiterActiveBlocks += snapshot.limiterActiveBlocks - previousSnapshot.limiterActiveBlocks;
                    stableLimiterTouchedSamples += snapshot.limiterTouchedSamples - previousSnapshot.limiterTouchedSamples;
                }
                else if (blockIndex >= stableBlocks + activeBlocks)
                {
                    recoveryLimiterActiveBlocks += snapshot.limiterActiveBlocks - previousSnapshot.limiterActiveBlocks;
                    recoveryLimiterTouchedSamples += snapshot.limiterTouchedSamples - previousSnapshot.limiterTouchedSamples;
                }
                previousSnapshot = snapshot;
            }

            expect(distortionOutput.finite && reverbOutput.finite && activeOutput.finite && lateRecoveryOutput.finite,
                "P8A Distortion/Reverb/Chorus chain must remain finite");
            expect(distortionOutput.peak <= 1.20,
                "P8A Distortion must not feed destructive peaks into Reverb: " + p8aMetricsSummary(distortionOutput));
            expect(reverbInput.clippedSamples == 0,
                "P8A Reverb input should not receive sustained >1.0 samples after Distortion containment: " + p8aMetricsSummary(reverbInput));
            expect(reverbOutput.peak <= 1.35,
                "P8A Reverb output should stay bounded during Distortion activation: " + p8aMetricsSummary(reverbOutput));
            expect(outputInput.peak <= 2.45,
                "P8A ChannelStrip/Chorus should not present destructive peaks to OutputChain: " + p8aMetricsSummary(outputInput));
            expect(activeOutput.nearClipSamples == 0 && activeOutput.clippedSamples == 0,
                "P8A OutputChain should not leave near-clip/clipped final samples while Distortion is active: " + p8aMetricsSummary(activeOutput));
            expect(lateRecoveryOutput.rms() <= juce::jmax(0.12, stableOutput.rms() * 2.35),
                "P8A bypass recovery should return near the pre-activation output window; stable="
                    + p8aMetricsSummary(stableOutput)
                    + ", lateRecovery="
                    + p8aMetricsSummary(lateRecoveryOutput));
            const double stableLimiterBlocksPerBlock = (double) stableLimiterActiveBlocks / (double) stableBlocks;
            const double recoveryLimiterBlocksPerBlock = (double) recoveryLimiterActiveBlocks / (double) recoveryBlocks;
            const double stableTouchedPerBlock = (double) stableLimiterTouchedSamples / (double) stableBlocks;
            const double recoveryTouchedPerBlock = (double) recoveryLimiterTouchedSamples / (double) recoveryBlocks;
            expect(recoveryLimiterBlocksPerBlock <= stableLimiterBlocksPerBlock + 0.05
                    && recoveryTouchedPerBlock <= stableTouchedPerBlock * 1.20 + 8.0,
                "P8A OutputChain limiter recovery activity should return to the stable-chain rate; stableActiveBlocks="
                    + juce::String(stableLimiterActiveBlocks)
                    + ", stableTouchedSamples="
                    + juce::String(stableLimiterTouchedSamples)
                    + ", recoveryActiveBlocks="
                    + juce::String(recoveryLimiterActiveBlocks)
                    + ", recoveryTouchedSamples="
                    + juce::String(recoveryLimiterTouchedSamples));
        }

        beginTest("P8B Distortion nominal modes remain expressive below containment knee");
        {
            auto renderNominalMode = [&](int modeIndex)
            {
                DistortionPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(modeIndex == 3 ? 36.0f : modeIndex == 4 ? 32.0f : 28.0f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(modeIndex == 3 ? 0.54f : 0.58f));
                pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(modeIndex == 3 ? 0.52f : 0.56f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.22f));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(modeIndex == 3 ? 0.42f : 0.48f));

                const int totalSamples = (int) (kSampleRate * 1.35);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float t = (float) i / (float) kSampleRate;
                    const int pickSample = i % (int) (kSampleRate * 0.145);
                    const float pick = std::exp(-(float) pickSample / 68.0f);
                    const float vibrato = 1.0f + 0.0035f * std::sin(juce::MathConstants<float>::twoPi * 5.1f * t);
                    const float note = 0.030f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * vibrato * t)
                        + 0.014f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t)
                        + 0.007f * std::sin(juce::MathConstants<float>::twoPi * 330.0f * t);
                    const float sample = note + 0.010f * pick;
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample * 0.97f);
                }

                return renderDistortionOutput(pedal, input, kBlockSize);
            };

            const auto classic = renderNominalMode(0);
            const auto metal = renderNominalMode(3);
            const auto studio = renderNominalMode(4);

            P8AWindowMetrics classicMetrics;
            P8AWindowMetrics metalMetrics;
            P8AWindowMetrics studioMetrics;
            classicMetrics.capture(classic);
            metalMetrics.capture(metal);
            studioMetrics.capture(studio);

            const int classicKneeSamples = classicMetrics.nearClipSamples;
            const int metalKneeSamples = metalMetrics.nearClipSamples;
            const int studioKneeSamples = studioMetrics.nearClipSamples;

            expect(classicMetrics.finite && metalMetrics.finite && studioMetrics.finite,
                "P8B nominal Distortion renders must remain finite");
            expect(classicMetrics.peak < 0.98 && metalMetrics.peak < 0.98 && studioMetrics.peak < 0.98,
                "P8B nominal modes should stay below the containment knee; classic="
                    + p8aMetricsSummary(classicMetrics)
                    + ", metal="
                    + p8aMetricsSummary(metalMetrics)
                    + ", studio="
                    + p8aMetricsSummary(studioMetrics));
            expect(classicMetrics.dc() < 0.018 && metalMetrics.dc() < 0.018 && studioMetrics.dc() < 0.018,
                "P8B nominal modes should not accumulate meaningful DC");
            expect(classicKneeSamples == 0 && metalKneeSamples == 0 && studioKneeSamples == 0,
                "P8B nominal modes should not live at near-clip containment levels");
            expect(computeBufferNullRms(classic, metal) > 1.5e-3
                    && computeBufferNullRms(classic, studio) > 1.5e-3
                    && computeBufferNullRms(metal, studio) > 1.2e-3,
                "P8B Classic/Metal/Studio should retain distinct signatures below containment");
        }

        beginTest("P8B Distortion level mix gain sweep remains bounded and audible");
        {
            DistortionPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 3));

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            P8AWindowMetrics metrics;
            const int blocksToRun = (int) ((kSampleRate * 2.5) / (double) kBlockSize);

            for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
            {
                const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(18.0f + 82.0f * phase));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.05f + 0.95f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.16f + 0.84f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi * 0.72f))));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.22f + 0.56f * phase));
                pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.32f + 0.38f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.18f + 0.74f * phase));

                for (int i = 0; i < kBlockSize; ++i)
                {
                    const int sampleIndex = blockIndex * kBlockSize + i;
                    const float t = (float) sampleIndex / (float) kSampleRate;
                    const int pickSample = sampleIndex % (int) (kSampleRate * 0.105);
                    const float pick = std::exp(-(float) pickSample / 48.0f);
                    const float sample = 0.19f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                        + 0.08f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
                        + 0.12f * pick;
                    block.setSample(0, i, sample);
                    block.setSample(1, i, sample * 0.95f);
                }

                pedal.processBlock(block, midi);
                metrics.capture(block);
            }

            expect(metrics.finite, "P8B Distortion level/mix/gain sweep must remain finite");
            expect(metrics.peak <= 1.02, "P8B sweep should remain inside the containment ceiling: " + p8aMetricsSummary(metrics));
            expect(metrics.clippedSamples == 0, "P8B sweep should not produce >1.0 samples: " + p8aMetricsSummary(metrics));
            expect(metrics.dc() < 0.025, "P8B sweep should not accumulate DC: " + p8aMetricsSummary(metrics));
            expect(metrics.rms() > 0.025, "P8B sweep should not accidentally silence the pedal: " + p8aMetricsSummary(metrics));
        }

        beginTest("P8B Distortion into amp and cabinet remains bounded");
        {
            DistortionPedal distortion;
            CleanAmp amp;
            CabinetPedal cabinet;
            OutputChainProcessor output;

            distortion.prepareToPlay(kSampleRate, kBlockSize);
            amp.prepareToPlay(kSampleRate, kBlockSize);
            cabinet.prepareToPlay(kSampleRate, kBlockSize);
            output.prepareToPlay(kSampleRate, kBlockSize);

            distortion.modeParam->setValueNotifyingHost(normalisedChoiceIndex(distortion.modeParam, 3));
            distortion.gainParam->setValueNotifyingHost(distortion.gainParam->convertTo0to1(34.0f));
            distortion.toneParam->setValueNotifyingHost(distortion.toneParam->convertTo0to1(0.54f));
            distortion.bodyParam->setValueNotifyingHost(distortion.bodyParam->convertTo0to1(0.55f));
            distortion.mixParam->setValueNotifyingHost(distortion.mixParam->convertTo0to1(1.0f));
            distortion.levelParam->setValueNotifyingHost(distortion.levelParam->convertTo0to1(0.24f));
            distortion.tightParam->setValueNotifyingHost(distortion.tightParam->convertTo0to1(0.48f));

            output.setParams(-3.0f, -12.0f);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            P8AWindowMetrics distortionOut;
            P8AWindowMetrics ampCabOut;
            P8AWindowMetrics finalOut;

            const int blocksToRun = (int) ((kSampleRate * 1.6) / (double) kBlockSize);
            for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
            {
                for (int i = 0; i < kBlockSize; ++i)
                {
                    const int sampleIndex = blockIndex * kBlockSize + i;
                    const float t = (float) sampleIndex / (float) kSampleRate;
                    const int pickSample = sampleIndex % (int) (kSampleRate * 0.118);
                    const float pick = std::exp(-(float) pickSample / 50.0f);
                    const float sample = 0.026f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                        + 0.012f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t)
                        + 0.012f * pick;
                    block.setSample(0, i, sample);
                    block.setSample(1, i, sample * 0.96f);
                }

                distortion.processBlock(block, midi);
                distortionOut.capture(block);
                amp.processBlock(block, midi);
                cabinet.processBlock(block, midi);
                ampCabOut.capture(block);
                output.processBlock(block, midi);
                finalOut.capture(block);
            }

            expect(distortionOut.finite && ampCabOut.finite && finalOut.finite,
                "P8B Distortion/Amp/Cabinet chain must remain finite");
            expect(distortionOut.peak <= 1.02 && distortionOut.clippedSamples == 0,
                "P8B Distortion should feed Amp/Cabinet with contained signal: " + p8aMetricsSummary(distortionOut));
            expect(ampCabOut.peak <= 1.80 && ampCabOut.dc() < 0.030,
                "P8B Amp/Cabinet output should remain bounded after Distortion: " + p8aMetricsSummary(ampCabOut));
            expect(finalOut.nearClipSamples == 0 && finalOut.clippedSamples == 0,
                "P8B OutputChain after Distortion/Amp/Cabinet should not emit near-clip/clipped samples: " + p8aMetricsSummary(finalOut));
            expect(finalOut.rms() > 0.015,
                "P8B Distortion/Amp/Cabinet chain should remain audibly non-silent: " + p8aMetricsSummary(finalOut));
        }

        beginTest("P8B Distortion bypass unbypass stays click bounded");
        {
            DistortionPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 3));
            pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(72.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.56f));
            pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.58f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.72f));
            pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.55f));

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            P8AWindowMetrics metrics;
            double worstDelta = 0.0;
            std::array<float, 2> previousSample { 0.0f, 0.0f };
            bool havePrevious = false;

            constexpr int blocksToRun = 240;
            for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
            {
                if ((blockIndex % 32) == 0)
                    pedal.setBypassed((blockIndex / 32) % 2 == 1);

                for (int i = 0; i < kBlockSize; ++i)
                {
                    const int sampleIndex = blockIndex * kBlockSize + i;
                    const float t = (float) sampleIndex / (float) kSampleRate;
                    const float sample = 0.18f * std::sin(juce::MathConstants<float>::twoPi * 123.47f * t)
                        + 0.07f * std::sin(juce::MathConstants<float>::twoPi * 246.94f * t);
                    block.setSample(0, i, sample);
                    block.setSample(1, i, sample * 0.93f);
                }

                pedal.processBlock(block, midi);
                metrics.capture(block);

                for (int i = 0; i < kBlockSize; ++i)
                {
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const float sample = block.getSample(ch, i);
                        if (havePrevious)
                            worstDelta = juce::jmax(worstDelta, (double) std::abs(sample - previousSample[(size_t) ch]));
                        previousSample[(size_t) ch] = sample;
                    }
                    havePrevious = true;
                }
            }

            expect(metrics.finite, "P8B repeated Distortion bypass/unbypass must remain finite");
            expect(metrics.peak <= 1.02, "P8B repeated bypass/unbypass should remain contained: " + p8aMetricsSummary(metrics));
            expect(metrics.dc() < 0.025, "P8B repeated bypass/unbypass should not accumulate DC: " + p8aMetricsSummary(metrics));
            expect(worstDelta < 1.35, "P8B repeated bypass/unbypass should not create extreme click deltas");
        }

        beginTest("DistortionPedal automation stress remains finite under aggressive changes");
        {
            DistortionPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.74f));

            juce::Random rng(0xD1570);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            bool finite = true;
            double peak = 0.0;

            const int blocksToRun = (int) ((kSampleRate * 3.0) / (double) kBlockSize);
            for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlockSize; ++i)
                        block.setSample(ch, i, 0.16f * ((rng.nextFloat() * 2.0f) - 1.0f));

                const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                const int mode = juce::jlimit(0, 4, (int) std::floor(phase * 5.0f));

                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.gainParam->setValueNotifyingHost(pedal.gainParam->convertTo0to1(8.0f + 90.0f * phase));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.10f + 0.85f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.bodyParam->setValueNotifyingHost(pedal.bodyParam->convertTo0to1(0.08f + 0.84f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.12f + 0.82f * phase));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.18f + 0.70f * (1.0f - phase)));
                pedal.tightParam->setValueNotifyingHost(pedal.tightParam->convertTo0to1(0.02f + 0.96f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));

                pedal.processBlock(block, midi);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
                finite = finite && bufferHasOnlyFiniteSamples(block);
            }

            expect(finite, "Aggressive distortion automation must remain finite");
            expect(peak <= 1.02, "Distortion automation stress should stay inside the P8A containment ceiling");
        }

        beginTest("FuzzPedal round-trips its commercial state");
        {
            FuzzPedal source;
            source.modeParam->setValueNotifyingHost(normalisedChoiceIndex(source.modeParam, 2));
            source.fuzzParam->setValueNotifyingHost(source.fuzzParam->convertTo0to1(81.0f));
            source.toneParam->setValueNotifyingHost(source.toneParam->convertTo0to1(0.41f));
            source.gateParam->setValueNotifyingHost(source.gateParam->convertTo0to1(0.63f));
            source.mixParam->setValueNotifyingHost(source.mixParam->convertTo0to1(0.89f));
            source.levelParam->setValueNotifyingHost(source.levelParam->convertTo0to1(0.54f));
            source.biasParam->setValueNotifyingHost(source.biasParam->convertTo0to1(0.28f));

            juce::MemoryBlock state;
            source.getStateInformation(state);

            FuzzPedal restored;
            restored.setStateInformation(state.getData(), (int) state.getSize());

            expectEquals(restored.modeParam->getIndex(), 2);
            expect(approximatelyEqual(restored.fuzzParam->get(), 81.0f, 1.0e-3f));
            expect(approximatelyEqual(restored.toneParam->get(), 0.41f, 1.0e-3f));
            expect(approximatelyEqual(restored.gateParam->get(), 0.63f, 1.0e-3f));
            expect(approximatelyEqual(restored.mixParam->get(), 0.89f, 1.0e-3f));
            expect(approximatelyEqual(restored.levelParam->get(), 0.54f, 1.0e-3f));
            expect(approximatelyEqual(restored.biasParam->get(), 0.28f, 1.0e-3f));
        }

        beginTest("FuzzPedal mix zero keeps the dry path transparent");
        {
            FuzzPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
            pedal.fuzzParam->setValueNotifyingHost(pedal.fuzzParam->convertTo0to1(92.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.38f));
            pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(0.54f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.0f));
            pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.58f));
            pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.64f));

            const int totalSamples = (int) (kSampleRate * 1.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phaseL = juce::MathConstants<float>::twoPi * 110.0f * (float) i / (float) kSampleRate;
                const float phaseR = juce::MathConstants<float>::twoPi * 173.0f * (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.18f * std::sin(phaseL));
                input.setSample(1, i, 0.13f * std::sin(phaseR));
            }

            const auto output = renderFuzzOutput(pedal, input, kBlockSize);
            const double nullRms = computeBufferNullRms(input, output);

            expect(bufferHasOnlyFiniteSamples(output), "Dry-only fuzz render must stay finite");
            expect(nullRms < 1.0e-5, "Mix at zero should leave the dry path effectively untouched");
        }

        beginTest("FuzzPedal modes produce distinct fuzz signatures");
        {
            auto renderMode = [&](int modeIndex)
            {
                FuzzPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, modeIndex));
                pedal.fuzzParam->setValueNotifyingHost(pedal.fuzzParam->convertTo0to1(modeIndex == 1 ? 76.0f : 84.0f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.44f));
                pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(modeIndex == 2 ? 0.62f : 0.24f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.58f));
                pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(modeIndex == 2 ? 0.22f : 0.60f));

                const int totalSamples = (int) (kSampleRate * 1.7);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                for (int i = 0; i < totalSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 146.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.19f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderFuzzOutput(pedal, input, kBlockSize);
            };

            const auto vintage = renderMode(0);
            const auto muff = renderMode(1);
            const auto velcro = renderMode(2);

            const double vintageMuffNull = computeBufferNullRms(vintage, muff);
            const double vintageVelcroNull = computeBufferNullRms(vintage, velcro);
            const double muffVelcroNull = computeBufferNullRms(muff, velcro);

            expect(vintageMuffNull > 1.3e-3, "Vintage and Muff modes should not collapse into the same fuzz signature");
            expect(vintageVelcroNull > 1.6e-3, "Vintage and Velcro modes should remain clearly distinct");
            expect(muffVelcroNull > 1.5e-3, "Muff and Velcro modes should remain clearly distinct");
        }

        beginTest("FuzzPedal gate and bias can force a stronger velcro decay");
        {
            auto renderPedal = [&](float gateAmount, float biasAmount)
            {
                FuzzPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 2));
                pedal.fuzzParam->setValueNotifyingHost(pedal.fuzzParam->convertTo0to1(88.0f));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.36f));
                pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(gateAmount));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.58f));
                pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(biasAmount));

                const int totalSamples = (int) (kSampleRate * 1.3);
                juce::AudioBuffer<float> input(2, totalSamples);
                input.clear();
                const int burstSamples = (int) (kSampleRate * 0.24);
                for (int i = 0; i < burstSamples; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi * 98.0f * (float) i / (float) kSampleRate;
                    const float sample = 0.22f * std::sin(phase);
                    input.setSample(0, i, sample);
                    input.setSample(1, i, sample);
                }

                return renderFuzzOutput(pedal, input, kBlockSize);
            };

            const auto loose = renderPedal(0.10f, 0.72f);
            const auto velcro = renderPedal(0.88f, 0.12f);
            const double looseTail = computeWindowRms(loose, (int) (kSampleRate * 0.86), (int) (kSampleRate * 0.18));
            const double velcroTail = computeWindowRms(velcro, (int) (kSampleRate * 0.86), (int) (kSampleRate * 0.18));

            expect(velcroTail < looseTail * 0.78, "High gate with low bias should clamp the late velcro tail more strongly");
        }

        beginTest("FuzzPedal automation stress remains finite under aggressive changes");
        {
            FuzzPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.78f));

            juce::Random rng(0xF0222);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            bool finite = true;
            double peak = 0.0;

            const int blocksToRun = (int) ((kSampleRate * 3.0) / (double) kBlockSize);
            for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlockSize; ++i)
                        block.setSample(ch, i, 0.15f * ((rng.nextFloat() * 2.0f) - 1.0f));

                const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                const int mode = juce::jlimit(0, 2, (int) std::floor(phase * 3.0f));

                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, mode));
                pedal.fuzzParam->setValueNotifyingHost(pedal.fuzzParam->convertTo0to1(5.0f + 92.0f * phase));
                pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(0.08f + 0.86f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                pedal.gateParam->setValueNotifyingHost(pedal.gateParam->convertTo0to1(0.04f + 0.94f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.16f + 0.82f * phase));
                pedal.levelParam->setValueNotifyingHost(pedal.levelParam->convertTo0to1(0.15f + 0.74f * (1.0f - phase)));
                pedal.biasParam->setValueNotifyingHost(pedal.biasParam->convertTo0to1(0.02f + 0.96f * std::abs(std::sin(phase * juce::MathConstants<float>::pi))));

                pedal.processBlock(block, midi);
                peak = juce::jmax(peak, (double) block.getMagnitude(0, 0, block.getNumSamples()));
                peak = juce::jmax(peak, (double) block.getMagnitude(1, 0, block.getNumSamples()));
                finite = finite && bufferHasOnlyFiniteSamples(block);
            }

            expect(finite, "Aggressive fuzz automation must remain finite");
            expect(peak < 2.4, "Fuzz automation stress should stay inside a sane peak ceiling");
        }

        beginTest("P8C active pedal catalog processors remain finite under strong input");
        {
            juce::MidiBuffer midi;
            int checkedProcessors = 0;

            for (const auto& entry : Nova::PedalCatalog::entries())
            {
                const auto typeName = juce::String(entry.typeID);
                expect(PedalRegistry::isTypeSupported(entry.typeID), "P8C catalog type must be registered: " + typeName);
                auto processor = PedalRegistry::createPedal(entry.typeID);
                expect(processor != nullptr, "P8C registry must instantiate active type: " + typeName);
                if (processor == nullptr)
                    continue;

                processor->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                processor->prepareToPlay(kSampleRate, kBlockSize);

                juce::AudioBuffer<float> block(2, kBlockSize);
                P8AWindowMetrics metrics;
                const int blocksToRun = (int) ((kSampleRate * 0.85) / (double) kBlockSize);

                for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
                {
                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        const int sampleIndex = blockIndex * kBlockSize + i;
                        const float t = (float) sampleIndex / (float) kSampleRate;
                        const int pickSample = sampleIndex % (int) (kSampleRate * 0.092);
                        const float pick = std::exp(-(float) pickSample / 42.0f);
                        const float transient = (sampleIndex % 4096 == 0) ? 0.32f : 0.0f;
                        const float sample = 0.070f * std::sin(juce::MathConstants<float>::twoPi * 97.999f * t)
                            + 0.030f * std::sin(juce::MathConstants<float>::twoPi * 195.998f * t)
                            + 0.018f * pick
                            + transient;
                        block.setSample(0, i, sample);
                        block.setSample(1, i, sample * 0.91f);
                    }

                    processor->processBlock(block, midi);
                    metrics.capture(block);
                }

                expect(metrics.finite, "P8C " + typeName + " strong-input render must remain finite");
                expect(metrics.peak <= (double) Nova::Config::HARD_ABS_LIMIT_LINEAR,
                    "P8C " + typeName + " strong-input render must stay below the hard absolute limit: " + p8aMetricsSummary(metrics));
                expect(metrics.dc() < 0.50,
                    "P8C " + typeName + " strong-input render should not accumulate dominant DC: " + p8aMetricsSummary(metrics));
                expect(metrics.clippedSamples < juce::jmax(1, metrics.sampleCount) * 95 / 100,
                    "P8C " + typeName + " strong-input render should not become sustained clipped output: " + p8aMetricsSummary(metrics));
                ++checkedProcessors;
            }

            expectEquals(checkedProcessors, (int) Nova::PedalCatalog::entries().size(),
                "P8C should exercise every active catalog processor");
        }

        beginTest("P8C active pedal catalog automation extremes remain finite");
        {
            juce::MidiBuffer midi;
            int checkedProcessors = 0;

            for (const auto& entry : Nova::PedalCatalog::entries())
            {
                const auto typeName = juce::String(entry.typeID);
                auto processor = PedalRegistry::createPedal(entry.typeID);
                if (processor == nullptr)
                    continue;

                processor->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                processor->prepareToPlay(kSampleRate, kBlockSize);

                auto params = processor->getParameters();
                juce::AudioBuffer<float> block(2, kBlockSize);
                P8AWindowMetrics metrics;
                const int blocksToRun = (int) ((kSampleRate * 0.90) / (double) kBlockSize);

                for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
                {
                    const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                    for (int p = 0; p < params.size(); ++p)
                    {
                        auto* param = params.getUnchecked(p);
                        if (param == nullptr)
                            continue;

                        const float lane = std::fmod(phase + 0.173f * (float) (p + 1), 1.0f);
                        const float value = (blockIndex % 37 == 0) ? 0.0f
                            : (blockIndex % 41 == 0) ? 1.0f
                            : 0.5f + 0.5f * std::sin(juce::MathConstants<float>::twoPi * lane);
                        param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
                    }

                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        const int sampleIndex = blockIndex * kBlockSize + i;
                        const float t = (float) sampleIndex / (float) kSampleRate;
                        const float sample = 0.045f * std::sin(juce::MathConstants<float>::twoPi * 123.47f * t)
                            + 0.021f * std::sin(juce::MathConstants<float>::twoPi * 246.94f * t);
                        block.setSample(0, i, sample);
                        block.setSample(1, i, sample * 0.88f);
                    }

                    processor->processBlock(block, midi);
                    metrics.capture(block);
                }

                expect(metrics.finite, "P8C " + typeName + " generic automation sweep must remain finite");
                expect(metrics.peak <= (double) Nova::Config::HARD_ABS_LIMIT_LINEAR,
                    "P8C " + typeName + " generic automation sweep must stay below the hard absolute limit: " + p8aMetricsSummary(metrics));
                expect(metrics.dc() < 0.50,
                    "P8C " + typeName + " generic automation sweep should not accumulate dominant DC: " + p8aMetricsSummary(metrics));
                ++checkedProcessors;
            }

            expectEquals(checkedProcessors, (int) Nova::PedalCatalog::entries().size(),
                "P8C automation sweep should exercise every active catalog processor");
        }

        beginTest("P8C active pedal catalog bypass transitions remain bounded");
        {
            juce::MidiBuffer midi;
            int checkedProcessors = 0;

            for (const auto& entry : Nova::PedalCatalog::entries())
            {
                const auto typeName = juce::String(entry.typeID);
                auto processor = PedalRegistry::createPedal(entry.typeID);
                auto* processorBase = dynamic_cast<ProcessorBase*>(processor.get());
                if (processor == nullptr || processorBase == nullptr)
                    continue;

                processor->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                processor->prepareToPlay(kSampleRate, kBlockSize);

                juce::AudioBuffer<float> block(2, kBlockSize);
                P8AWindowMetrics metrics;
                double worstDelta = 0.0;
                std::array<float, 2> previousSample { 0.0f, 0.0f };
                bool havePrevious = false;
                const int blocksToRun = (int) ((kSampleRate * 0.65) / (double) kBlockSize);

                for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
                {
                    if (blockIndex % 24 == 0)
                        processorBase->setBypassed((blockIndex / 24) % 2 == 1);

                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        const int sampleIndex = blockIndex * kBlockSize + i;
                        const float t = (float) sampleIndex / (float) kSampleRate;
                        const float sample = 0.055f * std::sin(juce::MathConstants<float>::twoPi * 146.83f * t)
                            + 0.026f * std::sin(juce::MathConstants<float>::twoPi * 293.66f * t);
                        block.setSample(0, i, sample);
                        block.setSample(1, i, sample * 0.93f);
                    }

                    processor->processBlock(block, midi);
                    metrics.capture(block);

                    for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    {
                        const auto* data = block.getReadPointer(ch);
                        for (int i = 0; i < block.getNumSamples(); ++i)
                        {
                            if (havePrevious)
                                worstDelta = juce::jmax(worstDelta, (double) std::abs(data[i] - previousSample[(size_t) ch]));
                            previousSample[(size_t) ch] = data[i];
                        }
                    }
                    havePrevious = true;
                }

                expect(metrics.finite, "P8C " + typeName + " bypass transitions must remain finite");
                expect(metrics.peak <= (double) Nova::Config::HARD_ABS_LIMIT_LINEAR,
                    "P8C " + typeName + " bypass transitions must stay below the hard absolute limit: " + p8aMetricsSummary(metrics));
                expect(worstDelta < 8.0,
                    "P8C " + typeName + " bypass transitions should not produce extreme discontinuities");
                ++checkedProcessors;
            }

            expectEquals(checkedProcessors, (int) Nova::PedalCatalog::entries().size(),
                "P8C bypass transition sweep should exercise every active catalog processor");
        }

        beginTest("P8D Wah sweep resonance bias and bypass remain bounded");
        {
            auto renderWah = [&](bool automate, bool bypass)
            {
                ClassicWahPedal pedal;
                pedal.prepareToPlay(kSampleRate, kBlockSize);
                pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 0));
                pedal.sweepParam->setValueNotifyingHost(pedal.sweepParam->convertTo0to1(automate ? 0.05f : 0.46f));
                pedal.sensitivityParam->setValueNotifyingHost(pedal.sensitivityParam->convertTo0to1(automate ? 0.92f : 0.58f));
                pedal.attackParam->setValueNotifyingHost(pedal.attackParam->convertTo0to1(automate ? 0.5f : 2.0f));
                pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(automate ? 18.0f : 120.0f));
                pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(automate ? 1.0f : 0.76f));
                pedal.resonanceParam->setValueNotifyingHost(pedal.resonanceParam->convertTo0to1(automate ? 9.6f : 4.2f));
                pedal.voiceParam->setValueNotifyingHost(pedal.voiceParam->convertTo0to1(automate ? 0.84f : 0.36f));
                pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(automate ? 1.0f : 0.72f));

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> block(2, kBlockSize);
                P8AWindowMetrics metrics;
                double worstDelta = 0.0;
                std::array<float, 2> previousSample { 0.0f, 0.0f };
                bool havePrevious = false;
                const int blocksToRun = (int) ((kSampleRate * 1.0) / (double) kBlockSize);

                for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
                {
                    const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                    if (automate)
                    {
                        pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, (blockIndex / 37) % 3));
                        pedal.sweepParam->setValueNotifyingHost(pedal.sweepParam->convertTo0to1(phase));
                        pedal.rangeParam->setValueNotifyingHost(pedal.rangeParam->convertTo0to1(0.05f + 0.95f * std::abs(std::sin(phase * juce::MathConstants<float>::twoPi))));
                        pedal.resonanceParam->setValueNotifyingHost(pedal.resonanceParam->convertTo0to1(0.6f + 9.2f * std::abs(std::cos(phase * juce::MathConstants<float>::pi))));
                        pedal.voiceParam->setValueNotifyingHost(pedal.voiceParam->convertTo0to1(0.04f + 0.92f * phase));
                    }
                    if (bypass && blockIndex % 28 == 0)
                        pedal.setBypassed((blockIndex / 28) % 2 == 1);

                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        const int sampleIndex = blockIndex * kBlockSize + i;
                        const float t = (float) sampleIndex / (float) kSampleRate;
                        const float peak = (sampleIndex % 4096 == 0) ? 0.32f : 0.0f;
                        const float bias = automate ? 0.055f : 0.0f;
                        const float sample = bias
                            + 0.055f * std::sin(juce::MathConstants<float>::twoPi * 130.81f * t)
                            + 0.025f * std::sin(juce::MathConstants<float>::twoPi * 261.63f * t)
                            + peak;
                        block.setSample(0, i, sample);
                        block.setSample(1, i, sample * 0.90f);
                    }

                    pedal.processBlock(block, midi);
                    metrics.capture(block);

                    for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    {
                        const auto* data = block.getReadPointer(ch);
                        for (int i = 0; i < block.getNumSamples(); ++i)
                        {
                            if (havePrevious)
                                worstDelta = juce::jmax(worstDelta, (double) std::abs(data[i] - previousSample[(size_t) ch]));
                            previousSample[(size_t) ch] = data[i];
                        }
                    }
                    havePrevious = true;
                }

                return std::pair<P8AWindowMetrics, double> { metrics, worstDelta };
            };

            auto nominal = renderWah(false, false);
            auto extreme = renderWah(true, false);
            auto bypass = renderWah(true, true);

            expect(nominal.first.finite && extreme.first.finite && bypass.first.finite,
                "P8D Wah targeted renders must remain finite");
            expect(nominal.first.nearClipSamples < juce::jmax(1, nominal.first.sampleCount) / 40,
                "P8D nominal Wah should not live near clip: " + p8aMetricsSummary(nominal.first));
            expect(extreme.first.peak <= (double) Nova::Config::HARD_ABS_LIMIT_LINEAR
                    && extreme.first.dc() < 0.20
                    && extreme.first.clippedSamples < juce::jmax(1, extreme.first.sampleCount) / 3,
                "P8D extreme Wah sweep/resonance should stay bounded: " + p8aMetricsSummary(extreme.first));
            expect(bypass.first.peak <= (double) Nova::Config::HARD_ABS_LIMIT_LINEAR && bypass.second < 8.0,
                "P8D Wah bypass/unbypass should stay bounded: " + p8aMetricsSummary(bypass.first));

            ClassicWahPedal dryOnly;
            dryOnly.prepareToPlay(kSampleRate, kBlockSize);
            dryOnly.mixParam->setValueNotifyingHost(dryOnly.mixParam->convertTo0to1(0.0f));
            juce::AudioBuffer<float> dryInput(2, (int) (kSampleRate * 0.25));
            for (int i = 0; i < dryInput.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                const float sample = 0.035f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t);
                dryInput.setSample(0, i, sample);
                dryInput.setSample(1, i, sample * 0.94f);
            }
            auto dryReference = dryInput;
            juce::AudioBuffer<float> dryOutput(2, dryInput.getNumSamples());
            juce::MidiBuffer dryMidi;
            for (int start = 0; start < dryInput.getNumSamples(); start += kBlockSize)
            {
                const int count = juce::jmin(kBlockSize, dryInput.getNumSamples() - start);
                juce::AudioBuffer<float> block(dryOutput.getArrayOfWritePointers(), dryOutput.getNumChannels(), start, count);
                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    block.copyFrom(ch, 0, dryInput, ch, start, count);
                dryOnly.processBlock(block, dryMidi);
            }
            juce::AudioBuffer<float> dryReferenceTail(2, dryReference.getNumSamples() / 2);
            juce::AudioBuffer<float> dryOutputTail(2, dryOutput.getNumSamples() / 2);
            const int tailStart = dryReference.getNumSamples() / 2;
            for (int ch = 0; ch < 2; ++ch)
            {
                dryReferenceTail.copyFrom(ch, 0, dryReference, ch, tailStart, dryReferenceTail.getNumSamples());
                dryOutputTail.copyFrom(ch, 0, dryOutput, ch, tailStart, dryOutputTail.getNumSamples());
            }
            expect(computeBufferNullRms(dryReferenceTail, dryOutputTail) < 2.0e-4,
                "P8D Wah mix=0 should remain transparent");
            expect(PedalRegistry::canonicalType("Auto Wah") == "Wah"
                    && PedalRegistry::canonicalType("Autowah") == "Wah"
                    && PedalRegistry::canonicalType("AutoWah") == "Wah",
                "P8D Wah legacy aliases should remain canonical");
        }

        beginTest("P8D amp variants targeted strong input automation and bypass remain bounded");
        {
            auto setParam = [](juce::AudioProcessor& processor, const juce::String& paramId, float plainValue)
            {
                for (auto* param : processor.getParameters())
                {
                    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                    {
                        if (ranged->getParameterID() == paramId)
                        {
                            ranged->setValueNotifyingHost(ranged->convertTo0to1(plainValue));
                            return true;
                        }
                    }
                }
                return false;
            };

            auto exerciseAmp = [&](const juce::String& typeId, bool highGain, bool bright)
            {
                auto processor = PedalRegistry::createPedal(typeId);
                expect(processor != nullptr, "P8D amp must instantiate: " + typeId);
                auto* base = dynamic_cast<ProcessorBase*>(processor.get());
                expect(base != nullptr, "P8D amp must derive ProcessorBase: " + typeId);
                if (processor == nullptr || base == nullptr)
                    return std::tuple<P8AWindowMetrics, P8AWindowMetrics, double> {};

                processor->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                processor->prepareToPlay(kSampleRate, kBlockSize);

                if (typeId == "Classic Amp")
                {
                    setParam(*processor, "ampDrive", 5.6f);
                    setParam(*processor, "ampTone", 0.78f);
                    setParam(*processor, "ampPresence", 0.82f);
                    setParam(*processor, "ampDepth", 0.70f);
                    setParam(*processor, "ampLevel", 0.86f);
                }
                else if (typeId == "High Gain Amp")
                {
                    setParam(*processor, "hgDrive", 9.4f);
                    setParam(*processor, "hgTone", 0.58f);
                    setParam(*processor, "hgPresence", 0.76f);
                    setParam(*processor, "hgTight", 0.78f);
                    setParam(*processor, "hgLevel", 0.70f);
                }
                else if (typeId == "Chime Amp")
                {
                    setParam(*processor, "chimeDrive", 3.6f);
                    setParam(*processor, "chimeTrebleCut", 0.92f);
                    setParam(*processor, "chimeBassCut", 0.26f);
                    setParam(*processor, "chimeBrill", 0.96f);
                    setParam(*processor, "chimeLevel", 0.84f);
                }
                else if (typeId == "Boutique Amp")
                {
                    setParam(*processor, "boutDrive", 3.2f);
                    setParam(*processor, "boutWarmth", 0.76f);
                    setParam(*processor, "boutMid", 0.68f);
                    setParam(*processor, "boutPres", 0.72f);
                    setParam(*processor, "boutLevel", 0.88f);
                }

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> block(2, kBlockSize);
                P8AWindowMetrics strongMetrics;
                P8AWindowMetrics nominalMetrics;
                double worstDelta = 0.0;
                std::array<float, 2> previousSample { 0.0f, 0.0f };
                bool havePrevious = false;
                const int blocksToRun = (int) ((kSampleRate * 1.0) / (double) kBlockSize);

                for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
                {
                    const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                    for (auto* param : processor->getParameters())
                    {
                        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                        {
                            const float value = 0.5f + 0.5f * std::sin(juce::MathConstants<float>::twoPi * (phase + 0.11f));
                            ranged->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, (blockIndex % 53 == 0) ? 1.0f : value));
                        }
                    }
                    if (blockIndex % 34 == 0)
                        base->setBypassed((blockIndex / 34) % 2 == 1);

                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        const int sampleIndex = blockIndex * kBlockSize + i;
                        const float t = (float) sampleIndex / (float) kSampleRate;
                        const int mute = sampleIndex % (int) (kSampleRate * 0.110);
                        const float palm = highGain ? std::exp(-(float) mute / 34.0f) : std::exp(-(float) mute / 65.0f);
                        const float spike = bright && (sampleIndex % 3072 == 0) ? 0.26f : 0.0f;
                        const float bias = (blockIndex < blocksToRun / 4) ? 0.018f : 0.0f;
                        const float sample = bias
                            + (highGain ? 0.060f : 0.070f) * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                            + (bright ? 0.042f : 0.025f) * std::sin(juce::MathConstants<float>::twoPi * 659.25f * t)
                            + 0.040f * palm
                            + spike;
                        block.setSample(0, i, sample);
                        block.setSample(1, i, sample * 0.92f);
                    }

                    processor->processBlock(block, midi);
                    strongMetrics.capture(block);
                    if (!base->getBypassed())
                        nominalMetrics.capture(block);

                    for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    {
                        const auto* data = block.getReadPointer(ch);
                        for (int i = 0; i < block.getNumSamples(); ++i)
                        {
                            if (havePrevious)
                                worstDelta = juce::jmax(worstDelta, (double) std::abs(data[i] - previousSample[(size_t) ch]));
                            previousSample[(size_t) ch] = data[i];
                        }
                    }
                    havePrevious = true;
                }

                return std::tuple<P8AWindowMetrics, P8AWindowMetrics, double> { strongMetrics, nominalMetrics, worstDelta };
            };

            const std::array<juce::String, 4> ampTypes { "Classic Amp", "High Gain Amp", "Chime Amp", "Boutique Amp" };
            for (const auto& ampType : ampTypes)
            {
                auto [strong, nominal, worstDelta] = exerciseAmp(ampType, ampType == "High Gain Amp", ampType == "Chime Amp");
                expect(strong.finite, "P8D " + ampType + " targeted render must remain finite");
                expect(strong.peak <= (double) Nova::Config::HARD_ABS_LIMIT_LINEAR && strong.dc() < 0.25,
                    "P8D " + ampType + " targeted render must stay bounded: " + p8aMetricsSummary(strong));
                expect(strong.clippedSamples < juce::jmax(1, strong.sampleCount) * 90 / 100,
                    "P8D " + ampType + " should not become sustained clipped output: " + p8aMetricsSummary(strong));
                expect(nominal.nearClipSamples < juce::jmax(1, nominal.sampleCount) * 75 / 100,
                    "P8D " + ampType + " nominal active windows should not be dominated by near-clip samples: " + p8aMetricsSummary(nominal));
                expect(worstDelta < 12.0, "P8D " + ampType + " bypass/bright transitions should remain bounded");
            }
        }

        beginTest("P8D cabinet variants and high-gain chains remain bounded");
        {
            auto setParam = [](juce::AudioProcessor& processor, const juce::String& paramId, float plainValue)
            {
                for (auto* param : processor.getParameters())
                {
                    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                    {
                        if (ranged->getParameterID() == paramId)
                        {
                            ranged->setValueNotifyingHost(ranged->convertTo0to1(plainValue));
                            return true;
                        }
                    }
                }
                return false;
            };

            auto exerciseCabinet = [&](const juce::String& cabType, bool useHighGainAmp)
            {
                auto amp = PedalRegistry::createPedal(useHighGainAmp ? "High Gain Amp" : "Classic Amp");
                auto cab = PedalRegistry::createPedal(cabType);
                expect(amp != nullptr && cab != nullptr, "P8D cabinet chain must instantiate: " + cabType);
                auto* cabBase = dynamic_cast<ProcessorBase*>(cab.get());
                expect(cabBase != nullptr, "P8D cabinet must derive ProcessorBase: " + cabType);
                if (amp == nullptr || cab == nullptr || cabBase == nullptr)
                    return std::pair<P8AWindowMetrics, double> {};

                amp->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                cab->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                amp->prepareToPlay(kSampleRate, kBlockSize);
                cab->prepareToPlay(kSampleRate, kBlockSize);

                if (useHighGainAmp)
                {
                    setParam(*amp, "hgDrive", 8.4f);
                    setParam(*amp, "hgTight", 0.82f);
                    setParam(*amp, "hgPresence", 0.66f);
                    setParam(*amp, "hgLevel", 0.58f);
                }
                else
                {
                    setParam(*amp, "ampDrive", 4.8f);
                    setParam(*amp, "ampPresence", 0.58f);
                    setParam(*amp, "ampLevel", 0.68f);
                }

                if (cabType == "Vintage 2x12")
                {
                    setParam(*cab, "v2x12Warmth", 6.0f);
                    setParam(*cab, "v2x12Sparkle", 4.5f);
                    setParam(*cab, "v2x12Distance", 0.35f);
                    setParam(*cab, "v2x12Mix", 1.0f);
                    setParam(*cab, "v2x12Level", 0.82f);
                }
                else if (cabType == "Modern 4x12")
                {
                    setParam(*cab, "m4x12Low", 5.0f);
                    setParam(*cab, "m4x12Presence", 6.0f);
                    setParam(*cab, "m4x12Distance", 0.25f);
                    setParam(*cab, "m4x12Mix", 1.0f);
                    setParam(*cab, "m4x12Level", 0.78f);
                }
                else
                {
                    setParam(*cab, "cabThump", 4.5f);
                    setParam(*cab, "cabAir", 4.0f);
                    setParam(*cab, "cabDistance", 0.32f);
                    setParam(*cab, "cabMix", 1.0f);
                    setParam(*cab, "cabLevel", 0.82f);
                }

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> block(2, kBlockSize);
                P8AWindowMetrics chainMetrics;
                double worstDelta = 0.0;
                std::array<float, 2> previousSample { 0.0f, 0.0f };
                bool havePrevious = false;
                const int blocksToRun = (int) ((kSampleRate * 1.0) / (double) kBlockSize);

                for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
                {
                    const float phase = (float) blockIndex / (float) juce::jmax(1, blocksToRun - 1);
                    for (auto* param : cab->getParameters())
                    {
                        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                        {
                            const float value = 0.5f + 0.5f * std::sin(juce::MathConstants<float>::twoPi * (phase + 0.19f));
                            ranged->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, (blockIndex % 47 == 0) ? 1.0f : value));
                        }
                    }
                    if (blockIndex % 31 == 0)
                        cabBase->setBypassed((blockIndex / 31) % 2 == 1);

                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        const int sampleIndex = blockIndex * kBlockSize + i;
                        const float t = (float) sampleIndex / (float) kSampleRate;
                        const int pickSample = sampleIndex % (int) (kSampleRate * 0.118);
                        const float pick = std::exp(-(float) pickSample / 46.0f);
                        const float spike = (sampleIndex % 3584 == 0) ? 0.24f : 0.0f;
                        const float bias = (blockIndex < blocksToRun / 5) ? 0.020f : 0.0f;
                        const float sample = bias
                            + (useHighGainAmp ? 0.050f : 0.060f) * std::sin(juce::MathConstants<float>::twoPi * 98.0f * t)
                            + 0.025f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t)
                            + 0.035f * pick
                            + spike;
                        block.setSample(0, i, sample);
                        block.setSample(1, i, sample * 0.91f);
                    }

                    amp->processBlock(block, midi);
                    cab->processBlock(block, midi);
                    chainMetrics.capture(block);

                    for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    {
                        const auto* data = block.getReadPointer(ch);
                        for (int i = 0; i < block.getNumSamples(); ++i)
                        {
                            if (havePrevious)
                                worstDelta = juce::jmax(worstDelta, (double) std::abs(data[i] - previousSample[(size_t) ch]));
                            previousSample[(size_t) ch] = data[i];
                        }
                    }
                    havePrevious = true;
                }

                return std::pair<P8AWindowMetrics, double> { chainMetrics, worstDelta };
            };

            const auto vintage = exerciseCabinet("Vintage 2x12", false);
            const auto modern = exerciseCabinet("Modern 4x12", true);
            const auto wrapper = exerciseCabinet("Cabinet", false);

            for (const auto& result : { std::pair<juce::String, std::pair<P8AWindowMetrics, double>> { "Vintage 2x12", vintage },
                                        { "Modern 4x12", modern },
                                        { "Cabinet", wrapper } })
            {
                const auto& cabinetName = result.first;
                const auto& metrics = result.second.first;
                expect(metrics.finite, "P8D " + cabinetName + " cabinet chain must remain finite");
                expect(metrics.peak <= (double) Nova::Config::HARD_ABS_LIMIT_LINEAR && metrics.dc() < 0.25,
                    "P8D " + cabinetName + " cabinet chain must stay bounded: " + p8aMetricsSummary(metrics));
                expect(metrics.clippedSamples < juce::jmax(1, metrics.sampleCount) / 2,
                    "P8D " + cabinetName + " cabinet chain should not become sustained clipped output: " + p8aMetricsSummary(metrics));
                expect(result.second.second < 12.0,
                    "P8D " + cabinetName + " cabinet bypass/automation transitions should remain bounded");
            }
        }

        beginTest("Processor switcher cycles through all three routing modes");
        {
            NOVAAudioProcessor processor;

            expectEquals((int)processor.getSwitcherMode(), (int)Nova::SwitcherMode::LineA_Only);

            processor.cycleSwitcher();
            expectEquals((int)processor.getSwitcherMode(), (int)Nova::SwitcherMode::Dual_Parallel);

            processor.cycleSwitcher();
            expectEquals((int)processor.getSwitcherMode(), (int)Nova::SwitcherMode::LineB_Only);

            processor.cycleSwitcher();
            expectEquals((int)processor.getSwitcherMode(), (int)Nova::SwitcherMode::LineA_Only);
        }

        beginTest("SessionStore command layer keeps canonical topology and runtime state");
        {
            SessionStore store;

            juce::AudioParameterBool engineOn(Nova::IDs::ENGINE_ON.toString(), "Engine", false);
            juce::AudioParameterChoice switchMode(Nova::IDs::SWITCH_MODE.toString(),
                "Switcher", juce::StringArray{ "Line A", "Line B", "Dual" }, 0);
            juce::AudioParameterFloat inputGain(Nova::IDs::INPUT_GAIN.toString(), "Input Gain", -60.0f, 24.0f, 0.0f);
            juce::AudioParameterFloat outputMix(Nova::IDs::OUTPUT_MIX.toString(), "Output Mix", 0.0f, 100.0f, 100.0f);

            SessionStore::ParameterBindings bindings;
            bindings.engineOn = &engineOn;
            bindings.switchMode = &switchMode;
            bindings.inputGain = &inputGain;
            bindings.outputMix = &outputMix;
            store.bindParameters(bindings);

            engineOn.setValueNotifyingHost(engineOn.convertTo0to1(true));
            inputGain.setValueNotifyingHost(inputGain.convertTo0to1(6.0f));
            outputMix.setValueNotifyingHost(outputMix.convertTo0to1(42.0f));

            expect(store.noteParameterValueChanged(Nova::IDs::ENGINE_ON.toString(), engineOn.convertTo0to1(true)));
            expect(store.noteParameterValueChanged(Nova::IDs::INPUT_GAIN.toString(), inputGain.convertTo0to1(6.0f)));
            expect(store.noteParameterValueChanged(Nova::IDs::OUTPUT_MIX.toString(), outputMix.convertTo0to1(42.0f)));
            store.syncStateFromBindingsNow();

            expect(store.isEngineEnabled(), "Engine flag should mirror host parameter state");
            expect(approximatelyEqual(store.getRuntimeGlobalParams().inputGainDb, 6.0f, 1.0e-3f),
                "Runtime cache should reflect the mirrored input gain");
            expect(approximatelyEqual(store.getRuntimeGlobalParams().outputMixRaw, 42.0f, 1.0e-3f),
                "Runtime cache should reflect the mirrored mix");

            auto addPre = store.applyCommand(SessionStore::Command::makeAddPedal(
                "Overdrive", Nova::ChainID::LineA, Nova::ZoneID::Pre, -1));
            auto addAmp1 = store.applyCommand(SessionStore::Command::makeAddPedal(
                "Classic Amp", Nova::ChainID::LineA, Nova::ZoneID::Amp, -1));
            auto addFx = store.applyCommand(SessionStore::Command::makeAddPedal(
                "Reverb", Nova::ChainID::LineA, Nova::ZoneID::FX, -1));
            auto addAmp2 = store.applyCommand(SessionStore::Command::makeAddPedal(
                "High Gain Amp", Nova::ChainID::LineA, Nova::ZoneID::Amp, -1));

            expect(addPre.changed && addAmp1.changed && addFx.changed && addAmp2.changed,
                "Typed session commands should mutate the store");

            const auto lineA = Nova::PluginStateModel::getLineTree(store.state(), Nova::ChainID::LineA);
            expectEquals(lineA.getNumChildren(), 3);
            expectEquals(lineA.getChild(0).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Overdrive"));
            expectEquals(lineA.getChild(1).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("High Gain Amp"));
            expectEquals(lineA.getChild(2).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Reverb"));
        }

        beginTest("NovaSettingsService editor settings round-trip and sanitize safely");
        {
            const auto settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-editor-settings-service-test.xml");
            settingsFile.deleteFile();

            auto missing = Nova::SettingsService::loadEditorSettingsFromFile(settingsFile);
            expect(!missing.loadedFromDisk && !missing.usedFallback,
                "Missing editor settings should fall back to defaults without marking corrupt fallback");
            expect(Nova::SettingsService::getBool(missing, Nova::SettingsService::Keys::showStats, false),
                "Default editor settings should show performance stats");
            expectEquals(Nova::SettingsService::getSwitcherModes(missing), 7);

            auto settings = Nova::SettingsService::makeDefaultEditorSettings();
            Nova::SettingsService::setString(settings, Nova::SettingsService::Keys::showStats, "not-a-bool");
            Nova::SettingsService::setString(settings, Nova::SettingsService::Keys::startupMode, "Mystery startup");
            Nova::SettingsService::setString(settings, Nova::SettingsService::Keys::tunerReference, "A = 999 Hz");
            Nova::SettingsService::setString(settings, Nova::SettingsService::Keys::libraryView, "Cloud");
            Nova::SettingsService::setString(settings, Nova::SettingsService::Keys::switcherShortcut, "-42");
            Nova::SettingsService::setString(settings, Nova::SettingsService::Keys::switcherModes, "1");
            Nova::SettingsService::setString(settings, "user.displayName", "Ada");

            expect(Nova::SettingsService::saveEditorSettingsToFile(settingsFile, settings),
                "Editor settings save should succeed");

            const auto restored = Nova::SettingsService::loadEditorSettingsFromFile(settingsFile);
            expect(restored.loadedFromDisk && !restored.usedFallback,
                "Valid editor settings should load from disk without corrupt fallback");
            expect(Nova::SettingsService::getBool(restored, Nova::SettingsService::Keys::showStats, false),
                "Invalid bool should sanitize to default");
            expectEquals(Nova::SettingsService::getString(restored,
                Nova::SettingsService::Keys::startupMode, {}),
                juce::String(Nova::SettingsService::StartupModes::rememberLastContext));
            expectEquals(Nova::SettingsService::parseTunerReference(restored), 440.0f);
            expectEquals(Nova::SettingsService::getString(restored,
                Nova::SettingsService::Keys::libraryView, {}),
                juce::String("All presets"));
            expectEquals(Nova::SettingsService::getSwitcherKeyCode(restored), juce::KeyPress::spaceKey);
            expectEquals(Nova::SettingsService::getSwitcherModes(restored), 7);
            expectEquals(Nova::SettingsService::getString(restored, "user.displayName", {}), juce::String("Ada"));

            settingsFile.replaceWithText("<NOVA_EDITOR_SETTINGS><SETTING");
            const auto corrupt = Nova::SettingsService::loadEditorSettingsFromFile(settingsFile);
            expect(corrupt.loadedFromDisk && corrupt.usedFallback,
                "Corrupt editor settings XML should fall back safely");
            expectEquals(Nova::SettingsService::getSwitcherModes(corrupt), 7);

            const auto invalidTarget = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-editor-settings-invalid-target");
            invalidTarget.deleteRecursively();
            invalidTarget.createDirectory();
            expect(!Nova::SettingsService::saveEditorSettingsToFile(invalidTarget, settings),
                "Editor settings persistence should report a failed write");
            invalidTarget.deleteRecursively();

            settingsFile.deleteFile();
        }

        beginTest("NovaSettingsService startup preset pointer helpers are explicit and bounded");
        {
            const auto pointerFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-startup-pointer-service-test.txt");
            const auto presetFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-startup-pointer-service-test.nova-preset");
            pointerFile.deleteFile();
            presetFile.deleteFile();
            presetFile.replaceWithText("placeholder");

            expect(Nova::SettingsService::writeStartupPresetPointer(pointerFile, presetFile),
                "Explicit startup pointer write should succeed for an existing preset file");

            juce::File resolved;
            expect(Nova::SettingsService::readStartupPresetPointer(pointerFile, resolved, false),
                "Existing startup pointer should resolve");
            expectEquals(resolved.getFullPathName(), presetFile.getFullPathName());

            presetFile.deleteFile();
            expect(!Nova::SettingsService::readStartupPresetPointer(pointerFile, resolved, true),
                "Invalid startup pointer should fail resolution");
            expect(!pointerFile.existsAsFile(),
                "Invalid startup pointer should be removed when requested");

            pointerFile.deleteFile();
        }

        beginTest("Editor settings drafts remain isolated until explicitly committed");
        {
            auto committed = Nova::SettingsService::makeDefaultEditorSettings();
            auto firstDraft = committed;
            auto secondDraft = committed;

            Nova::SettingsService::setBool(firstDraft,
                Nova::SettingsService::Keys::showStats,
                false);
            Nova::SettingsService::setString(secondDraft,
                Nova::SettingsService::Keys::startupMode,
                Nova::SettingsService::StartupModes::openCleanRig);

            expect(Nova::SettingsService::getBool(committed,
                Nova::SettingsService::Keys::showStats,
                false),
                "Editing one draft must not mutate committed settings");
            expect(Nova::SettingsService::getBool(secondDraft,
                Nova::SettingsService::Keys::showStats,
                false),
                "Editing one settings window must not mutate another window's draft");
            expectEquals(Nova::SettingsService::getString(firstDraft,
                Nova::SettingsService::Keys::startupMode,
                {}),
                juce::String(Nova::SettingsService::StartupModes::rememberLastContext));

            Nova::SettingsService::sanitize(firstDraft);
            committed = firstDraft;

            expect(!Nova::SettingsService::getBool(committed,
                Nova::SettingsService::Keys::showStats,
                true),
                "An explicit commit should publish the selected draft");
            expectEquals(Nova::SettingsService::getString(secondDraft,
                Nova::SettingsService::Keys::startupMode,
                {}),
                juce::String(Nova::SettingsService::StartupModes::openCleanRig));
        }

        beginTest("SessionPersistence saves and restores a canonical preset");
        {
            StartupPresetPointerGuard startupPointerGuard;
            constexpr const char* sentinelPointerText = "sentinel-startup-pointer";
            startupPointerGuard.pointerFile.getParentDirectory().createDirectory();
            startupPointerGuard.pointerFile.replaceWithText(sentinelPointerText);

            SessionStore store;
            store.applyCommand(SessionStore::Command::makeAddPedal(
                "Overdrive", Nova::ChainID::LineA, Nova::ZoneID::Pre, -1));
            store.applyCommand(SessionStore::Command::makeAddPedal(
                "Chorus", Nova::ChainID::LineB, Nova::ZoneID::FX, -1));

            AudioEngine sourceEngine;
            sourceEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            SessionPersistence::rebuildEngineFromState(sourceEngine, store.state());

            auto presetFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-session-persistence-test.nova-preset");
            presetFile.deleteFile();

            expect(SessionPersistence::savePresetToFile(presetFile, store, sourceEngine),
                "Preset save should succeed");
            expectEquals(startupPointerGuard.pointerFile.loadFileAsString(), juce::String(sentinelPointerText),
                "Saving a preset must not mutate the startup preset pointer");

            SessionStore restoredStore;
            AudioEngine restoredEngine;
            restoredEngine.prepare(kSampleRate, kBlockSize, 2, 2);

            expect(SessionPersistence::loadPresetFromFile(presetFile, restoredStore, restoredEngine),
                "Preset load should succeed");
            expectEquals(startupPointerGuard.pointerFile.loadFileAsString(), juce::String(sentinelPointerText),
                "Loading a preset must not mutate the startup preset pointer");

            const auto restoredLineA = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineA);
            const auto restoredLineB = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineB);
            expectEquals(restoredLineA.getNumChildren(), 1);
            expectEquals(restoredLineB.getNumChildren(), 1);
            expectEquals(restoredLineA.getChild(0).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Overdrive"));
            expectEquals(restoredLineB.getChild(0).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Chorus"));
            expectEquals((int)restoredEngine.getNodes(Nova::ChainID::LineA).size(), 1);
            expectEquals((int)restoredEngine.getNodes(Nova::ChainID::LineB).size(), 1);

            expect(Nova::SettingsService::writeStartupPresetPointer(startupPointerGuard.pointerFile, presetFile),
                "Explicit startup preset assignment should write the pointer");

            SessionStore startupStore;
            AudioEngine startupEngine;
            startupEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            expect(SessionPersistence::restoreStartupPresetIfAvailable(startupStore, startupEngine),
                "Existing explicit startup preset file should restore");
            expectEquals(startupPointerGuard.pointerFile.loadFileAsString(), presetFile.getFullPathName(),
                "Successful startup restore should preserve the explicit pointer");

            presetFile.deleteFile();
        }

        // ====================================================================
        // P7D - Preset / Session / Parameter Validation
        // Persistence and state robustness only; no DSP, routing, schema, preset
        // curation, or golden-baseline behavior is changed by these tests.
        // ====================================================================

        beginTest("P7D preset save-load-save remains canonical");
        {
            StartupPresetPointerGuard startupPointerGuard;

            auto sourceState = makeP7DState();
            auto settings = Nova::PluginStateModel::getSettingsTree(sourceState);
            settings.setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
            settings.setProperty(Nova::IDs::SWITCH_MODE, (int) Nova::SwitcherMode::Dual_Parallel, nullptr);
            settings.setProperty(Nova::IDs::INPUT_GAIN, 3.5f, nullptr);
            settings.setProperty(Nova::IDs::INPUT_GATE, -72.0f, nullptr);
            settings.setProperty(Nova::IDs::FORCE_MONO, true, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_VOL, -1.5f, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_LIMITER, -3.0f, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_MIX, 73.0f, nullptr);

            auto lineA = Nova::PluginStateModel::getLineTree(sourceState, Nova::ChainID::LineA);
            auto lineB = Nova::PluginStateModel::getLineTree(sourceState, Nova::ChainID::LineB);
            lineA.setProperty(Nova::IDs::MIXER_GAIN_A, 0.75f, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_PAN_A, -0.25f, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, 1.35f, nullptr);
            lineB.setProperty(Nova::IDs::MIXER_GAIN_B, 1.25f, nullptr);
            lineB.setProperty(Nova::IDs::MIXER_PAN_B, 0.2f, nullptr);
            lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, 0.85f, nullptr);

            appendP7DPedal(sourceState, Nova::ChainID::LineA, "Compressor", Nova::ZoneID::Pre, "p7d-a-pre", true);
            appendP7DPedal(sourceState, Nova::ChainID::LineA, "Classic Amp", Nova::ZoneID::Amp, "p7d-a-amp", true);
            appendP7DPedal(sourceState, Nova::ChainID::LineA, "Delay", Nova::ZoneID::FX, "p7d-a-fx", false);
            appendP7DPedal(sourceState, Nova::ChainID::LineA, "Cabinet", Nova::ZoneID::Cabinet, "p7d-a-cab", true);
            appendP7DPedal(sourceState, Nova::ChainID::LineB, "Wah", Nova::ZoneID::Pre, "p7d-b-pre", false);
            appendP7DPedal(sourceState, Nova::ChainID::LineB, "Clean Amp", Nova::ZoneID::Amp, "p7d-b-amp", true);
            appendP7DPedal(sourceState, Nova::ChainID::LineB, "Chorus", Nova::ZoneID::FX, "p7d-b-fx", true);
            appendP7DPedal(sourceState, Nova::ChainID::LineB, "Modern 4x12", Nova::ZoneID::Cabinet, "p7d-b-cab", false);
            Nova::PluginStateModel::canonicalizeStateTree(sourceState);

            SessionStore store;
            expect(store.applyCommand(SessionStore::Command::makeRestoreState(sourceState)).changed,
                "Canonical source state should restore into SessionStore");

            AudioEngine sourceEngine;
            sourceEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            SessionPersistence::rebuildEngineFromState(sourceEngine, store.state());
            expect(engineProcessesFiniteAfterP7DRestore(sourceEngine, store.getRuntimeGlobalParams(), store.isEngineEnabled()),
                "Source engine should process finite before save");

            const auto firstPreset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-p7d-roundtrip-first.nova-preset");
            const auto secondPreset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-p7d-roundtrip-second.nova-preset");
            firstPreset.deleteFile();
            secondPreset.deleteFile();

            expect(SessionPersistence::savePresetToFile(firstPreset, store, sourceEngine),
                "First preset save should succeed");

            SessionStore restoredStore;
            AudioEngine restoredEngine;
            restoredEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            expect(SessionPersistence::loadPresetFromFile(firstPreset, restoredStore, restoredEngine),
                "Preset load should succeed");
            expect(SessionPersistence::savePresetToFile(secondPreset, restoredStore, restoredEngine),
                "Second preset save should succeed");

            const auto firstState = readP7DValueTreeFile(firstPreset);
            const auto secondState = readP7DValueTreeFile(secondPreset);
            expect(firstState.isValid() && secondState.isValid(), "Both saved preset files should decode as ValueTrees");
            expectEquals(canonicalXmlForP7D(firstState), canonicalXmlForP7D(secondState),
                "save -> load -> save should be canonically identical");

            const auto restoredLineA = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineA);
            const auto restoredLineB = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineB);
            expectEquals(restoredLineA.getNumChildren(), 4);
            expectEquals(restoredLineB.getNumChildren(), 4);
            expectEquals(restoredLineA.getChild(2).getProperty(Nova::IDs::PEDAL_ID).toString(), juce::String("p7d-a-fx"));
            expect(!(bool)restoredLineA.getChild(2).getProperty(Nova::IDs::PEDAL_ENABLED),
                "Disabled pedal state should survive preset round-trip");
            expectEquals((int)restoredStore.getRuntimeGlobalParams().switchMode, (int)Nova::SwitcherMode::Dual_Parallel);
            expect(approximatelyEqual(restoredStore.getRuntimeGlobalParams().outputMixRaw, 73.0f, 1.0e-3f),
                "Global output mix should survive preset round-trip");

            firstPreset.deleteFile();
            secondPreset.deleteFile();
        }

        beginTest("P7D catalog presets round-trip every registered pedal");
        {
            StartupPresetPointerGuard startupPointerGuard;
            int entryIndex = 0;

            for (const auto& entry : Nova::PedalCatalog::entries())
            {
                auto state = makeP7DState();
                const auto requestedZone = entry.kind == Nova::PedalCatalog::Kind::Amplifier
                    ? Nova::ZoneID::Amp
                    : (entry.kind == Nova::PedalCatalog::Kind::Cabinet
                        ? Nova::ZoneID::Cabinet
                        : ((entryIndex % 2) == 0 ? Nova::ZoneID::Pre : Nova::ZoneID::FX));

                const auto pedalID = "p7d-catalog-" + juce::String(entryIndex);
                appendP7DPedal(state, Nova::ChainID::LineA, entry.typeID, requestedZone, pedalID, (entryIndex % 3) != 0);
                Nova::PluginStateModel::canonicalizeStateTree(state);

                SessionStore store;
                store.applyCommand(SessionStore::Command::makeRestoreState(state));

                AudioEngine sourceEngine;
                sourceEngine.prepare(kSampleRate, kBlockSize, 2, 2);
                SessionPersistence::rebuildEngineFromState(sourceEngine, store.state());

                const auto presetFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("nova-p7d-catalog-" + juce::String(entryIndex) + ".nova-preset");
                presetFile.deleteFile();

                expect(SessionPersistence::savePresetToFile(presetFile, store, sourceEngine),
                    juce::String(entry.typeID) + " catalog preset save should succeed");

                SessionStore restoredStore;
                AudioEngine restoredEngine;
                restoredEngine.prepare(kSampleRate, kBlockSize, 2, 2);
                expect(SessionPersistence::loadPresetFromFile(presetFile, restoredStore, restoredEngine),
                    juce::String(entry.typeID) + " catalog preset load should succeed");

                const auto line = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineA);
                expectEquals(line.getNumChildren(), 1);
                const auto pedal = line.getChild(0);
                expectEquals(pedal.getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String(entry.typeID));
                expectEquals(pedal.getProperty(Nova::IDs::PEDAL_ID).toString(), pedalID);
                expectEquals((int)pedal.getProperty(Nova::IDs::PEDAL_ZONE),
                    (int)Nova::PedalCatalog::enforceZone(entry.typeID, requestedZone));
                expect((bool)pedal.getProperty(Nova::IDs::PEDAL_ENABLED) == ((entryIndex % 3) != 0),
                    juce::String(entry.typeID) + " enabled state should survive preset round-trip");

                presetFile.deleteFile();
                ++entryIndex;
            }
        }

        beginTest("P7D chain/global preset round-trips routing modes and bypass state");
        {
            StartupPresetPointerGuard startupPointerGuard;

            for (const auto mode : { Nova::SwitcherMode::LineA_Only, Nova::SwitcherMode::LineB_Only, Nova::SwitcherMode::Dual_Parallel })
            {
                auto state = makeP7DState();
                auto settings = Nova::PluginStateModel::getSettingsTree(state);
                settings.setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
                settings.setProperty(Nova::IDs::SWITCH_MODE, (int) mode, nullptr);
                settings.setProperty(Nova::IDs::INPUT_GAIN, mode == Nova::SwitcherMode::LineB_Only ? 24.0f : -12.0f, nullptr);
                settings.setProperty(Nova::IDs::INPUT_GATE, mode == Nova::SwitcherMode::Dual_Parallel ? 0.0f : -100.0f, nullptr);
                settings.setProperty(Nova::IDs::OUTPUT_VOL, mode == Nova::SwitcherMode::LineA_Only ? -60.0f : 12.0f, nullptr);
                settings.setProperty(Nova::IDs::OUTPUT_LIMITER, mode == Nova::SwitcherMode::LineB_Only ? -12.0f : 0.0f, nullptr);
                settings.setProperty(Nova::IDs::OUTPUT_MIX, mode == Nova::SwitcherMode::Dual_Parallel ? 50.0f : 100.0f, nullptr);

                auto lineA = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineA);
                auto lineB = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineB);
                lineA.setProperty(Nova::IDs::MIXER_GAIN_A, 0.0f, nullptr);
                lineA.setProperty(Nova::IDs::MIXER_PAN_A, -1.0f, nullptr);
                lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, 2.0f, nullptr);
                lineB.setProperty(Nova::IDs::MIXER_GAIN_B, 2.0f, nullptr);
                lineB.setProperty(Nova::IDs::MIXER_PAN_B, 1.0f, nullptr);
                lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, 0.0f, nullptr);

                appendP7DPedal(state, Nova::ChainID::LineA, "Noise Gate", Nova::ZoneID::Pre, "p7d-mode-a-pre", true);
                appendP7DPedal(state, Nova::ChainID::LineA, "Boutique Amp", Nova::ZoneID::Amp, "p7d-mode-a-amp", false);
                appendP7DPedal(state, Nova::ChainID::LineA, "Phaser", Nova::ZoneID::FX, "p7d-mode-a-fx", true);
                appendP7DPedal(state, Nova::ChainID::LineA, "Vintage 2x12", Nova::ZoneID::Cabinet, "p7d-mode-a-cab", false);
                appendP7DPedal(state, Nova::ChainID::LineB, "EQ", Nova::ZoneID::Pre, "p7d-mode-b-pre", false);
                appendP7DPedal(state, Nova::ChainID::LineB, "High Gain Amp", Nova::ZoneID::Amp, "p7d-mode-b-amp", true);
                appendP7DPedal(state, Nova::ChainID::LineB, "Flanger", Nova::ZoneID::FX, "p7d-mode-b-fx", false);
                appendP7DPedal(state, Nova::ChainID::LineB, "Cabinet", Nova::ZoneID::Cabinet, "p7d-mode-b-cab", true);
                Nova::PluginStateModel::canonicalizeStateTree(state);

                SessionStore store;
                store.applyCommand(SessionStore::Command::makeRestoreState(state));
                AudioEngine engine;
                engine.prepare(kSampleRate, kBlockSize, 2, 2);
                SessionPersistence::rebuildEngineFromState(engine, store.state());

                const auto presetFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("nova-p7d-mode-" + juce::String((int) mode) + ".nova-preset");
                presetFile.deleteFile();
                expect(SessionPersistence::savePresetToFile(presetFile, store, engine),
                    "Mode preset save should succeed");

                SessionStore restoredStore;
                AudioEngine restoredEngine;
                restoredEngine.prepare(kSampleRate, kBlockSize, 2, 2);
                expect(SessionPersistence::loadPresetFromFile(presetFile, restoredStore, restoredEngine),
                    "Mode preset load should succeed");

                const auto restoredParams = restoredStore.getRuntimeGlobalParams();
                expectEquals(restoredParams.switchMode, (int) mode);
                expect(approximatelyEqual(restoredParams.gainA, 0.0f, 1.0e-3f), "Line A gain minimum should round-trip");
                expect(approximatelyEqual(restoredParams.panA, -1.0f, 1.0e-3f), "Line A pan minimum should round-trip");
                expect(approximatelyEqual(restoredParams.widthA, 2.0f, 1.0e-3f), "Line A width maximum should round-trip");
                expect(approximatelyEqual(restoredParams.gainB, 2.0f, 1.0e-3f), "Line B gain maximum should round-trip");
                expect(approximatelyEqual(restoredParams.panB, 1.0f, 1.0e-3f), "Line B pan maximum should round-trip");
                expect(approximatelyEqual(restoredParams.widthB, 0.0f, 1.0e-3f), "Line B width minimum should round-trip");

                const auto restoredLineA = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineA);
                const auto restoredLineB = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineB);
                expectEquals(restoredLineA.getChild(0).getProperty(Nova::IDs::PEDAL_ID).toString(), juce::String("p7d-mode-a-pre"));
                expectEquals(restoredLineA.getChild(1).getProperty(Nova::IDs::PEDAL_ID).toString(), juce::String("p7d-mode-a-amp"));
                expectEquals(restoredLineA.getChild(2).getProperty(Nova::IDs::PEDAL_ID).toString(), juce::String("p7d-mode-a-fx"));
                expectEquals(restoredLineA.getChild(3).getProperty(Nova::IDs::PEDAL_ID).toString(), juce::String("p7d-mode-a-cab"));
                expect(!(bool)restoredLineA.getChild(1).getProperty(Nova::IDs::PEDAL_ENABLED),
                    "Line A amp bypass state should round-trip");
                expect(!(bool)restoredLineB.getChild(0).getProperty(Nova::IDs::PEDAL_ENABLED),
                    "Line B pre bypass state should round-trip");
                expect(!(bool)restoredLineB.getChild(2).getProperty(Nova::IDs::PEDAL_ENABLED),
                    "Line B FX bypass state should round-trip");
                expect(engineProcessesFiniteAfterP7DRestore(restoredEngine, restoredParams, restoredStore.isEngineEnabled()),
                    "Restored mode engine should process finite");

                presetFile.deleteFile();
            }
        }

        beginTest("P7D schema canonicalization rejects unknowns and clamps topology");
        {
            juce::ValueTree state(Nova::IDs::MAIN_STATE);
            state.setProperty(Nova::IDs::STATE_SCHEMA_VERSION, 0, nullptr);
            auto settings = juce::ValueTree(Nova::IDs::SETTINGS);
            settings.setProperty("p7dExtraSettingsField", "ignored", nullptr);
            state.appendChild(settings, nullptr);

            juce::ValueTree lineA(Nova::IDs::LINE_A);
            lineA.setProperty("p7dExtraLineField", 123, nullptr);
            lineA.appendChild(juce::ValueTree("P7D_UNKNOWN_CHILD"), nullptr);
            lineA.appendChild(makeP7DPedal("Future Pedal", Nova::ZoneID::FX, "p7d-unknown", true), nullptr);

            auto legacyWah = makeP7DPedal("Auto Wah", Nova::ZoneID::Cabinet, "", true);
            legacyWah.removeProperty(Nova::IDs::PEDAL_ID, nullptr);
            lineA.appendChild(legacyWah, nullptr);

            lineA.appendChild(makeP7DPedal("Classic Amp", Nova::ZoneID::Pre, "p7d-amp-kept", true), nullptr);
            lineA.appendChild(makeP7DPedal("High Gain Amp", Nova::ZoneID::Amp, "p7d-amp-dropped", true), nullptr);
            lineA.appendChild(makeP7DPedal("Cabinet", Nova::ZoneID::FX, "p7d-cab-kept", true), nullptr);
            lineA.appendChild(makeP7DPedal("Modern 4x12", Nova::ZoneID::Cabinet, "p7d-cab-dropped", true), nullptr);

            for (int i = 0; i < Nova::Config::MAX_PEDALS_PER_FLEX_ZONE + 4; ++i)
                lineA.appendChild(makeP7DPedal("Overdrive", Nova::ZoneID::Pre,
                    "p7d-pre-" + juce::String(i), true), nullptr);

            state.appendChild(lineA, nullptr);
            state.appendChild(juce::ValueTree(Nova::IDs::LINE_B), nullptr);
            Nova::PluginStateModel::canonicalizeStateTree(state);

            expectEquals(Nova::PluginStateModel::getStateSchemaVersion(state), Nova::Config::STATE_SCHEMA_VERSION);
            expectEquals(Nova::PluginStateModel::countPedalsInZone(state, Nova::ChainID::LineA, Nova::ZoneID::Pre),
                Nova::Config::MAX_PEDALS_PER_FLEX_ZONE);
            expectEquals(Nova::PluginStateModel::countPedalsInZone(state, Nova::ChainID::LineA, Nova::ZoneID::Amp), 1);
            expectEquals(Nova::PluginStateModel::countPedalsInZone(state, Nova::ChainID::LineA, Nova::ZoneID::Cabinet), 1);

            const auto canonicalLineA = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineA);
            expectEquals(canonicalLineA.getChild(0).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Wah"));
            expectEquals((int)canonicalLineA.getChild(0).getProperty(Nova::IDs::PEDAL_ZONE), (int)Nova::ZoneID::Pre);
            expect(canonicalLineA.getChild(0).getProperty(Nova::IDs::PEDAL_ID).toString().isNotEmpty(),
                "Missing pedal IDs should be generated during canonicalization");

            for (int i = 0; i < canonicalLineA.getNumChildren(); ++i)
            {
                const auto pedal = canonicalLineA.getChild(i);
                expect(pedal.hasType(Nova::IDs::PEDAL), "Unknown child nodes should be removed from lines");
                expect(pedal.getProperty(Nova::IDs::PEDAL_TYPE).toString() != "Future Pedal",
                    "Unknown pedal types should be skipped safely");
                expect(pedal.getProperty(Nova::IDs::PEDAL_ID).toString() != "p7d-amp-dropped",
                    "Duplicate amps should be skipped safely");
                expect(pedal.getProperty(Nova::IDs::PEDAL_ID).toString() != "p7d-cab-dropped",
                    "Duplicate cabinets should be skipped safely");
            }

            SessionStore emptyStore;
            expect(emptyStore.applyCommand(SessionStore::Command::makeRestoreState(juce::ValueTree(Nova::IDs::MAIN_STATE))).changed,
                "Empty root state should restore through defaults");
            AudioEngine emptyEngine;
            emptyEngine.prepare(kSampleRate, kBlockSize, 2, 2);
            SessionPersistence::rebuildEngineFromState(emptyEngine, emptyStore.state());
            expect(engineProcessesFiniteAfterP7DRestore(emptyEngine, emptyStore.getRuntimeGlobalParams(), emptyStore.isEngineEnabled()),
                "Engine should remain usable after empty-state restore");
        }

        beginTest("P7D parameter boundary restore clamps unsafe values");
        {
            auto state = makeP7DState();
            auto settings = Nova::PluginStateModel::getSettingsTree(state);
            settings.setProperty(Nova::IDs::ENGINE_ON, "bad-bool", nullptr);
            settings.setProperty(Nova::IDs::SWITCH_MODE, 999, nullptr);
            settings.setProperty(Nova::IDs::INPUT_GAIN, std::numeric_limits<double>::infinity(), nullptr);
            settings.setProperty(Nova::IDs::INPUT_GATE, -std::numeric_limits<double>::infinity(), nullptr);
            settings.setProperty(Nova::IDs::FORCE_MONO, 1, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_VOL, 99.0, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_LIMITER, -99.0, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_MIX, std::numeric_limits<double>::quiet_NaN(), nullptr);

            auto lineA = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineA);
            auto lineB = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineB);
            lineA.setProperty(Nova::IDs::MIXER_GAIN_A, -4.0, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_PAN_A, 4.0, nullptr);
            lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, std::numeric_limits<double>::quiet_NaN(), nullptr);
            lineB.setProperty(Nova::IDs::MIXER_GAIN_B, std::numeric_limits<double>::infinity(), nullptr);
            lineB.setProperty(Nova::IDs::MIXER_PAN_B, "bad-pan", nullptr);
            lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, 999.0, nullptr);

            auto badPedal = makeP7DPedal("Delay", Nova::ZoneID::FX, "p7d-bad-enabled", true);
            badPedal.setProperty(Nova::IDs::PEDAL_ENABLED, "bad-enabled", nullptr);
            Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineA).appendChild(badPedal, nullptr);

            SessionStore store;
            expect(store.applyCommand(SessionStore::Command::makeRestoreState(state)).changed,
                "Boundary-corrupt state should restore after canonicalization");

            const auto restoredSettings = Nova::PluginStateModel::getSettingsTree(store.state());
            const auto restoredLineA = Nova::PluginStateModel::getLineTree(store.state(), Nova::ChainID::LineA);
            const auto restoredLineB = Nova::PluginStateModel::getLineTree(store.state(), Nova::ChainID::LineB);
            expect(!(bool)restoredSettings.getProperty(Nova::IDs::ENGINE_ON),
                "Invalid engineOn strings should fall back safely");
            expectEquals((int)restoredSettings.getProperty(Nova::IDs::SWITCH_MODE), (int)Nova::SwitcherMode::Dual_Parallel);
            expect(approximatelyEqual((float)restoredSettings.getProperty(Nova::IDs::INPUT_GAIN), 0.0f, 1.0e-3f),
                "Non-finite input gain should fall back safely");
            expect(approximatelyEqual((float)restoredSettings.getProperty(Nova::IDs::INPUT_GATE), -100.0f, 1.0e-3f),
                "Non-finite gate threshold should fall back safely");
            expect((bool)restoredSettings.getProperty(Nova::IDs::FORCE_MONO),
                "Numeric forceMono values should sanitize to true");
            expect(approximatelyEqual((float)restoredSettings.getProperty(Nova::IDs::OUTPUT_VOL), 12.0f, 1.0e-3f),
                "Output volume should clamp to parameter range");
            expect(approximatelyEqual((float)restoredSettings.getProperty(Nova::IDs::OUTPUT_LIMITER), -12.0f, 1.0e-3f),
                "Limiter threshold should clamp to parameter range");
            expect(approximatelyEqual((float)restoredSettings.getProperty(Nova::IDs::OUTPUT_MIX), 100.0f, 1.0e-3f),
                "Non-finite output mix should fall back safely");
            expect(approximatelyEqual((float)restoredLineA.getProperty(Nova::IDs::MIXER_GAIN_A), 0.0f, 1.0e-3f),
                "Line A gain should clamp low");
            expect(approximatelyEqual((float)restoredLineA.getProperty(Nova::IDs::MIXER_PAN_A), 1.0f, 1.0e-3f),
                "Line A pan should clamp high");
            expect(approximatelyEqual((float)restoredLineA.getProperty(Nova::IDs::MIXER_WIDTH_A), 1.0f, 1.0e-3f),
                "Line A non-finite width should fall back");
            expect(approximatelyEqual((float)restoredLineB.getProperty(Nova::IDs::MIXER_GAIN_B), 1.0f, 1.0e-3f),
                "Line B non-finite gain should fall back");
            expect(approximatelyEqual((float)restoredLineB.getProperty(Nova::IDs::MIXER_PAN_B), 0.0f, 1.0e-3f),
                "Line B invalid pan string should fall back");
            expect(approximatelyEqual((float)restoredLineB.getProperty(Nova::IDs::MIXER_WIDTH_B), 2.0f, 1.0e-3f),
                "Line B width should clamp high");
            expect((bool)restoredLineA.getChild(0).getProperty(Nova::IDs::PEDAL_ENABLED),
                "Invalid pedal enabled strings should fall back safely");

            const auto runtime = store.getRuntimeGlobalParams();
            expect(std::isfinite(runtime.inputGainDb) && std::isfinite(runtime.outputMixRaw)
                && std::isfinite(runtime.gainA) && std::isfinite(runtime.widthB),
                "Runtime cache should contain only finite restored values");

            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            SessionPersistence::rebuildEngineFromState(engine, store.state());
            expect(engineProcessesFiniteAfterP7DRestore(engine, runtime, store.isEngineEnabled()),
                "Boundary-restored engine should process finite audio");
        }

        beginTest("P7D pedal state payload restore rejects corrupt payloads safely");
        {
            struct PayloadCase
            {
                const char* name;
                juce::String payload;
            };

            const std::array<PayloadCase, 6> payloads{ {
                { "corrupt base64", "not-valid-base64!!" },
                { "decoded too small", base64PayloadWithSize(1, 0x11) },
                { "invalid xml bytes", base64PayloadWithSize(128, 0x33) },
                { "decoded too large", base64PayloadWithSize(2 * 1024 * 1024 + 8, 0x55) },
                { "empty payload", juce::String() },
                { "valid payload with unknown parameter", validUnknownParameterPayloadForP7D() }
            } };

            for (const auto& payloadCase : payloads)
            {
                auto state = makeP7DState();
                appendP7DPedal(state, Nova::ChainID::LineA, "Overdrive", Nova::ZoneID::Pre,
                    "p7d-payload-" + juce::String(payloadCase.name), true);

                auto line = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineA);
                line.getChild(0).setProperty(Nova::IDs::PEDAL_STATE, payloadCase.payload, nullptr);
                Nova::PluginStateModel::canonicalizeStateTree(state);

                SessionStore store;
                store.applyCommand(SessionStore::Command::makeRestoreState(state));

                AudioEngine engine;
                engine.prepare(kSampleRate, kBlockSize, 2, 2);
                SessionPersistence::rebuildEngineFromState(engine, store.state());

                expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), 1,
                    juce::String(payloadCase.name) + " should not invalidate the graph");
                expect(engineProcessesFiniteAfterP7DRestore(engine, store.getRuntimeGlobalParams(), true),
                    juce::String(payloadCase.name) + " should leave processing finite");
            }
        }

        beginTest("P7D corrupt session recovery leaves engine processable");
        {
            StartupPresetPointerGuard startupPointerGuard;

            SessionStore store;
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);

            juce::ValueTree partialState(Nova::IDs::MAIN_STATE);
            auto partialSettings = juce::ValueTree(Nova::IDs::SETTINGS);
            partialSettings.setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
            partialState.appendChild(partialSettings, nullptr);
            expect(store.applyCommand(SessionStore::Command::makeRestoreState(partialState)).changed,
                "Partial state should restore through structural defaults");
            SessionPersistence::rebuildEngineFromState(engine, store.state());
            expect(engineProcessesFiniteAfterP7DRestore(engine, store.getRuntimeGlobalParams(), store.isEngineEnabled()),
                "Partial restore followed by prepare/process should stay finite");

            const auto corruptPreset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("nova-p7d-corrupt-session.nova-preset");
            const uint8_t corruptBytes[] = { 0x4e, 0x4f, 0x56, 0x41, 0x00, 0x13, 0xff };
            corruptPreset.replaceWithData(corruptBytes, sizeof(corruptBytes));
            expect(!SessionPersistence::loadPresetFromFile(corruptPreset, store, engine),
                "Corrupt preset files should be rejected without throwing");
            expect(engineProcessesFiniteAfterP7DRestore(engine, store.getRuntimeGlobalParams(), store.isEngineEnabled()),
                "Engine should stay processable after rejected corrupt preset");

            startupPointerGuard.pointerFile.getParentDirectory().createDirectory();
            startupPointerGuard.pointerFile.replaceWithText(corruptPreset.getFullPathName());
            expect(!SessionPersistence::restoreStartupPresetIfAvailable(store, engine),
                "Corrupt startup preset should fail cleanly");
            expect(!startupPointerGuard.pointerFile.existsAsFile(),
                "Failed startup restore should clear the bad startup pointer");
            expect(engineProcessesFiniteAfterP7DRestore(engine, store.getRuntimeGlobalParams(), store.isEngineEnabled()),
                "Engine should stay processable after corrupt startup recovery");

            NOVAAudioProcessor processor;
            processor.prepareToPlay(kSampleRate, kBlockSize);
            const uint8_t hostBytes[] = { 0x01, 0x02, 0x03, 0x04, 0x80, 0x00 };
            processor.setStateInformation(hostBytes, (int)sizeof(hostBytes));
            juce::AudioBuffer<float> hostBuffer(2, kBlockSize);
            juce::MidiBuffer midi;
            for (int ch = 0; ch < hostBuffer.getNumChannels(); ++ch)
                for (int i = 0; i < hostBuffer.getNumSamples(); ++i)
                    hostBuffer.setSample(ch, i, static_cast<float>(0.05 * std::sin(2.0 * juce::MathConstants<double>::pi
                        * 330.0 * (double)i / kSampleRate)));
            processor.processBlock(hostBuffer, midi);
            expect(bufferHasOnlyFiniteSamples(hostBuffer),
                "Host corrupt state restore should leave plugin processing finite");
            processor.releaseResources();

            auto offState = makeP7DState();
            Nova::PluginStateModel::getSettingsTree(offState).setProperty(Nova::IDs::ENGINE_ON, false, nullptr);
            appendP7DPedal(offState, Nova::ChainID::LineA, "Boost", Nova::ZoneID::Pre, "p7d-off-on", true);
            store.applyCommand(SessionStore::Command::makeRestoreState(offState));
            SessionPersistence::rebuildEngineFromState(engine, store.state());
            expect(engineProcessesFiniteAfterP7DRestore(engine, store.getRuntimeGlobalParams(), false),
                "Restore while engine is off should stay processable");

            auto onState = offState.createCopy();
            Nova::PluginStateModel::getSettingsTree(onState).setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
            store.applyCommand(SessionStore::Command::makeRestoreState(onState));
            SessionPersistence::rebuildEngineFromState(engine, store.state());
            expect(engineProcessesFiniteAfterP7DRestore(engine, store.getRuntimeGlobalParams(), true),
                "Restore after engine re-enable should stay processable");

            corruptPreset.deleteFile();
        }

        beginTest("P7E host state get/set survives corrupt and repeated engine toggles");
        {
            NOVAAudioProcessor processor;
            processor.prepareToPlay(kSampleRate, kBlockSize);

            const uint8_t corruptHostState[] = { 0x7f, 0x45, 0x00, 0x10, 0xff, 0x01, 0x02 };
            processor.setStateInformation(corruptHostState, (int)sizeof(corruptHostState));

            juce::MemoryBlock cleanAfterCorrupt;
            processor.getStateInformation(cleanAfterCorrupt);
            auto cleanTree = juce::ValueTree::readFromData(cleanAfterCorrupt.getData(), cleanAfterCorrupt.getSize());
            expect(cleanTree.isValid() && cleanTree.hasType(Nova::IDs::MAIN_STATE),
                "getStateInformation after corrupt host restore should produce a valid main state");
            expectEquals(Nova::PluginStateModel::getStateSchemaVersion(cleanTree), Nova::Config::STATE_SCHEMA_VERSION);

            auto makeHostStateBlock = [](bool engineOn)
            {
                auto state = makeP7DState();
                auto settings = Nova::PluginStateModel::getSettingsTree(state);
                settings.setProperty(Nova::IDs::ENGINE_ON, engineOn, nullptr);
                settings.setProperty(Nova::IDs::SWITCH_MODE, (int)Nova::SwitcherMode::LineA_Only, nullptr);
                appendP7DPedal(state, Nova::ChainID::LineA, "Boost", Nova::ZoneID::Pre,
                    engineOn ? "p7e-host-on" : "p7e-host-off", true);
                Nova::PluginStateModel::canonicalizeStateTree(state);

                juce::MemoryBlock block;
                juce::MemoryOutputStream stream(block, false);
                state.writeToStream(stream);
                return block;
            };

            const auto offState = makeHostStateBlock(false);
            const auto onState = makeHostStateBlock(true);

            for (int i = 0; i < 4; ++i)
            {
                const auto& block = (i % 2 == 0) ? offState : onState;
                processor.setStateInformation(block.getData(), (int)block.getSize());

                juce::AudioBuffer<float> buffer(2, kBlockSize);
                juce::MidiBuffer midi;
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                        buffer.setSample(ch, sample, static_cast<float>(0.04 * std::sin(2.0 * juce::MathConstants<double>::pi
                            * 180.0 * (double)(i * kBlockSize + sample) / kSampleRate)));

                processor.processBlock(buffer, midi);
                expect(bufferHasOnlyFiniteSamples(buffer),
                    "Repeated host state restore while toggling engine state should process finite audio");

                juce::MemoryBlock roundTripState;
                processor.getStateInformation(roundTripState);
                auto roundTripTree = juce::ValueTree::readFromData(roundTripState.getData(), roundTripState.getSize());
                expect(roundTripTree.isValid() && roundTripTree.hasType(Nova::IDs::MAIN_STATE),
                    "Repeated host state restore should remain serializable");
                expectEquals(Nova::PluginStateModel::getStateSchemaVersion(roundTripTree), Nova::Config::STATE_SCHEMA_VERSION);
            }

            processor.releaseResources();
        }

        // ====================================================================
        // P7C - Allocation Fallback and Feedback Stress Closure
        // Stress coverage for the audit P1 items: DC accumulation, NaN/Inf
        // sanitization, high peaks, and max-feedback runaway on Delay / Flanger / Reverb.
        // Phaser DC is already covered above; Reverb DC is also covered above.
        // ====================================================================

        beginTest("DelayPedal feedback loop rejects DC accumulation under sustained bias");
        {
            DelayPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.85f));
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(180.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(7000.0f));
            pedal.lowCutParam->setValueNotifyingHost(pedal.lowCutParam->convertTo0to1(80.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 4.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) kSampleRate;
                const float sample = 0.10f * std::sin(phase) + 0.10f;
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderDelayOutput(pedal, input, kBlockSize);
            const int lateStart = (int) (kSampleRate * 2.5);
            const int lateLength = (int) (kSampleRate * 1.0);
            const double leftDc = std::abs(computeChannelMean(output, 0, lateStart, lateLength));
            const double rightDc = std::abs(computeChannelMean(output, 1, lateStart, lateLength));

            expect(bufferHasOnlyFiniteSamples(output), "Biased delay render must remain finite");
            expect(leftDc < 0.02 && rightDc < 0.02,
                "Delay feedback path should keep the late wet signal centered; leftDc="
                    + juce::String(leftDc, 8)
                    + " rightDc=" + juce::String(rightDc, 8));
        }

        beginTest("DelayPedal max feedback under sustained input stays bounded");
        {
            DelayPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.97f));
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(135.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.duckParam->setValueNotifyingHost(pedal.duckParam->convertTo0to1(0.0f));

            const int totalSamples = (int) (kSampleRate * 5.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 196.0f * (float) i / (float) kSampleRate;
                const float sample = 0.30f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderDelayOutput(pedal, input, kBlockSize);
            const double peak = computeBufferPeak(output, 0, output.getNumSamples());

            expect(bufferHasOnlyFiniteSamples(output), "Max-feedback delay must remain finite");
            expect(peak < 8.0, "Max-feedback delay should respect the safety ceiling");
        }

        beginTest("DelayPedal sanitizes NaN/Inf input under aggressive feedback");
        {
            DelayPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.92f));
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(220.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.6f));

            const int totalSamples = kBlockSize * 24;
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float clean = 0.08f * std::sin(juce::MathConstants<float>::twoPi * 220.0f
                    * (float) i / (float) kSampleRate);
                const float v = (i % 17 == 0) ? std::numeric_limits<float>::quiet_NaN()
                              : (i % 23 == 0) ? std::numeric_limits<float>::infinity()
                              : (i % 29 == 0) ? -std::numeric_limits<float>::infinity()
                              : clean;
                input.setSample(0, i, v);
                input.setSample(1, i, v);
            }

            const auto output = renderDelayOutput(pedal, input, kBlockSize);
            expect(bufferHasOnlyFiniteSamples(output), "Delay must scrub NaN/Inf inputs to a finite output");
        }

        beginTest("DelayPedal near-silence feedback tail remains finite");
        {
            DelayPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.94f));
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(160.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 2.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            input.setSample(0, 0, 1.0e-20f);
            input.setSample(1, 0, -1.0e-20f);

            const auto output = renderDelayOutput(pedal, input, kBlockSize);
            const double peak = computeBufferPeak(output, 0, output.getNumSamples());

            expect(bufferHasOnlyFiniteSamples(output), "Near-silence delay tail must remain finite");
            expect(peak < 1.0e-12, "Near-silence delay tail should stay effectively silent");
        }

        beginTest("DelayPedal high peak input under feedback stays bounded");
        {
            DelayPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.96f));
            pedal.timeParam->setValueNotifyingHost(pedal.timeParam->convertTo0to1(95.0f));
            pedal.toneParam->setValueNotifyingHost(pedal.toneParam->convertTo0to1(6500.0f));
            pedal.lowCutParam->setValueNotifyingHost(pedal.lowCutParam->convertTo0to1(120.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));
            pedal.duckParam->setValueNotifyingHost(pedal.duckParam->convertTo0to1(0.0f));

            const int totalSamples = (int) (kSampleRate * 4.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 329.63f * (float) i / (float) kSampleRate;
                const float tone = 0.08f * std::sin(phase);
                const float spike = (i % 397 == 0) ? (i % 794 == 0 ? 3.5f : -3.5f) : 0.0f;
                const float sample = tone + spike;
                input.setSample(0, i, sample);
                input.setSample(1, i, -sample);
            }

            const auto output = renderDelayOutput(pedal, input, kBlockSize);
            const double peak = computeBufferPeak(output, 0, output.getNumSamples());
            const double lateRms = computeWindowRms(output, totalSamples - (int) kSampleRate, (int) kSampleRate);

            expect(bufferHasOnlyFiniteSamples(output), "High-peak delay render must remain finite");
            expect(peak < 8.0, "High-peak delay should respect the safety ceiling");
            expect(lateRms < 2.0, "High-peak delay should not sustain near-clip energy");
        }

        beginTest("FlangerPedal feedback loop rejects DC accumulation under sustained bias");
        {
            FlangerPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 0));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.40f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.95f));
            pedal.manualParam->setValueNotifyingHost(pedal.manualParam->convertTo0to1(0.42f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.85f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.95f));

            const int totalSamples = (int) (kSampleRate * 3.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) kSampleRate;
                const float sample = 0.10f * std::sin(phase) + 0.10f;
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderFlangerOutput(pedal, input, kBlockSize);
            const int lateStart = (int) (kSampleRate * 1.8);
            const int lateLength = (int) (kSampleRate * 1.0);
            const double leftDc = std::abs(computeChannelMean(output, 0, lateStart, lateLength));
            const double rightDc = std::abs(computeChannelMean(output, 1, lateStart, lateLength));

            expect(bufferHasOnlyFiniteSamples(output), "Biased flanger render must remain finite");
            expect(leftDc < 0.02 && rightDc < 0.02,
                "Flanger feedback path should keep the late wet signal centered");
        }

        beginTest("FlangerPedal max feedback under sustained input stays bounded");
        {
            FlangerPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 0));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.32f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.92f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.95f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.95f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 4.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 246.94f * (float) i / (float) kSampleRate;
                const float sample = 0.32f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderFlangerOutput(pedal, input, kBlockSize);
            const double peak = computeBufferPeak(output, 0, output.getNumSamples());

            expect(bufferHasOnlyFiniteSamples(output), "Max-feedback flanger must remain finite");
            expect(peak < 8.0, "Max-feedback flanger should respect the safety ceiling");
        }

        beginTest("FlangerPedal near-silence feedback tail remains finite");
        {
            FlangerPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.92f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.88f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 1.5);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            input.setSample(0, 0, 1.0e-20f);
            input.setSample(1, 0, 1.0e-20f);

            const auto output = renderFlangerOutput(pedal, input, kBlockSize);
            const double peak = computeBufferPeak(output, 0, output.getNumSamples());

            expect(bufferHasOnlyFiniteSamples(output), "Near-silence flanger tail must remain finite");
            expect(peak < 1.0e-12, "Near-silence flanger tail should stay effectively silent");
        }

        beginTest("FlangerPedal sanitizes NaN/Inf input under aggressive feedback");
        {
            FlangerPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.90f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.85f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.65f));

            const int totalSamples = kBlockSize * 24;
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float clean = 0.08f * std::sin(juce::MathConstants<float>::twoPi * 246.94f
                    * (float) i / (float) kSampleRate);
                const float v = (i % 17 == 0) ? std::numeric_limits<float>::quiet_NaN()
                              : (i % 23 == 0) ? std::numeric_limits<float>::infinity()
                              : (i % 29 == 0) ? -std::numeric_limits<float>::infinity()
                              : clean;
                input.setSample(0, i, v);
                input.setSample(1, i, v);
            }

            const auto output = renderFlangerOutput(pedal, input, kBlockSize);
            expect(bufferHasOnlyFiniteSamples(output), "Flanger must scrub NaN/Inf inputs to a finite output");
        }

        beginTest("FlangerPedal high peak input under feedback stays bounded");
        {
            FlangerPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 0));
            pedal.rateParam->setValueNotifyingHost(pedal.rateParam->convertTo0to1(0.28f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.96f));
            pedal.manualParam->setValueNotifyingHost(pedal.manualParam->convertTo0to1(0.36f));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.92f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.90f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 3.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 277.18f * (float) i / (float) kSampleRate;
                const float tone = 0.08f * std::sin(phase);
                const float spike = (i % 181 == 0) ? (i % 362 == 0 ? 3.0f : -3.0f) : 0.0f;
                const float sample = tone + spike;
                input.setSample(0, i, sample);
                input.setSample(1, i, sample * 0.75f);
            }

            const auto output = renderFlangerOutput(pedal, input, kBlockSize);
            const double peak = computeBufferPeak(output, 0, output.getNumSamples());
            const double lateRms = computeWindowRms(output, totalSamples - (int) kSampleRate, (int) kSampleRate);

            expect(bufferHasOnlyFiniteSamples(output), "High-peak flanger render must remain finite");
            expect(peak < 8.0, "High-peak flanger should respect the safety ceiling");
            expect(lateRms < 2.0, "High-peak flanger should not sustain near-clip energy");
        }

        beginTest("ChorusPedal sanitizes NaN/Inf input and recovers to finite output");
        {
            ChorusPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.92f));
            pedal.widthParam->setValueNotifyingHost(pedal.widthParam->convertTo0to1(0.95f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.70f));

            juce::AudioBuffer<float> dirty(2, kBlockSize * 16);
            dirty.clear();
            for (int i = 0; i < dirty.getNumSamples(); ++i)
            {
                const float clean = 0.08f * std::sin(juce::MathConstants<float>::twoPi * 220.0f
                    * (float) i / (float) kSampleRate);
                const float v = (i % 13 == 0) ? std::numeric_limits<float>::quiet_NaN()
                              : (i % 19 == 0) ? std::numeric_limits<float>::infinity()
                              : (i % 31 == 0) ? -std::numeric_limits<float>::infinity()
                              : clean;
                dirty.setSample(0, i, v);
                dirty.setSample(1, i, -v);
            }

            const auto dirtyOutput = renderChorusOutput(pedal, dirty, kBlockSize);

            juce::AudioBuffer<float> recovery(2, kBlockSize * 16);
            recovery.clear();
            for (int i = 0; i < recovery.getNumSamples(); ++i)
            {
                const float clean = 0.06f * std::sin(juce::MathConstants<float>::twoPi * 246.94f
                    * (float) i / (float) kSampleRate);
                recovery.setSample(0, i, clean);
                recovery.setSample(1, i, clean);
            }

            const auto recoveryOutput = renderChorusOutput(pedal, recovery, kBlockSize);
            expect(bufferHasOnlyFiniteSamples(dirtyOutput), "Chorus must scrub NaN/Inf input to finite output");
            expect(bufferHasOnlyFiniteSamples(recoveryOutput), "Chorus state must recover to finite output after invalid input");
        }

        beginTest("ChorusPedal dry finite input is unchanged by safety scrub");
        {
            ChorusPedal pedal;
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.0f));
            pedal.prepareToPlay(kSampleRate, kBlockSize);

            juce::AudioBuffer<float> input(2, kBlockSize * 12);
            input.clear();
            for (int i = 0; i < input.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) kSampleRate;
                input.setSample(0, i, 0.11f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t));
                input.setSample(1, i, 0.09f * std::sin(juce::MathConstants<float>::twoPi * 247.0f * t));
            }

            const auto output = renderChorusOutput(pedal, input, kBlockSize);
            expect(bufferHasOnlyFiniteSamples(output), "Dry finite chorus render must remain finite");
            expect(computeBufferNullRms(input, output) <= 1.0e-6,
                "Dry finite chorus input should be materially unchanged by safety scrub");
        }

        beginTest("ReverbPedal max decay under sustained input stays bounded");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(0.4f);
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.98f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.96f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.92f));
            pedal.dampingParam->setValueNotifyingHost(pedal.dampingParam->convertTo0to1(0.18f));
            pedal.bassCutParam->setValueNotifyingHost(pedal.bassCutParam->convertTo0to1(0.10f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(20.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 5.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 174.61f * (float) i / (float) kSampleRate;
                const float sample = 0.30f * std::sin(phase);
                input.setSample(0, i, sample);
                input.setSample(1, i, sample);
            }

            const auto output = renderReverbOutput(pedal, input, kBlockSize);
            const double peak = computeBufferPeak(output, 0, output.getNumSamples());

            expect(bufferHasOnlyFiniteSamples(output), "Max-decay reverb must remain finite");
            expect(peak < 8.0, "Max-decay reverb should respect the safety ceiling");
        }

        beginTest("ReverbPedal sanitizes NaN/Inf input under aggressive feedback");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(0.4f);
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.92f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.85f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.7f));

            const int totalSamples = kBlockSize * 32;
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float clean = 0.10f * std::sin(juce::MathConstants<float>::twoPi * 174.61f
                    * (float) i / (float) kSampleRate);
                const float v = (i % 17 == 0) ? std::numeric_limits<float>::quiet_NaN()
                              : (i % 23 == 0) ? std::numeric_limits<float>::infinity()
                              : (i % 29 == 0) ? -std::numeric_limits<float>::infinity()
                              : clean;
                input.setSample(0, i, v);
                input.setSample(1, i, v);
            }

            const auto output = renderReverbOutput(pedal, input, kBlockSize);
            expect(bufferHasOnlyFiniteSamples(output), "Reverb must scrub NaN/Inf inputs to a finite output");
        }

        beginTest("ReverbPedal high peak input under max decay stays bounded");
        {
            ReverbPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(0.4f);
            pedal.decayParam->setValueNotifyingHost(pedal.decayParam->convertTo0to1(0.96f));
            pedal.sizeParam->setValueNotifyingHost(pedal.sizeParam->convertTo0to1(0.94f));
            pedal.diffusionParam->setValueNotifyingHost(pedal.diffusionParam->convertTo0to1(0.94f));
            pedal.dampingParam->setValueNotifyingHost(pedal.dampingParam->convertTo0to1(0.16f));
            pedal.bassCutParam->setValueNotifyingHost(pedal.bassCutParam->convertTo0to1(0.12f));
            pedal.predelayParam->setValueNotifyingHost(pedal.predelayParam->convertTo0to1(15.0f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(1.0f));

            const int totalSamples = (int) (kSampleRate * 5.0);
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * 164.81f * (float) i / (float) kSampleRate;
                const float tone = 0.06f * std::sin(phase);
                const float spike = (i % 997 == 0) ? (i % 1994 == 0 ? 3.5f : -3.5f) : 0.0f;
                const float sample = tone + spike;
                input.setSample(0, i, sample);
                input.setSample(1, i, -sample * 0.85f);
            }

            const auto output = renderReverbOutput(pedal, input, kBlockSize);
            const double peak = computeBufferPeak(output, 0, output.getNumSamples());
            const double lateRms = computeWindowRms(output, totalSamples - (int) kSampleRate, (int) kSampleRate);

            expect(bufferHasOnlyFiniteSamples(output), "High-peak reverb render must remain finite");
            expect(peak < 8.0, "High-peak reverb should respect the safety ceiling");
            expect(lateRms < 2.0, "High-peak reverb should not sustain near-clip energy");
        }

        beginTest("PhaserPedal sanitizes NaN/Inf input under aggressive feedback");
        {
            PhaserPedal pedal;
            pedal.prepareToPlay(kSampleRate, kBlockSize);
            pedal.modeParam->setValueNotifyingHost(normalisedChoiceIndex(pedal.modeParam, 1));
            pedal.feedbackParam->setValueNotifyingHost(pedal.feedbackParam->convertTo0to1(0.78f));
            pedal.depthParam->setValueNotifyingHost(pedal.depthParam->convertTo0to1(0.85f));
            pedal.mixParam->setValueNotifyingHost(pedal.mixParam->convertTo0to1(0.85f));

            const int totalSamples = kBlockSize * 24;
            juce::AudioBuffer<float> input(2, totalSamples);
            input.clear();
            for (int i = 0; i < totalSamples; ++i)
            {
                const float clean = 0.10f * std::sin(juce::MathConstants<float>::twoPi * 196.0f
                    * (float) i / (float) kSampleRate);
                const float v = (i % 17 == 0) ? std::numeric_limits<float>::quiet_NaN()
                              : (i % 23 == 0) ? std::numeric_limits<float>::infinity()
                              : (i % 29 == 0) ? -std::numeric_limits<float>::infinity()
                              : clean;
                input.setSample(0, i, v);
                input.setSample(1, i, v);
            }

            const auto output = renderPhaserOutput(pedal, input, kBlockSize);
            expect(bufferHasOnlyFiniteSamples(output), "Phaser must scrub NaN/Inf inputs to a finite output");
        }
    }
};

class P1PedalSafetyTests final : public juce::UnitTest
{
public:
    P1PedalSafetyTests()
        : juce::UnitTest("P1 Pedal Safety", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("P1 pedals stay finite without fallback across prepared block sizes and sample rates");
        {
            runP1PreparedMatrix<CabinetPedal>(*this, "Cabinet");
            runP1PreparedMatrix<Vintage2x12Cabinet>(*this, "Vintage2x12");
            runP1PreparedMatrix<Modern4x12Cabinet>(*this, "Modern4x12");
            runP1PreparedMatrix<BoostPedal>(*this, "Boost");
            runP1PreparedMatrix<CompressorPedal>(*this, "Compressor");
            runP1PreparedMatrix<DistortionPedal>(*this, "Distortion");
            runP1PreparedMatrix<FuzzPedal>(*this, "Fuzz");
            runP1PreparedMatrix<NeuralPedal>(*this, "Neural");
            runP1PreparedMatrix<ClassicWahPedal>(*this, "Wah");
            runP1PreparedMatrix<PhaserPedal>(*this, "Phaser");
            runP1PreparedMatrix<OverdrivePedal>(*this, "Overdrive");
            runP1PreparedMatrix<NoiseGatePedal>(*this, "Noise Gate");
            runP1PreparedMatrix<EQPedal>(*this, "EQ");
            runP1PreparedMatrix<OctavePedal>(*this, "Octave");
            runP1PreparedMatrix<ChorusPedal>(*this, "Chorus");
            runP1PreparedMatrix<FlangerPedal>(*this, "Flanger");
            runP1PreparedMatrix<TremoloPedal>(*this, "Tremolo");
            runP1PreparedMatrix<DelayPedal>(*this, "Delay");
            runP1PreparedMatrix<ReverbPedal>(*this, "Reverb");
        }

        beginTest("P1 pedals use dry no-allocation fallback when host exceeds prepared block size");
        {
            runP1OversizedFallback<CabinetPedal>(*this, "Cabinet");
            runP1OversizedFallback<Vintage2x12Cabinet>(*this, "Vintage2x12");
            runP1OversizedFallback<Modern4x12Cabinet>(*this, "Modern4x12");
            runP1OversizedFallback<BoostPedal>(*this, "Boost");
            runP1OversizedFallback<CompressorPedal>(*this, "Compressor");
            runP1OversizedFallback<DistortionPedal>(*this, "Distortion");
            runP1OversizedFallback<FuzzPedal>(*this, "Fuzz");
            runP1OversizedFallback<NeuralPedal>(*this, "Neural");
            runP1OversizedFallback<ClassicWahPedal>(*this, "Wah");
            runP1OversizedFallback<PhaserPedal>(*this, "Phaser");
            runP1OversizedFallback<OverdrivePedal>(*this, "Overdrive");
            runP1OversizedFallback<NoiseGatePedal>(*this, "Noise Gate");
            runP1OversizedFallback<EQPedal>(*this, "EQ");
            runP1OversizedFallback<OctavePedal>(*this, "Octave");
            runP1OversizedFallback<ChorusPedal>(*this, "Chorus");
            runP1OversizedFallback<FlangerPedal>(*this, "Flanger");
            runP1OversizedFallback<TremoloPedal>(*this, "Tremolo");
            runP1OversizedFallback<DelayPedal>(*this, "Delay");
            runP1OversizedFallback<ReverbPedal>(*this, "Reverb");
        }

        beginTest("P1 pedals keep bypass transitions finite and bounded");
        {
            runP1BypassToggleContinuity<CabinetPedal>(*this, "Cabinet");
            runP1BypassToggleContinuity<Vintage2x12Cabinet>(*this, "Vintage2x12");
            runP1BypassToggleContinuity<Modern4x12Cabinet>(*this, "Modern4x12");
            runP1BypassToggleContinuity<BoostPedal>(*this, "Boost");
            runP1BypassToggleContinuity<CompressorPedal>(*this, "Compressor");
            runP1BypassToggleContinuity<DistortionPedal>(*this, "Distortion");
            runP1BypassToggleContinuity<FuzzPedal>(*this, "Fuzz");
            runP1BypassToggleContinuity<NeuralPedal>(*this, "Neural");
            runP1BypassToggleContinuity<ClassicWahPedal>(*this, "Wah");
            runP1BypassToggleContinuity<PhaserPedal>(*this, "Phaser");
            runP1BypassToggleContinuity<OverdrivePedal>(*this, "Overdrive");
            runP1BypassToggleContinuity<NoiseGatePedal>(*this, "Noise Gate");
            runP1BypassToggleContinuity<EQPedal>(*this, "EQ");
            runP1BypassToggleContinuity<OctavePedal>(*this, "Octave");
            runP1BypassToggleContinuity<ChorusPedal>(*this, "Chorus");
            runP1BypassToggleContinuity<FlangerPedal>(*this, "Flanger");
            runP1BypassToggleContinuity<TremoloPedal>(*this, "Tremolo");
            runP1BypassToggleContinuity<DelayPedal>(*this, "Delay");
            runP1BypassToggleContinuity<ReverbPedal>(*this, "Reverb");
        }
    }
};

class P13EPedalContractProbe final : public NovaPedalProcessor
{
public:
    const juce::String getName() const override { return "P13E Contract Probe"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        preparePedalProcessing(sampleRate, samplesPerBlock, 2);
        renderCount = 0;
        fallbackHookCount = 0;
    }

    void releaseResources() override
    {
        releasePedalProcessing();
    }

    void reset() override
    {
    }

    static float mixForTest(float dry, float wet, float mix) noexcept
    {
        return mixDryWetEqualPower(dry, wet, mix);
    }

    static float fallbackParameterForTest(float fallback, float minimum, float maximum) noexcept
    {
        return readParameter(nullptr, fallback, minimum, maximum);
    }

    int renderCount = 0;
    int fallbackHookCount = 0;

protected:
    void processPedalBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        ++renderCount;
        buffer.applyGain(0.5f);
    }

    void handleRealtimeFallback() noexcept override
    {
        ++fallbackHookCount;
    }
};

class P13ESharedPedalContractTests final : public juce::UnitTest
{
public:
    P13ESharedPedalContractTests()
        : juce::UnitTest("P13E-P4 Shared Pedal RT-Safety Contract", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("P13E-P4 scaffold centralizes input scrub and wet dispatch");
        {
            P13EPedalContractProbe probe;
            probe.prepareToPlay(kSampleRate, kBlockSize);

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            buffer.clear();
            buffer.setSample(0, 0, std::numeric_limits<float>::quiet_NaN());
            buffer.setSample(0, 1, std::numeric_limits<float>::infinity());
            buffer.setSample(0, 2, -std::numeric_limits<float>::infinity());
            buffer.setSample(0, 3, 1.0e-35f);
            buffer.setSample(0, 4, Nova::Config::HARD_ABS_LIMIT_LINEAR * 8.0f);
            buffer.setSample(1, 5, 0.25f);

            juce::MidiBuffer midi;
            probe.processBlock(buffer, midi);

            expectEquals(probe.renderCount, 1, "Prepared normal block should dispatch wet DSP once");
            expectEquals(probe.getRealtimeFallbackCount(), 0,
                "Prepared normal block must not increment fallback diagnostics");
            expect(bufferHasOnlyFiniteSamples(buffer), "Shared scrub must remove NaN/Inf before wet DSP");
            expectWithinAbsoluteError(buffer.getSample(0, 0), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError(buffer.getSample(0, 1), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError(buffer.getSample(0, 2), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError(buffer.getSample(0, 3), 0.0f, 1.0e-7f);
            expect(std::abs(buffer.getSample(0, 4))
                    <= Nova::Config::HARD_ABS_LIMIT_LINEAR * 0.75f + 1.0e-4f,
                "Shared hot-value scrub should bound input before probe gain");
            expectWithinAbsoluteError(buffer.getSample(1, 5), 0.125f, 1.0e-6f);
        }

        beginTest("P13E-P4 scaffold oversized block takes dry fallback without wet dispatch");
        {
            constexpr int preparedBlockSize = 32;
            constexpr int oversizedBlockSize = 96;

            P13EPedalContractProbe probe;
            probe.prepareToPlay(kSampleRate, preparedBlockSize);

            juce::AudioBuffer<float> buffer(2, oversizedBlockSize);
            fillP1PedalSignal(buffer, P1PedalSignal::Sine1k, kSampleRate, 0x13E4);
            juce::AudioBuffer<float> dryCopy(buffer);

            juce::MidiBuffer midi;
            probe.processBlock(buffer, midi);

            expectEquals(probe.renderCount, 0, "Oversized block must not enter derived wet DSP");
            expectEquals(probe.fallbackHookCount, 1, "Oversized block must invoke the optional fallback hook");
            expectEquals(probe.getRealtimeFallbackCount(), 1,
                "Oversized block must increment shared fallback diagnostics");
            expect(bufferHasOnlyFiniteSamples(buffer), "Oversized dry fallback must remain finite");
            expect(computeBufferNullRms(buffer, dryCopy) < 1.0e-7,
                "Oversized finite input must be preserved as dry audio");
        }

        beginTest("P13E-P4 scaffold scrubs bypass dry scratch before crossfade");
        {
            P13EPedalContractProbe probe;
            probe.prepareToPlay(kSampleRate, kBlockSize);
            probe.setBypassed(true);

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            buffer.clear();
            buffer.setSample(0, 0, std::numeric_limits<float>::quiet_NaN());
            buffer.setSample(0, 1, std::numeric_limits<float>::infinity());
            buffer.setSample(1, 2, -std::numeric_limits<float>::infinity());
            buffer.setSample(1, 3, 1.0e-35f);
            buffer.setSample(0, 4, Nova::Config::HARD_ABS_LIMIT_LINEAR * 8.0f);

            juce::MidiBuffer midi;
            probe.processBlock(buffer, midi);

            expect(bufferHasOnlyFiniteSamples(buffer),
                "Bypass crossfade must not reintroduce corrupt input from dry scratch");
            expect(p1BufferPeak(buffer) <= Nova::Config::HARD_ABS_LIMIT_LINEAR * 1.5f + 1.0e-4f,
                "Bypass crossfade must retain the shared hot-value bound");
            expectEquals(probe.getRealtimeFallbackCount(), 0,
                "Prepared bypass transition must not enter realtime fallback");
        }

        beginTest("P13E-P4 parameter and equal-power mix helpers preserve finite behavior");
        {
            expectWithinAbsoluteError(
                P13EPedalContractProbe::fallbackParameterForTest(0.25f, 0.0f, 1.0f),
                0.25f,
                1.0e-7f);
            expectWithinAbsoluteError(
                P13EPedalContractProbe::fallbackParameterForTest(
                    std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f),
                0.0f,
                1.0e-7f);
            expectWithinAbsoluteError(P13EPedalContractProbe::mixForTest(0.4f, 0.8f, 0.0f),
                0.4f,
                1.0e-6f);
            expectWithinAbsoluteError(P13EPedalContractProbe::mixForTest(0.4f, 0.8f, 1.0f),
                0.8f,
                1.0e-6f);

            const float expectedMidMix = (0.4f + 0.8f) * std::sqrt(0.5f);
            expectWithinAbsoluteError(P13EPedalContractProbe::mixForTest(0.4f, 0.8f, 0.5f),
                expectedMidMix,
                1.0e-6f);
            expect(std::isfinite(P13EPedalContractProbe::mixForTest(
                std::numeric_limits<float>::infinity(), 0.5f, 0.5f)),
                "Mix helper must contain invalid operands");
        }

        beginTest("P13E-P4 migrated pedals preserve normal finite bounded output");
        {
            runMigratedPedalNormalSafety<BoostPedal>("Boost");
            runMigratedPedalNormalSafety<CompressorPedal>("Compressor");
        }

        beginTest("P13E-P4 migrated pedals survive NaN Inf denormal and hot input");
        {
            runMigratedPedalCorruptInputSafety<BoostPedal>("Boost");
            runMigratedPedalCorruptInputSafety<CompressorPedal>("Compressor");
        }

        beginTest("P13E-P4 migrated pedals retain oversized dry fallback");
        {
            runP1OversizedFallback<BoostPedal>(*this, "P13E-P4 Boost");
            runP1OversizedFallback<CompressorPedal>(*this, "P13E-P4 Compressor");
        }
    }

private:
    template <typename PedalType>
    void runMigratedPedalNormalSafety(const juce::String& name)
    {
        PedalType pedal;
        pedal.prepareToPlay(kSampleRate, kBlockSize);

        juce::AudioBuffer<float> block(2, kBlockSize);
        fillP1PedalSignal(block, P1PedalSignal::Sine1k, kSampleRate, 0x4E50);

        juce::MidiBuffer midi;
        pedal.processBlock(block, midi);

        expect(bufferHasOnlyFiniteSamples(block), name + " normal output must remain finite");
        expect(p1BufferPeak(block) < 16.0, name + " normal output must remain bounded");
        expect(computeWindowRms(block, 0, block.getNumSamples()) > 1.0e-5,
            name + " normal output must remain audible");
        expectEquals(pedal.getRealtimeFallbackCount(), 0,
            name + " normal block must not enter shared fallback");
    }

    template <typename PedalType>
    void runMigratedPedalCorruptInputSafety(const juce::String& name)
    {
        PedalType pedal;
        pedal.prepareToPlay(kSampleRate, kBlockSize);

        juce::AudioBuffer<float> block(2, kBlockSize);
        for (int ch = 0; ch < block.getNumChannels(); ++ch)
        {
            for (int sample = 0; sample < block.getNumSamples(); ++sample)
            {
                const int selector = (sample + ch) % 11;
                const float value = selector == 0 ? std::numeric_limits<float>::quiet_NaN()
                    : selector == 1 ? std::numeric_limits<float>::infinity()
                    : selector == 2 ? -std::numeric_limits<float>::infinity()
                    : selector == 3 ? 1.0e-35f
                    : selector == 4 ? Nova::Config::HARD_ABS_LIMIT_LINEAR * 8.0f
                    : 0.08f * std::sin(juce::MathConstants<float>::twoPi * 330.0f
                        * (float) sample / (float) kSampleRate);
                block.setSample(ch, sample, value);
            }
        }

        juce::MidiBuffer midi;
        pedal.processBlock(block, midi);

        expect(bufferHasOnlyFiniteSamples(block), name + " corrupt-input output must remain finite");
        expect(p1BufferPeak(block) < Nova::Config::HARD_ABS_LIMIT_LINEAR * 4.0f,
            name + " corrupt-input output must remain bounded");
        expectEquals(pedal.getRealtimeFallbackCount(), 0,
            name + " prepared corrupt-input block must not enter fallback");
    }
};

// P13E-P5: registry-wide pedal contract migration coverage plus inter-pedal
// NaN/Inf corruption containment across real stateful/feedback pedal chains.
class P13EPedalContractMigrationTests final : public juce::UnitTest
{
public:
    P13EPedalContractMigrationTests()
        : juce::UnitTest("P13E-P5 Pedal Contract Migration & Corruption Containment", "NOVA")
    {
    }

    void runTest() override
    {
        const auto pedalTypes = registeredPedalTypes();

        beginTest("P13E-P5 every registered pedal type derives from the shared NovaPedalProcessor contract");
        {
            expect(pedalTypes.size() >= 16,
                "Registry should expose the full migrated pedal set (found "
                    + juce::String(pedalTypes.size()) + ")");

            for (const auto& type : pedalTypes)
            {
                auto processor = PedalRegistry::createPedal(type);
                expect(processor != nullptr, "Registry must instantiate pedal " + type);
                if (processor == nullptr)
                    continue;

                auto* pedal = dynamic_cast<NovaPedalProcessor*>(processor.get());
                expect(pedal != nullptr,
                    type + " must derive from NovaPedalProcessor (shared RT-safety contract)");
            }
        }

        beginTest("P13E-P5 registry-wide invariant: finite/bounded across signal classes, channels and recovery");
        {
            for (const auto& type : pedalTypes)
                runRegistryInvariantForType(type);
        }

        beginTest("P13E-P5 registry-wide oversized blocks take a no-allocation dry fallback");
        {
            for (const auto& type : pedalTypes)
                runRegistryOversizedFallbackForType(type);
        }

        beginTest("P13E-P5 inter-pedal corruption containment: feedback chain stays finite, bounded and decays");
        {
            runInterPedalCorruptionChain();
        }

        beginTest("P13E-P5 Compressor realtime fallback resets the gain-reduction meter");
        {
            constexpr int preparedBlockSize = 32;
            constexpr int oversizedBlockSize = 96;

            CompressorPedal comp;
            comp.prepareToPlay(kSampleRate, preparedBlockSize);
            comp.clearRealtimeFallbackCount();

            juce::MidiBuffer midi;
            for (int b = 0; b < 24; ++b)
            {
                juce::AudioBuffer<float> hot(2, preparedBlockSize);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < preparedBlockSize; ++i)
                        hot.setSample(ch, i, 0.9f * std::sin(juce::MathConstants<float>::twoPi
                            * 220.0f * (float) (b * preparedBlockSize + i) / (float) kSampleRate));
                comp.processBlock(hot, midi);
            }

            expect(comp.getCurrentGainReductionDb() > 0.0f,
                "Compressor must register gain reduction under a hot signal");

            juce::AudioBuffer<float> oversized(2, oversizedBlockSize);
            fillP1PedalSignal(oversized, P1PedalSignal::Sine1k, kSampleRate, 0x6C0);
            comp.processBlock(oversized, midi);

            expectEquals(comp.getRealtimeFallbackCount(), 1,
                "Compressor oversized block must increment the realtime fallback counter");
            expectWithinAbsoluteError(comp.getCurrentGainReductionDb(), 0.0f, 1.0e-6f,
                "Compressor realtime fallback must reset the gain-reduction meter");
        }

        beginTest("P13E-P5 clearRealtimeFallbackCount resets the shared diagnostic counter");
        {
            BoostPedal boost;
            boost.prepareToPlay(kSampleRate, 32);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> oversized(2, 96);
            fillP1PedalSignal(oversized, P1PedalSignal::Sine1k, kSampleRate, 0x7AB);
            boost.processBlock(oversized, midi);

            expect(boost.getRealtimeFallbackCount() >= 1,
                "Oversized block must raise the shared fallback counter");
            boost.clearRealtimeFallbackCount();
            expectEquals(boost.getRealtimeFallbackCount(), 0,
                "clearRealtimeFallbackCount must reset the shared counter to zero");
        }
    }

private:
    static juce::StringArray registeredPedalTypes()
    {
        juce::StringArray types;
        for (auto zone : { Nova::ZoneID::Pre, Nova::ZoneID::FX })
            for (const auto& t : PedalRegistry::getPedalTypesForZone(zone))
                types.addIfNotAlreadyThere(t);
        return types;
    }

    static void fillCorruptHotSignal(juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const int selector = (sample + ch) % 11;
                const float value = selector == 0 ? std::numeric_limits<float>::quiet_NaN()
                    : selector == 1 ? std::numeric_limits<float>::infinity()
                    : selector == 2 ? -std::numeric_limits<float>::infinity()
                    : selector == 3 ? 1.0e-35f
                    : selector == 4 ? Nova::Config::HARD_ABS_LIMIT_LINEAR * 8.0f
                    : 0.12f * std::sin(juce::MathConstants<float>::twoPi * 330.0f
                        * (float) sample / (float) sampleRate);
                buffer.setSample(ch, sample, value);
            }
        }
    }

    // NaN/Inf/denormal interspersed with a normal-level tone (no extreme hot values),
    // so the inter-pedal chain stays at normal levels and the test isolates NaN/Inf
    // containment and feedback-tail decay rather than hot-level handling.
    static void fillNanInfSignal(juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const int selector = (sample + ch) % 9;
                const float value = selector == 0 ? std::numeric_limits<float>::quiet_NaN()
                    : selector == 1 ? std::numeric_limits<float>::infinity()
                    : selector == 2 ? -std::numeric_limits<float>::infinity()
                    : selector == 3 ? 1.0e-35f
                    : 0.20f * std::sin(juce::MathConstants<float>::twoPi * 196.0f
                        * (float) sample / (float) sampleRate);
                buffer.setSample(ch, sample, value);
            }
        }
    }

    void runRegistryInvariantForType(const juce::String& type)
    {
        constexpr double sampleRate = kSampleRate;
        constexpr int blockSize = kBlockSize;

        auto processor = PedalRegistry::createPedal(type);
        if (processor == nullptr)
        {
            expect(false, "createPedal failed for " + type);
            return;
        }

        auto* pedal = dynamic_cast<NovaPedalProcessor*>(processor.get());
        if (pedal == nullptr)
        {
            expect(false, type + " is not a NovaPedalProcessor");
            return;
        }

        processor->setPlayConfigDetails(2, 2, sampleRate, blockSize);
        processor->prepareToPlay(sampleRate, blockSize);

        juce::MidiBuffer midi;

        // 1. Normal finite signal (stereo).
        {
            pedal->clearRealtimeFallbackCount();
            juce::AudioBuffer<float> buffer(2, blockSize);
            fillP1PedalSignal(buffer, P1PedalSignal::Sine1k, sampleRate, 0x6F1);
            processor->processBlock(buffer, midi);
            expect(bufferHasOnlyFiniteSamples(buffer), type + " normal signal output must be finite");
            expect(p1BufferPeak(buffer) < 16.0, type + " normal signal output must be bounded");
            expectEquals(pedal->getRealtimeFallbackCount(), 0,
                type + " prepared normal block must not enter fallback");
        }

        // 2. NaN/Inf/denormal/hot input on a prepared block (scrub, not fallback).
        {
            pedal->clearRealtimeFallbackCount();
            juce::AudioBuffer<float> buffer(2, blockSize);
            fillCorruptHotSignal(buffer, sampleRate);
            processor->processBlock(buffer, midi);
            expect(bufferHasOnlyFiniteSamples(buffer), type + " corrupt/hot input output must be finite");
            expect(p1BufferPeak(buffer) < Nova::Config::HARD_ABS_LIMIT_LINEAR * 4.0f,
                type + " corrupt/hot input output must stay bounded");
            expectEquals(pedal->getRealtimeFallbackCount(), 0,
                type + " prepared corrupt/hot block must scrub rather than fall back");
        }

        // 3. Recovery: after corruption, a normal signal must process to finite, bounded, audible output
        //    (no stale corrupt state latched into the feedback/filter network).
        {
            pedal->clearRealtimeFallbackCount();
            double maxPeak = 0.0;
            double totalEnergy = 0.0;
            bool finite = true;
            for (int b = 0; b < 32; ++b)
            {
                juce::AudioBuffer<float> buffer(2, blockSize);
                fillP1PedalSignal(buffer, P1PedalSignal::Sine1k, sampleRate, 0x1234 + b);
                processor->processBlock(buffer, midi);
                finite = finite && bufferHasOnlyFiniteSamples(buffer);
                const double peak = p1BufferPeak(buffer);
                maxPeak = juce::jmax(maxPeak, peak);
                totalEnergy += peak;
            }
            expect(finite, type + " must recover to finite output after corruption");
            // The first recovery blocks may legitimately replay the delay/lookahead tail
            // of the bounded-but-hot corrupt block (scrubbed to <= ~1.5x the hard limit),
            // so bound recovery against the no-runaway ceiling rather than the nominal one.
            expect(maxPeak < Nova::Config::HARD_ABS_LIMIT_LINEAR * 4.0f,
                type + " must recover to bounded output after corruption");
            expect(totalEnergy > 0.0, type + " must not latch into NaN/zero after corruption");
            expectEquals(pedal->getRealtimeFallbackCount(), 0,
                type + " recovery blocks must not enter fallback");
        }

        // 4. reset() realtime-safety proxy: bypass on/off cycles call reset() on the audio thread.
        {
            pedal->clearRealtimeFallbackCount();
            bool finite = true;
            for (int b = 0; b < 8; ++b)
            {
                pedal->setBypassed((b % 2) == 0);
                juce::AudioBuffer<float> buffer(2, blockSize);
                fillP1PedalSignal(buffer, P1PedalSignal::Sine1k, sampleRate, 0xBEEF + b);
                processor->processBlock(buffer, midi);
                finite = finite && bufferHasOnlyFiniteSamples(buffer);
            }
            pedal->setBypassed(false);
            expect(finite, type + " bypass-toggle reset cycles must stay finite");
            expectEquals(pedal->getRealtimeFallbackCount(), 0,
                type + " bypass-toggle cycles must not enter fallback");
        }

        // 5. mono-content / prepared-channel safety: identical L/R content on the prepared
        //    stereo bus must process to finite, bounded output without falling back.
        //    (The pedal bus is always stereo; oversampled stages are prepared for two
        //    channels, so mono compatibility is exercised via mono-summed content.)
        {
            pedal->clearRealtimeFallbackCount();
            juce::AudioBuffer<float> buffer(2, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = 0.30f * std::sin(juce::MathConstants<float>::twoPi
                    * 440.0f * (float) i / (float) sampleRate);
                buffer.setSample(0, i, v);
                buffer.setSample(1, i, v);
            }
            processor->processBlock(buffer, midi);
            expect(bufferHasOnlyFiniteSamples(buffer), type + " mono-content block output must be finite");
            expect(p1BufferPeak(buffer) < 16.0, type + " mono-content block output must be bounded");
            expectEquals(pedal->getRealtimeFallbackCount(), 0,
                type + " mono-content block must not enter fallback");
        }
    }

    void runRegistryOversizedFallbackForType(const juce::String& type)
    {
        constexpr double sampleRate = kSampleRate;
        constexpr int preparedBlockSize = 32;
        constexpr int hostBlockSize = 64;

        auto processor = PedalRegistry::createPedal(type);
        if (processor == nullptr)
        {
            expect(false, "createPedal failed for " + type);
            return;
        }

        auto* pedal = dynamic_cast<NovaPedalProcessor*>(processor.get());
        if (pedal == nullptr)
        {
            expect(false, type + " is not a NovaPedalProcessor");
            return;
        }

        processor->setPlayConfigDetails(2, 2, sampleRate, preparedBlockSize);
        processor->prepareToPlay(sampleRate, preparedBlockSize);
        pedal->clearRealtimeFallbackCount();

        juce::AudioBuffer<float> buffer(2, hostBlockSize);
        fillP1PedalSignal(buffer, P1PedalSignal::Sine1k, sampleRate, 0x5151);
        juce::AudioBuffer<float> inputCopy(buffer);

        juce::MidiBuffer midi;
        processor->processBlock(buffer, midi);

        expect(bufferHasOnlyFiniteSamples(buffer), type + " oversized fallback must stay finite");
        expectEquals(pedal->getRealtimeFallbackCount(), 1,
            type + " oversized block must increment the realtime fallback counter");
        expect(computeBufferNullRms(buffer, inputCopy) < 1.0e-7,
            type + " oversized fallback must preserve dry audio without allocation");
    }

    void runInterPedalCorruptionChain()
    {
        constexpr double sampleRate = kSampleRate;
        constexpr int blockSize = kBlockSize;

        // Real registry pedals with feedback/delay/dry-capture state. A corrupting
        // source upstream must never poison their feedback tails.
        const juce::StringArray chainTypes { "Delay", "Chorus", "Flanger", "Reverb" };

        std::vector<std::unique_ptr<juce::AudioProcessor>> chain;
        for (const auto& type : chainTypes)
        {
            auto processor = PedalRegistry::createPedal(type);
            expect(processor != nullptr, "Corruption chain requires registry pedal " + type);
            if (processor == nullptr)
                return;

            processor->setPlayConfigDetails(2, 2, sampleRate, blockSize);
            processor->prepareToPlay(sampleRate, blockSize);
            chain.push_back(std::move(processor));
        }

        juce::MidiBuffer midi;
        auto runChain = [&](juce::AudioBuffer<float>& buffer) -> bool
        {
            bool finite = true;
            for (auto& processor : chain)
            {
                processor->processBlock(buffer, midi);
                finite = finite && bufferHasOnlyFiniteSamples(buffer);
            }
            return finite;
        };

        // Inject NaN/Inf/denormal/hot from the source into the live feedback chain.
        bool corruptFinite = true;
        double corruptPeak = 0.0;
        for (int b = 0; b < 16; ++b)
        {
            juce::AudioBuffer<float> buffer(2, blockSize);
            fillNanInfSignal(buffer, sampleRate);
            corruptFinite = corruptFinite && runChain(buffer);
            corruptPeak = juce::jmax(corruptPeak, p1BufferPeak(buffer));
        }

        expect(corruptFinite,
            "Corrupting source must never produce non-finite output downstream in the feedback chain");
        expect(corruptPeak < Nova::Config::HARD_ABS_LIMIT_LINEAR * 4.0f,
            "Corrupting source must stay bounded through the feedback chain");

        // Now feed silence: the feedback tails must decay toward silence (no poisoning / self-oscillation),
        // stay finite and stay bounded the entire time.
        double maxTailPeak = 0.0;
        double latePeak = 0.0;
        bool tailFinite = true;
        constexpr int tailBlocks = 2048; // ~2.7 s at 48 kHz / 64-sample blocks
        for (int b = 0; b < tailBlocks; ++b)
        {
            juce::AudioBuffer<float> buffer(2, blockSize);
            buffer.clear();
            tailFinite = tailFinite && runChain(buffer);
            const double peak = p1BufferPeak(buffer);
            maxTailPeak = juce::jmax(maxTailPeak, peak);
            if (b >= tailBlocks - 16)
                latePeak = juce::jmax(latePeak, peak);
        }

        expect(tailFinite, "Feedback chain tail must stay finite after corruption");
        expect(maxTailPeak < 4.0, "Feedback chain tail must stay bounded after corruption");
        expect(latePeak < 0.05,
            "Feedback chain tail must decay toward silence after corruption (latePeak="
                + juce::String(latePeak) + ")");
    }
};

class P13DDriveDynamicsSafetyHardeningTests final : public juce::UnitTest
{
public:
    P13DDriveDynamicsSafetyHardeningTests()
        : juce::UnitTest("P13D Drive/Dynamics Safety Hardening", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("P13D Boost oversized block uses finite dry fallback");
        {
            runP1OversizedFallback<BoostPedal>(*this, "P13D Boost");
        }

        beginTest("P13D Boost recovers after NaN/Inf abuse");
        {
            BoostPedal boost;
            prepare(boost);
            configureBoost(boost);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            bool finiteDuringAbuse = true;

            for (int blockIndex = 0; blockIndex < 24; ++blockIndex)
            {
                fillDriveDynamicsBlock(block, blockIndex, 0.075f);
                injectInvalidSamples(block, blockIndex);
                boost.processBlock(block, midi);
                finiteDuringAbuse = finiteDuringAbuse && bufferHasOnlyFiniteSamples(block);
            }

            double recoveryRmsSum = 0.0;
            int recoveryBlocks = 0;
            bool finiteDuringRecovery = true;

            for (int blockIndex = 24; blockIndex < 112; ++blockIndex)
            {
                fillDriveDynamicsBlock(block, blockIndex, 0.080f);
                boost.processBlock(block, midi);
                finiteDuringRecovery = finiteDuringRecovery && bufferHasOnlyFiniteSamples(block);

                if (blockIndex >= 36)
                {
                    recoveryRmsSum += computeWindowRms(block, 0, block.getNumSamples());
                    ++recoveryBlocks;
                }
            }

            const double recoveryRms = recoveryBlocks > 0 ? recoveryRmsSum / (double) recoveryBlocks : 0.0;
            expect(finiteDuringAbuse, "P13D Boost must scrub NaN/Inf input to finite output");
            expect(finiteDuringRecovery, "P13D Boost must stay finite after invalid input abuse");
            expect(recoveryRms > 0.0010, "P13D Boost must not latch into silence after invalid input");
        }

        beginTest("P13D Noise Gate recovers after NaN/Inf abuse");
        {
            NoiseGatePedal gate;
            prepare(gate);
            configureGate(gate);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            bool finiteDuringAbuse = true;

            for (int blockIndex = 0; blockIndex < 24; ++blockIndex)
            {
                fillDriveDynamicsBlock(block, blockIndex, 0.090f);
                injectInvalidSamples(block, blockIndex);
                gate.processBlock(block, midi);
                finiteDuringAbuse = finiteDuringAbuse && bufferHasOnlyFiniteSamples(block);
            }

            double recoveryRmsSum = 0.0;
            int recoveryBlocks = 0;
            bool finiteDuringRecovery = true;

            for (int blockIndex = 24; blockIndex < 128; ++blockIndex)
            {
                fillDriveDynamicsBlock(block, blockIndex, 0.090f);
                gate.processBlock(block, midi);
                finiteDuringRecovery = finiteDuringRecovery && bufferHasOnlyFiniteSamples(block);

                if (blockIndex >= 40)
                {
                    recoveryRmsSum += computeWindowRms(block, 0, block.getNumSamples());
                    ++recoveryBlocks;
                }
            }

            const double recoveryRms = recoveryBlocks > 0 ? recoveryRmsSum / (double) recoveryBlocks : 0.0;
            expect(finiteDuringAbuse, "P13D Noise Gate must scrub NaN/Inf input to finite output");
            expect(finiteDuringRecovery, "P13D Noise Gate must stay finite after invalid input abuse");
            expect(recoveryRms > 0.0010, "P13D Noise Gate detector/lookahead must not latch into silence after invalid input");
        }

        beginTest("P13D drive/dynamics near-silence remains finite");
        {
            runNearSilenceFinite<DistortionPedal>("Distortion");
            runNearSilenceFinite<FuzzPedal>("Fuzz");
            runNearSilenceFinite<CompressorPedal>("Compressor");
            runNearSilenceFinite<NoiseGatePedal>("Noise Gate");
        }

        beginTest("P13D nominal finite input remains audible and bounded");
        {
            BoostPedal boost;
            prepare(boost);
            configureBoost(boost);
            expectNominalFiniteAudible(boost, "Boost");

            NoiseGatePedal gate;
            prepare(gate);
            configureGate(gate);
            expectNominalFiniteAudible(gate, "Noise Gate");
        }
    }

private:
    static void prepare(juce::AudioProcessor& processor)
    {
        processor.setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
        processor.prepareToPlay(kSampleRate, kBlockSize);
    }

    static void configureBoost(BoostPedal& boost)
    {
        setRangedParamById(boost, "boostGain", 8.0f);
        setRangedParamById(boost, "boostTone", 0.58f);
        setRangedParamById(boost, "boostTight", 0.42f);
        setRangedParamById(boost, "boostChar", 0.18f);
        setRangedParamById(boost, "boostMid", 1.0f);
        setRangedParamById(boost, "boostLevel", 1.0f);
    }

    static void configureGate(NoiseGatePedal& gate)
    {
        setRangedParamById(gate, "gateThreshold", -58.0f);
        setRangedParamById(gate, "gateAttack", 0.20f);
        setRangedParamById(gate, "gateHold", 45.0f);
        setRangedParamById(gate, "gateRelease", 90.0f);
        setRangedParamById(gate, "gateRange", -72.0f);
        setRangedParamById(gate, "gateHysteresis", 5.0f);
        setRangedParamById(gate, "gateFocus", 0.55f);
    }

    static void fillDriveDynamicsBlock(juce::AudioBuffer<float>& block, int blockIndex, float level)
    {
        for (int i = 0; i < block.getNumSamples(); ++i)
        {
            const int sampleIndex = blockIndex * block.getNumSamples() + i;
            const float t = (float) sampleIndex / (float) kSampleRate;
            const float sample = level
                * (0.70f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                    + 0.22f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t)
                    + 0.08f * std::sin(juce::MathConstants<float>::twoPi * 329.63f * t));

            block.setSample(0, i, sample);
            block.setSample(1, i, sample * 0.97f);
        }
    }

    static void fillNearSilenceBlock(juce::AudioBuffer<float>& block, int blockIndex)
    {
        for (int i = 0; i < block.getNumSamples(); ++i)
        {
            const int sampleIndex = blockIndex * block.getNumSamples() + i;
            const float polarity = (sampleIndex & 1) == 0 ? 1.0f : -1.0f;
            const float sample = polarity * 1.0e-24f;
            block.setSample(0, i, sample);
            block.setSample(1, i, -sample);
        }
    }

    static void injectInvalidSamples(juce::AudioBuffer<float>& block, int blockIndex)
    {
        for (int ch = 0; ch < block.getNumChannels(); ++ch)
        {
            for (int i = 0; i < block.getNumSamples(); i += 13)
            {
                const int selector = (i + blockIndex + ch) % 3;
                const float value = selector == 0 ? std::numeric_limits<float>::quiet_NaN()
                    : selector == 1 ? std::numeric_limits<float>::infinity()
                    : -std::numeric_limits<float>::infinity();
                block.setSample(ch, i, value);
            }
        }
    }

    template <typename ProcessorType>
    void runNearSilenceFinite(const juce::String& processorName)
    {
        ProcessorType processor;
        prepare(processor);

        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, kBlockSize);
        bool finite = true;

        for (int blockIndex = 0; blockIndex < 180; ++blockIndex)
        {
            fillNearSilenceBlock(block, blockIndex);
            processor.processBlock(block, midi);
            finite = finite && bufferHasOnlyFiniteSamples(block);
        }

        expect(finite, "P13D " + processorName + " silence/near-silence render must stay finite");
    }

    void expectNominalFiniteAudible(juce::AudioProcessor& processor, const juce::String& processorName)
    {
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, kBlockSize);
        bool finite = true;
        double rmsSum = 0.0;
        int rmsBlocks = 0;

        for (int blockIndex = 0; blockIndex < 96; ++blockIndex)
        {
            fillDriveDynamicsBlock(block, blockIndex, 0.075f);
            processor.processBlock(block, midi);
            finite = finite && bufferHasOnlyFiniteSamples(block);
            if (blockIndex >= 12)
            {
                rmsSum += computeWindowRms(block, 0, block.getNumSamples());
                ++rmsBlocks;
            }
        }

        const double rms = rmsBlocks > 0 ? rmsSum / (double) rmsBlocks : 0.0;
        expect(finite, "P13D " + processorName + " nominal finite input must stay finite");
        expect(rms > 0.0010, "P13D " + processorName + " nominal finite input must remain audible");
        expect(rms < 1.0, "P13D " + processorName + " nominal finite input must remain bounded");
    }
};

#if NOVA_P14B_MEASUREMENT_HARNESS_ENABLED
class P14BDriveDynamicsMeasurementHarnessTests final : public juce::UnitTest
{
public:
    P14BDriveDynamicsMeasurementHarnessTests()
        : juce::UnitTest("P14B-P1 Drive/Dynamics Measurement Harness", "NOVA")
    {
    }

    void runTest() override
    {
        std::vector<P14BMeasurement::ReferenceResult> references;
        std::vector<P14BMeasurement::SweepResult> sweeps;
        std::vector<P14BMeasurement::SpectralResult> spectrals;
        P14BMeasurement::GateProbeResult gateProbe;

        beginTest("P14B helpers are deterministic and finite");
        {
            juce::AudioBuffer<float> noiseA(2, 256);
            juce::AudioBuffer<float> noiseB(2, 256);
            juce::AudioBuffer<float> noiseC(2, 256);
            P14BMeasurement::fillDeterministicNoise(noiseA, 0x1421u, 0.05f);
            P14BMeasurement::fillDeterministicNoise(noiseB, 0x1421u, 0.05f);
            P14BMeasurement::fillDeterministicNoise(noiseC, 0x1422u, 0.05f);

            expect(computeBufferNullRms(noiseA, noiseB) <= 1.0e-9,
                "Fixed-seed noise must reproduce exactly");
            expect(computeBufferNullRms(noiseA, noiseC) > 1.0e-5,
                "Different fixed seeds should produce different deterministic noise");

            juce::AudioBuffer<float> sine(2, 512);
            P14BMeasurement::fillSine(sine, 1000.0, 0.25f);
            const auto metrics = P14BMeasurement::measure(sine, 0, sine.getNumSamples());
            expect(metrics.finite, "1 kHz sine metrics must be finite");
            expect(metrics.rmsDbfs < -6.0 && metrics.rmsDbfs > -20.0,
                "1 kHz sine RMS should land in a plausible dBFS range");
        }

        beginTest("P14B neutral/reference renders complete and finite");
        {
            references = P14BMeasurement::buildReferenceBaselines();
            expectEquals((int) references.size(), 7, "All seven Drive/Dynamics targets must be measured");

            for (const auto& r : references)
            {
                expect(r.referenceSetting.isNotEmpty(), r.pedal + " must document its reference setting");
                expect(r.finite, r.pedal + " reference metrics must be finite");
                expect(r.input.rms > 0.0, r.pedal + " reference input must contain signal");
                expect(r.engaged.peak < Nova::Config::HARD_ABS_LIMIT_LINEAR,
                    r.pedal + " reference peak must stay below the hard absolute limit");
                expect(r.engaged.rmsDbfs > -180.0 && r.engaged.rmsDbfs < 48.0,
                    r.pedal + " reference RMS dBFS must stay in a possible range");
                expect(std::isfinite(r.engagedMinusBypassDb),
                    r.pedal + " unity delta must be reportable");
            }
        }

        beginTest("P14B bypass measurement preserves dry behavior on all targets");
        {
            const auto input = P14BMeasurement::createReferenceSignal();

            for (const auto& pedal : P14BMeasurement::targetPedals())
            {
                auto processor = PedalRegistry::createPedal(pedal);
                expect(processor != nullptr, "P14B bypass check must construct " + pedal);
                if (processor == nullptr)
                    continue;

                P14BMeasurement::configureReference(*processor, pedal);
                if (auto* base = dynamic_cast<ProcessorBase*>(processor.get()))
                    base->setBypassed(true);
                P14BMeasurement::prepare(*processor);

                const auto output = P14BMeasurement::render(*processor, input);
                const double nullRms = P14BMeasurement::computeWindowNullRms(output, input);
                expect(bufferHasOnlyFiniteSamples(output), pedal + " bypass output must stay finite");
                expect(nullRms < 1.0e-7,
                    pedal + " bypass render should preserve current dry behavior");
            }
        }

        beginTest("P14B single-control sweeps cover intended controls");
        {
            sweeps = P14BMeasurement::buildSweepBaselines();
            const auto expected = P14BMeasurement::sweepSpecs();
            expectEquals((int) sweeps.size(), (int) expected.size(),
                "Sweep report count must match the intended control matrix");

            juce::StringArray seen;
            for (const auto& sweep : sweeps)
            {
                seen.addIfNotAlreadyThere(sweep.pedal + ":" + sweep.controlId);
                expectEquals((int) sweep.steps.size(), P14BMeasurement::kSweepSteps,
                    sweep.pedal + " " + sweep.controlId + " must render every sweep step");
                expect(sweep.finite,
                    sweep.pedal + " " + sweep.controlId + " sweep metrics must stay finite");
                expect(std::isfinite(sweep.maxAdjacentJumpDb) && sweep.maxAdjacentJumpDb < 120.0,
                    sweep.pedal + " " + sweep.controlId + " adjacent level jump must stay broadly sane");

                for (const auto& step : sweep.steps)
                {
                    expect(step.output.finite,
                        sweep.pedal + " " + sweep.controlId + " step must be finite");
                    expect(step.output.peak < Nova::Config::HARD_ABS_LIMIT_LINEAR,
                        sweep.pedal + " " + sweep.controlId + " step peak must stay below hard absolute limit");
                }
            }

            for (const auto& spec : expected)
                expect(seen.contains(spec.pedal + ":" + spec.controlId),
                    "Sweep report must include " + spec.pedal + ":" + spec.controlId);
        }

        beginTest("P14B drive spectral proxy renders finite values without aliasing claims");
        {
            spectrals = P14BMeasurement::buildSpectralBaselines();
            expectEquals((int) spectrals.size(), 8,
                "Four drive pedals must each report 1 kHz and 5 kHz spectral proxies");

            for (const auto& spectral : spectrals)
            {
                expect(spectral.finite,
                    spectral.pedal + " " + juce::String(spectral.sineFrequencyHz, 0)
                        + " Hz spectral proxy must be finite");
                expect(!spectral.robustFftAvailable,
                    "P14B spectral baseline must explicitly report no robust FFT path");
                expect(spectral.method.contains("no_aliasing_claim"),
                    "P14B spectral method must avoid aliasing claims");
            }
        }

        beginTest("P14B Compressor computeGainReductionDb analytic cases");
        {
            struct Case
            {
                float inputDb;
                float thresholdDb;
                float ratio;
                float kneeDb;
                float expectedDb;
            };

            const std::array<Case, 5> cases {{
                { -30.0f, -20.0f, 4.0f, 6.0f, 0.0f },
                { -10.0f, -20.0f, 4.0f, 6.0f, 7.5f },
                { -20.0f, -20.0f, 4.0f, 6.0f, 0.5625f },
                { -6.0f, -18.0f, 1.0f, 6.0f, 0.0f },
                { 0.0f, -24.0f, 12.0f, 1.0f, 22.0f }
            }};

            for (const auto& c : cases)
            {
                const float actual = CompressorPedal::computeGainReductionDb(
                    c.inputDb,
                    c.thresholdDb,
                    c.ratio,
                    c.kneeDb);

                expectWithinAbsoluteError(actual, c.expectedDb, 1.0e-4f,
                    "Compressor static gain-reduction law must match the documented analytic case");
            }
        }

        beginTest("P14B Noise Gate ramp/sustain probe records attenuation and lookahead");
        {
            gateProbe = P14BMeasurement::runGateProbe();
            expect(gateProbe.finite, "Gate ramp/sustain probe must stay finite");
            expect(gateProbe.observedLookaheadSamples >= 16 && gateProbe.observedLookaheadSamples <= 192,
                "Gate observed lookahead should stay within the processor's documented clamp");
            expect(gateProbe.openAttenuationDb > -12.0 && gateProbe.openAttenuationDb < 6.0,
                "Gate sustain/open attenuation should remain broadly plausible");
            expect(gateProbe.closedAttenuationDb < -20.0,
                "Gate closed attenuation should be clearly below active sustain");
        }

        beginTest("P14B structured report output is deterministic and complete");
        {
            if (references.empty())
                references = P14BMeasurement::buildReferenceBaselines();
            if (sweeps.empty())
                sweeps = P14BMeasurement::buildSweepBaselines();
            if (spectrals.empty())
                spectrals = P14BMeasurement::buildSpectralBaselines();
            if (!gateProbe.finite)
                gateProbe = P14BMeasurement::runGateProbe();

            const auto report = P14BMeasurement::buildReportJson(references, sweeps, spectrals, gateProbe, false);
            const auto reportAgain = P14BMeasurement::buildReportJson(references, sweeps, spectrals, gateProbe, false);

            expect(report == reportAgain, "P14B report JSON must be deterministic for the same measurements");
            expect(report.contains("nova.p14b_p1.drive_dynamics_measurement.v1"),
                "P14B report JSON must include a stable schema id");
            expect(report.contains("\"metadata\""), "P14B report JSON must include build/run metadata");
            expect(report.contains("\"generatedAt\": \"<volatile>\""),
                "P14B deterministic report mode must replace volatile timestamps");
            expect(report.contains("\"referenceBaselines\""), "P14B report JSON must include reference baselines");
            expect(report.contains("\"sweeps\""), "P14B report JSON must include parameter sweeps");
            expect(report.contains("\"spectralProxies\""), "P14B report JSON must include spectral proxy results");
            expect(report.contains("\"gateProbe\""), "P14B report JSON must include the gate probe");
            expect(report.contains("\"normalUnitTestsWriteFiles\": false"),
                "P14B report JSON must document that normal unit tests do not write artifacts");
        }
    }
};
#endif

class P10CHighGainProfessionalizationTests final : public juce::UnitTest
{
public:
    P10CHighGainProfessionalizationTests()
        : juce::UnitTest("P10C High-Gain Professionalization Diagnostics", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("p10c_high_gain_amp_nominal_palm_mute");
        {
            HighGainAmp amp;
            prepareP10CProcessor(amp);
            setRangedParamById(amp, "hgDrive", 7.2f);
            setRangedParamById(amp, "hgTight", 0.78f);
            setRangedParamById(amp, "hgPresence", 0.58f);
            setRangedParamById(amp, "hgTone", 0.52f);
            setRangedParamById(amp, "hgLevel", 0.68f);

            const auto reports = renderP10CChain({ { "high_gain_amp", &amp } }, 220);
            expectP10CStageHealthy(reports, "high_gain_amp_nominal_palm_mute");
            expect(reports.front().metrics.rms() > 0.015,
                "P10C high-gain amp nominal palm mute should remain audible: " + p10cMetricsSummary(reports.front().metrics));
        }

        beginTest("p10c_high_gain_amp_extreme_gain_bounded");
        {
            HighGainAmp amp;
            prepareP10CProcessor(amp);
            setRangedParamById(amp, "hgDrive", 10.0f);
            setRangedParamById(amp, "hgTight", 0.15f);
            setRangedParamById(amp, "hgPresence", 1.0f);
            setRangedParamById(amp, "hgTone", 1.0f);
            setRangedParamById(amp, "hgLevel", 1.25f);

            const auto reports = renderP10CChain({ { "high_gain_amp_extreme", &amp } }, 220, 1.20f);
            expectP10CStageHealthy(reports, "high_gain_amp_extreme_gain_bounded", 0.65);
            expect(reports.front().metrics.clippedSamples < juce::jmax(1, reports.front().metrics.sampleCount) / 2,
                "P10C high-gain amp extreme case should not collapse into sustained hard clipping: "
                    + p10cMetricsSummary(reports.front().metrics));
        }

        beginTest("p10c_distortion_highgainamp_modern4x12_nominal");
        {
            DistortionPedal distortion;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);

            distortion.modeParam->setValueNotifyingHost(normalisedChoiceIndex(distortion.modeParam, 4));
            distortion.gainParam->setValueNotifyingHost(distortion.gainParam->convertTo0to1(34.0f));
            distortion.toneParam->setValueNotifyingHost(distortion.toneParam->convertTo0to1(0.48f));
            distortion.bodyParam->setValueNotifyingHost(distortion.bodyParam->convertTo0to1(0.46f));
            distortion.tightParam->setValueNotifyingHost(distortion.tightParam->convertTo0to1(0.76f));
            distortion.levelParam->setValueNotifyingHost(distortion.levelParam->convertTo0to1(0.22f));
            distortion.mixParam->setValueNotifyingHost(distortion.mixParam->convertTo0to1(1.0f));
            setRangedParamById(amp, "hgDrive", 6.2f);
            setRangedParamById(amp, "hgTight", 0.82f);
            setRangedParamById(amp, "hgPresence", 0.52f);
            setRangedParamById(amp, "hgLevel", 0.58f);
            setRangedParamById(cab, "m4x12Low", 1.5f);
            setRangedParamById(cab, "m4x12Presence", 2.0f);
            setRangedParamById(cab, "m4x12Level", 0.78f);

            const auto reports = renderP10CChain({ { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } }, 240);
            expectP10CStageHealthy(reports, "distortion_highgainamp_modern4x12_nominal", 0.50);
            expect(reports.back().metrics.nearClipSamples == 0,
                "P10C nominal Distortion -> HighGain -> Modern4x12 should not need near-clip final cabinet energy: "
                    + p10cMetricsSummary(reports.back().metrics));
        }

        beginTest("p10c_boost_highgainamp_modern4x12_nominal");
        {
            BoostPedal boost;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(boost);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);

            setRangedParamById(boost, "boostGain", 8.0f);
            setRangedParamById(boost, "boostTight", 0.72f);
            setRangedParamById(boost, "boostTone", 0.54f);
            setRangedParamById(boost, "boostLevel", 0.74f);
            setRangedParamById(amp, "hgDrive", 7.4f);
            setRangedParamById(amp, "hgTight", 0.84f);
            setRangedParamById(amp, "hgPresence", 0.56f);
            setRangedParamById(amp, "hgLevel", 0.56f);
            setRangedParamById(cab, "m4x12Low", 1.0f);
            setRangedParamById(cab, "m4x12Presence", 2.2f);
            setRangedParamById(cab, "m4x12Level", 0.76f);

            const auto reports = renderP10CChain({ { "boost", &boost }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } }, 240);
            expectP10CStageHealthy(reports, "boost_highgainamp_modern4x12_nominal", 0.55);
            expect(reports[0].metrics.peak < 1.8,
                "P10C nominal boost should push the amp without destructive pedal output: " + p10cMetricsSummary(reports[0].metrics));
        }

        beginTest("p10c_fuzz_classicamp_cabinet_nominal");
        {
            FuzzPedal fuzz;
            ClassicAmp amp;
            CabinetPedal cab;
            prepareP10CProcessor(fuzz);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);

            fuzz.fuzzParam->setValueNotifyingHost(fuzz.fuzzParam->convertTo0to1(58.0f));
            fuzz.toneParam->setValueNotifyingHost(fuzz.toneParam->convertTo0to1(0.44f));
            fuzz.levelParam->setValueNotifyingHost(fuzz.levelParam->convertTo0to1(0.44f));
            fuzz.mixParam->setValueNotifyingHost(fuzz.mixParam->convertTo0to1(1.0f));
            setRangedParamById(amp, "ampDrive", 3.7f);
            setRangedParamById(amp, "ampPresence", 0.48f);
            setRangedParamById(amp, "ampLevel", 0.66f);
            setRangedParamById(cab, "cabThump", 1.0f);
            setRangedParamById(cab, "cabAir", 0.5f);
            setRangedParamById(cab, "cabLevel", 0.82f);

            const auto reports = renderP10CChain({ { "fuzz", &fuzz }, { "classic_amp", &amp }, { "cabinet", &cab } }, 240);
            expectP10CStageHealthy(reports, "fuzz_classicamp_cabinet_nominal", 0.60);
        }

        beginTest("p10c_distortion_cleanamp_cabinet_nominal");
        {
            DistortionPedal distortion;
            CleanAmp amp;
            CabinetPedal cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);

            distortion.modeParam->setValueNotifyingHost(normalisedChoiceIndex(distortion.modeParam, 3));
            distortion.gainParam->setValueNotifyingHost(distortion.gainParam->convertTo0to1(30.0f));
            distortion.toneParam->setValueNotifyingHost(distortion.toneParam->convertTo0to1(0.50f));
            distortion.bodyParam->setValueNotifyingHost(distortion.bodyParam->convertTo0to1(0.50f));
            distortion.tightParam->setValueNotifyingHost(distortion.tightParam->convertTo0to1(0.64f));
            distortion.levelParam->setValueNotifyingHost(distortion.levelParam->convertTo0to1(0.24f));
            distortion.mixParam->setValueNotifyingHost(distortion.mixParam->convertTo0to1(1.0f));
            setRangedParamById(amp, "cleanDrive", 0.38f);
            setRangedParamById(amp, "cleanLevel", 0.72f);
            setRangedParamById(cab, "cabThump", 0.5f);
            setRangedParamById(cab, "cabAir", 0.0f);
            setRangedParamById(cab, "cabLevel", 0.84f);

            const auto reports = renderP10CChain({ { "distortion", &distortion }, { "clean_amp", &amp }, { "cabinet", &cab } }, 240);
            expectP10CStageHealthy(reports, "distortion_cleanamp_cabinet_nominal", 0.50);
            expect(reports.front().metrics.peak < 1.05,
                "P10C Distortion into clean amp should be contained before the amp: " + p10cMetricsSummary(reports.front().metrics));
        }

        beginTest("p10c_high_gain_chain_bypass_recovery");
        {
            BoostPedal boost;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            OutputChainProcessor output;
            prepareP10CProcessor(boost);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            output.prepareToPlay(kSampleRate, kBlockSize);
            output.setParams(-6.0f, 0.0f);

            setRangedParamById(boost, "boostGain", 7.0f);
            setRangedParamById(boost, "boostTight", 0.74f);
            setRangedParamById(boost, "boostLevel", 0.72f);
            setRangedParamById(amp, "hgDrive", 7.6f);
            setRangedParamById(amp, "hgTight", 0.82f);
            setRangedParamById(amp, "hgPresence", 0.54f);
            setRangedParamById(amp, "hgLevel", 0.56f);
            setRangedParamById(cab, "m4x12Level", 0.76f);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block(2, kBlockSize);
            P10CWindowMetrics active;
            P10CWindowMetrics recovery;

            for (int blockIndex = 0; blockIndex < 260; ++blockIndex)
            {
                boost.setBypassed(blockIndex >= 120 && blockIndex < 170);
                fillP10CPalmMuteBlock(block, blockIndex);
                boost.processBlock(block, midi);
                amp.processBlock(block, midi);
                cab.processBlock(block, midi);
                output.processBlock(block, midi);

                if (blockIndex >= 60 && blockIndex < 115)
                    active.capture(block);
                else if (blockIndex >= 205)
                    recovery.capture(block);
            }

            expect(active.finite && recovery.finite,
                "P10C high-gain bypass recovery must remain finite");
            expect(recovery.peak <= juce::jmax(0.35, active.peak * 1.35),
                "P10C high-gain bypass recovery should return near active-chain headroom; active="
                    + p10cMetricsSummary(active) + ", recovery=" + p10cMetricsSummary(recovery));
            expect(recovery.adjacentDeltaPeak < 0.75,
                "P10C high-gain bypass recovery should not produce harsh transition spikes: "
                    + p10cMetricsSummary(recovery));
        }

        beginTest("p10c_high_gain_chain_outputchain_limiter_independence");
        {
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            OutputChainProcessor output;
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            output.prepareToPlay(kSampleRate, kBlockSize);
            output.setParams(-6.0f, 0.0f);

            setRangedParamById(amp, "hgDrive", 7.2f);
            setRangedParamById(amp, "hgTight", 0.82f);
            setRangedParamById(amp, "hgPresence", 0.54f);
            setRangedParamById(amp, "hgLevel", 0.58f);
            setRangedParamById(cab, "m4x12Low", 1.0f);
            setRangedParamById(cab, "m4x12Presence", 2.0f);
            setRangedParamById(cab, "m4x12Level", 0.78f);

            int limiterTouchedSamples = 0;
            int limiterActiveBlocks = 0;
            int sustainedClampBlocks = 0;
            const auto reports = renderP10CChain({ { "high_gain_amp", &amp }, { "modern_4x12", &cab }, { "output_chain", &output } },
                240, 1.0f, &output, &limiterTouchedSamples, &limiterActiveBlocks, &sustainedClampBlocks);

            expectP10CStageHealthy(reports, "high_gain_chain_outputchain_limiter_independence", 0.50);
            expect(limiterTouchedSamples == 0 && limiterActiveBlocks == 0 && sustainedClampBlocks == 0,
                "P10C high-gain nominal chain should be staged before OutputChain limiting; limiterTouchedSamples="
                    + juce::String(limiterTouchedSamples)
                    + ", limiterActiveBlocks="
                    + juce::String(limiterActiveBlocks)
                    + ", sustainedClampBlocks="
                    + juce::String(sustainedClampBlocks)
                    + ", output="
                    + p10cMetricsSummary(reports.back().metrics));
        }
    }

private:
    void expectP10CStageHealthy(const std::vector<P10CStageMetrics>& reports,
                                const juce::String& scenario,
                                double maxDc = 0.75)
    {
        expect(!reports.empty(), "P10C scenario must emit stage reports: " + scenario);
        for (const auto& report : reports)
        {
            const auto& m = report.metrics;
            expect(m.finite && m.invalidSamples == 0,
                "P10C " + scenario + " stage " + report.name + " must remain finite: " + p10cMetricsSummary(m));
            expect(m.peak <= (double) Nova::Config::HARD_ABS_LIMIT_LINEAR,
                "P10C " + scenario + " stage " + report.name + " must stay inside hard absolute limit: " + p10cMetricsSummary(m));
            expect(m.dc() < maxDc,
                "P10C " + scenario + " stage " + report.name + " should not accumulate large DC: " + p10cMetricsSummary(m));
            expect(m.adjacentDeltaPeak < 12.0,
                "P10C " + scenario + " stage " + report.name + " should not emit destructive adjacent deltas: " + p10cMetricsSummary(m));
            expect(m.brightnessProxy() < 8.0,
                "P10C " + scenario + " stage " + report.name + " brightness proxy should remain bounded: " + p10cMetricsSummary(m));
            expect(m.rumbleProxy() < 1.25,
                "P10C " + scenario + " stage " + report.name + " rumble proxy should remain bounded: " + p10cMetricsSummary(m));
        }
    }
};

class P10DHighGainArtifactFizzHelicopterTests final : public juce::UnitTest
{
public:
    P10DHighGainArtifactFizzHelicopterTests()
        : juce::UnitTest("P10D High-Gain Artifact/Fizz/Helicopter Diagnostics", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("high_gain_helicopter_modulation_guard");
        {
            HighGainAmp ampOnly;
            prepareP10CProcessor(ampOnly);
            configureHighGainAmp(ampOnly);
            const auto ampOnlyReports = renderP10DChain({ { "high_gain_amp", &ampOnly } },
                760, P10DSignalKind::SustainLong, 1.0f, 48);
            expectSustainStable(ampOnlyReports.back(), "HighGainAmp sustain");

            HighGainAmp ampCab;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(ampCab);
            prepareP10CProcessor(cab);
            configureHighGainAmp(ampCab);
            configureModernCab(cab);
            const auto ampCabReports = renderP10DChain({ { "high_gain_amp", &ampCab }, { "modern_4x12", &cab } },
                760, P10DSignalKind::SustainLong, 1.0f, 48);
            expectSustainStable(ampCabReports.back(), "HighGainAmp -> Modern4x12 sustain");

            BoostPedal boost;
            HighGainAmp boostedAmp;
            Modern4x12Cabinet boostedCab;
            prepareP10CProcessor(boost);
            prepareP10CProcessor(boostedAmp);
            prepareP10CProcessor(boostedCab);
            configureBoost(boost);
            configureHighGainAmp(boostedAmp);
            configureModernCab(boostedCab);
            const auto boostReports = renderP10DChain({ { "boost", &boost }, { "high_gain_amp", &boostedAmp }, { "modern_4x12", &boostedCab } },
                760, P10DSignalKind::SustainLong, 0.92f, 48);
            expectSustainStable(boostReports.back(), "Boost -> HighGainAmp -> Modern4x12 sustain");
        }

        beginTest("high_gain_fizz_brightness_guard");
        {
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10DChain({ { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                360, P10DSignalKind::StrongChord, 1.15f, 36);
            expectProfessionalFinal(reports.back(), "HighGainAmp -> Modern4x12 brightness");
            expect(reports.back().metrics.highFrequencyEnergyProxy() < 0.040,
                "P10D cabinet should keep high-gain fizz proxy inside a bounded post-cab range; amp="
                    + p10dMetricsSummary(reports.front().metrics)
                    + ", cab="
                    + p10dMetricsSummary(reports.back().metrics));
            expect(reports.back().metrics.signal.brightnessProxy() < 0.85,
                "P10D post-cab brightness proxy must remain bounded: " + p10dMetricsSummary(reports.back().metrics));
        }

        beginTest("high_gain_strong_input_no_clipping");
        {
            assertStrongInputChain("high_gain_manual_warn_repro_nominal", P10DSignalKind::PalmMuteRepeated, 1.30f);
            assertStrongInputChain("high_gain_palm_mute_strong_input", P10DSignalKind::LowEBurst, 1.45f);
            assertStrongInputChain("high_gain_sustain_strong_input", P10DSignalKind::SustainLong, 1.32f);
            assertStrongInputChain("high_gain_chord_strong_input", P10DSignalKind::StrongChord, 1.18f);
        }

        beginTest("tight_modern_rhythm_high_gain_artifact_guard");
        {
            NoiseGatePedal gate;
            BoostPedal boost;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(gate);
            prepareP10CProcessor(boost);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureTightModernGate(gate);
            configureBoost(boost);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10DChain(
                { { "noise_gate", &gate }, { "boost", &boost }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                620, P10DSignalKind::SilenceRecovery, 1.05f, 24);
            expectProfessionalFinal(reports.back(), "Tight Modern Rhythm artifact guard");
            expect(reports.front().metrics.gateTransitions <= 18 && reports.front().metrics.gateDeltaPeak < 0.82,
                "P10D Noise Gate should not chatter in the tight-modern recovery pattern: "
                    + p10dMetricsSummary(reports.front().metrics));
            expect(reports.back().metrics.signal.nearClipSamples == 0 && reports.back().metrics.signal.clippedSamples == 0,
                "P10D Tight Modern Rhythm phrase/recovery pattern should stay bounded without clipping: "
                    + p10dMetricsSummary(reports.back().metrics));
        }

        beginTest("distortion_highgain_modern4x12_professional_bounds");
        {
            DistortionPedal distortion;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureStudioDistortion(distortion);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10DChain(
                { { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                620, P10DSignalKind::PalmMuteRepeated, 1.08f, 32);
            expectProfessionalFinal(reports.back(), "Distortion -> HighGainAmp -> Modern4x12 professional bounds");
            expect(reports.front().metrics.gateTransitions <= 16 && reports.front().metrics.gateDeltaPeak < 0.55,
                "P10D integrated distortion gate should not chatter into the amp: "
                    + p10dMetricsSummary(reports.front().metrics));
            expect(reports.back().metrics.highFrequencyEnergyProxy() < 0.58,
                "P10D Distortion -> HighGainAmp -> Modern4x12 should keep fizz proxy bounded: "
                    + p10dMetricsSummary(reports.back().metrics));
        }

        beginTest("high_gain_noise_gate_chatter_guard");
        {
            NoiseGatePedal gate;
            prepareP10CProcessor(gate);
            configureTightModernGate(gate);

            const auto reports = renderP10DChain({ { "noise_gate", &gate } },
                620, P10DSignalKind::SilenceRecovery, 1.0f, 24);
            expect(reports.front().metrics.signal.finite && reports.front().metrics.signal.invalidSamples == 0,
                "P10D noise gate chatter render must remain finite: " + p10dMetricsSummary(reports.front().metrics));
            expect(reports.front().metrics.gateTransitions <= 18,
                "P10D noise gate should not repeatedly open/close around phrase tails: "
                    + p10dMetricsSummary(reports.front().metrics));
            expect(reports.front().metrics.gateDeltaPeak < 0.82,
                "P10D noise gate gain should move without hard tremolo steps: " + p10dMetricsSummary(reports.front().metrics));
        }

        beginTest("modern4x12_high_gain_fizz_control");
        {
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10DChain({ { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                360, P10DSignalKind::LowEBurst, 1.20f, 36);
            expect(reports.back().metrics.highFrequencyEnergyProxy() < 0.040,
                "P10D Modern4x12 must keep high-gain fizz proxy inside a bounded post-cab range; amp="
                    + p10dMetricsSummary(reports.front().metrics)
                    + ", cab="
                    + p10dMetricsSummary(reports.back().metrics));
            expectProfessionalFinal(reports.back(), "Modern4x12 high-gain fizz control");
        }
    }

private:
    static void configureHighGainAmp(HighGainAmp& amp)
    {
        setRangedParamById(amp, "hgDrive", 7.4f);
        setRangedParamById(amp, "hgTight", 0.82f);
        setRangedParamById(amp, "hgPresence", 0.52f);
        setRangedParamById(amp, "hgTone", 0.50f);
        setRangedParamById(amp, "hgLevel", 0.58f);
    }

    static void configureModernCab(Modern4x12Cabinet& cab)
    {
        setRangedParamById(cab, "m4x12Low", 1.0f);
        setRangedParamById(cab, "m4x12Presence", 1.6f);
        setRangedParamById(cab, "m4x12Distance", 0.34f);
        setRangedParamById(cab, "m4x12Level", 0.78f);
        setRangedParamById(cab, "m4x12Mix", 1.0f);
    }

    static void configureBoost(BoostPedal& boost)
    {
        setRangedParamById(boost, "boostGain", 7.0f);
        setRangedParamById(boost, "boostTight", 0.76f);
        setRangedParamById(boost, "boostTone", 0.50f);
        setRangedParamById(boost, "boostLevel", 0.70f);
    }

    static void configureStudioDistortion(DistortionPedal& distortion)
    {
        distortion.modeParam->setValueNotifyingHost(normalisedChoiceIndex(distortion.modeParam, 4));
        distortion.gainParam->setValueNotifyingHost(distortion.gainParam->convertTo0to1(36.0f));
        distortion.toneParam->setValueNotifyingHost(distortion.toneParam->convertTo0to1(0.45f));
        distortion.bodyParam->setValueNotifyingHost(distortion.bodyParam->convertTo0to1(0.46f));
        distortion.tightParam->setValueNotifyingHost(distortion.tightParam->convertTo0to1(0.74f));
        distortion.levelParam->setValueNotifyingHost(distortion.levelParam->convertTo0to1(0.21f));
        distortion.mixParam->setValueNotifyingHost(distortion.mixParam->convertTo0to1(1.0f));
    }

    static void configureTightModernGate(NoiseGatePedal& gate)
    {
        gate.thresholdParam->setValueNotifyingHost(gate.thresholdParam->convertTo0to1(-48.0f));
        gate.attackParam->setValueNotifyingHost(gate.attackParam->convertTo0to1(0.32f));
        gate.holdParam->setValueNotifyingHost(gate.holdParam->convertTo0to1(92.0f));
        gate.releaseParam->setValueNotifyingHost(gate.releaseParam->convertTo0to1(155.0f));
        gate.rangeParam->setValueNotifyingHost(gate.rangeParam->convertTo0to1(-78.0f));
        gate.hysteresisParam->setValueNotifyingHost(gate.hysteresisParam->convertTo0to1(10.5f));
        gate.focusParam->setValueNotifyingHost(gate.focusParam->convertTo0to1(0.62f));
    }

    void assertStrongInputChain(const juce::String& scenario, P10DSignalKind signalKind, float inputScale)
    {
        BoostPedal boost;
        HighGainAmp amp;
        Modern4x12Cabinet cab;
        OutputChainProcessor output;
        prepareP10CProcessor(boost);
        prepareP10CProcessor(amp);
        prepareP10CProcessor(cab);
        output.prepareToPlay(kSampleRate, kBlockSize);
        output.setParams(-6.0f, 0.0f);
        configureBoost(boost);
        configureHighGainAmp(amp);
        configureModernCab(cab);

        int limiterTouchedSamples = 0;
        int limiterActiveBlocks = 0;
        int sustainedClampBlocks = 0;
        const auto reports = renderP10DChain(
            { { "boost", &boost }, { "high_gain_amp", &amp }, { "modern_4x12", &cab }, { "output_chain", &output } },
            430, signalKind, inputScale, 28, &output, &limiterTouchedSamples, &limiterActiveBlocks, &sustainedClampBlocks);

        const auto& final = reports.back().metrics;
        expect(final.signal.finite && final.signal.invalidSamples == 0,
            "P10D " + scenario + " must remain finite: " + p10dMetricsSummary(final));
        expect(final.signal.clippedSamples == 0 && final.signal.nearClipSamples == 0 && final.signal.peak < 0.98,
            "P10D " + scenario + " should keep strong input below near-clip at final output: " + p10dMetricsSummary(final));
        expect(limiterTouchedSamples == 0 && limiterActiveBlocks == 0 && sustainedClampBlocks == 0,
            "P10D " + scenario + " must not depend on OutputChain limiter masking; limiterTouchedSamples="
                + juce::String(limiterTouchedSamples)
                + ", limiterActiveBlocks="
                + juce::String(limiterActiveBlocks)
                + ", sustainedClampBlocks="
                + juce::String(sustainedClampBlocks)
                + ", final="
                + p10dMetricsSummary(final));
    }

    void expectSustainStable(const P10DStageMetrics& report, const juce::String& label)
    {
        const auto& m = report.metrics;
        expect(m.signal.finite && m.signal.invalidSamples == 0,
            "P10D " + label + " must remain finite: " + p10dMetricsSummary(m));
        expect(m.modulationDepth3To20() < 0.34,
            "P10D " + label + " should not show helicopter-like 3-20 Hz amplitude modulation: " + p10dMetricsSummary(m));
        expect(m.blockRmsVarianceProxy() < 0.82,
            "P10D " + label + " should not have excessive block-to-block RMS variance: " + p10dMetricsSummary(m));
        expect(m.signal.clippedSamples == 0,
            "P10D " + label + " should not clip under sustained high-gain input: " + p10dMetricsSummary(m));
    }

    void expectProfessionalFinal(const P10DStageMetrics& report, const juce::String& label)
    {
        const auto& m = report.metrics;
        expect(m.signal.finite && m.signal.invalidSamples == 0,
            "P10D " + label + " final output must remain finite: " + p10dMetricsSummary(m));
        expect(m.signal.clippedSamples == 0,
            "P10D " + label + " final output must not hard clip: " + p10dMetricsSummary(m));
        expect(m.signal.nearClipSamples == 0 && m.signal.peak < 0.98,
            "P10D " + label + " final output should retain strong-input headroom: " + p10dMetricsSummary(m));
        expect(m.signal.dc() < 0.025,
            "P10D " + label + " final output should stay low-DC: " + p10dMetricsSummary(m));
        expect(m.signal.adjacentDeltaPeak < 0.95,
            "P10D " + label + " final output should avoid harsh adjacent-sample jumps: " + p10dMetricsSummary(m));
    }
};

class P10EHighGainMuteHelicopterReverbTests final : public juce::UnitTest
{
public:
    P10EHighGainMuteHelicopterReverbTests()
        : juce::UnitTest("P10E High-Gain Mute/Helicopter/Reverb Follow-up", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("p10e_distortion_highgain_mute_repro");
        {
            DistortionPedal distortion;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10EDistortion(distortion);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto reports = renderP10ELongChain(
                { { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                1700, P10DSignalKind::SilenceRecovery, 1.16f, 36);
            expectNoUnexpectedMute(reports.back(), "Distortion -> HighGainAmp -> Modern4x12");
            expectBoundedFinal(reports.back(), "Distortion -> HighGainAmp -> Modern4x12");
            expect(reports.front().silentWhileInputBlocks <= 8,
                "P10E Distortion stage should not enter a stuck muted state while receiving input: "
                    + p10eLongSummary(reports.front()));
        }

        beginTest("p10e_distortion_cleanamp_helicopter_repro");
        {
            DistortionPedal distortion;
            CleanAmp amp;
            CabinetPedal cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10EDistortion(distortion);
            configureP10ECleanAmp(amp);
            configureP10EClassicCab(cab);

            const auto reports = renderP10ELongChain(
                { { "distortion", &distortion }, { "clean_amp", &amp }, { "cabinet", &cab } },
                1500, P10DSignalKind::PalmMuteRepeated, 1.14f, 36);
            expectNoUnexpectedMute(reports.back(), "Distortion -> CleanAmp -> Cabinet");
            expectNoHelicopter(reports.back(), "Distortion -> CleanAmp -> Cabinet", 0.74);
            expectBoundedFinal(reports.back(), "Distortion -> CleanAmp -> Cabinet");
        }

        beginTest("p10e_distortion_reverb_helicopter_guard");
        {
            DistortionPedal distortion;
            ReverbPedal reverb;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(reverb);
            configureP10EDistortion(distortion);
            configureP10EReverb(reverb);

            const auto reports = renderP10ELongChain(
                { { "distortion", &distortion }, { "reverb", &reverb } },
                1350, P10DSignalKind::StrongChord, 1.22f, 36, 760);
            expectNoUnexpectedMute(reports.back(), "Distortion -> Reverb");
            expectNoHelicopter(reports.back(), "Distortion -> Reverb", 0.78);
            expectTailRecovered(reports.back(), "Distortion -> Reverb");
            expectBoundedFinal(reports.back(), "Distortion -> Reverb");

            ReverbPedal saturatedReverb;
            prepareP10CProcessor(saturatedReverb);
            configureP10EReverb(saturatedReverb);
            const auto soloReports = renderP10ELongChain(
                { { "reverb_saturated_input", &saturatedReverb } },
                1220, P10DSignalKind::StrongChord, 1.95f, 36, 640);
            expectNoHelicopter(soloReports.back(), "Reverb saturated-input recovery", 0.82);
            expectTailRecovered(soloReports.back(), "Reverb saturated-input recovery");
            expectBoundedFinal(soloReports.back(), "Reverb saturated-input recovery");
        }

        beginTest("p10e_distortion_reverb_chorus_recovery_guard");
        {
            DistortionPedal distortion;
            ReverbPedal reverb;
            ChorusPedal chorus;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(reverb);
            prepareP10CProcessor(chorus);
            configureP10EDistortion(distortion);
            configureP10EReverb(reverb);
            configureP10EChorus(chorus);

            const auto reports = renderP10ELongChain(
                { { "distortion", &distortion }, { "reverb", &reverb }, { "chorus", &chorus } },
                1400, P10DSignalKind::SilenceRecovery, 1.16f, 36, 760);
            expectNoUnexpectedMute(reports.back(), "Distortion -> Reverb -> Chorus");
            expectNoHelicopter(reports.back(), "Distortion -> Reverb -> Chorus", 1.45, 2.65);
            expectTailRecovered(reports.back(), "Distortion -> Reverb -> Chorus");
            expectBoundedFinal(reports.back(), "Distortion -> Reverb -> Chorus");
        }

        beginTest("p10e_boost_highgain_noise_clipping_guard");
        {
            BoostPedal boost;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(boost);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10EBoost(boost);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto reports = renderP10ELongChain(
                { { "boost", &boost }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                1500, P10DSignalKind::LowEBurst, 1.32f, 36, 840);
            expectBoundedFinal(reports.front(), "Boost stage into HighGainAmp");
            expectNoUnexpectedMute(reports.back(), "Boost -> HighGainAmp -> Modern4x12");
            expectBoundedFinal(reports.back(), "Boost -> HighGainAmp -> Modern4x12");
            expect(reports.back().tailRms < 0.012,
                "P10E Boost -> HighGainAmp should not leave a ground-like noise floor after silence: "
                    + p10eLongSummary(reports.back()));
            expect(reports.back().metrics.highFrequencyEnergyProxy() < 0.075,
                "P10E Boost -> HighGainAmp should keep fizz proxy bounded after the cabinet: "
                    + p10eLongSummary(reports.back()));
        }

        beginTest("p10e_fuzz_classicamp_stuck_silence_guard");
        {
            FuzzPedal fuzz;
            ClassicAmp amp;
            CabinetPedal cab;
            prepareP10CProcessor(fuzz);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10EFuzz(fuzz);
            configureP10EClassicAmp(amp);
            configureP10EClassicCab(cab);

            const auto reports = renderP10ELongChain(
                { { "fuzz", &fuzz }, { "classic_amp", &amp }, { "cabinet", &cab } },
                1700, P10DSignalKind::SilenceRecovery, 1.08f, 36);
            expectNoUnexpectedMute(reports.front(), "Fuzz stage");
            expectNoUnexpectedMute(reports.back(), "Fuzz -> ClassicAmp -> Cabinet");
            expectBoundedFinal(reports.back(), "Fuzz -> ClassicAmp -> Cabinet", 1.65);
        }

        beginTest("p10e_sample_rate_reset_recovers_stuck_chain");
        {
            DistortionPedal distortion;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10EDistortion(distortion);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto preResetReports = renderP10ELongChain(
                { { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                1050, P10DSignalKind::SilenceRecovery, 1.14f, 36);
            expectNoUnexpectedMute(preResetReports.back(), "Pre-reset Distortion -> HighGainAmp -> Modern4x12");

            prepareProcessorAt(distortion, 44100.0);
            prepareProcessorAt(amp, 44100.0);
            prepareProcessorAt(cab, 44100.0);

            const auto postResetReports = renderP10ELongChain(
                { { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                760, P10DSignalKind::PalmMuteRepeated, 1.08f, 28);
            expectNoUnexpectedMute(postResetReports.back(), "Post sample-rate-reset Distortion -> HighGainAmp -> Modern4x12");
            expect(postResetReports.back().finalActiveRms > preResetReports.back().finalActiveRms * 0.18,
                "P10E sample-rate reset should recover to an audible chain level, not a silent latch; pre="
                    + p10eLongSummary(preResetReports.back())
                    + ", post="
                    + p10eLongSummary(postResetReports.back()));
            expectBoundedFinal(postResetReports.back(), "Post sample-rate-reset Distortion -> HighGainAmp -> Modern4x12");
        }

        beginTest("p10e_tight_modern_rhythm_availability_doc_check");
        {
            const auto repoRoot = findRepoRoot();
            const auto generatedPreset = repoRoot.getChildFile("Resources")
                                                .getChildFile("Presets")
                                                .getChildFile("DraftFactory")
                                                .getChildFile("generated")
                                                .getChildFile("Tight-Modern-Rhythm.nova-preset");
            expect(repoRoot.exists() && generatedPreset.existsAsFile(),
                "P10E Tight Modern Rhythm draft preset should exist as generated draft artifact; repoRoot="
                    + repoRoot.getFullPathName()
                    + ", preset="
                    + generatedPreset.getFullPathName());
        }
    }

private:
    static void prepareProcessorAt(juce::AudioProcessor& processor, double sampleRate)
    {
        processor.setPlayConfigDetails(2, 2, sampleRate, kBlockSize);
        processor.prepareToPlay(sampleRate, kBlockSize);
    }

    static juce::File findRepoRoot()
    {
        auto dir = juce::File::getCurrentWorkingDirectory();
        for (int i = 0; i < 10; ++i)
        {
            if (dir.getChildFile("NOVA.jucer").existsAsFile())
                return dir;

            const auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        return juce::File::getCurrentWorkingDirectory();
    }

    static void configureP10ECleanAmp(CleanAmp& amp)
    {
        setRangedParamById(amp, "cleanDrive", 0.42f);
        setRangedParamById(amp, "cleanBass", 0.48f);
        setRangedParamById(amp, "cleanTreble", 0.50f);
        setRangedParamById(amp, "cleanReverb", 0.0f);
        setRangedParamById(amp, "cleanLevel", 0.64f);
    }

    static void configureP10EClassicAmp(ClassicAmp& amp)
    {
        setRangedParamById(amp, "ampDrive", 3.4f);
        setRangedParamById(amp, "ampTone", 0.46f);
        setRangedParamById(amp, "ampPresence", 0.42f);
        setRangedParamById(amp, "ampDepth", 0.42f);
        setRangedParamById(amp, "ampLevel", 0.58f);
    }

    static void configureP10EClassicCab(CabinetPedal& cab)
    {
        setRangedParamById(cab, "cabThump", 0.8f);
        setRangedParamById(cab, "cabAir", 0.7f);
        setRangedParamById(cab, "cabDistance", 0.36f);
        setRangedParamById(cab, "cabLevel", 0.72f);
        setRangedParamById(cab, "cabMix", 1.0f);
    }

    static void configureP10EChorus(ChorusPedal& chorus)
    {
        chorus.modeParam->setValueNotifyingHost(normalisedChoiceIndex(chorus.modeParam, 0));
        chorus.rateParam->setValueNotifyingHost(chorus.rateParam->convertTo0to1(0.46f));
        chorus.depthParam->setValueNotifyingHost(chorus.depthParam->convertTo0to1(0.30f));
        chorus.widthParam->setValueNotifyingHost(chorus.widthParam->convertTo0to1(0.54f));
        chorus.toneParam->setValueNotifyingHost(chorus.toneParam->convertTo0to1(0.46f));
        chorus.mixParam->setValueNotifyingHost(chorus.mixParam->convertTo0to1(0.22f));
        chorus.lagParam->setValueNotifyingHost(chorus.lagParam->convertTo0to1(6.5f));
    }

    void expectNoUnexpectedMute(const P10ELongStageMetrics& report, const juce::String& label)
    {
        expect(report.metrics.signal.finite && report.metrics.signal.invalidSamples == 0,
            "P10E " + label + " must remain finite: " + p10eLongSummary(report));
        expect(report.finalActiveRms > 0.0012,
            "P10E " + label + " should remain audible while active input is present: " + p10eLongSummary(report));
        expect(report.silentWhileInputBlocks <= 10 && report.maxConsecutiveSilentBlocks <= 4,
            "P10E " + label + " should not mute or latch closed while input is active: " + p10eLongSummary(report));
    }

    void expectNoHelicopter(const P10ELongStageMetrics& report,
                            const juce::String& label,
                            double maxModDepth,
                            double maxBlockVariance = 1.80)
    {
        expect(report.metrics.modulationDepth3To20() < maxModDepth,
            "P10E " + label + " should avoid helicopter-like 3-20 Hz amplitude modulation: "
                + p10eLongSummary(report));
        expect(report.metrics.blockRmsVarianceProxy() < maxBlockVariance,
            "P10E " + label + " should avoid excessive block-to-block RMS variance: "
                + p10eLongSummary(report));
    }

    void expectTailRecovered(const P10ELongStageMetrics& report, const juce::String& label)
    {
        const double allowedTail = juce::jmax(0.010, report.maxActiveRms * 0.42);
        expect(report.tailRms < allowedTail,
            "P10E " + label + " should recover after silence without runaway tail or tremolo floor: "
                + p10eLongSummary(report));
    }

    void expectBoundedFinal(const P10ELongStageMetrics& report,
                            const juce::String& label,
                            double maxAdjacentDelta = 1.15)
    {
        const auto& m = report.metrics;
        expect(m.signal.finite && m.signal.invalidSamples == 0,
            "P10E " + label + " output must remain finite: " + p10eLongSummary(report));
        expect(m.signal.clippedSamples == 0 && m.signal.nearClipSamples == 0 && m.signal.peak < 0.99,
            "P10E " + label + " should stay below near-clip without OutputChain masking: "
                + p10eLongSummary(report));
        expect(m.signal.dc() < 0.035,
            "P10E " + label + " should not accumulate DC: " + p10eLongSummary(report));
        expect(m.signal.adjacentDeltaPeak < maxAdjacentDelta,
            "P10E " + label + " should not emit harsh adjacent-sample jumps: "
                + p10eLongSummary(report));
    }
};

class P10FHighGainRootCauseFuzzReferenceTests final : public juce::UnitTest
{
public:
    P10FHighGainRootCauseFuzzReferenceTests()
        : juce::UnitTest("P10F High-Gain Architecture Root Fix / Fuzz Reference", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("p10f_fuzz_reference_gain_behavior");
        {
            FuzzPedal fuzz;
            ClassicAmp amp;
            CabinetPedal cab;
            prepareP10CProcessor(fuzz);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10FFuzzReference(fuzz);
            configureP10FClassicAmp(amp);
            configureP10FClassicCab(cab);

            const auto reports = renderP10FChain(
                { { "fuzz", &fuzz }, { "classic_amp", &amp }, { "cabinet", &cab } },
                620, 1.0f, 36);
            expectP10FReference(reports.back(), "Fuzz -> ClassicAmp -> Cabinet");
        }

        beginTest("p10f_distortion_gain_ducking_guard");
        {
            DistortionPedal distortion;
            CleanAmp amp;
            CabinetPedal cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10FHighGainDistortion(distortion);
            configureP10FCleanAmp(amp);
            configureP10FClassicCab(cab);

            const auto reports = renderP10FChain(
                { { "distortion", &distortion }, { "clean_amp", &amp }, { "cabinet", &cab } },
                620, 1.04f, 36);
            expectP10FNoVolumeCollapse(reports.front(), "Distortion stage into CleanAmp");
            expectP10FNoVolumeCollapse(reports.back(), "Distortion -> CleanAmp -> Cabinet");
        }

        beginTest("p10f_distortion_highgain_ducking_guard");
        {
            DistortionPedal distortion;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10FHighGainDistortion(distortion);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto reports = renderP10FChain(
                { { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                620, 0.96f, 36);
            expectP10FNoVolumeCollapse(reports.front(), "Distortion stage before HighGainAmp");
            expectP10FNoVolumeCollapse(reports.back(), "Distortion -> HighGainAmp -> Modern4x12");
            expect(reports.front().gateGainMin > 0.46 && reports.front().gateGainMaxDelta < 0.42,
                "P10F Distortion integrated gate should not act like active-input ducking: "
                    + p10fDuckingSummary(reports.front()));
        }

        beginTest("p10f_boost_highgain_ducking_guard");
        {
            BoostPedal boost;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(boost);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10FBoostIntoHighGain(boost);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto reports = renderP10FChain(
                { { "boost", &boost }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                620, 0.98f, 36);
            expectP10FNoVolumeCollapse(reports.back(), "Boost -> HighGainAmp -> Modern4x12");
            expect(reports.back().metrics.highFrequencyEnergyProxy() < 0.075,
                "P10F Boost/HighGain post-cab fizz proxy should stay controlled without OutputChain masking: "
                    + p10fDuckingSummary(reports.back()));
        }

        beginTest("p10f_highgainamp_internal_ducking_guard");
        {
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto reports = renderP10FChain(
                { { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                620, 1.18f, 36);
            expectP10FNoVolumeCollapse(reports.front(), "HighGainAmp internal response");
            expectP10FNoVolumeCollapse(reports.back(), "HighGainAmp -> Modern4x12 baseline");
        }

        beginTest("p10f_noise_gate_low_setting_reference");
        {
            NoiseGatePedal fuzzGate;
            FuzzPedal fuzz;
            ClassicAmp fuzzAmp;
            CabinetPedal fuzzCab;
            prepareP10CProcessor(fuzzGate);
            prepareP10CProcessor(fuzz);
            prepareP10CProcessor(fuzzAmp);
            prepareP10CProcessor(fuzzCab);
            configureP10FLowNoiseGate(fuzzGate);
            configureP10FFuzzReference(fuzz);
            configureP10FClassicAmp(fuzzAmp);
            configureP10FClassicCab(fuzzCab);

            const auto fuzzReports = renderP10FChain(
                { { "noise_gate", &fuzzGate }, { "fuzz", &fuzz }, { "classic_amp", &fuzzAmp }, { "cabinet", &fuzzCab } },
                560, 1.0f, 32);
            expectP10FReference(fuzzReports.back(), "Fuzz plus low Noise Gate reference");

            NoiseGatePedal distortionGate;
            DistortionPedal distortion;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(distortionGate);
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10FLowNoiseGate(distortionGate);
            configureP10FHighGainDistortion(distortion);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto distortionReports = renderP10FChain(
                { { "noise_gate", &distortionGate }, { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                560, 0.96f, 32);
            expectP10FNoVolumeCollapse(distortionReports.back(), "Distortion/HighGain plus low Noise Gate");
            expect(distortionReports.front().gateGainMin > 0.72,
                "P10F low external Noise Gate setting should not close hard while input is active: "
                    + p10fDuckingSummary(distortionReports.front()));
        }

        beginTest("p10f_highgain_noise_floor_after_silence");
        {
            BoostPedal boost;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(boost);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10FBoostIntoHighGain(boost);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto reports = renderP10ELongChain(
                { { "boost", &boost }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                700, P10DSignalKind::SilenceRecovery, 1.0f, 32, 430, 0.004, 0.00020);
            expect(reports.back().tailRms < juce::jmax(0.010, reports.back().maxActiveRms * 0.30),
                "P10F high-gain route should recover to a controlled noise floor after silence: "
                    + p10eLongSummary(reports.back()));
        }

        beginTest("p10f_highgain_no_perceptible_volume_collapse");
        {
            DistortionPedal distortion;
            BoostPedal boost;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepareP10CProcessor(distortion);
            prepareP10CProcessor(boost);
            prepareP10CProcessor(amp);
            prepareP10CProcessor(cab);
            configureP10FHighGainDistortion(distortion);
            configureP10FBoostIntoHighGain(boost);
            configureP10EHighGainAmp(amp);
            configureP10EModernCab(cab);

            const auto reports = renderP10FChain(
                { { "distortion", &distortion }, { "boost", &boost }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } },
                680, 0.72f, 40);
            expectP10FNoVolumeCollapse(reports.back(), "Distortion -> Boost -> HighGainAmp -> Modern4x12 stacked route");
            expect(reports.back().ceilingTouchedSamples == 0,
                "P10F stacked high-gain route should not depend on final ceiling hits: "
                    + p10fDuckingSummary(reports.back()));
        }
    }

private:
    static void configureP10FFuzzReference(FuzzPedal& fuzz)
    {
        fuzz.modeParam->setValueNotifyingHost(normalisedChoiceIndex(fuzz.modeParam, 1));
        fuzz.fuzzParam->setValueNotifyingHost(fuzz.fuzzParam->convertTo0to1(58.0f));
        fuzz.toneParam->setValueNotifyingHost(fuzz.toneParam->convertTo0to1(0.44f));
        fuzz.gateParam->setValueNotifyingHost(fuzz.gateParam->convertTo0to1(0.12f));
        fuzz.levelParam->setValueNotifyingHost(fuzz.levelParam->convertTo0to1(0.36f));
        fuzz.biasParam->setValueNotifyingHost(fuzz.biasParam->convertTo0to1(0.64f));
        fuzz.mixParam->setValueNotifyingHost(fuzz.mixParam->convertTo0to1(1.0f));
    }

    static void configureP10FHighGainDistortion(DistortionPedal& distortion)
    {
        distortion.modeParam->setValueNotifyingHost(normalisedChoiceIndex(distortion.modeParam, 4));
        distortion.gainParam->setValueNotifyingHost(distortion.gainParam->convertTo0to1(42.0f));
        distortion.toneParam->setValueNotifyingHost(distortion.toneParam->convertTo0to1(0.46f));
        distortion.bodyParam->setValueNotifyingHost(distortion.bodyParam->convertTo0to1(0.46f));
        distortion.tightParam->setValueNotifyingHost(distortion.tightParam->convertTo0to1(0.68f));
        distortion.levelParam->setValueNotifyingHost(distortion.levelParam->convertTo0to1(0.22f));
        distortion.mixParam->setValueNotifyingHost(distortion.mixParam->convertTo0to1(1.0f));
    }

    static void configureP10FBoostIntoHighGain(BoostPedal& boost)
    {
        setRangedParamById(boost, "boostGain", 8.5f);
        setRangedParamById(boost, "boostTight", 0.78f);
        setRangedParamById(boost, "boostTone", 0.46f);
        setRangedParamById(boost, "boostLevel", 0.70f);
        setRangedParamById(boost, "boostChar", 0.12f);
    }

    static void configureP10FLowNoiseGate(NoiseGatePedal& gate)
    {
        gate.thresholdParam->setValueNotifyingHost(gate.thresholdParam->convertTo0to1(-76.8f));
        gate.attackParam->setValueNotifyingHost(gate.attackParam->convertTo0to1(0.32f));
        gate.holdParam->setValueNotifyingHost(gate.holdParam->convertTo0to1(78.0f));
        gate.releaseParam->setValueNotifyingHost(gate.releaseParam->convertTo0to1(170.0f));
        gate.rangeParam->setValueNotifyingHost(gate.rangeParam->convertTo0to1(-28.0f));
        gate.hysteresisParam->setValueNotifyingHost(gate.hysteresisParam->convertTo0to1(9.0f));
        gate.focusParam->setValueNotifyingHost(gate.focusParam->convertTo0to1(0.54f));
    }

    static void configureP10FClassicAmp(ClassicAmp& amp)
    {
        setRangedParamById(amp, "ampDrive", 3.4f);
        setRangedParamById(amp, "ampTone", 0.46f);
        setRangedParamById(amp, "ampPresence", 0.42f);
        setRangedParamById(amp, "ampDepth", 0.42f);
        setRangedParamById(amp, "ampLevel", 0.58f);
    }

    static void configureP10FCleanAmp(CleanAmp& amp)
    {
        setRangedParamById(amp, "cleanDrive", 0.42f);
        setRangedParamById(amp, "cleanBass", 0.48f);
        setRangedParamById(amp, "cleanTreble", 0.50f);
        setRangedParamById(amp, "cleanReverb", 0.0f);
        setRangedParamById(amp, "cleanLevel", 0.64f);
    }

    static void configureP10FClassicCab(CabinetPedal& cab)
    {
        setRangedParamById(cab, "cabThump", 0.8f);
        setRangedParamById(cab, "cabAir", 0.7f);
        setRangedParamById(cab, "cabDistance", 0.36f);
        setRangedParamById(cab, "cabLevel", 0.72f);
        setRangedParamById(cab, "cabMix", 1.0f);
    }

    void expectP10FReference(const P10FDuckingMetrics& report, const juce::String& label)
    {
        expect(report.metrics.signal.finite && report.metrics.signal.invalidSamples == 0,
            "P10F reference " + label + " must remain finite: " + p10fDuckingSummary(report));
        expect(report.outputRmsWhileInputActive() > 0.0025,
            "P10F reference " + label + " should stay audibly present while input is active: "
                + p10fDuckingSummary(report));
        expect(report.gainDropDuringActiveInput < 0.72 && report.maxConsecutiveGainReductionBlocks <= 8,
            "P10F reference " + label + " defines acceptable high-gain movement without volume collapse: "
                + p10fDuckingSummary(report));
        expect(report.ceilingTouchedSamples == 0,
            "P10F reference " + label + " should not depend on near-clip or clipping samples: "
                + p10fDuckingSummary(report));
    }

    void expectP10FNoVolumeCollapse(const P10FDuckingMetrics& report, const juce::String& label)
    {
        expect(report.metrics.signal.finite && report.metrics.signal.invalidSamples == 0,
            "P10F " + label + " must remain finite: " + p10fDuckingSummary(report));
        expect(report.outputRmsWhileInputActive() > 0.0016,
            "P10F " + label + " should keep active input audible: " + p10fDuckingSummary(report));
        expect(report.gainDropDuringActiveInput < 0.76,
            "P10F " + label + " should not collapse volume while input remains active: "
                + p10fDuckingSummary(report));
        expect(report.maxConsecutiveGainReductionBlocks <= 10,
            "P10F " + label + " should not sustain consecutive active-input gain reduction: "
                + p10fDuckingSummary(report));
        expect(report.metrics.signal.clippedSamples == 0 && report.metrics.signal.nearClipSamples == 0,
            "P10F " + label + " should stay bounded without clipping/near-clip masking: "
                + p10fDuckingSummary(report));
        expect(report.metrics.modulationDepth3To20() < 0.62,
            "P10F " + label + " should not show low-rate envelope pumping: "
                + p10fDuckingSummary(report));
    }
};

struct P10GNoiseFloorMetrics
{
    juce::String name;
    P10DWindowMetrics signal;
    double idleNoiseRms = 0.0;
    double postPhraseNoiseRms = 0.0;
    double outputRmsWhileInputActive = 0.0;
    double outputRmsDuringSilence = 0.0;
    double sustainRmsAfterGate = 0.0;
    double activeInputToOutputSum = 0.0;
    double previousActiveInputRms = 0.0;
    double previousActiveRatio = 0.0;
    double volumeCollapseDuringActiveInput = 0.0;
    double gateReductionNeededForSilence = 0.0;
    int idleBlocks = 0;
    int activeBlocks = 0;
    int postPhraseBlocks = 0;

    void capture(const juce::AudioBuffer<float>& block,
                 int blockIndex,
                 int idleEndBlock,
                 int postPhraseStartBlock,
                 double inputRms,
                 double gateGain = -1.0)
    {
        signal.capture(block, gateGain);

        const double outputRms = p10eBlockRms(block);
        if (blockIndex < idleEndBlock)
        {
            idleNoiseRms += outputRms;
            ++idleBlocks;
            outputRmsDuringSilence += outputRms;
            return;
        }

        if (blockIndex >= postPhraseStartBlock)
        {
            postPhraseNoiseRms += outputRms;
            ++postPhraseBlocks;
            outputRmsDuringSilence += outputRms;
            return;
        }

        if (inputRms > 0.018)
        {
            ++activeBlocks;
            outputRmsWhileInputActive += outputRms;
            if (blockIndex < 390)
            {
                const double ratio = outputRms / juce::jmax(1.0e-9, inputRms);
                activeInputToOutputSum += ratio;
                if (previousActiveInputRms > 1.0e-8 && previousActiveRatio > 1.0e-8 && inputRms >= previousActiveInputRms * 0.82)
                {
                    const double ratioDrop = ratio / previousActiveRatio;
                    if (ratioDrop < 1.0)
                        volumeCollapseDuringActiveInput = juce::jmax(volumeCollapseDuringActiveInput, 1.0 - ratioDrop);
                }
                previousActiveInputRms = inputRms;
                previousActiveRatio = ratio;
            }
        }
        else if (blockIndex < postPhraseStartBlock)
        {
            sustainRmsAfterGate += outputRms;
        }
    }

    void finish()
    {
        if (idleBlocks > 0)
            idleNoiseRms /= (double) idleBlocks;
        if (postPhraseBlocks > 0)
            postPhraseNoiseRms /= (double) postPhraseBlocks;
        if (activeBlocks > 0)
        {
            outputRmsWhileInputActive /= (double) activeBlocks;
            activeInputToOutputSum /= (double) activeBlocks;
        }

        const int silenceBlocks = idleBlocks + postPhraseBlocks;
        outputRmsDuringSilence = silenceBlocks > 0 ? outputRmsDuringSilence / (double) silenceBlocks : 0.0;
        sustainRmsAfterGate = juce::jmax(sustainRmsAfterGate / (double) juce::jmax(1, postPhraseBlocks), postPhraseNoiseRms);

    }

    double noiseToSignalRatio() const noexcept
    {
        return outputRmsWhileInputActive > 1.0e-9
            ? postPhraseNoiseRms / outputRmsWhileInputActive
            : 0.0;
    }
};

juce::String p10gMetricsSummary(const P10GNoiseFloorMetrics& metrics)
{
    return "idleNoiseRms="
        + juce::String(metrics.idleNoiseRms, 7)
        + ", postPhraseNoiseRms="
        + juce::String(metrics.postPhraseNoiseRms, 7)
        + ", noiseToSignalRatio="
        + juce::String(metrics.noiseToSignalRatio(), 5)
        + ", outputRmsWhileInputActive="
        + juce::String(metrics.outputRmsWhileInputActive, 6)
        + ", outputRmsDuringSilence="
        + juce::String(metrics.outputRmsDuringSilence, 7)
        + ", sustainRmsAfterGate="
        + juce::String(metrics.sustainRmsAfterGate, 7)
        + ", gateTransitions="
        + juce::String(metrics.signal.gateTransitions)
        + ", gateDeltaPeak="
        + juce::String(metrics.signal.gateDeltaPeak, 4)
        + ", volumeCollapseDuringActiveInput="
        + juce::String(metrics.volumeCollapseDuringActiveInput, 4)
        + ", nearClip="
        + juce::String(metrics.signal.signal.nearClipSamples)
        + ", clipped="
        + juce::String(metrics.signal.signal.clippedSamples)
        + ", invalid="
        + juce::String(metrics.signal.signal.invalidSamples)
        + ", brightnessProxy="
        + juce::String(metrics.signal.signal.brightnessProxy(), 4)
        + ", highFrequencyEnergyProxy="
        + juce::String(metrics.signal.highFrequencyEnergyProxy(), 5)
        + ", rumbleProxy="
        + juce::String(metrics.signal.signal.rumbleProxy(), 4);
}

void fillP10GNoisePhraseBlock(juce::AudioBuffer<float>& block, int blockIndex)
{
    constexpr int idleEndBlock = 110;
    constexpr int postPhraseStartBlock = 560;
    const bool active = blockIndex >= idleEndBlock && blockIndex < postPhraseStartBlock;
    const bool sustainTail = blockIndex >= 440 && blockIndex < postPhraseStartBlock;

    for (int i = 0; i < block.getNumSamples(); ++i)
    {
        const int sampleIndex = blockIndex * block.getNumSamples() + i;
        const float t = (float) sampleIndex / (float) kSampleRate;
        const float deterministicHiss = 0.000090f
            * (std::sin(juce::MathConstants<float>::twoPi * 3713.0f * t + 0.17f)
                + 0.62f * std::sin(juce::MathConstants<float>::twoPi * 5879.0f * t + 1.3f)
                + 0.38f * std::sin(juce::MathConstants<float>::twoPi * 9137.0f * t + 2.1f));
        const float hum = 0.000040f * std::sin(juce::MathConstants<float>::twoPi * 60.0f * t);

        float phrase = 0.0f;
        if (active)
        {
            const int pickSample = sampleIndex % (int) (kSampleRate * 0.135);
            const float pick = std::exp(-(float) pickSample / 52.0f);
            const float tailScale = sustainTail ? juce::jmap((float) (blockIndex - 440), 0.0f, 120.0f, 0.62f, 0.26f) : 1.0f;
            phrase = tailScale
                * (0.070f * std::sin(juce::MathConstants<float>::twoPi * 82.41f * t)
                    + 0.036f * std::sin(juce::MathConstants<float>::twoPi * 123.47f * t)
                    + 0.025f * std::sin(juce::MathConstants<float>::twoPi * 164.82f * t))
                + 0.030f * pick;
        }

        const float sample = phrase + deterministicHiss + hum;
        block.setSample(0, i, sample);
        block.setSample(1, i, sample * 0.97f);
    }
}

std::vector<P10GNoiseFloorMetrics> renderP10GChain(
    const std::vector<std::pair<juce::String, juce::AudioProcessor*>>& stages,
    int blocksToRun = 760,
    int warmupBlocks = 20)
{
    constexpr int idleEndBlock = 110;
    constexpr int postPhraseStartBlock = 560;
    std::vector<P10GNoiseFloorMetrics> reports;
    reports.reserve(stages.size());
    for (const auto& stage : stages)
        reports.push_back({ stage.first });

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> block(2, kBlockSize);

    for (int blockIndex = 0; blockIndex < blocksToRun; ++blockIndex)
    {
        fillP10GNoisePhraseBlock(block, blockIndex);
        const double inputRms = p10eBlockRms(block);

        for (size_t stageIndex = 0; stageIndex < stages.size(); ++stageIndex)
        {
            auto* processor = stages[stageIndex].second;
            if (processor != nullptr)
                processor->processBlock(block, midi);

            if (blockIndex < warmupBlocks)
                continue;

            double gateGain = -1.0;
            if (auto* gate = dynamic_cast<NoiseGatePedal*>(processor))
                gateGain = (double) gate->currentGateGainAtomic.load();
            else if (auto* distortion = dynamic_cast<DistortionPedal*>(processor))
                gateGain = (double) distortion->currentGateGain;

            reports[stageIndex].capture(block, blockIndex, idleEndBlock, postPhraseStartBlock, inputRms, gateGain);
        }
    }

    for (auto& report : reports)
        report.finish();

    return reports;
}

class P10GHighGainNoiseFloorGateCollapseTests final : public juce::UnitTest
{
public:
    P10GHighGainNoiseFloorGateCollapseTests()
        : juce::UnitTest("P10G High-Gain Noise Floor / Gate / Collapse Diagnostics", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("p10g_fuzz_low_gate_reference");
        {
            NoiseGatePedal gate;
            FuzzPedal fuzz;
            ClassicAmp amp;
            CabinetPedal cab;
            prepare(gate); prepare(fuzz); prepare(amp); prepare(cab);
            configureLowGate(gate, 0.04f);
            configureFuzzReference(fuzz);
            configureClassicAmp(amp);
            configureClassicCab(cab);

            const auto reports = renderP10GChain({ { "noise_gate", &gate }, { "fuzz", &fuzz }, { "classic_amp", &amp }, { "cabinet", &cab } });
            expectReference(reports.back(), "Fuzz low-gate reference");
            expect(reports.front().signal.gateTransitions <= 24,
                "P10G Fuzz low-gate reference should not chatter: " + p10gMetricsSummary(reports.front()));
        }

        beginTest("p10g_distortion_low_gate_noise_floor");
        {
            NoiseGatePedal gate;
            DistortionPedal distortion;
            CleanAmp amp;
            CabinetPedal cab;
            prepare(gate); prepare(distortion); prepare(amp); prepare(cab);
            configureLowGate(gate, 0.04f);
            configureHighGainDistortion(distortion);
            configureCleanAmp(amp);
            configureClassicCab(cab);

            const auto reports = renderP10GChain({ { "noise_gate", &gate }, { "distortion", &distortion }, { "clean_amp", &amp }, { "cabinet", &cab } });
            expectNoiseFloorControlled(reports.back(), "Distortion low-gate noise floor", 0.24);
            expectNoActiveCollapse(reports[1], "Distortion low-gate active stage");
        }

        beginTest("p10g_highgain_low_gate_noise_floor");
        {
            NoiseGatePedal gate;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepare(gate); prepare(amp); prepare(cab);
            configureLowGate(gate, 0.04f);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10GChain({ { "noise_gate", &gate }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } });
            expectNoiseFloorControlled(reports.back(), "HighGainAmp low-gate noise floor", 0.20);
            expectNoActiveCollapse(reports.back(), "HighGainAmp low-gate active sustain");
        }

        beginTest("p10g_boost_highgain_ground_noise_guard");
        {
            BoostPedal boost;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepare(boost); prepare(amp); prepare(cab);
            configureBoost(boost);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10GChain({ { "boost", &boost }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } });
            expectNoiseFloorControlled(reports.back(), "Boost -> HighGainAmp ground-noise guard", 0.25);
            expect(reports.back().signal.highFrequencyEnergyProxy() < 0.078,
                "P10G Boost -> HighGainAmp should not leave cheap fizz in post-cab output: "
                    + p10gMetricsSummary(reports.back()));
        }

        beginTest("p10g_distortion_highgain_ground_noise_guard");
        {
            DistortionPedal distortion;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepare(distortion); prepare(amp); prepare(cab);
            configureHighGainDistortion(distortion);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10GChain({ { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } });
            expectNoiseFloorControlled(reports.back(), "Distortion -> HighGainAmp ground-noise guard", 0.28);
            expectNoActiveCollapse(reports.front(), "Distortion before HighGain active stage");
        }

        beginTest("p10g_distortion_active_volume_collapse_guard");
        {
            DistortionPedal distortion;
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepare(distortion); prepare(amp); prepare(cab);
            configureHighGainDistortion(distortion);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10GChain({ { "distortion", &distortion }, { "high_gain_amp", &amp }, { "modern_4x12", &cab } });
            expectNoActiveCollapse(reports.front(), "Distortion active volume-collapse guard");
            expectNoActiveCollapse(reports.back(), "Distortion -> HighGainAmp active volume-collapse guard");
        }

        beginTest("p10g_noise_gate_sustain_preservation_guard");
        {
            NoiseGatePedal lowGate;
            HighGainAmp gatedAmp;
            Modern4x12Cabinet gatedCab;
            HighGainAmp openAmp;
            Modern4x12Cabinet openCab;
            prepare(lowGate); prepare(gatedAmp); prepare(gatedCab); prepare(openAmp); prepare(openCab);
            configureLowGate(lowGate, 0.04f);
            configureHighGainAmp(gatedAmp); configureModernCab(gatedCab);
            configureHighGainAmp(openAmp); configureModernCab(openCab);

            const auto gated = renderP10GChain({ { "noise_gate", &lowGate }, { "high_gain_amp", &gatedAmp }, { "modern_4x12", &gatedCab } });
            const auto open = renderP10GChain({ { "high_gain_amp", &openAmp }, { "modern_4x12", &openCab } });
            expect(gated.back().outputRmsWhileInputActive > open.back().outputRmsWhileInputActive * 0.72,
                "P10G low gate should preserve active-note sustain; gated="
                    + p10gMetricsSummary(gated.back())
                    + ", open="
                    + p10gMetricsSummary(open.back()));
            expect(gated.back().idleNoiseRms < open.back().idleNoiseRms * 0.82,
                "P10G low gate should still reduce idle noise usefully; gated="
                    + p10gMetricsSummary(gated.back())
                    + ", open="
                    + p10gMetricsSummary(open.back()));
        }

        beginTest("p10g_highgain_fizz_proxy_guard");
        {
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepare(amp); prepare(cab);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10GChain({ { "high_gain_amp", &amp }, { "modern_4x12", &cab } });
            expect(reports.back().signal.highFrequencyEnergyProxy() < 0.070,
                "P10G high-gain fizz proxy should remain controlled without dulling by OutputChain masking: "
                    + p10gMetricsSummary(reports.back()));
            expect(reports.back().signal.signal.brightnessProxy() < 0.92,
                "P10G high-gain post-cab brightness proxy should stay musical: "
                    + p10gMetricsSummary(reports.back()));
        }

        beginTest("p10g_highgain_baseline_preservation");
        {
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepare(amp); prepare(cab);
            configureHighGainAmp(amp);
            configureModernCab(cab);

            const auto reports = renderP10GChain({ { "high_gain_amp", &amp }, { "modern_4x12", &cab } });
            expect(reports.back().outputRmsWhileInputActive > 0.010,
                "P10G HighGainAmp -> Modern4x12 baseline should remain clearly audible: "
                    + p10gMetricsSummary(reports.back()));
            expect(reports.back().signal.signal.nearClipSamples == 0 && reports.back().signal.signal.clippedSamples == 0,
                "P10G HighGainAmp -> Modern4x12 baseline should keep headroom: "
                    + p10gMetricsSummary(reports.back()));
            expect(reports.back().noiseToSignalRatio() < 0.22,
                "P10G HighGainAmp -> Modern4x12 baseline should not trade tone for a high idle floor: "
                    + p10gMetricsSummary(reports.back()));
        }
    }

private:
    static void prepare(juce::AudioProcessor& processor)
    {
        processor.setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
        processor.prepareToPlay(kSampleRate, kBlockSize);
    }

    static void configureLowGate(NoiseGatePedal& gate, float amount)
    {
        const float bounded = juce::jlimit(0.0f, 1.0f, amount);
        gate.thresholdParam->setValueNotifyingHost(gate.thresholdParam->convertTo0to1(-80.0f + bounded * 80.0f));
        gate.attackParam->setValueNotifyingHost(gate.attackParam->convertTo0to1(0.24f));
        gate.holdParam->setValueNotifyingHost(gate.holdParam->convertTo0to1(82.0f));
        gate.releaseParam->setValueNotifyingHost(gate.releaseParam->convertTo0to1(185.0f));
        gate.rangeParam->setValueNotifyingHost(gate.rangeParam->convertTo0to1(-34.0f));
        gate.hysteresisParam->setValueNotifyingHost(gate.hysteresisParam->convertTo0to1(9.5f));
        gate.focusParam->setValueNotifyingHost(gate.focusParam->convertTo0to1(0.58f));
    }

    static void configureFuzzReference(FuzzPedal& fuzz)
    {
        fuzz.modeParam->setValueNotifyingHost(normalisedChoiceIndex(fuzz.modeParam, 1));
        fuzz.fuzzParam->setValueNotifyingHost(fuzz.fuzzParam->convertTo0to1(58.0f));
        fuzz.toneParam->setValueNotifyingHost(fuzz.toneParam->convertTo0to1(0.44f));
        fuzz.gateParam->setValueNotifyingHost(fuzz.gateParam->convertTo0to1(0.12f));
        fuzz.levelParam->setValueNotifyingHost(fuzz.levelParam->convertTo0to1(0.36f));
        fuzz.biasParam->setValueNotifyingHost(fuzz.biasParam->convertTo0to1(0.64f));
        fuzz.mixParam->setValueNotifyingHost(fuzz.mixParam->convertTo0to1(1.0f));
    }

    static void configureHighGainDistortion(DistortionPedal& distortion)
    {
        distortion.modeParam->setValueNotifyingHost(normalisedChoiceIndex(distortion.modeParam, 4));
        distortion.gainParam->setValueNotifyingHost(distortion.gainParam->convertTo0to1(42.0f));
        distortion.toneParam->setValueNotifyingHost(distortion.toneParam->convertTo0to1(0.44f));
        distortion.bodyParam->setValueNotifyingHost(distortion.bodyParam->convertTo0to1(0.46f));
        distortion.tightParam->setValueNotifyingHost(distortion.tightParam->convertTo0to1(0.68f));
        distortion.levelParam->setValueNotifyingHost(distortion.levelParam->convertTo0to1(0.22f));
        distortion.mixParam->setValueNotifyingHost(distortion.mixParam->convertTo0to1(1.0f));
    }

    static void configureBoost(BoostPedal& boost)
    {
        setRangedParamById(boost, "boostGain", 8.5f);
        setRangedParamById(boost, "boostTight", 0.78f);
        setRangedParamById(boost, "boostTone", 0.46f);
        setRangedParamById(boost, "boostLevel", 0.70f);
        setRangedParamById(boost, "boostChar", 0.12f);
    }

    static void configureHighGainAmp(HighGainAmp& amp)
    {
        setRangedParamById(amp, "hgDrive", 7.2f);
        setRangedParamById(amp, "hgTight", 0.82f);
        setRangedParamById(amp, "hgPresence", 0.54f);
        setRangedParamById(amp, "hgTone", 0.52f);
        setRangedParamById(amp, "hgLevel", 0.58f);
    }

    static void configureClassicAmp(ClassicAmp& amp)
    {
        setRangedParamById(amp, "ampDrive", 3.4f);
        setRangedParamById(amp, "ampTone", 0.46f);
        setRangedParamById(amp, "ampPresence", 0.42f);
        setRangedParamById(amp, "ampDepth", 0.42f);
        setRangedParamById(amp, "ampLevel", 0.58f);
    }

    static void configureCleanAmp(CleanAmp& amp)
    {
        setRangedParamById(amp, "cleanDrive", 0.42f);
        setRangedParamById(amp, "cleanBass", 0.48f);
        setRangedParamById(amp, "cleanTreble", 0.50f);
        setRangedParamById(amp, "cleanReverb", 0.0f);
        setRangedParamById(amp, "cleanLevel", 0.64f);
    }

    static void configureClassicCab(CabinetPedal& cab)
    {
        setRangedParamById(cab, "cabThump", 0.8f);
        setRangedParamById(cab, "cabAir", 0.7f);
        setRangedParamById(cab, "cabDistance", 0.36f);
        setRangedParamById(cab, "cabLevel", 0.72f);
        setRangedParamById(cab, "cabMix", 1.0f);
    }

    static void configureModernCab(Modern4x12Cabinet& cab)
    {
        setRangedParamById(cab, "m4x12Low", 1.0f);
        setRangedParamById(cab, "m4x12Presence", 1.8f);
        setRangedParamById(cab, "m4x12Distance", 0.34f);
        setRangedParamById(cab, "m4x12Level", 0.76f);
        setRangedParamById(cab, "m4x12Mix", 1.0f);
    }

    void expectReference(const P10GNoiseFloorMetrics& report, const juce::String& label)
    {
        expect(report.signal.signal.finite && report.signal.signal.invalidSamples == 0,
            "P10G " + label + " must remain finite: " + p10gMetricsSummary(report));
        expect(report.outputRmsWhileInputActive > 0.0025,
            "P10G " + label + " should remain audibly present while active: " + p10gMetricsSummary(report));
        expect(report.noiseToSignalRatio() < 0.34,
            "P10G " + label + " defines acceptable low-gate noise behavior: " + p10gMetricsSummary(report));
        expect(report.volumeCollapseDuringActiveInput < 0.82,
            "P10G " + label + " should not collapse active notes: " + p10gMetricsSummary(report));
    }

    void expectNoiseFloorControlled(const P10GNoiseFloorMetrics& report, const juce::String& label, double maxNoiseToSignal)
    {
        expect(report.signal.signal.finite && report.signal.signal.invalidSamples == 0,
            "P10G " + label + " must remain finite: " + p10gMetricsSummary(report));
        expect(report.outputRmsWhileInputActive > 0.0018,
            "P10G " + label + " should remain audible while played: " + p10gMetricsSummary(report));
        expect(report.noiseToSignalRatio() < maxNoiseToSignal,
            "P10G " + label + " should keep idle/post-phrase noise below the active signal: "
                + p10gMetricsSummary(report));
        expect(report.postPhraseNoiseRms < 0.012,
            "P10G " + label + " should not leave a high ground-like post-phrase floor: "
                + p10gMetricsSummary(report));
        expect(report.signal.signal.nearClipSamples == 0 && report.signal.signal.clippedSamples == 0,
            "P10G " + label + " should not use clipping or near-clip masking: " + p10gMetricsSummary(report));
    }

    void expectNoActiveCollapse(const P10GNoiseFloorMetrics& report, const juce::String& label)
    {
        expect(report.volumeCollapseDuringActiveInput < 0.93,
            "P10G " + label + " should not show active-input volume collapse: " + p10gMetricsSummary(report));
        expect(report.outputRmsWhileInputActive > 0.0016,
            "P10G " + label + " should stay audible while input remains active: " + p10gMetricsSummary(report));
        expect(report.signal.gateDeltaPeak < 0.52,
            "P10G " + label + " gate movement should not feel like hard ducking: " + p10gMetricsSummary(report));
    }
};

struct P11AAmpRenderStats
{
    double rms = 0.0;
    double peak = 0.0;
    int measuredBlocks = 0;
    P10DWindowMetrics signal;

    void capture(const juce::AudioBuffer<float>& block)
    {
        rms += p10eBlockRms(block);
        peak = juce::jmax(peak, computeBufferPeak(block, 0, block.getNumSamples()));
        signal.capture(block);
        ++measuredBlocks;
    }

    void finish()
    {
        if (measuredBlocks > 0)
            rms /= (double) measuredBlocks;
    }
};

juce::String p11aAmpStatsSummary(const P11AAmpRenderStats& stats)
{
    return "rms=" + juce::String(stats.rms, 6)
        + ", peak=" + juce::String(stats.peak, 5)
        + ", invalid=" + juce::String(stats.signal.signal.invalidSamples)
        + ", nearClip=" + juce::String(stats.signal.signal.nearClipSamples)
        + ", clipped=" + juce::String(stats.signal.signal.clippedSamples)
        + ", brightnessProxy=" + juce::String(stats.signal.signal.brightnessProxy(), 4)
        + ", rumbleProxy=" + juce::String(stats.signal.signal.rumbleProxy(), 4);
}

class P11AAmpProfessionalizationTests final : public juce::UnitTest
{
public:
    P11AAmpProfessionalizationTests()
        : juce::UnitTest("P11A Amp Interface And Circuit Professionalization", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("p11a_amp_catalog_surface_guard");
        {
            const std::array<juce::String, 5> ampTypes {
                "Clean Amp", "Classic Amp", "High Gain Amp", "Chime Amp", "Boutique Amp"
            };

            for (const auto& type : ampTypes)
            {
                expect(Nova::PedalCatalog::kindFromType(type) == Nova::PedalCatalog::Kind::Amplifier,
                    type + " must remain cataloged as an amplifier");

                auto amp = PedalRegistry::createPedal(type);
                expect(amp != nullptr, type + " must remain constructible");
                expect(amp != nullptr && amp->hasEditor(), type + " must expose its amp editor");
                expect(amp != nullptr && amp->getParameters().size() >= 6,
                    type + " should expose a professional amp control surface");
            }

            expectHasParam("Clean Amp", "cleanHeadroom");
            expectHasParam("Classic Amp", "ampSag");
            expectHasParam("Classic Amp", "ampBright");
            expectHasParam("High Gain Amp", "hgResonance");
            expectHasParam("High Gain Amp", "hgFeel");
            expectHasParam("Chime Amp", "chimeSag");
            expectHasParam("Boutique Amp", "boutTouch");
        }

        beginTest("p11a_amp_state_roundtrip_new_controls");
        {
            expectRoundTrip("Clean Amp", "cleanHeadroom", 0.72f);
            expectRoundTrip("Classic Amp", "ampSag", 0.68f);
            expectRoundTrip("Classic Amp", "ampBright", 0.36f);
            expectRoundTrip("High Gain Amp", "hgResonance", 0.58f);
            expectRoundTrip("High Gain Amp", "hgFeel", 0.42f);
            expectRoundTrip("Chime Amp", "chimeSag", 0.64f);
            expectRoundTrip("Boutique Amp", "boutTouch", 0.76f);
        }

        beginTest("p11a_amp_tonal_stability_guard");
        {
            const std::array<juce::String, 5> ampTypes {
                "Clean Amp", "Classic Amp", "High Gain Amp", "Chime Amp", "Boutique Amp"
            };

            for (const auto& type : ampTypes)
            {
                auto amp = PedalRegistry::createPedal(type);
                expect(amp != nullptr, type + " must be constructible for rendering");
                if (amp == nullptr)
                    continue;

                configureUsableAmpVoice(*amp, type);
                auto stats = renderAmp(*amp, type);
                expect(stats.signal.signal.finite && stats.signal.signal.invalidSamples == 0,
                    "P11A " + type + " must remain finite: " + p11aAmpStatsSummary(stats));
                expect(stats.rms > 0.0010,
                    "P11A " + type + " should remain clearly audible: " + p11aAmpStatsSummary(stats));
                expect(stats.peak < 1.85,
                    "P11A " + type + " should keep local amp headroom without OutputChain masking: " + p11aAmpStatsSummary(stats));
                expect(stats.signal.signal.clippedSamples == 0 && stats.signal.signal.nearClipSamples == 0,
                    "P11A " + type + " should avoid clipping/near-clip samples: " + p11aAmpStatsSummary(stats));
            }
        }

        beginTest("p11a_highgain_baseline_preservation");
        {
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepare(amp); prepare(cab);
            setRangedParamById(amp, "hgDrive", 7.2f);
            setRangedParamById(amp, "hgTight", 0.82f);
            setRangedParamById(amp, "hgPresence", 0.54f);
            setRangedParamById(amp, "hgTone", 0.52f);
            setRangedParamById(amp, "hgResonance", 0.46f);
            setRangedParamById(amp, "hgFeel", 0.55f);
            setRangedParamById(amp, "hgLevel", 0.58f);
            setRangedParamById(cab, "m4x12Low", 1.0f);
            setRangedParamById(cab, "m4x12Presence", 1.8f);
            setRangedParamById(cab, "m4x12Distance", 0.34f);
            setRangedParamById(cab, "m4x12Level", 0.76f);
            setRangedParamById(cab, "m4x12Mix", 1.0f);

            const auto reports = renderP10GChain({ { "high_gain_amp", &amp }, { "modern_4x12", &cab } });
            expect(reports.back().outputRmsWhileInputActive > 0.010,
                "P11A HighGainAmp -> Modern4x12 baseline must stay audible: " + p10gMetricsSummary(reports.back()));
            expect(reports.back().noiseToSignalRatio() < 0.23,
                "P11A HighGainAmp -> Modern4x12 baseline must not regain high idle floor: " + p10gMetricsSummary(reports.back()));
            expect(reports.back().signal.highFrequencyEnergyProxy() < 0.073,
                "P11A HighGainAmp -> Modern4x12 fizz proxy should stay controlled: " + p10gMetricsSummary(reports.back()));
        }

        beginTest("p11a_clean_path_preservation");
        {
            CleanAmp amp;
            prepare(amp);
            setRangedParamById(amp, "cleanDrive", 0.32f);
            setRangedParamById(amp, "cleanReverb", 0.0f);
            setRangedParamById(amp, "cleanHeadroom", 0.62f);
            setRangedParamById(amp, "cleanLevel", 0.70f);

            auto stats = renderAmp(amp, "Clean Amp");
            expect(stats.signal.signal.finite && stats.signal.signal.invalidSamples == 0,
                "P11A Clean Amp path must remain finite: " + p11aAmpStatsSummary(stats));
            expect(stats.rms > 0.0012,
                "P11A Clean Amp path must remain audible: " + p11aAmpStatsSummary(stats));
            expect(stats.signal.signal.clippedSamples == 0 && stats.signal.signal.nearClipSamples == 0,
                "P11A Clean Amp path must keep headroom: " + p11aAmpStatsSummary(stats));
        }

        beginTest("p13c_amp_oversized_blocks_stay_safe");
        {
            const std::array<juce::String, 5> ampTypes {
                "Clean Amp", "Classic Amp", "High Gain Amp", "Chime Amp", "Boutique Amp"
            };

            for (const auto& type : ampTypes)
            {
                auto amp = PedalRegistry::createPedal(type);
                expect(amp != nullptr, "P13C oversized amp should construct " + type);
                if (amp == nullptr)
                    continue;

                configureUsableAmpVoice(*amp, type);
                amp->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                amp->prepareToPlay(kSampleRate, kBlockSize);

                juce::AudioBuffer<float> oversized(2, kBlockSize * 4);
                fillAmpSafetyBlock(oversized, 0, 0.055f);
                juce::AudioBuffer<float> dryCopy;
                dryCopy.makeCopyOf(oversized);

                juce::MidiBuffer midi;
                amp->processBlock(oversized, midi);

                expect(bufferHasOnlyFiniteSamples(oversized), "P13C " + type + " oversized block must stay finite");
                if (type != "Clean Amp")
                {
                    expect(computeBufferNullRms(dryCopy, oversized) <= 1.0e-7,
                        "P13C " + type + " oversized fallback should leave finite dry input materially unchanged");
                }
            }
        }

        beginTest("p13c_amp_nan_inf_recovery");
        {
            const std::array<juce::String, 4> ampTypes {
                "Classic Amp", "High Gain Amp", "Chime Amp", "Boutique Amp"
            };

            for (const auto& type : ampTypes)
            {
                auto amp = PedalRegistry::createPedal(type);
                expect(amp != nullptr, "P13C NaN/Inf amp should construct " + type);
                if (amp == nullptr)
                    continue;

                configureUsableAmpVoice(*amp, type);
                amp->setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
                amp->prepareToPlay(kSampleRate, kBlockSize);

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> block(2, kBlockSize);
                bool finiteDuringAbuse = true;

                for (int blockIndex = 0; blockIndex < 24; ++blockIndex)
                {
                    fillAmpSafetyBlock(block, blockIndex, 0.080f);
                    injectNanInfSamples(block, blockIndex);
                    amp->processBlock(block, midi);
                    finiteDuringAbuse = finiteDuringAbuse && bufferHasOnlyFiniteSamples(block);
                }

                P11AAmpRenderStats recoveryStats;
                bool finiteDuringRecovery = true;
                for (int blockIndex = 24; blockIndex < 112; ++blockIndex)
                {
                    fillAmpSafetyBlock(block, blockIndex, 0.085f);
                    amp->processBlock(block, midi);
                    finiteDuringRecovery = finiteDuringRecovery && bufferHasOnlyFiniteSamples(block);
                    if (blockIndex >= 36)
                        recoveryStats.capture(block);
                }
                recoveryStats.finish();

                expect(finiteDuringAbuse, "P13C " + type + " must scrub NaN/Inf input to finite output");
                expect(finiteDuringRecovery, "P13C " + type + " must recover finite output after NaN/Inf input");
                expect(recoveryStats.rms > 0.00045,
                    "P13C " + type + " must not latch into silence after invalid input: " + p11aAmpStatsSummary(recoveryStats));

                if (type == "High Gain Amp")
                {
                    expect(recoveryStats.rms > 0.0010,
                        "P13C HighGainAmp gate/envelope must not latch into persistent silence: " + p11aAmpStatsSummary(recoveryStats));
                }
            }
        }
    }

private:
    static void prepare(juce::AudioProcessor& processor)
    {
        processor.setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
        processor.prepareToPlay(kSampleRate, kBlockSize);
    }

    static juce::RangedAudioParameter* findParam(juce::AudioProcessor& processor, const juce::String& paramId)
    {
        for (auto* param : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                if (ranged->getParameterID() == paramId)
                    return ranged;

        return nullptr;
    }

    void expectHasParam(const juce::String& type, const juce::String& paramId)
    {
        auto amp = PedalRegistry::createPedal(type);
        expect(amp != nullptr && findParam(*amp, paramId) != nullptr,
            "P11A " + type + " must expose parameter " + paramId);
    }

    void expectRoundTrip(const juce::String& type, const juce::String& paramId, float plainValue)
    {
        auto source = PedalRegistry::createPedal(type);
        auto restored = PedalRegistry::createPedal(type);
        expect(source != nullptr && restored != nullptr, "P11A roundtrip processors must be constructible for " + type);
        if (source == nullptr || restored == nullptr)
            return;

        expect(setRangedParamById(*source, paramId, plainValue),
            "P11A source should accept parameter " + paramId);

        juce::MemoryBlock state;
        source->getStateInformation(state);
        restored->setStateInformation(state.getData(), (int) state.getSize());

        auto* restoredParam = findParam(*restored, paramId);
        expect(restoredParam != nullptr, "P11A restored amp should contain parameter " + paramId);
        if (restoredParam == nullptr)
            return;

        const float restoredPlain = restoredParam->convertFrom0to1(restoredParam->getValue());
        expect(std::abs(restoredPlain - plainValue) <= 0.015f,
            "P11A " + type + " should round-trip " + paramId
                + " expected=" + juce::String(plainValue, 3)
                + " actual=" + juce::String(restoredPlain, 3));
    }

    static void configureUsableAmpVoice(juce::AudioProcessor& amp, const juce::String& type)
    {
        if (type == "Clean Amp")
        {
            setRangedParamById(amp, "cleanDrive", 0.38f);
            setRangedParamById(amp, "cleanBass", 0.50f);
            setRangedParamById(amp, "cleanTreble", 0.52f);
            setRangedParamById(amp, "cleanReverb", 0.0f);
            setRangedParamById(amp, "cleanHeadroom", 0.62f);
            setRangedParamById(amp, "cleanLevel", 0.70f);
        }
        else if (type == "Classic Amp")
        {
            setRangedParamById(amp, "ampDrive", 3.2f);
            setRangedParamById(amp, "ampTone", 0.50f);
            setRangedParamById(amp, "ampPresence", 0.46f);
            setRangedParamById(amp, "ampDepth", 0.48f);
            setRangedParamById(amp, "ampSag", 0.50f);
            setRangedParamById(amp, "ampBright", 0.46f);
            setRangedParamById(amp, "ampLevel", 0.62f);
        }
        else if (type == "High Gain Amp")
        {
            setRangedParamById(amp, "hgDrive", 6.8f);
            setRangedParamById(amp, "hgTone", 0.50f);
            setRangedParamById(amp, "hgPresence", 0.52f);
            setRangedParamById(amp, "hgTight", 0.80f);
            setRangedParamById(amp, "hgResonance", 0.44f);
            setRangedParamById(amp, "hgFeel", 0.52f);
            setRangedParamById(amp, "hgLevel", 0.58f);
        }
        else if (type == "Chime Amp")
        {
            setRangedParamById(amp, "chimeDrive", 1.8f);
            setRangedParamById(amp, "chimeTrebleCut", 0.58f);
            setRangedParamById(amp, "chimeBassCut", 0.50f);
            setRangedParamById(amp, "chimeBrill", 0.52f);
            setRangedParamById(amp, "chimeSag", 0.54f);
            setRangedParamById(amp, "chimeLevel", 0.66f);
        }
        else if (type == "Boutique Amp")
        {
            setRangedParamById(amp, "boutDrive", 1.45f);
            setRangedParamById(amp, "boutWarmth", 0.52f);
            setRangedParamById(amp, "boutMid", 0.56f);
            setRangedParamById(amp, "boutPres", 0.48f);
            setRangedParamById(amp, "boutTouch", 0.60f);
            setRangedParamById(amp, "boutLevel", 0.68f);
        }
    }

    static void fillAmpExerciseBlock(juce::AudioBuffer<float>& block, int blockIndex)
    {
        for (int i = 0; i < block.getNumSamples(); ++i)
        {
            const int sampleIndex = blockIndex * block.getNumSamples() + i;
            const float t = (float) sampleIndex / (float) kSampleRate;
            const float phraseEnv = blockIndex < 190 ? 1.0f : std::exp(-(float)(blockIndex - 190) / 48.0f);
            const float pick = std::exp(-(float)(sampleIndex % 1800) / 70.0f) * 0.020f;
            const float sample = phraseEnv
                * (0.058f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                    + 0.032f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t)
                    + 0.018f * std::sin(juce::MathConstants<float>::twoPi * 329.63f * t)
                    + pick);

            block.setSample(0, i, sample);
            block.setSample(1, i, sample * 0.985f);
        }
    }

    static void fillAmpSafetyBlock(juce::AudioBuffer<float>& block, int blockIndex, float level)
    {
        for (int i = 0; i < block.getNumSamples(); ++i)
        {
            const int sampleIndex = blockIndex * block.getNumSamples() + i;
            const float t = (float) sampleIndex / (float) kSampleRate;
            const float sample = level
                * (0.70f * std::sin(juce::MathConstants<float>::twoPi * 110.0f * t)
                    + 0.22f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * t)
                    + 0.08f * std::sin(juce::MathConstants<float>::twoPi * 329.63f * t));

            block.setSample(0, i, sample);
            block.setSample(1, i, sample * 0.97f);
        }
    }

    static void injectNanInfSamples(juce::AudioBuffer<float>& block, int blockIndex)
    {
        for (int ch = 0; ch < block.getNumChannels(); ++ch)
        {
            for (int i = 0; i < block.getNumSamples(); i += 17)
            {
                const int selector = (i + blockIndex + ch) % 3;
                const float value = selector == 0 ? std::numeric_limits<float>::quiet_NaN()
                    : selector == 1 ? std::numeric_limits<float>::infinity()
                    : -std::numeric_limits<float>::infinity();
                block.setSample(ch, i, value);
            }
        }
    }

    static P11AAmpRenderStats renderAmp(juce::AudioProcessor& amp, const juce::String& type)
    {
        juce::ignoreUnused(type);
        prepare(amp);

        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, kBlockSize);
        P11AAmpRenderStats stats;

        for (int blockIndex = 0; blockIndex < 260; ++blockIndex)
        {
            fillAmpExerciseBlock(block, blockIndex);
            amp.processBlock(block, midi);
            if (blockIndex >= 24)
                stats.capture(block);
        }

        stats.finish();
        return stats;
    }
};

class P11BCabinetProfessionalizationTests final : public juce::UnitTest
{
public:
    P11BCabinetProfessionalizationTests()
        : juce::UnitTest("P11B Cabinet Interface And Voicing Professionalization", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("p11b_cabinet_catalog_surface_guard");
        {
            const std::array<juce::String, 3> cabinetTypes { "Cabinet", "Vintage 2x12", "Modern 4x12" };
            for (const auto& type : cabinetTypes)
            {
                expect(Nova::PedalCatalog::kindFromType(type) == Nova::PedalCatalog::Kind::Cabinet,
                    type + " must remain cataloged as a cabinet");

                auto cabinet = PedalRegistry::createPedal(type);
                expect(cabinet != nullptr, type + " must remain constructible");
                expect(cabinet != nullptr && cabinet->hasEditor(), type + " must expose its cabinet editor");
                expect(cabinet != nullptr && cabinet->getParameters().size() >= 8,
                    type + " should expose a professional cabinet control surface");
            }

            expectHasParam("Cabinet", "cabResonance");
            expectHasParam("Cabinet", "cabLowCut");
            expectHasParam("Cabinet", "cabHighCut");
            expectHasParam("Vintage 2x12", "v2x12Resonance");
            expectHasParam("Vintage 2x12", "v2x12LowCut");
            expectHasParam("Vintage 2x12", "v2x12HighCut");
            expectHasParam("Modern 4x12", "m4x12Resonance");
            expectHasParam("Modern 4x12", "m4x12LowCut");
            expectHasParam("Modern 4x12", "m4x12HighCut");
        }

        beginTest("p11b_cabinet_state_roundtrip_new_controls");
        {
            expectRoundTrip("Cabinet", "cabResonance", 1.8f);
            expectRoundTrip("Cabinet", "cabLowCut", 92.0f);
            expectRoundTrip("Cabinet", "cabHighCut", 6800.0f);
            expectRoundTrip("Vintage 2x12", "v2x12Resonance", 1.4f);
            expectRoundTrip("Vintage 2x12", "v2x12LowCut", 105.0f);
            expectRoundTrip("Vintage 2x12", "v2x12HighCut", 5200.0f);
            expectRoundTrip("Modern 4x12", "m4x12Resonance", 1.2f);
            expectRoundTrip("Modern 4x12", "m4x12LowCut", 88.0f);
            expectRoundTrip("Modern 4x12", "m4x12HighCut", 5600.0f);
        }

        beginTest("p11b_cabinet_voicing_stability_guard");
        {
            const std::array<juce::String, 3> cabinetTypes { "Cabinet", "Vintage 2x12", "Modern 4x12" };
            for (const auto& type : cabinetTypes)
            {
                auto cabinet = PedalRegistry::createPedal(type);
                expect(cabinet != nullptr, type + " must be constructible for rendering");
                if (cabinet == nullptr)
                    continue;

                configureUsableCabinetVoice(*cabinet, type);
                auto stats = renderCabinet(*cabinet);
                expect(stats.signal.signal.finite && stats.signal.signal.invalidSamples == 0,
                    "P11B " + type + " must remain finite: " + p11aAmpStatsSummary(stats));
                expect(stats.rms > 0.0008,
                    "P11B " + type + " should remain audible: " + p11aAmpStatsSummary(stats));
                expect(stats.peak < 1.70,
                    "P11B " + type + " should not create unnecessary overs: " + p11aAmpStatsSummary(stats));
                expect(stats.signal.signal.clippedSamples == 0 && stats.signal.signal.nearClipSamples == 0,
                    "P11B " + type + " should avoid clipping/near-clip samples: " + p11aAmpStatsSummary(stats));
                expect(stats.signal.signal.rumbleProxy() < 0.72,
                    "P11B " + type + " should keep rumble bounded: " + p11aAmpStatsSummary(stats));
                expect(stats.signal.highFrequencyEnergyProxy() < 0.18,
                    "P11B " + type + " should keep high-frequency cabinet energy controlled: " + p11aAmpStatsSummary(stats));
            }
        }

        beginTest("p11b_modern4x12_highgain_baseline_preservation");
        {
            HighGainAmp amp;
            Modern4x12Cabinet cab;
            prepare(amp); prepare(cab);
            setRangedParamById(amp, "hgDrive", 7.2f);
            setRangedParamById(amp, "hgTight", 0.82f);
            setRangedParamById(amp, "hgPresence", 0.54f);
            setRangedParamById(amp, "hgTone", 0.52f);
            setRangedParamById(amp, "hgResonance", 0.46f);
            setRangedParamById(amp, "hgFeel", 0.55f);
            setRangedParamById(amp, "hgLevel", 0.58f);
            configureUsableCabinetVoice(cab, "Modern 4x12");

            const auto reports = renderP10GChain({ { "high_gain_amp", &amp }, { "modern_4x12", &cab } });
            expect(reports.back().outputRmsWhileInputActive > 0.010,
                "P11B HighGainAmp -> Modern4x12 baseline must stay audible: " + p10gMetricsSummary(reports.back()));
            expect(reports.back().noiseToSignalRatio() < 0.23,
                "P11B HighGainAmp -> Modern4x12 baseline must not regain high idle floor: " + p10gMetricsSummary(reports.back()));
            expect(reports.back().signal.highFrequencyEnergyProxy() < 0.073,
                "P11B Modern4x12 high-gain fizz proxy should remain controlled: " + p10gMetricsSummary(reports.back()));
        }

        beginTest("p11b_clean_cabinet_path_preservation");
        {
            CleanAmp amp;
            CabinetPedal cab;
            prepare(amp); prepare(cab);
            setRangedParamById(amp, "cleanDrive", 0.32f);
            setRangedParamById(amp, "cleanReverb", 0.0f);
            setRangedParamById(amp, "cleanHeadroom", 0.62f);
            setRangedParamById(amp, "cleanLevel", 0.70f);
            configureUsableCabinetVoice(cab, "Cabinet");

            const auto reports = renderP10GChain({ { "clean_amp", &amp }, { "cabinet", &cab } }, 420, 20);
            expect(reports.back().signal.signal.finite && reports.back().signal.signal.invalidSamples == 0,
                "P11B CleanAmp -> Cabinet path must remain finite: " + p10gMetricsSummary(reports.back()));
            expect(reports.back().outputRmsWhileInputActive > 0.0020,
                "P11B CleanAmp -> Cabinet path must remain audible: " + p10gMetricsSummary(reports.back()));
            expect(reports.back().signal.signal.clippedSamples == 0 && reports.back().signal.signal.nearClipSamples == 0,
                "P11B CleanAmp -> Cabinet path must keep headroom: " + p10gMetricsSummary(reports.back()));
        }

        beginTest("p13c_cabinets_near_silence_tail_remains_finite");
        {
            const std::array<juce::String, 3> cabinetTypes { "Cabinet", "Vintage 2x12", "Modern 4x12" };
            for (const auto& type : cabinetTypes)
            {
                auto cabinet = PedalRegistry::createPedal(type);
                expect(cabinet != nullptr, "P13C near-silence cabinet should construct " + type);
                if (cabinet == nullptr)
                    continue;

                configureUsableCabinetVoice(*cabinet, type);
                prepare(*cabinet);

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> block(2, kBlockSize);
                bool finite = true;
                double peak = 0.0;

                for (int blockIndex = 0; blockIndex < 180; ++blockIndex)
                {
                    fillCabinetNearSilenceBlock(block, blockIndex);
                    cabinet->processBlock(block, midi);
                    finite = finite && bufferHasOnlyFiniteSamples(block);
                    peak = juce::jmax(peak, computeBufferPeak(block, 0, block.getNumSamples()));
                }

                expect(finite, "P13C " + type + " near-silence/tail render must stay finite");
                expect(peak < 1.0e-10,
                    "P13C " + type + " near-silence/tail render should remain effectively silent");
            }
        }
    }

private:
    static void prepare(juce::AudioProcessor& processor)
    {
        processor.setPlayConfigDetails(2, 2, kSampleRate, kBlockSize);
        processor.prepareToPlay(kSampleRate, kBlockSize);
    }

    static juce::RangedAudioParameter* findParam(juce::AudioProcessor& processor, const juce::String& paramId)
    {
        for (auto* param : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                if (ranged->getParameterID() == paramId)
                    return ranged;

        return nullptr;
    }

    void expectHasParam(const juce::String& type, const juce::String& paramId)
    {
        auto cabinet = PedalRegistry::createPedal(type);
        expect(cabinet != nullptr && findParam(*cabinet, paramId) != nullptr,
            "P11B " + type + " must expose parameter " + paramId);
    }

    void expectRoundTrip(const juce::String& type, const juce::String& paramId, float plainValue)
    {
        auto source = PedalRegistry::createPedal(type);
        auto restored = PedalRegistry::createPedal(type);
        expect(source != nullptr && restored != nullptr, "P11B roundtrip processors must be constructible for " + type);
        if (source == nullptr || restored == nullptr)
            return;

        expect(setRangedParamById(*source, paramId, plainValue),
            "P11B source should accept parameter " + paramId);

        juce::MemoryBlock state;
        source->getStateInformation(state);
        restored->setStateInformation(state.getData(), (int) state.getSize());

        auto* restoredParam = findParam(*restored, paramId);
        expect(restoredParam != nullptr, "P11B restored cabinet should contain parameter " + paramId);
        if (restoredParam == nullptr)
            return;

        const float restoredPlain = restoredParam->convertFrom0to1(restoredParam->getValue());
        expect(std::abs(restoredPlain - plainValue) <= juce::jmax(0.020f, std::abs(plainValue) * 0.010f),
            "P11B " + type + " should round-trip " + paramId
                + " expected=" + juce::String(plainValue, 3)
                + " actual=" + juce::String(restoredPlain, 3));
    }

    static void configureUsableCabinetVoice(juce::AudioProcessor& cabinet, const juce::String& type)
    {
        if (type == "Cabinet")
        {
            setRangedParamById(cabinet, "cabThump", 1.5f);
            setRangedParamById(cabinet, "cabAir", 0.0f);
            setRangedParamById(cabinet, "cabResonance", 0.0f);
            setRangedParamById(cabinet, "cabLowCut", 65.0f);
            setRangedParamById(cabinet, "cabHighCut", 7800.0f);
            setRangedParamById(cabinet, "cabDistance", 0.34f);
            setRangedParamById(cabinet, "cabMix", 1.0f);
            setRangedParamById(cabinet, "cabLevel", 0.72f);
        }
        else if (type == "Vintage 2x12")
        {
            setRangedParamById(cabinet, "v2x12Warmth", 3.0f);
            setRangedParamById(cabinet, "v2x12Sparkle", 1.5f);
            setRangedParamById(cabinet, "v2x12Resonance", 0.0f);
            setRangedParamById(cabinet, "v2x12LowCut", 82.0f);
            setRangedParamById(cabinet, "v2x12HighCut", 6200.0f);
            setRangedParamById(cabinet, "v2x12Distance", 0.28f);
            setRangedParamById(cabinet, "v2x12Mix", 1.0f);
            setRangedParamById(cabinet, "v2x12Level", 0.76f);
        }
        else if (type == "Modern 4x12")
        {
            setRangedParamById(cabinet, "m4x12Low", 1.0f);
            setRangedParamById(cabinet, "m4x12Presence", 1.8f);
            setRangedParamById(cabinet, "m4x12Resonance", 0.0f);
            setRangedParamById(cabinet, "m4x12LowCut", 72.0f);
            setRangedParamById(cabinet, "m4x12HighCut", 6200.0f);
            setRangedParamById(cabinet, "m4x12Distance", 0.34f);
            setRangedParamById(cabinet, "m4x12Mix", 1.0f);
            setRangedParamById(cabinet, "m4x12Level", 0.76f);
        }
    }

    static P11AAmpRenderStats renderCabinet(juce::AudioProcessor& cabinet)
    {
        prepare(cabinet);

        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, kBlockSize);
        P11AAmpRenderStats stats;

        for (int blockIndex = 0; blockIndex < 300; ++blockIndex)
        {
            fillCabinetExerciseBlock(block, blockIndex);
            cabinet.processBlock(block, midi);
            if (blockIndex >= 24)
                stats.capture(block);
        }

        stats.finish();
        return stats;
    }

    static void fillCabinetExerciseBlock(juce::AudioBuffer<float>& block, int blockIndex)
    {
        for (int i = 0; i < block.getNumSamples(); ++i)
        {
            const int sampleIndex = blockIndex * block.getNumSamples() + i;
            const float t = (float) sampleIndex / (float) kSampleRate;
            const float phraseEnv = blockIndex < 210 ? 1.0f : std::exp(-(float)(blockIndex - 210) / 54.0f);
            const float sample = phraseEnv
                * (0.065f * std::sin(juce::MathConstants<float>::twoPi * 98.0f * t)
                    + 0.038f * std::sin(juce::MathConstants<float>::twoPi * 196.0f * t)
                    + 0.020f * std::sin(juce::MathConstants<float>::twoPi * 392.0f * t)
                    + 0.006f * std::sin(juce::MathConstants<float>::twoPi * 5400.0f * t));

            block.setSample(0, i, sample);
            block.setSample(1, i, sample * 0.985f);
        }
    }

    static void fillCabinetNearSilenceBlock(juce::AudioBuffer<float>& block, int blockIndex)
    {
        block.clear();
        if (blockIndex == 0)
        {
            block.setSample(0, 0, 1.0e-20f);
            block.setSample(1, 0, -1.0e-20f);
        }
    }
};

class P7IDiagnosticsProfilerTests final : public juce::UnitTest
{
public:
    P7IDiagnosticsProfilerTests()
        : juce::UnitTest("P7I Diagnostics / Profiler", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("P7I CpuMeter reset clears all counters");
        {
            CpuMeter meter;
            const double start = meter.beginBlock();
            // Simulate a tiny amount of work; endBlock should record finite values.
            meter.endBlock(start, 64, 48000.0);
            expect(std::isfinite(meter.getCpuLoad()), "CpuMeter load must be finite after endBlock");
            expect(std::isfinite(meter.getLastProcessTimeMs()), "CpuMeter last process time must be finite");
            expect(std::isfinite(meter.getAverageProcessTimeMs()), "CpuMeter average process time must be finite");
            expect(std::isfinite(meter.getPeakProcessTimeMs()), "CpuMeter peak process time must be finite");

            meter.reset();
            expectEquals(meter.getCpuLoad(), 0.0);
            expectEquals(meter.getLastProcessTimeMs(), 0.0);
            expectEquals(meter.getAverageProcessTimeMs(), 0.0);
            expectEquals(meter.getPeakProcessTimeMs(), 0.0);
        }

        beginTest("P7I CpuMeter ignores invalid sample rate / block size without polluting state");
        {
            CpuMeter meter;
            const double start = meter.beginBlock();
            meter.endBlock(start, 0, 0.0);
            expect(std::isfinite(meter.getCpuLoad()), "Invalid block must not produce non-finite CPU load");
            expectEquals(meter.getCpuLoad(), 0.0);

            const double start2 = meter.beginBlock();
            meter.endBlock(start2, 64, -1.0);
            expectEquals(meter.getCpuLoad(), 0.0);
        }

        beginTest("P7I CpuMeter peak decays toward floor across many empty blocks");
        {
            CpuMeter meter;
            const double start = meter.beginBlock();
            meter.endBlock(start, 64, 48000.0);
            const double initialPeak = meter.getPeakProcessTimeMs();

            for (int i = 0; i < 10000; ++i)
            {
                const double s = meter.beginBlock();
                meter.endBlock(s, 64, 48000.0);
            }

            expect(meter.getPeakProcessTimeMs() <= initialPeak + 1.0,
                "Peak should not grow unboundedly under steady-state");
            expect(std::isfinite(meter.getPeakProcessTimeMs()), "Peak must remain finite");
        }

        beginTest("P7I DiagnosticsManager::formatProfilingLine produces deterministic shape");
        {
            AudioEngine::ProfilingResult r;
            r.blockSize = 64;
            r.processedBlocks = 750;
            r.sampleRate = 48000.0;
            r.avgMs = 0.1234;
            r.peakMs = 0.5678;
            r.avgCpuPercent = 12.34;
            r.peakCpuPercent = 45.67;
            r.invalidSamples = 0;
            r.clippedSamples = 0;
            r.dropoutBlocks = 0;
            r.clickSpikeBlocks = 0;
            r.passed = true;

            const auto line = Nova::Audio::DiagnosticsManager::formatProfilingLine(r);
            expect(line.contains("block=64"), "Profiling line must contain block size token");
            expect(line.contains("blocks=750"), "Profiling line must contain processed block count");
            expect(line.contains("avgMs="), "Profiling line must contain avgMs");
            expect(line.contains("peakMs="), "Profiling line must contain peakMs");
            expect(line.contains("avgCpu="), "Profiling line must contain avgCpu");
            expect(line.contains("peakCpu="), "Profiling line must contain peakCpu");
            expect(line.contains("passed=true"), "Profiling line must reflect passed=true");
            expect(! line.contains("notes="), "Empty notes must not be emitted");
        }

        beginTest("P7I DiagnosticsManager::formatProfilingLine appends notes when present");
        {
            AudioEngine::ProfilingResult r;
            r.blockSize = 32;
            r.processedBlocks = 1500;
            r.passed = false;
            r.notes = "Investigate dropout headroom.";

            const auto line = Nova::Audio::DiagnosticsManager::formatProfilingLine(r);
            expect(line.contains("block=32"), "Profiling line must reflect block 32");
            expect(line.contains("passed=false"), "Profiling line must reflect failed run");
            expect(line.contains("notes=Investigate dropout headroom."),
                "Non-empty notes must be appended");
        }

        beginTest("P7I DiagnosticsManager::formatProfilingResults joins lines and includes header");
        {
            std::vector<AudioEngine::ProfilingResult> results;
            for (int blockSize : { 32, 64 })
            {
                AudioEngine::ProfilingResult r;
                r.blockSize = blockSize;
                r.processedBlocks = 100;
                r.passed = true;
                results.push_back(r);
            }

            const auto report = Nova::Audio::DiagnosticsManager::formatProfilingResults(results);
            expect(report.contains("AudioEngine realtime profiling results:"),
                "Header line must be present");
            expect(report.contains("block=32"), "Block 32 entry must be present");
            expect(report.contains("block=64"), "Block 64 entry must be present");

            juce::StringArray reportLines;
            reportLines.addLines(report);
            expect(reportLines.size() >= 3, "Header plus per-result lines must be emitted");
        }

        beginTest("P7I runRealtimeProfilingSuite covers blocks 32 and 64 explicitly");
        {
            AudioEngine engine;
            engine.prepare(48000.0, 128, 2, 2);
            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();

            const auto results = engine.runRealtimeProfilingSuite(1);
            expect(results.size() >= 2, "Profiling suite must emit at least block 32 and block 64 entries");

            bool sawBlock32 = false;
            bool sawBlock64 = false;
            for (const auto& r : results)
            {
                if (r.blockSize == 32)
                    sawBlock32 = true;
                if (r.blockSize == 64)
                    sawBlock64 = true;

                expect(r.processedBlocks > 0,
                    "Block " + juce::String(r.blockSize) + " must process at least one block");
                expect(std::isfinite(r.avgMs),
                    "Block " + juce::String(r.blockSize) + " avgMs must be finite");
                expect(std::isfinite(r.peakMs),
                    "Block " + juce::String(r.blockSize) + " peakMs must be finite");
                expect(std::isfinite(r.avgCpuPercent),
                    "Block " + juce::String(r.blockSize) + " avgCpu must be finite");
                expect(std::isfinite(r.peakCpuPercent),
                    "Block " + juce::String(r.blockSize) + " peakCpu must be finite");
                expect(r.invalidSamples == 0,
                    "Block " + juce::String(r.blockSize) + " must not contain invalid samples");
            }

            expect(sawBlock32, "Profiling suite must include block 32 scenario");
            expect(sawBlock64, "Profiling suite must include block 64 scenario");
        }

        beginTest("P7I AudioEngine::buildDiagnosticReport is non-empty and contains stable fields");
        {
            AudioEngine engine;
            engine.prepare(48000.0, kBlockSize, 2, 2);
            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();

            const auto report = engine.buildDiagnosticReport();
            expect(report.isNotEmpty(), "Diagnostic report must not be empty");
            expect(report.contains("engineOn="), "Diagnostic report must report engineOn");
            expect(report.contains("sampleRate="), "Diagnostic report must report sampleRate");
            expect(report.contains("blockSize="), "Diagnostic report must report blockSize");
            expect(report.contains("cpuLoad="), "Diagnostic report must report cpuLoad");
            expect(report.contains("autoHealCount="), "Diagnostic report must report autoHealCount");
        }
    }
};

// =============================================================================
// T2-10 Latency Autopilot: decision engine on synthetic device inventories
// (no hardware). Names mirror what JUCE reports on Windows.
// =============================================================================
namespace AutopilotFixtures
{
namespace AP = Nova::AudioAutopilot;

const juce::String kRealtekMic { "Microphone Array (Realtek(R) Audio)" };
const juce::String kRealtekSpeakers { "Speakers (Realtek(R) Audio)" };
const juce::String kUrLine { "Line (Steinberg UR22C)" };
const juce::String kUrSpeakers { "Speakers (Steinberg UR22C)" };
const juce::String kBtHeadset { "Headset (WH-1000XM4 Hands-Free AG Audio)" };

AP::DriverTypeEntry makeType(const juce::String& typeName, const juce::StringArray& inputs, const juce::StringArray& outputs)
{
    AP::DriverTypeEntry entry;
    entry.typeName = typeName;
    entry.inputNames = inputs;
    entry.outputNames = outputs;
    return entry;
}

AP::ActiveDevice makeActive(const juce::String& typeName, const juce::String& input, const juce::String& output,
                            double sampleRate = 48000.0, int bufferSize = 480)
{
    AP::ActiveDevice active;
    active.isOpen = true;
    active.typeName = typeName;
    active.inputName = input;
    active.outputName = output;
    active.sampleRate = sampleRate;
    active.bufferSize = bufferSize;
    active.numInputChannels = input.isNotEmpty() ? 2 : 0;
    active.numOutputChannels = 2;
    active.availableSampleRates = { 44100.0, 48000.0 };
    active.availableBufferSizes = { 32, 64, 128, 256, 480 };
    return active;
}

void addWindowsTypes(AP::Inventory& inventory, const juce::StringArray& inputs, const juce::StringArray& outputs)
{
    inventory.types.push_back(makeType("Windows Audio", inputs, outputs));
    inventory.types.push_back(makeType("Windows Audio (Exclusive Mode)", inputs, outputs));
    inventory.types.push_back(makeType("Windows Audio (Low Latency Mode)", inputs, outputs));

    auto dsInputs = inputs;
    auto dsOutputs = outputs;
    dsInputs.insert(0, "Primary Sound Capture Driver");
    dsOutputs.insert(0, "Primary Sound Driver");
    inventory.types.push_back(makeType("DirectSound", dsInputs, dsOutputs));
}

void addAsioType(AP::Inventory& inventory, const juce::StringArray& drivers)
{
    inventory.types.push_back(makeType("ASIO", drivers, drivers));
}

// Laptop with only its onboard codec, running the JUCE default (shared mode).
AP::Inventory makeLaptop()
{
    AP::Inventory inventory;
    addWindowsTypes(inventory, { kRealtekMic }, { kRealtekSpeakers });
    inventory.active = makeActive("Windows Audio", kRealtekMic, kRealtekSpeakers);
    return inventory;
}

// Same laptop with a USB interface plugged in and several ASIO drivers installed.
AP::Inventory makeInterfaceRig()
{
    AP::Inventory inventory;
    addWindowsTypes(inventory, { kRealtekMic, kUrLine }, { kRealtekSpeakers, kUrSpeakers });
    addAsioType(inventory, { "Yamaha Steinberg USB ASIO", "ASIO4ALL v2", "Ableton Move" });
    inventory.active = makeActive("Windows Audio", kRealtekMic, kRealtekSpeakers);
    return inventory;
}

int indexOfKey(const std::vector<AP::Candidate>& ranked, const juce::String& key)
{
    for (size_t i = 0; i < ranked.size(); ++i)
        if (ranked[i].getKey() == key)
            return (int) i;

    return -1;
}

bool reasonsMention(const AP::Candidate& candidate, const juce::String& text)
{
    for (const auto& reason : candidate.reasons)
        if (reason.containsIgnoreCase(text))
            return true;

    return false;
}
}

class AudioAutopilotTests final : public juce::UnitTest
{
public:
    AudioAutopilotTests()
        : juce::UnitTest("Audio Autopilot", "NOVA")
    {
    }

    void runTest() override
    {
        using namespace AutopilotFixtures;

        beginTest("AudioAutopilot classifies JUCE driver types and ASIO drivers");
        {
            expect(AP::classifyDriverType("ASIO") == AP::DriverFamily::Asio);
            expect(AP::classifyDriverType("Windows Audio") == AP::DriverFamily::WasapiShared);
            expect(AP::classifyDriverType("Windows Audio (Exclusive Mode)") == AP::DriverFamily::WasapiExclusive);
            expect(AP::classifyDriverType("Windows Audio (Low Latency Mode)") == AP::DriverFamily::WasapiLowLatency);
            expect(AP::classifyDriverType("DirectSound") == AP::DriverFamily::DirectSound);

            expect(AP::classifyAsioDriver("Yamaha Steinberg USB ASIO") == AP::AsioDriverClass::Native);
            expect(AP::classifyAsioDriver("Focusrite USB ASIO") == AP::AsioDriverClass::Native);
            expect(AP::classifyAsioDriver("ASIO4ALL v2") == AP::AsioDriverClass::Generic);
            expect(AP::classifyAsioDriver("FlexASIO") == AP::AsioDriverClass::Generic);
            expect(AP::classifyAsioDriver("Realtek ASIO") == AP::AsioDriverClass::Generic);
            expect(AP::classifyAsioDriver("Voicemeeter Virtual ASIO") == AP::AsioDriverClass::Virtual);

            const auto tokens = AP::getBrandTokens("Yamaha Steinberg USB ASIO");
            expectEquals(tokens.joinIntoString(","), juce::String("yamaha,steinberg"));
        }

        beginTest("AudioAutopilot reads Windows endpoint names");
        {
            const auto line = AP::classifyEndpoint(kUrLine);
            expectEquals(line.label, juce::String("Line"));
            expectEquals(line.device, juce::String("Steinberg UR22C"));
            expect(! line.builtIn && ! line.microphone && ! line.bluetooth);

            const auto speakers = AP::classifyEndpoint(kRealtekSpeakers);
            expectEquals(speakers.device, juce::String("Realtek(R) Audio"));
            expect(speakers.builtIn && speakers.speakers && ! speakers.headphones);

            const auto intelMic = AP::classifyEndpoint(juce::String(juce::CharPointer_UTF8(
                "Microphone Array (Intel\xc2\xae Smart Sound Technology for Digital Microphones)")));
            expect(intelMic.builtIn && intelMic.microphone);

            expect(AP::classifyEndpoint(kBtHeadset).bluetooth);
            expect(! AP::classifyEndpoint(kBtHeadset).builtIn);
            expectEquals(AP::classifyEndpoint("Speakers (2- USB Audio CODEC)").device, juce::String("USB Audio CODEC"));
            expect(AP::classifyEndpoint("CABLE Output (VB-Audio Virtual Cable)").virtualDevice);
            expect(AP::classifyEndpoint("Stereo Mix (Realtek(R) Audio)").virtualDevice);
            expect(AP::classifyEndpoint("Microphone (HD Pro Webcam C920)").webcam);
            expect(AP::classifyEndpoint("Speakers (NVIDIA High Definition Audio)").display);
            expect(! AP::classifyEndpoint("Speakers (NVIDIA High Definition Audio)").builtIn);
            expect(AP::classifyEndpoint("Auriculares (Realtek(R) Audio)").headphones);
            expect(AP::classifyEndpoint("Altavoces (Realtek(R) Audio)").speakers);
            expect(AP::classifyEndpoint("Primary Sound Capture Driver").systemAlias);
            expect(AP::isSameDevice(AP::classifyEndpoint(kUrLine), AP::classifyEndpoint(kUrSpeakers)));
            expect(! AP::isSameDevice(AP::classifyEndpoint(kUrLine), AP::classifyEndpoint(kRealtekSpeakers)));
        }

        beginTest("AudioAutopilot prefers the connected interface's native ASIO driver");
        {
            const auto inventory = makeInterfaceRig();
            const auto recommendation = AP::recommend(inventory);

            expect(recommendation.kind == AP::RecommendationKind::SwitchDevice);
            expectEquals(recommendation.candidate.typeName, juce::String("ASIO"));
            expectEquals(recommendation.candidate.outputName, juce::String("Yamaha Steinberg USB ASIO"));
            expect(recommendation.candidate.tier == AP::Tier::AsioNativeVerified);
            expectEquals(recommendation.candidate.sampleRate, 48000.0);
            expectEquals(recommendation.candidate.bufferSize, 0, "ASIO keeps the buffer chosen in the driver panel");

            const auto ranked = AP::rankCandidates(inventory);
            expectEquals((int) ranked.size(), 7);
            const AP::Tier expectedOrder[] = { AP::Tier::AsioNativeVerified, AP::Tier::WasapiExclusive,
                                               AP::Tier::WasapiLowLatency, AP::Tier::AsioNativeUnverified,
                                               AP::Tier::AsioGeneric, AP::Tier::WasapiShared, AP::Tier::DirectSound };
            for (size_t i = 0; i < ranked.size() && i < std::size(expectedOrder); ++i)
                expect(ranked[i].tier == expectedOrder[i], "Unexpected tier at rank " + juce::String((int) i));

            expectEquals(ranked[1].inputName, kUrLine, "WASAPI keeps input and output on the interface");
            expectEquals(ranked[1].outputName, kUrSpeakers);
            expectEquals(ranked[1].bufferSize, 128);
            expectEquals(ranked[3].outputName, juce::String("Ableton Move"));
            expectEquals(ranked[4].outputName, juce::String("ASIO4ALL v2"));
            expect(reasonsMention(ranked[1], "same device"));
        }

        beginTest("AudioAutopilot falls back to WASAPI Exclusive or Low Latency without ASIO");
        {
            const auto inventory = makeLaptop();
            const auto recommendation = AP::recommend(inventory);

            expect(recommendation.kind == AP::RecommendationKind::SwitchDevice);
            expectEquals(recommendation.candidate.typeName, juce::String("Windows Audio (Exclusive Mode)"));
            expectEquals(recommendation.candidate.inputName, kRealtekMic);
            expectEquals(recommendation.candidate.outputName, kRealtekSpeakers);
            expectEquals(recommendation.candidate.bufferSize, 128);
            expect(reasonsMention(recommendation.candidate, "built-in microphone"));

            AP::Options noExclusive;
            noExclusive.allowExclusive = false;
            const auto withoutExclusive = AP::recommend(inventory, noExclusive);
            expectEquals(withoutExclusive.candidate.typeName, juce::String("Windows Audio (Low Latency Mode)"));

            for (const auto& candidate : AP::rankCandidates(inventory, noExclusive))
                expect(candidate.tier != AP::Tier::WasapiExclusive);

            for (const auto& candidate : AP::rankCandidates(inventory))
                expect(! candidate.inputName.startsWith("Primary Sound"), "DirectSound aliases are never candidates");
        }

        beginTest("AudioAutopilot ranks ASIO wrappers below WASAPI Low Latency");
        {
            auto inventory = makeLaptop();
            addAsioType(inventory, { "ASIO4ALL v2", "FlexASIO" });

            const auto ranked = AP::rankCandidates(inventory);
            expectEquals(AP::recommend(inventory).candidate.typeName, juce::String("Windows Audio (Exclusive Mode)"));
            expectEquals(indexOfKey(ranked, "ASIO|ASIO4ALL v2|ASIO4ALL v2"), 2);
            expect(ranked[2].tier == AP::Tier::AsioGeneric);
            expect(ranked[4].tier == AP::Tier::WasapiShared);
        }

        beginTest("AudioAutopilot does not trust ASIO drivers whose hardware is unplugged");
        {
            auto unplugged = makeLaptop();
            addAsioType(unplugged, { "Ableton Move" });
            const auto ranked = AP::rankCandidates(unplugged);
            expectEquals(AP::recommend(unplugged).candidate.typeName, juce::String("Windows Audio (Exclusive Mode)"));
            expect(ranked[indexOfKey(ranked, "ASIO|Ableton Move|Ableton Move")].tier == AP::Tier::AsioNativeUnverified);

            AP::Inventory push;
            addWindowsTypes(push, { kRealtekMic, "Line (Ableton Push 3)" }, { kRealtekSpeakers, "Speakers (Ableton Push 3)" });
            addAsioType(push, { "Ableton Move", "Ableton Push" });
            push.active = makeActive("Windows Audio", kRealtekMic, kRealtekSpeakers);
            expectEquals(AP::recommend(push).candidate.outputName, juce::String("Ableton Push"),
                         "The driver whose name matches the plugged hardware best wins");
        }

        beginTest("AudioAutopilot avoids Bluetooth outputs and never opens a Bluetooth mic");
        {
            AP::Inventory inventory;
            addWindowsTypes(inventory, { kRealtekMic, kBtHeadset }, { kBtHeadset, kRealtekSpeakers });
            inventory.active = makeActive("Windows Audio", kRealtekMic, kBtHeadset);

            for (const auto& candidate : AP::rankCandidates(inventory))
            {
                expect(candidate.inputName != kBtHeadset, "Bluetooth headset mic must never be a candidate input");
                expect(candidate.outputName == kRealtekSpeakers, "Any wired output beats Bluetooth");
            }

            AP::Inventory onlyBluetooth;
            addWindowsTypes(onlyBluetooth, { kRealtekMic }, { kBtHeadset });
            const auto recommendation = AP::recommend(onlyBluetooth);
            expectEquals(recommendation.candidate.outputName, kBtHeadset);
            expect(reasonsMention(recommendation.candidate, "Bluetooth"), "Bluetooth latency must be explained");
        }

        beginTest("AudioAutopilot keeps input and output on the same device when possible");
        {
            AP::Inventory inventory;
            addWindowsTypes(inventory,
                            { "Microphone (Blue Yeti)", "Analogue 1 + 2 (Focusrite USB Audio)" },
                            { kRealtekSpeakers, "Speakers (Focusrite USB Audio)" });
            const auto recommendation = AP::recommend(inventory);
            expectEquals(recommendation.candidate.inputName, juce::String("Analogue 1 + 2 (Focusrite USB Audio)"));
            expectEquals(recommendation.candidate.outputName, juce::String("Speakers (Focusrite USB Audio)"));

            AP::Inventory split;
            addWindowsTypes(split, { "Microphone (Blue Yeti)" }, { kRealtekSpeakers });
            expect(reasonsMention(AP::recommend(split).candidate, "different devices"));
        }

        beginTest("AudioAutopilot recognises a running setup that is already optimal");
        {
            auto onAsio = makeInterfaceRig();
            onAsio.active = makeActive("ASIO", "Yamaha Steinberg USB ASIO", "Yamaha Steinberg USB ASIO", 48000.0, 64);
            expect(AP::recommend(onAsio).kind == AP::RecommendationKind::AlreadyOptimal);

            auto at44k = onAsio;
            at44k.active.sampleRate = 44100.0;
            const auto rateChange = AP::recommend(at44k);
            expect(rateChange.kind == AP::RecommendationKind::ChangeSampleRate);
            expectEquals(rateChange.candidate.sampleRate, 48000.0);
            expectEquals(rateChange.candidate.bufferSize, 64, "A rate change keeps the current buffer");

            auto only44k = at44k;
            only44k.active.availableSampleRates = { 44100.0 };
            expect(AP::recommend(only44k).kind == AP::RecommendationKind::AlreadyOptimal);

            // A vendor driver with a generic name is trusted once it is running.
            auto generic = makeLaptop();
            addAsioType(generic, { "TUSBAudio ASIO Driver" });
            generic.active = makeActive("ASIO", "TUSBAudio ASIO Driver", "TUSBAudio ASIO Driver");
            const auto running = AP::recommend(generic);
            expect(running.kind == AP::RecommendationKind::AlreadyOptimal);
            expect(running.candidate.tier == AP::Tier::AsioNativeVerified);
        }

        beginTest("AudioAutopilot skips candidates that already failed this session");
        {
            const auto inventory = makeInterfaceRig();
            const juce::StringArray failed { "ASIO|Yamaha Steinberg USB ASIO|Yamaha Steinberg USB ASIO" };
            const auto next = AP::recommend(inventory, {}, failed);
            expectEquals(next.candidate.typeName, juce::String("Windows Audio (Exclusive Mode)"));
            expectEquals(next.candidate.outputName, kUrSpeakers);
        }

        beginTest("AudioAutopilot reports no candidate without a usable input");
        {
            AP::Inventory noInputs;
            addWindowsTypes(noInputs, {}, { kRealtekSpeakers });
            expect(AP::recommend(noInputs).kind == AP::RecommendationKind::NoCandidate);

            AP::Inventory virtualOnly;
            addWindowsTypes(virtualOnly, { "CABLE Output (VB-Audio Virtual Cable)" }, { kRealtekSpeakers });
            addAsioType(virtualOnly, { "Voicemeeter Virtual ASIO" });
            expect(AP::recommend(virtualOnly).kind == AP::RecommendationKind::NoCandidate);

            expect(AP::recommend(AP::Inventory {}).kind == AP::RecommendationKind::NoCandidate);
        }

        beginTest("AudioAutopilot input mute policy unmutes only when it is safe");
        {
            const auto advise = [] (const juce::String& type, const juce::String& input, const juce::String& output)
            {
                return AP::adviseInputMute(makeActive(type, input, output));
            };

            const auto asio = advise("ASIO", "Yamaha Steinberg USB ASIO", "Yamaha Steinberg USB ASIO");
            expect(asio.advice == AP::MuteAdvice::Unmute);

            const auto wrapper = advise("ASIO", "ASIO4ALL v2", "ASIO4ALL v2");
            expect(wrapper.advice == AP::MuteAdvice::KeepMuted && ! wrapper.feedbackRisk);

            const auto laptop = advise("Windows Audio", kRealtekMic, kRealtekSpeakers);
            expect(laptop.advice == AP::MuteAdvice::KeepMuted && laptop.feedbackRisk);
            expect(laptop.reason.containsIgnoreCase("feedback"));

            expect(advise("Windows Audio", kRealtekMic, "Headphones (Realtek(R) Audio)").advice == AP::MuteAdvice::Unmute);
            expect(advise("Windows Audio", kRealtekMic, "Auriculares (Realtek(R) Audio)").advice == AP::MuteAdvice::Unmute);
            expect(advise("Windows Audio (Exclusive Mode)", kUrLine, kUrSpeakers).advice == AP::MuteAdvice::Unmute);
            expect(advise("Windows Audio", kUrLine, kRealtekSpeakers).advice == AP::MuteAdvice::Unmute);

            const auto usbMic = advise("Windows Audio", "Microphone (Blue Yeti)", kRealtekSpeakers);
            expect(usbMic.advice == AP::MuteAdvice::KeepMuted && ! usbMic.feedbackRisk);

            const auto webcam = advise("Windows Audio", "Microphone (HD Pro Webcam C920)", "Speakers (NVIDIA High Definition Audio)");
            expect(webcam.advice == AP::MuteAdvice::KeepMuted && webcam.feedbackRisk);

            const auto bluetooth = advise("Windows Audio", kBtHeadset, kBtHeadset);
            expect(bluetooth.advice == AP::MuteAdvice::KeepMuted && bluetooth.feedbackRisk);

            const auto loopback = advise("Windows Audio", "Stereo Mix (Realtek(R) Audio)", "Headphones (Realtek(R) Audio)");
            expect(loopback.advice == AP::MuteAdvice::KeepMuted && loopback.feedbackRisk);

            expect(advise("DirectSound", "Primary Sound Capture Driver", "Primary Sound Driver").advice == AP::MuteAdvice::KeepMuted);
            expect(advise("Windows Audio", {}, kRealtekSpeakers).advice == AP::MuteAdvice::NoInput);

            auto closed = makeActive("ASIO", "Yamaha Steinberg USB ASIO", "Yamaha Steinberg USB ASIO");
            closed.isOpen = false;
            expect(AP::adviseInputMute(closed).advice == AP::MuteAdvice::NoInput);
        }

        beginTest("AudioAutopilot describes candidates for the UI");
        {
            const auto recommendation = AP::recommend(makeInterfaceRig());
            const juce::String expected(juce::CharPointer_UTF8("ASIO  \xc2\xb7  Yamaha Steinberg USB ASIO  \xc2\xb7  48 kHz"));
            expectEquals(AP::describeCandidate(recommendation.candidate), expected);

            const auto laptop = AP::recommend(makeLaptop());
            expect(AP::describeCandidate(laptop.candidate).contains("In: " + kRealtekMic));
            expect(AP::describeCandidate(laptop.candidate).endsWith("128 samples"));
        }

        beginTest("AudioAutopilot calibration ladder walks up from the smallest buffer");
        {
            const auto joined = [] (const juce::Array<int>& sizes)
            {
                juce::StringArray parts;
                for (int size : sizes)
                    parts.add(juce::String(size));
                return parts.joinIntoString(",");
            };

            // ASIO drivers usually list powers of two.
            expectEquals(joined(AP::buildCalibrationLadder({ 16, 32, 64, 128, 256, 512, 1024, 2048 })),
                         juce::String("32,64,128,256,512"));

            // WASAPI Exclusive lists every multiple of 32 above the device minimum: keep it short.
            juce::Array<int> exclusive { 144, 480 };
            for (int size = 160; size <= 2048; size += 32)
                exclusive.addIfNotAlreadyThere(size);
            expectEquals(joined(AP::buildCalibrationLadder(exclusive)), juce::String("144,192,256,384,512"));

            // Unsorted input, and a fine-grained Low Latency list.
            expectEquals(joined(AP::buildCalibrationLadder({ 512, 64, 32, 128, 96, 48 })), juce::String("32,48,64,96,128,512"));

            // Nothing at or below the limit: still one measurement at the smallest size.
            expectEquals(joined(AP::buildCalibrationLadder({ 1024, 2048 })), juce::String("1024"));
            expect(AP::buildCalibrationLadder({}).isEmpty());

            AP::CalibrationOptions playAlong;
            playAlong.firstBufferSamples = 64;
            expectEquals(joined(AP::buildCalibrationLadder({ 32, 64, 128 }, playAlong)), juce::String("64,128"));
        }

        beginTest("AudioAutopilot stability verdict respects each threshold");
        {
            // 64 samples at 48 kHz = 1.333 ms per block; 4 s = 3000 blocks expected.
            AP::StabilityMeasurement clean;
            clean.bufferSize = 64;
            clean.sampleRate = 48000.0;
            clean.windowSeconds = 4.0;
            clean.xruns = 0;
            clean.peakProcessMs = 0.40;
            clean.maxCallbackGapMs = 1.70;
            clean.blocksDelivered = 3000;

            const auto stable = AP::evaluateStability(clean);
            expect(stable.stable, "Clean measurement must pass: " + stable.summary);
            expectWithinAbsoluteError(stable.peakLoad, 0.30, 1.0e-6);
            expectWithinAbsoluteError(stable.gapRatio, 1.275, 1.0e-6);
            expectWithinAbsoluteError(stable.deliveredRatio, 1.0, 1.0e-6);

            auto dropout = clean;
            dropout.xruns = 1;
            expect(! AP::evaluateStability(dropout).stable);
            expect(AP::evaluateStability(dropout).summary.contains("dropout"));

            auto justBelowLoad = clean;
            justBelowLoad.peakProcessMs = 0.69 * 1000.0 * 64.0 / 48000.0;
            expect(AP::evaluateStability(justBelowLoad).stable, "69% peak load passes");

            auto atLoadLimit = clean;
            atLoadLimit.peakProcessMs = 0.705 * 1000.0 * 64.0 / 48000.0;
            expect(! AP::evaluateStability(atLoadLimit).stable, "Over 70% peak load fails");
            expect(AP::evaluateStability(atLoadLimit).summary.contains("peak load"));

            auto justBelowGap = clean;
            justBelowGap.maxCallbackGapMs = 1.99 * 1000.0 * 64.0 / 48000.0;
            expect(AP::evaluateStability(justBelowGap).stable, "1.99x gap passes (USB drivers deliver in bursts)");

            auto atGapLimit = clean;
            atGapLimit.maxCallbackGapMs = 2.01 * 1000.0 * 64.0 / 48000.0;
            expect(! AP::evaluateStability(atGapLimit).stable, "A gap over 2 blocks fails");

            auto missingCallbacks = clean;
            missingCallbacks.blocksDelivered = 2669; // 89%
            expect(! AP::evaluateStability(missingCallbacks).stable);
            missingCallbacks.blocksDelivered = 2700; // 90%
            expect(AP::evaluateStability(missingCallbacks).stable);

            AP::StabilityMeasurement empty;
            expect(! AP::evaluateStability(empty).stable);
        }
    }
};

// T2-01 F0 (docs/reports/LAT-T2-01-live-mode.md): worst-case measurements of the
// output limiter and the Noise Gate in Studio and Live, latency breakdown of the
// typical chain, and the B-11 graph transition. Ports the offline Python
// simulations to C++. Measured values go to the log with the "T2-01 F0" prefix.
class T201LiveModeF0Tests final : public juce::UnitTest
{
public:
    T201LiveModeF0Tests()
        : juce::UnitTest("T2-01 F0 Live mode measurements", "NOVA")
    {
    }

    void runTest() override
    {
        runLimiterSuite();
        runGateSuite();
        runLatencyBreakdown();
        runB11Transition();
    }

private:
    using Signal = std::vector<float>;

    static constexpr double kRate = 48000.0;
    static constexpr int kBlock = 64;
    static constexpr float kLimiterCeiling = 0.97f;

    struct OutputRun
    {
        Signal output;
        int latency = 0;
        OutputChainProcessor::DebugSnapshot snapshot;
        bool finite = true;
    };

    struct LimiterCase
    {
        juce::String name;
        Signal input;
        float masterDb = 0.0f;
        std::vector<int> onsets;
        bool sustained = false;
        bool postCab = false;
    };

    // ---------------------------------------------------------------- helpers

    static float toDb(double linear) noexcept
    {
        return (float)(20.0 * std::log10(juce::jmax(1.0e-12, linear)));
    }

    static float energyRatioDb(double numerator, double denominator) noexcept
    {
        return (float)(10.0 * std::log10(juce::jmax(1.0e-30, numerator) / juce::jmax(1.0e-30, denominator)));
    }

    static OutputRun runOutputChain(bool live, const Signal& input, float masterDb)
    {
        OutputRun run;
        auto output = std::make_unique<OutputChainProcessor>();
        output->setLiveMode(live);
        output->setPlayConfigDetails(2, 2, kRate, kBlock);
        output->setParams(masterDb, 0.0f);
        output->prepareToPlay(kRate, kBlock);
        run.latency = output->getLatencySamples();

        // Flush the latency so the aligned output covers the whole input.
        const int total = (int)input.size() + run.latency + kBlock;
        run.output.assign((size_t)total, 0.0f);

        juce::AudioBuffer<float> block(2, kBlock);
        juce::MidiBuffer midi;

        for (int start = 0; start < total; start += kBlock)
        {
            const int count = juce::jmin(kBlock, total - start);
            block.setSize(2, count, false, false, true);

            for (int i = 0; i < count; ++i)
            {
                const int n = start + i;
                const float x = n < (int)input.size() ? input[(size_t)n] : 0.0f;
                block.setSample(0, i, x);
                block.setSample(1, i, x);
            }

            output->processBlock(block, midi);
            run.finite = run.finite && bufferHasOnlyFiniteSamples(block);

            // Both channels carry the same signal (linked limiter), so one is enough.
            for (int i = 0; i < count; ++i)
                run.output[(size_t)(start + i)] = block.getSample(0, i);
        }

        run.snapshot = output->getDebugSnapshot();
        return run;
    }

    // Output sample n aligned with input sample n (removes the reported latency).
    static float aligned(const OutputRun& run, int n) noexcept
    {
        const int index = n + run.latency;
        return juce::isPositiveAndBelow(index, (int)run.output.size()) ? run.output[(size_t)index] : 0.0f;
    }

    static float samplePeak(const Signal& x) noexcept
    {
        float peak = 0.0f;
        for (float v : x)
            peak = juce::jmax(peak, std::abs(v));
        return peak;
    }

    // True peak via 8x linear-phase FIR oversampling (BS.1770-style estimate).
    static float truePeak(const Signal& x)
    {
        juce::dsp::Oversampling<float> oversampler(1, 3,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
        oversampler.initProcessing((size_t)kBlock);

        juce::AudioBuffer<float> chunk(1, kBlock);
        float peak = 0.0f;
        const int total = (int)x.size() + 4 * kBlock;

        for (int start = 0; start < total; start += kBlock)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const int n = start + i;
                chunk.setSample(0, i, n < (int)x.size() ? x[(size_t)n] : 0.0f);
            }

            juce::dsp::AudioBlock<float> block(chunk);
            auto up = oversampler.processSamplesUp(block);
            const auto* data = up.getChannelPointer(0);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                peak = juce::jmax(peak, std::abs(data[i]));
        }

        return peak;
    }

    static Signal applyButterworth(const Signal& x, float cutoffHz, bool highPass)
    {
        auto coefficients = highPass
            ? juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod(cutoffHz, kRate, 4)
            : juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod(cutoffHz, kRate, 4);

        std::vector<juce::dsp::IIR::Filter<float>> filters;
        for (auto* c : coefficients)
            filters.emplace_back(c);

        Signal y(x.size());
        for (size_t n = 0; n < x.size(); ++n)
        {
            float v = x[n];
            for (auto& f : filters)
                v = f.processSample(v);
            y[n] = v;
        }

        return y;
    }

    static void normalisePeak(Signal& x, float targetPeak)
    {
        const float peak = samplePeak(x);
        if (peak <= 0.0f)
            return;

        const float scale = targetPeak / peak;
        for (auto& v : x)
            v *= scale;
    }

    // Plucked guitar-like notes (82-330 Hz) every 250 ms with a short pick noise burst.
    static Signal makePlucks(double seconds, std::vector<int>& onsets)
    {
        static constexpr double notes[] = { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 };
        const int length = (int)(seconds * kRate);
        const int period = (int)(0.25 * kRate);
        juce::Random rng(0x7201F0);

        Signal x((size_t)length, 0.0f);
        onsets.clear();

        for (int n = 0; n < length; ++n)
        {
            const int pluck = n / period;
            const int offset = n - pluck * period;
            if (offset == 0)
                onsets.push_back(n);

            const double tp = offset / kRate;
            const double f = notes[pluck % 6];
            double v = 0.0;

            for (int k = 1; k <= 24 && k * f < 0.45 * kRate; ++k)
                v += (1.0 / k) * std::exp(-tp * (2.5 + 0.9 * k))
                    * std::sin(juce::MathConstants<double>::twoPi * k * f * tp + 0.7 * k);

            v *= juce::jmin(1.0, tp / 0.0005);
            v += 0.3 * std::exp(-tp / 0.002) * (rng.nextFloat() * 2.0f - 1.0f);
            x[(size_t)n] = (float)v;
        }

        return x;
    }

    static Signal makeSine(double seconds, double frequency, double amplitude, double phase = 0.0)
    {
        Signal x((size_t)(seconds * kRate));
        for (size_t n = 0; n < x.size(); ++n)
            x[n] = (float)(amplitude * std::sin(juce::MathConstants<double>::twoPi * frequency * (double)n / kRate + phase));
        return x;
    }

    static std::vector<LimiterCase> buildLimiterCases()
    {
        std::vector<LimiterCase> cases;
        const float overSix = kLimiterCeiling * juce::Decibels::decibelsToGain(6.0f);
        const float overTwelve = kLimiterCeiling * juce::Decibels::decibelsToGain(12.0f);

        for (float amplitude : { 1.0f, 2.0f })
        {
            LimiterCase c;
            c.name = "impulse_" + juce::String(amplitude, 1);
            c.input.assign((size_t)(0.5 * kRate), 0.0f);
            c.input[(size_t)(0.1 * kRate)] = amplitude;
            c.onsets = { (int)(0.1 * kRate) };
            cases.push_back(c);
        }

        {
            LimiterCase c;
            c.name = "sine_1k_0dBFS";
            c.input = makeSine(1.0, 1000.0, 1.0);
            c.sustained = true;
            cases.push_back(c);

            c.name = "sine_1k_+6dB";
            c.input = makeSine(1.0, 1000.0, 2.0);
            cases.push_back(c);

            c.name = "sine_4321_+6dB";
            c.input = makeSine(1.0, 4321.0, 2.0);
            cases.push_back(c);
        }

        {
            // fs/4 tone at 45 degrees: samples at 0.919, true peak 1.3 (B-13 case).
            LimiterCase c;
            c.name = "tone_fs4_45deg";
            c.input = makeSine(1.0, kRate / 4.0, 1.3, juce::MathConstants<double>::pi / 4.0);
            c.sustained = true;
            cases.push_back(c);
        }

        {
            LimiterCase c;
            c.name = "ramp_-12_to_+12dB";
            c.input = makeSine(2.0, 1000.0, 1.0);
            for (size_t n = 0; n < c.input.size(); ++n)
                c.input[n] *= juce::Decibels::decibelsToGain(-12.0f + 24.0f * (float)n / (float)c.input.size());
            cases.push_back(c);
        }

        {
            // P12D scenario of the investigation: a 0.5 sine with master +10.73 dB.
            LimiterCase c;
            c.name = "p12d_master_+10.73dB";
            c.input = makeSine(1.0, 220.0, 0.5);
            c.masterDb = 10.73f;
            c.sustained = true;
            cases.push_back(c);

            // Two-tone variant of the P12D unit test (220 + 277 Hz): the beating
            // envelope makes the gain move, so its residual is intermodulation.
            c.name = "p12d_twotone_master_+10.73dB";
            c.input.resize((size_t)kRate);
            for (size_t n = 0; n < c.input.size(); ++n)
            {
                const double t = (double)n / kRate;
                c.input[n] = (float)(0.25 * std::sin(juce::MathConstants<double>::twoPi * 220.0 * t)
                    + 0.25 * std::sin(juce::MathConstants<double>::twoPi * 277.0 * t + 0.19));
            }
            normalisePeak(c.input, 0.5f);
            cases.push_back(c);
        }

        std::vector<int> onsets;
        const auto plucks = makePlucks(2.0, onsets);

        for (float target : { overSix, overTwelve })
        {
            LimiterCase c;
            c.name = target == overSix ? "picks_postcab_+6dB" : "picks_postcab_+12dB";
            c.input = applyButterworth(plucks, 5500.0f, false);
            normalisePeak(c.input, target);
            c.onsets = onsets;
            c.postCab = true;
            cases.push_back(c);
        }

        {
            Signal driven = plucks;
            normalisePeak(driven, 1.0f);
            for (auto& v : driven)
                v = std::tanh(10.0f * v);

            LimiterCase c;
            c.name = "picks_highgain_postcab_+6dB";
            c.input = applyButterworth(driven, 5500.0f, false);
            normalisePeak(c.input, overSix);
            c.onsets = onsets;
            c.postCab = true;
            cases.push_back(c);

            // No cabinet: NOVA used as a preamp into a real amp or FRFR.
            c.name = "picks_highgain_nocab_+6dB";
            c.input = driven;
            normalisePeak(c.input, overSix);
            c.postCab = false;
            cases.push_back(c);
        }

        {
            LimiterCase c;
            c.name = "picks_fullband_nocab_+6dB";
            c.input = plucks;
            normalisePeak(c.input, overSix);
            c.onsets = onsets;
            cases.push_back(c);
        }

        return cases;
    }

    // Nonlinear residual of a sustained case: hot render against a reference render
    // scaled below the threshold (same DC blockers and latency), least-squares gain
    // fit over the settled second half.
    static float sustainedResidualDb(bool live, const LimiterCase& c, const OutputRun& hot)
    {
        constexpr float kReferenceScale = 0.25f;
        Signal quiet = c.input;
        const float quietPeak = samplePeak(quiet) * juce::Decibels::decibelsToGain(c.masterDb);
        const float scale = kReferenceScale / juce::jmax(1.0e-6f, quietPeak);
        for (auto& v : quiet)
            v *= scale;

        const auto reference = runOutputChain(live, quiet, c.masterDb);
        const int start = (int)c.input.size() / 2;
        const int end = (int)c.input.size();

        double cross = 0.0;
        double refEnergy = 0.0;
        for (int n = start; n < end; ++n)
        {
            cross += (double)aligned(hot, n) * aligned(reference, n);
            refEnergy += (double)aligned(reference, n) * aligned(reference, n);
        }

        const double gain = cross / juce::jmax(1.0e-30, refEnergy);
        double residual = 0.0;
        double signal = 0.0;
        for (int n = start; n < end; ++n)
        {
            const double fit = gain * aligned(reference, n);
            const double error = aligned(hot, n) - fit;
            residual += error * error;
            signal += fit * fit;
        }

        return energyRatioDb(residual, signal);
    }

    struct Splatter
    {
        float absoluteDb = 0.0f;   // C5 as defined in the investigation: out_HF / in_HF
        float normalisedDb = 0.0f; // out_HF / in_HF relative to out / in broadband
    };

    // Energy above 9 kHz in the first 10 ms of each attack. "absolute" compares the
    // treble out against the treble in (criterion C5). "normalised" also discounts the
    // gain reduction, so 0 dB means the limiter scaled the attack without adding treble.
    static Splatter attackSplatter(const LimiterCase& c, const OutputRun& run)
    {
        Signal alignedOutput(c.input.size());
        for (size_t n = 0; n < c.input.size(); ++n)
            alignedOutput[n] = aligned(run, (int)n);

        const auto inputHf = applyButterworth(c.input, 9000.0f, true);
        const auto outputHf = applyButterworth(alignedOutput, 9000.0f, true);
        const int window = (int)(0.010 * kRate);

        double inHf = 0.0, outHf = 0.0, inAll = 0.0, outAll = 0.0;
        for (int onset : c.onsets)
        {
            for (int n = onset; n < juce::jmin(onset + window, (int)c.input.size()); ++n)
            {
                inHf += (double)inputHf[(size_t)n] * inputHf[(size_t)n];
                outHf += (double)outputHf[(size_t)n] * outputHf[(size_t)n];
                inAll += (double)c.input[(size_t)n] * c.input[(size_t)n];
                outAll += (double)alignedOutput[(size_t)n] * alignedOutput[(size_t)n];
            }
        }

        Splatter splatter;
        splatter.absoluteDb = energyRatioDb(outHf, inHf);
        splatter.normalisedDb = splatter.absoluteDb - energyRatioDb(outAll, inAll);
        return splatter;
    }

    // ------------------------------------------------------------ limiter suite

    void runLimiterSuite()
    {
        beginTest("T2-01 F0 limiter worst case: Studio lookahead vs Live without lookahead");

        for (const auto& c : buildLimiterCases())
        {
            const auto studio = runOutputChain(false, c.input, c.masterDb);
            const auto live = runOutputChain(true, c.input, c.masterDb);

            expectEquals(live.latency, 0, c.name + ": Live OutputChain must report zero latency");
            expect(studio.latency > 0, c.name + ": Studio OutputChain must report its lookahead");
            expect(studio.finite && live.finite, c.name + ": render must stay finite");

            const float studioPeak = samplePeak(studio.output);
            const float livePeak = samplePeak(live.output);
            const float studioTp = truePeak(studio.output);
            const float liveTp = truePeak(live.output);

            // The debug snapshot covers only the last telemetry window, so the line
            // reports the input peak (after master gain) instead of the gain reduction.
            const float inputPeakDb = toDb(samplePeak(c.input)) + c.masterDb;

            juce::String line;
            line << "T2-01 F0 limiter " << c.name
                 << " | in peak=" << juce::String(inputPeakDb, 2) << " dBFS"
                 << " | studio peak=" << juce::String(toDb(studioPeak), 2) << " dBFS tp=" << juce::String(toDb(studioTp), 2)
                 << " dBTP ceilingHits=" << studio.snapshot.softCeilingTouchedSamples
                 << " | live peak=" << juce::String(toDb(livePeak), 2) << " dBFS tp=" << juce::String(toDb(liveTp), 2)
                 << " dBTP ceilingHits=" << live.snapshot.softCeilingTouchedSamples;

            // C2: Live computes the gain from the same sample it attenuates, so the
            // sample ceiling holds by construction and the soft ceiling stays untouched.
            expect(livePeak <= kLimiterCeiling * 1.0001f,
                c.name + ": Live sample peak must stay at or below 0.97; peak=" + juce::String(livePeak, 6));
            expectEquals(live.snapshot.softCeilingTouchedSamples, 0,
                c.name + ": Live must not reach the soft ceiling");

            if (c.sustained)
            {
                const float studioResidual = sustainedResidualDb(false, c, studio);
                const float liveResidual = sustainedResidualDb(true, c, live);
                line << " | residual studio=" << juce::String(studioResidual, 1)
                     << " dB live=" << juce::String(liveResidual, 1) << " dB";

                // C4: a limiter (gain), not a clipper. Single sine of the investigation.
                if (c.name == "p12d_master_+10.73dB")
                    expect(liveResidual <= -60.0f,
                        c.name + ": Live residual must stay <= -60 dB; residual=" + juce::String(liveResidual, 2));

                // Every other sustained case: Live adds no distortion over Studio.
                expect(liveResidual <= studioResidual + 1.5f,
                    c.name + ": Live residual must stay within +1.5 dB of Studio; studio="
                        + juce::String(studioResidual, 2) + " live=" + juce::String(liveResidual, 2));
            }

            if (!c.onsets.empty() && c.input.size() > (size_t)kRate / 2)
            {
                const auto studioSplatter = attackSplatter(c, studio);
                const auto liveSplatter = attackSplatter(c, live);
                line << " | splatter abs studio=" << juce::String(studioSplatter.absoluteDb, 2)
                     << " dB live=" << juce::String(liveSplatter.absoluteDb, 2)
                     << " dB | splatter norm studio=" << juce::String(studioSplatter.normalisedDb, 2)
                     << " dB live=" << juce::String(liveSplatter.normalisedDb, 2) << " dB";

                // C5: no click-like treble on the attacks of post-cab material.
                if (c.postCab)
                    expect(liveSplatter.absoluteDb <= 1.5f,
                        c.name + ": Live attack splatter must stay <= input +1.5 dB; splatter="
                            + juce::String(liveSplatter.absoluteDb, 2));
            }

            // C3: post-cab true peak within 0.5 dB of Studio. Without a cabinet both
            // modes go over 0 dBTP (B-13, Catmull-Rom estimator); Live within +1 dB.
            if (c.postCab)
                expect(toDb(liveTp) <= toDb(studioTp) + 0.5f,
                    c.name + ": Live true peak must stay within +0.5 dB of Studio; studio="
                        + juce::String(toDb(studioTp), 2) + " live=" + juce::String(toDb(liveTp), 2));
            else if (!c.onsets.empty())
                expect(toDb(liveTp) <= toDb(studioTp) + 1.0f,
                    c.name + ": no-cab Live true peak must stay within +1 dB of Studio; studio="
                        + juce::String(toDb(studioTp), 2) + " live=" + juce::String(toDb(liveTp), 2));

            // P12D guarantees G1-G4 in both modes.
            if (c.name.startsWith("p12d"))
            {
                for (const auto* run : { &studio, &live })
                {
                    const auto mode = run == &studio ? juce::String("Studio") : juce::String("Live");
                    expect(samplePeak(run->output) < 0.98f, mode + " P12D peak must stay below 0.98");
                    expect(run->snapshot.limiterMaxReductionDb > 0.5f, mode + " P12D limiter must reduce gain");
                    expectEquals(run->snapshot.softCeilingTouchedSamples, 0, mode + " P12D must not touch the soft ceiling");
                }
            }

            logMessage(line);
        }

        beginTest("T2-01 F0 limiter clean path: Live equals Studio minus the lookahead");
        {
            std::vector<int> onsets;
            auto clean = applyButterworth(makePlucks(1.0, onsets), 5500.0f, false);
            normalisePeak(clean, 0.5f);

            const auto studio = runOutputChain(false, clean, 0.0f);
            const auto live = runOutputChain(true, clean, 0.0f);

            float maxDifference = 0.0f;
            for (int n = 0; n < (int)clean.size(); ++n)
                maxDifference = juce::jmax(maxDifference, std::abs(aligned(studio, n) - aligned(live, n)));

            logMessage("T2-01 F0 limiter clean path | max |studio - live| = " + juce::String(maxDifference, 9)
                + " | latency studio=" + juce::String(studio.latency) + " live=" + juce::String(live.latency));
            expect(maxDifference <= 1.0e-7f,
                "Below the threshold Live must be Studio shifted by the lookahead; maxDifference=" + juce::String(maxDifference, 9));
            expectEquals(studio.snapshot.limiterActiveBlocks, 0);
            expectEquals(live.snapshot.limiterActiveBlocks, 0);
        }

        beginTest("T2-01 F0 OutputChain Live latency at 44.1 / 48 / 96 kHz");
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            for (bool liveMode : { false, true })
            {
                auto output = std::make_unique<OutputChainProcessor>();
                output->setLiveMode(liveMode);
                output->setPlayConfigDetails(2, 2, sr, kBlock);
                output->prepareToPlay(sr, kBlock);
                const int expected = liveMode ? 0 : juce::roundToInt(sr * 0.002);
                expectEquals(output->getLatencySamples(), expected,
                    juce::String(liveMode ? "Live" : "Studio") + " OutputChain latency @ " + juce::String(sr));
            }
        }
    }

    // --------------------------------------------------------------- gate suite

    struct GateCase
    {
        juce::String name;
        float noteDbfs = -12.0f;
        bool palmMute = false;
    };

    // DI with a noise floor (hum -66 dBFS + hiss -72 dBFS), 0.6 s of floor so the gate
    // closes, then a plucked note (or 16th-note palm mutes at 120 bpm).
    static Signal makeGateSignal(const GateCase& c, int& onset)
    {
        const int length = (int)(1.2 * kRate);
        onset = (int)(0.6 * kRate);
        const int palmPeriod = (int)(0.125 * kRate);
        juce::Random rng(0x6A7E);
        const float noteAmplitude = juce::Decibels::decibelsToGain(c.noteDbfs);

        Signal x((size_t)length);
        for (int n = 0; n < length; ++n)
        {
            const double t = n / kRate;
            double v = 0.0005 * std::sin(juce::MathConstants<double>::twoPi * 60.0 * t)
                + 0.00025 * (rng.nextFloat() * 2.0f - 1.0f);

            if (n >= onset)
            {
                const int local = c.palmMute ? (n - onset) % palmPeriod : n - onset;
                const double tp = local / kRate;
                const double decay = c.palmMute ? 40.0 : 4.0;
                double note = 0.0;
                for (int k = 1; k <= 12; ++k)
                    note += (1.0 / k) * std::exp(-tp * (decay + 0.7 * k))
                        * std::sin(juce::MathConstants<double>::twoPi * k * 110.0 * tp + 0.7 * k);
                note *= juce::jmin(1.0, tp / 0.0005);
                v += noteAmplitude * 0.62 * note;
            }

            x[(size_t)n] = (float)v;
        }

        return x;
    }

    struct GateRun
    {
        Signal output;
        int latency = 0;
    };

    static GateRun runGate(bool live, const Signal& input, double sr = kRate)
    {
        GateRun run;
        auto gate = std::make_unique<NoiseGatePedal>();
        gate->setLiveMode(live);
        gate->setPlayConfigDetails(2, 2, sr, kBlock);
        gate->prepareToPlay(sr, kBlock);
        run.latency = gate->getLatencySamples();

        const int total = (int)input.size() + run.latency + kBlock;
        run.output.assign((size_t)total, 0.0f);
        juce::AudioBuffer<float> block(2, kBlock);
        juce::MidiBuffer midi;

        for (int start = 0; start < total; start += kBlock)
        {
            const int count = juce::jmin(kBlock, total - start);
            block.setSize(2, count, false, false, true);
            for (int i = 0; i < count; ++i)
            {
                const int n = start + i;
                const float x = n < (int)input.size() ? input[(size_t)n] : 0.0f;
                block.setSample(0, i, x);
                block.setSample(1, i, x);
            }

            gate->processBlock(block, midi);
            for (int i = 0; i < count; ++i)
                run.output[(size_t)(start + i)] = block.getSample(0, i);
        }

        return run;
    }

    static float gateLossDb(const Signal& input, const GateRun& run, int onset, double windowSeconds)
    {
        const int window = (int)(windowSeconds * kRate);
        double in = 0.0, out = 0.0;
        for (int n = onset; n < onset + window; ++n)
        {
            in += (double)input[(size_t)n] * input[(size_t)n];
            const float y = run.output[(size_t)(n + run.latency)];
            out += (double)y * y;
        }
        return energyRatioDb(out, in);
    }

    void runGateSuite()
    {
        beginTest("T2-01 F0 Noise Gate attack: Studio 1.5 ms vs Live 0.25 ms lookahead");

        // The detector listens to a filtered sidechain (the default focus high-passes
        // around 200 Hz), so a note's dBFS does not map to the threshold directly.
        // "Near threshold" is the quietest note that opens the gate in Studio.
        float nearThresholdDbfs = -20.0f;
        for (float level = -46.0f; level <= -20.0f; level += 1.0f)
        {
            int onset = 0;
            const GateCase probe { "probe", level, false };
            const auto input = makeGateSignal(probe, onset);
            if (gateLossDb(input, runGate(false, input), onset, 0.003) >= -0.2f)
            {
                nearThresholdDbfs = level;
                break;
            }
        }

        const GateCase cases[] = {
            { "strong_pick_-12dBFS", -12.0f, false },
            { "soft_fingerpicking_-36dBFS", -36.0f, false },
            { "near_threshold_" + juce::String(nearThresholdDbfs, 0) + "dBFS", nearThresholdDbfs, false },
            { "palm_mute_16ths_-18dBFS", -18.0f, true }
        };

        for (const auto& c : cases)
        {
            const bool nearThreshold = c.noteDbfs == nearThresholdDbfs;
            int onset = 0;
            const auto input = makeGateSignal(c, onset);
            const auto studio = runGate(false, input);
            const auto live = runGate(true, input);

            const float studio1 = gateLossDb(input, studio, onset, 0.001);
            const float studio3 = gateLossDb(input, studio, onset, 0.003);
            const float live1 = gateLossDb(input, live, onset, 0.001);
            const float live3 = gateLossDb(input, live, onset, 0.003);

            logMessage("T2-01 F0 gate " + c.name
                + " | studio (lat " + juce::String(studio.latency) + ") 1ms=" + juce::String(studio1, 2) + " dB 3ms=" + juce::String(studio3, 2)
                + " dB | live (lat " + juce::String(live.latency) + ") 1ms=" + juce::String(live1, 2) + " dB 3ms=" + juce::String(live3, 2) + " dB");

            expect(studio1 >= -0.2f, c.name + ": Studio lookahead must hide the gate opening; loss=" + juce::String(studio1, 2));

            // C6: <= 1 dB on notes >= 10 dB over the threshold, <= 2.5 dB near it.
            const float allowed = nearThreshold ? -2.5f : -1.0f;
            expect(live1 >= allowed, c.name + ": Live first-ms loss " + juce::String(live1, 2)
                + " dB exceeds " + juce::String(allowed, 1) + " dB");
        }

        beginTest("T2-01 F0 Noise Gate Live latency at 44.1 / 48 / 96 kHz");
        for (const auto& [sr, studioExpected, liveExpected] : { std::tuple<double, int, int>{ 44100.0, 66, 11 },
                                                                std::tuple<double, int, int>{ 48000.0, 72, 12 },
                                                                std::tuple<double, int, int>{ 96000.0, 144, 24 } })
        {
            for (bool liveMode : { false, true })
            {
                auto gate = std::make_unique<NoiseGatePedal>();
                gate->setLiveMode(liveMode);
                gate->setPlayConfigDetails(2, 2, sr, kBlock);
                gate->prepareToPlay(sr, kBlock);
                expectEquals(gate->getLatencySamples(), liveMode ? liveExpected : studioExpected,
                    juce::String(liveMode ? "Live" : "Studio") + " gate latency @ " + juce::String(sr));
            }
        }
    }

    // -------------------------------------------------------- latency breakdown

    void runLatencyBreakdown()
    {
        beginTest("T2-01 F0 latency breakdown of the typical chain (Studio)");

        // Gate 1.5 ms + Overdrive 4 + High Gain Amp 4 + Cabinet 0 + Reverb 0 + limiter 2.0 ms.
        for (const auto& [sr, expected] : { std::pair<double, int>{ 44100.0, 162 },
                                            std::pair<double, int>{ 48000.0, 176 },
                                            std::pair<double, int>{ 96000.0, 344 } })
        {
            AudioEngine engine;
            engine.prepare(sr, kBlock, 2, 2);
            engine.addPedal("Noise Gate", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "f0-gate");
            engine.addPedal("Overdrive", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "f0-overdrive");
            engine.addPedal("High Gain Amp", Nova::ChainID::LineA, -1, Nova::ZoneID::Amp, "f0-amp");
            engine.addPedal("Cabinet", Nova::ChainID::LineA, -1, Nova::ZoneID::Cabinet, "f0-cab");
            engine.addPedal("Reverb", Nova::ChainID::LineA, -1, Nova::ZoneID::FX, "f0-reverb");
            engine.synchronizeProcessingState();

            const auto report = engine.buildDiagnosticReport();
            // One log line: the session log keeps only the first line of a message prefixed.
            juce::String breakdown;
            for (const auto& reportLine : juce::StringArray::fromLines(report))
                if (reportLine.contains("latency.breakdown") || reportLine.contains("latencySamples="))
                    breakdown << " || " << reportLine.trim();

            logMessage("T2-01 F0 typical chain @ " + juce::String(sr, 0) + " Hz: graph="
                + juce::String(engine.getLatencyNumSamples()) + " samples ("
                + juce::String(1000.0 * engine.getLatencyNumSamples() / sr, 2) + " ms)" + breakdown);

            expectEquals(engine.getLatencyNumSamples(), expected,
                "Typical chain Studio latency @ " + juce::String(sr));
        }
    }

    // ------------------------------------------------------------ B-11 transition

    struct TransitionStats
    {
        int rawBlocks = 0;              // output equal to the input, bit for bit
        int silentBlocks = 0;
        float steadyPeak = 0.0f;        // processed output outside the transition
        float transitionPeak = 0.0f;
        float steadyMaxDelta = 0.0f;    // largest sample-to-sample step outside the transition
        float transitionMaxDelta = 0.0f;
        bool resumed = false;
    };

    enum class RebuildKind { AddPedal, LatencyBypass, ColdStart };

    // 220 Hz sine at 0.3 with the master at -20 dB, so processed output is easy to tell
    // apart from raw input. The rebuild happens at 0.4 s; the transition window covers
    // the fade-out, the startup blocks and the fade-in, with margin.
    static TransitionStats renderGraphTransition(int blockSize, RebuildKind kind)
    {
        TransitionStats stats;
        AudioEngine engine;
        engine.prepare(kRate, blockSize, 2, 2);

        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        params.outputVolumeDb = -20.0f;
        engine.updateGlobalParams(params);

        if (kind == RebuildKind::LatencyBypass)
            engine.addPedal("Noise Gate", Nova::ChainID::LineA, -1, Nova::ZoneID::Pre, "b11-gate");

        if (kind != RebuildKind::ColdStart)
        {
            engine.setEngineEnabled(true);
            engine.synchronizeProcessingState();
            warmUpEngine(engine, blockSize, 48);
        }

        const int totalBlocks = (int)(1.5 * kRate) / blockSize;
        const int rebuildAt = kind == RebuildKind::ColdStart ? 0 : (int)(0.4 * kRate) / blockSize;
        const int fadeBlocks = (int)std::ceil(0.005 * kRate / blockSize);
        const int transitionEnd = rebuildAt + 4 + Nova::Config::STARTUP_COUNTER_GRAPH_CHANGE + fadeBlocks + 4;

        juce::MidiBuffer midi;
        int globalSample = 0;
        float previous = 0.0f;

        for (int b = 0; b < totalBlocks; ++b)
        {
            if (b == rebuildAt)
            {
                if (kind == RebuildKind::AddPedal)
                    engine.addPedal("Reverb", Nova::ChainID::LineA, -1, Nova::ZoneID::FX, "b11-reverb");
                else if (kind == RebuildKind::LatencyBypass)
                    engine.setPedalBypassed(Nova::ChainID::LineA, 0, true);
                else
                    engine.setEngineEnabled(true);

                engine.synchronizeProcessingState();
            }

            juce::AudioBuffer<float> block(2, blockSize);
            for (int i = 0; i < blockSize; ++i, ++globalSample)
            {
                const float x = 0.3f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * (float)globalSample / (float)kRate);
                block.setSample(0, i, x);
                block.setSample(1, i, x);
            }

            juce::AudioBuffer<float> input;
            input.makeCopyOf(block);
            engine.process(block, midi);

            const bool inTransition = b >= rebuildAt && b < transitionEnd;
            bool raw = true;
            for (int ch = 0; ch < 2 && raw; ++ch)
                for (int i = 0; i < blockSize && raw; ++i)
                    raw = block.getSample(ch, i) == input.getSample(ch, i);

            stats.rawBlocks += raw ? 1 : 0;
            stats.silentBlocks += block.getMagnitude(0, blockSize) == 0.0f ? 1 : 0;

            for (int i = 0; i < blockSize; ++i)
            {
                const float y = block.getSample(0, i);
                const float delta = std::abs(y - previous);
                previous = y;

                if (inTransition)
                {
                    stats.transitionMaxDelta = juce::jmax(stats.transitionMaxDelta, delta);
                    stats.transitionPeak = juce::jmax(stats.transitionPeak, std::abs(y));
                }
                else if (b > 0)
                {
                    // The step reference comes from the old graph only (a pedal added by
                    // the rebuild, like a reverb, can legitimately raise it afterwards).
                    if (b < rebuildAt || kind == RebuildKind::ColdStart)
                        stats.steadyMaxDelta = juce::jmax(stats.steadyMaxDelta, delta);
                    stats.steadyPeak = juce::jmax(stats.steadyPeak, std::abs(y));
                }
            }

            if (b == totalBlocks - 1)
                stats.resumed = block.getMagnitude(0, blockSize) > 0.0f;
        }

        return stats;
    }

    void runB11Transition()
    {
        beginTest("B-11 graph transition: fade-out, silence and fade-in instead of raw input");

        struct Scenario
        {
            const char* name;
            int blockSize;
            RebuildKind kind;
        };

        const Scenario scenarios[] = {
            { "add_pedal_block64", 64, RebuildKind::AddPedal },
            { "add_pedal_block32", 32, RebuildKind::AddPedal },
            { "add_pedal_block512", 512, RebuildKind::AddPedal },
            { "latency_bypass_block64", 64, RebuildKind::LatencyBypass },
            { "cold_start_block64", 64, RebuildKind::ColdStart }
        };

        for (const auto& scenario : scenarios)
        {
            const auto stats = renderGraphTransition(scenario.blockSize, scenario.kind);
            const juce::String name(scenario.name);

            logMessage("B-11 transition " + name
                + " | raw blocks=" + juce::String(stats.rawBlocks)
                + " silent blocks=" + juce::String(stats.silentBlocks)
                + " | peak steady=" + juce::String(toDb(stats.steadyPeak), 1)
                + " dBFS transition=" + juce::String(toDb(stats.transitionPeak), 1)
                + " dBFS | max step steady=" + juce::String(stats.steadyMaxDelta, 6)
                + " transition=" + juce::String(stats.transitionMaxDelta, 6));

            expectEquals(stats.rawBlocks, 0, name + ": no block may pass the raw input");
            expect(stats.silentBlocks >= 1, name + ": the new graph must settle in silence");
            expect(stats.resumed, name + ": processed output must resume after the transition");
            expect(stats.transitionPeak <= stats.steadyPeak * 1.05f,
                name + ": the transition must not jump above the processed level; transition="
                    + juce::String(stats.transitionPeak, 6) + " steady=" + juce::String(stats.steadyPeak, 6));
            // A hard cut of this sine would step up to ~35x the steady maximum.
            expect(stats.transitionMaxDelta <= stats.steadyMaxDelta * 1.5f,
                name + ": the transition must not click; step=" + juce::String(stats.transitionMaxDelta, 6)
                    + " steady=" + juce::String(stats.steadyMaxDelta, 6));
        }
    }
};

// E2 (beta engine checklist, docs/reports/E2-preset-format-freeze.md): the preset
// format is frozen at schema 1. These tests check that every parameter of every
// processor survives a preset file round-trip, that a parameter range change keeps
// the real value, and that the committed v1 fixtures keep loading with the same
// values. Regenerate the fixtures only on purpose: NOVA_WRITE_PRESET_FIXTURES=1.
class E2PresetFormatFreezeTests final : public juce::UnitTest
{
public:
    E2PresetFormatFreezeTests()
        : juce::UnitTest("E2 preset format freeze", "NOVA")
    {
    }

    void runTest() override
    {
        runFullParameterRoundTrip();
        runRangeChangeKeepsRealValue();
        runV1Fixtures();
    }

private:
    struct LoadedSession
    {
        std::unique_ptr<SessionStore> store = std::make_unique<SessionStore>();
        std::unique_ptr<AudioEngine> engine = std::make_unique<AudioEngine>();
    };

    static Nova::ZoneID zoneFor(const Nova::PedalCatalog::Entry& entry, int index)
    {
        if (entry.kind == Nova::PedalCatalog::Kind::Amplifier)
            return Nova::ZoneID::Amp;
        if (entry.kind == Nova::PedalCatalog::Kind::Cabinet)
            return Nova::ZoneID::Cabinet;
        return (index % 2) == 0 ? Nova::ZoneID::Pre : Nova::ZoneID::FX;
    }

    // Deterministic non-default values; choice and bool parameters snap to a step.
    static void applyPattern(juce::AudioProcessor& processor, int seed)
    {
        const auto& parameters = processor.getParameters();
        for (int i = 0; i < parameters.size(); ++i)
        {
            const double position = (double)i * 0.6180339887 + (double)seed * 0.29;
            parameters[i]->setValueNotifyingHost(0.15f + 0.70f * (float)(position - std::floor(position)));
        }
    }

    static juce::String paramKey(juce::AudioProcessorParameter& param, int index)
    {
        if (auto* withID = dynamic_cast<juce::AudioProcessorParameterWithID*>(&param))
            return withID->paramID;
        return "#" + juce::String(index);
    }

    // Real value in parameter units (index for choices); normalized for non-ranged.
    static double realValue(juce::AudioProcessorParameter& param)
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(&param))
            return ranged->convertFrom0to1(ranged->getValue());
        return param.getValue();
    }

    static LoadedSession buildSession(const juce::ValueTree& canonicalState)
    {
        LoadedSession session;
        session.store->applyCommand(SessionStore::Command::makeRestoreState(canonicalState));
        session.engine->prepare(kSampleRate, kBlockSize, 2, 2);
        SessionPersistence::rebuildEngineFromState(*session.engine, session.store->state());
        return session;
    }

    static LoadedSession loadSession(const juce::File& file, bool& loaded)
    {
        LoadedSession session;
        session.engine->prepare(kSampleRate, kBlockSize, 2, 2);
        loaded = SessionPersistence::loadPresetFromFile(file, *session.store, *session.engine);
        return session;
    }

    static juce::File tempPreset(const juce::String& name)
    {
        return juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("nova-e2-" + name + ".nova-preset");
    }

    static juce::String fileNameFor(const juce::String& type)
    {
        return type.replaceCharacter(' ', '-') + ".nova-preset";
    }

    // ------------------------------------------------- full parameter round-trip

    void runFullParameterRoundTrip()
    {
        beginTest("E2 preset file round-trip keeps every parameter of every processor");
        StartupPresetPointerGuard startupPointerGuard;
        int index = 0;

        for (const auto& entry : Nova::PedalCatalog::entries())
        {
            const juce::String type(entry.typeID);
            auto state = makeP7DState();
            appendP7DPedal(state, Nova::ChainID::LineA, type, zoneFor(entry, index), "e2-" + juce::String(index), true);
            Nova::PluginStateModel::canonicalizeStateTree(state);

            auto source = buildSession(state);
            auto* processor = source.engine->getProcessorForPedal(Nova::ChainID::LineA, 0);
            expect(processor != nullptr, type + ": source processor must exist");
            if (processor == nullptr)
                continue;

            applyPattern(*processor, index);

            const auto file = tempPreset(juce::String(index));
            file.deleteFile();
            expect(SessionPersistence::savePresetToFile(file, *source.store, *source.engine), type + ": save must succeed");

            bool loaded = false;
            auto restored = loadSession(file, loaded);
            expect(loaded, type + ": load must succeed");
            auto* restoredProcessor = restored.engine->getProcessorForPedal(Nova::ChainID::LineA, 0);
            expect(restoredProcessor != nullptr, type + ": restored processor must exist");

            int mismatches = 0;
            if (restoredProcessor != nullptr)
            {
                const auto& before = processor->getParameters();
                const auto& after = restoredProcessor->getParameters();
                expectEquals(after.size(), before.size(), type + ": parameter count");

                for (int i = 0; i < juce::jmin(before.size(), after.size()); ++i)
                {
                    // Switches and selectors compare their effective step (a bool set to
                    // 0.58 is "on" and may be stored as 1.0); continuous ones compare the
                    // exact normalized value.
                    const bool stepped = before[i]->isDiscrete() || before[i]->isBoolean();
                    const float expected = stepped ? (float)realValue(*before[i]) : before[i]->getValue();
                    const float actual = stepped ? (float)realValue(*after[i]) : after[i]->getValue();
                    if (std::abs(expected - actual) > 1.0e-6f)
                    {
                        ++mismatches;
                        expect(false, type + ": parameter " + paramKey(*before[i], i) + " expected "
                            + juce::String(expected, 6) + " got " + juce::String(actual, 6));
                    }
                }

                logMessage("E2 round-trip " + type + ": " + juce::String(before.size()) + " parameters, "
                    + juce::String(mismatches) + " mismatches");
            }

            file.deleteFile();
            ++index;
        }
    }

    // ------------------------------------------------------- range change safety

    void runRangeChangeKeepsRealValue()
    {
        beginTest("E2 a parameter range change keeps the stored real value");

        // Compressor uses the generic ProcessorBase serializer.
        auto source = PedalRegistry::createPedal("Compressor");
        expect(source != nullptr, "Compressor must exist");
        if (source == nullptr)
            return;

        applyPattern(*source, 3);
        juce::MemoryBlock block;
        source->getStateInformation(block);
        auto xml = juce::AudioProcessor::getXmlFromBinary(block.getData(), (int)block.getSize());
        expect(xml != nullptr && xml->hasTagName("PLUGIN_STATE"), "Compressor state must be PLUGIN_STATE");
        if (xml == nullptr)
            return;

        // Pick the first float parameter with a real range.
        juce::RangedAudioParameter* target = nullptr;
        for (auto* param : source->getParameters())
            if (auto* f = dynamic_cast<juce::AudioParameterFloat*>(param); f != nullptr && f->range.getRange().getLength() > 1.0f)
            {
                target = f;
                break;
            }

        expect(target != nullptr, "Compressor must have a float parameter with a real range");
        if (target == nullptr)
            return;

        const double originalReal = realValue(*target);
        int rawAttributes = 0;
        juce::XmlElement* element = nullptr;
        for (auto* child : xml->getChildIterator())
        {
            rawAttributes += child->hasAttribute("raw") ? 1 : 0;
            if (child->getStringAttribute("id") == target->paramID)
                element = child;
        }

        expectEquals(rawAttributes, source->getParameters().size(), "Every ranged parameter must store its real value");
        expect(element != nullptr, "Target parameter must be in the state");
        if (element == nullptr)
            return;

        // Simulate a range change: the stored normalized value no longer maps to the
        // stored real value. The real value must win.
        const float storedNormalised = (float)element->getDoubleAttribute("value");
        element->setAttribute("value", storedNormalised < 0.5f ? storedNormalised + 0.3f : storedNormalised - 0.3f);
        juce::MemoryBlock changed;
        juce::AudioProcessor::copyXmlToBinary(*xml, changed);

        auto restored = PedalRegistry::createPedal("Compressor");
        restored->setStateInformation(changed.getData(), (int)changed.getSize());
        double restoredReal = 0.0;
        for (auto* param : restored->getParameters())
            if (auto* f = dynamic_cast<juce::RangedAudioParameter*>(param); f != nullptr && f->paramID == target->paramID)
                restoredReal = realValue(*f);

        expect(std::abs(restoredReal - originalReal) <= 1.0e-3 * juce::jmax(1.0, std::abs(originalReal)),
            "Range change: real value must be kept; original=" + juce::String(originalReal, 4)
                + " restored=" + juce::String(restoredReal, 4));

        // States written before the real values existed still load by normalized value.
        for (auto* child : xml->getChildIterator())
            child->removeAttribute("raw");
        element->setAttribute("value", storedNormalised);
        juce::MemoryBlock legacy;
        juce::AudioProcessor::copyXmlToBinary(*xml, legacy);

        auto legacyRestored = PedalRegistry::createPedal("Compressor");
        legacyRestored->setStateInformation(legacy.getData(), (int)legacy.getSize());
        float legacyNormalised = -1.0f;
        for (auto* param : legacyRestored->getParameters())
            if (auto* f = dynamic_cast<juce::RangedAudioParameter*>(param); f != nullptr && f->paramID == target->paramID)
                legacyNormalised = f->getValue();

        expect(std::abs(legacyNormalised - storedNormalised) <= 1.0e-6f,
            "Legacy state without real values must restore the normalized value");
    }

    // --------------------------------------------------------------- v1 fixtures

    static juce::File findRepoRoot()
    {
        for (auto dir : { juce::File::getCurrentWorkingDirectory(),
                          juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory() })
        {
            for (int i = 0; i < 12; ++i)
            {
                if (dir.getChildFile("NOVA.jucer").existsAsFile())
                    return dir;
                const auto parent = dir.getParentDirectory();
                if (parent == dir)
                    break;
                dir = parent;
            }
        }
        return {};
    }

    // Everything a preset restores, in real units: settings, mixer and, per pedal,
    // type, zone, ID, enabled state and parameter real values.
    static juce::var describeSession(const LoadedSession& session)
    {
        auto* root = new juce::DynamicObject();
        const auto& state = session.store->state();

        auto describeProperties = [](const juce::ValueTree& tree)
        {
            auto* props = new juce::DynamicObject();
            for (int i = 0; i < tree.getNumProperties(); ++i)
            {
                const auto name = tree.getPropertyName(i);
                const auto value = tree.getProperty(name);
                if (Nova::PluginStateModel::isNumericScalar(value))
                    props->setProperty(name, (double)value);
            }
            return juce::var(props);
        };

        root->setProperty("settings", describeProperties(Nova::PluginStateModel::getSettingsTree(state)));

        juce::Array<juce::var> pedals;
        for (auto chain : { Nova::ChainID::LineA, Nova::ChainID::LineB })
        {
            const auto line = Nova::PluginStateModel::getLineTree(state, chain);
            root->setProperty(chain == Nova::ChainID::LineA ? "mixerA" : "mixerB", describeProperties(line));

            for (int i = 0; i < line.getNumChildren(); ++i)
            {
                const auto child = line.getChild(i);
                auto* pedal = new juce::DynamicObject();
                pedal->setProperty("line", chain == Nova::ChainID::LineA ? "A" : "B");
                pedal->setProperty("index", i);
                pedal->setProperty("type", child.getProperty(Nova::IDs::PEDAL_TYPE).toString());
                pedal->setProperty("zone", (int)child.getProperty(Nova::IDs::PEDAL_ZONE));
                pedal->setProperty("id", child.getProperty(Nova::IDs::PEDAL_ID).toString());
                pedal->setProperty("enabled", (bool)child.getProperty(Nova::IDs::PEDAL_ENABLED));

                auto* params = new juce::DynamicObject();
                if (auto* processor = session.engine->getProcessorForPedal(chain, i))
                {
                    const auto& parameters = processor->getParameters();
                    for (int p = 0; p < parameters.size(); ++p)
                        params->setProperty(paramKey(*parameters[p], p), realValue(*parameters[p]));
                }

                pedal->setProperty("params", juce::var(params));
                pedals.add(juce::var(pedal));
            }
        }

        root->setProperty("pedals", pedals);
        return juce::var(root);
    }

    void compareDescriptions(const juce::String& name, const juce::var& expected, const juce::var& actual)
    {
        int checks = 0;
        int mismatches = 0;
        const auto near = [](double a, double b) { return std::abs(a - b) <= 1.0e-3 * juce::jmax(1.0, std::abs(a)); };

        for (const auto* section : { "settings", "mixerA", "mixerB" })
        {
            const auto* want = expected[section].getDynamicObject();
            const auto* got = actual[section].getDynamicObject();
            if (want == nullptr || got == nullptr)
                continue;

            for (const auto& prop : want->getProperties())
            {
                ++checks;
                if (!got->hasProperty(prop.name) || !near((double)prop.value, (double)got->getProperty(prop.name)))
                {
                    ++mismatches;
                    expect(false, name + ": " + section + "." + prop.name.toString() + " expected "
                        + prop.value.toString() + " got " + got->getProperty(prop.name).toString());
                }
            }
        }

        const auto* wantPedals = expected["pedals"].getArray();
        const auto* gotPedals = actual["pedals"].getArray();
        expect(wantPedals != nullptr && gotPedals != nullptr && wantPedals->size() == gotPedals->size(),
            name + ": pedal count must match");

        if (wantPedals != nullptr && gotPedals != nullptr)
        {
            for (int i = 0; i < juce::jmin(wantPedals->size(), gotPedals->size()); ++i)
            {
                const auto& want = wantPedals->getReference(i);
                const auto& got = gotPedals->getReference(i);
                for (const auto* key : { "line", "index", "type", "zone", "id", "enabled" })
                {
                    ++checks;
                    if (want[key].toString() != got[key].toString())
                    {
                        ++mismatches;
                        expect(false, name + ": pedal " + juce::String(i) + " " + key + " expected "
                            + want[key].toString() + " got " + got[key].toString());
                    }
                }

                const auto* wantParams = want["params"].getDynamicObject();
                const auto* gotParams = got["params"].getDynamicObject();
                if (wantParams == nullptr || gotParams == nullptr)
                    continue;

                // Parameters added after the fixture was written are not checked
                // (they load at their defaults); stored ones must keep their value.
                for (const auto& param : wantParams->getProperties())
                {
                    ++checks;
                    if (!gotParams->hasProperty(param.name)
                        || !near((double)param.value, (double)gotParams->getProperty(param.name)))
                    {
                        ++mismatches;
                        expect(false, name + ": " + want["type"].toString() + "." + param.name.toString()
                            + " expected " + param.value.toString() + " got "
                            + gotParams->getProperty(param.name).toString());
                    }
                }
            }
        }

        logMessage("E2 fixture " + name + ": " + juce::String(checks) + " checks, " + juce::String(mismatches) + " mismatches");
    }

    struct FixtureSpec
    {
        juce::String fileName;
        juce::ValueTree state;
        std::vector<int> seeds; // per pedal, LineA then LineB
    };

    static std::vector<FixtureSpec> buildFixtureSpecs()
    {
        std::vector<FixtureSpec> specs;
        int index = 0;

        for (const auto& entry : Nova::PedalCatalog::entries())
        {
            FixtureSpec spec;
            spec.fileName = fileNameFor(entry.typeID);
            spec.state = makeP7DState();
            appendP7DPedal(spec.state, Nova::ChainID::LineA, entry.typeID, zoneFor(entry, index),
                "e2-v1-" + juce::String(index), (index % 3) != 0);
            Nova::PluginStateModel::canonicalizeStateTree(spec.state);
            spec.seeds = { index };
            specs.push_back(spec);
            ++index;
        }

        // A full rig: both lines, every zone, dual routing and non-default globals.
        FixtureSpec rig;
        rig.fileName = "Full-Rig.nova-preset";
        rig.state = makeP7DState();
        auto settings = Nova::PluginStateModel::getSettingsTree(rig.state);
        settings.setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
        settings.setProperty(Nova::IDs::SWITCH_MODE, (int)Nova::SwitcherMode::Dual_Parallel, nullptr);
        settings.setProperty(Nova::IDs::INPUT_GAIN, 4.5f, nullptr);
        settings.setProperty(Nova::IDs::INPUT_GATE, -70.0f, nullptr);
        settings.setProperty(Nova::IDs::FORCE_MONO, true, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_VOL, -2.0f, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_LIMITER, -3.0f, nullptr);
        settings.setProperty(Nova::IDs::OUTPUT_MIX, 80.0f, nullptr);
        auto lineA = Nova::PluginStateModel::getLineTree(rig.state, Nova::ChainID::LineA);
        auto lineB = Nova::PluginStateModel::getLineTree(rig.state, Nova::ChainID::LineB);
        lineA.setProperty(Nova::IDs::MIXER_GAIN_A, 0.8f, nullptr);
        lineA.setProperty(Nova::IDs::MIXER_PAN_A, -0.3f, nullptr);
        lineA.setProperty(Nova::IDs::MIXER_WIDTH_A, 1.2f, nullptr);
        lineB.setProperty(Nova::IDs::MIXER_GAIN_B, 1.1f, nullptr);
        lineB.setProperty(Nova::IDs::MIXER_PAN_B, 0.4f, nullptr);
        lineB.setProperty(Nova::IDs::MIXER_WIDTH_B, 0.9f, nullptr);
        appendP7DPedal(rig.state, Nova::ChainID::LineA, "Noise Gate", Nova::ZoneID::Pre, "rig-a-1", true);
        appendP7DPedal(rig.state, Nova::ChainID::LineA, "Overdrive", Nova::ZoneID::Pre, "rig-a-2", true);
        appendP7DPedal(rig.state, Nova::ChainID::LineA, "High Gain Amp", Nova::ZoneID::Amp, "rig-a-3", true);
        appendP7DPedal(rig.state, Nova::ChainID::LineA, "Delay", Nova::ZoneID::FX, "rig-a-4", false);
        appendP7DPedal(rig.state, Nova::ChainID::LineA, "Modern 4x12", Nova::ZoneID::Cabinet, "rig-a-5", true);
        appendP7DPedal(rig.state, Nova::ChainID::LineB, "Compressor", Nova::ZoneID::Pre, "rig-b-1", true);
        appendP7DPedal(rig.state, Nova::ChainID::LineB, "Clean Amp", Nova::ZoneID::Amp, "rig-b-2", true);
        appendP7DPedal(rig.state, Nova::ChainID::LineB, "Chorus", Nova::ZoneID::FX, "rig-b-3", true);
        appendP7DPedal(rig.state, Nova::ChainID::LineB, "Reverb", Nova::ZoneID::FX, "rig-b-4", true);
        appendP7DPedal(rig.state, Nova::ChainID::LineB, "Vintage 2x12", Nova::ZoneID::Cabinet, "rig-b-5", false);
        Nova::PluginStateModel::canonicalizeStateTree(rig.state);
        rig.seeds = { 101, 102, 103, 104, 105, 106, 107, 108, 109, 110 };
        specs.push_back(rig);

        return specs;
    }

    void writeFixtures(const juce::File& dir)
    {
        dir.createDirectory();
        juce::Array<juce::var> entries;

        for (const auto& spec : buildFixtureSpecs())
        {
            auto session = buildSession(spec.state);
            size_t seedIndex = 0;
            for (auto chain : { Nova::ChainID::LineA, Nova::ChainID::LineB })
            {
                const auto line = Nova::PluginStateModel::getLineTree(session.store->state(), chain);
                for (int i = 0; i < line.getNumChildren() && seedIndex < spec.seeds.size(); ++i, ++seedIndex)
                    if (auto* processor = session.engine->getProcessorForPedal(chain, i))
                        applyPattern(*processor, spec.seeds[seedIndex]);
            }

            const auto file = dir.getChildFile(spec.fileName);
            expect(SessionPersistence::savePresetToFile(file, *session.store, *session.engine),
                "Fixture save must succeed: " + spec.fileName);

            auto* entry = new juce::DynamicObject();
            entry->setProperty("file", spec.fileName);
            entry->setProperty("expected", describeSession(session));
            entries.add(juce::var(entry));
        }

        auto* root = new juce::DynamicObject();
        root->setProperty("schemaVersion", Nova::Config::STATE_SCHEMA_VERSION);
        root->setProperty("note", "Presets written by the beta build (schema 1). Future NOVA versions must keep "
                                  "loading them with these real parameter values. Do not regenerate to make a test pass.");
        root->setProperty("presets", entries);
        dir.getChildFile("expected.json").replaceWithText(juce::JSON::toString(juce::var(root)));
        logMessage("E2 fixtures written to " + dir.getFullPathName() + " (" + juce::String(entries.size()) + " presets)");
    }

    void runV1Fixtures()
    {
        beginTest("E2 v1 preset fixtures keep loading with the same values");
        StartupPresetPointerGuard startupPointerGuard;

        const auto root = findRepoRoot();
        expect(root.exists(), "Repository root (NOVA.jucer) must be found to read the fixtures");
        if (!root.exists())
            return;

        const auto dir = root.getChildFile("docs/golden-metrics/presets-v1");
        if (juce::SystemStats::getEnvironmentVariable("NOVA_WRITE_PRESET_FIXTURES", {}) == "1")
            writeFixtures(dir);

        const auto expectedFile = dir.getChildFile("expected.json");
        expect(expectedFile.existsAsFile(), "v1 fixtures must exist: " + expectedFile.getFullPathName());
        if (!expectedFile.existsAsFile())
            return;

        const auto parsed = juce::JSON::parse(expectedFile.loadFileAsString());
        const auto* presets = parsed["presets"].getArray();
        expect(presets != nullptr && presets->size() > 0, "expected.json must list the fixture presets");
        if (presets == nullptr)
            return;

        for (const auto& entry : *presets)
        {
            const auto name = entry["file"].toString();
            bool loaded = false;
            auto session = loadSession(dir.getChildFile(name), loaded);
            expect(loaded, name + " must load");
            if (loaded)
                compareDescriptions(name, entry["expected"], describeSession(session));
        }
    }
};

// E3 (beta engine checklist): device robustness. The Standalone re-prepares the
// processor whenever the device, sample rate or buffer size changes (also on Latency
// Autopilot Apply/Undo/calibration and when an unplugged device falls back to
// another one). Pedal settings must survive that, and every rate/buffer combination
// and host block size must process clean audio.
class E3DeviceRobustnessTests final : public juce::UnitTest
{
public:
    E3DeviceRobustnessTests()
        : juce::UnitTest("E3 device robustness", "NOVA")
    {
    }

    void runTest() override
    {
        runReprepareKeepsSettings();
        runRateAndBufferMatrix();
        runIrregularHostBlocks();
        runProcessorReleaseAndReprepare();
    }

private:
    struct PedalSpec
    {
        const char* type;
        Nova::ZoneID zone;
    };

    static constexpr PedalSpec kTypicalChain[] = {
        { "Noise Gate", Nova::ZoneID::Pre },
        { "Overdrive", Nova::ZoneID::Pre },
        { "High Gain Amp", Nova::ZoneID::Amp },
        { "Delay", Nova::ZoneID::FX },
        { "Cabinet", Nova::ZoneID::Cabinet }
    };

    static void applyPattern(juce::AudioProcessor& processor, int seed)
    {
        const auto& parameters = processor.getParameters();
        for (int i = 0; i < parameters.size(); ++i)
        {
            const double position = (double)i * 0.6180339887 + (double)seed * 0.29;
            parameters[i]->setValueNotifyingHost(0.15f + 0.70f * (float)(position - std::floor(position)));
        }
    }

    // Effective value: stepped parameters by their step, continuous by normalized value.
    static std::vector<float> captureParameters(juce::AudioProcessor& processor)
    {
        std::vector<float> values;
        for (auto* param : processor.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param);
            const bool stepped = param->isDiscrete() || param->isBoolean();
            values.push_back(stepped && ranged != nullptr ? ranged->convertFrom0to1(param->getValue()) : param->getValue());
        }
        return values;
    }

    static void fillGuitar(juce::AudioBuffer<float>& buffer, int64_t& sampleCounter, double sr, float level)
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i, ++sampleCounter)
        {
            const double t = (double)sampleCounter / sr;
            const double tp = std::fmod(t, 0.25);
            const double env = std::exp(-tp * 6.0) * juce::jmin(1.0, tp / 0.002);
            const float x = (float)(level * env * (0.7 * std::sin(juce::MathConstants<double>::twoPi * 110.0 * t)
                + 0.3 * std::sin(juce::MathConstants<double>::twoPi * 330.0 * t)));
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample(ch, i, x);
        }
    }

    struct RenderStats
    {
        bool finite = true;
        float peak = 0.0f;
        int blocks = 0;
    };

    static RenderStats render(AudioEngine& engine, double sr, const std::vector<int>& blockSizes, int repeats, float level)
    {
        RenderStats stats;
        juce::MidiBuffer midi;
        int64_t counter = 0;

        for (int r = 0; r < repeats; ++r)
        {
            for (int size : blockSizes)
            {
                juce::AudioBuffer<float> block(2, size);
                fillGuitar(block, counter, sr, level);
                engine.process(block, midi);
                stats.finite = stats.finite && bufferHasOnlyFiniteSamples(block);
                if (size > 0)
                    stats.peak = juce::jmax(stats.peak, block.getMagnitude(0, size));
                ++stats.blocks;
            }
        }

        return stats;
    }

    static int expectedTypicalLatency(double sr)
    {
        const int gate = juce::jlimit(16, 192, juce::roundToInt(sr * 0.0015));
        const int limiter = juce::jmax(1, juce::roundToInt(sr * 0.002));
        return gate + 4 + 4 + limiter; // Overdrive and High Gain Amp oversampling: 4 each
    }

    static void addTypicalChain(AudioEngine& engine)
    {
        int i = 0;
        for (const auto& spec : kTypicalChain)
            engine.addPedal(spec.type, Nova::ChainID::LineA, -1, spec.zone, "e3-" + juce::String(i++));
        engine.synchronizeProcessingState();
    }

    static void enable(AudioEngine& engine)
    {
        AudioEngine::RuntimeGlobalParams params;
        params.switchMode = (int)Nova::SwitcherMode::LineA_Only;
        params.outputMixRaw = 100.0f;
        engine.updateGlobalParams(params);
        engine.setEngineEnabled(true);
        engine.synchronizeProcessingState();
    }

    // ------------------------------------------------------------------ tests

    void runReprepareKeepsSettings()
    {
        beginTest("E3 re-preparing the engine keeps pedal settings, bypass and topology");

        AudioEngine engine;
        engine.prepare(48000.0, 64, 2, 2);
        addTypicalChain(engine);
        enable(engine);

        const int numPedals = (int)std::size(kTypicalChain);
        std::vector<std::vector<float>> expected;
        for (int i = 0; i < numPedals; ++i)
        {
            auto* processor = engine.getProcessorForPedal(Nova::ChainID::LineA, i);
            expect(processor != nullptr, "Pedal " + juce::String(i) + " must exist");
            if (processor == nullptr)
                return;
            applyPattern(*processor, 10 + i);
            expected.push_back(captureParameters(*processor));
        }

        engine.setPedalBypassed(Nova::ChainID::LineA, 3, true); // Delay
        engine.synchronizeProcessingState();

        const std::pair<double, int> devices[] = {
            { 96000.0, 512 }, { 44100.0, 32 }, { 48000.0, 1024 }, { 88200.0, 256 }, { 192000.0, 128 }, { 48000.0, 64 }
        };

        for (const auto& [sr, block] : devices)
        {
            const juce::String context = juce::String(sr, 0) + " Hz / " + juce::String(block);
            engine.prepare(sr, block, 2, 2);
            engine.synchronizeProcessingState();

            expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), numPedals, context + ": topology");

            int mismatches = 0;
            for (int i = 0; i < numPedals; ++i)
            {
                auto* processor = engine.getProcessorForPedal(Nova::ChainID::LineA, i);
                if (processor == nullptr)
                {
                    ++mismatches;
                    continue;
                }

                const auto actual = captureParameters(*processor);
                for (size_t p = 0; p < juce::jmin(actual.size(), expected[(size_t)i].size()); ++p)
                    if (std::abs(actual[p] - expected[(size_t)i][p]) > 1.0e-6f)
                        ++mismatches;
            }

            auto* delay = dynamic_cast<ProcessorBase*>(engine.getProcessorForPedal(Nova::ChainID::LineA, 3));
            const bool delayBypassed = delay != nullptr && delay->getBypassed();
            const auto stats = render(engine, sr, { block }, juce::jmax(16, (int)(0.2 * sr) / block), 0.3f);

            logMessage("E3 re-prepare " + context + ": parameter mismatches=" + juce::String(mismatches)
                + " delayBypassed=" + juce::String(delayBypassed ? "yes" : "no")
                + " latency=" + juce::String(engine.getLatencyNumSamples())
                + " peak=" + juce::String(stats.peak, 3));

            expectEquals(mismatches, 0, context + ": pedal settings must survive the re-prepare");
            expect(delayBypassed, context + ": bypass state must survive the re-prepare");
            expect(stats.finite, context + ": output must stay finite");
        }
    }

    void runRateAndBufferMatrix()
    {
        beginTest("E3 every sample rate and buffer size processes clean audio with the typical chain");

        for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        {
            juce::String line;
            line << "E3 matrix " << juce::String(sr, 0) << " Hz:";

            for (int block : { 16, 32, 64, 128, 256, 512, 1024, 2048 })
            {
                AudioEngine engine;
                engine.prepare(sr, block, 2, 2);
                addTypicalChain(engine);
                enable(engine);

                const auto stats = render(engine, sr, { block }, juce::jmax(24, (int)(0.15 * sr) / block), 0.5f);
                const int latency = engine.getLatencyNumSamples();
                const juce::String context = juce::String(sr, 0) + " Hz / " + juce::String(block);

                expect(stats.finite, context + ": output must stay finite");
                expect(stats.peak <= 0.999f, context + ": output must stay under the ceiling; peak=" + juce::String(stats.peak, 4));
                expect(stats.peak > 0.001f, context + ": output must not be silent");
                expectEquals(latency, expectedTypicalLatency(sr), context + ": reported latency");

                line << " " << block << "=" << (stats.finite && stats.peak <= 0.999f && stats.peak > 0.001f ? "ok" : "FAIL")
                     << "(" << latency << ")";
            }

            logMessage(line);
        }
    }

    void runIrregularHostBlocks()
    {
        beginTest("E3 host blocks smaller or larger than prepared stay clean");

        AudioEngine engine;
        engine.prepare(48000.0, 512, 2, 2);
        addTypicalChain(engine);
        enable(engine);

        // Hosts and drivers may deliver fewer samples than prepared, odd sizes, empty
        // blocks, or (breaking the contract) more: the oversized path processes wet only.
        const std::vector<int> sizes = { 512, 1, 7, 100, 333, 0, 512, 64, 513, 1024, 4096, 256, 2, 511 };
        const auto stats = render(engine, 48000.0, sizes, 12, 0.5f);

        logMessage("E3 irregular blocks: " + juce::String(stats.blocks) + " blocks, finite="
            + juce::String(stats.finite ? "yes" : "no") + " peak=" + juce::String(stats.peak, 4));
        expect(stats.finite, "Irregular block sizes must keep the output finite");
        expect(stats.peak <= 0.999f, "Irregular block sizes must stay under the ceiling; peak=" + juce::String(stats.peak, 4));
    }

    void runProcessorReleaseAndReprepare()
    {
        beginTest("E3 unplug and replug (release, re-prepare at another rate) keeps the session");
        StartupPresetPointerGuard startupPointerGuard;

        NOVAAudioProcessor processor;
        processor.setPlayConfigDetails(2, 2, 48000.0, 128);
        processor.prepareToPlay(48000.0, 128);
        processor.requestAddPedal("Overdrive", Nova::ChainID::LineA, Nova::ZoneID::Pre, 0);
        processor.requestAddPedal("Clean Amp", Nova::ChainID::LineA, Nova::ZoneID::Amp, -1);

        auto& engine = processor.getAudioEngine();
        auto* overdrive = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
        expect(overdrive != nullptr, "Overdrive must exist");
        if (overdrive == nullptr)
            return;

        applyPattern(*overdrive, 42);
        const auto expected = captureParameters(*overdrive);

        juce::MidiBuffer midi;
        for (int i = 0; i < 32; ++i)
        {
            juce::AudioBuffer<float> block(2, 128);
            block.clear();
            processor.processBlock(block, midi);
        }

        // Device unplugged: the Standalone stops the callback and releases; the fallback
        // or replugged device reopens at its own rate and buffer size.
        processor.releaseResources();
        processor.setPlayConfigDetails(2, 2, 44100.0, 480);
        processor.prepareToPlay(44100.0, 480);

        auto* restored = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
        expect(restored != nullptr, "Overdrive must exist after the re-prepare");
        if (restored == nullptr)
            return;

        const auto actual = captureParameters(*restored);
        int mismatches = 0;
        for (size_t p = 0; p < juce::jmin(actual.size(), expected.size()); ++p)
            if (std::abs(actual[p] - expected[p]) > 1.0e-6f)
                ++mismatches;

        bool finite = true;
        int64_t counter = 0;
        for (int i = 0; i < 64; ++i)
        {
            juce::AudioBuffer<float> block(2, 480);
            fillGuitar(block, counter, 44100.0, 0.4f);
            processor.processBlock(block, midi);
            finite = finite && bufferHasOnlyFiniteSamples(block);
        }

        logMessage("E3 unplug/replug: parameter mismatches=" + juce::String(mismatches)
            + " host latency=" + juce::String(processor.getLatencySamples()));
        expectEquals((int)engine.getNodes(Nova::ChainID::LineA).size(), 2, "Topology must survive the re-prepare");
        expectEquals(mismatches, 0, "Pedal settings must survive unplug and replug");
        expect(finite, "Output after the re-prepare must stay finite");
        expectEquals(processor.getLatencySamples(), engine.getLatencyNumSamples(), "Host latency must follow the new rate");
    }
};

static AudioEngineValidationTests audioEngineValidationTests;
static P1PedalSafetyTests p1PedalSafetyTests;
static P13ESharedPedalContractTests p13eSharedPedalContractTests;
static P13EPedalContractMigrationTests p13ePedalContractMigrationTests;
static P13DDriveDynamicsSafetyHardeningTests p13dDriveDynamicsSafetyHardeningTests;
#if NOVA_P14B_MEASUREMENT_HARNESS_ENABLED
static P14BDriveDynamicsMeasurementHarnessTests p14bDriveDynamicsMeasurementHarnessTests;
#endif
static P10CHighGainProfessionalizationTests p10cHighGainProfessionalizationTests;
static P10DHighGainArtifactFizzHelicopterTests p10dHighGainArtifactFizzHelicopterTests;
static P10EHighGainMuteHelicopterReverbTests p10eHighGainMuteHelicopterReverbTests;
static P10FHighGainRootCauseFuzzReferenceTests p10fHighGainRootCauseFuzzReferenceTests;
static P10GHighGainNoiseFloorGateCollapseTests p10gHighGainNoiseFloorGateCollapseTests;
static P11AAmpProfessionalizationTests p11aAmpProfessionalizationTests;
static P11BCabinetProfessionalizationTests p11bCabinetProfessionalizationTests;
static P7IDiagnosticsProfilerTests p7iDiagnosticsProfilerTests;
static AudioAutopilotTests audioAutopilotTests;
static T201LiveModeF0Tests t201LiveModeF0Tests;
static E2PresetFormatFreezeTests e2PresetFormatFreezeTests;
static E3DeviceRobustnessTests e3DeviceRobustnessTests;

void touchAudioEngineValidationTests()
{
    juce::ignoreUnused(audioEngineValidationTests,
        p1PedalSafetyTests,
        p13eSharedPedalContractTests,
        p13ePedalContractMigrationTests,
        p13dDriveDynamicsSafetyHardeningTests,
#if NOVA_P14B_MEASUREMENT_HARNESS_ENABLED
        p14bDriveDynamicsMeasurementHarnessTests,
#endif
        p10cHighGainProfessionalizationTests,
        p10dHighGainArtifactFizzHelicopterTests,
        p10eHighGainMuteHelicopterReverbTests,
        p10fHighGainRootCauseFuzzReferenceTests,
        p10gHighGainNoiseFloorGateCollapseTests,
        p11aAmpProfessionalizationTests,
        p11bCabinetProfessionalizationTests,
        p7iDiagnosticsProfilerTests,
        audioAutopilotTests,
        t201LiveModeF0Tests,
        e2PresetFormatFreezeTests,
        e3DeviceRobustnessTests);
}
}

namespace NovaDiagnostics
{
void ensureAudioEngineValidationTestsLinked()
{
    touchAudioEngineValidationTests();
}

#if NOVA_P14B_MEASUREMENT_HARNESS_ENABLED
juce::String buildP14BDriveDynamicsMeasurementReportJson()
{
    return P14BMeasurement::buildReportJson();
}
#endif

#if JUCE_DEBUG || NOVA_ENABLE_RELEASE_DIAGNOSTICS
bool runP9DDraftPresetBuilderFromEnvironment()
{
    return runP9DDraftPresetBuilderTool();
}
#endif
}
