#include "AudioTrack.h"
#include "BeatQuantizer.h"
#include <cmath>
#include <algorithm>

AudioTrack::AudioTrack(int id, const std::string& name, double sampleRate, int blockSize)
    : id(id), name(name), sampleRate(sampleRate), blockSize(blockSize) {}

AudioTrack::~AudioTrack() = default;

void AudioTrack::setBuffer(std::unique_ptr<juce::AudioBuffer<float>> buffer, double fileSampleRate) {
    audioBuffer = std::move(buffer);
    bufferSampleRate = fileSampleRate;
}

bool AudioTrack::hasBuffer() const {
    return audioBuffer != nullptr && audioBuffer->getNumSamples() > 0;
}

std::vector<float> AudioTrack::getWaveformData(int numPoints) const {
    std::vector<float> result(numPoints, 0.0f);
    if (!hasBuffer()) return result;

    int totalSamples = audioBuffer->getNumSamples();
    float samplesPerPoint = static_cast<float>(totalSamples) / numPoints;

    for (int i = 0; i < numPoints; i++) {
        int start = static_cast<int>(i * samplesPerPoint);
        int end = std::min(static_cast<int>((i + 1) * samplesPerPoint), totalSamples);
        float maxVal = 0.0f;
        for (int s = start; s < end; s++) {
            float val = std::abs(audioBuffer->getSample(0, s));
            if (audioBuffer->getNumChannels() > 1)
                val = std::max(val, std::abs(audioBuffer->getSample(1, s)));
            maxVal = std::max(maxVal, val);
        }
        result[i] = maxVal;
    }
    return result;
}

double AudioTrack::getDuration() const {
    if (!hasBuffer()) return 0.0;
    return static_cast<double>(audioBuffer->getNumSamples()) / bufferSampleRate;
}

void AudioTrack::prepareForPlayback(double /*startTimeInSeconds*/) {
    isPlaying = true;
}

void AudioTrack::processBlock(juce::AudioBuffer<float>& output, int numSamples, double currentTime) {
    if (!hasBuffer()) return;

    int bufferLength = audioBuffer->getNumSamples();
    int numChannels = std::min(output.getNumChannels(), audioBuffer->getNumChannels());

    // Compute clip bounds in samples
    int clipStartSample = static_cast<int>(regionClipStart * bufferSampleRate);
    int clipEndSample = (regionClipEnd < 0.0)
        ? bufferLength
        : std::min(static_cast<int>(regionClipEnd * bufferSampleRate), bufferLength);
    int clipLength = clipEndSample - clipStartSample;
    if (clipLength <= 0) return;

    // How far into the region is the current playback position?
    double relativeTime = currentTime - regionOffset;
    if (relativeTime < 0.0) return; // region hasn't started yet

    int relativeSample = static_cast<int>(relativeTime * bufferSampleRate);

    // Without looping, check if we've gone past the clip
    if (!regionLoopEnabled && relativeSample >= clipLength) return;

    // Prepare plugin process buffer
    pluginProcessBuffer.setSize(2, numSamples, false, false, true);
    pluginProcessBuffer.clear();

    int samplesToRead = numSamples;
    for (int i = 0; i < samplesToRead; i++) {
        int sampleInClip;
        if (regionLoopEnabled) {
            sampleInClip = (relativeSample + i) % clipLength;
        } else {
            sampleInClip = relativeSample + i;
            if (sampleInClip >= clipLength) {
                samplesToRead = i;
                break;
            }
        }
        int bufIdx = clipStartSample + sampleInClip;
        if (bufIdx < 0 || bufIdx >= bufferLength) continue;

        float sL = audioBuffer->getSample(0, bufIdx);
        float sR = (numChannels > 1) ? audioBuffer->getSample(1, bufIdx) : sL;

        // Apply fade in/out envelope
        float fadeGain = 1.0f;
        if (fadeInDuration > 0.0) {
            int fadeInSamples = static_cast<int>(fadeInDuration * bufferSampleRate);
            if (sampleInClip < fadeInSamples)
                fadeGain *= static_cast<float>(sampleInClip) / static_cast<float>(fadeInSamples);
        }
        if (fadeOutDuration > 0.0) {
            int fadeOutSamples = static_cast<int>(fadeOutDuration * bufferSampleRate);
            int distFromEnd = clipLength - sampleInClip;
            if (distFromEnd < fadeOutSamples)
                fadeGain *= static_cast<float>(distFromEnd) / static_cast<float>(fadeOutSamples);
        }
        sL *= fadeGain;
        sR *= fadeGain;

        pluginProcessBuffer.setSample(0, i, sL);
        pluginProcessBuffer.setSample(1, i, sR);
    }

    // Route through insert chain
    emptyMidiBuffer.clear();
    for (int slot = 0; slot < MAX_INSERT_SLOTS; slot++) {
        if (insertSlots[slot]) {
            insertSlots[slot]->processBlock(pluginProcessBuffer, emptyMidiBuffer);
        }
    }

    // Apply volume/pan and mix to output
    float leftGain = volume * std::max(0.0f, 1.0f - pan);
    float rightGain = volume * std::max(0.0f, 1.0f + pan);

    float localPeak = 0.0f;
    float localRms = 0.0f;

    for (int i = 0; i < samplesToRead; i++) {
        float outL = pluginProcessBuffer.getSample(0, i) * leftGain;
        float outR = pluginProcessBuffer.getSample(1, i) * rightGain;

        output.addSample(0, i, outL);
        if (output.getNumChannels() > 1)
            output.addSample(1, i, outR);

        float absPeak = std::max(std::abs(outL), std::abs(outR));
        localPeak = std::max(localPeak, absPeak);
        localRms += outL * outL + outR * outR;
    }

    if (samplesToRead > 0)
        localRms = std::sqrt(localRms / (samplesToRead * 2));

    peakLevel.store(localPeak);
    rmsLevel.store(localRms);
}

