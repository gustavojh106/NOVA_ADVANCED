#include "PluginProcessor.h"
#include "PluginEditor.h"

// ==============================================================================
// ESTILO Y COLORES
// ==============================================================================
namespace NovaColors
{
    const juce::Colour Background = juce::Colour::fromString("ff111111");
    const juce::Colour Panel = juce::Colour::fromString("ff1a1a1a");
    const juce::Colour CableOff = juce::Colour::fromString("ff333333");
    const juce::Colour CableOnA = juce::Colour::fromString("ff00aaff"); // Cyan Neon
    const juce::Colour CableOnB = juce::Colour::fromString("ffffaa00"); // Gold Neon
}

// ==============================================================================
// DROP ZONE (El Imán)
// ==============================================================================
class DropZone : public juce::Component, public juce::DragAndDropTarget
{
public:
    DropZone(NOVAAudioProcessor& p, Nova::ChainID c, Nova::ZoneID z)
        : proc(p), chain(c), zone(z) {
    }

    bool isInterestedInDragSource(const SourceDetails&) override { return true; }

    void itemDropped(const SourceDetails& d) override
    {
        isHover = false;
        repaint();
        proc.requestAddPedal(d.description.toString(), chain, zone);
    }

    void itemDragEnter(const SourceDetails&) override { isHover = true; repaint(); }
    void itemDragExit(const SourceDetails&) override { isHover = false; repaint(); }

    void paint(juce::Graphics& g) override
    {
        // Dibujamos un marco sutil para saber que es una zona
        g.setColour(juce::Colours::white.withAlpha(0.03f));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 4.0f, 1.0f);

        juce::String label;
        switch (zone) {
        case Nova::ZoneID::Pre: label = "PRE-FX (DRAG HERE)"; break;
        case Nova::ZoneID::Amp: label = "AMP"; break;
        case Nova::ZoneID::FX:  label = "MOD/DELAY (DRAG HERE)"; break;
        case Nova::ZoneID::Cabinet: label = "CAB"; break;
        }

        // Etiqueta en el fondo
        g.setColour(juce::Colours::grey.withAlpha(0.3f));
        g.setFont(12.0f);
        g.drawText(label, getLocalBounds().removeFromBottom(20), juce::Justification::centred);

        // Feedback al arrastrar (El "Imán" visual)
        if (isHover)
        {
            g.setColour(juce::Colours::white.withAlpha(0.1f));
            g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
            g.setColour(juce::Colours::cyan);
            g.drawRoundedRectangle(getLocalBounds().toFloat(), 4.0f, 2.0f);
        }
    }

private:
    NOVAAudioProcessor& proc;
    Nova::ChainID chain;
    Nova::ZoneID zone;
    bool isHover = false;
};

