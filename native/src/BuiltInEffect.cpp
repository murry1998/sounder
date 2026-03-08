#include "BuiltInEffect.h"
#include <cmath>

std::vector<BuiltInEffect::EffectTypeInfo> BuiltInEffect::getAvailableTypes() {
    return {
        {"gate",       "Gate",       "Dynamics"},
        {"eq",         "EQ",         "EQ"},
        {"compressor", "Compressor", "Dynamics"},
        {"distortion", "Distortion", "Saturation"},
        {"filter",     "Filter",     "Filter"},
        {"chorus",     "Chorus",     "Modulation"},
        {"phaser",     "Phaser",     "Modulation"},
        {"delay",      "Delay",      "Time"},
        {"reverb",     "Reverb",     "Time"},
        {"limiter",    "Limiter",    "Dynamics"}
    };
}

std::unique_ptr<BuiltInEffect> BuiltInEffect::create(const std::string& type) {
    auto types = getAvailableTypes();
    for (auto& t : types) {
        if (t.type == type) return std::make_unique<BuiltInEffect>(type);
    }
    return nullptr;
}

void BuiltInEffect::prepareToPlay(double sampleRate, int samplesPerBlock) {
    cachedSampleRate = sampleRate;
    cachedBlockSize = samplesPerBlock;
    juce::dsp::ProcessSpec spec{sampleRate, (uint32_t)samplesPerBlock, 2};

    if (effectType == "filter") { svf.prepare(spec); svf.setType(juce::dsp::StateVariableTPTFilterType::lowpass); svf.setCutoffFrequency((float)filterCutoff); svf.setResonance(filterResonance); }
    else if (effectType == "chorus") { chorus.prepare(spec); chorus.setRate(chorusRate); chorus.setDepth(chorusDepth); chorus.setCentreDelay(chorusCentreDelay); chorus.setFeedback(chorusFeedback); chorus.setMix(chorusMix); }
    else if (effectType == "phaser") { phaser.prepare(spec); phaser.setRate(phaserRate); phaser.setDepth(phaserDepth); phaser.setCentreFrequency(phaserCentreFreq); phaser.setFeedback(phaserFeedback); phaser.setMix(phaserMix); }
    else if (effectType == "delay") { delayLine.prepare(spec); delayLine.setDelay((float)(delayTime * sampleRate)); delayL = delayR = 0.0f; }
    else if (effectType == "reverb") { reverb.prepare(spec); juce::dsp::Reverb::Parameters rp; rp.roomSize = reverbSize; rp.damping = reverbDamping; rp.wetLevel = reverbMix; rp.dryLevel = 1.0f - reverbMix; rp.width = reverbWidth; reverb.setParameters(rp); }
    else if (effectType == "limiter") { limiter.prepare(spec); limiter.setThreshold(limiterThreshold); limiter.setRelease(limiterRelease); }
}

