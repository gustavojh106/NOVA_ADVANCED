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
    const char* subtitle;
    const char* searchTokens;
    const char* accentHex;
    Kind kind;
    bool quickAccess = true;
};

inline const std::array<Entry, 7>& entries() noexcept
{
    static constexpr std::array<Entry, 7> data{ {
        { "Compressor", "Compressor", "Studio sustain", "compressor dynamics sustain punch clean leveler", "ff62d8b2", Kind::Pedal, true },
        { "Overdrive", "Overdrive", "Articulate gain", "overdrive drive dirt boost crunch rhythm solo", "fff36f45", Kind::Pedal, true },
        { "Chorus", "Chorus", "Stereo motion", "chorus modulation width shimmer dimension spread", "ff72c1ff", Kind::Pedal, true },
        { "Delay", "Delay", "Spatial repeats", "delay echoes ambient repeats slapback space", "ff7cb8ff", Kind::Pedal, true },
        { "Reverb", "Reverb", "Lush ambience", "reverb room hall plate ambient trail wash", "ff98d9d1", Kind::Pedal, true },
        { "Classic Amp", "Classic Amp", "British head", "amp amplifier head crunch lead stack", "fff4b942", Kind::Amplifier, true },
        { "Cabinet", "Cabinet", "Atlas 4x12", "cabinet speaker mic room 4x12 resonance", "ff6bd1ff", Kind::Cabinet, true }
    } };

    return data;
}

inline const Entry* findEntry(const juce::String& requestedType) noexcept
{
    const auto cleaned = requestedType.trim();

    for (const auto& e : entries())
    {
        if (cleaned.equalsIgnoreCase(e.typeID) || cleaned.equalsIgnoreCase(e.displayName))
            return &e;
    }

    return nullptr;
}

inline Kind kindFromType(const juce::String& requestedType)
{
    if (const auto* entry = findEntry(requestedType))
        return entry->kind;

    const auto cleaned = requestedType.trim();

    if (cleaned.containsIgnoreCase("amp"))
        return Kind::Amplifier;
    if (cleaned.containsIgnoreCase("cab"))
        return Kind::Cabinet;

    return Kind::Pedal;
}

inline juce::String canonicalType(const juce::String& requestedType)
{
    if (const auto* entry = findEntry(requestedType))
        return juce::String(entry->typeID);

    return requestedType.trim();
}

inline bool isKnownType(const juce::String& requestedType)
{
    return findEntry(requestedType) != nullptr;
}

inline juce::Colour accentForType(const juce::String& requestedType)
{
    if (const auto* entry = findEntry(requestedType))
        return juce::Colour::fromString(entry->accentHex);

    return juce::Colour::fromString("ff6bd1ff");
}

inline juce::String subtitleForType(const juce::String& requestedType)
{
    if (const auto* entry = findEntry(requestedType))
        return juce::String(entry->subtitle);

    return {};
}

inline bool matchesFilter(const Entry& entry, const juce::String& filter)
{
    const auto trimmed = filter.trim();
    if (trimmed.isEmpty())
        return true;

    juce::String haystack;
    haystack << entry.typeID << " "
        << entry.displayName << " "
        << entry.subtitle << " "
        << entry.searchTokens;

    return haystack.containsIgnoreCase(trimmed);
}

inline juce::String badgeForKind(Kind kind)
{
    switch (kind)
    {
        case Kind::Amplifier: return "Amp";
        case Kind::Cabinet:   return "Cab";
        case Kind::Pedal:
        default:              return "Pedal";
    }
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
