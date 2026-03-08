#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

struct PluginInfo {
    std::string pluginId;
    std::string name;
    std::string manufacturer;
    std::string format;      // "VST", "VST3", or "AudioUnit"
    std::string category;
    bool isInstrument;
};

class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    // Persistence
    void loadPersistedPlugins();
    void savePlugins();

    // Scanning (synchronous, saves incrementally to disk)
    int scanForPlugins();
    int scanDirectory(const std::string& dirPath);
    std::vector<PluginInfo> getAvailablePlugins() const;

    // Loading
    std::unique_ptr<juce::AudioPluginInstance> loadPlugin(
        const std::string& pluginId,
        double sampleRate,
        int blockSize
    );

    // Direct access for async loading
    juce::AudioPluginFormatManager& getFormatManager() { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() { return knownPlugins; }

    // Editor windows
    void openEditorWindow(juce::AudioPluginInstance* plugin);
    void closeEditorWindow(juce::AudioPluginInstance* plugin);

    // Crash isolation for worker processes
    static void setupProcessCrashGuards();

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    std::map<juce::AudioPluginInstance*, std::unique_ptr<juce::DocumentWindow>> editorWindows;

    juce::File getDataDir() const;
    juce::File getPluginListFile() const;
    juce::File getDeadManFile() const;
};
