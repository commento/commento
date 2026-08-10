#include "EcosystemEngine.h"

#include <algorithm>
#include <cmath>

#if JUCE_LINUX
 #include <pthread.h>
 #include <sched.h>
#endif

namespace
{
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

[[nodiscard]] SaxPatch makeSaxKeyboardTreatment(SaxPatch patch) noexcept
{
    // A moving sax phrase multiplied into a chord becomes metallic very
    // quickly. Keep the real sax treatment expansive, but give its keyboard
    // instrument a shorter, darker and almost-linear space of its own.
    patch.toneHz = juce::jlimit(2600.0f, 5200.0f, patch.toneHz);
    patch.drive = juce::jlimit(1.0f, 1.025f, patch.drive);
    patch.delayMilliseconds = juce::jlimit(
        260.0f, 620.0f, patch.delayMilliseconds * 0.55f);
    patch.delaySpread = 1.23f;
    patch.feedback = juce::jlimit(0.0f, 0.24f, patch.feedback * 0.48f);
    patch.crossFeedback = juce::jlimit(
        0.0f, 0.42f, patch.crossFeedback * 0.48f);
    patch.delayMix = juce::jlimit(0.0f, 0.24f, patch.delayMix * 0.62f);
    patch.modulationRateHz *= 0.70f;
    patch.modulationDepthMilliseconds = juce::jmin(
        1.2f, patch.modulationDepthMilliseconds * 0.42f);
    patch.reverbSize = juce::jmin(0.82f, patch.reverbSize);
    patch.reverbDamping = juce::jmax(0.62f, patch.reverbDamping);
    patch.reverbWet = juce::jlimit(0.0f, 0.24f, patch.reverbWet * 0.68f);
    patch.tremoloDepth *= 0.30f;
    patch.outputGain = juce::jlimit(0.52f, 0.68f, patch.outputGain * 1.12f);
    return patch;
}
}

EcosystemEngine::EcosystemEngine()
{
    for (auto& delayLevel : delayLevels)
        delayLevel.store(1.0f, std::memory_order_relaxed);

    const auto& initialScenario = CommentoScenarios::get(0);
    for (int index = 0; index < midiMemoryCount; ++index)
    {
        midiMemories[static_cast<size_t>(index)].events.reserve(maximumMidiEvents + 128);
        internalSynths[static_cast<size_t>(index)] = std::make_unique<AmbientSynth>(index);
        internalSynths[static_cast<size_t>(index)]->setPatch(
            initialScenario.layers[static_cast<size_t>(index)]);
    }
    saxProcessor.setPatch(initialScenario.sax);
    saxLoopKeyboardProcessor.setPatch(
        makeSaxKeyboardTreatment(initialScenario.sax));
    activeSaxLoopKeyboardPatch = initialScenario.saxLoopKeyboard;
    saxLoopKeyboardModeActive = initialScenario.saxLoopKeyboard.enabled;
    audioDecay.store(initialScenario.sax.loopDecay);
    activeScenario.store(0);
}

