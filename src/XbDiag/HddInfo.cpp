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
//  Firmware rev                      HDD Key     (16 bytes hex)
//  Capacity                          XBE Region  (4 bytes hex)
//  Interface (UDMA mode)             Online Key  (16 bytes hex)
//  Buffer size
//  LBA28 / LBA48 sectors
//  Security state (locked/unlocked)
//
//  [BOT BAR]  [A] Export    [Right] SMART    [RT] Bench    [B] Back
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

    // Partitions (C/E/F/G — X/Y/Z cache skipped)
    struct PartInfo {
        char   letter;
        bool   present;
        char   totalStr[12];   // "XX.X GB"
        char   freeStr[12];    // "XX.X GB"
        float  freeRatio;      // free/total 0.0-1.0 for bar render
    } parts[4];  // [0]=C [1]=E [2]=F [3]=G

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
enum HddView { VIEW_INFO = 0, VIEW_SMART, VIEW_BENCH };

static HddData s_data;
static WORD    s_prevBtns;
static bool    s_loaded;
static HddView s_view;

#define BENCH_FILE         "E:\\xbdiag_bench.tmp"
#define BENCH_FILE_SIZE    (64 * 1024 * 1024)
#define BENCH_CHUNK        (64 * 1024)
#define BENCH_SEEK_ITERS   1024
#define BENCH_CACHE_BLOCK  (512 * 1024)   // 512 KB per HDD cache pass
#define BENCH_CACHE_PASSES 64             // 32 MB total
#define BENCH_4K_ITERS     2048           // 4K random reads for SSD test
#define BENCH_4K_SIZE      4096           // 4 KB = one flash page

enum BenchState
{
    BENCH_IDLE = 0,
    BENCH_CONFIRM,
    BENCH_WRITE,
    BENCH_RAW_WR,
    BENCH_READ,
    BENCH_CACHE_RD,     // HDD: repeated 512KB reads, platter buffer
    BENCH_4K_RAND,      // SSD: 4K random reads, IOPS
    BENCH_SEEK,
    BENCH_DONE
};

struct BenchData
{
    BenchState  state;
    bool        readOnly;           // true = write failed, fell back to read-only
    char        readSrc[64];        // path of file used for read/seek test

    // Write
    HANDLE      hWrite;
    DWORD       writeTotal;         // bytes written so far
    DWORD       writeT0;
    float       writeMBs;           // result

    // Raw write
    HANDLE      hRawWr;
    DWORD       rawWrTotal;
    DWORD       rawWrT0;
    float       rawWrMBs;

    // Read
    HANDLE      hRead;
    DWORD       readTotal;
    DWORD       readT0;
    float       readMBs;            // result

    // Cache read (HDD)
    HANDLE      hCache;
    int         cachePass;
    DWORD       cacheTotal;
    DWORD       cacheT0;
    float       cacheMBs;

    // 4K random read (SSD)
    HANDLE      hRand4k;
    int         rand4kIdx;
    DWORD       rand4kT0;
    float       rand4kMBs;
    float       rand4kIOPS;

    // Seek
    HANDLE      hSeek;
    int         seekIdx;
    DWORD       seekT0;
    float       seekMs;             // result avg ms

    // Cleanup
    bool        tmpExists;          // we own BENCH_FILE and must delete it

    // Export
    bool        exportDone;
    bool        exportOK;

    // Error message (fallback path or failure note)
    char        statusMsg[80];

    // Render pacing — IO runs in a tight loop; screen updates at ~5fps
    DWORD       nextRender;
};

static BenchData   s_bench;
// 512-byte alignment required by FILE_FLAG_NO_BUFFERING
static __declspec(align(512)) BYTE s_benchBuf[BENCH_CHUNK];

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


static void BenchCleanup();  // forward declaration

