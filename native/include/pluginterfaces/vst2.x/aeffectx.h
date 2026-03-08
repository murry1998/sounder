// VST2 Extended Plugin Interface Definitions
// Public ABI specification for VST2 plugin hosting

#pragma once

#include "aeffect.h"

// Dispatcher opcodes (effect -> host)
enum {
    effOpen = 0,
    effClose,
    effSetProgram,
    effGetProgram,
    effSetProgramName,
    effGetProgramName,
    effGetParamLabel,
    effGetParamDisplay,
    effGetParamName,
    effGetVu,
    effSetSampleRate = 10,
    effSetBlockSize,
    effMainsChanged,
    effEditGetRect,
    effEditOpen,
    effEditClose,
    effEditDraw,
    effEditMouse,
    effEditKey,
    effEditIdle,
    effEditTop,
    effEditSleep,
    effIdentify,
    effGetChunk,
    effSetChunk,
    effProcessEvents = 25,
    effCanBeAutomated,
    effString2Parameter,
    effGetNumProgramCategories,
    effGetProgramNameIndexed,
    effCopyProgram,
    effConnectInput,
    effConnectOutput,
    effGetInputProperties,
    effGetOutputProperties,
    effGetPlugCategory,
    effGetCurrentPosition,
    effGetDestinationBuffer,
    effOfflineNotify,
    effOfflinePrepare,
    effOfflineRun,
    effProcessVarIo,
    effSetSpeakerArrangement,
    effSetBlockSizeAndSampleRate,
    effSetBypass,
    effGetEffectName,
    effGetErrorText,
    effGetVendorString,
    effGetProductString,
    effGetVendorVersion,
    effVendorSpecific,
    effCanDo,
    effGetTailSize,
    effIdle,
    effGetIcon,
    effSetViewPosition,
    effGetParameterProperties,
    effKeysRequired,
    effGetVstVersion,
    effEditKeyDown = 59,
    effEditKeyUp,
    effSetEditKnobMode,
    effGetMidiProgramName,
    effGetCurrentMidiProgram,
    effGetMidiProgramCategory,
    effHasMidiProgramsChanged,
    effGetMidiKeyName,
    effBeginSetProgram,
    effEndSetProgram,
    effGetSpeakerArrangement = 69,
    effShellGetNextPlugin,
    effStartProcess,
    effStopProcess,
    effSetTotalSampleToProcess,
    effSetPanLaw,
    effBeginLoadBank,
    effBeginLoadProgram,
    effSetProcessPrecision = 77,
    effGetNumMidiInputChannels,
    effGetNumMidiOutputChannels
};

// Host callback opcodes (host -> effect)
enum {
    audioMasterAutomate = 0,
    audioMasterVersion,
    audioMasterCurrentId,
    audioMasterIdle,
    audioMasterPinConnected,
    audioMasterWantMidi = 6,
    audioMasterGetTime,
    audioMasterProcessEvents,
    audioMasterSetTime,
    audioMasterTempoAt,
    audioMasterGetNumAutomatableParameters,
    audioMasterGetParameterQuantization,
    audioMasterIOChanged,
    audioMasterNeedIdle,
    audioMasterSizeWindow,
    audioMasterGetSampleRate,
    audioMasterGetBlockSize,
    audioMasterGetInputLatency,
    audioMasterGetOutputLatency,
    audioMasterGetPreviousPlug,
    audioMasterGetNextPlug,
    audioMasterWillReplaceOrAccumulate,
    audioMasterGetCurrentProcessLevel,
    audioMasterGetAutomationState,
    audioMasterOfflineStart,
    audioMasterOfflineRead,
    audioMasterOfflineWrite,
    audioMasterOfflineGetCurrentPass,
    audioMasterOfflineGetCurrentMetaPass,
    audioMasterSetOutputSampleRate,
    audioMasterGetOutputSpeakerArrangement,
    audioMasterGetVendorString,
    audioMasterGetProductString,
    audioMasterGetVendorVersion,
    audioMasterVendorSpecific,
    audioMasterSetIcon,
    audioMasterCanDo,
    audioMasterGetLanguage,
    audioMasterOpenWindow,
    audioMasterCloseWindow,
    audioMasterGetDirectory,
    audioMasterUpdateDisplay,
    audioMasterBeginEdit,
    audioMasterEndEdit,
    audioMasterOpenFileSelector,
    audioMasterCloseFileSelector
};

