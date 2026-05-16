/*
  ==============================================================================
    CognGate.cpp — Cognitive Brain Training Audio Gate
    Single-file VST2 plugin for Equalizer APO

    Compiles to a single .dll with NO external dependencies.
    Uses the public VST2 C ABI (Application Binary Interface).

    Build command (MSVC):
      cl /LD /O2 /EHsc /DUNICODE CognGate.cpp /Fe:CognGate.dll ole32.lib

    Build command (MinGW):
      g++ -shared -O2 -o CognGate.dll CognGate.cpp -static -lole32
  ==============================================================================
*/

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <random>

// ============================================================================
//  VST2 ABI Definitions (Public C Interface Standard)
//  These define the binary interface for VST2 plugin hosting.
// ============================================================================

#if defined(_WIN32)
  #define VST_EXPORT extern "C" __declspec(dllexport)
#else
  #define VST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

typedef int32_t  VstInt32;
typedef intptr_t VstIntPtr;

// VST2 magic number
const VstInt32 kEffectMagic = 0x56737450; // 'VstP'

// Dispatcher opcodes (host → plugin)
enum {
    effOpen            = 0,
    effClose           = 1,
    effSetProgram      = 2,
    effGetProgram      = 3,
    effGetProgramName  = 5,
    effGetParamLabel   = 6,
    effGetParamDisplay = 7,
    effGetParamName    = 8,
    effSetSampleRate   = 10,
    effSetBlockSize    = 11,
    effMainsChanged    = 12,
    effEditGetRect     = 13,
    effEditOpen        = 14,
    effEditClose       = 15,
    effGetChunk        = 23,
    effSetChunk        = 24,
    effProcessEvents   = 25,
    effCanBeAutomated  = 26,
    effGetEffectName   = 45,
    effGetVendorString = 47,
    effGetProductString= 48,
    effGetVendorVersion= 49,
    effCanDo           = 51,
    effGetPlugCategory = 35,
    effBeginSetProgram = 67,
    effEndSetProgram   = 68,
};

// Plugin categories
enum { kPlugCategEffect = 1 };

// Plugin capability flags
enum {
    effFlagsHasEditor   = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth     = 1 << 8,
    effFlagsCanDoubleReplacing = 1 << 12,
};

struct ERect {
    int16_t top, left, bottom, right;
};

// Forward declarations
struct AEffect;
typedef VstIntPtr (*audioMasterCallback)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
typedef VstIntPtr (*AEffectDispatcherProc)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
typedef void      (*AEffectProcessProc)(AEffect*, float**, float**, VstInt32);
typedef void      (*AEffectSetParameterProc)(AEffect*, VstInt32, float);
typedef float     (*AEffectGetParameterProc)(AEffect*, VstInt32);

struct AEffect {
    VstInt32 magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;         // deprecated, but must be present
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
    VstInt32 realQualities;   // unused
    VstInt32 offQualities;    // unused
    float    ioRatio;         // unused
    void*    object;          // pointer to plugin instance
    void*    user;
    VstInt32 uniqueID;
    VstInt32 version;
    AEffectProcessProc processReplacing;
    AEffectProcessProc processDoubleReplacing;
    char     future[56];
};


// ============================================================================
//  CognGate Plugin Implementation
// ============================================================================

// Parameter indices
enum {
    kParamSilenceDuration = 0,  // 0.1 – 10.0 seconds
    kParamMaxLeeway       = 1,  // 0.1 – 10.0 seconds
    kParamSmoothness      = 2,  // 1.0 – 100.0 milliseconds
    kNumParams            = 3
};

// Gate states
enum GateState {
    STATE_AUDIO   = 0,
    STATE_SILENCE = 1
};

struct CognGatePlugin {
    AEffect effect;

    // Parameters stored as normalized [0, 1] values
    float params[kNumParams];

    // DSP state
    GateState   state;
    double      sampleRate;
    float       currentGain;     // working gain applied per-sample
    float       targetGain;      // 1.0 for AUDIO, 0.0 for SILENCE
    float       gainStepPerSample;
    int64_t     samplesRemaining;

    // Random number generator
    std::mt19937 rng;

    // ------------------------------------------------------------------
    //  Parameter mapping: normalized [0,1] ↔ real value
    // ------------------------------------------------------------------

    static float normalizeParam(VstInt32 index, float real) {
        switch (index) {
            case kParamSilenceDuration: return (real - 0.1f) / 9.9f;
            case kParamMaxLeeway:       return (real - 0.1f) / 9.9f;
            case kParamSmoothness:      return (real - 1.0f) / 99.0f;
            default: return 0.0f;
        }
    }

    static float denormalizeParam(VstInt32 index, float norm) {
        switch (index) {
            case kParamSilenceDuration: return 0.1f + norm * 9.9f;
            case kParamMaxLeeway:       return 0.1f + norm * 9.9f;
            case kParamSmoothness:      return 1.0f + norm * 99.0f;
            default: return 0.0f;
        }
    }

