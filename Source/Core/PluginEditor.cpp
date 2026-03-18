#include "PluginEditor.h"

#include "PedalCatalog.h"
#include "../GUI/Widgets/ChainLane.h"
#include "../GUI/Widgets/AssetBrowserOverlay.h"
#include <algorithm>

class PedalSlotComponent final : public juce::Component
{
public:
    PedalSlotComponent(NOVAAudioProcessor& ownerProcessor,
        Nova::ChainID ownerChain,
        std::unique_ptr<juce::AudioProcessorEditor> childEditor)
        : processor(ownerProcessor),
          chain(ownerChain),
          embeddedEditor(std::move(childEditor))
    {
        addAndMakeVisible(powerButton);
        powerButton.onClick = [this] { toggleBypass(); };

        addAndMakeVisible(removeButton);
        removeButton.onClick = [this] { removePedal(); };

        addAndMakeVisible(dragHandle);
        dragHandle.setMouseCursor(juce::MouseCursor::DraggingHandCursor);

        if (embeddedEditor != nullptr)
        {
            addAndMakeVisible(embeddedEditor.get());
            embeddedEditor->setInterceptsMouseClicks(true, true);
        }

        powerButton.setTriggeredOnMouseDown(false);
        removeButton.setTriggeredOnMouseDown(false);
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
        return juce::jmax(150, (embeddedEditor != nullptr ? embeddedEditor->getWidth() : 190) + 8);
    }

    int getPreferredHeight() const
    {
        return juce::jmax(132, (embeddedEditor != nullptr ? embeddedEditor->getHeight() : 170) + kHeaderHeight + 8);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool enabled = isPedalEnabled();

        juce::ColourGradient body(enabled ? juce::Colour::fromString("ff111827") : juce::Colour::fromString("ff080A0E"),
            bounds.getCentreX(),
            bounds.getY(),
            enabled ? juce::Colour::fromString("ff0B0E14") : juce::Colour::fromString("ff060810"),
            bounds.getCentreX(),
            bounds.getBottom(),
            false);
        g.setGradientFill(body);
        g.fillRoundedRectangle(bounds, 14.0f);

        g.setColour(enabled ? juce::Colours::white.withAlpha(0.08f) : juce::Colours::white.withAlpha(0.03f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 14.0f, 1.0f);

        auto header = bounds.reduced(6.0f).removeFromTop((float)kHeaderHeight - 4.0f);
        g.setColour(enabled ? juce::Colour::fromString("ff1A2332") : juce::Colour::fromString("ff0D1520"));
        g.fillRoundedRectangle(header, 10.0f);

        g.setColour(enabled ? juce::Colours::white.withAlpha(0.72f) : juce::Colours::white.withAlpha(0.32f));
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText(getDisplayName().toUpperCase(),
            header.toNearestInt().reduced(8, 0).withTrimmedRight(64),
            juce::Justification::centredLeft);

        if (!enabled)
        {
            auto overlay = bounds.reduced(6.0f);
            overlay.removeFromTop((float)kHeaderHeight);

            g.setColour(juce::Colours::black.withAlpha(0.62f));
            g.fillRoundedRectangle(overlay, 10.0f);

            g.setColour(juce::Colour::fromString("ff2A3548"));
            const float midY = overlay.getCentreY();
            g.drawLine(overlay.getX() + 20.0f, midY, overlay.getRight() - 20.0f, midY, 2.0f);
            g.drawLine(overlay.getRight() - 34.0f, midY - 7.0f, overlay.getRight() - 20.0f, midY, 2.0f);
            g.drawLine(overlay.getRight() - 34.0f, midY + 7.0f, overlay.getRight() - 20.0f, midY, 2.0f);

            g.setColour(juce::Colours::white.withAlpha(0.52f));
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText("POWER OFF", overlay.toNearestInt().withTrimmedBottom(8), juce::Justification::centredBottom);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6);
        auto header = area.removeFromTop(kHeaderHeight);

        removeButton.setBounds(header.removeFromRight(24).reduced(2));
        powerButton.setBounds(header.removeFromRight(42).reduced(2));
        dragHandle.setBounds(header.removeFromLeft(22).reduced(2));

        if (embeddedEditor != nullptr)
            embeddedEditor->setBounds(area.reduced(0, 2));
    }

private:
    // Drag handle component - initiates pedal reordering drag
    class DragHandleComponent : public juce::Component
    {
    public:
        DragHandleComponent() { setRepaintsOnMouseActivity(true); }

        void paint(juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(2.0f);
            const float cx = b.getCentreX();
            g.setColour(isMouseOver() ? juce::Colours::white.withAlpha(0.7f) : juce::Colours::grey.withAlpha(0.5f));
            for (int row = 0; row < 3; ++row)
            {
                float y = b.getY() + b.getHeight() * (0.2f + 0.3f * row);
                g.fillEllipse(cx - 4.0f, y - 1.0f, 2.0f, 2.0f);
                g.fillEllipse(cx + 2.0f, y - 1.0f, 2.0f, 2.0f);
            }
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (e.getDistanceFromDragStart() < 4)
                return;

            auto* slot = findParentComponentOfClass<PedalSlotComponent>();
            if (slot == nullptr)
                return;

            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
            {
                const auto chainStr = (slot->getChain() == Nova::ChainID::LineA) ? "LineA" : "LineB";
                const auto indexStr = juce::String(slot->getPedalIndex());
                container->startDragging("MOVE:" + juce::String(chainStr) + ":" + indexStr, slot);
            }
        }
    };

    static constexpr int kHeaderHeight = 24;

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

        return embeddedEditor != nullptr ? embeddedEditor->getName() : juce::String("Pedal");
    }

