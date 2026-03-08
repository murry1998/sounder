#include "SFZInstrument.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include <random>
#include <numeric>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ═══════════════════════════════════════════════════════════
// Section A: SFZ Parser
// ═══════════════════════════════════════════════════════════

int SFZParser::parseNoteNumber(const std::string& s) {
    // Try numeric first
    try { return std::stoi(s); } catch (...) {}
    // Note name: c4, c#4, db4 etc
    if (s.size() < 2) return 60;
    static const std::map<char, int> noteMap = {
        {'c',0},{'d',2},{'e',4},{'f',5},{'g',7},{'a',9},{'b',11}
    };
    char letter = std::tolower(s[0]);
    auto it = noteMap.find(letter);
    if (it == noteMap.end()) return 60;
    int note = it->second;
    size_t idx = 1;
    if (idx < s.size() && s[idx] == '#') { note++; idx++; }
    else if (idx < s.size() && s[idx] == 'b') { note--; idx++; }
    int octave = 4;
    if (idx < s.size()) {
        try { octave = std::stoi(s.substr(idx)); } catch (...) {}
    }
    return (octave + 1) * 12 + note;
}

void SFZParser::applyOpcode(SFZRegion& region, const std::string& key, const std::string& value) {
    if (key == "sample") region.samplePath = value;
    else if (key == "lokey") region.lokey = parseNoteNumber(value);
    else if (key == "hikey") region.hikey = parseNoteNumber(value);
    else if (key == "pitch_keycenter" || key == "key") region.pitchKeycenter = parseNoteNumber(value);
    else if (key == "lovel") region.lovel = std::stoi(value);
    else if (key == "hivel") region.hivel = std::stoi(value);
    else if (key == "volume") region.volume = std::stof(value);
    else if (key == "pan") region.pan = std::stof(value);
    else if (key == "transpose") region.transpose = std::stoi(value);
    else if (key == "tune") region.tune = std::stoi(value);
    else if (key == "ampeg_attack") region.ampegAttack = std::stof(value);
    else if (key == "ampeg_decay") region.ampegDecay = std::stof(value);
    else if (key == "ampeg_sustain") region.ampegSustain = std::stof(value) / 100.0f;
    else if (key == "ampeg_release") region.ampegRelease = std::stof(value);
    else if (key == "loop_start") region.loopStart = std::stoi(value);
    else if (key == "loop_end") region.loopEnd = std::stoi(value);
    else if (key == "loop_mode") {
        if (value == "loop_continuous") region.loopMode = 1;
        else if (value == "loop_sustain") region.loopMode = 2;
        else region.loopMode = 0;
    }
}

std::vector<SFZRegion> SFZParser::parse(const std::string& sfzContent, const std::string& basePath) {
    std::vector<SFZRegion> regions;
    SFZRegion groupDefaults;
    SFZRegion* current = nullptr;
    bool inRegion = false;

    std::istringstream stream(sfzContent);
    std::string line;

    while (std::getline(stream, line)) {
        // Strip comments
        auto commentPos = line.find("//");
        if (commentPos != std::string::npos) line = line.substr(0, commentPos);

        // Trim
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Check for headers
        if (line.find("<group>") != std::string::npos) {
            groupDefaults = SFZRegion();
            current = &groupDefaults;
            inRegion = false;
            // Parse opcodes on same line after <group>
            auto after = line.find("<group>");
            line = line.substr(after + 7);
        }
        if (line.find("<region>") != std::string::npos) {
            regions.push_back(groupDefaults);
            current = &regions.back();
            inRegion = true;
            auto after = line.find("<region>");
            line = line.substr(after + 8);
        }

        if (!current) continue;

        // Parse key=value pairs
        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            auto eq = token.find('=');
            if (eq == std::string::npos) continue;
            std::string key = token.substr(0, eq);
            std::string value = token.substr(eq + 1);
            if (key == "sample" && !basePath.empty() && !value.empty() && value[0] != '/') {
                value = basePath + "/" + value;
            }
            applyOpcode(*current, key, value);
        }
    }

    return regions;
}

// ═══════════════════════════════════════════════════════════
// Section B: SFZSound + SFZVoice
// ═══════════════════════════════════════════════════════════

bool SFZVoice::canPlaySound(juce::SynthesiserSound* sound) {
    return dynamic_cast<SFZSound*>(sound) != nullptr;
}

void SFZVoice::startNote(int midiNoteNumber, float velocity,
                          juce::SynthesiserSound* sound, int) {
    // Find the best velocity-layer match among all sounds that apply to this note
    SFZSound* bestMatch = nullptr;
    int vel = static_cast<int>(velocity * 127.0f);

    // Check if the provided sound matches this velocity
    auto* provided = dynamic_cast<SFZSound*>(sound);
    if (provided) {
        const auto& reg = provided->getRegion();
        if (vel >= reg.lovel && vel <= reg.hivel) {
            bestMatch = provided;
        }
    }

    // If no velocity match, fall back to the provided sound anyway
    if (!bestMatch) bestMatch = provided;

    if (!bestMatch) return;

    currentSFZSound = bestMatch;
    const auto& region = currentSFZSound->getRegion();
    auto& sampleBuf = currentSFZSound->getSampleBuffer();

    // Calculate pitch ratio
    double semitoneDiff = midiNoteNumber - region.pitchKeycenter + region.transpose + region.tune / 100.0;
    pitchRatio = std::pow(2.0, semitoneDiff / 12.0)
                 * (sampleBuf.sampleRate / getSampleRate());

    samplePosition = 0.0;

    // Volume: convert dB to linear, apply velocity with sensitivity
    float linearVol = std::pow(10.0f, region.volume / 20.0f);
    float velSens = owner ? owner->getLiveVelocitySensitivity() : 0.8f;
    noteGain = linearVol * (1.0f - velSens + velSens * velocity);

    // Pan: convert -100..+100 to L/R gains
    float panNorm = (region.pan + 100.0f) / 200.0f; // 0..1
    panLeft = std::cos(panNorm * static_cast<float>(M_PI) * 0.5f);
    panRight = std::sin(panNorm * static_cast<float>(M_PI) * 0.5f);

    // ADSR: use live params from instrument for real-time control
    juce::ADSR::Parameters adsrParams;
    if (owner) {
        adsrParams = { owner->getLiveAttack(), owner->getLiveDecay(),
                       owner->getLiveSustain(), owner->getLiveRelease() };
    } else {
        adsrParams = { region.ampegAttack, region.ampegDecay,
                       region.ampegSustain, region.ampegRelease };
    }
    adsr.setSampleRate(getSampleRate());
    adsr.setParameters(adsrParams);
    adsr.noteOn();

    // Looping
    looping = (region.loopMode == 1 || region.loopMode == 2);
    loopStartSample = region.loopStart;
    loopEndSample = region.loopEnd >= 0 ? region.loopEnd : sampleBuf.buffer.getNumSamples() - 1;
}

void SFZVoice::stopNote(float, bool allowTailOff) {
    if (allowTailOff) {
        adsr.noteOff();
    } else {
        adsr.reset();
        clearCurrentNote();
        currentSFZSound = nullptr;
    }
}

void SFZVoice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                int startSample, int numSamples) {
    if (!isVoiceActive() || !currentSFZSound) return;

    // Update ADSR from live params each block (real-time control)
    if (owner) {
        juce::ADSR::Parameters adsrParams{
            owner->getLiveAttack(), owner->getLiveDecay(),
            owner->getLiveSustain(), owner->getLiveRelease()
        };
        adsr.setParameters(adsrParams);
    }

    float masterVol = owner ? owner->getLiveMasterVolume() : 1.0f;

    auto& srcBuf = currentSFZSound->getSampleBuffer().buffer;
    int srcLen = srcBuf.getNumSamples();
    int srcCh = srcBuf.getNumChannels();

    for (int i = 0; i < numSamples; ++i) {
        int idx = static_cast<int>(samplePosition);

        if (idx >= srcLen - 1) {
            if (looping && loopEndSample > loopStartSample) {
                samplePosition = loopStartSample + std::fmod(samplePosition - loopStartSample,
                    static_cast<double>(loopEndSample - loopStartSample));
                idx = static_cast<int>(samplePosition);
            } else {
                clearCurrentNote();
                currentSFZSound = nullptr;
                break;
            }
        }

        // Linear interpolation
        float frac = static_cast<float>(samplePosition - idx);
        int idx2 = std::min(idx + 1, srcLen - 1);
        float sL = srcBuf.getSample(0, idx) * (1.0f - frac) + srcBuf.getSample(0, idx2) * frac;
        float sR = srcCh > 1
            ? srcBuf.getSample(1, idx) * (1.0f - frac) + srcBuf.getSample(1, idx2) * frac
            : sL;

        float env = adsr.getNextSample();
        float gain = env * noteGain * masterVol * 0.4f;

        outputBuffer.addSample(0, startSample + i, sL * gain * panLeft);
        if (outputBuffer.getNumChannels() > 1)
            outputBuffer.addSample(1, startSample + i, sR * gain * panRight);

        samplePosition += pitchRatio;

        // Handle loop wrap during playback
        if (looping && samplePosition >= loopEndSample && loopEndSample > loopStartSample) {
            samplePosition = loopStartSample + std::fmod(samplePosition - loopStartSample,
                static_cast<double>(loopEndSample - loopStartSample));
        }

        if (!adsr.isActive()) {
            clearCurrentNote();
            currentSFZSound = nullptr;
            break;
        }
    }
}

