#pragma once
#include <JuceHeader.h>
#include "../../Core/PedalCatalog.h"
#include "../../Effects/Pedals/Base/PedalUIFactory.h"

class DraggableButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    void setItemType(const juce::String& type) { itemType = type; }

    void setAccentColour(juce::Colour c) { accentColour = c; }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        juce::ignoreUnused(e);
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        {
            juce::String dragDescription = itemType + ":" + getButtonText();
            container->startDragging(dragDescription, this);
        }
    }

    void mouseEnter(const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseEnter(e);
        hovered = true;
        repaint();
    }

    void mouseExit(const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseExit(e);
        hovered = false;
        repaint();
    }

    void paintButton(juce::Graphics& g,
                     bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override
    {
        juce::ignoreUnused(shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

        const auto bounds = getLocalBounds().toFloat();
        const auto name = getButtonText();
        const auto subtitle = Nova::PedalCatalog::subtitleForType(name);

        // Check for custom per-pedal thumbnail painter
        if (auto* customUI = Nova::PedalUI::findPedalUI(name))
        {
            if (customUI->thumbnailPaint)
            {
                customUI->thumbnailPaint(g, bounds, name, subtitle, hovered);
                return;
            }
        }

        // ---- Default thumbnail paint ----
        const auto accent = accentColour;
        const auto accentBright = Nova::PedalCatalog::accentBrightForType(name);
        const auto badge = Nova::PedalCatalog::badgeForKind(Nova::PedalCatalog::kindFromType(name));

        // Card background
        juce::ColourGradient body(juce::Colour::fromString("ff141C2B"),
            bounds.getCentreX(), bounds.getY(),
            juce::Colour::fromString("ff0D1520"),
            bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(body);
        g.fillRoundedRectangle(bounds, 10.0f);

        // Border - accent glow on hover
        if (hovered)
        {
            g.setColour(accent.withAlpha(0.45f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.5f);
        }
        else
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.0f);
        }

        // Accent left strip
        auto strip = bounds.withWidth(4.0f).reduced(0.0f, 6.0f);
        g.setColour(accent.withAlpha(hovered ? 0.92f : 0.65f));
        g.fillRoundedRectangle(strip, 2.0f);

        // Top glow line
        if (hovered)
        {
            auto topGlow = bounds.reduced(16.0f, 0.0f).removeFromTop(2.0f);
            juce::ColourGradient glowGrad(juce::Colours::transparentBlack, topGlow.getX(), topGlow.getCentreY(),
                accentBright.withAlpha(0.40f), topGlow.getCentreX(), topGlow.getCentreY(), false);
            glowGrad.addColour(1.0, juce::Colours::transparentBlack);
            g.setGradientFill(glowGrad);
            g.fillRoundedRectangle(topGlow, 1.0f);
        }

        // Content area
        auto content = bounds.reduced(14.0f, 4.0f);
        content.removeFromLeft(2.0f);

        // Pedal name (bold)
        g.setColour(juce::Colours::white.withAlpha(hovered ? 0.96f : 0.88f));
        g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        g.drawText(name, content.removeFromTop(18.0f).toNearestInt(),
            juce::Justification::centredLeft, true);

        // Subtitle
        g.setColour(juce::Colours::white.withAlpha(0.48f));
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        auto subtitleRow = content.removeFromTop(14.0f);
        g.drawText(subtitle, subtitleRow.toNearestInt(),
            juce::Justification::centredLeft, true);

        // Type badge (right side)
        auto badgeBounds = juce::Rectangle<float>(
            bounds.getRight() - 42.0f,
            bounds.getCentreY() - 9.0f,
            32.0f, 18.0f);
        g.setColour(accent.withAlpha(hovered ? 0.28f : 0.16f));
        g.fillRoundedRectangle(badgeBounds, 5.0f);
        g.setColour(accent.withAlpha(hovered ? 0.92f : 0.72f));
        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.drawText(badge.toUpperCase(), badgeBounds.toNearestInt(),
            juce::Justification::centred);
    }

private:
    juce::String itemType = "PEDAL";
    juce::Colour accentColour { juce::Colour::fromString("ffA78BFA") };
    bool hovered = false;
};
