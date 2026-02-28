#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <memory>

#include "../Pedals/Base/ProcessorBase.h"

class ClassicAmpEditor final : public juce::AudioProcessorEditor
{
public:
    ClassicAmpEditor(juce::AudioProcessor& processor,
        juce::AudioParameterFloat* drive,
        juce::AudioParameterFloat* level)
        : juce::AudioProcessorEditor(&processor)
    {
        configureSlider(driveSlider);
        configureSlider(levelSlider);

        driveAttachment = std::make_unique<juce::SliderParameterAttachment>(*drive, driveSlider);
        levelAttachment = std::make_unique<juce::SliderParameterAttachment>(*level, levelSlider);

        title.setText("CLASSIC AMP", juce::dontSendNotification);
        title.setJustificationType(juce::Justification::centred);
        title.setFont(juce::Font(13.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(title);

        driveLabel.setText("DRIVE", juce::dontSendNotification);
        driveLabel.setJustificationType(juce::Justification::centred);
        driveLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(driveLabel);

        levelLabel.setText("LEVEL", juce::dontSendNotification);
        levelLabel.setJustificationType(juce::Justification::centred);
        levelLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(levelLabel);

        addAndMakeVisible(driveSlider);
        addAndMakeVisible(levelSlider);

        setSize(140, 190);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();

        g.fillAll(juce::Colour::fromString("ff1a1a1a"));
        g.setColour(juce::Colours::grey.withAlpha(0.35f));
        g.drawRoundedRectangle(bounds.reduced(2.0f), 6.0f, 2.0f);

        g.setColour(juce::Colours::red.withAlpha(0.7f));
        g.fillEllipse(bounds.getCentreX() - 3.0f, 24.0f, 6.0f, 6.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        title.setBounds(area.removeFromTop(20));
        area.removeFromTop(8);

        auto row = area.removeFromTop(120);
        auto left = row.removeFromLeft(row.getWidth() / 2);

        driveSlider.setBounds(left.reduced(6));
        levelSlider.setBounds(row.reduced(6));

        auto labels = area.removeFromTop(26);
        driveLabel.setBounds(labels.removeFromLeft(labels.getWidth() / 2));
        levelLabel.setBounds(labels);
    }

private:
    static void configureSlider(juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRotaryParameters(juce::MathConstants<float>::pi,
            juce::MathConstants<float>::twoPi,
            true);
    }

    juce::Slider driveSlider;
    juce::Slider levelSlider;

    juce::Label title;
    juce::Label driveLabel;
    juce::Label levelLabel;

    std::unique_ptr<juce::SliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> levelAttachment;
};

class ClassicAmp final : public ProcessorBase
{
public:
    ClassicAmp()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("ampDrive", "Drive", 0.5f, 6.0f, 2.5f));
        addParameter(levelParam = new juce::AudioParameterFloat("ampLevel", "Level", 0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Classic Amp"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new ClassicAmpEditor(*this, driveParam, levelParam);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.initProcessing(static_cast<size_t>(samplesPerBlock));
        const double innerRate = sampleRate * 4.0;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = innerRate;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock) * 4;
        spec.numChannels = static_cast<juce::uint32>(juce::jmax(1, getTotalNumOutputChannels()));

        preHighPass.prepare(spec);
        postLowPass.prepare(spec);
        dcBlock.prepare(spec);

        *preHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerRate, 35.0f);
        *postLowPass.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(innerRate, 9000.0f);
        *dcBlock.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerRate, 20.0f);

        driveSmooth.reset(innerRate, 0.01);
        levelSmooth.reset(innerRate, 0.01);
        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 2.5f);
        levelSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        setLatencySamples(oversampler.getLatencyInSamples());
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
        postLowPass.reset();
        dcBlock.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampledBlock = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampledBlock);

        preHighPass.process(context);

        driveSmooth.setTargetValue(driveParam != nullptr ? *driveParam : 2.5f);
        levelSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        float* channelData[2] = { nullptr, nullptr };
        const int numChannels = (int)juce::jmin<size_t>(2, upsampledBlock.getNumChannels());
        for (int ch = 0; ch < numChannels; ++ch)
            channelData[ch] = upsampledBlock.getChannelPointer((size_t)ch);

        const int numSamples = (int)upsampledBlock.getNumSamples();
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float drive = driveSmooth.getNextValue();
            const float level = levelSmooth.getNextValue();

            for (int ch = 0; ch < numChannels; ++ch)
                channelData[ch][sample] = std::tanh(channelData[ch][sample] * drive) * level;
        }

        postLowPass.process(context);
        dcBlock.process(context);
        oversampler.processSamplesDown(block);
        endBypassProcess(buffer);
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    juce::dsp::Oversampling<float> oversampler;
    Filter preHighPass;
    Filter postLowPass;
    Filter dcBlock;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmooth;

    juce::AudioParameterFloat* driveParam = nullptr;
    juce::AudioParameterFloat* levelParam = nullptr;
    bool isPrepared = false;
};
