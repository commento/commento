#pragma once

#include <JuceHeader.h>
#include "Engine/EcosystemEngine.h"
#include "Hardware/Model12AudioRouter.h"
#include <array>
#include <memory>

class MemoryOrb;

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
    void initialiseAudio();
    void handleIncomingMidiMessage(juce::MidiInput*,
                                   const juce::MidiMessage& message) override;
    void timerCallback() override;
    void selectMemory(int index);
    void updateControls();
    void toggleSettings();
    void configureModel12Routing();
    void configureKeyStepMidi();
    void updateHardwareIndicators();
    void updateClearHold();
    void changeScenario(int delta);
    void applyScenario(int index);
    void updateScenarioLabels();

    EcosystemEngine engine;
    Model12AudioRouter audioRouter { engine };
    juce::AudioDeviceManager deviceManager;
    std::array<std::unique_ptr<MemoryOrb>, EcosystemEngine::memoryCount> orbs;

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
    juce::TextButton model12RoutingButton { "RIPROVA MODEL 12" };
    juce::TextButton keyStepRoutingButton { "RIPROVA KEYSTEP PRO" };
    juce::Label connectionStatusLabel;
    juce::Label hardwareRouteLabel;
    juce::Label midiMapLabel;
    juce::Label midiConnectionLabel;
    juce::TextButton saxModeButton { "MONO DA INGRESSO 7" };
    juce::Slider decaySlider;
    juce::Label decayLabel;
    juce::ApplicationProperties properties;

    int selectedMemory = 0;
    bool settingsVisible = false;
    bool model12Ready = false;
    juce::String keyStepInputName;
    double clearHoldStartedAt = -1.0;
    bool clearHoldTriggered = false;
    double animationPhase = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
