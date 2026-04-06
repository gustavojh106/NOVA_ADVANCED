#pragma once

#include <JuceHeader.h>
#include "../Base/PedalUIFactory.h"

namespace Nova::ReverbUI
{

inline void paintThumbnail(juce::Graphics& g, juce::Rectangle<float> bounds,
    const juce::String& name, const juce::String& subtitle, bool hovered)
{
    const auto accent      = juce::Colour::fromString("ff2DD4BF");
    const auto accentBright = juce::Colour::fromString("ff5EEAD4");
    const auto baseDark    = juce::Colour::fromString("ff0A1018");
    const auto baseMid     = juce::Colour::fromString("ff101C28");
    const auto baseLight   = juce::Colour::fromString("ff182838");

    // Clip to rounded rect
    juce::Path clipPath;
    clipPath.addRoundedRectangle(bounds, 10.0f);
    g.saveState();
    g.reduceClipRegion(clipPath);

    // Enclosure body - deep dark gradient
    juce::ColourGradient body(baseMid, bounds.getCentreX(), bounds.getY(),
        baseDark, bounds.getCentreX(), bounds.getBottom(), false);
    body.addColour(0.35, baseLight);
    g.setGradientFill(body);
    g.fillRect(bounds);

    // Teal mist overlay
    {
        juce::ColourGradient mist(accent.withAlpha(hovered ? 0.10f : 0.05f),
            bounds.getCentreX(), bounds.getY(),
            juce::Colours::transparentBlack, bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(mist);
        g.fillRect(bounds);
    }

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
        g.setColour(baseLight.withAlpha(0.35f));
        g.drawRoundedRectangle(bounds.reduced(1.5f), 9.0f, 0.5f);
    }

    // Teal accent strip (left)
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

    // Mini wave decoration (reverb tail motif)
    {
        const float waveY = bounds.getBottom() - 14.0f;
        const float waveW = bounds.getWidth() * 0.6f;
        const float waveX = bounds.getX() + (bounds.getWidth() - waveW) * 0.5f;
        juce::Path wave;
        wave.startNewSubPath(waveX, waveY);
        for (int px = 0; px < (int)waveW; ++px)
        {
            float t = (float)px / waveW;
            float env = std::exp(-t * 3.5f) * 5.0f;
            float y = waveY - env * std::sin(t * 18.0f);
            wave.lineTo(waveX + (float)px, y);
        }
        g.setColour(accent.withAlpha(hovered ? 0.35f : 0.18f));
        g.strokePath(wave, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved));
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

inline void registerReverbThumbnail()
{
    auto& reg = Nova::PedalUI::getUIRegistry();
    auto it = reg.find("Reverb");
    if (it != reg.end())
        it->second.thumbnailPaint = paintThumbnail;
    else
        Nova::PedalUI::registerPedalUI("Reverb", { paintThumbnail, nullptr });
}

}
