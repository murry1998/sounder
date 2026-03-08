#include "AudioGraph.h"
#include "TransportEngine.h"
#include "Metronome.h"
#include <cmath>

AudioGraph::AudioGraph() = default;

AudioGraph::~AudioGraph() {
    closeDevice();
}

bool AudioGraph::openDevice(double sampleRate, int blockSize) {
    currentSampleRate = sampleRate;
    currentBlockSize = blockSize;

    auto result = deviceManager.initialise(
        2,     // stereo input
        2,     // stereo output
        nullptr, // no saved XML state
        true,    // select default device
        {},      // preferred device name (empty = default)
        nullptr  // preferred setup
    );

    if (result.isNotEmpty()) {
        DBG("AudioDeviceManager init error: " + result);
        return false;
    }

    deviceManager.addAudioCallback(this);
    return true;
}

void AudioGraph::closeDevice() {
    deviceManager.removeAudioCallback(this);
    deviceManager.closeAudioDevice();
}

bool AudioGraph::ensureInputEnabled() {
    auto* device = deviceManager.getCurrentAudioDevice();
    if (!device) return false;

    auto activeInputs = device->getActiveInputChannels();
    if (activeInputs.countNumberOfSetBits() > 0) return true;

    DBG("[Sounder] No active inputs - reopening device with input enabled");
    deviceManager.removeAudioCallback(this);

    auto result = deviceManager.initialise(2, 2, nullptr, true, {}, nullptr);
    if (result.isNotEmpty()) {
        DBG("[Sounder] Failed to reopen with input: " + result);
        deviceManager.addAudioCallback(this);
        return false;
    }

    deviceManager.addAudioCallback(this);
    device = deviceManager.getCurrentAudioDevice();
    if (device) {
        activeInputs = device->getActiveInputChannels();
        DBG("[Sounder] After reopen: " + juce::String(activeInputs.countNumberOfSetBits()) + " input channels");
        return activeInputs.countNumberOfSetBits() > 0;
    }
    return false;
}

// ── Audio tracks ──

int AudioGraph::addTrack(const std::string& name) {
    int id = nextTrackId++;
    tracks[id] = std::make_unique<AudioTrack>(id, name, currentSampleRate, currentBlockSize);
    return id;
}

void AudioGraph::removeTrack(int trackId) {
    tracks.erase(trackId);
}

AudioTrack* AudioGraph::getTrack(int trackId) {
    auto it = tracks.find(trackId);
    return (it != tracks.end()) ? it->second.get() : nullptr;
}

// ── MIDI tracks ──

int AudioGraph::addMidiTrack(const std::string& name) {
    int id = nextTrackId++;
    midiTracks[id] = std::make_unique<MidiTrack>(id, name, currentSampleRate, currentBlockSize);
    return id;
}

void AudioGraph::removeMidiTrack(int trackId) {
    midiTracks.erase(trackId);
}

MidiTrack* AudioGraph::getMidiTrack(int trackId) {
    auto it = midiTracks.find(trackId);
    return (it != midiTracks.end()) ? it->second.get() : nullptr;
}

// ── Bus tracks ──

int AudioGraph::addBusTrack(const std::string& name) {
    int id = nextBusId++;
    busTracks[id] = std::make_unique<BusTrack>(id, name, currentSampleRate, currentBlockSize);
    return id;
}

void AudioGraph::removeBusTrack(int busId) { busTracks.erase(busId); }

BusTrack* AudioGraph::getBusTrack(int busId) {
    auto it = busTracks.find(busId);
    return (it != busTracks.end()) ? it->second.get() : nullptr;
}

// ── External MIDI ──

void AudioGraph::feedExternalMidi(const juce::MidiMessage& msg, int sampleOffset) {
    std::lock_guard<std::mutex> lock(midiMutex);
    incomingMidiBuffer.addEvent(msg, sampleOffset);
}

// ── Master ──

void AudioGraph::setMasterVolume(float volume) {
    masterVolume = juce::jlimit(0.0f, 1.0f, volume);
}

float AudioGraph::getMasterVolume() const {
    return masterVolume;
}

// ── Master Inserts ──

void AudioGraph::insertMasterBuiltInEffect(int slot, std::unique_ptr<juce::AudioProcessor> effect) {
    if (slot >= 0 && slot < MAX_MASTER_INSERTS && effect) {
        effect->setPlayConfigDetails(2, 2, currentSampleRate, currentBlockSize);
        effect->prepareToPlay(currentSampleRate, currentBlockSize);
        masterInsertSlots[slot] = std::move(effect);
    }
}

