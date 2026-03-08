#include "StemSeparator.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <complex>
#include <thread>

StemSeparator::StemSeparator() {
    initWindow();
}

StemSeparator::~StemSeparator() = default;

void StemSeparator::initWindow() {
    window.resize(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; i++) {
        window[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / FFT_SIZE));
    }
}

bool StemSeparator::loadModel(const std::string& modelPath) {
    try {
        env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "StemSeparator");
        sessionOptions = std::make_unique<Ort::SessionOptions>();

        // Use available cores for faster inference
        int numCores = std::max(1, (int)std::thread::hardware_concurrency() / 2);
        sessionOptions->SetIntraOpNumThreads(numCores);
        sessionOptions->SetInterOpNumThreads(1);
        sessionOptions->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // BFCArena crashes with this model; keep disabled
        sessionOptions->DisableCpuMemArena();
        sessionOptions->DisableMemPattern();

        session = std::make_unique<Ort::Session>(*env, modelPath.c_str(), *sessionOptions);
        modelLoaded = true;
        return true;
    } catch (const Ort::Exception& e) {
        DBG("ONNX Runtime error loading model: " << e.what());
        modelLoaded = false;
        return false;
    }
}

bool StemSeparator::isModelLoaded() const {
    return modelLoaded;
}

// ── Sample Rate Conversion ──

juce::AudioBuffer<float> StemSeparator::resample(
    const juce::AudioBuffer<float>& source, double sourceSR, double targetSR) {

    if (std::abs(sourceSR - targetSR) < 1.0)
        return source;

    double ratio = targetSR / sourceSR;
    int srcLen = source.getNumSamples();
    int dstLen = static_cast<int>(std::ceil(srcLen * ratio));
    int numCh = source.getNumChannels();

    juce::AudioBuffer<float> result(numCh, dstLen);

    for (int ch = 0; ch < numCh; ch++) {
        const float* src = source.getReadPointer(ch);
        float* dst = result.getWritePointer(ch);

        // Lanczos-3 windowed sinc interpolation
        constexpr int SINC_HALF = 8;
        for (int i = 0; i < dstLen; i++) {
            double srcPos = i / ratio;
            int srcIdx = static_cast<int>(srcPos);
            double frac = srcPos - srcIdx;

            double sum = 0.0;
            double weightSum = 0.0;
            for (int k = -SINC_HALF + 1; k <= SINC_HALF; k++) {
                int idx = srcIdx + k;
                if (idx < 0 || idx >= srcLen) continue;
                double x = k - frac;
                double sinc = (std::abs(x) < 1e-8) ? 1.0 :
                    std::sin(juce::MathConstants<double>::pi * x) /
                    (juce::MathConstants<double>::pi * x);
                double lanczos = (std::abs(x) < SINC_HALF) ?
                    std::sin(juce::MathConstants<double>::pi * x / SINC_HALF) /
                    (juce::MathConstants<double>::pi * x / SINC_HALF) : 0.0;
                if (std::abs(x) < 1e-8) lanczos = 1.0;
                double w = sinc * lanczos;
                sum += src[idx] * w;
                weightSum += w;
            }
            dst[i] = static_cast<float>(weightSum > 1e-8 ? sum / weightSum : 0.0);
        }
    }
    return result;
}

// ── Center-padded complex STFT matching HTDemucs expectations ──

