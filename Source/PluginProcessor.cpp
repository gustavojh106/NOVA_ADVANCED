/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Pedal_Overdrive.h" // <--- ¡ESTA ES LA LÍNEA QUE FALTA!
#include "Pedal_Cabinet.h"
//==============================================================================
NOVAAudioProcessor::NOVAAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{

    mainGraph = std::make_unique<juce::AudioProcessorGraph>();
}

NOVAAudioProcessor::~NOVAAudioProcessor()
{
}

//==============================================================================
const juce::String NOVAAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NOVAAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool NOVAAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool NOVAAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double NOVAAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int NOVAAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int NOVAAudioProcessor::getCurrentProgram()
{
    return 0;
}

void NOVAAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String NOVAAudioProcessor::getProgramName (int index)
{
    return {};
}

void NOVAAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void NOVAAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Configuración del grafo
    mainGraph->setPlayConfigDetails(getTotalNumInputChannels(), getTotalNumOutputChannels(), sampleRate, samplesPerBlock);
    mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

    // --- ¿ESTÁ ESTA LÍNEA PRESENTE? ---
    initialiseGraph();
}
void NOVAAudioProcessor::initialiseGraph()
{
    mainGraph->clear();

    // 1. Crear Nodos
    auto inputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
    auto outputNode = mainGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    // Tus Pedales
    auto overdriveNode = mainGraph->addNode(std::make_unique<PedalOverdrive>());
    auto cabinetNode = mainGraph->addNode(std::make_unique<PedalCabinet>()); // <--- NUEVO

    // 2. Conectar (Routing)
    if (inputNode && outputNode && overdriveNode && cabinetNode)
    {
        // A. Entrada -> Overdrive (Mono Sum to Stereo)
        for (int channelIn = 0; channelIn < 2; ++channelIn)
        {
            mainGraph->addConnection({ { inputNode->nodeID, channelIn }, { overdriveNode->nodeID, 0 } });
            mainGraph->addConnection({ { inputNode->nodeID, channelIn }, { overdriveNode->nodeID, 1 } });
        }

        // B. Overdrive -> Cabinet (Stereo)
        for (int channel = 0; channel < 2; ++channel)
        {
            mainGraph->addConnection({ { overdriveNode->nodeID, channel }, { cabinetNode->nodeID, channel } });
        }

        // C. Cabinet -> Salida (Stereo)
        for (int channel = 0; channel < 2; ++channel)
        {
            mainGraph->addConnection({ { cabinetNode->nodeID, channel }, { outputNode->nodeID, channel } });
        }
    }
}
void NOVAAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool NOVAAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void NOVAAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // --- PUNTO CRÍTICO ---
    // Si esta línea no está, el motor está apagado.
    mainGraph->processBlock(buffer, midiMessages);
}
//==============================================================================
bool NOVAAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* NOVAAudioProcessor::createEditor()
{
    return new NOVAAudioProcessorEditor (*this);
}

//==============================================================================
void NOVAAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
}

void NOVAAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NOVAAudioProcessor();
}
