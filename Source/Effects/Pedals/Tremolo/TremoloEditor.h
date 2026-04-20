#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

#include <array>
#include <cmath>

class TremoloPedal;

class TremoloEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    TremoloEditor(TremoloPedal& pedal);
    ~TremoloEditor() override
    {
        stopTimer();
        for (auto* slider : allSliders())
            slider->setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintLfoViz(juce::Graphics& g, juce::Rectangle<float> bounds);

    std::array<juce::Slider*, 9> allSliders()
    {
        return {
            &sldRate, &sldDepth, &sldShape, &sldBias, &sldStereo,
            &sldHarmonic, &sldCrossover, &sldMix, &sldLevel
        };
    }

    TremoloPedal& proc;

    static constexpr int kWidth = 780;
    static constexpr int kHeight = 470;

    const juce::Colour accent = juce::Colour::fromString("ffFBBF24");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFDE68A");
    const juce::Colour bgDark = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim = juce::Colour(0xff7B8BA0);

    juce::Slider sldRate, sldDepth, sldShape, sldBias, sldStereo, sldHarmonic, sldCrossover, sldMix, sldLevel;
    juce::Label lblRate, lblDepth, lblShape, lblBias, lblStereo, lblHarmonic, lblCrossover, lblMix, lblLevel;
    juce::Label valRate, valDepth, valShape, valBias, valStereo, valHarmonic, valCrossover, valMix, valLevel;

    juce::Rectangle<float> vizBounds;

    struct TremoloKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent = juce::Colour::fromString("ffFBBF24");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffFDE68A");

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
            float sliderPos, float startAngle, float endAngle, juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(6.0f);
            const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float centreX = area.getCentreX();
            const float centreY = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcRadius = radius - 4.0f;

            juce::Path track;
            track.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff1E2A3A));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.56f;
            juce::ColourGradient gradient(juce::Colour(0xff1C2838), centreX, centreY - knobRadius,
                juce::Colour(0xff0D1520), centreX, centreY + knobRadius, false);
            g.setGradientFill(gradient);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2A3A4C));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.70f;
            const float pointerX = centreX + std::sin(angle) * pointerLength;
            const float pointerY = centreY - std::cos(angle) * pointerLength;
            g.setColour(kAccentGlow);
            g.drawLine(centreX, centreY, pointerX, pointerY, 2.0f);
            g.fillEllipse(pointerX - 3.0f, pointerY - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TremoloEditor)
};

inline TremoloEditor::TremoloEditor(TremoloPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& slider, juce::Label& label, juce::Label& value,
        const juce::String& name, float minimum, float maximum, float def, float step = 0.01f)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider.setRange(minimum, maximum, step);
        slider.setValue(def, juce::dontSendNotification);
        slider.setLookAndFeel(&knobLnF);
        addAndMakeVisible(slider);

        label.setText(name, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(label);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(juce::FontOptions(11.0f)));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(sldRate,      lblRate,      valRate,      "RATE",      0.5f,    15.0f,   4.5f,    0.1f);
    initKnob(sldDepth,     lblDepth,     valDepth,     "DEPTH",     0.0f,     1.0f,   0.65f);
    initKnob(sldShape,     lblShape,     valShape,     "SHAPE",     0.0f,     1.0f,   0.30f);
    initKnob(sldBias,      lblBias,      valBias,      "BIAS",      0.0f,     1.0f,   0.50f);
    initKnob(sldStereo,    lblStereo,    valStereo,    "STEREO",    0.0f,     1.0f,   0.0f);
    initKnob(sldHarmonic,  lblHarmonic,  valHarmonic,  "HARMONIC",  0.0f,     1.0f,   0.0f);
    initKnob(sldCrossover, lblCrossover, valCrossover, "XOVER",   250.0f,  2200.0f, 800.0f,  1.0f);
    initKnob(sldMix,       lblMix,       valMix,       "MIX",       0.0f,     1.0f,   1.0f);
    initKnob(sldLevel,     lblLevel,     valLevel,     "LEVEL",     0.5f,     2.0f,   1.0f,   0.01f);

    auto wireParam = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param == nullptr)
            return;

        slider.setValue(param->get(), juce::dontSendNotification);
        slider.onValueChange = [&slider, param]
        {
            param->beginChangeGesture();
            *param = (float) slider.getValue();
            param->endChangeGesture();
        };
    };

    wireParam(sldRate,      proc.rateParam);
    wireParam(sldDepth,     proc.depthParam);
    wireParam(sldShape,     proc.shapeParam);
    wireParam(sldBias,      proc.biasParam);
    wireParam(sldStereo,    proc.stereoParam);
    wireParam(sldHarmonic,  proc.harmonicParam);
    wireParam(sldCrossover, proc.crossoverParam);
    wireParam(sldMix,       proc.mixParam);
    wireParam(sldLevel,     proc.levelParam);

    startTimerHz(30);
}

