#pragma once
#include <JuceHeader.h>

// ==============================================================================
// 1. INPUT CHAIN (Gain -> Noise Gate -> Mono Sum)
// ==============================================================================
// EN GLOBALPROCESSORS.H

enum class InputRouting { Stereo, Left, Right, Sum };

class InputChainProcessor : public juce::AudioProcessor
{
public:
    InputChainProcessor()
        : AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo())
            .withOutput("Out", juce::AudioChannelSet::stereo()))
    {
        // Config SOTA por defecto
        gate.setThreshold(-100.0f);
        gate.setRatio(12.0f);
        gate.setAttack(0.5f); // Ataque ultra-rápido para transitorios de metal
        gate.setRelease(50.0f); // Release natural

        gain.setGainDecibels(0.0f);
    }

    // ... (prepareToPlay y releaseResources igual que antes) ...
    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };
        gate.prepare(spec);
        gain.prepare(spec);
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);

        int numSamples = buffer.getNumSamples();
        int numCh = buffer.getNumChannels();

        // 1. INPUT ROUTING LOGIC (SOTA)
        // Manejamos la señal ANTES de procesarla
        if (numCh > 1) // Solo tiene sentido si tenemos 2 canales de buffer
        {
            auto* l = buffer.getWritePointer(0);
            auto* r = buffer.getWritePointer(1);

            switch (currentRouting)
            {
            case InputRouting::Left:
                // Copiamos L a R (Dual Mono desde L)
                juce::FloatVectorOperations::copy(r, l, numSamples);
                break;

            case InputRouting::Right:
                // Copiamos R a L (Dual Mono desde R)
                juce::FloatVectorOperations::copy(l, r, numSamples);
                break;

            case InputRouting::Sum:
                // Suma (L+R)/2
                for (int i = 0; i < numSamples; ++i) {
                    float sum = (l[i] + r[i]) * 0.5f;
                    l[i] = sum;
                    r[i] = sum;
                }
                break;

            case InputRouting::Stereo:
            default:
                // Passthru (No hacemos nada, respetamos la entrada estéreo)
                break;
            }
        }

        // 2. Input Trim
        gain.setGainDecibels(inputGainDb);
        gain.process(context);

        // 3. Noise Gate (SOTA: Histéresis implícita en juce::dsp::NoiseGate)
        if (gateThreshold > -95.0f)
        {
            gate.setThreshold(gateThreshold);
            gate.process(context);
        }
    }

    // Setter SOTA
    void setParams(float gainDb, float gateDb, bool forceMono, int inputChannelIndex)
    {
        inputGainDb = gainDb;
        gateThreshold = gateDb;

        // Mapeo de lógica UI a lógica DSP
        // Si forceMono es true, asumimos Left (el estándar de guitarra)
        // O podríamos usar inputChannelIndex si tuvieras un selector L/R en la UI.
        // Por ahora, mantendremos compatibilidad con tu botón "Mono":

        if (forceMono)
            currentRouting = InputRouting::Left; // Asumimos guitarra en Input 1
        else
            currentRouting = InputRouting::Stereo;

        // NOTA: En el futuro, cambia 'bool forceMono' por 'int routingMode' en la UI
        // para soportar L, R y Stereo explícitamente.
    }

    // ... (Boilerplate igual) ...
    const juce::String getName() const override { return "InputChain"; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 0; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

private:
    juce::dsp::Gain<float> gain;
    juce::dsp::NoiseGate<float> gate;

    float inputGainDb = 0.0f;
    float gateThreshold = -100.0f;
    InputRouting currentRouting = InputRouting::Stereo;
};
// ==============================================================================
// 2. CHANNEL STRIP (Level -> Pan -> Width)
// ==============================================================================
class ChannelStripProcessor : public juce::AudioProcessor
{
public:
    ChannelStripProcessor()
        : AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo())
            .withOutput("Out", juce::AudioChannelSet::stereo()))
    {
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };
        gain.prepare(spec);
    }

    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        // 1. Gain (Level)
        // Suavizado manual para evitar zippers
        if (std::abs(currentGain - targetGain) > 0.001f)
            currentGain += (targetGain - currentGain) * 0.1f;
        else
            currentGain = targetGain;

        buffer.applyGain(currentGain);

        // 2. Stereo Width (Mid/Side Processing)
        if (std::abs(targetWidth - 1.0f) > 0.01f) // Solo si no es estándar
        {
            auto* l = buffer.getWritePointer(0);
            auto* r = buffer.getWritePointer(1);
            int numSamples = buffer.getNumSamples();

            for (int i = 0; i < numSamples; ++i)
            {
                float mid = (l[i] + r[i]) * 0.5f;
                float side = (l[i] - r[i]) * 0.5f;

                side *= targetWidth; // Expandir o contraer Side

                l[i] = mid + side;
                r[i] = mid - side;
            }
        }

        // 3. Panning (Constant Power)
        if (std::abs(targetPan) > 0.01f)
        {
            float pan = std::max(-1.0f, std::min(1.0f, targetPan));
            // Ley de paneo de potencia constante:
            // L = cos((pan + 1) * PI / 4)
            // R = sin((pan + 1) * PI / 4)
            float angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            float leftGain = std::cos(angle);
            float rightGain = std::sin(angle);

            buffer.applyGainRamp(0, 0, buffer.getNumSamples(), leftGain, leftGain);
            buffer.applyGainRamp(1, 0, buffer.getNumSamples(), rightGain, rightGain);
        }
    }

    void setParams(float gainLinear, float pan, float width)
    {
        targetGain = gainLinear;
        targetPan = pan;
        targetWidth = width;
    }

    // Boilerplate...
    const juce::String getName() const override { return "Strip"; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 0; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

private:
    juce::dsp::Gain<float> gain;
    float currentGain = 1.0f;
    float targetGain = 1.0f;
    float targetPan = 0.0f;
    float targetWidth = 1.0f;
};