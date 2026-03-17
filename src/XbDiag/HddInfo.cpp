// HddInfo.cpp
// XbDiag - HDD & Security Info
//
// Layout (640x480 design space):
//
//  [TOP BAR]
//  ---------------------------------------------------------------
//  LEFT COLUMN                       RIGHT COLUMN
//
//  DRIVE (ATA IDENTIFY)              SECURITY (EEPROM SMBus 0x54)
//  Model                             Serial No   (12 ASCII digits)
//  Serial                            Region      (NTSC-M/J, PAL)
//  Firmware rev                      HDD Key     (16 bytes hex, decrypted)
//  Capacity                          XBE Region  (4 bytes hex, decrypted)
//  Interface (UDMA mode)             Online Key  (16 bytes hex)
//  Buffer size
//  LBA28 / LBA48 sectors
//  Security state (locked/unlocked)
//
//  [BOT BAR]  [A] Export    [Right] SMART    [RT] Bench    [B] Back
//
// UDMA detection:
//   Word 53 bit 2 = validity gate for word 88 (ATA spec).
//   Xbox MCPX programs DMA timing in PCI registers without SET FEATURES —
//   active bits (word 88 bits 14:8) are usually zero even when UDMA is
//   running. When word 53 bit 2 is set and supported bits are non-zero,
//   highest supported mode is reported cleanly (no marker).
//   '?' = word 53 bit 2 clear, word 88 not spec-guaranteed.
//
// EEPROM / HDD Key:
//   ExQueryNonVolatileSetting(0xFFFF) returns raw physical EEPROM bytes.
//   Security section (0x14-0x2F, 28 bytes) is RC4-encrypted on the chip.
//   EepromDecryptSecurity() implements the Xbox custom two-pass SHA1 + RC4
//   decryption (ref: XboxEepromEditor HmacSha1.cs / RC4.cs) and tries all
//   four hardware versions (Debug / RetailFirst 1.0 / RetailMiddle 1.1-1.4 /
//   RetailLast 1.6), verifying via HMAC re-hash before accepting.
//   Factory/user sections (0x30+, serial/MAC/online key) are plaintext.
//
// ATA IDENTIFY (port 0x1F0, primary channel, master):
//   Word 10-19  = serial number (byte-swapped ASCII)
//   Word 23-26  = firmware revision (byte-swapped ASCII)
//   Word 27-46  = model string (byte-swapped ASCII)
//   Word 49     = capabilities (LBA support bit 9)
//   Word 53     = validity flags (bit 2 = word 88 valid)
//   Word 60-61  = LBA28 addressable sectors
//   Word 63     = multiword DMA modes
//   Word 80     = ATA standard version
//   Word 83     = LBA48 support (bit 10)
//   Word 88     = UDMA modes (bits 6:0 supported, bits 14:8 active)
//   Word 100-103= LBA48 addressable sectors (64-bit)
//   Word 128    = security status register
//   Word 217    = nominal media rotation rate (0x0001 = SSD)
//
// EEPROM layout (SMBus 0x54, 93LC56 256-byte):
//   0x00-0x13   HMAC SHA1 hash       (20 bytes)
//   0x14-0x1B   Confounder           (8 bytes)   [RC4-encrypted]
//   0x1C-0x2B   HDD key              (16 bytes)  [RC4-encrypted]
//   0x2C-0x2F   XBE region code      (4 bytes)   [RC4-encrypted]
//   0x30-0x33   Checksum2            (4 bytes)
//   0x34-0x3F   Serial number        (12 bytes ASCII)
//   0x40-0x45   MAC address          (6 bytes)
//   0x46-0x47   Unknown padding      (2 bytes)
//   0x48-0x57   Online key           (16 bytes)
//   0x58-0x5B   Video standard       (4 bytes)
//
// Region decode (XBERegion[0]):
//   0x01 = NTSC-M (North America)
//   0x02 = NTSC-J (Japan)
//   0x04 = PAL    (Europe/Australia)

#include "HddInfo.h"
#include "font.h"
#include "input.h"
#include <xtl.h>
#include "HddSmart.h"
#include "HddBench.h"

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// ATA port definitions
// ============================================================================

#define ATA_REG_DATA    0x1F0
#define ATA_REG_ERROR   0x1F1
#define ATA_REG_NSECT   0x1F2
#define ATA_REG_LBAL    0x1F3
#define ATA_REG_LBAM    0x1F4
#define ATA_REG_LBAH    0x1F5
#define ATA_REG_DEVICE  0x1F6
#define ATA_REG_STATUS  0x1F7
#define ATA_REG_CMD     0x1F7

// Secondary ATA channel (DVD drive) — defined but detection not implemented on CerbIOS
#define ATA2_REG_DATA   0x170
#define ATA2_REG_DEVICE 0x176
#define ATA2_REG_STATUS 0x177
#define ATA2_REG_CMD    0x177

#define ATA_SR_BSY      0x80
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_SMART     0xB0
#define SMART_SUBCMD_READ 0xD0   // SMART READ DATA — 512 bytes, 30 attributes
#define SMART_LBA_MID     0x4F   // magic values required by ATA spec for SMART
#define SMART_LBA_HI      0xC2
#define SMART_MAX_ATTRS   30
#define SMART_ATTR_SIZE   12     // bytes per attribute entry in the 512-byte blob

// ============================================================================
// Layout
// ============================================================================

static const float CY = CONTENT_Y + 6.f;
static const float COL_L = LM;
static const float COL_R = SW * 0.5f + 4.f;
static const float COL_VL = COL_L + 116.f;   // value x for left col
static const float COL_VR = COL_R + 116.f;   // value x for right col
static const float COL_RW = SW - LM - COL_R; // right col width
static const float LH = LINE_H - 1.f;
static const float GAP = 7.f;

HddData s_data;
static WORD    s_prevBtns;
static bool    s_loaded;
HddView s_view;


// ============================================================================
// Helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// Format a byte array as space-separated hex pairs, max outLen chars
static void FormatHex(const BYTE* data, int count, char* out, int outLen)
{
    out[0] = '\0';
    char t[4];
    for (int i = 0; i < count; ++i)
    {
        if (i > 0 && (outLen - (int)StrLen(out)) > 3)
            StrCat2(out, outLen, out, " ");
        IntToHex(data[i], 2, t, sizeof(t));
        if ((outLen - (int)StrLen(out)) > 2)
            StrCat2(out, outLen, out, t);
    }
}

// ============================================================================
// ATA helpers
// ============================================================================

extern "C" VOID __stdcall KeStallExecutionProcessor(ULONG Microseconds);
extern "C" LONG __stdcall ExQueryNonVolatileSetting(
    ULONG ValueIndex, ULONG* Type, void* Value,
    ULONG ValueLength, ULONG* ResultLength);

// Drive letter mounting — required before any Win32 file I/O on HDD partitions
typedef struct _XBOX_STRING {
    USHORT Length;
    USHORT MaximumLength;
    char* Buffer;
} XBOX_STRING;
extern "C" LONG WINAPI IoCreateSymbolicLink(XBOX_STRING* symLink, XBOX_STRING* target);

