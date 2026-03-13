#include "DropZone.h"
#include "../../Core/PluginEditor.h"
#include "../../Core/PedalCatalog.h"

// ==============================================================================
// CLASE TOOLTIP FLOTANTE (Se inyecta en el Editor Principal)
// ==============================================================================
class FloatingTooltip : public juce::Component
{
public:
    FloatingTooltip()
    {
        // Importante: Esto evita que el tooltip robe el click o el hover al aparecer
        setInterceptsMouseClicks(false, false);
    }

    ~FloatingTooltip() override
    {
        if (getParentComponent() != nullptr)
            getParentComponent()->removeChildComponent(this);
    }

    void show(juce::Component* topLevelParent, juce::Rectangle<int> iconAreaInParent, const juce::String& text)
    {
        helpText = text;
        topLevelParent->addAndMakeVisible(this); // Lo pegamos a la ventana principal

        int w = 180;
        int h = 75;

        // Lo posicionamos arriba del icono (i)
        int x = iconAreaInParent.getX() - w + 20;
        int y = iconAreaInParent.getY() - h - 5;

        // Si no cabe arriba (ej: zonas muy pegadas al techo), lo bajamos
        if (y < 0) y = iconAreaInParent.getBottom() + 5;
        if (x < 0) x = 5;

        setBounds(x, y, w, h);
        toFront(false); // Traer al frente de TODO en el Eje Z
    }

    void hide() { setVisible(false); }

    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colours::black.withAlpha(0.95f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
        g.setColour(juce::Colours::grey.withAlpha(0.5f));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 6.0f, 1.0f);
        g.setColour(juce::Colours::white);
        g.setFont(12.0f);
        g.drawFittedText(helpText, getLocalBounds().reduced(8), juce::Justification::centred, 4);
    }
private:
    juce::String helpText;
};


// ==============================================================================
// IMPLEMENTACION DE DROPZONE
// ==============================================================================

DropZone::DropZone(NOVAAudioProcessor& processor, Nova::ChainID chainId, Nova::ZoneID zoneId)
    : proc(processor), chain(chainId), zone(zoneId)
{
    setRepaintsOnMouseActivity(true);
    tooltipOverlay = std::make_unique<FloatingTooltip>(); // Instanciamos el overlay
}

DropZone::~DropZone()
{
    stopTimer();
    tooltipOverlay.reset(); // Destruccion segura
}

bool DropZone::isFixedSlot() const noexcept { return zone == Nova::ZoneID::Amp || zone == Nova::ZoneID::Cabinet; }

void DropZone::resized()
{
    auto bounds = getLocalBounds().toFloat();
    infoIconBounds = bounds.removeFromTop(30.0f).removeFromRight(30.0f).withSizeKeepingCentre(16.0f, 16.0f);
}

// Validacion y Drag & Drop
bool DropZone::isValidDragType(const juce::String& dragInfo) const
{
    if (!dragInfo.contains(":"))
        return false;

    const auto itemName = dragInfo.substring(dragInfo.indexOf(":") + 1);
    return Nova::PedalCatalog::canLiveInZone(itemName, zone);
}
bool DropZone::isInterestedInDragSource(const SourceDetails& d) { return d.description.isString(); }

void DropZone::itemDragEnter(const SourceDetails& d)
{
    dragState = isValidDragType(d.description.toString()) ? DragState::Valid : DragState::Invalid;
    repaint();
}

void DropZone::itemDragExit(const SourceDetails&)
{
    dragState = DragState::None;
    repaint();
}

void DropZone::itemDropped(const SourceDetails& details)
{
    dragState = DragState::None;
    juce::String dragSource = details.description.toString();

    if (isValidDragType(dragSource))
    {
        juce::String pedalName = dragSource.contains(":") ? dragSource.substring(dragSource.indexOf(":") + 1) : dragSource;
        proc.requestAddPedal(pedalName, chain, zone);
    }
    else triggerShake();

    repaint();
}

void DropZone::triggerShake()
{
    shakeTicks = 0;
    startTimerHz(60);
}

void DropZone::timerCallback()
{
    shakeOffset = (shakeTicks % 4 < 2) ? 6 : -6;
    shakeTicks++;
    if (shakeTicks > 12)
    {
        shakeOffset = 0;
        stopTimer();
    }
    repaint();
}

// ==============================================================================
// CONTROL DEL MOUSE (AQUI ESTA LA MAGIA DEL TOOLTIP FLOTANTE)
// ==============================================================================
void DropZone::mouseMove(const juce::MouseEvent& e)
{
    bool hover = infoIconBounds.contains(e.position.toFloat());
    if (isHoveringInfo != hover)
    {
        isHoveringInfo = hover;

        if (isHoveringInfo)
        {
            // Buscamos el editor principal y le pedimos sus coordenadas reales
            if (auto* editor = findParentComponentOfClass<NOVAAudioProcessorEditor>())
            {
                auto iconAreaInEditor = editor->getLocalArea(this, infoIconBounds.toNearestInt());
                tooltipOverlay->show(editor, iconAreaInEditor, getHelpText());
            }
        }
        else
        {
            tooltipOverlay->hide();
        }
        repaint();
    }
}

