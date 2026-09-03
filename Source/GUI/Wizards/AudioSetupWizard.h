#pragma once
#include "WizardBase.h"
#include "../../Core/PluginProcessor.h"
#include "../../Core/Audio/AudioAutoConfig.h"

#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

// =============================================================================
// Interactive Audio Setup Wizard
//
// Every step WAITS for explicit user feedback before allowing progression.
// Each step generates dynamic recommendations based on user selections and
// detected hardware state.
//
// Step 0 - System Scan:    Hardware detection + prioritized recommendations.
//                          Gated by explicit "GOT IT" acknowledgment.
// Step 1 - Device:         Full JUCE device selector + live status bar +
//                          recommendation banner from scan results.
// Step 2 - Latency:        Use-case card selector (none pre-selected, gates NEXT).
//                          Dynamic buffer size recommendation + APPLY button.
// Step 3 - Calibration:    3-phase interactive flow:
//                            Phase A: User clicks START -> noise floor measurement
//                            Phase B: User clicks CONTINUE -> signal level measurement
//                            Phase C: Results + gate/gain recommendations + APPLY
//                          Skippable via SKIP button.
// Step 4 - Tuner:          3 preset cards + custom Hz slider.
// Step 5 - Summary:        Interactive report card with per-item EDIT links and
//                          one-click APPLY ALL.
// =============================================================================
class AudioSetupWizard final : public WizardBase, private juce::Timer
{
public:
    AudioSetupWizard(NOVAAudioProcessor& proc, std::function<void()> onCloseFn)
        : WizardBase(buildSteps(), std::move(onCloseFn)),
          processor(proc)
    {
        // =================================================================
        // Step 0: System Scan
        // =================================================================
        {
            auto* page = new juce::Component();
            addPage(0, page);

            styleSecondaryButton(btnAcknowledge, "GOT IT");
            btnAcknowledge.onClick = [this]
            {
                scanAcknowledged = true;
                btnAcknowledge.setEnabled(false);
                btnAcknowledge.setButtonText("REVIEWED");
                setNextEnabled(true);
                repaint();
            };
            page->addAndMakeVisible(btnAcknowledge);

            styleAccentButton(btnApplyBest, "APPLY BEST CONFIG");
            btnApplyBest.onClick = [this] { applyRecommendation(); };
            page->addAndMakeVisible(btnApplyBest);
        }

        // =================================================================
        // Step 1: Device Selection
        // =================================================================
        {
            auto* page = new juce::Component();
            page->setLookAndFeel(&selectorLnf);
#if JucePlugin_Build_Standalone
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
            {
                const int maxIn  = juce::jmax(2, proc.getMainBusNumInputChannels());
                const int maxOut = juce::jmax(2, proc.getMainBusNumOutputChannels());
                deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent>(
                    holder->deviceManager, 0, maxIn, 0, maxOut,
                    true, proc.producesMidi(), true, false);
                deviceSelector->setItemHeight(26);
                deviceSelector->setLookAndFeel(&selectorLnf);
                page->addAndMakeVisible(deviceSelector.get());
            }
#endif
            addPage(1, page);
        }

        // =================================================================
        // Step 2: Latency / Use-Case (none pre-selected -> forces choice)
        // =================================================================
        {
            auto* page = new juce::Component();
            addPage(2, page);

            static const char* useCaseLabels[] = {
                "LIVE PERFORMANCE",
                "PRACTICE & JAMMING",
                "RECORDING & MIXING"
            };
            static const char* useCaseDescs[] = {
                "Lowest latency. Ideal for stage and real-time monitoring.",
                "Balanced latency and stability. Great for daily playing.",
                "Maximum stability. Dropouts matter more than latency."
            };

            for (int i = 0; i < 3; ++i)
            {
                useCaseButtons[i].setButtonText(useCaseLabels[i]);
                useCaseButtons[i].setRadioGroupId(9001);
                useCaseButtons[i].setClickingTogglesState(true);
                useCaseButtons[i].setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff101722"));
                useCaseButtons[i].setColour(juce::TextButton::buttonOnColourId, Nova::Colors::Accent.withAlpha(0.22f));
                useCaseButtons[i].setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.70f));
                useCaseButtons[i].setColour(juce::TextButton::textColourOnId, Nova::Colors::Text);
                useCaseButtons[i].onClick = [this, i]
                {
                    useCaseSelected = true;
                    Nova::Audio::AudioAutoConfig::chooseBufferForUseCase(recommendation, i);
                    setNextEnabled(true);
                    repaint();
                };
                page->addAndMakeVisible(useCaseButtons[i]);
            }
            juce::ignoreUnused(useCaseDescs);

