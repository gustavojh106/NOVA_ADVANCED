#include "PluginEditor.h"

#include "../GUI/Widgets/ChainLane.h"
#include "../GUI/Widgets/AssetBrowserOverlay.h"

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
    inputVolume.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
                .setProperty(Nova::IDs::INPUT_GAIN, (float)inputVolume.getValue(), nullptr);
        };

    setupKnob(inputGate, "GATE", -100.0f, 0.0f, -100.0f);
    inputGate.setTextValueSuffix(" dB");
    inputGate.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
                .setProperty(Nova::IDs::INPUT_GATE, (float)inputGate.getValue(), nullptr);
        };

    setupKnob(inputTranspose, "TRANS", -12.0f, 12.0f, 0.0f);
    inputTranspose.setRange(-12.0, 12.0, 1.0);
    inputTranspose.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
                .setProperty(Nova::IDs::INPUT_TRANS, (int)inputTranspose.getValue(), nullptr);
        };

    addAndMakeVisible(btnMonoStereo);
    btnMonoStereo.setButtonText("MONO");
    btnMonoStereo.setClickingTogglesState(true);
    btnMonoStereo.setColour(juce::ToggleButton::tickColourId, Nova::Colors::Accent);
    btnMonoStereo.onClick = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
                .setProperty(Nova::IDs::FORCE_MONO, btnMonoStereo.getToggleState(), nullptr);
        };

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
    volSliderA.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_A)
                .setProperty(Nova::IDs::MIXER_GAIN_A, (float)volSliderA.getValue(), nullptr);
        };

    setupKnob(panSliderA, "PAN A", -1.0f, 1.0f, 0.0f);
    panSliderA.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_A)
                .setProperty(Nova::IDs::MIXER_PAN_A, (float)panSliderA.getValue(), nullptr);
        };

    setupKnob(widthSliderA, "WIDTH A", 0.0f, 2.0f, 1.0f);
    widthSliderA.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_A)
                .setProperty(Nova::IDs::MIXER_WIDTH_A, (float)widthSliderA.getValue(), nullptr);
        };

    // Line B
    setupKnob(volSliderB, "LEVEL B", 0.0f, 2.0f, 1.0f);
    volSliderB.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_B)
                .setProperty(Nova::IDs::MIXER_GAIN_B, (float)volSliderB.getValue(), nullptr);
        };

    setupKnob(panSliderB, "PAN B", -1.0f, 1.0f, 0.0f);
    panSliderB.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_B)
                .setProperty(Nova::IDs::MIXER_PAN_B, (float)panSliderB.getValue(), nullptr);
        };

    setupKnob(widthSliderB, "WIDTH B", 0.0f, 2.0f, 1.0f);
    widthSliderB.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_B)
                .setProperty(Nova::IDs::MIXER_WIDTH_B, (float)widthSliderB.getValue(), nullptr);
        };

    // -----------------------
    // OUTPUT STRIP
    // -----------------------
    setupKnob(outputVolume, "MASTER", -60.0f, 12.0f, 0.0f);
    outputVolume.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
                .setProperty(Nova::IDs::OUTPUT_VOL, (float)outputVolume.getValue(), nullptr);
        };

    setupKnob(outputGain, "LIMIT", -20.0f, 0.0f, 0.0f);
    outputGain.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
                .setProperty(Nova::IDs::OUTPUT_LIMITER, (float)outputGain.getValue(), nullptr);
        };

    setupKnob(outputMix, "MIX", 0.0f, 100.0f, 100.0f);
    outputMix.onValueChange = [this]
        {
            audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS)
                .setProperty(Nova::IDs::OUTPUT_MIX, (float)outputMix.getValue(), nullptr);
        };

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

    // -----------------------
    // PRESETS & FOOTER
    // -----------------------
    addAndMakeVisible(searchBarPresets);
    searchBarPresets.setTextToShowWhenEmpty("Search...", juce::Colours::grey);

    addAndMakeVisible(btnSave);
    btnSave.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);

    addAndMakeVisible(btnLoad);
    btnLoad.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);

    addAndMakeVisible(audioProcessor.audioVisualizer);

    // -----------------------
    // INIT LOGIC
    // -----------------------
    audioProcessor.pluginState.addListener(this);

    setResizable(true, true);
    setSize(1920, 1080);

    // Valores iniciales
    const auto settings = audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS);
    const auto lA = audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_A);
    const auto lB = audioProcessor.pluginState.getChildWithName(Nova::IDs::LINE_B);

    if (settings.isValid())
    {
        inputVolume.setValue(settings.getProperty(Nova::IDs::INPUT_GAIN, 0.0f), juce::dontSendNotification);
        inputGate.setValue(settings.getProperty(Nova::IDs::INPUT_GATE, -100.0f), juce::dontSendNotification);
        inputTranspose.setValue(settings.getProperty(Nova::IDs::INPUT_TRANS, 0), juce::dontSendNotification);
        btnMonoStereo.setToggleState(settings.getProperty(Nova::IDs::FORCE_MONO, false), juce::dontSendNotification);
    }

    if (lA.isValid())
    {
        volSliderA.setValue(lA.getProperty(Nova::IDs::MIXER_GAIN_A, 1.0f), juce::dontSendNotification);
        panSliderA.setValue(lA.getProperty(Nova::IDs::MIXER_PAN_A, 0.0f), juce::dontSendNotification);
        widthSliderA.setValue(lA.getProperty(Nova::IDs::MIXER_WIDTH_A, 1.0f), juce::dontSendNotification);
    }

    if (lB.isValid())
    {
        volSliderB.setValue(lB.getProperty(Nova::IDs::MIXER_GAIN_B, 1.0f), juce::dontSendNotification);
        panSliderB.setValue(lB.getProperty(Nova::IDs::MIXER_PAN_B, 0.0f), juce::dontSendNotification);
        widthSliderB.setValue(lB.getProperty(Nova::IDs::MIXER_WIDTH_B, 1.0f), juce::dontSendNotification);
    }

    updateSwitcherState();
    updatePedalGui();

    statsTimer = std::make_unique<StatsTimer>(*this);
}

