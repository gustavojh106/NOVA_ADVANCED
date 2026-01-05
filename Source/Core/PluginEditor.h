#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
//class NOVAAudioProcessorEditor;
// Helper para botones arrastrables
class DraggableButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (auto* container = findParentComponentOfClass<juce::DragAndDropContainer>())
            container->startDragging(getButtonText(), this);
    }
};

class ChainLane; // Forward declaration
class TunerDisplay : public juce::Component, public juce::Timer
{
public:
    TunerDisplay(NOVAAudioProcessor& p) : processor(p) { startTimerHz(60); } // 30 FPS

    void timerCallback() override; // Lo implementaremos en el .cpp
    void paint(juce::Graphics& g) override; // Tu paint modificado
    void mouseDown(const juce::MouseEvent&) override;


    //void timerCallback() override { repaint(); }

    //void paint(juce::Graphics& g) override
    //{
    //    // Fondo Semi-transparente
    //    g.fillAll(juce::Colours::black.withAlpha(0.9f));

    //    // Obtener datos del Engine
    //    auto& engine = processor.getAudioEngine();
    //    int note = engine.getTunerNote();     // Nota MIDI
    //    float cents = engine.getTunerCents(); // -50 a +50

    //    float rms = engine.getTunerRMS();   // Volumen real que llega (0.0 a 1.0)
    //    float pitch = engine.getTunerPitch();
    //    g.setColour(juce::Colours::darkgrey);
    //    g.fillRect(20, 20, 20, getHeight() - 40); // Fondo barra

    //    int barHeight = (int)((getHeight() - 40) * juce::jmin(rms * 5.0f, 1.0f)); // Gain visual x5
    //    g.setColour(rms > 0.001f ? juce::Colours::green : juce::Colours::red);
    //    g.fillRect(20, getHeight() - 20 - barHeight, 20, barHeight); // Nivel

    //    // Texto de depuración
    //    g.setColour(juce::Colours::white);
    //    g.setFont(14.0f);
    //    g.drawText("IN: " + juce::String(rms, 4), 50, 20, 100, 20, juce::Justification::left);

    //    // 3. LOGICA VISUAL NORMAL
    //    if (pitch < 40.0f) {
    //        g.setColour(juce::Colours::grey);
    //        g.setFont(40.0f);
    //        g.drawText("--", getLocalBounds(), juce::Justification::centred);
    //        g.setFont(14.0f);
    //        g.drawText("Toca una cuerda...", getLocalBounds().removeFromBottom(50), juce::Justification::centred);
    //        return;
    //    }
    //    // Si no hay señal clara
    //    if (engine.getTunerPitch() < 40.0f) {
    //        g.setColour(juce::Colours::grey);
    //        g.setFont(40.0f);
    //        g.drawText("--", getLocalBounds(), juce::Justification::centred);
    //        return;
    //    }

    //    // Convertir MIDI a Texto (E, A, D...)
    //    juce::String noteName = juce::MidiMessage::getMidiNoteName(note, true, true, 3);
    //    // Quitamos la octava para que se vea mas limpio (Opcional: "E2" -> "E")
    //    // noteName = noteName.dropLastCharacters(1); 

    //    // Color Semáforo (Verde si está afinado +/- 5 cents)
    //    bool isInTune = std::abs(cents) < 5.0f;
    //    juce::Colour statusColor = isInTune ? juce::Colours::green : juce::Colours::red;

    //    // Dibujar Nota Gigante
    //    g.setColour(statusColor);
    //    g.setFont(80.0f);
    //    g.drawText(noteName, getLocalBounds().removeFromTop(getHeight() / 2), juce::Justification::centredBottom);

    //    // Dibujar Barra de Cents (La aguja)
    //    auto barArea = getLocalBounds().removeFromBottom(100).reduced(50, 40).toFloat();
    //    float centerX = barArea.getCentreX();

    //    // Línea central
    //    g.setColour(juce::Colours::white.withAlpha(0.3f));
    //    g.drawVerticalLine(centerX, barArea.getY(), barArea.getBottom());

    //    // La "Aguja" (Círculo que se mueve)
    //    // Mapear -50..50 cents a la anchura del area
    //    float offset = (cents / 50.0f) * (barArea.getWidth() / 2.0f);
    //    float needleX = centerX + offset;

    //    g.setColour(statusColor);
    //    g.fillEllipse(needleX - 10, barArea.getCentreY() - 10, 20, 20);

