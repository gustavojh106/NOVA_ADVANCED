#include "PluginEditor.h"

#include "PedalCatalog.h"
#include "../GUI/Widgets/ChainLane.h"
#include "../GUI/Widgets/AssetBrowserOverlay.h"
#include "../Effects/Pedals/Base/PedalUIFactory.h"
#include "../Effects/Pedals/Overdrive/OverdriveThumbnail.h"
#include "../Effects/Pedals/Overdrive/OverdriveDashboard.h"
#include <algorithm>

namespace
{
    struct PedalUIRegistrar
    {
        PedalUIRegistrar()
        {
            Nova::OverdriveUI::registerOverdriveThumbnail();
            Nova::OverdriveUI::registerOverdriveDashboard();
        }
    };
    static PedalUIRegistrar sUIRegistrar;

    // ---- Multi-row adaptive layout for Pre / FX zones ----
    struct FlexLayoutResult
    {
        int rows = 1;
        int cols = 1;
        int cardW = 156;
        int cardH = 112;
        int gapH = 12;
        int gapV = 10;
        int maxCapacity = 1;
    };

    FlexLayoutResult calculateFlexLayout(int zoneW, int zoneH, int pedalCount)
    {
        struct TierDef { int cardW, cardH, gapH, gapV, padH, padV, minGapH; };

        static constexpr TierDef tiers[3] = {
            { 156, 112, 12, 10, 10, 8, 4 },   // Full
            { 130,  96, 10,  8,  8, 6, 3 },   // Compact
            { 110,  80,  8,  6,  6, 4, 2 },   // Mini
        };

        const int count = juce::jmax(1, juce::jmin(pedalCount, Nova::Config::MAX_PEDALS_PER_FLEX_ZONE));

        for (int t = 0; t < 3; ++t)
        {
            const auto& d = tiers[t];
            const int usableW = juce::jmax(1, zoneW - 2 * d.padH);
            const int usableH = juce::jmax(1, zoneH - 2 * d.padV);

            const int cols = juce::jmax(1, (usableW + d.gapH) / (d.cardW + d.gapH));
            const int maxRows = juce::jmax(1, (usableH + d.gapV) / (d.cardH + d.gapV));
            const int capacity = cols * maxRows;

            if (count <= capacity)
            {
                const int rows = (count + cols - 1) / cols;

                int actualGapH = d.gapH;
                if (cols > 1)
                {
                    actualGapH = (usableW - cols * d.cardW) / (cols - 1);
                    actualGapH = juce::jlimit(d.minGapH, d.gapH * 2, actualGapH);
                }

                int actualGapV = d.gapV;
                if (rows > 1)
                {
                    actualGapV = (usableH - rows * d.cardH) / (rows - 1);
                    actualGapV = juce::jlimit(2, d.gapV * 3, actualGapV);
                }

                return { rows, cols, d.cardW, d.cardH, actualGapH, actualGapV, capacity };
            }
        }

        // Fallback: tier 3 at max capacity
        const auto& d = tiers[2];
        const int usableW = juce::jmax(1, zoneW - 2 * d.padH);
        const int usableH = juce::jmax(1, zoneH - 2 * d.padV);
        const int cols = juce::jmax(1, (usableW + d.gapH) / (d.cardW + d.gapH));
        const int maxRows = juce::jmax(1, (usableH + d.gapV) / (d.cardH + d.gapV));
        return { maxRows, cols, d.cardW, d.cardH, d.gapH, d.gapV, cols * maxRows };
    }
}

class PedalEditorOverlay final : public juce::Component
{
public:
    PedalEditorOverlay(juce::String pedalName,
        juce::String pedalSubtitle,
        std::unique_ptr<juce::AudioProcessorEditor> editorToShow,
        std::function<void()> onCloseFn,
        juce::Colour accentCol = juce::Colour::fromString("ffA78BFA"))
        : title(std::move(pedalName)),
          subtitle(std::move(pedalSubtitle)),
          pedalEditor(std::move(editorToShow)),
          onClose(std::move(onCloseFn)),
          accent(accentCol)
    {
        setWantsKeyboardFocus(true);

        addAndMakeVisible(viewport);
        viewport.setScrollBarsShown(true, false);
        editorCanvas = std::make_unique<juce::Component>();
        viewport.setViewedComponent(editorCanvas.get(), false);

        if (pedalEditor != nullptr)
        {
            naturalEditorSize = { juce::jmax(1, pedalEditor->getWidth()), juce::jmax(1, pedalEditor->getHeight()) };
            editorCanvas->addAndMakeVisible(pedalEditor.get());
            pedalEditor->setInterceptsMouseClicks(true, true);
        }
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (onClose)
                onClose();
            return true;
        }

        return false;
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (!panelBounds.contains(e.getPosition()))
        {
            if (onClose)
                onClose();
        }
    }

    void paint(juce::Graphics& g) override
    {
        // Dark scrim
        g.fillAll(juce::Colours::black.withAlpha(0.88f));

        const auto pf = panelBounds.toFloat();

        // Panel fill
        juce::ColourGradient panelFill(juce::Colour::fromString("ff111827"),
            pf.getCentreX(), pf.getY(),
            juce::Colour::fromString("ff0B0E14"),
            pf.getCentreX(), pf.getBottom(), false);
        g.setGradientFill(panelFill);
        g.fillRoundedRectangle(pf, 16.0f);

        // Accent border
        g.setColour(accent.withAlpha(0.35f));
        g.drawRoundedRectangle(pf.reduced(0.5f), 16.0f, 1.5f);

        // Top glow strip
        auto topGlow = pf.reduced(20.0f, 0.0f).removeFromTop(2.5f);
        juce::ColourGradient glowGrad(juce::Colours::transparentBlack, topGlow.getX(), topGlow.getCentreY(),
            accent.withAlpha(0.55f), topGlow.getCentreX(), topGlow.getCentreY(), false);
        glowGrad.addColour(1.0, juce::Colours::transparentBlack);
        g.setGradientFill(glowGrad);
        g.fillRoundedRectangle(topGlow, 1.5f);

        // Floating close button circle
        const auto closeCentre = juce::Point<float>(pf.getRight() - 6.0f, pf.getY() - 6.0f);
        const float closeR = 16.0f;
        g.setColour(juce::Colour::fromString("ff1A2332"));
        g.fillEllipse(closeCentre.x - closeR, closeCentre.y - closeR, closeR * 2.0f, closeR * 2.0f);
        g.setColour(accent.withAlpha(0.50f));
        g.drawEllipse(closeCentre.x - closeR, closeCentre.y - closeR, closeR * 2.0f, closeR * 2.0f, 1.2f);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        g.drawText(juce::String::charToString(0xD7),
            juce::Rectangle<float>(closeCentre.x - closeR, closeCentre.y - closeR, closeR * 2.0f, closeR * 2.0f).toNearestInt(),
            juce::Justification::centred);

        closeBtnBounds = juce::Rectangle<float>(closeCentre.x - closeR - 4.0f, closeCentre.y - closeR - 4.0f,
            (closeR + 4.0f) * 2.0f, (closeR + 4.0f) * 2.0f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (closeBtnBounds.contains(e.getPosition().toFloat()))
        {
            if (onClose)
                onClose();
        }
    }

    void resized() override
    {
        const auto windowBounds = getLocalBounds();
        const int padding = 16;

        // Calculate panel size: wrap tightly around editor + minimal padding
        int panelW = naturalEditorSize.x + padding * 2;
        int panelH = naturalEditorSize.y + padding * 2;

        // Clamp to available window size with margin
        const int maxW = windowBounds.getWidth() - 40;
        const int maxH = windowBounds.getHeight() - 40;
        const bool needsViewport = (panelW > maxW || panelH > maxH);

        panelW = juce::jmin(panelW, maxW);
        panelH = juce::jmin(panelH, maxH);

        // Ensure minimum size
        panelW = juce::jmax(panelW, 280);
        panelH = juce::jmax(panelH, 200);

        panelBounds = juce::Rectangle<int>(
            (windowBounds.getWidth() - panelW) / 2,
            (windowBounds.getHeight() - panelH) / 2,
            panelW, panelH);

        auto content = panelBounds.reduced(padding);
        viewport.setBounds(content);

        if (pedalEditor != nullptr)
        {
            const int canvasW = juce::jmax(content.getWidth(), naturalEditorSize.x);
            const int canvasH = juce::jmax(content.getHeight(), naturalEditorSize.y);
            editorCanvas->setSize(canvasW, canvasH);
            pedalEditor->setBounds((canvasW - naturalEditorSize.x) / 2,
                (canvasH - naturalEditorSize.y) / 2,
                naturalEditorSize.x,
                naturalEditorSize.y);
        }

        viewport.setScrollBarsShown(needsViewport, false);
    }

private:
    juce::String title;
    juce::String subtitle;
    std::unique_ptr<juce::AudioProcessorEditor> pedalEditor;
    std::unique_ptr<juce::Component> editorCanvas;
    std::function<void()> onClose;
    juce::Point<int> naturalEditorSize { 1, 1 };
    juce::Colour accent;

    juce::Viewport viewport;
    juce::Rectangle<int> panelBounds;
    mutable juce::Rectangle<float> closeBtnBounds;
};

class PedalSlotComponent final : public juce::Component
{
public:
    PedalSlotComponent(NOVAAudioProcessor& ownerProcessor,
        Nova::ChainID ownerChain,
        std::function<void(Nova::ChainID, juce::String)> openEditorFn)
        : processor(ownerProcessor),
          chain(ownerChain),
          onOpenEditor(std::move(openEditorFn))
    {
        setRepaintsOnMouseActivity(true);
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);

        addAndMakeVisible(powerButton);
        powerButton.onClick = [this] { toggleBypass(); };
        powerButton.setTriggeredOnMouseDown(false);

        addAndMakeVisible(removeButton);
        removeButton.onClick = [this] { removePedal(); };
        removeButton.setTriggeredOnMouseDown(false);