void EcosystemEngine::enqueueMidiMessage(const juce::MidiMessage& message)
{
    if (message.isActiveSense() || message.isMidiClock())
        return;

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
        requested.store(! requested.load());
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

bool EcosystemEngine::isSaxLoopKeyboardEnabled() const
{
    return CommentoScenarios::get(requestedScenario.load())
        .saxLoopKeyboard.enabled;
}

void EcosystemEngine::setScenarioIndex(int index)
{
    requestedScenario.store(CommentoScenarios::wrapIndex(index));
}

int EcosystemEngine::getScenarioIndex() const
{
    return requestedScenario.load();
}

void EcosystemEngine::setTextureAmount(float amount)
{
    requestedTexture.store(juce::jlimit(0.0f, 1.0f, amount));
}

float EcosystemEngine::getTextureAmount() const
{
    return requestedTexture.load();
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
    dspLoad.store(0.0f, std::memory_order_relaxed);
    dspNearOverloadCount.store(0, std::memory_order_relaxed);
    dspWarmupCallbacksRemaining = 8;
    previousMidiCallbackTimeSeconds
        = juce::Time::getMillisecondCounterHiRes() * 0.001;
    for (int index = 0; index < granularWindowSize; ++index)
        saxLoopGranularWindow[static_cast<std::size_t>(index)]
            = 0.5f - 0.5f * std::cos(
                juce::MathConstants<float>::twoPi
                * static_cast<float>(index)
                / static_cast<float>(granularWindowSize));
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
        audioMemory.phase.store(0.0);
        audioMemory.lengthSeconds.store(0.0);
        audioMemory.buffer.setSize(2, maximumSamples, false, true, false);
        audioMemory.buffer.clear();
    }

    const auto renderCapacity = juce::jmax(8192, maximumBlockSize);
    ambientSynthBuffer.setSize(2, renderCapacity, false, true, false);
    bassSynthBuffer.setSize(2, renderCapacity, false, true, false);
    layerSynthBuffer.setSize(2, renderCapacity, false, true, false);
    saxLoopTailBuffer.setSize(2, renderCapacity, false, true, false);
    saxRenderBuffer.setSize(2, renderCapacity, false, true, false);
    performanceLevels.prepare(sampleRate);
    bassMuteGain.reset(sampleRate, 0.006);
    bassMuteGain.setCurrentAndTargetValue(bassEnabled.load() ? 1.0f : 0.0f);
    bassWasEnabled = bassEnabled.load();
    blockMidiOutput.ensureSize(128 * 1024);
    for (auto& midi : layerMidiBuffers)
        midi.ensureSize(64 * 1024);
    for (int index = 0; index < midiMemoryCount; ++index)
    {
        auto& synth = internalSynths[static_cast<std::size_t>(index)];
        synth->setDelayLevel(getDelayLevel(index));
        synth->prepare(sampleRate, maximumBlockSize);
    }
    saxProcessor.setDelayLevel(getDelayLevel(midiMemoryCount));
    saxProcessor.prepare(sampleRate, maximumBlockSize);
    saxLoopKeyboardProcessor.setDelayLevel(getDelayLevel(bassLayerIndex));
    saxLoopKeyboardProcessor.prepare(sampleRate, maximumBlockSize);
    resetSaxLoopKeyboardVoices();
    resetCosmosHeads();
    saxLoopLastOutput.fill(0.0f);
    saxLoopClearFadeOffset.fill(0.0f);
    saxLoopClearFadeSamplesRemaining = 0;
    saxLoopClearFadeSamplesTotal = 0;
    channelOneLastOutput.fill(0.0f);
    channelOneTransitionOffset.fill(0.0f);
    channelOneTransitionSamplesRemaining = 0;
    channelOneTransitionSamplesTotal = 0;

    // Force the requested scene and texture back onto freshly prepared DSP;
    // prepare may run again after reconnecting the Model 12.
    activeScenario.store(-1);
    activeTexture = -1.0f;
    applyScenarioIfNeeded();
}

void EcosystemEngine::audioDeviceStopped()
{
    audioRunning.store(false);
    realtimeSchedulingStatus.store(-1, std::memory_order_relaxed);
    dspLoad.store(0.0f, std::memory_order_relaxed);
    dspNearOverloadCount.store(0, std::memory_order_relaxed);
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
    audioMemory.recordingRequested.store(false);
    audioMemory.recordingActive = false;
    audioMemory.initialCapture = false;
    audioMemory.recordingForDisplay.store(false);
    resetSaxLoopKeyboardVoices();
    resetCosmosHeads();
}

void EcosystemEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    const auto callbackStart = juce::Time::getHighResolutionTicks();
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

    blockMidiOutput.clear();
    if (midiOverflowed.exchange(false))
        for (const auto channel : midiChannels)
            blockMidiOutput.addEvent(juce::MidiMessage::allNotesOff(channel), 0);

    applyScenarioIfNeeded();
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
    const auto desired = CommentoScenarios::wrapIndex(requestedScenario.load());
    const auto texture = juce::jlimit(0.0f, 1.0f, requestedTexture.load());
    const auto scenarioChanged = desired != activeScenario.load();
    if (desired == activeScenario.load()
        && std::abs(texture - activeTexture) < 0.0001f)
        return;

    const auto& scenario = CommentoScenarios::get(desired);
    const auto saxKeyboardWasEnabled = saxLoopKeyboardModeActive;
    const auto saxKeyboardWillBeEnabled = scenario.saxLoopKeyboard.enabled;
    for (int index = 0; index < midiMemoryCount; ++index)
        internalSynths[static_cast<size_t>(index)]->setPatch(
            applyTexture(scenario.layers[static_cast<size_t>(index)], texture));
    const auto texturedSax = applyTexture(scenario.sax, texture);
    saxProcessor.setPatch(texturedSax);
    if (saxKeyboardWillBeEnabled)
    {
        saxLoopKeyboardProcessor.setPatch(
            makeSaxKeyboardTreatment(texturedSax));
        activeSaxLoopKeyboardPatch = scenario.saxLoopKeyboard;
    }
    if (scenarioChanged
        && saxKeyboardWasEnabled != saxKeyboardWillBeEnabled)
    {
        if (saxKeyboardWillBeEnabled)
        {
            // The old bass source disappears at this boundary. Its final
            // sample bridges the short gap, except when a sampler tail is
            // already continuing and would otherwise be counted twice.
            if (! saxLoopKeyboardTailActive
                && ! saxLoopKeyboardProcessor.isIncrementalTailResetActive())
            {
                channelOneTransitionOffset = channelOneLastOutput;
                channelOneTransitionSamplesRemaining
                    = channelOneTransitionSamplesTotal
                    = juce::jmax(1, static_cast<int>(sampleRate * 0.008));
            }
            else
            {
                channelOneTransitionOffset.fill(0.0f);
                channelOneTransitionSamplesRemaining = 0;
                channelOneTransitionSamplesTotal = 0;
            }
            internalSynths[static_cast<size_t>(bassLayerIndex)]->allNotesOff();
            if (! saxLoopKeyboardTailActive)
                resetSaxLoopKeyboardVoices();
            else
            {
                saxLoopKeyboardTailActive = false;
                saxLoopKeyboardTailSamplesRemaining = 0;
            }
        }
        else
        {
            // The sampler itself keeps rendering its release and FX tail, so
            // adding the generic last-sample bridge here would double it.
            channelOneTransitionOffset.fill(0.0f);
            channelOneTransitionSamplesRemaining = 0;
            channelOneTransitionSamplesTotal = 0;
            beginSaxLoopKeyboardFadeOut();
        }
        resetCosmosHeads();
    }
    saxLoopKeyboardModeActive = saxKeyboardWillBeEnabled;
    if (scenarioChanged)
        audioDecay.store(scenario.sax.loopDecay);
    activeScenario.store(desired);
    activeTexture = texture;
}

