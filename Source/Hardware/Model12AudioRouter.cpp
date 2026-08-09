#include "Model12AudioRouter.h"

#include <array>

int Model12AudioRouter::getPhysicalInputChannelCount() const
{
    return physicalInputChannels.load();
}

int Model12AudioRouter::getPhysicalOutputChannelCount() const
{
    return physicalOutputChannels.load();
}

void Model12AudioRouter::audioDeviceIOCallbackWithContext(
    const float* const* physicalInputs, int numPhysicalInputs,
    float* const* physicalOutputs, int numPhysicalOutputs, int numSamples,
    const juce::AudioIODeviceCallbackContext& context)
{
    physicalInputChannels.store(numPhysicalInputs);
    physicalOutputChannels.store(numPhysicalOutputs);

    for (int channel = 0; physicalOutputs != nullptr
         && channel < numPhysicalOutputs; ++channel)
        if (physicalOutputs[channel] != nullptr)
            juce::FloatVectorOperations::clear(physicalOutputs[channel], numSamples);

    const auto firstSaxChannel = numPhysicalInputs >= 8 ? 6 : 0;
    const auto secondSaxChannel = numPhysicalInputs >= 8 ? 7 : 1;
    const float* logicalInputs[2] { nullptr, nullptr };
    int numLogicalInputs = 0;
    if (physicalInputs != nullptr && numPhysicalInputs > firstSaxChannel)
    {
        logicalInputs[0] = physicalInputs[firstSaxChannel];
        numLogicalInputs = 1;
        if (numPhysicalInputs > secondSaxChannel)
        {
            logicalInputs[1] = physicalInputs[secondSaxChannel];
            numLogicalInputs = 2;
        }
    }

    // The engine owns five named buses. Their physical Model 12 returns are:
    // ambient 1/2, bass 5, and sax 7/8. Every other hardware output remains
    // at the zeroes written above.
    constexpr std::array<int, EcosystemEngine::logicalOutputBusCount> physicalMap {
        0, 1, 4, 6, 7
    };
    float* logicalOutputs[EcosystemEngine::logicalOutputBusCount] {};
    const auto numLogicalOutputs = physicalOutputs != nullptr
        ? EcosystemEngine::logicalOutputBusCount : 0;
    for (int bus = 0; bus < numLogicalOutputs; ++bus)
    {
        const auto physicalChannel = physicalMap[static_cast<size_t>(bus)];
        if (juce::isPositiveAndBelow(physicalChannel, numPhysicalOutputs))
            logicalOutputs[bus] = physicalOutputs[physicalChannel];
    }

    engine.audioDeviceIOCallbackWithContext(
        logicalInputs, numLogicalInputs, logicalOutputs, numLogicalOutputs,
        numSamples, context);
}

void Model12AudioRouter::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    engine.audioDeviceAboutToStart(device);
}

void Model12AudioRouter::audioDeviceStopped()
{
    physicalInputChannels.store(0);
    physicalOutputChannels.store(0);
    engine.audioDeviceStopped();
}
