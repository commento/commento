#pragma once

#include <JuceHeader.h>
#include "Scenarios.h"
#include <cstdint>

class AmbientSynth final
{
public:
    explicit AmbientSynth(int layerStyle);

    void prepare(double sampleRate, int maximumBlockSize);
    void setPatch(const SynthPatch& newPatch);
    void beginPatchMorph(const SynthPatch& targetPatch,
                         double durationSeconds);
    void setDelayLevel(float newLevel) noexcept;
    void setFreezeEnabled(bool shouldFreeze) noexcept;
    void allNotesOff();
    void render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                int startSample, int numSamples);

private:
    void updateEffectTargets(bool immediately);
    void prepareMorphBlock(int numSamples);
    void finishMorphBlock();
    void applyMorphPatchToVoices();
    void updateReverbParameters();
    void processEffects(int numSamples);

    juce::Synthesiser synthesiser;
    SynthPatch patch;
    juce::AudioBuffer<float> renderBuffer;
    juce::AudioBuffer<float> delayBuffer;
    juce::Reverb reverb;
    juce::SmoothedValue<float> delayLevel;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> freezeMix;
    SynthPatch morphSourcePatch;
    SynthPatch morphTargetPatch;
    std::array<float, 2> delayMorphSourceSamples { 1.0f, 1.0f };
    std::array<float, 2> delayMorphTargetSamples { 1.0f, 1.0f };
    int64_t morphElapsedSamples = 0;
    int64_t morphTotalSamples = 0;
    float blockMorphStart = 1.0f;
    float blockMorphEnd = 1.0f;
    float requestedDelayLevel = 1.0f;
    bool requestedFreeze = false;
    int delayWritePosition = 0;
    double currentSampleRate = 48000.0;
    bool processesAmbientEffects = true;
    bool morphActive = false;
    bool prepared = false;
};
