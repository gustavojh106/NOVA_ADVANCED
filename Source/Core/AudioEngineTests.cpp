#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "AudioEngine.h"
#include "Audio/DryWetMixer.h"
#include "Audio/GraphBuilder.h"
#include "Audio/GraphRetirementQueue.h"
#include "Audio/RuntimeGraphManager.h"
#include "Audio/RoutingMixer.h"
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
            expectEquals(output.getLatencySamples(), 0, "Bypassed limiter should not report lookahead latency");
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

            juce::AudioBuffer<float> outputBuffer(2, 4);
            outputBuffer.clear();
            outputBuffer.setSample(0, 0, 1.0f);
            outputBuffer.setSample(1, 0, 1.0f);
            output.processBlock(outputBuffer, midi);

            expect(outputBuffer.getSample(0, 0) < 0.55f && outputBuffer.getSample(0, 0) > 0.45f,
                "Output gain should survive reset");
            expect(outputBuffer.getSample(1, 0) < 0.55f && outputBuffer.getSample(1, 0) > 0.45f,
                "Output gain should survive reset on both channels");
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

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;

            const std::vector<float> left{ 0.25f, -0.5f, 0.75f, -1.0f };
            const std::vector<float> right{ -0.2f, 0.4f, -0.6f, 0.8f };

            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());

            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 3.5e-3f);
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

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;

            const std::vector<float> left{ 0.1f, 0.2f, -0.3f, 0.4f };
            const std::vector<float> right{ -0.4f, 0.3f, -0.2f, 0.1f };

            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());

            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 3.5e-3f);
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
                juce::AudioBuffer<float> output(input.getNumChannels(), input.getNumSamples());
                output.makeCopyOf(input);
                juce::MidiBuffer midi;
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
                engine->synchronizeProcessingState();
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

            juce::AudioBuffer<float> oversized(2, kBlockSize + 32);
            juce::MidiBuffer midi;
            for (int i = 0; i < oversized.getNumSamples(); ++i)
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

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;
            const std::vector<float> left{ 0.22f, -0.11f, 0.33f, -0.44f };
            const std::vector<float> right{ -0.15f, 0.25f, -0.35f, 0.45f };

            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());
            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 3.5e-3f);

            engine.setEngineEnabled(false);
            warmUpEngine(engine, kBlockSize, 4);
            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());
            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 3.5e-3f);

            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 10);
            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());
            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 3.5e-3f);
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

        beginTest("AudioEngine rebuilds graph latency when bypass changes node latency");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "latency-overdrive");
            engine.synchronizeProcessingState();

            const int activeLatency = engine.getLatencyNumSamples();
            expect(activeLatency > 0, "Overdrive should contribute graph latency when active");
            auto* activeProcessor = engine.getProcessorForPedal(Nova::ChainID::LineA, 0);
            expect(activeProcessor != nullptr, "Expected an active Overdrive processor before bypass");

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, true);
            engine.synchronizeProcessingState();
            expectEquals(engine.getLatencyNumSamples(), 0);
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) == activeProcessor,
                "Bypass latency rebuild should update the active graph in place, not publish a replacement graph");

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, false);
            engine.synchronizeProcessingState();
            expectEquals(engine.getLatencyNumSamples(), activeLatency);
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) == activeProcessor,
                "Unbypass latency rebuild should keep the same active processor instance");
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
            const int bypassLatency = engine.getLatencyNumSamples();
            expect(bypassLatency <= baselineLatency,
                "Bypass should not increase latency unexpectedly, baseline="
                    + juce::String(baselineLatency)
                    + ", bypass="
                    + juce::String(bypassLatency));
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) == activeProcessor,
                "Bypass latency rebuild should keep active processor instance");

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, false);
            engine.synchronizeProcessingState();
            expectEquals(engine.getLatencyNumSamples(), baselineLatency);
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) == activeProcessor,
                "Unbypass latency rebuild should keep active processor instance");
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
            const auto limited = runNoPedalScenario(0.0f, -6.0f, 0.42f);

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
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) == activeProcessor,
                "Known ProcessorBase pedal should keep active processor instance across bypass");

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, false);
            engine.synchronizeProcessingState();
            expect(engine.getProcessorForPedal(Nova::ChainID::LineA, 0) == activeProcessor,
                "Known ProcessorBase pedal should keep active processor instance across unbypass");

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
                expectEquals(runtime.appliedParamRevision, request.runtimeParamRevision);

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
            expect(peak < 2.3, "Distortion automation stress should stay inside a sane peak ceiling");
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

        beginTest("SessionPersistence saves and restores a canonical preset");
        {
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

            SessionStore restoredStore;
            AudioEngine restoredEngine;
            restoredEngine.prepare(kSampleRate, kBlockSize, 2, 2);

            expect(SessionPersistence::loadPresetFromFile(presetFile, restoredStore, restoredEngine),
                "Preset load should succeed");

            const auto restoredLineA = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineA);
            const auto restoredLineB = Nova::PluginStateModel::getLineTree(restoredStore.state(), Nova::ChainID::LineB);
            expectEquals(restoredLineA.getNumChildren(), 1);
            expectEquals(restoredLineB.getNumChildren(), 1);
            expectEquals(restoredLineA.getChild(0).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Overdrive"));
            expectEquals(restoredLineB.getChild(0).getProperty(Nova::IDs::PEDAL_TYPE).toString(), juce::String("Chorus"));
            expectEquals((int)restoredEngine.getNodes(Nova::ChainID::LineA).size(), 1);
            expectEquals((int)restoredEngine.getNodes(Nova::ChainID::LineB).size(), 1);

            presetFile.deleteFile();
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
            runP1PreparedMatrix<CompressorPedal>(*this, "Compressor");
            runP1PreparedMatrix<DistortionPedal>(*this, "Distortion");
            runP1PreparedMatrix<FuzzPedal>(*this, "Fuzz");
            runP1PreparedMatrix<NeuralPedal>(*this, "Neural");
            runP1PreparedMatrix<ClassicWahPedal>(*this, "Wah");
            runP1PreparedMatrix<PhaserPedal>(*this, "Phaser");
        }

        beginTest("P1 pedals use dry no-allocation fallback when host exceeds prepared block size");
        {
            runP1OversizedFallback<CabinetPedal>(*this, "Cabinet");
            runP1OversizedFallback<Vintage2x12Cabinet>(*this, "Vintage2x12");
            runP1OversizedFallback<Modern4x12Cabinet>(*this, "Modern4x12");
            runP1OversizedFallback<CompressorPedal>(*this, "Compressor");
            runP1OversizedFallback<DistortionPedal>(*this, "Distortion");
            runP1OversizedFallback<FuzzPedal>(*this, "Fuzz");
            runP1OversizedFallback<NeuralPedal>(*this, "Neural");
            runP1OversizedFallback<ClassicWahPedal>(*this, "Wah");
            runP1OversizedFallback<PhaserPedal>(*this, "Phaser");
        }

        beginTest("P1 pedals keep bypass transitions finite and bounded");
        {
            runP1BypassToggleContinuity<CabinetPedal>(*this, "Cabinet");
            runP1BypassToggleContinuity<Vintage2x12Cabinet>(*this, "Vintage2x12");
            runP1BypassToggleContinuity<Modern4x12Cabinet>(*this, "Modern4x12");
            runP1BypassToggleContinuity<CompressorPedal>(*this, "Compressor");
            runP1BypassToggleContinuity<DistortionPedal>(*this, "Distortion");
            runP1BypassToggleContinuity<FuzzPedal>(*this, "Fuzz");
            runP1BypassToggleContinuity<NeuralPedal>(*this, "Neural");
            runP1BypassToggleContinuity<ClassicWahPedal>(*this, "Wah");
            runP1BypassToggleContinuity<PhaserPedal>(*this, "Phaser");
        }
    }
};

static AudioEngineValidationTests audioEngineValidationTests;
static P1PedalSafetyTests p1PedalSafetyTests;

void touchAudioEngineValidationTests()
{
    juce::ignoreUnused(audioEngineValidationTests, p1PedalSafetyTests);
}
}

namespace NovaDiagnostics
{
void ensureAudioEngineValidationTestsLinked()
{
    touchAudioEngineValidationTests();
}
}