static void MountHddDrives()
{
    static const struct { const char* letter; const char* device; } k[] =
    {
        { "C", "\\Device\\Harddisk0\\Partition2" },
        { "E", "\\Device\\Harddisk0\\Partition1" },
        { "F", "\\Device\\Harddisk0\\Partition6" },
        { "G", "\\Device\\Harddisk0\\Partition7" },
    };
    char linkBuf[8];
    for (int i = 0; i < 4; ++i)
    {
        linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
        linkBuf[4] = k[i].letter[0];
        linkBuf[5] = ':'; linkBuf[6] = '\0';
        const char* dev = k[i].device;
        int devLen = 0; while (dev[devLen]) devLen++;
        XBOX_STRING sLink = { 6, 7, linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), (char*)dev };
        IoCreateSymbolicLink(&sLink, &sDev);
    }
}


static bool AtaWaitReady(DWORD msTimeout)
{
    DWORD t0 = GetTickCount();
    BYTE  sr = 0;
    do {
        __asm { mov dx, ATA_REG_STATUS }
        __asm { in  al, dx             }
        __asm { mov sr, al             }
        // Only check BSY+DRDY — do NOT abort on ERR here; stale ERR from a
        // prior command must not cause a false abort before IDENTIFY issues.
        if (!(sr & ATA_SR_BSY) && (sr & 0x40)) return true;
        KeStallExecutionProcessor(1);
    } while ((GetTickCount() - t0) < msTimeout);
    return false;
}

static bool AtaIdentify(WORD buf[256])
{
    // Select master drive
    BYTE devsel = 0xA0;  // ATA spec: bits[7,5] fixed, bit[4]=0 for master (ref: PrometheOS IDE_DEVICE_MASTER)
    __asm { mov dx, ATA_REG_DEVICE }
    __asm { mov al, devsel         }
    __asm { out dx, al             }

    // Settle: 5 status reads (400ns rule per ATA spec)
    for (int i = 0; i < 5; ++i)
    {
        BYTE dummy = 0;
        __asm { mov dx, ATA_REG_STATUS }
        __asm { in  al, dx             }
        __asm { mov dummy, al          }
    }

    if (!AtaWaitReady(2000)) return false;

    BYTE cmd = ATA_CMD_IDENTIFY;
    __asm { mov dx, ATA_REG_CMD }
    __asm { mov al, cmd         }
    __asm { out dx, al          }

    // Wait for DRQ - check BSY before ERR/DRQ, 2000ms hard timeout
    DWORD t0 = GetTickCount();
    bool  drq = false;
    while ((GetTickCount() - t0) < 2000)
    {
        BYTE sr = 0;
        __asm { mov dx, ATA_REG_STATUS }
        __asm { in  al, dx             }
        __asm { mov sr, al             }
        if (sr & ATA_SR_BSY) { KeStallExecutionProcessor(100); continue; }
        if (sr & ATA_SR_ERR) return false;
        if (sr & ATA_SR_DRQ) { drq = true; break; }
        KeStallExecutionProcessor(50);
    }
    if (!drq) return false;

    // Read 256 words
    for (int i = 0; i < 256; ++i)
    {
        WORD w = 0;
        __asm { mov dx, ATA_REG_DATA }
        __asm { in  ax, dx           }
        __asm { mov w, ax           }
        buf[i] = w;
    }
    return true;
}

// Issue ATA SMART READ DATA (0xB0 / 0xD0) — fills buf[512].
// Returns false if drive NAKs, times out, or SMART is not supported.
static bool SmartReadData(BYTE buf[512])
{
    // Select master drive (same as AtaIdentify)
    BYTE devsel = 0xA0;
    __asm { mov dx, ATA_REG_DEVICE }
    __asm { mov al, devsel         }
    __asm { out dx, al             }

    // Settle: 5 status reads
    for (int i = 0; i < 5; ++i)
    {
        BYTE dummy = 0;
        __asm { mov dx, ATA_REG_STATUS }
        __asm { in  al, dx             }
        __asm { mov dummy, al          }
    }

    if (!AtaWaitReady(2000)) return false;

    // Feature register (0x1F1 write) = SMART subcommand 0xD0
    BYTE feat = SMART_SUBCMD_READ;
    __asm { mov dx, ATA_REG_ERROR }
    __asm { mov al, feat          }
    __asm { out dx, al            }

    // Sector count = 0
    BYTE zero = 0;
    __asm { mov dx, ATA_REG_NSECT }
    __asm { mov al, zero          }
    __asm { out dx, al            }

    // LBA low = 0
    __asm { mov dx, ATA_REG_LBAL }
    __asm { mov al, zero         }
    __asm { out dx, al           }

    // LBA mid = 0x4F (SMART magic, required by ATA spec)
    BYTE lbam = SMART_LBA_MID;
    __asm { mov dx, ATA_REG_LBAM }
    __asm { mov al, lbam         }
    __asm { out dx, al           }

    // LBA high = 0xC2 (SMART magic, required by ATA spec)
    BYTE lbah = SMART_LBA_HI;
    __asm { mov dx, ATA_REG_LBAH }
    __asm { mov al, lbah         }
    __asm { out dx, al           }

    // Issue SMART command
    BYTE cmd = ATA_CMD_SMART;
    __asm { mov dx, ATA_REG_CMD }
    __asm { mov al, cmd         }
    __asm { out dx, al          }

    // Wait for DRQ
    DWORD t0 = GetTickCount();
    bool drq = false;
    while ((GetTickCount() - t0) < 2000)
    {
        BYTE sr = 0;
        __asm { mov dx, ATA_REG_STATUS }
        __asm { in  al, dx             }
        __asm { mov sr, al             }
        if (sr & ATA_SR_BSY) { KeStallExecutionProcessor(100); continue; }
        if (sr & ATA_SR_ERR) return false;
        if (sr & ATA_SR_DRQ) { drq = true; break; }
        KeStallExecutionProcessor(50);
    }
    if (!drq) return false;

    // Read 256 words (512 bytes)
    WORD* wp = (WORD*)buf;
    for (int i = 0; i < 256; ++i)
    {
        WORD w = 0;
        __asm { mov dx, ATA_REG_DATA }
        __asm { in  ax, dx           }
        __asm { mov w, ax            }
        wp[i] = w;
    }
    return true;
}

// Return a human-readable name for a SMART attribute ID.
// Returns NULL for unknown IDs (caller shows raw ID hex).
// Byte-swap ATA string words into null-terminated ASCII, trim trailing spaces
static void AtaStr(const WORD* words, int nWords, char* out, int outLen)
{
    int pos = 0;
    for (int i = 0; i < nWords && pos < outLen - 1; ++i)
    {
        char hi = (char)(words[i] >> 8);
        char lo = (char)(words[i] & 0xFF);
        if (pos < outLen - 1) out[pos++] = hi;
        if (pos < outLen - 1) out[pos++] = lo;
    }
    out[pos] = '\0';
    // Trim trailing spaces
    for (int i = pos - 1; i >= 0 && out[i] == ' '; --i)
        out[i] = '\0';
}

