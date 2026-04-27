#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"
#include <array>
#include <cmath>

class ClassicWahPedal;

class ClassicWahEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    ClassicWahEditor(ClassicWahPedal& pedal);
    ~ClassicWahEditor() override
    {
        stopTimer();
        for (auto* slider : knobs)
        {
            if (slider != nullptr)
                slider->setLookAndFeel(nullptr);
        }
        treadleSlider.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
    void timerCallback() override;
    void paintWahViz(juce::Graphics& g, juce::Rectangle<float> bounds);
    void syncModeFromProcessor();
    void syncControlSourceFromProcessor();
    void refreshShortcutUi();
    void updateShortcutAnimation();
    juce::String describeShortcutKey() const;
    juce::String describeMotionMode() const;
    juce::String describeControlSource() const;

    ClassicWahPedal& proc;

    static constexpr int kWidth = 940;
    static constexpr int kHeight = 620;

    const juce::Colour accent = juce::Colour::fromString("ffD97706");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFBBF24");
    const juce::Colour accentDim = juce::Colour::fromString("ffB45309");
    const juce::Colour bgDark = juce::Colour(0xff0A0D12);
    const juce::Colour bgPanel = juce::Colour(0xff111827);
    const juce::Colour bgCard = juce::Colour(0xff0F1724);
    const juce::Colour textBright = juce::Colour(0xffF6F2E9);
    const juce::Colour textDim = juce::Colour(0xff8B9BB0);

    juce::ComboBox modeBox;
    juce::ComboBox controlBox;
    juce::Label modeLabel;
    juce::Label controlLabel;
    juce::Label shortcutLabel;
    juce::Label shortcutValue;
    juce::Label helperLabel;
    juce::TextButton learnButton { "LEARN" };
    juce::TextButton resetButton { "RESET" };

    juce::Slider treadleSlider;
    juce::Label treadleLabel;
    juce::Label treadleValue;

    juce::Slider sldSensitivity;
    juce::Slider sldAttack;
    juce::Slider sldDecay;
    juce::Slider sldRange;
    juce::Slider sldResonance;
    juce::Slider sldVoice;
    juce::Slider sldMix;

    juce::Label lblSensitivity;
    juce::Label lblAttack;
    juce::Label lblDecay;
    juce::Label lblRange;
    juce::Label lblResonance;
    juce::Label lblVoice;
    juce::Label lblMix;

    juce::Label valSensitivity;
    juce::Label valAttack;
    juce::Label valDecay;
    juce::Label valRange;
    juce::Label valResonance;
    juce::Label valVoice;
    juce::Label valMix;

    std::array<juce::Slider*, 7> knobs
    {
        &sldSensitivity, &sldAttack, &sldDecay, &sldRange, &sldResonance, &sldVoice, &sldMix
    };

    juce::Rectangle<float> vizBounds;
    float displayFreq = 420.0f;
    float displaySweep = 0.46f;
    float displayManualSweep = 0.46f;
    float displayEnvelope = 0.0f;

    bool learningShortcut = false;
    bool shortcutActive = false;
    float shortcutBaseSweep = 0.46f;
    float shortcutAnimatedSweep = 0.46f;

    struct WahKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent = juce::Colour::fromString("ffD97706");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffFBBF24");

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
            g.setColour(juce::Colour(0xff1E2837));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.002f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.57f;
            juce::ColourGradient body(juce::Colour(0xff1B2430), cx, cy - knobRadius,
                juce::Colour(0xff0B1118), cx, cy + knobRadius, false);
            g.setGradientFill(body);
            g.fillEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2F4051));
            g.drawEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.72f;
            const float px = cx + std::sin(angle) * pointerLength;
            const float py = cy - std::cos(angle) * pointerLength;
            g.setColour(kAccentGlow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    struct TreadleLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent = juce::Colour::fromString("ffD97706");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffFBBF24");

        void drawLinearSlider(juce::Graphics& g,
            int x,
            int y,
            int w,
            int h,
            float sliderPos,
            float minSliderPos,
            float maxSliderPos,
            const juce::Slider::SliderStyle style,
            juce::Slider&) override
        {
            juce::ignoreUnused(minSliderPos, maxSliderPos, style);

            const auto area = juce::Rectangle<float>((float) x, (float) y, (float) w, (float) h).reduced(10.0f, 6.0f);
            const float trackWidth = juce::jmin(34.0f, area.getWidth() * 0.52f);
            const auto track = juce::Rectangle<float>(area.getCentreX() - trackWidth * 0.5f,
                area.getY(),
                trackWidth,
                area.getHeight());

            g.setColour(juce::Colour(0xff121C28));
            g.fillRoundedRectangle(track, 16.0f);
            g.setColour(juce::Colour(0xff253342));
            g.drawRoundedRectangle(track, 16.0f, 1.0f);

            const float sliderNorm = juce::jlimit(0.0f, 1.0f, (track.getBottom() - sliderPos) / track.getHeight());
            const float filledHeight = track.getHeight() * sliderNorm;
            const auto fill = juce::Rectangle<float>(track.getX(),
                track.getBottom() - filledHeight,
                track.getWidth(),
                filledHeight);

            juce::ColourGradient grad(kAccentGlow.withAlpha(0.90f), fill.getCentreX(), fill.getY(),
                kAccent.withAlpha(0.92f), fill.getCentreX(), fill.getBottom(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(fill, 16.0f);

            const float thumbH = 34.0f;
            const auto thumb = juce::Rectangle<float>(track.getX() - 10.0f,
                sliderPos - thumbH * 0.5f,
                track.getWidth() + 20.0f,
                thumbH);

            juce::ColourGradient thumbGrad(juce::Colour(0xff2C3745), thumb.getCentreX(), thumb.getY(),
                juce::Colour(0xff141C26), thumb.getCentreX(), thumb.getBottom(), false);
            g.setGradientFill(thumbGrad);
            g.fillRoundedRectangle(thumb, 12.0f);
            g.setColour(kAccentGlow.withAlpha(0.85f));
            g.drawRoundedRectangle(thumb, 12.0f, 1.2f);
        }
    } treadleLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClassicWahEditor)
};

