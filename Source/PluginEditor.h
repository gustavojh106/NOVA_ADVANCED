#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
// Clase auxiliar que envuelve un pedal para añadirle el menú contextual
class PedalWrapper : public juce::Component
{
public:
    PedalWrapper(juce::AudioProcessorEditor* editorToWrap, int index, NOVAAudioProcessor& p)
        : pedalIndex(index), processor(p)
    {
        // Guardamos el editor real dentro
        editor.reset(editorToWrap);
        addAndMakeVisible(editor.get());

        // Configuramos tamaño
        setSize(editor->getWidth(), editor->getHeight());
        addMouseListener(this, true);
    }

    void resized() override
    {
        if (editor) editor->setBounds(getLocalBounds());
    }

    // AQUÍ ESTÁ LA MAGIA DEL CLIC DERECHO
    void mouseDown(const juce::MouseEvent& e) override
    {
        // Si es clic derecho (o Ctrl+Clic en Mac)
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem(1, "Eliminar Pedal / Remove"); // ID 1

            // Mostramos el menú de forma asíncrona
            m.showMenuAsync(juce::PopupMenu::Options(), [this](int result)
                {
                    if (result == 1)
                    {
                        // Llamamos a la función de borrado del procesador
                        processor.requestRemovePedal(pedalIndex);
                    }
                });
        }
    }

private:
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    int pedalIndex;
    NOVAAudioProcessor& processor;
};
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
    juce::OwnedArray<PedalWrapper> activeEditors;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};