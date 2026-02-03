#pragma once
#include <JuceHeader.h>
#include "../../Core/Constants.h"

class AssetItem : public juce::Component
{
public:
    AssetItem(const juce::String& name, const juce::String& type, std::function<void()> onSelect);

    void mouseUp(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void paint(juce::Graphics& g) override;

private:
    juce::String itemName;
    juce::String itemType;
    std::function<void()> onSelectCallback;
    bool isHover = false;
};