        addAndMakeVisible(configButton);
        configButton.setButtonText("OPEN");
        configButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        configButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.8f));
        configButton.onClick = [this]
        {
            if (onOpenEditor == nullptr) return;
            const auto pedalID = getPedalID();
            if (pedalID.isNotEmpty())
                onOpenEditor(chain, pedalID);
        };
    }

    void mouseEnter(const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() < 5)
            return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        {
            const auto chainStr = (chain == Nova::ChainID::LineA) ? "LineA" : "LineB";
            container->startDragging("MOVE:" + juce::String(chainStr) + ":" + juce::String(getPedalIndex()), this);
        }
    }

    Nova::ChainID getChain() const { return chain; }
    juce::String getPedalID() const { return pedalState.getProperty(Nova::IDs::PEDAL_ID).toString(); }
    Nova::ZoneID getZone() const
    {
        return static_cast<Nova::ZoneID>(
            (int)pedalState.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
    }

    int getPedalIndex() const
    {
        auto parent = pedalState.getParent();
        return parent.isValid() ? parent.indexOf(pedalState) : -1;
    }

    void setPedalState(juce::ValueTree newState)
    {
        pedalState = newState;
        refreshVisualState();
        repaint();
    }

    int getPreferredWidth() const
    {
        switch (getZone())
        {
            case Nova::ZoneID::Amp:
            case Nova::ZoneID::Cabinet:  return 204;
            default:                     return 156;
        }
    }

    int getPreferredHeight() const
    {
        switch (getZone())
        {
            case Nova::ZoneID::Amp:
            case Nova::ZoneID::Cabinet:  return 132;
            default:                     return 112;
        }
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool enabled = isPedalEnabled();
        const auto accent = Nova::PedalCatalog::accentForType(getDisplayName());

        const float cornerR = getHeight() < 90 ? 8.0f : 12.0f;

        // ---- Accent-colored background ----
        const float sat = enabled ? 0.45f : 0.15f;
        const float briTop = enabled ? 0.18f : 0.08f;
        const float briBot = enabled ? 0.10f : 0.05f;
        const auto bgTop = accent.withSaturation(sat).withBrightness(briTop);
        const auto bgBot = accent.withSaturation(sat * 0.7f).withBrightness(briBot);

        juce::ColourGradient bg(bgTop, bounds.getCentreX(), bounds.getY(),
            bgBot, bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(bg);
        g.fillRoundedRectangle(bounds, cornerR);

        // ---- Border ----
        if (hovered && enabled)
        {
            g.setColour(accent.withAlpha(0.65f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), cornerR, 1.8f);
        }
        else
        {
            g.setColour(accent.withAlpha(enabled ? 0.25f : 0.08f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), cornerR, 1.0f);
        }

        // ---- Top accent glow line ----
        if (enabled)
        {
            auto glowBar = bounds.reduced(cornerR, 0.0f).removeFromTop(2.0f);
            g.setColour(accent.withAlpha(hovered ? 0.6f : 0.35f));
            g.fillRoundedRectangle(glowBar, 1.0f);
        }

        // ---- Pedal name (centred) ----
        const float nameFontSz = getHeight() < 90 ? 10.0f : (getHeight() < 108 ? 12.0f : 14.0f);
        auto nameArea = bounds.reduced(6.0f);
        nameArea.removeFromTop((float)(getHeight() < 90 ? 18 : 24));
        nameArea.removeFromBottom((float)(getHeight() < 90 ? 4 : 28));

        g.setColour(enabled ? juce::Colours::white.withAlpha(0.95f) : juce::Colours::white.withAlpha(0.4f));
        g.setFont(juce::Font(juce::FontOptions(nameFontSz, juce::Font::bold)));
        g.drawFittedText(getDisplayName(), nameArea.toNearestInt(), juce::Justification::centred, 2);

        // ---- Bypass darkening overlay ----
        if (!enabled)
        {
            g.setColour(juce::Colours::black.withAlpha(0.55f));
            g.fillRoundedRectangle(bounds, cornerR);

            // X icon
            const float midX = bounds.getCentreX();
            const float midY = bounds.getCentreY();
            const float r = getHeight() < 90 ? 4.0f : 6.0f;
            g.setColour(Nova::Colors::Error.withAlpha(0.5f));
            g.drawLine(midX - r, midY - r, midX + r, midY + r, 2.0f);
            g.drawLine(midX + r, midY - r, midX - r, midY + r, 2.0f);
        }
    }

    void resized() override
    {
        const bool mini = getHeight() < 90;
        const int pad   = mini ? 3 : 5;
        const int btnH  = mini ? 16 : 20;

        auto area = getLocalBounds().reduced(pad);

        // Top row: [power] ... [delete]
        auto topRow = area.removeFromTop(btnH);
        powerButton.setBounds(topRow.removeFromLeft(mini ? 28 : 36).reduced(1));
        removeButton.setBounds(topRow.removeFromRight(mini ? 18 : 22).reduced(1));

        // Bottom row: config button (hidden in mini)
        if (!mini)
        {
            auto botRow = area.removeFromBottom(btnH);
            const int cfgW = juce::jmin(60, botRow.getWidth() - 8);
            configButton.setBounds(botRow.withSizeKeepingCentre(cfgW, btnH));
            configButton.setVisible(true);
        }
        else
        {
            configButton.setVisible(false);
        }
    }

private:
    bool isPedalEnabled() const
    {
        return (bool)pedalState.getProperty(Nova::IDs::PEDAL_ENABLED, true);
    }

    juce::String getDisplayName() const
    {
        if (pedalState.isValid())
        {
            const auto type = pedalState.getProperty(Nova::IDs::PEDAL_TYPE).toString();
            if (type.isNotEmpty())
                return type;
        }
        return "Pedal";
    }

    void toggleBypass()
    {
        const int index = getPedalIndex();
        if (index >= 0)
            processor.requestBypassPedal(chain, index, isPedalEnabled());
    }

    void removePedal()
    {
        const int index = getPedalIndex();
        if (index >= 0)
            processor.requestRemovePedal(chain, index);
    }

    void refreshVisualState()
    {
        const bool enabled = isPedalEnabled();
        const auto accent = Nova::PedalCatalog::accentForType(getDisplayName());

        powerButton.setButtonText(enabled ? "ON" : "OFF");
        powerButton.setColour(juce::TextButton::buttonColourId,
            enabled ? accent.withSaturation(0.3f).withBrightness(0.2f)
                    : juce::Colour::fromString("ff3D1418"));
        powerButton.setColour(juce::TextButton::textColourOffId,
            enabled ? Nova::Colors::Success : Nova::Colors::Error.withAlpha(0.7f));

        removeButton.setButtonText("x");
        removeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        removeButton.setColour(juce::TextButton::textColourOffId, Nova::Colors::Error.withAlpha(0.65f));

        configButton.setColour(juce::TextButton::buttonColourId,
            accent.withSaturation(0.3f).withBrightness(0.15f));
        configButton.setColour(juce::TextButton::textColourOffId,
            juce::Colours::white.withAlpha(enabled ? 0.8f : 0.35f));
    }

    NOVAAudioProcessor& processor;
    Nova::ChainID chain;
    juce::ValueTree pedalState;
    std::function<void(Nova::ChainID, juce::String)> onOpenEditor;
    juce::TextButton powerButton;
    juce::TextButton removeButton;
    juce::TextButton configButton;
    bool hovered = false;
};

// ==============================================================================
// CONSTRUCTOR / INIT
// ==============================================================================

NOVAAudioProcessorEditor::NOVAAudioProcessorEditor(NOVAAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , audioProcessor(p)
{
    auto styleMetricLabel = [](juce::Label& label)
    {
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, Nova::Colors::Text.withAlpha(0.92f));
        label.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        label.setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
        label.setInterceptsMouseClicks(false, false);
    };

    auto styleRouteButton = [](juce::TextButton& button)
    {
        button.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff131A26"));
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromString("ff131A26"));
        button.setColour(juce::TextButton::textColourOffId, Nova::Colors::TextDim);
        button.setColour(juce::TextButton::textColourOnId, Nova::Colors::Text);
    };

    // -----------------------
    // HEADER
    // -----------------------
    addAndMakeVisible(btnStartStop);
    btnStartStop.setClickingTogglesState(true);
    btnStartStop.onClick = [this] { audioProcessor.toggleEngine(); };

    addAndMakeVisible(lblStats);
    lblStats.setJustificationType(juce::Justification::centred);
    lblStats.setColour(juce::Label::textColourId, Nova::Colors::TextDim);
    lblStats.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    lblStats.setText({}, juce::dontSendNotification);
    lblStats.setInterceptsMouseClicks(false, false);

    addAndMakeVisible(lblCpu);
    styleMetricLabel(lblCpu);
    lblCpu.setText("CPU USAGE\n--", juce::dontSendNotification);

    addAndMakeVisible(lblProc);
    styleMetricLabel(lblProc);
    lblProc.setText("PROCESS TIME\n--", juce::dontSendNotification);

    addAndMakeVisible(lblBuf);
    styleMetricLabel(lblBuf);
    lblBuf.setText("BUFFER TIME\n--", juce::dontSendNotification);

    addAndMakeVisible(btnTuner);

    // IMPORTANTE: esto estaba en resized() (redundante). Aquí queda 1 vez y ya.
    btnTuner.onClick = [this] { toggleTuner(); };
    btnTuner.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnTuner.setColour(juce::TextButton::textColourOffId, Nova::Colors::Success);
    btnTuner.setTooltip("Mute the output and open the tuner overlay");

    btnStartStop.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff2D1418"));
    btnStartStop.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.85f));

    // -----------------------
    // SIDEBAR DRAWERS
    // -----------------------
    leftDrawer.isLeftSide = true;
    leftDrawer.title = "QUICK ADD";
    addAndMakeVisible(leftDrawer);
    leftDrawer.setVisible(false);

    rightDrawer.isLeftSide = false;
    rightDrawer.title = "PRESETS";
    addAndMakeVisible(rightDrawer);
    rightDrawer.setVisible(false);

    addAndMakeVisible(btnToggleLeft);
    btnToggleLeft.setButtonText("+");
    btnToggleLeft.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff1A2332"));
    btnToggleLeft.setColour(juce::TextButton::textColourOffId, Nova::Colors::Accent);
    btnToggleLeft.setTooltip("Toggle effects browser");
    btnToggleLeft.onClick = [this] { toggleLeftPanel(); };

    addAndMakeVisible(btnToggleRight);
    btnToggleRight.setButtonText("PRESETS");
    btnToggleRight.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff1A2332"));
    btnToggleRight.setColour(juce::TextButton::textColourOffId, Nova::Colors::Accent);
    btnToggleRight.onClick = [this] { toggleRightPanel(); };

    // -----------------------
    // BROWSER (inside left drawer)
    // -----------------------
    leftDrawer.addAndMakeVisible(searchBarBrowser);
    searchBarBrowser.setTextToShowWhenEmpty("Filter quick-add modules...", juce::Colours::grey);
    searchBarBrowser.onTextChange = [this] { applyBrowserFilter(); };

    quickAddViewport.setScrollBarsShown(true, false);
    quickAddViewport.setScrollBarThickness(6);
    quickAddViewport.setViewedComponent(&quickAddContainer, false);
    leftDrawer.addAndMakeVisible(quickAddViewport);

    for (const auto& entry : Nova::PedalCatalog::entries())
    {
        auto btn = std::make_unique<DraggableButton>(entry.displayName);
        juce::String itemType;
        switch (entry.kind) {
            case Nova::PedalCatalog::Kind::Amplifier: itemType = "AMP"; break;
            case Nova::PedalCatalog::Kind::Cabinet:   itemType = "CAB"; break;
            default:                                   itemType = "PEDAL"; break;
        }
        setupQuickAddButton(*btn, entry.typeID, itemType);
        quickAddContainer.addAndMakeVisible(btn.get());
        quickAddButtons.push_back(std::move(btn));
    }
    // -----------------------
    // INPUT STRIP
    // -----------------------
    setupKnob(inputVolume, "GAIN", -60.0f, 24.0f, 0.0f);
    inputVolume.setTextValueSuffix(" dB");

    setupKnob(inputGate, "GATE", -100.0f, 0.0f, -100.0f);
    inputGate.setTextValueSuffix(" dB");

    setupKnob(inputTranspose, "TRANS", -12.0f, 12.0f, 0.0f);
    inputTranspose.setRange(-12.0, 12.0, 1.0);
    inputTranspose.setTextValueSuffix(" st");

    addAndMakeVisible(btnMonoStereo);
    btnMonoStereo.setButtonText("MONO");
    btnMonoStereo.setClickingTogglesState(true);
    btnMonoStereo.setColour(juce::ToggleButton::tickColourId, Nova::Colors::Accent);

    // -----------------------
    // MIXER & LANES
    // -----------------------
    laneA = std::make_unique<ChainLane>(p, Nova::ChainID::LineA);
    addAndMakeVisible(laneA.get());

    laneB = std::make_unique<ChainLane>(p, Nova::ChainID::LineB);
    addAndMakeVisible(laneB.get());

    addAndMakeVisible(btnSwitcher);
    btnSwitcher.setVisible(false);
    btnSwitcher.onClick = [this] { audioProcessor.cycleSwitcher(); };

    addAndMakeVisible(btnRouteA);
    btnRouteA.setButtonText("LINE A");
    styleRouteButton(btnRouteA);
    btnRouteA.onClick = [this] { audioProcessor.setSwitcherMode(Nova::SwitcherMode::LineA_Only); };

    addAndMakeVisible(btnRoutePar);
    btnRoutePar.setButtonText("PARALLEL");
    styleRouteButton(btnRoutePar);
    btnRoutePar.onClick = [this] { audioProcessor.setSwitcherMode(Nova::SwitcherMode::Dual_Parallel); };

    addAndMakeVisible(btnRouteB);
    btnRouteB.setButtonText("LINE B");
    styleRouteButton(btnRouteB);
    btnRouteB.onClick = [this] { audioProcessor.setSwitcherMode(Nova::SwitcherMode::LineB_Only); };

    // Line A
    setupKnob(volSliderA, "LEVEL", 0.0f, 2.0f, 1.0f);
    volSliderA.setTextValueSuffix("x");

    setupKnob(panSliderA, "PAN", -1.0f, 1.0f, 0.0f);
    panSliderA.setLookAndFeel(studioTrimLnf);

    setupKnob(widthSliderA, "WIDTH", 0.0f, 2.0f, 1.0f);
    widthSliderA.setTextValueSuffix("x");

    // Line B
    setupKnob(volSliderB, "LEVEL", 0.0f, 2.0f, 1.0f);
    volSliderB.setTextValueSuffix("x");

    setupKnob(panSliderB, "PAN", -1.0f, 1.0f, 0.0f);
    panSliderB.setLookAndFeel(studioTrimLnf);

    setupKnob(widthSliderB, "WIDTH", 0.0f, 2.0f, 1.0f);
    widthSliderB.setTextValueSuffix("x");

    // -----------------------
    // OUTPUT STRIP
    // -----------------------
    setupKnob(outputVolume, "MASTER", -60.0f, 12.0f, 0.0f);
    outputVolume.setTextValueSuffix(" dB");

    setupKnob(outputGain, "LIMIT", -20.0f, 0.0f, 0.0f);
    outputGain.setLookAndFeel(studioTrimLnf);
    outputGain.setTextValueSuffix(" dB");

    setupKnob(outputMix, "MIX", 0.0f, 100.0f, 100.0f);
    outputMix.setTextValueSuffix("%");

    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::INPUT_GAIN.toString()))
        inputVolumeAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, inputVolume);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::INPUT_GATE.toString()))
        inputGateAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, inputGate);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::INPUT_TRANS.toString()))
        inputTransposeAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, inputTranspose);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::FORCE_MONO.toString()))
        monoAttachment = std::make_unique<juce::ButtonParameterAttachment>(*param, btnMonoStereo);

    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_GAIN_A.toString()))
        volAAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, volSliderA);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_PAN_A.toString()))
        panAAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, panSliderA);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_WIDTH_A.toString()))
        widthAAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, widthSliderA);

    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_GAIN_B.toString()))
        volBAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, volSliderB);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_PAN_B.toString()))
        panBAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, panSliderB);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::MIXER_WIDTH_B.toString()))
        widthBAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, widthSliderB);

    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::OUTPUT_VOL.toString()))
        outputVolumeAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, outputVolume);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::OUTPUT_LIMITER.toString()))
        outputLimiterAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, outputGain);
    if (auto* param = audioProcessor.getGlobalParameter(Nova::IDs::OUTPUT_MIX.toString()))
        outputMixAttachment = std::make_unique<juce::SliderParameterAttachment>(*param, outputMix);

    // -----------------------
    // PRESETS (inside right drawer)
    // -----------------------
    rightDrawer.addAndMakeVisible(searchBarPresets);
    searchBarPresets.setTextToShowWhenEmpty("Filter...", juce::Colours::grey);
    searchBarPresets.onTextChange = [this] { refreshPresetList(); };

    rightDrawer.addAndMakeVisible(lblCurrentPreset);
    lblCurrentPreset.setJustificationType(juce::Justification::centred);
    lblCurrentPreset.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    lblCurrentPreset.setFont(juce::Font(juce::FontOptions(12.0f)));
    setCurrentPreset("No Preset");

    rightDrawer.addAndMakeVisible(presetSelector);
    presetSelector.setTextWhenNothingSelected("No Presets");
    presetSelector.onChange = [] {};

    rightDrawer.addAndMakeVisible(btnSave);
    btnSave.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnSave.onClick = [this] { saveSelectedOrPromptPreset(); };

    rightDrawer.addAndMakeVisible(btnLoad);
    btnLoad.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnLoad.onClick = [this] { loadSelectedPreset(); };

    rightDrawer.addAndMakeVisible(btnClear);
    btnClear.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff3D1418"));
    btnClear.onClick = [this] { clearPresetAndSession(); };

    addAndMakeVisible(audioProcessor.audioVisualizer);

    // -----------------------
    // INIT LOGIC
    // -----------------------
    audioProcessor.pluginState.addListener(this);

    setResizable(true, true);
    setSize(1920, 1080);

    // Valores iniciales
    syncControlsFromState();

    const auto startupPointer = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("NOVA")
        .getChildFile("startup-preset.txt");

    if (startupPointer.existsAsFile())
    {
        const auto presetPath = startupPointer.loadFileAsString().trim();
        const auto startupPreset = juce::File(presetPath);
        if (startupPreset.existsAsFile())
            setCurrentPreset(startupPreset.getFileNameWithoutExtension());
        else
            setCurrentPreset("No Preset");
    }
    else
    {
        setCurrentPreset("No Preset");
    }

    refreshPresetList();
    applyBrowserFilter();

    updateSwitcherState();
    updatePedalGui();

    statsTimer = std::make_unique<StatsTimer>(*this);
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
    audioProcessor.pluginState.removeListener(this);
    cancelPendingUpdate();
    activePedalEditors.clear();

    for (auto* knob : { &inputVolume, &inputGate, &inputTranspose,
        &volSliderA, &panSliderA, &widthSliderA,
        &volSliderB, &panSliderB, &widthSliderB,
        &outputVolume, &outputGain, &outputMix })
        knob->setLookAndFeel(nullptr);
}