inline void TremoloEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldRate,      proc.rateParam);
    syncSlider(sldDepth,     proc.depthParam);
    syncSlider(sldShape,     proc.shapeParam);
    syncSlider(sldBias,      proc.biasParam);
    syncSlider(sldStereo,    proc.stereoParam);
    syncSlider(sldHarmonic,  proc.harmonicParam);
    syncSlider(sldCrossover, proc.crossoverParam);
    syncSlider(sldMix,       proc.mixParam);
    syncSlider(sldLevel,     proc.levelParam);

    valRate.setText(juce::String((float) sldRate.getValue(), 1) + " Hz", juce::dontSendNotification);
    valDepth.setText(juce::String((int) std::round(sldDepth.getValue() * 100.0)) + "%", juce::dontSendNotification);

    {
        const float shape = (float) sldShape.getValue();
        const char* name = shape < 0.33f ? "Sine" : (shape < 0.66f ? "Tri" : "Square");
        valShape.setText(name, juce::dontSendNotification);
    }

    {
        const int biasPercent = (int) std::round((sldBias.getValue() - 0.5) * 200.0);
        valBias.setText((biasPercent >= 0 ? "+" : "") + juce::String(biasPercent) + "%", juce::dontSendNotification);
    }

    valStereo.setText(juce::String((int) std::round(sldStereo.getValue() * 180.0)) + " deg", juce::dontSendNotification);
    valHarmonic.setText(juce::String((int) std::round(sldHarmonic.getValue() * 100.0)) + "%", juce::dontSendNotification);

    {
        const float crossover = (float) sldCrossover.getValue();
        if (crossover >= 1000.0f)
            valCrossover.setText(juce::String(crossover / 1000.0f, 2) + " kHz", juce::dontSendNotification);
        else
            valCrossover.setText(juce::String((int) std::round(crossover)) + " Hz", juce::dontSendNotification);
    }

    valMix.setText(juce::String((int) std::round(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);

    {
        const float gain = (float) sldLevel.getValue();
        const float gainDb = 20.0f * std::log10(juce::jmax(gain, 0.001f));
        valLevel.setText((gainDb >= 0.0f ? "+" : "") + juce::String(gainDb, 1) + " dB", juce::dontSendNotification);
    }

    repaint(vizBounds.toNearestInt());
}

inline void TremoloEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
        bgDark, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    juce::ColourGradient glow(accent.withAlpha(0.30f), (float) getWidth() * 0.18f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth() * 0.82f, 0.0f, false);
    g.setGradientFill(glow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 2.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("TREMOLO", 28, 10, 220, 28, juce::Justification::centredLeft);

    g.setColour(accent);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    const float harmonic = (float) sldHarmonic.getValue();
    const float mix = (float) sldMix.getValue();
    const juce::String subtitle = harmonic > 0.75f ? "Harmonic Split"
        : (harmonic > 0.15f ? "Standard + Harmonic Blend" : "Bias / Optical AM");
    g.drawText(subtitle + "  |  Mix " + juce::String((int) std::round(mix * 100.0f)) + "%",
        28, 34, 320, 18, juce::Justification::centredLeft);

    paintLfoViz(g, vizBounds);
}

inline void TremoloEditor::paintLfoViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();
    const float midY = inner.getCentreY();

    const float depth = (float) sldDepth.getValue();
    const float shape = (float) sldShape.getValue();
    const float bias = (float) sldBias.getValue();
    const float stereo = (float) sldStereo.getValue();
    const float harmonic = (float) sldHarmonic.getValue();
    const float mix = (float) sldMix.getValue();
    const float phase = proc.visualPhase.load(std::memory_order_relaxed);
    const float dryGain = Nova::TremoloDSP::equalPowerDry(mix);
    const float wetGain = Nova::TremoloDSP::equalPowerWet(mix);

    g.setColour(juce::Colour(0xff1A2535));
    g.drawLine(inner.getX(), midY, inner.getRight(), midY, 0.5f);

    constexpr int numPoints = 300;
    auto drawTrace = [&](float stereoOffset, juce::Colour colour, float thickness, bool invertForHighBand)
    {
        juce::Path path;
        for (int i = 0; i < numPoints; ++i)
        {
            const float t = (float) i / (float) (numPoints - 1);
            const float phaseOffset = stereoOffset * 0.5f;
            const float previewPhase = std::fmod(t * 2.0f + phaseOffset, 1.0f);
            const float lfo = Nova::TremoloDSP::shapedLfo(previewPhase, shape, bias);
            const float wetMod = Nova::TremoloDSP::modulationGain(invertForHighBand ? -lfo : lfo, depth);
            const float totalGain = dryGain + wetGain * wetMod;
            const float x = inner.getX() + t * width;
            const float y = midY - (totalGain - 0.5f) * height * 1.35f;

            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }

        g.setColour(colour.withAlpha(0.06f));
        g.strokePath(path, juce::PathStrokeType(thickness + 4.0f, juce::PathStrokeType::curved));
        g.setColour(colour);
        g.strokePath(path, juce::PathStrokeType(thickness, juce::PathStrokeType::curved));
    };

    drawTrace(0.0f, accentGlow.withAlpha(0.70f), 1.6f, false);

    if (stereo > 0.01f)
        drawTrace(stereo, accent.withAlpha(0.45f), 1.1f, false);

    if (harmonic > 0.1f)
        drawTrace(0.0f, juce::Colour(0xff60A5FA).withAlpha(0.28f * harmonic), 1.0f, true);

    const float markerX = inner.getX() + std::fmod(phase * 2.0f, 1.0f) * width;
    g.setColour(accentGlow.withAlpha(0.35f));
    g.drawLine(markerX, inner.getY() + 4.0f, markerX, inner.getBottom() - 4.0f, 1.0f);

    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.setColour(accentGlow.withAlpha(0.45f));
    g.drawText("LOW", (int) inner.getX() + 4, (int) inner.getY() + 2, 28, 10, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff60A5FA).withAlpha(0.35f));
    g.drawText("HIGH", (int) inner.getX() + 32, (int) inner.getY() + 2, 32, 10, juce::Justification::centredLeft);
}

