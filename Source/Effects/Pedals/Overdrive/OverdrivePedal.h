#pragma once

#include "../Base/ProcessorBase.h"
#include "../Base/PremiumPedalUI.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

class OverdrivePedal final : public ProcessorBase
{
public:
    OverdrivePedal()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("drive", "Drive", 0.0f, 100.0f, 30.0f));
        addParameter(toneParam = new juce::AudioParameterFloat("tone", "Tone", 0.0f, 1.0f, 0.58f));
        addParameter(textureParam = new juce::AudioParameterFloat("texture", "Texture", 0.0f, 1.0f, 0.42f));
        addParameter(mixParam = new juce::AudioParameterFloat("mix", "Mix", 0.0f, 1.0f, 1.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("level", "Level", 0.0f, 1.0f, 0.74f));
    }

    const juce::String getName() const override { return "Overdrive"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    // Expose parameters for the custom editor
    juce::AudioParameterFloat* getDriveParam() const { return driveParam; }
    juce::AudioParameterFloat* getToneParam() const { return toneParam; }
    juce::AudioParameterFloat* getTextureParam() const { return textureParam; }
    juce::AudioParameterFloat* getMixParam() const { return mixParam; }
    juce::AudioParameterFloat* getLevelParam() const { return levelParam; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.reset();
        oversampler.initProcessing((size_t)juce::jmax(1, samplesPerBlock));

        const auto innerRate = sampleRate * 4.0;
        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate = innerRate;
        innerSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock * 4);
        innerSpec.numChannels = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        preHighPass.prepare(innerSpec);
        prePresence.prepare(innerSpec);
        driveStage.prepare(innerSpec);
        postBody.prepare(innerSpec);
        postLowPass.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        mixSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        levelSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
        mixSmooth.setCurrentAndTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? levelFromControl(*levelParam) : 1.0f);

        scratchBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
            juce::jmax(1, samplesPerBlock),
            false,
            false,
            true);

        currentInnerSampleRate = innerRate;
        cachedTone = std::numeric_limits<float>::quiet_NaN();
        cachedTexture = std::numeric_limits<float>::quiet_NaN();

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
        preHighPass.reset();
        prePresence.reset();
        postBody.reset();
        postLowPass.reset();
        dcBlock.reset();
        driveStage.reset();

        if (driveParam != nullptr && textureParam != nullptr)
            driveStage.setTargets(*driveParam, *textureParam);

        if (mixParam != nullptr)
            mixSmooth.setCurrentAndTargetValue(*mixParam);

        if (levelParam != nullptr)
            levelSmooth.setCurrentAndTargetValue(levelFromControl(*levelParam));
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        if (scratchBuffer.getNumChannels() < buffer.getNumChannels()
            || scratchBuffer.getNumSamples() < buffer.getNumSamples())
        {
            scratchBuffer.setSize(buffer.getNumChannels(),
                buffer.getNumSamples(),
                false,
                false,
                true);
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch), buffer.getReadPointer(ch), buffer.getNumSamples());

        updateFiltersIfNeeded();
        driveStage.setTargets(driveParam != nullptr ? *driveParam : 30.0f,
            textureParam != nullptr ? *textureParam : 0.42f);
        mixSmooth.setTargetValue(mixParam != nullptr ? *mixParam : 1.0f);
        levelSmooth.setTargetValue(levelFromControl(levelParam != nullptr ? *levelParam : 0.74f));

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> innerContext(upsampled);

        preHighPass.process(innerContext);
        prePresence.process(innerContext);
        driveStage.process(innerContext);
        postBody.process(innerContext);
        postLowPass.process(innerContext);
        dcBlock.process(innerContext);
        oversampler.processSamplesDown(block);

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float mix = mixSmooth.getNextValue();
            const float dry = 1.0f - mix;
            const float level = levelSmooth.getNextValue();

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float clean = scratchBuffer.getSample(ch, sample);
                const float wet = buffer.getSample(ch, sample);
                buffer.setSample(ch, sample, ((wet * mix) + (clean * dry)) * level);
            }
        }

        endBypassProcess(buffer);
    }

