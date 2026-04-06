#pragma once

#include <JuceHeader.h>

class ReverbPedal;

class ReverbEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    ReverbEditor(ReverbPedal& pedal);
    ~ReverbEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateModeButtons();
    void setMode(int mode);
    void toggleFreeze();
    void updateFreezeButton();
    void setShimmerPitch(int index);
    void updateShimmerPitchButtons();
    void paintDecayCurve(juce::Graphics& g, juce::Rectangle<float> bounds);

    ReverbPedal& proc;

    static constexpr int kWidth = 920;
    static constexpr int kHeight = 620;

    const juce::Colour accent { juce::Colour::fromString("ff2DD4BF") };
    const juce::Colour accentGlow { juce::Colour::fromString("ff5EEAD4") };
    const juce::Colour bgDark { 0xff0B0E14 };
    const juce::Colour panelDark { 0xff111827 };
    const juce::Colour textBright { 0xffF0EDE8 };
    const juce::Colour textDim { 0xff7B8BA0 };

    juce::TextButton btnSpring { "SPRING" }, btnPlate { "PLATE" }, btnHall { "HALL" },
        btnRoom { "ROOM" }, btnShimmer { "SHIMMER" }, btnCloud { "CLOUD" };
    juce::TextButton btnFreeze { "FREEZE" };

    // Shimmer pitch buttons (visible only in Shimmer mode)
    juce::TextButton btnPitch5th { "5TH" }, btnPitchOct { "OCT" },
        btnPitchOct5 { "OCT+5" }, btnPitch2Oct { "2 OCT" };

    juce::Slider sldDecay, sldTone, sldSize, sldDamping, sldBassCut,
        sldDiffusion, sldWidth, sldMod, sldPredelay, sldMix, sldDuck, sldSwell, sldGate;
    juce::Label lblDecay, lblTone, lblSize, lblDamping, lblBassCut,
        lblDiffusion, lblWidth, lblMod, lblPredelay, lblMix, lblDuck, lblSwell, lblGate;
    juce::Label valDecay, valTone, valSize, valDamping, valBassCut,
        valDiffusion, valWidth, valMod, valPredelay, valMix, valDuck, valSwell, valGate;

    juce::Rectangle<float> vizBounds;
    int cachedMode = -1;
    int cachedShimmerPitch = -1;
    bool cachedFreeze = false;

    struct ReverbKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour accent { juce::Colour::fromString("ff2DD4BF") };
        juce::Colour glow { juce::Colour::fromString("ff5EEAD4") };

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
            float sliderPos, float startAngle, float endAngle, juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(6.0f);
            const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float cx = area.getCentreX();
            const float cy = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcRadius = radius - 4.0f;

            juce::Path track;
            track.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff1E2A3A));
            g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));

            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
                g.setColour(accent.withAlpha(0.12f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(accent);
                g.strokePath(arc, juce::PathStrokeType(3.6f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.56f;
            juce::ColourGradient knobGrad(juce::Colour(0xff1C2838), cx, cy - knobRadius,
                juce::Colour(0xff0D1520), cx, cy + knobRadius, false);
            g.setGradientFill(knobGrad);
            g.fillEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2A3A4C));
            g.drawEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLength = knobRadius * 0.70f;
            const float px = cx + std::sin(angle) * pointerLength;
            const float py = cy - std::cos(angle) * pointerLength;
            g.setColour(glow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbEditor)
};

inline ReverbEditor::ReverbEditor(ReverbPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto setupButton = [this](juce::TextButton& button, int mode)
    {
        button.onClick = [this, mode] { setMode(mode); };
        addAndMakeVisible(button);
    };

    setupButton(btnSpring, 0);
    setupButton(btnPlate, 1);
    setupButton(btnHall, 2);
    setupButton(btnRoom, 3);
    setupButton(btnShimmer, 4);
    setupButton(btnCloud, 5);
    btnFreeze.onClick = [this] { toggleFreeze(); };
    addAndMakeVisible(btnFreeze);

    auto setupPitchButton = [this](juce::TextButton& button, int pitchIdx)
    {
        button.onClick = [this, pitchIdx] { setShimmerPitch(pitchIdx); };
        addAndMakeVisible(button);
    };
    setupPitchButton(btnPitch5th, 0);
    setupPitchButton(btnPitchOct, 1);
    setupPitchButton(btnPitchOct5, 2);
    setupPitchButton(btnPitch2Oct, 3);

    auto setupKnob = [this](juce::Slider& slider, juce::Label& label, juce::Label& value,
                         const juce::String& name, double min, double max, double def, double step = 0.01)
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

    setupKnob(sldDecay, lblDecay, valDecay, "DECAY", 0.0, 1.0, 0.50);
    setupKnob(sldTone, lblTone, valTone, "TONE", 0.0, 1.0, 0.62);
    setupKnob(sldSize, lblSize, valSize, "SIZE", 0.0, 1.0, 0.50);
    setupKnob(sldDamping, lblDamping, valDamping, "DAMP", 0.0, 1.0, 0.35);
    setupKnob(sldBassCut, lblBassCut, valBassCut, "LOW CUT", 0.0, 1.0, 0.20);
    setupKnob(sldDiffusion, lblDiffusion, valDiffusion, "DIFFUSE", 0.0, 1.0, 0.75);
    setupKnob(sldWidth, lblWidth, valWidth, "WIDTH", 0.0, 1.0, 0.90);
    setupKnob(sldMod, lblMod, valMod, "MOD", 0.0, 1.0, 0.30);
    setupKnob(sldPredelay, lblPredelay, valPredelay, "PREDELAY", 0.0, 250.0, 15.0, 1.0);
    setupKnob(sldMix, lblMix, valMix, "MIX", 0.0, 1.0, 0.28);
    setupKnob(sldDuck, lblDuck, valDuck, "DUCK", 0.0, 1.0, 0.0);
    setupKnob(sldSwell, lblSwell, valSwell, "SWELL", 0.0, 1.0, 0.0);
    setupKnob(sldGate, lblGate, valGate, "GATE", 0.0, 1.0, 0.0);

    auto wire = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param == nullptr)
            return;

        slider.setValue(param->get(), juce::dontSendNotification);
        slider.onDragStart = [param]
        {
            param->beginChangeGesture();
        };
        slider.onValueChange = [&slider, param]
        {
            param->setValueNotifyingHost(param->convertTo0to1((float) slider.getValue()));
        };
        slider.onDragEnd = [param]
        {
            param->endChangeGesture();
        };
    };

    wire(sldDecay, proc.decayParam);
    wire(sldTone, proc.toneParam);
    wire(sldSize, proc.sizeParam);
    wire(sldDamping, proc.dampingParam);
    wire(sldBassCut, proc.bassCutParam);
    wire(sldDiffusion, proc.diffusionParam);
    wire(sldWidth, proc.widthParam);
    wire(sldMod, proc.modParam);
    wire(sldPredelay, proc.predelayParam);
    wire(sldMix, proc.mixParam);
    wire(sldDuck, proc.duckParam);
    wire(sldSwell, proc.swellParam);
    wire(sldGate, proc.gateParam);

    startTimerHz(24);
}

