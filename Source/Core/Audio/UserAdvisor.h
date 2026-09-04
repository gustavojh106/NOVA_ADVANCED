#pragma once

#include <JuceHeader.h>
#include <vector>

namespace Nova
{
namespace Audio
{
    // =========================================================================
    // Turns the engine's numbers into things a guitarist would actually say.
    //
    // Everything this produces is read by someone who wants to play, not to
    // tune a DAW, so the wording rule is strict: no buffer sizes, no sample
    // rates, no CPU percentages, no "xrun" or "dropout". Say what they will
    // hear ("the sound may break up") and what they can do about it. The
    // numbers stay in the diagnostics report, where they belong.
    //
    // Advice is raised on the *transition* into a condition, never while it
    // merely persists, and a condition must clear for a while before it can
    // be raised again. Otherwise a machine sitting just over the line would
    // nag continuously, which trains people to ignore the messages.
    // =========================================================================
    class UserAdvisor
    {
    public:
        enum class Level
        {
            Info,       // worth knowing, nothing is wrong
            Caution,    // still working, but close to a limit
            Problem     // the user is hearing something bad right now
        };

        struct Advice
        {
            juce::String id;        // stable, so the UI can replace rather than stack
            juce::String title;
            juce::String message;
            Level level = Level::Info;
        };

        struct Snapshot
        {
            bool engineOn = false;

            // Proportion of the audio budget being used, 0-100. Kept out of the
            // messages themselves; it only decides which message applies.
            double cpuPercent = 0.0;

            // Rises when the engine had to recover from broken audio.
            int autoHealCount = 0;

            double latencyMs = 0.0;
            float inputPeak = 0.0f;
            int inputChannels = 0;

            int pedalsInBusiestZone = 0;
            int maxPedalsPerZone = 12;

            // Bumped by the editor whenever the user adds, removes or moves a
            // pedal, so a warning right after a change can name the cause.
            int chainRevision = 0;

            // Mirrors the "Warn about high-latency audio setup" preference.
            bool latencyTipsEnabled = true;
        };

        struct Thresholds
        {
            // Raise well before the audio actually breaks, so the warning is a
            // heads-up rather than an autopsy; clear a good way below it so a
            // machine hovering at the line does not flap between states.
            double cpuRaisePercent = 75.0;
            double cpuClearPercent = 60.0;
            double cpuSustainSeconds = 1.5;

            // A change within this window of a warning is almost certainly what
            // caused it, which lets the message name the pedal instead of the
            // machine.
            double recentChangeSeconds = 4.0;

            // Round-trip delay a player starts to feel as lag rather than as
            // part of the instrument.
            double highLatencyMs = 20.0;

            // Long enough that putting the guitar down between takes does not
            // trigger it.
            double silenceSeconds = 8.0;
            float silenceThreshold = 0.002f;

            // How close to a zone's limit counts as "nearly full".
            int zoneHeadroomWarning = 2;

            // A condition must stay clear this long before it can be raised again.
            double rearmSeconds = 20.0;
        };

        void setThresholds(const Thresholds& t) { thresholds = t; }

        void reset()
        {
            cpuOverSince = -1.0;
            cpuUnderSince = -1.0;
            silentSince = -1.0;
            lastChainRevision = -1;
            lastChangeTime = -1.0;
            lastAutoHealCount = -1;
            raisedAt.clear();
            started = false;
        }

        // Returns only what became true on this update.
        std::vector<Advice> update(const Snapshot& snapshot, double nowSeconds)
        {
            std::vector<Advice> raised;

            if (!started)
            {
                // First call establishes a baseline: the counters carried over
                // from before the UI opened are history, not news.
                lastAutoHealCount = snapshot.autoHealCount;
                lastChainRevision = snapshot.chainRevision;
                started = true;
            }

            if (snapshot.chainRevision != lastChainRevision)
            {
                lastChainRevision = snapshot.chainRevision;
                lastChangeTime = nowSeconds;
            }

            evaluateGlitches(snapshot, nowSeconds, raised);
            evaluateHeadroom(snapshot, nowSeconds, raised);
            evaluateSilence(snapshot, nowSeconds, raised);
            evaluateZoneFilling(snapshot, nowSeconds, raised);
            evaluateLatency(snapshot, nowSeconds, raised);

            return raised;
        }

        // ---------------------------------------------------------------------
        // Ids, exposed so the UI and tests can refer to them without matching text.
        // ---------------------------------------------------------------------
        static constexpr const char* idGlitch = "audio.glitch";
        static constexpr const char* idHeadroom = "audio.headroom";
        static constexpr const char* idHeadroomAfterChange = "audio.headroom.change";
        static constexpr const char* idSilence = "audio.silence";
        static constexpr const char* idZoneFilling = "chain.zoneFilling";
        static constexpr const char* idLatency = "audio.latency";

