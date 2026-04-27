#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class FlangerPedal;

class FlangerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    FlangerEditor(FlangerPedal& pedal);
    ~FlangerEditor() override
    {
        stopTimer();
        sldRate.setLookAndFeel(nullptr);
        sldDepth.setLookAndFeel(nullptr);
        sldManual.setLookAndFeel(nullptr);
        sldFeedback.setLookAndFeel(nullptr);
        sldWidth.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintCombViz(juce::Graphics& g, juce::Rectangle<float> bounds);
    void syncModeFromProcessor();
    void refreshModeSummary();

    FlangerPedal& proc;

    static constexpr int kWidth = 840;
    static constexpr int kHeight = 470;

    const juce::Colour accent = juce::Colour::fromString("ff38BDF8");
    const juce::Colour accentDim = juce::Colour::fromString("ff0284C7");
    const juce::Colour accentGlow = juce::Colour::fromString("ffBAE6FD");
    const juce::Colour bgTop = juce::Colour(0xff08111A);
    const juce::Colour bgBottom = juce::Colour(0xff0A1725);
    const juce::Colour panel = juce::Colour(0xff0F2231);
    const juce::Colour panelEdge = juce::Colour(0xff1F425A);
    const juce::Colour textBright = juce::Colour(0xffEFF7FC);
    const juce::Colour textDim = juce::Colour(0xff7FA4BC);

    juce::Slider sldRate, sldDepth, sldManual, sldFeedback, sldWidth, sldTone, sldMix;
    juce::Label lblRate, lblDepth, lblManual, lblFeedback, lblWidth, lblTone, lblMix;
    juce::Label valRate, valDepth, valManual, valFeedback, valWidth, valTone, valMix;

    juce::Label modeLabel;
    juce::ComboBox modeBox;
    juce::Label modeSummaryLabel;
    juce::Label stereoHintLabel;

    juce::Rectangle<float> vizBounds;
    int cachedMode = -1;

    static float freqToNorm(float freq)
    {
        constexpr float logMin = 1.69897f;
        constexpr float logMax = 4.30103f;
        const float logF = std::log10(juce::jlimit(50.0f, 20000.0f, freq));
        return (logF - logMin) / (logMax - logMin);
    }

    struct FlangerKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour accent = juce::Colour::fromString("ff38BDF8");
        juce::Colour accentGlow = juce::Colour::fromString("ffBAE6FD");

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
            g.setColour(juce::Colour(0xff153145));
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
            juce::ColourGradient body(juce::Colour(0xff1A3A50), centreX, centreY - knobRadius,
                juce::Colour(0xff0A1924), centreX, centreY + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2A5974));
            g.drawEllipse(centreX - knobRadius, centreY - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.74f;
            const float pointX = centreX + std::sin(angle) * pointerLength;
            const float pointY = centreY - std::cos(angle) * pointerLength;
            g.setColour(accentGlow);
            g.drawLine(centreX, centreY, pointX, pointY, 2.0f);
            g.fillEllipse(pointX - 2.8f, pointY - 2.8f, 5.6f, 5.6f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FlangerEditor)
};

inline FlangerEditor::FlangerEditor(FlangerPedal& pedal)
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

    initKnob(sldRate, lblRate, valRate, "RATE", 0.03, 5.5, 0.32, 0.01);
    initKnob(sldDepth, lblDepth, valDepth, "DEPTH", 0.0, 1.0, 0.72, 0.01);
    initKnob(sldManual, lblManual, valManual, "MANUAL", 0.0, 1.0, 0.34, 0.01);
    initKnob(sldFeedback, lblFeedback, valFeedback, "FEEDBACK", -0.95, 0.95, 0.42, 0.01);
    initKnob(sldWidth, lblWidth, valWidth, "WIDTH", 0.0, 1.0, 0.68, 0.01);
    initKnob(sldTone, lblTone, valTone, "TONE", 1000.0, 14000.0, 7800.0, 1.0);
    initKnob(sldMix, lblMix, valMix, "MIX", 0.0, 1.0, 0.46, 0.01);

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
    wireFloat(sldManual, proc.manualParam);
    wireFloat(sldFeedback, proc.feedbackParam);
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
    stereoHintLabel.setText("Stereo wings stay wide without blowing up the centre image", juce::dontSendNotification);
    addAndMakeVisible(stereoHintLabel);

    syncModeFromProcessor();
    refreshModeSummary();
    startTimerHz(30);
}

