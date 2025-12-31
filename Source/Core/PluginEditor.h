#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// ==============================================================================
// DEFINICIÓN DE BOTÓN ARRASTRABLE
// ==============================================================================
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
// EDITOR PRINCIPAL
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

private:
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { updatePedalGui(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { updatePedalGui(); }

    void updatePedalGui();
    void updateSwitcherState();

    NOVAAudioProcessor& audioProcessor;

    // Controles UI
    juce::TextButton btnStartStop;
    juce::TextButton btnSwitcher;

    // Paleta
    DraggableButton btnAddOverdrive{ "Overdrive" };
    DraggableButton btnAddCabinet{ "Cabinet" };

    // Carriles
    std::unique_ptr<ChainLane> laneA;
    std::unique_ptr<ChainLane> laneB;

    // Lista de editores activos
    juce::OwnedArray<juce::AudioProcessorEditor> activePedalEditors;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};