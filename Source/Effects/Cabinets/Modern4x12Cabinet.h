#pragma once

#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"
#include "SyntheticIR.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

class Modern4x12Cabinet final : public ProcessorBase
{
public:
    Modern4x12Cabinet()
    {
        addParameter(lowEndParam = new juce::AudioParameterFloat("m4x12Low", "Low End", -12.0f, 12.0f, 2.0f));
        addParameter(presenceParam = new juce::AudioParameterFloat("m4x12Presence", "Presence", -12.0f, 12.0f, 2.5f));
        addParameter(distanceParam = new juce::AudioParameterFloat("m4x12Distance", "Distance", 0.0f, 1.0f, 0.22f));
        addParameter(mixParam = new juce::AudioParameterFloat("m4x12Mix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("m4x12Level", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Modern 4x12"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Cabinet",
            "Modern 4x12",
            juce::Colour::fromString("ff6366F1"),
            {
                { "Low End", lowEndParam, [](float value) { return formatDecibels(value); } },
                { "Presence", presenceParam, [](float value) { return formatDecibels(value); } },
                { "Distance", distanceParam, [](float value) { return formatPercent(value); } },
                { "Mix", mixParam, [](float value) { return formatPercent(value); } },
                { "Level", levelParam, [](float value) { return formatGain(value); } }
            },
            214,
            178);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        preparedBlockSize = juce::jmax(1, samplesPerBlock);
        preparedChannels = juce::jlimit(1, kMaxProcessingChannels, juce::jmax(2, getTotalNumOutputChannels()));

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = (juce::uint32)preparedBlockSize;
        spec.numChannels = (juce::uint32)preparedChannels;

        convolution.prepare(spec);
        loadSyntheticIR(sampleRate);
        lowShelf.prepare(spec);
        highShelf.prepare(spec);
        contourLP.prepare(spec);
        contourHP.prepare(spec);
        midScoop.prepare(spec);

        mixSmooth.reset(sampleRate, 0.02);
        levelSmooth.reset(sampleRate, 0.02);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        scratchBuffer.setSize(preparedChannels,
            preparedBlockSize, false, false, true);
        postCabDcBlock.prepare(sampleRate, preparedChannels, 18.0f);

        currentSampleRate = sampleRate;
        cachedLowEnd = std::numeric_limits<float>::quiet_NaN();
        cachedPresence = std::numeric_limits<float>::quiet_NaN();
        cachedDistance = std::numeric_limits<float>::quiet_NaN();

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override
    {
        isPrepared = false;
    }

    void reset() override
    {
        convolution.reset();
        lowShelf.reset();
        highShelf.reset();
        contourLP.reset();
        contourHP.reset();
        midScoop.reset();
        postCabDcBlock.reset();

        if (mixParam != nullptr)
            mixSmooth.setCurrentAndTargetValue(*mixParam);
        if (levelParam != nullptr)
            levelSmooth.setCurrentAndTargetValue(*levelParam);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        scrubInvalidSamples(buffer);

        if (!canProcessBlock(buffer))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            endBypassProcess(buffer);
            return;
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch), buffer.getNumSamples());

        updateVoicing();
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);

        if (convolution.getCurrentIRSize() > 0)
            convolution.process(context);

