#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <vector>

// =============================================================================
// Latency Autopilot (T2-10) - pure decision logic
//
// Ranks driver/device candidates for the lowest stable latency and decides
// whether the Standalone input can be unmuted safely. Nothing here touches the
// audio-device APIs: StandaloneAudioAutopilot.h builds the inventory from the
// AudioDeviceManager and applies the result, and AudioAutopilotTests (in
// AudioEngineTests.cpp) drives this file with synthetic inventories.
//
// Device names are the only signal available without native Windows code, so
// the classifiers are best-effort heuristics. Windows endpoint names look like
// "<endpoint> (<device>)", e.g. "Line (Steinberg UR22C)", and are localized,
// hence the Spanish tokens. Phase 4 replaces the bus / form-factor guesses with
// MMDevice properties (docs/reports/LAT-T2-10-latency-autopilot.md).
// =============================================================================
namespace Nova::AudioAutopilot
{
    // -------------------------------------------------------------------------
    // Classification
    // -------------------------------------------------------------------------

    enum class DriverFamily { Asio, WasapiShared, WasapiExclusive, WasapiLowLatency, DirectSound, Other };

    // Candidate quality for the Studio profile, best first.
    enum class Tier
    {
        AsioNativeVerified = 0, // vendor ASIO driver whose hardware is connected (or already running)
        WasapiExclusive,
        WasapiLowLatency,
        AsioNativeUnverified,   // vendor ASIO driver, but its hardware is not among the Windows endpoints
        AsioGeneric,            // ASIO4ALL, FlexASIO... wrappers around the Windows audio stack
        WasapiShared,
        DirectSound,
        Other
    };

    enum class AsioDriverClass { Native, Generic, Virtual };

    inline bool containsAny(const juce::String& lowerText, std::initializer_list<const char*> tokens)
    {
        for (auto* token : tokens)
            if (lowerText.contains(juce::CharPointer_UTF8(token)))
                return true;

        return false;
    }

    inline DriverFamily classifyDriverType(const juce::String& typeName)
    {
        if (typeName.containsIgnoreCase("ASIO"))         return DriverFamily::Asio;
        if (typeName.containsIgnoreCase("Exclusive"))    return DriverFamily::WasapiExclusive;
        if (typeName.containsIgnoreCase("Low Latency"))  return DriverFamily::WasapiLowLatency;
        if (typeName.containsIgnoreCase("Windows Audio")) return DriverFamily::WasapiShared;
        if (typeName.containsIgnoreCase("DirectSound"))  return DriverFamily::DirectSound;
        return DriverFamily::Other;
    }

    inline AsioDriverClass classifyAsioDriver(const juce::String& driverName)
    {
        const auto name = driverName.toLowerCase();

        if (containsAny(name, { "voicemeeter", "rearoute", "jackrouter", "virtual" }))
            return AsioDriverClass::Virtual;

        if (containsAny(name, { "asio4all", "flexasio", "asio2wasapi", "asio2ks", "generic low latency",
                                "built-in asio", "realtek asio", "fl studio asio", "magix low latency" }))
            return AsioDriverClass::Generic;

        return AsioDriverClass::Native;
    }

    struct EndpointTraits
    {
        juce::String label;   // "Line", "Speakers", "Microphone Array"...
        juce::String device;  // "Steinberg UR22C" (Windows' "2- " duplicate prefix removed)
        bool builtIn = false;
        bool bluetooth = false;
        bool webcam = false;
        bool display = false;
        bool virtualDevice = false;
        bool systemAlias = false; // DirectSound "Primary Sound (Capture) Driver" = whatever Windows' default is
        bool headphones = false;
        bool speakers = false;
        bool microphone = false;
    };

