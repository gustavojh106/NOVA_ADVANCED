#include "ChannelStrip.h"

#include <cmath>

float ChannelStripProcessor::sanitizeGain(float value) noexcept
{
    if (!std::isfinite(value))
        return 1.0f;

    return juce::jlimit(0.0f, 2.0f, value);
}

float ChannelStripProcessor::sanitizePan(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;

    return juce::jlimit(-1.0f, 1.0f, value);
}

float ChannelStripProcessor::sanitizeWidth(float value) noexcept
{
    if (!std::isfinite(value))
        return 1.0f;

    return juce::jlimit(0.0f, 2.0f, value);
}

float ChannelStripProcessor::calculateWidthCompensation(float width) noexcept
{
    width = sanitizeWidth(width);

    if (width <= 1.0f)
        return 1.0f;

    // Energy-aware compensation for widened stereo.
    // width=2 -> about -4 dB side-expansion compensation, less aggressive pumping downstream.
    return 1.0f / std::sqrt(0.5f * (1.0f + (width * width)));
}

void ChannelStripProcessor::calculateBalanceGains(float pan, float& gainL, float& gainR) noexcept
{
    const float p = sanitizePan(pan);

    // Stereo balance law:
    // center keeps both channels at unity.
    // hard left/right fades only the opposite channel.
    gainL = p > 0.0f ? std::cos(p * juce::MathConstants<float>::halfPi) : 1.0f;
    gainR = p < 0.0f ? std::cos(-p * juce::MathConstants<float>::halfPi) : 1.0f;
}

ChannelStripProcessor::ChannelStripProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
}

void ChannelStripProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);

    currentTargetGain = sanitizeGain(targetGain.load(std::memory_order_acquire));
    currentTargetPan = sanitizePan(targetPan.load(std::memory_order_acquire));
    currentTargetWidth = sanitizeWidth(targetWidth.load(std::memory_order_acquire));

    gainSmooth.reset(sampleRate, 0.015);
    panSmooth.reset(sampleRate, 0.020);
    widthSmooth.reset(sampleRate, 0.025);

    gainSmooth.setCurrentAndTargetValue(currentTargetGain);
    panSmooth.setCurrentAndTargetValue(currentTargetPan);
    widthSmooth.setCurrentAndTargetValue(currentTargetWidth);

    signalTelemetry.reset();
    debugTelemetry.resetWindow();

    hardSyncParams = true;
}

void ChannelStripProcessor::releaseResources()
{
}

void ChannelStripProcessor::reset()
{
    currentTargetGain = sanitizeGain(targetGain.load(std::memory_order_acquire));
    currentTargetPan = sanitizePan(targetPan.load(std::memory_order_acquire));
    currentTargetWidth = sanitizeWidth(targetWidth.load(std::memory_order_acquire));

    gainSmooth.setCurrentAndTargetValue(currentTargetGain);
    panSmooth.setCurrentAndTargetValue(currentTargetPan);
    widthSmooth.setCurrentAndTargetValue(currentTargetWidth);

    signalTelemetry.reset();
    debugTelemetry.resetWindow();

    hardSyncParams = true;
}

void ChannelStripProcessor::setParams(float gainVal, float panVal, float widthVal)
{
    targetGain.store(sanitizeGain(gainVal), std::memory_order_release);
    targetPan.store(sanitizePan(panVal), std::memory_order_release);
    targetWidth.store(sanitizeWidth(widthVal), std::memory_order_release);
}

void ChannelStripProcessor::setTelemetryTag(const juce::String& newTag)
{
    telemetryTag = newTag.isNotEmpty() ? newTag : juce::String("channel-strip");
    signalTelemetry.setTag(telemetryTag);
}

void ChannelStripProcessor::applyParameterTargets(bool forceSync) noexcept
{
    currentTargetGain = sanitizeGain(targetGain.load(std::memory_order_acquire));
    currentTargetPan = sanitizePan(targetPan.load(std::memory_order_acquire));
    currentTargetWidth = sanitizeWidth(targetWidth.load(std::memory_order_acquire));

    if (forceSync)
    {
        gainSmooth.setCurrentAndTargetValue(currentTargetGain);
        panSmooth.setCurrentAndTargetValue(currentTargetPan);
        widthSmooth.setCurrentAndTargetValue(currentTargetWidth);
    }
    else
    {
        gainSmooth.setTargetValue(currentTargetGain);
        panSmooth.setTargetValue(currentTargetPan);
        widthSmooth.setTargetValue(currentTargetWidth);
    }
}

void ChannelStripProcessor::applyGainOnly(juce::AudioBuffer<float>& buffer, bool expectMuted) noexcept
{
    const int channels = buffer.getNumChannels();
    const int samples = buffer.getNumSamples();

    if (channels <= 0 || samples <= 0)
        return;

    if (expectMuted
        && !gainSmooth.isSmoothing()
        && gainSmooth.getCurrentValue() <= 1.0e-6f)
    {
        buffer.clear();
        ++debugTelemetry.mutedFastPathBlocks;
        return;
    }

    if (!gainSmooth.isSmoothing())
    {
        const float g = gainSmooth.getCurrentValue();
        debugTelemetry.captureControls(g, currentTargetPan, currentTargetWidth);

        if (std::abs(g - 1.0f) <= 1.0e-6f)
            return;

        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::multiply(buffer.getWritePointer(ch), g, samples);

        return;
    }

    std::array<float*, 8> data{};
    const int channelsToProcess = juce::jmin(channels, (int)data.size());

    for (int ch = 0; ch < channelsToProcess; ++ch)
        data[(size_t)ch] = buffer.getWritePointer(ch);

    for (int i = 0; i < samples; ++i)
    {
        const float g = gainSmooth.getNextValue();
        debugTelemetry.captureControls(g, currentTargetPan, currentTargetWidth);

        for (int ch = 0; ch < channelsToProcess; ++ch)
            data[(size_t)ch][i] *= g;
    }
}