            styleAccentButton(btnApplyBuffer, "APPLY RECOMMENDED BUFFER");
            btnApplyBuffer.onClick = [this] { applyRecommendedBuffer(); };
            btnApplyBuffer.setVisible(false);
            page->addAndMakeVisible(btnApplyBuffer);
        }

        // =================================================================
        // Step 3: Input Calibration (interactive multi-phase)
        // =================================================================
        {
            auto* page = new CalibrationPage(proc);
            calibrationPage = page;
            addPage(3, page);
        }

        // =================================================================
        // Step 4: Tuner Reference (presets + custom Hz)
        // =================================================================
        {
            auto* page = new juce::Component();
            addPage(4, page);

            static const char* pitchLabels[] = { "440 Hz", "442 Hz", "432 Hz" };
            for (int i = 0; i < 3; ++i)
            {
                tunerCards[i].setRadioGroupId(9002);
                tunerCards[i].setClickingTogglesState(true);
                tunerCards[i].setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff101722"));
                tunerCards[i].setColour(juce::TextButton::buttonOnColourId, Nova::Colors::Accent.withAlpha(0.20f));
                tunerCards[i].setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.72f));
                tunerCards[i].setColour(juce::TextButton::textColourOnId, Nova::Colors::Text);
                tunerCards[i].setButtonText(pitchLabels[i]);
                tunerCards[i].onClick = [this]
                {
                    customHzActive = false;
                    repaint();
                };
                page->addAndMakeVisible(tunerCards[i]);
            }

            // Custom Hz toggle
            btnCustomHz.setButtonText("CUSTOM");
            btnCustomHz.setClickingTogglesState(true);
            btnCustomHz.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff101722"));
            btnCustomHz.setColour(juce::TextButton::buttonOnColourId, Nova::Colors::Accent.withAlpha(0.20f));
            btnCustomHz.setColour(juce::TextButton::textColourOffId, Nova::Colors::TextDim);
            btnCustomHz.setColour(juce::TextButton::textColourOnId, Nova::Colors::Text);
            btnCustomHz.onClick = [this]
            {
                customHzActive = btnCustomHz.getToggleState();
                if (customHzActive)
                {
                    for (auto& tc : tunerCards)
                        tc.setToggleState(false, juce::dontSendNotification);
                }
                customHzSlider.setVisible(customHzActive);
                repaint();
            };
            page->addAndMakeVisible(btnCustomHz);

            customHzSlider.setRange(400.0, 480.0, 0.1);
            customHzSlider.setValue(440.0, juce::dontSendNotification);
            customHzSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 26);
            customHzSlider.setColour(juce::Slider::trackColourId, Nova::Colors::Accent);
            customHzSlider.setColour(juce::Slider::thumbColourId, Nova::Colors::Accent);
            customHzSlider.setColour(juce::Slider::textBoxTextColourId, Nova::Colors::Text);
            customHzSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour::fromString("ff101722"));
            customHzSlider.setColour(juce::Slider::textBoxOutlineColourId, Nova::Colors::Border);
            customHzSlider.onValueChange = [this] { repaint(); };
            customHzSlider.setVisible(false);
            page->addAndMakeVisible(customHzSlider);

            // Pre-select based on current setting
            auto currentRef = EditorPrefs::getString(EditorPrefs::tunerReferenceKey, "A = 440 Hz");
            if (currentRef.contains("442"))      tunerCards[1].setToggleState(true, juce::dontSendNotification);
            else if (currentRef.contains("432")) tunerCards[2].setToggleState(true, juce::dontSendNotification);
            else                                 tunerCards[0].setToggleState(true, juce::dontSendNotification);
        }

        // =================================================================
        // Step 5: Summary (interactive report)
        // =================================================================
        {
            auto* page = new juce::Component();
            addPage(5, page);

            styleAccentButton(btnApplyAll, "APPLY ALL SETTINGS");
            btnApplyAll.onClick = [this] { applyAllSettings(); };
            page->addAndMakeVisible(btnApplyAll);

            for (int i = 0; i < kMaxEditButtons; ++i)
            {
                editButtons[i].setButtonText("EDIT");
                editButtons[i].setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
                editButtons[i].setColour(juce::TextButton::textColourOffId, Nova::Colors::Accent);
                editButtons[i].setVisible(false);
                page->addAndMakeVisible(editButtons[i]);
            }
        }

        runSystemScan();
        setStep(0);
        startTimerHz(20);
    }

    ~AudioSetupWizard() override
    {
        stopTimer();
        if (deviceSelector) deviceSelector->setLookAndFeel(nullptr);
    }

    // =================================================================
    // Step Hooks
    // =================================================================

    bool canAdvance(int step) override
    {
        if (step == 0) return scanAcknowledged;
        return true;
    }

    void onStepActivated(int step) override
    {
        switch (step)
        {
            case 0:
                runSystemScan();
                refreshRecommendation();
                applyStatus = {};
                btnApplyBest.setEnabled(recommendation.valid);
                btnApplyBest.setButtonText("APPLY BEST CONFIG");
                scanAcknowledged = false;
                btnAcknowledge.setEnabled(true);
                btnAcknowledge.setButtonText("GOT IT");
                setNextEnabled(false);
                setStepSkippable(false);
                break;

            case 1:
                setNextEnabled(true);
                setStepSkippable(false);
                break;

            case 2:
                useCaseSelected = false;
                for (auto& b : useCaseButtons)
                    b.setToggleState(false, juce::dontSendNotification);
                btnApplyBuffer.setVisible(false);
                setNextEnabled(false);
                setStepSkippable(false);
                break;

            case 3:
                if (calibrationPage) calibrationPage->reset();
                setNextEnabled(false);
                setStepSkippable(true);
                break;

            case 4:
                setNextEnabled(true);
                setStepSkippable(true);
                break;

            case 5:
                buildSummary();
                setNextEnabled(true);
                setStepSkippable(false);
                setNextText("FINISH");
                break;
        }
    }

    void onStepLeaving(int step) override
    {
        if (step == 3 && calibrationPage) calibrationPage->stopCalibration();
    }

    void onFinished() override
    {
        applyAllSettings();
    }

    // =================================================================
    // Layout
    // =================================================================

    void resized() override
    {
        WizardBase::resized();

        // Every control below is a child of its step's page component, and
        // WizardBase places that page at the content area. Laying them out in
        // wizard coordinates would offset them by the page origin and push them
        // out of view, so work in page-local coordinates.
        auto area = getContentArea().withTrimmedTop(60).withZeroOrigin();
        if (area.isEmpty()) return;

        // Step 0: Acknowledge button
        {
            const int bw = 200, bh = 38, gap = 12;
            const int sx = area.getCentreX() - (bw * 2 + gap) / 2;
            btnApplyBest.setBounds(sx, area.getBottom() - 48, bw, bh);
            btnAcknowledge.setBounds(sx + bw + gap, area.getBottom() - 48, bw, bh);
        }

        // Step 1: Device selector
        if (deviceSelector)
            deviceSelector->setBounds(area.withTrimmedBottom(40));

        // Step 2: Use-case buttons + apply buffer
        {
            auto ucArea = area;
            const int btnH = 56;
            const int gap = 10;
            const int btnW = juce::jmin(520, ucArea.getWidth());
            const int cx = ucArea.getCentreX() - btnW / 2;
            int y = ucArea.getY();
            for (int i = 0; i < 3; ++i)
            {
                useCaseButtons[i].setBounds(cx, y, btnW, btnH);
                y += btnH + gap;
            }
            btnApplyBuffer.setBounds(cx, y + 120, 260, 36);
        }

        // Step 4: Tuner cards + custom Hz
        {
            auto tArea = area;
            const int cardW = juce::jmin(140, (tArea.getWidth() - 80) / 4);
            const int cardH = 68;
            const int gap = 12;
            const int totalW = cardW * 3 + gap * 2 + gap + cardW;
            const int sx = tArea.getCentreX() - totalW / 2;
            const int sy = tArea.getY() + 24;
            for (int i = 0; i < 3; ++i)
                tunerCards[i].setBounds(sx + i * (cardW + gap), sy, cardW, cardH);

            btnCustomHz.setBounds(sx + 3 * (cardW + gap), sy, cardW, cardH);
            customHzSlider.setBounds(sx, sy + cardH + 16, totalW, 30);
        }

        // Step 5: Apply all + edit buttons
        {
            auto sArea = area;
            btnApplyAll.setBounds(sArea.getCentreX() - 130, sArea.getBottom() - 48, 260, 38);
            // Edit buttons positioned dynamically in buildSummary
        }
    }

    // =================================================================
    // Paint
    // =================================================================

    void paint(juce::Graphics& g) override
    {
        WizardBase::paint(g);
        const int step = getCurrentStep();
        auto area = getContentArea().withTrimmedTop(60);
        if (area.isEmpty()) return;

        switch (step)
        {
            case 0: paintSystemScan(g, area);   break;
            case 1: paintDeviceStatus(g, area);  break;
            case 2: paintLatencyStep(g, area);   break;
            case 4: paintTunerStep(g, area);     break;
            case 5: paintSummary(g, area);       break;
            default: break;
        }
    }

private:
    // =================================================================
    // Step Definitions
    // =================================================================

    static std::vector<StepDef> buildSteps()
    {
        return {
            { "Scan",      "System Scan",
              "NOVA detected your hardware. Review the results below." },
            { "Device",    "Audio Device",
              "Select your audio driver and I/O devices." },
            { "Latency",   "Latency Profile",
              "Choose your workflow so NOVA can optimize settings for you." },
            { "Calibrate", "Input Calibration",
              "Let NOVA measure your signal to recommend optimal settings." },
            { "Tuner",     "Tuner Reference",
              "Select your concert pitch standard." },
            { "Done",      "Setup Complete",
              "Review your configuration and apply changes." }
        };
    }

    // =================================================================
    // STEP 0 — System Scan
    // =================================================================

    struct ScanResult
    {
        juce::String driverType = "Unknown";
        juce::String inputDevice = "None";
        juce::String outputDevice = "None";
        double sampleRate = 0.0;
        int bufferSize = 0;
        int numInputs = 0;
        int numOutputs = 0;
        double latencyMs = 0.0;
        bool asioAvailable = false;
        bool deviceActive = false;
    };

    ScanResult scan;
    bool scanAcknowledged = false;
    juce::TextButton btnAcknowledge;

    void runSystemScan()
    {
        scan = {};
#if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            auto& dm = holder->deviceManager;
            for (auto* type : dm.getAvailableDeviceTypes())
                if (type->getTypeName().containsIgnoreCase("ASIO"))
                    scan.asioAvailable = true;

            if (auto* dev = dm.getCurrentAudioDevice())
            {
                scan.deviceActive = true;
                scan.driverType = dev->getTypeName();
                scan.sampleRate = dev->getCurrentSampleRate();
                scan.bufferSize = dev->getCurrentBufferSizeSamples();
                scan.numInputs = dev->getActiveInputChannels().countNumberOfSetBits();
                scan.numOutputs = dev->getActiveOutputChannels().countNumberOfSetBits();
                scan.latencyMs = scan.sampleRate > 0
                    ? (scan.bufferSize / scan.sampleRate) * 1000.0 : 0.0;

                juce::AudioDeviceManager::AudioDeviceSetup setup;
                dm.getAudioDeviceSetup(setup);
                scan.inputDevice = setup.inputDeviceName.isNotEmpty()
                    ? setup.inputDeviceName : "Default";
                scan.outputDevice = setup.outputDeviceName.isNotEmpty()
                    ? setup.outputDeviceName : "Default";
            }
        }