void StemSeparator::computeComplexSTFT(const float* input, int numSamples,
                                        std::vector<float>& stftReal,
                                        std::vector<float>& stftImag) {
    int pad = FFT_SIZE / 2;
    int paddedLen = numSamples + 2 * pad;
    std::vector<float> padded(paddedLen, 0.0f);
    std::copy(input, input + numSamples, padded.begin() + pad);

    int numFrames = (paddedLen - FFT_SIZE) / HOP_SIZE + 1;
    if (numFrames > NUM_TIME_FRAMES) numFrames = NUM_TIME_FRAMES;

    stftReal.resize(NUM_FREQ_BINS * NUM_TIME_FRAMES, 0.0f);
    stftImag.resize(NUM_FREQ_BINS * NUM_TIME_FRAMES, 0.0f);

    std::vector<float> fftBuffer(FFT_SIZE * 2, 0.0f);

    for (int frame = 0; frame < numFrames; frame++) {
        int offset = frame * HOP_SIZE;

        for (int i = 0; i < FFT_SIZE; i++) {
            int idx = offset + i;
            float sample = (idx < paddedLen) ? padded[idx] : 0.0f;
            fftBuffer[i * 2] = sample * window[i];
            fftBuffer[i * 2 + 1] = 0.0f;
        }

        auto* complexBuf = reinterpret_cast<std::complex<float>*>(fftBuffer.data());
        fft.perform(complexBuf, complexBuf, false);

        for (int bin = 0; bin < NUM_FREQ_BINS; bin++) {
            stftReal[bin * NUM_TIME_FRAMES + frame] = fftBuffer[bin * 2];
            stftImag[bin * NUM_TIME_FRAMES + frame] = fftBuffer[bin * 2 + 1];
        }
    }
}

// ── Process one 10-second segment through the ONNX model ──

void StemSeparator::processSegment(const float* leftIn, const float* rightIn,
                                    std::array<juce::AudioBuffer<float>, NUM_STEMS>& outputs) {
    std::vector<float> realL, imagL, realR, imagR;
    computeComplexSTFT(leftIn, SEGMENT_SAMPLES, realL, imagL);
    computeComplexSTFT(rightIn, SEGMENT_SAMPLES, realR, imagR);

    // Build spec tensor: [1, 2, 2048, 431, 2]
    int specSize = 2 * NUM_FREQ_BINS * NUM_TIME_FRAMES * 2;
    std::vector<float> specInput(specSize);

    for (int bin = 0; bin < NUM_FREQ_BINS; bin++) {
        for (int frame = 0; frame < NUM_TIME_FRAMES; frame++) {
            int srcIdx = bin * NUM_TIME_FRAMES + frame;
            int dstBase0 = ((0 * NUM_FREQ_BINS + bin) * NUM_TIME_FRAMES + frame) * 2;
            specInput[dstBase0 + 0] = realL[srcIdx];
            specInput[dstBase0 + 1] = imagL[srcIdx];
            int dstBase1 = ((1 * NUM_FREQ_BINS + bin) * NUM_TIME_FRAMES + frame) * 2;
            specInput[dstBase1 + 0] = realR[srcIdx];
            specInput[dstBase1 + 1] = imagR[srcIdx];
        }
    }

    // Build mix tensor: [1, 2, 441000]
    std::vector<float> mixInput(2 * SEGMENT_SAMPLES);
    std::copy(leftIn, leftIn + SEGMENT_SAMPLES, mixInput.begin());
    std::copy(rightIn, rightIn + SEGMENT_SAMPLES, mixInput.begin() + SEGMENT_SAMPLES);

    Ort::AllocatorWithDefaultOptions allocator;
    size_t numInputs = session->GetInputCount();
    size_t numOutputs = session->GetOutputCount();

    std::vector<std::string> inputNameStrings(numInputs);
    for (size_t i = 0; i < numInputs; i++) {
        auto name = session->GetInputNameAllocated(i, allocator);
        inputNameStrings[i] = name.get();
    }
    std::vector<const char*> inputNames(numInputs);
    for (size_t i = 0; i < numInputs; i++)
        inputNames[i] = inputNameStrings[i].c_str();

    // Find the waveform output by shape [1, 4, 2, 441000]
    std::vector<std::string> outputNameStrings;
    for (size_t i = 0; i < numOutputs; i++) {
        auto name = session->GetOutputNameAllocated(i, allocator);
        auto shape = session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 4 && shape[3] == SEGMENT_SAMPLES) {
            outputNameStrings.push_back(name.get());
            break;
        }
    }
    if (outputNameStrings.empty()) {
        for (size_t i = 0; i < numOutputs; i++) {
            auto name = session->GetOutputNameAllocated(i, allocator);
            outputNameStrings.push_back(name.get());
        }
    }
    std::vector<const char*> outputNames(outputNameStrings.size());
    for (size_t i = 0; i < outputNameStrings.size(); i++)
        outputNames[i] = outputNameStrings[i].c_str();

    auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> inputTensors;

    for (size_t i = 0; i < numInputs; i++) {
        std::string name = inputNameStrings[i];
        if (name == "mix") {
            std::vector<int64_t> mixShape = {1, 2, SEGMENT_SAMPLES};
            inputTensors.push_back(Ort::Value::CreateTensor<float>(
                memoryInfo, mixInput.data(), mixInput.size(),
                mixShape.data(), mixShape.size()));
        } else {
            std::vector<int64_t> specShape = {1, 2, NUM_FREQ_BINS, NUM_TIME_FRAMES, 2};
            inputTensors.push_back(Ort::Value::CreateTensor<float>(
                memoryInfo, specInput.data(), specInput.size(),
                specShape.data(), specShape.size()));
        }
    }

    auto outputTensors = session->Run(
        Ort::RunOptions{nullptr},
        inputNames.data(), inputTensors.data(), inputTensors.size(),
        outputNames.data(), outputNames.size());

    // Extract waveform output: [1, 4, 2, 441000]
    const float* outData = outputTensors[0].GetTensorData<float>();
    int channelStride = SEGMENT_SAMPLES;
    int stemStride = 2 * SEGMENT_SAMPLES;

    for (int s = 0; s < NUM_STEMS; s++) {
        outputs[s].setSize(2, SEGMENT_SAMPLES);
        auto* dstL = outputs[s].getWritePointer(0);
        const float* srcL = outData + s * stemStride;
        std::copy(srcL, srcL + SEGMENT_SAMPLES, dstL);
        auto* dstR = outputs[s].getWritePointer(1);
        const float* srcR = outData + s * stemStride + channelStride;
        std::copy(srcR, srcR + SEGMENT_SAMPLES, dstR);
    }
}

