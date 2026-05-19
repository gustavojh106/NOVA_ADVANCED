#pragma once

#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"
#include "SyntheticIR.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

class CabinetPedal final : public ProcessorBase
{
public:
    CabinetPedal()
    {
        addParameter(thumpParam = new juce::AudioParameterFloat("cabThump", "Thump", -12.0f, 12.0f, 1.5f));
        addParameter(airParam = new juce::AudioParameterFloat("cabAir", "Air", -12.0f, 12.0f, 0.0f));
        addParameter(resonanceParam = new juce::AudioParameterFloat("cabResonance", "Resonance", -6.0f, 6.0f, 0.0f));
        addParameter(lowCutParam = new juce::AudioParameterFloat("cabLowCut", "Low Cut", 45.0f, 180.0f, 55.0f));
        addParameter(highCutParam = new juce::AudioParameterFloat("cabHighCut", "High Cut", 3500.0f, 10000.0f, 8200.0f));
        addParameter(distanceParam = new juce::AudioParameterFloat("cabDistance", "Distance", 0.0f, 1.0f, 0.34f));
        addParameter(mixParam = new juce::AudioParameterFloat("cabMix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("cabLevel", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Cabinet"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumHardwareEditor(*this,
            PremiumHardwareEditor::Skin::Cabinet,
            "Cabinet",
            "Atlas 4x12",
            "Balanced final voicing / IR",
            juce::Colour::fromString("ffA78BFA"),
            {
                { "Thump", thumpParam, [](float value) { return formatDecibels(value); } },
                { "Air", airParam, [](float value) { return formatDecibels(value); } },
                { "Resonance", resonanceParam, [](float value) { return formatDecibels(value); } },
                { "Low Cut", lowCutParam, [](float value) { return formatHertz(value); } },
                { "High Cut", highCutParam, [](float value) { return formatHertz(value); } },
                { "Distance", distanceParam, [](float value) { return formatPercent(value); } },
                { "Mix", mixParam, [](float value) { return formatPercent(value); } },
                { "Level", levelParam, [](float value) { return formatGain(value); } }
            },
            660,
            356);
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
        resonancePeak.prepare(spec);
        highShelf.prepare(spec);
        contourLowPass.prepare(spec);
        contourHighPass.prepare(spec);

        mixSmooth.reset(sampleRate, 0.02);
        levelSmooth.reset(sampleRate, 0.02);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        scratchBuffer.setSize(preparedChannels,
            preparedBlockSize,
            false,
            false,
            true);
        postCabDcBlock.prepare(sampleRate, preparedChannels, 18.0f);

        currentSampleRate = sampleRate;
        cachedThump = std::numeric_limits<float>::quiet_NaN();
        cachedAir = std::numeric_limits<float>::quiet_NaN();
        cachedResonance = std::numeric_limits<float>::quiet_NaN();
        cachedLowCut = std::numeric_limits<float>::quiet_NaN();
        cachedHighCut = std::numeric_limits<float>::quiet_NaN();
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
        resonancePeak.reset();
        highShelf.reset();
        contourLowPass.reset();
        contourHighPass.reset();
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
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), buffer.getNumSamples());

        updateCabinetVoicing();
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);

        if (convolution.getCurrentIRSize() > 0)
            convolution.process(context);

