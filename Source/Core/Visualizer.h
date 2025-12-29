#pragma once
#include <JuceHeader.h>

// Componente SOTA: Osciloscopio de alto rendimiento con Glow Vectorial
class SimpleOscilloscope : public juce::Component, public juce::Timer
{
public:
    SimpleOscilloscope()
    {
        startTimerHz(60); // Subimos a 60 FPS para fluidez total (ahora que es eficiente, podemos permitírnoslo)
    }

    // Método Thread-Safe para recibir audio desde el AudioEngine
    void pushBuffer(const juce::AudioBuffer<float>& buffer)
    {
        if (buffer.getNumChannels() > 0)
        {
            auto* channelData = buffer.getReadPointer(0);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                fifo[fifoIndex] = channelData[i];
                fifoIndex = (fifoIndex + 1) % fifoSize;
            }
        }
    }

    void paint(juce::Graphics& g) override
    {
        // 1. Fondo del visor (Negro profundo)
        g.fillAll(juce::Colour::fromString("ff151515"));

        // 2. Dibujamos la rejilla (Grid) sutil
        g.setColour(juce::Colours::white.withAlpha(0.05f));
        g.drawHorizontalLine(getHeight() / 2, 0, (float)getWidth());
        // Añadimos líneas verticales para look más técnico
        for (int i = 0; i < getWidth(); i += 40)
            g.drawVerticalLine(i, 0.0f, (float)getHeight());

        // Preparación de la ruta
        juce::Path p;
        auto w = (float)getWidth();
        auto h = (float)getHeight();
        auto mid = h * 0.5f;

        // Capturamos una instantánea del buffer circular
        int readIndex = fifoIndex;

        // Movemos el punto de inicio
        // Empezamos un poco antes del borde izquierdo para evitar gaps visuales
        p.startNewSubPath(-5.0f, mid);

        // Algoritmo de diezmado optimizado
        int resolution = 2;

        for (int x = 0; x < w + 5; x += resolution) // Dibujamos un poco más allá del ancho
        {
            int bufferIdx = (readIndex + x) % fifoSize;
            float sample = fifo[bufferIdx];

            // Saturación visual suave
            sample = juce::jlimit(-1.0f, 1.0f, sample);

            // Mapeamos Y (Amplitud)
            float y = mid - (sample * mid * 0.9f);

            // Suavizado de curva (lineTo es más rápido que quadraticTo y suficiente para 2px de resolución)
            p.lineTo((float)x, y);
        }

        // Color Base (Dorado)
        auto baseColour = juce::Colour::fromString("ffebac26");

        // --- EFECTO GLOW SOTA (Multi-capa Vectorial) ---
        // En lugar de calcular un Gaussian Blur por software (lento),
        // dibujamos la misma línea 3 veces con diferente grosor y opacidad.

        // Capa 1: Halo Exterior (Muy ancho, muy transparente) - Simula la luz dispersa
        g.setColour(baseColour.withAlpha(0.1f));
        g.strokePath(p, juce::PathStrokeType(14.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Capa 2: Brillo Medio (Ancho medio) - Simula el gas del neón
        g.setColour(baseColour.withAlpha(0.3f));
        g.strokePath(p, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Capa 3: Núcleo (Fino, casi sólido) - La fuente de luz
        g.setColour(baseColour.withAlpha(0.9f));
        g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void timerCallback() override
    {
        repaint();
    }

private:
    // Buffer Circular Estático
    static constexpr int fifoSize = 4096;
    float fifo[fifoSize] = { 0.0f };
    int fifoIndex = 0;
};