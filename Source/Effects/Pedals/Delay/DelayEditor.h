#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class DelayPedal;

class DelayEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    DelayEditor(DelayPedal& pedal);
    ~DelayEditor() override
    {
        stopTimer();
        for (auto* slider : sliders)
            if (slider != nullptr)
                slider->setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintDelayViz(juce::Graphics& g, juce::Rectangle<float> bounds);
    void syncModeFromProcessor();
    void syncFreezeFromProcessor();
    juce::String describeTexture(float amount) const;

    DelayPedal& proc;

    static constexpr int kWidth = 780;
    static constexpr int kHeight = 520;

    const juce::Colour accent = juce::Colour::fromString("ff60A5FA");
    const juce::Colour accentGlow = juce::Colour::fromString("ff93C5FD");
    const juce::Colour accentDim = juce::Colour::fromString("ff3B82F6");
    const juce::Colour bgDark = juce::Colour(0xff09111A);
    const juce::Colour bgPanel = juce::Colour(0xff111827);
    const juce::Colour bgCard = juce::Colour(0xff0B1320);
    const juce::Colour textBright = juce::Colour(0xffF3F4F6);
    const juce::Colour textDim = juce::Colour(0xff8AA0B8);

    juce::ComboBox modeBox;
    juce::Label modeLabel;
    juce::TextButton freezeButton{ "FREEZE" };

    juce::Slider sldTime, sldFeedback, sldTone, sldSpread, sldTexture;
    juce::Slider sldDuck, sldSwell, sldReverse, sldMix;
    juce::Label lblTime, lblFeedback, lblTone, lblSpread, lblTexture;
    juce::Label lblDuck, lblSwell, lblReverse, lblMix;
    juce::Label valTime, valFeedback, valTone, valSpread, valTexture;
    juce::Label valDuck, valSwell, valReverse, valMix;

    std::array<juce::Slider*, 9> sliders
    {
        &sldTime, &sldFeedback, &sldTone, &sldSpread, &sldTexture,
        &sldDuck, &sldSwell, &sldReverse, &sldMix
    };

    std::array<juce::Label*, 9> valueLabels
    {
        &valTime, &valFeedback, &valTone, &valSpread, &valTexture,
        &valDuck, &valSwell, &valReverse, &valMix
    };

    juce::Rectangle<float> vizBounds;
    float animPhase = 0.0f;

    struct DelayKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent = juce::Colour::fromString("ff60A5FA");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff93C5FD");

        void drawRotarySlider(juce::Graphics& g,
            int x,
            int y,
            int w,
            int h,
            float sliderPos,
            float startAngle,
            float endAngle,
            juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(7.0f);
            const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float cx = area.getCentreX();
            const float cy = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcRadius = radius - 4.0f;

            juce::Path track;
            track.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff1D2A3C));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.002f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.12f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.6f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.57f;
            juce::ColourGradient body(juce::Colour(0xff192433), cx, cy - knobRadius,
                juce::Colour(0xff0B1119), cx, cy + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2B3F55));
            g.drawEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.70f;
            const float px = cx + std::sin(angle) * pointerLength;
            const float py = cy - std::cos(angle) * pointerLength;
            g.setColour(kAccentGlow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DelayEditor)
};

