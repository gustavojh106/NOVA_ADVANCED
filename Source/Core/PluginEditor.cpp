#include "PluginProcessor.h"
#include "PluginEditor.h"

// ==============================================================================
// 1. ESTILO Y COLORES
// ==============================================================================
namespace NovaColors
{
    const juce::Colour Background = juce::Colour::fromString("ff111111");
    const juce::Colour MixerPanel = juce::Colour::fromString("ff1e1e1e");
    const juce::Colour ZoneOutline = juce::Colour::fromString("ff444444");
    const juce::Colour CableOff = juce::Colour::fromString("ff333333");
    const juce::Colour CableOnA = juce::Colour::fromString("ff00aaff");
    const juce::Colour CableOnB = juce::Colour::fromString("ffffaa00");
}

// ==============================================================================
// 2. CLASES INTERNAS (ASSETS, OVERLAY, DROPZONE)
// ==============================================================================

// --- AssetItem: Representación visual de un Ampli/Cab en el menú ---
class AssetItem : public juce::Component
{
public:
    AssetItem(const juce::String& name, const juce::String& type, std::function<void()> onSelect)
        : itemName(name), itemType(type), onSelectCallback(onSelect)
    {
        // CORRECCIÓN: Usamos setMouseCursor en lugar de setCursor
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (onSelectCallback) onSelectCallback();
    }

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

        // Dibujo esquemático
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
// --- AssetBrowserOverlay: El Modal con Buscador ---
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

    void textEditorTextChanged(juce::TextEditor& editor) override
    {
        populateList(editor.getText());
    }

private:
    void populateList(const juce::String& filter)
    {
        container->removeAllChildren();
        items.clear();

        std::vector<juce::String> mockData;
        if (targetZone == Nova::ZoneID::Amp) {
            mockData = { "British Lead 800", "USA Rectifier", "Jazz Clean 120", "German Fireball", "Blues Junior", "Bass SuperTube" };
        }
        else {
            mockData = { "4x12 Vintage 30", "2x12 Greenback", "1x12 Blue Alnico", "8x10 Bass Fridge", "4x12 Recto Std", "2x10 Tremolo" };
        }

        for (const auto& name : mockData)
        {
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
        int itemSize = 140;
        int gap = 20;
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

// --- DropZone: Zona magnética o slot fijo ---
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
        {
            if (auto* mainEditor = findParentComponentOfClass<NOVAAudioProcessorEditor>())
                mainEditor->showOverlay(zone, chain);
        }
    }

    void paint(juce::Graphics& g) override
    {
        bool isFixedSlot = (zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet);
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
            if (isHover) { g.setColour(juce::Colours::cyan.withAlpha(0.2f)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.f); }
        }
    }

private:
    NOVAAudioProcessor& proc;
    Nova::ChainID chain;
    Nova::ZoneID zone;
    bool isHover = false;
};

// --- ChainLane: El Carril ---
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
        int totalW = area.getWidth();
        int h = area.getHeight();
        int fixedZoneW = 240;
        int remainingW = totalW - (fixedZoneW * 2);
        int flexZoneW = remainingW / 2;

        zones[0]->setBounds(0, 0, flexZoneW, h);
        zones[1]->setBounds(flexZoneW, 0, fixedZoneW, h);
        zones[2]->setBounds(flexZoneW + fixedZoneW, 0, flexZoneW, h);
        zones[3]->setBounds(totalW - fixedZoneW, 0, fixedZoneW, h);
    }

    juce::Rectangle<int> getZoneRect(int zoneIndex)
    {
        if (zoneIndex >= 0 && zoneIndex < zones.size()) return zones[zoneIndex]->getBounds();
        return {};
    }

    void paint(juce::Graphics& g) override
    {
        float y = (float)getHeight() / 2.0f;
        float w = (float)getWidth();
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
    addAndMakeVisible(audioProcessor.audioVisualizer);

    laneA = std::make_unique<ChainLane>(p, Nova::ChainID::LineA);
    addAndMakeVisible(laneA.get());
    laneB = std::make_unique<ChainLane>(p, Nova::ChainID::LineB);
    addAndMakeVisible(laneB.get());

    addAndMakeVisible(btnStartStop);
    btnStartStop.setClickingTogglesState(true);
    btnStartStop.onClick = [this] { audioProcessor.toggleEngine(); };

    addAndMakeVisible(btnSwitcher);
    btnSwitcher.onClick = [this] { audioProcessor.cycleSwitcher(); };

    auto setupSlider = [&](juce::Slider& s, juce::Label& l, const juce::String& txt, const juce::Identifier& id) {
        addAndMakeVisible(s);
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRange(0.0, 1.2, 0.01);
        s.onValueChange = [this, &s, id] {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS).setProperty(id, (float)s.getValue(), nullptr);
            };
        addAndMakeVisible(l);
        l.setText(txt, juce::dontSendNotification);
        l.setJustificationType(juce::Justification::centred);
        l.setFont(12.0f);
        l.setColour(juce::Label::textColourId, juce::Colours::grey);
        };

    setupSlider(volSliderA, volLabelA, "GAIN A", Nova::IDs::MIXER_GAIN_A);
    setupSlider(volSliderB, volLabelB, "GAIN B", Nova::IDs::MIXER_GAIN_B);

    addAndMakeVisible(btnAddOverdrive);
    addAndMakeVisible(btnAddCabinet);

    audioProcessor.pluginState.addListener(this);
    setResizable(true, true);
    setSize(1920, 1080);

    auto s = audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (s.isValid()) {
        volSliderA.setValue(s.getProperty(Nova::IDs::MIXER_GAIN_A, 1.0f), juce::dontSendNotification);
        volSliderB.setValue(s.getProperty(Nova::IDs::MIXER_GAIN_B, 1.0f), juce::dontSendNotification);
    }
    updateSwitcherState();
    updatePedalGui();
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
    audioProcessor.pluginState.removeListener(this);
    activePedalEditors.clear();
}