void ChannelStripProcessor::emitTelemetry(juce::AudioBuffer<float>& buffer,
    bool stereoAvailable,
    bool expectMuted)
{
    signalTelemetry.captureOutputAndEmitIfNeeded(buffer,
        [this, stereoAvailable, expectMuted]()
        {
            auto safeMin = [](float value)
                {
                    return value >= 1.0e8f ? 0.0f : value;
                };

            juce::String report;
            report << debugTelemetry.postGainStage.buildSummary("gain.stage") << juce::newLine;

            if (stereoAvailable)
            {
                report << "stereo.window: midPeak=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.midPeak)
                    << ", sideInputPeak=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.sideInputPeak)
                    << ", sideOutputPeak=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.sideOutputPeak)
                    << ", widthCompMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.widthCompMin))
                    << ", widthCompMax=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.widthCompMax)
                    << ", panGainLMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.panGainLMin))
                    << ", panGainLMax=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.panGainLMax)
                    << ", panGainRMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.panGainRMin))
                    << ", panGainRMax=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.panGainRMax)
                    << ", muteLeakPeak=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.muteLeakPeak)
                    << ", muteLeakSamples=" << debugTelemetry.muteLeakSamples
                    << ", mutedFastPathBlocks=" << debugTelemetry.mutedFastPathBlocks
                    << juce::newLine;
            }
            else
            {
                report << "stereo.window: unavailable=non-stereo"
                    << ", mutedFastPathBlocks=" << debugTelemetry.mutedFastPathBlocks
                    << juce::newLine;
            }

            report << "guard: invalid=" << debugTelemetry.guardInvalidSamples
                << ", clipped=" << debugTelemetry.guardClippedSamples
                << ", denormals=" << debugTelemetry.guardDenormalSamples
                << juce::newLine
                << "params: tag=" << telemetryTag
                << ", targetGain=" << NovaDiagnostics::formatTelemetryScalar(currentTargetGain)
                << ", targetPan=" << NovaDiagnostics::formatTelemetryScalar(currentTargetPan)
                << ", targetWidth=" << NovaDiagnostics::formatTelemetryScalar(currentTargetWidth)
                << ", gainMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.gainMin))
                << ", gainMax=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.gainMax)
                << ", panMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.panMin))
                << ", panMax=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.panMax <= -1.0e8f ? 0.0f : debugTelemetry.panMax)
                << ", widthMin=" << NovaDiagnostics::formatTelemetryScalar(safeMin(debugTelemetry.widthMin))
                << ", widthMax=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.widthMax)
                << ", expectedMuted=" << (expectMuted ? "true" : "false")
                << ", hardSync=" << (hardSyncParams ? "true" : "false");
            return report;
        },
        [this]()
        {
            debugTelemetry.resetWindow();
        });
}

void ChannelStripProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    signalTelemetry.captureInput(buffer);

    applyParameterTargets(hardSyncParams);

    const bool expectMuted = currentTargetGain <= 1.0e-4f;

    const auto guardStats = Nova::DSP::scrub(buffer);
    debugTelemetry.guardInvalidSamples += guardStats.invalidSamples;
    debugTelemetry.guardClippedSamples += guardStats.clippedSamples;
    debugTelemetry.guardDenormalSamples += guardStats.denormalSamples;

    applyGainOnly(buffer, expectMuted);
    debugTelemetry.postGainStage.capture(buffer);

    if (buffer.getNumChannels() != 2)
    {
        emitTelemetry(buffer, false, expectMuted);
        hardSyncParams = false;
        return;
    }

    if (expectMuted
        && !gainSmooth.isSmoothing()
        && gainSmooth.getCurrentValue() <= 1.0e-6f)
    {
        // Already cleared in applyGainOnly().
        emitTelemetry(buffer, true, expectMuted);
        hardSyncParams = false;
        return;
    }

    auto* l = buffer.getWritePointer(0);
    auto* r = buffer.getWritePointer(1);
    const int samples = buffer.getNumSamples();

    const bool widthStatic = !widthSmooth.isSmoothing();
    const bool panStatic = !panSmooth.isSmoothing();

    const float staticWidth = widthSmooth.getCurrentValue();
    const float staticPan = panSmooth.getCurrentValue();

    for (int i = 0; i < samples; ++i)
    {
        const float width = widthStatic ? staticWidth : widthSmooth.getNextValue();
        const float pan = panStatic ? staticPan : panSmooth.getNextValue();

        debugTelemetry.captureControls(gainSmooth.getCurrentValue(), pan, width);

        const float inL = l[i];
        const float inR = r[i];

        const float mid = (inL + inR) * 0.5f;
        const float sideInput = (inL - inR) * 0.5f;

        const float side = sideInput * width;
        const float widthComp = calculateWidthCompensation(width);

        float sampleL = (mid + side) * widthComp;
        float sampleR = (mid - side) * widthComp;

        float gainL = 1.0f;
        float gainR = 1.0f;
        calculateBalanceGains(pan, gainL, gainR);

        const float outL = sampleL * gainL;
        const float outR = sampleR * gainR;

        debugTelemetry.captureStereo(mid,
            sideInput,
            side * widthComp,
            widthComp,
            gainL,
            gainR,
            outL,
            outR,
            expectMuted);

        l[i] = outL;
        r[i] = outR;
    }

    emitTelemetry(buffer, true, expectMuted);

    hardSyncParams = false;
}
