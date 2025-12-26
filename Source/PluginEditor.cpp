#include "PluginProcessor.h"
#include "PluginEditor.h"

NOVAAudioProcessorEditor::NOVAAudioProcessorEditor(NOVAAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(1000, 600);

    addAndMakeVisible(btnAddOverdrive);
    addAndMakeVisible(btnAddNeural);
    addAndMakeVisible(btnAddCabinet);

    // 1. Nos suscribimos a los cambios del modelo
    audioProcessor.pluginState.addListener(this);

    // 2. Carga inicial (por si abrimos el plugin y ya había pedales cargados)
    updatePedalGui();
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
    // IMPORTANTE: Desuscribirse siempre al destruir para evitar crash
    audioProcessor.pluginState.removeListener(this);
}

void NOVAAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    // Panel Lateral
    g.setColour(juce::Colours::darkgrey);
    g.fillRect(0, 0, 150, getHeight());

    g.setColour(juce::Colours::white);
    g.drawVerticalLine(150, 0.0f, (float)getHeight());
}

void NOVAAudioProcessorEditor::resized()
{
    // Layout de botones laterales
    int x = 10, w = 130, h = 40, padding = 10;
    btnAddOverdrive.setBounds(x, 20, w, h);
    btnAddNeural.setBounds(x, 20 + h + padding, w, h);
    btnAddCabinet.setBounds(x, 20 + (h + padding) * 2, w, h);

    // Layout de los Pedales (Flow Horizontal)
    int currentX = 160;
    int pedalY = 20;

    for (auto* editor : activeEditors)
    {
        editor->setBounds(currentX, pedalY, editor->getWidth(), editor->getHeight());
        currentX += editor->getWidth() + 10;
    }
}

// ==============================================================================
// Lógica Reactiva (SOTA)
// ==============================================================================

bool NOVAAudioProcessorEditor::isInterestedInDragSource(const SourceDetails& dragSourceDetails)
{
    return true;
}

void NOVAAudioProcessorEditor::itemDropped(const SourceDetails& dragSourceDetails)
{
    // EL CAMBIO SOTA:
    // No modificamos la UI ni el Grafo directamente.
    // Solo le pedimos al Processor que cambie el Estado.
    juce::String pedalType = dragSourceDetails.description.toString();
    audioProcessor.requestAddPedal(pedalType);

    // Y NO hacemos updatePedalGui() aquí. Esperamos la confirmación del Listener.
}

void NOVAAudioProcessorEditor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    // El Processor ya actualizó el grafo de audio. Ahora nosotros actualizamos la vista.
    // Usamos MessageManagerLock implícito (juce::Timer::callAsync) si fuera necesario,
    // pero los callbacks de ValueTree suelen ser síncronos en el hilo principal si se disparan desde ahí.
    updatePedalGui();
}

void NOVAAudioProcessorEditor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    updatePedalGui();
}

void NOVAAudioProcessorEditor::updatePedalGui()
{
    // 1. Limpiamos visualmente
    activeEditors.clear();

    // 2. Obtenemos la VERDAD desde el Processor (los nodos reales que están sonando)
    const auto& nodes = audioProcessor.getNodes();

    // 3. Recreamos los editores
    for (auto node : nodes)
    {
        if (node != nullptr)
        {
            auto* processor = node->getProcessor();
            if (processor && processor->hasEditor())
            {
                if (auto* editor = processor->createEditor())
                {
                    addAndMakeVisible(editor);
                    activeEditors.add(editor);
                }
            }
        }
    }

    // 4. Reacomodamos
    resized();
}