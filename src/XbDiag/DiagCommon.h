#pragma once
// DiagCommon.h
// Shared rendering infrastructure for XbDiag.
// All modules include this. Nothing here depends on any module.
//
// Provides:
//   - Screen layout constants
//   - Color palette (BiosScreen heritage)
//   - 2D vertex helpers: FillRect, FillRectGrad, HLine
//   - Text helpers: TW, DrawTextR, DrawLabelValue
//   - DiagResetShader
//   - Ftoi (safe float->int, no __ftol2_sse)
//   - String helpers: IntToStr, IntToHex, StrCat2, StrCopy, HexByte
//   - DDS loader
//   - DrawLogo
//   - DrawPageChrome  (top bar + bottom bar + logo + borders in one call)
//   - SMBus helpers

#include <xtl.h>

// ============================================================================
// Global D3D device  (defined in main.cpp, used everywhere)
// ============================================================================

extern LPDIRECT3DDEVICE8 g_pDevice;

// ============================================================================
// Screen geometry
// ============================================================================

// All layout is authored in 640x480 design units.
// SW/SH are ALWAYS 640/480 - they define the design canvas, not the backbuffer.
// The primitives (FillRect, DrawText etc.) apply g_sx/g_sy internally.
static const float SW = 640.0f;
static const float SH = 480.0f;
static const float TOP_BAR_H = 56.0f;

// BOT_BAR_Y / BOT_BAR_H are set for CRT title-safe compliance.
// Microsoft XDK guidelines specify 85% safe area = 7.5% inset per edge.
// At 480 design units: unsafe zone starts at y > 444 (480 * 0.925).
// Bottom bar text renders at: botY = BOT_BAR_Y + (BOT_BAR_H - textH) * 0.5
// With BOT_BAR_H=64, BOT_BAR_Y=416: botY = 416 + (64-9.1)*0.5 = 443.4
// This places hint text just inside the 7.5% title safe boundary on all CRTs.
// Modules using BOT_BAR_Y as their content lower limit gain 34px of usable space
// compared to the original BOT_BAR_Y=450.
static const float BOT_BAR_H = 64.0f;
static const float BOT_BAR_Y = SH - BOT_BAR_H;   // 416.0
static const float CONTENT_Y = TOP_BAR_H + 2.0f;
static const float TEXT_SCALE = 1.5f;
static const float LINE_H = 18.0f;
static const float GROUP_GAP = 10.0f;
static const float LM = 32.0f;
static const float VM = 200.0f;

// Video mode info - set by DetectVideoMode() in main.cpp
extern bool g_isHD;
extern bool g_isTrueInterlaced;  // true ONLY for 480i NTSC and 576i PAL50 — drives Font_SetSD
extern char g_videoModeStr[16];  // "480i" / "480p" / "720p" / "1080i" / "576i PAL" / "480i PAL60" / "480i PAL-M"

// ============================================================================
// Resolution scale factors
// All layout is authored in 640x480 design units.
// SX()/SY() scale any design-space coordinate to actual backbuffer pixels.
// Set by DetectVideoMode() alongside SW/SH.
// ============================================================================

extern float g_sx;   // SW / 640  (1.0 at 480i/480p, 2.0 at 720p width)
extern float g_sy;   // SH / 480  (1.0 at 480i/480p, 1.5 at 720p height)

inline float SX(float v) { return v * g_sx; }
inline float SY(float v) { return v * g_sy; }
// SS = scale for font size (use smaller axis to keep text proportional)
inline float SS(float v) { return v * (g_sx < g_sy ? g_sx : g_sy); }

// ScaledDrawText wraps font's DrawText, applying SX/SY/SS to all coordinates.
// We macro DrawText -> ScaledDrawText so all existing call sites work unchanged.
// Include DiagCommon.h BEFORE font.h in every module (or don't include font.h -
// DiagCommon.h includes it for you via the macro redirect).
void ScaledDrawText(float x, float y, const char* text, float scale, DWORD color);
#ifndef DrawText
#define DrawText ScaledDrawText
#endif

// ============================================================================
// Color palette  (BiosScreen heritage)
// ============================================================================