void DropZone::mouseExit(const juce::MouseEvent&)
{
    if (isHoveringInfo)
    {
        isHoveringInfo = false;
        tooltipOverlay->hide(); // Ocultar si sacamos el raton rapido
        repaint();
    }
}

void DropZone::mouseDown(const juce::MouseEvent& e)
{
    if (!e.mods.isLeftButtonDown() || infoIconBounds.contains(e.position.toFloat()))
        return;

    if (auto* editor = findParentComponentOfClass<NOVAAudioProcessorEditor>())
        editor->showOverlay(zone, chain);
}

juce::String DropZone::getHelpText() const
{
    switch (zone)
    {
    case Nova::ZoneID::Pre: return "PRE-AMPLIFIER\n\nIdeal for altering gain:\nDistortion, Wah, Compressor.";
    case Nova::ZoneID::Amp: return "AMPLIFIER HEAD\n\nThe core engine of your tone.";
    case Nova::ZoneID::FX:  return "POST-AMPLIFIER\n\nModulation and ambiance:\nDelay, Reverb, Chorus.";
    case Nova::ZoneID::Cabinet: return "SPEAKER CABINET\n\nFinal acoustic simulation\nof the speaker.";
    default: return juce::String();
    }
}

// ==============================================================================
// DIBUJADO DE LA ZONA (SIN EL TOOLTIP, PORQUE AHORA FLOTA SOLO)
// ==============================================================================
void DropZone::drawTechGrid(juce::Graphics& g) const
{
    g.setColour(Nova::Colors::GridLine.withAlpha(0.1f));
    for (int x = 0; x < getWidth(); x += 20)
        g.drawVerticalLine(x, 0.0f, (float)getHeight());
}

void DropZone::drawDashedOutline(juce::Graphics& g, juce::Colour color, juce::Rectangle<float> area) const
{
    juce::Path border;
    border.addRoundedRectangle(area, 6.0f);
    float dashLengths[] = { 4.0f, 4.0f };
    juce::PathStrokeType stroke(1.5f);
    juce::Path dashedPath;
    stroke.createDashedStroke(dashedPath, border, dashLengths, 2);
    g.setColour(color);
    g.strokePath(dashedPath, stroke);
}

void DropZone::paint(juce::Graphics& g)
{
    g.setOrigin(shakeOffset, 0);

    auto bounds = getLocalBounds().toFloat();
    if (bounds.isEmpty()) return;

    if (!isFixedSlot()) drawTechGrid(g);

    // Estados Visuales
    juce::Colour borderColor = juce::Colours::grey.withAlpha(0.3f);
    juce::Colour bgColor = juce::Colours::transparentBlack;

    if (dragState == DragState::Valid)
    {
        borderColor = Nova::Colors::CableOnA;
        bgColor = Nova::Colors::CableOnA.withAlpha(0.15f);
    }
    else if (dragState == DragState::Invalid)
    {
        borderColor = juce::Colours::red;
        bgColor = juce::Colours::red.withAlpha(0.15f);
    }
    else if (isMouseOver(true))
    {
        borderColor = juce::Colours::white.withAlpha(0.5f);
        bgColor = juce::Colours::white.withAlpha(0.05f);
    }

    g.setColour(bgColor);
    g.fillRoundedRectangle(bounds, 6.0f);
    drawDashedOutline(g, borderColor, bounds);

    // Textos
    juce::String title;
    if (zone == Nova::ZoneID::Pre)      title = "PRE-AMPLIFIER";
    else if (zone == Nova::ZoneID::Amp) title = "AMPLIFIER HEAD";
    else if (zone == Nova::ZoneID::FX)  title = "POST-AMPLIFIER";
    else if (zone == Nova::ZoneID::Cabinet) title = "SPEAKER CABINET";

    g.setColour(isMouseOver(true) ? juce::Colours::white.withAlpha(0.9f) : juce::Colours::grey.withAlpha(0.6f));
    g.setFont(juce::Font(13.0f, juce::Font::bold));

    auto textArea = bounds.removeFromBottom(40.0f);
    g.drawFittedText(title, textArea.toNearestInt(), juce::Justification::centred, 1);

    // Icono (i)
    g.setColour(isHoveringInfo ? juce::Colours::white : juce::Colours::grey.withAlpha(0.6f));
    g.drawEllipse(infoIconBounds, 1.0f);
    g.setFont(juce::Font(11.0f, juce::Font::bold));
    g.drawFittedText("i", infoIconBounds.toNearestInt(), juce::Justification::centred, 1);
}
