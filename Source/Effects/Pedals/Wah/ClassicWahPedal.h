#pragma once

#include "../Base/ProcessorBase.h"

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

namespace Nova::WahDSP
{
inline float blend(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept
    {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    void setBandPass(float freq, float q, double sr) noexcept
    {
        const float clampedFreq = juce::jlimit(20.0f, (float) (sr * 0.45), freq);
        const float w0 = juce::MathConstants<float>::twoPi * clampedFreq / (float) sr;
        const float sinW0 = std::sin(w0);
        const float cosW0 = std::cos(w0);
        const float alpha = sinW0 / (2.0f * juce::jmax(0.2f, q));
        const float a0inv = 1.0f / (1.0f + alpha);

        b0 = alpha * a0inv;
        b1 = 0.0f;
        b2 = -alpha * a0inv;
        a1 = -2.0f * cosW0 * a0inv;
        a2 = (1.0f - alpha) * a0inv;
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct SVF
{
    float ic1eq = 0.0f;
    float ic2eq = 0.0f;

    void reset() noexcept
    {
        ic1eq = 0.0f;
        ic2eq = 0.0f;
    }

    struct Output
    {
        float lp = 0.0f;
        float bp = 0.0f;
        float hp = 0.0f;
    };

    Output process(float x, float g, float k) noexcept
    {
        const float hp = (x - (k + g) * ic1eq - ic2eq) / (1.0f + k * g + g * g);
        const float bp = g * hp + ic1eq;
        ic1eq = g * hp + bp;
        const float lp = g * bp + ic2eq;
        ic2eq = g * bp + lp;
        return { lp, bp, hp };
    }
};

inline float jfetSaturate(float x) noexcept
{
    const float x2 = x * x;
    if (x >= 0.0f)
        return x / (1.0f + 0.62f * x2);

    return x / (1.0f + 0.36f * x2);
}

inline float smoothClip(float x) noexcept
{
    if (x > 2.2f)
        return 1.0f;
    if (x < -2.2f)
        return -1.0f;

    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float normaliseRaw(float raw, float minValue, float maxValue) noexcept
{
    if (maxValue <= minValue)
        return 0.0f;

    return juce::jlimit(0.0f, 1.0f, (raw - minValue) / (maxValue - minValue));
}

inline float denormaliseValue(float normalised, float minValue, float maxValue) noexcept
{
    return juce::jmap(juce::jlimit(0.0f, 1.0f, normalised), minValue, maxValue);
}
} // namespace Nova::WahDSP

class ClassicWahPedal : public ProcessorBase
{
public:
    enum class MotionMode
    {
        Pedal = 0,
        Touch,
        Hybrid
    };

    enum class ExternalControlSource
    {
        MouseWheel = 0,
        ShortcutHold
    };

    ClassicWahPedal()
    {
        addParameter(modeParam = new juce::AudioParameterChoice("wahMode",
            "Mode",
            juce::StringArray{ "Pedal", "Touch", "Hybrid" },
            0));
        addParameter(sweepParam = new juce::AudioParameterFloat("wahSweep", "Sweep", 0.0f, 1.0f, 0.46f));
        addParameter(sensitivityParam = new juce::AudioParameterFloat("wahSens", "Sensitivity", 0.0f, 1.0f, 0.58f));
        addParameter(attackParam = new juce::AudioParameterFloat("wahAttack", "Attack", 0.5f, 30.0f, 2.0f));
        addParameter(decayParam = new juce::AudioParameterFloat("wahDecay", "Decay", 10.0f, 800.0f, 120.0f));
        addParameter(rangeParam = new juce::AudioParameterFloat("wahRange", "Range", 0.0f, 1.0f, 0.76f));
        addParameter(resonanceParam = new juce::AudioParameterFloat("wahResonance", "Resonance", 0.5f, 10.0f, 4.2f));
        addParameter(voiceParam = new juce::AudioParameterFloat("wahVoice", "Voice", 0.0f, 1.0f, 0.36f));
        addParameter(mixParam = new juce::AudioParameterFloat("wahMix", "Mix", 0.0f, 1.0f, 1.0f));
    }

    const juce::String getName() const override { return "Wah"; }
    double getTailLengthSeconds() const override { return 0.2; }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        if (sampleRate <= 0.0)
            return;

        sr = sampleRate;
        piOverSr = juce::MathConstants<float>::pi / (float) sr;
        preparedBlockSize = juce::jmax(1, samplesPerBlock);
        preparedChannels = juce::jlimit(1, kMaxProcessingChannels, juce::jmax(2, getTotalNumOutputChannels()));

        sweepSmooth.reset(sr, 0.012);
        sensSmooth.reset(sr, 0.040);
        attackSmooth.reset(sr, 0.040);
        decaySmooth.reset(sr, 0.040);
        rangeSmooth.reset(sr, 0.040);
        resonanceSmooth.reset(sr, 0.040);
        voiceSmooth.reset(sr, 0.040);
        mixSmooth.reset(sr, 0.040);

        syncSmoothedTargets(true);

        for (auto& detectorFilter : detectorBandPass)
        {
            detectorFilter.reset();
            detectorFilter.setBandPass(950.0f, 0.85f, sr);
        }

        for (auto& filter : svf)
            filter.reset();
        for (auto& filter : svfCascade)
            filter.reset();
        for (auto& dcBlock : outputDcBlock)
            dcBlock.prepare(sr, 18.0f);

        envelope = 0.0f;
        envelopeSmoothed = 0.0f;
        currentFreq = 400.0f;
        currentSweep = sweepParam != nullptr ? sweepParam->get() : 0.46f;
        currentManualSweep = currentSweep;
        currentEnvelope = 0.0f;

        scratchBuffer.setSize(preparedChannels,
            preparedBlockSize,
            false,
            false,
            true);

        prepareBypassSmoother(sampleRate, samplesPerBlock);
        reset();
        isPrepared = true;
    }

    void releaseResources() override
    {
        isPrepared = false;
    }

    void reset() override
    {
        envelope = 0.0f;
        envelopeSmoothed = 0.0f;

        for (auto& detectorFilter : detectorBandPass)
            detectorFilter.reset();
        for (auto& filter : svf)
            filter.reset();
        for (auto& filter : svfCascade)
            filter.reset();
        for (auto& dcBlock : outputDcBlock)
            dcBlock.reset();

        syncSmoothedTargets(true);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!isPrepared || !beginBypassProcess(buffer))
            return;

        const int numChannels = juce::jmin(2, buffer.getNumChannels());
        const int numSamples = buffer.getNumSamples();

        scrubInvalidSamples(buffer);

        if (!canProcessBlock(buffer))
        {
            fallbackBlockCount.fetch_add(1, std::memory_order_relaxed);
            endBypassProcess(buffer);
            return;
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::FloatVectorOperations::copy(scratchBuffer.getWritePointer(ch),
                buffer.getReadPointer(ch),
                numSamples);
        }

        syncSmoothedTargets(false);

        constexpr float halfPi = juce::MathConstants<float>::halfPi;

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            const auto motionMode = getMotionMode();
            const float manualSweep = sweepSmooth.getNextValue();
            const float sensitivity = sensSmooth.getNextValue();
            const float attackMs = attackSmooth.getNextValue();
            const float decayMs = decaySmooth.getNextValue();
            const float range = rangeSmooth.getNextValue();
            const float resonance = resonanceSmooth.getNextValue();
            const float voice = voiceSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            float detector = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float raw = scratchBuffer.getSample(ch, sampleIndex);
                const float focused = detectorBandPass[(size_t) ch].process(raw);
                const float detectorSample = std::abs(raw) * 0.52f + std::abs(focused) * 1.85f;
                detector = juce::jmax(detector, detectorSample);
            }

            const float attackCoeff = std::exp(-1.0f / (juce::jmax(0.1f, attackMs) * 0.001f * (float) sr));
            const float decayCoeff = std::exp(-1.0f / (juce::jmax(1.0f, decayMs) * 0.001f * (float) sr));

            if (detector > envelope)
                envelope = detector + attackCoeff * (envelope - detector);
            else
                envelope = detector + decayCoeff * (envelope - detector);

            const float envelopeSmoothCoeff = std::exp(-1.0f / (0.0035f * (float) sr));
            envelopeSmoothed += (1.0f - envelopeSmoothCoeff) * (envelope - envelopeSmoothed);

            const float detectorThreshold = juce::jmap(sensitivity, 0.030f, 0.013f);
            const float detectorExcite = juce::jmax(0.0f, envelopeSmoothed - detectorThreshold);
            const float detectorDrive = juce::jmap(sensitivity, 1.6f, 4.2f) * (1.0f + range * 0.18f);
            const float envelopeCurve = juce::jmap(voice, 1.24f, 1.02f);
            const float touchOpen = juce::jlimit(0.0f, 1.0f, detectorExcite * detectorDrive * 5.0f);
            const float touchPush = juce::jlimit(0.0f, 1.0f, (touchOpen - 0.20f) / 0.80f);
            const float accentThreshold = detectorThreshold + juce::jmap(sensitivity, 0.016f, 0.010f);
            const float accentHeadroom = juce::jmap(range, 0.060f, 0.035f);
            const float touchAccent = juce::jlimit(0.0f, 1.0f, (envelopeSmoothed - accentThreshold) / accentHeadroom);
            const float touchSweep = juce::jlimit(0.0f,
                1.0f,
                std::pow(touchOpen, envelopeCurve) * 0.72f + std::pow(touchPush, 0.85f) * 0.28f);
            const float touchFloor = juce::jmap(voice, 0.0f, 0.02f);
            const float touchBaseTravel = juce::jmap(range, 0.18f, 0.38f) * juce::jmap(sensitivity, 0.44f, 0.80f);
            const float touchAccentTravel = juce::jmap(range, 0.18f, 0.36f) * std::pow(touchAccent, 0.72f);
            const float touchMappedSweep = juce::jlimit(0.0f,
                1.0f,
                touchFloor + touchSweep * touchBaseTravel + touchAccentTravel);

            float effectiveSweep = manualSweep;
            switch (motionMode)
            {
                case MotionMode::Touch:
                    effectiveSweep = touchMappedSweep;
                    break;

                case MotionMode::Hybrid:
                {
                    effectiveSweep = juce::jlimit(0.0f, 1.0f, manualSweep * 0.62f + touchMappedSweep * 0.48f);
                    break;
                }

                case MotionMode::Pedal:
                default:
                    break;
            }

            const float minFreq = juce::jmap(voice, 280.0f, 440.0f);
            const float pedalMaxFreq = juce::jmap(voice, 1750.0f, 2550.0f) + range * 1450.0f;
            const float touchAccentFreq = touchAccent * (220.0f + range * 420.0f);
            const float touchMaxFreq = juce::jmap(voice, 1260.0f, 1980.0f) + range * 920.0f + touchAccentFreq;
            const float maxFreq = motionMode == MotionMode::Pedal ? pedalMaxFreq : touchMaxFreq;
            const float freq = juce::jlimit(90.0f,
                (float) (sr * 0.40),
                std::exp(std::log(minFreq) + effectiveSweep * (std::log(maxFreq) - std::log(minFreq))));

            const float touchLift = motionMode == MotionMode::Pedal ? 0.0f : touchSweep * 0.16f + touchAccent * 0.40f;
            const float dynamicQ = juce::jmax(0.55f,
                resonance * (0.76f + effectiveSweep * 0.28f + touchLift * 0.18f));
            const float k = 1.0f / dynamicQ;
            const float g = std::tan(piOverSr * freq);

            const float bodyBlend = juce::jlimit(0.06f,
                0.38f,
                juce::jmap(voice, 0.33f, 0.12f) + (1.0f - touchSweep) * 0.05f - touchAccent * 0.14f);
            const float gainComp = 1.0f / (1.0f + juce::jmax(0.0f, resonance - 1.0f) * 0.10f);

            currentFreq = freq;
            currentSweep = effectiveSweep;
            currentManualSweep = manualSweep;
            currentEnvelope = touchSweep;

            const float dryGain = std::cos(mix * halfPi);
            const float wetGain = std::sin(mix * halfPi);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = scratchBuffer.getSample(ch, sampleIndex);
                const float input = buffer.getSample(ch, sampleIndex);

                const float feedbackDrive = svf[(size_t) ch].ic1eq * (0.08f + resonance * 0.02f);
                const float saturatedInput = Nova::WahDSP::jfetSaturate(input * 1.75f + feedbackDrive) * 0.58f;

                const auto stage1 = svf[(size_t) ch].process(saturatedInput, g, k);
                const auto stage2 = svfCascade[(size_t) ch].process(stage1.bp, g, k * 2.2f);

                const float vocalPeak = stage1.bp * dynamicQ * (0.42f + touchAccent * 0.34f);
                const float body = stage2.lp * 1.55f;
                float wet = Nova::WahDSP::blend(vocalPeak, body, bodyBlend);

                if (motionMode != MotionMode::Pedal)
                    wet = Nova::WahDSP::blend(wet,
                        stage1.bp * dynamicQ * (0.48f + touchAccent * 0.24f),
                        juce::jlimit(0.0f, 1.0f, touchSweep * 0.14f + touchAccent * 0.44f));

                wet = Nova::WahDSP::smoothClip(wet * 0.95f) * gainComp * 1.08f;
                wet = outputDcBlock[(size_t) ch].process(wet);
                buffer.setSample(ch, sampleIndex, dry * dryGain + wet * wetGain);
            }
        }

