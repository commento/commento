#pragma once

#include <array>

enum class OscillatorModel
{
    sub,
    warm,
    pluck,
    glass,
    reed,
    cloud,
    pulse,
    dualSquare,
    bell,
    air
};

struct SynthPatch
{
    const char* name = "";
    OscillatorModel model = OscillatorModel::warm;
    int transposeSemitones = 0;
    float detuneCents = 0.0f;
    float attackSeconds = 0.1f;
    float decaySeconds = 1.0f;
    float sustain = 0.75f;
    float releaseSeconds = 3.0f;
    float cutoffHz = 5000.0f;
    float keyTrack = 0.5f;
    float drive = 1.0f;
    float harmonicMix = 0.25f;
    float noiseMix = 0.0f;
    float pulseWidth = 0.5f;
    float lfoRateHz = 0.08f;
    float lfoDepth = 0.08f;
    float pan = 0.0f;
    float level = 0.16f;
    float delayMilliseconds = 0.0f;
    float delaySpread = 1.17f;
    float delayFeedback = 0.0f;
    float delayMix = 0.0f;
    float reverbSize = 0.65f;
    float reverbDamping = 0.45f;
    float reverbWet = 0.12f;
};

struct SaxPatch
{
    const char* name = "";
    float toneHz = 8000.0f;
    float drive = 1.0f;
    float delayMilliseconds = 900.0f;
    float delaySpread = 1.31f;
    float feedback = 0.35f;
    float crossFeedback = 0.45f;
    float delayMix = 0.28f;
    float modulationRateHz = 0.08f;
    float modulationDepthMilliseconds = 2.0f;
    float reverbSize = 0.78f;
    float reverbDamping = 0.42f;
    float reverbWet = 0.28f;
    float tremoloRateHz = 0.0f;
    float tremoloDepth = 0.0f;
    float outputGain = 0.62f;
    float loopDecay = 0.965f;
};

// One deliberately simple wearable gesture per scenario. Every physical pad
// on the NM2 recalls this same profile, so it can be played without looking at
// the controller. Values are mixed by the already allocated sax-only DSP;
// structural flags reuse the existing delay and tail controls.
struct Nm2SceneGesture
{
    const char* name = "GESTO";
    float fuzz = 0.0f;
    float dark = 0.0f;
    float radio = 0.0f;
    float narrow = 0.0f;
    float empty = 0.0f;
    float blade = 0.0f;
    float pulse = 0.0f;
    float metal = 0.0f;
    float orbit = 0.0f;
    bool echoThrow = false;
    bool freeze = false;
    bool freeTail = false;
    bool listen = false;
    bool stutter = false;
};

struct SoundScenario
{
    const char* name = "";
    const char* character = "";
    std::array<SynthPatch, 4> layers;
    SaxPatch sax;
    Nm2SceneGesture nm2;
    bool useFourHeadSaxLoopPlayback = false;
};

namespace CommentoScenarios
{
constexpr int count = 14;

[[nodiscard]] int wrapIndex(int index);
[[nodiscard]] const SoundScenario& get(int index);
}