// ==============================================================================
// CHAIN LANE (El Carril de Cables)
// ==============================================================================
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

        // --- LAYOUT FÍSICO (N Pedales) ---
        // Pre-FX y Post-FX necesitan mucho espacio. Amp y Cab son fijos.
        // Total width ~ 100%

        int w = area.getWidth();
        int h = area.getHeight();

        // Amp y Cab suelen ser grandes pero únicos, démosle tamaño fijo o proporcional
        int ampW = 200;
        int cabW = 200;
        int remaining = w - (ampW + cabW);

        // Repartimos el resto entre PRE y FX (donde van N pedales)
        int preW = remaining / 2;
        int fxW = remaining / 2;

        zones[0]->setBounds(0, 0, preW, h);               // PRE
        zones[1]->setBounds(preW, 0, ampW, h);            // AMP
        zones[2]->setBounds(preW + ampW, 0, fxW, h);      // FX
        zones[3]->setBounds(preW + ampW + fxW, 0, cabW, h); // CAB
    }

    void paint(juce::Graphics& g) override
    {
        // --- DIBUJADO DE CABLES FÍSICOS ---
        auto h = getHeight() / 2.0f;
        auto w = (float)getWidth();

        juce::Path cablePath;
        cablePath.startNewSubPath(0, h);
        cablePath.lineTo(w, h);

        // Color según estado y cadena
        juce::Colour glowCol = (chainID == Nova::ChainID::LineA) ? NovaColors::CableOnA : NovaColors::CableOnB;
        if (!isLaneActive) glowCol = NovaColors::CableOff;

        // 1. Cable grueso oscuro (el cuerpo del cable)
        g.setColour(juce::Colour::fromString("ff050505"));
        g.strokePath(cablePath, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // 2. Núcleo del cable (Brillo)
        g.setColour(glowCol);
        float thickness = isLaneActive ? 3.0f : 1.0f;
        g.strokePath(cablePath, juce::PathStrokeType(thickness));

        // 3. Efecto Glow (Neon) si está activo
        if (isLaneActive)
        {
            g.setColour(glowCol.withAlpha(0.4f));
            g.strokePath(cablePath, juce::PathStrokeType(8.0f));
        }
    }

    juce::Rectangle<int> getZoneBounds(int zoneIndex)
    {
        if (zoneIndex >= 0 && zoneIndex < zones.size()) return zones[zoneIndex]->getBounds();
        return {};
    }

private:
    juce::OwnedArray<DropZone> zones;
    Nova::ChainID chainID;
    bool isLaneActive = false;
};

// ==============================================================================
// EDITOR PRINCIPAL IMPLEMENTACIÓN
// ==============================================================================

NOVAAudioProcessorEditor::NOVAAudioProcessorEditor(NOVAAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    // 1. Visualizer
    addAndMakeVisible(audioProcessor.audioVisualizer);

    // 2. Lanes
    laneA = std::make_unique<ChainLane>(p, Nova::ChainID::LineA);
    addAndMakeVisible(laneA.get());

    laneB = std::make_unique<ChainLane>(p, Nova::ChainID::LineB);
    addAndMakeVisible(laneB.get());

    // 3. Controles
    addAndMakeVisible(btnStartStop);
    btnStartStop.setClickingTogglesState(true);
    btnStartStop.onClick = [this] { audioProcessor.toggleEngine(); };

    addAndMakeVisible(btnSwitcher);
    btnSwitcher.onClick = [this] { audioProcessor.cycleSwitcher(); };

    // 4. Paleta
    addAndMakeVisible(btnAddOverdrive);
    addAndMakeVisible(btnAddCabinet);

    audioProcessor.pluginState.addListener(this);

    // --- RESOLUCIÓN FULL HD ---
    setResizable(true, true);
    setSize(1920, 1080); // <--- REQUERIMIENTO 3

    updateSwitcherState();
    updatePedalGui();
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
    audioProcessor.pluginState.removeListener(this);
    activePedalEditors.clear();
}

void NOVAAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(NovaColors::Background);

    // Header Paleta
    g.setColour(NovaColors::Panel);
    g.fillRect(0, 0, 150, getHeight()); // Panel izquierdo más ancho

    g.setColour(juce::Colours::white);
    g.setFont(30.0f);
    g.drawText("NOVA", 0, 20, 150, 40, juce::Justification::centred);

    // Labels Lineas
    g.setFont(20.0f);
    g.setColour(NovaColors::CableOnA);
    if (laneA) g.drawText("LINE A", 160, laneA->getY() - 30, 100, 30, juce::Justification::left);

    g.setColour(NovaColors::CableOnB);
    if (laneB) g.drawText("LINE B", 160, laneB->getY() - 30, 100, 30, juce::Justification::left);
}

void NOVAAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    auto palette = area.removeFromLeft(150); // Paleta más ancha

    palette.removeFromTop(100);
    btnAddOverdrive.setBounds(palette.removeFromTop(50).reduced(15, 5));
    btnAddCabinet.setBounds(palette.removeFromTop(50).reduced(15, 5));

    auto main = area;
    audioProcessor.audioVisualizer.setBounds(main.removeFromTop(150)); // Visualizador más grande

    auto controls = main.removeFromBottom(80);
    btnStartStop.setBounds(controls.removeFromLeft(150).reduced(10));
    btnSwitcher.setBounds(controls.removeFromLeft(150).reduced(10));

    main.removeFromTop(40); // Margen para títulos

    int laneH = main.getHeight() / 2;
    // Ajustamos lanes con margen para que se vean los cables
    if (laneA) laneA->setBounds(main.removeFromTop(laneH).reduced(10, 20));
    if (laneB) laneB->setBounds(main.reduced(10, 20));

    updatePedalGui();
}

