#include "MidiGenerator.h"
#include <algorithm>
#include <cmath>
#include <map>

// ── Scale tables ──

static const std::map<std::string, std::vector<int>> scaleTables = {
    {"major",       {0, 2, 4, 5, 7, 9, 11}},
    {"minor",       {0, 2, 3, 5, 7, 8, 10}},
    {"dorian",      {0, 2, 3, 5, 7, 9, 10}},
    {"mixolydian",  {0, 2, 4, 5, 7, 9, 10}},
    {"pentatonic",  {0, 2, 4, 7, 9}},
    {"chromatic",   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}}
};

// ── Common rhythm patterns (in beats) ──

struct RhythmPattern {
    std::vector<double> onsets;   // beat positions within a bar
    std::vector<double> lengths;  // note lengths
};

static const std::vector<RhythmPattern> melodyRhythms = {
    {{0, 1, 2, 3},           {1.0, 1.0, 1.0, 1.0}},       // straight quarters
    {{0, 1, 2, 3.5},         {1.0, 1.0, 1.5, 0.5}},       // dotted + eighth
    {{0, 0.5, 1, 2, 3},      {0.5, 0.5, 1.0, 1.0, 1.0}},  // eighth pickup
    {{0, 1.5, 2, 3},         {1.5, 0.5, 1.0, 1.0}},       // syncopated
    {{0, 0.5, 1, 1.5, 2, 3}, {0.5, 0.5, 0.5, 0.5, 1.0, 1.0}}, // eighth run + quarters
    {{0, 2},                  {2.0, 2.0}},                  // half notes
    {{0, 1, 2.5, 3},         {1.0, 1.0, 0.5, 1.0}},       // offbeat hit
};

static const std::vector<RhythmPattern> bassRhythms = {
    {{0, 2},                  {1.5, 1.5}},                  // root-fifth feel
    {{0, 1, 2, 3},           {1.0, 1.0, 1.0, 1.0}},       // walking quarters
    {{0, 0.5, 2, 2.5},       {0.5, 1.5, 0.5, 1.5}},       // octave bounce
    {{0, 1.5, 2, 3.5},       {1.5, 0.5, 1.5, 0.5}},       // syncopated
    {{0, 3},                  {3.0, 1.0}},                  // sustained root
};

// ── Chord progressions (scale degrees, 0-indexed) ──

static const std::vector<std::vector<int>> commonProgressions = {
    {0, 3, 4, 0},     // I-IV-V-I
    {0, 5, 3, 4},     // I-vi-IV-V
    {1, 4, 0, 0},     // ii-V-I-I
    {0, 3, 0, 4},     // I-IV-I-V
    {0, 2, 3, 4},     // I-iii-IV-V
    {5, 3, 0, 4},     // vi-IV-I-V
    {0, 4, 5, 3},     // I-V-vi-IV
};

// ── GM Drum map ──

static constexpr int KICK  = 36;
static constexpr int SNARE = 38;
static constexpr int HIHAT_CLOSED = 42;
static constexpr int HIHAT_OPEN   = 46;
static constexpr int CRASH = 49;
static constexpr int RIDE  = 51;
static constexpr int TOM_HIGH = 48;
static constexpr int TOM_MID  = 45;
static constexpr int TOM_LOW  = 41;

// ── Helpers ──

std::vector<int> MidiGenerator::getScaleNotes(int root, const std::string& scaleType) const {
    auto it = scaleTables.find(scaleType);
    if (it == scaleTables.end()) {
        // Default to major
        it = scaleTables.find("major");
    }
    std::vector<int> notes;
    for (int interval : it->second) {
        notes.push_back((root + interval) % 12);
    }
    return notes;
}

int MidiGenerator::pickScaleNote(int currentNote, const std::vector<int>& scaleNotes,
                                  int octLow, int octHigh, std::mt19937& rng,
                                  double temperature) const {
    int midiLow = octLow * 12;
    int midiHigh = (octHigh + 1) * 12 - 1;

    // Build candidate pool: all scale notes in the octave range
    std::vector<int> candidates;
    for (int oct = octLow; oct <= octHigh; ++oct) {
        for (int pc : scaleNotes) {
            int midi = oct * 12 + pc;
            if (midi >= midiLow && midi <= midiHigh)
                candidates.push_back(midi);
        }
    }
    if (candidates.empty()) return 60; // fallback C4

    // Weight by distance: prefer stepwise motion
    std::vector<double> weights(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        int dist = std::abs(candidates[i] - currentNote);
        if (dist == 0) {
            weights[i] = 0.3; // slight chance of repetition
        } else if (dist <= 2) {
            weights[i] = 4.0; // stepwise = strong preference
        } else if (dist <= 4) {
            weights[i] = 2.0; // small skip
        } else if (dist <= 7) {
            weights[i] = 1.0; // larger skip
        } else {
            weights[i] = 0.3; // leap
        }
        // Apply temperature: higher temp = flatter distribution
        weights[i] = std::pow(weights[i], 1.0 / std::max(temperature, 0.1));
    }

    std::discrete_distribution<int> dist(weights.begin(), weights.end());
    return candidates[dist(rng)];
}

