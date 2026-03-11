// EepromView.cpp
// XbDiag - EEPROM Viewer
//
// Two views toggled with [Left/Right]:
//
//  HEX VIEW
//  --------
//  16 rows x 16 bytes = 256 bytes total
//  Each row: "AA  xx xx xx xx xx xx xx xx  xx xx xx xx xx xx xx xx  ................"
//             offset  8 bytes              8 bytes                   ASCII
//  D-pad up/down scrolls cursor row (highlights row, shows field name at bottom)
//  No scrolling needed — all 16 rows fit in content area at LINE_H-1 spacing
//
//  DECODED VIEW
//  ------------
//  Named fields in scrollable list, two columns:
//    LEFT:  field label + hex bytes (short fields inline, long fields 2 rows)
//    RIGHT: decoded human-readable value
//  D-pad up/down scrolls field list
//
//  [A]         Save E:\eeprom.bin (raw 256 bytes)  [skipped if no HDD]
//  [B]/[Back]  Return to menu
//  [Left/Right] Toggle hex / decoded view
//
// EEPROM map (SMBus 0x54, 256 bytes):
//   0x00-0x11  18  HMAC Confounder     key derivation seed
//   0x12-0x21  16  HDD Key             ATA security key
//   0x22-0x31  16  Console Key Seed    online auth seed
//   0x32-0x33   2  Region              0x01=NTSC-M 0x02=NTSC-J 0x04=PAL
//   0x34-0x3F  12  Serial Number       ASCII
//   0x40-0x47   8  Online Key          Xbox Live key material
//   0x48-0x4B   4  Video Flags         display standard + AV flags
//   0x4C-0x4F   4  Game Region         game region DWORD
//   0x50-0x53   4  DVD Region          DVD CSS region
//   0x54-0x57   4  Parental Control    pin / settings
//   0x58-0x5F   8  Timezone Bias       UTC bias + DST bias
//   0x60-0x63   4  TZ Name 1           timezone string (4 chars)
//   0x64-0x67   4  TZ Name 2           timezone string alt (4 chars)
//   0x70-0x73   4  DST Start           DST transition start
//   0x74-0x77   4  DST End             DST transition end
//   0x80-0x87   8  Last Boot Time      FILETIME (100ns since 1601-01-01)
//   0x88-0x8B   4  DVD Playback Key    CSS key material
//   0x9C-0x9F   4  Misc Flags          system flags
//   0xA0-0xDF  64  Online Block        Xbox Live reserved
//   0xEA-0xEF   6  MAC Address         Ethernet MAC

#include "EepromView.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Layout constants
// ============================================================================

static const float CY = CONTENT_Y + 4.f;
static const float LH = LINE_H - 2.f;
static const float LM2 = LM + 8.f;

// Hex view column positions (640 design space)
static const float HEX_OFF_X = LM2;           // offset label "AA"
static const float HEX_B0_X = LM2 + 28.f;   // first group of 8 bytes
static const float HEX_B8_X = LM2 + 164.f;  // second group of 8 bytes
static const float HEX_ASC_X = LM2 + 302.f;  // ASCII
static const float HEX_FLD_Y = BOT_BAR_Y - 14.f; // field label strip above bot bar

// Decoded view columns
static const float DEC_LBL_X = LM2;
static const float DEC_HEX_X = LM2 + 130.f;
static const float DEC_VAL_X = LM2 + 310.f;
static const int   DEC_VISIBLE = 13; // rows visible at once

// ============================================================================
// EEPROM field table
// ============================================================================

struct EepField
{
    BYTE        offset;
    BYTE        size;
    const char* name;
    const char* shortName; // shown in hex view row highlight
};

