#pragma once

#include <JuceHeader.h>
#include <deque>

#include "../../Core/Constants.h"
#include "../../Core/Audio/UserAdvisor.h"

// =============================================================================
// Shows what UserAdvisor has to say, without getting in the way.
//
// These messages arrive while someone is playing, so nothing here is modal and
// nothing steals focus: cards fade in at the bottom-right, sit for a few
// seconds and leave. hitTest() only claims the pixels a card occupies, so the
// pedalboard underneath stays fully usable while one is on screen.
// =============================================================================
class AdviceOverlay final : public juce::Component, private juce::Timer
{
public:
    AdviceOverlay()
    {
        setWantsKeyboardFocus(false);
        startTimerHz(30);
    }

    void show(const Nova::Audio::UserAdvisor::Advice& advice)
    {
        // A repeat of something already on screen refreshes it rather than
        // stacking a duplicate.
        for (auto& card : cards)
        {
            if (card.advice.id == advice.id)
            {
                card.advice = advice;
                card.shownAt = juce::Time::getMillisecondCounterHiRes();
                card.dismissing = false;
                repaint();
                return;
            }
        }

        Card card;
        card.advice = advice;
        card.shownAt = juce::Time::getMillisecondCounterHiRes();
        cards.push_back(std::move(card));

        // Three at once is already a lot to read mid-song; drop the oldest.
        while (cards.size() > (size_t) maxVisibleCards)
            cards.pop_front();

        resized();
        repaint();
    }

    void clear()
    {
        cards.clear();
        repaint();
    }

    bool hasVisibleAdvice() const noexcept { return !cards.empty(); }

    // Only the cards themselves are clickable; everything else passes through
    // to the editor beneath.
    bool hitTest(int x, int y) override
    {
        for (const auto& card : cards)
            if (card.bounds.contains(x, y))
                return true;

        return false;
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        for (auto& card : cards)
        {
            if (card.bounds.contains(event.getPosition()))
            {
                card.dismissing = true;
                return;
            }
        }
    }

    void resized() override
    {
        layoutCards();
    }

    void paint(juce::Graphics& g) override
    {
        for (const auto& card : cards)
        {
            const float alpha = card.alpha;
            if (alpha <= 0.01f)
                continue;

            auto bounds = card.bounds.toFloat();
            const auto accent = colourForLevel(card.advice.level);

            g.setColour(juce::Colour::fromString("ff0E1520").withAlpha(0.96f * alpha));
            g.fillRoundedRectangle(bounds, 10.0f);
            g.setColour(accent.withAlpha(0.45f * alpha));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.0f);

            // Level stripe: colour carries the urgency so the text does not
            // have to shout it.
            g.setColour(accent.withAlpha(0.9f * alpha));
            g.fillRoundedRectangle(bounds.withWidth(4.0f).reduced(0.0f, 8.0f), 2.0f);

            auto text = card.bounds.reduced(16, 12).withTrimmedLeft(4);

            g.setColour(accent.withAlpha(alpha));
            g.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
            g.drawText(card.advice.title, text.removeFromTop(18),
                juce::Justification::centredLeft, true);

            text.removeFromTop(2);
            g.setColour(Nova::Colors::Text.withAlpha(0.85f * alpha));
            g.setFont(juce::Font(juce::FontOptions(11.5f)));
            g.drawFittedText(card.advice.message, text, juce::Justification::topLeft, 3);

            g.setColour(Nova::Colors::TextDim.withAlpha(0.5f * alpha));
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.drawText("CLICK TO DISMISS",
                card.bounds.withTop(card.bounds.getBottom() - 14).withTrimmedRight(12),
                juce::Justification::centredRight);
        }
    }

private:
    struct Card
    {
        Nova::Audio::UserAdvisor::Advice advice;
        juce::Rectangle<int> bounds;
        double shownAt = 0.0;
        float alpha = 0.0f;
        bool dismissing = false;
    };

    static juce::Colour colourForLevel(Nova::Audio::UserAdvisor::Level level)
    {
        switch (level)
        {
            case Nova::Audio::UserAdvisor::Level::Problem: return Nova::Colors::Error;
            case Nova::Audio::UserAdvisor::Level::Caution: return juce::Colour::fromString("ffF59E0B");
            case Nova::Audio::UserAdvisor::Level::Info:
            default:                                       return Nova::Colors::Accent;
        }
    }

    // A problem the user just heard deserves longer on screen than a tip.
    static double lifetimeSeconds(Nova::Audio::UserAdvisor::Level level)
    {
        switch (level)
        {
            case Nova::Audio::UserAdvisor::Level::Problem: return 12.0;
            case Nova::Audio::UserAdvisor::Level::Caution: return 9.0;
            case Nova::Audio::UserAdvisor::Level::Info:
            default:                                       return 7.0;
        }
    }

    void layoutCards()
    {
        const int cardW = juce::jmin(360, juce::jmax(200, getWidth() - 40));
        const int cardH = 88;
        const int margin = 18;
        const int gap = 8;

        int y = getHeight() - margin - cardH;
        for (auto it = cards.rbegin(); it != cards.rend(); ++it)
        {
            it->bounds = juce::Rectangle<int>(getWidth() - margin - cardW, y, cardW, cardH);
            y -= cardH + gap;
        }
    }

    void timerCallback() override
    {
        if (cards.empty())
            return;

        const double now = juce::Time::getMillisecondCounterHiRes();
        bool needsRepaint = false;
        bool removed = false;

        for (auto& card : cards)
        {
            const double age = (now - card.shownAt) / 1000.0;
            if (!card.dismissing && age > lifetimeSeconds(card.advice.level))
                card.dismissing = true;

            const float target = card.dismissing ? 0.0f : 1.0f;
            if (std::abs(card.alpha - target) > 0.001f)
            {
                card.alpha += (target - card.alpha) * 0.18f;
                needsRepaint = true;
            }
        }

        for (auto it = cards.begin(); it != cards.end();)
        {
            if (it->dismissing && it->alpha < 0.02f)
            {
                it = cards.erase(it);
                removed = true;
            }
            else
            {
                ++it;
            }
        }

        if (removed)
            layoutCards();

        if (needsRepaint || removed)
            repaint();
    }

    static constexpr int maxVisibleCards = 3;
    std::deque<Card> cards;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AdviceOverlay)
};