// ── Main entry point ──

StemResult StemSeparator::separate(
    const juce::AudioBuffer<float>& source,
    double sampleRate,
    std::function<void(float)> progressCallback) {

    int totalSamples = source.getNumSamples();
    int numChannels = source.getNumChannels();

    // Ensure stereo
    juce::AudioBuffer<float> stereoSource;
    if (numChannels == 1) {
        stereoSource.setSize(2, totalSamples);
        stereoSource.copyFrom(0, 0, source, 0, 0, totalSamples);
        stereoSource.copyFrom(1, 0, source, 0, 0, totalSamples);
    } else {
        stereoSource.setSize(2, totalSamples);
        stereoSource.copyFrom(0, 0, source, 0, 0, totalSamples);
        stereoSource.copyFrom(1, 0, source, 1, 0, totalSamples);
    }

    // Resample to MODEL_SR if needed
    bool needsResample = std::abs(sampleRate - MODEL_SR) >= 1.0;
    juce::AudioBuffer<float> modelInput;
    if (needsResample) {
        DBG("StemSeparator: resampling from " << sampleRate << " to " << MODEL_SR);
        modelInput = resample(stereoSource, sampleRate, MODEL_SR);
    } else {
        modelInput = std::move(stereoSource);
    }
    int modelSamples = modelInput.getNumSamples();

    // Normalize to [-1, 1]
    float maxAbs = 0.0f;
    for (int ch = 0; ch < 2; ch++) {
        auto* data = modelInput.getReadPointer(ch);
        for (int i = 0; i < modelSamples; i++)
            maxAbs = std::max(maxAbs, std::abs(data[i]));
    }
    float normFactor = (maxAbs > 1e-8f) ? (1.0f / maxAbs) : 1.0f;
    modelInput.applyGain(normFactor);

    // Prepare output buffers at model sample rate
    StemResult result;
    result.vocals = std::make_unique<juce::AudioBuffer<float>>(2, modelSamples);
    result.bass = std::make_unique<juce::AudioBuffer<float>>(2, modelSamples);
    result.drums = std::make_unique<juce::AudioBuffer<float>>(2, modelSamples);
    result.other = std::make_unique<juce::AudioBuffer<float>>(2, modelSamples);
    result.vocals->clear();
    result.bass->clear();
    result.drums->clear();
    result.other->clear();

    // Segment with overlap
    int segmentLen = SEGMENT_SAMPLES;
    int stepSize = segmentLen - OVERLAP_SAMPLES;
    int numSegments = std::max(1, (modelSamples + stepSize - 1) / stepSize);

    std::vector<float> weightSum(modelSamples, 0.0f);

    for (int seg = 0; seg < numSegments; seg++) {
        int startSample = seg * stepSize;
        int endSample = std::min(startSample + segmentLen, modelSamples);
        int segLen = endSample - startSample;

        std::vector<float> segL(segmentLen, 0.0f);
        std::vector<float> segR(segmentLen, 0.0f);
        for (int i = 0; i < segLen; i++) {
            segL[i] = modelInput.getSample(0, startSample + i);
            segR[i] = modelInput.getSample(1, startSample + i);
        }

        std::array<juce::AudioBuffer<float>, NUM_STEMS> segOutputs;
        processSegment(segL.data(), segR.data(), segOutputs);

        // Hann overlap window (smoother than linear)
        std::vector<float> segWindow(segmentLen, 1.0f);
        int fadeLen = OVERLAP_SAMPLES;
        if (seg > 0) {
            for (int i = 0; i < fadeLen && i < segmentLen; i++) {
                float t = static_cast<float>(i) / fadeLen;
                segWindow[i] = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * t));
            }
        }
        if (seg < numSegments - 1) {
            for (int i = 0; i < fadeLen && (segLen - 1 - i) >= 0; i++) {
                float t = static_cast<float>(i) / fadeLen;
                float fadeOut = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * t));
                segWindow[segLen - 1 - i] *= fadeOut;
            }
        }

        // Overlap-add: stem order is drums(0), bass(1), other(2), vocals(3)
        juce::AudioBuffer<float>* stemPtrs[] = {
            result.drums.get(), result.bass.get(),
            result.other.get(), result.vocals.get()
        };

        for (int s = 0; s < NUM_STEMS; s++) {
            for (int ch = 0; ch < 2; ch++) {
                auto* dst = stemPtrs[s]->getWritePointer(ch);
                auto* src = segOutputs[s].getReadPointer(ch);
                for (int i = 0; i < segLen; i++) {
                    dst[startSample + i] += src[i] * segWindow[i];
                }
            }
        }

        for (int i = 0; i < segLen; i++)
            weightSum[startSample + i] += segWindow[i];

        if (progressCallback)
            progressCallback(static_cast<float>(seg + 1) / numSegments);
    }

    // Normalize by overlap weights
    juce::AudioBuffer<float>* stemPtrs[] = {
        result.drums.get(), result.bass.get(),
        result.other.get(), result.vocals.get()
    };
    for (int s = 0; s < NUM_STEMS; s++) {
        for (int ch = 0; ch < 2; ch++) {
            auto* data = stemPtrs[s]->getWritePointer(ch);
            for (int i = 0; i < modelSamples; i++) {
                if (weightSum[i] > 1e-8f)
                    data[i] /= weightSum[i];
            }
        }
    }

    // Undo normalization
    float denorm = 1.0f / normFactor;
    for (auto* buf : stemPtrs)
        buf->applyGain(denorm);

    // Resample back to original sample rate if needed
    if (needsResample) {
        DBG("StemSeparator: resampling stems back to " << sampleRate);
        auto resampleStem = [&](std::unique_ptr<juce::AudioBuffer<float>>& stem) {
            auto resampled = resample(*stem, MODEL_SR, sampleRate);
            auto trimmed = std::make_unique<juce::AudioBuffer<float>>(2, totalSamples);
            trimmed->clear();
            int copyLen = std::min(resampled.getNumSamples(), totalSamples);
            for (int ch = 0; ch < 2; ch++)
                trimmed->copyFrom(ch, 0, resampled, ch, 0, copyLen);
            stem = std::move(trimmed);
        };
        resampleStem(result.vocals);
        resampleStem(result.bass);
        resampleStem(result.drums);
        resampleStem(result.other);
    }

    return result;
}