static const EepField s_fields[] =
{
    { 0x00, 18, "HMAC Confounder",  "HMAC CONFOUNDER"  },
    { 0x12, 16, "HDD Key",          "HDD KEY"          },
    { 0x22, 16, "Console Key Seed", "CONSOLE KEY SEED" },
    { 0x32,  2, "Region Flags",     "REGION FLAGS"     },
    { 0x34, 12, "Serial Number",    "SERIAL NUMBER"    },
    { 0x40,  8, "Online Key",       "ONLINE KEY"       },
    { 0x48,  4, "Video Flags",      "VIDEO FLAGS"      },
    { 0x4C,  4, "Game Region",      "GAME REGION"      },
    { 0x50,  4, "DVD Region",       "DVD REGION"       },
    { 0x54,  4, "Parental Control", "PARENTAL CTRL"    },
    { 0x58,  8, "Timezone Bias",    "TIMEZONE BIAS"    },
    { 0x60,  4, "TZ Name 1",        "TZ NAME 1"        },
    { 0x64,  4, "TZ Name 2",        "TZ NAME 2"        },
    { 0x70,  4, "DST Start",        "DST START"        },
    { 0x74,  4, "DST End",          "DST END"          },
    { 0x80,  8, "Last Boot Time",   "LAST BOOT TIME"   },
    { 0x88,  4, "DVD Playback Key", "DVD PLAYBACK KEY" },
    { 0x9C,  4, "Misc Flags",       "MISC FLAGS"       },
    { 0xA0, 64, "Online Block",     "ONLINE BLOCK"     },
    { 0xEA,  6, "MAC Address",      "MAC ADDRESS"      },
};
static const int NUM_FIELDS = sizeof(s_fields) / sizeof(s_fields[0]);

// ============================================================================
// State
// ============================================================================

enum EepView { VIEW_HEX = 0, VIEW_DECODED };

static BYTE  s_eeprom[256];
static bool  s_readOK;
static WORD  s_prevBtns;

static EepView s_view;
static int     s_hexCurRow;   // 0-15, highlighted row in hex view
static int     s_decScroll;   // top field index in decoded view
static bool    s_saveDone;
static bool    s_saveOK;
static bool    s_loaded = false;

// ============================================================================
// Helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// Copy at most n-1 chars of src to dst, null-terminate
static void SafeCopy(char* dst, int n, const char* src)
{
    StrCopy(dst, n, src);
}

// Append src to dst (dst is both source and dest buffer)
static void SafeAppend(char* dst, int n, const char* src)
{
    StrCat2(dst, n, dst, src);
}

// Format bytes as "xx xx xx ..." into out (outLen includes null)
static void FmtHex(const BYTE* data, int count, char* out, int outLen)
{
    out[0] = '\0';
    char t[4];
    for (int i = 0; i < count && (outLen - (int)StrLen(out)) > 3; ++i)
    {
        if (i > 0) SafeAppend(out, outLen, " ");
        IntToHex(data[i], 2, t, sizeof(t));
        SafeAppend(out, outLen, t);
    }
}

// Format 6 bytes as "xx:xx:xx:xx:xx:xx"
static void FmtMAC(const BYTE* d, char* out, int outLen)
{
    out[0] = '\0';
    char t[4];
    for (int i = 0; i < 6; ++i)
    {
        IntToHex(d[i], 2, t, sizeof(t));
        SafeAppend(out, outLen, t);
        if (i < 5) SafeAppend(out, outLen, ":");
    }
}

// Format DWORD as "0xXXXXXXXX"
static void FmtDword(DWORD v, char* out, int outLen)
{
    char t[12];
    SafeCopy(out, outLen, "0x");
    IntToHex((v >> 16) & 0xFFFF, 4, t, sizeof(t)); SafeAppend(out, outLen, t);
    IntToHex(v & 0xFFFF, 4, t, sizeof(t)); SafeAppend(out, outLen, t);
}

// Read DWORD little-endian from eeprom at offset
static DWORD ReadDW(int off)
{
    return (DWORD)s_eeprom[off]
        | ((DWORD)s_eeprom[off + 1] << 8)
        | ((DWORD)s_eeprom[off + 2] << 16)
        | ((DWORD)s_eeprom[off + 3] << 24);
}

// ============================================================================
// Field value decoder
// ============================================================================

