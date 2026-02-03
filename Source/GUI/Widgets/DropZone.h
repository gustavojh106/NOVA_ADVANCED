#pragma once
#include <JuceHeader.h>
#include "../../Core/PluginProcessor.h"
#include "../../Core/Constants.h"

class NOVAAudioProcessorEditor; 

class DropZone : public juce::Component, public juce::DragAndDropTarget
{
public:
    DropZone(NOVAAudioProcessor& p, Nova::ChainID c, Nova::ZoneID z);

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails& d) override;

    void itemDragEnter(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void paint(juce::Graphics& g) override;
private:
    bool isFixedSlot() const noexcept;
    void setHover(bool shouldHover);
    void drawTechGrid(juce::Graphics& g) const;
    void drawOutline(juce::Graphics& g, bool fixed) const;
    void drawFixedSlotUI(juce::Graphics& g) const;
    void drawNormalSlotUI(juce::Graphics& g) const;

    NOVAAudioProcessor& proc;
    Nova::ChainID chain;
    Nova::ZoneID zone;
    bool isHover = false;
};
