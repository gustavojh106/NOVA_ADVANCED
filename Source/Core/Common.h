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

        // --- INPUT SECTION ---
        static const juce::Identifier INPUT_GAIN("inGain");     // -60 a +24 dB
        static const juce::Identifier INPUT_GATE("gateThresh"); // -100 a 0 dB
        static const juce::Identifier INPUT_TRANS("transpose"); // -12 a +12 semitonos
        static const juce::Identifier FORCE_MONO("forceMono");  // bool

        // --- MIXER SECTION ---
        static const juce::Identifier MIXER_GAIN_A("gainA");
        static const juce::Identifier MIXER_PAN_A("panA");     // -1.0 (L) a 1.0 (R)
        static const juce::Identifier MIXER_WIDTH_A("widthA"); // 0.0 (Mono) a 2.0 (Super Wide)

        static const juce::Identifier MIXER_GAIN_B("gainB");
        static const juce::Identifier MIXER_PAN_B("panB");
        static const juce::Identifier MIXER_WIDTH_B("widthB");

        // --- OUTPUT SECTION ---
        static const juce::Identifier OUTPUT_VOL("outVol");     // -60 a +12 dB
        static const juce::Identifier OUTPUT_LIMITER("limiter"); // Threshold
        static const juce::Identifier OUTPUT_MIX("globalMix");   // 0.0 a 1.0
    }
}