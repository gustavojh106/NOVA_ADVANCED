#pragma once

// =============================================================================
// Audio panel + header status chip.
//
// One place for everything audio: the chip in the editor header shows the device,
// rate, buffer and total latency at a glance (colour = health) and opens the panel.
// The panel shows latency, guitar input level and input monitoring, the Latency
// Autopilot recommendation (Apply / Undo), and only the settings a guitarist needs:
// driver, device, guitar input, sample rate and buffer (with "Find lowest stable").
// Output channels are always the device's first pair; MIDI is not used.
//
// Included by PluginEditor.cpp after the settings helpers it reuses
// (SettingsLookAndFeel, styleSettingsActionButton). Message thread only.
// =============================================================================

#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
 #include "../../Core/Audio/StandaloneAudioAutopilot.h"
#endif
#include "../../Core/Audio/AudioAutopilot.h"

namespace NovaAudioUi
{
    inline const juce::Colour kWarningColour = juce::Colour::fromString("ffFBBF24");

    enum class Health { Good, Warning, Problem };

    struct Status
    {
        bool standalone = false;   // false: the DAW owns the device
        bool deviceOpen = false;
        juce::String typeName;
        juce::String inputName;
        juce::String outputName;
        double sampleRate = 0.0;
        int bufferSize = 0;
        double latencyMs = 0.0;    // standalone: driver round trip + NOVA; plugin: NOVA only
        bool inputMuted = false;
    };

    inline Status captureStatus(NOVAAudioProcessor& processor)
    {
        Status status;

       #if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            status.standalone = true;
            status.inputMuted = (bool) holder->getMuteInputValue().getValue();
            auto& deviceManager = holder->deviceManager;

            if (auto* device = deviceManager.getCurrentAudioDevice(); device != nullptr && device->isOpen())
            {
                const auto setup = deviceManager.getAudioDeviceSetup();
                status.deviceOpen = true;
                status.typeName = device->getTypeName();
                status.inputName = setup.inputDeviceName;
                status.outputName = setup.outputDeviceName;
                status.sampleRate = device->getCurrentSampleRate();
                status.bufferSize = device->getCurrentBufferSizeSamples();

                // Same estimate as the Latency Autopilot: driver-reported in + out
                // (both include the buffers) plus NOVA's own latency.
                int driverSamples = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
                if (driverSamples <= 0)
                    driverSamples = 2 * status.bufferSize;
                if (status.sampleRate > 0.0)
                    status.latencyMs = 1000.0 * (driverSamples + juce::jmax(0, processor.getLatencySamples())) / status.sampleRate;
            }

            return status;
        }
       #endif