void AudioTrack::stopPlayback() {
    isPlaying = false;
    peakLevel.store(0.0f);
    rmsLevel.store(0.0f);
}

void AudioTrack::setVolume(float v) { volume = juce::jlimit(0.0f, 1.0f, v); }
void AudioTrack::setPan(float p) { pan = juce::jlimit(-1.0f, 1.0f, p); }
void AudioTrack::setMute(bool m) { muted = m; }
void AudioTrack::setSolo(bool s) { soloed = s; }
void AudioTrack::setArmed(bool a) { armed = a; }

void AudioTrack::setRegionOffset(double t) { regionOffset = std::max(0.0, t); }
void AudioTrack::setRegionClipStart(double t) { regionClipStart = std::max(0.0, t); }
void AudioTrack::setRegionClipEnd(double t) { regionClipEnd = t; }
void AudioTrack::setRegionLoopEnabled(bool enabled) { regionLoopEnabled = enabled; }

std::unique_ptr<juce::AudioBuffer<float>> AudioTrack::extractBuffer(int startSample, int endSample) {
    if (!hasBuffer()) return nullptr;
    int total = audioBuffer->getNumSamples();
    startSample = std::max(0, startSample);
    endSample = std::min(endSample, total);
    int length = endSample - startSample;
    if (length <= 0) return nullptr;

    int ch = audioBuffer->getNumChannels();
    auto buf = std::make_unique<juce::AudioBuffer<float>>(ch, length);
    for (int c = 0; c < ch; c++) {
        buf->copyFrom(c, 0, *audioBuffer, c, startSample, length);
    }
    return buf;
}

void AudioTrack::trimBufferTo(int startSample, int endSample) {
    auto extracted = extractBuffer(startSample, endSample);
    if (extracted) {
        audioBuffer = std::move(extracted);
    }
}

void AudioTrack::setFxParam(const std::string& fxType, const std::string& param, float value) {
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

void AudioTrack::setFxEnabled(const std::string& fxType, bool enabled) {
    if (fxType == "eq") builtInFx.eqEnabled = enabled;
    else if (fxType == "compressor") builtInFx.compEnabled = enabled;
    else if (fxType == "delay") builtInFx.delayEnabled = enabled;
}

void AudioTrack::insertPlugin(int slotIndex, std::unique_ptr<juce::AudioPluginInstance> plugin) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS) {
        plugin->prepareToPlay(sampleRate, blockSize);
        insertSlots[slotIndex] = std::move(plugin);
    }
}

void AudioTrack::insertBuiltInEffect(int slotIndex, std::unique_ptr<juce::AudioProcessor> effect) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS) {
        effect->prepareToPlay(sampleRate, blockSize);
        insertSlots[slotIndex] = std::move(effect);
    }
}

void AudioTrack::removeInsert(int slotIndex) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS)
        insertSlots[slotIndex].reset();
}

void AudioTrack::removePlugin(int slotIndex) { removeInsert(slotIndex); }

juce::AudioProcessor* AudioTrack::getInsert(int slotIndex) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS)
        return insertSlots[slotIndex].get();
    return nullptr;
}

juce::AudioPluginInstance* AudioTrack::getPlugin(int slotIndex) {
    return dynamic_cast<juce::AudioPluginInstance*>(getInsert(slotIndex));
}

bool AudioTrack::isInsertBuiltIn(int slotIndex) {
    auto* proc = getInsert(slotIndex);
    if (!proc) return false;
    return dynamic_cast<juce::AudioPluginInstance*>(proc) == nullptr;
}

bool AudioTrack::quantizeAudio(const QuantizeOptions& options) {
    if (!hasBuffer() || options.bpm <= 0.0) return false;

    BeatQuantizer quantizer;
    auto result = quantizer.quantize(*audioBuffer, bufferSampleRate,
                                     options.bpm, options.gridDivision, options.strength);
    if (!result.success || !result.quantizedBuffer) return false;

    audioBuffer = std::move(result.quantizedBuffer);
    return true;
}

bool AudioTrack::tempoMatchAudio(double targetBPM, double& detectedBPM,
                                 double manualSourceBPM, double minBPM, double maxBPM) {
    if (!hasBuffer() || targetBPM <= 0.0) return false;

    BeatQuantizer quantizer;
    auto result = quantizer.tempoMatch(*audioBuffer, bufferSampleRate, targetBPM,
                                       manualSourceBPM, minBPM, maxBPM);
    detectedBPM = result.detectedBPM;

    if (!result.success || !result.stretchedBuffer) return false;

    audioBuffer = std::move(result.stretchedBuffer);
    return true;
}

float AudioTrack::getPeakLevel() const { return peakLevel.load(); }
float AudioTrack::getRMSLevel() const { return rmsLevel.load(); }
