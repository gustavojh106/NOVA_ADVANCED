#pragma once

#include <JuceHeader.h>
#include "../../Core/Constants.h"
#include "AssetItem.h"
#include <functional>
#include <vector>

class AssetBrowserOverlay : public juce::Component,
    public juce::TextEditor::Listener
{
public:
    AssetBrowserOverlay(Nova::ZoneID zone,
        std::function<void(juce::String)> onAssetSelected,
        std::function<void()> onClose);

    void paint(juce::Graphics& g) override;
    void resized() override;

    void textEditorTextChanged(juce::TextEditor& editor) override;

private:
    void populateList(const juce::String& filter);
    void layoutItems();

    juce::String getTitleForZone() const;
    juce::String getTypeLabelForZone() const;
    std::vector<juce::String> getMockDataForZone() const;
    juce::String mapToInternalID(const juce::String& displayName) const;

    Nova::ZoneID targetZone{};
    std::function<void(juce::String)> onSelect;
    std::function<void()> onClose;

    juce::TextEditor searchBar;
    juce::Viewport viewport;
    std::unique_ptr<juce::Component> container;
    juce::OwnedArray<AssetItem> items;
    juce::TextButton closeBtn;
};
