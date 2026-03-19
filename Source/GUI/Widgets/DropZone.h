#pragma once
#include <JuceHeader.h>
#include <optional>
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
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;

    // Mouse
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

private:
    struct DropPreview
    {
        bool valid = false;
        int insertIndex = -1;
        int slotIndex = -1;
        juce::Rectangle<float> markerBounds;
    };

    bool isFixedSlot() const noexcept;
    juce::Rectangle<float> getDropContentBounds() const;
    juce::String getHelpText() const;
    juce::String getDraggedPedalType(const juce::String& dragInfo) const;
    bool isValidDragType(const juce::String& dragInfo) const;
    bool isMoveDrag(const juce::String& dragInfo) const;
    DropPreview calculateDropPreview(int dropX) const;
    std::optional<int> getDraggedPedalIndex(const juce::String& dragInfo) const;
    void updateDropPreview(const juce::String& dragInfo, juce::Point<int> localPos);
    void clearDropPreview();
    void handleMoveDrop(const juce::String& dragInfo, const DropPreview& preview);
    void handleAddDrop(const juce::String& dragInfo, const DropPreview& preview);

    void triggerShake();
    void timerCallback() override;

    void drawTechGrid(juce::Graphics& g) const;
    void drawDashedOutline(juce::Graphics& g, juce::Colour color, juce::Rectangle<float> area) const;

    NOVAAudioProcessor& proc;
    Nova::ChainID chain;
    Nova::ZoneID zone;

    enum class DragState { None, Valid, Invalid };
    DragState dragState = DragState::None;
    DropPreview currentPreview;

    bool isHoveringInfo = false;
    juce::Rectangle<float> infoIconBounds;

    // Reserved padding areas (never covered by pedal cards)
    static constexpr int kTopReserved    = 28;   // space for (i) icon
    static constexpr int kBottomReserved = 44;   // space for zone name + ADD button

    juce::TextButton addButton{ "+" };

    int shakeOffset = 0;
    int shakeTicks = 0;

    // Floating tooltip overlay
    std::unique_ptr<FloatingTooltip> tooltipOverlay;
};
