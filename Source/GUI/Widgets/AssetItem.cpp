#include "AssetItem.h"

#include <array>

namespace
{
constexpr float kCornerRadius = 14.0f;
constexpr float kHoverStroke = 1.5f;
}

AssetItem::AssetItem(const Nova::PedalCatalog::Entry& entry,
    std::function<void()> onSelect)
    : itemTypeID(entry.typeID),
      itemName(entry.displayName),
      itemSubtitle(entry.subtitle),
      itemBadge(Nova::PedalCatalog::badgeForKind(entry.kind)),
      accentColour(juce::Colour::fromString(entry.accentHex)),
      itemKind(entry.kind),
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

void AssetItem::drawIcon(juce::Graphics& g, juce::Rectangle<float> area) const
{
    g.setColour(accentColour.withAlpha(0.18f));
    g.fillRoundedRectangle(area, 14.0f);

    g.setColour(accentColour.withAlpha(0.9f));
    g.drawRoundedRectangle(area, 14.0f, 1.0f);

    const auto centre = area.getCentre();

    if (itemKind == Nova::PedalCatalog::Kind::Amplifier)
    {
        const auto ampFace = area.reduced(area.getWidth() * 0.12f, area.getHeight() * 0.2f);
        g.setColour(juce::Colour::fromString("ff0D1520"));
        g.fillRoundedRectangle(ampFace, 10.0f);
        g.setColour(accentColour.withAlpha(0.7f));
        g.drawRoundedRectangle(ampFace, 10.0f, 1.2f);

        const float knobSize = 10.0f;
        const float startX = ampFace.getX() + 16.0f;
        const float y = ampFace.getY() + 20.0f;
        for (int i = 0; i < 4; ++i)
            g.fillEllipse(startX + (float)i * 22.0f, y, knobSize, knobSize);

        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.fillRect(ampFace.getX() + 14.0f, ampFace.getBottom() - 18.0f, ampFace.getWidth() - 28.0f, 4.0f);
        return;
    }

    if (itemKind == Nova::PedalCatalog::Kind::Cabinet)
    {
        const auto cabArea = area.reduced(area.getWidth() * 0.16f, area.getHeight() * 0.12f);
        g.setColour(juce::Colour::fromString("ff0D1520"));
        g.fillRoundedRectangle(cabArea, 12.0f);
        g.setColour(accentColour.withAlpha(0.75f));
        g.drawRoundedRectangle(cabArea, 12.0f, 1.2f);

        const float speaker = juce::jmin(cabArea.getWidth(), cabArea.getHeight()) * 0.28f;
        g.drawEllipse(cabArea.getX() + 14.0f, cabArea.getY() + 12.0f, speaker, speaker, 1.8f);
        g.drawEllipse(cabArea.getRight() - 14.0f - speaker, cabArea.getY() + 12.0f, speaker, speaker, 1.8f);
        g.drawEllipse(cabArea.getX() + 14.0f, cabArea.getBottom() - 12.0f - speaker, speaker, speaker, 1.8f);
        g.drawEllipse(cabArea.getRight() - 14.0f - speaker, cabArea.getBottom() - 12.0f - speaker, speaker, speaker, 1.8f);
        return;
    }

    if (itemTypeID.containsIgnoreCase("Compressor"))
    {
        juce::Path bars;
        const float baseX = area.getX() + 24.0f;
        const float baseY = area.getBottom() - 24.0f;
        const float barWidth = 12.0f;
        const float gaps = 10.0f;
        const std::array<float, 4> heights{ 30.0f, 44.0f, 56.0f, 38.0f };
        for (size_t i = 0; i < heights.size(); ++i)
            bars.addRoundedRectangle(baseX + (float)i * (barWidth + gaps), baseY - heights[i], barWidth, heights[i], 4.0f);
        g.fillPath(bars);
        return;
    }

    if (itemTypeID.containsIgnoreCase("Overdrive"))
    {
        juce::Path wave;
        wave.startNewSubPath(area.getX() + 18.0f, centre.y);
        wave.lineTo(centre.x - 18.0f, centre.y);
        wave.lineTo(centre.x - 18.0f, area.getY() + 22.0f);
        wave.lineTo(centre.x + 18.0f, area.getY() + 22.0f);
        wave.lineTo(centre.x + 18.0f, area.getBottom() - 22.0f);
        wave.lineTo(area.getRight() - 18.0f, area.getBottom() - 22.0f);
        g.strokePath(wave, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }

    if (itemTypeID.containsIgnoreCase("Chorus"))
    {
        for (int i = 0; i < 3; ++i)
        {
            juce::Path wave;
            const float offset = -14.0f + 14.0f * (float)i;
            wave.startNewSubPath(area.getX() + 16.0f, centre.y + offset * 0.25f);
            wave.cubicTo(area.getX() + 42.0f, area.getY() + 16.0f + offset * 0.1f,
                area.getRight() - 42.0f, area.getBottom() - 16.0f - offset * 0.1f,
                area.getRight() - 16.0f, centre.y + offset * 0.25f);
            g.strokePath(wave, juce::PathStrokeType(i == 1 ? 3.0f : 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        return;
    }

    if (itemTypeID.containsIgnoreCase("Delay"))
    {
        for (int i = 0; i < 3; ++i)
        {
            const float alpha = 1.0f - 0.25f * (float)i;
            const float x = area.getX() + 20.0f + (float)i * 18.0f;
            g.setColour(accentColour.withAlpha(alpha));
            g.drawRoundedRectangle(x, area.getY() + 24.0f, 34.0f, area.getHeight() - 48.0f, 8.0f, 2.0f);
        }
        return;
    }

    if (itemTypeID.containsIgnoreCase("Reverb"))
    {
        for (int i = 0; i < 5; ++i)
        {
            const float radius = 10.0f + (float)i * 8.0f;
            g.drawEllipse(centre.x - radius * 0.5f, centre.y - radius * 0.5f, radius, radius, 1.4f);
        }
        return;
    }

    g.fillEllipse(centre.x - 22.0f, centre.y - 22.0f, 44.0f, 44.0f);
}

void AssetItem::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    juce::ColourGradient body(juce::Colour::fromString("ff111827"),
        bounds.getCentreX(),
        bounds.getY(),
        juce::Colour::fromString("ff0B0E14"),
        bounds.getCentreX(),
        bounds.getBottom(),
        false);
    g.setGradientFill(body);
    g.fillRoundedRectangle(bounds, kCornerRadius);

    g.setColour(isHover ? accentColour.withAlpha(0.42f) : juce::Colours::white.withAlpha(0.08f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), kCornerRadius, isHover ? kHoverStroke : 1.0f);

    auto content = bounds.reduced(12.0f);
    auto header = content.removeFromTop(24.0f);
    auto badge = header.removeFromLeft(48.0f);

    g.setColour(accentColour.withAlpha(0.22f));
    g.fillRoundedRectangle(badge, 8.0f);
    g.setColour(accentColour.withAlpha(0.9f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText(itemBadge.toUpperCase(), badge.toNearestInt(), juce::Justification::centred);

    g.setColour(juce::Colours::white.withAlpha(0.55f));
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText("Drag or click", header.toNearestInt(), juce::Justification::centredRight);

    auto iconArea = content.removeFromTop(bounds.getHeight() * 0.46f);
    drawIcon(g, iconArea);

    content.removeFromTop(8.0f);
    g.setColour(Nova::Colors::Text);
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    g.drawFittedText(itemName, content.removeFromTop(28.0f).toNearestInt(), juce::Justification::centredLeft, 1);

    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawFittedText(itemSubtitle, content.removeFromTop(34.0f).toNearestInt(), juce::Justification::topLeft, 2);
}

void AssetItem::mouseDrag(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
    {
        juce::String prefix = "PEDAL";
        if (itemKind == Nova::PedalCatalog::Kind::Amplifier)
            prefix = "AMP";
        else if (itemKind == Nova::PedalCatalog::Kind::Cabinet)
            prefix = "CAB";

        container->startDragging(prefix + ":" + itemTypeID, this);
    }
}
