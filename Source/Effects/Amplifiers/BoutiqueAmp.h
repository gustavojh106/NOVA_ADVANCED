#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <limits>

#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"

// ============================================================================
//  Boutique Amp — Dumble / Two-Rock / Matchless inspired
//
//  Transparent amplification: the signal character comes from the guitar and
//  pickups, not from the amp coloring. High headroom, touch-sensitive breakup
//  that responds to pick attack dynamics. Even-harmonic warmth without fizz.
//
//  Key behaviors:
//    - At low drive: pristine clean with subtle tube warmth
//    - At medium drive: edge-of-breakup that cleans up with volume knob
//    - At high drive: smooth, singing sustain without harsh clipping
//    - Dynamic range preserved at all settings
//
//  Signal chain (all at 4x oversampled rate):
//    Input HP → Input shelf (warmth) → Dynamic saturation (envelope-responsive)
//    → Mid contour → Presence → DC block → Downsample → Master
// ============================================================================

class BoutiqueAmp final : public ProcessorBase
{
public:
    BoutiqueAmp()
        : oversampler(2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam   = new juce::AudioParameterFloat("boutDrive",   "Drive",   0.2f, 3.5f, 1.2f));
        addParameter(warmthParam  = new juce::AudioParameterFloat("boutWarmth",  "Warmth",  0.0f, 1.0f, 0.50f));
        addParameter(midParam     = new juce::AudioParameterFloat("boutMid",     "Mid",     0.0f, 1.0f, 0.55f));
        addParameter(presParam    = new juce::AudioParameterFloat("boutPres",    "Presence", 0.0f, 1.0f, 0.50f));
        addParameter(levelParam   = new juce::AudioParameterFloat("boutLevel",   "Master",  0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Boutique Amp"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Amplifier",
            "Velvet",
            juce::Colour::fromString("ffA78BFA"),
            {
                { "Drive",    driveParam,  [](float v) { return formatGain(v); } },
                { "Warmth",   warmthParam, [](float v) { return formatPercent(v); } },
                { "Mid",      midParam,    [](float v) { return formatPercent(v); } },
                { "Presence", presParam,   [](float v) { return formatPercent(v); } },
                { "Master",   levelParam,  [](float v) { return formatGain(v); } }
            },
            214,
            178);
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        oversampler.reset();
        oversampler.initProcessing((size_t)juce::jmax(1, samplesPerBlock));

        innerRate = sampleRate * 8.0;

        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate       = innerRate;
        innerSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock * 8);
        innerSpec.numChannels      = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        inputHP.prepare(innerSpec);
        warmthShelf.prepare(innerSpec);
        midPeak.prepare(innerSpec);
        presenceShelf.prepare(innerSpec);
        outputLP.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        driveSmooth.reset(innerRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        masterSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 1.2f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        cachedWarmth = std::numeric_limits<float>::quiet_NaN();
        cachedMid    = std::numeric_limits<float>::quiet_NaN();
        cachedPres   = std::numeric_limits<float>::quiet_NaN();

        setProcessingLatency((int)oversampler.getLatencyInSamples());
        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override { isPrepared = false; }

    void reset() override
    {
        oversampler.reset();
        inputHP.reset();
        warmthShelf.reset();
        midPeak.reset();
        presenceShelf.reset();
        outputLP.reset();
        dcBlock.reset();

        for (auto& e : envelope) e = 0.0f;

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 1.2f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateVoicingIfNeeded();
        driveSmooth.setTargetValue(driveParam != nullptr ? *driveParam : 1.2f);
        masterSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampled);

        inputHP.process(context);
        warmthShelf.process(context);

        // ---- Dynamic touch-sensitive saturation ----
        {
            const int channels = (int)upsampled.getNumChannels();
            const int samples  = (int)upsampled.getNumSamples();
            std::array<float*, kMaxAmpChannels> channelData{};
            const int channelsToProcess = juce::jmin(channels, kMaxAmpChannels);
            for (int ch = 0; ch < channelsToProcess; ++ch)
                channelData[(size_t) ch] = upsampled.getChannelPointer((size_t) ch);

            for (int s = 0; s < samples; ++s)
            {
                const float drive = driveSmooth.getNextValue();

                for (int ch = 0; ch < channelsToProcess; ++ch)
                {
                    auto* data = channelData[(size_t) ch];
                    float x = data[s];

                    // --- Envelope follower for dynamic response ---
                    // Fast attack (catches pick transient), slow release (sustain)
                    const float absX = std::abs(x * drive);
                    const float envCoeff = (absX > envelope[(size_t)ch]) ? envAttack : envRelease;
                    envelope[(size_t)ch] += (absX - envelope[(size_t)ch]) * envCoeff;

                    // --- Dynamic gain: reduces drive when input is hot ---
                    // This is what makes boutique amps "clean up" with guitar volume
                    // High envelope → less gain → stays clean on hard hits
                    // Low envelope → more gain → breaks up on soft picking
                    const float dynamicGain = drive * (1.0f / (1.0f + envelope[(size_t)ch] * 0.6f));

                    // --- Stage 1: Preamp tube (high headroom) ---
                    // Soft polynomial curve below threshold, tanh above
                    // This gives transparent clean at low levels, smooth breakup at high
                    const float input1 = x * (0.8f + dynamicGain * 1.4f);
                    float stage1;
                    if (std::abs(input1) < 0.6f)
                    {
                        // Below threshold: gentle 2nd harmonic warmth (even harmonics)
                        // y = x - (x^2 * sign(x) * amount) — adds only H2
                        const float sign = (input1 >= 0.0f) ? 1.0f : -1.0f;
                        stage1 = input1 - (input1 * input1 * sign * 0.12f);
                    }
                    else
                    {
                        // Above threshold: smooth tanh compression
                        stage1 = std::tanh(input1 * 0.85f) * 1.12f;
                    }

                    // --- Stage 2: Power section (very light) ---
                    // Just a hint of compression — boutique amps have huge headroom
                    const float input2 = stage1 * (1.05f + drive * 0.18f);
                    const float stage2 = std::tanh(input2 * 0.65f) * 1.35f;

                    // --- Mix: mostly stage1 (transparent), stage2 adds body ---
                    data[s] = (stage1 * 0.55f) + (stage2 * 0.45f);
                }
            }
        }

        midPeak.process(context);
        presenceShelf.process(context);
        outputLP.process(context);
        dcBlock.process(context);
        oversampler.processSamplesDown(block);

        // Master volume
        for (int s = 0; s < buffer.getNumSamples(); ++s)
        {
            const float master = masterSmooth.getNextValue();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample(ch, s, buffer.getSample(ch, s) * master);
        }

        endBypassProcess(buffer);
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>>;

    void updateVoicingIfNeeded()
    {
        const float warmth = warmthParam != nullptr ? *warmthParam : 0.50f;
        const float mid    = midParam    != nullptr ? *midParam    : 0.55f;
        const float pres   = presParam   != nullptr ? *presParam   : 0.50f;

        const bool warmthChanged = !std::isfinite(cachedWarmth) || std::abs(cachedWarmth - warmth) > 1.0e-4f;
        const bool midChanged    = !std::isfinite(cachedMid)    || std::abs(cachedMid    - mid)    > 1.0e-4f;
        const bool presChanged   = !std::isfinite(cachedPres)   || std::abs(cachedPres   - pres)   > 1.0e-4f;
        if (!warmthChanged && !midChanged && !presChanged)
            return;

        cachedWarmth = warmth;
        cachedMid    = mid;
        cachedPres   = pres;

        // Warmth: low shelf at 200 Hz — adds body without mud
        const float warmthGain = juce::jmap(warmth, -3.0f, 6.0f);

        // Mid: peak filter centered around 800 Hz — the "Dumble mid hump"
        // Q is moderate (1.0) — broad and musical, not surgical
        const float midGain = juce::jmap(mid, -4.0f, 6.0f);

        // Presence: high shelf at 4 kHz — adds air and articulation
        const float presGain = juce::jmap(pres, -3.0f, 6.0f);

        // Output LP: gentle rolloff to prevent harshness (boutique amps are never fizzy)
        const float lpFreq = juce::jmap(pres, 7000.0f, 14000.0f);

        *inputHP.state       = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(innerRate, 25.0f);
        *warmthShelf.state   = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf(innerRate,
                                    200.0f, 0.72f, juce::Decibels::decibelsToGain(warmthGain));
        *midPeak.state       = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter(innerRate,
                                    800.0f, 1.0f, juce::Decibels::decibelsToGain(midGain));
        *presenceShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf(innerRate,
                                    4000.0f, 0.68f, juce::Decibels::decibelsToGain(presGain));
        *outputLP.state      = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass(innerRate, lpFreq, 0.55f);
        *dcBlock.state       = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(innerRate, 18.0f);
    }

    // Envelope follower coefficients at 4x rate (~176.4 kHz)
    // Attack: ~0.3 ms (catches pick transient immediately)
    // Release: ~80 ms (slow enough to sustain, fast enough to feel dynamics)
    static constexpr float envAttack  = 0.018f;
    static constexpr float envRelease = 0.00006f;

    juce::dsp::Oversampling<float> oversampler;
    Filter inputHP;
    Filter warmthShelf;
    Filter midPeak;
    Filter presenceShelf;
    Filter outputLP;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterSmooth;

    static constexpr int kMaxAmpChannels = 8;
    std::array<float, kMaxAmpChannels> envelope{};

    juce::AudioParameterFloat* driveParam   = nullptr;
    juce::AudioParameterFloat* warmthParam  = nullptr;
    juce::AudioParameterFloat* midParam     = nullptr;
    juce::AudioParameterFloat* presParam    = nullptr;
    juce::AudioParameterFloat* levelParam   = nullptr;

    double innerRate = 352800.0;
    float cachedWarmth = std::numeric_limits<float>::quiet_NaN();
    float cachedMid    = std::numeric_limits<float>::quiet_NaN();
    float cachedPres   = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
