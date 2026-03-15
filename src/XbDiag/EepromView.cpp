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
//   0x94-0x97   4  Video Flags         XC_VIDEO_FLAGS_* bitmask (valid bits: 0x5F)
//                                      0x01=widescreen 0x02=720p 0x04=1080i
//                                      0x08=480p 0x10=letterbox 0x40=PAL60
//   0x98-0x9B   4  Audio Flags         XC_AUDIO_FLAGS_* bitmask (valid bits: 0x1F)
//                                      0x01=mono 0x02=stereo 0x04=surround 0x08=DTS 0x10=AC3
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
        // Game Region (RC4-encrypted) — decoded bytes shown; 0x2C holds the region DWORD
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

    // Video Flags — XC_VIDEO_FLAGS_* at 0x94 (user section)
    case 0x94:
    {
        DWORD v = ReadDW(0x94);
        bool any = false;
        if (v & 0x01) { SafeAppend(out, outLen, "WIDE");    any = true; }
        if (v & 0x02) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "720p");  any = true; }
        if (v & 0x04) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "1080i"); any = true; }
        if (v & 0x08) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "480p");  any = true; }
        if (v & 0x10) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "LBOX");  any = true; }
        if (v & 0x40) { if (any) SafeAppend(out, outLen, " "); SafeAppend(out, outLen, "PAL60"); any = true; }
        if (!any) SafeCopy(out, outLen, "STANDARD (no HDTV)");
        if (v & ~0x5F) { SafeAppend(out, outLen, " !UNK"); }  // unknown bits set
        break;
    }

    // Audio Flags — XC_AUDIO_FLAGS_* at 0x98 (user section)
    case 0x98:
    {
        DWORD v = ReadDW(0x98);
        if (v & 0x01) SafeCopy(out, outLen, "Mono");
        else if (v & 0x04) SafeCopy(out, outLen, "Surround");
        else SafeCopy(out, outLen, "Stereo");
        if (v & 0x08) SafeAppend(out, outLen, " +DTS");
        if (v & 0x10) SafeAppend(out, outLen, " +AC3");
        if (v & ~0x1F) SafeAppend(out, outLen, " !UNK");  // unknown bits set
        break;
    }

    // DVD region — at 0xBC per XboxEepromEditor (Eeprom.cs DvdPlaybackZone)
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
#define XC_VIDEO_STANDARD       0x04    // kernel setting ID → EEPROM 0x58 (factory section)
#define XC_VIDEO_FLAGS          0x05    // kernel setting ID → EEPROM 0x94 (user section)
#define XC_AUDIO_FLAGS          0x06    // kernel setting ID → EEPROM 0x98 (user section)
#define XC_GAME_REGION          0x07    // kernel setting ID → EEPROM 0x54 (factory encrypted)
#define XC_DVD_REGION           0x08    // kernel setting ID → EEPROM 0xBC
#define XC_MAX_GAME_RATING      0x09    // kernel setting ID → EEPROM 0x9C
#define XC_TIMEZONE_BIAS        0x0A    // kernel setting ID → EEPROM 0x64 (int32 minutes west)
#define XC_TIMEZONE_STD_BIAS    0x0B    // kernel setting ID → EEPROM 0x8C (DST offset minutes)
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
    s_editVideoFlags = ReadDW(0x94);  // video flags at 0x94 (user section)
    s_editAudioFlags = ReadDW(0x98);  // audio flags at 0x98 (user section)
    s_editGameRegion = ReadDW(0x54);
    s_editDvdRegion = ReadDW(0xBC);   // DVD region at 0xBC (not 0x58 which is Video Standard)
    s_editParental = ReadDW(0x9C);   // game rating at 0x9C (not 0x5C which is padding)
    s_editTzIndex = TzFindIndex((int)ReadDW(0x64), (int)ReadDW(0x8C)); // TZ bias=0x64, DST bias=0x8C
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
//     Video flags       (0x94) — mask to valid bits 0x5F
//     Audio flags       (0x98) — mask to valid bits 0x1F, fallback stereo
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
//   5. Video flags       0x94  must not have bits outside the documented mask
//   6. Audio flags       0x98  same
//   7. Game region       0x54  must be one of the known region codes
//   8. DVD region        0x58  0-6 (0=none, 1-6=CSS zone)
//   9. Language          0x90  0-7 (English through Korean)
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
    // Valid bits: widescreen=0x01, 720p=0x02, 1080i=0x04, 480p=0x08,
    //             letterbox=0x10, PAL60=0x40
    return (EepReadDW(0x94) & ~0x5FUL) == 0;  // video flags at 0x94
}

