#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class CompressorPedal;

class CompressorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    CompressorEditor(CompressorPedal& pedal);
    ~CompressorEditor() override
    {
        stopTimer();
        for (auto* s : allSliders())
            s->setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintCompressionCurve(juce::Graphics& g, juce::Rectangle<float> bounds);
    void paintGRMeter(juce::Graphics& g, juce::Rectangle<float> bounds);

    std::array<juce::Slider*, 8> allSliders()
    {
        return { &sldThresh, &sldRatio, &sldAttack, &sldRelease, &sldKnee, &sldFocus, &sldBlend, &sldMakeup };
    }

    CompressorPedal& proc;

    static constexpr int kWidth = 700;
    static constexpr int kHeight = 440;

    const juce::Colour accent = juce::Colour::fromString("ff34D399");
    const juce::Colour accentDim = juce::Colour::fromString("ff059669");
    const juce::Colour accentGlow = juce::Colour::fromString("ff6EE7B7");
    const juce::Colour bgDark = juce::Colour(0xff0B0E14);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim = juce::Colour(0xff7B8BA0);

    juce::Slider sldThresh, sldRatio, sldAttack, sldRelease, sldKnee, sldFocus, sldBlend, sldMakeup;
    juce::Label lblThresh, lblRatio, lblAttack, lblRelease, lblKnee, lblFocus, lblBlend, lblMakeup;
    juce::Label valThresh, valRatio, valAttack, valRelease, valKnee, valFocus, valBlend, valMakeup;

    juce::Rectangle<float> curveBounds;
    juce::Rectangle<float> grBounds;
    float displayGR = 0.0f;

    struct CompKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent = juce::Colour::fromString("ff34D399");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff6EE7B7");

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
            float sliderPos, float startAngle, float endAngle,
            juce::Slider&) override
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
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            const float knobRadius = radius * 0.56f;
            juce::ColourGradient grad(juce::Colour(0xff1C2838), cx, cy - knobRadius,
                juce::Colour(0xff0D1520), cx, cy + knobRadius, false);
            g.setGradientFill(grad);
            g.fillEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            g.setColour(juce::Colour(0xff2A3A4C));
            g.drawEllipse(cx - knobRadius, cy - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

            const float pointerLen = knobRadius * 0.70f;
            const float px = cx + std::sin(angle) * pointerLen;
            const float py = cy - std::cos(angle) * pointerLen;
            g.setColour(kAccentGlow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompressorEditor)
};

inline CompressorEditor::CompressorEditor(CompressorPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    auto initKnob = [this](juce::Slider& s, juce::Label& lbl, juce::Label& val,
        const juce::String& name, float min, float max, float def, float step = 0.01f)
    {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        s.setRange(min, max, step);
        s.setValue(def, juce::dontSendNotification);
        s.setLookAndFeel(&knobLnF);
        addAndMakeVisible(s);

        lbl.setText(name, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centred);
        lbl.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        lbl.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(lbl);

        val.setJustificationType(juce::Justification::centred);
        val.setFont(juce::Font(juce::FontOptions(11.0f)));
        val.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(val);
    };

    initKnob(sldThresh,  lblThresh,  valThresh,  "THRESH",  -42.0f, 0.0f,   -20.0f, 0.5f);
    initKnob(sldRatio,   lblRatio,   valRatio,   "RATIO",   1.0f,   12.0f,  4.0f,   0.1f);
    initKnob(sldAttack,  lblAttack,  valAttack,  "ATTACK",  1.0f,   80.0f,  12.0f,  0.5f);
    initKnob(sldRelease, lblRelease, valRelease, "RELEASE", 25.0f,  350.0f, 120.0f, 1.0f);
    initKnob(sldKnee,    lblKnee,    valKnee,    "KNEE",    1.0f,   12.0f,  6.0f,   0.1f);
    initKnob(sldFocus,   lblFocus,   valFocus,   "FOCUS",   0.0f,   1.0f,   0.55f,  0.01f);
    initKnob(sldBlend,   lblBlend,   valBlend,   "BLEND",   0.0f,   1.0f,   0.82f,  0.01f);
    initKnob(sldMakeup,  lblMakeup,  valMakeup,  "MAKEUP",  0.0f,   18.0f,  2.5f,   0.1f);

    auto wireParam = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p == nullptr)
            return;

        s.setValue(p->get(), juce::dontSendNotification);
        s.onValueChange = [&s, p]
        {
            p->beginChangeGesture();
            *p = (float) s.getValue();
            p->endChangeGesture();
        };
    };

    wireParam(sldThresh,  proc.thresholdParam);
    wireParam(sldRatio,   proc.ratioParam);
    wireParam(sldAttack,  proc.attackParam);
    wireParam(sldRelease, proc.releaseParam);
    wireParam(sldKnee,    proc.kneeParam);
    wireParam(sldFocus,   proc.focusParam);
    wireParam(sldBlend,   proc.blendParam);
    wireParam(sldMakeup,  proc.makeupParam);

    startTimerHz(30);
}