    void toggleBypass()
    {
        const int index = getPedalIndex();
        if (index < 0)
            return;

        processor.requestBypassPedal(chain, index, isPedalEnabled());
    }

    void removePedal()
    {
        const int index = getPedalIndex();
        if (index < 0)
            return;

        processor.requestRemovePedal(chain, index);
    }

    void refreshVisualState()
    {
        const bool enabled = isPedalEnabled();
        if (embeddedEditor != nullptr)
        {
            embeddedEditor->setAlpha(enabled ? 1.0f : 0.22f);
            embeddedEditor->setEnabled(enabled);
        }

        powerButton.setButtonText(enabled ? "ON" : "OFF");
        powerButton.setColour(juce::TextButton::buttonColourId,
            enabled ? juce::Colour::fromString("ff0F3D22") : juce::Colour::fromString("ff3D1418"));
        powerButton.setColour(juce::TextButton::textColourOffId,
            enabled ? Nova::Colors::Success : Nova::Colors::Error.withAlpha(0.7f));

        removeButton.setButtonText("x");
        removeButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff2D1418"));
        removeButton.setColour(juce::TextButton::textColourOffId, Nova::Colors::Error.withAlpha(0.65f));
    }

    NOVAAudioProcessor& processor;
    Nova::ChainID chain;
    juce::ValueTree pedalState;
    std::unique_ptr<juce::AudioProcessorEditor> embeddedEditor;
    juce::TextButton powerButton;
    juce::TextButton removeButton;
    DragHandleComponent dragHandle;
};

// ==============================================================================
// CONSTRUCTOR / INIT
// ==============================================================================

