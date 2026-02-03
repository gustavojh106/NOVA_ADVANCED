#include "DropZone.h"
#include "../../Core/PluginEditor.h"

DropZone::DropZone(NOVAAudioProcessor& processor, Nova::ChainID chainId, Nova::ZoneID zoneId)
    : proc(processor), chain(chainId), zone(zoneId)
{
}

bool DropZone::isFixedSlot() const noexcept
{
    return zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet;
}

void DropZone::setHover(bool shouldHover)
{
    if (isHover == shouldHover)
        return;

    isHover = shouldHover;
    repaint();
}

bool DropZone::isInterestedInDragSource(const SourceDetails&)
{
    // Slots fijos (Amp/Cab) no aceptan drag and drop directo
    return !isFixedSlot();
}

void DropZone::itemDropped(const SourceDetails& details)
{
    setHover(false);

    // Añadir pedal al motor (solo aplica para zonas no fijas, porque las fijas no se interesan)
    proc.requestAddPedal(details.description.toString(), chain, zone);
}

void DropZone::itemDragEnter(const SourceDetails&) { setHover(true); }
void DropZone::itemDragExit(const SourceDetails&) { setHover(false); }

void DropZone::mouseDown(const juce::MouseEvent& e)
{
    if (!isFixedSlot() || !e.mods.isLeftButtonDown())
        return;

    if (auto* mainEditor = findParentComponentOfClass<NOVAAudioProcessorEditor>())
        mainEditor->showOverlay(zone, chain);
}

void DropZone::drawTechGrid(juce::Graphics& g) const
{
    g.setColour(Nova::Colors::GridLine.withAlpha(0.2f));
    for (int x = 0; x < getWidth(); x += 20)
        g.drawVerticalLine(x, 0.0f, (float)getHeight());

    g.setColour(Nova::Colors::GridLine.withAlpha(0.1f));
    for (int y = 0; y < getHeight(); y += 20)
        g.drawHorizontalLine(y, 0.0f, (float)getWidth());
}

void DropZone::drawOutline(juce::Graphics& g, bool fixed) const
{
    g.setColour(Nova::Colors::ZoneOutline.withAlpha(fixed ? 0.4f : 0.2f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 6.0f, 1.0f);
}

void DropZone::drawFixedSlotUI(juce::Graphics& g) const
{
    const bool mouseOver = isMouseOver(true);
    g.setColour(juce::Colours::white.withAlpha((mouseOver && !isHover) ? 0.2f : 0.05f));

    auto center = getLocalBounds().getCentre().toFloat();
    g.fillEllipse(center.x - 25.0f, center.y - 25.0f, 50.0f, 50.0f);

    // Icono "+"
    g.setColour(juce::Colours::grey);
    g.fillRect(center.x - 1.5f, center.y - 12.0f, 3.0f, 24.0f);
    g.fillRect(center.x - 12.0f, center.y - 1.5f, 24.0f, 3.0f);

    g.setFont(12.0f);
    const auto text = (zone == Nova::ZoneID::Amp) ? "ADD AMP" : "ADD CAB";
    g.drawText(text, getLocalBounds().removeFromBottom(30), juce::Justification::centred);
}

void DropZone::drawNormalSlotUI(juce::Graphics& g) const
{
    const auto label = (zone == Nova::ZoneID::Pre) ? "PRE-FX" : "FX LOOP";

    g.setColour(juce::Colours::grey.withAlpha(0.3f));
    g.setFont(12.0f);
    g.drawText(label, getLocalBounds().removeFromBottom(25), juce::Justification::centred);

    if (!isHover)
        return;

    g.setColour(Nova::Colors::CableOnA.withAlpha(0.2f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);

    g.setColour(Nova::Colors::CableOnA);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 2.0f);
}

void DropZone::paint(juce::Graphics& g)
{
    const bool fixed = isFixedSlot();

    // Grid solo en zonas no fijas
    if (!fixed)
        drawTechGrid(g);

    drawOutline(g, fixed);

    if (fixed)
        drawFixedSlotUI(g);
    else
        drawNormalSlotUI(g);
}
