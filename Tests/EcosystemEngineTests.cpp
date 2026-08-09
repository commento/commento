#include <JuceHeader.h>
#include "Engine/EcosystemEngine.h"
#include "Hardware/Model12AudioRouter.h"

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

void process(juce::AudioIODeviceCallback& engine,
             const float* const* inputs, int inputChannels,
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
    engine.enqueueMidiMessage(juce::MidiMessage::noteOn(5, 60, 0.8f));
    engine.enqueueMidiMessage(juce::MidiMessage::noteOff(5, 60));
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

    EcosystemEngine multichannelEngine;
    multichannelEngine.prepare(48000.0);
    std::array<std::vector<float>, 10> multichannelStorage;
    std::array<float*, 10> multichannelOutputs {};
    for (size_t channel = 0; channel < multichannelStorage.size(); ++channel)
    {
        multichannelStorage[channel].assign(512, 0.5f);
        multichannelOutputs[channel] = multichannelStorage[channel].data();
    }

    multichannelEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(5, 60, 0.9f));
    process(multichannelEngine, nullptr, 0, multichannelOutputs.data(), 10, 512);
    const auto stereoIsAudible = std::any_of(
        multichannelStorage[0].begin(), multichannelStorage[0].end(),
        [](float sample) { return std::abs(sample) > 0.000001f; });
    passed &= expect(stereoIsAudible, "le uscite USB 1-2 devono contenere il mix");

    for (size_t channel = 2; channel < multichannelStorage.size(); ++channel)
    {
        const auto silent = std::all_of(
            multichannelStorage[channel].begin(), multichannelStorage[channel].end(),
            [](float sample) { return sample == 0.0f; });
        passed &= expect(silent, "le uscite USB 3-10 devono restare silenziose");
    }

    EcosystemEngine routedEngine;
    routedEngine.prepare(48000.0, 256);
    Model12AudioRouter router(routedEngine);
    std::array<std::vector<float>, 12> physicalInputStorage;
    std::array<const float*, 12> physicalInputs {};
    for (size_t channel = 0; channel < physicalInputStorage.size(); ++channel)
    {
        const auto value = channel == 0 ? 0.9f : (channel == 6 ? 0.2f : -0.3f);
        physicalInputStorage[channel].assign(4800, value);
        physicalInputs[channel] = physicalInputStorage[channel].data();
    }
    std::array<std::vector<float>, 10> physicalOutputStorage;
    std::array<float*, 10> physicalOutputs {};
    for (size_t channel = 0; channel < physicalOutputStorage.size(); ++channel)
    {
        physicalOutputStorage[channel].assign(4800, 0.75f);
        physicalOutputs[channel] = physicalOutputStorage[channel].data();
    }

    routedEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    process(router, physicalInputs.data(), 12, physicalOutputs.data(), 10, 4800);
    routedEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    process(router, nullptr, 0, physicalOutputs.data(), 10, 256);

    const auto routedPeak = *std::max_element(
        physicalOutputStorage[0].begin(),
        physicalOutputStorage[0].begin() + 256);
    passed &= expect(routedPeak > 0.05f && routedPeak < 0.3f,
                     "il router deve acquisire il fisico 7 e ignorare il fisico 1");
    passed &= expect(std::equal(physicalOutputStorage[0].begin(),
                                physicalOutputStorage[0].begin() + 256,
                                physicalOutputStorage[1].begin()),
                     "il sax mono deve essere centrato sulle uscite 1-2");
    for (size_t channel = 2; channel < physicalOutputStorage.size(); ++channel)
    {
        const auto silent = std::all_of(
            physicalOutputStorage[channel].begin(),
            physicalOutputStorage[channel].begin() + 256,
            [](float sample) { return sample == 0.0f; });
        passed &= expect(silent, "il router deve azzerare le uscite fisiche 3-10");
    }
    passed &= expect(router.getPhysicalInputChannelCount() == 0
                         && router.getPhysicalOutputChannelCount() == 10,
                     "il router deve esporre il conteggio fisico del callback corrente");

    routedEngine.prepare(48000.0, 256);
    passed &= expect(routedEngine.hasMaterial(EcosystemEngine::midiMemoryCount),
                     "un riavvio a 48 kHz deve conservare il loop del sax");

    if (passed)
        std::cout << "Commento engine tests passed\n";
    return passed ? 0 : 1;
}