inline void CompressorEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };

    syncSlider(sldThresh,  proc.thresholdParam);
    syncSlider(sldRatio,   proc.ratioParam);
    syncSlider(sldAttack,  proc.attackParam);
    syncSlider(sldRelease, proc.releaseParam);
    syncSlider(sldKnee,    proc.kneeParam);
    syncSlider(sldFocus,   proc.focusParam);
    syncSlider(sldBlend,   proc.blendParam);
    syncSlider(sldMakeup,  proc.makeupParam);

    valThresh.setText(juce::String((float) sldThresh.getValue(), 1) + " dB", juce::dontSendNotification);
    valRatio.setText(juce::String((float) sldRatio.getValue(), 1) + ":1", juce::dontSendNotification);
    valAttack.setText(juce::String((float) sldAttack.getValue(), 1) + " ms", juce::dontSendNotification);
    valRelease.setText(juce::String((int) sldRelease.getValue()) + " ms", juce::dontSendNotification);
    valKnee.setText(juce::String((float) sldKnee.getValue(), 1) + " dB", juce::dontSendNotification);
    valFocus.setText(juce::String((int) (sldFocus.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valBlend.setText(juce::String((int) (sldBlend.getValue() * 100.0)) + "%", juce::dontSendNotification);
    valMakeup.setText("+" + juce::String((float) sldMakeup.getValue(), 1) + " dB", juce::dontSendNotification);

    const float targetGR = proc.getCurrentGainReductionDb();
    if (targetGR > displayGR)
        displayGR = targetGR;
    else
        displayGR = displayGR * 0.85f + targetGR * 0.15f;

    repaint(curveBounds.toNearestInt().expanded(2));
    repaint(grBounds.toNearestInt().expanded(2));
}

inline void CompressorEditor::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
        bgDark, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    juce::ColourGradient glow(accent.withAlpha(0.30f), (float) getWidth() * 0.2f, 0.0f,
        accent.withAlpha(0.0f), (float) getWidth() * 0.8f, 0.0f, false);
    g.setGradientFill(glow);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), 2.0f);

    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("COMPRESSOR", 28, 10, 250, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("Studio sidechain", 28, 33, 220, 18, juce::Justification::centredLeft);

    paintCompressionCurve(g, curveBounds);
    paintGRMeter(g, grBounds);
}

inline void CompressorEditor::paintCompressionCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(14.0f, 10.0f);
    const float width = inner.getWidth();
    const float height = inner.getHeight();

    const float threshDb = (float) sldThresh.getValue();
    const float ratio = (float) sldRatio.getValue();
    const float kneeDb = (float) sldKnee.getValue();
    constexpr float rangeDb = 48.0f;

    g.setFont(juce::Font(juce::FontOptions(7.0f)));
    for (float db = -48.0f; db <= 0.0f; db += 12.0f)
    {
        const float norm = (db + rangeDb) / rangeDb;
        const float x = inner.getX() + norm * width;
        const float y = inner.getBottom() - norm * height;

        g.setColour(juce::Colour(0xff1A2535));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.3f);
        g.drawLine(inner.getX(), y, inner.getRight(), y, 0.3f);

        g.setColour(textDim.withAlpha(0.3f));
        g.drawText(juce::String((int) db), (int) (x - 10), (int) (inner.getBottom() + 1), 20, 9, juce::Justification::centred);
    }

    g.setColour(juce::Colour(0xff1E2A3A).withAlpha(0.8f));
    g.drawLine(inner.getX(), inner.getBottom(), inner.getRight(), inner.getY(), 0.5f);

    juce::Path curvePath;
    const int steps = (int) width;

    for (int px = 0; px <= steps; ++px)
    {
        const float t = (float) px / (float) steps;
        const float inputDb = -rangeDb + t * rangeDb;
        const float reductionDb = CompressorPedal::computeGainReductionDb(inputDb, threshDb, ratio, kneeDb);
        const float outputDb = inputDb - reductionDb;
        const float outNorm = juce::jlimit(0.0f, 1.0f, (outputDb + rangeDb) / rangeDb);
        const float plotX = inner.getX() + t * width;
        const float plotY = inner.getBottom() - outNorm * height;

        if (px == 0)
            curvePath.startNewSubPath(plotX, plotY);
        else
            curvePath.lineTo(plotX, plotY);
    }

    g.setColour(accent.withAlpha(0.08f));
    g.strokePath(curvePath, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved));
    g.setColour(accentGlow.withAlpha(0.8f));
    g.strokePath(curvePath, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));

    const float threshNorm = (threshDb + rangeDb) / rangeDb;
    const float tx = inner.getX() + threshNorm * width;
    g.setColour(accent.withAlpha(0.4f));
    for (float dy = inner.getY(); dy < inner.getBottom(); dy += 6.0f)
        g.drawLine(tx, dy, tx, juce::jmin(dy + 3.0f, inner.getBottom()), 0.8f);

    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    g.setColour(textDim.withAlpha(0.4f));
    g.drawText("IN", (int) inner.getRight() - 14, (int) (inner.getBottom() - 12), 14, 10, juce::Justification::centredRight);
    g.drawText("OUT", (int) inner.getX() + 2, (int) inner.getY() + 2, 22, 10, juce::Justification::centredLeft);
}

