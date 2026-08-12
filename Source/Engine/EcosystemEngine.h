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

    enum class LoopEvolution : int
    {
        normal = 0,
        octaveUp,
        reverse
    };

    enum class MidiInputRole : int
    {
        generic = 0,
        keyStep,
        model12,
        nm2
    };

    // Physical order on the NM2: three rows of six pads. The factory mapping
    // already sends these as notes 60..77 on MIDI channel 1.
    //
    // A player wearing the controller memorises geography, not names, so each
    // row is one family: the top row throws something that keeps sounding, the
    // middle row recolours the tone, the bottom row moves it or changes its
    // level. Within a row the gestures run from the gentlest to the most
    // extreme.
    enum class Nm2Gesture : int
    {
        // Top row: time and space.
        codaLibera = 0,
        ecoThrow,
        gelo,
        caduta,
        scatto,
        abisso,
        // Middle row: timbre.
        ombra,
        radio,
        lama,
        grana,
        fuzz,
        ferro,
        // Bottom row: movement and levels.
        pulso,
        orbita,
        stretto,
        vuoto,
        ascolto,
        pausa,
        count
    };

    enum class SaxFootswitchMessageType : int
    {
        none = 0,
        controller
    };

    struct SaxFootswitchBinding
    {
        MidiInputRole role = MidiInputRole::generic;
        SaxFootswitchMessageType type = SaxFootswitchMessageType::none;
        int number = -1;

        [[nodiscard]] bool valid() const noexcept
        {
            const auto validRole = role >= MidiInputRole::generic
                && role <= MidiInputRole::model12;
            if (! validRole)
                return false;

            if (type == SaxFootswitchMessageType::controller)
                return juce::isPositiveAndBelow(number, 128);
            return false;
        }
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
    static constexpr int nm2MidiChannel = 1;
    static constexpr int nm2BaseNote = 60;
    // Factory mapping of the motion sensor. The two axes arrive on their own
    // channels, but Commento owns every message from this endpoint, so the
    // controller number alone is enough to recognise them even under a
    // customised preset.
    static constexpr int nm2TiltXController = 74;
    static constexpr int nm2TiltYController = 75;
    static constexpr int nm2GestureCount
        = static_cast<int>(Nm2Gesture::count);
    static constexpr double scenarioMorphSeconds = 8.0;
    static_assert(PerformanceLevels::count == memoryCount);

    EcosystemEngine();

    void enqueueMidiMessage(
        const juce::MidiMessage& message,
        MidiInputRole role = MidiInputRole::generic);
    void toggleRecording(int memoryIndex);
    void clearMemory(int memoryIndex);
    void beginSaxFootswitchLearn() noexcept;
    void cancelSaxFootswitchLearn() noexcept;
    void clearSaxFootswitchBinding() noexcept;
    [[nodiscard]] bool isSaxFootswitchLearning() const noexcept;
    [[nodiscard]] bool hasSaxFootswitchBinding() const noexcept;
    [[nodiscard]] SaxFootswitchBinding getSaxFootswitchBinding() const noexcept;
    void setSaxFootswitchBinding(SaxFootswitchBinding binding) noexcept;
    void releaseSaxFootswitch() noexcept;
    void releaseNm2Gestures() noexcept;
    // What the dedicated endpoint last sent. Without this a controller whose
    // preset no longer matches the factory grid is swallowed in silence and
    // looks identical to one that is not connected at all.
    struct Nm2Diagnostics
    {
        enum class Kind : int { none = 0, noteOn, noteOff, controller, other };
        Kind kind = Kind::none;
        int number = -1;
        int channel = 0;
        int value = 0;
        int messageCount = 0;
    };

    [[nodiscard]] Nm2Diagnostics getNm2Diagnostics() const noexcept;
    void resetNm2Diagnostics() noexcept;
    [[nodiscard]] std::uint32_t getNm2HeldMask() const noexcept;
    [[nodiscard]] bool isNm2GestureHeld(Nm2Gesture gesture) const noexcept;
    [[nodiscard]] float getNm2TiltDepth() const noexcept;
    [[nodiscard]] bool hasNm2TiltSensor() const noexcept;
    [[nodiscard]] static const char* getNm2GestureName(
        Nm2Gesture gesture) noexcept;

    [[nodiscard]] bool isRecording(int memoryIndex) const;
    [[nodiscard]] bool isWaitingForFirstNote(int memoryIndex) const;
    [[nodiscard]] bool hasMaterial(int memoryIndex) const;
    [[nodiscard]] double getPhase(int memoryIndex) const;
    [[nodiscard]] double getLengthSeconds(int memoryIndex) const;
    [[nodiscard]] int getEventCount(int memoryIndex) const;
    [[nodiscard]] LoopEvolution getLoopEvolution(int memoryIndex) const noexcept;
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
    void setLoopEvolutionEnabled(bool shouldBeEnabled) noexcept;
    [[nodiscard]] bool isLoopEvolutionEnabled() const noexcept;
    void setBassEnabled(bool shouldBeEnabled);
    [[nodiscard]] bool isBassEnabled() const;
    void setPerformanceLevel(int memoryIndex, float linearGain) noexcept;
    [[nodiscard]] float getPerformanceLevel(int memoryIndex) const noexcept;
    void setDelayLevel(int memoryIndex, float amount) noexcept;
    [[nodiscard]] float getDelayLevel(int memoryIndex) const noexcept;
    void setGestureTarget(int memoryIndex) noexcept;
    void setFreezeEnabled(int memoryIndex, bool shouldFreeze) noexcept;
    [[nodiscard]] bool isFreezeEnabled(int memoryIndex) const noexcept;
    void setEchoThrowEnabled(int memoryIndex, bool shouldThrow) noexcept;
    [[nodiscard]] bool isEchoThrowEnabled(int memoryIndex) const noexcept;
    void setFreeTailEnabled(int memoryIndex, bool shouldReleaseTail) noexcept;
    [[nodiscard]] bool isFreeTailEnabled(int memoryIndex) const noexcept;
    void setThinningEnabled(bool shouldThin) noexcept;
    [[nodiscard]] bool isThinningEnabled() const noexcept;
    [[nodiscard]] int getThinnedMemoryIndex() const noexcept;
    void setLoopPlaying(int memoryIndex, bool shouldPlay) noexcept;
    [[nodiscard]] bool isLoopPlaying(int memoryIndex) const noexcept;
    void setSaxListenAmount(float amount) noexcept;
    [[nodiscard]] float getSaxListenAmount() const noexcept;
    void releaseMomentaryGestures() noexcept;

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
        // Logical controller/note state immediately before playbackPosition.
        // PAUSA silences only the private stored-loop voices but preserves
        // this fixed-size snapshot, so PLAY can rebuild held and sustained
        // notes without scanning the event list in the realtime callback.
        std::array<bool, 128> playbackKeyDownNotes {};
        std::array<bool, 128> playbackActiveNotes {};
        std::array<float, 128> playbackNoteVelocities {};
        bool playbackSustainDown = false;
        int playbackPitchWheel = 8192;
        int playbackModulation = 0;
        int playbackBrightness = 0;
        int playbackPressure = 0;
        bool restorePlaybackNotesNextBlock = false;
        // Latest live controller state seeds a take when SEMINA is armed and
        // the first note arrives after a pedal/bend message. These values are
        // audio-thread-owned and do not add events while merely waiting.
        bool liveSustainDown = false;
        int livePitchWheel = 8192;
        int liveModulation = 0;
        int liveBrightness = 0;
        int livePressure = 0;
        int evolutionNote = -1;
        float evolutionVelocity = 0.25f;
        std::uint32_t evolutionRandomState = 0x9e3779b9u;
        LoopEvolution evolution = LoopEvolution::normal;
        std::atomic<int> evolutionForDisplay {
            static_cast<int>(LoopEvolution::normal)
        };
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
        bool clearStartedWhilePaused = false;
        std::uint32_t evolutionRandomState = 0xa341316cu;
        LoopEvolution evolution = LoopEvolution::normal;
        std::atomic<int> evolutionForDisplay {
            static_cast<int>(LoopEvolution::normal)
        };
        int64_t evolutionStartPosition = 0;
        int64_t evolutionDurationSamples = 0;
        double evolutionSourcePosition = 0.0;
    };

    static constexpr int incomingCapacity = 512;
    static constexpr int maximumMidiEvents = 8192;
    static constexpr double maximumAudioSeconds = 120.0;
    static constexpr std::array<int, midiMemoryCount> midiChannels { 5, 2, 3, 4 };
    // Stored loop voices use private channels inside each AmbientSynth. Live
    // keyboard messages stay on 2/3/4, so PAUSA never mutes the performer.
    static constexpr std::array<int, midiMemoryCount> playbackMidiChannels {
        0, 12, 13, 14
    };
    static constexpr std::array<int, midiMemoryCount> evolutionMidiChannels {
        0, 9, 10, 11
    };

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
    void updateMomentaryGestureTargets(int numSamples) noexcept;
    void updateNm2EffectTargets(int numSamples) noexcept;
    void updateNm2TiltDepth(int numSamples) noexcept;
    void updateNm2ColourFilter() noexcept;
    void applyLoopTransportCommands(juce::MidiBuffer& output) noexcept;
    static void addPrivateLoopPanic(juce::MidiBuffer& output,
                                    int playbackChannel, int ghostChannel,
                                    int sampleOffset) noexcept;
    static void resetMidiPlaybackSnapshot(MidiMemory& memory) noexcept;
    void restoreMidiNotesAtPlayhead(int memoryIndex,
                                    juce::MidiBuffer& output,
                                    int sampleOffset = 0) noexcept;
    void resetLoopTransportState(bool resetRequests) noexcept;
    void updateThinningState() noexcept;
    void scheduleNextThinning(bool firstGesture) noexcept;
    void startThinningCycle(int memoryIndex, int outputOffset,
                            int numSamples, juce::MidiBuffer& output) noexcept;
    void finishThinningCycle(int memoryIndex, int outputOffset,
                             int blockSamples,
                             juce::MidiBuffer& output) noexcept;
    void cancelThinningForMemory(int memoryIndex) noexcept;
    void setThinningGainTarget(int memoryIndex, float target,
                               int transitionOffset) noexcept;
    void prepareSaxListenBlock(int numSamples) noexcept;
    void processPerformanceEffects(float* const* outputs, int outputChannels,
                                   int numSamples) noexcept;
    void processNm2Effects(float* const* outputs, int outputChannels,
                           int numSamples) noexcept;
    void recordIncomingMidi(int numSamples, juce::MidiBuffer& liveMidi);
    void renderMidiMemories(int numSamples, juce::MidiBuffer& output);
    void renderMidiSegment(MidiMemory& memory, int memoryIndex,
                           int64_t segmentStart, int segmentLength,
                           int outputOffset, juce::MidiBuffer& output,
                           bool emitEvents = true);
    void renderMidiEvolutionSegment(const MidiMemory& memory, int memoryIndex,
                                    int64_t segmentStart, int segmentLength,
                                    int outputOffset, juce::MidiBuffer& output);
    void chooseMidiEvolution(MidiMemory& memory, int memoryIndex) noexcept;
    void chooseAudioEvolution() noexcept;
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
    void setMidiFreezeEnabled(bool shouldFreeze) noexcept;
    void setMidiEchoThrowEnabled(bool shouldThrow) noexcept;
    void setMidiFreeTailEnabled(bool shouldReleaseTail) noexcept;
    void clearMomentaryGestures() noexcept;
    [[nodiscard]] bool consumeNm2Message(
        const juce::MidiMessage& message, MidiInputRole role) noexcept;
    void releaseNm2GesturesUnlocked() noexcept;
    void setNm2TargetGesture(
        std::array<std::atomic<std::uint8_t>, memoryCount>& masks,
        std::atomic<int>& capturedTarget, std::uint8_t ownerBit,
        bool shouldEnable) noexcept;
    [[nodiscard]] bool consumeSaxFootswitchMessage(
        const juce::MidiMessage& message, MidiInputRole role) noexcept;

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
    // Bit 0 is owned by the touchscreen, bit 1 by a dedicated MIDI CC and bit
    // 2 by the NM2. This
    // prevents one control source from releasing a gesture still held by the
    // other without locks or message-thread callbacks.
    std::array<std::atomic<std::uint8_t>, memoryCount> freezeGestureMasks;
    std::array<std::atomic<std::uint8_t>, memoryCount> echoThrowGestureMasks;
    std::array<std::atomic<std::uint8_t>, memoryCount> freeTailGestureMasks;
    std::atomic<int> gestureTarget { bassLayerIndex };
    std::atomic<int> midiFreezeTarget { -1 };
    std::atomic<int> midiEchoThrowTarget { -1 };
    std::atomic<int> midiFreeTailTarget { -1 };
    std::atomic<std::uint32_t> nm2HeldMask { 0u };
    std::atomic<int> nm2LastEventKind { 0 };
    std::atomic<int> nm2LastEventNumber { -1 };
    std::atomic<int> nm2LastEventChannel { 0 };
    std::atomic<int> nm2LastEventValue { 0 };
    std::atomic<int> nm2EventCount { 0 };
    // Only MIDI/device-management threads enter this very short critical
    // section. The audio callback merely reads the atomics above/below.
    juce::SpinLock nm2ControlLock;
    std::atomic<int> nm2FreezeTarget { -1 };
    std::atomic<int> nm2EchoThrowTarget { -1 };
    std::atomic<int> nm2FreeTailTarget { -1 };
    std::atomic<int> nm2PauseTarget { -1 };
    std::atomic<int> nm2AbyssTarget { -1 };
    // Binding, learn and edge state share one lock-free word, so MIDI input and
    // UI threads cannot observe a partially published learned assignment.
    std::atomic<std::uint32_t> saxFootswitchState { 0u };
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>,
               memoryCount> echoThrowMixes;
    std::array<float, memoryCount> echoThrowBlockAmounts {};
    std::array<bool, memoryCount> echoThrowWasActive {};
    // Requested state is written by UI/control threads; the remaining state
    // and smoothers belong exclusively to the audio callback.
    std::array<std::atomic<bool>, memoryCount> loopPlayingRequested;
    std::array<bool, memoryCount> loopPlayingApplied {};
    std::array<juce::SmoothedValue<float,
               juce::ValueSmoothingTypes::Linear>, memoryCount>
        loopTransportGains;
    std::atomic<bool> thinningEnabled { false };
    std::atomic<int> thinnedMemoryForDisplay { -1 };
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>,
               midiMemoryCount> thinningGains;
    std::array<int, midiMemoryCount> thinningTransitionOffsets {};
    int activeThinnedMemory = -1;
    int scheduledThinningMemory = -1;
    int64_t nextThinningSample = 0;
    std::uint32_t thinningRandomState = 0x6d2b79f5u;
    bool thinningWasEnabled = false;
    std::atomic<float> saxListenAmount { 0.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> saxListenMix;
    float saxListenEnvelope = 0.0f;
    float saxListenBlockGainStart = 1.0f;
    float saxListenBlockGainEnd = 1.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bassMuteGain;
    std::atomic<float> audioDecay { 0.985f };
    std::atomic<uint32_t> audioDecayManualRevision { 0 };
    // Model 12 profile captures the sax from the configured 7/8 pair. Mono
    // remains available from the large RESPIRO button when only input 7 is used.
    std::atomic<bool> saxStereoInput { true };
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
    std::atomic<bool> loopEvolutionEnabled { false };
    std::atomic<bool> bassEnabled { true };
    bool fourHeadSaxLoopPlaybackActive = false;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        fourHeadSaxLoopMix;
    float fourHeadMixBlockStart = 0.0f;
    float fourHeadMixBlockEnd = 0.0f;
    std::array<double, 4> cosmosHeadPositions {};
    std::array<float, 2> audioLoopLastSamples {};
    double cosmosModulationPhase = 0.0;
    std::array<float, 2> audioEvolutionFilteredSamples {};
    float audioEvolutionLowPassCoefficient = 1.0f;
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
    std::array<float, logicalOutputBusCount> grainFilteredSamples {};
    // The NM2 crush is far harsher than the scenario texture, and the two
    // cannot share a hold counter or a quantisation: the factory bass levels
    // are calibrated against the global one, so it has to stay exactly as it
    // is while the wearable pad hits much harder on the sax bus alone.
    std::array<float, logicalOutputBusCount> nm2GrainHeldSamples {};
    std::array<float, logicalOutputBusCount> nm2GrainFilteredSamples {};
    int nm2GrainHoldCounter = 0;
    bool nm2GrainFilterNeedsPrime = true;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2GrainMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2FuzzMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2DarkMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2RadioMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2NarrowMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2EmptyMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2BladeMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2PulseMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2MetalMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2OrbitMix;
    // One topology-preserving state-variable filter per sax channel replaces
    // the four one-pole sections OMBRA, RADIO and LAMA used to own. It costs
    // slightly less per sample than those four, produces low, band and high
    // outputs at once, and adds the resonance that made the old tone tilts
    // sound like a tone knob rather than a gesture.
    std::array<float, logicalOutputBusCount> nm2ColourFilterState1 {};
    std::array<float, logicalOutputBusCount> nm2ColourFilterState2 {};
    // Cutoff envelopes are deliberately separate from the wet mixes: LAMA has
    // to appear almost immediately while its corner is still climbing, and
    // OMBRA has to keep sinking after it is already fully wet.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2DarkEnvelope;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2RadioEnvelope;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>
        nm2BladeEnvelope;
    // Recomputed once per block. A cutoff ramp spread over hundreds of
    // milliseconds moves by a fraction of a percent across one block, so the
    // coefficient steps are inaudible and the per-sample tan() disappears.
    float nm2ColourG = 0.0f;
    float nm2ColourK = 1.0f;
    float nm2ColourA1 = 0.0f;
    float nm2ColourA2 = 0.0f;
    float nm2ColourA3 = 0.0f;
    float nm2ColourLowWeight = 0.0f;
    float nm2ColourBandWeight = 0.0f;
    float nm2ColourHighWeight = 0.0f;
    float nm2ColourMakeup = 1.0f;
    double nm2PulsePhase = 0.0;
    double nm2MetalPhase = 0.0;
    double nm2OrbitPhase = 0.0;
    std::uint32_t nm2PreviousHeldMask = 0u;
    bool nm2FiltersNeedPrime = true;
    // Tilt depth. Until the sensor is enabled on the controller no CC ever
    // arrives, nm2TiltSeen stays false and every gesture keeps its full
    // strength, so a controller with tilt switched off behaves exactly as it
    // did before this existed.
    std::atomic<float> nm2TiltX { 0.5f };
    std::atomic<float> nm2TiltY { 0.5f };
    std::atomic<bool> nm2TiltSeen { false };
    float nm2TiltReferenceX = 0.5f;
    float nm2TiltReferenceY = 0.5f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> nm2TiltDepth;
    // Written once per block by the audio thread and readable from the UI, so
    // it follows the same convention as the other display-facing values.
    std::atomic<float> nm2TiltBlockDepth { 1.0f };
    float grainLowPassCoefficient = 1.0f;
    float activeGrainEffectTarget = -1.0f;
    float activeFuzzEffectTarget = -1.0f;
    int grainHoldCounter = 0;
    bool grainFilterNeedsPrime = true;
    bool bassWasEnabled = true;
    int activeMidiEvolutionMemory = -1;
    int64_t evolutionSampleClock = 0;
    int64_t nextEvolutionSample = 0;
    bool evolutionWasEnabled = false;
    int64_t saxDangerSamples = 0;
    int64_t saxRecoverySamples = 0;
    float saxSafetyGain = 1.0f;
    double diagnosticTonePhase = 0.0;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EcosystemEngine)
};
