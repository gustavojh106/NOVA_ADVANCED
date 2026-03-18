#include "TunerOverlay.h"
#include <algorithm>

// -----------------------------------------------------------------------------
// Filtering
// -----------------------------------------------------------------------------
float TunerOverlay::pushMedian(float newVal)
{
    medianBuffer.push_back(newVal);
    if ((int)medianBuffer.size() > kMedianSize)
        medianBuffer.pop_front();

    std::vector<float> tmp(medianBuffer.begin(), medianBuffer.end());
    if (tmp.empty())
        return 0.0f;

    std::sort(tmp.begin(), tmp.end());
    return tmp[tmp.size() / 2];
}

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------
TunerOverlay::TunerOverlay(NOVAAudioProcessor& p)
    : processor(p)
{
    initPresets();

    // Close button
    addAndMakeVisible(closeButton);
    closeButton.setButtonText("X");
    closeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    closeButton.onClick = [this] { closeOverlayAndDisableTuner(); };

    // Tuning preset selector
    addAndMakeVisible(tuningSelector);
    tuningSelector.setJustificationType(juce::Justification::centred);
    tuningSelector.setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromString("ff111827"));
    tuningSelector.setColour(juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha(0.2f));

    for (int i = 0; i < (int)presets.size(); ++i)
        tuningSelector.addItem(presets[i].name, i + 1);

    tuningSelector.setSelectedId(1);
    tuningSelector.onChange = [this]
        {
            currentPresetIndex = tuningSelector.getSelectedId() - 1;
            selectString(0);
            repaint();
        };

    // Reset / Start buttons (shown only when completed)
    addAndMakeVisible(resetButton);
    resetButton.setButtonText("TUNE AGAIN");
    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    resetButton.onClick = [this]
        {
            std::fill(stringIsTuned.begin(), stringIsTuned.end(), false);
            startPlayingButton.setVisible(false);
            resetButton.setVisible(false);
            selectString(0);
        };
    resetButton.setVisible(false);

    addAndMakeVisible(startPlayingButton);
    startPlayingButton.setButtonText("START PLAYING");
    startPlayingButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromString("ff154a28"));
    startPlayingButton.onClick = [this] { closeOverlayAndDisableTuner(); };
    startPlayingButton.setVisible(false);

    stringIsTuned.resize(8, false);

    selectString(0);
    startTimerHz(60);
}

TunerOverlay::~TunerOverlay() = default;

// -----------------------------------------------------------------------------
// Presets
// -----------------------------------------------------------------------------
void TunerOverlay::initPresets()
{
    presets.push_back({ "Standard (E)",
        { "E", "A", "D", "G", "B", "e" },
        { 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f } });

    presets.push_back({ "Drop D",
        { "D", "A", "D", "G", "B", "e" },
        { 73.42f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f } });

    presets.push_back({ "Eb Standard",
        { "Eb", "Ab", "Db", "Gb", "Bb", "eb" },
        { 77.78f, 103.83f, 138.59f, 185.00f, 233.08f, 311.13f } });

    presets.push_back({ "7-String Std (B)",
        { "B", "E", "A", "D", "G", "B", "e" },
        { 61.74f, 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f } });

    presets.push_back({ "8-String Std (F#)",
        { "F#", "B", "E", "A", "D", "G", "B", "e" },
        { 46.25f, 61.74f, 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f } });
}

