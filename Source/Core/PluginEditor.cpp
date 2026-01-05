#include "PluginProcessor.h"
#include "PluginEditor.h"

// ==============================================================================
// 1. ESTILO Y COLORES
// ==============================================================================
namespace NovaColors
{
    const juce::Colour Background = juce::Colour::fromString("ff111111");
    const juce::Colour Panel = juce::Colour::fromString("ff1a1a1a");
    const juce::Colour Border = juce::Colour::fromString("ff444444");
    const juce::Colour Accent = juce::Colour::fromString("ff00ff00");

    const juce::Colour MixerPanel = juce::Colour::fromString("ff1e1e1e");
    const juce::Colour ZoneOutline = juce::Colour::fromString("ff555555");
    const juce::Colour GridLine = juce::Colour::fromString("ff333333"); // Color de la rejilla

    const juce::Colour CableOff = juce::Colour::fromString("ff333333");
    const juce::Colour CableOnA = juce::Colour::fromString("ff00aaff");
    const juce::Colour CableOnB = juce::Colour::fromString("ffffaa00");
}

// ==============================================================================
// 2. CLASES INTERNAS (ASSETS, OVERLAY, DROPZONE)
// ==============================================================================

// --- AssetItem (Sin cambios) ---
class AssetItem : public juce::Component
{
public:
    AssetItem(const juce::String& name, const juce::String& type, std::function<void()> onSelect)
        : itemName(name), itemType(type), onSelectCallback(onSelect)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void mouseUp(const juce::MouseEvent&) override { if (onSelectCallback) onSelectCallback(); }
    void mouseEnter(const juce::MouseEvent&) override { isHover = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { isHover = false; repaint(); }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(isHover ? juce::Colours::white.withAlpha(0.1f) : juce::Colours::transparentBlack);
        g.fillRoundedRectangle(area, 6.0f);

        auto iconArea = area.removeFromTop(area.getHeight() * 0.7f).reduced(10);
        g.setColour(juce::Colour::fromString("ff202020"));
        g.fillRoundedRectangle(iconArea, 4.0f);
        g.setColour(juce::Colours::white.withAlpha(0.2f));
        g.drawRoundedRectangle(iconArea, 4.0f, 1.0f);

        if (itemType == "Amp") {
            g.setColour(juce::Colours::grey);
            float yKnob = iconArea.getCentreY();
            for (int i = 0; i < 4; ++i) g.fillEllipse(iconArea.getX() + 10 + (i * 15), yKnob - 4, 8, 8);
        }
        else {
            g.setColour(juce::Colours::black.withAlpha(0.3f));
            g.fillEllipse(iconArea.getCentreX() - 15, iconArea.getCentreY() - 15, 12, 12);
            g.fillEllipse(iconArea.getCentreX() + 3, iconArea.getCentreY() - 15, 12, 12);
            g.fillEllipse(iconArea.getCentreX() - 15, iconArea.getCentreY() + 3, 12, 12);
            g.fillEllipse(iconArea.getCentreX() + 3, iconArea.getCentreY() + 3, 12, 12);
        }
        g.setColour(juce::Colours::white);
        g.setFont(14.0f);
        g.drawText(itemName, area, juce::Justification::centred);
        if (isHover) {
            g.setColour(NovaColors::CableOnA);
            g.drawRoundedRectangle(getLocalBounds().toFloat(), 6.0f, 1.5f);
        }
    }
private:
    juce::String itemName;
    juce::String itemType;
    std::function<void()> onSelectCallback;
    bool isHover = false;
};

// --- AssetBrowserOverlay (Sin cambios importantes) ---
class AssetBrowserOverlay : public juce::Component, public juce::TextEditor::Listener
{
public:
    AssetBrowserOverlay(Nova::ZoneID zone, std::function<void(juce::String)> onAssetSelected, std::function<void()> onClose)
        : targetZone(zone), onSelect(onAssetSelected), onClose(onClose)
    {
        addAndMakeVisible(searchBar);
        searchBar.setMultiLine(false);
        searchBar.setTextToShowWhenEmpty("Search model...", juce::Colours::grey);
        searchBar.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromString("ff151515"));
        searchBar.setColour(juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha(0.2f));
        searchBar.addListener(this);