inline ReverbEditor::~ReverbEditor()
{
    stopTimer();
    for (auto* slider : { &sldDecay, &sldTone, &sldSize, &sldDamping, &sldBassCut,
                          &sldDiffusion, &sldWidth, &sldMod, &sldPredelay, &sldMix, &sldDuck,
                          &sldSwell, &sldGate })
        slider->setLookAndFeel(nullptr);
}

inline void ReverbEditor::setMode(int mode)
{
    if (proc.modeParam == nullptr)
        return;

    const int count = proc.modeParam->choices.size();
    const float normalised = count > 1 ? (float) mode / (float) (count - 1) : 0.0f;
    proc.modeParam->beginChangeGesture();
    proc.modeParam->setValueNotifyingHost(normalised);
    proc.modeParam->endChangeGesture();
    updateModeButtons();
    repaint();
}

inline void ReverbEditor::toggleFreeze()
{
    if (proc.freezeParam == nullptr)
        return;

    proc.freezeParam->beginChangeGesture();
    proc.freezeParam->setValueNotifyingHost(proc.freezeParam->get() ? 0.0f : 1.0f);
    proc.freezeParam->endChangeGesture();
    updateFreezeButton();
    repaint();
}

inline void ReverbEditor::updateModeButtons()
{
    const int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;
    auto style = [&](juce::TextButton& button, bool active)
    {
        button.setColour(juce::TextButton::buttonColourId, active ? accent : juce::Colour(0xff1A2332));
        button.setColour(juce::TextButton::textColourOffId, active ? bgDark : textDim);
    };

    style(btnSpring, mode == 0);
    style(btnPlate, mode == 1);
    style(btnHall, mode == 2);
    style(btnRoom, mode == 3);
    style(btnShimmer, mode == 4);
    style(btnCloud, mode == 5);
    cachedMode = mode;

    // Show/hide shimmer pitch buttons based on mode
    const bool showPitch = (mode == 4);
    btnPitch5th.setVisible(showPitch);
    btnPitchOct.setVisible(showPitch);
    btnPitchOct5.setVisible(showPitch);
    btnPitch2Oct.setVisible(showPitch);
    if (showPitch) updateShimmerPitchButtons();
}

