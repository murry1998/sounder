#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <string>
#include <vector>
#include <memory>

class AudioGraph;
class TransportEngine;
class MidiInputManager;

struct ProjectInfo {
    std::string id;
    std::string name;
    std::string date;
    int trackCount;
};

class FileIO {
public:
    // Audio file read/write
    static std::unique_ptr<juce::AudioBuffer<float>> readAudioFile(
        const std::string& path, double targetSampleRate);
    static bool writeWAV(const std::string& path,
        const juce::AudioBuffer<float>& buffer, double sampleRate, int bitDepth = 24);
    static bool writeAIFF(const std::string& path,
        const juce::AudioBuffer<float>& buffer, double sampleRate, int bitDepth = 24);
    static bool writeMP3(const std::string& path,
        const juce::AudioBuffer<float>& buffer, double sampleRate, int bitrate = 320);

    // Project file I/O
    static bool saveProject(const std::string& projectsDir, const std::string& projectName,
        const AudioGraph& graph, const TransportEngine& transport, int bpm, int timeSigNum);
    static bool loadProject(const std::string& projectsDir, const std::string& projectId,
        AudioGraph& graph, TransportEngine& transport, int& bpm, int& timeSigNum);
    static std::vector<ProjectInfo> listProjects(const std::string& projectsDir);
    static bool deleteProject(const std::string& projectsDir, const std::string& projectId);

    // MIDI file I/O (Standard MIDI File)
    static juce::MidiMessageSequence readMidiFile(const std::string& path, int trackIndex = 0);
    static bool writeMidiFile(const std::string& path, const juce::MidiMessageSequence& sequence,
                              double bpm, int timeSigNum = 4);
};
