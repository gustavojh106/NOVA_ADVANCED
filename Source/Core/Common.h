#pragma once
#include <JuceHeader.h>

namespace Nova
{
    // Enum para las dos cadenas de audio
    enum class ChainID { LineA, LineB };

    // Enum para las 4 Zonas obligatorias
    enum class ZoneID { Pre = 0, Amp, FX, Cabinet };

    // Modos del Switcher Global
    enum class SwitcherMode { LineA_Only, LineB_Only, Dual_Parallel };

    // IDs para el ValueTree (Base de datos del estado)
    namespace IDs
    {
        static const juce::Identifier MAIN_STATE("NOVA_STATE");

        static const juce::Identifier LINE_A("LINE_A");
        static const juce::Identifier LINE_B("LINE_B");

        static const juce::Identifier PEDAL_TAG("PEDAL");
        static const juce::Identifier PEDAL_TYPE("type");
        static const juce::Identifier PEDAL_ZONE("zone"); // Guardamos en qué zona está (int)

        static const juce::Identifier SETTINGS("SETTINGS");
        static const juce::Identifier SWITCH_MODE("switchMode");
        static const juce::Identifier ENGINE_ON("engineOn");

        static const juce::Identifier MIXER_GAIN_A("gainA");
        static const juce::Identifier MIXER_GAIN_B("gainB");
    }
}