inline void ReverbEditor::updateFreezeButton()
{
    const bool isFrozen = proc.freezeParam != nullptr && proc.freezeParam->get();
    btnFreeze.setColour(juce::TextButton::buttonColourId, isFrozen ? accentGlow : juce::Colour(0xff1A2332));
    btnFreeze.setColour(juce::TextButton::textColourOffId, isFrozen ? bgDark : textDim);
    cachedFreeze = isFrozen;
}

inline void ReverbEditor::setShimmerPitch(int index)
{
    if (proc.shimmerPitchParam == nullptr)
        return;

    const int count = proc.shimmerPitchParam->choices.size();
    const float normalised = count > 1 ? (float) index / (float) (count - 1) : 0.0f;
    proc.shimmerPitchParam->beginChangeGesture();
    proc.shimmerPitchParam->setValueNotifyingHost(normalised);
    proc.shimmerPitchParam->endChangeGesture();
    updateShimmerPitchButtons();
    repaint();
}

inline void ReverbEditor::updateShimmerPitchButtons()
{
    const int pitch = proc.shimmerPitchParam != nullptr ? proc.shimmerPitchParam->getIndex() : 1;
    auto style = [&](juce::TextButton& button, bool active)
    {
        button.setColour(juce::TextButton::buttonColourId, active ? accentGlow : juce::Colour(0xff1A2332));
        button.setColour(juce::TextButton::textColourOffId, active ? bgDark : textDim);
    };

    style(btnPitch5th, pitch == 0);
    style(btnPitchOct, pitch == 1);
    style(btnPitchOct5, pitch == 2);
    style(btnPitch2Oct, pitch == 3);
    cachedShimmerPitch = pitch;
}

