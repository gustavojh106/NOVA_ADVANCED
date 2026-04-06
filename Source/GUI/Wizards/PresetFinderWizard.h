#pragma once
#include "WizardBase.h"
#include "../../Core/Constants.h"
#include "../../Core/PedalCatalog.h"

// =============================================================================
// Preset Finder / Creator wizard - guides the user through genre, guitar type,
// and tone preferences to suggest a signal chain or find matching presets.
// =============================================================================
class PresetFinderWizard final : public WizardBase
{
public:
    // Callbacks to interact with the main editor
    struct Callbacks
    {
        std::function<juce::File()>                    getPresetDirectory;
        std::function<void(const juce::File&)>         loadPresetFile;
        std::function<void(const juce::String&, Nova::ChainID, Nova::ZoneID)> addPedal;
    };

    PresetFinderWizard(Callbacks cbs, std::function<void()> onCloseFn)
        : WizardBase(buildSteps(), std::move(onCloseFn)),
          callbacks(std::move(cbs))
    {
        // --- Step 0: Welcome ------------------------------------------------
        addPage(0, new juce::Component());

        // --- Step 1: Genre / Style ------------------------------------------
        auto* genrePage = new juce::Component();
        addPage(1, genrePage);

        static constexpr const char* genreNames[] = {
            "Rock", "Metal", "Blues", "Jazz", "Pop/Indie",
            "Country", "Funk/R&B", "Ambient/Post-Rock"
        };
        for (int i = 0; i < 8; ++i)
        {
            genreToggles[i].setButtonText(genreNames[i]);
            genreToggles[i].setColour(juce::ToggleButton::textColourId, Nova::Colors::Text.withAlpha(0.88f));
            genreToggles[i].setColour(juce::ToggleButton::tickColourId, Nova::Colors::Accent);
            genrePage->addAndMakeVisible(genreToggles[i]);
        }

        // --- Step 2: Guitar / Pickup type -----------------------------------
        auto* guitarPage = new juce::Component();
        addPage(2, guitarPage);

        guitarTypeBox.addItem("Single Coil (Strat/Tele)", 1);
        guitarTypeBox.addItem("Humbucker (Les Paul/SG)", 2);
        guitarTypeBox.addItem("P90", 3);
        guitarTypeBox.addItem("Active Pickups (EMG/Fishman)", 4);
        guitarTypeBox.addItem("Acoustic with Pickup", 5);
        guitarTypeBox.addItem("Bass Guitar", 6);
        guitarTypeBox.addItem("7/8 String Extended Range", 7);
        guitarTypeBox.setSelectedId(1, juce::dontSendNotification);
        styleComboBox(guitarTypeBox);
        guitarPage->addAndMakeVisible(guitarTypeBox);

        gainPreferenceBox.addItem("Clean - sparkly, no breakup", 1);
        gainPreferenceBox.addItem("Crunch - edge of breakup, responsive", 2);
        gainPreferenceBox.addItem("Drive - classic overdrive, sustain", 3);
        gainPreferenceBox.addItem("High Gain - heavy saturation, compressed", 4);
        gainPreferenceBox.addItem("Fuzz - wooly, vintage, extreme", 5);
        gainPreferenceBox.setSelectedId(2, juce::dontSendNotification);
        styleComboBox(gainPreferenceBox);
        guitarPage->addAndMakeVisible(gainPreferenceBox);

        // --- Step 3: Tone Preferences (FX flavoring) ------------------------
        auto* tonePage = new juce::Component();
        addPage(3, tonePage);

        static constexpr const char* fxNames[] = {
            "Reverb", "Delay", "Chorus", "Phaser",
            "Tremolo", "Compressor", "EQ", "Wah"
        };
        for (int i = 0; i < 8; ++i)
        {
            fxToggles[i].setButtonText(fxNames[i]);
            fxToggles[i].setColour(juce::ToggleButton::textColourId, Nova::Colors::Text.withAlpha(0.88f));
            fxToggles[i].setColour(juce::ToggleButton::tickColourId, Nova::Colors::Accent);
            tonePage->addAndMakeVisible(fxToggles[i]);
        }

        reverbAmountBox.addItem("None", 1);
        reverbAmountBox.addItem("Subtle - room ambience", 2);
        reverbAmountBox.addItem("Medium - hall, present", 3);
        reverbAmountBox.addItem("Lush - large, washy", 4);
        reverbAmountBox.setSelectedId(2, juce::dontSendNotification);
        styleComboBox(reverbAmountBox);
        tonePage->addAndMakeVisible(reverbAmountBox);

        // --- Step 4: Results ------------------------------------------------
        auto* resultsPage = new juce::Component();
        addPage(4, resultsPage);

        resultsViewport.setScrollBarsShown(true, false);
        resultsViewport.setViewedComponent(&resultsContainer, false);
        resultsPage->addAndMakeVisible(resultsViewport);

        styleAccentButton(btnBuildChain, "BUILD THIS CHAIN");
        resultsPage->addAndMakeVisible(btnBuildChain);
        btnBuildChain.onClick = [this] { buildRecommendedChain(); };

        setStep(0);
    }

