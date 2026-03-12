#include "BeatQuantizer.h"
#include <cmath>
#include <algorithm>
#include <numeric>

BeatQuantizer::BeatQuantizer() { initWindow(); }
BeatQuantizer::~BeatQuantizer() = default;

void BeatQuantizer::initWindow() {
    window.resize(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; i++)
        window[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / FFT_SIZE));
}

// ── Spectral Flux Onset Detection ──

std::vector<float> BeatQuantizer::computeSpectralFlux(const float* monoSamples, int numSamples) {
    int numFrames = (numSamples - FFT_SIZE) / HOP_SIZE + 1;
    if (numFrames < 2) return {};

    std::vector<float> flux(numFrames, 0.0f);
    std::vector<float> prevMagnitude(FFT_SIZE / 2 + 1, 0.0f);
    std::vector<float> fftData(FFT_SIZE * 2, 0.0f);

    for (int frame = 0; frame < numFrames; frame++) {
        int offset = frame * HOP_SIZE;

        for (int i = 0; i < FFT_SIZE; i++) {
            int idx = offset + i;
            float sample = (idx < numSamples) ? monoSamples[idx] : 0.0f;
            fftData[i] = sample * window[i];
        }
        for (int i = FFT_SIZE; i < FFT_SIZE * 2; i++)
            fftData[i] = 0.0f;

        fft.performRealOnlyForwardTransform(fftData.data());

        float frameFlux = 0.0f;
        for (int bin = 0; bin <= FFT_SIZE / 2; bin++) {
            float real = fftData[bin * 2];
            float imag = fftData[bin * 2 + 1];
            float mag = std::sqrt(real * real + imag * imag);
            float diff = mag - prevMagnitude[bin];
            if (diff > 0.0f) frameFlux += diff;
            prevMagnitude[bin] = mag;
        }
        flux[frame] = frameFlux;
    }
    return flux;
}

// ── Peak Picking with Adaptive Threshold ──

std::vector<int> BeatQuantizer::pickOnsets(const std::vector<float>& df, double sampleRate) {
    int numFrames = static_cast<int>(df.size());
    if (numFrames < 3) return {};

    int halfWin = MEDIAN_WINDOW / 2;
    int minDistFrames = static_cast<int>((MIN_ONSET_DISTANCE_MS / 1000.0) * sampleRate / HOP_SIZE);

    // Compute global mean for a floor threshold
    float globalSum = 0.0f;
    for (auto v : df) globalSum += v;
    float globalMean = globalSum / numFrames;

    std::vector<int> onsetFrames;
    int lastOnsetFrame = -minDistFrames - 1;

    for (int i = 1; i < numFrames - 1; i++) {
        int start = std::max(0, i - halfWin);
        int end = std::min(numFrames, i + halfWin + 1);
        std::vector<float> localValues(df.begin() + start, df.begin() + end);
        std::nth_element(localValues.begin(), localValues.begin() + (int)localValues.size() / 2, localValues.end());
        float localMedian = localValues[localValues.size() / 2];

        // Adaptive threshold: multiplier * local median, but at least 0.5 * global mean
        float threshold = std::max(ONSET_THRESHOLD_MULTIPLIER * localMedian,
                                   0.5f * globalMean);

        if (df[i] > threshold &&
            df[i] > df[i - 1] &&
            df[i] >= df[i + 1] &&
            (i - lastOnsetFrame) >= minDistFrames) {
            onsetFrames.push_back(i);
            lastOnsetFrame = i;
        }
    }
    return onsetFrames;
}

// ── Detect Onsets ──