inline void ReverbEditor::timerCallback()
{
    auto sync = [](juce::Slider& slider, juce::AudioParameterFloat* param)
    {
        if (param != nullptr)
            slider.setValue(param->get(), juce::dontSendNotification);
    };

    sync(sldDecay, proc.decayParam);
    sync(sldTone, proc.toneParam);
    sync(sldSize, proc.sizeParam);
    sync(sldDamping, proc.dampingParam);
    sync(sldBassCut, proc.bassCutParam);
    sync(sldDiffusion, proc.diffusionParam);
    sync(sldWidth, proc.widthParam);
    sync(sldMod, proc.modParam);
    sync(sldPredelay, proc.predelayParam);
    sync(sldMix, proc.mixParam);
    sync(sldDuck, proc.duckParam);
    sync(sldSwell, proc.swellParam);
    sync(sldGate, proc.gateParam);

    if ((proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0) != cachedMode)
        updateModeButtons();

    if ((proc.shimmerPitchParam != nullptr ? proc.shimmerPitchParam->getIndex() : 1) != cachedShimmerPitch)
        updateShimmerPitchButtons();

    if ((proc.freezeParam != nullptr ? proc.freezeParam->get() : false) != cachedFreeze)
        updateFreezeButton();

    auto fmt = [](juce::Label& label, float value, const juce::String& suffix)
    {
        label.setText(juce::String(value, suffix == "%" ? 0 : 1) + suffix, juce::dontSendNotification);
    };

    fmt(valDecay, (float) sldDecay.getValue() * 100.0f, "%");
    fmt(valTone, (float) sldTone.getValue() * 100.0f, "%");
    fmt(valSize, (float) sldSize.getValue() * 100.0f, "%");
    fmt(valDamping, (float) sldDamping.getValue() * 100.0f, "%");
    fmt(valBassCut, (float) sldBassCut.getValue() * 100.0f, "%");
    fmt(valDiffusion, (float) sldDiffusion.getValue() * 100.0f, "%");
    fmt(valWidth, (float) sldWidth.getValue() * 100.0f, "%");
    fmt(valMod, (float) sldMod.getValue() * 100.0f, "%");
    fmt(valPredelay, (float) sldPredelay.getValue(), " ms");
    fmt(valMix, (float) sldMix.getValue() * 100.0f, "%");
    fmt(valDuck, (float) sldDuck.getValue() * 100.0f, "%");
    fmt(valSwell, (float) sldSwell.getValue() * 100.0f, "%");
    fmt(valGate, (float) sldGate.getValue() * 100.0f, "%");

    repaint(vizBounds.toNearestInt());
}

inline void ReverbEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f, bgDark, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 7.0f, 1.0f);
    g.setColour(textBright);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    g.drawText("REVERB", 28, 12, 220, 30, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Nimbus Commercial Series", 28, 40, 260, 18, juce::Justification::centredLeft);

    g.setColour(textDim);
    g.setFont(juce::Font(12.0f));
    g.drawText("Dense tails, shaped attacks, gated ambience and modern modulation.", 28, 62, 460, 18, juce::Justification::centredLeft);

    g.setColour(textDim);
    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.drawText("PERFORMANCE", btnFreeze.getX() - 96, btnFreeze.getY(), 90, btnFreeze.getHeight(),
        juce::Justification::centredRight);

    // Shimmer pitch section label
    if (btnPitch5th.isVisible())
    {
        g.setColour(textDim);
        g.setFont(juce::Font(10.0f, juce::Font::bold));
        g.drawText("SHIMMER PITCH", btnPitch5th.getX() - 90, btnPitch5th.getY(), 84, 24,
            juce::Justification::centredRight);
    }

    g.setColour(panelDark.withAlpha(0.55f));
    g.fillRoundedRectangle(vizBounds.expanded(0.0f, 0.0f), 8.0f);
    paintDecayCurve(g, vizBounds);
}

