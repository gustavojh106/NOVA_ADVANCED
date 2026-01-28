#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Constants.h"               // <--- Para Nova::IDs y Colors
#include "../GUI/Overlays/TunerOverlay.h" // <--- Para el nuevo Afinador Modular

// Helper para botones arrastrables (Se mantiene aquí por ahora)
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

// ==============================================================================
// CLASE PRINCIPAL DEL EDITOR
// ==============================================================================
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
    // Helpers visuales
    void setupKnob(juce::Slider& slider, const juce::String& name, float min, float max, float def);
    void drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);

    // Callbacks de ValueTree
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { updatePedalGui(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { updatePedalGui(); }

    void updatePedalGui();
    void updateSwitcherState();
    void updateStats();

    NOVAAudioProcessor& audioProcessor;

    // --- HEADER ---
    juce::TextButton btnStartStop;
    juce::Label lblStats;

    juce::TextButton btnTuner{ "T" };
    juce::TextButton btnMetronome{ "M" };
    juce::TextButton btnSettings{ "Set" };
    juce::TextButton btnProfile{ "Profile" };

    // --- LEFT COLUMN 1: BROWSER ---
    juce::TextEditor searchBarBrowser;
    DraggableButton btnAddOverdrive{ "Overdrive" };
    DraggableButton btnAddCabinet{ "Cabinet" };
    DraggableButton btnAddNeural{ "Neural Amp" };

    // --- LEFT COLUMN 2: INPUT STRIP ---
    juce::Slider inputVolume;
    juce::Slider inputGate;
    juce::Slider inputTranspose;
    juce::ToggleButton btnMonoStereo{ "Mono/Stereo" };
    juce::Slider inputFader;

    // --- CENTER: MIXER ---
    juce::TextButton btnSwitcher;

    // Line A
    juce::Slider volSliderA;
    juce::Slider panSliderA;
    juce::Slider widthSliderA;

    // Line B
    juce::Slider volSliderB;
    juce::Slider panSliderB;
    juce::Slider widthSliderB;

    // --- RIGHT COLUMN 1: OUTPUT STRIP ---
    juce::Slider outputVolume;
    juce::Slider outputGain;
    juce::Slider outputMix;
    juce::Slider outputFader;

    // --- RIGHT COLUMN 2: PRESETS ---
    juce::TextEditor searchBarPresets;
    juce::TextButton btnSave{ "SAVE" };
    juce::TextButton btnLoad{ "LOAD" };

    // --- CARRILES ---
    std::unique_ptr<ChainLane> laneA;
    std::unique_ptr<ChainLane> laneB;

    // --- VISUALIZADORES Y MODALES ---
    std::map<juce::AudioProcessorGraph::NodeID, std::unique_ptr<juce::AudioProcessorEditor>> activePedalEditors;
    std::unique_ptr<juce::Component> currentOverlay;

    // --- TUNER (Modularizado) ---
    std::unique_ptr<TunerOverlay> tunerOverlay; // <--- Reemplaza a tunerDisplay

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