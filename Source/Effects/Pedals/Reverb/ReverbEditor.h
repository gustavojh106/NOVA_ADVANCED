#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class ReverbPedal;

class ReverbEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    ReverbEditor(ReverbPedal& pedal);
    ~ReverbEditor() override
    {
        stopTimer();
        sldDecay.setLookAndFeel(nullptr);
        sldTone.setLookAndFeel(nullptr);
        sldPredelay.setLookAndFeel(nullptr);
        sldMix.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateModeButtons();
    void paintDecayCurve(juce::Graphics& g, juce::Rectangle<float> bounds);
    void setMode(int mode);
    void setupSlider(juce::Slider& slider, juce::Label& label, juce::Label& valLabel,
        const juce::String& name);

    ReverbPedal& proc;

    static constexpr int kWidth = 700;
    static constexpr int kHeight = 450;

    // Accent palette
    const juce::Colour accent     = juce::Colour::fromString("ff2DD4BF");
    const juce::Colour accentDim  = juce::Colour::fromString("ff1A9E8F");
    const juce::Colour accentGlow = juce::Colour::fromString("ff5EEAD4");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    // Mode buttons
    juce::TextButton btnSpring{ "SPRING" }, btnPlate{ "PLATE" }, btnHall{ "HALL" };

    // Knobs
    juce::Slider sldDecay, sldTone, sldPredelay, sldMix;
    juce::Label lblDecay, lblTone, lblPredelay, lblMix;
    juce::Label valDecay, valTone, valPredelay, valMix;

    // Visualization bounds (set in resized)
    juce::Rectangle<float> vizBounds;

    // Knob LookAndFeel
    struct ReverbKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ff2DD4BF");
        juce::Colour kAccentGlow = juce::Colour::fromString("ff5EEAD4");

        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
            float sliderPos, float startAngle, float endAngle,
            juce::Slider&) override
        {
            const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(6.0f);
            const float r = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const float cx = area.getCentreX();
            const float cy = area.getCentreY();
            const float angle = startAngle + sliderPos * (endAngle - startAngle);
            const float arcR = r - 4.0f;

            // Background track
            {
                juce::Path track;
                track.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
                g.setColour(juce::Colour(0xff1E2A3A));
                g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
            }

            // Value arc + glow
            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            // Knob body
            const float kr = r * 0.56f;
            {
                juce::ColourGradient grad(juce::Colour(0xff1C2838), cx, cy - kr,
                    juce::Colour(0xff0D1520), cx, cy + kr, false);
                g.setGradientFill(grad);
                g.fillEllipse(cx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
                g.setColour(juce::Colour(0xff2A3A4C));
                g.drawEllipse(cx - kr, cy - kr, kr * 2.0f, kr * 2.0f, 1.0f);
            }

            // Pointer line + dot
            const float pLen = kr * 0.70f;
            const float px = cx + std::sin(angle) * pLen;
            const float py = cy - std::cos(angle) * pLen;
            g.setColour(kAccentGlow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    int cachedMode = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbEditor)
};

// ============================================================================
//  Inline implementation — included AFTER ReverbPedal is fully defined
// ============================================================================

inline ReverbEditor::ReverbEditor(ReverbPedal& pedal)
    : juce::AudioProcessorEditor(pedal), proc(pedal)
{
    setSize(kWidth, kHeight);

    // Mode buttons
    auto setupBtn = [this](juce::TextButton& btn, int mode)
    {
        btn.setClickingTogglesState(false);
        btn.onClick = [this, mode] { setMode(mode); };
        addAndMakeVisible(btn);
    };
    setupBtn(btnSpring, 0);
    setupBtn(btnPlate, 1);
    setupBtn(btnHall, 2);

    // Sliders
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
        lbl.setFont(juce::Font(11.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, textDim);
        addAndMakeVisible(lbl);

        val.setJustificationType(juce::Justification::centred);
        val.setFont(juce::Font(11.0f));
        val.setColour(juce::Label::textColourId, accentGlow);
        addAndMakeVisible(val);
    };

    initKnob(sldDecay,    lblDecay,    valDecay,    "DECAY",    0.0f, 1.0f, 0.5f);
    initKnob(sldTone,     lblTone,     valTone,     "TONE",     0.0f, 1.0f, 0.5f);
    initKnob(sldPredelay, lblPredelay, valPredelay, "PREDELAY", 0.0f, 150.0f, 20.0f, 1.0f);
    initKnob(sldMix,      lblMix,      valMix,      "MIX",      0.0f, 1.0f, 0.28f);

    // Wire sliders to parameters
    auto wireParam = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p == nullptr) return;
        s.setValue(p->get(), juce::dontSendNotification);
        s.onValueChange = [&s, p]
        {
            p->beginChangeGesture();
            *p = (float)s.getValue();
            p->endChangeGesture();
        };
    };
    wireParam(sldDecay,    proc.decayParam);
    wireParam(sldTone,     proc.toneParam);
    wireParam(sldPredelay, proc.predelayParam);
    wireParam(sldMix,      proc.mixParam);

    startTimerHz(20);
}