        addAndMakeVisible(viewport);
        container.reset(new juce::Component());
        viewport.setViewedComponent(container.get(), false);
        viewport.setScrollBarsShown(true, false);

        addAndMakeVisible(closeBtn);
        closeBtn.setButtonText("X");
        closeBtn.onClick = onClose;
        closeBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);

        populateList("");
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black.withAlpha(0.85f));
        auto area = getLocalBounds().reduced(100, 50);
        g.setColour(NovaColors::MixerPanel);
        g.fillRoundedRectangle(area.toFloat(), 12.0f);
        g.setColour(juce::Colours::white.withAlpha(0.1f));
        g.drawRoundedRectangle(area.toFloat(), 12.0f, 1.0f);
        g.setColour(juce::Colours::white);
        g.setFont(24.0f);
        juce::String title = (targetZone == Nova::ZoneID::Amp) ? "SELECT AMPLIFIER" : "SELECT CABINET";
        g.drawText(title, area.removeFromTop(60), juce::Justification::centred);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(100, 50);
        closeBtn.setBounds(area.getRight() - 40, area.getY() + 10, 30, 30);
        area.removeFromTop(60);
        searchBar.setBounds(area.removeFromTop(40).reduced(100, 0));
        area.removeFromTop(20);
        viewport.setBounds(area.reduced(20));
        layoutItems();
    }
    void textEditorTextChanged(juce::TextEditor& editor) override { populateList(editor.getText()); }

private:
    void populateList(const juce::String& filter)
    {
        container->removeAllChildren();
        items.clear();
        std::vector<juce::String> mockData;
        if (targetZone == Nova::ZoneID::Amp) mockData = { "British Lead 800", "USA Rectifier", "Jazz Clean 120", "German Fireball", "Blues Junior", "Bass SuperTube" };
        else mockData = { "4x12 Vintage 30", "2x12 Greenback", "1x12 Blue Alnico", "8x10 Bass Fridge", "4x12 Recto Std", "2x10 Tremolo" };

        for (const auto& name : mockData) {
            if (filter.isNotEmpty() && !name.containsIgnoreCase(filter)) continue;
            auto* item = new AssetItem(name, (targetZone == Nova::ZoneID::Amp ? "Amp" : "Cab"), [this, name]() {
                juce::String internalID = (targetZone == Nova::ZoneID::Amp) ? "Overdrive" : "Cabinet";
                if (onSelect) onSelect(internalID);
                if (onClose) onClose();
                });
            container->addAndMakeVisible(item);
            items.add(item);
        }
        layoutItems();
    }
    void layoutItems()
    {
        int itemSize = 140; int gap = 20;
        int cols = juce::jmax(1, viewport.getWidth() / (itemSize + gap));
        int x = 0, y = 0, col = 0;
        for (auto* item : items) {
            item->setBounds(x, y, itemSize, itemSize);
            col++;
            if (col >= cols) { col = 0; x = 0; y += itemSize + gap; }
            else { x += itemSize + gap; }
        }
        container->setSize(viewport.getWidth(), y + itemSize + gap);
    }
    Nova::ZoneID targetZone;
    std::function<void(juce::String)> onSelect;
    std::function<void()> onClose;
    juce::TextEditor searchBar;
    juce::Viewport viewport;
    std::unique_ptr<juce::Component> container;
    juce::OwnedArray<AssetItem> items;
    juce::TextButton closeBtn;
};

// --- DropZone: Zona magnética con REJILLA VISUAL ---
class DropZone : public juce::Component, public juce::DragAndDropTarget
{
public:
    DropZone(NOVAAudioProcessor& p, Nova::ChainID c, Nova::ZoneID z)
        : proc(p), chain(c), zone(z) {
    }

    bool isInterestedInDragSource(const SourceDetails&) override
    {
        if (zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet) return false;
        return true;
    }

    void itemDropped(const SourceDetails& d) override
    {
        isHover = false; repaint();
        proc.requestAddPedal(d.description.toString(), chain, zone);
    }
    void itemDragEnter(const SourceDetails&) override { isHover = true; repaint(); }
    void itemDragExit(const SourceDetails&) override { isHover = false; repaint(); }
    void mouseDown(const juce::MouseEvent& e) override
    {
        if ((zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet) && e.mods.isLeftButtonDown())
            if (auto* mainEditor = findParentComponentOfClass<NOVAAudioProcessorEditor>())
                mainEditor->showOverlay(zone, chain);
    }

