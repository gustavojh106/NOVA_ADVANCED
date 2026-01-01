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

    // --- NUEVO: Función pública para abrir el modal ---
    void showOverlay(Nova::ZoneID zone, Nova::ChainID chain);

private:
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { updatePedalGui(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { updatePedalGui(); }

    void updatePedalGui();
    void updateSwitcherState();

    NOVAAudioProcessor& audioProcessor;

    // --- MIXER FOOTER ---
    juce::TextButton btnStartStop;
    juce::TextButton btnSwitcher;

    // Knobs de Volumen
    juce::Slider volSliderA, volSliderB;
    juce::Label  volLabelA, volLabelB;

    // --- PALETA ---
    DraggableButton btnAddOverdrive{ "Overdrive" };
    DraggableButton btnAddCabinet{ "Cabinet" };

    // --- CARRILES ---
    std::unique_ptr<ChainLane> laneA;
    std::unique_ptr<ChainLane> laneB;

    // Almacenamiento de editores
    juce::OwnedArray<juce::AudioProcessorEditor> activePedalEditors;

    // --- NUEVO: Variable para el Modal (Overlay) ---
    std::unique_ptr<juce::Component> currentOverlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};