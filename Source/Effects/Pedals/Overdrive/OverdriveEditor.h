#pragma once

#include <cmath>

#include "../Base/PremiumPedalUI.h"
#include "../Base/ProcessorBase.h"
#include <BinaryData.h>

class OverdrivePedal;

// Helper to load texture/asset images from BinaryData once
namespace Nova::Textures
{
    inline juce::Image loadOnce(const char* data, int size)
    {
        return juce::ImageCache::getFromMemory(data, size);
    }

    // --- Textures ---
    inline const juce::Image& rustedOrangeMetal()
    {
        static auto img = loadOnce(BinaryData::rusted_orange_metal_jpg, BinaryData::rusted_orange_metal_jpgSize);
        return img;
    }

    inline const juce::Image& brushedMetal()
    {
        static auto img = loadOnce(BinaryData::brushed_metal_jpg, BinaryData::brushed_metal_jpgSize);
        return img;
    }

    inline const juce::Image& carbonFiber()
    {
        static auto img = loadOnce(BinaryData::carbon_fiber_png, BinaryData::carbon_fiber_pngSize);
        return img;
    }

    // --- Filmstrip Knobs ---
    inline const juce::Image& knobOrange()
    {
        static auto img = loadOnce(BinaryData::knob_orange_png, BinaryData::knob_orange_pngSize);
        return img;
    }

    inline const juce::Image& knobBossSilver()
    {
        static auto img = loadOnce(BinaryData::knob_boss_silver_png, BinaryData::knob_boss_silver_pngSize);
        return img;
    }

    // --- Hardware details ---
    inline const juce::Image& screwPhillips()
    {
        static auto img = loadOnce(BinaryData::screw_phillips_silver_png, BinaryData::screw_phillips_silver_pngSize);
        return img;
    }

    inline const juce::Image& screwTorx()
    {
        static auto img = loadOnce(BinaryData::screw_torx_png, BinaryData::screw_torx_pngSize);
        return img;
    }

    inline const juce::Image& ledRedGreen()
    {
        static auto img = loadOnce(BinaryData::led_red_green_png, BinaryData::led_red_green_pngSize);
        return img;
    }

    // --- Filmstrip drawing helper ---
    inline void drawFilmstripFrame(juce::Graphics& g, const juce::Image& filmstrip,
        int numFrames, float sliderPos, juce::Rectangle<float> dest)
    {
        if (!filmstrip.isValid() || numFrames <= 0) return;
        const int frameIndex = juce::jlimit(0, numFrames - 1, (int)(sliderPos * (float)(numFrames - 1)));
        const int frameW = filmstrip.getWidth();
        const int frameH = filmstrip.getHeight() / numFrames;
        g.drawImage(filmstrip,
            (int)dest.getX(), (int)dest.getY(), (int)dest.getWidth(), (int)dest.getHeight(),
            0, frameIndex * frameH, frameW, frameH);
    }

    // --- Screw drawing helper ---
    inline void drawScrew(juce::Graphics& g, const juce::Image& screwImg, float cx, float cy, float size)
    {
        if (!screwImg.isValid()) return;
        g.drawImage(screwImg,
            (int)(cx - size * 0.5f), (int)(cy - size * 0.5f), (int)size, (int)size,
            0, 0, screwImg.getWidth(), screwImg.getHeight());
    }
}

class OverdriveEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    OverdriveEditor(OverdrivePedal& pedal);
    ~OverdriveEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    static constexpr int kWidth = 1080;
    static constexpr int kHeight = 760;

    static inline const juce::Colour accent       { juce::Colour::fromString("ffF06848") };
    static inline const juce::Colour accentBright { juce::Colour::fromString("ffFF9B70") };
    static inline const juce::Colour accentDeep   { juce::Colour::fromString("ff7B291A") };
    static inline const juce::Colour shellTop     { juce::Colour::fromString("ff2A1E14") };
    static inline const juce::Colour shellBottom  { juce::Colour::fromString("ff0E0A06") };
    static inline const juce::Colour panelTop     { juce::Colour::fromString("ff1E1610") };
    static inline const juce::Colour panelBottom  { juce::Colour::fromString("ff100C08") };
    static inline const juce::Colour borderCol    { juce::Colour::fromString("ff3D2E20") };
    static inline const juce::Colour metalBright  { juce::Colour::fromString("ffD8D0C8") };
    static inline const juce::Colour metalMid     { juce::Colour::fromString("ff9A9088") };
    static inline const juce::Colour metalDark    { juce::Colour::fromString("ff5A524A") };
    static inline const juce::Colour textStrong   { juce::Colour::fromString("ffF8F0E8") };
    static inline const juce::Colour textMuted    { juce::Colour::fromString("ffA89888") };
    static inline const juce::Colour textDim      { juce::Colour::fromString("ff7A6A5A") };

    class PremiumKnobLnF final : public juce::LookAndFeel_V4
    {
    public:
        PremiumKnobLnF(juce::Colour accentColour, juce::Colour glowColour, bool useHeroStyle)
            : accent(accentColour), glow(glowColour), hero(useHeroStyle)
        {
            setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
            setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            setColour(juce::Label::textColourId, juce::Colours::white);
        }

        juce::Label* createSliderTextBox(juce::Slider& slider) override
        {
            auto* label = LookAndFeel_V4::createSliderTextBox(slider);
            label->setFont(juce::Font(juce::FontOptions(hero ? 13.0f : 11.0f, juce::Font::bold)));
            label->setJustificationType(juce::Justification::centred);
            label->setBorderSize(juce::BorderSize<int>(0));
            label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(hero ? 0.96f : 0.90f));
            return label;
        }

        void drawLabel(juce::Graphics& g, juce::Label& label) override
        {
            auto bounds = label.getLocalBounds().toFloat().reduced(hero ? 2.0f : 1.0f, 1.0f);

            juce::ColourGradient back(shellTop.brighter(hero ? 0.14f : 0.06f),
                bounds.getCentreX(), bounds.getY(),
                shellBottom.darker(hero ? 0.08f : 0.02f), bounds.getCentreX(), bounds.getBottom(), false);
            g.setGradientFill(back);
            g.fillRoundedRectangle(bounds, hero ? 9.0f : 7.0f);

            g.setColour(juce::Colours::black.withAlpha(hero ? 0.55f : 0.45f));
            g.drawRoundedRectangle(bounds.translated(0.0f, 1.0f), hero ? 9.0f : 7.0f, 1.0f);

            g.setColour(accent.withAlpha(hero ? 0.42f : 0.26f));
            g.drawRoundedRectangle(bounds, hero ? 9.0f : 7.0f, 1.0f);

            auto topGlow = bounds.removeFromTop(hero ? 2.5f : 2.0f);
            juce::ColourGradient glowGrad(juce::Colours::transparentBlack, topGlow.getX(), topGlow.getCentreY(),
                glow.withAlpha(hero ? 0.55f : 0.30f), topGlow.getCentreX(), topGlow.getCentreY(), false);
            glowGrad.addColour(1.0, juce::Colours::transparentBlack);
            g.setGradientFill(glowGrad);
            g.fillRoundedRectangle(topGlow, hero ? 3.0f : 2.0f);

            g.setColour(label.findColour(juce::Label::textColourId));
            g.setFont(label.getFont());
            g.drawFittedText(label.getText(), label.getLocalBounds().reduced(6, 1),
                label.getJustificationType(), 1);
        }

        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
            float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
            juce::Slider& slider) override
        {
            const float alpha = slider.isEnabled() ? 1.0f : 0.35f;
            auto area = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height)
                .reduced(hero ? 14.0f : 10.0f, hero ? 10.0f : 8.0f);

            const float radius = juce::jmax(0.0f, juce::jmin(area.getWidth(), area.getHeight()) * 0.5f - (hero ? 6.0f : 4.0f));
            if (radius <= 0.0f)
                return;

            const auto centre = area.getCentre();
            const float angle = UI::KnobGeometry::upperArcAngle(rotaryStartAngle, rotaryEndAngle, sliderPos);
            const float arcRadius = radius + (hero ? 10.0f : 7.0f);
            const float trackThickness = hero ? 6.0f : 4.0f;

            // ---- Shadow under knob ----
            g.setColour(juce::Colours::black.withAlpha((hero ? 0.50f : 0.38f) * alpha));
            g.fillEllipse(centre.x - radius - 8.0f, centre.y - radius - 2.0f,
                (radius + 8.0f) * 2.0f, (radius + 10.0f) * 2.0f);

            // ---- Tick marks ----
            const int tickCount = hero ? 16 : 12;
            for (int i = 0; i <= tickCount; ++i)
            {
                const float t = (float)i / (float)tickCount;
                const float tickAngle = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
                const bool isMajor = (i % 4) == 0;
                const auto outer = UI::KnobGeometry::pointOnUpperArc(centre.x, centre.y,
                    arcRadius + (hero ? 6.0f : 4.5f), tickAngle);
                const auto inner = UI::KnobGeometry::pointOnUpperArc(centre.x, centre.y,
                    arcRadius - (isMajor ? (hero ? 8.0f : 6.0f) : (hero ? 4.5f : 3.0f)), tickAngle);

                g.setColour((isMajor ? juce::Colours::white : juce::Colour::fromString("ff6B7280"))
                    .withAlpha((hero ? 0.78f : 0.52f) * alpha));
                g.drawLine({ inner, outer }, isMajor ? 1.6f : 1.0f);
            }

            // ---- Arc track (background) ----
            auto track = UI::KnobGeometry::createUpperArc(centre.x, centre.y, arcRadius,
                rotaryStartAngle, rotaryEndAngle);
            g.setColour(juce::Colour::fromString("ff1A1410").withAlpha((hero ? 0.90f : 0.78f) * alpha));
            g.strokePath(track, juce::PathStrokeType(trackThickness + 2.0f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(juce::Colour::fromString("ff344154").withAlpha((hero ? 0.85f : 0.72f) * alpha));
            g.strokePath(track, juce::PathStrokeType(trackThickness,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // ---- Value arc (filled portion) ----
            if (sliderPos > 0.001f)
            {
                auto valueArc = UI::KnobGeometry::createUpperArc(centre.x, centre.y, arcRadius,
                    rotaryStartAngle, angle);

                // Outer glow
                g.setColour(glow.withAlpha((hero ? 0.22f : 0.14f) * alpha));
                g.strokePath(valueArc, juce::PathStrokeType(trackThickness + (hero ? 12.0f : 8.0f),
                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                // Main arc
                g.setColour(accent.withAlpha(alpha));
                g.strokePath(valueArc, juce::PathStrokeType(trackThickness,
                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                // Bright leading edge dot
                auto tipPt = UI::KnobGeometry::pointOnUpperArc(centre.x, centre.y, arcRadius, angle);
                g.setColour(glow.withAlpha((hero ? 0.85f : 0.65f) * alpha));
                const float dotR = hero ? 4.5f : 3.0f;
                g.fillEllipse(tipPt.x - dotR, tipPt.y - dotR, dotR * 2.0f, dotR * 2.0f);
            }

            // ---- Filmstrip knob image ----
            const auto& filmstrip = hero ? Nova::Textures::knobOrange() : Nova::Textures::knobBossSilver();
            const int numFrames = hero ? 101 : 101;
            const float knobSize = radius * 2.0f;
            auto knobDest = juce::Rectangle<float>(
                centre.x - knobSize * 0.5f, centre.y - knobSize * 0.5f, knobSize, knobSize);

            // Dark circle behind knob for clean edge
            g.setColour(juce::Colours::black.withAlpha(0.70f * alpha));
            g.fillEllipse(knobDest.expanded(2.0f));

            g.setOpacity(alpha);
            Nova::Textures::drawFilmstripFrame(g, filmstrip, numFrames, sliderPos, knobDest);
            g.setOpacity(1.0f);

            // Subtle ring highlight around knob edge
            g.setColour(juce::Colours::white.withAlpha((hero ? 0.10f : 0.06f) * alpha));
            g.drawEllipse(knobDest, 1.0f);

            // Accent ring glow on hero
            if (hero)
            {
                g.setColour(accent.withAlpha(0.12f * alpha));
                g.drawEllipse(knobDest.expanded(1.5f), 2.0f);
            }
        }

    private:
        juce::Colour accent;
        juce::Colour glow;
        bool hero = false;
    };

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        juce::String hint;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;
    };

    struct RangeLegend
    {
        juce::String minLabel;
        juce::String maxLabel;
        juce::String focusLabel;
    };

    PremiumKnobLnF heroLnF { accent, accentBright, true };
    PremiumKnobLnF controlLnF { accent, accentBright, false };

    Knob driveKnob, toneKnob, textureKnob, mixKnob, levelKnob;

    OverdrivePedal& od;
    bool lastBypassState = false;

    juce::Rectangle<int> headerBounds;
    juce::Rectangle<int> heroBounds;
    juce::Rectangle<int> infoBounds;
    juce::Rectangle<int> toneBounds;
    juce::Rectangle<int> textureBounds;
    juce::Rectangle<int> mixBounds;
    juce::Rectangle<int> levelBounds;

    void setupHero(Knob& k, const juce::String& name, const juce::String& hint,
        juce::RangedAudioParameter* param,
        std::function<juce::String(double)> textFn);

    void setupControl(Knob& k, const juce::String& name, const juce::String& hint,
        juce::RangedAudioParameter* param,
        std::function<juce::String(double)> textFn);

    void layoutHero(Knob& k, juce::Rectangle<int> bounds);
    void layoutControl(Knob& k, juce::Rectangle<int> bounds);

    void paintShell(juce::Graphics& g, const juce::Rectangle<float>& bounds, bool bypassed) const;
    void paintPanel(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerSize,
        juce::Colour tint, bool bypassed, float glowAlpha) const;
    void paintHeader(juce::Graphics& g, bool bypassed) const;
    void paintInfoPanel(juce::Graphics& g, bool bypassed) const;
    void paintKnobCard(juce::Graphics& g, const juce::Rectangle<int>& bounds,
        const Knob& k, const juce::String& descriptor, bool bypassed, bool hero) const;
    void paintChip(juce::Graphics& g, juce::Rectangle<float> bounds,
        const juce::String& caption, const juce::String& value, bool bypassed) const;
    void paintValueRail(juce::Graphics& g, juce::Rectangle<float> bounds,
        const Knob& k, bool bypassed) const;
    void paintAnalysisBar(juce::Graphics& g, juce::Rectangle<float> bounds,
        const juce::String& name, float amount, juce::Colour colour, bool bypassed) const;

    RangeLegend rangeLegendFor(const Knob& k) const;
    float normalizedValueFor(const Knob& k) const;
    float defaultNormalizedValueFor(const Knob& k) const;

    juce::String describeDrive(double value) const;
    juce::String describeTone(double value) const;
    juce::String describeTexture(double value) const;
    juce::String describeMix(double value) const;
    juce::String describeLevel(double value) const;

    juce::Path makeResponseCurve(juce::Rectangle<float> bounds) const;

    void timerCallback() override;
    bool isBypassed() const;
};

#include "OverdrivePedal.h"

inline OverdriveEditor::OverdriveEditor(OverdrivePedal& pedal)
    : juce::AudioProcessorEditor(&pedal), od(pedal)
{
    using namespace Nova::PedalUI;

    setupHero(driveKnob, "DRIVE", "Gain staging",
        od.getDriveParam(),
        [](double v) { return juce::String(juce::roundToInt(v)) + "%"; });

    setupControl(toneKnob, "TONE", "High-end focus",
        od.getToneParam(),
        [](double v) { return formatPercent((float)v); });

    setupControl(textureKnob, "TEXTURE", "Clip density",
        od.getTextureParam(),
        [](double v) { return formatPercent((float)v); });

    setupControl(mixKnob, "MIX", "Parallel blend",
        od.getMixParam(),
        [](double v) { return formatPercent((float)v); });

    setupControl(levelKnob, "LEVEL", "Output trim",
        od.getLevelParam(),
        [](double v) { return formatPercent((float)v); });

    setSize(kWidth, kHeight);
    startTimerHz(24);
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

inline void OverdriveEditor::setupHero(Knob& k, const juce::String& name, const juce::String& hint,
    juce::RangedAudioParameter* param,
    std::function<juce::String(double)> textFn)
{
    k.hint = hint;
    k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 82, 22);
    k.slider.setRotaryParameters(UI::KnobGeometry::knobStartAngleRadians(),
        UI::KnobGeometry::knobEndAngleRadians(), true);
    k.slider.setDoubleClickReturnValue(true,
        param->convertFrom0to1(param->getDefaultValue()));
    k.slider.setMouseDragSensitivity(235);
    k.slider.textFromValueFunction = std::move(textFn);
    k.slider.valueFromTextFunction = [](const juce::String& t)
    {
        return t.retainCharacters("0123456789-+.").getDoubleValue();
    };
    k.slider.onValueChange = [this] { repaint(); };
    k.slider.setLookAndFeel(&heroLnF);

    k.label.setText(name, juce::dontSendNotification);
    k.label.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    k.label.setJustificationType(juce::Justification::centred);
    k.label.setInterceptsMouseClicks(false, false);
    k.label.setColour(juce::Label::textColourId, textStrong.withAlpha(0.92f));

    k.attachment = std::make_unique<juce::SliderParameterAttachment>(*param, k.slider);
    addAndMakeVisible(k.slider);
    addAndMakeVisible(k.label);
}

inline void OverdriveEditor::setupControl(Knob& k, const juce::String& name, const juce::String& hint,
    juce::RangedAudioParameter* param,
    std::function<juce::String(double)> textFn)
{
    k.hint = hint;
    k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 62, 18);
    k.slider.setRotaryParameters(UI::KnobGeometry::knobStartAngleRadians(),
        UI::KnobGeometry::knobEndAngleRadians(), true);
    k.slider.setDoubleClickReturnValue(true,
        param->convertFrom0to1(param->getDefaultValue()));
    k.slider.setMouseDragSensitivity(220);
    k.slider.textFromValueFunction = std::move(textFn);
    k.slider.valueFromTextFunction = [](const juce::String& t)
    {
        return t.retainCharacters("0123456789-+.").getDoubleValue();
    };
    k.slider.onValueChange = [this] { repaint(); };
    k.slider.setLookAndFeel(&controlLnF);

    k.label.setText(name, juce::dontSendNotification);
    k.label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    k.label.setJustificationType(juce::Justification::centred);
    k.label.setInterceptsMouseClicks(false, false);
    k.label.setColour(juce::Label::textColourId, textStrong.withAlpha(0.88f));

    k.attachment = std::make_unique<juce::SliderParameterAttachment>(*param, k.slider);
    addAndMakeVisible(k.slider);
    addAndMakeVisible(k.label);
}

inline void OverdriveEditor::paint(juce::Graphics& g)
{
    const bool bypassed = isBypassed();
    paintShell(g, getLocalBounds().toFloat(), bypassed);
    paintHeader(g, bypassed);
    paintInfoPanel(g, bypassed);

    paintKnobCard(g, heroBounds, driveKnob, describeDrive(driveKnob.slider.getValue()), bypassed, true);
    paintKnobCard(g, toneBounds, toneKnob, describeTone(toneKnob.slider.getValue()), bypassed, false);
    paintKnobCard(g, textureBounds, textureKnob, describeTexture(textureKnob.slider.getValue()), bypassed, false);
    paintKnobCard(g, mixBounds, mixKnob, describeMix(mixKnob.slider.getValue()), bypassed, false);
    paintKnobCard(g, levelBounds, levelKnob, describeLevel(levelKnob.slider.getValue()), bypassed, false);
}

inline void OverdriveEditor::resized()
{
    auto bounds = getLocalBounds().reduced(22);

    headerBounds = bounds.removeFromTop(84);
    bounds.removeFromTop(18);

    infoBounds = bounds.removeFromBottom(170);
    bounds.removeFromBottom(18);

    heroBounds = bounds.removeFromLeft(360);
    bounds.removeFromLeft(18);

    auto topRow = bounds.removeFromTop((bounds.getHeight() - 18) / 2);
    bounds.removeFromTop(18);
    auto bottomRow = bounds;

    toneBounds = topRow.removeFromLeft((topRow.getWidth() - 18) / 2);
    topRow.removeFromLeft(18);
    textureBounds = topRow;

    mixBounds = bottomRow.removeFromLeft((bottomRow.getWidth() - 18) / 2);
    bottomRow.removeFromLeft(18);
    levelBounds = bottomRow;

    layoutHero(driveKnob, heroBounds);
    layoutControl(toneKnob, toneBounds);
    layoutControl(textureKnob, textureBounds);
    layoutControl(mixKnob, mixBounds);
    layoutControl(levelKnob, levelBounds);
}

inline void OverdriveEditor::layoutHero(Knob& k, juce::Rectangle<int> bounds)
{
    auto content = bounds.reduced(24);
    k.label.setBounds(content.removeFromTop(30));
    content.removeFromTop(12);

    auto sliderZone = content.withTrimmedBottom(118);
    auto knobArea = juce::Rectangle<int>(0, 0,
        juce::jmin(286, sliderZone.getWidth()),
        juce::jmin(308, sliderZone.getHeight()));
    knobArea.setCentre(sliderZone.getCentreX(), sliderZone.getCentreY());
    k.slider.setBounds(knobArea);
}

inline void OverdriveEditor::layoutControl(Knob& k, juce::Rectangle<int> bounds)
{
    auto content = bounds.reduced(18);
    k.label.setBounds(content.removeFromTop(24));
    content.removeFromTop(10);

    auto sliderZone = content.withTrimmedBottom(86);
    auto knobArea = juce::Rectangle<int>(0, 0,
        juce::jmin(200, sliderZone.getWidth()),
        juce::jmin(214, sliderZone.getHeight()));
    knobArea.setCentre(sliderZone.getCentreX(), sliderZone.getCentreY());
    k.slider.setBounds(knobArea);
}

inline void OverdriveEditor::paintShell(juce::Graphics& g, const juce::Rectangle<float>& bounds, bool bypassed) const
{
    // Clip to rounded rect for texture rendering
    juce::Path clipPath;
    clipPath.addRoundedRectangle(bounds, 20.0f);
    g.saveState();
    g.reduceClipRegion(clipPath);

    // Base dark gradient
    juce::ColourGradient shell(bypassed ? juce::Colour::fromString("ff0A0806") : shellTop,
        bounds.getCentreX(), bounds.getY(),
        bypassed ? juce::Colour::fromString("ff060504") : shellBottom,
        bounds.getCentreX(), bounds.getBottom(), false);
    if (!bypassed)
        shell.addColour(0.3, juce::Colour::fromString("ff322518"));
    g.setGradientFill(shell);
    g.fillRect(bounds);

    // Real rusted orange metal texture overlay
    if (!bypassed)
    {
        const auto& tex = Nova::Textures::rustedOrangeMetal();
        if (tex.isValid())
        {
            g.setOpacity(0.18f);
            g.drawImage(tex, bounds, juce::RectanglePlacement::stretchToFit);
            g.setOpacity(1.0f);
        }

        // Subtle orange color wash over texture
        g.setColour(accent.withAlpha(0.06f));
        g.fillRect(bounds);
    }

    g.restoreState();

    // Outer shadow ring
    g.setColour(juce::Colours::black.withAlpha(0.50f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 20.0f, 1.0f);

    // Orange halo at top
    auto haloBounds = bounds.reduced(18.0f, 0.0f).removeFromTop(140.0f);
    juce::ColourGradient halo(juce::Colours::transparentBlack, haloBounds.getX(), haloBounds.getY(),
        accent.withAlpha(bypassed ? 0.0f : 0.22f), haloBounds.getCentreX(), haloBounds.getBottom(), false);
    halo.addColour(1.0, juce::Colours::transparentBlack);
    g.setGradientFill(halo);
    g.fillRoundedRectangle(haloBounds, 18.0f);

    // Bottom vignette for depth
    if (!bypassed)
    {
        auto vigBounds = juce::Rectangle<float>(bounds.getX(), bounds.getBottom() - bounds.getHeight() * 0.35f,
            bounds.getWidth(), bounds.getHeight() * 0.35f);
        juce::ColourGradient vig(juce::Colours::transparentBlack, vigBounds.getCentreX(), vigBounds.getY(),
            juce::Colours::black.withAlpha(0.30f), vigBounds.getCentreX(), vigBounds.getBottom(), false);
        g.setGradientFill(vig);
        g.fillRoundedRectangle(vigBounds, 20.0f);
    }

    // Accent border
    g.setColour(bypassed ? borderCol : accent.withAlpha(0.38f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 20.0f, 1.2f);

    // Inner bevel highlight (top edge)
    if (!bypassed)
    {
        auto topEdge = getLocalBounds().toFloat().reduced(6.0f, 0.0f).removeFromTop(1.5f).translated(0.0f, 2.0f);
        juce::ColourGradient edgeGrad(juce::Colours::transparentBlack, topEdge.getX(), topEdge.getCentreY(),
            accentBright.withAlpha(0.14f), topEdge.getCentreX(), topEdge.getCentreY(), false);
        edgeGrad.addColour(1.0, juce::Colours::transparentBlack);
        g.setGradientFill(edgeGrad);
        g.fillRect(topEdge);
    }

    // ---- Decorative screws at corners ----
    {
        const auto& screw = Nova::Textures::screwPhillips();
        const float screwSize = 22.0f;
        const float inset = 16.0f;
        const auto b = getLocalBounds().toFloat();
        Nova::Textures::drawScrew(g, screw, b.getX() + inset, b.getY() + inset, screwSize);
        Nova::Textures::drawScrew(g, screw, b.getRight() - inset, b.getY() + inset, screwSize);
        Nova::Textures::drawScrew(g, screw, b.getX() + inset, b.getBottom() - inset, screwSize);
        Nova::Textures::drawScrew(g, screw, b.getRight() - inset, b.getBottom() - inset, screwSize);
    }
}

inline void OverdriveEditor::paintPanel(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerSize,
    juce::Colour tint, bool bypassed, float glowAlpha) const
{
    bounds = bounds.reduced(0.5f);

    // Drop shadow
    g.setColour(juce::Colours::black.withAlpha(0.28f));
    g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f), cornerSize);

    // Panel body - warm metal tinted
    juce::ColourGradient panel(panelTop.interpolatedWith(tint, bypassed ? 0.02f : 0.12f),
        bounds.getCentreX(), bounds.getY(),
        panelBottom, bounds.getCentreX(), bounds.getBottom(), false);
    g.setGradientFill(panel);
    g.fillRoundedRectangle(bounds, cornerSize);

    // Carbon fiber texture overlay on panels
    if (!bypassed)
    {
        const auto& cfTex = Nova::Textures::carbonFiber();
        if (cfTex.isValid())
        {
            juce::Path panelPath;
            panelPath.addRoundedRectangle(bounds, cornerSize);
            g.saveState();
            g.reduceClipRegion(panelPath);
            g.setTiledImageFill(cfTex, (int)bounds.getX(), (int)bounds.getY(), 0.07f);
            g.fillRect(bounds);
            g.setOpacity(1.0f);
            g.restoreState();
        }
    }

    // Orange glow strip at top
    auto topStrip = bounds.reduced(14.0f, 0.0f).removeFromTop(2.5f);
    juce::ColourGradient topGlow(juce::Colours::transparentBlack, topStrip.getX(), topStrip.getCentreY(),
        tint.withAlpha(bypassed ? 0.0f : glowAlpha), topStrip.getCentreX(), topStrip.getCentreY(), false);
    topGlow.addColour(1.0, juce::Colours::transparentBlack);
    g.setGradientFill(topGlow);
    g.fillRoundedRectangle(topStrip, 2.0f);

    // Border
    g.setColour(borderCol.withAlpha(0.85f));
    g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
    g.setColour(tint.withAlpha(bypassed ? 0.12f : 0.25f));
    g.drawRoundedRectangle(bounds.reduced(1.0f), cornerSize - 1.0f, 1.0f);
}

inline void OverdriveEditor::paintHeader(juce::Graphics& g, bool bypassed) const
{
    paintPanel(g, headerBounds.toFloat(), 16.0f, accent, bypassed, 0.42f);

    auto area = headerBounds.reduced(22, 14);
    auto left = area.removeFromLeft(620);

    g.setColour(textDim.withAlpha(0.92f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText("PEDAL EDITOR", left.removeFromTop(12), juce::Justification::centredLeft);

    g.setColour(textStrong.withAlpha(bypassed ? 0.58f : 0.96f));
    g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
    g.drawText("AURORA OVERDRIVE", left.removeFromTop(26), juce::Justification::centredLeft);

    g.setColour(textMuted.withAlpha(0.92f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText("Premium gain staging with focused bite and parallel polish.",
        left, juce::Justification::centredLeft);

    auto right = area;
    auto serial = right.removeFromLeft(170);
    g.setColour(textDim.withAlpha(0.88f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawText("CIRCUIT  OD-A1", serial.removeFromTop(12), juce::Justification::centredLeft);
    g.setColour(textMuted.withAlpha(0.90f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText("Discrete clip stage\nParallel mix bus", serial, juce::Justification::centredLeft);

    right.removeFromLeft(12);
    auto status = right.removeFromRight(150).toFloat();
    g.setColour(bypassed ? juce::Colour::fromString("ff3F1D1D") : juce::Colour::fromString("ff112118"));
    g.fillRoundedRectangle(status, 11.0f);
    g.setColour(bypassed ? Nova::Colors::Error.withAlpha(0.58f) : accent.withAlpha(0.50f));
    g.drawRoundedRectangle(status, 11.0f, 1.0f);

    // LED indicator from real image (top half = red/off, bottom half = green/on)
    {
        const auto& ledImg = Nova::Textures::ledRedGreen();
        if (ledImg.isValid())
        {
            const int ledFrameH = ledImg.getHeight() / 2;
            const int srcY = bypassed ? 0 : ledFrameH;  // top = red, bottom = green
            auto ledDest = status.removeFromLeft(36.0f).reduced(6.0f, 10.0f);
            g.drawImage(ledImg,
                (int)ledDest.getX(), (int)ledDest.getY(), (int)ledDest.getWidth(), (int)ledDest.getHeight(),
                0, srcY, ledImg.getWidth(), ledFrameH);
        }
        else
        {
            auto lamp = status.removeFromLeft(14.0f).reduced(0.0f, 17.0f);
            g.setColour(bypassed ? Nova::Colors::Error.withAlpha(0.80f) : accentBright.withAlpha(0.95f));
            g.fillRoundedRectangle(lamp, 2.0f);
        }
    }

    g.setColour(textStrong.withAlpha(0.88f));
    g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    g.drawText(bypassed ? "BYPASSED" : "ACTIVE", status.toNearestInt().reduced(4, 0),
        juce::Justification::centred);
}

inline void OverdriveEditor::paintInfoPanel(juce::Graphics& g, bool bypassed) const
{
    paintPanel(g, infoBounds.toFloat(), 18.0f, accentBright, bypassed, 0.28f);

    auto content = infoBounds.reduced(20, 18);
    auto textArea = content.removeFromLeft(308);
    content.removeFromLeft(18);
    auto graphArea = content.removeFromRight(270).toFloat();
    content.removeFromRight(18);
    auto chipsArea = content;

    g.setColour(textDim.withAlpha(0.92f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawText("VOICE", textArea.removeFromTop(12), juce::Justification::centredLeft);

    g.setColour(textStrong.withAlpha(bypassed ? 0.52f : 0.92f));
    g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    g.drawText("Warm clipping, controlled highs, mix-ready level.",
        textArea.removeFromTop(24), juce::Justification::centredLeft);

    g.setColour(textMuted.withAlpha(0.88f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawFittedText("Use Drive for push, Texture for grain and Mix for parallel definition.",
        textArea.removeFromTop(28), juce::Justification::topLeft, 2);

    textArea.removeFromTop(10);
    g.setColour(textDim.withAlpha(0.90f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawText("SUGGESTED WORKFLOW", textArea.removeFromTop(12), juce::Justification::centredLeft);
    g.setColour(textMuted.withAlpha(0.92f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawFittedText("Set Drive first, shape with Tone, add grain with Texture, then trim Mix and Level.",
        textArea, juce::Justification::topLeft, 3);

    auto topChipRow = chipsArea.removeFromTop((chipsArea.getHeight() - 10) / 2).toFloat();
    chipsArea.removeFromTop(10);
    auto bottomChipRow = chipsArea.toFloat();
    const float chipGap = 10.0f;
    const float topChipWidth = (topChipRow.getWidth() - chipGap) / 2.0f;
    const float bottomChipWidth = (bottomChipRow.getWidth() - chipGap) / 2.0f;

    paintChip(g, topChipRow.removeFromLeft(topChipWidth), "Clip", describeDrive(driveKnob.slider.getValue()), bypassed);
    topChipRow.removeFromLeft(chipGap);
    paintChip(g, topChipRow, "Tone", describeTone(toneKnob.slider.getValue()), bypassed);
    paintChip(g, bottomChipRow.removeFromLeft(bottomChipWidth), "Texture", describeTexture(textureKnob.slider.getValue()), bypassed);
    bottomChipRow.removeFromLeft(chipGap);
    paintChip(g, bottomChipRow, "Level", describeLevel(levelKnob.slider.getValue()), bypassed);

    g.setColour(juce::Colour::fromString("ff101722"));
    g.fillRoundedRectangle(graphArea, 14.0f);
    g.setColour(borderCol.withAlpha(0.85f));
    g.drawRoundedRectangle(graphArea, 14.0f, 1.0f);

    auto graphInner = graphArea.reduced(14.0f, 12.0f);
    for (int i = 0; i < 4; ++i)
    {
        const float y = juce::jmap((float)i / 3.0f, graphInner.getBottom(), graphInner.getY());
        g.setColour(juce::Colour::fromString("ff334155").withAlpha(0.30f));
        g.drawHorizontalLine((int)std::round(y), graphInner.getX(), graphInner.getRight());
    }

    const auto curve = makeResponseCurve(graphInner);
    g.setColour(juce::Colour::fromString("ffFBBF24").withAlpha(bypassed ? 0.28f : 0.92f));
    g.strokePath(curve, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour(accent.withAlpha(bypassed ? 0.12f : 0.12f));
    g.strokePath(curve, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour(textDim.withAlpha(0.88f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawText("RESPONSE MAP", graphArea.toNearestInt().removeFromTop(12).reduced(2, 0), juce::Justification::centredLeft);
}

inline void OverdriveEditor::paintKnobCard(juce::Graphics& g, const juce::Rectangle<int>& bounds,
    const Knob& k, const juce::String& descriptor, bool bypassed, bool hero) const
{
    paintPanel(g, bounds.toFloat(), hero ? 22.0f : 16.0f, hero ? accent : accentBright, bypassed, hero ? 0.34f : 0.22f);

    // Small screws on card corners
    {
        const auto& screw = Nova::Textures::screwTorx();
        const float sz = hero ? 14.0f : 10.0f;
        const float in = hero ? 14.0f : 10.0f;
        const auto b = bounds.toFloat();
        Nova::Textures::drawScrew(g, screw, b.getX() + in, b.getY() + in, sz);
        Nova::Textures::drawScrew(g, screw, b.getRight() - in, b.getY() + in, sz);
    }

    auto footer = bounds.reduced(hero ? 24 : 18).removeFromBottom(hero ? 112 : 82);
    if (hero)
    {
        g.setColour(textDim.withAlpha(0.92f));
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(k.hint.toUpperCase(), footer.removeFromTop(12), juce::Justification::centred);

        g.setColour(textStrong.withAlpha(bypassed ? 0.54f : 0.94f));
        g.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
        g.drawText(descriptor.toUpperCase(), footer.removeFromTop(22), juce::Justification::centred);

        g.setColour(textMuted.withAlpha(0.88f));
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Stage behavior", footer.removeFromTop(16), juce::Justification::centred);

        footer.removeFromTop(6);
        const float gap = 8.0f;
        const float rowH = (footer.getHeight() - gap * 3.0f) / 4.0f;
        paintAnalysisBar(g, footer.removeFromTop((int)std::round(rowH)).toFloat(),
            "Saturation", juce::jmap((float)driveKnob.slider.getValue(), 0.0f, 100.0f, 0.08f, 1.0f), accent, bypassed);
        footer.removeFromTop((int)std::round(gap));
        paintAnalysisBar(g, footer.removeFromTop((int)std::round(rowH)).toFloat(),
            "Attack", juce::jlimit(0.0f, 1.0f, 0.30f + normalizedValueFor(toneKnob) * 0.70f), accentBright, bypassed);
        footer.removeFromTop((int)std::round(gap));
        paintAnalysisBar(g, footer.removeFromTop((int)std::round(rowH)).toFloat(),
            "Blend", normalizedValueFor(mixKnob), juce::Colour::fromString("ffFBBF24"), bypassed);
        footer.removeFromTop((int)std::round(gap));
        paintAnalysisBar(g, footer.toFloat(),
            "Output", normalizedValueFor(levelKnob), juce::Colour::fromString("ff94A3B8"), bypassed);
    }
    else
    {
        auto header = bounds.reduced(18).removeFromTop(18);
        g.setColour(textDim.withAlpha(0.90f));
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawText("CONTROL", header.removeFromLeft(72), juce::Justification::centredLeft);

        g.setColour(textDim.withAlpha(0.90f));
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawText(k.hint.toUpperCase(), footer.removeFromTop(12), juce::Justification::centred);

        g.setColour(textStrong.withAlpha(bypassed ? 0.52f : 0.88f));
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawFittedText(descriptor.toUpperCase(), footer.removeFromTop(16), juce::Justification::centred, 2);
        footer.removeFromTop(6);
        paintValueRail(g, footer.toFloat(), k, bypassed);
    }
}

inline void OverdriveEditor::paintChip(juce::Graphics& g, juce::Rectangle<float> bounds,
    const juce::String& caption, const juce::String& value, bool bypassed) const
{
    g.setColour(juce::Colour::fromString("ff101722"));
    g.fillRoundedRectangle(bounds, 9.0f);
    g.setColour((bypassed ? borderCol : accent.withAlpha(0.22f)));
    g.drawRoundedRectangle(bounds, 9.0f, 1.0f);

    auto content = bounds.reduced(10.0f, 5.0f);
    g.setColour(textDim.withAlpha(0.92f));
    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    g.drawText(caption.toUpperCase(), content.removeFromTop(8).toNearestInt(), juce::Justification::centredLeft);

    g.setColour(textStrong.withAlpha(bypassed ? 0.50f : 0.90f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawFittedText(value.toUpperCase(), content.toNearestInt(), juce::Justification::centredLeft, 1);
}

inline void OverdriveEditor::paintValueRail(juce::Graphics& g, juce::Rectangle<float> bounds,
    const Knob& k, bool bypassed) const
{
    auto legend = rangeLegendFor(k);
    auto title = bounds.removeFromTop(12.0f);
    g.setColour(textMuted.withAlpha(0.88f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawText(legend.focusLabel.toUpperCase(), title.toNearestInt(), juce::Justification::centred);

    bounds.removeFromTop(8.0f);
    auto rail = bounds.removeFromTop(8.0f).reduced(6.0f, 0.0f);

    g.setColour(juce::Colour::fromString("ff243042"));
    g.fillRoundedRectangle(rail, 4.0f);

    const float defaultNorm = defaultNormalizedValueFor(k);
    auto sweetSpot = rail;
    sweetSpot.setX(rail.getX() + rail.getWidth() * juce::jlimit(0.0f, 1.0f, defaultNorm - 0.08f));
    sweetSpot.setWidth(rail.getWidth() * 0.16f);
    g.setColour(accent.withAlpha(bypassed ? 0.12f : 0.22f));
    g.fillRoundedRectangle(sweetSpot, 4.0f);

    auto fill = rail;
    fill.setWidth(juce::jmax(6.0f, rail.getWidth() * normalizedValueFor(k)));
    g.setColour(accentBright.withAlpha(bypassed ? 0.28f : 0.92f));
    g.fillRoundedRectangle(fill, 4.0f);

    const float knobX = rail.getX() + rail.getWidth() * normalizedValueFor(k);
    g.setColour(juce::Colours::white.withAlpha(bypassed ? 0.35f : 0.95f));
    g.fillEllipse(knobX - 5.0f, rail.getCentreY() - 5.0f, 10.0f, 10.0f);
    g.setColour(shellBottom.withAlpha(0.95f));
    g.drawEllipse(knobX - 5.0f, rail.getCentreY() - 5.0f, 10.0f, 10.0f, 1.0f);

    bounds.removeFromTop(8.0f);
    auto labels = bounds.removeFromTop(12.0f);
    g.setColour(textDim.withAlpha(0.90f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawText(legend.minLabel.toUpperCase(), labels.removeFromLeft((int)(labels.getWidth() * 0.33f)).toNearestInt(),
        juce::Justification::centredLeft);
    g.drawText(legend.maxLabel.toUpperCase(), labels.removeFromRight((int)(labels.getWidth() * 0.5f)).toNearestInt(),
        juce::Justification::centredRight);
}

inline void OverdriveEditor::paintAnalysisBar(juce::Graphics& g, juce::Rectangle<float> bounds,
    const juce::String& name, float amount, juce::Colour colour, bool bypassed) const
{
    amount = juce::jlimit(0.0f, 1.0f, amount);

    auto label = bounds.removeFromLeft(88.0f);
    g.setColour(textDim.withAlpha(0.90f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawText(name.toUpperCase(), label.toNearestInt(), juce::Justification::centredLeft);

    auto rail = bounds.reduced(6.0f, 5.0f);
    g.setColour(juce::Colour::fromString("ff243042"));
    g.fillRoundedRectangle(rail, 5.0f);

    auto fill = rail;
    fill.setWidth(juce::jmax(8.0f, rail.getWidth() * amount));
    g.setColour(colour.withAlpha(bypassed ? 0.24f : 0.88f));
    g.fillRoundedRectangle(fill, 5.0f);

    g.setColour(colour.withAlpha(bypassed ? 0.10f : 0.18f));
    g.fillRoundedRectangle(fill.expanded(3.0f, 2.0f), 6.0f);
}

inline OverdriveEditor::RangeLegend OverdriveEditor::rangeLegendFor(const Knob& k) const
{
    const auto name = k.label.getText();
    if (name == "TONE")
        return { "Dark", "Bright", "EQ balance" };
    if (name == "TEXTURE")
        return { "Smooth", "Raw", "Clipping grain" };
    if (name == "MIX")
        return { "Dry", "Wet", "Parallel blend" };
    if (name == "LEVEL")
        return { "Trim", "Boost", "Output trim" };

    return { "Low", "High", "Control range" };
}

inline float OverdriveEditor::normalizedValueFor(const Knob& k) const
{
    const auto range = k.slider.getRange();
    const auto span = range.getLength();
    if (span <= 0.0)
        return 0.0f;

    return juce::jlimit(0.0f, 1.0f, (float)((k.slider.getValue() - range.getStart()) / span));
}

inline float OverdriveEditor::defaultNormalizedValueFor(const Knob& k) const
{
    const auto range = k.slider.getRange();
    const auto span = range.getLength();
    if (span <= 0.0)
        return 0.0f;

    return juce::jlimit(0.0f, 1.0f, (float)((k.slider.getDoubleClickReturnValue() - range.getStart()) / span));
}

inline juce::String OverdriveEditor::describeDrive(double value) const
{
    if (value < 28.0)
        return "Edge push";
    if (value < 58.0)
        return "Focused crunch";
    if (value < 82.0)
        return "Saturated lead";
    return "Full molten";
}

inline juce::String OverdriveEditor::describeTone(double value) const
{
    if (value < 0.30)
        return "Dark";
    if (value < 0.60)
        return "Balanced";
    if (value < 0.82)
        return "Bright";
    return "Cutting";
}

inline juce::String OverdriveEditor::describeTexture(double value) const
{
    if (value < 0.30)
        return "Smooth";
    if (value < 0.60)
        return "Tight";
    if (value < 0.82)
        return "Grainy";
    return "Raw";
}

inline juce::String OverdriveEditor::describeMix(double value) const
{
    if (value < 0.24)
        return "Dry";
    if (value < 0.55)
        return "Parallel";
    if (value < 0.86)
        return "Forward";
    return "Full wet";
}

inline juce::String OverdriveEditor::describeLevel(double value) const
{
    if (value < 0.28)
        return "Trimmed";
    if (value < 0.62)
        return "Unity";
    if (value < 0.84)
        return "Boosted";
    return "Hot output";
}

inline juce::Path OverdriveEditor::makeResponseCurve(juce::Rectangle<float> bounds) const
{
    const float drive = (float)juce::jlimit(0.0, 100.0, driveKnob.slider.getValue()) / 100.0f;
    const float tone = (float)juce::jlimit(0.0, 1.0, toneKnob.slider.getValue());
    const float texture = (float)juce::jlimit(0.0, 1.0, textureKnob.slider.getValue());
    const float mix = (float)juce::jlimit(0.0, 1.0, mixKnob.slider.getValue());

    juce::Path curve;
    for (int i = 0; i <= 42; ++i)
    {
        const float t = (float)i / 42.0f;
        const float x = bounds.getX() + t * bounds.getWidth();

        const float tilt = juce::jmap(tone, -0.22f, 0.22f) * (t - 0.5f);
        const float bump = std::sin(t * juce::MathConstants<float>::pi) * juce::jmap(texture, 0.06f, 0.24f);
        const float driveLift = juce::jmap(drive, 0.10f, 0.26f);
        const float blendFloor = juce::jmap(mix, -0.04f, 0.05f);
        const float normalized = juce::jlimit(0.08f, 0.92f, 0.62f - driveLift + blendFloor - bump + tilt);
        const float y = bounds.getY() + normalized * bounds.getHeight();

        if (i == 0)
            curve.startNewSubPath(x, y);
        else
            curve.lineTo(x, y);
    }

    return curve;
}

inline void OverdriveEditor::timerCallback()
{
    const bool bypassed = isBypassed();
    if (bypassed != lastBypassState)
    {
        lastBypassState = bypassed;
        repaint();
    }
}

inline bool OverdriveEditor::isBypassed() const
{
    if (auto* base = dynamic_cast<const ProcessorBase*>(&od))
        return base->getBypassed();
    return false;
}

inline juce::AudioProcessorEditor* OverdrivePedal::createEditor()
{
    return new OverdriveEditor(*this);
}