    void paint(juce::Graphics& g) override
    {
        bool isFixedSlot = (zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet);

        // --- UX MEJORA: Rejilla "Empty Tech" ---
        if (!isFixedSlot) {
            g.setColour(NovaColors::GridLine.withAlpha(0.2f));
            // Líneas verticales
            for (int x = 0; x < getWidth(); x += 20) g.drawVerticalLine(x, 0.0f, (float)getHeight());
            // Líneas horizontales
            g.setColour(NovaColors::GridLine.withAlpha(0.1f));
            for (int y = 0; y < getHeight(); y += 20) g.drawHorizontalLine(y, 0.0f, (float)getWidth());
        }

        g.setColour(NovaColors::ZoneOutline.withAlpha(isFixedSlot ? 0.4f : 0.2f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2), 6.0f, 1.0f);

        if (isFixedSlot) {
            if (isMouseOver(true) && !isHover) g.setColour(juce::Colours::white.withAlpha(0.2f));
            else g.setColour(juce::Colours::white.withAlpha(0.05f));
            auto center = getLocalBounds().getCentre().toFloat();
            g.fillEllipse(center.x - 25, center.y - 25, 50, 50);
            g.setColour(juce::Colours::grey);
            g.fillRect(center.x - 1.5f, center.y - 12, 3.0f, 24.0f);
            g.fillRect(center.x - 12, center.y - 1.5f, 24.0f, 3.0f);
            g.setFont(12.0f);
            g.drawText(zone == Nova::ZoneID::Amp ? "ADD AMP" : "ADD CAB", getLocalBounds().removeFromBottom(30), juce::Justification::centred);
        }
        else {
            juce::String label = (zone == Nova::ZoneID::Pre) ? "PRE-FX" : "FX LOOP";
            g.setColour(juce::Colours::grey.withAlpha(0.3f)); g.setFont(12.f);
            g.drawText(label, getLocalBounds().removeFromBottom(25), juce::Justification::centred);
            if (isHover) {
                g.setColour(juce::Colours::cyan.withAlpha(0.2f)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.f);
                g.setColour(juce::Colours::cyan); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1), 6.f, 2.0f);
            }
        }
    }
private:
    NOVAAudioProcessor& proc;
    Nova::ChainID chain;
    Nova::ZoneID zone;
    bool isHover = false;
};

// --- ChainLane (Sin cambios) ---
class ChainLane : public juce::Component
{
public:
    ChainLane(NOVAAudioProcessor& p, Nova::ChainID c) : chainID(c)
    {
        zones.add(new DropZone(p, c, Nova::ZoneID::Pre));
        zones.add(new DropZone(p, c, Nova::ZoneID::Amp));
        zones.add(new DropZone(p, c, Nova::ZoneID::FX));
        zones.add(new DropZone(p, c, Nova::ZoneID::Cabinet));
        for (auto* z : zones) addAndMakeVisible(z);
    }
    void setActive(bool isActive) { isLaneActive = isActive; repaint(); }
    void resized() override
    {
        auto area = getLocalBounds();
        int totalW = area.getWidth(); int h = area.getHeight();
        int fixedZoneW = 240; int remainingW = totalW - (fixedZoneW * 2); int flexZoneW = remainingW / 2;
        zones[0]->setBounds(0, 0, flexZoneW, h);
        zones[1]->setBounds(flexZoneW, 0, fixedZoneW, h);
        zones[2]->setBounds(flexZoneW + fixedZoneW, 0, flexZoneW, h);
        zones[3]->setBounds(totalW - fixedZoneW, 0, fixedZoneW, h);
    }
    juce::Rectangle<int> getZoneRect(int zoneIndex) { if (zoneIndex >= 0 && zoneIndex < zones.size()) return zones[zoneIndex]->getBounds(); return {}; }
    void paint(juce::Graphics& g) override
    {
        float y = (float)getHeight() / 2.0f; float w = (float)getWidth();
        juce::Path cable; cable.startNewSubPath(0, y); cable.lineTo(w, y);
        juce::Colour glow = (chainID == Nova::ChainID::LineA) ? NovaColors::CableOnA : NovaColors::CableOnB;
        if (!isLaneActive) glow = NovaColors::CableOff;
        g.setColour(juce::Colours::black.withAlpha(0.6f)); g.strokePath(cable, juce::PathStrokeType(10.0f));
        g.setColour(juce::Colour::fromString("ff151515")); g.strokePath(cable, juce::PathStrokeType(6.0f));
        g.setColour(glow); g.strokePath(cable, juce::PathStrokeType(isLaneActive ? 2.0f : 1.0f));
        if (isLaneActive) { g.setColour(glow.withAlpha(0.4f)); g.strokePath(cable, juce::PathStrokeType(8.0f)); }
    }
private:
    juce::OwnedArray<DropZone> zones;
    Nova::ChainID chainID;
    bool isLaneActive = false;
};