// ═══════════════════════════════════════════════════════════
// Section C: SFZInstrument
// ═══════════════════════════════════════════════════════════

SFZInstrument::SFZInstrument() : BuiltInInstrument("SFZ Instrument") {}

void SFZInstrument::prepareSynth(double sampleRate, int) {
    cachedSampleRate = sampleRate;
    synth.setCurrentPlaybackSampleRate(sampleRate);
    if (!regions.empty()) rebuildSynth();
}

void SFZInstrument::rebuildSynth() {
    synth.clearVoices();
    synth.clearSounds();

    for (size_t i = 0; i < regions.size(); ++i) {
        auto& region = regions[i];
        if (region.sampleIndex >= 0 && region.sampleIndex < static_cast<int>(sampleBuffers.size())) {
            synth.addSound(new SFZSound(region, *sampleBuffers[region.sampleIndex]));
        }
    }

    for (int i = 0; i < 24; ++i) {
        auto* voice = new SFZVoice();
        voice->setOwner(this);
        synth.addVoice(voice);
    }

    synth.setCurrentPlaybackSampleRate(cachedSampleRate);

    // Initialize live ADSR from first region
    if (!regions.empty()) {
        liveAttack = regions[0].ampegAttack;
        liveDecay = regions[0].ampegDecay;
        liveSustain = regions[0].ampegSustain;
        liveRelease = regions[0].ampegRelease;
    }
}

void SFZInstrument::loadRegionsAndSamples(const std::string& sfzContent,
                                            const std::string& basePath) {
    regions = SFZParser::parse(sfzContent, basePath);
    sampleBuffers.clear();

    // Deduplicate sample paths
    std::map<std::string, int> pathToIndex;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    for (auto& region : regions) {
        if (region.samplePath.empty()) continue;

        auto it = pathToIndex.find(region.samplePath);
        if (it != pathToIndex.end()) {
            region.sampleIndex = it->second;
            continue;
        }

        juce::File file(region.samplePath);
        if (!file.existsAsFile()) {
            fprintf(stderr, "[SFZ] Sample not found: %s\n", region.samplePath.c_str());
            continue;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(file));
        if (!reader) {
            fprintf(stderr, "[SFZ] Cannot read sample: %s\n", region.samplePath.c_str());
            continue;
        }

        auto buf = std::make_unique<SFZSampleBuffer>();
        buf->buffer.setSize(static_cast<int>(reader->numChannels),
                            static_cast<int>(reader->lengthInSamples));
        reader->read(&buf->buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
        buf->sampleRate = reader->sampleRate;
        buf->path = region.samplePath;

        int idx = static_cast<int>(sampleBuffers.size());
        pathToIndex[region.samplePath] = idx;
        region.sampleIndex = idx;
        sampleBuffers.push_back(std::move(buf));
    }

    rebuildSynth();
}

void SFZInstrument::loadPreset(const std::string& presetId) {
    const auto& presets = SFZSampleGenerator::getPresetDefinitions();
    const SFZPresetDefinition* found = nullptr;
    for (const auto& p : presets) {
        if (p.id == presetId) { found = &p; break; }
    }
    if (!found) {
        fprintf(stderr, "[SFZ] Unknown preset: %s\n", presetId.c_str());
        return;
    }

    // Get cache directory
    auto cacheBase = juce::File::getSpecialLocation(
        juce::File::userHomeDirectory).getChildFile(".sounder/instruments");
    auto presetDir = cacheBase.getChildFile(presetId);
    std::string dirPath = presetDir.getFullPathName().toStdString();

    // Generate samples if needed
    auto marker = presetDir.getChildFile(".generated");
    if (!marker.existsAsFile() && found->generator) {
        presetDir.createDirectory();
        found->generator(dirPath, cachedSampleRate > 0 ? cachedSampleRate : 44100.0);
        marker.create();
    }

    currentPresetId = presetId;
    instrumentName = found->name;
    loadRegionsAndSamples(found->sfzContent, dirPath);
}

void SFZInstrument::loadSFZFile(const std::string& filePath) {
    juce::File file(filePath);
    if (!file.existsAsFile()) return;

    std::string content = file.loadFileAsString().toStdString();
    std::string basePath = file.getParentDirectory().getFullPathName().toStdString();

    currentPresetId = "";
    instrumentName = file.getFileNameWithoutExtension().toStdString();
    loadRegionsAndSamples(content, basePath);
}

void SFZInstrument::setParam(const std::string& name, float value) {
    if (name == "masterVolume") masterVolume = value;
    else if (name == "velocitySensitivity") velocitySensitivity = value;
    else if (name == "ampegAttack") {
        liveAttack = value;
        for (auto& r : regions) r.ampegAttack = value;
    }
    else if (name == "ampegDecay") {
        liveDecay = value;
        for (auto& r : regions) r.ampegDecay = value;
    }
    else if (name == "ampegSustain") {
        liveSustain = value;
        for (auto& r : regions) r.ampegSustain = value;
    }
    else if (name == "ampegRelease") {
        liveRelease = value;
        for (auto& r : regions) r.ampegRelease = value;
    }
}

float SFZInstrument::getParam(const std::string& name) const {
    if (name == "masterVolume") return masterVolume;
    if (name == "velocitySensitivity") return velocitySensitivity;
    if (name == "ampegAttack" && !regions.empty()) return regions[0].ampegAttack;
    if (name == "ampegDecay" && !regions.empty()) return regions[0].ampegDecay;
    if (name == "ampegSustain" && !regions.empty()) return regions[0].ampegSustain;
    if (name == "ampegRelease" && !regions.empty()) return regions[0].ampegRelease;
    return 0.0f;
}

// ═══════════════════════════════════════════════════════════
// Section D: DSP Helpers
// ═══════════════════════════════════════════════════════════

void SFZSampleGenerator::writeWav(const std::string& path,
                                    const juce::AudioBuffer<float>& buf, double sr) {
    juce::File file(path);
    file.deleteFile();
    auto stream = file.createOutputStream();
    if (!stream) return;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.release(), sr,
            static_cast<unsigned int>(buf.getNumChannels()), 16, {}, 0));
    if (writer) {
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
    }
}

void SFZSampleGenerator::generateAdditiveTone(juce::AudioBuffer<float>& buf,
                                                double sr, double freq,
                                                const std::vector<float>& harmonicAmps,
                                                float duration, float attack, float decay,
                                                float sustainLevel) {
    int numSamples = static_cast<int>(sr * duration);
    buf.setSize(1, numSamples);
    buf.clear();

    float* data = buf.getWritePointer(0);
    double nyquist = sr * 0.5;

    for (int i = 0; i < numSamples; ++i) {
        double t = i / sr;
        float sample = 0.0f;

        for (size_t h = 0; h < harmonicAmps.size(); ++h) {
            double harmFreq = freq * (h + 1);
            if (harmFreq >= nyquist) break;
            sample += harmonicAmps[h] * static_cast<float>(std::sin(2.0 * M_PI * harmFreq * t));
        }

        // Envelope: attack ramp, then exponential decay to sustainLevel
        float env = 1.0f;
        if (t < attack) {
            env = static_cast<float>(t / attack);
        } else {
            float elapsed = static_cast<float>(t - attack);
            env = sustainLevel + (1.0f - sustainLevel) * std::exp(-elapsed / std::max(decay, 0.001f));
        }

        data[i] = sample * env;
    }

    // Normalize
    float peak = buf.getMagnitude(0, numSamples);
    if (peak > 0.001f) buf.applyGain(0.9f / peak);
}

void SFZSampleGenerator::generateFMTone(juce::AudioBuffer<float>& buf,
                                          double sr, double freq,
                                          double modRatio, double modIndexStart,
                                          double modIndexEnd, float duration,
                                          float attack, float decay) {
    int numSamples = static_cast<int>(sr * duration);
    buf.setSize(1, numSamples);
    buf.clear();

    float* data = buf.getWritePointer(0);
    double modFreq = freq * modRatio;

    for (int i = 0; i < numSamples; ++i) {
        double t = i / sr;

        // Modulation index decays over time
        float progress = static_cast<float>(t / duration);
        double modIndex = modIndexStart + (modIndexEnd - modIndexStart) * progress;

        double modulator = std::sin(2.0 * M_PI * modFreq * t);
        double carrier = std::sin(2.0 * M_PI * freq * t + modIndex * modulator);

        // Envelope
        float env = 1.0f;
        if (t < attack) {
            env = static_cast<float>(t / attack);
        } else {
            float elapsed = static_cast<float>(t - attack);
            env = std::exp(-elapsed / std::max(decay, 0.001f));
        }

        data[i] = static_cast<float>(carrier) * env;
    }

    float peak = buf.getMagnitude(0, numSamples);
    if (peak > 0.001f) buf.applyGain(0.9f / peak);
}

