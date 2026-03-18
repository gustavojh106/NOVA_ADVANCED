#pragma once

#include "../Base/PremiumPedalUI.h"
#include "../Base/ProcessorBase.h"

class OverdrivePedal;

class OverdriveEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    OverdriveEditor(OverdrivePedal& pedal);
    ~OverdriveEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    static constexpr int kWidth = 230;
    static constexpr int kHeight = 244;

    static inline const juce::Colour accent      { juce::Colour::fromString("ffF06848") };
    static inline const juce::Colour accentBright { juce::Colour::fromString("ffFF8860") };
    static inline const juce::Colour bodyDark     { juce::Colour::fromString("ff0D1520") };
    static inline const juce::Colour bodyMid      { juce::Colour::fromString("ff1A2332") };
    static inline const juce::Colour borderCol    { juce::Colour::fromString("ff2A3548") };

    // ── Custom LookAndFeel for the big hero Drive knob ───────────────
    class HeroKnobLnF final : public juce::LookAndFeel_V4
    {
    public:
        HeroKnobLnF()
        {
            setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
            setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour::fromString("ff0D1520"));
            setColour(juce::Label::textColourId, juce::Colours::white);
        }

        juce::Label* createSliderTextBox(juce::Slider& slider) override
        {
            auto* label = LookAndFeel_V4::createSliderTextBox(slider);
            label->setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            label->setJustificationType(juce::Justification::centred);
            label->setBorderSize(juce::BorderSize<int>(1, 4, 1, 4));
            label->setColour(juce::Label::backgroundColourId, juce::Colour::fromString("ff0D1520"));
            label->setColour(juce::Label::outlineColourId, juce::Colour::fromString("ff2A3548"));
            label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.92f));
            return label;
        }

        void drawLabel(juce::Graphics& g, juce::Label& label) override
        {
            auto bounds = label.getLocalBounds().toFloat().reduced(0.5f, 1.0f);
            g.setColour(juce::Colour::fromString("ff111827"));
            g.fillRoundedRectangle(bounds, 4.0f);
            g.setColour(juce::Colour::fromString("ff2A3548"));
            g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
            g.setColour(label.findColour(juce::Label::textColourId));
            g.setFont(label.getFont());
            g.drawFittedText(label.getText(), label.getLocalBounds().reduced(4, 1),
                label.getJustificationType(), 1);
        }

        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
            float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
            juce::Slider& slider) override
        {
            const auto alpha = slider.isEnabled() ? 1.0f : 0.35f;
            const auto area = juce::Rectangle<float>((float)x, (float)y,
                (float)width, (float)height).reduced(6.0f);
            const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
            const auto centre = area.getCentre();
            const float angle = UI::KnobGeometry::upperArcAngle(rotaryStartAngle, rotaryEndAngle, sliderPos);

            const auto col   = juce::Colour::fromString("ffF06848");
            const auto glow  = juce::Colour::fromString("ffFF8860");

            // Soft outer halo
            g.setColour(col.withAlpha(0.06f * alpha));
            g.fillEllipse(area.expanded(8.0f));

            // Body – warm-dark gradient
            {
                juce::ColourGradient grad(
                    juce::Colour::fromString("ff1E1418"), centre.x, centre.y - radius,
                    juce::Colour::fromString("ff0D1520"), centre.x, centre.y + radius, false);
                g.setGradientFill(grad);
                g.fillEllipse(area);
            }

            // Inner face
            const auto face = area.reduced(radius * 0.14f);
            {
                juce::ColourGradient grad(
                    juce::Colour::fromString("ff2A1E28"), centre.x, face.getY(),
                    juce::Colour::fromString("ff151118"), centre.x, face.getBottom(), false);
                g.setGradientFill(grad);
                g.fillEllipse(face);
            }

            // Rim & face border
            g.setColour(juce::Colour::fromString("ff0B0E14").withMultipliedAlpha(alpha));
            g.drawEllipse(area, 1.5f);
            g.setColour(col.withAlpha(0.12f * alpha));
            g.drawEllipse(face, 1.0f);

            // Arc track
            const float arcR = radius + 4.0f;
            auto track = UI::KnobGeometry::createUpperArc(centre.x, centre.y, arcR,
                rotaryStartAngle, rotaryEndAngle);
            g.setColour(juce::Colour::fromString("ff2A3548").withMultipliedAlpha(alpha));
            g.strokePath(track, juce::PathStrokeType(5.0f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // Value arc + triple glow
            if (sliderPos > 0.001f)
            {
                auto valueArc = UI::KnobGeometry::createUpperArc(centre.x, centre.y, arcR,
                    rotaryStartAngle, angle);

                g.setColour(col.withMultipliedAlpha(alpha));
                g.strokePath(valueArc, juce::PathStrokeType(5.0f,
                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(glow.withAlpha(0.24f * alpha));
                g.strokePath(valueArc, juce::PathStrokeType(12.0f,
                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(col.withAlpha(0.06f * alpha));
                g.strokePath(valueArc, juce::PathStrokeType(22.0f,
                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            // Pointer
            const float pLen = radius * 0.66f;
            const float pW = 2.8f;
            const auto innerPoint = UI::KnobGeometry::pointOnUpperArc(centre.x, centre.y, radius * 0.16f, angle);
            const auto outerPoint = UI::KnobGeometry::pointOnUpperArc(centre.x, centre.y, pLen, angle);
            g.setColour(juce::Colours::white.withAlpha(0.95f * alpha));
            g.drawLine({ innerPoint, outerPoint }, pW);

            // Center dot
            g.setColour(juce::Colour::fromString("ff0B0E14").withMultipliedAlpha(alpha));
            g.fillEllipse(centre.x - 4.0f, centre.y - 4.0f, 8.0f, 8.0f);
            g.setColour(col.withAlpha(0.90f * alpha));
            g.drawEllipse(centre.x - 4.0f, centre.y - 4.0f, 8.0f, 8.0f, 1.2f);
        }
    };

    // ── Members ──────────────────────────────────────────────────────
    Nova::PedalUI::PedalLookAndFeel smallLnF{ accent };
    HeroKnobLnF heroLnF;

    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;
    };

    Knob driveKnob, toneKnob, textureKnob, mixKnob, levelKnob;

    OverdrivePedal& od;
    bool lastBypassState = false;

    void setupHero(Knob& k, const juce::String& name,
        juce::RangedAudioParameter* param,
        std::function<juce::String(double)> textFn);

    void setupSmall(Knob& k, const juce::String& name,
        juce::RangedAudioParameter* param,
        std::function<juce::String(double)> textFn);

    void timerCallback() override;
    bool isBypassed() const;
};

// ═══════════════════════════════════════════════════════════════════
//  INLINE IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════

#include "OverdrivePedal.h"

inline OverdriveEditor::OverdriveEditor(OverdrivePedal& pedal)
    : juce::AudioProcessorEditor(&pedal), od(pedal)
{
    using namespace Nova::PedalUI;

    setupHero(driveKnob, "DRIVE", od.getDriveParam(),
        [](double v) { return juce::String(juce::roundToInt(v)) + "%"; });

    setupSmall(toneKnob, "TONE", od.getToneParam(),
        [](double v) { return formatPercent((float)v); });

    setupSmall(textureKnob, "TEXTURE", od.getTextureParam(),
        [](double v) { return formatPercent((float)v); });

    setupSmall(mixKnob, "MIX", od.getMixParam(),
        [](double v) { return formatPercent((float)v); });

    setupSmall(levelKnob, "LEVEL", od.getLevelParam(),
        [](double v) { return formatPercent((float)v); });

    setSize(kWidth, kHeight);
    startTimerHz(18);
}

inline OverdriveEditor::~OverdriveEditor()
{
    stopTimer();
    driveKnob.slider.setLookAndFeel(nullptr);
    toneKnob.slider.setLookAndFeel(nullptr);
    textureKnob.slider.setLookAndFeel(nullptr);
    mixKnob.slider.setLookAndFeel(nullptr);
    levelKnob.slider.setLookAndFeel(nullptr);
}

inline void OverdriveEditor::setupHero(Knob& k, const juce::String& name,
    juce::RangedAudioParameter* param,
    std::function<juce::String(double)> textFn)
{
    k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 16);
    k.slider.setRotaryParameters(UI::KnobGeometry::knobStartAngleRadians(),
        UI::KnobGeometry::knobEndAngleRadians(), true);
    k.slider.setDoubleClickReturnValue(true,
        param->convertFrom0to1(param->getDefaultValue()));
    k.slider.textFromValueFunction = std::move(textFn);
    k.slider.valueFromTextFunction = [](const juce::String& t)
    { return t.retainCharacters("0123456789-+.").getDoubleValue(); };
    k.slider.setLookAndFeel(&heroLnF);

    k.label.setText(name, juce::dontSendNotification);
    k.label.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    k.label.setJustificationType(juce::Justification::centred);
    k.label.setInterceptsMouseClicks(false, false);
    k.label.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.7f));

    k.attachment = std::make_unique<juce::SliderParameterAttachment>(*param, k.slider);
    addAndMakeVisible(k.slider);
    addAndMakeVisible(k.label);
}

inline void OverdriveEditor::setupSmall(Knob& k, const juce::String& name,
    juce::RangedAudioParameter* param,
    std::function<juce::String(double)> textFn)
{
    k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 14);
    k.slider.setRotaryParameters(UI::KnobGeometry::knobStartAngleRadians(),
        UI::KnobGeometry::knobEndAngleRadians(), true);
    k.slider.setDoubleClickReturnValue(true,
        param->convertFrom0to1(param->getDefaultValue()));
    k.slider.textFromValueFunction = std::move(textFn);
    k.slider.valueFromTextFunction = [](const juce::String& t)
    { return t.retainCharacters("0123456789-+.").getDoubleValue(); };
    k.slider.setLookAndFeel(&smallLnF);

    k.label.setText(name, juce::dontSendNotification);
    k.label.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    k.label.setJustificationType(juce::Justification::centred);
    k.label.setInterceptsMouseClicks(false, false);
    k.label.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.6f));

    k.attachment = std::make_unique<juce::SliderParameterAttachment>(*param, k.slider);
    addAndMakeVisible(k.slider);
    addAndMakeVisible(k.label);
}

