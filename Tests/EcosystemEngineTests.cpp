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

double estimateEnvelopeRate(const std::vector<float>& samples,
                            double sampleRate)
{
    const auto rmsWindow = juce::jmax(
        1, static_cast<int>(std::round(sampleRate * 0.010)));
    const auto frameCount = samples.size()
        / static_cast<std::size_t>(rmsWindow);
    if (frameCount < 32)
        return 0.0;

    std::vector<double> envelope(frameCount, 0.0);
    auto mean = 0.0;
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        auto sum = 0.0;
        for (int sample = 0; sample < rmsWindow; ++sample)
        {
            const auto value = samples[
                frame * static_cast<std::size_t>(rmsWindow)
                + static_cast<std::size_t>(sample)];
            sum += static_cast<double>(value) * value;
        }
        envelope[frame] = std::sqrt(sum / static_cast<double>(rmsWindow));
        mean += envelope[frame];
    }
    mean /= static_cast<double>(frameCount);

    auto bestRate = 0.0;
    auto bestPower = 0.0;
    for (auto rate = 1.5; rate <= 5.0; rate += 0.02)
    {
        auto real = 0.0;
        auto imaginary = 0.0;
        for (std::size_t frame = 0; frame < frameCount; ++frame)
        {
            const auto hann = 0.5 - 0.5 * std::cos(
                juce::MathConstants<double>::twoPi
                * static_cast<double>(frame)
                / static_cast<double>(frameCount - 1));
            const auto time = (static_cast<double>(frame) + 0.5)
                * static_cast<double>(rmsWindow) / sampleRate;
            const auto phase = juce::MathConstants<double>::twoPi
                * rate * time;
            const auto value = (envelope[frame] - mean) * hann;
            real += value * std::cos(phase);
            imaginary -= value * std::sin(phase);
        }
        const auto power = real * real + imaginary * imaginary;
        if (power > bestPower)
        {
            bestPower = power;
            bestRate = rate;
        }
    }
    return bestRate;
}
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
    int saxLoopKeyboardScenarioCount = 0;
    int saxLoopKeyboardScenarioIndex = -1;
    int droneScenarioIndex = -1;
    int noiseScenarioIndex = -1;
    int metalScenarioIndex = -1;
    for (int index = 0; index < CommentoScenarios::count; ++index)
    {
        scenarioNames.insert(CommentoScenarios::get(index).name);
        const auto& scenario = CommentoScenarios::get(index);
        if (scenario.saxLoopKeyboard.enabled)
        {
            ++saxLoopKeyboardScenarioCount;
            saxLoopKeyboardScenarioIndex = index;
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
    passed &= expect(saxLoopKeyboardScenarioCount == 1
                         && saxLoopKeyboardScenarioIndex
                                == CommentoScenarios::count - 1
                         && std::string(cosmosScenario.name) == "COSMOS"
                         && cosmosScenario.saxLoopKeyboard.enabled,
                     "solo COSMOS, ultimo scenario, deve abilitare il sax MIDI 5");
    passed &= expect(cosmosScenario.saxLoopKeyboard.rootNote == 60
                         && cosmosScenario.saxLoopKeyboard.attackSeconds > 0.0f
                         && cosmosScenario.saxLoopKeyboard.releaseSeconds > 0.0f
                         && cosmosScenario.saxLoopKeyboard.level > 0.0f
                         && cosmosScenario.saxLoopKeyboard.level <= 1.0f
                         && cosmosScenario.saxLoopKeyboard.grainMilliseconds
                                >= 24.0f
                         && cosmosScenario.saxLoopKeyboard.grainMilliseconds
                                <= 120.0f,
                     "COSMOS deve dichiarare root e grana del pitch shifter");
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
    passed &= expect(CommentoScenarios::wrapIndex(-1)
                             == CommentoScenarios::count - 1
                         && CommentoScenarios::wrapIndex(
                                CommentoScenarios::count) == 0,
                     "la selezione scenario deve essere circolare");

    EcosystemEngine bassEngine;
    bassEngine.prepare(sampleRate, blockSize);
    OutputBlock<EcosystemEngine::logicalOutputBusCount> bassOutput(blockSize);
    bassEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(5, 48, 0.85f));
    process(bassEngine, nullptr, 0, bassOutput.pointers.data(),
            EcosystemEngine::logicalOutputBusCount, blockSize);
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
        if (CommentoScenarios::get(scenario).saxLoopKeyboard.enabled)
            continue;

        ++calibratedBassScenarioCount;
        EcosystemEngine scenarioBassEngine;
        scenarioBassEngine.prepare(sampleRate, blockSize);
        scenarioBassEngine.setScenarioIndex(scenario);
        scenarioBassEngine.setTextureAmount(1.0f);
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
    passed &= expect(calibratedBassScenarioCount
                             == CommentoScenarios::count - 1,
                     "solo COSMOS deve essere escluso dalla calibrazione basso");
    passed &= expect(quietestBassScenarioPeak >= 0.24f
                         && loudestBassScenarioPeak <= 0.32f,
                     "ogni scena basso synth deve restare espressiva con headroom");
    passed &= expect(loudestBassScenarioPeak
                         <= quietestBassScenarioPeak * 1.08f,
                     "i livelli di fabbrica del basso devono essere coerenti");

    // COSMOS replaces the MIDI-5 oscillator with a polyphonic reader of the
    // captured sax memory. A deterministic, amplitude-modulated tone proves
    // that sound comes from that memory, reaches only the dedicated MIDI-5 bus
    // and obeys
    // the configured release and clear commands.
    {
        constexpr auto cosmosCaptureSamples = 24000;
        EcosystemEngine cosmosEngine;
        cosmosEngine.setScenarioIndex(CommentoScenarios::count - 1);
        cosmosEngine.setSaxPathMode(EcosystemEngine::SaxPathMode::cleanLooper);
        cosmosEngine.setSaxStereoInput(true);
        cosmosEngine.setPerformanceLevel(EcosystemEngine::bassLayerIndex, 1.0f);
        cosmosEngine.setPerformanceLevel(EcosystemEngine::midiMemoryCount, 0.0f);
        cosmosEngine.setDelayLevel(EcosystemEngine::bassLayerIndex, 0.0f);
        cosmosEngine.prepare(sampleRate, cosmosCaptureSamples);
        OutputBlock<EcosystemEngine::logicalOutputBusCount> cosmosOutput(
            cosmosCaptureSamples);

        cosmosEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(
            5, cosmosScenario.saxLoopKeyboard.rootNote, 1.0f));
        process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        passed &= expect(cosmosEngine.isSaxLoopKeyboardEnabled()
                             && cosmosOutput.silent(
                                 EcosystemEngine::bassBus, blockSize),
                         "COSMOS senza cattura non deve ripiegare sul synth basso");
        cosmosEngine.enqueueMidiMessage(
            juce::MidiMessage::controllerEvent(5, 120, 0));
        cosmosOutput.clear();
        process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);

        std::array<std::vector<float>, 2> cosmosInputStorage;
        std::array<const float*, 2> cosmosInputs {};
        for (std::size_t channel = 0; channel < cosmosInputStorage.size();
             ++channel)
        {
            cosmosInputStorage[channel].resize(cosmosCaptureSamples);
            for (int sample = 0; sample < cosmosCaptureSamples; ++sample)
            {
                const auto envelope = 0.060f + 0.045f
                    * static_cast<float>(std::cos(
                        juce::MathConstants<double>::twoPi
                        * static_cast<double>(sample)
                        / static_cast<double>(cosmosCaptureSamples)));
                const auto tone = envelope * static_cast<float>(std::sin(
                    juce::MathConstants<double>::twoPi * 220.0
                    * static_cast<double>(sample) / sampleRate));
                cosmosInputStorage[channel][static_cast<std::size_t>(sample)] =
                    channel == 0 ? tone : tone * 0.75f;
            }
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
        passed &= expect(cosmosEngine.hasMaterial(
                             EcosystemEngine::midiMemoryCount)
                             && std::abs(cosmosEngine.getLengthSeconds(
                                 EcosystemEngine::midiMemoryCount) - 0.5)
                                    < 0.0001,
                         "COSMOS deve chiudere una cattura sax libera di 500 ms");

        // Re-preparing at the same sample rate resets voices and effect tails
        // but deliberately preserves the recorded audio memory. Count stable
        // positive crossings so the chromatic contract is tested without
        // depending on an exact phase or sample amplitude.
        const auto measureCosmosPitch = [&](int midiNote, float& outputPeak,
                                             float& meterPeak,
                                             bool& stayedFinite,
                                             bool& ambientStayedSilent,
                                             std::vector<float>& timingSamples)
        {
            cosmosEngine.prepare(sampleRate, blockSize);
            cosmosEngine.enqueueMidiMessage(
                juce::MidiMessage::noteOn(5, midiNote, 1.0f));
            outputPeak = 0.0f;
            meterPeak = 0.0f;
            stayedFinite = true;
            ambientStayedSilent = true;
            int positiveCrossings = 0;
            int measuredSamples = 0;
            float previousSample = 0.0f;
            bool hasPreviousSample = false;
            constexpr auto warmupBlocks = 12;
            constexpr auto measurementBlocks = 375;
            timingSamples.clear();
            timingSamples.reserve(
                static_cast<std::size_t>(measurementBlocks * blockSize));
            for (int block = 0;
                 block < warmupBlocks + measurementBlocks; ++block)
            {
                cosmosOutput.clear();
                process(cosmosEngine, nullptr, 0,
                        cosmosOutput.pointers.data(),
                        EcosystemEngine::logicalOutputBusCount, blockSize);
                outputPeak = std::max(outputPeak,
                    cosmosOutput.peak(EcosystemEngine::bassBus, blockSize));
                meterPeak = std::max(meterPeak,
                                     cosmosEngine.getBassOutputLevel());
                stayedFinite &= cosmosOutput.finite(blockSize);
                ambientStayedSilent &= cosmosOutput.silent(
                    EcosystemEngine::ambientLeftBus, blockSize)
                    && cosmosOutput.silent(
                        EcosystemEngine::ambientRightBus, blockSize);

                if (block < warmupBlocks)
                    continue;
                timingSamples.insert(
                    timingSamples.end(),
                    cosmosOutput.storage[EcosystemEngine::bassBus].begin(),
                    cosmosOutput.storage[EcosystemEngine::bassBus].begin()
                        + blockSize);
                for (int sampleIndex = 0; sampleIndex < blockSize;
                     ++sampleIndex)
                {
                    const auto sample = cosmosOutput.storage[
                        EcosystemEngine::bassBus][sampleIndex];
                    if (hasPreviousSample
                        && previousSample <= 0.0f && sample > 0.0f)
                        ++positiveCrossings;
                    previousSample = sample;
                    hasPreviousSample = true;
                    ++measuredSamples;
                }
            }
            cosmosEngine.enqueueMidiMessage(
                juce::MidiMessage::controllerEvent(5, 120, 0));
            cosmosOutput.clear();
            process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
            return measuredSamples > 0
                ? static_cast<double>(positiveCrossings) * sampleRate
                    / static_cast<double>(measuredSamples)
                : 0.0;
        };

        float cosmosRootPeak = 0.0f;
        float cosmosRootMeterPeak = 0.0f;
        bool cosmosRootStayedFinite = true;
        bool cosmosAmbientStayedSilent = true;
        std::vector<float> cosmosRootTimingSamples;
        const auto cosmosRootFrequency = measureCosmosPitch(
            cosmosScenario.saxLoopKeyboard.rootNote, cosmosRootPeak,
            cosmosRootMeterPeak, cosmosRootStayedFinite,
            cosmosAmbientStayedSilent, cosmosRootTimingSamples);
        float cosmosOctavePeak = 0.0f;
        float cosmosOctaveMeterPeak = 0.0f;
        bool cosmosOctaveStayedFinite = true;
        bool cosmosOctaveAmbientStayedSilent = true;
        std::vector<float> cosmosOctaveTimingSamples;
        const auto cosmosOctaveFrequency = measureCosmosPitch(
            cosmosScenario.saxLoopKeyboard.rootNote + 12, cosmosOctavePeak,
            cosmosOctaveMeterPeak, cosmosOctaveStayedFinite,
            cosmosOctaveAmbientStayedSilent, cosmosOctaveTimingSamples);
        const auto cosmosRootEnvelopeRate = estimateEnvelopeRate(
            cosmosRootTimingSamples, sampleRate);
        const auto cosmosOctaveEnvelopeRate = estimateEnvelopeRate(
            cosmosOctaveTimingSamples, sampleRate);
        passed &= expect(cosmosRootPeak > 0.0001f
                             && cosmosOctavePeak > 0.0001f
                             && cosmosRootStayedFinite
                             && cosmosOctaveStayedFinite,
                         "root e ottava COSMOS devono produrre audio finito");
        passed &= expect(cosmosRootFrequency > 190.0
                             && cosmosRootFrequency < 250.0
                             && cosmosOctaveFrequency
                                    > cosmosRootFrequency * 1.75
                             && cosmosOctaveFrequency
                                    < cosmosRootFrequency * 2.25,
                         "MIDI 5 +12 deve raddoppiare la frequenza COSMOS");
        passed &= expect(cosmosRootEnvelopeRate > 1.7
                             && cosmosRootEnvelopeRate < 2.4
                             && cosmosOctaveEnvelopeRate
                                    > cosmosRootEnvelopeRate * 0.82
                             && cosmosOctaveEnvelopeRate
                                    < cosmosRootEnvelopeRate * 1.18,
                         "+12 deve cambiare il pitch senza accelerare il loop");
        passed &= expect(cosmosAmbientStayedSilent
                             && cosmosOctaveAmbientStayedSilent,
                         "la tastiera sax COSMOS non deve contaminare AMBIENTE");

        // Eight identical starts make voice count and constant-power
        // normalisation observable: one voice would stay near 1x, while an
        // unnormalised stack would approach 8x instead of sqrt(8)x.
        cosmosEngine.prepare(sampleRate, blockSize);
        for (int voice = 0; voice < 8; ++voice)
            cosmosEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(
                5, cosmosScenario.saxLoopKeyboard.rootNote, 1.0f));
        float cosmosUnisonPeak = 0.0f;
        float cosmosUnisonMeterPeak = 0.0f;
        bool cosmosUnisonStayedFinite = true;
        for (int block = 0; block < 48; ++block)
        {
            cosmosOutput.clear();
            process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
            cosmosUnisonStayedFinite &= cosmosOutput.finite(blockSize);
            if (block < 12)
                continue;
            cosmosUnisonPeak = std::max(cosmosUnisonPeak,
                cosmosOutput.peak(EcosystemEngine::bassBus, blockSize));
            cosmosUnisonMeterPeak = std::max(cosmosUnisonMeterPeak,
                                             cosmosEngine.getBassOutputLevel());
        }
        const auto unisonToRoot = cosmosRootPeak > 0.0f
            ? cosmosUnisonPeak / cosmosRootPeak : 0.0f;
        passed &= expect(unisonToRoot > 1.4f && unisonToRoot < 4.2f
                             && cosmosUnisonStayedFinite,
                         "otto voci COSMOS devono suonare con normalizzazione");

        // Fill the eight slots with distinct notes, then submit a ninth. Once
        // the original chord is released, a stable octave proves that the new
        // note really survived voice stealing rather than the old chord tail.
        cosmosEngine.prepare(sampleRate, blockSize);
        for (int note = 0; note < 8; ++note)
            cosmosEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(
                5, cosmosScenario.saxLoopKeyboard.rootNote + note, 1.0f));
        float cosmosChordPeak = 0.0f;
        float maximumPreProtectionPeak = std::max(
            { cosmosRootMeterPeak, cosmosOctaveMeterPeak,
              cosmosUnisonMeterPeak });
        bool cosmosChordStayedFinite = true;
        for (int block = 0; block < 36; ++block)
        {
            cosmosOutput.clear();
            process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
            cosmosChordStayedFinite &= cosmosOutput.finite(blockSize);
            if (block >= 12)
                cosmosChordPeak = std::max(cosmosChordPeak,
                    cosmosOutput.peak(EcosystemEngine::bassBus, blockSize));
            maximumPreProtectionPeak = std::max(maximumPreProtectionPeak,
                                                cosmosEngine.getBassOutputLevel());
        }
        const auto sampleBeforeSteal = cosmosOutput.storage[
            EcosystemEngine::bassBus][blockSize - 1];
        cosmosEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(
            5, cosmosScenario.saxLoopKeyboard.rootNote + 12, 1.0f));
        cosmosOutput.clear();
        process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        float maximumStealStep = std::abs(cosmosOutput.storage[
            EcosystemEngine::bassBus][0] - sampleBeforeSteal);
        for (int sample = 1; sample < blockSize; ++sample)
            maximumStealStep = std::max(maximumStealStep,
                std::abs(cosmosOutput.storage[EcosystemEngine::bassBus][sample]
                    - cosmosOutput.storage[EcosystemEngine::bassBus][sample - 1]));
        maximumPreProtectionPeak = std::max(maximumPreProtectionPeak,
                                            cosmosEngine.getBassOutputLevel());

        for (int note = 0; note < 8; ++note)
            cosmosEngine.enqueueMidiMessage(juce::MidiMessage::noteOff(
                5, cosmosScenario.saxLoopKeyboard.rootNote + note));
        constexpr auto stolenVoiceWarmupBlocks = 36;
        constexpr auto stolenVoiceMeasurementBlocks = 48;
        int stolenVoiceCrossings = 0;
        int stolenVoiceSamples = 0;
        float stolenVoicePrevious = 0.0f;
        bool hasStolenVoicePrevious = false;
        float stolenVoicePeak = 0.0f;
        for (int block = 0;
             block < stolenVoiceWarmupBlocks + stolenVoiceMeasurementBlocks;
             ++block)
        {
            cosmosOutput.clear();
            process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
            cosmosChordStayedFinite &= cosmosOutput.finite(blockSize);
            maximumPreProtectionPeak = std::max(maximumPreProtectionPeak,
                                                cosmosEngine.getBassOutputLevel());
            if (block < stolenVoiceWarmupBlocks)
                continue;
            stolenVoicePeak = std::max(stolenVoicePeak,
                cosmosOutput.peak(EcosystemEngine::bassBus, blockSize));
            for (int sampleIndex = 0; sampleIndex < blockSize;
                 ++sampleIndex)
            {
                const auto sample = cosmosOutput.storage[
                    EcosystemEngine::bassBus][sampleIndex];
                if (hasStolenVoicePrevious
                    && stolenVoicePrevious <= 0.0f && sample > 0.0f)
                    ++stolenVoiceCrossings;
                stolenVoicePrevious = sample;
                hasStolenVoicePrevious = true;
                ++stolenVoiceSamples;
            }
        }
        const auto stolenVoiceFrequency = stolenVoiceSamples > 0
            ? static_cast<double>(stolenVoiceCrossings) * sampleRate
                / static_cast<double>(stolenVoiceSamples)
            : 0.0;
        passed &= expect(cosmosChordPeak > 0.0001f
                             && stolenVoicePeak > 0.0001f
                             && cosmosChordStayedFinite,
                         "accordo e nona voce COSMOS devono restare udibili e finiti");
        passed &= expect(stolenVoiceFrequency
                                 > cosmosRootFrequency * 1.65
                             && stolenVoiceFrequency
                                 < cosmosRootFrequency * 2.35,
                         "la nona nota COSMOS deve sopravvivere al voice stealing");
        passed &= expect(maximumStealStep < 0.08f,
                         "il voice stealing COSMOS non deve creare crackle");
        passed &= expect(maximumPreProtectionPeak > 0.0001f
                             && maximumPreProtectionPeak < 0.78f,
                         "COSMOS polifonico deve conservare headroom prima della protezione");

        // Clear while a full chord is sounding. The first post-clear sample
        // must join the previous one, the explicit 8 ms fade must then reach
        // silence inside the block, and fresh MIDI must not resurrect a
        // deleted source while the delay storage is drained incrementally.
        cosmosEngine.prepare(sampleRate, blockSize);
        for (int note = 0; note < 8; ++note)
            cosmosEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(
                5, cosmosScenario.saxLoopKeyboard.rootNote + note, 1.0f));
        float preClearPeak = 0.0f;
        float sampleBeforeClear = 0.0f;
        bool meaningfulClearBoundary = false;
        for (int block = 0; block < 64; ++block)
        {
            cosmosOutput.clear();
            process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
            preClearPeak = std::max(preClearPeak,
                cosmosOutput.peak(EcosystemEngine::bassBus, blockSize));
            sampleBeforeClear = cosmosOutput.storage[
                EcosystemEngine::bassBus][blockSize - 1];
            if (block >= 12 && std::abs(sampleBeforeClear) > 0.001f)
            {
                meaningfulClearBoundary = true;
                break;
            }
        }
        cosmosEngine.clearMemory(EcosystemEngine::midiMemoryCount);
        cosmosOutput.clear();
        process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
        float maximumClearStep = std::abs(cosmosOutput.storage[
            EcosystemEngine::bassBus][0] - sampleBeforeClear);
        for (int sample = 1; sample < blockSize; ++sample)
            maximumClearStep = std::max(maximumClearStep,
                std::abs(cosmosOutput.storage[EcosystemEngine::bassBus][sample]
                    - cosmosOutput.storage[EcosystemEngine::bassBus][sample - 1]));
        const auto clearFadeSamples = static_cast<int>(
            std::ceil(sampleRate * 0.008));
        float postFadePeak = 0.0f;
        for (int sample = juce::jlimit(0, blockSize, clearFadeSamples);
             sample < blockSize; ++sample)
            postFadePeak = std::max(postFadePeak,
                std::abs(cosmosOutput.storage[
                    EcosystemEngine::bassBus][sample]));

        cosmosEngine.enqueueMidiMessage(juce::MidiMessage::noteOn(
            5, cosmosScenario.saxLoopKeyboard.rootNote, 1.0f));
        float clearedCosmosPeak = 0.0f;
        bool clearedCosmosStayedFinite = cosmosOutput.finite(blockSize);
        for (int block = 0; block < 16; ++block)
        {
            cosmosOutput.clear();
            process(cosmosEngine, nullptr, 0, cosmosOutput.pointers.data(),
                    EcosystemEngine::logicalOutputBusCount, blockSize);
            clearedCosmosPeak = std::max(clearedCosmosPeak,
                cosmosOutput.peak(EcosystemEngine::bassBus, blockSize));
            clearedCosmosStayedFinite &= cosmosOutput.finite(blockSize);
        }
        const auto clearStepLimit = std::max(
            0.001f, std::abs(sampleBeforeClear) * 0.25f);
        passed &= expect(meaningfulClearBoundary && preClearPeak > 0.0001f
                             && maximumClearStep < clearStepLimit,
                         "clear COSMOS live deve applicare un raccordo anti-click");
        passed &= expect(! cosmosEngine.hasMaterial(
                             EcosystemEngine::midiMemoryCount)
                             && postFadePeak < 0.000001f
                             && clearedCosmosPeak < 0.000001f
                             && clearedCosmosStayedFinite,
                         "clear COSMOS deve diventare rapidamente silenzioso");
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
    const auto recordChordLoop = [&multiLoopEngine, &multiLoopOutput](
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

    float multiLoopPeak = 0.0f;
    float multiLoopMaximumStep = 0.0f;
    float previousMultiLoopSample = 0.0f;
    bool multiLoopStayedFinite = true;
    int multiLoopProtectedSamples = 0;
    for (int block = 0; block < 400; ++block)
    {
        multiLoopOutput.clear();
        process(multiLoopEngine, nullptr, 0, multiLoopOutput.pointers.data(),
                EcosystemEngine::logicalOutputBusCount, blockSize);
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
    }
    passed &= expect(multiLoopStayedFinite && multiLoopPeak < 0.60f,
                     "tre loop polifonici devono restare finiti e con headroom");
    passed &= expect(multiLoopProtectedSamples == 0,
                     "tre loop polifonici non devono tenere attiva la protezione");
    passed &= expect(multiLoopMaximumStep < 0.08f,
                     "i wrap asincroni dei tre loop non devono creare crackle");

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