    float getRealParam(VstInt32 index) const {
        return denormalizeParam(index, params[index]);
    }

    // ------------------------------------------------------------------
    //  DSP helpers
    // ------------------------------------------------------------------

    void recalcGainStep() {
        float smoothMs  = getRealParam(kParamSmoothness);
        float smoothSec = smoothMs * 0.001f;
        float smoothSamples = static_cast<float>(sampleRate) * smoothSec;
        gainStepPerSample = (smoothSamples > 0.0f) ? (1.0f / smoothSamples) : 1.0f;
    }

    int64_t rollNewLeewayInSamples() {
        float maxLeeway = getRealParam(kParamMaxLeeway);
        float upper = (maxLeeway > 0.1f) ? maxLeeway : 0.1f;
        std::uniform_real_distribution<float> dist(0.1f, upper);
        float randomSeconds = dist(rng);
        return static_cast<int64_t>(randomSeconds * sampleRate);
    }

    void resetState() {
        state = STATE_AUDIO;
        currentGain = 1.0f;
        targetGain  = 1.0f;
        recalcGainStep();
        samplesRemaining = rollNewLeewayInSamples();
    }
};


// ============================================================================
//  VST2 Callback Implementations
// ============================================================================

static void cogngate_setParameter(AEffect* effect, VstInt32 index, float value) {
    CognGatePlugin* plugin = (CognGatePlugin*)effect->object;
    if (index >= 0 && index < kNumParams)
        plugin->params[index] = value;
}

static float cogngate_getParameter(AEffect* effect, VstInt32 index) {
    CognGatePlugin* plugin = (CognGatePlugin*)effect->object;
    if (index >= 0 && index < kNumParams)
        return plugin->params[index];
    return 0.0f;
}

// ------------------------------------------------------------------
//  The core DSP loop — fully autonomous, no host transport used
// ------------------------------------------------------------------
static void cogngate_processReplacing(AEffect* effect, float** inputs,
                                       float** outputs, VstInt32 sampleFrames)
{
    CognGatePlugin* plugin = (CognGatePlugin*)effect->object;

    float* inL  = inputs[0];
    float* inR  = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];

    // Snapshot parameters once per block
    float silenceDur = plugin->getRealParam(kParamSilenceDuration);
    float maxLeeway  = plugin->getRealParam(kParamMaxLeeway);

    // Recalculate ramp step from smoothness
    plugin->recalcGainStep();

    float gain     = plugin->currentGain;
    float target   = plugin->targetGain;
    float step     = plugin->gainStepPerSample;
    int64_t remain = plugin->samplesRemaining;

    for (VstInt32 i = 0; i < sampleFrames; ++i) {
        // ── State transition check ──────────────────────────────────────
        if (remain <= 0) {
            if (plugin->state == STATE_AUDIO) {
                // AUDIO → SILENCE
                plugin->state = STATE_SILENCE;
                target = 0.0f;
                remain = static_cast<int64_t>(silenceDur * plugin->sampleRate);
            } else {
                // SILENCE → AUDIO
                plugin->state = STATE_AUDIO;
                target = 1.0f;

                // Roll a fresh random leeway
                float upper = (maxLeeway > 0.1f) ? maxLeeway : 0.1f;
                std::uniform_real_distribution<float> dist(0.1f, upper);
                float randomSec = dist(plugin->rng);
                remain = static_cast<int64_t>(randomSec * plugin->sampleRate);
            }
        }

        // ── Linear gain ramp toward target ──────────────────────────────
        if (gain < target) {
            gain += step;
            if (gain > target) gain = target;
        } else if (gain > target) {
            gain -= step;
            if (gain < target) gain = target;
        }

        // ── Apply gain to both channels ─────────────────────────────────
        outL[i] = inL[i] * gain;
        outR[i] = inR[i] * gain;

        --remain;
    }

    // Write back state
    plugin->currentGain     = gain;
    plugin->targetGain      = target;
    plugin->samplesRemaining = remain;
}

// Deprecated process callback (some hosts still call it)
static void cogngate_process(AEffect* effect, float** inputs,
                              float** outputs, VstInt32 sampleFrames) {
    cogngate_processReplacing(effect, inputs, outputs, sampleFrames);
}

