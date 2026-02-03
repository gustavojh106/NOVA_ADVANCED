#pragma once
#include <JuceHeader.h>
#include "DropZone.h"

class ChainLane : public juce::Component
{
public:
    ChainLane(NOVAAudioProcessor& p, Nova::ChainID c);

    void setActive(bool isActive);
    void paint(juce::Graphics& g) override;
    void resized() override;

    // Para saber dónde poner los pedales
    juce::Rectangle<int> getZoneRect(int zoneIndex);

private:
    juce::OwnedArray<DropZone> zones;
    Nova::ChainID chainID;
    bool isLaneActive = false;
};