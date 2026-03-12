#include "MidiTrack.h"
#include <cmath>
#include <algorithm>

MidiTrack::MidiTrack(int id, const std::string& name, double sampleRate, int blockSize)
    : id(id), name(name), sampleRate(sampleRate), blockSize(blockSize) {
    instrumentOutputBuffer.setSize(2, blockSize);
}

MidiTrack::~MidiTrack() = default;

// ── MIDI data management ──

void MidiTrack::setMidiSequence(const juce::MidiMessageSequence& seq) {
    midiSequence = seq;
}

const juce::MidiMessageSequence& MidiTrack::getMidiSequence() const {
    return midiSequence;
}

juce::MidiMessageSequence& MidiTrack::getMidiSequenceMutable() {
    return midiSequence;
}

// ── Note editing ──

int MidiTrack::addNote(int noteNumber, double startBeat, double lengthBeats,
                       int velocity, int channel) {
    NoteEvent evt;
    evt.noteNumber = noteNumber;
    evt.startBeat = startBeat;
    evt.lengthBeats = lengthBeats;
    evt.velocity = juce::jlimit(1, 127, velocity);
    evt.channel = juce::jlimit(1, 16, channel);
    notes.push_back(evt);
    rebuildNoteSequence();
    return static_cast<int>(notes.size()) - 1;
}

void MidiTrack::removeNote(int noteIndex) {
    if (noteIndex >= 0 && noteIndex < static_cast<int>(notes.size())) {
        notes.erase(notes.begin() + noteIndex);
        rebuildNoteSequence();
    }
}

void MidiTrack::moveNote(int noteIndex, int newNoteNumber, double newStartBeat) {
    if (noteIndex >= 0 && noteIndex < static_cast<int>(notes.size())) {
        notes[noteIndex].noteNumber = juce::jlimit(0, 127, newNoteNumber);
        notes[noteIndex].startBeat = std::max(0.0, newStartBeat);
        rebuildNoteSequence();
    }
}

void MidiTrack::resizeNote(int noteIndex, double newLengthBeats) {
    if (noteIndex >= 0 && noteIndex < static_cast<int>(notes.size())) {
        notes[noteIndex].lengthBeats = std::max(0.001, newLengthBeats);
        rebuildNoteSequence();
    }
}

void MidiTrack::setNoteVelocity(int noteIndex, int velocity) {
    if (noteIndex >= 0 && noteIndex < static_cast<int>(notes.size())) {
        notes[noteIndex].velocity = juce::jlimit(1, 127, velocity);
        rebuildNoteSequence();
    }
}

void MidiTrack::addControlChange(int cc, int value, double beat, int channel) {
    auto msg = juce::MidiMessage::controllerEvent(
        juce::jlimit(1, 16, channel), cc, juce::jlimit(0, 127, value));
    msg.setTimeStamp(beat);
    midiSequence.addEvent(msg);
    midiSequence.sort();
}

void MidiTrack::quantizeNotes(double gridSizeBeats) {
    if (gridSizeBeats <= 0.0) return;
    for (auto& note : notes) {
        note.startBeat = std::round(note.startBeat / gridSizeBeats) * gridSizeBeats;
    }
    rebuildNoteSequence();
}

std::vector<MidiTrack::NoteInfo> MidiTrack::getAllNotes() const {
    std::vector<NoteInfo> result;
    result.reserve(notes.size());
    for (auto& n : notes) {
        NoteInfo info;
        info.noteNumber = n.noteNumber;
        info.startBeat = n.startBeat;
        info.lengthBeats = n.lengthBeats;
        info.velocity = n.velocity;
        info.channel = n.channel;
        result.push_back(info);
    }
    return result;
}

void MidiTrack::rebuildNoteSequence() {
    midiSequence.clear();
    for (auto& n : notes) {
        auto noteOn = juce::MidiMessage::noteOn(n.channel, n.noteNumber,
                                                 static_cast<juce::uint8>(n.velocity));
        noteOn.setTimeStamp(n.startBeat);
        auto noteOff = juce::MidiMessage::noteOff(n.channel, n.noteNumber);
        noteOff.setTimeStamp(n.startBeat + n.lengthBeats);
        midiSequence.addEvent(noteOn);
        midiSequence.addEvent(noteOff);
    }
    midiSequence.sort();
    midiSequence.updateMatchedPairs();
}

// ── Transport ──