// ==============================================================================
// 3. IMPLEMENTACIÓN EDITOR PRINCIPAL
// ==============================================================================

NOVAAudioProcessorEditor::NOVAAudioProcessorEditor(NOVAAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    // --- 1. HEADER ---
    addAndMakeVisible(btnStartStop);
    btnStartStop.setClickingTogglesState(true);
    btnStartStop.onClick = [this] { audioProcessor.toggleEngine(); };

    addAndMakeVisible(lblStats);
    lblStats.setJustificationType(juce::Justification::centred);
    lblStats.setColour(juce::Label::textColourId, juce::Colours::grey);
    lblStats.setFont(12.0f);
    lblStats.setText("CPU: - | Latency: -", juce::dontSendNotification);

    addAndMakeVisible(btnTuner);
    addAndMakeVisible(btnMetronome);
    addAndMakeVisible(btnSettings);
    addAndMakeVisible(btnProfile);

    // --- 2. BROWSER ---
    addAndMakeVisible(searchBarBrowser);
    searchBarBrowser.setTextToShowWhenEmpty("Search...", juce::Colours::grey);
    addAndMakeVisible(btnAddOverdrive);
    addAndMakeVisible(btnAddCabinet);
    addAndMakeVisible(btnAddNeural);

    // --- 3. INPUT STRIP ---
    // Vol
    addAndMakeVisible(inputVolume); inputVolume.setSliderStyle(juce::Slider::Rotary); inputVolume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    // Gate
    addAndMakeVisible(inputGate); inputGate.setSliderStyle(juce::Slider::Rotary); inputGate.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    // Transpose
    addAndMakeVisible(inputTranspose); inputTranspose.setSliderStyle(juce::Slider::Rotary); inputTranspose.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    // Mono/Stereo
    addAndMakeVisible(btnMonoStereo);
    // Fader
    addAndMakeVisible(inputFader); inputFader.setSliderStyle(juce::Slider::LinearVertical); inputFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);

    // --- 4. CENTER MIXER & LANES ---
    laneA = std::make_unique<ChainLane>(p, Nova::ChainID::LineA); addAndMakeVisible(laneA.get());
    laneB = std::make_unique<ChainLane>(p, Nova::ChainID::LineB); addAndMakeVisible(laneB.get());
    addAndMakeVisible(btnSwitcher);
    btnSwitcher.onClick = [this] { audioProcessor.cycleSwitcher(); };

    auto setupKnob = [&](juce::Slider& s) {
        addAndMakeVisible(s); s.setSliderStyle(juce::Slider::Rotary); s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        };

    // Line A
    setupKnob(volSliderA);
    setupKnob(panSliderA);   panSliderA.setRange(-1.0, 1.0, 0.01); panSliderA.setValue(0.0);
    setupKnob(widthSliderA); widthSliderA.setRange(0.0, 2.0, 0.01); widthSliderA.setValue(1.0);

    // Line B
    setupKnob(volSliderB);
    setupKnob(panSliderB);   panSliderB.setRange(-1.0, 1.0, 0.01); panSliderB.setValue(0.0);
    setupKnob(widthSliderB); widthSliderB.setRange(0.0, 2.0, 0.01); widthSliderB.setValue(1.0);

    // Conexión Volumen (El resto espera implementación en AudioEngine)
    volSliderA.onValueChange = [this] { audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS).setProperty(Nova::IDs::MIXER_GAIN_A, (float)volSliderA.getValue(), nullptr); };
    volSliderB.onValueChange = [this] { audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS).setProperty(Nova::IDs::MIXER_GAIN_B, (float)volSliderB.getValue(), nullptr); };

    // --- 5. OUTPUT STRIP ---
    setupKnob(outputVolume);
    setupKnob(outputGain);
    setupKnob(outputMix); // Mix Global
    addAndMakeVisible(outputFader); outputFader.setSliderStyle(juce::Slider::LinearVertical); outputFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);

    // --- 6. PRESETS ---
    addAndMakeVisible(searchBarPresets); searchBarPresets.setTextToShowWhenEmpty("Search...", juce::Colours::grey);
    addAndMakeVisible(btnSave); btnSave.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
    addAndMakeVisible(btnLoad); btnLoad.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);

    // --- 7. FOOTER ---
    addAndMakeVisible(audioProcessor.audioVisualizer);

    // Init
    audioProcessor.pluginState.addListener(this);
    setResizable(true, true);
    setSize(1920, 1080);

    // Load Values
    auto s = audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (s.isValid()) {
        volSliderA.setValue(s.getProperty(Nova::IDs::MIXER_GAIN_A, 1.0f), juce::dontSendNotification);
        volSliderB.setValue(s.getProperty(Nova::IDs::MIXER_GAIN_B, 1.0f), juce::dontSendNotification);
    }
    updateSwitcherState();
    updatePedalGui();

    // Iniciar timer stats
    statsTimer = std::make_unique<StatsTimer>(*this);
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
    audioProcessor.pluginState.removeListener(this);
    activePedalEditors.clear();
}

