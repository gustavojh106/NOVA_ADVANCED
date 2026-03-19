#pragma once

#include <JuceHeader.h>
#include <functional>
#include "../../../Core/PedalCatalog.h"

namespace Nova::PedalUI
{

// Paint function signatures for per-pedal UI customization
using ThumbnailPaintFn = std::function<void(juce::Graphics& g, juce::Rectangle<float> bounds,
    const juce::String& name, const juce::String& subtitle, bool hovered)>;

using DashboardPaintFn = std::function<void(juce::Graphics& g, juce::Rectangle<float> bounds,
    const juce::String& name, const juce::String& subtitle, const juce::String& badge,
    bool enabled, bool hovered, int headerHeight)>;

struct PedalUIEntry
{
    ThumbnailPaintFn  thumbnailPaint;
    DashboardPaintFn  dashboardPaint;
    int dashboardWidth  = 0;   // 0 = use default
    int dashboardHeight = 0;
};

// Central registry
inline std::map<juce::String, PedalUIEntry>& getUIRegistry()
{
    static std::map<juce::String, PedalUIEntry> registry;
    return registry;
}

inline void registerPedalUI(const juce::String& typeID, PedalUIEntry entry)
{
    getUIRegistry()[typeID] = std::move(entry);
}

inline const PedalUIEntry* findPedalUI(const juce::String& typeID)
{
    auto& reg = getUIRegistry();
    auto it = reg.find(typeID);
    return (it != reg.end()) ? &it->second : nullptr;
}

}
