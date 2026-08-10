#include "SaxProcessor.h"

#include <cstddef>
#include <cmath>

namespace
{
// Exactly transparent below the knee.  The rational curve has unit slope at
// the knee and approaches the ceiling asymptotically, so it only intervenes
// when a feedback or reverb peak is genuinely close to full scale.
[[nodiscard]] float applyConditionalCeiling(float sample, float knee,
                                            float ceiling) noexcept
{
    if (! std::isfinite(sample))
        return 0.0f;

    const auto magnitude = std::abs(sample);
    if (magnitude <= knee)
        return sample;

    const auto headroom = ceiling - knee;
    const auto excess = magnitude - knee;
    const auto limitedMagnitude = knee
        + headroom * excess / (headroom + excess);
    return std::copysign(limitedMagnitude, sample);
}

// Drive values up to unity deliberately keep a completely linear signal path.
// Above unity, the saturated signal is gain-compensated and progressively
// crossfaded in.  This makes distortion a scenario colour rather than a
// permanent part of the sax path.
[[nodiscard]] float applyIntentionalDrive(float sample,
                                          float driveAmount) noexcept
{
    if (driveAmount <= 1.0f)
        return sample;

    const auto blend = juce::jlimit(0.0f, 1.0f, driveAmount - 1.0f);
    const auto saturated = std::tanh(sample * driveAmount) / driveAmount;
    return sample + blend * (saturated - sample);
}
}

void SaxProcessor::prepare(double newSampleRate, int maximumBlockSize)
{
    juce::ignoreUnused(maximumBlockSize);
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    delayBuffer.setSize(2, static_cast<int>(std::ceil(sampleRate * 8.0)) + 2,
                        false, true, false);
    delayLevel.reset(sampleRate, 0.045);
    delayLevel.setCurrentAndTargetValue(requestedDelayLevel);
    reverb.setSampleRate(sampleRate);
    prepared = true;
    resetTails();
    updateTargets(true);
}

void SaxProcessor::setPatch(const SaxPatch& newPatch)
{
    patch = newPatch;
    updateTargets(! prepared);
}

void SaxProcessor::setDelayLevel(float newLevel) noexcept
{
    requestedDelayLevel = juce::jlimit(0.0f, 1.0f, newLevel);
    if (std::abs(delayLevel.getTargetValue() - requestedDelayLevel) > 0.0001f)
        delayLevel.setTargetValue(requestedDelayLevel);
}

void SaxProcessor::updateTargets(bool immediately)
{
    const auto setTarget = [this, immediately](juce::SmoothedValue<float>& value,
                                                float target)
    {
        if (immediately)
            value.setCurrentAndTargetValue(target);
        else
        {
            value.reset(sampleRate, 1.0);
            value.setTargetValue(target);
        }
    };

    const auto cutoff = juce::jlimit(350.0f,
        static_cast<float>(sampleRate * 0.44), patch.toneHz);
    setTarget(toneCoefficient,
              std::exp(-juce::MathConstants<float>::twoPi * cutoff
                       / static_cast<float>(sampleRate)));
    setTarget(drive, juce::jlimit(0.55f, 2.0f, patch.drive));
    const auto baseDelay = patch.delayMilliseconds * 0.001f
                         * static_cast<float>(sampleRate);
    setTarget(delaySamplesLeft, juce::jmax(1.0f, baseDelay));
    setTarget(delaySamplesRight,
              juce::jmax(1.0f, baseDelay * patch.delaySpread));
    setTarget(feedback, juce::jlimit(0.0f, 0.68f, patch.feedback));
    setTarget(crossFeedback, juce::jlimit(0.0f, 0.92f, patch.crossFeedback));
    setTarget(delayMix, juce::jlimit(0.0f, 0.68f, patch.delayMix));
    setTarget(modulationDepthSamples,
              patch.modulationDepthMilliseconds * 0.001f
              * static_cast<float>(sampleRate));
    setTarget(tremoloDepth, juce::jlimit(0.0f, 0.65f, patch.tremoloDepth));
    setTarget(outputGain, juce::jlimit(0.1f, 0.8f, patch.outputGain));

    juce::Reverb::Parameters parameters;
    parameters.roomSize = juce::jlimit(0.0f, 1.0f, patch.reverbSize);
    parameters.damping = juce::jlimit(0.0f, 1.0f, patch.reverbDamping);
    parameters.wetLevel = juce::jlimit(0.0f, 0.7f, patch.reverbWet);
    parameters.dryLevel = 1.0f - parameters.wetLevel * 0.38f;
    parameters.width = 1.0f;
    parameters.freezeMode = 0.0f;
    reverb.setParameters(parameters);
}