void SFZSampleGenerator::generateKarplusStrong(juce::AudioBuffer<float>& buf,
                                                 double sr, double freq,
                                                 float duration, float damping) {
    int numSamples = static_cast<int>(sr * duration);
    buf.setSize(1, numSamples);
    buf.clear();

    int delayLen = static_cast<int>(sr / freq);
    if (delayLen < 2) delayLen = 2;

    std::vector<float> delayLine(delayLen);
    std::mt19937 rng(static_cast<unsigned int>(freq * 1000));
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& s : delayLine) s = dist(rng);

    float* data = buf.getWritePointer(0);
    int pos = 0;

    for (int i = 0; i < numSamples; ++i) {
        float current = delayLine[pos];
        int next = (pos + 1) % delayLen;
        float averaged = 0.5f * (delayLine[pos] + delayLine[next]) * damping;
        delayLine[pos] = averaged;
        data[i] = current;
        pos = next;
    }

    float peak = buf.getMagnitude(0, numSamples);
    if (peak > 0.001f) buf.applyGain(0.9f / peak);
}

void SFZSampleGenerator::generateFilteredNoise(juce::AudioBuffer<float>& buf,
                                                  double sr, double cutoff,
                                                  float resonance, float duration,
                                                  float attack, float decay) {
    int numSamples = static_cast<int>(sr * duration);
    buf.setSize(1, numSamples);
    buf.clear();

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    float* data = buf.getWritePointer(0);

    // Simple 2-pole lowpass filter
    double w0 = 2.0 * M_PI * cutoff / sr;
    double alpha = std::sin(w0) / (2.0 * resonance);
    double b0 = (1.0 - std::cos(w0)) / 2.0;
    double b1 = 1.0 - std::cos(w0);
    double b2 = b0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * std::cos(w0);
    double a2 = 1.0 - alpha;

    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    for (int i = 0; i < numSamples; ++i) {
        double t = i / sr;
        double x0 = dist(rng);

        double y0 = (b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;

        float env = 1.0f;
        if (t < attack) {
            env = static_cast<float>(t / attack);
        } else {
            float elapsed = static_cast<float>(t - attack);
            env = std::exp(-elapsed / std::max(decay, 0.001f));
        }

        data[i] = static_cast<float>(y0) * env;
    }

    float peak = buf.getMagnitude(0, numSamples);
    if (peak > 0.001f) buf.applyGain(0.9f / peak);
}

// ═══════════════════════════════════════════════════════════
// Section E: Instrument Generators
// ═══════════════════════════════════════════════════════════

void SFZSampleGenerator::generateGrandPiano(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    // Notes: C2(36), C3(48), C4(60), C5(72), C6(84)
    // Two velocity layers each
    int notes[] = {36, 48, 60, 72, 84};
    const char* noteNames[] = {"c2", "c3", "c4", "c5", "c6"};

    for (int n = 0; n < 5; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);

        // Soft layer: fewer harmonics
        {
            std::vector<float> harmonics = {1.0f, 0.45f, 0.25f, 0.12f, 0.06f, 0.03f};
            juce::AudioBuffer<float> buf;
            generateAdditiveTone(buf, sr, freq, harmonics, 3.0f, 0.005f, 1.2f, 0.0f);
            writeWav(dir + "/piano_" + noteNames[n] + "_soft.wav", buf, sr);
        }
        // Hard layer: brighter
        {
            std::vector<float> harmonics = {1.0f, 0.65f, 0.45f, 0.30f, 0.20f, 0.12f, 0.07f, 0.04f};
            juce::AudioBuffer<float> buf;
            generateAdditiveTone(buf, sr, freq, harmonics, 3.0f, 0.003f, 0.9f, 0.0f);
            writeWav(dir + "/piano_" + noteNames[n] + "_hard.wav", buf, sr);
        }
    }
}

void SFZSampleGenerator::generateElectricPiano(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {36, 48, 60, 72, 84};
    const char* noteNames[] = {"c2", "c3", "c4", "c5", "c6"};
    std::mt19937 rng(5555);
    std::uniform_real_distribution<float> noiseDist(-1.0f, 1.0f);

    for (int n = 0; n < 5; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        int numSamples = static_cast<int>(sr * 2.5);

        // Soft: less modulation, warmer
        {
            juce::AudioBuffer<float> buf(1, numSamples);
            buf.clear();
            float* data = buf.getWritePointer(0);
            double detune[] = {-2.0, 2.0};

            for (int i = 0; i < numSamples; ++i) {
                double t = i / sr;
                float sample = 0.0f;

                for (int osc = 0; osc < 2; ++osc) {
                    double f = freq * std::pow(2.0, detune[osc] / 1200.0);
                    // FM with modRatio 7.0 (more musical than 14.0)
                    double modIdx = 1.5 * std::exp(-t * 3.0) + 0.2;
                    double mod = modIdx * std::sin(2.0 * M_PI * f * 7.0 * t);
                    // Second modulator at ratio 1.0 for warmth
                    double mod2 = 0.3 * std::exp(-t * 2.0) * std::sin(2.0 * M_PI * f * 1.0 * t);
                    sample += static_cast<float>(std::sin(2.0 * M_PI * f * t + mod + mod2));
                }
                sample *= 0.5f;

                // Tine transient: short noise burst at onset
                float tine = (t < 0.002) ? noiseDist(rng) * static_cast<float>(1.0 - t / 0.002) * 0.15f : 0.0f;

                // ADSR envelope
                float env = 1.0f;
                if (t < 0.002) env = static_cast<float>(t / 0.002);
                else env = std::exp(static_cast<float>(-t * 1.2));

                data[i] = (sample + tine) * env;
            }

            float peak = buf.getMagnitude(0, numSamples);
            if (peak > 0.001f) buf.applyGain(0.9f / peak);
            writeWav(dir + "/epiano_" + noteNames[n] + "_soft.wav", buf, sr);
        }

        // Hard: more modulation, brighter attack
        {
            juce::AudioBuffer<float> buf(1, numSamples);
            buf.clear();
            float* data = buf.getWritePointer(0);
            double detune[] = {-2.0, 2.0};

            for (int i = 0; i < numSamples; ++i) {
                double t = i / sr;
                float sample = 0.0f;

                for (int osc = 0; osc < 2; ++osc) {
                    double f = freq * std::pow(2.0, detune[osc] / 1200.0);
                    double modIdx = 3.5 * std::exp(-t * 4.0) + 0.4;
                    double mod = modIdx * std::sin(2.0 * M_PI * f * 7.0 * t);
                    double mod2 = 0.5 * std::exp(-t * 2.5) * std::sin(2.0 * M_PI * f * 1.0 * t);
                    sample += static_cast<float>(std::sin(2.0 * M_PI * f * t + mod + mod2));
                }
                sample *= 0.5f;

                // Stronger tine bark on hard velocity
                float tine = (t < 0.003) ? noiseDist(rng) * static_cast<float>(1.0 - t / 0.003) * 0.25f : 0.0f;

                float env = 1.0f;
                if (t < 0.001) env = static_cast<float>(t / 0.001);
                else env = std::exp(static_cast<float>(-t * 1.0));

                data[i] = (sample + tine) * env;
            }

            float peak = buf.getMagnitude(0, numSamples);
            if (peak > 0.001f) buf.applyGain(0.9f / peak);
            writeWav(dir + "/epiano_" + noteNames[n] + "_hard.wav", buf, sr);
        }
    }
}

void SFZSampleGenerator::generatePipeOrgan(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {36, 48, 60, 72};
    const char* noteNames[] = {"c2", "c3", "c4", "c5"};

    for (int n = 0; n < 4; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        // Drawbar-style: fundamental + sub-harmonics typical of organ
        // 16', 8', 5-1/3', 4', 2-2/3', 2', 1-3/5', 1-1/3', 1'
        std::vector<float> harmonics = {0.8f, 1.0f, 0.7f, 0.8f, 0.5f, 0.6f, 0.3f, 0.4f, 0.3f};

        juce::AudioBuffer<float> buf;
        // Sustained tone (will be looped)
        generateAdditiveTone(buf, sr, freq, harmonics, 2.0f, 0.02f, 100.0f, 0.95f);
        writeWav(dir + "/organ_" + noteNames[n] + ".wav", buf, sr);
    }
}

