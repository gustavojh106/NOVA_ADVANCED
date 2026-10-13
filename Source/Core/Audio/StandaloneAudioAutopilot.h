#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include "AudioAutopilot.h"
#include "../Constants.h"
#include "../PluginProcessor.h"
#include "../SessionLogger.h"

#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

// =============================================================================
// Latency Autopilot (T2-10) - Standalone integration. Message thread only.
//
// - Inventory: driver types and device names the AudioDeviceManager has already
//   scanned. Inactive ASIO drivers are never opened: a broken driver can hang the
//   app, so only the running device is queried for rates and buffer sizes.
// - Apply / Undo (phase 1): nothing changes until the user presses APPLY. The
//   previous setup is kept for UNDO, and when the new device doesn't open or
//   delivers no audio callbacks within 2 s the previous setup comes back on its own.
// - Buffer calibration (phase 2): on request, walks the buffer ladder from the
//   smallest size, measuring dropouts, peak load and callback gaps with the output
//   silenced, and keeps the first size that runs cleanly. UNDO restores the buffer.
// - Input mute: the JUCE Standalone starts with the input muted. When the active
//   route is safe (AudioAutopilot::adviseInputMute) the input is unmuted with a
//   notice; a route with feedback risk is muted again. A manual mute is respected.
//
// The audio thread only feeds counters that already existed (block counter,
// CpuMeter) plus the CpuMeter measurement window and the processor's output
// silence flag; all decisions happen here, on the message thread.
// =============================================================================
namespace Nova
{
class StandaloneAudioAutopilot final : private juce::ChangeListener,
                                       private juce::Value::Listener,
                                       private juce::Timer
{
public:
    enum class Activity { None, Verifying, Applied, Reverted, Undone, Calibrating, Calibrated, CalibrationFailed };

    struct Notice
    {
        juce::String text;
        juce::String actionLabel;
        std::function<void()> action;
    };

    StandaloneAudioAutopilot(juce::StandalonePluginHolder& holderToUse, NOVAAudioProcessor& processorToUse)
        : holder(holderToUse),
          processor(processorToUse),
          engine(processorToUse.getAudioEngine()),
          lastSeenMuted(isInputMuted()),
          createdAtMs(juce::Time::getMillisecondCounterHiRes())
    {
        if (auto* props = holder.settings.get())
        {
            mutePinnedByUser = props->getBoolValue(kMutePinnedKey, false);
            unmuteApprovedRoute = props->getValue(kUnmuteApprovedRouteKey);
        }

        holder.getMuteInputValue().addListener(this);
        holder.deviceManager.addChangeListener(this);
        startTimer(kTimerIntervalMs); // first mute decision once the window has settled
    }

    ~StandaloneAudioAutopilot() override
    {
        stopTimer();

        // Never leave the output silenced or an unproven buffer behind.
        if (activity == Activity::Calibrating)
        {
            processor.setOutputSilenced(false);
            restoreUndoState();
        }

        holder.deviceManager.removeChangeListener(this);
        holder.getMuteInputValue().removeListener(this);
    }

    // Shown by the editor as a short banner with an optional action (UNDO-style).
    std::function<void(const Notice&)> onNotice;

    // Bumped on every device, mute or activity change so views know when to refresh.
    int getRevision() const noexcept { return revision; }

    // -------------------------------------------------------------------------
    // Inventory and recommendation
    // -------------------------------------------------------------------------

    AudioAutopilot::ActiveDevice captureActiveDevice() const
    {
        AudioAutopilot::ActiveDevice active;
        auto& deviceManager = holder.deviceManager;

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            const auto setup = deviceManager.getAudioDeviceSetup();
            active.isOpen = device->isOpen();
            active.typeName = device->getTypeName();
            active.inputName = setup.inputDeviceName;
            active.outputName = setup.outputDeviceName;
            active.sampleRate = device->getCurrentSampleRate();
            active.bufferSize = device->getCurrentBufferSizeSamples();
            active.numInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
            active.numOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
            active.availableSampleRates = device->getAvailableSampleRates();
            active.availableBufferSizes = device->getAvailableBufferSizes();
        }

        return active;
    }