    void onStepActivated(int step) override
    {
        if (step == 4)
            generateResults();
    }

    void onFinished() override
    {
        // The user can click "BUILD THIS CHAIN" from the results,
        // or simply close. Finishing just closes.
    }

    void resized() override
    {
        WizardBase::resized();
        auto area = getContentArea().withTrimmedTop(60);

        // Step 1: Genre grid (4x2)
        layoutToggleGrid(genreToggles, 8, area);

        // Step 2: Guitar + Gain
        {
            auto guitarArea = area;
            const int fieldW = juce::jmin(420, guitarArea.getWidth());
            const int fieldH = 36;
            const int gap = 16;
            auto cx = guitarArea.getCentreX() - fieldW / 2;
            auto y = guitarArea.getY();

            guitarTypeBox.setBounds(cx, y, fieldW, fieldH);
            y += fieldH + gap;
            gainPreferenceBox.setBounds(cx, y, fieldW, fieldH);
        }

        // Step 3: FX toggles + reverb box
        {
            auto fxArea = area;
            layoutToggleGrid(fxToggles, 8, fxArea);

            const int fieldW = juce::jmin(420, fxArea.getWidth());
            const int fieldH = 36;
            auto cx = fxArea.getCentreX() - fieldW / 2;
            // Below the toggle grid
            reverbAmountBox.setBounds(cx, fxArea.getY() + 80, fieldW, fieldH);
        }

        // Step 4: Results
        {
            auto resArea = area;
            auto btnArea = resArea.removeFromBottom(50);
            btnBuildChain.setBounds(btnArea.withSizeKeepingCentre(240, 40));
            resultsViewport.setBounds(resArea);
            resultsContainer.setBounds(0, 0, resArea.getWidth() - 16,
                                       juce::jmax(resArea.getHeight(), resultsContentHeight));
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
        else if (getCurrentStep() == 1)
        {
            paintSectionLabel(g, area, "SELECT YOUR GENRES", "Pick one or more genres that describe your playing style.");
        }
        else if (getCurrentStep() == 2)
        {
            paintSectionLabel(g, area, "YOUR GUITAR", "Tell NOVA about your instrument and preferred gain level.");
            // Labels above combo boxes
            const int fieldW = juce::jmin(420, area.getWidth());
            auto cx = area.getCentreX() - fieldW / 2;
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
            g.drawText("GUITAR / PICKUP TYPE", cx, area.getY() - 14, fieldW, 12, juce::Justification::centredLeft);
            g.drawText("GAIN PREFERENCE", cx, area.getY() + 36 + 16 - 14, fieldW, 12, juce::Justification::centredLeft);
        }
        else if (getCurrentStep() == 3)
        {
            paintSectionLabel(g, area, "EFFECTS YOU LIKE", "Check effects you enjoy and set your reverb preference.");
            const int fieldW = juce::jmin(420, area.getWidth());
            auto cx = area.getCentreX() - fieldW / 2;
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
            g.drawText("REVERB AMOUNT", cx, area.getY() + 80 - 14, fieldW, 12, juce::Justification::centredLeft);
        }
        else if (getCurrentStep() == 4)
        {
            paintResultsOverlay(g, area);
        }
    }

private:
    static std::vector<StepDef> buildSteps()
    {
        return {
            { "Start",   "Find Your Tone",
              "NOVA will recommend presets and signal chains based on your preferences." },
            { "Genre",   "Genre & Style",
              "What kind of music do you play?" },
            { "Guitar",  "Instrument & Gain",
              "What guitar are you using and how much gain do you like?" },
            { "Effects", "Effects & Color",
              "What effects do you enjoy using?" },
            { "Results", "Your Recommendations",
              "Here's what NOVA suggests based on your preferences." }
        };
    }

    static void styleComboBox(juce::ComboBox& box)
    {
        box.setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromString("ff101722"));
        box.setColour(juce::ComboBox::textColourId, Nova::Colors::Text);
        box.setColour(juce::ComboBox::outlineColourId, Nova::Colors::Border.withAlpha(0.7f));
    }