// ==============================================================================
// WIDGET HELPERS
// ==============================================================================

void NOVAAudioProcessorEditor::setupKnob(juce::Slider& slider,
    const juce::String& name,
    float min,
    float max,
    float def)
{
    addAndMakeVisible(slider);
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 14);
    slider.setRotaryParameters(UI::KnobGeometry::knobStartAngleRadians(),
        UI::KnobGeometry::knobEndAngleRadians(),
        true);
    slider.setLookAndFeel(knobLnf);
    slider.setRange(min, max, 0.01);
    slider.setValue(def, juce::dontSendNotification);
    slider.setTooltip(name);
    slider.setName(name);
}

void NOVAAudioProcessorEditor::setupQuickAddButton(DraggableButton& button,
    const juce::String& typeID,
    const juce::String& itemType)
{
    button.setButtonText(typeID);
    button.setItemType(itemType);
    button.setAccentColour(Nova::PedalCatalog::accentForType(typeID));
    button.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    button.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.86f));
    button.setTooltip(Nova::PedalCatalog::subtitleForType(typeID));
}

void NOVAAudioProcessorEditor::applyBrowserFilter()
{
    const auto filter = searchBarBrowser.getText().trim();
    const auto& catalog = Nova::PedalCatalog::entries();

    for (size_t i = 0; i < quickAddButtons.size() && i < catalog.size(); ++i)
    {
        const bool matches = Nova::PedalCatalog::matchesFilter(catalog[i], filter);
        quickAddButtons[i]->setVisible(matches);
        quickAddButtons[i]->setEnabled(matches);
    }

    layoutQuickAddButtons();
    quickAddViewport.repaint();
}

void NOVAAudioProcessorEditor::layoutQuickAddButtons()
{
    constexpr int buttonH = 46;
    constexpr int buttonGap = 10;
    const int contentW = quickAddViewport.getWidth() - quickAddViewport.getScrollBarThickness() - 4;
    const int buttonW = juce::jmax(80, contentW - 12);

    int y = 6;
    for (auto& btn : quickAddButtons)
    {
        if (!btn->isVisible())
            continue;

        btn->setBounds(6, y, buttonW, buttonH);
        y += buttonH + buttonGap;
    }

    quickAddContainer.setSize(quickAddViewport.getWidth(), juce::jmax(y, quickAddViewport.getHeight()));
}

juce::Rectangle<int> NOVAAudioProcessorEditor::getInputStripBounds() const
{
    return { 0, Nova::Config::HEADER_HEIGHT, 80,
        juce::jmax(0, getHeight() - Nova::Config::HEADER_HEIGHT - Nova::Config::FOOTER_HEIGHT) };
}

juce::Rectangle<int> NOVAAudioProcessorEditor::getOutputStripBounds() const
{
    return { juce::jmax(0, getWidth() - 80), Nova::Config::HEADER_HEIGHT, 80,
        juce::jmax(0, getHeight() - Nova::Config::HEADER_HEIGHT - Nova::Config::FOOTER_HEIGHT) };
}