// ============================================================================
// Xbox EEPROM Security Section Decryption
// ============================================================================
//
// ExQueryNonVolatileSetting(0xFFFF) returns raw physical EEPROM bytes.
// The security section (bytes 0x14–0x2F, 28 bytes) is RC4-encrypted on the
// chip. Factory/user sections (0x30+) are plaintext — so serial, MAC, online
// key are fine to read directly; HDD key, confounder, and region are not.
//
// Decryption algorithm (from XboxEepromEditor HmacSha1.cs / RC4.cs):
//   1. SecurityHash   = eep[0x00..0x13]  (20 bytes)
//   2. EncryptedBlock = eep[0x14..0x2F]  (28 bytes: confounder + hddkey + region)
//   3. For each EepromVersion (RetailFirst/Middle/Last/Debug):
//      a. rc4Key  = XboxHmac(version, SecurityHash)          → 20 bytes
//      b. plaintext = RC4(rc4Key, EncryptedBlock)             → 28 bytes
//      c. verify  = XboxHmac(version, plaintext) == SecurityHash
//      d. if verified: plaintext[8..23] = HDD key, done
//
// XboxHmac is a two-pass custom SHA1 variant:
//   - Uses right-rotation instead of left-rotation
//   - Pre-seeded intermediate hash state (per hardware version)
//   - Inner length counter pre-set to 512 bits
//
// IV constants sourced from XboxEepromEditor/Cryptography/HmacSha1.cs
// ============================================================================

// ---- Custom SHA1 ----

// SHA1 requires left rotation (ROTL).
static DWORD Rotl32(DWORD x, int n)
{
    return (x << n) | (x >> (32 - n));
}

struct XSha1
{
    DWORD H[5];
    BYTE  B[64];
    DWORD bi;
    DWORD bitLen;
};

static void XSha1ProcessBlock(XSha1& s)
{
    static const DWORD k[4] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    DWORD w[80];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (DWORD)s.B[i * 4] << 24;
        w[i] |= (DWORD)s.B[i * 4 + 1] << 16;
        w[i] |= (DWORD)s.B[i * 4 + 2] << 8;
        w[i] |= s.B[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i)
        w[i] = Rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    DWORD a = s.H[0], b = s.H[1], c = s.H[2], d = s.H[3], e = s.H[4];
    for (int i = 0; i < 80; ++i)
    {
        DWORD f;
        switch (i / 20)
        {
        case 0: f = (b & c) | ((~b) & d);              break;
        case 1: f = b ^ c ^ d;                          break;
        case 2: f = (b & c) | (b & d) | (c & d);       break;
        default:f = b ^ c ^ d;                          break;
        }
        DWORD tmp = Rotl32(a, 5) + f + e + w[i] + k[i / 20];
        e = d; d = c; c = Rotl32(b, 30); b = a; a = tmp;
    }
    s.H[0] += a; s.H[1] += b; s.H[2] += c; s.H[3] += d; s.H[4] += e;
    s.bi = 0;
}

static void XSha1Update(XSha1& s, const BYTE* data, int len)
{
    for (int i = 0; i < len; ++i)
    {
        s.B[s.bi++] = data[i];
        s.bitLen += 8;
        if (s.bi == 64) XSha1ProcessBlock(s);
    }
}

static void XSha1Final(XSha1& s, BYTE out[20])
{
    // Pad
    if (s.bi > 55)
    {
        s.B[s.bi++] = 0x80;
        while (s.bi < 64) s.B[s.bi++] = 0;
        XSha1ProcessBlock(s);
        while (s.bi < 56) s.B[s.bi++] = 0;
    }
    else
    {
        s.B[s.bi++] = 0x80;
        while (s.bi < 56) s.B[s.bi++] = 0;
    }
    // Length in bits, big-endian 64-bit (upper 32 always 0 for our input sizes)
    s.B[56] = 0; s.B[57] = 0; s.B[58] = 0; s.B[59] = 0;
    s.B[60] = (BYTE)(s.bitLen >> 24);
    s.B[61] = (BYTE)(s.bitLen >> 16);
    s.B[62] = (BYTE)(s.bitLen >> 8);
    s.B[63] = (BYTE)(s.bitLen);
    XSha1ProcessBlock(s);

    for (int i = 0; i < 20; ++i)
        out[i] = (BYTE)(s.H[i >> 2] >> (8 * (3 - (i & 3))));
}

// Xbox custom HMAC: two-pass with pre-seeded state per hardware version.
// version: 0=Debug 1=RetailFirst(1.0) 2=RetailMiddle(1.1-1.4) 3=RetailLast(1.6)
static void XboxHmac(int version, const BYTE* data, int dataLen, BYTE out[20])
{
    // Pre-seeded IV pairs (isFirst, isSecond) from HmacSha1.cs
    static const DWORD ivs[4][2][5] = {
        { // Debug
            { 0x85F9E51A, 0xE04613D2, 0x6D86A50C, 0x77C32E3C, 0x4BD717A4 },
            { 0x5D7A9C6B, 0xE1922BEB, 0xB82CCDBC, 0x3137AB34, 0x486B52B3 }
        },
        { // RetailFirst (1.0)
            { 0x72127625, 0x336472B9, 0xBE609BEA, 0xF55E226B, 0x99958DAC },
            { 0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA, 0xC4FE2DE8 }
        },
        { // RetailMiddle (1.1-1.4)
            { 0x39B06E79, 0xC9BD25E8, 0xDBC6B498, 0x40B4389D, 0x86BBD7ED },
            { 0x9B49BED3, 0x84B430FC, 0x6B8749CD, 0xEBFE5FE5, 0xD96E7393 }
        },
        { // RetailLast (1.6)
            { 0x8058763A, 0xF97D4E0E, 0x865A9762, 0x8A3D920D, 0x08995B2C },
            { 0x01075307, 0xA2F1E037, 0x1186EEEA, 0x88DA9992, 0x168A5609 }
        }
    };

    XSha1 s;

    // Pass 1: inner hash
    for (int j = 0; j < 5; ++j) s.H[j] = ivs[version][0][j];
    s.bi = 0; s.bitLen = 512;
    XSha1Update(s, data, dataLen);
    BYTE mid[20];
    XSha1Final(s, mid);

    // Pass 2: outer hash
    for (int j = 0; j < 5; ++j) s.H[j] = ivs[version][1][j];
    s.bi = 0; s.bitLen = 512;
    XSha1Update(s, mid, 20);
    XSha1Final(s, out);
}

// ---- RC4 ----

struct RC4Ctx { BYTE S[256]; int x, y; };

static void RC4Init(RC4Ctx& ctx, const BYTE* key, int keyLen)
{
    ctx.x = 0; ctx.y = 0;
    for (int i = 0; i < 256; ++i) ctx.S[i] = (BYTE)i;
    int j = 0;
    for (int i = 0; i < 256; ++i)
    {
        j = (key[i % keyLen] + ctx.S[i] + j) & 0xFF;
        BYTE t = ctx.S[i]; ctx.S[i] = ctx.S[j]; ctx.S[j] = t;
    }
}