        lowShelf.process(context);
        highShelf.process(context);
        midScoop.process(context);
        contourLP.process(context);
        contourHP.process(context);
        postCabDcBlock.processBuffer(buffer);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();
            const float dry = 1.0f - mix;
            const float level = levelSmooth.getNextValue();

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float clean = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample) * kProfessionalCabinetTrim;
                buffer.setSample(ch, sample, ((wet * mix) + (clean * dry)) * level);
            }
        }

        endBypassProcess(buffer);
    }

    int getRealtimeFallbackCount() const noexcept
    {
        return fallbackBlockCount.load(std::memory_order_relaxed);
    }

    void clearRealtimeFallbackCount() noexcept
    {
        fallbackBlockCount.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr int kMaxProcessingChannels = 2;
    static constexpr float kProfessionalCabinetTrim = 0.70f;

    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    struct MultiChannelDcBlocker
    {
        void prepare(double newSampleRate, int channels, float cutoffHz) noexcept
        {
            sampleRate = juce::jmax(1.0, newSampleRate);
            numChannels = juce::jlimit(1, kMaxProcessingChannels, channels);
            const float cutoff = juce::jlimit(5.0f, 45.0f, cutoffHz);
            pole = (float) std::exp(-juce::MathConstants<double>::twoPi * (double) cutoff / sampleRate);
            reset();
        }

        void reset() noexcept
        {
            x1.fill(0.0f);
            y1.fill(0.0f);
        }

        void processBuffer(juce::AudioBuffer<float>& buffer) noexcept
        {
            const int channels = juce::jmin(buffer.getNumChannels(), numChannels);
            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = buffer.getWritePointer(ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float x = data[i];
                    const float y = x - x1[(size_t) ch] + pole * y1[(size_t) ch];
                    x1[(size_t) ch] = x;
                    y1[(size_t) ch] = y;
                    data[i] = y;
                }
            }
        }

        double sampleRate = 44100.0;
        int numChannels = 2;
        float pole = 0.995f;
        std::array<float, kMaxProcessingChannels> x1 {};
        std::array<float, kMaxProcessingChannels> y1 {};
    };

    bool canProcessBlock(const juce::AudioBuffer<float>& buffer) const noexcept
    {
        return buffer.getNumSamples() <= preparedBlockSize
            && buffer.getNumChannels() <= preparedChannels
            && buffer.getNumChannels() <= kMaxProcessingChannels;
    }

    static void scrubInvalidSamples(juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (!std::isfinite(data[i]))
                    data[i] = 0.0f;
        }
    }

    void loadSyntheticIR(double sampleRate)
    {
        auto ir = Nova::CabinetIR::generateModern4x12(sampleRate);
        convolution.loadImpulseResponse(std::move(ir), sampleRate, juce::dsp::Convolution::Stereo::no,
            juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    }

    void updateVoicing()
    {
        const float lowEnd = lowEndParam != nullptr ? *lowEndParam : 2.0f;
        const float presence = presenceParam != nullptr ? *presenceParam : 2.5f;
        const float distance = distanceParam != nullptr ? *distanceParam : 0.22f;

        const bool lowChanged = !std::isfinite(cachedLowEnd) || std::abs(cachedLowEnd - lowEnd) > 1.0e-4f;
        const bool presChanged = !std::isfinite(cachedPresence) || std::abs(cachedPresence - presence) > 1.0e-4f;
        const bool distChanged = !std::isfinite(cachedDistance) || std::abs(cachedDistance - distance) > 1.0e-4f;
        if (!lowChanged && !presChanged && !distChanged)
            return;

        cachedLowEnd = lowEnd;
        cachedPresence = presence;
        cachedDistance = distance;

        // Modern 4x12 voicing: tight low end with a controlled post-amp top cut.
        const float lpFreq = juce::jmap(distance, 6200.0f, 3200.0f);
        const float hpFreq = juce::jmap(distance, 65.0f, 150.0f);
        const float compensatedPresence = juce::jlimit(-12.0f, 3.5f, presence * 0.45f - 1.2f);

        *lowShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf(currentSampleRate,
            100.0f, 0.85f, juce::Decibels::decibelsToGain(lowEnd));
        *highShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf(currentSampleRate,
            3100.0f, 0.74f, juce::Decibels::decibelsToGain(compensatedPresence));
        // Slight mid-scoop characteristic of modern V30-style cabs
        *midScoop.state = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter(currentSampleRate,
            650.0f, 1.2f, juce::Decibels::decibelsToGain(-2.0f));
        *contourLP.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass(currentSampleRate, lpFreq, 0.72f);
        *contourHP.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(currentSampleRate, hpFreq, 0.72f);
    }

    juce::dsp::Convolution convolution;
    Filter lowShelf;
    Filter highShelf;
    Filter contourLP;
    Filter contourHP;
    Filter midScoop;
    MultiChannelDcBlocker postCabDcBlock;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    juce::AudioParameterFloat* lowEndParam = nullptr;
    juce::AudioParameterFloat* presenceParam = nullptr;
    juce::AudioParameterFloat* distanceParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    float cachedLowEnd = std::numeric_limits<float>::quiet_NaN();
    float cachedPresence = std::numeric_limits<float>::quiet_NaN();
    float cachedDistance = std::numeric_limits<float>::quiet_NaN();
    std::atomic<int> fallbackBlockCount{ 0 };
    bool isPrepared = false;
};
