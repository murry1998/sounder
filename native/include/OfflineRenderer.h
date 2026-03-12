#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <string>
#include <vector>
#include <functional>

class AudioGraph;

struct ExportOptions {
    std::string outputDir;
    bool exportStems = true;     // individual tracks
    bool exportMixdown = true;   // full mix
    int bitDepth = 24;           // 16, 24, or 32
    std::string format = "wav";  // "wav", "aiff", or "mp3"
    int mp3Bitrate = 320;        // 128, 192, 256, or 320 kbps
    double startTime = 0.0;
    double endTime = 0.0;        // 0 = auto (use longest track)
    std::string filePrefix;      // prefix for stem files
    bool normalize = false;      // normalize output to 0 dBFS
};

struct ExportProgress {
    int currentTrack = 0;
    int totalTracks = 0;
    float progress = 0.0f;       // 0.0 to 1.0
    std::string currentFile;
    bool complete = false;
    bool error = false;
    std::string errorMessage;
};

class OfflineRenderer {
public:
    using ProgressCallback = std::function<void(const ExportProgress&)>;

    static ExportProgress exportTracks(AudioGraph& graph, const ExportOptions& options,
                                        ProgressCallback progress = nullptr);

private:
    static bool renderTrackToFile(AudioGraph& graph, int trackId, bool isMidi,
                                   const std::string& filePath, double sampleRate,
                                   int blockSize, double startTime, double endTime,
                                   const ExportOptions& options, double bpm);
    static bool renderMixdownToFile(AudioGraph& graph, const std::string& filePath,
                                     double sampleRate, int blockSize,
                                     double startTime, double endTime,
                                     const ExportOptions& options, double bpm);
    static bool writeWithFormat(const std::string& filePath,
                                 const juce::AudioBuffer<float>& buffer,
                                 double sampleRate, const ExportOptions& options);
};
