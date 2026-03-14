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
// Offsets verified against XKEEPROM.h (Team Assembly) and PrometheOS EEPROMDATA struct.
//   0x00-0x13  20  HMAC SHA1 Hash      integrity checksum (encrypted section)
//   0x14-0x1B   8  Confounder          RC4 encrypted confounder
//   0x1C-0x2B  16  HDD Key             RC4 encrypted ATA security key
//   0x2C-0x2F   4  XBE Region          factory region code (little-endian DWORD)
//   0x30-0x33   4  Checksum2           checksum of following 44 bytes
//   0x34-0x3F  12  Serial Number       ASCII factory serial
//   0x40-0x45   6  MAC Address         Ethernet MAC
//   0x46-0x47   2  (reserved/pad)
//   0x48-0x57  16  Online Key          Xbox Live key (16 bytes)
//   0x58-0x5B   4  Video Standard      XC_VIDEO_STANDARD_* (1=NTSC-M 2=NTSC-J 3=PAL)
//   0x46-0x47   2  (reserved/pad)
//   0x48-0x4B   4  Video Standard      XC_VIDEO_STANDARD_* (1=NTSC-M 2=NTSC-J 3=PAL)
//   0x4C-0x4F   4  Video Flags         XC_VIDEO_FLAGS_* (widescreen, 480p, 720p, 1080i...)
//   0x50-0x53   4  Audio Flags         XC_AUDIO_FLAGS_* (stereo/mono/surround, AC3, DTS)
//   0x54-0x57   4  Game Region         XC_GAME_REGION_* (NA/Japan/RoW)
//   0x58-0x5B   4  DVD Region          CSS region code (1-6)
//   0x5C-0x5F   4  Parental Control    XC_PC_ESRB_* rating setting
//   0x60-0x63   4  TZ Std Bias         UTC offset minutes (int32, standard time)
//   0x64-0x67   4  TZ Dst Bias         UTC offset minutes (int32, daylight time)
//   0x68-0x6F   8  TZ Std Name         WCHAR[4] timezone abbreviation
//   0x70-0x77   8  TZ Dst Name         WCHAR[4] DST abbreviation
//   0x78-0x7B   4  TZ Std Date         transition date (packed SYSTEMTIME fields)
//   0x7C-0x7F   4  TZ Dst Date         transition date
//   0x80-0x87   8  Last Boot Time      FILETIME (100ns intervals since 1601-01-01)
//   0x88-0x8B   4  DVD Playback Key    CSS key material
//   0x9C-0x9F   4  Misc Flags          PIC scratch register shadow
//   0xA0-0xDF  64  Online Block        Xbox Live reserved block

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
static const int   DEC_VISIBLE = 20; // rows visible at once — always scroll, never overflow

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
    { 0x00, 20, "HMAC SHA1 Hash",   "HMAC SHA1 HASH"   },
    { 0x14,  8, "Confounder",       "CONFOUNDER"       },
    { 0x1C, 16, "HDD Key",          "HDD KEY"          },
    { 0x2C,  4, "XBE Region",       "XBE REGION"       },
    { 0x30,  4, "Checksum2",        "CHECKSUM2"        },
    { 0x34, 12, "Serial Number",    "SERIAL NUMBER"    },
    { 0x40,  6, "MAC Address",      "MAC ADDRESS"      },
    { 0x48, 16, "Online Key",       "ONLINE KEY"       },
    { 0x58,  4, "Video Standard",   "VIDEO STANDARD"   },
    { 0x4C,  4, "Video Flags",      "VIDEO FLAGS"      },
    { 0x50,  4, "Audio Flags",      "AUDIO FLAGS"      },
    { 0x54,  4, "Game Region",      "GAME REGION"      },
    { 0x58,  4, "DVD Region",       "DVD REGION"       },
    { 0x5C,  4, "Parental Control", "PARENTAL CTRL"    },
    { 0x60,  4, "TZ Std Bias",      "TZ STD BIAS"      },
    { 0x64,  4, "TZ Dst Bias",      "TZ DST BIAS"      },
    { 0x68,  8, "TZ Std Name",      "TZ STD NAME"      },
    { 0x70,  8, "TZ Dst Name",      "TZ DST NAME"      },
    { 0x78,  4, "TZ Std Date",      "TZ STD DATE"      },
    { 0x7C,  4, "TZ Dst Date",      "TZ DST DATE"      },
    { 0x80,  8, "Last Boot Time",   "LAST BOOT TIME"   },
    { 0x88,  4, "DVD Playback Key", "DVD PLAYBACK KEY" },
    { 0x9C,  4, "Misc Flags",       "MISC FLAGS"       },
    { 0xA0, 64, "Online Block",     "ONLINE BLOCK"     },
};
static const int NUM_FIELDS = sizeof(s_fields) / sizeof(s_fields[0]);

// ============================================================================
// State
// ============================================================================

enum EepView { VIEW_HEX = 0, VIEW_DECODED, VIEW_EDIT };

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
// Edit view state
// ============================================================================

// Cards: 0=VIDEO 1=AUDIO 2=REGION 3=TIME
static int  s_editCard;
static int  s_editCursor;   // field index within current card

// Working copies of editable fields (loaded from s_eeprom on VIEW_EDIT entry)
static DWORD s_editVideoStd;   // 1=NTSC-M 2=NTSC-J 3=PAL
static DWORD s_editVideoFlags; // bitmask
static DWORD s_editAudioFlags; // low word=mode, high word=AC3/DTS bits
static DWORD s_editGameRegion; // enum
static DWORD s_editDvdRegion;  // 1-6
static DWORD s_editParental;   // 0-6
static int   s_editTzIndex;    // index into s_tzTable

// Confirm-write prompt state
static bool  s_editConfirm;    // true = confirm overlay showing
static bool  s_editWriteDone;  // true = write result showing
static bool  s_editWriteOK;

