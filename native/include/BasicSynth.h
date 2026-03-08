#pragma once

#include "BuiltInInstrument.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>

// 2-oscillator subtractive synth with filter, ADSR, 16 polyphonic voices.
class BasicSynthSound : public juce::SynthesiserSound {
public:
    bool appliesToNote(int) override { return true; }
    bool appliesToChannel(int) override { return true; }
};

struct BasicSynthParams {
    // Oscillators
    int osc1Waveform = 1;   // 0=sine, 1=saw, 2=square, 3=triangle
    int osc2Waveform = 0;
    float osc2Detune = 0.0f;       // semitones
    float oscMix = 0.5f;           // 0=osc1 only, 1=osc2 only

    // Filter
    int filterType = 0;            // 0=LP, 1=HP, 2=BP
    float filterCutoff = 8000.0f;  // Hz
    float filterResonance = 0.5f;  // 0-1

    // Amplitude envelope
    float ampAttack = 0.01f;
    float ampDecay = 0.1f;
    float ampSustain = 0.8f;
    float ampRelease = 0.3f;

    // Filter envelope
    float filterEnvAttack = 0.01f;
    float filterEnvDecay = 0.2f;
    float filterEnvSustain = 0.3f;
    float filterEnvRelease = 0.3f;
    float filterEnvDepth = 0.0f;   // semitones of cutoff modulation
};

class BasicSynthVoice : public juce::SynthesiserVoice {
public:
    BasicSynthVoice(BasicSynthParams& params);

    bool canPlaySound(juce::SynthesiserSound* sound) override;
    void startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int) override;
    void stopNote(float velocity, bool allowTailOff) override;
    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}
    void renderNextBlock(juce::AudioBuffer<float>&, int startSample, int numSamples) override;

private:
    BasicSynthParams& params;
    double phase1 = 0.0, phase2 = 0.0;
    double frequency = 440.0;
    float noteVelocity = 0.0f;
    juce::ADSR ampEnv, filterEnv;
    juce::dsp::StateVariableTPTFilter<float> filter;

    float generateOscSample(double& phase, double freq, int waveform);
    void updateFilterParams(float envValue);
};

class BasicSynth : public BuiltInInstrument {
public:
    BasicSynth();
    ~BasicSynth() override = default;

    void setParam(const std::string& name, float value) override;
    float getParam(const std::string& name) const override;

protected:
    void prepareSynth(double sampleRate, int samplesPerBlock) override;

private:
    BasicSynthParams params;
};
