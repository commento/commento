#pragma once

#include <JuceHeader.h>
#include "Engine/EcosystemEngine.h"

#include <atomic>

class Model12AudioRouter final : public juce::AudioIODeviceCallback
{
public:
    explicit Model12AudioRouter(EcosystemEngine& targetEngine)
        : engine(targetEngine)
    {
    }

    [[nodiscard]] int getPhysicalInputChannelCount() const;
    [[nodiscard]] int getPhysicalOutputChannelCount() const;

    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData, int numOutputChannels, int numSamples,
        const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    EcosystemEngine& engine;
    std::atomic<int> physicalInputChannels { 0 };
    std::atomic<int> physicalOutputChannels { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Model12AudioRouter)
};