std::vector<OnsetInfo> BeatQuantizer::detectOnsets(const juce::AudioBuffer<float>& source, double sampleRate) {
    int numSamples = source.getNumSamples();
    int numChannels = source.getNumChannels();

    // Mix to mono
    std::vector<float> mono(numSamples, 0.0f);
    for (int ch = 0; ch < numChannels; ch++) {
        const float* data = source.getReadPointer(ch);
        for (int i = 0; i < numSamples; i++)
            mono[i] += data[i];
    }
    float scale = 1.0f / std::max(1, numChannels);
    for (auto& s : mono) s *= scale;

    auto flux = computeSpectralFlux(mono.data(), numSamples);
    auto onsetFrames = pickOnsets(flux, sampleRate);

    std::vector<OnsetInfo> result;
    result.push_back({0, 0.0, 0.0, 0});

    for (int frame : onsetFrames) {
        int samplePos = frame * HOP_SIZE;
        if (samplePos <= 0) continue;
        OnsetInfo info;
        info.samplePosition = samplePos;
        info.timeSeconds = static_cast<double>(samplePos) / sampleRate;
        info.nearestBeatTime = 0.0;
        info.targetSamplePosition = 0;
        result.push_back(info);
    }
    return result;
}

// ── Grid Snapping ──

void BeatQuantizer::snapToGrid(std::vector<OnsetInfo>& onsets, double sampleRate,
                                double bpm, double gridDivision, float strength) {
    double beatsPerSecond = bpm / 60.0;
    double gridIntervalSec = gridDivision / beatsPerSecond;

    for (auto& onset : onsets) {
        if (onset.samplePosition == 0) {
            onset.nearestBeatTime = 0.0;
            onset.targetSamplePosition = 0;
            continue;
        }
        double timeSec = onset.timeSeconds;
        double gridIndex = std::round(timeSec / gridIntervalSec);
        double snappedTime = gridIndex * gridIntervalSec;
        double targetTime = timeSec + strength * (snappedTime - timeSec);
        onset.nearestBeatTime = targetTime;
        onset.targetSamplePosition = static_cast<int>(targetTime * sampleRate);
    }

    // Enforce monotonicity
    for (size_t i = 1; i < onsets.size(); i++) {
        if (onsets[i].targetSamplePosition <= onsets[i - 1].targetSamplePosition)
            onsets[i].targetSamplePosition = onsets[i - 1].targetSamplePosition + 1;
    }
}

// ── WSOLA Time-Stretch ──

int BeatQuantizer::findBestOverlap(const float* regionA, const float* regionB,
                                    int searchLength, int overlapSize) {
    if (overlapSize <= 0 || searchLength <= 0) return 0;
    int bestOffset = 0;
    float bestCorrelation = -1e30f;
    int maxOffset = searchLength - overlapSize;
    if (maxOffset < 0) maxOffset = 0;

    for (int offset = 0; offset <= maxOffset; offset++) {
        float correlation = 0.0f;
        for (int i = 0; i < overlapSize; i++)
            correlation += regionA[i] * regionB[offset + i];
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestOffset = offset;
        }
    }
    return bestOffset;
}

