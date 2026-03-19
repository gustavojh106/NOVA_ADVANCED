#include "ChainLane.h"

namespace
{
    constexpr int kFixedZoneWidth = 240; // Amp y Cab
    constexpr float kCableShadowThickness = 10.0f;
    constexpr float kCableBodyThickness = 6.0f;
    constexpr float kCableSignalOn = 2.0f;
    constexpr float kCableSignalOff = 1.0f;
    constexpr float kCableGlowThickness = 8.0f;

    const juce::Colour kCableBodyColour = juce::Colour::fromString("ff111827");
}

ChainLane::ChainLane(NOVAAudioProcessor& p, Nova::ChainID c)
    : chainID(c)
{
    // Zonas obligatorias en orden
    zones.add(new DropZone(p, c, Nova::ZoneID::Pre));
    zones.add(new DropZone(p, c, Nova::ZoneID::Amp));
    zones.add(new DropZone(p, c, Nova::ZoneID::FX));
    zones.add(new DropZone(p, c, Nova::ZoneID::Cabinet));

    for (auto* z : zones)
        addAndMakeVisible(z);
}

void ChainLane::setActive(bool shouldBeActive)
{
    isLaneActive = shouldBeActive;
    repaint();
}

juce::Rectangle<int> ChainLane::getZoneRect(int zoneIndex) const
{
    if (juce::isPositiveAndBelow(zoneIndex, zones.size()))
        return zones[zoneIndex]->getBounds();

    return {};
}

juce::Colour ChainLane::getCableGlowColour() const noexcept
{
    if (!isLaneActive)
        return Nova::Colors::CableOff;

    return (chainID == Nova::ChainID::LineA) ? Nova::Colors::CableOnA
        : Nova::Colors::CableOnB;
}

void ChainLane::resized()
{
    // 1. Reducimos el �rea general para dar un margen de 4px
    auto area = getLocalBounds().reduced(4);
    const int gap = 8; // ESTE ES EL ESPACIO ENTRE ZONAS

    // 2. Calculamos el ancho restante quitando los 3 "gaps"
    const int totalW = area.getWidth() - (gap * 3);
    const int fixedTotal = kFixedZoneWidth * 2;
    const int flexZoneW = juce::jmax(0, totalW - fixedTotal) / 2;

    // 3. Vamos asignando las �reas y "saltando" el gap
    auto rPre = area.removeFromLeft(flexZoneW);
    area.removeFromLeft(gap); // Dejamos hueco

    auto rAmp = area.removeFromLeft(kFixedZoneWidth);
    area.removeFromLeft(gap); // Dejamos hueco

    auto rFx = area.removeFromLeft(flexZoneW);
    area.removeFromLeft(gap); // Dejamos hueco

    auto rCab = area.removeFromLeft(kFixedZoneWidth);

    // 4. Aplicamos
    zones[0]->setBounds(rPre);
    zones[1]->setBounds(rAmp);
    zones[2]->setBounds(rFx);
    zones[3]->setBounds(rCab);
}

void ChainLane::paint(juce::Graphics& g)
{
    const float y = getHeight() * 0.5f;
    const auto glow = getCableGlowColour();

    // Build cable path only through the gaps BETWEEN zones (not through zone interiors)
    juce::Path cable;

    // Segment: left edge → Pre zone start
    if (zones.size() > 0)
    {
        cable.startNewSubPath(0.0f, y);
        cable.lineTo((float)zones[0]->getX(), y);
    }

    // Segments between consecutive zones
    for (int i = 0; i + 1 < zones.size(); ++i)
    {
        const float gapStart = (float)zones[i]->getRight();
        const float gapEnd   = (float)zones[i + 1]->getX();
        cable.startNewSubPath(gapStart, y);
        cable.lineTo(gapEnd, y);
    }

    // Segment: last zone end → right edge
    if (zones.size() > 0)
    {
        cable.startNewSubPath((float)zones.getLast()->getRight(), y);
        cable.lineTo((float)getWidth(), y);
    }

    // Shadow
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.strokePath(cable, juce::PathStrokeType(kCableShadowThickness));

    // Body
    g.setColour(kCableBodyColour);
    g.strokePath(cable, juce::PathStrokeType(kCableBodyThickness));

    // Signal
    g.setColour(glow);
    g.strokePath(cable, juce::PathStrokeType(isLaneActive ? kCableSignalOn : kCableSignalOff));

    // External glow when active
    if (isLaneActive)
    {
        g.setColour(glow.withAlpha(0.4f));
        g.strokePath(cable, juce::PathStrokeType(kCableGlowThickness));
    }
}