inline void OverdriveEditor::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool bypassed = isBypassed();

    // ── Body ─────────────────────────────────────────────────────
    if (bypassed)
    {
        g.setColour(juce::Colour::fromString("ff080A0E"));
        g.fillRoundedRectangle(bounds, 10.0f);
    }
    else
    {
        // Warm dark gradient body
        juce::ColourGradient bodyGrad(
            juce::Colour::fromString("ff14100E"), bounds.getCentreX(), 0.0f,
            bodyDark, bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(bodyGrad);
        g.fillRoundedRectangle(bounds, 10.0f);
    }

    // Border
    g.setColour(bypassed ? juce::Colour::fromString("ff1A2332") : accent.withAlpha(0.25f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.0f);

    // ── Heat line accent at top ──────────────────────────────────
    if (!bypassed)
    {
        auto heatLine = juce::Rectangle<float>(20.0f, 0.0f, bounds.getWidth() - 40.0f, 3.0f);
        juce::ColourGradient heatGrad(
            juce::Colours::transparentBlack, heatLine.getX(), heatLine.getCentreY(),
            accent.withAlpha(0.55f), heatLine.getCentreX(), heatLine.getCentreY(), false);
        heatGrad.addColour(1.0, juce::Colours::transparentBlack);
        g.setGradientFill(heatGrad);
        g.fillRect(heatLine);
    }

    // ── Badge header ─────────────────────────────────────────────
    const auto panelBounds = getLocalBounds().toFloat();
    auto badge = juce::Rectangle<float>(10.0f, 8.0f, panelBounds.getWidth() - 20.0f, 24.0f);
    g.setColour(bypassed ? juce::Colour::fromString("ff0B0E14") : juce::Colour::fromString("ff111827"));
    g.fillRoundedRectangle(badge, 6.0f);
    g.setColour(bypassed ? juce::Colour::fromString("ff1A2332") : accent.withAlpha(0.35f));
    g.drawRoundedRectangle(badge, 6.0f, 1.0f);

    // Title text
    g.setColour(bypassed ? juce::Colours::white.withAlpha(0.35f) : juce::Colours::white.withAlpha(0.72f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText("DRIVE  AURORA", badge.toNearestInt().reduced(8, 0), juce::Justification::centredLeft);

    // Status pill
    g.setColour(bypassed ? Nova::Colors::Error.withAlpha(0.5f) : accent);
    g.fillRoundedRectangle(badge.removeFromRight(8.0f).reduced(1.0f, 5.0f), 2.0f);

    // ── Separator line between hero and small knobs ──────────────
    if (!bypassed)
    {
        const float sepY = 154.0f;
        juce::ColourGradient sepGrad(
            juce::Colours::transparentBlack, 20.0f, sepY,
            accent.withAlpha(0.18f), panelBounds.getCentreX(), sepY, false);
        sepGrad.addColour(1.0, juce::Colours::transparentBlack);
        g.setGradientFill(sepGrad);
        g.fillRect(20.0f, sepY, panelBounds.getWidth() - 40.0f, 1.0f);
    }
}

inline void OverdriveEditor::resized()
{
    auto area = getLocalBounds().reduced(8);
    area.removeFromTop(32); // badge

    // Hero Drive knob — big, centered
    auto heroArea = area.removeFromTop(112);
    {
        auto knobArea = heroArea.withSizeKeepingCentre(100, 112);
        driveKnob.label.setBounds(knobArea.removeFromTop(14));
        driveKnob.slider.setBounds(knobArea);
    }

    area.removeFromTop(4); // separator gap

    // Four small knobs in a row
    auto row = area.removeFromTop(80);
    const int cellW = row.getWidth() / 4;

    auto layoutSmall = [&](Knob& k, int index)
    {
        auto cell = row.withTrimmedLeft(index * cellW).removeFromLeft(cellW).reduced(2, 1);
        k.label.setBounds(cell.removeFromTop(13));
        k.slider.setBounds(cell);
    };

    layoutSmall(toneKnob, 0);
    layoutSmall(textureKnob, 1);
    layoutSmall(mixKnob, 2);
    layoutSmall(levelKnob, 3);
}

inline void OverdriveEditor::timerCallback()
{
    const bool bp = isBypassed();
    if (bp != lastBypassState)
    {
        lastBypassState = bp;
        repaint();
    }
}

inline bool OverdriveEditor::isBypassed() const
{
    if (auto* base = dynamic_cast<const ProcessorBase*>(&od))
        return base->getBypassed();
    return false;
}

// ── OverdrivePedal::createEditor() ──────────────────────────────
inline juce::AudioProcessorEditor* OverdrivePedal::createEditor()
{
    return new OverdriveEditor(*this);
}
