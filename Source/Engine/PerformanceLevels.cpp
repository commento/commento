#include "PerformanceLevels.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

PerformanceLevels::PerformanceLevels() noexcept
{
    for (auto& target : targetGains)
        target.store(1.0f, std::memory_order_relaxed);
}

void PerformanceLevels::setTargetGain(int index, float linearGain) noexcept
{
    if (! juce::isPositiveAndBelow(index, count))
        return;

    const auto finiteGain = std::isfinite(linearGain) ? linearGain : 1.0f;
    targetGains[static_cast<std::size_t>(index)].store(
        std::clamp(finiteGain, 0.0f, 1.0f), std::memory_order_relaxed);
}

float PerformanceLevels::getTargetGain(int index) const noexcept
{
    return juce::isPositiveAndBelow(index, count)
        ? targetGains[static_cast<std::size_t>(index)].load(std::memory_order_relaxed)
        : 1.0f;
}

void PerformanceLevels::prepare(double sampleRate) noexcept
{
    const auto usableSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (int index = 0; index < count; ++index)
    {
        auto& smoother = smoothedGains[static_cast<std::size_t>(index)];
        const auto target = getTargetGain(index);
        smoother.reset(usableSampleRate, 0.035);
        smoother.setCurrentAndTargetValue(target);
    }
}

void PerformanceLevels::process(int index, juce::AudioBuffer<float>& buffer,
                                int numSamples) noexcept
{
    if (! juce::isPositiveAndBelow(index, count) || numSamples <= 0)
        return;

    const auto samples = juce::jmin(numSamples, buffer.getNumSamples());
    const auto channels = buffer.getNumChannels();
    if (samples <= 0 || channels <= 0)
        return;

    auto& smoother = smoothedGains[static_cast<std::size_t>(index)];
    const auto target = getTargetGain(index);
    if (std::abs(target - smoother.getTargetValue()) > 1.0e-6f)
        smoother.setTargetValue(target);

    for (int sample = 0; sample < samples; ++sample)
    {
        const auto gain = smoother.getNextValue();
        for (int channel = 0; channel < channels; ++channel)
            buffer.setSample(channel, sample,
                             buffer.getSample(channel, sample) * gain);
    }
}