void MidiTrack::prepareForPlayback(double sr, int bs) {
    sampleRate = sr;
    blockSize = bs;
    {
        juce::SpinLock::ScopedLockType lock(instrumentLock);
        if (instrumentProcessor) {
            instrumentProcessor->prepareToPlay(sr, bs);
            instrumentChannelCount = std::max(
                instrumentProcessor->getTotalNumInputChannels(),
                instrumentProcessor->getTotalNumOutputChannels());
            instrumentChannelCount = std::max(instrumentChannelCount, 2);
        }
        instrumentOutputBuffer.setSize(instrumentChannelCount, bs);
    }
    for (auto& fx : insertSlots) {
        if (fx) {
            fx->setPlayConfigDetails(2, 2, sr, bs);
            fx->prepareToPlay(sr, bs);
        }
    }
}

void MidiTrack::processBlock(juce::AudioBuffer<float>& output, int numSamples,
                             double currentTimeInSeconds, double bpm,
                             const juce::MidiBuffer& externalMidi) {
    // Try-lock: if instrument is being swapped on another thread, skip this block
    juce::SpinLock::ScopedTryLockType lock(instrumentLock);
    if (!lock.isLocked() || !instrumentProcessor) return;

    // Convert current time to beat position
    double beatsPerSecond = bpm / 60.0;
    double currentBeat = currentTimeInSeconds * beatsPerSecond;
    double secondsPerSample = 1.0 / sampleRate;

    // Fill midiBuffer with events in this block's time range
    midiBuffer.clear();

    double blockEndBeat = (currentTimeInSeconds + numSamples * secondsPerSample) * beatsPerSecond;

    for (int i = 0; i < midiSequence.getNumEvents(); i++) {
        auto* evt = midiSequence.getEventPointer(i);
        double eventBeat = evt->message.getTimeStamp();

        if (eventBeat >= currentBeat && eventBeat < blockEndBeat) {
            // Convert beat position to sample offset within block
            double eventTimeSec = eventBeat / beatsPerSecond;
            int sampleOffset = static_cast<int>((eventTimeSec - currentTimeInSeconds) * sampleRate);
            sampleOffset = juce::jlimit(0, numSamples - 1, sampleOffset);
            midiBuffer.addEvent(evt->message, sampleOffset);
        }
    }

    // Merge external MIDI (from controllers)
    for (const auto metadata : externalMidi) {
        midiBuffer.addEvent(metadata.getMessage(), metadata.samplePosition);
    }

    // Size buffer to match plugin's actual output channel count.
    // Multi-output plugins (Kontakt, etc.) write beyond 2 channels and
    // would SIGSEGV if the buffer is too small.
    int channels = std::max(2, instrumentChannelCount);
    instrumentOutputBuffer.setSize(channels, numSamples, false, false, true);
    instrumentOutputBuffer.clear();
    instrumentProcessor->processBlock(instrumentOutputBuffer, midiBuffer);

    // Route through effect chain (try-lock: skip if insert is being swapped)
    juce::MidiBuffer emptyMidi;
    {
        juce::SpinLock::ScopedTryLockType iLock(insertLock);
        if (iLock.isLocked()) {
            for (auto& fx : insertSlots) {
                if (fx) {
                    fx->processBlock(instrumentOutputBuffer, emptyMidi);
                }
            }
        }
    }

    // Apply volume/pan and mix first two channels to output
    float leftGain = volume * std::max(0.0f, 1.0f - pan);
    float rightGain = volume * std::max(0.0f, 1.0f + pan);

    float localPeak = 0.0f;
    float localRms = 0.0f;

    for (int i = 0; i < numSamples; i++) {
        float outL = instrumentOutputBuffer.getSample(0, i) * leftGain;
        float outR = (instrumentOutputBuffer.getNumChannels() > 1
                      ? instrumentOutputBuffer.getSample(1, i) : outL) * rightGain;

        output.addSample(0, i, outL);
        if (output.getNumChannels() > 1)
            output.addSample(1, i, outR);

        float absPeak = std::max(std::abs(outL), std::abs(outR));
        localPeak = std::max(localPeak, absPeak);
        localRms += outL * outL + outR * outR;
    }

    if (numSamples > 0)
        localRms = std::sqrt(localRms / (numSamples * 2));

    peakLevel.store(localPeak);
    rmsLevel.store(localRms);
}

// ── Instrument / Effects ──

