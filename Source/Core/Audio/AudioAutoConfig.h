#pragma once

#include <JuceHeader.h>

namespace Nova
{
namespace Audio
{
    // =========================================================================
    // Picks the best audio configuration the current machine can actually run.
    //
    // This is the deterministic half of the answer: back-ends and devices expose
    // what they support, so the driver type, sample rate and the range of usable
    // buffer sizes can all be decided by inspection alone. What cannot be decided
    // by inspection is the smallest buffer that survives NOVA's DSP load without
    // dropouts on this machine — that needs measurement, and is what
    // `hardwareFloorBufferSize` is reported for rather than silently applied.
    //
    // Every recommendation carries the reason it was made, so the UI can tell the
    // user which constraint sets their ceiling instead of just showing a number.
    // =========================================================================
    struct AudioAutoConfig
    {
        struct Recommendation
        {
            bool valid = false;

            juce::String deviceTypeName;
            juce::String inputDeviceName;
            juce::String outputDeviceName;
            double sampleRate = 0.0;
            int bufferSize = 0;

            // Smallest buffer this device offers at all. The recommended buffer
            // never goes below it, and it is what the ceiling message quotes.
            int hardwareFloorBufferSize = 0;

            // Kept so a different use case can be costed without rescanning the
            // hardware; an ASIO rescan can take long enough to stall the UI.
            juce::Array<int> availableBufferSizes;

            juce::String driverReason;
            juce::String sampleRateReason;
            juce::String bufferReason;
            juce::String ceilingReason;

            // True when the chosen back-end is the best tier this platform offers,
            // i.e. no driver change could lower the floor any further.
            bool onBestAvailableDriver = false;

            double latencyMs() const noexcept
            {
                return sampleRate > 0.0 ? (bufferSize / sampleRate) * 1000.0 : 0.0;
            }

            double floorLatencyMs() const noexcept
            {
                return sampleRate > 0.0 ? (hardwareFloorBufferSize / sampleRate) * 1000.0 : 0.0;
            }
        };

        // Buffer targets for the three use cases offered in the wizard.
        static int bufferTargetForUseCase(int useCase) noexcept
        {
            static const int targets[] = { 64, 128, 256 };
            return juce::isPositiveAndBelow(useCase, 3) ? targets[useCase] : 128;
        }

        // ---------------------------------------------------------------------
        // Driver ranking
        // ---------------------------------------------------------------------

        // Higher is better. Ordered by how directly the back-end reaches the
        // hardware: an exclusive path beats a mixed one, and a vendor driver
        // beats the OS mixer. Names are AudioIODeviceType::getTypeName().
        static int rankDeviceType(const juce::String& typeName) noexcept
        {
            if (typeName == "ASIO")                              return 100;
            if (typeName == "CoreAudio")                         return 90;
            if (typeName == "Windows Audio (Exclusive Mode)")    return 80;
            if (typeName == "Windows Audio (Low Latency Mode)")  return 70;
            if (typeName == "Windows Audio")                     return 40;
            if (typeName == "DirectSound")                       return 10;
            return 20;
        }

        static juce::String describeDeviceType(const juce::String& typeName)
        {
            if (typeName == "ASIO")
                return "Vendor ASIO driver: the audio interface is addressed directly, "
                       "which gives the lowest latency available on Windows.";
            if (typeName == "CoreAudio")
                return "CoreAudio is macOS's native low-latency audio path. "
                       "There is no lower-level driver to switch to.";
            if (typeName == "Windows Audio (Exclusive Mode)")
                return "WASAPI exclusive: NOVA takes sole ownership of the device and "
                       "bypasses the Windows mixer.";
            if (typeName == "Windows Audio (Low Latency Mode)")
                return "WASAPI low-latency shared mode: smaller buffers than normal shared "
                       "mode while other apps keep using the device.";
            if (typeName == "Windows Audio")
                return "WASAPI shared mode: audio is mixed by Windows, which adds latency.";
            if (typeName == "DirectSound")
                return "DirectSound is a legacy path and the slowest option available.";
            return typeName;
        }