    AudioAutopilot::Inventory captureInventory() const
    {
        AudioAutopilot::Inventory inventory;

        for (auto* type : holder.deviceManager.getAvailableDeviceTypes())
        {
            AudioAutopilot::DriverTypeEntry entry;
            entry.typeName = type->getTypeName();
            entry.inputNames = type->getDeviceNames(true);
            entry.outputNames = type->getDeviceNames(false);
            entry.defaultInputIndex = type->getDefaultDeviceIndex(true);
            entry.defaultOutputIndex = type->getDefaultDeviceIndex(false);
            inventory.types.push_back(std::move(entry));
        }

        inventory.active = captureActiveDevice();
        return inventory;
    }

    AudioAutopilot::Recommendation getRecommendation() const
    {
        return AudioAutopilot::recommend(captureInventory(), options, failedCandidateKeys);
    }

    // Driver-reported input + output latency (both include the buffers) plus NOVA's own latency.
    double estimateRoundTripMs() const
    {
        auto* device = holder.deviceManager.getCurrentAudioDevice();
        if (device == nullptr || device->getCurrentSampleRate() <= 0.0)
            return 0.0;

        int driverSamples = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
        if (driverSamples <= 0)
            driverSamples = 2 * device->getCurrentBufferSizeSamples();

        return 1000.0 * (driverSamples + juce::jmax(0, processor.getLatencySamples())) / device->getCurrentSampleRate();
    }

    // -------------------------------------------------------------------------
    // Status for the wizard card
    // -------------------------------------------------------------------------

    Activity getActivity() const noexcept { return activity; }
    const juce::String& getStatusMessage() const noexcept { return statusMessage; }
    bool isBusy() const noexcept { return activity == Activity::Verifying || activity == Activity::Calibrating; }
    bool canUndo() const noexcept { return undoState != nullptr && ! isBusy(); }

    // 0..1 while calibrating.
    float getCalibrationProgress() const noexcept
    {
        if (activity != Activity::Calibrating || calibration.ladder.isEmpty())
            return 0.0f;

        const double phaseSeconds = calibration.phase == CalibrationRun::Phase::Settling
            ? calibrationOptions.settleSeconds : calibrationOptions.measureSeconds;
        const double phaseFraction = juce::jlimit(0.0, 1.0,
            (juce::Time::getMillisecondCounterHiRes() - calibration.phaseStartMs) / (1000.0 * phaseSeconds));
        const double stepFraction = calibration.phase == CalibrationRun::Phase::Settling
            ? 0.1 * phaseFraction : 0.1 + 0.9 * phaseFraction;

        return (float) juce::jlimit(0.0, 1.0, (calibration.index + stepFraction) / calibration.ladder.size());
    }

    // -------------------------------------------------------------------------
    // Apply / Undo (phase 1)
    // -------------------------------------------------------------------------

    void apply(const AudioAutopilot::Candidate& candidate)
    {
        if (isBusy())
            return;

        undoState = captureDeviceState();
        attemptedKey = candidate.getKey();
        attemptedDescription = AudioAutopilot::describeCandidate(candidate);

        auto xml = std::make_unique<juce::XmlElement>(*undoState);
        const bool sameDevices = xml->getStringAttribute("deviceType") == candidate.typeName
            && xml->getStringAttribute("audioInputDeviceName") == candidate.inputName
            && xml->getStringAttribute("audioOutputDeviceName") == candidate.outputName;

        xml->setAttribute("deviceType", candidate.typeName);
        xml->removeAttribute("audioDeviceName");
        xml->setAttribute("audioInputDeviceName", candidate.inputName);
        xml->setAttribute("audioOutputDeviceName", candidate.outputName);

        if (candidate.sampleRate > 0.0)
            xml->setAttribute("audioDeviceRate", candidate.sampleRate);
        else
            xml->removeAttribute("audioDeviceRate");

        if (candidate.bufferSize > 0)
            xml->setAttribute("audioDeviceBufferSize", candidate.bufferSize);
        else
            xml->removeAttribute("audioDeviceBufferSize");

        // Channel choices belong to the old device; a new one opens its default channels.
        if (! sameDevices)
        {
            xml->removeAttribute("audioDeviceInChans");
            xml->removeAttribute("audioDeviceOutChans");
        }

        const auto error = openFromXml(*xml, false);

        if (error.isNotEmpty() || holder.deviceManager.getCurrentAudioDevice() == nullptr)
        {
            revert("Couldn't open " + attemptedDescription
                   + (error.isNotEmpty() ? " (" + error.upToFirstOccurrenceOf("\n", false, false).trim() + ")." : juce::String(".")));
            return;
        }

        setActivity(Activity::Verifying, "Checking " + attemptedDescription + "...");
        verifyStartMs = juce::Time::getMillisecondCounterHiRes();
        verifyBaselineTaken = false;
        startTimer(kTimerIntervalMs);
    }

