#include "AmbientSynth.h"

#include <cmath>
#include <cstdint>

namespace
{
float polyBlep(float phase, float phaseIncrement)
{
    const auto increment = juce::jlimit(1.0e-6f, 0.49f,
                                        std::abs(phaseIncrement));
    if (phase < increment)
    {
        const auto position = phase / increment;
        return position + position - position * position - 1.0f;
    }

    if (phase > 1.0f - increment)
    {
        const auto position = (phase - 1.0f) / increment;
        return position * position + position + position + 1.0f;
    }

    return 0.0f;
}

float softProtect(float sample)
{
    constexpr auto threshold = 0.94f;
    constexpr auto headroom = 1.0f - threshold;
    const auto magnitude = std::abs(sample);
    if (magnitude <= threshold)
        return sample;

    const auto protectedMagnitude = threshold
        + headroom * std::tanh((magnitude - threshold) / headroom);
    return std::copysign(protectedMagnitude, sample);
}

float applyDrive(float sample, float drive)
{
    // A drive value of one is a true, bit-for-bit linear bypass. Above one,
    // only peaks enter a soft knee; quiet material is not continually passed
    // through a waveshaper.
    const auto amount = juce::jlimit(0.0f, 1.0f, (drive - 1.0f) / 0.75f);
    constexpr auto threshold = 0.72f;
    constexpr auto headroom = 1.0f - threshold;
    const auto magnitude = std::abs(sample);
    if (amount <= 0.0f || magnitude <= threshold)
        return sample;

    const auto knee = 1.0f + amount * 3.0f;
    const auto excess = (magnitude - threshold) / headroom;
    const auto shapedMagnitude = threshold
        + headroom * std::tanh(excess * knee) / knee;
    const auto shaped = std::copysign(shapedMagnitude, sample);
    return sample + amount * (shaped - sample);
}

void advancePhase(double& phase, double increment)
{
    const auto twoPi = juce::MathConstants<double>::twoPi;
    phase += increment;
    phase -= twoPi * std::floor(phase / twoPi);
}

class AmbientSound final : public juce::SynthesiserSound
{
public:
    bool appliesToNote(int) override    { return true; }
    bool appliesToChannel(int) override { return true; }
};

class AmbientVoice final : public juce::SynthesiserVoice
{
public:
    explicit AmbientVoice(bool shouldGlide) : glides(shouldGlide) {}

    bool canPlaySound(juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<AmbientSound*>(sound) != nullptr;
    }

    void setPatch(const SynthPatch& newPatch)
    {
        patch = newPatch;
        secondaryDetuneRatio = std::pow(2.0,
                                        static_cast<double>(patch.detuneCents) / 1200.0);
        const auto ratios = secondaryRatios(patch.model);
        secondaryRatioA = ratios[0];
        secondaryRatioB = ratios[1];
        updateEnvelope();
        if (currentMidiNote >= 0)
            targetFrequency = noteFrequency(currentMidiNote);
    }

    void startNote(int midiNoteNumber, float velocity,
                   juce::SynthesiserSound*, int pitchWheelPosition) override
    {
        const auto wasActive = envelope.isActive();
        currentMidiNote = midiNoteNumber;
        pitchBend = (static_cast<float>(pitchWheelPosition) - 8192.0f) / 8192.0f;
        targetFrequency = noteFrequency(midiNoteNumber);
        if (! wasActive || ! glides || currentFrequency <= 0.0)
        {
            currentFrequency = targetFrequency;
            phaseA = 0.0;
            phaseB = 0.0;
            phaseC = 0.0;
            triangleState = -1.0f;
        }

        lfoPhase = 0.0;
        filterStateA = filterStateB = 0.0f;
        velocityLevel = velocity * patch.level;
        noiseState ^= static_cast<uint32_t>(midiNoteNumber) * 2654435761u;
        updateEnvelope();
        envelope.noteOn();
    }

    void stopNote(float, bool allowTailOff) override
    {
        if (allowTailOff)
            envelope.noteOff();
        else
        {
            envelope.reset();
            currentMidiNote = -1;
            clearCurrentNote();
        }
    }

