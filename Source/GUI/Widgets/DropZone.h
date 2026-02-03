#pragma once
#include <JuceHeader.h>
#include "../../Core/PluginProcessor.h"
#include "../../Core/Constants.h"

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
    NOVAAudioProcessor& proc;
    Nova::ChainID chain;
    Nova::ZoneID zone;
    bool isHover = false;
};