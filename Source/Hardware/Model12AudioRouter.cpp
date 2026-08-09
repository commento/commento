#include "Model12AudioRouter.h"

#include <array>
#include <cstddef>

namespace
{
constexpr std::uint64_t channelFieldMask = 0xffu;
constexpr int encodedNone = 0xff;
constexpr int minimumScratchSamples = 8192;

constexpr std::uint64_t encodeChannel(int channel) noexcept
{
    return static_cast<std::uint64_t>(channel < 0 ? encodedNone : channel);
}

constexpr int decodeChannel(std::uint64_t packed, int field) noexcept
{
    const auto value = static_cast<int>(
        (packed >> (field * 8)) & channelFieldMask);
    return value == encodedNone
        ? Model12AudioRouter::RoutingConfig::none : value;
}
}

// The supported Raspberry Pi 5 image is 64-bit. Failing explicitly on a
// 32-bit target is preferable to silently putting a library lock in the audio
// callback; installing Raspberry Pi OS 64-bit satisfies this requirement.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "Commento audio routing requires a 64-bit lock-free target");

Model12AudioRouter::Model12AudioRouter(
    EcosystemEngine& targetEngine) noexcept
    : engine(targetEngine),
      packedRouting(pack(getModel12DefaultRouting()))
{
    // Constructor and device start both run off the real-time thread. Keeping
    // a useful initial capacity also supports the engine's direct test host.
    logicalOutputBuffer.setSize(EcosystemEngine::logicalOutputBusCount,
                                minimumScratchSamples, false, true, false);
}

void Model12AudioRouter::setRoutingConfig(
    const RoutingConfig& newRouting) noexcept
{
    packedRouting.store(pack(sanitise(newRouting)), std::memory_order_release);
}

Model12AudioRouter::RoutingConfig
Model12AudioRouter::getRoutingConfig() const noexcept
{
    return unpack(packedRouting.load(std::memory_order_acquire));
}

Model12AudioRouter::RoutingConfig
Model12AudioRouter::getModel12DefaultRouting() noexcept
{
    return {};
}

Model12AudioRouter::RoutingConfig
Model12AudioRouter::getGenericStereoRouting() noexcept
{
    RoutingConfig result;
    result.saxInputLeft = 0;
    result.saxInputRight = 1;
    result.ambientOutputLeft = 0;
    result.ambientOutputRight = 1;
    result.bassOutputLeft = 0;
    result.bassOutputRight = 1;
    result.saxOutputLeft = 0;
    result.saxOutputRight = 1;
    return result;
}

Model12AudioRouter::RoutingConfig Model12AudioRouter::sanitise(
    const RoutingConfig& routing) noexcept
{
    auto result = routing;
    const auto validChannel = [](int channel) noexcept
    {
        return channel >= 0 && channel <= maximumPhysicalChannelIndex
            ? channel : RoutingConfig::none;
    };

    result.saxInputLeft = validChannel(result.saxInputLeft);
    result.saxInputRight = validChannel(result.saxInputRight);
    if (result.saxInputLeft == RoutingConfig::none
        && result.saxInputRight != RoutingConfig::none)
    {
        result.saxInputLeft = result.saxInputRight;
        result.saxInputRight = RoutingConfig::none;
    }

    std::array<int*, 6> outputs {
        &result.ambientOutputLeft,
        &result.ambientOutputRight,
        &result.bassOutputLeft,
        &result.bassOutputRight,
        &result.saxOutputLeft,
        &result.saxOutputRight
    };
    for (auto* output : outputs)
        *output = validChannel(*output);

    // Sending the same mono bass bus twice to one physical channel would
    // merely add 6 dB, so collapse that accidental duplicate. Collisions
    // between different logical buses remain valid and are mixed later.
    if (result.bassOutputRight == result.bassOutputLeft)
        result.bassOutputRight = RoutingConfig::none;

    return result;
}

std::uint64_t Model12AudioRouter::pack(
    const RoutingConfig& routing) noexcept
{
    const auto safe = sanitise(routing);
    return encodeChannel(safe.saxInputLeft)
        | (encodeChannel(safe.saxInputRight) << 8)
        | (encodeChannel(safe.ambientOutputLeft) << 16)
        | (encodeChannel(safe.ambientOutputRight) << 24)
        | (encodeChannel(safe.bassOutputLeft) << 32)
        | (encodeChannel(safe.bassOutputRight) << 40)
        | (encodeChannel(safe.saxOutputLeft) << 48)
        | (encodeChannel(safe.saxOutputRight) << 56);
}

Model12AudioRouter::RoutingConfig Model12AudioRouter::unpack(
    std::uint64_t packed) noexcept
{
    RoutingConfig result;
    result.saxInputLeft = decodeChannel(packed, 0);
    result.saxInputRight = decodeChannel(packed, 1);
    result.ambientOutputLeft = decodeChannel(packed, 2);
    result.ambientOutputRight = decodeChannel(packed, 3);
    result.bassOutputLeft = decodeChannel(packed, 4);
    result.bassOutputRight = decodeChannel(packed, 5);
    result.saxOutputLeft = decodeChannel(packed, 6);
    result.saxOutputRight = decodeChannel(packed, 7);
    return result;
}