void SFZSampleGenerator::generateStringEnsemble(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {36, 48, 60, 72, 84};
    const char* noteNames[] = {"c2", "c3", "c4", "c5", "c6"};

    for (int n = 0; n < 5; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        int numSamples = static_cast<int>(sr * 3.0);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);

        // 4 detuned sawtooth oscillators
        double detuneCents[] = {-7.0, -3.0, 3.0, 7.0};

        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float sample = 0.0f;

            for (int d = 0; d < 4; ++d) {
                double detuned = freq * std::pow(2.0, detuneCents[d] / 1200.0);
                // Add slight vibrato
                double vib = 1.0 + 0.002 * std::sin(2.0 * M_PI * 5.0 * t + d * 1.5);
                double f = detuned * vib;
                double phase = std::fmod(f * t, 1.0);
                sample += static_cast<float>(2.0 * phase - 1.0); // sawtooth
            }
            sample *= 0.25f;

            // Slow attack envelope
            float env = 1.0f;
            if (t < 0.5) env = static_cast<float>(t / 0.5);
            // Keep sustained for looping
            env *= 0.95f;

            data[i] = sample * env;
        }

        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);

        writeWav(dir + "/strings_" + noteNames[n] + ".wav", buf, sr);
    }
}

void SFZSampleGenerator::generateBrassSection(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {48, 60, 72};
    const char* noteNames[] = {"c3", "c4", "c5"};

    for (int n = 0; n < 3; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);

        // Soft: less modulation, mellower
        {
            juce::AudioBuffer<float> buf;
            generateFMTone(buf, sr, freq, 1.0, 1.5, 0.8, 2.0f, 0.03f, 0.8f);
            writeWav(dir + "/brass_" + noteNames[n] + "_soft.wav", buf, sr);
        }
        // Hard: more mod index, brighter
        {
            juce::AudioBuffer<float> buf;
            generateFMTone(buf, sr, freq, 1.0, 4.0, 1.5, 2.0f, 0.015f, 0.6f);
            writeWav(dir + "/brass_" + noteNames[n] + "_hard.wav", buf, sr);
        }
    }
}

void SFZSampleGenerator::generateFingeredBass(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {28, 36, 43, 48};
    const char* noteNames[] = {"e1", "c2", "g2", "c3"};

    for (int n = 0; n < 4; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        std::vector<float> harmonics = {1.0f, 0.5f, 0.15f, 0.05f};
        juce::AudioBuffer<float> buf;
        generateAdditiveTone(buf, sr, freq, harmonics, 2.0f, 0.005f, 0.4f, 0.0f);
        writeWav(dir + "/bass_" + noteNames[n] + ".wav", buf, sr);
    }
}

void SFZSampleGenerator::generateNylonGuitar(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {40, 48, 55, 60, 67};
    const char* noteNames[] = {"e2", "c3", "g3", "c4", "g4"};

    for (int n = 0; n < 5; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        juce::AudioBuffer<float> buf;
        generateKarplusStrong(buf, sr, freq, 3.0f, 0.996f);
        writeWav(dir + "/guitar_" + noteNames[n] + ".wav", buf, sr);
    }
}

void SFZSampleGenerator::generateSynthPad(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {36, 48, 60, 72};
    const char* noteNames[] = {"c2", "c3", "c4", "c5"};

    for (int n = 0; n < 4; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        int numSamples = static_cast<int>(sr * 3.0);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);

        // 7 detuned sawtooth oscillators (super-saw)
        double detuneCents[] = {-15.0, -10.0, -5.0, 0.0, 5.0, 10.0, 15.0};

        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float sample = 0.0f;

            for (int d = 0; d < 7; ++d) {
                double detuned = freq * std::pow(2.0, detuneCents[d] / 1200.0);
                double phase = std::fmod(detuned * t, 1.0);
                sample += static_cast<float>(2.0 * phase - 1.0);
            }
            sample /= 7.0f;

            // Gentle lowpass: running average
            static float prev = 0.0f;
            if (i == 0) prev = 0.0f;
            sample = prev * 0.7f + sample * 0.3f;
            prev = sample;

            // Slow attack
            float env = 1.0f;
            if (t < 0.8) env = static_cast<float>(t / 0.8);

            data[i] = sample * env * 0.9f;
        }

        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);

        writeWav(dir + "/pad_" + noteNames[n] + ".wav", buf, sr);
    }
}

void SFZSampleGenerator::generateSFZDrumKit(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();

    // Kick drum (note 36): sine sweep from 150Hz to 50Hz
    {
        int numSamples = static_cast<int>(sr * 0.5);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 50.0 + 100.0 * std::exp(-t * 15.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 8.0));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/kick.wav", buf, sr);
    }

    // Snare drum (note 38): noise + sine transient
    {
        int numSamples = static_cast<int>(sr * 0.3);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng) * std::exp(static_cast<float>(-t * 12.0));
            float tone = static_cast<float>(std::sin(2.0 * M_PI * 200.0 * t))
                         * std::exp(static_cast<float>(-t * 20.0));
            data[i] = noise * 0.6f + tone * 0.4f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/snare.wav", buf, sr);
    }

    // Closed hi-hat (note 42): filtered noise, short
    {
        juce::AudioBuffer<float> buf;
        generateFilteredNoise(buf, sr, 8000.0, 1.5f, 0.15f, 0.001f, 0.04f);
        writeWav(dir + "/hihat_closed.wav", buf, sr);
    }

    // Open hi-hat (note 46): filtered noise, longer
    {
        juce::AudioBuffer<float> buf;
        generateFilteredNoise(buf, sr, 7000.0, 1.2f, 0.5f, 0.001f, 0.2f);
        writeWav(dir + "/hihat_open.wav", buf, sr);
    }

    // Crash cymbal (note 49): wide filtered noise
    {
        juce::AudioBuffer<float> buf;
        generateFilteredNoise(buf, sr, 5000.0, 0.8f, 1.5f, 0.001f, 0.6f);
        writeWav(dir + "/crash.wav", buf, sr);
    }
}

void SFZSampleGenerator::generate808Kit(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();

    // 808 Kick (note 36): long sine sweep from ~160Hz to ~40Hz with sub-bass sustain
    {
        int numSamples = static_cast<int>(sr * 1.2);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 40.0 + 120.0 * std::exp(-t * 12.0);
            phase += freq / sr;
            float click = (t < 0.003) ? static_cast<float>(std::sin(2.0 * M_PI * 3000.0 * t) * (1.0 - t / 0.003)) : 0.0f;
            float env = std::exp(static_cast<float>(-t * 3.5));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env + click * 0.3f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/808_kick.wav", buf, sr);
    }

    // 808 Snare (note 38): tuned oscillator + noise burst
    {
        int numSamples = static_cast<int>(sr * 0.4);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        std::mt19937 rng(808);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 180.0 + 40.0 * std::exp(-t * 30.0);
            phase += freq / sr;
            float tone = static_cast<float>(std::sin(2.0 * M_PI * phase)) * std::exp(static_cast<float>(-t * 10.0));
            float noise = dist(rng) * std::exp(static_cast<float>(-t * 8.0));
            data[i] = tone * 0.55f + noise * 0.45f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/808_snare.wav", buf, sr);
    }

    // 808 Closed Hat (note 42): metallic square waves + highpass noise
    {
        int numSamples = static_cast<int>(sr * 0.08);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        std::mt19937 rng(809);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        double freqs[] = {800.0, 1040.0, 1270.0, 1480.0, 1570.0, 1720.0};
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float sig = 0.0f;
            for (auto f : freqs)
                sig += (std::fmod(t * f, 1.0) > 0.5 ? 1.0f : -1.0f);
            sig /= 6.0f;
            sig += dist(rng) * 0.2f;
            float env = std::exp(static_cast<float>(-t * 60.0));
            data[i] = sig * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/808_hihat_closed.wav", buf, sr);
    }

    // 808 Open Hat (note 46): same metallic tones, longer decay
    {
        int numSamples = static_cast<int>(sr * 0.5);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        std::mt19937 rng(810);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        double freqs[] = {800.0, 1040.0, 1270.0, 1480.0, 1570.0, 1720.0};
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float sig = 0.0f;
            for (auto f : freqs)
                sig += (std::fmod(t * f, 1.0) > 0.5 ? 1.0f : -1.0f);
            sig /= 6.0f;
            sig += dist(rng) * 0.15f;
            float env = std::exp(static_cast<float>(-t * 6.0));
            data[i] = sig * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/808_hihat_open.wav", buf, sr);
    }

    // 808 Clap (note 39): layered noise bursts
    {
        int numSamples = static_cast<int>(sr * 0.3);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        std::mt19937 rng(811);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            // 3 quick bursts in the first 30ms then a tail
            float burstEnv = 0.0f;
            if (t < 0.01) burstEnv = 1.0f;
            else if (t > 0.012 && t < 0.02) burstEnv = 0.9f;
            else if (t > 0.022 && t < 0.03) burstEnv = 0.8f;
            float tailEnv = (t >= 0.03) ? std::exp(static_cast<float>(-(t - 0.03) * 12.0)) : 0.0f;
            data[i] = noise * (burstEnv + tailEnv);
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/808_clap.wav", buf, sr);
    }

    // 808 Cowbell (note 56): two detuned square waves
    {
        int numSamples = static_cast<int>(sr * 0.25);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float sq1 = (std::fmod(t * 540.0, 1.0) > 0.5 ? 1.0f : -1.0f);
            float sq2 = (std::fmod(t * 800.0, 1.0) > 0.5 ? 1.0f : -1.0f);
            float env = std::exp(static_cast<float>(-t * 12.0));
            data[i] = (sq1 + sq2) * 0.25f * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/808_cowbell.wav", buf, sr);
    }

    // 808 Tom (note 45): pitch-sweeping sine
    {
        int numSamples = static_cast<int>(sr * 0.5);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 100.0 + 80.0 * std::exp(-t * 15.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 5.0));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/808_tom.wav", buf, sr);
    }

    // 808 Rim (note 37): short click
    {
        int numSamples = static_cast<int>(sr * 0.05);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float env = std::exp(static_cast<float>(-t * 100.0));
            float tone = static_cast<float>(std::sin(2.0 * M_PI * 1800.0 * t));
            data[i] = tone * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/808_rim.wav", buf, sr);
    }
}

