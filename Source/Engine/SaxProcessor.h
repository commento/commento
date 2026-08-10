#pragma once

#include <JuceHeader.h>
#include "Scenarios.h"

class SaxProcessor final
{
public:
    void prepare(double sampleRate, int maximumBlockSize);
    void setPatch(const SaxPatch& newPatch);
    void setDelayLevel(float newLevel) noexcept;
    void process(juce::AudioBuffer<float>& buffer, int numSamples);
    void resetTails();
    void beginIncrementalTailReset() noexcept;
    [[nodiscard]] bool isIncrementalTailResetActive() const noexcept;

private:
    void updateTargets(bool immediately);
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
    juce::SmoothedValue<float> modulationDepthSamples;
    juce::SmoothedValue<float> tremoloDepth;
    juce::SmoothedValue<float> outputGain;
    std::array<float, 2> lowPassState {};
    std::array<float, 2> highPassState {};
    std::array<float, 2> previousInput {};
    double sampleRate = 48000.0;
    double modulationPhase = 0.0;
    double tremoloPhase = 0.0;
    int writePosition = 0;
    int incrementalClearPosition = -1;
    float requestedDelayLevel = 1.0f;
    bool prepared = false;
};
