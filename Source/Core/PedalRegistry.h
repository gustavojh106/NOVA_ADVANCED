#pragma once
#include <JuceHeader.h>
#include <map>
#include <functional>

// == TUS INCLUDES DE PEDALES ==
// Ajusta estas rutas si tus archivos están en otra carpeta
#include "../Effects/Pedals/Overdrive/OverdrivePedal.h" 
#include "../Effects/Cabinets/CabinetPedal.h"
// #include "../Effects/Amps/MyAmp.h" 

class PedalRegistry
{
public:
    using PedalCreator = std::function<std::unique_ptr<juce::AudioProcessor>()>;

    static std::unique_ptr<juce::AudioProcessor> createPedal(const juce::String& pedalType)
    {
        static const std::map<juce::String, PedalCreator> factory = {
            { "Overdrive", []() { return std::make_unique<OverdrivePedal>(); } },
            { "Cabinet",   []() { return std::make_unique<CabinetPedal>(); } }
            // Agrega tus futuros amplificadores aqui
        };

        auto it = factory.find(pedalType);
        if (it != factory.end()) return it->second();
        return nullptr;
    }
};