#include "InputChain.h"

float InputChainProcessor::sanitizeInputGainDb(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;

    return juce::jlimit(-36.0f, 24.0f, value);
}

float InputChainProcessor::sanitizeGateDb(float value) noexcept
{
    if (!std::isfinite(value))
        return -100.0f;

    return juce::jlimit(-120.0f, 0.0f, value);
}

float InputChainProcessor::gateDbToLinear(float gateDb) noexcept
{
    const float cleanGateDb = sanitizeGateDb(gateDb);

    if (cleanGateDb <= -95.0f)
        return 0.0f;

    return juce::Decibels::decibelsToGain(cleanGateDb, -120.0f);
}

InputChainProcessor::InputChainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
}

void InputChainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);

    conditioner.prepare(sampleRate);
    autoRouting.reset();
    inputGate.prepare(sampleRate);

    currentInputGainDb = sanitizeInputGainDb(targetInputGainDb.load(std::memory_order_acquire));
    currentGateDb = sanitizeGateDb(targetGateDb.load(std::memory_order_acquire));
    currentForceMono = targetForceMono.load(std::memory_order_acquire);

    inputGainLinearSmooth.reset(sampleRate, 0.020);
    monoBlendSmooth.reset(sampleRate, 0.012);
    gateThresholdLinearSmooth.reset(sampleRate, 0.035);

    inputGainLinearSmooth.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(currentInputGainDb, -120.0f));
    monoBlendSmooth.setCurrentAndTargetValue(currentForceMono ? 1.0f : 0.0f);
    gateThresholdLinearSmooth.setCurrentAndTargetValue(gateDbToLinear(currentGateDb));

    hardSyncParams = true;
}

void InputChainProcessor::releaseResources()
{
    reset();
}

void InputChainProcessor::reset()
{
    conditioner.reset();
    autoRouting.reset();
    inputGate.reset();

    currentInputGainDb = sanitizeInputGainDb(targetInputGainDb.load(std::memory_order_acquire));
    currentGateDb = sanitizeGateDb(targetGateDb.load(std::memory_order_acquire));
    currentForceMono = targetForceMono.load(std::memory_order_acquire);

    inputGainLinearSmooth.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(currentInputGainDb, -120.0f));
    monoBlendSmooth.setCurrentAndTargetValue(currentForceMono ? 1.0f : 0.0f);
    gateThresholdLinearSmooth.setCurrentAndTargetValue(gateDbToLinear(currentGateDb));

    hardSyncParams = true;
}

void InputChainProcessor::setParams(float gainDb, float gateDb, bool forceMono)
{
    targetInputGainDb.store(sanitizeInputGainDb(gainDb), std::memory_order_release);
    targetGateDb.store(sanitizeGateDb(gateDb), std::memory_order_release);
    targetForceMono.store(forceMono, std::memory_order_release);
}

void InputChainProcessor::applyParameterTargets(bool forceSync) noexcept
{
    currentInputGainDb = sanitizeInputGainDb(targetInputGainDb.load(std::memory_order_acquire));
    currentGateDb = sanitizeGateDb(targetGateDb.load(std::memory_order_acquire));
    currentForceMono = targetForceMono.load(std::memory_order_acquire);

    const float inputGainLinear = juce::Decibels::decibelsToGain(currentInputGainDb, -120.0f);
    const float monoBlend = currentForceMono ? 1.0f : 0.0f;
    const float gateThresholdLinear = gateDbToLinear(currentGateDb);

    if (forceSync)
    {
        inputGainLinearSmooth.setCurrentAndTargetValue(inputGainLinear);
        monoBlendSmooth.setCurrentAndTargetValue(monoBlend);
        gateThresholdLinearSmooth.setCurrentAndTargetValue(gateThresholdLinear);
    }
    else
    {
        inputGainLinearSmooth.setTargetValue(inputGainLinear);
        monoBlendSmooth.setTargetValue(monoBlend);
        gateThresholdLinearSmooth.setTargetValue(gateThresholdLinear);
    }
}

void InputChainProcessor::applyForceMonoBlend(juce::AudioBuffer<float>& buffer) noexcept
{
    if (buffer.getNumChannels() < 2 || buffer.getNumSamples() <= 0)
        return;

    auto* l = buffer.getWritePointer(0);
    auto* r = buffer.getWritePointer(1);
    const int samples = buffer.getNumSamples();

    if (!monoBlendSmooth.isSmoothing())
    {
        const float blend = monoBlendSmooth.getCurrentValue();

        if (blend <= 1.0e-6f)
            return;

        for (int i = 0; i < samples; ++i)
        {
            const float mono = (l[i] + r[i]) * 0.5f;
            l[i] = l[i] + ((mono - l[i]) * blend);
            r[i] = r[i] + ((mono - r[i]) * blend);
        }

        return;
    }

    for (int i = 0; i < samples; ++i)
    {
        const float blend = monoBlendSmooth.getNextValue();
        const float mono = (l[i] + r[i]) * 0.5f;

        l[i] = l[i] + ((mono - l[i]) * blend);
        r[i] = r[i] + ((mono - r[i]) * blend);
    }
}

void InputChainProcessor::applyInputGain(juce::AudioBuffer<float>& buffer) noexcept
{
    const int channels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    const int samples = buffer.getNumSamples();

    if (channels <= 0 || samples <= 0)
        return;

    std::array<float*, kMaxChannels> data{};
    for (int ch = 0; ch < channels; ++ch)
        data[(size_t)ch] = buffer.getWritePointer(ch);

    if (!inputGainLinearSmooth.isSmoothing())
    {
        const float g = inputGainLinearSmooth.getCurrentValue();

        if (std::abs(g - 1.0f) <= 1.0e-6f)
            return;

        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::multiply(data[(size_t)ch], g, samples);

        return;
    }

    for (int i = 0; i < samples; ++i)
    {
        const float g = inputGainLinearSmooth.getNextValue();

        for (int ch = 0; ch < channels; ++ch)
            data[(size_t)ch][i] *= g;
    }
}

void InputChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    applyParameterTargets(hardSyncParams);

    const auto guardStats = Nova::DSP::scrub(buffer);

    if (guardStats.invalidSamples > 0)
        invalidSampleCount.fetch_add(guardStats.invalidSamples, std::memory_order_relaxed);

    if (guardStats.clippedSamples > 0)
        clippedSampleCount.fetch_add(guardStats.clippedSamples, std::memory_order_relaxed);

    if (guardStats.denormalSamples > 0)
        denormalSampleCount.fetch_add(guardStats.denormalSamples, std::memory_order_relaxed);

    autoRouting.process(buffer);
    applyForceMonoBlend(buffer);
    conditioner.process(buffer);
    applyInputGain(buffer);
    inputGate.process(buffer, gateThresholdLinearSmooth);

    hardSyncParams = false;
}
