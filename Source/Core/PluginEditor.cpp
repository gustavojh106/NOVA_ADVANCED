#include "PluginEditor.h"

#include "PedalCatalog.h"
#include "PluginStateModel.h"
#include "../GUI/Widgets/ChainLane.h"
#include "../GUI/Widgets/AssetBrowserOverlay.h"
#include "../Effects/Pedals/Base/PedalUIFactory.h"
#include "../Effects/Pedals/Delay/DelayPedal.h"
#include "../Effects/Pedals/Overdrive/OverdriveThumbnail.h"
#include "../Effects/Pedals/Overdrive/OverdriveDashboard.h"
#include "../Effects/Pedals/Reverb/ReverbThumbnail.h"
#include "../Effects/Pedals/Reverb/ReverbDashboard.h"
#include <algorithm>

#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace
{
    namespace EditorPrefs
    {
        constexpr const char* showStatsKey = "editor.showPerformanceStats";
        constexpr const char* openBrowserOnStartupKey = "editor.openBrowserOnStartup";
        constexpr const char* openPresetsOnStartupKey = "editor.openPresetsOnStartup";
        constexpr const char* startupModeKey = "editor.startupMode";
        constexpr const char* confirmBeforeClearKey = "editor.confirmBeforeClear";
        constexpr const char* tunerReferenceKey = "editor.tunerReference";
        constexpr const char* showLatencyTipsKey = "editor.showLatencyTips";
        constexpr const char* libraryViewKey = "editor.libraryView";
        constexpr const char* favoritesFirstKey = "editor.libraryFavoritesFirst";
        constexpr const char* switcherShortcutKey = "editor.switcherShortcut";
        constexpr const char* switcherModesKey = "editor.switcherModes";
        constexpr const char* rootTag = "NOVA_EDITOR_SETTINGS";
        constexpr const char* itemTag = "SETTING";
        constexpr const char* nameAttr = "name";
        constexpr const char* valueAttr = "value";

        juce::File getSettingsFile()
        {
            auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                .getChildFile("NOVA");

            if (!directory.exists())
                directory.createDirectory();

            return directory.getChildFile("editor-settings.xml");
        }

        juce::StringPairArray& cache()
        {
            static bool loaded = false;
            static juce::StringPairArray values;

            if (!loaded)
            {
                const auto file = getSettingsFile();

                if (file.existsAsFile())
                {
                    auto xml = juce::parseXML(file);
                    if (xml != nullptr && xml->hasTagName(rootTag))
                    {
                        for (auto* child = xml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
                        {
                            if (!child->hasTagName(itemTag))
                                continue;
                            const auto key = child->getStringAttribute(nameAttr);
                            if (key.isNotEmpty())
                                values.set(key, child->getStringAttribute(valueAttr));
                        }
                    }
                }

                loaded = true;
            }

            return values;
        }

        void flushToDisk()
        {
            juce::XmlElement root(rootTag);
            const auto& values = cache();

            for (int i = 0; i < values.size(); ++i)
            {
                auto* child = root.createNewChildElement(itemTag);
                child->setAttribute(nameAttr, values.getAllKeys()[i]);
                child->setAttribute(valueAttr, values.getAllValues()[i]);
            }

            root.writeTo(getSettingsFile(), {});
        }

        bool getBool(const char* key, bool defaultValue)
        {
            const auto& values = cache();
            return values.containsKey(key)
                ? values[key].equalsIgnoreCase("true") || values[key] == "1"
                : defaultValue;
        }

        void setBool(const char* key, bool value)
        {
            cache().set(key, value ? "true" : "false");
            flushToDisk();
        }

        juce::String getString(const char* key, const juce::String& defaultValue)
        {
            const auto& values = cache();
            return values.containsKey(key) ? values[key] : defaultValue;
        }

        void setString(const char* key, const juce::String& value)
        {
            cache().set(key, value);
            flushToDisk();
        }

        float parseTunerReference()
        {
            const auto val = getString(tunerReferenceKey, "A = 440 Hz");
            if (val.contains("442")) return 442.0f;
            if (val.contains("432")) return 432.0f;
            return 440.0f;
        }

        // Switcher shortcut key — stored as keyCode, default=space
        int getSwitcherKeyCode()
        {
            const auto val = getString(switcherShortcutKey, "");
            if (val.isNotEmpty())
                return val.getIntValue();
            return juce::KeyPress::spaceKey;
        }

        void setSwitcherKeyCode(int keyCode)
        {
            setString(switcherShortcutKey, juce::String(keyCode));
        }

        juce::String keyCodeToDisplayName(int keyCode)
        {
            if (keyCode == juce::KeyPress::spaceKey)   return "SPACE";
            if (keyCode == juce::KeyPress::returnKey)   return "ENTER";
            if (keyCode == juce::KeyPress::tabKey)      return "TAB";
            if (keyCode == juce::KeyPress::escapeKey)   return "ESC";
            if (keyCode == juce::KeyPress::F1Key)       return "F1";
            if (keyCode == juce::KeyPress::F2Key)       return "F2";
            if (keyCode == juce::KeyPress::F3Key)       return "F3";
            if (keyCode == juce::KeyPress::F4Key)       return "F4";
            if (keyCode == juce::KeyPress::F5Key)       return "F5";
            if (keyCode == juce::KeyPress::F6Key)       return "F6";
            if (keyCode == juce::KeyPress::F7Key)       return "F7";
            if (keyCode == juce::KeyPress::F8Key)       return "F8";
            if (keyCode == juce::KeyPress::F9Key)       return "F9";
            if (keyCode == juce::KeyPress::F10Key)      return "F10";
            if (keyCode == juce::KeyPress::F11Key)      return "F11";
            if (keyCode == juce::KeyPress::F12Key)      return "F12";
            if (keyCode >= 'A' && keyCode <= 'Z')
                return juce::String::charToString((juce::juce_wchar)keyCode);
            if (keyCode >= '0' && keyCode <= '9')
                return juce::String::charToString((juce::juce_wchar)keyCode);
            return juce::KeyPress(keyCode, {}, 0).getTextDescription().toUpperCase();
        }

        // Enabled switcher modes — bitmask: bit0=LineA, bit1=Parallel, bit2=LineB
        // Default: all three enabled (7 = 0b111)
        int getSwitcherModes()
        {
            const auto val = getString(switcherModesKey, "7");
            int modes = val.getIntValue();
            // Must have at least 2 modes enabled
            int count = ((modes >> 0) & 1) + ((modes >> 1) & 1) + ((modes >> 2) & 1);
            if (count < 2)
                return 7;
            return modes & 7;
        }

        void setSwitcherModes(int bitmask)
        {
            setString(switcherModesKey, juce::String(bitmask & 7));
        }

        bool isModeEnabled(int bitmask, Nova::SwitcherMode mode)
        {
            return (bitmask >> (int)mode) & 1;
        }
    }

    juce::File getStartupPresetPointerFile()
    {
        auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("NOVA");

        if (!directory.exists())
            directory.createDirectory();

        return directory.getChildFile("startup-preset.txt");
    }

    juce::String getStartupPresetName()
    {
        const auto pointerFile = getStartupPresetPointerFile();
        if (!pointerFile.existsAsFile())
            return {};

        const auto presetFile = juce::File(pointerFile.loadFileAsString().trim());
        return presetFile.existsAsFile() ? presetFile.getFileNameWithoutExtension() : juce::String{};
    }

    juce::String sanitiseFactoryPresetStem(const juce::String& presetName)
    {
        auto safe = presetName.trim()
            .retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_");

        if (safe.isEmpty())
            safe = "Preset";

        return safe;
    }

    juce::ValueTree createFactoryDelayPresetState(int presetIndex)
    {
        DelayPedal delay;
        delay.applyFlagshipPreset(presetIndex);

        juce::MemoryBlock pedalState;
        delay.getStateInformation(pedalState);

        juce::ValueTree state(Nova::IDs::MAIN_STATE);
        Nova::PluginStateModel::resetToCleanState(state);

        if (auto settings = Nova::PluginStateModel::getSettingsTree(state); settings.isValid())
        {
            settings.setProperty(Nova::IDs::ENGINE_ON, true, nullptr);
            settings.setProperty(Nova::IDs::SWITCH_MODE, (int) Nova::SwitcherMode::LineA_Only, nullptr);
            settings.setProperty(Nova::IDs::OUTPUT_MIX, 100.0f, nullptr);
        }

        if (auto lineA = Nova::PluginStateModel::getLineTree(state, Nova::ChainID::LineA); lineA.isValid())
        {
            auto pedal = juce::ValueTree(Nova::IDs::PEDAL);
            pedal.setProperty(Nova::IDs::PEDAL_ID, "factory-delay-" + juce::String(presetIndex), nullptr);
            pedal.setProperty(Nova::IDs::PEDAL_TYPE, "Delay", nullptr);
            pedal.setProperty(Nova::IDs::PEDAL_ZONE, (int) Nova::ZoneID::FX, nullptr);
            pedal.setProperty(Nova::IDs::PEDAL_ENABLED, true, nullptr);

            if (pedalState.getSize() > 0)
            {
                pedal.setProperty(Nova::IDs::PEDAL_STATE,
                    juce::Base64::toBase64(pedalState.getData(), pedalState.getSize()),
                    nullptr);
            }

            lineA.appendChild(pedal, nullptr);
        }

        Nova::PluginStateModel::canonicalizeStateTree(state);
        return state;
    }

    void ensureBundledDelayPresets(const juce::File& presetDirectory)
    {
        if (!presetDirectory.exists())
            presetDirectory.createDirectory();

        for (int i = 0; i < DelayPedal::getNumFlagshipPresets(); ++i)
        {
            const auto presetName = "Factory - Orbit " + DelayPedal::getFlagshipPresetName(i);
            const auto presetFile = presetDirectory.getChildFile(sanitiseFactoryPresetStem(presetName) + ".nova-preset");

            if (presetFile.existsAsFile())
                continue;

            juce::MemoryOutputStream stream;
            createFactoryDelayPresetState(i).writeToStream(stream);
            presetFile.replaceWithData(stream.getData(), stream.getDataSize());
        }
    }

    struct PedalUIRegistrar
    {
        PedalUIRegistrar()
        {
            Nova::OverdriveUI::registerOverdriveThumbnail();
            Nova::OverdriveUI::registerOverdriveDashboard();
            Nova::ReverbUI::registerReverbThumbnail();
            Nova::ReverbUI::registerReverbDashboard();
        }
    };
    static PedalUIRegistrar sUIRegistrar;

    // ---- Multi-row adaptive layout for Pre / FX zones ----
    struct FlexLayoutResult
    {
        int rows = 1;
        int cols = 1;
        int cardW = 156;
        int cardH = 112;
        int gapH = 12;
        int gapV = 10;
        int maxCapacity = 1;
    };

    FlexLayoutResult calculateFlexLayout(int zoneW, int zoneH, int pedalCount)
    {
        struct TierDef { int cardW, cardH, gapH, gapV, padH, padV, minGapH; };

        static constexpr TierDef tiers[3] = {
            { 156, 112, 12, 10, 10, 8, 4 },   // Full
            { 130,  96, 10,  8,  8, 6, 3 },   // Compact
            { 110,  80,  8,  6,  6, 4, 2 },   // Mini
        };

        const int count = juce::jmax(1, juce::jmin(pedalCount, Nova::Config::MAX_PEDALS_PER_FLEX_ZONE));

        for (int t = 0; t < 3; ++t)
        {
            const auto& d = tiers[t];
            const int usableW = juce::jmax(1, zoneW - 2 * d.padH);
            const int usableH = juce::jmax(1, zoneH - 2 * d.padV);

            const int cols = juce::jmax(1, (usableW + d.gapH) / (d.cardW + d.gapH));
            const int maxRows = juce::jmax(1, (usableH + d.gapV) / (d.cardH + d.gapV));
            const int capacity = cols * maxRows;

            if (count <= capacity)
            {
                const int rows = (count + cols - 1) / cols;

                int actualGapH = d.gapH;
                if (cols > 1)
                {
                    actualGapH = (usableW - cols * d.cardW) / (cols - 1);
                    actualGapH = juce::jlimit(d.minGapH, d.gapH * 2, actualGapH);
                }

                int actualGapV = d.gapV;
                if (rows > 1)
                {
                    actualGapV = (usableH - rows * d.cardH) / (rows - 1);
                    actualGapV = juce::jlimit(2, d.gapV * 3, actualGapV);
                }

                return { rows, cols, d.cardW, d.cardH, actualGapH, actualGapV, capacity };
            }
        }

        // Fallback: tier 3 at max capacity
        const auto& d = tiers[2];
        const int usableW = juce::jmax(1, zoneW - 2 * d.padH);
        const int usableH = juce::jmax(1, zoneH - 2 * d.padV);
        const int cols = juce::jmax(1, (usableW + d.gapH) / (d.cardW + d.gapH));
        const int maxRows = juce::jmax(1, (usableH + d.gapV) / (d.cardH + d.gapV));
        return { maxRows, cols, d.cardW, d.cardH, d.gapH, d.gapV, cols * maxRows };
    }

    class SettingsLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        SettingsLookAndFeel()
        {
            setColour(juce::ResizableWindow::backgroundColourId, Nova::Colors::Panel);
            setColour(juce::Label::textColourId, Nova::Colors::Text);
            setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff182131"));
            setColour(juce::TextButton::buttonOnColourId, Nova::Colors::Accent.withAlpha(0.18f));
            setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.88f));
            setColour(juce::TextButton::textColourOnId, Nova::Colors::Text);
            setColour(juce::ToggleButton::tickColourId, Nova::Colors::Accent);
            setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromString("ff101722"));
            setColour(juce::ComboBox::textColourId, Nova::Colors::Text);
            setColour(juce::ComboBox::outlineColourId, Nova::Colors::Border.withAlpha(0.75f));
            setColour(juce::ComboBox::arrowColourId, Nova::Colors::Accent);
            setColour(juce::PopupMenu::backgroundColourId, juce::Colour::fromString("ff101722"));
            setColour(juce::PopupMenu::textColourId, Nova::Colors::Text);
            setColour(juce::PopupMenu::highlightedBackgroundColourId, Nova::Colors::Accent.withAlpha(0.18f));
            setColour(juce::PopupMenu::highlightedTextColourId, Nova::Colors::Text);
            setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff101722"));
            setColour(juce::TextEditor::textColourId, Nova::Colors::Text);
            setColour(juce::TextEditor::outlineColourId, Nova::Colors::Border.withAlpha(0.7f));
            setColour(juce::ListBox::backgroundColourId, juce::Colour::fromString("ff101722"));
            setColour(juce::ScrollBar::thumbColourId, Nova::Colors::Accent.withAlpha(0.55f));
            setColour(juce::Slider::trackColourId, Nova::Colors::Accent);
        }
    };

    void styleSettingsActionButton(juce::TextButton& button,
        const juce::String& text,
        bool secondary = false)
    {
        button.setButtonText(text);
        button.setColour(juce::TextButton::buttonColourId,
            secondary ? juce::Colour::fromString("ff172131") : Nova::Colors::Accent.withAlpha(0.18f));
        button.setColour(juce::TextButton::textColourOffId,
            secondary ? Nova::Colors::Text.withAlpha(0.85f) : Nova::Colors::Text);
    }

    void styleSettingsLabel(juce::Label& label,
        float fontSize,
        juce::Colour colour,
        juce::Justification justification = juce::Justification::topLeft)
    {
        label.setColour(juce::Label::textColourId, colour);
        label.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        label.setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
        label.setFont(juce::Font(juce::FontOptions(fontSize)));
        label.setJustificationType(justification);
        label.setInterceptsMouseClicks(false, false);
    }

    void styleSettingsTabButton(juce::TextButton& button,
        const juce::String& text)
    {
        button.setButtonText(text);
        button.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff121A27"));
        button.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.82f));
    }

    void setSettingsTabButtonActive(juce::TextButton& button, bool active)
    {
        button.setColour(juce::TextButton::buttonColourId,
            active ? Nova::Colors::Accent.withAlpha(0.22f) : juce::Colour::fromString("ff121A27"));
        button.setColour(juce::TextButton::textColourOffId,
            active ? Nova::Colors::Text : Nova::Colors::Text.withAlpha(0.82f));
    }

    void paintSettingsContentSurface(juce::Graphics& g,
        juce::Rectangle<int> bounds,
        const juce::String& title,
        const juce::String& subtitle)
    {
        auto rect = bounds.toFloat();
        juce::ColourGradient fill(juce::Colour::fromString("ff162131"),
            rect.getCentreX(), rect.getY(),
            juce::Colour::fromString("ff0F1724"),
            rect.getCentreX(), rect.getBottom(), false);
        g.setGradientFill(fill);
        g.fillRoundedRectangle(rect, 16.0f);

        g.setColour(Nova::Colors::Border.withAlpha(0.65f));
        g.drawRoundedRectangle(rect.reduced(0.5f), 16.0f, 1.0f);

        auto header = bounds.reduced(20, 18);
        g.setColour(Nova::Colors::Accent);
        g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        g.drawText(title, header.removeFromTop(22), juce::Justification::centredLeft);

        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawFittedText(subtitle, header.removeFromTop(30), juce::Justification::centredLeft, 2);
    }

    class PlaceholderSettingsPage final : public juce::Component
    {
    public:
        PlaceholderSettingsPage(juce::String pageTitle,
            juce::String description,
            juce::String footerText)
            : title(std::move(pageTitle)),
              body(std::move(description)),
              footer(std::move(footerText))
        {
        }

        void paint(juce::Graphics& g) override
        {
            auto area = getLocalBounds().reduced(10);
            const int gap = 14;
            const int topHeight = juce::jmax(140, (int)std::round((double)area.getHeight() * 0.44));
            auto hero = area.removeFromTop(topHeight);
            area.removeFromTop(gap);
            auto details = area;

            drawCard(g, hero, title, body,
                "This category is already wired into the NOVA settings architecture.");
            drawCard(g, details, "What Will Live Here", footer,
                "For now it acts as a visible placeholder instead of a dead end.");
        }

    private:
        static void drawCard(juce::Graphics& g,
            juce::Rectangle<int> bounds,
            const juce::String& heading,
            const juce::String& paragraph,
            const juce::String& note)
        {
            auto rect = bounds.toFloat();
            juce::ColourGradient fill(juce::Colour::fromString("ff172131"),
                rect.getCentreX(), rect.getY(),
                juce::Colour::fromString("ff0F1622"),
                rect.getCentreX(), rect.getBottom(), false);
            g.setGradientFill(fill);
            g.fillRoundedRectangle(rect, 18.0f);

            g.setColour(Nova::Colors::Border.withAlpha(0.65f));
            g.drawRoundedRectangle(rect.reduced(0.5f), 18.0f, 1.0f);

            auto content = bounds.reduced(24, 22);
            g.setColour(Nova::Colors::Accent);
            g.setFont(juce::Font(juce::FontOptions(16.5f, juce::Font::bold)));
            g.drawText(heading, content.removeFromTop(26), juce::Justification::centredLeft);

            content.removeFromTop(10);
            g.setColour(Nova::Colors::Text.withAlpha(0.86f));
            g.setFont(juce::Font(juce::FontOptions(13.0f)));
            const int paragraphHeight = juce::jmax(54, juce::jmin(88, bounds.getHeight() / 3));
            g.drawFittedText(paragraph, content.removeFromTop(paragraphHeight), juce::Justification::topLeft, 5);

            content.removeFromTop(12);
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::italic)));
            g.drawFittedText(note, content, juce::Justification::topLeft, 4);
        }

        juce::String title;
        juce::String body;
        juce::String footer;
    };

    class GeneralSettingsPage final : public juce::Component
    {
    public:
        struct Callbacks
        {
            std::function<void(bool)> onShowStatsChanged;
            std::function<void(bool)> onOpenBrowserOnStartupChanged;
            std::function<void(bool)> onOpenPresetsOnStartupChanged;
            std::function<void()> onSwitcherConfigChanged;
        };

        explicit GeneralSettingsPage(Callbacks pageCallbacks)
            : callbacks(std::move(pageCallbacks))
        {
            setLookAndFeel(&lookAndFeel);

            configureTabButton(btnInterfaceTab, "Interface", [this] { setTab(Tab::Interface); });
            configureTabButton(btnStartupTab, "Startup", [this] { setTab(Tab::Startup); });

            // --- Interface tab controls ---
            setupToggle(showStatsToggle, "Show performance stats");
            addAndMakeVisible(showStatsToggle);
            showStatsToggle.setTooltip("Show CPU, process time and buffer time in the header.");

            // Shortcut key recorder button
            addAndMakeVisible(btnShortcutKey);
            refreshShortcutKeyLabel();
            btnShortcutKey.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff101722"));
            btnShortcutKey.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.92f));
            btnShortcutKey.onClick = [this] { startRecordingKey(); };

            // Reset shortcut button
            addAndMakeVisible(btnResetShortcut);
            btnResetShortcut.setButtonText("RESET");
            btnResetShortcut.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff1A1420"));
            btnResetShortcut.setColour(juce::TextButton::textColourOffId, Nova::Colors::TextDim);
            btnResetShortcut.onClick = [this]
            {
                EditorPrefs::setSwitcherKeyCode(juce::KeyPress::spaceKey);
                recordingKey = false;
                refreshShortcutKeyLabel();
                repaint();
            };

            // Mode toggle chips
            switcherModes = EditorPrefs::getSwitcherModes();
            for (int i = 0; i < 3; ++i)
            {
                addAndMakeVisible(modeChips[i]);
                modeChips[i].setClickingTogglesState(true);
                modeChips[i].setToggleState((switcherModes >> i) & 1, juce::dontSendNotification);
                modeChips[i].onClick = [this, i] { onModeChipClicked(i); };
            }
            modeChips[0].setButtonText("LINE A");
            modeChips[1].setButtonText("PARALLEL");
            modeChips[2].setButtonText("LINE B");
            updateModeChipColors();

            // --- Startup tab controls ---
            setupToggle(openBrowserToggle, "Open Quick Add at startup");
            setupToggle(openPresetsToggle, "Open Presets at startup");
            setupToggle(confirmClearToggle, "Confirm before clearing rig");
            addAndMakeVisible(openBrowserToggle);
            addAndMakeVisible(openPresetsToggle);
            addAndMakeVisible(confirmClearToggle);

            openBrowserToggle.setTooltip("Open the Quick Add drawer automatically when the editor starts.");
            openPresetsToggle.setTooltip("Open the Presets drawer automatically when the editor starts.");
            confirmClearToggle.setTooltip("Ask before clearing the active rig and preset state.");

            startupModeBox.addItem("Remember last context", 1);
            startupModeBox.addItem("Open clean rig", 2);
            startupModeBox.addItem("Open startup preset", 3);
            addAndMakeVisible(startupModeBox);

            // Load state
            showStatsToggle.setToggleState(EditorPrefs::getBool(EditorPrefs::showStatsKey, true), juce::dontSendNotification);
            openBrowserToggle.setToggleState(EditorPrefs::getBool(EditorPrefs::openBrowserOnStartupKey, false), juce::dontSendNotification);
            openPresetsToggle.setToggleState(EditorPrefs::getBool(EditorPrefs::openPresetsOnStartupKey, false), juce::dontSendNotification);
            confirmClearToggle.setToggleState(EditorPrefs::getBool(EditorPrefs::confirmBeforeClearKey, true), juce::dontSendNotification);
            startupModeBox.setText(EditorPrefs::getString(EditorPrefs::startupModeKey, "Remember last context"),
                juce::dontSendNotification);

            // Wire callbacks
            showStatsToggle.onClick = [this]
            {
                const bool enabled = showStatsToggle.getToggleState();
                EditorPrefs::setBool(EditorPrefs::showStatsKey, enabled);
                if (callbacks.onShowStatsChanged)
                    callbacks.onShowStatsChanged(enabled);
            };

            openBrowserToggle.onClick = [this]
            {
                const bool enabled = openBrowserToggle.getToggleState();
                EditorPrefs::setBool(EditorPrefs::openBrowserOnStartupKey, enabled);
                if (callbacks.onOpenBrowserOnStartupChanged)
                    callbacks.onOpenBrowserOnStartupChanged(enabled);
            };

            openPresetsToggle.onClick = [this]
            {
                const bool enabled = openPresetsToggle.getToggleState();
                EditorPrefs::setBool(EditorPrefs::openPresetsOnStartupKey, enabled);
                if (callbacks.onOpenPresetsOnStartupChanged)
                    callbacks.onOpenPresetsOnStartupChanged(enabled);
            };

            confirmClearToggle.onClick = [this]
            {
                EditorPrefs::setBool(EditorPrefs::confirmBeforeClearKey, confirmClearToggle.getToggleState());
            };

            startupModeBox.onChange = [this]
            {
                EditorPrefs::setString(EditorPrefs::startupModeKey, startupModeBox.getText());
            };

            setTab(Tab::Interface);
        }

        ~GeneralSettingsPage() override
        {
            setLookAndFeel(nullptr);
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (recordingKey)
            {
                const int code = key.getKeyCode();
                if (code == juce::KeyPress::escapeKey)
                {
                    recordingKey = false;
                    refreshShortcutKeyLabel();
                    repaint();
                    return true;
                }

                EditorPrefs::setSwitcherKeyCode(code);
                recordingKey = false;
                refreshShortcutKeyLabel();
                repaint();
                fireSwitcherConfigChanged();
                return true;
            }
            return false;
        }

        void paint(juce::Graphics& g) override
        {
            juce::String title;
            juce::String subtitle;

            switch (selectedTab)
            {
                case Tab::Interface:
                    title = "Interface";
                    subtitle = "Visual preferences and routing shortcut configuration.";
                    break;
                case Tab::Startup:
                    title = "Startup";
                    subtitle = "Decide what opens automatically and how NOVA should behave on launch.";
                    break;
            }

            paintSettingsContentSurface(g, contentBounds, title, subtitle);

            if (selectedTab == Tab::Interface)
                paintInterfaceContent(g);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(10);
            tabsBounds = area.removeFromTop(34);
            area.removeFromTop(12);
            contentBounds = area;

            auto tabRow = tabsBounds;
            const int gap = 8;
            const int tabW = (tabRow.getWidth() - gap) / 2;
            btnInterfaceTab.setBounds(tabRow.removeFromLeft(tabW));
            tabRow.removeFromLeft(gap);
            btnStartupTab.setBounds(tabRow);

            auto content = contentBounds.reduced(22, 18);
            content.removeFromTop(44);

            if (selectedTab == Tab::Interface)
            {
                showStatsToggle.setBounds(content.removeFromTop(30));
                content.removeFromTop(20);

                // Section: Routing Shortcut
                routingSectionY = content.getY();
                content.removeFromTop(26); // section title

                // Shortcut key row
                auto keyRow = content.removeFromTop(34);
                shortcutLabelBounds = keyRow.removeFromLeft(110);
                btnShortcutKey.setBounds(keyRow.removeFromLeft(140));
                keyRow.removeFromLeft(8);
                btnResetShortcut.setBounds(keyRow.removeFromLeft(64));
                content.removeFromTop(16);

                // Mode chips row
                auto modesRow = content.removeFromTop(34);
                modesLabelBounds = modesRow.removeFromLeft(110);
                const int chipW = 96;
                const int chipGap = 8;
                for (int i = 0; i < 3; ++i)
                {
                    modeChips[i].setBounds(modesRow.removeFromLeft(chipW));
                    modesRow.removeFromLeft(chipGap);
                }
                content.removeFromTop(16);

                // Cycle preview area
                cyclePreviewBounds = content.removeFromTop(44);
            }
            else
            {
                openBrowserToggle.setBounds(content.removeFromTop(34));
                content.removeFromTop(12);
                openPresetsToggle.setBounds(content.removeFromTop(34));
                content.removeFromTop(12);
                startupModeBox.setBounds(content.removeFromTop(34));
                content.removeFromTop(12);
                confirmClearToggle.setBounds(content.removeFromTop(34));
            }
        }

    private:
        enum class Tab { Interface, Startup };

        static juce::Colour getModeColor(int index)
        {
            if (index == 0) return juce::Colour(0xff60A5FA);
            if (index == 1) return juce::Colour(0xffFBBF24);
            return juce::Colour(0xffF97316);
        }

        juce::String getModeName(int index) const
        {
            if (index == 0) return "LINE A";
            if (index == 1) return "PARALLEL";
            return "LINE B";
        }

        void paintInterfaceContent(juce::Graphics& g)
        {
            auto content = contentBounds.reduced(22, 18);

            // Section title
            g.setColour(Nova::Colors::Accent);
            g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            g.drawText("ROUTING SHORTCUT", content.getX(), routingSectionY,
                content.getWidth(), 20, juce::Justification::centredLeft);

            g.setColour(Nova::Colors::Border.withAlpha(0.5f));
            g.drawHorizontalLine(routingSectionY + 22,
                (float)content.getX(), (float)(content.getX() + content.getWidth()));

            // Labels
            g.setColour(Nova::Colors::Text.withAlpha(0.8f));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText("Shortcut key", shortcutLabelBounds, juce::Justification::centredLeft);
            g.drawText("Cycle modes", modesLabelBounds, juce::Justification::centredLeft);

            // Recording pulse
            if (recordingKey)
            {
                const float pulse = 0.5f + 0.5f * std::sin((float)juce::Time::getMillisecondCounter() * 0.006f);
                auto keyBounds = btnShortcutKey.getBounds().toFloat();
                g.setColour(Nova::Colors::Accent.withAlpha(0.15f + 0.15f * pulse));
                g.fillRoundedRectangle(keyBounds.expanded(4.0f), 8.0f);
                g.setColour(Nova::Colors::Accent.withAlpha(0.6f + 0.3f * pulse));
                g.drawRoundedRectangle(keyBounds.expanded(2.0f), 8.0f, 1.5f);
            }

            // Cycle preview
            if (!cyclePreviewBounds.isEmpty())
                paintCyclePreview(g);
        }

        void paintCyclePreview(juce::Graphics& g)
        {
            auto area = cyclePreviewBounds;

            // Background surface
            auto rect = area.toFloat();
            g.setColour(juce::Colour::fromString("ff0D1520").withAlpha(0.7f));
            g.fillRoundedRectangle(rect, 10.0f);
            g.setColour(Nova::Colors::Border.withAlpha(0.4f));
            g.drawRoundedRectangle(rect.reduced(0.5f), 10.0f, 1.0f);

            // Collect enabled modes
            std::vector<int> enabled;
            for (int i = 0; i < 3; ++i)
                if ((switcherModes >> i) & 1)
                    enabled.push_back(i);

            if (enabled.empty())
                return;

            const int count = (int)enabled.size();
            const int chipH = 26;
            const int chipW = 80;
            const int arrowW = 24;
            const int totalW = count * chipW + (count) * arrowW; // arrows loop back
            const int startX = area.getCentreX() - totalW / 2;
            const int chipY = area.getCentreY() - chipH / 2;

            for (int i = 0; i < count; ++i)
            {
                const int idx = enabled[(size_t)i];
                const int x = startX + i * (chipW + arrowW);

                // Mode chip
                auto chipRect = juce::Rectangle<float>((float)x, (float)chipY, (float)chipW, (float)chipH);
                auto color = getModeColor(idx);
                g.setColour(color.withAlpha(0.18f));
                g.fillRoundedRectangle(chipRect, 6.0f);
                g.setColour(color.withAlpha(0.7f));
                g.drawRoundedRectangle(chipRect.reduced(0.5f), 6.0f, 1.2f);

                g.setColour(color);
                g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
                g.drawText(getModeName(idx), chipRect, juce::Justification::centred);

                // Arrow to next (wraps around)
                const float arrowStartX = chipRect.getRight() + 4.0f;
                const float arrowEndX = arrowStartX + (float)arrowW - 8.0f;
                const float arrowCY = chipRect.getCentreY();

                auto nextColor = getModeColor(enabled[(size_t)((i + 1) % count)]);
                g.setColour(nextColor.withAlpha(0.55f));
                g.drawArrow(juce::Line<float>(arrowStartX, arrowCY, arrowEndX, arrowCY), 1.2f, 6.0f, 6.0f);
            }
        }

        void configureTabButton(juce::TextButton& button,
            const juce::String& text,
            std::function<void()> onClick)
        {
            addAndMakeVisible(button);
            styleSettingsTabButton(button, text);
            button.onClick = std::move(onClick);
        }

        void setTab(Tab newTab)
        {
            selectedTab = newTab;

            setSettingsTabButtonActive(btnInterfaceTab, selectedTab == Tab::Interface);
            setSettingsTabButtonActive(btnStartupTab, selectedTab == Tab::Startup);

            const bool interfaceVisible = selectedTab == Tab::Interface;
            const bool startupVisible = selectedTab == Tab::Startup;

            showStatsToggle.setVisible(interfaceVisible);
            btnShortcutKey.setVisible(interfaceVisible);
            btnResetShortcut.setVisible(interfaceVisible);
            for (auto& chip : modeChips) chip.setVisible(interfaceVisible);

            openBrowserToggle.setVisible(startupVisible);
            openPresetsToggle.setVisible(startupVisible);
            startupModeBox.setVisible(startupVisible);
            confirmClearToggle.setVisible(startupVisible);

            if (recordingKey)
            {
                recordingKey = false;
                refreshShortcutKeyLabel();
            }

            resized();
            repaint();
        }

        void startRecordingKey()
        {
            recordingKey = true;
            btnShortcutKey.setButtonText("Press a key...");
            btnShortcutKey.setColour(juce::TextButton::buttonColourId, Nova::Colors::Accent.withAlpha(0.14f));
            btnShortcutKey.setColour(juce::TextButton::textColourOffId, Nova::Colors::Accent);
            getTopLevelComponent()->grabKeyboardFocus();
            grabKeyboardFocus();
            repaint();
        }

        void refreshShortcutKeyLabel()
        {
            const int code = EditorPrefs::getSwitcherKeyCode();
            btnShortcutKey.setButtonText(EditorPrefs::keyCodeToDisplayName(code));
            btnShortcutKey.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff101722"));
            btnShortcutKey.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.92f));
        }

        void onModeChipClicked(int chipIndex)
        {
            // Count how many are currently on
            int tempModes = 0;
            for (int i = 0; i < 3; ++i)
                if (modeChips[i].getToggleState())
                    tempModes |= (1 << i);

            int count = ((tempModes >> 0) & 1) + ((tempModes >> 1) & 1) + ((tempModes >> 2) & 1);

            if (count < 2)
            {
                // Revert: can't disable — need at least 2
                modeChips[chipIndex].setToggleState(true, juce::dontSendNotification);
                return;
            }

            switcherModes = tempModes;
            EditorPrefs::setSwitcherModes(switcherModes);
            updateModeChipColors();
            repaint();
            fireSwitcherConfigChanged();
        }

        void updateModeChipColors()
        {
            for (int i = 0; i < 3; ++i)
            {
                const bool on = (switcherModes >> i) & 1;
                auto color = getModeColor(i);
                modeChips[i].setColour(juce::TextButton::buttonColourId,
                    on ? color.withAlpha(0.18f) : juce::Colour::fromString("ff0D1520"));
                modeChips[i].setColour(juce::TextButton::buttonOnColourId,
                    on ? color.withAlpha(0.28f) : juce::Colour::fromString("ff0D1520"));
                modeChips[i].setColour(juce::TextButton::textColourOffId,
                    on ? color : Nova::Colors::TextDim.withAlpha(0.4f));
                modeChips[i].setColour(juce::TextButton::textColourOnId,
                    on ? color : Nova::Colors::TextDim.withAlpha(0.4f));
            }
        }

        void fireSwitcherConfigChanged()
        {
            if (callbacks.onSwitcherConfigChanged)
                callbacks.onSwitcherConfigChanged();
        }

        static void setupToggle(juce::ToggleButton& toggle, const juce::String& text)
        {
            toggle.setButtonText(text);
            toggle.setColour(juce::ToggleButton::textColourId, Nova::Colors::Text.withAlpha(0.9f));
        }

        Callbacks callbacks;
        SettingsLookAndFeel lookAndFeel;
        juce::TextButton btnInterfaceTab;
        juce::TextButton btnStartupTab;

        // Interface tab
        juce::ToggleButton showStatsToggle;
        juce::TextButton btnShortcutKey;
        juce::TextButton btnResetShortcut;
        juce::TextButton modeChips[3];
        int switcherModes = 7;
        bool recordingKey = false;
        int routingSectionY = 0;
        juce::Rectangle<int> shortcutLabelBounds;
        juce::Rectangle<int> modesLabelBounds;
        juce::Rectangle<int> cyclePreviewBounds;

        // Startup tab
        juce::ToggleButton openBrowserToggle;
        juce::ToggleButton openPresetsToggle;
        juce::ToggleButton confirmClearToggle;
        juce::ComboBox startupModeBox;

        juce::Rectangle<int> tabsBounds;
        juce::Rectangle<int> contentBounds;
        Tab selectedTab = Tab::Interface;
    };

    class AudioSettingsPage final : public juce::Component,
        private juce::Timer
    {
    public:
        explicit AudioSettingsPage(NOVAAudioProcessor& processor)
            : audioProcessor(processor)
        {
            setLookAndFeel(&lookAndFeel);

            configureTabButton(btnSessionTab, "Session", [this] { setTab(Tab::Session); });
            configureTabButton(btnDefaultsTab, "Defaults", [this] { setTab(Tab::Defaults); });
            configureTabButton(btnDeviceTab, "Device", [this] { setTab(Tab::Device); });

            addAndMakeVisible(summaryLabel);
            summaryLabel.setColour(juce::Label::textColourId, Nova::Colors::Text.withAlpha(0.88f));
            summaryLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
            summaryLabel.setJustificationType(juce::Justification::topLeft);
            summaryLabel.setMinimumHorizontalScale(0.75f);

            addAndMakeVisible(hintLabel);
            hintLabel.setColour(juce::Label::textColourId, Nova::Colors::TextDim);
            hintLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
            hintLabel.setJustificationType(juce::Justification::topLeft);
            hintLabel.setMinimumHorizontalScale(0.8f);
            hintLabel.setText("NOVA keeps using the same JUCE audio device manager that already works in standalone.",
                juce::dontSendNotification);

            addAndMakeVisible(deviceHintLabel);
            deviceHintLabel.setColour(juce::Label::textColourId, Nova::Colors::Text.withAlpha(0.88f));
            deviceHintLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
            deviceHintLabel.setJustificationType(juce::Justification::topLeft);
            deviceHintLabel.setMinimumHorizontalScale(0.78f);

            tunerReferenceBox.addItem("A = 440 Hz", 1);
            tunerReferenceBox.addItem("A = 442 Hz", 2);
            tunerReferenceBox.addItem("A = 432 Hz", 3);
            tunerReferenceBox.setText(EditorPrefs::getString(EditorPrefs::tunerReferenceKey, "A = 440 Hz"),
                juce::dontSendNotification);
            tunerReferenceBox.onChange = [this]
            {
                const auto text = tunerReferenceBox.getText();
                EditorPrefs::setString(EditorPrefs::tunerReferenceKey, text);
                audioProcessor.getAudioEngine().setTunerReferencePitch(EditorPrefs::parseTunerReference());
            };
            addAndMakeVisible(tunerReferenceBox);

            latencyTipsToggle.setButtonText("Warn about high-latency audio setup");
            latencyTipsToggle.setToggleState(EditorPrefs::getBool(EditorPrefs::showLatencyTipsKey, true), juce::dontSendNotification);
            latencyTipsToggle.onClick = [this]
            {
                EditorPrefs::setBool(EditorPrefs::showLatencyTipsKey, latencyTipsToggle.getToggleState());
            };
            addAndMakeVisible(latencyTipsToggle);
            latencyTipsToggle.setTooltip("Warn when buffer size or device settings may hurt live playability.");

#if JucePlugin_Build_Standalone
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
            {
                const int maxInputs = juce::jmax(2, audioProcessor.getMainBusNumInputChannels());
                const int maxOutputs = juce::jmax(2, audioProcessor.getMainBusNumOutputChannels());

                audioSelector = std::make_unique<juce::AudioDeviceSelectorComponent>(
                    holder->deviceManager,
                    0, maxInputs,
                    0, maxOutputs,
                    true,
                    audioProcessor.producesMidi(),
                    true,
                    false);

                audioSelector->setItemHeight(30);
                audioSelector->setLookAndFeel(&lookAndFeel);
                addAndMakeVisible(audioSelector.get());
            }
#endif

            refreshSummary();
            startTimerHz(2);
            setTab(Tab::Session);
        }

        ~AudioSettingsPage() override
        {
            stopTimer();
            if (audioSelector != nullptr)
                audioSelector->setLookAndFeel(nullptr);
            setLookAndFeel(nullptr);
        }

        void paint(juce::Graphics& g) override
        {
            juce::String title;
            juce::String subtitle;

            switch (selectedTab)
            {
                case Tab::Session:
                    title = "Current Audio Session";
                    subtitle = "A live view of the device configuration that NOVA is currently using.";
                    break;
                case Tab::Defaults:
                    title = "Audio Defaults";
                    subtitle = "Tuning reference and latency preferences for your setup.";
                    break;
                case Tab::Device:
                    title = "Audio Device Manager";
                    subtitle = "Direct access to the embedded JUCE device setup.";
                    break;
            }

            paintSettingsContentSurface(g, contentBounds, title, subtitle);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(10);
            tabsBounds = area.removeFromTop(34);
            area.removeFromTop(12);
            contentBounds = area;

            auto tabRow = tabsBounds;
            const int gap = 8;
            const int tabW = (tabRow.getWidth() - gap * 2) / 3;
            btnSessionTab.setBounds(tabRow.removeFromLeft(tabW));
            tabRow.removeFromLeft(gap);
            btnDefaultsTab.setBounds(tabRow.removeFromLeft(tabW));
            tabRow.removeFromLeft(gap);
            btnDeviceTab.setBounds(tabRow);

            auto content = contentBounds.reduced(22, 18);
            content.removeFromTop(44);

            if (selectedTab == Tab::Session)
            {
                summaryLabel.setBounds(content.removeFromTop(64));
                content.removeFromTop(10);
                hintLabel.setBounds(content.removeFromTop(34));
            }
            else if (selectedTab == Tab::Defaults)
            {
                tunerReferenceBox.setBounds(content.removeFromTop(34));
                content.removeFromTop(12);
                latencyTipsToggle.setBounds(content.removeFromTop(34));
            }
            else
            {
                deviceHintLabel.setBounds(content.removeFromTop(40));
                content.removeFromTop(12);
                if (audioSelector != nullptr)
                    audioSelector->setBounds(content);
            }
        }

    private:
        enum class Tab
        {
            Session,
            Defaults,
            Device
        };

        void timerCallback() override
        {
            refreshSummary();
        }

        void configureTabButton(juce::TextButton& button,
            const juce::String& text,
            std::function<void()> onClick)
        {
            addAndMakeVisible(button);
            styleSettingsTabButton(button, text);
            button.onClick = std::move(onClick);
        }

        void setTab(Tab newTab)
        {
            selectedTab = newTab;

            setSettingsTabButtonActive(btnSessionTab, selectedTab == Tab::Session);
            setSettingsTabButtonActive(btnDefaultsTab, selectedTab == Tab::Defaults);
            setSettingsTabButtonActive(btnDeviceTab, selectedTab == Tab::Device);

            const bool sessionVisible = selectedTab == Tab::Session;
            const bool defaultsVisible = selectedTab == Tab::Defaults;
            const bool deviceVisible = selectedTab == Tab::Device;

            summaryLabel.setVisible(sessionVisible);
            hintLabel.setVisible(sessionVisible);

            tunerReferenceBox.setVisible(defaultsVisible);
            latencyTipsToggle.setVisible(defaultsVisible);

            if (audioSelector != nullptr)
                audioSelector->setVisible(deviceVisible);
            deviceHintLabel.setVisible(deviceVisible);

            resized();
            repaint();
        }

        void refreshSummary()
        {
#if JucePlugin_Build_Standalone
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
            {
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                holder->deviceManager.getAudioDeviceSetup(setup);

                if (auto* currentDevice = holder->deviceManager.getCurrentAudioDevice())
                {
                    const auto sampleRate = juce::String(currentDevice->getCurrentSampleRate(), 0);
                    const auto bufferSize = juce::String(currentDevice->getCurrentBufferSizeSamples());
                    const auto inputName = setup.inputDeviceName.isNotEmpty() ? setup.inputDeviceName : "Default Input";
                    const auto outputName = setup.outputDeviceName.isNotEmpty() ? setup.outputDeviceName : "Default Output";
                    const auto tunerReference = EditorPrefs::getString(EditorPrefs::tunerReferenceKey, "A = 440 Hz");

                    summaryLabel.setText(
                        "Driver: " + currentDevice->getTypeName()
                        + "    |    Sample Rate: " + sampleRate + " Hz"
                        + "    |    Buffer: " + bufferSize + " samples\n"
                        + "Input: " + inputName + "\n"
                        + "Output: " + outputName + "\n"
                        + "Tuner Reference: " + tunerReference,
                        juce::dontSendNotification);
                    deviceHintLabel.setText("Use this tab for direct interface, driver and buffer selection.",
                        juce::dontSendNotification);
                    return;
                }

                summaryLabel.setText("No active audio device is currently open.", juce::dontSendNotification);
                deviceHintLabel.setText("No audio device is currently open.", juce::dontSendNotification);
                return;
            }
#endif

            summaryLabel.setText(
                "Audio device setup is controlled by the host in plugin formats.\n"
                "Open NOVA Standalone to manage interfaces, drivers and direct monitoring locally.",
                juce::dontSendNotification);
            deviceHintLabel.setText(
                "This instance is hosted by a DAW. Use NOVA Standalone for direct device configuration.",
                juce::dontSendNotification);
        }

        NOVAAudioProcessor& audioProcessor;
        SettingsLookAndFeel lookAndFeel;
        juce::TextButton btnSessionTab;
        juce::TextButton btnDefaultsTab;
        juce::TextButton btnDeviceTab;
        juce::Label summaryLabel;
        juce::Label hintLabel;
        juce::Label deviceHintLabel;
        juce::ComboBox tunerReferenceBox;
        juce::ToggleButton latencyTipsToggle;
        std::unique_ptr<juce::AudioDeviceSelectorComponent> audioSelector;
        juce::Rectangle<int> tabsBounds;
        juce::Rectangle<int> contentBounds;
        Tab selectedTab = Tab::Session;
    };

    class LibrarySettingsPage final : public juce::Component
    {
    public:
        struct Callbacks
        {
            std::function<juce::String()> getCurrentPresetName;
            std::function<juce::String()> getStartupPresetName;
            std::function<juce::File()> getPresetDirectory;
            std::function<void()> onSetCurrentPresetAsStartup;
            std::function<void()> onClearStartupPreset;
        };

        explicit LibrarySettingsPage(Callbacks pageCallbacks)
            : callbacks(std::move(pageCallbacks))
        {
            setLookAndFeel(&lookAndFeel);

            configureTabButton(btnStatusTab, "Status", [this] { setTab(Tab::Status); });
            configureTabButton(btnDefaultsTab, "Defaults", [this] { setTab(Tab::Defaults); });

            styleSettingsLabel(currentPresetLabel, 13.0f, Nova::Colors::Text.withAlpha(0.88f));
            styleSettingsLabel(startupPresetLabel, 13.0f, Nova::Colors::Text.withAlpha(0.88f));
            currentPresetLabel.setMinimumHorizontalScale(0.75f);
            startupPresetLabel.setMinimumHorizontalScale(0.75f);
            addAndMakeVisible(currentPresetLabel);
            addAndMakeVisible(startupPresetLabel);

            libraryViewBox.addItem("All presets", 1);
            libraryViewBox.addItem("Favorites first", 2);
            libraryViewBox.addItem("Recent first", 3);
            libraryViewBox.setText(EditorPrefs::getString(EditorPrefs::libraryViewKey, "All presets"),
                juce::dontSendNotification);
            libraryViewBox.onChange = [this]
            {
                EditorPrefs::setString(EditorPrefs::libraryViewKey, libraryViewBox.getText());
            };
            addAndMakeVisible(libraryViewBox);

            favoritesFirstToggle.setButtonText("Show favorites first");
            favoritesFirstToggle.setToggleState(EditorPrefs::getBool(EditorPrefs::favoritesFirstKey, false), juce::dontSendNotification);
            favoritesFirstToggle.onClick = [this]
            {
                EditorPrefs::setBool(EditorPrefs::favoritesFirstKey, favoritesFirstToggle.getToggleState());
            };
            addAndMakeVisible(favoritesFirstToggle);
            favoritesFirstToggle.setTooltip("Prefer favorite presets near the top of the browser.");

            styleSettingsActionButton(btnSetStartup, "Use Current on Start");
            btnSetStartup.onClick = [this]
            {
                if (callbacks.onSetCurrentPresetAsStartup)
                    callbacks.onSetCurrentPresetAsStartup();
                refreshSummary();
            };
            addAndMakeVisible(btnSetStartup);
            btnSetStartup.setTooltip("Use the current saved preset when NOVA starts.");

            styleSettingsActionButton(btnClearStartup, "Clear Startup", true);
            btnClearStartup.onClick = [this]
            {
                if (callbacks.onClearStartupPreset)
                    callbacks.onClearStartupPreset();
                refreshSummary();
            };
            addAndMakeVisible(btnClearStartup);
            btnClearStartup.setTooltip("Remove the preset currently assigned for startup.");

            styleSettingsActionButton(btnOpenFolder, "Open Preset Folder", true);
            btnOpenFolder.onClick = [this]
            {
                if (callbacks.getPresetDirectory)
                    callbacks.getPresetDirectory().revealToUser();
            };
            addAndMakeVisible(btnOpenFolder);
            btnOpenFolder.setTooltip("Open the local preset folder in the file explorer.");

            refreshSummary();
            setTab(Tab::Status);
        }

        ~LibrarySettingsPage() override
        {
            setLookAndFeel(nullptr);
        }

        void paint(juce::Graphics& g) override
        {
            juce::String title;
            juce::String subtitle;

            switch (selectedTab)
            {
                case Tab::Status:
                    title = "Library Status";
                    subtitle = "Quick access to the current preset, startup preset and local preset folder.";
                    break;
                case Tab::Defaults:
                    title = "Library Defaults";
                    subtitle = "Behavior that shapes how presets are surfaced and remembered.";
                    break;
            }

            paintSettingsContentSurface(g, contentBounds, title, subtitle);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(10);
            tabsBounds = area.removeFromTop(34);
            area.removeFromTop(12);
            contentBounds = area;

            auto tabRow = tabsBounds;
            const int gap = 8;
            const int tabW = (tabRow.getWidth() - gap) / 2;
            btnStatusTab.setBounds(tabRow.removeFromLeft(tabW));
            tabRow.removeFromLeft(gap);
            btnDefaultsTab.setBounds(tabRow);

            auto content = contentBounds.reduced(22, 18);
            content.removeFromTop(44);

            if (selectedTab == Tab::Status)
            {
                currentPresetLabel.setBounds(content.removeFromTop(28));
                content.removeFromTop(8);
                startupPresetLabel.setBounds(content.removeFromTop(28));
                content.removeFromTop(12);
                auto topButtons = content.removeFromTop(34);
                const int buttonGap = 10;
                const int buttonW = (topButtons.getWidth() - buttonGap) / 2;
                btnSetStartup.setBounds(topButtons.removeFromLeft(buttonW));
                topButtons.removeFromLeft(buttonGap);
                btnClearStartup.setBounds(topButtons);
                content.removeFromTop(12);
                btnOpenFolder.setBounds(content.removeFromTop(34));
            }
            else
            {
                favoritesFirstToggle.setBounds(content.removeFromTop(34));
                content.removeFromTop(12);
                libraryViewBox.setBounds(content.removeFromTop(34));
            }
        }

    private:
        enum class Tab
        {
            Status,
            Defaults
        };

        void configureTabButton(juce::TextButton& button,
            const juce::String& text,
            std::function<void()> onClick)
        {
            addAndMakeVisible(button);
            styleSettingsTabButton(button, text);
            button.onClick = std::move(onClick);
        }

        void setTab(Tab newTab)
        {
            selectedTab = newTab;

            setSettingsTabButtonActive(btnStatusTab, selectedTab == Tab::Status);
            setSettingsTabButtonActive(btnDefaultsTab, selectedTab == Tab::Defaults);

            const bool statusVisible = selectedTab == Tab::Status;
            const bool defaultsVisible = selectedTab == Tab::Defaults;

            currentPresetLabel.setVisible(statusVisible);
            startupPresetLabel.setVisible(statusVisible);
            btnSetStartup.setVisible(statusVisible);
            btnClearStartup.setVisible(statusVisible);
            btnOpenFolder.setVisible(statusVisible);

            favoritesFirstToggle.setVisible(defaultsVisible);
            libraryViewBox.setVisible(defaultsVisible);

            resized();
            repaint();
        }

        void refreshSummary()
        {
            const auto currentPreset = callbacks.getCurrentPresetName ? callbacks.getCurrentPresetName() : juce::String{};
            const auto startupPreset = callbacks.getStartupPresetName ? callbacks.getStartupPresetName() : juce::String{};

            const auto currentText = "Current preset: "
                + (currentPreset.isNotEmpty() ? currentPreset : "No preset selected");
            const auto startupText = "Startup preset: "
                + (startupPreset.isNotEmpty() ? startupPreset : "None");

            currentPresetLabel.setText(currentText, juce::dontSendNotification);
            startupPresetLabel.setText(startupText, juce::dontSendNotification);
            currentPresetLabel.setTooltip(currentText);
            startupPresetLabel.setTooltip(startupText);
        }

        Callbacks callbacks;
        SettingsLookAndFeel lookAndFeel;
        juce::TextButton btnStatusTab;
        juce::TextButton btnDefaultsTab;
        juce::Label currentPresetLabel;
        juce::Label startupPresetLabel;
        juce::ComboBox libraryViewBox;
        juce::ToggleButton favoritesFirstToggle;
        juce::TextButton btnSetStartup;
        juce::TextButton btnClearStartup;
        juce::TextButton btnOpenFolder;
        juce::Rectangle<int> tabsBounds;
        juce::Rectangle<int> contentBounds;
        Tab selectedTab = Tab::Status;
    };

    // Controllers, Profile and Cloud are planned features — show clean placeholders

    class SettingsOverlay final : public juce::Component
    {
    public:
        SettingsOverlay(NOVAAudioProcessor& processor,
            std::function<void(bool)> onShowStatsChanged,
            std::function<void(bool)> onOpenBrowserOnStartupChanged,
            std::function<void(bool)> onOpenPresetsOnStartupChanged,
            std::function<void()> onSwitcherConfigChangedFn,
            LibrarySettingsPage::Callbacks libraryCallbacks,
            std::function<void(int)> onLaunchWizardFn,
            std::function<void()> onCloseFn)
            : onClose(std::move(onCloseFn)),
              onLaunchWizard(std::move(onLaunchWizardFn))
        {
            setWantsKeyboardFocus(true);

            addAndMakeVisible(closeButton);
            closeButton.setButtonText("CLOSE");
            closeButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff2A1418"));
            closeButton.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.88f));
            closeButton.onClick = [this]
            {
                if (onClose)
                    onClose();
            };

            configureCategoryButton(btnGeneral, "GENERAL", [this] { setCategory(Category::General); });
            configureCategoryButton(btnAudio, "AUDIO", [this] { setCategory(Category::Audio); });
            configureCategoryButton(btnLibrary, "LIBRARY", [this] { setCategory(Category::Library); });
            configureCategoryButton(btnControllers, "CONTROLLERS", [this] { setCategory(Category::Controllers); });
            configureCategoryButton(btnProfile, "PROFILE", [this] { setCategory(Category::Profile); });
            configureCategoryButton(btnCloud, "CLOUD", [this] { setCategory(Category::Cloud); });

            // --- Wizard launch buttons in the rail ---
            configureWizardButton(btnWizardAudio, "Audio Setup", [this] { fireWizard(0); });
            configureWizardButton(btnWizardStart, "Start Wizard", [this] { fireWizard(1); });
            configureWizardButton(btnWizardPreset, "Preset Finder", [this] { fireWizard(2); });

            generalPage = std::make_unique<GeneralSettingsPage>(GeneralSettingsPage::Callbacks{
                std::move(onShowStatsChanged),
                std::move(onOpenBrowserOnStartupChanged),
                std::move(onOpenPresetsOnStartupChanged),
                std::move(onSwitcherConfigChangedFn)
            });

            audioPage = std::make_unique<AudioSettingsPage>(processor);
            libraryPage = std::make_unique<LibrarySettingsPage>(std::move(libraryCallbacks));

            controllersPage = std::make_unique<PlaceholderSettingsPage>(
                "Controllers & MIDI",
                "MIDI learn, footswitch mapping, expression pedal configuration and scene switching "
                "will live here once the controller subsystem ships.",
                "Setup wizards for MIDI, footswitches and expression pedals will guide "
                "first-time configuration when this section becomes active.");

            profilePage = std::make_unique<PlaceholderSettingsPage>(
                "Player Profile",
                "Player identity, skill level, genre preferences and personalized tone "
                "recommendations will live here once the recommendation engine ships.",
                "Personalization and tone preference wizards will help build a profile "
                "tailored to each player's style and needs.");

            cloudPage = std::make_unique<PlaceholderSettingsPage>(
                "Cloud & Account",
                "Account login, preset sync, cloud backup and multi-device restore "
                "will live here once online services are available.",
                "Login, sync and restore wizards will guide account setup and "
                "cloud operations when the backend ships.");

            addAndMakeVisible(*generalPage);
            addAndMakeVisible(*audioPage);
            addAndMakeVisible(*libraryPage);
            addAndMakeVisible(*controllersPage);
            addAndMakeVisible(*profilePage);
            addAndMakeVisible(*cloudPage);

            setCategory(Category::General);
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::escapeKey)
            {
                if (onClose)
                    onClose();
                return true;
            }

            return false;
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (!panelBounds.contains(e.getPosition()))
            {
                if (onClose)
                    onClose();
            }
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colours::black.withAlpha(0.88f));

            const auto pf = panelBounds.toFloat();
            juce::ColourGradient fill(juce::Colour::fromString("ff121A27"),
                pf.getCentreX(), pf.getY(),
                juce::Colour::fromString("ff0B0E14"),
                pf.getCentreX(), pf.getBottom(), false);
            g.setGradientFill(fill);
            g.fillRoundedRectangle(pf, 22.0f);

            g.setColour(Nova::Colors::Border.withAlpha(0.85f));
            g.drawRoundedRectangle(pf.reduced(0.5f), 22.0f, 1.0f);

            g.setColour(Nova::Colors::Accent.withAlpha(0.07f));
            g.fillRoundedRectangle(pf.reduced(1.0f).removeFromTop(84.0f), 22.0f);

            auto rail = leftRailBounds.toFloat();
            juce::ColourGradient railFill(juce::Colour::fromString("ff1A2332"),
                rail.getCentreX(), rail.getY(),
                juce::Colour::fromString("ff101722"),
                rail.getCentreX(), rail.getBottom(), false);
            g.setGradientFill(railFill);
            g.fillRoundedRectangle(rail, 20.0f);

            g.setColour(Nova::Colors::Border.withAlpha(0.6f));
            g.drawVerticalLine(leftRailBounds.getRight(), (float)(leftRailBounds.getY() + 24), (float)(leftRailBounds.getBottom() - 24));
            g.drawHorizontalLine(headerBounds.getBottom(), (float)contentAreaBounds.getX(), (float)panelBounds.getRight() - 24.0f);

            auto titleArea = headerBounds;
            g.setColour(Nova::Colors::Accent);
            g.setFont(juce::Font(juce::FontOptions(21.0f, juce::Font::bold)));
            g.drawText("NOVA SETTINGS", titleArea.removeFromTop(24), juce::Justification::centredLeft);

            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawFittedText("One home for audio, workflow, library, control and future account setup.",
                titleArea, juce::Justification::centredLeft, 2);

            // "WIZARDS" label above wizard buttons in rail
            if (btnWizardAudio.getY() > 0)
            {
                g.setColour(Nova::Colors::TextDim.withAlpha(0.6f));
                g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
                g.drawText("WIZARDS", btnWizardAudio.getX(), btnWizardAudio.getY() - 18,
                           btnWizardAudio.getWidth(), 14, juce::Justification::centredLeft);

                g.setColour(Nova::Colors::Border.withAlpha(0.4f));
                g.drawHorizontalLine(btnWizardAudio.getY() - 22,
                    (float)(leftRailBounds.getX() + 14), (float)(leftRailBounds.getRight() - 14));
            }
        }

        void resized() override
        {
            const int modalW = juce::jlimit(760, 1120, (int)std::round((double)getWidth() * 0.66));
            const int modalH = juce::jlimit(560, 780, (int)std::round((double)getHeight() * 0.72));
            panelBounds = juce::Rectangle<int>(0, 0, modalW, modalH).withCentre(getLocalBounds().getCentre());

            const int outerPad = 18;
            const int railWidth = 188;
            const int contentGap = 22;
            const int headerHeight = 62;

            leftRailBounds = juce::Rectangle<int>(panelBounds.getX() + outerPad,
                panelBounds.getY() + outerPad,
                railWidth,
                panelBounds.getHeight() - outerPad * 2);

            contentAreaBounds = juce::Rectangle<int>(leftRailBounds.getRight() + contentGap,
                panelBounds.getY() + outerPad,
                panelBounds.getRight() - leftRailBounds.getRight() - contentGap - outerPad,
                panelBounds.getHeight() - outerPad * 2);

            headerBounds = contentAreaBounds.removeFromTop(headerHeight);
            contentAreaBounds.removeFromTop(16);
            contentBounds = contentAreaBounds;

            closeButton.setBounds(panelBounds.getRight() - 100, panelBounds.getY() + 18, 82, 28);

            auto nav = leftRailBounds.reduced(14, 16);
            auto brand = nav.removeFromTop(56);
            juce::ignoreUnused(brand);
            const int buttonH = 32;
            const int buttonGap = 8;
            btnGeneral.setBounds(nav.removeFromTop(buttonH));
            nav.removeFromTop(buttonGap);
            btnAudio.setBounds(nav.removeFromTop(buttonH));
            nav.removeFromTop(buttonGap);
            btnLibrary.setBounds(nav.removeFromTop(buttonH));
            nav.removeFromTop(buttonGap);
            btnControllers.setBounds(nav.removeFromTop(buttonH));
            nav.removeFromTop(buttonGap);
            btnProfile.setBounds(nav.removeFromTop(buttonH));
            nav.removeFromTop(buttonGap);
            btnCloud.setBounds(nav.removeFromTop(buttonH));

            // Wizard buttons at bottom of rail
            {
                const int wizBtnH = 28;
                const int wizGap = 6;
                auto wizArea = leftRailBounds.reduced(14, 0);
                wizArea = wizArea.removeFromBottom(wizBtnH * 3 + wizGap * 2 + 16);
                wizArea.removeFromTop(8);
                btnWizardAudio.setBounds(wizArea.removeFromTop(wizBtnH));
                wizArea.removeFromTop(wizGap);
                btnWizardStart.setBounds(wizArea.removeFromTop(wizBtnH));
                wizArea.removeFromTop(wizGap);
                btnWizardPreset.setBounds(wizArea.removeFromTop(wizBtnH));
            }

            if (generalPage != nullptr) generalPage->setBounds(contentBounds);
            if (audioPage != nullptr) audioPage->setBounds(contentBounds);
            if (libraryPage != nullptr) libraryPage->setBounds(contentBounds);
            if (controllersPage != nullptr) controllersPage->setBounds(contentBounds);
            if (profilePage != nullptr) profilePage->setBounds(contentBounds);
            if (cloudPage != nullptr) cloudPage->setBounds(contentBounds);
        }

    private:
        enum class Category
        {
            General,
            Audio,
            Library,
            Controllers,
            Profile,
            Cloud
        };

        void configureCategoryButton(juce::TextButton& button,
            const juce::String& text,
            std::function<void()> onClick)
        {
            addAndMakeVisible(button);
            button.setButtonText(text);
            button.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff121A27"));
            button.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.82f));
            button.onClick = std::move(onClick);
        }

        void configureWizardButton(juce::TextButton& button,
            const juce::String& text,
            std::function<void()> onClick)
        {
            addAndMakeVisible(button);
            button.setButtonText(text);
            button.setColour(juce::TextButton::buttonColourId, Nova::Colors::Accent.withAlpha(0.10f));
            button.setColour(juce::TextButton::textColourOffId, Nova::Colors::Accent.withAlpha(0.85f));
            button.onClick = std::move(onClick);
        }

        void fireWizard(int id)
        {
            if (onClose) onClose();
            if (onLaunchWizard) onLaunchWizard(id);
        }

        void setCategory(Category newCategory)
        {
            selectedCategory = newCategory;

            auto setButtonState = [](juce::TextButton& button, bool active)
            {
                button.setColour(juce::TextButton::buttonColourId,
                    active ? Nova::Colors::Accent.withAlpha(0.22f) : juce::Colour::fromString("ff121A27"));
                button.setColour(juce::TextButton::textColourOffId,
                    active ? Nova::Colors::Text : Nova::Colors::Text.withAlpha(0.82f));
            };

            setButtonState(btnGeneral, selectedCategory == Category::General);
            setButtonState(btnAudio, selectedCategory == Category::Audio);
            setButtonState(btnLibrary, selectedCategory == Category::Library);
            setButtonState(btnControllers, selectedCategory == Category::Controllers);
            setButtonState(btnProfile, selectedCategory == Category::Profile);
            setButtonState(btnCloud, selectedCategory == Category::Cloud);

            if (generalPage != nullptr) generalPage->setVisible(selectedCategory == Category::General);
            if (audioPage != nullptr) audioPage->setVisible(selectedCategory == Category::Audio);
            if (libraryPage != nullptr) libraryPage->setVisible(selectedCategory == Category::Library);
            if (controllersPage != nullptr) controllersPage->setVisible(selectedCategory == Category::Controllers);
            if (profilePage != nullptr) profilePage->setVisible(selectedCategory == Category::Profile);
            if (cloudPage != nullptr) cloudPage->setVisible(selectedCategory == Category::Cloud);

            repaint();
        }

        std::function<void()> onClose;
        std::function<void(int)> onLaunchWizard;
        juce::Rectangle<int> panelBounds;
        juce::Rectangle<int> leftRailBounds;
        juce::Rectangle<int> headerBounds;
        juce::Rectangle<int> contentAreaBounds;
        juce::Rectangle<int> contentBounds;
        juce::TextButton closeButton;
        juce::TextButton btnGeneral;
        juce::TextButton btnAudio;
        juce::TextButton btnLibrary;
        juce::TextButton btnControllers;
        juce::TextButton btnProfile;
        juce::TextButton btnCloud;
        juce::TextButton btnWizardAudio;
        juce::TextButton btnWizardStart;
        juce::TextButton btnWizardPreset;
        std::unique_ptr<GeneralSettingsPage> generalPage;
        std::unique_ptr<AudioSettingsPage> audioPage;
        std::unique_ptr<LibrarySettingsPage> libraryPage;
        std::unique_ptr<PlaceholderSettingsPage> controllersPage;
        std::unique_ptr<PlaceholderSettingsPage> profilePage;
        std::unique_ptr<PlaceholderSettingsPage> cloudPage;
        Category selectedCategory = Category::General;
    };
}

