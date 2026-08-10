#pragma once

#include <JuceHeader.h>
#include "Scenarios.h"

class AmbientSynth final
{
public:
    explicit AmbientSynth(int layerStyle);

    void prepare(double sampleRate, int maximumBlockSize);
    void setPatch(const SynthPatch& newPatch);
    void setDelayLevel(float newLevel) noexcept;
    void allNotesOff();
    void render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                int startSample, int numSamples);

private:
    void updateEffectTargets(bool immediately);
    void processEffects(int numSamples);

    juce::Synthesiser synthesiser;
    SynthPatch patch;
    juce::AudioBuffer<float> renderBuffer;
    juce::AudioBuffer<float> delayBuffer;
    juce::Reverb reverb;
    juce::SmoothedValue<float> delaySamplesLeft;
    juce::SmoothedValue<float> delaySamplesRight;
    juce::SmoothedValue<float> delayFeedback;
    juce::SmoothedValue<float> delayMix;
    juce::SmoothedValue<float> delayLevel;
    float requestedDelayLevel = 1.0f;
    int delayWritePosition = 0;
    double currentSampleRate = 48000.0;
    bool prepared = false;
};