inline DelayEditor::DelayEditor(DelayPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& slider,
        juce::Label& label,
        juce::Label& value,
        const juce::String& name,
        double min,
        double max,
        double def,
        double step)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider.setRange(min, max, step);
        slider.setValue(def, juce::dontSendNotification);
        slider.setLookAndFeel(&knobLnF);
        addAndMakeVisible(slider);

        label.setText(name, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(11.0f, juce::Font::bold));
        label.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(label);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(11.0f));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(sldTime, lblTime, valTime, "TIME", 35.0, 2500.0, 480.0, 1.0);
    initKnob(sldFeedback, lblFeedback, valFeedback, "FEEDBACK", 0.0, 0.97, 0.46, 0.001);
    initKnob(sldTone, lblTone, valTone, "TONE", 600.0, 14000.0, 5800.0, 1.0);
    initKnob(sldSpread, lblSpread, valSpread, "SPREAD", 0.0, 1.0, 0.42, 0.001);
    initKnob(sldTexture, lblTexture, valTexture, "TEXTURE", 0.0, 1.0, 0.45, 0.001);
    initKnob(sldDuck, lblDuck, valDuck, "DUCK", 0.0, 1.0, 0.0, 0.001);
    initKnob(sldSwell, lblSwell, valSwell, "SWELL", 0.0, 1.0, 0.0, 0.001);
    initKnob(sldReverse, lblReverse, valReverse, "REVERSE", 0.0, 1.0, 0.0, 0.001);
    initKnob(sldMix, lblMix, valMix, "MIX", 0.0, 1.0, 0.32, 0.001);

    auto wireFloat = [](juce::Slider& slider, juce::AudioParameterFloat* param)
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

    wireFloat(sldTime, proc.timeParam);
    wireFloat(sldFeedback, proc.feedbackParam);
    wireFloat(sldTone, proc.toneParam);
    wireFloat(sldSpread, proc.spreadParam);
    wireFloat(sldTexture, proc.textureParam);
    wireFloat(sldDuck, proc.duckParam);
    wireFloat(sldSwell, proc.swellParam);
    wireFloat(sldReverse, proc.reverseParam);
    wireFloat(sldMix, proc.mixParam);

    modeLabel.setText("MODE", juce::dontSendNotification);
    modeLabel.setFont(juce::Font(11.0f, juce::Font::bold));
    modeLabel.setColour(juce::Label::textColourId, textDim);
    addAndMakeVisible(modeLabel);

    modeBox.addItemList(proc.modeParam != nullptr ? proc.modeParam->choices : juce::StringArray{ "Analog" }, 1);
    modeBox.setColour(juce::ComboBox::backgroundColourId, bgCard);
    modeBox.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff243447));
    modeBox.setColour(juce::ComboBox::textColourId, textBright);
    modeBox.setColour(juce::ComboBox::arrowColourId, accentGlow);
    modeBox.onChange = [this]
    {
        if (proc.modeParam == nullptr)
            return;

        proc.modeParam->beginChangeGesture();
        proc.modeParam->setValueNotifyingHost((float) (modeBox.getSelectedItemIndex())
            / (float) juce::jmax(1, proc.modeParam->choices.size() - 1));
        proc.modeParam->endChangeGesture();
    };
    addAndMakeVisible(modeBox);
    syncModeFromProcessor();

    freezeButton.setClickingTogglesState(true);
    freezeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff152130));
    freezeButton.setColour(juce::TextButton::buttonOnColourId, accentDim);
    freezeButton.setColour(juce::TextButton::textColourOffId, textDim);
    freezeButton.setColour(juce::TextButton::textColourOnId, textBright);
    freezeButton.onClick = [this]
    {
        if (proc.freezeParam == nullptr)
            return;

        proc.freezeParam->beginChangeGesture();
        proc.freezeParam->setValueNotifyingHost(freezeButton.getToggleState() ? 1.0f : 0.0f);
        proc.freezeParam->endChangeGesture();
    };
    addAndMakeVisible(freezeButton);
    syncFreezeFromProcessor();

    startTimerHz(30);
}

