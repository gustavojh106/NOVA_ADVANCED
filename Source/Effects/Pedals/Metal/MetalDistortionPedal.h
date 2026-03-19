#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class MetalDistortionPedal final : public ProcessorBase
{
public:
    MetalDistortionPedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(gainParam = new juce::AudioParameterFloat("metalGain", "Gain", 0.0f, 100.0f, 64.0f));
        addParameter(lowParam = new juce::AudioParameterFloat("metalLow", "Low", -12.0f, 12.0f, 3.0f));
        addParameter(midParam = new juce::AudioParameterFloat("metalMid", "Mid", -15.0f, 15.0f, -3.5f));
        addParameter(midFreqParam = new juce::AudioParameterFloat("metalMidFreq", "Mid Freq", 250.0f, 5000.0f, 950.0f));
        addParameter(highParam = new juce::AudioParameterFloat("metalHigh", "High", -12.0f, 12.0f, 4.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("metalLevel", "Level", 0.0f, 1.0f, 0.68f));
    }

    const juce::String getName() const override { return "Metal Distortion"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Distortion",
            "Metal Stack",
            juce::Colour::fromString("ffF97316"),
            {
                { "Gain", gainParam, [](float value) { return juce::String(juce::roundToInt(value)) + "%"; } },
                { "Low", lowParam, [](float value) { return formatDecibels(value); } },
                { "Mid", midParam, [](float value) { return formatDecibels(value); } },
                { "Mid Freq", midFreqParam, [](float value) { return formatHertz(value); } },
                { "High", highParam, [](float value) { return formatDecibels(value); } },
                { "Level", levelParam, [](float value) { return formatPercent(value); } }
            },
            228,
            190);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.reset();
        oversampler.initProcessing((size_t)juce::jmax(1, samplesPerBlock));

        currentInnerRate = sampleRate * 4.0;

        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate = currentInnerRate;
        innerSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock * 4);
        innerSpec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        inputHighPass.prepare(innerSpec);
        lowShelf.prepare(innerSpec);
        midPeak.prepare(innerSpec);
        highShelf.prepare(innerSpec);
        outputLowPass.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        gainSmooth.reset(sampleRate, 0.04);
        levelSmooth.reset(sampleRate, 0.04);
        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 64.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.68f);

        cachedLow = std::numeric_limits<float>::quiet_NaN();
        cachedMid = std::numeric_limits<float>::quiet_NaN();
        cachedMidFreq = std::numeric_limits<float>::quiet_NaN();
        cachedHigh = std::numeric_limits<float>::quiet_NaN();
        cachedGain = std::numeric_limits<float>::quiet_NaN();

        setProcessingLatency((int)oversampler.getLatencyInSamples());
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
        oversampler.reset();
        inputHighPass.reset();
        lowShelf.reset();
        midPeak.reset();
        highShelf.reset();
        outputLowPass.reset();
        dcBlock.reset();

        gainSmooth.setCurrentAndTargetValue(gainParam != nullptr ? *gainParam : 64.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 0.68f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateFiltersIfNeeded();
        gainSmooth.setTargetValue(gainParam != nullptr ? *gainParam : 64.0f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 0.68f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);

        {
            juce::dsp::ProcessContextReplacing<float> context(upsampled);
            inputHighPass.process(context);
        }

        {
            const int channels = (int)upsampled.getNumChannels();
            const int samples = (int)upsampled.getNumSamples();

            for (int sample = 0; sample < samples; ++sample)
            {
                const float driveControl = gainSmooth.getNextValue();
                const float drive = juce::Decibels::decibelsToGain(10.0f + driveControl * 0.50f);
                const float secondStage = 1.4f + driveControl * 0.018f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = upsampled.getChannelPointer((size_t)ch);
                    float x = data[sample] * drive;

                    const float first = std::tanh(x * 0.9f);
                    float y = first * secondStage;

                    const float pos = std::tanh(y * 1.55f);
                    const float neg = std::tanh(y * 1.22f);
                    y = y >= 0.0f ? pos : neg;

                    if (std::abs(y) > 0.55f)
                        y += 0.09f * std::sin(y * juce::MathConstants<float>::twoPi);

                    data[sample] = y;
                }
            }
        }

        {
            juce::dsp::ProcessContextReplacing<float> context(upsampled);
            lowShelf.process(context);
            midPeak.process(context);
            highShelf.process(context);
            outputLowPass.process(context);
            dcBlock.process(context);
        }

        oversampler.processSamplesDown(block);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float level = juce::jmap(levelSmooth.getNextValue(), 0.10f, 1.55f);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * level);
        }

        endBypassProcess(buffer);
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void updateFiltersIfNeeded()
    {
        const float gain = gainParam != nullptr ? *gainParam : 64.0f;
        const float low = lowParam != nullptr ? *lowParam : 3.0f;
        const float mid = midParam != nullptr ? *midParam : -3.5f;
        const float midFreq = midFreqParam != nullptr ? *midFreqParam : 950.0f;
        const float high = highParam != nullptr ? *highParam : 4.0f;

        const bool gainChanged = !std::isfinite(cachedGain) || std::abs(cachedGain - gain) > 1.0e-4f;
        const bool lowChanged = !std::isfinite(cachedLow) || std::abs(cachedLow - low) > 0.01f;
        const bool midChanged = !std::isfinite(cachedMid) || std::abs(cachedMid - mid) > 0.01f;
        const bool midFreqChanged = !std::isfinite(cachedMidFreq) || std::abs(cachedMidFreq - midFreq) > 0.5f;
        const bool highChanged = !std::isfinite(cachedHigh) || std::abs(cachedHigh - high) > 0.01f;

        if (!gainChanged && !lowChanged && !midChanged && !midFreqChanged && !highChanged)
            return;

        cachedGain = gain;
        cachedLow = low;
        cachedMid = mid;
        cachedMidFreq = midFreq;
        cachedHigh = high;

        const float topCut = juce::jmap(gain, 7600.0f, 10800.0f);

        *inputHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 70.0f);
        *lowShelf.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentInnerRate,
            110.0f, 0.74f, juce::Decibels::decibelsToGain(low));
        *midPeak.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentInnerRate,
            midFreq, 0.82f, juce::Decibels::decibelsToGain(mid));
        *highShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentInnerRate,
            3200.0f, 0.72f, juce::Decibels::decibelsToGain(high));
        *outputLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentInnerRate, topCut, 0.75f);
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerRate, 18.0f);
    }

    juce::dsp::Oversampling<float> oversampler;
    Filter inputHighPass;
    Filter lowShelf;
    Filter midPeak;
    Filter highShelf;
    Filter outputLowPass;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    juce::AudioParameterFloat* gainParam = nullptr;
    juce::AudioParameterFloat* lowParam = nullptr;
    juce::AudioParameterFloat* midParam = nullptr;
    juce::AudioParameterFloat* midFreqParam = nullptr;
    juce::AudioParameterFloat* highParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentInnerRate = 176400.0;
    float cachedGain = std::numeric_limits<float>::quiet_NaN();
    float cachedLow = std::numeric_limits<float>::quiet_NaN();
    float cachedMid = std::numeric_limits<float>::quiet_NaN();
    float cachedMidFreq = std::numeric_limits<float>::quiet_NaN();
    float cachedHigh = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