inline void TremoloEditor::resized()
{
    vizBounds = juce::Rectangle<float>(28.0f, 60.0f, (float) (getWidth() - 56), 150.0f);

    const int knobSize = 74;
    const int labelHeight = 16;
    const int valueHeight = 16;
    const int topRowY = 228;
    const int bottomRowY = 348;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup topRow[] = {
        { sldRate, lblRate, valRate },
        { sldDepth, lblDepth, valDepth },
        { sldShape, lblShape, valShape },
        { sldBias, lblBias, valBias },
        { sldStereo, lblStereo, valStereo }
    };

    KnobGroup bottomRow[] = {
        { sldHarmonic, lblHarmonic, valHarmonic },
        { sldCrossover, lblCrossover, valCrossover },
        { sldMix, lblMix, valMix },
        { sldLevel, lblLevel, valLevel }
    };

    const int topSlotWidth = getWidth() / 5;
    for (int i = 0; i < 5; ++i)
    {
        const int centreX = topSlotWidth * i + topSlotWidth / 2;
        topRow[i].label.setBounds(centreX - 54, topRowY, 108, labelHeight);
        topRow[i].slider.setBounds(centreX - knobSize / 2, topRowY + labelHeight + 2, knobSize, knobSize);
        topRow[i].value.setBounds(centreX - 54, topRowY + labelHeight + knobSize + 4, 108, valueHeight);
    }

    const int bottomStartX = 70;
    const int bottomSlotWidth = (getWidth() - bottomStartX * 2) / 4;
    for (int i = 0; i < 4; ++i)
    {
        const int centreX = bottomStartX + bottomSlotWidth * i + bottomSlotWidth / 2;
        bottomRow[i].label.setBounds(centreX - 54, bottomRowY, 108, labelHeight);
        bottomRow[i].slider.setBounds(centreX - knobSize / 2, bottomRowY + labelHeight + 2, knobSize, knobSize);
        bottomRow[i].value.setBounds(centreX - 54, bottomRowY + labelHeight + knobSize + 4, 108, valueHeight);
    }
}

inline juce::AudioProcessorEditor* TremoloPedal::createEditor()
{
    return new TremoloEditor(*this);
}