NOVAAudioProcessorEditor::NOVAAudioProcessorEditor(NOVAAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , audioProcessor(p)
{
    // -----------------------
    // HEADER
    // -----------------------
    addAndMakeVisible(btnStartStop);
    btnStartStop.setClickingTogglesState(true);
    btnStartStop.onClick = [this] { audioProcessor.toggleEngine(); };

    addAndMakeVisible(lblStats);
    lblStats.setJustificationType(juce::Justification::centred);
    lblStats.setColour(juce::Label::textColourId, juce::Colours::grey);
    lblStats.setFont(juce::Font(juce::FontOptions(12.0f)));
    lblStats.setText("CPU: - | Latency: -", juce::dontSendNotification);

    addAndMakeVisible(btnTuner);

    // IMPORTANTE: esto estaba en resized() (redundante). Aquí queda 1 vez y ya.
    btnTuner.onClick = [this] { toggleTuner(); };
    btnTuner.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnTuner.setColour(juce::TextButton::textColourOffId, Nova::Colors::Success);
    btnTuner.setTooltip("Mute the output and open the tuner overlay");

    btnStartStop.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff2D1418"));
    btnStartStop.setColour(juce::TextButton::textColourOffId, Nova::Colors::Text.withAlpha(0.85f));

    // -----------------------
    // BROWSER
    // -----------------------
    addAndMakeVisible(searchBarBrowser);
    searchBarBrowser.setTextToShowWhenEmpty("Filter quick-add modules...", juce::Colours::grey);
    searchBarBrowser.onTextChange = [this] { applyBrowserFilter(); };

    // Curated quick-add modules
    setupQuickAddButton(btnAddCompressor, "Compressor", "PEDAL");
    setupQuickAddButton(btnAddOverdrive, "Overdrive", "PEDAL");

    setupQuickAddButton(btnAddChorus, "Chorus", "PEDAL");
    setupQuickAddButton(btnAddDelay, "Delay", "PEDAL");

    setupQuickAddButton(btnAddReverb, "Reverb", "PEDAL");

    setupQuickAddButton(btnAddNeural, "Classic Amp", "AMP");

    setupQuickAddButton(btnAddCabinet, "Cabinet", "CAB");
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

    addAndMakeVisible(inputFader);
    inputFader.setSliderStyle(juce::Slider::LinearVertical);
    inputFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    inputFader.setRange(-60.0, 6.0, 0.1);
    inputFader.setValue(0.0, juce::dontSendNotification);

    // -----------------------
    // MIXER & LANES
    // -----------------------
    laneA = std::make_unique<ChainLane>(p, Nova::ChainID::LineA);
    addAndMakeVisible(laneA.get());

    laneB = std::make_unique<ChainLane>(p, Nova::ChainID::LineB);
    addAndMakeVisible(laneB.get());

    addAndMakeVisible(btnSwitcher);
    btnSwitcher.onClick = [this] { audioProcessor.cycleSwitcher(); };

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

    addAndMakeVisible(outputFader);
    outputFader.setSliderStyle(juce::Slider::LinearVertical);
    outputFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    outputFader.setRange(-60.0, 12.0, 0.1);
    outputFader.setValue(0.0, juce::dontSendNotification);
    outputFader.onValueChange = [this]
        {
            // Igual que antes: fader mueve el knob MASTER
            outputVolume.setValue(outputFader.getValue(), juce::sendNotification);
        };

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
    // PRESETS & FOOTER
    // -----------------------
    addAndMakeVisible(searchBarPresets);
    searchBarPresets.setTextToShowWhenEmpty("Filter...", juce::Colours::grey);
    searchBarPresets.onTextChange = [this] { refreshPresetList(); };

    addAndMakeVisible(lblCurrentPreset);
    lblCurrentPreset.setJustificationType(juce::Justification::centred);
    lblCurrentPreset.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    lblCurrentPreset.setFont(juce::Font(juce::FontOptions(12.0f)));
    setCurrentPreset("No Preset");

    addAndMakeVisible(presetSelector);
    presetSelector.setTextWhenNothingSelected("No Presets");
    // Seleccionar en la lista no cambia el preset activo hasta presionar LOAD.
    presetSelector.onChange = [] {};

    addAndMakeVisible(btnSave);
    btnSave.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnSave.onClick = [this] { saveSelectedOrPromptPreset(); };

    addAndMakeVisible(btnLoad);
    btnLoad.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff0F3D22"));
    btnLoad.onClick = [this] { loadSelectedPreset(); };

    addAndMakeVisible(btnClear);
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
    addAndMakeVisible(button);
    button.setButtonText(typeID);
    button.setItemType(itemType);
    button.setColour(juce::TextButton::buttonColourId,
        Nova::PedalCatalog::accentForType(typeID).withAlpha(0.18f));
    button.setColour(juce::TextButton::buttonOnColourId,
        Nova::PedalCatalog::accentForType(typeID).withAlpha(0.35f));
    button.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.86f));
    button.setTooltip(Nova::PedalCatalog::subtitleForType(typeID));
}

void NOVAAudioProcessorEditor::applyBrowserFilter()
{
    const auto filter = searchBarBrowser.getText().trim();
    const std::array<DraggableButton*, 7> quickButtons{
        &btnAddCompressor,
        &btnAddOverdrive,
        &btnAddChorus,
        &btnAddDelay,
        &btnAddReverb,
        &btnAddNeural,
        &btnAddCabinet
    };

    for (auto* button : quickButtons)
    {
        const bool matches = filter.isEmpty()
            || button->getButtonText().containsIgnoreCase(filter)
            || button->getTooltip().containsIgnoreCase(filter);

        button->setVisible(matches);
        button->setEnabled(matches);
    }

    resized();
    repaint();
}

juce::Rectangle<int> NOVAAudioProcessorEditor::getInputStripBounds() const
{
    auto area = getLocalBounds();
    area.removeFromTop(80);
    area.removeFromBottom(100);
    area.removeFromLeft(176);
    return area.removeFromLeft(120);
}

