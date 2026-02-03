#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <vector>

#include "../../Constants.h"

class TunerService final
{
public:
    TunerService();
    ~TunerService() = default;

    // Called from the audio thread (processBlock)
    void pushBuffer(const juce::AudioBuffer<float>& buffer);

    // Called from a background thread (AudioEngine::run)
    void process();

    // Thread-safe getters (UI)
    float getCurrentPitch() const { return currentPitch.load(); }
    float getCurrentClarity() const { return currentClarity.load(); }
    float getCurrentRMS() const { return currentRMS.load(); }
    int   getCurrentNote() const { return currentNote.load(); }

    void reset();
    void setSampleRate(double rate) { sampleRate = rate; }

private:
    std::pair<float, float> calculateFrequencyWithClarity(const float* signal, int numSamples);

private:
    // Config
    double sampleRate = 44100.0;

    // FIFO + buffers
    juce::AbstractFifo tunerFifo{ Nova::Config::TUNER_FIFO_SIZE };
    std::vector<float> circularBuffer; // ring buffer input
    std::vector<float> workBuffer;     // contiguous analysis window

    // Results (written by background thread, read by UI)
    std::atomic<float> currentPitch{ 0.0f };
    std::atomic<float> currentClarity{ 0.0f };
    std::atomic<float> currentRMS{ 0.0f };
    std::atomic<int>   currentNote{ 0 };
};