void SFZSampleGenerator::generateFlute(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {60, 67, 72, 79, 84};
    const char* noteNames[] = {"c4", "g4", "c5", "g5", "c6"};

    for (int n = 0; n < 5; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        int numSamples = static_cast<int>(sr * 2.5);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);

        std::mt19937 rng(static_cast<unsigned int>(freq));
        std::uniform_real_distribution<float> noiseDist(-1.0f, 1.0f);

        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;

            // Vibrato
            double vib = 1.0 + 0.003 * std::sin(2.0 * M_PI * 5.5 * t);
            double f = freq * vib;

            // Sine fundamental + weak 2nd harmonic
            float tone = static_cast<float>(
                std::sin(2.0 * M_PI * f * t) * 0.85 +
                std::sin(2.0 * M_PI * f * 2.0 * t) * 0.12
            );

            // Breathy noise component
            float noise = noiseDist(rng) * 0.08f;

            float env = 1.0f;
            if (t < 0.08) env = static_cast<float>(t / 0.08);
            // Sustain for looping
            env *= 0.9f;

            data[i] = (tone + noise) * env;
        }

        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);

        writeWav(dir + "/flute_" + noteNames[n] + ".wav", buf, sr);
    }
}

// ═══════════════════════════════════════════════════════════
// Section E2: New Instrument Generators
// ═══════════════════════════════════════════════════════════

void SFZSampleGenerator::generateSquareLead(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {36, 48, 60, 72, 84, 96};
    const char* noteNames[] = {"c2", "c3", "c4", "c5", "c6", "c7"};

    for (int n = 0; n < 6; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        int numSamples = static_cast<int>(sr * 2.0);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);

        int maxHarmonic = static_cast<int>((sr * 0.45) / freq);

        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float sample = 0.0f;

            // Pure band-limited square: odd harmonics, 1/n amplitude
            for (int h = 1; h <= maxHarmonic; h += 2) {
                double harmFreq = freq * h;
                if (harmFreq > sr * 0.45) break;
                sample += (1.0f / h) * static_cast<float>(std::sin(2.0 * M_PI * harmFreq * t));
            }

            // Fast attack, flat sustain
            float env = (t < 0.003) ? static_cast<float>(t / 0.003) : 1.0f;

            data[i] = sample * env * 0.5f;
        }

        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/square_" + noteNames[n] + ".wav", buf, sr);
    }
}

void SFZSampleGenerator::generateSawLead(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    int notes[] = {36, 48, 60, 72, 84, 96};
    const char* noteNames[] = {"c2", "c3", "c4", "c5", "c6", "c7"};

    for (int n = 0; n < 6; ++n) {
        double freq = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
        int numSamples = static_cast<int>(sr * 2.0);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);

        int maxHarmonic = static_cast<int>((sr * 0.45) / freq);

        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float sample = 0.0f;

            // Pure band-limited sawtooth: all harmonics, alternating sign, 1/n amplitude
            for (int h = 1; h <= maxHarmonic; ++h) {
                double harmFreq = freq * h;
                if (harmFreq > sr * 0.45) break;
                float sign = (h % 2 == 0) ? -1.0f : 1.0f;
                sample += sign * (1.0f / h) * static_cast<float>(std::sin(2.0 * M_PI * harmFreq * t));
            }

            // Fast attack, flat sustain
            float env = (t < 0.003) ? static_cast<float>(t / 0.003) : 1.0f;

            data[i] = sample * env * 0.5f;
        }

        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/saw_" + noteNames[n] + ".wav", buf, sr);
    }
}

void SFZSampleGenerator::generateAnalogKit(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    std::mt19937 rng(4242);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Kick (note 36): sine pitch drop 200→55Hz + noise transient
    {
        int numSamples = static_cast<int>(sr * 0.6);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 55.0 + 145.0 * std::exp(-t * 18.0);
            phase += freq / sr;
            float click = (t < 0.004) ? dist(rng) * static_cast<float>(1.0 - t / 0.004) * 0.5f : 0.0f;
            float env = std::exp(static_cast<float>(-t * 4.5));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env + click;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/analog_kick.wav", buf, sr);
    }

    // Snare (note 38): noise burst + sine body at 180Hz
    {
        int numSamples = static_cast<int>(sr * 0.35);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng) * std::exp(static_cast<float>(-t * 10.0));
            float tone = static_cast<float>(std::sin(2.0 * M_PI * 180.0 * t)) * std::exp(static_cast<float>(-t * 18.0));
            // Band-pass feel: simple one-pole highpass on the noise
            data[i] = noise * 0.55f + tone * 0.45f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/analog_snare.wav", buf, sr);
    }

    // Closed HH (note 42): high-pass noise, 30ms decay
    {
        int numSamples = static_cast<int>(sr * 0.06);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        float prev = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            float hp = noise - prev;
            prev = noise;
            float env = std::exp(static_cast<float>(-t * 80.0));
            data[i] = hp * env * 0.8f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/analog_hihat_closed.wav", buf, sr);
    }

    // Open HH (note 46): same but longer decay
    {
        int numSamples = static_cast<int>(sr * 0.35);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        float prev = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            float hp = noise - prev;
            prev = noise;
            float env = std::exp(static_cast<float>(-t * 8.0));
            data[i] = hp * env * 0.7f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/analog_hihat_open.wav", buf, sr);
    }

    // Clap (note 39): 3 noise bursts 10ms apart + tail
    {
        int numSamples = static_cast<int>(sr * 0.3);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            float burstEnv = 0.0f;
            if (t < 0.008) burstEnv = 1.0f;
            else if (t > 0.01 && t < 0.018) burstEnv = 0.85f;
            else if (t > 0.02 && t < 0.028) burstEnv = 0.7f;
            float tailEnv = (t >= 0.03) ? std::exp(static_cast<float>(-(t - 0.03) * 10.0)) : 0.0f;
            data[i] = noise * (burstEnv + tailEnv) * 0.8f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/analog_clap.wav", buf, sr);
    }

    // Tom Lo (note 45): sine pitch drop, longer decay
    {
        int numSamples = static_cast<int>(sr * 0.5);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 100.0 + 60.0 * std::exp(-t * 12.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 5.5));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/analog_tom_lo.wav", buf, sr);
    }

    // Tom Hi (note 48): higher pitch
    {
        int numSamples = static_cast<int>(sr * 0.4);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 160.0 + 80.0 * std::exp(-t * 14.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 6.0));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/analog_tom_hi.wav", buf, sr);
    }

    // Rimshot (note 37): short 800Hz burst + noise click
    {
        int numSamples = static_cast<int>(sr * 0.06);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float tone = static_cast<float>(std::sin(2.0 * M_PI * 800.0 * t));
            float click = dist(rng) * 0.4f;
            float env = std::exp(static_cast<float>(-t * 80.0));
            data[i] = (tone * 0.6f + click * 0.4f) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/analog_rim.wav", buf, sr);
    }
}