inline void CompressorEditor::paintGRMeter(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(8.0f, 10.0f);
    const float meterW = inner.getWidth();
    const float meterH = inner.getHeight() - 16.0f;
    const float meterX = inner.getX();
    const float meterY = inner.getY() + 12.0f;

    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.setColour(textDim.withAlpha(0.5f));
    g.drawText("GR", (int) inner.getX(), (int) inner.getY(), (int) inner.getWidth(), 11, juce::Justification::centred);

    g.setColour(juce::Colour(0xff0D1520));
    g.fillRoundedRectangle(meterX, meterY, meterW, meterH, 2.0f);

    constexpr float maxGRDisplay = 24.0f;
    const float grNorm = juce::jlimit(0.0f, 1.0f, displayGR / maxGRDisplay);
    const float grBarH = grNorm * meterH;

    if (grBarH > 0.5f)
    {
        juce::Colour grColour;
        if (grNorm < 0.4f)
            grColour = accent;
        else if (grNorm < 0.7f)
            grColour = accent.interpolatedWith(juce::Colour(0xffFBBF24), (grNorm - 0.4f) / 0.3f);
        else
            grColour = juce::Colour(0xffFBBF24).interpolatedWith(juce::Colour(0xffEF4444), (grNorm - 0.7f) / 0.3f);

        g.setColour(grColour.withAlpha(0.7f));
        g.fillRoundedRectangle(meterX, meterY, meterW, grBarH, 2.0f);

        g.setColour(grColour.withAlpha(0.15f));
        g.fillRoundedRectangle(meterX - 2.0f, meterY, meterW + 4.0f, grBarH, 3.0f);
    }

    g.setFont(juce::Font(juce::FontOptions(7.0f)));
    for (float db = 0.0f; db <= 24.0f; db += 6.0f)
    {
        const float yPos = meterY + (db / maxGRDisplay) * meterH;
        g.setColour(juce::Colour(0xff2A3A4C));
        g.drawLine(meterX, yPos, meterX + meterW, yPos, 0.4f);
        g.setColour(textDim.withAlpha(0.3f));
        g.drawText(juce::String((int) db), (int) (meterX + meterW + 1), (int) (yPos - 4), 16, 8, juce::Justification::centredLeft);
    }

    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.setColour(accentGlow.withAlpha(0.7f));
    g.drawText("-" + juce::String(displayGR, 1), (int) inner.getX(), (int) (inner.getBottom() - 12), (int) inner.getWidth(), 12, juce::Justification::centred);
}

inline void CompressorEditor::resized()
{
    const int width = getWidth();
    const float grMeterW = 50.0f;
    const float gap = 10.0f;
    curveBounds = juce::Rectangle<float>(28.0f, 58.0f, (float) (width - 56) - grMeterW - gap, 130.0f);
    grBounds = juce::Rectangle<float>(curveBounds.getRight() + gap, 58.0f, grMeterW, 130.0f);

    const int columns = 4;
    const int left = 24;
    const int right = 24;
    const int slotW = (width - left - right) / columns;
    const int knobSize = 68;
    const int labelH = 16;
    const int valH = 16;
    const int row1Y = 208;
    const int row2Y = 316;

    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup row1[] = {
        { sldThresh,  lblThresh,  valThresh },
        { sldRatio,   lblRatio,   valRatio },
        { sldAttack,  lblAttack,  valAttack },
        { sldRelease, lblRelease, valRelease }
    };
    KnobGroup row2[] = {
        { sldKnee,    lblKnee,    valKnee },
        { sldFocus,   lblFocus,   valFocus },
        { sldBlend,   lblBlend,   valBlend },
        { sldMakeup,  lblMakeup,  valMakeup }
    };

    for (int i = 0; i < columns; ++i)
    {
        const int cx = left + slotW * i + slotW / 2;

        row1[i].lbl.setBounds(cx - 56, row1Y, 112, labelH);
        row1[i].s.setBounds(cx - knobSize / 2, row1Y + labelH + 2, knobSize, knobSize);
        row1[i].val.setBounds(cx - 56, row1Y + labelH + 2 + knobSize + 1, 112, valH);

        row2[i].lbl.setBounds(cx - 56, row2Y, 112, labelH);
        row2[i].s.setBounds(cx - knobSize / 2, row2Y + labelH + 2, knobSize, knobSize);
        row2[i].val.setBounds(cx - 56, row2Y + labelH + 2 + knobSize + 1, 112, valH);
    }
}

inline juce::AudioProcessorEditor* CompressorPedal::createEditor()
{
    return new CompressorEditor(*this);
}