// --- PINTADO ---
void NOVAAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(NovaColors::Background);
    auto area = getLocalBounds();

    // HEADER
    auto headerRect = area.removeFromTop(80);
    g.setColour(NovaColors::Panel); g.fillRect(headerRect);
    g.setColour(NovaColors::Border); g.drawHorizontalLine(headerRect.getBottom(), 0, (float)getWidth());
    g.setColour(juce::Colours::white); g.setFont(30.0f);
    g.drawText("NOVA", headerRect.removeFromLeft(150), juce::Justification::centred);

    // FOOTER (Borde visualizer)
    auto footerRect = area.removeFromBottom(100);
    g.setColour(NovaColors::Border); g.drawHorizontalLine(footerRect.getY(), 0, (float)getWidth());

    // --- COLUMNAS ---
    // Browser (Left 1)
    auto left1 = area.removeFromLeft(150);
    g.setColour(NovaColors::Panel); g.fillRect(left1);
    g.setColour(NovaColors::Border); g.drawVerticalLine(left1.getRight(), (float)left1.getY(), (float)left1.getBottom());
    g.setColour(juce::Colours::white); g.setFont(14.0f);
    g.drawText("PEDALS", left1.getX(), left1.getY() + 60, left1.getWidth(), 20, juce::Justification::centred);
    g.drawText("AMPLIFIERS", left1.getX(), left1.getY() + 250, left1.getWidth(), 20, juce::Justification::centred); // CORREGIDO
    g.drawText("CABINETS", left1.getX(), left1.getY() + 400, left1.getWidth(), 20, juce::Justification::centred);

    // Input (Left 2)
    auto left2 = area.removeFromLeft(120);
    drawChannelStrip(g, left2, "INPUT");

    // Presets (Right 2)
    auto right2 = area.removeFromRight(150);
    g.setColour(NovaColors::Panel); g.fillRect(right2);
    g.setColour(NovaColors::Border); g.drawVerticalLine(right2.getX(), (float)right2.getY(), (float)right2.getBottom());
    g.setColour(juce::Colours::white); g.drawText("PRESETS", right2.getX(), right2.getY() + 60, right2.getWidth(), 20, juce::Justification::centred);

    // Output (Right 1)
    auto right1 = area.removeFromRight(120);
    drawChannelStrip(g, right1, "OUTPUT");

    // --- MIXER ---
    auto center = area;
    auto mixerArea = center.removeFromBottom(150);
    g.setColour(NovaColors::Panel);
    g.drawRoundedRectangle(mixerArea.toFloat().reduced(10), 5.0f, 1.0f);
    g.drawText("LINE A", mixerArea.getX() + 50, mixerArea.getY() + 10, 100, 20, juce::Justification::centred);
    g.drawText("LINE B", mixerArea.getRight() - 150, mixerArea.getY() + 10, 100, 20, juce::Justification::centred);

    // Etiquetas Actualizadas (Pan/Width)
    g.setFont(10.0f); g.setColour(juce::Colours::grey);
    int yLbl = mixerArea.getBottom() - 30;

    // Line A
    g.drawText("Level", mixerArea.getX() + 30, yLbl, 60, 20, juce::Justification::centred);
    g.drawText("Pan", mixerArea.getX() + 100, yLbl, 60, 20, juce::Justification::centred);
    g.drawText("Width", mixerArea.getX() + 170, yLbl, 60, 20, juce::Justification::centred);

    // Line B
    g.drawText("Level", mixerArea.getRight() - 190, yLbl, 60, 20, juce::Justification::centred);
    g.drawText("Pan", mixerArea.getRight() - 120, yLbl, 60, 20, juce::Justification::centred);
    g.drawText("Width", mixerArea.getRight() - 50, yLbl, 60, 20, juce::Justification::centred);
}

