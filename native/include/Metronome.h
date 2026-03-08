#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

class Metronome {
public:
    Metronome();
    ~Metronome();

    void setBPM(int bpm);
    int getBPM() const { return bpm.load(); }
    void setTimeSignature(int numerator, int denominator);
    void setVolume(float volume);
    void processBlock(juce::AudioBuffer<float>& buffer, int64_t positionInSamples, double sampleRate);
    int getCurrentBeat() const;

    void prepareSamples(double sampleRate);

private:
    std::atomic<int> bpm{120};
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    std::atomic<float> volume{0.0f};
    std::atomic<int> currentBeat{0};

    juce::AudioBuffer<float> downbeatClick;
    juce::AudioBuffer<float> normalClick;
};
