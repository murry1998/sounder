// VST2 Plugin Interface Definitions
// Public ABI specification for VST2 plugin hosting

#pragma once

#include <cstdint>

#if _WIN32
 #define VSTCALLBACK __cdecl
#else
 #define VSTCALLBACK
#endif

typedef intptr_t VstIntPtr;
typedef int32_t VstInt32;

struct AEffect;

typedef VstIntPtr (VSTCALLBACK *audioMasterCallback)(AEffect* effect, VstInt32 opcode,
    VstInt32 index, VstIntPtr value, void* ptr, float opt);

typedef VstIntPtr (VSTCALLBACK *AEffectDispatcherProc)(AEffect* effect, VstInt32 opcode,
    VstInt32 index, VstIntPtr value, void* ptr, float opt);

typedef void (VSTCALLBACK *AEffectProcessProc)(AEffect* effect, float** inputs,
    float** outputs, VstInt32 sampleFrames);

typedef void (VSTCALLBACK *AEffectProcessDoubleProc)(AEffect* effect, double** inputs,
    double** outputs, VstInt32 sampleFrames);

typedef void (VSTCALLBACK *AEffectSetParameterProc)(AEffect* effect, VstInt32 index, float parameter);
typedef float (VSTCALLBACK *AEffectGetParameterProc)(AEffect* effect, VstInt32 index);

enum VstAEffectFlags {
    effFlagsHasEditor     = 1 << 0,
    effFlagsCanReplacing  = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth       = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12
};

enum {
    kEffectMagic = 0x56737450  // 'VstP'
};

enum {
    kVstMaxNameLen        = 64,
    kVstMaxLabelLen       = 64,
    kVstMaxShortLabelLen  = 8,
    kVstMaxCategLabelLen  = 24,
    kVstMaxFileNameLen    = 100,
    kVstMaxProductStrLen  = 64,
    kVstMaxVendorStrLen   = 64,
    kVstMaxParamStrLen    = 8,
    kVstMaxProgNameLen    = 24,
    kVstMaxEffectNameLen  = 32
};

struct AEffect {
    VstInt32 magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    VstInt32 numPrograms;
    VstInt32 numParams;
    VstInt32 numInputs;
    VstInt32 numOutputs;
    VstInt32 flags;
    VstIntPtr resvd1;
    VstIntPtr resvd2;
    VstInt32 initialDelay;
    VstInt32 realQualities;
    VstInt32 offQualities;
    float ioRatio;
    void* object;
    void* user;
    VstInt32 uniqueID;
    VstInt32 version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56];
};