void EcosystemEngine::applyMidiCommands(MidiMemory& memory, int channel,
                                         juce::MidiBuffer& output)
{
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
        output.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
    }

    const auto shouldRecord = memory.recordingRequested.load();
    if (shouldRecord == memory.recordingActive)
        return;

    memory.recordingActive = shouldRecord;
    memory.recordingForDisplay.store(shouldRecord);
    memory.waitingForFirstNote = shouldRecord;
    memory.waitingForFirstNoteForDisplay.store(shouldRecord);
    output.addEvent(juce::MidiMessage::allNotesOff(channel), 0);

    if (shouldRecord)
    {
        memory.events.clear();
        memory.recordPosition = 0;
        memory.playbackPosition = 0;
        memory.loopLength = 0;
        memory.containsMaterial.store(false);
        memory.eventCount.store(0);
        memory.lengthSeconds.store(0.0);
        memory.phase.store(0.0);
        memory.activeRecordedNotes.fill(false);
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
    }
}

void EcosystemEngine::applyAudioCommands()
{
    if (audioMemory.clearRequested.exchange(false))
    {
        beginSaxLoopKeyboardFadeOut(true);
        audioMemory.recordingRequested.store(false);
        audioMemory.recordingActive = false;
        audioMemory.recordingForDisplay.store(false);
        audioMemory.initialCapture = false;
        audioMemory.writePosition = 0;
        audioMemory.playbackPosition = 0;
        audioMemory.loopLength = 0;
        audioMemory.containsMaterial.store(false);
        audioMemory.lengthSeconds.store(0.0);
        audioMemory.phase.store(0.0);
        resetCosmosHeads();
    }

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
            audioMemory.containsMaterial.store(false);
            audioMemory.lengthSeconds.store(0.0);
            resetSaxLoopKeyboardVoices();
            resetCosmosHeads();
        }
    }
    else if (audioMemory.initialCapture)
    {
        audioMemory.loopLength = audioMemory.writePosition;
        audioMemory.playbackPosition = 0;
        audioMemory.initialCapture = false;
        const auto usable = audioMemory.loopLength > static_cast<int64_t>(sampleRate * 0.05);
        audioMemory.containsMaterial.store(usable);
        audioMemory.lengthSeconds.store(usable
            ? static_cast<double>(audioMemory.loopLength) / sampleRate : 0.0);
        if (! usable)
            audioMemory.loopLength = 0;
        resetSaxLoopKeyboardVoices();
        resetCosmosHeads();
    }
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
                    memory.activeRecordedNotes[static_cast<size_t>(message.getNoteNumber())] = true;
                else if (message.isNoteOff())
                    memory.activeRecordedNotes[static_cast<size_t>(message.getNoteNumber())] = false;
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
        || layerSynthBuffer.getNumSamples() < numSamples
        || saxLoopTailBuffer.getNumSamples() < numSamples)
        return;
    ambientSynthBuffer.clear(0, numSamples);
    bassSynthBuffer.clear(0, numSamples);
    const juce::MidiBuffer emptyMidi;

    for (int layer = 0; layer < midiMemoryCount; ++layer)
    {
        auto& layerMidi = layerMidiBuffers[static_cast<size_t>(layer)];
        layerMidi.clear();
        for (const auto metadata : midi)
            if (metadata.getMessage().getChannel()
                == midiChannels[static_cast<size_t>(layer)])
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
        const auto isSaxKeyboardLayer = layer == bassLayerIndex
            && saxLoopKeyboardModeActive;
        if (isSaxKeyboardLayer)
            renderSaxLoopKeyboard(layerSynthBuffer, layerMidi, numSamples);
        else
        {
            internalSynths[static_cast<size_t>(layer)]->setDelayLevel(
                getDelayLevel(layer));
            internalSynths[static_cast<size_t>(layer)]->render(
                layerSynthBuffer, layerMidi, 0, numSamples);
        }
        if (layer == bassLayerIndex && ! isSaxKeyboardLayer
            && (saxLoopKeyboardTailActive
                || saxLoopKeyboardProcessor.isIncrementalTailResetActive()))
        {
            saxLoopTailBuffer.clear(0, numSamples);
            renderSaxLoopKeyboard(saxLoopTailBuffer, emptyMidi, numSamples);
            if (saxLoopKeyboardTailActive)
            {
                const auto fadeLength = juce::jmax<int64_t>(
                    1, static_cast<int64_t>(sampleRate * 0.050));
                const auto startGain = juce::jlimit(
                    0.0f, 1.0f,
                    static_cast<float>(saxLoopKeyboardTailSamplesRemaining)
                        / static_cast<float>(fadeLength));
                const auto endGain = juce::jlimit(
                    0.0f, 1.0f,
                    static_cast<float>(saxLoopKeyboardTailSamplesRemaining
                                       - numSamples)
                        / static_cast<float>(fadeLength));
                for (int channel = 0; channel < 2; ++channel)
                {
                    saxLoopTailBuffer.applyGainRamp(
                        channel, 0, numSamples, startGain, endGain);
                    layerSynthBuffer.addFrom(channel, 0, saxLoopTailBuffer,
                                             channel, 0, numSamples);
                }
                saxLoopKeyboardTailSamplesRemaining -= numSamples;
            }
            if (saxLoopKeyboardTailActive
                && saxLoopKeyboardTailSamplesRemaining <= 0)
            {
                resetSaxLoopKeyboardVoices();
                saxLoopKeyboardProcessor.beginIncrementalTailReset();
            }
        }
        if (layer == bassLayerIndex && saxLoopClearFadeSamplesRemaining > 0
            && saxLoopClearFadeSamplesTotal > 0)
        {
            for (int sample = 0; sample < numSamples; ++sample)
            {
                if (saxLoopClearFadeSamplesRemaining <= 0)
                    break;
                const auto gain = static_cast<float>(
                    saxLoopClearFadeSamplesRemaining)
                    / static_cast<float>(saxLoopClearFadeSamplesTotal);
                for (int channel = 0; channel < 2; ++channel)
                    layerSynthBuffer.addSample(
                        channel, sample,
                        saxLoopClearFadeOffset[static_cast<std::size_t>(channel)]
                            * gain);
                --saxLoopClearFadeSamplesRemaining;
            }
        }
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

    if (channelOneTransitionSamplesRemaining > 0
        && channelOneTransitionSamplesTotal > 0)
        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (channelOneTransitionSamplesRemaining <= 0)
                break;
            const auto gain = static_cast<float>(
                channelOneTransitionSamplesRemaining)
                / static_cast<float>(channelOneTransitionSamplesTotal);
            for (int channel = 0; channel < 2; ++channel)
                bassSynthBuffer.addSample(
                    channel, sample,
                    channelOneTransitionOffset[static_cast<std::size_t>(channel)]
                        * gain);
            --channelOneTransitionSamplesRemaining;
        }

    if (numSamples > 0)
        for (int channel = 0; channel < 2; ++channel)
            channelOneLastOutput[static_cast<std::size_t>(channel)]
                = bassSynthBuffer.getSample(channel, numSamples - 1);

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