void NOVAAudioProcessorEditor::drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    g.setColour(juce::Colours::black); g.fillRect(area);
    g.setColour(NovaColors::Border);
    g.drawVerticalLine(area.getRight(), (float)area.getY(), (float)area.getBottom());
    g.drawVerticalLine(area.getX(), (float)area.getY(), (float)area.getBottom());
    g.setColour(juce::Colours::white); g.setFont(16.0f);
    g.drawText(title, area.removeFromTop(40), juce::Justification::centred);

    g.setFont(12.0f); g.setColour(juce::Colours::grey);
    g.drawText("Vol", area.getX(), area.getY() + 50, area.getWidth(), 20, juce::Justification::centred);

    if (title == "INPUT") {
        g.drawText("Gate", area.getX(), area.getY() + 110, area.getWidth(), 20, juce::Justification::centred);
        g.drawText("Trans", area.getX(), area.getY() + 170, area.getWidth(), 20, juce::Justification::centred);
    }
    else {
        g.drawText("Gain", area.getX(), area.getY() + 110, area.getWidth(), 20, juce::Justification::centred);
        g.drawText("Mix", area.getX(), area.getY() + 170, area.getWidth(), 20, juce::Justification::centred);
    }
}

void NOVAAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    if (currentOverlay) currentOverlay->setBounds(area);

    // 1. HEADER
    auto header = area.removeFromTop(80);
    int centerX = header.getCentreX();

    btnStartStop.setBounds(centerX - 60, header.getCentreY() - 10, 120, 40);
    lblStats.setBounds(centerX - 100, header.getCentreY() - 35, 200, 20); // Stats encima de Power

    btnMetronome.setBounds(centerX - 200, header.getCentreY() - 15, 30, 30);
    btnTuner.setBounds(centerX - 240, header.getCentreY() - 15, 30, 30);
    btnSettings.setBounds(centerX + 160, header.getCentreY() - 15, 40, 40);
    btnProfile.setBounds(centerX + 210, header.getCentreY() - 15, 60, 40);

    // 2. FOOTER
    auto footer = area.removeFromBottom(100);
    audioProcessor.audioVisualizer.setBounds(footer);

    // 3. COLUMNS
    // Browser
    auto left1 = area.removeFromLeft(150);
    searchBarBrowser.setBounds(left1.removeFromTop(40).reduced(10, 5));
    btnAddOverdrive.setBounds(left1.getX() + 10, left1.getY() + 50, 130, 50);
    btnAddNeural.setBounds(left1.getX() + 10, left1.getY() + 240, 130, 50);
    btnAddCabinet.setBounds(left1.getX() + 10, left1.getY() + 400, 130, 50);

    // Input Strip
    auto left2 = area.removeFromLeft(120);
    auto inputArea = left2;
    inputArea.removeFromTop(30);
    inputArea.removeFromTop(30); inputVolume.setBounds(inputArea.removeFromTop(50).reduced(30, 0));
    inputArea.removeFromTop(10); inputGate.setBounds(inputArea.removeFromTop(50).reduced(30, 0));
    inputArea.removeFromTop(10); inputTranspose.setBounds(inputArea.removeFromTop(50).reduced(30, 0));
    inputArea.removeFromTop(20); btnMonoStereo.setBounds(inputArea.removeFromTop(30).reduced(10, 0));
    inputFader.setBounds(left2.getX() + 80, left2.getY() + 300, 30, 200);

    // Presets
    auto right2 = area.removeFromRight(150);
    searchBarPresets.setBounds(right2.removeFromTop(40).reduced(10, 5));
    auto btnArea = right2.removeFromBottom(60);
    btnSave.setBounds(btnArea.removeFromLeft(75).reduced(5));
    btnLoad.setBounds(btnArea.reduced(5));

    // Output Strip
    auto right1 = area.removeFromRight(120);
    auto outputArea = right1;
    outputArea.removeFromTop(30);
    outputArea.removeFromTop(30); outputVolume.setBounds(outputArea.removeFromTop(50).reduced(30, 0));
    outputArea.removeFromTop(10); outputGain.setBounds(outputArea.removeFromTop(50).reduced(30, 0));
    outputArea.removeFromTop(10); outputMix.setBounds(outputArea.removeFromTop(50).reduced(30, 0));
    outputFader.setBounds(right1.getX() + 10, right1.getY() + 300, 30, 200);

    // 4. MIXER CENTER
    auto center = area;
    auto mixerArea = center.removeFromBottom(150);
    btnSwitcher.setBounds(mixerArea.getCentreX() - 50, mixerArea.getCentreY() - 30, 100, 60);

    // Knobs A
    int kSz = 60; int gap = 10;
    int startXA = mixerArea.getX() + 30; int yKnobs = mixerArea.getCentreY() - 10;
    volSliderA.setBounds(startXA, yKnobs, kSz, kSz);
    panSliderA.setBounds(startXA + kSz + gap, yKnobs, kSz, kSz);
    widthSliderA.setBounds(startXA + (kSz + gap) * 2, yKnobs, kSz, kSz);

    // Knobs B
    int startXB = mixerArea.getRight() - 30 - kSz;
    widthSliderB.setBounds(startXB, yKnobs, kSz, kSz);
    panSliderB.setBounds(startXB - (kSz + gap), yKnobs, kSz, kSz);
    volSliderB.setBounds(startXB - (kSz + gap) * 2, yKnobs, kSz, kSz);

    int laneH = center.getHeight() / 2;
    if (laneA) laneA->setBounds(center.removeFromTop(laneH).reduced(10));
    if (laneB) laneB->setBounds(center.reduced(10));

    updatePedalGui();
}

