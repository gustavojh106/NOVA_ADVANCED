#pragma once
#include <JuceHeader.h>

class DraggableButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton; // Heredar constructores

    void mouseDrag(const juce::MouseEvent& e) override
    {
        // Si este botón está dentro de un contenedor Drag&Drop, inicia el arrastre
        if (auto* container = findParentComponentOfClass<juce::DragAndDropContainer>())
        {
            container->startDragging(getButtonText(), this);
        }
    }
};