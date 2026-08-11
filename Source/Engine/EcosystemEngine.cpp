#include "EcosystemEngine.h"
#include "FastSine.h"

#include <algorithm>
#include <cmath>

#if JUCE_LINUX
 #include <pthread.h>
 #include <sched.h>
#endif

namespace
{
constexpr std::uint8_t touchscreenGestureBit = 1u;
constexpr std::uint8_t midiGestureBit = 2u;
constexpr int freezeController = 80;
constexpr int echoThrowController = 81;
constexpr int saxListenController = 82;
constexpr std::uint32_t saxFootswitchNumberMask = 0xffu;
constexpr std::uint32_t saxFootswitchTypeShift = 8u;
constexpr std::uint32_t saxFootswitchTypeMask = 0x7u << saxFootswitchTypeShift;
constexpr std::uint32_t saxFootswitchRoleShift = 11u;
constexpr std::uint32_t saxFootswitchRoleMask = 0x3u << saxFootswitchRoleShift;
constexpr std::uint32_t saxFootswitchBindingMask
    = saxFootswitchNumberMask | saxFootswitchTypeMask | saxFootswitchRoleMask;
constexpr std::uint32_t saxFootswitchHeldBit = 1u << 30u;
constexpr std::uint32_t saxFootswitchLearningBit = 1u << 31u;

[[nodiscard]] bool isReservedPerformanceController(int controller) noexcept
{
    return controller == freezeController
        || controller == echoThrowController
        || controller == saxListenController
        || controller == 120 || controller == 123;
}

[[nodiscard]] std::uint32_t encodeSaxFootswitchBinding(
    EcosystemEngine::SaxFootswitchBinding binding) noexcept
{
    if (! binding.valid())
        return 0u;
    return static_cast<std::uint32_t>(binding.number + 1)
        | (static_cast<std::uint32_t>(binding.type)
            << saxFootswitchTypeShift)
        | (static_cast<std::uint32_t>(binding.role)
            << saxFootswitchRoleShift);
}

[[nodiscard]] EcosystemEngine::SaxFootswitchBinding
decodeSaxFootswitchBinding(std::uint32_t state) noexcept
{
    EcosystemEngine::SaxFootswitchBinding binding;
    binding.number = static_cast<int>(state & saxFootswitchNumberMask) - 1;
    binding.type = static_cast<EcosystemEngine::SaxFootswitchMessageType>(
        (state & saxFootswitchTypeMask) >> saxFootswitchTypeShift);
    binding.role = static_cast<EcosystemEngine::MidiInputRole>(
        (state & saxFootswitchRoleMask) >> saxFootswitchRoleShift);
    return binding;
}

[[nodiscard]] EcosystemEngine::SaxFootswitchBinding
saxFootswitchCandidate(const juce::MidiMessage& message,
                       EcosystemEngine::MidiInputRole role) noexcept
{
    using MessageType = EcosystemEngine::SaxFootswitchMessageType;
    EcosystemEngine::SaxFootswitchBinding binding;
    binding.role = role;

    if (message.isController())
    {
        const auto controller = message.getControllerNumber();
        if (message.getControllerValue() >= 64
            && ! isReservedPerformanceController(controller))
        {
            binding.type = MessageType::controller;
            binding.number = controller;
        }
    }
    return binding;
}

#if JUCE_LINUX
[[nodiscard]] int enableRealtimeAudioScheduling() noexcept
{
    // JUCE's ALSA backend starts a Priority::high thread, but on Linux that
    // priority enum does not install a realtime scheduling policy.  The kiosk
    // unit grants RLIMIT_RTPRIO explicitly, so claim a conservative FIFO
    // priority within that allowance and keep the callback off the ordinary
    // desktop scheduler.
    sched_param parameters {};
    parameters.sched_priority = 60;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters) == 0
        ? 1 : 0;
}
#endif

// Ordinary levels remain exactly linear. Only exceptional peaks enter this
// smooth, unit-slope knee before reaching the physical output.
[[nodiscard]] float protectPeak(float sample, float knee = 0.90f,
                                float ceiling = 0.98f) noexcept
{
    if (! std::isfinite(sample))
        return 0.0f;

    const auto magnitude = std::abs(sample);
    if (magnitude <= knee)
        return sample;

    const auto headroom = ceiling - knee;
    const auto excess = magnitude - knee;
    const auto protectedMagnitude = knee
        + headroom * excess / (headroom + excess);
    return std::copysign(protectedMagnitude, sample);
}

[[nodiscard]] std::uint32_t nextEvolutionRandom(
    std::uint32_t& state) noexcept
{
    // Deterministic, allocation-free randomness: performances keep evolving,
    // while tests and recordings remain reproducible after a fresh start.
    if (state == 0)
        state = 0x9e3779b9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

[[nodiscard]] SynthPatch applyTexture(SynthPatch patch, float amount) noexcept
{
    patch.drive = 1.0f + juce::jmax(0.0f, patch.drive - 1.0f) * amount;
    patch.noiseMix = juce::jlimit(
        0.0f, 0.62f,
        patch.noiseMix * (0.48f + amount * 0.82f) + amount * 0.012f);
    patch.detuneCents *= 0.62f + amount * 0.58f;
    patch.lfoDepth = juce::jlimit(
        0.0f, 0.62f, patch.lfoDepth * (0.68f + amount * 0.54f));
    patch.delayFeedback = juce::jlimit(
        0.0f, 0.76f, patch.delayFeedback * (0.82f + amount * 0.18f));
    return patch;
}

[[nodiscard]] SaxPatch applyTexture(SaxPatch patch, float amount) noexcept
{
    patch.drive = 1.0f + juce::jmax(0.0f, patch.drive - 1.0f) * amount;
    patch.modulationDepthMilliseconds *= 0.58f + amount * 0.72f;
    patch.feedback = juce::jlimit(
        0.0f, 0.68f, patch.feedback * (0.84f + amount * 0.16f));
    patch.crossFeedback = juce::jlimit(
        0.0f, 0.92f, patch.crossFeedback * (0.78f + amount * 0.22f));
    patch.tremoloDepth = juce::jlimit(
        0.0f, 0.65f, patch.tremoloDepth * (0.72f + amount * 0.38f));
    return patch;
}

}

EcosystemEngine::EcosystemEngine()
{
    for (auto& delayLevel : delayLevels)
        delayLevel.store(1.0f, std::memory_order_relaxed);
    for (auto& mask : freezeGestureMasks)
        mask.store(0u, std::memory_order_relaxed);
    for (auto& mask : echoThrowGestureMasks)
        mask.store(0u, std::memory_order_relaxed);

    const auto& initialScenario = CommentoScenarios::get(0);
    for (int index = 0; index < midiMemoryCount; ++index)
    {
        auto& memory = midiMemories[static_cast<size_t>(index)];
        memory.events.reserve(maximumMidiEvents + 128);
        memory.evolutionRandomState ^= static_cast<std::uint32_t>(
            0x85ebca6bu * static_cast<std::uint32_t>(index + 1));
        internalSynths[static_cast<size_t>(index)] = std::make_unique<AmbientSynth>(index);
        internalSynths[static_cast<size_t>(index)]->setPatch(
            initialScenario.layers[static_cast<size_t>(index)]);
    }
    saxProcessor.setPatch(initialScenario.sax);
    fourHeadSaxLoopPlaybackActive
        = initialScenario.useFourHeadSaxLoopPlayback;
    audioDecay.store(initialScenario.sax.loopDecay);
    activeScenario.store(0);
}

void EcosystemEngine::beginSaxFootswitchLearn() noexcept
{
    auto state = saxFootswitchState.load(std::memory_order_relaxed);
    for (;;)
    {
        const auto desired = (state & saxFootswitchBindingMask)
            | saxFootswitchLearningBit;
        if (saxFootswitchState.compare_exchange_weak(
                state, desired, std::memory_order_release,
                std::memory_order_relaxed))
            return;
    }
}

void EcosystemEngine::cancelSaxFootswitchLearn() noexcept
{
    saxFootswitchState.fetch_and(saxFootswitchBindingMask,
                                 std::memory_order_release);
}

void EcosystemEngine::clearSaxFootswitchBinding() noexcept
{
    saxFootswitchState.store(0u, std::memory_order_release);
}

bool EcosystemEngine::isSaxFootswitchLearning() const noexcept
{
    return (saxFootswitchState.load(std::memory_order_acquire)
            & saxFootswitchLearningBit) != 0u;
}

bool EcosystemEngine::hasSaxFootswitchBinding() const noexcept
{
    return getSaxFootswitchBinding().valid();
}

EcosystemEngine::SaxFootswitchBinding
EcosystemEngine::getSaxFootswitchBinding() const noexcept
{
    return decodeSaxFootswitchBinding(
        saxFootswitchState.load(std::memory_order_acquire));
}

void EcosystemEngine::setSaxFootswitchBinding(
    SaxFootswitchBinding binding) noexcept
{
    if (binding.type == SaxFootswitchMessageType::controller
        && isReservedPerformanceController(binding.number))
        binding = {};
    saxFootswitchState.store(encodeSaxFootswitchBinding(binding),
                             std::memory_order_release);
}

void EcosystemEngine::releaseSaxFootswitch() noexcept
{
    saxFootswitchState.fetch_and(~saxFootswitchHeldBit,
                                 std::memory_order_release);
}

