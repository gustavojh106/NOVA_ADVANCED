#pragma once
#include "ProcessorBase.h"
#include <juce_dsp/juce_dsp.h>

// ==============================================================================
// 1. EDITOR GRÁFICO (Sin cambios, mantenemos tu UI bonita)
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
// 2. PROCESADOR DSP (Con Oversampling)
// ==============================================================================
class PedalOverdrive : public ProcessorBase
{
public:
    PedalOverdrive() :
        // Inicializamos el oversampler: 
        // 2 canales, Factor 2 (2^2 = 4x Oversampling), Filtro IIR (Eficiente y baja latencia)
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
        // 1. Preparar el Oversampler primero
        oversampler.initProcessing(static_cast<size_t>(samplesPerBlock));

        // 2. Preparar la cadena DSP con la frecuencia MULTIPLICADA
        // Como vamos a correr a 4x velocidad, el DSP debe saberlo (importante para filtros)
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate * 4.0; // <--- OJO AQUÍ
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock) * 4;
        spec.numChannels = 2;

        processorChain.prepare(spec);

        // Reportar latencia al host (El oversampling añade unos pocos samples de retraso)
        setLatencySamples(oversampler.getLatencyInSamples());
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::dsp::AudioBlock<float> block(buffer);

        // Actualizar parámetros
        processorChain.get<0>().setGainLinear(*driveParam);
        processorChain.get<2>().setGainLinear(*levelParam);

        // --- MAGIA DEL OVERSAMPLING ---

        // 1. Subir resolución (Upsample)
        // Esto crea un bloque temporal mucho más grande con más datos
        juce::dsp::AudioBlock<float> upsampledBlock = oversampler.processSamplesUp(block);

        // 2. Procesar la distorsión en Alta Resolución
        juce::dsp::ProcessContextReplacing<float> context(upsampledBlock);
        processorChain.process(context);

        // 3. Bajar resolución (Downsample)
        // Esto filtra el aliasing y devuelve el audio al tamaño normal del buffer
        oversampler.processSamplesDown(block);
    }

    const juce::String getName() const override { return "Overdrive"; }

private:
    using Chain = juce::dsp::ProcessorChain<juce::dsp::Gain<float>, juce::dsp::WaveShaper<float>, juce::dsp::Gain<float>>;
    Chain processorChain;

    // Objeto de Oversampling
    juce::dsp::Oversampling<float> oversampler;

    juce::AudioParameterFloat* driveParam;
    juce::AudioParameterFloat* levelParam;
};