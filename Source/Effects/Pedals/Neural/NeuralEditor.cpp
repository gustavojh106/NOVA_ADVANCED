//#include "PedalNeuralEditor.h"
//
//PedalNeuralEditor::PedalNeuralEditor(PedalNeural& p)
//    : AudioProcessorEditor(&p), audioProcessor(p)
//{
//    // --- Configurar Drive Knob ---
//    driveKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
//    driveKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 90, 0);
//    driveKnob.setRange(0.0f, 10.0f, 0.1f); // De 0 a 10 de ganancia
//    driveKnob.setValue(4.0f); // Valor inicial (coincide con el default del procesador)
//
//    // Al mover la perilla, actualizamos el procesador en tiempo real
//    driveKnob.onValueChange = [this] { audioProcessor.setDrive((float)driveKnob.getValue()); };
//    addAndMakeVisible(driveKnob);
//
//    driveLabel.setText("DRIVE", juce::dontSendNotification);
//    driveLabel.setJustificationType(juce::Justification::centred);
//    driveLabel.setColour(juce::Label::textColourId, juce::Colours::cyan); // Color Neon
//    addAndMakeVisible(driveLabel);
//
//    // --- Configurar Level Knob ---
//    levelKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
//    levelKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 90, 0);
//    levelKnob.setRange(0.0f, 2.0f, 0.1f); // Volumen de salida
//    levelKnob.setValue(0.5f);
//
//    levelKnob.onValueChange = [this] { audioProcessor.setLevel((float)levelKnob.getValue()); };
//    addAndMakeVisible(levelKnob);
//
//    levelLabel.setText("LEVEL", juce::dontSendNotification);
//    levelLabel.setJustificationType(juce::Justification::centred);
//    levelLabel.setColour(juce::Label::textColourId, juce::Colours::cyan);
//    addAndMakeVisible(levelLabel);
//
//    // Tamael pedal (ancho, alto)
//    setSize(200, 300);
//}
//
//PedalNeuralEditor::~PedalNeuralEditor()
//{
//}
//
//void PedalNeuralEditor::paint(juce::Graphics& g)
//{
//    // Fondo oscuro estilo metal
//    g.fillAll(juce::Colour::fromFloatRGBA(0.1f, 0.1f, 0.12f, 1.0f));
//
//    // Borde Neon
//    g.setColour(juce::Colours::cyan);
//    g.drawRect(getLocalBounds(), 2);
//
//    // T
//tulo del Pedal
//    g.setFont(18.0f);
//    g.drawFittedText("NEURAL AMP", getLocalBounds().removeFromTop(30), juce::Justification::centred, 1);
//}
//
//void PedalNeuralEditor::resized()
//{
//    // Posicionamiento manual de las perillas
//    // (x, y, ancho, alto)
//
//    driveLabel.setBounds(20, 40, 160, 20);
//    driveKnob.setBounds(50, 60, 100, 100);
//
//    levelLabel.setBounds(20, 170, 160, 20);
//    levelKnob.setBounds(50, 190, 100, 100);
//}