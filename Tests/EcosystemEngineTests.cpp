#include <JuceHeader.h>
#include "Engine/EcosystemEngine.h"
#include "Engine/Scenarios.h"
#include "Hardware/Model12AudioRouter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <set>
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

template <size_t Channels>
struct OutputBlock
{
    explicit OutputBlock(int samples, float initialValue = 0.0f)
    {
        for (size_t channel = 0; channel < Channels; ++channel)
        {
            storage[channel].assign(static_cast<size_t>(samples), initialValue);
            pointers[channel] = storage[channel].data();
        }
    }

    void clear()
    {
        for (auto& channel : storage)
            std::fill(channel.begin(), channel.end(), 0.0f);
    }

    [[nodiscard]] float peak(size_t channel, int samples) const
    {
        float result = 0.0f;
        for (int sample = 0; sample < samples; ++sample)
            result = std::max(result,
                std::abs(storage[channel][static_cast<size_t>(sample)]));
        return result;
    }

    [[nodiscard]] bool silent(size_t channel, int samples) const
    {
        return std::all_of(storage[channel].begin(),
                           storage[channel].begin() + samples,
                           [](float value) { return value == 0.0f; });
    }

    [[nodiscard]] bool finite(int samples) const
    {
        for (const auto& channel : storage)
            for (int sample = 0; sample < samples; ++sample)
                if (! std::isfinite(channel[static_cast<size_t>(sample)]))
                    return false;
        return true;
    }

    std::array<std::vector<float>, Channels> storage;
    std::array<float*, Channels> pointers {};
};
}

