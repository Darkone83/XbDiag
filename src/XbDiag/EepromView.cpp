// EepromView.cpp
// XbDiag — EEPROM Viewer, Editor, Repair, and Restore
//
// VIEWS  (navigate with [Left] / [Right] from Hex or Decoded):
//
//   HEX VIEW
//     16 rows × 16 bytes, offset label + two 8-byte groups + ASCII.
//     [DPad Up/Down]  move cursor row (highlighted, field name shown at bottom).
//     [A]             save raw 256-byte dump to D:\eeprom.bin.
//     [Left]          switch to Decoded View.
//     [B] / [Back]    return to menu.
//
//   DECODED VIEW   — default entry point
//     Scrollable named-field list; two columns: label/hex (left), human value (right).
//     [DPad Up/Down]  scroll field list.
//     [A]             save D:\eeprom.bin.
//     [Right]         switch to Hex View.
//     [Y]             enter Edit View.
//     [X]             enter Repair View.
//     [White]         enter Restore View (only shown in hint if D:\eeprom.bin exists).
//     [B] / [Back]    return to menu.
//
//   EDIT VIEW  ([Y] from Decoded)
//     Four card tabs: VIDEO / AUDIO / REGION / TIME.
//     [Left] / [Right]  cycle cards.
//     [DPad Up/Down]    move cursor within card.
//     [DPad Left/Right] change value for selected field.
//     [A]               save changes (confirm overlay before writing).
//     [B]               discard and return to Decoded View.
//
//   REPAIR VIEW  ([X] from Decoded)
//     Diagnoses 16 EEPROM fields against known-good rules derived from
//     XboxEepromEditor (RE'd kernel 4034 algorithm) and Xbox EEPROM layout docs.
//     Fields are categorised as Repairable or Detect-Only.
//     [A]  repair all corrupt fields (confirm overlay before writing).
//     [B]  return to Decoded View.
//     After repair, [A] re-scans live EEPROM to confirm changes.
//
//   RESTORE VIEW  ([White] from Decoded, only if D:\eeprom.bin present)
//     Reads D:\eeprom.bin, validates both factory and user checksums,
//     then writes all 256 bytes to EEPROM IC via SMBus (byte-by-byte, 10ms delay).
//     A confirm overlay is shown before any write occurs.
//     [A]  confirm restore.
//     [B]  cancel.
//     After restore, [A] re-scans live EEPROM.
//
// EEPROM MAP  (SMBus device 0xA8, 256 bytes)
// Layout from XboxEepromEditor (XboxEepromEditor-master/XboxEepromEditor/Eeprom.cs),
// cross-referenced with XKEEPROM.h (Team Assembly) and PrometheOS EEPROMDATA.
//
//   SECURITY SECTION  (0x00-0x2F, 48 bytes)
//   0x00-0x13  20  HMAC SHA1 Hash      HMAC-SHA1 over decrypted confounder+HDD key+region
//                                      RC4 key derived per-version from kernel constants
//   0x14-0x1B   8  Confounder          RC4-encrypted random nonce
//   0x1C-0x2B  16  HDD Key             RC4-encrypted ATA security key (never touch — bricks HDD)
//   0x2C-0x2F   4  Game Region (enc)   RC4-encrypted region DWORD (NA=1, Japan=2, RoW=4)
//
//   FACTORY SECTION  (0x30-0x5F, 48 bytes)
//   0x30-0x33   4  Factory Checksum    ~Calc(0x34, 0x2C); valid: Calc(0x30, 0x30)==0xFFFFFFFF
//   0x34-0x3F  12  Serial Number       12 ASCII decimal digits
//   0x40-0x45   6  MAC Address         Ethernet MAC (Xbox OUI: 00:50:F2 / 00:0D:3A / 00:12:5A)
//   0x46-0x47   2  (padding)
//   0x48-0x57  16  Online Key          Xbox Live provisioning key
//   0x58-0x5B   4  Video Standard      XC_VIDEO_STANDARD_* (NTSC-M=0x00400100, NTSC-J=0x00400200,
//                                      PAL-I=0x00800300, PAL-M=0x00400400)
//   0x5C-0x5F   4  (padding)
//
//   USER SECTION  (0x60-0xBF, 96 bytes)
//   0x60-0x63   4  User Checksum       ~Calc(0x64, 0x5C); valid: Calc(0x60, 0x60)==0xFFFFFFFF
//   0x64-0x67   4  TZ Bias             UTC offset in minutes (int32, west = positive)
//   0x68-0x6B   4  TZ Std Name         ASCII[4] standard timezone abbreviation
//   0x6C-0x6F   4  TZ DST Name         ASCII[4] daylight timezone abbreviation
//   0x70-0x77   8  (padding / DST info)
//   0x78-0x7B   4  TZ Std Start        transition date (packed Month/Day/DayOfWeek/Hour)
//   0x7C-0x7F   4  TZ DST Start        transition date
//   0x80-0x87   8  (padding)
//   0x88-0x8B   4  TZ Std Bias         standard time offset (int32, minutes)
//   0x8C-0x8F   4  TZ DST Bias         daylight offset (int32, minutes)
//   0x90-0x93   4  Language            1=English 2=Japanese 3=German 4=French
//                                      5=Spanish 6=Italian 7=Korean
//   0x94-0x97   4  Video Flags         XC_VIDEO_FLAGS_* bitmask (valid bits: 0x005F0000)
//                                      0x00010000=widescreen 0x00020000=720p 0x00040000=1080i
//                                      0x00080000=480p 0x00100000=letterbox 0x00400000=PAL60
//   0x98-0x9B   4  Audio Flags         XC_AUDIO_FLAGS_* bitmask (valid bits: 0x00030003)
//                                      0x00000001=mono 0x00000002=surround (0=stereo)
//                                      0x00010000=AC3  0x00020000=DTS
//   0x9C-0x9F   4  Game Rating         ESRB max: 0=disabled 1-8=rating 0xFF=all blocked
//   0xA0-0xA3   4  Parental Passcode   nibble-encoded D-pad sequence (0=none,1=up,2=down,
//                                      3=left,4=right); 0x00000000=disabled
//   0xA4-0xA7   4  Movie Rating        MPAA max: 0=disabled 1-8=rating 0xFF=all blocked
//   0xA8-0xAB   4  Live IP Address     Xbox Live static IP (0=DHCP)
//   0xAC-0xAF   4  Live DNS            Xbox Live DNS server IP
//   0xB0-0xB3   4  Live Gateway        Xbox Live gateway IP
//   0xB4-0xB7   4  Live Subnet         Xbox Live subnet mask
//   0xB8-0xBB   4  (unknown flags)
//   0xBC-0xBF   4  DVD Region          CSS zone (0=none/free 1-6=zone)
//
//   MISC SECTION  (0xC0-0xFF, 64 bytes)
//   0xC0-0xF3  52  (misc/refurb data, 1.6 boxes only)
//   0xF4-0xFF  12  (unknown)

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
    { 0x00, 20, "HMAC SHA1 Hash",   "HMAC SHA1 HASH"   },  // security section
    { 0x14,  8, "Confounder",       "CONFOUNDER"       },
    { 0x1C, 16, "HDD Key",          "HDD KEY"          },
    { 0x2C,  4, "Game Region(enc)", "REGION ENC"       },  // RC4-encrypted
    { 0x30,  4, "Factory Checksum", "FACTORY CHKSUM"   },  // factory section
    { 0x34, 12, "Serial Number",    "SERIAL NUMBER"    },
    { 0x40,  6, "MAC Address",      "MAC ADDRESS"      },
    { 0x48, 16, "Online Key",       "ONLINE KEY"       },
    { 0x58,  4, "Video Standard",   "VIDEO STANDARD"   },
    { 0x60,  4, "User Checksum",    "USER CHKSUM"      },  // user section
    { 0x64,  4, "TZ Bias",          "TZ BIAS"          },
    { 0x68,  4, "TZ Std Name",      "TZ STD NAME"      },
    { 0x6C,  4, "TZ DST Name",      "TZ DST NAME"      },
    { 0x70,  8, "TZ DST Info",      "TZ DST INFO"      },
    { 0x78,  4, "TZ Std Start",     "TZ STD START"     },
    { 0x7C,  4, "TZ DST Start",     "TZ DST START"     },
    { 0x80,  8, "Unknown 0x80",     "UNKNOWN 80"       },
    { 0x88,  4, "TZ Std Bias",      "TZ STD BIAS"      },
    { 0x8C,  4, "TZ DST Bias",      "TZ DST BIAS"      },
    { 0x90,  4, "Language",         "LANGUAGE"         },
    { 0x94,  4, "Video Flags",      "VIDEO FLAGS"      },
    { 0x98,  4, "Audio Flags",      "AUDIO FLAGS"      },
    { 0x9C,  4, "Game Rating",      "GAME RATING"      },
    { 0xA0,  4, "Parental Passcode","PARENTAL CODE"    },
    { 0xA4,  4, "Movie Rating",     "MOVIE RATING"     },
    { 0xA8,  4, "Live IP",          "LIVE IP"          },
    { 0xAC,  4, "Live DNS",         "LIVE DNS"         },
    { 0xB0,  4, "Live Gateway",     "LIVE GATEWAY"     },
    { 0xB4,  4, "Live Subnet",      "LIVE SUBNET"      },
    { 0xB8,  4, "Unknown 0xB8",     "UNKNOWN B8"       },
    { 0xBC,  4, "DVD Region",       "DVD REGION"       },
    { 0xC0, 52, "Misc Data",        "MISC DATA"        },  // misc section
    { 0xF4,  4, "Unknown 0xF4",     "UNKNOWN F4"       },
    { 0xF8,  4, "Unknown 0xF8",     "UNKNOWN F8"       },
    { 0xFC,  4, "Unknown 0xFC",     "UNKNOWN FC"       },
};
static const int NUM_FIELDS = sizeof(s_fields) / sizeof(s_fields[0]);

// ============================================================================
// State
// ============================================================================

enum EepView { VIEW_DECODED = 0, VIEW_HEX, VIEW_EDIT, VIEW_REPAIR };

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

// 0=ok, 1=read failed, 2=decrypt failed, 3=write failed
static int s_editFailReason = 0;
static DWORD s_editVideoStd;   // 1=NTSC-M 2=NTSC-J 3=PAL
static DWORD s_editVideoFlags; // bitmask
static DWORD s_editAudioFlags; // low word=mode, high word=AC3/DTS bits
static DWORD s_editGameRegion; // enum
static DWORD s_editDvdRegion;  // 0=Region Free, 1-6=CSS zone
static DWORD s_editGameRating;   // ESRB max game rating at 0x9C (0=disabled, 1-8=rating, 0xFF=all blocked)
static int   s_editTzIndex;    // index into s_tzTable

// Confirm-write prompt state
static bool  s_editConfirm;    // true = confirm overlay showing
static bool  s_editWriteDone;  // true = write result showing
static bool  s_editWriteOK;

// Card field counts
static const int CARD_FIELD_COUNT[4] = {
    7,  // VIDEO:  VideoStd, Wide, 720p, 1080i, 480p, Letterbox, PAL60 (PAL60 hidden when not PAL)
    3,  // AUDIO:  Mode, AC3, DTS
    2,  // REGION: DvdRegion, GameRating (GameRegion is display-only — encrypted, not editable)
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
// Kernel EEPROM API — declared here so DecodeField and all functions below
// can use them.  The full ReadEeprom()/WriteEeprom() section follows later.
// ============================================================================

extern "C" LONG __stdcall ExQueryNonVolatileSetting(
    ULONG ValueIndex, ULONG* Type, void* Value, ULONG ValueLength, ULONG* ResultLength);

extern "C" LONG __stdcall ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, const void* Value, ULONG ValueLength);

