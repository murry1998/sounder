#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <mutex>

class AudioGraph;

class MidiInputManager : public juce::MidiInputCallback {
public:
    MidiInputManager();
    ~MidiInputManager() override;

    void setAudioGraph(AudioGraph* graph) { audioGraph = graph; }

    // Device enumeration
    struct MidiDeviceInfo {
        std::string name;
        std::string identifier;
        bool isOpen = false;
    };
    std::vector<MidiDeviceInfo> getAvailableDevices() const;

    // Open / close
    bool openDevice(const std::string& identifier);
    void closeDevice(const std::string& identifier);
    void closeAllDevices();

    // Routing: which MIDI track receives input (-1 = all armed)
    void setTargetTrack(int trackId) { targetTrackId.store(trackId); }
    int getTargetTrack() const { return targetTrackId.load(); }

    // Activity monitoring
    bool hasRecentActivity() const;
    int getLastNote() const { return lastNote.load(); }
    int getLastVelocity() const { return lastVelocity.load(); }
    void resetActivity();

    // MIDI Learn
    struct MidiBinding {
        int cc;
        int channel;
        std::string paramPath; // "trackId:paramName"
    };
    void startMidiLearn(const std::string& paramPath);
    void stopMidiLearn();
    bool isMidiLearnActive() const { return midiLearnActive.load(); }
    std::string getMidiLearnTarget() const;
    void addBinding(int cc, int channel, const std::string& paramPath);
    void removeBinding(int cc, int channel);
    std::vector<MidiBinding> getBindings() const;

    // MidiInputCallback
    void handleIncomingMidiMessage(juce::MidiInput* source,
                                    const juce::MidiMessage& message) override;

private:
    AudioGraph* audioGraph = nullptr;
    std::map<std::string, std::unique_ptr<juce::MidiInput>> openDevices;

    std::atomic<int> targetTrackId{-1};

    // Activity
    std::atomic<bool> activityFlag{false};
    std::atomic<int> lastNote{-1};
    std::atomic<int> lastVelocity{0};
    std::atomic<uint64_t> lastActivityTime{0};

    // MIDI Learn
    std::atomic<bool> midiLearnActive{false};
    std::string midiLearnParamPath;
    std::mutex learnMutex;

    // Bindings
    std::vector<MidiBinding> bindings;
    std::mutex bindingsMutex;

    // Apply CC bindings
    void applyBinding(int cc, int value, int channel);
};
