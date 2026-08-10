#include "Scenarios.h"

#include <array>
#include <cstddef>

namespace
{
float calibratedBassLevel(OscillatorModel model) noexcept
{
    // The live bass is folded from a constant-power stereo voice to one mono
    // Model 12 channel. Oscillator families do not have the same peak/RMS
    // energy, so a single level made pulse/sub scenes noticeably hotter. These
    // values keep maximum-velocity, maximum-expression factory patches within
    // roughly one decibel of one another while retaining about 11 dBFS of
    // performance headroom before the user's per-sound attenuation.
    switch (model)
    {
        case OscillatorModel::sub:   return 0.205f;
        case OscillatorModel::warm:  return 0.240f;
        case OscillatorModel::pluck: return 0.215f;
        case OscillatorModel::pulse: return 0.192f;
        case OscillatorModel::dualSquare: return 0.197f;
        case OscillatorModel::glass:
        case OscillatorModel::reed:
        case OscillatorModel::cloud:
        case OscillatorModel::bell:
        case OscillatorModel::air:   return 0.215f;
    }
    return 0.215f;
}

SynthPatch makeBass(const char* name, OscillatorModel model, int transpose,
                    float cutoff, float attack, float release, float drive,
                    float calibratedLevel = 0.0f)
{
    SynthPatch patch;
    patch.name = name;
    patch.model = model;
    patch.transposeSemitones = transpose;
    patch.detuneCents = 2.0f;
    patch.attackSeconds = attack;
    patch.decaySeconds = 0.55f;
    patch.sustain = 0.76f;
    patch.releaseSeconds = release;
    patch.cutoffHz = cutoff;
    patch.keyTrack = 0.28f;
    patch.drive = drive;
    patch.harmonicMix = 0.24f;
    patch.lfoRateHz = 0.05f;
    patch.lfoDepth = 0.025f;
    patch.level = calibratedLevel > 0.0f
        ? calibratedLevel : calibratedBassLevel(model);
    patch.reverbSize = 0.35f;
    patch.reverbDamping = 0.72f;
    patch.reverbWet = 0.0f;
    return patch;
}

SynthPatch makeCosmosBass()
{
    auto patch = makeBass("DUE QUADRE", OscillatorModel::dualSquare, -12,
                          2450.0f, 0.005f, 0.34f, 1.48f);
    patch.detuneCents = 6.5f;
    patch.decaySeconds = 0.34f;
    patch.sustain = 0.72f;
    patch.keyTrack = 0.22f;
    patch.harmonicMix = 0.47f;
    patch.pulseWidth = 0.5f;
    patch.lfoRateHz = 0.0f;
    patch.lfoDepth = 0.0f;
    return patch;
}

SynthPatch makePad(const char* name, OscillatorModel model, float attack,
                   float release, float cutoff, float pan, float space)
{
    SynthPatch patch;
    patch.name = name;
    patch.model = model;
    patch.detuneCents = 8.0f;
    patch.attackSeconds = attack;
    patch.decaySeconds = 2.4f;
    patch.sustain = 0.78f;
    patch.releaseSeconds = release;
    patch.cutoffHz = cutoff;
    patch.keyTrack = 0.52f;
    patch.drive = 1.05f;
    patch.harmonicMix = 0.32f;
    patch.noiseMix = model == OscillatorModel::cloud ? 0.08f : 0.0f;
    patch.lfoRateHz = 0.035f + space * 0.06f;
    patch.lfoDepth = 0.08f + space * 0.10f;
    patch.pan = pan;
    patch.level = 0.115f;
    // Pads keep a broad halo, but no longer share the same multi-second wash.
    // The per-layer DELAY control scales this send down to a truly dry path.
    const auto sideTiming = pan < 0.0f ? 0.82f : 1.08f;
    patch.delayMilliseconds = (380.0f + space * 1250.0f) * sideTiming;
    patch.delaySpread = 1.11f + space * 0.18f;
    patch.delayFeedback = 0.22f + space * 0.16f;
    patch.delayMix = 0.10f + space * 0.12f;
    patch.reverbSize = 0.72f + space * 0.23f;
    patch.reverbDamping = 0.52f;
    patch.reverbWet = 0.20f + space * 0.20f;
    return patch;
}

SynthPatch makePluck(const char* name, OscillatorModel model, float cutoff,
                     float delayMs, float feedback, float pan, float brightness = 1.0f)
{
    SynthPatch patch;
    patch.name = name;
    patch.model = model;
    patch.detuneCents = 4.5f;
    patch.attackSeconds = 0.004f;
    patch.decaySeconds = 0.58f;
    patch.sustain = 0.035f;
    patch.releaseSeconds = 1.8f;
    patch.cutoffHz = cutoff;
    patch.keyTrack = 0.72f;
    patch.drive = 1.0f + brightness * 0.18f;
    patch.harmonicMix = 0.38f + brightness * 0.18f;
    patch.noiseMix = model == OscillatorModel::pluck ? 0.025f : 0.0f;
    patch.lfoRateHz = 0.11f;
    patch.lfoDepth = 0.045f;
    patch.pan = pan;
    patch.level = 0.14f;
    patch.delayMilliseconds = delayMs * 0.62f;
    patch.delaySpread = pan < 0.0f ? 1.27f : 1.43f;
    patch.delayFeedback = feedback * 0.72f;
    patch.delayMix = 0.28f;
    patch.reverbSize = 0.88f;
    patch.reverbDamping = 0.32f;
    patch.reverbWet = 0.34f;
    return patch;
}

SynthPatch makeBell(const char* name, float cutoff, float delayMs,
                    float pan, int transpose = 0)
{
    auto patch = makePluck(name, OscillatorModel::bell, cutoff, delayMs,
                           0.48f, pan, 1.15f);
    patch.transposeSemitones = transpose;
    patch.decaySeconds = 1.4f;
    patch.releaseSeconds = 4.8f;
    patch.sustain = 0.12f;
    patch.level = 0.105f;
    patch.delayMilliseconds *= 0.72f;
    patch.delayFeedback *= 0.82f;
    patch.delayMix = 0.24f;
    return patch;
}

SynthPatch makeAir(const char* name, float attack, float release,
                   float cutoff, float pan, float space)
{
    auto patch = makePad(name, OscillatorModel::air, attack, release,
                         cutoff, pan, space);
    patch.noiseMix = 0.16f;
    patch.harmonicMix = 0.18f;
    patch.level = 0.085f;
    patch.delayMilliseconds *= 0.52f;
    patch.delayFeedback *= 0.78f;
    patch.delayMix = 0.14f;
    patch.delaySpread = 1.53f;
    return patch;
}

SynthPatch makeDrone(const char* name, OscillatorModel model, int transpose,
                     float cutoff, float pan, float delayMs, float movement,
                     float level, float noise = 0.0f)
{
    SynthPatch patch;
    patch.name = name;
    patch.model = model;
    patch.transposeSemitones = transpose;
    patch.detuneCents = 11.0f + movement * 18.0f;
    patch.attackSeconds = 2.8f + movement * 7.0f;
    patch.decaySeconds = 4.0f;
    patch.sustain = 0.91f;
    patch.releaseSeconds = 13.0f + movement * 12.0f;
    patch.cutoffHz = cutoff;
    patch.keyTrack = 0.32f;
    patch.drive = 1.04f;
    patch.harmonicMix = 0.30f;
    patch.noiseMix = noise;
    patch.lfoRateHz = 0.012f + movement * 0.055f;
    patch.lfoDepth = 0.10f + movement * 0.22f;
    patch.pan = pan;
    patch.level = level;
    patch.delayMilliseconds = delayMs;
    patch.delaySpread = pan < 0.0f ? 1.19f : 1.37f;
    patch.delayFeedback = 0.30f + movement * 0.23f;
    patch.delayMix = 0.12f + movement * 0.12f;
    patch.reverbSize = 0.90f + movement * 0.08f;
    patch.reverbDamping = 0.58f;
    patch.reverbWet = 0.38f + movement * 0.12f;
    return patch;
}

SynthPatch makeMetal(const char* name, OscillatorModel model, int transpose,
                     float cutoff, float pan, float delayMs, float brightness)
{
    SynthPatch patch;
    patch.name = name;
    patch.model = model;
    patch.transposeSemitones = transpose;
    patch.detuneCents = 7.0f + brightness * 9.0f;
    patch.attackSeconds = 0.002f;
    patch.decaySeconds = 0.24f + brightness * 0.42f;
    patch.sustain = 0.015f;
    patch.releaseSeconds = 1.4f + brightness * 2.2f;
    patch.cutoffHz = cutoff;
    patch.keyTrack = 0.82f;
    patch.drive = 1.08f + brightness * 0.34f;
    patch.harmonicMix = 0.54f + brightness * 0.18f;
    patch.noiseMix = model == OscillatorModel::pulse ? 0.035f : 0.012f;
    patch.pulseWidth = pan < 0.0f ? 0.19f : 0.77f;
    patch.lfoRateHz = 0.17f + brightness * 0.24f;
    patch.lfoDepth = 0.035f;
    patch.pan = pan;
    patch.level = 0.085f + brightness * 0.018f;
    patch.delayMilliseconds = delayMs;
    patch.delaySpread = pan < 0.0f ? 1.43f : 1.71f;
    patch.delayFeedback = 0.34f + brightness * 0.20f;
    patch.delayMix = 0.18f + brightness * 0.10f;
    patch.reverbSize = 0.58f;
    patch.reverbDamping = 0.24f;
    patch.reverbWet = 0.18f + brightness * 0.10f;
    return patch;
}

SynthPatch makeNoise(const char* name, OscillatorModel model, float cutoff,
                     float pan, float delayMs, float movement, float level)
{
    SynthPatch patch;
    patch.name = name;
    patch.model = model;
    patch.detuneCents = 14.0f + movement * 20.0f;
    patch.attackSeconds = 0.025f + movement * 0.42f;
    patch.decaySeconds = 1.1f;
    patch.sustain = 0.34f + movement * 0.24f;
    patch.releaseSeconds = 3.2f + movement * 5.0f;
    patch.cutoffHz = cutoff;
    patch.keyTrack = 0.18f;
    patch.drive = 1.16f + movement * 0.30f;
    patch.harmonicMix = 0.13f;
    patch.noiseMix = 0.27f + movement * 0.22f;
    patch.pulseWidth = pan < 0.0f ? 0.14f : 0.84f;
    patch.lfoRateHz = 0.15f + movement * 0.54f;
    patch.lfoDepth = 0.22f + movement * 0.28f;
    patch.pan = pan;
    patch.level = level;
    patch.delayMilliseconds = delayMs;
    patch.delaySpread = pan < 0.0f ? 1.57f : 1.83f;
    patch.delayFeedback = 0.31f + movement * 0.22f;
    patch.delayMix = 0.13f + movement * 0.13f;
    patch.reverbSize = 0.72f + movement * 0.18f;
    patch.reverbDamping = 0.68f;
    patch.reverbWet = 0.22f + movement * 0.17f;
    return patch;
}

SynthPatch makeCosmosRing(const char* name, int transpose, float cutoff,
                          float pan, float delayMs, bool upperRing)
{
    // COSMOS used the generic drone recipe here.  Its 18--23 second releases,
    // wide detuning and deep modulation kept all eight voices alive and made
    // overlapping MIDI loops turn into an indistinct cluster.  These rings
    // retain the slow orbit, but favour a nearly-sine glass oscillator (the
    // cheapest model in AmbientSynth) and leave space for the recorded sax.
    SynthPatch patch;
    patch.name = name;
    patch.model = OscillatorModel::glass;
    patch.transposeSemitones = transpose;
    patch.detuneCents = upperRing ? 4.8f : 3.2f;
    patch.attackSeconds = upperRing ? 1.25f : 0.72f;
    patch.decaySeconds = upperRing ? 2.8f : 2.1f;
    patch.sustain = upperRing ? 0.68f : 0.74f;
    patch.releaseSeconds = upperRing ? 7.2f : 5.6f;
    patch.cutoffHz = cutoff;
    patch.keyTrack = 0.38f;
    patch.drive = 1.0f;
    patch.harmonicMix = upperRing ? 0.14f : 0.07f;
    patch.lfoRateHz = upperRing ? 0.031f : 0.019f;
    patch.lfoDepth = upperRing ? 0.065f : 0.045f;
    patch.pan = pan;
    patch.level = upperRing ? 0.060f : 0.070f;
    patch.delayMilliseconds = delayMs;
    patch.delaySpread = upperRing ? 1.31f : 1.19f;
    patch.delayFeedback = upperRing ? 0.24f : 0.20f;
    patch.delayMix = upperRing ? 0.13f : 0.10f;
    patch.reverbSize = upperRing ? 0.88f : 0.82f;
    patch.reverbDamping = 0.64f;
    patch.reverbWet = upperRing ? 0.30f : 0.25f;
    return patch;
}

SynthPatch makeCosmosDust(const char* name)
{
    // A restrained breath layer replaces the old makeNoise recipe.  The old
    // patch combined 26-cent detuning, heavy drive and 35% pitch movement;
    // beautiful in isolation, but harsh and needlessly dense over two rings.
    SynthPatch patch;
    patch.name = name;
    patch.model = OscillatorModel::cloud;
    patch.detuneCents = 4.0f;
    patch.attackSeconds = 1.65f;
    patch.decaySeconds = 2.6f;
    patch.sustain = 0.58f;
    patch.releaseSeconds = 6.4f;
    patch.cutoffHz = 2350.0f;
    patch.keyTrack = 0.24f;
    patch.drive = 1.0f;
    patch.harmonicMix = 0.10f;
    patch.noiseMix = 0.035f;
    patch.lfoRateHz = 0.043f;
    patch.lfoDepth = 0.075f;
    patch.pan = 0.38f;
    patch.level = 0.048f;
    patch.delayMilliseconds = 475.0f;
    patch.delaySpread = 1.47f;
    patch.delayFeedback = 0.18f;
    patch.delayMix = 0.09f;
    patch.reverbSize = 0.86f;
    patch.reverbDamping = 0.72f;
    patch.reverbWet = 0.28f;
    return patch;
}

SaxPatch makeSax(const char* name, float tone, float drive, float delay,
                 float spread, float feedback, float cross, float delayMix,
                 float modRate, float modDepth, float room, float damping,
                 float reverb, float tremoloRate, float tremoloDepth,
                 float gain, float decay)
{
    // Preserve the identity of the ten recipes while preventing every sax
    // treatment from becoming the same long feedback cloud.
    return { name, tone, drive, 280.0f + delay * 0.52f, spread,
             feedback * 0.72f, cross * 0.72f, delayMix * 0.62f,
             modRate, modDepth, room, damping, reverb, tremoloRate,
             tremoloDepth, gain, decay };
}

const std::array<SoundScenario, CommentoScenarios::count> scenarios {{
    {
        "ABISSO", "scuro, lento, profondo",
        {{
            makeBass("SUB PROFONDO", OscillatorModel::sub, -12, 780.0f, 0.018f, 0.42f, 1.35f),
            makePad("MAREA SCURA", OscillatorModel::warm, 1.8f, 9.0f, 2100.0f, -0.26f, 0.88f),
            makeBell("VETRO SOMMERSO", 3300.0f, 2400.0f, 0.28f, -12),
            makeAir("POLVERE FONDA", 2.4f, 11.0f, 1800.0f, 0.34f, 0.92f)
        }},
        makeSax("CAVERNA", 2300.0f, 1.08f, 2800.0f, 1.37f, 0.72f, 0.78f,
                0.58f, 0.035f, 4.0f, 0.94f, 0.67f, 0.48f, 0.0f, 0.0f, 0.52f, 0.955f)
    },
    {
        "GOCCE", "pluck dilatati e riflessi",
        {{
            makeBass("BASSO TONDO", OscillatorModel::warm, -12, 1250.0f, 0.012f, 0.34f, 1.18f),
            makePluck("LEGNO LONTANO", OscillatorModel::pluck, 5200.0f, 1320.0f, 0.62f, -0.34f),
            makeBell("GOCCE DI VETRO", 9200.0f, 1770.0f, 0.29f, 0),
            makePluck("FILO DI LUCE", OscillatorModel::glass, 7600.0f, 2310.0f, 0.68f, 0.38f)
        }},
        makeSax("PING PONG LIQUIDO", 7200.0f, 1.0f, 980.0f, 1.73f, 0.57f, 0.88f,
                0.52f, 0.11f, 2.8f, 0.83f, 0.34f, 0.34f, 0.0f, 0.0f, 0.58f, 0.968f)
    },
    {
        "NASTRO", "caldo, instabile, consumato",
        {{
            makeBass("BASSO NASTRO", OscillatorModel::warm, -12, 1650.0f, 0.026f, 0.55f, 1.42f),
            makePluck("TASTO CONSUMATO", OscillatorModel::reed, 4300.0f, 740.0f, 0.48f, -0.30f, 0.65f),
            makePad("CORO MAGNETICO", OscillatorModel::reed, 0.85f, 7.5f, 3900.0f, 0.24f, 0.67f),
            makeBell("SCINTILLA OPACA", 5100.0f, 1460.0f, 0.38f, -12)
        }},
        makeSax("ECO A NASTRO", 4600.0f, 1.32f, 690.0f, 1.09f, 0.64f, 0.32f,
                0.47f, 0.23f, 9.0f, 0.76f, 0.62f, 0.30f, 0.17f, 0.10f, 0.56f, 0.958f)
    },
    {
        "CATTEDRALE", "code immense e armoniche",
        {{
            makeBass("PEDALE", OscillatorModel::sub, -12, 1050.0f, 0.12f, 0.9f, 1.15f),
            makePad("ORGANO LENTO", OscillatorModel::reed, 1.25f, 12.0f, 5200.0f, -0.22f, 0.96f),
            makeBell("CAMPANE ALTE", 9800.0f, 3100.0f, 0.31f, 12),
            makePad("CORO ALTO", OscillatorModel::cloud, 2.7f, 14.0f, 6400.0f, 0.31f, 1.0f)
        }},
        makeSax("NAVATA", 6200.0f, 1.03f, 1850.0f, 1.47f, 0.55f, 0.65f,
                0.40f, 0.028f, 2.5f, 0.99f, 0.31f, 0.68f, 0.0f, 0.0f, 0.50f, 0.972f)
    },
    {
        "AURORA", "chiaro, mobile, aperto",
        {{
            makeBass("BASSO MORBIDO", OscillatorModel::sub, -12, 1450.0f, 0.022f, 0.38f, 1.12f),
            makePluck("PLUCK DI LUCE", OscillatorModel::glass, 9800.0f, 1540.0f, 0.58f, -0.36f, 1.25f),
            makeAir("ARIA AZZURRA", 1.35f, 8.5f, 7200.0f, 0.26f, 0.76f),
            makeBell("CRISTALLO", 12000.0f, 2180.0f, 0.39f, 12)
        }},
        makeSax("ALONE", 9800.0f, 0.94f, 1220.0f, 1.21f, 0.48f, 0.72f,
                0.39f, 0.16f, 7.0f, 0.92f, 0.27f, 0.51f, 0.08f, 0.15f, 0.56f, 0.974f)
    },
    {
        "MAREA", "onde asincrone e spazio largo",
        {{
            makeBass("BASSO FLUIDO", OscillatorModel::warm, -12, 1350.0f, 0.035f, 0.52f, 1.18f),
            makePad("ONDA LENTA", OscillatorModel::warm, 1.55f, 10.0f, 3600.0f, -0.32f, 0.86f),
            makePluck("LEGNO BAGNATO", OscillatorModel::pluck, 4700.0f, 1980.0f, 0.61f, 0.23f, 0.72f),
            makeAir("SCHIUMA", 0.75f, 7.8f, 5700.0f, 0.37f, 0.84f)
        }},
        makeSax("RISACCA", 5400.0f, 1.02f, 2140.0f, 1.53f, 0.67f, 0.74f,
                0.55f, 0.052f, 5.5f, 0.89f, 0.53f, 0.46f, 0.045f, 0.22f, 0.54f, 0.960f)
    },
    {
        "RADICE", "legno, terra, attacchi organici",
        {{
            makeBass("BASSO DI LEGNO", OscillatorModel::pluck, -12, 2100.0f, 0.006f, 0.26f, 1.48f),
            makePluck("CORDA CORTA", OscillatorModel::pluck, 6200.0f, 860.0f, 0.44f, -0.25f, 0.82f),
            makePad("TERRA", OscillatorModel::warm, 0.62f, 5.5f, 2800.0f, 0.18f, 0.42f),
            makeAir("FIATO", 0.38f, 4.7f, 4200.0f, 0.31f, 0.46f)
        }},
        makeSax("STANZA DI LEGNO", 4100.0f, 1.22f, 540.0f, 1.29f, 0.38f, 0.24f,
                0.29f, 0.19f, 3.0f, 0.66f, 0.71f, 0.20f, 0.0f, 0.0f, 0.62f, 0.982f)
    },
    {
        "ORBITA", "impulsi sospesi fuori tempo",
        {{
            makeBass("BASSO PULSO", OscillatorModel::pulse, -12, 1800.0f, 0.008f, 0.31f, 1.34f),
            makePluck("PLUCK ORBITALE", OscillatorModel::pulse, 7200.0f, 1110.0f, 0.66f, -0.38f, 0.95f),
            makeBell("SATELLITE", 10800.0f, 1870.0f, 0.28f, 12),
            makePad("COSMO", OscillatorModel::cloud, 2.1f, 11.0f, 4600.0f, 0.35f, 0.93f)
        }},
        makeSax("ELLISSE", 6800.0f, 1.06f, 1370.0f, 1.91f, 0.71f, 0.92f,
                0.57f, 0.074f, 4.2f, 0.90f, 0.39f, 0.43f, 0.13f, 0.30f, 0.52f, 0.956f)
    },
    {
        "POLVERE", "fragile, opaco, granuloso",
        {{
            makeBass("BASSO ROTTO", OscillatorModel::pulse, -12, 980.0f,
                     0.014f, 0.36f, 1.62f, 0.213f),
            makePluck("TASTO SECCO", OscillatorModel::reed, 3300.0f, 620.0f, 0.52f, -0.27f, 0.45f),
            makeAir("GRANA", 0.12f, 5.2f, 2600.0f, 0.22f, 0.64f),
            makePad("FANTASMA", OscillatorModel::cloud, 2.8f, 12.0f, 1900.0f, 0.36f, 0.88f)
        }},
        makeSax("RADIO LONTANA", 2700.0f, 1.55f, 760.0f, 1.04f, 0.59f, 0.18f,
                0.42f, 0.31f, 12.0f, 0.74f, 0.78f, 0.24f, 0.27f, 0.28f, 0.52f, 0.948f)
    },
    {
        "VUOTO", "pochi elementi, molto respiro",
        {{
            makeBass("BASSO PURO", OscillatorModel::sub, -12, 920.0f, 0.045f, 0.48f, 1.08f),
            makePad("SENO LENTO", OscillatorModel::warm, 2.8f, 13.0f, 2400.0f, -0.28f, 0.58f),
            makePluck("IMPULSO", OscillatorModel::glass, 6400.0f, 3600.0f, 0.72f, 0.24f, 0.72f),
            makeAir("SOSPESO", 3.6f, 15.0f, 3100.0f, 0.34f, 0.82f)
        }},
        makeSax("UN SOLO ECO", 5200.0f, 0.98f, 4200.0f, 1.13f, 0.43f, 0.12f,
                0.31f, 0.018f, 1.5f, 0.82f, 0.58f, 0.22f, 0.0f, 0.0f, 0.64f, 0.986f)
    },
    {
        "DRONE", "masse lente, pedali e deriva profonda",
        {{
            makeBass("PEDALE TELLURICO", OscillatorModel::sub, -12, 680.0f,
                     0.085f, 0.82f, 1.18f),
            makeDrone("FONDO IMMOBILE", OscillatorModel::warm, -12, 1150.0f,
                      -0.31f, 1320.0f, 0.22f, 0.090f),
            makeDrone("CORO FOSSILE", OscillatorModel::reed, 0, 2450.0f,
                      0.24f, 1710.0f, 0.46f, 0.072f),
            makeDrone("ARIA FERMA", OscillatorModel::air, 12, 1750.0f,
                      0.39f, 2070.0f, 0.68f, 0.052f, 0.20f)
        }},
        makeSax("COLONNA D'ARIA", 2200.0f, 1.05f, 3100.0f, 1.41f, 0.62f, 0.58f,
                0.46f, 0.024f, 5.0f, 0.96f, 0.64f, 0.52f, 0.025f, 0.14f,
                0.49f, 0.972f)
    },
    {
        "FERRO", "urti metallici, risonanze corte e taglienti",
        {{
            makeBass("MOTORE FERRO", OscillatorModel::pulse, -12, 1850.0f,
                     0.006f, 0.28f, 1.46f),
            makeMetal("LAMIERA", OscillatorModel::bell, 0, 10800.0f,
                      -0.37f, 313.0f, 0.72f),
            makeMetal("FILO TESO", OscillatorModel::pulse, 12, 7600.0f,
                      0.29f, 521.0f, 0.50f),
            makeMetal("RISONATORE", OscillatorModel::glass, -12, 6100.0f,
                      0.41f, 887.0f, 0.88f)
        }},
        makeSax("LASTRA", 9200.0f, 1.38f, 430.0f, 1.67f, 0.54f, 0.86f,
                0.45f, 0.33f, 2.0f, 0.61f, 0.23f, 0.18f, 0.43f, 0.32f,
                0.52f, 0.950f)
    },
    {
        "SCIAME", "rumore vivo, scatti e traiettorie instabili",
        {{
            makeBass("BASSO INSETTO", OscillatorModel::pluck, -12, 1550.0f,
                     0.004f, 0.24f, 1.42f),
            makeNoise("ALI", OscillatorModel::air, 4300.0f, -0.42f,
                      457.0f, 0.82f, 0.058f),
            makeNoise("NUVOLA VIVA", OscillatorModel::cloud, 2600.0f, 0.19f,
                      733.0f, 0.56f, 0.066f),
            makeNoise("SCATTI", OscillatorModel::pulse, 5700.0f, 0.43f,
                      1193.0f, 0.94f, 0.052f)
        }},
        makeSax("RONZIO", 3600.0f, 1.46f, 1040.0f, 1.97f, 0.65f, 0.90f,
                0.56f, 0.41f, 11.0f, 0.78f, 0.69f, 0.31f, 0.23f, 0.45f,
                0.48f, 0.952f)
    },
    {
        "COSMOS", "anelli armonici e respiro sospeso",
        {{
            makeCosmosBass(),
            makeCosmosRing("ANELLO BASSO", -12, 1550.0f, -0.31f,
                           610.0f, false),
            makeCosmosRing("ANELLO ALTO", 0, 3450.0f, 0.25f,
                           890.0f, true),
            makeCosmosDust("POLVERE D'ARIA")
        }},
        makeSax("ALONE COSMOS", 3900.0f, 0.98f, 1150.0f, 1.29f, 0.36f, 0.38f,
                0.28f, 0.027f, 2.2f, 0.86f, 0.68f, 0.31f, 0.013f, 0.07f,
                0.54f, 0.972f),
        true
    }
}};
}

int CommentoScenarios::wrapIndex(int index)
{
    const auto wrapped = index % count;
    return wrapped < 0 ? wrapped + count : wrapped;
}

const SoundScenario& CommentoScenarios::get(int index)
{
    return scenarios[static_cast<std::size_t>(wrapIndex(index))];
}
