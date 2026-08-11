#include <JuceHeader.h>
#include "Engine/AmbientSynth.h"
#include "Engine/EcosystemEngine.h"
#include "Engine/Scenarios.h"
#include "Hardware/Model12AudioRouter.h"

#include <algorithm>
#include <array>
#include <cstddef>
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
        auto gesturesStartOff = defaultEngine.getSaxListenAmount() == 0.0f;
        for (int memory = 0; memory < EcosystemEngine::memoryCount; ++memory)
            gesturesStartOff &= ! defaultEngine.isFreezeEnabled(memory)
                && ! defaultEngine.isEchoThrowEnabled(memory);
        passed &= expect(gesturesStartOff,
                         "GELO, ECO THROW e ASCOLTO devono partire spenti");

        // MIDI 5 is the dedicated live bass and deliberately owns neither a
        // delay nor a reverb tail. Both the direct UI API and the global MIDI
        // gestures must therefore leave it outside GELO/ECO THROW.
        defaultEngine.setFreezeEnabled(EcosystemEngine::bassLayerIndex, true);
        defaultEngine.setEchoThrowEnabled(EcosystemEngine::bassLayerIndex,
                                          true);
        defaultEngine.setGestureTarget(EcosystemEngine::bassLayerIndex);
        defaultEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(5, 80, 127));
        defaultEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(5, 81, 127));
        passed &= expect(! defaultEngine.isFreezeEnabled(
                              EcosystemEngine::bassLayerIndex)
                             && ! defaultEngine.isEchoThrowEnabled(
                                 EcosystemEngine::bassLayerIndex),
                         "il basso MIDI 5 deve restare escluso dai gesti di coda");
    }
    passed &= expect(CommentoScenarios::wrapIndex(-1)
                             == CommentoScenarios::count - 1
                         && CommentoScenarios::wrapIndex(
                                CommentoScenarios::count) == 0,
                     "la selezione scenario deve essere circolare");

    // The three dedicated MIDI controls are global performance controls, not
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

        controlEngine.setGestureTarget(2);
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(3, 80, 127));
        controlEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(3, 81, 127));
        controlEngine.releaseMomentaryGestures();
        passed &= expect(! controlEngine.isFreezeEnabled(2)
                             && ! controlEngine.isEchoThrowEnabled(2),
                         "il panic deve liberare GELO ed ECO THROW su ogni card");

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
                             && controlEngine.getEventCount(1) == 2,
                         "CC80-82 devono essere consumati e mai registrati nel loop MIDI");

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