juce::AudioBuffer<float> BeatQuantizer::wsolaStretch(
    const juce::AudioBuffer<float>& sourceSegment, int targetLength) {

    int sourceLength = sourceSegment.getNumSamples();
    int numChannels = sourceSegment.getNumChannels();

    if (targetLength <= 0) targetLength = 1;

    // Very short segments or near-identical lengths: direct copy
    if (sourceLength < 256 || std::abs(sourceLength - targetLength) < 64) {
        juce::AudioBuffer<float> result(numChannels, targetLength);
        result.clear();
        int copyLen = std::min(sourceLength, targetLength);
        for (int ch = 0; ch < numChannels; ch++)
            result.copyFrom(ch, 0, sourceSegment, ch, 0, copyLen);
        return result;
    }

    double ratio = static_cast<double>(targetLength) / sourceLength;
    ratio = std::clamp(ratio, 0.25, 4.0);
    int clampedTarget = static_cast<int>(sourceLength * ratio);

    // Use half-window overlap (hop = winSize/2) for cleaner COLA
    int winSize = std::min(WSOLA_WINDOW_SIZE, sourceLength / 2);
    if (winSize < 256) winSize = std::min(256, sourceLength);
    int hopOut = winSize / 2;
    int hopIn = std::max(1, static_cast<int>(hopOut / ratio));

    // Hann window
    std::vector<float> hann(winSize);
    for (int i = 0; i < winSize; i++)
        hann[i] = 0.5f * (1.0f - std::cos(2.0 * juce::MathConstants<double>::pi * i / winSize));

    int bufLen = clampedTarget + winSize * 2;
    juce::AudioBuffer<float> result(numChannels, bufLen);
    result.clear();
    std::vector<float> weightSum(bufLen, 0.0f);

    const float* srcCh0 = sourceSegment.getReadPointer(0);

    int outPos = 0;
    double srcPos = 0.0;

    while (outPos < clampedTarget) {
        int idealSrc = static_cast<int>(srcPos);
        idealSrc = std::clamp(idealSrc, 0, std::max(0, sourceLength - winSize));

        int bestSrc = idealSrc;

        // For all frames after the first, search for the best overlap match
        if (outPos > 0 && winSize > 0) {
            int searchMin = std::max(0, idealSrc - WSOLA_TOLERANCE);
            int searchMax = std::min(sourceLength - winSize, idealSrc + WSOLA_TOLERANCE);

            if (searchMax > searchMin) {
                // Compare candidates against what's already in the output at outPos
                // Use a small overlap region for correlation
                int corrLen = std::min(winSize / 2, clampedTarget - outPos);
                corrLen = std::min(corrLen, outPos);  // can't look back further than we've written
                if (corrLen < 32) corrLen = std::min(32, winSize);

                float bestCorr = -1e30f;
                // Get the output signal at the overlap point (already normalized)
                // Actually, compare source candidates directly
                // Find source position that best matches the end of the previous window
                int prevSrcEnd = std::max(0, idealSrc);
                for (int candidate = searchMin; candidate <= searchMax; candidate += 4) {
                    if (candidate + winSize > sourceLength) continue;
                    float corr = 0.0f;
                    // Cross-correlate the beginning of this candidate with the
                    // source region around where the previous frame ended
                    int refStart = std::max(0, prevSrcEnd - hopIn);
                    int cLen = std::min(corrLen, sourceLength - std::max(candidate, refStart));
                    cLen = std::min(cLen, winSize);
                    if (cLen <= 0) continue;
                    for (int i = 0; i < cLen; i++)
                        corr += srcCh0[refStart + i] * srcCh0[candidate + i];
                    if (corr > bestCorr) {
                        bestCorr = corr;
                        bestSrc = candidate;
                    }
                }
                bestSrc = std::clamp(bestSrc, 0, std::max(0, sourceLength - winSize));
            }
        }

        int remaining = std::min(winSize, bufLen - outPos);
        for (int ch = 0; ch < numChannels; ch++) {
            const float* src = sourceSegment.getReadPointer(ch);
            float* dst = result.getWritePointer(ch);
            for (int i = 0; i < remaining; i++) {
                int srcIdx = bestSrc + i;
                if (srcIdx < sourceLength)
                    dst[outPos + i] += src[srcIdx] * hann[i];
            }
        }
        for (int i = 0; i < remaining; i++)
            weightSum[outPos + i] += hann[i];

        outPos += hopOut;
        srcPos += hopIn;
    }

    // Normalize by overlap weight
    for (int ch = 0; ch < numChannels; ch++) {
        float* data = result.getWritePointer(ch);
        for (int i = 0; i < clampedTarget; i++) {
            if (weightSum[i] > 1e-6f)
                data[i] /= weightSum[i];
        }
    }

    // Trim to target length
    juce::AudioBuffer<float> trimmed(numChannels, targetLength);
    trimmed.clear();
    int copyLen = std::min(clampedTarget, targetLength);
    for (int ch = 0; ch < numChannels; ch++)
        trimmed.copyFrom(ch, 0, result, ch, 0, copyLen);

    return trimmed;
}

