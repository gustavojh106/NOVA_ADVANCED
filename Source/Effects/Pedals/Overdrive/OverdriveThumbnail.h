#pragma once

#include <JuceHeader.h>
#include "../Base/PedalUIFactory.h"
#include <BinaryData.h>

namespace Nova::OverdriveUI
{

inline void paintThumbnail(juce::Graphics& g, juce::Rectangle<float> bounds,
    const juce::String& name, const juce::String& subtitle, bool hovered)
{
    const auto accent      = juce::Colour::fromString("ffF06848");
    const auto accentBright = juce::Colour::fromString("ffFF9B70");
    const auto metalDark   = juce::Colour::fromString("ff1A1410");
    const auto metalMid    = juce::Colour::fromString("ff2A2018");
    const auto metalLight  = juce::Colour::fromString("ff3D3028");

    // Clip to rounded rect
    juce::Path clipPath;
    clipPath.addRoundedRectangle(bounds, 10.0f);
    g.saveState();
    g.reduceClipRegion(clipPath);

    // Enclosure body - dark base gradient
    juce::ColourGradient body(metalMid, bounds.getCentreX(), bounds.getY(),
        metalDark, bounds.getCentreX(), bounds.getBottom(), false);
    body.addColour(0.4, metalLight);
    g.setGradientFill(body);
    g.fillRect(bounds);

    // Real rusted orange metal texture
    static auto thumbTex = juce::ImageCache::getFromMemory(
        BinaryData::rusted_orange_metal_jpg, BinaryData::rusted_orange_metal_jpgSize);
    if (thumbTex.isValid())
    {
        g.setOpacity(hovered ? 0.22f : 0.15f);
        g.drawImage(thumbTex, bounds, juce::RectanglePlacement::stretchToFit);
        g.setOpacity(1.0f);
    }

    // Orange color tint
    g.setColour(accent.withAlpha(0.05f));
    g.fillRect(bounds);

    g.restoreState();

    // Border
    if (hovered)
    {
        g.setColour(accent.withAlpha(0.65f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.5f);
    }
    else
    {
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.0f);
        g.setColour(metalLight.withAlpha(0.35f));
        g.drawRoundedRectangle(bounds.reduced(1.5f), 9.0f, 0.5f);
    }

    // Orange accent strip (left)
    auto strip = bounds.withWidth(4.0f).reduced(0.0f, 5.0f);
    juce::ColourGradient stripGrad(accentBright, strip.getCentreX(), strip.getY(),
        accent, strip.getCentreX(), strip.getBottom(), false);
    g.setGradientFill(stripGrad);
    g.fillRoundedRectangle(strip, 2.0f);

    // Top glow strip
    if (hovered)
    {
        auto topGlow = bounds.reduced(14.0f, 0.0f).removeFromTop(2.0f);
        juce::ColourGradient glowGrad(juce::Colours::transparentBlack, topGlow.getX(), topGlow.getCentreY(),
            accent.withAlpha(0.55f), topGlow.getCentreX(), topGlow.getCentreY(), false);
        glowGrad.addColour(1.0, juce::Colours::transparentBlack);
        g.setGradientFill(glowGrad);
        g.fillRoundedRectangle(topGlow, 1.0f);
    }

    // Content
    auto content = bounds.reduced(14.0f, 5.0f);
    content.removeFromLeft(2.0f);

    // Pedal name
    g.setColour(juce::Colours::white.withAlpha(hovered ? 0.98f : 0.90f));
    g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    g.drawText(name, content.removeFromTop(18.0f).toNearestInt(),
        juce::Justification::centredLeft, true);

    // Subtitle
    g.setColour(accent.withAlpha(hovered ? 0.75f : 0.55f));
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText(subtitle, content.removeFromTop(14.0f).toNearestInt(),
        juce::Justification::centredLeft, true);

    // FX badge (right aligned)
    auto badgeBounds = juce::Rectangle<float>(
        bounds.getRight() - 40.0f,
        bounds.getCentreY() - 9.0f,
        30.0f, 18.0f);
    g.setColour(accent.withAlpha(hovered ? 0.35f : 0.20f));
    g.fillRoundedRectangle(badgeBounds, 5.0f);
    g.setColour(accent.withAlpha(hovered ? 0.95f : 0.78f));
    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    g.drawText("FX", badgeBounds.toNearestInt(), juce::Justification::centred);

    // Mini LED indicator dot
    const float ledX = bounds.getRight() - 14.0f;
    const float ledY = bounds.getY() + 10.0f;
    g.setColour(accent.withAlpha(hovered ? 0.92f : 0.55f));
    g.fillEllipse(ledX - 3.0f, ledY - 3.0f, 6.0f, 6.0f);
    if (hovered)
    {
        g.setColour(accentBright.withAlpha(0.25f));
        g.fillEllipse(ledX - 6.0f, ledY - 6.0f, 12.0f, 12.0f);
    }
}

inline void registerOverdriveThumbnail()
{
    auto& reg = Nova::PedalUI::getUIRegistry();
    auto it = reg.find("Overdrive");
    if (it != reg.end())
        it->second.thumbnailPaint = paintThumbnail;
    else
        Nova::PedalUI::registerPedalUI("Overdrive", { paintThumbnail, nullptr });
}

}
