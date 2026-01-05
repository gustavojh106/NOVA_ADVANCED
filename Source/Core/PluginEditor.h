#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

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
    juce::OwnedArray<juce::AudioProcessorEditor> activePedalEditors;
    std::unique_ptr<juce::Component> currentOverlay;

    // Timer para stats
    class StatsTimer : public juce::Timer {
        NOVAAudioProcessorEditor& parent;
    public:
        StatsTimer(NOVAAudioProcessorEditor& p) : parent(p) { startTimer(500); }
        void timerCallback() override { parent.updateStats(); }
    };
    std::unique_ptr<StatsTimer> statsTimer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};