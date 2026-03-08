#include "Metronome.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

Metronome::Metronome() = default;
Metronome::~Metronome() = default;

void Metronome::setBPM(int newBpm) {
    bpm.store(juce::jlimit(20, 300, newBpm));
}

void Metronome::setTimeSignature(int numerator, int denominator) {
    timeSigNumerator = numerator;
    timeSigDenominator = denominator;
}

void Metronome::setVolume(float v) {
    volume.store(juce::jlimit(0.0f, 1.0f, v));
}

int Metronome::getCurrentBeat() const {
    return currentBeat.load();
}

void Metronome::prepareSamples(double sampleRate) {
    int clickLength = static_cast<int>(sampleRate * 0.02); // 20ms click

    // Downbeat: 880 Hz sine burst
    downbeatClick.setSize(1, clickLength);
    for (int i = 0; i < clickLength; i++) {
        float env = 1.0f - static_cast<float>(i) / clickLength;
        env *= env;
        float sample = std::sin(2.0 * M_PI * 880.0 * i / sampleRate) * env * 0.8f;
        downbeatClick.setSample(0, i, sample);
    }

    // Normal beat: 660 Hz sine burst
    normalClick.setSize(1, clickLength);
    for (int i = 0; i < clickLength; i++) {
        float env = 1.0f - static_cast<float>(i) / clickLength;
        env *= env;
        float sample = std::sin(2.0 * M_PI * 660.0 * i / sampleRate) * env * 0.6f;
        normalClick.setSample(0, i, sample);
    }
}

void Metronome::processBlock(juce::AudioBuffer<float>& buffer,
                              int64_t positionInSamples, double sampleRate) {
    float vol = volume.load();
    if (vol <= 0.0f) return;

    int currentBPM = bpm.load();
    double samplesPerBeat = (sampleRate * 60.0) / currentBPM;
    int numSamples = buffer.getNumSamples();

    for (int i = 0; i < numSamples; i++) {
        int64_t samplePos = positionInSamples + i;
        if (samplePos < 0) continue;

        int beat = static_cast<int>(samplePos / samplesPerBeat);
        int posInBeat = static_cast<int>(samplePos - static_cast<int64_t>(beat * samplesPerBeat));

        int beatInBar = beat % timeSigNumerator;
        currentBeat.store(beatInBar);

        auto& click = (beatInBar == 0) ? downbeatClick : normalClick;
        int clickLen = click.getNumSamples();

        if (posInBeat < clickLen) {
            float clickSample = click.getSample(0, posInBeat) * vol;
            for (int ch = 0; ch < buffer.getNumChannels(); ch++) {
                buffer.addSample(ch, i, clickSample);
            }
        }
    }
}