// Card field counts
static const int CARD_FIELD_COUNT[4] = {
    7,  // VIDEO:  VideoStd, Wide, 720p, 1080i, 480p, Letterbox, PAL60 (PAL60 hidden when not PAL)
    3,  // AUDIO:  Mode, AC3, DTS
    3,  // REGION: GameRegion, DvdRegion, Parental
    1,  // TIME:   TZ picker (1 row)
};

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
        // XBE Region — factory DWORD, low byte is region
    case 0x2C:
    {
        BYTE r = p[0];
        if (r == 0x00) SafeCopy(out, outLen, "NTSC-M (N. America)");
        else if (r == 0x01) SafeCopy(out, outLen, "NTSC-J (Japan)");
        else if (r == 0x02) SafeCopy(out, outLen, "PAL (Europe/AUS)");
        else
        {
            SafeCopy(out, outLen, "0x");
            IntToHex(p[0], 2, t, sizeof(t)); SafeAppend(out, outLen, t);
            SafeAppend(out, outLen, " ");
            IntToHex(p[1], 2, t, sizeof(t)); SafeAppend(out, outLen, t);
            SafeAppend(out, outLen, " ");
            IntToHex(p[2], 2, t, sizeof(t)); SafeAppend(out, outLen, t);
            SafeAppend(out, outLen, " ");
            IntToHex(p[3], 2, t, sizeof(t)); SafeAppend(out, outLen, t);
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

    // MAC address
    case 0x40:
        FmtMAC(p, out, outLen);
        break;

        // Video Standard — XC_VIDEO_STANDARD_*
    case 0x48:
    {
        DWORD v = ReadDW(0x48);
        if (v == 1) SafeCopy(out, outLen, "NTSC-M (N. America)");
        else if (v == 2) SafeCopy(out, outLen, "NTSC-J (Japan)");
        else if (v == 3) SafeCopy(out, outLen, "PAL-I (Europe)");
        else
        {
            SafeCopy(out, outLen, "0x");
            IntToHex(v, 8, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        break;
    }

    // Video Flags — XC_VIDEO_FLAGS_* (bits 0-6, direct)
    case 0x4C:
    {
        DWORD v = ReadDW(0x4C);
        bool any = false;
        // XC_VIDEO_FLAGS_WIDESCREEN 0x01
        if (v & 0x00000001) { SafeAppend(out, outLen, "WIDE");    any = true; }
        // XC_VIDEO_FLAGS_HDTV_720p  0x02
        if (v & 0x00000002) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "720p");  any = true; }
        // XC_VIDEO_FLAGS_HDTV_1080i 0x04
        if (v & 0x00000004) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "1080i"); any = true; }
        // XC_VIDEO_FLAGS_HDTV_480p  0x08
        if (v & 0x00000008) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "480p");  any = true; }
        // XC_VIDEO_FLAGS_LETTERBOX  0x10
        if (v & 0x00000010) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "LBOX");  any = true; }
        // XC_VIDEO_FLAGS_PAL_60Hz   0x40
        if (v & 0x00000040) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "60Hz");  any = true; }
        if (!any) SafeCopy(out, outLen, "STANDARD (no HDTV)");
        break;
    }

    // Audio Flags — XC_AUDIO_FLAGS_*
    case 0x50:
    {
        DWORD v = ReadDW(0x50);
        // Low word: output mode
        DWORD basic = v & 0x0000FFFF;
        if (basic == 0x0000) SafeCopy(out, outLen, "Stereo");
        else if (basic == 0x0001) SafeCopy(out, outLen, "Mono");
        else if (basic == 0x0002) SafeCopy(out, outLen, "Surround");
        else
        {
            SafeCopy(out, outLen, "Unk=0x");
            IntToHex(basic, 4, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        // High word: encoded audio support
        if (v & 0x00010000) SafeAppend(out, outLen, " +AC3");
        if (v & 0x00020000) SafeAppend(out, outLen, " +DTS");
        break;
    }

    // Game region — XC_GAME_REGION_*
    case 0x54:
    {
        DWORD v = ReadDW(0x54);
        if (v == 0x00000001) SafeCopy(out, outLen, "N. America");
        else if (v == 0x00000002) SafeCopy(out, outLen, "Japan");
        else if (v == 0x00000004) SafeCopy(out, outLen, "Rest of World");
        else if (v == 0x80000000) SafeCopy(out, outLen, "Manufacturing");
        else if (v == 0xFFFFFFFF) SafeCopy(out, outLen, "ALL (debug)");
        else FmtDword(v, out, outLen);
        break;
    }

    // DVD region
    case 0x58:
    {
        DWORD v = ReadDW(0x58);
        if (v == 0x00000001) SafeCopy(out, outLen, "Region 1 (USA/CA)");
        else if (v == 0x00000002) SafeCopy(out, outLen, "Region 2 (EUR/JPN)");
        else if (v == 0x00000003) SafeCopy(out, outLen, "Region 3 (SE Asia)");
        else if (v == 0x00000004) SafeCopy(out, outLen, "Region 4 (AUS/LAT)");
        else if (v == 0x00000005) SafeCopy(out, outLen, "Region 5 (USSR)");
        else if (v == 0x00000006) SafeCopy(out, outLen, "Region 6 (China)");
        else FmtDword(v, out, outLen);
        break;
    }

    // Parental control — XC_PC_ESRB_*
    case 0x5C:
    {
        DWORD v = ReadDW(0x5C);
        if (v == 0) SafeCopy(out, outLen, "DISABLED (All)");
        else if (v == 1) SafeCopy(out, outLen, "Adults Only");
        else if (v == 2) SafeCopy(out, outLen, "Mature (M)");
        else if (v == 3) SafeCopy(out, outLen, "Teen (T)");
        else if (v == 4) SafeCopy(out, outLen, "Everyone (E)");
        else if (v == 5) SafeCopy(out, outLen, "Kids to Adults");
        else if (v == 6) SafeCopy(out, outLen, "Early Childhood");
        else FmtDword(v, out, outLen);
        break;
    }

    // TZ standard bias and DST bias — int32 minutes from UTC
    case 0x60:
    case 0x64:
    {
        int bias = (int)ReadDW(f.offset);
        if (bias == 0)
        {
            SafeCopy(out, outLen, "UTC+0");
            break;
        }
        // Kernel stores bias as minutes-WEST (subtract to get local).
        // Negate for human-readable UTC+ display.
        int display = -bias;
        int hrs = display / 60;
        int mins = display % 60;
        if (mins < 0) mins = -mins;
        SafeCopy(out, outLen, "UTC");
        if (hrs >= 0) SafeAppend(out, outLen, "+");
        IntToStr(hrs, t, sizeof(t)); SafeAppend(out, outLen, t);
        if (mins)
        {
            SafeAppend(out, outLen, ":");
            if (mins < 10) SafeAppend(out, outLen, "0");
            IntToStr(mins, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        break;
    }

    // TZ name strings — WCHAR[4] (UTF-16LE), extract ASCII low bytes
    case 0x68:
    case 0x70:
    {
        // Each WCHAR is 2 bytes; low byte is ASCII for standard timezone abbrevs
        for (int i = 0; i < 4; ++i)
        {
            BYTE lo = p[i * 2];
            t[0] = (lo >= 0x20 && lo < 0x7F) ? (char)lo : (lo ? '?' : '\0');
            if (t[0] == '\0') break;
            t[1] = '\0';
            SafeAppend(out, outLen, t);
        }
        if (out[0] == '\0') SafeCopy(out, outLen, "(not set)");
        break;
    }

    // TZ transition dates — packed: month/week/day/hour
    case 0x78:
    case 0x7C:
    {
        DWORD v = ReadDW(f.offset);
        if (v == 0)
        {
            SafeCopy(out, outLen, "(not set)");
            break;
        }
        // Standard SYSTEMTIME packing used by kernel
        BYTE month = (BYTE)(v & 0xFF);
        BYTE week = (BYTE)((v >> 8) & 0xFF);
        BYTE dow = (BYTE)((v >> 16) & 0xFF);
        BYTE hour = (BYTE)((v >> 24) & 0xFF);
        static const char* months[] = { "","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
        const char* mstr = (month >= 1 && month <= 12) ? months[month] : "?";
        SafeCopy(out, outLen, mstr);
        SafeAppend(out, outLen, " wk");
        IntToStr(week, t, sizeof(t)); SafeAppend(out, outLen, t);
        SafeAppend(out, outLen, " h");
        IntToStr(hour, t, sizeof(t)); SafeAppend(out, outLen, t);
        break;
    }

    // Last boot time — FILETIME
    case 0x80:
    {
        DWORD lo = ReadDW(0x80);
        DWORD hi = ReadDW(0x84);
        if (lo == 0 && hi == 0)
            SafeCopy(out, outLen, "Never / Not Set");
        else
        {
            // Convert FILETIME to approximate year (rough: hi*429 / 10^7 seconds since 1601)
            // hi * 429.4967296 seconds-worth of high 32-bits
            // Approx year: 1601 + (hi * 429 / 31536000)
            DWORD approxYear = 1601 + (hi * 429 / 31536000);
            SafeCopy(out, outLen, "~");
            IntToStr(approxYear, t, sizeof(t)); SafeAppend(out, outLen, t);
            SafeAppend(out, outLen, " (hi:");
            FmtDword(hi, t, sizeof(t)); SafeAppend(out, outLen, t);
            SafeAppend(out, outLen, ")");
        }
        break;
    }

    // Misc flags — PIC scratch register shadow (SCRATCH_REGISTER_BITVALUE_*)
    case 0x9C:
    {
        DWORD v = ReadDW(0x9C);
        if (v == 0)
        {
            SafeCopy(out, outLen, "NORMAL BOOT");
            break;
        }
        bool any = false;
        if (v & 0x01) { SafeAppend(out, outLen, "EJECT-BOOT"); any = true; }
        if (v & 0x02) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "DISP-ERR"); any = true; }
        if (v & 0x04) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "NO-ANIM");  any = true; }
        if (v & 0x08) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "DASH");     any = true; }
        // Show any unknown bits
        DWORD unk = v & ~0x0F;
        if (unk)
        {
            if (any) SafeAppend(out, outLen, " ");
            SafeAppend(out, outLen, "UNK=0x");
            IntToHex(unk, 8, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        break;
    }

    // Everything else: short fields show raw hex, long fields defer to hex view
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

extern "C" LONG __stdcall ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, const void* Value, ULONG ValueLength);