// Wizard headers — included after EditorPrefs namespace so inline methods can access it
#include "../GUI/Wizards/AudioSetupWizard.h"
#include "../GUI/Wizards/StartWizard.h"
#include "../GUI/Wizards/PresetFinderWizard.h"

class PedalEditorOverlay final : public juce::Component
{
public:
    PedalEditorOverlay(juce::String pedalName,
        juce::String pedalSubtitle,
        std::unique_ptr<juce::AudioProcessorEditor> editorToShow,
        std::function<void()> onCloseFn,
        juce::Colour accentCol = juce::Colour::fromString("ffA78BFA"))
        : title(std::move(pedalName)),
          subtitle(std::move(pedalSubtitle)),
          pedalEditor(std::move(editorToShow)),
          onClose(std::move(onCloseFn)),
          accent(accentCol)
    {
        setWantsKeyboardFocus(true);

        addAndMakeVisible(viewport);
        viewport.setScrollBarsShown(true, false);
        editorCanvas = std::make_unique<juce::Component>();
        viewport.setViewedComponent(editorCanvas.get(), false);

        if (pedalEditor != nullptr)
        {
            naturalEditorSize = { juce::jmax(1, pedalEditor->getWidth()), juce::jmax(1, pedalEditor->getHeight()) };
            editorCanvas->addAndMakeVisible(pedalEditor.get());
            pedalEditor->setInterceptsMouseClicks(true, true);
        }
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (onClose)
                onClose();
            return true;
        }

