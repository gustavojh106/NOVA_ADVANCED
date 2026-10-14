#pragma once

#include <JuceHeader.h>
#include <vector>
#include "PluginProcessor.h"
#include "SessionLogger.h"

// =============================================================================
// Problem report (E6): one zip a beta tester can attach when something goes wrong.
//
// - report.txt: what happened (the user's words), NOVA and system versions, the
//   audio setup and the engine diagnostics.
// - session-log.txt and session-log-previous.txt: this session's log and the one
//   before it (the session that crashed, when NOVA is reopened to report it).
// - current-rig.nova-preset: the current rig, loadable as a preset.
//
// The editor adds what only it knows (the user's words, the preset name, the audio
// device) and calls writeReport() on the message thread; nothing here touches the
// audio thread or the network. The user's home folder is replaced in every text
// file so paths don't carry their Windows user name.
// =============================================================================
namespace Nova::ProblemReport
{
    struct Info
    {
        juce::String description;       // "What happened?" (optional)
        juce::String createdAt;
        juce::String novaVersion;
        juce::String buildConfig;       // Debug / Release
        juce::String pluginFormat;      // Standalone / VST3
        juce::String host;              // the DAW; empty in the Standalone
        juce::StringArray system;       // "OS: ...", "CPU: ..."
        juce::String presetName;
        juce::StringArray audio;        // "Driver: ...", "Sample rate: ..."
        juce::String engineDiagnostics;
    };

    inline juce::String buildReportText(const Info& info)
    {
        juce::String text;
        const auto section = [&text] (const juce::String& title)
        {
            text << juce::newLine << "== " << title << " ==" << juce::newLine;
        };
        const auto orNone = [] (const juce::String& value, const char* fallback)
        {
            return value.trim().isNotEmpty() ? value.trim() : juce::String(fallback);
        };

        text << "NOVA problem report" << juce::newLine
             << "Created: " << info.createdAt << juce::newLine;

        section("What happened");
        text << orNone(info.description, "(not described)") << juce::newLine;

        section("NOVA");
        text << "Version: " << orNone(info.novaVersion, "unknown") << " (" << orNone(info.buildConfig, "unknown build") << ")" << juce::newLine
             << "Format: " << orNone(info.pluginFormat, "unknown") << juce::newLine;
        if (info.host.isNotEmpty())
            text << "Host: " << info.host << juce::newLine;
        text << "Preset: " << orNone(info.presetName, "(none)") << juce::newLine;

        section("System");
        text << info.system.joinIntoString(juce::newLine) << juce::newLine;

        section("Audio");
        text << info.audio.joinIntoString(juce::newLine) << juce::newLine;

        section("Engine");
        text << orNone(info.engineDiagnostics, "(not available)") << juce::newLine;

        section("Files in this report");
        text << "report.txt: this summary" << juce::newLine
             << "session-log.txt: log of this session" << juce::newLine
             << "session-log-previous.txt: log of the previous session, if any (after a crash, the one that crashed)" << juce::newLine
             << "current-rig.nova-preset: the current rig, loadable as a preset" << juce::newLine;

        return text;
    }

    // "C:\Users\ana\AppData\..." -> "%USERPROFILE%\AppData\..." in either slash style, any case.
    inline juce::String anonymizePaths(const juce::String& text, const juce::String& homePath)
    {
        const auto home = homePath.trimCharactersAtEnd("\\/");
        if (home.length() < 4)
            return text;

        return text.replace(home.replaceCharacter('/', '\\'), "%USERPROFILE%", true)
                   .replace(home.replaceCharacter('\\', '/'), "%USERPROFILE%", true);
    }

    inline juce::String makeFileName(const juce::Time& time)
    {
        return "NOVA-problem-report-" + time.formatted("%Y%m%d-%H%M%S") + ".zip";
    }

