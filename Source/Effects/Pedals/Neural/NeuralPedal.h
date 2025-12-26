//#pragma once
//#include "../Base/ProcessorBase.h"
//// Desactivar advertencias de libreria externa
//#if defined(_MSC_VER)
//#pragma warning (push)
//#pragma warning (disable : 4100 4244 4267)
//#endif
//
//
//#if defined(_MSC_VER)
//#pragma warning (pop)
//#endif
//
//class PedalNeural : public ProcessorBase
//{
//public:
//    PedalNeural();
//    ~PedalNeural() override;
//
//    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
//    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
//    const juce::String getName() const override { return "Neural Amp SOTA"; }
//
//    bool hasEditor() const override { return true; }
//    juce::AudioProcessorEditor* createEditor() override;
//
//    // Controles
//    void setDrive(float newDrive) { inputGain = newDrive; }
//    void setLevel(float newLevel) { outputGain = newLevel; }
//
//    // Funciutura para cargar modelos .json reales entrenados en Python
//    void loadModelWeights();
//
//private:
//    float inputGain = 1.0f; // Ajustado para LSTM
//    float outputGain = 0.5f;
//
//    // ==============================================================================
//    // ARQUITECTURA SOTA (LSTM)
//    // ==============================================================================
//    // 1 entrada -> LSTM (8 neuronas ocultas) -> Dense (8 a 1) -> 1 salida
//    // Esta topologa es estndar en plugins como NAM para modelar amplis limpios y crunch.
//    RTNeural::ModelT<float, 1, 1,
//        RTNeural::LSTMLayerT<float, 1, 8>, // Capa con Memoria (Recurrente)
//        RTNeural::DenseT<float, 8, 1>      // Capa de Salida (Mezcla)
//    > model;
//};