static void RC4Crypt(RC4Ctx& ctx, BYTE* data, int len)
{
    for (int i = 0; i < len; ++i)
    {
        ctx.x = (ctx.x + 1) & 0xFF;
        ctx.y = (ctx.S[ctx.x] + ctx.y) & 0xFF;
        BYTE t = ctx.S[ctx.x]; ctx.S[ctx.x] = ctx.S[ctx.y]; ctx.S[ctx.y] = t;
        data[i] ^= ctx.S[(ctx.S[ctx.x] + ctx.S[ctx.y]) & 0xFF];
    }
}

// Attempt to decrypt the 28-byte security section of a raw EEPROM buffer.
// Returns true if any version matched (verified by re-hashing).
// On success, buf[0x14..0x2F] is overwritten with plaintext.
static bool EepromDecryptSecurity(BYTE* buf)
{
    const BYTE* secHash = buf + 0x00;   // 20-byte SecurityHash at offset 0
    BYTE  encBlock[28];
    for (int i = 0; i < 28; ++i) encBlock[i] = buf[0x14 + i];

    for (int v = 0; v < 4; ++v)
    {
        // Derive RC4 key
        BYTE rc4Key[20];
        XboxHmac(v, secHash, 20, rc4Key);

        // Decrypt a copy of the 28-byte block
        BYTE plain[28];
        for (int i = 0; i < 28; ++i) plain[i] = encBlock[i];
        RC4Ctx rc4;
        RC4Init(rc4, rc4Key, 20);
        RC4Crypt(rc4, plain, 28);

        // Verify: XboxHmac(version, plaintext) should equal SecurityHash
        BYTE verify[20];
        XboxHmac(v, plain, 28, verify);
        bool match = true;
        for (int i = 0; i < 20; ++i)
            if (verify[i] != secHash[i]) { match = false; break; }

        if (match)
        {
            // Write decrypted plaintext back into the buffer
            for (int i = 0; i < 28; ++i) buf[0x14 + i] = plain[i];
            return true;
        }
    }
    return false;
}

// ============================================================================
// Data loading
// ============================================================================

