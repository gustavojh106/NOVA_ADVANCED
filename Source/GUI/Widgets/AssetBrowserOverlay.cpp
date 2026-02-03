#include "AssetBrowserOverlay.h"

namespace
{
    constexpr float kBackdropAlpha = 0.85f;

    constexpr int kOuterPadX = 100;
    constexpr int kOuterPadY = 50;

    constexpr int kHeaderHeight = 60;
    constexpr int kSearchHeight = 40;
    constexpr int kSearchPadX = 100;
    constexpr int kBetweenSearchAndList = 20;

    constexpr int kCloseBtnSize = 30;
    constexpr int kCloseBtnInset = 10;

    constexpr int kViewportPad = 20;

    constexpr int kItemSize = 140;
    constexpr int kItemGap = 20;

    constexpr float kPanelCorner = 12.0f;
    constexpr float kOutlineAlpha = 0.1f;
}

AssetBrowserOverlay::AssetBrowserOverlay(Nova::ZoneID zone,
    std::function<void(juce::String)> onAssetSelected,
    std::function<void()> onCloseFn)
    : targetZone(zone),
    onSelect(std::move(onAssetSelected)),
    onClose(std::move(onCloseFn))
{
    // Search
    addAndMakeVisible(searchBar);
    searchBar.setMultiLine(false);
    searchBar.setTextToShowWhenEmpty("Search model...", juce::Colours::grey);
    searchBar.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff151515"));
    searchBar.setColour(juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha(0.2f));
    searchBar.addListener(this);

    // Viewport / Container
    addAndMakeVisible(viewport);
    container = std::make_unique<juce::Component>();
    viewport.setViewedComponent(container.get(), false);
    viewport.setScrollBarsShown(true, false);

    // Close button
    addAndMakeVisible(closeBtn);
    closeBtn.setButtonText("X");
    closeBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    closeBtn.onClick = [this]
        {
            if (onClose) onClose();
        };

    populateList({});
}

juce::String AssetBrowserOverlay::getTitleForZone() const
{
    return (targetZone == Nova::ZoneID::Amp) ? "SELECT AMPLIFIER" : "SELECT CABINET";
}

juce::String AssetBrowserOverlay::getTypeLabelForZone() const
{
    return (targetZone == Nova::ZoneID::Amp) ? "Amp" : "Cab";
}

std::vector<juce::String> AssetBrowserOverlay::getMockDataForZone() const
{
    if (targetZone == Nova::ZoneID::Amp)
        return { "British Lead 800", "USA Rectifier", "Jazz Clean 120", "German Fireball", "Blues Junior", "Bass SuperTube" };

    return { "4x12 Vintage 30", "2x12 Greenback", "1x12 Blue Alnico", "8x10 Bass Fridge", "4x12 Recto Std", "2x10 Tremolo" };
}

// Nota: esto preserva TU mapeo actual (aunque no tenga sentido aún).
juce::String AssetBrowserOverlay::mapToInternalID(const juce::String&) const
{
    return (targetZone == Nova::ZoneID::Amp) ? "Overdrive" : "Cabinet";
}

void AssetBrowserOverlay::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(kBackdropAlpha));

    const auto panelArea = getLocalBounds().reduced(kOuterPadX, kOuterPadY);

    g.setColour(Nova::Colors::MixerPanel);
    g.fillRoundedRectangle(panelArea.toFloat(), kPanelCorner);

    g.setColour(juce::Colours::white.withAlpha(kOutlineAlpha));
    g.drawRoundedRectangle(panelArea.toFloat(), kPanelCorner, 1.0f);

    g.setColour(Nova::Colors::Text);
    g.setFont(24.0f);

    auto header = panelArea;
    header.removeFromBottom(panelArea.getHeight() - kHeaderHeight);
    g.drawText(getTitleForZone(), header, juce::Justification::centred);
}

void AssetBrowserOverlay::resized()
{
    auto area = getLocalBounds().reduced(kOuterPadX, kOuterPadY);

    closeBtn.setBounds(area.getRight() - (kCloseBtnSize + kCloseBtnInset),
        area.getY() + kCloseBtnInset,
        kCloseBtnSize,
        kCloseBtnSize);

    area.removeFromTop(kHeaderHeight);

    searchBar.setBounds(area.removeFromTop(kSearchHeight).reduced(kSearchPadX, 0));
    area.removeFromTop(kBetweenSearchAndList);

    viewport.setBounds(area.reduced(kViewportPad));

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

    const auto mockData = getMockDataForZone();
    const auto typeLabel = getTypeLabelForZone();

    for (const auto& name : mockData)
    {
        if (filter.isNotEmpty() && !name.containsIgnoreCase(filter))
            continue;

        auto* item = new AssetItem(name, typeLabel, [this, name]
            {
                const auto internalID = mapToInternalID(name);
                if (onSelect) onSelect(internalID);
                if (onClose)  onClose();
            });

        container->addAndMakeVisible(item);
        items.add(item);
    }

    layoutItems();
}

void AssetBrowserOverlay::layoutItems()
{
    const int w = viewport.getWidth();
    if (w <= 0)
        return;

    const int cols = juce::jmax(1, w / (kItemSize + kItemGap));

    int x = 0;
    int y = 0;
    int col = 0;

    for (auto* item : items)
    {
        item->setBounds(x, y, kItemSize, kItemSize);

        if (++col >= cols)
        {
            col = 0;
            x = 0;
            y += kItemSize + kItemGap;
        }
        else
        {
            x += kItemSize + kItemGap;
        }
    }

    container->setSize(w, y + kItemSize + kItemGap);
}
