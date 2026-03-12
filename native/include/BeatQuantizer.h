#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <memory>
#include <string>

struct OnsetInfo {
    int samplePosition;
    double timeSeconds;
    double nearestBeatTime;
    int targetSamplePosition;
};

struct QuantizeResult {
    std::unique_ptr<juce::AudioBuffer<float>> quantizedBuffer;
    std::vector<OnsetInfo> detectedOnsets;
    int numOnsetsDetected = 0;
    bool success = false;
    std::string errorMessage;
};

struct TempoMatchResult {
    std::unique_ptr<juce::AudioBuffer<float>> stretchedBuffer;
    double detectedBPM = 0.0;
    double targetBPM = 0.0;
    double stretchRatio = 1.0;
    bool success = false;
    std::string errorMessage;
};

class BeatQuantizer {
public:
    BeatQuantizer();
    ~BeatQuantizer();

    QuantizeResult quantize(
        const juce::AudioBuffer<float>& source,
        double sampleRate,
        double bpm,
        double gridDivision = 1.0,
        float quantizeStrength = 1.0f
    );

    std::vector<OnsetInfo> detectOnsets(
        const juce::AudioBuffer<float>& source,
        double sampleRate
    );

    double detectBPM(
        const juce::AudioBuffer<float>& source,
        double sampleRate,
        double minBPM = 40.0,
        double maxBPM = 220.0
    );

    TempoMatchResult tempoMatch(
        const juce::AudioBuffer<float>& source,
        double sampleRate,
        double targetBPM,
        double manualSourceBPM = 0.0,
        double minBPM = 40.0,
        double maxBPM = 220.0
    );

    // Public WSOLA stretch (also used by AudioTrack for pitch-preserving transpose)
    juce::AudioBuffer<float> wsolaStretch(
        const juce::AudioBuffer<float>& sourceSegment, int targetLength);

private:
    static constexpr int FFT_ORDER = 10;
    static constexpr int FFT_SIZE = 1 << FFT_ORDER; // 1024
    static constexpr int HOP_SIZE = 256;
    static constexpr float ONSET_THRESHOLD_MULTIPLIER = 1.15f;
    static constexpr int MEDIAN_WINDOW = 15;
    static constexpr int MIN_ONSET_DISTANCE_MS = 40;
    static constexpr int WSOLA_WINDOW_SIZE = 8192;
    static constexpr int WSOLA_TOLERANCE = 4096;

    juce::dsp::FFT fft{FFT_ORDER};
    std::vector<float> window;

    void initWindow();

    std::vector<float> computeSpectralFlux(
        const float* monoSamples, int numSamples);

    std::vector<int> pickOnsets(
        const std::vector<float>& detectionFunction, double sampleRate);

    void snapToGrid(
        std::vector<OnsetInfo>& onsets,
        double sampleRate, double bpm,
        double gridDivision, float strength);

    int findBestOverlap(
        const float* regionA, const float* regionB,
        int searchLength, int overlapSize);

    std::vector<float> computeAutocorrelation(
        const std::vector<float>& signal, int maxLag);
};