double MidiGenerator::applySwing(double beat, double swingAmount) const {
    if (swingAmount <= 0.0) return beat;
    // Swing affects offbeat eighth notes: beat positions 0.5, 1.5, 2.5, 3.5
    double fractional = beat - std::floor(beat);
    if (std::abs(fractional - 0.5) < 0.01) {
        return std::floor(beat) + 0.5 + (swingAmount * 0.16667); // push offbeats late
    }
    return beat;
}

// ── Main generate dispatcher ──

std::vector<GeneratedNote> MidiGenerator::generate(const MidiGenConfig& config) {
    std::mt19937 rng;
    if (config.seed >= 0) {
        rng.seed(static_cast<unsigned long>(config.seed));
    } else {
        rng.seed(std::random_device{}());
    }

    if (config.style == "melody")   return generateMelody(config, rng);
    if (config.style == "bass")     return generateBass(config, rng);
    if (config.style == "chords")   return generateChords(config, rng);
    if (config.style == "arpeggio") return generateArpeggio(config, rng);
    if (config.style == "drums")    return generateDrums(config, rng);

    // Default to melody
    return generateMelody(config, rng);
}

// ── Melody generator ──

std::vector<GeneratedNote> MidiGenerator::generateMelody(const MidiGenConfig& c, std::mt19937& rng) {
    std::vector<GeneratedNote> result;
    auto scaleNotes = getScaleNotes(c.keyRoot, c.scaleType);
    double totalBeats = c.numBars * c.beatsPerBar;

    // Start note: root in middle of range
    int midOct = (c.octaveLow + c.octaveHigh) / 2;
    int currentNote = midOct * 12 + c.keyRoot;

    std::uniform_int_distribution<int> rhythmPick(0, (int)melodyRhythms.size() - 1);
    std::uniform_real_distribution<double> densityCheck(0.0, 1.0);
    std::uniform_int_distribution<int> velRange(70, 110);

    for (int bar = 0; bar < c.numBars; ++bar) {
        double barStart = bar * c.beatsPerBar;
        const auto& pattern = melodyRhythms[rhythmPick(rng)];

        for (size_t i = 0; i < pattern.onsets.size(); ++i) {
            if (densityCheck(rng) > c.density) continue; // skip note based on density

            double onset = barStart + pattern.onsets[i];
            if (onset >= totalBeats) break;

            double length = pattern.lengths[i];
            // Don't extend past total
            if (onset + length > totalBeats) length = totalBeats - onset;

            currentNote = pickScaleNote(currentNote, scaleNotes,
                                        c.octaveLow, c.octaveHigh, rng, c.temperature);

            int vel = velRange(rng);
            // Accent on beat 1
            if (pattern.onsets[i] < 0.01) vel = std::min(127, vel + 10);

            double swungOnset = applySwing(onset, c.swingAmount);

            result.push_back({currentNote, swungOnset, length, vel});
        }
    }

    return result;
}

// ── Bass generator ──