juce::Rectangle<int> NOVAAudioProcessorEditor::getOutputStripBounds() const
{
    auto area = getLocalBounds();
    area.removeFromTop(80);
    area.removeFromBottom(100);
    area.removeFromRight(160);
    return area.removeFromRight(120);
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
    outputFader.setValue(outputVolume.getValue(), juce::dontSendNotification);
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

    // Header
    auto headerRect = area.removeFromTop(80);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(headerRect);

    g.setColour(Nova::Colors::Border);
    g.drawHorizontalLine(headerRect.getBottom(), 0, (float)getWidth());

    g.setColour(Nova::Colors::Accent);
    g.setFont(juce::Font(juce::FontOptions(30.0f, juce::Font::bold)));
    g.drawText("NOVA", headerRect.removeFromLeft(150), juce::Justification::centred);
    g.setColour(Nova::Colors::TextDim);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText("Guitar Rig Designer", headerRect.removeFromLeft(260), juce::Justification::centredLeft);

    // Footer
    auto footerRect = area.removeFromBottom(100);
    g.setColour(Nova::Colors::Border);
    g.drawHorizontalLine(footerRect.getY(), 0, (float)getWidth());

    // Left browser column
    auto left1 = area.removeFromLeft(176);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(left1);

    g.setColour(Nova::Colors::Border);
    g.drawVerticalLine(left1.getRight(), (float)left1.getY(), (float)left1.getBottom());

    g.setColour(Nova::Colors::Accent);
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    g.drawText("QUICK ADD", left1.getX(), left1.getY() + 54, left1.getWidth(), 20, juce::Justification::centred);
    g.setColour(Nova::Colors::TextDim);
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawFittedText("Drag a module into a lane or click any zone to open the full browser.",
        juce::Rectangle<int>(left1.getX() + 10, left1.getY() + 82, left1.getWidth() - 20, 54),
        juce::Justification::centred,
        3);

    // Input strip
    auto left2 = area.removeFromLeft(120);
    drawChannelStrip(g, left2, "INPUT");

    // Presets column
    auto right2 = area.removeFromRight(160);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(right2);

    g.setColour(Nova::Colors::Border);
    g.drawVerticalLine(right2.getX(), (float)right2.getY(), (float)right2.getBottom());

    g.setColour(Nova::Colors::Accent);
    g.drawText("PRESETS", right2.getX(), right2.getY() + 60, right2.getWidth(), 20, juce::Justification::centred);

    // Output strip
    auto right1 = area.removeFromRight(120);
    drawChannelStrip(g, right1, "OUTPUT");

    // Mixer labels
    auto center = area;
    auto mixerArea = center.removeFromBottom(170);

    g.setColour(Nova::Colors::MixerPanel);
    g.drawRoundedRectangle(mixerArea.toFloat().reduced(10), 5.0f, 1.0f);

    g.setColour(Nova::Colors::Text);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    g.drawText("LINE A", mixerArea.getX() + 50, mixerArea.getY() + 10, 100, 20, juce::Justification::centred);
    g.drawText("LINE B", mixerArea.getRight() - 150, mixerArea.getY() + 10, 100, 20, juce::Justification::centred);

    const bool on = audioProcessor.isEngineOn();
    const int mode = (int)audioProcessor.getSwitcherMode();
    const bool aActive = on && (mode != (int)Nova::SwitcherMode::LineB_Only);
    const bool bActive = on && (mode != (int)Nova::SwitcherMode::LineA_Only);

    // --- Knob name labels (above the text value) ---
    const int knobSz = 60;
    const int knobGap = 10;
    const int startXA = mixerArea.getX() + 30;
    const int startXB = mixerArea.getRight() - 30 - knobSz;

    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.setColour(Nova::Colors::TextDim);

    const int yKnobLbl = mixerArea.getCentreY() - 52;  // above the knob
    g.drawText("LEVEL", startXA, yKnobLbl, knobSz, 14, juce::Justification::centred);
    g.drawText("PAN",   startXA + knobSz + knobGap, yKnobLbl, knobSz, 14, juce::Justification::centred);
    g.drawText("WIDTH", startXA + (knobSz + knobGap) * 2, yKnobLbl, knobSz, 14, juce::Justification::centred);

    g.drawText("WIDTH", startXB, yKnobLbl, knobSz, 14, juce::Justification::centred);
    g.drawText("PAN",   startXB - (knobSz + knobGap), yKnobLbl, knobSz, 14, juce::Justification::centred);
    g.drawText("LEVEL", startXB - (knobSz + knobGap) * 2, yKnobLbl, knobSz, 14, juce::Justification::centred);

    // --- Routing circuit diagram ---
    const int leftGroupRight  = startXA + (knobSz * 3) + (knobGap * 2);
    const int rightGroupLeft  = startXB - ((knobSz + knobGap) * 2);

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

    // Signal flow lines: Line A (top), Line B (bottom)
    const float lineAY = cTop + 10.0f;
    const float lineBY = cBot - 10.0f;

    auto drawSignalLineOffset = [&](float y, float tapOffsetX, bool active, juce::Colour activeColour)
        {
            const juce::Colour offColour = Nova::Colors::CableOff;
            const float tapX = xC + tapOffsetX;

            // Horizontal line
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

            // Vertical connector to switcher
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

            // Nodes
            g.setColour(active ? activeColour : offColour);
            g.fillEllipse(cLeft - 3.5f, y - 3.5f, 7.0f, 7.0f);
            g.fillEllipse(cRight - 3.5f, y - 3.5f, 7.0f, 7.0f);
            g.fillEllipse(tapX - 3.0f, y - 3.0f, 6.0f, 6.0f);
        };

    drawSignalLineOffset(lineAY, -12.0f, aActive, Nova::Colors::CableOnA);
    drawSignalLineOffset(lineBY,  12.0f, bActive, Nova::Colors::CableOnB);

    // Switcher hub (center circle)
    g.setColour(juce::Colour::fromString("ff1A2332"));
    g.fillEllipse(xC - 14.0f, switchY - 14.0f, 28.0f, 28.0f);
    g.setColour(Nova::Colors::Border);
    g.drawEllipse(xC - 14.0f, switchY - 14.0f, 28.0f, 28.0f, 1.2f);

    // Hub active indicator
    if (aActive && bActive)
        g.setColour(Nova::Colors::Accent);
    else if (aActive)
        g.setColour(Nova::Colors::CableOnA);
    else if (bActive)
        g.setColour(Nova::Colors::CableOnB);
    else
        g.setColour(Nova::Colors::CableOff);

    g.fillEllipse(xC - 6.0f, switchY - 6.0f, 12.0f, 12.0f);

    // Labels
    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    g.setColour(aActive ? Nova::Colors::CableOnA : Nova::Colors::TextDim);
    g.drawText("A", (int)(cLeft - 2.0f), (int)(lineAY - 18.0f), 14, 12, juce::Justification::centred);
    g.setColour(bActive ? Nova::Colors::CableOnB : Nova::Colors::TextDim);
    g.drawText("B", (int)(cLeft - 2.0f), (int)(lineBY + 6.0f), 14, 12, juce::Justification::centred);
}

