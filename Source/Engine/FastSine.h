#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <cstddef>

namespace CommentoDsp
{
constexpr int sineTableSize = 4096;

inline const std::array<float, sineTableSize + 1> sineTable = []
{
    std::array<float, sineTableSize + 1> table {};
    for (int index = 0; index <= sineTableSize; ++index)
        table[static_cast<std::size_t>(index)] = static_cast<float>(std::sin(
            juce::MathConstants<double>::twoPi
            * static_cast<double>(index) / static_cast<double>(sineTableSize)));
    return table;
}();

// Audio oscillators in Commento keep their phase in [0, 2pi). Avoiding libm
// in the per-sample path materially reduces callback variance on Raspberry Pi.
[[nodiscard]] inline float fastSine(double phase) noexcept
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

[[nodiscard]] inline float fastCosine(double phase) noexcept
{
    auto shifted = phase + juce::MathConstants<double>::halfPi;
    if (shifted >= juce::MathConstants<double>::twoPi)
        shifted -= juce::MathConstants<double>::twoPi;
    return fastSine(shifted);
}
}
