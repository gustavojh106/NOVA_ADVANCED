#pragma once
#include <JuceHeader.h>
#include <functional>
#include <map>

// Incluimos todos los pedales disponibles aquí (ÚNICO lugar donde se hace hardcode de includes)
#include "../Effects/Pedals/Overdrive/OverdrivePedal.h"
#include "../Effects/Cabinets/CabinetPedal.h"
// #include "../Effects/Pedals/Neural/NeuralPedal.h" (Comentado por ahora)

class PedalRegistry
{
public:
    // Definimos el tipo de función "Creadora"
    using PedalCreator = std::function<std::unique_ptr<juce::AudioProcessor>()>;

    static std::unique_ptr<juce::AudioProcessor> createPedal(const juce::String& pedalType)
    {
        // Mapa estático: Nombre -> Función que crea el pedal
        static const std::map<juce::String, PedalCreator> factory = {
            { "Overdrive", []() { return std::make_unique<OverdrivePedal>(); } },
            { "Cabinet",   []() { return std::make_unique<CabinetPedal>(); } }
            // { "Neural",    []() { return std::make_unique<NeuralPedal>(); } }
        };

        // Buscamos en el mapa
        auto it = factory.find(pedalType);
        if (it != factory.end())
        {
            return it->second(); // Ejecutamos la función creadora
        }

        return nullptr; // No encontrado
    }
};