    inline EndpointTraits classifyEndpoint(const juce::String& endpointName)
    {
        EndpointTraits traits;
        const auto name = endpointName.trim();
        const int open = name.indexOf(" (");

        if (open > 0 && name.endsWithChar(')'))
        {
            traits.label = name.substring(0, open).trim();
            traits.device = name.substring(open + 2, name.length() - 1).trim();
        }
        else
        {
            traits.label = name;
            traits.device = name;
        }

        int digits = 0;
        while (digits < traits.device.length() && juce::CharacterFunctions::isDigit(traits.device[digits]))
            ++digits;
        if (digits > 0 && traits.device.substring(digits).startsWith("- "))
            traits.device = traits.device.substring(digits + 2).trim();

        const auto full = name.toLowerCase();
        const auto label = traits.label.toLowerCase();
        const auto device = traits.device.toLowerCase();

        traits.virtualDevice = containsAny(full, { "virtual", "vb-audio", "voicemeeter", "cable input", "cable output",
                                                   "steam streaming", "nvidia broadcast", "stereo mix",
                                                   "mezcla est\xc3\xa9reo", "wave out mix", "what u hear" });
        traits.systemAlias = full.startsWith("primary sound");
        traits.bluetooth = containsAny(full, { "bluetooth", "hands-free", "handsfree", "manos libres",
                                               "airpods", "a2dp", " ag audio" });
        traits.webcam = containsAny(full, { "webcam", "camera", "c\xc3\xa1mara", "camara", "brio", "c920", "c922",
                                            "c930", "lifecam", "kiyo", "facecam" });
        traits.display = containsAny(device, { "hdmi", "displayport", "display audio", "nvidia high definition audio",
                                               "amd high definition audio", "nvidia output" });
        traits.builtIn = ! traits.display && ! traits.bluetooth
            && (containsAny(device, { "realtek", "high definition audio", "smart sound", "conexant", "cirrus logic",
                                      "synaptics", "idt high", "sigmatel", "via hd audio", "audio coprocessor",
                                      "amd audio" })
                || containsAny(label, { "microphone array", "matriz de micr", "internal", "interno", "integrad",
                                        "built-in", "laptop" }));

        traits.headphones = containsAny(label, { "headphone", "auricular", "aud\xc3\xad" "fono", "audifono", "casco",
                                                 "headset", "earphone", "earbud" });
        traits.speakers = containsAny(label, { "speaker", "altavo", "parlante", "bocina" });
        traits.microphone = label.startsWith("mic")
            || containsAny(label, { "microphone", "micr\xc3\xb3" "fono", "microfono", "matriz de micr" });

        return traits;
    }

    inline bool isSameDevice(const EndpointTraits& a, const EndpointTraits& b)
    {
        return a.device.isNotEmpty() && a.device.equalsIgnoreCase(b.device);
    }

    // Brand-like tokens of an ASIO driver name ("Yamaha Steinberg USB ASIO" -> yamaha, steinberg).
    inline juce::StringArray getBrandTokens(const juce::String& asioDriverName)
    {
        static const juce::StringArray genericWords {
            "asio", "driver", "drivers", "usb", "audio", "device", "devices", "low", "latency", "generic",
            "x64", "x86", "64bit", "32bit", "bit", "the", "for", "and", "series", "interface", "wdm", "pro",
            "sound", "card", "multi", "client", "version", "control", "panel", "thunderbolt", "studio"
        };

        juce::StringArray tokens;
        tokens.addTokens(asioDriverName.toLowerCase(), " -_()[]/.,:;", "");
        tokens.trim();
        tokens.removeEmptyStrings();

        juce::StringArray result;
        for (const auto& token : tokens)
            if (token.length() >= 3 && ! token.containsOnly("0123456789v") && ! genericWords.contains(token))
                result.addIfNotAlreadyThere(token);

        return result;
    }

