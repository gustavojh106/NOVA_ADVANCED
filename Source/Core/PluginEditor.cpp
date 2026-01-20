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

// EN PLUGINEDITOR.CPP

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

    // ==============================================================================
    // 3. INPUT STRIP (CONEXIÓN REAL AL AUDIO ENGINE)
    // ==============================================================================

    // Vol (Input Gain) -> ID: INPUT_GAIN
    setupKnob(inputVolume, "IN GAIN", -60.0f, 24.0f, 0.0f);
    inputVolume.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::INPUT_GAIN, (float)inputVolume.getValue(), nullptr);
        };

    // Gate (Noise Gate) -> ID: INPUT_GATE
    setupKnob(inputGate, "GATE", -100.0f, 0.0f, -100.0f);
    inputGate.setTextValueSuffix(" dB");
    inputGate.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::INPUT_GATE, (float)inputGate.getValue(), nullptr);
        };

    // Transpose -> ID: INPUT_TRANS
    setupKnob(inputTranspose, "TRANS", -12.0f, 12.0f, 0.0f);
    inputTranspose.setRange(-12.0, 12.0, 1.0); // Pasos enteros
    inputTranspose.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::INPUT_TRANS, (int)inputTranspose.getValue(), nullptr);
        };

    // Mono/Stereo -> ID: FORCE_MONO
    addAndMakeVisible(btnMonoStereo);
    btnMonoStereo.setButtonText("MONO");
    btnMonoStereo.setClickingTogglesState(true);
    btnMonoStereo.setColour(juce::ToggleButton::tickColourId, NovaColors::Accent);
    btnMonoStereo.onClick = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::FORCE_MONO, btnMonoStereo.getToggleState(), nullptr);
        };

    // Fader Visual (Por ahora decorativo o linkeado al gain)
    addAndMakeVisible(inputFader);
    inputFader.setSliderStyle(juce::Slider::LinearVertical);
    inputFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    inputFader.setRange(-60.0, 6.0, 0.1);
    inputFader.setValue(0.0, juce::dontSendNotification);

    // ==============================================================================
    // 4. MIXER & LANES (CONEXIÓN REAL)
    // ==============================================================================


    // --- 5. OUTPUT STRIP ---

    // OUTPUT VOL (Master Fader)
    // Rango: -60dB a +12dB. Default: 0dB
    setupKnob(outputVolume, "MASTER", -60.0f, 12.0f, 0.0f);
    outputVolume.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::OUTPUT_VOL, (float)outputVolume.getValue(), nullptr);
        };

    // LIMITER (Ceiling/Threshold)
    // Rango: -20dB a 0dB. Default: 0dB (Limitador apagado/transparente)
    setupKnob(outputGain, "LIMIT", -20.0f, 0.0f, 0.0f);
    outputGain.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::OUTPUT_LIMITER, (float)outputGain.getValue(), nullptr);
        };

    // GLOBAL MIX (Dry/Wet global) - Por ahora placeholder visual o conectado si implementamos mix
    setupKnob(outputMix, "MIX", 0.0f, 100.0f, 100.0f);
    // outputMix.onValueChange = ... 

    addAndMakeVisible(outputFader);
    outputFader.setSliderStyle(juce::Slider::LinearVertical);
    outputFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    // Opcional: Que el fader controle también el volumen maestro
    outputFader.setRange(-60.0, 12.0, 0.1);
    outputFader.setValue(0.0, juce::dontSendNotification);
    outputFader.onValueChange = [this] {
        outputVolume.setValue(outputFader.getValue(), juce::sendNotification);
        };


    laneA = std::make_unique<ChainLane>(p, Nova::ChainID::LineA); addAndMakeVisible(laneA.get());
    laneB = std::make_unique<ChainLane>(p, Nova::ChainID::LineB); addAndMakeVisible(laneB.get());
    addAndMakeVisible(btnSwitcher);
    btnSwitcher.onClick = [this] { audioProcessor.cycleSwitcher(); };

    // --- LINE A ---
    // Level
    setupKnob(volSliderA, "LEVEL A", 0.0f, 2.0f, 1.0f);
    volSliderA.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_A)
            .setProperty(Nova::IDs::MIXER_GAIN_A, (float)volSliderA.getValue(), nullptr);
        };

    // Pan (-1 L a +1 R)
    setupKnob(panSliderA, "PAN A", -1.0f, 1.0f, 0.0f);
    panSliderA.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_A)
            .setProperty(Nova::IDs::MIXER_PAN_A, (float)panSliderA.getValue(), nullptr);
        };

    // Width (0 Mono a 2 Wide)
    setupKnob(widthSliderA, "WIDTH A", 0.0f, 2.0f, 1.0f);
    widthSliderA.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_A)
            .setProperty(Nova::IDs::MIXER_WIDTH_A, (float)widthSliderA.getValue(), nullptr);
        };

    // --- LINE B ---
    // Level
    setupKnob(volSliderB, "LEVEL B", 0.0f, 2.0f, 1.0f);
    volSliderB.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_B)
            .setProperty(Nova::IDs::MIXER_GAIN_B, (float)volSliderB.getValue(), nullptr);
        };

    // Pan
    setupKnob(panSliderB, "PAN B", -1.0f, 1.0f, 0.0f);
    panSliderB.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_B)
            .setProperty(Nova::IDs::MIXER_PAN_B, (float)panSliderB.getValue(), nullptr);
        };

    // Width
    setupKnob(widthSliderB, "WIDTH B", 0.0f, 2.0f, 1.0f);
    widthSliderB.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_B)
            .setProperty(Nova::IDs::MIXER_WIDTH_B, (float)widthSliderB.getValue(), nullptr);
        };

    // --- 5. OUTPUT STRIP ---
    setupKnob(outputVolume, "OUT VOL", -60.0f, 12.0f, 0.0f);
    outputVolume.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::OUTPUT_VOL, (float)outputVolume.getValue(), nullptr); // <--- Conexión
        };

    setupKnob(outputGain, "LIMIT", -20.0f, 0.0f, 0.0f); // Rango típico de limitador (Thresh negativo)
    outputGain.onValueChange = [this] {
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::OUTPUT_LIMITER, (float)outputGain.getValue(), nullptr);
        };
    setupKnob(outputMix, "MIX", 0.0f, 100.0f, 100.0f);

    outputMix.onValueChange = [this] {
        // Guardamos el valor en el ValueTree
        audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
            .setProperty(Nova::IDs::OUTPUT_MIX, (float)outputMix.getValue(), nullptr);
        };



    addAndMakeVisible(outputFader);
    outputFader.setSliderStyle(juce::Slider::LinearVertical);
    outputFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);

    // --- 6. PRESETS & FOOTER ---
    addAndMakeVisible(searchBarPresets); searchBarPresets.setTextToShowWhenEmpty("Search...", juce::Colours::grey);
    addAndMakeVisible(btnSave); btnSave.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
    addAndMakeVisible(btnLoad); btnLoad.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
    addAndMakeVisible(audioProcessor.audioVisualizer);

    // Init
    audioProcessor.pluginState.addListener(this);
    setResizable(true, true);
    setSize(1920, 1080);

    // ==============================================================================
    // CARGAR VALORES INICIALES (Sync UI <-> Data)
    // ==============================================================================
    auto settings = audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS);
    auto lA = audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_A);
    auto lB = audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_B);

    if (settings.isValid()) {
        inputVolume.setValue(settings.getProperty(Nova::IDs::INPUT_GAIN, 0.0f), juce::dontSendNotification);
        inputGate.setValue(settings.getProperty(Nova::IDs::INPUT_GATE, -100.0f), juce::dontSendNotification);
        inputTranspose.setValue(settings.getProperty(Nova::IDs::INPUT_TRANS, 0), juce::dontSendNotification);
        btnMonoStereo.setToggleState(settings.getProperty(Nova::IDs::FORCE_MONO, false), juce::dontSendNotification);
    }

    if (lA.isValid()) {
        volSliderA.setValue(lA.getProperty(Nova::IDs::MIXER_GAIN_A, 1.0f), juce::dontSendNotification);
        panSliderA.setValue(lA.getProperty(Nova::IDs::MIXER_PAN_A, 0.0f), juce::dontSendNotification);
        widthSliderA.setValue(lA.getProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f), juce::dontSendNotification);
    }

    if (lB.isValid()) {
        volSliderB.setValue(lB.getProperty(Nova::IDs::MIXER_GAIN_B, 1.0f), juce::dontSendNotification);
        panSliderB.setValue(lB.getProperty(Nova::IDs::MIXER_PAN_B, 0.0f), juce::dontSendNotification);
        widthSliderB.setValue(lB.getProperty(Nova::IDs::MIXER_WIDTH_B, 1.0f), juce::dontSendNotification);
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
        // --- AQUÍ ESTABA EL ERROR VISUAL ---
        g.drawText("Limit", area.getX(), area.getY() + 110, area.getWidth(), 20, juce::Justification::centred); // Antes decía "Gain"
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
    btnTuner.onClick = [this] { toggleTuner(); };
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
void NOVAAudioProcessorEditor::toggleTuner()
{
    bool currentState = audioProcessor.getAudioEngine().isTunerEnabled();
    bool newState = !currentState;

    audioProcessor.getAudioEngine().setTunerEnabled(newState);

    // UI: Cambiar color del botón T
    btnTuner.setColour(juce::TextButton::buttonColourId, newState ? juce::Colours::green : juce::Colours::transparentBlack);

    if (newState)
    {
        tunerDisplay = std::make_unique<TunerDisplay>(audioProcessor);
        addAndMakeVisible(tunerDisplay.get());
        tunerDisplay->setBounds(getLocalBounds()); // Cubrir toda la pantalla
        tunerDisplay->toFront(true);
    }
    else
    {
        tunerDisplay.reset();
    }
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
    // 1. Preparamos un set para rastrear qué nodos están vivos en este ciclo.
    // Los que no estén en este set al final, serán eliminados (Garbage Collection).
    std::set<juce::AudioProcessorGraph::NodeID> requiredNodeIDs;

    auto processLane = [&](Nova::ChainID chain, ChainLane* laneComp)
        {
            if (!laneComp) return;
            laneComp->resized(); // Asegurar que las DropZones tienen el tamaño correcto

            const auto& nodes = audioProcessor.getAudioEngine().getNodes(chain);
            auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
            auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);
            int flowCounters[4] = { 0, 0, 0, 0 }; // Contadores para apilar pedales en cada zona

            for (int i = 0; i < nodes.size(); ++i)
            {
                // Protección de seguridad
                if (i >= treeList.getNumChildren()) break;

                auto node = nodes[i];

                // Si el nodo es válido y tiene procesador
                if (node && node->getProcessor())
                {
                    // -- A. REGISTRO --
                    // Marcamos este ID como "Necesario" para que no sea borrado
                    requiredNodeIDs.insert(node->nodeID);

                    // -- B. DATOS DE POSICIÓN --
                    auto state = treeList.getChild(i);
                    int zoneIdx = state.getProperty(Nova::IDs::PEDAL_ZONE);

                    auto zoneRect = laneComp->getZoneRect(zoneIdx);
                    int zoneAbsX = laneComp->getX() + zoneRect.getX();
                    int zoneAbsY = laneComp->getY() + zoneRect.getY();
                    int zoneW = zoneRect.getWidth();
                    int zoneH = zoneRect.getHeight();
                    int pW = 120, pH = 180; // Tamaño fijo del pedal
                    int finalX = 0;
                    int finalY = zoneAbsY + (zoneH - pH) / 2; // Centrado verticalmente

                    if (zoneIdx == (int)Nova::ZoneID::Amp || zoneIdx == (int)Nova::ZoneID::Cabinet) {
                        finalX = zoneAbsX + (zoneW - pW) / 2; // Centrado horizontal (Slot único)
                    }
                    else {
                        int gap = 15;
                        int offset = flowCounters[zoneIdx] * (pW + gap);
                        finalX = zoneAbsX + 20 + offset; // Flujo izquierda->derecha
                    }

                    // -- C. GESTIÓN INTELIGENTE DE EDITORES (Smart Update) --
                    juce::AudioProcessorEditor* editor = nullptr;

                    // Buscamos si ya existe el editor en nuestro mapa
                    auto it = activePedalEditors.find(node->nodeID);

                    if (it == activePedalEditors.end())
                    {
                        // CASO 1: NO EXISTE -> CREAR NUEVO
                        if (auto* newEditor = node->getProcessor()->createEditor())
                        {
                            addAndMakeVisible(newEditor);
                            // Lo guardamos en el mapa (el unique_ptr toma posesión)
                            activePedalEditors[node->nodeID].reset(newEditor);
                            editor = newEditor;
                        }
                    }
                    else
                    {
                        // CASO 2: YA EXISTE -> REUTILIZAR
                        editor = it->second.get();
                    }

                    // -- D. ACTUALIZAR POSICIÓN --
                    if (editor)
                    {
                        editor->setBounds(finalX, finalY, pW, pH);
                        // Traer al frente para que no quede detrás de otros elementos, 
                        // pero 'false' evita robar el foco del teclado innecesariamente.
                        editor->toFront(false);
                    }

                    flowCounters[zoneIdx]++;
                }
            }
        };

    // Procesar ambas cadenas
    processLane(Nova::ChainID::LineA, laneA.get());
    processLane(Nova::ChainID::LineB, laneB.get());

    // 2. GARBAGE COLLECTION (Limpieza)
    // Borramos los editores que están en el mapa pero YA NO están en la cadena
    for (auto it = activePedalEditors.begin(); it != activePedalEditors.end(); )
    {
        if (requiredNodeIDs.find(it->first) == requiredNodeIDs.end())
        {
            // El nodo ya no existe en el grafo -> Borramos su editor
            it = activePedalEditors.erase(it); // erase devuelve el siguiente iterador válido
        }
        else
        {
            ++it;
        }
    }

    // 3. MANTENER OVERLAY
    // Si hay un menú de selección abierto, asegurarse de que siga encima de todo
    if (currentOverlay) currentOverlay->toFront(true);
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
float TunerDisplay::getMedianCents(float newVal)
{
    medianBuffer.push_back(newVal);
    if (medianBuffer.size() > MEDIAN_SIZE) medianBuffer.pop_front();

    // Hacemos una copia para ordenar
    std::vector<float> temp(medianBuffer.begin(), medianBuffer.end());
    std::sort(temp.begin(), temp.end());

    // Retornamos el valor central
    if (temp.empty()) return 0.0f;
    return temp[temp.size() / 2];
}
TunerDisplay::TunerDisplay(NOVAAudioProcessor& p) : processor(p)
{
    initPresets();

    // Inicializar historial de suavizado con ceros
    centsHistory.resize(SMOOTHING_BUFFER_SIZE, 0.0f);

    addAndMakeVisible(closeButton);
    closeButton.setButtonText("X");
    closeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    closeButton.onClick = [this] { if (auto* parent = findParentComponentOfClass<NOVAAudioProcessorEditor>()) parent->toggleTuner(); };

    addAndMakeVisible(tuningSelector);
    tuningSelector.setJustificationType(juce::Justification::centred);
    tuningSelector.setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromString("ff202020"));
    tuningSelector.setColour(juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha(0.2f));

    for (int i = 0; i < presets.size(); ++i)
        tuningSelector.addItem(presets[i].name, i + 1);

    tuningSelector.setSelectedId(1);
    tuningSelector.onChange = [this] {
        currentPresetIndex = tuningSelector.getSelectedId() - 1;
        selectString(0);
        repaint();
        };

    // Botón de reinicio (aparece al final)
    addAndMakeVisible(resetButton);
    resetButton.setButtonText("TUNE AGAIN");
    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    resetButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    resetButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    resetButton.onClick = [this] {
        // 1. Reiniciar memoria de cuerdas afinadas (Crucial)
        std::fill(stringIsTuned.begin(), stringIsTuned.end(), false);

        // 2. Ocultar botones de éxito explícitamente
        startPlayingButton.setVisible(false);
        resetButton.setVisible(false);

        // 3. Volver a la primera cuerda y resetear variables
        selectString(0);
        };
    resetButton.setVisible(false);
    addAndMakeVisible(startPlayingButton);
    startPlayingButton.setButtonText("START PLAYING");
    startPlayingButton.setColour(juce::TextButton::buttonColourId, juce::Colours::green);
    startPlayingButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    startPlayingButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    startPlayingButton.onClick = [this] {
        // Cerrar afinador
        if (auto* parent = findParentComponentOfClass<NOVAAudioProcessorEditor>())
            parent->toggleTuner();
        };
    startPlayingButton.setVisible(false);

    stringIsTuned.resize(8, false); // Aumentamos capacidad por seguridad (máx 8 cuerdas)
    startTimerHz(60);
}

