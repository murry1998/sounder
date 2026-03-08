#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <string>
#include <array>
#include <atomic>
#include <memory>

class BusTrack {
public:
    BusTrack(int id, const std::string& name, double sampleRate, int blockSize);
    ~BusTrack();

    static constexpr int MAX_INSERT_SLOTS = 5;

    // Input accumulation
    void clearInputBuffer(int numSamples);
    juce::AudioBuffer<float>& getInputBuffer() { return inputBuffer; }

    // Processing
    void processBlock(juce::AudioBuffer<float>& output, int numSamples);

    // Insert slots (native or VST)
    void insertBuiltInEffect(int slot, std::unique_ptr<juce::AudioProcessor> effect);
    void insertPlugin(int slot, std::unique_ptr<juce::AudioPluginInstance> plugin);
    void removeInsert(int slot);
    juce::AudioProcessor* getInsert(int slot);
    bool isInsertBuiltIn(int slot);

    // Mixer controls
    void setVolume(float v);
    void setPan(float p);
    void setMute(bool m);
    void setSolo(bool s);
    float getVolume() const { return volume; }
    float getPan() const { return pan; }
    bool isMuted() const { return muted; }
    bool isSoloed() const { return soloed; }

    // Built-in FX (channel strip)
    void setFxParam(const std::string& fxType, const std::string& param, float value);
    void setFxEnabled(const std::string& fxType, bool enabled);

    // Metering
    float getPeakLevel() const;
    float getRMSLevel() const;

    // State
    int getId() const { return id; }
    const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }

private:
    int id;
    std::string name;
    float volume = 0.8f;
    float pan = 0.0f;
    bool muted = false;
    bool soloed = false;
    double sampleRate;
    int blockSize;

    juce::AudioBuffer<float> inputBuffer;
    std::array<std::unique_ptr<juce::AudioProcessor>, MAX_INSERT_SLOTS> insertSlots;
    juce::MidiBuffer emptyMidi;

    // Built-in channel strip FX
    struct BuiltInFX {
        bool eqEnabled = true;
        bool compEnabled = true;
        bool delayEnabled = false;
        float eqLowGain = 0.0f, eqMidGain = 0.0f, eqMidFreq = 1000.0f, eqHighGain = 0.0f;
        float compThreshold = -24.0f, compRatio = 4.0f, compAttack = 0.003f, compRelease = 0.25f;
        float delayTime = 0.0f, delayMix = 0.0f, delayFeedback = 0.3f;
    } builtInFx;

    std::atomic<float> peakLevel{0.0f};
    std::atomic<float> rmsLevel{0.0f};
};
