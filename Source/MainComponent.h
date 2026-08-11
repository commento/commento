#pragma once

#include <JuceHeader.h>
#include "Engine/EcosystemEngine.h"
#include "Hardware/Model12AudioRouter.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class MemoryOrb;
class ConnectionChoice;

class MainComponent final : public juce::Component,
                            private juce::MidiInputCallback,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    struct ChannelRouteOption
    {
        juce::String name;
        int left = Model12AudioRouter::RoutingConfig::none;
        int right = Model12AudioRouter::RoutingConfig::none;
    };

    struct AudioConnectionDraft
    {
        int profile = 0; // 0 Model 12, 1 stereo generico, 2 personalizzato
        juce::String backend;
        juce::String inputDevice;
        juce::String outputDevice;
        double sampleRate = 48000.0;
        int bufferSize = 512;
        Model12AudioRouter::RoutingConfig routing;
        EcosystemEngine::SaxPathMode saxPath =
            EcosystemEngine::SaxPathMode::sceneEffects;
        EcosystemEngine::DiagnosticToneBus tone =
            EcosystemEngine::DiagnosticToneBus::off;
    };

    void initialiseAudio();
    void handleIncomingMidiMessage(juce::MidiInput*,
                                   const juce::MidiMessage& message) override;
    void timerCallback() override;
    void selectMemory(int index);
    void updateControls();
    void toggleSettings();
    void toggleGestures();
    void updatePageVisibility();
    void updateMidiMonitor();
    void scanAudioDevices();
    void applyAudioConfiguration();
    void applyAudioProfile(int profile);
    void refreshDeviceChoices();
    void refreshAudioCapabilities();
    void syncConnectionControls();
    void markConnectionDraftCustom();
    void saveAudioConfiguration();
    [[nodiscard]] juce::AudioIODeviceType* getDraftDeviceType();
    void configureKeyStepMidi();
    void loadSaxFootswitchBinding();
    void saveSaxFootswitchBinding(bool flushToDisk);
    void updateSaxFootswitchControls();
    void updateHardwareIndicators();
    void updateClearHold();
    void changeScenario(int delta);
    void applyScenario(int index);
    void updateScenarioLabels();
    void cycleTexture();
    void toggleSaxListening();
    void updateTextureButton();
    void updatePerformanceLevelControl();
    void savePerformanceLevels(bool flushToDisk);

    EcosystemEngine engine;
    Model12AudioRouter audioRouter { engine };
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<juce::LookAndFeel> interfaceLookAndFeel;
    std::array<std::unique_ptr<MemoryOrb>, EcosystemEngine::memoryCount> orbs;
    std::array<std::unique_ptr<juce::TextButton>, EcosystemEngine::memoryCount>
        gestureTargetButtons;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::TextButton previousScenarioButton { "<" };
    juce::TextButton nextScenarioButton { ">" };
    juce::Label scenarioLabel;
    juce::Label statusLabel;
    juce::Label audioStatusLabel;
    juce::Label midiStatusLabel;
    juce::TextButton recordButton { "SEMINA" };
    juce::TextButton clearButton { "DIMENTICA" };
    juce::TextButton settingsButton { "CONNESSIONI" };
    juce::TextButton gesturesButton { "GESTI" };
    juce::TextButton textureButton { "GRANA: PULITA" };
    juce::TextButton fuzzButton { "FUZZ: SPENTO" };
    juce::TextButton evolutionButton { "DERIVA: SPENTA" };
    juce::TextButton freezeButton { "GELO" };
    juce::TextButton echoThrowButton { "ECO THROW" };
    juce::TextButton freeTailButton { "CODA LIBERA: TIENI" };
    juce::TextButton thinningButton { "DIRADA: SPENTA" };
    juce::TextButton saxListenButton { "ASCOLTO: SPENTO" };
    juce::Label gesturesTitleLabel;
    juce::Label gesturesHintLabel;
    juce::Label gestureTargetLabel;
    juce::Label sustainMonitorLabel;
    juce::TextButton applyAudioButton { "APPLICA AUDIO" };
    juce::TextButton rescanAudioButton { "RILEGGI DISPOSITIVI" };
    juce::TextButton keyStepRoutingButton { "RILEGGI MIDI" };
    juce::Label connectionStatusLabel;
    juce::Label hardwareRouteLabel;
    juce::Label midiConnectionLabel;
    juce::Label saxFootswitchBindingLabel;
    juce::TextButton saxFootswitchLearnButton { "IMPARA PEDALE SAX" };
    juce::TextButton saxFootswitchClearButton { "RIMUOVI" };
    juce::TextButton saxModeButton { "MONO DA INGRESSO 7" };
    juce::Slider decaySlider;
    juce::Label decayLabel;
    juce::Slider performanceLevelSlider;
    juce::Label performanceLevelLabel;
    juce::TextButton resetPerformanceLevelButton { "0 dB" };
    juce::Slider delayLevelSlider;
    juce::Label delayLevelLabel;
    juce::TextButton toggleDelayDryButton { "ASCIUTTO" };
    juce::ApplicationProperties properties;

    std::unique_ptr<ConnectionChoice> profileChoice;
    std::unique_ptr<ConnectionChoice> backendChoice;
    std::unique_ptr<ConnectionChoice> inputDeviceChoice;
    std::unique_ptr<ConnectionChoice> outputDeviceChoice;
    std::unique_ptr<ConnectionChoice> sampleRateChoice;
    std::unique_ptr<ConnectionChoice> bufferChoice;
    std::unique_ptr<ConnectionChoice> saxInputChoice;
    std::unique_ptr<ConnectionChoice> ambientOutputChoice;
    std::unique_ptr<ConnectionChoice> bassOutputChoice;
    std::unique_ptr<ConnectionChoice> saxOutputChoice;
    std::unique_ptr<ConnectionChoice> saxPathChoice;
    std::unique_ptr<ConnectionChoice> diagnosticToneChoice;

    AudioConnectionDraft audioDraft;
    juce::StringArray backendNames;
    juce::StringArray inputDeviceNames;
    juce::StringArray outputDeviceNames;
    juce::Array<double> availableSampleRates;
    juce::Array<int> availableBufferSizes;
    std::vector<ChannelRouteOption> saxInputRoutes;
    std::vector<ChannelRouteOption> outputPairRoutes;
    std::vector<ChannelRouteOption> bassOutputRoutes;
    int draftInputChannelCount = 0;
    int draftOutputChannelCount = 0;
    bool draftInputCapabilitiesKnown = false;
    bool draftOutputCapabilitiesKnown = false;
    bool updatingConnectionControls = false;
    bool connectionDraftDirty = false;
    int appliedXRunBaseline = 0;
    juce::String lastAudioError;

    int selectedMemory = 0;
    bool settingsVisible = false;
    bool gesturesVisible = false;
    bool audioReady = false;
    juce::String keyStepInputName;
    juce::String model12MidiInputName;
    EcosystemEngine::SaxFootswitchBinding persistedSaxFootswitchBinding;
    double clearHoldStartedAt = -1.0;
    bool clearHoldTriggered = false;
    double animationPhase = 0.0;
    int touchscreenFreezeTarget = -1;
    int touchscreenEchoThrowTarget = -1;
    int touchscreenFreeTailTarget = -1;
    std::atomic<std::uint32_t> lastMidiMessagePacked { 0u };
    std::atomic<std::uint32_t> lastMidiMessageTick { 0u };
    std::atomic<int> lastSustainValue { -1 };
    std::atomic<std::uint32_t> lastSustainTick { 0u };
    std::atomic<std::uint32_t> sustainEdgeMask { 0u };
    juce::Rectangle<int> saxControlPanelBounds;
    juce::Rectangle<int> gesturesMainPanelBounds;
    juce::Rectangle<int> gesturesPedalPanelBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
