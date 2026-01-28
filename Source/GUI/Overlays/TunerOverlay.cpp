#include "TunerOverlay.h"

// ==============================================================================
// HELPERS MATEMÁTICOS
// ==============================================================================
float TunerOverlay::getMedianCents(float newVal)
{
    medianBuffer.push_back(newVal);
    if (medianBuffer.size() > MEDIAN_SIZE) medianBuffer.pop_front();
    std::vector<float> temp(medianBuffer.begin(), medianBuffer.end());
    std::sort(temp.begin(), temp.end());
    if (temp.empty()) return 0.0f;
    return temp[temp.size() / 2];
}

float TunerOverlay::getSmoothedCents(float newCents)
{
    centsHistory[historyIndex] = newCents;
    historyIndex = (historyIndex + 1) % SMOOTHING_BUFFER_SIZE;
    float sum = 0.0f;
    for (float val : centsHistory) sum += val;
    return sum / (float)SMOOTHING_BUFFER_SIZE;
}

// ==============================================================================
// IMPLEMENTACIÓN
// ==============================================================================

TunerOverlay::TunerOverlay(NOVAAudioProcessor& p) : processor(p)
{
    initPresets();
    centsHistory.resize(SMOOTHING_BUFFER_SIZE, 0.0f);

    // Botón Cerrar
    addAndMakeVisible(closeButton);
    closeButton.setButtonText("X");
    closeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    // Usamos una lambda segura que llama al processor helper
    closeButton.onClick = [this] {
        // Opción A: Llamar a un método público del padre si tenemos puntero
        // Opción B (Más desacoplada): Simplemente hacerse invisible o pedir al processor que cierre el estado
        // Como tu lógica actual depende de toggleTuner() en el Editor,
        // necesitamos acceder al AudioEngine para apagar el tuner state.

        processor.getAudioEngine().setTunerEnabled(false);
        this->setVisible(false);
        // Nota: El Editor principal detectará el cambio de estado en el timer o repaint si es necesario,
        // o idealmente usamos un callback. Por simplicidad ahora, replicamos la lógica inversa.
        };

    // Selector de Afinación
    addAndMakeVisible(tuningSelector);
    tuningSelector.setJustificationType(juce::Justification::centred);
    tuningSelector.setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromString("ff202020"));
    tuningSelector.setColour(juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha(0.2f));
    for (int i = 0; i < presets.size(); ++i) tuningSelector.addItem(presets[i].name, i + 1);

    tuningSelector.setSelectedId(1);
    tuningSelector.onChange = [this] {
        currentPresetIndex = tuningSelector.getSelectedId() - 1;
        selectString(0);
        repaint();
        };

    // Botones de Éxito
    addAndMakeVisible(resetButton);
    resetButton.setButtonText("TUNE AGAIN");
    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    resetButton.onClick = [this] {
        std::fill(stringIsTuned.begin(), stringIsTuned.end(), false);
        startPlayingButton.setVisible(false);
        resetButton.setVisible(false);
        selectString(0);
        };
    resetButton.setVisible(false);

    addAndMakeVisible(startPlayingButton);
    startPlayingButton.setButtonText("START PLAYING");
    startPlayingButton.setColour(juce::TextButton::buttonColourId, juce::Colours::green);
    startPlayingButton.onClick = [this] {
        processor.getAudioEngine().setTunerEnabled(false);
        this->setVisible(false);
        };
    startPlayingButton.setVisible(false);

    stringIsTuned.resize(8, false);
    startTimerHz(60);
}

TunerOverlay::~TunerOverlay() {}

void TunerOverlay::initPresets()
{
    presets.push_back({ "Standard (E)", {"E", "A", "D", "G", "B", "e"}, {82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f} });
    presets.push_back({ "Drop D", {"D", "A", "D", "G", "B", "e"}, {73.42f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f} });
    presets.push_back({ "Eb Standard", {"Eb", "Ab", "Db", "Gb", "Bb", "eb"}, {77.78f, 103.83f, 138.59f, 185.00f, 233.08f, 311.13f} });
    presets.push_back({ "7-String Std (B)", {"B", "E", "A", "D", "G", "B", "e"}, {61.74f, 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f} });
    presets.push_back({ "8-String Std (F#)", {"F#", "B", "E", "A", "D", "G", "B", "e"}, {46.25f, 61.74f, 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f} });
}

