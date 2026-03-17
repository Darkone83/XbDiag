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


// Hex view column positions (640 design space)
static const float HEX_OFF_X = EEP_LM2;           // offset label "AA"
static const float HEX_B0_X = EEP_LM2 + 28.f;   // first group of 8 bytes
static const float HEX_B8_X = EEP_LM2 + 193.f;  // second group — was 164, pushed right to clear B0 at SD advance
static const float HEX_ASC_X = EEP_LM2 + 358.f;  // ASCII — was 302, pushed right to clear B8 at SD advance
static const float HEX_FLD_Y = BOT_BAR_Y - 14.f; // field label strip above bot bar

// Decoded view columns
static const float DEC_LBL_X = EEP_LM2;
static const float DEC_HEX_X = EEP_LM2 + 130.f;
static const float DEC_VAL_X = EEP_LM2 + 310.f;
static const int   DEC_VISIBLE = 20; // rows visible at once — always scroll, never overflow

const EepField s_fields[] =
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
const int NUM_FIELDS = sizeof(s_fields) / sizeof(s_fields[0]);

// ============================================================================
// State
// ============================================================================

BYTE  s_eeprom[256];
bool  s_readOK;
WORD  s_prevBtns;

EepView s_eepView;
bool    s_repRan = false;
bool    s_repConfirm = false;
bool    s_binExists = false;
bool    s_restoreDone = false;
bool    s_restoreOK = false;
bool    s_restoreConfirm = false;
static int     s_hexCurRow;   // 0-15, highlighted row in hex view
static int     s_decScroll;   // top field index in decoded view
static bool    s_saveDone;
static bool    s_saveOK;
static bool    s_loaded = false;

// ============================================================================
// Helpers
// ============================================================================