static void LoadData()
{
    HddData& d = s_data;

    // Ensure HDD partitions are accessible via drive letters before any file I/O
    MountHddDrives();

    // ---- ATA IDENTIFY ----
    d.ataOK = AtaIdentify(d.identBuf);
    if (d.ataOK)
    {
        WORD* w = d.identBuf;

        AtaStr(&w[27], 20, d.model, sizeof(d.model));
        AtaStr(&w[10], 10, d.serial, sizeof(d.serial));
        AtaStr(&w[23], 4, d.fwRev, sizeof(d.fwRev));

        // Capacity: prefer LBA48 (words 100-103) when supported — needed for
        // modded Xboxes with >137GB drives.  LBA28 (words 60-61) caps at 137GB.
        DWORD lba28 = ((DWORD)w[61] << 16) | w[60];
        d.lba48supported = (w[83] & (1 << 10)) != 0;
        // LBA48 full 48-bit count: words 100-103.
        // Upper 32 bits (words 102-103) are non-zero only beyond 2TB — store
        // separately for display; use lower 32 for GB math (sufficient to 2TB).
        DWORD lba48lo = ((DWORD)w[101] << 16) | w[100];
        DWORD lba48hi = ((DWORD)w[103] << 16) | w[102];

        // GB = sectors / (2*1024*1024).  Use LBA48 count when available.
        DWORD gbSectors = d.lba48supported ? lba48lo : lba28;
        DWORD gbHi = d.lba48supported ? lba48hi : 0;
        // For drives > 2TB, lba48hi > 0: add its contribution (each hi unit = 2048 GB)
        DWORD gbTotal = (gbHi > 0 ? gbHi * 2048UL : 0UL)
            + gbSectors / (2UL * 1024UL * 1024UL);
        char t[12];
        IntToStr((int)gbTotal, t, sizeof(t));
        StrCopy(d.capacity, sizeof(d.capacity), t);
        StrCat2(d.capacity, sizeof(d.capacity), d.capacity, " GB");

        // LBA sector counts for display
        IntToStr((int)lba28, t, sizeof(t));
        StrCopy(d.lba28sectors, sizeof(d.lba28sectors), t);
        if (d.lba48supported)
        {
            // Show lower 32 bits; append '+' if upper 32 are non-zero (>2TB)
            IntToStr((int)lba48lo, t, sizeof(t));
            StrCopy(d.lba48sectors, sizeof(d.lba48sectors), t);
            if (lba48hi > 0)
                StrCat2(d.lba48sectors, sizeof(d.lba48sectors), d.lba48sectors, "+");
        }
        else StrCopy(d.lba48sectors, sizeof(d.lba48sectors), "N/A");

        // UDMA detection (ATA/ATAPI-7 T13 spec):
        //   Word 53 bit 2: validity gate for word 88.
        //   Word 88 bits 6:0  = supported UDMA modes.
        //   Word 88 bits 14:8 = active UDMA modes.
        //
        // Xbox MCPX programs DMA timing in its own PCI registers without issuing
        // software SET FEATURES — active bits (14:8) are usually zero on cold boot
        // even though UDMA IS running.  Treat zero-active / non-zero-supported (with
        // word 53 bit 2 set) as "MCPX-configured UDMA" — report cleanly, no marker.
        // '?' = word 53 bit 2 clear, word 88 validity not guaranteed by ATA spec.
        {
            bool w88valid = !!(w[53] & (1 << 2));
            BYTE active = (BYTE)(w[88] >> 8) & 0x7F;
            BYTE supported = (BYTE)(w[88] & 0xFF) & 0x7F;
            int  mode = -1;
            bool needMark = false;

            if (w88valid && (active || supported))
            {
                // Active preferred; fall back to supported (MCPX zero-active path)
                BYTE use = active ? active : supported;
                for (int i = 6; i >= 0; --i)
                    if (use & (1 << i)) { mode = i; break; }
                needMark = false;   // clean: word 88 valid, using correct data
            }
            else if (!w88valid && (active || supported))
            {
                // Word 53 bit 2 clear — word 88 validity not spec-guaranteed
                BYTE use = active ? active : supported;
                for (int i = 6; i >= 0; --i)
                    if (use & (1 << i)) { mode = i; break; }
                needMark = true;
            }

            if (mode >= 0)
            {
                StrCopy(d.udmaMode, sizeof(d.udmaMode), "UDMA");
                IntToStr(mode, t, sizeof(t));
                StrCat2(d.udmaMode, sizeof(d.udmaMode), d.udmaMode, t);
                if (needMark) StrCat2(d.udmaMode, sizeof(d.udmaMode), d.udmaMode, "?");
            }
            else
            {
                // Word 88 zero or unusable — fall back to MWDMA (word 63)
                BYTE mwA = (BYTE)(w[63] >> 8) & 0x07;
                BYTE mwS = (BYTE)(w[63] & 0xFF) & 0x07;
                int  mwMode = -1;
                bool mwFall = false;
                for (int i = 2; i >= 0; --i)
                    if (mwA & (1 << i)) { mwMode = i; break; }
                if (mwMode < 0)
                    for (int i = 2; i >= 0; --i)
                        if (mwS & (1 << i)) { mwMode = i; mwFall = true; break; }
                if (mwMode >= 0)
                {
                    StrCopy(d.udmaMode, sizeof(d.udmaMode), "MWDMA");
                    IntToStr(mwMode, t, sizeof(t));
                    StrCat2(d.udmaMode, sizeof(d.udmaMode), d.udmaMode, t);
                    if (mwFall) StrCat2(d.udmaMode, sizeof(d.udmaMode), d.udmaMode, "?");
                }
                else StrCopy(d.udmaMode, sizeof(d.udmaMode), "PIO");
            }
        }

        // Buffer size (word 21, in 512-byte units)
        DWORD bufSects = w[21];
        IntToStr((int)(bufSects / 2), t, sizeof(t));
        StrCopy(d.bufferKB, sizeof(d.bufferKB), t);
        StrCat2(d.bufferKB, sizeof(d.bufferKB), d.bufferKB, " KB");

        // ATA version (word 80 bitmask, highest set bit = version)
        int ataVer = 0;
        for (int i = 14; i >= 1; --i)
            if (w[80] & (1 << i)) { ataVer = i; break; }
        StrCopy(d.ataVersion, sizeof(d.ataVersion), "ATA-");
        IntToStr(ataVer, t, sizeof(t));
        StrCat2(d.ataVersion, sizeof(d.ataVersion), d.ataVersion, t);

        // SSD / RPM (word 217: 0x0001 = SSD, else RPM)
        WORD rpm = w[217];
        d.isSSD = (rpm == 0x0001);
        if (d.isSSD)
            StrCopy(d.rpmStr, sizeof(d.rpmStr), "SSD");
        else if (rpm > 1 && rpm < 20000)
        {
            IntToStr((int)rpm, t, sizeof(t));
            StrCopy(d.rpmStr, sizeof(d.rpmStr), t);
            StrCat2(d.rpmStr, sizeof(d.rpmStr), d.rpmStr, " RPM");
        }
        else StrCopy(d.rpmStr, sizeof(d.rpmStr), "N/A");

        // Security status (word 128)
        // Bit 0 = security supported
        // Bit 1 = security enabled
        // Bit 2 = security locked
        // Bit 3 = security frozen
        // Match PrometheOS isDriveLocked(): locked only when BOTH enabled AND locked
        // bits are set. Bit 2 alone can be set on some third-party drives in a
        // transient/malformed state — PrometheOS correctly requires bit 1 as well.
        d.securitySupported = (w[128] & 0x01) != 0;
        d.isLocked = ((w[128] & 0x02) != 0) && ((w[128] & 0x04) != 0);

        // SMART support: word 82 bit 0 = SMART feature set supported
        d.smartSupported = (w[82] & 0x01) != 0;
        if (d.smartSupported)
            d.smartOK = SmartReadData(d.smartBuf);
    }
    else
    {
        StrCopy(d.model, sizeof(d.model), "Not detected");
        StrCopy(d.serial, sizeof(d.serial), "N/A");
        StrCopy(d.fwRev, sizeof(d.fwRev), "N/A");
        StrCopy(d.capacity, sizeof(d.capacity), "N/A");
        StrCopy(d.udmaMode, sizeof(d.udmaMode), "N/A");
        StrCopy(d.bufferKB, sizeof(d.bufferKB), "N/A");
        StrCopy(d.lba28sectors, sizeof(d.lba28sectors), "N/A");
        StrCopy(d.lba48sectors, sizeof(d.lba48sectors), "N/A");
        StrCopy(d.ataVersion, sizeof(d.ataVersion), "N/A");
        StrCopy(d.rpmStr, sizeof(d.rpmStr), "N/A");
    }

    // ---- EEPROM — safe kernel read via ExQueryNonVolatileSetting(0xFFFF) ----
    // Returns raw physical EEPROM bytes (256 bytes).
    // Factory/user sections (0x30+) are plaintext — serial, MAC, online key OK.
    // Security section (0x14-0x2F: confounder + HDD key + region) is RC4-encrypted
    // on the chip. Must call EepromDecryptSecurity() before reading those fields.
    {
        BYTE   eepBuf[256];
        ULONG  eeType = 0, eeLen = 0;
        LONG   eeStatus = ExQueryNonVolatileSetting(0xFFFF, &eeType, eepBuf, 256, &eeLen);
        d.eepromOK = (eeStatus == 0 && eeLen >= 0x58);

        if (d.eepromOK)
        {
            // Decrypt the security section in-place before reading any of its fields.
            // EepromDecryptSecurity tries all 4 hardware versions and verifies via HMAC.
            d.eepromDecrypted = EepromDecryptSecurity(eepBuf);

            // HDD key: bytes 0x1C-0x2B (16 bytes) — in security section, must be decrypted
            for (int i = 0; i < 16; ++i) d.hddKey[i] = eepBuf[0x1C + i];

            // XBE Region: bytes 0x2C-0x2F (4 bytes) — also in security section
            for (int i = 0; i < 4; ++i) d.xbeRegion[i] = eepBuf[0x2C + i];

            // Online key: bytes 0x48-0x57 (16 bytes) — factory section, plaintext
            for (int i = 0; i < 16; ++i) d.onlineKey[i] = eepBuf[0x48 + i];

            // Region from XBERegion[0] — valid only if decrypted
            d.regionByte = d.eepromDecrypted ? d.xbeRegion[0] : 0;
            if (d.eepromDecrypted)
            {
                switch (d.regionByte & 0x07)
                {
                case 0x01: StrCopy(d.regionStr, sizeof(d.regionStr), "NTSC-M  (N. America)"); break;
                case 0x02: StrCopy(d.regionStr, sizeof(d.regionStr), "NTSC-J  (Japan)");      break;
                case 0x04: StrCopy(d.regionStr, sizeof(d.regionStr), "PAL     (Europe/AUS)"); break;
                default:
                {
                    char t[4]; IntToHex(d.regionByte, 2, t, sizeof(t));
                    StrCopy(d.regionStr, sizeof(d.regionStr), "0x");
                    StrCat2(d.regionStr, sizeof(d.regionStr), d.regionStr, t);
                }
                }
            }
            else
            {
                // Decryption failed — security section is still encrypted
                StrCopy(d.regionStr, sizeof(d.regionStr), "(encrypted)");
            }

            // Serial: bytes 0x34-0x3F (12 bytes ASCII) — factory section, plaintext
            for (int i = 0; i < 12; ++i) d.serialEEPROM[i] = (char)eepBuf[0x34 + i];
            d.serialEEPROM[12] = '\0';
        }
        else
        {
            d.eepromDecrypted = false;
            StrCopy(d.serialEEPROM, sizeof(d.serialEEPROM), "EEPROM READ FAILED");
        }
    }

    // ---- Partition sizes (C/E/F/G) ----
    {
        const char* letters = "CEFG";
        for (int pi = 0; pi < 4; ++pi)
        {
            d.parts[pi].letter = letters[pi];
            d.parts[pi].present = false;
            d.parts[pi].freeRatio = 0.f;

            char path[8];
            path[0] = letters[pi]; path[1] = ':'; path[2] = '\\'; path[3] = '\0';

            ULARGE_INTEGER freeToCaller, total, free;
            if (GetDiskFreeSpaceExA(path, &freeToCaller, &total, &free))
            {
                d.parts[pi].present = true;

                // Total GB (one decimal)
                DWORD totalMB = (DWORD)(total.QuadPart / (1024ULL * 1024ULL));
                DWORD totalGB = totalMB / 1024;
                DWORD totalDec = (totalMB % 1024) * 10 / 1024;
                char t[12];
                IntToStr((int)totalGB, t, sizeof(t));
                StrCopy(d.parts[pi].totalStr, sizeof(d.parts[pi].totalStr), t);
                StrCat2(d.parts[pi].totalStr, sizeof(d.parts[pi].totalStr), d.parts[pi].totalStr, ".");
                IntToStr((int)totalDec, t, sizeof(t));
                StrCat2(d.parts[pi].totalStr, sizeof(d.parts[pi].totalStr), d.parts[pi].totalStr, t);
                StrCat2(d.parts[pi].totalStr, sizeof(d.parts[pi].totalStr), d.parts[pi].totalStr, " GB");

                // Free GB (one decimal)
                DWORD freeMB = (DWORD)(free.QuadPart / (1024ULL * 1024ULL));
                DWORD freeGB = freeMB / 1024;
                DWORD freeDec = (freeMB % 1024) * 10 / 1024;
                IntToStr((int)freeGB, t, sizeof(t));
                StrCopy(d.parts[pi].freeStr, sizeof(d.parts[pi].freeStr), t);
                StrCat2(d.parts[pi].freeStr, sizeof(d.parts[pi].freeStr), d.parts[pi].freeStr, ".");
                IntToStr((int)freeDec, t, sizeof(t));
                StrCat2(d.parts[pi].freeStr, sizeof(d.parts[pi].freeStr), d.parts[pi].freeStr, t);
                StrCat2(d.parts[pi].freeStr, sizeof(d.parts[pi].freeStr), d.parts[pi].freeStr, " GB");
                // Ratio for bar render
                if (total.QuadPart > 0)
                    d.parts[pi].freeRatio = (float)((double)free.QuadPart / (double)total.QuadPart);
            }
        }
    }

    d.exportDone = false;
    d.exportOK = false;

}

