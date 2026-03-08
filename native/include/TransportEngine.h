#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <map>
#include <vector>
#include <memory>

class TransportEngine {
public:
    enum State { Stopped, Playing, Recording };

    TransportEngine();
    ~TransportEngine();

    void play();
    void stop();
    void record(const std::vector<int>& armedTrackIds);
    void rewind();
    void seekTo(double timeInSeconds);

    State getState() const;
    double getCurrentTimeInSeconds() const;
    int64_t getCurrentPositionInSamples() const;
    double getTotalDuration() const;
    void setTotalDuration(double dur);

    // Loop
    void setLoopEnabled(bool enabled);
    void setLoopRegion(double startSec, double endSec);
    bool isLoopEnabled() const { return loopEnabled; }

    // Called from the audio callback
    void processBlock(int numSamples);

    // Recording (audio)
    bool isRecordingTrack(int trackId) const;
    void feedRecordingInput(int trackId, const juce::AudioBuffer<float>& input, int numSamples);
    std::unique_ptr<juce::AudioBuffer<float>> finalizeRecording(int trackId);
    void clearRecordingBuffers() { recordingBuffers.clear(); }

    // Live recording waveform
    std::vector<float> getRecordingWaveform(int trackId, int numPoints) const;
    int getRecordingSampleCount(int trackId) const;

    // Recording (MIDI)
    void recordMidiTracks(const std::vector<int>& armedMidiTrackIds);
    bool isRecordingMidiTrack(int trackId) const;
    void feedRecordingMidi(int trackId, const juce::MidiMessage& msg, double beatPosition);
    struct RecordedMidiNote {
        int noteNumber;
        double startBeat;
        double endBeat;     // filled on noteOff
        int velocity;
        int channel;
    };
    std::vector<RecordedMidiNote> finalizeMidiRecording(int trackId);
    void clearMidiRecordingBuffers() { midiRecordingBuffers.clear(); }

    void setSampleRate(double sr) { sampleRate = sr; }
    double getSampleRate() const { return sampleRate; }

private:
    std::atomic<State> state{Stopped};
    std::atomic<int64_t> positionInSamples{0};
    double sampleRate = 48000.0;
    double totalDuration = 0.0;

    bool loopEnabled = false;
    int64_t loopStartSample = 0;
    int64_t loopEndSample = 0;

    struct RecordBuffer {
        std::vector<float> left, right;
        std::atomic<size_t> sampleCount{0};
    };
    std::map<int, RecordBuffer> recordingBuffers;

    // MIDI recording buffers
    struct MidiRecordBuffer {
        std::vector<RecordedMidiNote> completedNotes;
        // Active notes awaiting noteOff: key = (channel << 8) | noteNumber
        std::map<int, RecordedMidiNote> activeNotes;
    };
    std::map<int, MidiRecordBuffer> midiRecordingBuffers;
};