inline void ReverbEditor::paintDecayCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    auto shapeFor = [](int mode, float tPost, float decayRate, float size, float width)
    {
        switch (mode)
        {
            case 0: return std::exp(-tPost * decayRate) * (1.0f + 0.18f * std::sin(tPost * (28.0f + size * 8.0f)) * std::exp(-tPost * 6.0f));
            case 1: return (1.0f - std::exp(-tPost * 35.0f)) * std::exp(-tPost * decayRate * 0.85f) * (0.78f + size * 0.32f);
            case 3: return (1.0f - std::exp(-tPost * 22.0f)) * std::exp(-tPost * decayRate * 1.05f);
            case 4: return (1.0f - std::exp(-tPost * 7.5f)) * std::exp(-tPost * decayRate * 0.58f) * (1.0f + 0.10f * std::sin(tPost * 10.0f));
            case 5: return (1.0f - std::exp(-tPost * 4.0f)) * std::exp(-tPost * decayRate * 0.45f);
            default: return (1.0f - std::exp(-tPost * 6.0f)) * std::exp(-tPost * decayRate * 0.65f) * (0.94f + width * 0.16f);
        }
    };

    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 8.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 8.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 14.0f);
    const float widthPx = inner.getWidth();
    const float heightPx = inner.getHeight();
    const float baseY = inner.getBottom();
    const float decay = (float) sldDecay.getValue();
    const float predelay = (float) sldPredelay.getValue();
    const float tone = (float) sldTone.getValue();
    const float size = (float) sldSize.getValue();
    const float damping = (float) sldDamping.getValue();
    const float stereoWidth = (float) sldWidth.getValue();
    const float duck = (float) sldDuck.getValue();
    const float swell = (float) sldSwell.getValue();
    const float gate = (float) sldGate.getValue();
    const bool freeze = proc.freezeParam != nullptr && proc.freezeParam->get();
    const int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;
    const float pdNorm = juce::jlimit(0.0f, 0.25f, predelay / 600.0f);
    const float swellLead = swell * 0.14f;
    const float decayRate = 2.0f + (1.0f - decay) * 14.0f;

    juce::Path fillPath, strokePath;
    fillPath.startNewSubPath(inner.getX(), baseY);
    strokePath.startNewSubPath(inner.getX(), baseY);

    for (int px = 0; px < (int) widthPx; ++px)
    {
        const float t = (float) px / widthPx;
        float env = 0.0f;
        if (t >= pdNorm)
        {
            const float tPost = (t - pdNorm) / juce::jmax(0.01f, 1.0f - pdNorm);
            const float swellOpen = juce::jlimit(0.0f, 1.0f, (tPost - swellLead) / juce::jmax(0.05f, 0.14f + swellLead));
            const float swellShape = swellOpen * swellOpen * (3.0f - 2.0f * swellOpen);
            const float gateShape = 1.0f - gate * juce::jlimit(0.0f, 1.0f, (tPost - 0.22f) / 0.30f);
            env = shapeFor(mode, tPost, decayRate, size, stereoWidth)
                * (1.0f - damping * 0.12f)
                * (1.0f - duck * 0.28f)
                * (1.0f + (swellShape - 1.0f) * swell)
                * juce::jmax(0.05f, gateShape);
            if (freeze)
                env = juce::jmax(env, 0.62f);
        }

        const float y = baseY - env * heightPx * 0.88f;
        fillPath.lineTo(inner.getX() + (float) px, y);
        strokePath.lineTo(inner.getX() + (float) px, y);
    }

    fillPath.lineTo(inner.getRight(), baseY);
    fillPath.closeSubPath();

    juce::ColourGradient fill(accent.withAlpha(0.14f + tone * 0.10f), inner.getX(), inner.getY(),
        accent.withAlpha(0.02f), inner.getX(), baseY, false);
    g.setGradientFill(fill);
    g.fillPath(fillPath);
    g.setColour(accentGlow.withAlpha(0.82f));
    g.strokePath(strokePath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.16f));
    g.strokePath(strokePath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));

    const char* modeNames[] = { "SPRING", "PLATE", "HALL", "ROOM", "SHIMMER", "CLOUD" };
    g.setColour(textDim);
    g.setFont(juce::Font(11.0f, juce::Font::bold));
    g.drawText(modeNames[juce::jlimit(0, 5, mode)], (int) bounds.getRight() - 106, (int) bounds.getY() + 8, 92, 16, juce::Justification::centredRight);
}