        lowShelf.process(context);
        resonancePeak.process(context);
        highShelf.process(context);
        contourLowPass.process(context);
        contourHighPass.process(context);
        postCabDcBlock.processBuffer(buffer);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();
            const float dry = 1.0f - mix;
            const float level = levelSmooth.getNextValue();

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float clean = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample);
                buffer.setSample(ch, sample, containCabinetOutput(((wet * mix) + (clean * dry)) * level));
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

    static float containCabinetOutput(float x) noexcept
    {
        if (!std::isfinite(x))
            return 0.0f;

        constexpr float knee = 0.86f;
        constexpr float ceiling = 0.97f;
        const float ax = std::abs(x);
        if (ax <= knee)
            return x;

        const float over = ax - knee;
        const float shaped = knee + (ceiling - knee) * std::tanh(over / (ceiling - knee));
        return std::copysign(shaped, x);
    }

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
        auto ir = Nova::CabinetIR::generateAtlas4x12(sampleRate);
        convolution.loadImpulseResponse(std::move(ir), sampleRate, juce::dsp::Convolution::Stereo::no,
            juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    }

    void updateCabinetVoicing()
    {
        const float thump = thumpParam != nullptr ? *thumpParam : 1.5f;
        const float air = airParam != nullptr ? *airParam : 0.0f;
        const float resonance = resonanceParam != nullptr ? *resonanceParam : 0.0f;
        const float lowCut = lowCutParam != nullptr ? *lowCutParam : 55.0f;
        const float highCut = highCutParam != nullptr ? *highCutParam : 8200.0f;
        const float distance = distanceParam != nullptr ? *distanceParam : 0.34f;

        const bool thumpChanged = !std::isfinite(cachedThump) || std::abs(cachedThump - thump) > 1.0e-4f;
        const bool airChanged = !std::isfinite(cachedAir) || std::abs(cachedAir - air) > 1.0e-4f;
        const bool resonanceChanged = !std::isfinite(cachedResonance) || std::abs(cachedResonance - resonance) > 1.0e-4f;
        const bool lowCutChanged = !std::isfinite(cachedLowCut) || std::abs(cachedLowCut - lowCut) > 1.0e-4f;
        const bool highCutChanged = !std::isfinite(cachedHighCut) || std::abs(cachedHighCut - highCut) > 1.0e-4f;
        const bool distanceChanged = !std::isfinite(cachedDistance) || std::abs(cachedDistance - distance) > 1.0e-4f;
        if (!thumpChanged && !airChanged && !resonanceChanged && !lowCutChanged && !highCutChanged && !distanceChanged)
            return;

        cachedThump = thump;
        cachedAir = air;
        cachedResonance = resonance;
        cachedLowCut = lowCut;
        cachedHighCut = highCut;
        cachedDistance = distance;

        const float lowPass = juce::jmin(juce::jmap(distance, 9800.0f, 3600.0f), highCut);
        const float highPass = juce::jmax(juce::jmap(distance, 55.0f, 140.0f), lowCut);

        *lowShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf(currentSampleRate,
            130.0f,
            0.8f,
            juce::Decibels::decibelsToGain(thump));
        *resonancePeak.state = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter(currentSampleRate,
            92.0f,
            1.15f,
            juce::Decibels::decibelsToGain(resonance));
        *highShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf(currentSampleRate,
            3400.0f,
            0.72f,
            juce::Decibels::decibelsToGain(air));
        *contourLowPass.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass(currentSampleRate,
            lowPass,
            0.68f);
        *contourHighPass.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(currentSampleRate,
            highPass,
            0.7f);
    }

    juce::dsp::Convolution convolution;
    Filter lowShelf;
    Filter resonancePeak;
    Filter highShelf;
    Filter contourLowPass;
    Filter contourHighPass;
    MultiChannelDcBlocker postCabDcBlock;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    juce::AudioParameterFloat* thumpParam = nullptr;
    juce::AudioParameterFloat* airParam = nullptr;
    juce::AudioParameterFloat* resonanceParam = nullptr;
    juce::AudioParameterFloat* lowCutParam = nullptr;
    juce::AudioParameterFloat* highCutParam = nullptr;
    juce::AudioParameterFloat* distanceParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentSampleRate = 44100.0;
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    float cachedThump = std::numeric_limits<float>::quiet_NaN();
    float cachedAir = std::numeric_limits<float>::quiet_NaN();
    float cachedResonance = std::numeric_limits<float>::quiet_NaN();
    float cachedLowCut = std::numeric_limits<float>::quiet_NaN();
    float cachedHighCut = std::numeric_limits<float>::quiet_NaN();
    float cachedDistance = std::numeric_limits<float>::quiet_NaN();
    std::atomic<int> fallbackBlockCount{ 0 };
    bool isPrepared = false;
};
