#include "AssetBrowserOverlay.h"

AssetBrowserOverlay::AssetBrowserOverlay(Nova::ZoneID zone, std::function<void(juce::String)> onAssetSelected, std::function<void()> onClose)
    : targetZone(zone), onSelect(onAssetSelected), onClose(onClose)
{
    addAndMakeVisible(searchBar);
    searchBar.setMultiLine(false);
    searchBar.setTextToShowWhenEmpty("Search model...", juce::Colours::grey);
    searchBar.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff151515"));
    searchBar.setColour(juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha(0.2f));
    searchBar.addListener(this);

    addAndMakeVisible(viewport);
    container.reset(new juce::Component());
    viewport.setViewedComponent(container.get(), false);
    viewport.setScrollBarsShown(true, false);

    addAndMakeVisible(closeBtn);
    closeBtn.setButtonText("X");
    closeBtn.onClick = onClose;
    closeBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);

    populateList("");
}

void AssetBrowserOverlay::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.85f));
    auto area = getLocalBounds().reduced(100, 50);

    g.setColour(Nova::Colors::MixerPanel);
    g.fillRoundedRectangle(area.toFloat(), 12.0f);
    g.setColour(juce::Colours::white.withAlpha(0.1f));
    g.drawRoundedRectangle(area.toFloat(), 12.0f, 1.0f);

    g.setColour(Nova::Colors::Text);
    g.setFont(24.0f);
    juce::String title = (targetZone == Nova::ZoneID::Amp) ? "SELECT AMPLIFIER" : "SELECT CABINET";
    g.drawText(title, area.removeFromTop(60), juce::Justification::centred);
}

void AssetBrowserOverlay::resized()
{
    auto area = getLocalBounds().reduced(100, 50);
    closeBtn.setBounds(area.getRight() - 40, area.getY() + 10, 30, 30);
    area.removeFromTop(60);
    searchBar.setBounds(area.removeFromTop(40).reduced(100, 0));
    area.removeFromTop(20);
    viewport.setBounds(area.reduced(20));
    layoutItems();
}

void AssetBrowserOverlay::textEditorTextChanged(juce::TextEditor& editor)
{
    populateList(editor.getText());
}

void AssetBrowserOverlay::populateList(const juce::String& filter)
{
    container->removeAllChildren();
    items.clear();

    // Simulación de base de datos
    std::vector<juce::String> mockData;
    if (targetZone == Nova::ZoneID::Amp)
        mockData = { "British Lead 800", "USA Rectifier", "Jazz Clean 120", "German Fireball", "Blues Junior", "Bass SuperTube" };
    else
        mockData = { "4x12 Vintage 30", "2x12 Greenback", "1x12 Blue Alnico", "8x10 Bass Fridge", "4x12 Recto Std", "2x10 Tremolo" };

    for (const auto& name : mockData) {
        if (filter.isNotEmpty() && !name.containsIgnoreCase(filter)) continue;

        auto* item = new AssetItem(name, (targetZone == Nova::ZoneID::Amp ? "Amp" : "Cab"), [this, name]() {
            juce::String internalID = (targetZone == Nova::ZoneID::Amp) ? "Overdrive" : "Cabinet"; // Mapeo simple por ahora
            if (onSelect) onSelect(internalID);
            if (onClose) onClose();
            });

        container->addAndMakeVisible(item);
        items.add(item);
    }
    layoutItems();
}

void AssetBrowserOverlay::layoutItems()
{
    int itemSize = 140; int gap = 20;
    int w = viewport.getWidth();
    if (w <= 0) return;

    int cols = juce::jmax(1, w / (itemSize + gap));
    int x = 0, y = 0, col = 0;

    for (auto* item : items) {
        item->setBounds(x, y, itemSize, itemSize);
        col++;
        if (col >= cols) { col = 0; x = 0; y += itemSize + gap; }
        else { x += itemSize + gap; }
    }
    container->setSize(viewport.getWidth(), y + itemSize + gap);
}