#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class FuzzPedal;

class FuzzEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    FuzzEditor(FuzzPedal& pedal);
    ~FuzzEditor() override
    {
        stopTimer();
        sldFuzz.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldBias.setLookAndFeel(nullptr);
        sldGate.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
        sldLevel.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintTransferCurve(juce::Graphics& g, juce::Rectangle<float> bounds);
    void syncModeFromProcessor();
    void refreshModeSummary();

    FuzzPedal& proc;

    static constexpr int kWidth = 730;
    static constexpr int kHeight = 445;

    const juce::Colour accent = juce::Colour::fromString("ffA855F7");
    const juce::Colour accentDim = juce::Colour::fromString("ff7C3AED");
    const juce::Colour accentGlow = juce::Colour::fromString("ffDDD6FE");
    const juce::Colour bgTop = juce::Colour(0xff120A18);
    const juce::Colour bgBottom = juce::Colour(0xff1E1028);
    const juce::Colour panel = juce::Colour(0xff261336);
    const juce::Colour panelEdge = juce::Colour(0xff4B2C66);
    const juce::Colour textBright = juce::Colour(0xffF5EFFC);
    const juce::Colour textDim = juce::Colour(0xffAD9AC5);

    juce::Slider sldFuzz, sldTone, sldBias, sldGate, sldMix, sldLevel;
    juce::Label lblFuzz, lblTone, lblBias, lblGate, lblMix, lblLevel;
    juce::Label valFuzz, valTone, valBias, valGate, valMix, valLevel;

    juce::Label modeLabel;
    juce::ComboBox modeBox;
    juce::Label modeSummaryLabel;
    juce::Label sagHintLabel;

    juce::Rectangle<float> vizBounds;
    int cachedMode = -1;

    struct FuzzKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour accent = juce::Colour::fromString("ffA855F7");
        juce::Colour accentGlow = juce::Colour::fromString("ffDDD6FE");

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
            g.setColour(juce::Colour(0xff332043));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.001f)
            {
                juce::Path arc;
                arc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(accent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(accent);
                g.strokePath(arc, juce::PathStrokeType(3.4f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.58f;
            juce::ColourGradient body(juce::Colour(0xff46275F), centreX, centreY - knobRadius,
                juce::Colour(0xff1B1025), centreX, centreY + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff6C4093));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.74f;
            const float pointX = centreX + std::sin(angle) * pointerLength;
            const float pointY = centreY - std::cos(angle) * pointerLength;
            g.setColour(accentGlow);
            g.drawLine(centreX, centreY, pointX, pointY, 2.0f);
            g.fillEllipse(pointX - 2.8f, pointY - 2.8f, 5.6f, 5.6f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FuzzEditor)
};

inline FuzzEditor::FuzzEditor(FuzzPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& slider, juce::Label& label, juce::Label& value,
        const juce::String& text, double min, double max, double defaultValue, double step)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider.setRange(min, max, step);
        slider.setValue(defaultValue, juce::dontSendNotification);
        slider.setLookAndFeel(&knobLnF);
        addAndMakeVisible(slider);

        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(11.0f, juce::Font::bold));
        label.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(label);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(11.0f));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(sldFuzz, lblFuzz, valFuzz, "FUZZ", 0.0, 100.0, 67.0, 1.0);
    initKnob(sldTone, lblTone, valTone, "TONE", 0.0, 1.0, 0.48, 0.01);
    initKnob(sldBias, lblBias, valBias, "BIAS", 0.0, 1.0, 0.56, 0.01);
    initKnob(sldGate, lblGate, valGate, "GATE", 0.0, 1.0, 0.28, 0.01);
    initKnob(sldMix, lblMix, valMix, "MIX", 0.0, 1.0, 1.0, 0.01);
    initKnob(sldLevel, lblLevel, valLevel, "LEVEL", 0.0, 1.0, 0.58, 0.01);

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

    wireFloat(sldFuzz, proc.fuzzParam);
    wireFloat(sldTone, proc.toneParam);
    wireFloat(sldBias, proc.biasParam);
    wireFloat(sldGate, proc.gateParam);
    wireFloat(sldMix, proc.mixParam);
    wireFloat(sldLevel, proc.levelParam);

    modeLabel.setText("MODE", juce::dontSendNotification);
    modeLabel.setFont(juce::Font(11.0f, juce::Font::bold));
    modeLabel.setColour(juce::Label::textColourId, textDim);
    addAndMakeVisible(modeLabel);

    modeBox.addItemList(proc.modeParam != nullptr ? proc.modeParam->choices : juce::StringArray{ "Vintage" }, 1);
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

    modeSummaryLabel.setFont(juce::Font(12.0f));
    modeSummaryLabel.setColour(juce::Label::textColourId, textDim.withAlpha(0.96f));
    modeSummaryLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(modeSummaryLabel);

    sagHintLabel.setFont(juce::Font(11.0f, juce::Font::bold));
    sagHintLabel.setColour(juce::Label::textColourId, accentGlow.withAlpha(0.88f));
    sagHintLabel.setJustificationType(juce::Justification::centredRight);
    sagHintLabel.setText("Bias steers sustain vs starve; gate sharpens decay", juce::dontSendNotification);
    addAndMakeVisible(sagHintLabel);

    syncModeFromProcessor();
    refreshModeSummary();
    startTimerHz(30);
}

