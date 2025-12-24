/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
NOVAAudioProcessorEditor::NOVAAudioProcessorEditor(NOVAAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(800, 600); // Ventana grande para que quepa todo

    // --- BÚSQUEDA DEL PEDAL EN EL GRAFO ---
    // Recorremos todos los nodos que existen en el "Board" (Main Graph)
    for (auto* node : audioProcessor.mainGraph->getNodes())
    {
        // Obtenemos el procesador real dentro del nodo
        auto* processor = node->getProcessor();

        // Filtramos: No queremos dibujar los nodos de "Entrada/Salida" de la tarjeta, 
        // solo queremos nuestro efecto (que llamamos "Overdrive" en getName())
        if (processor->getName() == "Overdrive")
        {
            // ¡Lo encontramos! Creamos su editor (la cajita verde)
            if (auto* pedalEditor = processor->createEditor())
            {
                // Lo agregamos a la ventana principal
                addAndMakeVisible(pedalEditor);

                // Lo posicionamos en el centro
                pedalEditor->setBounds(300, 150, 200, 300);

                // IMPORTANTE: JUCE maneja la memoria de los componentes hijos automáticamente
                // si usamos addAndMakeVisible, pero como createEditor devuelve un puntero 'raw' nuevo,
                // idealmente deberíamos guardarlo en un std::unique_ptr para gestionarlo.
                // PARA ESTE TEST RÁPIDO: Lo dejamos así (leak menor al cerrar), 
                // pero en producción usaremos un vector de editores activos.
            }
        }
    }
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
}

//==============================================================================
void NOVAAudioProcessorEditor::paint (juce::Graphics& g)
{
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    //g.setColour (juce::Colours::white);
    //g.setFont (juce::FontOptions (15.0f));
    //g.drawFittedText ("Hello World!", getLocalBounds(), juce::Justification::centred, 1);
}

void NOVAAudioProcessorEditor::resized()
{
    // This is generally where you'll want to lay out the positions of any
    // subcomponents in your editor..
}
