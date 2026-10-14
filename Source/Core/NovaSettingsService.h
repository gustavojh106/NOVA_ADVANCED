#pragma once

#include <JuceHeader.h>

namespace Nova::SettingsService
{
    namespace Keys
    {
        static constexpr const char* showStats = "editor.showPerformanceStats";
        static constexpr const char* openBrowserOnStartup = "editor.openBrowserOnStartup";
        static constexpr const char* openPresetsOnStartup = "editor.openPresetsOnStartup";
        static constexpr const char* startupMode = "editor.startupMode";
        static constexpr const char* confirmBeforeClear = "editor.confirmBeforeClear";
        static constexpr const char* tunerReference = "editor.tunerReference";
        static constexpr const char* libraryView = "editor.libraryView";
        static constexpr const char* favoritesFirst = "editor.libraryFavoritesFirst";
        static constexpr const char* switcherShortcut = "editor.switcherShortcut";
        static constexpr const char* switcherModes = "editor.switcherModes";
    }

    namespace StartupModes
    {
        static constexpr const char* rememberLastContext = "Remember last context";
        static constexpr const char* openCleanRig = "Open clean rig";
        static constexpr const char* openStartupPreset = "Open startup preset";
    }

    struct EditorSettings
    {
        int schemaVersion = 1;
        juce::StringPairArray values;
        bool loadedFromDisk = false;
        bool usedFallback = false;
    };

    inline juce::File getSettingsDirectory()
    {
        auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("NOVA");

        if (!directory.exists())
            directory.createDirectory();

        return directory;
    }

    inline juce::File getEditorSettingsFile()
    {
        return getSettingsDirectory().getChildFile("editor-settings.xml");
    }

    inline juce::File getStartupPresetPointerFile()
    {
        return getSettingsDirectory().getChildFile("startup-preset.txt");
    }

    inline int countSwitcherModes(int modes) noexcept
    {
        modes &= 7;
        return ((modes >> 0) & 1) + ((modes >> 1) & 1) + ((modes >> 2) & 1);
    }

    inline bool tryParseBool(const juce::String& text, bool& result)
    {
        const auto normalized = text.trim().toLowerCase();

        if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
        {
            result = true;
            return true;
        }

        if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off")
        {
            result = false;
            return true;
        }

        return false;
    }

    inline bool isValidStartupMode(const juce::String& value)
    {
        return value == StartupModes::rememberLastContext
            || value == StartupModes::openCleanRig
            || value == StartupModes::openStartupPreset;
    }

    inline bool isValidTunerReference(const juce::String& value)
    {
        return value == "A = 440 Hz" || value == "A = 442 Hz" || value == "A = 432 Hz";
    }

    inline bool isValidLibraryView(const juce::String& value)
    {
        return value == "All presets" || value == "Favorites first" || value == "Recent first";
    }

    inline EditorSettings makeDefaultEditorSettings()
    {
        EditorSettings settings;
        settings.values.set(Keys::showStats, "true");
        settings.values.set(Keys::openBrowserOnStartup, "false");
        settings.values.set(Keys::openPresetsOnStartup, "false");
        settings.values.set(Keys::startupMode, StartupModes::rememberLastContext);
        settings.values.set(Keys::confirmBeforeClear, "true");
        settings.values.set(Keys::tunerReference, "A = 440 Hz");
        settings.values.set(Keys::libraryView, "All presets");
        settings.values.set(Keys::favoritesFirst, "false");
        settings.values.set(Keys::switcherShortcut, juce::String(juce::KeyPress::spaceKey));
        settings.values.set(Keys::switcherModes, "7");
        return settings;
    }

    inline juce::String getString(const EditorSettings& settings,
        const char* key,
        const juce::String& defaultValue)
    {
        return settings.values.containsKey(key) ? settings.values[key] : defaultValue;
    }

    inline bool getBool(const EditorSettings& settings, const char* key, bool defaultValue)
    {
        if (!settings.values.containsKey(key))
            return defaultValue;

        bool parsed = defaultValue;
        return tryParseBool(settings.values[key], parsed) ? parsed : defaultValue;
    }

    inline int getSwitcherKeyCode(const EditorSettings& settings)
    {
        const int keyCode = getString(settings, Keys::switcherShortcut, {}).getIntValue();
        return keyCode > 0 ? keyCode : juce::KeyPress::spaceKey;
    }

    inline int getSwitcherModes(const EditorSettings& settings)
    {
        const int modes = getString(settings, Keys::switcherModes, "7").getIntValue() & 7;
        return countSwitcherModes(modes) >= 2 ? modes : 7;
    }

    inline float parseTunerReference(const EditorSettings& settings)
    {
        const auto value = getString(settings, Keys::tunerReference, "A = 440 Hz");
        if (value.contains("442"))
            return 442.0f;
        if (value.contains("432"))
            return 432.0f;
        return 440.0f;
    }

    inline void setString(EditorSettings& settings, const char* key, const juce::String& value)
    {
        settings.values.set(key, value);
    }

    inline void setBool(EditorSettings& settings, const char* key, bool value)
    {
        settings.values.set(key, value ? "true" : "false");
    }

