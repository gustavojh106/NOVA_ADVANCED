#pragma once
#include <JuceHeader.h>
#include "../../Core/PluginProcessor.h"
#include "../../Core/Constants.h"
#include <deque>
#include <vector>

class TunerOverlay : public juce::Component, public juce::Timer
{
public:
    TunerOverlay(NOVAAudioProcessor& p);
    ~TunerOverlay() override;

    void timerCallback() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    NOVAAudioProcessor& processor;

    // --- Lógica de Suavizado y Mediana ---
    static constexpr int MEDIAN_SIZE = 9;
    static constexpr int SMOOTHING_BUFFER_SIZE = 12;

    std::deque<float> medianBuffer;
    std::vector<float> centsHistory;
    int historyIndex = 0;

    float getMedianCents(float newVal);
    float getSmoothedCents(float newCents);

    // --- Estado de Afinación ---
    float tuningProgress = 0.0f; // 0.0 a 100.0
    float currentDisplayCents = 0.0f;
    float smoothedRMS = 0.0f;

    // Datos de Cuerdas
    struct TuningPreset {
        juce::String name;
        std::vector<juce::String> stringNames;
        std::vector<float> frequencies;
    };
    std::vector<TuningPreset> presets;
    int currentPresetIndex = 0;
    int currentStringIndex = 0;

    std::vector<bool> stringIsTuned;
    bool isTuningComplete = false;

    // Métodos Helper
    void initPresets();
    void selectString(int index);

    // --- Elementos UI ---
    juce::TextButton closeButton;
    juce::ComboBox tuningSelector;
    juce::TextButton resetButton;
    juce::TextButton startPlayingButton;
    juce::Rectangle<int> stringBarArea;
};