#pragma once

#include <JuceHeader.h>
#include <functional>
#include <vector>

#include "AudioAutoConfig.h"

namespace Nova
{
namespace Audio
{
    // =========================================================================
    // Finds the smallest buffer this machine actually sustains, by running it.
    //
    // AudioAutoConfig can rank back-ends and read back what a device claims to
    // support, but nothing in that is a promise that a given buffer survives
    // NOVA's DSP load here: that depends on the driver, the power plan, what
    // else is running and how heavy the current pedal chain is. So this opens
    // the device for real at each candidate size and watches the two things
    // that decide whether audio breaks up — the driver's under/overrun counter
    // and the proportion of each callback period actually spent processing.
    //
    // Candidates are tried smallest first and the sweep stops at the first one
    // that runs clean, which is by definition the floor: everything below it
    // was measured to fail, and larger buffers only add headroom.
    //
    // The device is reopened between candidates, so the user hears clicks while
    // this runs. Callers should say so before starting it.
    // =========================================================================
    class LatencyCalibration final : private juce::Timer
    {
    public:
        struct Trial
        {
            int bufferSize = 0;
            int xruns = 0;
            double peakCpu = 0.0;   // 0..1, proportion of the callback period
            bool passed = false;
            juce::String note;
        };

        struct Thresholds
        {
            // Time for the reopened device to reach steady state before the
            // measurement window starts; the first callbacks after a restart
            // routinely overrun and would poison every trial.
            double settleSeconds = 0.6;
            double measureSeconds = 1.6;

            // A callback that regularly uses more than this proportion of its
            // period has no margin left for the rest of the system.
            double cpuCeiling = 0.70;

            // Bounds the worst-case duration of the sweep.
            int maxCandidates = 6;
        };

        enum class State { Idle, Settling, Measuring, Finished, Failed };

        ~LatencyCalibration() override { stopTimer(); }

        // Called on the message thread whenever progress or results change.
        std::function<void()> onUpdate;

        // ---------------------------------------------------------------------

        // Thresholds cannot be a default argument here: its default member
        // initializers are not usable inside the enclosing class definition.
        void start(juce::AudioDeviceManager& deviceManagerToUse,
            const AudioAutoConfig::Recommendation& recommendation,
            int useCase)
        {
            start(deviceManagerToUse, recommendation, useCase, Thresholds{});
        }

        void start(juce::AudioDeviceManager& deviceManagerToUse,
            const AudioAutoConfig::Recommendation& recommendation,
            int useCase,
            Thresholds thresholdsToUse)
        {
            stopTimer();

            deviceManager = &deviceManagerToUse;
            thresholds = thresholdsToUse;
            targetUseCase = useCase;
            trials.clear();
            chosenBufferSize = 0;
            measuredFloor = 0;
            currentIndex = 0;
            state = State::Idle;

            deviceManager->getAudioDeviceSetup(originalSetup);
            hasOriginalSetup = true;

            candidates = buildCandidates(recommendation, thresholds.maxCandidates);
            if (candidates.empty())
            {
                state = State::Failed;
                failureReason = "This device does not expose any selectable buffer sizes.";
                notify();
                return;
            }

            beginCandidate(0);
            startTimerHz(20);
        }

        // Stops the sweep and puts the device back the way it was found.
        void cancel()
        {
            stopTimer();

            if (state == State::Settling || state == State::Measuring)
                restoreOriginalSetup();

            state = State::Idle;
            notify();
        }

        // ---------------------------------------------------------------------

        State getState() const noexcept { return state; }
        bool isRunning() const noexcept { return state == State::Settling || state == State::Measuring; }
        const std::vector<Trial>& getTrials() const noexcept { return trials; }
        int getChosenBufferSize() const noexcept { return chosenBufferSize; }
        int getMeasuredFloor() const noexcept { return measuredFloor; }
        juce::String getFailureReason() const { return failureReason; }
        bool wasXRunCountingAvailable() const noexcept { return xrunsUsable; }

        int getCurrentCandidate() const noexcept
        {
            return currentIndex < candidates.size() ? candidates[currentIndex] : 0;
        }

        // 0..1 across the whole sweep, so a progress bar advances within a
        // candidate rather than jumping once per trial.
        double getProgress() const noexcept
        {
            if (candidates.empty())
                return 0.0;

            if (state == State::Finished || state == State::Failed)
                return 1.0;

            const double perCandidate = 1.0 / (double) candidates.size();
            const double phaseTotal = thresholds.settleSeconds + thresholds.measureSeconds;
            const double elapsed = juce::jlimit(0.0, phaseTotal, secondsInPhase()
                + (state == State::Measuring ? thresholds.settleSeconds : 0.0));

            return juce::jlimit(0.0, 1.0,
                (double) currentIndex * perCandidate + (elapsed / phaseTotal) * perCandidate);
        }

        // ---------------------------------------------------------------------

        // The floor is what was measured; the use case decides how much margin
        // to keep above it. Asking for a smaller buffer than the floor is not
        // honoured — it was just measured to break up.
        static int applySafetyMargin(int measuredFloorSize,
            int useCase,
            const juce::Array<int>& availableSizes)
        {
            const int target = juce::jmax(measuredFloorSize,
                AudioAutoConfig::bufferTargetForUseCase(useCase));

            int chosen = 0;
            for (auto size : availableSizes)
                if (size >= target && (chosen == 0 || size < chosen))
                    chosen = size;

            return chosen > 0 ? chosen : measuredFloorSize;
        }

