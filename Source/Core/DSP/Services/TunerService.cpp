#include "TunerService.h"

TunerService::TunerService()
{
    circularBuffer.resize(Nova::Config::TUNER_FIFO_SIZE, 0.0f);
    workBuffer.resize(Nova::Config::TUNER_PROCESS_SIZE, 0.0f);
    reset();
}

TunerService::~TunerService() {}

void TunerService::reset()
{
    std::fill(circularBuffer.begin(), circularBuffer.end(), 0.0f);
    std::fill(workBuffer.begin(), workBuffer.end(), 0.0f);
    tunerFifo.reset();

    currentPitch = 0.0f;
    currentClarity = 0.0f;
    currentRMS = 0.0f;
    currentNote = 0;
}

void TunerService::pushBuffer(const juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const auto* inL = buffer.getReadPointer(0);
    const auto* inR = (buffer.getNumChannels() > 1) ? buffer.getReadPointer(1) : nullptr;

    int start1, size1, start2, size2;
    tunerFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

    if (size1 > 0)
    {
        for (int i = 0; i < size1; ++i)
        {
            float val = inL[i];
            if (inR) val += inR[i];
            circularBuffer[start1 + i] = val * 0.5f; // Suma a Mono
        }
    }
    if (size2 > 0)
    {
        for (int i = 0; i < size2; ++i)
        {
            float val = inL[size1 + i];
            if (inR) val += inR[size1 + i];
            circularBuffer[start2 + i] = val * 0.5f;
        }
    }

    tunerFifo.finishedWrite(size1 + size2);
}

void TunerService::process()
{
    // Verificamos si hay suficientes datos para un análisis completo
    if (tunerFifo.getNumReady() >= Nova::Config::TUNER_PROCESS_SIZE)
    {
        int start1, size1, start2, size2;
        tunerFifo.prepareToRead(Nova::Config::TUNER_PROCESS_SIZE, start1, size1, start2, size2);

        // Copiamos del circular al lineal (workBuffer)
        if (size1 > 0)
            juce::FloatVectorOperations::copy(workBuffer.data(),
                circularBuffer.data() + start1, size1);
        if (size2 > 0)
            juce::FloatVectorOperations::copy(workBuffer.data() + size1,
                circularBuffer.data() + start2, size2);

        tunerFifo.finishedRead(size1 + size2);

        // 1. Calcular RMS
        float sumSq = 0.0f;
        for (float s : workBuffer) sumSq += s * s;
        float rms = std::sqrt(sumSq / (float)Nova::Config::TUNER_PROCESS_SIZE);
        currentRMS = rms;

        // 2. Gate Ultra-Bajo para tuner
        if (rms > 0.0002f)
        {
            auto result = calculateFrequencyWithClarity(workBuffer.data(), Nova::Config::TUNER_PROCESS_SIZE);
            float freq = result.first;
            float clarity = result.second;

            // Filtro de Claridad
            if (clarity > 0.85f && freq > 20.0f && freq < 4000.0f)
            {
                currentPitch = freq;
                currentClarity = clarity;

                float midiNote = 69.0f + 12.0f * std::log2(freq / 440.0f);
                currentNote = (int)std::round(midiNote);
            }
            else
            {
                currentClarity = 0.0f; // Señal sucia o ruido
            }
        }
        else
        {
            currentClarity = 0.0f;
            currentPitch = 0.0f;
        }
    }
}

std::pair<float, float> TunerService::calculateFrequencyWithClarity(const float* signal, int numSamples)
{
    if (sampleRate <= 0.0) return { 0.0f, 0.0f };

    int minPeriod = (int)(sampleRate / 1500.0);
    int maxPeriod = (int)(sampleRate / 40.0); // Hasta ~40Hz
    if (maxPeriod > numSamples / 2) maxPeriod = numSamples / 2;

    float bestCorrelation = 0.0f;
    int bestPeriod = 0;

    // Autocorrelación simple (YIN simplificado)
    for (int lag = minPeriod; lag < maxPeriod; ++lag)
    {
        float sum = 0.0f;
        float sumSq = 0.0f;
        int limit = numSamples - lag;

        for (int i = 0; i < limit; ++i) {
            float s1 = signal[i];
            float s2 = signal[i + lag];
            sum += s1 * s2;
            sumSq += s1 * s1;
        }

        float correlation = 0.0f;
        if (sumSq > 0.00001f) correlation = sum / sumSq;

        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            bestPeriod = lag;
        }
    }

    if (bestCorrelation < 0.2f) return { 0.0f, 0.0f };

    // Interpolación Parabólica para precisión fina
    float finalPeriod = (float)bestPeriod;
    if (bestPeriod > minPeriod && bestPeriod < maxPeriod - 1)
    {
        float prevCorr = 0.0f;
        float nextCorr = 0.0f;
        int limitPrev = numSamples - (bestPeriod - 1);
        int limitNext = numSamples - (bestPeriod + 1);

        for (int i = 0; i < limitPrev; ++i) prevCorr += signal[i] * signal[i + (bestPeriod - 1)];
        for (int i = 0; i < limitNext; ++i) nextCorr += signal[i] * signal[i + (bestPeriod + 1)];

        float denominator = prevCorr - 2.0f * bestCorrelation + nextCorr;
        if (std::abs(denominator) > 0.00001f)
        {
            float delta = (prevCorr - nextCorr) / (2.0f * denominator);
            finalPeriod = bestPeriod - delta;
        }
    }

    return { (float)(sampleRate / finalPeriod), bestCorrelation };
}