void MidiTrack::setInstrument(std::unique_ptr<juce::AudioPluginInstance> instrument) {
    // Prepare BEFORE acquiring the lock (can be slow, SpinLock busy-waits).
    // Leave the plugin's default bus layout intact — forcing a layout change
    // can put complex plugins (Kontakt, etc.) into an invalid internal state.
    int newChannelCount = 2;
    if (instrument) {
        instrument->prepareToPlay(sampleRate, blockSize);
        newChannelCount = std::max(instrument->getTotalNumInputChannels(),
                                   instrument->getTotalNumOutputChannels());
        newChannelCount = std::max(newChannelCount, 2);
    }

    // Swap pointers under lock (fast, just pointer moves)
    std::unique_ptr<juce::AudioProcessor> old;
    {
        juce::SpinLock::ScopedLockType lock(instrumentLock);
        old = std::move(instrumentProcessor);
        instrumentProcessor = std::move(instrument);
        instrumentChannelCount = newChannelCount;
    }
    // old destroyed here, outside the lock
}

void MidiTrack::setInstrumentProcessor(std::unique_ptr<juce::AudioProcessor> builtIn) {
    if (builtIn) {
        builtIn->setPlayConfigDetails(0, 2, sampleRate, blockSize);
        builtIn->prepareToPlay(sampleRate, blockSize);
    }

    std::unique_ptr<juce::AudioProcessor> old;
    {
        juce::SpinLock::ScopedLockType lock(instrumentLock);
        old = std::move(instrumentProcessor);
        instrumentProcessor = std::move(builtIn);
        instrumentChannelCount = instrumentProcessor
            ? instrumentProcessor->getTotalNumOutputChannels() : 2;
    }
}

juce::AudioProcessor* MidiTrack::getInstrument() {
    return instrumentProcessor.get();
}

bool MidiTrack::hasInstrument() const {
    return instrumentProcessor != nullptr;
}

void MidiTrack::insertEffect(int slotIndex, std::unique_ptr<juce::AudioPluginInstance> plugin) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS) {
        plugin->setPlayConfigDetails(2, 2, sampleRate, blockSize);
        plugin->prepareToPlay(sampleRate, blockSize);
        std::unique_ptr<juce::AudioProcessor> old;
        {
            juce::SpinLock::ScopedLockType lock(insertLock);
            old = std::move(insertSlots[slotIndex]);
            insertSlots[slotIndex] = std::move(plugin);
        }
    }
}

void MidiTrack::insertBuiltInEffect(int slotIndex, std::unique_ptr<juce::AudioProcessor> effect) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS) {
        effect->prepareToPlay(sampleRate, blockSize);
        std::unique_ptr<juce::AudioProcessor> old;
        {
            juce::SpinLock::ScopedLockType lock(insertLock);
            old = std::move(insertSlots[slotIndex]);
            insertSlots[slotIndex] = std::move(effect);
        }
    }
}

void MidiTrack::removeInsert(int slotIndex) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS) {
        std::unique_ptr<juce::AudioProcessor> old;
        {
            juce::SpinLock::ScopedLockType lock(insertLock);
            old = std::move(insertSlots[slotIndex]);
        }
    }
}

void MidiTrack::removeEffect(int slotIndex) { removeInsert(slotIndex); }

juce::AudioProcessor* MidiTrack::getInsert(int slotIndex) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS)
        return insertSlots[slotIndex].get();
    return nullptr;
}

juce::AudioPluginInstance* MidiTrack::getEffect(int slotIndex) {
    return dynamic_cast<juce::AudioPluginInstance*>(getInsert(slotIndex));
}

bool MidiTrack::isInsertBuiltIn(int slotIndex) {
    auto* proc = getInsert(slotIndex);
    if (!proc) return false;
    return dynamic_cast<juce::AudioPluginInstance*>(proc) == nullptr;
}

// ── Mixer controls ──

void MidiTrack::setVolume(float v) { volume = juce::jlimit(0.0f, 1.0f, v); }
void MidiTrack::setPan(float p) { pan = juce::jlimit(-1.0f, 1.0f, p); }
void MidiTrack::setMute(bool m) { muted = m; }
void MidiTrack::setSolo(bool s) { soloed = s; }
void MidiTrack::setArmed(bool a) { armed = a; }

// ── Metering ──

float MidiTrack::getPeakLevel() const { return peakLevel.load(); }
float MidiTrack::getRMSLevel() const { return rmsLevel.load(); }

// ── Duration ──

double MidiTrack::getDuration(double bpm) const {
    if (notes.empty()) return 0.0;
    double maxEndBeat = 0.0;
    for (auto& n : notes) {
        maxEndBeat = std::max(maxEndBeat, n.startBeat + n.lengthBeats);
    }
    return maxEndBeat / (bpm / 60.0);
}
