#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

// Five independent, attenuation-only performance trims. The UI writes only
// the atomics; smoothing and buffer changes stay on the audio thread.
class PerformanceLevels final
{
public:
    static constexpr int count = 5;

    PerformanceLevels() noexcept;

    void setTargetGain(int index, float linearGain) noexcept;
    [[nodiscard]] float getTargetGain(int index) const noexcept;

    void prepare(double sampleRate) noexcept;
    void process(int index, juce::AudioBuffer<float>& buffer,
                 int numSamples) noexcept;

private:
    std::array<std::atomic<float>, count> targetGains;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>,
               count> smoothedGains;
};