static void DecodeField(int fi, char* out, int outLen)
{
    out[0] = '\0';
    const EepField& f = s_fields[fi];
    const BYTE* p = s_eeprom + f.offset;
    char t[32];

    switch (f.offset)
    {
        // Region flags
    case 0x32:
    {
        BYTE r = p[0];
        if (r & 0x01) SafeCopy(out, outLen, "NTSC-M (N. America)");
        else if (r & 0x02) SafeCopy(out, outLen, "NTSC-J (Japan)");
        else if (r & 0x04) SafeCopy(out, outLen, "PAL (Europe/AUS)");
        else
        {
            SafeCopy(out, outLen, "0x");
            IntToHex(r, 2, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        break;
    }

    // Serial number — ASCII printable
    case 0x34:
    {
        int n = f.size < outLen - 1 ? f.size : outLen - 1;
        for (int i = 0; i < n; ++i)
        {
            char c = (char)p[i];
            t[0] = (c >= 0x20 && c < 0x7F) ? c : '.';
            t[1] = '\0';
            SafeAppend(out, outLen, t);
        }
        break;
    }

    // Video flags — bits
    case 0x48:
    {
        DWORD v = ReadDW(0x48);
        // Low byte = video standard
        BYTE vstd = (BYTE)(v & 0xFF);
        if (vstd == 0x00) SafeCopy(out, outLen, "NTSC-M");
        else if (vstd == 0x01) SafeCopy(out, outLen, "NTSC-J");
        else if (vstd == 0x02) SafeCopy(out, outLen, "PAL-I");
        else if (vstd == 0x03) SafeCopy(out, outLen, "PAL-M");
        else
        {
            SafeCopy(out, outLen, "vstd=0x");
            IntToHex(vstd, 2, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        // AV bits
        BYTE av = (BYTE)((v >> 8) & 0xFF);
        if (av & 0x01) SafeAppend(out, outLen, " HDTV");
        if (av & 0x02) SafeAppend(out, outLen, " 480p");
        if (av & 0x04) SafeAppend(out, outLen, " 720p");
        if (av & 0x08) SafeAppend(out, outLen, " 1080i");
        if (av & 0x10) SafeAppend(out, outLen, " 16:9");
        break;
    }

    // Game region
    case 0x4C:
    {
        DWORD v = ReadDW(0x4C);
        if (v == 0x00000001) SafeCopy(out, outLen, "NTSC-M");
        else if (v == 0x00000002) SafeCopy(out, outLen, "NTSC-J");
        else if (v == 0x00000004) SafeCopy(out, outLen, "PAL");
        else if (v == 0xFFFFFFFF) SafeCopy(out, outLen, "ALL (debug)");
        else FmtDword(v, out, outLen);
        break;
    }

    // DVD region
    case 0x50:
    {
        DWORD v = ReadDW(0x50);
        if (v == 0x00000001) SafeCopy(out, outLen, "Region 1 (USA)");
        else if (v == 0x00000002) SafeCopy(out, outLen, "Region 2 (EUR/JPN)");
        else if (v == 0x00000003) SafeCopy(out, outLen, "Region 3 (SE Asia)");
        else if (v == 0x00000004) SafeCopy(out, outLen, "Region 4 (AUS/LAT)");
        else if (v == 0x00000005) SafeCopy(out, outLen, "Region 5 (USSR)");
        else if (v == 0x00000006) SafeCopy(out, outLen, "Region 6 (China)");
        else FmtDword(v, out, outLen);
        break;
    }

    // Parental control — pin is BCD or raw DWORD
    case 0x54:
    {
        DWORD v = ReadDW(0x54);
        if (v == 0x00000000) SafeCopy(out, outLen, "DISABLED");
        else FmtDword(v, out, outLen);
        break;
    }

    // Timezone bias (int32 minutes from UTC, little-endian)
    case 0x58:
    {
        int bias = (int)ReadDW(0x58);
        int hrs = bias / 60;
        int mins = bias % 60;
        if (mins < 0) mins = -mins;
        SafeCopy(out, outLen, "UTC");
        if (hrs >= 0)
        {
            SafeAppend(out, outLen, "+");
            IntToStr(hrs, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        else
        {
            IntToStr(hrs, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        if (mins)
        {
            SafeAppend(out, outLen, ":");
            IntToStr(mins, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        break;
    }

    // TZ name strings — 4 ASCII chars
    case 0x60:
    case 0x64:
    {
        for (int i = 0; i < 4; ++i)
        {
            char c = (char)p[i];
            t[0] = (c >= 0x20 && c < 0x7F) ? c : '.';
            t[1] = '\0';
            SafeAppend(out, outLen, t);
        }
        break;
    }

    // Last boot time — FILETIME, convert to simple date
    case 0x80:
    {
        // FILETIME: 100ns intervals since 1601-01-01
        // We just show raw hi/lo DWORDs — full date math is complex
        DWORD lo = ReadDW(0x80);
        DWORD hi = ReadDW(0x84);
        if (lo == 0 && hi == 0)
            SafeCopy(out, outLen, "Never / Not Set");
        else
        {
            SafeCopy(out, outLen, "Hi:");
            FmtDword(hi, t, sizeof(t)); SafeAppend(out, outLen, t);
            SafeAppend(out, outLen, " Lo:");
            FmtDword(lo, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        break;
    }

    // MAC address
    case 0xEA:
        FmtMAC(p, out, outLen);
        break;

        // HDD key, console key, online key, online block, misc — show "see hex"
    default:
    {
        if (f.size <= 8)
            FmtHex(p, f.size, out, outLen);
        else
            SafeCopy(out, outLen, "(see hex view)");
        break;
    }
    }
}

// ============================================================================
// Which field does hex row belong to? (returns -1 if none / between fields)
// ============================================================================

static int RowToField(int row)
{
    int byteOff = row * 16;
    for (int i = 0; i < NUM_FIELDS; ++i)
    {
        int start = s_fields[i].offset;
        int end = start + s_fields[i].size - 1;
        // Row overlaps field if any byte in [byteOff, byteOff+15] touches field
        if (byteOff + 15 >= start && byteOff <= end)
            return i;
    }
    return -1;
}

// ============================================================================
// EEPROM read
// ============================================================================

// ExQueryNonVolatileSetting — kernel export for safe EEPROM access.
// Using index 0xFFFF reads all 256 bytes at once (ref: PrometheOS XKEEPROM::ReadFromXBOX).
// This is arbitrated by the kernel and is safe to call alongside other EEPROM users.
extern "C" LONG __stdcall ExQueryNonVolatileSetting(
    ULONG ValueIndex, ULONG* Type, void* Value, ULONG ValueLength, ULONG* ResultLength);

static void ReadEeprom()
{
    ULONG type = 0;
    ULONG resultLen = 0;
    LONG  status = ExQueryNonVolatileSetting(0xFFFF, &type, s_eeprom, 256, &resultLen);
    s_readOK = (status == 0 && resultLen >= 16);
    if (!s_readOK)
    {
        // Kernel read failed — zero fill so decoded view shows clean state
        for (int i = 0; i < 256; ++i) s_eeprom[i] = 0;
    }
}

// ============================================================================
// Save eeprom.bin
// ============================================================================

static void SaveBin()
{
    // D:\ is the XBE app directory — always mounted by the kernel as the title
    // directory, writable on modded hardware, and directly accessible via FTP.
    HANDLE hf = CreateFileA("D:\\eeprom.bin", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hf == INVALID_HANDLE_VALUE)
    {
        s_saveDone = true;
        s_saveOK = false;
        return;
    }

    DWORD bw = 0;
    BOOL  ok = WriteFile(hf, s_eeprom, 256, &bw, NULL);
    if (ok) FlushFileBuffers(hf);
    CloseHandle(hf);
    s_saveDone = true;
    s_saveOK = (ok && bw == 256);
}

// ============================================================================
// OnEnter
// ============================================================================

void EepromView_OnEnter()
{
    s_prevBtns = 0;
    s_view = VIEW_DECODED;
    s_hexCurRow = 0;
    s_decScroll = 0;
    s_saveDone = false;
    s_saveOK = false;
    s_loaded = false;
    s_readOK = false;
    // Zero the EEPROM buffer so a failed read always shows clean 00-fill,
    // not stale data from a previous visit.
    for (int i = 0; i < 256; ++i) s_eeprom[i] = 0;
}

// ============================================================================
// Render: HEX VIEW
// ============================================================================

static void RenderHex(const DiagLogo& logo)
{
    const char* hint = s_saveDone
        ? (s_saveOK ? "[A] Saved OK    [Right] Decoded    [B] Back"
            : "[A] Save failed [Right] Decoded    [B] Back")
        : "[A] Save eeprom.bin    [Right] Decoded    [B] Back";

    DrawPageChrome(logo, "EEPROM - HEX VIEW", hint);

    if (!s_readOK)
    {
        DrawText(LM2, CY, "EEPROM READ FAILED (SMBus 0x54 NAK)", 1.2f, COL_RED);
        return;
    }

    // Column headers
    float hy = CY;
    DrawText(HEX_OFF_X, hy, "OFF", 1.1f, COL_GRAY);
    DrawText(HEX_B0_X, hy, "00 01 02 03 04 05 06 07", 1.05f, COL_GRAY);
    DrawText(HEX_B8_X, hy, "08 09 0A 0B 0C 0D 0E 0F", 1.05f, COL_GRAY);
    DrawText(HEX_ASC_X, hy, "ASCII", 1.1f, COL_GRAY);
    hy += LH - 2.f;
    HLine(hy, LM2, SW - LM, COL_BORDER);
    hy += 3.f;

    for (int row = 0; row < 16; ++row)
    {
        float rowY = hy + row * LH;
        bool  sel = (row == s_hexCurRow);

        if (sel)
            FillRect(LM2 - 2.f, rowY - 1.f, SW - LM, rowY + LH, 0x28204040);

        // Offset label
        char offStr[6]; offStr[0] = '0'; offStr[1] = 'x';
        IntToHex(row * 16, 2, offStr + 2, 4); offStr[4] = '\0';
        DrawText(HEX_OFF_X, rowY, offStr, 1.05f, sel ? COL_YELLOW : COL_GRAY);

        // Bytes — two groups of 8
        const BYTE* rowData = s_eeprom + row * 16;
        char hexStr[28];

        FmtHex(rowData, 8, hexStr, sizeof(hexStr));
        DrawText(HEX_B0_X, rowY, hexStr, 1.05f, sel ? COL_WHITE : COL_CYAN);

        FmtHex(rowData + 8, 8, hexStr, sizeof(hexStr));
        DrawText(HEX_B8_X, rowY, hexStr, 1.05f, sel ? COL_WHITE : COL_CYAN);

        // ASCII
        char ascii[18]; ascii[16] = '\0';
        for (int c = 0; c < 16; ++c)
        {
            BYTE b = rowData[c];
            ascii[c] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        DrawText(HEX_ASC_X, rowY, ascii, 1.05f, sel ? COL_GREEN : COL_DIM);
    }

    // Field name strip above bottom bar
    int fi = RowToField(s_hexCurRow);
    if (fi >= 0)
    {
        char strip[64];
        SafeCopy(strip, sizeof(strip), "FIELD: ");
        SafeAppend(strip, sizeof(strip), s_fields[fi].shortName);
        SafeAppend(strip, sizeof(strip), "  [0x");
        char t[6];
        IntToHex(s_fields[fi].offset, 2, t, sizeof(t));
        SafeAppend(strip, sizeof(strip), t);
        SafeAppend(strip, sizeof(strip), " +");
        IntToStr(s_fields[fi].size, t, sizeof(t));
        SafeAppend(strip, sizeof(strip), t);
        SafeAppend(strip, sizeof(strip), "B]");
        DrawText(LM2, HEX_FLD_Y, strip, 1.2f, COL_YELLOW);
    }
    else
    {
        DrawText(LM2, HEX_FLD_Y, "FIELD: (reserved / unassigned)", 1.2f, COL_GRAY);
    }
}

// ============================================================================
// Render: DECODED VIEW
// ============================================================================

static void RenderDecoded(const DiagLogo& logo)
{
    const char* hint = s_saveDone
        ? (s_saveOK ? "[A] Saved OK    [Left] Hex    [B] Back"
            : "[A] Save failed [Left] Hex    [B] Back")
        : "[A] Save eeprom.bin    [Left] Hex    [B] Back";

    DrawPageChrome(logo, "EEPROM - DECODED VIEW", hint);

    if (!s_readOK)
    {
        DrawText(LM2, CY, "EEPROM READ FAILED (SMBus 0x54 NAK)", 1.2f, COL_RED);
        return;
    }

    // Column headers
    float hy = CY;
    DrawText(DEC_LBL_X, hy, "FIELD", 1.2f, COL_GRAY);
    DrawText(DEC_HEX_X, hy, "HEX", 1.2f, COL_GRAY);
    DrawText(DEC_VAL_X, hy, "DECODED", 1.2f, COL_GRAY);
    hy += LH - 2.f;
    HLine(hy, LM2, SW - LM, COL_BORDER);
    hy += 3.f;

    // In HD modes the content area fits all fields — no scroll or scrollbar needed.
    // In SD (480i/480p) cap to DEC_VISIBLE and show a scroll indicator.
    int visible = g_isHD ? NUM_FIELDS : DEC_VISIBLE;
    int scrollTop = g_isHD ? 0 : s_decScroll;

    int end = scrollTop + visible;
    if (end > NUM_FIELDS) end = NUM_FIELDS;

    for (int fi = scrollTop; fi < end; ++fi)
    {
        const EepField& f = s_fields[fi];
        const BYTE* p = s_eeprom + f.offset;
        float           rowY = hy + (fi - scrollTop) * LH;

        // Alternate row tint
        if ((fi & 1) == 0)
            FillRect(LM2 - 2.f, rowY - 1.f, SW - LM, rowY + LH - 1.f, 0x10FFFFFF);

        // Field name
        DrawText(DEC_LBL_X, rowY, f.name, 1.15f, COL_YELLOW);

        // Hex bytes — truncate long fields to 8 bytes displayed inline
        char hexStr[28];
        int  showBytes = f.size > 8 ? 8 : f.size;
        FmtHex(p, showBytes, hexStr, sizeof(hexStr));
        if (f.size > 8) SafeAppend(hexStr, sizeof(hexStr), "..");
        DrawText(DEC_HEX_X, rowY, hexStr, 1.0f, COL_CYAN);

        // Decoded value
        char decoded[64];
        DecodeField(fi, decoded, sizeof(decoded));
        DrawText(DEC_VAL_X, rowY, decoded, 1.15f, COL_WHITE);
    }

    // Scroll indicator — SD only
    if (!g_isHD && NUM_FIELDS > DEC_VISIBLE)
    {
        float sbTop = hy;
        float sbH = DEC_VISIBLE * LH;
        float thumbH = sbH * DEC_VISIBLE / (float)NUM_FIELDS;
        float thumbY = sbTop + sbH * s_decScroll / (float)NUM_FIELDS;
        VLine(SW - LM - 4.f, sbTop, sbTop + sbH, COL_BORDER);
        FillRect(SW - LM - 6.f, thumbY, SW - LM - 2.f, thumbY + thumbH, COL_CYAN);
    }
}

// ============================================================================
// Tick
// ============================================================================

void EepromView_Tick(const DiagLogo& logo)
{
    if (!s_loaded)
    {
        g_pDevice->BeginScene();
        DrawPageChrome(logo, "EEPROM VIEWER", "[B] Back");
        DrawText(LM, CONTENT_Y + 20.f, "Reading EEPROM...", 1.4f, COL_YELLOW);
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        ReadEeprom();
        s_loaded = true;
        return;
    }

    WORD cur = GetButtons();

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        RequestState(MSTATE_MENU);
        s_prevBtns = cur;
        return;
    }

    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT))
        s_view = VIEW_DECODED;

    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
        s_view = VIEW_HEX;

    if (EdgeDown(cur, s_prevBtns, BTN_A))
        SaveBin();

    if (s_view == VIEW_HEX)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
        {
            if (s_hexCurRow > 0)  --s_hexCurRow;
        }
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
        {
            if (s_hexCurRow < 15) ++s_hexCurRow;
        }
    }
    else
    {
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
        {
            if (!g_isHD && s_decScroll > 0) --s_decScroll;
        }
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
        {
            if (!g_isHD && s_decScroll < NUM_FIELDS - DEC_VISIBLE) ++s_decScroll;
        }
    }

    s_prevBtns = cur;

    g_pDevice->BeginScene();
    if (s_view == VIEW_HEX)
        RenderHex(logo);
    else
        RenderDecoded(logo);
    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}