TunerDisplay::~TunerDisplay() {}

void TunerDisplay::initPresets()
{
    // 6-String
    presets.push_back({ "Standard (E)", {"E", "A", "D", "G", "B", "e"}, {82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f} });
    presets.push_back({ "Drop D", {"D", "A", "D", "G", "B", "e"}, {73.42f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f} });
    presets.push_back({ "Eb Standard", {"Eb", "Ab", "Db", "Gb", "Bb", "eb"}, {77.78f, 103.83f, 138.59f, 185.00f, 233.08f, 311.13f} });
    presets.push_back({ "Drop C", {"C", "G", "C", "F", "A", "D"}, {65.41f, 98.00f, 130.81f, 174.61f, 220.00f, 293.66f} });

    // --- NUEVO: RANGO EXTENDIDO ---

    // 7-String Standard (B Low)
    // Notas: B1, E2, A2, D3, G3, B3, E4
    presets.push_back({ "7-String Std (B)",
        {"B", "E", "A", "D", "G", "B", "e"},
        {61.74f, 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f}
        });

    // 7-String Drop A
    // Notas: A1, E2, A2, D3, G3, B3, E4
    presets.push_back({ "7-String Drop A",
        {"A", "E", "A", "D", "G", "B", "e"},
        {55.00f, 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f}
        });

    // 8-String Standard (F# Low)
    // Notas: F#1, B1, E2, A2, D3, G3, B3, E4
    presets.push_back({ "8-String Std (F#)",
        {"F#", "B", "E", "A", "D", "G", "B", "e"},
        {46.25f, 61.74f, 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f}
        });
}

