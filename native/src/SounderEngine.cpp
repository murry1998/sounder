#include <napi.h>
#include <thread>
#include <chrono>
#include "SounderEngine.h"
#include "FileIO.h"
#include "OfflineRenderer.h"
#include "BasicSynth.h"
#include "SamplePlayer.h"
#include "DrumKit.h"
#include "StemSeparator.h"
#include "BuiltInEffect.h"
#include "SFZInstrument.h"
#include "MidiGenerator.h"

// ── Singleton engine and meter thread ──

static std::unique_ptr<SounderEngine> g_engine;
static Napi::ThreadSafeFunction g_meterTSFN;
static std::unique_ptr<std::thread> g_meterThread;
static std::atomic<bool> g_meterRunning{false};

// No CFRunLoop pump needed: JUCE registers its CFRunLoopSource in
// kCFRunLoopCommonModes, so Electron's main runloop already processes
// JUCE messages. The old CFRunLoop timer caused reentrancy issues with
// Electron's event loop, leading to freezes.

// ── SounderEngine class implementation ──

SounderEngine::SounderEngine() = default;
SounderEngine::~SounderEngine() { shutdown(); }

void SounderEngine::initialize(double sr, int bs, const std::string& projDir) {
    if (running.load()) return;

    sampleRate = sr;
    blockSize = bs;
    projectsDir = projDir;

    juce::initialiseJuce_GUI();

    transport = std::make_unique<TransportEngine>();
    transport->setSampleRate(sampleRate);

    metronome = std::make_unique<Metronome>();
    metronome->prepareSamples(sampleRate);

    pluginHost = std::make_unique<PluginHost>();
    pluginHost->loadPersistedPlugins();

    audioGraph = std::make_unique<AudioGraph>();
    audioGraph->setTransport(transport.get());
    audioGraph->setMetronome(metronome.get());
    audioGraph->openDevice(sampleRate, blockSize);

    midiInput = std::make_unique<MidiInputManager>();
    midiInput->setAudioGraph(audioGraph.get());

    running.store(true);
}

void SounderEngine::shutdown() {
    if (!running.load()) return;
    running.store(false);

    if (midiInput) midiInput->closeAllDevices();
    midiInput.reset();
    if (audioGraph) audioGraph->closeDevice();
    audioGraph.reset();
    pluginHost.reset();
    metronome.reset();
    transport.reset();

    juce::shutdownJuce_GUI();
}

// ── Meter polling thread ──

