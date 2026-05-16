/*
  ==============================================================================
    CognGate.cpp — Cognitive Brain Training Audio Gate
    Single-file VST2 plugin with dark Win32 GUI for Equalizer APO

    Compiles to a single .dll with NO external dependencies.
    Build (MSVC):
      cl /LD /O2 /EHsc /std:c++17 CognGate.cpp /Fe:CognGate.dll user32.lib gdi32.lib ole32.lib advapi32.lib
  ==============================================================================
*/

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>

// ============================================================================
//  VST2 ABI Definitions
// ============================================================================

#define VST_EXPORT extern "C" __declspec(dllexport)

typedef int32_t  VstInt32;
typedef intptr_t VstIntPtr;

const VstInt32 kEffectMagic = 0x56737450;

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3,
    effGetProgramName = 5, effGetParamLabel = 6, effGetParamDisplay = 7,
    effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11,
    effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14,
    effEditClose = 15, effEditIdle = 19, effGetChunk = 23, effSetChunk = 24,
    effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51,
};

enum { kPlugCategEffect = 1 };
enum {
    effFlagsHasEditor    = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
};

struct ERect { int16_t top, left, bottom, right; };
struct AEffect;
typedef VstIntPtr (*audioMasterCallback)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
typedef VstIntPtr (*AEffectDispatcherProc)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
typedef void      (*AEffectProcessProc)(AEffect*, float**, float**, VstInt32);
typedef void      (*AEffectSetParameterProc)(AEffect*, VstInt32, float);
typedef float     (*AEffectGetParameterProc)(AEffect*, VstInt32);

struct AEffect {
    VstInt32 magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    VstInt32 numPrograms, numParams, numInputs, numOutputs, flags;
    VstIntPtr resvd1, resvd2;
    VstInt32 initialDelay, realQualities, offQualities;
    float ioRatio;
    void* object;
    void* user;
    VstInt32 uniqueID, version;
    AEffectProcessProc processReplacing;
    AEffectProcessProc processDoubleReplacing;
    char future[56];
};

// ============================================================================
//  GUI Constants & Colors
// ============================================================================

static const int GUI_W = 480;
static const int GUI_H = 360;

static const COLORREF COL_BG_TOP       = RGB(13, 27, 42);
static const COLORREF COL_BG_BOT       = RGB(26, 26, 46);
static const COLORREF COL_TITLE        = RGB(0, 210, 255);
static const COLORREF COL_SUBTITLE     = RGB(102, 119, 136);
static const COLORREF COL_LABEL        = RGB(200, 200, 200);
static const COLORREF COL_VALUE        = RGB(150, 150, 150);
static const COLORREF COL_TRACK_BG     = RGB(22, 33, 62);
static const COLORREF COL_TRACK_FILL   = RGB(15, 52, 96);
static const COLORREF COL_THUMB        = RGB(0, 210, 255);
static const COLORREF COL_THUMB_HOVER  = RGB(50, 230, 255);
static const COLORREF COL_AUDIO        = RGB(0, 210, 255);
static const COLORREF COL_SILENCE      = RGB(255, 68, 102);
static const COLORREF COL_BAR_BG       = RGB(22, 33, 62);
static const COLORREF COL_BORDER       = RGB(15, 52, 96);
static const COLORREF COL_DIVIDER      = RGB(30, 45, 70);

// ============================================================================
//  Plugin Parameters & State
// ============================================================================

enum {
    kParamSilenceDuration = 0,
    kParamMaxLeeway       = 1,
    kParamSmoothness      = 2,
    kNumParams            = 3
};

enum GateState { STATE_AUDIO = 0, STATE_SILENCE = 1 };

struct ParamInfo {
    const wchar_t* name;
    const char*    nameA;
    const wchar_t* unit;
    const char*    unitA;
    float minVal, maxVal, defaultVal;
    int   decimals;
};

static const ParamInfo PARAM_INFO[kNumParams] = {
    { L"Silence Duration", "Silence Dur", L"sec", "sec",  0.1f, 10.0f,  2.0f, 2 },
    { L"Max Leeway Wait",  "Max Leeway",  L"sec", "sec",  0.1f, 10.0f,  5.0f, 2 },
    { L"Smoothness",       "Smoothness",  L"ms",  "ms",   1.0f, 100.0f, 10.0f, 1 },
};