std::vector<GeneratedNote> MidiGenerator::generateBass(const MidiGenConfig& c, std::mt19937& rng) {
    std::vector<GeneratedNote> result;
    auto scaleNotes = getScaleNotes(c.keyRoot, c.scaleType);
    double totalBeats = c.numBars * c.beatsPerBar;

    // Bass lives in octaves 1-3 typically
    int bassOctLow = std::max(1, c.octaveLow);
    int bassOctHigh = std::min(3, c.octaveHigh);
    if (bassOctHigh < bassOctLow) bassOctHigh = bassOctLow;

    // Pick a chord progression for harmonic movement
    std::uniform_int_distribution<int> progPick(0, (int)commonProgressions.size() - 1);
    const auto& prog = commonProgressions[progPick(rng)];

    std::uniform_int_distribution<int> rhythmPick(0, (int)bassRhythms.size() - 1);
    std::uniform_real_distribution<double> densityCheck(0.0, 1.0);
    std::uniform_int_distribution<int> velRange(80, 115);
    std::uniform_real_distribution<double> approachChance(0.0, 1.0);

    for (int bar = 0; bar < c.numBars; ++bar) {
        double barStart = bar * c.beatsPerBar;
        const auto& pattern = bassRhythms[rhythmPick(rng)];

        // Chord root for this bar
        int progIdx = bar % (int)prog.size();
        int degree = prog[progIdx];
        int rootPC = scaleNotes[degree % scaleNotes.size()];
        int rootNote = bassOctLow * 12 + rootPC;

        for (size_t i = 0; i < pattern.onsets.size(); ++i) {
            if (densityCheck(rng) > c.density * 1.2) continue; // bass stays denser

            double onset = barStart + pattern.onsets[i];
            if (onset >= totalBeats) break;

            double length = pattern.lengths[i];
            if (onset + length > totalBeats) length = totalBeats - onset;

            int note = rootNote;
            // Fifth on beat 3 or later hits
            if (i > 0 && scaleNotes.size() >= 5) {
                int fifthPC = scaleNotes[(degree + 4) % scaleNotes.size()]; // 5th scale degree
                if (approachChance(rng) < 0.4) {
                    note = bassOctLow * 12 + fifthPC;
                }
            }
            // Chromatic approach note at end of bar
            if (i == pattern.onsets.size() - 1 && approachChance(rng) < 0.25 * c.temperature) {
                int nextBarDeg = prog[(bar + 1) % prog.size()];
                int nextRoot = scaleNotes[nextBarDeg % scaleNotes.size()];
                int nextNote = bassOctLow * 12 + nextRoot;
                note = nextNote - 1; // half step below
                length = std::min(length, 0.5);
            }

            // Clamp
            note = std::max(24, std::min(note, 60));

            int vel = velRange(rng);
            if (pattern.onsets[i] < 0.01) vel = std::min(127, vel + 8);

            double swungOnset = applySwing(onset, c.swingAmount);
            result.push_back({note, swungOnset, length, vel});
        }
    }

    return result;
}

// ── Chords generator ──

std::vector<GeneratedNote> MidiGenerator::generateChords(const MidiGenConfig& c, std::mt19937& rng) {
    std::vector<GeneratedNote> result;
    auto scaleNotes = getScaleNotes(c.keyRoot, c.scaleType);
    double totalBeats = c.numBars * c.beatsPerBar;

    std::uniform_int_distribution<int> progPick(0, (int)commonProgressions.size() - 1);
    const auto& prog = commonProgressions[progPick(rng)];

    std::uniform_int_distribution<int> velRange(60, 100);
    std::uniform_real_distribution<double> rhythmChoice(0.0, 1.0);
    std::uniform_real_distribution<double> seventhChance(0.0, 1.0);

    int midOct = (c.octaveLow + c.octaveHigh) / 2;

    for (int bar = 0; bar < c.numBars; ++bar) {
        double barStart = bar * c.beatsPerBar;
        int progIdx = bar % (int)prog.size();
        int degree = prog[progIdx];

        // Build triad from scale
        int root = scaleNotes[degree % scaleNotes.size()];
        int third = scaleNotes[(degree + 2) % scaleNotes.size()];
        int fifth = scaleNotes[(degree + 4) % scaleNotes.size()];

        std::vector<int> chordTones = {
            midOct * 12 + root,
            midOct * 12 + third,
            midOct * 12 + fifth
        };

        // Sometimes add 7th
        if (seventhChance(rng) < 0.3 * c.temperature) {
            int seventh = scaleNotes[(degree + 6) % scaleNotes.size()];
            chordTones.push_back(midOct * 12 + seventh);
        }

        // Fix voicing: ensure third and fifth are above root
        for (size_t i = 1; i < chordTones.size(); ++i) {
            while (chordTones[i] <= chordTones[0])
                chordTones[i] += 12;
        }

        // Comping rhythm based on density
        std::vector<std::pair<double, double>> hits; // onset, length
        double rVal = rhythmChoice(rng);
        if (c.density < 0.3) {
            // Whole note
            hits.push_back({0, c.beatsPerBar});
        } else if (c.density < 0.6 || rVal < 0.4) {
            // Half notes
            hits.push_back({0, c.beatsPerBar / 2.0});
            hits.push_back({c.beatsPerBar / 2.0, c.beatsPerBar / 2.0});
        } else {
            // Syncopated comping
            hits.push_back({0, 1.5});
            hits.push_back({1.5, 1.0});
            hits.push_back({3.0, 1.0});
        }

        for (auto& [onset, length] : hits) {
            double absOnset = barStart + onset;
            if (absOnset >= totalBeats) break;
            double absLen = length;
            if (absOnset + absLen > totalBeats) absLen = totalBeats - absOnset;

            int vel = velRange(rng);
            double swungOnset = applySwing(absOnset, c.swingAmount);

            for (int tone : chordTones) {
                // Clamp to range
                while (tone < c.octaveLow * 12) tone += 12;
                while (tone > (c.octaveHigh + 1) * 12 - 1) tone -= 12;
                result.push_back({tone, swungOnset, absLen, vel});
            }
        }
    }

    return result;
}

