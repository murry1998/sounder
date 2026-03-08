#include "PluginHost.h"
#include <unistd.h>
#include <signal.h>

// Subclass that handles the close button
class PluginEditorWindow : public juce::DocumentWindow {
public:
    PluginEditorWindow(const juce::String& name, juce::Colour bg, int buttons)
        : juce::DocumentWindow(name, bg, buttons) {}

    void closeButtonPressed() override {
        setVisible(false);
    }
};

#if JUCE_MAC
#include <mach/mach.h>
#endif

PluginHost::PluginHost() {
#if JUCE_PLUGINHOST_VST
    formatManager.addFormat(std::make_unique<juce::VSTPluginFormat>());
#endif
#if JUCE_PLUGINHOST_VST3
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    formatManager.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
#endif
}

PluginHost::~PluginHost() {
    editorWindows.clear();
}

// ── Crash guards for worker process ──

static void workerCrashHandler(int) { _exit(1); }

void PluginHost::setupProcessCrashGuards() {
#if JUCE_MAC
    // Detach Mach exception ports so macOS crash reporter ignores this process
    task_set_exception_ports(mach_task_self(),
        EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION |
        EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT | EXC_MASK_SOFTWARE,
        MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif

    signal(SIGTRAP, workerCrashHandler);
    signal(SIGABRT, workerCrashHandler);
    signal(SIGSEGV, workerCrashHandler);
    signal(SIGBUS,  workerCrashHandler);
    signal(SIGILL,  workerCrashHandler);
}

// ── Data directory helpers ──

juce::File PluginHost::getDataDir() const {
    auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    auto dir = home.getChildFile(".sounder");
    dir.createDirectory();
    return dir;
}

juce::File PluginHost::getPluginListFile() const {
    return getDataDir().getChildFile("known-plugins.xml");
}

juce::File PluginHost::getDeadManFile() const {
    return getDataDir().getChildFile("scan-dead-man.txt");
}

// ── Persistence ──

void PluginHost::loadPersistedPlugins() {
    auto file = getPluginListFile();
    if (!file.existsAsFile()) return;

    auto xml = juce::parseXML(file);
    if (xml) {
        knownPlugins.recreateFromXml(*xml);
    }
}

void PluginHost::savePlugins() {
    auto xml = knownPlugins.createXml();
    if (xml) {
        xml->writeTo(getPluginListFile());
    }
}

// ── Synchronous scanning (runs in worker process) ──

int PluginHost::scanForPlugins() {
    auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    auto deadMan = getDeadManFile();

    std::vector<std::pair<juce::String, juce::FileSearchPath>> formatPaths;

#if JUCE_MAC
    {
        juce::FileSearchPath p;
        p.add(juce::File("/Library/Audio/Plug-Ins/VST3"));
        p.add(home.getChildFile("Library/Audio/Plug-Ins/VST3"));
        formatPaths.push_back({"VST3", p});
    }
    {
        juce::FileSearchPath p;
        p.add(juce::File("/Library/Audio/Plug-Ins/Components"));
        p.add(home.getChildFile("Library/Audio/Plug-Ins/Components"));
        formatPaths.push_back({"AudioUnit", p});
    }
    {
        juce::FileSearchPath p;
        p.add(juce::File("/Library/Audio/Plug-Ins/VST"));
        p.add(home.getChildFile("Library/Audio/Plug-Ins/VST"));
        formatPaths.push_back({"VST", p});
    }
#endif

    for (auto& [formatName, paths] : formatPaths) {
        if (paths.getNumPaths() == 0) continue;

        juce::AudioPluginFormat* format = nullptr;
        for (auto* f : formatManager.getFormats()) {
            if (f->getName() == formatName) { format = f; break; }
        }
        if (!format) continue;

        juce::PluginDirectoryScanner scanner(knownPlugins, *format, paths, true, deadMan);
        juce::String name;
        while (scanner.scanNextFile(true, name)) {
            savePlugins(); // incremental save after each plugin found
        }
    }

    savePlugins();
    return knownPlugins.getNumTypes();
}

int PluginHost::scanDirectory(const std::string& dirPath) {
    juce::File dir(dirPath);
    if (!dir.isDirectory()) return 0;

    auto deadMan = getDeadManFile();
    int before = knownPlugins.getNumTypes();

    juce::FileSearchPath paths;
    paths.add(dir);

    for (auto* format : formatManager.getFormats()) {
        juce::PluginDirectoryScanner scanner(knownPlugins, *format, paths, true, deadMan);
        juce::String name;
        while (scanner.scanNextFile(true, name)) {
            savePlugins(); // incremental save
        }
    }

    savePlugins();
    return knownPlugins.getNumTypes() - before;
}

std::vector<PluginInfo> PluginHost::getAvailablePlugins() const {
    std::vector<PluginInfo> result;
    for (auto& desc : knownPlugins.getTypes()) {
        PluginInfo info;
        info.pluginId = desc.createIdentifierString().toStdString();
        info.name = desc.name.toStdString();
        info.manufacturer = desc.manufacturerName.toStdString();
        info.format = desc.pluginFormatName.toStdString();
        info.category = desc.category.toStdString();
        info.isInstrument = desc.isInstrument;
        result.push_back(info);
    }
    return result;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::loadPlugin(
    const std::string& pluginId, double sampleRate, int blockSize)
{
    for (auto& desc : knownPlugins.getTypes()) {
        if (desc.createIdentifierString().toStdString() == pluginId) {
            juce::String errorMessage;
            auto instance = formatManager.createPluginInstance(
                desc, sampleRate, blockSize, errorMessage);
            if (instance) {
                instance->prepareToPlay(sampleRate, blockSize);
            }
            return instance;
        }
    }
    return nullptr;
}

void PluginHost::openEditorWindow(juce::AudioPluginInstance* plugin) {
    if (!plugin) return;
    closeEditorWindow(plugin);

    auto editor = plugin->createEditor();
    if (!editor) return;

    auto* window = new PluginEditorWindow(
        plugin->getName(),
        juce::Colours::darkgrey,
        juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton);
    window->setContentOwned(editor, true);
    window->setUsingNativeTitleBar(true);
    window->setResizable(false, false);
    window->centreWithSize(editor->getWidth(), editor->getHeight());
    window->setVisible(true);

    editorWindows[plugin] = std::unique_ptr<juce::DocumentWindow>(window);
}

void PluginHost::closeEditorWindow(juce::AudioPluginInstance* plugin) {
    editorWindows.erase(plugin);
}