void NOVAAudioProcessorEditor::updateMeterState()
{
    auto advanceMeter = [](float rawPeak, float& displayed, float& hold)
    {
        const float clampedPeak = juce::jlimit(0.0f, 1.2f, rawPeak);

        if (clampedPeak > displayed)
            displayed += (clampedPeak - displayed) * 0.55f;
        else
            displayed = clampedPeak + (displayed - clampedPeak) * 0.82f;

        displayed = juce::jlimit(0.0f, 1.2f, displayed);
        hold = juce::jmax(clampedPeak, juce::jmax(0.0f, hold - 0.018f));
    };

    advanceMeter(audioProcessor.getInputPeak(), inputMeterDisplay, inputMeterHold);
    advanceMeter(audioProcessor.getOutputPeak(), outputMeterDisplay, outputMeterHold);

    const auto inputStrip = getInputStripBounds();
    if (!inputStrip.isEmpty())
        repaint(inputStrip);

    const auto outputStrip = getOutputStripBounds();
    if (!outputStrip.isEmpty())
        repaint(outputStrip);
}

juce::File NOVAAudioProcessorEditor::getPresetDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("NOVA")
        .getChildFile("Presets");

    if (!dir.exists())
        dir.createDirectory();

    return dir;
}

juce::File NOVAAudioProcessorEditor::getPresetFileForName(const juce::String& presetName) const
{
    auto safe = presetName.trim();
    safe = safe.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_");

    if (safe.isEmpty())
        safe = "Preset";

    return getPresetDirectory().getChildFile(safe + ".nova-preset");
}

void NOVAAudioProcessorEditor::setCurrentPreset(const juce::String& presetName)
{
    currentPresetName = presetName.trim().isEmpty() ? "No Preset" : presetName.trim();
    const auto label = (currentPresetName == "No Preset")
        ? juce::String("No Preset")
        : juce::String("Preset: ") + currentPresetName;
    lblCurrentPreset.setText(label, juce::dontSendNotification);
}

void NOVAAudioProcessorEditor::refreshPresetList()
{
    juce::Array<juce::File> found;
    getPresetDirectory().findChildFiles(found, juce::File::TypesOfFileToFind::findFiles, false, "*.nova-preset");

    std::vector<juce::File> sorted;
    sorted.reserve((size_t)found.size());
    for (auto& f : found)
        sorted.push_back(f);

    std::sort(sorted.begin(), sorted.end(),
        [](const juce::File& a, const juce::File& b)
        {
            return a.getFileNameWithoutExtension().compareNatural(b.getFileNameWithoutExtension()) < 0;
        });

    const auto filter = searchBarPresets.getText().trim();

    presetFiles.clear();
    for (const auto& f : sorted)
    {
        const auto name = f.getFileNameWithoutExtension();
        if (filter.isNotEmpty() && !name.containsIgnoreCase(filter))
            continue;

        presetFiles.push_back(f);
    }

    presetSelector.clear(juce::dontSendNotification);

    int selectedId = 0;
    for (size_t i = 0; i < presetFiles.size(); ++i)
    {
        const auto name = presetFiles[i].getFileNameWithoutExtension();
        presetSelector.addItem(name, (int)i + 1);

        if (name == currentPresetName)
            selectedId = (int)i + 1;
    }

    if (selectedId > 0)
        presetSelector.setSelectedId(selectedId, juce::dontSendNotification);
    else
        presetSelector.setSelectedId(0, juce::dontSendNotification);

    presetSelector.setTextWhenNothingSelected(presetFiles.empty() ? "No Presets" : "Select Preset");
}

void NOVAAudioProcessorEditor::syncControlsFromState()
{
}

void NOVAAudioProcessorEditor::savePresetWithName(const juce::String& presetName)
{
    const auto target = getPresetFileForName(presetName);
    if (!audioProcessor.savePresetToFile(target))
        return;

    setCurrentPreset(target.getFileNameWithoutExtension());
    refreshPresetList();
}

void NOVAAudioProcessorEditor::saveSelectedOrPromptPreset()
{
    juce::String suggestedName;
    const int idx = presetSelector.getSelectedId() - 1;

    if (juce::isPositiveAndBelow(idx, (int)presetFiles.size()))
        suggestedName = presetFiles[(size_t)idx].getFileNameWithoutExtension();
    else if (currentPresetName != "No Preset")
        suggestedName = currentPresetName;

    auto* alert = new juce::AlertWindow("Save Preset", "Preset name", juce::AlertWindow::NoIcon);
    alert->addTextEditor("name", suggestedName, "Name");
    alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<NOVAAudioProcessorEditor> safeThis(this);
    alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safeThis, alert](int result)
        {
            if (safeThis == nullptr || result != 1)
                return;

            const auto name = alert->getTextEditorContents("name").trim();
            if (name.isEmpty())
                return;

            safeThis->savePresetWithName(name);
        }), true);
}

void NOVAAudioProcessorEditor::loadSelectedPreset()
{
    const int idx = presetSelector.getSelectedId() - 1;
    if (!juce::isPositiveAndBelow(idx, (int)presetFiles.size()))
        return;

    const auto& target = presetFiles[(size_t)idx];
    if (!audioProcessor.loadPresetFromFile(target))
        return;

    setCurrentPreset(target.getFileNameWithoutExtension());
    syncControlsFromState();
    refreshPresetList();
    updateSwitcherState();
    updatePedalGui();
    repaint();
}

void NOVAAudioProcessorEditor::clearPresetAndSession()
{
    audioProcessor.clearSessionAndForgetStartupPreset();

    setCurrentPreset("No Preset");
    presetSelector.setSelectedId(0, juce::dontSendNotification);
    syncControlsFromState();
    refreshPresetList();
    updateSwitcherState();
    updatePedalGui();
    repaint();
}


// ==============================================================================
// PAINT
// ==============================================================================

void NOVAAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(Nova::Colors::Background);
    auto area = getLocalBounds();
    const bool on = audioProcessor.isEngineOn();
    const int mode = (int)audioProcessor.getSwitcherMode();

    // Header
    auto headerRect = area.removeFromTop(80);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(headerRect);

    g.setColour(Nova::Colors::Border);
    g.drawHorizontalLine(headerRect.getBottom(), 0, (float)getWidth());

    const auto fullHeader = headerRect;

    auto drawRouteButtonGlow = [&](const juce::TextButton& button, bool active, juce::Colour colour)
    {
        if (!on || !active || button.getBounds().isEmpty())
            return;

        const auto buttonBounds = button.getBounds().toFloat();
        g.setColour(colour.withAlpha(0.12f));
        g.fillRoundedRectangle(buttonBounds.expanded(12.0f, 8.0f), 12.0f);
        g.setColour(colour.withAlpha(0.18f));
        g.fillRoundedRectangle(buttonBounds.expanded(7.0f, 4.0f), 10.0f);
        g.setColour(colour.withAlpha(0.42f));
        g.drawRoundedRectangle(buttonBounds.expanded(3.0f, 2.0f), 8.0f, 1.2f);
    };

    auto drawDashboardGlow = [&](juce::Rectangle<int> bounds, juce::Colour colour, float fillAlpha)
    {
        if (!on || bounds.isEmpty())
            return;

        const auto glowBounds = bounds.toFloat();
        g.setColour(colour.withAlpha(fillAlpha * 0.35f));
        g.fillRoundedRectangle(glowBounds.expanded(20.0f, 14.0f), 26.0f);
        g.setColour(colour.withAlpha(fillAlpha * 0.60f));
        g.fillRoundedRectangle(glowBounds.expanded(10.0f, 7.0f), 22.0f);
        g.setColour(colour.withAlpha(fillAlpha));
        g.fillRoundedRectangle(glowBounds.reduced(2.0f), 18.0f);
        g.setColour(colour.withAlpha(fillAlpha * 1.35f));
        g.drawRoundedRectangle(glowBounds.reduced(1.0f), 18.0f, 1.4f);
    };

    drawRouteButtonGlow(btnRouteA, mode == (int)Nova::SwitcherMode::LineA_Only, Nova::Colors::CableOnA);
    drawRouteButtonGlow(btnRoutePar, mode == (int)Nova::SwitcherMode::Dual_Parallel, Nova::Colors::Accent);
    drawRouteButtonGlow(btnRouteB, mode == (int)Nova::SwitcherMode::LineB_Only, Nova::Colors::CableOnB);

    g.setColour(Nova::Colors::Accent);
    g.setFont(juce::Font(juce::FontOptions(30.0f, juce::Font::bold)));
    auto logoArea = fullHeader.withTrimmedLeft(64).removeFromLeft(180);
    g.drawText("NOVA", logoArea, juce::Justification::centredLeft);
    g.setColour(Nova::Colors::TextDim);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    auto taglineArea = fullHeader.withTrimmedLeft(190).removeFromLeft(220);
    g.drawText("Guitar Rig Designer", taglineArea, juce::Justification::centredLeft);

    // Footer
    auto footerRect = area.removeFromBottom(100);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(footerRect);
    g.setColour(Nova::Colors::Border);
    g.drawHorizontalLine(footerRect.getY(), 0, (float)getWidth());

    // Input strip (left edge)
    auto left2 = area.removeFromLeft(80);
    drawChannelStrip(g, left2, "INPUT");

    // Output strip (right edge)
    auto right1 = area.removeFromRight(80);
    drawChannelStrip(g, right1, "OUTPUT");

    if (mode == (int)Nova::SwitcherMode::LineA_Only)
    {
        if (laneA && laneA->isVisible())
            drawDashboardGlow(laneA->getBounds(), Nova::Colors::CableOnA, 0.085f);
    }
    else if (mode == (int)Nova::SwitcherMode::LineB_Only)
    {
        if (laneB && laneB->isVisible())
            drawDashboardGlow(laneB->getBounds(), Nova::Colors::CableOnB, 0.085f);
    }
    else
    {
        if (laneA && laneA->isVisible())
            drawDashboardGlow(laneA->getBounds(), Nova::Colors::CableOnA, 0.055f);
        if (laneB && laneB->isVisible())
            drawDashboardGlow(laneB->getBounds(), Nova::Colors::CableOnB, 0.055f);
        if (laneA && laneB && laneA->isVisible() && laneB->isVisible())
            drawDashboardGlow(laneA->getBounds().getUnion(laneB->getBounds()).expanded(4, 4),
                Nova::Colors::Accent, 0.035f);
    }

    auto waveBounds = audioProcessor.audioVisualizer.getBounds().toFloat();
    if (!waveBounds.isEmpty())
    {
        g.setColour(juce::Colour::fromString("ff0F1520"));
        g.fillRoundedRectangle(waveBounds.expanded(6.0f, 4.0f), 10.0f);
        g.setColour(Nova::Colors::Border.withAlpha(0.85f));
        g.drawRoundedRectangle(waveBounds.expanded(6.0f, 4.0f), 10.0f, 1.0f);
    }

    auto metricsBounds = lblCpu.getBounds()
        .getUnion(lblProc.getBounds())
        .getUnion(lblBuf.getBounds());

    if (!lblStats.getText().isEmpty())
        metricsBounds = metricsBounds.getUnion(lblStats.getBounds());

    auto metricsBoundsF = metricsBounds.expanded(12, 8).toFloat();

    if (!metricsBoundsF.isEmpty())
    {
        juce::ColourGradient metricsFill(juce::Colour::fromString("ff141C28"),
            metricsBoundsF.getCentreX(), metricsBoundsF.getY(),
            juce::Colour::fromString("ff101620"),
            metricsBoundsF.getCentreX(), metricsBoundsF.getBottom(), false);
        g.setGradientFill(metricsFill);
        g.fillRoundedRectangle(metricsBoundsF, 12.0f);
        g.setColour(Nova::Colors::Border.withAlpha(0.8f));
        g.drawRoundedRectangle(metricsBoundsF, 12.0f, 1.0f);

        const auto cpuBounds = lblCpu.getBounds();
        const auto procBounds = lblProc.getBounds();
        const auto bufBounds = lblBuf.getBounds();
        const int sepTop = juce::jmin(cpuBounds.getY(), juce::jmin(procBounds.getY(), bufBounds.getY())) + 4;
        const int sepBottom = juce::jmax(cpuBounds.getBottom(), juce::jmax(procBounds.getBottom(), bufBounds.getBottom())) - 4;

        g.setColour(Nova::Colors::Border.withAlpha(0.5f));
        g.drawVerticalLine(cpuBounds.getRight() + 4, (float)sepTop, (float)sepBottom);
        g.drawVerticalLine(procBounds.getRight() + 4, (float)sepTop, (float)sepBottom);

        if (!lblStats.getText().isEmpty())
        {
            g.setColour(Nova::Colors::Error.withAlpha(0.14f));
            g.fillRoundedRectangle(lblStats.getBounds().toFloat().expanded(4.0f, 2.0f), 6.0f);
        }
    }

    // Mixer area
    const bool dualModePaint = (mode == (int)Nova::SwitcherMode::Dual_Parallel);
    const int paintMixerH = dualModePaint ? 0 : 170;

    auto center = area;
    auto mixerArea = center.removeFromBottom(paintMixerH);

    const bool aActive = on && (mode != (int)Nova::SwitcherMode::LineB_Only);
    const bool bActive = on && (mode != (int)Nova::SwitcherMode::LineA_Only);
    const bool dualMode = dualModePaint;

    if (paintMixerH > 0)
    {
        g.setColour(Nova::Colors::MixerPanel);
        g.drawRoundedRectangle(mixerArea.toFloat().reduced(10), 5.0f, 1.0f);
    }

    // Knob name labels — drawn above each visible knob using its actual bounds
    auto drawKnobLabel = [&](const juce::String& text, const juce::Slider& knob)
    {
        if (!knob.isVisible()) return;
        const auto b = knob.getBounds();
        g.drawText(text, b.getX(), b.getY() - 14, b.getWidth(), 14, juce::Justification::centred);
    };

    g.setColour(Nova::Colors::Text);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));

    if (dualMode)
    {
        // No bottom mixer bar in parallel mode.
    }
    else
    {
        const juce::String lineLabel = (mode == (int)Nova::SwitcherMode::LineA_Only) ? "LINE A" : "LINE B";
        const auto labelColour = (mode == (int)Nova::SwitcherMode::LineA_Only) ? Nova::Colors::CableOnA : Nova::Colors::CableOnB;
        g.setColour(on ? labelColour : Nova::Colors::TextDim);
        g.drawText(lineLabel, mixerArea.getX(), mixerArea.getY() + 10, mixerArea.getWidth(), 20, juce::Justification::centred);
    }

    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.setColour(Nova::Colors::TextDim);

    if (!dualMode)
    {
        drawKnobLabel("LEVEL", volSliderA);
        drawKnobLabel("PAN", panSliderA);
        drawKnobLabel("WIDTH", widthSliderA);
        drawKnobLabel("LEVEL", volSliderB);
        drawKnobLabel("PAN", panSliderB);
        drawKnobLabel("WIDTH", widthSliderB);
    }

    // --- Routing circuit diagram (only in dual mode) ---
    if (dualMode && paintMixerH > 0)
    {
        const int leftGroupRight  = widthSliderA.getRight();
        const int rightGroupLeft  = volSliderB.getX();

        auto circuitZone = juce::Rectangle<int>(leftGroupRight + 8,
            mixerArea.getY() + 12,
            juce::jmax(40, rightGroupLeft - leftGroupRight - 16),
            mixerArea.getHeight() - 24);

        g.setColour(juce::Colour::fromString("ff0B0E14"));
        g.fillRoundedRectangle(circuitZone.toFloat(), 10.0f);
        g.setColour(Nova::Colors::Border);
        g.drawRoundedRectangle(circuitZone.toFloat(), 10.0f, 1.0f);

        const float xC = circuitZone.toFloat().getCentreX();
        const float cTop = circuitZone.toFloat().getY() + 16.0f;
        const float cBot = circuitZone.toFloat().getBottom() - 16.0f;
        const float cLeft  = circuitZone.toFloat().getX() + 12.0f;
        const float cRight = circuitZone.toFloat().getRight() - 12.0f;
        const float switchY = circuitZone.toFloat().getCentreY();

        const float lineAY = cTop + 10.0f;
        const float lineBY = cBot - 10.0f;

        auto drawSignalLineOffset = [&](float y, float tapOffsetX, bool active, juce::Colour activeColour)
            {
                const juce::Colour offColour = Nova::Colors::CableOff;
                const float tapX = xC + tapOffsetX;

                if (active)
                {
                    g.setColour(activeColour.withAlpha(0.2f));
                    g.drawLine(cLeft, y, cRight, y, 8.0f);
                    g.setColour(activeColour);
                    g.drawLine(cLeft, y, cRight, y, 2.4f);
                }
                else
                {
                    g.setColour(offColour);
                    g.drawLine(cLeft, y, cRight, y, 2.4f);
                }

                if (active)
                {
                    g.setColour(activeColour.withAlpha(0.2f));
                    g.drawLine(tapX, y, tapX, switchY, 8.0f);
                    g.setColour(activeColour);
                    g.drawLine(tapX, y, tapX, switchY, 2.4f);
                }
                else
                {
                    g.setColour(offColour);
                    g.drawLine(tapX, y, tapX, switchY, 2.0f);
                }

                g.setColour(active ? activeColour : offColour);
                g.fillEllipse(cLeft - 3.5f, y - 3.5f, 7.0f, 7.0f);
                g.fillEllipse(cRight - 3.5f, y - 3.5f, 7.0f, 7.0f);
                g.fillEllipse(tapX - 3.0f, y - 3.0f, 6.0f, 6.0f);
            };

        drawSignalLineOffset(lineAY, -12.0f, aActive, Nova::Colors::CableOnA);
        drawSignalLineOffset(lineBY,  12.0f, bActive, Nova::Colors::CableOnB);

        g.setColour(juce::Colour::fromString("ff1A2332"));
        g.fillEllipse(xC - 14.0f, switchY - 14.0f, 28.0f, 28.0f);
        g.setColour(Nova::Colors::Border);
        g.drawEllipse(xC - 14.0f, switchY - 14.0f, 28.0f, 28.0f, 1.2f);

        if (aActive && bActive)
            g.setColour(Nova::Colors::Accent);
        else if (aActive)
            g.setColour(Nova::Colors::CableOnA);
        else if (bActive)
            g.setColour(Nova::Colors::CableOnB);
        else
            g.setColour(Nova::Colors::CableOff);

        g.fillEllipse(xC - 6.0f, switchY - 6.0f, 12.0f, 12.0f);

        g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        g.setColour(aActive ? Nova::Colors::CableOnA : Nova::Colors::TextDim);
        g.drawText("A", (int)(cLeft - 2.0f), (int)(lineAY - 18.0f), 14, 12, juce::Justification::centred);
        g.setColour(bActive ? Nova::Colors::CableOnB : Nova::Colors::TextDim);
        g.drawText("B", (int)(cLeft - 2.0f), (int)(lineBY + 6.0f), 14, 12, juce::Justification::centred);
    }

    if (!routeInfoIconBounds.isEmpty())
    {
        g.setColour(routeInfoHovered ? juce::Colours::white.withAlpha(0.92f) : Nova::Colors::TextDim.withAlpha(0.8f));
        g.drawEllipse(routeInfoIconBounds, 1.0f);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText("i", routeInfoIconBounds.toNearestInt(), juce::Justification::centred);

        if (routeInfoHovered)
        {
            const int popupW = 260;
            const int popupH = 56;
            const int popupX = juce::jmin(getWidth() - popupW - 16, (int)routeInfoIconBounds.getRight() + 10);
            const int popupY = juce::jmax(fullHeader.getBottom() + 6, (int)routeInfoIconBounds.getCentreY() - popupH / 2);
            auto popup = juce::Rectangle<float>((float)popupX, (float)popupY, (float)popupW, (float)popupH);

            g.setColour(juce::Colours::black.withAlpha(0.25f));
            g.fillRoundedRectangle(popup.translated(0.0f, 2.0f), 10.0f);
            g.setColour(juce::Colour::fromString("ff141C28"));
            g.fillRoundedRectangle(popup, 10.0f);
            g.setColour(Nova::Colors::Border.withAlpha(0.9f));
            g.drawRoundedRectangle(popup, 10.0f, 1.0f);
            g.setColour(Nova::Colors::Text.withAlpha(0.94f));
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawFittedText(routeInfoText, popup.reduced(10.0f, 8.0f).toNearestInt(), juce::Justification::centredLeft, 3);
        }
    }
}

