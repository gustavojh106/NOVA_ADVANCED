#pragma once

#include <JuceHeader.h>
#include "../Base/PedalUIFactory.h"
#include <BinaryData.h>

namespace Nova::OverdriveUI
{

inline void paintDashboard(juce::Graphics& g, juce::Rectangle<float> bounds,
    const juce::String& name, const juce::String& subtitle, const juce::String& badge,
    bool enabled, bool hovered, int headerHeight)
{
    const auto accent      = juce::Colour::fromString("ffF06848");
    const auto accentBright = juce::Colour::fromString("ffFF9B70");
    const auto accentDeep  = juce::Colour::fromString("ff7B291A");
    const auto metalDark   = juce::Colour::fromString("ff1A1410");
    const auto metalMid    = juce::Colour::fromString("ff241E18");
    const auto metalLight  = juce::Colour::fromString("ff352A22");

    // Clip to rounded rect for texture
    juce::Path clipPath;
    clipPath.addRoundedRectangle(bounds, 14.0f);
    g.saveState();
    g.reduceClipRegion(clipPath);

    // ---- Enclosure body - dark base gradient ----
    const auto topCol = enabled ? metalMid.interpolatedWith(accent, 0.08f) : juce::Colour::fromString("ff0A0A0A");
    const auto botCol = enabled ? metalDark : juce::Colour::fromString("ff060606");
    juce::ColourGradient body(topCol, bounds.getCentreX(), bounds.getY(),
        botCol, bounds.getCentreX(), bounds.getBottom(), false);
    if (enabled)
        body.addColour(0.35, metalLight.interpolatedWith(accent, 0.05f));
    g.setGradientFill(body);
    g.fillRect(bounds);

    // Real rusted orange metal texture
    if (enabled)
    {
        static auto dashTex = juce::ImageCache::getFromMemory(
            BinaryData::rusted_orange_metal_jpg, BinaryData::rusted_orange_metal_jpgSize);
        if (dashTex.isValid())
        {
            g.setOpacity(hovered ? 0.20f : 0.14f);
            g.drawImage(dashTex, bounds, juce::RectanglePlacement::stretchToFit);
            g.setOpacity(1.0f);
        }

        // Warm orange tint
        g.setColour(accent.withAlpha(0.04f));
        g.fillRect(bounds);
    }

    g.restoreState();

    // ---- Border ----
    if (hovered && enabled)
    {
        g.setColour(accent.withAlpha(0.55f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 14.0f, 1.5f);
    }
    else
    {
        g.setColour(enabled ? juce::Colours::black.withAlpha(0.40f) : juce::Colours::white.withAlpha(0.03f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 14.0f, 1.0f);
        if (enabled)
        {
            g.setColour(metalLight.withAlpha(0.22f));
            g.drawRoundedRectangle(bounds.reduced(1.5f), 13.0f, 0.5f);
        }
    }

    // ---- Top accent glow ----
    if (enabled)
    {
        auto topGlow = bounds.reduced(14.0f, 0.0f).removeFromTop(2.5f);
        juce::ColourGradient glowGrad(juce::Colours::transparentBlack, topGlow.getX(), topGlow.getCentreY(),
            accentBright.withAlpha(hovered ? 0.65f : 0.38f), topGlow.getCentreX(), topGlow.getCentreY(), false);
        glowGrad.addColour(1.0, juce::Colours::transparentBlack);
        g.setGradientFill(glowGrad);
        g.fillRoundedRectangle(topGlow, 1.5f);
    }

    // ---- Header bar ----
    auto header = bounds.reduced(6.0f).removeFromTop((float)headerHeight);
    g.setColour(enabled ? juce::Colour::fromString("ff1A1410").interpolatedWith(accent, 0.12f)
                        : juce::Colour::fromString("ff0D1520"));
    g.fillRoundedRectangle(header, 10.0f);

    g.setColour(enabled ? juce::Colours::white.withAlpha(0.80f) : juce::Colours::white.withAlpha(0.32f));
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawText(name.toUpperCase(),
        header.toNearestInt().reduced(8, 0).withTrimmedRight(72),
        juce::Justification::centredLeft);

    // ---- Body content ----
    auto bodyArea = bounds.reduced(8.0f);
    bodyArea.removeFromTop((float)headerHeight + 6.0f);

    // Type badge + status
    auto topBand = bodyArea.removeFromTop(26.0f);
    auto badgeBounds = topBand.removeFromLeft(58.0f);
    g.setColour(accent.withAlpha(enabled ? 0.28f : 0.10f));
    g.fillRoundedRectangle(badgeBounds, 6.0f);
    g.setColour(accent.withAlpha(enabled ? 0.95f : 0.40f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawFittedText(badge, badgeBounds.toNearestInt().reduced(6, 0), juce::Justification::centred, 1);

    auto statePill = topBand.removeFromRight(56.0f);
    const auto stateCol = enabled ? juce::Colour::fromString("ff34D399") : juce::Colour::fromString("ffEF4444");
    g.setColour(stateCol.withAlpha(0.18f));
    g.fillRoundedRectangle(statePill, 6.0f);
    g.setColour(stateCol);
    g.drawFittedText(enabled ? "ACTIVE" : "BYPASS", statePill.toNearestInt(), juce::Justification::centred, 1);

    // Name
    auto nameArea = bodyArea.removeFromTop(34.0f);
    g.setColour(enabled ? juce::Colours::white.withAlpha(0.96f) : juce::Colours::white.withAlpha(0.46f));
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    g.drawFittedText(name, nameArea.toNearestInt(), juce::Justification::centredLeft, 2);

    // Subtitle
    auto subtitleArea = bodyArea.removeFromTop(24.0f);
    g.setColour(accent.withAlpha(enabled ? 0.62f : 0.30f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawFittedText(subtitle, subtitleArea.toNearestInt(), juce::Justification::centredLeft, 2);

    // ---- Footer ----
    auto footer = bounds.reduced(8.0f).removeFromBottom(20.0f);
    // Accent bar
    auto accentBar = footer.removeFromLeft(44.0f).withHeight(3.0f).withY(footer.getCentreY() - 1.0f);
    juce::ColourGradient barGrad(accentBright.withAlpha(enabled ? (hovered ? 0.95f : 0.80f) : 0.25f),
        accentBar.getX(), accentBar.getCentreY(),
        accent.withAlpha(enabled ? (hovered ? 0.85f : 0.60f) : 0.15f),
        accentBar.getRight(), accentBar.getCentreY(), false);
    g.setGradientFill(barGrad);
    g.fillRoundedRectangle(accentBar, 1.5f);

    g.setColour(juce::Colours::white.withAlpha(enabled ? (hovered ? 0.75f : 0.48f) : 0.24f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText("EDIT", footer.toNearestInt().withTrimmedLeft(52), juce::Justification::centredLeft);

    // ---- Bypass overlay ----
    if (!enabled)
    {
        auto overlay = bounds.reduced(6.0f);
        overlay.removeFromTop((float)headerHeight);

        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(overlay, 10.0f);

        // Dashed line
        const float midY = overlay.getCentreY();
        const float dashLen = 8.0f, gapLen = 5.0f;
        g.setColour(accentDeep.withAlpha(0.45f));
        float cx = overlay.getX() + 16.0f;
        while (cx < overlay.getRight() - 16.0f)
        {
            const float segEnd = juce::jmin(cx + dashLen, overlay.getRight() - 16.0f);
            g.drawLine(cx, midY, segEnd, midY, 2.0f);
            cx = segEnd + gapLen;
        }

        // X mark
        const float iconX = overlay.getCentreX();
        g.setColour(juce::Colour::fromString("ffEF4444").withAlpha(0.55f));
        g.drawLine(iconX - 6.0f, midY - 6.0f, iconX + 6.0f, midY + 6.0f, 2.0f);
        g.drawLine(iconX + 6.0f, midY - 6.0f, iconX - 6.0f, midY + 6.0f, 2.0f);
    }
}

inline void registerOverdriveDashboard()
{
    auto& reg = Nova::PedalUI::getUIRegistry();
    auto it = reg.find("Overdrive");
    if (it != reg.end())
        it->second.dashboardPaint = paintDashboard;
    else
        Nova::PedalUI::registerPedalUI("Overdrive", { nullptr, paintDashboard });
}

}
