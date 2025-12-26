#include "PedalNeural.h"

// ==============================================================
//  CLASE DEL EDITOR (UI) - Se mantiene igual de bonita
// ==============================================================
class PedalNeuralEditor : public juce::AudioProcessorEditor
{
public:
    PedalNeuralEditor(PedalNeural& p)
        : AudioProcessorEditor(&p), audioProcessor(p)
    {
        // Drive
        driveKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        driveKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 90, 0);
        driveKnob.setRange(0.0f, 40.0f, 0.1f); // Rango más amplio para saturar la LSTM
        driveKnob.setValue(5.0f);
        driveKnob.onValueChange = [this] { audioProcessor.setDrive((float)driveKnob.getValue()); };
        addAndMakeVisible(driveKnob);

        // Level
        levelKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        levelKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 90, 0);
        levelKnob.setRange(0.0f, 2.0f, 0.1f);
        levelKnob.setValue(0.5f);
        levelKnob.onValueChange = [this] { audioProcessor.setLevel((float)levelKnob.getValue()); };
        addAndMakeVisible(levelKnob);

        // Etiquetas
        driveLabel.setText("TUBE DRIVE", juce::dontSendNotification); // Nombre más Pro
        driveLabel.setJustificationType(juce::Justification::centred);
        driveLabel.setColour(juce::Label::textColourId, juce::Colours::cyan);
        addAndMakeVisible(driveLabel);

        levelLabel.setText("MASTER", juce::dontSendNotification);
        levelLabel.setJustificationType(juce::Justification::centred);
        levelLabel.setColour(juce::Label::textColourId, juce::Colours::cyan);
        addAndMakeVisible(levelLabel);

        setSize(200, 300);
    }

    void paint(juce::Graphics& g) override
    {
        // Fondo más "Tech"
        g.fillAll(juce::Colour::fromFloatRGBA(0.08f, 0.08f, 0.10f, 1.0f));
        g.setColour(juce::Colours::cyan);
        g.drawRect(getLocalBounds(), 2);
        g.setFont(20.0f);
        g.drawFittedText("LSTM PREAMP", getLocalBounds().removeFromTop(40), juce::Justification::centred, 1);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        auto topArea = area.removeFromTop(area.getHeight() / 2);
        driveLabel.setBounds(topArea.removeFromTop(20));
        driveKnob.setBounds(topArea);
        levelLabel.setBounds(area.removeFromTop(20));
        levelKnob.setBounds(area);
    }

private:
    PedalNeural& audioProcessor;
    juce::Slider driveKnob, levelKnob;
    juce::Label driveLabel, levelLabel;
};

// ==============================================================
//  LÓGICA DEL PROCESADOR (SOTA DSP)
// ==============================================================

PedalNeural::PedalNeural()
{
}

PedalNeural::~PedalNeural()
{
}

juce::AudioProcessorEditor* PedalNeural::createEditor()
{
    return new PedalNeuralEditor(*this);
}

void PedalNeural::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    model.reset();
    loadModelWeights(); // Cargamos los pesos "mágicos"
}

void PedalNeural::loadModelWeights()
{
    //// TRUCO DE INGENIERÍA:
    //// Inicializar una LSTM a mano para que suene como un "Clean Boost" es difícil 
    //// porque tiene compuertas (gates) complejas.
    //// Aquí configuramos los pesos para que la señal pase casi transparente 
    //// pero con una ligera compresión "analógica".

    //auto& lstm = model.get<0>();
    //auto& dense = model.get<1>();

    //// 1. Pesos de la LSTM (Input -> Hidden)
    //// Hacemos que la entrada afecte fuertemente al estado interno
    //std::vector<std::vector<float>> lstm_kernel(1, std::vector<float>(8 * 4, 0.5f));
    //lstm.setWVals(lstm_kernel);

    //// 2. Pesos recurrentes (Hidden -> Hidden)
    //// Valores bajos para evitar feedback loop infinito (explosión)
    //std::vector<std::vector<float>> lstm_recurrent(8, std::vector<float>(8 * 4, 0.05f));
    //lstm.setUVals(lstm_recurrent);

    //// 3. Bias de la LSTM
    //// Truco: Bias positivo en la "Forget Gate" para recordar el estado (sustain)
    //std::vector<float> lstm_bias(8 * 4, 0.0f);
    //for (int i = 0; i < 8; ++i) lstm_bias[i + 8] = 1.0f; // Forget gate bias
    //lstm.setBVals(lstm_bias);

    //// 4. Capa Densa de Salida (Mezclar las 8 neuronas a 1 salida)
    //// Promediamos las salidas
    //std::vector<std::vector<float>> dense_w(8, std::vector<float>(1, 0.15f));
    //dense.setWeights(dense_w);

    //// Sin bias en la salida para evitar DC Offset
    //std::vector<float> dense_b = { 0.0f };
    //dense.setBias(dense_b.data());

    model.reset();
}

void PedalNeural::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    //const int numSamples = buffer.getNumSamples();

    //// Procesamiento estéreo independiente
    //// (Nota: Idealmente usaríamos 2 modelos para estéreo real, 
    //// pero para eficiencia compartimos el modelo reiniciando estado o procesando L/R secuencialmente.
    //// Para simplificar hoy, procesamos Mono sumado y duplicamos, o procesamos canal L).

    //auto* leftChannel = buffer.getWritePointer(0);
    //auto* rightChannel = (buffer.getNumChannels() > 1) ? buffer.getWritePointer(1) : nullptr;

    //for (int i = 0; i < numSamples; ++i)
    //{
    //    // 1. Acondicionamiento de entrada (Input Gain)
    //    float input = leftChannel[i] * inputGain;

    //    // Limiter suave antes de la red para evitar valores locos
    //    input = std::tanh(input * 0.5f);

    //    // 2. Inferencia Neural (SOTA LSTM)
    //    // La LSTM actualiza su estado interno automáticamente aquí.
    //    float output = model.forward(&input);

    //    // 3. Salida (Level)
    //    output = output * outputGain;

    //    // Asignar a canales
    //    leftChannel[i] = output;
    //    if (rightChannel)
    //        rightChannel[i] = output;
    //}
    return;
}