inline void FlangerEditor::timerCallback()
{
    const auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldRate, proc.rateParam);
    syncSlider(sldDepth, proc.depthParam);
    syncSlider(sldManual, proc.manualParam);
    syncSlider(sldFeedback, proc.feedbackParam);
    syncSlider(sldWidth, proc.widthParam);
    syncSlider(sldTone, proc.toneParam);
    syncSlider(sldMix, proc.mixParam);

    syncModeFromProcessor();

    valRate.setText(juce::String((float) sldRate.getValue(), 2) + " Hz", juce::dontSendNotification);
    valDepth.setText(juce::String((int) std::round(sldDepth.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valManual.setText(juce::String(FlangerPedal::manualToCentreDelayMs((float) sldManual.getValue()), 2) + " ms", juce::dontSendNotification);
    {
        const int percent = juce::roundToInt(sldFeedback.getValue() * 100.0);
        valFeedback.setText((percent >= 0 ? "+" : "") + juce::String(percent) + "%", juce::dontSendNotification);
    }
    valWidth.setText(juce::String((int) std::round(sldWidth.getValue() * 100.0)) + "%", juce::dontSendNotification);
    {
        const float tone = (float) sldTone.getValue();
        valTone.setText(tone >= 1000.0f
            ? juce::String(tone / 1000.0f, 1) + " kHz"
            : juce::String((int) std::round(tone)) + " Hz",
            juce::dontSendNotification);
    }
    valMix.setText(juce::String((int) std::round(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);
    repaint(vizBounds.toNearestInt());
}

inline void FlangerEditor::syncModeFromProcessor()
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

inline void FlangerEditor::refreshModeSummary()
{
    modeSummaryLabel.setText(FlangerPedal::getModeDescription(cachedMode < 0 ? 0 : cachedMode),
        juce::dontSendNotification);
}

inline void FlangerEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(bgTop, 0.0f, 0.0f, bgBottom, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    juce::ColourGradient glow(accent.withAlpha(0.28f), (float) getWidth() * 0.18f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth() * 0.82f, 0.0f, false);
    g.setGradientFill(glow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 2.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
    g.drawText("FLANGER", 28, 10, 220, 30, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("Commercial stereo flange with pro voicings", 28, 36, 320, 18, juce::Justification::centredLeft);

    paintCombViz(g, vizBounds);
}

inline void FlangerEditor::paintCombViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff07111A));
    g.fillRoundedRectangle(bounds, 8.0f);
    g.setColour(panelEdge.withAlpha(0.82f));
    g.drawRoundedRectangle(bounds, 8.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 12.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();

    const float feedback = (float) sldFeedback.getValue();
    const float mix = (float) sldMix.getValue();
    const float delayL = juce::jmax(0.35f, proc.lastDelayMs[0]);
    const float delayR = juce::jmax(0.35f, proc.lastDelayMs[1] > 0.0f ? proc.lastDelayMs[1] : proc.lastDelayMs[0]);
    const float polarity = cachedMode == 2 ? -1.0f : 1.0f;
    const float dryGain = std::cos(mix * juce::MathConstants<float>::halfPi);
    const float wetGain = std::sin(mix * juce::MathConstants<float>::halfPi);

    const float gridFreqs[] = { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
    const char* gridLabels[] = { "100", "200", "500", "1k", "2k", "5k", "10k" };
    g.setFont(juce::Font(juce::FontOptions(8.5f)));

    for (int i = 0; i < 7; ++i)
    {
        const float x = inner.getX() + freqToNorm(gridFreqs[i]) * width;
        g.setColour(juce::Colour(0xff152433));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.34f));
        g.drawText(gridLabels[i], (int) (x - 16.0f), (int) inner.getBottom() + 1, 32, 11, juce::Justification::centred);
    }

    g.setColour(juce::Colour(0xff152433));
    g.drawLine(inner.getX(), inner.getY() + height * 0.25f, inner.getRight(), inner.getY() + height * 0.25f, 0.5f);
    g.drawLine(inner.getX(), inner.getCentreY(), inner.getRight(), inner.getCentreY(), 0.5f);
    g.drawLine(inner.getX(), inner.getY() + height * 0.75f, inner.getRight(), inner.getY() + height * 0.75f, 0.5f);

    auto computeResponse = [&](float delayMs, juce::Path& path)
    {
        constexpr float dbRange = 18.0f;
        const float regenFeedback = juce::jlimit(-0.94f, 0.94f, feedback * 0.88f);
        const float delaySec = delayMs * 0.001f;

        for (int i = 0; i < 256; ++i)
        {
            const float xNorm = (float) i / 255.0f;
            const float logFreq = 1.69897f + xNorm * (4.30103f - 1.69897f);
            const float freq = std::pow(10.0f, logFreq);
            const float phase = juce::MathConstants<float>::twoPi * freq * delaySec;
            const float baseMag = juce::jmax(1.0e-6f,
                dryGain * dryGain
                    + wetGain * wetGain
                    + 2.0f * dryGain * wetGain * polarity * std::cos(phase));
            const float regenDenom = juce::jmax(1.0e-5f,
                1.0f + regenFeedback * regenFeedback - 2.0f * regenFeedback * std::cos(phase));
            const float mag = std::sqrt(baseMag) * (0.78f + 0.22f / std::sqrt(regenDenom));
            const float magDb = 20.0f * std::log10(juce::jmax(mag, 1.0e-4f));
            float yNorm = 0.5f - (magDb / (2.0f * dbRange));
            yNorm = juce::jlimit(0.0f, 1.0f, yNorm);

            const float x = inner.getX() + xNorm * width;
            const float y = inner.getY() + yNorm * height;
            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }
    };

    juce::Path leftPath;
    computeResponse(delayL, leftPath);
    g.setColour(accentGlow.withAlpha(0.08f));
    g.strokePath(leftPath, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.18f));
    g.strokePath(leftPath, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.88f));
    g.strokePath(leftPath, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved));

    juce::Path rightPath;
    computeResponse(delayR, rightPath);
    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(rightPath, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.46f));
    g.strokePath(rightPath, juce::PathStrokeType(1.1f, juce::PathStrokeType::curved));

    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.setColour(accentGlow.withAlpha(0.56f));
    g.drawText("L", (int) inner.getX() + 2, (int) inner.getY() + 2, 14, 12, juce::Justification::centredLeft);
    g.setColour(accent.withAlpha(0.35f));
    g.drawText("R", (int) inner.getX() + 16, (int) inner.getY() + 2, 14, 12, juce::Justification::centredLeft);

    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.setColour(textDim.withAlpha(0.58f));
    g.drawText(juce::String(delayL, 2) + " / " + juce::String(delayR, 2) + " ms",
        (int) (inner.getRight() - 92.0f), (int) inner.getY() + 2, 88, 12, juce::Justification::centredRight);
}