void TunerOverlay::selectString(int index)
{
    if (index < 0 || index >= presets[currentPresetIndex].frequencies.size()) return;
    currentStringIndex = index;
    isTuningComplete = false;
    tuningProgress = 0.0f;
    tuningSelector.setVisible(true);
    std::fill(centsHistory.begin(), centsHistory.end(), 0.0f);
    medianBuffer.clear();
}

void TunerOverlay::timerCallback()
{
    // Si el componente no es visible, no procesamos (Ahorro CPU UI)
    if (!isVisible()) return;

    if (isTuningComplete) { repaint(); return; }

    auto& engine = processor.getAudioEngine();
    float detectedFreq = engine.getTunerPitch();
    float clarity = engine.getTunerClarity();
    float rms = engine.getTunerRMS();

    float targetFreq = presets[currentPresetIndex].frequencies[currentStringIndex];
    float rawCentsError = 0.0f;
    bool signalIsValid = false;
    float dynamicThreshold = (clarity > 0.95f) ? 0.0001f : 0.002f;

    if (detectedFreq > 20.0f && rms > dynamicThreshold && clarity > 0.85f)
    {
        float tempFreq = detectedFreq;
        while (tempFreq > targetFreq * 1.55f) tempFreq *= 0.5f;
        while (tempFreq < targetFreq * 0.74f) tempFreq *= 2.0f;

        rawCentsError = 1200.0f * std::log2(tempFreq / targetFreq);
        rawCentsError = juce::jlimit(-50.0f, 50.0f, rawCentsError);
        signalIsValid = true;
    }

    float filteredCents = getMedianCents(signalIsValid ? rawCentsError : 0.0f);

    // Lógica SOTA de Progreso
    const float TOLERANCE = 6.0f;
    const float PERFECT_TOLERANCE = 2.0f;
    bool isInGreenZone = std::abs(filteredCents) <= TOLERANCE && signalIsValid;
    bool isPerfect = std::abs(filteredCents) <= PERFECT_TOLERANCE && signalIsValid;

    if (isInGreenZone) {
        float fillSpeed = isPerfect ? 1.5f : 0.8f;
        if (clarity > 0.98f) fillSpeed += 0.3f;
        tuningProgress += fillSpeed;
    }
    else {
        if (signalIsValid) {
            if (std::abs(filteredCents) >= 15.0f) tuningProgress -= 2.0f;
        }
        else {
            tuningProgress -= 0.15f;
        }
    }

    tuningProgress = juce::jlimit(0.0f, 100.0f, tuningProgress);

    // Check Finalización
    if (tuningProgress >= 100.0f)
    {
        stringIsTuned[currentStringIndex] = true;
        bool allDone = true;
        int numStrings = presets[currentPresetIndex].stringNames.size();
        for (int i = 0; i < numStrings; ++i) if (!stringIsTuned[i]) allDone = false;

        if (allDone) {
            isTuningComplete = true;
            resetButton.setVisible(true);
            startPlayingButton.setVisible(true);
            tuningSelector.setVisible(false);
        }
        else {
            // Auto Advance
            for (int i = 0; i < numStrings; ++i) {
                int next = (currentStringIndex + 1 + i) % numStrings;
                if (!stringIsTuned[next]) { selectString(next); break; }
            }
        }
    }

    // Physics Visuales
    currentDisplayCents += (filteredCents - currentDisplayCents) * 0.2f;
    smoothedRMS += (rms - smoothedRMS) * 0.1f;
    repaint();
}

