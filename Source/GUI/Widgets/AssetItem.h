#pragma once

#include <JuceHeader.h>
#include "../../Core/Constants.h"
#include "../../Core/PedalCatalog.h"
#include <functional>

class AssetItem : public juce::Component
{
public:
    AssetItem(const Nova::PedalCatalog::Entry& entry,
        std::function<void()> onSelect);

    void mouseUp(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void paint(juce::Graphics& g) override;

private:
    void drawIcon(juce::Graphics& g, juce::Rectangle<float> area) const;

    juce::String itemTypeID;
    juce::String itemName;
    juce::String itemSubtitle;
    juce::String itemBadge;
    juce::Colour accentColour;
    Nova::PedalCatalog::Kind itemKind = Nova::PedalCatalog::Kind::Pedal;
    std::function<void()> onSelectCallback;

    bool isHover = false;
};