private:
    class ResponsiveDriveStage
    {
    public:
        void prepare(const juce::dsp::ProcessSpec& spec)
        {
            sampleRate = spec.sampleRate;
            driveSmooth.reset(sampleRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
            textureSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);
            reset();
        }

        void reset()
        {
            dcOffset = 0.0f;
        }

        void setTargets(float driveControl, float textureControl)
        {
            driveSmooth.setTargetValue(juce::Decibels::decibelsToGain(4.0f + driveControl * 0.34f));
            textureSmooth.setTargetValue(juce::jlimit(0.0f, 1.0f, textureControl));
        }

        template <typename ProcessContext>
        void process(const ProcessContext& context) noexcept
        {
            auto&& block = context.getOutputBlock();
            const auto channels = (int)block.getNumChannels();
            const auto samples = (int)block.getNumSamples();

            for (int sample = 0; sample < samples; ++sample)
            {
                const float drive = driveSmooth.getNextValue();
                const float texture = textureSmooth.getNextValue();
                const float asymmetry = 0.03f + texture * 0.24f;
                const float density = 1.2f + texture * 2.2f;
                const float sparkle = 0.20f + texture * 0.45f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = block.getChannelPointer((size_t)ch);
                    float x = data[sample] * drive;
                    x += asymmetry * x * x * juce::jlimit(-1.5f, 1.5f, x);

                    const float rounded = std::tanh(x);
                    const float focused = std::atan(density * x) * (2.0f / juce::MathConstants<float>::pi);
                    float y = juce::jmap(texture, rounded, (rounded * (1.0f - sparkle)) + (focused * sparkle));

                    dcOffset = (dcOffset * Nova::Config::DC_OFFSET_DECAY) + (y * Nova::Config::DC_OFFSET_ATTACK);
                    data[sample] = y - dcOffset;
                }
            }
        }

    private:
        double sampleRate = 44100.0;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> textureSmooth;
        float dcOffset = 0.0f;
    };

    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    static float levelFromControl(float control) noexcept
    {
        return juce::jmap(juce::jlimit(0.0f, 1.0f, control), 0.08f, 1.55f);
    }

    void updateFiltersIfNeeded()
    {
        const float tone = toneParam != nullptr ? *toneParam : 0.58f;
        const float texture = textureParam != nullptr ? *textureParam : 0.42f;

        const bool toneChanged = !std::isfinite(cachedTone) || std::abs(cachedTone - tone) > 1.0e-4f;
        const bool textureChanged = !std::isfinite(cachedTexture) || std::abs(cachedTexture - texture) > 1.0e-4f;
        if (!toneChanged && !textureChanged)
            return;

        cachedTone = tone;
        cachedTexture = texture;
        const float toneFreq = juce::jmap(tone, 1700.0f, 9200.0f);
        const float presenceGain = juce::jmap(tone + texture * 0.4f, -3.0f, 7.0f);
        const float bodyGain = juce::jmap(texture, -1.2f, 2.4f);

        *preHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerSampleRate, 38.0f);
        *prePresence.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentInnerSampleRate,
            1050.0f,
            0.75f,
            juce::Decibels::decibelsToGain(presenceGain));
        *postBody.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentInnerSampleRate,
            240.0f,
            0.72f,
            juce::Decibels::decibelsToGain(bodyGain));
        *postLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentInnerSampleRate,
            toneFreq,
            0.68f);
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentInnerSampleRate, 18.0f);
    }

    juce::dsp::Oversampling<float> oversampler;
    Filter preHighPass;
    Filter prePresence;
    ResponsiveDriveStage driveStage;
    Filter postBody;
    Filter postLowPass;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;
    juce::AudioBuffer<float> scratchBuffer;

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* toneParam = nullptr;
    juce::AudioParameterFloat* textureParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;

    double currentInnerSampleRate = 176400.0;
    float cachedTone = std::numeric_limits<float>::quiet_NaN();
    float cachedTexture = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};

// Include after class definition to resolve circular dependency
#include "OverdriveEditor.h"
