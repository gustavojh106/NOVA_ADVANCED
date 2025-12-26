#pragma once
#include "../Base/ProcessorBase.h"
#include <juce_dsp/juce_dsp.h>

// ==============================================================================
// 1. EDITOR GRÁFICO (Sin cambios)
// ==============================================================================
class PedalOverdriveEditor : public juce::AudioProcessorEditor
{
public:
    PedalOverdriveEditor(juce::AudioProcessor& p, juce::AudioParameterFloat* drive, juce::AudioParameterFloat* level)
        : AudioProcessorEditor(&p)
    {
        addAndMakeVisible(driveSlider);
        driveSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        driveSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        driveAttachment.reset(new juce::SliderParameterAttachment(*drive, driveSlider));

        addAndMakeVisible(levelSlider);
        levelSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        levelSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        levelAttachment.reset(new juce::SliderParameterAttachment(*level, levelSlider));

        addAndMakeVisible(driveLabel);
        driveLabel.setText("DRIVE", juce::dontSendNotification);
        driveLabel.setJustificationType(juce::Justification::centred);

        addAndMakeVisible(levelLabel);
        levelLabel.setText("LEVEL", juce::dontSendNotification);
        levelLabel.setJustificationType(juce::Justification::centred);

        setSize(200, 300);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::darkgreen); // Color Tube Screamer
        g.setColour(juce::Colours::white);
        g.drawRect(getLocalBounds(), 4);
        g.setFont(20.0f);
        g.drawText("PRO OVERDRIVE", getLocalBounds().removeFromTop(40), juce::Justification::centred, true);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(20);
        auto topArea = area.removeFromTop(area.getHeight() / 2);
        driveLabel.setBounds(topArea.removeFromTop(20));
        driveSlider.setBounds(topArea);
        levelLabel.setBounds(area.removeFromTop(20));
        levelSlider.setBounds(area);
    }

private:
    juce::Slider driveSlider, levelSlider;
    juce::Label driveLabel, levelLabel;
    std::unique_ptr<juce::SliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> levelAttachment;
};

// ==============================================================================
// 2. PROCESADOR DSP (Con Protección Anti-Crash)
// ==============================================================================
class PedalOverdrive : public ProcessorBase
{
public:
    PedalOverdrive() :
        oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam = new juce::AudioParameterFloat("drive", "Drive", 0.0f, 100.0f, 25.0f));
        addParameter(levelParam = new juce::AudioParameterFloat("level", "Level", 0.0f, 1.0f, 0.1f));

        auto& waveshaper = processorChain.get<1>();
        waveshaper.functionToUse = [](float x) { return std::tanh(x); };
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return new PedalOverdriveEditor(*this, driveParam, levelParam); }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        // 1. Escudo Básico
        if (sampleRate <= 0) return;

        // 2. Inicializar DSP
        oversampler.initProcessing(static_cast<size_t>(samplesPerBlock));

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate * 4.0;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock) * 4;
        spec.numChannels = 2;

        processorChain.prepare(spec);
        setLatencySamples(oversampler.getLatencyInSamples());

        // 3. Reset
        oversampler.reset();
        processorChain.reset();

        // 4. ¡SEMÁFORO EN VERDE!
        isPrepared = true;
    }

    void releaseResources() override
    {
        isPrepared = false; // Apagar al destruir
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        // --- ESCUDO TOTAL ---
        // Si prepareToPlay no ha corrido, NO TOCAMOS EL OVERSAMPLER.
        // Esto evita el crash de memoria 0xFF...
        if (!isPrepared) return;

        juce::dsp::AudioBlock<float> block(buffer);

        // Actualizar parámetros
        processorChain.get<0>().setGainLinear(*driveParam);
        processorChain.get<2>().setGainLinear(*levelParam);

        // --- MAGIA DEL OVERSAMPLING SEGURA ---
        juce::dsp::AudioBlock<float> upsampledBlock = oversampler.processSamplesUp(block);

        juce::dsp::ProcessContextReplacing<float> context(upsampledBlock);
        processorChain.process(context);

        oversampler.processSamplesDown(block);
    }

    const juce::String getName() const override { return "Overdrive"; }

private:
    using Chain = juce::dsp::ProcessorChain<juce::dsp::Gain<float>, juce::dsp::WaveShaper<float>, juce::dsp::Gain<float>>;
    Chain processorChain;

    juce::dsp::Oversampling<float> oversampler;

    juce::AudioParameterFloat* driveParam;
    juce::AudioParameterFloat* levelParam;

    // EL SALVAVIDAS
    bool isPrepared = false;
};