void EcosystemEngine::renderSaxLoopKeyboard(juce::AudioBuffer<float>& output,
                                             const juce::MidiBuffer& midi,
                                             int numSamples)
{
    if (saxLoopKeyboardProcessor.isIncrementalTailResetActive())
    {
        saxLoopKeyboardProcessor.process(output, numSamples);
        saxLoopLastOutput.fill(0.0f);
        return;
    }

    const auto loopLength = audioMemory.loopLength;
    const auto crossfadeSamples = loopLength > 16
        ? juce::jmin(static_cast<int>(loopLength / 4),
                     juce::jmax(4, static_cast<int>(
                         std::round(sampleRate * 0.012))))
        : 0;
    const auto attackStep = 1.0f / juce::jmax(
        1.0f, activeSaxLoopKeyboardPatch.attackSeconds
              * static_cast<float>(sampleRate));
    const auto releaseStep = 1.0f / juce::jmax(
        1.0f, activeSaxLoopKeyboardPatch.releaseSeconds
              * static_cast<float>(sampleRate));
    const auto grainLength = getSaxLoopGrainLengthSamples();
    const auto halfGrainLength = grainLength / 2;
    const auto wrappedSpan = juce::jmax(
        1.0, static_cast<double>(loopLength - crossfadeSamples));
    const auto pitchSmoothing = 1.0 - std::exp(
        -1.0 / juce::jmax(1.0, sampleRate * 0.005));
    const auto velocitySmoothing = 1.0f - std::exp(
        -1.0f / juce::jmax(1.0f, static_cast<float>(sampleRate * 0.008)));
    const auto polyphonyAttenuationSmoothing = 1.0f - std::exp(
        -1.0f / juce::jmax(1.0f, static_cast<float>(sampleRate * 0.002)));
    const auto polyphonyRecoverySmoothing = 1.0f - std::exp(
        -1.0f / juce::jmax(1.0f, static_cast<float>(sampleRate * 0.024)));

    const auto renderVoices = [this, &output, loopLength, crossfadeSamples,
                               attackStep, releaseStep, grainLength,
                               halfGrainLength, wrappedSpan, pitchSmoothing,
                               velocitySmoothing,
                               polyphonyAttenuationSmoothing,
                               polyphonyRecoverySmoothing](int start, int length)
    {
        for (int sample = start; sample < start + length; ++sample)
        {
            auto activeVoiceCount = 0;
            for (const auto& voice : saxLoopVoices)
                activeVoiceCount += voice.active ? 1 : 0;
            const auto targetPolyphonyGain = 1.0f / std::sqrt(
                static_cast<float>(juce::jmax(1, activeVoiceCount)));
            const auto smoothing = targetPolyphonyGain < saxLoopPolyphonyGain
                ? polyphonyAttenuationSmoothing
                : polyphonyRecoverySmoothing;
            saxLoopPolyphonyGain += smoothing
                * (targetPolyphonyGain - saxLoopPolyphonyGain);

            for (auto& voice : saxLoopVoices)
            {
                if (! voice.active)
                    continue;

                if (voice.releasing)
                {
                    voice.envelope = juce::jmax(0.0f,
                                                voice.envelope - releaseStep);
                    if (voice.envelope <= 0.0f)
                    {
                        voice = {};
                        continue;
                    }
                }
                else
                    voice.envelope = juce::jmin(1.0f,
                                                voice.envelope + attackStep);

                voice.pitchRatio += pitchSmoothing
                    * (voice.targetPitchRatio - voice.pitchRatio);
                voice.velocity += velocitySmoothing
                    * (voice.targetVelocity - voice.velocity);

                const auto voiceGain = voice.envelope * voice.velocity
                                     * activeSaxLoopKeyboardPatch.level
                                     * saxLoopPolyphonyGain;
                std::array<float, 2> grainWeights {};
                for (std::size_t grainIndex = 0;
                     grainIndex < voice.grains.size(); ++grainIndex)
                {
                    const auto windowIndex = juce::jlimit(
                        0, granularWindowSize - 1,
                        voice.grains[grainIndex].age * granularWindowSize
                            / grainLength);
                    grainWeights[grainIndex] = saxLoopGranularWindow[
                        static_cast<std::size_t>(windowIndex)];
                }
                for (int channel = 0; channel < 2; ++channel)
                {
                    auto& filtered = voice.filterState[
                        static_cast<std::size_t>(channel)];
                    if (loopLength > 1)
                    {
                        auto source = 0.0f;
                        for (std::size_t grainIndex = 0;
                             grainIndex < voice.grains.size(); ++grainIndex)
                            source += readAudioMemoryCrossfaded(
                                channel,
                                voice.grains[grainIndex].readPosition,
                                loopLength, crossfadeSamples)
                                * grainWeights[grainIndex];
                        filtered = (1.0f - voice.playbackFilterPole) * source
                                 + voice.playbackFilterPole * filtered;
                    }
                    auto renderedSample = loopLength > 1
                        ? filtered * voiceGain : 0.0f;
                    if (voice.declickSamplesRemaining > 0
                        && voice.declickSamplesTotal > 0)
                        renderedSample += voice.declickOffset[
                            static_cast<std::size_t>(channel)]
                            * static_cast<float>(voice.declickSamplesRemaining)
                            / static_cast<float>(voice.declickSamplesTotal);
                    output.addSample(channel, sample, renderedSample);
                }
                if (voice.declickSamplesRemaining > 0)
                    --voice.declickSamplesRemaining;

                if (loopLength > 1)
                {
                    voice.transportPosition += 1.0;
                    while (voice.transportPosition
                           >= static_cast<double>(loopLength))
                        voice.transportPosition -= wrappedSpan;

                    for (auto& grain : voice.grains)
                    {
                        grain.readPosition += voice.pitchRatio;
                        grain.readPosition = wrapSaxLoopReadPosition(
                            grain.readPosition, loopLength,
                            crossfadeSamples);
                        ++grain.age;
                        if (grain.age >= grainLength)
                        {
                            grain.age = 0;
                            grain.readPosition = wrapSaxLoopReadPosition(
                                voice.transportPosition
                                    + (1.0 - voice.pitchRatio)
                                        * static_cast<double>(
                                            halfGrainLength),
                                loopLength, crossfadeSamples);
                        }
                    }
                }
            }
        }
    };

    auto rendered = 0;
    for (const auto metadata : midi)
    {
        const auto eventPosition = juce::jlimit(
            rendered, numSamples, metadata.samplePosition);
        renderVoices(rendered, eventPosition - rendered);
        handleSaxLoopKeyboardMessage(metadata.getMessage());
        rendered = eventPosition;
    }
    renderVoices(rendered, numSamples - rendered);

    saxLoopKeyboardProcessor.setDelayLevel(getDelayLevel(bassLayerIndex));
    saxLoopKeyboardProcessor.process(output, numSamples);
    if (numSamples > 0)
        for (int channel = 0; channel < 2; ++channel)
            saxLoopLastOutput[static_cast<std::size_t>(channel)]
                = output.getSample(channel, numSamples - 1);
}

