#pragma once

#include "BuiltInInstrument.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>

// 16-pad drum machine. Each pad maps to MIDI notes 36-51 (GM drum map).
// One-shot sample playback per pad with per-pad volume and velocity sensitivity.

struct DrumPad {
    juce::AudioBuffer<float> sample;
    double sampleRate = 44100.0;
    float volume = 1.0f;
    float velocitySensitivity = 0.8f;
    bool loaded = false;
};

class DrumKitSound : public juce::SynthesiserSound {
public:
    bool appliesToNote(int noteNumber) override {
        return noteNumber >= 36 && noteNumber <= 51;
    }
    bool appliesToChannel(int) override { return true; }
};

class DrumKitVoice : public juce::SynthesiserVoice {
public:
    DrumKitVoice(std::array<DrumPad, 16>& pads);

    bool canPlaySound(juce::SynthesiserSound* sound) override;
    void startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int) override;
    void stopNote(float velocity, bool allowTailOff) override;
    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}
    void renderNextBlock(juce::AudioBuffer<float>&, int startSample, int numSamples) override;

private:
    std::array<DrumPad, 16>& pads;
    int activePadIndex = -1;
    double samplePosition = 0.0;
    double playbackRate = 1.0;
    float noteVelocity = 0.0f;
};

class DrumKit : public BuiltInInstrument {
public:
    DrumKit();
    ~DrumKit() override = default;

    void setParam(const std::string& name, float value) override;
    float getParam(const std::string& name) const override;

    void loadPadSample(int padIndex, const std::string& filePath);

protected:
    void prepareSynth(double sampleRate, int samplesPerBlock) override;

private:
    std::array<DrumPad, 16> pads;
};
