#pragma once

#include <JuceHeader.h>
#include "../Base/PedalUIFactory.h"

namespace Nova::ReverbUI
{

inline void paintDashboard(juce::Graphics& g, juce::Rectangle<float> bounds,
    const juce::String& name, const juce::String& subtitle, const juce::String& badge,
    bool enabled, bool hovered, int headerHeight)
{
    const auto accent      = juce::Colour::fromString("ff2DD4BF");
    const auto accentBright = juce::Colour::fromString("ff5EEAD4");
    const auto accentDeep  = juce::Colour::fromString("ff145A50");
    const auto baseDark    = juce::Colour::fromString("ff0A1018");
    const auto baseMid     = juce::Colour::fromString("ff101C28");
    const auto baseLight   = juce::Colour::fromString("ff182838");

    // Clip to rounded rect
    juce::Path clipPath;
    clipPath.addRoundedRectangle(bounds, 14.0f);
    g.saveState();
    g.reduceClipRegion(clipPath);

    // Enclosure body
    const auto topCol = enabled ? baseMid.interpolatedWith(accent, 0.08f) : juce::Colour::fromString("ff0A0A0A");
    const auto botCol = enabled ? baseDark : juce::Colour::fromString("ff060606");
    juce::ColourGradient body(topCol, bounds.getCentreX(), bounds.getY(),
        botCol, bounds.getCentreX(), bounds.getBottom(), false);
    if (enabled)
        body.addColour(0.35, baseLight.interpolatedWith(accent, 0.05f));
    g.setGradientFill(body);
    g.fillRect(bounds);

    // Teal mist
    if (enabled)
    {
        juce::ColourGradient mist(accent.withAlpha(hovered ? 0.08f : 0.04f),
            bounds.getCentreX(), bounds.getY(),
            juce::Colours::transparentBlack, bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(mist);
        g.fillRect(bounds);
    }

    g.restoreState();

    // Border
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
            g.setColour(baseLight.withAlpha(0.22f));
            g.drawRoundedRectangle(bounds.reduced(1.5f), 13.0f, 0.5f);
        }
    }

    // Top accent glow
    if (enabled)
    {
        auto topGlow = bounds.reduced(14.0f, 0.0f).removeFromTop(2.5f);
        juce::ColourGradient glowGrad(juce::Colours::transparentBlack, topGlow.getX(), topGlow.getCentreY(),
            accentBright.withAlpha(hovered ? 0.65f : 0.38f), topGlow.getCentreX(), topGlow.getCentreY(), false);
        glowGrad.addColour(1.0, juce::Colours::transparentBlack);
        g.setGradientFill(glowGrad);
        g.fillRoundedRectangle(topGlow, 1.5f);
    }

    // Header bar
    auto header = bounds.reduced(6.0f).removeFromTop((float)headerHeight);
    g.setColour(enabled ? juce::Colour::fromString("ff0D1A24").interpolatedWith(accent, 0.12f)
                        : juce::Colour::fromString("ff0D1520"));
    g.fillRoundedRectangle(header, 10.0f);

    g.setColour(enabled ? juce::Colours::white.withAlpha(0.80f) : juce::Colours::white.withAlpha(0.32f));
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawText(name.toUpperCase(),
        header.toNearestInt().reduced(8, 0).withTrimmedRight(72),
        juce::Justification::centredLeft);

    // Body content
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

    // Reverb tail decoration
    if (enabled)
    {
        auto vizArea = bodyArea.removeFromTop(juce::jmin(28.0f, bodyArea.getHeight() * 0.35f));
        const float waveY = vizArea.getCentreY();
        const float waveW = vizArea.getWidth() - 8.0f;
        const float waveX = vizArea.getX() + 4.0f;
        juce::Path wave, fill;
        wave.startNewSubPath(waveX, waveY);
        fill.startNewSubPath(waveX, waveY + 6.0f);
        fill.lineTo(waveX, waveY);
        for (int px = 0; px < (int)waveW; ++px)
        {
            float t = (float)px / waveW;
            float env = std::exp(-t * 3.0f) * 6.0f;
            float y = waveY - env * std::sin(t * 20.0f);
            wave.lineTo(waveX + (float)px, y);
            fill.lineTo(waveX + (float)px, y);
        }
        fill.lineTo(waveX + waveW, waveY + 6.0f);
        fill.closeSubPath();

        juce::ColourGradient fillGrad(accent.withAlpha(0.12f), waveX, vizArea.getY(),
            juce::Colours::transparentBlack, waveX, vizArea.getBottom(), false);
        g.setGradientFill(fillGrad);
        g.fillPath(fill);
        g.setColour(accent.withAlpha(hovered ? 0.65f : 0.45f));
        g.strokePath(wave, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));
    }

    // Footer
    auto footer = bounds.reduced(8.0f).removeFromBottom(20.0f);
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

    // Bypass overlay
    if (!enabled)
    {
        auto overlay = bounds.reduced(6.0f);
        overlay.removeFromTop((float)headerHeight);

        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(overlay, 10.0f);

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

        const float iconX = overlay.getCentreX();
        g.setColour(juce::Colour::fromString("ffEF4444").withAlpha(0.55f));
        g.drawLine(iconX - 6.0f, midY - 6.0f, iconX + 6.0f, midY + 6.0f, 2.0f);
        g.drawLine(iconX + 6.0f, midY - 6.0f, iconX - 6.0f, midY + 6.0f, 2.0f);
    }
}

inline void registerReverbDashboard()
{
    auto& reg = Nova::PedalUI::getUIRegistry();
    auto it = reg.find("Reverb");
    if (it != reg.end())
        it->second.dashboardPaint = paintDashboard;
    else
        Nova::PedalUI::registerPedalUI("Reverb", { nullptr, paintDashboard });
}

}
