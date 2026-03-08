#include "BasicSynth.h"
#include <cmath>

// ─── Voice ──────────────────────────────────────────────

BasicSynthVoice::BasicSynthVoice(BasicSynthParams& p) : params(p) {
    filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
}

bool BasicSynthVoice::canPlaySound(juce::SynthesiserSound* sound) {
    return dynamic_cast<BasicSynthSound*>(sound) != nullptr;
}

void BasicSynthVoice::startNote(int midiNoteNumber, float velocity,
                                juce::SynthesiserSound*, int) {
    frequency = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber);
    noteVelocity = velocity;
    phase1 = 0.0;
    phase2 = 0.0;

    juce::ADSR::Parameters ampParams{params.ampAttack, params.ampDecay,
                                      params.ampSustain, params.ampRelease};
    ampEnv.setSampleRate(getSampleRate());
    ampEnv.setParameters(ampParams);
    ampEnv.noteOn();

    juce::ADSR::Parameters filtParams{params.filterEnvAttack, params.filterEnvDecay,
                                       params.filterEnvSustain, params.filterEnvRelease};
    filterEnv.setSampleRate(getSampleRate());
    filterEnv.setParameters(filtParams);
    filterEnv.noteOn();

    filter.reset();
    filter.prepare({getSampleRate(), 512u, 1});
}

void BasicSynthVoice::stopNote(float, bool allowTailOff) {
    if (allowTailOff) {
        ampEnv.noteOff();
        filterEnv.noteOff();
    } else {
        ampEnv.reset();
        filterEnv.reset();
        clearCurrentNote();
    }
}

float BasicSynthVoice::generateOscSample(double& phase, double freq, int waveform) {
    double sr = getSampleRate();
    double inc = freq / sr;
    phase += inc;
    if (phase >= 1.0) phase -= 1.0;

    float out = 0.0f;
    switch (waveform) {
        case 0: // sine
            out = std::sin(phase * juce::MathConstants<double>::twoPi);
            break;
        case 1: // saw
            out = static_cast<float>(2.0 * phase - 1.0);
            break;
        case 2: // square
            out = phase < 0.5 ? 1.0f : -1.0f;
            break;
        case 3: // triangle
            out = static_cast<float>(4.0 * std::abs(phase - 0.5) - 1.0);
            break;
    }
    return out;
}

void BasicSynthVoice::updateFilterParams(float envValue) {
    float baseCutoff = params.filterCutoff;
    float modSemitones = params.filterEnvDepth * envValue;
    float modFactor = std::pow(2.0f, modSemitones / 12.0f);
    float cutoff = juce::jlimit(20.0f, 20000.0f, baseCutoff * modFactor);

    switch (params.filterType) {
        case 0: filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass); break;
        case 1: filter.setType(juce::dsp::StateVariableTPTFilterType::highpass); break;
        case 2: filter.setType(juce::dsp::StateVariableTPTFilterType::bandpass); break;
    }
    filter.setCutoffFrequency(cutoff);
    filter.setResonance(juce::jlimit(0.1f, 5.0f, params.filterResonance * 5.0f));
}

void BasicSynthVoice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                       int startSample, int numSamples) {
    if (!isVoiceActive()) return;

    // Update ADSR from live params each block (real-time control)
    ampEnv.setParameters({params.ampAttack, params.ampDecay,
                          params.ampSustain, params.ampRelease});
    filterEnv.setParameters({params.filterEnvAttack, params.filterEnvDecay,
                             params.filterEnvSustain, params.filterEnvRelease});

    double osc2Freq = frequency * std::pow(2.0, params.osc2Detune / 12.0);
    float mix = params.oscMix;

    for (int i = 0; i < numSamples; ++i) {
        float s1 = generateOscSample(phase1, frequency, params.osc1Waveform);
        float s2 = generateOscSample(phase2, osc2Freq, params.osc2Waveform);
        float raw = s1 * (1.0f - mix) + s2 * mix;

        float filtEnvVal = filterEnv.getNextSample();
        updateFilterParams(filtEnvVal);
        float filtered = filter.processSample(0, raw);

        float ampEnvVal = ampEnv.getNextSample();
        float sample = filtered * ampEnvVal * noteVelocity * 0.3f;

        for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
            outputBuffer.addSample(ch, startSample + i, sample);

        if (!ampEnv.isActive()) {
            clearCurrentNote();
            break;
        }
    }
}

// ─── Synth instrument ───────────────────────────────────

BasicSynth::BasicSynth() : BuiltInInstrument("Basic Synth") {}

void BasicSynth::prepareSynth(double sampleRate, int samplesPerBlock) {
    synth.clearVoices();
    synth.clearSounds();
    synth.addSound(new BasicSynthSound());
    for (int i = 0; i < 16; ++i)
        synth.addVoice(new BasicSynthVoice(params));
    synth.setCurrentPlaybackSampleRate(sampleRate);
}

void BasicSynth::setParam(const std::string& name, float value) {
    if (name == "osc1Waveform") params.osc1Waveform = static_cast<int>(value);
    else if (name == "osc2Waveform") params.osc2Waveform = static_cast<int>(value);
    else if (name == "osc2Detune") params.osc2Detune = value;
    else if (name == "oscMix") params.oscMix = value;
    else if (name == "filterType") params.filterType = static_cast<int>(value);
    else if (name == "filterCutoff") params.filterCutoff = value;
    else if (name == "filterResonance") params.filterResonance = value;
    else if (name == "ampAttack") params.ampAttack = value;
    else if (name == "ampDecay") params.ampDecay = value;
    else if (name == "ampSustain") params.ampSustain = value;
    else if (name == "ampRelease") params.ampRelease = value;
    else if (name == "filterEnvAttack") params.filterEnvAttack = value;
    else if (name == "filterEnvDecay") params.filterEnvDecay = value;
    else if (name == "filterEnvSustain") params.filterEnvSustain = value;
    else if (name == "filterEnvRelease") params.filterEnvRelease = value;
    else if (name == "filterEnvDepth") params.filterEnvDepth = value;
}

float BasicSynth::getParam(const std::string& name) const {
    if (name == "osc1Waveform") return static_cast<float>(params.osc1Waveform);
    if (name == "osc2Waveform") return static_cast<float>(params.osc2Waveform);
    if (name == "osc2Detune") return params.osc2Detune;
    if (name == "oscMix") return params.oscMix;
    if (name == "filterType") return static_cast<float>(params.filterType);
    if (name == "filterCutoff") return params.filterCutoff;
    if (name == "filterResonance") return params.filterResonance;
    if (name == "ampAttack") return params.ampAttack;
    if (name == "ampDecay") return params.ampDecay;
    if (name == "ampSustain") return params.ampSustain;
    if (name == "ampRelease") return params.ampRelease;
    if (name == "filterEnvAttack") return params.filterEnvAttack;
    if (name == "filterEnvDecay") return params.filterEnvDecay;
    if (name == "filterEnvSustain") return params.filterEnvSustain;
    if (name == "filterEnvRelease") return params.filterEnvRelease;
    if (name == "filterEnvDepth") return params.filterEnvDepth;
    return 0.0f;
}
