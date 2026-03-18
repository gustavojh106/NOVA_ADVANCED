#include "AssetBrowserOverlay.h"

#include "../../Core/PedalRegistry.h"

namespace
{
constexpr float kBackdropAlpha = 0.88f;

constexpr int kOuterPadX = 96;
constexpr int kOuterPadY = 48;

constexpr int kHeaderHeight = 92;
constexpr int kSearchHeight = 42;
constexpr int kSearchPadX = 110;
constexpr int kBetweenSearchAndList = 20;

constexpr int kCloseBtnSize = 34;
constexpr int kCloseBtnInset = 12;

constexpr int kViewportPad = 20;

constexpr int kItemWidth = 190;
constexpr int kItemHeight = 182;
constexpr int kItemGap = 18;

constexpr float kPanelCorner = 18.0f;
constexpr float kOutlineAlpha = 0.12f;
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
    searchBar.setTextToShowWhenEmpty("Search modules, tones or use cases...", juce::Colours::grey);
    searchBar.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff111827"));
    searchBar.setColour(juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha(0.18f));
    searchBar.setColour(juce::TextEditor::textColourId, juce::Colours::white.withAlpha(0.86f));
    searchBar.addListener(this);

    addAndMakeVisible(viewport);
    container = std::make_unique<juce::Component>();
    viewport.setViewedComponent(container.get(), false);
    viewport.setScrollBarsShown(true, false);

    addAndMakeVisible(closeBtn);
    closeBtn.setButtonText("X");
    closeBtn.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff1A2332"));
    closeBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.72f));
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
        case Nova::ZoneID::Pre:     return "Build The Front End";
        case Nova::ZoneID::Amp:     return "Choose The Amp Voice";
        case Nova::ZoneID::FX:      return "Shape The Space";
        case Nova::ZoneID::Cabinet: return "Finish With The Cabinet";
        default:                    return "Choose A Module";
    }
}

juce::String AssetBrowserOverlay::getZoneHint() const
{
    switch (targetZone)
    {
        case Nova::ZoneID::Pre:     return "Dynamics, gain and texture before the amp.";
        case Nova::ZoneID::Amp:     return "One amp head per lane for clear routing and recall.";
        case Nova::ZoneID::FX:      return "Modulation and ambience after the amp stage.";
        case Nova::ZoneID::Cabinet: return "One cabinet per lane to lock the final speaker tone.";
        default:                    return {};
    }
}

void AssetBrowserOverlay::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(kBackdropAlpha));

    const auto panelArea = getLocalBounds().reduced(kOuterPadX, kOuterPadY);

    juce::ColourGradient panelFill(juce::Colour::fromString("ff111827"),
        (float)panelArea.getCentreX(),
        (float)panelArea.getY(),
        juce::Colour::fromString("ff0B0E14"),
        (float)panelArea.getCentreX(),
        (float)panelArea.getBottom(),
        false);
    g.setGradientFill(panelFill);
    g.fillRoundedRectangle(panelArea.toFloat(), kPanelCorner);

    g.setColour(juce::Colours::white.withAlpha(kOutlineAlpha));
    g.drawRoundedRectangle(panelArea.toFloat(), kPanelCorner, 1.0f);

    auto header = panelArea.reduced(24, 18).removeFromTop(kHeaderHeight);
    g.setColour(Nova::Colors::Text);
    g.setFont(juce::Font(juce::FontOptions(26.0f, juce::Font::bold)));
    g.drawText(getTitleForZone(), header.removeFromTop(38), juce::Justification::centredLeft);

    g.setColour(juce::Colours::white.withAlpha(0.62f));
    g.setFont(juce::Font(juce::FontOptions(12.5f)));
    g.drawFittedText(getZoneHint(), header.removeFromTop(24), juce::Justification::centredLeft, 2);
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

    for (const auto& entry : Nova::PedalCatalog::entries())
    {
        if (!PedalRegistry::isTypeSupported(entry.typeID))
            continue;

        if (!Nova::PedalCatalog::canLiveInZone(entry.typeID, targetZone))
            continue;

        if (!Nova::PedalCatalog::matchesFilter(entry, filter))
            continue;

        auto* item = new AssetItem(entry, [this, typeID = juce::String(entry.typeID)]
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

    const int cols = juce::jmax(1, (w + kItemGap) / (kItemWidth + kItemGap));
    const int rowWidth = cols * kItemWidth + juce::jmax(0, cols - 1) * kItemGap;

    int x = juce::jmax(0, (w - rowWidth) / 2);
    int y = 0;
    int col = 0;

    for (auto* item : items)
    {
        item->setBounds(x, y, kItemWidth, kItemHeight);

        if (++col >= cols)
        {
            col = 0;
            x = juce::jmax(0, (w - rowWidth) / 2);
            y += kItemHeight + kItemGap;
        }
        else
        {
            x += kItemWidth + kItemGap;
        }
    }

    container->setSize(w, y + (items.isEmpty() ? 0 : kItemHeight + kItemGap));
}