void TunerDisplay::selectString(int index)
{
    // Validación de rango
    if (index < 0 || index >= presets[currentPresetIndex].frequencies.size()) return;

    currentStringIndex = index;
    isTuningComplete = false;

    // --- RESET COMPLETO DE VARIABLES ---
    tuningProgress = 0.0f;       // <--- ESTO ARREGLA EL BUG DEL "DOBLE CLICK"
    framesInTune = 0;            // (Por si acaso quedó lógica vieja)

    // UI
    tuningSelector.setVisible(true);

    // Limpieza de buffers DSP
    std::fill(centsHistory.begin(), centsHistory.end(), 0.0f);
    medianBuffer.clear();
}

// ==============================================================================
// 2. LÓGICA MATEMÁTICA MEJORADA
// ==============================================================================

// Función Helper para el Promedio Móvil (Esto soluciona el "Jitter")
float TunerDisplay::getSmoothedCents(float newCents)
{
    // Guardamos el nuevo valor en el buffer circular
    centsHistory[historyIndex] = newCents;
    historyIndex = (historyIndex + 1) % SMOOTHING_BUFFER_SIZE;

    // Calculamos el promedio
    float sum = 0.0f;
    for (float val : centsHistory) sum += val;
    return sum / (float)SMOOTHING_BUFFER_SIZE;
}