float SaxProcessor::readDelay(int channel, float delayInSamples) const
{
    const auto size = delayBuffer.getNumSamples();
    auto readPosition = static_cast<float>(writePosition) - delayInSamples;
    while (readPosition < 0.0f)
        readPosition += static_cast<float>(size);
    while (readPosition >= static_cast<float>(size))
        readPosition -= static_cast<float>(size);
    const auto first = static_cast<int>(readPosition);
    const auto second = (first + 1) % size;
    const auto fraction = readPosition - static_cast<float>(first);
    return juce::jmap(fraction, delayBuffer.getSample(channel, first),
                      delayBuffer.getSample(channel, second));
}

void SaxProcessor::process(juce::AudioBuffer<float>& buffer, int numSamples)
{
    if (! prepared || buffer.getNumChannels() < 2
        || delayBuffer.getNumSamples() < 4 || numSamples <= 0)
        return;

    if (incrementalClearPosition >= 0)
    {
        // Clearing the complete eight-second delay in one audio callback can
        // create a realtime spike. Spread the work across a handful of
        // callbacks and keep this processor silent until the new generation
        // is ready.
        constexpr auto samplesPerClearPass = 32768;
        const auto samplesToClear = juce::jmin(
            samplesPerClearPass,
            delayBuffer.getNumSamples() - incrementalClearPosition);
        for (int channel = 0; channel < delayBuffer.getNumChannels(); ++channel)
            juce::FloatVectorOperations::clear(
                delayBuffer.getWritePointer(channel, incrementalClearPosition),
                samplesToClear);
        incrementalClearPosition += samplesToClear;
        if (incrementalClearPosition >= delayBuffer.getNumSamples())
            incrementalClearPosition = -1;
        buffer.clear(0, numSamples);
        return;
    }

    juce::ScopedNoDenormals noDenormals;
    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(1);
    const auto maximumDelay = static_cast<float>(delayBuffer.getNumSamples() - 2);
    const auto highPassPole = std::exp(-juce::MathConstants<float>::twoPi
                                       * 72.0f / static_cast<float>(sampleRate));

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const float source[2] { left[sample], right[sample] };
        float filtered[2] {};
        const auto pole = toneCoefficient.getNextValue();
        const auto saturation = drive.getNextValue();
        for (int channel = 0; channel < 2; ++channel)
        {
            highPassState[static_cast<std::size_t>(channel)] = highPassPole
                * (highPassState[static_cast<std::size_t>(channel)]
                   + source[channel]
                   - previousInput[static_cast<std::size_t>(channel)]);
            previousInput[static_cast<std::size_t>(channel)] = source[channel];
            lowPassState[static_cast<std::size_t>(channel)] = (1.0f - pole)
                * highPassState[static_cast<std::size_t>(channel)]
                + pole * lowPassState[static_cast<std::size_t>(channel)];
            filtered[channel] = applyIntentionalDrive(
                lowPassState[static_cast<std::size_t>(channel)], saturation);
        }

        const auto modulation = static_cast<float>(std::sin(modulationPhase))
                              * modulationDepthSamples.getNextValue();
        const auto leftDelay = juce::jlimit(1.0f, maximumDelay,
            delaySamplesLeft.getNextValue() + modulation);
        const auto rightDelay = juce::jlimit(1.0f, maximumDelay,
            delaySamplesRight.getNextValue() - modulation);
        const auto echoLeft = readDelay(0, leftDelay);
        const auto echoRight = readDelay(1, rightDelay);
        const auto amount = delayLevel.getNextValue();
        const auto feedbackAmount = feedback.getNextValue() * amount;
        const auto crossAmount = crossFeedback.getNextValue();
        const auto delayInputLeft = filtered[0] + feedbackAmount
            * (echoLeft * (1.0f - crossAmount) + echoRight * crossAmount);
        const auto delayInputRight = filtered[1] + feedbackAmount
            * (echoRight * (1.0f - crossAmount) + echoLeft * crossAmount);
        delayBuffer.setSample(0, writePosition,
            applyConditionalCeiling(delayInputLeft, 0.92f, 0.995f));
        delayBuffer.setSample(1, writePosition,
            applyConditionalCeiling(delayInputRight, 0.92f, 0.995f));

        const auto wet = delayMix.getNextValue() * amount;
        const auto tremolo = tremoloDepth.getNextValue();
        const auto leftMovement = 1.0f - tremolo
            + tremolo * (0.5f + 0.5f * static_cast<float>(std::sin(tremoloPhase)));
        const auto rightMovement = 1.0f - tremolo
            + tremolo * (0.5f - 0.5f * static_cast<float>(std::sin(tremoloPhase)));
        const auto gain = outputGain.getNextValue();
        left[sample] = (filtered[0] * (1.0f - wet * 0.28f) + echoLeft * wet)
                     * leftMovement * gain;
        right[sample] = (filtered[1] * (1.0f - wet * 0.28f) + echoRight * wet)
                      * rightMovement * gain;

        writePosition = (writePosition + 1) % delayBuffer.getNumSamples();
        modulationPhase += juce::MathConstants<double>::twoPi
                         * patch.modulationRateHz / sampleRate;
        tremoloPhase += juce::MathConstants<double>::twoPi
                      * patch.tremoloRateHz / sampleRate;
        if (modulationPhase >= juce::MathConstants<double>::twoPi)
            modulationPhase -= juce::MathConstants<double>::twoPi;
        if (tremoloPhase >= juce::MathConstants<double>::twoPi)
            tremoloPhase -= juce::MathConstants<double>::twoPi;
    }

    reverb.processStereo(left, right, numSamples);

    // Samples below the knee are copied bit-for-bit.  Only exceptional reverb
    // peaks are caught, replacing the old always-on tanh coloration.
    for (int channel = 0; channel < 2; ++channel)
    {
        auto* samples = buffer.getWritePointer(channel);
        for (int sample = 0; sample < numSamples; ++sample)
            samples[sample] = applyConditionalCeiling(samples[sample],
                                                       0.90f, 0.98f);
    }
}

void SaxProcessor::resetTails()
{
    delayBuffer.clear();
    reverb.reset();
    lowPassState.fill(0.0f);
    highPassState.fill(0.0f);
    previousInput.fill(0.0f);
    modulationPhase = 0.0;
    tremoloPhase = 0.0;
    writePosition = 0;
    incrementalClearPosition = -1;
}

void SaxProcessor::beginIncrementalTailReset() noexcept
{
    if (! prepared || delayBuffer.getNumSamples() <= 0)
        return;

    reverb.reset();
    lowPassState.fill(0.0f);
    highPassState.fill(0.0f);
    previousInput.fill(0.0f);
    modulationPhase = 0.0;
    tremoloPhase = 0.0;
    writePosition = 0;
    incrementalClearPosition = 0;
}

bool SaxProcessor::isIncrementalTailResetActive() const noexcept
{
    return incrementalClearPosition >= 0;
}