void TunerOverlay::paint(juce::Graphics& g)
{
    g.fillAll(Nova::Colors::Background.withAlpha(0.98f)); // Fondo casi sólido
    auto bounds = getLocalBounds();
    auto center = bounds.getCentre();

    if (isTuningComplete) {
        if (resetButton.isVisible()) {
            g.setColour(Nova::Colors::Text);
            g.drawRect(resetButton.getBounds().expanded(2), 2.0f);
        }
        g.setColour(Nova::Colors::Accent);
        g.setFont(juce::Font(60.0f, juce::Font::bold));
        g.drawText("GUITAR TUNED!", bounds.removeFromTop(bounds.getHeight() / 2), juce::Justification::centred);
        return;
    }

    // Barra de Cuerdas
    int numStrings = presets[currentPresetIndex].stringNames.size();
    if (numStrings > 0) {
        int btnWidth = stringBarArea.getWidth() / numStrings;
        for (int i = 0; i < numStrings; ++i) {
            auto rect = juce::Rectangle<int>(stringBarArea.getX() + i * btnWidth, stringBarArea.getY(), btnWidth, stringBarArea.getHeight()).reduced(5);
            bool isSelected = (i == currentStringIndex);

            // Barra de Progreso individual
            if (isSelected && tuningProgress > 0.0f) {
                g.setColour(Nova::Colors::Accent.withAlpha(0.3f));
                g.fillRoundedRectangle(rect.getX(), rect.getY(), rect.getWidth() * (tuningProgress / 100.0f), rect.getHeight(), 4.0f);
            }

            // Contorno
            juce::Colour c = stringIsTuned[i] ? Nova::Colors::Accent : (isSelected ? Nova::Colors::Text : Nova::Colors::TextDim.withAlpha(0.5f));
            g.setColour(c);
            g.drawRoundedRectangle(rect.toFloat(), 4.0f, isSelected ? 2.0f : 1.0f);
            g.setFont(juce::Font(22.0f, isSelected ? juce::Font::bold : juce::Font::plain));
            g.drawText(presets[currentPresetIndex].stringNames[i], rect, juce::Justification::centred);
        }
    }

    // Medidor Central
    bool hasSignal = (smoothedRMS > 0.002f);
    juce::Colour statusColor = Nova::Colors::Error;
    if (std::abs(currentDisplayCents) < 15.0f) statusColor = juce::Colours::orange;
    if (std::abs(currentDisplayCents) <= 6.0f) statusColor = Nova::Colors::Accent;
    if (!hasSignal) statusColor = Nova::Colors::TextDim;

    // Nota Gigante
    g.setColour(statusColor);
    g.setFont(juce::Font(100.0f, juce::Font::bold));
    g.drawText(presets[currentPresetIndex].stringNames[currentStringIndex], bounds.removeFromTop(bounds.getHeight() / 2), juce::Justification::centredBottom);

    // Aguja
    int barW = 500; int barY = center.getY() + 50; int barX = center.getX() - (barW / 2);
    g.setColour(juce::Colours::white.withAlpha(0.1f));
    g.fillRoundedRectangle((float)barX, (float)barY, (float)barW, 8.0f, 4.0f);
    g.setColour(Nova::Colors::Text);
    g.drawVerticalLine(center.getX(), (float)barY - 15, (float)barY + 23);

    if (hasSignal) {
        float pxPerCent = (float)barW / 100.0f;
        float needleX = center.getX() + (currentDisplayCents * pxPerCent);
        needleX = juce::jlimit((float)barX, (float)(barX + barW), needleX);

        g.setColour(statusColor);
        g.fillEllipse(needleX - 10, barY - 6, 20, 20);
    }
}

void TunerOverlay::resized()
{
    auto area = getLocalBounds();
    closeButton.setBounds(area.getRight() - 50, area.getY() + 10, 40, 40);
    tuningSelector.setBounds(area.getCentreX() - 100, area.getBottom() - 50, 200, 30);
    stringBarArea = juce::Rectangle<int>(area.getX() + 50, area.getBottom() - 120, area.getWidth() - 100, 50);

    int btnW = 160; int btnH = 40; int gap = 20;
    int startY = area.getBottom() - 100; int centerX = area.getCentreX();
    startPlayingButton.setBounds(centerX + gap / 2, startY, btnW, btnH);
    resetButton.setBounds(centerX - btnW - gap / 2, startY, btnW, btnH);
}

void TunerOverlay::mouseUp(const juce::MouseEvent& e)
{
    if (stringBarArea.contains(e.getPosition()) && !isTuningComplete) {
        int numStrings = presets[currentPresetIndex].stringNames.size();
        int btnWidth = stringBarArea.getWidth() / numStrings;
        int index = (e.x - stringBarArea.getX()) / btnWidth;
        selectString(index);
        repaint();
    }
}