    // How many brand tokens of the ASIO driver show up in the best-matching Windows endpoint.
    // Non-zero means the interface behind the driver is plugged in. Never loads the driver.
    inline int countAsioHardwareMatches(const juce::String& asioDriverName, const juce::StringArray& windowsEndpointNames)
    {
        const auto tokens = getBrandTokens(asioDriverName);
        int bestMatches = 0;

        for (const auto& endpoint : windowsEndpointNames)
        {
            const auto device = classifyEndpoint(endpoint).device.toLowerCase();
            int matches = 0;

            for (const auto& token : tokens)
                if (device.contains(token))
                    ++matches;

            bestMatches = juce::jmax(bestMatches, matches);
        }

        return bestMatches;
    }

    // -------------------------------------------------------------------------
    // Inventory
    // -------------------------------------------------------------------------

    struct DriverTypeEntry
    {
        juce::String typeName;
        juce::StringArray inputNames;
        juce::StringArray outputNames;
        int defaultInputIndex = 0;
        int defaultOutputIndex = 0;
    };

    struct ActiveDevice
    {
        bool isOpen = false;
        juce::String typeName;
        juce::String inputName;
        juce::String outputName;
        double sampleRate = 0.0;
        int bufferSize = 0;
        int numInputChannels = 0;
        int numOutputChannels = 0;
        juce::Array<double> availableSampleRates;
        juce::Array<int> availableBufferSizes;

        juce::String getRouteKey() const { return typeName + "|" + inputName + "|" + outputName; }
    };

    struct Inventory
    {
        std::vector<DriverTypeEntry> types;
        ActiveDevice active;
    };

    // -------------------------------------------------------------------------
    // Ranking
    // -------------------------------------------------------------------------

    struct Options
    {
        bool allowExclusive = true;                // decision 3 (2026-10-09): allowed in the Studio profile
        double preferredSampleRate = 48000.0;      // the rate golden metrics validate
        int wasapiLowLatencyBufferSamples = 128;   // until the F2 calibration picks the buffer
    };

    struct Candidate
    {
        juce::String typeName;
        juce::String inputName;
        juce::String outputName;
        double sampleRate = 0.0;  // requested; JUCE falls back to the closest supported rate
        int bufferSize = 0;       // requested; 0 = driver preferred / device default
        Tier tier = Tier::Other;
        int pairScore = 0;
        bool isActive = false;
        juce::StringArray reasons;

        juce::String getKey() const { return typeName + "|" + inputName + "|" + outputName; }
    };

    inline juce::String describeTier(Tier tier)
    {
        switch (tier)
        {
            case Tier::AsioNativeVerified:   return "Native ASIO driver for connected hardware: lowest latency.";
            case Tier::WasapiExclusive:      return "Exclusive mode: low latency, but other apps can't play through this device while NOVA runs.";
            case Tier::WasapiLowLatency:     return "Low Latency mode: small buffers while other apps keep sharing the device.";
            case Tier::AsioNativeUnverified: return "Native ASIO driver, but its hardware wasn't found among the connected devices.";
            case Tier::AsioGeneric:          return "Generic ASIO wrapper: usually slower than the vendor driver and may block other apps.";
            case Tier::WasapiShared:         return "Shared mode goes through the Windows mixer (~10 ms extra per direction).";
            case Tier::DirectSound:          return "DirectSound is a legacy driver with high latency.";
            case Tier::Other:                break;
        }

        return {};
    }

    inline juce::String getFriendlyTypeName(const juce::String& typeName)
    {
        switch (classifyDriverType(typeName))
        {
            case DriverFamily::Asio:             return "ASIO";
            case DriverFamily::WasapiExclusive:  return "Windows Audio (Exclusive)";
            case DriverFamily::WasapiLowLatency: return "Windows Audio (Low Latency)";
            case DriverFamily::WasapiShared:     return "Windows Audio (Shared)";
            case DriverFamily::DirectSound:      return "DirectSound";
            case DriverFamily::Other:            break;
        }

        return typeName;
    }