int main()
{
    bool passed = true;
    constexpr auto sampleRate = 48000.0;
    constexpr auto blockSize = 512;

    std::set<std::string> scenarioNames;
    for (int index = 0; index < CommentoScenarios::count; ++index)
        scenarioNames.insert(CommentoScenarios::get(index).name);
    passed &= expect(CommentoScenarios::count >= 10,
                     "devono esistere almeno dieci scenari");
    passed &= expect(scenarioNames.size() == CommentoScenarios::count,
                     "ogni scenario deve avere un nome distinto");
    passed &= expect(CommentoScenarios::wrapIndex(-1) == 9
                         && CommentoScenarios::wrapIndex(10) == 0,
                     "la selezione scenario deve essere circolare");

    EcosystemEngine bassEngine;
    bassEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> bassOutput(blockSize);
    bassEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(5, 48, 0.85f));
    process(bassEngine, nullptr, 0, bassOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(bassOutput.peak(EcosystemEngine::bassBus, blockSize) > 0.0001f,
                     "MIDI 5 deve suonare sul bus audio basso");
    passed &= expect(bassOutput.silent(EcosystemEngine::ambientLeftBus, blockSize)
                         && bassOutput.silent(EcosystemEngine::ambientRightBus, blockSize),
                     "il basso non deve contaminare il bus ambiente");
    passed &= expect(bassOutput.silent(EcosystemEngine::saxLeftBus, blockSize)
                         && bassOutput.silent(EcosystemEngine::saxRightBus, blockSize),
                     "il basso non deve contaminare il bus sax");
    bassEngine.toggleRecording(EcosystemEngine::bassLayerIndex);
    bassEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(5, 48));
    process(bassEngine, nullptr, 0, bassOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(! bassEngine.hasMaterial(EcosystemEngine::bassLayerIndex),
                     "il basso MIDI 5 deve restare live e non essere registrato");
    bassEngine.setBassEnabled(false);
    bassOutput.clear();
    process(bassEngine, nullptr, 0, bassOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(! bassEngine.isBassEnabled(),
                     "il basso live deve poter essere disattivato");

    EcosystemEngine loopEngine;
    loopEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> loopOutput(blockSize);
    loopEngine.toggleRecording(1);
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    loopEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(2, 55, 0.8f));
    loopEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(2, 55));
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    loopEngine.toggleRecording(1);
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    const auto loopLength = loopEngine.getLengthSeconds(1);
    passed &= expect(loopEngine.hasMaterial(1),
                     "il canale MIDI 2 deve creare una memoria ambient");
    passed &= expect(loopEngine.getEventCount(1) == 2,
                     "la memoria MIDI 2 deve contenere note on e note off");
    passed &= expect(std::abs(loopLength - 1024.0 / sampleRate) < 0.0001,
                     "la memoria MIDI deve conservare la durata libera");
    loopEngine.setScenarioIndex(6);
    loopOutput.clear();
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(loopEngine.getScenarioIndex() == 6,
                     "il motore deve accettare il cambio scenario");
    passed &= expect(loopEngine.hasMaterial(1)
                         && loopEngine.getEventCount(1) == 2
                         && std::abs(loopEngine.getLengthSeconds(1) - loopLength) < 0.0001,
                     "cambiare scenario non deve cancellare o alterare i loop MIDI");

    EcosystemEngine ambientEngine;
    ambientEngine.prepare(sampleRate, blockSize);
    Model12AudioRouter ambientRouter(ambientEngine);
    OutputBlock<10> physicalAmbient(blockSize, 0.75f);
    ambientEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(3, 64, 0.8f));
    process(ambientRouter, nullptr, 0, physicalAmbient.pointers.data(), 10, blockSize);
    passed &= expect(physicalAmbient.peak(0, blockSize) > 0.0001f
                         || physicalAmbient.peak(1, blockSize) > 0.0001f,
                     "MIDI 2/3/4 deve uscire sui canali audio 1/2 della Model 12");
    passed &= expect(physicalAmbient.silent(4, blockSize),
                     "un layer ambient non deve entrare nel canale audio 5");
    passed &= expect(physicalAmbient.silent(6, blockSize)
                         && physicalAmbient.silent(7, blockSize),
                     "un layer ambient non deve entrare nel ritorno sax 7/8");

    EcosystemEngine routedBassEngine;
    routedBassEngine.prepare(sampleRate, blockSize);
    Model12AudioRouter bassRouter(routedBassEngine);
    OutputBlock<10> physicalBass(blockSize, 0.75f);
    routedBassEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(5, 45, 0.9f));
    process(bassRouter, nullptr, 0, physicalBass.pointers.data(), 10, blockSize);
    passed &= expect(physicalBass.peak(4, blockSize) > 0.0001f,
                     "MIDI 5 deve uscire come audio sul canale 5 della Model 12");
    passed &= expect(physicalBass.silent(0, blockSize)
                         && physicalBass.silent(1, blockSize),
                     "il basso dedicato non deve entrare nell'audio 1/2");
    passed &= expect(physicalBass.silent(6, blockSize)
                         && physicalBass.silent(7, blockSize),
                     "il basso dedicato non deve entrare nell'audio sax 7/8");

    EcosystemEngine noInputEngine;
    noInputEngine.prepare(sampleRate, blockSize);
    Model12AudioRouter noInputRouter(noInputEngine);
    OutputBlock<10> physicalNoInput(blockSize, 0.75f);
    process(noInputRouter, nullptr, 0, physicalNoInput.pointers.data(), 10,
            blockSize);
    passed &= expect(physicalNoInput.silent(6, blockSize)
                         && physicalNoInput.silent(7, blockSize),
                     "input None/null deve produrre silenzio sui bus sax 7/8");
    passed &= expect(physicalNoInput.finite(blockSize),
                     "input None/null deve produrre soltanto campioni finiti");

    EcosystemEngine feedbackSafetyEngine;
    feedbackSafetyEngine.prepare(sampleRate, blockSize);
    std::array<std::vector<float>, 2> feedbackInputStorage;
    std::array<const float*, 2> feedbackInputs {};
    for (size_t channel = 0; channel < feedbackInputStorage.size(); ++channel)
    {
        feedbackInputStorage[channel].assign(blockSize, 0.97f);
        feedbackInputs[channel] = feedbackInputStorage[channel].data();
    }
    OutputBlock<EcosystemEngine::logicalOutputBusCount> feedbackOutput(blockSize);
    for (int block = 0; block < 20; ++block)
    {
        feedbackOutput.clear();
        process(feedbackSafetyEngine, feedbackInputs.data(), 2,
                feedbackOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
    }
    passed &= expect(feedbackSafetyEngine.isSaxSafetyMuted(),
                     "un ingresso sax quasi a fondo scala deve attivare la sicurezza");
    for (int block = 0; block < 100; ++block)
    {
        feedbackOutput.clear();
        process(feedbackSafetyEngine, nullptr, 0,
                feedbackOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
    }
    passed &= expect(! feedbackSafetyEngine.isSaxSafetyMuted(),
                     "la sicurezza sax deve recuperare dopo un secondo di silenzio");

    EcosystemEngine saxEngine;
    saxEngine.prepare(sampleRate, blockSize);
    Model12AudioRouter saxRouter(saxEngine);
    std::array<std::vector<float>, 12> physicalInputStorage;
    std::array<const float*, 12> physicalInputs {};
    for (size_t channel = 0; channel < physicalInputStorage.size(); ++channel)
    {
        const auto value = channel == 0 ? 0.9f : (channel == 6 ? 0.2f : -0.1f);
        physicalInputStorage[channel].assign(4800, value);
        physicalInputs[channel] = physicalInputStorage[channel].data();
    }
    OutputBlock<10> physicalSax(4800, 0.75f);
    saxEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    process(saxRouter, physicalInputs.data(), 12, physicalSax.pointers.data(), 10, 4800);
    saxEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    physicalSax.clear();
    process(saxRouter, nullptr, 0, physicalSax.pointers.data(), 10, blockSize);
    passed &= expect(saxEngine.hasMaterial(EcosystemEngine::midiMemoryCount),
                     "il sax fisico 7/8 deve creare una memoria audio");
    passed &= expect(std::abs(saxEngine.getLengthSeconds(
                                 EcosystemEngine::midiMemoryCount) - 0.1) < 0.0001,
                     "la memoria sax deve conservare la durata registrata");
    passed &= expect(physicalSax.peak(6, blockSize) > 0.001f
                         || physicalSax.peak(7, blockSize) > 0.001f,
                     "il sax deve uscire sui canali audio dedicati 7/8");
    passed &= expect(physicalSax.silent(0, blockSize)
                         && physicalSax.silent(1, blockSize)
                         && physicalSax.silent(4, blockSize),
                     "il sax non deve entrare nei bus ambiente o basso");

    constexpr std::array<size_t, 5> unusedPhysicalOutputs { 2, 3, 5, 8, 9 };
    for (const auto channel : unusedPhysicalOutputs)
    {
        passed &= expect(physicalAmbient.silent(channel, blockSize),
                         "le uscite fisiche non assegnate devono restare mute");
        passed &= expect(physicalBass.silent(channel, blockSize),
                         "il router basso deve lasciare mute le uscite non assegnate");
        passed &= expect(physicalSax.silent(channel, blockSize),
                         "il router sax deve lasciare mute le uscite non assegnate");
    }

    saxEngine.prepare(sampleRate, blockSize);
    passed &= expect(saxEngine.hasMaterial(EcosystemEngine::midiMemoryCount),
                     "un riavvio a 48 kHz deve conservare il loop sax");

    // A low-level sax loop must not grow merely because overdub remains active
    // while no physical input is connected.  The loop length is intentionally
    // greater than 50 ms so it is accepted as usable material by the engine.
    constexpr auto regressionLoopSamples = 4096;
    constexpr auto silentOverdubPasses = 48;
    EcosystemEngine overdubEngine;
    overdubEngine.prepare(sampleRate, blockSize);
    overdubEngine.setScenarioIndex(6);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> overdubOutput(
        regressionLoopSamples);

    // Let all smoothed scenario parameters reach their targets before taking
    // amplitude measurements.  Silence also verifies the DSP state stays
    // finite while its parameters move.
    for (int block = 0; block < 100; ++block)
    {
        overdubOutput.clear();
        process(overdubEngine, nullptr, 0, overdubOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        passed &= expect(overdubOutput.finite(blockSize),
                         "il DSP sax deve restare finito durante il cambio scenario");
    }

    std::array<std::vector<float>, 2> quietSaxInputStorage;
    std::array<const float*, 2> quietSaxInputs {};
    for (size_t channel = 0; channel < quietSaxInputStorage.size(); ++channel)
    {
        quietSaxInputStorage[channel].assign(regressionLoopSamples, 0.02f);
        quietSaxInputs[channel] = quietSaxInputStorage[channel].data();
    }

    overdubEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    overdubOutput.clear();
    process(overdubEngine, quietSaxInputs.data(), 2,
            overdubOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, regressionLoopSamples);
    overdubEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    overdubOutput.clear();
    process(overdubEngine, nullptr, 0, overdubOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, regressionLoopSamples);
    const auto referenceLoopPeak = std::max(
        overdubOutput.peak(EcosystemEngine::saxLeftBus, regressionLoopSamples),
        overdubOutput.peak(EcosystemEngine::saxRightBus, regressionLoopSamples));
    passed &= expect(referenceLoopPeak > 0.0001f,
                     "il loop sax di regressione deve essere udibile");

    overdubEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    float maximumSilentOverdubPeak = 0.0f;
    bool silentOverdubStayedFinite = true;
    for (int pass = 0; pass < silentOverdubPasses; ++pass)
    {
        overdubOutput.clear();
        process(overdubEngine, nullptr, 0, overdubOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, regressionLoopSamples);
        silentOverdubStayedFinite &= overdubOutput.finite(regressionLoopSamples);
        maximumSilentOverdubPeak = std::max(maximumSilentOverdubPeak,
            std::max(overdubOutput.peak(EcosystemEngine::saxLeftBus,
                                        regressionLoopSamples),
                     overdubOutput.peak(EcosystemEngine::saxRightBus,
                                        regressionLoopSamples)));
    }
    overdubEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    overdubOutput.clear();
    process(overdubEngine, nullptr, 0, overdubOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, regressionLoopSamples);
    const auto finalLoopPeak = std::max(
        overdubOutput.peak(EcosystemEngine::saxLeftBus, regressionLoopSamples),
        overdubOutput.peak(EcosystemEngine::saxRightBus, regressionLoopSamples));

    passed &= expect(silentOverdubStayedFinite && overdubOutput.finite(
                         regressionLoopSamples),
                     "molti giri di overdub silenzioso devono restare finiti");
    passed &= expect(finalLoopPeak <= referenceLoopPeak * 4.0f + 0.02f,
                     "l'overdub senza input non deve auto-amplificare il loop sax");
    passed &= expect(maximumSilentOverdubPeak < 0.25f,
                     "un loop sax a basso livello non deve avvicinarsi al clipping");

    EcosystemEngine scenarioEngine;
    scenarioEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> scenarioOutput(blockSize);
    for (int scenario = 0; scenario < CommentoScenarios::count; ++scenario)
    {
        scenarioEngine.setScenarioIndex(scenario);
        scenarioEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(4, 60 + scenario % 7, 0.7f));
        scenarioOutput.clear();
        process(scenarioEngine, nullptr, 0, scenarioOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        passed &= expect(scenarioOutput.finite(blockSize),
                         "ogni scenario deve produrre campioni finiti");
        scenarioEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(4, 60 + scenario % 7));
    }

    passed &= expect(saxRouter.getPhysicalInputChannelCount() == 0
                         && saxRouter.getPhysicalOutputChannelCount() == 10,
                     "il router deve esporre il conteggio del callback fisico");
    passed &= expect(noInputRouter.getPhysicalInputChannelCount() == 0
                         && noInputRouter.getPhysicalOutputChannelCount() == 10,
                     "il routing None deve conservare dieci uscite fisiche");

    if (passed)
        std::cout << "Commento engine tests passed\n";
    return passed ? 0 : 1;
}