void BuiltInEffect::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    int n = buffer.getNumSamples();
    if (n == 0) return;

    if (effectType == "gate") {
        float threshLin = std::pow(10.0f, gateThreshold / 20.0f);
        float aCoeff = std::exp(-1.0f / (gateAttack * 0.001f * (float)cachedSampleRate));
        float rCoeff = std::exp(-1.0f / (gateRelease * 0.001f * (float)cachedSampleRate));
        for (int i = 0; i < n; ++i) {
            float inL = buffer.getSample(0, i);
            float inR = buffer.getNumChannels() > 1 ? buffer.getSample(1, i) : inL;
            float level = std::max(std::abs(inL), std::abs(inR));
            float target = level > threshLin ? 1.0f : 1.0f / gateRatio;
            float coeff = target > gateEnvelope ? aCoeff : rCoeff;
            gateEnvelope = gateEnvelope * coeff + target * (1.0f - coeff);
            buffer.setSample(0, i, inL * gateEnvelope);
            if (buffer.getNumChannels() > 1) buffer.setSample(1, i, inR * gateEnvelope);
        }
    }
    else if (effectType == "eq") {
        float lowScale = std::pow(10.0f, eqLowGain / 20.0f);
        float midScale = std::pow(10.0f, eqMidGain / 20.0f);
        float highScale = std::pow(10.0f, eqHighGain / 20.0f);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
            float* data = buffer.getWritePointer(ch);
            for (int i = 0; i < n; ++i) data[i] *= midScale;
        }
        (void)lowScale; (void)highScale;
    }
    else if (effectType == "compressor") {
        float threshLin = std::pow(10.0f, compThreshold / 20.0f);
        float aCoeff = std::exp(-1.0f / (compAttack * 0.001f * (float)cachedSampleRate));
        float rCoeff = std::exp(-1.0f / (compRelease * 0.001f * (float)cachedSampleRate));
        for (int i = 0; i < n; ++i) {
            float inL = buffer.getSample(0, i);
            float inR = buffer.getNumChannels() > 1 ? buffer.getSample(1, i) : inL;
            float level = std::max(std::abs(inL), std::abs(inR));
            float coeff = level > compEnvelope ? aCoeff : rCoeff;
            compEnvelope = compEnvelope * coeff + level * (1.0f - coeff);
            float gain = 1.0f;
            if (compEnvelope > threshLin && compEnvelope > 0.0001f)
                gain = threshLin * std::pow(compEnvelope / threshLin, 1.0f / compRatio) / compEnvelope;
            buffer.setSample(0, i, inL * gain);
            if (buffer.getNumChannels() > 1) buffer.setSample(1, i, inR * gain);
        }
    }
    else if (effectType == "distortion") {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
            float* data = buffer.getWritePointer(ch);
            for (int i = 0; i < n; ++i) {
                float dry = data[i];
                float wet = std::tanh(dry * distDrive);
                data[i] = dry * (1.0f - distMix) + wet * distMix;
            }
        }
    }
    else if (effectType == "filter") {
        svf.setCutoffFrequency(filterCutoff);
        svf.setResonance(std::max(0.01f, filterResonance));
        if (filterMode == 0) svf.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
        else if (filterMode == 1) svf.setType(juce::dsp::StateVariableTPTFilterType::highpass);
        else svf.setType(juce::dsp::StateVariableTPTFilterType::bandpass);
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> ctx(block);
        svf.process(ctx);
    }
    else if (effectType == "chorus") {
        chorus.setRate(chorusRate); chorus.setDepth(chorusDepth);
        chorus.setCentreDelay(chorusCentreDelay); chorus.setFeedback(chorusFeedback); chorus.setMix(chorusMix);
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> ctx(block);
        chorus.process(ctx);
    }
    else if (effectType == "phaser") {
        phaser.setRate(phaserRate); phaser.setDepth(phaserDepth);
        phaser.setCentreFrequency(phaserCentreFreq); phaser.setFeedback(phaserFeedback); phaser.setMix(phaserMix);
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> ctx(block);
        phaser.process(ctx);
    }
    else if (effectType == "delay") {
        delayLine.setDelay((float)(delayTime * cachedSampleRate));
        for (int i = 0; i < n; ++i) {
            float inL = buffer.getSample(0, i);
            float dL = delayLine.popSample(0);
            delayLine.pushSample(0, inL + dL * delayFeedback);
            buffer.setSample(0, i, inL * (1.0f - delayMix) + dL * delayMix);
            if (buffer.getNumChannels() > 1) {
                float inR = buffer.getSample(1, i);
                float dR = delayLine.popSample(1);
                delayLine.pushSample(1, inR + dR * delayFeedback);
                buffer.setSample(1, i, inR * (1.0f - delayMix) + dR * delayMix);
            }
        }
    }
    else if (effectType == "reverb") {
        juce::dsp::Reverb::Parameters rp;
        rp.roomSize = reverbSize; rp.damping = reverbDamping;
        rp.wetLevel = reverbMix; rp.dryLevel = 1.0f - reverbMix; rp.width = reverbWidth;
        reverb.setParameters(rp);
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> ctx(block);
        reverb.process(ctx);
    }
    else if (effectType == "limiter") {
        limiter.setThreshold(limiterThreshold); limiter.setRelease(limiterRelease);
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> ctx(block);
        limiter.process(ctx);
    }
}

