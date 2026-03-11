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
//  Model                             HDD Key     (16 bytes hex)
//  Serial                            Console Key (16 bytes hex)
//  Firmware rev                      Online Key  (8 bytes hex)
//  Capacity                          Region
//  Interface (UDMA mode)             Serial No   (matches ATA)
//  Buffer size
//  LBA28 / LBA48 sectors
//  Security state (locked/unlocked)
//
//  [BOT BAR]  [A] Export    [B] Back
//
// ATA IDENTIFY (port 0x1F0, primary channel, master):
//   Word 10-19  = serial number (byte-swapped ASCII)
//   Word 23-26  = firmware revision (byte-swapped ASCII)
//   Word 27-46  = model string (byte-swapped ASCII)
//   Word 47     = max sectors per R/W multiple
//   Word 49     = capabilities (LBA support bit 9)
//   Word 54-58  = current CHS geometry
//   Word 60-61  = LBA28 addressable sectors
//   Word 63     = multiword DMA modes
//   Word 80     = ATA standard version
//   Word 83     = LBA48 support (bit 10)
//   Word 85-87  = enabled features
//   Word 88     = UDMA modes (bits 0-5 supported, bits 8-13 active)
//   Word 100-103= LBA48 addressable sectors (64-bit)
//   Word 128    = security status register
//   Word 168    = nominal media rotation rate (0x0001 = SSD)
//   Word 217    = nominal media rotation rate (newer standard)
//
// EEPROM layout (SMBus 0x54, 93LC56 256-byte):
//   0x00-0x13   HMAC SHA1 hash       (20 bytes)
//   0x14-0x1B   Confounder           (8 bytes)
//   0x1C-0x2B   HDD key              (16 bytes)
//   0x2C-0x2F   XBE region code      (4 bytes)
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

// ============================================================================
// Data
// ============================================================================

struct HddData
{
    // ATA drive info
    bool  ataOK;
    char  model[44];
    char  serial[22];
    char  fwRev[10];
    char  capacity[16];       // "XX.X GB"
    char  udmaMode[10];       // "UDMA5" etc
    char  bufferKB[10];       // buffer size in KB
    char  lba28sectors[16];
    char  lba48sectors[20];
    bool  lba48supported;
    bool  isLocked;
    bool  securitySupported;
    bool  isSSD;
    char  rpmStr[12];         // "SSD" or "XXXX RPM"
    char  ataVersion[8];      // "ATA-6" etc
    WORD  identBuf[256];

    // EEPROM security data
    bool  eepromOK;
    BYTE  hddKey[16];
    BYTE  xbeRegion[4];
    BYTE  onlineKey[16];
    BYTE  regionByte;
    char  regionStr[24];
    char  serialEEPROM[14];   // 12 chars + null

    // Export
    bool  exportDone;
    bool  exportOK;

    // SMART
    bool  smartSupported;    // word 82 bit 0 from IDENTIFY
    bool  smartOK;           // SMART READ DATA succeeded
    bool  smartExportDone;
    bool  smartExportOK;
    BYTE  smartBuf[512];     // raw SMART READ DATA response
};

// View toggle — Info (drive + EEPROM) or SMART attribute table
enum HddView { VIEW_INFO = 0, VIEW_SMART };

static HddData s_data;
static WORD    s_prevBtns;
static bool    s_loaded;
static HddView s_view;

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
static const char* SmartAttrName(BYTE id)
{
    switch (id)
    {
    case 0x01: return "Read Error Rate";
    case 0x02: return "Throughput Perf";
    case 0x03: return "Spin-Up Time";
    case 0x04: return "Start/Stop Count";
    case 0x05: return "Reallocated Sects";  // critical
    case 0x07: return "Seek Error Rate";
    case 0x08: return "Seek Time Perf";
    case 0x09: return "Power-On Hours";
    case 0x0A: return "Spin Retry Count";
    case 0x0B: return "Calibration Retry";
    case 0x0C: return "Power Cycle Count";
    case 0xAA: return "Available Reservd";
    case 0xAB: return "Program Fail Count";
    case 0xAC: return "Erase Fail Count";
    case 0xAD: return "Wear Level Count";
    case 0xAE: return "Unexpected Poweroff";
    case 0xB8: return "End-to-End Error";
    case 0xBB: return "Uncorr ECC Count";
    case 0xBC: return "Command Timeout";
    case 0xBD: return "High Fly Writes";
    case 0xBE: return "Temp Difference";
    case 0xBF: return "G-Sense Errors";
    case 0xC0: return "Unsafe Shutdowns";
    case 0xC1: return "Load/Unload Cycles";
    case 0xC2: return "Temperature";
    case 0xC3: return "Hardware ECC Recov";
    case 0xC4: return "Realloc Event Cnt";
    case 0xC5: return "Pending Sectors";    // critical
    case 0xC6: return "Uncorrectable Sects";// critical
    case 0xC7: return "UDMA CRC Errors";
    case 0xC8: return "Write Error Rate";
    case 0xCA: return "Data Addr Mark Errs";
    case 0xCB: return "Run Out Cancel";
    case 0xF0: return "Head Flying Hours";
    case 0xF1: return "Total LBA Written";
    case 0xF2: return "Total LBA Read";
    case 0xFE: return "Free Fall Protect";
    default:   return NULL;
    }
}