void TunerOverlay::selectString(int index)
{
    if (presets.empty())
        return;

    const auto& freqs = presets[currentPresetIndex].frequencies;
    if (index < 0 || index >= (int)freqs.size())
        return;

    currentStringIndex = index;
    isTuningComplete = false;
    tuningProgress = 0.0f;

    tuningSelector.setVisible(true);

    medianBuffer.clear();
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
void TunerOverlay::closeOverlayAndDisableTuner()
{
    processor.getAudioEngine().setTunerEnabled(false);
    setVisible(false);
}

void TunerOverlay::updateCompletionUI(bool allDone)
{
    if (!allDone)
        return;

    isTuningComplete = true;
    resetButton.setVisible(true);
    startPlayingButton.setVisible(true);
    tuningSelector.setVisible(false);
}

// -----------------------------------------------------------------------------
// Timer
// -----------------------------------------------------------------------------
void TunerOverlay::timerCallback()
{
    if (!isVisible())
        return;

    if (isTuningComplete)
    {
        repaint();
        return;
    }

    auto& engine = processor.getAudioEngine();

    const float detectedFreq = engine.getTunerPitch();
    const float clarity = engine.getTunerClarity();
    const float rms = engine.getTunerRMS();

    const float targetFreq = presets[currentPresetIndex].frequencies[currentStringIndex];

    float rawCentsError = 0.0f;
    bool signalIsValid = false;

    const float dynamicThreshold = (clarity > 0.95f) ? 0.0001f : 0.002f;

    if (detectedFreq > 20.0f && rms > dynamicThreshold && clarity > 0.85f)
    {
        float tempFreq = detectedFreq;

        while (tempFreq > targetFreq * 1.55f) tempFreq *= 0.5f;
        while (tempFreq < targetFreq * 0.74f) tempFreq *= 2.0f;

        rawCentsError = 1200.0f * std::log2(tempFreq / targetFreq);
        rawCentsError = juce::jlimit(-50.0f, 50.0f, rawCentsError);

        signalIsValid = true;
    }

    const float filteredCents = pushMedian(signalIsValid ? rawCentsError : 0.0f);

    // Progress logic
    static constexpr float kTolerance = 6.0f;
    static constexpr float kPerfectTolerance = 2.0f;

    const bool inGreen = signalIsValid && (std::abs(filteredCents) <= kTolerance);
    const bool perfect = signalIsValid && (std::abs(filteredCents) <= kPerfectTolerance);

    if (inGreen)
    {
        float fillSpeed = perfect ? 1.5f : 0.8f;
        if (clarity > 0.98f) fillSpeed += 0.3f;
        tuningProgress += fillSpeed;
    }
    else
    {
        if (signalIsValid)
        {
            if (std::abs(filteredCents) >= 15.0f)
                tuningProgress -= 2.0f;
        }
        else
        {
            tuningProgress -= 0.15f;
        }
    }

    tuningProgress = juce::jlimit(0.0f, 100.0f, tuningProgress);

    // Completion check
    if (tuningProgress >= 100.0f)
    {
        stringIsTuned[currentStringIndex] = true;

        const int numStrings = (int)presets[currentPresetIndex].stringNames.size();

        bool allDone = true;
        for (int i = 0; i < numStrings; ++i)
            if (!stringIsTuned[i]) allDone = false;

        if (allDone)
        {
            updateCompletionUI(true);
        }
        else
        {
            for (int i = 0; i < numStrings; ++i)
            {
                const int next = (currentStringIndex + 1 + i) % numStrings;
                if (!stringIsTuned[next])
                {
                    selectString(next);
                    break;
                }
            }
        }
    }

    // Visual smoothing
    currentDisplayCents += (filteredCents - currentDisplayCents) * 0.2f;
    smoothedRMS += (rms - smoothedRMS) * 0.1f;

    repaint();
}

// -----------------------------------------------------------------------------
// Paint
// -----------------------------------------------------------------------------
void TunerOverlay::paint(juce::Graphics& g)
{
    g.fillAll(Nova::Colors::Background.withAlpha(0.98f));

    const auto bounds = getLocalBounds();
    const auto center = bounds.getCentre();

    if (isTuningComplete)
    {
        if (resetButton.isVisible())
        {
            g.setColour(Nova::Colors::Text);
            g.drawRect(resetButton.getBounds().expanded(2), 2);
        }

        g.setColour(Nova::Colors::Success);
        g.setFont(juce::Font(juce::FontOptions(60.0f, juce::Font::bold)));
        g.drawText("GUITAR TUNED!", bounds.withHeight(bounds.getHeight() / 2), juce::Justification::centred);
        return;
    }

    // String bar
    const int numStrings = (int)presets[currentPresetIndex].stringNames.size();
    if (numStrings > 0)
    {
        const int btnWidth = stringBarArea.getWidth() / numStrings;

        for (int i = 0; i < numStrings; ++i)
        {
            auto rect = juce::Rectangle<int>(
                stringBarArea.getX() + i * btnWidth,
                stringBarArea.getY(),
                btnWidth,
                stringBarArea.getHeight()).reduced(5);

            const bool isSelected = (i == currentStringIndex);

            if (isSelected && tuningProgress > 0.0f)
            {
                g.setColour(Nova::Colors::Success.withAlpha(0.3f));
                g.fillRoundedRectangle((float)rect.getX(), (float)rect.getY(),
                    (float)rect.getWidth() * (tuningProgress / 100.0f),
                    (float)rect.getHeight(), 4.0f);
            }

            const juce::Colour c = stringIsTuned[i]
                ? Nova::Colors::Success
                : (isSelected ? Nova::Colors::Text : Nova::Colors::TextDim.withAlpha(0.5f));

            g.setColour(c);
            g.drawRoundedRectangle(rect.toFloat(), 4.0f, isSelected ? 2.0f : 1.0f);

            g.setFont(juce::Font(juce::FontOptions(22.0f, isSelected ? juce::Font::bold : juce::Font::plain)));
            g.drawText(presets[currentPresetIndex].stringNames[i], rect, juce::Justification::centred);
        }
    }

    // Center meter
    const bool hasSignal = (smoothedRMS > 0.002f);

    juce::Colour statusColor = Nova::Colors::Error;
    if (std::abs(currentDisplayCents) < 15.0f) statusColor = Nova::Colors::Accent;
    if (std::abs(currentDisplayCents) <= 6.0f) statusColor = Nova::Colors::Success;
    if (!hasSignal) statusColor = Nova::Colors::TextDim;

    // Giant note
    g.setColour(statusColor);
    g.setFont(juce::Font(juce::FontOptions(100.0f, juce::Font::bold)));
    g.drawText(presets[currentPresetIndex].stringNames[currentStringIndex],
        bounds.withHeight(bounds.getHeight() / 2),
        juce::Justification::centredBottom);

    // Needle bar
    const int barW = 500;
    const int barY = center.getY() + 50;
    const int barX = center.getX() - (barW / 2);

    g.setColour(juce::Colours::white.withAlpha(0.1f));
    g.fillRoundedRectangle((float)barX, (float)barY, (float)barW, 8.0f, 4.0f);

    g.setColour(Nova::Colors::Text);
    g.drawVerticalLine(center.getX(), (float)barY - 15.0f, (float)barY + 23.0f);

    if (hasSignal)
    {
        const float pxPerCent = (float)barW / 100.0f;
        float needleX = (float)center.getX() + (currentDisplayCents * pxPerCent);
        needleX = juce::jlimit((float)barX, (float)(barX + barW), needleX);

        g.setColour(statusColor);
        g.fillEllipse(needleX - 10.0f, (float)barY - 6.0f, 20.0f, 20.0f);
    }
}

// -----------------------------------------------------------------------------
// Layout / interaction
// -----------------------------------------------------------------------------
void TunerOverlay::resized()
{
    auto area = getLocalBounds();

    closeButton.setBounds(area.getRight() - 50, area.getY() + 10, 40, 40);
    tuningSelector.setBounds(area.getCentreX() - 100, area.getBottom() - 50, 200, 30);

    stringBarArea = juce::Rectangle<int>(
        area.getX() + 50,
        area.getBottom() - 120,
        area.getWidth() - 100,
        50);

    const int btnW = 160;
    const int btnH = 40;
    const int gap = 20;

    const int startY = area.getBottom() - 100;
    const int centerX = area.getCentreX();

    startPlayingButton.setBounds(centerX + gap / 2, startY, btnW, btnH);
    resetButton.setBounds(centerX - btnW - gap / 2, startY, btnW, btnH);
}

void TunerOverlay::mouseUp(const juce::MouseEvent& e)
{
    if (isTuningComplete)
        return;

    if (!stringBarArea.contains(e.getPosition()))
        return;

    const int numStrings = (int)presets[currentPresetIndex].stringNames.size();
    if (numStrings <= 0)
        return;

    const int btnWidth = stringBarArea.getWidth() / numStrings;
    const int index = (e.x - stringBarArea.getX()) / btnWidth;

    selectString(index);
    repaint();
}
