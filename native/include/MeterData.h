#pragma once

#include <vector>

struct TrackMeter {
    int trackId;
    float peakL;
    float peakR;
    float rmsL;
    float rmsR;
    bool clipping;
};

struct MeterData {
    float masterPeakL;
    float masterPeakR;
    float masterRmsL;
    float masterRmsR;
    std::vector<TrackMeter> tracks;
    std::vector<TrackMeter> busTracks;
    double currentTime;
    int currentBeat;
    const char* transportState;
    bool midiActivity = false;
    int lastMidiNote = -1;
    int lastMidiVelocity = 0;
};