inline ClassicWahEditor::ClassicWahEditor(ClassicWahPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);
    setWantsKeyboardFocus(true);

    auto configureCombo = [this](juce::ComboBox& box)
    {
        box.setColour(juce::ComboBox::backgroundColourId, bgCard);
        box.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff253447));
        box.setColour(juce::ComboBox::textColourId, textBright);
        box.setColour(juce::ComboBox::arrowColourId, accentGlow);
        addAndMakeVisible(box);
    };

    auto configureLabel = [this](juce::Label& label, const juce::String& text, bool value = false)
    {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(value ? juce::Justification::centredLeft : juce::Justification::centredLeft);
        label.setFont(juce::Font(juce::FontOptions(value ? 12.0f : 11.0f,
            value ? juce::Font::plain : juce::Font::bold)));
        label.setColour(juce::Label::textColourId, value ? textBright : textDim);
        addAndMakeVisible(label);
    };

    configureLabel(modeLabel, "MOTION");
    configureLabel(controlLabel, "CONTROL");
    configureLabel(shortcutLabel, "SHORTCUT");
    configureLabel(shortcutValue, {}, true);
    shortcutValue.setColour(juce::Label::textColourId, accentGlow);
    configureLabel(helperLabel, {}, true);
    helperLabel.setColour(juce::Label::textColourId, textDim.withAlpha(0.92f));
    helperLabel.setMinimumHorizontalScale(0.8f);

    configureCombo(modeBox);
    modeBox.addItemList(proc.modeParam != nullptr ? proc.modeParam->choices : juce::StringArray{ "Pedal" }, 1);
    modeBox.onChange = [this]
    {
        if (proc.modeParam == nullptr)
            return;

        proc.modeParam->beginChangeGesture();
        proc.modeParam->setValueNotifyingHost((float) modeBox.getSelectedItemIndex()
            / (float) juce::jmax(1, proc.modeParam->choices.size() - 1));
        proc.modeParam->endChangeGesture();
    };

    configureCombo(controlBox);
    controlBox.addItem("Mouse Wheel", 1);
    controlBox.addItem("Shortcut Hold", 2);
    controlBox.onChange = [this]
    {
        proc.setExternalControlSource((ClassicWahPedal::ExternalControlSource) controlBox.getSelectedItemIndex());
        learningShortcut = false;
        shortcutActive = false;
        shortcutBaseSweep = proc.sweepParam != nullptr ? proc.sweepParam->get() : 0.46f;
        shortcutAnimatedSweep = shortcutBaseSweep;
        refreshShortcutUi();
    };

    learnButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff132033));
    learnButton.setColour(juce::TextButton::textColourOffId, textBright);
    learnButton.onClick = [this]
    {
        learningShortcut = true;
        grabKeyboardFocus();
        refreshShortcutUi();
    };
    addAndMakeVisible(learnButton);

    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1A1420));
    resetButton.setColour(juce::TextButton::textColourOffId, textDim);
    resetButton.onClick = [this]
    {
        learningShortcut = false;
        proc.setShortcutKeyCode(juce::KeyPress::spaceKey);
        refreshShortcutUi();
    };
    addAndMakeVisible(resetButton);

    treadleLabel.setText("PEDAL", juce::dontSendNotification);
    treadleLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    treadleLabel.setColour(juce::Label::textColourId, textDim);
    treadleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(treadleLabel);

    treadleValue.setFont(juce::Font(juce::FontOptions(12.0f)));
    treadleValue.setColour(juce::Label::textColourId, accentGlow);
    treadleValue.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(treadleValue);

    treadleSlider.setSliderStyle(juce::Slider::LinearVertical);
    treadleSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    treadleSlider.setRange(0.0, 1.0, 0.001);
    treadleSlider.setLookAndFeel(&treadleLnF);
    treadleSlider.setValue(proc.sweepParam != nullptr ? proc.sweepParam->get() : 0.46f, juce::dontSendNotification);
    treadleSlider.onValueChange = [this]
    {
        proc.setSweepFromUI((float) treadleSlider.getValue());
    };
    addAndMakeVisible(treadleSlider);

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
        label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(label);

        value.setJustificationType(juce::Justification::centred);
        value.setFont(juce::Font(juce::FontOptions(11.0f)));
        value.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(value);
    };

    initKnob(sldSensitivity, lblSensitivity, valSensitivity, "SENSITIVITY", 0.0, 1.0, 0.58, 0.001);
    initKnob(sldAttack, lblAttack, valAttack, "ATTACK", 0.5, 30.0, 2.0, 0.1);
    initKnob(sldDecay, lblDecay, valDecay, "DECAY", 10.0, 800.0, 120.0, 1.0);
    initKnob(sldRange, lblRange, valRange, "RANGE", 0.0, 1.0, 0.76, 0.001);
    initKnob(sldResonance, lblResonance, valResonance, "RESONANCE", 0.5, 10.0, 4.2, 0.1);
    initKnob(sldVoice, lblVoice, valVoice, "VOICE", 0.0, 1.0, 0.36, 0.001);
    initKnob(sldMix, lblMix, valMix, "MIX", 0.0, 1.0, 1.0, 0.001);

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

    wireFloat(sldSensitivity, proc.sensitivityParam);
    wireFloat(sldAttack, proc.attackParam);
    wireFloat(sldDecay, proc.decayParam);
    wireFloat(sldRange, proc.rangeParam);
    wireFloat(sldResonance, proc.resonanceParam);
    wireFloat(sldVoice, proc.voiceParam);
    wireFloat(sldMix, proc.mixParam);

    syncModeFromProcessor();
    syncControlSourceFromProcessor();
    refreshShortcutUi();

    shortcutBaseSweep = proc.sweepParam != nullptr ? proc.sweepParam->get() : 0.46f;
    shortcutAnimatedSweep = shortcutBaseSweep;

    startTimerHz(30);
}