        return false;
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (!panelBounds.contains(e.getPosition()))
        {
            if (onClose)
                onClose();
        }
    }

    void paint(juce::Graphics& g) override
    {
        // Dark scrim
        g.fillAll(juce::Colours::black.withAlpha(0.88f));

        const auto pf = panelBounds.toFloat();

        // Panel fill
        juce::ColourGradient panelFill(juce::Colour::fromString("ff111827"),
            pf.getCentreX(), pf.getY(),
            juce::Colour::fromString("ff0B0E14"),
            pf.getCentreX(), pf.getBottom(), false);
        g.setGradientFill(panelFill);
        g.fillRoundedRectangle(pf, 16.0f);

        // Accent border
        g.setColour(accent.withAlpha(0.35f));
        g.drawRoundedRectangle(pf.reduced(0.5f), 16.0f, 1.5f);

        // Top glow strip
        auto topGlow = pf.reduced(20.0f, 0.0f).removeFromTop(2.5f);
        juce::ColourGradient glowGrad(juce::Colours::transparentBlack, topGlow.getX(), topGlow.getCentreY(),
            accent.withAlpha(0.55f), topGlow.getCentreX(), topGlow.getCentreY(), false);
        glowGrad.addColour(1.0, juce::Colours::transparentBlack);
        g.setGradientFill(glowGrad);
        g.fillRoundedRectangle(topGlow, 1.5f);

        // Floating close button circle
        const auto closeCentre = juce::Point<float>(pf.getRight() - 6.0f, pf.getY() - 6.0f);
        const float closeR = 16.0f;
        g.setColour(juce::Colour::fromString("ff1A2332"));
        g.fillEllipse(closeCentre.x - closeR, closeCentre.y - closeR, closeR * 2.0f, closeR * 2.0f);
        g.setColour(accent.withAlpha(0.50f));
        g.drawEllipse(closeCentre.x - closeR, closeCentre.y - closeR, closeR * 2.0f, closeR * 2.0f, 1.2f);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        g.drawText(juce::String::charToString(0xD7),
            juce::Rectangle<float>(closeCentre.x - closeR, closeCentre.y - closeR, closeR * 2.0f, closeR * 2.0f).toNearestInt(),
            juce::Justification::centred);

        closeBtnBounds = juce::Rectangle<float>(closeCentre.x - closeR - 4.0f, closeCentre.y - closeR - 4.0f,
            (closeR + 4.0f) * 2.0f, (closeR + 4.0f) * 2.0f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (closeBtnBounds.contains(e.getPosition().toFloat()))
        {
            if (onClose)
                onClose();
        }
    }

    void resized() override
    {
        const auto windowBounds = getLocalBounds();
        const int padding = 16;

        // Calculate panel size: wrap tightly around editor + minimal padding
        int panelW = naturalEditorSize.x + padding * 2;
        int panelH = naturalEditorSize.y + padding * 2;

        // Clamp to available window size with margin
        const int maxW = windowBounds.getWidth() - 40;
        const int maxH = windowBounds.getHeight() - 40;
        const bool needsViewport = (panelW > maxW || panelH > maxH);

        panelW = juce::jmin(panelW, maxW);
        panelH = juce::jmin(panelH, maxH);

        // Ensure minimum size
        panelW = juce::jmax(panelW, 280);
        panelH = juce::jmax(panelH, 200);

        panelBounds = juce::Rectangle<int>(
            (windowBounds.getWidth() - panelW) / 2,
            (windowBounds.getHeight() - panelH) / 2,
            panelW, panelH);

        auto content = panelBounds.reduced(padding);
        viewport.setBounds(content);

        if (pedalEditor != nullptr)
        {
            const int canvasW = juce::jmax(content.getWidth(), naturalEditorSize.x);
            const int canvasH = juce::jmax(content.getHeight(), naturalEditorSize.y);
            editorCanvas->setSize(canvasW, canvasH);
            pedalEditor->setBounds((canvasW - naturalEditorSize.x) / 2,
                (canvasH - naturalEditorSize.y) / 2,
                naturalEditorSize.x,
                naturalEditorSize.y);
        }

        viewport.setScrollBarsShown(needsViewport, false);
    }

private:
    juce::String title;
    juce::String subtitle;
    std::unique_ptr<juce::AudioProcessorEditor> pedalEditor;
    std::unique_ptr<juce::Component> editorCanvas;
    std::function<void()> onClose;
    juce::Point<int> naturalEditorSize { 1, 1 };
    juce::Colour accent;

    juce::Viewport viewport;
    juce::Rectangle<int> panelBounds;
    mutable juce::Rectangle<float> closeBtnBounds;
};