        endBypassProcess(buffer);
    }

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        juce::XmlElement xml("WAH_STATE");

        auto appendParam = [&xml](juce::AudioProcessorParameterWithID* param)
        {
            if (param == nullptr)
                return;

            auto* child = xml.createNewChildElement("PARAM");
            child->setAttribute("id", param->paramID);
            child->setAttribute("value", param->getValue());
        };

        for (auto* param : getParameters())
            appendParam(dynamic_cast<juce::AudioProcessorParameterWithID*>(param));

        xml.setAttribute("controlSource", (int) getExternalControlSource());
        xml.setAttribute("shortcutKeyCode", getShortcutKeyCode());
        copyXmlToBinary(xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        const auto xmlState = std::unique_ptr<juce::XmlElement>(getXmlFromBinary(data, sizeInBytes));
        if (xmlState == nullptr)
            return;

        const bool isModernState = xmlState->hasTagName("WAH_STATE");
        const bool isLegacyState = xmlState->hasTagName("PLUGIN_STATE");
        if (!isModernState && !isLegacyState)
            return;

        bool sawMode = false;
        bool sawLegacyTouchMotion = false;
        bool sawLegacyAutoMotion = false;

        for (auto* child : xmlState->getChildIterator())
        {
            if (!child->hasTagName("PARAM"))
                continue;

            const auto paramID = child->getStringAttribute("id");
            const float value = (float) child->getDoubleAttribute("value");
            const auto remapNormalisedRange = [](float normalised,
                float sourceMin,
                float sourceMax,
                float targetMin,
                float targetMax)
            {
                const float rawValue = Nova::WahDSP::denormaliseValue(normalised, sourceMin, sourceMax);
                return Nova::WahDSP::normaliseRaw(rawValue, targetMin, targetMax);
            };

            if (paramID == "wahMode")
            {
                applyNormalisedValue(modeParam, value);
                sawMode = true;
                continue;
            }

            if (paramID == "wahSweep")
            {
                applyNormalisedValue(sweepParam, value);
                continue;
            }

            if (paramID == "wahSens")
            {
                applyNormalisedValue(sensitivityParam, value);
                sawLegacyTouchMotion = true;
                continue;
            }

            if (paramID == "wahAttack")
            {
                applyNormalisedValue(attackParam, value);
                sawLegacyTouchMotion = true;
                continue;
            }

            if (paramID == "wahDecay")
            {
                applyNormalisedValue(decayParam, value);
                sawLegacyTouchMotion = true;
                continue;
            }

            if (paramID == "wahRange")
            {
                applyNormalisedValue(rangeParam, value);
                sawLegacyTouchMotion = true;
                continue;
            }

            if (paramID == "wahManualRange")
            {
                applyNormalisedValue(rangeParam, value);
                continue;
            }

            if (paramID == "wahResonance")
            {
                applyNormalisedValue(resonanceParam, value);
                sawLegacyTouchMotion = true;
                continue;
            }

            if (paramID == "wahManualResonance")
            {
                const float rawValue = Nova::WahDSP::denormaliseValue(value, 0.5f, 8.0f);
                const float newNormalised = Nova::WahDSP::normaliseRaw(rawValue, 0.5f, 10.0f);
                applyNormalisedValue(resonanceParam, newNormalised);
                continue;
            }

            if (paramID == "wahVoice")
            {
                applyNormalisedValue(voiceParam, value);
                continue;
            }

            if (paramID == "wahMix")
            {
                applyNormalisedValue(mixParam, value);
                sawLegacyTouchMotion = true;
                continue;
            }

            if (paramID == "wahManualMix")
            {
                applyNormalisedValue(mixParam, value);
                continue;
            }

            if (paramID == "autoWahSens")
            {
                applyNormalisedValue(sensitivityParam, value);
                sawLegacyAutoMotion = true;
                continue;
            }

            if (paramID == "autoWahAttack")
            {
                applyNormalisedValue(attackParam, value);
                sawLegacyAutoMotion = true;
                continue;
            }

            if (paramID == "autoWahRelease")
            {
                applyNormalisedValue(decayParam,
                    remapNormalisedRange(value, 15.0f, 900.0f, 10.0f, 800.0f));
                sawLegacyAutoMotion = true;
                continue;
            }

            if (paramID == "autoWahRange")
            {
                applyNormalisedValue(rangeParam, value);
                sawLegacyAutoMotion = true;
                continue;
            }

            if (paramID == "autoWahResonance")
            {
                applyNormalisedValue(resonanceParam,
                    remapNormalisedRange(value, 0.6f, 9.0f, 0.5f, 10.0f));
                sawLegacyAutoMotion = true;
                continue;
            }

            if (paramID == "autoWahVoice")
            {
                applyNormalisedValue(voiceParam, value);
                sawLegacyAutoMotion = true;
                continue;
            }

            if (paramID == "autoWahMix")
            {
                applyNormalisedValue(mixParam, value);
                sawLegacyAutoMotion = true;
                continue;
            }
        }

        if (!sawMode && sawLegacyTouchMotion && modeParam != nullptr)
            applyNormalisedValue(modeParam, 0.5f);
        else if (!sawMode && sawLegacyAutoMotion && modeParam != nullptr)
            applyNormalisedValue(modeParam, 0.5f);

        if (isModernState)
        {
            setExternalControlSource((ExternalControlSource) xmlState->getIntAttribute("controlSource",
                (int) ExternalControlSource::MouseWheel));
            setShortcutKeyCode(xmlState->getIntAttribute("shortcutKeyCode", juce::KeyPress::spaceKey));
        }
        else
        {
            setExternalControlSource(ExternalControlSource::MouseWheel);
            setShortcutKeyCode(juce::KeyPress::spaceKey);
        }

        syncSmoothedTargets(true);
    }

    void setExternalControlSource(ExternalControlSource source) noexcept
    {
        controlSource.store((int) source);
    }

    ExternalControlSource getExternalControlSource() const noexcept
    {
        return (ExternalControlSource) controlSource.load();
    }

    void setShortcutKeyCode(int keyCode) noexcept
    {
        shortcutKeyCode.store(keyCode);
    }

    int getShortcutKeyCode() const noexcept
    {
        return shortcutKeyCode.load();
    }

    void setSweepFromUI(float newSweep)
    {
        if (sweepParam == nullptr)
            return;

        sweepParam->setValueNotifyingHost(sweepParam->convertTo0to1(juce::jlimit(0.0f, 1.0f, newSweep)));
    }

    void nudgeSweep(float delta)
    {
        if (sweepParam == nullptr)
            return;

        setSweepFromUI(sweepParam->get() + delta);
    }

    MotionMode getMotionMode() const noexcept
    {
        const int index = modeParam != nullptr ? modeParam->getIndex() : 0;
        switch (index)
        {
            case 1: return MotionMode::Touch;
            case 2: return MotionMode::Hybrid;
            case 0:
            default: return MotionMode::Pedal;
        }
    }

    juce::AudioParameterChoice* modeParam = nullptr;
    juce::AudioParameterFloat* sweepParam = nullptr;
    juce::AudioParameterFloat* sensitivityParam = nullptr;
    juce::AudioParameterFloat* attackParam = nullptr;
    juce::AudioParameterFloat* decayParam = nullptr;
    juce::AudioParameterFloat* rangeParam = nullptr;
    juce::AudioParameterFloat* resonanceParam = nullptr;
    juce::AudioParameterFloat* voiceParam = nullptr;
    juce::AudioParameterFloat* mixParam = nullptr;

    float currentFreq = 400.0f;
    float currentSweep = 0.46f;
    float currentManualSweep = 0.46f;
    float currentEnvelope = 0.0f;

    int getRealtimeFallbackCount() const noexcept
    {
        return fallbackBlockCount.load(std::memory_order_relaxed);
    }

    void clearRealtimeFallbackCount() noexcept
    {
        fallbackBlockCount.store(0, std::memory_order_relaxed);
    }

