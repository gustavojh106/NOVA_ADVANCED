#pragma once

#include <JuceHeader.h>
#include <array>

#include "Constants.h"

namespace Nova::PedalCatalog
{
enum class Kind
{
    Pedal,
    Amplifier,
    Cabinet
};

struct Entry
{
    const char* typeID;
    const char* displayName;
    Kind kind;
};

inline const std::array<Entry, 3>& entries() noexcept
{
    static constexpr std::array<Entry, 3> data{ {
        { "Overdrive",  "Overdrive",  Kind::Pedal },
        { "Classic Amp", "Classic Amp", Kind::Amplifier },
        { "Cabinet",    "Cabinet",    Kind::Cabinet }
    } };

    return data;
}

inline Kind kindFromType(const juce::String& requestedType)
{
    const auto cleaned = requestedType.trim();

    for (const auto& e : entries())
    {
        if (cleaned.equalsIgnoreCase(e.typeID) || cleaned.equalsIgnoreCase(e.displayName))
            return e.kind;
    }

    if (cleaned.containsIgnoreCase("amp"))
        return Kind::Amplifier;
    if (cleaned.containsIgnoreCase("cab"))
        return Kind::Cabinet;

    return Kind::Pedal;
}

inline juce::String canonicalType(const juce::String& requestedType)
{
    const auto cleaned = requestedType.trim();

    for (const auto& e : entries())
    {
        if (cleaned.equalsIgnoreCase(e.typeID) || cleaned.equalsIgnoreCase(e.displayName))
            return juce::String(e.typeID);
    }

    return cleaned;
}

inline bool isKnownType(const juce::String& requestedType)
{
    const auto canonical = canonicalType(requestedType);

    for (const auto& e : entries())
        if (canonical.equalsIgnoreCase(e.typeID))
            return true;

    return false;
}

inline ZoneID enforceZone(const juce::String& requestedType, ZoneID requestedZone)
{
    switch (kindFromType(requestedType))
    {
        case Kind::Amplifier: return ZoneID::Amp;
        case Kind::Cabinet:   return ZoneID::Cabinet;
        case Kind::Pedal:
        default:
            return (requestedZone == ZoneID::Amp || requestedZone == ZoneID::Cabinet)
                ? ZoneID::Pre
                : requestedZone;
    }
}

inline bool canLiveInZone(const juce::String& requestedType, ZoneID zone)
{
    switch (kindFromType(requestedType))
    {
        case Kind::Amplifier: return zone == ZoneID::Amp;
        case Kind::Cabinet:   return zone == ZoneID::Cabinet;
        case Kind::Pedal:     return zone == ZoneID::Pre || zone == ZoneID::FX;
        default:              return false;
    }
}
}

