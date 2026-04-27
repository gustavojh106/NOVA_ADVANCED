#pragma once

#include <JuceHeader.h>
#include "../../../Core/Constants.h"

class BoostPedal;

class BoostEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    BoostEditor(BoostPedal& pedal);
    ~BoostEditor() override
    {
        stopTimer();
        for (auto* s : allSliders)
            s->setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintResponseCurve(juce::Graphics& g, juce::Rectangle<float> bounds);

    BoostPedal& proc;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 420;

    // Golden accent (from catalog)
    const juce::Colour accent     = juce::Colour::fromString("ffEAB308");
    const juce::Colour accentDim  = juce::Colour::fromString("ffCA8A04");
    const juce::Colour accentGlow = juce::Colour::fromString("ffFDE047");
    const juce::Colour bgDark     = juce::Colour(0xff0B0E14);
    const juce::Colour bgPanel    = juce::Colour(0xff111827);
    const juce::Colour textBright = juce::Colour(0xffF0EDE8);
    const juce::Colour textDim    = juce::Colour(0xff7B8BA0);

    juce::Slider sldGain, sldTone, sldTight, sldChar, sldMid, sldLevel;
    juce::Label lblGain, lblTone, lblTight, lblChar, lblMid, lblLevel;
    juce::Label valGain, valTone, valTight, valChar, valMid, valLevel;

    std::array<juce::Slider*, 6> allSliders = {
        &sldGain, &sldTone, &sldTight, &sldChar, &sldMid, &sldLevel
    };

    juce::Rectangle<float> vizBounds;

    static float freqToNorm(float freq)
    {
        static const float logMin = std::log(20.0f);
        static const float logMax = std::log(20000.0f);
        return (std::log(juce::jlimit(20.0f, 20000.0f, freq)) - logMin) / (logMax - logMin);
    }

    static float normToFreq(float norm)
    {
        static const float logMin = std::log(20.0f);
        static const float logMax = std::log(20000.0f);
        return std::exp(logMin + norm * (logMax - logMin));
    }

    struct BoostKnobLnF : public juce::LookAndFeel_V4
    {
        juce::Colour kAccent     = juce::Colour::fromString("ffEAB308");
        juce::Colour kAccentGlow = juce::Colour::fromString("ffFDE047");

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

            {
                juce::Path track;
                track.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
                g.setColour(juce::Colour(0xff1E2A3A));
                g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
            }

            if (sliderPos > 0.005f)
            {
                juce::Path arc;
                arc.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
                g.setColour(kAccent.withAlpha(0.10f));
                g.strokePath(arc, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved));
                g.setColour(kAccent);
                g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved));
            }

            const float kr = r * 0.56f;
            {
                juce::ColourGradient grad(juce::Colour(0xff1C2838), cx, cy - kr,
                    juce::Colour(0xff0D1520), cx, cy + kr, false);
                g.setGradientFill(grad);
                g.fillEllipse(cx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
                g.setColour(juce::Colour(0xff2A3A4C));
                g.drawEllipse(cx - kr, cy - kr, kr * 2.0f, kr * 2.0f, 1.0f);
            }

            const float pLen = kr * 0.70f;
            const float px = cx + std::sin(angle) * pLen;
            const float py = cy - std::cos(angle) * pLen;
            g.setColour(kAccentGlow);
            g.drawLine(cx, cy, px, py, 2.0f);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }
    } knobLnF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BoostEditor)
};

// ============================================================================
//  Inline implementation
// ============================================================================

