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

        if (Nova::PedalCatalog::kindFromType(name) == Nova::PedalCatalog::Kind::Cabinet)
        {
            juce::ColourGradient grille(juce::Colour::fromString("ff151821").interpolatedWith(accent, 0.10f),
                bounds.getCentreX(), bounds.getY(),
                juce::Colour::fromString("ff07080B"),
                bounds.getCentreX(), bounds.getBottom(), false);
            grille.addColour(0.52, juce::Colour::fromString("ff222633").interpolatedWith(accent, 0.06f));
            g.setGradientFill(grille);
            g.fillRoundedRectangle(bounds, 10.0f);

            g.setColour(hovered ? accent.withAlpha(0.60f) : accent.withAlpha(0.28f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, hovered ? 1.5f : 1.0f);

            auto content = bounds.reduced(13.0f, 5.0f);
            g.setColour(juce::Colours::white.withAlpha(hovered ? 0.98f : 0.88f));
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText(name, content.removeFromTop(17.0f).toNearestInt(),
                juce::Justification::centredLeft, true);

            g.setColour(accent.withAlpha(hovered ? 0.70f : 0.50f));
            g.setFont(juce::Font(juce::FontOptions(9.5f)));
            g.drawText(subtitle, content.removeFromTop(13.0f).toNearestInt(),
                juce::Justification::centredLeft, true);

            auto face = bounds.reduced(12.0f, 7.0f).removeFromBottom(13.0f);
            g.setColour(juce::Colour::fromString("ff050608").withAlpha(0.84f));
            g.fillRoundedRectangle(face, 3.0f);
            g.setColour(accent.withAlpha(hovered ? 0.36f : 0.20f));
            for (int i = 0; i < 2; ++i)
            {
                const float x = face.getX() + face.getWidth() * (i == 0 ? 0.34f : 0.66f);
                g.drawEllipse(x - 4.0f, face.getCentreY() - 4.0f, 8.0f, 8.0f, 1.1f);
            }

            auto badgeBounds = juce::Rectangle<float>(
                bounds.getRight() - 44.0f,
                bounds.getCentreY() - 9.0f,
                34.0f, 18.0f);
            g.setColour(accent.withAlpha(hovered ? 0.30f : 0.16f));
            g.fillRoundedRectangle(badgeBounds, 5.0f);
            g.setColour(accent.withAlpha(hovered ? 0.94f : 0.70f));
            g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            g.drawText("CAB", badgeBounds.toNearestInt(), juce::Justification::centred);
            return;
        }

        if (Nova::PedalCatalog::kindFromType(name) == Nova::PedalCatalog::Kind::Amplifier)
        {
            juce::ColourGradient face(juce::Colour::fromString("ff191B22").interpolatedWith(accent, 0.10f),
                bounds.getCentreX(), bounds.getY(),
                juce::Colour::fromString("ff07090D"),
                bounds.getCentreX(), bounds.getBottom(), false);
            face.addColour(0.48, juce::Colour::fromString("ff24212A").interpolatedWith(accent, 0.08f));
            g.setGradientFill(face);
            g.fillRoundedRectangle(bounds, 10.0f);

            g.setColour(hovered ? accent.withAlpha(0.62f) : accent.withAlpha(0.30f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, hovered ? 1.5f : 1.0f);

            auto content = bounds.reduced(13.0f, 5.0f);
            g.setColour(juce::Colours::white.withAlpha(hovered ? 0.98f : 0.88f));
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText(name, content.removeFromTop(17.0f).toNearestInt(),
                juce::Justification::centredLeft, true);

            g.setColour(accent.withAlpha(hovered ? 0.72f : 0.52f));
            g.setFont(juce::Font(juce::FontOptions(9.5f)));
            g.drawText(subtitle, content.removeFromTop(13.0f).toNearestInt(),
                juce::Justification::centredLeft, true);

            auto grille = bounds.reduced(12.0f, 7.0f).removeFromBottom(11.0f);
            g.setColour(juce::Colour::fromString("ff05070A").withAlpha(0.82f));
            g.fillRoundedRectangle(grille, 3.0f);
            g.setColour(accent.withAlpha(hovered ? 0.46f : 0.26f));
            for (int i = 0; i < 5; ++i)
            {
                const float x = grille.getX() + 8.0f + (grille.getWidth() - 16.0f) * ((float)i / 4.0f);
                g.fillEllipse(x - 2.0f, grille.getCentreY() - 2.0f, 4.0f, 4.0f);
            }

            auto badgeBounds = juce::Rectangle<float>(
                bounds.getRight() - 44.0f,
                bounds.getCentreY() - 9.0f,
                34.0f, 18.0f);
            g.setColour(accent.withAlpha(hovered ? 0.30f : 0.16f));
            g.fillRoundedRectangle(badgeBounds, 5.0f);
            g.setColour(accent.withAlpha(hovered ? 0.94f : 0.70f));
            g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            g.drawText("AMP", badgeBounds.toNearestInt(), juce::Justification::centred);
            return;
        }

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
