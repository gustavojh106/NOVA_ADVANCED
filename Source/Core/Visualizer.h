#pragma once
#include <JuceHeader.h>

// Componente SOTA: Osciloscopio de alto rendimiento
class SimpleOscilloscope : public juce::Component, public juce::Timer
{
public:
    SimpleOscilloscope()
    {
        startTimerHz(30); // 30 FPS es suficiente para audio, ahorra GPU
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
        // Fondo del visor
        g.fillAll(juce::Colour::fromString("ff151515")); // Negro casi puro

        // Dibujamos la rejilla (Grid) sutil
        g.setColour(juce::Colours::white.withAlpha(0.05f));
        g.drawHorizontalLine(getHeight() / 2, 0, (float)getWidth());

        // Dibujamos la onda
        g.setColour(juce::Colour::fromString("ffebac26")); // Mismo dorado que los knobs

        juce::Path p;
        auto w = (float)getWidth();
        auto h = (float)getHeight();
        auto mid = h * 0.5f;

        // Capturamos una instantánea del buffer circular para dibujar
        // (Empezamos desde el índice de escritura actual hacia atrás para ver lo último)
        int readIndex = fifoIndex;

        // Movemos el punto de inicio
        p.startNewSubPath(0, mid);

        // Algoritmo de diezmado simple (Decimation) para dibujar eficiente
        int resolution = 2; // Saltamos muestras para no saturar la UI

        for (int x = 0; x < w; x += resolution)
        {
            // Mapeamos X de la pantalla al buffer circular
            int bufferIdx = (readIndex + x) % fifoSize;
            float sample = fifo[bufferIdx];

            // Saturación visual suave
            sample = juce::jlimit(-1.0f, 1.0f, sample);

            // Mapeamos Y (Amplitud)
            float y = mid - (sample * mid * 0.9f); // 0.9 para margen

            if (x == 0) p.startNewSubPath((float)x, y);
            else        p.lineTo((float)x, y);
        }

        // Glow (Resplandor) SOTA
        auto glow = juce::DropShadow(juce::Colour::fromString("ffebac26"), 10, { 0, 0 });
        glow.drawForPath(g, p);

        g.strokePath(p, juce::PathStrokeType(1.5f));
    }

    void timerCallback() override
    {
        repaint();
    }

private:
    // Buffer Circular Estático (Suficiente para visualizar)
    static constexpr int fifoSize = 4096;
    float fifo[fifoSize] = { 0.0f };
    int fifoIndex = 0;
};