inline void DelayEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldTime, proc.timeParam);
    syncSlider(sldFeedback, proc.feedbackParam);
    syncSlider(sldTone, proc.toneParam);
    syncSlider(sldSpread, proc.spreadParam);
    syncSlider(sldTexture, proc.textureParam);
    syncSlider(sldDuck, proc.duckParam);
    syncSlider(sldSwell, proc.swellParam);
    syncSlider(sldReverse, proc.reverseParam);
    syncSlider(sldMix, proc.mixParam);

    syncModeFromProcessor();
    syncFreezeFromProcessor();

    const float timeMs = (float) sldTime.getValue();
    valTime.setText(timeMs >= 1000.0f ? juce::String(timeMs * 0.001f, 2) + " s"
                                      : juce::String((int) timeMs) + " ms",
        juce::dontSendNotification);
    valFeedback.setText(juce::String((int) std::round(sldFeedback.getValue() / 0.97 * 100.0)) + "%",
        juce::dontSendNotification);

    const float toneHz = (float) sldTone.getValue();
    valTone.setText(toneHz >= 1000.0f ? juce::String(toneHz * 0.001f, 1) + " kHz"
                                      : juce::String((int) toneHz) + " Hz",
        juce::dontSendNotification);

    const float spread = (float) sldSpread.getValue();
    valSpread.setText(spread < 0.25f ? "Mono"
                    : (spread < 0.65f ? "Stereo" : "Ping-Pong"),
        juce::dontSendNotification);

    valTexture.setText(describeTexture((float) sldTexture.getValue()), juce::dontSendNotification);
    valDuck.setText(juce::String((int) std::round(sldDuck.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valSwell.setText(juce::String((int) std::round(sldSwell.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valReverse.setText(juce::String((int) std::round(sldReverse.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valMix.setText(juce::String((int) std::round(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);

    animPhase += 0.025f;
    if (animPhase >= 1.0f)
        animPhase -= 1.0f;

    repaint(vizBounds.toNearestInt());
}

inline void DelayEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff0B1320), 0.0f, 0.0f,
        bgDark, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    juce::ColourGradient topGlow(accent.withAlpha(0.28f), (float) getWidth() * 0.18f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth() * 0.82f, 0.0f, false);
    g.setGradientFill(topGlow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 2.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    g.drawText("DELAY", 28, 12, 240, 30, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Orbit Flagship Series", 28, 38, 240, 18, juce::Justification::centredLeft);

    g.setColour(textDim);
    g.setFont(juce::Font(11.0f));
    g.drawText("Four hero voices with duck, swell, reverse and freeze", 28, 56, 360, 16, juce::Justification::centredLeft);

    paintDelayViz(g, vizBounds);
}

inline void DelayEditor::paintDelayViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(bgCard);
    g.fillRoundedRectangle(bounds, 8.0f);
    g.setColour(juce::Colour(0xff1D3044));
    g.drawRoundedRectangle(bounds, 8.0f, 1.0f);

    auto inner = bounds.reduced(16.0f, 14.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();
    const float baseY = inner.getBottom();

    const float feedback = (float) sldFeedback.getValue();
    const float spread = (float) sldSpread.getValue();
    const float texture = (float) sldTexture.getValue();
    const float duck = (float) sldDuck.getValue();
    const float swell = (float) sldSwell.getValue();
    const float reverse = (float) sldReverse.getValue();
    const float timeMs = (float) sldTime.getValue();
    const bool frozen = freezeButton.getToggleState();

    g.setColour(juce::Colour(0xff172434));
    g.drawLine(inner.getX(), baseY - height * 0.48f, inner.getRight(), baseY - height * 0.48f, 0.6f);

    const int maxTaps = frozen ? 14 : 12;
    const float totalMs = timeMs * (float) juce::jmax(3, maxTaps);
    const float barWidth = juce::jmin(width / (float) (maxTaps + 2) * 0.42f, 22.0f);

    for (int tap = 1; tap <= maxTaps; ++tap)
    {
        const float x = inner.getX() + (timeMs * (float) tap / totalMs) * width;
        const float decay = std::pow(juce::jlimit(0.05f, 0.995f, feedback), (float) tap);
        float amplitude = decay;
        amplitude *= 1.0f - duck * juce::jlimit(0.0f, 0.72f, 0.85f - 0.06f * (float) tap);
        amplitude *= 1.0f - swell * juce::jlimit(0.0f, 0.82f, 0.90f - 0.12f * (float) tap);
        amplitude *= 0.78f + texture * 0.25f;
        if (tap >= 3)
            amplitude = juce::jmin(amplitude * (1.0f + reverse * 0.20f), 1.0f);

        const float reverseShift = reverse * juce::jmin(width * 0.10f, (float) tap * 4.5f);
        const float pingPongOffset = spread > 0.35f ? (tap % 2 == 0 ? 1.0f : -1.0f) * barWidth * (0.5f + spread * 0.9f) : 0.0f;
        const float barX = x + pingPongOffset + reverseShift - barWidth * 0.5f;
        const float barH = height * 0.76f * amplitude;
        const float barY = baseY - barH;
        const float glowAlpha = juce::jlimit(0.06f, 0.28f, amplitude * (0.20f + texture * 0.16f));

        juce::ColourGradient glow(accent.withAlpha(glowAlpha), barX + barWidth * 0.5f, barY,
            accent.withAlpha(0.0f), barX + barWidth * 0.5f, baseY, false);
        g.setGradientFill(glow);
        g.fillRoundedRectangle(barX - 4.0f, barY, barWidth + 8.0f, barH, 4.0f);

        juce::ColourGradient fill(accentGlow.withAlpha(0.85f * amplitude), barX, barY,
            accent.withAlpha(0.40f + 0.30f * amplitude), barX, baseY, false);
        g.setGradientFill(fill);
        g.fillRoundedRectangle(barX, barY, barWidth, barH, 3.0f);

        g.setColour(accent.withAlpha(0.32f + 0.20f * amplitude));
        g.drawRoundedRectangle(barX, barY, barWidth, barH, 3.0f, 0.9f);
    }

    if (frozen)
    {
        const float padHeight = 18.0f + 8.0f * (0.5f + 0.5f * std::sin(animPhase * juce::MathConstants<float>::twoPi));
        g.setColour(accent.withAlpha(0.12f));
        g.fillRoundedRectangle(inner.getX() + 10.0f, inner.getY() + 12.0f, width - 20.0f, padHeight, 5.0f);
        g.setColour(accentGlow.withAlpha(0.72f));
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawText("HOLD", inner.toNearestInt().removeFromTop(28), juce::Justification::centred);
    }

    g.setFont(juce::Font(8.0f, juce::Font::bold));
    g.setColour(textDim.withAlpha(0.55f));
    g.drawText("DRY", (int) inner.getX(), (int) (baseY + 4.0f), 30, 10, juce::Justification::centredLeft);
    g.drawText(proc.modeParam != nullptr ? proc.modeParam->choices[proc.modeParam->getIndex()] : juce::String("Mode"),
        (int) inner.getRight() - 80, (int) (baseY + 4.0f), 80, 10, juce::Justification::centredRight);
}

inline juce::String DelayEditor::describeTexture(float amount) const
{
    if (amount < 0.22f)
        return "Clean";
    if (amount < 0.48f)
        return "Warm";
    if (amount < 0.72f)
        return "Worn";
    return "Bloom";
}

inline void DelayEditor::syncModeFromProcessor()
{
    if (proc.modeParam == nullptr)
        return;

    modeBox.setSelectedItemIndex(proc.modeParam->getIndex(), juce::dontSendNotification);
}

inline void DelayEditor::syncFreezeFromProcessor()
{
    if (proc.freezeParam == nullptr)
        return;

    freezeButton.setToggleState(proc.freezeParam->get(), juce::dontSendNotification);
}

inline void DelayEditor::resized()
{
    const int w = getWidth();

    vizBounds = juce::Rectangle<float>(28.0f, 92.0f, (float) (w - 56), 178.0f);

    modeLabel.setBounds(28, 286, 70, 20);
    modeBox.setBounds(28, 308, 180, 28);
    freezeButton.setBounds(w - 156, 304, 128, 34);

    const int knobSize = 82;
    const int labelH = 16;
    const int valueH = 16;
    const int topY = 352;
    const int bottomY = 438;
    const int slotW = w / 5;

    struct KnobGroup { juce::Slider& slider; juce::Label& label; juce::Label& value; };
    std::array<KnobGroup, 5> topKnobs
    {{
        { sldTime, lblTime, valTime },
        { sldFeedback, lblFeedback, valFeedback },
        { sldTone, lblTone, valTone },
        { sldSpread, lblSpread, valSpread },
        { sldTexture, lblTexture, valTexture }
    }};
    std::array<KnobGroup, 4> bottomKnobs
    {{
        { sldDuck, lblDuck, valDuck },
        { sldSwell, lblSwell, valSwell },
        { sldReverse, lblReverse, valReverse },
        { sldMix, lblMix, valMix }
    }};

    for (int i = 0; i < (int) topKnobs.size(); ++i)
    {
        const int cx = slotW * i + slotW / 2;
        topKnobs[(size_t) i].label.setBounds(cx - 54, topY, 108, labelH);
        topKnobs[(size_t) i].slider.setBounds(cx - knobSize / 2, topY + labelH + 4, knobSize, knobSize);
        topKnobs[(size_t) i].value.setBounds(cx - 54, topY + labelH + 4 + knobSize + 2, 108, valueH);
    }

    for (int i = 0; i < (int) bottomKnobs.size(); ++i)
    {
        const int cx = slotW * i + slotW / 2;
        bottomKnobs[(size_t) i].label.setBounds(cx - 54, bottomY, 108, labelH);
        bottomKnobs[(size_t) i].slider.setBounds(cx - knobSize / 2, bottomY + labelH + 4, knobSize, knobSize);
        bottomKnobs[(size_t) i].value.setBounds(cx - 54, bottomY + labelH + 4 + knobSize + 2, 108, valueH);
    }
}

inline juce::AudioProcessorEditor* DelayPedal::createEditor()
{
    return new DelayEditor(*this);
}