inline void FuzzEditor::timerCallback()
{
    const auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldFuzz, proc.fuzzParam);
    syncSlider(sldTone, proc.toneParam);
    syncSlider(sldBias, proc.biasParam);
    syncSlider(sldGate, proc.gateParam);
    syncSlider(sldMix, proc.mixParam);
    syncSlider(sldLevel, proc.levelParam);
    syncModeFromProcessor();

    valFuzz.setText(juce::String((int) sldFuzz.getValue()) + "%", juce::dontSendNotification);

    const float toneHz = FuzzPedal::toneToCutoffHz((float) sldTone.getValue());
    valTone.setText(toneHz >= 1000.0f
            ? juce::String(toneHz * 0.001f, 1) + " kHz"
            : juce::String((int) toneHz) + " Hz",
        juce::dontSendNotification);

    const float biasDescriptor = FuzzPedal::biasToDescriptor((float) sldBias.getValue());
    valBias.setText((biasDescriptor >= 0.0f ? "+" : "") + juce::String(biasDescriptor, 2), juce::dontSendNotification);
    valGate.setText(juce::String((int) std::round(sldGate.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valMix.setText(juce::String((int) std::round(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valLevel.setText(juce::String((int) std::round(sldLevel.getValue() * 100.0)) + "%", juce::dontSendNotification);

    repaint(vizBounds.toNearestInt());
}

inline void FuzzEditor::syncModeFromProcessor()
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

inline void FuzzEditor::refreshModeSummary()
{
    modeSummaryLabel.setText(FuzzPedal::getModeDescription(cachedMode < 0 ? 0 : cachedMode),
        juce::dontSendNotification);
}

inline void FuzzEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(bgTop, 0.0f, 0.0f, bgBottom, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.16f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    juce::ColourGradient glow(accent.withAlpha(0.34f), 0.0f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth(), 0.0f, false);
    g.setGradientFill(glow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 3.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    g.drawText("FUZZ", 26, 12, 220, 30, juce::Justification::centredLeft);

    g.setColour(accentGlow);
    g.setFont(juce::Font(13.0f));
    g.drawText(modeBox.getText().isNotEmpty() ? modeBox.getText() : juce::String("Vintage"),
        26, 38, 220, 18, juce::Justification::centredLeft);

    paintTransferCurve(g, vizBounds);
}

inline void FuzzEditor::paintTransferCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(panel.withAlpha(0.95f));
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(panelEdge);
    g.drawRoundedRectangle(bounds, 10.0f, 1.0f);

    auto inner = bounds.reduced(18.0f, 12.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float midX = inner.getCentreX();
    const float midY = inner.getCentreY();
    const float fuzzAmount = (float) sldFuzz.getValue();
    const float biasAmount = (float) sldBias.getValue();
    const int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;

    g.setColour(juce::Colour(0xff332043));
    g.drawLine(inner.getX(), midY, inner.getRight(), midY, 0.6f);
    g.drawLine(midX, inner.getY(), midX, inner.getBottom(), 0.6f);
    g.drawLine(inner.getX(), inner.getBottom(), inner.getRight(), inner.getY(), 0.45f);

    juce::Path curvePath;
    constexpr int numPoints = 220;
    for (int i = 0; i < numPoints; ++i)
    {
        const float t = (float) i / (float) (numPoints - 1);
        const float input = t * 2.0f - 1.0f;
        const float output = FuzzPedal::computeTransferCurve(input, fuzzAmount, biasAmount, mode);
        const float x = inner.getX() + t * w;
        const float y = midY - output * (h * 0.42f);

        if (i == 0)
            curvePath.startNewSubPath(x, y);
        else
            curvePath.lineTo(x, y);
    }

    g.setColour(accent.withAlpha(0.08f));
    g.strokePath(curvePath, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.14f));
    g.strokePath(curvePath, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.88f));
    g.strokePath(curvePath, juce::PathStrokeType(1.9f, juce::PathStrokeType::curved));

    g.setFont(juce::Font(8.5f));
    g.setColour(textDim.withAlpha(0.44f));
    g.drawText("IN", (int) inner.getRight() - 18, (int) midY + 2, 18, 10, juce::Justification::centredRight);
    g.drawText("OUT", (int) midX + 3, (int) inner.getY() + 1, 24, 10, juce::Justification::centredLeft);

    const float sag = juce::jlimit(0.0f, 1.0f, proc.displaySag * 1.9f);
    const float sagBarW = 5.0f;
    const float sagBarMaxH = 32.0f;
    const float sagBarX = inner.getRight() - 22.0f;
    const float sagBarY = inner.getY() + 8.0f;
    const float sagBarH = sag * sagBarMaxH;

    g.setColour(juce::Colour(0xff332043));
    g.fillRoundedRectangle(sagBarX, sagBarY, sagBarW, sagBarMaxH, 1.5f);
    g.setColour(accent.interpolatedWith(juce::Colour(0xffFB7185), sag).withAlpha(0.78f));
    g.fillRoundedRectangle(sagBarX, sagBarY + sagBarMaxH - sagBarH, sagBarW, sagBarH, 1.5f);

    g.setFont(juce::Font(7.5f));
    g.setColour(textDim.withAlpha(0.45f));
    g.drawText("SAG", (int) sagBarX - 7, (int) (sagBarY + sagBarMaxH + 2), 22, 8, juce::Justification::centred);

    const juce::String character = fuzzAmount < 24.0f ? "Edge"
        : fuzzAmount < 48.0f ? "Growl"
        : fuzzAmount < 74.0f ? "Roar"
        : "Wall";
    g.setFont(juce::Font(9.0f));
    g.setColour(accent.withAlpha(0.54f));
    g.drawText(character, (int) inner.getX(), (int) inner.getY() + 2, 72, 12, juce::Justification::centredLeft);
}

inline void FuzzEditor::resized()
{
    const int width = getWidth();

    modeLabel.setBounds(width - 210, 16, 70, 16);
    modeBox.setBounds(width - 210, 34, 170, 26);
    modeSummaryLabel.setBounds(26, 58, width - 52, 18);
    sagHintLabel.setBounds(width - 330, 60, 300, 16);

    vizBounds = juce::Rectangle<float>(26.0f, 86.0f, (float) (width - 52), 148.0f);

    const int knobAreaY = 255;
    const int knobSize = 84;
    const int labelH = 16;
    const int valueH = 16;
    const int slotW = width / 6;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup knobs[] = {
        { sldFuzz, lblFuzz, valFuzz },
        { sldTone, lblTone, valTone },
        { sldBias, lblBias, valBias },
        { sldGate, lblGate, valGate },
        { sldMix, lblMix, valMix },
        { sldLevel, lblLevel, valLevel }
    };

    for (int i = 0; i < 6; ++i)
    {
        const int centreX = slotW * i + slotW / 2;
        knobs[i].label.setBounds(centreX - 54, knobAreaY, 108, labelH);
        knobs[i].slider.setBounds(centreX - knobSize / 2, knobAreaY + labelH + 6, knobSize, knobSize);
        knobs[i].value.setBounds(centreX - 58, knobAreaY + labelH + 6 + knobSize + 4, 116, valueH);
    }
}

inline juce::AudioProcessorEditor* FuzzPedal::createEditor()
{
    return new FuzzEditor(*this);
}