void EcosystemEngine::handleSaxLoopKeyboardMessage(
    const juce::MidiMessage& message)
{
    if (message.isNoteOn())
    {
        if (audioMemory.loopLength <= 1)
            return;

        const auto note = juce::jlimit(0, 127, message.getNoteNumber());
        const auto noteAge = ++saxLoopVoiceAge;
        auto& heldCount = saxLoopHeldNoteCounts[static_cast<std::size_t>(note)];
        if (heldCount < 255)
            ++heldCount;
        saxLoopHeldVelocities[static_cast<std::size_t>(note)]
            = message.getFloatVelocity();
        saxLoopHeldAges[static_cast<std::size_t>(note)] = noteAge;

        // SAX TASTIERA is a monophonic, last-note instrument. During legato,
        // keep both grains and the envelope running and glide only their read
        // ratio; restarting the captured phrase at every key was one of the
        // main sources of the metallic/cacophonic result.
        auto& legatoVoice = saxLoopVoices.front();
        if (legatoVoice.active && ! legatoVoice.releasing)
        {
            legatoVoice.keyDown = true;
            legatoVoice.midiNote = note;
            legatoVoice.targetVelocity = message.getFloatVelocity();
            legatoVoice.age = noteAge;
            updateSaxLoopVoicePitches();
            return;
        }

        SaxLoopVoice* chosen = nullptr;
        for (auto& voice : saxLoopVoices)
        {
            if (! voice.active)
            {
                chosen = &voice;
                break;
            }
        }

        if (chosen == nullptr)
            chosen = &*std::min_element(
                saxLoopVoices.begin(), saxLoopVoices.end(),
                [](const SaxLoopVoice& left, const SaxLoopVoice& right)
                {
                    if (left.releasing != right.releasing)
                        return left.releasing;
                    if (std::abs(left.envelope - right.envelope) > 0.0001f)
                        return left.envelope < right.envelope;
                    return left.age < right.age;
                });

        std::array<float, 2> stolenResidual {};
        if (chosen->active)
        {
            for (std::size_t channel = 0; channel < stolenResidual.size();
                 ++channel)
                stolenResidual[channel] = chosen->filterState[channel]
                    * chosen->envelope * chosen->velocity
                    * activeSaxLoopKeyboardPatch.level
                    * saxLoopPolyphonyGain;
        }

        *chosen = {};
        chosen->active = true;
        chosen->keyDown = true;
        chosen->midiNote = note;
        chosen->velocity = message.getFloatVelocity();
        chosen->targetVelocity = chosen->velocity;
        chosen->age = noteAge;
        chosen->declickOffset = stolenResidual;
        if (stolenResidual[0] != 0.0f || stolenResidual[1] != 0.0f)
            chosen->declickSamplesRemaining = chosen->declickSamplesTotal
                = juce::jmax(1, static_cast<int>(sampleRate * 0.006));
        updateSaxLoopVoicePitches();
        chosen->pitchRatio = chosen->targetPitchRatio;
        const auto grainLength = getSaxLoopGrainLengthSamples();
        const auto halfGrainLength = grainLength / 2;
        chosen->grains[0].age = 0;
        const auto loopLength = audioMemory.loopLength;
        const auto crossfadeSamples = loopLength > 16
            ? juce::jmin(static_cast<int>(loopLength / 4),
                         juce::jmax(4, static_cast<int>(
                             std::round(sampleRate * 0.012))))
            : 0;
        chosen->grains[0].readPosition = wrapSaxLoopReadPosition(
            (1.0 - chosen->pitchRatio)
                * static_cast<double>(halfGrainLength),
            loopLength, crossfadeSamples);
        chosen->grains[1].age = halfGrainLength;
        chosen->grains[1].readPosition = 0.0;
        return;
    }

    if (message.isNoteOff())
    {
        const auto releasedNote = juce::jlimit(
            0, 127, message.getNoteNumber());
        auto& heldCount = saxLoopHeldNoteCounts[
            static_cast<std::size_t>(releasedNote)];
        if (heldCount > 0)
            --heldCount;
        for (auto& voice : saxLoopVoices)
        {
            if (! voice.active || voice.midiNote != releasedNote)
                continue;

            auto previousNote = -1;
            uint64_t newestAge = 0;
            for (int candidate = 0; candidate < 128; ++candidate)
                if (saxLoopHeldNoteCounts[static_cast<std::size_t>(candidate)] > 0
                    && saxLoopHeldAges[static_cast<std::size_t>(candidate)]
                        >= newestAge)
                {
                    previousNote = candidate;
                    newestAge = saxLoopHeldAges[
                        static_cast<std::size_t>(candidate)];
                }
            if (previousNote >= 0)
            {
                voice.midiNote = previousNote;
                voice.targetVelocity = saxLoopHeldVelocities[
                    static_cast<std::size_t>(previousNote)];
                voice.age = newestAge;
                voice.keyDown = true;
                voice.releasing = false;
                updateSaxLoopVoicePitches();
                continue;
            }
            voice.keyDown = false;
            if (! saxLoopSustain)
                voice.releasing = true;
        }
        return;
    }

    if (message.isPitchWheel())
    {
        saxLoopPitchBendSemitones = juce::jlimit(
            -2.0f, 2.0f,
            2.0f * static_cast<float>(message.getPitchWheelValue() - 8192)
                / 8192.0f);
        updateSaxLoopVoicePitches();
        return;
    }

    if (message.isController() && message.getControllerNumber() == 64)
    {
        const auto wasSustained = saxLoopSustain;
        saxLoopSustain = message.getControllerValue() >= 64;
        if (wasSustained && ! saxLoopSustain)
            for (auto& voice : saxLoopVoices)
                if (voice.active && ! voice.keyDown)
                    voice.releasing = true;
        return;
    }

    if (message.isAllSoundOff())
    {
        resetSaxLoopKeyboardVoices();
        return;
    }

    if (message.isAllNotesOff())
    {
        saxLoopHeldNoteCounts.fill(0);
        for (auto& voice : saxLoopVoices)
        {
            voice.keyDown = false;
            voice.releasing = voice.active;
        }
    }
}

