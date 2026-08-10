#pragma once

#include <JuceHeader.h>
#include "AmbientSynth.h"
#include "PerformanceLevels.h"
#include "SaxProcessor.h"
#include "Scenarios.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class EcosystemEngine final : public juce::AudioIODeviceCallback
{
public:
    enum class SaxPathMode : int
    {
        muted = 0,
        direct,
        cleanLooper,
        sceneEffects
    };

    enum class DiagnosticToneBus : int
    {
        off = -1,
        ambient = 0,
        bass,
        sax
    };

    static constexpr int midiMemoryCount = 4;
    static constexpr int memoryCount = 5;
    static constexpr int bassLayerIndex = 0;
    static constexpr int ambientLeftBus = 0;
    static constexpr int ambientRightBus = 1;
    static constexpr int bassBus = 2;
    static constexpr int saxLeftBus = 3;
    static constexpr int saxRightBus = 4;
    static constexpr int logicalOutputBusCount = 5;
    // The source is a monophonic sax phrase and MIDI 5 is normally played as
    // a live line. Keeping this instrument monophonic avoids transposed phrase
    // clusters and leaves considerably more headroom on Raspberry Pi.
    static constexpr int saxLoopKeyboardVoiceCount = 1;
    static_assert(PerformanceLevels::count == memoryCount);

    EcosystemEngine();

    void enqueueMidiMessage(const juce::MidiMessage& message);
    void toggleRecording(int memoryIndex);
    void clearMemory(int memoryIndex);

    [[nodiscard]] bool isRecording(int memoryIndex) const;
    [[nodiscard]] bool isWaitingForFirstNote(int memoryIndex) const;
    [[nodiscard]] bool hasMaterial(int memoryIndex) const;
    [[nodiscard]] double getPhase(int memoryIndex) const;
    [[nodiscard]] double getLengthSeconds(int memoryIndex) const;
    [[nodiscard]] int getEventCount(int memoryIndex) const;
    [[nodiscard]] int getMidiChannelForMemory(int memoryIndex) const;
    [[nodiscard]] bool isAudioRunning() const;
    // -1 while no callback has run, 0 when Linux refused realtime scheduling,
    // 1 when the audio callback owns an explicit realtime policy.
    [[nodiscard]] int getRealtimeSchedulingStatus() const noexcept;
    [[nodiscard]] float getDspLoad() const noexcept;
    [[nodiscard]] int getDspNearOverloadCount() const noexcept;
    [[nodiscard]] int getCallbackInputChannelCount() const;
    [[nodiscard]] int getCallbackOutputChannelCount() const;
    [[nodiscard]] float getSaxInputLevel() const;
    [[nodiscard]] float getStereoOutputLevel() const;
    [[nodiscard]] float getBassOutputLevel() const;
    [[nodiscard]] float getSaxOutputLevel() const;
    [[nodiscard]] bool isSaxSafetyMuted() const;
    [[nodiscard]] int getDroppedMidiMessageCount() const;
    [[nodiscard]] static bool isLiveBassLayer(int memoryIndex);
    [[nodiscard]] bool isSaxLoopKeyboardEnabled() const;

    void setScenarioIndex(int index);
    [[nodiscard]] int getScenarioIndex() const;
    void setTextureAmount(float amount);
    [[nodiscard]] float getTextureAmount() const;
    void setBassEnabled(bool shouldBeEnabled);
    [[nodiscard]] bool isBassEnabled() const;
    void setPerformanceLevel(int memoryIndex, float linearGain) noexcept;
    [[nodiscard]] float getPerformanceLevel(int memoryIndex) const noexcept;
    void setDelayLevel(int memoryIndex, float amount) noexcept;
    [[nodiscard]] float getDelayLevel(int memoryIndex) const noexcept;

    void setAudioDecay(float newDecay);
    [[nodiscard]] float getAudioDecay() const;
    void setSaxStereoInput(bool shouldUseStereo);
    [[nodiscard]] bool isSaxStereoInput() const;
    void setSaxPathMode(SaxPathMode mode);
    [[nodiscard]] SaxPathMode getSaxPathMode() const;
    void setDiagnosticToneBus(DiagnosticToneBus bus);
    [[nodiscard]] DiagnosticToneBus getDiagnosticToneBus() const;
    void prepare(double newSampleRate, int maximumBlockSize = 4096);

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

    struct IncomingMidiEvent
    {
        juce::MidiMessage message;
        double timestampSeconds = 0.0;
    };

    struct MidiMemory
    {
        std::vector<TimedMidiEvent> events;
        std::atomic<bool> recordingRequested { false };
        std::atomic<bool> recordingForDisplay { false };
        std::atomic<bool> waitingForFirstNoteForDisplay { false };
        std::atomic<double> armedAfterTimestampSeconds { 0.0 };
        std::atomic<bool> clearRequested { false };
        std::atomic<bool> containsMaterial { false };
        std::atomic<double> phase { 0.0 };
        std::atomic<double> lengthSeconds { 0.0 };
        std::atomic<int> eventCount { 0 };
        bool recordingActive = false;
        bool waitingForFirstNote = false;
        int64_t recordPosition = 0;
        int64_t playbackPosition = 0;
        int64_t loopLength = 0;
        std::array<bool, 128> activeRecordedNotes {};
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

    struct SaxLoopGrain
    {
        double readPosition = 0.0;
        int age = 0;
    };

    struct SaxLoopVoice
    {
        bool active = false;
        bool releasing = false;
        bool keyDown = false;
        int midiNote = -1;
        double transportPosition = 0.0;
        double pitchRatio = 1.0;
        double targetPitchRatio = 1.0;
        float envelope = 0.0f;
        float velocity = 0.0f;
        float targetVelocity = 0.0f;
        float playbackFilterPole = 0.0f;
        std::array<SaxLoopGrain, 2> grains;
        std::array<float, 2> filterState {};
        std::array<float, 2> declickOffset {};
        int declickSamplesRemaining = 0;
        int declickSamplesTotal = 0;
        uint64_t age = 0;
    };

    static constexpr int incomingCapacity = 512;
    static constexpr int maximumMidiEvents = 8192;
    static constexpr int granularWindowSize = 2048;
    static constexpr double maximumAudioSeconds = 120.0;
    static constexpr std::array<int, midiMemoryCount> midiChannels { 5, 2, 3, 4 };

    [[nodiscard]] static int memoryIndexForMidiChannel(int midiChannel);

    void applyMidiCommands(MidiMemory& memory, int channel, juce::MidiBuffer& output);
    void applyAudioCommands();
    void applyScenarioIfNeeded();
    void recordIncomingMidi(int numSamples, juce::MidiBuffer& liveMidi);
    void renderMidiMemories(int numSamples, juce::MidiBuffer& output);
    void renderMidiSegment(MidiMemory& memory, int64_t segmentStart,
                           int segmentLength, int outputOffset, juce::MidiBuffer& output);
    void renderAudioMemory(const float* const* inputs, int inputChannels,
                           float* const* outputs, int outputChannels, int numSamples);
    void renderInternalSynths(float* const* outputs, int outputChannels,
                              int numSamples, const juce::MidiBuffer& midi);
    void renderSaxLoopKeyboard(juce::AudioBuffer<float>& output,
                               const juce::MidiBuffer& midi, int numSamples);
    void handleSaxLoopKeyboardMessage(const juce::MidiMessage& message);
    void updateSaxLoopVoicePitches() noexcept;
    [[nodiscard]] int getSaxLoopGrainLengthSamples() const noexcept;
    void beginSaxLoopKeyboardFadeOut(bool sourceWillDisappear = false) noexcept;
    void resetSaxLoopKeyboardVoices() noexcept;
    void resetCosmosHeads() noexcept;
    [[nodiscard]] float readAudioMemorySample(int channel,
                                              double position,
                                              int64_t length) const noexcept;
    [[nodiscard]] float readAudioMemoryCrossfaded(int channel,
                                                  double position,
                                                  int64_t length,
                                                  int crossfadeSamples) const noexcept;
    [[nodiscard]] static double wrapSaxLoopReadPosition(
        double position, int64_t length, int crossfadeSamples) noexcept;
    void renderDiagnosticTone(float* const* outputs, int outputChannels,
                              int numSamples);

    std::array<MidiMemory, midiMemoryCount> midiMemories;
    std::array<std::unique_ptr<AmbientSynth>, midiMemoryCount> internalSynths;
    AudioMemory audioMemory;
    SaxProcessor saxProcessor;
    SaxProcessor saxLoopKeyboardProcessor;

    std::array<IncomingMidiEvent, incomingCapacity> incomingMessages;
    juce::AbstractFifo incomingFifo { incomingCapacity };
    double previousMidiCallbackTimeSeconds = 0.0;
    juce::MidiBuffer blockMidiOutput;
    std::array<juce::MidiBuffer, midiMemoryCount> layerMidiBuffers;
    juce::AudioBuffer<float> ambientSynthBuffer;
    juce::AudioBuffer<float> bassSynthBuffer;
    juce::AudioBuffer<float> layerSynthBuffer;
    juce::AudioBuffer<float> saxLoopTailBuffer;
    juce::AudioBuffer<float> saxRenderBuffer;
    PerformanceLevels performanceLevels;
    std::array<std::atomic<float>, memoryCount> delayLevels;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bassMuteGain;
    std::atomic<float> audioDecay { 0.985f };
    std::atomic<bool> saxStereoInput { false };
    std::atomic<int> saxPathMode { static_cast<int>(SaxPathMode::sceneEffects) };
    std::atomic<int> diagnosticToneBus {
        static_cast<int>(DiagnosticToneBus::off)
    };
    std::atomic<bool> audioRunning { false };
    std::atomic<int> realtimeSchedulingStatus { -1 };
    std::atomic<float> dspLoad { 0.0f };
    std::atomic<int> dspNearOverloadCount { 0 };
    int dspWarmupCallbacksRemaining = 0;
    std::atomic<int> callbackInputChannels { 0 };
    std::atomic<int> callbackOutputChannels { 0 };
    std::atomic<float> saxInputLevel { 0.0f };
    std::atomic<float> stereoOutputLevel { 0.0f };
    std::atomic<float> bassOutputLevel { 0.0f };
    std::atomic<float> saxOutputLevel { 0.0f };
    std::atomic<bool> saxSafetyMuted { false };
    std::atomic<bool> midiOverflowed { false };
    std::atomic<int> droppedMidiMessages { 0 };
    std::atomic<int> requestedScenario { 0 };
    std::atomic<int> activeScenario { -1 };
    std::atomic<float> requestedTexture { 0.0f };
    std::atomic<bool> bassEnabled { true };
    SaxLoopKeyboardPatch activeSaxLoopKeyboardPatch;
    bool saxLoopKeyboardModeActive = false;
    bool saxLoopKeyboardTailActive = false;
    int64_t saxLoopKeyboardTailSamplesRemaining = 0;
    std::array<SaxLoopVoice, saxLoopKeyboardVoiceCount> saxLoopVoices;
    std::array<uint8_t, 128> saxLoopHeldNoteCounts {};
    std::array<float, 128> saxLoopHeldVelocities {};
    std::array<uint64_t, 128> saxLoopHeldAges {};
    std::array<double, 4> cosmosHeadPositions {};
    double cosmosModulationPhase = 0.0;
    float saxLoopPitchBendSemitones = 0.0f;
    bool saxLoopSustain = false;
    uint64_t saxLoopVoiceAge = 0;
    std::array<float, granularWindowSize> saxLoopGranularWindow {};
    float saxLoopPolyphonyGain = 1.0f;
    std::array<float, 2> saxLoopLastOutput {};
    std::array<float, 2> saxLoopClearFadeOffset {};
    int saxLoopClearFadeSamplesRemaining = 0;
    int saxLoopClearFadeSamplesTotal = 0;
    std::array<float, 2> channelOneLastOutput {};
    std::array<float, 2> channelOneTransitionOffset {};
    int channelOneTransitionSamplesRemaining = 0;
    int channelOneTransitionSamplesTotal = 0;
    float activeTexture = -1.0f;
    bool bassWasEnabled = true;
    int64_t saxDangerSamples = 0;
    int64_t saxRecoverySamples = 0;
    float saxSafetyGain = 1.0f;
    double diagnosticTonePhase = 0.0;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EcosystemEngine)
};
