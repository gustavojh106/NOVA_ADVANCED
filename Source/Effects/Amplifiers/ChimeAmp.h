#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

#include "../Pedals/Base/ProcessorBase.h"
#include "../Pedals/Base/PremiumPedalUI.h"

// ============================================================================
//  Chime Amp — AC30 Top Boost inspired
//
//  Class A cathode-bias character: no negative feedback, pronounced upper-mid
//  chime, spongy compression from cathode bias sag, and a distinctive
//  treble-cut / bass-cut interactive tone stack.
//
//  Signal chain (all at 4x oversampled rate):
//    Input HP → Brilliance shelf → Cathode bias stage (2-stage Class A sat)
//    → Tone cut (interactive treble/bass) → Presence shelf → DC block
//    → Downsample → Master level
// ============================================================================

class ChimeAmp final : public ProcessorBase
{
public:
    ChimeAmp()
        : oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR)
    {
        addParameter(driveParam     = new juce::AudioParameterFloat("chimeDrive",     "Drive",     0.3f, 4.0f, 1.6f));
        addParameter(trebleCutParam = new juce::AudioParameterFloat("chimeTrebleCut", "Treble",    0.0f, 1.0f, 0.62f));
        addParameter(bassCutParam   = new juce::AudioParameterFloat("chimeBassCut",   "Bass",      0.0f, 1.0f, 0.48f));
        addParameter(brillParam     = new juce::AudioParameterFloat("chimeBrill",     "Brilliance",0.0f, 1.0f, 0.55f));
        addParameter(levelParam     = new juce::AudioParameterFloat("chimeLevel",     "Master",    0.0f, 2.0f, 1.0f));
    }

    const juce::String getName() const override { return "Chime Amp"; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        using namespace Nova::PedalUI;

        return new PremiumPedalEditor(*this,
            "Amplifier",
            "Chime",
            juce::Colour::fromString("ff22D3EE"),
            {
                { "Drive",      driveParam,     [](float v) { return formatGain(v); } },
                { "Treble",     trebleCutParam, [](float v) { return formatPercent(v); } },
                { "Bass",       bassCutParam,   [](float v) { return formatPercent(v); } },
                { "Brilliance", brillParam,     [](float v) { return formatPercent(v); } },
                { "Master",     levelParam,     [](float v) { return formatGain(v); } }
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

        innerRate = sampleRate * 4.0;

        juce::dsp::ProcessSpec innerSpec;
        innerSpec.sampleRate       = innerRate;
        innerSpec.maximumBlockSize = (juce::uint32)juce::jmax(1, samplesPerBlock * 4);
        innerSpec.numChannels      = (juce::uint32)juce::jmax(1, getTotalNumOutputChannels());

        inputHP.prepare(innerSpec);
        brillShelf.prepare(innerSpec);
        toneLowPass.prepare(innerSpec);
        toneHighPass.prepare(innerSpec);
        presenceShelf.prepare(innerSpec);
        dcBlock.prepare(innerSpec);

        driveSmooth.reset(innerRate, Nova::Config::SMOOTH_DRIVE_SECONDS);
        masterSmooth.reset(sampleRate, Nova::Config::SMOOTH_DEFAULT_SECONDS);

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 1.6f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        cachedTreble = std::numeric_limits<float>::quiet_NaN();
        cachedBass   = std::numeric_limits<float>::quiet_NaN();
        cachedBrill  = std::numeric_limits<float>::quiet_NaN();

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
        brillShelf.reset();
        toneLowPass.reset();
        toneHighPass.reset();
        presenceShelf.reset();
        dcBlock.reset();

        for (auto& e : sagEnv) e = 0.0f;
        for (auto& b : cathodeBias) b = 0.0f;

        driveSmooth.setCurrentAndTargetValue(driveParam != nullptr ? *driveParam : 1.6f);
        masterSmooth.setCurrentAndTargetValue(levelParam != nullptr ? *levelParam : 1.0f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        updateVoicingIfNeeded();
        driveSmooth.setTargetValue(driveParam != nullptr ? *driveParam : 1.6f);
        masterSmooth.setTargetValue(levelParam != nullptr ? *levelParam : 1.0f);

        juce::dsp::AudioBlock<float> block(buffer);
        auto upsampled = oversampler.processSamplesUp(block);
        juce::dsp::ProcessContextReplacing<float> context(upsampled);

        inputHP.process(context);
        brillShelf.process(context);

        // ---- Class A cathode-bias saturation ----
        {
            const int channels = (int)upsampled.getNumChannels();
            const int samples  = (int)upsampled.getNumSamples();

            for (int s = 0; s < samples; ++s)
            {
                const float drive = driveSmooth.getNextValue();
                const float preGain = 1.0f + drive * 1.5f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = upsampled.getChannelPointer((size_t)ch);
                    float x = data[s];

                    // --- Cathode bias: slow envelope creates spongy compression ---
                    // Class A amps have cathode resistor that shifts bias under load
                    const float rectified = std::abs(x * drive);
                    cathodeBias[(size_t)ch] += (rectified - cathodeBias[(size_t)ch]) * cathodeBiasCoeff;
                    const float biasShift = cathodeBias[(size_t)ch] * 0.35f;

                    // --- Power supply sag (lighter than push-pull, more spongy) ---
                    sagEnv[(size_t)ch] = juce::jmax(rectified, sagEnv[(size_t)ch] * 0.9988f);
                    const float sag = 1.0f / (1.0f + sagEnv[(size_t)ch] * 0.65f);

                    // --- Stage 1: EF86 pentode preamp ---
                    // Asymmetric: sharper positive clip, softer negative (pentode character)
                    const float input1 = (x * preGain * sag) - biasShift + 0.03f;
                    const float stage1 = (input1 >= 0.0f)
                        ? std::tanh(input1 * 1.3f)
                        : std::tanh(input1 * 0.95f) * 1.05f;

                    // --- Stage 2: EL84 power tube ---
                    // Class A: both halves of the wave hit the same tube pair
                    // More even-harmonic content than push-pull
                    const float input2 = stage1 * (1.4f + drive * 0.25f) + 0.02f;
                    const float stage2 = std::tanh(input2);

                    // --- Mix: more stage2 weight (Class A has less headroom) ---
                    data[s] = (stage1 * 0.32f) + (stage2 * 0.68f);
                }
            }
        }

        toneLowPass.process(context);
        toneHighPass.process(context);
        presenceShelf.process(context);
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
        const float treble = trebleCutParam != nullptr ? *trebleCutParam : 0.62f;
        const float bass   = bassCutParam   != nullptr ? *bassCutParam   : 0.48f;
        const float brill  = brillParam     != nullptr ? *brillParam     : 0.55f;

        const bool trebleChanged = !std::isfinite(cachedTreble) || std::abs(cachedTreble - treble) > 1.0e-4f;
        const bool bassChanged   = !std::isfinite(cachedBass)   || std::abs(cachedBass   - bass)   > 1.0e-4f;
        const bool brillChanged  = !std::isfinite(cachedBrill)  || std::abs(cachedBrill  - brill)  > 1.0e-4f;
        if (!trebleChanged && !bassChanged && !brillChanged)
            return;

        cachedTreble = treble;
        cachedBass   = bass;
        cachedBrill  = brill;

        // AC30 tone stack: treble = LP cutoff (cuts highs), bass = HP cutoff (cuts lows)
        // These interact — both cutting narrows the band for "cocked wah" territory
        const float lpFreq = juce::jmap(treble, 2200.0f, 12000.0f);   // treble full open = 12k
        const float hpFreq = juce::jmap(bass,   55.0f,   250.0f);     // bass full open = 55 Hz HP

        // Brilliance: high shelf boost at 3.5 kHz — the AC30 "Top Boost" channel
        const float brillGain = juce::jmap(brill, -2.0f, 9.0f);

        *inputHP.state      = *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerRate, 28.0f);
        *brillShelf.state   = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(innerRate,
                                   3500.0f, 0.65f, juce::Decibels::decibelsToGain(brillGain));
        *toneLowPass.state  = *juce::dsp::IIR::Coefficients<float>::makeLowPass(innerRate, lpFreq, 0.58f);
        *toneHighPass.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerRate, hpFreq, 0.58f);
        *presenceShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(innerRate,
                                    5200.0f, 0.72f, juce::Decibels::decibelsToGain(juce::jmap(treble, -1.0f, 3.5f)));
        *dcBlock.state      = *juce::dsp::IIR::Coefficients<float>::makeHighPass(innerRate, 18.0f);
    }

    // Cathode bias coefficient: ~5 ms time constant at 4x rate
    // This is the "spongy" feel — cathode cap charges/discharges slowly
    static constexpr float cathodeBiasCoeff = 0.0012f;

    juce::dsp::Oversampling<float> oversampler;
    Filter inputHP;
    Filter brillShelf;
    Filter toneLowPass;
    Filter toneHighPass;
    Filter presenceShelf;
    Filter dcBlock;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterSmooth;

    std::array<float, 2> sagEnv      { 0.0f, 0.0f };
    std::array<float, 2> cathodeBias { 0.0f, 0.0f };

    juce::AudioParameterFloat* driveParam     = nullptr;
    juce::AudioParameterFloat* trebleCutParam = nullptr;
    juce::AudioParameterFloat* bassCutParam   = nullptr;
    juce::AudioParameterFloat* brillParam     = nullptr;
    juce::AudioParameterFloat* levelParam     = nullptr;

    double innerRate = 176400.0;
    float cachedTreble = std::numeric_limits<float>::quiet_NaN();
    float cachedBass   = std::numeric_limits<float>::quiet_NaN();
    float cachedBrill  = std::numeric_limits<float>::quiet_NaN();
    bool isPrepared = false;
};
