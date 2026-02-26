#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Constants.h"
#include "StyleSheet.h"

// --- MÓDULOS DE UI ---
#include "../GUI/Overlays/TunerOverlay.h"
#include "../GUI/Widgets/DraggableButton.h" // Necesitamos la definición completa aquí porque lo usamos como miembro directo

// Forward Declarations (Para punteros inteligentes)
class ChainLane;
class AssetBrowserOverlay;

// ==============================================================================
// CLASE PRINCIPAL DEL EDITOR
// ==============================================================================
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

    // Métodos públicos para interacción con hijos (DropZones)
    void showOverlay(Nova::ZoneID zone, Nova::ChainID chain);
    void toggleTuner();

private:
    // Helpers visuales
    void setupKnob(juce::Slider& slider, const juce::String& name, float min, float max, float def);
    void drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);

    // Callbacks de ValueTree (Sincronización de Datos)
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { updatePedalGui(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { updatePedalGui(); }

    void updatePedalGui();
    void updateSwitcherState();
    void updateStats();

    NOVAAudioProcessor& audioProcessor;

    // --- 1. HEADER COMPONENTS ---
    juce::TextButton btnStartStop;
    juce::Label lblStats;

    juce::TextButton btnTuner{ "T" };
    juce::TextButton btnMetronome{ "M" };
    juce::TextButton btnSettings{ "Set" };
    juce::TextButton btnProfile{ "Profile" };

    // --- 2. BROWSER (Izquierda) ---
    juce::TextEditor searchBarBrowser;
    // Usamos la clase importada DraggableButton
    DraggableButton btnAddOverdrive{ "Overdrive" };
    DraggableButton btnAddCabinet{ "Cabinet" };
    DraggableButton btnAddNeural{ "Neural Amp" };

    // --- 3. INPUT STRIP ---
    juce::SharedResourcePointer<UI::ModernKnobLnF> knobLnf;
    juce::Slider inputVolume;
    juce::Slider inputGate;
    juce::Slider inputTranspose;
    juce::ToggleButton btnMonoStereo{ "Mono/Stereo" };
    juce::Slider inputFader;

    // --- 4. MIXER (Centro) ---
    juce::TextButton btnSwitcher;

    // Line A
    juce::Slider volSliderA;
    juce::Slider panSliderA;
    juce::Slider widthSliderA;

    // Line B
    juce::Slider volSliderB;
    juce::Slider panSliderB;
    juce::Slider widthSliderB;

    // --- 5. OUTPUT STRIP (Derecha) ---
    juce::Slider outputVolume;
    juce::Slider outputGain;
    juce::Slider outputMix;
    juce::Slider outputFader;

    // --- 6. PRESETS & FOOTER ---
    juce::TextEditor searchBarPresets;
    juce::TextButton btnSave{ "SAVE" };
    juce::TextButton btnLoad{ "LOAD" };

    // --- 7. INFRAESTRUCTURA DE VISUALIZACIÓN ---

    // Carriles de Pedales (Widgets modulares)
    std::unique_ptr<ChainLane> laneA;
    std::unique_ptr<ChainLane> laneB;

    // Mapa de editores de pedales activos
    std::map<juce::AudioProcessorGraph::NodeID, std::unique_ptr<juce::AudioProcessorEditor>> activePedalEditors;

    // Overlays (Ventanas modales)
    std::unique_ptr<juce::Component> currentOverlay; // Para el Browser
    std::unique_ptr<TunerOverlay> tunerOverlay;      // Para el Afinador

    // Timer para actualizar CPU y FPS
    class StatsTimer : public juce::Timer {
        NOVAAudioProcessorEditor& parent;
    public:
        StatsTimer(NOVAAudioProcessorEditor& p) : parent(p) { startTimer(500); }
        void timerCallback() override { parent.updateStats(); }
    };
    std::unique_ptr<StatsTimer> statsTimer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};