#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

class SimpleOscilloscope : public juce::Component, public juce::Timer
{
public:
    SimpleOscilloscope()
    {
        startTimerHz(60);
    }

    void pushBuffer(const juce::AudioBuffer<float>& buffer)
    {
        if (buffer.getNumChannels() <= 0)
            return;

        auto* channelData = buffer.getReadPointer(0);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            history[(size_t)historyIndex] = channelData[i];
            historyIndex = (historyIndex + 1) % fifoSize;
        }

        const int nextFrame = 1 - publishedFrame.load(std::memory_order_relaxed);
        auto& frame = frames[(size_t)nextFrame];

        for (int i = 0; i < fifoSize; ++i)
            frame[(size_t)i] = history[(size_t)((historyIndex + i) % fifoSize)];

        publishedFrame.store(nextFrame, std::memory_order_release);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour::fromString("ff151515"));

        g.setColour(juce::Colours::white.withAlpha(0.05f));
        g.drawHorizontalLine(getHeight() / 2, 0, (float)getWidth());
        for (int i = 0; i < getWidth(); i += 40)
            g.drawVerticalLine(i, 0.0f, (float)getHeight());

        juce::Path p;
        const float w = (float)getWidth();
        const float h = (float)getHeight();
        const float mid = h * 0.5f;

        const int frameIndex = publishedFrame.load(std::memory_order_acquire);
        const auto& frame = frames[(size_t)frameIndex];

        p.startNewSubPath(-5.0f, mid);

        const int resolution = 2;
        for (int x = 0; x < w + 5; x += resolution)
        {
            const int bufferIdx = juce::jlimit(0, fifoSize - 1,
                juce::roundToInt((static_cast<float>(x) / juce::jmax(1.0f, w))
                    * static_cast<float>(fifoSize - 1)));

            float sample = frame[(size_t)bufferIdx];
            sample = juce::jlimit(-1.0f, 1.0f, sample);

            const float y = mid - (sample * mid * 0.9f);
            p.lineTo((float)x, y);
        }

        const auto baseColour = juce::Colour::fromString("ffebac26");

        g.setColour(baseColour.withAlpha(0.1f));
        g.strokePath(p, juce::PathStrokeType(14.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour(baseColour.withAlpha(0.3f));
        g.strokePath(p, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour(baseColour.withAlpha(0.9f));
        g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void timerCallback() override
    {
        repaint();
    }

private:
    static constexpr int fifoSize = 4096;
    std::array<float, fifoSize> history{ {} };
    std::array<std::array<float, fifoSize>, 2> frames{ {} };
    int historyIndex = 0;
    std::atomic<int> publishedFrame{ 0 };
};