// ------------------------------------------------------------------
//  Dispatcher: handles all host ↔ plugin communication
// ------------------------------------------------------------------
static VstIntPtr cogngate_dispatcher(AEffect* effect, VstInt32 opcode,
                                      VstInt32 index, VstIntPtr value,
                                      void* ptr, float opt)
{
    CognGatePlugin* plugin = (CognGatePlugin*)effect->object;

    switch (opcode) {
        case effOpen:
            return 0;

        case effClose:
            delete plugin;
            return 0;

        case effSetSampleRate:
            plugin->sampleRate = static_cast<double>(opt);
            plugin->rng.seed(std::random_device{}());
            plugin->resetState();
            return 0;

        case effSetBlockSize:
            return 0;

        case effMainsChanged:
            if (value) {
                // Plugin activated — reseed and reset
                plugin->rng.seed(std::random_device{}());
                plugin->resetState();
            }
            return 0;

        case effGetEffectName:
            strncpy((char*)ptr, "CognGate", 31);
            return 1;

        case effGetVendorString:
            strncpy((char*)ptr, "CognGateAudio", 63);
            return 1;

        case effGetProductString:
            strncpy((char*)ptr, "CognGate Brain Training Gate", 63);
            return 1;

        case effGetVendorVersion:
            return 1000; // version 1.0.0.0

        case effGetPlugCategory:
            return kPlugCategEffect;

        case effCanDo:
            if (ptr) {
                // We support these capabilities
                if (strcmp((char*)ptr, "receiveVstEvents") == 0) return 0;
                if (strcmp((char*)ptr, "receiveVstMidiEvent") == 0) return 0;
            }
            return 0;

        case effCanBeAutomated:
            return 1; // all parameters are automatable

        case effGetParamName:
            switch (index) {
                case kParamSilenceDuration: strncpy((char*)ptr, "Silence Dur", 15); break;
                case kParamMaxLeeway:       strncpy((char*)ptr, "Max Leeway",  15); break;
                case kParamSmoothness:      strncpy((char*)ptr, "Smoothness",  15); break;
            }
            return 0;

        case effGetParamLabel:
            switch (index) {
                case kParamSilenceDuration: strncpy((char*)ptr, "sec", 15); break;
                case kParamMaxLeeway:       strncpy((char*)ptr, "sec", 15); break;
                case kParamSmoothness:      strncpy((char*)ptr, "ms",  15); break;
            }
            return 0;

        case effGetParamDisplay: {
            char buf[32];
            float real = plugin->getRealParam(index);
            snprintf(buf, sizeof(buf), "%.2f", real);
            strncpy((char*)ptr, buf, 15);
            return 0;
        }

        case effGetProgramName:
            strncpy((char*)ptr, "Default", 23);
            return 0;

        case effGetProgram:
            return 0;

        case effSetProgram:
            return 0;

        // State save/restore for Equalizer APO config persistence
        case effGetChunk: {
            // Allocate a block and store raw parameter values
            static float chunkData[kNumParams];
            for (int i = 0; i < kNumParams; ++i)
                chunkData[i] = plugin->params[i];
            *((float**)ptr) = chunkData;
            return static_cast<VstIntPtr>(sizeof(chunkData));
        }

        case effSetChunk: {
            if (value >= (VstIntPtr)sizeof(float) * kNumParams) {
                float* chunkData = (float*)ptr;
                for (int i = 0; i < kNumParams; ++i)
                    plugin->params[i] = chunkData[i];
            }
            return 0;
        }

        default:
            return 0;
    }
}


// ============================================================================
//  VST2 Entry Point — this is what the host calls to create the plugin
// ============================================================================

VST_EXPORT AEffect* VSTPluginMain(audioMasterCallback audioMaster) {
    if (!audioMaster)
        return nullptr;

    CognGatePlugin* plugin = new CognGatePlugin();
    memset(plugin, 0, sizeof(CognGatePlugin));

    AEffect* effect = &plugin->effect;

    effect->magic           = kEffectMagic;
    effect->dispatcher      = cogngate_dispatcher;
    effect->process         = cogngate_process;
    effect->setParameter    = cogngate_setParameter;
    effect->getParameter    = cogngate_getParameter;
    effect->numPrograms     = 1;
    effect->numParams       = kNumParams;
    effect->numInputs       = 2;   // stereo in
    effect->numOutputs      = 2;   // stereo out
    effect->flags           = effFlagsCanReplacing | effFlagsProgramChunks;
    effect->uniqueID        = 'CgGt';
    effect->version         = 1000;
    effect->processReplacing = cogngate_processReplacing;
    effect->object          = plugin;

    // Default parameter values (normalized)
    plugin->params[kParamSilenceDuration] = CognGatePlugin::normalizeParam(kParamSilenceDuration, 2.0f);
    plugin->params[kParamMaxLeeway]       = CognGatePlugin::normalizeParam(kParamMaxLeeway, 5.0f);
    plugin->params[kParamSmoothness]      = CognGatePlugin::normalizeParam(kParamSmoothness, 10.0f);

    // Initialize DSP state
    plugin->sampleRate = 44100.0;
    new (&plugin->rng) std::mt19937(std::random_device{}());
    plugin->resetState();

    return effect;
}

// Alternate entry point name used by some hosts
VST_EXPORT AEffect* main_plugin(audioMasterCallback audioMaster) {
    return VSTPluginMain(audioMaster);
}