void AudioGraph::insertMasterPlugin(int slot, std::unique_ptr<juce::AudioPluginInstance> plugin) {
    if (slot >= 0 && slot < MAX_MASTER_INSERTS && plugin) {
        plugin->setPlayConfigDetails(2, 2, currentSampleRate, currentBlockSize);
        plugin->prepareToPlay(currentSampleRate, currentBlockSize);
        masterInsertSlots[slot] = std::move(plugin);
    }
}

void AudioGraph::removeMasterInsert(int slot) {
    if (slot >= 0 && slot < MAX_MASTER_INSERTS)
        masterInsertSlots[slot].reset();
}

juce::AudioProcessor* AudioGraph::getMasterInsert(int slot) {
    if (slot >= 0 && slot < MAX_MASTER_INSERTS)
        return masterInsertSlots[slot].get();
    return nullptr;
}

bool AudioGraph::isMasterInsertBuiltIn(int slot) {
    auto* proc = getMasterInsert(slot);
    if (!proc) return false;
    return dynamic_cast<juce::AudioPluginInstance*>(proc) == nullptr;
}

double AudioGraph::getBPM() const {
    if (metronome) return static_cast<double>(metronome->getBPM());
    return 120.0;
}

MeterData AudioGraph::getMeterData() const {
    MeterData data{};
    data.masterPeakL = masterPeakL.load();
    data.masterPeakR = masterPeakR.load();
    data.masterRmsL = masterRmsL.load();
    data.masterRmsR = masterRmsR.load();

    for (auto& [id, track] : tracks) {
        TrackMeter tm;
        tm.trackId = id;
        float peak = track->getPeakLevel();
        float rms = track->getRMSLevel();
        tm.peakL = peak;
        tm.peakR = peak;
        tm.rmsL = rms;
        tm.rmsR = rms;
        tm.clipping = peak >= 1.0f;
        data.tracks.push_back(tm);
    }

    for (auto& [id, mtrack] : midiTracks) {
        TrackMeter tm;
        tm.trackId = id;
        float peak = mtrack->getPeakLevel();
        float rms = mtrack->getRMSLevel();
        tm.peakL = peak;
        tm.peakR = peak;
        tm.rmsL = rms;
        tm.rmsR = rms;
        tm.clipping = peak >= 1.0f;
        data.tracks.push_back(tm);
    }

    for (auto& [id, bus] : busTracks) {
        TrackMeter tm;
        tm.trackId = id;
        float peak = bus->getPeakLevel();
        float rms = bus->getRMSLevel();
        tm.peakL = peak; tm.peakR = peak;
        tm.rmsL = rms; tm.rmsR = rms;
        tm.clipping = peak >= 1.0f;
        data.busTracks.push_back(tm);
    }

    return data;
}

// ── Audio Device Enumeration / Config ──

std::vector<AudioGraph::DeviceInfo> AudioGraph::getAvailableDevices() {
    std::vector<DeviceInfo> result;
    auto& types = deviceManager.getAvailableDeviceTypes();
    for (auto* type : types) {
        type->scanForDevices();
        auto names = type->getDeviceNames(false); // output devices
        for (auto& name : names) {
            DeviceInfo info;
            info.name = name.toStdString();
            info.typeName = type->getTypeName().toStdString();
            // Open temporarily to query capabilities
            std::unique_ptr<juce::AudioIODevice> dev(
                type->createDevice(name, name));
            if (dev) {
                auto rates = dev->getAvailableSampleRates();
                for (auto r : rates) info.sampleRates.push_back(r);
                auto bufs = dev->getAvailableBufferSizes();
                for (auto b : bufs) info.bufferSizes.push_back(b);
                auto inNames = dev->getInputChannelNames();
                auto outNames = dev->getOutputChannelNames();
                info.numInputChannels = inNames.size();
                info.numOutputChannels = outNames.size();
                for (auto& n : inNames) info.inputChannelNames.push_back(n.toStdString());
                for (auto& n : outNames) info.outputChannelNames.push_back(n.toStdString());
            }
            result.push_back(std::move(info));
        }
    }
    return result;
}

AudioGraph::CurrentDeviceState AudioGraph::getCurrentDeviceState() const {
    CurrentDeviceState state;
    auto* dev = deviceManager.getCurrentAudioDevice();
    if (!dev) return state;

    state.name = dev->getName().toStdString();
    state.typeName = dev->getTypeName().toStdString();
    state.sampleRate = dev->getCurrentSampleRate();
    state.bufferSize = dev->getCurrentBufferSizeSamples();

    // Get the configured input device name from the setup (may differ from output)
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);
    state.inputDeviceName = setup.inputDeviceName.toStdString();

    auto inNames = dev->getInputChannelNames();
    auto outNames = dev->getOutputChannelNames();
    auto activeIn = dev->getActiveInputChannels();
    auto activeOut = dev->getActiveOutputChannels();
    state.activeInputs = activeIn.countNumberOfSetBits();
    state.activeOutputs = activeOut.countNumberOfSetBits();

    state.inputLatencyMs = dev->getInputLatencyInSamples() / state.sampleRate * 1000.0;
    state.outputLatencyMs = dev->getOutputLatencyInSamples() / state.sampleRate * 1000.0;

    for (auto& n : inNames) state.inputChannelNames.push_back(n.toStdString());
    for (auto& n : outNames) state.outputChannelNames.push_back(n.toStdString());

    return state;
}