// ============================================================================
// Export
// ============================================================================

static void ExportData()
{
    // D:\ is the XBE app directory — always mounted, writable on modded hardware,
    // and directly accessible via FTP alongside the XBE itself.
    HANDLE hf = CreateFileA("D:\\hddinfo.txt", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hf == INVALID_HANDLE_VALUE)
    {
        s_data.exportDone = true; s_data.exportOK = false; return;
    }

    const HddData& d = s_data;
    DWORD w;
    char line[128];

    const char* hdr = "XbDiag HDD Info\r\n===============\r\n";
    WriteFile(hf, hdr, StrLen(hdr), &w, NULL);

    StrCopy(line, sizeof(line), "Model:          ");
    StrCat2(line, sizeof(line), line, d.model);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Serial (ATA):   ");
    StrCat2(line, sizeof(line), line, d.serial);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Firmware:       ");
    StrCat2(line, sizeof(line), line, d.fwRev);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Capacity:       ");
    StrCat2(line, sizeof(line), line, d.capacity);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Interface:      ");
    StrCat2(line, sizeof(line), line, d.udmaMode);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Buffer:         ");
    StrCat2(line, sizeof(line), line, d.bufferKB);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "ATA Version:    ");
    StrCat2(line, sizeof(line), line, d.ataVersion);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Type:           ");
    StrCat2(line, sizeof(line), line, d.rpmStr);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "LBA28 Sectors:  ");
    StrCat2(line, sizeof(line), line, d.lba28sectors);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "LBA48 Sectors:  ");
    StrCat2(line, sizeof(line), line, d.lba48sectors);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "HDD Locked:     ");
    StrCat2(line, sizeof(line), line, d.isLocked ? "YES" : "NO");
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "\r\nEEPROM\r\n------\r\n");
    StrCat2(line, sizeof(line), line, "");
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Serial (EEPROM):");
    StrCat2(line, sizeof(line), line, d.serialEEPROM);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Region:         ");
    StrCat2(line, sizeof(line), line, d.regionStr);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    // HDD key as hex string
    char hexbuf[64];
    FormatHex(d.hddKey, 16, hexbuf, sizeof(hexbuf));
    StrCopy(line, sizeof(line), "HDD Key:        ");
    StrCat2(line, sizeof(line), line, hexbuf);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    FormatHex(d.xbeRegion, 4, hexbuf, sizeof(hexbuf));
    StrCopy(line, sizeof(line), "XBE Region:     ");
    StrCat2(line, sizeof(line), line, hexbuf);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    FormatHex(d.onlineKey, 16, hexbuf, sizeof(hexbuf));
    StrCopy(line, sizeof(line), "Online Key:     ");
    StrCat2(line, sizeof(line), line, hexbuf);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);


    FlushFileBuffers(hf);
    CloseHandle(hf);
    s_data.exportDone = true;
    s_data.exportOK = true;
}

void HddInfo_OnEnter()
{
    s_prevBtns = 0;
    s_view = VIEW_INFO;
    s_loaded = false;
    // Cleanup any leftover benchmark temp file from a prior visit
    HddBench_Reset();
    ZeroMemory(&s_data, sizeof(s_data));
}

// ============================================================================
// Render
// ============================================================================

static void DrawRow(float lx, float vx, float y,
    const char* label, const char* val, DWORD vc = COL_WHITE)
{
    DrawText(lx, y, label, 1.2f, COL_GRAY);
    DrawText(vx, y, val, 1.2f, vc);
}

