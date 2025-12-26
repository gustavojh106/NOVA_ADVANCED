#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

// Clase auxiliar para botones arrastrables (se mantiene igual, es perfecta)
class DraggableButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (auto* container = findParentComponentOfClass<juce::DragAndDropContainer>())
        {
            container->startDragging(getButtonText(), this);
        }
    }
};

// ==============================================================================
// CLASE PRINCIPAL DEL EDITOR (SOTA: Reactiva al ValueTree)
// ==============================================================================
class NOVAAudioProcessorEditor : public juce::AudioProcessorEditor,
    public juce::DragAndDropContainer,
    public juce::DragAndDropTarget,
    public juce::ValueTree::Listener // <--- NUEVO: Oídos del sistema
{
public:
    NOVAAudioProcessorEditor(NOVAAudioProcessor&);
    ~NOVAAudioProcessorEditor() override;

    // Gráficos
    void paint(juce::Graphics&) override;
    void resized() override;

    // Drag & Drop
    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override;
    void itemDropped(const SourceDetails& dragSourceDetails) override;

private:
    // Callbacks del ValueTree (Aquí reaccionamos a los cambios)
    void valueTreeChildAdded(juce::ValueTree& parentTree, juce::ValueTree& childWhichHasBeenAdded) override;
    void valueTreeChildRemoved(juce::ValueTree& parentTree, juce::ValueTree& childWhichHasBeenRemoved, int moveFromIndex) override;

    // Función para reconstruir la UI
    void updatePedalGui();

    NOVAAudioProcessor& audioProcessor;

    // Botones de la paleta
    DraggableButton btnAddOverdrive{ "Overdrive" };
    DraggableButton btnAddCabinet{ "Cabinet" };
    DraggableButton btnAddNeural{ "Neural" };

    // Lista de editores de los pedales (Solo para visualización)
    juce::OwnedArray<juce::AudioProcessorEditor> activeEditors;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};