NOVAAudioProcessorEditor::~NOVAAudioProcessorEditor()
{
    audioProcessor.pluginState.removeListener(this);
    activePedalEditors.clear();
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
    slider.setRange(min, max, 0.01);
    slider.setValue(def, juce::dontSendNotification);
    slider.setTooltip(name);
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

    g.drawText("LINE A", mixerArea.getX() + 50, mixerArea.getY() + 10, 100, 20, juce::Justification::centred);
    g.drawText("LINE B", mixerArea.getRight() - 150, mixerArea.getY() + 10, 100, 20, juce::Justification::centred);

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

    g.setColour(juce::Colours::white);
    g.setFont(16.0f);
    g.drawText(title, area.removeFromTop(40), juce::Justification::centred);

    g.setFont(12.0f);
    g.setColour(juce::Colours::grey);
    g.drawText("Vol", area.getX(), area.getY() + 50, area.getWidth(), 20, juce::Justification::centred);

    if (title == "INPUT")
    {
        g.drawText("Gate", area.getX(), area.getY() + 110, area.getWidth(), 20, juce::Justification::centred);
        g.drawText("Trans", area.getX(), area.getY() + 170, area.getWidth(), 20, juce::Justification::centred);
    }
    else
    {
        g.drawText("Limit", area.getX(), area.getY() + 110, area.getWidth(), 20, juce::Justification::centred);
        g.drawText("Mix", area.getX(), area.getY() + 170, area.getWidth(), 20, juce::Justification::centred);
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

    // Layout constants (no cambian nada, solo legibilidad)
    constexpr int headerH = 80;
    constexpr int footerH = 100;

    constexpr int leftBrowserW = 150;
    constexpr int stripW = 120;
    constexpr int rightPresetsW = 150;

    constexpr int mixerH = 150;
    constexpr int knobSz = 60;
    constexpr int knobGap = 10;

    // -----------------------
    // HEADER
    // -----------------------
    auto header = area.removeFromTop(headerH);
    const int cx = header.getCentreX();

    btnStartStop.setBounds(cx - 60, header.getCentreY() - 10, 120, 40);
    lblStats.setBounds(cx - 100, header.getCentreY() - 35, 200, 20);

    btnMetronome.setBounds(cx - 200, header.getCentreY() - 15, 30, 30);
    btnTuner.setBounds(cx - 240, header.getCentreY() - 15, 30, 30);

    btnSettings.setBounds(cx + 160, header.getCentreY() - 15, 40, 40);
    btnProfile.setBounds(cx + 210, header.getCentreY() - 15, 60, 40);

    // -----------------------
    // FOOTER
    // -----------------------
    auto footer = area.removeFromBottom(footerH);
    audioProcessor.audioVisualizer.setBounds(footer);

    // -----------------------
    // LEFT BROWSER COLUMN
    // -----------------------
    auto left1 = area.removeFromLeft(leftBrowserW);
    searchBarBrowser.setBounds(left1.removeFromTop(40).reduced(10, 5));
    btnAddOverdrive.setBounds(left1.getX() + 10, left1.getY() + 50, 130, 50);
    btnAddNeural.setBounds(left1.getX() + 10, left1.getY() + 240, 130, 50);
    btnAddCabinet.setBounds(left1.getX() + 10, left1.getY() + 400, 130, 50);

    // -----------------------
    // INPUT STRIP
    // -----------------------
    auto left2 = area.removeFromLeft(stripW);
    auto inputArea = left2;
    inputArea.removeFromTop(60);

    inputVolume.setBounds(inputArea.removeFromTop(50).reduced(30, 0));
    inputArea.removeFromTop(10);

    inputGate.setBounds(inputArea.removeFromTop(50).reduced(30, 0));
    inputArea.removeFromTop(10);

    inputTranspose.setBounds(inputArea.removeFromTop(50).reduced(30, 0));
    inputArea.removeFromTop(20);

    btnMonoStereo.setBounds(inputArea.removeFromTop(30).reduced(10, 0));
    inputFader.setBounds(left2.getX() + 80, left2.getY() + 300, 30, 200);

    // -----------------------
    // RIGHT PRESETS COLUMN
    // -----------------------
    auto right2 = area.removeFromRight(rightPresetsW);
    searchBarPresets.setBounds(right2.removeFromTop(40).reduced(10, 5));

    auto btnArea = right2.removeFromBottom(60);
    btnSave.setBounds(btnArea.removeFromLeft(75).reduced(5));
    btnLoad.setBounds(btnArea.reduced(5));

    // -----------------------
    // OUTPUT STRIP
    // -----------------------
    auto right1 = area.removeFromRight(stripW);
    auto outputArea = right1;
    outputArea.removeFromTop(60);

    outputVolume.setBounds(outputArea.removeFromTop(50).reduced(30, 0));
    outputArea.removeFromTop(10);

    outputGain.setBounds(outputArea.removeFromTop(50).reduced(30, 0));
    outputArea.removeFromTop(10);

    outputMix.setBounds(outputArea.removeFromTop(50).reduced(30, 0));
    outputFader.setBounds(right1.getX() + 10, right1.getY() + 300, 30, 200);

    // -----------------------
    // CENTER: MIXER + LANES
    // -----------------------
    auto center = area;
    auto mixerArea = center.removeFromBottom(mixerH);

    btnSwitcher.setBounds(mixerArea.getCentreX() - 50, mixerArea.getCentreY() - 30, 100, 60);

    const int yKnobs = mixerArea.getCentreY() - 10;
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

    lblStats.setText(txt, juce::dontSendNotification);
    lblStats.setColour(juce::Label::textColourId, (cpuPercent > 90.0) ? juce::Colours::red : juce::Colours::grey);
}

void NOVAAudioProcessorEditor::updatePedalGui()
{
    std::set<juce::AudioProcessorGraph::NodeID> requiredNodeIDs;

    auto processLane = [&](Nova::ChainID chain, ChainLane* laneComp)
        {
            if (!laneComp) return;
            laneComp->resized();

            const auto& engineNodes = audioProcessor.getAudioEngine().getNodes(chain);
            const auto treeListID = (chain == Nova::ChainID::LineA) ? Nova::IDs::LINE_A : Nova::IDs::LINE_B;
            auto treeList = audioProcessor.pluginState.getChildWithName(treeListID);

            // Hacemos una copia temporal de los nodos generados por el motor de audio
            juce::Array<juce::AudioProcessorGraph::Node::Ptr> availableNodes;
            for (auto n : engineNodes) availableNodes.add(n);

            int zoneCounts[4] = { 0, 0, 0, 0 };

            // ==============================================================================
            // 1. EMPAREJAMIENTO INTELIGENTE (Self-Healing)
            // ==============================================================================
            struct MatchedItem {
                juce::ValueTree state;
                juce::AudioProcessorGraph::Node::Ptr node;
                int zoneIdx;
            };
            std::vector<MatchedItem> itemsToDraw;

            for (int i = 0; i < treeList.getNumChildren(); ++i)
            {
                auto state = treeList.getChild(i);
                if (state.getType() != Nova::IDs::PEDAL) continue;

                juce::String expectedType = state.getProperty(Nova::IDs::PEDAL_TYPE).toString();
                int zIdx = static_cast<int>(state.getProperty(Nova::IDs::PEDAL_ZONE, 0));

                juce::AudioProcessorGraph::Node::Ptr matchedNode = nullptr;

                // Buscamos a su "alma gemela" en los nodos de audio comparando los nombres
                for (int j = 0; j < availableNodes.size(); ++j)
                {
                    auto n = availableNodes[j];
                    if (n && n->getProcessor())
                    {
                        juce::String procName = n->getProcessor()->getName();

                        // Si coinciden lógicamente, los emparejamos
                        if (procName.containsIgnoreCase(expectedType) || expectedType.containsIgnoreCase(procName))
                        {
                            matchedNode = n;
                            availableNodes.remove(j); // Lo sacamos de la lista para no repetirlo
                            break;
                        }
                    }
                }

                // Fallback: Si el nombre está raro, simplemente agarramos el primer nodo libre que coincida en orden
                if (!matchedNode && availableNodes.size() > 0)
                {
                    matchedNode = availableNodes[0];
                    availableNodes.remove(0);
                }

                // Si logramos emparejarlo (es decir, el motor SÍ logró cargar este código viejo sin explotar)
                if (matchedNode)
                {
                    if (zIdx >= 0 && zIdx < 4)
                    {
                        zoneCounts[zIdx]++;
                        itemsToDraw.push_back({ state, matchedNode, zIdx });
                    }
                }
            }

            // ==============================================================================
            // 2. DIBUJO, LAYOUT Y SALVAGUARDA DE INTERFAZ
            // ==============================================================================
            int flowCounters[4] = { 0, 0, 0, 0 };

            for (const auto& item : itemsToDraw)
            {
                requiredNodeIDs.insert(item.node->nodeID);
                const int zoneIdx = item.zoneIdx;

                const auto zoneRect = laneComp->getZoneRect(zoneIdx);
                const int zoneAbsX = laneComp->getX() + zoneRect.getX();
                const int zoneAbsY = laneComp->getY() + zoneRect.getY();
                const int zoneW = zoneRect.getWidth();
                const int zoneH = zoneRect.getHeight();

                const int pW = 120, pH = 180;
                const int finalY = zoneAbsY + (zoneH - pH) / 2;
                int finalX = 0;

                if (zoneIdx == (int)Nova::ZoneID::Amp || zoneIdx == (int)Nova::ZoneID::Cabinet)
                {
                    finalX = zoneAbsX + (zoneW - pW) / 2;
                }
                else
                {
                    int count = zoneCounts[zoneIdx];
                    int current = flowCounters[zoneIdx];
                    int gap = 15;

                    int totalNeeded = (count * pW) + ((count - 1) * gap);
                    if (totalNeeded > zoneW - 20 && count > 1)
                    {
                        gap = (zoneW - 20 - (count * pW)) / (count - 1);
                        totalNeeded = (count * pW) + ((count - 1) * gap);
                    }

                    int startX = zoneAbsX + (zoneW - totalNeeded) / 2;
                    finalX = startX + (current * (pW + gap));
                }

                juce::AudioProcessorEditor* editor = nullptr;
                auto it = activePedalEditors.find(item.node->nodeID);

                if (it == activePedalEditors.end())
                {
                    // EL SALVAVIDAS: Intentamos crear su ventana normal
                    juce::AudioProcessorEditor* newEditor = item.node->getProcessor()->createEditor();

                    // Si es código ultra viejo y devuelve nullptr, le forzamos una UI genérica de JUCE
                    if (newEditor == nullptr)
                    {
                        newEditor = new juce::GenericAudioProcessorEditor(*(item.node->getProcessor()));
                    }

                    if (newEditor != nullptr)
                    {
                        addAndMakeVisible(newEditor);
                        activePedalEditors[item.node->nodeID].reset(newEditor);
                        editor = newEditor;
                    }
                }
                else editor = it->second.get();

                if (editor)
                {
                    editor->setBounds(finalX, finalY, pW, pH);
                    editor->toFront(false);
                }

                flowCounters[zoneIdx]++;
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

    if (currentOverlay) currentOverlay->toFront(true);
    if (tunerOverlay && tunerOverlay->isVisible()) tunerOverlay->toFront(true);
}

void NOVAAudioProcessorEditor::updateSwitcherState()
{
    auto s = audioProcessor.pluginState.getChildWithName(Nova::IDs::SETTINGS);
    if (!s.isValid()) return;

    const bool on = s.getProperty(Nova::IDs::ENGINE_ON);
    const int mode = s.getProperty(Nova::IDs::SWITCH_MODE);

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