    void undo()
    {
        if (! canUndo())
            return;

        restoreUndoState();
        setActivity(Activity::Undone, "Previous audio setup restored.");
        evaluateInputMuteIfRouteChanged();
    }

    // -------------------------------------------------------------------------
    // Buffer calibration (phase 2)
    // -------------------------------------------------------------------------

    // Empty when calibration can start; otherwise the reason it can't.
    juce::String getCalibrationBlocker() const
    {
        if (isBusy())
            return "Wait for the current check to finish.";

        auto* device = holder.deviceManager.getCurrentAudioDevice();
        if (device == nullptr || ! device->isOpen())
            return "No audio device is running.";

        const auto family = AudioAutopilot::classifyDriverType(device->getTypeName());
        if (family == AudioAutopilot::DriverFamily::WasapiShared || family == AudioAutopilot::DriverFamily::DirectSound)
            return "Shared mode and DirectSound use the Windows mixer's buffer. Switch to the recommended driver to calibrate.";

        return {};
    }

    bool canOpenDriverPanel() const
    {
        auto* device = holder.deviceManager.getCurrentAudioDevice();
        return device != nullptr && device->hasControlPanel() && ! isBusy();
    }

    void startCalibration()
    {
        if (getCalibrationBlocker().isNotEmpty())
            return;

        auto* device = holder.deviceManager.getCurrentAudioDevice();
        const auto sizes = device->getAvailableBufferSizes();

        if (sizes.size() <= 1)
        {
            setActivity(Activity::CalibrationFailed,
                        "This driver only offers " + juce::String(device->getCurrentBufferSizeSamples())
                        + " samples. Change the buffer in the driver panel.");
            return;
        }

        calibration = {};
        calibration.ladder = AudioAutopilot::buildCalibrationLadder(sizes, calibrationOptions);
        calibration.routeKey = captureActiveDevice().getRouteKey();
        undoState = captureDeviceState();

        NovaDiagnostics::SessionLogger::logEvent("autopilot.calibration",
            "start device=" + calibration.routeKey + " ladder=" + joinSizes(calibration.ladder));

        processor.setOutputSilenced(true);
        setActivity(Activity::Calibrating, "Calibrating: output muted while NOVA tests each buffer size...");
        beginCalibrationStep();

        if (activity == Activity::Calibrating)
            startTimer(kTimerIntervalMs);
    }

    void cancelCalibration()
    {
        if (activity == Activity::Calibrating)
            finishCalibration(false, "Calibration cancelled. Your previous buffer is back.");
    }

    // Same steps as JUCE's AudioDeviceSelectorComponent: a modal blocker while the driver's
    // own dialog is up, then a device restart if the driver asks for it.
    void openDriverPanel()
    {
        auto* device = holder.deviceManager.getCurrentAudioDevice();
        if (device == nullptr || ! device->hasControlPanel() || isBusy())
            return;

        bool needsRestart = false;
        {
            juce::Component modalWindow;
            modalWindow.setOpaque(true);
            modalWindow.addToDesktop(0);
            modalWindow.enterModalState();
            needsRestart = device->showControlPanel();
        }

        if (needsRestart)
        {
            holder.deviceManager.closeAudioDevice();
            holder.deviceManager.restartLastAudioDevice();
        }

        ++revision;
    }

    // -------------------------------------------------------------------------
    // Input mute
    // -------------------------------------------------------------------------

    bool isInputMuted() const { return (bool) holder.getMuteInputValue().getValue(); }

    AudioAutopilot::InputMuteDecision getInputMuteDecision() const
    {
        return AudioAutopilot::adviseInputMute(captureActiveDevice());
    }

