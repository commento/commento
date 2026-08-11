#include "AmbientSynth.h"

#include <cmath>
#include <cstdint>

namespace
{
constexpr int sineTableSize = 4096;

// Twenty-four active ambient voices otherwise call libm several million
// times per second.  A linearly interpolated table is effectively exact for
// audio here (worst-case error is below 0.000001), fits in L1 cache, and is
// built before the realtime callback starts.
const std::array<float, sineTableSize + 1> sineTable = []
{
    std::array<float, sineTableSize + 1> table {};
    for (int index = 0; index <= sineTableSize; ++index)
        table[static_cast<std::size_t>(index)] = static_cast<float>(std::sin(
            juce::MathConstants<double>::twoPi
            * static_cast<double>(index) / static_cast<double>(sineTableSize)));
    return table;
}();

float fastSine(double phase) noexcept
{
    const auto position = phase
        * (static_cast<double>(sineTableSize)
           / juce::MathConstants<double>::twoPi);
    const auto index = juce::jlimit(
        0, sineTableSize - 1, static_cast<int>(position));
    const auto fraction = static_cast<float>(
        position - static_cast<double>(index));
    const auto first = sineTable[static_cast<std::size_t>(index)];
    const auto second = sineTable[static_cast<std::size_t>(index + 1)];
    return first + (second - first) * fraction;
}

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

float smoothMorph(float amount) noexcept
{
    const auto position = juce::jlimit(0.0f, 1.0f, amount);
    return position * position * (3.0f - 2.0f * position);
}

float linearMorph(float source, float target, float amount) noexcept
{
    return source + (target - source) * amount;
}

float logarithmicMorph(float source, float target, float amount) noexcept
{
    constexpr auto minimum = 1.0e-5f;
    if (source <= minimum || target <= minimum)
        return linearMorph(source, target, amount);

    return std::exp(linearMorph(std::log(source), std::log(target), amount));
}

float gainMorph(float source, float target, float amount) noexcept
{
    constexpr auto floorGain = 0.0001f;
    const auto sourceDb = juce::Decibels::gainToDecibels(
        juce::jmax(floorGain, source), -80.0f);
    const auto targetDb = juce::Decibels::gainToDecibels(
        juce::jmax(floorGain, target), -80.0f);
    return juce::Decibels::decibelsToGain(
        linearMorph(sourceDb, targetDb, amount), -80.0f);
}

SynthPatch interpolatePatch(const SynthPatch& source,
                            const SynthPatch& target,
                            float amount) noexcept
{
    SynthPatch result = target;

    // Model, transpose and the displayed name deliberately belong to the
    // destination. AmbientVoice only latches the structural fields at the
    // next note-on, while all values below move continuously.
    result.detuneCents = linearMorph(source.detuneCents,
                                     target.detuneCents, amount);
    result.attackSeconds = logarithmicMorph(source.attackSeconds,
                                             target.attackSeconds, amount);
    result.decaySeconds = logarithmicMorph(source.decaySeconds,
                                            target.decaySeconds, amount);
    result.sustain = linearMorph(source.sustain, target.sustain, amount);
    result.releaseSeconds = logarithmicMorph(source.releaseSeconds,
                                              target.releaseSeconds, amount);
    result.cutoffHz = logarithmicMorph(source.cutoffHz,
                                       target.cutoffHz, amount);
    result.keyTrack = linearMorph(source.keyTrack, target.keyTrack, amount);
    result.drive = linearMorph(source.drive, target.drive, amount);
    result.harmonicMix = linearMorph(source.harmonicMix,
                                     target.harmonicMix, amount);
    result.noiseMix = linearMorph(source.noiseMix, target.noiseMix, amount);
    result.pulseWidth = linearMorph(source.pulseWidth,
                                    target.pulseWidth, amount);
    result.lfoRateHz = logarithmicMorph(source.lfoRateHz,
                                        target.lfoRateHz, amount);
    result.lfoDepth = linearMorph(source.lfoDepth, target.lfoDepth, amount);
    result.pan = linearMorph(source.pan, target.pan, amount);
    result.level = gainMorph(source.level, target.level, amount);
    // Delay time itself is not read from this interpolated value. The effect
    // uses two fixed taps and crossfades them, avoiding Doppler pitch sweeps.
    // Keeping a representative value here makes an interrupted morph start
    // close to its current perceptual position.
    result.delayMilliseconds = linearMorph(source.delayMilliseconds,
                                            target.delayMilliseconds, amount);
    result.delaySpread = linearMorph(source.delaySpread,
                                     target.delaySpread, amount);
    result.delayFeedback = linearMorph(source.delayFeedback,
                                       target.delayFeedback, amount);
    result.delayMix = linearMorph(source.delayMix, target.delayMix, amount);
    result.reverbSize = linearMorph(source.reverbSize,
                                    target.reverbSize, amount);
    result.reverbDamping = linearMorph(source.reverbDamping,
                                       target.reverbDamping, amount);
    result.reverbWet = linearMorph(source.reverbWet,
                                   target.reverbWet, amount);
    return result;
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

    void setPatchImmediate(const SynthPatch& newPatch)
    {
        patch = newPatch;
        nextNotePatch = newPatch;
        activeModel = newPatch.model;
        activeTransposeSemitones = newPatch.transposeSemitones;
        secondaryDetuneRatio = std::pow(2.0,
                                        static_cast<double>(patch.detuneCents) / 1200.0);
        const auto ratios = secondaryRatios(activeModel);
        secondaryRatioA = ratios[0];
        secondaryRatioB = ratios[1];
        updateEnvelope(newPatch);
        if (currentMidiNote >= 0)
            targetFrequency = noteFrequency(currentMidiNote);
    }

    void setMorphPatch(const SynthPatch& continuousPatch,
                       const SynthPatch& destinationPatch)
    {
        patch = continuousPatch;
        nextNotePatch = continuousPatch;
        nextNotePatch.model = destinationPatch.model;
        nextNotePatch.transposeSemitones
            = destinationPatch.transposeSemitones;
        secondaryDetuneRatio = std::pow(
            2.0, static_cast<double>(patch.detuneCents) / 1200.0);
        // Do not touch activeModel, activeTransposeSemitones or the current
        // ADSR. Held notes keep their oscillator family, register and natural
        // envelope; the next note captures the destination structure and the
        // envelope values reached by the morph at that instant.
    }

    void startNote(int midiNoteNumber, float velocity,
                   juce::SynthesiserSound*, int pitchWheelPosition) override
    {
        // JUCE momentarily stops a voice before reusing it. This happens both
        // for mono-bass legato and when an ambient loop with long releases
        // fills its eight voices. Preserve bass state or bridge the ambient
        // output across that immediate restart instead of creating a reset
        // click.
        const auto continuingRestart = pendingVoiceRestart
            && envelope.isActive();
        pendingVoiceRestart = false;
        const auto previousModel = activeModel;
        const auto previousTranspose = activeTransposeSemitones;
        activeModel = nextNotePatch.model;
        activeTransposeSemitones = nextNotePatch.transposeSemitones;
        const auto ratios = secondaryRatios(activeModel);
        secondaryRatioA = ratios[0];
        secondaryRatioB = ratios[1];
        currentMidiNote = midiNoteNumber;
        pitchBend = (static_cast<float>(pitchWheelPosition) - 8192.0f) / 8192.0f;
        targetFrequency = noteFrequency(midiNoteNumber);
        const auto continuingLegato = continuingRestart && glides;
        const auto structureChanged = previousModel != activeModel
            || previousTranspose != activeTransposeSemitones;
        if (! continuingLegato || currentFrequency <= 0.0)
        {
            if (continuingRestart && ! glides)
            {
                // The old voice cannot keep rendering after JUCE reuses it,
                // but dropping its last non-zero sample creates a click. Carry
                // just that boundary value into a short residual ramp while
                // the new oscillator and envelope restart normally from zero.
                declickOffsetLeft = lastOutputLeft;
                declickOffsetRight = lastOutputRight;
                declickSamplesTotal = juce::jmax(32, static_cast<int>(
                    std::round(getSampleRate() * 0.001)));
                declickSamplesRemaining = declickSamplesTotal;
                envelope.reset();
            }
            else
            {
                declickOffsetLeft = declickOffsetRight = 0.0f;
                declickSamplesRemaining = declickSamplesTotal = 0;
            }
            currentFrequency = targetFrequency;
            phaseA = 0.0;
            phaseB = 0.0;
            phaseC = 0.0;
            triangleState = -1.0f;
            lfoPhase = 0.0;
            filterStateA = filterStateB = 0.0f;
        }
        else if (structureChanged)
        {
            // A monophonic bass can receive the first destination note while
            // its previous note is still legato. Bridge that structural
            // change without resetting its phase or envelope.
            declickOffsetLeft = lastOutputLeft;
            declickOffsetRight = lastOutputRight;
            declickSamplesTotal = juce::jmax(32, static_cast<int>(
                std::round(getSampleRate() * 0.005)));
            declickSamplesRemaining = declickSamplesTotal;
        }

        velocityLevel = velocity;
        noiseState ^= static_cast<uint32_t>(midiNoteNumber) * 2654435761u;
        updateEnvelope(nextNotePatch);
        envelope.noteOn();
    }

    void stopNote(float, bool allowTailOff) override
    {
        if (allowTailOff)
            envelope.noteOff();
        else
        {
            // Synthesiser::startVoice stops a voice with allowTailOff=false
            // immediately before reusing it. Keep its DSP state until the end
            // of this render call so startNote can perform a click-free
            // transition. A genuine all-notes-off is finalised by
            // finishRenderBlock below.
            pendingVoiceRestart = envelope.isActive();
            if (! pendingVoiceRestart)
                envelope.reset();
            currentMidiNote = -1;
            clearCurrentNote();
        }
    }

    void finishRenderBlock()
    {
        if (! pendingVoiceRestart)
            return;

        pendingVoiceRestart = false;
        envelope.reset();
        currentFrequency = 0.0;
        currentMidiNote = -1;
        lastOutputLeft = lastOutputRight = 0.0f;
        declickOffsetLeft = declickOffsetRight = 0.0f;
        declickSamplesRemaining = declickSamplesTotal = 0;
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

        // A Synthesiser voice is rendered in segments separated by MIDI
        // events, so note, pressure, brightness and pitch-bend are constant
        // for this whole call.  Keeping the tracked filter pole outside the
        // sample loop removes one pow and one exp per active voice/sample --
        // a material saving when three ambient loops occupy all 24 voices on
        // Raspberry Pi.
        const auto trackedCutoff = patch.cutoffHz
            * std::pow(2.0f, (static_cast<float>(currentMidiNote - 60)
                              * patch.keyTrack) / 12.0f)
            * (1.0f + brightness * 2.4f + pressure * 0.75f);
        const auto cutoff = juce::jlimit(45.0f,
            static_cast<float>(getSampleRate() * 0.44), trackedCutoff);
        const auto pole = std::exp(-juce::MathConstants<float>::twoPi
                                   * cutoff / static_cast<float>(getSampleRate()));
        const auto fixedBendRatio = std::exp2(
            static_cast<double>(pitchBend) / 6.0);
        constexpr auto semitoneToNaturalExponent = 0.057762265046662105;
        const auto vibratoExponentScale = static_cast<double>(modulation)
            * 0.16 * semitoneToNaturalExponent;

        for (int offset = 0; offset < numSamples; ++offset)
        {
            currentFrequency += (targetFrequency - currentFrequency) * glideCoefficient;
            const auto lfo = fastSine(lfoPhase);
            // Vibrato spans at most +/-0.16 semitone.  A third-order exp
            // polynomial over this tiny interval has sub-ppb error and avoids
            // an expensive transcendental call for every generated sample.
            const auto vibratoExponent = static_cast<double>(lfo)
                                       * vibratoExponentScale;
            const auto vibratoRatio = 1.0 + vibratoExponent
                * (1.0 + vibratoExponent
                   * (0.5 + vibratoExponent / 6.0));
            const auto bendRatio = fixedBendRatio * vibratoRatio;
            const auto baseFrequency = currentFrequency * bendRatio;
            const auto deltaA = twoPi * baseFrequency / getSampleRate();

            const auto a = oscillator(phaseA, deltaA, activeModel);
            const auto b = secondaryOscillator(baseFrequency, activeModel);
            const auto noise = nextNoise();
            auto tone = a * (1.0f - patch.harmonicMix)
                      + b * patch.harmonicMix + noise * patch.noiseMix;
            tone = applyDrive(tone, patch.drive);

            filterStateA = (1.0f - pole) * tone + pole * filterStateA;
            filterStateB = (1.0f - pole) * filterStateA + pole * filterStateB;

            const auto movement = 1.0f - patch.lfoDepth
                + patch.lfoDepth * (0.5f + 0.5f * lfo);
            const auto expression = 0.84f + pressure * 0.28f;
            const auto sample = filterStateB * velocityLevel * patch.level * movement
                              * expression * envelope.getNextSample();
            auto outputLeft = sample * leftGain;
            auto outputRight = sample * rightGain;
            if (declickSamplesRemaining > 0 && declickSamplesTotal > 0)
            {
                const auto residualGain = static_cast<float>(
                    declickSamplesRemaining)
                    / static_cast<float>(declickSamplesTotal);
                outputLeft = outputLeft * (1.0f - residualGain)
                           + declickOffsetLeft * residualGain;
                outputRight = outputRight * (1.0f - residualGain)
                            + declickOffsetRight * residualGain;
                --declickSamplesRemaining;
                if (declickSamplesRemaining == 0)
                    declickOffsetLeft = declickOffsetRight = 0.0f;
            }
            if (output.getNumChannels() > 0)
                output.addSample(0, startSample + offset, outputLeft);
            if (output.getNumChannels() > 1)
                output.addSample(1, startSample + offset, outputRight);
            lastOutputLeft = outputLeft;
            lastOutputRight = outputRight;

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
    void updateEnvelope(const SynthPatch& envelopePatch)
    {
        juce::ADSR::Parameters parameters;
        parameters.attack = juce::jmax(0.001f, envelopePatch.attackSeconds);
        parameters.decay = juce::jmax(0.001f, envelopePatch.decaySeconds);
        parameters.sustain = juce::jlimit(0.0f, 1.0f,
                                          envelopePatch.sustain);
        parameters.release = juce::jmax(0.005f,
                                         envelopePatch.releaseSeconds);
        envelope.setParameters(parameters);
    }

    double noteFrequency(int midiNote) const
    {
        const auto transposed = juce::jlimit(0, 127,
            midiNote + activeTransposeSemitones);
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
        const auto sine = fastSine(phase);
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
            case OscillatorModel::dualSquare:
                return bandLimitedPulse(phase, phaseIncrement, 0.5f);
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
            case OscillatorModel::dualSquare: return { 1.0, 0.0 };
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
        if (model == OscillatorModel::dualSquare)
        {
            const auto increment = juce::MathConstants<double>::twoPi
                * baseFrequency * secondaryDetuneRatio * secondaryRatioA
                / getSampleRate();
            return bandLimitedPulse(phaseB, increment, 0.5f) * firstGain;
        }
        if (model == OscillatorModel::bell)
        {
            const auto secondGain = partialGain(
                baseFrequency * secondaryDetuneRatio * secondaryRatioB);
            return fastSine(phaseB) * 0.62f * firstGain
                 + fastSine(phaseC) * 0.38f * secondGain;
        }

        return fastSine(phaseB) * firstGain;
    }

    SynthPatch patch;
    SynthPatch nextNotePatch;
    const bool glides = false;
    OscillatorModel activeModel = OscillatorModel::warm;
    int activeTransposeSemitones = 0;
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
    bool pendingVoiceRestart = false;
    float lastOutputLeft = 0.0f;
    float lastOutputRight = 0.0f;
    float declickOffsetLeft = 0.0f;
    float declickOffsetRight = 0.0f;
    int declickSamplesRemaining = 0;
    int declickSamplesTotal = 0;
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
    : processesAmbientEffects(layerStyle != 0)
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
    delayLevel.reset(currentSampleRate, 0.045);
    delayLevel.setCurrentAndTargetValue(requestedDelayLevel);
    freezeMix.reset(currentSampleRate, 0.025);
    freezeMix.setCurrentAndTargetValue(requestedFreeze ? 1.0f : 0.0f);
    reverb.setSampleRate(currentSampleRate);
    reverb.reset();
    morphSourcePatch = patch;
    morphTargetPatch = patch;
    morphElapsedSamples = 0;
    morphTotalSamples = 0;
    blockMorphStart = blockMorphEnd = 1.0f;
    morphActive = false;
    prepared = true;
    updateEffectTargets(true);
}

void AmbientSynth::setPatch(const SynthPatch& newPatch)
{
    patch = newPatch;
    morphSourcePatch = newPatch;
    morphTargetPatch = newPatch;
    morphElapsedSamples = 0;
    morphTotalSamples = 0;
    blockMorphStart = blockMorphEnd = 1.0f;
    morphActive = false;
    for (int index = 0; index < synthesiser.getNumVoices(); ++index)
        if (auto* voice = dynamic_cast<AmbientVoice*>(synthesiser.getVoice(index)))
            voice->setPatchImmediate(patch);
    updateEffectTargets(true);
}

void AmbientSynth::beginPatchMorph(const SynthPatch& targetPatch,
                                   double durationSeconds)
{
    if (! prepared || ! std::isfinite(durationSeconds)
        || durationSeconds <= 0.0)
    {
        setPatch(targetPatch);
        return;
    }

    morphSourcePatch = patch;
    morphTargetPatch = targetPatch;
    morphElapsedSamples = 0;
    morphTotalSamples = juce::jmax<int64_t>(
        1, static_cast<int64_t>(std::llround(
            juce::jmin(durationSeconds, 120.0) * currentSampleRate)));
    blockMorphStart = blockMorphEnd = 0.0f;
    morphActive = true;

    const auto sourceDelay = juce::jmax(
        1.0f, morphSourcePatch.delayMilliseconds * 0.001f
              * static_cast<float>(currentSampleRate));
    const auto targetDelay = juce::jmax(
        1.0f, morphTargetPatch.delayMilliseconds * 0.001f
              * static_cast<float>(currentSampleRate));
    delayMorphSourceSamples = {
        sourceDelay,
        sourceDelay * morphSourcePatch.delaySpread
    };
    delayMorphTargetSamples = {
        targetDelay,
        targetDelay * morphTargetPatch.delaySpread
    };

    // Destination structural fields are available immediately, but a voice
    // captures them only at its next note-on. Held notes remain undisturbed.
    applyMorphPatchToVoices();
}

void AmbientSynth::setDelayLevel(float newLevel) noexcept
{
    requestedDelayLevel = juce::jlimit(0.0f, 1.0f, newLevel);
    if (std::abs(delayLevel.getTargetValue() - requestedDelayLevel) > 0.0001f)
        delayLevel.setTargetValue(requestedDelayLevel);
}

void AmbientSynth::setFreezeEnabled(bool shouldFreeze) noexcept
{
    if (requestedFreeze == shouldFreeze)
        return;

    requestedFreeze = shouldFreeze;
    freezeMix.setTargetValue(requestedFreeze ? 1.0f : 0.0f);
    if (prepared)
        updateReverbParameters();
}

void AmbientSynth::allNotesOff()
{
    synthesiser.allNotesOff(0, false);
}

void AmbientSynth::updateEffectTargets(bool immediately)
{
    juce::ignoreUnused(immediately);
    const auto baseDelay = patch.delayMilliseconds * 0.001f
                         * static_cast<float>(currentSampleRate);
    const auto leftDelay = juce::jmax(1.0f, baseDelay);
    const auto rightDelay = juce::jmax(1.0f,
                                       baseDelay * patch.delaySpread);
    delayMorphSourceSamples = { leftDelay, rightDelay };
    delayMorphTargetSamples = delayMorphSourceSamples;
    updateReverbParameters();
}

void AmbientSynth::prepareMorphBlock(int numSamples)
{
    if (! morphActive || morphTotalSamples <= 0)
    {
        blockMorphStart = blockMorphEnd = 1.0f;
        return;
    }

    const auto startPosition = static_cast<float>(morphElapsedSamples)
        / static_cast<float>(morphTotalSamples);
    const auto blockEndSamples = juce::jmin<int64_t>(
        morphTotalSamples, morphElapsedSamples + numSamples);
    const auto endPosition = static_cast<float>(blockEndSamples)
        / static_cast<float>(morphTotalSamples);
    blockMorphStart = smoothMorph(startPosition);
    blockMorphEnd = smoothMorph(endPosition);
    patch = interpolatePatch(morphSourcePatch, morphTargetPatch,
                             blockMorphEnd);
    morphElapsedSamples = blockEndSamples;
    applyMorphPatchToVoices();
    updateReverbParameters();
}

void AmbientSynth::finishMorphBlock()
{
    if (! morphActive || morphElapsedSamples < morphTotalSamples)
        return;

    patch = morphTargetPatch;
    morphSourcePatch = patch;
    morphTargetPatch = patch;
    delayMorphSourceSamples = delayMorphTargetSamples;
    delayMorphTargetSamples = delayMorphSourceSamples;
    morphElapsedSamples = 0;
    morphTotalSamples = 0;
    blockMorphStart = blockMorphEnd = 1.0f;
    morphActive = false;
    applyMorphPatchToVoices();
}

void AmbientSynth::applyMorphPatchToVoices()
{
    for (int index = 0; index < synthesiser.getNumVoices(); ++index)
        if (auto* voice = dynamic_cast<AmbientVoice*>(
                synthesiser.getVoice(index)))
            voice->setMorphPatch(patch, morphTargetPatch);
}

void AmbientSynth::updateReverbParameters()
{
    // This changes coefficients without resetting or reallocating the JUCE
    // reverb network, so the old tail is allowed to become the new one.

    juce::Reverb::Parameters parameters;
    parameters.roomSize = juce::jlimit(0.0f, 1.0f, patch.reverbSize);
    parameters.damping = juce::jlimit(0.0f, 1.0f, patch.reverbDamping);
    parameters.wetLevel = juce::jlimit(0.0f, 0.65f, patch.reverbWet);
    parameters.dryLevel = 1.0f - parameters.wetLevel * 0.45f;
    parameters.width = 0.92f;
    parameters.freezeMode = requestedFreeze ? 1.0f : 0.0f;
    reverb.setParameters(parameters);
}

void AmbientSynth::processEffects(int numSamples)
{
    if (delayBuffer.getNumSamples() < 4)
        return;

    auto* left = renderBuffer.getWritePointer(0);
    auto* right = renderBuffer.getWritePointer(1);
    const auto maximumDelay = static_cast<float>(delayBuffer.getNumSamples() - 2);
    const auto freezeStart = freezeMix.getCurrentValue();
    const auto freezeEnd = freezeMix.skip(numSamples);
    const auto freezeActive = juce::jmax(freezeStart, freezeEnd) > 0.00001f;
    const auto leftUsesTwoTaps = std::abs(delayMorphSourceSamples[0]
                                          - delayMorphTargetSamples[0]) > 0.5f;
    const auto rightUsesTwoTaps = std::abs(delayMorphSourceSamples[1]
                                           - delayMorphTargetSamples[1]) > 0.5f;
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto dryLeft = left[sample];
        const auto dryRight = right[sample];
        const auto fraction = numSamples > 1
            ? static_cast<float>(sample) / static_cast<float>(numSamples - 1)
            : 1.0f;
        const auto morph = linearMorph(blockMorphStart, blockMorphEnd,
                                        fraction);
        // The taps often contain correlated drones. A constant-sum crossfade
        // avoids the +3 dB midpoint of an equal-power curve and removes two
        // table lookups per sample during a morph.
        const auto sourceGain = 1.0f - morph;
        const auto targetGain = morph;
        const auto sourceLeft = readDelaySample(
            delayBuffer, 0, delayWritePosition,
            juce::jlimit(1.0f, maximumDelay, delayMorphSourceSamples[0]));
        const auto sourceRight = readDelaySample(
            delayBuffer, 1, delayWritePosition,
            juce::jlimit(1.0f, maximumDelay, delayMorphSourceSamples[1]));
        const auto wetLeft = leftUsesTwoTaps
            ? sourceLeft * sourceGain
                + readDelaySample(
                    delayBuffer, 0, delayWritePosition,
                    juce::jlimit(1.0f, maximumDelay,
                                 delayMorphTargetSamples[0])) * targetGain
            : sourceLeft;
        const auto wetRight = rightUsesTwoTaps
            ? sourceRight * sourceGain
                + readDelaySample(
                    delayBuffer, 1, delayWritePosition,
                    juce::jlimit(1.0f, maximumDelay,
                                 delayMorphTargetSamples[1])) * targetGain
            : sourceRight;
        const auto amount = delayLevel.getNextValue();
        const auto feedback = juce::jlimit(
            0.0f, 0.78f,
            linearMorph(morphSourcePatch.delayFeedback,
                        morphTargetPatch.delayFeedback, morph)) * amount;
        const auto mix = juce::jlimit(
            0.0f, 0.75f,
            linearMorph(morphSourcePatch.delayMix,
                        morphTargetPatch.delayMix, morph)) * amount;
        auto writeLeft = dryLeft + wetLeft * feedback
                       + wetRight * feedback * 0.12f;
        auto writeRight = dryRight + wetRight * feedback
                        + wetLeft * feedback * 0.12f;
        auto effectiveMix = mix;
        if (freezeActive)
        {
            const auto freeze = linearMorph(freezeStart, freezeEnd, fraction);
            // The ordinary cross-feedback path has a 1.12 common-mode gain,
            // which is safe at its normal 0.78 cap but not close to unity.
            // GELO therefore uses a normalised matrix and removes new input as
            // it enters. No buffer is copied or cleared in the callback.
            constexpr auto frozenFeedback = 0.995f;
            const auto frozenLeft = frozenFeedback
                * (wetLeft * 0.88f + wetRight * 0.12f);
            const auto frozenRight = frozenFeedback
                * (wetRight * 0.88f + wetLeft * 0.12f);
            writeLeft += (frozenLeft - writeLeft) * freeze;
            writeRight += (frozenRight - writeRight) * freeze;
            effectiveMix = juce::jmax(effectiveMix, freeze * 0.72f);
        }
        delayBuffer.setSample(0, delayWritePosition, softProtect(writeLeft));
        delayBuffer.setSample(1, delayWritePosition, softProtect(writeRight));
        left[sample] = dryLeft * (1.0f - effectiveMix * 0.35f)
                     + wetLeft * effectiveMix;
        right[sample] = dryRight * (1.0f - effectiveMix * 0.35f)
                      + wetRight * effectiveMix;
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

    prepareMorphBlock(numSamples);
    renderBuffer.clear(0, numSamples);
    synthesiser.renderNextBlock(renderBuffer, midi, 0, numSamples);
    for (int index = 0; index < synthesiser.getNumVoices(); ++index)
        if (auto* voice = dynamic_cast<AmbientVoice*>(
                synthesiser.getVoice(index)))
            voice->finishRenderBlock();
    if (processesAmbientEffects)
        processEffects(numSamples);
    else
    {
        // Every factory bass patch is deliberately dry and its UI delay is
        // disabled. JUCE Reverb's dry path used to multiply this signal by
        // two, so preserve that exact gain while skipping a complete stereo
        // delay plus 8 comb/4 all-pass filters per channel on every callback.
        renderBuffer.applyGain(0, numSamples, 2.0f);
    }
    finishMorphBlock();

    const auto channels = juce::jmin(2, output.getNumChannels());
    for (int channel = 0; channel < channels; ++channel)
        output.addFrom(channel, startSample, renderBuffer, channel, 0, numSamples);
}