void EcosystemEngine::updateSaxLoopVoicePitches() noexcept
{
    for (auto& voice : saxLoopVoices)
    {
        if (! voice.active)
            continue;

        const auto semitones = static_cast<float>(
            voice.midiNote - activeSaxLoopKeyboardPatch.rootNote)
            + saxLoopPitchBendSemitones;
        voice.targetPitchRatio = juce::jlimit(
            0.25, 4.0, std::exp2(static_cast<double>(semitones) / 12.0));
        const auto cutoff = static_cast<float>(sampleRate * 0.44)
                          / juce::jmax(1.0f,
                              static_cast<float>(voice.targetPitchRatio));
        voice.playbackFilterPole = std::exp(
            -juce::MathConstants<float>::twoPi * cutoff
            / static_cast<float>(sampleRate));
    }
}

int EcosystemEngine::getSaxLoopGrainLengthSamples() const noexcept
{
    const auto milliseconds = juce::jlimit(
        24.0f, 120.0f, activeSaxLoopKeyboardPatch.grainMilliseconds);
    auto requested = juce::jmax(
        32, static_cast<int>(std::round(
            sampleRate * static_cast<double>(milliseconds) * 0.001)));
    const auto available = audioMemory.loopLength > 32
        ? static_cast<int>(audioMemory.loopLength) : requested;
    auto length = juce::jmax(32, juce::jmin(requested, available));
    length -= length % 2;
    return juce::jmax(32, length);
}