        status.sampleRate = processor.getSampleRate();
        status.bufferSize = processor.getBlockSize();
        status.deviceOpen = status.sampleRate > 0.0;
        if (status.sampleRate > 0.0)
            status.latencyMs = 1000.0 * juce::jmax(0, processor.getLatencySamples()) / status.sampleRate;
        return status;
    }

    inline Health classify(const Status& status)
    {
        if (! status.deviceOpen)
            return Health::Problem;
        if (status.standalone && (status.inputMuted || status.latencyMs >= 25.0))
            return Health::Warning;
        return Health::Good;
    }

    inline juce::Colour colourFor(Health health)
    {
        switch (health)
        {
            case Health::Good:    return Nova::Colors::Success;
            case Health::Warning: return kWarningColour;
            case Health::Problem: return Nova::Colors::Error;
        }
        return Nova::Colors::TextDim;
    }

    inline juce::String latencyVerdict(double ms)
    {
        if (ms <= 0.0)  return "No audio device";
        if (ms < 10.0)  return "Excellent for playing";
        if (ms < 16.0)  return "Good for playing";
        if (ms < 25.0)  return "Playable";
        return "High. Try ASIO or a smaller buffer";
    }

    inline juce::Colour latencyColour(double ms)
    {
        if (ms <= 0.0)  return Nova::Colors::TextDim;
        if (ms < 16.0)  return Nova::Colors::Success;
        if (ms < 25.0)  return kWarningColour;
        return Nova::Colors::Error;
    }

    // "Speakers (Steinberg UR12 )" -> "Steinberg UR12"; long names are shortened.
    inline juce::String shortDeviceName(const juce::String& name, int maxChars = 22)
    {
        auto shortName = name.trim();
        if (shortName.endsWithChar(')') && shortName.containsChar('('))
            shortName = shortName.fromFirstOccurrenceOf("(", false, false).upToLastOccurrenceOf(")", false, false).trim();

        if (shortName.length() > maxChars)
            shortName = shortName.substring(0, maxChars - 1).trimEnd() + juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"));

        return shortName;
    }

    inline juce::String formatRate(double sampleRate)
    {
        const double khz = sampleRate / 1000.0;
        return (std::abs(khz - std::round(khz)) < 0.05 ? juce::String((int) std::round(khz)) : juce::String(khz, 1)) + " kHz";
    }

    inline const juce::String& dot()
    {
        static const juce::String separator(juce::CharPointer_UTF8("  \xc2\xb7  "));
        return separator;
    }

    // =========================================================================
    // Header chip
    // =========================================================================
    class AudioStatusChip final : public juce::Component,
                                  public juce::SettableTooltipClient
    {
    public:
        AudioStatusChip()
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            setTooltip("Audio: device, latency and input level");
            setWantsKeyboardFocus(false);
        }

        std::function<void()> onClick;
        std::function<void()> onWidthChanged; // the text got longer or shorter: re-run the header layout

        void setStatus(const Status& status)
        {
            const auto newHealth = classify(status);
            juce::String route, details;

            if (! status.deviceOpen)
            {
                details = status.standalone ? "No audio device" : "Audio";
            }
            else if (! status.standalone)
            {
                route = "DAW audio";
                details = "NOVA " + juce::String(status.latencyMs, 1) + " ms";
            }
            else
            {
                const auto in = shortDeviceName(status.inputName, 14);
                const auto out = shortDeviceName(status.outputName, 14);
                route = (in.isNotEmpty() && out.isNotEmpty() && in != out) ? in + " > " + out : (out.isNotEmpty() ? out : in);
                details = formatRate(status.sampleRate) + dot() + juce::String(status.bufferSize) + dot()
                        + juce::String(status.latencyMs, 1) + " ms";
                if (status.inputMuted)
                    details << dot() << "input muted";
            }

            if (newHealth != health || route != routeText || details != detailText)
            {
                health = newHealth;
                routeText = route;
                detailText = details;
                const int previousIdeal = idealWidth;
                const int previousCompact = compactWidth;
                const juce::Font font(juce::FontOptions(12.0f));
                const auto full = routeText.isNotEmpty() ? routeText + dot() + detailText : detailText;
                idealWidth = 38 + (int) std::ceil(juce::GlyphArrangement::getStringWidth(font, full));
                compactWidth = 38 + (int) std::ceil(juce::GlyphArrangement::getStringWidth(font, detailText));
                repaint();

                if ((idealWidth != previousIdeal || compactWidth != previousCompact) && onWidthChanged != nullptr)
                    onWidthChanged();
            }
        }

        // Width for the whole text, and for the short form without the device names.
        int getIdealWidth() const noexcept { return idealWidth; }
        int getCompactWidth() const noexcept { return compactWidth; }

        // Fits the chip to the room: whole text, else the short form, else what's left.
        int widthFor(int available) const noexcept
        {
            if (available >= idealWidth)   return idealWidth;
            if (available >= compactWidth) return compactWidth;
            return available;
        }

        void paint(juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat().reduced(0.5f);
            const auto colour = colourFor(health);

            g.setColour(juce::Colour::fromString("ff1A2332").brighter(hovered ? 0.12f : 0.0f));
            g.fillRoundedRectangle(bounds, 8.0f);
            g.setColour(colour.withAlpha(hovered ? 0.85f : 0.55f));
            g.drawRoundedRectangle(bounds, 8.0f, 1.0f);

            auto area = getLocalBounds().reduced(10, 0);
            auto dotArea = area.removeFromLeft(10).toFloat();
            g.setColour(colour);
            g.fillEllipse(dotArea.withSizeKeepingCentre(8.0f, 8.0f));
            area.removeFromLeft(8);

            const juce::Font font(juce::FontOptions(12.0f));
            g.setFont(font);

            const auto fits = [&font, &area](const juce::String& text)
            {
                return juce::GlyphArrangement::getStringWidth(font, text) <= (float) area.getWidth();
            };

            const auto full = routeText.isNotEmpty() ? routeText + dot() + detailText : detailText;
            g.setColour(Nova::Colors::Text.withAlpha(0.92f));

            if (fits(full))
                g.drawText(full, area, juce::Justification::centredLeft, false);
            else if (fits(detailText))
                g.drawText(detailText, area, juce::Justification::centredLeft, false);
            else
                g.drawFittedText(detailText, area, juce::Justification::centredLeft, 1, 0.7f);
        }

        void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
        void mouseExit(const juce::MouseEvent&) override  { hovered = false; repaint(); }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (getLocalBounds().contains(e.getPosition()) && onClick != nullptr)
                onClick();
        }

    private:
        Health health = Health::Problem;
        juce::String routeText;
        juce::String detailText;
        int idealWidth = 160;
        int compactWidth = 160;
        bool hovered = false;
    };

    // =========================================================================
    // Panel
    // =========================================================================
    class AudioPanel final : public juce::Component,
                             private juce::ChangeListener,
                             private juce::Timer
    {
    public:
       #if JucePlugin_Build_Standalone
        AudioPanel(NOVAAudioProcessor& processorToUse, Nova::StandaloneAudioAutopilot* autopilotToUse, std::function<void()> onCloseFn)
            : processor(processorToUse), autopilot(autopilotToUse), onClose(std::move(onCloseFn))
       #else
        AudioPanel(NOVAAudioProcessor& processorToUse, std::function<void()> onCloseFn)
            : processor(processorToUse), onClose(std::move(onCloseFn))
       #endif
        {
            setLookAndFeel(&lookAndFeel);
            setWantsKeyboardFocus(true);

            addAndMakeVisible(closeButton);
            styleSettingsActionButton(closeButton, "Close", true);
            closeButton.onClick = [this] { close(); };

           #if JucePlugin_Build_Standalone
            holder = juce::StandalonePluginHolder::getInstance();
           #endif

           #if JucePlugin_Build_Standalone
            if (holder != nullptr)
            {
                for (auto* box : { &driverBox, &deviceBox, &inputDeviceBox, &outputDeviceBox, &guitarInputBox, &rateBox, &bufferBox })
                    addChildComponent(*box);

                driverBox.onChange = [this] { onDriverChanged(); };
                deviceBox.onChange = [this] { onDeviceChanged(deviceBox, true, true); };
                inputDeviceBox.onChange = [this] { onDeviceChanged(inputDeviceBox, true, false); };
                outputDeviceBox.onChange = [this] { onDeviceChanged(outputDeviceBox, false, true); };
                guitarInputBox.onChange = [this] { onGuitarInputChanged(); };
                rateBox.onChange = [this] { applySetupChange([this](auto& setup) { setup.sampleRate = rateBox.getSelectedId(); }); };
                bufferBox.onChange = [this] { applySetupChange([this](auto& setup) { setup.bufferSize = bufferBox.getSelectedId(); }); };
                guitarInputBox.setTooltip("The interface input your guitar is plugged into");

                addChildComponent(findBufferButton);
                styleSettingsActionButton(findBufferButton, "Find lowest stable", true);
                findBufferButton.setTooltip("Tests buffer sizes from the smallest and keeps the first that runs without dropouts. Output is muted for 5-25 s.");
                findBufferButton.onClick = [this] { onFindBufferClicked(); };

                addChildComponent(driverPanelButton);
                styleSettingsActionButton(driverPanelButton, "Driver panel", true);
                driverPanelButton.setTooltip("Open the driver's own settings (some ASIO drivers set the buffer only there)");
                driverPanelButton.onClick = [this] { onDriverPanelClicked(); };

                addChildComponent(monitorButton);
                monitorButton.onClick = [this] { onMonitorClicked(); };

                addChildComponent(applyButton);
                styleSettingsActionButton(applyButton, "Apply");
                applyButton.onClick = [this] { onApplyClicked(); };

                addChildComponent(undoButton);
                styleSettingsActionButton(undoButton, "Undo", true);
                undoButton.onClick = [this] { onUndoClicked(); };

                holder->deviceManager.addChangeListener(this);
                rebuildDeviceControls();
                refreshRecommendation();
            }
           #endif

            refreshStatus();
            startTimerHz(20);
        }

        ~AudioPanel() override
        {
            stopTimer();
           #if JucePlugin_Build_Standalone
            if (holder != nullptr)
                holder->deviceManager.removeChangeListener(this);
           #endif
            setLookAndFeel(nullptr);
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::escapeKey)
            {
                close();
                return true;
            }
            return false;
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (! panelBounds.contains(e.getPosition()))
                close();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colours::black.withAlpha(0.78f));

            const auto pf = panelBounds.toFloat();
            juce::ColourGradient fill(juce::Colour::fromString("ff121A27"), pf.getCentreX(), pf.getY(),
                                      juce::Colour::fromString("ff0B0E14"), pf.getCentreX(), pf.getBottom(), false);
            g.setGradientFill(fill);
            g.fillRoundedRectangle(pf, 18.0f);
            g.setColour(Nova::Colors::Border.withAlpha(0.85f));
            g.drawRoundedRectangle(pf.reduced(0.5f), 18.0f, 1.0f);

            // Title
            auto title = titleBounds;
            g.setColour(Nova::Colors::Text);
            g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
            g.drawText("Audio", title.removeFromTop(26), juce::Justification::centredLeft);
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText(status.standalone ? "Your interface, latency and guitar input. Changes apply right away."
                                         : "Inside a DAW, the DAW chooses the audio device and buffer.",
                       title, juce::Justification::centredLeft);

            paintLatencyTile(g, latencyTile);
            paintInputTile(g, inputTile);
            if (! monitorTile.isEmpty())
                paintMonitorTile(g, monitorTile);

            if (! stripBounds.isEmpty())
            {
                const auto sf = stripBounds.toFloat();
                g.setColour(Nova::Colors::Accent.withAlpha(0.10f));
                g.fillRoundedRectangle(sf, 10.0f);
                g.setColour(Nova::Colors::Accent.withAlpha(0.35f));
                g.drawRoundedRectangle(sf.reduced(0.5f), 10.0f, 1.0f);
                g.setColour(Nova::Colors::Text.withAlpha(0.92f));
                g.setFont(juce::Font(juce::FontOptions(12.5f)));
                g.drawFittedText(stripText, stripTextBounds, juce::Justification::centredLeft, 2);
            }

            g.setFont(juce::Font(juce::FontOptions(12.5f)));
            for (const auto& row : rowLabels)
            {
                g.setColour(Nova::Colors::TextDim);
                g.drawText(row.text, row.bounds, juce::Justification::centredLeft);
            }

            if (hintText.isNotEmpty())
            {
                g.setColour(hintIsProblem ? kWarningColour : Nova::Colors::TextDim);
                g.setFont(juce::Font(juce::FontOptions(12.0f)));
                g.drawFittedText(hintText, hintBounds, juce::Justification::topLeft, 3);
            }
        }

        void resized() override
        {
            const bool separate = holder != nullptr && hasSeparateInputsAndOutputs();
            const int rowH = 32;
            const int rowGap = 10;
            const int numRows = holder != nullptr ? (separate ? 6 : 5) : 0;
            const bool showStrip = holder != nullptr && stripText.isNotEmpty();

            const int panelW = juce::jlimit(520, 700, getWidth() - 80);
            int panelH = 24 + 48 + 16 + 96 + 16;
            if (showStrip) panelH += 52 + 14;
            panelH += numRows * rowH + juce::jmax(0, numRows - 1) * rowGap;
            panelH += 6 + 36 + 22;
            panelH = juce::jmin(panelH, getHeight() - 40);
            panelBounds = juce::Rectangle<int>(0, 0, panelW, panelH).withCentre(getLocalBounds().getCentre());

            auto area = panelBounds.reduced(24);
            auto title = area.removeFromTop(48);
            closeButton.setBounds(title.removeFromRight(80).withSizeKeepingCentre(80, 30));
            titleBounds = title;
            area.removeFromTop(16);

            // Tiles
            auto tiles = area.removeFromTop(96);
            const int tileGap = 12;
            const bool showMonitor = holder != nullptr;
            const int numTiles = showMonitor ? 3 : 2;
            const int tileW = (tiles.getWidth() - tileGap * (numTiles - 1)) / numTiles;
            latencyTile = tiles.removeFromLeft(tileW);
            tiles.removeFromLeft(tileGap);
            inputTile = tiles.removeFromLeft(tileW);
            tiles.removeFromLeft(tileGap);
            monitorTile = showMonitor ? tiles : juce::Rectangle<int>();
            monitorButton.setVisible(showMonitor);
            if (showMonitor)
                monitorButton.setBounds(monitorTile.reduced(12).removeFromBottom(26).removeFromLeft(110));
            area.removeFromTop(16);

            // Recommendation strip
            stripBounds = {};
            stripTextBounds = {};
            if (showStrip)
            {
                stripBounds = area.removeFromTop(52);
                auto strip = stripBounds.reduced(14, 10);
                if (undoButton.isVisible())
                {
                    undoButton.setBounds(strip.removeFromRight(76));
                    strip.removeFromRight(8);
                }
                if (applyButton.isVisible())
                {
                    applyButton.setBounds(strip.removeFromRight(76));
                    strip.removeFromRight(8);
                }
                stripTextBounds = strip;
                area.removeFromTop(14);
            }

            // Settings grid
            rowLabels.clear();
            if (holder != nullptr)
            {
                const int labelW = 120;
                auto nextRow = [&](const juce::String& label)
                {
                    auto row = area.removeFromTop(rowH);
                    area.removeFromTop(rowGap);
                    rowLabels.push_back({ label, row.removeFromLeft(labelW) });
                    return row;
                };

                driverBox.setBounds(nextRow("Driver"));
                deviceBox.setVisible(! separate);
                inputDeviceBox.setVisible(separate);
                outputDeviceBox.setVisible(separate);
                if (separate)
                {
                    inputDeviceBox.setBounds(nextRow("Input device"));
                    outputDeviceBox.setBounds(nextRow("Output device"));
                }
                else
                {
                    deviceBox.setBounds(nextRow("Device"));
                }

                guitarInputBox.setBounds(nextRow("Guitar input"));
                rateBox.setBounds(nextRow("Sample rate"));

                auto bufferRow = nextRow("Buffer");
                if (driverPanelButton.isVisible())
                {
                    driverPanelButton.setBounds(bufferRow.removeFromRight(110));
                    bufferRow.removeFromRight(8);
                }
                findBufferButton.setBounds(bufferRow.removeFromRight(150));
                bufferRow.removeFromRight(8);
                bufferBox.setBounds(bufferRow);

                for (auto* box : { &driverBox, &guitarInputBox, &rateBox, &bufferBox })
                    box->setVisible(true);
                findBufferButton.setVisible(true);
            }

            area.removeFromTop(6);
            hintBounds = area.removeFromTop(36);
        }

    private:
        struct RowLabel
        {
            juce::String text;
            juce::Rectangle<int> bounds;
        };

        // --- painting ----------------------------------------------------------

        static void paintTileSurface(juce::Graphics& g, juce::Rectangle<int> tile)
        {
            g.setColour(juce::Colour::fromString("ff162131"));
            g.fillRoundedRectangle(tile.toFloat(), 12.0f);
            g.setColour(Nova::Colors::Border.withAlpha(0.6f));
            g.drawRoundedRectangle(tile.toFloat().reduced(0.5f), 12.0f, 1.0f);
        }

        static void paintTileLabel(juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& label)
        {
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(11.5f)));
            g.drawText(label, area.removeFromTop(16), juce::Justification::centredLeft);
        }

        void paintLatencyTile(juce::Graphics& g, juce::Rectangle<int> tile) const
        {
            paintTileSurface(g, tile);
            auto area = tile.reduced(12);
            paintTileLabel(g, area, status.standalone ? "Total latency" : "NOVA latency");

            g.setColour(Nova::Colors::Text);
            g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
            g.drawText(status.deviceOpen ? juce::String(status.latencyMs, 1) + " ms" : juce::String("--"),
                       area.removeFromTop(34), juce::Justification::centredLeft);

            g.setFont(juce::Font(juce::FontOptions(11.5f)));
            if (status.standalone)
            {
                g.setColour(latencyColour(status.deviceOpen ? status.latencyMs : 0.0));
                g.drawFittedText(latencyVerdict(status.deviceOpen ? status.latencyMs : 0.0), area, juce::Justification::topLeft, 2);
            }
            else
            {
                g.setColour(Nova::Colors::TextDim);
                g.drawFittedText("The DAW adds its own buffer", area, juce::Justification::topLeft, 2);
            }
        }

        void paintInputTile(juce::Graphics& g, juce::Rectangle<int> tile) const
        {
            paintTileSurface(g, tile);
            auto area = tile.reduced(12);
            paintTileLabel(g, area, "Guitar input level");
            area.removeFromTop(10);

            // Meter: -60..0 dBFS, green up to -6, amber to -1, red above.
            auto meter = area.removeFromTop(10).toFloat();
            g.setColour(juce::Colour::fromString("ff0B0E14"));
            g.fillRoundedRectangle(meter, 4.0f);

            const float db = juce::Decibels::gainToDecibels(displayPeak, -60.0f);
            const float fraction = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
            const auto level = clipHoldTicks > 0 ? Nova::Colors::Error
                             : (db > -6.0f ? kWarningColour : Nova::Colors::Success);
            if (fraction > 0.0f && ! inputMutedNow())
            {
                g.setColour(level);
                g.fillRoundedRectangle(meter.withWidth(meter.getWidth() * fraction), 4.0f);
            }
            area.removeFromTop(10);

            juce::String text;
            juce::Colour colour = Nova::Colors::TextDim;
            if (inputMutedNow())
                text = "Input muted";
            else if (clipHoldTicks > 0)
            {
                text = "Clipping. Lower the gain on your interface";
                colour = Nova::Colors::Error;
            }
            else if (db < -45.0f)
                text = "Play to check your level";
            else if (db < -30.0f)
            {
                text = "Low. Raise the gain on your interface";
                colour = kWarningColour;
            }
            else if (db <= -6.0f)
            {
                text = "Good level";
                colour = Nova::Colors::Success;
            }
            else
            {
                text = "Hot. Fine while it never clips";
                colour = kWarningColour;
            }

            g.setColour(colour);
            g.setFont(juce::Font(juce::FontOptions(11.5f)));
            g.drawFittedText(text, area, juce::Justification::topLeft, 2);
        }

        void paintMonitorTile(juce::Graphics& g, juce::Rectangle<int> tile) const
        {
            paintTileSurface(g, tile);
            auto area = tile.reduced(12);
            paintTileLabel(g, area, "Input monitoring");

            const bool muted = inputMutedNow();
            g.setColour(muted ? kWarningColour : Nova::Colors::Text);
            g.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
            g.drawText(muted ? "Muted" : "On", area.removeFromTop(28), juce::Justification::centredLeft);
        }

        // --- state ---------------------------------------------------------------

        bool inputMutedNow() const { return status.standalone && status.inputMuted; }

        bool hasSeparateInputsAndOutputs() const
        {
           #if JucePlugin_Build_Standalone
            if (holder == nullptr)
                return false;
            auto* type = holder->deviceManager.getCurrentDeviceTypeObject();
            return type != nullptr && type->hasSeparateInputsAndOutputs();
           #else
            return false;
           #endif
        }

        bool autopilotBusy() const
        {
           #if JucePlugin_Build_Standalone
            return autopilot != nullptr && autopilot->isBusy();
           #else
            return false;
           #endif
        }

        void refreshStatus()
        {
            status = captureStatus(processor);

            // Peak meter with a slow fall and a 2 s clip hold.
            const float peak = processor.getAudioEngine().getLastInputPeak();
            displayPeak = juce::jmax(peak, displayPeak * 0.90f);
            if (peak >= 0.99f)
                clipHoldTicks = 40;
            else if (clipHoldTicks > 0)
                --clipHoldTicks;

            monitorButton.setButtonText(status.inputMuted ? "Unmute" : "Mute");
            styleSettingsActionButton(monitorButton, status.inputMuted ? "Unmute" : "Mute", ! status.inputMuted);
        }

        void refreshRecommendation()
        {
           #if JucePlugin_Build_Standalone
            juce::String newStrip;
            bool showApply = false;
            bool showUndo = false;

            if (autopilot != nullptr)
            {
                using Kind = Nova::AudioAutopilot::RecommendationKind;
                using Activity = Nova::StandaloneAudioAutopilot::Activity;
                const bool busy = autopilot->isBusy();
                recommendation = autopilot->getRecommendation();
                const auto kind = recommendation.kind;
                const bool actionable = kind == Kind::SwitchDevice || kind == Kind::ChangeSampleRate;

                showApply = ! busy && actionable;
                showUndo = autopilot->canUndo();

                if (busy || autopilot->getActivity() != Activity::None)
                    newStrip = autopilot->getStatusMessage();
                else if (actionable)
                    newStrip = "Recommended: " + Nova::AudioAutopilot::describeCandidate(recommendation.candidate);
                else if (kind == Kind::AlreadyOptimal)
                    newStrip = "Your setup is the fastest NOVA found on this computer.";

                if (autopilot->getActivity() == Activity::Calibrating)
                    newStrip << "  " << juce::String(juce::roundToInt(autopilot->getCalibrationProgress() * 100.0f)) << "%";

                // Driver panel only helps when the buffer can't be set here.
                driverPanelButton.setVisible(autopilot->canOpenDriverPanel() && bufferBox.getNumItems() <= 1);
                styleSettingsActionButton(findBufferButton,
                    autopilot->getActivity() == Activity::Calibrating ? "Cancel" : "Find lowest stable", true);
            }

            const bool layoutChanged = newStrip.isEmpty() != stripText.isEmpty()
                || applyButton.isVisible() != showApply || undoButton.isVisible() != showUndo;
            stripText = newStrip;
            applyButton.setVisible(showApply);
            undoButton.setVisible(showUndo);

            for (auto* control : std::initializer_list<juce::Component*> { &driverBox, &deviceBox, &inputDeviceBox, &outputDeviceBox,
                                                                           &guitarInputBox, &rateBox, &bufferBox, &driverPanelButton })
                control->setEnabled(! autopilotBusy()); // manual changes would fight an apply check or a calibration

            if (layoutChanged)
                resized();
            repaint();
           #endif
        }

        void refreshHint()
        {
            hintIsProblem = false;
            hintText = {};

            if (holder == nullptr)
            {
                hintText = "To change the device or buffer, use your DAW's audio settings. NOVA's own latency is shown above.";
                return;
            }

            if (lastError.isNotEmpty())
            {
                hintText = lastError;
                hintIsProblem = true;
                return;
            }

            if (! status.deviceOpen)
            {
                hintText = "No audio device is open. Choose a driver and a device.";
                hintIsProblem = true;
                return;
            }

            const auto family = Nova::AudioAutopilot::classifyDriverType(status.typeName);
            if (family == Nova::AudioAutopilot::DriverFamily::WasapiShared || family == Nova::AudioAutopilot::DriverFamily::DirectSound)
            {
                hintText = "Windows shared mode can add latency the driver doesn't report. ASIO, or Windows Audio in low latency or exclusive mode, is usually faster.";
                hintIsProblem = true;
                return;
            }

            hintText = "Output always uses the device's first two channels.";
        }

        // --- device controls -----------------------------------------------------

        void rebuildDeviceControls()
        {
           #if JucePlugin_Build_Standalone
            if (holder == nullptr)
                return;

            auto& deviceManager = holder->deviceManager;
            const auto setup = deviceManager.getAudioDeviceSetup();
            auto* device = deviceManager.getCurrentAudioDevice();
            auto* type = deviceManager.getCurrentDeviceTypeObject();

            driverBox.clear(juce::dontSendNotification);
            const auto& types = deviceManager.getAvailableDeviceTypes();
            for (int i = 0; i < types.size(); ++i)
            {
                driverBox.addItem(Nova::AudioAutopilot::getFriendlyTypeName(types[i]->getTypeName()), i + 1);
                if (types[i]->getTypeName() == deviceManager.getCurrentAudioDeviceType())
                    driverBox.setSelectedId(i + 1, juce::dontSendNotification);
            }

            const auto fillNames = [](juce::ComboBox& box, const juce::StringArray& names, const juce::String& current)
            {
                box.clear(juce::dontSendNotification);
                for (int i = 0; i < names.size(); ++i)
                {
                    box.addItem(names[i], i + 1);
                    if (names[i] == current)
                        box.setSelectedId(i + 1, juce::dontSendNotification);
                }
                if (box.getSelectedId() == 0)
                    box.setTextWhenNothingSelected(current.isNotEmpty() ? current : juce::String("Choose a device"));
            };

            if (type != nullptr)
            {
                fillNames(deviceBox, type->getDeviceNames(false), setup.outputDeviceName);
                fillNames(inputDeviceBox, type->getDeviceNames(true), setup.inputDeviceName);
                fillNames(outputDeviceBox, type->getDeviceNames(false), setup.outputDeviceName);
            }

            guitarInputBox.clear(juce::dontSendNotification);
            rateBox.clear(juce::dontSendNotification);
            bufferBox.clear(juce::dontSendNotification);

            if (device != nullptr && device->isOpen())
            {
                const auto names = device->getInputChannelNames();
                const auto active = device->getActiveInputChannels();
                const int activeCount = active.countNumberOfSetBits();
                const int firstActive = active.findNextSetBit(0);

                for (int i = 0; i < names.size(); ++i)
                    guitarInputBox.addItem(juce::String(i + 1) + ": " + names[i], i + 1);
                for (int i = 0; i + 1 < names.size(); i += 2)
                    guitarInputBox.addItem("Inputs " + juce::String(i + 1) + " + " + juce::String(i + 2) + " (stereo)", 1000 + i);

                if (activeCount == 1)
                    guitarInputBox.setSelectedId(firstActive + 1, juce::dontSendNotification);
                else if (activeCount == 2 && firstActive % 2 == 0 && active[firstActive + 1])
                    guitarInputBox.setSelectedId(1000 + firstActive, juce::dontSendNotification);
                else
                    guitarInputBox.setTextWhenNothingSelected(names.isEmpty() ? "No inputs" : "Choose the input your guitar uses");

                for (double rate : device->getAvailableSampleRates())
                {
                    rateBox.addItem(formatRate(rate), (int) rate);
                    if (std::abs(rate - device->getCurrentSampleRate()) < 1.0)
                        rateBox.setSelectedId((int) rate, juce::dontSendNotification);
                }

                const double sampleRate = device->getCurrentSampleRate();
                for (int size : device->getAvailableBufferSizes())
                {
                    const auto ms = sampleRate > 0.0 ? juce::String(1000.0 * size / sampleRate, 1) + " ms" : juce::String();
                    bufferBox.addItem(juce::String(size) + " samples" + (ms.isNotEmpty() ? dot() + ms : juce::String()), size);
                    if (size == device->getCurrentBufferSizeSamples())
                        bufferBox.setSelectedId(size, juce::dontSendNotification);
                }
            }

            refreshStatus();
            refreshHint();
            resized();
            repaint();
           #endif
        }

        template <typename Change>
        void applySetupChange(Change&& change)
        {
           #if ! JucePlugin_Build_Standalone
            juce::ignoreUnused(change);
           #else
            if (holder == nullptr || autopilotBusy())
                return;

            auto setup = holder->deviceManager.getAudioDeviceSetup();
            change(setup);
            lastError = holder->deviceManager.setAudioDeviceSetup(setup, true);
            if (lastError.isNotEmpty())
                lastError = "Couldn't apply that: " + lastError.upToFirstOccurrenceOf("\n", false, false);
            rebuildDeviceControls();
           #endif
        }

        void onDriverChanged()
        {
           #if JucePlugin_Build_Standalone
            if (holder == nullptr || autopilotBusy())
                return;

            const auto& types = holder->deviceManager.getAvailableDeviceTypes();
            const int index = driverBox.getSelectedId() - 1;
            if (! juce::isPositiveAndBelow(index, types.size()))
                return;

            lastError = {};
            holder->deviceManager.setCurrentAudioDeviceType(types[index]->getTypeName(), true);
            rebuildDeviceControls();
           #endif
        }

        void onDeviceChanged(juce::ComboBox& box, bool setsInput, bool setsOutput)
        {
            const auto name = box.getText();
            applySetupChange([name, setsInput, setsOutput](auto& setup)
            {
                if (setsInput)
                {
                    setup.inputDeviceName = name;
                    setup.useDefaultInputChannels = true;
                }
                if (setsOutput)
                {
                    setup.outputDeviceName = name;
                    setup.useDefaultOutputChannels = true;
                }
            });
        }

        void onGuitarInputChanged()
        {
            const int id = guitarInputBox.getSelectedId();
            if (id <= 0)
                return;

            applySetupChange([id](auto& setup)
            {
                setup.useDefaultInputChannels = false;
                setup.inputChannels.clear();
                if (id >= 1000)
                    setup.inputChannels.setRange(id - 1000, 2, true);
                else
                    setup.inputChannels.setBit(id - 1);
            });
        }

        void onFindBufferClicked()
        {
           #if JucePlugin_Build_Standalone
            if (autopilot == nullptr)
                return;

            if (autopilot->getActivity() == Nova::StandaloneAudioAutopilot::Activity::Calibrating)
            {
                autopilot->cancelCalibration();
            }
            else if (const auto blocker = autopilot->getCalibrationBlocker(); blocker.isNotEmpty())
            {
                lastError = blocker;
                refreshHint();
                repaint();
                return;
            }
            else
            {
                lastError = {};
                autopilot->startCalibration();
            }

            refreshRecommendation();
           #endif
        }

        void onDriverPanelClicked()
        {
           #if JucePlugin_Build_Standalone
            if (autopilot != nullptr)
                autopilot->openDriverPanel();
            rebuildDeviceControls();
           #endif
        }

        void onMonitorClicked()
        {
           #if JucePlugin_Build_Standalone
            if (autopilot != nullptr)
                autopilot->setInputMutedByUser(! autopilot->isInputMuted());
            else if (holder != nullptr)
                holder->getMuteInputValue().setValue(! (bool) holder->getMuteInputValue().getValue());
            refreshStatus();
            repaint();
           #endif
        }

        void onApplyClicked()
        {
           #if JucePlugin_Build_Standalone
            if (autopilot != nullptr)
                autopilot->apply(recommendation.candidate);
            refreshRecommendation();
           #endif
        }

        void onUndoClicked()
        {
           #if JucePlugin_Build_Standalone
            if (autopilot != nullptr)
                autopilot->undo();
            rebuildDeviceControls();
            refreshRecommendation();
           #endif
        }

        void close()
        {
            if (onClose != nullptr)
                onClose();
        }

        // --- callbacks -----------------------------------------------------------

        void changeListenerCallback(juce::ChangeBroadcaster*) override
        {
            rebuildDeviceControls();
            refreshRecommendation();
        }

        void timerCallback() override
        {
            refreshStatus();

            // Recommendation and autopilot activity change slowly: twice a second.
            if (++slowTicks >= 10)
            {
                slowTicks = 0;
                refreshHint();
                if (holder != nullptr)
                    refreshRecommendation();
            }

            repaint(latencyTile.getUnion(inputTile).getUnion(monitorTile.isEmpty() ? inputTile : monitorTile));
        }

        NOVAAudioProcessor& processor;
       #if JucePlugin_Build_Standalone
        Nova::StandaloneAudioAutopilot* autopilot = nullptr;
        Nova::AudioAutopilot::Recommendation recommendation;
        juce::StandalonePluginHolder* holder = nullptr;
       #else
        juce::Component* holder = nullptr; // never set: plugin builds have no device controls
       #endif
        std::function<void()> onClose;
        SettingsLookAndFeel lookAndFeel;

        Status status;
        float displayPeak = 0.0f;
        int clipHoldTicks = 0;
        int slowTicks = 0;
        juce::String stripText;
        juce::String hintText;
        juce::String lastError;
        bool hintIsProblem = false;

        juce::Rectangle<int> panelBounds, titleBounds, latencyTile, inputTile, monitorTile;
        juce::Rectangle<int> stripBounds, stripTextBounds, hintBounds;
        std::vector<RowLabel> rowLabels;

        juce::TextButton closeButton;
        juce::ComboBox driverBox, deviceBox, inputDeviceBox, outputDeviceBox, guitarInputBox, rateBox, bufferBox;
        juce::TextButton findBufferButton, driverPanelButton, monitorButton, applyButton, undoButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPanel)
    };
}