// XC_ index constants (ref: XAPI.H / PrometheOS XKEEPROM)
#define XC_VIDEO_STANDARD       0x04    // DWORD at 0x48
#define XC_VIDEO_FLAGS          0x05    // DWORD at 0x4C
#define XC_AUDIO_FLAGS          0x06    // DWORD at 0x50
#define XC_GAME_REGION          0x07    // DWORD at 0x54
#define XC_DVD_REGION           0x08    // DWORD at 0x58
#define XC_MAX_GAME_RATING      0x09    // DWORD at 0x5C
#define XC_TIMEZONE_BIAS        0x0A    // int32 at 0x60 (minutes west)
#define XC_TIMEZONE_STD_BIAS    0x0B    // int32 at 0x64 (DST offset)
#define REG_DWORD               4       // type tag for ExSaveNonVolatileSetting

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
    s_editCard = 0;
    s_editCursor = 0;
    s_editConfirm = false;
    s_editWriteDone = false;
    s_editWriteOK = false;
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
        ? (s_saveOK ? "[A] Saved OK    [Left] Hex    [Y] Edit    [B] Back"
            : "[A] Save failed [Left] Hex    [Y] Edit    [B] Back")
        : "[A] Save eeprom.bin    [Left] Hex    [Y] Edit    [B] Back";

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

    // Always cap to DEC_VISIBLE rows and scroll — field count exceeds content area height.
    int visible = DEC_VISIBLE;
    int scrollTop = s_decScroll;

    int end = scrollTop + visible;
    if (end > NUM_FIELDS) end = NUM_FIELDS;

    for (int fi = scrollTop; fi < end; ++fi)
    {
        const EepField& f = s_fields[fi];
        const BYTE* p = s_eeprom + f.offset;
        float           rowY = hy + (fi - scrollTop) * LH;

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

    // Scroll indicator
    if (NUM_FIELDS > DEC_VISIBLE)
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
// Timezone table — matches Xbox dashboard timezone list and kernel bias values.
// stdBias: minutes WEST of UTC (kernel convention, negate for UTC+ display).
// dstBias: additional DST offset in minutes (typically -60 = spring forward 1hr).
//          0 = no DST observed.
// ============================================================================

struct TzEntry
{
    const char* name;   // display name shown to user
    int         stdBias; // minutes west (stored at 0x60)
    int         dstBias; // DST offset minutes (stored at 0x64), 0=no DST
};

static const TzEntry s_tzTable[] =
{
    // Name                             stdBias   dstBias
    { "International Date Line West",    720,       0   }, // UTC-12
    { "Midway Island, Samoa",            660,       0   }, // UTC-11
    { "Hawaii",                          600,       0   }, // UTC-10 (no DST)
    { "Alaska",                          540,     -60   }, // UTC-9
    { "Pacific Time (US & Canada)",      480,     -60   }, // UTC-8
    { "Mountain Time (US & Canada)",     420,     -60   }, // UTC-7
    { "Arizona",                         420,       0   }, // UTC-7 no DST
    { "Central Time (US & Canada)",      360,     -60   }, // UTC-6
    { "Mexico City, Tegucigalpa",        360,     -60   }, // UTC-6
    { "Saskatchewan",                    360,       0   }, // UTC-6 no DST
    { "Eastern Time (US & Canada)",      300,     -60   }, // UTC-5
    { "Indiana (East)",                  300,       0   }, // UTC-5 no DST
    { "Bogota, Lima, Quito",             300,       0   }, // UTC-5 no DST
    { "Atlantic Time (Canada)",          240,     -60   }, // UTC-4
    { "Caracas, La Paz",                 240,       0   }, // UTC-4 no DST
    { "Santiago",                        240,     -60   }, // UTC-4
    { "Newfoundland",                    210,     -60   }, // UTC-3:30
    { "Brasilia",                        180,     -60   }, // UTC-3
    { "Buenos Aires, Georgetown",        180,       0   }, // UTC-3 no DST
    { "Greenland",                       180,     -60   }, // UTC-3
    { "Mid-Atlantic",                    120,     -60   }, // UTC-2
    { "Azores",                           60,     -60   }, // UTC-1
    { "Cape Verde Islands",               60,       0   }, // UTC-1 no DST
    { "Greenwich Mean Time (GMT)",          0,       0   }, // UTC+0 no DST
    { "Dublin, Edinburgh, London",          0,     -60   }, // UTC+0
    { "Casablanca, Monrovia",               0,       0   }, // UTC+0 no DST
    { "Amsterdam, Berlin, Rome",          -60,     -60   }, // UTC+1
    { "Prague, Paris, Madrid",            -60,     -60   }, // UTC+1
    { "West Central Africa",              -60,       0   }, // UTC+1 no DST
    { "Athens, Istanbul, Minsk",         -120,     -60   }, // UTC+2
    { "Bucharest, Cairo, Helsinki",      -120,     -60   }, // UTC+2
    { "Jerusalem",                       -120,     -60   }, // UTC+2
    { "Baghdad, Kuwait, Riyadh",         -180,       0   }, // UTC+3 no DST
    { "Moscow, St. Petersburg",          -180,     -60   }, // UTC+3
    { "Nairobi",                         -180,       0   }, // UTC+3 no DST
    { "Tehran",                          -210,     -60   }, // UTC+3:30
    { "Abu Dhabi, Muscat",               -240,       0   }, // UTC+4 no DST
    { "Baku, Tbilisi, Yerevan",          -240,     -60   }, // UTC+4
    { "Kabul",                           -270,       0   }, // UTC+4:30
    { "Ekaterinburg",                    -300,     -60   }, // UTC+5
    { "Islamabad, Karachi, Tashkent",    -300,       0   }, // UTC+5 no DST
    { "Calcutta, Chennai, Mumbai",       -330,       0   }, // UTC+5:30
    { "Kathmandu",                       -345,       0   }, // UTC+5:45
    { "Almaty, Novosibirsk",             -360,     -60   }, // UTC+6
    { "Astana, Dhaka",                   -360,       0   }, // UTC+6 no DST
    { "Sri Jayawardenepura",             -360,       0   }, // UTC+6 no DST
    { "Rangoon",                         -390,       0   }, // UTC+6:30
    { "Bangkok, Hanoi, Jakarta",         -420,       0   }, // UTC+7 no DST
    { "Krasnoyarsk",                     -420,     -60   }, // UTC+7
    { "Beijing, Chongqing, Hong Kong",   -480,       0   }, // UTC+8 no DST
    { "Kuala Lumpur, Singapore",         -480,       0   }, // UTC+8 no DST
    { "Irkutsk, Ulaan Bataar",           -480,     -60   }, // UTC+8
    { "Perth",                           -480,       0   }, // UTC+8 no DST
    { "Taipei",                          -480,       0   }, // UTC+8 no DST
    { "Osaka, Sapporo, Tokyo",           -540,       0   }, // UTC+9 no DST
    { "Seoul",                           -540,       0   }, // UTC+9 no DST
    { "Yakutsk",                         -540,     -60   }, // UTC+9
    { "Adelaide",                        -570,     -60   }, // UTC+9:30
    { "Darwin",                          -570,       0   }, // UTC+9:30 no DST
    { "Brisbane",                        -600,       0   }, // UTC+10 no DST
    { "Canberra, Melbourne, Sydney",     -600,     -60   }, // UTC+10
    { "Guam, Port Moresby",              -600,       0   }, // UTC+10 no DST
    { "Hobart",                          -600,     -60   }, // UTC+10
    { "Vladivostok",                     -600,     -60   }, // UTC+10
    { "Magadan, Solomon Is.",            -660,     -60   }, // UTC+11
    { "Auckland, Wellington",            -720,     -60   }, // UTC+12
    { "Fiji, Kamchatka",                 -720,       0   }, // UTC+12 no DST
};
static const int TZ_COUNT = sizeof(s_tzTable) / sizeof(s_tzTable[0]);

// Find the closest matching entry index for current bias values
static int TzFindIndex(int stdBias, int dstBias)
{
    // Exact match first
    for (int i = 0; i < TZ_COUNT; ++i)
        if (s_tzTable[i].stdBias == stdBias && s_tzTable[i].dstBias == dstBias)
            return i;
    // Fallback: match stdBias only
    for (int i = 0; i < TZ_COUNT; ++i)
        if (s_tzTable[i].stdBias == stdBias)
            return i;
    return 0;
}
// ============================================================================

// Load working copies from s_eeprom
static void EditLoadFromEeprom()
{
    s_editVideoStd = ReadDW(0x48);
    s_editVideoFlags = ReadDW(0x4C);
    s_editAudioFlags = ReadDW(0x50);
    s_editGameRegion = ReadDW(0x54);
    s_editDvdRegion = ReadDW(0x58);
    s_editParental = ReadDW(0x5C);
    s_editTzIndex = TzFindIndex((int)ReadDW(0x60), (int)ReadDW(0x64));
}

// Cycle an enum index by delta within [0, count-1]
static int CycleEnum(int cur, int count, int delta)
{
    cur += delta;
    if (cur < 0) cur = count - 1;
    if (cur >= count) cur = 0;
    return cur;
}

// Write all working copies back via ExSaveNonVolatileSetting
static bool EditCommit()
{
    bool ok = true;
    LONG r;
    r = ExSaveNonVolatileSetting(XC_VIDEO_STANDARD, REG_DWORD, &s_editVideoStd, 4);
    if (r != 0) ok = false;
    r = ExSaveNonVolatileSetting(XC_VIDEO_FLAGS, REG_DWORD, &s_editVideoFlags, 4);
    if (r != 0) ok = false;
    r = ExSaveNonVolatileSetting(XC_AUDIO_FLAGS, REG_DWORD, &s_editAudioFlags, 4);
    if (r != 0) ok = false;
    r = ExSaveNonVolatileSetting(XC_GAME_REGION, REG_DWORD, &s_editGameRegion, 4);
    if (r != 0) ok = false;
    r = ExSaveNonVolatileSetting(XC_DVD_REGION, REG_DWORD, &s_editDvdRegion, 4);
    if (r != 0) ok = false;
    r = ExSaveNonVolatileSetting(XC_MAX_GAME_RATING, REG_DWORD, &s_editParental, 4);
    if (r != 0) ok = false;
    ULONG tzStd = (ULONG)s_tzTable[s_editTzIndex].stdBias;
    r = ExSaveNonVolatileSetting(XC_TIMEZONE_BIAS, REG_DWORD, &tzStd, 4);
    if (r != 0) ok = false;
    ULONG tzDst = (ULONG)s_tzTable[s_editTzIndex].dstBias;
    r = ExSaveNonVolatileSetting(XC_TIMEZONE_STD_BIAS, REG_DWORD, &tzDst, 4);
    if (r != 0) ok = false;
    // Re-read EEPROM so decoded/hex views reflect new state
    ReadEeprom();
    return ok;
}

// ============================================================================
// Edit view — input handling (called from Tick while s_view == VIEW_EDIT)
// ============================================================================

static void EditHandleInput(WORD cur)
{
    // Confirm overlay eats all input except A=yes / B=no
    if (s_editConfirm)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_A))
        {
            s_editConfirm = false;
            s_editWriteOK = EditCommit();
            s_editWriteDone = true;
        }
        if (EdgeDown(cur, s_prevBtns, BTN_B))
            s_editConfirm = false;
        return;
    }

    // After write result: any button dismisses back to decoded
    if (s_editWriteDone)
    {
        s_view = VIEW_DECODED;
        s_editWriteDone = false;
        return;
    }

    // B = discard, back to decoded
    if (EdgeDown(cur, s_prevBtns, BTN_B))
    {
        s_view = VIEW_DECODED;
        return;
    }

    // A = request confirm
    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        s_editConfirm = true;
        return;
    }

    // Left/Right pages between cards
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
    {
        if (s_editCard > 0) --s_editCard;
        s_editCursor = 0;
        return;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT))
    {
        if (s_editCard < 3) ++s_editCard;
        s_editCursor = 0;
        return;
    }

    // Up/Down moves cursor
    int fieldCount = CARD_FIELD_COUNT[s_editCard];
    if (s_editCard == 0 && s_editVideoStd != 3)
        fieldCount = 6; // hide PAL-60 slot (cursor 6) when standard is not PAL

    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
    {
        if (s_editCursor > 0) --s_editCursor;
        return;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
    {
        if (s_editCursor < fieldCount - 1) ++s_editCursor;
        return;
    }

    // X/Y = cycle value left/right
    int delta = 0;
    if (EdgeDown(cur, s_prevBtns, BTN_X)) delta = -1;
    if (EdgeDown(cur, s_prevBtns, BTN_Y)) delta = 1;
    if (delta == 0) return;

    switch (s_editCard)
    {
    case 0: // VIDEO
        switch (s_editCursor)
        {
        case 0: // Video Standard — enum 1-3
        {
            int idx = (int)s_editVideoStd - 1; // 0-based
            idx = CycleEnum(idx, 3, delta);
            s_editVideoStd = (DWORD)(idx + 1);
            // If leaving PAL, clear PAL-60 flag
            if (s_editVideoStd != 3)
                s_editVideoFlags &= ~0x00000040;
            break;
        }
        case 1: s_editVideoFlags ^= 0x00000001; break; // Widescreen
        case 2: s_editVideoFlags ^= 0x00000002; break; // 720p
        case 3: s_editVideoFlags ^= 0x00000004; break; // 1080i
        case 4: s_editVideoFlags ^= 0x00000008; break; // 480p
        case 5: s_editVideoFlags ^= 0x00000010; break; // Letterbox
        case 6: s_editVideoFlags ^= 0x00000040; break; // PAL 60Hz (only reachable when PAL)
        }
        break;

    case 1: // AUDIO
        switch (s_editCursor)
        {
        case 0: // Audio mode — enum Stereo(0)/Mono(1)/Surround(2)
        {
            DWORD mode = s_editAudioFlags & 0x0000FFFF;
            int idx = (int)mode;
            if (idx > 2) idx = 0;
            idx = CycleEnum(idx, 3, delta);
            s_editAudioFlags = (s_editAudioFlags & 0xFFFF0000) | (DWORD)idx;
            break;
        }
        case 1: s_editAudioFlags ^= 0x00010000; break; // AC3
        case 2: s_editAudioFlags ^= 0x00020000; break; // DTS
        }
        break;

    case 2: // REGION
        switch (s_editCursor)
        {
        case 0: // Game region — enum 0-4 (NA/Japan/RoW/Mfg/All)
        {
            static const DWORD gameRegVals[5] = {
                0x00000001, 0x00000002, 0x00000004,
                0x80000000, 0xFFFFFFFF
            };
            // Find current index
            int idx = 0;
            for (int i = 0; i < 5; ++i)
                if (s_editGameRegion == gameRegVals[i]) { idx = i; break; }
            idx = CycleEnum(idx, 5, delta);
            s_editGameRegion = gameRegVals[idx];
            break;
        }
        case 1: // DVD region — 1-6
        {
            int idx = (int)s_editDvdRegion - 1;
            if (idx < 0 || idx > 5) idx = 0;
            idx = CycleEnum(idx, 6, delta);
            s_editDvdRegion = (DWORD)(idx + 1);
            break;
        }
        case 2: // Parental — 0-6
        {
            int idx = (int)s_editParental;
            if (idx < 0 || idx > 6) idx = 0;
            idx = CycleEnum(idx, 7, delta);
            s_editParental = (DWORD)idx;
            break;
        }
        }
        break;

    case 3: // TIME
        switch (s_editCursor)
        {
        case 0:
            s_editTzIndex = CycleEnum(s_editTzIndex, TZ_COUNT, delta);
            break;
        }
        break;
    }
}

