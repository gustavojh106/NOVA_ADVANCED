#pragma once
#include <JuceHeader.h>
#include "../../Core/PluginProcessor.h"
#include "../../Core/Constants.h"

// 1. Declaramos nuestra nueva clase flotante
class FloatingTooltip;

class DropZone : public juce::Component, public juce::DragAndDropTarget, private juce::Timer
{
public:
    DropZone(NOVAAudioProcessor& p, Nova::ChainID c, Nova::ZoneID z);
    ~DropZone() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Drag & Drop
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails& d) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;

    // Mouse
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

private:
    bool isFixedSlot() const noexcept;
    juce::String getHelpText() const;
    bool isValidDragType(const juce::String& dragInfo) const;

    void triggerShake();
    void timerCallback() override;

    void drawTechGrid(juce::Graphics& g) const;
    void drawDashedOutline(juce::Graphics& g, juce::Colour color, juce::Rectangle<float> area) const;

    NOVAAudioProcessor& proc;
    Nova::ChainID chain;
    Nova::ZoneID zone;

    enum class DragState { None, Valid, Invalid };
    DragState dragState = DragState::None;

    bool isHoveringInfo = false;
    juce::Rectangle<float> infoIconBounds;

    int shakeOffset = 0;
    int shakeTicks = 0;

    // 2. Nuestro componente flotante seguro
    std::unique_ptr<FloatingTooltip> tooltipOverlay;
};