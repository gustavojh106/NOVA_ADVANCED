#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class ChorusPedal;

class ChorusEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    ChorusEditor(ChorusPedal& pedal);
    ~ChorusEditor() override
    {
        stopTimer();
        sldRate.setLookAndFeel(nullptr);
        sldDepth.setLookAndFeel(nullptr);
        sldLag.setLookAndFeel(nullptr);
        sldWidth.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintLfoViz(juce::Graphics& g, juce::Rectangle<float> bounds);
    void syncModeFromProcessor();
    void refreshModeSummary();

    ChorusPedal& proc;

    static constexpr int kWidth = 720;
    static constexpr int kHeight = 440;

    const juce::Colour accent = juce::Colour::fromString("ff22D3EE");
    const juce::Colour accentDim = juce::Colour::fromString("ff0891B2");
    const juce::Colour accentGlow = juce::Colour::fromString("ff99F6E4");
    const juce::Colour bgTop = juce::Colour(0xff07131C);
    const juce::Colour bgBottom = juce::Colour(0xff091D28);
    const juce::Colour panel = juce::Colour(0xff0E2735);
    const juce::Colour panelEdge = juce::Colour(0xff1D4456);
    const juce::Colour textBright = juce::Colour(0xffEEF8FB);
    const juce::Colour textDim = juce::Colour(0xff7DA5B4);

    juce::Slider sldRate, sldDepth, sldLag, sldWidth, sldTone, sldMix;
    juce::Label lblRate, lblDepth, lblLag, lblWidth, lblTone, lblMix;
    juce::Label valRate, valDepth, valLag, valWidth, valTone, valMix;

    juce::Label modeLabel;
    juce::ComboBox modeBox;
    juce::Label modeSummaryLabel;
    juce::Label stereoHintLabel;

    juce::Rectangle<float> vizBounds;
    int cachedMode = -1;

    struct ChorusKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour accent = juce::Colour::fromString("ff22D3EE");
        juce::Colour accentGlow = juce::Colour::fromString("ff99F6E4");

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
            float sliderPos, float startAngle, float endAngle,
            juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(5.0f);
            const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float centreX = area.getCentreX();
            const float centreY = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcRadius = radius - 4.0f;

            juce::Path track;
            track.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff143241));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.001f)
            {
                juce::Path arc;
                arc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(accent.withAlpha(0.12f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(accent);
                g.strokePath(arc, juce::PathStrokeType(3.4f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.58f;
            juce::ColourGradient body(juce::Colour(0xff1B3C4B), centreX, centreY - knobRadius,
                juce::Colour(0xff0B1D29), centreX, centreY + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2D596A));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.74f;
            const float pointX = centreX + std::sin(angle) * pointerLength;
            const float pointY = centreY - std::cos(angle) * pointerLength;
            g.setColour(accentGlow);
            g.drawLine(centreX, centreY, pointX, pointY, 2.0f);
            g.fillEllipse(pointX - 2.8f, pointY - 2.8f, 5.6f, 5.6f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChorusEditor)
};

inline ChorusEditor::ChorusEditor(ChorusPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& slider, juce::Label& name, juce::Label& value,
        const juce::String& text, double min, double max, double defaultValue, double step)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider.setRange(min, max, step);
        slider.setValue(defaultValue, juce::dontSendNotification);
        slider.setLookAndFeel(&knobLnF);
        addAndMakeVisible(slider);

        name.setText(text, juce::dontSendNotification);
        name.setJustificationType(juce::Justification::centred);
        name.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        name.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(name);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(juce::FontOptions(11.0f)));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(sldRate, lblRate, valRate, "RATE", 0.03, 8.0, 0.85, 0.01);
    initKnob(sldDepth, lblDepth, valDepth, "DEPTH", 0.0, 1.0, 0.58, 0.01);
    initKnob(sldLag, lblLag, valLag, "LAG", 2.0, 18.0, 7.8, 0.1);
    initKnob(sldWidth, lblWidth, valWidth, "WIDTH", 0.0, 1.0, 0.72, 0.01);
    initKnob(sldTone, lblTone, valTone, "TONE", 0.0, 1.0, 0.62, 0.01);
    initKnob(sldMix, lblMix, valMix, "MIX", 0.0, 1.0, 0.36, 0.01);

    const auto wireFloat = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param == nullptr)
            return;

        slider.setValue(param->get(), juce::dontSendNotification);
        slider.onValueChange = [&slider, param]
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->convertTo0to1((float) slider.getValue()));
            param->endChangeGesture();
        };
    };

    wireFloat(sldRate, proc.rateParam);
    wireFloat(sldDepth, proc.depthParam);
    wireFloat(sldLag, proc.lagParam);
    wireFloat(sldWidth, proc.widthParam);
    wireFloat(sldTone, proc.toneParam);
    wireFloat(sldMix, proc.mixParam);

    modeLabel.setText("MODE", juce::dontSendNotification);
    modeLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    modeLabel.setColour(juce::Label::textColourId, textDim);
    addAndMakeVisible(modeLabel);

    modeBox.addItemList(proc.modeParam != nullptr ? proc.modeParam->choices : juce::StringArray{ "Classic" }, 1);
    modeBox.setColour(juce::ComboBox::backgroundColourId, panel);
    modeBox.setColour(juce::ComboBox::outlineColourId, panelEdge);
    modeBox.setColour(juce::ComboBox::textColourId, textBright);
    modeBox.setColour(juce::ComboBox::arrowColourId, accentGlow);
    modeBox.onChange = [this]
    {
        if (proc.modeParam == nullptr)
            return;

        proc.modeParam->beginChangeGesture();
        proc.modeParam->setValueNotifyingHost((float) modeBox.getSelectedItemIndex()
            / (float) juce::jmax(1, proc.modeParam->choices.size() - 1));
        proc.modeParam->endChangeGesture();
    };
    addAndMakeVisible(modeBox);

    modeSummaryLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    modeSummaryLabel.setColour(juce::Label::textColourId, textDim.withAlpha(0.96f));
    modeSummaryLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(modeSummaryLabel);

    stereoHintLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    stereoHintLabel.setColour(juce::Label::textColourId, accentGlow.withAlpha(0.88f));
    stereoHintLabel.setJustificationType(juce::Justification::centredRight);
    stereoHintLabel.setText("Mono-safe centre, expanded stereo edges", juce::dontSendNotification);
    addAndMakeVisible(stereoHintLabel);

    syncModeFromProcessor();
    refreshModeSummary();
    startTimerHz(30);
}

