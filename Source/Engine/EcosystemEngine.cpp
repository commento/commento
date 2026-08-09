#include "EcosystemEngine.h"

#include <algorithm>
#include <cmath>

EcosystemEngine::EcosystemEngine()
{
    for (int index = 0; index < midiMemoryCount; ++index)
    {
        midiMemories[static_cast<size_t>(index)].events.reserve(maximumMidiEvents + 128);
        internalSynths[static_cast<size_t>(index)] = std::make_unique<AmbientSynth>(index);
    }
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
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
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
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        midiMemories[static_cast<size_t>(memoryIndex)].clearRequested.store(true);
    else if (memoryIndex == midiMemoryCount)
        audioMemory.clearRequested.store(true);
}

bool EcosystemEngine::isRecording(int memoryIndex) const
{
    if (juce::isPositiveAndBelow(memoryIndex, midiMemoryCount))
        return midiMemories[static_cast<size_t>(memoryIndex)].recordingForDisplay.load();
    return memoryIndex == midiMemoryCount && audioMemory.recordingForDisplay.load();
}

bool EcosystemEngine::hasMaterial(int memoryIndex) const
{
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

int EcosystemEngine::getDroppedMidiMessageCount() const
{
    return droppedMidiMessages.load();
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

    synthMixBuffer.setSize(2, juce::jmax(8192, maximumBlockSize),
                           false, true, false);
    blockMidiOutput.ensureSize(128 * 1024);
    for (auto& midi : layerMidiBuffers)
        midi.ensureSize(64 * 1024);
    for (auto& synth : internalSynths)
        synth->prepare(sampleRate);
}

void EcosystemEngine::audioDeviceStopped()
{
    audioRunning.store(false);
    callbackInputChannels.store(0);
    callbackOutputChannels.store(0);
    saxInputLevel.store(0.0f);
    stereoOutputLevel.store(0.0f);
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
    callbackInputChannels.store(numInputChannels);
    callbackOutputChannels.store(numOutputChannels);

    for (int channel = 0; channel < numOutputChannels; ++channel)
        if (outputChannelData[channel] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

    blockMidiOutput.clear();
    if (midiOverflowed.exchange(false))
        for (const auto channel : midiChannels)
            blockMidiOutput.addEvent(juce::MidiMessage::allNotesOff(channel), 0);

    for (int index = 0; index < midiMemoryCount; ++index)
        applyMidiCommands(midiMemories[static_cast<size_t>(index)],
                          midiChannels[static_cast<size_t>(index)], blockMidiOutput);
    applyAudioCommands();
    recordIncomingMidi(numSamples, blockMidiOutput);
    renderMidiMemories(numSamples, blockMidiOutput);
    // USB multichannel devices such as Model 12 may force ALSA/JUCE to open
    // all hardware playback channels. Commento deliberately writes only to
    // USB outputs 1+2; channels 3-10 remain at the zeroes written above.
    const auto stereoOutputChannels = juce::jmin(2, numOutputChannels);
    renderInternalSynths(outputChannelData, stereoOutputChannels,
                         numSamples, blockMidiOutput);
    renderAudioMemory(inputChannelData, numInputChannels,
                      outputChannelData, stereoOutputChannels, numSamples);

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

    float outputPeak = 0.0f;
    for (int channel = 0; channel < stereoOutputChannels; ++channel)
    {
        if (outputChannelData[channel] == nullptr)
            continue;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            auto& value = outputChannelData[channel][sample];
            value = std::tanh(value * 0.88f);
            outputPeak = juce::jmax(outputPeak, std::abs(value));
        }
    }
    stereoOutputLevel.store(juce::jmax(outputPeak, stereoOutputLevel.load() * 0.86f));
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

    if (synthMixBuffer.getNumSamples() < numSamples)
        synthMixBuffer.setSize(2, numSamples, false, false, true);
    synthMixBuffer.clear(0, numSamples);

    for (int layer = 0; layer < midiMemoryCount; ++layer)
    {
        auto& layerMidi = layerMidiBuffers[static_cast<size_t>(layer)];
        layerMidi.clear();
        for (const auto metadata : midi)
            if (metadata.getMessage().getChannel()
                == midiChannels[static_cast<size_t>(layer)])
                layerMidi.addEvent(metadata.getMessage(), metadata.samplePosition);

        internalSynths[static_cast<size_t>(layer)]->render(
            synthMixBuffer, layerMidi, 0, numSamples);
    }

    for (int channel = 0; channel < outputChannels; ++channel)
    {
        if (outputs[channel] == nullptr)
            continue;
        const auto synthChannel = juce::jmin(channel, synthMixBuffer.getNumChannels() - 1);
        juce::FloatVectorOperations::add(outputs[channel],
            synthMixBuffer.getReadPointer(synthChannel), numSamples);
    }
}

void EcosystemEngine::renderMidiMemories(int numSamples, juce::MidiBuffer& output)
{
    for (int index = 0; index < midiMemoryCount; ++index)
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
    if (audioMemory.buffer.getNumSamples() == 0)
        return;

    const auto maximumLength = static_cast<int64_t>(audioMemory.buffer.getNumSamples());
    const auto decay = audioDecay.load();
    const auto rightInputChannel = juce::jmin(1, inputChannels - 1);
    const auto useStereoInput = saxStereoInput.load();

    const auto readInput = [inputs, inputChannels, rightInputChannel,
                            useStereoInput](int channel, int sample)
    {
        if (inputs == nullptr || inputChannels <= 0)
            return 0.0f;
        const auto sourceChannel = useStereoInput && channel > 0
            ? rightInputChannel : 0;
        return juce::isPositiveAndBelow(sourceChannel, inputChannels)
                && inputs[sourceChannel] != nullptr
            ? inputs[sourceChannel][sample] : 0.0f;
    };

    for (int sample = 0; sample < numSamples; ++sample)
    {
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
                    static_cast<int>(audioMemory.writePosition), input);
            }
            ++audioMemory.writePosition;
            continue;
        }

        if (audioMemory.loopLength <= 0)
            continue;

        const auto bufferPosition = static_cast<int>(audioMemory.playbackPosition);
        for (int channel = 0; channel < outputChannels; ++channel)
        {
            if (outputs[channel] == nullptr)
                continue;
            const auto memoryChannel = juce::jmin(channel,
                audioMemory.buffer.getNumChannels() - 1);
            auto loopSample = audioMemory.buffer.getSample(memoryChannel, bufferPosition);

            if (audioMemory.recordingActive)
            {
                const auto input = readInput(memoryChannel, sample);
                loopSample = loopSample * decay + input;
                audioMemory.buffer.setSample(memoryChannel, bufferPosition,
                                             juce::jlimit(-1.0f, 1.0f, loopSample));
            }
            outputs[channel][sample] += loopSample * 0.82f;
        }

        audioMemory.playbackPosition = (audioMemory.playbackPosition + 1)
                                       % audioMemory.loopLength;
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