// Plugin categories
enum VstPlugCategory {
    kPlugCategUnknown = 0,
    kPlugCategEffect,
    kPlugCategSynth,
    kPlugCategAnalysis,
    kPlugCategMastering,
    kPlugCategSpacializer,
    kPlugCategRoomFx,
    kPlugSurroundFx,
    kPlugCategRestoration,
    kPlugCategOfflineProcess,
    kPlugCategShell,
    kPlugCategGenerator,
    kPlugCategMaxCount
};

// SMPTE frame rates
enum VstSmpteFrameRate {
    kVstSmpte24fps = 0,
    kVstSmpte25fps = 1,
    kVstSmpte2997fps = 2,
    kVstSmpte30fps = 3,
    kVstSmpte2997dfps = 4,
    kVstSmpte30dfps = 5,
    kVstSmpte2398fps = 6,
    kVstSmpte239fps = 6,
    kVstSmpte249fps = 7,
    kVstSmpte599fps = 8,
    kVstSmpte60fps = 9
};

// Time info
struct VstTimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    VstInt32 timeSigNumerator;
    VstInt32 timeSigDenominator;
    VstInt32 smpteOffset;
    VstInt32 smpteFrameRate;
    VstInt32 samplesToNextClock;
    VstInt32 flags;
};

enum VstTimeInfoFlags {
    kVstTransportChanged     = 1,
    kVstTransportPlaying     = 1 << 1,
    kVstTransportCycleActive = 1 << 2,
    kVstTransportRecording   = 1 << 3,
    kVstAutomationWriting    = 1 << 6,
    kVstAutomationReading    = 1 << 7,
    kVstNanosValid           = 1 << 8,
    kVstPpqPosValid          = 1 << 9,
    kVstTempoValid           = 1 << 10,
    kVstBarsValid            = 1 << 11,
    kVstCyclePosValid        = 1 << 12,
    kVstTimeSigValid         = 1 << 13,
    kVstSmpteValid           = 1 << 14,
    kVstClockValid           = 1 << 15
};

// MIDI events
enum VstEventTypes {
    kVstMidiType = 1,
    kVstSysExType = 6
};

struct VstEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    char data[16];
};

struct VstEvents {
    VstInt32 numEvents;
    VstIntPtr reserved;
    VstEvent* events[2];
};

struct VstMidiEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    VstInt32 noteLength;
    VstInt32 noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

struct VstMidiSysexEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    VstInt32 dumpBytes;
    VstIntPtr resvd1;
    char* sysexDump;
    VstIntPtr resvd2;
};

// Speaker arrangement
struct VstSpeakerProperties {
    float azimuth;
    float elevation;
    float radius;
    float reserved;
    char name[kVstMaxNameLen];
    VstInt32 type;
    char future[28];
};

struct VstSpeakerArrangement {
    VstInt32 type;
    VstInt32 numChannels;
    VstSpeakerProperties speakers[8];
};

// Pin properties
struct VstPinProperties {
    char label[kVstMaxLabelLen];
    VstInt32 flags;
    VstInt32 arrangementType;
    char shortLabel[kVstMaxShortLabelLen];
    char future[48];
};

enum VstPinPropertiesFlags {
    kVstPinIsActive = 1 << 0,
    kVstPinIsStereo = 1 << 1,
    kVstPinUseSpeaker = 1 << 2
};

// Parameter properties
struct VstParameterProperties {
    float stepFloat;
    float smallStepFloat;
    float largeStepFloat;
    char label[kVstMaxLabelLen];
    VstInt32 flags;
    VstInt32 minInteger;
    VstInt32 maxInteger;
    VstInt32 stepInteger;
    VstInt32 largeStepInteger;
    char shortLabel[kVstMaxShortLabelLen];
    int16_t displayIndex;
    int16_t category;
    int16_t numParametersInCategory;
    int16_t reserved;
    char categoryLabel[kVstMaxCategLabelLen];
    char future[16];
};

// Editor rect
struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
};

