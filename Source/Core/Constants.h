/*
  ==============================================================================
    Source/Core/Constants.h
    -----------------------
    La fuente de la verdad del proyecto NOVA.
    Fusiona:
    1. Enums lógicos (Chain, Zones, Modes)
    2. Identificadores de ValueTree (Base de datos)
    3. Paleta de Colores (Estilo visual)
    4. Constantes Técnicas (Buffers, defaults)
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

namespace Nova
{
    // ==============================================================================
    // 1. ENUMS LÓGICOS
    // ==============================================================================

    // Identifica las dos líneas de procesamiento
    enum class ChainID { LineA, LineB };

    // Zonas obligatorias para los pedales
    enum class ZoneID { Pre = 0, Amp, FX, Cabinet };

    // Modos del Switcher Global
    enum class SwitcherMode { LineA_Only, LineB_Only, Dual_Parallel };

    // Modos de Ruteo de Entrada (Para el InputChain inteligente)
    enum class InputRouting { Stereo, Left, Right, Sum };

    // ==============================================================================
    // 2. IDENTIFICADORES (ValueTree IDs)
    // ==============================================================================
    namespace IDs
    {
        // --- Estructura Principal ---
        static const juce::Identifier MAIN_STATE("NOVA_STATE");
        static const juce::Identifier SETTINGS("SETTINGS");

        // Cadenas de pedales
        static const juce::Identifier LINE_A("LINE_A");
        static const juce::Identifier LINE_B("LINE_B");

        // --- Configuración Global ---
        static const juce::Identifier ENGINE_ON("engineOn");
        static const juce::Identifier SWITCH_MODE("switchMode");

        // --- Propiedades de Pedales (Nodo Hijo) ---
        static const juce::Identifier PEDAL("PEDAL");       // Nombre del nodo XML (Antes PEDAL_TAG)
        static const juce::Identifier PEDAL_ID("id");       // UUID único (NUEVO: Para gestión robusta)
        static const juce::Identifier PEDAL_TYPE("type");   // Ej: "Overdrive"
        static const juce::Identifier PEDAL_ZONE("zone");   // int (ZoneID)
        static const juce::Identifier PEDAL_ENABLED("enabled"); // bool (Bypass individual)

        // --- INPUT SECTION ---
        static const juce::Identifier INPUT_GAIN("inGain");      // -60 a +24 dB
        static const juce::Identifier INPUT_GATE("gateThresh");  // -100 a 0 dB
        static const juce::Identifier INPUT_TRANS("transpose");  // -12 a +12 semitonos
        static const juce::Identifier FORCE_MONO("forceMono");   // bool (Legacy, mapea a InputRouting)

        // --- MIXER SECTION (LINE A) ---
        static const juce::Identifier MIXER_GAIN_A("gainA");
        static const juce::Identifier MIXER_PAN_A("panA");       // -1.0 a 1.0
        static const juce::Identifier MIXER_WIDTH_A("widthA");   // 0.0 a 2.0

        // --- MIXER SECTION (LINE B) ---
        static const juce::Identifier MIXER_GAIN_B("gainB");
        static const juce::Identifier MIXER_PAN_B("panB");
        static const juce::Identifier MIXER_WIDTH_B("widthB");

        // --- OUTPUT SECTION ---
        static const juce::Identifier OUTPUT_VOL("outVol");      // -60 a +12 dB
        static const juce::Identifier OUTPUT_LIMITER("limiter"); // Threshold
        static const juce::Identifier OUTPUT_MIX("globalMix");   // 0.0 a 1.0
    }

    // ==============================================================================
    // 3. PALETA DE COLORES (Migrado de Stylesheet.h)
    // ==============================================================================
    namespace Colors
    {
        // Fondos
        const juce::Colour Background = juce::Colour::fromString("ff111111");
        const juce::Colour Panel = juce::Colour::fromString("ff1a1a1a");
        const juce::Colour MixerPanel = juce::Colour::fromString("ff1e1e1e");

        // Elementos UI
        const juce::Colour Border = juce::Colour::fromString("ff444444");
        const juce::Colour GridLine = juce::Colour::fromString("ff333333");
        const juce::Colour ZoneOutline = juce::Colour::fromString("ff555555");

        // Textos
        const juce::Colour Text = juce::Colours::white;
        const juce::Colour TextDim = juce::Colours::grey;

        // Énfasis
        const juce::Colour Accent = juce::Colour::fromString("ff00ff00"); // Verde Nova
        const juce::Colour Error = juce::Colour::fromString("ffea2e2e"); // Rojo Clip

        // Cables (Signal Flow)
        const juce::Colour CableOff = juce::Colour::fromString("ff333333");
        const juce::Colour CableOnA = juce::Colour::fromString("ff00aaff"); // Cyan
        const juce::Colour CableOnB = juce::Colour::fromString("ffffaa00"); // Naranja
    }

    // ==============================================================================
    // 4. CONSTANTES TÉCNICAS (Configuración DSP)
    // ==============================================================================
    namespace Config
    {
        // Afinador (Sincronización Engine <-> GUI)
        static constexpr int TUNER_FIFO_SIZE = 16384;
        static constexpr int TUNER_PROCESS_SIZE = 4096;

        // Layout (Opcional, para consistencia visual)
        static constexpr int HEADER_HEIGHT = 80;
        static constexpr int FOOTER_HEIGHT = 100;
    }
}