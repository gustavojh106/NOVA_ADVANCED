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
        static const juce::Identifier STATE_SCHEMA_VERSION("schemaVersion");

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
        // ---- Base surfaces (deep space navy, warm dark blue) ----
        const juce::Colour Background = juce::Colour::fromString("ff0B0E14");
        const juce::Colour Panel      = juce::Colour::fromString("ff111827");
        const juce::Colour MixerPanel  = juce::Colour::fromString("ff1A2332");
        const juce::Colour Surface     = juce::Colour::fromString("ff1A2332");

        // ---- Borders & grid ----
        const juce::Colour Border      = juce::Colour::fromString("ff2A3548");
        const juce::Colour GridLine    = juce::Colour::fromString("ff1E2A3A");
        const juce::Colour ZoneOutline = juce::Colour::fromString("ff3A4A5C");

        // ---- Typography ----
        const juce::Colour Text    = juce::Colour::fromString("ffF0EDE8");
        const juce::Colour TextDim = juce::Colour::fromString("ff7B8BA0");

        // ---- Accent & semantics ----
        const juce::Colour Accent    = juce::Colour::fromString("ffF0A030");  // warm amber/gold
        const juce::Colour AccentAlt = juce::Colour::fromString("ffE8853A");  // warm orange
        const juce::Colour Success   = juce::Colour::fromString("ff34D399");  // emerald green
        const juce::Colour Error     = juce::Colour::fromString("ffEF4444");  // rose red

        // ---- Signal cables ----
        const juce::Colour CableOff = juce::Colour::fromString("ff2D3748");
        const juce::Colour CableOnA = juce::Colour::fromString("ff60A5FA");
        const juce::Colour CableOnB = juce::Colour::fromString("ffF59E0B");
    }

    namespace Config
    {
        static constexpr int TUNER_FIFO_SIZE = 16384;
        static constexpr int TUNER_PROCESS_SIZE = 4096;
        static constexpr int MAX_GRAPH_LATENCY_SAMPLES = 8192;
        static constexpr int STATE_SCHEMA_VERSION = 1;

        static constexpr int HEADER_HEIGHT = 80;
        static constexpr int FOOTER_HEIGHT = 100;

        // ---- Audio Engine startup / recovery ----
        static constexpr int    STARTUP_COUNTER_INIT          = 5;      // blocks to skip at cold prepare
        static constexpr int    STARTUP_COUNTER_GRAPH_CHANGE  = 6;      // blocks to skip after topology rebuild
        static constexpr int    STARTUP_COUNTER_AUTO_HEAL     = 8;      // blocks to skip after auto-heal reset
        static constexpr int    RECOVERY_COOLDOWN_BLOCKS      = 256;    // cooldown after successful auto-heal
        static constexpr int    RECOVERY_COOLDOWN_SHORT       = 32;     // cooldown when lock was unavailable
        static constexpr int    CORRUPT_BLOCKS_BEFORE_HEAL    = 2;      // consecutive corrupt blocks before heal

        // ---- Audio safety limits ----
        static constexpr float  HARD_ABS_LIMIT_LINEAR         = 24.0f;  // hard clip ceiling (≈ +27.6 dBFS)
        static constexpr float  INPUT_ACTIVE_THRESHOLD        = 0.003f; // peak threshold for "input is live"
        static constexpr float  OUTPUT_SILENT_THRESHOLD       = 0.00008f; // peak threshold for "output is silent"
        static constexpr float  SILENT_OUTPUT_TRIGGER_SECONDS = 0.75f;  // sustained silence before incident

        // ---- CPU metering ----
        static constexpr double CPU_METER_SLOW_COEFF = 0.9; // slow-path weight (exponential moving avg)
        static constexpr double CPU_METER_FAST_COEFF = 0.1; // fast-path weight

        // ---- Parameter smoothing (seconds) ----
        static constexpr double SMOOTH_DEFAULT_SECONDS = 0.02;  // general-purpose control ramp time
        static constexpr double SMOOTH_DRIVE_SECONDS   = 0.012; // gain/drive stage ramp time
        static constexpr double SMOOTH_BYPASS_SECONDS  = 0.01;  // bypass crossfade ramp time

        // ---- Drive stage internal ----
        static constexpr float DC_OFFSET_DECAY  = 0.9993f; // per-sample DC tracking pole (Overdrive)
        static constexpr float DC_OFFSET_ATTACK = 0.0007f; // per-sample DC tracking zero (Overdrive)
        static constexpr float AMP_SAG_DECAY    = 0.9992f; // per-sample sag envelope decay (ClassicAmp power-supply sim)

        // ---- Flex zone layout (Pre / FX multi-row grid) ----
        static constexpr int MAX_PEDALS_PER_FLEX_ZONE = 12;
    }
}