    private:
        void evaluateGlitches(const Snapshot& s, double now, std::vector<Advice>& out)
        {
            if (s.autoHealCount <= lastAutoHealCount)
                return;

            lastAutoHealCount = s.autoHealCount;

            // Always worth saying: the user just heard this happen.
            if (canRaise(idGlitch, now))
                out.push_back(raise(idGlitch, now, Level::Problem,
                    "Your sound just broke up",
                    "NOVA caught it and recovered on its own. If it keeps happening, "
                    "run Audio Setup and let NOVA find a safer setting."));
        }

        void evaluateHeadroom(const Snapshot& s, double now, std::vector<Advice>& out)
        {
            if (!s.engineOn)
            {
                cpuOverSince = -1.0;
                return;
            }

            if (s.cpuPercent >= thresholds.cpuRaisePercent)
            {
                cpuUnderSince = -1.0;

                if (cpuOverSince < 0.0)
                    cpuOverSince = now;

                if (now - cpuOverSince < thresholds.cpuSustainSeconds)
                    return;

                const bool rightAfterChange = lastChangeTime >= 0.0
                    && (now - lastChangeTime) <= thresholds.recentChangeSeconds;

                // Same condition, but blaming the machine when the user did not
                // just change anything would be unhelpful and slightly wrong.
                if (rightAfterChange)
                {
                    if (canRaise(idHeadroomAfterChange, now))
                        out.push_back(raise(idHeadroomAfterChange, now, Level::Caution,
                            "That one is a lot to ask",
                            "Your setup was already close to its limit, and the sound may "
                            "start to break up. Switching off a pedal you are not using "
                            "will give it room."));
                }
                else if (canRaise(idHeadroom, now))
                {
                    out.push_back(raise(idHeadroom, now, Level::Caution,
                        "NOVA is working hard",
                        "The sound may start to break up. Switching off a pedal you are "
                        "not using will give it room."));
                }

                return;
            }

            if (s.cpuPercent > thresholds.cpuClearPercent)
                return;

            if (cpuUnderSince < 0.0)
                cpuUnderSince = now;

            cpuOverSince = -1.0;
        }

        void evaluateSilence(const Snapshot& s, double now, std::vector<Advice>& out)
        {
            // Nothing to say when the engine is off or there is no input at all;
            // the missing-input case is the device's story, not the player's.
            if (!s.engineOn || s.inputChannels <= 0)
            {
                silentSince = -1.0;
                return;
            }

            if (s.inputPeak > thresholds.silenceThreshold)
            {
                silentSince = -1.0;
                return;
            }

            if (silentSince < 0.0)
            {
                silentSince = now;
                return;
            }

            if (now - silentSince < thresholds.silenceSeconds)
                return;

            if (canRaise(idSilence, now))
                out.push_back(raise(idSilence, now, Level::Caution,
                    "NOVA is not hearing your guitar",
                    "Check that it is plugged in and that the volume on the guitar "
                    "and on your interface is up."));
        }

        void evaluateZoneFilling(const Snapshot& s, double now, std::vector<Advice>& out)
        {
            const int remaining = s.maxPedalsPerZone - s.pedalsInBusiestZone;
            if (remaining < 0 || remaining > thresholds.zoneHeadroomWarning)
                return;

            // Only worth saying right after the user added something.
            if (lastChangeTime < 0.0 || (now - lastChangeTime) > thresholds.recentChangeSeconds)
                return;

            if (!canRaise(idZoneFilling, now))
                return;

            const auto message = remaining <= 0
                ? juce::String("This section is full. Remove a pedal before adding another one.")
                : "You can fit " + juce::String(remaining) + " more pedal"
                    + (remaining == 1 ? "" : "s") + " in this section.";

            out.push_back(raise(idZoneFilling, now,
                remaining <= 0 ? Level::Caution : Level::Info,
                remaining <= 0 ? "This section is full" : "This section is nearly full",
                message));
        }

        void evaluateLatency(const Snapshot& s, double now, std::vector<Advice>& out)
        {
            if (!s.latencyTipsEnabled || s.latencyMs < thresholds.highLatencyMs)
                return;

            if (!canRaise(idLatency, now))
                return;

            out.push_back(raise(idLatency, now, Level::Info,
                "There is a delay before you hear yourself",
                "Run Audio Setup and NOVA will measure your machine and pick the "
                "fastest setting it can hold."));
        }

        // ---------------------------------------------------------------------

        bool canRaise(const juce::String& id, double now) const
        {
            const auto it = raisedAt.find(id);
            return it == raisedAt.end() || (now - it->second) >= thresholds.rearmSeconds;
        }

        Advice raise(const juce::String& id, double now, Level level,
            const juce::String& title, const juce::String& message)
        {
            raisedAt[id] = now;
            return { id, title, message, level };
        }

        Thresholds thresholds;

        std::map<juce::String, double> raisedAt;
        double cpuOverSince = -1.0;
        double cpuUnderSince = -1.0;
        double silentSince = -1.0;
        double lastChangeTime = -1.0;
        int lastChainRevision = -1;
        int lastAutoHealCount = -1;
        bool started = false;
    };
}
}
