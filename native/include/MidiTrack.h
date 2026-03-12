#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <string>
#include <array>
#include <vector>
#include <atomic>
#include <memory>

class MidiTrack {
public:
    MidiTrack(int id, const std::string& name, double sampleRate, int blockSize);
    ~MidiTrack();

    // MIDI data management
    void setMidiSequence(const juce::MidiMessageSequence& seq);
    const juce::MidiMessageSequence& getMidiSequence() const;
    juce::MidiMessageSequence& getMidiSequenceMutable();

    // Note editing
    int addNote(int noteNumber, double startBeat, double lengthBeats,
                int velocity, int channel = 1);
    void removeNote(int noteIndex);
    void moveNote(int noteIndex, int newNoteNumber, double newStartBeat);
    void resizeNote(int noteIndex, double newLengthBeats);
    void setNoteVelocity(int noteIndex, int velocity);
    void addControlChange(int cc, int value, double beat, int channel = 1);
    void quantizeNotes(double gridSizeBeats);

    // Serializable note data for UI
    struct NoteInfo {
        int noteNumber;
        double startBeat;
        double lengthBeats;
        int velocity;
        int channel;
    };
    std::vector<NoteInfo> getAllNotes() const;

    // Transport
    void processBlock(juce::AudioBuffer<float>& output, int numSamples,
                      double currentTimeInSeconds, double bpm,
                      const juce::MidiBuffer& externalMidi);
    void prepareForPlayback(double sampleRate, int blockSize);

    // Instrument (separate from insert chain)
    void setInstrument(std::unique_ptr<juce::AudioPluginInstance> instrument);
    void setInstrumentProcessor(std::unique_ptr<juce::AudioProcessor> builtInInstrument);
    juce::AudioProcessor* getInstrument();
    bool hasInstrument() const;

    // Insert slots (5 effect slots, native or VST)
    static constexpr int MAX_INSERT_SLOTS = 5;
    void insertEffect(int slotIndex, std::unique_ptr<juce::AudioPluginInstance> plugin);
    void insertBuiltInEffect(int slotIndex, std::unique_ptr<juce::AudioProcessor> effect);
    void removeInsert(int slotIndex);
    juce::AudioProcessor* getInsert(int slotIndex);
    bool isInsertBuiltIn(int slotIndex);

    // Legacy
    void removeEffect(int slotIndex);
    juce::AudioPluginInstance* getEffect(int slotIndex);

    // Output routing
    void setOutputBus(int busId) { outputBus = busId; }
    int getOutputBus() const { return outputBus; }

    // Mixer controls
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

    // Metering
    float getPeakLevel() const;
    float getRMSLevel() const;

    // State
    int getId() const { return id; }
    const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }

    double getDuration(double bpm) const;

private:
    int id;
    std::string name;
    float volume = 0.8f;
    float pan = 0.0f;
    bool muted = false;
    bool soloed = false;
    bool armed = false;
    double sampleRate;
    int blockSize;

    juce::MidiMessageSequence midiSequence;

    int outputBus = -1;

    // Instrument (separate)
    std::unique_ptr<juce::AudioProcessor> instrumentProcessor;
    // Insert effect slots (native or VST)
    std::array<std::unique_ptr<juce::AudioProcessor>, MAX_INSERT_SLOTS> insertSlots;
    juce::SpinLock insertLock;

    // Processing buffers
    juce::AudioBuffer<float> instrumentOutputBuffer;
    juce::MidiBuffer midiBuffer;

    // Thread safety for instrument swap (audio thread vs Node thread)
    juce::SpinLock instrumentLock;
    int instrumentChannelCount = 2;

    // Metering
    std::atomic<float> peakLevel{0.0f};
    std::atomic<float> rmsLevel{0.0f};

    // Internal helpers
    void rebuildNoteSequence();
    struct NoteEvent {
        int noteNumber;
        double startBeat;
        double lengthBeats;
        int velocity;
        int channel;
    };
    std::vector<NoteEvent> notes;
};