inline BoostEditor::BoostEditor(BoostPedal& pedal)
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

    initKnob(sldGain,  lblGain,  valGain,  "GAIN",      0.0f, 24.0f, 8.0f, 0.1f);
    initKnob(sldTone,  lblTone,  valTone,  "TONE",      0.0f, 1.0f,  0.58f);
    initKnob(sldTight, lblTight, valTight, "TIGHT",     0.0f, 1.0f,  0.24f);
    initKnob(sldChar,  lblChar,  valChar,  "CHARACTER", 0.0f, 1.0f,  0.0f);
    initKnob(sldMid,   lblMid,   valMid,   "MID",      -6.0f, 6.0f,  0.0f, 0.1f);
    initKnob(sldLevel, lblLevel, valLevel, "LEVEL",     0.5f, 2.0f,  1.0f);

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
    wireParam(sldGain,  proc.gainParam);
    wireParam(sldTone,  proc.toneParam);
    wireParam(sldTight, proc.tightParam);
    wireParam(sldChar,  proc.charParam);
    wireParam(sldMid,   proc.midParam);
    wireParam(sldLevel, proc.levelParam);

    startTimerHz(30);
}

inline void BoostEditor::timerCallback()
{
    auto syncSlider = [](juce::Slider& s, juce::AudioParameterFloat* p)
    {
        if (p != nullptr)
            s.setValue(p->get(), juce::dontSendNotification);
    };
    syncSlider(sldGain,  proc.gainParam);
    syncSlider(sldTone,  proc.toneParam);
    syncSlider(sldTight, proc.tightParam);
    syncSlider(sldChar,  proc.charParam);
    syncSlider(sldMid,   proc.midParam);
    syncSlider(sldLevel, proc.levelParam);

    // Gain (dB)
    {
        float db = (float)sldGain.getValue();
        valGain.setText("+" + juce::String(db, 1) + " dB", juce::dontSendNotification);
    }
    // Tone (%)
    valTone.setText(juce::String((int)(sldTone.getValue() * 100.0)) + "%", juce::dontSendNotification);
    // Tight (%)
    valTight.setText(juce::String((int)(sldTight.getValue() * 100.0)) + "%", juce::dontSendNotification);
    // Character label
    {
        float ch = (float)sldChar.getValue();
        const char* label = ch < 0.33f ? "Clean" : ch < 0.66f ? "Warm" : "Pushed";
        valChar.setText(label, juce::dontSendNotification);
    }
    // Mid (dB)
    {
        float db = (float)sldMid.getValue();
        juce::String sign = db >= 0.0f ? "+" : "";
        valMid.setText(sign + juce::String(db, 1) + " dB", juce::dontSendNotification);
    }
    // Level (dB)
    {
        float gain = (float)sldLevel.getValue();
        float db = 20.0f * std::log10(juce::jmax(gain, 0.001f));
        valLevel.setText((db >= 0 ? "+" : "") + juce::String(db, 1) + " dB", juce::dontSendNotification);
    }

    repaint(vizBounds.toNearestInt());
}

