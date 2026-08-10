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
    static constexpr double scenarioMorphSeconds = 8.0;
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
    [[nodiscard]] float getCallbackIntervalLoad() const noexcept;
    [[nodiscard]] int getLateCallbackCount() const noexcept;
    [[nodiscard]] int getCallbackInputChannelCount() const;
    [[nodiscard]] int getCallbackOutputChannelCount() const;
    [[nodiscard]] float getSaxInputLevel() const;
    [[nodiscard]] float getStereoOutputLevel() const;
    [[nodiscard]] float getBassOutputLevel() const;
    [[nodiscard]] float getSaxOutputLevel() const;
    [[nodiscard]] bool isSaxSafetyMuted() const;
    [[nodiscard]] int getDroppedMidiMessageCount() const;
    [[nodiscard]] static bool isLiveBassLayer(int memoryIndex);
    void setScenarioIndex(int index);
    [[nodiscard]] int getScenarioIndex() const;
    [[nodiscard]] int getScenarioMorphSourceIndex() const noexcept;
    [[nodiscard]] int getScenarioMorphDestinationIndex() const noexcept;
    [[nodiscard]] float getScenarioMorphProgress() const noexcept;
    void setTextureAmount(float amount);
    [[nodiscard]] float getTextureAmount() const;
    void setFuzzEnabled(bool shouldBeEnabled) noexcept;
    [[nodiscard]] bool isFuzzEnabled() const noexcept;
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
        float playbackGain = 1.0f;
        float playbackGainStart = 1.0f;
        float playbackGainTarget = 1.0f;
        int64_t gainTransitionSamplesRemaining = 0;
        int64_t gainTransitionSamplesTotal = 0;
        bool clearAfterGainTransition = false;
    };

    static constexpr int incomingCapacity = 512;
    static constexpr int maximumMidiEvents = 8192;
    static constexpr double maximumAudioSeconds = 120.0;
    static constexpr std::array<int, midiMemoryCount> midiChannels { 5, 2, 3, 4 };

    [[nodiscard]] static int memoryIndexForMidiChannel(int midiChannel);

    void applyMidiCommands(MidiMemory& memory, int channel, juce::MidiBuffer& output);
    void applyAudioCommands();
    void finishInitialAudioCapture() noexcept;
    void beginAudioMemoryGainTransition(float targetGain, double seconds,
                                        bool clearWhenFinished) noexcept;
    void advanceAudioMemoryGainTransition() noexcept;
    void finishAudioMemoryClear() noexcept;
    void applyScenarioIfNeeded();
    void advanceScenarioMorph(int numSamples) noexcept;
    void updatePerformanceEffectTargets() noexcept;
    void processPerformanceEffects(float* const* outputs, int outputChannels,
                                   int numSamples) noexcept;
    void recordIncomingMidi(int numSamples, juce::MidiBuffer& liveMidi);
    void renderMidiMemories(int numSamples, juce::MidiBuffer& output);
    void renderMidiSegment(MidiMemory& memory, int64_t segmentStart,
                           int segmentLength, int outputOffset, juce::MidiBuffer& output);
    void renderAudioMemory(const float* const* inputs, int inputChannels,
                           float* const* outputs, int outputChannels, int numSamples);
    void renderInternalSynths(float* const* outputs, int outputChannels,
                              int numSamples, const juce::MidiBuffer& midi);
    void resetCosmosHeads() noexcept;
    [[nodiscard]] float readAudioMemorySample(int channel,
                                              double position,
                                              int64_t length) const noexcept;
    [[nodiscard]] float readAudioMemoryCrossfaded(int channel,
                                                  double position,
                                                  int64_t length,
                                                  int crossfadeSamples) const noexcept;
    void renderDiagnosticTone(float* const* outputs, int outputChannels,
                              int numSamples);

    std::array<MidiMemory, midiMemoryCount> midiMemories;
    std::array<std::unique_ptr<AmbientSynth>, midiMemoryCount> internalSynths;
    AudioMemory audioMemory;
    SaxProcessor saxProcessor;

    std::array<IncomingMidiEvent, incomingCapacity> incomingMessages;
    juce::AbstractFifo incomingFifo { incomingCapacity };
    double previousMidiCallbackTimeSeconds = 0.0;
    juce::MidiBuffer blockMidiOutput;
    std::array<juce::MidiBuffer, midiMemoryCount> layerMidiBuffers;
    juce::AudioBuffer<float> ambientSynthBuffer;
    juce::AudioBuffer<float> bassSynthBuffer;
    juce::AudioBuffer<float> layerSynthBuffer;
    juce::AudioBuffer<float> saxRenderBuffer;
    PerformanceLevels performanceLevels;
    std::array<std::atomic<float>, memoryCount> delayLevels;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bassMuteGain;
    std::atomic<float> audioDecay { 0.985f };
    std::atomic<uint32_t> audioDecayManualRevision { 0 };
    std::atomic<bool> saxStereoInput { false };
    std::atomic<int> saxPathMode { static_cast<int>(SaxPathMode::sceneEffects) };
    std::atomic<int> diagnosticToneBus {
        static_cast<int>(DiagnosticToneBus::off)
    };
    std::atomic<bool> audioRunning { false };
    std::atomic<int> realtimeSchedulingStatus { -1 };
    std::atomic<float> dspLoad { 0.0f };
    std::atomic<int> dspNearOverloadCount { 0 };
    std::atomic<float> callbackIntervalLoad { 0.0f };
    std::atomic<int> lateCallbackCount { 0 };
    int dspWarmupCallbacksRemaining = 0;
    int callbackTimingWarmupRemaining = 0;
    juce::int64 previousAudioCallbackTick = 0;
    double previousAudioCallbackPeriod = 0.0;
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
    std::atomic<int> scenarioMorphSource { 0 };
    std::atomic<float> scenarioMorphProgress { 1.0f };
    std::atomic<float> requestedTexture { 0.0f };
    std::atomic<bool> fuzzEnabled { false };
    std::atomic<bool> bassEnabled { true };
    bool fourHeadSaxLoopPlaybackActive = false;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        fourHeadSaxLoopMix;
    float fourHeadMixBlockStart = 0.0f;
    float fourHeadMixBlockEnd = 0.0f;
    std::array<double, 4> cosmosHeadPositions {};
    double cosmosModulationPhase = 0.0;
    int64_t scenarioMorphElapsedSamples = 0;
    int64_t scenarioMorphTotalSamples = 0;
    float scenarioMorphDecaySource = 0.985f;
    float scenarioMorphDecayTarget = 0.985f;
    uint32_t scenarioMorphDecayRevision = 0;
    bool scenarioMorphDecayActive = false;
    float activeTexture = -1.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        grainEffectMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        fuzzEffectMix;
    std::array<float, logicalOutputBusCount> grainHeldSamples {};
    float activeGrainEffectTarget = -1.0f;
    float activeFuzzEffectTarget = -1.0f;
    int grainHoldCounter = 0;
    bool bassWasEnabled = true;
    int64_t saxDangerSamples = 0;
    int64_t saxRecoverySamples = 0;
    float saxSafetyGain = 1.0f;
    double diagnosticTonePhase = 0.0;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EcosystemEngine)
};