// EN PLUGINEDITOR.CPP - Reemplaza timerCallback completo
// EN PLUGINEDITOR.CPP - Reemplaza timerCallback

void TunerDisplay::timerCallback()
{
    if (isTuningComplete) {
        repaint();
        return;
    }
    auto& engine = processor.getAudioEngine();
    float detectedFreq = engine.getTunerPitch();
    float clarity = engine.getTunerClarity();
    float rms = engine.getTunerRMS();

    float targetFreq = presets[currentPresetIndex].frequencies[currentStringIndex];
    float rawCentsError = 0.0f;
    bool signalIsValid = false;

    // Gate Dinámico (Igual que antes)
    float dynamicThreshold = (clarity > 0.95f) ? 0.0001f : 0.002f;

    if (detectedFreq > 20.0f && rms > dynamicThreshold && clarity > 0.85f)
    {
        // Octave Folding (Igual que antes)
        float tempFreq = detectedFreq;
        while (tempFreq > targetFreq * 1.55f) tempFreq *= 0.5f;
        while (tempFreq < targetFreq * 0.74f) tempFreq *= 2.0f;

        rawCentsError = 1200.0f * std::log2(tempFreq / targetFreq);

        // Clamp
        if (rawCentsError > 50.0f) rawCentsError = 50.0f;
        if (rawCentsError < -50.0f) rawCentsError = -50.0f;

        signalIsValid = true;
    }

    // 1. APLICAR MEDIANA (Estabilidad)
    float filteredCents = getMedianCents(signalIsValid ? rawCentsError : 0.0f);

    // ------------------------------------------------------------
    // ESTRATEGIA DE PERMISIVIDAD (SOTA UX)
    // ------------------------------------------------------------

    // UMBRAL VERDE (TOLERANCIA):
    // Aumentamos a 6.0 cents. Esto es lo suficientemente preciso para el oído humano
    // pero mucho más fácil de "atinar" con la mano.
    const float TOLERANCE = 6.0f;
    const float PERFECT_TOLERANCE = 2.0f;

    bool isInGreenZone = std::abs(filteredCents) <= TOLERANCE && signalIsValid;
    bool isPerfect = std::abs(filteredCents) <= PERFECT_TOLERANCE && signalIsValid;

    if (isInGreenZone)
    {
        // VELOCIDAD DE LLENADO VARIABLE
        // Si estás "perfecto", llena turbo-rápido (recompensa).
        // Si estás en el borde verde, llena normal.
        float fillSpeed = isPerfect ? 1.5f : 0.8f;

        // Bonus por claridad (cuerda nueva/buen ataque)
        if (clarity > 0.98f) fillSpeed += 0.3f;

        tuningProgress += fillSpeed;
    }
    else
    {
        // LÓGICA DE CASTIGO "PERMISIVA"
        if (signalIsValid)
        {
            // Si te saliste del rango verde...
            if (std::abs(filteredCents) < 15.0f)
            {
                // ZONA DE PERDÓN (Grace Zone): Entre 6 y 15 cents.
                // Aquí NO bajamos la barra. La PAUSAMOS.
                // Esto permite al usuario hacer micro-ajustes sin perder su progreso.
            }
            else
            {
                // Si te fuiste muy lejos (>15 cents), entonces sí bajamos rápido.
                tuningProgress -= 2.0f;
            }
        }
        else
        {
            // Silencio (Leaky Bucket Lento)
            // Baja muy despacito para dar tiempo a volver a tocar
            tuningProgress -= 0.15f;
        }
    }

    // Clamps del progreso
    if (tuningProgress < 0.0f) tuningProgress = 0.0f;

    // CHECK DE FINALIZACIÓN (Igual que antes)
   // CHECK DE FINALIZACIÓN
    if (tuningProgress >= 100.0f)
    {
        tuningProgress = 100.0f;

        // 1. Marcar cuerda actual como LISTA
        stringIsTuned[currentStringIndex] = true;

        // 2. VERIFICAR SI TODAS LAS CUERDAS *DEL PRESET ACTUAL* ESTÁN LISTAS
        bool allStringsDone = true;
        int numStringsInPreset = presets[currentPresetIndex].stringNames.size(); // <--- CORRECCIÓN CLAVE

        // Solo iteramos hasta el número de cuerdas que tiene la guitarra actual
        for (int i = 0; i < numStringsInPreset; ++i)
        {
            if (!stringIsTuned[i]) {
                allStringsDone = false;
                break;
            }
        }

        if (allStringsDone)
        {
            // ¡ÉXITO!
            isTuningComplete = true;

            // Mostrar botones de éxito
            resetButton.setVisible(true);
            startPlayingButton.setVisible(true);

            // Ocultar selector
            tuningSelector.setVisible(false);
        }
        else
        {
            // Auto-Avance Inteligente (Solo dentro de las cuerdas válidas)
            int nextCandidate = -1;
            for (int i = 0; i < numStringsInPreset; ++i) {
                // Algoritmo circular para buscar la siguiente false
                int idx = (currentStringIndex + 1 + i) % numStringsInPreset;
                if (!stringIsTuned[idx]) {
                    nextCandidate = idx;
                    break;
                }
            }

            if (nextCandidate != -1) {
                currentStringIndex = nextCandidate;
                tuningProgress = 0.0f;
                medianBuffer.clear();
            }
        }
    }

    // Physics Visuales
    currentDisplayCents += (filteredCents - currentDisplayCents) * 0.2f; // Un poco más rápido para respuesta visual
    smoothedRMS += (rms - smoothedRMS) * 0.1f;

    repaint();
}
// ==============================================================================
// 3. PINTADO
// ==============================================================================
void TunerDisplay::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.95f));

    auto bounds = getLocalBounds();
    auto center = bounds.getCentre();

    // --- CASO: AFINACIÓN COMPLETA (FEEDBACK) ---
    if (isTuningComplete)
    {
        if (resetButton.isVisible())
        {
            g.setColour(juce::Colours::white);
            g.drawRect(resetButton.getBounds().expanded(2), 2.0f); // Borde externo elegante
        }
        g.setColour(juce::Colours::green);
        g.setFont(juce::Font(60.0f, juce::Font::bold));
        g.drawText("GUITAR TUNED!", bounds.removeFromTop(bounds.getHeight() / 2), juce::Justification::centred);

        g.setColour(juce::Colours::white);
        g.setFont(20.0f);
        g.drawText("Ready.", bounds.removeFromTop(100), juce::Justification::centred);

        // Dibujar un check gigante
        juce::Path p;
        p.startNewSubPath(center.getX() - 40, center.getY());
        p.lineTo(center.getX() - 10, center.getY() + 30);
        p.lineTo(center.getX() + 50, center.getY() - 40);

        g.setColour(juce::Colours::green);
        g.strokePath(p, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }

    // --- MODO NORMAL ---

    // 1. Barra de Cuerdas (Indicador de Progreso)
    int numStrings = presets[currentPresetIndex].stringNames.size();
    int btnWidth = stringBarArea.getWidth() / numStrings;

    for (int i = 0; i < numStrings; ++i)
    {
        auto rect = juce::Rectangle<int>(stringBarArea.getX() + i * btnWidth, stringBarArea.getY(), btnWidth, stringBarArea.getHeight()).reduced(5);
        bool isSelected = (i == currentStringIndex);
        bool isTuned = stringIsTuned[i];

        // Progreso Visual (Barra de carga en el botón si está seleccionada)
        if (isSelected && tuningProgress > 0.0f)
        {
            // ANTES: / 60.0f  -> AHORA: / 120.0f (para reflejar los 2 segundos)
            float progress = tuningProgress / 100.0f;
            if (progress > 1.0f) progress = 1.0f;

            g.setColour(juce::Colours::green.withAlpha(0.3f));
            g.fillRoundedRectangle(rect.getX(), rect.getY(), rect.getWidth() * progress, rect.getHeight(), 4.0f);
        }

        // Borde y Texto
        g.setColour(isTuned ? juce::Colours::green : (isSelected ? juce::Colours::white : juce::Colours::grey.withAlpha(0.5f)));
        g.drawRoundedRectangle(rect.toFloat(), 4.0f, isSelected ? 2.0f : 1.0f);

        g.setFont(juce::Font(22.0f, isSelected ? juce::Font::bold : juce::Font::plain));
        g.drawText(presets[currentPresetIndex].stringNames[i], rect, juce::Justification::centred);

        // Icono Lock si está afinada
        if (isTuned) {
            g.fillEllipse(rect.getRight() - 10, rect.getY() + 5, 5, 5);
        }
    }

    // 2. Medidor Central
    bool hasSignal = (smoothedRMS > 0.002f);

    // Color dinámico SINCROZINADO con la lógica matemática
    juce::Colour statusColor = juce::Colour::fromString("ffea2e2e"); // Rojo

    // Zona Naranja (Cerca)
    if (std::abs(currentDisplayCents) < 15.0f) statusColor = juce::Colours::orange;

    // Zona Verde (Debe coincidir con TOLERANCE = 6.0f de timerCallback)
    if (std::abs(currentDisplayCents) <= 6.0f) statusColor = juce::Colours::green;

    if (!hasSignal) statusColor = juce::Colours::darkgrey;

    // Nota Grande
    g.setColour(statusColor);
    g.setFont(juce::Font(100.0f, juce::Font::bold));
    g.drawText(presets[currentPresetIndex].stringNames[currentStringIndex], bounds.removeFromTop(bounds.getHeight() / 2), juce::Justification::centredBottom);

    // Barra Gauge
    int barW = 500; int barY = center.getY() + 50; int barX = center.getX() - (barW / 2);

    g.setColour(juce::Colours::white.withAlpha(0.1f));
    g.fillRoundedRectangle((float)barX, (float)barY, (float)barW, 8.0f, 4.0f);
    g.setColour(juce::Colours::white);
    g.drawVerticalLine(center.getX(), (float)barY - 15, (float)barY + 23);

    if (hasSignal)
    {
        float pxPerCent = (float)barW / 100.0f;
        float needleX = center.getX() + (currentDisplayCents * pxPerCent);
        needleX = juce::jlimit((float)barX, (float)(barX + barW), needleX);

        g.setColour(statusColor);
        g.fillEllipse(needleX - 10, barY - 6, 20, 20);
        // Glow que pulsa si está cerca
        float glowSize = 10.0f;
        if (std::abs(currentDisplayCents) < 5.0f) glowSize = 15.0f + (std::sin(juce::Time::getMillisecondCounter() / 100.0f) * 2.0f);

        g.setColour(statusColor.withAlpha(0.4f));
        g.fillEllipse(needleX - glowSize, barY + 4 - glowSize, glowSize * 2, glowSize * 2);
    }
}