    // Wizard and banner buttons. Recorded like any other manual change (see valueChanged).
    void setInputMutedByUser(bool shouldMute)
    {
        if (isInputMuted() != shouldMute)
            holder.getMuteInputValue().setValue(shouldMute);
    }

private:
    static constexpr int kTimerIntervalMs = 100;
    static constexpr double kStartupDelayMs = 500.0;
    static constexpr double kVerifyBaselineMs = 500.0;
    static constexpr double kVerifyTimeoutMs = 2000.0;
    static constexpr const char* kMutePinnedKey = "novaInputMutePinnedByUser";
    static constexpr const char* kUnmuteApprovedRouteKey = "novaInputUnmuteApprovedRoute";

    struct CalibrationRun
    {
        enum class Phase { Settling, Measuring };

        juce::Array<int> ladder;
        int index = 0;
        juce::Array<int> testedSizes;
        juce::String routeKey;
        Phase phase = Phase::Settling;
        double phaseStartMs = 0.0;
        int xrunsAtStart = 0;
        uint64_t blocksAtStart = 0;
        juce::StringArray results;
    };

    void setActivity(Activity newActivity, const juce::String& message)
    {
        activity = newActivity;
        statusMessage = message;
        ++revision;
    }

    static juce::String joinSizes(const juce::Array<int>& sizes)
    {
        juce::StringArray parts;
        for (int size : sizes)
            parts.add(juce::String(size));
        return parts.joinIntoString(",");
    }

    // --- device switching ------------------------------------------------------

    std::unique_ptr<juce::XmlElement> captureDeviceState() const
    {
        auto& deviceManager = holder.deviceManager;

        if (auto xml = deviceManager.createStateXml())
            return xml;

        // No explicit settings yet (first run on JUCE defaults): describe the open device.
        auto xml = std::make_unique<juce::XmlElement>("DEVICESETUP");
        const auto setup = deviceManager.getAudioDeviceSetup();
        xml->setAttribute("deviceType", deviceManager.getCurrentAudioDeviceType());
        xml->setAttribute("audioOutputDeviceName", setup.outputDeviceName);
        xml->setAttribute("audioInputDeviceName", setup.inputDeviceName);

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            xml->setAttribute("audioDeviceRate", device->getCurrentSampleRate());
            xml->setAttribute("audioDeviceBufferSize", device->getCurrentBufferSizeSamples());
        }

        if (! setup.useDefaultInputChannels)
            xml->setAttribute("audioDeviceInChans", setup.inputChannels.toString(2));
        if (! setup.useDefaultOutputChannels)
            xml->setAttribute("audioDeviceOutChans", setup.outputChannels.toString(2));

