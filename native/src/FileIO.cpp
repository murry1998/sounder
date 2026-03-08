#include "FileIO.h"
#include "AudioGraph.h"
#include "TransportEngine.h"
#include "MidiTrack.h"
#include "BuiltInInstrument.h"
#include "MidiInputManager.h"

std::unique_ptr<juce::AudioBuffer<float>> FileIO::readAudioFile(
    const std::string& path, double targetSampleRate)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    juce::File file(path);
    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(file));

    if (!reader) return nullptr;

    int numSamples = static_cast<int>(reader->lengthInSamples);
    int numChannels = std::min(static_cast<int>(reader->numChannels), 2);

    auto buffer = std::make_unique<juce::AudioBuffer<float>>(numChannels, numSamples);
    reader->read(buffer.get(), 0, numSamples, 0, true, numChannels > 1);

    if (std::abs(reader->sampleRate - targetSampleRate) > 1.0) {
        double ratio = targetSampleRate / reader->sampleRate;
        int newLength = static_cast<int>(numSamples * ratio);
        auto resampled = std::make_unique<juce::AudioBuffer<float>>(numChannels, newLength);

        for (int ch = 0; ch < numChannels; ch++) {
            auto* src = buffer->getReadPointer(ch);
            auto* dst = resampled->getWritePointer(ch);
            for (int i = 0; i < newLength; i++) {
                double srcIdx = i / ratio;
                int idx0 = static_cast<int>(srcIdx);
                int idx1 = std::min(idx0 + 1, numSamples - 1);
                float frac = static_cast<float>(srcIdx - idx0);
                dst[i] = src[idx0] * (1.0f - frac) + src[idx1] * frac;
            }
        }
        return resampled;
    }

    return buffer;
}

bool FileIO::writeWAV(const std::string& path,
    const juce::AudioBuffer<float>& buffer, double sampleRate, int bitDepth)
{
    juce::File file(path);
    file.deleteFile();

    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (!stream) return false;

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(stream.get(), sampleRate,
            static_cast<unsigned int>(buffer.getNumChannels()), bitDepth, {}, 0));

    if (!writer) return false;
    stream.release();
    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    return true;
}

bool FileIO::writeAIFF(const std::string& path,
    const juce::AudioBuffer<float>& buffer, double sampleRate, int bitDepth)
{
    juce::File file(path);
    file.deleteFile();

    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (!stream) return false;

    juce::AiffAudioFormat aiffFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        aiffFormat.createWriterFor(stream.get(), sampleRate,
            static_cast<unsigned int>(buffer.getNumChannels()), bitDepth, {}, 0));

    if (!writer) return false;
    stream.release();
    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    return true;
}

bool FileIO::writeMP3(const std::string& path,
    const juce::AudioBuffer<float>& buffer, double sampleRate, int bitrate)
{
#if JUCE_USE_LAME_AUDIO_FORMAT
    juce::File lameExe;
    const char* lamePaths[] = {
        "/opt/homebrew/bin/lame",
        "/usr/local/bin/lame",
        "/usr/bin/lame"
    };
    for (auto& p : lamePaths) {
        juce::File candidate(p);
        if (candidate.existsAsFile()) { lameExe = candidate; break; }
    }
    if (!lameExe.existsAsFile())
        return false;

    juce::File file(path);
    file.deleteFile();

    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (!stream) return false;

    juce::LAMEEncoderAudioFormat mp3Format(lameExe);

    // Quality indices: 0-9 = VBR, then CBR rates at index 10+:
    // [32,40,48,56,64,80,96,112,128,160,192,224,256,320]
    int qualityIndex = 23; // 320 kbps
    switch (bitrate) {
        case 128: qualityIndex = 18; break;
        case 192: qualityIndex = 20; break;
        case 256: qualityIndex = 22; break;
        case 320: qualityIndex = 23; break;
        default:  qualityIndex = 23; break;
    }

    juce::OutputStream* rawStream = stream.release();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        mp3Format.createWriterFor(rawStream, sampleRate,
            static_cast<unsigned int>(buffer.getNumChannels()), 16, {}, qualityIndex));

    if (!writer) { delete rawStream; return false; }
    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    return true;