void EcosystemEngine::beginSaxLoopKeyboardFadeOut(
    bool sourceWillDisappear) noexcept
{
    if (sourceWillDisappear)
    {
        saxLoopClearFadeOffset = saxLoopLastOutput;
        saxLoopClearFadeSamplesRemaining = saxLoopClearFadeSamplesTotal
            = juce::jmax(1, static_cast<int>(sampleRate * 0.008));
        resetSaxLoopKeyboardVoices();
        saxLoopKeyboardProcessor.beginIncrementalTailReset();
        return;
    }

    // MIDI 5 is handed back to the live bass outside COSMOS, so its later
    // note-offs no longer reach this sampler. Preserve the audible release,
    // but forget the keyboard stack now to avoid resurrecting held notes when
    // COSMOS is selected again during the tail.
    saxLoopHeldNoteCounts.fill(0);
    saxLoopHeldVelocities.fill(0.0f);
    saxLoopHeldAges.fill(0);

    auto activeVoiceCount = 0;
    for (const auto& voice : saxLoopVoices)
        activeVoiceCount += voice.active ? 1 : 0;
    const auto shouldDrain = saxLoopKeyboardModeActive
                          || saxLoopKeyboardTailActive
                          || activeVoiceCount > 0;

    for (auto& voice : saxLoopVoices)
    {
        if (! voice.active)
            continue;

        voice.keyDown = false;
        voice.releasing = true;
    }
    saxLoopSustain = false;
    saxLoopKeyboardTailActive = shouldDrain;
    saxLoopKeyboardTailSamplesRemaining = shouldDrain
        ? static_cast<int64_t>(std::ceil(sampleRate * 12.0)) : 0;
}