    inline juce::String describeCandidate(const Candidate& candidate)
    {
        const juce::String separator(juce::CharPointer_UTF8("  \xc2\xb7  "));
        auto text = getFriendlyTypeName(candidate.typeName);

        if (candidate.inputName == candidate.outputName)
            text << separator << candidate.outputName;
        else
            text << separator << "In: " << candidate.inputName << separator << "Out: " << candidate.outputName;

        if (candidate.sampleRate > 0.0)
            text << separator << juce::String(candidate.sampleRate / 1000.0, std::fmod(candidate.sampleRate, 1000.0) == 0.0 ? 0 : 1) << " kHz";

        if (candidate.bufferSize > 0)
            text << separator << candidate.bufferSize << " samples";

        return text;
    }

    namespace Detail
    {
        inline int scoreInput(const EndpointTraits& input)
        {
            if (input.webcam)  return -40;
            if (input.builtIn) return 0;
            return input.microphone ? 20 : 40; // external line / instrument inputs first
        }

        inline int scoreOutput(const EndpointTraits& output)
        {
            int score = output.bluetooth ? -300 : output.display ? -60 : output.builtIn ? 0 : 30;
            if (output.headphones)
                score += 10;
            return score;
        }

        inline Tier tierForFamily(DriverFamily family)
        {
            switch (family)
            {
                case DriverFamily::WasapiExclusive:  return Tier::WasapiExclusive;
                case DriverFamily::WasapiLowLatency: return Tier::WasapiLowLatency;
                case DriverFamily::WasapiShared:     return Tier::WasapiShared;
                case DriverFamily::DirectSound:      return Tier::DirectSound;
                case DriverFamily::Asio:
                case DriverFamily::Other:            break;
            }

            return Tier::Other;
        }

        inline bool isActiveRoute(const ActiveDevice& active, const juce::String& typeName,
                                  const juce::String& inputName, const juce::String& outputName)
        {
            return active.isOpen && active.typeName == typeName
                && active.inputName == inputName && active.outputName == outputName;
        }

        inline void addAsioCandidates(std::vector<Candidate>& result, const DriverTypeEntry& type,
                                      const ActiveDevice& active, const juce::StringArray& windowsEndpoints,
                                      const Options& options)
        {
            for (const auto& driverName : type.outputNames)
            {
                const auto driverClass = classifyAsioDriver(driverName);
                if (driverClass == AsioDriverClass::Virtual)
                    continue;

                Candidate candidate;
                candidate.typeName = type.typeName;
                candidate.inputName = driverName;
                candidate.outputName = driverName;
                candidate.sampleRate = options.preferredSampleRate;
                candidate.isActive = isActiveRoute(active, type.typeName, driverName, driverName);

                // "Ableton Push" beats "Ableton Move" when a Push is plugged in.
                const int hardwareMatches = countAsioHardwareMatches(driverName, windowsEndpoints);
                candidate.pairScore = 100 + hardwareMatches * 5 + (candidate.isActive ? 10 : 0);

                if (driverClass == AsioDriverClass::Generic)
                    candidate.tier = Tier::AsioGeneric;
                else if (candidate.isActive || hardwareMatches > 0)
                    candidate.tier = Tier::AsioNativeVerified;
                else
                    candidate.tier = Tier::AsioNativeUnverified;

                candidate.reasons.add(describeTier(candidate.tier));
                result.push_back(std::move(candidate));
            }
        }