    //    // Texto Cents exactos
    //    g.setFont(20.0f);
    //    g.drawText(juce::String(cents, 1) + " ct", barArea.getX(), barArea.getBottom() + 5, barArea.getWidth(), 20, juce::Justification::centred);
    //}
    //void mouseDown(const juce::MouseEvent&) override;
    //void mouseDown(const juce::MouseEvent&) override {
    //    // Clic para cerrar también
    //    if (auto* parent = findParentComponentOfClass<NOVAAudioProcessorEditor>())
    //        parent->toggleTuner(); // Función que crearemos abajo
    //}

private:
    NOVAAudioProcessor& processor;
    float smoothedCents = 0.0f;
    float smoothedRMS = 0.0f;
    int lastNoteIndex = -1;
};
class NOVAAudioProcessorEditor : public juce::AudioProcessorEditor,
    public juce::ValueTree::Listener,
    public juce::DragAndDropContainer
{
public:
    NOVAAudioProcessorEditor(NOVAAudioProcessor&);
    ~NOVAAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    void showOverlay(Nova::ZoneID zone, Nova::ChainID chain);
    void toggleTuner(); 
private:
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { updatePedalGui(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { updatePedalGui(); }

    void updatePedalGui();
    void updateSwitcherState();
    void updateStats(); // Actualizar etiqueta CPU

    // Función helper para dibujar tiras de canal (Input/Output)
    void drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);

    NOVAAudioProcessor& audioProcessor;

    // --- HEADER ---
    juce::TextButton btnStartStop;
    juce::Label lblStats; // Fusión CPU/Latency

    // Placeholders Header
    juce::TextButton btnTuner{ "T" };
    juce::TextButton btnMetronome{ "M" };
    juce::TextButton btnSettings{ "Set" };
    juce::TextButton btnProfile{ "Profile" }; // Reemplaza Cart

    // --- LEFT COLUMN 1: BROWSER ---
    juce::TextEditor searchBarBrowser;
    DraggableButton btnAddOverdrive{ "Overdrive" };
    DraggableButton btnAddCabinet{ "Cabinet" };
    DraggableButton btnAddNeural{ "Neural Amp" };

    // --- LEFT COLUMN 2: INPUT STRIP ---
    // (Eliminado DeviceSelector)
    juce::Slider inputVolume;
    juce::Slider inputGate;      // NUEVO: Noise Gate
    juce::Slider inputTranspose; // Mantenido
    juce::ToggleButton btnMonoStereo{ "Mono/Stereo" };
    juce::Slider inputFader;

    // --- CENTER: MIXER ---
    juce::TextButton btnSwitcher;

    // Line A Controls (EQ cambiado por Pan/Width)
    juce::Slider volSliderA;
    juce::Slider panSliderA;
    juce::Slider widthSliderA;

    // Line B Controls
    juce::Slider volSliderB;
    juce::Slider panSliderB;
    juce::Slider widthSliderB;

    // --- RIGHT COLUMN 1: OUTPUT STRIP ---
    // (Eliminado DeviceSelector y Transpose)
    juce::Slider outputVolume;
    juce::Slider outputGain;
    juce::Slider outputMix;    // NUEVO: Mix Global
    juce::Slider outputFader;

    // --- RIGHT COLUMN 2: PRESETS ---
    juce::TextEditor searchBarPresets;
    juce::TextButton btnSave{ "SAVE" };
    juce::TextButton btnLoad{ "LOAD" };
    // juce::ListBox presetList; (Opcional, placeholder visual dibujado en paint)

    // --- CARRILES ---
    std::unique_ptr<ChainLane> laneA;
    std::unique_ptr<ChainLane> laneB;

    // --- VISUALIZADORES Y MODALES ---
    juce::OwnedArray<juce::AudioProcessorEditor> activePedalEditors;
    std::unique_ptr<juce::Component> currentOverlay;

    // Timer para stats
    class StatsTimer : public juce::Timer {
        NOVAAudioProcessorEditor& parent;
    public:
        StatsTimer(NOVAAudioProcessorEditor& p) : parent(p) { startTimer(500); }
        void timerCallback() override { parent.updateStats(); }
    };
    std::unique_ptr<StatsTimer> statsTimer;
    std::unique_ptr<TunerDisplay> tunerDisplay; // Variable del componente

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};