bool EcosystemEngine::consumeSaxFootswitchMessage(
    const juce::MidiMessage& message, MidiInputRole role) noexcept
{
    const auto candidate = saxFootswitchCandidate(message, role);
    auto state = saxFootswitchState.load(std::memory_order_acquire);

    if (candidate.valid())
    {
        auto arrivedDuringLearn = false;
        while ((state & saxFootswitchLearningBit) != 0u)
        {
            arrivedDuringLearn = true;
            auto learnedState = encodeSaxFootswitchBinding(candidate);
            learnedState |= saxFootswitchHeldBit;

            if (saxFootswitchState.compare_exchange_weak(
                    state, learnedState, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return true;
        }

        // Another input may have won the learn CAS. This event still belonged
        // to the learning gesture and must never start/stop the sax recorder.
        if (arrivedDuringLearn)
            return true;
    }

    if ((state & saxFootswitchLearningBit) != 0u)
        return false;

    const auto binding = decodeSaxFootswitchBinding(state);
    if (! binding.valid() || binding.role != role)
        return false;

    const auto packedBinding = encodeSaxFootswitchBinding(binding);
    const auto stillMatches = [packedBinding](std::uint32_t value) noexcept
    {
        return (value & saxFootswitchLearningBit) == 0u
            && (value & saxFootswitchBindingMask) == packedBinding;
    };
    const auto toggleOnPress = [this, &state, &stillMatches]() noexcept
    {
        for (;;)
        {
            if (! stillMatches(state))
                return false;
            if ((state & saxFootswitchHeldBit) != 0u)
                return true;
            if (saxFootswitchState.compare_exchange_weak(
                    state, state | saxFootswitchHeldBit,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                toggleRecording(midiMemoryCount);
                return true;
            }
        }
    };
    const auto releaseHeld = [this, &state, &stillMatches]() noexcept
    {
        for (;;)
        {
            if (! stillMatches(state))
                return false;
            if ((state & saxFootswitchHeldBit) == 0u)
                return true;
            if (saxFootswitchState.compare_exchange_weak(
                    state, state & ~saxFootswitchHeldBit,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    };

    if (binding.type == SaxFootswitchMessageType::controller
        && message.isController()
        && message.getControllerNumber() == binding.number)
        return message.getControllerValue() >= 64
            ? toggleOnPress() : releaseHeld();

    return false;
}

void EcosystemEngine::enqueueMidiMessage(const juce::MidiMessage& message,
                                         MidiInputRole role)
{
    if (message.isActiveSense() || message.isMidiClock())
        return;

    if (consumeSaxFootswitchMessage(message, role))
        return;

    if (message.isController())
    {
        const auto controller = message.getControllerNumber();
        const auto value = message.getControllerValue();
        if (controller == freezeController)
        {
            setMidiFreezeEnabled(value >= 64);
            return;
        }
        if (controller == echoThrowController)
        {
            setMidiEchoThrowEnabled(value >= 64);
            return;
        }
        if (controller == saxListenController)
        {
            setSaxListenAmount(static_cast<float>(value) / 127.0f);
            return;
        }
        if (controller == 120 || controller == 123)
        {
            setMidiFreezeEnabled(false);
            setMidiEchoThrowEnabled(false);
        }
    }

    int start1 = 0;
    int size1 = 0;
    int start2 = 0;
    int size2 = 0;
    incomingFifo.prepareToWrite(1, start1, size1, start2, size2);
    juce::ignoreUnused(start2, size2);
    if (size1 == 0)
    {
        midiOverflowed.store(true);
        droppedMidiMessages.fetch_add(1);
        return;
    }

    auto& incoming = incomingMessages[static_cast<size_t>(start1)];
    incoming.message = message;
    incoming.timestampSeconds = std::isfinite(message.getTimeStamp())
        && message.getTimeStamp() > 0.0 ? message.getTimeStamp() : 0.0;
    incomingFifo.finishedWrite(1);
}

void EcosystemEngine::toggleRecording(int memoryIndex)
{
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount)
        && ! isLiveBassLayer(memoryIndex))
    {
        auto& memory = midiMemories[static_cast<size_t>(memoryIndex)];
        const auto shouldRecord = ! memory.recordingRequested.load();
        if (shouldRecord)
        {
            // Timestamp first, then publish the single request flag. Only the
            // audio thread writes the effective/display state.
            memory.armedAfterTimestampSeconds.store(
                juce::Time::getMillisecondCounterHiRes() * 0.001);
            memory.recordingRequested.store(true);
        }
        else
        {
            memory.recordingRequested.store(false);
            memory.armedAfterTimestampSeconds.store(0.0);
        }
    }
    else if (memoryIndex == midiMemoryCount)
    {
        auto& requested = audioMemory.recordingRequested;
        auto expected = requested.load(std::memory_order_relaxed);
        while (! requested.compare_exchange_weak(
            expected, ! expected, std::memory_order_release,
            std::memory_order_relaxed))
        {
        }
    }
}

void EcosystemEngine::clearMemory(int memoryIndex)
{
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount)
        && ! isLiveBassLayer(memoryIndex))
        midiMemories[static_cast<size_t>(memoryIndex)].clearRequested.store(true);
    else if (memoryIndex == midiMemoryCount)
        audioMemory.clearRequested.store(true);
}

bool EcosystemEngine::isRecording(int memoryIndex) const
{
    if (isLiveBassLayer(memoryIndex))
        return false;
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        return midiMemories[static_cast<size_t>(memoryIndex)].recordingRequested.load();
    return memoryIndex == midiMemoryCount && audioMemory.recordingForDisplay.load();
}

bool EcosystemEngine::isWaitingForFirstNote(int memoryIndex) const
{
    if (! juce::isPositiveAndBelow(memoryIndex, midiMemoryCount)
        || isLiveBassLayer(memoryIndex))
        return false;

    const auto& memory = midiMemories[static_cast<size_t>(memoryIndex)];
    return memory.recordingRequested.load()
        && (! memory.recordingForDisplay.load()
            || memory.waitingForFirstNoteForDisplay.load());
}

bool EcosystemEngine::hasMaterial(int memoryIndex) const
{
    if (isLiveBassLayer(memoryIndex))
        return false;
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        return midiMemories[static_cast<size_t>(memoryIndex)].containsMaterial.load();
    return memoryIndex == midiMemoryCount && audioMemory.containsMaterial.load();
}

double EcosystemEngine::getPhase(int memoryIndex) const
{
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        return midiMemories[static_cast<size_t>(memoryIndex)].phase.load();
    return memoryIndex == midiMemoryCount ? audioMemory.phase.load() : 0.0;
}

double EcosystemEngine::getLengthSeconds(int memoryIndex) const
{
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        return midiMemories[static_cast<size_t>(memoryIndex)].lengthSeconds.load();
    return memoryIndex == midiMemoryCount ? audioMemory.lengthSeconds.load() : 0.0;
}

int EcosystemEngine::getEventCount(int memoryIndex) const
{
    if (isLiveBassLayer(memoryIndex))
        return 0;
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        return midiMemories[static_cast<size_t>(memoryIndex)].eventCount.load();
    return 0;
}

EcosystemEngine::LoopEvolution EcosystemEngine::getLoopEvolution(
    int memoryIndex) const noexcept
{
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        return static_cast<LoopEvolution>(midiMemories[
            static_cast<std::size_t>(memoryIndex)].evolutionForDisplay.load(
                std::memory_order_relaxed));
    if (memoryIndex == midiMemoryCount)
        return static_cast<LoopEvolution>(
            audioMemory.evolutionForDisplay.load(std::memory_order_relaxed));
    return LoopEvolution::normal;
}

int EcosystemEngine::getMidiChannelForMemory(int memoryIndex) const
{
    return juce::isPositiveAndBelow(memoryIndex, midiMemoryCount)
        ? midiChannels[static_cast<size_t>(memoryIndex)] : 0;
}

bool EcosystemEngine::isAudioRunning() const
{
    return audioRunning.load();
}

int EcosystemEngine::getRealtimeSchedulingStatus() const noexcept
{
    return realtimeSchedulingStatus.load(std::memory_order_relaxed);
}

float EcosystemEngine::getDspLoad() const noexcept
{
    return dspLoad.load(std::memory_order_relaxed);
}

int EcosystemEngine::getDspNearOverloadCount() const noexcept
{
    return dspNearOverloadCount.load(std::memory_order_relaxed);
}

float EcosystemEngine::getCallbackIntervalLoad() const noexcept
{
    return callbackIntervalLoad.load(std::memory_order_relaxed);
}

int EcosystemEngine::getLateCallbackCount() const noexcept
{
    return lateCallbackCount.load(std::memory_order_relaxed);
}

int EcosystemEngine::getCallbackInputChannelCount() const
{
    return callbackInputChannels.load();
}

int EcosystemEngine::getCallbackOutputChannelCount() const
{
    return callbackOutputChannels.load();
}

float EcosystemEngine::getSaxInputLevel() const
{
    return saxInputLevel.load();
}

float EcosystemEngine::getStereoOutputLevel() const
{
    return stereoOutputLevel.load();
}

float EcosystemEngine::getBassOutputLevel() const
{
    return bassOutputLevel.load();
}

float EcosystemEngine::getSaxOutputLevel() const
{
    return saxOutputLevel.load();
}

bool EcosystemEngine::isSaxSafetyMuted() const
{
    return saxSafetyMuted.load();
}

int EcosystemEngine::getDroppedMidiMessageCount() const
{
    return droppedMidiMessages.load();
}

bool EcosystemEngine::isLiveBassLayer(int memoryIndex)
{
    return memoryIndex == bassLayerIndex;
}

void EcosystemEngine::setScenarioIndex(int index)
{
    requestedScenario.store(CommentoScenarios::wrapIndex(index));
}

int EcosystemEngine::getScenarioIndex() const
{
    return requestedScenario.load();
}

int EcosystemEngine::getScenarioMorphSourceIndex() const noexcept
{
    return scenarioMorphSource.load(std::memory_order_relaxed);
}

int EcosystemEngine::getScenarioMorphDestinationIndex() const noexcept
{
    const auto destination = activeScenario.load(std::memory_order_relaxed);
    return destination >= 0 ? destination : getScenarioIndex();
}

float EcosystemEngine::getScenarioMorphProgress() const noexcept
{
    return scenarioMorphProgress.load(std::memory_order_relaxed);
}

void EcosystemEngine::setTextureAmount(float amount)
{
    requestedTexture.store(juce::jlimit(0.0f, 1.0f, amount));
}

float EcosystemEngine::getTextureAmount() const
{
    return requestedTexture.load();
}

void EcosystemEngine::setFuzzEnabled(bool shouldBeEnabled) noexcept
{
    fuzzEnabled.store(shouldBeEnabled, std::memory_order_relaxed);
}

bool EcosystemEngine::isFuzzEnabled() const noexcept
{
    return fuzzEnabled.load(std::memory_order_relaxed);
}

void EcosystemEngine::setLoopEvolutionEnabled(
    bool shouldBeEnabled) noexcept
{
    loopEvolutionEnabled.store(shouldBeEnabled, std::memory_order_relaxed);
}

bool EcosystemEngine::isLoopEvolutionEnabled() const noexcept
{
    return loopEvolutionEnabled.load(std::memory_order_relaxed);
}

void EcosystemEngine::setBassEnabled(bool shouldBeEnabled)
{
    bassEnabled.store(shouldBeEnabled);
}

bool EcosystemEngine::isBassEnabled() const
{
    return bassEnabled.load();
}

void EcosystemEngine::setPerformanceLevel(int memoryIndex,
                                          float linearGain) noexcept
{
    performanceLevels.setTargetGain(memoryIndex, linearGain);
}

float EcosystemEngine::getPerformanceLevel(int memoryIndex) const noexcept
{
    return performanceLevels.getTargetGain(memoryIndex);
}

void EcosystemEngine::setDelayLevel(int memoryIndex, float amount) noexcept
{
    if (! juce::isPositiveAndBelow(memoryIndex, memoryCount))
        return;

    const auto safeAmount = std::isfinite(amount) ? amount : 0.0f;
    delayLevels[static_cast<std::size_t>(memoryIndex)].store(
        juce::jlimit(0.0f, 1.0f, safeAmount), std::memory_order_relaxed);
}

float EcosystemEngine::getDelayLevel(int memoryIndex) const noexcept
{
    return juce::isPositiveAndBelow(memoryIndex, memoryCount)
        ? delayLevels[static_cast<std::size_t>(memoryIndex)].load(
              std::memory_order_relaxed)
        : 0.0f;
}

void EcosystemEngine::setGestureTarget(int memoryIndex) noexcept
{
    gestureTarget.store(
        juce::jlimit(0, memoryCount - 1, memoryIndex),
        std::memory_order_relaxed);
}

void EcosystemEngine::setFreezeEnabled(int memoryIndex,
                                       bool shouldFreeze) noexcept
{
    if (memoryIndex <= bassLayerIndex
        || ! juce::isPositiveAndBelow(memoryIndex, memoryCount))
        return;

    auto& mask = freezeGestureMasks[static_cast<std::size_t>(memoryIndex)];
    if (shouldFreeze)
        mask.fetch_or(touchscreenGestureBit, std::memory_order_relaxed);
    else
        mask.fetch_and(static_cast<std::uint8_t>(~touchscreenGestureBit),
                       std::memory_order_relaxed);
}

bool EcosystemEngine::isFreezeEnabled(int memoryIndex) const noexcept
{
    return memoryIndex > bassLayerIndex
        && juce::isPositiveAndBelow(memoryIndex, memoryCount)
        && freezeGestureMasks[static_cast<std::size_t>(memoryIndex)].load(
               std::memory_order_relaxed) != 0u;
}

void EcosystemEngine::setEchoThrowEnabled(int memoryIndex,
                                          bool shouldThrow) noexcept
{
    if (memoryIndex <= bassLayerIndex
        || ! juce::isPositiveAndBelow(memoryIndex, memoryCount))
        return;

    auto& mask = echoThrowGestureMasks[static_cast<std::size_t>(memoryIndex)];
    if (shouldThrow)
        mask.fetch_or(touchscreenGestureBit, std::memory_order_relaxed);
    else
        mask.fetch_and(static_cast<std::uint8_t>(~touchscreenGestureBit),
                       std::memory_order_relaxed);
}

bool EcosystemEngine::isEchoThrowEnabled(int memoryIndex) const noexcept
{
    return memoryIndex > bassLayerIndex
        && juce::isPositiveAndBelow(memoryIndex, memoryCount)
        && echoThrowGestureMasks[static_cast<std::size_t>(memoryIndex)].load(
               std::memory_order_relaxed) != 0u;
}

void EcosystemEngine::setSaxListenAmount(float amount) noexcept
{
    const auto safeAmount = std::isfinite(amount) ? amount : 0.0f;
    saxListenAmount.store(juce::jlimit(0.0f, 1.0f, safeAmount),
                          std::memory_order_relaxed);
}

float EcosystemEngine::getSaxListenAmount() const noexcept
{
    return saxListenAmount.load(std::memory_order_relaxed);
}

void EcosystemEngine::releaseMomentaryGestures() noexcept
{
    clearMomentaryGestures();
}

void EcosystemEngine::setMidiFreezeEnabled(bool shouldFreeze) noexcept
{
    if (shouldFreeze)
    {
        const auto target = gestureTarget.load(std::memory_order_relaxed);
        const auto usableTarget = target > bassLayerIndex
            && juce::isPositiveAndBelow(target, memoryCount) ? target : -1;
        const auto previous = midiFreezeTarget.exchange(
            usableTarget, std::memory_order_relaxed);
        if (previous > bassLayerIndex && previous != usableTarget)
            freezeGestureMasks[static_cast<std::size_t>(previous)].fetch_and(
                static_cast<std::uint8_t>(~midiGestureBit),
                std::memory_order_relaxed);
        if (usableTarget > bassLayerIndex)
            freezeGestureMasks[static_cast<std::size_t>(usableTarget)].fetch_or(
                midiGestureBit, std::memory_order_relaxed);
        return;
    }

    midiFreezeTarget.store(-1, std::memory_order_relaxed);
    // Clear every MIDI-owned bit, not only the remembered target. Besides
    // acting as a panic, this makes release robust if an audio restart raced a
    // controller press between its target exchange and mask update.
    for (auto& mask : freezeGestureMasks)
        mask.fetch_and(static_cast<std::uint8_t>(~midiGestureBit),
                       std::memory_order_relaxed);
}

void EcosystemEngine::setMidiEchoThrowEnabled(bool shouldThrow) noexcept
{
    if (shouldThrow)
    {
        const auto target = gestureTarget.load(std::memory_order_relaxed);
        const auto usableTarget = target > bassLayerIndex
            && juce::isPositiveAndBelow(target, memoryCount) ? target : -1;
        const auto previous = midiEchoThrowTarget.exchange(
            usableTarget, std::memory_order_relaxed);
        if (previous > bassLayerIndex && previous != usableTarget)
            echoThrowGestureMasks[static_cast<std::size_t>(previous)].fetch_and(
                static_cast<std::uint8_t>(~midiGestureBit),
                std::memory_order_relaxed);
        if (usableTarget > bassLayerIndex)
            echoThrowGestureMasks[static_cast<std::size_t>(usableTarget)].fetch_or(
                midiGestureBit, std::memory_order_relaxed);
        return;
    }

    midiEchoThrowTarget.store(-1, std::memory_order_relaxed);
    for (auto& mask : echoThrowGestureMasks)
        mask.fetch_and(static_cast<std::uint8_t>(~midiGestureBit),
                       std::memory_order_relaxed);
}

void EcosystemEngine::clearMomentaryGestures() noexcept
{
    for (auto& mask : freezeGestureMasks)
        mask.store(0u, std::memory_order_relaxed);
    for (auto& mask : echoThrowGestureMasks)
        mask.store(0u, std::memory_order_relaxed);
    midiFreezeTarget.store(-1, std::memory_order_relaxed);
    midiEchoThrowTarget.store(-1, std::memory_order_relaxed);
}

int EcosystemEngine::memoryIndexForMidiChannel(int midiChannel)
{
    for (int index = 0; index < midiMemoryCount; ++index)
        if (midiChannels[static_cast<size_t>(index)] == midiChannel)
            return index;

    return -1;
}

void EcosystemEngine::setAudioDecay(float newDecay)
{
    audioDecay.store(juce::jlimit(0.80f, 1.0f, newDecay));
    audioDecayManualRevision.fetch_add(1, std::memory_order_relaxed);
}

float EcosystemEngine::getAudioDecay() const
{
    return audioDecay.load();
}

void EcosystemEngine::setSaxStereoInput(bool shouldUseStereo)
{
    saxStereoInput.store(shouldUseStereo);
}

bool EcosystemEngine::isSaxStereoInput() const
{
    return saxStereoInput.load();
}

void EcosystemEngine::setSaxPathMode(SaxPathMode mode)
{
    saxPathMode.store(juce::jlimit(
        static_cast<int>(SaxPathMode::muted),
        static_cast<int>(SaxPathMode::sceneEffects),
        static_cast<int>(mode)));
}

EcosystemEngine::SaxPathMode EcosystemEngine::getSaxPathMode() const
{
    return static_cast<SaxPathMode>(saxPathMode.load());
}

void EcosystemEngine::setDiagnosticToneBus(DiagnosticToneBus bus)
{
    diagnosticToneBus.store(juce::jlimit(
        static_cast<int>(DiagnosticToneBus::off),
        static_cast<int>(DiagnosticToneBus::sax),
        static_cast<int>(bus)));
}

EcosystemEngine::DiagnosticToneBus EcosystemEngine::getDiagnosticToneBus() const
{
    return static_cast<DiagnosticToneBus>(diagnosticToneBus.load());
}

void EcosystemEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    prepare(device != nullptr ? device->getCurrentSampleRate() : 48000.0,
            device != nullptr ? device->getCurrentBufferSizeSamples() : 4096);
    audioRunning.store(device != nullptr);
}

void EcosystemEngine::prepare(double newSampleRate, int maximumBlockSize)
{
    const auto preparedSampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    const auto sampleRateChanged = std::abs(sampleRate - preparedSampleRate) > 0.5;
    const auto needsAudioStorage = audioMemory.buffer.getNumSamples() == 0
        || sampleRateChanged;

    sampleRate = preparedSampleRate;
    clearMomentaryGestures();
    dspLoad.store(0.0f, std::memory_order_relaxed);
    dspNearOverloadCount.store(0, std::memory_order_relaxed);
    callbackIntervalLoad.store(0.0f, std::memory_order_relaxed);
    lateCallbackCount.store(0, std::memory_order_relaxed);
    dspWarmupCallbacksRemaining = 8;
    callbackTimingWarmupRemaining = 8;
    previousAudioCallbackTick = 0;
    previousAudioCallbackPeriod = 0.0;
    previousMidiCallbackTimeSeconds
        = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const auto maximumSamples = static_cast<int>(std::ceil(sampleRate * maximumAudioSeconds));
    if (needsAudioStorage)
    {
        audioMemory.recordingRequested.store(false);
        audioMemory.recordingForDisplay.store(false);
        audioMemory.containsMaterial.store(false);
        audioMemory.recordingActive = false;
        audioMemory.initialCapture = false;
        audioMemory.writePosition = 0;
        audioMemory.playbackPosition = 0;
        audioMemory.loopLength = 0;
        audioMemory.playbackGain = 1.0f;
        audioMemory.playbackGainStart = 1.0f;
        audioMemory.playbackGainTarget = 1.0f;
        audioMemory.gainTransitionSamplesRemaining = 0;
        audioMemory.gainTransitionSamplesTotal = 0;
        audioMemory.clearAfterGainTransition = false;
        audioMemory.phase.store(0.0);
        audioMemory.lengthSeconds.store(0.0);
        audioMemory.buffer.setSize(2, maximumSamples, false, true, false);
        audioMemory.buffer.clear();
    }
    else
    {
        // Device reconnection happens while audio is silent. Complete a
        // pending dissolve instead of allowing a half-cleared loop to return,
        // and resume a surviving loop at its stable gain.
        if (audioMemory.clearAfterGainTransition)
            finishAudioMemoryClear();
        audioMemory.playbackGain = 1.0f;
        audioMemory.playbackGainStart = 1.0f;
        audioMemory.playbackGainTarget = 1.0f;
        audioMemory.gainTransitionSamplesRemaining = 0;
        audioMemory.gainTransitionSamplesTotal = 0;
    }

    const auto renderCapacity = juce::jmax(8192, maximumBlockSize);
    ambientSynthBuffer.setSize(2, renderCapacity, false, true, false);
    bassSynthBuffer.setSize(2, renderCapacity, false, true, false);
    layerSynthBuffer.setSize(2, renderCapacity, false, true, false);
    saxRenderBuffer.setSize(2, renderCapacity, false, true, false);
    performanceLevels.prepare(sampleRate);
    bassMuteGain.reset(sampleRate, 0.006);
    bassMuteGain.setCurrentAndTargetValue(bassEnabled.load() ? 1.0f : 0.0f);
    bassWasEnabled = bassEnabled.load();
    saxListenMix.reset(sampleRate, 0.080);
    saxListenMix.setCurrentAndTargetValue(
        juce::jlimit(0.0f, 1.0f, saxListenAmount.load(
            std::memory_order_relaxed)));
    saxListenEnvelope = 0.0f;
    saxListenBlockGainStart = 1.0f;
    saxListenBlockGainEnd = 1.0f;
    for (int index = 0; index < memoryCount; ++index)
    {
        auto& mix = echoThrowMixes[static_cast<std::size_t>(index)];
        mix.reset(sampleRate, 0.025);
        mix.setCurrentAndTargetValue(0.0f);
        echoThrowBlockAmounts[static_cast<std::size_t>(index)] = 0.0f;
        echoThrowWasActive[static_cast<std::size_t>(index)] = false;
    }
    grainEffectMix.reset(sampleRate, 0.001);
    grainEffectMix.setCurrentAndTargetValue(
        juce::jlimit(0.0f, 1.0f, requestedTexture.load()));
    fuzzEffectMix.reset(sampleRate, 0.001);
    fuzzEffectMix.setCurrentAndTargetValue(
        fuzzEnabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
    activeGrainEffectTarget = grainEffectMix.getCurrentValue();
    activeFuzzEffectTarget = fuzzEffectMix.getCurrentValue();
    evolutionSampleClock = 0;
    nextEvolutionSample = 0;
    evolutionWasEnabled = false;
    activeMidiEvolutionMemory = -1;
    for (auto& memory : midiMemories)
    {
        memory.evolution = LoopEvolution::normal;
        memory.evolutionForDisplay.store(
            static_cast<int>(LoopEvolution::normal),
            std::memory_order_relaxed);
    }
    audioMemory.evolution = LoopEvolution::normal;
    audioMemory.evolutionForDisplay.store(
        static_cast<int>(LoopEvolution::normal),
        std::memory_order_relaxed);
    audioMemory.evolutionStartPosition = 0;
    audioMemory.evolutionDurationSamples = 0;
    audioMemory.evolutionSourcePosition = 0.0;
    grainHeldSamples.fill(0.0f);
    grainFilteredSamples.fill(0.0f);
    // A gentle one-pole low-pass removes the bright images produced by the
    // 6 kHz sample-and-hold without making the useful midrange opaque.  Its
    // coefficient is prepared off the audio thread and remains valid for
    // unusual device sample rates as well as the Model 12's usual 48 kHz.
    constexpr auto grainLowPassCutoffHz = 4200.0;
    const auto safeGrainCutoff = juce::jmin(
        grainLowPassCutoffHz, sampleRate * 0.40);
    grainLowPassCoefficient = static_cast<float>(1.0 - std::exp(
        -juce::MathConstants<double>::twoPi * safeGrainCutoff / sampleRate));
    grainHoldCounter = 0;
    grainFilterNeedsPrime = true;
    audioEvolutionFilteredSamples.fill(0.0f);
    constexpr auto audioEvolutionCutoffHz = 6000.0;
    const auto safeEvolutionCutoff = juce::jmin(
        audioEvolutionCutoffHz, sampleRate * 0.40);
    audioEvolutionLowPassCoefficient = static_cast<float>(1.0 - std::exp(
        -juce::MathConstants<double>::twoPi * safeEvolutionCutoff
        / sampleRate));
    blockMidiOutput.ensureSize(128 * 1024);
    for (auto& midi : layerMidiBuffers)
        midi.ensureSize(64 * 1024);
    for (int index = 0; index < midiMemoryCount; ++index)
    {
        auto& synth = internalSynths[static_cast<std::size_t>(index)];
        // A device reopen resets DERIVA ownership. Hard-stop every internal
        // channel first so a ghost voice cannot survive without its later
        // boundary note-off.
        synth->allNotesOff();
        synth->setFreezeEnabled(false);
        synth->setDelayLevel(getDelayLevel(index));
        synth->prepare(sampleRate, maximumBlockSize);
    }
    saxProcessor.setFreezeEnabled(false);
    saxProcessor.setDelayLevel(getDelayLevel(midiMemoryCount));
    saxProcessor.prepare(sampleRate, maximumBlockSize);
    fourHeadSaxLoopMix.reset(sampleRate, 0.001);
    fourHeadSaxLoopMix.setCurrentAndTargetValue(
        fourHeadSaxLoopPlaybackActive ? 1.0f : 0.0f);
    fourHeadMixBlockStart = fourHeadMixBlockEnd
        = fourHeadSaxLoopMix.getCurrentValue();
    resetCosmosHeads();

    // Force the requested scene and texture back onto freshly prepared DSP;
    // prepare may run again after reconnecting the Model 12.
    activeScenario.store(-1);
    activeTexture = -1.0f;
    scenarioMorphElapsedSamples = 0;
    scenarioMorphTotalSamples = 0;
    scenarioMorphProgress.store(1.0f, std::memory_order_relaxed);
    applyScenarioIfNeeded();
}

void EcosystemEngine::audioDeviceStopped()
{
    audioRunning.store(false);
    realtimeSchedulingStatus.store(-1, std::memory_order_relaxed);
    dspLoad.store(0.0f, std::memory_order_relaxed);
    dspNearOverloadCount.store(0, std::memory_order_relaxed);
    callbackIntervalLoad.store(0.0f, std::memory_order_relaxed);
    lateCallbackCount.store(0, std::memory_order_relaxed);
    callbackTimingWarmupRemaining = 0;
    previousAudioCallbackTick = 0;
    previousAudioCallbackPeriod = 0.0;
    callbackInputChannels.store(0);
    callbackOutputChannels.store(0);
    saxInputLevel.store(0.0f);
    stereoOutputLevel.store(0.0f);
    bassOutputLevel.store(0.0f);
    saxOutputLevel.store(0.0f);
    saxSafetyMuted.store(false);
    saxDangerSamples = 0;
    saxRecoverySamples = 0;
    saxSafetyGain = 1.0f;
    clearMomentaryGestures();
    for (int index = 0; index < midiMemoryCount; ++index)
    {
        auto& memory = midiMemories[static_cast<size_t>(index)];
        if (memory.recordingActive)
        {
            const auto closingPosition = juce::jmax<int64_t>(0, memory.recordPosition - 1);
            for (int note = 0;
                 note < static_cast<int>(memory.activeRecordedNotes.size()); ++note)
                if (memory.activeRecordedNotes[static_cast<size_t>(note)])
                    memory.events.push_back({ juce::MidiMessage::noteOff(
                                                  midiChannels[static_cast<size_t>(index)], note),
                                              closingPosition });
            memory.activeRecordedNotes.fill(false);
            memory.loopLength = memory.recordPosition;
            memory.playbackPosition = 0;
            const auto usable = memory.loopLength > 0 && ! memory.events.empty();
            memory.containsMaterial.store(usable);
            memory.eventCount.store(static_cast<int>(memory.events.size()));
            memory.lengthSeconds.store(usable
                ? static_cast<double>(memory.loopLength) / sampleRate : 0.0);
        }
        memory.recordingRequested.store(false);
        memory.recordingActive = false;
        memory.recordingForDisplay.store(false);
        memory.waitingForFirstNote = false;
        memory.waitingForFirstNoteForDisplay.store(false);
        memory.armedAfterTimestampSeconds.store(0.0);
    }

    if (audioMemory.recordingActive && audioMemory.initialCapture)
    {
        audioMemory.loopLength = audioMemory.writePosition;
        audioMemory.playbackPosition = 0;
        const auto usable = audioMemory.loopLength
            > static_cast<int64_t>(sampleRate * 0.05);
        audioMemory.containsMaterial.store(usable);
        audioMemory.lengthSeconds.store(usable
            ? static_cast<double>(audioMemory.loopLength) / sampleRate : 0.0);
        if (! usable)
            audioMemory.loopLength = 0;
    }
    if (audioMemory.clearAfterGainTransition)
        finishAudioMemoryClear();
    audioMemory.playbackGain = 1.0f;
    audioMemory.playbackGainStart = 1.0f;
    audioMemory.playbackGainTarget = 1.0f;
    audioMemory.gainTransitionSamplesRemaining = 0;
    audioMemory.gainTransitionSamplesTotal = 0;
    audioMemory.clearAfterGainTransition = false;
    audioMemory.recordingRequested.store(false);
    audioMemory.recordingActive = false;
    audioMemory.initialCapture = false;
    audioMemory.recordingForDisplay.store(false);
    resetCosmosHeads();
}

void EcosystemEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    const auto callbackStart = juce::Time::getHighResolutionTicks();
    const auto callbackPeriod = sampleRate > 0.0
        ? static_cast<double>(numSamples) / sampleRate : 0.0;
    if (previousAudioCallbackTick != 0 && callbackPeriod > 0.0)
    {
        const auto interval = juce::Time::highResolutionTicksToSeconds(
            callbackStart - previousAudioCallbackTick);
        // The elapsed interval belongs to the previous callback. This only
        // differs from callbackPeriod on variable-block backends, but using
        // the correct period prevents a 512 -> 256 transition from looking
        // like a false 200% scheduling delay.
        const auto expectedInterval = previousAudioCallbackPeriod > 0.0
            ? previousAudioCallbackPeriod : callbackPeriod;
        const auto intervalRatio = static_cast<float>(
            interval / expectedInterval);
        const auto previousRatio = callbackIntervalLoad.load(
            std::memory_order_relaxed);
        callbackIntervalLoad.store(
            juce::jmax(intervalRatio, previousRatio * 0.92f),
            std::memory_order_relaxed);
        if (callbackTimingWarmupRemaining > 0)
            --callbackTimingWarmupRemaining;
        else if (intervalRatio >= 1.35f)
            lateCallbackCount.fetch_add(1, std::memory_order_relaxed);
    }
    previousAudioCallbackTick = callbackStart;
    previousAudioCallbackPeriod = callbackPeriod;
#if JUCE_LINUX
    static thread_local const auto realtimeState
        = enableRealtimeAudioScheduling();
    realtimeSchedulingStatus.store(realtimeState, std::memory_order_relaxed);
#else
    realtimeSchedulingStatus.store(1, std::memory_order_relaxed);
#endif
    juce::ScopedNoDenormals noDenormals;
    callbackInputChannels.store(numInputChannels);
    callbackOutputChannels.store(numOutputChannels);

    for (int channel = 0; channel < numOutputChannels; ++channel)
        if (outputChannelData[channel] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

    evolutionSampleClock += juce::jmax(0, numSamples);
    const auto evolutionEnabledNow = loopEvolutionEnabled.load(
        std::memory_order_relaxed);
    if (evolutionEnabledNow && ! evolutionWasEnabled)
    {
        const auto firstDelaySeconds = 8u
            + nextEvolutionRandom(audioMemory.evolutionRandomState) % 5u;
        nextEvolutionSample = evolutionSampleClock
            + static_cast<int64_t>(std::round(
                sampleRate * static_cast<double>(firstDelaySeconds)));
    }
    else if (! evolutionEnabledNow)
        nextEvolutionSample = 0;
    evolutionWasEnabled = evolutionEnabledNow;

    blockMidiOutput.clear();
    if (midiOverflowed.exchange(false))
        for (const auto channel : midiChannels)
            blockMidiOutput.addEvent(juce::MidiMessage::allNotesOff(channel), 0);

    applyScenarioIfNeeded();
    advanceScenarioMorph(numSamples);
    updatePerformanceEffectTargets();
    updateMomentaryGestureTargets(numSamples);
    prepareSaxListenBlock(numSamples);
    for (int index = 1; index < midiMemoryCount; ++index)
        applyMidiCommands(midiMemories[static_cast<size_t>(index)],
                          midiChannels[static_cast<size_t>(index)], blockMidiOutput);
    applyAudioCommands();
    recordIncomingMidi(numSamples, blockMidiOutput);
    renderMidiMemories(numSamples, blockMidiOutput);
    renderInternalSynths(outputChannelData, numOutputChannels,
                         numSamples, blockMidiOutput);
    renderAudioMemory(inputChannelData, numInputChannels,
                      outputChannelData, numOutputChannels, numSamples);
    processPerformanceEffects(outputChannelData, numOutputChannels, numSamples);
    renderDiagnosticTone(outputChannelData, numOutputChannels, numSamples);

    float inputPeak = 0.0f;
    const auto saxLeftChannel = 0;
    const auto saxRightChannel = juce::jmin(1, numInputChannels - 1);
    if (numInputChannels > 0 && inputChannelData != nullptr
        && inputChannelData[saxLeftChannel] != nullptr)
    {
        for (int sample = 0; sample < numSamples; ++sample)
        {
            inputPeak = juce::jmax(inputPeak,
                                   std::abs(inputChannelData[saxLeftChannel][sample]));
            if (saxStereoInput.load()
                && juce::isPositiveAndBelow(saxRightChannel, numInputChannels)
                && inputChannelData[saxRightChannel] != nullptr)
                inputPeak = juce::jmax(inputPeak,
                                       std::abs(inputChannelData[saxRightChannel][sample]));
        }
    }
    saxInputLevel.store(juce::jmax(inputPeak, saxInputLevel.load() * 0.86f));

    // A sustained near-full-scale input is much more likely to be a USB
    // feedback loop or clipped preamp than a usable sax signal. Break the
    // return automatically, then recover only after one second of quiet.
    if (saxSafetyMuted.load())
    {
        saxRecoverySamples = inputPeak < 0.08f
            ? saxRecoverySamples + numSamples : 0;
        if (saxRecoverySamples >= static_cast<int64_t>(sampleRate))
        {
            saxSafetyMuted.store(false);
            saxRecoverySamples = 0;
            saxDangerSamples = 0;
        }
    }
    else
    {
        if (inputPeak > 0.94f)
            saxDangerSamples += numSamples;
        else
            saxDangerSamples = juce::jmax<int64_t>(
                0, saxDangerSamples - static_cast<int64_t>(numSamples) * 2);

        if (saxDangerSamples >= static_cast<int64_t>(sampleRate * 0.18))
        {
            saxSafetyMuted.store(true);
            saxRecoverySamples = 0;
        }
    }

    float ambientPeak = 0.0f;
    for (int channel = ambientLeftBus;
         channel <= ambientRightBus && channel < numOutputChannels; ++channel)
    {
        if (outputChannelData[channel] == nullptr)
            continue;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            auto& value = outputChannelData[channel][sample];
            value = protectPeak(value);
            ambientPeak = juce::jmax(ambientPeak, std::abs(value));
        }
    }
    stereoOutputLevel.store(juce::jmax(
        ambientPeak, stereoOutputLevel.load() * 0.86f));

    float bassPeak = 0.0f;
    if (numOutputChannels > bassBus && outputChannelData[bassBus] != nullptr)
    {
        for (int sample = 0; sample < numSamples; ++sample)
        {
            auto& value = outputChannelData[bassBus][sample];
            // Keep the meter honest about the level entering protection: if a
            // future patch overloads, the UI can show it even though the
            // physical Model 12 return remains safely below full scale. The
            // calibrated factory bass stays far below this transparent knee.
            bassPeak = juce::jmax(bassPeak,
                                  std::isfinite(value) ? std::abs(value) : 1.0f);
            value = protectPeak(value, 0.78f, 0.89f); // about -1 dBFS ceiling
        }
    }
    bassOutputLevel.store(juce::jmax(
        bassPeak, bassOutputLevel.load() * 0.86f));

    float saxPeak = 0.0f;
    for (int channel = saxLeftBus;
         channel <= saxRightBus && channel < numOutputChannels; ++channel)
        if (outputChannelData[channel] != nullptr)
            for (int sample = 0; sample < numSamples; ++sample)
                saxPeak = juce::jmax(saxPeak,
                    std::abs(outputChannelData[channel][sample]));
    saxOutputLevel.store(juce::jmax(
        saxPeak, saxOutputLevel.load() * 0.86f));

    if (dspWarmupCallbacksRemaining > 0)
        --dspWarmupCallbacksRemaining;
    else
    {
        const auto elapsed = juce::Time::highResolutionTicksToSeconds(
            juce::Time::getHighResolutionTicks() - callbackStart);
        const auto deadline = sampleRate > 0.0
            ? static_cast<double>(numSamples) / sampleRate : 0.0;
        const auto currentLoad = deadline > 0.0
            ? static_cast<float>(elapsed / deadline) : 0.0f;
        if (currentLoad >= 0.90f)
            dspNearOverloadCount.fetch_add(1, std::memory_order_relaxed);
        const auto previousLoad = dspLoad.load(std::memory_order_relaxed);
        dspLoad.store(juce::jmax(currentLoad, previousLoad * 0.92f),
                      std::memory_order_relaxed);
    }
}

void EcosystemEngine::applyScenarioIfNeeded()
{
    // Do not collapse a two-tap delay crossfade into a new one midway: that
    // cannot preserve the current sample and was audible when the arrows were
    // pressed rapidly. Keep only the latest requested scene/texture queued;
    // it starts on the callback after the current morph reaches its target.
    if (scenarioMorphTotalSamples > 0)
        return;

    const auto desired = CommentoScenarios::wrapIndex(requestedScenario.load());
    const auto texture = juce::jlimit(0.0f, 1.0f, requestedTexture.load());
    const auto previousScenario = activeScenario.load();
    const auto scenarioChanged = desired != previousScenario;
    if (desired == previousScenario
        && std::abs(texture - activeTexture) < 0.0001f)
        return;

    const auto& scenario = CommentoScenarios::get(desired);
    const auto initialApplication = previousScenario < 0;
    const auto fourHeadPlaybackWasActive = fourHeadSaxLoopPlaybackActive;
    const auto texturedSax = applyTexture(scenario.sax, texture);

    if (initialApplication)
    {
        for (int index = 0; index < midiMemoryCount; ++index)
            internalSynths[static_cast<size_t>(index)]->setPatch(
                applyTexture(scenario.layers[static_cast<size_t>(index)],
                             texture));
        saxProcessor.setPatch(texturedSax);
        audioDecay.store(scenario.sax.loopDecay);
        scenarioMorphSource.store(desired, std::memory_order_relaxed);
        scenarioMorphProgress.store(1.0f, std::memory_order_relaxed);
        scenarioMorphElapsedSamples = 0;
        scenarioMorphTotalSamples = 0;
        scenarioMorphDecayActive = false;
        fourHeadSaxLoopMix.setCurrentAndTargetValue(
            scenario.useFourHeadSaxLoopPlayback ? 1.0f : 0.0f);
        fourHeadMixBlockStart = fourHeadMixBlockEnd
            = fourHeadSaxLoopMix.getCurrentValue();
    }
    else
    {
        const auto duration = scenarioChanged ? scenarioMorphSeconds : 2.5;
        for (int index = 0; index < midiMemoryCount; ++index)
            internalSynths[static_cast<size_t>(index)]->beginPatchMorph(
                applyTexture(scenario.layers[static_cast<size_t>(index)],
                             texture), duration);
        saxProcessor.beginPatchMorph(texturedSax, duration);

        scenarioMorphSource.store(previousScenario,
                                  std::memory_order_relaxed);
        scenarioMorphProgress.store(0.0f, std::memory_order_relaxed);
        scenarioMorphElapsedSamples = 0;
        scenarioMorphTotalSamples = juce::jmax<int64_t>(
            1, static_cast<int64_t>(std::llround(duration * sampleRate)));
        scenarioMorphDecaySource = audioDecay.load();
        scenarioMorphDecayTarget = scenarioChanged
            ? scenario.sax.loopDecay : scenarioMorphDecaySource;
        scenarioMorphDecayRevision = audioDecayManualRevision.load(
            std::memory_order_relaxed);
        scenarioMorphDecayActive = scenarioChanged;

        const auto currentFourHeadMix
            = fourHeadSaxLoopMix.getCurrentValue();
        fourHeadSaxLoopMix.reset(sampleRate, duration);
        fourHeadSaxLoopMix.setCurrentAndTargetValue(currentFourHeadMix);
        fourHeadSaxLoopMix.setTargetValue(
            scenario.useFourHeadSaxLoopPlayback ? 1.0f : 0.0f);
        if (scenario.useFourHeadSaxLoopPlayback
            && ! fourHeadPlaybackWasActive)
            resetCosmosHeads();
    }

    fourHeadSaxLoopPlaybackActive = scenario.useFourHeadSaxLoopPlayback;
    activeScenario.store(desired);
    activeTexture = texture;
}

void EcosystemEngine::advanceScenarioMorph(int numSamples) noexcept
{
    fourHeadMixBlockStart = fourHeadSaxLoopMix.getCurrentValue();
    fourHeadMixBlockEnd = fourHeadSaxLoopMix.skip(juce::jmax(0, numSamples));

    if (scenarioMorphTotalSamples <= 0)
        return;

    scenarioMorphElapsedSamples = juce::jmin<int64_t>(
        scenarioMorphTotalSamples,
        scenarioMorphElapsedSamples + juce::jmax(0, numSamples));
    const auto linearProgress = static_cast<float>(scenarioMorphElapsedSamples)
        / static_cast<float>(scenarioMorphTotalSamples);
    const auto smoothProgress = linearProgress * linearProgress
        * (3.0f - 2.0f * linearProgress);
    scenarioMorphProgress.store(smoothProgress, std::memory_order_relaxed);
    if (scenarioMorphDecayActive)
    {
        if (audioDecayManualRevision.load(std::memory_order_relaxed)
            != scenarioMorphDecayRevision)
            scenarioMorphDecayActive = false;
        else
            audioDecay.store(scenarioMorphDecaySource
                + (scenarioMorphDecayTarget - scenarioMorphDecaySource)
                    * smoothProgress);
    }

    if (scenarioMorphElapsedSamples >= scenarioMorphTotalSamples)
    {
        if (scenarioMorphDecayActive)
            audioDecay.store(scenarioMorphDecayTarget);
        scenarioMorphDecayActive = false;
        scenarioMorphProgress.store(1.0f, std::memory_order_relaxed);
        scenarioMorphElapsedSamples = 0;
        scenarioMorphTotalSamples = 0;
    }
}

void EcosystemEngine::updatePerformanceEffectTargets() noexcept
{
    const auto grainTarget = juce::jlimit(
        0.0f, 1.0f, requestedTexture.load(std::memory_order_relaxed));
    if (std::abs(grainTarget - activeGrainEffectTarget) > 0.0001f)
    {
        const auto current = grainEffectMix.getCurrentValue();
        // These are performance gestures, not switches. The long ramps keep
        // their edges out of the audio and let the colour enter like a layer.
        grainEffectMix.reset(sampleRate,
                             grainTarget > current ? 1.5 : 3.0);
        grainEffectMix.setCurrentAndTargetValue(current);
        grainEffectMix.setTargetValue(grainTarget);
        activeGrainEffectTarget = grainTarget;
        if (grainTarget > current)
        {
            grainHoldCounter = 0;
            if (current <= 0.0001f)
                grainFilterNeedsPrime = true;
        }
    }

    const auto fuzzTarget = fuzzEnabled.load(std::memory_order_relaxed)
        ? 1.0f : 0.0f;
    if (std::abs(fuzzTarget - activeFuzzEffectTarget) > 0.0001f)
    {
        const auto current = fuzzEffectMix.getCurrentValue();
        fuzzEffectMix.reset(sampleRate,
                            fuzzTarget > current ? 1.5 : 3.0);
        fuzzEffectMix.setCurrentAndTargetValue(current);
        fuzzEffectMix.setTargetValue(fuzzTarget);
        activeFuzzEffectTarget = fuzzTarget;
    }
}

void EcosystemEngine::updateMomentaryGestureTargets(int numSamples) noexcept
{
    const auto samples = juce::jmax(0, numSamples);
    for (int index = 1; index < memoryCount; ++index)
    {
        const auto active = isEchoThrowEnabled(index);
        auto& wasActive = echoThrowWasActive[static_cast<std::size_t>(index)];
        auto& mix = echoThrowMixes[static_cast<std::size_t>(index)];
        if (active != wasActive)
        {
            const auto current = mix.getCurrentValue();
            // Fast enough to catch the performed note, slow enough to avoid a
            // wet-level edge. On release the return remains audible long
            // enough for the longest factory tap to speak at least once.
            mix.reset(sampleRate, active ? 0.025 : 4.0);
            mix.setCurrentAndTargetValue(current);
            mix.setTargetValue(active ? 1.0f : 0.0f);
            wasActive = active;
        }
        echoThrowBlockAmounts[static_cast<std::size_t>(index)]
            = mix.skip(samples);
    }
    echoThrowBlockAmounts[static_cast<std::size_t>(bassLayerIndex)] = 0.0f;
}

void EcosystemEngine::prepareSaxListenBlock(int numSamples) noexcept
{
    saxListenBlockGainStart = 1.0f;
    saxListenBlockGainEnd = 1.0f;
    if (numSamples <= 0 || sampleRate <= 0.0)
        return;

    const auto targetMix = juce::jlimit(
        0.0f, 1.0f, saxListenAmount.load(std::memory_order_relaxed));
    if (std::abs(targetMix - saxListenMix.getTargetValue()) > 0.0001f)
        saxListenMix.setTargetValue(targetMix);

    const auto mixStart = saxListenMix.getCurrentValue();
    const auto mixEnd = saxListenMix.skip(numSamples);
    if (juce::jmax(mixStart, mixEnd) <= 0.00001f)
    {
        saxListenEnvelope = 0.0f;
        return;
    }

    // Reuse the peak already measured for the hardware display. It belongs to
    // the preceding callback, adding only one block of latency (10.7 ms at the
    // Model 12 default) and avoiding a second scan of the sax input.
    auto inputLevel = saxInputLevel.load(std::memory_order_relaxed);
    if (! std::isfinite(inputLevel)
        || getSaxPathMode() == SaxPathMode::muted
        || saxSafetyMuted.load(std::memory_order_relaxed))
        inputLevel = 0.0f;
    const auto targetEnvelope = juce::jlimit(
        0.0f, 1.0f, (inputLevel - 0.015f) * 3.5f);
    const auto envelopeStart = saxListenEnvelope;
    const auto timeSeconds = targetEnvelope > envelopeStart ? 0.035 : 0.45;
    const auto blockCoefficient = static_cast<float>(1.0 - std::exp(
        -static_cast<double>(numSamples) / (timeSeconds * sampleRate)));
    saxListenEnvelope += (targetEnvelope - saxListenEnvelope)
                       * blockCoefficient;

    // ASCOLTO only makes the ambient bed yield to the live sax. Bass, RESPIRO
    // and physical routing remain untouched. The maximum reduction is about
    // 7 dB and both edges are rendered as one vector-friendly block ramp.
    constexpr auto maximumDuck = 0.55f;
    saxListenBlockGainStart = 1.0f
        - mixStart * maximumDuck * envelopeStart;
    saxListenBlockGainEnd = 1.0f
        - mixEnd * maximumDuck * saxListenEnvelope;
}

void EcosystemEngine::processPerformanceEffects(
    float* const* outputs, int outputChannels, int numSamples) noexcept
{
    const auto channels = juce::jmin(outputChannels, logicalOutputBusCount);
    if (outputs == nullptr || channels <= 0 || numSamples <= 0)
        return;

    const auto grainSilent = ! grainEffectMix.isSmoothing()
        && grainEffectMix.getCurrentValue() <= 0.0001f;
    const auto fuzzSilent = ! fuzzEffectMix.isSmoothing()
        && fuzzEffectMix.getCurrentValue() <= 0.0001f;
    if (grainSilent && fuzzSilent)
        return;

    // GRANA is deliberately fixed and cheap: a 6 kHz sample-and-hold at
    // 48 kHz followed by roughly six-bit quantisation and a gentle one-pole
    // high cut. The filter only colours the wet path; the slowly-smoothed
    // GRANA amount brings both in and out together without touching the dry
    // signal. FUZZ uses no transcendental maths and is level-contained before
    // the existing per-bus safety protection.
    constexpr int holdSamples = 8;
    constexpr float quantisationSteps = 64.0f;
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto grain = juce::jlimit(0.0f, 1.0f,
                                        grainEffectMix.getNextValue());
        const auto fuzz = juce::jlimit(0.0f, 1.0f,
                                       fuzzEffectMix.getNextValue());

        if (grainHoldCounter <= 0)
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto* data = outputs[channel];
                const auto input = data != nullptr && std::isfinite(data[sample])
                    ? data[sample] : 0.0f;
                grainHeldSamples[static_cast<std::size_t>(channel)]
                    = std::round(input * quantisationSteps)
                      / quantisationSteps;
            }
            if (grainFilterNeedsPrime)
            {
                for (int channel = 0; channel < channels; ++channel)
                    grainFilteredSamples[static_cast<std::size_t>(channel)]
                        = grainHeldSamples[static_cast<std::size_t>(channel)];
                grainFilterNeedsPrime = false;
            }
            grainHoldCounter = holdSamples;
        }
        --grainHoldCounter;

        for (int channel = 0; channel < channels; ++channel)
        {
            auto* data = outputs[channel];
            if (data == nullptr)
                continue;

            auto& filtered = grainFilteredSamples[
                static_cast<std::size_t>(channel)];
            filtered += grainLowPassCoefficient
                * (grainHeldSamples[static_cast<std::size_t>(channel)]
                   - filtered);
            const auto dry = std::isfinite(data[sample]) ? data[sample] : 0.0f;
            const auto crushed = dry
                + (filtered - dry) * grain;
            const auto hardFuzz = juce::jlimit(-1.0f, 1.0f,
                                               crushed * 4.0f) * 0.45f;
            data[sample] = crushed + (hardFuzz - crushed) * fuzz;
        }
    }
}

