#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// Helper para botones arrastrables (Se mantiene igual)
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

    // Función helper para dibujar tiras de canal (Input/Output)
    void drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);

    NOVAAudioProcessor& audioProcessor;

    // --- HEADER ---
    juce::TextButton btnStartStop; // Engine Button
    // Placeholders Header
    juce::TextButton btnTuner{ "T" };
    juce::TextButton btnMetronome{ "M" };
    juce::Label lblCPU{ "CPU: 1%" };
    juce::Label lblLatency{ "LATENCY" };
    juce::TextButton btnSettings{ "Set" };
    juce::TextButton btnCart{ "Cart" };

    // --- LEFT COLUMN 1: BROWSER ---
    juce::TextEditor searchBarBrowser;
    DraggableButton btnAddOverdrive{ "Overdrive" };
    DraggableButton btnAddCabinet{ "Cabinet" };
    DraggableButton btnAddNeural{ "Neural Amp" }; // Placeholder futuro

    // --- LEFT COLUMN 2: INPUT STRIP ---
    juce::ComboBox inputDeviceSelector;
    juce::Slider inputVolume;
    juce::Slider inputGain;
    juce::Slider inputTranspose;
    juce::ToggleButton btnMonoStereo{ "Mono/Stereo" };
    juce::Slider inputFader; // Vertical

    // --- CENTER: MIXER (Ahora bajo las cadenas) ---
    juce::TextButton btnSwitcher;
    // Line A Controls
    juce::Slider volSliderA;
    juce::Slider trebleSliderA, bassSliderA; // Placeholders visuales
    // Line B Controls
    juce::Slider volSliderB;
    juce::Slider trebleSliderB, bassSliderB; // Placeholders visuales

    // --- RIGHT COLUMN 1: OUTPUT STRIP ---
    juce::ComboBox outputDeviceSelector;
    juce::Slider outputVolume;
    juce::Slider outputGain;
    juce::Slider outputFader; // Vertical

    // --- RIGHT COLUMN 2: PRESETS ---
    juce::TextEditor searchBarPresets;
    juce::TextButton btnSave{ "SAVE" };
    juce::TextButton btnLoad{ "LOAD" };
    juce::ListBox presetList; // Placeholder visual

    // --- CARRILES ---
    std::unique_ptr<ChainLane> laneA;
    std::unique_ptr<ChainLane> laneB;

    // --- VISUALIZADORES Y MODALES ---
    juce::OwnedArray<juce::AudioProcessorEditor> activePedalEditors;
    std::unique_ptr<juce::Component> currentOverlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};