static void meterThreadFunc() {
    while (g_meterRunning.load()) {
        if (g_engine && g_engine->isRunning()) {
            MeterData data = g_engine->getAudioGraph().getMeterData();
            data.currentTime = g_engine->getTransport().getCurrentTimeInSeconds();
            data.currentBeat = g_engine->getMetronome().getCurrentBeat();

            auto state = g_engine->getTransport().getState();
            if (state == TransportEngine::Playing) data.transportState = "playing";
            else if (state == TransportEngine::Recording) data.transportState = "recording";
            else data.transportState = "stopped";

            data.midiActivity = g_engine->getMidiInput().hasRecentActivity();
            data.lastMidiNote = g_engine->getMidiInput().getLastNote();
            data.lastMidiVelocity = g_engine->getMidiInput().getLastVelocity();

            g_meterTSFN.NonBlockingCall([data](Napi::Env env, Napi::Function callback) {
                auto obj = Napi::Object::New(env);
                obj.Set("masterPeakL", data.masterPeakL);
                obj.Set("masterPeakR", data.masterPeakR);
                obj.Set("masterRmsL", data.masterRmsL);
                obj.Set("masterRmsR", data.masterRmsR);
                obj.Set("currentTime", data.currentTime);
                obj.Set("currentBeat", data.currentBeat);
                obj.Set("transportState", std::string(data.transportState));

                auto tracksArr = Napi::Array::New(env, data.tracks.size());
                for (size_t i = 0; i < data.tracks.size(); i++) {
                    auto t = Napi::Object::New(env);
                    t.Set("trackId", data.tracks[i].trackId);
                    t.Set("peakL", data.tracks[i].peakL);
                    t.Set("peakR", data.tracks[i].peakR);
                    t.Set("rmsL", data.tracks[i].rmsL);
                    t.Set("rmsR", data.tracks[i].rmsR);
                    t.Set("clipping", data.tracks[i].clipping);
                    tracksArr.Set(i, t);
                }
                obj.Set("tracks", tracksArr);
                auto busArr = Napi::Array::New(env, data.busTracks.size());
                for (size_t i = 0; i < data.busTracks.size(); i++) {
                    auto b = Napi::Object::New(env);
                    b.Set("trackId", data.busTracks[i].trackId);
                    b.Set("peakL", data.busTracks[i].peakL);
                    b.Set("peakR", data.busTracks[i].peakR);
                    b.Set("rmsL", data.busTracks[i].rmsL);
                    b.Set("rmsR", data.busTracks[i].rmsR);
                    b.Set("clipping", data.busTracks[i].clipping);
                    busArr.Set(i, b);
                }
                obj.Set("busTracks", busArr);
                obj.Set("midiActivity", data.midiActivity);
                obj.Set("lastMidiNote", data.lastMidiNote);
                obj.Set("lastMidiVelocity", data.lastMidiVelocity);

                callback.Call({obj});
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

// ── Microphone permission (macOS) — implemented in MicPermission.mm ──
Napi::Value NapiRequestMicrophoneAccess(const Napi::CallbackInfo& info);

// ── N-API exported functions ──

static Napi::Value NapiInitialize(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Expected config object").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    auto config = info[0].As<Napi::Object>();
    double sr = config.Has("sampleRate") ? config.Get("sampleRate").As<Napi::Number>().DoubleValue() : 48000.0;
    int bs = config.Has("bufferSize") ? config.Get("bufferSize").As<Napi::Number>().Int32Value() : 512;
    std::string projDir = config.Has("projectsDir") ? config.Get("projectsDir").As<Napi::String>().Utf8Value() : "";

    g_engine = std::make_unique<SounderEngine>();
    g_engine->initialize(sr, bs, projDir);

    return env.Undefined();
}

static Napi::Value NapiShutdown(const Napi::CallbackInfo& info) {
    g_meterRunning.store(false);
    if (g_meterThread && g_meterThread->joinable()) g_meterThread->join();
    g_meterThread.reset();
    if (g_meterTSFN) g_meterTSFN.Release();

    if (g_engine) g_engine->shutdown();
    g_engine.reset();
    return info.Env().Undefined();
}

// ── Transport ──

static Napi::Value NapiPlay(const Napi::CallbackInfo& info) {
    if (g_engine) g_engine->getTransport().play();
    return info.Env().Undefined();
}

static Napi::Value NapiStop(const Napi::CallbackInfo& info) {
    if (!g_engine) return info.Env().Undefined();

    bool wasRecording = (g_engine->getTransport().getState() == TransportEngine::Recording);
    g_engine->getTransport().stop();

    // Finalize recorded buffers and apply to armed tracks
    if (wasRecording) {
        // Audio tracks
        for (auto& [id, track] : g_engine->getAudioGraph().getTracks()) {
            if (track->isArmed()) {
                auto recordedBuf = g_engine->getTransport().finalizeRecording(id);
                if (recordedBuf) {
                    track->setBuffer(std::move(recordedBuf), g_engine->getSampleRate());
                }
            }
        }
        g_engine->getTransport().clearRecordingBuffers();

        // MIDI tracks - finalize recorded notes and add them to the track
        for (auto& [id, mtrack] : g_engine->getAudioGraph().getMidiTracks()) {
            if (mtrack->isArmed()) {
                auto recordedNotes = g_engine->getTransport().finalizeMidiRecording(id);
                for (auto& rn : recordedNotes) {
                    double lengthBeats = rn.endBeat - rn.startBeat;
                    if (lengthBeats < 0.001) lengthBeats = 0.0625;
                    mtrack->addNote(rn.noteNumber, rn.startBeat, lengthBeats, rn.velocity, rn.channel);
                }
            }
        }
        g_engine->getTransport().clearMidiRecordingBuffers();
    }

    return info.Env().Undefined();
}

static Napi::Value NapiRecord(const Napi::CallbackInfo& info) {
    if (!g_engine) return info.Env().Undefined();
    std::vector<int> armedAudioIds;
    std::vector<int> armedMidiIds;
    for (auto& [id, track] : g_engine->getAudioGraph().getTracks()) {
        if (track->isArmed()) armedAudioIds.push_back(id);
    }
    for (auto& [id, mtrack] : g_engine->getAudioGraph().getMidiTracks()) {
        if (mtrack->isArmed()) armedMidiIds.push_back(id);
    }
    if (armedAudioIds.empty() && armedMidiIds.empty()) {
        auto result = Napi::Object::New(info.Env());
        result.Set("error", Napi::String::New(info.Env(), "No tracks armed for recording"));
        return result;
    }
    // Ensure mic input is available if audio tracks are armed
    if (!armedAudioIds.empty())
        g_engine->getAudioGraph().ensureInputEnabled();
    // Set up recording buffers for both audio and MIDI
    g_engine->getTransport().record(armedAudioIds);
    if (!armedMidiIds.empty())
        g_engine->getTransport().recordMidiTracks(armedMidiIds);
    return info.Env().Undefined();
}

static Napi::Value NapiRewind(const Napi::CallbackInfo& info) {
    if (g_engine) g_engine->getTransport().rewind();
    return info.Env().Undefined();
}

static Napi::Value NapiSeekTo(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getTransport().seekTo(info[0].As<Napi::Number>().DoubleValue());
    return info.Env().Undefined();
}

static Napi::Value NapiGetTransportState(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    if (!g_engine) {
        obj.Set("state", "stopped");
        obj.Set("currentTime", 0.0);
        obj.Set("totalDuration", 0.0);
        return obj;
    }
    auto state = g_engine->getTransport().getState();
    if (state == TransportEngine::Playing) obj.Set("state", "playing");
    else if (state == TransportEngine::Recording) obj.Set("state", "recording");
    else obj.Set("state", "stopped");
    obj.Set("currentTime", g_engine->getTransport().getCurrentTimeInSeconds());
    obj.Set("totalDuration", g_engine->getTransport().getTotalDuration());
    return obj;
}

// ── Loop ──

static Napi::Value NapiSetLoopEnabled(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getTransport().setLoopEnabled(info[0].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

static Napi::Value NapiSetLoopRegion(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2)
        g_engine->getTransport().setLoopRegion(
            info[0].As<Napi::Number>().DoubleValue(),
            info[1].As<Napi::Number>().DoubleValue());
    return info.Env().Undefined();
}

// ── Tracks ──

static Napi::Value NapiAddTrack(const Napi::CallbackInfo& info) {
    if (!g_engine) return Napi::Number::New(info.Env(), -1);
    std::string name = (info.Length() >= 1 && info[0].IsString())
        ? info[0].As<Napi::String>().Utf8Value() : "Track";
    int id = g_engine->getAudioGraph().addTrack(name);
    return Napi::Number::New(info.Env(), id);
}

static Napi::Value NapiRemoveTrack(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getAudioGraph().removeTrack(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

static Napi::Value NapiSetTrackVolume(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setVolume(info[1].As<Napi::Number>().FloatValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetTrackPan(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setPan(info[1].As<Napi::Number>().FloatValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetTrackMute(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setMute(info[1].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetTrackSolo(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setSolo(info[1].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetTrackArmed(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setArmed(info[1].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiGetTrackWaveform(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return Napi::Array::New(env, 0);
    auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
    if (!t) return Napi::Array::New(env, 0);
    int numPoints = info[1].As<Napi::Number>().Int32Value();
    auto data = t->getWaveformData(numPoints);
    auto arr = Napi::Float32Array::New(env, data.size());
    for (size_t i = 0; i < data.size(); i++) arr[i] = data[i];
    return arr;
}

static Napi::Value NapiGetTrackDuration(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 1) return Napi::Number::New(env, 0.0);
    auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
    if (!t) return Napi::Number::New(env, 0.0);
    return Napi::Number::New(env, t->getDuration());
}

static Napi::Value NapiImportAudioToTrack(const Napi::CallbackInfo& info) {
    if (!g_engine || info.Length() < 2) return info.Env().Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string path = info[1].As<Napi::String>().Utf8Value();
    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (t) {
        auto buffer = FileIO::readAudioFile(path, g_engine->getSampleRate());
        if (buffer) {
            double fileSR = g_engine->getSampleRate();
            t->setBuffer(std::move(buffer), fileSR);
            double dur = t->getDuration();
            if (dur > g_engine->getTransport().getTotalDuration())
                g_engine->getTransport().setTotalDuration(dur);
        }
    }
    return info.Env().Undefined();
}

// ── Audio Region ──

static Napi::Value NapiSetAudioRegion(const Napi::CallbackInfo& info) {
    if (!g_engine || info.Length() < 5) return info.Env().Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    double offset = info[1].As<Napi::Number>().DoubleValue();
    double clipStart = info[2].As<Napi::Number>().DoubleValue();
    double clipEnd = info[3].As<Napi::Number>().DoubleValue();
    bool loopEnabled = info[4].As<Napi::Boolean>().Value();

    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (t) {
        t->setRegionOffset(offset);
        t->setRegionClipStart(clipStart);
        t->setRegionClipEnd(clipEnd);
        t->setRegionLoopEnabled(loopEnabled);
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSplitAudioTrack(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2)
        return Napi::Object::New(env);

    int trackId = info[0].As<Napi::Number>().Int32Value();
    double splitTime = info[1].As<Napi::Number>().DoubleValue();

    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (!t || !t->hasBuffer()) return Napi::Object::New(env);

    double offset = t->getRegionOffset();
    double clipStart = t->getRegionClipStart();
    double clipEnd = t->getRegionClipEnd();
    double bufDuration = t->getDuration();
    if (clipEnd < 0) clipEnd = bufDuration;

    // Compute where the split falls within the buffer
    double splitInBuffer = clipStart + (splitTime - offset);
    if (splitInBuffer <= clipStart || splitInBuffer >= clipEnd)
        return Napi::Object::New(env); // split outside clip bounds

    int splitSample = static_cast<int>(splitInBuffer * t->getBufferSampleRate());
    int clipStartSample = static_cast<int>(clipStart * t->getBufferSampleRate());
    int clipEndSample = static_cast<int>(clipEnd * t->getBufferSampleRate());

    // Extract right half into a new buffer
    auto rightBuf = t->extractBuffer(splitSample, clipEndSample);
    if (!rightBuf) return Napi::Object::New(env);

    // Trim left track to just the left portion
    t->trimBufferTo(clipStartSample, splitSample);
    t->setRegionClipStart(0.0);
    t->setRegionClipEnd(-1.0);
    t->setRegionLoopEnabled(false);
    // offset stays the same

    // Create new track for the right half
    std::string newName = t->getName() + " (R)";
    int newId = g_engine->getAudioGraph().addTrack(newName);
    auto* newTrack = g_engine->getAudioGraph().getTrack(newId);
    if (newTrack) {
        newTrack->setBuffer(std::move(rightBuf), t->getBufferSampleRate());
        newTrack->setRegionOffset(splitTime);
        newTrack->setRegionClipStart(0.0);
        newTrack->setRegionClipEnd(-1.0);
        newTrack->setVolume(t->getVolume());
        newTrack->setPan(t->getPan());
    }

    auto result = Napi::Object::New(env);
    result.Set("newTrackId", Napi::Number::New(env, newId));
    result.Set("newTrackName", Napi::String::New(env, newName));
    return result;
}

static Napi::Value NapiDuplicateAudioTrack(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 1)
        return Napi::Object::New(env);

    int srcId = info[0].As<Napi::Number>().Int32Value();
    auto* src = g_engine->getAudioGraph().getTrack(srcId);
    if (!src || !src->hasBuffer()) return Napi::Object::New(env);

    // Deep-copy the audio buffer
    const auto& srcBuf = src->getBuffer();
    auto newBuf = std::make_unique<juce::AudioBuffer<float>>(srcBuf.getNumChannels(), srcBuf.getNumSamples());
    for (int ch = 0; ch < srcBuf.getNumChannels(); ch++)
        newBuf->copyFrom(ch, 0, srcBuf, ch, 0, srcBuf.getNumSamples());

    std::string newName = src->getName() + " (copy)";
    int newId = g_engine->getAudioGraph().addTrack(newName);
    auto* newTrack = g_engine->getAudioGraph().getTrack(newId);
    if (newTrack) {
        newTrack->setBuffer(std::move(newBuf), src->getBufferSampleRate());
        newTrack->setVolume(src->getVolume());
        newTrack->setPan(src->getPan());
    }

    auto result = Napi::Object::New(env);
    result.Set("newTrackId", Napi::Number::New(env, newId));
    result.Set("newTrackName", Napi::String::New(env, newName));
    return result;
}

static Napi::Value NapiSetAudioFades(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3)
        return env.Undefined();

    int trackId = info[0].As<Napi::Number>().Int32Value();
    double fadeIn = info[1].As<Napi::Number>().DoubleValue();
    double fadeOut = info[2].As<Napi::Number>().DoubleValue();

    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (t) {
        t->setFadeIn(fadeIn);
        t->setFadeOut(fadeOut);
    }
    return env.Undefined();
}

static Napi::Value NapiGetRecordingWaveform(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) {
        auto obj = Napi::Object::New(env);
        obj.Set("data", Napi::Float32Array::New(env, 0));
        obj.Set("duration", Napi::Number::New(env, 0.0));
        return obj;
    }
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int numPoints = info[1].As<Napi::Number>().Int32Value();

    auto& transport = g_engine->getTransport();
    auto data = transport.getRecordingWaveform(trackId, numPoints);
    int sampleCount = transport.getRecordingSampleCount(trackId);
    double duration = static_cast<double>(sampleCount) / transport.getSampleRate();

    auto arr = Napi::Float32Array::New(env, data.size());
    for (size_t i = 0; i < data.size(); i++) arr[i] = data[i];

    auto obj = Napi::Object::New(env);
    obj.Set("data", arr);
    obj.Set("duration", Napi::Number::New(env, duration));
    return obj;
}

// ── MIDI Tracks ──

static Napi::Value NapiAddMidiTrack(const Napi::CallbackInfo& info) {
    if (!g_engine) return Napi::Number::New(info.Env(), -1);
    std::string name = (info.Length() >= 1 && info[0].IsString())
        ? info[0].As<Napi::String>().Utf8Value() : "MIDI Track";
    int id = g_engine->getAudioGraph().addMidiTrack(name);
    return Napi::Number::New(info.Env(), id);
}

static Napi::Value NapiRemoveMidiTrack(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getAudioGraph().removeMidiTrack(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

static Napi::Value NapiSetMidiTrackVolume(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setVolume(info[1].As<Napi::Number>().FloatValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetMidiTrackPan(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setPan(info[1].As<Napi::Number>().FloatValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetMidiTrackMute(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setMute(info[1].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetMidiTrackSolo(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setSolo(info[1].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetMidiTrackArmed(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setArmed(info[1].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetMidiTrackInstrument(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    if (!g_engine || info.Length() < 2) {
        deferred.Resolve(env.Undefined());
        return deferred.Promise();
    }
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string pluginId = info[1].As<Napi::String>().Utf8Value();
    auto promise = deferred.Promise();

    // Find the plugin description
    juce::PluginDescription targetDesc;
    bool found = false;
    for (auto& desc : g_engine->getPluginHost().getKnownPlugins().getTypes()) {
        if (desc.createIdentifierString().toStdString() == pluginId) {
            targetDesc = desc;
            found = true;
            break;
        }
    }
    if (!found) {
        auto obj = Napi::Object::New(env);
        obj.Set("error", "Plugin not found");
        deferred.Resolve(obj);
        return promise;
    }

    // Create TSFN to bridge async callback back to Node's event loop
    auto dummyFn = Napi::Function::New(env, [](const Napi::CallbackInfo& i) -> Napi::Value {
        return i.Env().Undefined();
    });
    auto tsfn = Napi::ThreadSafeFunction::New(env, dummyFn, "PluginLoad", 0, 1);
    auto* deferredPtr = new Napi::Promise::Deferred(std::move(deferred));
    double sr = g_engine->getSampleRate();
    int bs = g_engine->getBlockSize();

    // Lambda to resolve promise on Node's thread once we have the instance
    auto resolveOnNodeThread = [tsfn, deferredPtr, trackId](
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const std::string& errorMsg) mutable {

        auto* instancePtr = instance.release();

        tsfn.NonBlockingCall([deferredPtr, trackId, instancePtr, errorMsg](
            Napi::Env env, Napi::Function) {

            std::unique_ptr<juce::AudioPluginInstance> inst(instancePtr);

            if (!inst) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", errorMsg.empty() ? std::string("Failed to load plugin") : errorMsg);
                deferredPtr->Resolve(obj);
            } else if (!g_engine) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", "Engine shut down");
                deferredPtr->Resolve(obj);
            } else {
                auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
                if (t) {
                    t->setInstrument(std::move(inst));
                    deferredPtr->Resolve(env.Undefined());
                } else {
                    auto obj = Napi::Object::New(env);
                    obj.Set("error", "MIDI track not found");
                    deferredPtr->Resolve(obj);
                }
            }
            delete deferredPtr;
        });
        tsfn.Release();
    };

    bool isAudioUnit = targetDesc.pluginFormatName == "AudioUnit";

    if (isAudioUnit) {
        // AudioUnit: createPluginInstanceAsync uses AudioComponentInstantiate
        // which is truly async — runs plugin init on Apple's background thread.
        g_engine->getPluginHost().getFormatManager().createPluginInstanceAsync(
            targetDesc, sr, bs,
            [resolveOnNodeThread](
                std::unique_ptr<juce::AudioPluginInstance> instance,
                const juce::String& error) mutable {
                resolveOnNodeThread(std::move(instance), error.toStdString());
            });
    } else {
        // VST3/VST: createPluginInstanceAsync just posts to JUCE message thread
        // (same as synchronous), which deadlocks in Electron. VST3 does NOT
        // require the message thread, so load on a background std::thread.
        auto descCopy = targetDesc;
        std::thread([resolveOnNodeThread, descCopy, sr, bs]() mutable {
            juce::String error;
            auto instance = g_engine->getPluginHost().getFormatManager()
                .createPluginInstance(descCopy, sr, bs, error);
            resolveOnNodeThread(std::move(instance), error.toStdString());
        }).detach();
    }

    return promise;
}

static Napi::Value NapiSetMidiTrackBuiltInInstrument(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string instrumentType = info[1].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return env.Undefined();

    std::unique_ptr<juce::AudioProcessor> instrument;
    if (instrumentType == "basicSynth") {
        instrument = std::make_unique<BasicSynth>();
    } else if (instrumentType == "samplePlayer") {
        instrument = std::make_unique<SamplePlayer>();
    } else if (instrumentType == "drumKit") {
        instrument = std::make_unique<DrumKit>();
    } else if (instrumentType == "sfzInstrument") {
        instrument = std::make_unique<SFZInstrument>();
    } else {
        auto result = Napi::Object::New(env);
        result.Set("error", "Unknown instrument type: " + instrumentType);
        return result;
    }

    instrument->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
    t->setInstrumentProcessor(std::move(instrument));
    return env.Undefined();
}

// ── MIDI Note Editing ──

static Napi::Value NapiAddMidiNote(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 5) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int noteNumber = info[1].As<Napi::Number>().Int32Value();
    double startBeat = info[2].As<Napi::Number>().DoubleValue();
    double lengthBeats = info[3].As<Napi::Number>().DoubleValue();
    int velocity = info[4].As<Napi::Number>().Int32Value();
    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return env.Undefined();
    int idx = t->addNote(noteNumber, startBeat, lengthBeats, velocity);
    auto result = Napi::Object::New(env);
    result.Set("noteIndex", idx);
    return result;
}

static Napi::Value NapiRemoveMidiNote(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->removeNote(info[1].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiMoveMidiNote(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 4) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->moveNote(
            info[1].As<Napi::Number>().Int32Value(),
            info[2].As<Napi::Number>().Int32Value(),
            info[3].As<Napi::Number>().DoubleValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiResizeMidiNote(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 3) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->resizeNote(
            info[1].As<Napi::Number>().Int32Value(),
            info[2].As<Napi::Number>().DoubleValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetMidiNoteVelocity(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 3) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setNoteVelocity(
            info[1].As<Napi::Number>().Int32Value(),
            info[2].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiGetMidiNotes(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto result = Napi::Object::New(env);
    auto notesArr = Napi::Array::New(env);
    if (g_engine && info.Length() >= 1) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) {
            auto notes = t->getAllNotes();
            for (size_t i = 0; i < notes.size(); i++) {
                auto obj = Napi::Object::New(env);
                obj.Set("noteNumber", notes[i].noteNumber);
                obj.Set("startBeat", notes[i].startBeat);
                obj.Set("lengthBeats", notes[i].lengthBeats);
                obj.Set("velocity", notes[i].velocity);
                obj.Set("channel", notes[i].channel);
                notesArr.Set(static_cast<uint32_t>(i), obj);
            }
        }
    }
    result.Set("notes", notesArr);
    return result;
}

static Napi::Value NapiQuantizeMidiNotes(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->quantizeNotes(info[1].As<Napi::Number>().DoubleValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiAddMidiCC(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 4) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->addControlChange(
            info[1].As<Napi::Number>().Int32Value(),
            info[2].As<Napi::Number>().Int32Value(),
            info[3].As<Napi::Number>().DoubleValue());
    }
    return info.Env().Undefined();
}

// ── Built-in Instrument Parameters ──

static Napi::Value NapiSetBuiltInSynthParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string paramName = info[1].As<Napi::String>().Utf8Value();
    float value = info[2].As<Napi::Number>().FloatValue();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t || !t->getInstrument()) return env.Undefined();
    auto* builtIn = dynamic_cast<BuiltInInstrument*>(t->getInstrument());
    if (builtIn) builtIn->setParam(paramName, value);
    return env.Undefined();
}

static Napi::Value NapiGetBuiltInSynthParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string paramName = info[1].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t || !t->getInstrument()) return Napi::Number::New(env, 0.0);
    auto* builtIn = dynamic_cast<BuiltInInstrument*>(t->getInstrument());
    if (builtIn) return Napi::Number::New(env, builtIn->getParam(paramName));
    return Napi::Number::New(env, 0.0);
}

static Napi::Value NapiLoadSampleToPlayer(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string filePath = info[1].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t || !t->getInstrument()) return env.Undefined();
    auto* player = dynamic_cast<SamplePlayer*>(t->getInstrument());
    if (player) player->loadSample(filePath);
    return env.Undefined();
}

static Napi::Value NapiLoadDrumPadSample(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int padIndex = info[1].As<Napi::Number>().Int32Value();
    std::string filePath = info[2].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t || !t->getInstrument()) return env.Undefined();
    auto* kit = dynamic_cast<DrumKit*>(t->getInstrument());
    if (kit) kit->loadPadSample(padIndex, filePath);
    return env.Undefined();
}

static Napi::Value NapiGetBuiltInInstrumentTypes(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto arr = Napi::Array::New(env, 3);

    auto synth = Napi::Object::New(env);
    synth.Set("type", "basicSynth");
    synth.Set("name", "Basic Synth");
    synth.Set("description", "2-oscillator subtractive synthesizer");
    arr.Set((uint32_t)0, synth);

    auto sampler = Napi::Object::New(env);
    sampler.Set("type", "samplePlayer");
    sampler.Set("name", "Sample Player");
    sampler.Set("description", "Pitched sample playback");
    arr.Set((uint32_t)1, sampler);

    auto drums = Napi::Object::New(env);
    drums.Set("type", "drumKit");
    drums.Set("name", "Drum Kit");
    drums.Set("description", "16-pad drum machine");
    arr.Set((uint32_t)2, drums);

    return arr;
}

// ── SFZ Instruments ──

static Napi::Value NapiLoadSFZPreset(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string presetId = info[1].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return env.Undefined();

    auto sfz = std::make_unique<SFZInstrument>();
    sfz->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
    sfz->loadPreset(presetId);
    t->setInstrumentProcessor(std::move(sfz));
    return env.Undefined();
}

static Napi::Value NapiGetSFZPresets(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    const auto& presets = SFZSampleGenerator::getPresetDefinitions();
    auto arr = Napi::Array::New(env, presets.size());
    for (size_t i = 0; i < presets.size(); ++i) {
        auto obj = Napi::Object::New(env);
        obj.Set("id", presets[i].id);
        obj.Set("name", presets[i].name);
        obj.Set("description", presets[i].description);
        obj.Set("icon", presets[i].icon);
        arr.Set(static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static Napi::Value NapiLoadSFZFile(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string filePath = info[1].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return env.Undefined();

    auto sfz = std::make_unique<SFZInstrument>();
    sfz->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
    sfz->loadSFZFile(filePath);
    t->setInstrumentProcessor(std::move(sfz));
    return env.Undefined();
}

// ── Effects ──

static Napi::Value NapiSetTrackFxParam(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 4) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setFxParam(
            info[1].As<Napi::String>().Utf8Value(),
            info[2].As<Napi::String>().Utf8Value(),
            info[3].As<Napi::Number>().FloatValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetTrackFxEnabled(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 3) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setFxEnabled(
            info[1].As<Napi::String>().Utf8Value(),
            info[2].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

// ── Plugins ──

// Standalone scan for use in worker processes (no g_engine needed).
// Sets up crash guards, inits JUCE, scans, saves to disk, shuts down.
static Napi::Value NapiScanPluginsWorker(const Napi::CallbackInfo& info) {
    auto env = info.Env();

    PluginHost::setupProcessCrashGuards();
    juce::initialiseJuce_GUI();

    PluginHost host;
    host.loadPersistedPlugins();

    int count;
    if (info.Length() > 0 && info[0].IsString()) {
        std::string dir = info[0].As<Napi::String>().Utf8Value();
        if (!dir.empty()) {
            count = host.scanDirectory(dir);
        } else {
            count = host.scanForPlugins();
        }
    } else {
        count = host.scanForPlugins();
    }

    juce::shutdownJuce_GUI();

    auto result = Napi::Object::New(env);
    result.Set("count", Napi::Number::New(env, count));
    return result;
}

// Reload persisted plugin list from disk into the running engine.
// Called in the main process after a scan worker completes.
static Napi::Value NapiReloadPlugins(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (g_engine) {
        g_engine->getPluginHost().loadPersistedPlugins();
    }
    return env.Undefined();
}

static Napi::Value NapiGetPluginList(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto arr = Napi::Array::New(env);
    if (!g_engine) return arr;
    auto plugins = g_engine->getPluginHost().getAvailablePlugins();
    for (size_t i = 0; i < plugins.size(); i++) {
        auto obj = Napi::Object::New(env);
        obj.Set("pluginId", plugins[i].pluginId);
        obj.Set("name", plugins[i].name);
        obj.Set("manufacturer", plugins[i].manufacturer);
        obj.Set("format", plugins[i].format);
        obj.Set("category", plugins[i].category);
        obj.Set("isInstrument", plugins[i].isInstrument);
        arr.Set(i, obj);
    }
    return arr;
}

static Napi::Value NapiInsertPlugin(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    if (!g_engine || info.Length() < 3) {
        deferred.Resolve(env.Undefined());
        return deferred.Promise();
    }
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string pluginId = info[2].As<Napi::String>().Utf8Value();

    auto promise = deferred.Promise();

    // Find the plugin description
    juce::PluginDescription targetDesc;
    bool found = false;
    for (auto& desc : g_engine->getPluginHost().getKnownPlugins().getTypes()) {
        if (desc.createIdentifierString().toStdString() == pluginId) {
            targetDesc = desc;
            found = true;
            break;
        }
    }
    if (!found) {
        auto obj = Napi::Object::New(env);
        obj.Set("error", "Plugin not found");
        deferred.Resolve(obj);
        return promise;
    }

    auto dummyFn = Napi::Function::New(env, [](const Napi::CallbackInfo& i) -> Napi::Value {
        return i.Env().Undefined();
    });
    auto tsfn = Napi::ThreadSafeFunction::New(env, dummyFn, "FxPluginLoad", 0, 1);
    auto* deferredPtr = new Napi::Promise::Deferred(std::move(deferred));
    double sr = g_engine->getSampleRate();
    int bs = g_engine->getBlockSize();

    // Lambda to resolve promise on Node's thread once we have the instance
    auto resolveOnNodeThread = [tsfn, deferredPtr, trackId, slotIndex](
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const std::string& errorMsg) mutable {

        auto* instancePtr = instance.release();

        tsfn.NonBlockingCall([deferredPtr, trackId, slotIndex, instancePtr, errorMsg](
            Napi::Env env, Napi::Function) {

            std::unique_ptr<juce::AudioPluginInstance> inst(instancePtr);

            if (!inst) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", errorMsg.empty() ? std::string("Failed to load plugin") : errorMsg);
                deferredPtr->Resolve(obj);
            } else if (!g_engine) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", "Engine shut down");
                deferredPtr->Resolve(obj);
            } else {
                inst->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
                auto* t = g_engine->getAudioGraph().getTrack(trackId);
                if (t) {
                    t->insertPlugin(slotIndex, std::move(inst));
                    deferredPtr->Resolve(env.Undefined());
                } else {
                    auto obj = Napi::Object::New(env);
                    obj.Set("error", "Track not found");
                    deferredPtr->Resolve(obj);
                }
            }
            delete deferredPtr;
        });
        tsfn.Release();
    };

    bool isAudioUnit = targetDesc.pluginFormatName == "AudioUnit";

    if (isAudioUnit) {
        g_engine->getPluginHost().getFormatManager().createPluginInstanceAsync(
            targetDesc, sr, bs,
            [resolveOnNodeThread](
                std::unique_ptr<juce::AudioPluginInstance> instance,
                const juce::String& error) mutable {
                resolveOnNodeThread(std::move(instance), error.toStdString());
            });
    } else {
        auto descCopy = targetDesc;
        std::thread([resolveOnNodeThread, descCopy, sr, bs]() mutable {
            juce::String error;
            auto instance = g_engine->getPluginHost().getFormatManager()
                .createPluginInstance(descCopy, sr, bs, error);
            resolveOnNodeThread(std::move(instance), error.toStdString());
        }).detach();
    }

    return promise;
}

static Napi::Value NapiRemovePlugin(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->removePlugin(info[1].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiOpenPluginEditor(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        int slot = info[1].As<Napi::Number>().Int32Value();
        if (t) {
            auto* plugin = t->getPlugin(slot);
            if (plugin) g_engine->getPluginHost().openEditorWindow(plugin);
        }
    }
    return info.Env().Undefined();
}

static Napi::Value NapiOpenMidiPluginEditor(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        int trackId = info[0].As<Napi::Number>().Int32Value();
        int slot = info[1].As<Napi::Number>().Int32Value();
        auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
        if (t) {
            auto* plugin = t->getInsert(slot);
            auto* pluginInst = dynamic_cast<juce::AudioPluginInstance*>(plugin);
            if (pluginInst) g_engine->getPluginHost().openEditorWindow(pluginInst);
        }
    }
    return info.Env().Undefined();
}

static Napi::Value NapiOpenMidiInstrumentEditor(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1) {
        int trackId = info[0].As<Napi::Number>().Int32Value();
        auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
        if (t) {
            auto* inst = t->getInstrument();
            auto* pluginInst = dynamic_cast<juce::AudioPluginInstance*>(inst);
            if (pluginInst) {
                g_engine->getPluginHost().openEditorWindow(pluginInst);
            }
        }
    }
    return info.Env().Undefined();
}

static Napi::Value NapiClosePluginEditor(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        int slot = info[1].As<Napi::Number>().Int32Value();
        if (t) {
            auto* plugin = t->getPlugin(slot);
            if (plugin) g_engine->getPluginHost().closeEditorWindow(plugin);
        }
    }
    return info.Env().Undefined();
}

static Napi::Value NapiGetPluginState(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return Napi::Object::New(env);
    auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
    int slot = info[1].As<Napi::Number>().Int32Value();
    auto obj = Napi::Object::New(env);
    if (t) {
        auto* plugin = t->getPlugin(slot);
        if (plugin) {
            obj.Set("name", plugin->getName().toStdString());
            obj.Set("slot", slot);
        }
    }
    return obj;
}

// ── Master ──

static Napi::Value NapiSetMasterVolume(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getAudioGraph().setMasterVolume(info[0].As<Napi::Number>().FloatValue());
    return info.Env().Undefined();
}

// ── Metronome ──

static Napi::Value NapiSetMetronomeVolume(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getMetronome().setVolume(info[0].As<Napi::Number>().FloatValue());
    return info.Env().Undefined();
}

static Napi::Value NapiSetBPM(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getMetronome().setBPM(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

static Napi::Value NapiSetTimeSignature(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2)
        g_engine->getMetronome().setTimeSignature(
            info[0].As<Napi::Number>().Int32Value(),
            info[1].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

// ── Audio Devices ──

static Napi::Value NapiGetAudioDevices(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto devices = Napi::Array::New(env);
    if (!g_engine) return devices;

    auto devList = g_engine->getAudioGraph().getAvailableDevices();
    for (uint32_t i = 0; i < devList.size(); i++) {
        auto& d = devList[i];
        auto obj = Napi::Object::New(env);
        obj.Set("name", d.name);
        obj.Set("typeName", d.typeName);
        obj.Set("numInputChannels", d.numInputChannels);
        obj.Set("numOutputChannels", d.numOutputChannels);

        auto rates = Napi::Array::New(env);
        for (uint32_t r = 0; r < d.sampleRates.size(); r++)
            rates.Set(r, d.sampleRates[r]);
        obj.Set("sampleRates", rates);

        auto bufs = Napi::Array::New(env);
        for (uint32_t b = 0; b < d.bufferSizes.size(); b++)
            bufs.Set(b, d.bufferSizes[b]);
        obj.Set("bufferSizes", bufs);

        auto inNames = Napi::Array::New(env);
        for (uint32_t n = 0; n < d.inputChannelNames.size(); n++)
            inNames.Set(n, d.inputChannelNames[n]);
        obj.Set("inputChannelNames", inNames);

        auto outNames = Napi::Array::New(env);
        for (uint32_t n = 0; n < d.outputChannelNames.size(); n++)
            outNames.Set(n, d.outputChannelNames[n]);
        obj.Set("outputChannelNames", outNames);

        devices.Set(i, obj);
    }
    return devices;
}

static Napi::Value NapiGetAudioDeviceInfo(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto result = Napi::Object::New(env);
    if (!g_engine) return result;

    auto state = g_engine->getAudioGraph().getCurrentDeviceState();
    result.Set("name", state.name);
    result.Set("inputDeviceName", state.inputDeviceName);
    result.Set("typeName", state.typeName);
    result.Set("sampleRate", state.sampleRate);
    result.Set("bufferSize", state.bufferSize);
    result.Set("activeInputs", state.activeInputs);
    result.Set("activeOutputs", state.activeOutputs);
    result.Set("inputLatencyMs", state.inputLatencyMs);
    result.Set("outputLatencyMs", state.outputLatencyMs);

    auto inNames = Napi::Array::New(env);
    for (uint32_t i = 0; i < state.inputChannelNames.size(); i++)
        inNames.Set(i, state.inputChannelNames[i]);
    result.Set("inputChannelNames", inNames);

    auto outNames = Napi::Array::New(env);
    for (uint32_t i = 0; i < state.outputChannelNames.size(); i++)
        outNames.Set(i, state.outputChannelNames[i]);
    result.Set("outputChannelNames", outNames);

    return result;
}

static Napi::Value NapiSetAudioDevice(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) {
        auto r = Napi::Object::New(env);
        r.Set("ok", false);
        return r;
    }
    std::string name = info[0].As<Napi::String>().Utf8Value();
    double sampleRate = info[1].As<Napi::Number>().DoubleValue();
    int bufferSize = info[2].As<Napi::Number>().Int32Value();

    bool ok = g_engine->getAudioGraph().setAudioDevice(name, sampleRate, bufferSize);
    auto result = Napi::Object::New(env);
    result.Set("ok", ok);
    return result;
}

static Napi::Value NapiGetInputDevices(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto devices = Napi::Array::New(env);
    if (!g_engine) return devices;

    auto devList = g_engine->getAudioGraph().getInputDevices();
    for (uint32_t i = 0; i < devList.size(); i++) {
        auto& d = devList[i];
        auto obj = Napi::Object::New(env);
        obj.Set("name", d.name);
        obj.Set("typeName", d.typeName);
        obj.Set("numInputChannels", d.numInputChannels);
        auto inNames = Napi::Array::New(env);
        for (uint32_t n = 0; n < d.inputChannelNames.size(); n++)
            inNames.Set(n, d.inputChannelNames[n]);
        obj.Set("inputChannelNames", inNames);
        devices.Set(i, obj);
    }
    return devices;
}

static Napi::Value NapiSetAudioDeviceSeparate(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 4) {
        auto r = Napi::Object::New(env);
        r.Set("ok", false);
        return r;
    }
    std::string outputName = info[0].As<Napi::String>().Utf8Value();
    std::string inputName = info[1].As<Napi::String>().Utf8Value();
    double sampleRate = info[2].As<Napi::Number>().DoubleValue();
    int bufferSize = info[3].As<Napi::Number>().Int32Value();

    bool ok = g_engine->getAudioGraph().setAudioDeviceSeparate(outputName, inputName, sampleRate, bufferSize);
    auto result = Napi::Object::New(env);
    result.Set("ok", ok);
    return result;
}

// ── MIDI Input ──

static Napi::Value NapiGetMidiDevices(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto arr = Napi::Array::New(env);
    if (!g_engine) return arr;

    auto devices = g_engine->getMidiInput().getAvailableDevices();
    for (uint32_t i = 0; i < devices.size(); i++) {
        auto obj = Napi::Object::New(env);
        obj.Set("name", devices[i].name);
        obj.Set("identifier", devices[i].identifier);
        obj.Set("isOpen", devices[i].isOpen);
        arr.Set(i, obj);
    }
    return arr;
}

static Napi::Value NapiOpenMidiDevice(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 1) return Napi::Boolean::New(env, false);
    std::string id = info[0].As<Napi::String>().Utf8Value();
    bool ok = g_engine->getMidiInput().openDevice(id);
    return Napi::Boolean::New(env, ok);
}

static Napi::Value NapiCloseMidiDevice(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1) {
        std::string id = info[0].As<Napi::String>().Utf8Value();
        g_engine->getMidiInput().closeDevice(id);
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetMidiTarget(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1) {
        int trackId = info[0].As<Napi::Number>().Int32Value();
        g_engine->getMidiInput().setTargetTrack(trackId);
    }
    return info.Env().Undefined();
}

static Napi::Value NapiStartMidiLearn(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1) {
        std::string paramPath = info[0].As<Napi::String>().Utf8Value();
        g_engine->getMidiInput().startMidiLearn(paramPath);
    }
    return info.Env().Undefined();
}

static Napi::Value NapiStopMidiLearn(const Napi::CallbackInfo& info) {
    if (g_engine) g_engine->getMidiInput().stopMidiLearn();
    return info.Env().Undefined();
}

static Napi::Value NapiGetMidiBindings(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto arr = Napi::Array::New(env);
    if (!g_engine) return arr;

    auto bindings = g_engine->getMidiInput().getBindings();
    for (uint32_t i = 0; i < bindings.size(); i++) {
        auto obj = Napi::Object::New(env);
        obj.Set("cc", bindings[i].cc);
        obj.Set("channel", bindings[i].channel);
        obj.Set("paramPath", bindings[i].paramPath);
        arr.Set(i, obj);
    }
    return arr;
}

static Napi::Value NapiRemoveMidiBinding(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        int cc = info[0].As<Napi::Number>().Int32Value();
        int channel = info[1].As<Napi::Number>().Int32Value();
        g_engine->getMidiInput().removeBinding(cc, channel);
    }
    return info.Env().Undefined();
}

// ── File I/O ──

static Napi::Value NapiSaveProject(const Napi::CallbackInfo& info) {
    if (!g_engine || info.Length() < 2) return Napi::Boolean::New(info.Env(), false);
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    std::string name = info[1].As<Napi::String>().Utf8Value();
    bool ok = FileIO::saveProject(dir, name,
        g_engine->getAudioGraph(), g_engine->getTransport(),
        g_engine->getMetronome().getBPM(), 4);
    return Napi::Boolean::New(info.Env(), ok);
}

static Napi::Value NapiLoadProject(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return Napi::Object::New(env);
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    std::string id = info[1].As<Napi::String>().Utf8Value();
    int bpm = 120, tsNum = 4;
    bool ok = FileIO::loadProject(dir, id,
        g_engine->getAudioGraph(), g_engine->getTransport(), bpm, tsNum);
    auto result = Napi::Object::New(env);
    result.Set("ok", ok);
    result.Set("bpm", bpm);
    result.Set("timeSignature", tsNum);

    // Also read project.json and return raw data so renderer can rebuild UI state
    juce::File jsonFile(dir + "/" + id + "/project.json");
    if (jsonFile.existsAsFile()) {
        auto parsed = juce::JSON::parse(jsonFile.loadFileAsString());
        if (auto* obj = parsed.getDynamicObject()) {
            // Return project JSON as string for renderer to parse
            result.Set("projectJson", juce::JSON::toString(parsed).toStdString());
        }
    }
    return result;
}

static Napi::Value NapiExportWAV(const Napi::CallbackInfo& info) {
    if (!g_engine || info.Length() < 1) return info.Env().Undefined();
    std::string path = info[0].As<Napi::String>().Utf8Value();
    double dur = g_engine->getTransport().getTotalDuration();
    if (dur <= 0) dur = 1.0;
    int totalSamples = static_cast<int>(dur * g_engine->getSampleRate());
    juce::AudioBuffer<float> mixdown(2, totalSamples);
    mixdown.clear();
    FileIO::writeWAV(path, mixdown, g_engine->getSampleRate());
    return info.Env().Undefined();
}

static Napi::Value NapiExportAIFF(const Napi::CallbackInfo& info) {
    if (!g_engine || info.Length() < 1) return info.Env().Undefined();
    std::string path = info[0].As<Napi::String>().Utf8Value();
    double dur = g_engine->getTransport().getTotalDuration();
    if (dur <= 0) dur = 1.0;
    int totalSamples = static_cast<int>(dur * g_engine->getSampleRate());
    juce::AudioBuffer<float> mixdown(2, totalSamples);
    mixdown.clear();
    FileIO::writeAIFF(path, mixdown, g_engine->getSampleRate());
    return info.Env().Undefined();
}

static Napi::Value NapiExportStems(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 1) {
        auto r = Napi::Object::New(env);
        r.Set("ok", false);
        r.Set("error", "No engine or missing options");
        return r;
    }

    auto opts = info[0].As<Napi::Object>();
    ExportOptions exportOpts;
    exportOpts.outputDir = opts.Has("outputDir") ? opts.Get("outputDir").As<Napi::String>().Utf8Value() : "";
    exportOpts.exportStems = opts.Has("exportStems") ? opts.Get("exportStems").As<Napi::Boolean>().Value() : true;
    exportOpts.exportMixdown = opts.Has("exportMixdown") ? opts.Get("exportMixdown").As<Napi::Boolean>().Value() : true;
    exportOpts.bitDepth = opts.Has("bitDepth") ? opts.Get("bitDepth").As<Napi::Number>().Int32Value() : 24;
    exportOpts.filePrefix = opts.Has("filePrefix") ? opts.Get("filePrefix").As<Napi::String>().Utf8Value() : "";
    exportOpts.format = opts.Has("format") ? opts.Get("format").As<Napi::String>().Utf8Value() : "wav";
    exportOpts.mp3Bitrate = opts.Has("mp3Bitrate") ? opts.Get("mp3Bitrate").As<Napi::Number>().Int32Value() : 320;

    if (exportOpts.outputDir.empty()) {
        auto r = Napi::Object::New(env);
        r.Set("ok", false);
        r.Set("error", "No output directory specified");
        return r;
    }

    auto result = OfflineRenderer::exportTracks(g_engine->getAudioGraph(), exportOpts);

    auto r = Napi::Object::New(env);
    r.Set("ok", result.complete && !result.error);
    r.Set("error", result.error ? result.errorMessage : "");
    r.Set("totalTracks", result.totalTracks);
    return r;
}

// ── Meter callback ──

static Napi::Value NapiSetMeterCallback(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsFunction()) return env.Undefined();

    g_meterTSFN = Napi::ThreadSafeFunction::New(
        env, info[0].As<Napi::Function>(), "MeterCallback", 0, 1);

    g_meterRunning.store(true);
    g_meterThread = std::make_unique<std::thread>(meterThreadFunc);
    return env.Undefined();
}

// ── MIDI File I/O ──

static Napi::Value NapiImportMidiFile(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string path = info[1].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) {
        auto result = Napi::Object::New(env);
        result.Set("error", "Track not found");
        return result;
    }

    auto sequence = FileIO::readMidiFile(path);
    if (sequence.getNumEvents() == 0) {
        auto result = Napi::Object::New(env);
        result.Set("error", "Failed to read MIDI file or file is empty");
        return result;
    }

    t->setMidiSequence(sequence);
    auto result = Napi::Object::New(env);
    result.Set("ok", true);
    result.Set("eventCount", sequence.getNumEvents());
    return result;
}

static Napi::Value NapiExportMidiFile(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    std::string path = info[1].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) {
        auto result = Napi::Object::New(env);
        result.Set("error", "Track not found");
        return result;
    }

    double bpm = g_engine->getMetronome().getBPM();
    bool ok = FileIO::writeMidiFile(path, t->getMidiSequence(), bpm);
    auto result = Napi::Object::New(env);
    result.Set("ok", ok);
    return result;
}

// ── Beat Quantize ──

static Napi::Value NapiQuantizeAudio(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 1)
        return env.Null();

    int trackId = info[0].As<Napi::Number>().Int32Value();

    auto* track = g_engine->getAudioGraph().getTrack(trackId);
    if (!track || !track->hasBuffer())
        return env.Null();

    // Read optional options object
    double manualSourceBPM = 0.0;
    double minBPM = 40.0;
    double maxBPM = 220.0;
    if (info.Length() > 1 && info[1].IsObject()) {
        auto opts = info[1].As<Napi::Object>();
        if (opts.Has("sourceBpmMode") && opts.Get("sourceBpmMode").As<Napi::String>().Utf8Value() == "manual") {
            if (opts.Has("manualBpm"))
                manualSourceBPM = opts.Get("manualBpm").As<Napi::Number>().DoubleValue();
        }
        if (opts.Has("minBpm")) minBPM = opts.Get("minBpm").As<Napi::Number>().DoubleValue();
        if (opts.Has("maxBpm")) maxBPM = opts.Get("maxBpm").As<Napi::Number>().DoubleValue();
    }

    double targetBPM = g_engine->getMetronome().getBPM();
    double detectedBPM = 0.0;

    bool ok = track->tempoMatchAudio(targetBPM, detectedBPM, manualSourceBPM, minBPM, maxBPM);

    Napi::Object result = Napi::Object::New(env);
    result.Set("ok", ok);
    result.Set("detectedBPM", detectedBPM);
    result.Set("targetBPM", targetBPM);

    if (ok) {
        result.Set("duration", track->getDuration());
        auto waveform = track->getWaveformData(4000);
        auto wfArr = Napi::Float32Array::New(env, waveform.size());
        for (size_t i = 0; i < waveform.size(); i++)
            wfArr[i] = waveform[i];
        result.Set("waveform", wfArr);
    }

    return result;
}

// ── Stem Separation ──

static Napi::Value NapiSeparateStems(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 1)
        return env.Null();

    int trackId = info[0].As<Napi::Number>().Int32Value();
    auto* sourceTrack = g_engine->getAudioGraph().getTrack(trackId);
    if (!sourceTrack || !sourceTrack->hasBuffer())
        return env.Null();

    // Lazy-load model
    static StemSeparator separator;
    if (!separator.isModelLoaded()) {
        // Model path: look next to the .node binary, then in resources/models/
        std::string modelPath;
        std::string projDir = g_engine->getProjectsDir();
        // Try common locations
        std::vector<std::string> searchPaths = {
            projDir + "/../resources/models/htdemucs.onnx",
            projDir + "/../../resources/models/htdemucs.onnx",
            // For dev mode: relative to native/build
            std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) +
                "/../../resources/models/htdemucs.onnx"
        };

        // Also check the app bundle resources path
        #ifdef __APPLE__
        {
            // Check ~/Documents/Sounder/../models/ and standard app resource paths
            auto homeDir = juce::File::getSpecialLocation(
                juce::File::userHomeDirectory);
            searchPaths.push_back(
                homeDir.getChildFile("Downloads/sounder-desktop/resources/models/htdemucs.onnx")
                    .getFullPathName().toStdString());
        }
        #endif

        for (const auto& path : searchPaths) {
            if (juce::File(path).existsAsFile()) {
                modelPath = path;
                break;
            }
        }

        if (modelPath.empty()) {
            Napi::Error::New(env, "Stem separation model not found. "
                "Place htdemucs.onnx in resources/models/").ThrowAsJavaScriptException();
            return env.Null();
        }

        if (!separator.loadModel(modelPath)) {
            Napi::Error::New(env, "Failed to load stem separation model")
                .ThrowAsJavaScriptException();
            return env.Null();
        }
    }

    // Read optional options object
    std::vector<std::string> selectedStems = {"Vocals", "Bass", "Drums", "Other"};
    bool muteOriginal = false;
    if (info.Length() > 1 && info[1].IsObject()) {
        auto opts = info[1].As<Napi::Object>();
        if (opts.Has("stems") && opts.Get("stems").IsArray()) {
            auto stemsArr = opts.Get("stems").As<Napi::Array>();
            selectedStems.clear();
            for (uint32_t i = 0; i < stemsArr.Length(); i++)
                selectedStems.push_back(stemsArr.Get(i).As<Napi::String>().Utf8Value());
        }
        if (opts.Has("muteOriginal"))
            muteOriginal = opts.Get("muteOriginal").As<Napi::Boolean>().Value();
    }

    const auto& srcBuf = sourceTrack->getBuffer();
    double sr = g_engine->getSampleRate();
    std::string baseName = sourceTrack->getName();

    StemResult result;
    try {
        result = separator.separate(srcBuf, sr);
    } catch (const Ort::Exception& e) {
        std::string msg = std::string("ONNX Runtime error: ") + e.what();
        DBG(msg);
        Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        return env.Null();
    } catch (const std::exception& e) {
        std::string msg = std::string("Stem separation error: ") + e.what();
        DBG(msg);
        Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        return env.Null();
    }

    if (muteOriginal)
        sourceTrack->setMute(true);

    auto& graph = g_engine->getAudioGraph();

    const char* stemNames[] = {"Vocals", "Bass", "Drums", "Other"};
    std::unique_ptr<juce::AudioBuffer<float>>* stemPtrs[] = {
        &result.vocals, &result.bass, &result.drums, &result.other
    };

    Napi::Array trackArray = Napi::Array::New(env);
    uint32_t arrayIdx = 0;

    for (int i = 0; i < 4; i++) {
        // Skip stems not selected
        bool selected = false;
        for (auto& s : selectedStems) {
            if (s == stemNames[i]) { selected = true; break; }
        }
        if (!selected) continue;

        std::string trackName = baseName + " - " + stemNames[i];
        int newId = graph.addTrack(trackName);
        auto* newTrack = graph.getTrack(newId);
        if (!newTrack) continue;

        double fileSR = sr;
        newTrack->setBuffer(std::move(*stemPtrs[i]), fileSR);

        double dur = newTrack->getDuration();
        if (dur > g_engine->getTransport().getTotalDuration())
            g_engine->getTransport().setTotalDuration(dur);

        auto waveform = newTrack->getWaveformData(4000);

        Napi::Object obj = Napi::Object::New(env);
        obj.Set("trackId", newId);
        obj.Set("name", trackName);
        obj.Set("duration", dur);

        auto wfArr = Napi::Float32Array::New(env, waveform.size());
        for (size_t j = 0; j < waveform.size(); j++)
            wfArr[j] = waveform[j];
        obj.Set("waveform", wfArr);

        trackArray.Set(arrayIdx++, obj);
    }

    return trackArray;
}

// ── Built-In Effects ──

static Napi::Value NapiGetBuiltInEffectTypes(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto types = BuiltInEffect::getAvailableTypes();
    auto arr = Napi::Array::New(env, types.size());
    for (size_t i = 0; i < types.size(); i++) {
        auto obj = Napi::Object::New(env);
        obj.Set("type", types[i].type);
        obj.Set("name", types[i].name);
        obj.Set("category", types[i].category);
        arr.Set(static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static Napi::Value NapiInsertBuiltInEffect(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string effectType = info[2].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (!t) return env.Undefined();

    auto effect = BuiltInEffect::create(effectType);
    if (!effect) {
        auto result = Napi::Object::New(env);
        result.Set("error", "Unknown effect type: " + effectType);
        return result;
    }
    effect->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
    t->insertBuiltInEffect(slotIndex, std::move(effect));
    return env.Undefined();
}

static Napi::Value NapiInsertMidiBuiltInEffect(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string effectType = info[2].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return env.Undefined();

    auto effect = BuiltInEffect::create(effectType);
    if (!effect) {
        auto result = Napi::Object::New(env);
        result.Set("error", "Unknown effect type: " + effectType);
        return result;
    }
    effect->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
    t->insertBuiltInEffect(slotIndex, std::move(effect));
    return env.Undefined();
}

static Napi::Value NapiSetInsertEffectParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 4) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string paramName = info[2].As<Napi::String>().Utf8Value();
    float value = info[3].As<Napi::Number>().FloatValue();

    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (!t) return env.Undefined();
    auto* proc = t->getInsert(slotIndex);
    if (!proc) return env.Undefined();
    auto* effect = dynamic_cast<BuiltInEffect*>(proc);
    if (effect) effect->setParam(paramName, value);
    return env.Undefined();
}

static Napi::Value NapiGetInsertEffectParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return Napi::Number::New(env, 0.0);
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string paramName = info[2].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (!t) return Napi::Number::New(env, 0.0);
    auto* proc = t->getInsert(slotIndex);
    if (!proc) return Napi::Number::New(env, 0.0);
    auto* effect = dynamic_cast<BuiltInEffect*>(proc);
    if (effect) return Napi::Number::New(env, effect->getParam(paramName));
    return Napi::Number::New(env, 0.0);
}

static Napi::Value NapiSetMidiInsertEffectParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 4) return env.Undefined();
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string paramName = info[2].As<Napi::String>().Utf8Value();
    float value = info[3].As<Napi::Number>().FloatValue();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return env.Undefined();
    auto* proc = t->getInsert(slotIndex);
    if (!proc) return env.Undefined();
    auto* effect = dynamic_cast<BuiltInEffect*>(proc);
    if (effect) effect->setParam(paramName, value);
    return env.Undefined();
}

static Napi::Value NapiGetMidiInsertEffectParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return Napi::Number::New(env, 0.0);
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string paramName = info[2].As<Napi::String>().Utf8Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return Napi::Number::New(env, 0.0);
    auto* proc = t->getInsert(slotIndex);
    if (!proc) return Napi::Number::New(env, 0.0);
    auto* effect = dynamic_cast<BuiltInEffect*>(proc);
    if (effect) return Napi::Number::New(env, effect->getParam(paramName));
    return Napi::Number::New(env, 0.0);
}

// ── Insert Chain Info ──

static Napi::Value NapiGetInsertChainInfo(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto arr = Napi::Array::New(env, 5);
    if (!g_engine || info.Length() < 1) return arr;
    int trackId = info[0].As<Napi::Number>().Int32Value();
    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (!t) return arr;

    for (int i = 0; i < 5; i++) {
        auto obj = Napi::Object::New(env);
        auto* proc = t->getInsert(i);
        if (proc) {
            obj.Set("slot", i);
            obj.Set("name", proc->getName().toStdString());
            obj.Set("isBuiltIn", t->isInsertBuiltIn(i));
            auto* effect = dynamic_cast<BuiltInEffect*>(proc);
            if (effect) obj.Set("effectType", effect->getEffectType());
            else obj.Set("effectType", "");
        } else {
            obj.Set("slot", i);
            obj.Set("name", "");
            obj.Set("isBuiltIn", false);
            obj.Set("effectType", "");
        }
        arr.Set(static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static Napi::Value NapiGetMidiInsertChainInfo(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto arr = Napi::Array::New(env, 5);
    if (!g_engine || info.Length() < 1) return arr;
    int trackId = info[0].As<Napi::Number>().Int32Value();
    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return arr;

    for (int i = 0; i < 5; i++) {
        auto obj = Napi::Object::New(env);
        auto* proc = t->getInsert(i);
        if (proc) {
            obj.Set("slot", i);
            obj.Set("name", proc->getName().toStdString());
            obj.Set("isBuiltIn", t->isInsertBuiltIn(i));
            auto* effect = dynamic_cast<BuiltInEffect*>(proc);
            if (effect) obj.Set("effectType", effect->getEffectType());
            else obj.Set("effectType", "");
        } else {
            obj.Set("slot", i);
            obj.Set("name", "");
            obj.Set("isBuiltIn", false);
            obj.Set("effectType", "");
        }
        arr.Set(static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static Napi::Value NapiRemoveInsert(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->removeInsert(info[1].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiRemoveMidiInsert(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->removeInsert(info[1].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

// ── MIDI Track Plugin Insert ──

static Napi::Value NapiInsertMidiPlugin(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    if (!g_engine || info.Length() < 3) {
        deferred.Resolve(env.Undefined());
        return deferred.Promise();
    }
    int trackId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string pluginId = info[2].As<Napi::String>().Utf8Value();
    auto promise = deferred.Promise();

    juce::PluginDescription targetDesc;
    bool found = false;
    for (auto& desc : g_engine->getPluginHost().getKnownPlugins().getTypes()) {
        if (desc.createIdentifierString().toStdString() == pluginId) {
            targetDesc = desc;
            found = true;
            break;
        }
    }
    if (!found) {
        auto obj = Napi::Object::New(env);
        obj.Set("error", "Plugin not found");
        deferred.Resolve(obj);
        return promise;
    }

    auto dummyFn = Napi::Function::New(env, [](const Napi::CallbackInfo& i) -> Napi::Value {
        return i.Env().Undefined();
    });
    auto tsfn = Napi::ThreadSafeFunction::New(env, dummyFn, "MidiFxPluginLoad", 0, 1);
    auto* deferredPtr = new Napi::Promise::Deferred(std::move(deferred));
    double sr = g_engine->getSampleRate();
    int bs = g_engine->getBlockSize();

    auto resolveOnNodeThread = [tsfn, deferredPtr, trackId, slotIndex](
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const std::string& errorMsg) mutable {
        auto* instancePtr = instance.release();
        tsfn.NonBlockingCall([deferredPtr, trackId, slotIndex, instancePtr, errorMsg](
            Napi::Env env, Napi::Function) {
            std::unique_ptr<juce::AudioPluginInstance> inst(instancePtr);
            if (!inst) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", errorMsg.empty() ? std::string("Failed to load plugin") : errorMsg);
                deferredPtr->Resolve(obj);
            } else if (!g_engine) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", "Engine shut down");
                deferredPtr->Resolve(obj);
            } else {
                inst->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
                auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
                if (t) {
                    t->insertEffect(slotIndex, std::move(inst));
                    deferredPtr->Resolve(env.Undefined());
                } else {
                    auto obj = Napi::Object::New(env);
                    obj.Set("error", "MIDI track not found");
                    deferredPtr->Resolve(obj);
                }
            }
            delete deferredPtr;
        });
        tsfn.Release();
    };

    bool isAudioUnit = targetDesc.pluginFormatName == "AudioUnit";
    if (isAudioUnit) {
        g_engine->getPluginHost().getFormatManager().createPluginInstanceAsync(
            targetDesc, sr, bs,
            [resolveOnNodeThread](std::unique_ptr<juce::AudioPluginInstance> instance,
                const juce::String& error) mutable {
                resolveOnNodeThread(std::move(instance), error.toStdString());
            });
    } else {
        auto descCopy = targetDesc;
        std::thread([resolveOnNodeThread, descCopy, sr, bs]() mutable {
            juce::String error;
            auto instance = g_engine->getPluginHost().getFormatManager()
                .createPluginInstance(descCopy, sr, bs, error);
            resolveOnNodeThread(std::move(instance), error.toStdString());
        }).detach();
    }
    return promise;
}

// ── Track Output Routing ──

static Napi::Value NapiSetTrackOutput(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setOutputBus(info[1].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiGetTrackOutput(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 1) return Napi::Number::New(env, -1);
    auto* t = g_engine->getAudioGraph().getTrack(info[0].As<Napi::Number>().Int32Value());
    if (!t) return Napi::Number::New(env, -1);
    return Napi::Number::New(env, t->getOutputBus());
}

static Napi::Value NapiSetMidiTrackOutput(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
        if (t) t->setOutputBus(info[1].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiGetMidiTrackOutput(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 1) return Napi::Number::New(env, -1);
    auto* t = g_engine->getAudioGraph().getMidiTrack(info[0].As<Napi::Number>().Int32Value());
    if (!t) return Napi::Number::New(env, -1);
    return Napi::Number::New(env, t->getOutputBus());
}

// ── Bus Tracks ──

static Napi::Value NapiAddBusTrack(const Napi::CallbackInfo& info) {
    if (!g_engine) return Napi::Number::New(info.Env(), -1);
    std::string name = (info.Length() >= 1 && info[0].IsString())
        ? info[0].As<Napi::String>().Utf8Value() : "Bus";
    int id = g_engine->getAudioGraph().addBusTrack(name);
    return Napi::Number::New(info.Env(), id);
}

static Napi::Value NapiRemoveBusTrack(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getAudioGraph().removeBusTrack(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

static Napi::Value NapiSetBusTrackVolume(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* b = g_engine->getAudioGraph().getBusTrack(info[0].As<Napi::Number>().Int32Value());
        if (b) b->setVolume(info[1].As<Napi::Number>().FloatValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetBusTrackPan(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* b = g_engine->getAudioGraph().getBusTrack(info[0].As<Napi::Number>().Int32Value());
        if (b) b->setPan(info[1].As<Napi::Number>().FloatValue());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetBusTrackMute(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* b = g_engine->getAudioGraph().getBusTrack(info[0].As<Napi::Number>().Int32Value());
        if (b) b->setMute(info[1].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiSetBusTrackSolo(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* b = g_engine->getAudioGraph().getBusTrack(info[0].As<Napi::Number>().Int32Value());
        if (b) b->setSolo(info[1].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiInsertBusBuiltInEffect(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return env.Undefined();
    int busId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string effectType = info[2].As<Napi::String>().Utf8Value();

    auto* b = g_engine->getAudioGraph().getBusTrack(busId);
    if (!b) return env.Undefined();

    auto effect = BuiltInEffect::create(effectType);
    if (!effect) {
        auto result = Napi::Object::New(env);
        result.Set("error", "Unknown effect type: " + effectType);
        return result;
    }
    effect->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
    b->insertBuiltInEffect(slotIndex, std::move(effect));
    return env.Undefined();
}

static Napi::Value NapiRemoveBusInsert(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 2) {
        auto* b = g_engine->getAudioGraph().getBusTrack(info[0].As<Napi::Number>().Int32Value());
        if (b) b->removeInsert(info[1].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

static Napi::Value NapiGetBusInsertChainInfo(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto arr = Napi::Array::New(env, 5);
    if (!g_engine || info.Length() < 1) return arr;
    int busId = info[0].As<Napi::Number>().Int32Value();
    auto* b = g_engine->getAudioGraph().getBusTrack(busId);
    if (!b) return arr;

    for (int i = 0; i < 5; i++) {
        auto obj = Napi::Object::New(env);
        auto* proc = b->getInsert(i);
        if (proc) {
            obj.Set("slot", i);
            obj.Set("name", proc->getName().toStdString());
            obj.Set("isBuiltIn", b->isInsertBuiltIn(i));
            auto* effect = dynamic_cast<BuiltInEffect*>(proc);
            if (effect) obj.Set("effectType", effect->getEffectType());
            else obj.Set("effectType", "");
        } else {
            obj.Set("slot", i);
            obj.Set("name", "");
            obj.Set("isBuiltIn", false);
            obj.Set("effectType", "");
        }
        arr.Set(static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static Napi::Value NapiSetBusInsertEffectParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 4) return env.Undefined();
    int busId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string paramName = info[2].As<Napi::String>().Utf8Value();
    float value = info[3].As<Napi::Number>().FloatValue();

    auto* b = g_engine->getAudioGraph().getBusTrack(busId);
    if (!b) return env.Undefined();
    auto* proc = b->getInsert(slotIndex);
    if (!proc) return env.Undefined();
    auto* effect = dynamic_cast<BuiltInEffect*>(proc);
    if (effect) effect->setParam(paramName, value);
    return env.Undefined();
}

static Napi::Value NapiGetBusInsertEffectParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return Napi::Number::New(env, 0.0);
    int busId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string paramName = info[2].As<Napi::String>().Utf8Value();

    auto* b = g_engine->getAudioGraph().getBusTrack(busId);
    if (!b) return Napi::Number::New(env, 0.0);
    auto* proc = b->getInsert(slotIndex);
    if (!proc) return Napi::Number::New(env, 0.0);
    auto* effect = dynamic_cast<BuiltInEffect*>(proc);
    if (effect) return Napi::Number::New(env, effect->getParam(paramName));
    return Napi::Number::New(env, 0.0);
}

static Napi::Value NapiInsertBusPlugin(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    if (!g_engine || info.Length() < 3) {
        deferred.Resolve(env.Undefined());
        return deferred.Promise();
    }
    int busId = info[0].As<Napi::Number>().Int32Value();
    int slotIndex = info[1].As<Napi::Number>().Int32Value();
    std::string pluginId = info[2].As<Napi::String>().Utf8Value();
    auto promise = deferred.Promise();

    juce::PluginDescription targetDesc;
    bool found = false;
    for (auto& desc : g_engine->getPluginHost().getKnownPlugins().getTypes()) {
        if (desc.createIdentifierString().toStdString() == pluginId) {
            targetDesc = desc;
            found = true;
            break;
        }
    }
    if (!found) {
        auto obj = Napi::Object::New(env);
        obj.Set("error", "Plugin not found");
        deferred.Resolve(obj);
        return promise;
    }

    auto dummyFn = Napi::Function::New(env, [](const Napi::CallbackInfo& i) -> Napi::Value {
        return i.Env().Undefined();
    });
    auto tsfn = Napi::ThreadSafeFunction::New(env, dummyFn, "BusFxPluginLoad", 0, 1);
    auto* deferredPtr = new Napi::Promise::Deferred(std::move(deferred));
    double sr = g_engine->getSampleRate();
    int bs = g_engine->getBlockSize();

    auto resolveOnNodeThread = [tsfn, deferredPtr, busId, slotIndex](
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const std::string& errorMsg) mutable {
        auto* instancePtr = instance.release();
        tsfn.NonBlockingCall([deferredPtr, busId, slotIndex, instancePtr, errorMsg](
            Napi::Env env, Napi::Function) {
            std::unique_ptr<juce::AudioPluginInstance> inst(instancePtr);
            if (!inst) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", errorMsg.empty() ? std::string("Failed to load plugin") : errorMsg);
                deferredPtr->Resolve(obj);
            } else if (!g_engine) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", "Engine shut down");
                deferredPtr->Resolve(obj);
            } else {
                inst->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
                auto* b = g_engine->getAudioGraph().getBusTrack(busId);
                if (b) {
                    b->insertPlugin(slotIndex, std::move(inst));
                    deferredPtr->Resolve(env.Undefined());
                } else {
                    auto obj = Napi::Object::New(env);
                    obj.Set("error", "Bus track not found");
                    deferredPtr->Resolve(obj);
                }
            }
            delete deferredPtr;
        });
        tsfn.Release();
    };

    bool isAudioUnit = targetDesc.pluginFormatName == "AudioUnit";
    if (isAudioUnit) {
        g_engine->getPluginHost().getFormatManager().createPluginInstanceAsync(
            targetDesc, sr, bs,
            [resolveOnNodeThread](std::unique_ptr<juce::AudioPluginInstance> instance,
                const juce::String& error) mutable {
                resolveOnNodeThread(std::move(instance), error.toStdString());
            });
    } else {
        auto descCopy = targetDesc;
        std::thread([resolveOnNodeThread, descCopy, sr, bs]() mutable {
            juce::String error;
            auto instance = g_engine->getPluginHost().getFormatManager()
                .createPluginInstance(descCopy, sr, bs, error);
            resolveOnNodeThread(std::move(instance), error.toStdString());
        }).detach();
    }
    return promise;
}

static Napi::Value NapiSetBusTrackFxParam(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 4) {
        auto* b = g_engine->getAudioGraph().getBusTrack(info[0].As<Napi::Number>().Int32Value());
        if (b) b->setFxParam(
            info[1].As<Napi::String>().Utf8Value(),
            info[2].As<Napi::String>().Utf8Value(),
            info[3].As<Napi::Number>().FloatValue());
    }
    return info.Env().Undefined();
}

// ── Master Inserts ──

static Napi::Value NapiInsertMasterBuiltInEffect(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return env.Undefined();
    int slotIndex = info[0].As<Napi::Number>().Int32Value();
    std::string effectType = info[1].As<Napi::String>().Utf8Value();
    auto effect = BuiltInEffect::create(effectType);
    if (!effect) {
        auto result = Napi::Object::New(env);
        result.Set("error", "Unknown effect type: " + effectType);
        return result;
    }
    effect->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
    g_engine->getAudioGraph().insertMasterBuiltInEffect(slotIndex, std::move(effect));
    return env.Undefined();
}

static Napi::Value NapiRemoveMasterInsert(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 1)
        g_engine->getAudioGraph().removeMasterInsert(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

static Napi::Value NapiGetMasterInsertChainInfo(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto arr = Napi::Array::New(env, 5);
    if (!g_engine) return arr;
    for (int i = 0; i < 5; i++) {
        auto obj = Napi::Object::New(env);
        auto* proc = g_engine->getAudioGraph().getMasterInsert(i);
        if (proc) {
            obj.Set("slot", i);
            obj.Set("name", proc->getName().toStdString());
            obj.Set("isBuiltIn", g_engine->getAudioGraph().isMasterInsertBuiltIn(i));
            auto* effect = dynamic_cast<BuiltInEffect*>(proc);
            if (effect) obj.Set("effectType", effect->getEffectType());
            else obj.Set("effectType", "");
        } else {
            obj.Set("slot", i);
            obj.Set("name", "");
            obj.Set("isBuiltIn", false);
            obj.Set("effectType", "");
        }
        arr.Set(static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static Napi::Value NapiSetMasterInsertEffectParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return env.Undefined();
    int slotIndex = info[0].As<Napi::Number>().Int32Value();
    std::string paramName = info[1].As<Napi::String>().Utf8Value();
    float value = info[2].As<Napi::Number>().FloatValue();
    auto* proc = g_engine->getAudioGraph().getMasterInsert(slotIndex);
    if (!proc) return env.Undefined();
    auto* effect = dynamic_cast<BuiltInEffect*>(proc);
    if (effect) effect->setParam(paramName, value);
    return env.Undefined();
}

static Napi::Value NapiGetMasterInsertEffectParam(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 2) return Napi::Number::New(env, 0.0);
    int slotIndex = info[0].As<Napi::Number>().Int32Value();
    std::string paramName = info[1].As<Napi::String>().Utf8Value();
    auto* proc = g_engine->getAudioGraph().getMasterInsert(slotIndex);
    if (!proc) return Napi::Number::New(env, 0.0);
    auto* effect = dynamic_cast<BuiltInEffect*>(proc);
    if (effect) return Napi::Number::New(env, effect->getParam(paramName));
    return Napi::Number::New(env, 0.0);
}

static Napi::Value NapiInsertMasterPlugin(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    if (!g_engine || info.Length() < 2) {
        deferred.Resolve(env.Undefined());
        return deferred.Promise();
    }
    int slotIndex = info[0].As<Napi::Number>().Int32Value();
    std::string pluginId = info[1].As<Napi::String>().Utf8Value();
    auto promise = deferred.Promise();

    juce::PluginDescription targetDesc;
    bool found = false;
    for (auto& desc : g_engine->getPluginHost().getKnownPlugins().getTypes()) {
        if (desc.createIdentifierString().toStdString() == pluginId) {
            targetDesc = desc;
            found = true;
            break;
        }
    }
    if (!found) {
        auto obj = Napi::Object::New(env);
        obj.Set("error", "Plugin not found");
        deferred.Resolve(obj);
        return promise;
    }

    auto dummyFn = Napi::Function::New(env, [](const Napi::CallbackInfo& i) -> Napi::Value {
        return i.Env().Undefined();
    });
    auto tsfn = Napi::ThreadSafeFunction::New(env, dummyFn, "MasterFxPluginLoad", 0, 1);
    auto* deferredPtr = new Napi::Promise::Deferred(std::move(deferred));
    double sr = g_engine->getSampleRate();
    int bs = g_engine->getBlockSize();

    auto resolveOnNodeThread = [tsfn, deferredPtr, slotIndex](
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const std::string& errorMsg) mutable {
        auto* instancePtr = instance.release();
        tsfn.NonBlockingCall([deferredPtr, slotIndex, instancePtr, errorMsg](
            Napi::Env env, Napi::Function) {
            std::unique_ptr<juce::AudioPluginInstance> inst(instancePtr);
            if (!inst) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", errorMsg.empty() ? std::string("Failed to load plugin") : errorMsg);
                deferredPtr->Resolve(obj);
            } else if (!g_engine) {
                auto obj = Napi::Object::New(env);
                obj.Set("error", "Engine shut down");
                deferredPtr->Resolve(obj);
            } else {
                inst->prepareToPlay(g_engine->getSampleRate(), g_engine->getBlockSize());
                g_engine->getAudioGraph().insertMasterPlugin(slotIndex, std::move(inst));
                deferredPtr->Resolve(env.Undefined());
            }
            delete deferredPtr;
        });
        tsfn.Release();
    };

    bool isAudioUnit = targetDesc.pluginFormatName == "AudioUnit";
    if (isAudioUnit) {
        g_engine->getPluginHost().getFormatManager().createPluginInstanceAsync(
            targetDesc, sr, bs,
            [resolveOnNodeThread](std::unique_ptr<juce::AudioPluginInstance> instance,
                const juce::String& error) mutable {
                resolveOnNodeThread(std::move(instance), error.toStdString());
            });
    } else {
        auto descCopy = targetDesc;
        std::thread([resolveOnNodeThread, descCopy, sr, bs]() mutable {
            juce::String error;
            auto instance = g_engine->getPluginHost().getFormatManager()
                .createPluginInstance(descCopy, sr, bs, error);
            resolveOnNodeThread(std::move(instance), error.toStdString());
        }).detach();
    }
    return promise;
}

static Napi::Value NapiSetBusTrackFxEnabled(const Napi::CallbackInfo& info) {
    if (g_engine && info.Length() >= 3) {
        auto* b = g_engine->getAudioGraph().getBusTrack(info[0].As<Napi::Number>().Int32Value());
        if (b) b->setFxEnabled(
            info[1].As<Napi::String>().Utf8Value(),
            info[2].As<Napi::Boolean>().Value());
    }
    return info.Env().Undefined();
}

// ── AI MIDI Generation ──

static Napi::Value NapiGenerateMidi(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsObject()) return env.Undefined();

    auto cfg = info[0].As<Napi::Object>();
    MidiGenConfig config;

    if (cfg.Has("numBars"))      config.numBars      = cfg.Get("numBars").As<Napi::Number>().Int32Value();
    if (cfg.Has("temperature"))  config.temperature   = cfg.Get("temperature").As<Napi::Number>().DoubleValue();
    if (cfg.Has("keyRoot"))      config.keyRoot       = cfg.Get("keyRoot").As<Napi::Number>().Int32Value();
    if (cfg.Has("scaleType"))    config.scaleType     = cfg.Get("scaleType").As<Napi::String>().Utf8Value();
    if (cfg.Has("style"))        config.style         = cfg.Get("style").As<Napi::String>().Utf8Value();
    if (cfg.Has("density"))      config.density       = cfg.Get("density").As<Napi::Number>().DoubleValue();
    if (cfg.Has("octaveLow"))    config.octaveLow     = cfg.Get("octaveLow").As<Napi::Number>().Int32Value();
    if (cfg.Has("octaveHigh"))   config.octaveHigh    = cfg.Get("octaveHigh").As<Napi::Number>().Int32Value();
    if (cfg.Has("swingAmount"))  config.swingAmount   = cfg.Get("swingAmount").As<Napi::Number>().DoubleValue();
    if (cfg.Has("beatsPerBar"))  config.beatsPerBar   = cfg.Get("beatsPerBar").As<Napi::Number>().DoubleValue();
    if (cfg.Has("seed"))         config.seed          = cfg.Get("seed").As<Napi::Number>().Int64Value();

    MidiGenerator gen;
    auto notes = gen.generate(config);

    auto notesArr = Napi::Array::New(env, notes.size());
    for (size_t i = 0; i < notes.size(); ++i) {
        auto noteObj = Napi::Object::New(env);
        noteObj.Set("noteNumber",  notes[i].noteNumber);
        noteObj.Set("startBeat",   notes[i].startBeat);
        noteObj.Set("lengthBeats", notes[i].lengthBeats);
        noteObj.Set("velocity",    notes[i].velocity);
        notesArr.Set((uint32_t)i, noteObj);
    }
    auto result = Napi::Object::New(env);
    result.Set("notes", notesArr);
    return result;
}

static Napi::Value NapiInjectMidiNotes(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 3) return env.Undefined();

    int trackId = info[0].As<Napi::Number>().Int32Value();
    auto notesArr = info[1].As<Napi::Array>();
    bool clearFirst = info[2].As<Napi::Boolean>().Value();

    auto* t = g_engine->getAudioGraph().getMidiTrack(trackId);
    if (!t) return env.Undefined();

    if (clearFirst) {
        // Remove all existing notes
        auto existing = t->getAllNotes();
        for (int i = (int)existing.size() - 1; i >= 0; --i) {
            t->removeNote(i);
        }
    }

    uint32_t len = notesArr.Length();
    for (uint32_t i = 0; i < len; ++i) {
        auto noteObj = notesArr.Get(i).As<Napi::Object>();
        int noteNumber = noteObj.Get("noteNumber").As<Napi::Number>().Int32Value();
        double startBeat = noteObj.Get("startBeat").As<Napi::Number>().DoubleValue();
        double lengthBeats = noteObj.Get("lengthBeats").As<Napi::Number>().DoubleValue();
        int velocity = noteObj.Get("velocity").As<Napi::Number>().Int32Value();
        t->addNote(noteNumber, startBeat, lengthBeats, velocity);
    }

    auto result = Napi::Object::New(env);
    result.Set("ok", true);
    result.Set("count", (int)len);
    return result;
}


static Napi::Value NapiInjectAudioBuffer(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (!g_engine || info.Length() < 4)
        return env.Null();

    int trackId = info[0].As<Napi::Number>().Int32Value();
    auto floatArr = info[1].As<Napi::Float32Array>();
    int sampleRate = info[2].As<Napi::Number>().Int32Value();
    int numChannels = info[3].As<Napi::Number>().Int32Value();

    size_t totalFloats = floatArr.ElementLength();
    int numSamples = static_cast<int>(totalFloats / numChannels);

    auto* t = g_engine->getAudioGraph().getTrack(trackId);
    if (!t) {
        auto result = Napi::Object::New(env);
        result.Set("error", std::string("Track not found"));
        return result;
    }

    auto buffer = std::make_unique<juce::AudioBuffer<float>>(numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ch++) {
        float* dst = buffer->getWritePointer(ch);
        const float* src = floatArr.Data() + ch * numSamples;
        std::copy(src, src + numSamples, dst);
    }

    t->setBuffer(std::move(buffer), static_cast<double>(sampleRate));

    double dur = t->getDuration();
    if (dur > g_engine->getTransport().getTotalDuration())
        g_engine->getTransport().setTotalDuration(dur);

    auto result = Napi::Object::New(env);
    result.Set("ok", true);
    result.Set("duration", dur);
    return result;
}

// ── Module init ──

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("requestMicrophoneAccess", Napi::Function::New(env, NapiRequestMicrophoneAccess));
    exports.Set("initialize", Napi::Function::New(env, NapiInitialize));
    exports.Set("shutdown", Napi::Function::New(env, NapiShutdown));

    exports.Set("play", Napi::Function::New(env, NapiPlay));
    exports.Set("stop", Napi::Function::New(env, NapiStop));
    exports.Set("record", Napi::Function::New(env, NapiRecord));
    exports.Set("rewind", Napi::Function::New(env, NapiRewind));
    exports.Set("seekTo", Napi::Function::New(env, NapiSeekTo));
    exports.Set("getTransportState", Napi::Function::New(env, NapiGetTransportState));

    exports.Set("setLoopEnabled", Napi::Function::New(env, NapiSetLoopEnabled));
    exports.Set("setLoopRegion", Napi::Function::New(env, NapiSetLoopRegion));

    exports.Set("addTrack", Napi::Function::New(env, NapiAddTrack));
    exports.Set("removeTrack", Napi::Function::New(env, NapiRemoveTrack));
    exports.Set("setTrackVolume", Napi::Function::New(env, NapiSetTrackVolume));
    exports.Set("setTrackPan", Napi::Function::New(env, NapiSetTrackPan));
    exports.Set("setTrackMute", Napi::Function::New(env, NapiSetTrackMute));
    exports.Set("setTrackSolo", Napi::Function::New(env, NapiSetTrackSolo));
    exports.Set("setTrackArmed", Napi::Function::New(env, NapiSetTrackArmed));
    exports.Set("getTrackWaveform", Napi::Function::New(env, NapiGetTrackWaveform));
    exports.Set("getTrackDuration", Napi::Function::New(env, NapiGetTrackDuration));
    exports.Set("importAudioToTrack", Napi::Function::New(env, NapiImportAudioToTrack));

    exports.Set("setAudioRegion", Napi::Function::New(env, NapiSetAudioRegion));
    exports.Set("splitAudioTrack", Napi::Function::New(env, NapiSplitAudioTrack));
    exports.Set("duplicateAudioTrack", Napi::Function::New(env, NapiDuplicateAudioTrack));
    exports.Set("setAudioFades", Napi::Function::New(env, NapiSetAudioFades));
    exports.Set("getRecordingWaveform", Napi::Function::New(env, NapiGetRecordingWaveform));

    exports.Set("setTrackFxParam", Napi::Function::New(env, NapiSetTrackFxParam));
    exports.Set("setTrackFxEnabled", Napi::Function::New(env, NapiSetTrackFxEnabled));

    // MIDI Tracks
    exports.Set("addMidiTrack", Napi::Function::New(env, NapiAddMidiTrack));
    exports.Set("removeMidiTrack", Napi::Function::New(env, NapiRemoveMidiTrack));
    exports.Set("setMidiTrackVolume", Napi::Function::New(env, NapiSetMidiTrackVolume));
    exports.Set("setMidiTrackPan", Napi::Function::New(env, NapiSetMidiTrackPan));
    exports.Set("setMidiTrackMute", Napi::Function::New(env, NapiSetMidiTrackMute));
    exports.Set("setMidiTrackSolo", Napi::Function::New(env, NapiSetMidiTrackSolo));
    exports.Set("setMidiTrackArmed", Napi::Function::New(env, NapiSetMidiTrackArmed));
    exports.Set("setMidiTrackInstrument", Napi::Function::New(env, NapiSetMidiTrackInstrument));
    exports.Set("setMidiTrackBuiltInInstrument", Napi::Function::New(env, NapiSetMidiTrackBuiltInInstrument));

    // MIDI Note Editing
    exports.Set("addMidiNote", Napi::Function::New(env, NapiAddMidiNote));
    exports.Set("removeMidiNote", Napi::Function::New(env, NapiRemoveMidiNote));
    exports.Set("moveMidiNote", Napi::Function::New(env, NapiMoveMidiNote));
    exports.Set("resizeMidiNote", Napi::Function::New(env, NapiResizeMidiNote));
    exports.Set("setMidiNoteVelocity", Napi::Function::New(env, NapiSetMidiNoteVelocity));
    exports.Set("getMidiNotes", Napi::Function::New(env, NapiGetMidiNotes));
    exports.Set("quantizeMidiNotes", Napi::Function::New(env, NapiQuantizeMidiNotes));
    exports.Set("addMidiCC", Napi::Function::New(env, NapiAddMidiCC));

    // AI MIDI Generation
    exports.Set("generateMidi", Napi::Function::New(env, NapiGenerateMidi));
    exports.Set("injectMidiNotes", Napi::Function::New(env, NapiInjectMidiNotes));

    exports.Set("importMidiFile", Napi::Function::New(env, NapiImportMidiFile));
    exports.Set("exportMidiFile", Napi::Function::New(env, NapiExportMidiFile));

    exports.Set("setBuiltInSynthParam", Napi::Function::New(env, NapiSetBuiltInSynthParam));
    exports.Set("getBuiltInSynthParam", Napi::Function::New(env, NapiGetBuiltInSynthParam));
    exports.Set("loadSampleToPlayer", Napi::Function::New(env, NapiLoadSampleToPlayer));
    exports.Set("loadDrumPadSample", Napi::Function::New(env, NapiLoadDrumPadSample));
    exports.Set("getBuiltInInstrumentTypes", Napi::Function::New(env, NapiGetBuiltInInstrumentTypes));
    exports.Set("loadSFZPreset", Napi::Function::New(env, NapiLoadSFZPreset));
    exports.Set("getSFZPresets", Napi::Function::New(env, NapiGetSFZPresets));
    exports.Set("loadSFZFile", Napi::Function::New(env, NapiLoadSFZFile));

    exports.Set("scanPluginsWorker", Napi::Function::New(env, NapiScanPluginsWorker));
    exports.Set("reloadPlugins", Napi::Function::New(env, NapiReloadPlugins));
    exports.Set("getPluginList", Napi::Function::New(env, NapiGetPluginList));
    exports.Set("insertPlugin", Napi::Function::New(env, NapiInsertPlugin));
    exports.Set("removePlugin", Napi::Function::New(env, NapiRemovePlugin));
    exports.Set("openPluginEditor", Napi::Function::New(env, NapiOpenPluginEditor));
    exports.Set("openMidiPluginEditor", Napi::Function::New(env, NapiOpenMidiPluginEditor));
    exports.Set("openMidiInstrumentEditor", Napi::Function::New(env, NapiOpenMidiInstrumentEditor));
    exports.Set("closePluginEditor", Napi::Function::New(env, NapiClosePluginEditor));
    exports.Set("getPluginState", Napi::Function::New(env, NapiGetPluginState));

    exports.Set("setMasterVolume", Napi::Function::New(env, NapiSetMasterVolume));

    exports.Set("setMetronomeVolume", Napi::Function::New(env, NapiSetMetronomeVolume));
    exports.Set("setBPM", Napi::Function::New(env, NapiSetBPM));
    exports.Set("setTimeSignature", Napi::Function::New(env, NapiSetTimeSignature));

    exports.Set("getAudioDevices", Napi::Function::New(env, NapiGetAudioDevices));
    exports.Set("getInputDevices", Napi::Function::New(env, NapiGetInputDevices));
    exports.Set("getAudioDeviceInfo", Napi::Function::New(env, NapiGetAudioDeviceInfo));
    exports.Set("setAudioDevice", Napi::Function::New(env, NapiSetAudioDevice));
    exports.Set("setAudioDeviceSeparate", Napi::Function::New(env, NapiSetAudioDeviceSeparate));

    exports.Set("getMidiDevices", Napi::Function::New(env, NapiGetMidiDevices));
    exports.Set("openMidiDevice", Napi::Function::New(env, NapiOpenMidiDevice));
    exports.Set("closeMidiDevice", Napi::Function::New(env, NapiCloseMidiDevice));
    exports.Set("setMidiTarget", Napi::Function::New(env, NapiSetMidiTarget));
    exports.Set("startMidiLearn", Napi::Function::New(env, NapiStartMidiLearn));
    exports.Set("stopMidiLearn", Napi::Function::New(env, NapiStopMidiLearn));
    exports.Set("getMidiBindings", Napi::Function::New(env, NapiGetMidiBindings));
    exports.Set("removeMidiBinding", Napi::Function::New(env, NapiRemoveMidiBinding));

    exports.Set("saveProject", Napi::Function::New(env, NapiSaveProject));
    exports.Set("loadProject", Napi::Function::New(env, NapiLoadProject));
    exports.Set("exportWAV", Napi::Function::New(env, NapiExportWAV));
    exports.Set("exportAIFF", Napi::Function::New(env, NapiExportAIFF));
    exports.Set("exportStems", Napi::Function::New(env, NapiExportStems));

    exports.Set("separateStems", Napi::Function::New(env, NapiSeparateStems));
    exports.Set("quantizeAudio", Napi::Function::New(env, NapiQuantizeAudio));

    // AI Audio Injection (generation moved to JS/transformers.js)
    exports.Set("injectAudioBuffer", Napi::Function::New(env, NapiInjectAudioBuffer));

    exports.Set("setMeterCallback", Napi::Function::New(env, NapiSetMeterCallback));

    // Built-in effects
    exports.Set("getBuiltInEffectTypes", Napi::Function::New(env, NapiGetBuiltInEffectTypes));
    exports.Set("insertBuiltInEffect", Napi::Function::New(env, NapiInsertBuiltInEffect));
    exports.Set("insertMidiBuiltInEffect", Napi::Function::New(env, NapiInsertMidiBuiltInEffect));
    exports.Set("setInsertEffectParam", Napi::Function::New(env, NapiSetInsertEffectParam));
    exports.Set("getInsertEffectParam", Napi::Function::New(env, NapiGetInsertEffectParam));
    exports.Set("setMidiInsertEffectParam", Napi::Function::New(env, NapiSetMidiInsertEffectParam));
    exports.Set("getMidiInsertEffectParam", Napi::Function::New(env, NapiGetMidiInsertEffectParam));

    // Insert chain info
    exports.Set("getInsertChainInfo", Napi::Function::New(env, NapiGetInsertChainInfo));
    exports.Set("getMidiInsertChainInfo", Napi::Function::New(env, NapiGetMidiInsertChainInfo));
    exports.Set("removeInsert", Napi::Function::New(env, NapiRemoveInsert));
    exports.Set("removeMidiInsert", Napi::Function::New(env, NapiRemoveMidiInsert));
    exports.Set("insertMidiPlugin", Napi::Function::New(env, NapiInsertMidiPlugin));

    // Track output routing
    exports.Set("setTrackOutput", Napi::Function::New(env, NapiSetTrackOutput));
    exports.Set("getTrackOutput", Napi::Function::New(env, NapiGetTrackOutput));
    exports.Set("setMidiTrackOutput", Napi::Function::New(env, NapiSetMidiTrackOutput));
    exports.Set("getMidiTrackOutput", Napi::Function::New(env, NapiGetMidiTrackOutput));

    // Bus tracks
    exports.Set("addBusTrack", Napi::Function::New(env, NapiAddBusTrack));
    exports.Set("removeBusTrack", Napi::Function::New(env, NapiRemoveBusTrack));
    exports.Set("setBusTrackVolume", Napi::Function::New(env, NapiSetBusTrackVolume));
    exports.Set("setBusTrackPan", Napi::Function::New(env, NapiSetBusTrackPan));
    exports.Set("setBusTrackMute", Napi::Function::New(env, NapiSetBusTrackMute));
    exports.Set("setBusTrackSolo", Napi::Function::New(env, NapiSetBusTrackSolo));
    exports.Set("insertBusBuiltInEffect", Napi::Function::New(env, NapiInsertBusBuiltInEffect));
    exports.Set("removeBusInsert", Napi::Function::New(env, NapiRemoveBusInsert));
    exports.Set("getBusInsertChainInfo", Napi::Function::New(env, NapiGetBusInsertChainInfo));
    exports.Set("setBusInsertEffectParam", Napi::Function::New(env, NapiSetBusInsertEffectParam));
    exports.Set("getBusInsertEffectParam", Napi::Function::New(env, NapiGetBusInsertEffectParam));
    exports.Set("insertBusPlugin", Napi::Function::New(env, NapiInsertBusPlugin));
    exports.Set("setBusTrackFxParam", Napi::Function::New(env, NapiSetBusTrackFxParam));
    exports.Set("setBusTrackFxEnabled", Napi::Function::New(env, NapiSetBusTrackFxEnabled));

    // Master inserts
    exports.Set("insertMasterBuiltInEffect", Napi::Function::New(env, NapiInsertMasterBuiltInEffect));
    exports.Set("removeMasterInsert", Napi::Function::New(env, NapiRemoveMasterInsert));
    exports.Set("getMasterInsertChainInfo", Napi::Function::New(env, NapiGetMasterInsertChainInfo));
    exports.Set("setMasterInsertEffectParam", Napi::Function::New(env, NapiSetMasterInsertEffectParam));
    exports.Set("getMasterInsertEffectParam", Napi::Function::New(env, NapiGetMasterInsertEffectParam));
    exports.Set("insertMasterPlugin", Napi::Function::New(env, NapiInsertMasterPlugin));

    return exports;
}

NODE_API_MODULE(sounder_engine, Init)
