cmake_minimum_required(VERSION 3.22)
project(sounder_engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# cmake-js sets these variables
include_directories(${CMAKE_JS_INC})

# JUCE configuration (before add_subdirectory)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(JUCE)

# ONNX Runtime (macOS arm64) for stem separation
include(FetchContent)
FetchContent_Declare(
  onnxruntime
  URL https://github.com/microsoft/onnxruntime/releases/download/v1.24.2/onnxruntime-osx-arm64-1.24.2.tgz
)
FetchContent_MakeAvailable(onnxruntime)

# N-API addon shared library
add_library(sounder_engine SHARED
    src/SounderEngine.cpp
    src/StemSeparator.cpp
    src/MidiGenerator.cpp
    src/BeatQuantizer.cpp
    src/AudioGraph.cpp
    src/AudioTrack.cpp
    src/MidiTrack.cpp
    src/BasicSynth.cpp
    src/SamplePlayer.cpp
    src/DrumKit.cpp
    src/MidiInputManager.cpp
    src/OfflineRenderer.cpp
    src/PluginHost.cpp
    src/TransportEngine.cpp
    src/Metronome.cpp
    src/BuiltInEffect.cpp
    src/BusTrack.cpp
    src/SFZInstrument.cpp
    src/FileIO.cpp
    src/MicPermission.mm
    ${CMAKE_JS_SRC}
)

set_target_properties(sounder_engine PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
)

target_include_directories(sounder_engine PRIVATE
    include
    ${onnxruntime_SOURCE_DIR}/include
)

if(APPLE)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation)
endif()

target_link_libraries(sounder_engine PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_dsp
    juce::juce_events
    juce::juce_gui_basics
    ${CMAKE_JS_LIB}
    $<$<PLATFORM_ID:Darwin>:${AVFOUNDATION_FRAMEWORK}>
    ${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.dylib
)

# N-API headers (node_modules is in the parent sounder-desktop/ dir)
execute_process(COMMAND node -p "require('node-addon-api').include.replace(/\\\"/g, '')"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
    OUTPUT_VARIABLE NODE_ADDON_API_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
target_include_directories(sounder_engine PRIVATE ${NODE_ADDON_API_DIR})
target_compile_definitions(sounder_engine PRIVATE NAPI_VERSION=8)

# JUCE compile definitions
target_compile_definitions(sounder_engine PRIVATE
    JUCE_PLUGINHOST_VST=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_APPLICATION_NAME_STRING="Sounder"
    JUCE_APPLICATION_VERSION_STRING="1.0.0"
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_USE_LAME_AUDIO_FORMAT=1
)
