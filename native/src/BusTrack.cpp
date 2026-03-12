#include "BusTrack.h"
#include "BuiltInEffect.h"
#include <cmath>

BusTrack::BusTrack(int id, const std::string& name, double sampleRate, int blockSize)
    : id(id), name(name), sampleRate(sampleRate), blockSize(blockSize) {
    inputBuffer.setSize(2, blockSize);
}

BusTrack::~BusTrack() = default;

void BusTrack::clearInputBuffer(int numSamples) {
    if (inputBuffer.getNumSamples() < numSamples)
        inputBuffer.setSize(2, numSamples, false, false, true);
    inputBuffer.clear();
}

void BusTrack::processBlock(juce::AudioBuffer<float>& output, int numSamples) {
    // Process insert chain (try-lock: skip if insert is being swapped)
    emptyMidi.clear();
    {
        juce::SpinLock::ScopedTryLockType lock(insertLock);
        if (lock.isLocked()) {
            for (int i = 0; i < MAX_INSERT_SLOTS; ++i) {
                if (insertSlots[i])
                    insertSlots[i]->processBlock(inputBuffer, emptyMidi);
            }
        }
    }

    // Apply volume and pan, mix to output
    float leftGain = volume * std::max(0.0f, 1.0f - pan);
    float rightGain = volume * std::max(0.0f, 1.0f + pan);

    float localPeak = 0.0f, localRms = 0.0f;
    int samples = std::min(numSamples, inputBuffer.getNumSamples());

    for (int i = 0; i < samples; ++i) {
        float outL = inputBuffer.getSample(0, i) * leftGain;
        float outR = inputBuffer.getSample(1, i) * rightGain;
        output.addSample(0, i, outL);
        if (output.getNumChannels() > 1) output.addSample(1, i, outR);
        float absPeak = std::max(std::abs(outL), std::abs(outR));
        localPeak = std::max(localPeak, absPeak);
        localRms += outL * outL + outR * outR;
    }

    if (samples > 0) localRms = std::sqrt(localRms / (samples * 2));
    peakLevel.store(localPeak);
    rmsLevel.store(localRms);
}

void BusTrack::insertBuiltInEffect(int slot, std::unique_ptr<juce::AudioProcessor> effect) {
    if (slot >= 0 && slot < MAX_INSERT_SLOTS) {
        effect->prepareToPlay(sampleRate, blockSize);
        std::unique_ptr<juce::AudioProcessor> old;
        {
            juce::SpinLock::ScopedLockType lock(insertLock);
            old = std::move(insertSlots[slot]);
            insertSlots[slot] = std::move(effect);
        }
    }
}

void BusTrack::insertPlugin(int slot, std::unique_ptr<juce::AudioPluginInstance> plugin) {
    if (slot >= 0 && slot < MAX_INSERT_SLOTS) {
        plugin->prepareToPlay(sampleRate, blockSize);
        std::unique_ptr<juce::AudioProcessor> old;
        {
            juce::SpinLock::ScopedLockType lock(insertLock);
            old = std::move(insertSlots[slot]);
            insertSlots[slot] = std::move(plugin);
        }
    }
}

void BusTrack::removeInsert(int slot) {
    if (slot >= 0 && slot < MAX_INSERT_SLOTS) {
        std::unique_ptr<juce::AudioProcessor> old;
        {
            juce::SpinLock::ScopedLockType lock(insertLock);
            old = std::move(insertSlots[slot]);
        }
    }
}

juce::AudioProcessor* BusTrack::getInsert(int slot) {
    if (slot >= 0 && slot < MAX_INSERT_SLOTS) return insertSlots[slot].get();
    return nullptr;
}

bool BusTrack::isInsertBuiltIn(int slot) {
    if (slot >= 0 && slot < MAX_INSERT_SLOTS && insertSlots[slot]) {
        return dynamic_cast<juce::AudioPluginInstance*>(insertSlots[slot].get()) == nullptr;
    }
    return false;
}

void BusTrack::setVolume(float v) { volume = juce::jlimit(0.0f, 1.0f, v); }
void BusTrack::setPan(float p) { pan = juce::jlimit(-1.0f, 1.0f, p); }
void BusTrack::setMute(bool m) { muted = m; }
void BusTrack::setSolo(bool s) { soloed = s; }

void BusTrack::setFxParam(const std::string& fxType, const std::string& param, float value) {
    if (fxType == "eq") {
        if (param == "lowGain") builtInFx.eqLowGain = value;
        else if (param == "midGain") builtInFx.eqMidGain = value;
        else if (param == "midFreq") builtInFx.eqMidFreq = value;
        else if (param == "highGain") builtInFx.eqHighGain = value;
    } else if (fxType == "compressor") {
        if (param == "threshold") builtInFx.compThreshold = value;
        else if (param == "ratio") builtInFx.compRatio = value;
        else if (param == "attack") builtInFx.compAttack = value;
        else if (param == "release") builtInFx.compRelease = value;
    } else if (fxType == "delay") {
        if (param == "time") builtInFx.delayTime = value;
        else if (param == "mix") builtInFx.delayMix = value;
        else if (param == "feedback") builtInFx.delayFeedback = value;
    }
}

void BusTrack::setFxEnabled(const std::string& fxType, bool enabled) {
    if (fxType == "eq") builtInFx.eqEnabled = enabled;
    else if (fxType == "compressor") builtInFx.compEnabled = enabled;
    else if (fxType == "delay") builtInFx.delayEnabled = enabled;
}

float BusTrack::getPeakLevel() const { return peakLevel.load(); }
float BusTrack::getRMSLevel() const { return rmsLevel.load(); }
