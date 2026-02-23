#pragma once
#include <JuceHeader.h>

class DraggableButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton; // Heredar constructores

    // NUEVO: Método para asignarle el tipo (PEDAL, AMP, CAB) desde la UI principal
    void setItemType(const juce::String& type) { itemType = type; }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        // Usar findParentDragContainerFor es la forma más segura en JUCE
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        {
            // Formateamos el string final. Ejemplo: "PEDAL:Overdrive" o "AMP:Neural Amp"
            juce::String dragDescription = itemType + ":" + getButtonText();
            container->startDragging(dragDescription, this);
        }
    }

private:
    juce::String itemType = "PEDAL"; // Pedal por defecto por si olvidas setearlo
};