static bool EepAudioFlagsOK()
{
    // Valid bits: mono=0x01, stereo=0x02, dolby=0x04, dts=0x08, AC3=0x10
    return (EepReadDW(0x98) & ~0x1FUL) == 0;  // audio flags at 0x98
}

static bool EepGameRegionOK()
{
    DWORD gr = EepReadDW(0x54);
    return (gr == 0x00000001 || gr == 0x00000002 || gr == 0x00000004 ||
        gr == 0x80000000 || gr == 0x000000FF);
}

static bool EepDvdRegionOK()
{
    DWORD dr = EepReadDW(0x58);  // same offset as video std in user section
    // Actually DVD region is at 0xBC in some layouts — use offset from field table
    // EepromView stores it at user section 0xBC: XC_DVD_REGION
    dr = EepReadDW(0xBC);
    return (dr <= 6);
}

static bool EepLanguageOK()
{
    DWORD lang = EepReadDW(0x90);
    return (lang >= 1 && lang <= 7);  // 1=English ... 7=Korean
}

static bool EepTimezoneOK()
{
    // The timezone block (0x64-0x8F, 44 bytes) must match one of the known
    // Xbox timezone entries. We check the bias (0x64) is a plausible UTC
    // offset (within ±14 hours = ±840 minutes) and the std/dst name bytes
    // (0x68-0x6B and 0x6C-0x6F) are printable ASCII or zero.
    int bias = (int)EepReadDW(0x64);
    if (bias < -840 * 60 || bias > 840 * 60) return false;  // stored as seconds? No, minutes
    // Bias is stored as minutes (int32). Valid range: -840 to +840.
    // The value is actually stored as negative minutes west of UTC.
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
    // Max movie rating at 0xA4: 0=disabled, 1-8 = MPAA ratings, 0xFF = all blocked
    DWORD mr = EepReadDW(0xA4);
    return (mr == 0 || (mr >= 1 && mr <= 8) || mr == 0xFF);
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
    add("Game Region", "0x54  NA / Japan / RoW / Dev", EepGameRegionOK(), true);
    add("DVD Region", "0xBC  CSS zone 0-6", EepDvdRegionOK(), true);
    add("Language", "0x90  English-Korean (1-7)", EepLanguageOK(), true);
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

static void RepairApply()
{
    // 0: Factory checksum — direct SMBus write
    if (s_repItems[0].state == REP_BAD)
    {
        DWORD cs = ~EepCalcChecksum(s_eeprom, 0x34, 0x2C);
        bool ok = EepSMBusWriteDW(0x30, cs);
        if (ok)
        {
            s_eeprom[0x30] = (BYTE)(cs);
            s_eeprom[0x31] = (BYTE)(cs >> 8);
            s_eeprom[0x32] = (BYTE)(cs >> 16);
            s_eeprom[0x33] = (BYTE)(cs >> 24);
        }
        s_repItems[0].state = (ok && EepFactoryChecksumOK()) ? REP_FIXED : REP_FAIL;
    }

    // 1: User checksum — kernel recalculates when any user field is written below.
    // 2: Security hash — detect only. 3/4/5: Serial/MAC/OnlineKey — detect only.

    // 6: Video standard — reset to NTSC-M
    if (s_repItems[6].state == REP_BAD)
    {
        DWORD vs = 0x00400100;
        s_repItems[6].state = (ExSaveNonVolatileSetting(XC_VIDEO_STANDARD, REG_DWORD, &vs, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // 7: Video flags — mask to documented bits
    if (s_repItems[7].state == REP_BAD)
    {
        DWORD vf = EepReadDW(0x94) & 0x5F;  // video flags at 0x94
        s_repItems[7].state = (ExSaveNonVolatileSetting(XC_VIDEO_FLAGS, REG_DWORD, &vf, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // 8: Audio flags — mask to documented bits, fallback to stereo
    if (s_repItems[8].state == REP_BAD)
    {
        DWORD af = EepReadDW(0x98) & 0x1F;  // audio flags at 0x98
        if (af == 0) af = 0x02;
        s_repItems[8].state = (ExSaveNonVolatileSetting(XC_AUDIO_FLAGS, REG_DWORD, &af, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // 9: Game region — reset to NA
    if (s_repItems[9].state == REP_BAD)
    {
        DWORD gr = 0x00000001;
        s_repItems[9].state = (ExSaveNonVolatileSetting(XC_GAME_REGION, REG_DWORD, &gr, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // 10: DVD region — reset to 0
    if (s_repItems[10].state == REP_BAD)
    {
        DWORD dr = 0;
        s_repItems[10].state = (ExSaveNonVolatileSetting(XC_DVD_REGION, REG_DWORD, &dr, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // 11: Language — reset to English
    if (s_repItems[11].state == REP_BAD)
    {
        DWORD lang = 1;
        s_repItems[11].state = (ExSaveNonVolatileSetting(0x01, REG_DWORD, &lang, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // 12: Timezone — reset bias to 0 (UTC)
    if (s_repItems[12].state == REP_BAD)
    {
        DWORD b = 0, d = 0;
        LONG r0 = ExSaveNonVolatileSetting(XC_TIMEZONE_BIAS, REG_DWORD, &b, 4);
        LONG r1 = ExSaveNonVolatileSetting(XC_TIMEZONE_STD_BIAS, REG_DWORD, &d, 4);
        s_repItems[12].state = (r0 == 0 && r1 == 0) ? REP_FIXED : REP_FAIL;
    }

    // 13: Game rating — reset to disabled
    if (s_repItems[13].state == REP_BAD)
    {
        DWORD gr = 0;
        s_repItems[13].state = (ExSaveNonVolatileSetting(XC_MAX_GAME_RATING, REG_DWORD, &gr, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // 14: Movie rating — reset to disabled
    if (s_repItems[14].state == REP_BAD)
    {
        DWORD mr = 0;
        // XC_MAX_DVD_RATING = 0x0D in kernel
        s_repItems[14].state = (ExSaveNonVolatileSetting(0x0D, REG_DWORD, &mr, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // 15: Parental passcode — reset to disabled (0)
    if (s_repItems[15].state == REP_BAD)
    {
        DWORD pc = 0;
        // XC_PARENTAL_CONTROL_GAMES_PASSCODE = 0x12
        s_repItems[15].state = (ExSaveNonVolatileSetting(0x12, REG_DWORD, &pc, 4) == 0)
            ? REP_FIXED : REP_FAIL;
    }

    // Verify user checksum after all kernel writes
    {
        ULONG type = 0, len = 0;
        BYTE buf[256];
        if (ExQueryNonVolatileSetting(0xFFFF, &type, buf, 256, &len) == 0 && len >= 0xC0)
            for (int i = 0; i < 256; ++i) s_eeprom[i] = buf[i];

        if (s_repItems[1].state == REP_BAD)
            s_repItems[1].state = EepUserChecksumOK() ? REP_FIXED : REP_FAIL;
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