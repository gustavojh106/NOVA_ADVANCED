#include "TunerService.h"
#include <cmath>
#include <algorithm>

TunerService::TunerService()
{
    circularBuffer.resize(Nova::Config::TUNER_FIFO_SIZE, 0.0f);
    workBuffer.resize(Nova::Config::TUNER_PROCESS_SIZE, 0.0f);
    reset();
}

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

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    tunerFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

    // Write block 1
    for (int i = 0; i < size1; ++i)
    {
        float v = inL[i];
        if (inR) v += inR[i];
        circularBuffer[start1 + i] = v * 0.5f; // sum to mono
    }

    // Write block 2 (wrap-around)
    for (int i = 0; i < size2; ++i)
    {
        float v = inL[size1 + i];
        if (inR) v += inR[size1 + i];
        circularBuffer[start2 + i] = v * 0.5f;
    }

    tunerFifo.finishedWrite(size1 + size2);
}

void TunerService::process()
{
    // Only process when we have a full analysis window available
    if (tunerFifo.getNumReady() < Nova::Config::TUNER_PROCESS_SIZE)
        return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    tunerFifo.prepareToRead(Nova::Config::TUNER_PROCESS_SIZE, start1, size1, start2, size2);

    if (size1 > 0)
        juce::FloatVectorOperations::copy(workBuffer.data(),
            circularBuffer.data() + start1,
            size1);

    if (size2 > 0)
        juce::FloatVectorOperations::copy(workBuffer.data() + size1,
            circularBuffer.data() + start2,
            size2);

    tunerFifo.finishedRead(size1 + size2);

    // 1) RMS
    float sumSq = 0.0f;
    for (float s : workBuffer) sumSq += s * s;

    const float rms = std::sqrt(sumSq / (float)Nova::Config::TUNER_PROCESS_SIZE);
    currentRMS = rms;

    // 2) Ultra-low gate for tuning
    if (rms <= 0.0002f)
    {
        currentClarity = 0.0f;
        currentPitch = 0.0f;
        currentNote = 0;
        return;
    }

    const auto [freq, clarity] =
        calculateFrequencyWithClarity(workBuffer.data(), Nova::Config::TUNER_PROCESS_SIZE);

    // Clarity + frequency bounds filter
    if (clarity > 0.85f && freq > 20.0f && freq < 4000.0f)
    {
        currentPitch = freq;
        currentClarity = clarity;

        const float midiNote = 69.0f + 12.0f * std::log2(freq / 440.0f);
        currentNote = (int)std::round(midiNote);
    }
    else
    {
        currentClarity = 0.0f; // noisy/unstable input
        currentPitch = 0.0f;
        currentNote = 0;
    }
}

std::pair<float, float> TunerService::calculateFrequencyWithClarity(const float* signal, int numSamples)
{
    if (sampleRate <= 0.0)
        return { 0.0f, 0.0f };

    int minPeriod = (int)(sampleRate / 1500.0);
    int maxPeriod = (int)(sampleRate / 40.0); // down to ~40 Hz
    if (maxPeriod > numSamples / 2) maxPeriod = numSamples / 2;

    float bestCorrelation = 0.0f;
    int bestPeriod = 0;

    // Simple autocorrelation (YIN-like simplified)
    for (int lag = minPeriod; lag < maxPeriod; ++lag)
    {
        float sum = 0.0f;
        float sumSq = 0.0f;
        const int limit = numSamples - lag;

        for (int i = 0; i < limit; ++i)
        {
            const float s1 = signal[i];
            const float s2 = signal[i + lag];
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

    if (bestCorrelation < 0.2f)
        return { 0.0f, 0.0f };

    // Parabolic interpolation (keep your current behavior)
    float finalPeriod = (float)bestPeriod;

    if (bestPeriod > minPeriod && bestPeriod < maxPeriod - 1)
    {
        float prevCorr = 0.0f;
        float nextCorr = 0.0f;

        const int limitPrev = numSamples - (bestPeriod - 1);
        const int limitNext = numSamples - (bestPeriod + 1);

        for (int i = 0; i < limitPrev; ++i)
            prevCorr += signal[i] * signal[i + (bestPeriod - 1)];

        for (int i = 0; i < limitNext; ++i)
            nextCorr += signal[i] * signal[i + (bestPeriod + 1)];

        const float denominator = prevCorr - 2.0f * bestCorrelation + nextCorr;

        if (std::abs(denominator) > 0.00001f)
        {
            const float delta = (prevCorr - nextCorr) / (2.0f * denominator);
            finalPeriod = bestPeriod - delta;
        }
    }

    return { (float)(sampleRate / finalPeriod), bestCorrelation };
}