// ── Main Quantize ──

QuantizeResult BeatQuantizer::quantize(
    const juce::AudioBuffer<float>& source,
    double sampleRate, double bpm,
    double gridDivision, float quantizeStrength) {

    QuantizeResult result;
    int totalSamples = source.getNumSamples();
    int numChannels = source.getNumChannels();

    if (totalSamples == 0 || bpm <= 0.0) {
        result.errorMessage = "Invalid input";
        return result;
    }

    // Step 1: Detect onsets
    auto onsets = detectOnsets(source, sampleRate);

    if (onsets.size() < 2) {
        result.quantizedBuffer = std::make_unique<juce::AudioBuffer<float>>(numChannels, totalSamples);
        for (int ch = 0; ch < numChannels; ch++)
            result.quantizedBuffer->copyFrom(ch, 0, source, ch, 0, totalSamples);
        result.detectedOnsets = onsets;
        result.numOnsetsDetected = 0;
        result.success = true;
        result.errorMessage = "No transients detected; audio unchanged";
        return result;
    }

    // Step 2: Snap onsets to grid
    snapToGrid(onsets, sampleRate, bpm, gridDivision, quantizeStrength);

    // Calculate output length: last onset target + remaining tail after last onset
    int lastOnsetTarget = onsets.back().targetSamplePosition;
    int lastSegmentTail = totalSamples - onsets.back().samplePosition;
    int outputLength = lastOnsetTarget + lastSegmentTail;
    if (outputLength < 1) outputLength = totalSamples;

    // Build end marker at the computed output end
    OnsetInfo endMarker;
    endMarker.samplePosition = totalSamples;
    endMarker.timeSeconds = static_cast<double>(totalSamples) / sampleRate;
    endMarker.nearestBeatTime = static_cast<double>(outputLength) / sampleRate;
    endMarker.targetSamplePosition = outputLength;
    onsets.push_back(endMarker);

    // Step 3: WSOLA stretch each segment
    auto outputBuffer = std::make_unique<juce::AudioBuffer<float>>(numChannels, outputLength);
    outputBuffer->clear();

    int numSegments = static_cast<int>(onsets.size()) - 1;

    for (int seg = 0; seg < numSegments; seg++) {
        int srcStart = onsets[seg].samplePosition;
        int srcEnd = onsets[seg + 1].samplePosition;
        int srcLen = srcEnd - srcStart;

        int dstStart = onsets[seg].targetSamplePosition;
        int dstEnd = onsets[seg + 1].targetSamplePosition;
        int dstLen = dstEnd - dstStart;

        if (srcLen <= 0 || dstLen <= 0) continue;

        juce::AudioBuffer<float> segment(numChannels, srcLen);
        for (int ch = 0; ch < numChannels; ch++)
            segment.copyFrom(ch, 0, source, ch, srcStart, srcLen);

        auto stretched = wsolaStretch(segment, dstLen);

        int copyLen = std::min(stretched.getNumSamples(), outputLength - dstStart);
        if (copyLen > 0 && dstStart >= 0) {
            for (int ch = 0; ch < numChannels; ch++)
                outputBuffer->copyFrom(ch, dstStart, stretched, ch, 0, copyLen);
        }
    }

    // Remove end marker from reported onsets
    onsets.pop_back();

    result.quantizedBuffer = std::move(outputBuffer);
    result.detectedOnsets = onsets;
    result.numOnsetsDetected = static_cast<int>(onsets.size()) - 1;
    result.success = true;
    return result;
}

// ── Autocorrelation ──

std::vector<float> BeatQuantizer::computeAutocorrelation(
    const std::vector<float>& signal, int maxLag) {
    int n = static_cast<int>(signal.size());
    std::vector<float> ac(maxLag, 0.0f);

    for (int lag = 0; lag < maxLag; lag++) {
        float sum = 0.0f;
        int count = n - lag;
        for (int i = 0; i < count; i++)
            sum += signal[i] * signal[i + lag];
        ac[lag] = sum / std::max(1, count);
    }
    return ac;
}

