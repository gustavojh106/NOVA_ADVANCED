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
    const char* accentBrightHex;
    Kind kind;
    bool quickAccess = true;
};

inline juce::String resolveAlias(const juce::String& requestedType)
{
    const auto cleaned = requestedType.trim();

    if (cleaned.equalsIgnoreCase("Auto Wah")
        || cleaned.equalsIgnoreCase("Autowah")
        || cleaned.equalsIgnoreCase("AutoWah"))
        return "Wah";
    if (cleaned.equalsIgnoreCase("Metal Distortion"))
        return "Distortion";

    return cleaned;
}

inline const std::array<Entry, 24>& entries() noexcept
{
    static constexpr std::array<Entry, 24> data{ {
        // ---- Pedals ----
        { "Compressor", "Compressor", "Studio sustain", "compressor dynamics sustain punch clean leveler", "ff34D399", "ff6EE7B7", Kind::Pedal, true },
        { "Noise Gate", "Noise Gate", "Studio gate", "gate noise studio hysteresis lookahead focus sidechain clamp expander silencer high gain", "ff10B981", "ff34D399", Kind::Pedal, true },
        { "EQ", "EQ", "3-Band EQ", "eq equalizer tone shape bass mid treble parametric", "ff4ADE80", "ff86EFAC", Kind::Pedal, true },
        { "Boost", "Boost", "Micro boost", "boost preamp clean boost micro amp ep booster level push", "ffEAB308", "ffFDE047", Kind::Pedal, true },
        { "Neural", "Neural", "Adaptive preamp", "neural adaptive drive preamp ai tone model dynamic articulation", "ff22D3EE", "ff67E8F9", Kind::Pedal, true },
        { "Overdrive", "Overdrive", "Articulate gain", "overdrive drive dirt boost crunch rhythm solo", "ffF06848", "ffFF9B70", Kind::Pedal, true },
        { "Distortion", "Distortion", "Studio high gain", "distortion dist heavy metal modern studio gate high gain clip hard stack palm mute", "ffEF4444", "ffFCA5A5", Kind::Pedal, true },
        { "Fuzz", "Fuzz", "Velvet Fuzz", "fuzz vintage germanium silicon octave stoner doom", "ffA855F7", "ffC4B5FD", Kind::Pedal, true },
        { "Wah", "Wah", "Unified expressive wah", "wah treadle autowah envelope hybrid cry vocal filter sweep cocked wah", "ffD97706", "ffFBBF24", Kind::Pedal, true },
        { "Octave", "Octave", "Octave divider", "octave divider sub octave upper octave oc style pitch", "ff22C55E", "ff4ADE80", Kind::Pedal, true },
        { "Chorus", "Chorus", "Stereo motion", "chorus modulation width shimmer dimension spread", "ff818CF8", "ffA5B4FC", Kind::Pedal, true },
        { "Phaser", "Phaser", "Phase shift", "phaser phase modulation sweep jet swirl", "ffC084FC", "ffD8B4FE", Kind::Pedal, true },
        { "Flanger", "Flanger", "Jet flange", "flanger jet modulation sweep comb filter", "ff38BDF8", "ff7DD3FC", Kind::Pedal, true },
        { "Tremolo", "Tremolo", "Pulse", "tremolo amplitude modulation pulse chop vibrato", "ffFBBF24", "ffFDE68A", Kind::Pedal, true },
        { "Delay", "Delay", "Spatial repeats", "delay echoes ambient repeats slapback space", "ff60A5FA", "ff93C5FD", Kind::Pedal, true },
        { "Reverb", "Reverb", "Lush ambience", "reverb room hall plate ambient trail wash", "ff2DD4BF", "ff5EEAD4", Kind::Pedal, true },
        // ---- Amplifiers ----
        { "Classic Amp", "Classic Amp", "British head", "amp amplifier head crunch lead stack british marshall", "ffF0A030", "ffFBBF24", Kind::Amplifier, true },
        { "High Gain Amp", "High Gain Amp", "Inferno", "amp amplifier high gain metal mesa rectifier 5150 modern", "ffDC2626", "ffF87171", Kind::Amplifier, true },
        { "Clean Amp", "Clean Amp", "Crystal", "amp amplifier clean fender twin jazz pristine warm", "ff60A5FA", "ff93C5FD", Kind::Amplifier, true },
        { "Chime Amp", "Chime Amp", "Top Boost", "amp amplifier vox ac30 chime chimey class a jangle brit bright", "ff22D3EE", "ff67E8F9", Kind::Amplifier, true },
        { "Boutique Amp", "Boutique Amp", "Velvet", "amp amplifier boutique dumble two rock transparent touch dynamic clean", "ffA78BFA", "ffC4B5FD", Kind::Amplifier, true },
        // ---- Cabinets ----
        { "Cabinet", "Cabinet", "Atlas 4x12", "cabinet speaker mic room 4x12 resonance", "ffA78BFA", "ffC4B5FD", Kind::Cabinet, true },
        { "Vintage 2x12", "Vintage 2x12", "Vintage 2x12", "cabinet speaker 2x12 vintage warm open back", "ffD97706", "ffFBBF24", Kind::Cabinet, true },
        { "Modern 4x12", "Modern 4x12", "Modern 4x12", "cabinet speaker 4x12 modern tight v30 metal", "ff6366F1", "ff818CF8", Kind::Cabinet, true }
    } };

    return data;
}

inline const Entry* findEntry(const juce::String& requestedType) noexcept
{
    const auto cleaned = resolveAlias(requestedType);

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

    const auto cleaned = resolveAlias(requestedType);

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

    return resolveAlias(requestedType);
}

inline bool isKnownType(const juce::String& requestedType)
{
    return findEntry(requestedType) != nullptr;
}

inline juce::Colour accentForType(const juce::String& requestedType)
{
    if (const auto* entry = findEntry(requestedType))
        return juce::Colour::fromString(entry->accentHex);

    return juce::Colour::fromString("ffA78BFA");
}

inline juce::Colour accentBrightForType(const juce::String& requestedType)
{
    if (const auto* entry = findEntry(requestedType))
        return juce::Colour::fromString(entry->accentBrightHex);

    return juce::Colour::fromString("ffC4B5FD");
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
