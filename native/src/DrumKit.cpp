#include "DrumKit.h"
#include <cmath>

// ─── Voice ──────────────────────────────────────────────

DrumKitVoice::DrumKitVoice(std::array<DrumPad, 16>& p) : pads(p) {}

bool DrumKitVoice::canPlaySound(juce::SynthesiserSound* sound) {
    return dynamic_cast<DrumKitSound*>(sound) != nullptr;
}

void DrumKitVoice::startNote(int midiNoteNumber, float velocity,
                              juce::SynthesiserSound*, int) {
    int padIdx = midiNoteNumber - 36;
    if (padIdx < 0 || padIdx >= 16 || !pads[padIdx].loaded) {
        clearCurrentNote();
        return;
    }

    activePadIndex = padIdx;
    samplePosition = 0.0;
    playbackRate = pads[padIdx].sampleRate / getSampleRate();

    float sens = pads[padIdx].velocitySensitivity;
    noteVelocity = (1.0f - sens) + sens * velocity;
}

void DrumKitVoice::stopNote(float, bool) {
    // One-shot: ignore note-off, just let the sample play out
    // But we still need to clear if forced
    if (!isVoiceActive()) clearCurrentNote();
}

void DrumKitVoice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                    int startSample, int numSamples) {
    if (!isVoiceActive() || activePadIndex < 0) return;

    auto& pad = pads[activePadIndex];
    if (!pad.loaded) { clearCurrentNote(); return; }

    auto& srcBuf = pad.sample;
    int srcLen = srcBuf.getNumSamples();
    int srcCh = srcBuf.getNumChannels();
    float gain = pad.volume * noteVelocity * 0.5f;

    for (int i = 0; i < numSamples; ++i) {
        int idx = static_cast<int>(samplePosition);
        if (idx >= srcLen - 1) {
            clearCurrentNote();
            activePadIndex = -1;
            break;
        }

        float frac = static_cast<float>(samplePosition - idx);
        float sL = srcBuf.getSample(0, idx) * (1.0f - frac) + srcBuf.getSample(0, idx + 1) * frac;
        float sR = srcCh > 1
            ? srcBuf.getSample(1, idx) * (1.0f - frac) + srcBuf.getSample(1, idx + 1) * frac
            : sL;

        outputBuffer.addSample(0, startSample + i, sL * gain);
        if (outputBuffer.getNumChannels() > 1)
            outputBuffer.addSample(1, startSample + i, sR * gain);

        samplePosition += playbackRate;
    }
}

// ─── DrumKit instrument ─────────────────────────────────

DrumKit::DrumKit() : BuiltInInstrument("Drum Kit") {}

void DrumKit::prepareSynth(double sampleRate, int) {
    synth.clearVoices();
    synth.clearSounds();
    synth.addSound(new DrumKitSound());
    // 16 voices so every pad can play simultaneously
    for (int i = 0; i < 16; ++i)
        synth.addVoice(new DrumKitVoice(pads));
    synth.setCurrentPlaybackSampleRate(sampleRate);
}

void DrumKit::loadPadSample(int padIndex, const std::string& filePath) {
    if (padIndex < 0 || padIndex >= 16) return;

    juce::File file(filePath);
    if (!file.existsAsFile()) return;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(file));
    if (!reader) return;

    auto& pad = pads[padIndex];
    pad.sample.setSize(static_cast<int>(reader->numChannels),
                       static_cast<int>(reader->lengthInSamples));
    reader->read(&pad.sample, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
    pad.sampleRate = reader->sampleRate;
    pad.loaded = true;
}

void DrumKit::setParam(const std::string& name, float value) {
    // Params are formatted as "padN_volume" or "padN_velSens" where N is 0-15
    if (name.substr(0, 3) == "pad") {
        auto underscore = name.find('_');
        if (underscore == std::string::npos) return;
        int padIdx = std::stoi(name.substr(3, underscore - 3));
        if (padIdx < 0 || padIdx >= 16) return;
        std::string param = name.substr(underscore + 1);
        if (param == "volume") pads[padIdx].volume = value;
        else if (param == "velSens") pads[padIdx].velocitySensitivity = value;
    }
}

float DrumKit::getParam(const std::string& name) const {
    if (name.substr(0, 3) == "pad") {
        auto underscore = name.find('_');
        if (underscore == std::string::npos) return 0.0f;
        int padIdx = std::stoi(name.substr(3, underscore - 3));
        if (padIdx < 0 || padIdx >= 16) return 0.0f;
        std::string param = name.substr(underscore + 1);
        if (param == "volume") return pads[padIdx].volume;
        if (param == "velSens") return pads[padIdx].velocitySensitivity;
    }
    return 0.0f;
}
