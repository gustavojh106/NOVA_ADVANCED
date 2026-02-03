#include "AssetItem.h"

AssetItem::AssetItem(const juce::String& name, const juce::String& type, std::function<void()> onSelect)
    : itemName(name), itemType(type), onSelectCallback(onSelect)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void AssetItem::mouseUp(const juce::MouseEvent&) { if (onSelectCallback) onSelectCallback(); }
void AssetItem::mouseEnter(const juce::MouseEvent&) { isHover = true; repaint(); }
void AssetItem::mouseExit(const juce::MouseEvent&) { isHover = false; repaint(); }

void AssetItem::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour(isHover ? juce::Colours::white.withAlpha(0.1f) : juce::Colours::transparentBlack);
    g.fillRoundedRectangle(area, 6.0f);

    auto iconArea = area.removeFromTop(area.getHeight() * 0.7f).reduced(10);
    g.setColour(juce::Colour::fromString("ff202020"));
    g.fillRoundedRectangle(iconArea, 4.0f);
    g.setColour(juce::Colours::white.withAlpha(0.2f));
    g.drawRoundedRectangle(iconArea, 4.0f, 1.0f);

    // Dibujo simple del icono basado en tipo
    if (itemType == "Amp") {
        g.setColour(juce::Colours::grey);
        float yKnob = iconArea.getCentreY();
        for (int i = 0; i < 4; ++i) g.fillEllipse(iconArea.getX() + 10 + (i * 15), yKnob - 4, 8, 8);
    }
    else { // Cab o Pedal
        g.setColour(juce::Colours::black.withAlpha(0.3f));
        auto c = iconArea.getCentre();
        g.fillEllipse(c.x - 15, c.y - 15, 12, 12);
        g.fillEllipse(c.x + 3, c.y - 15, 12, 12);
        g.fillEllipse(c.x - 15, c.y + 3, 12, 12);
        g.fillEllipse(c.x + 3, c.y + 3, 12, 12);
    }

    g.setColour(Nova::Colors::Text);
    g.setFont(14.0f);
    g.drawText(itemName, area, juce::Justification::centred);

    if (isHover) {
        g.setColour(Nova::Colors::CableOnA); // Cyan highlight
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 6.0f, 1.5f);
    }
}