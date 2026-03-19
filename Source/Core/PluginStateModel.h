#pragma once

#include <algorithm>
#include <vector>

#include <JuceHeader.h>

#include "Constants.h"
#include "PedalRegistry.h"

namespace Nova::PluginStateModel
{
struct PedalInsertResult
{
    bool inserted = false;
    int index = -1;
    juce::String canonicalType;
    ZoneID zone = ZoneID::Pre;
    juce::String pedalID;
};

inline juce::ValueTree getSettingsTree(const juce::ValueTree& state)
{
    return state.getChildWithName(IDs::SETTINGS);
}

inline juce::ValueTree getLineTree(const juce::ValueTree& state, ChainID chain)
{
    const auto id = (chain == ChainID::LineA) ? IDs::LINE_A : IDs::LINE_B;
    return state.getChildWithName(id);
}

inline int getStateSchemaVersion(const juce::ValueTree& state) noexcept
{
    return state.isValid()
        ? static_cast<int>(state.getProperty(IDs::STATE_SCHEMA_VERSION, 0))
        : 0;
}

inline void stampStateSchema(juce::ValueTree state)
{
    if (!state.isValid())
        return;

    state.setProperty(IDs::STATE_SCHEMA_VERSION, Config::STATE_SCHEMA_VERSION, nullptr);
}

inline int zoneSortRank(ZoneID zone)
{
    switch (zone)
    {
        case ZoneID::Pre:     return 0;
        case ZoneID::Amp:     return 1;
        case ZoneID::FX:      return 2;
        case ZoneID::Cabinet: return 3;
        default:              return 4;
    }
}

inline void ensureStructure(juce::ValueTree state)
{
    if (!state.isValid())
        return;

    stampStateSchema(state);

    if (!getSettingsTree(state).isValid())
        state.appendChild(juce::ValueTree(IDs::SETTINGS), nullptr);

    if (!getLineTree(state, ChainID::LineA).isValid())
        state.appendChild(juce::ValueTree(IDs::LINE_A), nullptr);

    if (!getLineTree(state, ChainID::LineB).isValid())
        state.appendChild(juce::ValueTree(IDs::LINE_B), nullptr);
}

inline void applyDefaultValues(juce::ValueTree state)
{
    ensureStructure(state);

    auto settings = getSettingsTree(state);
    if (settings.isValid())
    {
        if (!settings.hasProperty(IDs::ENGINE_ON))
            settings.setProperty(IDs::ENGINE_ON, false, nullptr);
        if (!settings.hasProperty(IDs::SWITCH_MODE))
            settings.setProperty(IDs::SWITCH_MODE, (int)SwitcherMode::LineA_Only, nullptr);
        if (!settings.hasProperty(IDs::INPUT_GAIN))
            settings.setProperty(IDs::INPUT_GAIN, 0.0f, nullptr);
        if (!settings.hasProperty(IDs::INPUT_GATE))
            settings.setProperty(IDs::INPUT_GATE, -100.0f, nullptr);
        if (!settings.hasProperty(IDs::FORCE_MONO))
            settings.setProperty(IDs::FORCE_MONO, false, nullptr);
        if (!settings.hasProperty(IDs::INPUT_TRANS))
            settings.setProperty(IDs::INPUT_TRANS, 0, nullptr);
        if (!settings.hasProperty(IDs::OUTPUT_VOL))
            settings.setProperty(IDs::OUTPUT_VOL, 0.0f, nullptr);
        if (!settings.hasProperty(IDs::OUTPUT_LIMITER))
            settings.setProperty(IDs::OUTPUT_LIMITER, 0.0f, nullptr);
        if (!settings.hasProperty(IDs::OUTPUT_MIX))
            settings.setProperty(IDs::OUTPUT_MIX, 100.0f, nullptr);
    }

    auto lineA = getLineTree(state, ChainID::LineA);
    if (lineA.isValid())
    {
        if (!lineA.hasProperty(IDs::MIXER_GAIN_A))
            lineA.setProperty(IDs::MIXER_GAIN_A, 1.0f, nullptr);
        if (!lineA.hasProperty(IDs::MIXER_PAN_A))
            lineA.setProperty(IDs::MIXER_PAN_A, 0.0f, nullptr);
        if (!lineA.hasProperty(IDs::MIXER_WIDTH_A))
            lineA.setProperty(IDs::MIXER_WIDTH_A, 1.0f, nullptr);
    }

    auto lineB = getLineTree(state, ChainID::LineB);
    if (lineB.isValid())
    {
        if (!lineB.hasProperty(IDs::MIXER_GAIN_B))
            lineB.setProperty(IDs::MIXER_GAIN_B, 1.0f, nullptr);
        if (!lineB.hasProperty(IDs::MIXER_PAN_B))
            lineB.setProperty(IDs::MIXER_PAN_B, 0.0f, nullptr);
        if (!lineB.hasProperty(IDs::MIXER_WIDTH_B))
            lineB.setProperty(IDs::MIXER_WIDTH_B, 1.0f, nullptr);
    }
}

inline void sanitizeLine(juce::ValueTree line)
{
    if (!line.isValid())
        return;

    struct SanitizedPedal
    {
        juce::ValueTree state;
        int originalIndex = 0;
        ZoneID zone = ZoneID::Pre;
    };

    std::vector<SanitizedPedal> sanitized;
    sanitized.reserve((size_t) line.getNumChildren());
    bool sawAmp = false;
    bool sawCabinet = false;

    for (int i = 0; i < line.getNumChildren(); ++i)
    {
        auto child = line.getChild(i);
        if (!child.hasType(IDs::PEDAL))
            continue;

        const auto canonicalType = PedalRegistry::canonicalType(
            child.getProperty(IDs::PEDAL_TYPE).toString());

        if (!PedalRegistry::isTypeSupported(canonicalType))
            continue;

        const auto zone = static_cast<ZoneID>(
            (int) child.getProperty(IDs::PEDAL_ZONE, (int) ZoneID::Pre));
        const auto finalZone = PedalCatalog::enforceZone(canonicalType, zone);

        if (finalZone == ZoneID::Amp)
        {
            if (sawAmp)
                continue;

            sawAmp = true;
        }

        if (finalZone == ZoneID::Cabinet)
        {
            if (sawCabinet)
                continue;

            sawCabinet = true;
        }

        child.setProperty(IDs::PEDAL_TYPE, canonicalType, nullptr);
        child.setProperty(IDs::PEDAL_ZONE, static_cast<int>(finalZone), nullptr);
        if (!child.hasProperty(IDs::PEDAL_ENABLED))
            child.setProperty(IDs::PEDAL_ENABLED, true, nullptr);
        if (!child.hasProperty(IDs::PEDAL_ID))
            child.setProperty(IDs::PEDAL_ID, juce::Uuid().toString(), nullptr);

        sanitized.push_back({ child.createCopy(), i, finalZone });
    }

    std::stable_sort(sanitized.begin(), sanitized.end(),
        [](const SanitizedPedal& lhs, const SanitizedPedal& rhs)
        {
            const auto leftRank = zoneSortRank(lhs.zone);
            const auto rightRank = zoneSortRank(rhs.zone);
            if (leftRank != rightRank)
                return leftRank < rightRank;

            return lhs.originalIndex < rhs.originalIndex;
        });

    line.removeAllChildren(nullptr);

    for (auto& pedal : sanitized)
        line.appendChild(pedal.state, nullptr);
}

inline void canonicalizeStateTree(juce::ValueTree state)
{
    if (!state.isValid())
        return;

    ensureStructure(state);
    applyDefaultValues(state);
    sanitizeLine(getLineTree(state, ChainID::LineA));
    sanitizeLine(getLineTree(state, ChainID::LineB));
}

inline juce::ValueTree makeCanonicalCopy(const juce::ValueTree& state)
{
    auto copy = state.createCopy();
    canonicalizeStateTree(copy);
    return copy;
}

inline void resetToCleanState(juce::ValueTree state)
{
    if (!state.isValid())
        return;

    state.removeAllChildren(nullptr);
    ensureStructure(state);

    auto lineA = getLineTree(state, ChainID::LineA);
    auto lineB = getLineTree(state, ChainID::LineB);

    if (lineA.isValid())
        lineA.removeAllChildren(nullptr);

    if (lineB.isValid())
        lineB.removeAllChildren(nullptr);

    auto settings = getSettingsTree(state);
    if (settings.isValid())
    {
        settings.setProperty(IDs::ENGINE_ON, false, nullptr);
        settings.setProperty(IDs::SWITCH_MODE, (int) SwitcherMode::LineA_Only, nullptr);
        settings.setProperty(IDs::INPUT_GAIN, 0.0f, nullptr);
        settings.setProperty(IDs::INPUT_GATE, -100.0f, nullptr);
        settings.setProperty(IDs::FORCE_MONO, false, nullptr);
        settings.setProperty(IDs::INPUT_TRANS, 0, nullptr);
        settings.setProperty(IDs::OUTPUT_VOL, 0.0f, nullptr);
        settings.setProperty(IDs::OUTPUT_LIMITER, 0.0f, nullptr);
        settings.setProperty(IDs::OUTPUT_MIX, 100.0f, nullptr);
    }

    if (lineA.isValid())
    {
        lineA.setProperty(IDs::MIXER_GAIN_A, 1.0f, nullptr);
        lineA.setProperty(IDs::MIXER_PAN_A, 0.0f, nullptr);
        lineA.setProperty(IDs::MIXER_WIDTH_A, 1.0f, nullptr);
    }

    if (lineB.isValid())
    {
        lineB.setProperty(IDs::MIXER_GAIN_B, 1.0f, nullptr);
        lineB.setProperty(IDs::MIXER_PAN_B, 0.0f, nullptr);
        lineB.setProperty(IDs::MIXER_WIDTH_B, 1.0f, nullptr);
    }
}

inline int countPedalsInZone(const juce::ValueTree& state, ChainID chain, ZoneID zone)
{
    auto line = getLineTree(state, chain);
    if (!line.isValid())
        return 0;

    int count = 0;
    for (int i = 0; i < line.getNumChildren(); ++i)
    {
        auto child = line.getChild(i);
        if (!child.hasType(IDs::PEDAL))
            continue;
        const auto childZone = static_cast<ZoneID>(
            (int)child.getProperty(IDs::PEDAL_ZONE, (int)ZoneID::Pre));
        if (childZone == zone)
            ++count;
    }
    return count;
}

inline PedalInsertResult insertPedal(juce::ValueTree state,
    const juce::String& type,
    ChainID chain,
    ZoneID requestedZone,
    int requestedInsertIndex = -1)
{
    PedalInsertResult result;
    result.canonicalType = PedalRegistry::canonicalType(type);
    if (!PedalRegistry::isTypeSupported(result.canonicalType))
        return result;

    result.zone = PedalCatalog::enforceZone(result.canonicalType, requestedZone);
    auto line = getLineTree(state, chain);
    if (!line.isValid())
        return result;

    // Capacity gate for flex zones (Pre / FX)
    if (result.zone == ZoneID::Pre || result.zone == ZoneID::FX)
    {
        if (countPedalsInZone(state, chain, result.zone) >= Config::MAX_PEDALS_PER_FLEX_ZONE)
            return result;
    }

    if (result.zone == ZoneID::Amp || result.zone == ZoneID::Cabinet)
    {
        for (int i = line.getNumChildren(); --i >= 0;)
        {
            auto child = line.getChild(i);
            if (!child.hasType(IDs::PEDAL))
                continue;

            const auto childZone = static_cast<ZoneID>(
                (int) child.getProperty(IDs::PEDAL_ZONE, (int) ZoneID::Pre));

            if (childZone == result.zone)
                line.removeChild(i, nullptr);
        }
    }

    const auto targetRank = zoneSortRank(result.zone);
    int zoneStart = line.getNumChildren();
    int zoneEnd = line.getNumChildren();
    bool foundZoneStart = false;

    for (int i = 0; i < line.getNumChildren(); ++i)
    {
        auto child = line.getChild(i);
        if (!child.hasType(IDs::PEDAL))
            continue;

        const auto childZone = static_cast<ZoneID>(
            (int) child.getProperty(IDs::PEDAL_ZONE, (int) ZoneID::Pre));
        const auto childRank = zoneSortRank(childZone);

        if (!foundZoneStart && childRank >= targetRank)
        {
            zoneStart = i;
            foundZoneStart = true;
        }

        if (childRank > targetRank)
        {
            zoneEnd = i;
            break;
        }
    }

    if (!foundZoneStart)
        zoneStart = line.getNumChildren();

    result.index = requestedInsertIndex >= 0
        ? juce::jlimit(zoneStart, zoneEnd, requestedInsertIndex)
        : zoneEnd;

    if (result.zone == ZoneID::Amp || result.zone == ZoneID::Cabinet)
        result.index = zoneStart;

    result.index = juce::jlimit(0, line.getNumChildren(), result.index);

    if (result.zone == ZoneID::Amp || result.zone == ZoneID::Cabinet)
    {
        result.index = juce::jlimit(0, line.getNumChildren(), zoneStart);
    }

    juce::ValueTree newPedal(IDs::PEDAL);
    result.pedalID = juce::Uuid().toString();
    newPedal.setProperty(IDs::PEDAL_TYPE, result.canonicalType, nullptr);
    newPedal.setProperty(IDs::PEDAL_ZONE, static_cast<int>(result.zone), nullptr);
    newPedal.setProperty(IDs::PEDAL_ENABLED, true, nullptr);
    newPedal.setProperty(IDs::PEDAL_ID, result.pedalID, nullptr);
    line.addChild(newPedal, result.index, nullptr);

    result.inserted = true;
    return result;
}

inline bool movePedal(juce::ValueTree state,
    ChainID chain,
    int fromIndex,
    int toIndex,
    ZoneID targetZone);

inline bool movePedal(juce::ValueTree state, ChainID chain, int fromIndex, int toIndex)
{
    auto line = getLineTree(state, chain);
    if (!line.isValid())
        return false;

    if (!juce::isPositiveAndBelow(fromIndex, line.getNumChildren()))
        return false;

    auto movedChild = line.getChild(fromIndex);
    if (!movedChild.hasType(IDs::PEDAL))
        return false;

    const auto movedZone = static_cast<ZoneID>(
        (int)movedChild.getProperty(IDs::PEDAL_ZONE, (int)ZoneID::Pre));

    return movePedal(state, chain, fromIndex, toIndex, movedZone);
}

inline bool movePedal(juce::ValueTree state,
    ChainID chain,
    int fromIndex,
    int toIndex,
    ZoneID targetZone)
{
    auto line = getLineTree(state, chain);
    if (!line.isValid())
        return false;

    const int count = line.getNumChildren();
    if (!juce::isPositiveAndBelow(fromIndex, count) || toIndex < 0 || toIndex > count)
        return false;

    auto movedChild = line.getChild(fromIndex);
    if (!movedChild.hasType(IDs::PEDAL))
        return false;

    const auto movedType = PedalRegistry::canonicalType(
        movedChild.getProperty(IDs::PEDAL_TYPE).toString());
    if (!PedalRegistry::isTypeSupported(movedType))
        return false;

    const auto finalZone = PedalCatalog::enforceZone(movedType, targetZone);
    if (!PedalCatalog::canLiveInZone(movedType, finalZone))
        return false;

    const auto movedZone = static_cast<ZoneID>(
        (int)movedChild.getProperty(IDs::PEDAL_ZONE, (int)ZoneID::Pre));

    int adjustedTarget = toIndex;
    if (fromIndex < toIndex)
        adjustedTarget--;

    adjustedTarget = juce::jlimit(0, line.getNumChildren() - 1, adjustedTarget);

    if (movedZone == finalZone && adjustedTarget == fromIndex)
        return false;

    auto copy = movedChild.createCopy();
    line.removeChild(fromIndex, nullptr);

    adjustedTarget = juce::jlimit(0, line.getNumChildren(), adjustedTarget);
    copy.setProperty(IDs::PEDAL_ZONE, static_cast<int>(finalZone), nullptr);
    line.addChild(copy, adjustedTarget, nullptr);

    return true;
}

inline bool removePedal(juce::ValueTree state, ChainID chain, int index)
{
    auto line = getLineTree(state, chain);
    if (!line.isValid() || !juce::isPositiveAndBelow(index, line.getNumChildren()))
        return false;

    line.removeChild(index, nullptr);
    return true;
}

inline bool setPedalEnabled(juce::ValueTree state, ChainID chain, int index, bool enabled)
{
    auto line = getLineTree(state, chain);
    if (!line.isValid() || !juce::isPositiveAndBelow(index, line.getNumChildren()))
        return false;

    auto child = line.getChild(index);
    if (!child.hasType(IDs::PEDAL))
        return false;

    child.setProperty(IDs::PEDAL_ENABLED, enabled, nullptr);
    return true;
}
}