bool AudioGraph::setAudioDevice(const std::string& deviceName, double sampleRate, int bufferSize) {
    return setAudioDeviceSeparate(deviceName, deviceName, sampleRate, bufferSize);
}

bool AudioGraph::setAudioDeviceSeparate(const std::string& outputName, const std::string& inputName,
                                         double sampleRate, int bufferSize) {
    deviceManager.removeAudioCallback(this);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);
    setup.outputDeviceName = outputName;
    setup.inputDeviceName = inputName;
    setup.sampleRate = sampleRate;
    setup.bufferSize = bufferSize;

    auto err = deviceManager.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty()) {
        DBG("setAudioDevice error: " + err);
        deviceManager.addAudioCallback(this);
        return false;
    }

    currentSampleRate = sampleRate;
    currentBlockSize = bufferSize;

    deviceManager.addAudioCallback(this);
    return true;
}

std::vector<AudioGraph::DeviceInfo> AudioGraph::getInputDevices() {
    std::vector<DeviceInfo> result;
    auto& types = deviceManager.getAvailableDeviceTypes();
    for (auto* type : types) {
        type->scanForDevices();
        auto names = type->getDeviceNames(true); // input devices
        for (auto& name : names) {
            DeviceInfo info;
            info.name = name.toStdString();
            info.typeName = type->getTypeName().toStdString();
            std::unique_ptr<juce::AudioIODevice> dev(
                type->createDevice({}, name));
            if (dev) {
                auto rates = dev->getAvailableSampleRates();
                for (auto r : rates) info.sampleRates.push_back(r);
                auto inNames = dev->getInputChannelNames();
                info.numInputChannels = inNames.size();
                for (auto& n : inNames) info.inputChannelNames.push_back(n.toStdString());
            }
            result.push_back(std::move(info));
        }
    }
    return result;
}

// ── AudioIODeviceCallback ──

