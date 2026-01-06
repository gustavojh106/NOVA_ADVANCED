#pragma once
#include <JuceHeader.h>

class ProcessorBase : public juce::AudioProcessor
{
public:
    ProcessorBase()
        : AudioProcessor(BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo())
            .withOutput("Output", juce::AudioChannelSet::stereo()))
    {
    }
    void setBypassed(bool shouldBypass)
    {
        if (isBypassed != shouldBypass)
        {
            isBypassed = shouldBypass;
            // Opcional: Resetear buffers internos al reactivar para evitar "colas" de delay viejas
            if (!shouldBypass) reset();
        }
    }

    bool getBypassed() const { return isBypassed; }
    // ... (Layout support se mantiene igual) ...
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet()) return false;
        if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::mono()
            && layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo()) return false;
        return true;
    }

    // ==============================================================================
    //  SOTA AUTOMATION: TOTAL RECALL GENÉRICO
    // ==============================================================================

    // 1. Guardar Estado (Save)
    // Recorre todos los parámetros registrados y los guarda en un binario.
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        juce::XmlElement xml("PLUGIN_STATE");

        // Magia: Iteramos sobre los parámetros que tú añadiste con addParameter()
        for (auto* param : getParameters())
        {
            if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
            {
                auto* paramElem = xml.createNewChildElement("PARAM");
                paramElem->setAttribute("id", p->paramID);
                paramElem->setAttribute("value", p->getValue()); // Guarda valor normalizado (0.0 - 1.0)
            }
        }

        copyXmlToBinary(xml, destData);
    }

    // 2. Cargar Estado (Load)
    // Lee el binario y restaura las perillas
    void setStateInformation(const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

        if (xmlState != nullptr && xmlState->hasTagName("PLUGIN_STATE"))
        {
            for (auto* child : xmlState->getChildIterator())
            {
                if (child->hasTagName("PARAM"))
                {
                    juce::String paramID = child->getStringAttribute("id");
                    float value = (float)child->getDoubleAttribute("value");

                    // Buscar el parámetro por ID y actualizarlo
                    for (auto* param : getParameters())
                    {
                        if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
                        {
                            if (p->paramID == paramID)
                            {
                                // IMPORTANTE: sendNotificationSync avisa al DSP y a la UI del cambio
                                p->setValueNotifyingHost(value);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // ... (Resto de boilerplate igual: createEditor, getName, etc.) ...
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "Base"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
protected:
    // --- NUEVO: Helper para procesar o saltar ---
    // Retorna 'true' si debemos procesar el efecto.
    // Retorna 'false' si estamos en bypass (y el buffer pasa intacto).
    bool shouldProcess(juce::AudioBuffer<float>& buffer)
    {
        if (isBypassed)
        {
            // En VST/JUCE, "buffer" es Input y Output a la vez.
            // Si no hacemos nada y retornamos, el audio entra y sale igual (True Bypass).
            return false;
        }
        return true;
    }
private:
    std::atomic<bool> isBypassed{ false }; // Atómico para seguridad entre hilos
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProcessorBase)
};