void EcosystemEngine::applyMidiCommands(MidiMemory& memory, int channel,
                                         juce::MidiBuffer& output)
{
    const auto memoryIndex = memoryIndexForMidiChannel(channel);
    const auto ghostChannel = juce::isPositiveAndBelow(
        memoryIndex, midiMemoryCount)
        ? evolutionMidiChannels[static_cast<std::size_t>(memoryIndex)] : 0;
    if (memory.clearRequested.exchange(false))
    {
        memory.events.clear();
        memory.loopLength = 0;
        memory.recordPosition = 0;
        memory.playbackPosition = 0;
        memory.recordingRequested.store(false);
        memory.recordingActive = false;
        memory.recordingForDisplay.store(false);
        memory.waitingForFirstNote = false;
        memory.waitingForFirstNoteForDisplay.store(false);
        memory.armedAfterTimestampSeconds.store(0.0);
        memory.containsMaterial.store(false);
        memory.eventCount.store(0);
        memory.lengthSeconds.store(0.0);
        memory.phase.store(0.0);
        memory.activeRecordedNotes.fill(false);
        memory.evolutionNote = -1;
        memory.evolutionVelocity = 0.25f;
        memory.evolution = LoopEvolution::normal;
        memory.evolutionForDisplay.store(
            static_cast<int>(LoopEvolution::normal),
            std::memory_order_relaxed);
        output.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
        if (ghostChannel > 0)
            output.addEvent(juce::MidiMessage::allNotesOff(ghostChannel), 0);
        if (activeMidiEvolutionMemory == memoryIndex)
            activeMidiEvolutionMemory = -1;
    }

    const auto shouldRecord = memory.recordingRequested.load();
    if (shouldRecord == memory.recordingActive)
        return;

    memory.recordingActive = shouldRecord;
    memory.recordingForDisplay.store(shouldRecord);
    memory.waitingForFirstNote = shouldRecord;
    memory.waitingForFirstNoteForDisplay.store(shouldRecord);
    output.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
    if (ghostChannel > 0)
        output.addEvent(juce::MidiMessage::allNotesOff(ghostChannel), 0);

    if (shouldRecord)
    {
        if (activeMidiEvolutionMemory == memoryIndex)
            activeMidiEvolutionMemory = -1;
        memory.events.clear();
        memory.recordPosition = 0;
        memory.playbackPosition = 0;
        memory.loopLength = 0;
        memory.containsMaterial.store(false);
        memory.eventCount.store(0);
        memory.lengthSeconds.store(0.0);
        memory.phase.store(0.0);
        memory.activeRecordedNotes.fill(false);
        memory.evolutionNote = -1;
        memory.evolutionVelocity = 0.25f;
        memory.evolution = LoopEvolution::normal;
        memory.evolutionForDisplay.store(
            static_cast<int>(LoopEvolution::normal),
            std::memory_order_relaxed);
    }
    else
    {
        memory.waitingForFirstNote = false;
        memory.waitingForFirstNoteForDisplay.store(false);
        memory.armedAfterTimestampSeconds.store(0.0);
        const auto closingPosition = juce::jmax<int64_t>(0, memory.recordPosition - 1);
        for (int note = 0; note < static_cast<int>(memory.activeRecordedNotes.size()); ++note)
        {
            if (memory.activeRecordedNotes[static_cast<size_t>(note)])
                memory.events.push_back({ juce::MidiMessage::noteOff(channel, note),
                                          closingPosition });
        }
        memory.activeRecordedNotes.fill(false);
        memory.loopLength = memory.recordPosition;
        memory.playbackPosition = 0;
        const auto usable = memory.loopLength > 0 && ! memory.events.empty();
        memory.containsMaterial.store(usable);
        memory.eventCount.store(static_cast<int>(memory.events.size()));
        memory.lengthSeconds.store(usable ? static_cast<double>(memory.loopLength) / sampleRate : 0.0);
        if (usable)
            chooseMidiEvolution(memory, memoryIndex);
        else
        {
            memory.evolution = LoopEvolution::normal;
            memory.evolutionForDisplay.store(
                static_cast<int>(LoopEvolution::normal),
                std::memory_order_relaxed);
        }
    }
}