void NOVAAudioProcessorEditor::drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    const auto originalArea = area;
    g.setColour(Nova::Colors::Background);
    g.fillRect(area);

    g.setColour(Nova::Colors::Border);
    g.drawVerticalLine(area.getRight(), (float)area.getY(), (float)area.getBottom());
    g.drawVerticalLine(area.getX(), (float)area.getY(), (float)area.getBottom());

    g.setColour(Nova::Colors::Text);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    g.drawText(title, area.removeFromTop(32), juce::Justification::centred);

    // Thin vertical meter at right edge
    const bool isInput = (title == "INPUT");
    const float displayPeak = isInput ? inputMeterDisplay : outputMeterDisplay;
    const float holdPeak = isInput ? inputMeterHold : outputMeterHold;
    const float peakDb = juce::Decibels::gainToDecibels(juce::jmax(displayPeak, 0.000001f), -60.0f);
    const float holdDb = juce::Decibels::gainToDecibels(juce::jmax(holdPeak, 0.000001f), -60.0f);
    const float meterNorm = juce::jlimit(0.0f, 1.0f, (peakDb + 60.0f) / 60.0f);
    const float holdNorm = juce::jlimit(0.0f, 1.0f, (holdDb + 60.0f) / 60.0f);

    const int controlsBottom = isInput
        ? juce::jmax(inputVolume.getBottom(),
            juce::jmax(inputGate.getBottom(), juce::jmax(inputTranspose.getBottom(), btnMonoStereo.getBottom())))
        : juce::jmax(outputVolume.getBottom(), juce::jmax(outputGain.getBottom(), outputMix.getBottom()));

    const int meterTop = juce::jmax(originalArea.getY() + 42, controlsBottom + 14);
    const int meterBottom = juce::jmin(originalArea.getBottom() - 18, meterTop + 150);
    const int meterHeight = juce::jmax(0, meterBottom - meterTop);

    if (meterHeight < 24)
        return;

    auto meterArea = juce::Rectangle<float>((float)(originalArea.getCentreX() - 6),
        (float)meterTop,
        12.0f,
        (float)meterHeight);

    g.setColour(juce::Colour::fromString("ff0A1018"));
    g.fillRoundedRectangle(meterArea, 5.0f);
    g.setColour(Nova::Colors::Border.withAlpha(0.45f));
    g.drawRoundedRectangle(meterArea, 5.0f, 0.8f);

    const float redZoneTop = meterArea.getY() + meterArea.getHeight() * 0.12f;
    g.setColour(Nova::Colors::Error.withAlpha(0.10f));
    g.fillRoundedRectangle(juce::Rectangle<float>(meterArea.getX(), redZoneTop, meterArea.getWidth(), meterArea.getBottom() - redZoneTop), 5.0f);

    if (meterNorm > 0.0f)
    {
        auto fill = meterArea.withY(meterArea.getBottom() - meterArea.getHeight() * meterNorm).reduced(0.5f, 0.5f);
        fill.setHeight(juce::jmax(3.0f, meterArea.getHeight() * meterNorm - 1.0f));

        juce::ColourGradient meterGrad(Nova::Colors::Success, fill.getCentreX(), fill.getBottom(),
            juce::Colour::fromString("ffFBBF24"), fill.getCentreX(), fill.getCentreY(), false);
        meterGrad.addColour(0.35, juce::Colour::fromString("ffF59E0B"));
        meterGrad.addColour(0.0, displayPeak > 0.97f ? Nova::Colors::Error : juce::Colour::fromString("ffF97316"));
        g.setGradientFill(meterGrad);
        g.fillRoundedRectangle(fill, 3.0f);
    }

    if (holdNorm > 0.0f)
    {
        const float holdY = meterArea.getBottom() - meterArea.getHeight() * holdNorm;
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawLine(meterArea.getX(), holdY, meterArea.getRight(), holdY, 1.2f);
    }

    // Knob labels
    auto drawLabel = [&](const juce::String& txt, const juce::Component& c)
    {
        const auto b = c.getBounds();
        if (!b.isEmpty())
        {
            g.setColour(Nova::Colors::TextDim);
            g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            g.drawText(txt.toUpperCase(), b.getX() - 4, b.getY() - 12, b.getWidth() + 8, 12, juce::Justification::centred);
        }
    };

    if (isInput)
    {
        drawLabel("Gain", inputVolume);
        drawLabel("Gate", inputGate);
        drawLabel("Trans", inputTranspose);
    }
    else
    {
        drawLabel("Master", outputVolume);
        drawLabel("Limit", outputGain);
        drawLabel("Mix", outputMix);
    }
}

// ==============================================================================
// RESIZED (layout only)
// ==============================================================================

void NOVAAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    if (currentOverlay) currentOverlay->setBounds(area);
    if (tunerOverlay && tunerOverlay->isVisible()) tunerOverlay->setBounds(area);

    constexpr int headerH = 80;
    constexpr int footerH = 100;
    constexpr int stripW = 80;
    constexpr int drawerW = 220;

    constexpr int knobSz = 60;
    constexpr int knobH = 78;
    constexpr int knobGap = 10;

    // Header
    auto header = area.removeFromTop(headerH);
    const int headerCentreX = header.getCentreX();
    const int tunerW = 42;
    const int tunerH = 28;
    const int powerW = 132;
    const int powerH = 28;
    const int routeH = 30;
    const int routeButtonW = 84;
    const int routeButtonGap = 6;
    const int clusterGap = 16;
    const int infoSize = 16;
    const int infoGap = 10;
    const int rowY = header.getCentreY() - routeH / 2;
    const int routeGroupW = routeButtonW * 3 + routeButtonGap * 2;
    const int clusterW = tunerW + clusterGap + routeGroupW + infoGap + infoSize + clusterGap + powerW;
    int clusterX = headerCentreX - clusterW / 2;

    btnToggleLeft.setBounds(header.getX() + 10, header.getCentreY() - 15, 36, 30);
    btnToggleRight.setBounds(header.getRight() - 90, header.getCentreY() - 15, 80, 30);
    btnTuner.setBounds(clusterX, rowY + (routeH - tunerH) / 2, tunerW, tunerH);
    clusterX += tunerW + clusterGap;

    btnRouteA.setBounds(clusterX, rowY, routeButtonW, routeH);
    clusterX += routeButtonW + routeButtonGap;
    btnRoutePar.setBounds(clusterX, rowY, routeButtonW, routeH);
    clusterX += routeButtonW + routeButtonGap;
    btnRouteB.setBounds(clusterX, rowY, routeButtonW, routeH);
    clusterX += routeButtonW + infoGap;

    routeInfoIconBounds = juce::Rectangle<float>((float)clusterX,
        (float)(header.getCentreY() - infoSize / 2),
        (float)infoSize,
        (float)infoSize);
    clusterX += infoSize + clusterGap;

    btnStartStop.setBounds(clusterX, rowY + (routeH - powerH) / 2, powerW, powerH);

    // Footer
    auto footer = area.removeFromBottom(footerH);
    auto footerContent = footer.reduced(18, 12);
    auto waveArea = footerContent.removeFromLeft(juce::roundToInt((float)footerContent.getWidth() * 0.58f));
    waveArea.removeFromRight(16);
    audioProcessor.audioVisualizer.setBounds(waveArea.reduced(0, 6));

    auto statsArea = footerContent.reduced(8, 6);
    auto statRow = juce::Rectangle<int>(statsArea.getX(),
        statsArea.getCentreY() - 24,
        statsArea.getWidth(),
        48);
    constexpr int statGap = 12;
    const int statW = juce::jmax(72, (statRow.getWidth() - statGap * 2) / 3);
    lblCpu.setBounds(statRow.removeFromLeft(statW));
    statRow.removeFromLeft(statGap);
    lblProc.setBounds(statRow.removeFromLeft(statW));
    statRow.removeFromLeft(statGap);
    lblBuf.setBounds(statRow);
    lblStats.setBounds(statsArea.withTrimmedTop(statsArea.getHeight() - 18));

    // Input strip (left edge — knobs stacked, fader below)
    auto left2 = area.removeFromLeft(stripW);
    {
        const int kW = 48, kH = 58, toggleH = 26;
        const int topPad = 52;
        const int meterReserve = 182;
        const int controlsTop = left2.getY() + topPad;
        const int controlsBottom = left2.getBottom() - meterReserve;
        const int totalControlsH = kH * 3 + toggleH;
        const int gap = juce::jmax(12, (controlsBottom - controlsTop - totalControlsH) / 3);
        const int kX = left2.getX() + (left2.getWidth() - kW) / 2;
        const int toggleW = left2.getWidth() - 12;
        const int toggleX = left2.getX() + (left2.getWidth() - toggleW) / 2;

        int y = controlsTop;
        inputVolume.setBounds(kX, y, kW, kH);    y += kH + gap;
        inputGate.setBounds(kX, y, kW, kH);      y += kH + gap;
        inputTranspose.setBounds(kX, y, kW, kH); y += kH + gap;
        btnMonoStereo.setBounds(toggleX, y, toggleW, toggleH);
    }

    // Output strip (right edge — knobs stacked, fader below)
    auto right1 = area.removeFromRight(stripW);
    {
        const int kW = 48, kH = 58;
        const int topPad = 52;
        const int meterReserve = 182;
        const int controlsTop = right1.getY() + topPad;
        const int controlsBottom = right1.getBottom() - meterReserve;
        const int totalControlsH = kH * 3;
        const int gap = juce::jmax(18, (controlsBottom - controlsTop - totalControlsH) / 2);
        const int kX = right1.getX() + (right1.getWidth() - kW) / 2;

        int y = controlsTop;
        outputVolume.setBounds(kX, y, kW, kH); y += kH + gap;
        outputGain.setBounds(kX, y, kW, kH);   y += kH + gap;
        outputMix.setBounds(kX, y, kW, kH);
    }

    // Center: mixer + lanes
    const bool showA = laneA && laneA->isVisible();
    const bool showB = laneB && laneB->isVisible();
    const bool dualMode = showA && showB;

    const int mixerH = dualMode ? 0 : 170;

    auto center = area;
    auto mixerArea = center.removeFromBottom(mixerH);

    if (dualMode)
    {
        const auto hidden = juce::Rectangle<int>();
        volSliderA.setBounds(hidden);
        panSliderA.setBounds(hidden);
        widthSliderA.setBounds(hidden);
        volSliderB.setBounds(hidden);
        panSliderB.setBounds(hidden);
        widthSliderB.setBounds(hidden);
        btnSwitcher.setBounds(hidden);
    }
    else
    {
        // Single line: keep the active 3-knob group visually centered.
        const int yKnobs = mixerArea.getCentreY() - (knobH / 2);
        const int centeredKnobGap = knobGap + 8;
        const int groupW = knobSz * 3 + centeredKnobGap * 2;
        const int startX = mixerArea.getCentreX() - groupW / 2;

        if (showA)
        {
            volSliderA.setBounds(startX, yKnobs, knobSz, knobH);
            panSliderA.setBounds(startX + knobSz + centeredKnobGap, yKnobs, knobSz, knobH);
            widthSliderA.setBounds(startX + (knobSz + centeredKnobGap) * 2, yKnobs, knobSz, knobH);
        }
        else if (showB)
        {
            volSliderB.setBounds(startX, yKnobs, knobSz, knobH);
            panSliderB.setBounds(startX + knobSz + centeredKnobGap, yKnobs, knobSz, knobH);
            widthSliderB.setBounds(startX + (knobSz + centeredKnobGap) * 2, yKnobs, knobSz, knobH);
        }

        btnSwitcher.setBounds({});
    }

    // Lanes: full height for single visible lane, split 50/50 for dual
    if (dualMode)
    {
        const int lanePad = 4;
        const int laneGap = 4;
        const int laneH = (center.getHeight() - laneGap) / 2;
        laneA->setBounds(center.removeFromTop(laneH).reduced(lanePad));
        center.removeFromTop(laneGap);
        laneB->setBounds(center.reduced(lanePad));
    }
    else if (showA)
    {
        laneA->setBounds(center.reduced(10));
    }
    else if (showB)
    {
        laneB->setBounds(center.reduced(10));
    }

    // Sidebar drawers (overlay, positioned absolutely)
    const int drawerTop = headerH;
    const int drawerH = getHeight() - headerH - footerH;

    if (leftPanelOpen)
    {
        leftDrawer.setBounds(0, drawerTop, drawerW, drawerH);
        leftDrawer.toFront(false);

        auto d = leftDrawer.getLocalBounds();
        d.removeFromTop(36);
        d = d.reduced(12, 8);
        searchBarBrowser.setBounds(d.removeFromTop(34));
        d.removeFromTop(12);
        quickAddViewport.setBounds(d);
        layoutQuickAddButtons();
    }

    if (rightPanelOpen)
    {
        rightDrawer.setBounds(getWidth() - drawerW, drawerTop, drawerW, drawerH);
        rightDrawer.toFront(false);

        auto d = rightDrawer.getLocalBounds();
        d.removeFromTop(36);
        d = d.reduced(14, 10);
        searchBarPresets.setBounds(d.removeFromTop(34));
        d.removeFromTop(12);
        lblCurrentPreset.setBounds(d.removeFromTop(26));
        d.removeFromTop(10);
        presetSelector.setBounds(d.removeFromTop(36));
        d.removeFromTop(14);

        auto btnArea = d.removeFromBottom(108);
        auto topButtons = btnArea.removeFromTop(46);
        btnSave.setBounds(topButtons.removeFromLeft(topButtons.getWidth() / 2).reduced(4));
        btnLoad.setBounds(topButtons.reduced(4));
        btnArea.removeFromTop(10);
        btnClear.setBounds(btnArea.removeFromTop(46).reduced(4));
    }

    updatePedalGui();
}

