#include "AssetItem.h"

namespace
{
    constexpr float kCornerRadius = 6.0f;

    constexpr float kHoverFillAlpha = 0.10f;
    constexpr float kIconOutlineAlpha = 0.20f;

    constexpr float kIconAreaRatio = 0.7f;
    constexpr int   kIconPadding = 10;

    constexpr float kHoverStroke = 1.5f;

    const juce::Colour kIconPanelBg = juce::Colour::fromString("ff202020");
}

AssetItem::AssetItem(const juce::String& name,
    const juce::String& type,
    std::function<void()> onSelect)
    : itemName(name),
    itemType(type),
    onSelectCallback(std::move(onSelect))
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void AssetItem::mouseUp(const juce::MouseEvent&)
{
    if (onSelectCallback)
        onSelectCallback();
}

void AssetItem::mouseEnter(const juce::MouseEvent&)
{
    isHover = true;
    repaint();
}

void AssetItem::mouseExit(const juce::MouseEvent&)
{
    isHover = false;
    repaint();
}

void AssetItem::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    // 1) Hover background
    g.setColour(isHover ? juce::Colours::white.withAlpha(kHoverFillAlpha)
        : juce::Colours::transparentBlack);
    g.fillRoundedRectangle(bounds, kCornerRadius);

    // 2) Icon area
    auto iconArea = bounds.withTrimmedBottom(bounds.getHeight() * (1.0f - kIconAreaRatio))
        .reduced((float)kIconPadding);

    g.setColour(kIconPanelBg);
    g.fillRoundedRectangle(iconArea, 4.0f);

    g.setColour(juce::Colours::white.withAlpha(kIconOutlineAlpha));
    g.drawRoundedRectangle(iconArea, 4.0f, 1.0f);

    // 2.1) Draw icon based on type
    if (isAmpType())
    {
        g.setColour(juce::Colours::grey);

        const float yKnob = iconArea.getCentreY();
        const float startX = iconArea.getX() + 10.0f;
        const float stepX = 15.0f;
        const float knobSz = 8.0f;

        for (int i = 0; i < 4; ++i)
            g.fillEllipse(startX + (i * stepX), yKnob - 4.0f, knobSz, knobSz);
    }
    else
    {
        g.setColour(juce::Colours::black.withAlpha(0.3f));

        const auto c = iconArea.getCentre();
        g.fillEllipse(c.x - 15.0f, c.y - 15.0f, 12.0f, 12.0f);
        g.fillEllipse(c.x + 3.0f, c.y - 15.0f, 12.0f, 12.0f);
        g.fillEllipse(c.x - 15.0f, c.y + 3.0f, 12.0f, 12.0f);
        g.fillEllipse(c.x + 3.0f, c.y + 3.0f, 12.0f, 12.0f);
    }

    // 3) Label (full bounds, like your original)
    g.setColour(Nova::Colors::Text);
    g.setFont(14.0f);
    g.drawText(itemName, bounds, juce::Justification::centred);

    // 4) Hover outline
    if (isHover)
    {
        g.setColour(Nova::Colors::CableOnA);
        g.drawRoundedRectangle(bounds, kCornerRadius, kHoverStroke);
    }
}
void AssetItem::mouseDrag(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
    {
        juce::String prefix = "PEDAL";

        // Detección a prueba de balas basada en el nombre o en variaciones comunes
        if (itemName.containsIgnoreCase("Amp") || itemType.containsIgnoreCase("Amp"))
            prefix = "AMP";
        else if (itemName.containsIgnoreCase("Cab") || itemType.containsIgnoreCase("Cab"))
            prefix = "CAB";

        juce::String dragDescription = prefix + ":" + itemName;

        // Mensaje de diagnóstico para asegurar que estamos mandando lo correcto
        DBG("DRAG INICIADO -> " + dragDescription);

        container->startDragging(dragDescription, this);
    }
}