#pragma once
#include "../Pedals/Base/ProcessorBase.h"
#include <juce_dsp/juce_dsp.h>

// ==============================================================
// 1. EL EDITOR VISUAL (UI) - Se mantiene igual
// ==============================================================
class PedalCabinetEditor : public juce::AudioProcessorEditor
{
public:
    PedalCabinetEditor(juce::AudioProcessor& p) : AudioProcessorEditor(&p)
    {
        setSize(200, 300);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour::fromFloatRGBA(0.15f, 0.15f, 0.15f, 1.0f));
        g.setColour(juce::Colours::black);
        for (int i = 0; i < getHeight(); i += 10) g.drawHorizontalLine(i, 0, (float)getWidth());
        g.setColour(juce::Colours::silver);
        g.drawRect(getLocalBounds(), 4);
        g.setColour(juce::Colours::white);
        g.setFont(20.0f);
        g.drawRect(20, 40, 160, 60, 2);
        g.fillRect(20, 40, 160, 60);
        g.setColour(juce::Colours::black);
        g.drawText("CABINET 4x12", 20, 40, 160, 60, juce::Justification::centred, true);
    }
};

// ==============================================================
// 2. EL PROCESADOR (DSP) - CON PROTECCIÓN ANTI-CRASH
// ==============================================================
class PedalCabinet : public ProcessorBase
{
public:
    PedalCabinet()
    {
        // Cargamos el IR en el constructor, pero NO permitimos procesar todavía
        auto irFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory().getChildFile("demo.wav");

        if (!irFile.existsAsFile())
            irFile = juce::File(__FILE__).getParentDirectory().getChildFile("demo.wav");

        if (irFile.existsAsFile())
        {
            convolution.loadImpulseResponse(irFile,
                juce::dsp::Convolution::Stereo::yes,
                juce::dsp::Convolution::Trim::yes,
                0,
                juce::dsp::Convolution::Normalise::yes);
        }
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        // 1. Validamos los datos
        if (sampleRate <= 0) return;

        // 2. Preparamos el motor DSP
        juce::dsp::ProcessSpec spec{ sampleRate, static_cast<juce::uint32>(samplesPerBlock), 2 };
        convolution.prepare(spec);

        // 3. Reset por seguridad
        convolution.reset();

        // 4. ¡SEMÁFORO EN VERDE! Solo ahora es seguro procesar audio
        isPrepared = true;
    }

    void releaseResources() override
    {
        isPrepared = false; // Semáforo en rojo al cerrar
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        // --- ESCUDO TOTAL ---
        // Solo entramos si hay archivo Y si prepareToPlay ya ocurrió.
        if (isPrepared && convolution.getCurrentIRSize() > 0)
        {
            juce::dsp::AudioBlock<float> block(buffer);
            juce::dsp::ProcessContextReplacing<float> context(block);
            convolution.process(context);
        }
        // Si no está listo, el audio pasa limpio (Bypass) sin crashear.
    }

    const juce::String getName() const override { return "Cabinet"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new PedalCabinetEditor(*this);
    }

private:
    juce::dsp::Convolution convolution;

    // Esta variable salva el día:
    // Evita que el processBlock corra antes que el prepareToPlay
    bool isPrepared = false;
};