// ==============================================================================
// CONTROL LOGIC
// ==============================================================================

void NOVAAudioProcessorEditor::toggleLeftPanel()
{
    leftPanelOpen = !leftPanelOpen;
    leftDrawer.setVisible(leftPanelOpen);

    btnToggleLeft.setColour(juce::TextButton::buttonColourId,
        leftPanelOpen ? Nova::Colors::Accent.withAlpha(0.28f) : juce::Colour::fromString("ff1A2332"));

    resized();
    repaint();
}

void NOVAAudioProcessorEditor::toggleRightPanel()
{
    rightPanelOpen = !rightPanelOpen;
    rightDrawer.setVisible(rightPanelOpen);

    btnToggleRight.setColour(juce::TextButton::buttonColourId,
        rightPanelOpen ? Nova::Colors::Accent.withAlpha(0.28f) : juce::Colour::fromString("ff1A2332"));

    resized();
    repaint();
}

void NOVAAudioProcessorEditor::toggleTuner()
{
    audioProcessor.toggleTuner();
    const bool newState = audioProcessor.getAudioEngine().isTunerEnabled();

    btnTuner.setColour(juce::TextButton::buttonColourId,
        newState ? Nova::Colors::Success.withAlpha(0.28f) : juce::Colour::fromString("ff0F3D22"));

    if (newState)
    {
        tunerOverlay = std::make_unique<TunerOverlay>(audioProcessor);
        addAndMakeVisible(tunerOverlay.get());
        tunerOverlay->setBounds(getLocalBounds());
        tunerOverlay->toFront(true);
    }
    else
    {
        tunerOverlay.reset();
    }
}

void NOVAAudioProcessorEditor::updateStats()
{
    const double sampleRate = audioProcessor.getSampleRate();
    const double bufferSize = (double)audioProcessor.getBlockSize();
    const double cpuPercent = audioProcessor.getCpuUsage();

    double bufferDurationMs = 0.0;
    double procTimeMs = 0.0;

    if (sampleRate > 0)
    {
        bufferDurationMs = (bufferSize / sampleRate) * 1000.0;
        procTimeMs = (cpuPercent / 100.0) * bufferDurationMs;
    }

    // Detect auto-heal events from the engine and show a brief visible warning.
    const int currentHealCount = audioProcessor.getAudioEngine().getAutoHealCount();
    if (currentHealCount != lastKnownAutoHealCount)
    {
        lastKnownAutoHealCount = currentHealCount;
        autoHealFlashFrames = 45; // ~3 seconds at 15 Hz
    }

    if (autoHealFlashFrames > 0)
    {
        --autoHealFlashFrames;
        lblStats.setText("AUTO-HEAL TRIGGERED", juce::dontSendNotification);
    }
    else
    {
        lblStats.setText({}, juce::dontSendNotification);
    }

    lblCpu.setText("CPU USAGE\n" + juce::String(cpuPercent, 1) + "%", juce::dontSendNotification);
    lblProc.setText("PROCESS TIME\n" + juce::String(procTimeMs, 2) + " ms", juce::dontSendNotification);
    lblBuf.setText("BUFFER TIME\n" + juce::String(bufferDurationMs, 1) + " ms", juce::dontSendNotification);

    if (autoHealFlashFrames > 0)
        lblStats.setColour(juce::Label::textColourId, Nova::Colors::Error);
    else if (cpuPercent > 90.0)
        lblStats.setColour(juce::Label::textColourId, juce::Colours::red);
    else
        lblStats.setColour(juce::Label::textColourId, Nova::Colors::TextDim);
}

void NOVAAudioProcessorEditor::updatePedalGui()
{
    std::set<juce::AudioProcessorGraph::NodeID> requiredNodeIDs;

    auto processLane = [&](Nova::ChainID chain, ChainLane* laneComp)
        {
            if (!laneComp)
                return;

            const auto engineNodes = audioProcessor.getAudioEngine().getNodes(chain);
            const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
            auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);

            struct DrawItem
            {
                juce::ValueTree state;
                AudioEngine::ChainNodeView nodeView;
                int zoneIdx = 0;
                PedalSlotComponent* slot = nullptr;
                int preferredW = 120;
                int preferredH = 180;
                bool createdNow = false;
            };

            std::vector<DrawItem> itemsToDraw;
            itemsToDraw.reserve((size_t)treeList.getNumChildren());
            std::set<juce::AudioProcessorGraph::NodeID> usedNodeIDs;

            for (int i = 0; i < treeList.getNumChildren(); ++i)
            {
                auto state = treeList.getChild(i);
                if (state.getType() != Nova::IDs::PEDAL)
                    continue;

                juce::String expectedType = state.getProperty(Nova::IDs::PEDAL_TYPE).toString();
                const int zIdx = static_cast<int>(state.getProperty(Nova::IDs::PEDAL_ZONE, 0));
                if (zIdx < 0 || zIdx > 3)
                    continue;

                AudioEngine::ChainNodeView matchedNodeView;
                bool foundMatch = false;
                const auto pedalID = state.getProperty(Nova::IDs::PEDAL_ID).toString();

                for (const auto& candidate : engineNodes)
                {
                    if (candidate.node == nullptr || candidate.node->getProcessor() == nullptr)
                        continue;
                    if (usedNodeIDs.find(candidate.node->nodeID) != usedNodeIDs.end())
                        continue;

                    if (candidate.pedalID == pedalID && pedalID.isNotEmpty())
                    {
                        matchedNodeView = candidate;
                        foundMatch = true;
                        break;
                    }
                }

                if (!foundMatch)
                {
                    for (const auto& candidate : engineNodes)
                    {
                        if (candidate.node == nullptr || candidate.node->getProcessor() == nullptr)
                            continue;
                        if (usedNodeIDs.find(candidate.node->nodeID) != usedNodeIDs.end())
                            continue;

                        juce::String procName = candidate.node->getProcessor()->getName();
                        if (procName.containsIgnoreCase(expectedType) || expectedType.containsIgnoreCase(procName))
                        {
                            matchedNodeView = candidate;
                            foundMatch = true;
                            break;
                        }
                    }
                }

                if (foundMatch)
                {
                    usedNodeIDs.insert(matchedNodeView.node->nodeID);
                    itemsToDraw.push_back({ state, matchedNodeView, zIdx });
                }
            }

            for (auto& item : itemsToDraw)
            {
                requiredNodeIDs.insert(item.nodeView.node->nodeID);

                auto it = activePedalEditors.find(item.nodeView.node->nodeID);
                PedalSlotComponent* slot = nullptr;

                if (it == activePedalEditors.end())
                {
                    auto host = std::make_unique<PedalSlotComponent>(audioProcessor, chain,
                        [this](Nova::ChainID chainId, juce::String pedalId)
                        {
                            showPedalEditor(chainId, pedalId);
                        });
                    host->setPedalState(item.state);
                    addAndMakeVisible(host.get());
                    slot = host.get();
                    activePedalEditors[item.nodeView.node->nodeID] = std::move(host);
                    item.createdNow = true;
                }
                else
                {
                    slot = it->second.get();
                    slot->setPedalState(item.state);
                }

                if (slot)
                {
                    item.slot = slot;
                    item.preferredW = slot->getPreferredWidth();
                    item.preferredH = slot->getPreferredHeight();
                }
            }

            for (int zoneIdx = 0; zoneIdx < 4; ++zoneIdx)
            {
                std::vector<DrawItem*> zoneItems;
                zoneItems.reserve(itemsToDraw.size());

                for (auto& item : itemsToDraw)
                    if (item.zoneIdx == zoneIdx && item.slot != nullptr)
                        zoneItems.push_back(&item);

                if (zoneItems.empty())
                    continue;

                const auto zoneRect = laneComp->getZoneRect(zoneIdx);
                if (zoneRect.isEmpty())
                    continue;

                const int zoneAbsX = laneComp->getX() + zoneRect.getX();
                const int zoneAbsY = laneComp->getY() + zoneRect.getY();
                const int zoneW = zoneRect.getWidth();
                const int zoneH = zoneRect.getHeight();

                if (zoneIdx == (int)Nova::ZoneID::Amp || zoneIdx == (int)Nova::ZoneID::Cabinet)
                {
                    for (auto* item : zoneItems)
                    {
                        const int finalX = zoneAbsX + (zoneW - item->preferredW) / 2;
                        const int finalY = zoneAbsY + (zoneH - item->preferredH) / 2;
                        item->slot->setBounds(finalX, finalY, item->preferredW, item->preferredH);

                        if (item->createdNow)
                            item->slot->toFront(false);
                    }

                    continue;
                }

                // ---- Multi-row adaptive layout for Pre / FX zones ----
                const int itemCount = (int)zoneItems.size();
                const auto layout = calculateFlexLayout(zoneW, zoneH, itemCount);
                const int visibleCount = juce::jmin(itemCount, layout.maxCapacity);

                const int actualRows = (visibleCount + layout.cols - 1) / layout.cols;

                // Center the entire grid block within the zone
                const int colsFirstRow = juce::jmin(layout.cols, visibleCount);
                const int blockW = colsFirstRow * layout.cardW + (colsFirstRow - 1) * layout.gapH;
                const int blockH = actualRows * layout.cardH + juce::jmax(0, actualRows - 1) * layout.gapV;
                const int blockStartY = zoneAbsY + (zoneH - blockH) / 2;

                for (int i = 0; i < visibleCount; ++i)
                {
                    const int row = i / layout.cols;
                    const int col = i % layout.cols;

                    // Items in this specific row (last row may have fewer)
                    const int itemsInRow = (row < actualRows - 1)
                        ? layout.cols
                        : (visibleCount - row * layout.cols);
                    const int rowW = itemsInRow * layout.cardW + (itemsInRow - 1) * layout.gapH;
                    const int rowStartX = zoneAbsX + (zoneW - rowW) / 2;

                    const int x = rowStartX + col * (layout.cardW + layout.gapH);
                    const int y = blockStartY + row * (layout.cardH + layout.gapV);

                    zoneItems[(size_t)i]->slot->setBounds(x, y, layout.cardW, layout.cardH);

                    if (zoneItems[(size_t)i]->createdNow)
                        zoneItems[(size_t)i]->slot->toFront(false);
                }
            }
        };

    if (laneA && laneA->isVisible())
        processLane(Nova::ChainID::LineA, laneA.get());
    if (laneB && laneB->isVisible())
        processLane(Nova::ChainID::LineB, laneB.get());

    for (auto it = activePedalEditors.begin(); it != activePedalEditors.end();)
    {
        if (requiredNodeIDs.find(it->first) == requiredNodeIDs.end())
            it = activePedalEditors.erase(it);
        else
            ++it;
    }

    if (currentOverlay)
        currentOverlay->toFront(true);
    if (tunerOverlay && tunerOverlay->isVisible())
        tunerOverlay->toFront(true);
}

