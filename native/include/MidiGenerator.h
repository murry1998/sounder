#pragma once
#include <vector>
#include <string>
#include <random>

struct MidiGenConfig {
    int numBars = 4;
    double temperature = 1.0;
    int keyRoot = 0;              // 0=C .. 11=B
    std::string scaleType;        // major, minor, dorian, mixolydian, pentatonic, chromatic
    std::string style;            // melody, bass, chords, arpeggio, drums
    double density = 0.5;
    int octaveLow = 3, octaveHigh = 6;
    double swingAmount = 0.0;
    double beatsPerBar = 4.0;
    long seed = -1;
};

struct GeneratedNote {
    int noteNumber;
    double startBeat;
    double lengthBeats;
    int velocity;
};

class MidiGenerator {
public:
    std::vector<GeneratedNote> generate(const MidiGenConfig& config);
private:
    std::vector<GeneratedNote> generateMelody(const MidiGenConfig& c, std::mt19937& rng);
    std::vector<GeneratedNote> generateBass(const MidiGenConfig& c, std::mt19937& rng);
    std::vector<GeneratedNote> generateChords(const MidiGenConfig& c, std::mt19937& rng);
    std::vector<GeneratedNote> generateArpeggio(const MidiGenConfig& c, std::mt19937& rng);
    std::vector<GeneratedNote> generateDrums(const MidiGenConfig& c, std::mt19937& rng);
    std::vector<int> getScaleNotes(int root, const std::string& scaleType) const;
    int pickScaleNote(int currentNote, const std::vector<int>& scaleNotes,
                      int octLow, int octHigh, std::mt19937& rng, double temperature) const;
    double applySwing(double beat, double swingAmount) const;
};