        inline std::optional<Candidate> bestEndpointPair(const DriverTypeEntry& type, DriverFamily family,
                                                         const ActiveDevice& active, const Options& options)
        {
            std::optional<Candidate> best;

            for (int i = 0; i < type.inputNames.size(); ++i)
            {
                const auto input = classifyEndpoint(type.inputNames[i]);

                // A Bluetooth headset mic forces the headset into call quality (HFP).
                if (input.virtualDevice || input.bluetooth || input.systemAlias)
                    continue;

                for (int o = 0; o < type.outputNames.size(); ++o)
                {
                    const auto output = classifyEndpoint(type.outputNames[o]);
                    if (output.virtualDevice || output.systemAlias)
                        continue;

                    const bool sameDevice = isSameDevice(input, output);
                    const bool isActive = isActiveRoute(active, type.typeName, type.inputNames[i], type.outputNames[o]);

                    int score = scoreInput(input) + scoreOutput(output) + (sameDevice ? 100 : 0);
                    score += (i == type.defaultInputIndex ? 3 : 0) + (o == type.defaultOutputIndex ? 3 : 0);
                    score += isActive ? 10 : 0;

                    if (best.has_value() && score <= best->pairScore)
                        continue;

                    Candidate candidate;
                    candidate.typeName = type.typeName;
                    candidate.inputName = type.inputNames[i];
                    candidate.outputName = type.outputNames[o];
                    candidate.sampleRate = options.preferredSampleRate;
                    candidate.bufferSize = (family == DriverFamily::WasapiExclusive || family == DriverFamily::WasapiLowLatency)
                        ? options.wasapiLowLatencyBufferSamples : 0;
                    candidate.tier = tierForFamily(family);
                    candidate.pairScore = score;
                    candidate.isActive = isActive;

                    candidate.reasons.add(describeTier(candidate.tier));
                    candidate.reasons.add(sameDevice
                        ? "Input and output on the same device (single clock, no drift)."
                        : "Input and output on different devices: their clocks can drift and click now and then.");
                    if (output.bluetooth)
                        candidate.reasons.add("Bluetooth output adds ~150-300 ms. Use wired headphones or an interface output.");
                    if (input.builtIn || input.webcam)
                        candidate.reasons.add("Input is a built-in microphone: plug the guitar into an audio interface for the best sound.");

                    best = std::move(candidate);
                }
            }

            return best;
        }
    }

    inline std::vector<Candidate> rankCandidates(const Inventory& inventory, const Options& options = {})
    {
        juce::StringArray windowsEndpoints;
        for (const auto& type : inventory.types)
        {
            if (classifyDriverType(type.typeName) == DriverFamily::Asio)
                continue;

            windowsEndpoints.addArray(type.inputNames);
            windowsEndpoints.addArray(type.outputNames);
        }
        windowsEndpoints.removeDuplicates(true);

        std::vector<Candidate> result;

        for (const auto& type : inventory.types)
        {
            const auto family = classifyDriverType(type.typeName);

            if (family == DriverFamily::Asio)
            {
                Detail::addAsioCandidates(result, type, inventory.active, windowsEndpoints, options);
                continue;
            }

            if (family == DriverFamily::Other || (family == DriverFamily::WasapiExclusive && ! options.allowExclusive))
                continue;

            if (auto candidate = Detail::bestEndpointPair(type, family, inventory.active, options))
                result.push_back(std::move(*candidate));
        }

        std::stable_sort(result.begin(), result.end(), [] (const Candidate& a, const Candidate& b)
        {
            if (a.tier != b.tier)           return a.tier < b.tier;
            if (a.pairScore != b.pairScore) return a.pairScore > b.pairScore;
            return a.getKey() < b.getKey();
        });

        return result;
    }

    enum class RecommendationKind { NoCandidate, AlreadyOptimal, ChangeSampleRate, SwitchDevice };

    struct Recommendation
    {
        RecommendationKind kind = RecommendationKind::NoCandidate;
        Candidate candidate;
    };