void SFZSampleGenerator::generateCR78Kit(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    std::mt19937 rng(7878);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Kick (note 36): low sine 60Hz, fast decay, no noise
    {
        int numSamples = static_cast<int>(sr * 0.4);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 60.0 + 30.0 * std::exp(-t * 25.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 6.0));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/cr78_kick.wav", buf, sr);
    }

    // Snare (note 38): metallic ring at 250Hz + filtered noise
    {
        int numSamples = static_cast<int>(sr * 0.25);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float ring = static_cast<float>(std::sin(2.0 * M_PI * 250.0 * t) * 0.4 +
                          std::sin(2.0 * M_PI * 380.0 * t) * 0.3);
            float noise = dist(rng) * 0.4f;
            float envRing = std::exp(static_cast<float>(-t * 15.0));
            float envNoise = std::exp(static_cast<float>(-t * 20.0));
            data[i] = ring * envRing + noise * envNoise;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/cr78_snare.wav", buf, sr);
    }

    // HH (note 42): high metallic ring ~6kHz
    {
        int numSamples = static_cast<int>(sr * 0.04);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float ring = static_cast<float>(
                std::sin(2.0 * M_PI * 6000.0 * t) * 0.5 +
                std::sin(2.0 * M_PI * 7500.0 * t) * 0.3 +
                std::sin(2.0 * M_PI * 9200.0 * t) * 0.2
            );
            float env = std::exp(static_cast<float>(-t * 100.0));
            data[i] = ring * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/cr78_hihat.wav", buf, sr);
    }

    // Cymbal (note 49): ring at 3kHz, longer decay
    {
        int numSamples = static_cast<int>(sr * 0.8);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float ring = static_cast<float>(
                std::sin(2.0 * M_PI * 3000.0 * t) * 0.4 +
                std::sin(2.0 * M_PI * 4200.0 * t) * 0.3 +
                std::sin(2.0 * M_PI * 5800.0 * t) * 0.2
            );
            ring += dist(rng) * 0.1f;
            float env = std::exp(static_cast<float>(-t * 3.0));
            data[i] = ring * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/cr78_cymbal.wav", buf, sr);
    }

    // Cowbell (note 56): two-tone ring 540Hz + 800Hz
    {
        int numSamples = static_cast<int>(sr * 0.2);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float sq1 = (std::fmod(t * 540.0, 1.0) > 0.5 ? 1.0f : -1.0f);
            float sq2 = (std::fmod(t * 800.0, 1.0) > 0.5 ? 1.0f : -1.0f);
            float env = std::exp(static_cast<float>(-t * 15.0));
            data[i] = (sq1 + sq2) * 0.25f * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/cr78_cowbell.wav", buf, sr);
    }

    // Bongo Hi (note 48): short sine pop at 400Hz
    {
        int numSamples = static_cast<int>(sr * 0.15);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 400.0 + 50.0 * std::exp(-t * 30.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 18.0));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/cr78_bongo_hi.wav", buf, sr);
    }

    // Bongo Lo (note 45): short sine pop at 250Hz
    {
        int numSamples = static_cast<int>(sr * 0.2);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 250.0 + 40.0 * std::exp(-t * 25.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 12.0));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/cr78_bongo_lo.wav", buf, sr);
    }

    // Maracas (note 70): short high-pass noise
    {
        int numSamples = static_cast<int>(sr * 0.04);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        float prev = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            float hp = noise - prev;
            prev = noise;
            float env = std::exp(static_cast<float>(-t * 60.0));
            data[i] = hp * env * 0.9f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/cr78_maracas.wav", buf, sr);
    }
}

void SFZSampleGenerator::generateLM1Kit(const std::string& dir, double sr) {
    juce::File(dir).createDirectory();
    std::mt19937 rng(1111);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Kick (note 36): sine sweep 180→50Hz with more click transient
    {
        int numSamples = static_cast<int>(sr * 0.7);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 50.0 + 130.0 * std::exp(-t * 20.0);
            phase += freq / sr;
            float click = (t < 0.005) ? static_cast<float>(std::sin(2.0 * M_PI * 4000.0 * t) * (1.0 - t / 0.005)) * 0.6f : 0.0f;
            float env = std::exp(static_cast<float>(-t * 3.8));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env + click;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_kick.wav", buf, sr);
    }

    // Snare (note 38): tuned noise with resonance at 200Hz
    {
        int numSamples = static_cast<int>(sr * 0.35);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        float lp = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            // Resonant band-pass around 200Hz
            float tone = static_cast<float>(std::sin(2.0 * M_PI * 200.0 * t)) * std::exp(static_cast<float>(-t * 14.0));
            float noiseEnv = std::exp(static_cast<float>(-t * 9.0));
            // Simple lowpass for warmth
            lp = lp * 0.3f + noise * 0.7f;
            data[i] = lp * noiseEnv * 0.5f + tone * 0.5f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_snare.wav", buf, sr);
    }

    // Closed HH (note 42): noise through band-pass at 8kHz
    {
        int numSamples = static_cast<int>(sr * 0.05);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        float prev = 0.0f;
        float prevOut = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            // Simple highpass
            float hp = noise - prev;
            prev = noise;
            // Lowpass to create bandpass
            float bp = prevOut * 0.5f + hp * 0.5f;
            prevOut = bp;
            float env = std::exp(static_cast<float>(-t * 90.0));
            data[i] = bp * env * 0.9f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_hihat_closed.wav", buf, sr);
    }

    // Open HH (note 46): same but longer
    {
        int numSamples = static_cast<int>(sr * 0.4);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        float prev = 0.0f;
        float prevOut = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            float hp = noise - prev;
            prev = noise;
            float bp = prevOut * 0.4f + hp * 0.6f;
            prevOut = bp;
            float env = std::exp(static_cast<float>(-t * 5.0));
            data[i] = bp * env * 0.8f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_hihat_open.wav", buf, sr);
    }

    // Clap (note 39): layered noise bursts with reverb-like tail
    {
        int numSamples = static_cast<int>(sr * 0.4);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            // 4 bursts for thicker clap
            float burstEnv = 0.0f;
            if (t < 0.007) burstEnv = 1.0f;
            else if (t > 0.009 && t < 0.015) burstEnv = 0.9f;
            else if (t > 0.017 && t < 0.023) burstEnv = 0.8f;
            else if (t > 0.025 && t < 0.032) burstEnv = 0.7f;
            // Longer reverb-like tail
            float tailEnv = (t >= 0.035) ? std::exp(static_cast<float>(-(t - 0.035) * 6.0)) : 0.0f;
            data[i] = noise * (burstEnv + tailEnv) * 0.75f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_clap.wav", buf, sr);
    }

    // Cabasa (note 69): short high noise burst
    {
        int numSamples = static_cast<int>(sr * 0.06);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        float prev = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            float hp = noise - prev;
            prev = noise;
            float env = std::exp(static_cast<float>(-t * 50.0));
            data[i] = hp * env * 0.85f;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_cabasa.wav", buf, sr);
    }

    // Tambourine (note 54): noise with metallic ring overtone
    {
        int numSamples = static_cast<int>(sr * 0.2);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        float prev = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            float noise = dist(rng);
            float hp = noise - prev;
            prev = noise;
            float ring = static_cast<float>(
                std::sin(2.0 * M_PI * 7500.0 * t) * 0.3 +
                std::sin(2.0 * M_PI * 10000.0 * t) * 0.2
            );
            float env = std::exp(static_cast<float>(-t * 12.0));
            data[i] = (hp * 0.5f + ring * 0.5f) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_tambourine.wav", buf, sr);
    }

    // Tom Lo (note 45): pitched sine with natural decay
    {
        int numSamples = static_cast<int>(sr * 0.45);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 110.0 + 50.0 * std::exp(-t * 15.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 5.0));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_tom_lo.wav", buf, sr);
    }

    // Tom Hi (note 48): higher pitched
    {
        int numSamples = static_cast<int>(sr * 0.35);
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.clear();
        float* data = buf.getWritePointer(0);
        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            double t = i / sr;
            double freq = 170.0 + 70.0 * std::exp(-t * 18.0);
            phase += freq / sr;
            float env = std::exp(static_cast<float>(-t * 6.5));
            data[i] = static_cast<float>(std::sin(2.0 * M_PI * phase)) * env;
        }
        float peak = buf.getMagnitude(0, numSamples);
        if (peak > 0.001f) buf.applyGain(0.9f / peak);
        writeWav(dir + "/lm1_tom_hi.wav", buf, sr);
    }
}

// ═══════════════════════════════════════════════════════════
// Section F: Preset Definitions
// ═══════════════════════════════════════════════════════════