#else
    juce::ignoreUnused(path, buffer, sampleRate, bitrate);
    return false;
#endif
}

bool FileIO::saveProject(const std::string& projectsDir, const std::string& projectName,
    const AudioGraph& graph, const TransportEngine& transport, int bpm, int timeSigNum)
{
    juce::File dir(projectsDir + "/" + projectName);
    dir.createDirectory();

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("name", juce::String(projectName));
    root->setProperty("bpm", bpm);
    root->setProperty("timeSignature", timeSigNum);
    root->setProperty("masterVolume", graph.getMasterVolume());
    root->setProperty("createdAt", juce::Time::getCurrentTime().toISO8601(true));
    root->setProperty("updatedAt", juce::Time::getCurrentTime().toISO8601(true));

    juce::Array<juce::var> trackArray;
    for (auto& [id, track] : graph.getTracks()) {
        juce::DynamicObject::Ptr trackObj = new juce::DynamicObject();
        trackObj->setProperty("id", id);
        trackObj->setProperty("name", juce::String(track->getName()));
        trackObj->setProperty("volume", track->getVolume());
        trackObj->setProperty("pan", track->getPan());
        trackObj->setProperty("muted", track->isMuted());
        trackObj->setProperty("soloed", track->isSoloed());

        if (track->hasBuffer()) {
            juce::String audioFile = juce::String("track_") + juce::String(id) + ".wav";
            trackObj->setProperty("audioFile", audioFile);
            juce::File wavFile = dir.getChildFile(audioFile);
            writeWAV(wavFile.getFullPathName().toStdString(),
                     track->getBuffer(), track->getBufferSampleRate(), 24);
        }

        // Region fields
        trackObj->setProperty("regionOffset", track->getRegionOffset());
        trackObj->setProperty("regionClipStart", track->getRegionClipStart());
        trackObj->setProperty("regionClipEnd", track->getRegionClipEnd());
        trackObj->setProperty("regionLoopEnabled", track->getRegionLoopEnabled());
        trackObj->setProperty("fadeIn", track->getFadeIn());
        trackObj->setProperty("fadeOut", track->getFadeOut());

        trackArray.add(juce::var(trackObj.get()));
    }
    root->setProperty("tracks", trackArray);

    // Serialize MIDI tracks
    juce::Array<juce::var> midiTrackArray;
    for (auto& [id, mtrack] : graph.getMidiTracks()) {
        juce::DynamicObject::Ptr mobj = new juce::DynamicObject();
        mobj->setProperty("id", id);
        mobj->setProperty("type", juce::String("midi"));
        mobj->setProperty("name", juce::String(mtrack->getName()));
        mobj->setProperty("volume", mtrack->getVolume());
        mobj->setProperty("pan", mtrack->getPan());
        mobj->setProperty("muted", mtrack->isMuted());
        mobj->setProperty("soloed", mtrack->isSoloed());

        // Instrument info
        auto* inst = mtrack->getInstrument();
        if (auto* builtIn = dynamic_cast<BuiltInInstrument*>(inst)) {
            juce::String instName = builtIn->getName();
            std::string instType;
            if (instName == "BasicSynth") instType = "basicSynth";
            else if (instName == "SamplePlayer") instType = "samplePlayer";
            else if (instName == "DrumKit") instType = "drumKit";
            mobj->setProperty("instrumentType", juce::String(instType));
            mobj->setProperty("instrumentName", instName);

            // Serialize built-in params
            juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
            static const std::vector<std::string> synthParams = {
                "osc1Waveform","osc2Waveform","osc2Detune","oscMix",
                "filterType","filterCutoff","filterResonance",
                "ampAttack","ampDecay","ampSustain","ampRelease",
                "filtEnvAttack","filtEnvDecay","filtEnvSustain","filtEnvRelease","filtEnvDepth"
            };
            for (auto& p : synthParams) {
                paramsObj->setProperty(juce::String(p), builtIn->getParam(p));
            }
            mobj->setProperty("instrumentParams", juce::var(paramsObj.get()));
        }

        // Serialize notes
        auto notes = mtrack->getAllNotes();
        juce::Array<juce::var> noteArray;
        for (auto& note : notes) {
            juce::DynamicObject::Ptr nobj = new juce::DynamicObject();
            nobj->setProperty("note", note.noteNumber);
            nobj->setProperty("start", note.startBeat);
            nobj->setProperty("length", note.lengthBeats);
            nobj->setProperty("velocity", note.velocity);
            nobj->setProperty("channel", note.channel);
            noteArray.add(juce::var(nobj.get()));
        }
        mobj->setProperty("notes", noteArray);

        midiTrackArray.add(juce::var(mobj.get()));
    }
    root->setProperty("midiTracks", midiTrackArray);

    juce::String json = juce::JSON::toString(juce::var(root.get()));
    juce::File jsonFile = dir.getChildFile("project.json");
    jsonFile.replaceWithText(json);

    return true;
}

