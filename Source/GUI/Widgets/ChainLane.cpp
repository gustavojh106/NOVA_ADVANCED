#include "ChainLane.h"

ChainLane::ChainLane(NOVAAudioProcessor& p, Nova::ChainID c) : chainID(c)
{
    // Creamos las 4 zonas obligatorias en orden
    zones.add(new DropZone(p, c, Nova::ZoneID::Pre));
    zones.add(new DropZone(p, c, Nova::ZoneID::Amp));
    zones.add(new DropZone(p, c, Nova::ZoneID::FX));
    zones.add(new DropZone(p, c, Nova::ZoneID::Cabinet));

    for (auto* z : zones) addAndMakeVisible(z);
}

void ChainLane::setActive(bool isActive)
{
    isLaneActive = isActive;
    repaint();
}

void ChainLane::resized()
{
    auto area = getLocalBounds();
    int totalW = area.getWidth();
    int h = area.getHeight();

    int fixedZoneW = 240; // Ancho fijo para Amp y Cab
    int remainingW = totalW - (fixedZoneW * 2);
    int flexZoneW = remainingW / 2; // Ancho flexible para Pre y FX

    zones[0]->setBounds(0, 0, flexZoneW, h);                    // Pre
    zones[1]->setBounds(flexZoneW, 0, fixedZoneW, h);           // Amp
    zones[2]->setBounds(flexZoneW + fixedZoneW, 0, flexZoneW, h);// FX
    zones[3]->setBounds(totalW - fixedZoneW, 0, fixedZoneW, h); // Cab
}

juce::Rectangle<int> ChainLane::getZoneRect(int zoneIndex)
{
    if (zoneIndex >= 0 && zoneIndex < zones.size())
        return zones[zoneIndex]->getBounds();
    return {};
}

void ChainLane::paint(juce::Graphics& g)
{
    float y = (float)getHeight() / 2.0f;
    float w = (float)getWidth();

    // Dibujamos el cable que atraviesa todo
    juce::Path cable;
    cable.startNewSubPath(0, y);
    cable.lineTo(w, y);

    juce::Colour glow = (chainID == Nova::ChainID::LineA) ? Nova::Colors::CableOnA : Nova::Colors::CableOnB;
    if (!isLaneActive) glow = Nova::Colors::CableOff;

    // Sombra del cable
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.strokePath(cable, juce::PathStrokeType(10.0f));

    // Cuerpo del cable
    g.setColour(juce::Colour::fromString("ff151515"));
    g.strokePath(cable, juce::PathStrokeType(6.0f));

    // Luz interna (señal)
    g.setColour(glow);
    g.strokePath(cable, juce::PathStrokeType(isLaneActive ? 2.0f : 1.0f));

    // Glow externo si está activo
    if (isLaneActive) {
        g.setColour(glow.withAlpha(0.4f));
        g.strokePath(cable, juce::PathStrokeType(8.0f));
    }
}