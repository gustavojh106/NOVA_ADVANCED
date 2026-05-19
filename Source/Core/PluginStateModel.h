#pragma once

#include <algorithm>
#include <cmath>
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

inline bool isNumericScalar(const juce::var& value) noexcept
{
    return value.isBool() || value.isInt() || value.isInt64() || value.isDouble();
}

inline double readFiniteNumberOr(const juce::var& value, double fallback) noexcept
{
    if (!isNumericScalar(value))
        return fallback;

    const auto numeric = static_cast<double>(value);
    return std::isfinite(numeric) ? numeric : fallback;
}

inline void sanitizeBoolProperty(juce::ValueTree tree,
                                 const juce::Identifier& property,
                                 bool fallback)
{
    if (!tree.isValid())
        return;

    const auto raw = tree.getProperty(property, fallback);
    const auto value = raw.isBool()
        ? static_cast<bool>(raw)
        : (isNumericScalar(raw) ? (readFiniteNumberOr(raw, fallback ? 1.0 : 0.0) != 0.0) : fallback);

    tree.setProperty(property, value, nullptr);
}

inline void sanitizeIntProperty(juce::ValueTree tree,
                                const juce::Identifier& property,
                                int fallback,
                                int minimum,
                                int maximum)
{
    if (!tree.isValid())
        return;

    const auto raw = tree.getProperty(property, fallback);
    const auto numeric = readFiniteNumberOr(raw, (double) fallback);
    tree.setProperty(property, juce::jlimit(minimum, maximum, juce::roundToInt(numeric)), nullptr);
}

inline void sanitizeFloatProperty(juce::ValueTree tree,
                                  const juce::Identifier& property,
                                  float fallback,
                                  float minimum,
                                  float maximum)
{
    if (!tree.isValid())
        return;

    const auto raw = tree.getProperty(property, fallback);
    const auto numeric = readFiniteNumberOr(raw, (double) fallback);
    tree.setProperty(property, (float) juce::jlimit((double) minimum, (double) maximum, numeric), nullptr);
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
        sanitizeBoolProperty(settings, IDs::ENGINE_ON, false);
        sanitizeIntProperty(settings, IDs::SWITCH_MODE, (int) SwitcherMode::LineA_Only,
            (int) SwitcherMode::LineA_Only, (int) SwitcherMode::Dual_Parallel);
        sanitizeFloatProperty(settings, IDs::INPUT_GAIN, 0.0f, -60.0f, 24.0f);
        sanitizeFloatProperty(settings, IDs::INPUT_GATE, -100.0f, -100.0f, 0.0f);
        sanitizeBoolProperty(settings, IDs::FORCE_MONO, false);
        sanitizeFloatProperty(settings, IDs::OUTPUT_VOL, 0.0f, -60.0f, 12.0f);
        sanitizeFloatProperty(settings, IDs::OUTPUT_LIMITER, 0.0f, -12.0f, 0.0f);
        sanitizeFloatProperty(settings, IDs::OUTPUT_MIX, 100.0f, 0.0f, 100.0f);
    }

    auto lineA = getLineTree(state, ChainID::LineA);
    if (lineA.isValid())
    {
        sanitizeFloatProperty(lineA, IDs::MIXER_GAIN_A, 1.0f, 0.0f, 2.0f);
        sanitizeFloatProperty(lineA, IDs::MIXER_PAN_A, 0.0f, -1.0f, 1.0f);
        sanitizeFloatProperty(lineA, IDs::MIXER_WIDTH_A, 1.0f, 0.0f, 2.0f);
    }

    auto lineB = getLineTree(state, ChainID::LineB);
    if (lineB.isValid())
    {
        sanitizeFloatProperty(lineB, IDs::MIXER_GAIN_B, 1.0f, 0.0f, 2.0f);
        sanitizeFloatProperty(lineB, IDs::MIXER_PAN_B, 0.0f, -1.0f, 1.0f);
        sanitizeFloatProperty(lineB, IDs::MIXER_WIDTH_B, 1.0f, 0.0f, 2.0f);
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
    int preCount = 0;
    int fxCount = 0;

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

        if (finalZone == ZoneID::Pre)
        {
            if (preCount >= Config::MAX_PEDALS_PER_FLEX_ZONE)
                continue;

            ++preCount;
        }
        else if (finalZone == ZoneID::FX)
        {
            if (fxCount >= Config::MAX_PEDALS_PER_FLEX_ZONE)
                continue;

            ++fxCount;
        }

        child.setProperty(IDs::PEDAL_TYPE, canonicalType, nullptr);
        child.setProperty(IDs::PEDAL_ZONE, static_cast<int>(finalZone), nullptr);
        sanitizeBoolProperty(child, IDs::PEDAL_ENABLED, true);
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