void EcosystemEngine::applyAudioCommands()
{
    if (audioMemory.clearRequested.exchange(false))
    {
        audioMemory.recordingRequested.store(false);
        audioMemory.recordingActive = false;
        audioMemory.recordingForDisplay.store(false);
        audioMemory.initialCapture = false;

        // DIMENTICA is a musical dissolve, not a metadata operation. Keep the
        // loop readable until its contribution has reached zero; the live sax
        // monitor is outside this gain and therefore remains untouched.
        if (audioMemory.loopLength > 0 && audioMemory.playbackGain > 0.000001f)
            beginAudioMemoryGainTransition(0.0f, 1.0, true);
        else
            finishAudioMemoryClear();
    }

    // A new capture request made during the one-second dissolve is preserved
    // and starts on the first callback after the old memory is fully gone.
    if (audioMemory.clearAfterGainTransition)
        return;

    const auto shouldRecord = audioMemory.recordingRequested.load();
    if (shouldRecord == audioMemory.recordingActive)
        return;

    audioMemory.recordingActive = shouldRecord;
    audioMemory.recordingForDisplay.store(shouldRecord);
    if (shouldRecord)
    {
        audioMemory.initialCapture = audioMemory.loopLength == 0;
        if (audioMemory.initialCapture)
        {
            audioMemory.writePosition = 0;
            audioMemory.playbackPosition = 0;
            audioMemory.playbackGain = 0.0f;
            audioMemory.playbackGainStart = 0.0f;
            audioMemory.playbackGainTarget = 0.0f;
            audioMemory.gainTransitionSamplesRemaining = 0;
            audioMemory.gainTransitionSamplesTotal = 0;
            audioMemory.clearAfterGainTransition = false;
            audioMemory.containsMaterial.store(false);
            audioMemory.lengthSeconds.store(0.0);
            audioMemory.evolution = LoopEvolution::normal;
            audioMemory.evolutionForDisplay.store(
                static_cast<int>(LoopEvolution::normal),
                std::memory_order_relaxed);
            resetCosmosHeads();
        }
    }
    else if (audioMemory.initialCapture)
        finishInitialAudioCapture();
}