void HddInfo_OnEnter()
{
    s_prevBtns = 0;
    s_view = VIEW_INFO;
    s_loaded = false;
    // Cleanup any leftover benchmark temp file from a prior visit
    BenchCleanup();
    ZeroMemory(&s_bench, sizeof(s_bench));
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
        ? "[A] Saved OK    [Left] Drive Info    [B] Back"
        : "[A] Save failed [Left] Drive Info    [B] Back";
    else
        hint = "[A] Save smart.txt    [Left] Drive Info    [B] Back";

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
// HDD Benchmark
// ============================================================================



// Simple LCG for seek offsets — no CRT rand needed
static DWORD BenchRand(DWORD& seed)
{
    seed = seed * 1664525UL + 1013904223UL;
    return seed;
}

static void BenchFindReadSource()
{
    // Fallback only: find largest existing file on E:\ for read-only mode.
    // Normal benchmarks always use the written temp file so conditions are symmetric.
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("E:\\*", &fd);
    DWORD  bestSize = 0;
    s_bench.readSrc[0] = '\0';

    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (fd.nFileSizeLow > bestSize)
            {
                bestSize = fd.nFileSizeLow;
                StrCopy(s_bench.readSrc, sizeof(s_bench.readSrc), "E:\\");
                StrCat2(s_bench.readSrc, sizeof(s_bench.readSrc),
                    s_bench.readSrc, fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static void BenchStartRead()
{
    // Buffered sequential read — matches write path for apples-to-apples
    // filesystem throughput.  Both phases now measure the same stack.
    s_bench.hRead = CreateFileA(s_bench.readSrc, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (s_bench.hRead == INVALID_HANDLE_VALUE)
    {
        // Read source unreadable — skip straight to seek or done
        s_bench.readMBs = 0.f;
        s_bench.state = BENCH_SEEK;
        StrCopy(s_bench.statusMsg, sizeof(s_bench.statusMsg), "Read source unavailable");
        return;
    }
    s_bench.readTotal = 0;
    s_bench.readT0 = GetTickCount();
    s_bench.state = BENCH_READ;
}

static void BenchStart()
{
    ZeroMemory(&s_bench, sizeof(s_bench));

    // Fill write buffer with a simple pattern
    for (int i = 0; i < BENCH_CHUNK; i += 4)
    {
        s_benchBuf[i + 0] = 0xDE; s_benchBuf[i + 1] = 0xAD;
        s_benchBuf[i + 2] = 0xBE; s_benchBuf[i + 3] = 0xEF;
    }

    // Attempt write on E:\ first, fall back to D:\ (XBE dir, always writable)
    const char* writePaths[2] = { BENCH_FILE, "D:\\xbdiag_bench.tmp" };
    for (int wi = 0; wi < 2; ++wi)
    {
        s_bench.hWrite = CreateFileA(writePaths[wi], GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        // Buffered sequential write — OS coalesces writes efficiently.
        // FlushFileBuffers() at the end commits to disk so elapsed time
        // reflects sustained write throughput, not just cache fill rate.
        if (s_bench.hWrite != INVALID_HANDLE_VALUE)
        {
            StrCopy(s_bench.readSrc, sizeof(s_bench.readSrc), writePaths[wi]);
            s_bench.tmpExists = true;
            // Preallocate: set file to full size so the timed write pass
            // overwrites a pre-laid-out extent instead of growing the file.
            // This removes FATX cluster allocation from the timed measurement.
            SetFilePointer(s_bench.hWrite, BENCH_FILE_SIZE, NULL, FILE_BEGIN);
            SetEndOfFile(s_bench.hWrite);
            SetFilePointer(s_bench.hWrite, 0, NULL, FILE_BEGIN);
            s_bench.writeTotal = 0;
            s_bench.state = BENCH_CONFIRM;
            return;
        }
    }

    // Both write paths failed — true read-only fallback
    s_bench.readOnly = true;
    s_bench.writeMBs = 0.f;
    // Capture error code for diagnosis
    DWORD writeErr = GetLastError();
    char errCode[12];
    IntToStr((int)writeErr, errCode, sizeof(errCode));

    // Try to find an existing file to benchmark reads against
    BenchFindReadSource();
    if (s_bench.readSrc[0] != '\0')
    {
        StrCopy(s_bench.statusMsg, sizeof(s_bench.statusMsg),
            "Write failed (err ");
        StrCat2(s_bench.statusMsg, sizeof(s_bench.statusMsg),
            s_bench.statusMsg, errCode);
        StrCat2(s_bench.statusMsg, sizeof(s_bench.statusMsg),
            s_bench.statusMsg, ") - READ ONLY MODE");
        BenchStartRead();
        return;
    }

    // Nothing to read either — can't benchmark
    StrCopy(s_bench.statusMsg, sizeof(s_bench.statusMsg),
        "Write failed (err ");
    StrCat2(s_bench.statusMsg, sizeof(s_bench.statusMsg),
        s_bench.statusMsg, errCode);
    StrCat2(s_bench.statusMsg, sizeof(s_bench.statusMsg),
        s_bench.statusMsg, ") - no readable files found");
    s_bench.state = BENCH_DONE;
}

static void BenchCleanup()
{
    if (s_bench.hWrite != INVALID_HANDLE_VALUE && s_bench.hWrite != NULL)
    {
        CloseHandle(s_bench.hWrite); s_bench.hWrite = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hRawWr != INVALID_HANDLE_VALUE && s_bench.hRawWr != NULL)
    {
        CloseHandle(s_bench.hRawWr); s_bench.hRawWr = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hRead != INVALID_HANDLE_VALUE && s_bench.hRead != NULL)
    {
        CloseHandle(s_bench.hRead); s_bench.hRead = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hCache != INVALID_HANDLE_VALUE && s_bench.hCache != NULL)
    {
        CloseHandle(s_bench.hCache); s_bench.hCache = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hRand4k != INVALID_HANDLE_VALUE && s_bench.hRand4k != NULL)
    {
        CloseHandle(s_bench.hRand4k); s_bench.hRand4k = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hSeek != INVALID_HANDLE_VALUE && s_bench.hSeek != NULL)
    {
        CloseHandle(s_bench.hSeek); s_bench.hSeek = INVALID_HANDLE_VALUE;
    }
    if (s_bench.tmpExists)
    {
        // Delete whichever path was used (readSrc holds the write path)
        if (s_bench.readSrc[0] != '\0')
            DeleteFileA(s_bench.readSrc);
        else
            DeleteFileA(BENCH_FILE);
        s_bench.tmpExists = false;
    }
}

static void ExportBench()
{
    s_bench.exportDone = false;
    s_bench.exportOK = false;

    HANDLE hf = CreateFileA("D:\\hddbench.txt", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        s_bench.exportDone = true; return;
    }

    DWORD w;
    char  line[128];

    const char* hdr = s_data.isSSD
        ? "XbDiag HDD Benchmark (SSD)\r\n===========================\r\n"
        "FS WR:  buffered preallocated write+flush\r\n"
        "RAW WR: FILE_FLAG_NO_BUFFERING overwrite\r\n"
        "SEQ RD: FILE_FLAG_NO_BUFFERING sequential\r\n"
        "4K RND: 2048x 4KB random reads (IOPS)\r\n"
        "SEEK:   1024 random 512-byte reads\r\n\r\n"
        : "XbDiag HDD Benchmark (HDD)\r\n===========================\r\n"
        "FS WR:  buffered preallocated write+flush\r\n"
        "RAW WR: FILE_FLAG_NO_BUFFERING overwrite\r\n"
        "SEQ RD: FILE_FLAG_NO_BUFFERING sequential\r\n"
        "CACHE:  512KB x64 passes (platter buffer bandwidth)\r\n"
        "SEEK:   1024 random 512-byte reads\r\n\r\n";
    WriteFile(hf, hdr, StrLen(hdr), &w, NULL);

    // Drive identity
    StrCopy(line, sizeof(line), "Drive:      ");
    StrCat2(line, sizeof(line), line, s_data.model);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    StrCopy(line, sizeof(line), "Serial:     ");
    StrCat2(line, sizeof(line), line, s_data.serial);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    StrCopy(line, sizeof(line), "Interface:  ");
    StrCat2(line, sizeof(line), line, s_data.udmaMode);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    StrCopy(line, sizeof(line), "Mode:       ");
    StrCat2(line, sizeof(line), line, s_bench.readOnly ? "READ ONLY (write failed)" : "Full read/write");
    StrCat2(line, sizeof(line), line, "\r\n\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    // Results — format float as "XX.X"
    auto FmtFloat = [](float v, char* out, int outLen) {
        int whole = Ftoi(v);
        float frac = v - (float)whole;
        if (frac < 0.f) frac = 0.f;
        int dec = Ftoi(frac * 10.f);
        char t[12];
        IntToStr(whole, t, sizeof(t));
        StrCopy(out, outLen, t);
        StrCat2(out, outLen, out, ".");
        IntToStr(dec, t, sizeof(t));
        StrCat2(out, outLen, out, t);
        };

    char val[16];

    if (!s_bench.readOnly)
    {
        FmtFloat(s_bench.writeMBs, val, sizeof(val));
        StrCopy(line, sizeof(line), "FS Write:    ");
        StrCat2(line, sizeof(line), line, val);
        StrCat2(line, sizeof(line), line, " MB/s\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);

        FmtFloat(s_bench.rawWrMBs, val, sizeof(val));
        StrCopy(line, sizeof(line), "Raw Write:   ");
        StrCat2(line, sizeof(line), line, val);
        StrCat2(line, sizeof(line), line, " MB/s\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FmtFloat(s_bench.readMBs, val, sizeof(val));
    StrCopy(line, sizeof(line), "Seq Read:    ");
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, " MB/s\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    if (s_data.isSSD)
    {
        char iopsEx[12]; IntToStr(Ftoi(s_bench.rand4kIOPS), iopsEx, sizeof(iopsEx));
        StrCopy(line, sizeof(line), "4K Random:   ");
        StrCat2(line, sizeof(line), line, iopsEx);
        StrCat2(line, sizeof(line), line, " IOPS\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }
    else
    {
        FmtFloat(s_bench.cacheMBs, val, sizeof(val));
        StrCopy(line, sizeof(line), "Cache Read:  ");
        StrCat2(line, sizeof(line), line, val);
        StrCat2(line, sizeof(line), line, " MB/s\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FmtFloat(s_bench.seekMs, val, sizeof(val));
    StrCopy(line, sizeof(line), "Seek Time:   ");
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, " ms avg\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    if (s_bench.readOnly && s_bench.statusMsg[0])
    {
        StrCopy(line, sizeof(line), "Note:        ");
        StrCat2(line, sizeof(line), line, s_bench.statusMsg);
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FlushFileBuffers(hf);
    CloseHandle(hf);
    s_bench.exportDone = true;
    s_bench.exportOK = true;
}

// BenchTick — called every frame, but IO runs in a tight inner loop.
//
// The old design did one 64KB chunk per frame, then rendered.  At 60fps that
// caps throughput at 64KB * 60 = 3.84 MB/s regardless of drive speed.
//
// Fix: loop IO for ~200ms per call (5fps render rate), yielding for a render
// only when nextRender is due.  The measured elapsed time comes from
// GetTickCount() bracketing the tight IO loop, so vsync never enters the
// denominator.  Seek latency is unaffected — each seek is its own operation
// and timing is per-seek, not per-frame.
static void BenchTick()
{
    switch (s_bench.state)
    {
    case BENCH_CONFIRM:
        // Waiting for user to press [A] — handled in input, not here
        break;

    case BENCH_WRITE:
    {
        // Tight write loop — run until file is done, no per-frame yield.
        // Timer brackets only the WriteFile calls so render overhead is excluded.
        while (s_bench.writeTotal < (DWORD)BENCH_FILE_SIZE)
        {
            DWORD remaining = (DWORD)BENCH_FILE_SIZE - s_bench.writeTotal;
            DWORD toWrite = (remaining < BENCH_CHUNK) ? remaining : BENCH_CHUNK;

            DWORD written = 0;
            WriteFile(s_bench.hWrite, s_benchBuf, toWrite, &written, NULL);
            s_bench.writeTotal += written;
            if (written == 0) break;  // write error
        }

        // Flush to disk before timing stops — measures true write throughput
        FlushFileBuffers(s_bench.hWrite);
        CloseHandle(s_bench.hWrite);
        s_bench.hWrite = INVALID_HANDLE_VALUE;

        DWORD elapsed = GetTickCount() - s_bench.writeT0;
        if (elapsed > 0)
            s_bench.writeMBs = (float)s_bench.writeTotal / 1048576.f
            / ((float)elapsed / 1000.f);

        // Open same preallocated file for unbuffered overwrite
        s_bench.hRawWr = CreateFileA(s_bench.readSrc, GENERIC_WRITE, 0,
            NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
        if (s_bench.hRawWr == INVALID_HANDLE_VALUE)
        {
            s_bench.rawWrMBs = 0.f; BenchStartRead();
        }
        else
        {
            s_bench.rawWrTotal = 0;
            s_bench.rawWrT0 = GetTickCount();
            s_bench.state = BENCH_RAW_WR;
        }
        break;
    }

    case BENCH_RAW_WR:
    {
        // Unbuffered overwrite — 512-byte aligned buffer, no OS cache
        while (s_bench.rawWrTotal < (DWORD)BENCH_FILE_SIZE)
        {
            DWORD rem = (DWORD)BENCH_FILE_SIZE - s_bench.rawWrTotal;
            DWORD nw = 0;
            WriteFile(s_bench.hRawWr, s_benchBuf,
                rem < BENCH_CHUNK ? rem : BENCH_CHUNK, &nw, NULL);
            s_bench.rawWrTotal += nw;
            if (nw == 0) break;
        }
        CloseHandle(s_bench.hRawWr); s_bench.hRawWr = INVALID_HANDLE_VALUE;
        DWORD rawEl = GetTickCount() - s_bench.rawWrT0;
        if (rawEl > 0 && s_bench.rawWrTotal > 0)
            s_bench.rawWrMBs = (float)s_bench.rawWrTotal / 1048576.f
            / ((float)rawEl / 1000.f);
        BenchStartRead();
        break;
    }

    case BENCH_READ:
    {
        // Tight read loop — same approach as write.
        while (s_bench.readTotal < (DWORD)BENCH_FILE_SIZE)
        {
            DWORD bytesRead = 0;
            ReadFile(s_bench.hRead, s_benchBuf, BENCH_CHUNK, &bytesRead, NULL);
            s_bench.readTotal += bytesRead;
            if (bytesRead == 0) break;  // EOF or error
        }

        CloseHandle(s_bench.hRead);
        s_bench.hRead = INVALID_HANDLE_VALUE;

        DWORD elapsed = GetTickCount() - s_bench.readT0;
        if (elapsed > 0 && s_bench.readTotal > 0)
            s_bench.readMBs = (float)s_bench.readTotal / 1048576.f
            / ((float)elapsed / 1000.f);

        if (s_data.isSSD)
        {
            s_bench.hRand4k = CreateFileA(s_bench.readSrc, GENERIC_READ,
                FILE_SHARE_READ, NULL, OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, NULL);
            if (s_bench.hRand4k == INVALID_HANDLE_VALUE)
            {
                s_bench.rand4kMBs = 0.f; s_bench.rand4kIOPS = 0.f;
                s_bench.state = BENCH_SEEK;
            }
            else
            {
                s_bench.rand4kIdx = 0;
                s_bench.rand4kT0 = GetTickCount();
                s_bench.state = BENCH_4K_RAND;
            }
        }
        else
        {
            s_bench.hCache = CreateFileA(s_bench.readSrc, GENERIC_READ,
                FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
            if (s_bench.hCache == INVALID_HANDLE_VALUE)
            {
                s_bench.cacheMBs = 0.f; s_bench.state = BENCH_SEEK;
            }
            else
            {
                s_bench.cachePass = 0;
                s_bench.cacheTotal = 0;
                s_bench.cacheT0 = GetTickCount();
                s_bench.state = BENCH_CACHE_RD;
            }
        }
        break;
    }

    case BENCH_CACHE_RD:
    {
        // 512KB block repeated 64 times (32MB). Drive serves subsequent passes
        // from its internal read-ahead buffer — measures drive buffer bandwidth.
        while (s_bench.cachePass < BENCH_CACHE_PASSES)
        {
            SetFilePointer(s_bench.hCache, 0, NULL, FILE_BEGIN);
            DWORD passBytes = 0;
            while (passBytes < (DWORD)BENCH_CACHE_BLOCK)
            {
                DWORD toRead = BENCH_CHUNK;
                if (toRead > (DWORD)BENCH_CACHE_BLOCK - passBytes)
                    toRead = (DWORD)BENCH_CACHE_BLOCK - passBytes;
                DWORD nr = 0;
                ReadFile(s_bench.hCache, s_benchBuf, toRead, &nr, NULL);
                if (nr == 0) break;
                passBytes += nr;
                s_bench.cacheTotal += nr;
            }
            s_bench.cachePass++;
        }
        CloseHandle(s_bench.hCache); s_bench.hCache = INVALID_HANDLE_VALUE;
        DWORD cEl = GetTickCount() - s_bench.cacheT0;
        if (cEl > 0 && s_bench.cacheTotal > 0)
            s_bench.cacheMBs = (float)s_bench.cacheTotal / 1048576.f
            / ((float)cEl / 1000.f);

        // Open seek handle
        s_bench.hSeek = CreateFileA(s_bench.readSrc, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, NULL);
        if (s_bench.hSeek == INVALID_HANDLE_VALUE)
        {
            s_bench.seekMs = 0.f; s_bench.state = BENCH_DONE;
            if (s_bench.tmpExists)
            {
                DeleteFileA(BENCH_FILE); s_bench.tmpExists = false;
            }
        }
        else
        {
            s_bench.seekIdx = 0;
            s_bench.seekT0 = GetTickCount();
            s_bench.state = BENCH_SEEK;
        }
        break;
    }

    case BENCH_4K_RAND:
    {
        // SSD 4K random read test — time-sliced 30ms per frame.
        // Reports IOPS and MB/s.
        static DWORD seed4k = 0xCAFEBABE;
        DWORD fSize4k = GetFileSize(s_bench.hRand4k, NULL);
        if (fSize4k < BENCH_4K_SIZE) fSize4k = BENCH_4K_SIZE;
        DWORD range4k = (fSize4k / BENCH_4K_SIZE) - 1;

        DWORD sl4k = GetTickCount();
        while (s_bench.rand4kIdx < BENCH_4K_ITERS)
        {
            DWORD page = range4k > 0 ? (BenchRand(seed4k) % range4k) : 0;
            SetFilePointer(s_bench.hRand4k, (LONG)(page * BENCH_4K_SIZE), NULL, FILE_BEGIN);
            DWORD nr = 0;
            ReadFile(s_bench.hRand4k, s_benchBuf, BENCH_4K_SIZE, &nr, NULL);
            s_bench.rand4kIdx++;
            if ((GetTickCount() - sl4k) >= 30) break;
        }

        if (s_bench.rand4kIdx >= BENCH_4K_ITERS)
        {
            CloseHandle(s_bench.hRand4k); s_bench.hRand4k = INVALID_HANDLE_VALUE;
            DWORD el4k = GetTickCount() - s_bench.rand4kT0;
            if (el4k > 0)
            {
                float secs4k = (float)el4k / 1000.f;
                s_bench.rand4kIOPS = (float)BENCH_4K_ITERS / secs4k;
                s_bench.rand4kMBs = s_bench.rand4kIOPS * BENCH_4K_SIZE / 1048576.f;
            }
            s_bench.hSeek = CreateFileA(s_bench.readSrc, GENERIC_READ,
                FILE_SHARE_READ, NULL, OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, NULL);
            if (s_bench.hSeek == INVALID_HANDLE_VALUE)
            {
                s_bench.seekMs = 0.f; s_bench.state = BENCH_DONE;
                if (s_bench.tmpExists)
                {
                    DeleteFileA(BENCH_FILE); s_bench.tmpExists = false;
                }
            }
            else
            {
                s_bench.seekIdx = 0;
                s_bench.seekT0 = GetTickCount();
                s_bench.state = BENCH_SEEK;
            }
        }
        break;
    }

    case BENCH_SEEK:
    {
        // Seek runs one operation at a time so latency per-seek is meaningful.
        // Do a batch per frame but not a tight loop — each seek+read is ~10ms
        // so 16 per frame at 60fps = ~160ms/frame which would block rendering.
        // Use a time-sliced approach: run seeks for up to 30ms then yield.
        static DWORD seekSeed = 0xDEADBEEF;

        DWORD fileSize = GetFileSize(s_bench.hSeek, NULL);
        if (fileSize < 512) fileSize = 512;
        DWORD range = (fileSize / 512) - 1;

        DWORD sliceStart = GetTickCount();
        while (s_bench.seekIdx < BENCH_SEEK_ITERS)
        {
            DWORD sector = range > 0 ? (BenchRand(seekSeed) % range) : 0;
            DWORD offset = sector * 512;
            SetFilePointer(s_bench.hSeek, (LONG)offset, NULL, FILE_BEGIN);
            DWORD bytesRead = 0;
            ReadFile(s_bench.hSeek, s_benchBuf, 512, &bytesRead, NULL);
            s_bench.seekIdx++;
            // Yield for render every ~30ms so the progress display updates
            if ((GetTickCount() - sliceStart) >= 30) break;
        }

        if (s_bench.seekIdx >= BENCH_SEEK_ITERS)
        {
            CloseHandle(s_bench.hSeek);
            s_bench.hSeek = INVALID_HANDLE_VALUE;

            DWORD elapsed = GetTickCount() - s_bench.seekT0;
            if (BENCH_SEEK_ITERS > 0)
                s_bench.seekMs = (float)elapsed / (float)BENCH_SEEK_ITERS;

            if (s_bench.tmpExists)
            {
                DeleteFileA(BENCH_FILE); s_bench.tmpExists = false;
            }

            s_bench.state = BENCH_DONE;
        }
        break;
    }

    default:
        break;
    }
}

// Render benchmark bar (value 0..maxVal mapped to barW pixels)
static void DrawBenchBar(float x, float y, float barW, float val, float maxVal, DWORD col)
{
    float frac = val / maxVal;
    if (frac > 1.f) frac = 1.f;
    if (frac < 0.f) frac = 0.f;
    const float BH = 7.f;
    FillRect(x, y + 2.f, x + barW, y + 2.f + BH, D3DCOLOR_XRGB(20, 25, 40));
    DWORD dimR = ((col >> 16) & 0xFF) >> 1;
    DWORD dimG = ((col >> 8) & 0xFF) >> 1;
    DWORD dimB = ((col) & 0xFF) >> 1;
    FillRectGrad(x, y + 2.f, x + barW * frac, y + 2.f + BH, col,
        D3DCOLOR_XRGB(dimR, dimG, dimB));
    HLine(y + 2.f, x, x + barW, COL_BORDER);
    HLine(y + 2.f + BH, x, x + barW, COL_BORDER);
}

static void RenderBench(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    const char* hint;
    if (s_bench.state == BENCH_IDLE)
        hint = "[A] Start Benchmark    [Left] Drive Info    [B] Back";
    else if (s_bench.state == BENCH_DONE)
    {
        if (s_bench.exportDone)
            hint = s_bench.exportOK
            ? "[A] Saved OK    [Left] Drive Info    [B] Back"
            : "[A] Save failed [Left] Drive Info    [B] Back";
        else
            hint = "[A] Save hddbench.txt    [Left] Drive Info    [B] Back";
    }
    else
        if (s_bench.state == BENCH_CONFIRM)
            hint = "[A] Confirm start    [B] Cancel";
        else
            hint = "Benchmarking...    [B] Cancel";

    DrawPageChrome(logo, "HDD BENCHMARK", hint);

    float y = CONTENT_Y + 8.f;
    const float VX = LM + 140.f;
    const float BX = VX + 80.f;
    const float BW = 180.f;
    const float RLH = LINE_H + 2.f;

    // Drive info strip
    DrawText(LM, y, "DRIVE :", 1.2f, COL_GRAY);
    DrawText(VX, y, s_data.model, 1.2f, COL_CYAN);
    y += RLH;
    DrawText(LM, y, "IFACE :", 1.2f, COL_GRAY);
    DrawText(VX, y, s_data.udmaMode, 1.2f, COL_WHITE);
    // * = drive capability only; host active mode not confirmed by controller
    if (s_data.udmaMode[0] && s_data.udmaMode[StrLen(s_data.udmaMode) - 1] == '*')
        DrawText(VX + TW(s_data.udmaMode, 1.2f) + 4.f, y,
            "(drive cap, host mode unconfirmed)", 1.0f, COL_ORANGE);
    y += RLH + 4.f;
    HLine(y, LM, SW - LM, COL_BORDER);
    y += 6.f;

    // Read-only notice
    if (s_bench.readOnly)
    {
        DrawText(LM, y, "READ ONLY MODE", 1.2f, COL_YELLOW);
        y += RLH;
        DrawText(LM, y, s_bench.statusMsg, 1.1f, D3DCOLOR_XRGB(180, 140, 60));
        y += RLH + 4.f;
    }

    // Results
    auto FmtFloat = [](float v, char* out, int outLen) {
        int whole = Ftoi(v);
        float frac = v - (float)whole;
        if (frac < 0.f) frac = 0.f;
        int dec = Ftoi(frac * 10.f);
        char t[12];
        IntToStr(whole, t, sizeof(t));
        StrCopy(out, outLen, t);
        StrCat2(out, outLen, out, ".");
        IntToStr(dec, t, sizeof(t));
        StrCat2(out, outLen, out, t);
        };

    char valStr[20];

    // WRITE
    if (!s_bench.readOnly)
    {
        DrawText(LM, y, "FS WR :", 1.2f, COL_GRAY);
        if (s_bench.state == BENCH_WRITE)
        {
            // Progress during write
            DWORD pct = s_bench.writeTotal * 100 / BENCH_FILE_SIZE;
            char prog[12]; IntToStr((int)pct, prog, sizeof(prog));
            StrCat2(prog, sizeof(prog), prog, "%");
            DrawText(VX, y, prog, 1.2f, COL_YELLOW);
        }
        else if (s_bench.writeMBs > 0.f || s_bench.state > BENCH_WRITE)
        {
            FmtFloat(s_bench.writeMBs, valStr, sizeof(valStr));
            StrCat2(valStr, sizeof(valStr), valStr, " MB/s");
            DrawText(VX, y, valStr, 1.2f, COL_GREEN);
            DrawBenchBar(BX, y, BW, s_bench.writeMBs, 100.f, COL_GREEN);
        }
        else
            DrawText(VX, y, "---", 1.2f, COL_DIM);
        y += RLH;
    }

    // RAW WRITE
    if (!s_bench.readOnly)
    {
        DrawText(LM, y, "RAW WR:", 1.2f, COL_GRAY);
        if (s_bench.state == BENCH_RAW_WR)
        {
            DWORD pct = s_bench.rawWrTotal * 100 / BENCH_FILE_SIZE;
            char prog[12]; IntToStr((int)pct, prog, sizeof(prog));
            StrCat2(prog, sizeof(prog), prog, "%");
            DrawText(VX, y, prog, 1.2f, COL_YELLOW);
        }
        else if (s_bench.rawWrMBs > 0.f || (int)s_bench.state > (int)BENCH_RAW_WR)
        {
            FmtFloat(s_bench.rawWrMBs, valStr, sizeof(valStr));
            StrCat2(valStr, sizeof(valStr), valStr, " MB/s");
            DrawText(VX, y, valStr, 1.2f, COL_GREEN);
            DrawBenchBar(BX, y, BW, s_bench.rawWrMBs, 100.f, COL_GREEN);
        }
        else
            DrawText(VX, y, "---", 1.2f, COL_DIM);
        y += RLH;
    }

    // SEQ READ
    DrawText(LM, y, "SEQ RD:", 1.2f, COL_GRAY);
    if (s_bench.state == BENCH_READ)
    {
        DWORD pct = s_bench.readTotal * 100 / BENCH_FILE_SIZE;
        char prog[12]; IntToStr((int)pct, prog, sizeof(prog));
        StrCat2(prog, sizeof(prog), prog, "%");
        DrawText(VX, y, prog, 1.2f, COL_YELLOW);
    }
    else if (s_bench.readMBs > 0.f)
    {
        FmtFloat(s_bench.readMBs, valStr, sizeof(valStr));
        StrCat2(valStr, sizeof(valStr), valStr, " MB/s");
        DrawText(VX, y, valStr, 1.2f, COL_CYAN);
        DrawBenchBar(BX, y, BW, s_bench.readMBs, 120.f, COL_CYAN);
    }
    else
        DrawText(VX, y, "---", 1.2f, COL_DIM);
    y += RLH;

    // CACHE (HDD) / 4K RAND (SSD)
    if (s_data.isSSD)
    {
        DrawText(LM, y, "4K RND:", 1.2f, COL_GRAY);
        if (s_bench.state == BENCH_4K_RAND)
        {
            char p4k[16]; IntToStr(s_bench.rand4kIdx, p4k, sizeof(p4k));
            StrCat2(p4k, sizeof(p4k), p4k, "/2048");
            DrawText(VX, y, p4k, 1.2f, COL_YELLOW);
        }
        else if (s_bench.rand4kIOPS > 0.f)
        {
            char iopsStr[12], mbStr[12], result4k[40];
            IntToStr(Ftoi(s_bench.rand4kIOPS), iopsStr, sizeof(iopsStr));
            FmtFloat(s_bench.rand4kMBs, mbStr, sizeof(mbStr));
            StrCopy(result4k, sizeof(result4k), iopsStr);
            StrCat2(result4k, sizeof(result4k), result4k, " IOPS  (");
            StrCat2(result4k, sizeof(result4k), result4k, mbStr);
            StrCat2(result4k, sizeof(result4k), result4k, " MB/s)");
            DrawText(VX, y, result4k, 1.2f, COL_CYAN);
            DrawBenchBar(BX + 80.f, y, BW - 80.f, s_bench.rand4kIOPS, 50000.f, COL_CYAN);
        }
        else
            DrawText(VX, y, "---", 1.2f, COL_DIM);
    }
    else
    {
        DrawText(LM, y, "CACHE :", 1.2f, COL_GRAY);
        if (s_bench.state == BENCH_CACHE_RD)
        {
            char cprog[16]; IntToStr(s_bench.cachePass, cprog, sizeof(cprog));
            StrCat2(cprog, sizeof(cprog), cprog, "/64");
            DrawText(VX, y, cprog, 1.2f, COL_YELLOW);
        }
        else if (s_bench.cacheMBs > 0.f)
        {
            FmtFloat(s_bench.cacheMBs, valStr, sizeof(valStr));
            StrCat2(valStr, sizeof(valStr), valStr, " MB/s");
            DrawText(VX, y, valStr, 1.2f, COL_CYAN);
            DrawBenchBar(BX, y, BW, s_bench.cacheMBs, 200.f, COL_CYAN);
        }
        else
            DrawText(VX, y, "---", 1.2f, COL_DIM);
    }
    y += RLH;

    // SEEK
    DrawText(LM, y, "SEEK  :", 1.2f, COL_GRAY);
    if (s_bench.state == BENCH_SEEK)
    {
        char prog[16];
        IntToStr(s_bench.seekIdx, prog, sizeof(prog));
        StrCat2(prog, sizeof(prog), prog, "/1024");
        DrawText(VX, y, prog, 1.2f, COL_YELLOW);
    }
    else if (s_bench.seekMs > 0.f)
    {
        FmtFloat(s_bench.seekMs, valStr, sizeof(valStr));
        StrCat2(valStr, sizeof(valStr), valStr, " ms avg");
        DWORD seekCol = (s_bench.seekMs < 5.f) ? COL_GREEN
            : (s_bench.seekMs < 15.f) ? COL_CYAN : COL_YELLOW;
        DrawText(VX, y, valStr, 1.2f, seekCol);
        // Seek bar inverted: lower is better, max display 30ms
        float seekFrac = s_bench.seekMs / 30.f;
        if (seekFrac > 1.f) seekFrac = 1.f;
        DrawBenchBar(BX, y, BW, seekFrac, 1.f, seekCol);
    }
    else
        DrawText(VX, y, "---", 1.2f, COL_DIM);
    y += RLH;

    // Read source (fallback)
    if (s_bench.readOnly && s_bench.readSrc[0])
    {
        y += 4.f;
        DrawText(LM, y, "SOURCE:", 1.2f, COL_GRAY);
        DrawText(VX, y, s_bench.readSrc, 1.1f, D3DCOLOR_XRGB(160, 160, 160));
        y += RLH;
    }

    // Idle prompt
    if (s_bench.state == BENCH_IDLE)
    {
        y += 8.f;
        DrawText(LM, y, "Press [A] to start benchmark", 1.2f, COL_GRAY);
        y += LINE_H;
        DrawText(LM, y, "64MB write + read + 1024 seek ops", 1.1f, COL_DIM);
        DrawText(LM, y + LINE_H, "Buffered filesystem I/O  --  practical throughput", 1.1f, COL_DIM);
    }

    // Confirm overlay
    if (s_bench.state == BENCH_CONFIRM)
    {
        const float CW = 360.f;
        const float CH = 80.f;
        const float CX = (SW - CW) * 0.5f;
        const float CY = SH * 0.5f - CH * 0.5f;
        FillRectGrad(CX, CY, CX + CW, CY + CH,
            D3DCOLOR_XRGB(20, 28, 70), D3DCOLOR_XRGB(12, 16, 46));
        HLine(CY, CX, CX + CW, COL_CYAN);
        HLine(CY + CH, CX, CX + CW, COL_CYAN);
        VLine(CX, CY, CY + CH, COL_BORDER);
        VLine(CX + CW, CY, CY + CH, COL_BORDER);
        DrawText(CX + 12.f, CY + 8.f, "START HDD BENCHMARK?", 1.25f, COL_WHITE);
        DrawText(CX + 12.f, CY + 26.f, "64MB buffered write + read + 1024 seek ops", 1.1f, COL_YELLOW);
        DrawText(CX + 12.f, CY + 40.f, "Screen freezes during test.  ~30-60s.", 1.1f, COL_GRAY);
        DrawText(CX + 12.f, CY + 56.f, "[A] Confirm    [B] Cancel", 1.1f, COL_CYAN);
    }

    // Active phase overlay — shows while screen is frozen during IO
    if (s_bench.state == BENCH_WRITE || s_bench.state == BENCH_READ)
    {
        const char* phaseStr = (s_bench.state == BENCH_WRITE)
            ? "WRITING 64MB...  please wait"
            : "READING 64MB...  please wait";
        float ty = SH * 0.5f - 8.f;
        float tw = TW(phaseStr, 1.3f);
        DrawText((SW - tw) * 0.5f, ty, phaseStr, 1.3f, COL_YELLOW);
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
        bool hddBusy = (s_view == VIEW_BENCH &&
            s_bench.state != BENCH_IDLE && s_bench.state != BENCH_DONE);
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
            BenchCleanup();
        s_view = VIEW_INFO;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_RTRIG) && s_view == VIEW_INFO)
        s_view = VIEW_BENCH;

    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        if (s_view == VIEW_INFO)
            ExportData();
        else if (s_view == VIEW_SMART)
            ExportSmart();
        else if (s_view == VIEW_BENCH)
        {
            if (s_bench.state == BENCH_IDLE)
                BenchStart();
            else if (s_bench.state == BENCH_CONFIRM)
            {
                // User confirmed — start timing now, immediately before write loop
                s_bench.writeT0 = GetTickCount();
                s_bench.state = BENCH_WRITE;
            }
            else if (s_bench.state == BENCH_DONE)
                ExportBench();
        }
    }

    // Cancel HDD benchmark with B
    if (s_view == VIEW_BENCH && s_bench.state != BENCH_IDLE && s_bench.state != BENCH_DONE)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            BenchCleanup();
            s_bench.state = BENCH_DONE;
            StrCopy(s_bench.statusMsg, sizeof(s_bench.statusMsg), "Benchmark cancelled");
            s_prevBtns = cur;
            RenderBench(logo);
            return;
        }
    }

    // Cancel DVD benchmark with B (only while actively reading)

    s_prevBtns = cur;

    // Run one tick of work
    if (s_view == VIEW_BENCH &&
        s_bench.state != BENCH_IDLE &&
        s_bench.state != BENCH_CONFIRM &&
        s_bench.state != BENCH_DONE)
        BenchTick();

    if (s_view == VIEW_SMART)
        RenderSmart(logo);
    else if (s_view == VIEW_BENCH)
        RenderBench(logo);
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

        BenchStart();
        // BenchStart() halts at BENCH_CONFIRM waiting for [A] from the user.
        // In AutoRun there is no user — bypass confirm by stamping writeT0
        // and advancing to BENCH_WRITE directly.
        if (s_bench.state == BENCH_CONFIRM)
        {
            s_bench.writeT0 = GetTickCount();
            s_bench.state = BENCH_WRITE;
        }
        // Drive to completion — BenchTick() runs each phase in a tight loop
        while (s_bench.state != BENCH_DONE && s_bench.state != BENCH_IDLE)
            BenchTick();

        // Format results
        auto FmtF = [](float v, char* out, int len) {
            int whole = Ftoi(v);
            int dec = Ftoi((v - (float)whole) * 10.f);
            char t[12]; IntToStr(whole, t, sizeof(t));
            StrCopy(out, len, t); StrCat2(out, len, out, ".");
            IntToStr(dec, t, sizeof(t)); StrCat2(out, len, out, t);
            };

        char val[24];
        if (!s_bench.readOnly)
        {
            FmtF(s_bench.writeMBs, val, sizeof(val));
            StrCat2(val, sizeof(val), val, " MB/s");
            HddWriteLine(hReport, "FS Write:     ", val);

            FmtF(s_bench.rawWrMBs, val, sizeof(val));
            StrCat2(val, sizeof(val), val, " MB/s");
            HddWriteLine(hReport, "Raw Write:    ", val);
        }

        FmtF(s_bench.readMBs, val, sizeof(val));
        StrCat2(val, sizeof(val), val, " MB/s");
        HddWriteLine(hReport, "Seq Read:     ", val);

        if (d.isSSD)
        {
            char iopsStr[12]; IntToStr(Ftoi(s_bench.rand4kIOPS), iopsStr, sizeof(iopsStr));
            StrCat2(iopsStr, sizeof(iopsStr), iopsStr, " IOPS");
            HddWriteLine(hReport, "4K Random:    ", iopsStr);
        }
        else
        {
            FmtF(s_bench.cacheMBs, val, sizeof(val));
            StrCat2(val, sizeof(val), val, " MB/s");
            HddWriteLine(hReport, "Cache Read:   ", val);
        }

        FmtF(s_bench.seekMs, val, sizeof(val));
        StrCat2(val, sizeof(val), val, " ms avg");
        HddWriteLine(hReport, "Seek Time:    ", val);

        // Cleanup temp file
        BenchCleanup();
    }
}