void AudioGraph::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& /*context*/)
{
    // Clear output
    for (int ch = 0; ch < numOutputChannels; ch++)
        juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    if (!transport) return;

    // Read transport position BEFORE advancing so that notes at the exact
    // playback-start beat (e.g. beat 0) fall within the first block's range.
    double currentTime = transport->getCurrentTimeInSeconds();

    // Advance transport position
    transport->processBlock(numSamples);
    bool isPlaying = (transport->getState() == TransportEngine::Playing ||
                      transport->getState() == TransportEngine::Recording);

    // Check for solo'd tracks (across both audio and MIDI)
    bool anySolo = false;
    for (auto& [id, track] : tracks) {
        if (track->isSoloed()) { anySolo = true; break; }
    }
    if (!anySolo) {
        for (auto& [id, mtrack] : midiTracks) {
            if (mtrack->isSoloed()) { anySolo = true; break; }
        }
    }

    // Grab external MIDI and clear the incoming buffer
    juce::MidiBuffer externalMidi;
    {
        std::lock_guard<std::mutex> lock(midiMutex);
        externalMidi.swapWith(incomingMidiBuffer);
    }

    // Get BPM from metronome
    double bpm = metronome ? static_cast<double>(metronome->getBPM()) : 120.0;

    // Mix tracks into output
    if (isPlaying && numOutputChannels >= 2) {
        juce::AudioBuffer<float> outputBuffer(outputChannelData, numOutputChannels, numSamples);

        // Step 1: Clear bus input buffers
        for (auto& [bid, bus] : busTracks) {
            bus->clearInputBuffer(numSamples);
        }

        // Step 2: Process audio tracks (route to bus or master)
        for (auto& [id, track] : tracks) {
            if (track->isMuted()) continue;
            if (anySolo && !track->isSoloed()) continue;
            int outBus = track->getOutputBus();
            if (outBus >= 0) {
                auto bit = busTracks.find(outBus);
                if (bit != busTracks.end()) {
                    track->processBlock(bit->second->getInputBuffer(), numSamples, currentTime);
                    continue;
                }
            }
            track->processBlock(outputBuffer, numSamples, currentTime);
        }

        // Step 3: Process MIDI tracks
        // Route external MIDI only to armed tracks (for live play / recording)
        juce::MidiBuffer emptyMidiBuf;
        for (auto& [id, mtrack] : midiTracks) {
            if (mtrack->isMuted()) continue;
            if (anySolo && !mtrack->isSoloed()) continue;
            // Only armed tracks receive external MIDI input
            const juce::MidiBuffer& midiForTrack = mtrack->isArmed() ? externalMidi : emptyMidiBuf;
            int outBus = mtrack->getOutputBus();
            if (outBus >= 0) {
                auto bit = busTracks.find(outBus);
                if (bit != busTracks.end()) {
                    mtrack->processBlock(bit->second->getInputBuffer(), numSamples, currentTime, bpm, midiForTrack);
                    continue;
                }
            }
            mtrack->processBlock(outputBuffer, numSamples, currentTime, bpm, midiForTrack);
        }

        // Step 4: Process bus tracks (inserts + mix to master)
        for (auto& [bid, bus] : busTracks) {
            if (bus->isMuted()) continue;
            bus->processBlock(outputBuffer, numSamples);
        }

        // Metronome
        if (metronome) {
            metronome->processBlock(outputBuffer,
                transport->getCurrentPositionInSamples(), currentSampleRate);
        }

        // Master insert chain
        juce::MidiBuffer emptyMidiBuf2;
        for (auto& fx : masterInsertSlots) {
            if (fx) fx->processBlock(outputBuffer, emptyMidiBuf2);
        }

        // Apply master volume
        outputBuffer.applyGain(masterVolume);

        // Compute master meters
        float peakL = 0.0f, peakR = 0.0f;
        float rmsL = 0.0f, rmsR = 0.0f;
        for (int i = 0; i < numSamples; i++) {
            float sL = outputBuffer.getSample(0, i);
            float sR = outputBuffer.getSample(1, i);
            peakL = std::max(peakL, std::abs(sL));
            peakR = std::max(peakR, std::abs(sR));
            rmsL += sL * sL;
            rmsR += sR * sR;
        }
        rmsL = std::sqrt(rmsL / numSamples);
        rmsR = std::sqrt(rmsR / numSamples);
        masterPeakL.store(peakL);
        masterPeakR.store(peakR);
        masterRmsL.store(rmsL);
        masterRmsR.store(rmsR);
    }

    // Live MIDI monitoring: always process armed MIDI tracks when stopped
    // The instrument needs continuous processBlock calls to produce audio
    // (not just on callbacks that contain MIDI events)
    if (!isPlaying && numOutputChannels >= 2) {
        juce::AudioBuffer<float> outputBuffer(outputChannelData, numOutputChannels, numSamples);
        for (auto& [id, mtrack] : midiTracks) {
            if (!mtrack->isArmed()) continue;
            if (mtrack->isMuted()) continue;
            int outBus = mtrack->getOutputBus();
            if (outBus >= 0) {
                auto bit = busTracks.find(outBus);
                if (bit != busTracks.end()) {
                    mtrack->processBlock(bit->second->getInputBuffer(), numSamples, currentTime, bpm, externalMidi);
                    bit->second->processBlock(outputBuffer, numSamples);
                    continue;
                }
            }
            mtrack->processBlock(outputBuffer, numSamples, currentTime, bpm, externalMidi);
        }
        outputBuffer.applyGain(masterVolume);
    }

    // Feed recording input (works with mono or stereo mics)
    if (transport->getState() == TransportEngine::Recording && numInputChannels >= 1) {
        juce::AudioBuffer<float> inputBuffer(
            const_cast<float* const*>(inputChannelData), numInputChannels, numSamples);
        for (auto& [id, track] : tracks) {
            if (track->isArmed() && transport->isRecordingTrack(id)) {
                transport->feedRecordingInput(id, inputBuffer, numSamples);
            }
        }
    }

    // Feed MIDI recording input from external controllers
    if (transport->getState() == TransportEngine::Recording && !externalMidi.isEmpty()) {
        double beatsPerSecond = bpm / 60.0;
        double secondsPerSample = 1.0 / currentSampleRate;
        for (const auto metadata : externalMidi) {
            const auto& msg = metadata.getMessage();
            if (msg.isNoteOnOrOff()) {
                double eventTimeSec = currentTime + metadata.samplePosition * secondsPerSample;
                double beatPos = eventTimeSec * beatsPerSecond;
                for (auto& [id, mtrack] : midiTracks) {
                    if (mtrack->isArmed() && transport->isRecordingMidiTrack(id)) {
                        transport->feedRecordingMidi(id, msg, beatPos);
                    }
                }
            }
        }
    }
}

void AudioGraph::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    currentSampleRate = device->getCurrentSampleRate();
    currentBlockSize = device->getCurrentBufferSizeSamples();
}

void AudioGraph::audioDeviceStopped() {
    masterPeakL.store(0.0f);
    masterPeakR.store(0.0f);
    masterRmsL.store(0.0f);
    masterRmsR.store(0.0f);
}