#endif
    }

    // Recomputes the best configuration this machine can run. The use case, when
    // already chosen, decides the buffer target; before that the balanced profile
    // is assumed so step 0 has something concrete to show.
    void refreshRecommendation()
    {
#if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            const int uc = getSelectedUseCase();
            recommendation = Nova::Audio::AudioAutoConfig::compute(holder->deviceManager,
                uc < 0 ? 1 : uc);
            return;
        }
#endif
        recommendation = {};
    }

    void applyRecommendation()
    {
#if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            const auto error = Nova::Audio::AudioAutoConfig::apply(holder->deviceManager,
                recommendation);

            if (error.isNotEmpty())
            {
                applyStatus = error;
            }
            else
            {
                applyStatus = {};
                btnApplyBest.setButtonText("APPLIED");
                btnApplyBest.setEnabled(false);
                // The device selector on step 1 mirrors the device manager, so it
                // picks the change up on its own; the scan rows do not.
                runSystemScan();
            }

            repaint();
            return;
        }
#endif
        applyStatus = "Audio device is managed by the host.";
        repaint();
    }

    void paintSystemScan(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        const int cardW = juce::jmin(560, area.getWidth() - 20);
        const int cx = area.getCentreX() - cardW / 2;
        int y = area.getY();

        // ---- What is running right now -------------------------------------
        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("CURRENTLY ACTIVE", cx, y, cardW, 14, juce::Justification::centredLeft);
        y += 18;

        struct Row { juce::String label; juce::String value; bool ok; };
        std::vector<Row> rows;

        if (scan.deviceActive)
        {
            rows.push_back({ "Driver", scan.driverType, true });
            rows.push_back({ "Device", scan.outputDevice, scan.numOutputs > 0 });
            rows.push_back({ "Format", juce::String(scan.sampleRate, 0) + " Hz  |  "
                + juce::String(scan.bufferSize) + " samples", true });
            rows.push_back({ "Latency", juce::String(scan.latencyMs, 1) + " ms one-way",
                scan.latencyMs <= 12.0 });
        }
        else
        {
            rows.push_back({ "Audio Device", "No active device detected", false });
        }

        const int rowH = 24;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto rowRect = juce::Rectangle<int>(cx, y, cardW, rowH);
            if (i % 2 == 0)
            {
                g.setColour(juce::Colour::fromString("ff0E1520"));
                g.fillRoundedRectangle(rowRect.toFloat(), 6.0f);
            }

            g.setColour(rows[i].ok ? Nova::Colors::Success : juce::Colour::fromString("ffF59E0B"));
            g.fillEllipse((float)(cx + 10), (float)(y + rowH / 2 - 4), 8.0f, 8.0f);

            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawText(rows[i].label, cx + 28, y, 90, rowH, juce::Justification::centredLeft);

            g.setColour(Nova::Colors::Text.withAlpha(0.9f));
            g.setFont(juce::Font(juce::FontOptions(11.5f)));
            g.drawText(rows[i].value, cx + 124, y, cardW - 134, rowH, juce::Justification::centredLeft);
            y += rowH;
        }

        // ---- Best configuration available ----------------------------------
        y += 14;

        const bool active = isRecommendationActive();
        const int boxH = 120;
        auto box = juce::Rectangle<int>(cx, y, cardW, boxH);

        g.setColour(Nova::Colors::Accent.withAlpha(0.08f));
        g.fillRoundedRectangle(box.toFloat(), 10.0f);
        g.setColour(Nova::Colors::Accent.withAlpha(0.35f));
        g.drawRoundedRectangle(box.toFloat().reduced(0.5f), 10.0f, 1.0f);

        g.setColour(Nova::Colors::Accent);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("BEST AVAILABLE ON THIS SYSTEM", cx + 14, y + 6, cardW - 120, 14,
            juce::Justification::centredLeft);

        if (active)
        {
            g.setColour(Nova::Colors::Success);
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawText("ACTIVE", cx + cardW - 100, y + 6, 86, 14, juce::Justification::centredRight);
        }

        if (!recommendation.valid)
        {
            g.setColour(Nova::Colors::Text.withAlpha(0.85f));
            g.setFont(juce::Font(juce::FontOptions(11.5f)));
            g.drawFittedText(applyStatus.isNotEmpty() ? applyStatus
                    : "No audio device could be inspected. Connect an interface and reopen this step.",
                cx + 14, y + 26, cardW - 28, 40, juce::Justification::topLeft, 2);
            return;
        }

        // Headline: the configuration itself.
        g.setColour(Nova::Colors::Text);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(recommendation.deviceTypeName + "  |  "
            + juce::String(recommendation.sampleRate, 0) + " Hz  |  "
            + juce::String(recommendation.bufferSize) + " samples  |  "
            + juce::String(recommendation.latencyMs(), 1) + " ms",
            cx + 14, y + 24, cardW - 28, 18, juce::Justification::centredLeft);

        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(recommendation.outputDeviceName, cx + 14, y + 42, cardW - 28, 16,
            juce::Justification::centredLeft);

        // Why this is the ceiling — the part that tells the user what to upgrade.
        g.setColour(Nova::Colors::Text.withAlpha(0.82f));
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawFittedText(applyStatus.isNotEmpty() ? applyStatus : recommendation.ceilingReason,
            cx + 14, y + 62, cardW - 28, 50, juce::Justification::topLeft, 3);
    }

    bool isRecommendationActive() const
    {
#if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            return Nova::Audio::AudioAutoConfig::matchesCurrentDevice(holder->deviceManager,
                recommendation);
#endif
        return false;
    }

    // =================================================================
    // STEP 1 — Device Selection
    // =================================================================

    void paintDeviceStatus(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        // Recommendation banner at top: only shown while the manual selection below
        // is still short of what this machine can reach.
        if (recommendation.valid && !isRecommendationActive())
        {
            auto banner = juce::Rectangle<int>(area.getX(), area.getY() - 56, area.getWidth(), 32);
            g.setColour(juce::Colour::fromString("ff1A1408").withAlpha(0.9f));
            g.fillRoundedRectangle(banner.toFloat(), 8.0f);
            g.setColour(juce::Colour::fromString("ffF59E0B").withAlpha(0.4f));
            g.drawRoundedRectangle(banner.toFloat().reduced(0.5f), 8.0f, 1.0f);
            g.setColour(juce::Colour::fromString("ffF59E0B"));
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawText("RECOMMENDED: " + recommendation.deviceTypeName + "  |  "
                + juce::String(recommendation.sampleRate, 0) + " Hz  |  "
                + juce::String(recommendation.bufferSize) + " samples  ("
                + juce::String(recommendation.latencyMs(), 1) + " ms)",
                banner, juce::Justification::centred);
        }

        // Live status strip at bottom
#if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            auto strip = getContentArea().removeFromBottom(36).reduced(4, 0);
            g.setColour(juce::Colour::fromString("ff0A0F18").withAlpha(0.92f));
            g.fillRoundedRectangle(strip.toFloat(), 8.0f);

            if (auto* dev = holder->deviceManager.getCurrentAudioDevice())
            {
                const double sr = dev->getCurrentSampleRate();
                const int buf = dev->getCurrentBufferSizeSamples();
                const double lat = sr > 0 ? (buf / sr) * 1000.0 : 0.0;

                g.setColour(Nova::Colors::Success);
                g.fillEllipse((float)strip.getX() + 12.0f, (float)strip.getCentreY() - 3.0f, 6.0f, 6.0f);

                g.setColour(Nova::Colors::Text.withAlpha(0.8f));
                g.setFont(juce::Font(juce::FontOptions(11.0f)));
                g.drawText(dev->getTypeName() + "  |  "
                    + juce::String(sr, 0) + " Hz  |  "
                    + juce::String(buf) + " smp  |  "
                    + juce::String(lat, 1) + " ms",
                    strip.withTrimmedLeft(24), juce::Justification::centred);
            }
            else
            {
                g.setColour(Nova::Colors::Error);
                g.fillEllipse((float)strip.getX() + 12.0f, (float)strip.getCentreY() - 3.0f, 6.0f, 6.0f);
                g.setColour(Nova::Colors::TextDim);
                g.setFont(juce::Font(juce::FontOptions(11.0f)));
                g.drawText("No audio device active", strip.withTrimmedLeft(24), juce::Justification::centred);
            }
            return;
        }
