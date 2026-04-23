#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include "Constants.h"

namespace NovaDiagnostics
{
class SessionLogger final : public juce::Logger,
                            private juce::Thread
{
public:
    struct QueueStatsSnapshot
    {
        int queuedEntries = 0;
        int peakQueuedEntries = 0;
        int droppedEntries = 0;
    };

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

    static QueueStatsSnapshot getQueueStats()
    {
        auto& logger = instance();
        QueueStatsSnapshot stats;
        stats.queuedEntries = logger.queuedEntries.load(std::memory_order_relaxed);
        stats.peakQueuedEntries = logger.peakQueuedEntries.load(std::memory_order_relaxed);
        stats.droppedEntries = logger.droppedEntries.load(std::memory_order_relaxed);
        return stats;
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
    struct PendingEntry
    {
        juce::String timestamp;
        juce::String category;
        juce::String message;
    };

    SessionLogger()
        : juce::Thread("NOVA SessionLogger")
    {
    }

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
        juce::Logger::setCurrentLogger(nullptr);
        sessionActive.store(false, std::memory_order_release);
        queueWakeEvent.signal();
        stopThread(2000);

        const juce::ScopedLock sl(writeLock);
        if (outputStream != nullptr)
            outputStream->flush();

        outputStream.reset();
    }

    void beginSession()
    {
        sessionActive.store(false, std::memory_order_release);
        queueWakeEvent.signal();
        stopThread(2000);

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

        resetQueueState();
        sessionActive.store(true, std::memory_order_release);
        juce::Logger::setCurrentLogger(this);
        startThread();
        writeEntryUnlocked(makeTimestamp(), "session", "Session start");
        writeEntryUnlocked(makeTimestamp(), "session", "Log file: " + logFile.getFullPathName());
        if (outputStream != nullptr)
            outputStream->flush();
    }

    void writeStructured(const juce::String& category, const juce::String& message)
    {
        if (!sessionActive.load(std::memory_order_acquire))
            return;

        enqueue(makeTimestamp(), category, message);
    }

    void enqueue(const juce::String& timestamp, const juce::String& category, const juce::String& message)
    {
        bool queuedSuccessfully = false;

        {
            const juce::SpinLock::ScopedLockType sl(queueLock);
            if (queuedEntryCount < (int) pendingEntries.size())
            {
                auto& entry = pendingEntries[(size_t) queueWriteIndex];
                entry.timestamp = timestamp;
                entry.category = category;
                entry.message = message;

                queueWriteIndex = (queueWriteIndex + 1) % (int) pendingEntries.size();
                ++queuedEntryCount;
                queuedEntries.store(queuedEntryCount, std::memory_order_relaxed);
                peakQueuedEntries.store(juce::jmax(peakQueuedEntries.load(std::memory_order_relaxed), queuedEntryCount),
                    std::memory_order_relaxed);
                queuedSuccessfully = true;
            }
        }

        if (!queuedSuccessfully)
            droppedEntries.fetch_add(1, std::memory_order_relaxed);

        queueWakeEvent.signal();
    }

    bool dequeue(PendingEntry& entry)
    {
        const juce::SpinLock::ScopedLockType sl(queueLock);
        if (queuedEntryCount <= 0)
            return false;

        entry = std::move(pendingEntries[(size_t) queueReadIndex]);
        pendingEntries[(size_t) queueReadIndex] = {};
        queueReadIndex = (queueReadIndex + 1) % (int) pendingEntries.size();
        --queuedEntryCount;
        queuedEntries.store(queuedEntryCount, std::memory_order_relaxed);
        return true;
    }

    void resetQueueState()
    {
        const juce::SpinLock::ScopedLockType sl(queueLock);
        pendingEntries = {};
        queueReadIndex = 0;
        queueWriteIndex = 0;
        queuedEntryCount = 0;
        queuedEntries.store(0, std::memory_order_relaxed);
        peakQueuedEntries.store(0, std::memory_order_relaxed);
        droppedEntries.store(0, std::memory_order_relaxed);
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            flushPendingEntries();
            queueWakeEvent.wait(Nova::Config::LOGGER_FLUSH_INTERVAL_MS);
        }

        flushPendingEntries();
    }

    void flushPendingEntries()
    {
        const int droppedSinceLastFlush = droppedEntries.exchange(0, std::memory_order_acq_rel);
        PendingEntry entry;
        bool wroteAnything = false;

        const juce::ScopedLock sl(writeLock);
        if (outputStream == nullptr)
            return;

        if (droppedSinceLastFlush > 0)
        {
            writeEntryUnlocked(makeTimestamp(),
                "session.logger",
                "Dropped " + juce::String(droppedSinceLastFlush) + " async log events because the queue filled");
            wroteAnything = true;
        }

        while (dequeue(entry))
        {
            writeEntryUnlocked(entry.timestamp, entry.category, entry.message);
            wroteAnything = true;
        }

        if (wroteAnything)
            outputStream->flush();
    }

    void writeEntryUnlocked(const juce::String& timestamp, const juce::String& category, const juce::String& message)
    {
        const juce::String prefix = "[" + timestamp + "] [" + category + "] ";

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
    }

    juce::CriticalSection writeLock;
    std::unique_ptr<juce::FileOutputStream> outputStream;
    juce::File logFile;
    std::atomic<int> ownerCount{ 0 };
    std::atomic<bool> sessionActive{ false };
    std::atomic<int> queuedEntries{ 0 };
    std::atomic<int> peakQueuedEntries{ 0 };
    std::atomic<int> droppedEntries{ 0 };
    juce::SpinLock queueLock;
    std::array<PendingEntry, Nova::Config::LOGGER_QUEUE_CAPACITY> pendingEntries;
    int queueReadIndex = 0;
    int queueWriteIndex = 0;
    int queuedEntryCount = 0;
    juce::WaitableEvent queueWakeEvent;
};
}