        // Re-derives the buffer size for a use case from the sizes already probed.
        // Touches no hardware, so it is safe to call on every UI interaction.
        static void chooseBufferForUseCase(Recommendation& rec, int useCase)
        {
            const int target = bufferTargetForUseCase(useCase);
            const auto& sizes = rec.availableBufferSizes;

            if (sizes.isEmpty())
                return;

            int floor = sizes[0];
            for (auto size : sizes)
                floor = juce::jmin(floor, size);
            rec.hardwareFloorBufferSize = floor;

            // Smallest offered size that still meets the use case's target.
            int chosen = 0;
            for (auto size : sizes)
                if (size >= target && (chosen == 0 || size < chosen))
                    chosen = size;

            if (chosen == 0)
            {
                // Everything on offer is below target: take the largest available.
                for (auto size : sizes)
                    chosen = juce::jmax(chosen, size);
                rec.bufferReason = "This device caps out at " + juce::String(chosen)
                    + " samples, below the " + juce::String(target) + " this profile asks for.";
            }
            else if (chosen == floor)
            {
                rec.bufferReason = juce::String(chosen) + " samples is both what this profile "
                    "asks for and the smallest this device offers.";
            }
            else
            {
                rec.bufferReason = juce::String(chosen) + " samples matches this profile. "
                    "The device can go as low as " + juce::String(floor)
                    + ", which trades safety margin for latency.";
            }

            rec.bufferSize = chosen;
        }

        // ---------------------------------------------------------------------
        // Computing the recommendation
        // ---------------------------------------------------------------------

        static Recommendation compute(juce::AudioDeviceManager& dm, int useCase)
        {
            Recommendation rec;

            juce::AudioIODeviceType* bestType = nullptr;
            int bestScore = -1;
            bool sawAsio = false;

            for (auto* type : dm.getAvailableDeviceTypes())
            {
                if (type == nullptr)
                    continue;

                const auto name = type->getTypeName();
                if (name == "ASIO")
                    sawAsio = true;

                // A back-end with no devices is not a candidate no matter how it ranks.
                type->scanForDevices();
                if (type->getDeviceNames(false).isEmpty())
                    continue;

                const int score = rankDeviceType(name);
                if (score > bestScore)
                {
                    bestScore = score;
                    bestType = type;
                }
            }

            if (bestType == nullptr)
                return rec;

            rec.deviceTypeName = bestType->getTypeName();
            rec.driverReason = describeDeviceType(rec.deviceTypeName);

            const auto outputNames = bestType->getDeviceNames(false);
            const auto inputNames = bestType->getDeviceNames(true);

            rec.outputDeviceName = pickDefault(*bestType, outputNames, false);
            rec.inputDeviceName = pickDefault(*bestType, inputNames, true);

            // ASIO drivers expose a single device serving both directions; pairing a
            // different input would fail to open.
            if (rec.deviceTypeName == "ASIO" && rec.outputDeviceName.isNotEmpty())
                rec.inputDeviceName = inputNames.contains(rec.outputDeviceName)
                    ? rec.outputDeviceName
                    : rec.inputDeviceName;

            std::unique_ptr<juce::AudioIODevice> probe(
                bestType->createDevice(rec.outputDeviceName, rec.inputDeviceName));

            if (probe == nullptr)
                return rec;

            rec.sampleRate = pickSampleRate(probe->getAvailableSampleRates(), rec.sampleRateReason);
            rec.availableBufferSizes = probe->getAvailableBufferSizes();
            if (rec.availableBufferSizes.isEmpty())
                rec.availableBufferSizes.add(juce::jmax(1, probe->getDefaultBufferSize()));
            chooseBufferForUseCase(rec, useCase);

            rec.onBestAvailableDriver = isBestPossibleTier(rec.deviceTypeName);
            rec.ceilingReason = buildCeilingReason(rec, sawAsio);
            rec.valid = rec.sampleRate > 0.0 && rec.bufferSize > 0;
            return rec;
        }