// ============================================================================
// Edit view — render
// ============================================================================

static void RenderEdit(const DiagLogo& logo)
{
    // Build page indicator "< VIDEO >" etc.
    static const char* cardNames[4] = { "VIDEO", "AUDIO", "REGION", "TIME" };
    char title[48];
    SafeCopy(title, sizeof(title), "EEPROM EDIT  [");
    IntToStr(s_editCard + 1, title + StrLen(title), 4);
    SafeAppend(title, sizeof(title), "/4] ");
    SafeAppend(title, sizeof(title), cardNames[s_editCard]);

    const char* hint = s_editConfirm
        ? "[A] Confirm write    [B] Cancel"
        : "[Up/Dn] Select  [X/Y] Change  [A] Write  [B] Discard  [Lft/Rt] Page";

    DrawPageChrome(logo, title, hint);

    float y = CY + 4.f;
    const float ROW = LH + 2.f;
    const float LBL_X = LM2;
    const float VAL_X = LM2 + 220.f;

    // Confirm overlay
    if (s_editConfirm)
    {
        float bx = 80.f, by = 140.f, bw = 480.f, bh = 100.f;
        FillRect(bx, by, bx + bw, by + bh, 0xE0101010);
        HLine(by, bx, bx + bw, COL_YELLOW);
        HLine(by + bh, bx, bx + bw, COL_YELLOW);
        VLine(bx, by, by + bh, COL_YELLOW);
        VLine(bx + bw, by, by + bh, COL_YELLOW);
        DrawText(bx + 16.f, by + 14.f, "WRITE EEPROM SETTINGS?", 1.3f, COL_YELLOW);
        DrawText(bx + 16.f, by + 36.f, "This will update: Video, Audio, Region, Timezone.", 1.1f, COL_WHITE);
        DrawText(bx + 16.f, by + 52.f, "The kernel recalculates the HMAC automatically.", 1.1f, COL_GRAY);
        DrawText(bx + 16.f, by + 70.f, "[A] Yes, write    [B] Cancel", 1.2f, COL_WHITE);
        return;
    }

    // Write result overlay
    if (s_editWriteDone)
    {
        float bx = 80.f, by = 160.f, bw = 480.f, bh = 70.f;
        FillRect(bx, by, bx + bw, by + bh, 0xE0101010);
        DWORD borderCol = s_editWriteOK ? COL_GREEN : COL_RED;
        HLine(by, bx, bx + bw, borderCol);
        HLine(by + bh, bx, bx + bw, borderCol);
        VLine(bx, by, by + bh, borderCol);
        VLine(bx + bw, by, by + bh, borderCol);
        if (s_editWriteOK)
            DrawText(bx + 16.f, by + 20.f, "WRITE OK  - Settings saved.", 1.3f, s_editWriteOK ? COL_GREEN : COL_RED);
        else
            DrawText(bx + 16.f, by + 20.f, "WRITE FAILED - One or more settings not saved.", 1.2f, COL_RED);
        DrawText(bx + 16.f, by + 44.f, "Press any button to return.", 1.1f, COL_GRAY);
        return;
    }

    // Card page nav arrows
    if (s_editCard > 0)
        DrawText(LM2, y, "<  [Left]", 1.1f, COL_GRAY);
    if (s_editCard < 3)
        DrawText(SW - LM - 72.f, y, "[Right]  >", 1.1f, COL_GRAY);
    y += ROW + 2.f;
    HLine(y - 2.f, LM2, SW - LM, COL_BORDER);

    char buf[48];
    char t[16];

    switch (s_editCard)
    {
        // ------------------------------------------------------------------
    case 0: // VIDEO
    {
        // Row 0: Video Standard
        {
            bool sel = (s_editCursor == 0);
            DrawText(LBL_X, y, "Video Standard", 1.2f, sel ? COL_YELLOW : COL_WHITE);
            static const char* vstd[3] = { "NTSC-M (N.America)", "NTSC-J (Japan)", "PAL (Europe/AUS)" };
            int idx = (int)s_editVideoStd - 1;
            if (idx < 0 || idx > 2) idx = 0;
            SafeCopy(buf, sizeof(buf), sel ? "< " : "  ");
            SafeAppend(buf, sizeof(buf), vstd[idx]);
            if (sel) SafeAppend(buf, sizeof(buf), " >");
            DrawText(VAL_X, y, buf, 1.2f, sel ? COL_CYAN : COL_WHITE);
            y += ROW;
        }

        // Helper macro-style: bitmask rows
        struct { const char* lbl; DWORD bit; } vflags[5] = {
            { "Widescreen",  0x00000001 },
            { "HDTV 720p",   0x00000002 },
            { "HDTV 1080i",  0x00000004 },
            { "HDTV 480p",   0x00000008 },
            { "Letterbox",   0x00000010 },
        };
        for (int i = 0; i < 5; ++i)
        {
            bool sel = (s_editCursor == i + 1);
            DrawText(LBL_X + 16.f, y, vflags[i].lbl, 1.15f, sel ? COL_YELLOW : COL_WHITE);
            bool on = (s_editVideoFlags & vflags[i].bit) != 0;
            DrawText(VAL_X, y, on ? "[X] ON" : "[ ] OFF", 1.15f,
                sel ? (on ? COL_GREEN : COL_RED) : (on ? COL_CYAN : COL_GRAY));
            y += ROW;
        }

        // PAL-60 — only shown when PAL is selected
        if (s_editVideoStd == 3)
        {
            bool sel = (s_editCursor == 6);
            DrawText(LBL_X + 16.f, y, "PAL 60Hz", 1.15f, sel ? COL_YELLOW : COL_WHITE);
            bool on = (s_editVideoFlags & 0x00000040) != 0;
            DrawText(VAL_X, y, on ? "[X] ON" : "[ ] OFF", 1.15f,
                sel ? (on ? COL_GREEN : COL_RED) : (on ? COL_CYAN : COL_GRAY));
            y += ROW;
        }
        break;
    }

    // ------------------------------------------------------------------
    case 1: // AUDIO
    {
        // Row 0: Audio mode
        {
            bool sel = (s_editCursor == 0);
            DrawText(LBL_X, y, "Audio Output", 1.2f, sel ? COL_YELLOW : COL_WHITE);
            static const char* amodes[3] = { "Stereo", "Mono", "Surround" };
            DWORD mode = s_editAudioFlags & 0x0000FFFF;
            int idx = (int)mode;
            if (idx > 2) idx = 0;
            SafeCopy(buf, sizeof(buf), sel ? "< " : "  ");
            SafeAppend(buf, sizeof(buf), amodes[idx]);
            if (sel) SafeAppend(buf, sizeof(buf), " >");
            DrawText(VAL_X, y, buf, 1.2f, sel ? COL_CYAN : COL_WHITE);
            y += ROW;
        }

        // Row 1: AC3
        {
            bool sel = (s_editCursor == 1);
            DrawText(LBL_X + 16.f, y, "Dolby Digital (AC3)", 1.15f, sel ? COL_YELLOW : COL_WHITE);
            bool on = (s_editAudioFlags & 0x00010000) != 0;
            DrawText(VAL_X, y, on ? "[X] ON" : "[ ] OFF", 1.15f,
                sel ? (on ? COL_GREEN : COL_RED) : (on ? COL_CYAN : COL_GRAY));
            y += ROW;
        }

        // Row 2: DTS
        {
            bool sel = (s_editCursor == 2);
            DrawText(LBL_X + 16.f, y, "DTS", 1.15f, sel ? COL_YELLOW : COL_WHITE);
            bool on = (s_editAudioFlags & 0x00020000) != 0;
            DrawText(VAL_X, y, on ? "[X] ON" : "[ ] OFF", 1.15f,
                sel ? (on ? COL_GREEN : COL_RED) : (on ? COL_CYAN : COL_GRAY));
            y += ROW;
        }
        break;
    }

    // ------------------------------------------------------------------
    case 2: // REGION
    {
        // Warning banner
        DrawText(LBL_X, y, "! Changing Game Region may affect which games boot !", 1.1f, COL_RED);
        y += ROW;

        // Row 0: Game Region
        {
            bool sel = (s_editCursor == 0);
            DrawText(LBL_X, y, "Game Region", 1.2f, sel ? COL_YELLOW : COL_WHITE);
            static const char* greg[5] = {
                "N. America", "Japan", "Rest of World", "Manufacturing", "ALL (debug)"
            };
            static const DWORD gregVals[5] = {
                0x00000001, 0x00000002, 0x00000004, 0x80000000, 0xFFFFFFFF
            };
            int idx = 0;
            for (int i = 0; i < 5; ++i)
                if (s_editGameRegion == gregVals[i]) { idx = i; break; }
            SafeCopy(buf, sizeof(buf), sel ? "< " : "  ");
            SafeAppend(buf, sizeof(buf), greg[idx]);
            if (sel) SafeAppend(buf, sizeof(buf), " >");
            DrawText(VAL_X, y, buf, 1.2f, sel ? COL_CYAN : COL_WHITE);
            y += ROW;
        }

        // Row 1: DVD Region
        {
            bool sel = (s_editCursor == 1);
            DrawText(LBL_X, y, "DVD Region", 1.2f, sel ? COL_YELLOW : COL_WHITE);
            static const char* dvdr[6] = {
                "1 (USA/CA)", "2 (EUR/JPN)", "3 (SE Asia)",
                "4 (AUS/LAT)", "5 (USSR)", "6 (China)"
            };
            int idx = (int)s_editDvdRegion - 1;
            if (idx < 0 || idx > 5) idx = 0;
            SafeCopy(buf, sizeof(buf), sel ? "< " : "  ");
            SafeAppend(buf, sizeof(buf), dvdr[idx]);
            if (sel) SafeAppend(buf, sizeof(buf), " >");
            DrawText(VAL_X, y, buf, 1.2f, sel ? COL_CYAN : COL_WHITE);
            y += ROW;
        }

        // Row 2: Parental Control
        {
            bool sel = (s_editCursor == 2);
            DrawText(LBL_X, y, "Parental Control", 1.2f, sel ? COL_YELLOW : COL_WHITE);
            static const char* pcr[7] = {
                "DISABLED (All)", "Adults Only", "Mature (M)",
                "Teen (T)", "Everyone (E)", "Kids to Adults", "Early Childhood"
            };
            int idx = (int)s_editParental;
            if (idx < 0 || idx > 6) idx = 0;
            SafeCopy(buf, sizeof(buf), sel ? "< " : "  ");
            SafeAppend(buf, sizeof(buf), pcr[idx]);
            if (sel) SafeAppend(buf, sizeof(buf), " >");
            DrawText(VAL_X, y, buf, 1.2f, sel ? COL_CYAN : COL_WHITE);
            y += ROW;
        }
        break;
    }

    // ------------------------------------------------------------------
    case 3: // TIME
    {
        DrawText(LBL_X, y, "Select your timezone  [X] Prev  [Y] Next", 1.1f, COL_GRAY);
        y += ROW;
        HLine(y - 2.f, LM2, SW - LM, COL_BORDER);
        y += 4.f;

        // Show a context window of 5 entries centred on selected
        const int CTX = 5;
        int half = CTX / 2;
        for (int i = 0; i < CTX; ++i)
        {
            int idx = s_editTzIndex - half + i;
            if (idx < 0 || idx >= TZ_COUNT) { y += ROW; continue; }
            bool sel = (idx == s_editTzIndex);
            if (sel)
                FillRect(LBL_X - 4.f, y - 1.f, SW - LM, y + LH, 0x30204060);

            // Arrow indicator on selected row
            DrawText(LBL_X, y, sel ? ">" : " ", 1.2f, COL_YELLOW);
            DrawText(LBL_X + 14.f, y, s_tzTable[idx].name, 1.15f,
                sel ? COL_CYAN : COL_GRAY);

            // UTC offset on right side for selected row
            if (sel)
            {
                char utcStr[16];
                int display = -s_tzTable[idx].stdBias;
                int hrs = display / 60;
                int mins = display % 60;
                if (mins < 0) mins = -mins;
                SafeCopy(utcStr, sizeof(utcStr), "UTC");
                char t2[8];
                if (hrs >= 0) SafeAppend(utcStr, sizeof(utcStr), "+");
                IntToStr(hrs, t2, sizeof(t2)); SafeAppend(utcStr, sizeof(utcStr), t2);
                if (mins)
                {
                    SafeAppend(utcStr, sizeof(utcStr), ":");
                    if (mins < 10) SafeAppend(utcStr, sizeof(utcStr), "0");
                    IntToStr(mins, t2, sizeof(t2)); SafeAppend(utcStr, sizeof(utcStr), t2);
                }
                DrawText(VAL_X + 60.f, y, utcStr, 1.15f, COL_WHITE);

                // DST note
                if (s_tzTable[idx].dstBias != 0)
                    DrawText(VAL_X + 120.f, y, "DST", 1.1f, COL_GREEN);
                else
                    DrawText(VAL_X + 120.f, y, "No DST", 1.1f, COL_GRAY);
            }
            y += ROW;
        }

        // Scroll position indicator
        char posStr[24];
        SafeCopy(posStr, sizeof(posStr), "  ");
        char t2[8];
        IntToStr(s_editTzIndex + 1, t2, sizeof(t2)); SafeAppend(posStr, sizeof(posStr), t2);
        SafeAppend(posStr, sizeof(posStr), " / ");
        IntToStr(TZ_COUNT, t2, sizeof(t2)); SafeAppend(posStr, sizeof(posStr), t2);
        DrawText(LBL_X, y + 4.f, posStr, 1.05f, COL_GRAY);
        break;
    }
    } // end switch card
}