    inline void sanitize(EditorSettings& settings)
    {
        auto defaults = makeDefaultEditorSettings();

        auto sanitizeBool = [&settings, &defaults](const char* key)
        {
            bool parsed = false;
            if (!settings.values.containsKey(key) || !tryParseBool(settings.values[key], parsed))
                settings.values.set(key, defaults.values[key]);
            else
                settings.values.set(key, parsed ? "true" : "false");
        };

        sanitizeBool(Keys::showStats);
        sanitizeBool(Keys::openBrowserOnStartup);
        sanitizeBool(Keys::openPresetsOnStartup);
        sanitizeBool(Keys::confirmBeforeClear);
        sanitizeBool(Keys::favoritesFirst);

        if (!isValidStartupMode(getString(settings, Keys::startupMode, {})))
            settings.values.set(Keys::startupMode, defaults.values[Keys::startupMode]);

        if (!isValidTunerReference(getString(settings, Keys::tunerReference, {})))
            settings.values.set(Keys::tunerReference, defaults.values[Keys::tunerReference]);

        if (!isValidLibraryView(getString(settings, Keys::libraryView, {})))
            settings.values.set(Keys::libraryView, defaults.values[Keys::libraryView]);

        settings.values.set(Keys::switcherShortcut, juce::String(getSwitcherKeyCode(settings)));
        settings.values.set(Keys::switcherModes, juce::String(getSwitcherModes(settings)));
        settings.schemaVersion = 1;
    }

    inline std::unique_ptr<juce::XmlElement> createXml(const EditorSettings& input)
    {
        auto settings = input;
        sanitize(settings);

        auto root = std::make_unique<juce::XmlElement>("NOVA_EDITOR_SETTINGS");
        root->setAttribute("schemaVersion", settings.schemaVersion);

        const auto keys = settings.values.getAllKeys();
        const auto values = settings.values.getAllValues();

        for (int i = 0; i < settings.values.size(); ++i)
        {
            auto* child = root->createNewChildElement("SETTING");
            child->setAttribute("name", keys[i]);
            child->setAttribute("value", values[i]);
        }

        return root;
    }

    inline EditorSettings loadEditorSettingsFromXml(const juce::XmlElement& xml)
    {
        auto settings = makeDefaultEditorSettings();
        settings.schemaVersion = xml.getIntAttribute("schemaVersion", 0);
        settings.loadedFromDisk = true;

        if (!xml.hasTagName("NOVA_EDITOR_SETTINGS"))
        {
            settings.usedFallback = true;
            settings.schemaVersion = 1;
            return settings;
        }

        for (auto* child = xml.getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (!child->hasTagName("SETTING"))
                continue;

            const auto key = child->getStringAttribute("name");
            if (key.isNotEmpty())
                settings.values.set(key, child->getStringAttribute("value"));
        }

        sanitize(settings);
        return settings;
    }

    inline EditorSettings loadEditorSettingsFromFile(const juce::File& file)
    {
        if (!file.existsAsFile())
            return makeDefaultEditorSettings();

        auto xml = juce::parseXML(file);
        if (xml == nullptr)
        {
            auto settings = makeDefaultEditorSettings();
            settings.loadedFromDisk = true;
            settings.usedFallback = true;
            return settings;
        }

        return loadEditorSettingsFromXml(*xml);
    }

    inline bool saveEditorSettingsToFile(const juce::File& file, const EditorSettings& settings)
    {
        file.getParentDirectory().createDirectory();
        auto xml = createXml(settings);
        return xml != nullptr && xml->writeTo(file, {});
    }

    inline EditorSettings loadEditorSettings()
    {
        return loadEditorSettingsFromFile(getEditorSettingsFile());
    }

    inline bool saveEditorSettings(const EditorSettings& settings)
    {
        return saveEditorSettingsToFile(getEditorSettingsFile(), settings);
    }

    inline bool readStartupPresetPointer(const juce::File& pointerFile,
        juce::File& presetFile,
        bool deleteInvalidPointer)
    {
        if (!pointerFile.existsAsFile())
            return false;

        presetFile = juce::File(pointerFile.loadFileAsString().trim());
        if (presetFile.existsAsFile())
            return true;

        if (deleteInvalidPointer)
            pointerFile.deleteFile();

        return false;
    }

    inline bool writeStartupPresetPointer(const juce::File& pointerFile, const juce::File& presetFile)
    {
        if (!presetFile.existsAsFile())
            return false;

        pointerFile.getParentDirectory().createDirectory();
        return pointerFile.replaceWithText(presetFile.getFullPathName());
    }

    inline bool writeStartupPresetPointer(const juce::File& presetFile)
    {
        return writeStartupPresetPointer(getStartupPresetPointerFile(), presetFile);
    }

    inline void clearStartupPresetPointer(const juce::File& pointerFile)
    {
        if (pointerFile.existsAsFile())
            pointerFile.deleteFile();
    }

    inline void clearStartupPresetPointer()
    {
        clearStartupPresetPointer(getStartupPresetPointerFile());
    }

    inline juce::String getStartupPresetName()
    {
        juce::File presetFile;
        return readStartupPresetPointer(getStartupPresetPointerFile(), presetFile, false)
            ? presetFile.getFileNameWithoutExtension()
            : juce::String();
    }
}
