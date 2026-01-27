#pragma once
#include <JuceHeader.h>
#include "../../Constants.h"

class TunerService
{
public:
    TunerService();
    ~TunerService();

    // Llamado desde el Hilo de Audio (ProcessBlock)
    // Retorna true si se escribió algo (para debug)
    void pushBuffer(const juce::AudioBuffer<float>& buffer);

    // Llamado desde el Background Thread (run)
    // Realiza los cálculos matemáticos pesados
    void process();

    // Getters Atómicos (Thread-Safe para la UI)
    float getCurrentPitch() const { return currentPitch.load(); }
    float getCurrentClarity() const { return currentClarity.load(); }
    float getCurrentRMS() const { return currentRMS.load(); }
    int getCurrentNote() const { return currentNote.load(); }

    void reset();
    void setSampleRate(double rate) { sampleRate = rate; }

private:
    // Configuración
    double sampleRate = 44100.0;

    // Buffers y FIFO
    juce::AbstractFifo tunerFifo{ Nova::Config::TUNER_FIFO_SIZE };
    std::vector<float> circularBuffer; // Buffer circular de entrada
    std::vector<float> workBuffer;     // Buffer lineal para procesar

    // Resultados Atómicos (Lectura/Escritura concurrente)
    std::atomic<float> currentPitch{ 0.0f };
    std::atomic<float> currentClarity{ 0.0f };
    std::atomic<float> currentRMS{ 0.0f };
    std::atomic<int> currentNote{ 0 };

    // Matemática Pura (Privada)
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples);
};