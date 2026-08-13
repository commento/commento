#include <JuceHeader.h>
#include "Engine/AmbientSynth.h"
#include "Engine/EcosystemEngine.h"
#include "Engine/SaxProcessor.h"
#include "Engine/Scenarios.h"
#include "Hardware/Model12AudioRouter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct CommentoFreezeRampProbe
{
    [[nodiscard]] static float current(const AmbientSynth& synth) noexcept
    {
        return synth.freezeMix.getCurrentValue();
    }

    [[nodiscard]] static float current(const SaxProcessor& processor) noexcept
    {
        return processor.freezeMix.getCurrentValue();
    }
};

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
    constexpr auto legacyScenarioCount = 10;

    std::set<std::string> scenarioNames;
    std::set<int> ambientDelayBuckets;
    float longestAmbientDelay = 0.0f;
    float longestSaxDelay = 0.0f;
    int fourHeadSaxLoopScenarioCount = 0;
    int fourHeadSaxLoopScenarioIndex = -1;
    int droneScenarioIndex = -1;
    int noiseScenarioIndex = -1;
    int metalScenarioIndex = -1;
    bool allFactoryBassPatchesAreDry = true;
    for (int index = 0; index < CommentoScenarios::count; ++index)
    {
        scenarioNames.insert(CommentoScenarios::get(index).name);
        const auto& scenario = CommentoScenarios::get(index);
        const auto& bassPatch = scenario.layers[
            static_cast<std::size_t>(EcosystemEngine::bassLayerIndex)];
        allFactoryBassPatchesAreDry &= bassPatch.delayMix == 0.0f
            && bassPatch.delayFeedback == 0.0f
            && bassPatch.reverbWet == 0.0f;
        if (scenario.useFourHeadSaxLoopPlayback)
        {
            ++fourHeadSaxLoopScenarioCount;
            fourHeadSaxLoopScenarioIndex = index;
        }

        int droneLayers = 0;
        int noiseLayers = 0;
        int metalLayers = 0;
        for (int layer = 1; layer < EcosystemEngine::midiMemoryCount; ++layer)
        {
            const auto& patch = scenario.layers[static_cast<std::size_t>(layer)];
            const auto delay = patch.delayMilliseconds;
            longestAmbientDelay = std::max(longestAmbientDelay, delay);
            ambientDelayBuckets.insert(static_cast<int>(std::round(delay / 25.0f)));

            if (index >= legacyScenarioCount)
            {
                if (patch.attackSeconds >= 3.0f
                    && patch.releaseSeconds >= 12.0f
                    && patch.sustain >= 0.85f
                    && patch.lfoRateHz <= 0.08f)
                    ++droneLayers;
                if (patch.noiseMix >= 0.25f
                    && patch.lfoDepth >= 0.20f
                    && patch.keyTrack <= 0.25f)
                    ++noiseLayers;
                if (patch.attackSeconds <= 0.003f
                    && patch.sustain <= 0.03f
                    && patch.harmonicMix >= 0.50f
                    && patch.cutoffHz >= 6000.0f)
                    ++metalLayers;
            }
        }
        if (droneLayers >= 2 && droneScenarioIndex < 0)
            droneScenarioIndex = index;
        if (noiseLayers >= 2 && noiseScenarioIndex < 0)
            noiseScenarioIndex = index;
        if (metalLayers >= 2 && metalScenarioIndex < 0)
            metalScenarioIndex = index;
        longestSaxDelay = std::max(longestSaxDelay,
                                   scenario.sax.delayMilliseconds);
    }
    passed &= expect(CommentoScenarios::count >= legacyScenarioCount + 4,
                     "devono esistere almeno quattro nuovi scenari live");
    passed &= expect(scenarioNames.size() == CommentoScenarios::count,
                     "ogni scenario deve avere un nome distinto");
    const auto& cosmosScenario = CommentoScenarios::get(
        CommentoScenarios::count - 1);
    passed &= expect(fourHeadSaxLoopScenarioCount == 1
                         && fourHeadSaxLoopScenarioIndex
                                == CommentoScenarios::count - 1
                         && std::string(cosmosScenario.name) == "COSMOS"
                         && cosmosScenario.useFourHeadSaxLoopPlayback,
                     "solo COSMOS deve rileggere RESPIRO con quattro testine");
    const auto& cosmosBass = cosmosScenario.layers[
        static_cast<std::size_t>(EcosystemEngine::bassLayerIndex)];
    passed &= expect(cosmosBass.model == OscillatorModel::dualSquare
                         && cosmosBass.detuneCents >= 3.0f
                         && cosmosBass.detuneCents <= 12.0f
                         && cosmosBass.harmonicMix >= 0.35f,
                     "COSMOS deve dichiarare due onde quadre leggermente detunate");
    passed &= expect(droneScenarioIndex >= legacyScenarioCount,
                     "le nuove scene devono includere un vero assetto drone");
    passed &= expect(noiseScenarioIndex >= legacyScenarioCount,
                     "le nuove scene devono includere rumore modulato evidente");
    passed &= expect(metalScenarioIndex >= legacyScenarioCount,
                     "le nuove scene devono includere transienti metallici");
    passed &= expect(droneScenarioIndex != noiseScenarioIndex
                         && droneScenarioIndex != metalScenarioIndex
                         && noiseScenarioIndex != metalScenarioIndex,
                     "drone, noise e metal devono essere identita' distinte");
    passed &= expect(ambientDelayBuckets.size() >= 20,
                     "i delay ambient devono avere tempi realmente differenti");
    passed &= expect(longestAmbientDelay < 2300.0f
                         && longestSaxDelay < 2500.0f,
                     "i preset non devono imporre code eccessivamente lunghe");
    passed &= expect(allFactoryBassPatchesAreDry,
                     "il fast-path del basso richiede patch fabbrica asciutte");
    {
        EcosystemEngine defaultEngine;
        passed &= expect(! defaultEngine.isLoopEvolutionEnabled(),
                         "DERIVA deve partire spenta");
        passed &= expect(defaultEngine.isSaxStereoInput(),
                         "RESPIRO deve partire in stereo sugli ingressi 7/8");
        auto gesturesStartOff = defaultEngine.getSaxListenAmount() == 0.0f
            && ! defaultEngine.isThinningEnabled()
            && defaultEngine.getThinnedMemoryIndex() == -1;
        for (int memory = 0; memory < EcosystemEngine::memoryCount; ++memory)
            gesturesStartOff &= ! defaultEngine.isFreezeEnabled(memory)
                && ! defaultEngine.isEchoThrowEnabled(memory)
                && ! defaultEngine.isFreeTailEnabled(memory);
        passed &= expect(gesturesStartOff,
                         "GELO, ECO THROW, CODA, ASCOLTO e DIRADA devono partire spenti");

        // MIDI 5 is the dedicated live bass and deliberately owns neither a
        // delay nor a reverb tail. Both the direct UI API and the global MIDI
        // gestures must therefore leave it outside GELO/ECO THROW.
        defaultEngine.setFreezeEnabled(EcosystemEngine::bassLayerIndex, true);
        defaultEngine.setEchoThrowEnabled(EcosystemEngine::bassLayerIndex,
                                          true);
        defaultEngine.setFreeTailEnabled(EcosystemEngine::bassLayerIndex,
                                         true);
        defaultEngine.setGestureTarget(EcosystemEngine::bassLayerIndex);
        defaultEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(5, 80, 127));
        defaultEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(5, 81, 127));
        defaultEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(5, 83, 127));
        passed &= expect(! defaultEngine.isFreezeEnabled(
                              EcosystemEngine::bassLayerIndex)
                             && ! defaultEngine.isEchoThrowEnabled(
                                 EcosystemEngine::bassLayerIndex)
                             && ! defaultEngine.isFreeTailEnabled(
                                 EcosystemEngine::bassLayerIndex),
                         "il basso MIDI 5 deve restare escluso dai gesti di coda");
    }
    passed &= expect(CommentoScenarios::wrapIndex(-1)
                             == CommentoScenarios::count - 1
                         && CommentoScenarios::wrapIndex(
                                CommentoScenarios::count) == 0,
                     "la selezione scenario deve essere circolare");

    // The five dedicated MIDI controls are global performance controls, not
    // material for the loop recorder. Their state changes synchronously on
    // the MIDI thread, so threshold and ownership can be tested without any
    // scheduler-sensitive sleeps.
    {
        constexpr auto gestureSampleRate = 8000.0;
        constexpr auto gestureBlockSize = 400;
        EcosystemEngine controlEngine;

        controlEngine.setGestureTarget(1);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 80, 63));
        const auto freezeBelowThreshold = ! controlEngine.isFreezeEnabled(1);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 80, 64));
        controlEngine.setGestureTarget(2);
        const auto freezeCapturedTarget = controlEngine.isFreezeEnabled(1)
            && ! controlEngine.isFreezeEnabled(2);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 80, 0));
        const auto freezeReleasedCapturedTarget
            = ! controlEngine.isFreezeEnabled(1)
            && ! controlEngine.isFreezeEnabled(2);

        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(3, 80, 127));
        controlEngine.setFreezeEnabled(2, true);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(3, 80, 0));
        const auto touchscreenStillOwnsFreeze
            = controlEngine.isFreezeEnabled(2);
        controlEngine.setFreezeEnabled(2, false);

        controlEngine.setGestureTarget(3);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(4, 81, 63));
        const auto throwBelowThreshold
            = ! controlEngine.isEchoThrowEnabled(3);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(4, 81, 64));
        controlEngine.setGestureTarget(EcosystemEngine::midiMemoryCount);
        const auto throwCapturedTarget
            = controlEngine.isEchoThrowEnabled(3)
            && ! controlEngine.isEchoThrowEnabled(
                EcosystemEngine::midiMemoryCount);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(4, 81, 0));
        const auto throwReleasedCapturedTarget
            = ! controlEngine.isEchoThrowEnabled(3)
            && ! controlEngine.isEchoThrowEnabled(
                EcosystemEngine::midiMemoryCount);

        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 82, 64));
        const auto halfListen = controlEngine.getSaxListenAmount();
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 82, 127));
        const auto fullListen = controlEngine.getSaxListenAmount();
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 82, 0));

        controlEngine.setGestureTarget(1);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 83, 63));
        const auto freeTailBelowThreshold
            = ! controlEngine.isFreeTailEnabled(1);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 83, 64));
        controlEngine.setGestureTarget(2);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 83, 127));
        const auto freeTailCapturedTarget
            = controlEngine.isFreeTailEnabled(1)
            && ! controlEngine.isFreeTailEnabled(2);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 83, 0));
        const auto freeTailReleasedCapturedTarget
            = ! controlEngine.isFreeTailEnabled(1)
            && ! controlEngine.isFreeTailEnabled(2);

        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 84, 63));
        const auto thinningBelowThreshold
            = ! controlEngine.isThinningEnabled();
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 84, 64));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 84, 127));
        const auto thinningDuplicateHighStayedOn
            = controlEngine.isThinningEnabled();
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 84, 63));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 84, 0));
        const auto thinningDuplicateLowStayedOff
            = ! controlEngine.isThinningEnabled();

        passed &= expect(freezeBelowThreshold && freezeCapturedTarget
                             && freezeReleasedCapturedTarget
                             && touchscreenStillOwnsFreeze,
                         "CC80 deve usare soglia 64, catturare il target e rispettare l'owner touch");
        passed &= expect(throwBelowThreshold && throwCapturedTarget
                             && throwReleasedCapturedTarget,
                         "CC81 deve usare soglia 64 e rilasciare il target catturato");
        passed &= expect(std::abs(halfListen - 64.0f / 127.0f) < 0.000001f
                             && fullListen == 1.0f
                             && controlEngine.getSaxListenAmount() == 0.0f,
                         "CC82 deve controllare ASCOLTO sull'intero intervallo 0..1");
        passed &= expect(freeTailBelowThreshold && freeTailCapturedTarget
                             && freeTailReleasedCapturedTarget,
                         "CC83 deve essere momentaneo e rilasciare il target catturato");
        passed &= expect(thinningBelowThreshold
                             && thinningDuplicateHighStayedOn
                             && thinningDuplicateLowStayedOff,
                         "CC84 deve impostare DIRADA in modo assoluto e idempotente");

        controlEngine.setGestureTarget(2);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(3, 80, 127));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(3, 81, 127));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(3, 83, 127));
        controlEngine.releaseMomentaryGestures();
        passed &= expect(! controlEngine.isFreezeEnabled(2)
                             && ! controlEngine.isEchoThrowEnabled(2)
                             && ! controlEngine.isFreeTailEnabled(2),
                         "il panic deve liberare GELO, ECO THROW e CODA su ogni card");

        controlEngine.setDelayLevel(1, 0.0f);
        controlEngine.prepare(gestureSampleRate, gestureBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> controlOutput(
            gestureBlockSize);
        controlEngine.setGestureTarget(1);
        controlEngine.toggleRecording(1);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 80, 127));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 81, 127));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 82, 96));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 83, 127));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 84, 127));
        process(controlEngine, nullptr, 0, controlOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, gestureBlockSize);
        const auto controllersDidNotStartCapture
            = controlEngine.isWaitingForFirstNote(1)
            && controlEngine.getEventCount(1) == 0;

        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, 57, 0.45f));
        for (int block = 0; block < 4; ++block)
        {
            controlOutput.clear();
            process(controlEngine, nullptr, 0, controlOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    gestureBlockSize);
        }
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 80, 0));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 81, 0));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 82, 0));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 83, 0));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 84, 0));
        controlEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(2, 57));
        controlOutput.clear();
        process(controlEngine, nullptr, 0, controlOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, gestureBlockSize);
        controlEngine.toggleRecording(1);
        controlOutput.clear();
        process(controlEngine, nullptr, 0, controlOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, gestureBlockSize);
        passed &= expect(controllersDidNotStartCapture
                             && controlEngine.hasMaterial(1)
                             && controlEngine.getEventCount(1) == 2
                             && ! controlEngine.isThinningEnabled(),
                         "CC80-84 devono essere consumati e mai registrati nel loop MIDI");

        // Exercise both tails briefly with the recorded phrase. This is a
        // signal-safety invariant, not a wall-clock performance benchmark.
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 80, 127));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 81, 127));
        auto gestureAudioFinite = true;
        auto gestureAudioPeak = 0.0f;
        for (int block = 0; block < 32; ++block)
        {
            controlOutput.clear();
            process(controlEngine, nullptr, 0, controlOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    gestureBlockSize);
            gestureAudioFinite &= controlOutput.finite(gestureBlockSize);
            gestureAudioPeak = std::max(gestureAudioPeak,
                std::max(controlOutput.peak(EcosystemEngine::ambientLeftBus,
                                            gestureBlockSize),
                         controlOutput.peak(EcosystemEngine::ambientRightBus,
                                            gestureBlockSize)));
        }
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 80, 0));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 81, 0));
        for (int block = 0; block < 8; ++block)
        {
            controlOutput.clear();
            process(controlEngine, nullptr, 0, controlOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    gestureBlockSize);
            gestureAudioFinite &= controlOutput.finite(gestureBlockSize);
            gestureAudioPeak = std::max(gestureAudioPeak,
                std::max(controlOutput.peak(EcosystemEngine::ambientLeftBus,
                                            gestureBlockSize),
                         controlOutput.peak(EcosystemEngine::ambientRightBus,
                                            gestureBlockSize)));
        }
        passed &= expect(gestureAudioFinite && gestureAudioPeak < 0.90f
                             && controlEngine.getDelayLevel(1) == 0.0f
                             && ! controlEngine.isFreezeEnabled(1)
                             && ! controlEngine.isEchoThrowEnabled(1),
                         "GELO/ECO devono restare finiti, rilasciarsi e non cambiare DELAY");
    }

    // GELO uses an asymmetric momentary envelope on the delay feedback only:
    // 80 ms in and 350 ms out. Retargeting that envelope must start from its
    // exact current value, otherwise a quick release/re-press creates a click.
    // A low sample rate makes both durations exact multiples of the block and
    // keeps this signal-level regression test inexpensive.
    {
        constexpr auto freezeSampleRate = 8000.0;
        constexpr auto freezeBlockSize = 80; // 10 ms
        constexpr auto rampTolerance = 0.002f;

        const auto bufferIsFinite = [](const juce::AudioBuffer<float>& buffer)
        {
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                    if (! std::isfinite(buffer.getSample(channel, sample)))
                        return false;
            return true;
        };

        SynthPatch freezeSynthPatch;
        freezeSynthPatch.model = OscillatorModel::warm;
        freezeSynthPatch.attackSeconds = 0.001f;
        freezeSynthPatch.decaySeconds = 0.2f;
        freezeSynthPatch.sustain = 0.82f;
        freezeSynthPatch.releaseSeconds = 1.0f;
        freezeSynthPatch.cutoffHz = 3000.0f;
        freezeSynthPatch.harmonicMix = 0.12f;
        freezeSynthPatch.noiseMix = 0.0f;
        freezeSynthPatch.lfoDepth = 0.0f;
        freezeSynthPatch.level = 0.11f;
        freezeSynthPatch.delayMilliseconds = 40.0f;
        freezeSynthPatch.delaySpread = 1.19f;
        freezeSynthPatch.delayFeedback = 0.62f;
        freezeSynthPatch.delayMix = 0.52f;
        freezeSynthPatch.reverbSize = 0.72f;
        freezeSynthPatch.reverbDamping = 0.48f;
        freezeSynthPatch.reverbWet = 0.38f;

        AmbientSynth freezeSynth(1);
        freezeSynth.setPatch(freezeSynthPatch);
        freezeSynth.prepare(freezeSampleRate, freezeBlockSize);
        juce::AudioBuffer<float> freezeSynthOutput(2, freezeBlockSize);
        juce::MidiBuffer freezeSynthMidi;
        float previousSynthSample = 0.0f;
        float maximumSynthBoundaryStep = 0.0f;
        bool havePreviousSynthSample = false;
        bool freezeSynthFinite = true;
        const auto renderFreezeSynth = [&](bool measureBoundary)
        {
            freezeSynthOutput.clear();
            freezeSynth.render(freezeSynthOutput, freezeSynthMidi, 0,
                               freezeBlockSize);
            freezeSynthMidi.clear();
            freezeSynthFinite &= bufferIsFinite(freezeSynthOutput);
            if (measureBoundary && havePreviousSynthSample)
                maximumSynthBoundaryStep = std::max(
                    maximumSynthBoundaryStep,
                    std::abs(freezeSynthOutput.getSample(0, 0)
                             - previousSynthSample));
            previousSynthSample = freezeSynthOutput.getSample(
                0, freezeBlockSize - 1);
            havePreviousSynthSample = true;
        };

        freezeSynthMidi.addEvent(
            juce::MidiMessage::noteOn(2, 48, 0.72f), 0);
        renderFreezeSynth(false);
        for (int block = 0; block < 20; ++block)
            renderFreezeSynth(false);

        const auto synthBeforeAttack = CommentoFreezeRampProbe::current(
            freezeSynth);
        freezeSynth.setFreezeEnabled(true);
        const auto synthAtAttackBoundary = CommentoFreezeRampProbe::current(
            freezeSynth);
        for (int block = 0; block < 4; ++block)
            renderFreezeSynth(block == 0);
        const auto synthHalfAttack = CommentoFreezeRampProbe::current(
            freezeSynth);

        freezeSynth.setFreezeEnabled(false);
        const auto synthAtReleaseBoundary = CommentoFreezeRampProbe::current(
            freezeSynth);
        for (int block = 0; block < 7; ++block)
            renderFreezeSynth(block == 0);
        const auto synthDuringRelease = CommentoFreezeRampProbe::current(
            freezeSynth);

        freezeSynth.setFreezeEnabled(true);
        const auto synthAtRetriggerBoundary = CommentoFreezeRampProbe::current(
            freezeSynth);
        for (int block = 0; block < 4; ++block)
            renderFreezeSynth(block == 0);
        const auto synthHalfRetrigger = CommentoFreezeRampProbe::current(
            freezeSynth);
        for (int block = 0; block < 4; ++block)
            renderFreezeSynth(false);
        const auto synthFullAttack = CommentoFreezeRampProbe::current(
            freezeSynth);

        freezeSynth.setFreezeEnabled(false);
        for (int block = 0; block < 35; ++block)
            renderFreezeSynth(block == 0);
        const auto synthFullRelease = CommentoFreezeRampProbe::current(
            freezeSynth);

        passed &= expect(
            std::abs(synthBeforeAttack) < rampTolerance
                && std::abs(synthAtAttackBoundary - synthBeforeAttack)
                    < rampTolerance
                && std::abs(synthHalfAttack - 0.5f) < rampTolerance
                && std::abs(synthAtReleaseBoundary - synthHalfAttack)
                    < rampTolerance
                && std::abs(synthDuringRelease - 0.4f) < rampTolerance
                && std::abs(synthAtRetriggerBoundary - synthDuringRelease)
                    < rampTolerance
                && std::abs(synthHalfRetrigger - 0.7f) < rampTolerance
                && std::abs(synthFullAttack - 1.0f) < rampTolerance
                && std::abs(synthFullRelease) < rampTolerance,
            "GELO synth deve usare 80/350 ms e retrigger dal valore corrente");
        passed &= expect(freezeSynthFinite
                             && maximumSynthBoundaryStep < 0.04f,
                         "GELO synth deve restare finito e continuo ai boundary");

        SaxPatch freezeSaxPatch;
        freezeSaxPatch.toneHz = 3000.0f;
        freezeSaxPatch.drive = 1.0f;
        freezeSaxPatch.delayMilliseconds = 40.0f;
        freezeSaxPatch.delaySpread = 1.19f;
        freezeSaxPatch.feedback = 0.58f;
        freezeSaxPatch.crossFeedback = 0.42f;
        freezeSaxPatch.delayMix = 0.52f;
        freezeSaxPatch.modulationRateHz = 0.0f;
        freezeSaxPatch.modulationDepthMilliseconds = 0.0f;
        freezeSaxPatch.reverbSize = 0.72f;
        freezeSaxPatch.reverbDamping = 0.48f;
        freezeSaxPatch.reverbWet = 0.38f;
        freezeSaxPatch.tremoloDepth = 0.0f;
        freezeSaxPatch.outputGain = 0.58f;

        SaxProcessor freezeSax;
        freezeSax.setPatch(freezeSaxPatch);
        freezeSax.prepare(freezeSampleRate, freezeBlockSize);
        juce::AudioBuffer<float> freezeSaxBuffer(2, freezeBlockSize);
        int64_t freezeSaxSourceSample = 0;
        float previousSaxSample = 0.0f;
        float maximumSaxBoundaryStep = 0.0f;
        bool havePreviousSaxSample = false;
        bool freezeSaxFinite = true;
        const auto processFreezeSax = [&](bool measureBoundary)
        {
            for (int sample = 0; sample < freezeBlockSize; ++sample)
            {
                const auto phase = juce::MathConstants<double>::twoPi
                    * 137.0 * static_cast<double>(freezeSaxSourceSample++)
                    / freezeSampleRate;
                const auto value = 0.13f
                    * static_cast<float>(std::sin(phase));
                freezeSaxBuffer.setSample(0, sample, value);
                freezeSaxBuffer.setSample(1, sample, value * 0.91f);
            }
            freezeSax.process(freezeSaxBuffer, freezeBlockSize);
            freezeSaxFinite &= bufferIsFinite(freezeSaxBuffer);
            if (measureBoundary && havePreviousSaxSample)
                maximumSaxBoundaryStep = std::max(
                    maximumSaxBoundaryStep,
                    std::abs(freezeSaxBuffer.getSample(0, 0)
                             - previousSaxSample));
            previousSaxSample = freezeSaxBuffer.getSample(
                0, freezeBlockSize - 1);
            havePreviousSaxSample = true;
        };

        for (int block = 0; block < 20; ++block)
            processFreezeSax(false);

        const auto saxBeforeAttack = CommentoFreezeRampProbe::current(
            freezeSax);
        freezeSax.setFreezeEnabled(true);
        const auto saxAtAttackBoundary = CommentoFreezeRampProbe::current(
            freezeSax);
        for (int block = 0; block < 4; ++block)
            processFreezeSax(block == 0);
        const auto saxHalfAttack = CommentoFreezeRampProbe::current(freezeSax);

        freezeSax.setFreezeEnabled(false);
        const auto saxAtReleaseBoundary = CommentoFreezeRampProbe::current(
            freezeSax);
        for (int block = 0; block < 7; ++block)
            processFreezeSax(block == 0);
        const auto saxDuringRelease = CommentoFreezeRampProbe::current(
            freezeSax);

        freezeSax.setFreezeEnabled(true);
        const auto saxAtRetriggerBoundary = CommentoFreezeRampProbe::current(
            freezeSax);
        for (int block = 0; block < 4; ++block)
            processFreezeSax(block == 0);
        const auto saxHalfRetrigger = CommentoFreezeRampProbe::current(
            freezeSax);
        for (int block = 0; block < 4; ++block)
            processFreezeSax(false);
        const auto saxFullAttack = CommentoFreezeRampProbe::current(freezeSax);

        freezeSax.setFreezeEnabled(false);
        for (int block = 0; block < 35; ++block)
            processFreezeSax(block == 0);
        const auto saxFullRelease = CommentoFreezeRampProbe::current(freezeSax);

        passed &= expect(
            std::abs(saxBeforeAttack) < rampTolerance
                && std::abs(saxAtAttackBoundary - saxBeforeAttack)
                    < rampTolerance
                && std::abs(saxHalfAttack - 0.5f) < rampTolerance
                && std::abs(saxAtReleaseBoundary - saxHalfAttack)
                    < rampTolerance
                && std::abs(saxDuringRelease - 0.4f) < rampTolerance
                && std::abs(saxAtRetriggerBoundary - saxDuringRelease)
                    < rampTolerance
                && std::abs(saxHalfRetrigger - 0.7f) < rampTolerance
                && std::abs(saxFullAttack - 1.0f) < rampTolerance
                && std::abs(saxFullRelease) < rampTolerance,
            "GELO sax deve usare 80/350 ms e retrigger dal valore corrente");
        passed &= expect(freezeSaxFinite
                             && maximumSaxBoundaryStep < 0.04f,
                         "GELO sax deve restare finito e continuo ai boundary");
    }

    // CODA LIBERA must be a transparent, target-scoped gate on new
    // excitation: it may not clear the existing delay/reverb state.  Keep the
    // probe at 8 kHz so several musical delay times fit in a small test while
    // exercising the same sample-by-sample ramps used at 48 kHz.
    {
        constexpr auto freeTailSampleRate = 8000.0;
        constexpr auto freeTailBlockSize = 400;
        constexpr auto freeTailMemory = 1;
        constexpr auto freeTailChannel = 2;
        constexpr auto freeTailNote = 55;

        auto freeTailEngineStorage = std::make_unique<EcosystemEngine>();
        auto freeTailReferenceStorage = std::make_unique<EcosystemEngine>();
        auto& freeTailEngine = *freeTailEngineStorage;
        auto& freeTailReference = *freeTailReferenceStorage;
        std::array<EcosystemEngine*, 2> freeTailEngines {
            &freeTailEngine, &freeTailReference
        };
        for (auto* engine : freeTailEngines)
        {
            engine->setScenarioIndex(1); // GOCCE: distinct direct hits/taps.
            engine->setDelayLevel(freeTailMemory, 1.0f);
            engine->prepare(freeTailSampleRate, freeTailBlockSize);
        }

        OutputBlock<EcosystemEngine::logicalOutputBusCount> freeTailOutput(
            freeTailBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> freeTailRefOutput(
            freeTailBlockSize);
        std::array<OutputBlock<EcosystemEngine::logicalOutputBusCount>*, 2>
            freeTailOutputs { &freeTailOutput, &freeTailRefOutput };
        const auto renderFreeTail = [=](EcosystemEngine& engine,
                                        auto& output)
        {
            output.clear();
            process(engine, nullptr, 0, output.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    freeTailBlockSize);
        };
        const auto recordFreeTailLoop = [&](EcosystemEngine& engine,
                                             auto& output)
        {
            engine.toggleRecording(freeTailMemory);
            engine.enqueueMidiMessage(juce::MidiMessage::noteOn(
                freeTailChannel, freeTailNote, 0.82f));
            for (int block = 0; block < 6; ++block)
                renderFreeTail(engine, output);
            engine.enqueueMidiMessage(juce::MidiMessage::noteOff(
                freeTailChannel, freeTailNote));
            renderFreeTail(engine, output);
            engine.toggleRecording(freeTailMemory);
            renderFreeTail(engine, output);
        };
        for (std::size_t index = 0; index < freeTailEngines.size(); ++index)
            recordFreeTailLoop(*freeTailEngines[index],
                               *freeTailOutputs[index]);

        const auto ambientEnergy = [=](const auto& output)
        {
            auto energy = 0.0;
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
                for (int sample = 0; sample < freeTailBlockSize; ++sample)
                {
                    const auto value = static_cast<double>(output.storage[
                        static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)]);
                    energy += value * value;
                }
            return energy / static_cast<double>(2 * freeTailBlockSize);
        };
        const auto maximumAmbientDifference = [=](const auto& first,
                                                   const auto& second)
        {
            auto difference = 0.0f;
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
                for (int sample = 0; sample < freeTailBlockSize; ++sample)
                    difference = std::max(difference, std::abs(
                        first.storage[static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)]
                        - second.storage[static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)]));
            return difference;
        };

        auto freeTailDefaultParity = true;
        for (int block = 0; block < 42; ++block)
        {
            renderFreeTail(freeTailEngine, freeTailOutput);
            renderFreeTail(freeTailReference, freeTailRefOutput);
            freeTailDefaultParity &= maximumAmbientDifference(
                freeTailOutput, freeTailRefOutput) < 0.000001f;
        }
        passed &= expect(freeTailDefaultParity
                             && ! freeTailEngine.isFreeTailEnabled(
                                 freeTailMemory),
                         "CODA spenta deve essere trasparente bit per bit");

        const auto originalFreeTailEvents = freeTailEngine.getEventCount(
            freeTailMemory);
        const auto originalFreeTailLength = freeTailEngine.getLengthSeconds(
            freeTailMemory);
        std::array<float, 2> previousFreeTailSamples {
            freeTailOutput.storage[EcosystemEngine::ambientLeftBus].back(),
            freeTailOutput.storage[EcosystemEngine::ambientRightBus].back()
        };
        freeTailEngine.setFreeTailEnabled(freeTailMemory, true);
        auto freeTailStayedFinite = true;
        auto freeTailPeak = 0.0f;
        auto freeTailMaximumStep = 0.0f;
        auto earlyTailEnergy = 0.0;
        auto lateTailEnergy = 0.0;
        auto lateReferenceEnergy = 0.0;
        for (int block = 0; block < 80; ++block)
        {
            renderFreeTail(freeTailEngine, freeTailOutput);
            renderFreeTail(freeTailReference, freeTailRefOutput);
            freeTailStayedFinite &= freeTailOutput.finite(freeTailBlockSize);
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
            {
                const auto channelIndex = static_cast<std::size_t>(channel);
                auto previous = previousFreeTailSamples[channelIndex];
                for (const auto sample : freeTailOutput.storage[channelIndex])
                {
                    freeTailMaximumStep = std::max(
                        freeTailMaximumStep, std::abs(sample - previous));
                    previous = sample;
                    freeTailPeak = std::max(freeTailPeak, std::abs(sample));
                }
                previousFreeTailSamples[channelIndex] = previous;
            }
            if (block >= 6 && block < 22)
                earlyTailEnergy += ambientEnergy(freeTailOutput);
            if (block >= 60)
            {
                lateTailEnergy += ambientEnergy(freeTailOutput);
                lateReferenceEnergy += ambientEnergy(freeTailRefOutput);
            }
        }
        earlyTailEnergy /= 16.0;
        lateTailEnergy /= 20.0;
        lateReferenceEnergy /= 20.0;

        passed &= expect(earlyTailEnergy > 0.00000001
                             && lateTailEnergy < earlyTailEnergy * 0.80
                             && lateTailEnergy
                                    < lateReferenceEnergy * 0.55,
                         "CODA deve spegnere l'eccitazione lasciando una coda udibile che decade");
        passed &= expect(freeTailStayedFinite && freeTailPeak < 0.90f
                             && freeTailMaximumStep < 0.20f
                             && freeTailEngine.getEventCount(freeTailMemory)
                                    == originalFreeTailEvents
                             && std::abs(freeTailEngine.getLengthSeconds(
                                    freeTailMemory) - originalFreeTailLength)
                                    < 0.000001,
                         "CODA deve restare finita, contenuta, anti-click e non riscrivere il loop");

        freeTailEngine.setFreeTailEnabled(freeTailMemory, false);
        auto restoredEnergy = 0.0;
        auto restoredReferenceEnergy = 0.0;
        for (int block = 0; block < 24; ++block)
        {
            renderFreeTail(freeTailEngine, freeTailOutput);
            renderFreeTail(freeTailReference, freeTailRefOutput);
            freeTailStayedFinite &= freeTailOutput.finite(freeTailBlockSize);
            if (block >= 12)
            {
                restoredEnergy += ambientEnergy(freeTailOutput);
                restoredReferenceEnergy += ambientEnergy(freeTailRefOutput);
            }
        }
        passed &= expect(! freeTailEngine.isFreeTailEnabled(freeTailMemory)
                             && restoredEnergy
                                    > restoredReferenceEnergy * 0.45,
                         "rilasciare CODA deve far rientrare gradualmente il materiale dry");

        // Even while the ambient histories differ, the dedicated mono bass
        // remains an exact dry reference and cannot acquire CODA.
        freeTailEngine.setFreeTailEnabled(EcosystemEngine::bassLayerIndex,
                                          true);
        freeTailEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(5, 43, 0.75f));
        freeTailReference.enqueueMidiMessage(
            juce::MidiMessage::noteOn(5, 43, 0.75f));
        auto bassStayedIdentical = true;
        for (int block = 0; block < 6; ++block)
        {
            renderFreeTail(freeTailEngine, freeTailOutput);
            renderFreeTail(freeTailReference, freeTailRefOutput);
            for (int sample = 0; sample < freeTailBlockSize; ++sample)
                bassStayedIdentical &= std::abs(
                    freeTailOutput.storage[EcosystemEngine::bassBus][
                        static_cast<std::size_t>(sample)]
                    - freeTailRefOutput.storage[EcosystemEngine::bassBus][
                        static_cast<std::size_t>(sample)]) < 0.000001f;
        }
        passed &= expect(! freeTailEngine.isFreeTailEnabled(
                              EcosystemEngine::bassLayerIndex)
                             && bassStayedIdentical,
                         "CODA non deve toccare il fast-path del basso MIDI 5");
    }

    // RESPIRO owns CODA only in the scene-effects path. DIRECT and the clean
    // looper are diagnostic/capture paths and must remain exact even while the
    // gesture is held; the effected path instead stops accepting new sax
    // excitation and lets its already-charged delay/reverb decay.
    {
        constexpr auto saxTailSampleRate = 8000.0;
        constexpr auto saxTailBlockSize = 400;
        constexpr auto saxMemory = EcosystemEngine::midiMemoryCount;
        constexpr auto saxFrequency = 173.0;
        constexpr auto saxInputLevel = 0.12f;

        std::vector<float> saxTailInput(
            static_cast<std::size_t>(saxTailBlockSize));
        const float* saxTailInputPointer = saxTailInput.data();
        int64_t saxTailSamplePosition = 0;
        const auto fillSaxTailInput = [&]
        {
            for (int sample = 0; sample < saxTailBlockSize; ++sample)
                saxTailInput[static_cast<std::size_t>(sample)]
                    = saxInputLevel * static_cast<float>(std::sin(
                        juce::MathConstants<double>::twoPi * saxFrequency
                        * static_cast<double>(saxTailSamplePosition + sample)
                        / saxTailSampleRate));
            saxTailSamplePosition += saxTailBlockSize;
        };

        auto saxBypassEngineStorage = std::make_unique<EcosystemEngine>();
        auto& saxBypassEngine = *saxBypassEngineStorage;
        saxBypassEngine.setSaxStereoInput(false);
        saxBypassEngine.prepare(saxTailSampleRate, saxTailBlockSize);
        saxBypassEngine.setFreeTailEnabled(saxMemory, true);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> saxBypassOutput(
            saxTailBlockSize);
        const auto renderSaxBypass = [&](EcosystemEngine::SaxPathMode mode)
        {
            saxBypassEngine.setSaxPathMode(mode);
            fillSaxTailInput();
            saxBypassOutput.clear();
            process(saxBypassEngine, &saxTailInputPointer, 1,
                    saxBypassOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    saxTailBlockSize);
            auto maximumDryError = 0.0f;
            for (int sample = 0; sample < saxTailBlockSize; ++sample)
                maximumDryError = std::max(maximumDryError, std::abs(
                    saxBypassOutput.storage[EcosystemEngine::saxLeftBus][
                        static_cast<std::size_t>(sample)]
                    - saxTailInput[static_cast<std::size_t>(sample)] * 0.58f));
            return maximumDryError;
        };
        const auto directTailBypassError = renderSaxBypass(
            EcosystemEngine::SaxPathMode::direct);
        const auto cleanTailBypassError = renderSaxBypass(
            EcosystemEngine::SaxPathMode::cleanLooper);
        passed &= expect(saxBypassEngine.isFreeTailEnabled(saxMemory)
                             && directTailBypassError < 0.000001f
                             && cleanTailBypassError < 0.000001f,
                         "CODA su RESPIRO non deve colorare DIRECT o il looper pulito");

        auto saxTailEngineStorage = std::make_unique<EcosystemEngine>();
        auto& saxTailEngine = *saxTailEngineStorage;
        saxTailEngine.setScenarioIndex(1); // PING PONG LIQUIDO.
        saxTailEngine.setSaxPathMode(
            EcosystemEngine::SaxPathMode::sceneEffects);
        saxTailEngine.setSaxStereoInput(false);
        saxTailEngine.setDelayLevel(saxMemory, 1.0f);
        saxTailEngine.prepare(saxTailSampleRate, saxTailBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> saxTailOutput(
            saxTailBlockSize);
        const auto renderSaxTail = [&]
        {
            fillSaxTailInput();
            saxTailOutput.clear();
            process(saxTailEngine, &saxTailInputPointer, 1,
                    saxTailOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    saxTailBlockSize);
        };
        const auto saxOutputEnergy = [&]
        {
            auto energy = 0.0;
            for (int channel = EcosystemEngine::saxLeftBus;
                 channel <= EcosystemEngine::saxRightBus; ++channel)
                for (int sample = 0; sample < saxTailBlockSize; ++sample)
                {
                    const auto value = static_cast<double>(
                        saxTailOutput.storage[static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)]);
                    energy += value * value;
                }
            return energy / static_cast<double>(2 * saxTailBlockSize);
        };

        auto chargedSaxEnergy = 0.0;
        for (int block = 0; block < 36; ++block)
        {
            renderSaxTail();
            if (block >= 28)
                chargedSaxEnergy += saxOutputEnergy();
        }
        chargedSaxEnergy /= 8.0;

        saxTailEngine.setFreeTailEnabled(saxMemory, true);
        auto saxTailStayedFinite = true;
        auto saxTailPeak = 0.0f;
        auto saxTailEarlyEnergy = 0.0;
        auto saxTailLateEnergy = 0.0;
        for (int block = 0; block < 64; ++block)
        {
            renderSaxTail();
            saxTailStayedFinite &= saxTailOutput.finite(saxTailBlockSize);
            saxTailPeak = std::max(saxTailPeak,
                std::max(saxTailOutput.peak(EcosystemEngine::saxLeftBus,
                                            saxTailBlockSize),
                         saxTailOutput.peak(EcosystemEngine::saxRightBus,
                                            saxTailBlockSize)));
            if (block >= 5 && block < 17)
                saxTailEarlyEnergy += saxOutputEnergy();
            if (block >= 52)
                saxTailLateEnergy += saxOutputEnergy();
        }
        saxTailEarlyEnergy /= 12.0;
        saxTailLateEnergy /= 12.0;
        passed &= expect(saxTailEngine.isFreeTailEnabled(saxMemory)
                             && saxTailEarlyEnergy > 0.00000001
                             && saxTailLateEnergy < saxTailEarlyEnergy * 0.80
                             && saxTailLateEnergy < chargedSaxEnergy * 0.55,
                         "CODA su RESPIRO deve lasciare gli FX udibili e decadenti senza nuova eccitazione");
        passed &= expect(saxTailStayedFinite && saxTailPeak < 0.90f,
                         "la coda RESPIRO deve restare finita e con headroom");
    }

    // DIRADA is decided only at a MIDI-loop boundary. Two enabled engines
    // must therefore publish the same single owner for exactly one rotation,
    // while an engine left OFF remains the bit-identical reference.
    {
        constexpr auto thinningSampleRate = 8000.0;
        constexpr auto thinningBlockSize = 400;
        constexpr auto thinningLoopBlocks = 19;
        constexpr std::array<int, 3> thinningMemories { 1, 2, 3 };
        constexpr std::array<int, 3> thinningChannels { 2, 3, 4 };
        constexpr std::array<int, 3> thinningNotes { 48, 55, 62 };

        auto thinningEngineStorage = std::make_unique<EcosystemEngine>();
        auto thinningTwinStorage = std::make_unique<EcosystemEngine>();
        auto thinningOffReferenceStorage
            = std::make_unique<EcosystemEngine>();
        auto& thinningEngine = *thinningEngineStorage;
        auto& thinningTwin = *thinningTwinStorage;
        auto& thinningOffReference = *thinningOffReferenceStorage;
        std::array<EcosystemEngine*, 3> thinningEngines {
            &thinningEngine, &thinningTwin, &thinningOffReference
        };
        for (auto* engine : thinningEngines)
        {
            engine->setScenarioIndex(0);
            for (const auto memory : thinningMemories)
                engine->setDelayLevel(memory, 0.0f);
            engine->prepare(thinningSampleRate, thinningBlockSize);
        }

        OutputBlock<EcosystemEngine::logicalOutputBusCount> thinningOutput(
            thinningBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> thinningTwinOutput(
            thinningBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> thinningOffOutput(
            thinningBlockSize);
        std::array<OutputBlock<EcosystemEngine::logicalOutputBusCount>*, 3>
            thinningOutputs {
                &thinningOutput, &thinningTwinOutput, &thinningOffOutput
            };
        const auto renderThinning = [=](EcosystemEngine& engine,
                                        auto& output)
        {
            output.clear();
            process(engine, nullptr, 0, output.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    thinningBlockSize);
        };
        const auto recordThinningLoops = [&](EcosystemEngine& engine,
                                              auto& output)
        {
            for (const auto memory : thinningMemories)
                engine.toggleRecording(memory);
            for (std::size_t index = 0; index < thinningMemories.size();
                 ++index)
                engine.enqueueMidiMessage(juce::MidiMessage::noteOn(
                    thinningChannels[index], thinningNotes[index], 0.72f));
            for (int block = 0; block < thinningLoopBlocks - 1; ++block)
                renderThinning(engine, output);
            for (std::size_t index = 0; index < thinningMemories.size();
                 ++index)
                engine.enqueueMidiMessage(juce::MidiMessage::noteOff(
                    thinningChannels[index], thinningNotes[index]));
            renderThinning(engine, output);
            for (const auto memory : thinningMemories)
                engine.toggleRecording(memory);
            renderThinning(engine, output);
        };
        for (std::size_t index = 0; index < thinningEngines.size(); ++index)
            recordThinningLoops(*thinningEngines[index],
                                *thinningOutputs[index]);

        const auto thinningDifference = [=](const auto& first,
                                             const auto& second)
        {
            auto difference = 0.0f;
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
                for (int sample = 0; sample < thinningBlockSize; ++sample)
                    difference = std::max(difference, std::abs(
                        first.storage[static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)]
                        - second.storage[static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)]));
            return difference;
        };

        auto thinningDefaultOffParity = true;
        auto thinningOriginalMaterialIntact = true;
        for (int block = 0; block < thinningLoopBlocks + 2; ++block)
        {
            for (std::size_t index = 0; index < thinningEngines.size(); ++index)
                renderThinning(*thinningEngines[index],
                               *thinningOutputs[index]);
            thinningDefaultOffParity &= thinningEngine.getThinnedMemoryIndex()
                    == -1
                && thinningTwin.getThinnedMemoryIndex() == -1
                && thinningOffReference.getThinnedMemoryIndex() == -1
                && thinningDifference(thinningOutput, thinningTwinOutput)
                    < 0.000001f
                && thinningDifference(thinningOutput, thinningOffOutput)
                    < 0.000001f;
            for (const auto memory : thinningMemories)
                thinningOriginalMaterialIntact &= thinningEngine.hasMaterial(
                        memory)
                    && thinningEngine.getEventCount(memory) == 2
                    && std::abs(thinningEngine.getLengthSeconds(memory)
                                - thinningLoopBlocks * thinningBlockSize
                                    / thinningSampleRate) < 0.000001;
        }
        passed &= expect(thinningDefaultOffParity,
                         "DIRADA spenta deve attraversare i wrap senza alterare stato o audio");

        thinningEngine.setThinningEnabled(true);
        thinningTwin.setThinningEnabled(true);
        auto thinningChoicesStayedDeterministic = true;
        auto thinningOnlyEligibleOwner = true;
        auto thinningChangedOnlyAtWrap = true;
        auto thinningStayedFinite = true;
        auto thinningPeak = 0.0f;
        auto thinningBecameAudible = false;
        auto firstThinnedMemory = -1;
        auto firstOwnerReleasedEarly = false;
        auto firstOwnerWraps = 0;
        auto firstOwnerCompletedOneRotation = false;

        // The musical scheduler deliberately waits 4--6 seconds before the
        // first event; eight loop lengths cover that wait plus the aligning
        // wrap without turning this into a wall-clock endurance test.
        constexpr auto maximumThinningBlocks = thinningLoopBlocks * 8;
        for (int block = 0; block < maximumThinningBlocks; ++block)
        {
            std::array<double, EcosystemEngine::midiMemoryCount> phaseBefore {};
            for (const auto memory : thinningMemories)
                phaseBefore[static_cast<std::size_t>(memory)]
                    = thinningEngine.getPhase(memory);
            const auto ownerBefore = thinningEngine.getThinnedMemoryIndex();

            renderThinning(thinningEngine, thinningOutput);
            renderThinning(thinningTwin, thinningTwinOutput);
            renderThinning(thinningOffReference, thinningOffOutput);

            const auto ownerAfter = thinningEngine.getThinnedMemoryIndex();
            const auto twinOwnerAfter
                = thinningTwin.getThinnedMemoryIndex();
            const auto validOwner = [](int owner)
            {
                return owner == -1
                    || (owner > EcosystemEngine::bassLayerIndex
                        && owner < EcosystemEngine::midiMemoryCount);
            };
            thinningOnlyEligibleOwner &= validOwner(ownerAfter)
                && ownerAfter != EcosystemEngine::midiMemoryCount;
            thinningChoicesStayedDeterministic &= ownerAfter == twinOwnerAfter
                && thinningDifference(thinningOutput, thinningTwinOutput)
                    < 0.000001f;
            thinningStayedFinite &= thinningOutput.finite(thinningBlockSize)
                && thinningTwinOutput.finite(thinningBlockSize)
                && thinningOffOutput.finite(thinningBlockSize);
            thinningPeak = std::max(thinningPeak,
                std::max(thinningOutput.peak(
                             EcosystemEngine::ambientLeftBus,
                             thinningBlockSize),
                         thinningOutput.peak(
                             EcosystemEngine::ambientRightBus,
                             thinningBlockSize)));
            thinningBecameAudible |= thinningDifference(
                thinningOutput, thinningOffOutput) > 0.00001f;

            const auto wrapped = [&](int memory)
            {
                return memory > EcosystemEngine::bassLayerIndex
                    && memory < EcosystemEngine::midiMemoryCount
                    && thinningEngine.getPhase(memory) + 0.000001
                        < phaseBefore[static_cast<std::size_t>(memory)];
            };
            if (ownerAfter != ownerBefore)
            {
                if (ownerBefore < 0)
                    thinningChangedOnlyAtWrap &= wrapped(ownerAfter);
                else
                    thinningChangedOnlyAtWrap &= wrapped(ownerBefore);
            }

            if (firstThinnedMemory < 0 && ownerAfter >= 0)
                firstThinnedMemory = ownerAfter;
            if (firstThinnedMemory >= 0 && ownerBefore == firstThinnedMemory)
            {
                if (wrapped(firstThinnedMemory))
                {
                    ++firstOwnerWraps;
                    firstOwnerCompletedOneRotation
                        = ownerAfter != firstThinnedMemory;
                }
                else if (ownerAfter != firstThinnedMemory)
                    firstOwnerReleasedEarly = true;
            }

            for (const auto memory : thinningMemories)
                thinningOriginalMaterialIntact &= thinningEngine.hasMaterial(
                        memory)
                    && thinningEngine.getEventCount(memory) == 2;
            if (firstOwnerCompletedOneRotation)
                break;
        }

        passed &= expect(firstThinnedMemory >= 1
                             && firstThinnedMemory < EcosystemEngine::midiMemoryCount
                             && firstOwnerCompletedOneRotation
                             && firstOwnerWraps == 1
                             && ! firstOwnerReleasedEarly,
                         "DIRADA deve iniziare al wrap e durare esattamente un giro della memoria scelta");
        passed &= expect(thinningOnlyEligibleOwner
                             && thinningOffReference.getThinnedMemoryIndex()
                                    == -1,
                         "DIRADA deve avere al massimo un owner MIDI 1-3, mai basso o RESPIRO");
        passed &= expect(thinningChoicesStayedDeterministic
                             && thinningChangedOnlyAtWrap,
                         "DIRADA deve essere deterministica e cambiare owner soltanto ai wrap");
        passed &= expect(thinningOriginalMaterialIntact
                             && thinningStayedFinite && thinningPeak < 0.90f
                             && thinningBecameAudible,
                         "DIRADA deve conservare i loop e l'headroom modificando realmente il mix");
    }

    // Isolate one sustained ambient memory to measure DIRADA's fade down and
    // fade back up. Clearing it after a mid-cycle disable also verifies that
    // no suppressed note-on/off pair can leave a voice stuck behind the gain.
    {
        constexpr auto thinningSampleRate = 8000.0;
        constexpr auto thinningBlockSize = 400;
        constexpr auto thinningLoopBlocks = 19;
        constexpr auto thinningMemory = 1;
        constexpr auto thinningChannel = 2;
        constexpr auto thinningNote = 48;

        auto thinningAudioEngineStorage = std::make_unique<EcosystemEngine>();
        auto thinningAudioReferenceStorage
            = std::make_unique<EcosystemEngine>();
        auto& thinningAudioEngine = *thinningAudioEngineStorage;
        auto& thinningAudioReference = *thinningAudioReferenceStorage;
        std::array<EcosystemEngine*, 2> thinningAudioEngines {
            &thinningAudioEngine, &thinningAudioReference
        };
        for (auto* engine : thinningAudioEngines)
        {
            engine->setScenarioIndex(0);
            engine->setDelayLevel(thinningMemory, 0.0f);
            engine->prepare(thinningSampleRate, thinningBlockSize);
        }
        OutputBlock<EcosystemEngine::logicalOutputBusCount> thinningAudioOutput(
            thinningBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> thinningAudioRefOutput(
            thinningBlockSize);
        const auto renderThinningAudioSamples = [=](EcosystemEngine& engine,
                                                     auto& output,
                                                     int samples)
        {
            output.clear();
            process(engine, nullptr, 0, output.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    samples);
        };
        const auto renderThinningAudio = [&](EcosystemEngine& engine,
                                              auto& output)
        {
            renderThinningAudioSamples(engine, output, thinningBlockSize);
        };
        const auto recordThinningAudioLoop = [&](EcosystemEngine& engine,
                                                  auto& output)
        {
            engine.toggleRecording(thinningMemory);
            engine.enqueueMidiMessage(juce::MidiMessage::noteOn(
                thinningChannel, thinningNote, 0.78f));
            for (int block = 0; block < thinningLoopBlocks - 1; ++block)
                renderThinningAudio(engine, output);
            engine.enqueueMidiMessage(juce::MidiMessage::noteOff(
                thinningChannel, thinningNote));
            renderThinningAudio(engine, output);
            engine.toggleRecording(thinningMemory);
            renderThinningAudio(engine, output);
        };
        recordThinningAudioLoop(thinningAudioEngine, thinningAudioOutput);
        recordThinningAudioLoop(thinningAudioReference,
                                thinningAudioRefOutput);

        const auto thinningAudioEnergy = [=](const auto& output)
        {
            auto energy = 0.0;
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
                for (int sample = 0; sample < thinningBlockSize; ++sample)
                {
                    const auto value = static_cast<double>(output.storage[
                        static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)]);
                    energy += value * value;
                }
            return energy / static_cast<double>(2 * thinningBlockSize);
        };
        const auto renderThinningAudioPair = [&]
        {
            renderThinningAudio(thinningAudioEngine, thinningAudioOutput);
            renderThinningAudio(thinningAudioReference,
                                thinningAudioRefOutput);
        };
        const auto renderThinningAudioPairSamples = [&](int samples)
        {
            renderThinningAudioSamples(thinningAudioEngine,
                                       thinningAudioOutput, samples);
            renderThinningAudioSamples(thinningAudioReference,
                                       thinningAudioRefOutput, samples);
        };

        for (int block = 0; block < 3; ++block)
            renderThinningAudioPair();
        // Move the otherwise block-aligned loop by a prime-sized partial
        // callback. Its following wraps now land inside a 400-sample block,
        // making the exact transition boundary observable without timestamps
        // or sleeps.
        renderThinningAudioPairSamples(137);
        thinningAudioEngine.setThinningEnabled(true);
        auto selectedAtWrap = false;
        auto selectionPrefixStayedIdentical = false;
        auto selectionBoundaryOffset = -1;
        const auto thinningLoopSamples = static_cast<int>(std::llround(
            thinningAudioEngine.getLengthSeconds(thinningMemory)
                * thinningSampleRate));
        constexpr auto maximumThinningWaitBlocks = 260; // 13 seconds.
        for (int block = 0; block < maximumThinningWaitBlocks; ++block)
        {
            const auto phaseBefore = thinningAudioEngine.getPhase(
                thinningMemory);
            const auto positionBefore = juce::jlimit(
                0, thinningLoopSamples - 1,
                static_cast<int>(std::llround(
                    phaseBefore * thinningLoopSamples)));
            const auto samplesUntilWrap = thinningLoopSamples
                - positionBefore;
            renderThinningAudioPair();
            if (thinningAudioEngine.getThinnedMemoryIndex() == thinningMemory)
            {
                selectedAtWrap = thinningAudioEngine.getPhase(thinningMemory)
                        + 0.000001 < phaseBefore;
                selectionBoundaryOffset = samplesUntilWrap;
                auto prefixDifference = 0.0f;
                if (selectionBoundaryOffset > 0
                    && selectionBoundaryOffset < thinningBlockSize)
                    for (int channel = EcosystemEngine::ambientLeftBus;
                         channel <= EcosystemEngine::ambientRightBus;
                         ++channel)
                        for (int sample = 0;
                             sample < selectionBoundaryOffset; ++sample)
                            prefixDifference = std::max(
                                prefixDifference, std::abs(
                                    thinningAudioOutput.storage[
                                        static_cast<std::size_t>(channel)][
                                            static_cast<std::size_t>(sample)]
                                    - thinningAudioRefOutput.storage[
                                        static_cast<std::size_t>(channel)][
                                            static_cast<std::size_t>(sample)]));
                selectionPrefixStayedIdentical
                    = selectionBoundaryOffset > 0
                    && selectionBoundaryOffset < thinningBlockSize
                    && prefixDifference < 0.000001f;
                break;
            }
        }

        std::array<float, 2> previousThinningSamples {
            thinningAudioOutput.storage[
                EcosystemEngine::ambientLeftBus].back(),
            thinningAudioOutput.storage[
                EcosystemEngine::ambientRightBus].back()
        };
        auto thinningMaximumStep = 0.0f;
        auto thinningAudioFinite = true;
        auto thinningEntryEnergy = 0.0;
        auto thinningMutedEnergy = 0.0;
        auto thinningMutedReferenceEnergy = 0.0;
        for (int block = 0; block < thinningLoopBlocks - 1; ++block)
        {
            renderThinningAudioPair();
            thinningAudioFinite &= thinningAudioOutput.finite(
                thinningBlockSize);
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
            {
                const auto index = static_cast<std::size_t>(channel);
                auto previous = previousThinningSamples[index];
                for (const auto sample : thinningAudioOutput.storage[index])
                {
                    thinningMaximumStep = std::max(
                        thinningMaximumStep, std::abs(sample - previous));
                    previous = sample;
                }
                previousThinningSamples[index] = previous;
            }
            if (block < 2)
                thinningEntryEnergy += thinningAudioEnergy(
                    thinningAudioOutput);
            if (block >= 6 && block < 14)
            {
                thinningMutedEnergy += thinningAudioEnergy(
                    thinningAudioOutput);
                thinningMutedReferenceEnergy += thinningAudioEnergy(
                    thinningAudioRefOutput);
            }
        }
        thinningEntryEnergy /= 2.0;
        thinningMutedEnergy /= 8.0;
        thinningMutedReferenceEnergy /= 8.0;

        auto naturalReturnReached = false;
        for (int block = 0; block < 3; ++block)
        {
            renderThinningAudioPair();
            naturalReturnReached |= thinningAudioEngine.getThinnedMemoryIndex()
                != thinningMemory;
        }
        auto thinningRestoredEnergy = 0.0;
        auto thinningRestoredReferenceEnergy = 0.0;
        for (int block = 0; block < 16; ++block)
        {
            renderThinningAudioPair();
            if (block >= 10)
            {
                thinningRestoredEnergy += thinningAudioEnergy(
                    thinningAudioOutput);
                thinningRestoredReferenceEnergy += thinningAudioEnergy(
                    thinningAudioRefOutput);
            }
        }

        passed &= expect(selectedAtWrap && selectionPrefixStayedIdentical
                             && naturalReturnReached
                             && thinningEntryEnergy > thinningMutedEnergy * 2.0
                             && thinningMutedEnergy
                                    < thinningMutedReferenceEnergy * 0.20,
                         "DIRADA deve sfumare soltanto dal campione esatto del wrap intra-blocco");
        passed &= expect(thinningRestoredEnergy
                                    > thinningRestoredReferenceEnergy * 0.35
                             && thinningAudioFinite
                             && thinningMaximumStep < 0.20f,
                         "a fine giro DIRADA deve far rientrare la memoria senza click");

        // Resetting OFF then ON creates a fresh deterministic selection. If
        // OFF arrives halfway through the silent rotation, keep that owner to
        // its wrap: an early resume would have missed the skipped note-on.
        // The same boundary starts the fade back and OFF must schedule no new
        // rests afterwards.
        thinningAudioEngine.setThinningEnabled(false);
        renderThinningAudioPair();
        thinningAudioEngine.setThinningEnabled(true);
        auto selectedForDisable = false;
        for (int block = 0; block < maximumThinningWaitBlocks; ++block)
        {
            renderThinningAudioPair();
            if (thinningAudioEngine.getThinnedMemoryIndex() == thinningMemory)
            {
                selectedForDisable = true;
                break;
            }
        }
        for (int block = 0; block < 7; ++block)
            renderThinningAudioPair();
        thinningAudioEngine.setThinningEnabled(false);
        thinningAudioEngine.setThinningEnabled(false);
        const auto disabledOwnerStayedPublished
            = thinningAudioEngine.getThinnedMemoryIndex() == thinningMemory;
        auto disabledOwnerStayedUntilWrap = true;
        auto disabledOwnerClearedAtWrap = false;
        auto disabledScheduledNoNewOwner = true;
        auto disabledMutedEnergy = 0.0;
        auto disabledMutedReferenceEnergy = 0.0;
        for (int block = 0; block < thinningLoopBlocks + 2; ++block)
        {
            const auto phaseBefore = thinningAudioEngine.getPhase(
                thinningMemory);
            renderThinningAudioPair();
            const auto wrapped = thinningAudioEngine.getPhase(thinningMemory)
                    + 0.000001 < phaseBefore;
            const auto ownerAfter
                = thinningAudioEngine.getThinnedMemoryIndex();
            if (! wrapped)
            {
                disabledOwnerStayedUntilWrap &= ownerAfter == thinningMemory;
                disabledMutedEnergy += thinningAudioEnergy(
                    thinningAudioOutput);
                disabledMutedReferenceEnergy += thinningAudioEnergy(
                    thinningAudioRefOutput);
            }
            else
            {
                disabledOwnerClearedAtWrap = ownerAfter == -1;
                break;
            }
        }
        auto disabledRestoreEnergy = 0.0;
        auto disabledRestoreReferenceEnergy = 0.0;
        for (int block = 0; block < thinningLoopBlocks * 2; ++block)
        {
            renderThinningAudioPair();
            disabledScheduledNoNewOwner
                &= thinningAudioEngine.getThinnedMemoryIndex() == -1;
            if (block >= 10 && block < 16)
            {
                disabledRestoreEnergy += thinningAudioEnergy(
                    thinningAudioOutput);
                disabledRestoreReferenceEnergy += thinningAudioEnergy(
                    thinningAudioRefOutput);
            }
        }
        passed &= expect(selectedForDisable
                             && ! thinningAudioEngine.isThinningEnabled()
                             && disabledOwnerStayedPublished
                             && disabledOwnerStayedUntilWrap
                             && disabledOwnerClearedAtWrap
                             && disabledScheduledNoNewOwner
                             && disabledMutedEnergy
                                    < disabledMutedReferenceEnergy * 0.20
                             && disabledRestoreEnergy
                                    > disabledRestoreReferenceEnergy * 0.35,
                         "DIRADA spenta deve completare il giro, rientrare al wrap e non creare nuovi owner");

        const auto preservedThinningEvents
            = thinningAudioEngine.getEventCount(thinningMemory);
        const auto preservedThinningLength
            = thinningAudioEngine.getLengthSeconds(thinningMemory);
        thinningAudioEngine.clearMemory(thinningMemory);
        renderThinningAudio(thinningAudioEngine, thinningAudioOutput);
        auto thinningLateTailPeak = 0.0f;
        // ABISSO's first ambient layer has a nine-second release. Fifteen
        // seconds lets that envelope and its existing reverb decay naturally;
        // a genuinely stuck sustain would remain plainly above this floor.
        constexpr auto thinningReleaseBlocks = 300;
        for (int block = 0; block < thinningReleaseBlocks; ++block)
        {
            renderThinningAudio(thinningAudioEngine, thinningAudioOutput);
            if (block >= thinningReleaseBlocks - 20)
                thinningLateTailPeak = std::max(thinningLateTailPeak,
                    std::max(thinningAudioOutput.peak(
                                 EcosystemEngine::ambientLeftBus,
                                 thinningBlockSize),
                             thinningAudioOutput.peak(
                                 EcosystemEngine::ambientRightBus,
                                 thinningBlockSize)));
        }
        passed &= expect(preservedThinningEvents == 2
                             && preservedThinningLength > 0.0
                             && ! thinningAudioEngine.hasMaterial(
                                 thinningMemory)
                             && thinningAudioEngine.getThinnedMemoryIndex()
                                    == -1
                             && thinningLateTailPeak < 0.00001f,
                         "DIRADA non deve perdere note-off o lasciare voci MIDI bloccate");
    }

    // A loop shorter than the hardware block can wrap several times in one
    // callback. DIRADA must start on the last usable wrap, not start and end
    // invisibly in that same callback.
    {
        constexpr auto shortLoopSampleRate = 8000.0;
        constexpr auto shortLoopBlockSize = 400;
        constexpr auto shortCaptureSamples = 90;
        constexpr auto shortLoopMemory = 1;

        auto shortLoopEngineStorage = std::make_unique<EcosystemEngine>();
        auto shortLoopReferenceStorage = std::make_unique<EcosystemEngine>();
        auto& shortLoopEngine = *shortLoopEngineStorage;
        auto& shortLoopReference = *shortLoopReferenceStorage;
        std::array<EcosystemEngine*, 2> shortLoopEngines {
            &shortLoopEngine, &shortLoopReference
        };
        for (auto* engine : shortLoopEngines)
        {
            engine->setScenarioIndex(1); // GOCCE: transienti leggibili.
            engine->setDelayLevel(shortLoopMemory, 0.0f);
            engine->prepare(shortLoopSampleRate, shortLoopBlockSize);
        }
        OutputBlock<EcosystemEngine::logicalOutputBusCount> shortLoopOutput(
            shortLoopBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> shortLoopRefOutput(
            shortLoopBlockSize);
        const auto renderShortLoop = [=](EcosystemEngine& engine,
                                         auto& output, int samples)
        {
            output.clear();
            process(engine, nullptr, 0, output.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, samples);
        };
        const auto recordShortLoop = [&](EcosystemEngine& engine,
                                          auto& output)
        {
            engine.toggleRecording(shortLoopMemory);
            engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(2, 60, 0.76f));
            renderShortLoop(engine, output, shortCaptureSamples);
            engine.enqueueMidiMessage(juce::MidiMessage::noteOff(2, 60));
            renderShortLoop(engine, output, shortCaptureSamples);
            engine.toggleRecording(shortLoopMemory);
            renderShortLoop(engine, output, shortLoopBlockSize);
        };
        recordShortLoop(shortLoopEngine, shortLoopOutput);
        recordShortLoop(shortLoopReference, shortLoopRefOutput);
        const auto shortLoopSamples = static_cast<int>(std::llround(
            shortLoopEngine.getLengthSeconds(shortLoopMemory)
                * shortLoopSampleRate));

        shortLoopEngine.setThinningEnabled(true);
        auto shortOwnerWasObservable = false;
        auto shortFadeWasAudible = false;
        auto shortLoopStayedFinite = true;
        auto shortLoopPeak = 0.0f;
        for (int block = 0; block < 140; ++block)
        {
            renderShortLoop(shortLoopEngine, shortLoopOutput,
                            shortLoopBlockSize);
            renderShortLoop(shortLoopReference, shortLoopRefOutput,
                            shortLoopBlockSize);
            shortLoopStayedFinite &= shortLoopOutput.finite(
                shortLoopBlockSize);
            shortLoopPeak = std::max(shortLoopPeak,
                std::max(shortLoopOutput.peak(
                             EcosystemEngine::ambientLeftBus,
                             shortLoopBlockSize),
                         shortLoopOutput.peak(
                             EcosystemEngine::ambientRightBus,
                             shortLoopBlockSize)));
            if (shortLoopEngine.getThinnedMemoryIndex() == shortLoopMemory)
            {
                shortOwnerWasObservable = true;
                for (int channel = EcosystemEngine::ambientLeftBus;
                     channel <= EcosystemEngine::ambientRightBus; ++channel)
                    for (int sample = 0; sample < shortLoopBlockSize; ++sample)
                        shortFadeWasAudible |= std::abs(
                            shortLoopOutput.storage[
                                static_cast<std::size_t>(channel)][
                                    static_cast<std::size_t>(sample)]
                            - shortLoopRefOutput.storage[
                                static_cast<std::size_t>(channel)][
                                    static_cast<std::size_t>(sample)])
                                > 0.000001f;
                break;
            }
        }

        shortLoopEngine.setThinningEnabled(false);
        renderShortLoop(shortLoopEngine, shortLoopOutput, shortLoopBlockSize);
        renderShortLoop(shortLoopReference, shortLoopRefOutput,
                        shortLoopBlockSize);
        const auto shortOwnerReturned
            = shortLoopEngine.getThinnedMemoryIndex() == -1;
        auto shortReturnEnergy = 0.0;
        auto shortReferenceEnergy = 0.0;
        for (int block = 0; block < 16; ++block)
        {
            renderShortLoop(shortLoopEngine, shortLoopOutput,
                            shortLoopBlockSize);
            renderShortLoop(shortLoopReference, shortLoopRefOutput,
                            shortLoopBlockSize);
            shortLoopStayedFinite &= shortLoopOutput.finite(
                shortLoopBlockSize);
            if (block >= 10)
                for (int channel = EcosystemEngine::ambientLeftBus;
                     channel <= EcosystemEngine::ambientRightBus; ++channel)
                    for (int sample = 0; sample < shortLoopBlockSize; ++sample)
                    {
                        const auto actual = shortLoopOutput.storage[
                            static_cast<std::size_t>(channel)][
                                static_cast<std::size_t>(sample)];
                        const auto reference = shortLoopRefOutput.storage[
                            static_cast<std::size_t>(channel)][
                                static_cast<std::size_t>(sample)];
                        shortReturnEnergy += actual * actual;
                        shortReferenceEnergy += reference * reference;
                    }
        }
        passed &= expect(shortLoopSamples > 0
                             && shortLoopSamples < shortLoopBlockSize
                             && shortOwnerWasObservable && shortOwnerReturned,
                         "DIRADA multi-wrap deve pubblicare l'owner oltre il callback di avvio");
        passed &= expect(shortFadeWasAudible && shortLoopStayedFinite
                             && shortLoopPeak < 0.90f,
                         "DIRADA multi-wrap deve produrre una sfumatura finita e contenuta");
        passed &= expect(shortReturnEnergy > shortReferenceEnergy * 0.20,
                         "DIRADA multi-wrap deve far rientrare il loop breve");
    }

    // The sax footswitch is a source-aware MIDI Learn control, not another
    // selectable memory. Learning must consume the gesture that establishes
    // the binding, then a complete release/press edge always addresses the
    // audio memory regardless of the currently selected performance target.
    {
        using MidiRole = EcosystemEngine::MidiInputRole;
        using PedalType = EcosystemEngine::SaxFootswitchMessageType;
        using PedalBinding = EcosystemEngine::SaxFootswitchBinding;
        constexpr auto pedalSampleRate = 8000.0;
        constexpr auto pedalBlockSize = 400;
        constexpr auto saxMemory = EcosystemEngine::midiMemoryCount;

        EcosystemEngine pedalEngine;
        pedalEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::cleanLooper);
        pedalEngine.setSaxStereoInput(false);
        pedalEngine.prepare(pedalSampleRate, pedalBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> pedalOutput(
            pedalBlockSize);
        std::vector<float> pedalInput(static_cast<std::size_t>(pedalBlockSize));
        for (int sample = 0; sample < pedalBlockSize; ++sample)
            pedalInput[static_cast<std::size_t>(sample)]
                = 0.12f * static_cast<float>(std::sin(
                    juce::MathConstants<double>::twoPi * 173.0
                    * static_cast<double>(sample) / pedalSampleRate));
        const float* pedalInputPointer = pedalInput.data();
        const auto renderPedal = [&](bool withInput)
        {
            pedalOutput.clear();
            process(pedalEngine,
                    withInput ? &pedalInputPointer : nullptr,
                    withInput ? 1 : 0, pedalOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, pedalBlockSize);
        };
        const auto bindingMatches = [](PedalBinding binding, MidiRole role,
                                       PedalType type, int number)
        {
            return binding.valid() && binding.role == role
                && binding.type == type && binding.number == number;
        };

        passed &= expect(! pedalEngine.hasSaxFootswitchBinding()
                             && ! pedalEngine.isSaxFootswitchLearning(),
                         "il pedale sax deve partire senza associazione o learn attivo");

        // Performance controls and panic CCs keep their fixed meanings and
        // cannot accidentally become the sax footswitch during MIDI Learn.
        pedalEngine.beginSaxFootswitchLearn();
        for (const auto controller : { 80, 81, 82, 83, 84, 120, 123 })
            pedalEngine.enqueueMidiMessage(
                juce::MidiMessage::controllerEvent(2, controller, 127),
                MidiRole::keyStep);
        passed &= expect(pedalEngine.isSaxFootswitchLearning()
                             && ! pedalEngine.hasSaxFootswitchBinding(),
                         "CC80-84 e CC120/123 non devono essere apprendibili");
        pedalEngine.cancelSaxFootswitchLearn();
        pedalEngine.releaseMomentaryGestures();
        pedalEngine.setSaxListenAmount(0.0f);
        passed &= expect(! pedalEngine.isSaxFootswitchLearning()
                             && ! pedalEngine.hasSaxFootswitchBinding(),
                         "annullare un learn senza candidato deve lasciare il pedale libero");

        // Musical notes, Program Change and transport must never win Learn:
        // they may already be flowing from a running KeyStep sequence and
        // cannot be distinguished safely from ordinary performance controls.
        pedalEngine.beginSaxFootswitchLearn();
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(1, 47, 1.0f), MidiRole::model12);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::programChange(1, 19), MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(juce::MidiMessage::midiStart(),
                                       MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::midiMachineControlCommand(
                juce::MidiMessage::mmc_play),
            MidiRole::keyStep);
        renderPedal(false);
        passed &= expect(pedalEngine.isSaxFootswitchLearning()
                             && ! pedalEngine.hasSaxFootswitchBinding()
                             && ! pedalEngine.isRecording(saxMemory),
                         "note, Program Change e transport non devono essere apprendibili");
        pedalEngine.cancelSaxFootswitchLearn();

        // Learn the common KeyStep sustain pedal. Its initial high message
        // establishes a held binding: repeated highs are inert until the first
        // low, so learning with the pedal down can never start a capture.
        pedalEngine.beginSaxFootswitchLearn();
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(false);
        const auto learnedCc = pedalEngine.getSaxFootswitchBinding();
        const auto ccLearnWasInert
            = bindingMatches(learnedCc, MidiRole::keyStep,
                             PedalType::controller, 64)
            && ! pedalEngine.isSaxFootswitchLearning()
            && ! pedalEngine.isRecording(saxMemory);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(false);
        passed &= expect(ccLearnWasInert
                             && ! pedalEngine.isRecording(saxMemory),
                         "il CC appreso e i doppi valori alti non devono ritoggle prima del rilascio");

        pedalEngine.setGestureTarget(3);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 0),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(true);
        auto pedalTargetsOnlySax = pedalEngine.isRecording(saxMemory);
        for (int memory = 0; memory < EcosystemEngine::midiMemoryCount;
             ++memory)
            pedalTargetsOnlySax &= ! pedalEngine.isRecording(memory);
        renderPedal(true);
        const auto seededAudioStayedFinite = pedalOutput.finite(pedalBlockSize);

        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 0),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(false);
        const auto capturedLength = pedalEngine.getLengthSeconds(saxMemory);
        const auto closedCycle = ! pedalEngine.isRecording(saxMemory)
            && pedalEngine.hasMaterial(saxMemory)
            && std::abs(capturedLength
                        - 2.0 * pedalBlockSize / pedalSampleRate) < 0.000001;

        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 0),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(true);
        const auto startedNurture = pedalEngine.isRecording(saxMemory)
            && pedalEngine.hasMaterial(saxMemory);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 0),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(false);
        const auto stoppedNurture = ! pedalEngine.isRecording(saxMemory)
            && pedalEngine.hasMaterial(saxMemory)
            && std::abs(pedalEngine.getLengthSeconds(saxMemory)
                        - capturedLength) < 0.000001;
        passed &= expect(pedalTargetsOnlySax && seededAudioStayedFinite
                             && closedCycle && startedNurture
                             && stoppedNurture,
                         "il pedale deve compiere SEMINA/CHIUDI/NUTRI/STOP sempre su RESPIRO");

        // A bound CC is removed before the ordinary MIDI FIFO, both on press
        // and release. Record a known note around pedal activity and verify the
        // loop retains exactly note-on and note-off.
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 64, 0),
            MidiRole::keyStep);
        pedalEngine.toggleRecording(1);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, 55, 0.7f), MidiRole::keyStep);
        renderPedal(false);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 64, 127),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 64, 127),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(2, 55), MidiRole::keyStep);
        renderPedal(false);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 64, 0),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(2, 64, 127),
            MidiRole::keyStep);
        pedalEngine.toggleRecording(1);
        renderPedal(false);
        passed &= expect(pedalEngine.hasMaterial(1)
                             && pedalEngine.getEventCount(1) == 2
                             && ! pedalEngine.isRecording(1)
                             && ! pedalEngine.isRecording(saxMemory),
                         "il binding del pedale deve essere consumato e mai registrato nei loop MIDI");

        // The input role is part of the saved assignment. A matching message
        // from the other physical port remains ordinary MIDI and cannot operate
        // RESPIRO. set/get also reject reserved performance CCs.
        pedalEngine.setSaxFootswitchBinding(
            { MidiRole::model12, PedalType::controller, 11 });
        const auto savedBinding = pedalEngine.getSaxFootswitchBinding();
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 11, 0),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 11, 127),
            MidiRole::keyStep);
        renderPedal(false);
        const auto wrongSourceWasIgnored
            = ! pedalEngine.isRecording(saxMemory);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 11, 0),
            MidiRole::model12);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 11, 127),
            MidiRole::model12);
        renderPedal(false);
        const auto rightSourceWorked = pedalEngine.isRecording(saxMemory);
        pedalEngine.toggleRecording(saxMemory);
        renderPedal(false);
        pedalEngine.setSaxFootswitchBinding(
            { MidiRole::keyStep, PedalType::controller, 80 });
        passed &= expect(bindingMatches(savedBinding, MidiRole::model12,
                                        PedalType::controller, 11)
                             && wrongSourceWasIgnored && rightSourceWorked
                             && ! pedalEngine.hasSaxFootswitchBinding(),
                         "set/get deve conservare la sorgente e rifiutare CC riservati");

        // Deterministic UI/MIDI interleavings contain two user actions and
        // therefore cancel as a pair; neither ordering may lose one toggle.
        pedalEngine.setSaxFootswitchBinding(
            { MidiRole::keyStep, PedalType::controller, 64 });
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 0),
            MidiRole::keyStep);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        pedalEngine.toggleRecording(saxMemory);
        renderPedal(false);
        const auto midiThenUiCancelled
            = ! pedalEngine.isRecording(saxMemory);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 0),
            MidiRole::keyStep);
        pedalEngine.toggleRecording(saxMemory);
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(false);
        const auto uiThenMidiCancelled
            = ! pedalEngine.isRecording(saxMemory);

        // Panic-release creates a fresh edge; clearing while held removes the
        // binding as well as its held state, so subsequent physical repeats are
        // harmless until a new assignment is installed.
        pedalEngine.releaseSaxFootswitch();
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(false);
        const auto releaseCreatedFreshEdge
            = pedalEngine.isRecording(saxMemory);
        pedalEngine.clearSaxFootswitchBinding();
        pedalEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(1, 64, 127),
            MidiRole::keyStep);
        renderPedal(false);
        const auto clearIgnoredHeldRepeat
            = pedalEngine.isRecording(saxMemory)
            && ! pedalEngine.hasSaxFootswitchBinding();
        pedalEngine.toggleRecording(saxMemory);
        renderPedal(false);
        passed &= expect(midiThenUiCancelled && uiThenMidiCancelled
                             && releaseCreatedFreshEdge
                             && clearIgnoredHeldRepeat
                             && ! pedalEngine.isRecording(saxMemory),
                         "UI/MIDI, panic e clear devono mantenere coerenti i fronti del pedale");
    }

    // Run the dry and ducked render sequentially so the 120-second sax memory
    // of one engine is released before constructing the next. The schedules
    // and signals are identical, making bass/sax equality deterministic while
    // avoiding timing assertions.
    {
        struct ListenProbe
        {
            std::array<double, 3> activeRms {};
            std::array<double, 3> releasedRms {};
            float maximumPeak = 0.0f;
            bool finite = true;
            bool released = false;
        };
        const auto runListenProbe = [](float amount)
        {
            constexpr auto probeSampleRate = 8000.0;
            constexpr auto probeBlockSize = 400;
            constexpr auto activeFirstBlock = 40;
            constexpr auto releaseBlock = 48;
            constexpr auto releasedFirstBlock = 64;
            constexpr auto finalBlock = 72;
            ListenProbe result;
            std::array<double, 3> activeEnergy {};
            std::array<double, 3> releasedEnergy {};
            int activeSamples = 0;
            int releasedSamples = 0;

            EcosystemEngine engine;
            engine.setScenarioIndex(2); // deterministic warm/reed voices
            engine.setSaxPathMode(EcosystemEngine::SaxPathMode::direct);
            engine.setSaxStereoInput(false);
            engine.setDelayLevel(2, 0.0f);
            engine.setSaxListenAmount(amount);
            engine.prepare(probeSampleRate, probeBlockSize);
            engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(3, 60, 0.65f));
            engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(5, 48, 0.65f));

            std::vector<float> saxInput(static_cast<std::size_t>(
                probeBlockSize));
            const float* inputPointer = saxInput.data();
            OutputBlock<EcosystemEngine::logicalOutputBusCount> output(
                probeBlockSize);
            int64_t samplePosition = 0;
            for (int block = 0; block < finalBlock; ++block)
            {
                if (block == releaseBlock)
                    engine.setSaxListenAmount(0.0f);
                for (int sample = 0; sample < probeBlockSize; ++sample)
                    saxInput[static_cast<std::size_t>(sample)]
                        = 0.30f * static_cast<float>(std::sin(
                            juce::MathConstants<double>::twoPi * 173.0
                            * static_cast<double>(samplePosition + sample)
                            / probeSampleRate));
                output.clear();
                process(engine, &inputPointer, 1, output.pointers.data(),
                        EcosystemEngine::logicalOutputBusCount,
                        probeBlockSize);
                result.finite &= output.finite(probeBlockSize);
                for (int bus = 0;
                     bus < EcosystemEngine::logicalOutputBusCount; ++bus)
                    result.maximumPeak = std::max(result.maximumPeak,
                        output.peak(static_cast<std::size_t>(bus),
                                    probeBlockSize));

                const auto accumulate = [&output, probeBlockSize](
                                            std::array<double, 3>& energy)
                {
                    for (int sample = 0; sample < probeBlockSize; ++sample)
                    {
                        const auto ambientLeft = output.storage[
                            EcosystemEngine::ambientLeftBus][
                                static_cast<std::size_t>(sample)];
                        const auto ambientRight = output.storage[
                            EcosystemEngine::ambientRightBus][
                                static_cast<std::size_t>(sample)];
                        const auto bass = output.storage[
                            EcosystemEngine::bassBus][
                                static_cast<std::size_t>(sample)];
                        const auto sax = output.storage[
                            EcosystemEngine::saxLeftBus][
                                static_cast<std::size_t>(sample)];
                        energy[0] += 0.5 * (ambientLeft * ambientLeft
                                          + ambientRight * ambientRight);
                        energy[1] += bass * bass;
                        energy[2] += sax * sax;
                    }
                };
                if (block >= activeFirstBlock && block < releaseBlock)
                {
                    accumulate(activeEnergy);
                    activeSamples += probeBlockSize;
                }
                if (block >= releasedFirstBlock)
                {
                    accumulate(releasedEnergy);
                    releasedSamples += probeBlockSize;
                }
                samplePosition += probeBlockSize;
            }
            for (std::size_t index = 0; index < result.activeRms.size();
                 ++index)
            {
                result.activeRms[index] = std::sqrt(
                    activeEnergy[index] / static_cast<double>(activeSamples));
                result.releasedRms[index] = std::sqrt(
                    releasedEnergy[index]
                    / static_cast<double>(releasedSamples));
            }
            result.released = engine.getSaxListenAmount() == 0.0f;
            return result;
        };

        const auto listenOff = runListenProbe(0.0f);
        const auto listenOn = runListenProbe(1.0f);
        const auto relativelyEqual = [](double first, double second)
        {
            return std::abs(first - second)
                <= std::max(1.0e-9, std::abs(first) * 0.00001);
        };
        passed &= expect(listenOff.activeRms[0] > 0.0001
                             && listenOn.activeRms[0]
                                < listenOff.activeRms[0] * 0.65
                             && relativelyEqual(listenOn.activeRms[1],
                                                listenOff.activeRms[1])
                             && relativelyEqual(listenOn.activeRms[2],
                                                listenOff.activeRms[2]),
                         "ASCOLTO deve ridurre soltanto il bus ambient, non basso o sax");
        passed &= expect(listenOff.finite && listenOn.finite
                             && listenOff.maximumPeak < 0.80f
                             && listenOn.maximumPeak < 0.80f,
                         "ASCOLTO deve conservare campioni finiti e headroom");
        passed &= expect(listenOn.released
                             && relativelyEqual(listenOn.releasedRms[0],
                                                listenOff.releasedRms[0])
                             && relativelyEqual(listenOn.releasedRms[1],
                                                listenOff.releasedRms[1])
                             && relativelyEqual(listenOn.releasedRms[2],
                                                listenOff.releasedRms[2]),
                         "rilasciare ASCOLTO deve ripristinare tutti i bus senza residui");
    }

    EcosystemEngine bassEngine;
    bassEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> bassOutput(blockSize);
    bassEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(5, 48, 0.85f));
    process(bassEngine, nullptr, 0, bassOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(bassEngine.getRealtimeSchedulingStatus() >= 0
                         && std::isfinite(bassEngine.getDspLoad())
                         && bassEngine.getDspLoad() >= 0.0f,
                     "il callback deve pubblicare stato realtime e carico DSP");
    passed &= expect(bassOutput.peak(EcosystemEngine::bassBus, blockSize) > 0.0001f,
                     "MIDI 5 deve suonare sul bus audio basso");
    passed &= expect(bassEngine.getBassOutputLevel() > 0.0001f,
                     "il meter basso deve seguire il segnale live");
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
    passed &= expect(! bassEngine.isRecording(EcosystemEngine::bassLayerIndex)
                         && ! bassEngine.hasMaterial(EcosystemEngine::bassLayerIndex)
                         && bassEngine.getEventCount(
                             EcosystemEngine::bassLayerIndex) == 0,
                     "il basso MIDI 5 deve restare live e non essere registrato");
    const auto bassSampleBeforeMute = bassOutput.storage[
        EcosystemEngine::bassBus].back();
    bassEngine.setBassEnabled(false);
    bassOutput.clear();
    process(bassEngine, nullptr, 0, bassOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(! bassEngine.isBassEnabled(),
                     "il basso live deve poter essere disattivato");
    passed &= expect(std::abs(bassOutput.storage[
                                 EcosystemEngine::bassBus].front()
                             - bassSampleBeforeMute) < 0.015f,
                     "MUTA BASSO LIVE deve usare un raccordo anti-click");
    bassOutput.clear();
    process(bassEngine, nullptr, 0, bassOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(bassOutput.silent(EcosystemEngine::bassBus, blockSize),
                     "MUTA BASSO LIVE deve silenziare la coda dopo il raccordo");

    // Stress every live-bass patch at maximum MIDI expression. The bass is a
    // dedicated performance voice rather than a looper, so its factory scene
    // levels must retain useful headroom before the user output trim.
    float quietestBassScenarioPeak = 1.0f;
    float loudestBassScenarioPeak = 0.0f;
    int calibratedBassScenarioCount = 0;
    for (int scenario = 0; scenario < CommentoScenarios::count; ++scenario)
    {
        ++calibratedBassScenarioCount;
        EcosystemEngine scenarioBassEngine;
        scenarioBassEngine.setScenarioIndex(scenario);
        scenarioBassEngine.setTextureAmount(1.0f);
        // A saved startup scene is applied immediately; runtime scene changes
        // deliberately take eight seconds and are covered separately below.
        scenarioBassEngine.prepare(sampleRate, blockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> scenarioBassOutput(
            blockSize);
        float scenarioPeak = 0.0f;

        for (const auto note : { 36, 48, 60 })
        {
            scenarioBassEngine.enqueueMidiMessage(
                juce::MidiMessage::channelPressureChange(5, 127));
            scenarioBassEngine.enqueueMidiMessage(
                juce::MidiMessage::controllerEvent(5, 74, 127));
            scenarioBassEngine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(5, note, 1.0f));
            for (int block = 0; block < 24; ++block)
            {
                scenarioBassOutput.clear();
                process(scenarioBassEngine, nullptr, 0,
                        scenarioBassOutput.pointers.data(),
                        EcosystemEngine::logicalOutputBusCount, blockSize);
                scenarioPeak = std::max(scenarioPeak,
                    scenarioBassOutput.peak(EcosystemEngine::bassBus,
                                            blockSize));
            }
            scenarioBassEngine.enqueueMidiMessage(
                juce::MidiMessage::noteOff(5, note));
            scenarioBassOutput.clear();
            process(scenarioBassEngine, nullptr, 0,
                    scenarioBassOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
        }

        quietestBassScenarioPeak = std::min(quietestBassScenarioPeak,
                                            scenarioPeak);
        loudestBassScenarioPeak = std::max(loudestBassScenarioPeak,
                                           scenarioPeak);
    }
    passed &= expect(calibratedBassScenarioCount == CommentoScenarios::count,
                     "ogni scenario, incluso COSMOS, deve avere il basso MIDI 5");
    passed &= expect(quietestBassScenarioPeak >= 0.24f
                         && loudestBassScenarioPeak <= 0.32f,
                     "ogni scena basso synth deve restare espressiva con headroom");
    passed &= expect(loudestBassScenarioPeak
                         <= quietestBassScenarioPeak * 1.08f,
                     "i livelli di fabbrica del basso devono essere coerenti");

    // COSMOS keeps MIDI 5 as the same dedicated live-bass path used by every
    // other scenario. Its only special audio behaviour is the independent
    // four-head rereading of the captured RESPIRO loop on the sax bus.
    {
        constexpr auto cosmosCaptureSamples = 24000;
        EcosystemEngine cosmosEngine;
        cosmosEngine.setScenarioIndex(CommentoScenarios::count - 1);
        cosmosEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::cleanLooper);
        cosmosEngine.setSaxStereoInput(true);
        cosmosEngine.setPerformanceLevel(EcosystemEngine::bassLayerIndex, 1.0f);
        cosmosEngine.setPerformanceLevel(EcosystemEngine::midiMemoryCount, 1.0f);
        cosmosEngine.prepare(sampleRate, cosmosCaptureSamples);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> cosmosOutput(
            cosmosCaptureSamples);

        cosmosEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(5, 48, 1.0f));
        process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        passed &= expect(
            cosmosOutput.peak(EcosystemEngine::bassBus, blockSize) > 0.0001f
                && cosmosOutput.silent(EcosystemEngine::ambientLeftBus,
                                       blockSize)
                && cosmosOutput.silent(EcosystemEngine::ambientRightBus,
                                       blockSize)
                && cosmosOutput.silent(EcosystemEngine::saxLeftBus, blockSize)
                && cosmosOutput.silent(EcosystemEngine::saxRightBus, blockSize),
            "il basso due-quadre COSMOS deve usare solo il bus dedicato");

        std::array<std::vector<float>, 2> cosmosInputStorage;
        std::array<const float*, 2> cosmosInputs {};
        for (std::size_t channel = 0; channel < cosmosInputStorage.size();
             ++channel)
        {
            cosmosInputStorage[channel].resize(cosmosCaptureSamples);
            for (int sample = 0; sample < cosmosCaptureSamples; ++sample)
                cosmosInputStorage[channel][static_cast<std::size_t>(sample)]
                    = 0.18f * static_cast<float>(std::sin(
                        juce::MathConstants<double>::twoPi
                        * (220.0 + 37.0 * static_cast<double>(channel))
                        * static_cast<double>(sample) / sampleRate));
            cosmosInputs[channel] = cosmosInputStorage[channel].data();
        }

        cosmosEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
        cosmosOutput.clear();
        process(cosmosEngine, cosmosInputs.data(), 2,
                cosmosOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, cosmosCaptureSamples);
        cosmosEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
        cosmosOutput.clear();
        process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        passed &= expect(
            cosmosEngine.hasMaterial(EcosystemEngine::midiMemoryCount)
                && cosmosOutput.peak(EcosystemEngine::saxLeftBus, blockSize)
                    > 0.0001f
                && cosmosOutput.peak(EcosystemEngine::bassBus, blockSize)
                    > 0.0001f
                && cosmosOutput.finite(blockSize),
            "le quattro testine COSMOS e il basso MIDI 5 devono restare indipendenti");
    }

    EcosystemEngine legatoBassEngine;
    legatoBassEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> legatoBassOutput(
        blockSize);
    float previousBassSample = 0.0f;
    float maximumBassBoundaryStep = 0.0f;
    for (int block = 0; block < 64; ++block)
    {
        legatoBassEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(
            5, block % 2 == 0 ? 48 : 55, 1.0f));
        legatoBassOutput.clear();
        process(legatoBassEngine, nullptr, 0,
                legatoBassOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        const auto& bass = legatoBassOutput.storage[
            EcosystemEngine::bassBus];
        maximumBassBoundaryStep = std::max(maximumBassBoundaryStep,
            std::abs(bass.front() - previousBassSample));
        previousBassSample = bass.back();
    }
    passed &= expect(maximumBassBoundaryStep < 0.015f,
                     "il basso mono legato non deve creare click al note-stealing");

    // When eight long-release notes occupy a polyphonic layer, the next chord
    // must steal every voice. The first new sample is crossfaded from each
    // voice's exact previous stereo contribution; without the residual ramp
    // this deterministic boundary jumps by about 0.08 in the left channel.
    AmbientSynth voiceStealSynth(1);
    SynthPatch voiceStealPatch;
    voiceStealPatch.model = OscillatorModel::glass;
    voiceStealPatch.attackSeconds = 0.001f;
    voiceStealPatch.decaySeconds = 1.0f;
    voiceStealPatch.sustain = 1.0f;
    voiceStealPatch.releaseSeconds = 10.0f;
    voiceStealPatch.cutoffHz = 18000.0f;
    voiceStealPatch.keyTrack = 0.0f;
    voiceStealPatch.harmonicMix = 0.0f;
    voiceStealPatch.lfoDepth = 0.0f;
    voiceStealPatch.level = 0.12f;
    voiceStealPatch.delayMix = 0.0f;
    voiceStealPatch.reverbWet = 0.0f;
    voiceStealSynth.setPatch(voiceStealPatch);
    voiceStealSynth.prepare(sampleRate, blockSize);
    juce::AudioBuffer<float> voiceStealOutput(2, blockSize);
    juce::MidiBuffer voiceStealMidi;
    for (int note = 0; note < 8; ++note)
        voiceStealMidi.addEvent(juce::MidiMessage::noteOn(
            2, 48 + note * 3, 0.9f), 0);
    voiceStealOutput.clear();
    voiceStealSynth.render(voiceStealOutput, voiceStealMidi, 0, blockSize);
    for (int block = 0; block < 32; ++block)
    {
        voiceStealOutput.clear();
        voiceStealMidi.clear();
        voiceStealSynth.render(voiceStealOutput, voiceStealMidi, 0, blockSize);
    }
    voiceStealMidi.clear();
    for (int note = 0; note < 8; ++note)
        voiceStealMidi.addEvent(juce::MidiMessage::noteOff(
            2, 48 + note * 3), 0);
    voiceStealOutput.clear();
    voiceStealSynth.render(voiceStealOutput, voiceStealMidi, 0, blockSize);
    const std::array<float, 2> previousVoiceStealSamples {
        voiceStealOutput.getSample(0, blockSize - 1),
        voiceStealOutput.getSample(1, blockSize - 1)
    };
    voiceStealMidi.clear();
    for (int note = 0; note < 8; ++note)
        voiceStealMidi.addEvent(juce::MidiMessage::noteOn(
            2, 48 + note * 3, 0.9f), 0);
    voiceStealOutput.clear();
    voiceStealSynth.render(voiceStealOutput, voiceStealMidi, 0, blockSize);
    const auto voiceStealBoundaryStep = std::max(
        std::abs(voiceStealOutput.getSample(0, 0)
                 - previousVoiceStealSamples[0]),
        std::abs(voiceStealOutput.getSample(1, 0)
                 - previousVoiceStealSamples[1]));
    passed &= expect(voiceStealBoundaryStep < 0.005f,
                     "il voice-stealing ambient deve avere un raccordo anti-crackle");

    EcosystemEngine loopEngine;
    loopEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> loopOutput(blockSize);

    // SEMINA on a MIDI memory arms the recorder without starting its clock.
    // Waiting for a musician must not bake pre-roll silence into the loop, and
    // pressing SEMINA again while still armed must be a clean cancellation.
    auto preArmNote = juce::MidiMessage::noteOn(3, 67, 0.7f);
    preArmNote.setTimeStamp(
        juce::Time::getMillisecondCounterHiRes() * 0.001 - 0.010);
    loopEngine.enqueueMidiMessage(preArmNote);
    loopEngine.toggleRecording(2);
    passed &= expect(loopEngine.isRecording(2)
                         && loopEngine.isWaitingForFirstNote(2),
                     "SEMINA MIDI deve mostrare subito lo stato armato");
    loopOutput.clear();
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(loopEngine.isWaitingForFirstNote(2),
                     "SEMINA MIDI deve attendere una nota successiva all'armamento");
    passed &= expect(! loopEngine.hasMaterial(2)
                         && loopEngine.getEventCount(2) == 0
                         && loopEngine.getLengthSeconds(2) == 0.0
                         && loopEngine.getPhase(2) == 0.0,
                     "una memoria MIDI armata non deve avanzare senza note");

    for (int block = 0; block < 4; ++block)
    {
        loopOutput.clear();
        process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
    }
    passed &= expect(loopEngine.isWaitingForFirstNote(2)
                         && loopEngine.getEventCount(2) == 0
                         && loopEngine.getLengthSeconds(2) == 0.0
                         && loopEngine.getPhase(2) == 0.0,
                     "l'attesa della prima nota non deve creare pre-roll MIDI");

    loopEngine.toggleRecording(2);
    loopOutput.clear();
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(! loopEngine.isWaitingForFirstNote(2)
                         && ! loopEngine.isRecording(2)
                         && ! loopEngine.hasMaterial(2)
                         && loopEngine.getEventCount(2) == 0
                         && loopEngine.getLengthSeconds(2) == 0.0,
                     "annullare SEMINA prima di suonare deve lasciare la memoria vuota");

    // Leave several idle blocks between SEMINA and the performance. The first
    // note-on becomes sample zero; only audio processed from that point onward
    // contributes to the recorded duration.
    loopEngine.toggleRecording(1);
    for (int block = 0; block < 5; ++block)
    {
        loopOutput.clear();
        process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
    }
    passed &= expect(loopEngine.isWaitingForFirstNote(1)
                         && loopEngine.getLengthSeconds(1) == 0.0,
                     "SEMINA non deve misurare il tempo prima della prima nota");

    loopEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(2, 55));
    loopOutput.clear();
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(loopEngine.isWaitingForFirstNote(1)
                         && loopEngine.getEventCount(1) == 0
                         && loopEngine.getLengthSeconds(1) == 0.0,
                     "una note-off isolata non deve avviare la memoria armata");

    loopEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(2, 55, 0.8f));
    loopOutput.clear();
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(! loopEngine.isWaitingForFirstNote(1)
                         && loopEngine.isRecording(1)
                         && loopEngine.getEventCount(1) == 1
                         && std::abs(loopEngine.getLengthSeconds(1)
                                     - blockSize / sampleRate) < 0.0001,
                     "la prima note-on deve diventare sample zero del loop MIDI");

    loopEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(2, 55));
    loopOutput.clear();
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(loopEngine.getEventCount(1) == 2,
                     "la registrazione avviata deve conservare la note-off");

    loopEngine.toggleRecording(1);
    loopOutput.clear();
    process(loopEngine, nullptr, 0, loopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    const auto loopLength = loopEngine.getLengthSeconds(1);
    passed &= expect(loopEngine.hasMaterial(1),
                     "il canale MIDI 2 deve creare una memoria ambient");
    passed &= expect(loopEngine.getEventCount(1) == 2,
                     "la memoria MIDI 2 deve contenere note on e note off");
    passed &= expect(std::abs(loopLength - 1024.0 / sampleRate) < 0.0001,
                     "la memoria MIDI deve escludere l'attesa e conservare la durata suonata");

    // A real JUCE MIDI timestamp between two callbacks must survive as an
    // intra-block offset. The exact sample depends on the scheduler, so only
    // assert the invariant: capture starts after sample zero and before the
    // end of the block. Stopping an open note must append a safe note-off
    // without changing that measured duration.
    EcosystemEngine timestampLoopEngine;
    timestampLoopEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> timestampLoopOutput(
        blockSize);
    timestampLoopEngine.toggleRecording(3);
    process(timestampLoopEngine, nullptr, 0,
            timestampLoopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    juce::Thread::sleep(4);
    auto timestampedNote = juce::MidiMessage::noteOn(4, 62, 0.8f);
    timestampedNote.setTimeStamp(
        juce::Time::getMillisecondCounterHiRes() * 0.001);
    timestampLoopEngine.enqueueMidiMessage(timestampedNote);
    juce::Thread::sleep(4);
    timestampLoopOutput.clear();
    process(timestampLoopEngine, nullptr, 0,
            timestampLoopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    const auto timestampedLength = timestampLoopEngine.getLengthSeconds(3);
    const auto timestampedSamples = static_cast<int>(std::llround(
        timestampedLength * sampleRate));
    passed &= expect(! timestampLoopEngine.isWaitingForFirstNote(3)
                         && timestampLoopEngine.getEventCount(3) == 1
                         && timestampedSamples > 0
                         && timestampedSamples < blockSize,
                     "il timestamp MIDI deve conservare l'offset dentro il blocco");

    timestampLoopEngine.toggleRecording(3);
    timestampLoopOutput.clear();
    process(timestampLoopEngine, nullptr, 0,
            timestampLoopOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(timestampLoopEngine.hasMaterial(3)
                         && timestampLoopEngine.getEventCount(3) == 2
                         && std::abs(timestampLoopEngine.getLengthSeconds(3)
                                     - timestampedLength) < 0.0001,
                     "lo stop deve chiudere una nota aperta senza alterare il loop");

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

    // DERIVA is deliberately rare, so exercise it on a low sample-rate clock:
    // the musical delays remain measured in seconds while the test stays fast.
    // Two enabled engines must make exactly the same choices, while a third
    // engine left at the default OFF state is the unaltered acoustic reference.
    {
        constexpr auto evolutionSampleRate = 8000.0;
        constexpr auto evolutionBlockSize = 400;
        constexpr auto evolutionMemory = 2;
        constexpr auto evolutionChannel = 3;
        constexpr auto evolutionNote = 60;
        constexpr auto companionNote = 67;

        EcosystemEngine evolutionEngine;
        EcosystemEngine evolutionTwin;
        EcosystemEngine evolutionOffReference;
        std::array<EcosystemEngine*, 3> evolutionEngines {
            &evolutionEngine, &evolutionTwin, &evolutionOffReference
        };
        for (auto* engine : evolutionEngines)
        {
            engine->setScenarioIndex(1); // GOCCE: short release, clear spectrum
            engine->setDelayLevel(evolutionMemory, 0.0f);
            engine->prepare(evolutionSampleRate, evolutionBlockSize);
        }
        evolutionOffReference.setLoopEvolutionEnabled(false);

        OutputBlock<EcosystemEngine::logicalOutputBusCount> evolutionOutput(
            evolutionBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> evolutionTwinOutput(
            evolutionBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> evolutionOffOutput(
            evolutionBlockSize);
        std::array<OutputBlock<EcosystemEngine::logicalOutputBusCount>*, 3>
            evolutionOutputs {
                &evolutionOutput, &evolutionTwinOutput, &evolutionOffOutput
            };

        const auto renderEvolutionEngine = [=](EcosystemEngine& engine,
                                                auto& output)
        {
            output.clear();
            process(engine, nullptr, 0, output.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    evolutionBlockSize);
        };
        const auto recordEvolutionLoop = [&](EcosystemEngine& engine,
                                              auto& output)
        {
            engine.toggleRecording(evolutionMemory);
            engine.enqueueMidiMessage(juce::MidiMessage::noteOn(
                evolutionChannel, evolutionNote, 0.80f));
            engine.enqueueMidiMessage(juce::MidiMessage::noteOn(
                evolutionChannel, companionNote, 0.62f));
            renderEvolutionEngine(engine, output);
            engine.enqueueMidiMessage(juce::MidiMessage::noteOff(
                evolutionChannel, evolutionNote));
            engine.enqueueMidiMessage(juce::MidiMessage::noteOff(
                evolutionChannel, companionNote));
            renderEvolutionEngine(engine, output);
            engine.toggleRecording(evolutionMemory);
            renderEvolutionEngine(engine, output);
        };
        for (std::size_t index = 0; index < evolutionEngines.size(); ++index)
            recordEvolutionLoop(*evolutionEngines[index],
                                *evolutionOutputs[index]);

        const auto originalEventCount = evolutionEngine.getEventCount(
            evolutionMemory);
        const auto originalLoopLength = evolutionEngine.getLengthSeconds(
            evolutionMemory);
        auto defaultOffMaximumDifference = 0.0f;
        auto defaultOffStayedNormal = true;
        for (int block = 0; block < 8; ++block)
        {
            for (std::size_t index = 0; index < evolutionEngines.size(); ++index)
                renderEvolutionEngine(*evolutionEngines[index],
                                      *evolutionOutputs[index]);
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
                for (int sample = 0; sample < evolutionBlockSize; ++sample)
                {
                    const auto reference = evolutionOffOutput.storage[
                        static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)];
                    defaultOffMaximumDifference = std::max(
                        defaultOffMaximumDifference,
                        std::abs(evolutionOutput.storage[
                                     static_cast<std::size_t>(channel)][
                                         static_cast<std::size_t>(sample)]
                                 - reference));
                    defaultOffMaximumDifference = std::max(
                        defaultOffMaximumDifference,
                        std::abs(evolutionTwinOutput.storage[
                                     static_cast<std::size_t>(channel)][
                                         static_cast<std::size_t>(sample)]
                                 - reference));
                }
            for (int memory = 1; memory < EcosystemEngine::midiMemoryCount;
                 ++memory)
                defaultOffStayedNormal &= evolutionEngine.getLoopEvolution(memory)
                        == EcosystemEngine::LoopEvolution::normal
                    && evolutionTwin.getLoopEvolution(memory)
                        == EcosystemEngine::LoopEvolution::normal
                    && evolutionOffReference.getLoopEvolution(memory)
                        == EcosystemEngine::LoopEvolution::normal;
        }
        passed &= expect(originalEventCount == 4
                             && originalLoopLength > 0.0
                             && defaultOffStayedNormal
                             && defaultOffMaximumDifference < 0.000001f,
                         "DERIVA spenta non deve alterare eventi, stato o audio");

        evolutionEngine.setLoopEvolutionEnabled(true);
        evolutionTwin.setLoopEvolutionEnabled(true);
        auto choicesStayedDeterministic = true;
        auto onlyOneEvolutionOwner = true;
        auto originalLoopStayedIntact = true;
        auto evolutionStayedFinite = true;
        auto sawOctaveUp = false;
        auto sawReverse = false;
        auto renderedReverseGhost = false;
        auto octaveSpectrumMeasured = false;
        auto octaveUpPower = 0.0;
        auto hypotheticalOctaveDownPower = 0.0;

        const auto spectralPower = [=](const auto& samples, double frequency)
        {
            auto real = 0.0;
            auto imaginary = 0.0;
            for (int sample = 0; sample < evolutionBlockSize; ++sample)
            {
                const auto phase = juce::MathConstants<double>::twoPi
                    * frequency * static_cast<double>(sample)
                    / evolutionSampleRate;
                const auto window = 0.5 - 0.5 * std::cos(
                    juce::MathConstants<double>::twoPi
                    * static_cast<double>(sample)
                    / static_cast<double>(evolutionBlockSize - 1));
                const auto value = static_cast<double>(samples[
                    static_cast<std::size_t>(sample)]) * window;
                real += value * std::cos(phase);
                imaginary -= value * std::sin(phase);
            }
            return real * real + imaginary * imaginary;
        };

        // The first event is due after 8-12 seconds and the second after the
        // global 18-35 second cooldown. 36 seconds covers the deterministic
        // +12 then REVERSE sequence for this memory.
        constexpr auto maximumEvolutionBlocks = 720;
        for (int block = 0; block < maximumEvolutionBlocks; ++block)
        {
            const auto stateBefore = evolutionEngine.getLoopEvolution(
                evolutionMemory);
            const auto phaseBefore = evolutionEngine.getPhase(evolutionMemory);
            renderEvolutionEngine(evolutionEngine, evolutionOutput);
            renderEvolutionEngine(evolutionTwin, evolutionTwinOutput);
            renderEvolutionEngine(evolutionOffReference, evolutionOffOutput);

            evolutionStayedFinite &= evolutionOutput.finite(
                evolutionBlockSize)
                && evolutionTwinOutput.finite(evolutionBlockSize)
                && evolutionOffOutput.finite(evolutionBlockSize);
            auto enabledTwinMaximumDifference = 0.0f;
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
                for (int sample = 0; sample < evolutionBlockSize; ++sample)
                    enabledTwinMaximumDifference = std::max(
                        enabledTwinMaximumDifference,
                        std::abs(evolutionOutput.storage[
                                     static_cast<std::size_t>(channel)][
                                         static_cast<std::size_t>(sample)]
                                 - evolutionTwinOutput.storage[
                                     static_cast<std::size_t>(channel)][
                                         static_cast<std::size_t>(sample)]));
            choicesStayedDeterministic &= enabledTwinMaximumDifference
                    < 0.000001f;

            auto activeEvolutionOwners = 0;
            for (int memory = 1; memory < EcosystemEngine::memoryCount; ++memory)
            {
                const auto firstState = evolutionEngine.getLoopEvolution(memory);
                const auto twinState = evolutionTwin.getLoopEvolution(memory);
                choicesStayedDeterministic &= firstState == twinState;
                if (firstState != EcosystemEngine::LoopEvolution::normal)
                    ++activeEvolutionOwners;
                sawOctaveUp |= firstState
                    == EcosystemEngine::LoopEvolution::octaveUp;
                sawReverse |= firstState
                    == EcosystemEngine::LoopEvolution::reverse;
            }
            onlyOneEvolutionOwner &= activeEvolutionOwners <= 1;
            originalLoopStayedIntact &= evolutionEngine.hasMaterial(
                    evolutionMemory)
                && evolutionEngine.getEventCount(evolutionMemory)
                    == originalEventCount
                && std::abs(evolutionEngine.getLengthSeconds(evolutionMemory)
                            - originalLoopLength) < 0.000001;

            // GOCCE's layer 3 has no transpose. Subtracting the perfectly
            // synchronised OFF engine isolates the ghost voice: its fundamental
            // must sit at +12, with no corresponding -12 copy.
            if (! octaveSpectrumMeasured
                && stateBefore == EcosystemEngine::LoopEvolution::octaveUp
                && phaseBefore < 0.001)
            {
                std::array<float, evolutionBlockSize> ghostDifference {};
                for (int sample = 0; sample < evolutionBlockSize; ++sample)
                    ghostDifference[static_cast<std::size_t>(sample)]
                        = evolutionOutput.storage[
                            EcosystemEngine::ambientLeftBus][
                                static_cast<std::size_t>(sample)]
                        - evolutionOffOutput.storage[
                            EcosystemEngine::ambientLeftBus][
                                static_cast<std::size_t>(sample)];
                const auto midiFrequency = [](int note)
                {
                    return 440.0 * std::pow(
                        2.0, static_cast<double>(note - 69) / 12.0);
                };
                octaveUpPower = spectralPower(
                    ghostDifference, midiFrequency(evolutionNote + 12));
                hypotheticalOctaveDownPower = spectralPower(
                    ghostDifference, midiFrequency(evolutionNote - 12));
                octaveSpectrumMeasured = true;
            }
            if (stateBefore == EcosystemEngine::LoopEvolution::reverse
                && phaseBefore < 0.001)
                renderedReverseGhost = true;
            if (sawOctaveUp && sawReverse && renderedReverseGhost
                && octaveSpectrumMeasured)
                break;
        }

        passed &= expect(choicesStayedDeterministic
                             && sawOctaveUp && sawReverse,
                         "DERIVA deve produrre deterministicamente +12 e REVERSE");
        passed &= expect(onlyOneEvolutionOwner,
                         "DERIVA deve assegnare una sola memoria ghost alla volta");
        passed &= expect(octaveSpectrumMeasured
                             && octaveUpPower > 0.000001
                             && octaveUpPower
                                    > hypotheticalOctaveDownPower * 8.0,
                         "la copia DERIVA deve essere solo un'ottava sopra, mai sotto");
        passed &= expect(originalLoopStayedIntact && evolutionStayedFinite,
                         "DERIVA non deve riscrivere il loop MIDI originale");

        // Clear while the reversed ghost has already emitted its note-on. Both
        // the owner channel and its hidden channel must release completely.
        evolutionEngine.clearMemory(evolutionMemory);
        renderEvolutionEngine(evolutionEngine, evolutionOutput);
        auto lateTailPeak = 0.0f;
        constexpr auto releaseBlocks = 300; // 15 seconds in GOCCE
        for (int block = 0; block < releaseBlocks; ++block)
        {
            renderEvolutionEngine(evolutionEngine, evolutionOutput);
            if (block >= releaseBlocks - 40)
                lateTailPeak = std::max(lateTailPeak,
                    std::max(evolutionOutput.peak(
                                 EcosystemEngine::ambientLeftBus,
                                 evolutionBlockSize),
                             evolutionOutput.peak(
                                 EcosystemEngine::ambientRightBus,
                                 evolutionBlockSize)));
        }
        passed &= expect(! evolutionEngine.hasMaterial(evolutionMemory)
                             && evolutionEngine.getEventCount(evolutionMemory) == 0
                             && evolutionEngine.getLoopEvolution(evolutionMemory)
                                    == EcosystemEngine::LoopEvolution::normal
                             && lateTailPeak < 0.00001f,
                         "il ghost MIDI deve spegnersi senza note bloccate al wrap o al clear");
    }

    // RESPIRO uses the same rare global scheduler, but evolves a short,
    // crossfaded reread of the audio loop. Compare it with an identical OFF
    // engine so onset and release of that added voice can be measured directly.
    {
        constexpr auto audioEvolutionSampleRate = 8000.0;
        constexpr auto audioEvolutionBlockSize = 400;
        constexpr auto captureBlocks = 80; // four-second seamless loop
        EcosystemEngine audioEvolutionEngine;
        EcosystemEngine audioEvolutionReference;
        for (auto* engine : { &audioEvolutionEngine, &audioEvolutionReference })
        {
            engine->setScenarioIndex(1);
            engine->setSaxPathMode(EcosystemEngine::SaxPathMode::cleanLooper);
            engine->setDelayLevel(EcosystemEngine::midiMemoryCount, 0.0f);
            engine->prepare(audioEvolutionSampleRate, audioEvolutionBlockSize);
        }
        audioEvolutionReference.setLoopEvolutionEnabled(false);

        std::array<std::vector<float>, 2> evolutionInputStorage;
        std::array<const float*, 2> evolutionInputs {};
        constexpr std::array<double, 2> inputFrequencies { 100.0, 140.0 };
        constexpr std::array<float, 2> inputLevels { 0.16f, 0.12f };
        for (std::size_t channel = 0; channel < evolutionInputStorage.size();
             ++channel)
        {
            evolutionInputStorage[channel].resize(audioEvolutionBlockSize);
            for (int sample = 0; sample < audioEvolutionBlockSize; ++sample)
                evolutionInputStorage[channel][static_cast<std::size_t>(sample)]
                    = inputLevels[channel] * static_cast<float>(std::sin(
                        juce::MathConstants<double>::twoPi
                        * inputFrequencies[channel]
                        * static_cast<double>(sample)
                        / audioEvolutionSampleRate));
            evolutionInputs[channel] = evolutionInputStorage[channel].data();
        }

        OutputBlock<EcosystemEngine::logicalOutputBusCount> audioEvolutionOutput(
            audioEvolutionBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> audioReferenceOutput(
            audioEvolutionBlockSize);
        const auto renderAudioEvolution = [&](EcosystemEngine& engine,
                                               auto& output,
                                               bool withInput)
        {
            output.clear();
            process(engine, withInput ? evolutionInputs.data() : nullptr,
                    withInput ? 2 : 0, output.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    audioEvolutionBlockSize);
        };
        audioEvolutionEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
        audioEvolutionReference.toggleRecording(EcosystemEngine::midiMemoryCount);
        for (int block = 0; block < captureBlocks; ++block)
        {
            renderAudioEvolution(audioEvolutionEngine, audioEvolutionOutput, true);
            renderAudioEvolution(audioEvolutionReference, audioReferenceOutput,
                                 true);
        }
        audioEvolutionEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
        audioEvolutionReference.toggleRecording(EcosystemEngine::midiMemoryCount);
        renderAudioEvolution(audioEvolutionEngine, audioEvolutionOutput, false);
        renderAudioEvolution(audioEvolutionReference, audioReferenceOutput, false);
        const auto capturedAudioLength = audioEvolutionEngine.getLengthSeconds(
            EcosystemEngine::midiMemoryCount);
        audioEvolutionEngine.setLoopEvolutionEnabled(true);

        auto audioEvolutionStayedFinite = true;
        auto audioEvolutionPeak = 0.0f;
        auto audioEvolutionMaximumStep = 0.0f;
        std::array<float, 2> previousAudioSamples {};
        auto sawAudioEvolution = false;
        auto audioEvolutionFinished = false;
        auto firstEvolutionDifference = -1.0f;
        auto lastEvolutionDifference = 0.0f;
        auto maximumEvolutionDifference = 0.0f;
        auto postEvolutionDifference = 1.0f;

        constexpr auto maximumAudioEvolutionBlocks = 440; // 22 seconds
        for (int block = 0; block < maximumAudioEvolutionBlocks; ++block)
        {
            const auto stateBefore = audioEvolutionEngine.getLoopEvolution(
                EcosystemEngine::midiMemoryCount);
            renderAudioEvolution(audioEvolutionEngine, audioEvolutionOutput,
                                 false);
            renderAudioEvolution(audioEvolutionReference, audioReferenceOutput,
                                 false);
            const auto stateAfter = audioEvolutionEngine.getLoopEvolution(
                EcosystemEngine::midiMemoryCount);

            auto squaredDifference = 0.0;
            for (int channel = EcosystemEngine::saxLeftBus;
                 channel <= EcosystemEngine::saxRightBus; ++channel)
            {
                for (int sample = 0; sample < audioEvolutionBlockSize; ++sample)
                {
                    const auto value = audioEvolutionOutput.storage[
                        static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)];
                    const auto reference = audioReferenceOutput.storage[
                        static_cast<std::size_t>(channel)][
                            static_cast<std::size_t>(sample)];
                    squaredDifference += static_cast<double>(value - reference)
                        * static_cast<double>(value - reference);
                    audioEvolutionPeak = std::max(audioEvolutionPeak,
                                                  std::abs(value));
                    audioEvolutionMaximumStep = std::max(
                        audioEvolutionMaximumStep,
                        std::abs(value - previousAudioSamples[
                            static_cast<std::size_t>(channel
                                - EcosystemEngine::saxLeftBus)]));
                    previousAudioSamples[static_cast<std::size_t>(
                        channel - EcosystemEngine::saxLeftBus)] = value;
                }
            }
            const auto differenceRms = static_cast<float>(std::sqrt(
                squaredDifference
                / static_cast<double>(audioEvolutionBlockSize * 2)));
            audioEvolutionStayedFinite &= audioEvolutionOutput.finite(
                    audioEvolutionBlockSize)
                && audioReferenceOutput.finite(audioEvolutionBlockSize);

            if (stateBefore != EcosystemEngine::LoopEvolution::normal)
            {
                if (! sawAudioEvolution)
                    firstEvolutionDifference = differenceRms;
                sawAudioEvolution = true;
                lastEvolutionDifference = differenceRms;
                maximumEvolutionDifference = std::max(
                    maximumEvolutionDifference, differenceRms);
            }
            else if (sawAudioEvolution
                     && stateAfter == EcosystemEngine::LoopEvolution::normal)
            {
                postEvolutionDifference = differenceRms;
                audioEvolutionFinished = true;
                break;
            }
        }

        passed &= expect(audioEvolutionEngine.hasMaterial(
                             EcosystemEngine::midiMemoryCount)
                             && std::abs(audioEvolutionEngine.getLengthSeconds(
                                    EcosystemEngine::midiMemoryCount)
                                        - capturedAudioLength) < 0.000001
                             && sawAudioEvolution && audioEvolutionFinished,
                         "DERIVA deve aggiungere una rilettura temporanea a RESPIRO");
        passed &= expect(audioEvolutionStayedFinite
                             && audioEvolutionPeak < 0.20f
                             && audioEvolutionMaximumStep < 0.035f,
                         "RESPIRO in DERIVA deve restare finito, con headroom e senza click");
        passed &= expect(maximumEvolutionDifference > 0.0001f
                             && firstEvolutionDifference
                                    < maximumEvolutionDifference * 0.12f
                             && lastEvolutionDifference
                                    < maximumEvolutionDifference * 0.12f
                             && postEvolutionDifference < 0.000001f,
                         "la rilettura RESPIRO deve entrare e uscire con dissolvenze graduali");
    }

    // Exercise the public morph contract on an exactly divisible time base:
    // 80 callbacks cover the declared eight seconds.  The loop is recorded
    // first so every progress observation also guards its event metadata.
    {
        constexpr auto morphSampleRate = 8000.0;
        constexpr auto morphBlockSize = 800;
        const auto morphTotalSamples = static_cast<int64_t>(std::llround(
            EcosystemEngine::scenarioMorphSeconds * morphSampleRate));
        const auto morphBlockCount = static_cast<int>(
            morphTotalSamples / morphBlockSize);
        passed &= expect(morphTotalSamples % morphBlockSize == 0
                             && morphBlockCount == 80,
                         "il test morph deve coprire esattamente la durata dichiarata");

        EcosystemEngine morphEngine;
        morphEngine.prepare(morphSampleRate, morphBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> morphOutput(
            morphBlockSize);
        const auto renderMorphBlock = [&]
        {
            morphOutput.clear();
            process(morphEngine, nullptr, 0, morphOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, morphBlockSize);
        };

        morphEngine.toggleRecording(1);
        morphEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, 50, 0.72f));
        renderMorphBlock();
        morphEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(2, 50));
        renderMorphBlock();
        morphEngine.toggleRecording(1);
        renderMorphBlock();
        const auto preservedEventCount = morphEngine.getEventCount(1);
        const auto preservedLoopLength = morphEngine.getLengthSeconds(1);
        passed &= expect(morphEngine.hasMaterial(1)
                             && preservedEventCount == 2
                             && preservedLoopLength > 0.0,
                         "il probe morph deve contenere un loop MIDI valido");

        constexpr auto firstMorphTarget = 6;
        morphEngine.setScenarioIndex(firstMorphTarget);
        auto previousProgress = -1.0f;
        auto firstProgress = 1.0f;
        auto penultimateProgress = 1.0f;
        bool progressStayedFiniteAndMonotonic = true;
        bool progressFollowedDeclaredDuration = true;
        bool loopSurvivedWholeMorph = true;
        const auto smoothStep = [](float position)
        {
            return position * position * (3.0f - 2.0f * position);
        };

        for (int block = 1; block <= morphBlockCount; ++block)
        {
            renderMorphBlock();
            const auto progress = morphEngine.getScenarioMorphProgress();
            if (block == 1)
                firstProgress = progress;
            if (block == morphBlockCount - 1)
                penultimateProgress = progress;

            const auto expectedProgress = smoothStep(
                static_cast<float>(block)
                    / static_cast<float>(morphBlockCount));
            progressStayedFiniteAndMonotonic &= std::isfinite(progress)
                && progress >= 0.0f && progress <= 1.0f
                && progress + 0.000001f >= previousProgress;
            progressFollowedDeclaredDuration &= std::abs(
                progress - expectedProgress) < 0.00001f;
            loopSurvivedWholeMorph &= morphEngine.hasMaterial(1)
                && morphEngine.getEventCount(1) == preservedEventCount
                && std::abs(morphEngine.getLengthSeconds(1)
                            - preservedLoopLength) < 0.000001;
            previousProgress = progress;
        }

        passed &= expect(firstProgress < 0.001f
                             && penultimateProgress < 1.0f
                             && std::abs(previousProgress - 1.0f) < 0.000001f
                             && progressStayedFiniteAndMonotonic
                             && progressFollowedDeclaredDuration,
                         "il morph deve avanzare monotono da zero a uno in otto secondi");
        passed &= expect(morphEngine.getScenarioIndex() == firstMorphTarget
                             && morphEngine.getScenarioMorphSourceIndex() == 0,
                         "il morph deve pubblicare sorgente e destinazione corrette");
        passed &= expect(loopSurvivedWholeMorph,
                         "il morph non deve modificare eventi o durata del loop MIDI");

        // Queue new requests during an audible, half-completed morph.  The
        // active two-tap crossfade must continue to its original destination;
        // only the latest queued request starts on the following callback.
        morphEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(4, 48, 0.65f));
        renderMorphBlock();
        constexpr auto interruptedTarget = 2;
        morphEngine.setScenarioIndex(interruptedTarget);
        bool firstHalfStayedFinite = true;
        for (int block = 0; block < morphBlockCount / 2; ++block)
        {
            renderMorphBlock();
            firstHalfStayedFinite &= morphOutput.finite(morphBlockSize)
                && std::isfinite(morphEngine.getScenarioMorphProgress());
        }
        const auto interruptedProgress = morphEngine.getScenarioMorphProgress();
        const std::array<float, 2> samplesBeforeRetarget {
            morphOutput.storage[EcosystemEngine::ambientLeftBus].back(),
            morphOutput.storage[EcosystemEngine::ambientRightBus].back()
        };

        constexpr auto supersededQueuedTarget = 10;
        const auto finalQueuedTarget = CommentoScenarios::count - 1;
        morphEngine.setScenarioIndex(supersededQueuedTarget);
        renderMorphBlock();
        const auto queuedProgress = morphEngine.getScenarioMorphProgress();
        const auto queuedBoundaryStep = std::max(
            std::abs(morphOutput.storage[EcosystemEngine::ambientLeftBus].front()
                     - samplesBeforeRetarget[0]),
            std::abs(morphOutput.storage[EcosystemEngine::ambientRightBus].front()
                     - samplesBeforeRetarget[1]));
        const auto firstQueuedRequestStayedQueued
            = morphEngine.getScenarioIndex() == supersededQueuedTarget
            && morphEngine.getScenarioMorphSourceIndex() == firstMorphTarget
            && morphEngine.getScenarioMorphDestinationIndex()
                   == interruptedTarget
            && std::abs(queuedProgress - smoothStep(
                static_cast<float>(morphBlockCount / 2 + 1)
                    / static_cast<float>(morphBlockCount))) < 0.00001f;

        morphEngine.setScenarioIndex(finalQueuedTarget);
        auto originalMorphStayedFiniteAndMonotonic = morphOutput.finite(
            morphBlockSize) && std::isfinite(queuedProgress);
        auto originalMorphPreviousProgress = queuedProgress;
        auto queuedMorphLoopSurvived = morphEngine.hasMaterial(1)
            && morphEngine.getEventCount(1) == preservedEventCount
            && std::abs(morphEngine.getLengthSeconds(1)
                        - preservedLoopLength) < 0.000001;
        auto latestRequestStayedQueued = true;
        for (int block = morphBlockCount / 2 + 2;
             block <= morphBlockCount; ++block)
        {
            renderMorphBlock();
            const auto progress = morphEngine.getScenarioMorphProgress();
            const auto expectedProgress = smoothStep(
                static_cast<float>(block)
                    / static_cast<float>(morphBlockCount));
            originalMorphStayedFiniteAndMonotonic &= morphOutput.finite(
                morphBlockSize) && std::isfinite(progress)
                && progress + 0.000001f >= originalMorphPreviousProgress
                && std::abs(progress - expectedProgress) < 0.00001f;
            latestRequestStayedQueued &= morphEngine.getScenarioIndex()
                    == finalQueuedTarget
                && morphEngine.getScenarioMorphSourceIndex()
                    == firstMorphTarget
                && morphEngine.getScenarioMorphDestinationIndex()
                    == interruptedTarget;
            queuedMorphLoopSurvived &= morphEngine.hasMaterial(1)
                && morphEngine.getEventCount(1) == preservedEventCount
                && std::abs(morphEngine.getLengthSeconds(1)
                            - preservedLoopLength) < 0.000001;
            originalMorphPreviousProgress = progress;
        }

        const std::array<float, 2> samplesBeforeQueuedMorph {
            morphOutput.storage[EcosystemEngine::ambientLeftBus].back(),
            morphOutput.storage[EcosystemEngine::ambientRightBus].back()
        };
        renderMorphBlock();
        const auto queuedMorphFirstProgress
            = morphEngine.getScenarioMorphProgress();
        const auto queuedMorphStartBoundaryStep = std::max(
            std::abs(morphOutput.storage[
                         EcosystemEngine::ambientLeftBus].front()
                     - samplesBeforeQueuedMorph[0]),
            std::abs(morphOutput.storage[
                         EcosystemEngine::ambientRightBus].front()
                     - samplesBeforeQueuedMorph[1]));
        auto queuedMorphStayedFiniteAndMonotonic = morphOutput.finite(
            morphBlockSize) && std::isfinite(queuedMorphFirstProgress);
        auto queuedMorphPreviousProgress = queuedMorphFirstProgress;
        for (int block = 2; block <= morphBlockCount; ++block)
        {
            renderMorphBlock();
            const auto progress = morphEngine.getScenarioMorphProgress();
            queuedMorphStayedFiniteAndMonotonic &= morphOutput.finite(
                morphBlockSize) && std::isfinite(progress)
                && progress + 0.000001f >= queuedMorphPreviousProgress;
            queuedMorphLoopSurvived &= morphEngine.hasMaterial(1)
                && morphEngine.getEventCount(1) == preservedEventCount
                && std::abs(morphEngine.getLengthSeconds(1)
                            - preservedLoopLength) < 0.000001;
            queuedMorphPreviousProgress = progress;
        }
        morphEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(4, 48));

        passed &= expect(firstHalfStayedFinite
                             && std::abs(interruptedProgress - 0.5f) < 0.00001f,
                         "il primo morph deve arrivare finito a meta' corsa");
        passed &= expect(firstQueuedRequestStayedQueued
                             && latestRequestStayedQueued
                             && std::abs(originalMorphPreviousProgress - 1.0f)
                                    < 0.000001f
                             && originalMorphStayedFiniteAndMonotonic,
                         "le richieste accodate non devono interrompere il morph attivo");
        passed &= expect(morphEngine.getScenarioIndex() == finalQueuedTarget
                             && morphEngine.getScenarioMorphSourceIndex()
                                    == interruptedTarget
                             && morphEngine.getScenarioMorphDestinationIndex()
                                    == finalQueuedTarget
                             && std::abs(queuedMorphFirstProgress - firstProgress)
                                    < 0.00001f
                             && std::abs(queuedMorphPreviousProgress - 1.0f)
                                    < 0.000001f
                             && queuedMorphStayedFiniteAndMonotonic,
                         "a fine corsa deve partire il morph verso l'ultima richiesta");
        passed &= expect(queuedBoundaryStep < 0.20f
                             && queuedMorphStartBoundaryStep < 0.20f,
                         "accodamento e avvio del morph seguente non devono creare salti macroscopici");
        passed &= expect(queuedMorphLoopSurvived,
                         "i morph accodati non devono cancellare o riscrivere il loop MIDI");
    }

    // Probe the global GRANA/FUZZ gestures through the direct sax path.  This
    // keeps the dry reference deterministic while still exercising the exact
    // master-bus ramps used by a performance.
    {
        constexpr auto gestureSampleRate = 8000.0;
        constexpr auto gestureBlockSize = 400;
        constexpr auto gestureFrequency = 173.0;
        constexpr auto gestureInputLevel = 0.15f;
        constexpr auto gestureDryGain = 0.58f;
        constexpr auto attackBlocks = 30;  // 1.5 seconds
        constexpr auto releaseBlocks = 60; // 3.0 seconds

        EcosystemEngine gestureEngine;
        gestureEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::direct);
        gestureEngine.setSaxStereoInput(false);
        gestureEngine.setTextureAmount(0.0f);
        gestureEngine.setFuzzEnabled(false);
        gestureEngine.prepare(gestureSampleRate, gestureBlockSize);

        std::array<std::vector<float>, 1> gestureInputStorage;
        gestureInputStorage[0].resize(gestureBlockSize);
        std::array<const float*, 1> gestureInputs {
            gestureInputStorage[0].data()
        };
        OutputBlock<EcosystemEngine::logicalOutputBusCount> gestureOutput(
            gestureBlockSize);
        int64_t gestureSamplePosition = 0;
        bool gestureOutputStayedFinite = true;
        const auto renderGestureBlock = [&](bool withInput = true)
        {
            for (int sample = 0; sample < gestureBlockSize; ++sample)
                gestureInputStorage[0][static_cast<std::size_t>(sample)]
                    = gestureInputLevel * static_cast<float>(std::sin(
                        juce::MathConstants<double>::twoPi
                        * gestureFrequency
                        * static_cast<double>(gestureSamplePosition + sample)
                        / gestureSampleRate));

            gestureOutput.clear();
            process(gestureEngine,
                    withInput ? gestureInputs.data() : nullptr,
                    withInput ? 1 : 0,
                    gestureOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    gestureBlockSize);
            gestureOutputStayedFinite &= gestureOutput.finite(
                gestureBlockSize);
            gestureSamplePosition += gestureBlockSize;
        };
        const auto differenceFromDry = [&]
        {
            auto squaredError = 0.0;
            for (int sample = 0; sample < gestureBlockSize; ++sample)
            {
                const auto expectedDry = gestureInputStorage[0][
                    static_cast<std::size_t>(sample)] * gestureDryGain;
                const auto error = gestureOutput.storage[
                    EcosystemEngine::saxLeftBus][static_cast<std::size_t>(sample)]
                    - expectedDry;
                squaredError += static_cast<double>(error) * error;
            }
            return static_cast<float>(std::sqrt(
                squaredError / static_cast<double>(gestureBlockSize)));
        };

        renderGestureBlock();
        const auto initialDryError = differenceFromDry();

        gestureEngine.setTextureAmount(1.0f);
        renderGestureBlock();
        const auto grainEarlyDifference = differenceFromDry();
        for (int block = 1; block < attackBlocks; ++block)
            renderGestureBlock();
        renderGestureBlock();
        const auto grainFullDifference = differenceFromDry();

        gestureEngine.setTextureAmount(0.0f);
        for (int block = 0; block < releaseBlocks; ++block)
            renderGestureBlock();
        renderGestureBlock();
        const auto dryAfterGrainError = differenceFromDry();

        passed &= expect(initialDryError < 0.000001f
                             && dryAfterGrainError < 0.000001f,
                         "GRANA spenta deve lasciare invariato il percorso dry");
        passed &= expect(grainEarlyDifference > 0.0f
                             && grainEarlyDifference
                                    < grainFullDifference * 0.10f,
                         "GRANA deve entrare con una rampa e non istantaneamente");
        passed &= expect(grainFullDifference > 0.003f,
                         "GRANA piena deve modificare realmente il segnale");

        gestureEngine.setFuzzEnabled(true);
        renderGestureBlock();
        const auto fuzzEarlyDifference = differenceFromDry();
        for (int block = 1; block < attackBlocks; ++block)
            renderGestureBlock();
        renderGestureBlock();
        const auto fuzzFullDifference = differenceFromDry();
        const auto sampleBeforeFuzzRelease = gestureOutput.storage[
            EcosystemEngine::saxLeftBus].back();

        gestureEngine.setFuzzEnabled(false);
        renderGestureBlock();
        const auto fuzzReleaseEarlyDifference = differenceFromDry();
        auto fuzzReleaseMaximumStep = 0.0f;
        auto previousFuzzReleaseSample = sampleBeforeFuzzRelease;
        const auto inspectFuzzRelease = [&]
        {
            for (const auto sample : gestureOutput.storage[
                     EcosystemEngine::saxLeftBus])
            {
                fuzzReleaseMaximumStep = std::max(
                    fuzzReleaseMaximumStep,
                    std::abs(sample - previousFuzzReleaseSample));
                previousFuzzReleaseSample = sample;
            }
        };
        inspectFuzzRelease();
        for (int block = 1; block < releaseBlocks / 2; ++block)
        {
            renderGestureBlock();
            inspectFuzzRelease();
        }
        const auto fuzzReleaseHalfDifference = differenceFromDry();
        for (int block = releaseBlocks / 2; block < releaseBlocks; ++block)
        {
            renderGestureBlock();
            inspectFuzzRelease();
        }
        const auto fuzzReleaseLateDifference = differenceFromDry();
        renderGestureBlock();
        inspectFuzzRelease();
        const auto finalDryError = differenceFromDry();
        renderGestureBlock(false);
        const auto fuzzReturnedToSilence = gestureOutput.silent(
            EcosystemEngine::saxLeftBus, gestureBlockSize)
            && gestureOutput.silent(EcosystemEngine::saxRightBus,
                                    gestureBlockSize);

        passed &= expect(fuzzEarlyDifference > 0.0f
                             && fuzzEarlyDifference
                                    < fuzzFullDifference * 0.10f
                             && fuzzFullDifference > 0.01f,
                         "FUZZ deve entrare gradualmente e diventare udibile");
        passed &= expect(fuzzReleaseEarlyDifference
                                    > fuzzFullDifference * 0.85f
                             && fuzzReleaseHalfDifference
                                    > fuzzFullDifference * 0.35f
                             && fuzzReleaseHalfDifference
                                    < fuzzFullDifference * 0.65f
                             && fuzzReleaseLateDifference
                                    < fuzzFullDifference * 0.03f
                             && finalDryError < 0.000001f
                             && fuzzReturnedToSilence,
                         "FUZZ spenta deve tornare gradualmente al dry e al silenzio");
        passed &= expect(gestureOutputStayedFinite
                             && fuzzReleaseMaximumStep < 0.08f,
                         "GRANA e FUZZ devono restare finite e senza salti macroscopici");
    }

    // At 48 kHz a full-strength GRANA capture changes every eight samples.
    // Feed those boundaries an alternating signal: the wet high-cut must
    // round each edge instead of reproducing the bright, unfiltered step.
    {
        constexpr auto filterProbeSamples = 64;
        EcosystemEngine grainFilterEngine;
        grainFilterEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::direct);
        grainFilterEngine.setSaxStereoInput(false);
        grainFilterEngine.setTextureAmount(1.0f);
        grainFilterEngine.setFuzzEnabled(false);
        grainFilterEngine.prepare(sampleRate, filterProbeSamples);

        std::array<std::vector<float>, 1> filterInputStorage;
        filterInputStorage[0].resize(filterProbeSamples);
        for (int sample = 0; sample < filterProbeSamples; ++sample)
            filterInputStorage[0][static_cast<std::size_t>(sample)]
                = ((sample / 8) % 2 == 0) ? 0.25f : -0.25f;
        std::array<const float*, 1> filterInputs {
            filterInputStorage[0].data()
        };
        OutputBlock<EcosystemEngine::logicalOutputBusCount> filterOutput(
            filterProbeSamples);
        process(grainFilterEngine, filterInputs.data(), 1,
                filterOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, filterProbeSamples);

        auto maximumFilteredStep = 0.0f;
        const auto& filteredSax = filterOutput.storage[
            EcosystemEngine::saxLeftBus];
        for (int sample = 1; sample < filterProbeSamples; ++sample)
            maximumFilteredStep = std::max(
                maximumFilteredStep,
                std::abs(filteredSax[static_cast<std::size_t>(sample)]
                         - filteredSax[static_cast<std::size_t>(sample - 1)]));

        passed &= expect(filterOutput.finite(filterProbeSamples)
                             && maximumFilteredStep < 0.18f,
                         "il passa-basso GRANA deve smussare le immagini acute");
    }

    // Fill all eight voices on each of the three loopable MIDI layers. This
    // reproduces the worst useful polyphony (24 ambient voices) without a
    // timing assertion that would depend on the build machine. The rendered
    // signal must remain finite, retain headroom and cross asynchronous loop
    // wraps without full-scale discontinuities.
    EcosystemEngine multiLoopEngine;
    constexpr auto factoryPerformanceGain = 0.5011872f; // -6 dB
    for (int layer = 1; layer < EcosystemEngine::midiMemoryCount; ++layer)
        multiLoopEngine.setPerformanceLevel(layer, factoryPerformanceGain);
    multiLoopEngine.prepare(sampleRate, blockSize);
    multiLoopEngine.setScenarioIndex(2); // NASTRO: hottest stress-probe scene
    OutputBlock<EcosystemEngine::logicalOutputBusCount> multiLoopOutput(
        blockSize);
    const auto recordChordLoop = [&multiLoopEngine, &multiLoopOutput,
                                  blockSize](
                                     int memory, int channel, int baseNote,
                                     int lengthInBlocks)
    {
        multiLoopEngine.toggleRecording(memory);
        multiLoopOutput.clear();
        process(multiLoopEngine, nullptr, 0, multiLoopOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);

        for (int block = 0; block < lengthInBlocks; ++block)
        {
            if (block == 0)
                for (int note = 0; note < 8; ++note)
                    multiLoopEngine.enqueueMidiMessage(
                        juce::MidiMessage::noteOn(
                            channel, baseNote + note * 4, 0.88f));
            if (block == lengthInBlocks / 2)
                for (int note = 0; note < 8; ++note)
                    multiLoopEngine.enqueueMidiMessage(
                        juce::MidiMessage::noteOff(
                            channel, baseNote + note * 4));
            multiLoopOutput.clear();
            process(multiLoopEngine, nullptr, 0,
                    multiLoopOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
        }

        multiLoopEngine.toggleRecording(memory);
        multiLoopOutput.clear();
        process(multiLoopEngine, nullptr, 0, multiLoopOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
    };

    recordChordLoop(1, 2, 48, 79);
    recordChordLoop(2, 3, 55, 103);
    recordChordLoop(3, 4, 60, 127);
    passed &= expect(multiLoopEngine.getEventCount(1) == 16
                         && multiLoopEngine.getEventCount(2) == 16
                         && multiLoopEngine.getEventCount(3) == 16,
                     "i tre loop di stress devono conservare tutte le note");

    // Keep the non-loopable performance bass held throughout the worst-case
    // ambient playback.  This catches regressions that are invisible when
    // only the shared stereo pair is inspected.
    multiLoopEngine.setLoopEvolutionEnabled(true);
    multiLoopEngine.enqueueMidiMessage(
        juce::MidiMessage::noteOn(5, 40, 0.92f));
    float multiLoopPeak = 0.0f;
    float multiLoopMaximumStep = 0.0f;
    float previousMultiLoopSample = 0.0f;
    float multiLoopBassPeak = 0.0f;
    float multiLoopBassMaximumStep = 0.0f;
    float previousMultiLoopBassSample = 0.0f;
    bool multiLoopStayedFinite = true;
    bool multiLoopBassStayedFinite = true;
    int multiLoopProtectedSamples = 0;
    bool multiLoopEvolutionOwnerStayedUnique = true;
    bool sawMultiLoopEvolution = false;
    // 1,200 blocks are 12.8 seconds at 48 kHz: enough to cross DERIVA's
    // deterministic 8-12 second first delay while all three loops and the live
    // bass are under the same worst-case load.
    for (int block = 0; block < 1200; ++block)
    {
        multiLoopOutput.clear();
        process(multiLoopEngine, nullptr, 0, multiLoopOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        auto activeEvolutionOwners = 0;
        for (int memory = 1; memory < EcosystemEngine::memoryCount; ++memory)
            if (multiLoopEngine.getLoopEvolution(memory)
                != EcosystemEngine::LoopEvolution::normal)
                ++activeEvolutionOwners;
        multiLoopEvolutionOwnerStayedUnique &= activeEvolutionOwners <= 1;
        sawMultiLoopEvolution |= activeEvolutionOwners == 1;
        multiLoopStayedFinite &= multiLoopOutput.finite(blockSize);
        for (int channel = EcosystemEngine::ambientLeftBus;
             channel <= EcosystemEngine::ambientRightBus; ++channel)
        {
            for (const auto sample : multiLoopOutput.storage[
                     static_cast<std::size_t>(channel)])
            {
                multiLoopPeak = std::max(multiLoopPeak, std::abs(sample));
                if (std::abs(sample) > 0.900001f)
                    ++multiLoopProtectedSamples;
                if (channel == EcosystemEngine::ambientLeftBus)
                {
                    multiLoopMaximumStep = std::max(multiLoopMaximumStep,
                        std::abs(sample - previousMultiLoopSample));
                    previousMultiLoopSample = sample;
                }
            }
        }
        for (const auto sample : multiLoopOutput.storage[
                 EcosystemEngine::bassBus])
        {
            multiLoopBassStayedFinite &= std::isfinite(sample);
            multiLoopBassPeak = std::max(multiLoopBassPeak,
                                         std::abs(sample));
            multiLoopBassMaximumStep = std::max(
                multiLoopBassMaximumStep,
                std::abs(sample - previousMultiLoopBassSample));
            previousMultiLoopBassSample = sample;
        }
    }
    multiLoopEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(5, 40));
    passed &= expect(multiLoopStayedFinite && multiLoopPeak < 0.60f,
                     "tre loop polifonici devono restare finiti e con headroom");
    passed &= expect(multiLoopProtectedSamples == 0,
                     "tre loop polifonici non devono tenere attiva la protezione");
    passed &= expect(multiLoopMaximumStep < 0.08f,
                     "i wrap asincroni dei tre loop non devono creare crackle");
    passed &= expect(multiLoopBassStayedFinite
                         && multiLoopBassPeak > 0.0001f
                         && multiLoopBassPeak < 0.50f,
                     "il basso tenuto con tre loop deve restare finito e con headroom");
    passed &= expect(multiLoopBassMaximumStep < 0.08f,
                     "il basso tenuto con tre loop non deve introdurre crackle");
    passed &= expect(multiLoopEngine.isLoopEvolutionEnabled()
                         && sawMultiLoopEvolution
                         && multiLoopEvolutionOwnerStayedUnique
                         && multiLoopEngine.getEventCount(1) == 16
                         && multiLoopEngine.getEventCount(2) == 16
                         && multiLoopEngine.getEventCount(3) == 16,
                     "tre loop e basso con DERIVA devono conservare eventi e un solo owner");

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

    // The generic preset deliberately converges ambient, bass and sax on the
    // same stereo pair. Each destination has three logical routes, therefore
    // every individual route must arrive at exactly one third of its logical
    // level. This checks both the complete mapping and its deterministic
    // headroom without relying on a particular audio interface.
    struct ToneRouteExpectation
    {
        EcosystemEngine::DiagnosticToneBus destination;
        int referenceBus;
        const char* message;
    };
    constexpr std::array<ToneRouteExpectation, 3> toneRouteExpectations {{
        { EcosystemEngine::DiagnosticToneBus::ambient,
          EcosystemEngine::ambientLeftBus,
          "il tono ambiente deve convergere sul preset stereo con headroom" },
        { EcosystemEngine::DiagnosticToneBus::bass,
          EcosystemEngine::bassBus,
          "il tono basso deve convergere sul preset stereo con headroom" },
        { EcosystemEngine::DiagnosticToneBus::sax,
          EcosystemEngine::saxLeftBus,
          "il tono sax deve convergere sul preset stereo con headroom" }
    }};

    for (const auto& expectation : toneRouteExpectations)
    {
        EcosystemEngine logicalToneEngine;
        logicalToneEngine.prepare(sampleRate, blockSize);
        logicalToneEngine.setDiagnosticToneBus(expectation.destination);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> logicalToneOutput(
            blockSize);
        process(logicalToneEngine, nullptr, 0, logicalToneOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);

        EcosystemEngine genericToneEngine;
        genericToneEngine.prepare(sampleRate, blockSize);
        genericToneEngine.setDiagnosticToneBus(expectation.destination);
        Model12AudioRouter genericToneRouter(genericToneEngine);
        genericToneRouter.setRoutingConfig(
            Model12AudioRouter::getGenericStereoRouting());
        OutputBlock<2> genericToneOutput(blockSize);
        process(genericToneRouter, nullptr, 0, genericToneOutput.pointers.data(),
                2, blockSize);

        const auto logicalPeak = logicalToneOutput.peak(
            static_cast<size_t>(expectation.referenceBus), blockSize);
        const auto leftPeak = genericToneOutput.peak(0, blockSize);
        const auto rightPeak = genericToneOutput.peak(1, blockSize);
        passed &= expect(logicalPeak > 0.001f
                             && std::abs(leftPeak * 3.0f - logicalPeak) < 0.00001f
                             && std::abs(rightPeak * 3.0f - logicalPeak) < 0.00001f,
                         expectation.message);
        passed &= expect(genericToneOutput.finite(blockSize),
                         "la convergenza dei bus deve produrre campioni finiti");
    }

    // A configurable router must use the selected physical input pair rather
    // than assuming channels 7/8. Distinct signs and amplitudes make an
    // accidental read from physical channel 1 immediately visible.
    EcosystemEngine selectedInputEngine;
    selectedInputEngine.prepare(sampleRate, blockSize);
    selectedInputEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::direct);
    selectedInputEngine.setSaxStereoInput(true);
    Model12AudioRouter selectedInputRouter(selectedInputEngine);
    Model12AudioRouter::RoutingConfig selectedInputRouting;
    selectedInputRouting.saxInputLeft = 3;
    selectedInputRouting.saxInputRight = 4;
    selectedInputRouting.ambientOutputLeft = Model12AudioRouter::RoutingConfig::none;
    selectedInputRouting.ambientOutputRight = Model12AudioRouter::RoutingConfig::none;
    selectedInputRouting.bassOutputLeft = Model12AudioRouter::RoutingConfig::none;
    selectedInputRouting.bassOutputRight = Model12AudioRouter::RoutingConfig::none;
    selectedInputRouting.saxOutputLeft = 2;
    selectedInputRouting.saxOutputRight = 3;
    selectedInputRouter.setRoutingConfig(selectedInputRouting);

    std::array<std::vector<float>, 5> selectableInputStorage;
    std::array<const float*, 5> selectableInputs {};
    constexpr std::array<float, 5> selectableValues {
        0.91f, -0.73f, 0.49f, 0.20f, -0.30f
    };
    for (size_t channel = 0; channel < selectableInputStorage.size(); ++channel)
    {
        selectableInputStorage[channel].assign(
            blockSize, selectableValues[channel]);
        selectableInputs[channel] = selectableInputStorage[channel].data();
    }
    OutputBlock<4> selectedInputOutput(blockSize, 0.75f);
    process(selectedInputRouter, selectableInputs.data(), 5,
            selectedInputOutput.pointers.data(), 4, blockSize);
    const auto selectedLeft = selectedInputOutput.storage[2][blockSize / 2];
    const auto selectedRight = selectedInputOutput.storage[3][blockSize / 2];
    passed &= expect(std::abs(selectedLeft - 0.20f * 0.58f) < 0.00001f
                         && std::abs(selectedRight + 0.30f * 0.58f) < 0.00001f,
                     "il router deve leggere la coppia di ingressi fisici scelta");
    passed &= expect(selectedInputOutput.silent(0, blockSize)
                         && selectedInputOutput.silent(1, blockSize)
                         && selectedInputOutput.finite(blockSize),
                     "il test diretto deve rispettare le sole uscite sax scelte");
    passed &= expect(selectedInputRouter.getPhysicalInputChannelCount() == 5,
                     "il router configurabile deve contare tutti gli ingressi fisici");

    // A mono destination for a stereo bus must be a downmix, not a silent
    // discard of its right side. Pointing both sax routes at one output asks
    // the router for (L + R) / 2 with deterministic headroom.
    selectedInputRouting.saxOutputLeft = 2;
    selectedInputRouting.saxOutputRight = 2;
    selectedInputRouter.setRoutingConfig(selectedInputRouting);
    selectedInputOutput.clear();
    process(selectedInputRouter, selectableInputs.data(), 5,
            selectedInputOutput.pointers.data(), 4, blockSize);
    const auto expectedMono = (0.20f - 0.30f) * 0.58f * 0.5f;
    passed &= expect(std::abs(selectedInputOutput.storage[2][blockSize / 2]
                              - expectedMono) < 0.00001f
                         && selectedInputOutput.silent(3, blockSize),
                     "una route sax mono deve fare downmix L+R senza scartare R");

    // Diagnostic path modes isolate progressively larger parts of the sax
    // chain. Muted must be bit-silent, direct must preserve the raw signal and
    // cleanLooper must be able to record/play a finite loop without effects.
    std::array<std::vector<float>, 2> diagnosticInputStorage;
    std::array<const float*, 2> diagnosticInputs {};
    for (size_t channel = 0; channel < diagnosticInputStorage.size(); ++channel)
    {
        diagnosticInputStorage[channel].assign(blockSize,
                                                channel == 0 ? 0.08f : -0.06f);
        diagnosticInputs[channel] = diagnosticInputStorage[channel].data();
    }

    EcosystemEngine mutedPathEngine;
    mutedPathEngine.prepare(sampleRate, blockSize);
    mutedPathEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::muted);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> mutedPathOutput(blockSize);
    process(mutedPathEngine, diagnosticInputs.data(), 2,
            mutedPathOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(mutedPathOutput.silent(EcosystemEngine::saxLeftBus, blockSize)
                         && mutedPathOutput.silent(EcosystemEngine::saxRightBus,
                                                   blockSize)
                         && mutedPathOutput.finite(blockSize),
                     "il percorso sax MUTO deve essere silenzioso e finito");

    EcosystemEngine directPathEngine;
    directPathEngine.prepare(sampleRate, blockSize);
    directPathEngine.setSaxStereoInput(true);
    directPathEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::direct);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> directPathOutput(blockSize);
    process(directPathEngine, diagnosticInputs.data(), 2,
            directPathOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(directPathOutput.peak(EcosystemEngine::saxLeftBus,
                                          blockSize) > 0.001f
                         && directPathOutput.peak(EcosystemEngine::saxRightBus,
                                                 blockSize) > 0.001f
                         && directPathOutput.finite(blockSize),
                     "il percorso sax DIRETTO deve passare segnale finito");

    constexpr auto cleanLoopSamples = 4096;
    EcosystemEngine cleanLooperEngine;
    cleanLooperEngine.prepare(sampleRate, cleanLoopSamples);
    cleanLooperEngine.setSaxStereoInput(true);
    cleanLooperEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::cleanLooper);
    std::array<std::vector<float>, 2> cleanInputStorage;
    std::array<const float*, 2> cleanInputs {};
    for (size_t channel = 0; channel < cleanInputStorage.size(); ++channel)
    {
        cleanInputStorage[channel].resize(cleanLoopSamples);
        for (int sample = 0; sample < cleanLoopSamples; ++sample)
            cleanInputStorage[channel][static_cast<size_t>(sample)] = 0.025f
                * static_cast<float>(std::sin(
                    juce::MathConstants<double>::twoPi
                    * (220.0 + 37.0 * static_cast<double>(channel))
                    * static_cast<double>(sample) / sampleRate));
        cleanInputs[channel] = cleanInputStorage[channel].data();
    }
    OutputBlock<EcosystemEngine::logicalOutputBusCount> cleanLooperOutput(
        cleanLoopSamples);
    cleanLooperEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    process(cleanLooperEngine, cleanInputs.data(), 2,
            cleanLooperOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, cleanLoopSamples);
    cleanLooperEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    cleanLooperOutput.clear();
    process(cleanLooperEngine, nullptr, 0, cleanLooperOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, cleanLoopSamples);
    passed &= expect(cleanLooperEngine.hasMaterial(
                         EcosystemEngine::midiMemoryCount)
                         && cleanLooperOutput.peak(EcosystemEngine::saxLeftBus,
                                                   cleanLoopSamples) > 0.0001f
                         && cleanLooperOutput.finite(cleanLoopSamples),
                     "LOOP PULITO deve registrare e riprodurre un loop finito");

    // Closing a first capture must not add the stored signal at full gain on a
    // callback boundary. DIMENTICA then fades only that stored component: the
    // live monitor remains present, and metadata disappears at gain zero.
    constexpr auto fadeProbeSamples = 512;
    EcosystemEngine saxFadeEngine;
    saxFadeEngine.prepare(sampleRate, fadeProbeSamples);
    saxFadeEngine.setSaxStereoInput(true);
    saxFadeEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::cleanLooper);
    std::array<std::vector<float>, 2> fadeInputStorage;
    std::array<const float*, 2> fadeInputs {};
    constexpr std::array<float, 2> fadeInputValues { 0.04f, -0.03f };
    for (std::size_t channel = 0; channel < fadeInputStorage.size(); ++channel)
    {
        fadeInputStorage[channel].assign(
            fadeProbeSamples, fadeInputValues[channel]);
        fadeInputs[channel] = fadeInputStorage[channel].data();
    }
    OutputBlock<EcosystemEngine::logicalOutputBusCount> saxFadeOutput(
        fadeProbeSamples);
    saxFadeEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    float captureMonitorLast = 0.0f;
    for (int block = 0; block < 6; ++block)
    {
        saxFadeOutput.clear();
        process(saxFadeEngine, fadeInputs.data(), 2,
                saxFadeOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, fadeProbeSamples);
        captureMonitorLast = saxFadeOutput.storage[
            EcosystemEngine::saxLeftBus][fadeProbeSamples - 1];
    }

    saxFadeEngine.toggleRecording(EcosystemEngine::midiMemoryCount);
    saxFadeOutput.clear();
    process(saxFadeEngine, fadeInputs.data(), 2,
            saxFadeOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, fadeProbeSamples);
    const auto captureCloseFirst = saxFadeOutput.storage[
        EcosystemEngine::saxLeftBus][0];
    const auto captureCloseLast = saxFadeOutput.storage[
        EcosystemEngine::saxLeftBus][fadeProbeSamples - 1];
    passed &= expect(std::abs(captureCloseFirst - captureMonitorLast) < 0.00001f
                         && captureCloseLast > captureCloseFirst
                         && saxFadeEngine.hasMaterial(
                             EcosystemEngine::midiMemoryCount),
                     "chiudere la prima cattura RESPIRO deve inserirla con fade-in");

    // Let the 125 ms fade-in reach unity before probing the one-second clear.
    for (int block = 0; block < 16; ++block)
    {
        saxFadeOutput.clear();
        process(saxFadeEngine, fadeInputs.data(), 2,
                saxFadeOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, fadeProbeSamples);
    }
    const auto stableLoopLast = saxFadeOutput.storage[
        EcosystemEngine::saxLeftBus][fadeProbeSamples - 1];

    saxFadeEngine.clearMemory(EcosystemEngine::midiMemoryCount);
    saxFadeOutput.clear();
    process(saxFadeEngine, fadeInputs.data(), 2,
            saxFadeOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, fadeProbeSamples);
    const auto clearFirst = saxFadeOutput.storage[
        EcosystemEngine::saxLeftBus][0];
    const auto clearFirstBlockLast = saxFadeOutput.storage[
        EcosystemEngine::saxLeftBus][fadeProbeSamples - 1];
    passed &= expect(std::abs(clearFirst - stableLoopLast) < 0.00001f
                         && clearFirstBlockLast < clearFirst
                         && saxFadeEngine.hasMaterial(
                             EcosystemEngine::midiMemoryCount),
                     "DIMENTICA RESPIRO deve partire senza taglio e tenere la memoria durante il fade");

    // 93 blocks of 512 samples are still just below one second including the
    // first block above; the following block crosses the exact zero point.
    for (int block = 1; block < 93; ++block)
    {
        saxFadeOutput.clear();
        process(saxFadeEngine, fadeInputs.data(), 2,
                saxFadeOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, fadeProbeSamples);
    }
    const auto metadataSurvivedUntilZero = saxFadeEngine.hasMaterial(
        EcosystemEngine::midiMemoryCount);
    saxFadeOutput.clear();
    process(saxFadeEngine, fadeInputs.data(), 2,
            saxFadeOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, fadeProbeSamples);
    const auto expectedLiveMonitor = fadeInputValues[0] * 0.58f;
    const auto finalMonitorSample = saxFadeOutput.storage[
        EcosystemEngine::saxLeftBus][fadeProbeSamples - 1];
    passed &= expect(metadataSurvivedUntilZero
                         && ! saxFadeEngine.hasMaterial(
                             EcosystemEngine::midiMemoryCount)
                         && std::abs(finalMonitorSample - expectedLiveMonitor)
                                < 0.00001f
                         && saxFadeOutput.finite(fadeProbeSamples),
                     "DIMENTICA RESPIRO deve cancellare a zero senza mutare il monitor live");

    EcosystemEngine effectsPathEngine;
    effectsPathEngine.prepare(sampleRate, blockSize);
    effectsPathEngine.setSaxStereoInput(true);
    effectsPathEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::sceneEffects);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> effectsPathOutput(blockSize);
    process(effectsPathEngine, diagnosticInputs.data(), 2,
            effectsPathOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(effectsPathOutput.finite(blockSize),
                     "il percorso sax FX deve produrre campioni finiti");

    // With no musical input, every diagnostic tone destination must energise
    // only its requested logical bus (or stereo pair). This is the invariant
    // used by the touch panel to test one section at a time.
    EcosystemEngine toneSelectionEngine;
    toneSelectionEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> toneSelectionOutput(
        blockSize);
    toneSelectionEngine.setDiagnosticToneBus(
        EcosystemEngine::DiagnosticToneBus::off);
    process(toneSelectionEngine, nullptr, 0,
            toneSelectionOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(toneSelectionOutput.silent(
                         EcosystemEngine::ambientLeftBus, blockSize)
                         && toneSelectionOutput.silent(
                             EcosystemEngine::ambientRightBus, blockSize)
                         && toneSelectionOutput.silent(
                             EcosystemEngine::bassBus, blockSize)
                         && toneSelectionOutput.silent(
                             EcosystemEngine::saxLeftBus, blockSize)
                         && toneSelectionOutput.silent(
                             EcosystemEngine::saxRightBus, blockSize),
                     "tono diagnostico OFF deve lasciare tutti i bus muti");

    for (const auto& expectation : toneRouteExpectations)
    {
        toneSelectionOutput.clear();
        toneSelectionEngine.setDiagnosticToneBus(expectation.destination);
        process(toneSelectionEngine, nullptr, 0,
                toneSelectionOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);

        const auto ambientActive = expectation.destination
            == EcosystemEngine::DiagnosticToneBus::ambient;
        const auto bassActive = expectation.destination
            == EcosystemEngine::DiagnosticToneBus::bass;
        const auto saxActive = expectation.destination
            == EcosystemEngine::DiagnosticToneBus::sax;
        passed &= expect((toneSelectionOutput.peak(
                              EcosystemEngine::ambientLeftBus, blockSize)
                              > 0.001f) == ambientActive
                             && (toneSelectionOutput.peak(
                                     EcosystemEngine::ambientRightBus, blockSize)
                                     > 0.001f) == ambientActive
                             && (toneSelectionOutput.peak(
                                     EcosystemEngine::bassBus, blockSize)
                                     > 0.001f) == bassActive
                             && (toneSelectionOutput.peak(
                                     EcosystemEngine::saxLeftBus, blockSize)
                                     > 0.001f) == saxActive
                             && (toneSelectionOutput.peak(
                                     EcosystemEngine::saxRightBus, blockSize)
                                     > 0.001f) == saxActive,
                         "il tono diagnostico deve raggiungere soltanto il bus scelto");
    }

    // Performance trims are independent targets, and zero must really mute
    // each synth before the shared ambient mix. Raising a muted trim again is
    // deliberately smoothed but must become audible without restarting DSP.
    EcosystemEngine storedLevelEngine;
    constexpr std::array<float, EcosystemEngine::memoryCount> storedLevels {
        0.0f, 0.2f, 0.4f, 0.6f, 0.8f
    };
    bool independentTargets = true;
    bool independentDelayTargets = true;
    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
    {
        storedLevelEngine.setPerformanceLevel(
            index, storedLevels[static_cast<size_t>(index)]);
        independentTargets &= std::abs(storedLevelEngine.getPerformanceLevel(index)
            - storedLevels[static_cast<size_t>(index)]) < 0.000001f;
        const auto delayAmount = static_cast<float>(index) * 0.2f;
        storedLevelEngine.setDelayLevel(index, delayAmount);
        independentDelayTargets &= std::abs(
            storedLevelEngine.getDelayLevel(index) - delayAmount) < 0.000001f;
    }
    passed &= expect(independentTargets,
                     "i cinque livelli devono conservare target indipendenti");
    passed &= expect(independentDelayTargets,
                     "i cinque controlli delay devono restare indipendenti");

    PerformanceLevels smoothingProbe;
    smoothingProbe.setTargetGain(0, 0.0f);
    smoothingProbe.prepare(sampleRate);
    smoothingProbe.setTargetGain(0, 1.0f);
    juce::AudioBuffer<float> smoothingBuffer(2, blockSize);
    for (int channel = 0; channel < smoothingBuffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
            smoothingBuffer.setSample(channel, sample, 1.0f);
    smoothingProbe.process(0, smoothingBuffer, blockSize);
    passed &= expect(smoothingBuffer.getSample(0, 0) < 0.01f
                         && smoothingBuffer.getSample(0, blockSize - 1) > 0.20f
                         && smoothingBuffer.getSample(0, blockSize - 1) < 0.50f,
                     "i cambi di livello devono usare una rampa anti-click");

    for (int layer = 0; layer < EcosystemEngine::midiMemoryCount; ++layer)
    {
        EcosystemEngine levelEngine;
        levelEngine.setPerformanceLevel(layer, 0.0f);
        levelEngine.prepare(sampleRate, blockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> levelOutput(blockSize);
        levelEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(
            levelEngine.getMidiChannelForMemory(layer), 60, 0.85f));
        process(levelEngine, nullptr, 0, levelOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);

        const auto mutedPeak = layer == EcosystemEngine::bassLayerIndex
            ? levelOutput.peak(EcosystemEngine::bassBus, blockSize)
            : std::max(levelOutput.peak(EcosystemEngine::ambientLeftBus, blockSize),
                       levelOutput.peak(EcosystemEngine::ambientRightBus, blockSize));
        passed &= expect(mutedPeak < 0.000001f,
                         "un livello MIDI a zero deve silenziare solo il suo synth");

        levelEngine.setPerformanceLevel(layer, 1.0f);
        float restoredPeak = 0.0f;
        for (int block = 0; block < 16; ++block)
        {
            levelOutput.clear();
            process(levelEngine, nullptr, 0, levelOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
            restoredPeak = std::max(restoredPeak,
                layer == EcosystemEngine::bassLayerIndex
                    ? levelOutput.peak(EcosystemEngine::bassBus, blockSize)
                    : std::max(levelOutput.peak(EcosystemEngine::ambientLeftBus,
                                                blockSize),
                               levelOutput.peak(EcosystemEngine::ambientRightBus,
                                                blockSize)));
        }
        passed &= expect(restoredPeak > 0.00001f,
                         "rialzare un livello MIDI deve ripristinare il synth");
    }

    EcosystemEngine saxLevelEngine;
    saxLevelEngine.setPerformanceLevel(EcosystemEngine::midiMemoryCount, 0.0f);
    saxLevelEngine.prepare(sampleRate, blockSize);
    saxLevelEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::direct);
    saxLevelEngine.setSaxStereoInput(true);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> saxLevelOutput(blockSize);
    process(saxLevelEngine, diagnosticInputs.data(), 2,
            saxLevelOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
    passed &= expect(saxLevelOutput.silent(EcosystemEngine::saxLeftBus, blockSize)
                         && saxLevelOutput.silent(EcosystemEngine::saxRightBus,
                                                  blockSize),
                     "il livello SAX a zero deve silenziare monitor e loop");

    saxLevelEngine.setPerformanceLevel(EcosystemEngine::midiMemoryCount, 1.0f);
    float restoredSaxPeak = 0.0f;
    for (int block = 0; block < 6; ++block)
    {
        saxLevelOutput.clear();
        process(saxLevelEngine, diagnosticInputs.data(), 2,
                saxLevelOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        restoredSaxPeak = std::max(restoredSaxPeak,
            std::max(saxLevelOutput.peak(EcosystemEngine::saxLeftBus, blockSize),
                     saxLevelOutput.peak(EcosystemEngine::saxRightBus, blockSize)));
    }
    passed &= expect(restoredSaxPeak > 0.001f,
                     "rialzare il livello SAX deve ripristinare il monitor");

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

    // The GESTI transport is deliberately per-memory: only the three MIDI
    // loopers and RESPIRO can be paused.  PAUSA freezes the read head without
    // erasing material, while the live bass and the other loopers keep
    // running.  A sustained note crossing the frozen playhead must be rebuilt
    // on PLAY; simply waiting for its next stored note-on would leave an
    // audible hole for the rest of that rotation.
    {
        constexpr auto transportSampleRate = 8000.0;
        constexpr auto transportBlockSize = 400;
        auto transportEngineStorage = std::make_unique<EcosystemEngine>();
        auto& transportEngine = *transportEngineStorage;
        transportEngine.setScenarioIndex(1); // GOCCE: quick, measurable tails.
        for (int memory = 1; memory < EcosystemEngine::midiMemoryCount;
             ++memory)
            transportEngine.setDelayLevel(memory, 0.0f);
        transportEngine.prepare(transportSampleRate, transportBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> transportOutput(
            transportBlockSize);
        const auto renderTransport = [&]
        {
            transportOutput.clear();
            process(transportEngine, nullptr, 0,
                    transportOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    transportBlockSize);
        };
        const auto recordTransportLoop = [&](int memory, int channel,
                                              int note, int blocks,
                                              int noteOffBlock)
        {
            transportEngine.toggleRecording(memory);
            transportEngine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(channel, note, 0.82f));
            for (int block = 0; block < blocks; ++block)
            {
                if (block == noteOffBlock)
                    transportEngine.enqueueMidiMessage(
                        juce::MidiMessage::noteOff(channel, note));
                renderTransport();
            }
            transportEngine.toggleRecording(memory);
            renderTransport();
        };

        auto transportStartsPlaying = true;
        for (int memory = 1; memory < EcosystemEngine::memoryCount; ++memory)
            transportStartsPlaying &= transportEngine.isLoopPlaying(memory);
        const auto bassTransportState
            = transportEngine.isLoopPlaying(EcosystemEngine::bassLayerIndex);
        transportEngine.setLoopPlaying(EcosystemEngine::bassLayerIndex, false);
        transportEngine.setLoopPlaying(-1, false);
        transportEngine.setLoopPlaying(EcosystemEngine::memoryCount, false);
        passed &= expect(transportStartsPlaying
                             && transportEngine.isLoopPlaying(
                                 EcosystemEngine::bassLayerIndex)
                                    == bassTransportState,
                         "PLAY deve essere il default e il basso live non deve accettare PAUSA");

        recordTransportLoop(1, 2, 48, 17, 12);
        // Park the first loop at its first playback block while the other
        // memories are captured, so the later pause point is provably between
        // this note-on and note-off rather than dependent on elapsed helpers.
        transportEngine.setLoopPlaying(1, false);
        recordTransportLoop(2, 3, 55, 19, 13);
        recordTransportLoop(3, 4, 60, 23, 16);
        transportEngine.setLoopPlaying(1, true);
        std::array<int, EcosystemEngine::midiMemoryCount>
            preservedTransportEvents {};
        std::array<double, EcosystemEngine::midiMemoryCount>
            preservedTransportLengths {};
        for (int memory = 1; memory < EcosystemEngine::midiMemoryCount;
             ++memory)
        {
            preservedTransportEvents[static_cast<std::size_t>(memory)]
                = transportEngine.getEventCount(memory);
            preservedTransportLengths[static_cast<std::size_t>(memory)]
                = transportEngine.getLengthSeconds(memory);
        }

        // Leave memory 1 inside its sustained note, then freeze only that
        // playhead. The coprime loop lengths ensure memories 2 and 3 cannot
        // accidentally return to their exact starting phase during this
        // observation window.
        renderTransport();
        renderTransport();
        const auto phaseOneBeforePause = transportEngine.getPhase(1);
        const auto phaseTwoBeforePause = transportEngine.getPhase(2);
        const auto phaseThreeBeforePause = transportEngine.getPhase(3);
        transportEngine.setLoopPlaying(1, false);

        auto transportFinite = true;
        auto transportPeak = 0.0f;
        auto transportMaximumStep = 0.0f;
        std::array<float, 2> previousTransportSamples {
            transportOutput.storage[EcosystemEngine::ambientLeftBus].back(),
            transportOutput.storage[EcosystemEngine::ambientRightBus].back()
        };
        for (int block = 0; block < 40; ++block)
        {
            renderTransport();
            transportFinite &= transportOutput.finite(transportBlockSize);
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
            {
                const auto index = static_cast<std::size_t>(channel);
                auto previous = previousTransportSamples[
                    static_cast<std::size_t>(
                        channel - EcosystemEngine::ambientLeftBus)];
                for (const auto sample : transportOutput.storage[index])
                {
                    transportPeak = std::max(transportPeak,
                                             std::abs(sample));
                    transportMaximumStep = std::max(
                        transportMaximumStep, std::abs(sample - previous));
                    previous = sample;
                }
                previousTransportSamples[static_cast<std::size_t>(
                    channel - EcosystemEngine::ambientLeftBus)] = previous;
            }
        }
        const auto frozenPhaseOne = transportEngine.getPhase(1);
        const auto otherMidiLoopsAdvanced
            = std::abs(transportEngine.getPhase(2) - phaseTwoBeforePause)
                    > 0.000001
            && std::abs(transportEngine.getPhase(3) - phaseThreeBeforePause)
                    > 0.000001;
        auto midiTransportMaterialIntact = true;
        for (int memory = 1; memory < EcosystemEngine::midiMemoryCount;
             ++memory)
            midiTransportMaterialIntact &= transportEngine.hasMaterial(memory)
                && transportEngine.getEventCount(memory)
                    == preservedTransportEvents[static_cast<std::size_t>(memory)]
                && std::abs(transportEngine.getLengthSeconds(memory)
                    - preservedTransportLengths[static_cast<std::size_t>(memory)])
                    < 0.000001;
        passed &= expect(! transportEngine.isLoopPlaying(1)
                             && transportEngine.isLoopPlaying(2)
                             && transportEngine.isLoopPlaying(3)
                             && std::abs(frozenPhaseOne - phaseOneBeforePause)
                                    < 0.000000001
                             && otherMidiLoopsAdvanced
                             && midiTransportMaterialIntact,
                         "PAUSA MIDI deve essere indipendente e conservare fase e materiale");

        // The live bass remains playable while every stored MIDI loop is
        // paused.  Keep all ambient loopers stopped long enough for their
        // release/reverb state to drain, so the following PLAY probe can only
        // succeed if memory 1 reconstructs the note active at its playhead.
        const auto phaseTwoBeforeAllPause = transportEngine.getPhase(2);
        const auto phaseThreeBeforeAllPause = transportEngine.getPhase(3);
        transportEngine.setLoopPlaying(2, false);
        transportEngine.setLoopPlaying(3, false);
        transportEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(5, 43, 0.88f));
        auto pausedBassPeak = 0.0f;
        for (int block = 0; block < 8; ++block)
        {
            renderTransport();
            pausedBassPeak = std::max(pausedBassPeak,
                transportOutput.peak(EcosystemEngine::bassBus,
                                     transportBlockSize));
        }
        transportEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(5, 43));
        for (int block = 0; block < 120; ++block)
            renderTransport();
        // PAUSA belongs only to the stored loop. The same card's live MIDI
        // channel must remain playable while its private playback channel is
        // frozen.
        transportEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, 72, 0.82f));
        auto pausedLayerLivePeak = 0.0f;
        for (int block = 0; block < 4; ++block)
        {
            renderTransport();
            pausedLayerLivePeak = std::max(
                pausedLayerLivePeak,
                std::max(transportOutput.peak(
                             EcosystemEngine::ambientLeftBus,
                             transportBlockSize),
                         transportOutput.peak(
                             EcosystemEngine::ambientRightBus,
                             transportBlockSize)));
        }
        transportEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(2, 72));
        for (int block = 0; block < 120; ++block)
            renderTransport();
        const auto phaseBeforeResume = transportEngine.getPhase(1);
        previousTransportSamples = {
            transportOutput.storage[EcosystemEngine::ambientLeftBus].back(),
            transportOutput.storage[EcosystemEngine::ambientRightBus].back()
        };
        transportEngine.setLoopPlaying(1, true);
        auto resumedCrossingNotePeak = 0.0f;
        auto midiFadeInStartEnergy = 0.0;
        auto midiFadeInEndEnergy = 0.0;
        constexpr auto transportFadeProbeSamples = 64;
        for (int block = 0; block < 4; ++block)
        {
            renderTransport();
            transportFinite &= transportOutput.finite(transportBlockSize);
            resumedCrossingNotePeak = std::max(resumedCrossingNotePeak,
                std::max(transportOutput.peak(
                             EcosystemEngine::ambientLeftBus,
                             transportBlockSize),
                         transportOutput.peak(
                             EcosystemEngine::ambientRightBus,
                             transportBlockSize)));
            if (block == 0)
                for (int channel = EcosystemEngine::ambientLeftBus;
                     channel <= EcosystemEngine::ambientRightBus; ++channel)
                    for (int sample = 0;
                         sample < transportFadeProbeSamples; ++sample)
                    {
                        const auto first = static_cast<double>(
                            transportOutput.storage[
                                static_cast<std::size_t>(channel)][
                                static_cast<std::size_t>(sample)]);
                        const auto last = static_cast<double>(
                            transportOutput.storage[
                                static_cast<std::size_t>(channel)][
                                static_cast<std::size_t>(
                                    transportBlockSize
                                    - transportFadeProbeSamples + sample)]);
                        midiFadeInStartEnergy += first * first;
                        midiFadeInEndEnergy += last * last;
                    }
            for (int channel = EcosystemEngine::ambientLeftBus;
                 channel <= EcosystemEngine::ambientRightBus; ++channel)
            {
                const auto previousIndex = static_cast<std::size_t>(
                    channel - EcosystemEngine::ambientLeftBus);
                auto previous = previousTransportSamples[previousIndex];
                for (const auto sample : transportOutput.storage[
                         static_cast<std::size_t>(channel)])
                {
                    transportMaximumStep = std::max(
                        transportMaximumStep, std::abs(sample - previous));
                    previous = sample;
                }
                previousTransportSamples[previousIndex] = previous;
            }
        }
        passed &= expect(pausedBassPeak > 0.0001f
                             && pausedLayerLivePeak > 0.0001f
                             && resumedCrossingNotePeak > 0.0001f
                             && transportEngine.getPhase(1)
                                    != phaseBeforeResume
                             && std::abs(transportEngine.getPhase(2)
                                    - phaseTwoBeforeAllPause) < 0.000000001
                             && std::abs(transportEngine.getPhase(3)
                                    - phaseThreeBeforeAllPause) < 0.000000001
                             && midiFadeInEndEnergy
                                    > midiFadeInStartEnergy * 1.5
                             && transportFinite && transportPeak < 0.90f
                             && transportMaximumStep < 0.20f,
                         "PLAY deve ricostruire e sfumare le note attraversate senza fermare il basso o creare click");

        // SEMINA on a paused MIDI memory deliberately returns it to PLAY and
        // replaces the old loop. DIMENTICA also normalises transport state so
        // an empty card can never remain marked IN PAUSA.
        transportEngine.setLoopPlaying(1, false);
        renderTransport();
        auto midiFadeOutStartEnergy = 0.0;
        auto midiFadeOutEndEnergy = 0.0;
        for (int channel = EcosystemEngine::ambientLeftBus;
             channel <= EcosystemEngine::ambientRightBus; ++channel)
            for (int sample = 0; sample < transportFadeProbeSamples; ++sample)
            {
                const auto first = static_cast<double>(
                    transportOutput.storage[static_cast<std::size_t>(channel)][
                        static_cast<std::size_t>(sample)]);
                const auto last = static_cast<double>(
                    transportOutput.storage[static_cast<std::size_t>(channel)][
                        static_cast<std::size_t>(
                            transportBlockSize
                            - transportFadeProbeSamples + sample)]);
                midiFadeOutStartEnergy += first * first;
                midiFadeOutEndEnergy += last * last;
            }
        transportEngine.toggleRecording(1);
        const auto midiRecordForcedPlay = transportEngine.isLoopPlaying(1);
        transportEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, 67, 0.75f));
        for (int block = 0; block < 8; ++block)
        {
            if (block == 5)
                transportEngine.enqueueMidiMessage(
                    juce::MidiMessage::noteOff(2, 67));
            renderTransport();
        }
        transportEngine.toggleRecording(1);
        renderTransport();
        const auto midiRecordedFromPause = transportEngine.hasMaterial(1)
            && transportEngine.getEventCount(1) == 2
            && transportEngine.isLoopPlaying(1);
        transportEngine.setLoopPlaying(1, false);
        transportEngine.clearMemory(1);
        renderTransport();
        // Factory reverbs deliberately outlive the MIDI note.  Twelve
        // simulated seconds distinguish a decaying tail from a genuinely
        // stuck voice without requiring the clear command to hard-reset DSP.
        for (int block = 0; block < 240; ++block)
            renderTransport();
        const auto midiClearRestoredPlay = transportEngine.isLoopPlaying(1)
            && ! transportEngine.hasMaterial(1);
        const auto midiClearDrained
            = transportOutput.peak(EcosystemEngine::ambientLeftBus,
                                   transportBlockSize) < 0.0001f
            && transportOutput.peak(EcosystemEngine::ambientRightBus,
                                    transportBlockSize) < 0.0001f;
        passed &= expect(midiFadeOutStartEnergy
                                    > midiFadeOutEndEnergy * 1.5,
                         "PAUSA MIDI deve usare un fade-out progressivo");
        passed &= expect(midiRecordForcedPlay && midiRecordedFromPause,
                         "SEMINA MIDI deve uscire da PAUSA e riscrivere il loop");
        passed &= expect(midiClearRestoredPlay,
                         "DIMENTICA MIDI deve uscire da PAUSA e cancellare il materiale");
        passed &= expect(midiClearDrained,
                         "DIMENTICA MIDI non deve lasciare note bloccate");

        // A device re-prepare/reconnect never persists performance pauses.
        // Same-rate prepare preserves the already recorded material, whereas
        // both lifecycle boundaries restore every transport to PLAY.
        transportEngine.setLoopPlaying(2, false);
        transportEngine.setLoopPlaying(3, false);
        transportEngine.prepare(transportSampleRate, transportBlockSize);
        const auto prepareRestoredPlay = transportEngine.isLoopPlaying(2)
            && transportEngine.isLoopPlaying(3)
            && transportEngine.hasMaterial(2)
            && transportEngine.hasMaterial(3);
        transportEngine.setLoopPlaying(2, false);
        transportEngine.setLoopPlaying(3, false);
        transportEngine.audioDeviceStopped();
        const auto stopRestoredPlay = transportEngine.isLoopPlaying(2)
            && transportEngine.isLoopPlaying(3)
            && transportEngine.hasMaterial(2)
            && transportEngine.hasMaterial(3);
        passed &= expect(prepareRestoredPlay && stopRestoredPlay,
                         "prepare e stop audio devono ripartire in PLAY conservando i loop chiusi");
    }

    // DIRADA cannot own a paused memory. With only one eligible MIDI loop,
    // pausing it while RESPIRA is active must release ownership immediately;
    // the scheduler must then leave it excluded for subsequent cycles.
    {
        constexpr auto pauseThinningSampleRate = 8000.0;
        constexpr auto pauseThinningBlockSize = 400;
        constexpr auto pauseThinningMemory = 1;
        auto pauseThinningEngineStorage
            = std::make_unique<EcosystemEngine>();
        auto& pauseThinningEngine = *pauseThinningEngineStorage;
        pauseThinningEngine.setScenarioIndex(1);
        pauseThinningEngine.setDelayLevel(pauseThinningMemory, 0.0f);
        pauseThinningEngine.prepare(pauseThinningSampleRate,
                                    pauseThinningBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount>
            pauseThinningOutput(pauseThinningBlockSize);
        const auto renderPauseThinning = [&]
        {
            pauseThinningOutput.clear();
            process(pauseThinningEngine, nullptr, 0,
                    pauseThinningOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    pauseThinningBlockSize);
        };
        pauseThinningEngine.toggleRecording(pauseThinningMemory);
        pauseThinningEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, 52, 0.78f));
        for (int block = 0; block < 11; ++block)
        {
            if (block == 8)
                pauseThinningEngine.enqueueMidiMessage(
                    juce::MidiMessage::noteOff(2, 52));
            renderPauseThinning();
        }
        pauseThinningEngine.toggleRecording(pauseThinningMemory);
        renderPauseThinning();
        pauseThinningEngine.setThinningEnabled(true);
        auto thinningOwnedBeforePause = false;
        for (int block = 0; block < 220; ++block)
        {
            renderPauseThinning();
            if (pauseThinningEngine.getThinnedMemoryIndex()
                == pauseThinningMemory)
            {
                thinningOwnedBeforePause = true;
                break;
            }
        }
        pauseThinningEngine.setLoopPlaying(pauseThinningMemory, false);
        renderPauseThinning();
        auto pausedMemoryStayedExcluded
            = pauseThinningEngine.getThinnedMemoryIndex() == -1;
        for (int block = 0; block < 320; ++block)
        {
            renderPauseThinning();
            pausedMemoryStayedExcluded
                &= pauseThinningEngine.getThinnedMemoryIndex() == -1;
        }
        passed &= expect(thinningOwnedBeforePause
                             && pauseThinningEngine.isThinningEnabled()
                             && ! pauseThinningEngine.isLoopPlaying(
                                 pauseThinningMemory)
                             && pausedMemoryStayedExcluded
                             && pauseThinningEngine.hasMaterial(
                                 pauseThinningMemory),
                         "PAUSA deve annullare DIRADA e tenere la memoria fuori dallo scheduler");
    }

    // RESPIRO transport gates only stored playback. Its physical sax monitor
    // remains continuous, and the loop/four COSMOS heads resume from the exact
    // frozen phase through a short anti-click ramp.
    {
        constexpr auto saxTransportSampleRate = 8000.0;
        constexpr auto saxTransportBlockSize = 400;
        constexpr auto saxCaptureBlocks = 12;
        constexpr auto saxMemory = EcosystemEngine::midiMemoryCount;
        auto saxTransportEngineStorage = std::make_unique<EcosystemEngine>();
        auto& saxTransportEngine = *saxTransportEngineStorage;
        saxTransportEngine.setScenarioIndex(CommentoScenarios::count - 1);
        saxTransportEngine.setSaxPathMode(
            EcosystemEngine::SaxPathMode::cleanLooper);
        saxTransportEngine.setSaxStereoInput(true);
        saxTransportEngine.prepare(saxTransportSampleRate,
                                   saxTransportBlockSize);
        std::array<std::vector<float>, 2> saxTransportInputStorage;
        std::array<const float*, 2> saxTransportInputs {};
        for (std::size_t channel = 0;
             channel < saxTransportInputStorage.size(); ++channel)
        {
            saxTransportInputStorage[channel].resize(saxTransportBlockSize);
            for (int sample = 0; sample < saxTransportBlockSize; ++sample)
                saxTransportInputStorage[channel][static_cast<std::size_t>(sample)]
                    = 0.055f * static_cast<float>(std::sin(
                        juce::MathConstants<double>::twoPi
                        * (170.0 + 23.0 * static_cast<double>(channel))
                        * static_cast<double>(sample) / saxTransportSampleRate));
            saxTransportInputs[channel]
                = saxTransportInputStorage[channel].data();
        }
        OutputBlock<EcosystemEngine::logicalOutputBusCount> saxTransportOutput(
            saxTransportBlockSize);
        const auto renderSaxTransport = [&](const float* const* inputs)
        {
            saxTransportOutput.clear();
            process(saxTransportEngine, inputs, inputs != nullptr ? 2 : 0,
                    saxTransportOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount,
                    saxTransportBlockSize);
        };

        saxTransportEngine.toggleRecording(saxMemory);
        for (int block = 0; block < saxCaptureBlocks; ++block)
            renderSaxTransport(saxTransportInputs.data());
        saxTransportEngine.toggleRecording(saxMemory);
        for (int block = 0; block < 6; ++block)
            renderSaxTransport(nullptr);
        const auto saxEventsBeforePause = saxTransportEngine.getEventCount(
            saxMemory);
        const auto saxLengthBeforePause = saxTransportEngine.getLengthSeconds(
            saxMemory);
        const auto saxPhaseBeforePause = saxTransportEngine.getPhase(saxMemory);
        std::array<float, 2> previousSaxSamples {
            saxTransportOutput.storage[EcosystemEngine::saxLeftBus].back(),
            saxTransportOutput.storage[EcosystemEngine::saxRightBus].back()
        };
        saxTransportEngine.setLoopPlaying(saxMemory, false);

        // Probe the stored component without live input first: its first
        // window must retain energy and its final window must be lower. This
        // distinguishes a musical fade from a block-boundary hard mute.
        renderSaxTransport(nullptr);
        constexpr auto saxFadeProbeSamples = 64;
        auto saxFadeOutStartEnergy = 0.0;
        auto saxFadeOutEndEnergy = 0.0;
        auto saxTransportMaximumStep = 0.0f;
        for (int channel = EcosystemEngine::saxLeftBus;
             channel <= EcosystemEngine::saxRightBus; ++channel)
        {
            const auto previousIndex = static_cast<std::size_t>(
                channel - EcosystemEngine::saxLeftBus);
            auto previous = previousSaxSamples[previousIndex];
            for (const auto sample : saxTransportOutput.storage[
                     static_cast<std::size_t>(channel)])
            {
                saxTransportMaximumStep = std::max(
                    saxTransportMaximumStep, std::abs(sample - previous));
                previous = sample;
            }
            previousSaxSamples[previousIndex] = previous;
            for (int sample = 0; sample < saxFadeProbeSamples; ++sample)
            {
                const auto first = static_cast<double>(
                    saxTransportOutput.storage[
                        static_cast<std::size_t>(channel)][
                        static_cast<std::size_t>(sample)]);
                const auto last = static_cast<double>(
                    saxTransportOutput.storage[
                        static_cast<std::size_t>(channel)][
                        static_cast<std::size_t>(
                            saxTransportBlockSize
                            - saxFadeProbeSamples + sample)]);
                saxFadeOutStartEnergy += first * first;
                saxFadeOutEndEnergy += last * last;
            }
        }

        std::array<std::vector<float>, 2> liveSaxInputStorage;
        std::array<const float*, 2> liveSaxInputs {};
        constexpr std::array<float, 2> liveSaxValues { 0.031f, -0.027f };
        for (std::size_t channel = 0; channel < liveSaxInputStorage.size();
             ++channel)
        {
            liveSaxInputStorage[channel].assign(saxTransportBlockSize,
                                                liveSaxValues[channel]);
            liveSaxInputs[channel] = liveSaxInputStorage[channel].data();
        }
        auto saxTransportFinite = true;
        auto saxTransportPeak = 0.0f;
        for (int block = 0; block < 12; ++block)
        {
            renderSaxTransport(liveSaxInputs.data());
            saxTransportFinite &= saxTransportOutput.finite(
                saxTransportBlockSize);
            for (int channel = EcosystemEngine::saxLeftBus;
                 channel <= EcosystemEngine::saxRightBus; ++channel)
            {
                const auto previousIndex = static_cast<std::size_t>(
                    channel - EcosystemEngine::saxLeftBus);
                auto previous = previousSaxSamples[previousIndex];
                for (const auto sample : saxTransportOutput.storage[
                         static_cast<std::size_t>(channel)])
                {
                    saxTransportPeak = std::max(saxTransportPeak,
                                                std::abs(sample));
                    saxTransportMaximumStep = std::max(
                        saxTransportMaximumStep,
                        std::abs(sample - previous));
                    previous = sample;
                }
                previousSaxSamples[previousIndex] = previous;
            }
        }
        const auto pausedSaxPhase = saxTransportEngine.getPhase(saxMemory);
        const auto liveMonitorPreserved
            = std::abs(saxTransportOutput.storage[
                    EcosystemEngine::saxLeftBus].back()
                - liveSaxValues[0] * 0.58f) < 0.00001f
            && std::abs(saxTransportOutput.storage[
                    EcosystemEngine::saxRightBus].back()
                - liveSaxValues[1] * 0.58f) < 0.00001f;
        renderSaxTransport(nullptr);
        const auto cosmosHeadsAreSilentWhilePaused
            = saxTransportOutput.silent(EcosystemEngine::saxLeftBus,
                                        saxTransportBlockSize)
            && saxTransportOutput.silent(EcosystemEngine::saxRightBus,
                                         saxTransportBlockSize);
        passed &= expect(! saxTransportEngine.isLoopPlaying(saxMemory)
                             && std::abs(pausedSaxPhase - saxPhaseBeforePause)
                                    < 0.000000001
                             && saxTransportEngine.hasMaterial(saxMemory)
                             && saxTransportEngine.getEventCount(saxMemory)
                                    == saxEventsBeforePause
                             && std::abs(saxTransportEngine.getLengthSeconds(
                                    saxMemory) - saxLengthBeforePause)
                                    < 0.000001
                             && saxFadeOutStartEnergy
                                    > saxFadeOutEndEnergy * 1.5
                             && liveMonitorPreserved
                             && cosmosHeadsAreSilentWhilePaused,
                         "PAUSA RESPIRO deve sfumare e congelare loop/testine COSMOS lasciando vivo il sax");

        saxTransportEngine.setLoopPlaying(saxMemory, true);
        auto resumedSaxPeak = 0.0f;
        auto saxFadeInStartEnergy = 0.0;
        auto saxFadeInEndEnergy = 0.0;
        for (int block = 0; block < 6; ++block)
        {
            renderSaxTransport(nullptr);
            saxTransportFinite &= saxTransportOutput.finite(
                saxTransportBlockSize);
            resumedSaxPeak = std::max(resumedSaxPeak,
                std::max(saxTransportOutput.peak(
                             EcosystemEngine::saxLeftBus,
                             saxTransportBlockSize),
                         saxTransportOutput.peak(
                             EcosystemEngine::saxRightBus,
                             saxTransportBlockSize)));
            for (int channel = EcosystemEngine::saxLeftBus;
                 channel <= EcosystemEngine::saxRightBus; ++channel)
            {
                const auto previousIndex = static_cast<std::size_t>(
                    channel - EcosystemEngine::saxLeftBus);
                auto previous = previousSaxSamples[previousIndex];
                for (const auto sample : saxTransportOutput.storage[
                         static_cast<std::size_t>(channel)])
                {
                    saxTransportMaximumStep = std::max(
                        saxTransportMaximumStep,
                        std::abs(sample - previous));
                    previous = sample;
                }
                previousSaxSamples[previousIndex] = previous;
                if (block == 0)
                    for (int sample = 0; sample < saxFadeProbeSamples;
                         ++sample)
                    {
                        const auto first = static_cast<double>(
                            saxTransportOutput.storage[
                                static_cast<std::size_t>(channel)][
                                static_cast<std::size_t>(sample)]);
                        const auto last = static_cast<double>(
                            saxTransportOutput.storage[
                                static_cast<std::size_t>(channel)][
                                static_cast<std::size_t>(
                                    saxTransportBlockSize
                                    - saxFadeProbeSamples + sample)]);
                        saxFadeInStartEnergy += first * first;
                        saxFadeInEndEnergy += last * last;
                    }
            }
        }
        const auto saxResumeAdvanced = std::abs(
            saxTransportEngine.getPhase(saxMemory) - pausedSaxPhase)
                > 0.000001;

        // NUTRI/overdub is never allowed to run against a frozen playhead.
        // DIMENTICA then dissolves the loop and restores PLAY for the empty
        // card; the live monitor remains outside both transport gains.
        saxTransportEngine.setLoopPlaying(saxMemory, false);
        saxTransportEngine.toggleRecording(saxMemory);
        const auto saxOverdubForcedPlay
            = saxTransportEngine.isLoopPlaying(saxMemory);
        renderSaxTransport(liveSaxInputs.data());
        const auto saxOverdubStarted = saxTransportEngine.isRecording(
            saxMemory);
        saxTransportEngine.toggleRecording(saxMemory);
        renderSaxTransport(nullptr);
        saxTransportEngine.setLoopPlaying(saxMemory, false);
        renderSaxTransport(nullptr);
        renderSaxTransport(nullptr);
        saxTransportEngine.clearMemory(saxMemory);
        const auto saxStayedPausedForClear
            = ! saxTransportEngine.isLoopPlaying(saxMemory);
        renderSaxTransport(nullptr);
        const auto pausedClearDidNotReappear
            = saxTransportOutput.silent(EcosystemEngine::saxLeftBus,
                                        saxTransportBlockSize)
            && saxTransportOutput.silent(EcosystemEngine::saxRightBus,
                                         saxTransportBlockSize);
        for (int block = 0; block < 24; ++block)
            renderSaxTransport(liveSaxInputs.data());
        const auto saxClearKeptLiveInput
            = ! saxTransportEngine.hasMaterial(saxMemory)
            && saxTransportEngine.isLoopPlaying(saxMemory)
            && std::abs(saxTransportOutput.storage[
                    EcosystemEngine::saxLeftBus].back()
                - liveSaxValues[0] * 0.58f) < 0.00001f;
        passed &= expect(resumedSaxPeak > 0.0001f
                             && resumedSaxPeak < 0.90f && saxResumeAdvanced
                             && saxFadeInEndEnergy
                                    > saxFadeInStartEnergy * 1.5
                             && saxOverdubForcedPlay && saxOverdubStarted
                             && saxStayedPausedForClear
                             && pausedClearDidNotReappear
                             && saxClearKeptLiveInput
                             && saxTransportFinite && saxTransportPeak < 0.90f
                             && saxTransportMaximumStep < 0.20f,
                         "PLAY/NUTRI/DIMENTICA RESPIRO devono restare fluidi, finiti e senza mutare il live input");
    }

    // The NM2 is a dedicated, source-aware momentary surface.  Its factory
    // grid is notes 60..77 on channel 1: those messages must never leak into
    // the musical MIDI path, while the same notes from a keyboard remain
    // ordinary performance data.
    {
        using MidiRole = EcosystemEngine::MidiInputRole;
        using Nm2Gesture = EcosystemEngine::Nm2Gesture;
        constexpr auto nm2SampleRate = 8000.0;
        constexpr auto nm2BlockSize = 128;
        constexpr auto nm2Channel = EcosystemEngine::nm2MidiChannel;
        constexpr auto nm2BaseNote = EcosystemEngine::nm2BaseNote;
        constexpr auto nm2Count = EcosystemEngine::nm2GestureCount;
        static_assert(nm2Count == 18);

        auto nm2EngineStorage = std::make_unique<EcosystemEngine>();
        auto& nm2Engine = *nm2EngineStorage;
        nm2Engine.prepare(nm2SampleRate, nm2BlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> nm2Output(
            nm2BlockSize);
        const auto renderNm2 = [&]()
        {
            nm2Output.clear();
            process(nm2Engine, nullptr, 0, nm2Output.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, nm2BlockSize);
        };

        nm2Engine.setGestureTarget(2);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(nm2Channel, nm2BaseNote, 0.8f),
            MidiRole::generic);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(nm2Channel, nm2BaseNote, 0.8f),
            MidiRole::keyStep);
        const auto keyboardSourcesStayedMusical
            = nm2Engine.getNm2HeldMask() == 0u
            && ! nm2Engine.isFreeTailEnabled(2);
        renderNm2();
        passed &= expect(keyboardSourcesStayedMusical,
                         "note 60 da Generic/KeyStep non devono fingersi NM2");

        // Wrong-channel grid notes and notes just outside the factory range
        // are consumed as NM2 traffic, without arming a gesture or becoming
        // the first event of an already armed MIDI memory.
        nm2Engine.toggleRecording(1);
        renderNm2();
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, nm2BaseNote, 0.9f), MidiRole::nm2);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, nm2BaseNote - 1, 0.9f),
            MidiRole::nm2);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(2, nm2BaseNote + nm2Count, 0.9f),
            MidiRole::nm2);
        renderNm2();
        passed &= expect(nm2Engine.getNm2HeldMask() == 0u
                             && nm2Engine.isWaitingForFirstNote(1)
                             && nm2Engine.getEventCount(1) == 0
                             && nm2Engine.getLengthSeconds(1) == 0.0,
                         "canale errato e note NM2 59/78 devono essere consumati senza gesti o eventi");
        nm2Engine.toggleRecording(1);
        renderNm2();

        // Every physical key owns exactly one bit. Repeated note-ons are
        // idempotent, and both a real note-off and note-on velocity zero are
        // valid releases (the latter is common over Bluetooth MIDI).
        auto exactNm2Bits = true;
        auto duplicatePressesAreIdempotent = true;
        auto everyReleaseWorks = true;
        for (int index = 0; index < nm2Count; ++index)
        {
            const auto note = nm2BaseNote + index;
            const auto expectedBit = std::uint32_t { 1u }
                << static_cast<unsigned int>(index);
            nm2Engine.releaseNm2Gestures();
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(nm2Channel, note, 0.75f),
                MidiRole::nm2);
            exactNm2Bits &= nm2Engine.getNm2HeldMask() == expectedBit;
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(nm2Channel, note, 0.75f),
                MidiRole::nm2);
            duplicatePressesAreIdempotent
                &= nm2Engine.getNm2HeldMask() == expectedBit;
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOff(nm2Channel, note), MidiRole::nm2);
            everyReleaseWorks &= nm2Engine.getNm2HeldMask() == 0u;

            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(nm2Channel, note, 0.75f),
                MidiRole::nm2);
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(
                    nm2Channel, note, static_cast<juce::uint8>(0)),
                MidiRole::nm2);
            everyReleaseWorks &= nm2Engine.getNm2HeldMask() == 0u;
        }
        passed &= expect(exactNm2Bits && duplicatePressesAreIdempotent
                             && everyReleaseWorks,
                         "i 18 tasti NM2 devono avere bit esatti, press idempotente e release NoteOff/vel0");

        // The wearable controller is sax-first: spatial pads always own
        // RESPIRO, regardless of the card currently selected on screen.
        constexpr auto nm2SaxMemory = EcosystemEngine::midiMemoryCount;
        const auto checkSaxTarget = [&](int note, auto isEnabled)
        {
            nm2Engine.releaseNm2Gestures();
            nm2Engine.setGestureTarget(EcosystemEngine::bassLayerIndex);
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(nm2Channel, note, 1.0f),
                MidiRole::nm2);
            nm2Engine.setGestureTarget(3);
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(nm2Channel, note, 1.0f),
                MidiRole::nm2);
            auto onlySaxWasTargeted = isEnabled(nm2SaxMemory);
            for (int memory = 0; memory < nm2SaxMemory; ++memory)
                onlySaxWasTargeted &= ! isEnabled(memory);
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOff(nm2Channel, note), MidiRole::nm2);
            for (int memory = 0; memory < EcosystemEngine::memoryCount;
                 ++memory)
                onlySaxWasTargeted &= ! isEnabled(memory);
            return onlySaxWasTargeted;
        };
        const auto codaTargetsSax = checkSaxTarget(
            nm2BaseNote + static_cast<int>(Nm2Gesture::codaLibera),
            [&](int memory) { return nm2Engine.isFreeTailEnabled(memory); });
        const auto geloTargetsSax = checkSaxTarget(
            nm2BaseNote + static_cast<int>(Nm2Gesture::gelo),
            [&](int memory) { return nm2Engine.isFreezeEnabled(memory); });
        const auto echoTargetsSax = checkSaxTarget(
            nm2BaseNote + static_cast<int>(Nm2Gesture::ecoThrow),
            [&](int memory) { return nm2Engine.isEchoThrowEnabled(memory); });
        passed &= expect(codaTargetsSax && geloTargetsSax && echoTargetsSax,
                         "CODA, GELO ed ECO NM2 devono restare sul bus RESPIRO/SAX");

        // PAUSA likewise owns only the RESPIRO loop and never gates the live
        // sax. Seed a short audio take; a missing take intentionally ignores
        // the pad.
        std::vector<float> nm2SaxTake(
            static_cast<std::size_t>(nm2BlockSize), 0.04f);
        const float* nm2SaxInput = nm2SaxTake.data();
        nm2Engine.toggleRecording(nm2SaxMemory);
        for (int block = 0; block < 4; ++block)
        {
            nm2Output.clear();
            process(nm2Engine, &nm2SaxInput, 1, nm2Output.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, nm2BlockSize);
        }
        nm2Engine.toggleRecording(nm2SaxMemory);
        renderNm2();
        const auto pauseLoopWasSeeded = nm2Engine.hasMaterial(nm2SaxMemory)
            && nm2Engine.getLengthSeconds(nm2SaxMemory) > 0.0;
        nm2Engine.releaseNm2Gestures();
        nm2Engine.setGestureTarget(EcosystemEngine::bassLayerIndex);
        const auto pauseNote
            = nm2BaseNote + static_cast<int>(Nm2Gesture::pausa);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(nm2Channel, pauseNote, 1.0f),
            MidiRole::nm2);
        nm2Engine.setGestureTarget(2);
        const auto onlySaxLoopPaused
            = nm2Engine.isLoopPlaying(1)
            && nm2Engine.isLoopPlaying(2)
            && nm2Engine.isLoopPlaying(3)
            && ! nm2Engine.isLoopPlaying(nm2SaxMemory);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(nm2Channel, pauseNote), MidiRole::nm2);
        const auto pauseReleasedToPreviousPlay
            = nm2Engine.isLoopPlaying(nm2SaxMemory);
        nm2Engine.setGestureTarget(1);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(nm2Channel, pauseNote, 1.0f),
            MidiRole::nm2);
        nm2Engine.setLoopPlaying(nm2SaxMemory, false);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(nm2Channel, pauseNote), MidiRole::nm2);
        const auto pauseDidNotOverrideNewUiIntent
            = ! nm2Engine.isLoopPlaying(nm2SaxMemory);
        nm2Engine.setLoopPlaying(nm2SaxMemory, true);
        // A lost Note Off must never be able to strand RESPIRO. Simulate the
        // pad staying latched and check the screen can still start the loop:
        // without this the button kept writing PLAY while isLoopPlaying()
        // answered false, and the loop was unrecoverable without a restart.
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(nm2Channel, pauseNote, 1.0f),
            MidiRole::nm2);
        const auto stuckPadPausedRespiro
            = ! nm2Engine.isLoopPlaying(nm2SaxMemory);
        nm2Engine.setLoopPlaying(nm2SaxMemory, true);
        const auto uiRecoveredFromStuckPad
            = nm2Engine.isLoopPlaying(nm2SaxMemory);
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(nm2Channel, pauseNote), MidiRole::nm2);
        renderNm2();
        passed &= expect(stuckPadPausedRespiro && uiRecoveredFromStuckPad
                             && nm2Engine.isLoopPlaying(nm2SaxMemory),
                         "PLAY dallo schermo deve riprendere RESPIRO anche con PAUSA incastrata");

        passed &= expect(pauseLoopWasSeeded && onlySaxLoopPaused
                             && pauseReleasedToPreviousPlay
                             && pauseDidNotOverrideNewUiIntent
                             && nm2Engine.isLoopPlaying(1)
                             && nm2Engine.isLoopPlaying(nm2SaxMemory),
                         "PAUSA NM2 deve essere un overlay del solo loop RESPIRO senza sovrascrivere la UI");

        // The complete factory grid remains outside the recorder FIFO. An
        // armed memory must still wait at sample zero after every press and
        // release, with no synthetic controller/note events added.
        nm2Engine.releaseNm2Gestures();
        nm2Engine.setGestureTarget(1);
        nm2Engine.toggleRecording(1);
        renderNm2();
        for (int index = 0; index < nm2Count; ++index)
        {
            const auto note = nm2BaseNote + index;
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(nm2Channel, note, 0.8f),
                MidiRole::nm2);
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::noteOff(nm2Channel, note), MidiRole::nm2);
        }
        renderNm2();
        passed &= expect(nm2Engine.getNm2HeldMask() == 0u
                             && nm2Engine.isWaitingForFirstNote(1)
                             && nm2Engine.getEventCount(1) == 0
                             && nm2Engine.getLengthSeconds(1) == 0.0
                             && ! nm2Engine.hasMaterial(1),
                         "i 18 gesti NM2 non devono avviare o popolare una memoria MIDI armata");
        nm2Engine.toggleRecording(1);
        renderNm2();

        // The knobs stay ignored and the tilt now only feeds gesture depth.
        // Neither may reach the global CC80..84 gestures, win sax-foot-switch
        // Learn, or enter a recorder.
        nm2Engine.releaseMomentaryGestures();
        nm2Engine.setGestureTarget(2);
        nm2Engine.setSaxListenAmount(0.0f);
        nm2Engine.setThinningEnabled(false);
        nm2Engine.beginSaxFootswitchLearn();
        for (const auto controller : { 1, 74, 80, 81, 82, 83, 84 })
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::controllerEvent(1, controller, 127),
                MidiRole::nm2);
        const auto nm2ContinuousControlsIgnored
            = nm2Engine.getNm2HeldMask() == 0u
            && ! nm2Engine.isFreezeEnabled(2)
            && ! nm2Engine.isEchoThrowEnabled(2)
            && ! nm2Engine.isFreeTailEnabled(2)
            && nm2Engine.getSaxListenAmount() == 0.0f
            && ! nm2Engine.isThinningEnabled()
            && nm2Engine.isSaxFootswitchLearning()
            && ! nm2Engine.hasSaxFootswitchBinding()
            && ! nm2Engine.isRecording(EcosystemEngine::midiMemoryCount);
        nm2Engine.cancelSaxFootswitchLearn();
        passed &= expect(nm2ContinuousControlsIgnored,
                         "knob/tilt NM2 non devono pilotare gesti globali o footswitch");

        // Both MIDI panic conventions and the public disconnect hook release
        // held target owners as well as the visible 18-bit state.
        const auto pressPanicSet = [&]()
        {
            nm2Engine.setGestureTarget(1);
            for (const auto gesture : { Nm2Gesture::codaLibera,
                                        Nm2Gesture::gelo,
                                        Nm2Gesture::ecoThrow,
                                        Nm2Gesture::pausa })
                nm2Engine.enqueueMidiMessage(
                    juce::MidiMessage::noteOn(
                        nm2Channel,
                        nm2BaseNote + static_cast<int>(gesture), 1.0f),
                    MidiRole::nm2);
        };
        const auto panicReleasedEverything = [&]()
        {
            return nm2Engine.getNm2HeldMask() == 0u
                && ! nm2Engine.isFreeTailEnabled(nm2SaxMemory)
                && ! nm2Engine.isFreezeEnabled(nm2SaxMemory)
                && ! nm2Engine.isEchoThrowEnabled(nm2SaxMemory)
                && nm2Engine.isLoopPlaying(nm2SaxMemory);
        };
        pressPanicSet();
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(nm2Channel, 120, 0),
            MidiRole::nm2);
        const auto cc120Released = panicReleasedEverything();
        pressPanicSet();
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(nm2Channel, 123, 0),
            MidiRole::nm2);
        const auto cc123Released = panicReleasedEverything();
        pressPanicSet();
        nm2Engine.releaseNm2Gestures();
        const auto disconnectReleased = panicReleasedEverything();
        passed &= expect(cc120Released && cc123Released
                             && disconnectReleased,
                         "CC120, CC123 e disconnect devono liberare tutti i gesti NM2");

        std::set<std::string> nm2Names;
        auto everyNm2NameIsVisible = true;
        for (int index = 0; index < nm2Count; ++index)
        {
            const auto* name = EcosystemEngine::getNm2GestureName(
                static_cast<Nm2Gesture>(index));
            everyNm2NameIsVisible &= name != nullptr && name[0] != '\0';
            if (name != nullptr)
                nm2Names.emplace(name);
        }
        passed &= expect(everyNm2NameIsVisible
                             && nm2Names.size()
                                    == static_cast<std::size_t>(nm2Count),
                         "i 18 gesti NM2 devono avere nomi UI non vuoti e distinti");

        // The player memorises where a gesture lives, not what it is called,
        // so the three rows are part of the instrument and an accidental
        // reorder of the enum has to fail here rather than on stage.
        static constexpr const char* expectedGrid[] {
            "CODA LIBERA", "ECO THROW", "GELO", "CADUTA", "SCATTO", "ABISSO",
            "OMBRA", "RADIO", "LAMA", "GRANA", "FUZZ", "FERRO",
            "PULSO", "ORBITA", "STRETTO", "VUOTO", "ASCOLTO", "PAUSA" };
        static_assert(
            static_cast<int>(std::size(expectedGrid))
                == EcosystemEngine::nm2GestureCount,
            "la griglia attesa deve coprire i 18 pad");
        auto gridMatchesRows = true;
        for (int index = 0; index < nm2Count; ++index)
            gridMatchesRows &= std::string(
                EcosystemEngine::getNm2GestureName(
                    static_cast<Nm2Gesture>(index)))
                == std::string(expectedGrid[index]);
        passed &= expect(gridMatchesRows,
                         "le note 60-77 devono seguire le righe tempo/timbro/movimento");

        // Until the motion sensor is switched on nothing may change: a
        // controller with tilt disabled has to behave exactly as it did
        // before the sensor was supported at all. This needs an engine that
        // has never seen a tilt CC, so it cannot share the one above.
        const auto ombraNote
            = nm2BaseNote + static_cast<int>(Nm2Gesture::ombra);
        auto silentSensorStorage = std::make_unique<EcosystemEngine>();
        auto& silentSensorEngine = *silentSensorStorage;
        silentSensorEngine.prepare(nm2SampleRate, nm2BlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> silentSensorOutput(
            nm2BlockSize);
        silentSensorEngine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(nm2Channel, ombraNote, 1.0f),
            MidiRole::nm2);
        for (int block = 0; block < 20; ++block)
        {
            silentSensorOutput.clear();
            process(silentSensorEngine, nullptr, 0,
                    silentSensorOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, nm2BlockSize);
        }
        const auto depthIsFullWithoutSensor
            = ! silentSensorEngine.hasNm2TiltSensor()
            && std::abs(silentSensorEngine.getNm2TiltDepth() - 1.0f) < 0.001f;

        // With the sensor alive the depth is measured from the pose held when
        // the phrase started. The pad supplies 70% at rest and movement hands
        // it the remaining travel; without sensor CC the legacy 100% behaviour
        // above remains unchanged.
        const auto sendTilt = [&](int value)
        {
            nm2Engine.enqueueMidiMessage(
                juce::MidiMessage::controllerEvent(
                    3, EcosystemEngine::nm2TiltXController, value),
                MidiRole::nm2);
        };
        sendTilt(64);
        renderNm2();
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(nm2Channel, ombraNote, 1.0f),
            MidiRole::nm2);
        for (int block = 0; block < 20; ++block)
            renderNm2();
        const auto restingDepth = nm2Engine.getNm2TiltDepth();
        sendTilt(110);
        for (int block = 0; block < 20; ++block)
            renderNm2();
        const auto movedDepth = nm2Engine.getNm2TiltDepth();
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(nm2Channel, ombraNote), MidiRole::nm2);
        renderNm2();
        passed &= expect(depthIsFullWithoutSensor
                             && nm2Engine.hasNm2TiltSensor()
                             && restingDepth > 0.68f
                             && restingDepth < 0.72f
                             && movedDepth > 0.98f
                             && movedDepth <= 1.0f,
                         "il tilt deve approfondire il gesto dal 70% al 100%");

        // A reference pose captured for one phrase must not leak into the
        // next: after the pads are released a new press starts from wherever
        // the instrument is now, which is what makes drift irrelevant.
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOn(nm2Channel, ombraNote, 1.0f),
            MidiRole::nm2);
        for (int block = 0; block < 20; ++block)
            renderNm2();
        const auto secondPhraseDepth = nm2Engine.getNm2TiltDepth();
        nm2Engine.enqueueMidiMessage(
            juce::MidiMessage::noteOff(nm2Channel, ombraNote), MidiRole::nm2);
        renderNm2();
        passed &= expect(std::abs(secondPhraseDepth - restingDepth) < 0.02f,
                         "una nuova frase deve ricatturare la posa di riferimento");
    }

    // Lightweight audio smoke probe: every gesture is exercised over live
    // ambient, bass and sax signals. The dedicated surface must stay finite
    // and below full scale, while BASSO remains sample-identical to an engine
    // following the same schedule without NM2 processing.
    {
        using MidiRole = EcosystemEngine::MidiInputRole;
        constexpr auto probeSampleRate = 8000.0;
        constexpr auto probeBlockSize = 128;
        auto activeStorage = std::make_unique<EcosystemEngine>();
        auto referenceStorage = std::make_unique<EcosystemEngine>();
        auto& active = *activeStorage;
        auto& reference = *referenceStorage;
        active.setSaxPathMode(EcosystemEngine::SaxPathMode::cleanLooper);
        reference.setSaxPathMode(EcosystemEngine::SaxPathMode::cleanLooper);
        active.prepare(probeSampleRate, probeBlockSize);
        reference.prepare(probeSampleRate, probeBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> activeOutput(
            probeBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> referenceOutput(
            probeBlockSize);
        // A reed-like harmonic series rather than a single sine. A band-pass
        // or a high-pass measured against one low sine says almost nothing:
        // the tone either survives or vanishes, and the number that comes out
        // has no relation to what the filter does to a real horn.
        std::vector<float> saxInput(static_cast<std::size_t>(probeBlockSize));
        for (int sample = 0; sample < probeBlockSize; ++sample)
        {
            auto value = 0.0;
            for (int partial = 1; partial <= 8; ++partial)
                value += std::sin(
                    juce::MathConstants<double>::twoPi * 211.0
                    * static_cast<double>(partial)
                    * static_cast<double>(sample) / probeSampleRate)
                    / static_cast<double>(partial);
            saxInput[static_cast<std::size_t>(sample)]
                = 0.09f * static_cast<float>(value);
        }
        const float* saxInputPointer = saxInput.data();
        const auto renderPair = [&]()
        {
            activeOutput.clear();
            referenceOutput.clear();
            process(active, &saxInputPointer, 1,
                    activeOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, probeBlockSize);
            process(reference, &saxInputPointer, 1,
                    referenceOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, probeBlockSize);
        };
        for (const auto message : {
                 juce::MidiMessage::noteOn(2, 48, 0.72f),
                 juce::MidiMessage::noteOn(3, 55, 0.68f),
                 juce::MidiMessage::noteOn(4, 62, 0.64f),
                 juce::MidiMessage::noteOn(5, 40, 0.80f) })
        {
            active.enqueueMidiMessage(message);
            reference.enqueueMidiMessage(message);
        }
        for (int block = 0; block < 4; ++block)
            renderPair();

        auto nm2AudioFinite = true;
        auto nm2AudioPeak = 0.0f;
        auto bassStayedBitIdentical = true;
        auto ombraKeptAmbientBitIdentical = false;
        auto ombraChangedSax = false;
        const auto saxDifferenceFromReference = [&]()
        {
            auto difference = 0.0f;
            for (const auto channel : { EcosystemEngine::saxLeftBus,
                                        EcosystemEngine::saxRightBus })
            {
                const auto index = static_cast<std::size_t>(channel);
                for (int sample = 0; sample < probeBlockSize; ++sample)
                    difference += std::abs(
                        activeOutput.storage[index][
                            static_cast<std::size_t>(sample)]
                        - referenceOutput.storage[index][
                            static_cast<std::size_t>(sample)]);
            }
            return difference;
        };

        // With no colour held the resonant section must be exactly bypassed.
        // A filter that leaks at rest would colour every phrase the player
        // never asked to colour.
        renderPair();
        const auto colourFilterIsTransparentAtRest
            = saxDifferenceFromReference() <= 0.0f;

        // How much of the sax bus a gesture actually rewrites, relative to the
        // untouched signal. A colour that scores a few percent is one the
        // player cannot hear over a live horn, which is exactly the failure
        // this suite missed the first time round.
        const auto saxReferenceEnergy = [&]()
        {
            auto energy = 0.0f;
            for (const auto channel : { EcosystemEngine::saxLeftBus,
                                        EcosystemEngine::saxRightBus })
            {
                const auto index = static_cast<std::size_t>(channel);
                for (int sample = 0; sample < probeBlockSize; ++sample)
                    energy += std::abs(
                        referenceOutput.storage[index][
                            static_cast<std::size_t>(sample)]);
            }
            return energy;
        };
        auto weakestColourStrength = 1.0f;
        std::string weakestColourName = "nessuno";

        const auto inspectPair = [&]()
        {
            nm2AudioFinite &= activeOutput.finite(probeBlockSize)
                && referenceOutput.finite(probeBlockSize);
            for (int channel = 0;
                 channel < EcosystemEngine::logicalOutputBusCount; ++channel)
                nm2AudioPeak = std::max(
                    nm2AudioPeak,
                    activeOutput.peak(static_cast<std::size_t>(channel),
                                      probeBlockSize));
            bassStayedBitIdentical &= std::equal(
                activeOutput.storage[EcosystemEngine::bassBus].begin(),
                activeOutput.storage[EcosystemEngine::bassBus].end(),
                referenceOutput.storage[EcosystemEngine::bassBus].begin());
        };
        for (int index = 0; index < EcosystemEngine::nm2GestureCount; ++index)
        {
            const auto note = EcosystemEngine::nm2BaseNote + index;
            const auto target = 1 + index % (EcosystemEngine::memoryCount - 1);
            active.setGestureTarget(target);
            reference.setGestureTarget(target);
            active.enqueueMidiMessage(
                juce::MidiMessage::noteOn(
                    EcosystemEngine::nm2MidiChannel, note, 1.0f),
                MidiRole::nm2);
            renderPair();
            inspectPair();
            if (index == static_cast<int>(
                    EcosystemEngine::Nm2Gesture::ombra))
            {
                ombraKeptAmbientBitIdentical = std::equal(
                    activeOutput.storage[EcosystemEngine::ambientLeftBus].begin(),
                    activeOutput.storage[EcosystemEngine::ambientLeftBus].end(),
                    referenceOutput.storage[EcosystemEngine::ambientLeftBus].begin())
                    && std::equal(
                        activeOutput.storage[EcosystemEngine::ambientRightBus].begin(),
                        activeOutput.storage[EcosystemEngine::ambientRightBus].end(),
                        referenceOutput.storage[EcosystemEngine::ambientRightBus].begin());
                auto saxDifference = 0.0f;
                for (const auto channel : { EcosystemEngine::saxLeftBus,
                                            EcosystemEngine::saxRightBus })
                {
                    const auto channelIndex = static_cast<std::size_t>(channel);
                    for (int sample = 0; sample < probeBlockSize; ++sample)
                        saxDifference += std::abs(
                            activeOutput.storage[channelIndex][static_cast<std::size_t>(sample)]
                            - referenceOutput.storage[channelIndex][static_cast<std::size_t>(sample)]);
                }
                ombraChangedSax = saxDifference > 0.0001f;
            }

            // STRETTO is excluded on purpose: the probe feeds a mono source,
            // where collapsing the stereo pair has almost nothing to collapse.
            const auto gesture = static_cast<EcosystemEngine::Nm2Gesture>(index);
            const auto isMeasurableColour
                = gesture == EcosystemEngine::Nm2Gesture::ombra
               || gesture == EcosystemEngine::Nm2Gesture::radio
               || gesture == EcosystemEngine::Nm2Gesture::lama
               || gesture == EcosystemEngine::Nm2Gesture::grana
               || gesture == EcosystemEngine::Nm2Gesture::fuzz
               || gesture == EcosystemEngine::Nm2Gesture::ferro
               || gesture == EcosystemEngine::Nm2Gesture::pulso
               || gesture == EcosystemEngine::Nm2Gesture::orbita
               || gesture == EcosystemEngine::Nm2Gesture::vuoto;
            if (isMeasurableColour)
            {
                // Let the attack ramp finish before measuring.
                for (int block = 0; block < 6; ++block)
                {
                    renderPair();
                    inspectPair();
                }
                const auto energy = saxReferenceEnergy();
                const auto strength = energy > 0.0001f
                    ? saxDifferenceFromReference() / energy : 0.0f;
                if (strength < weakestColourStrength)
                {
                    weakestColourStrength = strength;
                    weakestColourName
                        = EcosystemEngine::getNm2GestureName(gesture);
                }
            }
            active.enqueueMidiMessage(
                juce::MidiMessage::noteOff(
                    EcosystemEngine::nm2MidiChannel, note),
                MidiRole::nm2);
            renderPair();
            inspectPair();
        }
        passed &= expect(nm2AudioFinite && nm2AudioPeak < 0.99f,
                         "i 18 gesti NM2 devono produrre audio finito con headroom");
        passed &= expect(bassStayedBitIdentical,
                         "i gesti NM2 devono lasciare BASSO bit-identico al riferimento");
        passed &= expect(ombraKeptAmbientBitIdentical && ombraChangedSax,
                         "i colori NM2 devono elaborare SAX/RESPIRO senza tingere i loop ambiente");
        passed &= expect(colourFilterIsTransparentAtRest,
                         "senza colori premuti il filtro risonante deve essere trasparente");
        if (weakestColourStrength < 0.30f)
            std::cerr << "colore piu' debole: " << weakestColourName << " a "
                      << static_cast<int>(std::round(
                             weakestColourStrength * 100.0f))
                      << "% del bus sax\n";
        passed &= expect(weakestColourStrength >= 0.30f,
                         "ogni colore NM2 deve riscrivere almeno il 30% del bus sax");
    }

    // CADUTA and SCATTO live entirely inside the sax delay, which only runs
    // under the scenario effects path, so they need their own probe: under
    // CLEAN LOOPER the processor is bypassed and neither could speak.
    {
        using MidiRole = EcosystemEngine::MidiInputRole;
        constexpr auto tailSampleRate = 8000.0;
        constexpr auto tailBlockSize = 128;
        auto activeStorage = std::make_unique<EcosystemEngine>();
        auto referenceStorage = std::make_unique<EcosystemEngine>();
        auto& active = *activeStorage;
        auto& reference = *referenceStorage;
        active.setSaxPathMode(EcosystemEngine::SaxPathMode::sceneEffects);
        reference.setSaxPathMode(EcosystemEngine::SaxPathMode::sceneEffects);
        active.prepare(tailSampleRate, tailBlockSize);
        reference.prepare(tailSampleRate, tailBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> activeOutput(
            tailBlockSize);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> referenceOutput(
            tailBlockSize);
        std::vector<float> saxInput(static_cast<std::size_t>(tailBlockSize));
        for (int sample = 0; sample < tailBlockSize; ++sample)
            saxInput[static_cast<std::size_t>(sample)]
                = 0.18f * static_cast<float>(std::sin(
                    juce::MathConstants<double>::twoPi * 197.0
                    * static_cast<double>(sample) / tailSampleRate));
        const float* saxInputPointer = saxInput.data();
        const auto renderTailPair = [&]()
        {
            activeOutput.clear();
            referenceOutput.clear();
            process(active, &saxInputPointer, 1, activeOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, tailBlockSize);
            process(reference, &saxInputPointer, 1,
                    referenceOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, tailBlockSize);
        };
        const auto tailDifference = [&]()
        {
            auto difference = 0.0f;
            for (const auto channel : { EcosystemEngine::saxLeftBus,
                                        EcosystemEngine::saxRightBus })
            {
                const auto index = static_cast<std::size_t>(channel);
                for (int sample = 0; sample < tailBlockSize; ++sample)
                    difference += std::abs(
                        activeOutput.storage[index][
                            static_cast<std::size_t>(sample)]
                        - referenceOutput.storage[index][
                            static_cast<std::size_t>(sample)]);
            }
            return difference;
        };

        auto tailAudioFinite = true;
        auto tailAudioPeak = 0.0f;
        const auto probeTailGesture = [&](EcosystemEngine::Nm2Gesture gesture)
        {
            const auto note = EcosystemEngine::nm2BaseNote
                + static_cast<int>(gesture);
            // Fill the delay first: a gesture that only reshapes the tail has
            // nothing to reshape on a line that is still empty.
            for (int block = 0; block < 40; ++block)
                renderTailPair();
            active.enqueueMidiMessage(
                juce::MidiMessage::noteOn(
                    EcosystemEngine::nm2MidiChannel, note, 1.0f),
                MidiRole::nm2);
            auto difference = 0.0f;
            for (int block = 0; block < 24; ++block)
            {
                renderTailPair();
                difference += tailDifference();
                tailAudioFinite &= activeOutput.finite(tailBlockSize);
                for (int channel = 0;
                     channel < EcosystemEngine::logicalOutputBusCount;
                     ++channel)
                    tailAudioPeak = std::max(
                        tailAudioPeak,
                        activeOutput.peak(static_cast<std::size_t>(channel),
                                          tailBlockSize));
            }
            active.enqueueMidiMessage(
                juce::MidiMessage::noteOff(
                    EcosystemEngine::nm2MidiChannel, note),
                MidiRole::nm2);
            for (int block = 0; block < 40; ++block)
                renderTailPair();
            return difference > 0.0001f;
        };
        const auto cadutaChangedSax = probeTailGesture(
            EcosystemEngine::Nm2Gesture::caduta);
        const auto scattoChangedSax = probeTailGesture(
            EcosystemEngine::Nm2Gesture::scatto);
        passed &= expect(cadutaChangedSax && scattoChangedSax,
                         "CADUTA e SCATTO devono trasformare il bus sax");
        passed &= expect(tailAudioFinite && tailAudioPeak < 0.99f,
                         "CADUTA e SCATTO devono restare finiti e sotto il fondo scala");
    }

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
