#pragma once
#include <JuceHeader.h>
#include "../../Core/Constants.h"

// =============================================================================
// Reusable wizard overlay with step navigation, progress indicator, and premium
// dark theme matching the NOVA settings aesthetic.
// =============================================================================
class WizardBase : public juce::Component
{
public:
    struct StepDef
    {
        juce::String label;     // Short text for the step dot (e.g. "Driver")
        juce::String title;     // Heading shown in the content area
        juce::String subtitle;  // Description below the heading
    };

    WizardBase(std::vector<StepDef> stepDefs,
               std::function<void()> onCloseFn)
        : steps(std::move(stepDefs)),
          onClose(std::move(onCloseFn))
    {
        jassert(!steps.empty());
        setWantsKeyboardFocus(true);

        addAndMakeVisible(btnClose);
        btnClose.setButtonText("CLOSE");
        btnClose.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff2A1418"));
        btnClose.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.88f));
        btnClose.onClick = [this] { closeWizard(); };

        addAndMakeVisible(btnBack);
        btnBack.setButtonText("BACK");
        btnBack.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff182131"));
        btnBack.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.82f));
        btnBack.onClick = [this] { goBack(); };

        addAndMakeVisible(btnNext);
        styleAccentButton(btnNext, "NEXT");
        btnNext.onClick = [this] { goNext(); };

        addAndMakeVisible(btnSkip);
        btnSkip.setButtonText("SKIP");
        btnSkip.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        btnSkip.setColour(juce::TextButton::textColourOffId, Nova::Colors::TextDim);
        btnSkip.onClick = [this] { goNext(); };
        btnSkip.setVisible(false);
    }

    ~WizardBase() override
    {
        for (auto& p : pages)
            if (p != nullptr) p->setLookAndFeel(nullptr);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)   { closeWizard(); return true; }
        if (key == juce::KeyPress::returnKey)   { goNext();      return true; }
        return false;
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (!panelBounds.contains(e.getPosition()))
            closeWizard();
    }

    // --- Step management API for subclasses -----------------------------------

    int  getCurrentStep() const              { return currentStep; }
    int  getStepCount() const                { return (int)steps.size(); }
    bool isLastStep() const                  { return currentStep == (int)steps.size() - 1; }

    void setStepSkippable(bool skippable)    { btnSkip.setVisible(skippable); }
    void setNextEnabled(bool enabled)        { btnNext.setEnabled(enabled); }
    void setNextText(const juce::String& t)  { btnNext.setButtonText(t); }

    void goNext()
    {
        if (!canAdvance(currentStep)) return;
        onStepLeaving(currentStep);

        if (isLastStep())
        {
            onFinished();
            closeWizard();
            return;
        }

        setStep(currentStep + 1);
    }

    void goBack()
    {
        if (currentStep > 0)
            setStep(currentStep - 1);
    }

    void setStep(int idx)
    {
        if (idx < 0 || idx >= (int)steps.size()) return;
        currentStep = idx;

        for (int i = 0; i < (int)pages.size(); ++i)
            if (pages[i] != nullptr)
                pages[i]->setVisible(i == currentStep);

        btnBack.setVisible(currentStep > 0);
        btnSkip.setVisible(false);
        btnNext.setButtonText(isLastStep() ? "FINISH" : "NEXT");
        btnNext.setEnabled(true);

        onStepActivated(currentStep);
        resized();
        repaint();
    }

    // --- Overridable hooks ----------------------------------------------------

    virtual bool canAdvance(int /*step*/)          { return true; }
    virtual void onStepActivated(int /*step*/)     {}
    virtual void onStepLeaving(int /*step*/)       {}
    virtual void onFinished()                      {}

    // --- Page registration (call from subclass constructor) --------------------

    void addPage(int stepIndex, juce::Component* page)
    {
        if (stepIndex >= (int)pages.size())
            pages.resize(stepIndex + 1);

        pages[stepIndex].reset(page);
        addAndMakeVisible(page);
        page->setVisible(stepIndex == currentStep);
    }

    juce::Rectangle<int> getContentArea() const { return contentArea; }

    // --- Paint ----------------------------------------------------------------

    void paint(juce::Graphics& g) override
    {
        // Scrim
        g.fillAll(juce::Colours::black.withAlpha(0.90f));

        // Panel
        auto pf = panelBounds.toFloat();
        juce::ColourGradient panelFill(juce::Colour::fromString("ff121A27"),
            pf.getCentreX(), pf.getY(),
            juce::Colour::fromString("ff0B0E14"),
            pf.getCentreX(), pf.getBottom(), false);
        g.setGradientFill(panelFill);
        g.fillRoundedRectangle(pf, 22.0f);

        g.setColour(Nova::Colors::Border.withAlpha(0.85f));
        g.drawRoundedRectangle(pf.reduced(0.5f), 22.0f, 1.0f);

        // Header tint
        g.setColour(Nova::Colors::Accent.withAlpha(0.06f));
        g.fillRoundedRectangle(pf.reduced(1.0f).removeFromTop(80.0f), 22.0f);

        // Step indicator
        paintStepIndicator(g);

        // Step title + subtitle
        if (currentStep < (int)steps.size())
        {
            auto titleArea = contentArea.withHeight(56);
            g.setColour(Nova::Colors::Accent);
            g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
            g.drawText(steps[currentStep].title, titleArea.removeFromTop(26),
                       juce::Justification::centredLeft);

            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawFittedText(steps[currentStep].subtitle, titleArea,
                             juce::Justification::centredLeft, 2);
        }
    }

    void resized() override
    {
        const int panelW = juce::jlimit(780, 1200, (int)std::round(getWidth() * 0.80));
        const int panelH = juce::jlimit(520, 820,  (int)std::round(getHeight() * 0.82));
        panelBounds = juce::Rectangle<int>(0, 0, panelW, panelH)
                          .withCentre(getLocalBounds().getCentre());

        const int pad = 28;
        const int navH = 48;
        const int stepIndicatorH = 56;
        const int titleH = 60;

        auto inner = panelBounds.reduced(pad);

        // Close button
        btnClose.setBounds(panelBounds.getRight() - 110, panelBounds.getY() + 18, 82, 28);

        // Step indicator area (top)
        stepIndicatorBounds = inner.removeFromTop(stepIndicatorH);
        inner.removeFromTop(8);

        // Navigation bar (bottom)
        auto navBar = inner.removeFromBottom(navH);
        inner.removeFromBottom(12);

        // Content = remaining
        contentArea = inner;

        // Layout nav buttons
        const int navBtnW = 120;
        const int navBtnH = 36;
        btnBack.setBounds(navBar.getX(), navBar.getCentreY() - navBtnH / 2, navBtnW, navBtnH);
        btnSkip.setBounds(navBar.getCentreX() - navBtnW / 2, navBar.getCentreY() - navBtnH / 2, navBtnW, navBtnH);
        btnNext.setBounds(navBar.getRight() - navBtnW, navBar.getCentreY() - navBtnH / 2, navBtnW, navBtnH);

        // Page content (offset below title)
        auto pageArea = contentArea.withTrimmedTop(titleH);
        for (auto& p : pages)
            if (p != nullptr) p->setBounds(pageArea);

        btnBack.setVisible(currentStep > 0);
    }