static float normToReal(int idx, float norm) {
    return PARAM_INFO[idx].minVal + norm * (PARAM_INFO[idx].maxVal - PARAM_INFO[idx].minVal);
}
static float realToNorm(int idx, float real) {
    float range = PARAM_INFO[idx].maxVal - PARAM_INFO[idx].minVal;
    return (range > 0.0f) ? (real - PARAM_INFO[idx].minVal) / range : 0.0f;
}

// ============================================================================
//  Plugin Instance
// ============================================================================

struct CognGatePlugin {
    AEffect effect;
    float params[kNumParams];

    GateState state;
    double    sampleRate;
    float     currentGain;
    float     targetGain;
    float     gainStepPerSample;
    int64_t   samplesRemaining;
    std::mt19937 rng;

    HWND hwndEditor;
    HWND hwndParent;
    bool editorOpen;
    int  dragParam;
    int  hoverParam;

    float getRealParam(int i) const { return normToReal(i, params[i]); }

    void recalcGainStep() {
        float ms = getRealParam(kParamSmoothness);
        float samples = (float)sampleRate * ms * 0.001f;
        gainStepPerSample = (samples > 0.0f) ? (1.0f / samples) : 1.0f;
    }

    int64_t rollLeeway() {
        float mx = getRealParam(kParamMaxLeeway);
        float upper = (mx > 0.1f) ? mx : 0.1f;
        std::uniform_real_distribution<float> d(0.1f, upper);
        return (int64_t)(d(rng) * sampleRate);
    }

    void resetState() {
        state = STATE_AUDIO;
        currentGain = 1.0f;
        targetGain = 1.0f;
        recalcGainStep();
        samplesRemaining = rollLeeway();
    }
};

// ============================================================================
//  GUI – Slider geometry
// ============================================================================

struct SliderRect {
    int labelY, trackX, trackY, trackW, trackH, valueX;
};

static const int SLIDER_START_Y = 80;
static const int SLIDER_SPACING = 72;
static const int TRACK_H = 8;
static const int THUMB_W = 16;
static const int THUMB_H = 22;

static SliderRect getSliderRect(int idx) {
    SliderRect r;
    r.labelY = SLIDER_START_Y + idx * SLIDER_SPACING;
    r.trackX = 30;
    r.trackY = r.labelY + 30;
    r.trackW = GUI_W - 120;
    r.trackH = TRACK_H;
    r.valueX = r.trackX + r.trackW + 12;
    return r;
}

static int thumbXFromNorm(const SliderRect& sr, float norm) {
    return sr.trackX + (int)(norm * (float)sr.trackW);
}

static float normFromMouseX(const SliderRect& sr, int mouseX) {
    float n = (float)(mouseX - sr.trackX) / (float)sr.trackW;
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    return n;
}

static bool hitTestSlider(int idx, int mx, int my) {
    SliderRect sr = getSliderRect(idx);
    int ty = sr.trackY - THUMB_H / 2;
    return (mx >= sr.trackX - THUMB_W && mx <= sr.trackX + sr.trackW + THUMB_W &&
            my >= ty && my <= ty + THUMB_H + 10);
}

// ============================================================================
//  GUI – GDI Drawing Helpers
// ============================================================================

static void fillRect(HDC hdc, int x, int y, int w, int h, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    RECT rc = { x, y, x + w, y + h };
    FillRect(hdc, &rc, br);
    DeleteObject(br);
}

static void fillRoundRect(HDC hdc, int x, int y, int w, int h, int r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, br);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    RoundRect(hdc, x, y, x + w, y + h, r, r);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
}

