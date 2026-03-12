#pragma once

#include <memory>
#include <string>
#include <atomic>
#include "AudioGraph.h"
#include "TransportEngine.h"
#include "PluginHost.h"
#include "Metronome.h"
#include "MeterData.h"
#include "MidiInputManager.h"

class SounderEngine {
public:
    SounderEngine();
    ~SounderEngine();

    void initialize(double sampleRate, int blockSize, const std::string& projectsDir);
    void shutdown();

    AudioGraph& getAudioGraph() { return *audioGraph; }
    TransportEngine& getTransport() { return *transport; }
    PluginHost& getPluginHost() { return *pluginHost; }
    Metronome& getMetronome() { return *metronome; }
    MidiInputManager& getMidiInput() { return *midiInput; }

    const std::string& getProjectsDir() const { return projectsDir; }
    const std::string& getResourcesDir() const { return resourcesDir; }
    void setResourcesDir(const std::string& dir) { resourcesDir = dir; }
    double getSampleRate() const { return sampleRate; }
    int getBlockSize() const { return blockSize; }
    bool isRunning() const { return running.load(); }

private:
    std::unique_ptr<AudioGraph> audioGraph;
    std::unique_ptr<TransportEngine> transport;
    std::unique_ptr<PluginHost> pluginHost;
    std::unique_ptr<Metronome> metronome;
    std::unique_ptr<MidiInputManager> midiInput;
    std::atomic<bool> running{false};
    std::string projectsDir;
    std::string resourcesDir;
    double sampleRate = 48000.0;
    int blockSize = 512;
};