// ── Arpeggio generator ──

std::vector<GeneratedNote> MidiGenerator::generateArpeggio(const MidiGenConfig& c, std::mt19937& rng) {
    std::vector<GeneratedNote> result;
    auto scaleNotes = getScaleNotes(c.keyRoot, c.scaleType);
    double totalBeats = c.numBars * c.beatsPerBar;

    std::uniform_int_distribution<int> progPick(0, (int)commonProgressions.size() - 1);
    const auto& prog = commonProgressions[progPick(rng)];

    // Note subdivision based on density
    double noteLen = 0.5; // default eighths
    if (c.density < 0.3)      noteLen = 1.0;   // quarters
    else if (c.density > 0.7) noteLen = 0.25;  // sixteenths

    std::uniform_int_distribution<int> velRange(65, 100);
    std::uniform_int_distribution<int> directionPick(0, 2); // 0=up, 1=down, 2=alternate

    int midOct = (c.octaveLow + c.octaveHigh) / 2;

    for (int bar = 0; bar < c.numBars; ++bar) {
        double barStart = bar * c.beatsPerBar;
        int progIdx = bar % (int)prog.size();
        int degree = prog[progIdx];

        // Build chord tones across octave range
        int root = scaleNotes[degree % scaleNotes.size()];
        int third = scaleNotes[(degree + 2) % scaleNotes.size()];
        int fifth = scaleNotes[(degree + 4) % scaleNotes.size()];

        std::vector<int> arpTones;
        for (int oct = c.octaveLow; oct <= c.octaveHigh; ++oct) {
            arpTones.push_back(oct * 12 + root);
            arpTones.push_back(oct * 12 + third);
            arpTones.push_back(oct * 12 + fifth);
        }
        std::sort(arpTones.begin(), arpTones.end());

        // Remove duplicates
        arpTones.erase(std::unique(arpTones.begin(), arpTones.end()), arpTones.end());

        // Keep tones in valid range
        std::vector<int> validTones;
        for (int t : arpTones) {
            if (t >= c.octaveLow * 12 && t <= (c.octaveHigh + 1) * 12 - 1)
                validTones.push_back(t);
        }
        if (validTones.empty()) continue;

        int direction = directionPick(rng);
        int notesPerBar = (int)(c.beatsPerBar / noteLen);
        int idx = 0;
        bool ascending = true;

        for (int n = 0; n < notesPerBar; ++n) {
            double onset = barStart + n * noteLen;
            if (onset >= totalBeats) break;

            int toneIdx;
            if (direction == 0) {
                toneIdx = n % (int)validTones.size();
            } else if (direction == 1) {
                toneIdx = ((int)validTones.size() - 1) - (n % (int)validTones.size());
            } else {
                // Alternating
                toneIdx = idx;
                if (ascending) {
                    idx++;
                    if (idx >= (int)validTones.size()) { idx = (int)validTones.size() - 2; ascending = false; }
                } else {
                    idx--;
                    if (idx < 0) { idx = 1; ascending = true; }
                }
                toneIdx = std::max(0, std::min(toneIdx, (int)validTones.size() - 1));
            }

            int vel = velRange(rng);
            // Accent pattern: louder on beat 1 and 3
            if (n == 0 || n == notesPerBar / 2) vel = std::min(127, vel + 15);

            double swungOnset = applySwing(onset, c.swingAmount);
            result.push_back({validTones[toneIdx], swungOnset, noteLen * 0.9, vel});
        }
    }

    return result;
}

// ── Drums generator ──