    void layoutToggleGrid(juce::ToggleButton* toggles, int count, juce::Rectangle<int> area) const
    {
        const int cols = 4;
        const int fieldW = juce::jmin(420, area.getWidth());
        const int gap = 10;
        const int toggleW = (fieldW - gap * (cols - 1)) / cols;
        const int toggleH = 28;
        const int cx = area.getCentreX() - fieldW / 2;

        for (int i = 0; i < count; ++i)
        {
            const int col = i % cols;
            const int row = i / cols;
            toggles[i].setBounds(cx + col * (toggleW + gap),
                                 area.getY() + row * (toggleH + 6),
                                 toggleW, toggleH);
        }
    }

    void paintWelcomeStep(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        auto iconArea = area.removeFromTop(80).withSizeKeepingCentre(80, 80);
        g.setColour(Nova::Colors::Accent.withAlpha(0.12f));
        g.fillRoundedRectangle(iconArea.toFloat(), 20.0f);
        g.setColour(Nova::Colors::Accent);
        g.setFont(juce::Font(juce::FontOptions(32.0f, juce::Font::bold)));
        g.drawText(juce::CharPointer_UTF8("\xe2\x99\xab"), iconArea, juce::Justification::centred);

        area.removeFromTop(20);

        g.setColour(Nova::Colors::Text);
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawFittedText(
            "Answer a few quick questions about your playing style, guitar, and "
            "tone preferences. NOVA will suggest signal chains and help you find "
            "presets that match.\n\n"
            "If you already have presets saved, we'll also search those for matches.\n\n"
            "You can re-run this wizard anytime from Settings > Library.",
            area.reduced(30, 0), juce::Justification::centredTop, 8);
    }

    void paintSectionLabel(juce::Graphics& g, juce::Rectangle<int> area,
                           const juce::String& heading, const juce::String& sub) const
    {
        // Draw above the content area (using negative offset)
        auto labelArea = area.withHeight(0).translated(0, -30);
        g.setColour(Nova::Colors::TextDim);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(heading, labelArea.withHeight(14), juce::Justification::centredLeft);
        juce::ignoreUnused(sub);
    }

