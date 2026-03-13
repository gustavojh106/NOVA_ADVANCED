#include <JuceHeader.h>
#include <cmath>
#include <vector>

#include "AudioEngine.h"
#include "DSP/Global/ChannelStrip.h"
#include "DSP/Global/InputChain.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr float kTolerance = 1.0e-4f;

bool approximatelyEqual(float actual, float expected, float tolerance = kTolerance)
{
    return std::abs(actual - expected) <= tolerance;
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
    }
};

static AudioEngineValidationTests audioEngineValidationTests;
}