// ============================================================================
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

    // Global B/Back — but not when edit view owns input
    if (s_view != VIEW_EDIT)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            RequestState(MSTATE_MENU);
            s_prevBtns = cur;
            return;
        }
    }

    if (s_view == VIEW_EDIT)
    {
        // Edit view owns all input
        EditHandleInput(cur);
    }
    else
    {
        // Left/Right only switches views when NOT in edit mode
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT))
            s_view = VIEW_DECODED;

        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
            s_view = VIEW_HEX;

        if (EdgeDown(cur, s_prevBtns, BTN_A))
            SaveBin();

        // Y enters edit view from decoded
        if (s_view == VIEW_DECODED && EdgeDown(cur, s_prevBtns, BTN_Y))
        {
            s_editCard = 0;
            s_editCursor = 0;
            s_editConfirm = false;
            s_editWriteDone = false;
            EditLoadFromEeprom();
            s_view = VIEW_EDIT;
            s_prevBtns = cur;
            return;
        }

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
                if (s_decScroll > 0) --s_decScroll;
            }
            if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
            {
                if (s_decScroll < NUM_FIELDS - DEC_VISIBLE) ++s_decScroll;
            }
        }
    }

    s_prevBtns = cur;

    g_pDevice->BeginScene();
    if (s_view == VIEW_HEX)
        RenderHex(logo);
    else if (s_view == VIEW_DECODED)
        RenderDecoded(logo);
    else
        RenderEdit(logo);
    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}
