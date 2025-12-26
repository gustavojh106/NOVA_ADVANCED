#include "PluginProcessor.h"
#include "PluginEditor.h"

NOVAAudioProcessor::NOVAAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    ),
    pluginState(IDs::MAIN_STATE)
#endif
{
    pluginState.addListener(this);
}

NOVAAudioProcessor::~NOVAAudioProcessor()
{
    pluginState.removeListener(this);
}

// ==============================================================================
//  AUDIO PROCESSING
// ==============================================================================
void NOVAAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // NO usamos std::max(channels, 2). 
    // Le decimos al motor la verdad cruda del hardware.
    int inCh = getTotalNumInputChannels();
    int outCh = getTotalNumOutputChannels();

    DBG("--- PREPARE TO PLAY ---");
    DBG("Rate: " << sampleRate << " In: " << inCh << " Out: " << outCh);

    // Pasamos los canales reales
    audioEngine.prepare(sampleRate, samplesPerBlock, inCh, outCh);
}

void NOVAAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Limpiar canales extra
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Delegar al motor
    audioEngine.process(buffer, midiMessages);
}

// ==============================================================================
//  GESTIÓN DE ESTADO (ValueTree)
// ==============================================================================
void NOVAAudioProcessor::requestAddPedal(const juce::String& pedalType)
{
    juce::ValueTree newPedal(IDs::PEDAL_TAG);
    newPedal.setProperty(IDs::PEDAL_TYPE, pedalType, nullptr);
    pluginState.appendChild(newPedal, nullptr);
}

void NOVAAudioProcessor::requestRemovePedal(int index)
{
    if (index >= 0 && index < pluginState.getNumChildren())
        pluginState.removeChild(index, nullptr);
}

void NOVAAudioProcessor::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent == pluginState && child.hasType(IDs::PEDAL_TAG))
    {
        juce::String type = child.getProperty(IDs::PEDAL_TYPE);
        audioEngine.addPedal(type);
    }
}

void NOVAAudioProcessor::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int index)
{
    if (parent == pluginState)
    {
        // En lugar de reconstruir todo (lento), le decimos al motor que borre ese específico
        // Esto es una optimización respecto a tu versión anterior.
        audioEngine.removePedal(index);
    }
}

void NOVAAudioProcessor::rebuildChain()
{
    audioEngine.clearChain();
    for (const auto& child : pluginState)
    {
        if (child.hasType(IDs::PEDAL_TAG))
        {
            audioEngine.addPedal(child.getProperty(IDs::PEDAL_TYPE));
        }
    }
}

// ==============================================================================
//  PERSISTENCIA
// ==============================================================================
void NOVAAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream stream(destData, true);
    pluginState.writeToStream(stream);
}

void NOVAAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto tree = juce::ValueTree::readFromData(data, sizeInBytes);
    if (tree.isValid() && tree.hasType(IDs::MAIN_STATE))
    {
        pluginState.removeListener(this);
        pluginState.copyPropertiesAndChildrenFrom(tree, nullptr);
        rebuildChain();
        pluginState.addListener(this);
    }
}

// ==============================================================================
//  BOILERPLATE
// ==============================================================================
const juce::String NOVAAudioProcessor::getName() const { return JucePlugin_Name; }
bool NOVAAudioProcessor::acceptsMidi() const { return false; }
bool NOVAAudioProcessor::producesMidi() const { return false; }
bool NOVAAudioProcessor::isMidiEffect() const { return false; }
double NOVAAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int NOVAAudioProcessor::getNumPrograms() { return 1; }
int NOVAAudioProcessor::getCurrentProgram() { return 0; }
void NOVAAudioProcessor::setCurrentProgram(int index) {}
const juce::String NOVAAudioProcessor::getProgramName(int index) { return {}; }
void NOVAAudioProcessor::changeProgramName(int index, const juce::String& newName) {}
void NOVAAudioProcessor::releaseResources() { audioEngine.reset(); }

bool NOVAAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

bool NOVAAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* NOVAAudioProcessor::createEditor() { return new NOVAAudioProcessorEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NOVAAudioProcessor(); }