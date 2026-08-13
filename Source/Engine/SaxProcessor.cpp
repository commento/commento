#include "SaxProcessor.h"
#include "FastSine.h"

#include <cstddef>
#include <cmath>

namespace
{
constexpr double freezeAttackSeconds = 0.080;
constexpr double freezeReleaseSeconds = 0.350;

// SCATTO. Short enough to read as a stutter rather than as a delay, long
// enough to hold a recognisable fragment of a phrase.
constexpr float stutterMilliseconds = 125.0f;
constexpr double stutterAttackSeconds = 0.012;
constexpr double stutterReleaseSeconds = 0.090;

// SCINTILLE. A short slice of the recent sax is replayed at 2x: an octave-
// tape gesture rather than a continuous pitch shifter. The source is low-pass
// filtered before decimation, voices have soft edges, and at most two can
// overlap, keeping both aliasing and Pi 5 cost bounded.
constexpr double sparkleBufferSeconds = 0.42;
constexpr double sparkleDurationSeconds = 0.24;
constexpr double sparkleAttackSeconds = 0.018;
constexpr double sparkleReleaseSeconds = 0.105;
constexpr double sparkleCooldownSeconds = 0.44;
constexpr double sparkleLookbackSeconds = 0.30;
constexpr float sparkleOnsetThreshold = 0.020f;
constexpr float sparkleMaximumGain = 0.64f;
constexpr float sparkleDryDuck = 0.24f;
constexpr double sparkleMinimumRetriggerSeconds = 0.085;

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
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    delayBuffer.setSize(2, static_cast<int>(std::ceil(sampleRate * 8.0)) + 2,
                        false, true, false);
    // Channel 2 stores only the onset envelope. It lets a pad pressed before
    // the player blows wait for genuinely audible history instead of
    // spending its one-shot trigger on a silent slice.
    sparkleBuffer.setSize(
        3, static_cast<int>(std::ceil(sampleRate * sparkleBufferSeconds)) + 8,
        false, true, false);
    transportDryBuffer.setSize(2, juce::jmax(1, maximumBlockSize),
                               false, true, false);
    delayLevel.reset(sampleRate, 0.045);
    delayLevel.setCurrentAndTargetValue(requestedDelayLevel);
    freezeMix.reset(sampleRate, freezeAttackSeconds);
    freezeMix.setCurrentAndTargetValue(requestedFreeze ? 1.0f : 0.0f);
    excitationGain.reset(sampleRate, 0.001);
    excitationGain.setCurrentAndTargetValue(requestedFreeTail ? 0.0f : 1.0f);
    stutterMix.reset(sampleRate, stutterAttackSeconds);
    stutterMix.setCurrentAndTargetValue(requestedStutter ? 1.0f : 0.0f);
    sparkleMix.reset(sampleRate, 0.018);
    sparkleMix.setCurrentAndTargetValue(requestedSparkleAmount);
    loopTransportFxMix.reset(sampleRate, 0.001);
    loopTransportFxMix.setCurrentAndTargetValue(
        loopTransportPlaying ? 1.0f : 0.0f);
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
    const auto current = freezeMix.getCurrentValue();
    freezeMix.reset(sampleRate,
                    requestedFreeze ? freezeAttackSeconds
                                    : freezeReleaseSeconds);
    freezeMix.setCurrentAndTargetValue(current);
    freezeMix.setTargetValue(requestedFreeze ? 1.0f : 0.0f);
}

void SaxProcessor::setFreeTailEnabled(bool shouldReleaseTail) noexcept
{
    if (requestedFreeTail == shouldReleaseTail)
        return;

    requestedFreeTail = shouldReleaseTail;
    const auto current = excitationGain.getCurrentValue();
    excitationGain.reset(sampleRate, requestedFreeTail ? 0.12 : 0.24);
    excitationGain.setCurrentAndTargetValue(current);
    excitationGain.setTargetValue(requestedFreeTail ? 0.0f : 1.0f);
}

void SaxProcessor::setStutterEnabled(bool shouldStutter) noexcept
{
    if (requestedStutter == shouldStutter)
        return;

    requestedStutter = shouldStutter;
    const auto current = stutterMix.getCurrentValue();
    stutterMix.reset(sampleRate, requestedStutter ? stutterAttackSeconds
                                                  : stutterReleaseSeconds);
    stutterMix.setCurrentAndTargetValue(current);
    stutterMix.setTargetValue(requestedStutter ? 1.0f : 0.0f);
}