    // excludedKeys: candidates that already failed to open or deliver audio this session.
    inline Recommendation recommend(const Inventory& inventory, const Options& options = {},
                                    const juce::StringArray& excludedKeys = {})
    {
        for (const auto& candidate : rankCandidates(inventory, options))
        {
            if (! candidate.isActive && excludedKeys.contains(candidate.getKey()))
                continue;

            Recommendation recommendation;
            recommendation.candidate = candidate;

            if (! candidate.isActive)
            {
                recommendation.kind = RecommendationKind::SwitchDevice;
                return recommendation;
            }

            const auto& active = inventory.active;
            const bool canUsePreferredRate = active.sampleRate != options.preferredSampleRate
                && active.availableSampleRates.contains(options.preferredSampleRate);

            if (canUsePreferredRate)
            {
                recommendation.kind = RecommendationKind::ChangeSampleRate;
                recommendation.candidate.bufferSize = active.bufferSize;
                recommendation.candidate.reasons.insert(0, "Same device at " + juce::String(options.preferredSampleRate / 1000.0, 0)
                    + " kHz, the rate NOVA's sound is tuned at.");
            }
            else
            {
                recommendation.kind = RecommendationKind::AlreadyOptimal;
            }

            return recommendation;
        }

        return {};
    }

    // -------------------------------------------------------------------------
    // Input mute policy (the Standalone starts with the input muted)
    // -------------------------------------------------------------------------

    enum class MuteAdvice { NoInput, Unmute, KeepMuted };

    struct InputMuteDecision
    {
        MuteAdvice advice = MuteAdvice::NoInput;
        bool feedbackRisk = false; // a microphone that can hear the output: re-mute when the route changes to this
        juce::String reason;
    };

    // Unmutes only with evidence that it is safe (an interface input or headphones);
    // when in doubt the input stays muted and the reason is shown to the user.
    inline InputMuteDecision adviseInputMute(const ActiveDevice& route)
    {
        if (! route.isOpen || route.inputName.isEmpty() || route.numInputChannels <= 0)
            return { MuteAdvice::NoInput, false, "No audio input is active." };

        if (classifyDriverType(route.typeName) == DriverFamily::Asio)
        {
            switch (classifyAsioDriver(route.inputName))
            {
                case AsioDriverClass::Native:
                    return { MuteAdvice::Unmute, false, "Audio interface input (native ASIO driver)." };
                case AsioDriverClass::Virtual:
                    return { MuteAdvice::KeepMuted, true, "Virtual ASIO driver: its input can loop NOVA's output back." };
                case AsioDriverClass::Generic:
                    return { MuteAdvice::KeepMuted, false, "Generic ASIO driver: NOVA can't tell whether the input is a microphone near your speakers." };
            }
        }

        const auto input = classifyEndpoint(route.inputName);
        const auto output = classifyEndpoint(route.outputName);

        if (input.virtualDevice)
            return { MuteAdvice::KeepMuted, true, "Virtual/loopback input: it can feed NOVA's output back into itself." };

        if (input.bluetooth)
            return { MuteAdvice::KeepMuted, true, "Bluetooth headset microphone: it drops the headset to call quality." };

        if (input.systemAlias)
            return { MuteAdvice::KeepMuted, false, "The input follows the Windows default device, which may be a microphone." };

        if (output.headphones && ! output.speakers)
            return { MuteAdvice::Unmute, false, "Output is headphones: no feedback risk." };

        if (isSameDevice(input, output) && ! input.builtIn && ! input.webcam)
            return { MuteAdvice::Unmute, false, "Input and output on the same external device (audio interface)." };

        if (input.builtIn || input.webcam)
            return { MuteAdvice::KeepMuted, true, "Built-in microphone with speakers can cause loud feedback. Use headphones or an audio interface." };

        if (! input.microphone)
            return { MuteAdvice::Unmute, false, "External line/instrument input." };

        return { MuteAdvice::KeepMuted, false, "NOVA can't confirm this is an instrument input. Unmute it if you use headphones." };
    }

    // -------------------------------------------------------------------------
    // Buffer calibration (phase 2): find the smallest buffer that runs cleanly
    // -------------------------------------------------------------------------

