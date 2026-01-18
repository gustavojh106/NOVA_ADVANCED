#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <deque> 
#include <algorithm>
//class NOVAAudioProcessorEditor;
// 

// Helper para botones arrastrables
class DraggableButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (auto* container = findParentComponentOfClass<juce::DragAndDropContainer>())
            container->startDragging(getButtonText(), this);
    }
};

class ChainLane; // Forward declaration
class TunerDisplay : public juce::Component, public juce::Timer
{
public:
    TunerDisplay(NOVAAudioProcessor& p);
    ~TunerDisplay() override;

    void timerCallback() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    std::deque<float> medianBuffer;
    static constexpr int MEDIAN_SIZE = 9; // Tamaño impar es mejor
    float tuningProgress = 0.0f; // Va de 0.0 a 100.0 (sustituye a framesInTune directo)
    float getMedianCents(float newVal);
    NOVAAudioProcessor& processor;

    struct TuningPreset {
        juce::String name;
        std::vector<juce::String> stringNames;
        std::vector<float> frequencies;
    };

    std::vector<TuningPreset> presets;
    int currentPresetIndex = 0;
    int currentStringIndex = 0;

    // Estado
    std::vector<bool> stringIsTuned;
    bool isTuningComplete = false; // Nueva bandera de finalización

    // Lógica de Estabilidad (Auto-Advance)
    int framesInTune = 0;    // Contador de tiempo afinado

    // Lógica de Suavizado (Buffer Circular para el indicador)
    static constexpr int SMOOTHING_BUFFER_SIZE = 12; // Tamaño del promedio (ajustable)
    std::vector<float> centsHistory;
    int historyIndex = 0;

    // UI Elements
    juce::TextButton closeButton;
    juce::ComboBox tuningSelector;
    juce::Rectangle<int> stringBarArea;
    juce::TextButton resetButton; // Botón para reiniciar el proceso
    juce::TextButton startPlayingButton; // "START PLAYING" (Nuevo)

    // Physics Visuales
    float currentDisplayCents = 0.0f;
    float smoothedRMS = 0.0f;

    void initPresets();
    void selectString(int index);

    // Nueva función para obtener el valor suavizado
    float getSmoothedCents(float newCents);
};
class NOVAAudioProcessorEditor : public juce::AudioProcessorEditor,
    public juce::ValueTree::Listener,
    public juce::DragAndDropContainer
{
public:
    NOVAAudioProcessorEditor(NOVAAudioProcessor&);
    ~NOVAAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    void showOverlay(Nova::ZoneID zone, Nova::ChainID chain);
    void toggleTuner(); 
private:
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { updatePedalGui(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { updatePedalGui(); }

    void updatePedalGui();
    void updateSwitcherState();
    void updateStats(); // Actualizar etiqueta CPU

    // Función helper para dibujar tiras de canal (Input/Output)
    void drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);

    NOVAAudioProcessor& audioProcessor;

    // --- HEADER ---
    juce::TextButton btnStartStop;
    juce::Label lblStats; // Fusión CPU/Latency

    // Placeholders Header
    juce::TextButton btnTuner{ "T" };
    juce::TextButton btnMetronome{ "M" };
    juce::TextButton btnSettings{ "Set" };
    juce::TextButton btnProfile{ "Profile" }; // Reemplaza Cart

    // --- LEFT COLUMN 1: BROWSER ---
    juce::TextEditor searchBarBrowser;
    DraggableButton btnAddOverdrive{ "Overdrive" };
    DraggableButton btnAddCabinet{ "Cabinet" };
    DraggableButton btnAddNeural{ "Neural Amp" };

    // --- LEFT COLUMN 2: INPUT STRIP ---
    // (Eliminado DeviceSelector)
    juce::Slider inputVolume;
    juce::Slider inputGate;      // NUEVO: Noise Gate
    juce::Slider inputTranspose; // Mantenido
    juce::ToggleButton btnMonoStereo{ "Mono/Stereo" };
    juce::Slider inputFader;

    // --- CENTER: MIXER ---
    juce::TextButton btnSwitcher;

    // Line A Controls (EQ cambiado por Pan/Width)
    juce::Slider volSliderA;
    juce::Slider panSliderA;
    juce::Slider widthSliderA;

    // Line B Controls
    juce::Slider volSliderB;
    juce::Slider panSliderB;
    juce::Slider widthSliderB;

    // --- RIGHT COLUMN 1: OUTPUT STRIP ---
    // (Eliminado DeviceSelector y Transpose)
    juce::Slider outputVolume;
    juce::Slider outputGain;
    juce::Slider outputMix;    // NUEVO: Mix Global
    juce::Slider outputFader;

    // --- RIGHT COLUMN 2: PRESETS ---
    juce::TextEditor searchBarPresets;
    juce::TextButton btnSave{ "SAVE" };
    juce::TextButton btnLoad{ "LOAD" };
    // juce::ListBox presetList; (Opcional, placeholder visual dibujado en paint)

    // --- CARRILES ---
    std::unique_ptr<ChainLane> laneA;
    std::unique_ptr<ChainLane> laneB;

    // --- VISUALIZADORES Y MODALES ---
    //juce::OwnedArray<juce::AudioProcessorEditor> activePedalEditors;
    std::map<juce::AudioProcessorGraph::NodeID, std::unique_ptr<juce::AudioProcessorEditor>> activePedalEditors;
    std::unique_ptr<juce::Component> currentOverlay;

    // Timer para stats
    class StatsTimer : public juce::Timer {
        NOVAAudioProcessorEditor& parent;
    public:
        StatsTimer(NOVAAudioProcessorEditor& p) : parent(p) { startTimer(500); }
        void timerCallback() override { parent.updateStats(); }
    };
    std::unique_ptr<StatsTimer> statsTimer;
    std::unique_ptr<TunerDisplay> tunerDisplay; // Variable del componente

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};