#include "DropZone.h"
#include "../../Core/PluginEditor.h"
#include "../../Core/PedalCatalog.h"
#include "../../Core/PluginStateModel.h"

#include <limits>

namespace
{
Nova::ChainID chainFromToken(const juce::String& token)
{
    return token == "LineB" ? Nova::ChainID::LineB : Nova::ChainID::LineA;
}
}

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
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
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
    tooltipOverlay = std::make_unique<FloatingTooltip>();

    addAndMakeVisible(addButton);
    addButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    addButton.setColour(juce::TextButton::textColourOnId, Nova::Colors::Accent);
    addButton.setColour(juce::TextButton::textColourOffId, Nova::Colors::TextDim);
    addButton.onClick = [this]
    {
        if (auto* editor = findParentComponentOfClass<NOVAAudioProcessorEditor>())
            editor->showOverlay(zone, chain);
    };
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

    // Top reserved area — (i) icon
    auto topArea = bounds.removeFromTop((float)kTopReserved);
    infoIconBounds = topArea.removeFromRight(28.0f).withSizeKeepingCentre(16.0f, 16.0f);

    // Bottom reserved area — zone name + ADD button
    auto bottomArea = getLocalBounds().removeFromBottom(kBottomReserved);
    auto addBtnArea = bottomArea.removeFromRight(juce::jmin(40, bottomArea.getWidth() / 3));
    addButton.setBounds(addBtnArea.reduced(2, 6));
}

juce::Rectangle<float> DropZone::getDropContentBounds() const
{
    auto bounds = getLocalBounds().toFloat().reduced(6.0f, 0.0f);
    bounds.removeFromTop((float)kTopReserved);
    bounds.removeFromBottom((float)kBottomReserved);
    return bounds;
}

// Validacion y Drag & Drop
bool DropZone::isMoveDrag(const juce::String& dragInfo) const
{
    return dragInfo.startsWith("MOVE:");
}

std::optional<int> DropZone::getDraggedPedalIndex(const juce::String& dragInfo) const
{
    if (!isMoveDrag(dragInfo))
        return std::nullopt;

    auto parts = juce::StringArray::fromTokens(dragInfo, ":", "");
    if (parts.size() < 3)
        return std::nullopt;

    return parts[2].getIntValue();
}

juce::String DropZone::getDraggedPedalType(const juce::String& dragInfo) const
{
    if (isMoveDrag(dragInfo))
    {
        auto parts = juce::StringArray::fromTokens(dragInfo, ":", "");
        if (parts.size() < 3)
            return {};

        const auto dragChain = chainFromToken(parts[1]);
        if (dragChain != chain)
            return {};

        const int fromIndex = parts[2].getIntValue();
        const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
        auto treeList = proc.pluginState.getChildWithName(treeListID);
        if (!treeList.isValid() || !juce::isPositiveAndBelow(fromIndex, treeList.getNumChildren()))
            return {};

        return treeList.getChild(fromIndex).getProperty(Nova::IDs::PEDAL_TYPE).toString();
    }

    if (!dragInfo.contains(":"))
        return {};

    return dragInfo.substring(dragInfo.indexOf(":") + 1).trim();
}

