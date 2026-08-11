#include "SaxProcessor.h"
#include "FastSine.h"

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
    freezeMix.reset(sampleRate, 0.025);
    freezeMix.setCurrentAndTargetValue(requestedFreeze ? 1.0f : 0.0f);
    reverb.setSampleRate(sampleRate);
    prepared = true;
    resetTails();
    updateTargets(true);
}

void SaxProcessor::setPatch(const SaxPatch& newPatch)
{
    patch = newPatch;
    delayMorphActive = false;
    updateTargets(! prepared);
}

void SaxProcessor::beginPatchMorph(const SaxPatch& newPatch,
                                   double durationSeconds)
{
    if (! prepared || durationSeconds <= 0.0)
    {
        setPatch(newPatch);
        return;
    }

    const auto duration = juce::jlimit(0.05, 30.0, durationSeconds);
    const auto oldProgress = delayMorphActive
        ? delayMorphProgress.getCurrentValue() : 1.0f;
    delayMorphFromLeft = delayMorphActive
        ? juce::jmap(oldProgress, delayMorphFromLeft, delayMorphToLeft)
        : delaySamplesLeft.getCurrentValue();
    delayMorphFromRight = delayMorphActive
        ? juce::jmap(oldProgress, delayMorphFromRight, delayMorphToRight)
        : delaySamplesRight.getCurrentValue();

    const auto baseDelay = newPatch.delayMilliseconds * 0.001f
                         * static_cast<float>(sampleRate);
    delayMorphToLeft = juce::jmax(1.0f, baseDelay);
    delayMorphToRight = juce::jmax(1.0f, baseDelay * newPatch.delaySpread);
    delayMorphProgress.reset(sampleRate, duration);
    delayMorphProgress.setCurrentAndTargetValue(0.0f);
    delayMorphProgress.setTargetValue(1.0f);
    delayMorphActive = std::abs(delayMorphFromLeft - delayMorphToLeft) > 0.5f
                    || std::abs(delayMorphFromRight - delayMorphToRight) > 0.5f;

    patch = newPatch;
    updateTargets(false, duration);
    // The tap position is crossfaded explicitly below. Keeping the ordinary
    // smoothers at the destination prevents a second, Doppler-producing ramp
    // after the crossfade has finished.
    delaySamplesLeft.setCurrentAndTargetValue(delayMorphToLeft);
    delaySamplesRight.setCurrentAndTargetValue(delayMorphToRight);
}

void SaxProcessor::setDelayLevel(float newLevel) noexcept
{
    requestedDelayLevel = juce::jlimit(0.0f, 1.0f, newLevel);
    if (std::abs(delayLevel.getTargetValue() - requestedDelayLevel) > 0.0001f)
        delayLevel.setTargetValue(requestedDelayLevel);
}

void SaxProcessor::setFreezeEnabled(bool shouldFreeze) noexcept
{
    if (requestedFreeze == shouldFreeze)
        return;

    requestedFreeze = shouldFreeze;
    freezeMix.setTargetValue(requestedFreeze ? 1.0f : 0.0f);
}