class PedalSlotComponent final : public juce::Component
{
public:
    PedalSlotComponent(NOVAAudioProcessor& ownerProcessor,
        Nova::ChainID ownerChain,
        std::function<void(Nova::ChainID, juce::String)> openEditorFn)
        : processor(ownerProcessor),
          chain(ownerChain),
          onOpenEditor(std::move(openEditorFn))
    {
        setRepaintsOnMouseActivity(true);
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);

        addAndMakeVisible(powerButton);
        powerButton.onClick = [this] { toggleBypass(); };
        powerButton.setTriggeredOnMouseDown(false);

        addAndMakeVisible(removeButton);
        removeButton.onClick = [this] { removePedal(); };
        removeButton.setTriggeredOnMouseDown(false);

        addAndMakeVisible(configButton);
        configButton.setButtonText("OPEN");
        configButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        configButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.8f));
        configButton.onClick = [this]
        {
            if (onOpenEditor == nullptr) return;
            const auto pedalID = getPedalID();
            if (pedalID.isNotEmpty())
                onOpenEditor(chain, pedalID);
        };
    }

    void mouseEnter(const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() < 5)
            return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        {
            const auto chainStr = (chain == Nova::ChainID::LineA) ? "LineA" : "LineB";
            container->startDragging("MOVE:" + juce::String(chainStr) + ":" + juce::String(getPedalIndex()), this);
        }
    }

    Nova::ChainID getChain() const { return chain; }
    juce::String getPedalID() const { return pedalState.getProperty(Nova::IDs::PEDAL_ID).toString(); }
    Nova::ZoneID getZone() const
    {
        return static_cast<Nova::ZoneID>(
            (int)pedalState.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
    }

    int getPedalIndex() const
    {
        auto parent = pedalState.getParent();
        return parent.isValid() ? parent.indexOf(pedalState) : -1;
    }

    void setPedalState(juce::ValueTree newState)
    {
        pedalState = newState;
        refreshVisualState();
        repaint();
    }

    int getPreferredWidth() const
    {
        switch (getZone())
        {
            case Nova::ZoneID::Amp:
            case Nova::ZoneID::Cabinet:  return 204;
            default:                     return 156;
        }
    }

    int getPreferredHeight() const
    {
        switch (getZone())
        {
            case Nova::ZoneID::Amp:
            case Nova::ZoneID::Cabinet:  return 132;
            default:                     return 112;
        }
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool enabled = isPedalEnabled();
        const auto accent = Nova::PedalCatalog::accentForType(getDisplayName());

        const float cornerR = getHeight() < 90 ? 8.0f : 12.0f;

        // ---- Accent-colored background ----
        const float sat = enabled ? 0.45f : 0.15f;
        const float briTop = enabled ? 0.18f : 0.08f;
        const float briBot = enabled ? 0.10f : 0.05f;
        const auto bgTop = accent.withSaturation(sat).withBrightness(briTop);
        const auto bgBot = accent.withSaturation(sat * 0.7f).withBrightness(briBot);

        juce::ColourGradient bg(bgTop, bounds.getCentreX(), bounds.getY(),
            bgBot, bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(bg);
        g.fillRoundedRectangle(bounds, cornerR);

        // ---- Border ----
        if (hovered && enabled)
        {
            g.setColour(accent.withAlpha(0.65f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), cornerR, 1.8f);
        }
        else
        {
            g.setColour(accent.withAlpha(enabled ? 0.25f : 0.08f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), cornerR, 1.0f);
        }

        // ---- Top accent glow line ----
        if (enabled)
        {
            auto glowBar = bounds.reduced(cornerR, 0.0f).removeFromTop(2.0f);
            g.setColour(accent.withAlpha(hovered ? 0.6f : 0.35f));
            g.fillRoundedRectangle(glowBar, 1.0f);
        }

        // ---- Pedal name (centred) ----
        const float nameFontSz = getHeight() < 90 ? 10.0f : (getHeight() < 108 ? 12.0f : 14.0f);
        auto nameArea = bounds.reduced(6.0f);
        nameArea.removeFromTop((float)(getHeight() < 90 ? 18 : 24));
        nameArea.removeFromBottom((float)(getHeight() < 90 ? 4 : 28));

        g.setColour(enabled ? juce::Colours::white.withAlpha(0.95f) : juce::Colours::white.withAlpha(0.4f));
        g.setFont(juce::Font(juce::FontOptions(nameFontSz, juce::Font::bold)));
        g.drawFittedText(getDisplayName(), nameArea.toNearestInt(), juce::Justification::centred, 2);

        // ---- Bypass darkening overlay ----
        if (!enabled)
        {
            g.setColour(juce::Colours::black.withAlpha(0.55f));
            g.fillRoundedRectangle(bounds, cornerR);

            // X icon
            const float midX = bounds.getCentreX();
            const float midY = bounds.getCentreY();
            const float r = getHeight() < 90 ? 4.0f : 6.0f;
            g.setColour(Nova::Colors::Error.withAlpha(0.5f));
            g.drawLine(midX - r, midY - r, midX + r, midY + r, 2.0f);
            g.drawLine(midX + r, midY - r, midX - r, midY + r, 2.0f);
        }
    }

    void resized() override
    {
        const bool mini = getHeight() < 90;
        const int pad   = mini ? 3 : 5;
        const int btnH  = mini ? 16 : 20;

        auto area = getLocalBounds().reduced(pad);

        // Top row: [power] ... [delete]
        auto topRow = area.removeFromTop(btnH);
        powerButton.setBounds(topRow.removeFromLeft(mini ? 28 : 36).reduced(1));
        removeButton.setBounds(topRow.removeFromRight(mini ? 18 : 22).reduced(1));

        // Bottom row: config button (hidden in mini)
        if (!mini)
        {
            auto botRow = area.removeFromBottom(btnH);
            const int cfgW = juce::jmin(60, botRow.getWidth() - 8);
            configButton.setBounds(botRow.withSizeKeepingCentre(cfgW, btnH));
            configButton.setVisible(true);
        }
        else
        {
            configButton.setVisible(false);
        }
    }

private:
    bool isPedalEnabled() const
    {
        return (bool)pedalState.getProperty(Nova::IDs::PEDAL_ENABLED, true);
    }

    juce::String getDisplayName() const
    {
        if (pedalState.isValid())
        {
            const auto type = pedalState.getProperty(Nova::IDs::PEDAL_TYPE).toString();
            if (type.isNotEmpty())
                return type;
        }
        return "Pedal";
    }

    void toggleBypass()
    {
        const int index = getPedalIndex();
        if (index >= 0)
            processor.requestBypassPedal(chain, index, isPedalEnabled());
    }

    void removePedal()
    {
        const int index = getPedalIndex();
        if (index >= 0)
            processor.requestRemovePedal(chain, index);
    }

    void refreshVisualState()
    {
        const bool enabled = isPedalEnabled();
        const auto accent = Nova::PedalCatalog::accentForType(getDisplayName());

        powerButton.setButtonText(enabled ? "ON" : "OFF");
        powerButton.setColour(juce::TextButton::buttonColourId,
            enabled ? accent.withSaturation(0.3f).withBrightness(0.2f)
                    : juce::Colour::fromString("ff3D1418"));
        powerButton.setColour(juce::TextButton::textColourOffId,
            enabled ? Nova::Colors::Success : Nova::Colors::Error.withAlpha(0.7f));

        removeButton.setButtonText("x");
        removeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        removeButton.setColour(juce::TextButton::textColourOffId, Nova::Colors::Error.withAlpha(0.65f));

        configButton.setColour(juce::TextButton::buttonColourId,
            accent.withSaturation(0.3f).withBrightness(0.15f));
        configButton.setColour(juce::TextButton::textColourOffId,
            juce::Colours::white.withAlpha(enabled ? 0.8f : 0.35f));
    }

    NOVAAudioProcessor& processor;
    Nova::ChainID chain;
    juce::ValueTree pedalState;
    std::function<void(Nova::ChainID, juce::String)> onOpenEditor;
    juce::TextButton powerButton;
    juce::TextButton removeButton;
    juce::TextButton configButton;
    bool hovered = false;
};

// ==============================================================================
// CONSTRUCTOR / INIT
// ==============================================================================

NOVAAudioProcessorEditor::NOVAAudioProcessorEditor(NOVAAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , audioProcessor(p)
{
    auto styleMetricLabel = [](juce::Label& label)
    {
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, Nova::Colors::Text.withAlpha(0.92f));
        label.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        label.setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
        label.setInterceptsMouseClicks(false, false);
    };

    auto styleRouteButton = [](juce::TextButton& button)
    {
        button.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff131A26"));
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromString("ff131A26"));
        button.setColour(juce::TextButton::textColourOffId, Nova::Colors::TextDim);
        button.setColour(juce::TextButton::textColourOnId, Nova::Colors::Text);
    };

    // -----------------------
    // HEADER
    // -----------------------
    addAndMakeVisible(btnStartStop);
    btnStartStop.setClickingTogglesState(true);
    btnStartStop.onClick = [this] { audioProcessor.toggleEngine(); };

    addAndMakeVisible(lblStats);
    lblStats.setJustificationType(juce::Justification::centred);
    lblStats.setColour(juce::Label::textColourId, Nova::Colors::TextDim);
    lblStats.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    lblStats.setText({}, juce::dontSendNotification);
    lblStats.setInterceptsMouseClicks(false, false);

    addAndMakeVisible(lblCpu);
    styleMetricLabel(lblCpu);
    lblCpu.setText("CPU USAGE\n--", juce::dontSendNotification);

    addAndMakeVisible(lblProc);
    styleMetricLabel(lblProc);
    lblProc.setText("PROCESS TIME\n--", juce::dontSendNotification);

    addAndMakeVisible(lblBuf);
    styleMetricLabel(lblBuf);
    lblBuf.setText("BUFFER TIME\n--", juce::dontSendNotification);

    addAndMakeVisible(btnTuner);

    // IMPORTANTE: esto estaba en resized() (redundante). Aquí queda 1 vez y ya.
    btnTuner.onClick = [this] { toggleTuner(); };
    btnTuner.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnTuner.setColour(juce::TextButton::textColourOffId, Nova::Colors::Success);
    btnTuner.setTooltip("Mute the output and open the tuner overlay");

    btnStartStop.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff2D1418"));
    btnStartStop.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.85f));

    // -----------------------
    // SIDEBAR DRAWERS
    // -----------------------
    leftDrawer.isLeftSide = true;
    leftDrawer.title = "QUICK ADD";
    addAndMakeVisible(leftDrawer);
    leftDrawer.setVisible(false);

    rightDrawer.isLeftSide = false;
    rightDrawer.title = "PRESETS";
    addAndMakeVisible(rightDrawer);
    rightDrawer.setVisible(false);

    addAndMakeVisible(btnToggleLeft);
    btnToggleLeft.setButtonText("+");
    btnToggleLeft.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff1A2332"));
    btnToggleLeft.setColour(juce::TextButton::textColourOffId, Nova::Colors::Accent);
    btnToggleLeft.setTooltip("Toggle effects browser");
    btnToggleLeft.onClick = [this] { toggleLeftPanel(); };

    addAndMakeVisible(btnToggleRight);
    btnToggleRight.setButtonText("PRESETS");
    btnToggleRight.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff1A2332"));
    btnToggleRight.setColour(juce::TextButton::textColourOffId, Nova::Colors::Accent);
    btnToggleRight.onClick = [this] { toggleRightPanel(); };

    addAndMakeVisible(btnSettings);
    btnSettings.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff1A2332"));
    btnSettings.setColour(juce::TextButton::textColourOffId, Nova::Colors::Accent);
    btnSettings.setTooltip("Open NOVA settings");
    btnSettings.onClick = [this] { openSettingsOverlay(); };

    // -----------------------
    // BROWSER (inside left drawer)
    // -----------------------
    leftDrawer.addAndMakeVisible(searchBarBrowser);
    searchBarBrowser.setTextToShowWhenEmpty("Filter quick-add modules...", juce::Colours::grey);
    searchBarBrowser.onTextChange = [this] { applyBrowserFilter(); };

    quickAddViewport.setScrollBarsShown(true, false);
    quickAddViewport.setScrollBarThickness(6);
    quickAddViewport.setViewedComponent(&quickAddContainer, false);
    leftDrawer.addAndMakeVisible(quickAddViewport);

    for (const auto& entry : Nova::PedalCatalog::entries())
    {
        auto btn = std::make_unique<DraggableButton>(entry.displayName);
        juce::String itemType;
        switch (entry.kind) {
            case Nova::PedalCatalog::Kind::Amplifier: itemType = "AMP"; break;
            case Nova::PedalCatalog::Kind::Cabinet:   itemType = "CAB"; break;
            default:                                   itemType = "PEDAL"; break;
        }
        setupQuickAddButton(*btn, entry.typeID, itemType);
        quickAddContainer.addAndMakeVisible(btn.get());
        quickAddButtons.push_back(std::move(btn));
    }
    // -----------------------
    // INPUT STRIP
    // -----------------------
    setupKnob(inputVolume, "GAIN", -60.0f, 24.0f, 0.0f);
    inputVolume.setTextValueSuffix(" dB");

    setupKnob(inputGate, "GATE", -100.0f, 0.0f, -100.0f);
    inputGate.setTextValueSuffix(" dB");

    setupKnob(inputTranspose, "TRANS", -12.0f, 12.0f, 0.0f);
    inputTranspose.setRange(-12.0, 12.0, 1.0);
    inputTranspose.setTextValueSuffix(" st");

    addAndMakeVisible(btnMonoStereo);
    btnMonoStereo.setButtonText("MONO");
    btnMonoStereo.setClickingTogglesState(true);
    btnMonoStereo.setColour(juce::ToggleButton::tickColourId, Nova::Colors::Accent);

    // -----------------------
    // MIXER & LANES
    // -----------------------
    laneA = std::make_unique<ChainLane>(p, Nova::ChainID::LineA);
    addAndMakeVisible(laneA.get());

    laneB = std::make_unique<ChainLane>(p, Nova::ChainID::LineB);
    addAndMakeVisible(laneB.get());

    addAndMakeVisible(btnSwitcher);
    btnSwitcher.setVisible(false);
    btnSwitcher.onClick = [this] { audioProcessor.cycleSwitcherWithMask(EditorPrefs::getSwitcherModes()); };

    addAndMakeVisible(btnRouteA);
    btnRouteA.setButtonText("LINE A");
    styleRouteButton(btnRouteA);
    btnRouteA.onClick = [this] { audioProcessor.setSwitcherMode(Nova::SwitcherMode::LineA_Only); };

    addAndMakeVisible(btnRoutePar);
    btnRoutePar.setButtonText("PARALLEL");
    styleRouteButton(btnRoutePar);
    btnRoutePar.onClick = [this] { audioProcessor.setSwitcherMode(Nova::SwitcherMode::Dual_Parallel); };

    addAndMakeVisible(btnRouteB);
    btnRouteB.setButtonText("LINE B");
    styleRouteButton(btnRouteB);
    btnRouteB.onClick = [this] { audioProcessor.setSwitcherMode(Nova::SwitcherMode::LineB_Only); };

    // Line A
    setupKnob(volSliderA, "LEVEL", 0.0f, 2.0f, 1.0f);
    volSliderA.setTextValueSuffix("x");

    setupKnob(panSliderA, "PAN", -1.0f, 1.0f, 0.0f);
    panSliderA.setLookAndFeel(studioTrimLnf);

    setupKnob(widthSliderA, "WIDTH", 0.0f, 2.0f, 1.0f);
    widthSliderA.setTextValueSuffix("x");

    // Line B
    setupKnob(volSliderB, "LEVEL", 0.0f, 2.0f, 1.0f);
    volSliderB.setTextValueSuffix("x");

    setupKnob(panSliderB, "PAN", -1.0f, 1.0f, 0.0f);
    panSliderB.setLookAndFeel(studioTrimLnf);

    setupKnob(widthSliderB, "WIDTH", 0.0f, 2.0f, 1.0f);
    widthSliderB.setTextValueSuffix("x");

    // -----------------------
    // OUTPUT STRIP
    // -----------------------
    setupKnob(outputVolume, "MASTER", -60.0f, 12.0f, 0.0f);
    outputVolume.setTextValueSuffix(" dB");

    setupKnob(outputGain, "LIMIT", -20.0f, 0.0f, 0.0f);
    outputGain.setLookAndFeel(studioTrimLnf);
    outputGain.setTextValueSuffix(" dB");

    setupKnob(outputMix, "MIX", 0.0f, 100.0f, 100.0f);
    outputMix.setTextValueSuffix("%");

    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::INPUT_GAIN.toString()))
        inputVolumeAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, inputVolume);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::INPUT_GATE.toString()))
        inputGateAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, inputGate);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::INPUT_TRANS.toString()))
        inputTransposeAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, inputTranspose);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::FORCE_MONO.toString()))
        monoAttachment = std::make_unique<juce::ButtonParameterAttachment>(*param, btnMonoStereo);

    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_GAIN_A.toString()))
        volAAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, volSliderA);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_PAN_A.toString()))
        panAAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, panSliderA);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_WIDTH_A.toString()))
        widthAAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, widthSliderA);

    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_GAIN_B.toString()))
        volBAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, volSliderB);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_PAN_B.toString()))
        panBAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, panSliderB);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_WIDTH_B.toString()))
        widthBAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, widthSliderB);

    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::OUTPUT_VOL.toString()))
        outputVolumeAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, outputVolume);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::OUTPUT_LIMITER.toString()))
        outputLimiterAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, outputGain);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::OUTPUT_MIX.toString()))
        outputMixAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, outputMix);

    // -----------------------
    // PRESETS (inside right drawer)
    // -----------------------
    rightDrawer.addAndMakeVisible(searchBarPresets);
    searchBarPresets.setTextToShowWhenEmpty("Filter...", juce::Colours::grey);
    searchBarPresets.onTextChange = [this] { refreshPresetList(); };

    rightDrawer.addAndMakeVisible(lblCurrentPreset);
    lblCurrentPreset.setJustificationType(juce::Justification::centred);
    lblCurrentPreset.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    lblCurrentPreset.setFont(juce::Font(juce::FontOptions(12.0f)));
    setCurrentPreset("No Preset");

    rightDrawer.addAndMakeVisible(presetSelector);
    presetSelector.setTextWhenNothingSelected("No Presets");
    presetSelector.onChange = [] {};

    rightDrawer.addAndMakeVisible(btnSave);
    btnSave.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnSave.onClick = [this] { saveSelectedOrPromptPreset(); };

    rightDrawer.addAndMakeVisible(btnLoad);
    btnLoad.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnLoad.onClick = [this] { loadSelectedPreset(); };

    rightDrawer.addAndMakeVisible(btnClear);
    btnClear.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff3D1418"));
    btnClear.onClick = [this] { clearPresetAndSession(); };

    addAndMakeVisible(audioProcessor.audioVisualizer);

    // -----------------------
    // INIT LOGIC
    // -----------------------
    audioProcessor.pluginState.addListener(this);

    setResizable(true, true);
    setSize(1920, 1080);

    // Valores iniciales
    syncControlsFromState();

    const auto startupMode = EditorPrefs::getString(EditorPrefs::startupModeKey, "Remember last context");
    const auto startupPointer = getStartupPresetPointerFile();

    if (startupMode == "Open clean rig")
    {
        setCurrentPreset("No Preset");
    }
    else if (startupPointer.existsAsFile())
    {
        const auto presetPath = startupPointer.loadFileAsString().trim();
        const auto startupPreset = juce::File(presetPath);
        if (startupPreset.existsAsFile())
            setCurrentPreset(startupPreset.getFileNameWithoutExtension());
        else
            setCurrentPreset("No Preset");
    }
    else
    {
        setCurrentPreset("No Preset");
    }

    refreshPresetList();
    applyBrowserFilter();

    updateSwitcherState();
    updatePedalGui();

    showPerformanceStats = EditorPrefs::getBool(EditorPrefs::showStatsKey, true);
    openQuickAddOnStartup = EditorPrefs::getBool(EditorPrefs::openBrowserOnStartupKey, false);
    openPresetsOnStartup = EditorPrefs::getBool(EditorPrefs::openPresetsOnStartupKey, false);
    audioProcessor.getAudioEngine().setTunerReferencePitch(EditorPrefs::parseTunerReference());
    applyEditorPreferences(true);

    statsTimer = std::make_unique<StatsTimer>(*this);
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
    audioProcessor.pluginState.removeListener(this);
    cancelPendingUpdate();
    activePedalEditors.clear();

    for (auto* knob : { &inputVolume, &inputGate, &inputTranspose,
        &volSliderA, &panSliderA, &widthSliderA,
        &volSliderB, &panSliderB, &widthSliderB,
        &outputVolume, &outputGain, &outputMix })
        knob->setLookAndFeel(nullptr);
}