void EcosystemEngine::resetSaxLoopKeyboardVoices() noexcept
{
    for (auto& voice : saxLoopVoices)
        voice = {};
    saxLoopHeldNoteCounts.fill(0);
    saxLoopHeldVelocities.fill(0.0f);
    saxLoopHeldAges.fill(0);
    saxLoopPitchBendSemitones = 0.0f;
    saxLoopSustain = false;
    saxLoopVoiceAge = 0;
    saxLoopPolyphonyGain = 1.0f;
    saxLoopKeyboardTailActive = false;
    saxLoopKeyboardTailSamplesRemaining = 0;
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

double EcosystemEngine::wrapSaxLoopReadPosition(
    double position, int64_t length, int crossfadeSamples) noexcept
{
    if (length <= 0)
        return 0.0;

    const auto wrappedSpan = juce::jmax(
        1.0, static_cast<double>(length - crossfadeSamples));
    while (position < 0.0)
        position += wrappedSpan;
    while (position >= static_cast<double>(length))
        position -= wrappedSpan;
    return position;
}

void EcosystemEngine::resetCosmosHeads() noexcept
{
    const auto length = static_cast<double>(audioMemory.loopLength);
    cosmosHeadPositions = { 0.0, length * 0.853 * 0.211,
                            length * 0.719 * 0.433,
                            length * 0.593 * 0.677 };
    cosmosModulationPhase = 0.0;
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
            memory.playbackPosition += untilWrap;
            remaining -= untilWrap;
            outputOffset += untilWrap;

            if (memory.playbackPosition >= memory.loopLength)
            {
                memory.playbackPosition = 0;
            }
        }
        memory.phase.store(static_cast<double>(memory.playbackPosition)
                           / static_cast<double>(memory.loopLength));
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
    if (saxLoopKeyboardModeActive && audioMemory.loopLength > 0)
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
        if (pathMode == SaxPathMode::muted)
            continue;

        // The configured sax input is monitored with deliberate headroom.
        for (int channel = 0; channel < 2; ++channel)
            saxRenderBuffer.addSample(channel, sample,
                                      readInput(channel, sample) * 0.58f);

        // This path is deliberately just input -> output. It distinguishes a
        // capture/driver problem from loop and effects processing.
        if (pathMode == SaxPathMode::direct)
            continue;

        if (audioMemory.recordingActive && audioMemory.initialCapture)
        {
            if (audioMemory.writePosition >= maximumLength)
            {
                audioMemory.recordingRequested.store(false);
                break;
            }
            for (int channel = 0; channel < audioMemory.buffer.getNumChannels(); ++channel)
            {
                const auto input = readInput(channel, sample);
                audioMemory.buffer.setSample(channel,
                    static_cast<int>(audioMemory.writePosition), input * 0.72f);
            }
            ++audioMemory.writePosition;
            continue;
        }

        if (audioMemory.loopLength <= 0)
            continue;

        const auto bufferPosition = static_cast<int>(audioMemory.playbackPosition);
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

            if (saxLoopKeyboardModeActive)
            {
                loopSample = 0.0f;
                for (std::size_t head = 0; head < cosmosHeadPositions.size();
                     ++head)
                {
                    loopSample += readAudioMemoryCrossfaded(
                        memoryChannel, cosmosHeadPositions[head],
                        cosmosHeadLengths[head], cosmosCrossfades[head])
                        * cosmosWeights[static_cast<std::size_t>(channel)][head];
                }
                loopSample *= 0.72f;
            }
            saxRenderBuffer.addSample(channel, sample, loopSample * 0.72f);
        }

        audioMemory.playbackPosition = (audioMemory.playbackPosition + 1)
                                       % audioMemory.loopLength;
        if (saxLoopKeyboardModeActive)
        {
            const auto modulationSin = std::sin(cosmosModulationPhase);
            const auto modulationCos = std::cos(cosmosModulationPhase);
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
    }

    if (pathMode == SaxPathMode::sceneEffects)
    {
        saxProcessor.setDelayLevel(getDelayLevel(midiMemoryCount));
        saxProcessor.process(saxRenderBuffer, numSamples);
    }
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
        const auto value = static_cast<float>(std::sin(diagnosticTonePhase)) * level;
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