void NOVAAudioProcessorEditor::updateSwitcherState()
{
    const bool on = audioProcessor.isEngineOn();
    const int mode = (int)audioProcessor.getSwitcherMode();
    const bool dualParallel = (mode == (int)Nova::SwitcherMode::Dual_Parallel);

    btnStartStop.setButtonText(on ? "POWER ON" : "POWER OFF");
    btnStartStop.setColour(juce::TextButton::buttonColourId,
        on ? juce::Colour::fromString("ff0F3D22") : juce::Colour::fromString("ff3D1418"));
    btnStartStop.setColour(juce::TextButton::buttonOnColourId,
        on ? juce::Colour::fromString("ff0F3D22") : juce::Colour::fromString("ff3D1418"));
    btnStartStop.setToggleState(on, juce::dontSendNotification);

    juce::String txt;
    bool aActive = false, bActive = false;
    juce::Colour switcherColour = juce::Colour::fromString("ff1A2332");

    if (mode == (int)Nova::SwitcherMode::LineA_Only)
    {
        txt = "ROUTING: LINE A";
        aActive = true;
        switcherColour = Nova::Colors::CableOnA.withAlpha(0.28f);
    }
    else if (mode == (int)Nova::SwitcherMode::LineB_Only)
    {
        txt = "ROUTING: LINE B";
        bActive = true;
        switcherColour = Nova::Colors::CableOnB.withAlpha(0.28f);
    }
    else
    {
        txt = "ROUTING: DUAL PARALLEL";
        aActive = true;
        bActive = true;
        switcherColour = Nova::Colors::Accent.withAlpha(0.18f);
    }

    btnSwitcher.setButtonText(txt);
    btnSwitcher.setColour(juce::TextButton::buttonColourId, on ? switcherColour : juce::Colour::fromString("ff0D1520"));
    btnSwitcher.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(on ? 0.92f : 0.5f));

    auto applyRouteState = [on](juce::TextButton& button, bool active, juce::Colour activeColour)
    {
        const auto fill = active
            ? activeColour.withAlpha(on ? 0.52f : 0.22f)
            : juce::Colour::fromString("ff131A26");
        button.setColour(juce::TextButton::buttonColourId, fill);
        button.setColour(juce::TextButton::buttonOnColourId, fill);
        button.setColour(juce::TextButton::textColourOffId,
            active ? juce::Colours::white.withAlpha(on ? 0.94f : 0.55f) : Nova::Colors::TextDim.withAlpha(0.88f));
        button.setColour(juce::TextButton::textColourOnId,
            active ? juce::Colours::white.withAlpha(on ? 0.98f : 0.65f) : Nova::Colors::TextDim.withAlpha(0.88f));
    };

    applyRouteState(btnRouteA, mode == (int)Nova::SwitcherMode::LineA_Only, Nova::Colors::CableOnA);
    applyRouteState(btnRoutePar, mode == (int)Nova::SwitcherMode::Dual_Parallel, Nova::Colors::Accent);
    applyRouteState(btnRouteB, mode == (int)Nova::SwitcherMode::LineB_Only, Nova::Colors::CableOnB);

    // Show only the active line(s) — single-line modes get full vertical space
    bool layoutChanged = false;

    if (laneA && laneA->isVisible() != aActive)
    {
        laneA->setVisible(aActive);
        layoutChanged = true;
    }
    if (laneB && laneB->isVisible() != bActive)
    {
        laneB->setVisible(bActive);
        layoutChanged = true;
    }

    const bool showMixerControls = !dualParallel;
    volSliderA.setVisible(aActive && showMixerControls);
    panSliderA.setVisible(aActive && showMixerControls);
    widthSliderA.setVisible(aActive && showMixerControls);

    volSliderB.setVisible(bActive && showMixerControls);
    panSliderB.setVisible(bActive && showMixerControls);
    widthSliderB.setVisible(bActive && showMixerControls);

    if (laneA) laneA->setActive(aActive && on);
    if (laneB) laneB->setActive(bActive && on);

    if (layoutChanged)
        resized();

    repaint();
}

void NOVAAudioProcessorEditor::requestUiRefresh()
{
    // Always defer to next message-loop iteration via triggerAsyncUpdate().
    // DO NOT run handleAsyncUpdate() synchronously here — when called from
    // valueTreeChildAdded during requestAddPedal, the engine rebuild hasn't
    // happened yet, so updatePedalGui() would see stale engine state (the
    // "always one action behind" bug). Deferring ensures the full
    // requestAddPedal/requestMovePedal call completes before the UI refreshes.
    const bool wasPending = uiRefreshPending.exchange(true, std::memory_order_acq_rel);
    if (!wasPending)
        triggerAsyncUpdate();
}

void NOVAAudioProcessorEditor::handleAsyncUpdate()
{
    uiRefreshPending.store(false, std::memory_order_release);
    updateSwitcherState();
    updatePedalGui();
}

void NOVAAudioProcessorEditor::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& id)
{
    if (id == Nova::IDs::PEDAL_ENABLED
        || id == Nova::IDs::PEDAL_TYPE
        || id == Nova::IDs::PEDAL_ZONE
        || id == Nova::IDs::ENGINE_ON
        || id == Nova::IDs::SWITCH_MODE)
    {
        requestUiRefresh();
    }
}

void NOVAAudioProcessorEditor::mouseMove(const juce::MouseEvent& e)
{
    const bool shouldHover = routeInfoIconBounds.contains(e.position);
    if (routeInfoHovered != shouldHover)
    {
        routeInfoHovered = shouldHover;
        repaint();
    }
}

void NOVAAudioProcessorEditor::mouseExit(const juce::MouseEvent&)
{
    if (routeInfoHovered)
    {
        routeInfoHovered = false;
        repaint();
    }
}

bool NOVAAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        audioProcessor.cycleSwitcher();
        return true;
    }
    return false;
}

void NOVAAudioProcessorEditor::showPedalEditor(Nova::ChainID chain, const juce::String& pedalID)
{
    const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
    auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);
    if (!treeList.isValid())
        return;

    int pedalIndex = -1;
    juce::ValueTree pedalState;

    for (int i = 0; i < treeList.getNumChildren(); ++i)
    {
        auto child = treeList.getChild(i);
        if (!child.hasType(Nova::IDs::PEDAL))
            continue;

        if (child.getProperty(Nova::IDs::PEDAL_ID).toString() == pedalID)
        {
            pedalIndex = i;
            pedalState = child;
            break;
        }
    }

    if (pedalIndex < 0 || !pedalState.isValid())
        return;

    auto* processorForPedal = audioProcessor.getAudioEngine().getProcessorForPedal(chain, pedalIndex);
    if (processorForPedal == nullptr)
        return;

    std::unique_ptr<juce::AudioProcessorEditor> pedalEditor(processorForPedal->createEditor());
    if (pedalEditor == nullptr)
        pedalEditor = std::make_unique<juce::GenericAudioProcessorEditor>(*processorForPedal);

    if (pedalEditor == nullptr)
        return;

    const auto pedalType = pedalState.getProperty(Nova::IDs::PEDAL_TYPE).toString();
    auto overlay = std::make_unique<PedalEditorOverlay>(
        pedalType,
        Nova::PedalCatalog::subtitleForType(pedalType),
        std::move(pedalEditor),
        [this]()
        {
            juce::MessageManager::callAsync([this]()
            {
                currentOverlay.reset();
                resized();
            });
        },
        Nova::PedalCatalog::accentForType(pedalType));

    addAndMakeVisible(overlay.get());
    overlay->setBounds(getLocalBounds());
    overlay->toFront(true);
    overlay->grabKeyboardFocus();
    currentOverlay = std::move(overlay);
}

std::vector<juce::Rectangle<int>> NOVAAudioProcessorEditor::getPedalBoundsForZone(Nova::ChainID chain, Nova::ZoneID zone) const
{
    std::vector<juce::Rectangle<int>> bounds;

    const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
    auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);
    if (!treeList.isValid())
        return bounds;

    bounds.reserve((size_t)treeList.getNumChildren());

    for (int i = 0; i < treeList.getNumChildren(); ++i)
    {
        auto state = treeList.getChild(i);
        if (!state.hasType(Nova::IDs::PEDAL))
            continue;

        const auto stateZone = static_cast<Nova::ZoneID>(
            (int)state.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
        if (stateZone != zone)
            continue;

        const auto pedalID = state.getProperty(Nova::IDs::PEDAL_ID).toString();
        if (pedalID.isEmpty())
            continue;

        for (const auto& entry : activePedalEditors)
        {
            auto* slot = entry.second.get();

            if (slot == nullptr || slot->getChain() != chain || slot->getZone() != zone)
                continue;

            if (slot->getPedalID() == pedalID)
            {
                bounds.push_back(slot->getBounds());
                break;
            }
        }
    }

    return bounds;
}

void NOVAAudioProcessorEditor::showOverlay(Nova::ZoneID zone, Nova::ChainID chain)
{
    auto overlay = std::make_unique<AssetBrowserOverlay>(
        zone,
        [this, zone, chain](juce::String typeID)
        {
            audioProcessor.requestAddPedal(typeID, chain, zone);
        },
        [this]()
        {
            juce::MessageManager::callAsync([this]()
                {
                    currentOverlay.reset();
                    resized();
                });
        }
    );

    addAndMakeVisible(overlay.get());
    overlay->setBounds(getLocalBounds());
    currentOverlay = std::move(overlay);
}
