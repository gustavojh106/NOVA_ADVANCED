#pragma once
#include <JuceHeader.h>
#include "StyleSheet.h" // Incluimos nuestro estilo

class OverdriveEditor : public juce::AudioProcessorEditor
{
public:
    OverdriveEditor(juce::AudioProcessor& p, juce::AudioParameterFloat* drive, juce::AudioParameterFloat* level)
        : AudioProcessorEditor(&p)
    {
        // 1. Aplicamos el LookAndFeel SOTA
        // (SharedResourcePointer es ideal para compartir una instancia única del estilo entre todos los pedales)
        setLookAndFeel(lnf);

        // Configuración de Drive
        addAndMakeVisible(driveSlider);
        driveSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        driveSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        driveAttachment.reset(new juce::SliderParameterAttachment(*drive, driveSlider));

        // Configuración de Level
        addAndMakeVisible(levelSlider);
        levelSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        levelSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        levelAttachment.reset(new juce::SliderParameterAttachment(*level, levelSlider));

        // Etiquetas
        addAndMakeVisible(driveLabel);
        driveLabel.setText("DRIVE", juce::dontSendNotification);
        driveLabel.setJustificationType(juce::Justification::centred);
        driveLabel.setFont(juce::Font(14.0f, juce::Font::bold));

        addAndMakeVisible(levelLabel);
        levelLabel.setText("LEVEL", juce::dontSendNotification);
        levelLabel.setJustificationType(juce::Justification::centred);
        levelLabel.setFont(juce::Font(14.0f, juce::Font::bold));

        // Título del Pedal
        addAndMakeVisible(titleLabel);
        titleLabel.setText("OD-808", juce::dontSendNotification); // Nombre clásico
        titleLabel.setJustificationType(juce::Justification::centred);
        titleLabel.setFont(juce::Font(22.0f, juce::Font::bold));
        titleLabel.setColour(juce::Label::textColourId, UI::Colors::Accent);

        setSize(180, 280); // Tamaño típico de pedal compacto
    }

    ~OverdriveEditor() override
    {
        setLookAndFeel(nullptr); // Importante limpiar al destruir
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // 1. Fondo del Pedal (Chasis Metálico)
        g.setColour(UI::Colors::Panel);
        g.fillRoundedRectangle(bounds, 10.0f);

        // 2. Borde Sutil (Bevel)
        g.setColour(juce::Colours::white.withAlpha(0.1f));
        g.drawRoundedRectangle(bounds.reduced(1.0f), 10.0f, 2.0f);
        g.setColour(juce::Colours::black.withAlpha(0.3f));
        g.drawRoundedRectangle(bounds.reduced(2.0f), 10.0f, 2.0f);

        // 3. Tornillos (Detalle cosmético SOTA)
        drawScrew(g, 10, 10);
        drawScrew(g, getWidth() - 10, 10);
        drawScrew(g, 10, getHeight() - 10);
        drawScrew(g, getWidth() - 10, getHeight() - 10);

        // 4. Luz LED de encendido (Simulada)
        g.setColour(juce::Colours::red);
        auto ledBounds = juce::Rectangle<float>(getWidth() / 2 - 4, 45, 8, 8);
        g.fillEllipse(ledBounds);
        // Brillo central del LED
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.fillEllipse(ledBounds.reduced(2.0f));
        // Glow
        g.setGradientFill(juce::ColourGradient(juce::Colours::red.withAlpha(0.5f), ledBounds.getCentreX(), ledBounds.getCentreY(),
            juce::Colours::transparentBlack, ledBounds.getCentreX(), ledBounds.getCentreY() + 10, true));
        g.fillEllipse(ledBounds.expanded(5.0f));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(15);

        titleLabel.setBounds(area.removeFromTop(30));
        area.removeFromTop(20); // Espacio para el LED

        auto knobArea = area.removeFromTop(120);
        // Layout de 2 columnas para knobs
        auto leftCol = knobArea.removeFromLeft(knobArea.getWidth() / 2);

        driveLabel.setBounds(leftCol.removeFromBottom(20));
        driveSlider.setBounds(leftCol.reduced(5));

        levelLabel.setBounds(knobArea.removeFromBottom(20));
        levelSlider.setBounds(knobArea.reduced(5));
    }

private:
    void drawScrew(juce::Graphics& g, int x, int y)
    {
        g.setColour(juce::Colours::darkgrey.darker());
        g.fillEllipse(x - 4, y - 4, 8, 8);
        g.setColour(juce::Colours::grey);
        g.drawLine(x - 2, y - 2, x + 2, y + 2, 2.0f);
        g.drawLine(x - 2, y + 2, x + 2, y - 2, 2.0f);
    }

    juce::Slider driveSlider, levelSlider;
    juce::Label driveLabel, levelLabel, titleLabel;

    std::unique_ptr<juce::SliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> levelAttachment;

    // SharedResourcePointer crea una única instancia estática del estilo
    // y la borra cuando el último editor se cierra. Eficiencia SOTA.
    juce::SharedResourcePointer<UI::ModernKnobLnF> lnf;
};