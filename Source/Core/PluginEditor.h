#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "PluginProcessor.h"
#include "Constants.h"
#include "StyleSheet.h"

// --- MÓDULOS DE UI ---
#include "../GUI/Overlays/TunerOverlay.h"
#include "../GUI/Widgets/AdviceOverlay.h"
#include "../GUI/Widgets/DraggableButton.h" // Necesitamos la definición completa aquí porque lo usamos como miembro directo

// Forward Declarations (Para punteros inteligentes)
class ChainLane;
class AssetBrowserOverlay;
class PedalSlotComponent;

// ==============================================================================
// CLASE PRINCIPAL DEL EDITOR
// ==============================================================================
class NOVAAudioProcessorEditor : public juce::AudioProcessorEditor,
    public juce::ValueTree::Listener,
    public juce::AsyncUpdater,
    public juce::DragAndDropContainer
{
public:
    NOVAAudioProcessorEditor(NOVAAudioProcessor&);
    ~NOVAAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

    // Métodos públicos para interacción con hijos (DropZones)
    void showOverlay(Nova::ZoneID zone, Nova::ChainID chain);
    void toggleTuner();
    std::vector<juce::Rectangle<int>> getPedalBoundsForZone(Nova::ChainID chain, Nova::ZoneID zone) const;

private:
    // Helpers visuales
    void setupKnob(juce::Slider& slider, const juce::String& name, float min, float max, float def);
    void setupQuickAddButton(DraggableButton& button, const juce::String& typeID, const juce::String& itemType);
    void applyBrowserFilter();
    void drawChannelStrip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);
    void showPedalEditor(Nova::ChainID chain, const juce::String& pedalID);
    juce::Rectangle<int> getInputStripBounds() const;
    juce::Rectangle<int> getOutputStripBounds() const;
    void updateMeterState();
    void requestUiRefresh();
    void handleAsyncUpdate() override;

    // Callbacks de ValueTree (Sincronización de Datos)
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { requestUiRefresh(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { requestUiRefresh(); }

    void updatePedalGui();
    void updateSwitcherState();
    void updateStats();
    void syncControlsFromState();
    void refreshPresetList();
    juce::File getPresetDirectory() const;
    juce::File getPresetFileForName(const juce::String& presetName) const;
    void setCurrentPreset(const juce::String& presetName);
    void saveSelectedOrPromptPreset();
    void savePresetWithName(const juce::String& presetName);
    void loadSelectedPreset();
    void clearPresetAndSession();
    void openSettingsOverlay();
    void launchWizard(int wizardID); // 0=AudioSetup, 1=Start, 2=PresetFinder
    void setLeftPanelOpen(bool shouldOpen);
    void setRightPanelOpen(bool shouldOpen);
    void refreshDrawerButtons();
    void applyEditorPreferences(bool applyStartupPanels);

    NOVAAudioProcessor& audioProcessor;

    // --- 1. HEADER COMPONENTS ---
    juce::TextButton btnStartStop;
    juce::Label lblStats;
    juce::Label lblCpu;
    juce::Label lblProc;
    juce::Label lblBuf;

    juce::TextButton btnTuner{ "T" };
    juce::TextButton btnSettings{ "SETTINGS" };

    // --- SIDEBAR DRAWERS ---
    struct SidebarDrawer final : public juce::Component
    {
        bool isLeftSide = true;
        juce::String title;
        void paint(juce::Graphics& g) override
        {
            g.fillAll(Nova::Colors::Panel);
            g.setColour(Nova::Colors::Border);
            if (isLeftSide)
                g.drawVerticalLine(getWidth() - 1, 0.0f, (float)getHeight());
            else
                g.drawVerticalLine(0, 0.0f, (float)getHeight());

            if (title.isNotEmpty())
            {
                g.setColour(Nova::Colors::Accent);
                g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
                g.drawText(title, 10, 8, getWidth() - 20, 22, juce::Justification::centred);
            }
        }
    };

    SidebarDrawer leftDrawer;
    SidebarDrawer rightDrawer;
    juce::TextButton btnToggleLeft;
    juce::TextButton btnToggleRight;
    bool leftPanelOpen = false;
    bool rightPanelOpen = false;
    void toggleLeftPanel();
    void toggleRightPanel();

    // --- 2. BROWSER (inside left drawer) ---
    juce::TextEditor searchBarBrowser;
    juce::Viewport quickAddViewport;
    juce::Component quickAddContainer;
    std::vector<std::unique_ptr<DraggableButton>> quickAddButtons;
    void layoutQuickAddButtons();

    // --- 3. INPUT STRIP ---
    juce::SharedResourcePointer<UI::ModernKnobLnF> knobLnf;
    juce::SharedResourcePointer<UI::StudioTrimKnobLnF> studioTrimLnf;
    juce::Slider inputVolume;
    juce::Slider inputGate;
    juce::ToggleButton btnMonoStereo{ "Mono/Stereo" };

    // --- 4. MIXER (Centro) ---
    juce::TextButton btnSwitcher;     // kept for legacy — hidden, space key still cycles
    juce::TextButton btnRouteA{ "A" };
    juce::TextButton btnRoutePar{ "||" };
    juce::TextButton btnRouteB{ "B" };
    bool routeInfoHovered = false;
    juce::Rectangle<float> routeInfoIconBounds;
    juce::String routeInfoText { "Shortcut: Space. Routing behavior can be configured in Settings." };

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

    // --- 6. PRESETS & FOOTER ---
    juce::TextEditor searchBarPresets;
    juce::ComboBox presetSelector;
    juce::Label lblCurrentPreset;
    juce::TextButton btnSave{ "SAVE" };
    juce::TextButton btnLoad{ "LOAD" };
    juce::TextButton btnClear{ "CLEAR" };
    std::vector<juce::File> presetFiles;
    juce::String currentPresetName { "No Preset" };

    // --- 7. INFRAESTRUCTURA DE VISUALIZACIÓN ---

    // Carriles de Pedales (Widgets modulares)
    std::unique_ptr<ChainLane> laneA;
    std::unique_ptr<ChainLane> laneB;

    // Mapa de editores de pedales activos
    std::map<juce::AudioProcessorGraph::NodeID, std::unique_ptr<PedalSlotComponent>> activePedalEditors;

    // Overlays (Ventanas modales)
    std::unique_ptr<juce::Component> currentOverlay; // Para el Browser
    std::unique_ptr<TunerOverlay> tunerOverlay;      // Para el Afinador

    std::unique_ptr<juce::SliderParameterAttachment> inputVolumeAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> inputGateAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> monoAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> engineAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> volAAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> panAAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> widthAAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> volBAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> panBAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> widthBAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> outputVolumeAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> outputLimiterAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> outputMixAttachment;

    // Timer para actualizar CPU y FPS
    class StatsTimer : public juce::Timer {
        NOVAAudioProcessorEditor& parent;
    public:
        StatsTimer(NOVAAudioProcessorEditor& p) : parent(p) { startTimerHz(30); }
        void timerCallback() override
        {
            parent.updateMeterState();
            parent.updateStats();
            parent.updateSwitcherState();
        }
    };
    std::unique_ptr<StatsTimer> statsTimer;
    std::atomic<bool> uiRefreshPending{ false };
    float inputMeterDisplay = 0.0f;
    float outputMeterDisplay = 0.0f;
    float inputMeterHold = 0.0f;
    float outputMeterHold = 0.0f;

    int lastKnownAutoHealCount = 0;
    int autoHealFlashFrames    = 0;

    // Plain-language feedback about what the engine is doing. The advisor holds
    // the hysteresis, the overlay just draws what it hands over.
    Nova::Audio::UserAdvisor advisor;
    std::unique_ptr<AdviceOverlay> adviceOverlay;
    int chainFingerprint = 0;
    int chainRevisionCounter = 0;

    void updateAdvice(double cpuPercent, double latencyMs);
    int computeChainFingerprint() const;
    bool showPerformanceStats = true;
    bool openQuickAddOnStartup = false;
    bool openPresetsOnStartup = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NOVAAudioProcessorEditor)
};