// XC_ index constants (ref: XAPI.H / PrometheOS XKEEPROM)
#define XC_VIDEO_STANDARD       0x04    // kernel setting ID → EEPROM 0x58 (factory section)
#define XC_VIDEO_FLAGS          0x05    // kernel setting ID → EEPROM 0x94 (user section)
#define XC_AUDIO_FLAGS          0x06    // kernel setting ID → EEPROM 0x98 (user section)
#define XC_GAME_REGION          0x07    // kernel setting ID → EEPROM 0x2C (security section, RC4-encrypted)
#define XC_DVD_REGION           0x08    // kernel setting ID → EEPROM 0xBC
#define XC_MAX_GAME_RATING      0x09    // kernel setting ID → EEPROM 0x9C
#define XC_TIMEZONE_BIAS        0x0A    // kernel setting ID → EEPROM 0x64  main UTC offset (int32 minutes west)
#define XC_TIMEZONE_STD_NAME    0x0B    // kernel setting ID → EEPROM 0x68  4-char ASCII std TZ name ("EST\0")
#define XC_TIMEZONE_STD_DATE    0x0C    // kernel setting ID → EEPROM 0x78  DST→STD transition date
#define XC_TIMEZONE_STD_BIAS    0x0D    // kernel setting ID → EEPROM 0x88  standard-time offset (int32, usually 0)
#define XC_TIMEZONE_DST_NAME    0x0E    // kernel setting ID → EEPROM 0x6C  4-char ASCII DST TZ name ("EDT\0")
#define XC_TIMEZONE_DST_DATE    0x0F    // kernel setting ID → EEPROM 0x7C  STD→DST transition date
#define XC_TIMEZONE_DST_BIAS    0x10    // kernel setting ID → EEPROM 0x8C  DST offset (int32, e.g. -60)
#define REG_DWORD               4       // type tag for ExSaveNonVolatileSetting

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
        // Game Region (0x2C) — RC4-encrypted in the raw EEPROM buffer.
        // The raw bytes here are ciphertext, not the region DWORD (0x1/0x2/0x4).
        // Read the decrypted value via the kernel API instead.
    case 0x2C:
    {
        ULONG type = 0, len = 0;
        DWORD gr = 0;
        LONG  r = ExQueryNonVolatileSetting(XC_GAME_REGION, &type, &gr, 4, &len);
        if (r != 0)
        {
            SafeCopy(out, outLen, "(kernel read failed)");
            break;
        }
        if (gr == 0x00000001) SafeCopy(out, outLen, "N. America  (0x00000001)");
        else if (gr == 0x00000002) SafeCopy(out, outLen, "Japan       (0x00000002)");
        else if (gr == 0x00000004) SafeCopy(out, outLen, "Rest of World (0x00000004)");
        else if (gr == 0x80000000) SafeCopy(out, outLen, "Manufacturing (0x80000000)");
        else if (gr == 0xFFFFFFFF) SafeCopy(out, outLen, "All / Debug (0xFFFFFFFF)");
        else
        {
            SafeCopy(out, outLen, "Unknown 0x");
            IntToHex(gr, 8, t, sizeof(t)); SafeAppend(out, outLen, t);
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

        // Online Key at 0x48 — 16-byte Xbox Live provisioning key, show hex
        // (no special decode — any non-zero value just means "provisioned")

        // Video Standard at 0x58 — XC_VIDEO_STANDARD_* DWORD constants
    case 0x58:
    {
        DWORD v = ReadDW(0x58);
        if (v == 0x00400100) SafeCopy(out, outLen, "NTSC-M (N. America)");
        else if (v == 0x00400200) SafeCopy(out, outLen, "NTSC-J (Japan)");
        else if (v == 0x00800300) SafeCopy(out, outLen, "PAL-I (Europe/AUS)");
        else if (v == 0x00400400) SafeCopy(out, outLen, "PAL-M (Brazil)");
        else
        {
            SafeCopy(out, outLen, "0x");
            IntToHex(v, 8, t, sizeof(t)); SafeAppend(out, outLen, t);
        }
        break;
    }

    // Video Flags — XC_VIDEO_FLAGS_* at 0x94 (user section)
    // Bits are in the HIGH word: bit16=widescreen, bit17=720p, bit18=1080i,
    // bit19=480p, bit20=letterbox, bit23=PAL60. Valid mask: 0x005F0000.
    case 0x94:
    {
        DWORD v = ReadDW(0x94);
        bool any = false;
        if (v & 0x00010000) { SafeAppend(out, outLen, "WIDE");    any = true; }
        if (v & 0x00020000) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "720p");  any = true; }
        if (v & 0x00040000) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "1080i"); any = true; }
        if (v & 0x00080000) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "480p");  any = true; }
        if (v & 0x00100000) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "LBOX");  any = true; }
        if (v & 0x00400000) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "PAL60"); any = true; }
        if (!any) SafeCopy(out, outLen, "STANDARD (no HDTV)");
        if (v & ~0x005F0000UL) { SafeAppend(out, outLen, " !UNK"); }
        break;
    }

    // Audio Flags — XC_AUDIO_FLAGS_* at 0x98 (user section)
    // bit0=mono, bit1=surround (0=stereo), bit16=AC3, bit17=DTS.
    case 0x98:
    {
        DWORD v = ReadDW(0x98);
        if (v & 0x01) SafeCopy(out, outLen, "Mono");
        else if (v & 0x02) SafeCopy(out, outLen, "Surround");
        else SafeCopy(out, outLen, "Stereo");
        if (v & 0x00010000) SafeAppend(out, outLen, " +AC3");
        if (v & 0x00020000) SafeAppend(out, outLen, " +DTS");
        if (v & ~0x00030003UL) SafeAppend(out, outLen, " !UNK");
        break;
    }

    // Language — XC_LANGUAGE enum at 0x90 (user section)
    // 0=Neutral/Unknown, 1=English, 2=Japanese, 3=German, 4=French,
    // 5=Spanish, 6=Italian, 7=Korean, 8=Chinese, 9=Portuguese
    case 0x90:
    {
        DWORD v = ReadDW(0x90);
        switch (v)
        {
        case 0: SafeCopy(out, outLen, "Neutral / Unknown"); break;
        case 1: SafeCopy(out, outLen, "English");           break;
        case 2: SafeCopy(out, outLen, "Japanese");          break;
        case 3: SafeCopy(out, outLen, "German");            break;
        case 4: SafeCopy(out, outLen, "French");            break;
        case 5: SafeCopy(out, outLen, "Spanish");           break;
        case 6: SafeCopy(out, outLen, "Italian");           break;
        case 7: SafeCopy(out, outLen, "Korean");            break;
        case 8: SafeCopy(out, outLen, "Chinese");           break;
        case 9: SafeCopy(out, outLen, "Portuguese");        break;
        default: FmtDword(v, out, outLen);                  break;
        }
        break;
    }

    // Parental Passcode — nibble-encoded D-pad sequence at 0xA0
    // Each nibble: 0=none 1=up 2=down 3=left 4=right
    // All-zero = disabled
    case 0xA0:
    {
        DWORD v = ReadDW(0xA0);
        if (v == 0) { SafeCopy(out, outLen, "Disabled"); break; }
        static const char* dirs[5] = { ".", "U", "D", "L", "R" };
        bool any = false;
        for (int ni = 0; ni < 8; ++ni)
        {
            BYTE nib = (BYTE)((v >> (ni * 4)) & 0xF);
            if (nib == 0) break;
            if (any) SafeAppend(out, outLen, " ");
            SafeAppend(out, outLen, nib <= 4 ? dirs[nib] : "?");
            any = true;
        }
        break;
    }

    // Movie Rating — MPAA max at 0xA4 (XboxEepromEditor MovieRating enum)
    // Values: NR=0, NC-17=1, R=2, (3 unused), PG-13=4, PG=5, (6 unused), G=7
    // 0xFF = all movies blocked
    case 0xA4:
    {
        DWORD v = ReadDW(0xA4);
        switch (v)
        {
        case 0:    SafeCopy(out, outLen, "Disabled (All)"); break;
        case 1:    SafeCopy(out, outLen, "NC-17");          break;
        case 2:    SafeCopy(out, outLen, "R");              break;
        case 4:    SafeCopy(out, outLen, "PG-13");          break;
        case 5:    SafeCopy(out, outLen, "PG");             break;
        case 7:    SafeCopy(out, outLen, "G");              break;
        case 0xFF: SafeCopy(out, outLen, "All Blocked");    break;
        default:   FmtDword(v, out, outLen);                break;
        }
        break;
    }

    // Xbox Live network settings (0xA8-0xB7) — stored as network-byte-order IPs.
    // The EEPROM stores the 4 octets in big-endian order:
    //   s_eeprom[off]=a, [off+1]=b, [off+2]=c, [off+3]=d → "a.b.c.d"
    // ReadDW is little-endian so: ReadDW(off)=0xDDCCBBAA → v&0xFF=a, v>>8&0xFF=b ...
    // All-zeros = DHCP / not configured.
    case 0xA8:  // Live IP
    case 0xAC:  // Live DNS
    case 0xB0:  // Live Gateway
    case 0xB4:  // Live Subnet
    {
        DWORD v = ReadDW(f.offset);
        if (v == 0)
        {
            // Subnet mask 0.0.0.0 means DHCP for the IP fields; show clearly
            SafeCopy(out, outLen, f.offset == 0xB4 ? "0.0.0.0" : "DHCP (0.0.0.0)");
            break;
        }
        // Format each octet from the LE DWORD
        // v = (d<<24)|(c<<16)|(b<<8)|a  where a.b.c.d is the dotted form
        BYTE a = (BYTE)(v & 0xFF);
        BYTE b = (BYTE)((v >> 8) & 0xFF);
        BYTE c = (BYTE)((v >> 16) & 0xFF);
        BYTE d = (BYTE)((v >> 24) & 0xFF);
        IntToStr(a, t, sizeof(t)); SafeAppend(out, outLen, t);
        SafeAppend(out, outLen, ".");
        IntToStr(b, t, sizeof(t)); SafeAppend(out, outLen, t);
        SafeAppend(out, outLen, ".");
        IntToStr(c, t, sizeof(t)); SafeAppend(out, outLen, t);
        SafeAppend(out, outLen, ".");
        IntToStr(d, t, sizeof(t)); SafeAppend(out, outLen, t);
        break;
    }
    case 0xBC:
    {
        DWORD v = ReadDW(0xBC);
        if (v == 0x00000000) SafeCopy(out, outLen, "None (Region Free)");
        else if (v == 0x00000001) SafeCopy(out, outLen, "Region 1 (USA/CA)");
        else if (v == 0x00000002) SafeCopy(out, outLen, "Region 2 (EUR/JPN)");
        else if (v == 0x00000003) SafeCopy(out, outLen, "Region 3 (SE Asia)");
        else if (v == 0x00000004) SafeCopy(out, outLen, "Region 4 (AUS/LAT)");
        else if (v == 0x00000005) SafeCopy(out, outLen, "Region 5 (Russia)");
        else if (v == 0x00000006) SafeCopy(out, outLen, "Region 6 (China)");
        else FmtDword(v, out, outLen);
        break;
    }

    // Game rating — at 0x9C per XboxEepromEditor (GameRating enum)
    case 0x9C:
    {
        DWORD v = ReadDW(0x9C);
        if (v == 0)    SafeCopy(out, outLen, "Disabled (All ages)");
        else if (v == 1) SafeCopy(out, outLen, "Adults Only (AO)");
        else if (v == 2) SafeCopy(out, outLen, "Mature (M)");
        else if (v == 3) SafeCopy(out, outLen, "Teen (T)");
        else if (v == 4) SafeCopy(out, outLen, "Everyone 10+ (E10)");
        else if (v == 5) SafeCopy(out, outLen, "Everyone (E)");
        else if (v == 6) SafeCopy(out, outLen, "Early Childhood (EC)");
        else if (v == 7) SafeCopy(out, outLen, "Rating Pending (RP)");
        else if (v == 8) SafeCopy(out, outLen, "No Rating");
        else if (v == 0xFF) SafeCopy(out, outLen, "All Blocked");
        else FmtDword(v, out, outLen);
        break;
    }

    // TZ bias — int32 minutes-west of UTC (0x64); 0x88 = std bias, 0x8C = DST bias
    case 0x64:
    case 0x88:
    case 0x8C:
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

    // TZ name strings — plain ASCII[4] at 0x68 (standard) and 0x6C (DST)
    case 0x68:
    case 0x6C:
    {
        // 4 plain ASCII bytes, null-terminated
        for (int i = 0; i < 4; ++i)
        {
            BYTE b = p[i];
            t[0] = (b >= 0x20 && b < 0x7F) ? (char)b : (b ? '?' : '\0');
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
// EEPROM Security Section Crypto
// Matches PrometheOS XKEEPROM::Decrypt() and EncryptAndCalculateCRC() exactly.
// Used by EditCommit to re-encrypt the security section before writing.
// ============================================================================

static DWORD EV_Rotl32(DWORD x, int n) { return (x << n) | (x >> (32 - n)); }

struct EVSha1 { DWORD H[5]; BYTE B[64]; DWORD bi; DWORD bitLen; };

static void EVSha1Block(EVSha1& s)
{
    static const DWORD k[4] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    DWORD w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = ((DWORD)s.B[i * 4] << 24) | ((DWORD)s.B[i * 4 + 1] << 16) | ((DWORD)s.B[i * 4 + 2] << 8) | s.B[i * 4 + 3];
    for (int i = 16; i < 80; ++i)
        w[i] = EV_Rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    DWORD a = s.H[0], b = s.H[1], c = s.H[2], d = s.H[3], e = s.H[4];
    for (int i = 0; i < 80; ++i)
    {
        DWORD f;
        switch (i / 20)
        {
        case 0: f = (b & c) | ((~b) & d); break;
        case 1: f = b ^ c ^ d;          break;
        case 2: f = (b & c) | (b & d) | (c & d); break;
        default:f = b ^ c ^ d;          break;
        }
        DWORD t = EV_Rotl32(a, 5) + f + e + w[i] + k[i / 20]; e = d; d = c; c = EV_Rotl32(b, 30); b = a; a = t;
    }
    s.H[0] += a; s.H[1] += b; s.H[2] += c; s.H[3] += d; s.H[4] += e; s.bi = 0;
}
static void EVSha1Update(EVSha1& s, const BYTE* d, int len)
{
    for (int i = 0; i < len; ++i) { s.B[s.bi++] = d[i]; s.bitLen += 8; if (s.bi == 64) EVSha1Block(s); }
}
static void EVSha1Final(EVSha1& s, BYTE out[20])
{
    if (s.bi > 55) { s.B[s.bi++] = 0x80; while (s.bi < 64) s.B[s.bi++] = 0; EVSha1Block(s); while (s.bi < 56) s.B[s.bi++] = 0; }
    else { s.B[s.bi++] = 0x80; while (s.bi < 56) s.B[s.bi++] = 0; }
    s.B[56] = s.B[57] = s.B[58] = s.B[59] = 0;
    s.B[60] = (BYTE)(s.bitLen >> 24); s.B[61] = (BYTE)(s.bitLen >> 16); s.B[62] = (BYTE)(s.bitLen >> 8); s.B[63] = (BYTE)s.bitLen;
    EVSha1Block(s);
    for (int i = 0; i < 20; ++i) out[i] = (BYTE)(s.H[i >> 2] >> (8 * (3 - (i & 3))));
}
static void EVXboxHmac(int ver, const BYTE* data, int len, BYTE out[20])
{
    static const DWORD ivs[4][2][5] = {
        {{0x85F9E51A,0xE04613D2,0x6D86A50C,0x77C32E3C,0x4BD717A4},{0x5D7A9C6B,0xE1922BEB,0xB82CCDBC,0x3137AB34,0x486B52B3}},
        {{0x72127625,0x336472B9,0xBE609BEA,0xF55E226B,0x99958DAC},{0x76441D41,0x4DE82659,0x2E8EF85E,0xB256FACA,0xC4FE2DE8}},
        {{0x39B06E79,0xC9BD25E8,0xDBC6B498,0x40B4389D,0x86BBD7ED},{0x9B49BED3,0x84B430FC,0x6B8749CD,0xEBFE5FE5,0xD96E7393}},
        {{0x8058763A,0xF97D4E0E,0x865A9762,0x8A3D920D,0x08995B2C},{0x01075307,0xA2F1E037,0x1186EEEA,0x88DA9992,0x168A5609}},
    };
    EVSha1 s; BYTE mid[20];
    for (int j = 0; j < 5; ++j) s.H[j] = ivs[ver][0][j]; s.bi = 0; s.bitLen = 512;
    EVSha1Update(s, data, len); EVSha1Final(s, mid);
    for (int j = 0; j < 5; ++j) s.H[j] = ivs[ver][1][j]; s.bi = 0; s.bitLen = 512;
    EVSha1Update(s, mid, 20); EVSha1Final(s, out);
}
struct EVRC4 { BYTE S[256]; int x, y; };
static void EVRC4Init(EVRC4& c, const BYTE* key, int klen)
{
    c.x = c.y = 0; for (int i = 0; i < 256; ++i) c.S[i] = (BYTE)i;
    int j = 0;
    for (int i = 0; i < 256; ++i) { j = (key[i % klen] + c.S[i] + j) & 0xFF; BYTE t = c.S[i]; c.S[i] = c.S[j]; c.S[j] = t; }
}
static void EVRC4Crypt(EVRC4& c, BYTE* data, int len)
{
    for (int i = 0; i < len; ++i) {
        c.x = (c.x + 1) & 0xFF; c.y = (c.S[c.x] + c.y) & 0xFF;
        BYTE t = c.S[c.x]; c.S[c.x] = c.S[c.y]; c.S[c.y] = t;
        data[i] ^= c.S[(c.S[c.x] + c.S[c.y]) & 0xFF];
    }
}

// Decrypt security section (0x14-0x2F) in-place. Returns version index or -1.
// Matches PrometheOS Decrypt() — auto-detects version by trying all 4.
static int EVDecryptSecurity(BYTE* buf)
{
    BYTE orig[28];
    for (int i = 0; i < 28; ++i) orig[i] = buf[0x14 + i];
    for (int v = 0; v < 4; ++v)
    {
        BYTE kh[20]; EVXboxHmac(v, buf + 0x00, 20, kh);
        BYTE plain[28]; for (int i = 0; i < 28; ++i) plain[i] = orig[i];
        EVRC4 rc4; EVRC4Init(rc4, kh, 20); EVRC4Crypt(rc4, plain, 28);
        BYTE chk[20]; EVXboxHmac(v, plain, 28, chk);
        bool ok = true; for (int i = 0; i < 20; ++i) if (chk[i] != buf[i]) { ok = false; break; }
        if (ok) { for (int i = 0; i < 28; ++i) buf[0x14 + i] = plain[i]; return v; }
        // restore on mismatch
        for (int i = 0; i < 28; ++i) buf[0x14 + i] = orig[i];
    }
    return -1;
}

// Re-encrypt security section in-place using the given version.
// Matches PrometheOS EncryptAndCalculateCRC():
//   1. Zero HMAC field, compute HMAC over Confounder(8)+HDDKey(20)
//   2. Derive RC4 key from HMAC
//   3. RC4-encrypt Confounder(8) then HDDKey(20) with the same stream
static void EVEncryptSecurity(BYTE* buf, int ver)
{
    // Step 1: zero HMAC, compute new HMAC over plaintext Confounder+HDDKey
    for (int i = 0; i < 20; ++i) buf[i] = 0;
    // EVXboxHmac takes a single contiguous block — Confounder(8)+HDDKey(20) = 28 bytes at 0x14
    // PrometheOS passes them as two separate variadic args (8 bytes, 20 bytes) but
    // XBOX_HMAC_SHA1 feeds them sequentially into SHA1Input — same as one 28-byte block
    EVXboxHmac(ver, buf + 0x14, 28, buf + 0x00);

    // Step 2: derive RC4 key from the new HMAC
    BYTE kh[20]; EVXboxHmac(ver, buf + 0x00, 20, kh);

    // Step 3: encrypt Confounder(8) then HDDKey(20) — same RC4 stream
    EVRC4 rc4; EVRC4Init(rc4, kh, 20);
    EVRC4Crypt(rc4, buf + 0x14, 8);   // Confounder
    EVRC4Crypt(rc4, buf + 0x1C, 20);  // HDDKey (16 bytes) + XBERegion (4 bytes) = 20 bytes
}

// ============================================================================
// EEPROM read
// ============================================================================

static void ReadEeprom()
{
    // Matches PrometheOS XKEEPROM::ReadFromXBOX():
    //   ExQueryNonVolatileSetting(0xFFFF, &type, &m_EEPROMData, 256, &size)
    // This is the same source buffer that save() reads from before writing.
    ULONG type = 0, resultLen = 0;
    LONG status = ExQueryNonVolatileSetting(0xFFFF, &type, s_eeprom, 256, &resultLen);
    s_readOK = (status == 0 && resultLen >= 16);
    if (!s_readOK)
        for (int i = 0; i < 256; ++i) s_eeprom[i] = 0;
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

// Forward declarations — repair functions defined after Tick
static void RepairBuildDiag();
static void RepairHandleInput(WORD cur, WORD prev, const DiagLogo& logo);
static void RenderRepair(const DiagLogo& logo);
static bool s_repRan;
static bool s_repConfirm;
static bool s_binExists = false;
static bool s_restoreDone = false;
static bool s_restoreOK = false;
static bool s_restoreConfirm = false;

void EepromView_OnEnter()
{
    s_prevBtns = GetButtons();  // seed to prevent held buttons from firing as edges
    s_view = VIEW_DECODED;
    s_hexCurRow = 0;
    s_decScroll = 0;
    s_saveDone = false;
    s_saveOK = false;
    s_loaded = false;
    s_repRan = false;
    s_repConfirm = false;
    s_restoreDone = false;
    s_restoreOK = false;
    s_restoreConfirm = false;
    {
        HANDLE hf = CreateFileA("D:\\eeprom.bin", GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        s_binExists = (hf != INVALID_HANDLE_VALUE);
        if (s_binExists) CloseHandle(hf);
    }
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
        ? (s_saveOK ? "[A] Saved OK    [Left] Decoded    [B] Back"
            : "[A] Save failed [Left] Decoded    [B] Back")
        : "[A] Save eeprom.bin    [Left] Decoded    [B] Back";

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
        ? (s_saveOK ? "[A] Save OK    [Right] Hex    [Y] Edit    [X] Repair    [B] Back"
            : "[A] Save FAIL  [Right] Hex    [Y] Edit    [X] Repair    [B] Back")
        : "[A] Save eeprom.bin    [Right] Hex    [Y] Edit    [X] Repair    [B] Back";
    static char s_decHint[128];
    if (s_binExists)
    {
        StrCopy(s_decHint, sizeof(s_decHint), hint);
        StrCat2(s_decHint, sizeof(s_decHint), s_decHint, "    [White] Restore");
        hint = s_decHint;
    }

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
// Timezone table — each entry carries the full 44-byte raw EEPROM block for
// offsets 0x64-0x8F, sourced directly from XboxEepromEditor _timeZoneMappings.
// This is the authoritative data; writing these 44 bytes produces a fully
// consistent timezone block that the Xbox kernel recognises.
//
// Block layout (44 bytes):
//   [0x00-0x03]  0x64  TZ Bias (int32 LE, minutes west of UTC)
//   [0x04-0x07]  0x68  Std TZ Name (4 ASCII bytes)
//   [0x08-0x0B]  0x6C  DST TZ Name (4 ASCII bytes)
//   [0x0C-0x13]  0x70  padding (8 bytes)
//   [0x14-0x17]  0x78  Std transition date (packed Month/Day/DayOfWeek/Hour)
//   [0x18-0x1B]  0x7C  DST transition date
//   [0x1C-0x23]  0x80  padding (8 bytes)
//   [0x24-0x27]  0x88  Std Bias (int32 LE, minutes)
//   [0x28-0x2B]  0x8C  DST Bias (int32 LE, minutes)
// ============================================================================

struct TzEntry
{
    const char* name;
    BYTE        raw[44];  // full 44-byte block for 0x64-0x8F
};

// Bias helpers for TzFindIndex — read from raw block
static int TzRawBias(const TzEntry& e) // stdBias at raw[0]
{
    return (int)((DWORD)e.raw[0] | ((DWORD)e.raw[1] << 8) | ((DWORD)e.raw[2] << 16) | ((DWORD)e.raw[3] << 24));
}
static int TzRawDstBias(const TzEntry& e) // dstBias at raw[40]
{
    return (int)((DWORD)e.raw[40] | ((DWORD)e.raw[41] << 8) | ((DWORD)e.raw[42] << 16) | ((DWORD)e.raw[43] << 24));
}

static const TzEntry s_tzTable[] =
{
    { "Samoa",
      {0x94,0x02,0x00,0x00, 0x4E,0x53,0x54,0x00, 0x4E,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Hawaii",
      {0x58,0x02,0x00,0x00, 0x48,0x53,0x54,0x00, 0x48,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Alaska",
      {0x1C,0x02,0x00,0x00, 0x59,0x53,0x54,0x00, 0x59,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x04,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Pacific Time (US & Canada)",
      {0xE0,0x01,0x00,0x00, 0x50,0x53,0x54,0x00, 0x50,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x04,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Mountain Time (US & Canada)",
      {0xA4,0x01,0x00,0x00, 0x4D,0x53,0x54,0x00, 0x4D,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x04,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Arizona",
      {0xA4,0x01,0x00,0x00, 0x4D,0x53,0x54,0x00, 0x4D,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Saskatchewan",
      {0x68,0x01,0x00,0x00, 0x43,0x43,0x53,0x54, 0x43,0x43,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Mexico City, Tegucigalpa",
      {0x68,0x01,0x00,0x00, 0x4D,0x53,0x54,0x00, 0x4D,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x04,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Central Time (US & Canada)",
      {0x68,0x01,0x00,0x00, 0x43,0x53,0x54,0x00, 0x43,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x04,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Central America",
      {0x68,0x01,0x00,0x00, 0x43,0x41,0x53,0x54, 0x43,0x41,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Indiana (East)",
      {0x2C,0x01,0x00,0x00, 0x45,0x53,0x54,0x00, 0x45,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Eastern Time (US & Canada)",
      {0x2C,0x01,0x00,0x00, 0x45,0x53,0x54,0x00, 0x45,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x04,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Bogota, Lima, Quito",
      {0x2C,0x01,0x00,0x00, 0x53,0x50,0x53,0x54, 0x53,0x50,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Santiago",
      {0xF0,0x00,0x00,0x00, 0x50,0x53,0x53,0x54, 0x50,0x53,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x03,0x02,0x06,0x00, 0x0A,0x02,0x06,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Caracas, La Paz",
      {0xF0,0x00,0x00,0x00, 0x53,0x57,0x53,0x54, 0x53,0x57,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Atlantic Time (Canada)",
      {0xF0,0x00,0x00,0x00, 0x41,0x53,0x54,0x00, 0x41,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x04,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Greenland",
      {0xB4,0x00,0x00,0x00, 0x47,0x53,0x54,0x00, 0x47,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Buenos Aires, Georgetown",
      {0xB4,0x00,0x00,0x00, 0x53,0x45,0x53,0x54, 0x53,0x45,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Brasilia",
      {0xB4,0x00,0x00,0x00, 0x45,0x53,0x52,0x54, 0x45,0x53,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x02,0x02,0x00,0x02, 0x0A,0x03,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Newfoundland",
      {0xD2,0x00,0x00,0x00, 0x4E,0x53,0x54,0x00, 0x4E,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x04,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Mid-Atlantic",
      {0x78,0x00,0x00,0x00, 0x4D,0x41,0x53,0x54, 0x4D,0x41,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x09,0x05,0x00,0x02, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Cape Verde Islands",
      {0x3C,0x00,0x00,0x00, 0x57,0x41,0x54,0x00, 0x57,0x41,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Azores",
      {0x3C,0x00,0x00,0x00, 0x41,0x53,0x54,0x00, 0x41,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Casablanca, Monrovia",
      {0x00,0x00,0x00,0x00, 0x47,0x53,0x54,0x00, 0x47,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Dublin, Edinburgh, London",
      {0x00,0x00,0x00,0x00, 0x47,0x4D,0x54,0x00, 0x42,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02, 0x03,0x05,0x00,0x01, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Amsterdam, Berlin, Rome",
      {0xC4,0xFF,0xFF,0xFF, 0x57,0x45,0x53,0x54, 0x57,0x45,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Belgrade, Bratislava",
      {0xC4,0xFF,0xFF,0xFF, 0x43,0x45,0x53,0x54, 0x43,0x45,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Paris, Madrid",
      {0xC4,0xFF,0xFF,0xFF, 0x52,0x53,0x54,0x00, 0x52,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "West Central Africa",
      {0xC4,0xFF,0xFF,0xFF, 0x57,0x41,0x53,0x54, 0x57,0x41,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Athens, Istanbul, Minsk",
      {0x88,0xFF,0xFF,0xFF, 0x47,0x54,0x53,0x54, 0x47,0x54,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Cairo",
      {0x88,0xFF,0xFF,0xFF, 0x45,0x53,0x54,0x00, 0x45,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x09,0x05,0x03,0x02, 0x05,0x01,0x05,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Helsinki, Kyiv",
      {0x88,0xFF,0xFF,0xFF, 0x46,0x4C,0x53,0x54, 0x46,0x4C,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x04, 0x03,0x05,0x00,0x03, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Jerusalem",
      {0x88,0xFF,0xFF,0xFF, 0x4A,0x53,0x54,0x00, 0x4A,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Baghdad",
      {0x4C,0xFF,0xFF,0xFF, 0x41,0x53,0x54,0x00, 0x41,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x01,0x00,0x04, 0x04,0x01,0x00,0x03, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Kuwait, Riyadh",
      {0x4C,0xFF,0xFF,0xFF, 0x41,0x53,0x54,0x00, 0x41,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Moscow, St. Petersburg",
      {0x4C,0xFF,0xFF,0xFF, 0x52,0x53,0x54,0x00, 0x52,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Nairobi",
      {0x4C,0xFF,0xFF,0xFF, 0x45,0x41,0x53,0x54, 0x45,0x41,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Tehran",
      {0x2E,0xFF,0xFF,0xFF, 0x49,0x53,0x54,0x00, 0x49,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x09,0x04,0x02,0x02, 0x03,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Abu Dhabi, Muscat",
      {0x10,0xFF,0xFF,0xFF, 0x41,0x53,0x54,0x00, 0x41,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Baku, Tbilisi, Yerevan",
      {0x10,0xFF,0xFF,0xFF, 0x43,0x53,0x54,0x00, 0x43,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Kabul",
      {0xF2,0xFE,0xFF,0xFF, 0x41,0x53,0x54,0x00, 0x41,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Ekaterinburg",
      {0xD4,0xFE,0xFF,0xFF, 0x45,0x53,0x54,0x00, 0x45,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Islamabad, Karachi, Tashkent",
      {0xD4,0xFE,0xFF,0xFF, 0x57,0x41,0x53,0x54, 0x57,0x41,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Calcutta, Chennai, Mumbai",
      {0xB6,0xFE,0xFF,0xFF, 0x49,0x53,0x54,0x00, 0x49,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Kathmandu",
      {0xA7,0xFE,0xFF,0xFF, 0x4E,0x53,0x54,0x00, 0x4E,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Almaty, Novosibirsk",
      {0x98,0xFE,0xFF,0xFF, 0x4E,0x43,0x53,0x54, 0x4E,0x43,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Astana, Dhaka",
      {0x98,0xFE,0xFF,0xFF, 0x43,0x41,0x53,0x54, 0x43,0x41,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Sri Lanka",
      {0x98,0xFE,0xFF,0xFF, 0x53,0x52,0x53,0x54, 0x53,0x52,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Rangoon",
      {0x7A,0xFE,0xFF,0xFF, 0x4D,0x53,0x54,0x00, 0x4D,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Bangkok, Hanoi, Jakarta",
      {0x5C,0xFE,0xFF,0xFF, 0x53,0x41,0x53,0x54, 0x53,0x41,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Krasnoyarsk",
      {0x5C,0xFE,0xFF,0xFF, 0x4E,0x41,0x53,0x54, 0x4E,0x41,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Beijing, Chongqing, Hong Kong",
      {0x20,0xFE,0xFF,0xFF, 0x43,0x53,0x54,0x00, 0x43,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Irkutsk, Ulaan Bataar",
      {0x20,0xFE,0xFF,0xFF, 0x4E,0x45,0x53,0x54, 0x4E,0x45,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Perth",
      {0x20,0xFE,0xFF,0xFF, 0x41,0x57,0x53,0x54, 0x41,0x57,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Kuala Lumpur, Singapore",
      {0x20,0xFE,0xFF,0xFF, 0x4D,0x50,0x53,0x54, 0x4D,0x50,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Taipei",
      {0x20,0xFE,0xFF,0xFF, 0x54,0x53,0x54,0x00, 0x54,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Seoul",
      {0xE4,0xFD,0xFF,0xFF, 0x4B,0x53,0x54,0x00, 0x4B,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Osaka, Sapporo, Tokyo",
      {0xE4,0xFD,0xFF,0xFF, 0x54,0x53,0x54,0x00, 0x54,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Yakutsk",
      {0xE4,0xFD,0xFF,0xFF, 0x59,0x53,0x54,0x00, 0x59,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Adelaide",
      {0xC6,0xFD,0xFF,0xFF, 0x41,0x43,0x53,0x54, 0x41,0x43,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x03,0x05,0x00,0x02, 0x0A,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Darwin",
      {0xC6,0xFD,0xFF,0xFF, 0x41,0x43,0x53,0x54, 0x41,0x43,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Brisbane",
      {0xA8,0xFD,0xFF,0xFF, 0x41,0x45,0x53,0x54, 0x41,0x45,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Canberra, Melbourne, Sydney",
      {0xA8,0xFD,0xFF,0xFF, 0x41,0x45,0x53,0x54, 0x41,0x45,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x03,0x05,0x00,0x02, 0x0A,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Guam, Port Moresby",
      {0xA8,0xFD,0xFF,0xFF, 0x57,0x50,0x53,0x54, 0x57,0x50,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Hobart",
      {0xA8,0xFD,0xFF,0xFF, 0x54,0x53,0x54,0x00, 0x54,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x03,0x05,0x00,0x02, 0x0A,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Vladivostok",
      {0xA8,0xFD,0xFF,0xFF, 0x56,0x53,0x54,0x00, 0x56,0x44,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x03, 0x03,0x05,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Magadan, Solomon Islands",
      {0x6C,0xFD,0xFF,0xFF, 0x43,0x50,0x53,0x54, 0x43,0x50,0x53,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
    { "Auckland, Wellington",
      {0x30,0xFD,0xFF,0xFF, 0x4E,0x5A,0x53,0x54, 0x4E,0x5A,0x44,0x54, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x03,0x03,0x00,0x02, 0x0A,0x01,0x00,0x02, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xC4,0xFF,0xFF,0xFF} },
    { "Fiji, Kamchatka",
      {0x30,0xFD,0xFF,0xFF, 0x46,0x53,0x54,0x00, 0x46,0x53,0x54,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00} },
};
static const int TZ_COUNT = sizeof(s_tzTable) / sizeof(s_tzTable[0]);

// Find index by matching the full 44-byte raw block against current EEPROM data.
// Falls back to bias-only match if no exact match found.
static int TzFindIndex(int stdBias, int dstBias)
{
    // Try exact raw block match first
    const BYTE* cur = s_eeprom + 0x64;
    for (int i = 0; i < TZ_COUNT; ++i)
    {
        bool match = true;
        for (int j = 0; j < 44; ++j)
            if (s_tzTable[i].raw[j] != cur[j]) { match = false; break; }
        if (match) return i;
    }
    // Fallback: match on bias values
    for (int i = 0; i < TZ_COUNT; ++i)
        if (TzRawBias(s_tzTable[i]) == stdBias && TzRawDstBias(s_tzTable[i]) == dstBias)
            return i;
    for (int i = 0; i < TZ_COUNT; ++i)
        if (TzRawBias(s_tzTable[i]) == stdBias)
            return i;
    return 0;
}
// ============================================================================

// Load working copies from s_eeprom
static void EditLoadFromEeprom()
{
    // Video Standard at 0x58 — stored as XC_VIDEO_STANDARD_* DWORD constants.
    // Internally we use enum 1=NTSC-M, 2=NTSC-J, 3=PAL-I, 4=PAL-M so the Edit
    // view can cycle with simple index arithmetic.
    {
        DWORD vs = ReadDW(0x58);
        if (vs == 0x00400200) s_editVideoStd = 2; // NTSC-J
        else if (vs == 0x00800300) s_editVideoStd = 3; // PAL-I
        else if (vs == 0x00400400) s_editVideoStd = 4; // PAL-M (Brazil)
        else                       s_editVideoStd = 1; // NTSC-M (default / unknown)
    }

    s_editVideoFlags = ReadDW(0x94);  // video flags bitmask, stored as-is (bits 16-23)

    // Audio flags at 0x98 — EEPROM format: bit0=mono, bit1=surround (0=stereo),
    // bit16=AC3, bit17=DTS.  Internal Edit format: low word = mode (0/1/2),
    // bit16=AC3, bit17=DTS (same positions as EEPROM — only mode bits differ).
    {
        DWORD raw = ReadDW(0x98);
        DWORD mode;
        if (raw & 0x01) mode = 1; // mono
        else if (raw & 0x02) mode = 2; // surround
        else                 mode = 0; // stereo
        s_editAudioFlags = mode | (raw & 0x00030000); // AC3/DTS pass through
    }

    s_editDvdRegion = ReadDW(0xBC);  // DVD region at 0xBC
    s_editGameRating = ReadDW(0x9C);  // max game rating at 0x9C
    s_editTzIndex = TzFindIndex((int)ReadDW(0x64), (int)ReadDW(0x8C));
    // Game region (0x2C) is not loaded here — it is in the encrypted security
    // section and editing it requires per-console key re-encryption. Displayed
    // read-only on the Region card via a direct ExQueryNonVolatileSetting call.
}

// Cycle an enum index by delta within [0, count-1]
static int CycleEnum(int cur, int count, int delta)
{
    cur += delta;
    if (cur < 0) cur = count - 1;
    if (cur >= count) cur = 0;
    return cur;
}

// Forward declaration — defined in the repair section below.
static DWORD EepCalcChecksum(const BYTE* data, int offset, int size);
static bool  EepSMBusWriteDW(int offset, DWORD val);

// Write a DWORD little-endian into a raw buffer at offset
static void BufWriteDW(BYTE* buf, int off, DWORD val)
{
    buf[off + 0] = (BYTE)(val);
    buf[off + 1] = (BYTE)(val >> 8);
    buf[off + 2] = (BYTE)(val >> 16);
    buf[off + 3] = (BYTE)(val >> 24);
}

// Read a DWORD little-endian from a raw buffer at offset
static DWORD BufReadDW(const BYTE* buf, int off)
{
    return (DWORD)buf[off]
        | ((DWORD)buf[off + 1] << 8)
        | ((DWORD)buf[off + 2] << 16)
        | ((DWORD)buf[off + 3] << 24);
}

// Recalculate and write both checksums into a 256-byte EEPROM buffer.
// Matches PrometheOS CalculateChecksum2() + CalculateChecksum3():
//   Checksum2 (0x30): ~Calc(buf+0x34, 0x2C) — serial/MAC/online key/video std
//   Checksum3 (0x60): ~Calc(buf+0x64, 0x5C) — tz/language/flags/ratings/dvd
static void EepRecalcChecksums(BYTE* buf)
{
    DWORD cs2 = ~EepCalcChecksum(buf, 0x34, 0x2C);
    BufWriteDW(buf, 0x30, cs2);
    DWORD cs3 = ~EepCalcChecksum(buf, 0x64, 0x5C);
    BufWriteDW(buf, 0x60, cs3);
}

// Write all working copies back to EEPROM.
// Exact port of PrometheOS xboxConfig::save() + XKEEPROM:
//   ReadFromXBOX()         = ExQueryNonVolatileSetting(0xFFFF, ...)
//   SetXBERegionVal()      = Decrypt() + modify + EncryptAndCalculateCRC()
//   SetVideoFlagsVal()     = direct write + CalculateChecksum3
//   SetAudioFlagsVal()     = direct write + CalculateChecksum3
//   SetVideoStandardVal()  = direct write + CalculateChecksum2
//   SetDVDRegionVal()      = direct write + CalculateChecksum3
//   WriteToXBOX()          = ExSaveNonVolatileSetting(0xFFFF, 3, buf, 256)
static bool EditCommit()
{
    s_editFailReason = 0;

    // ReadFromXBOX — read raw encrypted bytes directly from chip via SMBus.
    // We do NOT use ExQueryNonVolatileSetting(0xFFFF) here because the kernel
    // returns a partially-processed buffer where 0x00-0x13 (HMAC) may be zeroed
    // after internal decryption, causing EVDecryptSecurity to false-positive and
    // EVEncryptSecurity to produce an invalid HMAC that survives the write call
    // but fails verification on next boot.
    // SMBus gives us the true raw chip bytes — the same source RestoreFromFile uses.
    BYTE buf[256];
    {
        for (int i = 0; i < 256; ++i)
        {
            BYTE b = 0;
            if (!SMBusRead(SMBADDR_EEPROM, (BYTE)i, b))
            {
                s_editFailReason = 1; return false;
            }
            buf[i] = b;
        }
    }

    // SetXBERegionVal equivalent — Decrypt, then EncryptAndCalculateCRC.
    // PrometheOS always calls this even when region is unchanged.
    // This is what regenerates a valid HMAC before WriteToXBOX.
    // If decrypt fails (kernel already decrypted the buffer), skip decrypt
    // but still re-encrypt to generate the required HMAC.
    int ver = EVDecryptSecurity(buf);
    if (ver < 0)
    {
        s_editFailReason = 2; return false;
    }

    // SetVideoStandardVal — direct write + CalculateChecksum2
    {
        static const DWORD vsMap[5] = {
            0x00400100, 0x00400100, 0x00400200, 0x00800300, 0x00400400
        };
        DWORD vs = (s_editVideoStd >= 1 && s_editVideoStd <= 4)
            ? vsMap[s_editVideoStd] : 0x00400100;
        BufWriteDW(buf, 0x58, vs);
    }

    // SetVideoFlagsVal — direct write + CalculateChecksum3
    BufWriteDW(buf, 0x94, s_editVideoFlags & 0x005F0000UL);

    // SetAudioFlagsVal — direct write + CalculateChecksum3
    {
        DWORD mode = s_editAudioFlags & 0x0000FFFFUL;
        DWORD raw = (mode == 1) ? 0x01 : (mode == 2) ? 0x02 : 0x00;
        raw |= (s_editAudioFlags & 0x00030000UL);
        raw &= 0x00030003UL;
        BufWriteDW(buf, 0x98, raw);
    }

    // SetDVDRegionVal — direct write + CalculateChecksum3
    BufWriteDW(buf, 0xBC, s_editDvdRegion);

    // Game rating — direct write + CalculateChecksum3
    BufWriteDW(buf, 0x9C, s_editGameRating);

    // Timezone — full 44-byte block + CalculateChecksum3
    for (int i = 0; i < 44; ++i)
        buf[0x64 + i] = s_tzTable[s_editTzIndex].raw[i];

    // CalculateChecksum2 + CalculateChecksum3
    EepRecalcChecksums(buf);

    // EncryptAndCalculateCRC — regenerate HMAC, RC4 re-encrypt security section,
    // recalculate both checksums. This is what makes the kernel accept the write.
    EVEncryptSecurity(buf, ver);
    EepRecalcChecksums(buf);

    // WriteToXBOX — ExSaveNonVolatileSetting(0xFFFF, 3, buf, 256)
    // Updates the kernel's internal cache so the running system sees new values.
    {
        ULONG type = 3;
        if (ExSaveNonVolatileSetting(0xFFFF, type, buf, 256) != 0)
        {
            s_editFailReason = 3; return false;
        }
    }

    // Also write directly to the physical EEPROM chip via SMBus.
    // ExSaveNonVolatileSetting updates the kernel cache but may not flush to
    // hardware outside of the dashboard context. SMBus write ensures persistence
    // across reboots — the same path RestoreFromFile uses successfully.
    for (int i = 0; i < 256; ++i)
    {
        if (!SMBusWrite(SMBADDR_EEPROM, (BYTE)i, buf[i]))
        {
            s_editFailReason = 3; return false;
        }
        Sleep(10);
    }

    // Update s_eeprom[] from what we wrote so decoded/hex views reflect new state
    for (int i = 0; i < 256; ++i) s_eeprom[i] = buf[i];
    EditLoadFromEeprom();
    return true;
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

    // After write result: wait for a button press before returning to decoded
    if (s_editWriteDone)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_A) || EdgeDown(cur, s_prevBtns, BTN_B)
            || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            s_view = VIEW_DECODED;
            s_editWriteDone = false;
        }
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
    if (s_editCard == 0 && s_editVideoStd != 3 && s_editVideoStd != 4)
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
        case 0: // Video Standard — enum 1-4
        {
            int idx = (int)s_editVideoStd - 1; // 0-based
            idx = CycleEnum(idx, 4, delta);
            s_editVideoStd = (DWORD)(idx + 1);
            // If leaving PAL, clear PAL-60 flag
            if (s_editVideoStd != 3 && s_editVideoStd != 4)
                s_editVideoFlags &= ~0x00400000u;
            break;
        }
        case 1: s_editVideoFlags ^= 0x00010000; break; // Widescreen
        case 2: s_editVideoFlags ^= 0x00020000; break; // 720p
        case 3: s_editVideoFlags ^= 0x00040000; break; // 1080i
        case 4: s_editVideoFlags ^= 0x00080000; break; // 480p
        case 5: s_editVideoFlags ^= 0x00100000; break; // Letterbox
        case 6: s_editVideoFlags ^= 0x00400000; break; // PAL 60Hz (only reachable when PAL)
        }
        break;

    case 1: // AUDIO
        switch (s_editCursor)
        {
        case 0: // Audio mode — Stereo(0) / Mono(1) / Surround(2)
        {
            DWORD mode = s_editAudioFlags & 0x0000FFFF;
            int idx = (int)mode;
            if (idx > 2) idx = 0;
            idx = CycleEnum(idx, 3, delta);
            s_editAudioFlags = (s_editAudioFlags & 0xFFFF0000) | (DWORD)idx;
            break;
        }
        case 1: s_editAudioFlags ^= 0x00010000; break; // AC3  (bit16)
        case 2: s_editAudioFlags ^= 0x00020000; break; // DTS  (bit17)
        }
        break;

    case 2: // REGION
        switch (s_editCursor)
        {
        case 0: // DVD region — 0=Region Free, 1-6=CSS zones  (7 options)
        {
            int idx = (int)s_editDvdRegion;
            if (idx < 0 || idx > 6) idx = 0;
            idx = CycleEnum(idx, 7, delta);
            s_editDvdRegion = (DWORD)idx;
            break;
        }
        case 1: // Game Rating at 0x9C
        {
            int idx = (int)s_editGameRating;
            if (idx < 0 || idx > 6) idx = 0;
            idx = CycleEnum(idx, 7, delta);
            s_editGameRating = (DWORD)idx;
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
        float bx = 80.f, by = 130.f, bw = 480.f, bh = 130.f;
        FillRect(bx, by, bx + bw, by + bh, 0xE0101010);
        HLine(by, bx, bx + bw, COL_YELLOW);
        HLine(by + bh, bx, bx + bw, COL_YELLOW);
        VLine(bx, by, by + bh, COL_YELLOW);
        VLine(bx + bw, by, by + bh, COL_YELLOW);
        DrawText(bx + 16.f, by + 14.f, "WRITE EEPROM SETTINGS?", 1.3f, COL_YELLOW);
        HLine(by + 34.f, bx + 1.f, bx + bw - 1.f, 0x44FFFF00);
        DrawText(bx + 16.f, by + 44.f, "This will update: Video, Audio, Region, Timezone.", 1.1f, COL_WHITE);
        DrawText(bx + 16.f, by + 64.f, "Security section will be re-encrypted correctly.", 1.1f, COL_GRAY);
        HLine(by + 84.f, bx + 1.f, bx + bw - 1.f, 0x33FFFFFF);
        DrawText(bx + 16.f, by + 96.f, "[A] Yes, write    [B] Cancel", 1.2f, COL_WHITE);
        return;
    }

    // Write result overlay
    if (s_editWriteDone)
    {
        float bx = 80.f, by = 150.f, bw = 480.f, bh = 110.f;
        FillRect(bx, by, bx + bw, by + bh, 0xE0101010);
        DWORD borderCol = s_editWriteOK ? COL_GREEN : COL_RED;
        HLine(by, bx, bx + bw, borderCol);
        HLine(by + bh, bx, bx + bw, borderCol);
        VLine(bx, by, by + bh, borderCol);
        VLine(bx + bw, by, by + bh, borderCol);
        if (s_editWriteOK)
        {
            DrawText(bx + 16.f, by + 16.f, "WRITE OK  - Settings saved.", 1.3f, COL_GREEN);
            HLine(by + 38.f, bx + 1.f, bx + bw - 1.f, 0x3300FF00);
            DrawText(bx + 16.f, by + 48.f, "Reboot required for changes to take effect.", 1.1f, COL_YELLOW);
        }
        else
        {
            const char* failMsg =
                (s_editFailReason == 1) ? "FAILED - Could not read EEPROM." :
                (s_editFailReason == 2) ? "FAILED - Security decrypt failed." :
                (s_editFailReason == 4) ? "FAILED - Checksum validation failed." :
                "FAILED - Write rejected by kernel.";
            DrawText(bx + 16.f, by + 16.f, failMsg, 1.3f, COL_RED);
            HLine(by + 38.f, bx + 1.f, bx + bw - 1.f, 0x33FF0000);
        }
        HLine(by + 72.f, bx + 1.f, bx + bw - 1.f, 0x33FFFFFF);
        DrawText(bx + 16.f, by + 84.f, "Press any button to return.", 1.1f, COL_GRAY);
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
            static const char* vstd[4] = {
                "NTSC-M (N.America)", "NTSC-J (Japan)",
                "PAL-I (Europe/AUS)", "PAL-M (Brazil)"
            };
            int idx = (int)s_editVideoStd - 1;
            if (idx < 0 || idx > 3) idx = 0;
            SafeCopy(buf, sizeof(buf), sel ? "< " : "  ");
            SafeAppend(buf, sizeof(buf), vstd[idx]);
            if (sel) SafeAppend(buf, sizeof(buf), " >");
            DrawText(VAL_X, y, buf, 1.2f, sel ? COL_CYAN : COL_WHITE);
            y += ROW;
        }

        // Helper macro-style: bitmask rows (bits in the HIGH word per VideoSettings enum)
        struct { const char* lbl; DWORD bit; } vflags[5] = {
            { "Widescreen",  0x00010000 },
            { "HDTV 720p",   0x00020000 },
            { "HDTV 1080i",  0x00040000 },
            { "HDTV 480p",   0x00080000 },
            { "Letterbox",   0x00100000 },
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

        // PAL-60 — only shown when PAL-I or PAL-M is selected
        if (s_editVideoStd == 3 || s_editVideoStd == 4)
        {
            bool sel = (s_editCursor == 6);
            DrawText(LBL_X + 16.f, y, "PAL 60Hz", 1.15f, sel ? COL_YELLOW : COL_WHITE);
            bool on = (s_editVideoFlags & 0x00400000) != 0;
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

        // Row 0: Game Region — display only
        // The region is in the RC4-encrypted security section (0x2C).
        // Writing it requires per-console key re-encryption — not supported here.
        {
            DrawText(LBL_X, y, "Game Region", 1.2f, COL_DIM);
            static const char* greg[5] = {
                "N. America", "Japan", "Rest of World", "Manufacturing", "ALL (debug)"
            };
            static const DWORD gregVals[5] = {
                0x00000001, 0x00000002, 0x00000004, 0x80000000, 0xFFFFFFFF
            };
            ULONG type = 0, len = 0;
            DWORD gr = 0x00000001;
            ExQueryNonVolatileSetting(XC_GAME_REGION, &type, &gr, 4, &len);
            int idx = 0;
            for (int i = 0; i < 5; ++i)
                if (gr == gregVals[i]) { idx = i; break; }
            char tmp[32];
            SafeCopy(tmp, sizeof(tmp), greg[idx]);
            SafeAppend(tmp, sizeof(tmp), " (read-only)");
            DrawText(VAL_X, y, tmp, 1.2f, COL_DIM);
            y += ROW;
        }

        // Row 1: DVD Region
        {
            bool sel = (s_editCursor == 0);
            DrawText(LBL_X, y, "DVD Region", 1.2f, sel ? COL_YELLOW : COL_WHITE);
            static const char* dvdr[7] = {
                "0 - Region Free",
                "1 (USA/CA)", "2 (EUR/JPN)", "3 (SE Asia)",
                "4 (AUS/LAT)", "5 (USSR)", "6 (China)"
            };
            int idx = (int)s_editDvdRegion;
            if (idx < 0 || idx > 6) idx = 0;
            SafeCopy(buf, sizeof(buf), sel ? "< " : "  ");
            SafeAppend(buf, sizeof(buf), dvdr[idx]);
            if (sel) SafeAppend(buf, sizeof(buf), " >");
            DrawText(VAL_X, y, buf, 1.2f, sel ? COL_CYAN : COL_WHITE);
            y += ROW;
        }

        // Row 2: Game Rating
        {
            bool sel = (s_editCursor == 1);
            DrawText(LBL_X, y, "Game Rating", 1.2f, sel ? COL_YELLOW : COL_WHITE);
            static const char* pcr[7] = {
                "DISABLED (All)", "Adults Only", "Mature (M)",
                "Teen (T)", "Everyone (E)", "Kids to Adults", "Early Childhood"
            };
            int idx = (int)s_editGameRating;
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
                int display = -TzRawBias(s_tzTable[idx]);
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
                if (TzRawDstBias(s_tzTable[idx]) != 0)
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
        RepairBuildDiag();
        s_loaded = true;
        return;
    }

    WORD cur = GetButtons();

    // Global B/Back — not when edit or repair own input
    if (s_view != VIEW_EDIT && s_view != VIEW_REPAIR)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            RequestState(MSTATE_MENU);
            s_prevBtns = cur;
            return;
        }
    }

    if (s_view == VIEW_REPAIR)
    {
        RepairHandleInput(cur, s_prevBtns, logo);
        s_prevBtns = cur;
        RenderRepair(logo);  // repair view owns its own BeginScene/EndScene/Present
        return;
    }
    else if (s_view == VIEW_EDIT)
    {
        // Edit view owns all input
        EditHandleInput(cur);
    }
    else
    {
        // Left/Right only switches views when NOT in edit mode
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
            s_view = VIEW_DECODED;

        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT))
            s_view = VIEW_HEX;

        if (EdgeDown(cur, s_prevBtns, BTN_A))
            SaveBin();

        // X enters repair view from decoded
        if (s_view == VIEW_DECODED && EdgeDown(cur, s_prevBtns, BTN_X))
            s_view = VIEW_REPAIR;

        // White enters restore-from-file flow (only if eeprom.bin exists)
        if (s_view == VIEW_DECODED && s_binExists
            && EdgeDown(cur, s_prevBtns, BTN_WHITE))
        {
            s_restoreConfirm = true;
            s_restoreDone = false;
            s_repRan = false;
            s_view = VIEW_REPAIR;
        }

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
    else if (s_view == VIEW_EDIT)
        RenderEdit(logo);
    else if (s_view == VIEW_REPAIR)
        RenderRepair(logo);
    else
        RenderDecoded(logo);
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

    // Game region: encrypted in security section (0x2C).
    // Read via kernel API which returns the decrypted value.
    {
        ULONG type = 0, len = 0;
        DWORD gr = 0;
        const char* grStr = "Unknown";
        if (ExQueryNonVolatileSetting(XC_GAME_REGION, &type, &gr, 4, &len) == 0)
        {
            if (gr == 0x00000001) grStr = "N. America";
            else if (gr == 0x00000002) grStr = "Japan";
            else if (gr == 0x00000004) grStr = "Rest of World";
            else if (gr == 0x80000000) grStr = "Manufacturing";
            else if (gr == 0xFFFFFFFF) grStr = "All / Debug";
        }
        else grStr = "Kernel read failed";
        WL("Game Region:  ", grStr);
    }
}

// ============================================================================
// EEPROM Repair
// ============================================================================
//
// Diagnostic items (16 total):
//
//   REPAIRABLE via SMBus direct write:
//     Factory checksum  (0x30) — ~Calc(data, 0x34, 0x2C)
//
//   REPAIRABLE via ExSaveNonVolatileSetting (kernel recalculates user checksum):
//     User checksum     (0x60) — verified after kernel writes; repaired implicitly
//     Video standard    (0x58) — reset to NTSC-M (0x00400100)
//     Video flags       (0x94) — mask to valid bits 0x005F0000
//     Audio flags       (0x98) — mask to valid bits 0x00030003
//     Game region       (0x54) — reset to NA (0x00000001)
//     DVD region        (0xBC) — reset to 0 (no region lock)
//     Language          (0x90) — reset to English (1)
//     Timezone          (0x64) — reset bias to 0 (UTC)
//     Game rating       (0x9C) — reset to disabled (0)
//     Movie rating      (0xA4) — reset to disabled (0)
//     Parental passcode (0xA0) — reset to disabled (0)
//
//   DETECT ONLY (no repair possible):
//     Security hash     (0x00) — HMAC-SHA1 requires per-version RC4 key baked into kernel
//                                Detected via ExQueryNonVolatileSetting return value
//     Serial number     (0x34) — factory assigned, 12 ASCII digits check
//     MAC address       (0x40) — Xbox OUI prefix check + no multicast bit
//     Online key        (0x48) — nonzero check (all-zeros = never provisioned)
//
//   NEVER TOUCHED:
//     HDD key           (0x1C) — encrypted, hardware-unique; touching this bricks the HDD
//     Confounder        (0x14) — part of the security section encryption
//
// Checksum algorithm (RE'd from Xbox kernel 4034 by XboxEepromEditor):
//   Accumulate all DWORDs with 64-bit carry tracking.
//   result = high + low
//   stored = ~result
//   valid:  stored + result == 0xFFFFFFFF  →  Calc(section_including_stored) == 0xFFFFFFFF
//
// Write paths:
//   Factory checksum  → HalWriteSMBusValue to EEPROM IC at 0xA8, byte by byte, 10ms delay
//   All user fields   → ExSaveNonVolatileSetting, which re-encrypts and recalculates checksums
//
// Restore from D:\eeprom.bin:
//   Reads 256 bytes, validates both checksums, writes all bytes via SMBus.
//   A confirm modal is shown before any write for both repair and restore.
//   valid: ~result + result == 0xFFFFFFFF  -->  (checksum + Calc(section)) == 0xFFFFFFFF
//
// Factory write path: direct HalWriteSMBusValue to EEPROM at 0xA8
// User write path:    ExSaveNonVolatileSetting — kernel handles re-encryption + checksums
// ============================================================================
// EEPROM Repair
// ============================================================================
//
// Diagnostic items:
//
//   CHECKSUM SECTION (can repair)
//   1. Factory checksum  0x30  ~Calc(0x34, 0x2C)  covers serial/MAC/online key/video std
//   2. User checksum     0x60  ~Calc(0x64, 0x5C)  covers timezone/language/flags/ratings
//
//   SECURITY SECTION (detect only — cannot repair without per-version RC4 key)
//   3. HMAC SHA1 hash    0x00  result of HMAC-SHA1 over decrypted confounder+HDD key+region
//      We detect this by checking if ExQueryNonVolatileSetting returns STATUS_DEVICE_DATA_ERROR
//      (0xC000009C), which is exactly what the kernel returns on hash mismatch.
//
//   USER SETTINGS (can repair via ExSaveNonVolatileSetting)
//   4. Video standard    0x58  must be one of four known DWORD values
//   5. Video flags       0x94  must not have bits outside mask 0x005F0000
//   6. Audio flags       0x98  must not have bits outside mask 0x00030003
//   7. Game region       0x2C  (security section, read via kernel) must be known region code
//   8. DVD region        0xBC  0-6 (0=none, 1-6=CSS zone)
//   9. Language          0x90  0-9 (Neutral through Portuguese)
//  10. Timezone          0x64  zone bias + std/dst names match a known Xbox TZ entry
//
// NEVER touched: HDD key (0x1C-0x2B), confounder (0x14-0x1B), serial (0x34),
//                MAC (0x40), online key (0x48), security hash (0x00).
//
// Checksum algorithm RE'd from Xbox kernel 4034 (confirmed same in 5838):
//   Sum all DWORDs tracking carry in high DWORD.
//   stored = ~(high + low)
//   valid:  stored + (high+low) == 0xFFFFFFFF
// ============================================================================

// ---- Checksum ---------------------------------------------------------------

static DWORD EepCalcChecksum(const BYTE* data, int offset, int size)
{
    DWORD high = 0, low = 0;
    const BYTE* p = data + offset;
    for (int i = 0; i < size / 4; ++i)
    {
        DWORD val = (DWORD)p[i * 4 + 0]
            | ((DWORD)p[i * 4 + 1] << 8)
            | ((DWORD)p[i * 4 + 2] << 16)
            | ((DWORD)p[i * 4 + 3] << 24);
        DWORD newLow = low + val;
        if (newLow < low) ++high;
        low = newLow;
    }
    return high + low;
}

static bool EepFactoryChecksumOK()
{
    return EepCalcChecksum(s_eeprom, 0x30, 0x30) == 0xFFFFFFFF;
}

static bool EepUserChecksumOK()
{
    return EepCalcChecksum(s_eeprom, 0x60, 0x60) == 0xFFFFFFFF;
}

static DWORD EepReadDW(int off)
{
    return (DWORD)s_eeprom[off]
        | ((DWORD)s_eeprom[off + 1] << 8)
        | ((DWORD)s_eeprom[off + 2] << 16)
        | ((DWORD)s_eeprom[off + 3] << 24);
}

// ---- Field validators -------------------------------------------------------

static bool EepHmacOK()
{
    // Probe: ExQueryNonVolatileSetting(0xFFFF) returns STATUS_DEVICE_DATA_ERROR
    // (0xC000009C) if the HMAC SHA1 verification fails during the kernel's
    // cached EEPROM read. A 0 return means the hash is valid for this console.
    // This is the same check the kernel itself uses — no need to re-implement
    // the HMAC algorithm.
    ULONG type = 0, len = 0;
    BYTE  buf[256];
    LONG  r = ExQueryNonVolatileSetting(0xFFFF, &type, buf, 256, &len);
    return (r == 0);
}

static bool EepVideoStdOK()
{
    DWORD vs = EepReadDW(0x58);
    return (vs == 0x00400100 || vs == 0x00400200 ||
        vs == 0x00800300 || vs == 0x00400400);
}

static bool EepVideoFlagsOK()
{
    // Valid bits: widescreen=0x00010000, 720p=0x00020000, 1080i=0x00040000,
    //             480p=0x00080000, letterbox=0x00100000, PAL60=0x00400000
    return (EepReadDW(0x94) & ~0x005F0000UL) == 0;
}

static bool EepAudioFlagsOK()
{
    // Valid bits: mono=0x00000001, surround=0x00000002 (stereo=0), AC3=0x00010000, DTS=0x00020000
    return (EepReadDW(0x98) & ~0x00030003UL) == 0;
}

static bool EepGameRegionOK()
{
    // Security section is RC4-encrypted in the raw buffer — read game region
    // via kernel API which returns the decrypted value.
    ULONG type = 0, len = 0;
    DWORD gr = 0;
    if (ExQueryNonVolatileSetting(XC_GAME_REGION, &type, &gr, 4, &len) != 0)
        return false;
    return (gr == 0x00000001 || gr == 0x00000002 || gr == 0x00000004 ||
        gr == 0x80000000 || gr == 0x000000FF);
}

static bool EepDvdRegionOK()
{
    DWORD dr = EepReadDW(0xBC);
    return (dr <= 6);
}

static bool EepLanguageOK()
{
    DWORD lang = EepReadDW(0x90);
    // 0=Neutral, 1=English, 2=Japanese, 3=German, 4=French,
    // 5=Spanish, 6=Italian, 7=Korean, 8=Chinese, 9=Portuguese
    return (lang <= 9);
}

static bool EepTimezoneOK()
{
    // The timezone block (0x64-0x8F, 44 bytes) must match one of the known
    // Xbox timezone entries. We check the bias (0x64) is a plausible UTC
    // offset (within ±14 hours = ±840 minutes) and the std/dst name bytes
    // (0x68-0x6B and 0x6C-0x6F) are printable ASCII or zero.
    int bias = (int)EepReadDW(0x64);
    // The bias is stored in minutes (int32, positive = west of UTC).
    // Valid range: ±840 minutes (UTC-14 to UTC+14).
    if (bias < -840 || bias > 840) return false;
    // Check name bytes are ASCII printable or zero
    for (int i = 0x68; i < 0x70; ++i)
    {
        BYTE b = s_eeprom[i];
        if (b != 0 && (b < 0x20 || b > 0x7E)) return false;
    }
    return true;
}

// Additional validators aligned to XboxEepromEditor field definitions

static bool EepSerialOK()
{
    // Serial: 12 ASCII digits at 0x34
    for (int i = 0; i < 12; ++i)
        if (s_eeprom[0x34 + i] < '0' || s_eeprom[0x34 + i] > '9') return false;
    return true;
}

static bool EepMacOK()
{
    // MAC at 0x40: first byte must be one of the known Xbox OUI prefixes
    // 00:50:F2 (1.0+), 00:0D:3A (1.1+), 00:12:5A (1.6)
    BYTE b0 = s_eeprom[0x40], b1 = s_eeprom[0x41], b2 = s_eeprom[0x42];
    if (b0 == 0x00 && b1 == 0x50 && b2 == 0xF2) return true;
    if (b0 == 0x00 && b1 == 0x0D && b2 == 0x3A) return true;
    if (b0 == 0x00 && b1 == 0x12 && b2 == 0x5A) return true;
    // Multicast bit must not be set
    return (b0 & 0x01) == 0 && !(b0 == 0 && b1 == 0 && b2 == 0);
}

static bool EepParentalPasscodeOK()
{
    // Parental passcode at 0xA0: each nibble must be 0-4 (up/right/down/left/none)
    // 0=none, 1=up, 2=down, 3=left, 4=right; 0x00000000 = disabled
    DWORD pc = EepReadDW(0xA0);
    if (pc == 0) return true;
    for (int i = 0; i < 8; ++i)
    {
        BYTE nib = (BYTE)((pc >> (i * 4)) & 0xF);
        if (nib > 4) return false;
    }
    return true;
}

static bool EepGameRatingOK()
{
    // Max game rating at 0x9C: 0=disabled, 1-8 = ESRB ratings, 0xFF = all blocked
    DWORD gr = EepReadDW(0x9C);
    return (gr == 0 || (gr >= 1 && gr <= 8) || gr == 0xFF);
}

static bool EepMovieRatingOK()
{
    // XboxEepromEditor MovieRating: NR=0, NC17=1, R=2, PG13=4, PG=5, G=7
    // Values 3 and 6 are not defined (non-sequential enum).
    DWORD mr = EepReadDW(0xA4);
    return (mr == 0 || mr == 1 || mr == 2 || mr == 4 || mr == 5 || mr == 7);
}

static bool EepOnlineKeyNonzero()
{
    // Online key at 0x48 (16 bytes) — all zeros suggests it was never provisioned
    for (int i = 0; i < 16; ++i)
        if (s_eeprom[0x48 + i] != 0) return true;
    return false;
}

// ---- Restore from D:\eeprom.bin ---------------------------------------------
// Writes 256 bytes back to the EEPROM IC via SMBus, byte by byte.
// The file must be exactly 256 bytes. We verify the factory and user checksums
// of the file before writing — if they're wrong we abort.

static bool RestoreFromFile()
{
    HANDLE hf = CreateFileA("D:\\eeprom.bin", GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    BYTE  buf[256];
    DWORD nr = 0;
    BOOL  ok = ReadFile(hf, buf, 256, &nr, NULL);
    CloseHandle(hf);

    if (!ok || nr != 256) return false;

    // Validate checksums before writing
    DWORD factoryCS = EepCalcChecksum(buf, 0x30, 0x30);
    DWORD userCS = EepCalcChecksum(buf, 0x60, 0x60);
    if (factoryCS != 0xFFFFFFFF || userCS != 0xFFFFFFFF) return false;

    // Write byte by byte via SMBus with write cycle delay
    for (int i = 0; i < 256; ++i)
    {
        if (!SMBusWrite(SMBADDR_EEPROM, (BYTE)i, buf[i])) return false;
        Sleep(10);  // 93LC56 write cycle time
    }

    // Update local cache
    for (int i = 0; i < 256; ++i) s_eeprom[i] = buf[i];
    return true;
}

// ---- SMBus EEPROM direct write (byte at a time with write cycle delay) ------

static bool EepSMBusWriteDW(int offset, DWORD val)
{
    bool ok = true;
    for (int i = 0; i < 4; ++i)
    {
        BYTE b = (BYTE)((val >> (i * 8)) & 0xFF);
        if (!SMBusWrite(SMBADDR_EEPROM, (BYTE)(offset + i), b))
            ok = false;
        Sleep(10);  // 93LC56 write cycle time

    }
    return ok;
}

// ---- Repair item table ------------------------------------------------------

enum RepairItemState { REP_OK = 0, REP_BAD, REP_FIXED, REP_FAIL, REP_INFO };

struct RepairItem
{
    const char* label;
    const char* description;
    RepairItemState state;
    bool            canRepair;  // false = detect-only
};

static RepairItem s_repItems[16];
static int        s_repCount = 0;

static void RepairBuildDiag()
{
    s_repCount = 0;

    auto add = [](const char* lbl, const char* desc, bool ok, bool canFix) {
        s_repItems[s_repCount].label = lbl;
        s_repItems[s_repCount].description = desc;
        s_repItems[s_repCount].state = ok ? REP_OK : (canFix ? REP_BAD : REP_INFO);
        s_repItems[s_repCount].canRepair = canFix;
        ++s_repCount;
        };

    // Checksum section
    add("Factory Checksum", "0x30  covers serial/MAC/keys/video std", EepFactoryChecksumOK(), true);
    add("User Checksum", "0x60  covers timezone/language/flags", EepUserChecksumOK(), true);

    // Security section (detect only — no repair without kernel crypto)
    add("Security Hash", "0x00  HMAC-SHA1  (detect only)", EepHmacOK(), false);

    // Factory section fields — from XboxEepromEditor layout
    add("Serial Number", "0x34  12 ASCII digits", EepSerialOK(), false);
    add("MAC Address", "0x40  Xbox OUI prefix + non-multicast", EepMacOK(), false);
    add("Online Key", "0x48  16-byte key (nonzero check)", EepOnlineKeyNonzero(), false);

    // User settings — repairable via ExSaveNonVolatileSetting
    add("Video Standard", "0x58  NTSC-M/J or PAL-I/M", EepVideoStdOK(), true);
    add("Video Flags", "0x94  no undocumented bits set", EepVideoFlagsOK(), true);
    add("Audio Flags", "0x98  no undocumented bits set", EepAudioFlagsOK(), true);
    add("Game Region", "0x2C (enc)  NA / Japan / RoW / Dev", EepGameRegionOK(), true);
    add("DVD Region", "0xBC  CSS zone 0-6", EepDvdRegionOK(), true);
    add("Language", "0x90  Neutral-Portuguese (0-9)", EepLanguageOK(), true);
    add("Timezone", "0x64  valid bias + printable TZ name", EepTimezoneOK(), true);
    add("Game Rating", "0x9C  ESRB 0-8 or 0xFF", EepGameRatingOK(), true);
    add("Movie Rating", "0xA4  MPAA 0-8 or 0xFF", EepMovieRatingOK(), true);
    add("Parental Passcode", "0xA0  nibbles 0-4 or zero", EepParentalPasscodeOK(), true);
}

static int RepairBadCount()
{
    int n = 0;
    for (int i = 0; i < s_repCount; ++i)
        if (s_repItems[i].state == REP_BAD) ++n;
    return n;
}

// ---- Apply repairs ----------------------------------------------------------
// Strategy matches PrometheOS save() approach:
//   1. Game region (security section) — kernel-managed per-field write first
//   2. Read fresh 256-byte image
//   3. Write all bad user-section field defaults directly into the buffer
//   4. Recalculate both checksums in the buffer
//   5. Write all 256 bytes atomically
//
// Factory checksum (item 0) is included in the Checksum2 recalculation at step 4 —
// no separate SMBus write needed. Items 2-5 (security hash, serial, MAC, online key)
// are detect-only and never modified.

static void RepairApply()
{
    // ── Step 1: Game region (item 9) — kernel handles security section encryption
    if (s_repItems[9].state == REP_BAD)
    {
        DWORD gr = 0x00000001;  // reset to N. America
        LONG r = ExSaveNonVolatileSetting(XC_GAME_REGION, REG_DWORD, &gr, 4);
        s_repItems[9].state = (r == 0) ? REP_FIXED : REP_FAIL;
    }

    // ── Step 2: Read fresh 256-byte image ─────────────────────────────────────
    BYTE buf[256];
    {
        ULONG type = 0, len = 0;
        if (ExQueryNonVolatileSetting(0xFFFF, &type, buf, 256, &len) != 0)
        {
            // Cannot read — mark all pending repairs failed
            for (int i = 0; i < s_repCount; ++i)
                if (s_repItems[i].state == REP_BAD) s_repItems[i].state = REP_FAIL;
            return;
        }
    }

    // ── Step 3: Write safe defaults for each bad user-section field ───────────

    // Item 6: Video Standard (0x58) — factory section (Checksum2 covers this)
    if (s_repItems[6].state == REP_BAD)
    {
        BufWriteDW(buf, 0x58, 0x00400100);  // NTSC-M
        s_repItems[6].state = REP_FIXED;    // confirmed below after write
    }

    // Item 7: Video Flags (0x94) — mask undocumented bits, preserve valid ones
    if (s_repItems[7].state == REP_BAD)
    {
        DWORD vf = EepReadDW(0x94) & 0x005F0000UL;
        BufWriteDW(buf, 0x94, vf);
        s_repItems[7].state = REP_FIXED;
    }

    // Item 8: Audio Flags (0x98) — mask undocumented bits, resolve mono+surround conflict
    if (s_repItems[8].state == REP_BAD)
    {
        DWORD af = EepReadDW(0x98) & 0x00030003UL;
        if ((af & 0x03) == 0x03) af &= ~0x01UL;  // mono+surround both set → keep surround
        BufWriteDW(buf, 0x98, af);
        s_repItems[8].state = REP_FIXED;
    }

    // Item 10: DVD Region (0xBC)
    if (s_repItems[10].state == REP_BAD)
    {
        BufWriteDW(buf, 0xBC, 0);  // 0 = region free
        s_repItems[10].state = REP_FIXED;
    }

    // Item 11: Language (0x90)
    if (s_repItems[11].state == REP_BAD)
    {
        BufWriteDW(buf, 0x90, 1);  // 1 = English
        s_repItems[11].state = REP_FIXED;
    }

    // Item 12: Timezone — reset to London/UTC+0 (full 44-byte block at 0x64-0x8F)
    if (s_repItems[12].state == REP_BAD)
    {
        // Find the London entry (or first UTC+0 entry) in the table
        int tzIdx = 0;
        for (int i = 0; i < TZ_COUNT; ++i)
            if (TzRawBias(s_tzTable[i]) == 0) { tzIdx = i; break; }
        for (int i = 0; i < 44; ++i)
            buf[0x64 + i] = s_tzTable[tzIdx].raw[i];
        s_repItems[12].state = REP_FIXED;
    }

    // Item 13: Game Rating (0x9C)
    if (s_repItems[13].state == REP_BAD)
    {
        BufWriteDW(buf, 0x9C, 0);  // 0 = disabled (all ages)
        s_repItems[13].state = REP_FIXED;
    }

    // Item 14: Movie Rating (0xA4)
    if (s_repItems[14].state == REP_BAD)
    {
        BufWriteDW(buf, 0xA4, 0);  // 0 = disabled
        s_repItems[14].state = REP_FIXED;
    }

    // Item 15: Parental Passcode (0xA0)
    if (s_repItems[15].state == REP_BAD)
    {
        BufWriteDW(buf, 0xA0, 0);  // 0 = disabled
        s_repItems[15].state = REP_FIXED;
    }

    // ── Step 4: Recalculate both checksums ────────────────────────────────────
    // Checksum2 (0x30) covers factory section 0x34-0x5F (video std lives here).
    // Checksum3 (0x60) covers user section 0x64-0xBF (all other repaired fields).
    // This also implicitly repairs items 0 and 1 (bad checksums).
    EepRecalcChecksums(buf);

    // ── Step 5: Write all 256 bytes atomically ────────────────────────────────
    ULONG type = 3;
    bool writeOK = (ExSaveNonVolatileSetting(0xFFFF, type, buf, 256) == 0);

    // If write failed, demote all freshly-marked FIXED items to FAIL
    if (!writeOK)
    {
        for (int i = 0; i < s_repCount; ++i)
            if (s_repItems[i].state == REP_FIXED) s_repItems[i].state = REP_FAIL;
        return;
    }

    // ── Step 6: Re-read and verify checksums ──────────────────────────────────
    {
        ULONG rtype = 0, rlen = 0;
        BYTE verify[256];
        if (ExQueryNonVolatileSetting(0xFFFF, &rtype, verify, 256, &rlen) == 0)
        {
            for (int i = 0; i < 256; ++i) s_eeprom[i] = verify[i];

            // Update checksum repair states based on actual on-chip result
            if (s_repItems[0].state == REP_BAD || s_repItems[0].state == REP_FIXED)
                s_repItems[0].state = EepFactoryChecksumOK() ? REP_FIXED : REP_FAIL;
            if (s_repItems[1].state == REP_BAD || s_repItems[1].state == REP_FIXED)
                s_repItems[1].state = EepUserChecksumOK() ? REP_FIXED : REP_FAIL;
        }
    }
}

// ---- Repair render ----------------------------------------------------------

static void RenderRepair(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    // ---- Full-screen confirm overlay (repair or restore) --------------------
    // Rendered over a dimmed background before anything else so it
    // feels like a proper modal rather than a banner at the bottom.
    if (s_repConfirm || s_restoreConfirm)
    {
        // Dim the whole screen
        FillRect(0.f, 0.f, SW, SH, D3DCOLOR_ARGB(160, 0, 0, 0));

        // Box
        const float BW = 420.f;
        const float BH = 110.f;
        const float BX = (SW - BW) * 0.5f;
        const float BY = (SH - BH) * 0.5f;
        FillRect(BX, BY, BX + BW, BY + BH, D3DCOLOR_XRGB(18, 18, 32));
        // Border
        HLine(BY, BX, BX + BW, s_restoreConfirm ? COL_ORANGE : COL_CYAN);
        HLine(BY + BH, BX, BX + BW, s_restoreConfirm ? COL_ORANGE : COL_CYAN);
        VLine(BX, BY, BY + BH, s_restoreConfirm ? COL_ORANGE : COL_CYAN);
        VLine(BX + BW, BY, BY + BH, s_restoreConfirm ? COL_ORANGE : COL_CYAN);

        const char* title = s_restoreConfirm ? "RESTORE FROM EEPROM.BIN" : "CONFIRM REPAIR";
        const char* body = s_restoreConfirm
            ? "Write all 256 bytes from D:\\eeprom.bin to EEPROM hardware?"
            : "Write repairs to EEPROM hardware?";
        const char* warning = s_restoreConfirm
            ? "File checksums are verified before writing."
            : "Only corrupt fields will be written. HDD key untouched.";

        DrawText(BX + 12.f, BY + 10.f, title, 1.3f,
            s_restoreConfirm ? COL_ORANGE : COL_CYAN);
        DrawText(BX + 12.f, BY + 32.f, body, 1.1f, COL_WHITE);
        DrawText(BX + 12.f, BY + 52.f, warning, 1.0f, COL_DIM);
        DrawText(BX + 12.f, BY + 76.f,
            "[A] Yes, proceed    [B] Cancel",
            1.15f, COL_YELLOW);

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // Build hint based on current state.
    // Note: s_repConfirm and s_restoreConfirm are handled by the early-return
    // modal overlay above — those flags can never be true here.
    const char* hint;
    if (s_restoreDone)
        hint = s_restoreOK ? "[A] Re-scan    [B] Back"
        : "[B] Back — restore failed";
    else if (s_repRan)
        hint = "[A] Re-scan    [B] Back";
    else if (RepairBadCount() > 0)
        hint = "[A] Repair issues    [B] Back";
    else
        hint = "[B] Back — no repairable issues";

    DrawPageChrome(logo, "EEPROM - REPAIR", hint);

    float y = CONTENT_Y + 4.f;

    // ---- Restore-from-file result banner (restore done, not confirm — confirm
    //      was already handled by the modal overlay above) --------------------
    if (s_restoreDone)
    {
        // Restore done
        if (s_restoreOK)
        {
            FillRect(0.f, y, SW, y + LINE_H * 2.f + 12.f, D3DCOLOR_ARGB(50, 0, 160, 60));
            DrawText(LM, y + 4.f, "RESTORE COMPLETE", 1.3f, COL_GREEN);
            y += LINE_H + 6.f;
            DrawText(LM, y,
                "All 256 bytes written. Press [A] to re-scan, or [B] to return.",
                1.1f, COL_WHITE);
        }
        else
        {
            FillRect(0.f, y, SW, y + LINE_H * 3.f + 14.f, D3DCOLOR_ARGB(50, 180, 40, 0));
            DrawText(LM, y + 4.f, "RESTORE FAILED", 1.3f, COL_RED);
            y += LINE_H + 6.f;
            DrawText(LM, y,
                "Possible causes: file not found, not 256 bytes,",
                1.05f, COL_WHITE);
            y += LINE_H;
            DrawText(LM, y,
                "invalid checksums in file, or SMBus write error.",
                1.05f, COL_WHITE);
        }

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // ---- Normal repair view -------------------------------------------------
    DrawText(LM, y,
        "! HDD key / confounder / security hash / serial / MAC are never modified.",
        1.05f, COL_ORANGE);
    y += LINE_H + 6.f;
    HLine(y, LM, SW - LM, COL_BORDER);
    y += 4.f;

    // Column headers
    const float COL_LBL = LM;
    const float COL_DESC = LM + 148.f;
    const float COL_STA = LM + 360.f;
    DrawText(COL_LBL, y, "FIELD", 1.0f, COL_DIM);
    DrawText(COL_DESC, y, "DESCRIPTION", 1.0f, COL_DIM);
    DrawText(COL_STA, y, "STATUS", 1.0f, COL_DIM);
    y += LINE_H;
    HLine(y, 0.f, SW, D3DCOLOR_XRGB(18, 22, 48));
    y += 3.f;

    const float ROW_H = LINE_H + 3.f;
    for (int i = 0; i < s_repCount; ++i)
    {
        const RepairItem& ri = s_repItems[i];

        if (ri.state == REP_BAD)
            FillRect(0.f, y - 1.f, SW, y + ROW_H - 1.f, D3DCOLOR_ARGB(35, 200, 60, 0));
        else if (ri.state == REP_FIXED)
            FillRect(0.f, y - 1.f, SW, y + ROW_H - 1.f, D3DCOLOR_ARGB(35, 0, 180, 60));
        else if (ri.state == REP_INFO)
            FillRect(0.f, y - 1.f, SW, y + ROW_H - 1.f, D3DCOLOR_ARGB(20, 80, 80, 200));

        DWORD lblCol = (ri.state == REP_BAD || ri.state == REP_FAIL) ? COL_WHITE
            : (ri.state == REP_FIXED) ? D3DCOLOR_XRGB(140, 220, 160)
            : COL_GRAY;

        DrawText(COL_LBL, y, ri.label, 1.15f, lblCol);
        DrawText(COL_DESC, y, ri.description, 1.0f, COL_DIM);

        const char* stateStr;
        DWORD stateCol;
        switch (ri.state)
        {
        case REP_OK:    stateStr = "OK";       stateCol = COL_GREEN;                 break;
        case REP_BAD:   stateStr = "CORRUPT";  stateCol = COL_RED;                   break;
        case REP_FIXED: stateStr = "REPAIRED"; stateCol = COL_CYAN;                  break;
        case REP_FAIL:  stateStr = "FAILED";   stateCol = COL_ORANGE;                break;
        case REP_INFO:  stateStr = "INVALID";  stateCol = D3DCOLOR_XRGB(160, 120, 80); break;
        default:        stateStr = "?";        stateCol = COL_DIM;                   break;
        }

        static char s_staBuf[32];
        if (!ri.canRepair && ri.state == REP_INFO)
        {
            StrCopy(s_staBuf, sizeof(s_staBuf), stateStr);
            StrCat2(s_staBuf, sizeof(s_staBuf), s_staBuf, " (no repair)");
            stateStr = s_staBuf;
        }

        DrawText(COL_STA, y, stateStr, 1.1f, stateCol);
        HLine(y + ROW_H - 1.f, 0.f, SW, D3DCOLOR_XRGB(14, 16, 34));
        y += ROW_H;
    }

    y += 6.f;

    if (s_repRan)
    {
        int fixed = 0, failed = 0;
        for (int i = 0; i < s_repCount; ++i)
        {
            if (s_repItems[i].state == REP_FIXED) ++fixed;
            if (s_repItems[i].state == REP_FAIL)  ++failed;
        }
        char msg[80]; char fa[6], fb[6];
        IntToStr(fixed, fa, sizeof(fa)); IntToStr(failed, fb, sizeof(fb));
        StrCopy(msg, sizeof(msg), "Complete: ");
        StrCat2(msg, sizeof(msg), msg, fa);
        StrCat2(msg, sizeof(msg), msg, " item(s) repaired");
        if (failed > 0)
        {
            StrCat2(msg, sizeof(msg), msg, ",  ");
            StrCat2(msg, sizeof(msg), msg, fb);
            StrCat2(msg, sizeof(msg), msg, " failed");
        }
        DrawText(LM, y, msg, 1.2f, failed ? COL_ORANGE : COL_GREEN);
        y += LINE_H + 2.f;
        DrawText(LM, y,
            "Press [A] to re-scan and confirm, or [B] to return to decoded view.",
            1.05f, COL_DIM);
    }
    else if (RepairBadCount() == 0)
    {
        int infoCount = 0;
        for (int i = 0; i < s_repCount; ++i)
            if (s_repItems[i].state == REP_INFO) ++infoCount;

        DrawText(LM, y, "All repairable fields are valid.", 1.2f, COL_GREEN);
        if (infoCount > 0)
        {
            y += LINE_H + 2.f;
            DrawText(LM, y,
                "! Security hash is invalid (detect only). "
                "Usually caused by EEPROM corruption or wrong kernel version.",
                1.05f, COL_ORANGE);
        }
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ---- Repair input handler ---------------------------------------------------

static void RepairHandleInput(WORD cur, WORD prev, const DiagLogo& logo)
{
    auto Edge = [](WORD c, WORD p, WORD b) { return (c & b) && !(p & b); };

    // ── Confirm: REPAIR ───────────────────────────────────────────────────
    if (s_repConfirm)
    {
        if (Edge(cur, prev, BTN_A))
        {
            // Confirmed — draw progress frame then execute
            g_pDevice->BeginScene();
            DrawPageChrome(logo, "EEPROM - REPAIR", "");
            DrawText(LM, CONTENT_Y + 60.f, "Repairing EEPROM...", 1.5f, COL_YELLOW);
            DrawText(LM, CONTENT_Y + 80.f, "Do not power off", 1.2f, COL_RED);
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            RepairApply();
            s_repRan = true;
            s_repConfirm = false;
        }
        else if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
        {
            // Aborted — dismiss confirm, stay in repair view
            s_repConfirm = false;
        }
        return;
    }

    // ── Confirm: RESTORE ──────────────────────────────────────────────────
    if (s_restoreConfirm)
    {
        if (Edge(cur, prev, BTN_A))
        {
            // Confirmed — draw progress frame then execute
            g_pDevice->BeginScene();
            DrawPageChrome(logo, "EEPROM - REPAIR", "");
            DrawText(LM, CONTENT_Y + 60.f, "Writing EEPROM...", 1.5f, COL_YELLOW);
            DrawText(LM, CONTENT_Y + 80.f, "Do not power off", 1.2f, COL_RED);
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            s_restoreOK = RestoreFromFile();
            s_restoreDone = true;
            s_restoreConfirm = false;
        }
        else if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
        {
            // Aborted — dismiss confirm, stay in repair view
            s_restoreConfirm = false;
        }
        return;
    }

    // ── Post-restore result ───────────────────────────────────────────────
    if (s_restoreDone)
    {
        if (Edge(cur, prev, BTN_A))
        {
            ReadEeprom();
            RepairBuildDiag();
            s_restoreDone = false;
            s_restoreOK = false;
            s_repRan = false;
        }
        else if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
        {
            s_restoreDone = false;
            s_view = VIEW_DECODED;
        }
        return;
    }

    // ── Post-repair result ────────────────────────────────────────────────
    if (s_repRan)
    {
        if (Edge(cur, prev, BTN_A))
        {
            ReadEeprom();
            RepairBuildDiag();
            s_repRan = false;
        }
        else if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
        {
            s_repRan = false;
            s_view = VIEW_DECODED;
        }
        return;
    }

    // ── Normal repair list ────────────────────────────────────────────────
    if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
    {
        s_view = VIEW_DECODED;
        return;
    }
    if (Edge(cur, prev, BTN_A) && RepairBadCount() > 0)
        s_repConfirm = true;
}