        return xml;
    }

    // Opens exactly the device in the XML. setCurrentAudioDeviceType() is avoided on purpose:
    // it first opens the type's default device, which for ASIO means loading an arbitrary driver.
    juce::String openFromXml(const juce::XmlElement& xml, bool selectDefaultDeviceOnFailure)
    {
        auto& deviceManager = holder.deviceManager;
        const bool typeChanges = deviceManager.getCurrentAudioDeviceType() != xml.getStringAttribute("deviceType");

        // initialise() keeps the open device when only the driver type differs (e.g. Shared ->
        // Exclusive on the same endpoint), so close it first, with the same pause
        // AudioDeviceManager::setCurrentAudioDeviceType() takes to avoid driver clashes.
        // Within one type (new device or new rate) JUCE reopens the device itself.
        if (typeChanges && deviceManager.getCurrentAudioDevice() != nullptr)
        {
            deviceManager.closeAudioDevice();
            juce::Thread::sleep(1500);
        }

        return deviceManager.initialise(holder.getNumInputChannels(), holder.getNumOutputChannels(),
                                        &xml, selectDefaultDeviceOnFailure);
    }

    void restoreUndoState()
    {
        if (undoState == nullptr)
            return;

        // If even the previous setup fails to reopen, JUCE falls back to the default devices.
        openFromXml(*undoState, true);
        undoState.reset();
    }

    void revert(const juce::String& reason)
    {
        failedCandidateKeys.addIfNotAlreadyThere(attemptedKey);
        restoreUndoState();
        setActivity(Activity::Reverted, reason + " Your previous setup is back.");
        notify({ statusMessage, {}, {} });
        evaluateInputMuteIfRouteChanged();
    }

    void updateVerification()
    {
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - verifyStartMs;

        if (holder.deviceManager.getCurrentAudioDevice() == nullptr)
        {
            revert(attemptedDescription + " stopped right after opening.");
            return;
        }

        if (! verifyBaselineTaken)
        {
            if (elapsedMs >= kVerifyBaselineMs)
            {
                verifyBaselineBlocks = engine.getAudioBlockCount();
                verifyBaselineTaken = true;
            }
            return;
        }

        if (elapsedMs < kVerifyTimeoutMs)
            return;

        // Any movement counts: prepare() resets the counter when the driver restarts the stream.
        if (engine.getAudioBlockCount() == verifyBaselineBlocks)
        {
            revert("No audio from " + attemptedDescription + " after 2 s.");
            return;
        }

        setActivity(Activity::Applied, "Applied: " + attemptedDescription + ". UNDO restores your previous setup.");
        evaluateInputMuteIfRouteChanged();
    }

    // --- buffer calibration ----------------------------------------------------

    void beginCalibrationStep()
    {
        auto& deviceManager = holder.deviceManager;

        while (calibration.index < calibration.ladder.size())
        {
            const int requested = calibration.ladder[calibration.index];
            auto* device = deviceManager.getCurrentAudioDevice();

            if (device == nullptr)
            {
                finishCalibration(false, "The audio device stopped during calibration.");
                return;
            }

            if (device->getCurrentBufferSizeSamples() != requested)
            {
                auto setup = deviceManager.getAudioDeviceSetup();
                setup.bufferSize = requested;
                const auto error = deviceManager.setAudioDeviceSetup(setup, true);

                if (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr)
                {
                    finishCalibration(false, "Couldn't switch to " + juce::String(requested) + " samples.");
                    return;
                }
            }

            // Drivers may impose another size; never measure the same one twice.
            const int actual = deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples();
            if (calibration.testedSizes.contains(actual))
            {
                ++calibration.index;
                continue;
            }

            calibration.testedSizes.add(actual);
            calibration.phase = CalibrationRun::Phase::Settling;
            calibration.phaseStartMs = juce::Time::getMillisecondCounterHiRes();
            setActivity(Activity::Calibrating, "Calibrating: testing " + juce::String(actual) + " samples ("
                        + juce::String(calibration.index + 1) + " of " + juce::String(calibration.ladder.size())
                        + "). Output muted.");
            return;
        }

        finishCalibration(false, "No buffer up to " + juce::String(calibrationOptions.lastBufferSamples)
                          + " samples ran cleanly (" + calibration.results.joinIntoString("; ")
                          + "). Your previous buffer is back. Try closing other apps or a lighter preset.");
    }

    void updateCalibration()
    {
        auto& deviceManager = holder.deviceManager;
        auto* device = deviceManager.getCurrentAudioDevice();

        if (device == nullptr || captureActiveDevice().getRouteKey() != calibration.routeKey)
        {
            finishCalibration(false, "The audio device changed during calibration. Your previous buffer is back.");
            return;
        }

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double elapsedMs = nowMs - calibration.phaseStartMs;

        if (calibration.phase == CalibrationRun::Phase::Settling)
        {
            if (elapsedMs < 1000.0 * calibrationOptions.settleSeconds)
                return;

            engine.requestTimingWindowReset();
            calibration.xrunsAtStart = deviceManager.getXRunCount();
            calibration.blocksAtStart = engine.getAudioBlockCount();
            calibration.phase = CalibrationRun::Phase::Measuring;
            calibration.phaseStartMs = nowMs;
            return;
        }

        if (elapsedMs < 1000.0 * calibrationOptions.measureSeconds)
            return;

        // Counters that went backwards mean the stream restarted mid-window: count it as a dropout.
        const int xrunsNow = deviceManager.getXRunCount();
        const uint64_t blocksNow = engine.getAudioBlockCount();

        AudioAutopilot::StabilityMeasurement measurement;
        measurement.bufferSize = device->getCurrentBufferSizeSamples();
        measurement.sampleRate = device->getCurrentSampleRate();
        measurement.windowSeconds = elapsedMs / 1000.0;
        measurement.xruns = xrunsNow >= calibration.xrunsAtStart ? xrunsNow - calibration.xrunsAtStart : 1;
        measurement.blocksDelivered = blocksNow >= calibration.blocksAtStart ? (juce::int64) (blocksNow - calibration.blocksAtStart) : 0;
        measurement.peakProcessMs = engine.getWindowPeakProcessTimeMs();
        measurement.maxCallbackGapMs = engine.getWindowMaxCallbackIntervalMs();

        const auto verdict = AudioAutopilot::evaluateStability(measurement, calibrationOptions);
        calibration.results.add(juce::String(measurement.bufferSize) + ": " + verdict.summary);

        NovaDiagnostics::SessionLogger::logEvent("autopilot.calibration",
            "buffer=" + juce::String(measurement.bufferSize)
            + " stable=" + juce::String(verdict.stable ? "true" : "false")
            + " xruns=" + juce::String(measurement.xruns)
            + " peakLoad=" + juce::String(verdict.peakLoad, 3)
            + " gapRatio=" + juce::String(verdict.gapRatio, 2)
            + " delivered=" + juce::String(verdict.deliveredRatio, 3));

        if (verdict.stable)
        {
            finishCalibration(true, "Stable at " + juce::String(measurement.bufferSize) + " samples ("
                              + verdict.summary + "). ~" + juce::String(estimateRoundTripMs(), 1)
                              + " ms round trip. UNDO restores your previous buffer.");
            return;
        }

        ++calibration.index;
        beginCalibrationStep();
    }

    void finishCalibration(bool stable, const juce::String& message)
    {
        processor.setOutputSilenced(false);

        if (! stable)
            restoreUndoState();

        NovaDiagnostics::SessionLogger::logEvent("autopilot.calibration", "finish " + message);
        setActivity(stable ? Activity::Calibrated : Activity::CalibrationFailed, message);

        if (stable)
            notify({ message, "UNDO", [this] { undo(); } });
        else
            notify({ message, {}, {} });
    }

    // --- input mute -------------------------------------------------------------

    void evaluateInputMuteIfRouteChanged()
    {
        const auto active = captureActiveDevice();
        const auto routeKey = active.isOpen ? active.getRouteKey() : juce::String();

        if (routeKey == lastEvaluatedRoute)
            return;

        lastEvaluatedRoute = routeKey;
        evaluateInputMute(active, true);
    }

    void evaluateInputMute(const AudioAutopilot::ActiveDevice& route, bool routeChanged)
    {
        const auto decision = AudioAutopilot::adviseInputMute(route);
        const bool muted = isInputMuted();

        if (decision.advice == AudioAutopilot::MuteAdvice::Unmute && muted && ! mutePinnedByUser)
        {
            setInputMutedAutomatically(false);
            notify({ "Input on. " + decision.reason, "MUTE", [this] { setInputMutedByUser(true); } });
        }
        else if (decision.advice == AudioAutopilot::MuteAdvice::KeepMuted && decision.feedbackRisk
                 && ! muted && route.getRouteKey() != unmuteApprovedRoute)
        {
            setInputMutedAutomatically(true);
            notify({ "Input muted. " + decision.reason, "UNMUTE", [this] { setInputMutedByUser(false); } });
        }
        else if (decision.advice == AudioAutopilot::MuteAdvice::KeepMuted && muted && routeChanged && ! mutePinnedByUser)
        {
            notify({ "Input stays muted. " + decision.reason, "UNMUTE", [this] { setInputMutedByUser(false); } });
        }
    }

    void setInputMutedAutomatically(bool shouldMute)
    {
        if (isInputMuted() == shouldMute)
            return;

        automaticMuteChangeInFlight = true;
        holder.getMuteInputValue().setValue(shouldMute);
    }

    // Value notifications are asynchronous, so the value itself tells whether this is news.
    void valueChanged(juce::Value& value) override
    {
        const bool muted = (bool) value.getValue();
        if (muted == lastSeenMuted)
            return;

        lastSeenMuted = muted;
        ++revision;

        if (automaticMuteChangeInFlight)
        {
            automaticMuteChangeInFlight = false;
            return;
        }

        // Manual change: from JUCE's audio settings, the wizard, or a banner button.
        mutePinnedByUser = muted;

        if (! muted && getInputMuteDecision().advice == AudioAutopilot::MuteAdvice::KeepMuted)
            unmuteApprovedRoute = captureActiveDevice().getRouteKey();

        if (auto* props = holder.settings.get())
        {
            props->setValue(kMutePinnedKey, mutePinnedByUser);
            props->setValue(kUnmuteApprovedRouteKey, unmuteApprovedRoute);
        }
    }

    // --- callbacks ----------------------------------------------------------------

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        ++revision;

        // Mid-switch, mid-calibration and pre-startup changes are evaluated once those settle.
        if (! isBusy() && ! startupPending)
            evaluateInputMuteIfRouteChanged();
    }

    void timerCallback() override
    {
        if (startupPending && juce::Time::getMillisecondCounterHiRes() - createdAtMs >= kStartupDelayMs)
        {
            startupPending = false;
            const auto active = captureActiveDevice();
            lastEvaluatedRoute = active.isOpen ? active.getRouteKey() : juce::String();
            evaluateInputMute(active, false);
        }

        if (activity == Activity::Verifying)
            updateVerification();
        else if (activity == Activity::Calibrating)
            updateCalibration();

        if (! startupPending && ! isBusy())
            stopTimer();
    }

    void notify(const Notice& notice)
    {
        if (onNotice != nullptr)
            onNotice(notice);
    }

    juce::StandalonePluginHolder& holder;
    NOVAAudioProcessor& processor;
    AudioEngine& engine;
    AudioAutopilot::Options options;
    AudioAutopilot::CalibrationOptions calibrationOptions;

    Activity activity = Activity::None;
    juce::String statusMessage;
    std::unique_ptr<juce::XmlElement> undoState;

    // Apply
    juce::String attemptedKey;
    juce::String attemptedDescription;
    juce::StringArray failedCandidateKeys;
    double verifyStartMs = 0.0;
    bool verifyBaselineTaken = false;
    uint64_t verifyBaselineBlocks = 0;

    // Calibration
    CalibrationRun calibration;

    // Input mute
    bool mutePinnedByUser = false;
    juce::String unmuteApprovedRoute;
    juce::String lastEvaluatedRoute;
    bool lastSeenMuted = true;
    bool automaticMuteChangeInFlight = false;

    bool startupPending = true;
    double createdAtMs = 0.0;
    int revision = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StandaloneAudioAutopilot)
};

