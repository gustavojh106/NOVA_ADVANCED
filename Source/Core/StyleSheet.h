#pragma once
#include <JuceHeader.h>

namespace UI
{
    // Colores SOTA (Paleta "Dark Boutique")
    namespace Colors
    {
        const juce::Colour Background = juce::Colour::fromString("ff1a1a1a"); // Gris muy oscuro
        const juce::Colour Panel = juce::Colour::fromString("ff2b2b2b"); // Gris panel
        const juce::Colour Accent = juce::Colour::fromString("ffebac26"); // Dorado suave (Vintage)
        const juce::Colour Text = juce::Colour::fromString("ffeaeaea"); // Blanco roto
        const juce::Colour Shadow = juce::Colours::black.withAlpha(0.5f);
    }

    class ModernKnobLnF : public juce::LookAndFeel_V4
    {
    public:
        ModernKnobLnF()
        {
            // Configuramos fuentes por defecto
            setColour(juce::Label::textColourId, Colors::Text);
        }

        // SOTA: Dibujado de Potenciro Vectorial Realista
        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
            const float rotaryStartAngle, const float rotaryEndAngle, juce::Slider&) override
        {
            auto radius = (float)juce::jmin(width / 2, height / 2) - 4.0f;
            auto centreX = (float)x + (float)width * 0.5f;
            auto centreY = (float)y + (float)height * 0.5f;
            auto rx = centreX - radius;
            auto ry = centreY - radius;
            auto rw = radius * 2.0f;
            auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

            // 1. Sombra del cuerpo (Depth)
            g.setColour(Colors::Shadow);
            g.fillEllipse(rx + 2, ry + 2, rw, rw);

            // 2. Cuerpo del Knob (Gradiente metlico sutil)
            juce::ColourGradient grad(juce::Colours::darkgrey.darker(0.2f), centreX, ry,
                juce::Colours::black, centreX, ry + rw, false);
            g.setGradientFill(grad);
            g.fillEllipse(rx, ry, rw, rw);

            // 3. Borde metlico
            g.setColour(juce::Colours::grey);
            g.drawEllipse(rx, ry, rw, rw, 1.0f);

            // 4. Indicador de Valor (El arco alrededor)
            juce::Path arcPath;
            arcPath.addCentredArc(centreX, centreY, radius + 5.0f, radius + 5.0f,
                0.0f, rotaryStartAngle, angle, true);

            g.setColour(Colors::Accent);
            g.strokePath(arcPath, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // 5. El "Puntero" (Dot o Lnea en el knob)
            juce::Path pointer;
            float pointerWidth = 4.0f;
            pointer.addRectangle(-pointerWidth * 0.5f, -radius * 0.8f, pointerWidth, radius * 0.3f);

            pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(centreX, centreY));

            g.setColour(Colors::Accent.brighter(0.2f));
            g.fillPath(pointer);
        }

        // Eliminamos el recuadro feo de foco
        // Eliminamos el recuadro feo de foco y corregimos el typo io::Font
        void drawLabel(juce::Graphics& g, juce::Label& label) override
        {
            g.fillAll(label.findColour(juce::Label::backgroundColourId));

            if (!label.isBeingEdited())
            {
                auto alpha = label.isEnabled() ? 1.0f : 0.5f;

                // CORRECCIN AQU: Usamos juce::Font, no io::Font
                const juce::Font font = getLabelFont(label);

                g.setColour(label.findColour(juce::Label::textColourId).withMultipliedAlpha(alpha));
                g.setFont(font);

                auto textArea = getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());

                g.drawFittedText(label.getText(), textArea, label.getJustificationType(),
                    juce::jmax(1, (int)(textArea.getHeight() / font.getHeight())),
                    label.getMinimumHorizontalScale());
            }
        }
    };
}