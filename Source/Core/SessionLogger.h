#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace NovaDiagnostics
{
class SessionLogger final : public juce::Logger
{
public:
    static SessionLogger& instance()
    {
        static SessionLogger logger;
        return logger;
    }

    static void attachOwner(const juce::String& ownerName)
    {
        auto& logger = instance();
        const int previousOwners = logger.ownerCount.fetch_add(1, std::memory_order_acq_rel);

        if (previousOwners == 0)
            logger.beginSession();

        logEvent("session.owner", ownerName + " attached. owners=" + juce::String(previousOwners + 1));
    }

    static void detachOwner(const juce::String& ownerName)
    {
        auto& logger = instance();
        const int previousOwners = logger.ownerCount.fetch_sub(1, std::memory_order_acq_rel);
        const int remainingOwners = juce::jmax(0, previousOwners - 1);

        logEvent("session.owner", ownerName + " detached. owners=" + juce::String(remainingOwners));

        if (remainingOwners == 0)
        {
            logEvent("session", "Session end");
            logger.endSession();
        }
    }

    static juce::File getLogFile()
    {
        return instance().logFile;
    }

    static void logEvent(const juce::String& category, const juce::String& message)
    {
        instance().writeStructured(category, message);
    }

    static void logValueTree(const juce::String& category, const juce::String& label, const juce::ValueTree& tree)
    {
        auto payload = label;
        payload << juce::newLine << dumpValueTree(tree);
        logEvent(category, payload);
    }

    static juce::String dumpValueTree(const juce::ValueTree& tree, int indentLevel = 0)
    {
        juce::String result;
        const juce::String indent = juce::String::repeatedString(" ", indentLevel * 2);

        if (!tree.isValid())
        {
            result << indent << "<invalid>" << juce::newLine;
            return result;
        }

        result << indent << tree.getType().toString();

        if (tree.getNumProperties() > 0)
        {
            result << " { ";

            for (int i = 0; i < tree.getNumProperties(); ++i)
            {
                const auto name = tree.getPropertyName(i).toString();
                const auto value = tree.getProperty(tree.getPropertyName(i)).toString();
                if (i > 0)
                    result << ", ";

                result << name << "=" << value;
            }

            result << " }";
        }

        result << juce::newLine;

        for (int i = 0; i < tree.getNumChildren(); ++i)
            result << dumpValueTree(tree.getChild(i), indentLevel + 1);

        return result;
    }

    void logMessage(const juce::String& message) override
    {
        writeStructured("juce", message);
    }

private:
    SessionLogger() = default;

    static juce::String makeTimestamp()
    {
        const auto now = juce::Time::getCurrentTime();
        return now.formatted("%Y-%m-%d %H:%M:%S");
    }

    static juce::File getSessionLogPath()
    {
        auto appDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("NOVA");

        if (!appDir.exists())
            appDir.createDirectory();

        auto logsDir = appDir.getChildFile("Logs");
        if (!logsDir.exists())
            logsDir.createDirectory();

        return logsDir.getChildFile("session-log.txt");
    }

    void endSession()
    {
        const juce::ScopedLock sl(writeLock);
        juce::Logger::setCurrentLogger(nullptr);
        outputStream.reset();
    }

    void beginSession()
    {
        const juce::ScopedLock sl(writeLock);

        logFile = getSessionLogPath();
        if (logFile.existsAsFile())
            logFile.deleteFile();

        outputStream = logFile.createOutputStream();

        if (outputStream != nullptr)
        {
            outputStream->setPosition(0);
            outputStream->truncate();
        }

        juce::Logger::setCurrentLogger(this);
        writeLineUnlocked("session", "Session start");
        writeLineUnlocked("session", "Log file: " + logFile.getFullPathName());
    }

    void writeStructured(const juce::String& category, const juce::String& message)
    {
        const juce::ScopedLock sl(writeLock);
        writeLineUnlocked(category, message);
    }

    void writeLineUnlocked(const juce::String& category, const juce::String& message)
    {
        const juce::String prefix = "[" + makeTimestamp() + "] [" + category + "] ";

        if (outputStream == nullptr)
            return;

        juce::StringArray lines;
        lines.addLines(message);

        if (lines.isEmpty())
            lines.add({});

        for (int i = 0; i < lines.size(); ++i)
        {
            const auto& line = lines[i];
            outputStream->writeText(prefix + line + juce::newLine, false, false, "\n");
        }

        outputStream->flush();
    }

    juce::CriticalSection writeLock;
    std::unique_ptr<juce::FileOutputStream> outputStream;
    juce::File logFile;
    std::atomic<int> ownerCount{ 0 };
};
}
