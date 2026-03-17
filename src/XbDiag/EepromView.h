#pragma once
// EepromView.h
// XbDiag - EEPROM Viewer public interface and shared state.
//
// s_eeprom, s_readOK, s_view, and the repair/restore flags are defined in
// EepromView.cpp and exposed here as extern so EepromCrypto, EepromSettings,
// and EepromRepair can access them directly without coupling through functions.

#include "DiagCommon.h"
#include <xtl.h>

// ============================================================================
// Shared types
// ============================================================================

enum EepView { VIEW_DECODED = 0, VIEW_HEX, VIEW_EDIT, VIEW_REPAIR };

struct EepField
{
    BYTE        offset;
    BYTE        size;
    const char* name;
    const char* shortName;
};

// ============================================================================
// Shared globals — defined in EepromView.cpp
// ============================================================================

extern BYTE    s_eeprom[256];
extern WORD    s_prevBtns;
extern bool    s_readOK;
extern EepView s_eepView;

// Repair / restore flags read by EepromRepair, written by EepromView_OnEnter
extern bool    s_repRan;
extern bool    s_repConfirm;
extern bool    s_binExists;
extern bool    s_restoreDone;
extern bool    s_restoreOK;
extern bool    s_restoreConfirm;

// Field table — read by EepromSettings for card mapping
extern const EepField s_fields[];
extern const int      NUM_FIELDS;




// ============================================================================
// Shared layout constants (used by EepromView, EepromSettings, EepromRepair)
// ============================================================================

static const float EEP_CY = CONTENT_Y + 4.f;
static const float EEP_LH = LINE_H - 2.f;
static const float EEP_LM2 = LM + 8.f;

// ============================================================================
// Kernel EEPROM API (used by EepromView, EepromSettings, EepromRepair)
// ============================================================================

extern "C" LONG __stdcall ExQueryNonVolatileSetting(
    ULONG ValueIndex, ULONG* Type, void* Value, ULONG ValueLength, ULONG* ResultLength);

extern "C" LONG __stdcall ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, const void* Value, ULONG ValueLength);

// XC_ setting index constants (XAPI.H / PrometheOS)
#define XC_VIDEO_STANDARD    0x04
#define XC_VIDEO_FLAGS       0x05
#define XC_AUDIO_FLAGS       0x06
#define XC_GAME_REGION       0x07
#define XC_DVD_REGION        0x08
#define XC_MAX_GAME_RATING   0x09
#define XC_TIMEZONE_BIAS     0x0A
#define XC_TIMEZONE_STD_NAME 0x0B
#define XC_TIMEZONE_STD_DATE 0x0C
#define XC_TIMEZONE_STD_BIAS 0x0D
#define XC_TIMEZONE_DST_NAME 0x0E
#define XC_TIMEZONE_DST_DATE 0x0F
#define XC_TIMEZONE_DST_BIAS 0x10
#define REG_DWORD            4

// ============================================================================
// Shared inline helpers (used by EepromView, EepromSettings, EepromRepair)
// ============================================================================

#include "font.h"  // for StrCopy, StrCat2, IntToHex, StrLen

static inline bool EepEdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static inline void EepSafeCopy(char* dst, int n, const char* src)
{
    StrCopy(dst, n, src);
}

static inline void EepSafeAppend(char* dst, int n, const char* src)
{
    StrCat2(dst, n, dst, src);
}

static inline void EepFmtHex(const BYTE* data, int count, char* out, int outLen)
{
    out[0] = 0;
    char t[4];
    for (int i = 0; i < count && (outLen - (int)StrLen(out)) > 3; ++i)
    {
        if (i > 0) EepSafeAppend(out, outLen, " ");
        IntToHex(data[i], 2, t, sizeof(t));
        EepSafeAppend(out, outLen, t);
    }
}

static inline void EepFmtMAC(const BYTE* d, char* out, int outLen)
{
    out[0] = 0; char t[4];
    for (int i = 0; i < 6; ++i)
    {
        IntToHex(d[i], 2, t, sizeof(t));
        EepSafeAppend(out, outLen, t);
        if (i < 5) EepSafeAppend(out, outLen, ":");
    }
}

static inline void EepFmtDword(DWORD v, char* out, int outLen)
{
    char t[12];
    EepSafeCopy(out, outLen, "0x");
    IntToHex((v >> 16) & 0xFFFF, 4, t, sizeof(t)); EepSafeAppend(out, outLen, t);
    IntToHex(v & 0xFFFF, 4, t, sizeof(t));          EepSafeAppend(out, outLen, t);
}

static inline DWORD EepReadDW(int off)
{
    return (DWORD)s_eeprom[off]
        | ((DWORD)s_eeprom[off + 1] << 8)
        | ((DWORD)s_eeprom[off + 2] << 16)
        | ((DWORD)s_eeprom[off + 3] << 24);
}


// ---- Buffer write/read helpers (used by EepromSettings and EepromRepair) ----

static inline void BufWriteDW(BYTE* buf, int off, DWORD val)
{
    buf[off + 0] = (BYTE)(val);      buf[off + 1] = (BYTE)(val >> 8);
    buf[off + 2] = (BYTE)(val >> 16);  buf[off + 3] = (BYTE)(val >> 24);
}

static inline DWORD BufReadDW(const BYTE* buf, int off)
{
    return (DWORD)buf[off] | ((DWORD)buf[off + 1] << 8) | ((DWORD)buf[off + 2] << 16) | ((DWORD)buf[off + 3] << 24);
}

// ============================================================================
// Public API
// ============================================================================

void EepromView_Reload();       // re-read EEPROM from chip and rebuild repair diag
void EepromView_OnEnter();
void EepromView_Tick(const DiagLogo& logo);
void EepromView_AutoRun(HANDLE hReport);