inline void BoostEditor::paint(juce::Graphics& g)
{
    // Background gradient
    {
        juce::ColourGradient bg(juce::Colour(0xff0E1219), 0.0f, 0.0f,
            bgDark, 0.0f, (float)getHeight(), false);
        g.setGradientFill(bg);
        g.fillAll();
    }

    // Border
    g.setColour(accent.withAlpha(0.12f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);

    // Top accent line
    {
        juce::ColourGradient glow(accent.withAlpha(0.30f), (float)getWidth() * 0.2f, 0.0f,
            accent.withAlpha(0.0f), (float)getWidth() * 0.8f, 0.0f, false);
        g.setGradientFill(glow);
        g.fillRect(0.0f, 0.0f, (float)getWidth(), 2.0f);
    }

    // Title
    g.setColour(textBright);
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("BOOSTER", 28, 10, 200, 28, juce::Justification::centredLeft);
    g.setColour(accent);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    {
        float ch = (float)sldChar.getValue();
        const char* subtitle = ch < 0.33f ? "Clean Boost" : ch < 0.66f ? "Warm Preamp" : "Pushed Drive";
        g.drawText(subtitle, 28, 33, 200, 18, juce::Justification::centredLeft);
    }

    paintResponseCurve(g, vizBounds);
}

inline void BoostEditor::paintResponseCurve(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // Background
    g.setColour(juce::Colour(0xff080C12));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colour(0xff1E2A3A));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);

    auto inner = bounds.reduced(36.0f, 12.0f);
    const float w = inner.getWidth();
    const float h = inner.getHeight();

    // Show tone shaping response (filters only, no gain) on -12 to +12 dB scale
    const float dbMin = -12.0f;
    const float dbMax = 12.0f;
    const float dbRange = dbMax - dbMin;

    auto dbToY = [&](float db) -> float
    {
        return inner.getBottom() - ((db - dbMin) / dbRange) * h;
    };

    // dB grid lines
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    for (float db : { -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f, 9.0f })
    {
        float y = dbToY(db);
        g.setColour(db == 0.0f ? juce::Colour(0xff2A3A50) : juce::Colour(0xff161E2C));
        g.drawLine(inner.getX(), y, inner.getRight(), y, db == 0.0f ? 1.0f : 0.5f);
        if (db == -6.0f || db == 0.0f || db == 6.0f)
        {
            g.setColour(textDim.withAlpha(0.5f));
            juce::String label = (db >= 0 ? "+" : "") + juce::String((int)db);
            g.drawText(label, (int)(bounds.getX() + 4), (int)(y - 6), 28, 12,
                juce::Justification::centredRight);
        }
    }

    // Frequency grid lines
    const float gridFreqs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    const char* gridLabels[] = { "50", "100", "200", "500", "1k", "2k", "5k", "10k" };
    for (int i = 0; i < 8; ++i)
    {
        float x = inner.getX() + freqToNorm(gridFreqs[i]) * w;
        g.setColour(juce::Colour(0xff161E2C));
        g.drawLine(x, inner.getY(), x, inner.getBottom(), 0.5f);
        g.setColour(textDim.withAlpha(0.4f));
        g.drawText(gridLabels[i], (int)(x - 16), (int)(inner.getBottom() + 2), 32, 12,
            juce::Justification::centred);
    }

    // Get current param values
    float tone  = (float)sldTone.getValue();
    float tight = (float)sldTight.getValue();
    float mid   = (float)sldMid.getValue();
    float gainDb = (float)sldGain.getValue();

    double sampleRate = proc.getSampleRate();
    if (sampleRate <= 0.0) sampleRate = 48000.0;

    // Build filter instances for magnitude evaluation
    using namespace Nova::BoostDSP;
    Biquad fInputHP, fMidPeak, fPresShelf, fAirLP;

    float hpFreq = juce::jmap(tight, 28.0f, 220.0f);
    float hpQ    = juce::jmap(tight, 0.707f, 1.2f);
    fInputHP.setHighPass(hpFreq, hpQ, sampleRate);

    fMidPeak.setPeak(1000.0f, mid, 0.7f, sampleRate);

    float presDb = juce::jmap(tone, -4.0f, 6.0f);
    fPresShelf.setHighShelf(1600.0f, presDb, 0.72f, sampleRate);

    float airFreq = juce::jmap(tone, 4200.0f, 18000.0f);
    fAirLP.setLowPass(airFreq, 0.72f, sampleRate);

    constexpr int numPoints = 300;

    // Combined tone shaping response
    juce::Path curvePath;
    juce::Path fillPath;
    float zeroDbY = dbToY(0.0f);

    fillPath.startNewSubPath(inner.getX(), zeroDbY);

    for (int i = 0; i < numPoints; ++i)
    {
        float xNorm = (float)i / (float)(numPoints - 1);
        float freq = normToFreq(xNorm);

        float mag = fInputHP.magnitudeAt(freq, sampleRate)
                  * fMidPeak.magnitudeAt(freq, sampleRate)
                  * fPresShelf.magnitudeAt(freq, sampleRate)
                  * fAirLP.magnitudeAt(freq, sampleRate);

        float db = 20.0f * std::log10(juce::jmax(mag, 1.0e-6f));
        db = juce::jlimit(dbMin, dbMax, db);

        float px = inner.getX() + xNorm * w;
        float py = dbToY(db);

        if (i == 0) curvePath.startNewSubPath(px, py);
        else curvePath.lineTo(px, py);

        fillPath.lineTo(px, py);
    }

    fillPath.lineTo(inner.getRight(), zeroDbY);
    fillPath.closeSubPath();

    // Fill between curve and 0dB
    g.setColour(accent.withAlpha(0.08f));
    g.fillPath(fillPath);

    // Curve glow + stroke
    g.setColour(accent.withAlpha(0.06f));
    g.strokePath(curvePath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved));
    g.setColour(accent.withAlpha(0.85f));
    g.strokePath(curvePath, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));

    // Gain level indicator (dashed line showing boost amount in passband)
    if (gainDb > 0.5f)
    {
        // Show gain as a label in the top-right of the viz
        g.setColour(accentGlow.withAlpha(0.6f));
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText("+" + juce::String(gainDb, 1) + " dB",
            (int)(inner.getRight() - 90), (int)(inner.getY() + 4), 84, 16,
            juce::Justification::centredRight);
    }

    // Character indicator (small saturation curve inset in top-left of viz)
    {
        float ch = (float)sldChar.getValue();
        if (ch > 0.05f)
        {
            auto inset = juce::Rectangle<float>(inner.getX() + 4, inner.getY() + 4, 48.0f, 32.0f);
            g.setColour(juce::Colour(0xff0D1520).withAlpha(0.7f));
            g.fillRoundedRectangle(inset, 3.0f);
            g.setColour(juce::Colour(0xff1E2A3A).withAlpha(0.5f));
            g.drawRoundedRectangle(inset, 3.0f, 0.5f);

            // Mini transfer curve
            juce::Path satPath;
            auto si = inset.reduced(4.0f);
            for (int i = 0; i <= 30; ++i)
            {
                float t = (float)i / 30.0f;
                float input = t * 2.0f - 1.0f; // -1 to +1
                float output = Nova::BoostDSP::boostClip(input, ch);
                float px = si.getX() + t * si.getWidth();
                float py = si.getCentreY() - output * si.getHeight() * 0.45f;
                if (i == 0) satPath.startNewSubPath(px, py);
                else satPath.lineTo(px, py);
            }
            g.setColour(accentGlow.withAlpha(0.6f));
            g.strokePath(satPath, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved));
        }
    }
}