        static std::vector<int> buildCandidates(const AudioAutoConfig::Recommendation& rec,
            int maxCandidates)
        {
            juce::Array<int> sorted(rec.availableBufferSizes);
            sorted.sort();

            std::vector<int> out;
            for (auto size : sorted)
            {
                // Sizes outside this range are not worth the sweep time: below 16
                // no driver sustains NOVA's chain, and above 1024 latency is bad
                // enough that stability was never the problem.
                if (size < 16 || size > 1024)
                    continue;

                out.push_back(size);
                if ((int) out.size() >= juce::jmax(1, maxCandidates))
                    break;
            }
            return out;
        }

    private:
        void beginCandidate(size_t index)
        {
            currentIndex = index;
            state = State::Settling;
            phaseStartMs = juce::Time::getMillisecondCounterHiRes();
            peakCpuThisTrial = 0.0;
            xrunBaseline = 0;

            auto setup = originalSetup;
            setup.bufferSize = candidates[index];

            openError = deviceManager->setAudioDeviceSetup(setup, true);
            notify();
        }

        void timerCallback() override
        {
            if (deviceManager == nullptr)
                return;

            if (state == State::Settling)
            {
                if (secondsInPhase() < thresholds.settleSeconds)
                    return;

                // Baseline after settling, so restart overruns are not counted
                // against the candidate.
                const int xruns = deviceManager->getXRunCount();
                xrunsUsable = xruns >= 0;
                xrunBaseline = juce::jmax(0, xruns);

                state = State::Measuring;
                phaseStartMs = juce::Time::getMillisecondCounterHiRes();
                peakCpuThisTrial = 0.0;
                notify();
                return;
            }

            if (state != State::Measuring)
                return;

            peakCpuThisTrial = juce::jmax(peakCpuThisTrial, deviceManager->getCpuUsage());

            if (secondsInPhase() < thresholds.measureSeconds)
            {
                notify();
                return;
            }

            recordTrial();
        }

        void recordTrial()
        {
            Trial trial;
            trial.bufferSize = candidates[currentIndex];
            trial.peakCpu = peakCpuThisTrial;

            auto* device = deviceManager->getCurrentAudioDevice();
            const bool opened = openError.isEmpty() && device != nullptr
                && device->getCurrentBufferSizeSamples() == trial.bufferSize;

            const int xrunsNow = deviceManager->getXRunCount();
            trial.xruns = (xrunsUsable && xrunsNow >= 0)
                ? juce::jmax(0, xrunsNow - xrunBaseline)
                : 0;

            if (!opened)
            {
                trial.passed = false;
                trial.note = openError.isNotEmpty()
                    ? openError
                    : "The device refused this buffer size.";
            }
            else if (trial.xruns > 0)
            {
                trial.passed = false;
                trial.note = juce::String(trial.xruns) + " dropout"
                    + (trial.xruns == 1 ? "" : "s") + " while measuring.";
            }
            else if (trial.peakCpu > thresholds.cpuCeiling)
            {
                trial.passed = false;
                trial.note = "No headroom: peaked at "
                    + juce::String(juce::roundToInt(trial.peakCpu * 100.0)) + "% of the callback.";
            }
            else
            {
                trial.passed = true;
                trial.note = "Clean: no dropouts, peaked at "
                    + juce::String(juce::roundToInt(trial.peakCpu * 100.0)) + "%.";
            }

            trials.push_back(trial);

            if (trial.passed)
            {
                measuredFloor = trial.bufferSize;
                finish();
                return;
            }

            if (currentIndex + 1 < candidates.size())
            {
                beginCandidate(currentIndex + 1);
                return;
            }

            // Nothing ran clean. The largest candidate is still the best of a bad
            // set, and is what gets restored rather than leaving the smallest one
            // applied after it was measured to fail.
            measuredFloor = candidates.back();
            finish();
        }

        void finish()
        {
            stopTimer();

            juce::Array<int> available;
            for (auto size : candidates)
                available.add(size);

            chosenBufferSize = applySafetyMargin(measuredFloor, targetUseCase, available);

            auto setup = originalSetup;
            setup.bufferSize = chosenBufferSize;
            const auto error = deviceManager->setAudioDeviceSetup(setup, true);

            if (error.isNotEmpty())
            {
                state = State::Failed;
                failureReason = error;
            }
            else
            {
                state = State::Finished;
            }

            notify();
        }

        void restoreOriginalSetup()
        {
            if (hasOriginalSetup && deviceManager != nullptr)
                deviceManager->setAudioDeviceSetup(originalSetup, true);
        }

        double secondsInPhase() const noexcept
        {
            return (juce::Time::getMillisecondCounterHiRes() - phaseStartMs) / 1000.0;
        }

        void notify() const
        {
            if (onUpdate)
                onUpdate();
        }

        juce::AudioDeviceManager* deviceManager = nullptr;
        juce::AudioDeviceManager::AudioDeviceSetup originalSetup;
        bool hasOriginalSetup = false;

        Thresholds thresholds;
        std::vector<int> candidates;
        std::vector<Trial> trials;
        size_t currentIndex = 0;
        int targetUseCase = 1;

        State state = State::Idle;
        double phaseStartMs = 0.0;
        double peakCpuThisTrial = 0.0;
        int xrunBaseline = 0;
        bool xrunsUsable = false;
        int measuredFloor = 0;
        int chosenBufferSize = 0;
        juce::String openError;
        juce::String failureReason;
    };
}
}