inline void ChorusEditor::timerCallback()
{
    const auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldRate, proc.rateParam);
    syncSlider(sldDepth, proc.depthParam);
    syncSlider(sldLag, proc.lagParam);
    syncSlider(sldWidth, proc.widthParam);
    syncSlider(sldTone, proc.toneParam);
    syncSlider(sldMix, proc.mixParam);

    syncModeFromProcessor();

    valRate.setText(juce::String((float) sldRate.getValue(), 2) + " Hz", juce::dontSendNotification);
    valDepth.setText(juce::String((int) std::round(sldDepth.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valLag.setText(juce::String((float) sldLag.getValue(), 1) + " ms", juce::dontSendNotification);
    valWidth.setText(juce::String((int) std::round(sldWidth.getValue() * 100.0)) + "%", juce::dontSendNotification);

    const float toneCutoff = ChorusPedal::toneToCutoffHz((float) sldTone.getValue());
    valTone.setText(toneCutoff >= 1000.0f
            ? juce::String(toneCutoff / 1000.0f, 1) + " kHz"
            : juce::String((int) std::round(toneCutoff)) + " Hz",
        juce::dontSendNotification);

    valMix.setText(juce::String((int) std::round(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);
    repaint(vizBounds.toNearestInt());
}

inline void ChorusEditor::syncModeFromProcessor()
{
    if (proc.modeParam == nullptr)
        return;

    const int mode = proc.modeParam->getIndex();
    modeBox.setSelectedItemIndex(mode, juce::dontSendNotification);
    if (mode != cachedMode)
    {
        cachedMode = mode;
        refreshModeSummary();
    }
}

inline void ChorusEditor::refreshModeSummary()
{
    modeSummaryLabel.setText(ChorusPedal::getModeDescription(cachedMode < 0 ? 0 : cachedMode),
        juce::dontSendNotification);
}

inline void ChorusEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(bgTop, 0.0f, 0.0f, bgBottom, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.14f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    juce::ColourGradient topGlow(accent.withAlpha(0.36f), 0.0f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth(), 0.0f, false);
    g.setGradientFill(topGlow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 3.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
    g.drawText("CHORUS", 26, 12, 220, 30, juce::Justification::centredLeft);

    g.setColour(accentGlow);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText(modeBox.getText().isNotEmpty() ? modeBox.getText() : juce::String("Classic"),
        26, 38, 220, 18, juce::Justification::centredLeft);

    paintLfoViz(g, vizBounds);
}

inline void ChorusEditor::paintLfoViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(panel.withAlpha(0.92f));
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(panelEdge);
    g.drawRoundedRectangle(bounds, 10.0f, 1.0f);

    auto inner = bounds.reduced(18.0f, 16.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();
    const float midY = inner.getCentreY();
    const float depth = (float) sldDepth.getValue();
    const float stereo = (float) sldWidth.getValue();
    const float phase = proc.lfoPhase;
    const int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;
    const int voices = mode == 1 ? 3 : (mode == 2 ? 2 : 2);
    const float triangleBlend = mode == 1 ? 0.22f : (mode == 2 ? 0.06f : 0.10f);
    const float harmonicBlend = mode == 1 ? 0.24f : (mode == 2 ? 0.06f : 0.12f);

    for (int voice = 0; voice < voices; ++voice)
    {
        juce::Path pathL;
        juce::Path pathR;
        const float phaseOffset = voice == 0 ? 0.0f : (voice == 1 ? 0.23f : 0.57f);
        const float stereoOffset = (0.12f + 0.14f * (float) voice) * stereo;
        const float amplitude = height * (0.22f + 0.08f * depth) * (1.0f - 0.16f * (float) voice);

        for (int px = 0; px < (int) width; ++px)
        {
            const float t = (float) px / juce::jmax(1.0f, width - 1.0f);
            const float visualPhase = t * 2.3f + phase;
            const float lfoL = Nova::ChorusDSP::shapedLfo(visualPhase + phaseOffset - stereoOffset, triangleBlend, harmonicBlend);
            const float lfoR = Nova::ChorusDSP::shapedLfo(visualPhase + phaseOffset + stereoOffset, triangleBlend, harmonicBlend);
            const float x = inner.getX() + (float) px;
            const float yL = midY - lfoL * amplitude;
            const float yR = midY - lfoR * amplitude;

            if (px == 0)
            {
                pathL.startNewSubPath(x, yL);
                pathR.startNewSubPath(x, yR);
            }
            else
            {
                pathL.lineTo(x, yL);
                pathR.lineTo(x, yR);
            }
        }

        const float alphaBase = voice == 0 ? 0.82f : (voice == 1 ? 0.58f : 0.34f);
        g.setColour(accent.withAlpha(0.08f * alphaBase));
        g.strokePath(pathL, juce::PathStrokeType(5.5f - (float) voice, juce::PathStrokeType::curved));
        g.setColour(accentGlow.withAlpha(alphaBase));
        g.strokePath(pathL, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));

        g.setColour(accentDim.withAlpha(0.08f * alphaBase));
        g.strokePath(pathR, juce::PathStrokeType(5.0f - (float) voice, juce::PathStrokeType::curved));
        g.setColour(accent.withAlpha(0.44f * alphaBase));
        g.strokePath(pathR, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));
    }

    g.setColour(panelEdge.withAlpha(0.85f));
    g.drawLine(inner.getX(), midY, inner.getRight(), midY, 0.6f);

    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.setColour(accentGlow.withAlpha(0.75f));
    g.drawText("L", (int) inner.getX() + 4, (int) inner.getY() + 2, 18, 14, juce::Justification::centredLeft);
    g.setColour(accent.withAlpha(0.55f));
    g.drawText("R", (int) inner.getX() + 20, (int) inner.getY() + 2, 18, 14, juce::Justification::centredLeft);
}

inline void ChorusEditor::resized()
{
    const int width = getWidth();

    modeLabel.setBounds(width - 210, 16, 70, 16);
    modeBox.setBounds(width - 210, 34, 170, 26);
    modeSummaryLabel.setBounds(26, 58, width - 52, 18);
    stereoHintLabel.setBounds(width - 290, 60, 250, 16);

    vizBounds = juce::Rectangle<float>(26.0f, 84.0f, (float) (width - 52), 150.0f);

    const int knobAreaY = 254;
    const int knobSize = 84;
    const int labelHeight = 16;
    const int valueHeight = 16;
    const int slotWidth = width / 6;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup knobs[] = {
        { sldRate, lblRate, valRate },
        { sldDepth, lblDepth, valDepth },
        { sldLag, lblLag, valLag },
        { sldWidth, lblWidth, valWidth },
        { sldTone, lblTone, valTone },
        { sldMix, lblMix, valMix }
    };

    for (int i = 0; i < 6; ++i)
    {
        const int centreX = slotWidth * i + slotWidth / 2;
        knobs[i].label.setBounds(centreX - 52, knobAreaY, 104, labelHeight);
        knobs[i].slider.setBounds(centreX - knobSize / 2, knobAreaY + labelHeight + 6, knobSize, knobSize);
        knobs[i].value.setBounds(centreX - 58, knobAreaY + labelHeight + 6 + knobSize + 4, 116, valueHeight);
    }
}

inline juce::AudioProcessorEditor* ChorusPedal::createEditor()
{
    return new ChorusEditor(*this);
}