    void pitchWheelMoved(int value) override
    {
        pitchBend = (static_cast<float>(value) - 8192.0f) / 8192.0f;
    }

    void controllerMoved(int controller, int value) override
    {
        const auto normalised = static_cast<float>(value) / 127.0f;
        if (controller == 1)
            modulation = normalised;
        else if (controller == 74)
            brightness = normalised;
    }

    void aftertouchChanged(int value) override
    {
        pressure = static_cast<float>(value) / 127.0f;
    }

    void channelPressureChanged(int value) override
    {
        pressure = static_cast<float>(value) / 127.0f;
    }

    void setCurrentPlaybackSampleRate(double newRate) override
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate(newRate);
        if (newRate > 0.0)
            envelope.setSampleRate(newRate);
    }

    void renderNextBlock(juce::AudioBuffer<float>& output, int startSample,
                         int numSamples) override
    {
        if (! isVoiceActive() || getSampleRate() <= 0.0)
            return;

        const auto twoPi = juce::MathConstants<double>::twoPi;
        const auto pan = juce::jlimit(-0.95f, 0.95f, patch.pan);
        const auto leftGain = std::sqrt(0.5f * (1.0f - pan));
        const auto rightGain = std::sqrt(0.5f * (1.0f + pan));
        const auto glideCoefficient = glides
            ? 1.0 - std::exp(-1.0 / (0.055 * getSampleRate())) : 1.0;

        for (int offset = 0; offset < numSamples; ++offset)
        {
            currentFrequency += (targetFrequency - currentFrequency) * glideCoefficient;
            const auto lfo = static_cast<float>(std::sin(lfoPhase));
            const auto bendSemitones = pitchBend * 2.0f + lfo * modulation * 0.16f;
            const auto bendRatio = std::pow(2.0, static_cast<double>(bendSemitones) / 12.0);
            const auto baseFrequency = currentFrequency * bendRatio;
            const auto deltaA = twoPi * baseFrequency / getSampleRate();

            const auto a = oscillator(phaseA, deltaA, patch.model);
            const auto b = secondaryOscillator(baseFrequency, patch.model);
            const auto noise = nextNoise();
            auto tone = a * (1.0f - patch.harmonicMix)
                      + b * patch.harmonicMix + noise * patch.noiseMix;
            tone = applyDrive(tone, patch.drive);

            const auto trackedCutoff = patch.cutoffHz
                * std::pow(2.0f, (static_cast<float>(currentMidiNote - 60)
                                  * patch.keyTrack) / 12.0f)
                * (1.0f + brightness * 2.4f + pressure * 0.75f);
            const auto cutoff = juce::jlimit(45.0f,
                static_cast<float>(getSampleRate() * 0.44), trackedCutoff);
            const auto pole = std::exp(-juce::MathConstants<float>::twoPi
                                       * cutoff / static_cast<float>(getSampleRate()));
            filterStateA = (1.0f - pole) * tone + pole * filterStateA;
            filterStateB = (1.0f - pole) * filterStateA + pole * filterStateB;

            const auto movement = 1.0f - patch.lfoDepth
                + patch.lfoDepth * (0.5f + 0.5f * lfo);
            const auto expression = 0.84f + pressure * 0.28f;
            const auto sample = filterStateB * velocityLevel * movement
                              * expression * envelope.getNextSample();
            if (output.getNumChannels() > 0)
                output.addSample(0, startSample + offset, sample * leftGain);
            if (output.getNumChannels() > 1)
                output.addSample(1, startSample + offset, sample * rightGain);

            advancePhase(phaseA, deltaA);
            advancePhase(phaseB, deltaA * secondaryDetuneRatio * secondaryRatioA);
            if (secondaryRatioB > 0.0)
                advancePhase(phaseC, deltaA * secondaryDetuneRatio * secondaryRatioB);
            lfoPhase += twoPi * patch.lfoRateHz / getSampleRate();
            if (lfoPhase >= twoPi)
                lfoPhase -= twoPi;
        }

        if (! envelope.isActive())
        {
            currentMidiNote = -1;
            clearCurrentNote();
        }
    }

private:
    void updateEnvelope()
    {
        juce::ADSR::Parameters parameters;
        parameters.attack = juce::jmax(0.001f, patch.attackSeconds);
        parameters.decay = juce::jmax(0.001f, patch.decaySeconds);
        parameters.sustain = juce::jlimit(0.0f, 1.0f, patch.sustain);
        parameters.release = juce::jmax(0.005f, patch.releaseSeconds);
        envelope.setParameters(parameters);
    }