        // Applies the recommendation. Returns an empty string on success, or the
        // device manager's error message.
        static juce::String apply(juce::AudioDeviceManager& dm, const Recommendation& rec)
        {
            if (!rec.valid)
                return "No usable audio configuration was found.";

            if (dm.getCurrentAudioDeviceType() != rec.deviceTypeName)
                dm.setCurrentAudioDeviceType(rec.deviceTypeName, true);

            juce::AudioDeviceManager::AudioDeviceSetup setup;
            dm.getAudioDeviceSetup(setup);

            setup.outputDeviceName = rec.outputDeviceName;
            setup.inputDeviceName = rec.inputDeviceName;
            setup.sampleRate = rec.sampleRate;
            setup.bufferSize = rec.bufferSize;
            setup.useDefaultInputChannels = true;
            setup.useDefaultOutputChannels = true;

            return dm.setAudioDeviceSetup(setup, true);
        }

        // True when the running device already matches the recommendation.
        static bool matchesCurrentDevice(juce::AudioDeviceManager& dm, const Recommendation& rec)
        {
            if (!rec.valid)
                return false;

            auto* device = dm.getCurrentAudioDevice();
            if (device == nullptr)
                return false;

            return device->getTypeName() == rec.deviceTypeName
                && std::abs(device->getCurrentSampleRate() - rec.sampleRate) < 1.0
                && device->getCurrentBufferSizeSamples() == rec.bufferSize;
        }

    private:
        static juce::String pickDefault(juce::AudioIODeviceType& type,
            const juce::StringArray& names,
            bool forInput)
        {
            if (names.isEmpty())
                return {};

            const int index = type.getDefaultDeviceIndex(forInput);
            return juce::isPositiveAndBelow(index, names.size()) ? names[index] : names[0];
        }

        // 48 kHz is the target. Higher rates cost proportionally more CPU for no
        // benefit here: the saturation pedals already oversample internally, which
        // is where aliasing actually matters.
        static double pickSampleRate(const juce::Array<double>& available, juce::String& reason)
        {
            if (available.isEmpty())
            {
                reason = "The device did not report any sample rates.";
                return 48000.0;
            }

            if (available.contains(48000.0))
            {
                reason = "48 kHz is the best balance: the saturation pedals oversample "
                         "internally, so a higher device rate costs CPU without adding quality.";
                return 48000.0;
            }

            if (available.contains(44100.0))
            {
                reason = "48 kHz is not offered by this device, so 44.1 kHz is used.";
                return 44100.0;
            }

            double best = available[0];
            for (auto rate : available)
                if (rate >= 44100.0 && (best < 44100.0 || rate < best))
                    best = rate;

            reason = "This device only offers non-standard rates; "
                     + juce::String(best, 0) + " Hz is the closest usable one.";
            return best;
        }

        // CoreAudio on macOS and ASIO on Windows are the lowest-level paths their
        // platform offers; anything else means a better driver could still exist.
        static bool isBestPossibleTier(const juce::String& typeName) noexcept
        {
            return typeName == "ASIO" || typeName == "CoreAudio";
        }

        static juce::String buildCeilingReason(const Recommendation& rec, bool asioTypePresent)
        {
            const auto floorMs = juce::String(rec.floorLatencyMs(), 1) + " ms";

            if (rec.deviceTypeName == "CoreAudio")
                return "This is as low as macOS goes. Your floor is " + floorMs
                     + ", set by the audio device itself.";

            if (rec.deviceTypeName == "ASIO")
                return "This is as low as Windows goes. Your floor is " + floorMs
                     + ", set by your interface's ASIO driver.";

            if (!asioTypePresent)
                return "Your floor is " + floorMs + " because no ASIO driver was found. "
                       "An audio interface that ships its own ASIO driver would lower it further.";

            return "Your floor is " + floorMs + " on this back-end. An ASIO driver is present "
                   "on this system but exposes no device — check that your interface is connected.";
        }
    };
}
}