inline void BoostEditor::resized()
{
    const int w = getWidth();

    vizBounds = juce::Rectangle<float>(28.0f, 54.0f, (float)(w - 56), 140.0f);

    const int knobSize = 74;
    const int labelH = 16;
    const int valH = 16;
    const int rowSpacing = knobSize + labelH + valH + 12;

    // Row 1: Gain, Tone, Tight
    struct KnobGroup { juce::Slider& s; juce::Label& lbl; juce::Label& val; };
    KnobGroup topRow[] = {
        { sldGain,  lblGain,  valGain },
        { sldTone,  lblTone,  valTone },
        { sldTight, lblTight, valTight }
    };
    KnobGroup bottomRow[] = {
        { sldChar,  lblChar,  valChar },
        { sldMid,   lblMid,   valMid },
        { sldLevel, lblLevel, valLevel }
    };

    const int topY = 214;
    const int slotW = w / 3;
    for (int i = 0; i < 3; ++i)
    {
        int cx = slotW * i + slotW / 2;
        topRow[i].lbl.setBounds(cx - 50, topY, 100, labelH);
        topRow[i].s.setBounds(cx - knobSize / 2, topY + labelH + 2, knobSize, knobSize);
        topRow[i].val.setBounds(cx - 50, topY + labelH + 2 + knobSize + 2, 100, valH);
    }

    const int bottomY = topY + rowSpacing;
    for (int i = 0; i < 3; ++i)
    {
        int cx = slotW * i + slotW / 2;
        bottomRow[i].lbl.setBounds(cx - 50, bottomY, 100, labelH);
        bottomRow[i].s.setBounds(cx - knobSize / 2, bottomY + labelH + 2, knobSize, knobSize);
        bottomRow[i].val.setBounds(cx - 50, bottomY + labelH + 2 + knobSize + 2, 100, valH);
    }
}

// ---- Wire createEditor ----
inline juce::AudioProcessorEditor* BoostPedal::createEditor()
{
    return new BoostEditor(*this);
}
