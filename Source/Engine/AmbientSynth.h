#pragma once

#include <JuceHeader.h>

class AmbientSynth final
{
public:
    explicit AmbientSynth(int layerStyle);

    void prepare(double sampleRate);
    void render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                int startSample, int numSamples);

private:
    juce::Synthesiser synthesiser;
};