// Process precision
enum VstProcessPrecision {
    kVstProcessPrecision32 = 0,
    kVstProcessPrecision64
};

// MIDI program
struct MidiProgramName {
    VstInt32 thisProgramIndex;
    char name[kVstMaxNameLen];
    int8_t midiProgram;
    int8_t midiBankMsb;
    int8_t midiBankLsb;
    int8_t reserved;
    VstInt32 parentCategoryIndex;
    VstInt32 flags;
};

struct MidiProgramCategory {
    VstInt32 thisCategoryIndex;
    char name[kVstMaxNameLen];
    VstInt32 parentCategoryIndex;
    VstInt32 flags;
};

struct MidiKeyName {
    VstInt32 thisProgramIndex;
    VstInt32 thisKeyNumber;
    char keyName[kVstMaxNameLen];
    VstInt32 reserved;
    VstInt32 flags;
};

// Speaker types
enum VstSpeakerType {
    kSpeakerUndefined = 0x7fffffff,
    kSpeakerM = 0,
    kSpeakerL,
    kSpeakerR,
    kSpeakerC,
    kSpeakerLfe,
    kSpeakerLs,
    kSpeakerRs,
    kSpeakerLc,
    kSpeakerRc,
    kSpeakerS,
    kSpeakerCs = kSpeakerS,
    kSpeakerSl,
    kSpeakerSr,
    kSpeakerTm,
    kSpeakerTfl,
    kSpeakerTfc,
    kSpeakerTfr,
    kSpeakerTrl,
    kSpeakerTrc,
    kSpeakerTrr,
    kSpeakerLfe2
};

enum VstSpeakerArrangementType {
    kSpeakerArrUserDefined = -2,
    kSpeakerArrEmpty = -1,
    kSpeakerArrMono = 0,
    kSpeakerArrStereo,
    kSpeakerArrStereoSurround,
    kSpeakerArrStereoCenter,
    kSpeakerArrStereoSide,
    kSpeakerArrStereoCLfe,
    kSpeakerArr30Cine,
    kSpeakerArr30Music,
    kSpeakerArr31Cine,
    kSpeakerArr31Music,
    kSpeakerArr40Cine,
    kSpeakerArr40Music,
    kSpeakerArr41Cine,
    kSpeakerArr41Music,
    kSpeakerArr50,
    kSpeakerArr51,
    kSpeakerArr60Cine,
    kSpeakerArr60Music,
    kSpeakerArr61Cine,
    kSpeakerArr61Music,
    kSpeakerArr70Cine,
    kSpeakerArr70Music,
    kSpeakerArr71Cine,
    kSpeakerArr71Music,
    kSpeakerArr80Cine,
    kSpeakerArr80Music,
    kSpeakerArr81Cine,
    kSpeakerArr81Music,
    kSpeakerArr102,
    kNumSpeakerArr
};

// Pan law
enum VstPanLawType {
    kLinearPanLaw = 0,
    kEqualPowerPanLaw
};

// Process levels
enum VstProcessLevels {
    kVstProcessLevelUnknown = 0,
    kVstProcessLevelUser,
    kVstProcessLevelRealtime,
    kVstProcessLevelPrefetch,
    kVstProcessLevelOffline
};

// Automation states
enum VstAutomationStates {
    kVstAutomationUnsupported = 0,
    kVstAutomationOff,
    kVstAutomationRead,
    kVstAutomationWrite,
    kVstAutomationReadWrite
};

// File selector
struct VstFileType {
    char name[128];
    char macType[8];
    char dosType[8];
    char unixType[8];
    char mimeType1[128];
    char mimeType2[128];
};

struct VstFileSelect {
    VstInt32 command;
    VstInt32 type;
    VstInt32 macCreator;
    VstInt32 nbFileTypes;
    VstFileType* fileTypes;
    char title[1024];
    char* initialPath;
    char* returnPath;
    VstInt32 sizeReturnPath;
    char** returnMultiplePaths;
    VstInt32 nbReturnPath;
    VstIntPtr reserved;
    char future[116];
};

enum VstFileSelectCommand {
    kVstFileLoad = 0,
    kVstFileSave,
    kVstMultipleFilesLoad,
    kVstDirectorySelect
};

enum VstFileSelectType {
    kVstFileType = 0
};