void EcosystemEngine::finishInitialAudioCapture() noexcept
{
    audioMemory.loopLength = audioMemory.writePosition;
    audioMemory.playbackPosition = 0;
    audioMemory.initialCapture = false;
    const auto usable = audioMemory.loopLength
        > static_cast<int64_t>(sampleRate * 0.05);
    audioMemory.containsMaterial.store(usable);
    audioMemory.lengthSeconds.store(usable
        ? static_cast<double>(audioMemory.loopLength) / sampleRate : 0.0);
    if (usable)
    {
        // The capture monitor was already audible. Bring the newly closed loop
        // underneath it over 125 ms so the first playback sample cannot double
        // the sax level at a block boundary.
        audioMemory.playbackGain = 0.0f;
        beginAudioMemoryGainTransition(1.0f, 0.125, false);
        chooseAudioEvolution();
    }
    else
    {
        audioMemory.loopLength = 0;
        audioMemory.playbackGain = 1.0f;
        audioMemory.playbackGainStart = 1.0f;
        audioMemory.playbackGainTarget = 1.0f;
        audioMemory.gainTransitionSamplesRemaining = 0;
        audioMemory.gainTransitionSamplesTotal = 0;
    }
    resetCosmosHeads();
}

void EcosystemEngine::beginAudioMemoryGainTransition(
    float targetGain, double seconds, bool clearWhenFinished) noexcept
{
    const auto finiteTarget = std::isfinite(targetGain) ? targetGain : 0.0f;
    audioMemory.playbackGainStart = audioMemory.playbackGain;
    audioMemory.playbackGainTarget = juce::jlimit(0.0f, 1.0f, finiteTarget);
    audioMemory.gainTransitionSamplesTotal
        = audioMemory.gainTransitionSamplesRemaining
        = juce::jmax<int64_t>(1, static_cast<int64_t>(
            std::llround(juce::jmax(0.0, seconds) * sampleRate)));
    audioMemory.clearAfterGainTransition = clearWhenFinished;
}