#include "EepromCrypto.h"
#include "EepromSettings.h"
#include "EepromRepair.h"


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
            EepSafeCopy(out, outLen, "(kernel read failed)");
            break;
        }
        if (gr == 0x00000001) EepSafeCopy(out, outLen, "N. America  (0x00000001)");
        else if (gr == 0x00000002) EepSafeCopy(out, outLen, "Japan       (0x00000002)");
        else if (gr == 0x00000004) EepSafeCopy(out, outLen, "Rest of World (0x00000004)");
        else if (gr == 0x80000000) EepSafeCopy(out, outLen, "Manufacturing (0x80000000)");
        else if (gr == 0xFFFFFFFF) EepSafeCopy(out, outLen, "All / Debug (0xFFFFFFFF)");
        else
        {
            EepSafeCopy(out, outLen, "Unknown 0x");
            IntToHex(gr, 8, t, sizeof(t)); EepSafeAppend(out, outLen, t);
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
            EepSafeAppend(out, outLen, t);
        }
        break;
    }

    // MAC address
    case 0x40:
        EepFmtMAC(p, out, outLen);
        break;

        // Online Key at 0x48 — 16-byte Xbox Live provisioning key, show hex
        // (no special decode — any non-zero value just means "provisioned")

        // Video Standard at 0x58 — XC_VIDEO_STANDARD_* DWORD constants
    case 0x58:
    {
        DWORD v = EepReadDW(0x58);
        if (v == 0x00400100) EepSafeCopy(out, outLen, "NTSC-M (N. America)");
        else if (v == 0x00400200) EepSafeCopy(out, outLen, "NTSC-J (Japan)");
        else if (v == 0x00800300) EepSafeCopy(out, outLen, "PAL-I (Europe/AUS)");
        else if (v == 0x00400400) EepSafeCopy(out, outLen, "PAL-M (Brazil)");
        else
        {
            EepSafeCopy(out, outLen, "0x");
            IntToHex(v, 8, t, sizeof(t)); EepSafeAppend(out, outLen, t);
        }
        break;
    }

    // Video Flags — XC_VIDEO_FLAGS_* at 0x94 (user section)
    // Bits are in the HIGH word: bit16=widescreen, bit17=720p, bit18=1080i,
    // bit19=480p, bit20=letterbox, bit23=PAL60. Valid mask: 0x005F0000.
    case 0x94:
    {
        DWORD v = EepReadDW(0x94);
        bool any = false;
        if (v & 0x00010000) { EepSafeAppend(out, outLen, "WIDE");    any = true; }
        if (v & 0x00020000) { if (any) EepSafeAppend(out, outLen, " "); EepSafeAppend(out, outLen, "720p");  any = true; }
        if (v & 0x00040000) { if (any) EepSafeAppend(out, outLen, " "); EepSafeAppend(out, outLen, "1080i"); any = true; }
        if (v & 0x00080000) { if (any) EepSafeAppend(out, outLen, " "); EepSafeAppend(out, outLen, "480p");  any = true; }
        if (v & 0x00100000) { if (any) EepSafeAppend(out, outLen, " "); EepSafeAppend(out, outLen, "LBOX");  any = true; }
        if (v & 0x00400000) { if (any) EepSafeAppend(out, outLen, " "); EepSafeAppend(out, outLen, "PAL60"); any = true; }
        if (!any) EepSafeCopy(out, outLen, "STANDARD (no HDTV)");
        if (v & ~0x005F0000UL) { EepSafeAppend(out, outLen, " !UNK"); }
        break;
    }

    // Audio Flags — XC_AUDIO_FLAGS_* at 0x98 (user section)
    // bit0=mono, bit1=surround (0=stereo), bit16=AC3, bit17=DTS.
    case 0x98:
    {
        DWORD v = EepReadDW(0x98);
        if (v & 0x01) EepSafeCopy(out, outLen, "Mono");
        else if (v & 0x02) EepSafeCopy(out, outLen, "Surround");
        else EepSafeCopy(out, outLen, "Stereo");
        if (v & 0x00010000) EepSafeAppend(out, outLen, " +AC3");
        if (v & 0x00020000) EepSafeAppend(out, outLen, " +DTS");
        if (v & ~0x00030003UL) EepSafeAppend(out, outLen, " !UNK");
        break;
    }

    // Language — XC_LANGUAGE enum at 0x90 (user section)
    // 0=Neutral/Unknown, 1=English, 2=Japanese, 3=German, 4=French,
    // 5=Spanish, 6=Italian, 7=Korean, 8=Chinese, 9=Portuguese
    case 0x90:
    {
        DWORD v = EepReadDW(0x90);
        switch (v)
        {
        case 0: EepSafeCopy(out, outLen, "Neutral / Unknown"); break;
        case 1: EepSafeCopy(out, outLen, "English");           break;
        case 2: EepSafeCopy(out, outLen, "Japanese");          break;
        case 3: EepSafeCopy(out, outLen, "German");            break;
        case 4: EepSafeCopy(out, outLen, "French");            break;
        case 5: EepSafeCopy(out, outLen, "Spanish");           break;
        case 6: EepSafeCopy(out, outLen, "Italian");           break;
        case 7: EepSafeCopy(out, outLen, "Korean");            break;
        case 8: EepSafeCopy(out, outLen, "Chinese");           break;
        case 9: EepSafeCopy(out, outLen, "Portuguese");        break;
        default: EepFmtDword(v, out, outLen);                  break;
        }
        break;
    }

    // Parental Passcode — nibble-encoded D-pad sequence at 0xA0
    // Each nibble: 0=none 1=up 2=down 3=left 4=right
    // All-zero = disabled
    case 0xA0:
    {
        DWORD v = EepReadDW(0xA0);
        if (v == 0) { EepSafeCopy(out, outLen, "Disabled"); break; }
        static const char* dirs[5] = { ".", "U", "D", "L", "R" };
        bool any = false;
        for (int ni = 0; ni < 8; ++ni)
        {
            BYTE nib = (BYTE)((v >> (ni * 4)) & 0xF);
            if (nib == 0) break;
            if (any) EepSafeAppend(out, outLen, " ");
            EepSafeAppend(out, outLen, nib <= 4 ? dirs[nib] : "?");
            any = true;
        }
        break;
    }

    // Movie Rating — MPAA max at 0xA4 (XboxEepromEditor MovieRating enum)
    // Values: NR=0, NC-17=1, R=2, (3 unused), PG-13=4, PG=5, (6 unused), G=7
    // 0xFF = all movies blocked
    case 0xA4:
    {
        DWORD v = EepReadDW(0xA4);
        switch (v)
        {
        case 0:    EepSafeCopy(out, outLen, "Disabled (All)"); break;
        case 1:    EepSafeCopy(out, outLen, "NC-17");          break;
        case 2:    EepSafeCopy(out, outLen, "R");              break;
        case 4:    EepSafeCopy(out, outLen, "PG-13");          break;
        case 5:    EepSafeCopy(out, outLen, "PG");             break;
        case 7:    EepSafeCopy(out, outLen, "G");              break;
        case 0xFF: EepSafeCopy(out, outLen, "All Blocked");    break;
        default:   EepFmtDword(v, out, outLen);                break;
        }
        break;
    }

    // Xbox Live network settings (0xA8-0xB7) — stored as network-byte-order IPs.
    // The EEPROM stores the 4 octets in big-endian order:
    //   s_eeprom[off]=a, [off+1]=b, [off+2]=c, [off+3]=d → "a.b.c.d"
    // EepReadDW is little-endian so: EepReadDW(off)=0xDDCCBBAA → v&0xFF=a, v>>8&0xFF=b ...
    // All-zeros = DHCP / not configured.
    case 0xA8:  // Live IP
    case 0xAC:  // Live DNS
    case 0xB0:  // Live Gateway
    case 0xB4:  // Live Subnet
    {
        DWORD v = EepReadDW(f.offset);
        if (v == 0)
        {
            // Subnet mask 0.0.0.0 means DHCP for the IP fields; show clearly
            EepSafeCopy(out, outLen, f.offset == 0xB4 ? "0.0.0.0" : "DHCP (0.0.0.0)");
            break;
        }
        // Format each octet from the LE DWORD
        // v = (d<<24)|(c<<16)|(b<<8)|a  where a.b.c.d is the dotted form
        BYTE a = (BYTE)(v & 0xFF);
        BYTE b = (BYTE)((v >> 8) & 0xFF);
        BYTE c = (BYTE)((v >> 16) & 0xFF);
        BYTE d = (BYTE)((v >> 24) & 0xFF);
        IntToStr(a, t, sizeof(t)); EepSafeAppend(out, outLen, t);
        EepSafeAppend(out, outLen, ".");
        IntToStr(b, t, sizeof(t)); EepSafeAppend(out, outLen, t);
        EepSafeAppend(out, outLen, ".");
        IntToStr(c, t, sizeof(t)); EepSafeAppend(out, outLen, t);
        EepSafeAppend(out, outLen, ".");
        IntToStr(d, t, sizeof(t)); EepSafeAppend(out, outLen, t);
        break;
    }
    case 0xBC:
    {
        DWORD v = EepReadDW(0xBC);
        if (v == 0x00000000) EepSafeCopy(out, outLen, "None (Region Free)");
        else if (v == 0x00000001) EepSafeCopy(out, outLen, "Region 1 (USA/CA)");
        else if (v == 0x00000002) EepSafeCopy(out, outLen, "Region 2 (EUR/JPN)");
        else if (v == 0x00000003) EepSafeCopy(out, outLen, "Region 3 (SE Asia)");
        else if (v == 0x00000004) EepSafeCopy(out, outLen, "Region 4 (AUS/LAT)");
        else if (v == 0x00000005) EepSafeCopy(out, outLen, "Region 5 (Russia)");
        else if (v == 0x00000006) EepSafeCopy(out, outLen, "Region 6 (China)");
        else EepFmtDword(v, out, outLen);
        break;
    }

    // Game rating — at 0x9C per XboxEepromEditor (GameRating enum)
    case 0x9C:
    {
        DWORD v = EepReadDW(0x9C);
        if (v == 0)    EepSafeCopy(out, outLen, "Disabled (All ages)");
        else if (v == 1) EepSafeCopy(out, outLen, "Adults Only (AO)");
        else if (v == 2) EepSafeCopy(out, outLen, "Mature (M)");
        else if (v == 3) EepSafeCopy(out, outLen, "Teen (T)");
        else if (v == 4) EepSafeCopy(out, outLen, "Everyone 10+ (E10)");
        else if (v == 5) EepSafeCopy(out, outLen, "Everyone (E)");
        else if (v == 6) EepSafeCopy(out, outLen, "Early Childhood (EC)");
        else if (v == 7) EepSafeCopy(out, outLen, "Rating Pending (RP)");
        else if (v == 8) EepSafeCopy(out, outLen, "No Rating");
        else if (v == 0xFF) EepSafeCopy(out, outLen, "All Blocked");
        else EepFmtDword(v, out, outLen);
        break;
    }

    // TZ bias — int32 minutes-west of UTC (0x64); 0x88 = std bias, 0x8C = DST bias
    case 0x64:
    case 0x88:
    case 0x8C:
    {
        int bias = (int)EepReadDW(f.offset);
        if (bias == 0)
        {
            EepSafeCopy(out, outLen, "UTC+0");
            break;
        }
        // Kernel stores bias as minutes-WEST (subtract to get local).
        // Negate for human-readable UTC+ display.
        int display = -bias;
        int hrs = display / 60;
        int mins = display % 60;
        if (mins < 0) mins = -mins;
        EepSafeCopy(out, outLen, "UTC");
        if (hrs >= 0) EepSafeAppend(out, outLen, "+");
        IntToStr(hrs, t, sizeof(t)); EepSafeAppend(out, outLen, t);
        if (mins)
        {
            EepSafeAppend(out, outLen, ":");
            if (mins < 10) EepSafeAppend(out, outLen, "0");
            IntToStr(mins, t, sizeof(t)); EepSafeAppend(out, outLen, t);
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
            EepSafeAppend(out, outLen, t);
        }
        if (out[0] == '\0') EepSafeCopy(out, outLen, "(not set)");
        break;
    }

    // TZ transition dates — packed: month/week/day/hour
    case 0x78:
    case 0x7C:
    {
        DWORD v = EepReadDW(f.offset);
        if (v == 0)
        {
            EepSafeCopy(out, outLen, "(not set)");
            break;
        }
        // Standard SYSTEMTIME packing used by kernel
        BYTE month = (BYTE)(v & 0xFF);
        BYTE week = (BYTE)((v >> 8) & 0xFF);
        BYTE dow = (BYTE)((v >> 16) & 0xFF);
        BYTE hour = (BYTE)((v >> 24) & 0xFF);
        static const char* months[] = { "","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
        const char* mstr = (month >= 1 && month <= 12) ? months[month] : "?";
        EepSafeCopy(out, outLen, mstr);
        EepSafeAppend(out, outLen, " wk");
        IntToStr(week, t, sizeof(t)); EepSafeAppend(out, outLen, t);
        EepSafeAppend(out, outLen, " h");
        IntToStr(hour, t, sizeof(t)); EepSafeAppend(out, outLen, t);
        break;
    }

    // Last boot time — FILETIME
    case 0x80:
    {
        DWORD lo = EepReadDW(0x80);
        DWORD hi = EepReadDW(0x84);
        if (lo == 0 && hi == 0)
            EepSafeCopy(out, outLen, "Never / Not Set");
        else
        {
            // Convert FILETIME to approximate year (rough: hi*429 / 10^7 seconds since 1601)
            // hi * 429.4967296 seconds-worth of high 32-bits
            // Approx year: 1601 + (hi * 429 / 31536000)
            DWORD approxYear = 1601 + (hi * 429 / 31536000);
            EepSafeCopy(out, outLen, "~");
            IntToStr(approxYear, t, sizeof(t)); EepSafeAppend(out, outLen, t);
            EepSafeAppend(out, outLen, " (hi:");
            EepFmtDword(hi, t, sizeof(t)); EepSafeAppend(out, outLen, t);
            EepSafeAppend(out, outLen, ")");
        }
        break;
    }

    // Everything else: short fields show raw hex, long fields defer to hex view
    default:
    {
        if (f.size <= 8)
            EepFmtHex(p, f.size, out, outLen);
        else
            EepSafeCopy(out, outLen, "(see hex view)");
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

void EepromView_OnEnter()
{
    s_prevBtns = GetButtons();  // seed to prevent held buttons from firing as edges
    s_eepView = VIEW_DECODED;
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
        DrawText(EEP_LM2, EEP_CY, "EEPROM READ FAILED (SMBus 0x54 NAK)", 1.2f, COL_RED);
        return;
    }

    // Column headers
    float hy = EEP_CY;
    DrawText(HEX_OFF_X, hy, "OFF", 1.1f, COL_GRAY);
    DrawText(HEX_B0_X, hy, "00 01 02 03 04 05 06 07", 1.05f, COL_GRAY);
    DrawText(HEX_B8_X, hy, "08 09 0A 0B 0C 0D 0E 0F", 1.05f, COL_GRAY);
    DrawText(HEX_ASC_X, hy, "ASCII", 1.1f, COL_GRAY);
    hy += EEP_LH - 2.f;
    HLine(hy, EEP_LM2, SW - LM, COL_BORDER);
    hy += 3.f;

    for (int row = 0; row < 16; ++row)
    {
        float rowY = hy + row * EEP_LH;
        bool  sel = (row == s_hexCurRow);

        if (sel)
            FillRect(EEP_LM2 - 2.f, rowY - 1.f, SW - LM, rowY + EEP_LH, 0x28204040);

        // Offset label
        char offStr[6]; offStr[0] = '0'; offStr[1] = 'x';
        IntToHex(row * 16, 2, offStr + 2, 4); offStr[4] = '\0';
        DrawText(HEX_OFF_X, rowY, offStr, 1.05f, sel ? COL_YELLOW : COL_GRAY);

        // Bytes — two groups of 8
        const BYTE* rowData = s_eeprom + row * 16;
        char hexStr[28];

        EepFmtHex(rowData, 8, hexStr, sizeof(hexStr));
        DrawText(HEX_B0_X, rowY, hexStr, 1.05f, sel ? COL_WHITE : COL_CYAN);

        EepFmtHex(rowData + 8, 8, hexStr, sizeof(hexStr));
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
        EepSafeCopy(strip, sizeof(strip), "FIELD: ");
        EepSafeAppend(strip, sizeof(strip), s_fields[fi].shortName);
        EepSafeAppend(strip, sizeof(strip), "  [0x");
        char t[6];
        IntToHex(s_fields[fi].offset, 2, t, sizeof(t));
        EepSafeAppend(strip, sizeof(strip), t);
        EepSafeAppend(strip, sizeof(strip), " +");
        IntToStr(s_fields[fi].size, t, sizeof(t));
        EepSafeAppend(strip, sizeof(strip), t);
        EepSafeAppend(strip, sizeof(strip), "B]");
        DrawText(EEP_LM2, HEX_FLD_Y, strip, 1.2f, COL_YELLOW);
    }
    else
    {
        DrawText(EEP_LM2, HEX_FLD_Y, "FIELD: (reserved / unassigned)", 1.2f, COL_GRAY);
    }
}

// ============================================================================
// Render: DECODED VIEW
// ============================================================================

static void RenderDecoded(const DiagLogo& logo)
{
    const char* hint = s_saveDone
        ? (s_saveOK ? "[A] Save OK  [Right] Hex  [Y] Edit  [X] Repair  [B] Back"
            : "[A] Save FAIL  [Right] Hex  [Y] Edit  [X] Repair  [B] Back")
        : "[A] Save  [Right] Hex  [Y] Edit  [X] Repair  [B] Back";
    static char s_decHint[128];
    if (s_binExists)
    {
        StrCopy(s_decHint, sizeof(s_decHint), hint);
        StrCat2(s_decHint, sizeof(s_decHint), s_decHint, "  [White]");
        hint = s_decHint;
    }

    DrawPageChrome(logo, "EEPROM - DECODED VIEW", hint);

    if (!s_readOK)
    {
        DrawText(EEP_LM2, EEP_CY, "EEPROM READ FAILED (SMBus 0x54 NAK)", 1.2f, COL_RED);
        return;
    }

    // Column headers
    float hy = EEP_CY;
    DrawText(DEC_LBL_X, hy, "FIELD", 1.2f, COL_GRAY);
    DrawText(DEC_HEX_X, hy, "HEX", 1.2f, COL_GRAY);
    DrawText(DEC_VAL_X, hy, "DECODED", 1.2f, COL_GRAY);
    hy += EEP_LH - 2.f;
    HLine(hy, EEP_LM2, SW - LM, COL_BORDER);
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
        float           rowY = hy + (fi - scrollTop) * EEP_LH;

        // Field name
        DrawText(DEC_LBL_X, rowY, f.name, 1.15f, COL_YELLOW);

        // Hex bytes — truncate long fields to 8 bytes displayed inline
        char hexStr[28];
        int  showBytes = f.size > 8 ? 8 : f.size;
        EepFmtHex(p, showBytes, hexStr, sizeof(hexStr));
        if (f.size > 8) EepSafeAppend(hexStr, sizeof(hexStr), "..");
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
        float sbH = DEC_VISIBLE * EEP_LH;
        float thumbH = sbH * DEC_VISIBLE / (float)NUM_FIELDS;
        float thumbY = sbTop + sbH * s_decScroll / (float)NUM_FIELDS;
        VLine(SW - LM - 4.f, sbTop, sbTop + sbH, COL_BORDER);
        FillRect(SW - LM - 6.f, thumbY, SW - LM - 2.f, thumbY + thumbH, COL_CYAN);
    }
}