    double noteFrequency(int midiNote) const
    {
        const auto transposed = juce::jlimit(0, 127,
            midiNote + patch.transposeSemitones);
        return juce::MidiMessage::getMidiNoteInHertz(transposed);
    }

    float nextNoise()
    {
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        return static_cast<float>(static_cast<int32_t>(noiseState))
               / static_cast<float>(0x7fffffff);
    }

    float bandLimitedPulse(double phase, double phaseIncrement,
                           float pulseWidth) const
    {
        const auto normalisedPhase = static_cast<float>(
            phase / juce::MathConstants<double>::twoPi);
        const auto normalisedIncrement = static_cast<float>(
            phaseIncrement / juce::MathConstants<double>::twoPi);
        const auto width = juce::jlimit(0.08f, 0.92f, pulseWidth);
        auto pulse = normalisedPhase < width ? 1.0f : -1.0f;
        pulse += polyBlep(normalisedPhase, normalisedIncrement);
        auto fallingPhase = normalisedPhase - width;
        if (fallingPhase < 0.0f)
            fallingPhase += 1.0f;
        pulse -= polyBlep(fallingPhase, normalisedIncrement);
        return pulse;
    }

    float bandLimitedTriangle(double phase, double phaseIncrement)
    {
        const auto normalisedIncrement = static_cast<float>(
            phaseIncrement / juce::MathConstants<double>::twoPi);
        const auto square = bandLimitedPulse(phase, phaseIncrement, 0.5f);
        triangleState += square * 4.0f * normalisedIncrement;
        triangleState = juce::jlimit(-1.02f, 1.02f, triangleState);
        return juce::jlimit(-1.0f, 1.0f, triangleState);
    }

    float oscillator(double phase, double phaseIncrement, OscillatorModel model)
    {
        const auto sine = static_cast<float>(std::sin(phase));
        const auto triangle = [&]
        {
            return bandLimitedTriangle(phase, phaseIncrement);
        };
        switch (model)
        {
            case OscillatorModel::sub:   return sine * 0.88f + triangle() * 0.12f;
            case OscillatorModel::warm:  return sine * 0.62f + triangle() * 0.38f;
            case OscillatorModel::pluck: return triangle() * 0.82f + nextNoise() * 0.08f;
            case OscillatorModel::glass: return sine;
            case OscillatorModel::reed:
                return sine * 0.78f
                     + bandLimitedPulse(phase, phaseIncrement, 0.46f) * 0.22f;
            case OscillatorModel::cloud: return sine * 0.72f + nextNoise() * 0.16f;
            case OscillatorModel::pulse:
                return bandLimitedPulse(phase, phaseIncrement, patch.pulseWidth);
            case OscillatorModel::bell:  return sine;
            case OscillatorModel::air:   return sine * 0.52f + nextNoise() * 0.30f;
        }
        return sine;
    }

    static std::array<double, 2> secondaryRatios(OscillatorModel model)
    {
        switch (model)
        {
            case OscillatorModel::sub:   return { 0.5, 0.0 };
            case OscillatorModel::glass: return { 3.01, 0.0 };
            case OscillatorModel::bell:  return { 2.01, 3.99 };
            case OscillatorModel::reed:  return { 2.0, 0.0 };
            case OscillatorModel::pulse: return { 2.0, 0.0 };
            case OscillatorModel::warm:
            case OscillatorModel::pluck:
            case OscillatorModel::cloud:
            case OscillatorModel::air:   return { 1.5, 0.0 };
        }
        return { 1.0, 0.0 };
    }

