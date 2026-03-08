#pragma once

#include "BuiltInInstrument.h"
#include <juce_audio_formats/juce_audio_formats.h>

// Pitched sample playback instrument. Load a WAV, map to root note,
// play at different pitches via playback rate adjustment.
class SamplePlayerSound : public juce::SynthesiserSound {
public:
    SamplePlayerSound(juce::AudioBuffer<float>& buf, double fileSampleRate, int rootNote);

    bool appliesToNote(int noteNumber) override;
    bool appliesToChannel(int) override { return true; }

    juce::AudioBuffer<float>& getBuffer() { return buffer; }
    double getFileSampleRate() const { return fileSampleRate; }
    int getRootNote() const { return rootNote; }

private:
    juce::AudioBuffer<float>& buffer;
    double fileSampleRate;
    int rootNote;
    int lowNote = 0, highNote = 127;

    friend class SamplePlayerVoice;
    friend class SamplePlayer;
};

struct SamplePlayerParams {
    int rootNote = 60;       // C4
    int lowNote = 0;
    int highNote = 127;
    float ampAttack = 0.005f;
    float ampDecay = 0.0f;
    float ampSustain = 1.0f;
    float ampRelease = 0.1f;
    float velocitySensitivity = 0.8f;
};

class SamplePlayerVoice : public juce::SynthesiserVoice {
public:
    SamplePlayerVoice(SamplePlayerParams& params);

    bool canPlaySound(juce::SynthesiserSound* sound) override;
    void startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int) override;
    void stopNote(float velocity, bool allowTailOff) override;
    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}
    void renderNextBlock(juce::AudioBuffer<float>&, int startSample, int numSamples) override;

private:
    SamplePlayerParams& params;
    double samplePosition = 0.0;
    double pitchRatio = 1.0;
    float noteVelocity = 0.0f;
    juce::ADSR adsr;
    SamplePlayerSound* currentSound = nullptr;
};

class SamplePlayer : public BuiltInInstrument {
public:
    SamplePlayer();
    ~SamplePlayer() override = default;

    void setParam(const std::string& name, float value) override;
    float getParam(const std::string& name) const override;

    void loadSample(const std::string& filePath);

protected:
    void prepareSynth(double sampleRate, int samplesPerBlock) override;

private:
    SamplePlayerParams params;
    juce::AudioBuffer<float> sampleBuffer;
    double fileSampleRate = 44100.0;
    bool sampleLoaded = false;

    void rebuildSound();
};