// ==============================================================================
// WIDGET HELPERS
// ==============================================================================

void NOVAAudioProcessorEditor::setupKnob(juce::Slider& slider,
    const juce::String& name,
    float min,
    float max,
    float def)
{
    addAndMakeVisible(slider);
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 14);
    slider.setRotaryParameters(UI::KnobGeometry::knobStartAngleRadians(),
        UI::KnobGeometry::knobEndAngleRadians(),
        true);
    slider.setLookAndFeel(knobLnf);
    slider.setRange(min, max, 0.01);
    slider.setValue(def, juce::dontSendNotification);
    slider.setTooltip(name);
    slider.setName(name);
}

void NOVAAudioProcessorEditor::setupQuickAddButton(DraggableButton& button,
    const juce::String& typeID,
    const juce::String& itemType)
{
    button.setButtonText(typeID);
    button.setItemType(itemType);
    button.setAccentColour(Nova::PedalCatalog::accentForType(typeID));
    button.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    button.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.86f));
    button.setTooltip(Nova::PedalCatalog::subtitleForType(typeID));
}

void NOVAAudioProcessorEditor::applyBrowserFilter()
{
    const auto filter = searchBarBrowser.getText().trim();
    const auto& catalog = Nova::PedalCatalog::entries();

    for (size_t i = 0; i < quickAddButtons.size() && i < catalog.size(); ++i)
    {
        const bool matches = Nova::PedalCatalog::matchesFilter(catalog[i], filter);
        quickAddButtons[i]->setVisible(matches);
        quickAddButtons[i]->setEnabled(matches);
    }

    layoutQuickAddButtons();
    quickAddViewport.repaint();
}

void NOVAAudioProcessorEditor::layoutQuickAddButtons()
{
    constexpr int buttonH = 46;
    constexpr int buttonGap = 10;
    const int contentW = quickAddViewport.getWidth() - quickAddViewport.getScrollBarThickness() - 4;
    const int buttonW = juce::jmax(80, contentW - 12);

    int y = 6;
    for (auto& btn : quickAddButtons)
    {
        if (!btn->isVisible())
            continue;

        btn->setBounds(6, y, buttonW, buttonH);
        y += buttonH + buttonGap;
    }

    quickAddContainer.setSize(quickAddViewport.getWidth(), juce::jmax(y, quickAddViewport.getHeight()));
}

juce::Rectangle<int> NOVAAudioProcessorEditor::getInputStripBounds() const
{
    return { 0, Nova::Config::HEADER_HEIGHT, 80,
        juce::jmax(0, getHeight() - Nova::Config::HEADER_HEIGHT - Nova::Config::FOOTER_HEIGHT) };
}

juce::Rectangle<int> NOVAAudioProcessorEditor::getOutputStripBounds() const
{
    return { juce::jmax(0, getWidth() - 80), Nova::Config::HEADER_HEIGHT, 80,
        juce::jmax(0, getHeight() - Nova::Config::HEADER_HEIGHT - Nova::Config::FOOTER_HEIGHT) };
}

void NOVAAudioProcessorEditor::updateMeterState()
{
    auto advanceMeter = [](float rawPeak, float& displayed, float& hold)
    {
        const float clampedPeak = juce::jlimit(0.0f, 1.2f, rawPeak);

        if (clampedPeak > displayed)
            displayed += (clampedPeak - displayed) * 0.55f;
        else
            displayed = clampedPeak + (displayed - clampedPeak) * 0.82f;

        displayed = juce::jlimit(0.0f, 1.2f, displayed);
        hold = juce::jmax(clampedPeak, juce::jmax(0.0f, hold - 0.018f));
    };

    advanceMeter(audioProcessor.getInputPeak(), inputMeterDisplay, inputMeterHold);
    advanceMeter(audioProcessor.getOutputPeak(), outputMeterDisplay, outputMeterHold);

    const auto inputStrip = getInputStripBounds();
    if (!inputStrip.isEmpty())
        repaint(inputStrip);

    const auto outputStrip = getOutputStripBounds();
    if (!outputStrip.isEmpty())
        repaint(outputStrip);
}

juce::File NOVAAudioProcessorEditor::getPresetDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("NOVA")
        .getChildFile("Presets");

    if (!dir.exists())
        dir.createDirectory();

    return dir;
}

juce::File NOVAAudioProcessorEditor::getPresetFileForName(const juce::String& presetName) const
{
    auto safe = presetName.trim();
    safe = safe.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_");

    if (safe.isEmpty())
        safe = "Preset";

    return getPresetDirectory().getChildFile(safe + ".nova-preset");
}

void NOVAAudioProcessorEditor::setCurrentPreset(const juce::String& presetName)
{
    currentPresetName = presetName.trim().isEmpty() ? "No Preset" : presetName.trim();
    const auto label = (currentPresetName == "No Preset")
        ? juce::String("No Preset")
        : juce::String("Preset: ") + currentPresetName;
    lblCurrentPreset.setText(label, juce::dontSendNotification);
}