inline void FlangerEditor::resized()
{
    const int width = getWidth();

    modeLabel.setBounds(28, 60, 44, 18);
    modeBox.setBounds(78, 56, 134, 24);
    modeSummaryLabel.setBounds(226, 56, width - 470, 24);
    stereoHintLabel.setBounds(width - 260, 56, 232, 24);

    vizBounds = juce::Rectangle<float>(28.0f, 92.0f, (float) (width - 56), 156.0f);

    const int knobSize = 84;
    const int knobY = 286;
    const int labelH = 16;
    const int valueH = 16;
    const int slotW = width / 7;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup groups[] = {
        { sldRate, lblRate, valRate },
        { sldDepth, lblDepth, valDepth },
        { sldManual, lblManual, valManual },
        { sldFeedback, lblFeedback, valFeedback },
        { sldWidth, lblWidth, valWidth },
        { sldTone, lblTone, valTone },
        { sldMix, lblMix, valMix }
    };

    for (int i = 0; i < 7; ++i)
    {
        const int centreX = slotW * i + slotW / 2;
        groups[i].label.setBounds(centreX - 52, knobY, 104, labelH);
        groups[i].slider.setBounds(centreX - knobSize / 2, knobY + labelH + 4, knobSize, knobSize);
        groups[i].value.setBounds(centreX - 52, knobY + labelH + 4 + knobSize + 2, 104, valueH);
    }
}

inline juce::AudioProcessorEditor* FlangerPedal::createEditor()
{
    return new FlangerEditor(*this);
}