void EcosystemEngine::advanceAudioMemoryGainTransition() noexcept
{
    if (audioMemory.gainTransitionSamplesRemaining <= 0
        || audioMemory.gainTransitionSamplesTotal <= 0)
        return;

    --audioMemory.gainTransitionSamplesRemaining;
    const auto progress = 1.0f
        - static_cast<float>(audioMemory.gainTransitionSamplesRemaining)
            / static_cast<float>(audioMemory.gainTransitionSamplesTotal);
    audioMemory.playbackGain = audioMemory.playbackGainStart
        + (audioMemory.playbackGainTarget - audioMemory.playbackGainStart)
            * progress;

    if (audioMemory.gainTransitionSamplesRemaining == 0)
    {
        audioMemory.playbackGain = audioMemory.playbackGainTarget;
        if (audioMemory.clearAfterGainTransition)
            finishAudioMemoryClear();
    }
}

void EcosystemEngine::finishAudioMemoryClear() noexcept
{
    audioMemory.writePosition = 0;
    audioMemory.playbackPosition = 0;
    audioMemory.loopLength = 0;
    audioMemory.containsMaterial.store(false);
    audioMemory.lengthSeconds.store(0.0);
    audioMemory.phase.store(0.0);
    audioMemory.playbackGain = 1.0f;
    audioMemory.playbackGainStart = 1.0f;
    audioMemory.playbackGainTarget = 1.0f;
    audioMemory.gainTransitionSamplesRemaining = 0;
    audioMemory.gainTransitionSamplesTotal = 0;
    audioMemory.clearAfterGainTransition = false;
    audioMemory.evolution = LoopEvolution::normal;
    audioMemory.evolutionForDisplay.store(
        static_cast<int>(LoopEvolution::normal),
        std::memory_order_relaxed);
    audioMemory.evolutionStartPosition = 0;
    audioMemory.evolutionDurationSamples = 0;
    audioMemory.evolutionSourcePosition = 0.0;
    resetCosmosHeads();
}

void EcosystemEngine::recordIncomingMidi(int numSamples, juce::MidiBuffer& liveMidi)
{
    const auto callbackTimeSeconds
        = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const auto callbackIntervalSeconds
        = callbackTimeSeconds - previousMidiCallbackTimeSeconds;
    const auto callbackIntervalIsUsable = std::isfinite(callbackIntervalSeconds)
        && callbackIntervalSeconds > 0.0 && callbackIntervalSeconds < 0.5;

    const auto callbackIntervalSamples = callbackIntervalSeconds * sampleRate;
    const auto timestampToSampleOffset = [this, callbackIntervalSamples,
                                           callbackIntervalIsUsable,
                                           numSamples](double timestampSeconds)
    {
        if (timestampSeconds <= 0.0 || ! std::isfinite(timestampSeconds)
            || ! callbackIntervalIsUsable)
            return 0;

        const auto sourceOffset = (timestampSeconds
            - previousMidiCallbackTimeSeconds) * sampleRate;
        const auto mappedOffset = callbackIntervalSamples
                <= static_cast<double>(numSamples)
            ? static_cast<double>(numSamples) - callbackIntervalSamples
                + sourceOffset
            : sourceOffset * static_cast<double>(numSamples)
                / callbackIntervalSamples;
        return juce::jlimit(0, numSamples - 1,
            static_cast<int>(std::llround(mappedOffset)));
    };

    std::array<int, midiMemoryCount> captureStartOffsets {};
    for (int index = 0; index < midiMemoryCount; ++index)
    {
        const auto& memory = midiMemories[static_cast<size_t>(index)];
        captureStartOffsets[static_cast<size_t>(index)]
            = memory.recordingActive && ! memory.waitingForFirstNote
                ? 0 : numSamples;
    }

    int start1 = 0;
    int size1 = 0;
    int start2 = 0;
    int size2 = 0;
    incomingFifo.prepareToRead(incomingCapacity, start1, size1, start2, size2);

    auto lastSampleOffset = 0;
    const auto consumeRange = [this, &liveMidi, &captureStartOffsets,
                               &timestampToSampleOffset, &lastSampleOffset,
                               numSamples](int start, int size)
    {
        for (int offset = 0; offset < size; ++offset)
        {
            const auto& incoming
                = incomingMessages[static_cast<size_t>(start + offset)];
            const auto& message = incoming.message;
            const auto channel = message.getChannel();
            const auto memoryIndex = memoryIndexForMidiChannel(channel);
            if (memoryIndex < 0)
                continue;

            const auto sampleOffset = juce::jlimit(
                0, numSamples - 1,
                juce::jmax(lastSampleOffset,
                           timestampToSampleOffset(incoming.timestampSeconds)));
            lastSampleOffset = sampleOffset;
            auto& memory = midiMemories[static_cast<size_t>(memoryIndex)];

            // If SEMINA or its cancellation arrived after the command pass at
            // the top of this callback, synchronise at the first following
            // MIDI event. Do not close an already-running capture mid-block;
            // its stop remains a clean block-boundary operation.
            const auto shouldRecord = memory.recordingRequested.load();
            if (shouldRecord != memory.recordingActive
                && (shouldRecord || memory.waitingForFirstNote))
                applyMidiCommands(memory, channel, liveMidi);

            liveMidi.addEvent(message, sampleOffset);
            if (! memory.recordingActive)
                continue;

            auto& captureStartOffset
                = captureStartOffsets[static_cast<size_t>(memoryIndex)];
            if (memory.waitingForFirstNote)
            {
                if (! message.isNoteOn())
                    continue;

                const auto armedAfter
                    = memory.armedAfterTimestampSeconds.load();
                if (incoming.timestampSeconds > 0.0 && armedAfter > 0.0
                    && incoming.timestampSeconds < armedAfter)
                    continue;

                memory.waitingForFirstNote = false;
                memory.waitingForFirstNoteForDisplay.store(false);
                memory.armedAfterTimestampSeconds.store(0.0);
                captureStartOffset = sampleOffset;
            }

            if (static_cast<int>(memory.events.size()) < maximumMidiEvents)
            {
                const auto relativeOffset = juce::jmax(
                    0, sampleOffset - captureStartOffset);
                memory.events.push_back(
                    { message, memory.recordPosition + relativeOffset });
                if (message.isNoteOn())
                {
                    auto& active = memory.activeRecordedNotes[
                        static_cast<size_t>(message.getNoteNumber())];
                    active = true;
                    if (memory.evolutionNote < 0)
                    {
                        memory.evolutionNote = message.getNoteNumber();
                        memory.evolutionVelocity = juce::jlimit(
                            0.12f, 0.42f,
                            message.getFloatVelocity() * 0.38f);
                    }
                }
                else if (message.isNoteOff())
                {
                    auto& active = memory.activeRecordedNotes[
                        static_cast<size_t>(message.getNoteNumber())];
                    active = false;
                }
            }
        }
    };

    consumeRange(start1, size1);
    consumeRange(start2, size2);
    incomingFifo.finishedRead(size1 + size2);
    previousMidiCallbackTimeSeconds = callbackTimeSeconds;

    for (int index = 0; index < midiMemoryCount; ++index)
    {
        auto& memory = midiMemories[static_cast<size_t>(index)];
        if (! memory.recordingActive || memory.waitingForFirstNote)
            continue;
        memory.recordPosition += numSamples
            - captureStartOffsets[static_cast<size_t>(index)];
        memory.phase.store(std::fmod(static_cast<double>(memory.recordPosition) / sampleRate, 1.0));
        memory.lengthSeconds.store(static_cast<double>(memory.recordPosition) / sampleRate);
        memory.eventCount.store(static_cast<int>(memory.events.size()));
    }
}

void EcosystemEngine::renderInternalSynths(float* const* outputs, int outputChannels,
                                            int numSamples, const juce::MidiBuffer& midi)
{
    if (outputChannels <= 0)
        return;

    if (ambientSynthBuffer.getNumSamples() < numSamples
        || bassSynthBuffer.getNumSamples() < numSamples
        || layerSynthBuffer.getNumSamples() < numSamples)
        return;
    ambientSynthBuffer.clear(0, numSamples);
    bassSynthBuffer.clear(0, numSamples);

    for (int layer = 0; layer < midiMemoryCount; ++layer)
    {
        auto& layerMidi = layerMidiBuffers[static_cast<size_t>(layer)];
        layerMidi.clear();
        for (const auto metadata : midi)
            if (metadata.getMessage().getChannel()
                    == midiChannels[static_cast<size_t>(layer)]
                || metadata.getMessage().getChannel()
                    == evolutionMidiChannels[static_cast<size_t>(layer)])
                layerMidi.addEvent(metadata.getMessage(), metadata.samplePosition);

        if (layer == bassLayerIndex)
        {
            const auto enabled = bassEnabled.load();
            if (! enabled)
            {
                layerMidi.clear();
                if (bassWasEnabled)
                    layerMidi.addEvent(juce::MidiMessage::allNotesOff(
                                           midiChannels[static_cast<size_t>(layer)]), 0);
            }
            bassWasEnabled = enabled;
        }

        layerSynthBuffer.clear(0, numSamples);
        auto& synth = internalSynths[static_cast<size_t>(layer)];
        const auto throwAmount = echoThrowBlockAmounts[
            static_cast<std::size_t>(layer)];
        synth->setDelayLevel(juce::jmax(getDelayLevel(layer), throwAmount));
        synth->setFreezeEnabled(isFreezeEnabled(layer));
        synth->render(
            layerSynthBuffer, layerMidi, 0, numSamples);
        performanceLevels.process(layer, layerSynthBuffer, numSamples);
        if (layer == bassLayerIndex)
        {
            const auto muteTarget = bassWasEnabled ? 1.0f : 0.0f;
            if (std::abs(bassMuteGain.getTargetValue() - muteTarget) > 0.001f)
                bassMuteGain.setTargetValue(muteTarget);
            auto* left = layerSynthBuffer.getWritePointer(0);
            auto* right = layerSynthBuffer.getWritePointer(1);
            for (int sample = 0; sample < numSamples; ++sample)
            {
                const auto gain = bassMuteGain.getNextValue();
                left[sample] *= gain;
                right[sample] *= gain;
            }
        }

        auto& destination = layer == bassLayerIndex
            ? bassSynthBuffer : ambientSynthBuffer;
        for (int channel = 0; channel < destination.getNumChannels(); ++channel)
            destination.addFrom(channel, 0, layerSynthBuffer, channel, 0,
                                numSamples);
    }

    if (saxListenBlockGainStart < 0.99999f
        || saxListenBlockGainEnd < 0.99999f)
        for (int channel = 0; channel < ambientSynthBuffer.getNumChannels();
             ++channel)
            ambientSynthBuffer.applyGainRamp(
                channel, 0, numSamples,
                saxListenBlockGainStart, saxListenBlockGainEnd);

    for (int channel = ambientLeftBus;
         channel <= ambientRightBus && channel < outputChannels; ++channel)
    {
        if (outputs[channel] == nullptr)
            continue;
        juce::FloatVectorOperations::add(outputs[channel],
            ambientSynthBuffer.getReadPointer(channel), numSamples);
    }

    if (outputChannels > bassBus && outputs[bassBus] != nullptr)
        for (int sample = 0; sample < numSamples; ++sample)
            outputs[bassBus][sample] += 0.5f
                * (bassSynthBuffer.getSample(0, sample)
                   + bassSynthBuffer.getSample(1, sample));
    else
        for (int channel = 0; channel < juce::jmin(2, outputChannels); ++channel)
            if (outputs[channel] != nullptr)
                juce::FloatVectorOperations::add(outputs[channel],
                    bassSynthBuffer.getReadPointer(channel), numSamples);
}

