#include "Scenarios.h"

#include <array>
#include <cstddef>

namespace
{
SynthPatch makeBass(const char* name, OscillatorModel model, int transpose,
                    float cutoff, float attack, float release, float drive,
                    float level = 0.24f)
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
    patch.level = level;
    patch.reverbSize = 0.35f;
    patch.reverbDamping = 0.72f;
    patch.reverbWet = 0.0f;
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
    patch.delayMilliseconds = 1100.0f + space * 1700.0f;
    patch.delaySpread = 1.23f;
    patch.delayFeedback = 0.30f + space * 0.22f;
    patch.delayMix = 0.14f + space * 0.20f;
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
    patch.delayMilliseconds = delayMs;
    patch.delaySpread = 1.41f;
    patch.delayFeedback = feedback;
    patch.delayMix = 0.43f;
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
    return patch;
}

SaxPatch makeSax(const char* name, float tone, float drive, float delay,
                 float spread, float feedback, float cross, float delayMix,
                 float modRate, float modDepth, float room, float damping,
                 float reverb, float tremoloRate, float tremoloDepth,
                 float gain, float decay)
{
    return { name, tone, drive, delay, spread, feedback, cross, delayMix,
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
            makeBass("BASSO ROTTO", OscillatorModel::pulse, -12, 980.0f, 0.014f, 0.36f, 1.62f),
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