void NOVAAudioProcessorEditor::refreshPresetList()
{
    ensureBundledDelayPresets(getPresetDirectory());

    juce::Array<juce::File> found;
    getPresetDirectory().findChildFiles(found, juce::File::TypesOfFileToFind::findFiles, false, "*.nova-preset");

    std::vector<juce::File> sorted;
    sorted.reserve((size_t)found.size());
    for (auto& f : found)
        sorted.push_back(f);

    std::sort(sorted.begin(), sorted.end(),
        [](const juce::File& a, const juce::File& b)
        {
            return a.getFileNameWithoutExtension().compareNatural(b.getFileNameWithoutExtension()) < 0;
        });

    const auto filter = searchBarPresets.getText().trim();

    presetFiles.clear();
    for (const auto& f : sorted)
    {
        const auto name = f.getFileNameWithoutExtension();
        if (filter.isNotEmpty() && !name.containsIgnoreCase(filter))
            continue;

        presetFiles.push_back(f);
    }

    presetSelector.clear(juce::dontSendNotification);

    int selectedId = 0;
    for (size_t i = 0; i < presetFiles.size(); ++i)
    {
        const auto name = presetFiles[i].getFileNameWithoutExtension();
        presetSelector.addItem(name, (int)i + 1);

        if (name == currentPresetName)
            selectedId = (int)i + 1;
    }

    if (selectedId > 0)
        presetSelector.setSelectedId(selectedId, juce::dontSendNotification);
    else
        presetSelector.setSelectedId(0, juce::dontSendNotification);

    presetSelector.setTextWhenNothingSelected(presetFiles.empty() ? "No Presets" : "Select Preset");
}

void NOVAAudioProcessorEditor::syncControlsFromState()
{
}

void NOVAAudioProcessorEditor::savePresetWithName(const juce::String& presetName)
{
    const auto target = getPresetFileForName(presetName);
    if (!audioProcessor.savePresetToFile(target))
        return;

    setCurrentPreset(target.getFileNameWithoutExtension());
    refreshPresetList();
}

void NOVAAudioProcessorEditor::saveSelectedOrPromptPreset()
{
    juce::String suggestedName;
    const int idx = presetSelector.getSelectedId() - 1;

    if (juce::isPositiveAndBelow(idx, (int)presetFiles.size()))
        suggestedName = presetFiles[(size_t)idx].getFileNameWithoutExtension();
    else if (currentPresetName != "No Preset")
        suggestedName = currentPresetName;

    auto* alert = new juce::AlertWindow("Save Preset", "Preset name", juce::AlertWindow::NoIcon);
    alert->addTextEditor("name", suggestedName, "Name");
    alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<NOVAAudioProcessorEditor> safeThis(this);
    alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safeThis, alert](int result)
        {
            if (safeThis == nullptr || result != 1)
                return;

            const auto name = alert->getTextEditorContents("name").trim();
            if (name.isEmpty())
                return;

            safeThis->savePresetWithName(name);
        }), true);
}

void NOVAAudioProcessorEditor::loadSelectedPreset()
{
    const int idx = presetSelector.getSelectedId() - 1;
    if (!juce::isPositiveAndBelow(idx, (int)presetFiles.size()))
        return;

    const auto& target = presetFiles[(size_t)idx];
    if (!audioProcessor.loadPresetFromFile(target))
        return;

    setCurrentPreset(target.getFileNameWithoutExtension());
    syncControlsFromState();
    refreshPresetList();
    updateSwitcherState();
    updatePedalGui();
    repaint();
}

void NOVAAudioProcessorEditor::clearPresetAndSession()
{
    const auto performClear = [this]()
    {
        audioProcessor.clearSessionAndForgetStartupPreset();

        setCurrentPreset("No Preset");
        presetSelector.setSelectedId(0, juce::dontSendNotification);
        syncControlsFromState();
        refreshPresetList();
        updateSwitcherState();
        updatePedalGui();
        repaint();
    };

    if (!EditorPrefs::getBool(EditorPrefs::confirmBeforeClearKey, true))
    {
        performClear();
        return;
    }

    auto* alert = new juce::AlertWindow("Clear Current Rig",
        "This will remove the current preset selection and clear the active session state.",
        juce::AlertWindow::WarningIcon);
    alert->addButton("Clear", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<NOVAAudioProcessorEditor> safeThis(this);
    alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safeThis, performClear](int result)
        {
            if (safeThis == nullptr || result != 1)
                return;

            performClear();
        }), true);
}


// ==============================================================================
// PAINT
// ==============================================================================

void NOVAAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(Nova::Colors::Background);
    auto area = getLocalBounds();
    const bool on = audioProcessor.isEngineOn();
    const int mode = (int)audioProcessor.getSwitcherMode();

    // Header
    auto headerRect = area.removeFromTop(80);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(headerRect);

    g.setColour(Nova::Colors::Border);
    g.drawHorizontalLine(headerRect.getBottom(), 0, (float)getWidth());

    const auto fullHeader = headerRect;

    auto drawRouteButtonGlow = [&](const juce::TextButton& button, bool active, juce::Colour colour)
    {
        if (!on || !active || !button.isVisible() || button.getBounds().isEmpty())
            return;

        const auto buttonBounds = button.getBounds().toFloat();
        g.setColour(colour.withAlpha(0.12f));
        g.fillRoundedRectangle(buttonBounds.expanded(12.0f, 8.0f), 12.0f);
        g.setColour(colour.withAlpha(0.18f));
        g.fillRoundedRectangle(buttonBounds.expanded(7.0f, 4.0f), 10.0f);
        g.setColour(colour.withAlpha(0.42f));
        g.drawRoundedRectangle(buttonBounds.expanded(3.0f, 2.0f), 8.0f, 1.2f);
    };

    auto drawDashboardGlow = [&](juce::Rectangle<int> bounds, juce::Colour colour, float fillAlpha)
    {
        if (!on || bounds.isEmpty())
            return;

        const auto glowBounds = bounds.toFloat();
        g.setColour(colour.withAlpha(fillAlpha * 0.35f));
        g.fillRoundedRectangle(glowBounds.expanded(20.0f, 14.0f), 26.0f);
        g.setColour(colour.withAlpha(fillAlpha * 0.60f));
        g.fillRoundedRectangle(glowBounds.expanded(10.0f, 7.0f), 22.0f);
        g.setColour(colour.withAlpha(fillAlpha));
        g.fillRoundedRectangle(glowBounds.reduced(2.0f), 18.0f);
        g.setColour(colour.withAlpha(fillAlpha * 1.35f));
        g.drawRoundedRectangle(glowBounds.reduced(1.0f), 18.0f, 1.4f);
    };

    drawRouteButtonGlow(btnRouteA, mode == (int)Nova::SwitcherMode::LineA_Only, Nova::Colors::CableOnA);
    drawRouteButtonGlow(btnRoutePar, mode == (int)Nova::SwitcherMode::Dual_Parallel, Nova::Colors::Accent);
    drawRouteButtonGlow(btnRouteB, mode == (int)Nova::SwitcherMode::LineB_Only, Nova::Colors::CableOnB);

    g.setColour(Nova::Colors::Accent);
    g.setFont(juce::Font(juce::FontOptions(30.0f, juce::Font::bold)));
    auto logoArea = fullHeader.withTrimmedLeft(64).removeFromLeft(180);
    g.drawText("NOVA", logoArea, juce::Justification::centredLeft);
    g.setColour(Nova::Colors::TextDim);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    auto taglineArea = fullHeader.withTrimmedLeft(190).removeFromLeft(220);
    g.drawText("Guitar Rig Designer", taglineArea, juce::Justification::centredLeft);

    // Footer
    auto footerRect = area.removeFromBottom(100);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(footerRect);
    g.setColour(Nova::Colors::Border);
    g.drawHorizontalLine(footerRect.getY(), 0, (float)getWidth());

    // Input strip (left edge)
    auto left2 = area.removeFromLeft(80);
    drawChannelStrip(g, left2, "INPUT");

    // Output strip (right edge)
    auto right1 = area.removeFromRight(80);
    drawChannelStrip(g, right1, "OUTPUT");

    if (mode == (int)Nova::SwitcherMode::LineA_Only)
    {
        if (laneA && laneA->isVisible())
            drawDashboardGlow(laneA->getBounds(), Nova::Colors::CableOnA, 0.085f);
    }
    else if (mode == (int)Nova::SwitcherMode::LineB_Only)
    {
        if (laneB && laneB->isVisible())
            drawDashboardGlow(laneB->getBounds(), Nova::Colors::CableOnB, 0.085f);
    }
    else
    {
        if (laneA && laneA->isVisible())
            drawDashboardGlow(laneA->getBounds(), Nova::Colors::CableOnA, 0.055f);
        if (laneB && laneB->isVisible())
            drawDashboardGlow(laneB->getBounds(), Nova::Colors::CableOnB, 0.055f);
        if (laneA && laneB && laneA->isVisible() && laneB->isVisible())
            drawDashboardGlow(laneA->getBounds().getUnion(laneB->getBounds()).expanded(4, 4),
                Nova::Colors::Accent, 0.035f);
    }

    auto waveBounds = audioProcessor.audioVisualizer.getBounds().toFloat();
    if (!waveBounds.isEmpty())
    {
        g.setColour(juce::Colour::fromString("ff0F1520"));
        g.fillRoundedRectangle(waveBounds.expanded(6.0f, 4.0f), 10.0f);
        g.setColour(Nova::Colors::Border.withAlpha(0.85f));
        g.drawRoundedRectangle(waveBounds.expanded(6.0f, 4.0f), 10.0f, 1.0f);
    }

    juce::Rectangle<int> metricsBounds;

    if (showPerformanceStats)
    {
        metricsBounds = lblCpu.getBounds()
            .getUnion(lblProc.getBounds())
            .getUnion(lblBuf.getBounds());
    }

    if (!lblStats.getText().isEmpty())
        metricsBounds = metricsBounds.isEmpty() ? lblStats.getBounds() : metricsBounds.getUnion(lblStats.getBounds());

    auto metricsBoundsF = metricsBounds.expanded(12, 8).toFloat();

    if (!metricsBoundsF.isEmpty())
    {
        juce::ColourGradient metricsFill(juce::Colour::fromString("ff141C28"),
            metricsBoundsF.getCentreX(), metricsBoundsF.getY(),
            juce::Colour::fromString("ff101620"),
            metricsBoundsF.getCentreX(), metricsBoundsF.getBottom(), false);
        g.setGradientFill(metricsFill);
        g.fillRoundedRectangle(metricsBoundsF, 12.0f);
        g.setColour(Nova::Colors::Border.withAlpha(0.8f));
        g.drawRoundedRectangle(metricsBoundsF, 12.0f, 1.0f);

        if (showPerformanceStats)
        {
            const auto cpuBounds = lblCpu.getBounds();
            const auto procBounds = lblProc.getBounds();
            const auto bufBounds = lblBuf.getBounds();
            const int sepTop = juce::jmin(cpuBounds.getY(), juce::jmin(procBounds.getY(), bufBounds.getY())) + 4;
            const int sepBottom = juce::jmax(cpuBounds.getBottom(), juce::jmax(procBounds.getBottom(), bufBounds.getBottom())) - 4;

            g.setColour(Nova::Colors::Border.withAlpha(0.5f));
            g.drawVerticalLine(cpuBounds.getRight() + 4, (float)sepTop, (float)sepBottom);
            g.drawVerticalLine(procBounds.getRight() + 4, (float)sepTop, (float)sepBottom);
        }

        if (!lblStats.getText().isEmpty())
        {
            g.setColour(Nova::Colors::Error.withAlpha(0.14f));
            g.fillRoundedRectangle(lblStats.getBounds().toFloat().expanded(4.0f, 2.0f), 6.0f);
        }
    }

    // Mixer area
    const bool dualModePaint = (mode == (int)Nova::SwitcherMode::Dual_Parallel);
    const int paintMixerH = dualModePaint ? 0 : 170;

    auto center = area;
    auto mixerArea = center.removeFromBottom(paintMixerH);

    const bool aActive = on && (mode != (int)Nova::SwitcherMode::LineB_Only);
    const bool bActive = on && (mode != (int)Nova::SwitcherMode::LineA_Only);
    const bool dualMode = dualModePaint;

    if (paintMixerH > 0)
    {
        g.setColour(Nova::Colors::MixerPanel);
        g.drawRoundedRectangle(mixerArea.toFloat().reduced(10), 5.0f, 1.0f);
    }

    // Knob name labels — drawn above each visible knob using its actual bounds
    auto drawKnobLabel = [&](const juce::String& text, const juce::Slider& knob)
    {
        if (!knob.isVisible()) return;
        const auto b = knob.getBounds();
        g.drawText(text, b.getX(), b.getY() - 14, b.getWidth(), 14, juce::Justification::centred);
    };

    g.setColour(Nova::Colors::Text);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));

    if (dualMode)
    {
        // No bottom mixer bar in parallel mode.
    }
    else
    {
        const juce::String lineLabel = (mode == (int)Nova::SwitcherMode::LineA_Only) ? "LINE A" : "LINE B";
        const auto labelColour = (mode == (int)Nova::SwitcherMode::LineA_Only) ? Nova::Colors::CableOnA : Nova::Colors::CableOnB;
        g.setColour(on ? labelColour : Nova::Colors::TextDim);
        g.drawText(lineLabel, mixerArea.getX(), mixerArea.getY() + 10, mixerArea.getWidth(), 20, juce::Justification::centred);
    }

    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.setColour(Nova::Colors::TextDim);

    if (!dualMode)
    {
        drawKnobLabel("LEVEL", volSliderA);
        drawKnobLabel("PAN", panSliderA);
        drawKnobLabel("WIDTH", widthSliderA);
        drawKnobLabel("LEVEL", volSliderB);
        drawKnobLabel("PAN", panSliderB);
        drawKnobLabel("WIDTH", widthSliderB);
    }

    // --- Routing circuit diagram (only in dual mode) ---
    if (dualMode && paintMixerH > 0)
    {
        const int leftGroupRight  = widthSliderA.getRight();
        const int rightGroupLeft  = volSliderB.getX();

        auto circuitZone = juce::Rectangle<int>(leftGroupRight + 8,
            mixerArea.getY() + 12,
            juce::jmax(40, rightGroupLeft - leftGroupRight - 16),
            mixerArea.getHeight() - 24);

        g.setColour(juce::Colour::fromString("ff0B0E14"));
        g.fillRoundedRectangle(circuitZone.toFloat(), 10.0f);
        g.setColour(Nova::Colors::Border);
        g.drawRoundedRectangle(circuitZone.toFloat(), 10.0f, 1.0f);

        const float xC = circuitZone.toFloat().getCentreX();
        const float cTop = circuitZone.toFloat().getY() + 16.0f;
        const float cBot = circuitZone.toFloat().getBottom() - 16.0f;
        const float cLeft  = circuitZone.toFloat().getX() + 12.0f;
        const float cRight = circuitZone.toFloat().getRight() - 12.0f;
        const float switchY = circuitZone.toFloat().getCentreY();

        const float lineAY = cTop + 10.0f;
        const float lineBY = cBot - 10.0f;

        auto drawSignalLineOffset = [&](float y, float tapOffsetX, bool active, juce::Colour activeColour)
            {
                const juce::Colour offColour = Nova::Colors::CableOff;
                const float tapX = xC + tapOffsetX;

                if (active)
                {
                    g.setColour(activeColour.withAlpha(0.2f));
                    g.drawLine(cLeft, y, cRight, y, 8.0f);
                    g.setColour(activeColour);
                    g.drawLine(cLeft, y, cRight, y, 2.4f);
                }
                else
                {
                    g.setColour(offColour);
                    g.drawLine(cLeft, y, cRight, y, 2.4f);
                }

                if (active)
                {
                    g.setColour(activeColour.withAlpha(0.2f));
                    g.drawLine(tapX, y, tapX, switchY, 8.0f);
                    g.setColour(activeColour);
                    g.drawLine(tapX, y, tapX, switchY, 2.4f);
                }
                else
                {
                    g.setColour(offColour);
                    g.drawLine(tapX, y, tapX, switchY, 2.0f);
                }

                g.setColour(active ? activeColour : offColour);
                g.fillEllipse(cLeft - 3.5f, y - 3.5f, 7.0f, 7.0f);
                g.fillEllipse(cRight - 3.5f, y - 3.5f, 7.0f, 7.0f);
                g.fillEllipse(tapX - 3.0f, y - 3.0f, 6.0f, 6.0f);
            };

        drawSignalLineOffset(lineAY, -12.0f, aActive, Nova::Colors::CableOnA);
        drawSignalLineOffset(lineBY,  12.0f, bActive, Nova::Colors::CableOnB);

        g.setColour(juce::Colour::fromString("ff1A2332"));
        g.fillEllipse(xC - 14.0f, switchY - 14.0f, 28.0f, 28.0f);
        g.setColour(Nova::Colors::Border);
        g.drawEllipse(xC - 14.0f, switchY - 14.0f, 28.0f, 28.0f, 1.2f);

        if (aActive && bActive)
            g.setColour(Nova::Colors::Accent);
        else if (aActive)
            g.setColour(Nova::Colors::CableOnA);
        else if (bActive)
            g.setColour(Nova::Colors::CableOnB);
        else
            g.setColour(Nova::Colors::CableOff);

        g.fillEllipse(xC - 6.0f, switchY - 6.0f, 12.0f, 12.0f);

        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.setColour(aActive ? Nova::Colors::CableOnA : Nova::Colors::TextDim);
        g.drawText("A", (int)(cLeft - 2.0f), (int)(lineAY - 18.0f), 14, 12, juce::Justification::centred);
        g.setColour(bActive ? Nova::Colors::CableOnB : Nova::Colors::TextDim);
        g.drawText("B", (int)(cLeft - 2.0f), (int)(lineBY + 6.0f), 14, 12, juce::Justification::centred);
    }

    if (!routeInfoIconBounds.isEmpty())
    {
        g.setColour(routeInfoHovered ? juce::Colours::white.withAlpha(0.92f) : Nova::Colors::TextDim.withAlpha(0.8f));
        g.drawEllipse(routeInfoIconBounds, 1.0f);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("i", routeInfoIconBounds.toNearestInt(), juce::Justification::centred);

        if (routeInfoHovered)
        {
            const int popupW = 260;
            const int popupH = 56;
            const int popupX = juce::jmin(getWidth() - popupW - 16, (int)routeInfoIconBounds.getRight() + 10);
            const int popupY = juce::jmax(fullHeader.getBottom() + 6, (int)routeInfoIconBounds.getCentreY() - popupH / 2);
            auto popup = juce::Rectangle<float>((float)popupX, (float)popupY, (float)popupW, (float)popupH);

            g.setColour(juce::Colours::black.withAlpha(0.25f));
            g.fillRoundedRectangle(popup.translated(0.0f, 2.0f), 10.0f);
            g.setColour(juce::Colour::fromString("ff141C28"));
            g.fillRoundedRectangle(popup, 10.0f);
            g.setColour(Nova::Colors::Border.withAlpha(0.9f));
            g.drawRoundedRectangle(popup, 10.0f, 1.0f);
            g.setColour(Nova::Colors::Text.withAlpha(0.94f));
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawFittedText(routeInfoText, popup.reduced(10.0f, 8.0f).toNearestInt(), juce::Justification::centredLeft, 3);
        }
    }
}

