#include <JuceHeader.h>
#include "Engine/EcosystemEngine.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
bool expect(bool condition, const char* message)
{
    if (! condition)
        std::cerr << "FAILED: " << message << '\n';
    return condition;
}

void process(EcosystemEngine& engine, const float* const* inputs, int inputChannels,
             float* const* outputs, int outputChannels, int samples)
{
    juce::AudioIODeviceCallbackContext context {};
    engine.audioDeviceIOCallbackWithContext(inputs, inputChannels, outputs,
                                             outputChannels, samples, context);
}
}

int main()
{
    bool passed = true;
    EcosystemEngine engine;
    engine.prepare(48000.0);

    std::array<std::vector<float>, 2> outputStorage {
        std::vector<float>(4800, 0.0f), std::vector<float>(4800, 0.0f)
    };
    std::array<float*, 2> outputs {
        outputStorage[0].data(), outputStorage[1].data()
    };

    engine.enqueueMidiMessage(juce::MidiMessage::noteOn(2, 55, 0.8f));
    process(engine, nullptr, 0, outputs.data(), 2, 480);
    const auto internalVoiceAudible = std::any_of(
        outputStorage[0].begin(), outputStorage[0].begin() + 480,
        [](float sample) { return std::abs(sample) > 0.000001f; });
    passed &= expect(internalVoiceAudible,
                     "il canale MIDI 2 deve suonare il motore interno 2");
    engine.enqueueMidiMessage(juce::MidiMessage::noteOff(2, 55));
    process(engine, nullptr, 0, outputs.data(), 2, 480);
    std::fill(outputStorage[0].begin(), outputStorage[0].end(), 0.0f);
    std::fill(outputStorage[1].begin(), outputStorage[1].end(), 0.0f);

    engine.toggleRecording(0);
    process(engine, nullptr, 0, outputs.data(), 2, 480);
    engine.enqueueMidiMessage(juce::MidiMessage::noteOn(1, 60, 0.8f));
    engine.enqueueMidiMessage(juce::MidiMessage::noteOff(1, 60));
    process(engine, nullptr, 0, outputs.data(), 2, 480);
    engine.toggleRecording(0);
    process(engine, nullptr, 0, outputs.data(), 2, 480);

    passed &= expect(engine.hasMaterial(0), "la memoria MIDI deve contenere materiale");
    passed &= expect(engine.getEventCount(0) == 2, "la memoria MIDI deve contenere due eventi");
    passed &= expect(std::abs(engine.getLengthSeconds(0) - 0.02) < 0.0001,
                     "la durata MIDI deve seguire il tempo libero registrato");

    std::array<std::vector<float>, 2> inputStorage {
        std::vector<float>(4800, 0.2f), std::vector<float>(4800, -0.2f)
    };
    std::array<const float*, 2> inputs {
        inputStorage[0].data(), inputStorage[1].data()
    };

    engine.toggleRecording(EcosystemEngine::midiMemoryCount);
    process(engine, inputs.data(), 2, outputs.data(), 2, 4800);
    engine.toggleRecording(EcosystemEngine::midiMemoryCount);
    process(engine, nullptr, 0, outputs.data(), 2, 128);

    passed &= expect(engine.hasMaterial(EcosystemEngine::midiMemoryCount),
                     "la memoria audio deve contenere materiale");
    passed &= expect(std::abs(engine.getLengthSeconds(EcosystemEngine::midiMemoryCount) - 0.1)
                         < 0.0001,
                     "la memoria audio deve conservare la durata registrata");

    std::fill(outputStorage[0].begin(), outputStorage[0].end(), 0.0f);
    std::fill(outputStorage[1].begin(), outputStorage[1].end(), 0.0f);
    process(engine, nullptr, 0, outputs.data(), 2, 480);
    const auto audible = std::any_of(outputStorage[0].begin(),
                                     outputStorage[0].begin() + 480,
                                     [](float sample) { return std::abs(sample) > 0.01f; });
    passed &= expect(audible, "la memoria audio deve essere riprodotta");

    if (passed)
        std::cout << "Commento engine tests passed\n";
    return passed ? 0 : 1;
}