bool DropZone::isValidDragType(const juce::String& dragInfo) const
{
    const auto pedalType = getDraggedPedalType(dragInfo);
    if (pedalType.isEmpty())
        return false;

    const bool isMoveOp = isMoveDrag(dragInfo);

    if (isMoveOp)
    {
        auto parts = juce::StringArray::fromTokens(dragInfo, ":", "");
        if (parts.size() < 3 || chainFromToken(parts[1]) != chain)
            return false;
    }

    if (!Nova::PedalCatalog::canLiveInZone(pedalType, zone))
        return false;

    // Capacity gate for flex zones (Pre / FX)
    if (zone == Nova::ZoneID::Pre || zone == Nova::ZoneID::FX)
    {
        const int current = Nova::PluginStateModel::countPedalsInZone(proc.pluginState, chain, zone);
        const int limit   = Nova::Config::MAX_PEDALS_PER_FLEX_ZONE;

        if (isMoveOp)
        {
            // A move from the same zone doesn't change count — always OK
            auto parts = juce::StringArray::fromTokens(dragInfo, ":", "");
            const int fromIndex = parts[2].getIntValue();
            const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
            auto treeList = proc.pluginState.getChildWithName(treeListID);
            if (treeList.isValid() && juce::isPositiveAndBelow(fromIndex, treeList.getNumChildren()))
            {
                const auto fromZone = static_cast<Nova::ZoneID>(
                    (int)treeList.getChild(fromIndex).getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
                if (fromZone == zone)
                    return true;  // same-zone reorder, no net change
            }
            // Cross-zone move — needs a free slot
            if (current >= limit)
                return false;
        }
        else
        {
            // New pedal from browser
            if (current >= limit)
                return false;
        }
    }

    return true;
}

DropZone::DropPreview DropZone::calculateDropPreview(int dropX) const
{
    const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
    auto treeList = proc.pluginState.getChildWithName(treeListID);
    if (!treeList.isValid())
        return {};

    std::vector<int> zoneIndices;
    for (int i = 0; i < treeList.getNumChildren(); ++i)
    {
        auto child = treeList.getChild(i);
        if (!child.hasType(Nova::IDs::PEDAL))
            continue;
        const auto pZone = static_cast<Nova::ZoneID>(
            (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
        if (pZone == zone)
            zoneIndices.push_back(i);
    }

    DropPreview preview;
    preview.valid = true;

    const auto contentBounds = getDropContentBounds();
    if (contentBounds.isEmpty())
        return {};

    if (isFixedSlot())
    {
        int zoneStart = treeList.getNumChildren();
        const auto zoneRank = Nova::PluginStateModel::zoneSortRank(zone);

        for (int i = 0; i < treeList.getNumChildren(); ++i)
        {
            auto child = treeList.getChild(i);
            if (!child.hasType(Nova::IDs::PEDAL))
                continue;

            const auto childZone = static_cast<Nova::ZoneID>(
                (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
            if (Nova::PluginStateModel::zoneSortRank(childZone) >= zoneRank)
            {
                zoneStart = i;
                break;
            }
        }

        preview.insertIndex = juce::jlimit(0, treeList.getNumChildren(), zoneStart);
        preview.slotIndex = 0;
        preview.markerBounds = contentBounds.reduced(12.0f, 10.0f);
        return preview;
    }

    auto slotBounds = std::vector<juce::Rectangle<int>>{};
    if (auto* editor = findParentComponentOfClass<NOVAAudioProcessorEditor>())
    {
        slotBounds = editor->getPedalBoundsForZone(chain, zone);
        for (auto& b : slotBounds)
            b = getLocalArea(editor, b);
    }

    std::vector<float> slotXs;

    if (!slotBounds.empty())
    {
        slotXs.reserve(slotBounds.size() + 1);

        const float firstGap = juce::jmax(12.0f, (slotBounds.front().getX() - contentBounds.getX()) * 0.5f);
        slotXs.push_back(juce::jlimit(contentBounds.getX(), contentBounds.getRight(),
            (float)slotBounds.front().getX() - firstGap));

        for (size_t i = 0; i + 1 < slotBounds.size(); ++i)
        {
            const auto left = (float)slotBounds[i].getRight();
            const auto right = (float)slotBounds[i + 1].getX();
            slotXs.push_back(juce::jlimit(contentBounds.getX(), contentBounds.getRight(), (left + right) * 0.5f));
        }

        const float lastGap = juce::jmax(12.0f, (contentBounds.getRight() - slotBounds.back().getRight()) * 0.5f);
        slotXs.push_back(juce::jlimit(contentBounds.getX(), contentBounds.getRight(),
            (float)slotBounds.back().getRight() + lastGap));
    }
    else
    {
        const int slotCount = juce::jmax(1, (int)zoneIndices.size() + 1);
        slotXs.reserve((size_t)slotCount);

        for (int i = 0; i < slotCount; ++i)
        {
            const float t = slotCount == 1 ? 0.5f : (float)i / (float)(slotCount - 1);
            slotXs.push_back(contentBounds.getX() + contentBounds.getWidth() * t);
        }
    }

    if (slotXs.empty())
        slotXs.push_back(contentBounds.getCentreX());

    float nearestDistance = std::numeric_limits<float>::max();
    int nearestSlot = 0;

    for (int i = 0; i < (int)slotXs.size(); ++i)
    {
        const float distance = std::abs((float)dropX - slotXs[(size_t)i]);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearestSlot = i;
        }
    }

    int insertIndex = 0;
    if (zoneIndices.empty())
    {
        const auto zoneRank = Nova::PluginStateModel::zoneSortRank(zone);
        insertIndex = treeList.getNumChildren();

        for (int i = 0; i < treeList.getNumChildren(); ++i)
        {
            auto child = treeList.getChild(i);
            if (!child.hasType(Nova::IDs::PEDAL))
                continue;

            const auto childZone = static_cast<Nova::ZoneID>(
                (int)child.getProperty(Nova::IDs::PEDAL_ZONE, (int)Nova::ZoneID::Pre));
            if (Nova::PluginStateModel::zoneSortRank(childZone) > zoneRank)
            {
                insertIndex = i;
                break;
            }
        }
    }
    else if (nearestSlot <= 0)
    {
        insertIndex = zoneIndices.front();
    }
    else if (nearestSlot >= (int)zoneIndices.size())
    {
        insertIndex = zoneIndices.back() + 1;
    }
    else
    {
        insertIndex = zoneIndices[(size_t)nearestSlot];
    }

    preview.insertIndex = juce::jlimit(0, treeList.getNumChildren(), insertIndex);
    preview.slotIndex = nearestSlot;
    preview.markerBounds = juce::Rectangle<float>(slotXs[(size_t)nearestSlot] - 3.0f,
        contentBounds.getY() + 6.0f,
        6.0f,
        juce::jmax(16.0f, contentBounds.getHeight() - 12.0f));

    return preview;
}

void DropZone::clearDropPreview()
{
    currentPreview = {};
}

void DropZone::updateDropPreview(const juce::String& dragInfo, juce::Point<int> localPos)
{
    if (!isValidDragType(dragInfo))
    {
        clearDropPreview();
        return;
    }

    currentPreview = calculateDropPreview(localPos.getX());
}

void DropZone::handleMoveDrop(const juce::String& dragInfo, const DropPreview& preview)
{
    auto parts = juce::StringArray::fromTokens(dragInfo, ":", "");
    if (parts.size() < 3 || !preview.valid)
        return;

    const int fromIndex = parts[2].getIntValue();
    proc.requestMovePedal(chain, fromIndex, preview.insertIndex, zone);
}

void DropZone::handleAddDrop(const juce::String& dragInfo, const DropPreview& preview)
{
    if (!preview.valid)
        return;

    const auto pedalName = getDraggedPedalType(dragInfo);
    if (pedalName.isNotEmpty())
        proc.requestAddPedal(pedalName, chain, zone, preview.insertIndex);
}

bool DropZone::isInterestedInDragSource(const SourceDetails& d) { return d.description.isString(); }

void DropZone::itemDragEnter(const SourceDetails& d)
{
    dragState = isValidDragType(d.description.toString()) ? DragState::Valid : DragState::Invalid;
    updateDropPreview(d.description.toString(), d.localPosition);
    repaint();
}

void DropZone::itemDragMove(const SourceDetails& d)
{
    dragState = isValidDragType(d.description.toString()) ? DragState::Valid : DragState::Invalid;
    updateDropPreview(d.description.toString(), d.localPosition);
    repaint();
}

void DropZone::itemDragExit(const SourceDetails&)
{
    dragState = DragState::None;
    clearDropPreview();
    repaint();
}

void DropZone::itemDropped(const SourceDetails& details)
{
    dragState = DragState::None;
    const juce::String dragSource = details.description.toString();
    updateDropPreview(dragSource, details.localPosition);
    const auto preview = currentPreview;

    if (isMoveDrag(dragSource) && preview.valid)
    {
        handleMoveDrop(dragSource, preview);
    }
    else if (preview.valid)
    {
        handleAddDrop(dragSource, preview);
    }
    else
    {
        triggerShake();
    }

    clearDropPreview();
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
    const auto validDragColour = (chain == Nova::ChainID::LineA) ? Nova::Colors::CableOnA : Nova::Colors::CableOnB;
    juce::Colour borderColor = juce::Colours::grey.withAlpha(0.3f);
    juce::Colour bgColor = juce::Colours::transparentBlack;

    if (dragState == DragState::Valid)
    {
        borderColor = validDragColour;
        bgColor = validDragColour.withAlpha(0.15f);
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

    if (dragState == DragState::Valid && currentPreview.valid)
    {
        if (isFixedSlot())
        {
            g.setColour(borderColor.withAlpha(0.12f));
            g.fillRoundedRectangle(currentPreview.markerBounds, 10.0f);
            g.setColour(borderColor.withAlpha(0.35f));
            g.drawRoundedRectangle(currentPreview.markerBounds, 10.0f, 1.5f);
        }
        else
        {
            auto glowBounds = currentPreview.markerBounds.expanded(7.0f, 4.0f);
            g.setColour(borderColor.withAlpha(0.15f));
            g.fillRoundedRectangle(glowBounds, 10.0f);
            g.setColour(borderColor.withAlpha(0.92f));
            g.fillRoundedRectangle(currentPreview.markerBounds, 4.0f);
        }
    }

    // ---- Bottom reserved: zone title + ADD button ----
    juce::String title;
    if (zone == Nova::ZoneID::Pre)          title = "PRE-AMP";
    else if (zone == Nova::ZoneID::Amp)     title = "AMP HEAD";
    else if (zone == Nova::ZoneID::FX)      title = "POST-AMP";
    else if (zone == Nova::ZoneID::Cabinet) title = "CABINET";

    auto footerBounds = getLocalBounds().toFloat().removeFromBottom((float)kBottomReserved);
    footerBounds = footerBounds.reduced(8.0f, 0.0f);

    // Zone title — left of the ADD button
    auto titleArea = footerBounds.withTrimmedRight(40.0f);
    g.setColour(isMouseOver(true) ? juce::Colours::white.withAlpha(0.9f) : juce::Colours::grey.withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawFittedText(title, titleArea.toNearestInt(), juce::Justification::centredLeft, 1);

    // ---- Top reserved: (i) icon ----
    g.setColour(isHoveringInfo ? juce::Colours::white : juce::Colours::grey.withAlpha(0.6f));
    g.drawEllipse(infoIconBounds, 1.0f);
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawFittedText("i", infoIconBounds.toNearestInt(), juce::Justification::centred, 1);
}
