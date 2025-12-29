#include "PluginProcessor.h"
#include "PluginEditor.h"

NOVAAudioProcessorEditor::NOVAAudioProcessorEditor(NOVAAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(1000, 650); // Aumentamos un poco la altura para el header
    addAndMakeVisible(audioProcessor.audioVisualizer);
    addAndMakeVisible(btnAddOverdrive);
    //addAndMakeVisible(btnAddNeural);
    addAndMakeVisible(btnAddCabinet);

    // 1. Nos suscribimos a los cambios del modelo
    audioProcessor.pluginState.addListener(this);

    // 2. Carga inicial (por si abrimos el plugin y ya habia pedales cargados)
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
    g.setColour(juce::Colours::white.withAlpha(0.8f));
    g.setFont(30.0f);
    g.drawText("NOVA", 20, 0, 100, 80, juce::Justification::centredLeft);
    g.setColour(juce::Colours::white);
    g.drawVerticalLine(150, 0.0f, (float)getHeight());
}

void NOVAAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    // 1. HEADER (Visualizador) - 80px de alto en la cima
    auto headerArea = area.removeFromTop(80);
    audioProcessor.audioVisualizer.setBounds(headerArea);

    // 2. BARRA LATERAL (Resto del área a la izquierda)
    auto sideBar = area.removeFromLeft(150);

    int w = 130, h = 40, padding = 10;
    int btnX = sideBar.getX() + 10;

    btnAddOverdrive.setBounds(btnX, sideBar.getY() + 20, w, h);
    btnAddNeural.setBounds(btnX, sideBar.getY() + 20 + h + padding, w, h);
    btnAddCabinet.setBounds(btnX, sideBar.getY() + 20 + (h + padding) * 2, w, h);

    // 3. ZONA DE PEDALES (Lo que sobra)
    int currentX = area.getX() + 10;
    int pedalY = area.getY() + 20;

    for (auto* editor : activeEditors)
    {
        editor->setBounds(currentX, pedalY, editor->getWidth(), editor->getHeight());
        currentX += editor->getWidth() + 10;
    }
}
// ==============================================================================
// La Reactiva (SOTA)
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

    // Y NO hacemos updatePedalGui() aqu. Esperamos la confirmacin del Listener.
}

void NOVAAudioProcessorEditor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    // El Processor ya actualiz el grafo de audio. Ahora nosotros actualizamos la vista.
    // Usamos MessageManagerLock implcito (juce::Timer::callAsync) si fuera necesario,
    // pero los callbacks de ValueTree suelen ser sincronos en el hilo principal si se disparan desde ah.
    updatePedalGui();
}

void NOVAAudioProcessorEditor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    updatePedalGui();
}

void NOVAAudioProcessorEditor::updatePedalGui()
{
    // 1. Limpiamos
    activeEditors.clear();

    // 2. Obtenemos los nodos reales del audio
    const auto& nodes = audioProcessor.getAudioEngine().getActiveNodes();
    // 3. Iteramos con un ndice (necesario para saber cul borrar)
    int index = 0;
    for (auto node : nodes)
    {
        if (node != nullptr)
        {
            auto* processor = node->getProcessor();
            if (processor && processor->hasEditor())
            {
                // Creamos el editor del pedal
                if (auto* pedalEditor = processor->createEditor())
                {
                    // LO ENVOLVEMOS EN EL WRAPPER (Aqui pasamos el indice)
                    auto* wrapper = new PedalWrapper(pedalEditor, index, audioProcessor);

                    addAndMakeVisible(wrapper);
                    activeEditors.add(wrapper);
                }
            }
        }
        index++;
    }

    // 4. Reacomodamos la pantalla
    resized();
}