#endif
        auto strip = getContentArea().removeFromBottom(36).reduced(4, 0);
        g.setColour(juce::Colour::fromString("ff0A0F18").withAlpha(0.92f));
        g.fillRoundedRectangle(strip.toFloat(), 8.0f);
        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Audio device managed by host (DAW)", strip, juce::Justification::centred);
    }

    // =================================================================
    // STEP 2 — Latency / Use-Case
    // =================================================================

    bool useCaseSelected = false;
    juce::TextButton useCaseButtons[3];
    juce::TextButton btnApplyBuffer;
    bool bufferApplied = false;

    int getSelectedUseCase() const
    {
        for (int i = 0; i < 3; ++i)
            if (useCaseButtons[i].getToggleState()) return i;
        return -1;
    }

    int getRecommendedBufferSize() const
    {
        const int uc = getSelectedUseCase();
        if (uc < 0) return 0;

        // The recommendation already reflects what this device actually offers.
        if (recommendation.valid && recommendation.bufferSize > 0)
            return recommendation.bufferSize;

        return Nova::Audio::AudioAutoConfig::bufferTargetForUseCase(uc);
    }

    void applyRecommendedBuffer()
    {
#if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            int recommended = getRecommendedBufferSize();
            if (recommended <= 0) return;

            juce::AudioDeviceManager::AudioDeviceSetup setup;
            holder->deviceManager.getAudioDeviceSetup(setup);
            setup.bufferSize = recommended;
            holder->deviceManager.setAudioDeviceSetup(setup, true);
            bufferApplied = true;
            btnApplyBuffer.setButtonText("APPLIED");
            btnApplyBuffer.setEnabled(false);
            runSystemScan();
            repaint();
        }
