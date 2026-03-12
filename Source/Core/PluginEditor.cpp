#include "PluginEditor.h"

#include "../GUI/Widgets/ChainLane.h"
#include "../GUI/Widgets/AssetBrowserOverlay.h"
#include <algorithm>
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
    lblStats.setFont(12.0f);
    lblStats.setText("CPU: - | Latency: -", juce::dontSendNotification);

    addAndMakeVisible(btnTuner);
    addAndMakeVisible(btnMetronome);
    addAndMakeVisible(btnSettings);
    addAndMakeVisible(btnProfile);

    // IMPORTANTE: esto estaba en resized() (redundante). Aquí queda 1 vez y ya.
    btnTuner.onClick = [this] { toggleTuner(); };

    // -----------------------
    // BROWSER
    // -----------------------
    addAndMakeVisible(searchBarBrowser);
    searchBarBrowser.setTextToShowWhenEmpty("Search...", juce::Colours::grey);

    // 1. Configurar Overdrive
    addAndMakeVisible(btnAddOverdrive);
    btnAddOverdrive.setButtonText("Overdrive");
    btnAddOverdrive.setItemType("PEDAL"); // Opcional, ya es PEDAL por defecto

    // 2. Configurar el Amplificador
    addAndMakeVisible(btnAddNeural);
    btnAddNeural.setButtonText("Classic Amp"); // Cambio de nombre
    btnAddNeural.setItemType("AMP");           // Aseguramos que sea tipo AMP

    // 3. Configurar el Gabinete
    addAndMakeVisible(btnAddCabinet);
    btnAddCabinet.setButtonText("Cabinet");
    btnAddCabinet.setItemType("CAB");     // <--- ¡AQUÍ ESTÁ LA MAGIA!
    // -----------------------
    // INPUT STRIP
    // -----------------------
    setupKnob(inputVolume, "IN GAIN", -60.0f, 24.0f, 0.0f);

    setupKnob(inputGate, "GATE", -100.0f, 0.0f, -100.0f);
    inputGate.setTextValueSuffix(" dB");

    setupKnob(inputTranspose, "TRANS", -12.0f, 12.0f, 0.0f);
    inputTranspose.setRange(-12.0, 12.0, 1.0);

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
    setupKnob(volSliderA, "LEVEL A", 0.0f, 2.0f, 1.0f);

    setupKnob(panSliderA, "PAN A", -1.0f, 1.0f, 0.0f);

    setupKnob(widthSliderA, "WIDTH A", 0.0f, 2.0f, 1.0f);

    // Line B
    setupKnob(volSliderB, "LEVEL B", 0.0f, 2.0f, 1.0f);

    setupKnob(panSliderB, "PAN B", -1.0f, 1.0f, 0.0f);

    setupKnob(widthSliderB, "WIDTH B", 0.0f, 2.0f, 1.0f);

    // -----------------------
    // OUTPUT STRIP
    // -----------------------
    setupKnob(outputVolume, "MASTER", -60.0f, 12.0f, 0.0f);

    setupKnob(outputGain, "LIMIT", -20.0f, 0.0f, 0.0f);

    setupKnob(outputMix, "MIX", 0.0f, 100.0f, 100.0f);

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
    lblCurrentPreset.setFont(12.0f);
    setCurrentPreset("No Preset");

    addAndMakeVisible(presetSelector);
    presetSelector.setTextWhenNothingSelected("No Presets");
    // Seleccionar en la lista no cambia el preset activo hasta presionar LOAD.
    presetSelector.onChange = [] {};

    addAndMakeVisible(btnSave);
    btnSave.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
    btnSave.onClick = [this] { saveSelectedOrPromptPreset(); };

    addAndMakeVisible(btnLoad);
    btnLoad.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
    btnLoad.onClick = [this] { loadSelectedPreset(); };

    addAndMakeVisible(btnClear);
    btnClear.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
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

    updateSwitcherState();
    updatePedalGui();

    statsTimer = std::make_unique<StatsTimer>(*this);
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
    audioProcessor.pluginState.removeListener(this);
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
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters(juce::MathConstants<float>::pi,
        juce::MathConstants<float>::twoPi,
        true);
    slider.setLookAndFeel(knobLnf);
    slider.setRange(min, max, 0.01);
    slider.setValue(def, juce::dontSendNotification);
    slider.setTooltip(name);
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

    g.setColour(juce::Colours::white);
    g.setFont(30.0f);
    g.drawText("NOVA", headerRect.removeFromLeft(150), juce::Justification::centred);

    // Footer
    auto footerRect = area.removeFromBottom(100);
    g.setColour(Nova::Colors::Border);
    g.drawHorizontalLine(footerRect.getY(), 0, (float)getWidth());

    // Left browser column
    auto left1 = area.removeFromLeft(150);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(left1);

    g.setColour(Nova::Colors::Border);
    g.drawVerticalLine(left1.getRight(), (float)left1.getY(), (float)left1.getBottom());

    g.setColour(juce::Colours::white);
    g.setFont(14.0f);
    g.drawText("PEDALS", left1.getX(), left1.getY() + 60, left1.getWidth(), 20, juce::Justification::centred);
    g.drawText("AMPLIFIERS", left1.getX(), left1.getY() + 250, left1.getWidth(), 20, juce::Justification::centred);
    g.drawText("CABINETS", left1.getX(), left1.getY() + 400, left1.getWidth(), 20, juce::Justification::centred);

    // Input strip
    auto left2 = area.removeFromLeft(120);
    drawChannelStrip(g, left2, "INPUT");

    // Presets column
    auto right2 = area.removeFromRight(150);
    g.setColour(Nova::Colors::Panel);
    g.fillRect(right2);

    g.setColour(Nova::Colors::Border);
    g.drawVerticalLine(right2.getX(), (float)right2.getY(), (float)right2.getBottom());

    g.setColour(juce::Colours::white);
    g.drawText("PRESETS", right2.getX(), right2.getY() + 60, right2.getWidth(), 20, juce::Justification::centred);

    // Output strip
    auto right1 = area.removeFromRight(120);
    drawChannelStrip(g, right1, "OUTPUT");

    // Mixer labels
    auto center = area;
    auto mixerArea = center.removeFromBottom(150);

    g.setColour(Nova::Colors::MixerPanel);
    g.drawRoundedRectangle(mixerArea.toFloat().reduced(10), 5.0f, 1.0f);

    g.setColour(juce::Colours::white);
    g.drawText("LINE A", mixerArea.getX() + 50, mixerArea.getY() + 10, 100, 20, juce::Justification::centred);
    g.drawText("LINE B", mixerArea.getRight() - 150, mixerArea.getY() + 10, 100, 20, juce::Justification::centred);

    const bool on = audioProcessor.isEngineOn();
    const int mode = (int)audioProcessor.getSwitcherMode();
    const bool aActive = on && (mode != (int)Nova::SwitcherMode::LineB_Only);
    const bool bActive = on && (mode != (int)Nova::SwitcherMode::LineA_Only);

    const int knobSz = 60;
    const int knobGap = 10;
    const int startXA = mixerArea.getX() + 30;
    const int startXB = mixerArea.getRight() - 30 - knobSz;
    const int leftGroupRight = startXA + (knobSz * 3) + (knobGap * 2);
    const int rightGroupLeft = startXB - ((knobSz + knobGap) * 2);

    auto circuitZone = juce::Rectangle<int>(leftGroupRight + 8,
        mixerArea.getCentreY() - 36,
        juce::jmax(40, rightGroupLeft - leftGroupRight - 16),
        72);

    g.setColour(juce::Colour::fromString("ff121212"));
    g.fillRoundedRectangle(circuitZone.toFloat(), 8.0f);
    g.setColour(Nova::Colors::Border);
    g.drawRoundedRectangle(circuitZone.toFloat(), 8.0f, 1.0f);

    const float yA = (float)circuitZone.getY() + 20.0f;
    const float yB = (float)circuitZone.getBottom() - 20.0f;
    const float xL = (float)circuitZone.getX() + 8.0f;
    const float xR = (float)circuitZone.getRight() - 8.0f;
    const float xC = (float)mixerArea.getCentreX();
    const float gapHalf = 58.0f;

    const auto switchRect = juce::Rectangle<float>(xC - 50.0f,
        (float)mixerArea.getCentreY() - 30.0f,
        100.0f,
        60.0f);

    const auto selectorPlate = switchRect.expanded(14.0f, 12.0f);
    juce::ColourGradient plateGrad(juce::Colour::fromString("ff3e3e3e"), selectorPlate.getCentreX(), selectorPlate.getY(),
        juce::Colour::fromString("ff1a1a1a"), selectorPlate.getCentreX(), selectorPlate.getBottom(), false);
    g.setGradientFill(plateGrad);
    g.fillRoundedRectangle(selectorPlate, 8.0f);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawRoundedRectangle(selectorPlate, 8.0f, 1.4f);

    auto drawScrew = [&](float sx, float sy)
        {
            g.setColour(juce::Colour::fromString("ff202020"));
            g.fillEllipse(sx - 3.2f, sy - 3.2f, 6.4f, 6.4f);
            g.setColour(juce::Colour::fromString("ff6a6a6a"));
            g.drawEllipse(sx - 3.2f, sy - 3.2f, 6.4f, 6.4f, 0.8f);
            g.drawLine(sx - 1.5f, sy, sx + 1.5f, sy, 0.8f);
        };

    drawScrew(selectorPlate.getX() + 8.0f, selectorPlate.getY() + 8.0f);
    drawScrew(selectorPlate.getRight() - 8.0f, selectorPlate.getY() + 8.0f);
    drawScrew(selectorPlate.getX() + 8.0f, selectorPlate.getBottom() - 8.0f);
    drawScrew(selectorPlate.getRight() - 8.0f, selectorPlate.getBottom() - 8.0f);

    const float switchBottomY = switchRect.getBottom() + 2.0f;
    const float tapXA = xC - 18.0f;
    const float tapXB = xC + 18.0f;
    const float bridgeYA = switchBottomY + 2.0f;
    const float bridgeYB = switchBottomY + 7.0f;

    auto drawCircuit = [&](float y, float bridgeY, float tapX, bool active, juce::Colour activeColour)
        {
            const juce::Colour offColour = juce::Colour::fromString("ff2a2a2a");
            const float leftInner = xC - gapHalf;
            const float rightInner = xC + gapHalf;

            // Cables laterales hacia el dashboard
            g.setColour(offColour);
            g.drawLine(xL, y, leftInner, y, 4.0f);
            g.drawLine(rightInner, y, xR, y, 4.0f);

            // Bus central del selector, conectado al dashboard
            juce::Path busBridge;
            busBridge.startNewSubPath(leftInner, y);
            busBridge.lineTo(leftInner, bridgeY);
            busBridge.lineTo(rightInner, bridgeY);
            busBridge.lineTo(rightInner, y);
            g.strokePath(busBridge, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            juce::Path selectorFeed;
            selectorFeed.startNewSubPath(tapX, switchBottomY);
            selectorFeed.lineTo(tapX, bridgeY);
            selectorFeed.lineTo(xC, bridgeY);
            g.strokePath(selectorFeed, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (active)
            {
                g.setColour(activeColour.withAlpha(0.35f));
                g.drawLine(xL, y, leftInner, y, 7.0f);
                g.drawLine(rightInner, y, xR, y, 7.0f);
                g.strokePath(busBridge, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.strokePath(selectorFeed, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(activeColour);
                g.drawLine(xL, y, leftInner, y, 2.2f);
                g.drawLine(rightInner, y, xR, y, 2.2f);
                g.strokePath(busBridge, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.strokePath(selectorFeed, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            g.setColour((active ? activeColour : offColour).withAlpha(0.9f));
            g.fillEllipse(xL - 2.5f, y - 2.5f, 5.0f, 5.0f);
            g.fillEllipse(xR - 2.5f, y - 2.5f, 5.0f, 5.0f);
            g.fillEllipse(leftInner - 2.5f, bridgeY - 2.5f, 5.0f, 5.0f);
            g.fillEllipse(rightInner - 2.5f, bridgeY - 2.5f, 5.0f, 5.0f);
            g.fillEllipse(tapX - 2.4f, switchBottomY - 2.4f, 4.8f, 4.8f);
        };

    drawCircuit(yA, bridgeYA, tapXA, aActive, Nova::Colors::CableOnA);
    drawCircuit(yB, bridgeYB, tapXB, bActive, Nova::Colors::CableOnB);

    g.setFont(11.0f);
    g.setColour(juce::Colours::grey);
    g.drawText("CIRCUIT LINK", circuitZone.withTrimmedTop(25), juce::Justification::centredTop);

    g.setFont(10.0f);
    g.setColour(juce::Colours::grey);

    const int yLbl = mixerArea.getBottom() - 30;

    g.drawText("Level", mixerArea.getX() + 30, yLbl, 60, 20, juce::Justification::centred);
    g.drawText("Pan", mixerArea.getX() + 100, yLbl, 60, 20, juce::Justification::centred);
    g.drawText("Width", mixerArea.getX() + 170, yLbl, 60, 20, juce::Justification::centred);

    g.drawText("Level", mixerArea.getRight() - 190, yLbl, 60, 20, juce::Justification::centred);
    g.drawText("Pan", mixerArea.getRight() - 120, yLbl, 60, 20, juce::Justification::centred);
    g.drawText("Width", mixerArea.getRight() - 50, yLbl, 60, 20, juce::Justification::centred);
}

void NOVAAudioProcessorEditor::drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    g.setColour(juce::Colours::black);
    g.fillRect(area);

    g.setColour(Nova::Colors::Border);
    g.drawVerticalLine(area.getRight(), (float)area.getY(), (float)area.getBottom());
    g.drawVerticalLine(area.getX(), (float)area.getY(), (float)area.getBottom());

    auto contentArea = area;

    g.setColour(juce::Colours::white);
    g.setFont(16.0f);
    g.drawText(title, contentArea.removeFromTop(40), juce::Justification::centred);

    g.setFont(12.0f);
    g.setColour(juce::Colours::grey);

    auto drawLabelFor = [&](const juce::String& txt, const juce::Component& c)
        {
            const auto b = c.getBounds();
            if (!b.isEmpty())
                g.drawText(txt, area.getX(), b.getCentreY() - 10, area.getWidth(), 20, juce::Justification::centred);
        };

    if (title == "INPUT")
    {
        drawLabelFor("Vol", inputVolume);
        drawLabelFor("Gate", inputGate);
        drawLabelFor("Trans", inputTranspose);
    }
    else
    {
        drawLabelFor("Vol", outputVolume);
        drawLabelFor("Limit", outputGain);
        drawLabelFor("Mix", outputMix);
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

    constexpr int leftBrowserW = 150;
    constexpr int stripW = 120;
    constexpr int rightPresetsW = 150;

    constexpr int mixerH = 150;
    constexpr int knobSz = 60;
    constexpr int knobGap = 10;

    // Header
    auto header = area.removeFromTop(headerH);
    const int cx = header.getCentreX();

    btnStartStop.setBounds(cx - 60, header.getCentreY() - 10, 120, 40);
    lblStats.setBounds(cx - 100, header.getCentreY() - 35, 200, 20);

    btnMetronome.setBounds(cx - 200, header.getCentreY() - 15, 30, 30);
    btnTuner.setBounds(cx - 240, header.getCentreY() - 15, 30, 30);

    btnSettings.setBounds(cx + 160, header.getCentreY() - 15, 40, 40);
    btnProfile.setBounds(cx + 210, header.getCentreY() - 15, 60, 40);

    // Footer
    auto footer = area.removeFromBottom(footerH);
    audioProcessor.audioVisualizer.setBounds(footer);

    // Left browser column
    auto left1 = area.removeFromLeft(leftBrowserW);
    searchBarBrowser.setBounds(left1.removeFromTop(40).reduced(10, 5));
    btnAddOverdrive.setBounds(left1.getX() + 10, left1.getY() + 50, 130, 50);
    btnAddNeural.setBounds(left1.getX() + 10, left1.getY() + 240, 130, 50);
    btnAddCabinet.setBounds(left1.getX() + 10, left1.getY() + 400, 130, 50);

    // Input strip
    auto left2 = area.removeFromLeft(stripW);
    const int sideKnobW = 50;
    const int sideKnobH = 50;
    const int sideGap = 12;
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
    const int outputStartY = outputKnobCol.getY() + (outputKnobCol.getHeight() - sideStackH) / 2;
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

    btnSwitcher.setBounds(mixerArea.getCentreX() - 50, mixerArea.getCentreY() - 30, 100, 60);

    const int yKnobs = mixerArea.getCentreY() - (knobSz / 2);
    const int startXA = mixerArea.getX() + 30;

    volSliderA.setBounds(startXA, yKnobs, knobSz, knobSz);
    panSliderA.setBounds(startXA + knobSz + knobGap, yKnobs, knobSz, knobSz);
    widthSliderA.setBounds(startXA + (knobSz + knobGap) * 2, yKnobs, knobSz, knobSz);

    const int startXB = mixerArea.getRight() - 30 - knobSz;
    widthSliderB.setBounds(startXB, yKnobs, knobSz, knobSz);
    panSliderB.setBounds(startXB - (knobSz + knobGap), yKnobs, knobSz, knobSz);
    volSliderB.setBounds(startXB - (knobSz + knobGap) * 2, yKnobs, knobSz, knobSz);

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
    const bool currentState = audioProcessor.getAudioEngine().isTunerEnabled();
    const bool newState = !currentState;

    audioProcessor.getAudioEngine().setTunerEnabled(newState);

    btnTuner.setColour(juce::TextButton::buttonColourId,
        newState ? juce::Colours::green : juce::Colours::transparentBlack);

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

    juce::String txt;
    txt << "CPU: " << juce::String(cpuPercent, 1) << "%"
        << "  |  Proc: " << juce::String(procTimeMs, 2) << "ms"
        << "  |  Buf: " << juce::String(bufferDurationMs, 1) << "ms";

    outputFader.setValue(outputVolume.getValue(), juce::dontSendNotification);
    lblStats.setText(txt, juce::dontSendNotification);
    lblStats.setColour(juce::Label::textColourId, (cpuPercent > 90.0) ? juce::Colours::red : juce::Colours::grey);
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
                juce::AudioProcessorEditor* editor = nullptr;
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
                juce::AudioProcessorEditor* editor = nullptr;

                if (it == activePedalEditors.end())
                {
                    juce::AudioProcessorEditor* newEditor = item.nodeView.node->getProcessor()->createEditor();
                    if (newEditor == nullptr)
                        newEditor = new juce::GenericAudioProcessorEditor(*(item.nodeView.node->getProcessor()));

                    if (newEditor != nullptr)
                    {
                        addAndMakeVisible(newEditor);
                        activePedalEditors[item.nodeView.node->nodeID].reset(newEditor);
                        editor = newEditor;
                        item.createdNow = true;
                    }
                }
                else
                {
                    editor = it->second.get();
                }

                if (editor)
                {
                    item.editor = editor;
                    item.preferredW = juce::jmax(100, editor->getWidth());
                    item.preferredH = juce::jmax(140, editor->getHeight());
                }
            }

            for (int zoneIdx = 0; zoneIdx < 4; ++zoneIdx)
            {
                std::vector<DrawItem*> zoneItems;
                zoneItems.reserve(itemsToDraw.size());

                for (auto& item : itemsToDraw)
                    if (item.zoneIdx == zoneIdx && item.editor != nullptr)
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
                        item->editor->setBounds(finalX, finalY, item->preferredW, item->preferredH);

                        if (item->createdNow)
                            item->editor->toFront(false);
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
                    item->editor->setBounds(currentX, finalY, item->preferredW, item->preferredH);

                    if (item->createdNow)
                        item->editor->toFront(false);

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

    // Forzar refresco de la zona central del circuito al cambiar modo/estado
    repaint();
}

void NOVAAudioProcessorEditor::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& id)
{
    if (id == Nova::IDs::PEDAL_ENABLED || id == Nova::IDs::PEDAL_TYPE || id == Nova::IDs::PEDAL_ZONE)
        updatePedalGui();
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
