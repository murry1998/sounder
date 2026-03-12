#include "MidiInputManager.h"
#include "AudioGraph.h"
#include <cstdio>

MidiInputManager::MidiInputManager() = default;

MidiInputManager::~MidiInputManager() {
    closeAllDevices();
}

std::vector<MidiInputManager::MidiDeviceInfo> MidiInputManager::getAvailableDevices() const {
    std::vector<MidiDeviceInfo> result;
    auto devices = juce::MidiInput::getAvailableDevices();
    for (auto& d : devices) {
        MidiDeviceInfo info;
        info.name = d.name.toStdString();
        info.identifier = d.identifier.toStdString();
        info.isOpen = openDevices.find(d.identifier.toStdString()) != openDevices.end();
        result.push_back(std::move(info));
    }
    return result;
}

bool MidiInputManager::openDevice(const std::string& identifier) {
    if (openDevices.find(identifier) != openDevices.end()) {
        fprintf(stderr, "[MIDI] Device already open: %s\n", identifier.c_str());
        return true;
    }

    auto devices = juce::MidiInput::getAvailableDevices();
    for (auto& d : devices) {
        if (d.identifier.toStdString() == identifier) {
            auto input = juce::MidiInput::openDevice(d.identifier, this);
            if (input) {
                input->start();
                openDevices[identifier] = std::move(input);
                fprintf(stderr, "[MIDI] Opened device: %s\n", d.name.toRawUTF8());
                return true;
            } else {
                fprintf(stderr, "[MIDI] Failed to open device: %s\n", d.name.toRawUTF8());
            }
        }
    }
    fprintf(stderr, "[MIDI] Device not found: %s\n", identifier.c_str());
    return false;
}

void MidiInputManager::closeDevice(const std::string& identifier) {
    auto it = openDevices.find(identifier);
    if (it != openDevices.end()) {
        it->second->stop();
        openDevices.erase(it);
    }
}

void MidiInputManager::closeAllDevices() {
    for (auto& [id, device] : openDevices)
        device->stop();
    openDevices.clear();
}

// ── Activity ──

bool MidiInputManager::hasRecentActivity() const {
    auto now = juce::Time::getMillisecondCounterHiRes();
    auto last = static_cast<double>(lastActivityTime.load());
    return (now - last) < 500.0; // active within 500ms
}

void MidiInputManager::resetActivity() {
    activityFlag.store(false);
    lastNote.store(-1);
    lastVelocity.store(0);
}

// ── MIDI Learn ──

void MidiInputManager::startMidiLearn(const std::string& paramPath) {
    std::lock_guard<std::mutex> lock(learnMutex);
    midiLearnParamPath = paramPath;
    midiLearnActive.store(true);
}

void MidiInputManager::stopMidiLearn() {
    std::lock_guard<std::mutex> lock(learnMutex);
    midiLearnActive.store(false);
    midiLearnParamPath.clear();
}

std::string MidiInputManager::getMidiLearnTarget() const {
    // Not ideal to lock in const, but safe enough for this use
    return midiLearnParamPath;
}

void MidiInputManager::addBinding(int cc, int channel, const std::string& paramPath) {
    std::lock_guard<std::mutex> lock(bindingsMutex);
    // Remove existing binding for same cc+channel
    bindings.erase(
        std::remove_if(bindings.begin(), bindings.end(),
            [cc, channel](const MidiBinding& b) { return b.cc == cc && b.channel == channel; }),
        bindings.end());
    bindings.push_back({cc, channel, paramPath});
}

void MidiInputManager::removeBinding(int cc, int channel) {
    std::lock_guard<std::mutex> lock(bindingsMutex);
    bindings.erase(
        std::remove_if(bindings.begin(), bindings.end(),
            [cc, channel](const MidiBinding& b) { return b.cc == cc && b.channel == channel; }),
        bindings.end());
}

std::vector<MidiInputManager::MidiBinding> MidiInputManager::getBindings() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(bindingsMutex));
    return bindings;
}

// ── Incoming MIDI ──

void MidiInputManager::handleIncomingMidiMessage(juce::MidiInput* /*source*/,
                                                   const juce::MidiMessage& message) {
    // Update activity
    lastActivityTime.store(static_cast<uint64_t>(juce::Time::getMillisecondCounterHiRes()));
    activityFlag.store(true);

    if (message.isNoteOn()) {
        lastNote.store(message.getNoteNumber());
        lastVelocity.store(message.getVelocity());
    }

    // MIDI Learn: capture CC
    if (midiLearnActive.load() && message.isController()) {
        std::lock_guard<std::mutex> lock(learnMutex);
        if (!midiLearnParamPath.empty()) {
            addBinding(message.getControllerNumber(), message.getChannel(), midiLearnParamPath);
            midiLearnActive.store(false);
            midiLearnParamPath.clear();
        }
        return; // consume the CC during learn
    }

    // Apply CC bindings
    if (message.isController()) {
        applyBinding(message.getControllerNumber(), message.getControllerValue(), message.getChannel());
    }

    // Forward to AudioGraph for instrument playback
    if (audioGraph) {
        audioGraph->feedExternalMidi(message, 0);
    } else {
        fprintf(stderr, "[MIDI] WARNING: audioGraph is null, cannot route MIDI\n");
    }
}

void MidiInputManager::applyBinding(int cc, int value, int channel) {
    std::lock_guard<std::mutex> lock(bindingsMutex);
    for (auto& b : bindings) {
        if (b.cc == cc && b.channel == channel) {
            // Parse paramPath "trackId:paramName"
            auto sep = b.paramPath.find(':');
            if (sep == std::string::npos) continue;
            // Binding application would go through the engine
            // For now, just store for the UI to query and apply
        }
    }
}
