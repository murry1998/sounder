#include "OfflineRenderer.h"
#include "AudioGraph.h"
#include "FileIO.h"
#include <cmath>

// static
bool OfflineRenderer::writeWithFormat(const std::string& filePath,
                                       const juce::AudioBuffer<float>& buffer,
                                       double sampleRate, const ExportOptions& options) {
    if (options.format == "mp3")
        return FileIO::writeMP3(filePath, buffer, sampleRate, options.mp3Bitrate);
    else if (options.format == "aiff")
        return FileIO::writeAIFF(filePath, buffer, sampleRate, options.bitDepth);
    else
        return FileIO::writeWAV(filePath, buffer, sampleRate, options.bitDepth);
}

ExportProgress OfflineRenderer::exportTracks(AudioGraph& graph, const ExportOptions& options,
                                              ProgressCallback progress) {
    ExportProgress result;
    double sampleRate = 48000.0;
    int blockSize = 512;

    // Get sample rate from current device
    auto* dev = graph.getDeviceManager().getCurrentAudioDevice();
    if (dev) {
        sampleRate = dev->getCurrentSampleRate();
        blockSize = dev->getCurrentBufferSizeSamples();
    }

    double bpm = graph.getBPM();

    // File extension based on format
    std::string ext = ".wav";
    if (options.format == "mp3") ext = ".mp3";
    else if (options.format == "aiff") ext = ".aiff";

    // Determine time range
    double startTime = options.startTime;
    double endTime = options.endTime;
    if (endTime <= startTime) {
        // Find longest track duration
        for (auto& [id, track] : graph.getTracks()) {
            double d = track->getDuration();
            if (d > endTime) endTime = d;
        }
        // MIDI tracks: estimate from note data (use 60 seconds as fallback)
        for (auto& [id, mtrack] : graph.getMidiTracks()) {
            // Each MIDI track's duration depends on note positions + BPM
            // Use a generous estimate
            endTime = std::max(endTime, 30.0);
        }
        if (endTime <= 0.0) endTime = 1.0;
    }

    // Count total tracks to export
    int totalTracks = 0;
    if (options.exportStems) {
        totalTracks += static_cast<int>(graph.getTracks().size());
        totalTracks += static_cast<int>(graph.getMidiTracks().size());
    }
    if (options.exportMixdown) totalTracks++;

    result.totalTracks = totalTracks;
    int trackIndex = 0;

    // Export individual stems
    if (options.exportStems) {
        for (auto& [id, track] : graph.getTracks()) {
            std::string fileName = options.filePrefix.empty()
                ? track->getName() : options.filePrefix + "_" + track->getName();
            // Sanitize filename
            for (auto& c : fileName) if (c == '/' || c == '\\' || c == ':') c = '_';
            std::string filePath = options.outputDir + "/" + fileName + ext;

            result.currentTrack = trackIndex + 1;
            result.currentFile = fileName + ext;
            result.progress = static_cast<float>(trackIndex) / totalTracks;
            if (progress) progress(result);

            bool ok = renderTrackToFile(graph, id, false, filePath, sampleRate,
                                         blockSize, startTime, endTime, options, bpm);
            if (!ok) {
                result.error = true;
                result.errorMessage = "Failed to export: " + fileName;
                return result;
            }
            trackIndex++;
        }
        for (auto& [id, mtrack] : graph.getMidiTracks()) {
            std::string fileName = options.filePrefix.empty()
                ? mtrack->getName() : options.filePrefix + "_" + mtrack->getName();
            for (auto& c : fileName) if (c == '/' || c == '\\' || c == ':') c = '_';
            std::string filePath = options.outputDir + "/" + fileName + ext;

            result.currentTrack = trackIndex + 1;
            result.currentFile = fileName + ext;
            result.progress = static_cast<float>(trackIndex) / totalTracks;
            if (progress) progress(result);

            bool ok = renderTrackToFile(graph, id, true, filePath, sampleRate,
                                         blockSize, startTime, endTime, options, bpm);
            if (!ok) {
                result.error = true;
                result.errorMessage = "Failed to export: " + fileName;
                return result;
            }
            trackIndex++;
        }
    }

    // Export mixdown
    if (options.exportMixdown) {
        std::string fileName = options.filePrefix.empty() ? "Mixdown" : options.filePrefix + "_Mixdown";
        std::string filePath = options.outputDir + "/" + fileName + ext;

        result.currentTrack = trackIndex + 1;
        result.currentFile = fileName + ext;
        result.progress = static_cast<float>(trackIndex) / totalTracks;
        if (progress) progress(result);

        bool ok = renderMixdownToFile(graph, filePath, sampleRate, blockSize,
                                       startTime, endTime, options, bpm);
        if (!ok) {
            result.error = true;
            result.errorMessage = "Failed to export mixdown";
            return result;
        }
    }

    result.progress = 1.0f;
    result.complete = true;
    if (progress) progress(result);
    return result;
}