int Model12AudioRouter::getPhysicalInputChannelCount() const
{
    return physicalInputChannels.load(std::memory_order_relaxed);
}

int Model12AudioRouter::getPhysicalOutputChannelCount() const
{
    return physicalOutputChannels.load(std::memory_order_relaxed);
}

void Model12AudioRouter::audioDeviceIOCallbackWithContext(
    const float* const* physicalInputs, int numPhysicalInputs,
    float* const* physicalOutputs, int numPhysicalOutputs, int numSamples,
    const juce::AudioIODeviceCallbackContext& context)
{
    physicalInputChannels.store(numPhysicalInputs, std::memory_order_relaxed);
    physicalOutputChannels.store(numPhysicalOutputs, std::memory_order_relaxed);

    for (int channel = 0; physicalOutputs != nullptr
         && channel < numPhysicalOutputs; ++channel)
        if (physicalOutputs[channel] != nullptr)
            juce::FloatVectorOperations::clear(physicalOutputs[channel], numSamples);

    const auto routing = getRoutingConfig();
    const float* logicalInputs[2] { nullptr, nullptr };
    const auto routeInput = [physicalInputs, numPhysicalInputs](int channel)
        -> const float*
    {
        return physicalInputs != nullptr
                && juce::isPositiveAndBelow(channel, numPhysicalInputs)
            ? physicalInputs[channel] : nullptr;
    };
    logicalInputs[0] = routeInput(routing.saxInputLeft);
    logicalInputs[1] = routeInput(routing.saxInputRight);
    const auto numLogicalInputs = logicalInputs[1] != nullptr ? 2
        : (logicalInputs[0] != nullptr ? 1 : 0);

    if (numSamples > logicalOutputBuffer.getNumSamples())
    {
        // Never resize or allocate on the audio thread. The hardware buffers
        // have already been silenced, but the engine still receives input so
        // recording and safety state can advance gracefully.
        engine.audioDeviceIOCallbackWithContext(
            logicalInputs, numLogicalInputs, nullptr, 0, numSamples, context);
        return;
    }

    logicalOutputBuffer.clear(0, numSamples);
    float* logicalOutputs[EcosystemEngine::logicalOutputBusCount] {};
    for (int bus = 0; bus < EcosystemEngine::logicalOutputBusCount; ++bus)
        logicalOutputs[bus] = logicalOutputBuffer.getWritePointer(bus);

    engine.audioDeviceIOCallbackWithContext(
        logicalInputs, numLogicalInputs, logicalOutputs,
        EcosystemEngine::logicalOutputBusCount, numSamples, context);

    struct BusRoute
    {
        int bus;
        int physicalChannel;
    };
    const std::array<BusRoute, 6> routes {{
        { EcosystemEngine::ambientLeftBus, routing.ambientOutputLeft },
        { EcosystemEngine::ambientRightBus, routing.ambientOutputRight },
        { EcosystemEngine::bassBus, routing.bassOutputLeft },
        { EcosystemEngine::bassBus, routing.bassOutputRight },
        { EcosystemEngine::saxLeftBus, routing.saxOutputLeft },
        { EcosystemEngine::saxRightBus, routing.saxOutputRight }
    }};

    const auto mixBusToPhysical = [physicalOutputs, numPhysicalOutputs,
                                   numSamples, this, &routes](
                                      const BusRoute& route)
    {
        if (physicalOutputs == nullptr
            || ! juce::isPositiveAndBelow(route.physicalChannel,
                                          numPhysicalOutputs)
            || physicalOutputs[route.physicalChannel] == nullptr)
            return;

        int busesOnDestination = 0;
        for (const auto& candidate : routes)
            if (candidate.physicalChannel == route.physicalChannel)
                ++busesOnDestination;

        // Equal-power artistic mixing is not wanted here: 1/N guarantees
        // deterministic headroom when a generic stereo preset converges
        // ambient, bass and sax. A collision-free route keeps unity gain.
        const auto routeGain = 1.0f
            / static_cast<float>(juce::jmax(1, busesOnDestination));
        juce::FloatVectorOperations::addWithMultiply(
            physicalOutputs[route.physicalChannel],
            logicalOutputBuffer.getReadPointer(route.bus),
            routeGain, numSamples);
    };

    for (const auto& route : routes)
        mixBusToPhysical(route);
}

void Model12AudioRouter::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const auto deviceBlockSize = device != nullptr
        ? device->getCurrentBufferSizeSamples() : 0;
    logicalOutputBuffer.setSize(
        EcosystemEngine::logicalOutputBusCount,
        juce::jmax(minimumScratchSamples, deviceBlockSize),
        false, true, false);
    engine.audioDeviceAboutToStart(device);
}

void Model12AudioRouter::audioDeviceStopped()
{
    physicalInputChannels.store(0, std::memory_order_relaxed);
    physicalOutputChannels.store(0, std::memory_order_relaxed);
    engine.audioDeviceStopped();
}