inline void ReverbEditor::resized()
{
    const int buttonW = 126;
    const int buttonH = 30;
    const int buttonGap = 10;
    const int totalButtonW = buttonW * 3 + buttonGap * 2;
    int x = (getWidth() - totalButtonW) / 2;
    const int row1Y = 98;
    const int row2Y = 136;

    btnSpring.setBounds(x, row1Y, buttonW, buttonH); x += buttonW + buttonGap;
    btnPlate.setBounds(x, row1Y, buttonW, buttonH); x += buttonW + buttonGap;
    btnHall.setBounds(x, row1Y, buttonW, buttonH);

    x = (getWidth() - totalButtonW) / 2;
    btnRoom.setBounds(x, row2Y, buttonW, buttonH); x += buttonW + buttonGap;
    btnShimmer.setBounds(x, row2Y, buttonW, buttonH); x += buttonW + buttonGap;
    btnCloud.setBounds(x, row2Y, buttonW, buttonH);

    // Shimmer pitch buttons — right-aligned, between mode buttons and viz
    {
        const int pbW = 68, pbH = 24, pbGap = 6;
        const int totalPbW = pbW * 4 + pbGap * 3;
        int px = (getWidth() - totalPbW) / 2;
        const int pbY = 172;
        btnPitch5th.setBounds(px, pbY, pbW, pbH);  px += pbW + pbGap;
        btnPitchOct.setBounds(px, pbY, pbW, pbH);  px += pbW + pbGap;
        btnPitchOct5.setBounds(px, pbY, pbW, pbH); px += pbW + pbGap;
        btnPitch2Oct.setBounds(px, pbY, pbW, pbH);
        btnFreeze.setBounds(getWidth() - 146, pbY, 96, pbH);
    }

    vizBounds = juce::Rectangle<float>(32.0f, 204.0f, (float) (getWidth() - 64), 138.0f);

    struct Group { juce::Slider* slider; juce::Label* label; juce::Label* value; };
    const Group groups[] = {
        { &sldDecay, &lblDecay, &valDecay }, { &sldTone, &lblTone, &valTone }, { &sldSize, &lblSize, &valSize },
        { &sldDamping, &lblDamping, &valDamping }, { &sldBassCut, &lblBassCut, &valBassCut },
        { &sldDiffusion, &lblDiffusion, &valDiffusion }, { &sldWidth, &lblWidth, &valWidth }, { &sldMod, &lblMod, &valMod },
        { &sldPredelay, &lblPredelay, &valPredelay }, { &sldMix, &lblMix, &valMix }, { &sldDuck, &lblDuck, &valDuck },
        { &sldSwell, &lblSwell, &valSwell }, { &sldGate, &lblGate, &valGate }
    };

    const int knobSize = 84;
    const int labelH = 16;
    const int valueH = 16;
    const int left = 28;
    const int firstRowY = 362;
    const int secondRowY = 486;
    const int topCount = 7;
    const int bottomCount = 6;
    const int topSlotW = (getWidth() - left * 2) / topCount;
    const int bottomSlotW = (getWidth() - left * 2) / bottomCount;

    for (int i = 0; i < 13; ++i)
    {
        const bool topRow = i < topCount;
        const int col = topRow ? i : (i - topCount);
        const int slotW = topRow ? topSlotW : bottomSlotW;
        const int centreX = left + col * slotW + slotW / 2;
        const int y = topRow ? firstRowY : secondRowY;
        groups[i].label->setBounds(centreX - 54, y, 108, labelH);
        groups[i].slider->setBounds(centreX - knobSize / 2, y + labelH + 4, knobSize, knobSize);
        groups[i].value->setBounds(centreX - 54, y + labelH + knobSize + 8, 108, valueH);
    }

    updateModeButtons();
    updateFreezeButton();
}

inline juce::AudioProcessorEditor* ReverbPedal::createEditor()
{
    return new ReverbEditor(*this);
}
