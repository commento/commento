#pragma once

#include <JuceHeader.h>
#include "AmbientSynth.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

class EcosystemEngine final : public juce::AudioIODeviceCallback
{
public:
    static constexpr int midiMemoryCount = 4;
    static constexpr int memoryCount = 5;

    EcosystemEngine();

    void enqueueMidiMessage(const juce::MidiMessage& message);
    void toggleRecording(int memoryIndex);
    void clearMemory(int memoryIndex);

    [[nodiscard]] bool isRecording(int memoryIndex) const;
    [[nodiscard]] bool hasMaterial(int memoryIndex) const;
    [[nodiscard]] double getPhase(int memoryIndex) const;
    [[nodiscard]] double getLengthSeconds(int memoryIndex) const;
    [[nodiscard]] int getEventCount(int memoryIndex) const;

    void setAudioDecay(float newDecay);
    [[nodiscard]] float getAudioDecay() const;
    void prepare(double newSampleRate);

    juce::MidiBuffer takeMidiOutput();

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    struct TimedMidiEvent
    {
        juce::MidiMessage message;
        int64_t samplePosition = 0;
    };

    struct MidiMemory
    {
        std::vector<TimedMidiEvent> events;
        std::atomic<bool> recordingRequested { false };
        std::atomic<bool> recordingForDisplay { false };
        std::atomic<bool> clearRequested { false };
        std::atomic<bool> containsMaterial { false };
        std::atomic<double> phase { 0.0 };
        std::atomic<double> lengthSeconds { 0.0 };
        std::atomic<int> eventCount { 0 };
        bool recordingActive = false;
        int64_t recordPosition = 0;
        int64_t playbackPosition = 0;
        int64_t loopLength = 0;
    };

    struct AudioMemory
    {
        juce::AudioBuffer<float> buffer;
        std::atomic<bool> recordingRequested { false };
        std::atomic<bool> recordingForDisplay { false };
        std::atomic<bool> clearRequested { false };
        std::atomic<bool> containsMaterial { false };
        std::atomic<double> phase { 0.0 };
        std::atomic<double> lengthSeconds { 0.0 };
        bool recordingActive = false;
        bool initialCapture = false;
        int64_t writePosition = 0;
        int64_t playbackPosition = 0;
        int64_t loopLength = 0;
    };

    static constexpr int incomingCapacity = 512;
    static constexpr int maximumMidiEvents = 8192;
    static constexpr double maximumAudioSeconds = 120.0;

    void applyMidiCommands(MidiMemory& memory, int channel, juce::MidiBuffer& output);
    void applyAudioCommands();
    void recordIncomingMidi(int numSamples, juce::MidiBuffer& liveMidi);
    void renderMidiMemories(int numSamples, juce::MidiBuffer& output);
    void renderMidiSegment(MidiMemory& memory, int64_t segmentStart,
                           int segmentLength, int outputOffset, juce::MidiBuffer& output);
    void renderAudioMemory(const float* const* inputs, int inputChannels,
                           float* const* outputs, int outputChannels, int numSamples);
    void renderInternalSynths(float* const* outputs, int outputChannels,
                              int numSamples, const juce::MidiBuffer& midi);

    std::array<MidiMemory, midiMemoryCount> midiMemories;
    std::array<std::unique_ptr<AmbientSynth>, midiMemoryCount> internalSynths;
    AudioMemory audioMemory;

    std::array<juce::MidiMessage, incomingCapacity> incomingMessages;
    juce::AbstractFifo incomingFifo { incomingCapacity };
    juce::MidiBuffer blockMidiOutput;
    juce::AudioBuffer<float> synthMixBuffer;
    juce::CriticalSection midiOutputLock;
    juce::MidiBuffer pendingMidiOutput;

    std::atomic<float> audioDecay { 0.985f };
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EcosystemEngine)
};