inline void ReverbEditor::setMode(int mode)
{
    if (proc.modeParam == nullptr) return;
    int numItems = proc.modeParam->choices.size();
    float norm = (numItems > 1) ? (float)mode / (float)(numItems - 1) : 0.0f;
    proc.modeParam->beginChangeGesture();
    proc.modeParam->setValueNotifyingHost(norm);
    proc.modeParam->endChangeGesture();
    updateModeButtons();
    repaint();
}

inline void ReverbEditor::updateModeButtons()
{
    int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;
    auto style = [&](juce::TextButton& btn, bool active)
    {
        btn.setColour(juce::TextButton::buttonColourId,
            active ? accent : juce::Colour(0xff1A2332));
        btn.setColour(juce::TextButton::buttonOnColourId, accent);
        btn.setColour(juce::TextButton::textColourOffId,
            active ? bgDark : textDim);
        btn.setColour(juce::TextButton::textColourOnId, bgDark);
    };
    style(btnSpring, mode == 0);
    style(btnPlate,  mode == 1);
    style(btnHall,   mode == 2);
    cachedMode = mode;
}

inline void ReverbEditor::timerCallback()
{
    // Sync sliders from host
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldDecay,    proc.decayParam);
    syncSlider(sldTone,     proc.toneParam);
    syncSlider(sldPredelay, proc.predelayParam);
    syncSlider(sldMix,      proc.mixParam);

    // Mode sync
    int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;
    if (mode != cachedMode)
        updateModeButtons();

    // Value readouts
    auto fmt = [](juce::Label& lbl, float v, const juce::String& suffix)
    {
        lbl.setText(juce::String(v, suffix == "%" ? 0 : 1) + suffix, juce::dontSendNotification);
    };
    fmt(valDecay,    (float)sldDecay.getValue() * 100.0f, "%");
    fmt(valTone,     (float)sldTone.getValue() * 100.0f, "%");
    fmt(valPredelay, (float)sldPredelay.getValue(), " ms");
    fmt(valMix,      (float)sldMix.getValue() * 100.0f, "%");

    repaint(vizBounds.toNearestInt());
}

inline void ReverbEditor::paint(juce::Graphics& g)
{
    // Background
    {
        juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
            bgDark, 0.0f, (float)getHeight(), false);
        g.setGradientFill(bg);
        g.fillAll();
    }

    // Subtle border
    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    // Top accent line
    {
        juce::ColourGradient glow(accent.withAlpha(0.30f), (float)getWidth() * 0.2f, 0.0f,
            accent.withAlpha(0.0f), (float)getWidth() * 0.8f, 0.0f, false);
        g.setGradientFill(glow);
        g.fillRect(0.0f, 0.0f, (float)getWidth(), 2.0f);
    }

    // Header text
    g.setColour(textBright);
    g.setFont(juce::Font(22.0f, juce::Font::bold));
    g.drawText("REVERB", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(13.0f));
    g.drawText("Nimbus", 28, 33, 200, 18, juce::Justification::centredLeft);

    // Visualization panel
    paintDecayCurve(g, vizBounds);
}

