#pragma once

#include <JuceHeader.h>
#include "Engine/EcosystemEngine.h"

#include <atomic>
#include <cstdint>

class Model12AudioRouter final : public juce::AudioIODeviceCallback
{
public:
    struct RoutingConfig
    {
        static constexpr int none = -1;

        // Zero-based physical channel indices. `none` disconnects a route.
        int saxInputLeft = 6;
        int saxInputRight = 7;
        int ambientOutputLeft = 0;
        int ambientOutputRight = 1;
        int bassOutputLeft = 4;
        int bassOutputRight = none;
        int saxOutputLeft = 6;
        int saxOutputRight = 7;

        [[nodiscard]] bool operator==(const RoutingConfig& other) const noexcept
        {
            return saxInputLeft == other.saxInputLeft
                && saxInputRight == other.saxInputRight
                && ambientOutputLeft == other.ambientOutputLeft
                && ambientOutputRight == other.ambientOutputRight
                && bassOutputLeft == other.bassOutputLeft
                && bassOutputRight == other.bassOutputRight
                && saxOutputLeft == other.saxOutputLeft
                && saxOutputRight == other.saxOutputRight;
        }

        [[nodiscard]] bool operator!=(const RoutingConfig& other) const noexcept
        {
            return ! (*this == other);
        }
    };

    static constexpr int maximumPhysicalChannelIndex = 254;

    explicit Model12AudioRouter(EcosystemEngine& targetEngine) noexcept;

    // This can be called from the message thread while audio is running. The
    // whole routing snapshot becomes visible to the callback atomically.
    // Invalid indices become `none`. Multiple logical buses may deliberately
    // target the same physical output; they are mixed through an intermediate
    // buffer, never by aliasing the engine's bus pointers.
    void setRoutingConfig(const RoutingConfig& newRouting) noexcept;
    [[nodiscard]] RoutingConfig getRoutingConfig() const noexcept;
    [[nodiscard]] static RoutingConfig getModel12DefaultRouting() noexcept;
    [[nodiscard]] static RoutingConfig getGenericStereoRouting() noexcept;

    [[nodiscard]] int getPhysicalInputChannelCount() const;
    [[nodiscard]] int getPhysicalOutputChannelCount() const;

    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData, int numOutputChannels, int numSamples,
        const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    [[nodiscard]] static RoutingConfig sanitise(
        const RoutingConfig& routing) noexcept;
    [[nodiscard]] static std::uint64_t pack(
        const RoutingConfig& routing) noexcept;
    [[nodiscard]] static RoutingConfig unpack(std::uint64_t packed) noexcept;

    EcosystemEngine& engine;
    juce::AudioBuffer<float> logicalOutputBuffer;
    std::atomic<std::uint64_t> packedRouting { 0 };
    std::atomic<int> physicalInputChannels { 0 };
    std::atomic<int> physicalOutputChannels { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Model12AudioRouter)
};
