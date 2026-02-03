#pragma once

#include <JuceHeader.h>
#include "../../Core/PluginProcessor.h"
#include "../../Core/Constants.h"
#include <deque>
#include <vector>

class TunerOverlay : public juce::Component, public juce::Timer
{
public:
    explicit TunerOverlay(NOVAAudioProcessor& p);
    ~TunerOverlay() override;

    void timerCallback() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    // -------------------------------------------------------------------------
    // Model
    // -------------------------------------------------------------------------
    struct TuningPreset
    {
        juce::String name;
        std::vector<juce::String> stringNames;
        std::vector<float> frequencies;
    };

    void initPresets();
    void selectString(int index);

    void closeOverlayAndDisableTuner();
    void updateCompletionUI(bool allDone);

    // -------------------------------------------------------------------------
    // Filtering (median)
    // -------------------------------------------------------------------------
    static constexpr int kMedianSize = 9;
    std::deque<float> medianBuffer;

    float pushMedian(float newVal);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    NOVAAudioProcessor& processor;

    std::vector<TuningPreset> presets;
    int currentPresetIndex = 0;
    int currentStringIndex = 0;

    std::vector<bool> stringIsTuned;
    bool isTuningComplete = false;

    float tuningProgress = 0.0f;       // 0..100
    float currentDisplayCents = 0.0f;  // smoothed for needle
    float smoothedRMS = 0.0f;          // smoothed for "has signal"

    // -------------------------------------------------------------------------
    // UI
    // -------------------------------------------------------------------------
    juce::TextButton closeButton;
    juce::ComboBox tuningSelector;
    juce::TextButton resetButton;
    juce::TextButton startPlayingButton;

    juce::Rectangle<int> stringBarArea;
};