void NOVAAudioProcessorEditor::updateStats()
{
    // 1. Datos del Sistema
    double sampleRate = audioProcessor.getSampleRate();
    double bufferSize = (double)audioProcessor.getBlockSize();
    double cpuPercent = audioProcessor.getCpuUsage(); // Ej: 5.5%

    // 2. Cálculo de Tiempos
    double bufferDurationMs = 0.0;
    double procTimeMs = 0.0;

    if (sampleRate > 0)
    {
        // Tiempo disponible total (Fijo por el driver)
        // Ej: 512 samples / 44100 Hz = 11.6 ms
        bufferDurationMs = (bufferSize / sampleRate) * 1000.0;

        // Tiempo real de procesamiento (Fluctúa)
        // Si el CPU es 10% y el buffer es 11.6ms, tardamos 1.16ms reales
        procTimeMs = (cpuPercent / 100.0) * bufferDurationMs;
    }

    // 3. Formateo del Texto
    // Mostramos: CPU % | Tiempo Real (ms) | Buffer Fijo (ms)
    juce::String txt;
    txt << "CPU: " << juce::String(cpuPercent, 1) << "%"
        << "  |  Proc: " << juce::String(procTimeMs, 2) << "ms" // Este se moverá
        << "  |  Buf: " << juce::String(bufferDurationMs, 1) << "ms"; // Este es fijo

    lblStats.setText(txt, juce::dontSendNotification);

    // Feedback visual de advertencia si nos acercamos al límite
    if (cpuPercent > 90.0)
        lblStats.setColour(juce::Label::textColourId, juce::Colours::red);
    else
        lblStats.setColour(juce::Label::textColourId, juce::Colours::grey);
}
void NOVAAudioProcessorEditor::updatePedalGui()
{
    activePedalEditors.clear();
    auto processLane = [&](Nova::ChainID chain, ChainLane* laneComp)
        {
            if (!laneComp) return;
            laneComp->resized();
            const auto& nodes = audioProcessor.getAudioEngine().getNodes(chain);
            auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
            auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);
            int flowCounters[4] = { 0, 0, 0, 0 };

            for (int i = 0; i < nodes.size(); ++i) {
                if (i >= treeList.getNumChildren()) break;
                auto node = nodes[i];
                auto state = treeList.getChild(i);
                int zoneIdx = state.getProperty(Nova::IDs::PEDAL_ZONE);

                if (node && node->getProcessor()) {
                    if (auto* editor = node->getProcessor()->createEditor()) {
                        addAndMakeVisible(editor);
                        activePedalEditors.add(editor);
                        auto zoneRect = laneComp->getZoneRect(zoneIdx);
                        int zoneAbsX = laneComp->getX() + zoneRect.getX();
                        int zoneAbsY = laneComp->getY() + zoneRect.getY();
                        int zoneW = zoneRect.getWidth(), zoneH = zoneRect.getHeight();
                        int pW = 120, pH = 180;
                        int finalX = 0, finalY = zoneAbsY + (zoneH - pH) / 2;

                        if (zoneIdx == (int)Nova::ZoneID::Amp || zoneIdx == (int)Nova::ZoneID::Cabinet) {
                            finalX = zoneAbsX + (zoneW - pW) / 2;
                        }
                        else {
                            int gap = 15;
                            int offset = flowCounters[zoneIdx] * (pW + gap);
                            finalX = zoneAbsX + 20 + offset;
                        }
                        editor->setBounds(finalX, finalY, pW, pH);
                        // Si el overlay está activo, los pedales deben estar debajo visualmente (Z-Order)
                        // Pero addAndMakeVisible los pone encima. 
                        // Si tienes overlay, tráelo al frente.
                        if (currentOverlay) currentOverlay->toFront(true);
                        flowCounters[zoneIdx]++;
                    }
                }
            }
        };
    processLane(Nova::ChainID::LineA, laneA.get());
    processLane(Nova::ChainID::LineB, laneB.get());
}