    struct CalibrationOptions
    {
        int firstBufferSamples = 32;        // Studio profile; Play-along would start at 64 (phase 4)
        int lastBufferSamples = 512;        // ~10.7 ms per direction at 48 kHz: past this, playing feels late anyway
        double settleSeconds = 0.5;         // after each buffer change, before measuring
        double measureSeconds = 4.0;
        double maxPeakLoad = 0.70;          // worst block's processing time / block duration
        double maxCallbackGapRatio = 2.0;   // longest gap between callbacks / block duration
        double minDeliveredRatio = 0.90;    // callbacks received / callbacks expected
    };

    // Sizes to try, smallest first: the smallest available size at or above each step of a
    // 1x / 1.5x ladder. Drivers that list every multiple of 32 would otherwise take minutes.
    inline juce::Array<int> buildCalibrationLadder(const juce::Array<int>& availableSizes,
                                                   const CalibrationOptions& options = {})
    {
        static const int steps[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048 };

        juce::Array<int> sorted(availableSizes);
        sorted.sort();

        juce::Array<int> ladder;
        for (int step : steps)
        {
            if (step < options.firstBufferSamples)
                continue;

            for (int size : sorted)
            {
                if (size >= step)
                {
                    if (size <= options.lastBufferSamples)
                        ladder.addIfNotAlreadyThere(size);
                    break;
                }
            }
        }

        // A device whose smallest buffer is already above the limit still gets one measurement.
        if (ladder.isEmpty() && ! sorted.isEmpty())
            ladder.add(sorted.getFirst());

        return ladder;
    }

    struct StabilityMeasurement
    {
        int bufferSize = 0;
        double sampleRate = 0.0;
        double windowSeconds = 0.0;
        int xruns = 0;                  // driver-reported dropouts + callbacks that overran the block
        double peakProcessMs = 0.0;     // worst block processing time
        double maxCallbackGapMs = 0.0;  // longest time between two consecutive callbacks
        juce::int64 blocksDelivered = 0;
    };

    struct StabilityVerdict
    {
        bool stable = false;
        double peakLoad = 0.0;
        double gapRatio = 0.0;
        double deliveredRatio = 0.0;
        juce::String summary;           // the failure reason, or the measured margins when stable
    };

    inline StabilityVerdict evaluateStability(const StabilityMeasurement& measurement,
                                              const CalibrationOptions& options = {})
    {
        StabilityVerdict verdict;

        if (measurement.bufferSize <= 0 || measurement.sampleRate <= 0.0 || measurement.windowSeconds <= 0.0)
        {
            verdict.summary = "no measurement";
            return verdict;
        }

        const double blockMs = 1000.0 * measurement.bufferSize / measurement.sampleRate;
        const double expectedBlocks = measurement.windowSeconds * measurement.sampleRate / measurement.bufferSize;
        verdict.peakLoad = measurement.peakProcessMs / blockMs;
        verdict.gapRatio = measurement.maxCallbackGapMs / blockMs;
        verdict.deliveredRatio = (double) measurement.blocksDelivered / expectedBlocks;

        const auto percent = [] (double ratio) { return juce::String(juce::roundToInt(ratio * 100.0)) + "%"; };

        if (measurement.xruns > 0)
            verdict.summary = juce::String(measurement.xruns) + " dropout(s)";
        else if (verdict.deliveredRatio < options.minDeliveredRatio)
            verdict.summary = "only " + percent(verdict.deliveredRatio) + " of the expected callbacks arrived";
        else if (verdict.peakLoad >= options.maxPeakLoad)
            verdict.summary = "peak load " + percent(verdict.peakLoad) + " of the block";
        else if (verdict.gapRatio >= options.maxCallbackGapRatio)
            verdict.summary = "a gap of " + juce::String(verdict.gapRatio, 1) + "x the block between callbacks";
        else
        {
            verdict.stable = true;
            verdict.summary = "peak load " + percent(verdict.peakLoad) + ", longest gap " + juce::String(verdict.gapRatio, 1) + "x";
        }

        return verdict;
    }
}