#endif
    }

    void paintLatencyStep(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        // Use-case description text (painted under the buttons)
        static const char* descriptions[] = {
            "Target: 1-6 ms. Ideal for stage monitoring and live rigs.\n"
            "Requires a stable ASIO driver and low buffer size.",
            "Target: 6-12 ms. Comfortable for playing along with tracks.\n"
            "Good balance of responsiveness and CPU headroom.",
            "Target: 12-30 ms. Prioritizes zero dropouts over latency.\n"
            "Best for tracking, mixing, and CPU-heavy sessions."
        };

        const int btnH = 56;
        const int gap = 10;
        const int btnW = juce::jmin(520, area.getWidth());
        const int cx = area.getCentreX() - btnW / 2;

        int uc = getSelectedUseCase();

        // Paint use-case description icons and details
        for (int i = 0; i < 3; ++i)
        {
            const bool sel = (i == uc);
            int y = area.getY() + i * (btnH + gap);

            // Icon area (left of button text)
            g.setColour(sel ? Nova::Colors::Accent.withAlpha(0.15f) : juce::Colours::transparentBlack);
            auto iconRect = juce::Rectangle<int>(cx + 8, y + 10, 36, 36);
            g.fillRoundedRectangle(iconRect.toFloat(), 8.0f);

            g.setColour(sel ? Nova::Colors::Accent : Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
            static const char* icons[] = { "\xe2\x9a\xa1", "\xf0\x9f\x8e\xb8", "\xf0\x9f\x8e\x9a" };
            g.drawText(juce::CharPointer_UTF8(icons[i]), iconRect, juce::Justification::centred);
        }

        if (uc < 0) return; // nothing selected yet

        // --- Dynamic recommendation panel ---
        const int panelY = area.getY() + 3 * (btnH + gap) + 8;
        const int panelH = 110;
        auto panelRect = juce::Rectangle<int>(cx, panelY, btnW, panelH);

        g.setColour(juce::Colour::fromString("ff0E1520"));
        g.fillRoundedRectangle(panelRect.toFloat(), 12.0f);
        g.setColour(Nova::Colors::Accent.withAlpha(0.3f));
        g.drawRoundedRectangle(panelRect.toFloat().reduced(0.5f), 12.0f, 1.0f);

        // Current vs recommended
        float currentLatencyMs = 0.0f;
        int currentBuf = 0;
        double sr = 0.0;
#if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            if (auto* dev = holder->deviceManager.getCurrentAudioDevice())
            {
                sr = dev->getCurrentSampleRate();
                currentBuf = dev->getCurrentBufferSizeSamples();
                currentLatencyMs = sr > 0 ? (float)(currentBuf / sr * 1000.0) : 0.0f;
            }
        }
#endif

        int recBuf = getRecommendedBufferSize();
        float recLatencyMs = (sr > 0 && recBuf > 0) ? (float)(recBuf / sr * 1000.0) : 0.0f;

        auto inner = panelRect.reduced(16, 10);

        g.setColour(Nova::Colors::Accent);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("YOUR SETUP vs RECOMMENDATION", inner.removeFromTop(16), juce::Justification::centredLeft);

        inner.removeFromTop(6);
        auto row1 = inner.removeFromTop(20);
        auto row2 = inner.removeFromTop(20);

        const int labelW = 140;
        const int valW = 120;

        // Current
        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Current:", row1.removeFromLeft(labelW), juce::Justification::centredLeft);
        g.setColour(Nova::Colors::Text.withAlpha(0.85f));
        g.drawText(juce::String(currentBuf) + " smp / " + juce::String(currentLatencyMs, 1) + " ms",
                   row1.removeFromLeft(valW), juce::Justification::centredLeft);

        // Latency color badge
        auto badgeCol1 = currentLatencyMs <= 6.0f ? Nova::Colors::Success
                       : currentLatencyMs <= 12.0f ? Nova::Colors::Accent
                       : Nova::Colors::Error;
        g.setColour(badgeCol1);
        g.fillRoundedRectangle(row1.removeFromLeft(8).toFloat().reduced(1.0f, 5.0f), 3.0f);

        // Recommended
        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Recommended:", row2.removeFromLeft(labelW), juce::Justification::centredLeft);
        g.setColour(Nova::Colors::Text);
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText(juce::String(recBuf) + " smp / " + juce::String(recLatencyMs, 1) + " ms",
                   row2.removeFromLeft(valW), juce::Justification::centredLeft);

        auto badgeCol2 = recLatencyMs <= 6.0f ? Nova::Colors::Success
                       : recLatencyMs <= 12.0f ? Nova::Colors::Accent
                       : Nova::Colors::Error;
        g.setColour(badgeCol2);
        g.fillRoundedRectangle(row2.removeFromLeft(8).toFloat().reduced(1.0f, 5.0f), 3.0f);

        // Description
        inner.removeFromTop(4);
        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        g.drawFittedText(descriptions[uc], inner, juce::Justification::centredLeft, 3);

        // Latency bar
        auto barArea = juce::Rectangle<int>(cx, panelY + panelH + 10, btnW, 24);
        paintLatencyBar(g, barArea, currentLatencyMs);

        // Show apply button if buffer differs
        const_cast<AudioSetupWizard*>(this)->btnApplyBuffer.setVisible(
            useCaseSelected && recBuf > 0 && recBuf != currentBuf && !bufferApplied);
    }

    void paintLatencyBar(juce::Graphics& g, juce::Rectangle<int> bounds, float latencyMs) const
    {
        g.setColour(juce::Colour::fromString("ff0A0F18"));
        g.fillRoundedRectangle(bounds.toFloat(), 6.0f);

        const float maxMs = 30.0f;
        const float norm = juce::jlimit(0.0f, 1.0f, latencyMs / maxMs);
        auto fill = bounds.toFloat().reduced(2.0f);
        fill.setWidth(fill.getWidth() * norm);

        juce::Colour col = latencyMs <= 6.0f ? Nova::Colors::Success
                         : latencyMs <= 12.0f ? Nova::Colors::Accent
                         : Nova::Colors::Error;

        juce::ColourGradient barGrad(col.withAlpha(0.9f), fill.getX(), fill.getY(),
                                     col.withAlpha(0.4f), fill.getRight(), fill.getY(), false);
        g.setGradientFill(barGrad);
        g.fillRoundedRectangle(fill, 4.0f);

        // Zone markers
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        struct Zone { float ms; const char* label; juce::Colour c; };
        const Zone zones[] = {
            { 3.0f,  "3ms",  Nova::Colors::Success },
            { 6.0f,  "6ms",  Nova::Colors::Success },
            { 10.0f, "10ms", Nova::Colors::Accent  },
            { 15.0f, "15ms", Nova::Colors::Accent  },
            { 20.0f, "20ms", Nova::Colors::Error   },
        };
        for (const auto& z : zones)
        {
            const float x = bounds.getX() + bounds.getWidth() * (z.ms / maxMs);
            g.setColour(z.c.withAlpha(0.35f));
            g.drawVerticalLine((int)x, (float)bounds.getY(), (float)bounds.getBottom());
            g.setColour(Nova::Colors::TextDim);
            g.drawText(z.label, (int)x - 14, bounds.getBottom() + 1, 28, 12, juce::Justification::centred);
        }
    }

    // =================================================================
    // STEP 3 — Calibration Page (interactive multi-phase)
    // =================================================================

    class CalibrationPage final : public juce::Component, private juce::Timer
    {
    public:
        explicit CalibrationPage(NOVAAudioProcessor& proc)
            : processor(proc)
        {
            styleButton(btnStart, "START NOISE MEASUREMENT");
            btnStart.setColour(juce::TextButton::buttonColourId, Nova::Colors::Accent.withAlpha(0.22f));
            btnStart.onClick = [this] { startNoisePhase(); };
            addAndMakeVisible(btnStart);

            styleButton(btnContinue, "CONTINUE TO SIGNAL TEST");
            btnContinue.setColour(juce::TextButton::buttonColourId, Nova::Colors::Accent.withAlpha(0.22f));
            btnContinue.onClick = [this] { startSignalPhase(); };
            btnContinue.setVisible(false);
            addAndMakeVisible(btnContinue);

            styleButton(btnRestart, "RESTART CALIBRATION");
            btnRestart.onClick = [this] { reset(); };
            btnRestart.setVisible(false);
            addAndMakeVisible(btnRestart);

            styleButton(btnApplyGate, "APPLY GATE");
            btnApplyGate.setColour(juce::TextButton::buttonColourId, Nova::Colors::Accent.withAlpha(0.22f));
            btnApplyGate.onClick = [this] { applyGate(); };
            btnApplyGate.setVisible(false);
            addAndMakeVisible(btnApplyGate);

            styleButton(btnApplyGain, "APPLY GAIN");
            btnApplyGain.setColour(juce::TextButton::buttonColourId, Nova::Colors::Accent.withAlpha(0.22f));
            btnApplyGain.onClick = [this] { applyGain(); };
            btnApplyGain.setVisible(false);
            addAndMakeVisible(btnApplyGain);
        }

        enum class Phase { Idle, MeasuringNoise, NoiseResult, MeasuringSignal, Done };

        bool isCalibrationDone() const { return phase == Phase::Done; }
        float getSuggestedGateDb() const { return suggestedGateDb; }
        float getSuggestedGainDb() const { return suggestedGainDb; }
        float getPeakDb() const { return juce::Decibels::gainToDecibels(peakHold, -60.0f); }
        float getNoiseFloorDb() const { return juce::Decibels::gainToDecibels(noiseFloor, -100.0f); }
        bool wasGateApplied() const { return gateApplied; }
        bool wasGainApplied() const { return gainApplied; }

        void reset()
        {
            stopTimer();
            phase = Phase::Idle;
            peakHold = 0.0f;
            noiseFloor = 1.0f;
            avgNoise = 0.0f;
            noiseSamples = 0;
            silenceFrames = 0;
            signalFrames = 0;
            progressNorm = 0.0f;
            smoothedLevel = 0.0f;
            suggestedGateDb = -100.0f;
            suggestedGainDb = 0.0f;
            gateApplied = false;
            gainApplied = false;

            btnStart.setVisible(true);
            btnStart.setEnabled(true);
            btnStart.setButtonText("START NOISE MEASUREMENT");
            btnContinue.setVisible(false);
            btnRestart.setVisible(false);
            btnApplyGate.setVisible(false);
            btnApplyGain.setVisible(false);

            // Notify parent wizard that calibration is not done
            if (auto* wizard = findParentComponentOfClass<AudioSetupWizard>())
                wizard->setNextEnabled(false);

            startTimerHz(30);
            repaint();
        }

        void stopCalibration() { stopTimer(); }

        void timerCallback() override
        {
            const float peak = processor.getInputPeak();
            smoothedLevel += (peak - smoothedLevel) * 0.3f;

            if (phase == Phase::MeasuringNoise)
            {
                if (peak < 0.005f)
                {
                    ++silenceFrames;
                    noiseFloor = juce::jmin(noiseFloor, juce::jmax(peak, 0.00001f));
                    avgNoise += peak;
                    ++noiseSamples;
                }
                else
                {
                    silenceFrames = juce::jmax(0, silenceFrames - 2);
                }

                progressNorm = juce::jlimit(0.0f, 1.0f, (float)silenceFrames / 60.0f);

                if (silenceFrames >= 60)
                {
                    if (noiseSamples > 0) avgNoise /= (float)noiseSamples;
                    suggestedGateDb = juce::Decibels::gainToDecibels(
                        juce::jmax(avgNoise, noiseFloor) * 5.0f, -100.0f);
                    suggestedGateDb = juce::jlimit(-100.0f, -20.0f, std::round(suggestedGateDb));

                    phase = Phase::NoiseResult;
                    progressNorm = 1.0f;
                    btnStart.setVisible(false);
                    btnContinue.setVisible(true);
                }
            }
            else if (phase == Phase::MeasuringSignal)
            {
                if (peak > 0.01f)
                {
                    peakHold = juce::jmax(peakHold, peak);
                    ++signalFrames;
                }
                progressNorm = juce::jlimit(0.0f, 1.0f, (float)signalFrames / 90.0f);

                if (signalFrames >= 90)
                {
                    phase = Phase::Done;
                    progressNorm = 1.0f;
                    btnContinue.setVisible(false);
                    btnRestart.setVisible(true);

                    // Compute gain recommendation
                    const float peakDb = juce::Decibels::gainToDecibels(peakHold, -60.0f);
                    const float targetPeakDb = -12.0f;
                    suggestedGainDb = juce::jlimit(-20.0f, 20.0f,
                        std::round(targetPeakDb - peakDb));

                    btnApplyGate.setVisible(true);
                    btnApplyGain.setVisible(std::abs(suggestedGainDb) > 1.0f);

                    // Enable NEXT in parent wizard
                    if (auto* wizard = findParentComponentOfClass<AudioSetupWizard>())
                        wizard->setNextEnabled(true);
                }
            }

            repaint();
        }

        void resized() override
        {
            auto area = getLocalBounds();
            auto bottom = area.removeFromBottom(50).reduced(6, 6);

            const int btnW = 200;
            const int btnH = 34;
            const int gap = 8;

            // Center buttons at bottom
            int totalBtnW = btnW * 2 + gap;
            int bx = bottom.getCentreX() - totalBtnW / 2;
            int by = bottom.getCentreY() - btnH / 2;

            btnStart.setBounds(bottom.getCentreX() - btnW / 2, by, btnW, btnH);
            btnContinue.setBounds(bottom.getCentreX() - btnW / 2, by, btnW, btnH);

            btnApplyGate.setBounds(bx, by, btnW, btnH);
            btnApplyGain.setBounds(bx + btnW + gap, by, btnW, btnH);
            btnRestart.setBounds(bottom.getRight() - 150, by, 140, btnH);
        }

        void paint(juce::Graphics& g) override
        {
            auto area = getLocalBounds().reduced(6, 0);

            // --- Phase header ---
            juce::String phaseLabel;
            juce::Colour phaseCol;
            juce::String instruction;

            switch (phase)
            {
                case Phase::Idle:
                    phaseLabel = "READY TO CALIBRATE";
                    phaseCol = Nova::Colors::TextDim;
                    instruction = "Click START below to begin. Make sure your guitar is connected\n"
                                  "and your audio interface is active. Keep quiet during noise measurement.";
                    break;
                case Phase::MeasuringNoise:
                    phaseLabel = "PHASE 1: MEASURING NOISE FLOOR";
                    phaseCol = Nova::Colors::Accent;
                    instruction = "Keep quiet and mute your guitar strings.\n"
                                  "NOVA is listening to your noise floor...";
                    break;
                case Phase::NoiseResult:
                    phaseLabel = "PHASE 1 COMPLETE";
                    phaseCol = Nova::Colors::Success;
                    instruction = "Noise floor measured at "
                        + juce::String(juce::Decibels::gainToDecibels(noiseFloor, -100.0f), 1)
                        + " dB. Click CONTINUE to measure your playing level.";
                    break;
                case Phase::MeasuringSignal:
                    phaseLabel = "PHASE 2: MEASURING SIGNAL LEVEL";
                    phaseCol = juce::Colour::fromString("ff60A5FA");
                    instruction = "Play your guitar at normal volume.\n"
                                  "Strum chords and play single notes for accurate measurement.";
                    break;
                case Phase::Done:
                    phaseLabel = "CALIBRATION COMPLETE";
                    phaseCol = Nova::Colors::Success;
                    instruction = buildResultText();
                    break;
            }

            // Phase label + progress
            auto headerArea = area.removeFromTop(20);
            g.setColour(phaseCol);
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawText(phaseLabel, headerArea, juce::Justification::centredLeft);

            // Progress bar
            if (phase == Phase::MeasuringNoise || phase == Phase::MeasuringSignal)
            {
                auto progBar = headerArea.removeFromRight(160).reduced(0, 5);
                g.setColour(juce::Colour::fromString("ff0A0F18"));
                g.fillRoundedRectangle(progBar.toFloat(), 4.0f);
                auto fillRect = progBar.toFloat().reduced(1.0f);
                fillRect.setWidth(fillRect.getWidth() * progressNorm);
                g.setColour(phaseCol);
                g.fillRoundedRectangle(fillRect, 3.0f);
            }

            area.removeFromTop(8);

            // Instruction text
            g.setColour(Nova::Colors::Text.withAlpha(0.9f));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            int instrH = (phase == Phase::Done) ? 64 : 40;
            g.drawFittedText(instruction, area.removeFromTop(instrH),
                             juce::Justification::topLeft, 4);

            area.removeFromTop(8);

            // === METERS ===
            auto meterArea = area.removeFromTop(56);

            // Input level meter
            paintPremiumMeter(g, meterArea.removeFromTop(24), smoothedLevel, "INPUT");
            meterArea.removeFromTop(8);

            // Noise floor meter (after noise is measured)
            if (phase != Phase::Idle && phase != Phase::MeasuringNoise)
                paintNoiseFloorMeter(g, meterArea.removeFromTop(24));

            // === RESULT CARDS ===
            if (phase == Phase::Done)
            {
                area.removeFromTop(10);
                paintResultCards(g, area.removeFromTop(76));
            }
        }

    private:
        void startNoisePhase()
        {
            phase = Phase::MeasuringNoise;
            silenceFrames = 0;
            noiseFloor = 1.0f;
            avgNoise = 0.0f;
            noiseSamples = 0;
            progressNorm = 0.0f;
            btnStart.setVisible(true);
            btnStart.setEnabled(false);
            btnStart.setButtonText("MEASURING...");
            repaint();
        }

        void startSignalPhase()
        {
            phase = Phase::MeasuringSignal;
            signalFrames = 0;
            peakHold = 0.0f;
            progressNorm = 0.0f;
            btnContinue.setVisible(false);
            repaint();
        }

        void applyGate()
        {
            if (auto* p = processor.getGlobalParameter(Nova::IDs::INPUT_GATE.toString()))
            {
                p->setValueNotifyingHost(p->convertTo0to1(suggestedGateDb));
                gateApplied = true;
                btnApplyGate.setButtonText("GATE APPLIED");
                btnApplyGate.setEnabled(false);
                repaint();
            }
        }

        void applyGain()
        {
            if (auto* p = processor.getGlobalParameter(Nova::IDs::INPUT_GAIN.toString()))
            {
                float currentGain = p->convertFrom0to1(p->getValue());
                float newGain = juce::jlimit(-60.0f, 24.0f, currentGain + suggestedGainDb);
                p->setValueNotifyingHost(p->convertTo0to1(newGain));
                gainApplied = true;
                btnApplyGain.setButtonText("GAIN APPLIED");
                btnApplyGain.setEnabled(false);
                repaint();
            }
        }

        static void styleButton(juce::TextButton& btn, const juce::String& text)
        {
            btn.setButtonText(text);
            btn.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff182131"));
            btn.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.88f));
        }

        juce::String buildResultText() const
        {
            const float peakDb = juce::Decibels::gainToDecibels(peakHold, -60.0f);
            juce::String text;

            if (peakDb > -3.0f)
                text = "Signal is very hot. Reduce your interface gain to avoid clipping.";
            else if (peakDb > -18.0f)
                text = "Healthy signal level. Good headroom and signal-to-noise ratio.";
            else if (peakDb > -30.0f)
                text = "Signal is quiet. We recommend boosting your input gain.";
            else
                text = "Very low signal. Check your cable and interface gain.";

            if (std::abs(suggestedGainDb) > 1.0f)
                text += "\nSuggested gain adjustment: " + juce::String(suggestedGainDb > 0 ? "+" : "")
                    + juce::String(suggestedGainDb, 0) + " dB to reach optimal level.";

            return text;
        }

        void paintPremiumMeter(juce::Graphics& g, juce::Rectangle<int> bounds,
                               float level, const char* label) const
        {
            const float peakDb = juce::Decibels::gainToDecibels(juce::jmax(level, 0.000001f), -60.0f);
            const float norm = juce::jlimit(0.0f, 1.0f, (peakDb + 60.0f) / 60.0f);

            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            auto labelArea = bounds.removeFromLeft(50);
            g.drawText(label, labelArea, juce::Justification::centredRight);
            bounds.removeFromLeft(8);

            g.setColour(juce::Colour::fromString("ff0A0F18"));
            g.fillRoundedRectangle(bounds.toFloat(), 5.0f);

            auto fillRect = bounds.toFloat().reduced(2.0f);
            const float fillW = fillRect.getWidth() * norm;
            const int segments = (int)(fillW / 3.0f);

            for (int i = 0; i < segments; ++i)
            {
                float segNorm = (float)i / (fillRect.getWidth() / 3.0f);
                juce::Colour segCol;
                if (segNorm > 0.92f)      segCol = Nova::Colors::Error;
                else if (segNorm > 0.75f)  segCol = juce::Colour::fromString("ffF59E0B");
                else                       segCol = Nova::Colors::Accent;

                float sx = fillRect.getX() + i * 3.0f;
                g.setColour(segCol.withAlpha(0.85f));
                g.fillRect(sx, fillRect.getY(), 2.0f, fillRect.getHeight());
            }

            g.setColour(Nova::Colors::Text.withAlpha(0.7f));
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            g.drawText(juce::String(peakDb, 1) + " dB",
                       bounds.removeFromRight(60), juce::Justification::centred);
        }

        void paintNoiseFloorMeter(juce::Graphics& g, juce::Rectangle<int> bounds) const
        {
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            auto labelArea = bounds.removeFromLeft(50);
            g.drawText("NOISE", labelArea, juce::Justification::centredRight);
            bounds.removeFromLeft(8);

            g.setColour(juce::Colour::fromString("ff0A0F18"));
            g.fillRoundedRectangle(bounds.toFloat(), 5.0f);

            const float nfDb = juce::Decibels::gainToDecibels(noiseFloor, -60.0f);
            const float nfNorm = juce::jlimit(0.0f, 1.0f, (nfDb + 60.0f) / 60.0f);
            auto inner = bounds.toFloat().reduced(2.0f);

            g.setColour(Nova::Colors::Error.withAlpha(0.15f));
            g.fillRect(inner.getX(), inner.getY(), inner.getWidth() * nfNorm, inner.getHeight());

            // Gate threshold marker
            if (suggestedGateDb > -99.0f)
            {
                const float gateNorm = juce::jlimit(0.0f, 1.0f, (suggestedGateDb + 60.0f) / 60.0f);
                const float gateX = inner.getX() + inner.getWidth() * gateNorm;
                g.setColour(Nova::Colors::Accent);
                g.drawVerticalLine((int)gateX, inner.getY() - 2.0f, inner.getBottom() + 2.0f);

                g.setColour(Nova::Colors::TextDim);
                g.setFont(juce::Font(juce::FontOptions(9.0f)));
                g.drawText("Gate: " + juce::String(suggestedGateDb, 0) + " dB",
                           (int)gateX + 4, (int)inner.getY() - 1, 80, (int)inner.getHeight() + 2,
                           juce::Justification::centredLeft);
            }

            g.drawText(juce::String(nfDb, 1) + " dB floor",
                       bounds.removeFromRight(80), juce::Justification::centred);
        }

        void paintResultCards(juce::Graphics& g, juce::Rectangle<int> area) const
        {
            const float peakDb = juce::Decibels::gainToDecibels(peakHold, -60.0f);
            const float nfDb = juce::Decibels::gainToDecibels(noiseFloor, -100.0f);
            const float snr = peakDb - nfDb;

            struct Card { const char* label; juce::String value; juce::Colour col; bool applied; };
            Card cards[] = {
                { "Peak Level",     juce::String(peakDb, 1) + " dB",
                  peakDb > -3.0f ? Nova::Colors::Error : peakDb > -18.0f ? Nova::Colors::Success : Nova::Colors::Accent, false },
                { "Noise Floor",    juce::String(nfDb, 1) + " dB",
                  nfDb < -60.0f ? Nova::Colors::Success : nfDb < -40.0f ? Nova::Colors::Accent : Nova::Colors::Error, false },
                { "SNR",            juce::String(snr, 0) + " dB",
                  snr > 50.0f ? Nova::Colors::Success : snr > 30.0f ? Nova::Colors::Accent : Nova::Colors::Error, false },
                { "Suggested Gate", juce::String(suggestedGateDb, 0) + " dB",
                  gateApplied ? Nova::Colors::Success : Nova::Colors::Accent, gateApplied },
            };

            const int gap = 8;
            const int cardW = (area.getWidth() - gap * 3) / 4;
            for (int i = 0; i < 4; ++i)
            {
                auto cardRect = juce::Rectangle<int>(
                    area.getX() + i * (cardW + gap), area.getY(), cardW, area.getHeight());

                g.setColour(juce::Colour::fromString("ff0E1520"));
                g.fillRoundedRectangle(cardRect.toFloat(), 10.0f);
                g.setColour(cards[i].col.withAlpha(0.3f));
                g.drawRoundedRectangle(cardRect.toFloat().reduced(0.5f), 10.0f, 1.0f);

                auto inner = cardRect.reduced(10, 8);
                g.setColour(Nova::Colors::TextDim);
                g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
                g.drawText(cards[i].label, inner.removeFromTop(14), juce::Justification::centredLeft);

                g.setColour(cards[i].col);
                g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
                g.drawText(cards[i].value, inner, juce::Justification::centredLeft);

                if (cards[i].applied)
                {
                    g.setColour(Nova::Colors::Success);
                    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
                    g.drawText("APPLIED", cardRect.reduced(6, 4), juce::Justification::topRight);
                }
            }
        }

        NOVAAudioProcessor& processor;
        Phase phase = Phase::Idle;
        float smoothedLevel = 0.0f;
        float peakHold = 0.0f;
        float noiseFloor = 1.0f;
        float avgNoise = 0.0f;
        int noiseSamples = 0;
        float suggestedGateDb = -100.0f;
        float suggestedGainDb = 0.0f;
        float progressNorm = 0.0f;
        int silenceFrames = 0;
        int signalFrames = 0;
        bool gateApplied = false;
        bool gainApplied = false;

        juce::TextButton btnStart;
        juce::TextButton btnContinue;
        juce::TextButton btnRestart;
        juce::TextButton btnApplyGate;
        juce::TextButton btnApplyGain;
    };

    // =================================================================
    // STEP 4 — Tuner Reference
    // =================================================================

    juce::TextButton tunerCards[3];
    juce::TextButton btnCustomHz;
    juce::Slider customHzSlider;
    bool customHzActive = false;

    float getSelectedTunerHz() const
    {
        if (customHzActive) return (float)customHzSlider.getValue();
        if (tunerCards[1].getToggleState()) return 442.0f;
        if (tunerCards[2].getToggleState()) return 432.0f;
        return 440.0f;
    }

    void paintTunerStep(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        static const char* descriptions[] = {
            "International standard.\nUsed worldwide in most contexts.",
            "European orchestral pitch.\nCommon in classical performance.",
            "Alternative tuning.\nPopular in experimental and healing music."
        };
        static const char* subtitles[] = { "Standard", "Orchestral", "Alternative" };

        const int cardW = juce::jmin(140, (area.getWidth() - 80) / 4);
        const int gap = 12;
        const int totalW = cardW * 3 + gap * 2 + gap + cardW;
        const int sx = area.getCentreX() - totalW / 2;
        const int cardBottom = area.getY() + 24 + 68;

        for (int i = 0; i < 3; ++i)
        {
            const int cardX = sx + i * (cardW + gap);
            const bool selected = tunerCards[i].getToggleState();

            g.setColour(selected ? Nova::Colors::Accent : Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText(subtitles[i], cardX, area.getY() + 10, cardW, 14, juce::Justification::centred);

            g.setColour(selected ? Nova::Colors::Text.withAlpha(0.85f) : Nova::Colors::TextDim.withAlpha(0.5f));
            g.setFont(juce::Font(juce::FontOptions(10.5f)));
            g.drawFittedText(descriptions[i], cardX, cardBottom + 8, cardW, 40,
                             juce::Justification::centredTop, 3);
        }

        // Custom Hz label
        {
            const int custX = sx + 3 * (cardW + gap);
            g.setColour(customHzActive ? Nova::Colors::Accent : Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText("Custom", custX, area.getY() + 10, cardW, 14, juce::Justification::centred);

            if (customHzActive)
            {
                g.setColour(Nova::Colors::Text.withAlpha(0.7f));
                g.setFont(juce::Font(juce::FontOptions(10.0f)));
                g.drawText("400 - 480 Hz range", custX, cardBottom + 8, cardW, 16, juce::Justification::centredTop);
            }
        }

        // Current pitch display
        float hz = getSelectedTunerHz();
        auto displayArea = area.withTrimmedTop(180).removeFromTop(60);

        g.setColour(Nova::Colors::Accent.withAlpha(0.08f));
        g.fillRoundedRectangle(displayArea.reduced(40, 0).toFloat(), 14.0f);

        g.setColour(Nova::Colors::Accent);
        g.setFont(juce::Font(juce::FontOptions(36.0f, juce::Font::bold)));
        g.drawText("A = " + juce::String(hz, customHzActive ? 1 : 0) + " Hz",
                   displayArea, juce::Justification::centred);
    }

    // =================================================================
    // STEP 5 — Summary
    // =================================================================

    struct SummaryItem
    {
        juce::String label;
        juce::String value;
        juce::Colour statusCol;
        bool done;
        int editStep = -1; // which step to jump to on EDIT
    };

    std::vector<SummaryItem> summaryItems;
    juce::TextButton btnApplyAll;
    static constexpr int kMaxEditButtons = 8;
    juce::TextButton editButtons[kMaxEditButtons];
    bool allSettingsApplied = false;

    void buildSummary()
    {
        summaryItems.clear();
        allSettingsApplied = false;
        runSystemScan();

        if (scan.deviceActive)
        {
            summaryItems.push_back({ "Audio Driver", scan.driverType, Nova::Colors::Success, true, 1 });
            summaryItems.push_back({ "Input Device", scan.inputDevice,
                scan.numInputs > 0 ? Nova::Colors::Success : Nova::Colors::Error, scan.numInputs > 0, 1 });
            summaryItems.push_back({ "Sample Rate", juce::String(scan.sampleRate, 0) + " Hz", Nova::Colors::Success, true, 1 });
            summaryItems.push_back({ "Buffer / Latency",
                juce::String(scan.bufferSize) + " smp / " + juce::String(scan.latencyMs, 1) + " ms",
                scan.latencyMs <= 12.0 ? Nova::Colors::Success : Nova::Colors::Accent, true, 2 });
        }
        else
        {
            summaryItems.push_back({ "Audio Device", "Managed by host", Nova::Colors::TextDim, true, -1 });
        }

        // Use case
        int uc = getSelectedUseCase();
        static const char* ucNames[] = { "Live Performance", "Practice & Jamming", "Recording & Mixing" };
        summaryItems.push_back({ "Use Case",
            uc >= 0 ? juce::String(ucNames[uc]) : "Not selected",
            uc >= 0 ? Nova::Colors::Success : Nova::Colors::TextDim,
            uc >= 0, 2 });

        // Calibration
        if (calibrationPage && calibrationPage->isCalibrationDone())
        {
            summaryItems.push_back({ "Peak Level",
                juce::String(calibrationPage->getPeakDb(), 1) + " dB",
                calibrationPage->getPeakDb() > -3.0f ? Nova::Colors::Error : Nova::Colors::Success, true, 3 });
            summaryItems.push_back({ "Gate Threshold",
                juce::String(calibrationPage->getSuggestedGateDb(), 0) + " dB"
                    + (calibrationPage->wasGateApplied() ? "  (applied)" : "  (pending)"),
                calibrationPage->wasGateApplied() ? Nova::Colors::Success : Nova::Colors::Accent,
                calibrationPage->wasGateApplied(), 3 });
            if (std::abs(calibrationPage->getSuggestedGainDb()) > 1.0f)
            {
                float sg = calibrationPage->getSuggestedGainDb();
                summaryItems.push_back({ "Input Gain Adj.",
                    (sg > 0 ? "+" : "") + juce::String(sg, 0) + " dB"
                        + (calibrationPage->wasGainApplied() ? "  (applied)" : "  (pending)"),
                    calibrationPage->wasGainApplied() ? Nova::Colors::Success : Nova::Colors::Accent,
                    calibrationPage->wasGainApplied(), 3 });
            }
        }
        else
        {
            summaryItems.push_back({ "Calibration", "Skipped", Nova::Colors::TextDim, false, 3 });
        }

        // Tuner
        float hz = getSelectedTunerHz();
        juce::String refText = "A = " + juce::String(hz, customHzActive ? 1 : 0) + " Hz";
        if (!customHzActive)
        {
            if (hz == 442.0f)     refText += " (Orchestral)";
            else if (hz == 432.0f) refText += " (Alternative)";
            else                   refText += " (Standard)";
        }
        else refText += " (Custom)";

        summaryItems.push_back({ "Tuner Reference", refText, Nova::Colors::Accent, true, 4 });

        // Setup edit buttons
        for (int i = 0; i < kMaxEditButtons; ++i)
            editButtons[i].setVisible(false);

        for (int i = 0; i < juce::jmin((int)summaryItems.size(), kMaxEditButtons); ++i)
        {
            if (summaryItems[i].editStep >= 0)
            {
                editButtons[i].setVisible(true);
                const int targetStep = summaryItems[i].editStep;
                editButtons[i].onClick = [this, targetStep] { setStep(targetStep); };
            }
        }

        resized();
    }

    void paintSummary(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        const int cardW = juce::jmin(600, area.getWidth() - 20);
        const int cx = area.getCentreX() - cardW / 2;
        int y = area.getY();

        for (int i = 0; i < (int)summaryItems.size(); ++i)
        {
            const auto& item = summaryItems[i];
            const int rowH = 30;
            auto rowRect = juce::Rectangle<int>(cx, y, cardW, rowH);

            if (i % 2 == 0)
            {
                g.setColour(juce::Colour::fromString("ff0E1520"));
                g.fillRoundedRectangle(rowRect.toFloat(), 6.0f);
            }

            // Status dot
            g.setColour(item.statusCol);
            if (item.done)
                g.fillEllipse((float)(cx + 10), (float)(y + rowH / 2 - 4), 8.0f, 8.0f);
            else
                g.drawEllipse((float)(cx + 10), (float)(y + rowH / 2 - 4), 8.0f, 8.0f, 1.5f);

            // Label
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawText(item.label, cx + 28, y, 150, rowH, juce::Justification::centredLeft);

            // Value
            g.setColour(Nova::Colors::Text.withAlpha(0.9f));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText(item.value, cx + 180, y, cardW - 250, rowH, juce::Justification::centredLeft);

            // Position EDIT button
            if (i < kMaxEditButtons && item.editStep >= 0)
            {
                const_cast<AudioSetupWizard*>(this)->editButtons[i].setBounds(
                    cx + cardW - 60, y + 2, 50, rowH - 4);
            }

            y += rowH;
        }

        // Applied badge
        if (allSettingsApplied)
        {
            y += 12;
            g.setColour(Nova::Colors::Success);
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText("All settings applied successfully.",
                       cx, y, cardW, 24, juce::Justification::centred);
        }
        else
        {
            y += 12;
            g.setColour(Nova::Colors::Accent);
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText("Click APPLY ALL or FINISH to save your settings.",
                       cx, y, cardW, 24, juce::Justification::centred);
        }
    }

    // =================================================================
    // Apply All Settings
    // =================================================================

    void applyAllSettings()
    {
        // Tuner reference
        float hz = getSelectedTunerHz();
        juce::String refKey = "A = " + juce::String((int)hz) + " Hz";
        EditorPrefs::setString(EditorPrefs::tunerReferenceKey, refKey);
        processor.getAudioEngine().setTunerReferencePitch(hz);

        // Gate from calibration
        if (calibrationPage && calibrationPage->isCalibrationDone() && !calibrationPage->wasGateApplied())
        {
            if (auto* p = processor.getGlobalParameter(Nova::IDs::INPUT_GATE.toString()))
            {
                p->setValueNotifyingHost(p->convertTo0to1(calibrationPage->getSuggestedGateDb()));
            }
        }

        // Gain from calibration
        if (calibrationPage && calibrationPage->isCalibrationDone() && !calibrationPage->wasGainApplied())
        {
            float gainAdj = calibrationPage->getSuggestedGainDb();
            if (std::abs(gainAdj) > 1.0f)
            {
                if (auto* p = processor.getGlobalParameter(Nova::IDs::INPUT_GAIN.toString()))
                {
                    float current = p->convertFrom0to1(p->getValue());
                    float newGain = juce::jlimit(-60.0f, 24.0f, current + gainAdj);
                    p->setValueNotifyingHost(p->convertTo0to1(newGain));
                }
            }
        }

        allSettingsApplied = true;
        repaint();
    }

    // =================================================================
    // Timer
    // =================================================================

    void timerCallback() override { repaint(); }

    // =================================================================
    // Look and Feel for device selector
    // =================================================================

    class SelectorLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        SelectorLookAndFeel()
        {
            setColour(juce::ResizableWindow::backgroundColourId, juce::Colours::transparentBlack);
            setColour(juce::Label::textColourId, Nova::Colors::Text);
            setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromString("ff101722"));
            setColour(juce::ComboBox::textColourId, Nova::Colors::Text);
            setColour(juce::ComboBox::outlineColourId, Nova::Colors::Border.withAlpha(0.7f));
            setColour(juce::ComboBox::arrowColourId, Nova::Colors::Accent);
            setColour(juce::ListBox::backgroundColourId, juce::Colour::fromString("ff101722"));
            setColour(juce::PopupMenu::backgroundColourId, juce::Colour::fromString("ff101722"));
            setColour(juce::PopupMenu::textColourId, Nova::Colors::Text);
            setColour(juce::PopupMenu::highlightedBackgroundColourId, Nova::Colors::Accent.withAlpha(0.18f));
            setColour(juce::PopupMenu::highlightedTextColourId, Nova::Colors::Text);
            setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff182131"));
            setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.88f));
            setColour(juce::ToggleButton::tickColourId, Nova::Colors::Accent);
            setColour(juce::Slider::trackColourId, Nova::Colors::Accent);
            setColour(juce::ScrollBar::thumbColourId, Nova::Colors::Accent.withAlpha(0.55f));
        }
    };

    // =================================================================
    // Members
    // =================================================================

    NOVAAudioProcessor& processor;
    SelectorLookAndFeel selectorLnf;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;

    // Step 3
    CalibrationPage* calibrationPage = nullptr;

    // Best configuration this machine can run, recomputed when step 0 opens.
    Nova::Audio::AudioAutoConfig::Recommendation recommendation;
    juce::String applyStatus;
    juce::TextButton btnApplyBest;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioSetupWizard)
};