bool FileIO::loadProject(const std::string& projectsDir, const std::string& projectId,
    AudioGraph& graph, TransportEngine& transport, int& bpm, int& timeSigNum)
{
    juce::File dir(projectsDir + "/" + projectId);
    juce::File jsonFile = dir.getChildFile("project.json");
    if (!jsonFile.existsAsFile()) return false;

    auto parsed = juce::JSON::parse(jsonFile.loadFileAsString());
    if (auto* obj = parsed.getDynamicObject()) {
        bpm = obj->getProperty("bpm");
        timeSigNum = obj->getProperty("timeSignature");
        graph.setMasterVolume(obj->getProperty("masterVolume"));

        // Load audio tracks
        if (auto* tracks = obj->getProperty("tracks").getArray()) {
            for (auto& trackVar : *tracks) {
                if (auto* trackObj = trackVar.getDynamicObject()) {
                    std::string name = trackObj->getProperty("name").toString().toStdString();
                    int id = graph.addTrack(name);
                    auto* track = graph.getTrack(id);
                    if (track) {
                        track->setVolume(trackObj->getProperty("volume"));
                        track->setPan(trackObj->getProperty("pan"));
                        track->setMute(trackObj->getProperty("muted"));
                        track->setSolo(trackObj->getProperty("soloed"));

                        if (trackObj->hasProperty("audioFile")) {
                            juce::String audioFile = trackObj->getProperty("audioFile").toString();
                            juce::File wavFile = dir.getChildFile(audioFile);
                            if (wavFile.existsAsFile()) {
                                double sr = transport.getSampleRate();
                                if (sr <= 0) sr = 48000.0;
                                auto buf = readAudioFile(wavFile.getFullPathName().toStdString(), sr);
                                if (buf) track->setBuffer(std::move(buf), sr);
                            }
                        }

                        // Restore region fields
                        if (trackObj->hasProperty("regionOffset"))
                            track->setRegionOffset(trackObj->getProperty("regionOffset"));
                        if (trackObj->hasProperty("regionClipStart"))
                            track->setRegionClipStart(trackObj->getProperty("regionClipStart"));
                        if (trackObj->hasProperty("regionClipEnd"))
                            track->setRegionClipEnd(trackObj->getProperty("regionClipEnd"));
                        if (trackObj->hasProperty("regionLoopEnabled"))
                            track->setRegionLoopEnabled(trackObj->getProperty("regionLoopEnabled"));
                        if (trackObj->hasProperty("fadeIn"))
                            track->setFadeIn((double)trackObj->getProperty("fadeIn"));
                        if (trackObj->hasProperty("fadeOut"))
                            track->setFadeOut((double)trackObj->getProperty("fadeOut"));
                    }
                }
            }
        }

        // Load MIDI tracks
        if (auto* midiTracks = obj->getProperty("midiTracks").getArray()) {
            for (auto& mtVar : *midiTracks) {
                if (auto* mtObj = mtVar.getDynamicObject()) {
                    std::string name = mtObj->getProperty("name").toString().toStdString();
                    int id = graph.addMidiTrack(name);
                    auto* mtrack = graph.getMidiTrack(id);
                    if (!mtrack) continue;

                    mtrack->setVolume(mtObj->getProperty("volume"));
                    mtrack->setPan(mtObj->getProperty("pan"));
                    mtrack->setMute(mtObj->getProperty("muted"));
                    mtrack->setSolo(mtObj->getProperty("soloed"));

                    // Restore notes
                    if (auto* noteArr = mtObj->getProperty("notes").getArray()) {
                        for (auto& nVar : *noteArr) {
                            if (auto* nObj = nVar.getDynamicObject()) {
                                mtrack->addNote(
                                    (int)nObj->getProperty("note"),
                                    (double)nObj->getProperty("start"),
                                    (double)nObj->getProperty("length"),
                                    (int)nObj->getProperty("velocity"),
                                    (int)nObj->getProperty("channel")
                                );
                            }
                        }
                    }

                    // Instrument type and params are restored via JS/N-API layer
                    // (instrumentType and instrumentParams are in the JSON for JS to read)
                }
            }
        }
    }
    return true;
}

