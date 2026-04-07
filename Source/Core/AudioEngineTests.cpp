#include <JuceHeader.h>
#include <cmath>
#include <vector>

#include "AudioEngine.h"
#include "PluginProcessor.h"
#include "PluginStateModel.h"
#include "PedalRegistry.h"
#include "SessionPersistence.h"
#include "SessionStore.h"
#include "DSP/Global/ChannelStrip.h"
#include "DSP/Global/InputChain.h"
#include "DSP/Global/OutputChain.h"
#include "DSP/Services/TunerService.h"

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
        test.expect(approximatelyEqual(buffer.getSample(0, i), expectedLeft[(size_t)i], tolerance),
            "Left channel mismatch at sample " + juce::String(i));
        test.expect(approximatelyEqual(buffer.getSample(1, i), expectedRight[(size_t)i], tolerance),
            "Right channel mismatch at sample " + juce::String(i));
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

class AudioEngineValidationTests final : public juce::UnitTest
{
public:
    AudioEngineValidationTests()
        : juce::UnitTest("Audio Engine Validation", "NOVA")
    {
    }

    void runTest() override
    {
        beginTest("InputChain forceMono sums both channels");
        {
            InputChainProcessor input;
            input.prepareToPlay(kSampleRate, kBlockSize);
            input.setParams(0.0f, -100.0f, true, 0);

            juce::AudioBuffer<float> buffer(2, 4);
            juce::MidiBuffer midi;

            const std::vector<float> left{ 1.0f, -0.5f, 0.25f, -0.25f };
            const std::vector<float> right{ -1.0f, 0.5f, 0.75f, 0.25f };

            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());

            input.processBlock(buffer, midi);

            const std::vector<float> summed{ 0.0f, 0.0f, 0.5f, 0.0f };
            expectStereoSamplesMatch(*this, buffer, summed, summed);
        }

        beginTest("ChannelStrip is unity at default balance");
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
            expectStereoSamplesMatch(*this, buffer, left, right);
        }

        beginTest("ChannelStrip uses a smooth balance curve");
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

            const float expectedLeft = std::cos(juce::MathConstants<float>::pi * 0.25f);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                expect(approximatelyEqual(buffer.getSample(0, i), expectedLeft, 2.0e-4f),
                    "Left channel balance mismatch at sample " + juce::String(i));
                expect(approximatelyEqual(buffer.getSample(1, i), 1.0f, 2.0e-4f),
                    "Right channel should remain unity at sample " + juce::String(i));
            }
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

        beginTest("Global processors preserve active params after reset");
        {
            juce::MidiBuffer midi;

            InputChainProcessor input;
            input.prepareToPlay(kSampleRate, kBlockSize);
            input.setParams(6.0f, -100.0f, false, 0);
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

        beginTest("AudioEngine is transparent in single-line mode");
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
            expectStereoSamplesMatch(*this, buffer, left, right, 2.0e-4f);
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

        beginTest("AudioEngine parallel routing keeps unity on identical clean lines");
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
            expectStereoSamplesMatch(*this, buffer, left, right, 2.0e-4f);
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
            expectStereoSamplesMatch(*this, buffer, left, right, 2.0e-4f);
        }

        beginTest("AudioEngine recovers cleanly across engine disable and re-enable");
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
            expectStereoSamplesMatch(*this, buffer, left, right, 2.0e-4f);

            engine.setEngineEnabled(false);
            warmUpEngine(engine, kBlockSize, 4);
            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());
            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 2.0e-4f);

            engine.setEngineEnabled(true);
            warmUpEngine(engine, kBlockSize, 10);
            buffer.copyFrom(0, 0, left.data(), (int)left.size());
            buffer.copyFrom(1, 0, right.data(), (int)right.size());
            engine.process(buffer, midi);
            expectStereoSamplesMatch(*this, buffer, left, right, 2.0e-4f);
        }

        beginTest("AudioEngine rebuilds graph latency when bypass changes node latency");
        {
            AudioEngine engine;
            engine.prepare(kSampleRate, kBlockSize, 2, 2);
            engine.addPedal("Overdrive", Nova::ChainID::LineA, 0, Nova::ZoneID::Pre, "latency-overdrive");
            engine.synchronizeProcessingState();

            const int activeLatency = engine.getLatencyNumSamples();
            expect(activeLatency > 0, "Overdrive should contribute graph latency when active");

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, true);
            engine.synchronizeProcessingState();
            expectEquals(engine.getLatencyNumSamples(), 0);

            engine.setPedalBypassed(Nova::ChainID::LineA, 0, false);
            engine.synchronizeProcessingState();
            expectEquals(engine.getLatencyNumSamples(), activeLatency);
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
            expect(PedalRegistry::isTypeSupported("Wah"), "Wah should be registered");
            expect(PedalRegistry::isTypeSupported("Octave"), "Octave should be registered");
            expect(PedalRegistry::isTypeSupported("Metal Distortion"), "Metal Distortion should be registered");

            const auto preTypes = PedalRegistry::getPedalTypesForZone(Nova::ZoneID::Pre);
            const auto fxTypes = PedalRegistry::getPedalTypesForZone(Nova::ZoneID::FX);

            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Compressor")) != preTypes.end(),
                "Compressor should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Boost")) != preTypes.end(),
                "Boost should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Wah")) != preTypes.end(),
                "Wah should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Octave")) != preTypes.end(),
                "Octave should be available in the pre zone");
            expect(std::find(preTypes.begin(), preTypes.end(), juce::String("Metal Distortion")) != preTypes.end(),
                "Metal Distortion should be available in the pre zone");
            expect(std::find(fxTypes.begin(), fxTypes.end(), juce::String("Chorus")) != fxTypes.end(),
                "Chorus should be available in the FX zone");
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
            expect(heldRms > captureRms * 0.55, "Freeze should preserve most of the captured reverse pad");
            expect(heldRms > baselineHeld * 2.50, "Freeze should hold longer than the unfrozen reverse tail");
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
            expect(heldRms > captureRms * 0.50, "Freeze should retain a significant part of the captured repeat bed");
            expect(heldRms > baselineHeld * 2.0, "Freeze should outlast the unfrozen tail decisively");
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

            expect(comboEarly < baselineEarly * 0.82, "Reverse+swell should clearly soften the early delay onset");
            expect(comboLate > comboEarly * 0.52, "Reverse+swell should keep a meaningful later ambient body");
            expect(comboLate > baselineLate * 0.70, "Reverse+swell should still retain a commercially usable late body");
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

static AudioEngineValidationTests audioEngineValidationTests;
}
