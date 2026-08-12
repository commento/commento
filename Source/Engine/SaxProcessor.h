#pragma once

#include <JuceHeader.h>
#include "Scenarios.h"

struct CommentoFreezeRampProbe;

class SaxProcessor final
{
public:
    void prepare(double sampleRate, int maximumBlockSize);
    void setPatch(const SaxPatch& newPatch);
    void beginPatchMorph(const SaxPatch& newPatch, double durationSeconds);
    void setDelayLevel(float newLevel) noexcept;
    void setFreezeEnabled(bool shouldFreeze) noexcept;
    void setFreeTailEnabled(bool shouldReleaseTail) noexcept;
    void setDiveEnabled(bool shouldDive) noexcept;
    void setStutterEnabled(bool shouldStutter) noexcept;
    void process(juce::AudioBuffer<float>& buffer, int numSamples);
    void advanceMorph(int numSamples) noexcept;
    void resetTails();
    void beginIncrementalTailReset() noexcept;
    [[nodiscard]] bool isIncrementalTailResetActive() const noexcept;

private:
    friend struct CommentoFreezeRampProbe;

    void updateTargets(bool immediately, double transitionSeconds = 1.0);
    void updateReverbParameters(int numSamples);
    [[nodiscard]] float readDelay(int channel, float delayInSamples) const;

    SaxPatch patch;
    juce::AudioBuffer<float> delayBuffer;
    juce::Reverb reverb;
    juce::SmoothedValue<float> toneCoefficient;
    juce::SmoothedValue<float> drive;
    juce::SmoothedValue<float> delaySamplesLeft;
    juce::SmoothedValue<float> delaySamplesRight;
    juce::SmoothedValue<float> feedback;
    juce::SmoothedValue<float> crossFeedback;
    juce::SmoothedValue<float> delayMix;
    juce::SmoothedValue<float> delayLevel;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> freezeMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        excitationGain;
    // CADUTA lengthens the tap by a fixed number of samples rather than by a
    // ratio, so the interval of the fall is the same under every scenario
    // instead of following whatever delay time the patch happens to use.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> diveSamples;
    // SCATTO reads a second, short tap in parallel and crossfades into it, so
    // there is no glissando while the tap moves and no seam at the loop point:
    // under freeze the line simply recirculates that short window.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stutterMix;
    juce::SmoothedValue<float> modulationDepthSamples;
    juce::SmoothedValue<float> modulationRateHz;
    juce::SmoothedValue<float> tremoloDepth;
    juce::SmoothedValue<float> tremoloRateHz;
    juce::SmoothedValue<float> outputGain;
    juce::SmoothedValue<float> reverbSize;
    juce::SmoothedValue<float> reverbDamping;
    juce::SmoothedValue<float> reverbWet;
    juce::SmoothedValue<float> delayMorphProgress;
    std::array<float, 2> lowPassState {};
    std::array<float, 2> highPassState {};
    std::array<float, 2> previousInput {};
    double sampleRate = 48000.0;
    double modulationPhase = 0.0;
    double tremoloPhase = 0.0;
    int writePosition = 0;
    int incrementalClearPosition = -1;
    float delayMorphFromLeft = 1.0f;
    float delayMorphFromRight = 1.0f;
    float delayMorphToLeft = 1.0f;
    float delayMorphToRight = 1.0f;
    float requestedDelayLevel = 1.0f;
    bool requestedFreeze = false;
    bool requestedFreeTail = false;
    bool requestedDive = false;
    bool requestedStutter = false;
    bool delayMorphActive = false;
    bool prepared = false;
};