static const DWORD COL_BG = D3DCOLOR_XRGB(10, 10, 26);  // near-black navy
static const DWORD COL_BAR_TOP = D3DCOLOR_XRGB(26, 42, 90);  // dark steel blue
static const DWORD COL_BAR_BOT = D3DCOLOR_XRGB(26, 26, 42);  // dark gray-blue
static const DWORD COL_WHITE = D3DCOLOR_XRGB(220, 220, 220);
static const DWORD COL_CYAN = D3DCOLOR_XRGB(80, 220, 255);  // detected values / highlights
static const DWORD COL_YELLOW = D3DCOLOR_XRGB(255, 220, 60);  // prompt text
static const DWORD COL_GRAY = D3DCOLOR_XRGB(130, 130, 150);  // dim labels
static const DWORD COL_GREEN = D3DCOLOR_XRGB(80, 220, 100);  // OK / pass
static const DWORD COL_RED = D3DCOLOR_XRGB(220, 60, 60);  // fail / error
static const DWORD COL_ORANGE = D3DCOLOR_XRGB(255, 160, 40);  // warning
static const DWORD COL_BORDER = D3DCOLOR_XRGB(50, 80, 160);  // separator lines
static const DWORD COL_SEL_BAR = D3DCOLOR_XRGB(30, 55, 110);  // menu selection highlight
static const DWORD COL_SEL_BAR2 = D3DCOLOR_XRGB(18, 35, 75);  // selection highlight gradient end
static const DWORD COL_DIM = D3DCOLOR_XRGB(70, 70, 90);  // very dim / disabled

// ============================================================================
// Vertex types
// ============================================================================

struct DV { float x, y, z, rhw; DWORD c; };             // diffuse only
struct DVT { float x, y, z, rhw; DWORD c; float u, v; }; // diffuse + tex

// ============================================================================
// DDS texture info (used by DrawPageChrome callers)
// ============================================================================

struct DiagLogo
{
    LPDIRECT3DTEXTURE8 tex;
    int                w;
    int                h;
};

// ============================================================================
// Function declarations
// ============================================================================

// --- D3D state ---
// Call after any textured draw to restore plain XYZRHW|DIFFUSE shader.
void DiagResetShader();

// --- Safe float->int (no __ftol2_sse) ---
int  Ftoi(float f);

// --- 2D filled rectangles ---
void FillRect(float x0, float y0, float x1, float y1, DWORD col);
void FillRectGrad(float x0, float y0, float x1, float y1, DWORD cTop, DWORD cBot);
void HLine(float y, float x0, float x1, DWORD col);
void VLine(float x, float y0, float y1, DWORD col);

// --- Text measurement & drawing ---
float TW(const char* s, float scale);                                // measure width
void  DrawTextR(float rightX, float y, const char* s, float sc, DWORD col); // right-aligned
void  DrawLabelValue(float x, float y,
    const char* label, const char* value,
    float sc, DWORD labelCol, DWORD valueCol);

// --- String helpers (no CRT sprintf) ---
void IntToStr(int  v, char* buf, int bufLen);
void UIntToStr(unsigned v, char* buf, int bufLen);
void IntToHex(unsigned v, int digits, char* buf, int bufLen); // zero-padded hex
void HexByte(unsigned char b, char* out2);                  // writes exactly 2 hex chars + NUL
void StrCopy(char* dst, int dstLen, const char* src);
void StrCat2(char* buf, int bufLen, const char* a, const char* b);
void StrCat3(char* buf, int bufLen, const char* a, const char* b, const char* c);
int  StrLen(const char* s);

// --- DDS loader (uncompressed A8R8G8B8 only) ---
LPDIRECT3DTEXTURE8 DiagLoadDDS(const char* path, int& outW, int& outH);

// --- Logo draw ---
void DrawLogo(const DiagLogo& logo, float cx, float cy,
    float dispW, float dispH, BYTE alpha);

// --- Page chrome ---
// Draws background clear, top bar (with logo + title), bottom bar (with hints),
// and separator lines.  Call once per frame before drawing content.
//   title     : shown right-aligned in the top bar next to logo divider
//   hints     : bottom bar text, e.g. "[A] Select  [B] Back  [Start] Exit"
void DrawPageChrome(const DiagLogo& logo,
    const char* title,
    const char* hints);

// --- SMBus helpers ---
// All addresses are SOFTWARE-SHIFTED 8-bit (7-bit hw addr << 1), matching
// HalReadSMBusValue convention confirmed by PrometheOS reference source.
#define SMBADDR_PIC       0x20   // PIC16L / SMC
#define SMBADDR_ENC_CNXT  0x8A   // Conexant video encoder
#define SMBADDR_ENC_FOCUS 0xD4   // Focus FS454 video encoder
#define SMBADDR_ENC_XCAL  0xE0   // Xcalibur (1.6)
#define SMBADDR_ADM1032   0x98   // ADM1032 temp monitor (1.0-1.5 only)
#define SMBADDR_EEPROM    0xA8   // 93LC56 EEPROM
#define SMBADDR_ICS       0xD2   // ICS clock generator

// Returns true on ACK, writes value to outVal.  On NAK / error returns false.
bool SMBusRead(BYTE addr, BYTE reg, BYTE& outVal);
bool SMBusReadWord(BYTE addr, BYTE reg, WORD& outVal);
bool SMBusWrite(BYTE addr, BYTE reg, BYTE  val);