inline bool ClassicWahEditor::keyPressed(const juce::KeyPress& key)
{
    if (learningShortcut)
    {
        if (key == juce::KeyPress::escapeKey)
        {
            learningShortcut = false;
            refreshShortcutUi();
            return true;
        }

        proc.setShortcutKeyCode(key.getKeyCode());
        learningShortcut = false;
        refreshShortcutUi();
        return true;
    }

    return false;
}

inline void ClassicWahEditor::mouseDown(const juce::MouseEvent& event)
{
    juce::ignoreUnused(event);
    grabKeyboardFocus();
}

inline void ClassicWahEditor::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused(event);

    if (proc.getExternalControlSource() != ClassicWahPedal::ExternalControlSource::MouseWheel)
        return;

    const float delta = (wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX) * 0.09f;
    if (std::abs(delta) > 0.0001f)
        proc.nudgeSweep(delta);
}

inline void ClassicWahEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    syncSlider(sldSensitivity, proc.sensitivityParam);
    syncSlider(sldAttack, proc.attackParam);
    syncSlider(sldDecay, proc.decayParam);
    syncSlider(sldRange, proc.rangeParam);
    syncSlider(sldResonance, proc.resonanceParam);
    syncSlider(sldVoice, proc.voiceParam);
    syncSlider(sldMix, proc.mixParam);
    syncSlider(treadleSlider, proc.sweepParam);

    syncModeFromProcessor();
    syncControlSourceFromProcessor();
    updateShortcutAnimation();
    refreshShortcutUi();

    valSensitivity.setText(juce::String((int) std::round(sldSensitivity.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valAttack.setText(juce::String((float) sldAttack.getValue(), sldAttack.getValue() < 10.0 ? 1 : 0) + " ms", juce::dontSendNotification);
    valDecay.setText(juce::String((int) std::round(sldDecay.getValue())) + " ms", juce::dontSendNotification);
    valRange.setText(juce::String((int) std::round(sldRange.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valResonance.setText(juce::String((float) sldResonance.getValue(), 1) + "x", juce::dontSendNotification);

    const float voiceValue = (float) sldVoice.getValue();
    const char* voiceName = voiceValue < 0.33f ? "Dark" : voiceValue < 0.66f ? "Classic" : "Bright";
    valVoice.setText(voiceName, juce::dontSendNotification);
    valMix.setText(juce::String((int) std::round(sldMix.getValue() * 100.0)) + "%", juce::dontSendNotification);
    treadleValue.setText(juce::String((int) std::round(treadleSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);

    displayFreq += 0.24f * (proc.currentFreq - displayFreq);
    displaySweep += 0.24f * (proc.currentSweep - displaySweep);
    displayManualSweep += 0.24f * (proc.currentManualSweep - displayManualSweep);
    displayEnvelope += 0.24f * (proc.currentEnvelope - displayEnvelope);

    repaint(vizBounds.toNearestInt());
}

inline void ClassicWahEditor::syncModeFromProcessor()
{
    if (proc.modeParam != nullptr)
        modeBox.setSelectedItemIndex(proc.modeParam->getIndex(), juce::dontSendNotification);
}

inline void ClassicWahEditor::syncControlSourceFromProcessor()
{
    controlBox.setSelectedItemIndex((int) proc.getExternalControlSource(), juce::dontSendNotification);
}

inline juce::String ClassicWahEditor::describeShortcutKey() const
{
    const int keyCode = proc.getShortcutKeyCode();
    if (keyCode == juce::KeyPress::spaceKey)  return "SPACE";
    if (keyCode == juce::KeyPress::returnKey) return "ENTER";
    if (keyCode == juce::KeyPress::tabKey)    return "TAB";
    if (keyCode == juce::KeyPress::escapeKey) return "ESC";
    if (keyCode >= 'A' && keyCode <= 'Z')
        return juce::String::charToString((juce::juce_wchar) keyCode);
    if (keyCode >= '0' && keyCode <= '9')
        return juce::String::charToString((juce::juce_wchar) keyCode);

    return juce::KeyPress(keyCode, {}, 0).getTextDescription().toUpperCase();
}

inline juce::String ClassicWahEditor::describeMotionMode() const
{
    switch (proc.getMotionMode())
    {
        case ClassicWahPedal::MotionMode::Touch:
            return "Touch uses an envelope follower on the guitar attack.";

        case ClassicWahPedal::MotionMode::Hybrid:
            return "Hybrid keeps your treadle position and adds touch-driven lift.";

        case ClassicWahPedal::MotionMode::Pedal:
        default:
            return "Pedal follows the on-screen treadle and external controller only.";
    }
}

inline juce::String ClassicWahEditor::describeControlSource() const
{
    switch (proc.getExternalControlSource())
    {
        case ClassicWahPedal::ExternalControlSource::ShortcutHold:
            return "Shortcut Hold morphs from the slider position to toe-down while the key is held.";

        case ClassicWahPedal::ExternalControlSource::MouseWheel:
        default:
            return "Mouse Wheel is continuous and is the default treadle controller.";
    }
}

inline void ClassicWahEditor::refreshShortcutUi()
{
    if (learningShortcut)
    {
        shortcutValue.setText("Press any key", juce::dontSendNotification);
        helperLabel.setText("Capture a keyboard key or a USB footswitch that emulates a key. ESC cancels.",
            juce::dontSendNotification);
        learnButton.setButtonText("LISTENING");
        return;
    }

    shortcutValue.setText(describeShortcutKey(), juce::dontSendNotification);
    helperLabel.setText(describeMotionMode() + "  " + describeControlSource(), juce::dontSendNotification);
    learnButton.setButtonText("LEARN");
}

inline void ClassicWahEditor::updateShortcutAnimation()
{
    if (proc.getExternalControlSource() != ClassicWahPedal::ExternalControlSource::ShortcutHold || learningShortcut)
    {
        shortcutActive = false;
        shortcutBaseSweep = proc.sweepParam != nullptr ? proc.sweepParam->get() : shortcutBaseSweep;
        shortcutAnimatedSweep = shortcutBaseSweep;
        return;
    }

    const int keyCode = proc.getShortcutKeyCode();
    const bool keyDown = keyCode > 0 && juce::KeyPress::isKeyCurrentlyDown(keyCode);
    const float currentSweep = proc.sweepParam != nullptr ? proc.sweepParam->get() : shortcutAnimatedSweep;

    if (keyDown && !shortcutActive)
    {
        shortcutActive = true;
        shortcutBaseSweep = currentSweep;
        shortcutAnimatedSweep = currentSweep;
    }

    const float target = keyDown ? 1.0f : shortcutBaseSweep;
    shortcutAnimatedSweep += 0.22f * (target - shortcutAnimatedSweep);

    if (std::abs(currentSweep - shortcutAnimatedSweep) > 0.002f)
        proc.setSweepFromUI(shortcutAnimatedSweep);

    if (!keyDown && std::abs(shortcutAnimatedSweep - shortcutBaseSweep) <= 0.003f)
        shortcutActive = false;
}

inline void ClassicWahEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f, bgDark, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.0f);

    juce::ColourGradient glow(accent.withAlpha(0.34f),
        (float) getWidth() * 0.22f,
        0.0f,
        accent.withAlpha(0.0f),
        (float) getWidth() * 0.82f,
        0.0f,
        false);
    g.setGradientFill(glow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 2.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(26.0f, juce::Font::bold)));
    g.drawText("WAH", 28, 16, 120, 28, juce::Justification::centredLeft);
    g.setColour(accentGlow);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("One pedal. Manual, touch and hybrid sweep.", 28, 44, 360, 18, juce::Justification::centredLeft);

    paintWahViz(g, vizBounds);
}

inline void ClassicWahEditor::paintWahViz(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff091019));
    g.fillRoundedRectangle(bounds, 14.0f);
    g.setColour(juce::Colour(0xff1C2939));
    g.drawRoundedRectangle(bounds, 14.0f, 1.0f);

    auto inner = bounds.reduced(18.0f, 14.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float freq = juce::jlimit(140.0f, 6500.0f, displayFreq);
    const float range = proc.rangeParam != nullptr ? proc.rangeParam->get() : 0.76f;
    const float voice = proc.voiceParam != nullptr ? proc.voiceParam->get() : 0.36f;
    const float resonance = proc.resonanceParam != nullptr ? proc.resonanceParam->get() : 4.2f;

    auto freqToX = [inner, w](float frequency)
    {
        constexpr float minLog = 2.17609f;
        constexpr float maxLog = 3.81291f;
        const float t = (std::log10(juce::jlimit(150.0f, 6500.0f, frequency)) - minLog) / (maxLog - minLog);
        return inner.getX() + juce::jlimit(0.0f, 1.0f, t) * w;
    };

    const float gridFreqs[] = { 200.0f, 300.0f, 500.0f, 800.0f, 1200.0f, 2000.0f, 3200.0f, 5000.0f };
    const char* gridLabels[] = { "200", "300", "500", "800", "1.2k", "2k", "3.2k", "5k" };
    g.setFont(juce::Font(juce::FontOptions(8.0f)));

    for (int i = 0; i < 8; ++i)
    {
        const float x = freqToX(gridFreqs[i]);
        g.setColour(juce::Colour(0xff182231));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.30f));
        g.drawText(gridLabels[i], (int) (x - 16.0f), (int) inner.getBottom() + 3, 32, 10, juce::Justification::centred);
    }

    g.setColour(juce::Colour(0xff182231));
    g.drawLine(inner.getX(), inner.getY() + h * 0.50f, inner.getRight(), inner.getY() + h * 0.50f, 0.5f);

    const float minFreq = juce::jmap(voice, 300.0f, 470.0f);
    const float maxFreq = juce::jmap(voice, 1700.0f, 2850.0f) + range * 1750.0f;
    const float leftX = freqToX(minFreq);
    const float rightX = freqToX(maxFreq);
    g.setColour(accent.withAlpha(0.04f));
    g.fillRect(leftX, inner.getY(), rightX - leftX, h);
    g.setColour(accent.withAlpha(0.18f));
    g.drawLine(leftX, inner.getY(), leftX, inner.getBottom(), 0.5f);
    g.drawLine(rightX, inner.getY(), rightX, inner.getBottom(), 0.5f);

    juce::Path responsePath;
    constexpr int numPoints = 220;
    const float q = juce::jmax(0.6f, resonance * (0.84f + displaySweep * 0.45f));

    for (int i = 0; i < numPoints; ++i)
    {
        const float t = (float) i / (float) (numPoints - 1);
        const float logF = 2.17609f + t * (3.81291f - 2.17609f);
        const float f = std::pow(10.0f, logF);
        const float ratio = f / juce::jmax(120.0f, freq);
        const float logRatio = std::log2(ratio);
        const float bpMag = q / (1.0f + q * q * logRatio * logRatio * 4.0f);
        const float lpMag = 1.0f / std::sqrt(1.0f + std::pow(ratio, 4.0f));
        const float bodyBlend = juce::jmap(voice, 0.33f, 0.12f);
        const float mag = bpMag * (1.0f - bodyBlend) + lpMag * bodyBlend;
        const float magDb = 20.0f * std::log10(juce::jmax(mag, 1.0e-6f));
        constexpr float dbRange = 24.0f;
        const float yNorm = juce::jlimit(0.0f, 1.0f, 1.0f - (magDb + dbRange) / (2.0f * dbRange));
        const float px = inner.getX() + t * w;
        const float py = inner.getY() + yNorm * h;

        if (i == 0)
            responsePath.startNewSubPath(px, py);
        else
            responsePath.lineTo(px, py);
    }

    juce::Path fillPath(responsePath);
    fillPath.lineTo(inner.getRight(), inner.getBottom());
    fillPath.lineTo(inner.getX(), inner.getBottom());
    fillPath.closeSubPath();

    juce::ColourGradient fillGrad(accent.withAlpha(0.14f), inner.getX(), inner.getY(),
        accent.withAlpha(0.02f), inner.getX(), inner.getBottom(), false);
    g.setGradientFill(fillGrad);
    g.fillPath(fillPath);

    g.setColour(accent.withAlpha(0.08f));
    g.strokePath(responsePath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.85f));
    g.strokePath(responsePath, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved));

    const float manualX = inner.getX() + displayManualSweep * w;
    const float liveX = inner.getX() + displaySweep * w;

    g.setColour(textDim.withAlpha(0.35f));
    g.drawLine(manualX, inner.getY() + 10.0f, manualX, inner.getBottom() - 26.0f, 1.0f);

    juce::ColourGradient freqGlow(accentGlow.withAlpha(0.24f), liveX, inner.getY(),
        accent.withAlpha(0.0f), liveX, inner.getBottom(), false);
    g.setGradientFill(freqGlow);
    g.fillRect(liveX - 16.0f, inner.getY(), 32.0f, h - 18.0f);
    g.setColour(accentGlow);
    g.drawLine(liveX, inner.getY(), liveX, inner.getBottom() - 18.0f, 1.8f);

    const float envBarY = inner.getBottom() - 9.0f;
    g.setColour(juce::Colour(0xff1B2635));
    g.fillRoundedRectangle(inner.getX(), envBarY, w, 5.0f, 2.5f);
    g.setColour(accent.withAlpha(0.75f));
    g.fillRoundedRectangle(inner.getX(), envBarY, juce::jmax(2.0f, displayEnvelope * w), 5.0f, 2.5f);

    g.setColour(textDim.withAlpha(0.45f));
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.drawText("manual", (int) manualX - 24, (int) inner.getY() + 2, 48, 12, juce::Justification::centred);
    g.drawText("live", (int) liveX - 18, (int) inner.getY() + 18, 36, 12, juce::Justification::centred);
    g.drawText("envelope", (int) inner.getX(), (int) envBarY - 14, 70, 12, juce::Justification::centredLeft);

    g.setColour(accentGlow);
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    const juce::String freqLabel = freq >= 1000.0f
        ? juce::String(freq * 0.001f, 2) + " kHz"
        : juce::String((int) std::round(freq)) + " Hz";
    g.drawText(freqLabel, (int) liveX - 32, (int) inner.getY() + 2, 64, 14, juce::Justification::centred);
}