void NOVAAudioProcessorEditor::drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    g.setColour(Nova::Colors::Background);
    g.fillRect(area);

    g.setColour(Nova::Colors::Border);
    g.drawVerticalLine(area.getRight(), (float)area.getY(), (float)area.getBottom());
    g.drawVerticalLine(area.getX(), (float)area.getY(), (float)area.getBottom());

    auto contentArea = area;

    g.setColour(Nova::Colors::Text);
    g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    g.drawText(title, contentArea.removeFromTop(40), juce::Justification::centred);

    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.setColour(Nova::Colors::TextDim);

    const bool isInput = (title == "INPUT");
    const float displayPeak = isInput ? inputMeterDisplay : outputMeterDisplay;
    const float holdPeak = isInput ? inputMeterHold : outputMeterHold;
    const float peakDb = juce::Decibels::gainToDecibels(juce::jmax(displayPeak, 0.000001f), -60.0f);
    const float holdDb = juce::Decibels::gainToDecibels(juce::jmax(holdPeak, 0.000001f), -60.0f);
    const float meterNorm = juce::jlimit(0.0f, 1.0f, (peakDb + 60.0f) / 60.0f);
    const float holdNorm = juce::jlimit(0.0f, 1.0f, (holdDb + 60.0f) / 60.0f);
    auto meterArea = juce::Rectangle<float>((float)area.getRight() - 22.0f,
        (float)area.getCentreY() - 116.0f,
        12.0f,
        232.0f);

    g.setColour(juce::Colour::fromString("ff0A1018"));
    g.fillRoundedRectangle(meterArea, 6.0f);
    g.setColour(juce::Colours::black.withAlpha(0.32f));
    g.fillRoundedRectangle(meterArea.reduced(1.0f).translated(0.0f, 1.0f), 5.0f);
    g.setColour(Nova::Colors::Border.withAlpha(0.55f));
    g.drawRoundedRectangle(meterArea, 6.0f, 1.0f);

    for (float step : { 0.1667f, 0.3333f, 0.5f, 0.6667f, 0.8333f })
    {
        const float y = meterArea.getBottom() - meterArea.getHeight() * step;
        g.setColour(Nova::Colors::Border.withAlpha(step == 0.5f ? 0.42f : 0.22f));
        g.drawHorizontalLine((int)y, meterArea.getX() + 1.0f, meterArea.getRight() - 1.0f);
    }

    if (meterNorm > 0.0f)
    {
        auto fill = meterArea.withY(meterArea.getBottom() - meterArea.getHeight() * meterNorm).reduced(1.5f, 1.5f);
        fill.setHeight(juce::jmax(4.0f, meterArea.getHeight() * meterNorm - 3.0f));

        juce::ColourGradient meterGradient(Nova::Colors::Success,
            fill.getCentreX(),
            fill.getBottom(),
            juce::Colour::fromString("ffFBBF24"),
            fill.getCentreX(),
            fill.getY(),
            false);
        if (displayPeak > 0.97f)
            meterGradient.addColour(0.0, Nova::Colors::Error);

        g.setGradientFill(meterGradient);
        g.fillRoundedRectangle(fill, 5.0f);

        auto sheen = fill;
        sheen.setHeight(juce::jmin(10.0f, fill.getHeight() * 0.22f));
        g.setColour(juce::Colours::white.withAlpha(0.14f));
        g.fillRoundedRectangle(sheen, 4.0f);
    }

    if (holdNorm > 0.0f)
    {
        const float holdY = meterArea.getBottom() - meterArea.getHeight() * holdNorm;
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.drawLine(meterArea.getX() - 1.0f, holdY, meterArea.getRight() + 1.0f, holdY, 1.8f);
    }

    auto drawLabelAbove = [&](const juce::String& txt, const juce::Component& c)
        {
            const auto b = c.getBounds();
            if (!b.isEmpty())
            {
                g.setColour(Nova::Colors::TextDim);
                g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
                g.drawText(txt.toUpperCase(), b.getX() - 10, b.getY() - 14, b.getWidth() + 20, 14, juce::Justification::centred);
            }
        };

    if (title == "INPUT")
    {
        drawLabelAbove("Gain", inputVolume);
        drawLabelAbove("Gate", inputGate);
        drawLabelAbove("Trans", inputTranspose);
    }
    else
    {
        drawLabelAbove("Master", outputVolume);
        drawLabelAbove("Limit", outputGain);
        drawLabelAbove("Mix", outputMix);
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

    constexpr int leftBrowserW = 176;
    constexpr int stripW = 120;
    constexpr int rightPresetsW = 160;

    constexpr int mixerH = 170;
    constexpr int knobSz = 60;
    constexpr int knobH = 78;  // 60 knob + 18 text
    constexpr int knobGap = 10;

    // Header
    auto header = area.removeFromTop(headerH);
    const int cx = header.getCentreX();

    btnStartStop.setBounds(cx - 64, header.getCentreY() - 10, 128, 40);
    lblStats.setBounds(cx - 100, header.getCentreY() - 35, 200, 20);
    btnTuner.setBounds(cx - 238, header.getCentreY() - 15, 40, 30);

    // Footer
    auto footer = area.removeFromBottom(footerH);
    audioProcessor.audioVisualizer.setBounds(footer);

    // Left browser column
    auto left1 = area.removeFromLeft(leftBrowserW);
    searchBarBrowser.setBounds(left1.removeFromTop(42).reduced(10, 6));
    left1.removeFromTop(96);

    const std::array<DraggableButton*, 7> quickButtons{
        &btnAddCompressor,
        &btnAddOverdrive,
        &btnAddChorus,
        &btnAddDelay,
        &btnAddReverb,
        &btnAddNeural,
        &btnAddCabinet
    };

    int buttonY = left1.getY();
    for (auto* button : quickButtons)
    {
        if (!button->isVisible())
            continue;

        button->setBounds(left1.getX() + 10, buttonY, left1.getWidth() - 20, 42);
        buttonY += 50;
    }

    // Input strip
    auto left2 = area.removeFromLeft(stripW);
    const int sideKnobW = 56;
    const int sideKnobH = 68;  // 50 knob + 18 text
    const int sideGap = 18;
    const int sideToggleH = 30;
    const int sideStackH = sideKnobH * 3 + sideGap * 2 + 14 + sideToggleH;

    auto inputKnobCol = juce::Rectangle<int>(left2.getX() + 4, left2.getY(), left2.getWidth() - 34, left2.getHeight());
    const int inputStartY = inputKnobCol.getY() + (inputKnobCol.getHeight() - sideStackH) / 2;
    const int inputKnobX = inputKnobCol.getX() + (inputKnobCol.getWidth() - sideKnobW) / 2;

    int yIn = inputStartY;
    inputVolume.setBounds(inputKnobX, yIn, sideKnobW, sideKnobH); yIn += sideKnobH + sideGap;
    inputGate.setBounds(inputKnobX, yIn, sideKnobW, sideKnobH); yIn += sideKnobH + sideGap;
    inputTranspose.setBounds(inputKnobX, yIn, sideKnobW, sideKnobH); yIn += sideKnobH + 14;
    btnMonoStereo.setBounds(inputKnobCol.getX() + 2, yIn, inputKnobCol.getWidth() - 4, sideToggleH);

    const int sideFaderW = 22;
    const int sideFaderH = 220;
    inputFader.setBounds(left2.getRight() - sideFaderW - 6,
        left2.getY() + (left2.getHeight() - sideFaderH) / 2,
        sideFaderW,
        sideFaderH);

    // Right presets column
    auto right2 = area.removeFromRight(rightPresetsW);
    searchBarPresets.setBounds(right2.removeFromTop(32).reduced(10, 4));
    lblCurrentPreset.setBounds(right2.removeFromTop(24).reduced(8, 2));
    presetSelector.setBounds(right2.removeFromTop(34).reduced(8, 2));
    right2.removeFromTop(8);

    auto btnArea = right2.removeFromBottom(96);
    auto topButtons = btnArea.removeFromTop(42);
    btnSave.setBounds(topButtons.removeFromLeft(75).reduced(5));
    btnLoad.setBounds(topButtons.reduced(5));
    btnClear.setBounds(btnArea.removeFromTop(42).reduced(5));

    // Output strip
    auto right1 = area.removeFromRight(stripW);
    auto outputKnobCol = juce::Rectangle<int>(right1.getX() + 4, right1.getY(), right1.getWidth() - 34, right1.getHeight());
    const int outStackH = sideKnobH * 3 + sideGap * 2;
    const int outputStartY = outputKnobCol.getY() + (outputKnobCol.getHeight() - outStackH) / 2;
    const int outputKnobX = outputKnobCol.getX() + (outputKnobCol.getWidth() - sideKnobW) / 2;

    int yOut = outputStartY;
    outputVolume.setBounds(outputKnobX, yOut, sideKnobW, sideKnobH); yOut += sideKnobH + sideGap;
    outputGain.setBounds(outputKnobX, yOut, sideKnobW, sideKnobH); yOut += sideKnobH + sideGap;
    outputMix.setBounds(outputKnobX, yOut, sideKnobW, sideKnobH);

    outputFader.setBounds(right1.getRight() - sideFaderW - 6,
        right1.getY() + (right1.getHeight() - sideFaderH) / 2,
        sideFaderW,
        sideFaderH);

    // Center: mixer + lanes
    auto center = area;
    auto mixerArea = center.removeFromBottom(mixerH);

    // Position the switcher button at the center of the circuit zone
    const int cKnobSz = 60;
    const int cKnobGap = 10;
    const int cStartXA = mixerArea.getX() + 30;
    const int cStartXB = mixerArea.getRight() - 30 - cKnobSz;
    const int cLeftGroupRight = cStartXA + (cKnobSz * 3) + (cKnobGap * 2);
    const int cRightGroupLeft = cStartXB - ((cKnobSz + cKnobGap) * 2);
    const int circuitCentreX = (cLeftGroupRight + cRightGroupLeft) / 2;
    btnSwitcher.setBounds(circuitCentreX - 50, mixerArea.getBottom() - 44, 100, 32);

    const int yKnobs = mixerArea.getCentreY() - (knobH / 2);
    const int startXA = mixerArea.getX() + 30;

    volSliderA.setBounds(startXA, yKnobs, knobSz, knobH);
    panSliderA.setBounds(startXA + knobSz + knobGap, yKnobs, knobSz, knobH);
    widthSliderA.setBounds(startXA + (knobSz + knobGap) * 2, yKnobs, knobSz, knobH);

    const int startXB = mixerArea.getRight() - 30 - knobSz;
    widthSliderB.setBounds(startXB, yKnobs, knobSz, knobH);
    panSliderB.setBounds(startXB - (knobSz + knobGap), yKnobs, knobSz, knobH);
    volSliderB.setBounds(startXB - (knobSz + knobGap) * 2, yKnobs, knobSz, knobH);

    const int laneH = center.getHeight() / 2;
    if (laneA) laneA->setBounds(center.removeFromTop(laneH).reduced(10));
    if (laneB) laneB->setBounds(center.reduced(10));

    updatePedalGui();
}