// Draw a multi-byte hex key across up to two lines
// Draw a multi-byte hex key across up to two lines.
// If eepromOK but !decrypted, the security-section bytes are still encrypted.
static void DrawKeyRow(float lx, float vx, float y,
    const char* label, const BYTE* data, int count,
    bool eepromOK, bool decrypted = true)
{
    DrawText(lx, y, label, 1.2f, COL_GRAY);
    if (!eepromOK)
    {
        DrawText(vx, y, "EEPROM NAK", 1.2f, COL_RED);
        return;
    }
    if (!decrypted)
    {
        DrawText(vx, y, "(encrypted - version unknown)", 1.2f, COL_ORANGE);
        return;
    }
    // Split 16 bytes into two rows of 8
    char hex[28];
    FormatHex(data, count > 8 ? 8 : count, hex, sizeof(hex));
    DrawText(vx, y, hex, 1.1f, COL_CYAN);
    if (count > 8)
    {
        FormatHex(data + 8, count - 8, hex, sizeof(hex));
        DrawText(vx, y + LH, hex, 1.1f, COL_CYAN);
    }
}

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    const char* hint = s_data.exportDone
        ? (s_data.exportOK
            ? "[A] Saved OK    [Right] SMART  [RT] Bench  [B] Back"
            : "[A] Save failed [Right] SMART  [RT] Bench  [B] Back")
        : "[A] Save hddinfo.txt  [Right] SMART  [RT] Bench  [B] Back";

    DrawPageChrome(logo, "HDD INFO", hint);

    const HddData& d = s_data;

    // Vertical divider
    VLine(COL_R - 10.f, CY, BOT_BAR_Y - 4.f, COL_BORDER);

    float y1 = CY;
    float y2 = CY;

    // ---- LEFT: DRIVE ----
    DrawText(COL_L, y1, "DRIVE", 1.3f, COL_YELLOW);
    HLine(y1 + LH + 1.f, COL_L, COL_R - 12.f, COL_BORDER);
    y1 += LH + 6.f;

    DrawRow(COL_L, COL_VL, y1, "MODEL   :", d.model,
        d.ataOK ? COL_CYAN : COL_RED);                    y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "SERIAL  :", d.serial);        y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "FIRMWARE:", d.fwRev);         y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "CAPACITY:", d.capacity,
        COL_CYAN);                                         y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "TYPE    :", d.rpmStr,
        d.isSSD ? COL_GREEN : COL_WHITE);                  y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "IFACE   :", d.udmaMode,
        COL_WHITE);                                        y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "BUFFER  :", d.bufferKB);      y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "ATA VER :", d.ataVersion);    y1 += LH + GAP;

    // LBA info sub-section
    DrawText(COL_L, y1, "SECTORS", 1.2f, COL_YELLOW);
    HLine(y1 + LH + 1.f, COL_L, COL_R - 12.f, COL_BORDER);
    y1 += LH + 5.f;
    DrawRow(COL_L, COL_VL, y1, "LBA28   :", d.lba28sectors);  y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "LBA48   :", d.lba48sectors,
        d.lba48supported ? COL_GREEN : COL_GRAY);          y1 += LH + GAP;

    // Security state
    DrawText(COL_L, y1, "ATA SECURITY", 1.2f, COL_YELLOW);
    HLine(y1 + LH + 1.f, COL_L, COL_R - 12.f, COL_BORDER);
    y1 += LH + 5.f;
    if (d.ataOK)
    {
        DrawRow(COL_L, COL_VL, y1, "SUPPORT :",
            d.securitySupported ? "YES" : "NO",
            d.securitySupported ? COL_WHITE : COL_GRAY);   y1 += LH;
        DrawRow(COL_L, COL_VL, y1, "LOCKED  :",
            d.isLocked ? "YES" : "NO",
            d.isLocked ? COL_RED : COL_GREEN);
    }
    else DrawText(COL_L, y1, "Drive not detected", 1.2f, COL_RED);

    // ---- LEFT: PARTITIONS ----
    y1 += LH + GAP;
    DrawText(COL_L, y1, "PARTITIONS", 1.2f, COL_YELLOW);
    HLine(y1 + LH + 1.f, COL_L, COL_R - 12.f, COL_BORDER);
    y1 += LH + 5.f;
    {
        bool anyPart = false;
        for (int pi = 0; pi < 4; ++pi)
        {
            const HddData::PartInfo& p = d.parts[pi];
            if (!p.present) continue;
            anyPart = true;

            // Label  e.g. "E:"
            char label[4]; label[0] = p.letter; label[1] = ':'; label[2] = '\0';
            DrawText(COL_L, y1, label, 1.2f, COL_GRAY);

            // Usage bar: total width covers the value column space
            const float BAR_X = COL_L + 22.f;
            const float BAR_W = SW * 0.22f;  // ~140px at 640 — leaves room for free text
            const float BAR_H = LH - 3.f;
            float usedFrac = 1.f - p.freeRatio;
            if (usedFrac < 0.f) usedFrac = 0.f;
            if (usedFrac > 1.f) usedFrac = 1.f;

            // Bar background
            FillRect(BAR_X, y1, BAR_X + BAR_W, y1 + BAR_H,
                D3DCOLOR_XRGB(18, 22, 45));
            // Used portion — colour shifts green→orange→red based on fill
            DWORD barCol;
            if (usedFrac < 0.7f)      barCol = D3DCOLOR_XRGB(40, 160, 60);
            else if (usedFrac < 0.9f) barCol = D3DCOLOR_XRGB(200, 140, 0);
            else                      barCol = D3DCOLOR_XRGB(200, 50, 30);
            if (usedFrac > 0.f)
                FillRect(BAR_X, y1, BAR_X + BAR_W * usedFrac, y1 + BAR_H, barCol);
            HLine(y1, BAR_X, BAR_X + BAR_W, COL_BORDER);
            HLine(y1 + BAR_H, BAR_X, BAR_X + BAR_W, COL_BORDER);
            VLine(BAR_X, y1, y1 + BAR_H, COL_BORDER);
            VLine(BAR_X + BAR_W, y1, y1 + BAR_H, COL_BORDER);

            // Free value to the right of bar
            char freeLabel[32];
            StrCopy(freeLabel, sizeof(freeLabel), p.freeStr);
            StrCat2(freeLabel, sizeof(freeLabel), freeLabel, " free");
            DrawText(BAR_X + BAR_W + 6.f, y1, freeLabel, 1.05f, COL_WHITE);

            y1 += LH;
        }
        if (!anyPart)
            DrawText(COL_L, y1, "No partitions found", 1.2f, COL_GRAY);
    }

    // ---- RIGHT: SECURITY / EEPROM ----
    DrawText(COL_R, y2, "EEPROM SECURITY", 1.3f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 6.f;

    DrawRow(COL_R, COL_VR, y2, "SERIAL  :", d.serialEEPROM,
        d.eepromOK ? COL_WHITE : COL_RED);                 y2 += LH;
    DrawRow(COL_R, COL_VR, y2, "REGION  :", d.regionStr,
        COL_CYAN);                                         y2 += LH + GAP;

    // HDD key (16 bytes = 2 rows)
    {
        // Header shows decryption status
        const char* hdrStr = d.eepromDecrypted ? "HDD KEY" : "HDD KEY  (decryption failed)";
        DWORD hdrCol = d.eepromDecrypted ? COL_YELLOW : COL_ORANGE;
        DrawText(COL_R, y2, hdrStr, 1.2f, hdrCol);
    }
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 5.f;
    DrawKeyRow(COL_R, COL_VR, y2, "BYTES   :", d.hddKey, 16, d.eepromOK, d.eepromDecrypted);
    y2 += LH * 2.f + GAP;

    // XBE Region (4 bytes = 1 row)
    DrawText(COL_R, y2, "XBE REGION", 1.2f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 5.f;
    DrawKeyRow(COL_R, COL_VR, y2, "BYTES   :", d.xbeRegion, 4, d.eepromOK, d.eepromDecrypted);
    y2 += LH + GAP;

    // Online key (16 bytes = 2 rows)
    DrawText(COL_R, y2, "ONLINE KEY", 1.2f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 5.f;
    DrawKeyRow(COL_R, COL_VR, y2, "BYTES   :", d.onlineKey, 16, d.eepromOK);
    y2 += LH * 2.f + GAP;


    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Render: SMART view
// ============================================================================

// ============================================================================
// Tick
// ============================================================================

void HddInfo_Tick(const DiagLogo& logo)
{
    if (!s_loaded)
    {
        Render(logo);   // loading screen
        LoadData();
        s_loaded = true;
        return;
    }

    WORD cur = GetButtons();

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        bool hddBusy = (s_view == VIEW_BENCH &&
            HddBench_GetData().state != BENCH_IDLE && HddBench_GetData().state != BENCH_DONE);
        if (!hddBusy)
        {
            RequestState(MSTATE_MENU);
            s_prevBtns = cur;
            return;
        }
    }

    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT) && s_view == VIEW_INFO)
        s_view = VIEW_SMART;
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
    {
        if (s_view == VIEW_BENCH)
            HddBench_Cleanup();
        s_view = VIEW_INFO;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_RTRIG) && s_view == VIEW_INFO)
        s_view = VIEW_BENCH;

    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        if (s_view == VIEW_INFO)
            ExportData();
        else if (s_view == VIEW_SMART)
            HddSmart_Export();
        else if (s_view == VIEW_BENCH)
        {
            if (HddBench_GetData().state == BENCH_IDLE)
                HddBench_Start();
            else if (HddBench_GetData().state == BENCH_CONFIRM)
            {
                // User confirmed — start timing now, immediately before write loop
                HddBench_GetData().writeT0 = GetTickCount();
                HddBench_GetData().state = BENCH_WRITE;
            }
            else if (HddBench_GetData().state == BENCH_DONE)
                HddBench_Export();
        }
    }

    // Cancel HDD benchmark with B
    if (s_view == VIEW_BENCH && HddBench_GetData().state != BENCH_IDLE && HddBench_GetData().state != BENCH_DONE)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            HddBench_Cleanup();
            HddBench_GetData().state = BENCH_DONE;
            StrCopy(HddBench_GetData().statusMsg, sizeof(HddBench_GetData().statusMsg), "Benchmark cancelled");
            s_prevBtns = cur;
            HddBench_Render(logo);
            return;
        }
    }

    // Cancel DVD benchmark with B (only while actively reading)

    s_prevBtns = cur;

    // Run one tick of work
    if (s_view == VIEW_BENCH &&
        HddBench_GetData().state != BENCH_IDLE &&
        HddBench_GetData().state != BENCH_CONFIRM &&
        HddBench_GetData().state != BENCH_DONE)
        HddBench_Tick();

    if (s_view == VIEW_SMART)
        HddSmart_Render(logo);
    else if (s_view == VIEW_BENCH)
        HddBench_Render(logo);
    else
        Render(logo);
}
// ============================================================================
// AutoRun — headless HDD info gather + benchmark for XbSet automation
// ============================================================================