void NOVAAudioProcessorEditor::drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    const auto originalArea = area;
    g.setColour(Nova::Colors::Background);
    g.fillRect(area);

    g.setColour(Nova::Colors::Border);
    g.drawVerticalLine(area.getRight(), (float)area.getY(), (float)area.getBottom());
    g.drawVerticalLine(area.getX(), (float)area.getY(), (float)area.getBottom());

    g.setColour(Nova::Colors::Text);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    g.drawText(title, area.removeFromTop(32), juce::Justification::centred);

    // Thin vertical meter at right edge
    const bool isInput = (title == "INPUT");
    const float displayPeak = isInput ? inputMeterDisplay : outputMeterDisplay;
    const float holdPeak = isInput ? inputMeterHold : outputMeterHold;
    const float peakDb = juce::Decibels::gainToDecibels(juce::jmax(displayPeak, 0.000001f), -60.0f);
    const float holdDb = juce::Decibels::gainToDecibels(juce::jmax(holdPeak, 0.000001f), -60.0f);
    const float meterNorm = juce::jlimit(0.0f, 1.0f, (peakDb + 60.0f) / 60.0f);
    const float holdNorm = juce::jlimit(0.0f, 1.0f, (holdDb + 60.0f) / 60.0f);

    const int controlsBottom = isInput
        ? juce::jmax(inputVolume.getBottom(),
            juce::jmax(inputGate.getBottom(), juce::jmax(inputTranspose.getBottom(), btnMonoStereo.getBottom())))
        : juce::jmax(outputVolume.getBottom(), juce::jmax(outputGain.getBottom(), outputMix.getBottom()));

    const int meterTop = juce::jmax(originalArea.getY() + 42, controlsBottom + 14);
    const int meterBottom = juce::jmin(originalArea.getBottom() - 18, meterTop + 150);
    const int meterHeight = juce::jmax(0, meterBottom - meterTop);

    if (meterHeight < 24)
        return;

    auto meterArea = juce::Rectangle<float>((float)(originalArea.getCentreX() - 6),
        (float)meterTop,
        12.0f,
        (float)meterHeight);

    g.setColour(juce::Colour::fromString("ff0A1018"));
    g.fillRoundedRectangle(meterArea, 5.0f);
    g.setColour(Nova::Colors::Border.withAlpha(0.45f));
    g.drawRoundedRectangle(meterArea, 5.0f, 0.8f);

    const float redZoneTop = meterArea.getY() + meterArea.getHeight() * 0.12f;
    g.setColour(Nova::Colors::Error.withAlpha(0.10f));
    g.fillRoundedRectangle(juce::Rectangle<float>(meterArea.getX(), redZoneTop, meterArea.getWidth(), meterArea.getBottom() - redZoneTop), 5.0f);

    if (meterNorm > 0.0f)
    {
        auto fill = meterArea.withY(meterArea.getBottom() - meterArea.getHeight() * meterNorm).reduced(0.5f, 0.5f);
        fill.setHeight(juce::jmax(3.0f, meterArea.getHeight() * meterNorm - 1.0f));

        juce::ColourGradient meterGrad(Nova::Colors::Success, fill.getCentreX(), fill.getBottom(),
            juce::Colour::fromString("ffFBBF24"), fill.getCentreX(), fill.getCentreY(), false);
        meterGrad.addColour(0.35, juce::Colour::fromString("ffF59E0B"));
        meterGrad.addColour(0.0, displayPeak > 0.97f ? Nova::Colors::Error : juce::Colour::fromString("ffF97316"));
        g.setGradientFill(meterGrad);
        g.fillRoundedRectangle(fill, 3.0f);
    }

    if (holdNorm > 0.0f)
    {
        const float holdY = meterArea.getBottom() - meterArea.getHeight() * holdNorm;
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawLine(meterArea.getX(), holdY, meterArea.getRight(), holdY, 1.2f);
    }

    // Knob labels
    auto drawLabel = [&](const juce::String& txt, const juce::Component& c)
    {
        const auto b = c.getBounds();
        if (!b.isEmpty())
        {
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            g.drawText(txt.toUpperCase(), b.getX() - 4, b.getY() - 12, b.getWidth() + 8, 12, juce::Justification::centred);
        }
    };

    if (isInput)
    {
        drawLabel("Gain", inputVolume);
        drawLabel("Gate", inputGate);
        drawLabel("Trans", inputTranspose);
    }
    else
    {
        drawLabel("Master", outputVolume);
        drawLabel("Limit", outputGain);
        drawLabel("Mix", outputMix);
    }
}

// ==============================================================================
// RESIZED (layout only)
// ==============================================================================

void NOVAAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    if (currentOverlay) currentOverlay->setBounds(area);
    if (tunerOverlay && tunerOverlay->isVisible()) tunerOverlay->setBounds(area);

    constexpr int headerH = 80;
    constexpr int footerH = 100;
    constexpr int stripW = 80;
    constexpr int drawerW = 220;

    constexpr int knobSz = 60;
    constexpr int knobH = 78;
    constexpr int knobGap = 10;

    // Header
    auto header = area.removeFromTop(headerH);
    const int headerCentreX = header.getCentreX();
    const int tunerW = 42;
    const int tunerH = 28;
    const int powerW = 132;
    const int powerH = 28;
    const int routeH = 30;
    const int routeButtonW = 84;
    const int routeButtonGap = 6;
    const int clusterGap = 16;
    const int infoSize = 16;
    const int infoGap = 10;
    const int rowY = header.getCentreY() - routeH / 2;

    // Dynamic route group width based on visible buttons
    const int enabledModes = EditorPrefs::getSwitcherModes();
    const int visibleRouteCount = ((enabledModes >> 0) & 1) + ((enabledModes >> 1) & 1) + ((enabledModes >> 2) & 1);
    const int routeGroupW = routeButtonW * visibleRouteCount
        + routeButtonGap * juce::jmax(0, visibleRouteCount - 1);

    const int clusterW = tunerW + clusterGap + routeGroupW + infoGap + infoSize + clusterGap + powerW;
    int clusterX = headerCentreX - clusterW / 2;

    btnToggleLeft.setBounds(header.getX() + 10, header.getCentreY() - 15, 36, 30);
    btnSettings.setBounds(header.getRight() - 190, header.getCentreY() - 15, 92, 30);
    btnToggleRight.setBounds(header.getRight() - 92, header.getCentreY() - 15, 82, 30);
    btnTuner.setBounds(clusterX, rowY + (routeH - tunerH) / 2, tunerW, tunerH);
    clusterX += tunerW + clusterGap;

    // Only layout visible route buttons — clear bounds on hidden ones
    juce::TextButton* routeButtons[] = { &btnRouteA, &btnRoutePar, &btnRouteB };
    for (int i = 0; i < 3; ++i)
    {
        if (EditorPrefs::isModeEnabled(enabledModes, static_cast<Nova::SwitcherMode>(i)))
        {
            routeButtons[i]->setVisible(true);
            routeButtons[i]->setBounds(clusterX, rowY, routeButtonW, routeH);
            clusterX += routeButtonW + routeButtonGap;
        }
        else
        {
            routeButtons[i]->setVisible(false);
            routeButtons[i]->setBounds(0, 0, 0, 0);
        }
    }
    clusterX += infoGap - routeButtonGap; // adjust: last button has no trailing gap

    routeInfoIconBounds = juce::Rectangle<float>((float)clusterX,
        (float)(header.getCentreY() - infoSize / 2),
        (float)infoSize,
        (float)infoSize);
    clusterX += infoSize + clusterGap;

    btnStartStop.setBounds(clusterX, rowY + (routeH - powerH) / 2, powerW, powerH);

    // Footer
    auto footer = area.removeFromBottom(footerH);
    auto footerContent = footer.reduced(18, 12);
    auto waveArea = footerContent.removeFromLeft(juce::roundToInt((float)footerContent.getWidth() * 0.58f));
    waveArea.removeFromRight(16);
    audioProcessor.audioVisualizer.setBounds(waveArea.reduced(0, 6));

    auto statsArea = footerContent.reduced(8, 6);
    auto statRow = juce::Rectangle<int>(statsArea.getX(),
        statsArea.getCentreY() - 24,
        statsArea.getWidth(),
        48);
    constexpr int statGap = 12;
    const int statW = juce::jmax(72, (statRow.getWidth() - statGap * 2) / 3);
    lblCpu.setBounds(statRow.removeFromLeft(statW));
    statRow.removeFromLeft(statGap);
    lblProc.setBounds(statRow.removeFromLeft(statW));
    statRow.removeFromLeft(statGap);
    lblBuf.setBounds(statRow);
    lblStats.setBounds(statsArea.withTrimmedTop(statsArea.getHeight() - 18));

    // Input strip (left edge — knobs stacked, fader below)
    auto left2 = area.removeFromLeft(stripW);
    {
        const int kW = 48, kH = 58, toggleH = 26;
        const int topPad = 52;
        const int meterReserve = 182;
        const int controlsTop = left2.getY() + topPad;
        const int controlsBottom = left2.getBottom() - meterReserve;
        const int totalControlsH = kH * 3 + toggleH;
        const int gap = juce::jmax(12, (controlsBottom - controlsTop - totalControlsH) / 3);
        const int kX = left2.getX() + (left2.getWidth() - kW) / 2;
        const int toggleW = left2.getWidth() - 12;
        const int toggleX = left2.getX() + (left2.getWidth() - toggleW) / 2;

        int y = controlsTop;
        inputVolume.setBounds(kX, y, kW, kH);    y += kH + gap;
        inputGate.setBounds(kX, y, kW, kH);      y += kH + gap;
        inputTranspose.setBounds(kX, y, kW, kH); y += kH + gap;
        btnMonoStereo.setBounds(toggleX, y, toggleW, toggleH);
    }

    // Output strip (right edge — knobs stacked, fader below)
    auto right1 = area.removeFromRight(stripW);
    {
        const int kW = 48, kH = 58;
        const int topPad = 52;
        const int meterReserve = 182;
        const int controlsTop = right1.getY() + topPad;
        const int controlsBottom = right1.getBottom() - meterReserve;
        const int totalControlsH = kH * 3;
        const int gap = juce::jmax(18, (controlsBottom - controlsTop - totalControlsH) / 2);
        const int kX = right1.getX() + (right1.getWidth() - kW) / 2;

        int y = controlsTop;
        outputVolume.setBounds(kX, y, kW, kH); y += kH + gap;
        outputGain.setBounds(kX, y, kW, kH);   y += kH + gap;
        outputMix.setBounds(kX, y, kW, kH);
    }

    // Center: mixer + lanes
    const bool showA = laneA && laneA->isVisible();
    const bool showB = laneB && laneB->isVisible();
    const bool dualMode = showA && showB;

    const int mixerH = dualMode ? 0 : 170;

    auto center = area;
    auto mixerArea = center.removeFromBottom(mixerH);

    if (dualMode)
    {
        const auto hidden = juce::Rectangle<int>();
        volSliderA.setBounds(hidden);
        panSliderA.setBounds(hidden);
        widthSliderA.setBounds(hidden);
        volSliderB.setBounds(hidden);
        panSliderB.setBounds(hidden);
        widthSliderB.setBounds(hidden);
        btnSwitcher.setBounds(hidden);
    }
    else
    {
        // Single line: keep the active 3-knob group visually centered.
        const int yKnobs = mixerArea.getCentreY() - (knobH / 2);
        const int centeredKnobGap = knobGap + 8;
        const int groupW = knobSz * 3 + centeredKnobGap * 2;
        const int startX = mixerArea.getCentreX() - groupW / 2;

        if (showA)
        {
            volSliderA.setBounds(startX, yKnobs, knobSz, knobH);
            panSliderA.setBounds(startX + knobSz + centeredKnobGap, yKnobs, knobSz, knobH);
            widthSliderA.setBounds(startX + (knobSz + centeredKnobGap) * 2, yKnobs, knobSz, knobH);
        }
        else if (showB)
        {
            volSliderB.setBounds(startX, yKnobs, knobSz, knobH);
            panSliderB.setBounds(startX + knobSz + centeredKnobGap, yKnobs, knobSz, knobH);
            widthSliderB.setBounds(startX + (knobSz + centeredKnobGap) * 2, yKnobs, knobSz, knobH);
        }

        btnSwitcher.setBounds({});
    }

    // Lanes: full height for single visible lane, split 50/50 for dual
    if (dualMode)
    {
        const int lanePad = 4;
        const int laneGap = 4;
        const int laneH = (center.getHeight() - laneGap) / 2;
        laneA->setBounds(center.removeFromTop(laneH).reduced(lanePad));
        center.removeFromTop(laneGap);
        laneB->setBounds(center.reduced(lanePad));
    }
    else if (showA)
    {
        laneA->setBounds(center.reduced(10));
    }
    else if (showB)
    {
        laneB->setBounds(center.reduced(10));
    }

    // Sidebar drawers (overlay, positioned absolutely)
    const int drawerTop = headerH;
    const int drawerH = getHeight() - headerH - footerH;

    if (leftPanelOpen)
    {
        leftDrawer.setBounds(0, drawerTop, drawerW, drawerH);
        leftDrawer.toFront(false);

        auto d = leftDrawer.getLocalBounds();
        d.removeFromTop(36);
        d = d.reduced(12, 8);
        searchBarBrowser.setBounds(d.removeFromTop(34));
        d.removeFromTop(12);
        quickAddViewport.setBounds(d);
        layoutQuickAddButtons();
    }

    if (rightPanelOpen)
    {
        rightDrawer.setBounds(getWidth() - drawerW, drawerTop, drawerW, drawerH);
        rightDrawer.toFront(false);

        auto d = rightDrawer.getLocalBounds();
        d.removeFromTop(36);
        d = d.reduced(14, 10);
        searchBarPresets.setBounds(d.removeFromTop(34));
        d.removeFromTop(12);
        lblCurrentPreset.setBounds(d.removeFromTop(26));
        d.removeFromTop(10);
        presetSelector.setBounds(d.removeFromTop(36));
        d.removeFromTop(14);

        auto btnArea = d.removeFromBottom(108);
        auto topButtons = btnArea.removeFromTop(46);
        btnSave.setBounds(topButtons.removeFromLeft(topButtons.getWidth() / 2).reduced(4));
        btnLoad.setBounds(topButtons.reduced(4));
        btnArea.removeFromTop(10);
        btnClear.setBounds(btnArea.removeFromTop(46).reduced(4));
    }

    updatePedalGui();
}

// ==============================================================================
// CONTROL LOGIC
// ==============================================================================

void NOVAAudioProcessorEditor::refreshDrawerButtons()
{
    btnToggleLeft.setColour(juce::TextButton::buttonColourId,
        leftPanelOpen ? Nova::Colors::Accent.withAlpha(0.28f) : juce::Colour::fromString("ff1A2332"));

    btnToggleRight.setColour(juce::TextButton::buttonColourId,
        rightPanelOpen ? Nova::Colors::Accent.withAlpha(0.28f) : juce::Colour::fromString("ff1A2332"));
}

void NOVAAudioProcessorEditor::setLeftPanelOpen(bool shouldOpen)
{
    leftPanelOpen = shouldOpen;
    leftDrawer.setVisible(leftPanelOpen);
    refreshDrawerButtons();
    resized();
    repaint();
}

void NOVAAudioProcessorEditor::setRightPanelOpen(bool shouldOpen)
{
    rightPanelOpen = shouldOpen;
    rightDrawer.setVisible(rightPanelOpen);
    refreshDrawerButtons();
    resized();
    repaint();
}

void NOVAAudioProcessorEditor::applyEditorPreferences(bool applyStartupPanels)
{
    lblCpu.setVisible(showPerformanceStats);
    lblProc.setVisible(showPerformanceStats);
    lblBuf.setVisible(showPerformanceStats);

    // Update route info tooltip with configured shortcut
    const int keyCode = EditorPrefs::getSwitcherKeyCode();
    routeInfoText = "Shortcut: " + EditorPrefs::keyCodeToDisplayName(keyCode)
        + ". Configure in Settings > General > Interface.";

    if (applyStartupPanels)
    {
        leftPanelOpen = openQuickAddOnStartup;
        rightPanelOpen = openPresetsOnStartup;
        leftDrawer.setVisible(leftPanelOpen);
        rightDrawer.setVisible(rightPanelOpen);
        refreshDrawerButtons();
    }

    resized();
    repaint();
}

void NOVAAudioProcessorEditor::openSettingsOverlay()
{
    auto overlay = std::make_unique<SettingsOverlay>(
        audioProcessor,
        [this](bool enabled)
        {
            showPerformanceStats = enabled;
            applyEditorPreferences(false);
        },
        [this](bool enabled)
        {
            openQuickAddOnStartup = enabled;
        },
        [this](bool enabled)
        {
            openPresetsOnStartup = enabled;
        },
        [this]()
        {
            // Switcher config changed — update route info tooltip and force relayout
            const int keyCode = EditorPrefs::getSwitcherKeyCode();
            routeInfoText = "Shortcut: " + EditorPrefs::keyCodeToDisplayName(keyCode)
                + ". Configure in Settings > General > Interface.";

            // If current mode is now disabled, switch to first enabled mode
            const int modes = EditorPrefs::getSwitcherModes();
            const int current = (int)audioProcessor.getSwitcherMode();
            if (!((modes >> current) & 1))
            {
                for (int i = 0; i < 3; ++i)
                {
                    if ((modes >> i) & 1)
                    {
                        audioProcessor.setSwitcherMode(static_cast<Nova::SwitcherMode>(i));
                        break;
                    }
                }
            }

            resized();
            repaint();
        },
        LibrarySettingsPage::Callbacks{
            [this]()
            {
                return currentPresetName == "No Preset" ? juce::String{} : currentPresetName;
            },
            []()
            {
                return getStartupPresetName();
            },
            [this]()
            {
                return getPresetDirectory();
            },
            [this]()
            {
                if (currentPresetName == "No Preset")
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                        "Startup Preset",
                        "Load or save a preset first before setting a startup preset.");
                    return;
                }

                const auto presetFile = getPresetFileForName(currentPresetName);
                if (!presetFile.existsAsFile())
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                        "Startup Preset",
                        "The current preset has not been saved to disk yet.");
                    return;
                }

                getStartupPresetPointerFile().replaceWithText(presetFile.getFullPathName());
            },
            []()
            {
                const auto pointerFile = getStartupPresetPointerFile();
                if (pointerFile.existsAsFile())
                    pointerFile.deleteFile();
            }
        },
        [this](int wizardID)
        {
            juce::MessageManager::callAsync([this, wizardID]()
            {
                launchWizard(wizardID);
            });
        },
        [this]()
        {
            juce::MessageManager::callAsync([this]()
            {
                currentOverlay.reset();
                resized();
            });
        });

    addAndMakeVisible(overlay.get());
    overlay->setBounds(getLocalBounds());
    overlay->toFront(true);
    overlay->grabKeyboardFocus();
    currentOverlay = std::move(overlay);
}

void NOVAAudioProcessorEditor::launchWizard(int wizardID)
{
    auto closeFn = [this]()
    {
        juce::MessageManager::callAsync([this]()
        {
            currentOverlay.reset();
            resized();
        });
    };

    std::unique_ptr<juce::Component> wizard;

    switch (wizardID)
    {
        case 0: // Audio Setup
            wizard = std::make_unique<AudioSetupWizard>(audioProcessor, closeFn);
            break;

        case 1: // Start / Launcher
            wizard = std::make_unique<StartWizard>(
                closeFn,
                [this] { launchWizard(0); },  // link to Audio Setup
                [this] { launchWizard(2); }); // link to Preset Finder
            break;

        case 2: // Preset Finder
        {
            PresetFinderWizard::Callbacks pfCallbacks;
            pfCallbacks.getPresetDirectory = [this]() { return getPresetDirectory(); };
            pfCallbacks.loadPresetFile = [this](const juce::File& f)
            {
                if (audioProcessor.loadPresetFromFile(f))
                {
                    setCurrentPreset(f.getFileNameWithoutExtension());
                    refreshPresetList();
                }
            };
            pfCallbacks.addPedal = [this](const juce::String& typeID, Nova::ChainID chain, Nova::ZoneID zone)
            {
                audioProcessor.requestAddPedal(typeID, chain, zone);
            };
            wizard = std::make_unique<PresetFinderWizard>(std::move(pfCallbacks), closeFn);
            break;
        }

        default: return;
    }

    addAndMakeVisible(wizard.get());
    wizard->setBounds(getLocalBounds());
    wizard->toFront(true);
    wizard->grabKeyboardFocus();
    currentOverlay = std::move(wizard);
}

void NOVAAudioProcessorEditor::toggleLeftPanel()
{
    setLeftPanelOpen(!leftPanelOpen);
}

void NOVAAudioProcessorEditor::toggleRightPanel()
{
    setRightPanelOpen(!rightPanelOpen);
}