void NOVAAudioProcessorEditor::updatePedalGui()
{
    activePedalEditors.clear();

    // --- LÓGICA DE N PEDALES (REQUERIMIENTO 1 y 2) ---
    // Esta función coloca los pedales visualmente.

    auto processLine = [&](Nova::ChainID chain, ChainLane* laneComp)
        {
            if (!laneComp) return;
            const auto& nodes = audioProcessor.getAudioEngine().getNodes(chain);
            auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
            auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);

            // Contadores para apilar pedales en la misma zona
            // zoneCounts[0] = cuantos hay en pre, zoneCounts[2] = cuantos hay en FX...
            int zoneCounts[4] = { 0, 0, 0, 0 };

            for (int i = 0; i < nodes.size(); ++i)
            {
                if (i >= treeList.getNumChildren()) break;

                auto node = nodes[i];
                auto state = treeList.getChild(i);
                int zoneIdx = state.getProperty(Nova::IDs::PEDAL_ZONE);

                if (node && node->getProcessor())
                {
                    if (auto* editor = node->getProcessor()->createEditor())
                    {
                        addAndMakeVisible(editor);
                        activePedalEditors.add(editor);

                        // --- LAYOUT DINÁMICO ---
                        // Obtenemos el rectángulo de la zona completa (ej. todo el ancho de PRE)
                        auto zoneRect = laneComp->getZoneBounds(zoneIdx);

                        // Definimos ancho estándar de un pedal
                        int pedalW = 120;
                        int pedalH = 180;

                        // Calculamos X: Inicio zona + (Numero de pedales ya puestos * ancho pedal)
                        int xOffset = zoneCounts[zoneIdx] * (pedalW + 5);

                        // Centramos verticalmente en el cable
                        int yCenter = zoneRect.getY() + (zoneRect.getHeight() - pedalH) / 2;

                        // Posición Global
                        auto globalPos = laneComp->getLocalPoint(this, zoneRect.getPosition());

                        editor->setBounds(laneComp->getX() + zoneRect.getX() + xOffset + 10,
                            laneComp->getY() + yCenter,
                            pedalW, pedalH);

                        zoneCounts[zoneIdx]++;
                    }
                }
            }
        };

    processLine(Nova::ChainID::LineA, laneA.get());
    processLine(Nova::ChainID::LineB, laneB.get());
}

void NOVAAudioProcessorEditor::updateSwitcherState()
{
    auto s = audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (!s.isValid()) return;

    bool on = s.getProperty(Nova::IDs::ENGINE_ON);
    int mode = s.getProperty(Nova::IDs::SWITCH_MODE);

    btnStartStop.setButtonText(on ? "SYSTEM ONLINE" : "SYSTEM OFFLINE");
    btnStartStop.setColour(juce::TextButton::buttonOnColourId, on ? juce::Colours::green : juce::Colours::red);
    btnStartStop.setToggleState(on, juce::dontSendNotification);

    juce::String txt;
    // Iluminación de cables según modo
    bool aActive = false;
    bool bActive = false;

    if (mode == 0) { txt = "ROUTING: A"; aActive = true; }
    else if (mode == 1) { txt = "ROUTING: B"; bActive = true; }
    else { txt = "ROUTING: PARALLEL"; aActive = true; bActive = true; }

    btnSwitcher.setButtonText(txt);

    // Actualizamos visualización de cables
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
    if (key == juce::KeyPress::spaceKey) {
        audioProcessor.cycleSwitcher();
        return true;
    }
    return false;
}