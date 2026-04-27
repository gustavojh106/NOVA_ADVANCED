#include "ChannelStrip.h"
#include <cmath>

ChannelStripProcessor::ChannelStripProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("In", juce::AudioChannelSet::stereo())
        .withOutput("Out", juce::AudioChannelSet::stereo()))
{
}

void ChannelStripProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    gain.prepare(spec);
    gain.setRampDurationSeconds(0.02);
    gain.setGainLinear(targetGain);
    gain.reset();

    panSmooth.reset(sampleRate, 0.02);
    widthSmooth.reset(sampleRate, 0.02);
    panSmooth.setCurrentAndTargetValue(targetPan);
    widthSmooth.setCurrentAndTargetValue(targetWidth);
    signalTelemetry.reset();
    debugTelemetry.resetWindow();
    hardSyncParams = true;
}

void ChannelStripProcessor::releaseResources()
{
}

void ChannelStripProcessor::reset()
{
    gain.setGainLinear(targetGain);
    gain.reset();
    panSmooth.setCurrentAndTargetValue(targetPan);
    widthSmooth.setCurrentAndTargetValue(targetWidth);
    signalTelemetry.reset();
    debugTelemetry.resetWindow();
    hardSyncParams = true;
}

void ChannelStripProcessor::setParams(float gainVal, float panVal, float widthVal)
{
    targetGain = juce::jlimit(0.0f, 2.0f, gainVal);
    targetPan = juce::jlimit(-1.0f, 1.0f, panVal);
    targetWidth = juce::jlimit(0.0f, 2.0f, widthVal);

    if (hardSyncParams)
    {
        gain.setGainLinear(targetGain);
        gain.reset();
        panSmooth.setCurrentAndTargetValue(targetPan);
        widthSmooth.setCurrentAndTargetValue(targetWidth);
    }
    else
    {
        gain.setGainLinear(targetGain);
        panSmooth.setTargetValue(targetPan);
        widthSmooth.setTargetValue(targetWidth);
    }
}

void ChannelStripProcessor::setTelemetryTag(const juce::String& newTag)
{
    telemetryTag = newTag.isNotEmpty() ? newTag : juce::String("channel-strip");
    signalTelemetry.setTag(telemetryTag);
}

void ChannelStripProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    signalTelemetry.captureInput(buffer);

    // Per-chain safety scrub: catch any NaN/Inf or runaway peaks produced by
    // the upstream pedal chain so they cannot poison this strip's gain ramp,
    // M/S width math, or downstream merger / limiter state.
    const auto guardStats = Nova::DSP::scrub(buffer);
    debugTelemetry.guardInvalidSamples += guardStats.invalidSamples;
    debugTelemetry.guardClippedSamples += guardStats.clippedSamples;
    debugTelemetry.guardDenormalSamples += guardStats.denormalSamples;

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    // 1) Gain (smoothed internally)
    gain.process(context);
    debugTelemetry.postGainStage.capture(buffer);

    if (buffer.getNumChannels() != 2)
    {
        signalTelemetry.captureOutputAndEmitIfNeeded(buffer,
            [this]()
            {
                juce::String report;
                report << debugTelemetry.postGainStage.buildSummary("gain.stage") << juce::newLine
                       << "stereo.window: unavailable=non-stereo"
                       << juce::newLine
                       << "guard: invalid=" << debugTelemetry.guardInvalidSamples
                       << ", clipped=" << debugTelemetry.guardClippedSamples
                       << ", denormals=" << debugTelemetry.guardDenormalSamples
                       << juce::newLine
                       << "params: tag=" << telemetryTag
                       << ", targetGain=" << NovaDiagnostics::formatTelemetryScalar(targetGain)
                       << ", targetPan=" << NovaDiagnostics::formatTelemetryScalar(targetPan)
                       << ", targetWidth=" << NovaDiagnostics::formatTelemetryScalar(targetWidth)
                       << ", hardSync=" << (hardSyncParams ? "true" : "false");
                return report;
            },
            [this]()
            {
                debugTelemetry.resetWindow();
            });
        return;
    }

    auto* l = buffer.getWritePointer(0);
    auto* r = buffer.getWritePointer(1);
    const int numSamples = buffer.getNumSamples();

    const bool expectMuted = targetGain <= 1.0e-4f;

    // 2) Width + smooth balance curve with per-sample smoothing
    for (int i = 0; i < numSamples; ++i)
    {
        const float width = widthSmooth.getNextValue();
        const float pan = panSmooth.getNextValue();

        const float mid = (l[i] + r[i]) * 0.5f;
        float side = (l[i] - r[i]) * 0.5f;
        side *= width;
        const float widthComp = (width > 1.0f) ? (1.0f / std::sqrt(width)) : 1.0f;

        float sampleL = (mid + side) * widthComp;
        float sampleR = (mid - side) * widthComp;

        const float clampedPan = juce::jlimit(-1.0f, 1.0f, pan);
        const float gainL = clampedPan > 0.0f
            ? std::cos(clampedPan * juce::MathConstants<float>::halfPi)
            : 1.0f;
        const float gainR = clampedPan < 0.0f
            ? std::cos(-clampedPan * juce::MathConstants<float>::halfPi)
            : 1.0f;

        const float outL = sampleL * gainL;
        const float outR = sampleR * gainR;

        debugTelemetry.captureStereo(mid,
            side,
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

    signalTelemetry.captureOutputAndEmitIfNeeded(buffer,
        [this, expectMuted]()
        {
            auto safeMin = [](float value)
            {
                return value >= 1.0e8f ? 0.0f : value;
            };

            juce::String report;
            report << debugTelemetry.postGainStage.buildSummary("gain.stage") << juce::newLine
                   << "stereo.window: midPeak=" << NovaDiagnostics::formatTelemetryScalar(debugTelemetry.midPeak)
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
                   << juce::newLine
                   << "guard: invalid=" << debugTelemetry.guardInvalidSamples
                   << ", clipped=" << debugTelemetry.guardClippedSamples
                   << ", denormals=" << debugTelemetry.guardDenormalSamples
                   << juce::newLine
                   << "params: tag=" << telemetryTag
                   << ", targetGain=" << NovaDiagnostics::formatTelemetryScalar(targetGain)
                   << ", targetPan=" << NovaDiagnostics::formatTelemetryScalar(targetPan)
                   << ", targetWidth=" << NovaDiagnostics::formatTelemetryScalar(targetWidth)
                   << ", expectedMuted=" << (expectMuted ? "true" : "false")
                   << ", hardSync=" << (hardSyncParams ? "true" : "false");
            return report;
        },
        [this]()
        {
            debugTelemetry.resetWindow();
        });

    hardSyncParams = false;
}