float EcosystemEngine::readAudioMemorySample(int channel, double position,
                                              int64_t length) const noexcept
{
    if (length <= 0 || ! juce::isPositiveAndBelow(
            channel, audioMemory.buffer.getNumChannels()))
        return 0.0f;

    const auto safeLength = static_cast<double>(length);
    while (position < 0.0)
        position += safeLength;
    while (position >= safeLength)
        position -= safeLength;
    const auto first = static_cast<int64_t>(position);
    const auto second = (first + 1) % length;
    const auto fraction = static_cast<float>(position
                                              - static_cast<double>(first));
    return juce::jmap(
        fraction,
        audioMemory.buffer.getSample(channel, static_cast<int>(first)),
        audioMemory.buffer.getSample(channel, static_cast<int>(second)));
}

float EcosystemEngine::readAudioMemoryCrossfaded(
    int channel, double position, int64_t length,
    int crossfadeSamples) const noexcept
{
    if (length <= 0)
        return 0.0f;

    const auto safeLength = static_cast<double>(length);
    while (position < 0.0)
        position += safeLength;
    while (position >= safeLength)
        position -= safeLength;
    const auto endSample = readAudioMemorySample(channel, position, length);
    if (crossfadeSamples <= 0
        || position < static_cast<double>(length - crossfadeSamples))
        return endSample;

    const auto phase = juce::jlimit(0.0f, 1.0f, static_cast<float>(
        (position - static_cast<double>(length - crossfadeSamples))
        / static_cast<double>(crossfadeSamples)));
    const auto startPosition = position
                             - static_cast<double>(length - crossfadeSamples);
    const auto startSample = readAudioMemorySample(channel, startPosition,
                                                   length);
    return endSample * (1.0f - phase) + startSample * phase;
}

void EcosystemEngine::resetCosmosHeads() noexcept
{
    const auto length = static_cast<double>(audioMemory.loopLength);
    cosmosHeadPositions = { 0.0, length * 0.853 * 0.211,
                            length * 0.719 * 0.433,
                            length * 0.593 * 0.677 };
    cosmosModulationPhase = 0.0;
}

void EcosystemEngine::chooseMidiEvolution(MidiMemory& memory,
                                           int memoryIndex) noexcept
{
    auto next = LoopEvolution::normal;
    const auto validMemory = memoryIndex > bassLayerIndex
        && memoryIndex < midiMemoryCount;
    if (activeMidiEvolutionMemory == memoryIndex)
        activeMidiEvolutionMemory = -1;

    // At most one of the three memories grows an extra voice, and only the
    // first recorded line is shadowed. This keeps the evolution sparse and
    // avoids recreating the 24+ voice condition that used to crackle on Pi.
    if (loopEvolutionEnabled.load(std::memory_order_relaxed)
        && validMemory && activeMidiEvolutionMemory < 0
        && audioMemory.evolution == LoopEvolution::normal
        && memory.evolutionNote >= 0
        && nextEvolutionSample > 0
        && evolutionSampleClock >= nextEvolutionSample)
    {
        const auto random = nextEvolutionRandom(memory.evolutionRandomState);
        if (random % 100u < 55u && memory.evolutionNote <= 115)
            next = LoopEvolution::octaveUp;
        else
            next = LoopEvolution::reverse;
        if (next != LoopEvolution::normal)
        {
            activeMidiEvolutionMemory = memoryIndex;
            const auto cooldownSeconds = 18u + (random >> 8) % 18u;
            nextEvolutionSample = evolutionSampleClock
                + static_cast<int64_t>(std::round(sampleRate
                    * static_cast<double>(cooldownSeconds)));
        }
    }
    memory.evolution = next;
    memory.evolutionForDisplay.store(static_cast<int>(next),
                                     std::memory_order_relaxed);
}

void EcosystemEngine::chooseAudioEvolution() noexcept
{
    auto next = LoopEvolution::normal;
    audioEvolutionFilteredSamples.fill(0.0f);
    audioMemory.evolutionStartPosition = 0;
    audioMemory.evolutionDurationSamples = 0;
    audioMemory.evolutionSourcePosition = 0.0;
    if (loopEvolutionEnabled.load(std::memory_order_relaxed)
        && activeMidiEvolutionMemory < 0
        && audioMemory.loopLength > 0
        && nextEvolutionSample > 0
        && evolutionSampleClock >= nextEvolutionSample)
    {
        const auto random = nextEvolutionRandom(
            audioMemory.evolutionRandomState);
        if (random % 100u < 55u)
            next = LoopEvolution::octaveUp;
        else
            next = LoopEvolution::reverse;

        if (next != LoopEvolution::normal)
        {
            const auto desiredDuration = static_cast<int64_t>(std::round(
                sampleRate * (2.5 + static_cast<double>((random >> 8) % 350u)
                                         * 0.01)));
            // A +12 tape head consumes two source samples for every rendered
            // sample. Reverse consumes one. Keep the random fragment wholly
            // inside the recorded material so neither mode has to wrap at a
            // discontinuous loop edge.
            const auto maximumDuration = next == LoopEvolution::octaveUp
                ? juce::jmax<int64_t>(1,
                    (audioMemory.loopLength + 1) / 2)
                : audioMemory.loopLength;
            audioMemory.evolutionDurationSamples = juce::jmin(
                maximumDuration, juce::jmax<int64_t>(1, desiredDuration));
            // The global scheduler already makes the event unpredictable.
            // Starting at the wrap keeps ownership bounded to 2-6 seconds
            // even when RESPIRO itself lasts two minutes.
            audioMemory.evolutionStartPosition = 0;
            const auto sourceRandom = nextEvolutionRandom(
                audioMemory.evolutionRandomState);
            if (next == LoopEvolution::octaveUp)
            {
                const auto lastOffset = 2 * juce::jmax<int64_t>(
                    0, audioMemory.evolutionDurationSamples - 1);
                const auto possibleStarts = juce::jmax<int64_t>(
                    1, audioMemory.loopLength - lastOffset);
                audioMemory.evolutionSourcePosition = static_cast<double>(
                    sourceRandom % static_cast<std::uint32_t>(possibleStarts));
            }
            else
            {
                const auto minimumStart = juce::jmax<int64_t>(
                    0, audioMemory.evolutionDurationSamples - 1);
                const auto possibleStarts = juce::jmax<int64_t>(
                    1, audioMemory.loopLength - minimumStart);
                audioMemory.evolutionSourcePosition = static_cast<double>(
                    minimumStart + sourceRandom
                        % static_cast<std::uint32_t>(possibleStarts));
            }
            const auto cooldownSeconds = 18u + (random >> 8) % 18u;
            nextEvolutionSample = evolutionSampleClock
                + static_cast<int64_t>(std::round(sampleRate
                    * static_cast<double>(cooldownSeconds)));
        }
    }
    audioMemory.evolution = next;
    audioMemory.evolutionForDisplay.store(static_cast<int>(next),
                                          std::memory_order_relaxed);
}

void EcosystemEngine::renderMidiMemories(int numSamples, juce::MidiBuffer& output)
{
    for (int index = 1; index < midiMemoryCount; ++index)
    {
        auto& memory = midiMemories[static_cast<size_t>(index)];
        if (memory.recordingActive || memory.loopLength <= 0 || memory.events.empty())
            continue;

        auto remaining = numSamples;
        auto outputOffset = 0;
        while (remaining > 0)
        {
            const auto untilWrap = static_cast<int>(std::min<int64_t>(
                remaining, memory.loopLength - memory.playbackPosition));
            renderMidiSegment(memory, memory.playbackPosition, untilWrap,
                              outputOffset, output);
            renderMidiEvolutionSegment(memory, index, memory.playbackPosition,
                                       untilWrap, outputOffset, output);
            memory.playbackPosition += untilWrap;
            remaining -= untilWrap;
            outputOffset += untilWrap;

            if (memory.playbackPosition >= memory.loopLength)
            {
                if (memory.evolution != LoopEvolution::normal)
                    output.addEvent(juce::MidiMessage::allNotesOff(
                        evolutionMidiChannels[static_cast<std::size_t>(index)]),
                        juce::jlimit(0, numSamples - 1, outputOffset));
                memory.playbackPosition = 0;
                chooseMidiEvolution(memory, index);
            }
        }
        memory.phase.store(static_cast<double>(memory.playbackPosition)
                           / static_cast<double>(memory.loopLength));
    }
}