void NOVAAudioProcessorEditor::updateSwitcherState()
{
    auto s = audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (!s.isValid()) return;
    bool on = s.getProperty(Nova::IDs::ENGINE_ON);
    int mode = s.getProperty(Nova::IDs::SWITCH_MODE);
    btnStartStop.setButtonText(on ? "POWER ON" : "POWER OFF");
    btnStartStop.setColour(juce::TextButton::buttonOnColourId, on ? juce::Colours::green : juce::Colours::red);
    btnStartStop.setToggleState(on, juce::dontSendNotification);
    juce::String txt;
    bool aActive = false, bActive = false;
    if (mode == 0) { txt = "ROUTING: LINE A"; aActive = true; }
    else if (mode == 1) { txt = "ROUTING: LINE B"; bActive = true; }
    else { txt = "ROUTING: PARALLEL"; aActive = true; bActive = true; }
    btnSwitcher.setButtonText(txt);
    if (laneA) laneA->setActive(aActive && on);
    if (laneB) laneB->setActive(bActive && on);
}

void NOVAAudioProcessorEditor::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& id)
{
    if (id == Nova::IDs::ENGINE_ON || id == Nova::IDs::SWITCH_MODE)
        updateSwitcherState();
}

bool NOVAAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey) { audioProcessor.cycleSwitcher(); return true; }
    return false;
}

void NOVAAudioProcessorEditor::showOverlay(Nova::ZoneID zone, Nova::ChainID chain)
{
    // Crear el overlay (Modal)
    auto overlay = std::make_unique<AssetBrowserOverlay>(
        zone,
        // 1. Callback de Selección
        [this, zone, chain](juce::String typeID) {
            // CORRECCIÓN C2660: Pasamos los 3 argumentos requeridos (Tipo, Cadena, Zona)
            audioProcessor.requestAddPedal(typeID, chain, zone);
        },
        // 2. Callback de Cierre (Botón X)
        [this]() {
            // Usamos callAsync para evitar el crash al borrar el botón que disparó el evento
            juce::MessageManager::callAsync([this]() {
                currentOverlay.reset();
                resized(); // Re-acomodar si es necesario
                });
        }
    );

    addAndMakeVisible(overlay.get());
    overlay->setBounds(getLocalBounds());
    currentOverlay = std::move(overlay);
}