#include "DropZone.h"
// Incluimos el Editor SOLO para poder llamar a showOverlay con cast dinámico
#include "../../Core/PluginEditor.h" 

DropZone::DropZone(NOVAAudioProcessor& p, Nova::ChainID c, Nova::ZoneID z)
    : proc(p), chain(c), zone(z)
{
}

bool DropZone::isInterestedInDragSource(const SourceDetails&)
{
    // Los slots de Amp y Cabinet son fijos, no aceptan drag and drop directo (por ahora)
    if (zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet) return false;
    return true;
}

void DropZone::itemDropped(const SourceDetails& d)
{
    isHover = false; repaint();
    // Añadir pedal al motor
    proc.requestAddPedal(d.description.toString(), chain, zone);
}

void DropZone::itemDragEnter(const SourceDetails&) { isHover = true; repaint(); }
void DropZone::itemDragExit(const SourceDetails&) { isHover = false; repaint(); }

void DropZone::mouseDown(const juce::MouseEvent& e)
{
    // Si clickamos en un slot fijo (Amp/Cab), abrimos el browser
    if ((zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet) && e.mods.isLeftButtonDown())
    {
        if (auto* mainEditor = findParentComponentOfClass<NOVAAudioProcessorEditor>())
        {
            mainEditor->showOverlay(zone, chain);
        }
    }
}

void DropZone::paint(juce::Graphics& g)
{
    bool isFixedSlot = (zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet);

    // Rejilla estilo "Tech" para zonas vacías
    if (!isFixedSlot) {
        g.setColour(Nova::Colors::GridLine.withAlpha(0.2f));
        for (int x = 0; x < getWidth(); x += 20) g.drawVerticalLine(x, 0.0f, (float)getHeight());
        g.setColour(Nova::Colors::GridLine.withAlpha(0.1f));
        for (int y = 0; y < getHeight(); y += 20) g.drawHorizontalLine(y, 0.0f, (float)getWidth());
    }

    g.setColour(Nova::Colors::ZoneOutline.withAlpha(isFixedSlot ? 0.4f : 0.2f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2), 6.0f, 1.0f);

    if (isFixedSlot) {
        // Estilo Amp/Cab placeholder
        if (isMouseOver(true) && !isHover) g.setColour(juce::Colours::white.withAlpha(0.2f));
        else g.setColour(juce::Colours::white.withAlpha(0.05f));

        auto center = getLocalBounds().getCentre().toFloat();
        g.fillEllipse(center.x - 25, center.y - 25, 50, 50);

        // Icono +
        g.setColour(juce::Colours::grey);
        g.fillRect(center.x - 1.5f, center.y - 12, 3.0f, 24.0f);
        g.fillRect(center.x - 12, center.y - 1.5f, 24.0f, 3.0f);

        g.setFont(12.0f);
        g.drawText(zone == Nova::ZoneID::Amp ? "ADD AMP" : "ADD CAB", getLocalBounds().removeFromBottom(30), juce::Justification::centred);
    }
    else {
        // Estilo Dropzone normal
        juce::String label = (zone == Nova::ZoneID::Pre) ? "PRE-FX" : "FX LOOP";
        g.setColour(juce::Colours::grey.withAlpha(0.3f)); g.setFont(12.f);
        g.drawText(label, getLocalBounds().removeFromBottom(25), juce::Justification::centred);

        if (isHover) {
            g.setColour(Nova::Colors::CableOnA.withAlpha(0.2f));
            g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.f);
            g.setColour(Nova::Colors::CableOnA);
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1), 6.f, 2.0f);
        }
    }
}