void EcosystemEngine::renderMidiEvolutionSegment(
    const MidiMemory& memory, int memoryIndex, int64_t segmentStart,
    int segmentLength,
    int outputOffset, juce::MidiBuffer& output)
{
    if (memory.evolution == LoopEvolution::normal
        || segmentLength <= 0 || memory.loopLength <= 0
        || memory.evolutionNote < 0
        || ! juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        return;

    const auto segmentEnd = segmentStart + segmentLength;
    const auto ghostChannel = evolutionMidiChannels[
        static_cast<std::size_t>(memoryIndex)];
    if (memory.evolution == LoopEvolution::octaveUp)
    {
        for (const auto& event : memory.events)
        {
            if (event.samplePosition < segmentStart)
                continue;
            if (event.samplePosition >= segmentEnd)
                break;

            const auto& message = event.message;
            if (! message.isNoteOnOrOff()
                || message.getNoteNumber() != memory.evolutionNote
                || message.getNoteNumber() > 115)
                continue;
            const auto transformed = message.isNoteOn()
                ? juce::MidiMessage::noteOn(
                    ghostChannel, message.getNoteNumber() + 12,
                    memory.evolutionVelocity)
                : juce::MidiMessage::noteOff(
                    ghostChannel, message.getNoteNumber() + 12);
            output.addEvent(transformed,
                outputOffset + static_cast<int>(
                    event.samplePosition - segmentStart));
        }
        return;
    }

    // Reversing raw note events would put note-offs before note-ons. Iterating
    // backward and swapping their roles mirrors each complete note interval.
    for (auto iterator = memory.events.rbegin();
         iterator != memory.events.rend(); ++iterator)
    {
        const auto reversePosition = memory.loopLength - 1
                                   - iterator->samplePosition;
        if (reversePosition < segmentStart)
            continue;
        if (reversePosition >= segmentEnd)
            break;

        const auto& message = iterator->message;
        if (! message.isNoteOnOrOff()
            || message.getNoteNumber() != memory.evolutionNote)
            continue;
        const auto transformed = message.isNoteOff()
            ? juce::MidiMessage::noteOn(ghostChannel,
                                        message.getNoteNumber(),
                                        memory.evolutionVelocity)
            : juce::MidiMessage::noteOff(ghostChannel,
                                         message.getNoteNumber());
        output.addEvent(transformed,
            outputOffset + static_cast<int>(reversePosition - segmentStart));
    }
}

void EcosystemEngine::renderMidiSegment(MidiMemory& memory, int64_t segmentStart,
                                         int segmentLength, int outputOffset,
                                         juce::MidiBuffer& output)
{
    const auto segmentEnd = segmentStart + segmentLength;
    for (const auto& event : memory.events)
    {
        if (event.samplePosition < segmentStart)
            continue;
        if (event.samplePosition >= segmentEnd)
            break;
        output.addEvent(event.message,
                        outputOffset + static_cast<int>(event.samplePosition - segmentStart));
    }
}

void EcosystemEngine::renderAudioMemory(
    const float* const* inputs, int inputChannels,
    float* const* outputs, int outputChannels, int numSamples)
{
    if (audioMemory.buffer.getNumSamples() == 0
        || saxRenderBuffer.getNumSamples() < numSamples)
        return;

    saxRenderBuffer.clear(0, numSamples);
    const auto maximumLength = static_cast<int64_t>(audioMemory.buffer.getNumSamples());
    const auto decay = audioDecay.load();
    const auto rightInputChannel = juce::jmin(1, inputChannels - 1);
    const auto useStereoInput = saxStereoInput.load();
    const auto pathMode = getSaxPathMode();
    const auto saxGestureIndex = midiMemoryCount;
    saxProcessor.setDelayLevel(juce::jmax(
        getDelayLevel(saxGestureIndex),
        echoThrowBlockAmounts[static_cast<std::size_t>(saxGestureIndex)]));
    saxProcessor.setFreezeEnabled(isFreezeEnabled(saxGestureIndex));
    const auto inputAllowed = pathMode != SaxPathMode::muted
        && ! saxSafetyMuted.load();

    const auto readInput = [inputs, inputChannels, rightInputChannel,
                             useStereoInput, inputAllowed](int channel, int sample)
    {
        if (! inputAllowed || inputs == nullptr || inputChannels <= 0)
            return 0.0f;
        const auto sourceChannel = useStereoInput && channel > 0
            ? rightInputChannel : 0;
        return juce::isPositiveAndBelow(sourceChannel, inputChannels)
                && inputs[sourceChannel] != nullptr
            ? inputs[sourceChannel][sample] : 0.0f;
    };

    constexpr std::array<double, 4> cosmosLengthRatios {
        1.0, 0.853, 0.719, 0.593
    };
    constexpr std::array<std::array<float, 4>, 2> cosmosWeights {{
        {{ 0.46f, 0.10f, 0.27f, 0.12f }},
        {{ 0.10f, 0.46f, 0.12f, 0.27f }}
    }};
    std::array<int64_t, 4> cosmosHeadLengths {};
    std::array<int, 4> cosmosCrossfades {};
    std::array<double, 4> cosmosWrappedSpans {};
    const auto renderFourHead = juce::jmax(fourHeadMixBlockStart,
                                            fourHeadMixBlockEnd) > 0.00001f;
    const auto evolutionFadeInLimit = static_cast<int64_t>(
        std::round(sampleRate * 0.40));
    const auto evolutionFadeOutLimit = static_cast<int64_t>(
        std::round(sampleRate * 1.0));
    bool closedInitialCaptureAtLimit = false;
    if (renderFourHead && audioMemory.loopLength > 0)
        for (std::size_t head = 0; head < cosmosHeadPositions.size(); ++head)
        {
            cosmosHeadLengths[head] = juce::jmax<int64_t>(
                16, static_cast<int64_t>(std::llround(
                    static_cast<double>(audioMemory.loopLength)
                    * cosmosLengthRatios[head])));
            cosmosCrossfades[head] = juce::jmin(
                static_cast<int>(cosmosHeadLengths[head] / 4),
                juce::jmax(4, static_cast<int>(std::round(
                    sampleRate * (0.010 + 0.003
                        * static_cast<double>(head))))));
            cosmosWrappedSpans[head] = juce::jmax(
                1.0, static_cast<double>(
                    cosmosHeadLengths[head] - cosmosCrossfades[head]));
            while (cosmosHeadPositions[head]
                   >= static_cast<double>(cosmosHeadLengths[head]))
                cosmosHeadPositions[head]
                    -= static_cast<double>(cosmosHeadLengths[head]);
        }

    for (int sample = 0; sample < numSamples; ++sample)
    {
        // Read the gain before advancing it: the first sample after a command
        // exactly matches the preceding callback. This also advances/finalises
        // a dissolve while the diagnostic path is DIRECT or MUTO.
        const auto loopPlaybackGain = audioMemory.playbackGain;
        const auto blockFraction = numSamples > 1
            ? static_cast<float>(sample) / static_cast<float>(numSamples - 1)
            : 1.0f;
        const auto fourHeadMix = juce::jlimit(
            0.0f, 1.0f,
            fourHeadMixBlockStart
                + (fourHeadMixBlockEnd - fourHeadMixBlockStart)
                    * blockFraction);

        if (pathMode == SaxPathMode::muted)
        {
            advanceAudioMemoryGainTransition();
            continue;
        }

        // The configured sax input is monitored with deliberate headroom.
        for (int channel = 0; channel < 2; ++channel)
            saxRenderBuffer.addSample(channel, sample,
                                      readInput(channel, sample) * 0.58f);

        // This path is deliberately just input -> output. It distinguishes a
        // capture/driver problem from loop and effects processing.
        if (pathMode == SaxPathMode::direct)
        {
            advanceAudioMemoryGainTransition();
            continue;
        }

        if (audioMemory.recordingActive && audioMemory.initialCapture)
        {
            if (audioMemory.writePosition >= maximumLength)
            {
                // Close exactly at the 120-second limit, but keep rendering the
                // live monitor and the rest of this callback. The old `break`
                // left the remainder of the hardware block at zero.
                audioMemory.recordingRequested.store(false);
                audioMemory.recordingActive = false;
                audioMemory.recordingForDisplay.store(false);
                finishInitialAudioCapture();
                closedInitialCaptureAtLimit = true;
            }
            else
            {
                for (int channel = 0;
                     channel < audioMemory.buffer.getNumChannels(); ++channel)
                {
                    const auto input = readInput(channel, sample);
                    audioMemory.buffer.setSample(channel,
                        static_cast<int>(audioMemory.writePosition), input * 0.72f);
                }
                ++audioMemory.writePosition;
                advanceAudioMemoryGainTransition();
                continue;
            }
        }

        // Four-head playback geometry was prepared before this block, while
        // the just-completed capture still had no loop. Start loop playback on
        // the next callback; monitoring remains continuous in this one.
        if (closedInitialCaptureAtLimit)
        {
            advanceAudioMemoryGainTransition();
            continue;
        }

        if (audioMemory.loopLength <= 0)
        {
            advanceAudioMemoryGainTransition();
            continue;
        }

        const auto bufferPosition = static_cast<int>(audioMemory.playbackPosition);
        const auto evolutionRelativePosition
            = audioMemory.playbackPosition
                - audioMemory.evolutionStartPosition;
        if (audioMemory.evolution != LoopEvolution::normal
            && audioMemory.evolutionDurationSamples > 0
            && evolutionRelativePosition
                >= audioMemory.evolutionDurationSamples)
        {
            audioMemory.evolution = LoopEvolution::normal;
            audioMemory.evolutionForDisplay.store(
                static_cast<int>(LoopEvolution::normal),
                std::memory_order_relaxed);
        }
        const auto renderEvolution
            = audioMemory.evolution != LoopEvolution::normal
                && evolutionRelativePosition >= 0
                && evolutionRelativePosition
                    < audioMemory.evolutionDurationSamples;
        auto evolutionGain = 0.0f;
        auto evolutionPlaybackRate = 0.0;
        auto evolutionPosition = audioMemory.evolutionSourcePosition;
        if (renderEvolution)
        {
            const auto duration = juce::jmax<int64_t>(
                1, audioMemory.evolutionDurationSamples);
            const auto fadeIn = juce::jmax<int64_t>(
                1, juce::jmin<int64_t>(duration / 3,
                                       evolutionFadeInLimit));
            const auto fadeOut = juce::jmax<int64_t>(
                1, juce::jmin<int64_t>(duration / 2,
                                       evolutionFadeOutLimit));
            const auto fadeInGain = juce::jlimit(
                0.0f, 1.0f, static_cast<float>(evolutionRelativePosition)
                    / static_cast<float>(fadeIn));
            const auto fadeOutGain = juce::jlimit(
                0.0f, 1.0f,
                static_cast<float>(duration - evolutionRelativePosition)
                    / static_cast<float>(fadeOut));
            evolutionGain = juce::jmin(fadeInGain, fadeOutGain);
            evolutionGain = evolutionGain * evolutionGain
                * (3.0f - 2.0f * evolutionGain);
            evolutionGain *= 0.16f * (1.0f - fourHeadMix);
            evolutionPlaybackRate
                = audioMemory.evolution == LoopEvolution::octaveUp
                    ? 2.0 : -1.0;
        }
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto memoryChannel = channel;
            auto loopSample = audioMemory.buffer.getSample(memoryChannel, bufferPosition);

            if (audioMemory.recordingActive)
            {
                const auto input = readInput(memoryChannel, sample);
                // The former normalised tanh amplified the stored loop on
                // every silent pass. Retention is now strictly below unity;
                // protection only starts near full scale.
                const auto overdub = loopSample * juce::jmin(decay, 0.995f)
                                   + input * 0.22f;
                loopSample = protectPeak(overdub, 0.88f, 0.98f);
                audioMemory.buffer.setSample(memoryChannel, bufferPosition,
                                             loopSample);
            }

            if (renderFourHead)
            {
                auto fourHeadSample = 0.0f;
                for (std::size_t head = 0; head < cosmosHeadPositions.size();
                     ++head)
                {
                    fourHeadSample += readAudioMemoryCrossfaded(
                        memoryChannel, cosmosHeadPositions[head],
                        cosmosHeadLengths[head], cosmosCrossfades[head])
                        * cosmosWeights[static_cast<std::size_t>(channel)][head];
                }
                fourHeadSample *= 0.72f;
                loopSample += (fourHeadSample - loopSample) * fourHeadMix;
            }

            if (renderEvolution)
            {
                auto evolutionSample = 0.0f;
                if (audioMemory.evolution == LoopEvolution::octaveUp)
                {
                    // A five-tap binomial low-pass runs on the source before
                    // the 2:1 tape read. Unlike an output high-cut, this
                    // suppresses frequencies that would otherwise fold back
                    // into the audible band during the octave-up decimation.
                    constexpr std::array<float, 5> antiAliasWeights {
                        1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f,
                        4.0f / 16.0f, 1.0f / 16.0f
                    };
                    const auto centre = static_cast<int64_t>(
                        std::llround(evolutionPosition));
                    for (int tap = -2; tap <= 2; ++tap)
                    {
                        const auto source = juce::jlimit<int64_t>(
                            0, audioMemory.loopLength - 1,
                            centre + static_cast<int64_t>(tap));
                        evolutionSample += audioMemory.buffer.getSample(
                            memoryChannel, static_cast<int>(source))
                            * antiAliasWeights[static_cast<std::size_t>(tap + 2)];
                    }
                    auto& filtered = audioEvolutionFilteredSamples[
                        static_cast<std::size_t>(channel)];
                    filtered += audioEvolutionLowPassCoefficient
                        * (evolutionSample - filtered);
                    evolutionSample = filtered;
                }
                else
                    evolutionSample = readAudioMemorySample(
                        memoryChannel, evolutionPosition,
                        audioMemory.loopLength);
                loopSample += evolutionSample * evolutionGain;
            }
            saxRenderBuffer.addSample(channel, sample,
                                      loopSample * 0.72f * loopPlaybackGain);
        }

        if (renderEvolution)
        {
            audioMemory.evolutionSourcePosition = juce::jlimit(
                0.0,
                static_cast<double>(juce::jmax<int64_t>(
                    0, audioMemory.loopLength - 1)),
                audioMemory.evolutionSourcePosition
                    + evolutionPlaybackRate);
        }

        ++audioMemory.playbackPosition;
        if (audioMemory.playbackPosition >= audioMemory.loopLength)
        {
            audioMemory.playbackPosition = 0;
            chooseAudioEvolution();
        }
        if (renderFourHead)
        {
            const auto modulationSin = static_cast<double>(
                CommentoDsp::fastSine(cosmosModulationPhase));
            const auto modulationCos = static_cast<double>(
                CommentoDsp::fastCosine(cosmosModulationPhase));
            const std::array<double, 4> modulation {
                modulationSin, modulationCos,
                -modulationSin, -modulationCos
            };
            for (std::size_t head = 0; head < cosmosHeadPositions.size(); ++head)
            {
                const auto drift = 1.0 + 0.0017 * modulation[head];
                cosmosHeadPositions[head] += drift;
                while (cosmosHeadPositions[head]
                       >= static_cast<double>(cosmosHeadLengths[head]))
                    cosmosHeadPositions[head] -= cosmosWrappedSpans[head];
            }
            cosmosModulationPhase += juce::MathConstants<double>::twoPi
                                   * 0.017 / sampleRate;
            if (cosmosModulationPhase
                >= juce::MathConstants<double>::twoPi)
                cosmosModulationPhase
                    -= juce::MathConstants<double>::twoPi;
        }
        advanceAudioMemoryGainTransition();
    }

    if (pathMode == SaxPathMode::sceneEffects)
    {
        saxProcessor.process(saxRenderBuffer, numSamples);
    }
    else
        saxProcessor.advanceMorph(numSamples);
    performanceLevels.process(midiMemoryCount, saxRenderBuffer, numSamples);
    const auto safetyTarget = saxSafetyMuted.load()
            || pathMode == SaxPathMode::muted ? 0.0f : 1.0f;
    const auto safetyTime = safetyTarget < saxSafetyGain ? 0.006 : 0.25;
    const auto safetyCoefficient = static_cast<float>(
        1.0 - std::exp(-1.0 / (safetyTime * sampleRate)));
    for (int sample = 0; sample < numSamples; ++sample)
    {
        saxSafetyGain += (safetyTarget - saxSafetyGain) * safetyCoefficient;
        saxRenderBuffer.setSample(0, sample,
            saxRenderBuffer.getSample(0, sample) * saxSafetyGain);
        saxRenderBuffer.setSample(1, sample,
            saxRenderBuffer.getSample(1, sample) * saxSafetyGain);
    }
    if (outputChannels > saxRightBus)
    {
        if (outputs[saxLeftBus] != nullptr)
            juce::FloatVectorOperations::add(outputs[saxLeftBus],
                saxRenderBuffer.getReadPointer(0), numSamples);
        if (outputs[saxRightBus] != nullptr)
            juce::FloatVectorOperations::add(outputs[saxRightBus],
                saxRenderBuffer.getReadPointer(1), numSamples);
    }

    if (audioMemory.recordingActive && audioMemory.initialCapture)
    {
        audioMemory.phase.store(static_cast<double>(audioMemory.writePosition)
                                / static_cast<double>(maximumLength));
        audioMemory.lengthSeconds.store(
            static_cast<double>(audioMemory.writePosition) / sampleRate);
    }
    else if (audioMemory.loopLength > 0)
    {
        audioMemory.phase.store(static_cast<double>(audioMemory.playbackPosition)
                                / static_cast<double>(audioMemory.loopLength));
    }
}

void EcosystemEngine::renderDiagnosticTone(float* const* outputs,
                                           int outputChannels,
                                           int numSamples)
{
    const auto destination = getDiagnosticToneBus();
    if (destination == DiagnosticToneBus::off || outputs == nullptr
        || numSamples <= 0)
    {
        diagnosticTonePhase = 0.0;
        return;
    }

    // A diagnostic tone must be unambiguous: silence every musical bus first,
    // then write only the selected destination. This also makes capture-on vs
    // capture-off comparisons possible while loops keep their state.
    for (int channel = 0; channel < outputChannels; ++channel)
        if (outputs[channel] != nullptr)
            juce::FloatVectorOperations::clear(outputs[channel], numSamples);

    constexpr auto frequency = 997.0;
    constexpr auto level = 0.06309573f; // -24 dBFS
    const auto increment = juce::MathConstants<double>::twoPi
                         * frequency / sampleRate;
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto value = CommentoDsp::fastSine(diagnosticTonePhase) * level;
        const auto add = [outputs, outputChannels, sample, value](int bus)
        {
            if (juce::isPositiveAndBelow(bus, outputChannels)
                && outputs[bus] != nullptr)
                outputs[bus][sample] = value;
        };

        if (destination == DiagnosticToneBus::ambient)
        {
            add(ambientLeftBus);
            add(ambientRightBus);
        }
        else if (destination == DiagnosticToneBus::bass)
            add(bassBus);
        else if (destination == DiagnosticToneBus::sax)
        {
            add(saxLeftBus);
            add(saxRightBus);
        }

        diagnosticTonePhase += increment;
        if (diagnosticTonePhase >= juce::MathConstants<double>::twoPi)
            diagnosticTonePhase -= juce::MathConstants<double>::twoPi;
    }
}