// ============================================================================
// ============================================================================
// ============================================================================


void EepromView_Reload()
{
    ReadEeprom();
    EepromRepair_BuildDiag();
}

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
        EepromRepair_BuildDiag();
        s_loaded = true;
        return;
    }

    WORD cur = GetButtons();

    // Global B/Back — not when edit or repair own input
    if (s_eepView != VIEW_EDIT && s_eepView != VIEW_REPAIR)
    {
        if (EepEdgeDown(cur, s_prevBtns, BTN_B) || EepEdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            RequestState(MSTATE_MENU);
            s_prevBtns = cur;
            return;
        }
    }

    if (s_eepView == VIEW_REPAIR)
    {
        EepromRepair_HandleInput(cur, s_prevBtns, logo);
        s_prevBtns = cur;
        EepromRepair_Render(logo);  // repair view owns its own BeginScene/EndScene/Present
        return;
    }
    else if (s_eepView == VIEW_EDIT)
    {
        // Edit view owns all input
        EepromSettings_HandleInput(cur, s_prevBtns);
    }
    else
    {
        // Left/Right only switches views when NOT in edit mode
        if (EepEdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
            s_eepView = VIEW_DECODED;

        if (EepEdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT))
            s_eepView = VIEW_HEX;

        if (EepEdgeDown(cur, s_prevBtns, BTN_A))
            SaveBin();

        // X enters repair view from decoded
        if (s_eepView == VIEW_DECODED && EepEdgeDown(cur, s_prevBtns, BTN_X))
            s_eepView = VIEW_REPAIR;

        // White enters restore-from-file flow (only if eeprom.bin exists)
        if (s_eepView == VIEW_DECODED && s_binExists
            && EepEdgeDown(cur, s_prevBtns, BTN_WHITE))
        {
            s_restoreConfirm = true;
            s_restoreDone = false;
            s_repRan = false;
            s_eepView = VIEW_REPAIR;
        }

        // Y enters edit view from decoded
        if (s_eepView == VIEW_DECODED && EepEdgeDown(cur, s_prevBtns, BTN_Y))
        {
            EepromSettings_Load();
            s_eepView = VIEW_EDIT;
            s_prevBtns = cur;
            return;
        }

        if (s_eepView == VIEW_HEX)
        {
            if (EepEdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
            {
                if (s_hexCurRow > 0)  --s_hexCurRow;
            }
            if (EepEdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
            {
                if (s_hexCurRow < 15) ++s_hexCurRow;
            }
        }
        else
        {
            if (EepEdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
            {
                if (s_decScroll > 0) --s_decScroll;
            }
            if (EepEdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
            {
                if (s_decScroll < NUM_FIELDS - DEC_VISIBLE) ++s_decScroll;
            }
        }
    }

    s_prevBtns = cur;

    g_pDevice->BeginScene();
    if (s_eepView == VIEW_HEX)
        RenderHex(logo);
    else if (s_eepView == VIEW_EDIT)
        EepromSettings_Render(logo);
    else if (s_eepView == VIEW_REPAIR)
        EepromRepair_Render(logo);
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