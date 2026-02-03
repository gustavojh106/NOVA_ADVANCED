#pragma once

#include <JuceHeader.h>
#include <functional>
#include <map>
#include <memory>

// Pedales registrados (sí: esto aumenta tiempos de build, pero mantiene el estilo header-only)
#include "../Effects/Pedals/Overdrive/OverdrivePedal.h"
#include "../Effects/Cabinets/CabinetPedal.h"

class PedalRegistry final
{
public:
    using PedalCreator = std::function<std::unique_ptr<juce::AudioProcessor>()>;

    static std::unique_ptr<juce::AudioProcessor> createPedal(const juce::String& type)
    {
        const auto& factory = getFactory();
        if (auto it = factory.find(type); it != factory.end())
            return it->second();

        return nullptr;
    }

private:
    static const std::map<juce::String, PedalCreator>& getFactory()
    {
        static const std::map<juce::String, PedalCreator> factory{
            { "Overdrive", [] { return std::make_unique<OverdrivePedal>(); } },
            { "Cabinet",   [] { return std::make_unique<CabinetPedal>(); } },
            // { "MyAmp",   [] { return std::make_unique<MyAmp>(); } },
        };
        return factory;
    }

    PedalRegistry() = delete;
};