// ── BPM Detection via Autocorrelation of Spectral Flux ──

double BeatQuantizer::detectBPM(
    const juce::AudioBuffer<float>& source, double sampleRate,
    double minBPM, double maxBPM) {

    int numSamples = source.getNumSamples();
    int numChannels = source.getNumChannels();

    // Mix to mono
    std::vector<float> mono(numSamples, 0.0f);
    for (int ch = 0; ch < numChannels; ch++) {
        const float* data = source.getReadPointer(ch);
        for (int i = 0; i < numSamples; i++)
            mono[i] += data[i];
    }
    float scale = 1.0f / std::max(1, numChannels);
    for (auto& s : mono) s *= scale;

    // Compute spectral flux
    auto flux = computeSpectralFlux(mono.data(), numSamples);
    if (flux.size() < 16) return 0.0;

    // Frames per second
    double fps = sampleRate / HOP_SIZE;

    // Lag range for minBPM–maxBPM
    int minLag = static_cast<int>(fps * 60.0 / maxBPM);
    int maxLag = static_cast<int>(fps * 60.0 / minBPM);
    maxLag = std::min(maxLag, static_cast<int>(flux.size()) / 2);
    if (minLag >= maxLag) return 0.0;

    // Autocorrelation
    auto ac = computeAutocorrelation(flux, maxLag + 1);

    // Find strongest peak in the BPM range
    float bestVal = -1.0f;
    int bestLag = minLag;
    for (int lag = minLag; lag <= maxLag; lag++) {
        if (ac[lag] > bestVal) {
            bestVal = ac[lag];
            bestLag = lag;
        }
    }

    double rawBPM = 60.0 * fps / bestLag;

    // Resolve octave ambiguity: check half-period (double tempo)
    int halfLag = bestLag / 2;
    if (halfLag >= minLag && halfLag < static_cast<int>(ac.size())) {
        float halfVal = ac[halfLag];
        // Prefer double tempo if its peak is strong relative to the fundamental
        if (halfVal > bestVal * 0.8f) {
            double doubleBPM = 60.0 * fps / halfLag;
            if (doubleBPM <= maxBPM)
                rawBPM = doubleBPM;
        }
    }

    // Also check double-period (half tempo) for tracks detected too fast
    int doubleLag = bestLag * 2;
    if (doubleLag <= maxLag && doubleLag < static_cast<int>(ac.size())) {
        float doubleVal = ac[doubleLag];
        if (doubleVal > bestVal * 0.9f && rawBPM > 160.0) {
            rawBPM = 60.0 * fps / doubleLag;
        }
    }

    return rawBPM;
}

// ── Tempo Match: Detect BPM + Uniform Time-Stretch ──