void SaxProcessor::updateTargets(bool immediately, double transitionSeconds)
{
    const auto setTarget = [this, immediately, transitionSeconds](
                               juce::SmoothedValue<float>& value, float target)
    {
        if (immediately)
            value.setCurrentAndTargetValue(target);
        else
        {
            const auto current = value.getCurrentValue();
            value.reset(sampleRate, juce::jmax(0.001, transitionSeconds));
            value.setCurrentAndTargetValue(current);
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
    setTarget(modulationRateHz, juce::jmax(0.0f, patch.modulationRateHz));
    setTarget(tremoloDepth, juce::jlimit(0.0f, 0.65f, patch.tremoloDepth));
    setTarget(tremoloRateHz, juce::jmax(0.0f, patch.tremoloRateHz));
    setTarget(outputGain, juce::jlimit(0.1f, 0.8f, patch.outputGain));
    setTarget(reverbSize, juce::jlimit(0.0f, 1.0f, patch.reverbSize));
    setTarget(reverbDamping, juce::jlimit(0.0f, 1.0f, patch.reverbDamping));
    setTarget(reverbWet, juce::jlimit(0.0f, 0.7f, patch.reverbWet));
    updateReverbParameters(0);
}

void SaxProcessor::updateReverbParameters(int numSamples)
{
    juce::Reverb::Parameters parameters;
    parameters.roomSize = reverbSize.getCurrentValue();
    parameters.damping = reverbDamping.getCurrentValue();
    parameters.wetLevel = reverbWet.getCurrentValue();
    parameters.dryLevel = 1.0f - parameters.wetLevel * 0.38f;
    parameters.width = 1.0f;
    parameters.freezeMode = requestedFreeze ? 1.0f : 0.0f;
    reverb.setParameters(parameters);
    if (numSamples > 0)
    {
        reverbSize.skip(numSamples);
        reverbDamping.skip(numSamples);
        reverbWet.skip(numSamples);
    }
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
    updateReverbParameters(numSamples);
    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(1);
    const auto maximumDelay = static_cast<float>(delayBuffer.getNumSamples() - 2);
    const auto highPassPole = std::exp(-juce::MathConstants<float>::twoPi
                                       * 72.0f / static_cast<float>(sampleRate));
    const auto freezeStart = freezeMix.getCurrentValue();
    const auto freezeEnd = freezeMix.skip(numSamples);
    const auto freezeActive = juce::jmax(freezeStart, freezeEnd) > 0.00001f;

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

        const auto modulation = CommentoDsp::fastSine(modulationPhase)
                              * modulationDepthSamples.getNextValue();
        auto echoLeft = 0.0f;
        auto echoRight = 0.0f;
        if (delayMorphActive)
        {
            const auto progress = juce::jlimit(
                0.0f, 1.0f, delayMorphProgress.getNextValue());
            // Sax loops are strongly correlated between taps. Constant-sum
            // gains keep the midpoint from becoming a +3 dB transient.
            const auto oldGain = 1.0f - progress;
            const auto newGain = progress;
            const auto oldLeft = juce::jlimit(
                1.0f, maximumDelay, delayMorphFromLeft + modulation);
            const auto oldRight = juce::jlimit(
                1.0f, maximumDelay, delayMorphFromRight - modulation);
            const auto newLeft = juce::jlimit(
                1.0f, maximumDelay, delayMorphToLeft + modulation);
            const auto newRight = juce::jlimit(
                1.0f, maximumDelay, delayMorphToRight - modulation);
            echoLeft = readDelay(0, oldLeft) * oldGain
                     + readDelay(0, newLeft) * newGain;
            echoRight = readDelay(1, oldRight) * oldGain
                      + readDelay(1, newRight) * newGain;
            if (! delayMorphProgress.isSmoothing())
                delayMorphActive = false;
        }
        else
        {
            const auto leftDelay = juce::jlimit(1.0f, maximumDelay,
                delaySamplesLeft.getNextValue() + modulation);
            const auto rightDelay = juce::jlimit(1.0f, maximumDelay,
                delaySamplesRight.getNextValue() - modulation);
            echoLeft = readDelay(0, leftDelay);
            echoRight = readDelay(1, rightDelay);
        }
        const auto amount = delayLevel.getNextValue();
        const auto feedbackAmount = feedback.getNextValue() * amount;
        const auto crossAmount = crossFeedback.getNextValue();
        auto delayInputLeft = filtered[0] + feedbackAmount
            * (echoLeft * (1.0f - crossAmount) + echoRight * crossAmount);
        auto delayInputRight = filtered[1] + feedbackAmount
            * (echoRight * (1.0f - crossAmount) + echoLeft * crossAmount);
        auto wet = delayMix.getNextValue() * amount;
        if (freezeActive)
        {
            const auto fraction = numSamples > 1
                ? static_cast<float>(sample) / static_cast<float>(numSamples - 1)
                : 1.0f;
            const auto freeze = juce::jmap(fraction, freezeStart, freezeEnd);
            constexpr auto frozenFeedback = 0.995f;
            const auto frozenLeft = frozenFeedback
                * (echoLeft * (1.0f - crossAmount) + echoRight * crossAmount);
            const auto frozenRight = frozenFeedback
                * (echoRight * (1.0f - crossAmount) + echoLeft * crossAmount);
            delayInputLeft += (frozenLeft - delayInputLeft) * freeze;
            delayInputRight += (frozenRight - delayInputRight) * freeze;
            wet = juce::jmax(wet, freeze * 0.68f);
        }
        delayBuffer.setSample(0, writePosition,
            applyConditionalCeiling(delayInputLeft, 0.92f, 0.995f));
        delayBuffer.setSample(1, writePosition,
            applyConditionalCeiling(delayInputRight, 0.92f, 0.995f));

        const auto tremolo = tremoloDepth.getNextValue();
        const auto tremoloWave = CommentoDsp::fastSine(tremoloPhase);
        const auto leftMovement = 1.0f - tremolo
            + tremolo * (0.5f + 0.5f * tremoloWave);
        const auto rightMovement = 1.0f - tremolo
            + tremolo * (0.5f - 0.5f * tremoloWave);
        const auto gain = outputGain.getNextValue();
        left[sample] = (filtered[0] * (1.0f - wet * 0.28f) + echoLeft * wet)
                     * leftMovement * gain;
        right[sample] = (filtered[1] * (1.0f - wet * 0.28f) + echoRight * wet)
                      * rightMovement * gain;

        writePosition = (writePosition + 1) % delayBuffer.getNumSamples();
        modulationPhase += juce::MathConstants<double>::twoPi
                         * modulationRateHz.getNextValue() / sampleRate;
        tremoloPhase += juce::MathConstants<double>::twoPi
                      * tremoloRateHz.getNextValue() / sampleRate;
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

void SaxProcessor::advanceMorph(int numSamples) noexcept
{
    if (! prepared || numSamples <= 0)
        return;

    const auto advance = [numSamples](juce::SmoothedValue<float>& value)
    {
        value.skip(numSamples);
    };
    advance(toneCoefficient);
    advance(drive);
    advance(delaySamplesLeft);
    advance(delaySamplesRight);
    advance(feedback);
    advance(crossFeedback);
    advance(delayMix);
    advance(delayLevel);
    advance(freezeMix);
    advance(modulationDepthSamples);
    advance(modulationRateHz);
    advance(tremoloDepth);
    advance(tremoloRateHz);
    advance(outputGain);
    advance(reverbSize);
    advance(reverbDamping);
    advance(reverbWet);
    if (delayMorphActive)
    {
        delayMorphProgress.skip(numSamples);
        if (! delayMorphProgress.isSmoothing())
            delayMorphActive = false;
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
