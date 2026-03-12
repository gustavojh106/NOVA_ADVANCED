/*
  ==============================================================================
    Source/Core/Constants.h
    -----------------------
    Single source of truth for NOVA.
    Combines:
    1. Logical enums (chains, zones, modes)
    2. ValueTree identifiers
    3. Color palette
    4. Technical constants
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

namespace Nova
{
    enum class ChainID { LineA, LineB };
    enum class ZoneID { Pre = 0, Amp, FX, Cabinet };
    enum class SwitcherMode { LineA_Only, LineB_Only, Dual_Parallel };
    enum class InputRouting { Stereo, Left, Right, Sum };

    namespace IDs
    {
        static const juce::Identifier MAIN_STATE("NOVA_STATE");
        static const juce::Identifier SETTINGS("SETTINGS");

        static const juce::Identifier LINE_A("LINE_A");
        static const juce::Identifier LINE_B("LINE_B");

        static const juce::Identifier ENGINE_ON("engineOn");
        static const juce::Identifier SWITCH_MODE("switchMode");

        static const juce::Identifier PEDAL("PEDAL");
        static const juce::Identifier PEDAL_ID("id");
        static const juce::Identifier PEDAL_TYPE("type");
        static const juce::Identifier PEDAL_ZONE("zone");
        static const juce::Identifier PEDAL_ENABLED("enabled");
        static const juce::Identifier PEDAL_STATE("state");

        static const juce::Identifier INPUT_GAIN("inGain");
        static const juce::Identifier INPUT_GATE("gateThresh");
        static const juce::Identifier INPUT_TRANS("transpose");
        static const juce::Identifier FORCE_MONO("forceMono");

        static const juce::Identifier MIXER_GAIN_A("gainA");
        static const juce::Identifier MIXER_PAN_A("panA");
        static const juce::Identifier MIXER_WIDTH_A("widthA");

        static const juce::Identifier MIXER_GAIN_B("gainB");
        static const juce::Identifier MIXER_PAN_B("panB");
        static const juce::Identifier MIXER_WIDTH_B("widthB");

        static const juce::Identifier OUTPUT_VOL("outVol");
        static const juce::Identifier OUTPUT_LIMITER("limiter");
        static const juce::Identifier OUTPUT_MIX("globalMix");
    }

    namespace Colors
    {
        const juce::Colour Background = juce::Colour::fromString("ff111111");
        const juce::Colour Panel = juce::Colour::fromString("ff1a1a1a");
        const juce::Colour MixerPanel = juce::Colour::fromString("ff1e1e1e");

        const juce::Colour Border = juce::Colour::fromString("ff444444");
        const juce::Colour GridLine = juce::Colour::fromString("ff333333");
        const juce::Colour ZoneOutline = juce::Colour::fromString("ff555555");

        const juce::Colour Text = juce::Colours::white;
        const juce::Colour TextDim = juce::Colours::grey;

        const juce::Colour Accent = juce::Colour::fromString("ff00ff00");
        const juce::Colour Error = juce::Colour::fromString("ffea2e2e");

        const juce::Colour CableOff = juce::Colour::fromString("ff333333");
        const juce::Colour CableOnA = juce::Colour::fromString("ff00aaff");
        const juce::Colour CableOnB = juce::Colour::fromString("ffffaa00");
    }

    namespace Config
    {
        static constexpr int TUNER_FIFO_SIZE = 16384;
        static constexpr int TUNER_PROCESS_SIZE = 4096;
        static constexpr int MAX_GRAPH_LATENCY_SAMPLES = 8192;

        static constexpr int HEADER_HEIGHT = 80;
        static constexpr int FOOTER_HEIGHT = 100;
    }
}