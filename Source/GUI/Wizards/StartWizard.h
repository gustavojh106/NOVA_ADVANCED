#pragma once
#include "WizardBase.h"
#include "../../Core/Constants.h"

// =============================================================================
// Start / Launcher wizard - first-run welcome, future login/profile hub,
// and app suite entry point.  All account/cloud features are placeholder UI
// until the backend ships.
// =============================================================================
class StartWizard final : public WizardBase
{
public:
    StartWizard(std::function<void()> onCloseFn,
                std::function<void()> onLaunchAudioSetup = nullptr,
                std::function<void()> onLaunchPresetFinder = nullptr)
        : WizardBase(buildSteps(), std::move(onCloseFn)),
          launchAudioSetup(std::move(onLaunchAudioSetup)),
          launchPresetFinder(std::move(onLaunchPresetFinder))
    {
        // --- Step 0: Welcome splash -----------------------------------------
        addPage(0, new juce::Component());

        // --- Step 1: Account / Login (placeholder) --------------------------
        auto* loginPage = new juce::Component();
        addPage(1, loginPage);

        loginEmailEditor.setTextToShowWhenEmpty("Email address", Nova::Colors::TextDim);
        loginEmailEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff101722"));
        loginEmailEditor.setColour(juce::TextEditor::textColourId, Nova::Colors::Text);
        loginEmailEditor.setColour(juce::TextEditor::outlineColourId, Nova::Colors::Border.withAlpha(0.7f));
        loginPage->addAndMakeVisible(loginEmailEditor);

        loginPasswordEditor.setTextToShowWhenEmpty("Password", Nova::Colors::TextDim);
        loginPasswordEditor.setPasswordCharacter(0x2022);
        loginPasswordEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff101722"));
        loginPasswordEditor.setColour(juce::TextEditor::textColourId, Nova::Colors::Text);
        loginPasswordEditor.setColour(juce::TextEditor::outlineColourId, Nova::Colors::Border.withAlpha(0.7f));
        loginPage->addAndMakeVisible(loginPasswordEditor);

        styleAccentButton(btnLogin, "LOG IN");
        loginPage->addAndMakeVisible(btnLogin);
        btnLogin.onClick = [] {}; // placeholder

        styleSecondaryButton(btnCreateAccount, "CREATE ACCOUNT");
        loginPage->addAndMakeVisible(btnCreateAccount);
        btnCreateAccount.onClick = [] {};

        loginStatusLabel.setText("Account features are coming soon.\nSkip this step to continue offline.",
                                juce::dontSendNotification);
        loginStatusLabel.setColour(juce::Label::textColourId, Nova::Colors::TextDim);
        loginStatusLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
        loginStatusLabel.setJustificationType(juce::Justification::centred);
        loginPage->addAndMakeVisible(loginStatusLabel);

        // --- Step 2: Player Profile -----------------------------------------
        auto* profilePage = new juce::Component();
        addPage(2, profilePage);

        profileNameEditor.setTextToShowWhenEmpty("Your display name", Nova::Colors::TextDim);
        profileNameEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff101722"));
        profileNameEditor.setColour(juce::TextEditor::textColourId, Nova::Colors::Text);
        profileNameEditor.setColour(juce::TextEditor::outlineColourId, Nova::Colors::Border.withAlpha(0.7f));
        profilePage->addAndMakeVisible(profileNameEditor);

        skillLevelBox.addItem("Beginner", 1);
        skillLevelBox.addItem("Intermediate", 2);
        skillLevelBox.addItem("Advanced", 3);
        skillLevelBox.addItem("Professional", 4);
        skillLevelBox.setSelectedId(2, juce::dontSendNotification);
        skillLevelBox.setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromString("ff101722"));
        skillLevelBox.setColour(juce::ComboBox::textColourId, Nova::Colors::Text);
        skillLevelBox.setColour(juce::ComboBox::outlineColourId, Nova::Colors::Border.withAlpha(0.7f));
        profilePage->addAndMakeVisible(skillLevelBox);

