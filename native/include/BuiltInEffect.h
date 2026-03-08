#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <string>
#include <memory>

// Base class for native insertable effects. Extends AudioProcessor so it
// can sit in insert slots alongside VST AudioPluginInstance objects.
class BuiltInEffect : public juce::AudioProcessor {
public:
    BuiltInEffect(const std::string& effectType)
        : AudioProcessor(BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          effectType(effectType) {}

    ~BuiltInEffect() override = default;

    const juce::String getName() const override { return effectType; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Parameter interface
    void setParam(const std::string& name, float value);
    float getParam(const std::string& name) const;
    const std::string& getEffectType() const { return effectType; }

    // Factory
    static std::unique_ptr<BuiltInEffect> create(const std::string& type);

    // List all available effect types
    struct EffectTypeInfo {
        std::string type;
        std::string name;
        std::string category;
    };
    static std::vector<EffectTypeInfo> getAvailableTypes();

private:
    std::string effectType;
    double cachedSampleRate = 44100.0;
    int cachedBlockSize = 512;

    // Gate
    float gateThreshold = -40.0f, gateRatio = 10.0f, gateAttack = 0.5f, gateRelease = 50.0f;
    float gateEnvelope = 0.0f;

    // EQ (3-band)
    float eqLowGain = 0.0f, eqMidGain = 0.0f, eqMidFreq = 1000.0f, eqHighGain = 0.0f;

    // Compressor
    float compThreshold = -24.0f, compRatio = 4.0f, compAttack = 5.0f, compRelease = 100.0f;
    float compEnvelope = 0.0f;

    // Distortion
    float distDrive = 1.0f, distMix = 0.5f;

    // Filter
    float filterCutoff = 1000.0f, filterResonance = 0.5f;
    int filterMode = 0; // 0=LP, 1=HP, 2=BP
    juce::dsp::StateVariableTPTFilter<float> svf;

    // Chorus
    float chorusRate = 1.0f, chorusDepth = 0.5f, chorusCentreDelay = 7.0f;
    float chorusFeedback = 0.0f, chorusMix = 0.5f;
    juce::dsp::Chorus<float> chorus;

    // Phaser
    float phaserRate = 1.0f, phaserDepth = 0.5f, phaserCentreFreq = 1300.0f;
    float phaserFeedback = 0.0f, phaserMix = 0.5f;
    juce::dsp::Phaser<float> phaser;

    // Delay
    float delayTime = 0.25f, delayMix = 0.3f, delayFeedback = 0.3f;
    juce::dsp::DelayLine<float> delayLine{192000};
    float delayL = 0.0f, delayR = 0.0f;

    // Reverb
    float reverbSize = 0.5f, reverbDamping = 0.5f, reverbMix = 0.3f, reverbWidth = 1.0f;
    juce::dsp::Reverb reverb;

    // Limiter
    float limiterThreshold = -1.0f, limiterRelease = 50.0f;
    juce::dsp::Limiter<float> limiter;
};