// Is this attribute ID one we consider health-critical?
static bool SmartAttrCritical(BYTE id)
{
    return (id == 0x05 || id == 0xC5 || id == 0xC6);
}

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
// Data loading
// ============================================================================

static void LoadData()
{
    HddData& d = s_data;

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

        // UDMA mode (word 88: bits [13:8] = active, bits [5:0] = supported)
        // Active bits are set by the host controller after negotiation.
        // At first IDENTIFY they may be 0 even if the BIOS has programmed
        // UDMA — fall back to highest supported bit in that case.
        BYTE udmaActive = (BYTE)(w[88] >> 8) & 0x7F;
        BYTE udmaSupported = (BYTE)(w[88]) & 0x7F;
        int  udmaA = -1, udmaS = -1;
        for (int i = 6; i >= 0; --i)
        {
            if (udmaA < 0 && (udmaActive & (1 << i))) udmaA = i;
            if (udmaS < 0 && (udmaSupported & (1 << i))) udmaS = i;
        }
        if (udmaA >= 0)
        {
            // Host confirmed active mode
            StrCopy(d.udmaMode, sizeof(d.udmaMode), "UDMA");
            IntToStr(udmaA, t, sizeof(t));
            StrCat2(d.udmaMode, sizeof(d.udmaMode), d.udmaMode, t);
        }
        else if (udmaS >= 0)
        {
            // Controller hasn't written active bit yet — show supported max
            StrCopy(d.udmaMode, sizeof(d.udmaMode), "UDMA");
            IntToStr(udmaS, t, sizeof(t));
            StrCat2(d.udmaMode, sizeof(d.udmaMode), d.udmaMode, t);
            StrCat2(d.udmaMode, sizeof(d.udmaMode), d.udmaMode, "*");
        }
        else
        {
            // No UDMA — check MWDMA (word 63 high=active, low=supported)
            BYTE mwA = (BYTE)(w[63] >> 8) & 0x07;
            BYTE mwS = (BYTE)(w[63]) & 0x07;
            int  mwMode = -1;
            for (int i = 2; i >= 0; --i)
                if ((mwA & (1 << i)) || (mwS & (1 << i))) { mwMode = i; break; }
            if (mwMode >= 0)
            {
                StrCopy(d.udmaMode, sizeof(d.udmaMode), "MWDMA");
                IntToStr(mwMode, t, sizeof(t));
                StrCat2(d.udmaMode, sizeof(d.udmaMode), d.udmaMode, t);
            }
            else StrCopy(d.udmaMode, sizeof(d.udmaMode), "PIO");
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
        d.securitySupported = (w[128] & 0x01) != 0;
        d.isLocked = (w[128] & 0x04) != 0;

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
    // Reads all 256 bytes atomically through the kernel, avoiding raw SMBus
    // contention with the dashboard/other EEPROM users (ref: PrometheOS XKEEPROM).
    {
        BYTE   eepBuf[256];
        ULONG  eeType = 0, eeLen = 0;
        LONG   eeStatus = ExQueryNonVolatileSetting(0xFFFF, &eeType, eepBuf, 256, &eeLen);
        d.eepromOK = (eeStatus == 0 && eeLen >= 0x58);

        if (d.eepromOK)
        {
            // HDD key: bytes 0x1C-0x2B (16 bytes)
            for (int i = 0; i < 16; ++i) d.hddKey[i] = eepBuf[0x1C + i];
            // XBE Region: bytes 0x2C-0x2F (4 bytes)
            for (int i = 0; i < 4; ++i) d.xbeRegion[i] = eepBuf[0x2C + i];
            // Online key: bytes 0x48-0x57 (16 bytes)
            for (int i = 0; i < 16; ++i) d.onlineKey[i] = eepBuf[0x48 + i];

            // Region from XBERegion[0]
            d.regionByte = d.xbeRegion[0];
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

            // Serial: bytes 0x34-0x3F (12 bytes ASCII)
            for (int i = 0; i < 12; ++i) d.serialEEPROM[i] = (char)eepBuf[0x34 + i];
            d.serialEEPROM[12] = '\0';

        }
        else
        {
            StrCopy(d.serialEEPROM, sizeof(d.serialEEPROM), "EEPROM READ FAILED");
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

// ============================================================================
// SMART export  (D:\smart.txt)
// ============================================================================

static void ExportSmart()
{
    HddData& d = s_data;
    d.smartExportDone = false;
    d.smartExportOK = false;

    HANDLE hf = CreateFileA("D:\\smart.txt", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        d.smartExportDone = true; return;
    }

    DWORD w;
    char line[128];

    const char* hdr = "XbDiag SMART Data\r\n==================\r\n";
    WriteFile(hf, hdr, StrLen(hdr), &w, NULL);

    // Drive identity header
    StrCopy(line, sizeof(line), "Drive:    "); StrCat2(line, sizeof(line), line, d.model);
    StrCat2(line, sizeof(line), line, "\r\n"); WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Serial:   "); StrCat2(line, sizeof(line), line, d.serial);
    StrCat2(line, sizeof(line), line, "\r\n"); WriteFile(hf, line, StrLen(line), &w, NULL);

    if (!d.smartOK)
    {
        const char* msg = "SMART: Not supported or read failed\r\n";
        WriteFile(hf, msg, StrLen(msg), &w, NULL);
        FlushFileBuffers(hf); CloseHandle(hf);
        d.smartExportDone = true; d.smartExportOK = true;
        return;
    }

    const char* col = "\r\nID    Name                 Cur  Wst  Thr  Raw\r\n"
        "----  -------------------  ---  ---  ---  ------------\r\n";
    WriteFile(hf, col, StrLen(col), &w, NULL);

    // SMART data starts at byte 2, 30 entries of 12 bytes each
    const BYTE* p = d.smartBuf + 2;
    for (int i = 0; i < SMART_MAX_ATTRS; ++i, p += SMART_ATTR_SIZE)
    {
        BYTE id = p[0];
        if (id == 0) continue;  // empty slot

        BYTE cur = p[3];
        BYTE wst = p[4];
        BYTE thr = p[5];
        // Raw value: 6 bytes little-endian, show as 48-bit hex
        const BYTE* raw = p + 6;

        char idStr[4];   IntToHex(id, 2, idStr, sizeof(idStr));
        char curStr[4];  IntToStr(cur, curStr, sizeof(curStr));
        char wstStr[4];  IntToStr(wst, wstStr, sizeof(wstStr));
        char thrStr[4];  IntToStr(thr, thrStr, sizeof(thrStr));
        char rawStr[20];
        // Raw as 6 hex bytes space-separated
        rawStr[0] = '\0';
        char rb[4];
        for (int j = 5; j >= 0; --j)   // big-endian display
        {
            IntToHex(raw[j], 2, rb, sizeof(rb));
            StrCat2(rawStr, sizeof(rawStr), rawStr, rb);
            if (j > 0) StrCat2(rawStr, sizeof(rawStr), rawStr, " ");
        }

        const char* name = SmartAttrName(id);
        char nameBuf[22];
        if (name)
        {
            StrCopy(nameBuf, sizeof(nameBuf), name);
        }
        else
        {
            StrCopy(nameBuf, sizeof(nameBuf), "Attr 0x");
            StrCat2(nameBuf, sizeof(nameBuf), nameBuf, idStr);
        }
        // Pad name to 20 chars
        int nl = (int)StrLen(nameBuf);
        while (nl < 20) { nameBuf[nl++] = ' '; }
        nameBuf[20] = '\0';

        // Pad cur/wst/thr to 3 chars right-aligned
        char cStr[5]; StrCopy(cStr, sizeof(cStr), cur < 100 ? (cur < 10 ? "  " : " ") : "");
        StrCat2(cStr, sizeof(cStr), cStr, curStr);
        char wStr[5]; StrCopy(wStr, sizeof(wStr), wst < 100 ? (wst < 10 ? "  " : " ") : "");
        StrCat2(wStr, sizeof(wStr), wStr, wstStr);
        char tStr[5]; StrCopy(tStr, sizeof(tStr), thr < 100 ? (thr < 10 ? "  " : " ") : "");
        StrCat2(tStr, sizeof(tStr), tStr, thrStr);

        StrCopy(line, sizeof(line), idStr);
        StrCat2(line, sizeof(line), line, "    ");
        StrCat2(line, sizeof(line), line, nameBuf);
        StrCat2(line, sizeof(line), line, "  ");
        StrCat2(line, sizeof(line), line, cStr);
        StrCat2(line, sizeof(line), line, "  ");
        StrCat2(line, sizeof(line), line, wStr);
        StrCat2(line, sizeof(line), line, "  ");
        StrCat2(line, sizeof(line), line, tStr);
        StrCat2(line, sizeof(line), line, "  ");
        StrCat2(line, sizeof(line), line, rawStr);
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FlushFileBuffers(hf);
    CloseHandle(hf);
    d.smartExportDone = true;
    d.smartExportOK = true;
}


void HddInfo_OnEnter()
{
    s_prevBtns = 0;
    s_view = VIEW_INFO;
    s_loaded = false;
    // Zero the data struct on every entry — LoadData uses StrCopy/StrCat2
    // into fields that assume a clean buffer, and exportDone/exportOK must
    // never carry a stale fail state from a previous visit.
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
static void DrawKeyRow(float lx, float vx, float y,
    const char* label, const BYTE* data, int count,
    bool eepromOK)
{
    DrawText(lx, y, label, 1.2f, COL_GRAY);
    if (!eepromOK)
    {
        DrawText(vx, y, "EEPROM NAK", 1.2f, COL_RED);
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
        ? (s_data.exportOK ? "[A] Exported OK    [Right] SMART    [B] Back"
            : "[A] Export failed  [Right] SMART    [B] Back")
        : "[A] Export    [Right] SMART    [B] Back";

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

    // ---- RIGHT: SECURITY / EEPROM ----
    DrawText(COL_R, y2, "EEPROM SECURITY", 1.3f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 6.f;

    DrawRow(COL_R, COL_VR, y2, "SERIAL  :", d.serialEEPROM,
        d.eepromOK ? COL_WHITE : COL_RED);                 y2 += LH;
    DrawRow(COL_R, COL_VR, y2, "REGION  :", d.regionStr,
        COL_CYAN);                                         y2 += LH + GAP;

    // HDD key (16 bytes = 2 rows)
    DrawText(COL_R, y2, "HDD KEY", 1.2f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 5.f;
    DrawKeyRow(COL_R, COL_VR, y2, "BYTES   :", d.hddKey, 16, d.eepromOK);
    y2 += LH * 2.f + GAP;

    // XBE Region (4 bytes = 1 row)
    DrawText(COL_R, y2, "XBE REGION", 1.2f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 5.f;
    DrawKeyRow(COL_R, COL_VR, y2, "BYTES   :", d.xbeRegion, 4, d.eepromOK);
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

static void RenderSmart(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    const HddData& d = s_data;

    const char* hint;
    if (!d.smartOK)
        hint = "[Left] Drive Info    [B] Back";
    else if (d.smartExportDone)
        hint = d.smartExportOK
        ? "[A] Exported OK    [Left] Drive Info    [B] Back"
        : "[A] Export failed  [Left] Drive Info    [B] Back";
    else
        hint = "[A] Export    [Left] Drive Info    [B] Back";

    DrawPageChrome(logo, "HDD SMART", hint);

    float y = CONTENT_Y + 6.f;
    const float LH2 = LINE_H - 1.f;

    if (!d.smartSupported)
    {
        DrawText(LM, y, "SMART not supported by this drive.", 1.3f, COL_GRAY);
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }
    if (!d.smartOK)
    {
        DrawText(LM, y, "SMART READ DATA failed.", 1.3f, COL_RED);
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // Column headers
    const float CX_ID = LM;
    const float CX_NAME = LM + 28.f;
    const float CX_CUR = LM + 230.f;
    const float CX_WST = LM + 264.f;
    const float CX_THR = LM + 298.f;
    const float CX_RAW = LM + 336.f;

    DrawText(CX_ID, y, "ID", 1.1f, COL_GRAY);
    DrawText(CX_NAME, y, "ATTRIBUTE", 1.1f, COL_GRAY);
    DrawText(CX_CUR, y, "CUR", 1.1f, COL_GRAY);
    DrawText(CX_WST, y, "WST", 1.1f, COL_GRAY);
    DrawText(CX_THR, y, "THR", 1.1f, COL_GRAY);
    DrawText(CX_RAW, y, "RAW VALUE", 1.1f, COL_GRAY);
    y += LH2 - 2.f;
    HLine(y, LM, SW - LM, COL_BORDER);
    y += 3.f;

    const BYTE* p = d.smartBuf + 2;
    for (int i = 0; i < SMART_MAX_ATTRS; ++i, p += SMART_ATTR_SIZE)
    {
        BYTE id = p[0];
        if (id == 0) continue;

        BYTE cur = p[3];
        BYTE wst = p[4];
        BYTE thr = p[5];
        const BYTE* raw = p + 6;

        // Alternate row tint
        if ((i & 1) == 0)
            FillRect(LM - 2.f, y - 1.f, SW - LM, y + LH2 - 1.f, 0x10FFFFFF);

        // ID
        char idStr[4]; IntToHex(id, 2, idStr, sizeof(idStr));
        DrawText(CX_ID, y, idStr, 1.05f, COL_DIM);

        // Name — critical attrs in red if raw != 0
        const char* name = SmartAttrName(id);
        char nameBuf[22];
        if (name) StrCopy(nameBuf, sizeof(nameBuf), name);
        else { StrCopy(nameBuf, sizeof(nameBuf), "Attr 0x"); StrCat2(nameBuf, sizeof(nameBuf), nameBuf, idStr); }

        bool isCrit = SmartAttrCritical(id);
        // Check if raw value is non-zero (any of 6 bytes)
        bool rawNonZero = false;
        for (int j = 0; j < 6; ++j) if (raw[j]) { rawNonZero = true; break; }
        DWORD nameCol = (isCrit && rawNonZero) ? COL_RED : COL_WHITE;
        DrawText(CX_NAME, y, nameBuf, 1.1f, nameCol);

        // Cur / Wst / Thr — highlight if below threshold
        DWORD valCol = (cur <= thr && thr > 0) ? COL_RED : COL_CYAN;
        char curStr[4]; IntToStr(cur, curStr, sizeof(curStr));
        char wstStr[4]; IntToStr(wst, wstStr, sizeof(wstStr));
        char thrStr[4]; IntToStr(thr, thrStr, sizeof(thrStr));
        DrawText(CX_CUR, y, curStr, 1.05f, valCol);
        DrawText(CX_WST, y, wstStr, 1.05f, COL_GRAY);
        DrawText(CX_THR, y, thrStr, 1.05f, COL_DIM);

        // Raw — 6 bytes as 3 space-separated pairs (most significant first)
        char rawStr[20]; rawStr[0] = '\0';
        char rb[4];
        for (int j = 5; j >= 0; --j)
        {
            IntToHex(raw[j], 2, rb, sizeof(rb));
            StrCat2(rawStr, sizeof(rawStr), rawStr, rb);
            if (j > 0) StrCat2(rawStr, sizeof(rawStr), rawStr, " ");
        }
        DrawText(CX_RAW, y, rawStr, 1.0f, rawNonZero ? COL_YELLOW : COL_DIM);

        y += LH2;
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

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
        RequestState(MSTATE_MENU);
        s_prevBtns = cur;
        return;
    }

    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT))
        s_view = VIEW_SMART;
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
        s_view = VIEW_INFO;

    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        if (s_view == VIEW_INFO)
            ExportData();
        else
            ExportSmart();
    }

    s_prevBtns = cur;

    if (s_view == VIEW_SMART)
        RenderSmart(logo);
    else
        Render(logo);
}