const std::vector<SFZPresetDefinition>& SFZSampleGenerator::getPresetDefinitions() {
    static std::vector<SFZPresetDefinition> presets = {
        // 1. Grand Piano
        { "grandPiano", "Grand Piano", "Acoustic piano", "\xF0\x9F\x8E\xB9",
          "<group> ampeg_attack=0.005 ampeg_decay=80 ampeg_sustain=0 ampeg_release=30\n"
          "<region> sample=piano_c2_soft.wav pitch_keycenter=36 lokey=21 hikey=41 lovel=0 hivel=80\n"
          "<region> sample=piano_c2_hard.wav pitch_keycenter=36 lokey=21 hikey=41 lovel=81 hivel=127\n"
          "<region> sample=piano_c3_soft.wav pitch_keycenter=48 lokey=42 hikey=53 lovel=0 hivel=80\n"
          "<region> sample=piano_c3_hard.wav pitch_keycenter=48 lokey=42 hikey=53 lovel=81 hivel=127\n"
          "<region> sample=piano_c4_soft.wav pitch_keycenter=60 lokey=54 hikey=65 lovel=0 hivel=80\n"
          "<region> sample=piano_c4_hard.wav pitch_keycenter=60 lokey=54 hikey=65 lovel=81 hivel=127\n"
          "<region> sample=piano_c5_soft.wav pitch_keycenter=72 lokey=66 hikey=77 lovel=0 hivel=80\n"
          "<region> sample=piano_c5_hard.wav pitch_keycenter=72 lokey=66 hikey=77 lovel=81 hivel=127\n"
          "<region> sample=piano_c6_soft.wav pitch_keycenter=84 lokey=78 hikey=108 lovel=0 hivel=80\n"
          "<region> sample=piano_c6_hard.wav pitch_keycenter=84 lokey=78 hikey=108 lovel=81 hivel=127\n",
          generateGrandPiano
        },
        // 2. Electric Piano
        { "electricPiano", "Electric Piano", "FM Rhodes-style", "\xF0\x9F\x8E\xB9",
          "<group> ampeg_attack=0.002 ampeg_decay=80 ampeg_sustain=5 ampeg_release=25\n"
          "<region> sample=epiano_c2_soft.wav pitch_keycenter=36 lokey=21 hikey=41 lovel=0 hivel=80\n"
          "<region> sample=epiano_c2_hard.wav pitch_keycenter=36 lokey=21 hikey=41 lovel=81 hivel=127\n"
          "<region> sample=epiano_c3_soft.wav pitch_keycenter=48 lokey=42 hikey=53 lovel=0 hivel=80\n"
          "<region> sample=epiano_c3_hard.wav pitch_keycenter=48 lokey=42 hikey=53 lovel=81 hivel=127\n"
          "<region> sample=epiano_c4_soft.wav pitch_keycenter=60 lokey=54 hikey=65 lovel=0 hivel=80\n"
          "<region> sample=epiano_c4_hard.wav pitch_keycenter=60 lokey=54 hikey=65 lovel=81 hivel=127\n"
          "<region> sample=epiano_c5_soft.wav pitch_keycenter=72 lokey=66 hikey=77 lovel=0 hivel=80\n"
          "<region> sample=epiano_c5_hard.wav pitch_keycenter=72 lokey=66 hikey=77 lovel=81 hivel=127\n"
          "<region> sample=epiano_c6_soft.wav pitch_keycenter=84 lokey=78 hikey=108 lovel=0 hivel=80\n"
          "<region> sample=epiano_c6_hard.wav pitch_keycenter=84 lokey=78 hikey=108 lovel=81 hivel=127\n",
          generateElectricPiano
        },
        // 3. Pipe Organ
        { "pipeOrgan", "Pipe Organ", "Drawbar organ", "\xE2\x9B\xAA",
          "<group> ampeg_attack=2 ampeg_decay=0 ampeg_sustain=100 ampeg_release=10 loop_mode=loop_continuous\n"
          "<region> sample=organ_c2.wav pitch_keycenter=36 lokey=21 hikey=41 loop_start=44100 loop_end=88200\n"
          "<region> sample=organ_c3.wav pitch_keycenter=48 lokey=42 hikey=53 loop_start=44100 loop_end=88200\n"
          "<region> sample=organ_c4.wav pitch_keycenter=60 lokey=54 hikey=71 loop_start=44100 loop_end=88200\n"
          "<region> sample=organ_c5.wav pitch_keycenter=72 lokey=72 hikey=108 loop_start=44100 loop_end=88200\n",
          generatePipeOrgan
        },
        // 4. String Ensemble
        { "stringEnsemble", "String Ensemble", "Orchestral strings", "\xF0\x9F\x8E\xBB",
          "<group> ampeg_attack=50 ampeg_decay=0 ampeg_sustain=100 ampeg_release=30 loop_mode=loop_continuous\n"
          "<region> sample=strings_c2.wav pitch_keycenter=36 lokey=21 hikey=41 loop_start=66150 loop_end=132300\n"
          "<region> sample=strings_c3.wav pitch_keycenter=48 lokey=42 hikey=53 loop_start=66150 loop_end=132300\n"
          "<region> sample=strings_c4.wav pitch_keycenter=60 lokey=54 hikey=65 loop_start=66150 loop_end=132300\n"
          "<region> sample=strings_c5.wav pitch_keycenter=72 lokey=66 hikey=77 loop_start=66150 loop_end=132300\n"
          "<region> sample=strings_c6.wav pitch_keycenter=84 lokey=78 hikey=108 loop_start=66150 loop_end=132300\n",
          generateStringEnsemble
        },
        // 5. Brass Section
        { "brassSection", "Brass Section", "Brass ensemble", "\xF0\x9F\x8E\xBA",
          "<group> ampeg_attack=3 ampeg_decay=50 ampeg_sustain=60 ampeg_release=20\n"
          "<region> sample=brass_c3_soft.wav pitch_keycenter=48 lokey=36 hikey=53 lovel=0 hivel=80\n"
          "<region> sample=brass_c3_hard.wav pitch_keycenter=48 lokey=36 hikey=53 lovel=81 hivel=127\n"
          "<region> sample=brass_c4_soft.wav pitch_keycenter=60 lokey=54 hikey=65 lovel=0 hivel=80\n"
          "<region> sample=brass_c4_hard.wav pitch_keycenter=60 lokey=54 hikey=65 lovel=81 hivel=127\n"
          "<region> sample=brass_c5_soft.wav pitch_keycenter=72 lokey=66 hikey=96 lovel=0 hivel=80\n"
          "<region> sample=brass_c5_hard.wav pitch_keycenter=72 lokey=66 hikey=96 lovel=81 hivel=127\n",
          generateBrassSection
        },
        // 6. Fingered Bass
        { "fingeredBass", "Fingered Bass", "Electric bass", "\xF0\x9F\x8E\xB8",
          "<group> ampeg_attack=0.5 ampeg_decay=40 ampeg_sustain=10 ampeg_release=15\n"
          "<region> sample=bass_e1.wav pitch_keycenter=28 lokey=21 hikey=31\n"
          "<region> sample=bass_c2.wav pitch_keycenter=36 lokey=32 hikey=39\n"
          "<region> sample=bass_g2.wav pitch_keycenter=43 lokey=40 hikey=45\n"
          "<region> sample=bass_c3.wav pitch_keycenter=48 lokey=46 hikey=72\n",
          generateFingeredBass
        },
        // 7. Nylon Guitar
        { "nylonGuitar", "Nylon Guitar", "Classical guitar", "\xF0\x9F\x8E\xB8",
          "<group> ampeg_attack=0.2 ampeg_decay=80 ampeg_sustain=5 ampeg_release=20\n"
          "<region> sample=guitar_e2.wav pitch_keycenter=40 lokey=36 hikey=43\n"
          "<region> sample=guitar_c3.wav pitch_keycenter=48 lokey=44 hikey=51\n"
          "<region> sample=guitar_g3.wav pitch_keycenter=55 lokey=52 hikey=57\n"
          "<region> sample=guitar_c4.wav pitch_keycenter=60 lokey=58 hikey=63\n"
          "<region> sample=guitar_g4.wav pitch_keycenter=67 lokey=64 hikey=96\n",
          generateNylonGuitar
        },
        // 8. Synth Pad
        { "synthPad", "Synth Pad", "Super-saw pad", "\xE2\x99\xAC",
          "<group> ampeg_attack=80 ampeg_decay=0 ampeg_sustain=100 ampeg_release=50 loop_mode=loop_continuous\n"
          "<region> sample=pad_c2.wav pitch_keycenter=36 lokey=21 hikey=41 loop_start=66150 loop_end=132300\n"
          "<region> sample=pad_c3.wav pitch_keycenter=48 lokey=42 hikey=53 loop_start=66150 loop_end=132300\n"
          "<region> sample=pad_c4.wav pitch_keycenter=60 lokey=54 hikey=71 loop_start=66150 loop_end=132300\n"
          "<region> sample=pad_c5.wav pitch_keycenter=72 lokey=72 hikey=108 loop_start=66150 loop_end=132300\n",
          generateSynthPad
        },
        // 9. 808 Kit
        { "808kit", "808 Kit", "Classic TR-808 drum machine", "\xF0\x9F\xA5\x81",
          "<region> sample=808_kick.wav pitch_keycenter=36 lokey=36 hikey=36 ampeg_attack=0 ampeg_decay=80 ampeg_sustain=0 ampeg_release=10\n"
          "<region> sample=808_rim.wav pitch_keycenter=37 lokey=37 hikey=37 ampeg_attack=0 ampeg_decay=5 ampeg_sustain=0 ampeg_release=2\n"
          "<region> sample=808_snare.wav pitch_keycenter=38 lokey=38 hikey=38 ampeg_attack=0 ampeg_decay=40 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=808_clap.wav pitch_keycenter=39 lokey=39 hikey=39 ampeg_attack=0 ampeg_decay=30 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=808_hihat_closed.wav pitch_keycenter=42 lokey=42 hikey=42 ampeg_attack=0 ampeg_decay=8 ampeg_sustain=0 ampeg_release=2\n"
          "<region> sample=808_tom.wav pitch_keycenter=45 lokey=45 hikey=45 ampeg_attack=0 ampeg_decay=50 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=808_hihat_open.wav pitch_keycenter=46 lokey=46 hikey=46 ampeg_attack=0 ampeg_decay=50 ampeg_sustain=0 ampeg_release=10\n"
          "<region> sample=808_cowbell.wav pitch_keycenter=56 lokey=56 hikey=56 ampeg_attack=0 ampeg_decay=25 ampeg_sustain=0 ampeg_release=5\n",
          generate808Kit
        },
        // 10. Flute
        { "flute", "Flute", "Breathy flute", "\xF0\x9F\x8E\xB6",
          "<group> ampeg_attack=8 ampeg_decay=0 ampeg_sustain=100 ampeg_release=15 loop_mode=loop_continuous\n"
          "<region> sample=flute_c4.wav pitch_keycenter=60 lokey=55 hikey=63 loop_start=22050 loop_end=88200\n"
          "<region> sample=flute_g4.wav pitch_keycenter=67 lokey=64 hikey=69 loop_start=22050 loop_end=88200\n"
          "<region> sample=flute_c5.wav pitch_keycenter=72 lokey=70 hikey=75 loop_start=22050 loop_end=88200\n"
          "<region> sample=flute_g5.wav pitch_keycenter=79 lokey=76 hikey=81 loop_start=22050 loop_end=88200\n"
          "<region> sample=flute_c6.wav pitch_keycenter=84 lokey=82 hikey=108 loop_start=22050 loop_end=88200\n",
          generateFlute
        },
        // 11. Square Lead
        { "squareLead", "Square Lead", "Square wave synth lead", "\xE2\x96\xA1",
          "<group> ampeg_attack=0.005 ampeg_decay=0.05 ampeg_sustain=80 ampeg_release=10 loop_mode=loop_continuous\n"
          "<region> sample=square_c2.wav pitch_keycenter=36 lokey=21 hikey=41 loop_start=44100 loop_end=88200\n"
          "<region> sample=square_c3.wav pitch_keycenter=48 lokey=42 hikey=53 loop_start=44100 loop_end=88200\n"
          "<region> sample=square_c4.wav pitch_keycenter=60 lokey=54 hikey=65 loop_start=44100 loop_end=88200\n"
          "<region> sample=square_c5.wav pitch_keycenter=72 lokey=66 hikey=77 loop_start=44100 loop_end=88200\n"
          "<region> sample=square_c6.wav pitch_keycenter=84 lokey=78 hikey=89 loop_start=44100 loop_end=88200\n"
          "<region> sample=square_c7.wav pitch_keycenter=96 lokey=90 hikey=108 loop_start=44100 loop_end=88200\n",
          generateSquareLead
        },
        // 12. Saw Lead
        { "sawLead", "Saw Lead", "Sawtooth synth lead", "\xE2\x96\xB3",
          "<group> ampeg_attack=0.005 ampeg_decay=0 ampeg_sustain=100 ampeg_release=15 loop_mode=loop_continuous\n"
          "<region> sample=saw_c2.wav pitch_keycenter=36 lokey=21 hikey=41 loop_start=44100 loop_end=88200\n"
          "<region> sample=saw_c3.wav pitch_keycenter=48 lokey=42 hikey=53 loop_start=44100 loop_end=88200\n"
          "<region> sample=saw_c4.wav pitch_keycenter=60 lokey=54 hikey=65 loop_start=44100 loop_end=88200\n"
          "<region> sample=saw_c5.wav pitch_keycenter=72 lokey=66 hikey=77 loop_start=44100 loop_end=88200\n"
          "<region> sample=saw_c6.wav pitch_keycenter=84 lokey=78 hikey=89 loop_start=44100 loop_end=88200\n"
          "<region> sample=saw_c7.wav pitch_keycenter=96 lokey=90 hikey=108 loop_start=44100 loop_end=88200\n",
          generateSawLead
        },
        // 13. Analog Kit (LinnDrum/DMX-inspired)
        { "analogKit", "Analog Kit", "LinnDrum-inspired drum machine", "\xF0\x9F\xA5\x81",
          "<region> sample=analog_kick.wav pitch_keycenter=36 lokey=36 hikey=36 ampeg_attack=0 ampeg_decay=60 ampeg_sustain=0 ampeg_release=10\n"
          "<region> sample=analog_rim.wav pitch_keycenter=37 lokey=37 hikey=37 ampeg_attack=0 ampeg_decay=5 ampeg_sustain=0 ampeg_release=2\n"
          "<region> sample=analog_snare.wav pitch_keycenter=38 lokey=38 hikey=38 ampeg_attack=0 ampeg_decay=35 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=analog_clap.wav pitch_keycenter=39 lokey=39 hikey=39 ampeg_attack=0 ampeg_decay=30 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=analog_hihat_closed.wav pitch_keycenter=42 lokey=42 hikey=42 ampeg_attack=0 ampeg_decay=6 ampeg_sustain=0 ampeg_release=2\n"
          "<region> sample=analog_tom_lo.wav pitch_keycenter=45 lokey=45 hikey=45 ampeg_attack=0 ampeg_decay=50 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=analog_hihat_open.wav pitch_keycenter=46 lokey=46 hikey=46 ampeg_attack=0 ampeg_decay=35 ampeg_sustain=0 ampeg_release=10\n"
          "<region> sample=analog_tom_hi.wav pitch_keycenter=48 lokey=48 hikey=48 ampeg_attack=0 ampeg_decay=40 ampeg_sustain=0 ampeg_release=5\n",
          generateAnalogKit
        },
        // 14. CR-78 Kit (Roland CR-78 inspired)
        { "cr78Kit", "CR-78 Kit", "Roland CR-78 drum machine", "\xF0\x9F\xA5\x81",
          "<region> sample=cr78_kick.wav pitch_keycenter=36 lokey=36 hikey=36 ampeg_attack=0 ampeg_decay=40 ampeg_sustain=0 ampeg_release=10\n"
          "<region> sample=cr78_snare.wav pitch_keycenter=38 lokey=38 hikey=38 ampeg_attack=0 ampeg_decay=25 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=cr78_hihat.wav pitch_keycenter=42 lokey=42 hikey=42 ampeg_attack=0 ampeg_decay=4 ampeg_sustain=0 ampeg_release=2\n"
          "<region> sample=cr78_bongo_lo.wav pitch_keycenter=45 lokey=45 hikey=45 ampeg_attack=0 ampeg_decay=20 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=cr78_bongo_hi.wav pitch_keycenter=48 lokey=48 hikey=48 ampeg_attack=0 ampeg_decay=15 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=cr78_cymbal.wav pitch_keycenter=49 lokey=49 hikey=49 ampeg_attack=0 ampeg_decay=80 ampeg_sustain=0 ampeg_release=10\n"
          "<region> sample=cr78_cowbell.wav pitch_keycenter=56 lokey=56 hikey=56 ampeg_attack=0 ampeg_decay=20 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=cr78_maracas.wav pitch_keycenter=70 lokey=70 hikey=70 ampeg_attack=0 ampeg_decay=4 ampeg_sustain=0 ampeg_release=2\n",
          generateCR78Kit
        },
        // 15. LM-1 Kit (Linn LM-1 inspired)
        { "lm1Kit", "LM-1 Kit", "Linn LM-1 drum machine", "\xF0\x9F\xA5\x81",
          "<region> sample=lm1_kick.wav pitch_keycenter=36 lokey=36 hikey=36 ampeg_attack=0 ampeg_decay=70 ampeg_sustain=0 ampeg_release=10\n"
          "<region> sample=lm1_snare.wav pitch_keycenter=38 lokey=38 hikey=38 ampeg_attack=0 ampeg_decay=35 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=lm1_clap.wav pitch_keycenter=39 lokey=39 hikey=39 ampeg_attack=0 ampeg_decay=40 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=lm1_hihat_closed.wav pitch_keycenter=42 lokey=42 hikey=42 ampeg_attack=0 ampeg_decay=5 ampeg_sustain=0 ampeg_release=2\n"
          "<region> sample=lm1_tom_lo.wav pitch_keycenter=45 lokey=45 hikey=45 ampeg_attack=0 ampeg_decay=45 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=lm1_hihat_open.wav pitch_keycenter=46 lokey=46 hikey=46 ampeg_attack=0 ampeg_decay=40 ampeg_sustain=0 ampeg_release=10\n"
          "<region> sample=lm1_tom_hi.wav pitch_keycenter=48 lokey=48 hikey=48 ampeg_attack=0 ampeg_decay=35 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=lm1_tambourine.wav pitch_keycenter=54 lokey=54 hikey=54 ampeg_attack=0 ampeg_decay=20 ampeg_sustain=0 ampeg_release=5\n"
          "<region> sample=lm1_cabasa.wav pitch_keycenter=69 lokey=69 hikey=69 ampeg_attack=0 ampeg_decay=6 ampeg_sustain=0 ampeg_release=2\n",
          generateLM1Kit
        }
    };

    return presets;
}

void SFZSampleGenerator::ensurePresetsGenerated(const std::string& cacheDir, double sampleRate) {
    auto basePath = juce::File(cacheDir);
    if (!basePath.isDirectory()) basePath.createDirectory();

    const auto& presets = getPresetDefinitions();
    for (const auto& preset : presets) {
        auto presetDir = basePath.getChildFile(preset.id);
        auto marker = presetDir.getChildFile(".generated");
        if (!marker.existsAsFile() && preset.generator) {
            fprintf(stderr, "[SFZ] Generating samples for %s...\n", preset.name.c_str());
            presetDir.createDirectory();
            preset.generator(presetDir.getFullPathName().toStdString(), sampleRate);
            marker.create();
            fprintf(stderr, "[SFZ] Done generating %s\n", preset.name.c_str());
        }
    }
}
