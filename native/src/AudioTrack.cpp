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

double AudioTrack::appendBuffer(std::unique_ptr<juce::AudioBuffer<float>> newAudio, double newSampleRate) {
    if (!hasBuffer()) {
        setBuffer(std::move(newAudio), newSampleRate);
        return 0.0;
    }

    int existingSamples = audioBuffer->getNumSamples();
    int existingChannels = audioBuffer->getNumChannels();
    double appendOffset = static_cast<double>(existingSamples) / bufferSampleRate;

    // Resample new audio to match existing buffer sample rate if needed
    int newSamples = newAudio->getNumSamples();
    int newChannels = newAudio->getNumChannels();
    int outChannels = std::max(existingChannels, newChannels);

    std::unique_ptr<juce::AudioBuffer<float>> resampledNew;
    if (std::abs(newSampleRate - bufferSampleRate) > 1.0) {
        double ratio = bufferSampleRate / newSampleRate;
        int resampledLength = static_cast<int>(newSamples * ratio);
        resampledNew = std::make_unique<juce::AudioBuffer<float>>(newChannels, resampledLength);
        for (int ch = 0; ch < newChannels; ch++) {
            const float* src = newAudio->getReadPointer(ch);
            float* dst = resampledNew->getWritePointer(ch);
            for (int i = 0; i < resampledLength; i++) {
                double srcPos = i / ratio;
                int idx = static_cast<int>(srcPos);
                float frac = static_cast<float>(srcPos - idx);
                if (idx + 1 < newSamples)
                    dst[i] = src[idx] * (1.0f - frac) + src[idx + 1] * frac;
                else if (idx < newSamples)
                    dst[i] = src[idx];
                else
                    dst[i] = 0.0f;
            }
        }
        newSamples = resampledLength;
    } else {
        resampledNew = std::move(newAudio);
    }

    // Create combined buffer
    int totalSamples = existingSamples + newSamples;
    auto combined = std::make_unique<juce::AudioBuffer<float>>(outChannels, totalSamples);
    combined->clear();

    // Copy existing audio
    for (int ch = 0; ch < outChannels; ch++) {
        int srcCh = std::min(ch, existingChannels - 1);
        combined->copyFrom(ch, 0, *audioBuffer, srcCh, 0, existingSamples);
    }

    // Copy new audio after existing
    for (int ch = 0; ch < outChannels; ch++) {
        int srcCh = std::min(ch, resampledNew->getNumChannels() - 1);
        combined->copyFrom(ch, existingSamples, *resampledNew, srcCh, 0, newSamples);
    }

    audioBuffer = std::move(combined);
    return appendOffset;
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

    // Compute total playable length (accounting for loop count)
    int totalPlayLength = clipLength;
    if (regionLoopEnabled && regionLoopCount > 0) {
        totalPlayLength = clipLength * (regionLoopCount + 1);
    }

    // Without looping, or past finite loop end, stop
    if (!regionLoopEnabled && relativeSample >= clipLength) return;
    if (regionLoopEnabled && regionLoopCount > 0 && relativeSample >= totalPlayLength) return;

    // Prepare plugin process buffer
    pluginProcessBuffer.setSize(2, numSamples, false, false, true);
    pluginProcessBuffer.clear();

    int samplesToRead = numSamples;
    for (int i = 0; i < samplesToRead; i++) {
        int sampleInClip;
        if (regionLoopEnabled) {
            int pos = relativeSample + i;
            // Stop at loop count boundary
            if (regionLoopCount > 0 && pos >= totalPlayLength) {
                samplesToRead = i;
                break;
            }
            sampleInClip = pos % clipLength;
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

    // Route through insert chain (try-lock: skip if insert is being swapped)
    emptyMidiBuffer.clear();
    {
        juce::SpinLock::ScopedTryLockType lock(insertLock);
        if (lock.isLocked()) {
            for (int slot = 0; slot < MAX_INSERT_SLOTS; slot++) {
                if (insertSlots[slot]) {
                    insertSlots[slot]->processBlock(pluginProcessBuffer, emptyMidiBuffer);
                }
            }
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
void AudioTrack::setRegionLoopCount(int count) { regionLoopCount = std::max(0, count); }

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
        std::unique_ptr<juce::AudioProcessor> old;
        {
            juce::SpinLock::ScopedLockType lock(insertLock);
            old = std::move(insertSlots[slotIndex]);
            insertSlots[slotIndex] = std::move(plugin);
        }
        // old destroyed outside lock
    }
}

void AudioTrack::insertBuiltInEffect(int slotIndex, std::unique_ptr<juce::AudioProcessor> effect) {
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

void AudioTrack::removeInsert(int slotIndex) {
    if (slotIndex >= 0 && slotIndex < MAX_INSERT_SLOTS) {
        std::unique_ptr<juce::AudioProcessor> old;
        {
            juce::SpinLock::ScopedLockType lock(insertLock);
            old = std::move(insertSlots[slotIndex]);
        }
    }
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

bool AudioTrack::transposeAudio(const TransposeOptions& options) {
    if (!hasBuffer() || options.semitones == 0) return true; // no-op is success
    if (options.semitones < -24 || options.semitones > 24) return false;

    double pitchRatio = std::pow(2.0, options.semitones / 12.0);
    int numChannels = audioBuffer->getNumChannels();
    int originalLength = audioBuffer->getNumSamples();

    // Step 1: Resample to change pitch (new length = oldLength / pitchRatio)
    int resampledLength = static_cast<int>(originalLength / pitchRatio);
    if (resampledLength <= 0) return false;

    auto resampled = std::make_unique<juce::AudioBuffer<float>>(numChannels, resampledLength);
    for (int ch = 0; ch < numChannels; ch++) {
        const float* src = audioBuffer->getReadPointer(ch);
        float* dst = resampled->getWritePointer(ch);
        for (int i = 0; i < resampledLength; i++) {
            double srcPos = i * pitchRatio;
            int idx = static_cast<int>(srcPos);
            float frac = static_cast<float>(srcPos - idx);
            if (idx + 1 < originalLength)
                dst[i] = src[idx] * (1.0f - frac) + src[idx + 1] * frac;
            else if (idx < originalLength)
                dst[i] = src[idx];
            else
                dst[i] = 0.0f;
        }
    }

    if (options.preserveTempo) {
        // Step 2: Time-stretch back to original length using WSOLA
        BeatQuantizer quantizer;
        auto stretched = quantizer.wsolaStretch(*resampled, originalLength);
        audioBuffer = std::make_unique<juce::AudioBuffer<float>>(std::move(stretched));
    } else {
        // Speed change mode: just use resampled buffer
        audioBuffer = std::move(resampled);
    }

    return true;
}

bool AudioTrack::normalizeAudio(float targetPeakDb) {
    if (!hasBuffer()) return false;

    int numChannels = audioBuffer->getNumChannels();
    int numSamples = audioBuffer->getNumSamples();
    if (numSamples == 0) return false;

    // Find peak amplitude across all channels
    float currentPeak = 0.0f;
    for (int ch = 0; ch < numChannels; ch++) {
        const float* data = audioBuffer->getReadPointer(ch);
        for (int i = 0; i < numSamples; i++) {
            float absVal = std::abs(data[i]);
            if (absVal > currentPeak) currentPeak = absVal;
        }
    }

    if (currentPeak < 1e-8f) return false; // silence, can't normalize

    // Calculate target peak from dB
    float targetPeak = std::pow(10.0f, targetPeakDb / 20.0f);
    float gain = targetPeak / currentPeak;

    // Apply gain
    audioBuffer->applyGain(gain);
    return true;
}

float AudioTrack::getPeakLevel() const { return peakLevel.load(); }
float AudioTrack::getRMSLevel() const { return rmsLevel.load(); }