void NOVAAudioProcessorEditor::showOverlay(Nova::ZoneID zone, Nova::ChainID chain)
{
    // Crear el overlay
    auto overlay = std::make_unique<AssetBrowserOverlay>(
        zone,
        // 1. Callback de Selección (Item click)
        [this, zone, chain](juce::String typeID) {
            audioProcessor.requestAddPedal(typeID, chain, zone);
        },
        // 2. Callback de Cierre (X button o al seleccionar)
        [this]() {
            // CRASH FIX:
            // No podemos hacer currentOverlay.reset() aquí directamente,
            // porque destruiríamos el botón que disparó este evento MIENTRAS
            // todavía se está ejecutando el clic.

            // Usamos callAsync para diferir la destrucción al siguiente ciclo del loop,
            // cuando el botón ya haya terminado su proceso.
            juce::MessageManager::callAsync([this]() {
                currentOverlay.reset();
                resized();
                });
        }
    );

    addAndMakeVisible(overlay.get());
    overlay->setBounds(getLocalBounds());
    currentOverlay = std::move(overlay);
}
 
void NOVAAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(NovaColors::Background);
    auto area = getLocalBounds();
    auto palette = area.removeFromLeft(140);
    g.setColour(NovaColors::MixerPanel); g.fillRect(palette);
    g.setColour(juce::Colours::black.withAlpha(0.5f)); g.drawVerticalLine(palette.getRight() - 1, 0, (float)getHeight());
    g.setColour(juce::Colours::white); g.setFont(24.0f); g.drawText("NOVA", palette.removeFromTop(60), juce::Justification::centred);
    g.setColour(juce::Colours::grey); g.setFont(12.0f); g.drawText("COMPONENTS", palette.removeFromTop(20), juce::Justification::centred);

    int footerH = 180;
    auto footer = getLocalBounds().removeFromBottom(footerH);
    g.setColour(NovaColors::MixerPanel); g.fillRect(footer);
    g.setColour(juce::Colours::white.withAlpha(0.1f)); g.drawHorizontalLine(footer.getY(), 0, (float)getWidth());
    int mid = footer.getCentreX();
    g.setColour(juce::Colours::black); g.drawVerticalLine(mid, (float)footer.getY() + 10, (float)footer.getBottom() - 10);
    g.setColour(NovaColors::CableOnA); g.setFont(14.0f); g.drawText("LINE A OUTPUT", footer.getX() + 200, footer.getY() + 10, 150, 20, juce::Justification::centred);
    g.setColour(NovaColors::CableOnB); g.drawText("LINE B OUTPUT", footer.getRight() - 350, footer.getY() + 10, 150, 20, juce::Justification::centred);
}

void NOVAAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    // Si el overlay está activo, lo redimensionamos al tamaño total
    if (currentOverlay) {
        currentOverlay->setBounds(area);
    }

    auto palette = area.removeFromLeft(140);
    palette.removeFromTop(80);
    btnAddOverdrive.setBounds(palette.removeFromTop(50).reduced(15, 5));
    btnAddCabinet.setBounds(palette.removeFromTop(50).reduced(15, 5));

    auto footer = area.removeFromBottom(180);
    auto glob = footer.removeFromLeft(250); glob.reduced(20);
    btnStartStop.setBounds(glob.removeFromTop(50).reduced(10));
    btnSwitcher.setBounds(glob.removeFromTop(50).reduced(10));
    auto mixA = footer.removeFromLeft(footer.getWidth() / 2);
    int kz = 90;
    volSliderA.setBounds(mixA.getCentreX() - kz / 2, mixA.getCentreY() - kz / 2 - 10, kz, kz);
    volLabelA.setBounds(mixA.getCentreX() - 40, mixA.getCentreY() + 35, 80, 20);
    auto mixB = footer;
    volSliderB.setBounds(mixB.getCentreX() - kz / 2, mixB.getCentreY() - kz / 2 - 10, kz, kz);
    volLabelB.setBounds(mixB.getCentreX() - 40, mixB.getCentreY() + 35, 80, 20);

    auto main = area;
    audioProcessor.audioVisualizer.setBounds(main.removeFromTop(120));
    main.removeFromTop(20);
    int laneH = main.getHeight() / 2;
    if (laneA) laneA->setBounds(main.removeFromTop(laneH).reduced(20, 10));
    if (laneB) laneB->setBounds(main.reduced(20, 10));
    updatePedalGui();
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