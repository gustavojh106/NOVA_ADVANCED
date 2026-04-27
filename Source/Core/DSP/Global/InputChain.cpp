#include "InputChain.h"

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

    gate.prepare(spec);
    gain.prepare(spec);
    subsonicHighPass.prepare(spec);
    *subsonicHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate,
        20.0f,
        0.70710678f);
    subsonicHighPass.reset();
    for (auto& dcBlock : dcBlockers)
        dcBlock.prepare(sampleRate);
    gain.setRampDurationSeconds(0.02);
    gain.setGainDecibels(inputGainDb);
    gain.reset();

    hardSyncParams = true;
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
    subsonicHighPass.reset();
    *subsonicHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate,
        20.0f,
        0.70710678f);
    for (auto& dcBlock : dcBlockers)
        dcBlock.reset();

    hardSyncParams = true;
}

void InputChainProcessor::setParams(float gainDb, float gateDb, bool forceMono)
{
    inputGainDb = gainDb;
    gateThreshold = gateDb;

    currentRouting = forceMono ? Nova::InputRouting::Sum
        : Nova::InputRouting::Stereo;

    if (hardSyncParams)
    {
        gain.setGainDecibels(inputGainDb);
        gain.reset();
    }
}

void InputChainProcessor::normalizeInstrumentRouting(juce::AudioBuffer<float>& buffer, Nova::InputRouting routing) noexcept
{
    if (buffer.getNumChannels() < 2 || buffer.getNumSamples() <= 0)
        return;

    auto* l = buffer.getWritePointer(0);
    auto* r = buffer.getWritePointer(1);
    const int numSamples = buffer.getNumSamples();

    switch (routing)
    {
        case Nova::InputRouting::Left:
            juce::FloatVectorOperations::copy(r, l, numSamples);
            return;

        case Nova::InputRouting::Right:
            juce::FloatVectorOperations::copy(l, r, numSamples);
            return;

        default:
            break;
    }

    float peakL = 0.0f;
    float peakR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        peakL = juce::jmax(peakL, std::abs(l[i]));
        peakR = juce::jmax(peakR, std::abs(r[i]));
    }

    constexpr float activeThreshold = 0.0005f;
    constexpr float inactiveRelativeToActive = 0.08f;
    const bool leftOnly = peakL >= activeThreshold && peakR < peakL * inactiveRelativeToActive;
    const bool rightOnly = peakR >= activeThreshold && peakL < peakR * inactiveRelativeToActive;

    if (leftOnly)
    {
        juce::FloatVectorOperations::copy(r, l, numSamples);
        return;
    }

    if (rightOnly)
    {
        juce::FloatVectorOperations::copy(l, r, numSamples);
        return;
    }

    if (routing == Nova::InputRouting::Sum)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float sum = (l[i] + r[i]) * 0.5f;
            l[i] = sum;
            r[i] = sum;
        }
    }
}

void InputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Scrub host-side garbage (NaN/Inf, denormals, runaway peaks) before any DSP
    // touches it. Without this, a corrupt input poisons the subsonic HP / DC
    // blocker IIR state for many blocks, requiring an engine-level auto-heal.
    const auto guardStats = Nova::DSP::scrub(buffer);
    if (guardStats.invalidSamples > 0)
        invalidSampleCount.fetch_add(guardStats.invalidSamples, std::memory_order_relaxed);
    if (guardStats.clippedSamples > 0)
        clippedSampleCount.fetch_add(guardStats.clippedSamples, std::memory_order_relaxed);

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    normalizeInstrumentRouting(buffer, currentRouting);

    subsonicHighPass.process(context);

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        auto& dcBlock = dcBlockers[(size_t) juce::jmin(ch, (int) dcBlockers.size() - 1)];

        for (int i = 0; i < numSamples; ++i)
            data[i] = dcBlock.process(data[i]);
    }

    gain.setGainDecibels(inputGainDb);
    gain.process(context);

    if (gateThreshold > -95.0f)
    {
        gate.setThreshold(gateThreshold);
        gate.process(context);
    }

    hardSyncParams = false;
}
