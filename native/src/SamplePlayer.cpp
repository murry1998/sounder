#include "SamplePlayer.h"
#include <cmath>

// ─── Sound ──────────────────────────────────────────────

SamplePlayerSound::SamplePlayerSound(juce::AudioBuffer<float>& buf,
                                      double sr, int root)
    : buffer(buf), fileSampleRate(sr), rootNote(root) {}

bool SamplePlayerSound::appliesToNote(int noteNumber) {
    return noteNumber >= lowNote && noteNumber <= highNote;
}

// ─── Voice ──────────────────────────────────────────────

SamplePlayerVoice::SamplePlayerVoice(SamplePlayerParams& p) : params(p) {}

bool SamplePlayerVoice::canPlaySound(juce::SynthesiserSound* sound) {
    return dynamic_cast<SamplePlayerSound*>(sound) != nullptr;
}

void SamplePlayerVoice::startNote(int midiNoteNumber, float velocity,
                                   juce::SynthesiserSound* sound, int) {
    currentSound = dynamic_cast<SamplePlayerSound*>(sound);
    if (!currentSound) return;

    // Pitch ratio: shift playback speed based on distance from root note
    double semitoneDiff = midiNoteNumber - currentSound->getRootNote();
    pitchRatio = std::pow(2.0, semitoneDiff / 12.0)
                 * (currentSound->getFileSampleRate() / getSampleRate());
    samplePosition = 0.0;

    // Velocity sensitivity: blend between full velocity and 1.0
    float sens = params.velocitySensitivity;
    noteVelocity = (1.0f - sens) + sens * velocity;

    juce::ADSR::Parameters adsrParams{params.ampAttack, params.ampDecay,
                                       params.ampSustain, params.ampRelease};
    adsr.setSampleRate(getSampleRate());
    adsr.setParameters(adsrParams);
    adsr.noteOn();
}

void SamplePlayerVoice::stopNote(float, bool allowTailOff) {
    if (allowTailOff) {
        adsr.noteOff();
    } else {
        adsr.reset();
        clearCurrentNote();
        currentSound = nullptr;
    }
}

void SamplePlayerVoice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                         int startSample, int numSamples) {
    if (!isVoiceActive() || !currentSound) return;

    // Update ADSR from live params each block (real-time control)
    adsr.setParameters({params.ampAttack, params.ampDecay,
                        params.ampSustain, params.ampRelease});

    auto& srcBuf = currentSound->getBuffer();
    int srcLen = srcBuf.getNumSamples();
    int srcCh = srcBuf.getNumChannels();

    for (int i = 0; i < numSamples; ++i) {
        int idx = static_cast<int>(samplePosition);
        if (idx >= srcLen - 1) {
            clearCurrentNote();
            currentSound = nullptr;
            break;
        }

        // Linear interpolation
        float frac = static_cast<float>(samplePosition - idx);
        float sL = srcBuf.getSample(0, idx) * (1.0f - frac) + srcBuf.getSample(0, idx + 1) * frac;
        float sR = srcCh > 1
            ? srcBuf.getSample(1, idx) * (1.0f - frac) + srcBuf.getSample(1, idx + 1) * frac
            : sL;

        float env = adsr.getNextSample();
        float gain = env * noteVelocity * 0.5f;

        outputBuffer.addSample(0, startSample + i, sL * gain);
        if (outputBuffer.getNumChannels() > 1)
            outputBuffer.addSample(1, startSample + i, sR * gain);

        samplePosition += pitchRatio;

        if (!adsr.isActive()) {
            clearCurrentNote();
            currentSound = nullptr;
            break;
        }
    }
}

// ─── Instrument ─────────────────────────────────────────

SamplePlayer::SamplePlayer() : BuiltInInstrument("Sample Player") {}

void SamplePlayer::prepareSynth(double sampleRate, int) {
    synth.clearVoices();
    synth.clearSounds();
    for (int i = 0; i < 16; ++i)
        synth.addVoice(new SamplePlayerVoice(params));
    synth.setCurrentPlaybackSampleRate(sampleRate);
    if (sampleLoaded) rebuildSound();
}

void SamplePlayer::rebuildSound() {
    synth.clearSounds();
    if (sampleBuffer.getNumSamples() > 0) {
        auto* sound = new SamplePlayerSound(sampleBuffer, fileSampleRate, params.rootNote);
        sound->lowNote = params.lowNote;
        sound->highNote = params.highNote;
        synth.addSound(sound);
    }
}

void SamplePlayer::loadSample(const std::string& filePath) {
    juce::File file(filePath);
    if (!file.existsAsFile()) return;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(file));
    if (!reader) return;

    sampleBuffer.setSize(static_cast<int>(reader->numChannels),
                         static_cast<int>(reader->lengthInSamples));
    reader->read(&sampleBuffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
    fileSampleRate = reader->sampleRate;
    sampleLoaded = true;
    rebuildSound();
}

void SamplePlayer::setParam(const std::string& name, float value) {
    if (name == "rootNote") { params.rootNote = static_cast<int>(value); rebuildSound(); }
    else if (name == "lowNote") { params.lowNote = static_cast<int>(value); rebuildSound(); }
    else if (name == "highNote") { params.highNote = static_cast<int>(value); rebuildSound(); }
    else if (name == "ampAttack") params.ampAttack = value;
    else if (name == "ampDecay") params.ampDecay = value;
    else if (name == "ampSustain") params.ampSustain = value;
    else if (name == "ampRelease") params.ampRelease = value;
    else if (name == "velocitySensitivity") params.velocitySensitivity = value;
}

float SamplePlayer::getParam(const std::string& name) const {
    if (name == "rootNote") return static_cast<float>(params.rootNote);
    if (name == "lowNote") return static_cast<float>(params.lowNote);
    if (name == "highNote") return static_cast<float>(params.highNote);
    if (name == "ampAttack") return params.ampAttack;
    if (name == "ampDecay") return params.ampDecay;
    if (name == "ampSustain") return params.ampSustain;
    if (name == "ampRelease") return params.ampRelease;
    if (name == "velocitySensitivity") return params.velocitySensitivity;
    return 0.0f;
}
