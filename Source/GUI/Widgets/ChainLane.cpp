#include "ChainLane.h"

namespace
{
    constexpr int kFixedZoneWidth = 240; // Amp y Cab
    constexpr float kCableShadowThickness = 10.0f;
    constexpr float kCableBodyThickness = 6.0f;
    constexpr float kCableSignalOn = 2.0f;
    constexpr float kCableSignalOff = 1.0f;
    constexpr float kCableGlowThickness = 8.0f;

    const juce::Colour kCableBodyColour = juce::Colour::fromString("ff151515");
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
    const auto area = getLocalBounds();
    const int totalW = area.getWidth();
    const int h = area.getHeight();

    const int fixedTotal = kFixedZoneWidth * 2;
    const int remaining = juce::jmax(0, totalW - fixedTotal);
    const int flexZoneW = remaining / 2;

    // Pre | Amp | FX | Cab
    const int xPre = 0;
    const int xAmp = xPre + flexZoneW;
    const int xFx = xAmp + kFixedZoneWidth;
    const int xCab = totalW - kFixedZoneWidth;

    zones[0]->setBounds(xPre, 0, flexZoneW, h);          // Pre
    zones[1]->setBounds(xAmp, 0, kFixedZoneWidth, h);    // Amp
    zones[2]->setBounds(xFx, 0, flexZoneW, h);          // FX
    zones[3]->setBounds(xCab, 0, kFixedZoneWidth, h);    // Cab
}

void ChainLane::paint(juce::Graphics& g)
{
    const float y = getHeight() * 0.5f;
    const float w = (float)getWidth();

    juce::Path cable;
    cable.startNewSubPath(0.0f, y);
    cable.lineTo(w, y);

    const auto glow = getCableGlowColour();

    // Sombra
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.strokePath(cable, juce::PathStrokeType(kCableShadowThickness));

    // Cuerpo
    g.setColour(kCableBodyColour);
    g.strokePath(cable, juce::PathStrokeType(kCableBodyThickness));

    // Señal
    g.setColour(glow);
    g.strokePath(cable, juce::PathStrokeType(isLaneActive ? kCableSignalOn : kCableSignalOff));

    // Glow externo cuando está activo
    if (isLaneActive)
    {
        g.setColour(glow.withAlpha(0.4f));
        g.strokePath(cable, juce::PathStrokeType(kCableGlowThickness));
    }
}