protected:
    static void applyNormalisedValue(juce::RangedAudioParameter* parameter, float normalisedValue)
    {
        if (parameter != nullptr)
            parameter->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normalisedValue));
    }

private:
    static constexpr int kMaxProcessingChannels = 2;

    struct DcBlocker
    {
        void prepare(double sampleRate, float cutoffHz) noexcept
        {
            const auto safeSampleRate = juce::jmax(1.0, sampleRate);
            const auto safeCutoff = juce::jlimit(5.0f, (float) (safeSampleRate * 0.45), cutoffHz);
            pole = (float) std::exp(-juce::MathConstants<double>::twoPi * (double) safeCutoff / safeSampleRate);
            reset();
        }

        float process(float x) noexcept
        {
            const float y = x - x1 + pole * y1;
            x1 = x;
            y1 = y;
            return y;
        }

        void reset() noexcept
        {
            x1 = 0.0f;
            y1 = 0.0f;
        }

        float pole = 0.995f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    bool canProcessBlock(const juce::AudioBuffer<float>& buffer) const noexcept
    {
        return buffer.getNumSamples() <= preparedBlockSize
            && buffer.getNumChannels() <= preparedChannels
            && buffer.getNumChannels() <= kMaxProcessingChannels;
    }

    static void scrubInvalidSamples(juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (!std::isfinite(data[i]))
                    data[i] = 0.0f;
        }
    }

    void syncSmoothedTargets(bool snap)
    {
        auto sync = [snap](auto& smoother, juce::AudioParameterFloat* parameter, float fallback)
        {
            const float value = parameter != nullptr ? parameter->get() : fallback;
            if (snap)
                smoother.setCurrentAndTargetValue(value);
            else
                smoother.setTargetValue(value);
        };

        sync(sweepSmooth, sweepParam, 0.46f);
        sync(sensSmooth, sensitivityParam, 0.58f);
        sync(attackSmooth, attackParam, 2.0f);
        sync(decaySmooth, decayParam, 120.0f);
        sync(rangeSmooth, rangeParam, 0.76f);
        sync(resonanceSmooth, resonanceParam, 4.2f);
        sync(voiceSmooth, voiceParam, 0.36f);
        sync(mixSmooth, mixParam, 1.0f);
    }

    double sr = 44100.0;
    float piOverSr = juce::MathConstants<float>::pi / 44100.0f;

    std::array<Nova::WahDSP::Biquad, 2> detectorBandPass;
    std::array<Nova::WahDSP::SVF, 2> svf;
    std::array<Nova::WahDSP::SVF, 2> svfCascade;
    std::array<DcBlocker, 2> outputDcBlock;

    float envelope = 0.0f;
    float envelopeSmoothed = 0.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sweepSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sensSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> attackSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decaySmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rangeSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> resonanceSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> voiceSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    std::atomic<int> controlSource { (int) ExternalControlSource::MouseWheel };
    std::atomic<int> shortcutKeyCode { juce::KeyPress::spaceKey };
    std::atomic<int> fallbackBlockCount{ 0 };

    juce::AudioBuffer<float> scratchBuffer;
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    bool isPrepared = false;
};

#include "ClassicWahEditor.h"