// ==============================================================================
// CONTROL LOGIC (sin tocar)
// ==============================================================================

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

    juce::String txt;
    if (autoHealFlashFrames > 0)
    {
        --autoHealFlashFrames;
        txt << "! AUTO-HEAL TRIGGERED !  |  CPU: " << juce::String(cpuPercent, 1) << "%";
    }
    else
    {
        txt << "CPU: " << juce::String(cpuPercent, 1) << "%"
            << "  |  Proc: " << juce::String(procTimeMs, 2) << "ms"
            << "  |  Buf: " << juce::String(bufferDurationMs, 1) << "ms";
    }

    outputFader.setValue(outputVolume.getValue(), juce::dontSendNotification);
    lblStats.setText(txt, juce::dontSendNotification);

    if (autoHealFlashFrames > 0)
        lblStats.setColour(juce::Label::textColourId, Nova::Colors::Error);
    else if (cpuPercent > 90.0)
        lblStats.setColour(juce::Label::textColourId, juce::Colours::red);
    else
        lblStats.setColour(juce::Label::textColourId, juce::Colours::grey);
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
                    std::unique_ptr<juce::AudioProcessorEditor> newEditor(item.nodeView.node->getProcessor()->createEditor());
                    if (newEditor == nullptr)
                        newEditor = std::make_unique<juce::GenericAudioProcessorEditor>(*(item.nodeView.node->getProcessor()));

                    if (newEditor != nullptr)
                    {
                        auto host = std::make_unique<PedalSlotComponent>(audioProcessor, chain, std::move(newEditor));
                        host->setPedalState(item.state);
                        addAndMakeVisible(host.get());
                        slot = host.get();
                        activePedalEditors[item.nodeView.node->nodeID] = std::move(host);
                        item.createdNow = true;
                    }
                }
                else
                {
                    slot = it->second.get();
                    slot->setPedalState(item.state);
                }

                if (slot)
                {
                    item.slot = slot;
                    item.preferredW = juce::jmax(166, slot->getPreferredWidth());
                    item.preferredH = juce::jmax(152, slot->getPreferredHeight());
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

                int widthSum = 0;
                for (auto* item : zoneItems)
                    widthSum += item->preferredW;

                int gap = 15;
                const int itemCount = (int)zoneItems.size();
                const int availableW = juce::jmax(0, zoneW - 20);
                int totalNeeded = widthSum + (itemCount - 1) * gap;

                if (itemCount > 1 && totalNeeded > availableW)
                {
                    gap = (availableW - widthSum) / (itemCount - 1);
                    gap = juce::jlimit(-40, 15, gap);
                    totalNeeded = widthSum + (itemCount - 1) * gap;
                }

                int currentX = zoneAbsX + juce::jmax(10, (zoneW - totalNeeded) / 2);
                for (auto* item : zoneItems)
                {
                    const int finalY = zoneAbsY + (zoneH - item->preferredH) / 2;
                    item->slot->setBounds(currentX, finalY, item->preferredW, item->preferredH);

                    if (item->createdNow)
                        item->slot->toFront(false);

                    currentX += item->preferredW + gap;
                }
            }
        };

    processLane(Nova::ChainID::LineA, laneA.get());
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

    if (laneA) laneA->setActive(aActive && on);
    if (laneB) laneB->setActive(bActive && on);

    // Forzar refresco de la zona central del circuito al cambiar modo/estado
    repaint();
}

void NOVAAudioProcessorEditor::requestUiRefresh()
{
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        uiRefreshPending.store(false, std::memory_order_release);
        handleAsyncUpdate();
        return;
    }

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

bool NOVAAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        audioProcessor.cycleSwitcher();
        return true;
    }
    return false;
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