void SaxProcessor::setSparkleAmount(float amount) noexcept
{
    const auto previousRequest = requestedSparkleAmount;
    requestedSparkleAmount = juce::jlimit(
        0.0f, 1.0f, std::isfinite(amount) ? amount : 0.0f);
    if (previousRequest <= 0.0001f && requestedSparkleAmount > 0.0001f)
        sparkleTriggerPending = true;
    else if (requestedSparkleAmount <= 0.0001f)
        sparkleTriggerPending = false;
    if (std::abs(requestedSparkleAmount - sparkleMix.getTargetValue())
        <= 0.0001f)
        return;

    const auto current = sparkleMix.getCurrentValue();
    sparkleMix.reset(sampleRate,
                     requestedSparkleAmount > current ? 0.018 : 0.55);
    sparkleMix.setCurrentAndTargetValue(current);
    sparkleMix.setTargetValue(requestedSparkleAmount);
}

void SaxProcessor::setLoopTransportPlaying(bool shouldPlay) noexcept
{
    if (loopTransportPlaying == shouldPlay
        && std::abs(loopTransportFxMix.getTargetValue()
                    - (shouldPlay ? 1.0f : 0.0f)) <= 0.0001f)
        return;

    loopTransportPlaying = shouldPlay;
    const auto current = loopTransportFxMix.getCurrentValue();
    // PAUSA must be obvious, but never a hard cut. PLAY reintroduces the
    // scene a little more slowly so a preserved tail cannot jump forward.
    loopTransportFxMix.reset(sampleRate, shouldPlay ? 0.180 : 0.100);
    loopTransportFxMix.setCurrentAndTargetValue(current);
    loopTransportFxMix.setTargetValue(shouldPlay ? 1.0f : 0.0f);
    if (shouldPlay && requestedSparkleAmount > 0.0001f)
        sparkleTriggerPending = true;
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
    // Keep GELO on the continuously-ramped delay matrix. JUCE's reverb
    // freezeMode is an on/off coefficient switch and would reintroduce a
    // hard boundary even though freezeMix itself is smooth.
    parameters.freezeMode = 0.0f;
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

float SaxProcessor::readSparkleSource(int channel,
                                      double position) const noexcept
{
    const auto size = sparkleBuffer.getNumSamples();
    if (size <= 1 || ! juce::isPositiveAndBelow(channel,
                                                 sparkleBuffer.getNumChannels()))
        return 0.0f;

    while (position < 0.0)
        position += static_cast<double>(size);
    while (position >= static_cast<double>(size))
        position -= static_cast<double>(size);
    const auto first = static_cast<int>(position);
    const auto second = (first + 1) % size;
    const auto fraction = static_cast<float>(position - first);
    return juce::jmap(fraction,
                      sparkleBuffer.getSample(channel, first),
                      sparkleBuffer.getSample(channel, second));
}

void SaxProcessor::process(juce::AudioBuffer<float>& buffer, int numSamples)
{
    if (! prepared || buffer.getNumChannels() < 2
        || delayBuffer.getNumSamples() < 4 || numSamples <= 0
        || transportDryBuffer.getNumSamples() < numSamples)
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
        excitationGain.skip(numSamples);
        stutterMix.skip(numSamples);
        sparkleMix.skip(numSamples);
        loopTransportFxMix.skip(numSamples);
        buffer.clear(0, numSamples);
        return;
    }

    juce::ScopedNoDenormals noDenormals;
    const auto excitationStart = excitationGain.getCurrentValue();
    const auto excitationEnd = excitationGain.skip(numSamples);
    if (juce::jmin(excitationStart, excitationEnd) < 0.99999f)
        for (int channel = 0; channel < 2; ++channel)
            buffer.applyGainRamp(channel, 0, numSamples,
                                 excitationStart, excitationEnd);
    updateReverbParameters(numSamples);
    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(1);
    const auto maximumDelay = static_cast<float>(delayBuffer.getNumSamples() - 2);
    const auto highPassPole = std::exp(-juce::MathConstants<float>::twoPi
                                       * 72.0f / static_cast<float>(sampleRate));
    const auto freezeStart = freezeMix.getCurrentValue();
    const auto freezeEnd = freezeMix.skip(numSamples);
    const auto stutterStart = stutterMix.getCurrentValue();
    const auto stutterEnd = stutterMix.skip(numSamples);
    const auto stutterActive = loopTransportPlaying
        && juce::jmax(stutterStart, stutterEnd) > 0.00001f;
    // SCATTO has to hold the line still for the short window to recirculate,
    // so it engages the same freeze GELO uses instead of owning a second one.
    const auto freezeActive = loopTransportPlaying
        && (juce::jmax(freezeStart, freezeEnd) > 0.00001f
            || stutterActive);
    const auto stutterTap = juce::jlimit(
        1.0f, maximumDelay,
        stutterMilliseconds * 0.001f * static_cast<float>(sampleRate));
    const auto sparkleBufferSize = sparkleBuffer.getNumSamples();
    const auto sparkleDuration = juce::jmax(
        8, static_cast<int>(std::round(sampleRate * sparkleDurationSeconds)));
    const auto sparkleAttack = juce::jmax(
        1, static_cast<int>(std::round(sampleRate * sparkleAttackSeconds)));
    const auto sparkleRelease = juce::jmax(
        1, static_cast<int>(std::round(sampleRate * sparkleReleaseSeconds)));
    const auto sparkleLookback = juce::jmin(
        sparkleBufferSize - 4,
        juce::jmax(4, static_cast<int>(std::round(
            sampleRate * sparkleLookbackSeconds))));
    const auto sparkleCooldown = juce::jmax(
        1, static_cast<int>(std::round(sampleRate * sparkleCooldownSeconds)));
    const auto sparkleMinimumRetrigger = juce::jmax(
        1, static_cast<int>(std::round(
            sampleRate * sparkleMinimumRetriggerSeconds)));
    // Four cascaded one-poles suppress material that would fold
    // below Nyquist when the sparkle head advances at 2x. The state follows
    // only the dry/pre-effect sax, never the generated octave. The slightly
    // higher cutoff keeps the octave bright without weakening alias rejection.
    const auto sparkleCutoff = juce::jmin(9000.0,
                                          sampleRate * 0.20);
    const auto sparkleLowPassCoefficient = static_cast<float>(1.0 - std::exp(
        -juce::MathConstants<double>::twoPi * sparkleCutoff / sampleRate));

    const auto startSparkleVoice = [&]() noexcept
    {
        auto& voice = sparkleVoices[static_cast<std::size_t>(
            sparkleVoiceCursor % static_cast<int>(sparkleVoices.size()))];
        sparkleVoiceCursor = (sparkleVoiceCursor + 1)
            % static_cast<int>(sparkleVoices.size());
        voice = {};
        voice.active = true;
        voice.durationSamples = sparkleDuration;
        voice.readPosition = static_cast<double>(
            sparkleWritePosition - sparkleLookback);
        while (voice.readPosition < 0.0)
            voice.readPosition += static_cast<double>(sparkleBufferSize);
        voice.gain = sparkleMaximumGain;
        const auto goesLeft = (sparkleVoiceCursor & 1) == 0;
        voice.panLeft = goesLeft ? 1.0f : 0.45f;
        voice.panRight = goesLeft ? 0.45f : 1.0f;
        // Model 12 input 7/8 is often a microphone connected to only one of
        // the two sockets. A fixed (L + R) * 0.5 downmix made that common
        // case another 6 dB quieter. Select the occupied side when one side
        // clearly dominates; use a centre-safe sum for genuine stereo.
        const auto leftEnergy = sparkleInputEnergy[0];
        const auto rightEnergy = sparkleInputEnergy[1];
        if (leftEnergy > rightEnergy * 4.0f)
        {
            voice.sourceLeftGain = 1.0f;
            voice.sourceRightGain = 0.0f;
        }
        else if (rightEnergy > leftEnergy * 4.0f)
        {
            voice.sourceLeftGain = 0.0f;
            voice.sourceRightGain = 1.0f;
        }
        else
        {
            voice.sourceLeftGain = 0.5f;
            voice.sourceRightGain = 0.5f;
        }
        sparkleTriggerCooldownSamples = sparkleCooldown;
    };

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const float source[2] { left[sample], right[sample] };
        auto inputMagnitude = 0.0f;
        if (sparkleBufferSize > 0)
        {
            for (int channel = 0; channel < 2; ++channel)
            {
                auto& poles = sparkleCaptureLowPass[
                    static_cast<std::size_t>(channel)];
                auto filteredSource = source[channel];
                for (auto& poleState : poles)
                {
                    poleState += sparkleLowPassCoefficient
                        * (filteredSource - poleState);
                    filteredSource = poleState;
                }
                sparkleBuffer.setSample(channel, sparkleWritePosition,
                                        filteredSource);
                constexpr auto energyFollow = 0.0005f;
                sparkleInputEnergy[static_cast<std::size_t>(channel)]
                    += energyFollow
                    * (source[channel] * source[channel]
                       - sparkleInputEnergy[static_cast<std::size_t>(channel)]);
                inputMagnitude = juce::jmax(inputMagnitude,
                                             std::abs(source[channel]));
            }
            sparkleValidSamples = juce::jmin(
                sparkleBufferSize, sparkleValidSamples + 1);
        }

        const auto sparkleAmount = juce::jlimit(
            0.0f, 1.0f, sparkleMix.getNextValue());
        const auto armed = loopTransportPlaying
            && requestedSparkleAmount > 0.0001f;
        constexpr auto onsetAttack = 0.15f;
        const auto onsetRelease = static_cast<float>(1.0 - std::exp(
            -1.0 / (0.060 * sampleRate)));
        sparkleOnsetEnvelope += (inputMagnitude - sparkleOnsetEnvelope)
            * (inputMagnitude > sparkleOnsetEnvelope
                ? onsetAttack : onsetRelease);
        if (sparkleBufferSize > 0)
            sparkleBuffer.setSample(2, sparkleWritePosition,
                                    sparkleOnsetEnvelope);
        const auto onset = sparkleOnsetEnvelope >= sparkleOnsetThreshold
            && sparklePreviousEnvelope < sparkleOnsetThreshold;
        // Preserve attacks that arrive while the previous particle is still
        // in its cooldown. The queued onset fires as soon as the voice budget
        // is available; it is not lost at a block or cooldown boundary.
        if (armed && onset)
            sparkleTriggerPending = true;
        if (sparkleTriggerCooldownSamples > 0)
            --sparkleTriggerCooldownSamples;
        const auto recentAttack = inputMagnitude
            > juce::jmax(sparkleOnsetThreshold * 1.35f,
                         sparkleOnsetEnvelope * 1.75f);
        auto historyReadPosition = static_cast<double>(
            sparkleWritePosition - sparkleLookback);
        if (historyReadPosition < 0.0)
            historyReadPosition += static_cast<double>(sparkleBufferSize);
        const auto hasAudibleMaterial = readSparkleSource(
            2, historyReadPosition) >= sparkleOnsetThreshold * 0.45f;
        if (armed && sparkleValidSamples >= sparkleLookback + 4
            && hasAudibleMaterial
            && ((sparkleTriggerCooldownSamples <= 0
                 && (sparkleTriggerPending || onset))
                || (recentAttack
                    && sparkleTriggerCooldownSamples
                        <= sparkleCooldown - sparkleMinimumRetrigger)))
        {
            startSparkleVoice();
            sparkleTriggerPending = false;
        }
        sparklePreviousEnvelope = sparkleOnsetEnvelope;

        float sparkleLeft = 0.0f;
        float sparkleRight = 0.0f;
        float sparklePresence = 0.0f;
        for (auto& voice : sparkleVoices)
        {
            if (! voice.active)
                continue;

            const auto remaining = voice.durationSamples - voice.ageSamples;
            auto envelope = juce::jmin(
                1.0f,
                static_cast<float>(voice.ageSamples) /
                    static_cast<float>(sparkleAttack));
            envelope = juce::jmin(
                envelope,
                static_cast<float>(remaining) /
                    static_cast<float>(sparkleRelease));
            envelope = juce::jlimit(0.0f, 1.0f, envelope);
            envelope = envelope * envelope * (3.0f - 2.0f * envelope);
            const auto mono
                = readSparkleSource(0, voice.readPosition)
                    * voice.sourceLeftGain
                + readSparkleSource(1, voice.readPosition)
                    * voice.sourceRightGain;
            const auto gain = voice.gain * sparkleAmount * envelope;
            sparkleLeft += mono * gain * voice.panLeft;
            sparkleRight += mono * gain * voice.panRight;
            sparklePresence = juce::jmax(sparklePresence,
                                         sparkleAmount * envelope);
            voice.readPosition += 2.0;
            while (voice.readPosition >= static_cast<double>(sparkleBufferSize))
                voice.readPosition -= static_cast<double>(sparkleBufferSize);
            if (++voice.ageSamples >= voice.durationSamples)
                voice.active = false;
        }

        if (sparkleBufferSize > 0)
            sparkleWritePosition = (sparkleWritePosition + 1)
                % sparkleBufferSize;

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

        const auto blockFraction = numSamples > 1
            ? static_cast<float>(sample) / static_cast<float>(numSamples - 1)
            : 1.0f;
        const auto stutter = stutterActive
            ? juce::jmap(blockFraction, stutterStart, stutterEnd) : 0.0f;
        if (stutterActive)
        {
            // Crossfading the tap here, before the feedback is formed, is what
            // makes the recirculation loop the short window rather than the
            // scenario's own tap, and it avoids the upward glissando a swept
            // tap would produce on the way in.
            echoLeft += (readDelay(0, stutterTap) - echoLeft) * stutter;
            echoRight += (readDelay(1, stutterTap) - echoRight) * stutter;
        }
        const auto amount = delayLevel.getNextValue();
        const auto feedbackAmount = feedback.getNextValue() * amount;
        const auto crossAmount = crossFeedback.getNextValue();
        auto delayInputLeft = filtered[0] + feedbackAmount
            * (echoLeft * (1.0f - crossAmount) + echoRight * crossAmount);
        auto delayInputRight = filtered[1] + feedbackAmount
            * (echoRight * (1.0f - crossAmount) + echoLeft * crossAmount);
        // A restrained send lets the octave fragment leave a trace in the
        // existing scenario delay without turning it into another feedback
        // voice. The direct sparkle below remains the clearly audible event.
        delayInputLeft += sparkleLeft * 0.55f;
        delayInputRight += sparkleRight * 0.55f;
        auto wet = delayMix.getNextValue() * amount;
        if (freezeActive)
        {
            const auto freeze = juce::jmax(
                juce::jmap(blockFraction, freezeStart, freezeEnd), stutter);
            constexpr auto frozenFeedback = 0.995f;
            const auto frozenLeft = frozenFeedback
                * (echoLeft * (1.0f - crossAmount) + echoRight * crossAmount);
            const auto frozenRight = frozenFeedback
                * (echoRight * (1.0f - crossAmount) + echoLeft * crossAmount);
            delayInputLeft += (frozenLeft - delayInputLeft) * freeze;
            delayInputRight += (frozenRight - delayInputRight) * freeze;
            wet = juce::jmax(wet, freeze * 0.68f);
        }
        // SCATTO lives entirely in the tail, so the live sax steps aside while
        // the captured fragment speaks.
        const auto tailFocus = stutter;
        wet = juce::jmax(wet, tailFocus * 0.92f);
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
        transportDryBuffer.setSample(0, sample,
                                     filtered[0] * leftMovement * gain);
        transportDryBuffer.setSample(1, sample,
                                     filtered[1] * rightMovement * gain);
        const auto dryGain = (1.0f - wet * 0.28f)
            * (1.0f - tailFocus * 0.94f)
            * (1.0f - sparklePresence * sparkleDryDuck);
        left[sample] = (filtered[0] * dryGain + echoLeft * wet)
                     * leftMovement * gain + sparkleLeft;
        right[sample] = (filtered[1] * dryGain + echoRight * wet)
                      * rightMovement * gain + sparkleRight;

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

    // Once the stored loop has faded to zero, PAUSA also withdraws the
    // shared delay/reverb/SCINTILLE tail that otherwise made it sound as if
    // RESPIRO were still playing. The current live sax remains available via
    // the already-filtered dry reference. DSP state keeps advancing, so PLAY
    // can return without resetting an eight-second delay in the audio thread.
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto effectMix = loopTransportFxMix.getNextValue();
        const auto dryLeft = transportDryBuffer.getSample(0, sample);
        const auto dryRight = transportDryBuffer.getSample(1, sample);
        left[sample] = dryLeft + effectMix * (left[sample] - dryLeft);
        right[sample] = dryRight + effectMix * (right[sample] - dryRight);
    }

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
    advance(excitationGain);
    advance(stutterMix);
    advance(sparkleMix);
    advance(loopTransportFxMix);
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
    sparkleBuffer.clear();
    reverb.reset();
    lowPassState.fill(0.0f);
    highPassState.fill(0.0f);
    previousInput.fill(0.0f);
    modulationPhase = 0.0;
    tremoloPhase = 0.0;
    writePosition = 0;
    sparkleWritePosition = 0;
    sparkleCaptureLowPass = {};
    sparkleInputEnergy = {};
    sparkleVoices = {};
    sparkleOnsetEnvelope = 0.0f;
    sparklePreviousEnvelope = 0.0f;
    sparkleTriggerCooldownSamples = 0;
    sparkleVoiceCursor = 0;
    sparkleValidSamples = 0;
    sparkleTriggerPending = false;
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
    sparkleWritePosition = 0;
    sparkleCaptureLowPass = {};
    sparkleInputEnergy = {};
    sparkleVoices = {};
    sparkleOnsetEnvelope = 0.0f;
    sparklePreviousEnvelope = 0.0f;
    sparkleTriggerCooldownSamples = 0;
    sparkleVoiceCursor = 0;
    sparkleValidSamples = 0;
    sparkleTriggerPending = false;
    incrementalClearPosition = 0;
}

bool SaxProcessor::isIncrementalTailResetActive() const noexcept
{
    return incrementalClearPosition >= 0;
}
