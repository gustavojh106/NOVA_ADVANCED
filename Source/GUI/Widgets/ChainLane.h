#pragma once

#include <JuceHeader.h>
#include "DropZone.h"

class ChainLane : public juce::Component
{
public:
    ChainLane(NOVAAudioProcessor& processor, Nova::ChainID chain);

    void setActive(bool shouldBeActive);
    void paint(juce::Graphics& g) override;
    void resized() override;

    // Para saber dónde posicionar los pedales dentro de cada zona
    juce::Rectangle<int> getZoneRect(int zoneIndex) const;

private:
    juce::Colour getCableGlowColour() const noexcept;

    juce::OwnedArray<DropZone> zones;
    Nova::ChainID chainID;
    bool isLaneActive = false;
};