    float partialGain(double frequency) const
    {
        const auto fadeStart = getSampleRate() * 0.40;
        const auto fadeEnd = getSampleRate() * 0.48;
        const auto absoluteFrequency = std::abs(frequency);
        if (absoluteFrequency <= fadeStart)
            return 1.0f;
        if (absoluteFrequency >= fadeEnd)
            return 0.0f;

        const auto position = static_cast<float>(
            (absoluteFrequency - fadeStart) / (fadeEnd - fadeStart));
        const auto smoothStep = position * position * (3.0f - 2.0f * position);
        return 1.0f - smoothStep;
    }

    float secondaryOscillator(double baseFrequency, OscillatorModel model) const
    {
        const auto firstGain = partialGain(
            baseFrequency * secondaryDetuneRatio * secondaryRatioA);
        if (model == OscillatorModel::bell)
        {
            const auto secondGain = partialGain(
                baseFrequency * secondaryDetuneRatio * secondaryRatioB);
            return static_cast<float>(std::sin(phaseB)) * 0.62f * firstGain
                 + static_cast<float>(std::sin(phaseC)) * 0.38f * secondGain;
        }

        return static_cast<float>(std::sin(phaseB)) * firstGain;
    }

    SynthPatch patch;
    const bool glides = false;
    int currentMidiNote = -1;
    double phaseA = 0.0;
    double phaseB = 0.0;
    double phaseC = 0.0;
    double lfoPhase = 0.0;
    double currentFrequency = 0.0;
    double targetFrequency = 0.0;
    float velocityLevel = 0.0f;
    float filterStateA = 0.0f;
    float filterStateB = 0.0f;
    float pitchBend = 0.0f;
    float modulation = 0.0f;
    float brightness = 0.0f;
    float pressure = 0.0f;
    float triangleState = -1.0f;
    double secondaryDetuneRatio = 1.0;
    double secondaryRatioA = 1.5;
    double secondaryRatioB = 0.0;
    uint32_t noiseState = 0x9e3779b9u;
    juce::ADSR envelope;
};

float readDelaySample(const juce::AudioBuffer<float>& buffer, int channel,
                      int writePosition, float delayInSamples)
{
    const auto size = buffer.getNumSamples();
    auto readPosition = static_cast<float>(writePosition) - delayInSamples;
    while (readPosition < 0.0f)
        readPosition += static_cast<float>(size);
    while (readPosition >= static_cast<float>(size))
        readPosition -= static_cast<float>(size);

    const auto indexA = static_cast<int>(readPosition);
    const auto indexB = (indexA + 1) % size;
    const auto fraction = readPosition - static_cast<float>(indexA);
    return juce::jmap(fraction, buffer.getSample(channel, indexA),
                      buffer.getSample(channel, indexB));
}
}

AmbientSynth::AmbientSynth(int layerStyle)
{
    synthesiser.addSound(new AmbientSound());
    const auto voiceCount = layerStyle == 0 ? 1 : 8;
    for (int voice = 0; voice < voiceCount; ++voice)
        synthesiser.addVoice(new AmbientVoice(layerStyle == 0));
    synthesiser.setNoteStealingEnabled(true);
}

void AmbientSynth::prepare(double sampleRate, int maximumBlockSize)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    synthesiser.setCurrentPlaybackSampleRate(currentSampleRate);
    renderBuffer.setSize(2, juce::jmax(8192, maximumBlockSize),
                         false, true, false);
    delayBuffer.setSize(2, static_cast<int>(std::ceil(currentSampleRate * 8.0)) + 2,
                        false, true, false);
    delayBuffer.clear();
    delayWritePosition = 0;
    reverb.setSampleRate(currentSampleRate);
    reverb.reset();
    prepared = true;
    updateEffectTargets(true);
}