void NOVAAudioProcessorEditor::toggleTuner()
{
    audioProcessor.toggleTuner();
    const bool newState = audioProcessor.getAudioEngine().isTunerEnabled();

    btnTuner.setColour(juce::TextButton::buttonColourId,
        newState ? Nova::Colors::Success.withAlpha(0.28f) : juce::Colour::fromString("ff0F3D22"));

    if (newState)
    {
        tunerOverlay = std::make_unique<TunerOverlay>(audioProcessor);
        addAndMakeVisible(tunerOverlay.get());
        tunerOverlay->setBounds(getLocalBounds());
        tunerOverlay->toFront(true);
    }
    else
    {
        tunerOverlay.reset();
    }
}

void NOVAAudioProcessorEditor::updateStats()
{
    const double sampleRate = audioProcessor.getSampleRate();
    const double bufferSize = (double)audioProcessor.getBlockSize();
    const double cpuPercent = audioProcessor.getCpuUsage();

    double bufferDurationMs = 0.0;
    double procTimeMs = 0.0;

    if (sampleRate > 0)
    {
        bufferDurationMs = (bufferSize / sampleRate) * 1000.0;
        procTimeMs = (cpuPercent / 100.0) * bufferDurationMs;
    }

    // Detect auto-heal events from the engine and show a brief visible warning.
    const int currentHealCount = audioProcessor.getAudioEngine().getAutoHealCount();
    if (currentHealCount != lastKnownAutoHealCount)
    {
        lastKnownAutoHealCount = currentHealCount;
        autoHealFlashFrames = 45; // ~3 seconds at 15 Hz
    }

    if (autoHealFlashFrames > 0)
    {
        --autoHealFlashFrames;
        lblStats.setText("AUTO-HEAL TRIGGERED", juce::dontSendNotification);
    }
    else
    {
        lblStats.setText({}, juce::dontSendNotification);
    }

    lblCpu.setText("CPU USAGE\n" + juce::String(cpuPercent, 1) + "%", juce::dontSendNotification);
    lblProc.setText("PROCESS TIME\n" + juce::String(procTimeMs, 2) + " ms", juce::dontSendNotification);
    lblBuf.setText("BUFFER TIME\n" + juce::String(bufferDurationMs, 1) + " ms", juce::dontSendNotification);

    if (autoHealFlashFrames > 0)
        lblStats.setColour(juce::Label::textColourId, Nova::Colors::Error);
    else if (cpuPercent > 90.0)
        lblStats.setColour(juce::Label::textColourId, juce::Colours::red);
    else
        lblStats.setColour(juce::Label::textColourId, Nova::Colors::TextDim);
}

void NOVAAudioProcessorEditor::updatePedalGui()
{
    std::set<juce::AudioProcessorGraph::NodeID> requiredNodeIDs;

    auto processLane = [&](Nova::ChainID chain, ChainLane* laneComp)
        {
            if (!laneComp)
                return;

            const auto engineNodes = audioProcessor.getAudioEngine().getNodes(chain);
            const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
            auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);

            struct DrawItem
            {
                juce::ValueTree state;
                AudioEngine::ChainNodeView nodeView;
                int zoneIdx = 0;
                PedalSlotComponent* slot = nullptr;
                int preferredW = 120;
                int preferredH = 180;
                bool createdNow = false;
            };

            std::vector<DrawItem> itemsToDraw;
            itemsToDraw.reserve((size_t)treeList.getNumChildren());
            std::set<juce::AudioProcessorGraph::NodeID> usedNodeIDs;

            for (int i = 0; i < treeList.getNumChildren(); ++i)
            {
                auto state = treeList.getChild(i);
                if (state.getType() != Nova::IDs::PEDAL)
                    continue;

                juce::String expectedType = state.getProperty(Nova::IDs::PEDAL_TYPE).toString();
                const int zIdx = static_cast<int>(state.getProperty(Nova::IDs::PEDAL_ZONE, 0));
                if (zIdx < 0 || zIdx > 3)
                    continue;

                AudioEngine::ChainNodeView matchedNodeView;
                bool foundMatch = false;
                const auto pedalID = state.getProperty(Nova::IDs::PEDAL_ID).toString();

                for (const auto& candidate : engineNodes)
                {
                    if (candidate.node == nullptr || candidate.node->getProcessor() == nullptr)
                        continue;
                    if (usedNodeIDs.find(candidate.node->nodeID) != usedNodeIDs.end())
                        continue;

                    if (candidate.pedalID == pedalID && pedalID.isNotEmpty())
                    {
                        matchedNodeView = candidate;
                        foundMatch = true;
                        break;
                    }
                }

                if (!foundMatch)
                {
                    for (const auto& candidate : engineNodes)
                    {
                        if (candidate.node == nullptr || candidate.node->getProcessor() == nullptr)
                            continue;
                        if (usedNodeIDs.find(candidate.node->nodeID) != usedNodeIDs.end())
                            continue;

                        juce::String procName = candidate.node->getProcessor()->getName();
                        if (procName.containsIgnoreCase(expectedType) || expectedType.containsIgnoreCase(procName))
                        {
                            matchedNodeView = candidate;
                            foundMatch = true;
                            break;
                        }
                    }
                }

                if (foundMatch)
                {
                    usedNodeIDs.insert(matchedNodeView.node->nodeID);
                    itemsToDraw.push_back({ state, matchedNodeView, zIdx });
                }
            }

            for (auto& item : itemsToDraw)
            {
                requiredNodeIDs.insert(item.nodeView.node->nodeID);

                auto it = activePedalEditors.find(item.nodeView.node->nodeID);
                PedalSlotComponent* slot = nullptr;

                if (it == activePedalEditors.end())
                {
                    auto host = std::make_unique<PedalSlotComponent>(audioProcessor, chain,
                        [this](Nova::ChainID chainId, juce::String pedalId)
                        {
                            showPedalEditor(chainId, pedalId);
                        });
                    host->setPedalState(item.state);
                    addAndMakeVisible(host.get());
                    slot = host.get();
                    activePedalEditors[item.nodeView.node->nodeID] = std::move(host);
                    item.createdNow = true;
                }
                else
                {
                    slot = it->second.get();
                    slot->setPedalState(item.state);
                }

                if (slot)
                {
                    item.slot = slot;
                    item.preferredW = slot->getPreferredWidth();
                    item.preferredH = slot->getPreferredHeight();
                }
            }

            for (int zoneIdx = 0; zoneIdx < 4; ++zoneIdx)
            {
                std::vector<DrawItem*> zoneItems;
                zoneItems.reserve(itemsToDraw.size());

                for (auto& item : itemsToDraw)
                    if (item.zoneIdx == zoneIdx && item.slot != nullptr)
                        zoneItems.push_back(&item);

                if (zoneItems.empty())
                    continue;

                const auto zoneRect = laneComp->getZoneRect(zoneIdx);
                if (zoneRect.isEmpty())
                    continue;

                const int zoneAbsX = laneComp->getX() + zoneRect.getX();
                const int zoneAbsY = laneComp->getY() + zoneRect.getY();
                const int zoneW = zoneRect.getWidth();
                const int zoneH = zoneRect.getHeight();

                if (zoneIdx == (int)Nova::ZoneID::Amp || zoneIdx == (int)Nova::ZoneID::Cabinet)
                {
                    for (auto* item : zoneItems)
                    {
                        const int finalX = zoneAbsX + (zoneW - item->preferredW) / 2;
                        const int finalY = zoneAbsY + (zoneH - item->preferredH) / 2;
                        item->slot->setBounds(finalX, finalY, item->preferredW, item->preferredH);

                        if (item->createdNow)
                            item->slot->toFront(false);
                    }

                    continue;
                }

                // ---- Multi-row adaptive layout for Pre / FX zones ----
                const int itemCount = (int)zoneItems.size();
                const auto layout = calculateFlexLayout(zoneW, zoneH, itemCount);
                const int visibleCount = juce::jmin(itemCount, layout.maxCapacity);

                const int actualRows = (visibleCount + layout.cols - 1) / layout.cols;

                // Center the entire grid block within the zone
                const int colsFirstRow = juce::jmin(layout.cols, visibleCount);
                const int blockW = colsFirstRow * layout.cardW + (colsFirstRow - 1) * layout.gapH;
                const int blockH = actualRows * layout.cardH + juce::jmax(0, actualRows - 1) * layout.gapV;
                const int blockStartY = zoneAbsY + (zoneH - blockH) / 2;

                for (int i = 0; i < visibleCount; ++i)
                {
                    const int row = i / layout.cols;
                    const int col = i % layout.cols;

                    // Items in this specific row (last row may have fewer)
                    const int itemsInRow = (row < actualRows - 1)
                        ? layout.cols
                        : (visibleCount - row * layout.cols);
                    const int rowW = itemsInRow * layout.cardW + (itemsInRow - 1) * layout.gapH;
                    const int rowStartX = zoneAbsX + (zoneW - rowW) / 2;

                    const int x = rowStartX + col * (layout.cardW + layout.gapH);
                    const int y = blockStartY + row * (layout.cardH + layout.gapV);

                    zoneItems[(size_t)i]->slot->setBounds(x, y, layout.cardW, layout.cardH);

                    if (zoneItems[(size_t)i]->createdNow)
                        zoneItems[(size_t)i]->slot->toFront(false);
                }
            }
        };

    if (laneA && laneA->isVisible())
        processLane(Nova::ChainID::LineA, laneA.get());
    if (laneB && laneB->isVisible())
        processLane(Nova::ChainID::LineB, laneB.get());

    for (auto it = activePedalEditors.begin(); it != activePedalEditors.end();)
    {
        if (requiredNodeIDs.find(it->first) == requiredNodeIDs.end())
            it = activePedalEditors.erase(it);
        else
            ++it;
    }

    if (currentOverlay)
        currentOverlay->toFront(true);
    if (tunerOverlay && tunerOverlay->isVisible())
        tunerOverlay->toFront(true);
}

void NOVAAudioProcessorEditor::updateSwitcherState()
{
    const bool on = audioProcessor.isEngineOn();
    const int mode = (int)audioProcessor.getSwitcherMode();
    const bool dualParallel = (mode == (int)Nova::SwitcherMode::Dual_Parallel);
    const int enabledModes = EditorPrefs::getSwitcherModes();

    btnStartStop.setButtonText(on ? "POWER ON" : "POWER OFF");
    btnStartStop.setColour(juce::TextButton::buttonColourId,
        on ? juce::Colour::fromString("ff0F3D22") : juce::Colour::fromString("ff3D1418"));
    btnStartStop.setColour(juce::TextButton::buttonOnColourId,
        on ? juce::Colour::fromString("ff0F3D22") : juce::Colour::fromString("ff3D1418"));
    btnStartStop.setToggleState(on, juce::dontSendNotification);

    juce::String txt;
    bool aActive = false, bActive = false;
    juce::Colour switcherColour = juce::Colour::fromString("ff1A2332");

    if (mode == (int)Nova::SwitcherMode::LineA_Only)
    {
        txt = "ROUTING: LINE A";
        aActive = true;
        switcherColour = Nova::Colors::CableOnA.withAlpha(0.28f);
    }
    else if (mode == (int)Nova::SwitcherMode::LineB_Only)
    {
        txt = "ROUTING: LINE B";
        bActive = true;
        switcherColour = Nova::Colors::CableOnB.withAlpha(0.28f);
    }
    else
    {
        txt = "ROUTING: DUAL PARALLEL";
        aActive = true;
        bActive = true;
        switcherColour = Nova::Colors::Accent.withAlpha(0.18f);
    }

    btnSwitcher.setButtonText(txt);
    btnSwitcher.setColour(juce::TextButton::buttonColourId, on ? switcherColour : juce::Colour::fromString("ff0D1520"));
    btnSwitcher.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(on ? 0.92f : 0.5f));

    auto applyRouteState = [on](juce::TextButton& button, bool active, juce::Colour activeColour)
    {
        const auto fill = active
            ? activeColour.withAlpha(on ? 0.52f : 0.22f)
            : juce::Colour::fromString("ff131A26");
        button.setColour(juce::TextButton::buttonColourId, fill);
        button.setColour(juce::TextButton::buttonOnColourId, fill);
        button.setColour(juce::TextButton::textColourOffId,
            active ? juce::Colours::white.withAlpha(on ? 0.94f : 0.55f) : Nova::Colors::TextDim.withAlpha(0.88f));
        button.setColour(juce::TextButton::textColourOnId,
            active ? juce::Colours::white.withAlpha(on ? 0.98f : 0.65f) : Nova::Colors::TextDim.withAlpha(0.88f));
    };

    if (btnRouteA.isVisible())
        applyRouteState(btnRouteA, mode == (int)Nova::SwitcherMode::LineA_Only, Nova::Colors::CableOnA);
    if (btnRoutePar.isVisible())
        applyRouteState(btnRoutePar, mode == (int)Nova::SwitcherMode::Dual_Parallel, Nova::Colors::Accent);
    if (btnRouteB.isVisible())
        applyRouteState(btnRouteB, mode == (int)Nova::SwitcherMode::LineB_Only, Nova::Colors::CableOnB);

    // Check if route button layout needs updating (visibility is managed by resized())
    const bool showA   = EditorPrefs::isModeEnabled(enabledModes, Nova::SwitcherMode::LineA_Only);
    const bool showPar = EditorPrefs::isModeEnabled(enabledModes, Nova::SwitcherMode::Dual_Parallel);
    const bool showB   = EditorPrefs::isModeEnabled(enabledModes, Nova::SwitcherMode::LineB_Only);

    bool routeLayoutChanged = (btnRouteA.isVisible() != showA)
                           || (btnRoutePar.isVisible() != showPar)
                           || (btnRouteB.isVisible() != showB);

    // Show only the active line(s) — single-line modes get full vertical space
    bool layoutChanged = routeLayoutChanged;

    if (laneA && laneA->isVisible() != aActive)
    {
        laneA->setVisible(aActive);
        layoutChanged = true;
    }
    if (laneB && laneB->isVisible() != bActive)
    {
        laneB->setVisible(bActive);
        layoutChanged = true;
    }

    const bool showMixerControls = !dualParallel;
    volSliderA.setVisible(aActive && showMixerControls);
    panSliderA.setVisible(aActive && showMixerControls);
    widthSliderA.setVisible(aActive && showMixerControls);

    volSliderB.setVisible(bActive && showMixerControls);
    panSliderB.setVisible(bActive && showMixerControls);
    widthSliderB.setVisible(bActive && showMixerControls);

    if (laneA) laneA->setActive(aActive && on);
    if (laneB) laneB->setActive(bActive && on);

    if (layoutChanged)
        resized();

    repaint();
}

void NOVAAudioProcessorEditor::requestUiRefresh()
{
    // Always defer to next message-loop iteration via triggerAsyncUpdate().
    // DO NOT run handleAsyncUpdate() synchronously here — when called from
    // valueTreeChildAdded during requestAddPedal, the engine rebuild hasn't
    // happened yet, so updatePedalGui() would see stale engine state (the
    // "always one action behind" bug). Deferring ensures the full
    // requestAddPedal/requestMovePedal call completes before the UI refreshes.
    const bool wasPending = uiRefreshPending.exchange(true, std::memory_order_acq_rel);
    if (!wasPending)
        triggerAsyncUpdate();
}

void NOVAAudioProcessorEditor::handleAsyncUpdate()
{
    uiRefreshPending.store(false, std::memory_order_release);
    updateSwitcherState();
    updatePedalGui();
}

void NOVAAudioProcessorEditor::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& id)
{
    if (id == Nova::IDs::PEDAL_ENABLED
        || id == Nova::IDs::PEDAL_TYPE
        || id == Nova::IDs::PEDAL_ZONE
        || id == Nova::IDs::ENGINE_ON
        || id == Nova::IDs::SWITCH_MODE)
    {
        requestUiRefresh();
    }
}

void NOVAAudioProcessorEditor::mouseMove(const juce::MouseEvent& e)
{
    const bool shouldHover = routeInfoIconBounds.contains(e.position);
    if (routeInfoHovered != shouldHover)
    {
        routeInfoHovered = shouldHover;
        repaint();
    }
}

void NOVAAudioProcessorEditor::mouseExit(const juce::MouseEvent&)
{
    if (routeInfoHovered)
    {
        routeInfoHovered = false;
        repaint();
    }
}

bool NOVAAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    const int configuredKey = EditorPrefs::getSwitcherKeyCode();
    if (key.getKeyCode() == configuredKey)
    {
        const int modes = EditorPrefs::getSwitcherModes();
        audioProcessor.cycleSwitcherWithMask(modes);
        return true;
    }
    return false;
}

void NOVAAudioProcessorEditor::showPedalEditor(Nova::ChainID chain, const juce::String& pedalID)
{
    const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
    auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);
    if (!treeList.isValid())
        return;

    int pedalIndex = -1;
    juce::ValueTree pedalState;

    for (int i = 0; i < treeList.getNumChildren(); ++i)
    {
        auto child = treeList.getChild(i);
        if (!child.hasType(Nova::IDs::PEDAL))
            continue;

        if (child.getProperty(Nova::IDs::PEDAL_ID).toString() == pedalID)
        {
            pedalIndex = i;
            pedalState = child;
            break;
        }
    }

    if (pedalIndex < 0 || !pedalState.isValid())
        return;

    auto* processorForPedal = audioProcessor.getAudioEngine().getProcessorForPedal(chain, pedalIndex);
    if (processorForPedal == nullptr)
        return;

    std::unique_ptr<juce::AudioProcessorEditor> pedalEditor(processorForPedal->createEditor());
    if (pedalEditor == nullptr)
        pedalEditor = std::make_unique<juce::GenericAudioProcessorEditor>(*processorForPedal);

    if (pedalEditor == nullptr)
        return;

    const auto pedalType = pedalState.getProperty(Nova::IDs::PEDAL_TYPE).toString();
    auto overlay = std::make_unique<PedalEditorOverlay>(
        pedalType,
        Nova::PedalCatalog::subtitleForType(pedalType),
        std::move(pedalEditor),
        [this]()
        {
            juce::MessageManager::callAsync([this]()
            {
                currentOverlay.reset();
                resized();
            });
        },
        Nova::PedalCatalog::accentForType(pedalType));

    addAndMakeVisible(overlay.get());
    overlay->setBounds(getLocalBounds());
    overlay->toFront(true);
    overlay->grabKeyboardFocus();
    currentOverlay = std::move(overlay);
}

std::vector<juce::Rectangle<int>> NOVAAudioProcessorEditor::getPedalBoundsForZone(Nova::ChainID chain, Nova::ZoneID zone) const
{
    std::vector<juce::Rectangle<int>> bounds;

    const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
    auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);
    if (!treeList.isValid())
        return bounds;

    bounds.reserve((size_t)treeList.getNumChildren());

    for (int i = 0; i < treeList.getNumChildren(); ++i)
    {
        auto state = treeList.getChild(i);
        if (!state.hasType(Nova::IDs::PEDAL))
            continue;

        const auto stateZone = static_cast<Nova::ZoneID>(
            (int)state.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
        if (stateZone != zone)
            continue;

        const auto pedalID = state.getProperty(Nova::IDs::PEDAL_ID).toString();
        if (pedalID.isEmpty())
            continue;

        for (const auto& entry : activePedalEditors)
        {
            auto* slot = entry.second.get();

            if (slot == nullptr || slot->getChain() != chain || slot->getZone() != zone)
                continue;

            if (slot->getPedalID() == pedalID)
            {
                bounds.push_back(slot->getBounds());
                break;
            }
        }
    }

    return bounds;
}

void NOVAAudioProcessorEditor::showOverlay(Nova::ZoneID zone, Nova::ChainID chain)
{
    auto overlay = std::make_unique<AssetBrowserOverlay>(
        zone,
        [this, zone, chain](juce::String typeID)
        {
            audioProcessor.requestAddPedal(typeID, chain, zone);
        },
        [this]()
        {
            juce::MessageManager::callAsync([this]()
                {
                    currentOverlay.reset();
                    resized();
                });
        }
    );

    addAndMakeVisible(overlay.get());
    overlay->setBounds(getLocalBounds());
    currentOverlay = std::move(overlay);
}