bool OfflineRenderer::renderTrackToFile(AudioGraph& graph, int trackId, bool isMidi,
                                         const std::string& filePath, double sampleRate,
                                         int blockSize, double startTime, double endTime,
                                         const ExportOptions& options, double bpm) {
    int totalSamples = static_cast<int>((endTime - startTime) * sampleRate);
    if (totalSamples <= 0) return false;

    juce::AudioBuffer<float> output(2, totalSamples);
    output.clear();

    // Render block by block
    juce::AudioBuffer<float> tempBuffer(2, blockSize);
    int samplesRendered = 0;

    while (samplesRendered < totalSamples) {
        int samplesThisBlock = std::min(blockSize, totalSamples - samplesRendered);
        tempBuffer.clear();
        double currentTime = startTime + static_cast<double>(samplesRendered) / sampleRate;

        if (isMidi) {
            auto* mtrack = graph.getMidiTrack(trackId);
            if (!mtrack) return false;
            juce::MidiBuffer emptyMidi;
            mtrack->processBlock(tempBuffer, samplesThisBlock, currentTime, bpm, emptyMidi);
        } else {
            auto* track = graph.getTrack(trackId);
            if (!track) return false;
            track->processBlock(tempBuffer, samplesThisBlock, currentTime);
        }

        // Copy rendered samples to output
        for (int ch = 0; ch < 2; ch++) {
            output.copyFrom(ch, samplesRendered, tempBuffer, ch, 0, samplesThisBlock);
        }
        samplesRendered += samplesThisBlock;
    }

    return writeWithFormat(filePath, output, sampleRate, options);
}

bool OfflineRenderer::renderMixdownToFile(AudioGraph& graph, const std::string& filePath,
                                            double sampleRate, int blockSize,
                                            double startTime, double endTime,
                                            const ExportOptions& options, double bpm) {
    int totalSamples = static_cast<int>((endTime - startTime) * sampleRate);
    if (totalSamples <= 0) return false;

    juce::AudioBuffer<float> output(2, totalSamples);
    output.clear();

    juce::AudioBuffer<float> tempBuffer(2, blockSize);
    int samplesRendered = 0;

    while (samplesRendered < totalSamples) {
        int samplesThisBlock = std::min(blockSize, totalSamples - samplesRendered);
        double currentTime = startTime + static_cast<double>(samplesRendered) / sampleRate;

        juce::MidiBuffer emptyMidi;

        // Mix all audio tracks
        for (auto& [id, track] : graph.getTracks()) {
            if (track->isMuted()) continue;
            tempBuffer.clear();
            track->processBlock(tempBuffer, samplesThisBlock, currentTime);
            for (int ch = 0; ch < 2; ch++)
                output.addFrom(ch, samplesRendered, tempBuffer, ch, 0, samplesThisBlock);
        }

        // Mix all MIDI tracks
        for (auto& [id, mtrack] : graph.getMidiTracks()) {
            if (mtrack->isMuted()) continue;
            tempBuffer.clear();
            mtrack->processBlock(tempBuffer, samplesThisBlock, currentTime, bpm, emptyMidi);
            for (int ch = 0; ch < 2; ch++)
                output.addFrom(ch, samplesRendered, tempBuffer, ch, 0, samplesThisBlock);
        }

        samplesRendered += samplesThisBlock;
    }

    // Apply master volume
    output.applyGain(graph.getMasterVolume());

    // Normalize if requested
    if (options.normalize) {
        float peak = 0.0f;
        for (int ch = 0; ch < 2; ch++) {
            const float* data = output.getReadPointer(ch);
            for (int i = 0; i < totalSamples; i++) {
                float absVal = std::abs(data[i]);
                if (absVal > peak) peak = absVal;
            }
        }
        if (peak > 1e-8f) {
            output.applyGain(1.0f / peak);
        }
    }

    return writeWithFormat(filePath, output, sampleRate, options);
}
