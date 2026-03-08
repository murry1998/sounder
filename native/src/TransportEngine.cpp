#include "TransportEngine.h"

TransportEngine::TransportEngine() = default;
TransportEngine::~TransportEngine() = default;

void TransportEngine::play() {
    if (state.load() == Stopped || state.load() == Recording) {
        state.store(Playing);
    }
}

void TransportEngine::stop() {
    state.store(Stopped);
    // Don't clear recordingBuffers here — caller must finalize first
}

void TransportEngine::record(const std::vector<int>& armedTrackIds) {
    recordingBuffers.clear();
    for (int id : armedTrackIds) {
        recordingBuffers.try_emplace(id);
    }
    state.store(Recording);
}

void TransportEngine::rewind() {
    positionInSamples.store(0);
}

void TransportEngine::seekTo(double timeInSeconds) {
    int64_t pos = static_cast<int64_t>(timeInSeconds * sampleRate);
    positionInSamples.store(pos);
}

TransportEngine::State TransportEngine::getState() const {
    return state.load();
}

double TransportEngine::getCurrentTimeInSeconds() const {
    return static_cast<double>(positionInSamples.load()) / sampleRate;
}

int64_t TransportEngine::getCurrentPositionInSamples() const {
    return positionInSamples.load();
}

double TransportEngine::getTotalDuration() const {
    return totalDuration;
}

void TransportEngine::setTotalDuration(double dur) {
    totalDuration = dur;
}

void TransportEngine::setLoopEnabled(bool enabled) {
    loopEnabled = enabled;
}

void TransportEngine::setLoopRegion(double startSec, double endSec) {
    loopStartSample = static_cast<int64_t>(startSec * sampleRate);
    loopEndSample = static_cast<int64_t>(endSec * sampleRate);
}

void TransportEngine::processBlock(int numSamples) {
    auto currentState = state.load();
    if (currentState == Stopped) return;

    int64_t pos = positionInSamples.load();
    pos += numSamples;

    if (loopEnabled && loopEndSample > loopStartSample) {
        if (pos >= loopEndSample) {
            pos = loopStartSample + (pos - loopEndSample);
        }
    }

    positionInSamples.store(pos);
}

bool TransportEngine::isRecordingTrack(int trackId) const {
    return recordingBuffers.count(trackId) > 0;
}

void TransportEngine::feedRecordingInput(int trackId, const juce::AudioBuffer<float>& input, int numSamples) {
    auto it = recordingBuffers.find(trackId);
    if (it == recordingBuffers.end()) return;

    auto& buf = it->second;
    for (int i = 0; i < numSamples; i++) {
        buf.left.push_back(input.getSample(0, i));
        if (input.getNumChannels() > 1)
            buf.right.push_back(input.getSample(1, i));
        else
            buf.right.push_back(input.getSample(0, i));
    }
    buf.sampleCount.store(buf.left.size(), std::memory_order_release);
}

std::unique_ptr<juce::AudioBuffer<float>> TransportEngine::finalizeRecording(int trackId) {
    auto it = recordingBuffers.find(trackId);
    if (it == recordingBuffers.end()) return nullptr;

    auto& buf = it->second;
    int numSamples = static_cast<int>(buf.left.size());
    if (numSamples == 0) return nullptr;

    auto result = std::make_unique<juce::AudioBuffer<float>>(2, numSamples);
    for (int i = 0; i < numSamples; i++) {
        result->setSample(0, i, buf.left[i]);
        result->setSample(1, i, buf.right[i]);
    }

    recordingBuffers.erase(it);
    return result;
}

int TransportEngine::getRecordingSampleCount(int trackId) const {
    auto it = recordingBuffers.find(trackId);
    if (it == recordingBuffers.end()) return 0;
    return static_cast<int>(it->second.sampleCount.load(std::memory_order_acquire));
}

std::vector<float> TransportEngine::getRecordingWaveform(int trackId, int numPoints) const {
    std::vector<float> result(numPoints, 0.0f);
    auto it = recordingBuffers.find(trackId);
    if (it == recordingBuffers.end()) return result;

    const auto& buf = it->second;
    int totalSamples = static_cast<int>(buf.sampleCount.load(std::memory_order_acquire));
    if (totalSamples <= 0) return result;

    float samplesPerPoint = static_cast<float>(totalSamples) / numPoints;

    for (int i = 0; i < numPoints; i++) {
        int start = static_cast<int>(i * samplesPerPoint);
        int end = std::min(static_cast<int>((i + 1) * samplesPerPoint), totalSamples);
        float maxVal = 0.0f;
        for (int s = start; s < end; s++) {
            float valL = (s < static_cast<int>(buf.left.size())) ? std::abs(buf.left[s]) : 0.0f;
            float valR = (s < static_cast<int>(buf.right.size())) ? std::abs(buf.right[s]) : 0.0f;
            maxVal = std::max(maxVal, std::max(valL, valR));
        }
        result[i] = maxVal;
    }
    return result;
}

// ── MIDI Recording ──

void TransportEngine::recordMidiTracks(const std::vector<int>& armedMidiTrackIds) {
    for (int id : armedMidiTrackIds) {
        midiRecordingBuffers[id] = MidiRecordBuffer{};
    }
}

bool TransportEngine::isRecordingMidiTrack(int trackId) const {
    return midiRecordingBuffers.count(trackId) > 0;
}

void TransportEngine::feedRecordingMidi(int trackId, const juce::MidiMessage& msg, double beatPosition) {
    auto it = midiRecordingBuffers.find(trackId);
    if (it == midiRecordingBuffers.end()) return;

    auto& buf = it->second;
    int key = (msg.getChannel() << 8) | msg.getNoteNumber();

    if (msg.isNoteOn() && msg.getVelocity() > 0) {
        RecordedMidiNote note;
        note.noteNumber = msg.getNoteNumber();
        note.startBeat = beatPosition;
        note.endBeat = 0.0;
        note.velocity = msg.getVelocity();
        note.channel = msg.getChannel();
        buf.activeNotes[key] = note;
    }
    else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0)) {
        auto ait = buf.activeNotes.find(key);
        if (ait != buf.activeNotes.end()) {
            ait->second.endBeat = beatPosition;
            // Ensure minimum length
            if (ait->second.endBeat <= ait->second.startBeat)
                ait->second.endBeat = ait->second.startBeat + 0.0625; // 1/16 note min
            buf.completedNotes.push_back(ait->second);
            buf.activeNotes.erase(ait);
        }
    }
}

std::vector<TransportEngine::RecordedMidiNote> TransportEngine::finalizeMidiRecording(int trackId) {
    auto it = midiRecordingBuffers.find(trackId);
    if (it == midiRecordingBuffers.end()) return {};

    auto& buf = it->second;
    // Close any still-active notes at current position
    double currentBeat = (static_cast<double>(positionInSamples.load()) / sampleRate) * (120.0 / 60.0);
    for (auto& [key, note] : buf.activeNotes) {
        note.endBeat = currentBeat;
        if (note.endBeat <= note.startBeat)
            note.endBeat = note.startBeat + 0.0625;
        buf.completedNotes.push_back(note);
    }

    auto result = std::move(buf.completedNotes);
    midiRecordingBuffers.erase(it);
    return result;
}
