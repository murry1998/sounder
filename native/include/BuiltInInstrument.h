#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <string>
#include <map>

// Base class for built-in instruments. Wraps juce::Synthesiser inside
// juce::AudioProcessor so it can sit in MidiTrack's instrument slot.
class BuiltInInstrument : public juce::AudioProcessor {
public:
    BuiltInInstrument(const std::string& instrumentName)
        : AudioProcessor(BusesProperties()
            .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          instrumentName(instrumentName) {}

    ~BuiltInInstrument() override = default;

    // AudioProcessor overrides
    const juce::String getName() const override { return instrumentName; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override {
        synth.setCurrentPlaybackSampleRate(sampleRate);
        prepareSynth(sampleRate, samplesPerBlock);
    }

    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override {
        buffer.clear();
        synth.renderNextBlock(buffer, midiMessages, 0, buffer.getNumSamples());
    }

    double getTailLengthSeconds() const override { return 0.0; }

    bool acceptsMidi() const override { return true; }
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

    // Parameter interface for built-in instruments
    virtual void setParam(const std::string& name, float value) = 0;
    virtual float getParam(const std::string& name) const = 0;

protected:
    juce::Synthesiser synth;
    std::string instrumentName;

    virtual void prepareSynth(double sampleRate, int samplesPerBlock) = 0;
};