        // Genre toggles
        static constexpr const char* genres[] = {
            "Rock", "Metal", "Blues", "Jazz", "Pop", "Country", "Funk", "Ambient"
        };
        for (int i = 0; i < 8; ++i)
        {
            genreToggles[i].setButtonText(genres[i]);
            genreToggles[i].setColour(juce::ToggleButton::textColourId, Nova::Colors::Text.withAlpha(0.88f));
            genreToggles[i].setColour(juce::ToggleButton::tickColourId, Nova::Colors::Accent);
            profilePage->addAndMakeVisible(genreToggles[i]);
        }

        // --- Step 3: App Suite Hub ------------------------------------------
        addPage(3, new juce::Component());

        // --- Step 4: Quick Links / Summary ----------------------------------
        auto* linksPage = new juce::Component();
        addPage(4, linksPage);

        if (launchAudioSetup)
        {
            styleAccentButton(btnGoAudioSetup, "AUDIO SETUP WIZARD");
            linksPage->addAndMakeVisible(btnGoAudioSetup);
            btnGoAudioSetup.onClick = [this]
            {
                closeWizard();
                if (launchAudioSetup) launchAudioSetup();
            };
        }

        if (launchPresetFinder)
        {
            styleSecondaryButton(btnGoPresetFinder, "PRESET FINDER WIZARD");
            linksPage->addAndMakeVisible(btnGoPresetFinder);
            btnGoPresetFinder.onClick = [this]
            {
                closeWizard();
                if (launchPresetFinder) launchPresetFinder();
            };
        }

        // Skippable login + profile
        setStep(0);
        onStepActivated(0);
    }

    void onStepActivated(int step) override
    {
        setStepSkippable(step == 1 || step == 2);

        if (step == getStepCount() - 1)
            setNextText("FINISH");
    }

    void onFinished() override
    {
        // Save profile if the user entered data
        const auto name = profileNameEditor.getText().trim();
        if (name.isNotEmpty())
            EditorPrefs::setString("user.displayName", name);

        EditorPrefs::setString("user.skillLevel", skillLevelBox.getText());

        juce::String genreList;
        for (int i = 0; i < 8; ++i)
        {
            if (genreToggles[i].getToggleState())
            {
                if (genreList.isNotEmpty()) genreList += ",";
                genreList += genreToggles[i].getButtonText();
            }
        }
        if (genreList.isNotEmpty())
            EditorPrefs::setString("user.genres", genreList);
    }

    void resized() override
    {
        WizardBase::resized();
        auto area = getContentArea().withTrimmedTop(60);

        // Step 1: login form
        {
            auto loginArea = area;
            const int fieldW = juce::jmin(380, loginArea.getWidth());
            const int fieldH = 36;
            const int gap = 14;

            auto centeredX = loginArea.getCentreX() - fieldW / 2;
            auto y = loginArea.getY();

            loginEmailEditor.setBounds(centeredX, y, fieldW, fieldH);
            y += fieldH + gap;
            loginPasswordEditor.setBounds(centeredX, y, fieldW, fieldH);
            y += fieldH + gap + 4;

            const int btnW = (fieldW - gap) / 2;
            btnLogin.setBounds(centeredX, y, btnW, fieldH);
            btnCreateAccount.setBounds(centeredX + btnW + gap, y, btnW, fieldH);
            y += fieldH + gap + 8;
            loginStatusLabel.setBounds(centeredX, y, fieldW, 40);
        }

        // Step 2: profile
        {
            auto profArea = area;
            const int fieldW = juce::jmin(380, profArea.getWidth());
            const int fieldH = 36;
            const int gap = 12;

            auto centeredX = profArea.getCentreX() - fieldW / 2;
            auto y = profArea.getY();

            profileNameEditor.setBounds(centeredX, y, fieldW, fieldH);
            y += fieldH + gap;
            skillLevelBox.setBounds(centeredX, y, fieldW, fieldH);
            y += fieldH + gap + 8;

            // Genre grid: 4 columns x 2 rows
            const int toggleW = (fieldW - gap * 3) / 4;
            const int toggleH = 28;
            for (int i = 0; i < 8; ++i)
            {
                const int col = i % 4;
                const int row = i / 4;
                genreToggles[i].setBounds(centeredX + col * (toggleW + gap),
                                          y + row * (toggleH + 4),
                                          toggleW, toggleH);
            }
        }

        // Step 4: quick links
        {
            auto linksArea = area;
            const int btnW = juce::jmin(300, linksArea.getWidth());
            const int btnH = 44;
            const int gap = 16;
            auto centeredX = linksArea.getCentreX() - btnW / 2;
            auto y = linksArea.getY() + 30;

            btnGoAudioSetup.setBounds(centeredX, y, btnW, btnH);
            y += btnH + gap;
            btnGoPresetFinder.setBounds(centeredX, y, btnW, btnH);
        }
    }

    void paint(juce::Graphics& g) override
    {
        WizardBase::paint(g);
        const auto area = getContentArea().withTrimmedTop(60);

        if (getCurrentStep() == 0)
        {
            paintWelcomeStep(g, area);
        }
        else if (getCurrentStep() == 2)
        {
            // Profile labels
            auto profArea = area;
            const int fieldW = juce::jmin(380, profArea.getWidth());
            auto centeredX = profArea.getCentreX() - fieldW / 2;

            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawText("DISPLAY NAME", centeredX, profArea.getY() - 16, fieldW, 14, juce::Justification::centredLeft);
            g.drawText("SKILL LEVEL", centeredX, profArea.getY() + 36 + 12 - 16, fieldW, 14, juce::Justification::centredLeft);
            g.drawText("FAVORITE GENRES", centeredX, profArea.getY() + 36 * 2 + 12 * 2 + 8 - 16, fieldW, 14, juce::Justification::centredLeft);
        }
        else if (getCurrentStep() == 3)
        {
            paintAppHubStep(g, area);
        }
        else if (getCurrentStep() == 4)
        {
            paintLinksStep(g, area);
        }
    }