std::vector<ProjectInfo> FileIO::listProjects(const std::string& projectsDir) {
    std::vector<ProjectInfo> result;
    juce::File dir(projectsDir);
    for (auto& child : dir.findChildFiles(juce::File::findDirectories, false)) {
        juce::File jsonFile = child.getChildFile("project.json");
        if (!jsonFile.existsAsFile()) continue;
        auto parsed = juce::JSON::parse(jsonFile.loadFileAsString());
        if (auto* obj = parsed.getDynamicObject()) {
            ProjectInfo info;
            info.id = child.getFileName().toStdString();
            info.name = obj->getProperty("name").toString().toStdString();
            info.date = obj->getProperty("updatedAt").toString().toStdString();
            if (auto* tracks = obj->getProperty("tracks").getArray())
                info.trackCount = tracks->size();
            result.push_back(info);
        }
    }
    return result;
}

bool FileIO::deleteProject(const std::string& projectsDir, const std::string& projectId) {
    juce::File dir(projectsDir + "/" + projectId);
    return dir.deleteRecursively();
}

juce::MidiMessageSequence FileIO::readMidiFile(const std::string& path, int trackIndex) {
    juce::MidiMessageSequence result;
    juce::File file(path);
    if (!file.existsAsFile()) return result;

    juce::FileInputStream stream(file);
    if (!stream.openedOk()) return result;

    juce::MidiFile midiFile;
    if (!midiFile.readFrom(stream)) return result;

    midiFile.convertTimestampTicksToSeconds();

    if (trackIndex < midiFile.getNumTracks()) {
        result = *midiFile.getTrack(trackIndex);
    }
    return result;
}

bool FileIO::writeMidiFile(const std::string& path, const juce::MidiMessageSequence& sequence,
                           double bpm, int timeSigNum)
{
    juce::File file(path);
    file.deleteFile();

    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(480);

    // Add tempo track
    juce::MidiMessageSequence tempoTrack;
    double microsecondsPerBeat = 60000000.0 / bpm;
    auto tempoEvent = juce::MidiMessage::tempoMetaEvent(static_cast<int>(microsecondsPerBeat));
    tempoEvent.setTimeStamp(0);
    tempoTrack.addEvent(tempoEvent);

    auto timeSigEvent = juce::MidiMessage::timeSignatureMetaEvent(timeSigNum, 2);
    timeSigEvent.setTimeStamp(0);
    tempoTrack.addEvent(timeSigEvent);
    midiFile.addTrack(tempoTrack);

    // Convert note timestamps from seconds to ticks
    double ticksPerSecond = 480.0 * bpm / 60.0;
    juce::MidiMessageSequence tickSequence;
    for (int i = 0; i < sequence.getNumEvents(); i++) {
        auto msg = sequence.getEventPointer(i)->message;
        msg.setTimeStamp(msg.getTimeStamp() * ticksPerSecond);
        tickSequence.addEvent(msg);
    }
    midiFile.addTrack(tickSequence);

    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (!stream) return false;

    return midiFile.writeTo(*stream);
}