protected:
    void closeWizard()
    {
        if (onClose) onClose();
    }

    static void styleAccentButton(juce::TextButton& btn, const juce::String& text)
    {
        btn.setButtonText(text);
        btn.setColour(juce::TextButton::buttonColourId, Nova::Colors::Accent.withAlpha(0.22f));
        btn.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text);
    }

    static void styleSecondaryButton(juce::TextButton& btn, const juce::String& text)
    {
        btn.setButtonText(text);
        btn.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff182131"));
        btn.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.85f));
    }

private:
    void paintStepIndicator(juce::Graphics& g) const
    {
        const int n = (int)steps.size();
        if (n <= 1) return;

        const float dotRadius = 14.0f;
        const float lineY = stepIndicatorBounds.getCentreY() - 6.0f;
        const float labelY = lineY + dotRadius + 6.0f;
        const float totalW = (float)stepIndicatorBounds.getWidth();
        const float margin = 40.0f;
        const float usableW = totalW - margin * 2.0f;
        const float spacing = usableW / (float)(n - 1);
        const float startX = (float)stepIndicatorBounds.getX() + margin;

        // Connecting lines
        for (int i = 0; i < n - 1; ++i)
        {
            const float x1 = startX + (float)i * spacing + dotRadius;
            const float x2 = startX + (float)(i + 1) * spacing - dotRadius;

            g.setColour(i < currentStep
                ? Nova::Colors::Accent.withAlpha(0.6f)
                : Nova::Colors::Border.withAlpha(0.4f));
            g.drawLine(x1, lineY, x2, lineY, 2.0f);
        }

        // Dots and labels
        for (int i = 0; i < n; ++i)
        {
            const float cx = startX + (float)i * spacing;
            const bool completed = i < currentStep;
            const bool active    = i == currentStep;

            if (active)
            {
                g.setColour(Nova::Colors::Accent.withAlpha(0.15f));
                g.fillEllipse(cx - dotRadius - 3.0f, lineY - dotRadius - 3.0f,
                              (dotRadius + 3.0f) * 2.0f, (dotRadius + 3.0f) * 2.0f);
            }

            g.setColour(completed ? Nova::Colors::Accent
                      : active    ? Nova::Colors::Accent
                      :             Nova::Colors::Border.withAlpha(0.5f));
            g.fillEllipse(cx - dotRadius, lineY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);

            // Number inside dot
            g.setColour(completed || active ? Nova::Colors::Background : Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText(juce::String(i + 1),
                       juce::Rectangle<float>(cx - dotRadius, lineY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f).toNearestInt(),
                       juce::Justification::centred);

            // Label below
            g.setColour(active ? Nova::Colors::Text : Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            g.drawText(steps[i].label,
                       juce::Rectangle<float>(cx - 40.0f, labelY, 80.0f, 16.0f).toNearestInt(),
                       juce::Justification::centred);
        }
    }

    std::vector<StepDef> steps;
    std::vector<std::unique_ptr<juce::Component>> pages;
    std::function<void()> onClose;
    int currentStep = 0;

    juce::TextButton btnClose;
    juce::TextButton btnBack;
    juce::TextButton btnNext;
    juce::TextButton btnSkip;

    juce::Rectangle<int> panelBounds;
    juce::Rectangle<int> stepIndicatorBounds;
    juce::Rectangle<int> contentArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WizardBase)
};