// Short-lived banner for autopilot notices (top of the editor, above overlays).
class AutopilotNoticeBanner final : public juce::Component, private juce::Timer
{
public:
    AutopilotNoticeBanner()
    {
        setAlwaysOnTop(true);
        setInterceptsMouseClicks(true, true);

        actionButton.setColour(juce::TextButton::buttonColourId, Nova::Colors::Accent.withAlpha(0.22f));
        actionButton.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text);
        actionButton.onClick = [this]
        {
            auto pendingAction = action;
            dismiss();
            if (pendingAction != nullptr)
                pendingAction();
        };
        addChildComponent(actionButton);

        closeButton.setButtonText("X");
        closeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        closeButton.setColour(juce::TextButton::textColourOffId, Nova::Colors::TextDim);
        closeButton.onClick = [this] { dismiss(); };
        addAndMakeVisible(closeButton);
    }

    void show(const StandaloneAudioAutopilot::Notice& notice)
    {
        text = notice.text;
        action = notice.action;
        actionButton.setButtonText(notice.actionLabel);
        actionButton.setVisible(notice.actionLabel.isNotEmpty() && action != nullptr);

        setVisible(true);
        toFront(false);
        resized();
        repaint();
        startTimer(9000);
    }

    void dismiss()
    {
        stopTimer();
        action = nullptr;
        setVisible(false);
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(juce::Colour::fromString("f0101722"));
        g.fillRoundedRectangle(bounds, 10.0f);
        g.setColour(Nova::Colors::Accent.withAlpha(0.55f));
        g.drawRoundedRectangle(bounds, 10.0f, 1.0f);

        g.setColour(Nova::Colors::Text.withAlpha(0.92f));
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawFittedText(text, textArea, juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10, 6);
        closeButton.setBounds(area.removeFromRight(28));
        area.removeFromRight(6);

        if (actionButton.isVisible())
        {
            actionButton.setBounds(area.removeFromRight(90));
            area.removeFromRight(10);
        }

        textArea = area;
    }

private:
    void timerCallback() override { dismiss(); }

    juce::String text;
    std::function<void()> action;
    juce::Rectangle<int> textArea;
    juce::TextButton actionButton;
    juce::TextButton closeButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutopilotNoticeBanner)
};
}
#endif