    // Documents\NOVA\Problem Reports
    inline juce::File getReportsFolder()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("NOVA").getChildFile("Problem Reports");
    }

    struct Entry
    {
        juce::String name;
        juce::MemoryBlock data;
    };

    inline Entry makeTextEntry(const juce::String& name, const juce::String& text)
    {
        Entry entry;
        entry.name = name;
        const auto utf8 = text.toUTF8();
        entry.data.append(utf8.getAddress(), utf8.sizeInBytes() - 1);
        return entry;
    }

    // Writes the entries into a new zip (through a temporary file, so a failure never
    // leaves half a report behind).
    inline juce::Result writeZip(const juce::File& zipFile, const std::vector<Entry>& entries)
    {
        if (! zipFile.getParentDirectory().createDirectory())
            return juce::Result::fail("Couldn't create " + zipFile.getParentDirectory().getFullPathName());

        juce::ZipFile::Builder builder;
        const auto now = juce::Time::getCurrentTime();
        for (const auto& entry : entries)
            builder.addEntry(new juce::MemoryInputStream(entry.data, false), 9, entry.name, now);

        juce::TemporaryFile temp(zipFile);
        {
            auto stream = temp.getFile().createOutputStream();
            if (stream == nullptr || ! builder.writeToStream(*stream, nullptr))
                return juce::Result::fail("Couldn't write " + temp.getFile().getFullPathName());
        }

        if (! temp.overwriteTargetFileWithTemporary())
            return juce::Result::fail("Couldn't save " + zipFile.getFullPathName());

        return juce::Result::ok();
    }

    inline juce::String getBuildConfig()
    {
       #if JUCE_DEBUG
        return "Debug";
       #else
        return "Release";
       #endif
    }

    inline juce::String getNovaVersion()
    {
       #ifdef JucePlugin_VersionString
        return JucePlugin_VersionString;
       #else
        return {};
       #endif
    }

    // "NOVA 1.0.0 · Standalone · Release build"
    inline juce::String describeBuild(const juce::AudioProcessor& processor)
    {
        const juce::String dot(juce::CharPointer_UTF8("  \xc2\xb7  "));
        return ("NOVA " + getNovaVersion()).trim() + dot
            + juce::AudioProcessor::getWrapperTypeDescription(processor.wrapperType) + dot
            + getBuildConfig() + " build";
    }

    // Collects what NOVA itself knows (versions, system, engine, logs, current rig) on
    // top of `info`, and saves the zip in `folder`. `savedFile` is the zip written.
    inline juce::Result writeReport(NOVAAudioProcessor& processor, Info info, const juce::File& folder, juce::File& savedFile)
    {
        using NovaDiagnostics::SessionLogger;
        const auto now = juce::Time::getCurrentTime();

        info.createdAt = now.formatted("%Y-%m-%d %H:%M:%S");
        info.novaVersion = getNovaVersion();
        info.buildConfig = getBuildConfig();
        info.pluginFormat = juce::AudioProcessor::getWrapperTypeDescription(processor.wrapperType);
        if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone
            && processor.wrapperType != juce::AudioProcessor::wrapperType_Undefined)
            info.host = juce::PluginHostType().getHostDescription();

        info.system.add("OS: " + juce::SystemStats::getOperatingSystemName()
                        + (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : ""));
        info.system.add("CPU: " + juce::SystemStats::getCpuModel() + ", "
                        + juce::String(juce::SystemStats::getNumCpus()) + " threads");
        info.system.add("RAM: " + juce::String(juce::SystemStats::getMemorySizeInMegabytes()) + " MB");
        info.system.add("JUCE: " + juce::SystemStats::getJUCEVersion());
        info.engineDiagnostics = processor.getAudioEngine().buildDiagnosticReport();

        const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
        const auto anonymize = [&home] (const juce::String& text) { return anonymizePaths(text, home); };

        std::vector<Entry> entries;
        entries.push_back(makeTextEntry("report.txt", anonymize(buildReportText(info))));

        SessionLogger::logEvent("report.export", "Exporting a problem report");
        SessionLogger::flush();
        const auto addLog = [&entries, &anonymize] (const juce::File& file, const char* name)
        {
            if (file.existsAsFile())
                entries.push_back(makeTextEntry(name, anonymize(file.loadFileAsString())));
        };
        addLog(SessionLogger::getLogFile(), "session-log.txt");
        addLog(SessionLogger::getPreviousLogFile(), "session-log-previous.txt");

        Entry rig;
        rig.name = "current-rig.nova-preset";
        processor.getStateInformation(rig.data);
        entries.push_back(std::move(rig));

        savedFile = folder.getChildFile(makeFileName(now)).getNonexistentSibling(false);
        const auto result = writeZip(savedFile, entries);
        SessionLogger::logEvent("report.export", result.wasOk() ? "Saved " + savedFile.getFileName()
                                                                : "Failed: " + result.getErrorMessage());
        return result;
    }
}