static void HddWriteLine(HANDLE hf, const char* label, const char* val)
{
    char line[128]; DWORD w;
    StrCopy(line, sizeof(line), label);
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
}

void HddInfo_AutoRun(HANDLE hReport)
{
    // Load ATA identify data
    LoadData();

    const HddData& d = s_data;
    HddWriteLine(hReport, "Drive Model:  ", d.ataOK ? d.model : "Not detected");
    HddWriteLine(hReport, "Drive Serial: ", d.ataOK ? d.serial : "N/A");
    HddWriteLine(hReport, "Capacity:     ", d.ataOK ? d.capacity : "N/A");
    HddWriteLine(hReport, "Interface:    ", d.ataOK ? d.udmaMode : "N/A");
    HddWriteLine(hReport, "Type:         ", d.ataOK ? d.rpmStr : "N/A");

    // Run benchmark if drive present
    if (d.ataOK)
    {
        DWORD w;
        const char* bmsg = "Running benchmark (64MB)...\r\n";
        WriteFile(hReport, bmsg, StrLen(bmsg), &w, NULL);

        HddBench_Start();
        // HddBench_Start() halts at BENCH_CONFIRM waiting for [A] from the user.
        // In AutoRun there is no user — bypass confirm by stamping writeT0
        // and advancing to BENCH_WRITE directly.
        if (HddBench_GetData().state == BENCH_CONFIRM)
        {
            HddBench_GetData().writeT0 = GetTickCount();
            HddBench_GetData().state = BENCH_WRITE;
        }
        // Drive to completion — HddBench_Tick() runs each phase in a tight loop
        while (HddBench_GetData().state != BENCH_DONE && HddBench_GetData().state != BENCH_IDLE)
            HddBench_Tick();

        // Format results
        auto FmtF = [](float v, char* out, int len) {
            int whole = Ftoi(v);
            int dec = Ftoi((v - (float)whole) * 10.f);
            char t[12]; IntToStr(whole, t, sizeof(t));
            StrCopy(out, len, t); StrCat2(out, len, out, ".");
            IntToStr(dec, t, sizeof(t)); StrCat2(out, len, out, t);
            };

        char val[24];
        if (!HddBench_GetData().readOnly)
        {
            FmtF(HddBench_GetData().writeMBs, val, sizeof(val));
            StrCat2(val, sizeof(val), val, " MB/s");
            HddWriteLine(hReport, "FS Write:     ", val);

            FmtF(HddBench_GetData().rawWrMBs, val, sizeof(val));
            StrCat2(val, sizeof(val), val, " MB/s");
            HddWriteLine(hReport, "Raw Write:    ", val);
        }

        FmtF(HddBench_GetData().readMBs, val, sizeof(val));
        StrCat2(val, sizeof(val), val, " MB/s");
        HddWriteLine(hReport, "Seq Read:     ", val);

        if (d.isSSD)
        {
            char iopsStr[12]; IntToStr(Ftoi(HddBench_GetData().rand4kIOPS), iopsStr, sizeof(iopsStr));
            StrCat2(iopsStr, sizeof(iopsStr), iopsStr, " IOPS");
            HddWriteLine(hReport, "4K Random:    ", iopsStr);
        }
        else
        {
            FmtF(HddBench_GetData().cacheMBs, val, sizeof(val));
            StrCat2(val, sizeof(val), val, " MB/s");
            HddWriteLine(hReport, "Cache Read:   ", val);
        }

        FmtF(HddBench_GetData().seekMs, val, sizeof(val));
        StrCat2(val, sizeof(val), val, " ms avg");
        HddWriteLine(hReport, "Seek Time:    ", val);

        // Cleanup temp file
        HddBench_Cleanup();
    }
}