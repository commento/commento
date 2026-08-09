#include "AmbientSynth.h"

#include <array>
#include <cmath>

namespace
{
class AmbientSound final : public juce::SynthesiserSound
{
public:
    bool appliesToNote(int) override    { return true; }
    bool appliesToChannel(int) override { return true; }
};

class AmbientVoice final : public juce::SynthesiserVoice
{
public:
    explicit AmbientVoice(int newStyle) : style(newStyle) {}

    bool canPlaySound(juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<AmbientSound*>(sound) != nullptr;
    }

    void startNote(int midiNoteNumber, float velocity,
                   juce::SynthesiserSound*, int) override
    {
        const auto frequency = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber);
        phaseA = phaseB = lfoPhase = 0.0;
        deltaA = juce::MathConstants<double>::twoPi * frequency / getSampleRate();
        const auto detune = std::array<double, 4> { 3.5, 7.0, -5.0, 11.0 }
            [static_cast<size_t>(style)];
        deltaB = deltaA * std::pow(2.0, detune / 1200.0);
        level = velocity * 0.16f;

        juce::ADSR::Parameters parameters;
        parameters.attack = std::array<float, 4> { 0.35f, 0.8f, 1.4f, 0.18f }
            [static_cast<size_t>(style)];
        parameters.decay = 1.8f;
        parameters.sustain = std::array<float, 4> { 0.82f, 0.72f, 0.90f, 0.68f }
            [static_cast<size_t>(style)];
        parameters.release = std::array<float, 4> { 4.0f, 6.5f, 8.0f, 3.2f }
            [static_cast<size_t>(style)];
        envelope.setParameters(parameters);
        envelope.noteOn();
    }

    void stopNote(float, bool allowTailOff) override
    {
        if (allowTailOff)
            envelope.noteOff();
        else
        {
            envelope.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void setCurrentPlaybackSampleRate(double newRate) override
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate(newRate);
        envelope.setSampleRate(newRate);
    }

    void renderNextBlock(juce::AudioBuffer<float>& output, int startSample,
                         int numSamples) override
    {
        if (! isVoiceActive())
            return;

        const auto pan = std::array<float, 4> { -0.32f, 0.24f, -0.12f, 0.38f }
            [static_cast<size_t>(style)];
        for (int offset = 0; offset < numSamples; ++offset)
        {
            const auto a = std::sin(phaseA);
            const auto b = std::sin(phaseB);
            double tone = 0.0;
            switch (style)
            {
                case 0: tone = a * 0.72 + b * 0.28; break;
                case 1: tone = a * 0.58 + std::sin(phaseB * 0.5) * 0.42; break;
                case 2: tone = a * 0.64 + std::sin(phaseB * 1.5) * 0.24
                                     + std::sin(phaseA * 0.25) * 0.12; break;
                default: tone = a * 0.54 + b * 0.24
                                       + std::sin(phaseA * 2.0) * 0.22; break;
            }

            const auto lfo = 0.84 + 0.16 * std::sin(lfoPhase);
            const auto sample = static_cast<float>(tone * lfo)
                                * level * envelope.getNextSample();
            if (output.getNumChannels() > 0)
                output.addSample(0, startSample + offset,
                                 sample * std::sqrt(0.5f * (1.0f - pan)));
            if (output.getNumChannels() > 1)
                output.addSample(1, startSample + offset,
                                 sample * std::sqrt(0.5f * (1.0f + pan)));

            phaseA = std::fmod(phaseA + deltaA, juce::MathConstants<double>::twoPi);
            phaseB = std::fmod(phaseB + deltaB, juce::MathConstants<double>::twoPi);
            lfoPhase = std::fmod(lfoPhase
                + juce::MathConstants<double>::twoPi * 0.07 / getSampleRate(),
                juce::MathConstants<double>::twoPi);
        }

        if (! envelope.isActive())
            clearCurrentNote();
    }

private:
    int style = 0;
    double phaseA = 0.0;
    double phaseB = 0.0;
    double lfoPhase = 0.0;
    double deltaA = 0.0;
    double deltaB = 0.0;
    float level = 0.0f;
    juce::ADSR envelope;
};
}

AmbientSynth::AmbientSynth(int layerStyle)
{
    synthesiser.addSound(new AmbientSound());
    for (int voice = 0; voice < 8; ++voice)
        synthesiser.addVoice(new AmbientVoice(layerStyle));
    synthesiser.setNoteStealingEnabled(true);
}

void AmbientSynth::prepare(double sampleRate)
{
    synthesiser.setCurrentPlaybackSampleRate(sampleRate);
}

void AmbientSynth::render(juce::AudioBuffer<float>& output,
                          const juce::MidiBuffer& midi,
                          int startSample, int numSamples)
{
    synthesiser.renderNextBlock(output, midi, startSample, numSamples);
}

