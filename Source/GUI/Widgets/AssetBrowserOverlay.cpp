#include "AssetBrowserOverlay.h"
#include "../../Core/PedalRegistry.h"

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
    addAndMakeVisible(searchBar);
    searchBar.setMultiLine(false);
    searchBar.setTextToShowWhenEmpty("Search asset...", juce::Colours::grey);
    searchBar.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff151515"));
    searchBar.setColour(juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha(0.2f));
    searchBar.addListener(this);

    addAndMakeVisible(viewport);
    container = std::make_unique<juce::Component>();
    viewport.setViewedComponent(container.get(), false);
    viewport.setScrollBarsShown(true, false);

    addAndMakeVisible(closeBtn);
    closeBtn.setButtonText("X");
    closeBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    closeBtn.onClick = [this]
        {
            if (onClose)
                onClose();
        };

    populateList({});
}

juce::String AssetBrowserOverlay::getTitleForZone() const
{
    switch (targetZone)
    {
        case Nova::ZoneID::Pre:     return "SELECT PRE EFFECT";
        case Nova::ZoneID::Amp:     return "SELECT AMPLIFIER";
        case Nova::ZoneID::FX:      return "SELECT POST EFFECT";
        case Nova::ZoneID::Cabinet: return "SELECT CABINET";
        default:                    return "SELECT ASSET";
    }
}

juce::String AssetBrowserOverlay::getTypeLabelForZone() const
{
    switch (targetZone)
    {
        case Nova::ZoneID::Amp:     return "Amp";
        case Nova::ZoneID::Cabinet: return "Cab";
        case Nova::ZoneID::FX:      return "Space";
        case Nova::ZoneID::Pre:
        default:                    return "Pedal";
    }
}

std::vector<juce::String> AssetBrowserOverlay::getAvailableTypeIDsForZone() const
{
    return PedalRegistry::getPedalTypesForZone(targetZone);
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

    const auto typeIDs = getAvailableTypeIDsForZone();
    const auto typeLabel = getTypeLabelForZone();

    for (const auto& typeID : typeIDs)
    {
        if (filter.isNotEmpty() && !typeID.containsIgnoreCase(filter))
            continue;

        auto* item = new AssetItem(typeID, typeLabel, [this, typeID]
            {
                if (onSelect)
                    onSelect(typeID);

                if (onClose)
                    onClose();
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