std::vector<GeneratedNote> MidiGenerator::generateDrums(const MidiGenConfig& c, std::mt19937& rng) {
    std::vector<GeneratedNote> result;
    double totalBeats = c.numBars * c.beatsPerBar;

    std::uniform_real_distribution<double> densityCheck(0.0, 1.0);
    std::uniform_int_distribution<int> velKick(90, 120);
    std::uniform_int_distribution<int> velSnare(85, 115);
    std::uniform_int_distribution<int> velHat(50, 90);
    std::uniform_int_distribution<int> velGhost(30, 55);
    std::uniform_int_distribution<int> stylePick(0, 2); // 0=rock, 1=electronic, 2=jazz

    int style = stylePick(rng);

    for (int bar = 0; bar < c.numBars; ++bar) {
        double barStart = bar * c.beatsPerBar;
        bool isFillBar = (bar > 0 && bar % 4 == 3 && c.density > 0.4);

        if (isFillBar) {
            // Fill: toms and snare rolls
            double noteLen = c.density > 0.7 ? 0.25 : 0.5;
            int notesInFill = (int)(c.beatsPerBar / noteLen);
            std::uniform_int_distribution<int> fillDrum(0, 3);
            int fillDrums[] = {SNARE, TOM_HIGH, TOM_MID, TOM_LOW};

            for (int n = 0; n < notesInFill; ++n) {
                double onset = barStart + n * noteLen;
                if (onset >= totalBeats) break;
                int drum = fillDrums[fillDrum(rng)];
                int vel = velSnare(rng);
                // Crescendo through fill
                vel = std::min(127, vel + (n * 3));
                result.push_back({drum, onset, noteLen * 0.8, vel});
            }
            // Crash on next downbeat if not last bar
            if (bar < c.numBars - 1) {
                double nextBar = (bar + 1) * c.beatsPerBar;
                if (nextBar < totalBeats)
                    result.push_back({CRASH, nextBar, 1.0, 110});
            }
            continue;
        }

        // Normal pattern
        for (double beat = 0; beat < c.beatsPerBar; beat += 0.5) {
            double onset = barStart + beat;
            if (onset >= totalBeats) break;
            double swungOnset = applySwing(onset, c.swingAmount);

            bool isDownbeat = (std::abs(beat - std::round(beat)) < 0.01);
            int beatInt = (int)std::round(beat);

            // Kick
            if (style == 0) { // Rock: beats 1, 3
                if (beatInt == 0 || beatInt == 2) {
                    result.push_back({KICK, swungOnset, 0.4, velKick(rng)});
                }
            } else if (style == 1) { // Electronic: four on the floor
                if (isDownbeat) {
                    result.push_back({KICK, swungOnset, 0.4, velKick(rng)});
                }
                // Extra kick on "and of 3" sometimes
                if (std::abs(beat - 3.5) < 0.01 && densityCheck(rng) < c.density * 0.5) {
                    result.push_back({KICK, swungOnset, 0.3, velKick(rng) - 10});
                }
            } else { // Jazz: light kick on 1, feathered
                if (beatInt == 0 && isDownbeat) {
                    result.push_back({KICK, swungOnset, 0.3, std::min(80, velKick(rng) - 20)});
                }
            }

            // Snare
            if (beatInt == 1 || beatInt == 3) {
                if (isDownbeat) {
                    result.push_back({SNARE, swungOnset, 0.4, velSnare(rng)});
                }
                // Ghost notes before snare
                if (!isDownbeat && densityCheck(rng) < c.density * 0.4) {
                    result.push_back({SNARE, swungOnset, 0.2, velGhost(rng)});
                }
            }

            // Hi-hat
            if (style == 2) { // Jazz: ride pattern
                if (isDownbeat) {
                    result.push_back({RIDE, swungOnset, 0.4, velHat(rng)});
                } else if (densityCheck(rng) < c.density) {
                    result.push_back({RIDE, swungOnset, 0.3, velHat(rng) - 10});
                }
            } else {
                // Straight or swung hi-hat
                if (c.density > 0.3) {
                    bool open = (!isDownbeat && densityCheck(rng) < 0.15);
                    result.push_back({open ? HIHAT_OPEN : HIHAT_CLOSED,
                                     swungOnset, 0.3, velHat(rng)});
                } else if (isDownbeat) {
                    result.push_back({HIHAT_CLOSED, swungOnset, 0.3, velHat(rng)});
                }
            }
        }

        // Crash on bar 1
        if (bar == 0) {
            result.push_back({CRASH, barStart, 1.0, 100});
        }
    }

    return result;
}
