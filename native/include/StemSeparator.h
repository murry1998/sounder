#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <array>
#include <memory>
#include <functional>
#include <vector>

struct StemResult {
    std::unique_ptr<juce::AudioBuffer<float>> vocals;
    std::unique_ptr<juce::AudioBuffer<float>> bass;
    std::unique_ptr<juce::AudioBuffer<float>> drums;
    std::unique_ptr<juce::AudioBuffer<float>> other;
};

class StemSeparator {
public:
    StemSeparator();
    ~StemSeparator();

    bool loadModel(const std::string& modelPath);
    bool isModelLoaded() const;

    StemResult separate(
        const juce::AudioBuffer<float>& source,
        double sampleRate,
        std::function<void(float)> progressCallback = nullptr
    );

private:
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::SessionOptions> sessionOptions;
    bool modelLoaded = false;

    // Model-fixed parameters (HTDemucs v4 ONNX)
    static constexpr int FFT_SIZE = 4096;
    static constexpr int HOP_SIZE = 1024;
    static constexpr int NUM_STEMS = 4;
    static constexpr int MODEL_SR = 44100;
    static constexpr int SEGMENT_SAMPLES = 441000;  // 10 seconds at 44100
    static constexpr int NUM_FREQ_BINS = 2048;      // FFT_SIZE / 2
    static constexpr int NUM_TIME_FRAMES = 431;     // center-padded STFT frames
    static constexpr int OVERLAP_SAMPLES = 88200;   // 2 seconds overlap for cleaner joins

    juce::dsp::FFT fft{12}; // log2(4096) = 12
    std::vector<float> window;

    void initWindow();

    void computeComplexSTFT(const float* input, int numSamples,
                            std::vector<float>& stftReal,
                            std::vector<float>& stftImag);

    void processSegment(const float* leftIn, const float* rightIn,
                        std::array<juce::AudioBuffer<float>, NUM_STEMS>& outputs);

    juce::AudioBuffer<float> resample(const juce::AudioBuffer<float>& source,
                                       double sourceSR, double targetSR);
};
