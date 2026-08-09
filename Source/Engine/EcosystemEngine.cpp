#include "EcosystemEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
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
    return patch;
}

[[nodiscard]] SaxPatch applyTexture(SaxPatch patch, float amount) noexcept
{
    patch.drive = 1.0f + juce::jmax(0.0f, patch.drive - 1.0f) * amount;
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

    incomingMessages[static_cast<size_t>(start1)] = message;
    incomingFifo.finishedWrite(1);
}

void EcosystemEngine::toggleRecording(int memoryIndex)
{
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount)
        && ! isLiveBassLayer(memoryIndex))
    {
        auto& requested = midiMemories[static_cast<size_t>(memoryIndex)].recordingRequested;
        requested.store(! requested.load());
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
        return midiMemories[static_cast<size_t>(memoryIndex)].recordingForDisplay.load();
    return memoryIndex == midiMemoryCount && audioMemory.recordingForDisplay.load();
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

    // Force the requested scene and texture back onto freshly prepared DSP;
    // prepare may run again after reconnecting the Model 12.
    activeScenario.store(-1);
    activeTexture = -1.0f;
    applyScenarioIfNeeded();
}

void EcosystemEngine::audioDeviceStopped()
{
    audioRunning.store(false);
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
}

void EcosystemEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
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
    for (int index = 0; index < midiMemoryCount; ++index)
        internalSynths[static_cast<size_t>(index)]->setPatch(
            applyTexture(scenario.layers[static_cast<size_t>(index)], texture));
    saxProcessor.setPatch(applyTexture(scenario.sax, texture));
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
    }
}

void EcosystemEngine::recordIncomingMidi(int numSamples, juce::MidiBuffer& liveMidi)
{
    int start1 = 0;
    int size1 = 0;
    int start2 = 0;
    int size2 = 0;
    incomingFifo.prepareToRead(incomingCapacity, start1, size1, start2, size2);

    const auto consumeRange = [this, &liveMidi](int start, int size)
    {
        for (int offset = 0; offset < size; ++offset)
        {
            const auto& message = incomingMessages[static_cast<size_t>(start + offset)];
            const auto channel = message.getChannel();
            const auto memoryIndex = memoryIndexForMidiChannel(channel);
            if (memoryIndex < 0)
                continue;

            liveMidi.addEvent(message, 0);

            auto& memory = midiMemories[static_cast<size_t>(memoryIndex)];
            if (memory.recordingActive
                && static_cast<int>(memory.events.size()) < maximumMidiEvents)
            {
                memory.events.push_back({ message, memory.recordPosition });
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

    for (auto& memory : midiMemories)
    {
        if (! memory.recordingActive)
            continue;
        memory.recordPosition += numSamples;
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
        internalSynths[static_cast<size_t>(layer)]->setDelayLevel(
            getDelayLevel(layer));
        internalSynths[static_cast<size_t>(layer)]->render(
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
            saxRenderBuffer.addSample(channel, sample, loopSample * 0.72f);
        }

        audioMemory.playbackPosition = (audioMemory.playbackPosition + 1)
                                       % audioMemory.loopLength;
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