TempoMatchResult BeatQuantizer::tempoMatch(
    const juce::AudioBuffer<float>& source,
    double sampleRate, double targetBPM,
    double manualSourceBPM, double minBPM, double maxBPM) {

    TempoMatchResult result;
    result.targetBPM = targetBPM;

    int numSamples = source.getNumSamples();
    int numChannels = source.getNumChannels();

    if (numSamples == 0 || targetBPM <= 0.0) {
        result.errorMessage = "Invalid input";
        return result;
    }

    // Step 1: Detect or use manual source BPM
    double sourceBPM = (manualSourceBPM > 0.0) ? manualSourceBPM : detectBPM(source, sampleRate, minBPM, maxBPM);
    result.detectedBPM = sourceBPM;

    if (sourceBPM <= 0.0) {
        result.errorMessage = "Could not detect BPM";
        return result;
    }

    // Step 2: Compute stretch ratio
    double ratio = targetBPM / sourceBPM;
    result.stretchRatio = ratio;

    // If essentially identical (within 0.5%), return a copy
    if (ratio > 0.995 && ratio < 1.005) {
        result.stretchedBuffer = std::make_unique<juce::AudioBuffer<float>>(numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ch++)
            result.stretchedBuffer->copyFrom(ch, 0, source, ch, 0, numSamples);
        result.success = true;
        result.errorMessage = "BPM already matches target";
        return result;
    }

    // Step 3: Uniform time-stretch via WSOLA in segments
    // targetLength = sourceLength * sourceBPM / targetBPM = sourceLength / ratio
    int targetLength = static_cast<int>(numSamples / ratio);
    if (targetLength < 1) targetLength = 1;

    // For long audio, process in segments to maintain quality
    static constexpr int SEGMENT_SAMPLES = 1323000;  // ~30s at 44100
    static constexpr int OVERLAP_SAMPLES = 44100;     // 1s overlap

    if (numSamples <= SEGMENT_SAMPLES * 2) {
        // Short enough to process in one shot
        auto stretched = wsolaStretch(source, targetLength);
        result.stretchedBuffer = std::make_unique<juce::AudioBuffer<float>>(
            stretched.getNumChannels(), stretched.getNumSamples());
        for (int ch = 0; ch < stretched.getNumChannels(); ch++)
            result.stretchedBuffer->copyFrom(ch, 0, stretched, ch, 0, stretched.getNumSamples());
    } else {
        // Process in overlapping segments
        result.stretchedBuffer = std::make_unique<juce::AudioBuffer<float>>(numChannels, targetLength);
        result.stretchedBuffer->clear();

        int step = SEGMENT_SAMPLES - OVERLAP_SAMPLES;
        int srcPos = 0;

        // Hann crossfade window for overlap region
        std::vector<float> fadeIn(OVERLAP_SAMPLES);
        std::vector<float> fadeOut(OVERLAP_SAMPLES);
        for (int i = 0; i < OVERLAP_SAMPLES; i++) {
            float t = static_cast<float>(i) / OVERLAP_SAMPLES;
            fadeIn[i] = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * t));
            fadeOut[i] = 1.0f - fadeIn[i];
        }

        while (srcPos < numSamples) {
            int segLen = std::min(SEGMENT_SAMPLES, numSamples - srcPos);
            int segTarget = static_cast<int>(segLen / ratio);
            if (segTarget < 1) segTarget = 1;

            juce::AudioBuffer<float> segment(numChannels, segLen);
            for (int ch = 0; ch < numChannels; ch++)
                segment.copyFrom(ch, 0, source, ch, srcPos, segLen);

            auto stretched = wsolaStretch(segment, segTarget);

            int dstPos = static_cast<int>(srcPos / ratio);
            int copyLen = std::min(stretched.getNumSamples(), targetLength - dstPos);
            if (copyLen <= 0) break;

            // Apply overlap crossfade with previous segment
            int overlapTarget = static_cast<int>(OVERLAP_SAMPLES / ratio);
            if (srcPos > 0 && overlapTarget > 0) {
                int fadeLen = std::min(overlapTarget, copyLen);
                for (int ch = 0; ch < numChannels; ch++) {
                    float* dst = result.stretchedBuffer->getWritePointer(ch);
                    const float* src = stretched.getReadPointer(ch);
                    for (int i = 0; i < fadeLen; i++) {
                        float t = static_cast<float>(i) / fadeLen;
                        float fi = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * t));
                        dst[dstPos + i] = dst[dstPos + i] * (1.0f - fi) + src[i] * fi;
                    }
                    // Copy rest after fade
                    for (int i = fadeLen; i < copyLen; i++) {
                        if (dstPos + i < targetLength)
                            dst[dstPos + i] = src[i];
                    }
                }
            } else {
                for (int ch = 0; ch < numChannels; ch++)
                    result.stretchedBuffer->copyFrom(ch, dstPos, stretched, ch, 0, copyLen);
            }

            srcPos += step;
        }
    }

    result.success = true;
    return result;
}
