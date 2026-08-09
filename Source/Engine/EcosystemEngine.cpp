#include "EcosystemEngine.h"

#include <algorithm>
#include <cmath>

EcosystemEngine::EcosystemEngine()
{
    for (int index = 0; index < midiMemoryCount; ++index)
    {
        midiMemories[static_cast<size_t>(index)].events.reserve(maximumMidiEvents);
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
        return;

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

void EcosystemEngine::setAudioDecay(float newDecay)
{
    audioDecay.store(juce::jlimit(0.80f, 1.0f, newDecay));
}

float EcosystemEngine::getAudioDecay() const
{
    return audioDecay.load();
}

juce::MidiBuffer EcosystemEngine::takeMidiOutput()
{
    const juce::ScopedLock lock(midiOutputLock);
    juce::MidiBuffer result;
    result.swapWith(pendingMidiOutput);
    return result;
}

void EcosystemEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    prepare(device != nullptr ? device->getCurrentSampleRate() : 48000.0);
}

void EcosystemEngine::prepare(double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    const auto maximumSamples = static_cast<int>(std::ceil(sampleRate * maximumAudioSeconds));
    audioMemory.buffer.setSize(2, maximumSamples, false, true, false);
    audioMemory.buffer.clear();
    synthMixBuffer.setSize(2, 4096, false, true, false);
    for (auto& synth : internalSynths)
        synth->prepare(sampleRate);
}

void EcosystemEngine::audioDeviceStopped()
{
    for (auto& memory : midiMemories)
        memory.recordingForDisplay.store(false);
    audioMemory.recordingForDisplay.store(false);
}

void EcosystemEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    for (int channel = 0; channel < numOutputChannels; ++channel)
        if (outputChannelData[channel] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

    blockMidiOutput.clear();
    for (int index = 0; index < midiMemoryCount; ++index)
        applyMidiCommands(midiMemories[static_cast<size_t>(index)], index + 1, blockMidiOutput);
    applyAudioCommands();
    recordIncomingMidi(numSamples, blockMidiOutput);
    renderMidiMemories(numSamples, blockMidiOutput);
    renderInternalSynths(outputChannelData, numOutputChannels,
                         numSamples, blockMidiOutput);
    renderAudioMemory(inputChannelData, numInputChannels,
                      outputChannelData, numOutputChannels, numSamples);

    if (! blockMidiOutput.isEmpty())
    {
        const juce::ScopedLock lock(midiOutputLock);
        pendingMidiOutput.addEvents(blockMidiOutput, 0, numSamples, 0);
    }
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
    }
    else
    {
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
        audioMemory.buffer.clear();
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
            audioMemory.buffer.clear();
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
            if (! juce::isPositiveAndBelow(channel - 1, midiMemoryCount))
                continue;

            liveMidi.addEvent(message, 0);

            auto& memory = midiMemories[static_cast<size_t>(channel - 1)];
            if (memory.recordingActive
                && static_cast<int>(memory.events.size()) < maximumMidiEvents)
                memory.events.push_back({ message, memory.recordPosition });
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
        juce::MidiBuffer layerMidi;
        for (const auto metadata : midi)
            if (metadata.getMessage().getChannel() == layer + 1)
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
                output.addEvent(juce::MidiMessage::allNotesOff(index + 1),
                                juce::jlimit(0, numSamples - 1, outputOffset));
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
                const auto inputChannel = inputChannels > 0 ? juce::jmin(channel, inputChannels - 1) : -1;
                const auto input = inputChannel >= 0 && inputs[inputChannel] != nullptr
                    ? inputs[inputChannel][sample] : 0.0f;
                audioMemory.buffer.setSample(channel,
                    static_cast<int>(audioMemory.writePosition), input);
            }
            ++audioMemory.writePosition;
            audioMemory.phase.store(static_cast<double>(audioMemory.writePosition)
                                    / static_cast<double>(maximumLength));
            audioMemory.lengthSeconds.store(static_cast<double>(audioMemory.writePosition) / sampleRate);
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
                const auto inputChannel = inputChannels > 0 ? juce::jmin(channel, inputChannels - 1) : -1;
                const auto input = inputChannel >= 0 && inputs[inputChannel] != nullptr
                    ? inputs[inputChannel][sample] : 0.0f;
                loopSample = loopSample * decay + input;
                audioMemory.buffer.setSample(memoryChannel, bufferPosition,
                                             juce::jlimit(-1.0f, 1.0f, loopSample));
            }
            outputs[channel][sample] += loopSample * 0.82f;
        }

        audioMemory.playbackPosition = (audioMemory.playbackPosition + 1)
                                       % audioMemory.loopLength;
        audioMemory.phase.store(static_cast<double>(audioMemory.playbackPosition)
                                / static_cast<double>(audioMemory.loopLength));
    }
}
