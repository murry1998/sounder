#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <functional>
#include "AudioTrack.h"
#include "MidiTrack.h"
#include "BusTrack.h"
#include "MeterData.h"
#include <mutex>

class TransportEngine;
class Metronome;

class AudioGraph : public juce::AudioIODeviceCallback {
public:
    AudioGraph();
    ~AudioGraph();

    bool openDevice(double sampleRate, int blockSize);
    void closeDevice();
    bool ensureInputEnabled();

    // Audio tracks
    int addTrack(const std::string& name);
    int addTrackWithId(int id, const std::string& name);  // Force specific ID (for project loading)
    void removeTrack(int trackId);
    AudioTrack* getTrack(int trackId);
    const std::map<int, std::unique_ptr<AudioTrack>>& getTracks() const { return tracks; }

    // MIDI tracks
    int addMidiTrack(const std::string& name);
    int addMidiTrackWithId(int id, const std::string& name);  // Force specific ID (for project loading)
    void removeMidiTrack(int trackId);
    MidiTrack* getMidiTrack(int trackId);
    const std::map<int, std::unique_ptr<MidiTrack>>& getMidiTracks() const { return midiTracks; }

    // Reset track ID counter (call after clearing all tracks, before loading a project)
    void resetNextTrackId(int id = 0) { nextTrackId = id; }

    // Bus tracks
    int addBusTrack(const std::string& name);
    void removeBusTrack(int busId);
    BusTrack* getBusTrack(int busId);
    const std::map<int, std::unique_ptr<BusTrack>>& getBusTracks() const { return busTracks; }

    // External MIDI input (from controllers, thread-safe)
    void feedExternalMidi(const juce::MidiMessage& msg, int sampleOffset = 0);

    void setMasterVolume(float volume);
    float getMasterVolume() const;

    // Master insert slots (5 effect slots on the master bus)
    static constexpr int MAX_MASTER_INSERTS = 5;
    void insertMasterBuiltInEffect(int slot, std::unique_ptr<juce::AudioProcessor> effect);
    void insertMasterPlugin(int slot, std::unique_ptr<juce::AudioPluginInstance> plugin);
    void removeMasterInsert(int slot);
    juce::AudioProcessor* getMasterInsert(int slot);
    bool isMasterInsertBuiltIn(int slot);

    MeterData getMeterData() const;

    void setTransport(TransportEngine* t) { transport = t; }
    void setMetronome(Metronome* m) { metronome = m; }
    double getBPM() const;

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    // Audio device enumeration / configuration
    struct DeviceInfo {
        std::string name;
        std::string typeName;
        std::vector<double> sampleRates;
        std::vector<int> bufferSizes;
        int numInputChannels = 0;
        int numOutputChannels = 0;
        std::vector<std::string> inputChannelNames;
        std::vector<std::string> outputChannelNames;
    };
    struct CurrentDeviceState {
        std::string name;
        std::string inputDeviceName;
        std::string typeName;
        double sampleRate = 0;
        int bufferSize = 0;
        int activeInputs = 0;
        int activeOutputs = 0;
        double inputLatencyMs = 0;
        double outputLatencyMs = 0;
        std::vector<std::string> inputChannelNames;
        std::vector<std::string> outputChannelNames;
    };

    std::vector<DeviceInfo> getAvailableDevices();
    std::vector<DeviceInfo> getInputDevices();
    CurrentDeviceState getCurrentDeviceState() const;
    bool setAudioDevice(const std::string& deviceName, double sampleRate, int bufferSize);
    bool setAudioDeviceSeparate(const std::string& outputName, const std::string& inputName,
                                double sampleRate, int bufferSize);

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData, int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    juce::AudioDeviceManager deviceManager;
    std::map<int, std::unique_ptr<AudioTrack>> tracks;
    std::map<int, std::unique_ptr<MidiTrack>> midiTracks;
    std::map<int, std::unique_ptr<BusTrack>> busTracks;
    int nextTrackId = 0;
    int nextBusId = 0;
    float masterVolume = 0.8f;
    double currentSampleRate = 48000.0;
    int currentBlockSize = 512;

    TransportEngine* transport = nullptr;
    Metronome* metronome = nullptr;

    // Master insert slots
    std::array<std::unique_ptr<juce::AudioProcessor>, MAX_MASTER_INSERTS> masterInsertSlots;

    // External MIDI input buffer (written from MIDI callback thread, read from audio thread)
    juce::MidiBuffer incomingMidiBuffer;
    std::mutex midiMutex;

    std::atomic<float> masterPeakL{0.0f};
    std::atomic<float> masterPeakR{0.0f};
    std::atomic<float> masterRmsL{0.0f};
    std::atomic<float> masterRmsR{0.0f};
};
