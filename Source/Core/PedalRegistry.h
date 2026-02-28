#pragma once

#include <JuceHeader.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "PedalCatalog.h"

// Registered pedals
#include "../Effects/Pedals/Overdrive/OverdrivePedal.h"
#include "../Effects/Cabinets/CabinetPedal.h"
#include "../Effects/Amplifiers/ClassicAmp.h"

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
            { "Overdrive",   [] { return std::make_unique<OverdrivePedal>(); } },
            { "Cabinet",     [] { return std::make_unique<CabinetPedal>(); } },
            { "Classic Amp", [] { return std::make_unique<ClassicAmp>(); } }
        };

        return factory;
    }

    PedalRegistry() = delete;
};