inline void ClassicWahEditor::resized()
{
    auto bounds = getLocalBounds().reduced(24);
    auto top = bounds.removeFromTop(96);
    auto bottom = bounds.removeFromBottom(180);

    auto topCards = top.removeFromTop(74);
    auto modeCard = topCards.removeFromLeft(220);
    topCards.removeFromLeft(16);
    auto controlCard = topCards.removeFromLeft(220);
    topCards.removeFromLeft(16);
    auto shortcutCard = topCards;

    modeLabel.setBounds(modeCard.removeFromTop(16));
    modeBox.setBounds(modeCard.removeFromTop(36));

    controlLabel.setBounds(controlCard.removeFromTop(16));
    controlBox.setBounds(controlCard.removeFromTop(36));

    shortcutLabel.setBounds(shortcutCard.removeFromTop(16));
    auto shortcutRow = shortcutCard.removeFromTop(32);
    shortcutValue.setBounds(shortcutRow.removeFromLeft(120));
    learnButton.setBounds(shortcutRow.removeFromLeft(96).reduced(0, 2));
    shortcutRow.removeFromLeft(8);
    resetButton.setBounds(shortcutRow.removeFromLeft(84).reduced(0, 2));
    shortcutCard.removeFromTop(4);
    helperLabel.setBounds(shortcutCard);

    auto middle = bounds;
    auto treadleArea = middle.removeFromRight(130);
    middle.removeFromRight(18);
    vizBounds = middle.removeFromTop(250).toFloat();

    treadleLabel.setBounds(treadleArea.removeFromTop(18));
    treadleSlider.setBounds(treadleArea.removeFromTop(214));
    treadleValue.setBounds(treadleArea.removeFromTop(18));

    const int knobSize = 82;
    const int slotW = bottom.getWidth() / 7;

    struct KnobGroup
    {
        juce::Slider& slider;
        juce::Label& label;
        juce::Label& value;
    };

    KnobGroup groups[] =
    {
        { sldSensitivity, lblSensitivity, valSensitivity },
        { sldAttack, lblAttack, valAttack },
        { sldDecay, lblDecay, valDecay },
        { sldRange, lblRange, valRange },
        { sldResonance, lblResonance, valResonance },
        { sldVoice, lblVoice, valVoice },
        { sldMix, lblMix, valMix }
    };

    const int knobY = bottom.getY() + 20;
    for (int i = 0; i < 7; ++i)
    {
        const int cx = bottom.getX() + slotW * i + slotW / 2;
        groups[i].label.setBounds(cx - 56, knobY, 112, 16);
        groups[i].slider.setBounds(cx - knobSize / 2, knobY + 18, knobSize, knobSize);
        groups[i].value.setBounds(cx - 56, knobY + 18 + knobSize + 4, 112, 16);
    }
}

inline juce::AudioProcessorEditor* ClassicWahPedal::createEditor()
{
    return new ClassicWahEditor(*this);
}
