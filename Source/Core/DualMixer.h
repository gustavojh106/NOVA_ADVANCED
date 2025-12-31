{
type: uploaded file
fileName : DualMixer.h
fullContent :
#pragma once
#include <JuceHeader.h>
#include "Common.h"

// Un procesador interno que mezcla 4 canales (L_A, R_A, L_B, R_B) a 2 salidas
class DualMixer : public juce::AudioProcessor
{
public:
    DualMixer()
        : AudioProcessor(BusesProperties()
            .withInput("InputA", juce::AudioChannelSet::stereo())
            .withInput("InputB", juce::AudioChannelSet::stereo())
            .withOutput("Output", juce::AudioChannelSet::stereo()))
    {
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        // Este nodo recibirá 4 canales de entrada si el grafo está bien configurado:
        // 0-1: Cadena A, 2-3: Cadena B (dependiendo de cómo conectemos en AudioEngine)
        // PERO, en AudioProcessorGraph, lo más fácil es tener múltiples buses de entrada.
        // Simplificación SOTA: Asumiremos que el AudioEngine suma todo al bus principal antes, 
        // O mejor: Hacemos el mixing nosotros gestionando las ganancias.

        // Para simplificar la topología del grafo, controlaremos parámetros atómicos
        // y dejaremos que AudioEngine use nodos de ganancia estándar, 
        // o implementamos lógica aquí. Hagamos lógica aquí para ser robustos.

        // Nota: En un Grafo JUCE, un nodo con múltiples buses de entrada es complejo.
        // ESTRATEGIA: Este Mixer será el nodo final. Recibirá inputs de A y B.
        // Implementación real: El AudioEngine conectará el final de ChainA a las entradas 0/1
        // y el final de ChainB a las entradas 2/3 de este nodo.

        int numSamples = buffer.getNumSamples();

        // Punteros a datos (suponiendo bus layout 4 in -> 2 out en el grafo real)
        // Si el host no soporta buses > 2 canales, el grafo lo maneja internamente.

        float gainA = currentGainA;
        float gainB = currentGainB;

        // Lógica de Switching (Hard switch o Crossfade suave si quisieras mejorarlo)
        if (mode == Nova::SwitcherMode::B) gainA = 0.0f;
        if (mode == Nova::SwitcherMode::A) gainB = 0.0f;

        // Aplicamos ganancia
        // INPUTS: 0=LeftA, 1=RightA, 2=LeftB, 3=RightB
        // OUTPUTS: 0=Left, 1=Right

        // Debido a como JUCE maneja los buffers en processBlock, si tenemos mas inputs que outputs,
        // los canales extra estan en el buffer.

        auto* outL = buffer.getWritePointer(0);
        auto* outR = buffer.getWritePointer(1);

        const auto* inL_A = buffer.getReadPointer(0);
        const auto* inR_A = buffer.getReadPointer(1);

        // Si no hay 4 canales, algo anda mal, protegemos
        if (buffer.getNumChannels() >= 4)
        {
            const auto* inL_B = buffer.getReadPointer(2);
            const auto* inR_B = buffer.getReadPointer(3);

            for (int i = 0; i < numSamples; ++i)
            {
                // Suma simple con ganancia
                float left = (inL_A[i] * gainA) + (inL_B[i] * gainB);
                float right = (inR_A[i] * gainA) + (inR_B[i] * gainB);

                outL[i] = left;
                outR[i] = right;
            }
        }
        else
        {
            // Fallback si solo llega A
            buffer.applyGain(gainA);
        }
    }

    void setMixerState(float gA, float gB, Nova::SwitcherMode m)
    {
        currentGainA = gA;
        currentGainB = gB;
        mode = m;
    }

    // Boilerplate mínimo
    const juce::String getName() const override { return "DualMixer"; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 0; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    float currentGainA = 1.0f;
    float currentGainB = 1.0f;
    Nova::SwitcherMode mode = Nova::SwitcherMode::Dual;
};
}