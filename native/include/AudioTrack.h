#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <string>
#include <array>
#include <vector>
#include <atomic>
#include <memory>

class AudioTrack {
public:
    AudioTrack(int id, const std::string& name, double sampleRate, int blockSize);
    ~AudioTrack();

    // Buffer management
    void setBuffer(std::unique_ptr<juce::AudioBuffer<float>> buffer, double fileSampleRate);
    double appendBuffer(std::unique_ptr<juce::AudioBuffer<float>> newAudio, double newSampleRate);
    bool hasBuffer() const;
    const juce::AudioBuffer<float>& getBuffer() const { return *audioBuffer; }
    std::vector<float> getWaveformData(int numPoints) const;
    double getDuration() const;

    // Transport
    void prepareForPlayback(double startTimeInSeconds);
    void processBlock(juce::AudioBuffer<float>& output, int numSamples, double currentTime);
    void stopPlayback();

    // Parameters
    void setVolume(float v);
    void setPan(float p);
    void setMute(bool m);
    void setSolo(bool s);
    void setArmed(bool a);
    float getVolume() const { return volume; }
    float getPan() const { return pan; }
    bool isMuted() const { return muted; }
    bool isSoloed() const { return soloed; }
    bool isArmed() const { return armed; }

    // Beat Quantize
    struct QuantizeOptions { double bpm; double gridDivision; float strength; };
    bool quantizeAudio(const QuantizeOptions& options);

    // Tempo Match
    bool tempoMatchAudio(double targetBPM, double& detectedBPM,
                         double manualSourceBPM = 0.0, double minBPM = 40.0, double maxBPM = 220.0);

    // Transpose
    struct TransposeOptions {
        int semitones;           // -24 to +24
        bool preserveTempo;      // false = speed change, true = pitch shift with time-stretch
    };
    bool transposeAudio(const TransposeOptions& options);

    // Normalize
    bool normalizeAudio(float targetPeakDb = 0.0f);

    // Region editing
    void setRegionOffset(double t);
    void setRegionClipStart(double t);
    void setRegionClipEnd(double t);
    void setRegionLoopEnabled(bool enabled);
    void setRegionLoopCount(int count);
    double getRegionOffset() const { return regionOffset; }
    double getRegionClipStart() const { return regionClipStart; }
    double getRegionClipEnd() const { return regionClipEnd; }
    bool getRegionLoopEnabled() const { return regionLoopEnabled; }
    int getRegionLoopCount() const { return regionLoopCount; }

    std::unique_ptr<juce::AudioBuffer<float>> extractBuffer(int startSample, int endSample);
    void trimBufferTo(int startSample, int endSample);

    // Fade in/out
    void setFadeIn(double seconds)  { fadeInDuration = seconds; }
    void setFadeOut(double seconds) { fadeOutDuration = seconds; }
    double getFadeIn() const { return fadeInDuration; }
    double getFadeOut() const { return fadeOutDuration; }

    // Built-in FX
    void setFxParam(const std::string& fxType, const std::string& param, float value);
    void setFxEnabled(const std::string& fxType, bool enabled);

    // Insert slots (up to 5 per track, native or VST)
    static constexpr int MAX_INSERT_SLOTS = 5;
    void insertPlugin(int slotIndex, std::unique_ptr<juce::AudioPluginInstance> plugin);
    void insertBuiltInEffect(int slotIndex, std::unique_ptr<juce::AudioProcessor> effect);
    void removeInsert(int slotIndex);
    juce::AudioProcessor* getInsert(int slotIndex);
    bool isInsertBuiltIn(int slotIndex);

    // Legacy compatibility
    juce::AudioPluginInstance* getPlugin(int slotIndex);
    void removePlugin(int slotIndex);

    // Output routing (-1 = master, else bus id)
    void setOutputBus(int busId) { outputBus = busId; }
    int getOutputBus() const { return outputBus; }

    // Metering
    float getPeakLevel() const;
    float getRMSLevel() const;

    // State
    int getId() const { return id; }
    const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }
    double getBufferSampleRate() const { return bufferSampleRate; }

private:
    int id;
    std::string name;
    float volume = 0.8f;
    float pan = 0.0f;
    bool muted = false;
    bool soloed = false;
    bool armed = false;

    std::unique_ptr<juce::AudioBuffer<float>> audioBuffer;
    double bufferSampleRate = 48000.0;
    int64_t playbackPosition = 0;
    bool isPlaying = false;
    double sampleRate;
    int blockSize;

    // Region
    double regionOffset = 0.0;       // timeline position (seconds) where region starts
    double regionClipStart = 0.0;    // seconds into buffer where playback begins
    double regionClipEnd = -1.0;     // seconds into buffer where playback ends (-1 = full)
    bool regionLoopEnabled = false;  // loop the clip during playback
    int regionLoopCount = 0;         // 0 = infinite loop, >0 = finite repeat count
    double fadeInDuration = 0.0;     // seconds
    double fadeOutDuration = 0.0;    // seconds

    // Built-in effects
    struct BuiltInFX {
        bool eqEnabled = false;
        bool compEnabled = false;
        bool delayEnabled = false;
        float eqLowGain = 0.0f, eqMidGain = 0.0f, eqMidFreq = 1000.0f, eqHighGain = 0.0f;
        float compThreshold = -24.0f, compRatio = 4.0f, compAttack = 0.003f, compRelease = 0.25f;
        float delayTime = 0.0f, delayMix = 0.0f, delayFeedback = 0.3f;
    } builtInFx;

    int outputBus = -1;

    // Insert slots (AudioProcessor* supports both VST and native)
    std::array<std::unique_ptr<juce::AudioProcessor>, MAX_INSERT_SLOTS> insertSlots;
    juce::SpinLock insertLock;
    juce::AudioBuffer<float> pluginProcessBuffer;
    juce::MidiBuffer emptyMidiBuffer;

    // Metering
    std::atomic<float> peakLevel{0.0f};
    std::atomic<float> rmsLevel{0.0f};
};