    void paintResultsOverlay(juce::Graphics& g, juce::Rectangle<int> area) const
    {
        if (matchingPresets.empty() && recommendedChain.empty())
        {
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(13.0f)));
            g.drawFittedText("Generating recommendations...",
                             area, juce::Justification::centred, 2);
        }
    }

    // =========================================================================
    // Recommendation Engine
    // =========================================================================
    void generateResults()
    {
        recommendedChain.clear();
        matchingPresets.clear();

        buildRecommendedChainList();
        searchExistingPresets();
        layoutResultsContent();
        resized();
        repaint();
    }

    void buildRecommendedChainList()
    {
        const int gainPref = gainPreferenceBox.getSelectedId(); // 1=Clean..5=Fuzz

        // Always start with a compressor for tighter playing
        recommendedChain.push_back({ "Compressor", "Tames dynamics for consistent level" });

        // Gate for high gain
        if (gainPref >= 3)
            recommendedChain.push_back({ "Noise Gate", "Silences hum between notes" });

        // EQ is almost always useful
        if (fxToggles[6].getToggleState()) // EQ toggle
            recommendedChain.push_back({ "EQ", "Shape your base tone" });

        // Wah before drive
        if (fxToggles[7].getToggleState())
            recommendedChain.push_back({ "Wah", "Classic vocal sweep" });

        // Drive section
        switch (gainPref)
        {
            case 1: // Clean - maybe a subtle boost
                recommendedChain.push_back({ "Boost", "Subtle sparkle and presence" });
                break;
            case 2: // Crunch
                recommendedChain.push_back({ "Overdrive", "Edge-of-breakup crunch" });
                break;
            case 3: // Drive
                recommendedChain.push_back({ "Overdrive", "Classic driven tone" });
                break;
            case 4: // High Gain
                recommendedChain.push_back({ "Distortion", "Heavy saturation" });
                break;
            case 5: // Fuzz
                recommendedChain.push_back({ "Fuzz", "Woolly vintage fuzz" });
                break;
            default: break;
        }

        // Amp selection based on genre + gain
        bool hasGenre[8] = {};
        for (int i = 0; i < 8; ++i)
            hasGenre[i] = genreToggles[i].getToggleState();

        juce::String ampType = "Classic Amp";
        juce::String ampReason = "Versatile British-style tone";

        if (hasGenre[1] || gainPref >= 4) // Metal or High Gain
        {
            ampType = "High Gain Amp";
            ampReason = "Modern high-gain voicing";
        }
        else if (hasGenre[3] || gainPref == 1) // Jazz or Clean
        {
            ampType = "Clean Amp";
            ampReason = "Pristine clean headroom";
        }
        else if (hasGenre[4]) // Pop/Indie
        {
            ampType = "Chime Amp";
            ampReason = "Jangly, chimey character";
        }
        else if (hasGenre[2]) // Blues
        {
            ampType = "Boutique Amp";
            ampReason = "Touch-responsive dynamics";
        }

        recommendedChain.push_back({ ampType, ampReason });

        // Cabinet
        if (hasGenre[1] || gainPref >= 4)
            recommendedChain.push_back({ "Modern 4x12", "Tight, focused low end" });
        else if (hasGenre[2] || hasGenre[3])
            recommendedChain.push_back({ "Vintage 2x12", "Warm, open character" });
        else
            recommendedChain.push_back({ "Cabinet", "Balanced 4x12 response" });

        // Modulation
        if (fxToggles[2].getToggleState()) // Chorus
            recommendedChain.push_back({ "Chorus", "Stereo width and motion" });
        if (fxToggles[3].getToggleState()) // Phaser
            recommendedChain.push_back({ "Phaser", "Swirling phase shift" });
        if (fxToggles[4].getToggleState()) // Tremolo
            recommendedChain.push_back({ "Tremolo", "Rhythmic amplitude pulse" });

        // Time-based
        if (fxToggles[1].getToggleState()) // Delay
            recommendedChain.push_back({ "Delay", "Spatial repeats and depth" });

        // Reverb
        const int reverbPref = reverbAmountBox.getSelectedId();
        if (reverbPref >= 2 || fxToggles[0].getToggleState())
        {
            juce::String reason;
            switch (reverbPref)
            {
                case 2: reason = "Subtle room ambience"; break;
                case 3: reason = "Present hall reverb"; break;
                case 4: reason = "Lush, expansive wash"; break;
                default: reason = "Natural space"; break;
            }
            recommendedChain.push_back({ "Reverb", reason });
        }
    }

    void searchExistingPresets()
    {
        if (!callbacks.getPresetDirectory)
            return;

        auto dir = callbacks.getPresetDirectory();
        if (!dir.isDirectory())
            return;

        juce::Array<juce::File> found;
        dir.findChildFiles(found, juce::File::findFiles, false, "*.nova-preset");

        // Simple keyword matching against preset names
        juce::StringArray keywords;
        for (int i = 0; i < 8; ++i)
        {
            if (genreToggles[i].getToggleState())
                keywords.add(genreToggles[i].getButtonText().toLowerCase());
        }

        // Add gain-related keywords
        switch (gainPreferenceBox.getSelectedId())
        {
            case 1: keywords.addArray({ "clean", "crystal", "pristine", "jazz" }); break;
            case 2: keywords.addArray({ "crunch", "edge", "blues", "classic" }); break;
            case 3: keywords.addArray({ "drive", "overdrive", "rock", "lead" }); break;
            case 4: keywords.addArray({ "heavy", "metal", "high gain", "distortion", "shred" }); break;
            case 5: keywords.addArray({ "fuzz", "doom", "stoner", "vintage" }); break;
            default: break;
        }

        for (auto& file : found)
        {
            const auto name = file.getFileNameWithoutExtension().toLowerCase();
            for (const auto& kw : keywords)
            {
                if (name.contains(kw))
                {
                    matchingPresets.push_back(file);
                    break;
                }
            }
        }
    }

    void layoutResultsContent()
    {
        // Clear old labels
        resultLabels.clear();
        presetButtons.clear();

        int y = 10;
        const int lineH = 28;
        const int sectionGap = 18;
        const int containerW = resultsViewport.getWidth() - 20;

        // Recommended chain section
        {
            auto lbl = std::make_unique<juce::Label>();
            lbl->setText("RECOMMENDED SIGNAL CHAIN", juce::dontSendNotification);
            lbl->setColour(juce::Label::textColourId, Nova::Colors::Accent);
            lbl->setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            lbl->setBounds(10, y, containerW, 20);
            resultsContainer.addAndMakeVisible(lbl.get());
            resultLabels.push_back(std::move(lbl));
            y += 24;

            for (int i = 0; i < (int)recommendedChain.size(); ++i)
            {
                const auto& item = recommendedChain[i];

                auto chainLbl = std::make_unique<juce::Label>();
                juce::String text = juce::String(i + 1) + ". " + item.typeID + " - " + item.reason;
                chainLbl->setText(text, juce::dontSendNotification);
                chainLbl->setColour(juce::Label::textColourId, Nova::Colors::Text.withAlpha(0.9f));
                chainLbl->setFont(juce::Font(juce::FontOptions(12.5f)));
                chainLbl->setBounds(20, y, containerW - 20, lineH);
                resultsContainer.addAndMakeVisible(chainLbl.get());
                resultLabels.push_back(std::move(chainLbl));
                y += lineH;
            }
        }

        y += sectionGap;

        // Matching presets section
        {
            auto lbl = std::make_unique<juce::Label>();
            juce::String heading = matchingPresets.empty()
                ? "NO MATCHING PRESETS FOUND"
                : "MATCHING PRESETS (" + juce::String((int)matchingPresets.size()) + ")";
            lbl->setText(heading, juce::dontSendNotification);
            lbl->setColour(juce::Label::textColourId, Nova::Colors::Accent);
            lbl->setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            lbl->setBounds(10, y, containerW, 20);
            resultsContainer.addAndMakeVisible(lbl.get());
            resultLabels.push_back(std::move(lbl));
            y += 24;

            if (matchingPresets.empty())
            {
                auto hint = std::make_unique<juce::Label>();
                hint->setText("Save more presets with descriptive names for better matching.",
                              juce::dontSendNotification);
                hint->setColour(juce::Label::textColourId, Nova::Colors::TextDim);
                hint->setFont(juce::Font(juce::FontOptions(11.5f)));
                hint->setBounds(20, y, containerW - 20, lineH);
                resultsContainer.addAndMakeVisible(hint.get());
                resultLabels.push_back(std::move(hint));
                y += lineH;
            }
            else
            {
                for (int i = 0; i < (int)matchingPresets.size() && i < 10; ++i)
                {
                    const auto& file = matchingPresets[i];
                    auto btn = std::make_unique<juce::TextButton>();
                    btn->setButtonText(file.getFileNameWithoutExtension());
                    btn->setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff182131"));
                    btn->setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.88f));
                    btn->setBounds(20, y, containerW - 20, lineH);
                    btn->onClick = [this, f = file]
                    {
                        if (callbacks.loadPresetFile)
                        {
                            callbacks.loadPresetFile(f);
                            closeWizard();
                        }
                    };
                    resultsContainer.addAndMakeVisible(btn.get());
                    presetButtons.push_back(std::move(btn));
                    y += lineH + 4;
                }
            }
        }

        resultsContentHeight = y + 20;
        resultsContainer.setBounds(0, 0, containerW, resultsContentHeight);
    }

    void buildRecommendedChain()
    {
        if (!callbacks.addPedal)
            return;

        for (const auto& item : recommendedChain)
        {
            // Determine zone from catalog
            Nova::ZoneID zone = Nova::ZoneID::Pre;
            for (const auto& entry : Nova::PedalCatalog::entries())
            {
                if (item.typeID == juce::String(entry.typeID))
                {
                    if (entry.kind == Nova::PedalCatalog::Kind::Amplifier)
                        zone = Nova::ZoneID::Amp;
                    else if (entry.kind == Nova::PedalCatalog::Kind::Cabinet)
                        zone = Nova::ZoneID::Cabinet;
                    else
                        zone = Nova::ZoneID::FX; // Most go to FX after amp
                    break;
                }
            }

            // Pre-drive pedals go to Pre zone
            static const juce::StringArray preZonePedals = {
                "Compressor", "Noise Gate", "EQ", "Boost", "Wah"
            };
            if (preZonePedals.contains(item.typeID))
                zone = Nova::ZoneID::Pre;

            callbacks.addPedal(item.typeID, Nova::ChainID::LineA, zone);
        }

        closeWizard();
    }

    // =========================================================================

    struct ChainItem
    {
        juce::String typeID;
        juce::String reason;
    };

    Callbacks callbacks;

    // Step 1: Genre
    juce::ToggleButton genreToggles[8];

    // Step 2: Guitar
    juce::ComboBox guitarTypeBox;
    juce::ComboBox gainPreferenceBox;

    // Step 3: Tone
    juce::ToggleButton fxToggles[8];
    juce::ComboBox reverbAmountBox;

    // Step 4: Results
    juce::Viewport resultsViewport;
    juce::Component resultsContainer;
    juce::TextButton btnBuildChain;
    int resultsContentHeight = 400;

    std::vector<ChainItem> recommendedChain;
    std::vector<juce::File> matchingPresets;
    std::vector<std::unique_ptr<juce::Label>> resultLabels;
    std::vector<std::unique_ptr<juce::TextButton>> presetButtons;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetFinderWizard)
};
