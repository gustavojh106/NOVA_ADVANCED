#pragma once

#include <JuceHeader.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "PedalCatalog.h"

// Registered pedals
#include "../Effects/Pedals/CompressorPedal.h"
#include "../Effects/Pedals/Boost/BoostPedal.h"
#include "../Effects/Pedals/Gate/NoiseGatePedal.h"
#include "../Effects/Pedals/EQ/EQPedal.h"
#include "../Effects/Pedals/Overdrive/OverdrivePedal.h"
#include "../Effects/Pedals/Distortion/DistortionPedal.h"
#include "../Effects/Pedals/Metal/MetalDistortionPedal.h"
#include "../Effects/Pedals/Fuzz/FuzzPedal.h"
#include "../Effects/Pedals/Wah/ClassicWahPedal.h"
#include "../Effects/Pedals/Wah/AutoWahPedal.h"
#include "../Effects/Pedals/Octave/OctavePedal.h"
#include "../Effects/Pedals/ChorusPedal.h"
#include "../Effects/Pedals/Phaser/PhaserPedal.h"
#include "../Effects/Pedals/Flanger/FlangerPedal.h"
#include "../Effects/Pedals/Tremolo/TremoloPedal.h"
#include "../Effects/Pedals/Delay/DelayPedal.h"
#include "../Effects/Pedals/Reverb/ReverbPedal.h"
#include "../Effects/Cabinets/CabinetPedal.h"
#include "../Effects/Cabinets/Vintage2x12Cabinet.h"
#include "../Effects/Cabinets/Modern4x12Cabinet.h"
#include "../Effects/Amplifiers/ClassicAmp.h"
#include "../Effects/Amplifiers/HighGainAmp.h"
#include "../Effects/Amplifiers/CleanAmp.h"
#include "../Effects/Amplifiers/ChimeAmp.h"
#include "../Effects/Amplifiers/BoutiqueAmp.h"

class PedalRegistry final
{
public:
    using PedalCreator = std::function<std::unique_ptr<juce::AudioProcessor>()>;

    static juce::String canonicalType(const juce::String& requestedType)
    {
        return Nova::PedalCatalog::canonicalType(requestedType);
    }

    static bool isTypeSupported(const juce::String& requestedType)
    {
        const auto typeID = canonicalType(requestedType);
        const auto& factory = getFactory();
        return factory.find(typeID) != factory.end();
    }

    static std::vector<juce::String> getPedalTypesForZone(Nova::ZoneID zone)
    {
        std::vector<juce::String> types;
        const auto& factory = getFactory();

        for (const auto& e : Nova::PedalCatalog::entries())
        {
            if (!Nova::PedalCatalog::canLiveInZone(e.typeID, zone))
                continue;

            if (factory.find(e.typeID) == factory.end())
                continue;

            types.emplace_back(e.typeID);
        }

        return types;
    }

    static std::unique_ptr<juce::AudioProcessor> createPedal(const juce::String& requestedType)
    {
        const auto typeID = canonicalType(requestedType);
        const auto& factory = getFactory();

        if (auto it = factory.find(typeID); it != factory.end())
            return it->second();

        return nullptr;
    }

private:
    static const std::map<juce::String, PedalCreator>& getFactory()
    {
        static const std::map<juce::String, PedalCreator> factory{
            // Pedals
            { "Compressor",    [] { return std::make_unique<CompressorPedal>(); } },
            { "Boost",         [] { return std::make_unique<BoostPedal>(); } },
            { "Noise Gate",    [] { return std::make_unique<NoiseGatePedal>(); } },
            { "EQ",            [] { return std::make_unique<EQPedal>(); } },
            { "Overdrive",     [] { return std::make_unique<OverdrivePedal>(); } },
            { "Distortion",    [] { return std::make_unique<DistortionPedal>(); } },
            { "Metal Distortion", [] { return std::make_unique<MetalDistortionPedal>(); } },
            { "Fuzz",          [] { return std::make_unique<FuzzPedal>(); } },
            { "Wah",           [] { return std::make_unique<ClassicWahPedal>(); } },
            { "Auto Wah",      [] { return std::make_unique<AutoWahPedal>(); } },
            { "Octave",        [] { return std::make_unique<OctavePedal>(); } },
            { "Chorus",        [] { return std::make_unique<ChorusPedal>(); } },
            { "Phaser",        [] { return std::make_unique<PhaserPedal>(); } },
            { "Flanger",       [] { return std::make_unique<FlangerPedal>(); } },
            { "Tremolo",       [] { return std::make_unique<TremoloPedal>(); } },
            { "Delay",         [] { return std::make_unique<DelayPedal>(); } },
            { "Reverb",        [] { return std::make_unique<ReverbPedal>(); } },
            // Amplifiers
            { "Classic Amp",   [] { return std::make_unique<ClassicAmp>(); } },
            { "High Gain Amp", [] { return std::make_unique<HighGainAmp>(); } },
            { "Clean Amp",     [] { return std::make_unique<CleanAmp>(); } },
            { "Chime Amp",     [] { return std::make_unique<ChimeAmp>(); } },
            { "Boutique Amp",  [] { return std::make_unique<BoutiqueAmp>(); } },
            // Cabinets
            { "Cabinet",       [] { return std::make_unique<CabinetPedal>(); } },
            { "Vintage 2x12",  [] { return std::make_unique<Vintage2x12Cabinet>(); } },
            { "Modern 4x12",   [] { return std::make_unique<Modern4x12Cabinet>(); } }
        };

        return factory;
    }

    PedalRegistry() = delete;
};
