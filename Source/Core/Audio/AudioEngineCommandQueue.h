#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <deque>

namespace Nova
{
namespace Audio
{
    template <typename Command>
    class AudioEngineCommandQueue
    {
    public:
        void enqueue(const Command& command)
        {
            const juce::ScopedLock lock(commandLock);
            pendingCommands.push_back(command);
            commandsPending.store(true, std::memory_order_release);
        }

        bool hasPending() const noexcept
        {
            return commandsPending.load(std::memory_order_acquire);
        }

        std::deque<Command> drain()
        {
            std::deque<Command> commands;
            const juce::ScopedLock lock(commandLock);
            commands.swap(pendingCommands);
            commandsPending.store(false, std::memory_order_release);
            return commands;
        }

    private:
        juce::CriticalSection commandLock;
        std::deque<Command> pendingCommands;
        std::atomic<bool> commandsPending{ false };
    };
}
}