inline void ReverbEditor::paintDecayCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Inset panel background
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    const float inset = 12.0f;
    auto inner = bounds.reduced(inset, inset);
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const float baseY = inner.getBottom();

    const float decay = (float)sldDecay.getValue();
    const float pd = (float)sldPredelay.getValue();
    const float tone = (float)sldTone.getValue();
    const int mode = proc.modeParam != nullptr ? proc.modeParam->getIndex() : 0;

    // Compute decay curve
    const float pdNorm = juce::jlimit(0.0f, 0.25f, pd / 600.0f); // Predelay as fraction of display
    const float decayRate = 2.0f + (1.0f - decay) * 14.0f;

    juce::Path curvePath;
    curvePath.startNewSubPath(inner.getX(), baseY);

    for (int px = 0; px < (int)w; ++px)
    {
        float t = (float)px / w;
        float env = 0.0f;

        if (t >= pdNorm)
        {
            float tPost = (t - pdNorm) / juce::jmax(0.01f, 1.0f - pdNorm);

            switch (mode)
            {
                case 0: // Spring — bouncy decay with ripple
                {
                    float base = std::exp(-tPost * decayRate);
                    float ripple = 1.0f + 0.18f * std::sin(tPost * 28.0f) * std::exp(-tPost * 6.0f);
                    env = base * ripple;
                    break;
                }
                case 1: // Plate — dense, fast attack, smooth decay
                {
                    float attack = 1.0f - std::exp(-tPost * 35.0f);
                    env = attack * std::exp(-tPost * decayRate * 0.85f);
                    break;
                }
                case 2: // Hall — slow buildup, long tail
                default:
                {
                    float attack = 1.0f - std::exp(-tPost * 6.0f);
                    env = attack * std::exp(-tPost * decayRate * 0.65f);
                    break;
                }
            }
        }

        float y = baseY - env * h * 0.88f;
        if (px == 0 && t < pdNorm)
            curvePath.lineTo(inner.getX() + (float)px, baseY);
        else
            curvePath.lineTo(inner.getX() + (float)px, y);
    }

    // Close path for fill
    curvePath.lineTo(inner.getRight(), baseY);
    curvePath.closeSubPath();

    // Fill gradient
    {
        float bright = 0.14f + tone * 0.10f;
        juce::ColourGradient fill(accent.withAlpha(bright), inner.getX(), inner.getY(),
            accent.withAlpha(0.02f), inner.getX(), baseY, false);
        g.setGradientFill(fill);
        g.fillPath(curvePath);
    }

    // Stroke
    {
        // Re-create stroke-only path (without the closing bottom line)
        juce::Path strokePath;
        strokePath.startNewSubPath(inner.getX(), baseY);
        for (int px = 0; px < (int)w; ++px)
        {
            float t = (float)px / w;
            float env = 0.0f;

            if (t >= pdNorm)
            {
                float tPost = (t - pdNorm) / juce::jmax(0.01f, 1.0f - pdNorm);
                switch (mode)
                {
                    case 0:
                    {
                        float base = std::exp(-tPost * decayRate);
                        float ripple = 1.0f + 0.18f * std::sin(tPost * 28.0f) * std::exp(-tPost * 6.0f);
                        env = base * ripple;
                        break;
                    }
                    case 1:
                    {
                        float attack = 1.0f - std::exp(-tPost * 35.0f);
                        env = attack * std::exp(-tPost * decayRate * 0.85f);
                        break;
                    }
                    default:
                    {
                        float attack = 1.0f - std::exp(-tPost * 6.0f);
                        env = attack * std::exp(-tPost * decayRate * 0.65f);
                        break;
                    }
                }
            }

            strokePath.lineTo(inner.getX() + (float)px, baseY - env * h * 0.88f);
        }

        g.setColour(accentGlow.withAlpha(0.80f));
        g.strokePath(strokePath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));

        // Bright leading glow
        g.setColour(accent.withAlpha(0.15f));
        g.strokePath(strokePath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    }

    // Baseline
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawLine(inner.getX(), baseY, inner.getRight(), baseY, 0.5f);

    // Mode label in corner
    const char* modeNames[] = { "SPRING", "PLATE", "HALL" };
    g.setColour(accent.withAlpha(0.40f));
    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.drawText(modeNames[mode], bounds.getRight() - 70.0f, bounds.getY() + 6.0f,
        60.0f, 14.0f, juce::Justification::centredRight);
}

inline void ReverbEditor::resized()
{
    const int w = getWidth();

    // Mode buttons
    const int btnW = 110, btnH = 30, btnGap = 10;
    const int totalBtnW = 3 * btnW + 2 * btnGap;
    int bx = (w - totalBtnW) / 2;
    const int by = 56;
    btnSpring.setBounds(bx, by, btnW, btnH); bx += btnW + btnGap;
    btnPlate.setBounds(bx, by, btnW, btnH);  bx += btnW + btnGap;
    btnHall.setBounds(bx, by, btnW, btnH);

    // Visualization
    vizBounds = juce::Rectangle<float>(28.0f, 96.0f, (float)(w - 56), 126.0f);

    // Knobs row
    const int knobSize = 86;
    const int knobY = 240;
    const int labelH = 16;
    const int valH = 16;
    const int slotW = w / 4;

    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup knobs[] = {
        { sldDecay,    lblDecay,    valDecay },
        { sldTone,     lblTone,     valTone },
        { sldPredelay, lblPredelay, valPredelay },
        { sldMix,      lblMix,      valMix }
    };

    for (int i = 0; i < 4; ++i)
    {
        int cx = slotW * i + slotW / 2;
        knobs[i].lbl.setBounds(cx - 50, knobY, 100, labelH);
        knobs[i].s.setBounds(cx - knobSize / 2, knobY + labelH + 4, knobSize, knobSize);
        knobs[i].val.setBounds(cx - 50, knobY + labelH + 4 + knobSize + 2, 100, valH);
    }

    updateModeButtons();
}

// ---- Wire createEditor back to processor ----
inline juce::AudioProcessorEditor* ReverbPedal::createEditor()
{
    return new ReverbEditor(*this);
}
