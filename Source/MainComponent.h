#pragma once

#include <JuceHeader.h>
#include "Engine/EcosystemEngine.h"
#include <array>
#include <memory>

class MemoryOrb;

class MainComponent final : public juce::Component,
                            private juce::MidiInputCallback,
                            private juce::Timer,
                            private juce::HighResolutionTimer
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
    void hiResTimerCallback() override;
    void selectMemory(int index);
    void updateControls();
    void toggleSettings();

    EcosystemEngine engine;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    std::array<std::unique_ptr<MemoryOrb>, EcosystemEngine::memoryCount> orbs;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::Label statusLabel;
    juce::TextButton recordButton { "SEMINA" };
    juce::TextButton clearButton { "DIMENTICA" };
    juce::TextButton settingsButton { "CONNESSIONI" };
    juce::Slider decaySlider;
    juce::Label decayLabel;

    int selectedMemory = 0;
    bool settingsVisible = false;
    double animationPhase = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