void BuiltInEffect::setParam(const std::string& name, float value) {
    if (effectType == "gate") {
        if (name == "threshold") gateThreshold = value;
        else if (name == "ratio") gateRatio = value;
        else if (name == "attack") gateAttack = value;
        else if (name == "release") gateRelease = value;
    } else if (effectType == "eq") {
        if (name == "lowGain") eqLowGain = value;
        else if (name == "midGain") eqMidGain = value;
        else if (name == "midFreq") eqMidFreq = value;
        else if (name == "highGain") eqHighGain = value;
    } else if (effectType == "compressor") {
        if (name == "threshold") compThreshold = value;
        else if (name == "ratio") compRatio = value;
        else if (name == "attack") compAttack = value;
        else if (name == "release") compRelease = value;
    } else if (effectType == "distortion") {
        if (name == "drive") distDrive = value;
        else if (name == "mix") distMix = value;
    } else if (effectType == "filter") {
        if (name == "cutoff") filterCutoff = value;
        else if (name == "resonance") filterResonance = value;
        else if (name == "mode") filterMode = (int)value;
    } else if (effectType == "chorus") {
        if (name == "rate") chorusRate = value;
        else if (name == "depth") chorusDepth = value;
        else if (name == "centreDelay") chorusCentreDelay = value;
        else if (name == "feedback") chorusFeedback = value;
        else if (name == "mix") chorusMix = value;
    } else if (effectType == "phaser") {
        if (name == "rate") phaserRate = value;
        else if (name == "depth") phaserDepth = value;
        else if (name == "centreFrequency") phaserCentreFreq = value;
        else if (name == "feedback") phaserFeedback = value;
        else if (name == "mix") phaserMix = value;
    } else if (effectType == "delay") {
        if (name == "time") delayTime = value;
        else if (name == "mix") delayMix = value;
        else if (name == "feedback") delayFeedback = value;
    } else if (effectType == "reverb") {
        if (name == "size") reverbSize = value;
        else if (name == "damping") reverbDamping = value;
        else if (name == "mix") reverbMix = value;
        else if (name == "width") reverbWidth = value;
    } else if (effectType == "limiter") {
        if (name == "threshold") limiterThreshold = value;
        else if (name == "release") limiterRelease = value;
    }
}

float BuiltInEffect::getParam(const std::string& name) const {
    if (effectType == "gate") {
        if (name == "threshold") return gateThreshold;
        if (name == "ratio") return gateRatio;
        if (name == "attack") return gateAttack;
        if (name == "release") return gateRelease;
    } else if (effectType == "eq") {
        if (name == "lowGain") return eqLowGain;
        if (name == "midGain") return eqMidGain;
        if (name == "midFreq") return eqMidFreq;
        if (name == "highGain") return eqHighGain;
    } else if (effectType == "compressor") {
        if (name == "threshold") return compThreshold;
        if (name == "ratio") return compRatio;
        if (name == "attack") return compAttack;
        if (name == "release") return compRelease;
    } else if (effectType == "distortion") {
        if (name == "drive") return distDrive;
        if (name == "mix") return distMix;
    } else if (effectType == "filter") {
        if (name == "cutoff") return filterCutoff;
        if (name == "resonance") return filterResonance;
        if (name == "mode") return (float)filterMode;
    } else if (effectType == "chorus") {
        if (name == "rate") return chorusRate;
        if (name == "depth") return chorusDepth;
        if (name == "centreDelay") return chorusCentreDelay;
        if (name == "feedback") return chorusFeedback;
        if (name == "mix") return chorusMix;
    } else if (effectType == "phaser") {
        if (name == "rate") return phaserRate;
        if (name == "depth") return phaserDepth;
        if (name == "centreFrequency") return phaserCentreFreq;
        if (name == "feedback") return phaserFeedback;
        if (name == "mix") return phaserMix;
    } else if (effectType == "delay") {
        if (name == "time") return delayTime;
        if (name == "mix") return delayMix;
        if (name == "feedback") return delayFeedback;
    } else if (effectType == "reverb") {
        if (name == "size") return reverbSize;
        if (name == "damping") return reverbDamping;
        if (name == "mix") return reverbMix;
        if (name == "width") return reverbWidth;
    } else if (effectType == "limiter") {
        if (name == "threshold") return limiterThreshold;
        if (name == "release") return limiterRelease;
    }
    return 0.0f;
}