private:
    static std::vector<StepDef> buildSteps()
    {
        return {
            { "Welcome",  "Welcome to NOVA Audio Suite",
              "Your gateway to professional guitar tone." },
            { "Account",  "Account",
              "Sign in or create an account to sync presets and unlock cloud features." },
            { "Profile",  "Player Profile",
              "Tell NOVA about yourself for personalized tone recommendations." },
            { "Apps",     "NOVA Audio Suite",
              "Your apps at a glance. More titles are coming to the suite." },
            { "Go!",      "You're All Set",
              "Jump into NOVA or run a setup wizard." }
        };
    }

    void paintWelcomeStep(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        // Logo area
        auto logoArea = area.removeFromTop(100).withSizeKeepingCentre(100, 100);
        g.setColour(Nova::Colors::Accent.withAlpha(0.10f));
        g.fillRoundedRectangle(logoArea.toFloat(), 24.0f);
        g.setColour(Nova::Colors::Accent);
        g.setFont(juce::Font(juce::FontOptions(42.0f, juce::Font::bold)));
        g.drawText("N", logoArea, juce::Justification::centred);

        area.removeFromTop(20);

        g.setColour(Nova::Colors::Text);
        g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
        g.drawText("NOVA", area.removeFromTop(30), juce::Justification::centred);

        area.removeFromTop(6);

        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawFittedText(
            "NOVA Audio Suite is your home for professional guitar processing.\n\n"
            "This wizard will help you set up your account, build a player profile, "
            "and explore the suite. You can skip any step and come back later.\n\n"
            "More apps are coming to the NOVA ecosystem - stay tuned.",
            area.reduced(40, 0), juce::Justification::centredTop, 8);
    }

    void paintAppHubStep(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        // App tiles grid
        struct AppTile
        {
            const char* name;
            const char* subtitle;
            const char* icon;
            juce::Colour accent;
            bool available;
        };

        const AppTile tiles[] = {
            { "NOVA",       "Multi-FX Processor", "N", Nova::Colors::Accent, true },
            { "NOVA Amp",   "Neural Amp Modeler",  "A", juce::Colour::fromString("ffEF4444"), false },
            { "NOVA Synth", "Guitar Synthesizer", "S", juce::Colour::fromString("ff818CF8"), false },
            { "NOVA Loop",  "Live Looper",        "L", juce::Colour::fromString("ff34D399"), false }
        };

        const int tileW = juce::jmin(200, (area.getWidth() - 60) / 2);
        const int tileH = 120;
        const int gap = 16;
        const int gridW = tileW * 2 + gap;
        const int startX = area.getCentreX() - gridW / 2;
        int startY = area.getY() + 10;

        for (int i = 0; i < 4; ++i)
        {
            const int col = i % 2;
            const int row = i / 2;
            auto tileRect = juce::Rectangle<int>(
                startX + col * (tileW + gap),
                startY + row * (tileH + gap),
                tileW, tileH).toFloat();

            // Card background
            g.setColour(tiles[i].available
                ? juce::Colour::fromString("ff172131")
                : juce::Colour::fromString("ff0F1622"));
            g.fillRoundedRectangle(tileRect, 16.0f);

            // Border
            g.setColour(tiles[i].available
                ? tiles[i].accent.withAlpha(0.35f)
                : Nova::Colors::Border.withAlpha(0.4f));
            g.drawRoundedRectangle(tileRect.reduced(0.5f), 16.0f, 1.0f);

            // Icon
            auto iconArea = tileRect.reduced(14.0f).removeFromTop(40.0f).removeFromLeft(40.0f);
            g.setColour(tiles[i].accent.withAlpha(tiles[i].available ? 0.18f : 0.08f));
            g.fillRoundedRectangle(iconArea, 10.0f);
            g.setColour(tiles[i].accent.withAlpha(tiles[i].available ? 1.0f : 0.4f));
            g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
            g.drawText(tiles[i].icon, iconArea.toNearestInt(), juce::Justification::centred);

            // Name + subtitle
            auto textArea = tileRect.reduced(14.0f);
            textArea.removeFromTop(48.0f);
            g.setColour(tiles[i].available ? Nova::Colors::Text : Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
            g.drawText(tiles[i].name, textArea.removeFromTop(20.0f).toNearestInt(), juce::Justification::centredLeft);

            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawText(tiles[i].subtitle, textArea.removeFromTop(16.0f).toNearestInt(), juce::Justification::centredLeft);

            // "Coming Soon" badge for unavailable
            if (!tiles[i].available)
            {
                auto badge = tileRect.reduced(10.0f);
                g.setColour(Nova::Colors::TextDim.withAlpha(0.6f));
                g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
                g.drawText("COMING SOON", badge.toNearestInt(), juce::Justification::topRight);
            }
            else
            {
                auto badge = tileRect.reduced(10.0f);
                g.setColour(Nova::Colors::Success);
                g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
                g.drawText("ACTIVE", badge.toNearestInt(), juce::Justification::topRight);
            }
        }
    }

    void paintLinksStep(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        g.setColour(Nova::Colors::Text);
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawFittedText(
            "You're ready to go! Here are some optional next steps:",
            area.removeFromTop(24).reduced(10, 0), juce::Justification::centredLeft, 2);
    }

    // Callbacks
    std::function<void()> launchAudioSetup;
    std::function<void()> launchPresetFinder;

    // Step 1: Login
    juce::TextEditor loginEmailEditor;
    juce::TextEditor loginPasswordEditor;
    juce::TextButton btnLogin;
    juce::TextButton btnCreateAccount;
    juce::Label loginStatusLabel;

    // Step 2: Profile
    juce::TextEditor profileNameEditor;
    juce::ComboBox skillLevelBox;
    juce::ToggleButton genreToggles[8];

    // Step 4: Quick links
    juce::TextButton btnGoAudioSetup;
    juce::TextButton btnGoPresetFinder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StartWizard)
};