static void drawRoundRectBorder(HDC hdc, int x, int y, int w, int h, int r, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    RoundRect(hdc, x, y, x + w, y + h, r, r);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void drawText(HDC hdc, int x, int y, int w, int h, const wchar_t* text,
                     COLORREF color, int size, bool bold = false, UINT align = DT_LEFT) {
    HFONT font = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    RECT rc = { x, y, x + w, y + h };
    DrawTextW(hdc, text, -1, &rc, align | DT_SINGLELINE | DT_VCENTER);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// ============================================================================
//  GUI – Paint
// ============================================================================

static void paintEditor(HWND hwnd, CognGatePlugin* plugin) {
    PAINTSTRUCT ps;
    HDC hdcWin = BeginPaint(hwnd, &ps);

    HDC hdc = CreateCompatibleDC(hdcWin);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWin, GUI_W, GUI_H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);

    // Background gradient
    for (int y = 0; y < GUI_H; y++) {
        float t = (float)y / (float)GUI_H;
        int r = (int)(13 + t * (26 - 13));
        int g = (int)(27 + t * (26 - 27));
        int b = (int)(42 + t * (46 - 42));
        fillRect(hdc, 0, y, GUI_W, 1, RGB(r, g, b));
    }

    // Title
    drawText(hdc, 0, 10, GUI_W, 32, L"CognGate", COL_TITLE, 22, true, DT_CENTER);
    drawText(hdc, 0, 42, GUI_W, 20, L"Cognitive Brain Training Gate", COL_SUBTITLE, 11, false, DT_CENTER);
    fillRect(hdc, 30, 68, GUI_W - 60, 1, COL_DIVIDER);

    // Sliders
    for (int i = 0; i < kNumParams; i++) {
        SliderRect sr = getSliderRect(i);
        float norm = plugin->params[i];
        float real = normToReal(i, norm);

        drawText(hdc, sr.trackX, sr.labelY, 200, 22, PARAM_INFO[i].name, COL_LABEL, 14);

        fillRoundRect(hdc, sr.trackX, sr.trackY, sr.trackW, sr.trackH, 4, COL_TRACK_BG);

        int fillW = (int)(norm * sr.trackW);
        if (fillW > 0)
            fillRoundRect(hdc, sr.trackX, sr.trackY, fillW, sr.trackH, 4, COL_TRACK_FILL);

        int tx = thumbXFromNorm(sr, norm) - THUMB_W / 2;
        int ty = sr.trackY - (THUMB_H - sr.trackH) / 2;
        COLORREF thumbCol = (plugin->hoverParam == i || plugin->dragParam == i)
                            ? COL_THUMB_HOVER : COL_THUMB;
        fillRoundRect(hdc, tx, ty, THUMB_W, THUMB_H, 6, thumbCol);

        wchar_t valBuf[32];
        if (PARAM_INFO[i].decimals == 1)
            swprintf(valBuf, 32, L"%.1f %s", real, PARAM_INFO[i].unit);
        else
            swprintf(valBuf, 32, L"%.2f %s", real, PARAM_INFO[i].unit);
        drawText(hdc, sr.valueX, sr.trackY - 6, 80, 22, valBuf, COL_VALUE, 12);
    }

    // Divider
    int divY = SLIDER_START_Y + kNumParams * SLIDER_SPACING + 4;
    fillRect(hdc, 30, divY, GUI_W - 60, 1, COL_DIVIDER);

    // State indicator
    int stateY = divY + 12;
    bool isAudio = (plugin->state == STATE_AUDIO);
    const wchar_t* stateText = isAudio ? L"\x25B6  AUDIO PLAYING" : L"\x2716  SILENCE";
    COLORREF stateCol = isAudio ? COL_AUDIO : COL_SILENCE;
    drawText(hdc, 0, stateY, GUI_W, 24, stateText, stateCol, 16, true, DT_CENTER);

    // Gain bar
    int barY = stateY + 32;
    int barX = 30;
    int barW = GUI_W - 60;
    int barH = 14;

    fillRoundRect(hdc, barX, barY, barW, barH, 6, COL_BAR_BG);
    float gain = plugin->currentGain;
    int gfillW = (int)(gain * barW);
    if (gfillW > 0) {
        COLORREF barCol = isAudio ? COL_AUDIO : COL_SILENCE;
        fillRoundRect(hdc, barX, barY, gfillW, barH, 6, barCol);
    }
    drawRoundRectBorder(hdc, barX, barY, barW, barH, 6, COL_BORDER);

    wchar_t gainBuf[32];
    swprintf(gainBuf, 32, L"Gain: %.0f%%", gain * 100.0f);
    drawText(hdc, barX, barY + barH + 4, barW, 16, gainBuf, COL_VALUE, 11, false, DT_CENTER);

    BitBlt(hdcWin, 0, 0, GUI_W, GUI_H, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
}

// ============================================================================
//  GUI – Window Proc
// ============================================================================

static LRESULT CALLBACK EditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CognGatePlugin* plugin = (CognGatePlugin*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_PAINT:
            if (plugin) paintEditor(hwnd, plugin);
            return 0;

        case WM_TIMER:
            if (plugin) InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            if (!plugin) break;
            int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
            for (int i = 0; i < kNumParams; i++) {
                if (hitTestSlider(i, mx, my)) {
                    plugin->dragParam = i;
                    SetCapture(hwnd);
                    SliderRect sr = getSliderRect(i);
                    plugin->params[i] = normFromMouseX(sr, mx);
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!plugin) break;
            int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
            if (plugin->dragParam >= 0) {
                SliderRect sr = getSliderRect(plugin->dragParam);
                plugin->params[plugin->dragParam] = normFromMouseX(sr, mx);
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                int newHover = -1;
                for (int i = 0; i < kNumParams; i++) {
                    if (hitTestSlider(i, mx, my)) { newHover = i; break; }
                }
                if (newHover != plugin->hoverParam) {
                    plugin->hoverParam = newHover;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (plugin && plugin->dragParam >= 0) {
                plugin->dragParam = -1;
                ReleaseCapture();
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        default:
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================================
//  GUI – Window Class
// ============================================================================

static const wchar_t* EDITOR_CLASS = L"CognGateEditorClass";
static bool classRegistered = false;

static HINSTANCE getDllInstance() {
    HMODULE hMod = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&getDllInstance, &hMod);
    return (HINSTANCE)hMod;
}

static void registerEditorClass(HINSTANCE hInst) {
    if (classRegistered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = EditorWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = EDITOR_CLASS;
    RegisterClassExW(&wc);
    classRegistered = true;
}

// ============================================================================
//  VST2 Callbacks
// ============================================================================

static void cogngate_setParameter(AEffect* effect, VstInt32 index, float value) {
    CognGatePlugin* p = (CognGatePlugin*)effect->object;
    if (index >= 0 && index < kNumParams) p->params[index] = value;
}

static float cogngate_getParameter(AEffect* effect, VstInt32 index) {
    CognGatePlugin* p = (CognGatePlugin*)effect->object;
    return (index >= 0 && index < kNumParams) ? p->params[index] : 0.0f;
}

// ── Core DSP — fully autonomous, no host transport ──────────────────────
static void cogngate_processReplacing(AEffect* effect, float** inputs,
                                       float** outputs, VstInt32 sampleFrames)
{
    CognGatePlugin* p = (CognGatePlugin*)effect->object;

    float* inL  = inputs[0];
    float* inR  = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];

    float silenceDur = p->getRealParam(kParamSilenceDuration);
    float maxLeeway  = p->getRealParam(kParamMaxLeeway);
    p->recalcGainStep();

    float   gain   = p->currentGain;
    float   target = p->targetGain;
    float   step   = p->gainStepPerSample;
    int64_t remain = p->samplesRemaining;

    for (VstInt32 i = 0; i < sampleFrames; ++i) {
        if (remain <= 0) {
            if (p->state == STATE_AUDIO) {
                p->state = STATE_SILENCE;
                target = 0.0f;
                remain = (int64_t)(silenceDur * p->sampleRate);
            } else {
                p->state = STATE_AUDIO;
                target = 1.0f;
                float upper = (maxLeeway > 0.1f) ? maxLeeway : 0.1f;
                std::uniform_real_distribution<float> d(0.1f, upper);
                remain = (int64_t)(d(p->rng) * p->sampleRate);
            }
        }

        if (gain < target) { gain += step; if (gain > target) gain = target; }
        else if (gain > target) { gain -= step; if (gain < target) gain = target; }

        outL[i] = inL[i] * gain;
        outR[i] = inR[i] * gain;
        --remain;
    }

    p->currentGain      = gain;
    p->targetGain       = target;
    p->samplesRemaining = remain;
}

static void cogngate_process(AEffect* effect, float** inputs,
                              float** outputs, VstInt32 sampleFrames) {
    cogngate_processReplacing(effect, inputs, outputs, sampleFrames);
}

// ── Dispatcher ──────────────────────────────────────────────────────────
static ERect editorRect = { 0, 0, (int16_t)GUI_H, (int16_t)GUI_W };

static VstIntPtr cogngate_dispatcher(AEffect* effect, VstInt32 opcode,
                                      VstInt32 index, VstIntPtr value,
                                      void* ptr, float opt)
{
    CognGatePlugin* p = (CognGatePlugin*)effect->object;

    switch (opcode) {
        case effOpen: return 0;

        case effClose:
            delete p;
            return 0;

        case effEditGetRect:
            if (ptr) *((ERect**)ptr) = &editorRect;
            return 1;

        case effEditOpen: {
            HINSTANCE hInst = getDllInstance();
            registerEditorClass(hInst);
            p->hwndParent = (HWND)ptr;
            p->hwndEditor = CreateWindowExW(
                0, EDITOR_CLASS, L"CognGate",
                WS_CHILD | WS_VISIBLE,
                0, 0, GUI_W, GUI_H,
                p->hwndParent, NULL, hInst, NULL);
            if (p->hwndEditor) {
                SetWindowLongPtr(p->hwndEditor, GWLP_USERDATA, (LONG_PTR)p);
                p->dragParam  = -1;
                p->hoverParam = -1;
                p->editorOpen = true;
                SetTimer(p->hwndEditor, 1, 33, NULL);
                InvalidateRect(p->hwndEditor, NULL, FALSE);
            }
            return p->hwndEditor ? 1 : 0;
        }

        case effEditClose:
            if (p->hwndEditor) {
                KillTimer(p->hwndEditor, 1);
                DestroyWindow(p->hwndEditor);
                p->hwndEditor = NULL;
                p->editorOpen = false;
            }
            return 0;

        case effEditIdle:
            if (p->hwndEditor) InvalidateRect(p->hwndEditor, NULL, FALSE);
            return 0;

        case effSetSampleRate:
            p->sampleRate = (double)opt;
            p->rng.seed(std::random_device{}());
            p->resetState();
            return 0;

        case effSetBlockSize: return 0;

        case effMainsChanged:
            if (value) { p->rng.seed(std::random_device{}()); p->resetState(); }
            return 0;

        case effGetEffectName:    strncpy((char*)ptr, "CognGate", 31); return 1;
        case effGetVendorString:  strncpy((char*)ptr, "CognGateAudio", 63); return 1;
        case effGetProductString: strncpy((char*)ptr, "CognGate Brain Training Gate", 63); return 1;
        case effGetVendorVersion: return 1000;
        case effGetPlugCategory:  return kPlugCategEffect;
        case effCanBeAutomated:   return 1;
        case effCanDo:            return 0;

        case effGetParamName:
            if (index >= 0 && index < kNumParams)
                strncpy((char*)ptr, PARAM_INFO[index].nameA, 15);
            return 0;

        case effGetParamLabel:
            if (index >= 0 && index < kNumParams)
                strncpy((char*)ptr, PARAM_INFO[index].unitA, 15);
            return 0;

        case effGetParamDisplay: {
            if (index >= 0 && index < kNumParams) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.2f", p->getRealParam(index));
                strncpy((char*)ptr, buf, 15);
            }
            return 0;
        }

        case effGetProgramName: strncpy((char*)ptr, "Default", 23); return 0;
        case effGetProgram: return 0;
        case effSetProgram: return 0;

        case effGetChunk: {
            static float chunkData[kNumParams];
            for (int i = 0; i < kNumParams; i++) chunkData[i] = p->params[i];
            *((float**)ptr) = chunkData;
            return (VstIntPtr)sizeof(chunkData);
        }

        case effSetChunk: {
            if (value >= (VstIntPtr)(sizeof(float) * kNumParams)) {
                float* d = (float*)ptr;
                for (int i = 0; i < kNumParams; i++) p->params[i] = d[i];
            }
            return 0;
        }

        default: return 0;
    }
}

// ============================================================================
//  Entry Point
// ============================================================================

VST_EXPORT AEffect* VSTPluginMain(audioMasterCallback audioMaster) {
    if (!audioMaster) return nullptr;

    CognGatePlugin* p = new CognGatePlugin();
    memset(p, 0, sizeof(CognGatePlugin));

    AEffect* e = &p->effect;
    e->magic            = kEffectMagic;
    e->dispatcher       = cogngate_dispatcher;
    e->process          = cogngate_process;
    e->setParameter     = cogngate_setParameter;
    e->getParameter     = cogngate_getParameter;
    e->numPrograms      = 1;
    e->numParams        = kNumParams;
    e->numInputs        = 2;
    e->numOutputs       = 2;
    e->flags            = effFlagsCanReplacing | effFlagsHasEditor | effFlagsProgramChunks;
    e->uniqueID         = 'CgGt';
    e->version          = 1000;
    e->processReplacing = cogngate_processReplacing;
    e->object           = p;

    for (int i = 0; i < kNumParams; i++)
        p->params[i] = realToNorm(i, PARAM_INFO[i].defaultVal);

    p->sampleRate  = 44100.0;
    p->hwndEditor  = NULL;
    p->editorOpen  = false;
    p->dragParam   = -1;
    p->hoverParam  = -1;
    new (&p->rng) std::mt19937(std::random_device{}());
    p->resetState();

    return e;
}

VST_EXPORT AEffect* main_plugin(audioMasterCallback audioMaster) {
    return VSTPluginMain(audioMaster);
}