void TunerDisplay::resized()
{
    auto area = getLocalBounds();
    closeButton.setBounds(area.getRight() - 50, area.getY() + 10, 40, 40);

    // Selector (Solo visible durante afinación)
    tuningSelector.setBounds(area.getCentreX() - 100, area.getBottom() - 50, 200, 30);

    // Área de cuerdas
    stringBarArea = juce::Rectangle<int>(area.getX() + 50, area.getBottom() - 120, area.getWidth() - 100, 50);

    // --- LAYOUT PANTALLA DE ÉXITO ---
    int btnW = 160;
    int btnH = 40;
    int gap = 20;
    int startY = area.getBottom() - 100;
    int centerX = area.getCentreX();

    // Start Playing (Verde) a la derecha (acción principal)
    startPlayingButton.setBounds(centerX + gap / 2, startY, btnW, btnH);

    // Tune Again (Negro) a la izquierda (acción secundaria)
    resetButton.setBounds(centerX - btnW - gap / 2, startY, btnW, btnH);
}

void TunerDisplay::mouseUp(const juce::MouseEvent& e)
{
    if (stringBarArea.contains(e.getPosition()) && !isTuningComplete)
    {
        int numStrings = presets[currentPresetIndex].stringNames.size();
        int btnWidth = stringBarArea.getWidth() / numStrings;
        int index = (e.x - stringBarArea.getX()) / btnWidth;
        selectString(index);
        repaint();
    }
}

// EN PLUGINEDITOR.CPP (Al final del archivo o antes de resized)

void NOVAAudioProcessorEditor::setupKnob(juce::Slider& slider, const juce::String& name, float min, float max, float def)
{
    addAndMakeVisible(slider);
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setRange(min, max, 0.01);
    slider.setValue(def, juce::dontSendNotification);

    // Tooltip simple
    slider.setTooltip(name);

    // (Opcional) Si quieres que tengan LookAndFeel personalizado en el futuro:
    // slider.setLookAndFeel(&myCustomLookAndFeel);
}