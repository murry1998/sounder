#pragma once

#include "BuiltInInstrument.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <functional>

// ─── SFZ Region Data ─────────────────────────────────────

struct SFZRegion {
    std::string samplePath;
    int sampleIndex = -1;

    int lokey = 0;
    int hikey = 127;
    int pitchKeycenter = 60;

    int lovel = 0;
    int hivel = 127;

    float volume = 0.0f;        // dB
    float pan = 0.0f;           // -100 to +100

    int transpose = 0;          // semitones
    int tune = 0;               // cents

    float ampegAttack = 0.001f;
    float ampegDecay = 0.0f;
    float ampegSustain = 1.0f;  // 0-1 internally (SFZ uses 0-100)
    float ampegRelease = 0.05f;

    int loopMode = 0;           // 0=no_loop, 1=loop_continuous, 2=loop_sustain
    int loopStart = 0;
    int loopEnd = -1;           // -1 = end of sample
};

// ─── Loaded Sample Buffer ────────────────────────────────

struct SFZSampleBuffer {
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    std::string path;
};

// ─── SFZ Sound ───────────────────────────────────────────

class SFZSound : public juce::SynthesiserSound {
public:
    SFZSound(const SFZRegion& region, SFZSampleBuffer& sampleBuf)
        : region(region), sampleBuf(sampleBuf) {}

    bool appliesToNote(int midiNoteNumber) override {
        return midiNoteNumber >= region.lokey && midiNoteNumber <= region.hikey;
    }
    bool appliesToChannel(int) override { return true; }

    const SFZRegion& getRegion() const { return region; }
    SFZSampleBuffer& getSampleBuffer() { return sampleBuf; }

private:
    SFZRegion region;
    SFZSampleBuffer& sampleBuf;
};

// ─── SFZ Voice ───────────────────────────────────────────

class SFZInstrument; // forward declaration

class SFZVoice : public juce::SynthesiserVoice {
public:
    bool canPlaySound(juce::SynthesiserSound* sound) override;
    void startNote(int midiNoteNumber, float velocity,
                   juce::SynthesiserSound* sound, int currentPitchWheelPosition) override;
    void stopNote(float velocity, bool allowTailOff) override;
    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                         int startSample, int numSamples) override;

    void setOwner(SFZInstrument* o) { owner = o; }

private:
    double samplePosition = 0.0;
    double pitchRatio = 1.0;
    float noteGain = 0.0f;
    float panLeft = 1.0f;
    float panRight = 1.0f;
    juce::ADSR adsr;
    SFZSound* currentSFZSound = nullptr;
    bool looping = false;
    int loopStartSample = 0;
    int loopEndSample = 0;
    SFZInstrument* owner = nullptr;
};

// ─── SFZ Preset Definition ──────────────────────────────

struct SFZPresetDefinition {
    std::string id;
    std::string name;
    std::string description;
    std::string icon;
    std::string sfzContent;
    using GeneratorFunc = std::function<void(const std::string& outputDir, double sampleRate)>;
    GeneratorFunc generator;
};

// ─── SFZ Parser ──────────────────────────────────────────

class SFZParser {
public:
    static std::vector<SFZRegion> parse(const std::string& sfzContent,
                                         const std::string& basePath = "");
private:
    static void applyOpcode(SFZRegion& region,
                            const std::string& key, const std::string& value);
    static int parseNoteNumber(const std::string& s);
};

// ─── Sample Generator ────────────────────────────────────

class SFZSampleGenerator {
public:
    static void ensurePresetsGenerated(const std::string& cacheDir, double sampleRate);
    static const std::vector<SFZPresetDefinition>& getPresetDefinitions();

    // Individual instrument generators
    static void generateGrandPiano(const std::string& dir, double sr);
    static void generateElectricPiano(const std::string& dir, double sr);
    static void generatePipeOrgan(const std::string& dir, double sr);
    static void generateStringEnsemble(const std::string& dir, double sr);
    static void generateBrassSection(const std::string& dir, double sr);
    static void generateFingeredBass(const std::string& dir, double sr);
    static void generateNylonGuitar(const std::string& dir, double sr);
    static void generateSynthPad(const std::string& dir, double sr);
    static void generateSFZDrumKit(const std::string& dir, double sr);
    static void generate808Kit(const std::string& dir, double sr);
    static void generateFlute(const std::string& dir, double sr);
    static void generateSquareLead(const std::string& dir, double sr);
    static void generateSawLead(const std::string& dir, double sr);
    static void generateAnalogKit(const std::string& dir, double sr);
    static void generateCR78Kit(const std::string& dir, double sr);
    static void generateLM1Kit(const std::string& dir, double sr);

    // DSP helpers
    static void writeWav(const std::string& path,
                          const juce::AudioBuffer<float>& buf, double sr);
    static void generateAdditiveTone(juce::AudioBuffer<float>& buf,
                                      double sr, double freq,
                                      const std::vector<float>& harmonicAmps,
                                      float duration, float attack, float decay,
                                      float sustainLevel = 0.0f);
    static void generateFMTone(juce::AudioBuffer<float>& buf,
                                double sr, double freq,
                                double modRatio, double modIndexStart,
                                double modIndexEnd, float duration,
                                float attack, float decay);
    static void generateKarplusStrong(juce::AudioBuffer<float>& buf,
                                       double sr, double freq,
                                       float duration, float damping);
    static void generateFilteredNoise(juce::AudioBuffer<float>& buf,
                                       double sr, double cutoff,
                                       float resonance, float duration,
                                       float attack, float decay);
};

// ─── SFZ Instrument ──────────────────────────────────────

class SFZInstrument : public BuiltInInstrument {
public:
    SFZInstrument();
    ~SFZInstrument() override = default;

    void setParam(const std::string& name, float value) override;
    float getParam(const std::string& name) const override;

    void loadPreset(const std::string& presetId);
    void loadSFZFile(const std::string& filePath);

    const std::string& getCurrentPresetId() const { return currentPresetId; }

    // Live parameter accessors (read by voices on audio thread)
    float getLiveMasterVolume() const { return masterVolume; }
    float getLiveVelocitySensitivity() const { return velocitySensitivity; }
    float getLiveAttack() const { return liveAttack; }
    float getLiveDecay() const { return liveDecay; }
    float getLiveSustain() const { return liveSustain; }
    float getLiveRelease() const { return liveRelease; }

protected:
    void prepareSynth(double sampleRate, int samplesPerBlock) override;

private:
    std::string currentPresetId;
    std::vector<SFZRegion> regions;
    std::vector<std::unique_ptr<SFZSampleBuffer>> sampleBuffers;

    float masterVolume = 1.0f;
    float velocitySensitivity = 0.8f;

    // Live ADSR (updated by setParam, read by voices)
    float liveAttack = 0.001f;
    float liveDecay = 0.0f;
    float liveSustain = 1.0f;
    float liveRelease = 0.05f;

    double cachedSampleRate = 44100.0;

    void loadRegionsAndSamples(const std::string& sfzContent,
                                const std::string& basePath);
    void rebuildSynth();
};