void AmbientSynth::setPatch(const SynthPatch& newPatch)
{
    patch = newPatch;
    for (int index = 0; index < synthesiser.getNumVoices(); ++index)
        if (auto* voice = dynamic_cast<AmbientVoice*>(synthesiser.getVoice(index)))
            voice->setPatch(patch);
    updateEffectTargets(! prepared);
}

void AmbientSynth::updateEffectTargets(bool immediately)
{
    const auto resetValue = [this, immediately](juce::SmoothedValue<float>& value,
                                                 float target)
    {
        if (immediately)
            value.setCurrentAndTargetValue(target);
        else
        {
            value.reset(currentSampleRate, 0.75);
            value.setTargetValue(target);
        }
    };

    const auto baseDelay = patch.delayMilliseconds * 0.001f
                         * static_cast<float>(currentSampleRate);
    resetValue(delaySamplesLeft, juce::jmax(1.0f, baseDelay));
    resetValue(delaySamplesRight,
               juce::jmax(1.0f, baseDelay * patch.delaySpread));
    resetValue(delayFeedback, juce::jlimit(0.0f, 0.78f, patch.delayFeedback));
    resetValue(delayMix, juce::jlimit(0.0f, 0.75f, patch.delayMix));

    juce::Reverb::Parameters parameters;
    parameters.roomSize = juce::jlimit(0.0f, 1.0f, patch.reverbSize);
    parameters.damping = juce::jlimit(0.0f, 1.0f, patch.reverbDamping);
    parameters.wetLevel = juce::jlimit(0.0f, 0.65f, patch.reverbWet);
    parameters.dryLevel = 1.0f - parameters.wetLevel * 0.45f;
    parameters.width = 0.92f;
    parameters.freezeMode = 0.0f;
    reverb.setParameters(parameters);
}

void AmbientSynth::processEffects(int numSamples)
{
    if (delayBuffer.getNumSamples() < 4)
        return;

    auto* left = renderBuffer.getWritePointer(0);
    auto* right = renderBuffer.getWritePointer(1);
    const auto maximumDelay = static_cast<float>(delayBuffer.getNumSamples() - 2);
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto dryLeft = left[sample];
        const auto dryRight = right[sample];
        const auto delayLeft = juce::jlimit(1.0f, maximumDelay,
                                            delaySamplesLeft.getNextValue());
        const auto delayRight = juce::jlimit(1.0f, maximumDelay,
                                             delaySamplesRight.getNextValue());
        const auto wetLeft = readDelaySample(delayBuffer, 0, delayWritePosition,
                                             delayLeft);
        const auto wetRight = readDelaySample(delayBuffer, 1, delayWritePosition,
                                              delayRight);
        const auto feedback = delayFeedback.getNextValue();
        const auto mix = delayMix.getNextValue();
        delayBuffer.setSample(0, delayWritePosition, softProtect(
            dryLeft + wetLeft * feedback + wetRight * feedback * 0.12f));
        delayBuffer.setSample(1, delayWritePosition, softProtect(
            dryRight + wetRight * feedback + wetLeft * feedback * 0.12f));
        left[sample] = dryLeft * (1.0f - mix * 0.35f) + wetLeft * mix;
        right[sample] = dryRight * (1.0f - mix * 0.35f) + wetRight * mix;
        delayWritePosition = (delayWritePosition + 1) % delayBuffer.getNumSamples();
    }

    reverb.processStereo(left, right, numSamples);
}

void AmbientSynth::render(juce::AudioBuffer<float>& output,
                          const juce::MidiBuffer& midi,
                          int startSample, int numSamples)
{
    if (numSamples <= 0 || renderBuffer.getNumSamples() < numSamples)
        return;

    renderBuffer.clear(0, numSamples);
    synthesiser.renderNextBlock(renderBuffer, midi, 0, numSamples);
    processEffects(numSamples);

    const auto channels = juce::jmin(2, output.getNumChannels());
    for (int channel = 0; channel < channels; ++channel)
        output.addFrom(channel, startSample, renderBuffer, channel, 0, numSamples);
}
