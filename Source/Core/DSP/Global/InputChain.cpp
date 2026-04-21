#include "InputChain.h"

#include <cmath>

InputChainProcessor::InputChainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
    gate.setThreshold(-100.0f);
    gate.setRatio(12.0f);
    gate.setAttack(0.5f);
    gate.setRelease(50.0f);

    gain.setGainDecibels(0.0f);
}

void InputChainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    currentSampleRate = sampleRate;
    pitchWindowSamples = juce::jlimit(256, 4096, juce::roundToInt(sampleRate * 0.04));
    pitchMinDelaySamples = juce::jmax(48, pitchWindowSamples / 4);
    pitchBufferSize = pitchMinDelaySamples + pitchWindowSamples + samplesPerBlock + 8;

    gate.prepare(spec);
    gain.prepare(spec);
    gain.setRampDurationSeconds(0.02);
    gain.setGainDecibels(inputGainDb);
    gain.reset();

    transposeRatioSmooth.reset(sampleRate, 0.03);
    transposeMixSmooth.reset(sampleRate, 0.03);
    transposeRatioSmooth.setCurrentAndTargetValue(semitonesToRatio(inputTranspose));
    transposeMixSmooth.setCurrentAndTargetValue(inputTranspose == 0 ? 0.0f : 1.0f);
    hardSyncParams = true;

    pitchBuffer.setSize(juce::jmax(1, getTotalNumOutputChannels()), pitchBufferSize, false, false, true);
    resetPitchShifter();
}

void InputChainProcessor::releaseResources()
{
    reset();
}

void InputChainProcessor::reset()
{
    gate.reset();
    gate.setThreshold(gateThreshold);
    gate.setRatio(12.0f);
    gate.setAttack(0.5f);
    gate.setRelease(50.0f);

    gain.setGainDecibels(inputGainDb);
    gain.reset();

    transposeRatioSmooth.setCurrentAndTargetValue(semitonesToRatio(inputTranspose));
    transposeMixSmooth.setCurrentAndTargetValue(inputTranspose == 0 ? 0.0f : 1.0f);
    resetPitchShifter();
    hardSyncParams = true;
}

void InputChainProcessor::setParams(float gainDb, float gateDb, bool forceMono, int inputTransposeSemitones)
{
    inputGainDb = gainDb;
    gateThreshold = gateDb;
    inputTranspose = juce::jlimit(-12, 12, inputTransposeSemitones);

    currentRouting = forceMono ? Nova::InputRouting::Sum
        : Nova::InputRouting::Stereo;

    if (hardSyncParams)
    {
        gain.setGainDecibels(inputGainDb);
        gain.reset();
        transposeRatioSmooth.setCurrentAndTargetValue(semitonesToRatio(inputTranspose));
        transposeMixSmooth.setCurrentAndTargetValue(inputTranspose == 0 ? 0.0f : 1.0f);
    }
    else
    {
        transposeRatioSmooth.setTargetValue(semitonesToRatio(inputTranspose));
        transposeMixSmooth.setTargetValue(inputTranspose == 0 ? 0.0f : 1.0f);
    }
}

void InputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    if (numCh > 1)
    {
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);

        switch (currentRouting)
        {
            case Nova::InputRouting::Left:
                juce::FloatVectorOperations::copy(r, l, numSamples);
                break;

            case Nova::InputRouting::Right:
                juce::FloatVectorOperations::copy(l, r, numSamples);
                break;

            case Nova::InputRouting::Sum:
                for (int i = 0; i < numSamples; ++i)
                {
                    const float sum = (l[i] + r[i]) * 0.5f;
                    l[i] = sum;
                    r[i] = sum;
                }
                break;

            default:
                break;
        }
    }

    gain.setGainDecibels(inputGainDb);
    gain.process(context);

    if (gateThreshold > -95.0f)
    {
        gate.setThreshold(gateThreshold);
        gate.process(context);
    }

    processTranspose(buffer);
    hardSyncParams = false;
}

float InputChainProcessor::semitonesToRatio(int semitones)
{
    return std::pow(2.0f, static_cast<float>(semitones) / 12.0f);
}

float InputChainProcessor::readPitchSample(int channel, float delaySamples) const
{
    if (pitchBufferSize <= 0)
        return 0.0f;

    float readPos = static_cast<float>(pitchWritePos) - delaySamples;
    while (readPos < 0.0f)
        readPos += static_cast<float>(pitchBufferSize);

    const int indexA = static_cast<int>(readPos) % pitchBufferSize;
    const int indexB = (indexA + 1) % pitchBufferSize;
    const float frac = readPos - static_cast<float>(static_cast<int>(readPos));

    const float a = pitchBuffer.getSample(channel, indexA);
    const float b = pitchBuffer.getSample(channel, indexB);
    return a + ((b - a) * frac);
}

void InputChainProcessor::resetPitchShifter()
{
    pitchPhase = 0.0f;
    pitchWritePos = 0;

    if (pitchBuffer.getNumSamples() > 0)
        pitchBuffer.clear();
}

void InputChainProcessor::processTranspose(juce::AudioBuffer<float>& buffer)
{
    const bool transposeInactive = (inputTranspose == 0)
        && !transposeMixSmooth.isSmoothing()
        && transposeMixSmooth.getCurrentValue() <= 0.0001f;

    if (transposeInactive)
    {
        resetPitchShifter();
        return;
    }

    const int numChannels = juce::jmin(2, buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const float ratio = transposeRatioSmooth.getNextValue();
        const float mix = transposeMixSmooth.getNextValue();
        const float slope = 1.0f - ratio;
        const float slopeMagnitude = std::abs(slope);
        const float phaseIncrement = (pitchWindowSamples > 0)
            ? juce::jlimit(0.0f, 0.5f, slopeMagnitude / static_cast<float>(pitchWindowSamples))
            : 0.0f;

        const float phaseA = pitchPhase;
        const float phaseB = std::fmod(pitchPhase + 0.5f, 1.0f);
        const bool pitchUp = (slope < 0.0f);

        const auto computeDelay = [this, pitchUp](float phase)
        {
            const float shapedPhase = pitchUp ? (1.0f - phase) : phase;
            return static_cast<float>(pitchMinDelaySamples)
                + shapedPhase * static_cast<float>(pitchWindowSamples);
        };

        const float delayA = computeDelay(phaseA);
        const float delayB = computeDelay(phaseB);

        const float gainA = std::sin(phaseA * juce::MathConstants<float>::pi);
        const float gainB = std::sin(phaseB * juce::MathConstants<float>::pi);
        const float gainNorm = juce::jmax(0.0001f, gainA + gainB);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = buffer.getSample(ch, sample);
            pitchBuffer.setSample(ch, pitchWritePos, dry);

            float shifted = dry;
            if (std::abs(ratio - 1.0f) > 0.0001f)
                shifted = ((readPitchSample(ch, delayA) * gainA) + (readPitchSample(ch, delayB) * gainB)) / gainNorm;

            buffer.setSample(ch, sample, (shifted * mix) + (dry * (1.0f - mix)));
        }

        pitchWritePos = (pitchWritePos + 1) % juce::jmax(1, pitchBufferSize);
        pitchPhase += phaseIncrement;
        if (pitchPhase >= 1.0f)
            pitchPhase -= 1.0f;
    }
}