// ============================================================================
// AutoRun — read EEPROM and report key fields
// ============================================================================

void EepromView_AutoRun(HANDLE hReport)
{
    ReadEeprom();

    char line[128]; DWORD w;
    auto WL = [&](const char* lbl, const char* val)
        {
            StrCopy(line, sizeof(line), lbl);
            StrCat2(line, sizeof(line), line, val);
            StrCat2(line, sizeof(line), line, "\r\n");
            WriteFile(hReport, line, StrLen(line), &w, NULL);
        };

    if (!s_readOK) { WL("EEPROM:       ", "Read failed"); return; }

    // Serial: bytes 0x34-0x3F (12 chars)
    char serial[14];
    for (int i = 0; i < 12; ++i) serial[i] = (char)s_eeprom[0x34 + i];
    serial[12] = '\0';
    WL("Serial:       ", serial);

    // MAC: bytes 0x40-0x45
    {
        char mac[20]; char* mp = mac;
        static const char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 6; ++i)
        {
            BYTE b = s_eeprom[0x40 + i];
            *mp++ = hex[b >> 4]; *mp++ = hex[b & 0xF];
            if (i < 5) *mp++ = ':';
        }
        *mp = '\0';
        WL("MAC:          ", mac);
    }

    // Video standard: bytes 0x58-0x5B
    {
        DWORD vs = (DWORD)s_eeprom[0x58] | ((DWORD)s_eeprom[0x59] << 8)
            | ((DWORD)s_eeprom[0x5A] << 16) | ((DWORD)s_eeprom[0x5B] << 24);
        const char* vsStr = "Unknown";
        if (vs == 0x00400100) vsStr = "NTSC-M";
        else if (vs == 0x00400200) vsStr = "NTSC-J";
        else if (vs == 0x00800300) vsStr = "PAL-I 50Hz";
        else if (vs == 0x00400400) vsStr = "PAL-M 60Hz";
        WL("Video Std:    ", vsStr);
    }

    // Game region: bytes 0x54-0x57
    {
        DWORD gr = (DWORD)s_eeprom[0x54] | ((DWORD)s_eeprom[0x55] << 8)
            | ((DWORD)s_eeprom[0x56] << 16) | ((DWORD)s_eeprom[0x57] << 24);
        char grHex[12];
        IntToHex(gr, 8, grHex, sizeof(grHex));
        char grLine[16]; StrCat2(grLine, sizeof(grLine), "0x", grHex);
        WL("Game Region:  ", grLine);
    }
}